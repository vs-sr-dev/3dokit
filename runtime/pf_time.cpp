// 3dokit runtime -- time: the guest's clock, the events that stand for the console's interrupts,
// and the vertical blank.
//
// The clock counts nanoseconds from the boot and is the guest's own, not the host's. Recompiled
// code passes a safe point (ARM_POLL) at every backward branch and call; every PF_POLL_EVERY of them
// it calls arm_poll, which moves the clock on by that many times g_pf_safe_point_ns, runs the
// events that have come due and lets a higher-priority task that one of them made ready take
// over. When every task waits (pf_task.cpp's block), the clock jumps straight to the next event.
// Nothing here looks at the host's time, so a run is the same on any host and at any speed: the
// trace of one run is the trace of every run. A run in real time (a window, sound) will hold the
// guest back to the host's clock where it jumps ahead -- not yet.
//
// What a safe point is worth is an estimate: the ARM60 runs at 12.5 MHz, and a safe point comes
// every few instructions, so 1 us (about 12 cycles) is the default. It sets how much work fits
// between two vertical blanks; a game that waits for the blank, as most do, is not otherwise
// changed by it.
//
// The kernel's own quantum (its "kernel quanta" FIRQ, os_code 0x18f40), which would let tasks of
// equal priority take turns, is not an event yet: no program run so far has two such tasks ready.
#include "pf.h"
#include <cstdio>
#include <queue>
#include <vector>

uint64_t g_pf_safe_point_ns = 1000;

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

void pf_time_init() {
    g_events = {};
    g_now = g_seq = 0;
    g_vbl.clear();
    pf_at(kFieldNs, vbl);
}

bool pf_time_idle(bool (*someone_ready)()) {
    uint64_t limit = g_now + kIdleLimitNs;
    while (!g_events.empty() && g_events.top().when <= limit) {
        run_until(g_events.top().when);
        if (someone_ready()) return true;
    }
    return false;
}

void arm_poll(ArmCpu& c) {
    uint64_t passed = (uint64_t)(PF_POLL_EVERY - c.budget);
    c.budget = PF_POLL_EVERY;
    run_until(g_now + passed * g_pf_safe_point_ns);
    pf_task_reschedule();
}
