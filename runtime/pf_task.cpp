// 3dokit runtime -- tasks and threads, signals and the scheduler, as the 1993 kernel (os_code
// v0.16) runs them: CreateTask's thread (0x16a54), AllocSignal (0x19bb0), FreeSignal (0x19c68),
// WaitSignal (0x19a7c), SendSignal (0x19d40) and the kernel's own signal (0x19c70), Yield
// (0x16850), SetItemPri on a task (0x12fac, 0x1685c), and the switch at the end of an OS call
// (0x104fc).
//
// Each task runs on a host thread of its own, because the recompiled code nests its calls on the
// host's stack; exactly one runs at a time, and a switch hands the turn over and waits for it to
// come back. So the run is as deterministic as with one task: a switch happens only where the
// kernel would make one -- at the end of an OS call, or where an interrupt (an event of
// pf_time.cpp, at a safe point) has made a higher-priority task ready. When every task waits, the
// guest's clock jumps to the next event. The kernel's quantum timer makes equal priorities take
// turns (below, "the quantum"); a higher priority that became ready without a reschedule (see
// pf_create_task) runs at once here rather than at the next tick.
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
    T_FLAGS = 0xd0, TASK_ALLOCATED_SP = 2,
    // n_Flags of a task
    TASK_READY = 1, TASK_WAITING = 2, TASK_SUPER = 8,
    SIGF_ABORT = 4, SIGF_DEADTASK = 0x10,
    // CREATETASK_TAG_* (task.h), TAG_NOP (types.h)
    TAG_NAME = 1, TAG_PRI = 2, TAG_PC = 10, TAG_MAXQ = 11, TAG_STACKSIZE = 12, TAG_ARGC = 13,
    TAG_ARGP = 14, TAG_SP = 15, TAG_BASE = 16, TAG_IMAGESZ = 18, TAG_AIF = 19, TAG_CMDSTR = 20,
    TAG_ALLOCDTHREADSP = 24, TAG_NOP = 255,
    // the File folio's own (its task callback's): the task's current and program directories
    FILETASK_TAG_CURRENTDIRECTORY = 0x3000a, FILETASK_TAG_PROGRAMDIRECTORY = 0x3000b,
    // the shell's spawnpri (System/Tasks/shell, 0x64f0): a program it starts runs at 100
    SPAWN_PRI = 100,
};

// The kernel's errors (the values os_code builds).
enum : uint32_t {
    KERR_BADITEM = 0xD57B9001u, KERR_BADTAG = 0xD57B9002u, KERR_BADTAGVAL = 0xD57B9003u,
    KERR_NOTPRIV = 0xD57B9004u, KERR_BADPRIORITY = 0xD57B910Cu, KERR_SMALLSTACK = 0xD57B910Fu,
    KERR_ILLEGALSIGNAL = 0xD57B9116u, KERR_NOTAIF = 0xD57B9118u, KERR_THREADTASK = 0xD57B910Du,
    KERR_BADSIGNATURE = 0xD57B9112u,            // 23.10's, for a signature that is not the image's
};

struct Task {
    uint32_t node;
    bool started, dead;
    uint32_t image;                             // a task with its own image: its AIF header
    uint32_t quantum = 15000;                   // its quantum, us (t_MaxUSecs, +0xc0 in 20.21's Task)
};

static std::vector<Task*> g_tasks;              // every task alive; never freed
static std::vector<Task*> g_ready;              // kb_TaskReadyQ: by priority, highest first
static Task* g_running;
static bool g_reschedule;                       // kb_PleaseReschedule
// Made once and never destroyed: a host thread may still wait on them when the process ends.
static std::mutex* g_lock = new std::mutex;
static std::condition_variable* g_turn = new std::condition_variable;

static const uint32_t kThreadExit = 0xFFFFFFD0u;    // where a thread's function returns
static const uint32_t kTaskExit = 0x410u;           // 23.10's lr for a task: its kernel's exit

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
static void arm_quantum();

