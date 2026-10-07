// 3dokit runtime -- tasks and threads, signals and the scheduler, as the 1993 kernel (os_code
// v0.16) runs them: CreateTask's thread (0x16a54), AllocSignal (0x19bb0), FreeSignal (0x19c68),
// WaitSignal (0x19a7c), SendSignal (0x19d40) and the kernel's own signal (0x19c70), Yield
// (0x16850), SetItemPri on a task (0x12fac, 0x1685c), and the switch at the end of an OS call
// (0x104fc).
//
// Each task runs on a host thread of its own, because the recompiled code nests its calls on the
// host's stack; exactly one runs at a time, and a switch hands the turn over and waits for it to
// come back. So the run is as deterministic as with one task: a switch happens only where the
// kernel would make one at an OS call. What is not modelled yet is time: the kernel's quantum
// timer, which would also make equal priorities take turns and let a higher priority that became
// ready without a reschedule (see pf_create_task) run at the next tick.
#include "pf.h"
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>
#include <vector>

enum : uint32_t {
    KERNELNODE = 1, TASKNODE = 5,
    // Task (task.h)
    T_THREADTASK = 0x24, T_REGS = 0x44, T_SP = 0x78, T_LK = 0x7c, T_PC = 0x80, T_PSR = 0x84,
    // n_Flags of a task
    TASK_READY = 1, TASK_WAITING = 2, TASK_SUPER = 8,
    SIGF_ABORT = 4,
    // CREATETASK_TAG_* (task.h), TAG_NOP (types.h)
    TAG_NAME = 1, TAG_PRI = 2, TAG_PC = 10, TAG_MAXQ = 11, TAG_STACKSIZE = 12, TAG_ARGC = 13,
    TAG_ARGP = 14, TAG_SP = 15, TAG_BASE = 16, TAG_NOP = 255,
    // the shell's spawnpri (System/Tasks/shell, 0x64f0): a program it starts runs at 100
    SPAWN_PRI = 100,
};

// The kernel's errors (the values os_code builds).
enum : uint32_t {
    KERR_BADITEM = 0xD57B9001u, KERR_BADTAG = 0xD57B9002u, KERR_BADTAGVAL = 0xD57B9003u,
    KERR_NOTPRIV = 0xD57B9004u, KERR_BADPRIORITY = 0xD57B910Cu, KERR_SMALLSTACK = 0xD57B910Fu,
    KERR_ILLEGALSIGNAL = 0xD57B9116u,
};

struct Task {
    uint32_t node;
    bool started, dead;
};

static std::vector<Task*> g_tasks;              // every task made; never freed
static std::vector<Task*> g_ready;              // kb_TaskReadyQ: by priority, highest first
static Task* g_running;
static bool g_reschedule;                       // kb_PleaseReschedule
// Made once and never destroyed: a host thread may still wait on them when the process ends.
static std::mutex* g_lock = new std::mutex;
static std::condition_variable* g_turn = new std::condition_variable;

static const uint32_t kThreadExit = 0xFFFFFFD0u;    // where a thread's function returns

static uint8_t pri(const Task* t) { return (uint8_t)pf_r8(t->node + 10); }
static const char* name(const Task* t) {
    static char s[64];
    uint32_t n = pf_r32(t->node + 16);
    if (n) pf_cstring(n, s, sizeof s);
    else std::snprintf(s, sizeof s, "(item %d)", (int)pf_r32(t->node + 24));
    return s;
}
static void set_flags(Task* t, uint32_t set, uint32_t clear) {
    pf_w8(t->node + 11, (pf_r8(t->node + 11) | set) & ~clear);
}

static Task* find(uint32_t node) {
    for (Task* t : g_tasks)
        if (t->node == node) return t;
    return nullptr;
}

// InsertNodeFromTail: after the last task of at least its priority.
static void make_ready(Task* t) {
    size_t i = g_ready.size();
    while (i > 0 && pri(g_ready[i - 1]) < pri(t)) --i;
    g_ready.insert(g_ready.begin() + (long)i, t);
    set_flags(t, TASK_READY, TASK_WAITING);
}

static void run_task(Task* t);

// Hand the turn to `next` and wait for it to come back (a dead task's thread just returns).
static void switch_to(Task* next) {
    Task* me = g_running;
    if (g_pf_trace) pf_log("        (task \"%s\" runs)\n", name(next));
    std::unique_lock<std::mutex> l(*g_lock);
    g_running = next;
    g_reschedule = false;
    pf_w32(pf_folio_base(PF_KERNEL) + KB_CURRENTTASK, next->node);
    if (!next->started) {
        next->started = true;
        std::thread(run_task, next).detach();
    }
    g_turn->notify_all();
    if (me->dead) return;
    g_turn->wait(l, [me] { return g_running == me; });
}

