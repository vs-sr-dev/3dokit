// 3dokit runtime -- devices and IO: IOReqs, SendIO and CompleteIO as the 1993 kernel (os_code
// v0.16) does them, signals as far as IO needs them, and the devices the programs run so far
// open. The kernel's functions are read in its code (the addresses are its own); a device's
// driver is the native function the device was made with.
#include "pf.h"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>

enum : uint32_t {
    DEVICENODE = 15, IOREQNODE = 14, TASKNODE = 5, MESSAGENODE = 9,
    // Device (device.h; the kernel's CreateDevice and OpenDevice store these)
    DEV_DRIVER = 0x24, DEV_OPENCNT = 0x28, DEV_IOREQSIZE = 0x3c, DEV_IOREQS = 0x40,
    DEV_MAXUNITNUM = 0x60, DEV_SIZE = 0x70,
    // IOReq (io.h)
    IO_LINK = 0x24, IO_DEV = 0x2c, IO_CALLBACK = 0x30, IO_INFO = 0x34, IO_ACTUAL = 0x54,
    IO_FLAGS = 0x58, IO_ERROR = 0x5c, IO_MSGITEM = 0x68, IO_SIGITEM = 0x6c, IOREQ_SIZE = 0x70,
    // IOInfo, from the IOReq
    IOI_COMMAND = IO_INFO, IOI_FLAGS = IO_INFO + 1, IOI_UNIT = IO_INFO + 2, IOI_FLAGS2 = IO_INFO + 3,
    IOI_CMDOPTIONS = IO_INFO + 4, IOI_OFFSET = IO_INFO + 12, IOI_SEND_BUF = IO_INFO + 16,
    IOI_SEND_LEN = IO_INFO + 20, IOI_RECV_BUF = IO_INFO + 24, IOI_RECV_LEN = IO_INFO + 28,
    IO_DONE = 1, IO_QUICK = 2,
    SIGF_IODONE = 8,
};

// The kernel's errors (kernel.h's MAKEKERR, the values os_code builds).
enum : uint32_t {
    KERR_BADITEM = 0xD57B9001u, KERR_BADTAG = 0xD57B9002u, KERR_NOTFOUND = 0xD57B9005u,
    KERR_NOTPRIV = 0xD57B9004u, KERR_BADPTR = 0xD57B9009u, KERR_BADUNIT = 0xD57B900Bu,
    KERR_BADCOMMAND = 0xD57B900Cu, KERR_BADIOARG = 0xD57B900Du, KERR_IOINPROGRESS = 0xD57B900Fu,
    KERR_NOTOWNER = 0xD57B9012u, KERR_ILLEGALSIGNAL = 0xD57B9116u,
    TASK_SUPER = 8,
};

static std::map<uint32_t, PfDispatchIO> g_drivers;     // device node -> its driver
static std::map<uint32_t, PfDeleteDev> g_delete_hooks; // device node -> its dev_DeleteDev

static uint32_t task_item() { return pf_r32(pf_current_task() + 24); }

// ---- devices --------------------------------------------------------------------------------
// A device as CreateDevice (0x14974) leaves one: its IOReq size (0x70 when none is given), its
// list of IOReqs, its units; and, here, its driver.
uint32_t pf_device_new(const char* name, int max_unit, PfDispatchIO dispatch, PfDeleteDev del, uint32_t size) {
    if (!size) size = DEV_SIZE;
    uint32_t dev = pf_os_alloc(size);
    pf_w32(dev + 12, size);
    pf_item_new(dev, 1, DEVICENODE, name);
    pf_w32(dev + DEV_IOREQSIZE, IOREQ_SIZE);
    pf_list_init(dev + DEV_IOREQS, "Device ioreqs");
    pf_w8(dev + DEV_MAXUNITNUM, (uint32_t)max_unit);
    g_drivers[dev] = dispatch;
    if (del) g_delete_hooks[dev] = del;
    return dev;
}

// ---- IOReqs ---------------------------------------------------------------------------------
// CreateSizedItem of an IOReq (0x13c88): tags TAG_ITEM_NAME (1), TAG_ITEM_PRI (2),
// CREATEIOREQ_TAG_REPLYPORT (10), CREATEIOREQ_TAG_DEVICE (11, required). The device must be
// open (by the task: one task here); the node is the device's IOReq size, on the device's list,
// born done. Without a reply port both io_MsgItem and io_SigItem are the task, so completion
// signals it.
static uint32_t make_ioreq(ArmCpu& c, bool have_dev, uint32_t dev_item, uint32_t port, uint32_t name,
                           uint32_t pri);

