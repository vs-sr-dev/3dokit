// 3dokit runtime -- the services with no OS and no hardware behind them: a
// build that only runs the game's own code (the self-test) stops on any SWI,
// any call outside the program, any access outside DRAM and VRAM.
#include "arm60.h"
#include <cstdio>
#include <cstdlib>

[[noreturn]] void arm_fault(ArmCpu& c, uint32_t addr, const char* why) {
    std::fprintf(stderr, "fault at %08X: %s\n ", addr, why);
    for (int i = 0; i < 16; ++i) std::fprintf(stderr, " r%d=%08X", i, c.r[i]);
    std::fprintf(stderr, " nzcv=%u%u%u%u\n", c.n, c.z, c.c, c.v);
    std::exit(3);
}

void arm_swi(ArmCpu& c, uint32_t number, uint32_t site) {
    char why[48];
    std::snprintf(why, sizeof why, "swi %#x with no OS", number);
    arm_fault(c, site, why);
}

void arm_call_unknown(ArmCpu& c, uint32_t addr) {
    arm_fault(c, addr, "a call to an address that is no function's entry");
}

void arm_poll(ArmCpu& c) { c.budget = 1 << 20; }

uint32_t arm_io_read(uint32_t a, int size) {
    std::fprintf(stderr, "fault: a %d-byte read at %08X, outside DRAM and VRAM\n", size, a);
    std::exit(3);
}

void arm_io_write(uint32_t a, uint32_t v, int size) {
    std::fprintf(stderr, "fault: a %d-byte write of %08X at %08X, outside DRAM and VRAM\n", size, v, a);
    std::exit(3);
}