// Hand the turn to `next` and wait for it to come back (a dead task's thread just returns). The
// task that waited may be the one woken: then it simply goes on.
static void switch_to(Task* next) {
    Task* me = g_running;
    if (next == me) {
        g_reschedule = false;
        return;
    }
    if (g_pf_trace) pf_log("        (task \"%s\" runs)\n", name(next));
    std::unique_lock<std::mutex> l(*g_lock);
    g_running = next;
    g_reschedule = false;
    pf_w32(pf_folio_base(PF_KERNEL) + KB_CURRENTTASK, next->node);
    arm_quantum();
    if (!next->started) {
        next->started = true;
        std::thread(run_task, next).detach();
    }
    g_turn->notify_all();
    if (me->dead) return;
    g_turn->wait(l, [me] { return g_running == me; });
}

static bool someone_ready() { return !g_ready.empty(); }

// The highest ready task, taken off the queue; when there is none, the clock moves on to the
// events that make one ready -- or, if none does within pf_time_idle's limit, the run ends.
static Task* take_ready(const char* who) {
    if (g_ready.empty()) {
        uint64_t from = pf_now();
        if (!pf_time_idle(someone_ready)) {
            std::fflush(stdout);
            std::fprintf(stderr, "%s: every task waits, and no event wakes one (%.6f s of guest time)\n", who,
                         (double)pf_now() / 1e9);
            std::exit(3);
        }
        if (g_pf_trace)
            pf_log("        (every task waits: %.6f s to %.6f s)\n", (double)from / 1e9, (double)pf_now() / 1e9);
    }
    Task* t = g_ready.front();
    g_ready.erase(g_ready.begin());
    return t;
}

// The current task waits (WaitSignal): the highest ready task runs until this one is woken.
static void block() {
    set_flags(g_running, TASK_WAITING, TASK_READY);
    switch_to(take_ready(name(g_running)));
}

// A thread's host thread: its turn, then its function from the registers CreateTask left in its
// node; when the function returns (to the kernel's 0x168fc, which deletes the thread) or it exits,
// the thread is gone and the next ready task runs. 23.10's return (0x631c, through 0x410) first
// gives a thread made with _ALLOCDTHREADSP its stack back: FreeMemToMemLists of t_StackBase and
// t_StackSize to the task's lists, each of their semaphores locked before -- the runtime has none.
static void give_back_stack(uint32_t node) {
    if (pf_r32(node + T_THREADTASK) && (pf_r32(node + T_FLAGS) & TASK_ALLOCATED_SP))
        pf_free_mem(pf_r32(node + T_FREEMEMORYLISTS), pf_r32(node + T_STACKBASE), (int32_t)pf_r32(node + T_STACKSIZE));
}

// Kernel -148: what the C library's exit() calls in 23.10 (its startup's 0x118) -- the same 0x631c
// a thread returns to: a thread's stack given back as above, then the task deletes itself; the
// status is not read. Here the task ends as a program's exit (SWI 0x11) ends it.
static void k_exit(ArmCpu& c) {
    give_back_stack(pf_current_task());
    throw PfExit{(int)c.r[0]};
}

// A task with its own image starts at the image's AIF header, where the kernel leaves its pc: the
// header's four words call, in turn, its decompression, its self-relocation (which makes its own
// word 0x04 a no-op), its zero-init and its entry, and the word at 0x10, `swi 0x11`, is the exit.
// The header is read word by word as it stands in memory (code that changes itself is not
// recompiled); what each BL calls is the program's own recompiled code.
static void run_image(ArmCpu& c, uint32_t image) {
    for (uint32_t off = 0; off < 0x10; off += 4) {
        uint32_t w = pf_r32(image + off);
        if (w == 0xE1A00000u) continue;                 // mov r0, r0
        if (w >> 24 != 0xEB) pf_stop(c, "a task's AIF header word that is no BL or no-op: not yet");
        uint32_t disp = w & 0xFFFFFF;
        uint32_t to = image + off + 8 + 4 * (disp & 0x800000 ? disp - 0x1000000 : disp);
        c.r[14] = image + off + 4;
        arm_call(c, to);
        if (c.pc != image + off + 4) arm_bad_return(c, image + off + 4);
    }
    if (pf_r32(image + 0x10) != 0xEF000011u) pf_stop(c, "a task's AIF header without its exit: not yet");
    arm_swi(c, 0x11, image + 0x10);
}