static uint32_t create_ioreq(ArmCpu& c, uint32_t tags) {
    uint32_t dev_item = 0, port = 0, name = 0, pri = 0;
    bool have_dev = false;
    for (uint32_t t = tags;; ) {
        uint32_t tag = pf_r32(t);
        if (!tag) break;
        uint32_t v = pf_r32(t + 4);
        t += 8;
        if (tag == 10) port = v;
        else if (tag == 1) name = v;
        else if (tag == 2) pri = v & 0xff;
        else if (tag == 11) { dev_item = v; have_dev = true; }
        else if (tag != 0xff) return KERR_BADTAG;
    }
    return make_ioreq(c, have_dev, dev_item, port, name, pri);
}

int32_t pf_create_ioreq(ArmCpu& c, int32_t device) { return (int32_t)make_ioreq(c, true, (uint32_t)device, 0, 0, 0); }

static uint32_t make_ioreq(ArmCpu& c, bool have_dev, uint32_t dev_item, uint32_t port, uint32_t name,
                           uint32_t pri) {
    uint32_t dev = have_dev ? pf_check_item((int32_t)dev_item, 1, DEVICENODE) : 0;
    if (!dev) return KERR_BADITEM;
    if (!pf_r32(dev + DEV_OPENCNT)) return KERR_NOTFOUND;   // not opened (os_code's own code)
    if (name) pf_stop(c, "CreateIOReq: a name: not yet");
    if (port) pf_stop(c, "CreateIOReq: a reply port: not yet");
    uint32_t size = pf_r32(dev + DEV_IOREQSIZE);
    uint32_t ior = pf_os_alloc(size);
    pf_w32(ior + 12, size);
    int32_t item = pf_item_new(ior, 1, IOREQNODE, nullptr);
    pf_w32(ior + 28, task_item());                          // n_Owner
    pf_w32(ior + IO_DEV, dev);
    pf_w8(ior + 10, pri);
    pf_w32(ior + IO_MSGITEM, task_item());
    pf_w32(ior + IO_SIGITEM, task_item());
    pf_w32(ior + IO_FLAGS, pf_r32(ior + IO_FLAGS) | IO_DONE | IO_QUICK);
    pf_list_add_tail(dev + DEV_IOREQS, ior + IO_LINK);
    return (uint32_t)item;
}

// swi 0x10000: Item CreateSizedItem(int32 ctype, TagArg* tags, int32 size) -- ctype is
// MKNODEID(subsystem, type): the kernel makes its own kinds, a folio's are made by the routine it
// registered (pf_on_create). The kinds the programs run so far make.
static std::map<int, PfCreateItem> g_creators;

void pf_on_create(int subsys, PfCreateItem fn) { g_creators[subsys] = fn; }

static void k_createsizeditem(ArmCpu& c) {
    int subsys = (int)(c.r[0] >> 8 & 0xFF), type = (int)(c.r[0] & 0xFF);
    auto it = g_creators.find(subsys);
    if (c.r[0] == (1u << 8 | IOREQNODE)) c.r[0] = create_ioreq(c, c.r[1]);
    else if (c.r[0] == (1u << 8 | TASKNODE)) c.r[0] = pf_create_task(c, c.r[1]);
    else if (it != g_creators.end()) c.r[0] = it->second(c, type, c.r[1]);
    else {
        char why[80];
        std::snprintf(why, sizeof why, "CreateSizedItem of %#x: not yet", c.r[0]);
        pf_stop(c, why);
    }
}

// CompleteIO (0x141c0): done; then a callback would chain the next request, a quick request
// tells no one, and otherwise the requester hears: a reply to its message, or SIGF_IODONE.
void pf_complete_io(uint32_t ior) {
    pf_w32(ior + IO_FLAGS, pf_r32(ior + IO_FLAGS) | IO_DONE);
    if (pf_r32(ior + IO_CALLBACK)) {
        std::fprintf(stderr, "CompleteIO: an IOReq with a callback: not yet\n");
        std::exit(3);
    }
    if (pf_r32(ior + IO_FLAGS) & IO_QUICK) return;
    uint32_t m = pf_item_node((int32_t)pf_r32(ior + IO_MSGITEM));
    if (m && pf_r8(m + 9) == MESSAGENODE) {
        std::fprintf(stderr, "CompleteIO: a reply message: not yet\n");
        std::exit(3);
    }
    uint32_t s = pf_item_node((int32_t)pf_r32(ior + IO_SIGITEM));
    if (s && pf_r8(s + 9) == TASKNODE) pf_signal(s, SIGF_IODONE);
    else if (m && pf_r8(m + 9) == TASKNODE) pf_signal(m, SIGF_IODONE);
}

