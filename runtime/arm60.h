// 3dokit runtime -- the ARM60 as seen by recompiled code.
//
// Recompiled functions have the signature `void f_XXXXXXXX(ArmCpu& c)`, one
// namespace per program (`python -m 3dokit.recomp`). They touch guest state
// only through this header: the registers and flags in ArmCpu, memory
// through ld*/st* (big-endian; the 3DO's 2 MB of DRAM at 0 and 1 MB of VRAM
// at 0x200000 as one host array, anything else through arm_io_*), and a few
// out-of-line services the runtime provides (calls through a register, the
// SWI door, safe points).
//
// Semantics are 3dokit/armemu.py's, instruction for instruction: the
// self-test (runtime/selftest.cpp) holds the two together.
#pragma once
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#define ARM_UNLIKELY(x) __builtin_expect(!!(x), 0)

struct ArmCpu {
    uint32_t r[16];
    uint32_t n, z, c, v;                // the flags, 0 or 1 each
    uint32_t pc;                        // where the last return went: the caller checks it
    int32_t  budget;                    // ARM60 clocks left before arm_poll
};

typedef void (*ArmFunc)(ArmCpu&);

// A program's recompiled code: its image (to recognise it in memory) and
// its entries, sorted by address.
struct ArmFuncEntry { uint32_t addr; ArmFunc fn; };
struct ArmModule {
    const char* name;
    uint32_t base, size, crc;           // crc32 of the image as loaded at base
    const ArmFuncEntry* funcs;
    uint32_t nfuncs;
};

// ---- services provided by the runtime ---------------------------------------------
enum : uint32_t { ARM_MEM_SIZE = 0x300000u };   // DRAM 0-0x1FFFFF, VRAM 0x200000-0x2FFFFF
extern uint8_t g_arm_mem[];
void     arm_call(ArmCpu& c, uint32_t addr);         // a call or jump through a register
void     arm_swi(ArmCpu& c, uint32_t number, uint32_t site);   // the OS's door; site: the swi's address
void     arm_poll(ArmCpu& c);                        // the budget is spent: time, other threads
void     arm_bad_return(ArmCpu& c, uint32_t expected);   // a return went elsewhere
[[noreturn]] void arm_fault(ArmCpu& c, uint32_t addr, const char* why);
uint32_t arm_io_read(uint32_t a, int size);          // outside DRAM and VRAM
void     arm_io_write(uint32_t a, uint32_t v, int size);

// The clocks the ARM60 would take: a block pays for its instructions at its start (recomp.emit's
// `clocks`), from the budget.
#define ARM_TICK(c, n) ((c).budget -= (int32_t)(n))
// A multiply's internal cycles: Booth's algorithm two bits of rs a cycle, ending when the rest
// are zero (1 to 16).
static inline int32_t arm_mul_m(uint32_t rs) {
    int32_t m = rs ? (32 - __builtin_clz(rs)) / 2 + 1 : 1;
    return m > 16 ? 16 : m;
}
// A safe point (backward branches and calls): the budget spent, the runtime's turn.
#define ARM_POLL(c) do { if (ARM_UNLIKELY((c).budget < 0)) arm_poll(c); } while (0)
// After a call: the callee's return must have come back here.
#define ARM_RET(c, a) do { if (ARM_UNLIKELY((c).pc != (a))) arm_bad_return(c, a); } while (0)

