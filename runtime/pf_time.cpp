// 3dokit runtime -- time: the guest's clock, the events that stand for the console's interrupts,
// and the vertical blank.
//
// The clock counts nanoseconds from the boot and is the guest's own, not the host's. Recompiled
// code takes from its budget the clocks the ARM60 would spend on it (ARM_TICK: the datasheet's
// cycles, an N cycle two clocks, a block paying at its start; recomp.emit's `clocks`) and passes
// a safe point (ARM_POLL) at every backward branch and call; at the first safe point after
// PF_POLL_EVERY clocks it calls arm_poll, which moves the clock on by the clocks spent at
// g_pf_clock_ns each (80 ns: the 3DO's ARM60 runs at 12.5 MHz), runs the events that have come
// due and lets a higher-priority task that one of them made ready take over. When every task waits (pf_task.cpp's block), the clock jumps straight to the next event.
// Nothing here looks at the host's time, so a run is the same on any host and at any speed: the
// trace of one run is the trace of every run. A run in real time (pfboot's window) holds the
// guest back to the host's clock at each vertical blank, which changes when the guest's events
// happen in the host's time but not in the guest's: the run is still the same for the same pad.
//
// It sets how much work fits between two vertical blanks, as on the console -- what is not
// counted: the OS's work (the runtime's own, native, takes no guest time), the cel engine's and
// the other DMA's share of the bus (the ARM60 waits for them on the console), a CD's reading
// time. (A fixed 1 us a safe point, as before, made the guest 1.5 to 7 times the console's speed:
// a safe point stands for 19 clocks in Crash 'n Burn's menus, 90 in its race.)
//
// The kernel's own quantum (its "kernel quanta" FIRQ, os_code 0x18f40), which would let tasks of
// equal priority take turns, is not an event yet: no program run so far has two such tasks ready.
#include "pf.h"
#include <cstdio>
#include <queue>
#include <vector>

uint64_t g_pf_clock_ns = 80;

// The NTSC field: 1001/60 ms. GRAPHIX's gf_VBLTime says 16,684 us, its rounding of the same.
static const uint64_t kFieldNs = 16683333;
static const uint64_t kIdleLimitNs = 10000000000ull;   // 10 s with every task waiting

struct Event {
    uint64_t when, seq;
    PfTimeFn fn;
    bool operator>(const Event& o) const { return when != o.when ? when > o.when : seq > o.seq; }
};
static std::priority_queue<Event, std::vector<Event>, std::greater<Event>> g_events;
static uint64_t g_now, g_seq;
static std::vector<PfTimeFn> g_vbl;

uint64_t pf_now() { return g_now; }

void pf_at(uint64_t when, PfTimeFn fn) { g_events.push({when < g_now ? g_now : when, g_seq++, fn}); }

void pf_on_vbl(PfTimeFn fn) { g_vbl.push_back(fn); }

// Every event due by `t`, in the order of their times (and, at one time, of their making); the
// clock stands at each one's time as it runs.
static void run_until(uint64_t t) {
    while (!g_events.empty() && g_events.top().when <= t) {
        Event e = g_events.top();
        g_events.pop();
        g_now = e.when;
        e.fn(e.when);
    }
    g_now = t;
}

static void vbl(uint64_t when) {
    for (PfTimeFn fn : g_vbl) fn(when);
    pf_at(when + kFieldNs, vbl);
}

// The first program's boot starts the clock at 0; a later one's (the shell running the next
// program, pfboot --boot) keeps it running, its first blank at the next field's.
static bool g_started;

void pf_time_init() {
    g_events = {};
    g_vbl.clear();
    if (!g_started) g_now = g_seq = 0;
    g_started = true;
    pf_at((g_now / kFieldNs + 1) * kFieldNs, vbl);
}

bool g_pf_wait_forever;

bool pf_time_idle(bool (*someone_ready)()) {
    uint64_t limit = g_pf_wait_forever ? ~0ull : g_now + kIdleLimitNs;
    while (!g_events.empty() && g_events.top().when <= limit) {
        run_until(g_events.top().when);
        if (someone_ready()) return true;
    }
    return false;
}

void arm_poll(ArmCpu& c) {
    uint64_t passed = (uint64_t)(PF_POLL_EVERY - c.budget);
    c.budget = PF_POLL_EVERY;
    run_until(g_now + passed * g_pf_clock_ns);
    pf_task_reschedule();
}
