// 3dokit runtime -- Portfolio's frame: the OS's memory, the folio tables and
// their traps, the SWI and vector dispatch, the trace, and the boot. The
// folios' functions are in pf_kernel.cpp and the files after it.
#include "pf.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

const char* const g_pf_folio_names[PF_NFOLIOS] = {"Kernel", "Graphics", "audio", "File", "Operamath"};
int g_pf_trace = 1;
bool g_pf_lenient = false;

static uint8_t g_os[PF_OS_SIZE];
static PfFn g_slots[PF_NFOLIOS][PF_SLOTS];
static std::map<uint32_t, PfFn> g_swis;
static unsigned long long g_ncalls;
unsigned long long g_pf_max_calls;


static const uint32_t kExitSentinel = 0xFFFFFFF0u;
static const uint32_t kStackBase = 0x00200000u - 0x10000;   // the program's stack: 64 KB under the top of DRAM

// ---- names -----------------------------------------------------------------------------
const char* pf_swi_name(uint32_t number) {
    for (int i = 0; i < g_pf_nswi_names; ++i)
        if (g_pf_swi_names[i].number == number) return g_pf_swi_names[i].name;
    return "?";
}

const char* pf_slot_name(PfFolio folio, int slot) {
    for (int i = 0; i < g_pf_nslot_names; ++i)
        if (g_pf_slot_names[i].slot == slot && !std::strcmp(g_pf_slot_names[i].folio, g_pf_folio_names[folio]))
            return g_pf_slot_names[i].name;
    return "?";
}

// ---- the trace -------------------------------------------------------------------------
void pf_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

[[noreturn]] void arm_fault(ArmCpu& c, uint32_t addr, const char* why) {
    std::fflush(stdout);
    std::fprintf(stderr, "fault at %08X: %s\n ", addr, why);
    for (int i = 0; i < 16; ++i) std::fprintf(stderr, " r%d=%08X", i, c.r[i]);
    std::fprintf(stderr, " nzcv=%u%u%u%u\n", c.n, c.z, c.c, c.v);
    std::exit(3);
}

// The site of the OS call this task's host thread is in (0 outside one): where a stop inside a
// call is reported. Outside a call, the return address in lr -- which compiled code may also use
// as a register of its own, as launchme's sound code does.
static thread_local uint32_t t_site;

[[noreturn]] void pf_stop(ArmCpu& c, const char* why) {
    arm_fault(c, t_site ? t_site : c.r[14], why);
}

// ---- the OS's memory ---------------------------------------------------------------------
static bool in_os(uint32_t a, int size) { return a - PF_OS_BASE <= PF_OS_SIZE - (uint32_t)size; }

static bool in_vectors(uint32_t a) {
    for (int f = 0; f < PF_NFOLIOS; ++f) {
        uint32_t base = pf_folio_base((PfFolio)f);
        if (a < base && a >= base - 4u * PF_SLOTS) return true;
    }
    return false;
}

static uint32_t os_get(uint32_t a, int size) {
    const uint8_t* p = g_os + (a - PF_OS_BASE);
    uint32_t v = 0;
    for (int i = 0; i < size; ++i) v = v << 8 | p[i];
    return v;
}

static void os_put(uint32_t a, uint32_t v, int size) {
    uint8_t* p = g_os + (a - PF_OS_BASE);
    for (int i = size - 1; i >= 0; --i, v >>= 8) p[i] = (uint8_t)v;
}

uint32_t arm_io_read(uint32_t a, int size) {
    if (in_os(a, size)) {
        uint32_t v = os_get(a, size);
        if (g_pf_trace >= 2 && !in_vectors(a)) pf_log("        os read%d  %08X -> %08X\n", size * 8, a, v);
        return v;
    }
    std::fflush(stdout);
    std::fprintf(stderr, "fault: a %d-byte read at %08X, outside DRAM, VRAM and the OS\n", size, a);
    std::exit(3);
}

void arm_io_write(uint32_t a, uint32_t v, int size) {
    if (in_os(a, size)) {
        if (g_pf_trace >= 2) pf_log("        os write%d %08X <- %08X\n", size * 8, a, v);
        os_put(a, v, size);
        return;
    }
    std::fflush(stdout);
    std::fprintf(stderr, "fault: a %d-byte write of %08X at %08X, outside DRAM, VRAM and the OS\n", size, v, a);
    std::exit(3);
}

uint32_t pf_folio_base(PfFolio folio) { return PF_OS_BASE + 0x1000u * (folio + 1); }