// ---- memory -------------------------------------------------------------------------
// ld32/st32 and ldm/stm ignore address bits 1-0; ldw is the ARM60's ldr,
// which rotates the aligned word right by 8 x address[1:0].
static inline uint32_t ld8(uint32_t a) {
    if (a < ARM_MEM_SIZE) return g_arm_mem[a];
    return arm_io_read(a, 1) & 0xFFu;
}
static inline uint32_t ld32(uint32_t a) {
    a &= ~3u;
    if (ARM_UNLIKELY(a >= ARM_MEM_SIZE)) return arm_io_read(a, 4);
    uint32_t v;
    std::memcpy(&v, g_arm_mem + a, 4);
    return __builtin_bswap32(v);
}
static inline uint32_t ldw(uint32_t a) {
    uint32_t v = ld32(a);
    return ARM_UNLIKELY(a & 3u) ? std::rotr(v, (int)(a & 3u) * 8) : v;
}
static inline void st8(uint32_t a, uint32_t v) {
    if (a < ARM_MEM_SIZE) g_arm_mem[a] = (uint8_t)v;
    else arm_io_write(a, v & 0xFFu, 1);
}
static inline void st32(uint32_t a, uint32_t v) {
    a &= ~3u;
    if (ARM_UNLIKELY(a >= ARM_MEM_SIZE)) { arm_io_write(a, v, 4); return; }
    uint32_t w = __builtin_bswap32(v);
    std::memcpy(g_arm_mem + a, &w, 4);
}

// ---- flags ----------------------------------------------------------------------------
static inline uint32_t arm_cpsr(const ArmCpu& c) {
    return c.n << 31 | c.z << 30 | c.c << 29 | c.v << 28 | 0x10u;     // user mode
}
static inline void arm_set_flags(ArmCpu& c, uint32_t w) {     // msr cpsr_f
    c.n = w >> 31; c.z = w >> 30 & 1u; c.c = w >> 29 & 1u; c.v = w >> 28 & 1u;
}
// x + y + cin with every flag set: sub is (a, ~b, 1), sbc (a, ~b, C), rsb (b, ~a, 1)...
static inline uint32_t arm_adc(ArmCpu& c, uint32_t x, uint32_t y, uint32_t cin) {
    uint64_t full = (uint64_t)x + y + cin;
    uint32_t r = (uint32_t)full;
    c.n = r >> 31; c.z = r == 0; c.c = (uint32_t)(full >> 32); c.v = ((x ^ r) & (y ^ r)) >> 31;
    return r;
}

// ---- the shifter by a register's bottom byte: (result, carry out) -----------------------
static inline uint32_t arm_lsl(uint32_t v, uint32_t n, uint32_t cin, uint32_t& cout) {
    if (n == 0) { cout = cin; return v; }
    if (n < 32) { cout = v >> (32 - n) & 1u; return v << n; }
    cout = n == 32 ? v & 1u : 0; return 0;
}
static inline uint32_t arm_lsr(uint32_t v, uint32_t n, uint32_t cin, uint32_t& cout) {
    if (n == 0) { cout = cin; return v; }
    if (n < 32) { cout = v >> (n - 1) & 1u; return v >> n; }
    cout = n == 32 ? v >> 31 : 0; return 0;
}
static inline uint32_t arm_asr(uint32_t v, uint32_t n, uint32_t cin, uint32_t& cout) {
    if (n == 0) { cout = cin; return v; }
    if (n < 32) { cout = v >> (n - 1) & 1u; return (uint32_t)((int32_t)v >> n); }
    cout = v >> 31; return (uint32_t)((int32_t)v >> 31);
}
static inline uint32_t arm_ror(uint32_t v, uint32_t n, uint32_t cin, uint32_t& cout) {
    if (n == 0) { cout = cin; return v; }
    n &= 31u;
    if (n == 0) { cout = v >> 31; return v; }
    cout = v >> (n - 1) & 1u; return std::rotr(v, (int)n);
}

// ---- the runtime's view of the modules (core.cpp) -------------------------------------
// The generated modules.cpp lists every module of the build; one is active
// at a time (every 3DO program is linked at 0).
extern const ArmModule* const g_arm_modules[];
extern const int g_arm_nmodules;
const ArmModule* arm_module(const char* name);
const ArmModule* arm_identify();                // the module whose image is in memory at 0
void    arm_activate(const ArmModule* m);
ArmFunc arm_lookup(uint32_t addr);              // nullptr if not an entry of the active module
void    arm_call_unknown(ArmCpu& c, uint32_t addr);   // services: not an entry (the OS...)
uint32_t arm_crc32(const uint8_t* p, size_t n, uint32_t crc = 0);
