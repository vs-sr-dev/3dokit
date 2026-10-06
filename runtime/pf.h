// 3dokit runtime -- Portfolio, reimplemented at the folio boundary.
//
// A recompiled program reaches the OS two ways (3dokit/portfolio.py): by
// `swi folio << 16 | n`, which lands in arm_swi, and by `ldr pc, [base,
// #-slot]` through a folio's vector table, which lands on whatever the
// table holds. Here the tables hold trap addresses (PF_TRAP_BASE...) that
// no program code occupies, so the call reaches arm_call_unknown and from
// there the slot's native handler.
//
// The OS's own structures (KernelBase, the folios, later the items) live in
// guest address space above DRAM and VRAM, at PF_OS_BASE, in a host array:
// every access the program makes to them goes through arm_io_* and can be
// traced. A program only ever reaches them through pointers the OS handed
// it, so where they are does not matter to it.
//
// A handler is `void fn(ArmCpu& c)`: the arguments in r0-r3 and on the
// stack (APCS), the result in r0. For a vector the runtime returns to lr
// after it; a SWI simply goes on.
#pragma once
#include "arm60.h"

enum : uint32_t {
    PF_OS_BASE = 0x00400000u,           // the OS's memory, 1 MB
    PF_OS_SIZE = 0x00100000u,
    PF_TRAP_BASE = 0x00800000u,         // trap addresses: folio << 10 | slot index << 2
    PF_TRAP_SIZE = 0x00010000u,
};

// The folios a program can find, by the index the traps use. The names are
// the SDK's (3dokit.sdk), with which the generated pf_names.cpp is keyed.
enum PfFolio { PF_KERNEL, PF_GRAPHICS, PF_AUDIO, PF_FILE, PF_MATH, PF_NFOLIOS };
extern const char* const g_pf_folio_names[PF_NFOLIOS];
enum { PF_SLOTS = 64 };                 // vector slots per folio table

typedef void (*PfFn)(ArmCpu& c);

void     pf_on_swi(uint32_t number, PfFn fn);
void     pf_on_slot(PfFolio folio, int slot, PfFn fn);       // slot: the byte offset, -4, -8...
uint32_t pf_folio_base(PfFolio folio);                      // the folio's node in guest memory

// Boot a program (an AIF image's bytes) the way the OS's loader does: the
// image at 0, its zero-initialised data cleared, r7 = KernelBase, and the
// entry called with lr at the exit sentinel. Returns the exit code.
int      pf_run(const uint8_t* image, size_t size, uint32_t bss_end, uint32_t entry);

// The trace: every OS call with its arguments and result (level 1), and
// every access to the OS's memory other than a vector table (level 2).
extern int g_pf_trace;
extern bool g_pf_lenient;                   // an OS call not implemented returns 0
extern unsigned long long g_pf_max_calls;   // stop after this many OS calls (0: never)
void     pf_log(const char* fmt, ...);

// Guest memory helpers for handlers.
uint32_t pf_arg(const ArmCpu& c, int n);                    // the n-th argument (APCS)
size_t   pf_cstring(uint32_t addr, char* out, size_t max);   // a C string from guest memory
[[noreturn]] void pf_stop(ArmCpu& c, const char* why);

// The OS's own reads and writes (of its memory or the program's), untraced,
// and its allocations in its memory.
uint32_t pf_r32(uint32_t a);
uint32_t pf_r8(uint32_t a);
void     pf_w32(uint32_t a, uint32_t v);
void     pf_w8(uint32_t a, uint32_t v);
uint32_t pf_os_alloc(uint32_t size);
uint32_t pf_os_string(const char* s);

// Items (pf_kernel.cpp): numbers for nodes in guest memory. A node starts
// with the SDK's ItemNode (nodes.h): n_SubsysType at +8, n_Type +9,
// n_Flags +11, n_Size +12, n_Name +16, n_Item +24, n_Owner +28; 36 bytes.
enum : uint32_t { PF_ITEMNODE_SIZE = 36 };
int32_t  pf_item_new(uint32_t node, int subsys, int type, const char* name);
uint32_t pf_item_node(int32_t item);            // 0 if no such item
// An Err: negative, as Portfolio's are (the bits are not yet the OS's own).
enum : int32_t { PF_ERR_NOTFOUND = -1, PF_ERR_BADITEM = -2 };

// The folios' handlers register themselves here.
void     pf_kernel_init();
void     pf_file_init();

// The SDK's names (generated: pf_names.cpp).
struct PfSwiName { uint32_t number; const char* name; };
struct PfSlotName { const char* folio; int slot; const char* name; };
extern const PfSwiName g_pf_swi_names[];
extern const int g_pf_nswi_names;
extern const PfSlotName g_pf_slot_names[];
extern const int g_pf_nslot_names;
const char* pf_swi_name(uint32_t number);
const char* pf_slot_name(PfFolio folio, int slot);
