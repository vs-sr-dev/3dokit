// 3dokit runtime -- what every build of recompiled ARM60 code links: guest
// memory, the active module, dispatch through a register, the return check.
#include "arm60.h"
#include <cstdio>
#include <cstring>

alignas(16) uint8_t g_arm_mem[ARM_MEM_SIZE];

static const ArmModule* g_active = nullptr;

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

const ArmModule* arm_identify() {
    for (int i = 0; i < g_arm_nmodules; ++i) {
        const ArmModule* m = g_arm_modules[i];
        if (m->base + m->size <= ARM_MEM_SIZE && arm_crc32(g_arm_mem + m->base, m->size) == m->crc) return m;
    }
    return nullptr;
}

void arm_activate(const ArmModule* m) { g_active = m; }

ArmFunc arm_lookup(uint32_t addr) {
    const ArmModule* m = g_active;
    if (!m) return nullptr;
    uint32_t lo = 0, hi = m->nfuncs;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (m->funcs[mid].addr < addr) lo = mid + 1;
        else hi = mid;
    }
    return lo < m->nfuncs && m->funcs[lo].addr == addr ? m->funcs[lo].fn : nullptr;
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