// swi 0x10018: Err SendIO(Item ior, IOInfo* info) -- 0x142e8: the request checked (its owner,
// not in progress, the IOInfo's flags and unit, the buffers: what it receives into must be the
// task's to write, what it sends from inside memory), the IOInfo copied in, then the driver.
int32_t pf_send_io(ArmCpu& c, int32_t item, uint32_t info) {
    uint32_t task = pf_current_task();
    uint32_t ior = pf_check_item(item, 1, IOREQNODE);
    if (!ior) return (int32_t)KERR_BADITEM;
    if (pf_r32(ior + 28) != task_item()) return (int32_t)KERR_NOTOWNER;
    if (!(pf_r32(ior + IO_FLAGS) & IO_DONE)) return (int32_t)KERR_IOINPROGRESS;
    uint32_t dev = pf_r32(ior + IO_DEV);
    pf_w8(ior + 10, pf_r8(task + 10));                      // the task's priority
    for (uint32_t i = 0; i < 32; i += 4) pf_w32(ior + IO_INFO + i, pf_r32(info + i));
    if (pf_r8(ior + IOI_FLAGS2) || (pf_r8(ior + IOI_FLAGS) & ~IO_QUICK)) return (int32_t)KERR_BADIOARG;
    if (pf_r8(ior + IOI_UNIT) > pf_r8(dev + DEV_MAXUNITNUM)) return (int32_t)KERR_BADUNIT;
    pf_w32(ior + IO_CALLBACK, 0);
    uint32_t rlen = pf_r32(ior + IOI_RECV_LEN), slen = pf_r32(ior + IOI_SEND_LEN);
    if (rlen && pf_task_can_write(task, pf_r32(ior + IOI_RECV_BUF), (int32_t)rlen) < 0) return (int32_t)KERR_BADPTR;
    // the kernel's readable check (0x125c4): the range inside memory's top
    if (slen && (slen > ARM_MEM_SIZE || pf_r32(ior + IOI_SEND_BUF) + slen > ARM_MEM_SIZE)) return (int32_t)KERR_BADPTR;
    // the kernel's internal SendIO (0x142b4), then the driver's dispatch
    pf_w32(ior + IO_ERROR, 0);
    pf_w32(ior + IO_ACTUAL, 0);
    uint32_t flags = pf_r32(ior + IO_FLAGS) & ~(IO_DONE | IO_QUICK);
    if (pf_r8(ior + IOI_FLAGS) & IO_QUICK) flags |= IO_QUICK;
    pf_w32(ior + IO_FLAGS, flags);
    auto d = g_drivers.find(dev);
    if (d == g_drivers.end()) pf_stop(c, "SendIO: a device without a driver");
    // the kernel's dispatch (0x1468c): a command done at once is completed here, and SendIO is 1;
    // a driver with a dispatch of its own (the File folio's) answers SendIO itself
    int32_t r = d->second(ior);
    if (r > 0) {
        pf_complete_io(ior);
        return 1;
    }
    return r;
}

static void k_sendio(ArmCpu& c) { c.r[0] = (uint32_t)pf_send_io(c, (int32_t)c.r[0], c.r[1]); }

// swi 0x10003: Err DeleteItem(Item) -- 0x138c8 and 0x1379c: the node, else BADITEM; the task must
// own it or be it, or be privileged (0x12f7c), else NOTPRIV; then the kind's own deletion, and
// the item is gone. The kinds made so far:
// * an IOReq (0x14114): one in progress is aborted and waited for first (not yet: the run
//   stops); off its device's list.
// * a device (0x14a60): its delete hook, and when that says 0 every IOReq on the device, each
//   deleted as by its owner, and the device off the kernel's list (which the runtime does not
//   keep).
// The kernel also gives the node's memory and its name back to the OS; here the OS's memory is
// never freed. Any other kind stops the run: not yet.
static int32_t delete_as(ArmCpu& c, int32_t item, uint32_t task) {
    uint32_t n = pf_item_node(item);
    if (!n) return (int32_t)KERR_BADITEM;
    uint32_t me = pf_r32(task + 24);
    if (!(pf_r8(task + 11) & TASK_SUPER) && pf_r32(n + 28) != me && (uint32_t)item != me)
        return (int32_t)KERR_NOTPRIV;
    uint32_t kind = pf_r8(n + 8) << 8 | pf_r8(n + 9);
    if (kind == (1u << 8 | IOREQNODE)) {
        if (!(pf_r32(n + IO_FLAGS) & IO_DONE)) pf_stop(c, "DeleteItem: an IOReq in progress: not yet");
        pf_list_rem_node(n + IO_LINK);
    } else if (kind == (1u << 8 | DEVICENODE)) {
        auto h = g_delete_hooks.find(n);
        int32_t r = h == g_delete_hooks.end() ? 0 : h->second(n);
        if (r) return r;
        for (uint32_t l; (l = pf_r32(n + DEV_IOREQS + PF_LIST_HEAD)) != n + DEV_IOREQS + PF_LIST_TAIL;) {
            uint32_t ior = l - IO_LINK;
            uint32_t owner = pf_item_node((int32_t)pf_r32(ior + 28));
            delete_as(c, (int32_t)pf_r32(ior + 24), owner ? owner : pf_current_task());
        }
        g_drivers.erase(n);
        g_delete_hooks.erase(n);
    } else {
        char why[64];
        std::snprintf(why, sizeof why, "DeleteItem of a node %#x: not yet", kind);
        pf_stop(c, why);
    }
    pf_item_free(item);
    return 0;
}

