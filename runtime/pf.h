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
// The same boot without the call: 0 when the program and the OS are in place.
int      pf_boot(const uint8_t* image, size_t size, uint32_t bss_end);

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
uint32_t pf_os_next();                          // where the OS's next allocation will be

// Items (pf_kernel.cpp): numbers for nodes in guest memory. A node starts
// with the SDK's ItemNode (nodes.h): n_SubsysType at +8, n_Type +9,
// n_Flags +11, n_Size +12, n_Name +16, n_Item +24, n_Owner +28; 36 bytes.
enum : uint32_t { PF_ITEMNODE_SIZE = 36 };
int32_t  pf_item_new(uint32_t node, int subsys, int type, const char* name);
uint32_t pf_item_node(int32_t item);            // 0 if no such item
uint32_t pf_check_item(int32_t item, int subsys, int type);    // CheckItem: 0 unless of that kind
int32_t  pf_item_count();                       // items are 1 to this, less one
// An Err: negative, as Portfolio's are (the bits are not yet the OS's own).
enum : int32_t { PF_ERR_NOTFOUND = -1, PF_ERR_BADITEM = -2 };

// Where the 1993 headers put the fields a program reads directly (the 1.2 and 1.3 SDKs
// agree; the offsets are a compiler's over those headers, and each one used is checked
// against a program's own reads).
enum : uint32_t {
    // List (list.h): a Node, then the anchor
    PF_LIST_SIZE = 0x20,
    PF_LIST_HEAD = 0x14,                // ListAnchor.head.links.flink: the first node
    PF_LIST_TAIL = 0x18,                // the pseudo-node the last node's n_Next points at
    PF_LIST_LAST = 0x1c,                // ListAnchor.tail.links.blink: the last node
    // KernelBase (kernel.h)
    KB_MEMFREELISTS = 0x74, KB_MEMHDRLIST = 0x78, KB_CURRENTTASK = 0x98,
    // Task (task.h)
    PF_TASK_SIZE = 0xdc, T_WAITBITS = 0x30, T_SIGBITS = 0x34, T_ALLOCATEDSIGS = 0x38,
    T_STACKBASE = 0x3c, T_STACKSIZE = 0x40, T_FREEMEMORYLISTS = 0xa8,
    // GrafFolio (graphics.h)
    GF_VBLNUMBER = 0x74, GF_ZEROPAGE = 0x78, GF_VIRSPAGE = 0x7c, GF_VRAMPAGESIZE = 0x80,
    GF_DEFAULTDISPLAYWIDTH = 0x84, GF_DEFAULTDISPLAYHEIGHT = 0x88, GF_VDLFORCEDFIRST = 0x9c,
    GF_VDLPREDISPLAY = 0xa0, GF_VDLPOSTDISPLAY = 0xa4, GF_VDLBLANK = 0xa8, GF_CURRENTVDLEVEN = 0xac,
    GF_CURRENTVDLODD = 0xb0, GF_VDLDISPLAYLINK = 0xb4, GF_VBLTIME = 0xc4, GF_VBLFREQ = 0xc8,
};

// Lists (pf_kernel.cpp), as the kernel's own functions link them.
void     pf_list_init(uint32_t l, const char* name);
void     pf_list_insert_before(uint32_t at, uint32_t n);
void     pf_list_add_head(uint32_t l, uint32_t n);
void     pf_list_add_tail(uint32_t l, uint32_t n);
void     pf_list_insert_from_tail(uint32_t l, uint32_t n);   // by n_Priority
void     pf_list_rem_node(uint32_t n);

// Memory (pf_mem.cpp). The flags are mem.h's.
enum : uint32_t {
    MEMTYPE_FILL = 0x100, MEMTYPE_MYPOOL = 0x200, MEMTYPE_TASKMEM = 0x8000,
    MEMTYPE_VRAM = 0x10000, MEMTYPE_DMA = 0x20000, MEMTYPE_CEL = 0x40000, MEMTYPE_DRAM = 0x80000,
    MEMTYPE_INPAGE = 0x1000000, MEMTYPE_STARTPAGE = 0x2000000, MEMTYPE_SYSTEMPAGESIZE = 0x4000000,
    MEMTYPE_BANK1 = 0x10000000, MEMTYPE_BANK2 = 0x20000000, MEMTYPE_BANKSELECT = 0x40000000,
};
// The MemHdrs, the OS's MemLists, the program's task and its MemLists: the program's image
// [0, image_end) and its stack [stack_base, top of DRAM) are the task's pages already.
void     pf_mem_init(const char* task_name, uint32_t image_end, uint32_t stack_base);
uint32_t pf_current_task();
uint32_t pf_kernel_lists();                         // KernelBase->kb_MemFreeLists
// AllocMemFromMemLists and FreeMemToMemLists; `user` is the mode of the caller, which
// decides where more pages come from (the task's, or the OS's from the top).
uint32_t pf_alloc_mem(uint32_t lists, int32_t size, uint32_t flags, bool user);
void     pf_free_mem(uint32_t lists, uint32_t p, int32_t size);
uint32_t pf_page_size(uint32_t flags);              // GetPageSize
uint32_t pf_find_mh(uint32_t p);                    // FindMH
int32_t  pf_scavenge(bool user);                    // ScavengeMem, SystemScavengeMem
// 0 when every page of [p, p + size) is writable by `task`, else the kernel's BADPTR
int32_t  pf_task_can_write(uint32_t task, uint32_t p, int32_t size);

// A test of the allocator (pf_memtest.cpp): after the boot, `ops` random allocations, frees
// and scavenges from `seed`, written to DIR as ops.txt with the guest memory before and
// after (before.bin, after.bin: DRAM and VRAM, then the OS's memory), for 3dokit.pfcheck to
// replay on the 1993 kernel's own code.
int      pf_memtest(const char* dir, int ops, uint32_t seed);
const uint8_t* pf_os_memory();

// A snapshot of one OS call (pf_memtest.cpp), the g_pf_snap_call-th as the trace counts them:
// before its handler runs, the guest memory to DIR/before.bin and the call, the registers, the
// task, the items and where the OS allocates next to DIR/call.txt; after it, the memory to
// after.bin and the result to call.txt, and the run ends. python -m 3dokit.pfcheck replays the
// call on the 1993 OS's own code over the memory before.
extern unsigned long long g_pf_snap_call;
extern const char* g_pf_snap_dir;
void     pf_snap_before(const ArmCpu& c, const char* call);
[[noreturn]] void pf_snap_after(const ArmCpu& c);

// Devices and IOReqs (pf_io.cpp). A device is a node of devices.h's layout with an item; its
// driver is a native function that starts an IOReq and calls pf_complete_io when it is done.
typedef void (*PfDispatchIO)(uint32_t ior);
uint32_t pf_device_new(const char* name, int max_unit, PfDispatchIO dispatch);
void     pf_complete_io(uint32_t ior);
int32_t  pf_signal(uint32_t task, uint32_t bits);   // the kernel's own SendSignal

// The folios' handlers register themselves here.
void     pf_kernel_init();
void     pf_io_init();
void     pf_file_init();
void     pf_graphics_init();

// The SDK's names (generated: pf_names.cpp).
struct PfSwiName { uint32_t number; const char* name; };
struct PfSlotName { const char* folio; int slot; const char* name; };
extern const PfSwiName g_pf_swi_names[];
extern const int g_pf_nswi_names;
extern const PfSlotName g_pf_slot_names[];
extern const int g_pf_nslot_names;
const char* pf_swi_name(uint32_t number);
const char* pf_slot_name(PfFolio folio, int slot);
