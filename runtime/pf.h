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
#include <string>

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
void     pf_os_free(uint32_t a);                            // what pf_os_alloc gave, for its next of that size
uint32_t pf_os_string(const char* s);
uint32_t pf_os_next();                          // where the OS's next allocation will be

// A call from the OS into the program: `fn` with up to four arguments in r0-r3, on the caller's
// stack (below c's sp), on a copy of c's registers; r0 back. For the callbacks a program hands
// the OS, such as the C library's putc that the kernel's vfprintf writes through.
uint32_t pf_guest_call(const ArmCpu& c, uint32_t fn, uint32_t r0, uint32_t r1 = 0, uint32_t r2 = 0,
                       uint32_t r3 = 0);

// The cel engine (pf_cel.cpp): the CCB list from `ccb` on, into the bitmap that the control word
// and REGCTL0-3 describe, as MADAM draws it once GRAPHIX's DrawCels has set it going.
void     pf_cel_draw(ArmCpu& c, uint32_t cecontrol, const uint32_t regctl[4], uint32_t ccb);

// Items (pf_kernel.cpp): numbers for nodes in guest memory. A node starts
// with the SDK's ItemNode (nodes.h): n_SubsysType at +8, n_Type +9,
// n_Flags +11, n_Size +12, n_Name +16, n_Item +24, n_Owner +28; 36 bytes.
enum : uint32_t { PF_ITEMNODE_SIZE = 36 };
int32_t  pf_item_new(uint32_t node, int subsys, int type, const char* name);
uint32_t pf_item_node(int32_t item);            // 0 if no such item
uint32_t pf_check_item(int32_t item, int subsys, int type);    // CheckItem: 0 unless of that kind
int32_t  pf_item_count();                       // items are 1 to this, less one
void     pf_item_free(int32_t item);            // the node deleted: its number names nothing now
int32_t  pf_open_item(int32_t item);            // OpenItem and CloseItem by the current task
int32_t  pf_close_item(int32_t item);
// ItemOpened (Kernel -128): 0 when the task has the item open (OpenItem, not yet CloseItem),
// else the kernel's NOTFOUND; BADITEM when `task` is no task's item.
int32_t  pf_item_opened(int32_t task, int32_t item);
// CreateSizedItem for a subsystem's items: the folio's own creation routine (its ir_Create), given
// the node type and the caller's tags; the new item or an Err.
typedef uint32_t (*PfCreateItem)(ArmCpu& c, int type, uint32_t tags);
void     pf_on_create(int subsys, PfCreateItem fn);
// DeleteItem of a subsystem's item, after the kernel's own checks: the folio's ir_Delete, given
// the node type, the item and the deleting task's node; 0 lets the kernel free the item, anything
// else is DeleteItem's result and the item stays (os_code 0x1379c).
typedef int32_t (*PfDeleteItem)(ArmCpu& c, int type, int32_t item, uint32_t task);
void     pf_on_delete(int subsys, PfDeleteItem fn);
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

// Tasks (pf_task.cpp): the program's own task and the threads it makes, each on a host thread
// of its own, one running at a time; signals; the switch the kernel makes at the end of an OS
// call. A program's exit (SWI 0x11) throws PfExit: the program ends, or a thread does.
struct PfExit { int code; };
void     pf_task_init();
void     pf_task_reschedule();
uint32_t pf_create_task(ArmCpu& c, uint32_t tags);  // CreateSizedItem of a TASKNODE
int32_t  pf_delete_task(ArmCpu& c, uint32_t task);  // DeleteItem's own part for a TASKNODE
// A deleted task's opened items closed and its semaphores unlocked (pf_kernel.cpp).
void     pf_task_release(int32_t task);

// Time (pf_time.cpp): the guest's clock, in nanoseconds since the boot. It is not the host's:
// it moves on by a fixed amount at each safe point the recompiled code passes (backward branches
// and calls, ARM_POLL), and when every task waits it jumps to the next event. So a run is the
// same every time, however fast the host is. An event is what an interrupt does on the console
// (the vertical blank, the audio clock's tick): at its time, at the next safe point or as the
// waiting tasks idle, it runs and may signal tasks, and a higher-priority task made ready then
// runs at once, as it would when the interrupt returns.
typedef void (*PfTimeFn)(uint64_t when);
enum : int32_t { PF_POLL_EVERY = 64 };              // safe points between two calls of arm_poll
extern uint64_t g_pf_safe_point_ns;                 // the guest time a safe point stands for
void     pf_time_init();
uint64_t pf_now();
void     pf_at(uint64_t when, PfTimeFn fn);         // fn(when) at that time (or now, if past)
// Every task waits: the clock jumps to the next event and runs it. False when, after 10 s of
// guest time, no event has made a task ready (or there is no event at all).
bool     pf_time_idle(bool (*someone_ready)());
// The vertical blank: 59.94 fields a second (NTSC), each one running these, in this order.
void     pf_on_vbl(PfTimeFn fn);

// Semaphores (pf_kernel.cpp), as the 1993 kernel makes and locks them (os_code 0x13960,
// 0x139c8, 0x13b30): a node of semaphore.h's layout and its item.
int32_t  pf_semaphore_new(const char* name);
int32_t  pf_lock_item(int32_t item, uint32_t flags);   // LockItem: 1 locked, 0 not (no wait), or an Err
int32_t  pf_unlock_item(int32_t item);                 // UnlockItem