int32_t pf_delete_item(ArmCpu& c, int32_t item) { return delete_as(c, item, pf_current_task()); }

static void k_deleteitem(ArmCpu& c) { c.r[0] = (uint32_t)pf_delete_item(c, (int32_t)c.r[0]); }

// ---- the Operator's devices ----------------------------------------------------------------
// The SPORT and timer drivers are not on the disc: the 1993 kernel names no device, and the
// console's ROM brings them, in its Operator (the FZ-1's, built 3 August 1993, an AIF image at the
// ROM's 0xa830 linked at 0x20000: python -m 3dokit.rom). Each is a command table under the
// kernel's own dispatch; the addresses are the Operator's.
//
// A request a driver cannot complete at once is queued with IO_QUICK cleared, so that its
// completion tells the task: the kernel's SendIO ends by jumping to the driver (0x142e4) and
// leaves io_Flags to it, and a program's WaitIO returns at once while IO_QUICK is set.
static void defer(uint32_t ior) { pf_w32(ior + IO_FLAGS, pf_r32(ior + IO_FLAGS) & ~IO_QUICK); }

// ---- SPORT ----------------------------------------------------------------------------------
// The VRAM's serial port (0x23188, commands 4 to 6; 0 to 3 are BADCOMMAND): SPORTCMD_CLONE (4)
// repeats the page at ioi_Send over ioi_Recv's length, SPORTCMD_COPY (5) copies ioi_Recv's length
// from ioi_Send, both keeping the destination's bits where the mask ioi_Offset is clear; they are
// queued (0x233ec) and done by the SPORT FIRQ at the vertical blank (0x22f80), in order.
// FLASHWRITE_CMD (6) writes the value ioi_Offset under the mask ioi_CmdOptions at once (0x2337c).
// What the hardware does with the words the driver hands it is the SDK's documentation ("The
// SPORT Device"). The driver's own range checks (BADIOARG) are not made: a request outside whole
// VRAM pages stops the run.
static std::vector<uint32_t> g_sport_waiting;   // copies and clones, for the next blank

static void sport_do(uint32_t ior) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND), page = pf_page_size(MEMTYPE_VRAM);
    uint32_t src = pf_r32(ior + IOI_SEND_BUF), dst = pf_r32(ior + IOI_RECV_BUF);
    uint32_t len = pf_r32(ior + IOI_RECV_LEN);
    uint32_t mask = cmd == 6 ? pf_r32(ior + IOI_CMDOPTIONS) : pf_r32(ior + IOI_OFFSET);
    for (uint32_t i = 0; i < len; i += 4) {
        uint32_t v = cmd == 6 ? pf_r32(ior + IOI_OFFSET) : pf_r32(src + (cmd == 4 ? i % page : i));
        pf_w32(dst + i, (v & mask) | (pf_r32(dst + i) & ~mask));
    }
}

static int32_t sport_dispatch(uint32_t ior) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND), page = pf_page_size(MEMTYPE_VRAM);
    uint32_t src = pf_r32(ior + IOI_SEND_BUF), dst = pf_r32(ior + IOI_RECV_BUF);
    uint32_t len = pf_r32(ior + IOI_RECV_LEN);
    auto in_vram = [&](uint32_t a, uint32_t n) { return a >= 0x200000u && a + n <= 0x300000u && !(a % page); };
    if (cmd < 4 || cmd > 6) {
        pf_w32(ior + IO_ERROR, KERR_BADCOMMAND);
        return 1;
    }
    if (!in_vram(dst, len) || len % page || (cmd != 6 && !in_vram(src, cmd == 4 ? page : len))) {
        std::fprintf(stderr, "SPORT: command %u from %08X to %08X, %u bytes: not whole VRAM pages\n",
                     cmd, src, dst, len);
        std::exit(3);
    }
    if (cmd == 6) {
        sport_do(ior);
        return 1;
    }
    defer(ior);
    g_sport_waiting.push_back(ior);
    return 0;
}