// A task with its own image that has ended (its exit, or its code's return to the kernel's 0x410,
// which exits): the kernel deletes it, as DeleteItem of a task does (pf_delete_task) -- the items
// it owns deleted as by it, the last made first (its threads among them), its semaphores
// unlocked, its owner sent SIGF_DEADTASK -- and with it its memory: every page its own MemLists
// own goes back to the system. Its recompiled module is unloaded from its image, and the task's
// item is gone.
static void end_image_task(ArmCpu& c, Task* t) {
    uint32_t node = t->node;
    int32_t me = (int32_t)pf_r32(node + 24);
    for (int32_t i = pf_item_count() - 1; i > 0; --i) {
        uint32_t n = pf_item_node(i);
        if (n && n != node && (int32_t)pf_r32(n + 28) == me) pf_delete_item_as_owner(c, i);
    }
    pf_task_release(me);
    if (uint32_t owner = pf_item_node((int32_t)pf_r32(node + 28))) pf_signal(owner, SIGF_DEADTASK);
    for (size_t i = 0; i < g_tasks.size(); ++i)
        if (g_tasks[i] == t) { g_tasks.erase(g_tasks.begin() + (long)i); break; }
    pf_mem_task_gone(node);
    pf_unload_image(t->image);
    pf_item_free(me);
    pf_scavenge(false);
}

static void run_task(Task* t) {
    {
        std::unique_lock<std::mutex> l(*g_lock);
        g_turn->wait(l, [t] { return g_running == t; });
    }
    ArmCpu c{};
    for (int i = 0; i < 13; ++i) c.r[i] = pf_r32(t->node + T_REGS + 4u * i);
    c.r[13] = pf_r32(t->node + T_SP);
    c.r[14] = kThreadExit;
    c.budget = PF_POLL_EVERY;
    try {
        if (t->image) {
            run_image(c, t->image);
        } else {
            arm_call(c, pf_r32(t->node + T_PC));
            if (c.pc != kThreadExit) arm_fault(c, c.pc, "a thread returned somewhere other than the kernel");
            if (g_pf_trace) pf_log("        (thread \"%s\" returns: gone)\n", name(t));
            give_back_stack(t->node);
        }
    } catch (const PfExit& e) {
        if (t->image) {
            if (g_pf_trace) pf_log("        (task \"%s\" exits with %d: gone)\n", name(t), e.code);
            char who[64];
            std::snprintf(who, sizeof who, "%s", name(t));
            t->dead = true;
            set_flags(t, 0, TASK_READY | TASK_WAITING);
            end_image_task(c, t);                       // its node freed with its item
            switch_to(take_ready(who));
            return;
        } else if (g_pf_trace) {
            pf_log("        (thread \"%s\" exits: gone)\n", name(t));
        }
    }
    t->dead = true;
    set_flags(t, 0, TASK_READY | TASK_WAITING);
    switch_to(take_ready(name(t)));
}

// ---- the quantum --------------------------------------------------------------------------
// The kernel's "kernel quanta" FIRQ (20.21's 0x16ec8, made at 0x184fc at priority 0xfa; 1993's
// 0x1780c and 23.10's 0x7320 the same), on a timer of CLIO's that counts 16 us steps: each task
// switched in loads it with its quantum (0x16db8: t_MaxUSecs >> 4, less 1 -- 15,000 us unless
// CREATETASK_TAG_MAXQ says otherwise, 0x161ec), so it ends (us >> 4) steps later. Then, when a task
// of at least the running one's priority is ready, kb_PleaseReschedule is set -- the switch below,
// at the next safe point -- else the quantum is added to the task's time and the timer reloaded.
static uint64_t g_quantum_due;                  // when the running task's quantum ends