// Devices and IOReqs (pf_io.cpp). A device is a node of devices.h's layout with an item; its
// driver is a native function that starts an IOReq, as a command of a 1993 driver does: 1 when
// the request is done (the kernel's dispatch then completes it, and SendIO returns 1), 0 when it
// is queued (the driver clears IO_QUICK, and calls pf_complete_io when it is done) or completed
// it itself, an Err when the driver refuses it (SendIO returns that, the request left as the
// driver left it). A device may have a delete hook (dev_DeleteDev), which DeleteItem runs first;
// 0 lets the deletion go on. A node bigger than a Device (`size`, 0 for the Device alone) holds
// the driver's own fields after it.
typedef int32_t (*PfDispatchIO)(uint32_t ior);
typedef int32_t (*PfDeleteDev)(uint32_t dev);
uint32_t pf_device_new(const char* name, int max_unit, PfDispatchIO dispatch, PfDeleteDev del = nullptr,
                       uint32_t size = 0);
void     pf_complete_io(uint32_t ior);
// What the OS's own code asks of the kernel, as a program's SWI would: CreateIOReq on a device
// (CREATEIOREQ_TAG_DEVICE alone), SendIO with the IOInfo at guest address `info`, DeleteItem.
int32_t  pf_create_ioreq(ArmCpu& c, int32_t device);
int32_t  pf_send_io(ArmCpu& c, int32_t ior, uint32_t info);
int32_t  pf_delete_item(ArmCpu& c, int32_t item);
int32_t  pf_delete_item_as_owner(ArmCpu& c, int32_t item);   // the kernel's vector 34 (0x1387c)
int32_t  pf_signal(uint32_t task, uint32_t bits);   // the kernel's own SendSignal (pf_task.cpp)
// The current task's AllocSignal, FreeSignal and WaitSignal (pf_task.cpp), for the OS's own use.
uint32_t pf_alloc_signal(uint32_t sigs);
int32_t  pf_free_signal(uint32_t sigs);
int32_t  pf_wait_signal(uint32_t sigs);

// Messages (pf_msg.cpp), as the 1993 kernel makes and passes them: CreateSizedItem of a MsgPort
// and of a Message (the caller's tags and size; the item or an Err), and their deletion's own part.
uint32_t pf_create_msgport(ArmCpu& c, uint32_t tags, uint32_t size);
uint32_t pf_create_msg(ArmCpu& c, uint32_t tags, uint32_t size);
void     pf_delete_msgport(ArmCpu& c, uint32_t port);
void     pf_delete_msg(uint32_t msg);
// The OS's own ports and messages. A port made here has no task behind it: a message sent to it
// (by SendMsg or ReplyMsg) goes to `on_msg` with the port and the message, as the port's owner
// would be woken; the OS reads the port with pf_get_msg. A message made here has `reply_port` and,
// when `data_size` is not 0, a pass-by-value buffer of that size. pf_send_msg is SendMsg without
// the caller's checks; pf_reply_msg and pf_get_msg are ReplyMsg and GetMsg.
typedef void (*PfPortFn)(int32_t port, int32_t msg);
int32_t  pf_msgport_new(const char* name, PfPortFn on_msg);
int32_t  pf_msg_new(int32_t reply_port, uint32_t data_size);
int32_t  pf_send_msg(int32_t port, int32_t msg, uint32_t data, uint32_t size);
int32_t  pf_reply_msg(int32_t msg, int32_t result, uint32_t data, uint32_t size);
int32_t  pf_get_msg(int32_t port);

// The event broker (pf_event.cpp): the disc's System/Tasks/eventbroker (Aug 1993) at its message
// boundary -- its port "eventbroker", its listeners and their focus, and the events it reports --
// and the Control Port's first pad, whose buttons (event.h's ControlPadEventData bits) are what
// pf_pad_press schedules: `bits` down from field `first` (gf_VBLNumber, the fields counted from the
// boot) for `hold` fields, then up; `count` times, every `every` fields.
void     pf_pad_press(uint32_t bits, uint64_t first, int count, int every, int hold);

// Files (pf_file.cpp). The disc is a directory on the host, `g_pf_disc_root` (pfboot: the
// program's own directory unless --disc says otherwise); a program's path is walked there as the
// File folio walks it -- from the current directory or the root, through its aliases, with names
// matched without case. The host path, or "" when there is no such file.
extern std::string g_pf_disc_root;
std::string pf_host_path(const char* path);

// The folios' handlers register themselves here.
void     pf_kernel_init();
void     pf_msg_init();
void     pf_event_init();
void     pf_io_init();
void     pf_file_init();
void     pf_graphics_init();
void     pf_audio_init();
void     pf_math_init();

// What the display shows (pf_graphics.cpp): with g_pf_frames_dir set, at each vertical blank
// the field the VDLs describe, as a PPM in that directory whenever it differs from the last one
// written (vblNNNNNN.ppm, by gf_VBLNumber). A diagnostic, not the display. Only the fields from
// g_pf_frames_first to g_pf_frames_last, every g_pf_frames_every-th of them, are looked at.
extern const char* g_pf_frames_dir;
extern uint32_t g_pf_frames_first, g_pf_frames_last, g_pf_frames_every;

// The SDK's names (generated: pf_names.cpp).
struct PfSwiName { uint32_t number; const char* name; };
struct PfSlotName { const char* folio; int slot; const char* name; };
extern const PfSwiName g_pf_swi_names[];
extern const int g_pf_nswi_names;
extern const PfSlotName g_pf_slot_names[];
extern const int g_pf_nslot_names;
const char* pf_swi_name(uint32_t number);
const char* pf_slot_name(PfFolio folio, int slot);