static void sport_vbl(uint64_t) {
    std::vector<uint32_t> now;
    now.swap(g_sport_waiting);
    for (uint32_t ior : now) {
        sport_do(ior);
        pf_complete_io(ior);
    }
}

// ---- the timer ------------------------------------------------------------------------------
// Unit 0 counts vertical blanks, unit 1 microseconds; five commands (0x25de0). On unit 0:
// TIMERCMD_DELAY (3, 0x21694) queues the request -- always, IO_QUICK cleared -- in order of
// ioi_Offset, and at each blank (0x213a8) the timer's 64-bit count goes up by 1 and every queued
// request's io_Actual by 1, a request completing once io_Actual reaches ioi_Offset (unsigned): so
// a delay of n completes at the n-th blank after it is sent, and of 0 at the next one.
// TIMERCMD_DELAYUNTIL (4, 0x217d4) first makes ioi_Offset the count less ioi_Offset -- the wrong
// way round, so a time to come waits about 2^32 blanks -- and is then a DELAY. CMD_READ (1,
// 0x21548) writes the count, high word first, to an aligned ioi_Recv of 8 bytes or more (else
// BADIOARG); CMD_WRITE (0) is BADCOMMAND. Unit 1 and CMD_STATUS (2) stop the run: not yet.
enum : uint32_t { TIMER_UNIT_VBLANK = 0, TIMERCMD_DELAY = 3, TIMERCMD_DELAYUNTIL = 4 };

static uint64_t g_vbl_count;
static std::vector<uint32_t> g_timer_waiting;   // by ioi_Offset, as the Operator's list

static int32_t timer_dispatch(uint32_t ior) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND), unit = pf_r8(ior + IOI_UNIT);
    if (cmd == 0) {
        pf_w32(ior + IO_ERROR, KERR_BADCOMMAND);
        return 1;
    }
    if (unit != TIMER_UNIT_VBLANK || cmd == 2 || cmd > 4) {
        std::fprintf(stderr, "timer: command %u on unit %u: not yet\n", cmd, unit);
        std::exit(3);
    }
    if (cmd == 1) {
        uint32_t buf = pf_r32(ior + IOI_RECV_BUF);
        if ((buf & 3) || (int32_t)pf_r32(ior + IOI_RECV_LEN) < 8) {
            pf_w32(ior + IO_ERROR, KERR_BADIOARG);
            return 1;
        }
        pf_w32(buf, (uint32_t)(g_vbl_count >> 32));
        pf_w32(buf + 4, (uint32_t)g_vbl_count);
        pf_w32(ior + IO_ACTUAL, 8);
        return 1;
    }
    if (cmd == TIMERCMD_DELAYUNTIL) pf_w32(ior + IOI_OFFSET, (uint32_t)g_vbl_count - pf_r32(ior + IOI_OFFSET));
    int32_t n = (int32_t)pf_r32(ior + IOI_OFFSET);
    size_t at = 0;
    while (at < g_timer_waiting.size() && (int32_t)pf_r32(g_timer_waiting[at] + IOI_OFFSET) <= n) ++at;
    g_timer_waiting.insert(g_timer_waiting.begin() + (long)at, ior);
    defer(ior);
    return 0;
}

static void timer_vbl(uint64_t) {
    ++g_vbl_count;
    std::vector<uint32_t> later, done;
    for (uint32_t ior : g_timer_waiting) {
        uint32_t n = pf_r32(ior + IO_ACTUAL) + 1;
        pf_w32(ior + IO_ACTUAL, n);
        (n >= pf_r32(ior + IOI_OFFSET) ? done : later).push_back(ior);
    }
    g_timer_waiting.swap(later);
    for (uint32_t ior : done) pf_complete_io(ior);
}

void pf_io_init() {
    g_drivers.clear();
    g_delete_hooks.clear();
    g_creators.clear();                         // the folios after this one register theirs
    g_sport_waiting.clear();
    g_timer_waiting.clear();
    g_vbl_count = 0;
    pf_on_swi(0x10000, k_createsizeditem);
    pf_on_swi(0x10018, k_sendio);
    pf_on_swi(0x10003, k_deleteitem);
    pf_device_new("SPORT", 0, sport_dispatch);
    pf_device_new("timer", 1, timer_dispatch);
    pf_on_vbl(sport_vbl);
    pf_on_vbl(timer_vbl);
}