static void quantum_end(uint64_t when) {
    if (when != g_quantum_due || !g_running) return;
    g_quantum_due = 0;                          // this end taken (another event at it is stale)
    if (!g_ready.empty() && pri(g_running) <= pri(g_ready.front())) {
        g_reschedule = true;
        return;
    }
    g_quantum_due = when + (uint64_t)(g_running->quantum >> 4) * 16000;
    pf_at(g_quantum_due, quantum_end);
}

static void arm_quantum() {
    g_quantum_due = pf_now() + (uint64_t)(g_running->quantum >> 4) * 16000;
    pf_at(g_quantum_due, quantum_end);
}

// ---- the switch at the end of an OS call (0x104fc) -----------------------------------------
// With kb_PleaseReschedule set and a task ready: the current task goes on if its priority is
// above the ready head's; otherwise it joins the ready queue behind its equals and the head runs.
void pf_task_reschedule() {
    if (!g_reschedule || g_ready.empty()) return;
    if (pri(g_running) > pri(g_ready.front())) return;
    Task* next = take_ready(name(g_running));
    make_ready(g_running);
    switch_to(next);
}

// ---- signals ----------------------------------------------------------------------------------
// The kernel's own signal (0x19c70): bits the task has not allocated are refused; otherwise they
// join t_SigBits, and a task waiting for any of them is made ready -- with a reschedule when its
// priority is above the current task's. (The current task itself is waiting only while the clock
// moves on for it, in take_ready.)
int32_t pf_signal(uint32_t node, uint32_t bits) {
    if (bits & ~pf_r32(node + T_ALLOCATEDSIGS)) return (int32_t)KERR_ILLEGALSIGNAL;
    uint32_t sig = pf_r32(node + T_SIGBITS) | bits;
    pf_w32(node + T_SIGBITS, sig);
    Task* t = find(node);
    if (t && (pf_r8(node + 11) & TASK_WAITING) && (sig & pf_r32(node + T_WAITBITS))) {
        make_ready(t);
        if (pri(g_running) < pri(t)) g_reschedule = true;
    }
    return 0;
}

// swi 0x10015: int32 AllocSignal(int32 sigs) -- 0 asks for one: the highest free bit from bit 30
// down, cleared in t_SigBits (0 when every bit is taken). Otherwise those bits, which may not be
// the system's (0-7) or bit 31, and 0 if any of them is already allocated.
uint32_t pf_alloc_signal(uint32_t want) {
    uint32_t task = pf_current_task(), have = pf_r32(task + T_ALLOCATEDSIGS);
    if (want) {
        if (want & 0x800000FFu) return KERR_ILLEGALSIGNAL;
        if (want & have) return 0;
        pf_w32(task + T_ALLOCATEDSIGS, have | want);
        return want;
    }
    if (have == 0x7FFFFFFFu) return 0;
    uint32_t bit = 0x40000000u;
    while (bit & have) bit >>= 1;
    pf_w32(task + T_ALLOCATEDSIGS, have | bit);
    pf_w32(task + T_SIGBITS, pf_r32(task + T_SIGBITS) & ~bit);
    return bit;
}
static void k_allocsignal(ArmCpu& c) { c.r[0] = pf_alloc_signal(c.r[0]); }