[[noreturn]] static void nothing_runs(const char* why) {
    std::fflush(stdout);
    std::fprintf(stderr, "%s: every task waits, and nothing can wake one yet (no timer, no interrupts)\n", why);
    std::exit(3);
}

static Task* take_ready() {
    Task* t = g_ready.front();
    g_ready.erase(g_ready.begin());
    return t;
}

// The current task waits (WaitSignal): the highest ready task runs until this one is woken.
static void block() {
    set_flags(g_running, TASK_WAITING, TASK_READY);
    if (g_ready.empty()) nothing_runs(name(g_running));
    switch_to(take_ready());
}

// A thread's host thread: its turn, then its function from the registers CreateTask left in its
// node; when the function returns (to the kernel's 0x168fc, which deletes the thread) or it exits,
// the thread is gone and the next ready task runs.
static void run_task(Task* t) {
    {
        std::unique_lock<std::mutex> l(*g_lock);
        g_turn->wait(l, [t] { return g_running == t; });
    }
    ArmCpu c{};
    for (int i = 0; i < 13; ++i) c.r[i] = pf_r32(t->node + T_REGS + 4u * i);
    c.r[13] = pf_r32(t->node + T_SP);
    c.r[14] = kThreadExit;
    c.budget = 1 << 20;
    try {
        arm_call(c, pf_r32(t->node + T_PC));
        if (c.pc != kThreadExit) arm_fault(c, c.pc, "a thread returned somewhere other than the kernel");
        if (g_pf_trace) pf_log("        (thread \"%s\" returns: gone)\n", name(t));
    } catch (const PfExit&) {
        if (g_pf_trace) pf_log("        (thread \"%s\" exits: gone)\n", name(t));
    }
    t->dead = true;
    set_flags(t, 0, TASK_READY | TASK_WAITING);
    if (g_ready.empty()) nothing_runs(name(t));
    switch_to(take_ready());
}

// ---- the switch at the end of an OS call (0x104fc) -----------------------------------------
// With kb_PleaseReschedule set and a task ready: the current task goes on if its priority is
// above the ready head's; otherwise it joins the ready queue behind its equals and the head runs.
void pf_task_reschedule() {
    if (!g_reschedule || g_ready.empty()) return;
    if (pri(g_running) > pri(g_ready.front())) return;
    Task* next = take_ready();
    make_ready(g_running);
    switch_to(next);
}

// ---- signals ----------------------------------------------------------------------------------
// The kernel's own signal (0x19c70): bits the task has not allocated are refused; otherwise they
// join t_SigBits, and a task waiting for any of them is made ready -- with a reschedule when its
// priority is above the current task's.
int32_t pf_signal(uint32_t node, uint32_t bits) {
    if (bits & ~pf_r32(node + T_ALLOCATEDSIGS)) return (int32_t)KERR_ILLEGALSIGNAL;
    uint32_t sig = pf_r32(node + T_SIGBITS) | bits;
    pf_w32(node + T_SIGBITS, sig);
    Task* t = find(node);
    if (t && t != g_running && (pf_r8(node + 11) & TASK_WAITING) && (sig & pf_r32(node + T_WAITBITS))) {
        make_ready(t);
        if (pri(g_running) < pri(t)) g_reschedule = true;
    }
    return 0;
}

// swi 0x10015: int32 AllocSignal(int32 sigs) -- 0 asks for one: the highest free bit from bit 30
// down, cleared in t_SigBits (0 when every bit is taken). Otherwise those bits, which may not be
// the system's (0-7) or bit 31, and 0 if any of them is already allocated.
static void k_allocsignal(ArmCpu& c) {
    uint32_t task = pf_current_task(), have = pf_r32(task + T_ALLOCATEDSIGS), want = c.r[0];
    if (want) {
        if (want & 0x800000FFu) c.r[0] = KERR_ILLEGALSIGNAL;
        else if (want & have) c.r[0] = 0;
        else pf_w32(task + T_ALLOCATEDSIGS, have | want);
        return;
    }
    if (have == 0x7FFFFFFFu) { c.r[0] = 0; return; }
    uint32_t bit = 0x40000000u;
    while (bit & have) bit >>= 1;
    pf_w32(task + T_ALLOCATEDSIGS, have | bit);
    pf_w32(task + T_SIGBITS, pf_r32(task + T_SIGBITS) & ~bit);
    c.r[0] = bit;
}

