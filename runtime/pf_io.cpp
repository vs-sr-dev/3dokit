// 3dokit runtime -- devices and IO: IOReqs, SendIO and CompleteIO as the 1993 kernel (os_code
// v0.16) does them, signals as far as IO needs them, and the devices the programs run so far
// open. The kernel's functions are read in its code (the addresses are its own); a device's
// driver is the native function the device was made with.
#include "pf.h"
#include <cstdio>
#include <cstdlib>
#include <map>

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
    KERR_BADPTR = 0xD57B9009u, KERR_BADUNIT = 0xD57B900Bu,
    KERR_BADCOMMAND = 0xD57B900Cu, KERR_BADIOARG = 0xD57B900Du, KERR_IOINPROGRESS = 0xD57B900Fu,
    KERR_NOTOWNER = 0xD57B9012u, KERR_ILLEGALSIGNAL = 0xD57B9116u,
};

static std::map<uint32_t, PfDispatchIO> g_drivers;     // device node -> its driver

static uint32_t task_item() { return pf_r32(pf_current_task() + 24); }

// ---- devices --------------------------------------------------------------------------------
// A device as CreateDevice (0x14974) leaves one: its IOReq size (0x70 when none is given), its
// list of IOReqs, its units; and, here, its driver.
uint32_t pf_device_new(const char* name, int max_unit, PfDispatchIO dispatch) {
    uint32_t dev = pf_os_alloc(DEV_SIZE);
    pf_w32(dev + 12, DEV_SIZE);
    pf_item_new(dev, 1, DEVICENODE, name);
    pf_w32(dev + DEV_IOREQSIZE, IOREQ_SIZE);
    pf_list_init(dev + DEV_IOREQS, "Device ioreqs");
    pf_w8(dev + DEV_MAXUNITNUM, (uint32_t)max_unit);
    g_drivers[dev] = dispatch;
    return dev;
}

// ---- IOReqs ---------------------------------------------------------------------------------
// CreateSizedItem of an IOReq (0x13c88): tags TAG_ITEM_NAME (1), TAG_ITEM_PRI (2),
// CREATEIOREQ_TAG_REPLYPORT (10), CREATEIOREQ_TAG_DEVICE (11, required). The device must be
// open (by the task: one task here); the node is the device's IOReq size, on the device's list,
// born done. Without a reply port both io_MsgItem and io_SigItem are the task, so completion
// signals it.
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
static void k_sendio(ArmCpu& c) {
    uint32_t task = pf_current_task();
    uint32_t ior = pf_check_item((int32_t)c.r[0], 1, IOREQNODE);
    if (!ior) { c.r[0] = KERR_BADITEM; return; }
    if (pf_r32(ior + 28) != task_item()) { c.r[0] = KERR_NOTOWNER; return; }
    if (!(pf_r32(ior + IO_FLAGS) & IO_DONE)) { c.r[0] = KERR_IOINPROGRESS; return; }
    uint32_t dev = pf_r32(ior + IO_DEV);
    pf_w8(ior + 10, pf_r8(task + 10));                      // the task's priority
    for (uint32_t i = 0; i < 32; i += 4) pf_w32(ior + IO_INFO + i, pf_r32(c.r[1] + i));
    if (pf_r8(ior + IOI_FLAGS2) || (pf_r8(ior + IOI_FLAGS) & ~IO_QUICK)) { c.r[0] = KERR_BADIOARG; return; }
    if (pf_r8(ior + IOI_UNIT) > pf_r8(dev + DEV_MAXUNITNUM)) { c.r[0] = KERR_BADUNIT; return; }
    pf_w32(ior + IO_CALLBACK, 0);
    uint32_t rlen = pf_r32(ior + IOI_RECV_LEN), slen = pf_r32(ior + IOI_SEND_LEN);
    if (rlen && pf_task_can_write(task, pf_r32(ior + IOI_RECV_BUF), (int32_t)rlen) < 0) { c.r[0] = KERR_BADPTR; return; }
    // the kernel's readable check (0x125c4): the range inside memory's top
    if (slen && (slen > ARM_MEM_SIZE || pf_r32(ior + IOI_SEND_BUF) + slen > ARM_MEM_SIZE)) { c.r[0] = KERR_BADPTR; return; }
    // the kernel's internal SendIO (0x142b4), then the driver's dispatch
    pf_w32(ior + IO_ERROR, 0);
    pf_w32(ior + IO_ACTUAL, 0);
    uint32_t flags = pf_r32(ior + IO_FLAGS) & ~(IO_DONE | IO_QUICK);
    if (pf_r8(ior + IOI_FLAGS) & IO_QUICK) flags |= IO_QUICK;
    pf_w32(ior + IO_FLAGS, flags);
    auto d = g_drivers.find(dev);
    if (d == g_drivers.end()) pf_stop(c, "SendIO: a device without a driver");
    d->second(ior);
    c.r[0] = 0;
}

// ---- SPORT ----------------------------------------------------------------------------------
// The VRAM's serial port: copies and clones of whole VRAM pages under a mask, and a fill. Its
// driver is not on the disc (the 1993 kernel names no device; the console's ROM brings it), so
// this is the SDK's documentation of it ("The SPORT Device"): SPORTCMD_CLONE (4) repeats the
// page at ioi_Send over ioi_Recv's length, SPORTCMD_COPY (5) copies ioi_Recv's length from
// ioi_Send, both keeping the destination's bits where the mask ioi_Offset is clear;
// FLASHWRITE_CMD (6) writes the value ioi_Offset under the mask ioi_CmdOptions. On the console
// a copy or clone waits for the vertical blank; here it is done at once.
static void sport_dispatch(uint32_t ior) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND), page = pf_page_size(MEMTYPE_VRAM);
    uint32_t src = pf_r32(ior + IOI_SEND_BUF), dst = pf_r32(ior + IOI_RECV_BUF);
    uint32_t len = pf_r32(ior + IOI_RECV_LEN);
    auto in_vram = [&](uint32_t a, uint32_t n) { return a >= 0x200000u && a + n <= 0x300000u && !(a % page); };
    if (cmd < 4 || cmd > 6) {
        pf_w32(ior + IO_ERROR, KERR_BADCOMMAND);
    } else if (!in_vram(dst, len) || len % page || (cmd != 6 && !in_vram(src, cmd == 4 ? page : len))) {
        std::fprintf(stderr, "SPORT: command %u from %08X to %08X, %u bytes: not whole VRAM pages\n",
                     cmd, src, dst, len);
        std::exit(3);
    } else {
        uint32_t mask = cmd == 6 ? pf_r32(ior + IOI_CMDOPTIONS) : pf_r32(ior + IOI_OFFSET);
        for (uint32_t i = 0; i < len; i += 4) {
            uint32_t v = cmd == 6 ? pf_r32(ior + IOI_OFFSET) : pf_r32(src + (cmd == 4 ? i % page : i));
            pf_w32(dst + i, (v & mask) | (pf_r32(dst + i) & ~mask));
        }
    }
    pf_complete_io(ior);
}

void pf_io_init() {
    g_drivers.clear();
    g_creators.clear();                         // the folios after this one register theirs
    pf_on_swi(0x10000, k_createsizeditem);
    pf_on_swi(0x10018, k_sendio);
    pf_device_new("SPORT", 0, sport_dispatch);
}