// swi 0x10016: Err FreeSignal(int32 sigs) -- allocated bits only, not the system's. The kernel's
// own, of any task's bits (20.21's 0x1910c; the SWI is it on the current task, 0x19140).
int32_t pf_free_signal(uint32_t sigs, uint32_t task) {
    if (!task) task = pf_current_task();
    uint32_t have = pf_r32(task + T_ALLOCATEDSIGS);
    if ((sigs & 0x800000FFu) || (sigs & ~have)) return (int32_t)KERR_ILLEGALSIGNAL;
    pf_w32(task + T_ALLOCATEDSIGS, have & ~sigs);
    return 0;
}
static void k_freesignal(ArmCpu& c) { c.r[0] = (uint32_t)pf_free_signal(c.r[0]); }

// swi 0x10001: int32 WaitSignal(int32 sigs) -- allocated bits only; SIGF_ABORT is always waited
// for too. The bits that came, cleared from t_SigBits; when none has yet, the task waits.
int32_t pf_wait_signal(uint32_t sigs) {
    uint32_t task = pf_current_task();
    if (sigs & ~pf_r32(task + T_ALLOCATEDSIGS)) return (int32_t)KERR_ILLEGALSIGNAL;
    sigs |= SIGF_ABORT;
    if (!(pf_r32(task + T_SIGBITS) & sigs)) {
        pf_w32(task + T_WAITBITS, sigs);
        block();
    }
    uint32_t got = pf_r32(task + T_SIGBITS) & sigs;
    pf_w32(task + T_SIGBITS, pf_r32(task + T_SIGBITS) & ~got);
    return (int32_t)got;
}
static void k_waitsignal(ArmCpu& c) { c.r[0] = (uint32_t)pf_wait_signal(c.r[0]); }

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
// r5 and r6), _BASE (r9 and r7), _MAXQ (the quantum: kept, unused), and _ALLOCDTHREADSP (24),
// whose value is not read: the stack is the thread's, to be freed when it returns (bit 1 of
// t_Flags, 0xd0). That tag is Portfolio 23.10's (its kernel's tag callback, 0x6400 there, the last
// of its switch); the 1993 kernel's switch ends at 23 and refuses it (BADTAG), and a program of
// that time's library never passes it -- 23.10's CreateThread does. A thread shares its creator's
// memory lists, has the eight system signals, sl at its stack's base + 0x80, and returns to the
// kernel's 0x168fc. The kernel also gives each of the shared MemLists a semaphore: not here, where
// item numbers are not the console's anyway.
//
// Made ready, the 1993 kernel asks for a reschedule when the creator's priority is *above* the
// new task's (0x17584) -- which changes nothing -- and not when below, so a higher-priority thread
// waits for the next quantum tick. Without a timer here, the reschedule is asked for when the new
// task's priority is above the creator's: it runs as the OS call returns, as it would a tick
// later on the console.
static uint32_t create_image_task(ArmCpu& c, uint32_t nm, uint32_t p, uint32_t size, uint32_t argc, uint32_t argp,
                                  uint32_t base, uint32_t image, uint32_t imagesz, uint32_t cmd);