// swi 0x10016: Err FreeSignal(int32 sigs) -- allocated bits only, not the system's.
static void k_freesignal(ArmCpu& c) {
    uint32_t task = pf_current_task(), have = pf_r32(task + T_ALLOCATEDSIGS);
    if ((c.r[0] & 0x800000FFu) || (c.r[0] & ~have)) { c.r[0] = KERR_ILLEGALSIGNAL; return; }
    pf_w32(task + T_ALLOCATEDSIGS, have & ~c.r[0]);
    c.r[0] = 0;
}

// swi 0x10001: int32 WaitSignal(int32 sigs) -- allocated bits only; SIGF_ABORT is always waited
// for too. The bits that came, cleared from t_SigBits; when none has yet, the task waits.
static void k_waitsignal(ArmCpu& c) {
    uint32_t task = pf_current_task(), sigs = c.r[0];
    if (sigs & ~pf_r32(task + T_ALLOCATEDSIGS)) { c.r[0] = KERR_ILLEGALSIGNAL; return; }
    sigs |= SIGF_ABORT;
    if (!(pf_r32(task + T_SIGBITS) & sigs)) {
        pf_w32(task + T_WAITBITS, sigs);
        block();
    }
    uint32_t got = pf_r32(task + T_SIGBITS) & sigs;
    pf_w32(task + T_SIGBITS, pf_r32(task + T_SIGBITS) & ~got);
    c.r[0] = got;
}

// swi 0x10002: Err SendSignal(Item task, int32 sigs) -- 0 is the caller; the system's bits only
// from a privileged task, bit 31 never. The kernel's signal's own refusal is not passed on: 0.
static void k_sendsignal(ArmCpu& c) {
    uint32_t task = c.r[0] ? pf_check_item((int32_t)c.r[0], KERNELNODE, TASKNODE) : pf_current_task();
    if (!task) { c.r[0] = KERR_BADITEM; return; }
    if ((c.r[1] & 0xFF) && !(pf_r8(pf_current_task() + 11) & TASK_SUPER)) { c.r[0] = KERR_NOTPRIV; return; }
    if (c.r[1] & 0x80000000u) { c.r[0] = KERR_ILLEGALSIGNAL; return; }
    pf_signal(task, c.r[1]);
    c.r[0] = 0;
}

// swi 0x10009: void Yield(void) -- a reschedule: the ready head runs if it is not below.
static void k_yield(ArmCpu& c) {
    (void)c;
    g_reschedule = true;
}

// swi 0x1000a: int32 SetItemPri(Item, uint8 pri) -- the item's owner, the item itself or a
// privileged task (0x12f7c), else NOTPRIV; for a task (0x1685c) a priority of 1 to 199. Lowering
// the current task's, or raising a ready task's above the current task's old one, reschedules.
// The current task's old priority back.
static void k_setitempri(ArmCpu& c) {
    uint32_t n = pf_item_node((int32_t)c.r[0]), np = c.r[1] & 0xFF, me = pf_current_task();
    if (!n) { c.r[0] = KERR_BADITEM; return; }
    uint32_t caller = pf_r32(me + 24);
    if (!(pf_r8(me + 11) & TASK_SUPER) && pf_r32(n + 28) != caller && pf_r32(n + 24) != caller) {
        c.r[0] = KERR_NOTPRIV;
        return;
    }
    if (pf_r8(n + 8) != KERNELNODE || pf_r8(n + 9) != TASKNODE) pf_stop(c, "SetItemPri of an item other than a task: not yet");
    if (np == 0 || np >= 200) { c.r[0] = KERR_BADPRIORITY; return; }
    uint32_t old = pf_r8(me + 10);
    Task* t = find(n);
    if (n == me) {
        pf_w8(n + 10, np);
        if (np < old) g_reschedule = true;
    } else {
        bool queued = false;
        for (size_t i = 0; i < g_ready.size(); ++i)
            if (g_ready[i] == t) { g_ready.erase(g_ready.begin() + (long)i); queued = true; break; }
        pf_w8(n + 10, np);
        if (queued) make_ready(t);
    }
    if ((pf_r8(n + 11) & TASK_READY) && np > old) g_reschedule = true;
    c.r[0] = old;
}