uint32_t pf_arg(const ArmCpu& c, int n) {
    return n < 4 ? c.r[n] : ld32(c.r[13] + 4u * (n - 4));
}

size_t pf_cstring(uint32_t addr, char* out, size_t max) {
    size_t n = 0;
    while (n + 1 < max) {
        char ch = (char)ld8(addr + n);
        if (!ch) break;
        out[n++] = ch;
    }
    out[n] = 0;
    return n;
}

// ---- dispatch ------------------------------------------------------------------------------
void pf_on_swi(uint32_t number, PfFn fn) { g_swis[number] = fn; }

void pf_on_slot(PfFolio folio, int slot, PfFn fn) {
    int idx = -slot / 4 - 1;
    if (slot >= 0 || slot % 4 || idx >= PF_SLOTS) {
        std::fprintf(stderr, "pf_on_slot: %s slot %d out of range\n", g_pf_folio_names[folio], slot);
        std::exit(2);
    }
    g_slots[folio][idx] = fn;
}

// One OS call: counted and traced, then its handler -- or, not implemented, a stop, or with
// --lenient 0 and on (a preview of what a program calls next, not a run to trust). `call` is the
// call as a snapshot names it (pf_snap_before).
static void os_call(ArmCpu& c, PfFn fn, const char* what, uint32_t site, const char* call) {
    ++g_ncalls;
    t_site = site;
    if (g_pf_trace)
        pf_log("[%5llu] %06X %-40s r0=%08X r1=%08X r2=%08X r3=%08X\n", g_ncalls, site, what,
               c.r[0], c.r[1], c.r[2], c.r[3]);
    if (g_pf_max_calls && g_ncalls >= g_pf_max_calls) pf_stop(c, "--max-calls reached");
    bool snap = g_ncalls == g_pf_snap_call;
    if (snap) pf_snap_before(c, call);          // a call not implemented yet too: what it is given
    if (!fn) {
        if (g_pf_lenient) {
            pf_log("        (not implemented: returns 0)\n");
            c.r[0] = 0;
            return;
        }
        char why[112];
        std::snprintf(why, sizeof why, "%s: not implemented", what);
        pf_stop(c, why);
    }
    fn(c);
    if (g_pf_trace) pf_log("        -> %08X\n", c.r[0]);
    if (snap) pf_snap_after(c);
    pf_task_reschedule();
    t_site = 0;
}

void arm_swi(ArmCpu& c, uint32_t number, uint32_t site) {
    char what[64], call[32];
    std::snprintf(what, sizeof what, "swi %#x %s", number, pf_swi_name(number));
    std::snprintf(call, sizeof call, "swi %#x", number);
    auto it = g_swis.find(number);
    os_call(c, it == g_swis.end() ? nullptr : it->second, what, site, call);
}

void arm_call_unknown(ArmCpu& c, uint32_t addr) {
    if (addr - PF_TRAP_BASE < PF_TRAP_SIZE) {
        uint32_t off = addr - PF_TRAP_BASE;
        int folio = off >> 10, idx = (off >> 2) & 255;
        int slot = -4 * (idx + 1);
        if (folio < PF_NFOLIOS && idx < PF_SLOTS) {
            char what[80], call[32];
            std::snprintf(what, sizeof what, "%s %d %s", g_pf_folio_names[folio], slot,
                          pf_slot_name((PfFolio)folio, slot));
            std::snprintf(call, sizeof call, "slot %s %d", g_pf_folio_names[folio], slot);
            os_call(c, g_slots[folio][idx], what, c.r[14] - 4, call);
            c.pc = c.r[14] & ~3u;               // the folio's function returns to lr
            return;
        }
    }
    arm_fault(c, addr, "a call to an address that is no function's entry and no OS trap");
}

static const uint32_t kGuestReturn = 0xFFFFFFE0u;      // where a call from the OS returns

uint32_t pf_guest_call(const ArmCpu& c, uint32_t fn, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    ArmCpu g = c;
    g.r[0] = r0;
    g.r[1] = r1;
    g.r[2] = r2;
    g.r[3] = r3;
    g.r[14] = kGuestReturn;
    arm_call(g, fn);
    if (g.pc != kGuestReturn) arm_fault(g, g.pc, "a call from the OS returned somewhere else");
    return g.r[0];
}