uint32_t pf_create_task(ArmCpu& c, uint32_t tags) {
    uint32_t me = pf_current_task();
    uint32_t nm = 0, p = pf_r8(me + 10), pc = 0, size = 0, argc = 0, argp = 0, sp = 0, base = 0, maxq = 0;
    uint32_t flags = 0, image = 0, imagesz = 0, cmd = 0;
    bool size_given = false;
    for (uint32_t a = tags; a; a += 8) {
        uint32_t tag = pf_r32(a), v = pf_r32(a + 4);
        if (!tag) break;
        switch (tag) {
        case TAG_NAME: nm = v; break;
        case TAG_PRI: p = v & 0xFF; break;
        case TAG_PC: pc = v & ~0xFC000003u; break;
        case TAG_MAXQ: maxq = v; break;
        case TAG_STACKSIZE: size = v; size_given = true; break;
        case TAG_ARGC: argc = v; break;
        case TAG_ARGP: argp = v; break;
        case TAG_SP: sp = v; break;
        case TAG_BASE: base = v; break;
        case TAG_IMAGESZ: imagesz = v; break;
        case TAG_AIF: image = v; break;
        case TAG_CMDSTR: cmd = v; break;
        case TAG_ALLOCDTHREADSP: flags |= TASK_ALLOCATED_SP; break;
        case TAG_NOP: break;
        // the File folio's, which its callback reads: the program's directory, the root's here
        case FILETASK_TAG_CURRENTDIRECTORY: case FILETASK_TAG_PROGRAMDIRECTORY: break;
        default: pf_stop(c, "CreateTask: a tag the runtime does not take yet");
        }
    }
    if (maxq && (maxq < 5000 || maxq > 1000000)) pf_stop(c, "CreateTask: a quantum out of 5,000 to 1,000,000 us: not yet");
    if (!sp && image) {
        if (maxq) pf_stop(c, "CreateTask: a quantum for a task with its own image: not yet");
        if (!size_given) size = 0x100;
        return create_image_task(c, nm, p, size, argc, argp, base, image, imagesz, cmd);
    }
    if (!sp) pf_stop(c, "CreateTask: a task with neither a stack nor an image: not yet");
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
    pf_w32(n + T_FLAGS, pf_r32(n + T_FLAGS) | flags);
    const uint32_t regs[13] = {argc, argp, 0, 0, 0, argc, argp, base, 0, base, stack_base + 0x80, 0, 0};
    for (int i = 0; i < 13; ++i) pf_w32(n + T_REGS + 4u * i, regs[i]);
    pf_w32(n + T_SP, sp);
    pf_w32(n + T_LK, 0x168FC);                              // the kernel's: the thread deleted
    pf_w32(n + T_PC, pc);
    pf_w32(n + T_PSR, 0x10);                                // user mode
    Task* t = new Task{n, false, false};
    if (maxq) t->quantum = maxq;
    g_tasks.push_back(t);
    make_ready(t);
    if (p > pf_r8(me + 10)) g_reschedule = true;
    if (g_pf_trace) pf_log("        thread \"%s\" at %06X, priority %u, stack %08X-%08X\n", s, pc, p, stack_base, sp);
    return (uint32_t)item;
}

// ---- making a task with its own image (23.10's CreateTask, its kernel's 0x64e0) -----------------
// What the File folio's loader asks for (LoadProgram): CREATETASK_TAG_AIF, the image it read;
// _IMAGESZ, the file's bytes; _CMDSTR, the command line. In 23.10's order: a thread may not make a
// task (0x65c8); the stack, 0x100 unless given, is at least the command line's length (rounded to
// words with its 0, 0x6640: when shorter, that and 0x100); a name; a priority of 10 to 199 from a
// task that is not privileged; a stack of at least 0x100. The image's header (0x6a84): ro of at
// least 0x80, rw and bss not negative, and it needs ro + rw + bss (rounded up to 16), or the file's
// bytes when more. When its AIF header says it has the 3DO header (+0x2c, bit 30), that header's
// stack (+0x28; at least 0x100, or the command line's as above), version and revision (+0x14) are
// the task's, and its signature and privilege are looked at (pf_image_header); an image with flag
// 0x20 (+0x24) stops the run. The task's memory is pf_mem_image_task's. Its registers (0x7008): r0 and r5
// _ARGC, r1 and r6 _ARGP, r7 and r9 _BASE (KernelBase unless given), sl its stack's base + 0x80,
// lr the kernel's 0x410, pc the image; its program's startup (Kernel -120) reads the command line
// left at its stack's top. Made ready as a thread is (see pf_create_task).
// The 3DO header's signature, privilege and priority, as 23.10's CreateTask takes them (0x6cc0): a
// signature (its length at +0x34) must end the file -- the image's signed bytes (+0x30) and it, the
// file's bytes, else 0xD57B9112 --, and then the kernel checks it against the 3DO Company's key
// (0xacd0), which the runtime does not: an image on the disc is taken as the console takes the
// disc's own, signed. An image flagged privileged (+0x24, bit 1) must be signed (else 0xD57B9112)
// and makes a privileged task (TASK_SUPER, which the creator's privilege does not give). The
// header's priority (+0x0a), when not 0, is the task's; a task neither privileged nor signed
// may have only 10 to 199 (else BADPRIORITY).
int32_t pf_image_header(uint32_t image, uint32_t imagesz, uint32_t& p, bool& privileged) {
    uint32_t hdr = image + 0x80;
    bool signed_ok = false;
    privileged = false;
    if (uint32_t siglen = pf_r32(hdr + 0x34)) {
        if (siglen + pf_r32(hdr + 0x30) != imagesz) return (int32_t)KERR_BADSIGNATURE;
        signed_ok = true;
    }
    if (pf_r8(hdr + 0x24) & 2) {
        if (!signed_ok) return (int32_t)KERR_BADSIGNATURE;
        privileged = true;
    }
    if (uint32_t hp = pf_r8(hdr + 0x0a)) {
        p = hp;
        if (!privileged && !signed_ok && (p < 10 || p > 199)) return (int32_t)KERR_BADPRIORITY;
    }
    return 0;
}