// ---- making a thread (CreateSizedItem of a TASKNODE, 0x16a54) ----------------------------------
// Tags: TAG_ITEM_NAME (required), TAG_ITEM_PRI (the creator's when not given; 10 to 199 from a
// task that is not privileged), CREATETASK_TAG_PC, _STACKSIZE (at least 0x80 for a thread), _SP
// (which makes it a thread: its stack is the caller's memory), _ARGC and _ARGP (r0 and r1, and
// r5 and r6), _BASE (r9 and r7), _MAXQ (the quantum: kept, unused). A thread shares its creator's
// memory lists, has the eight system signals, sl at its stack's base + 0x80, and returns to the
// kernel's 0x168fc. The kernel also gives each of the shared MemLists a semaphore: not here, where
// item numbers are not the console's anyway.
//
// Made ready, the 1993 kernel asks for a reschedule when the creator's priority is *above* the
// new task's (0x17584) -- which changes nothing -- and not when below, so a higher-priority thread
// waits for the next quantum tick. Without a timer here, the reschedule is asked for when the new
// task's priority is above the creator's: it runs as the OS call returns, as it would a tick
// later on the console.
uint32_t pf_create_task(ArmCpu& c, uint32_t tags) {
    uint32_t me = pf_current_task();
    uint32_t nm = 0, p = pf_r8(me + 10), pc = 0, size = 0, argc = 0, argp = 0, sp = 0, base = 0, maxq = 0;
    for (uint32_t a = tags; a; a += 8) {
        uint32_t tag = pf_r32(a), v = pf_r32(a + 4);
        if (!tag) break;
        switch (tag) {
        case TAG_NAME: nm = v; break;
        case TAG_PRI: p = v & 0xFF; break;
        case TAG_PC: pc = v & ~0xFC000003u; break;
        case TAG_MAXQ: maxq = v; break;
        case TAG_STACKSIZE: size = v; break;
        case TAG_ARGC: argc = v; break;
        case TAG_ARGP: argp = v; break;
        case TAG_SP: sp = v; break;
        case TAG_BASE: base = v; break;
        case TAG_NOP: break;
        default: pf_stop(c, "CreateTask: a tag of a task with its own image: not yet");
        }
    }
    (void)maxq;
    if (!sp) pf_stop(c, "CreateTask: a task rather than a thread: not yet");
    if (!nm) return KERR_BADTAGVAL;
    if (!(pf_r8(me + 11) & TASK_SUPER) && (p < 10 || p > 199)) return KERR_BADPRIORITY;
    if (size < 0x80) return KERR_SMALLSTACK;
    char s[64];
    pf_cstring(nm, s, sizeof s);
    uint32_t n = pf_os_alloc(PF_TASK_SIZE);
    pf_w32(n + 12, PF_TASK_SIZE);
    int32_t item = pf_item_new(n, KERNELNODE, TASKNODE, s);
    pf_w8(n + 10, p);
    pf_w32(n + 28, pf_r32(me + 24));                        // n_Owner: the creator
    pf_w32(n + T_THREADTASK, me);
    pf_w32(n + T_FREEMEMORYLISTS, pf_r32(me + T_FREEMEMORYLISTS));
    uint32_t stack_base = sp - (size & ~3u);
    pf_w32(n + T_STACKBASE, stack_base);
    pf_w32(n + T_STACKSIZE, (size + 3) & ~3u);
    pf_w32(n + T_ALLOCATEDSIGS, 0xFF);
    const uint32_t regs[13] = {argc, argp, 0, 0, 0, argc, argp, base, 0, base, stack_base + 0x80, 0, 0};
    for (int i = 0; i < 13; ++i) pf_w32(n + T_REGS + 4u * i, regs[i]);
    pf_w32(n + T_SP, sp);
    pf_w32(n + T_LK, 0x168FC);                              // the kernel's: the thread deleted
    pf_w32(n + T_PC, pc);
    pf_w32(n + T_PSR, 0x10);                                // user mode
    Task* t = new Task{n, false, false};
    g_tasks.push_back(t);
    make_ready(t);
    if (p > pf_r8(me + 10)) g_reschedule = true;
    if (g_pf_trace) pf_log("        thread \"%s\" at %06X, priority %u, stack %08X-%08X\n", s, pc, p, stack_base, sp);
    return (uint32_t)item;
}

// The program's own task: the one pf_mem_init made, running on the host's main thread at the
// shell's spawn priority.
void pf_task_init() {
    g_tasks.clear();
    g_ready.clear();
    g_reschedule = false;
    uint32_t main = pf_current_task();
    pf_w8(main + 10, SPAWN_PRI);
    pf_w8(main + 11, pf_r8(main + 11) | TASK_READY);
    g_running = new Task{main, true, false};
    g_tasks.push_back(g_running);
    pf_on_swi(0x10001, k_waitsignal);
    pf_on_swi(0x10002, k_sendsignal);
    pf_on_swi(0x10009, k_yield);
    pf_on_swi(0x1000a, k_setitempri);
    pf_on_swi(0x10015, k_allocsignal);
    pf_on_swi(0x10016, k_freesignal);
}