// ---- the OS's own access to memory: not traced ---------------------------------------------
uint32_t pf_r32(uint32_t a) { return in_os(a, 4) ? os_get(a, 4) : ld32(a); }
uint32_t pf_r8(uint32_t a) { return in_os(a, 1) ? os_get(a, 1) : ld8(a); }
void pf_w32(uint32_t a, uint32_t v) { if (in_os(a, 4)) os_put(a, v, 4); else st32(a, v); }
void pf_w8(uint32_t a, uint32_t v) { if (in_os(a, 1)) os_put(a, v, 1); else st8(a, v); }

static uint32_t g_os_free;                      // the OS's own allocations, upward

uint32_t pf_os_alloc(uint32_t size) {
    uint32_t a = g_os_free;
    g_os_free = (g_os_free + size + 3) & ~3u;
    if (g_os_free > PF_OS_BASE + PF_OS_SIZE) {
        std::fprintf(stderr, "the OS's memory is full\n");
        std::exit(3);
    }
    return a;
}

uint32_t pf_os_next() { return g_os_free; }

uint32_t pf_os_string(const char* s) {
    size_t n = std::strlen(s) + 1;
    uint32_t a = pf_os_alloc((uint32_t)n);
    std::memcpy(g_os + (a - PF_OS_BASE), s, n);
    return a;
}

// ---- the boot ------------------------------------------------------------------------------
static void build_folios() {
    std::memset(g_os, 0, sizeof g_os);
    g_os_free = PF_OS_BASE + 0x10000;           // above the folios' pages
    for (int f = 0; f < PF_NFOLIOS; ++f) {
        uint32_t base = pf_folio_base((PfFolio)f);
        for (int i = 0; i < PF_SLOTS; ++i)
            os_put(base - 4u * (i + 1), PF_TRAP_BASE + ((uint32_t)f << 10) + 4u * i, 4);
    }
}

static void pf_exit_swi(ArmCpu& c) { throw PfExit{(int)c.r[0]}; }

static const ArmModule* g_module;

int pf_boot(const uint8_t* image, size_t size, uint32_t bss_end) {
    if (size > ARM_MEM_SIZE || bss_end > ARM_MEM_SIZE) {
        std::fprintf(stderr, "the program does not fit in DRAM\n");
        return 2;
    }
    std::memset(g_arm_mem, 0, ARM_MEM_SIZE);
    std::memcpy(g_arm_mem, image, size);
    const ArmModule* m = g_module = arm_identify();
    if (!m) {
        std::fprintf(stderr, "no recompiled module matches this program\n");
        return 2;
    }
    arm_activate(m);
    build_folios();
    pf_on_swi(0x11, pf_exit_swi);
    pf_time_init();
    pf_kernel_init();
    pf_msg_init();
    pf_mem_init(m->name, bss_end, kStackBase);
    pf_task_init();
    pf_io_init();
    pf_file_init();
    pf_graphics_init();
    pf_audio_init();
    pf_math_init();
    pf_event_init();                            // after the graphics: its fields are gf_VBLNumber's
    // argv: the program's name, in the OS's memory
    const uint32_t argv = PF_OS_BASE + 0x100, name = PF_OS_BASE + 0x110;
    os_put(argv, name, 4);
    os_put(argv + 4, 0, 4);
    std::memcpy(g_os + (name - PF_OS_BASE), m->name, std::strlen(m->name) + 1);
    return 0;
}

const uint8_t* pf_os_memory() { return g_os; }

int pf_run(const uint8_t* image, size_t size, uint32_t bss_end, uint32_t entry) {
    if (int bad = pf_boot(image, size, bss_end)) return bad;
    const ArmModule* m = g_module;
    const uint32_t argv = PF_OS_BASE + 0x100;
    ArmCpu c{};
    c.r[5] = 1;                                 // argc
    c.r[6] = argv;
    c.r[7] = pf_folio_base(PF_KERNEL);          // KernelBase
    c.r[13] = 0x00200000u - 16;                 // the stack: the top of DRAM
    c.r[10] = kStackBase;                       // sl, 64 KB below
    c.r[14] = kExitSentinel;
    c.budget = PF_POLL_EVERY;
    pf_log("boot %s: entry %08X, KernelBase %08X, bss to %08X\n", m->name, entry, c.r[7], bss_end);
    try {
        arm_call(c, entry);
    } catch (const PfExit& e) {
        pf_log("exit(%d) after %llu OS calls\n", e.code, g_ncalls);
        return e.code;
    }
    if (c.pc != kExitSentinel) arm_fault(c, c.pc, "the program returned somewhere other than the OS");
    pf_log("returned to the OS, r0 = %d, after %llu OS calls\n", (int)c.r[0], g_ncalls);
    return (int)c.r[0];
}