static uint32_t create_image_task(ArmCpu& c, uint32_t nm, uint32_t p, uint32_t size, uint32_t argc, uint32_t argp,
                                  uint32_t base, uint32_t image, uint32_t imagesz, uint32_t cmd) {
    uint32_t me = pf_current_task();
    bool super = pf_r8(me + 11) & TASK_SUPER;
    if (pf_r32(me + T_THREADTASK)) return KERR_THREADTASK;
    uint32_t cmdlen = 0;
    if (cmd) {
        while (pf_r8(cmd + cmdlen)) ++cmdlen;
        cmdlen = (cmdlen + 4) & ~3u;
    }
    if (size < cmdlen) size = cmdlen + 0x100;
    if (!nm) return KERR_BADTAGVAL;
    if (!super && (p < 10 || p > 199)) return KERR_BADPRIORITY;
    if (size < 0x100) return KERR_SMALLSTACK;
    int32_t ro = (int32_t)pf_r32(image + 0x14), rw = (int32_t)pf_r32(image + 0x18), bss = (int32_t)pf_r32(image + 0x20);
    uint32_t need = (uint32_t)(ro + rw) + (((uint32_t)bss + 15) & ~15u);
    if (ro < 0x80 || rw < 0 || bss < 0 || !need) return KERR_NOTAIF;
    if (need < imagesz) need = imagesz;
    uint8_t version = 0, revision = 0;
    bool privileged = false;
    if (pf_r32(image + 0x2c) & 0x40000000u) {
        uint32_t hdr = image + 0x80;
        if (uint32_t hs = pf_r32(hdr + 0x28)) {
            size = hs < cmdlen ? cmdlen + 0x100 : hs;
            if (size < 0x100) return KERR_SMALLSTACK;
        }
        version = (uint8_t)pf_r8(hdr + 0x14);
        revision = (uint8_t)pf_r8(hdr + 0x15);
        if (pf_r8(hdr + 0x24) & 0x20) pf_stop(c, "CreateTask: an image whose 3DO header has flag 0x20: not yet");
        int32_t err = pf_image_header(image, imagesz, p, privileged);
        if (err) return (uint32_t)err;
    }
    size = (size + 3) & ~3u;
    char s[64];
    pf_cstring(nm, s, sizeof s);
    uint32_t n = pf_os_alloc(PF_TASK_SIZE);
    pf_w32(n + 12, PF_TASK_SIZE);
    int32_t item = pf_item_new(n, KERNELNODE, TASKNODE, s);
    pf_w8(n + 10, p);
    if (privileged) pf_w8(n + 11, pf_r8(n + 11) | TASK_SUPER);
    pf_w8(n + 20, version);
    pf_w8(n + 21, revision);
    pf_w32(n + 28, pf_r32(me + 24));                        // n_Owner: the creator
    pf_w32(n + T_ALLOCATEDSIGS, 0xFF);
    uint32_t sp = pf_mem_image_task(me, n, image, need, size, cmd, cmdlen);
    if (!sp) pf_stop(c, "CreateTask: no pages for a task's stack: not yet");
    if (!base) base = pf_folio_base(PF_KERNEL);
    uint32_t sbase = pf_r32(n + T_STACKBASE);
    const uint32_t regs[13] = {argc, argp, 0, 0, 0, argc, argp, base, 0, base, sbase + 0x80, 0, 0};
    for (int i = 0; i < 13; ++i) pf_w32(n + T_REGS + 4u * i, regs[i]);
    pf_w32(n + T_SP, sp);
    pf_w32(n + T_LK, kTaskExit);
    pf_w32(n + T_PC, image);
    pf_w32(n + T_PSR, 0x10);                                // user mode
    Task* t = new Task{n, false, false, image};
    g_tasks.push_back(t);
    make_ready(t);
    if (p > pf_r8(me + 10)) g_reschedule = true;
    if (g_pf_trace)
        pf_log("        task \"%s\" with its image at %08X (%u bytes), priority %u, stack %08X-%08X\n", s, image, need, p,
               sbase, sp);
    return (uint32_t)item;
}

