// 3dokit runtime -- what every build of recompiled ARM60 code links: guest
// memory, the active module, dispatch through a register, the return check.
#include "arm60.h"
#include <cstdio>
#include <cstring>

alignas(16) uint8_t g_arm_mem[ARM_MEM_SIZE];

static const ArmModule* g_active = nullptr;     // the program at 0
static const ArmModule* g_loaded[16];           // every module in memory, the active one first
static int g_nloaded;

uint32_t arm_crc32(const uint8_t* p, size_t n, uint32_t crc) {
    static uint32_t table[256];
    if (!table[1]) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

const ArmModule* arm_module(const char* name) {
    for (int i = 0; i < g_arm_nmodules; ++i)
        if (!std::strcmp(g_arm_modules[i]->name, name)) return g_arm_modules[i];
    return nullptr;
}

const ArmModule* arm_identify(uint32_t at) {
    for (int i = 0; i < g_arm_nmodules; ++i) {
        const ArmModule* m = g_arm_modules[i];
        if (at + m->size <= ARM_MEM_SIZE && arm_crc32(g_arm_mem + at, m->size) == m->crc) return m;
    }
    return nullptr;
}

void arm_activate(const ArmModule* m) {
    g_active = m;
    g_nloaded = 0;
    if (!m) return;
    *m->base = 0;
    g_loaded[g_nloaded++] = m;
}

bool arm_load(const ArmModule* m, uint32_t base) {
    for (int i = 0; i < g_nloaded; ++i)
        if (g_loaded[i] == m) return false;
    if (g_nloaded == (int)(sizeof g_loaded / sizeof g_loaded[0])) return false;
    *m->base = base;
    g_loaded[g_nloaded++] = m;
    return true;
}

void arm_unload(const ArmModule* m) {
    for (int i = 0; i < g_nloaded; ++i)
        if (g_loaded[i] == m) {
            for (int j = i + 1; j < g_nloaded; ++j) g_loaded[j - 1] = g_loaded[j];
            --g_nloaded;
            if (m == g_active) g_active = nullptr;
            return;
        }
}

// An entry of the module `addr` falls in: from its base to the end of its read-only area or
// its last entry, whichever is further (hand-written code can sit in the read-write area).
static ArmFunc lookup_in(const ArmModule* m, uint32_t addr) {
    uint32_t off = addr - *m->base, end = m->size;
    if (m->nfuncs && m->funcs[m->nfuncs - 1].addr + 4 > end) end = m->funcs[m->nfuncs - 1].addr + 4;
    if (off >= end) return nullptr;
    uint32_t lo = 0, hi = m->nfuncs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->funcs[mid].addr < off) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->nfuncs && m->funcs[lo].addr == off ? m->funcs[lo].fn : nullptr;
}

ArmFunc arm_lookup(uint32_t addr) {
    for (int i = 0; i < g_nloaded; ++i)
        if (ArmFunc f = lookup_in(g_loaded[i], addr)) return f;
    return nullptr;
}

void arm_call(ArmCpu& c, uint32_t addr) {
    if (ArmFunc f = arm_lookup(addr)) {
        f(c);
        return;
    }
    arm_call_unknown(c, addr);
}

void arm_bad_return(ArmCpu& c, uint32_t expected) {
    char why[96];
    std::snprintf(why, sizeof why, "returned to %08X, not to the call's %08X", c.pc, expected);
    arm_fault(c, expected - 4, why);
}