// ---- deleting a task (DeleteItem of a TASKNODE, 0x167cc) ----------------------------------------
// What the task holds goes first (0x16760): its resource table from the last entry back, each item
// it made deleted as by the task, each it opened closed; then every semaphore it holds is unlocked
// (0x165fc), its owner gets SIGF_DEADTASK, and it leaves the queue it is ready or waiting on; then
// its per-folio data, supervisor stack, resource table and name are freed (0x16658), and the OS's
// lists scavenged (0x157c4, ScavengeMem in supervisor mode). The runtime keeps no resource table:
// the items whose n_Owner is the task, the last made first, then those it has open -- the table's
// order for a task that opens before it makes, as the threads run so far do. It makes no per-folio
// data or supervisor stack. The task's host thread is left waiting for a turn that never comes. A
// task deleting itself stops the run: not yet.
int32_t pf_delete_task(ArmCpu& c, uint32_t node) {
    Task* t = find(node);
    if (!t) pf_stop(c, "DeleteItem of a task the runtime does not run");
    if (t == g_running) pf_stop(c, "DeleteItem of the current task: not yet");
    int32_t me = (int32_t)pf_r32(node + 24);
    for (int32_t i = pf_item_count() - 1; i > 0; --i) {
        uint32_t n = pf_item_node(i);
        if (n && n != node && (int32_t)pf_r32(n + 28) == me) pf_delete_item_as_owner(c, i);
    }
    pf_task_release(me);
    if (uint32_t owner = pf_item_node((int32_t)pf_r32(node + 28))) pf_signal(owner, SIGF_DEADTASK);
    for (size_t i = 0; i < g_ready.size(); ++i)
        if (g_ready[i] == t) { g_ready.erase(g_ready.begin() + (long)i); break; }
    for (size_t i = 0; i < g_tasks.size(); ++i)
        if (g_tasks[i] == t) { g_tasks.erase(g_tasks.begin() + (long)i); break; }
    t->dead = true;
    set_flags(t, 0, TASK_READY | TASK_WAITING);
    if (g_pf_trace) pf_log("        task \"%s\" deleted\n", name(t));
    pf_scavenge(false);
    return 0;
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
    arm_quantum();
    pf_on_swi(0x10001, k_waitsignal);
    pf_on_slot(PF_KERNEL, -148, k_exit);
    pf_on_swi(0x10002, k_sendsignal);
    pf_on_swi(0x10009, k_yield);
    pf_on_swi(0x1000a, k_setitempri);
    pf_on_swi(0x10015, k_allocsignal);
    pf_on_swi(0x10016, k_freesignal);
}
