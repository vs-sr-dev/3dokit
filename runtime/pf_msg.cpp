// 3dokit runtime -- the kernel's messages, as the 1993 kernel (os_code v0.16) makes and passes
// them: MsgPort items (0x18418) and Msg items (0x1898c), SendMsg (SWI 16, 0x184d0), ReplyMsg (SWI
// 18, 0x186b8), GetMsg (SWI 19, 0x18bd4), GetThisMsg (SWI 15, 0x18b34), the send underneath the
// two (0x185fc), and the deletion of both kinds (0x1884c, 0x187f8). WaitPort is not the kernel's
// in 1993 (its SWI slot is empty): a program's library waits on the port's signal itself.
//
// A port may also be the OS's own (pf_msgport_new): then a message sent to it goes to a native
// function, as it would wake the task that owns the port -- the runtime's event broker is one.
#include "pf.h"
#include <map>

enum : uint32_t {
    KERNELNODE = 1, TASKNODE = 5, MESSAGENODE = 9, MSGPORTNODE = 10,
    // MsgPort (msgport.h): 0x50 bytes, the kernel's node table says
    MP_SIGNAL = 0x24, MP_MSGS = 0x28, MP_USERDATA = 0x48, MP_SIZE = 0x50,
    MSGPORT_SIGNAL_ALLOCATED = 1,
    // Message (msgport.h): 0x40 bytes and a pass-by-value buffer after them. +0x34 is the item
    // that holds the message: the port it is queued on, else the task that has it.
    MSG_REPLYPORT = 0x24, MSG_RESULT = 0x28, MSG_DATAPTR = 0x2c, MSG_DATASIZE = 0x30,
    MSG_HOLDER = 0x34, MSG_DATAPTRSIZE = 0x38, MSG_SIGITEM = 0x3c, MSG_SIZE = 0x40,
    MESSAGE_SENT = 1, MESSAGE_REPLIED = 2, MESSAGE_SMALL = 4, MESSAGE_PASS_BY_VALUE = 8,
    // n_Flags the kernel's node table gives both kinds: NODE_NAMEVALID, NODE_SIZELOCKED,
    // NODE_ITEMVALID
    NODE_FLAGS = 0xb0,
    // the tags (types.h, msgport.h)
    TAG_ITEM_NAME = 1, TAG_ITEM_PRI = 2, TAG_ITEM_VERSION = 3, TAG_ITEM_REVISION = 4,
    TAG_JUMP = 0xfe, TAG_NOP = 0xff,
    CREATEPORT_TAG_SIGNAL = 10, CREATEPORT_TAG_USERDATA = 11,
    CREATEMSG_TAG_REPLYPORT = 10, CREATEMSG_TAG_MSG_IS_SMALL = 11, CREATEMSG_TAG_DATA_SIZE = 12,
    TASK_SUPER = 8,
};

// The kernel's errors (operror.h; the values os_code builds).
enum : uint32_t {
    KERR_BADITEM = 0xD57B9001u, KERR_BADTAG = 0xD57B9002u, KERR_BADTAGVAL = 0xD57B9003u,
    KERR_NOTPRIV = 0xD57B9004u, KERR_NOMEM = 0xD57B9006u, KERR_BADPTR = 0xD57B9009u,
    KERR_BADNAME = 0xD57B900Eu, KERR_NOTOWNER = 0xD57B9012u, KERR_NOSIGS = 0xD57B9108u,
    KERR_MSGSENT = 0xD57B9109u, KERR_NOREPLYPORT = 0xD57B910Au, KERR_BADSIZE = 0xD57B910Bu,
    KERR_REPLYPORTNEEDED = 0xD57B9115u, KERR_ILLEGALSIGNAL = 0xD57B9116u,
};

static std::map<int32_t, PfPortFn> g_native;    // the OS's own ports, by item

static uint32_t task_item() { return pf_r32(pf_current_task() + 24); }
static uint32_t flags(uint32_t n) { return pf_r8(n + 11); }
static void set_flags(uint32_t n, uint32_t v) { pf_w8(n + 11, v); }

// The kernel's tag walk (0x1acf4) over a new node: TAG_ITEM_NAME (a copy in the OS's memory; NULL
// is BADNAME), _PRI, _VERSION, _REVISION, TAG_JUMP (more than 20, or back to the start, is
// BADPTR), TAG_NOP; any other tag goes to the kind's own `other`, whose Err stops the walk. On an
// Err the name is given back.
template <class Other>
static int32_t walk_tags(uint32_t node, uint32_t tags, Other other) {
    int32_t r = 0;
    int jumps = 0;
    for (uint32_t t = tags; t;) {
        uint32_t tag = pf_r32(t), v = pf_r32(t + 4);
        t += 8;
        if (tag == 0) break;
        if (tag == TAG_ITEM_NAME) {
            if (!v) return (int32_t)KERR_BADNAME;
            char s[256];
            pf_cstring(v, s, sizeof s);
            pf_w32(node + 16, pf_os_string(s));
        } else if (tag == TAG_ITEM_PRI) pf_w8(node + 10, v);
        else if (tag == TAG_ITEM_VERSION) pf_w8(node + 0x14, v);
        else if (tag == TAG_ITEM_REVISION) pf_w8(node + 0x15, v);
        else if (tag == TAG_JUMP) {
            if (++jumps > 20 || v == tags) { r = (int32_t)KERR_BADPTR; break; }
            t = v;
        } else if (tag != TAG_NOP) {
            r = other(tag, v);
            if (r < 0) break;
        }
    }
    if (r < 0) pf_w32(node + 16, 0);
    return r;
}

// A node of a kernel kind with its item, as the kernel's AllocNode (0x11de4) leaves one.
static uint32_t new_node(int type, uint32_t size, int32_t* item) {
    uint32_t n = pf_os_alloc(size);
    pf_w32(n + 12, size);
    *item = pf_item_new(n, KERNELNODE, type, nullptr);
    set_flags(n, NODE_FLAGS);
    return n;
}

// ---- making them -------------------------------------------------------------------------------
// CreateSizedItem of a MsgPort (0x18418): tags CREATEPORT_TAG_SIGNAL (taken as given) and
// _USERDATA. Without a signal the kernel allocates one of the creator's and marks the port
// MSGPORT_SIGNAL_ALLOCATED; none left is NOSIGS. The kernel's node table locks the size, so a size
// given to CreateSizedItem is NOMEM (0x11de4). The kernel also puts the port on KernelBase's list
// of ports, which the runtime does not keep.
uint32_t pf_create_msgport(ArmCpu& c, uint32_t tags, uint32_t size) {
    (void)c;
    if (size) return KERR_NOMEM;
    int32_t item;
    uint32_t p = new_node(MSGPORTNODE, MP_SIZE, &item);
    int32_t r = walk_tags(p, tags, [&](uint32_t tag, uint32_t v) -> int32_t {
        if (tag == CREATEPORT_TAG_SIGNAL) pf_w32(p + MP_SIGNAL, v);
        else if (tag == CREATEPORT_TAG_USERDATA) pf_w32(p + MP_USERDATA, v);
        else return (int32_t)KERR_BADTAG;
        return 0;
    });
    if (r >= 0 && !pf_r32(p + MP_SIGNAL)) {
        set_flags(p, flags(p) | MSGPORT_SIGNAL_ALLOCATED);
        pf_w32(p + MP_SIGNAL, pf_alloc_signal(0));
        if (!pf_r32(p + MP_SIGNAL)) r = (int32_t)KERR_NOSIGS;
    }
    if (r < 0) {
        pf_item_free(item);
        return (uint32_t)r;
    }
    pf_list_init(p + MP_MSGS, nullptr);
    pf_w32(p + 28, task_item());                            // n_Owner, as CreateSizedItem sets it
    return (uint32_t)item;
}

// CreateSizedItem of a Semaphore (0x13960): the standard tags only (its own callback, 0x1394c,
// is BADTAG for any other), the waiters' list (+0x30, "Semaphore WaitQ"), no owner (+0x28 -1),
// as pf_semaphore_new makes the OS's own. A size is NOMEM, as for a port. The kernel also puts it
// on KernelBase's list of semaphores, which the runtime does not keep.
uint32_t pf_create_semaphore(ArmCpu& c, uint32_t tags, uint32_t size) {
    (void)c;
    if (size) return KERR_NOMEM;
    int32_t item;
    uint32_t s = new_node(7, 0x50, &item);
    int32_t r = walk_tags(s, tags, [](uint32_t, uint32_t) -> int32_t { return (int32_t)KERR_BADTAG; });
    if (r < 0) {
        pf_item_free(item);
        return (uint32_t)r;
    }
    pf_list_init(s + 0x30, "Semaphore WaitQ");
    pf_w32(s + 0x28, 0xFFFFFFFFu);
    pf_w32(s + 28, task_item());                            // n_Owner, as CreateSizedItem sets it
    return (uint32_t)item;
}

// CreateSizedItem of a Message (0x1898c): the kernel's node table gives a message no size, so
// CreateSizedItem hands its creation routine no node (-1), and a size given to CreateSizedItem is
// BADSIZE. The node is 0x40 bytes, and CREATEMSG_TAG_DATA_SIZE's bytes more for a pass-by-value
// buffer (msg_DataPtr at it, msg_DataPtrSize its size); _MSG_IS_SMALL makes a small message, and
// both together are BADTAGVAL. CREATEMSG_TAG_REPLYPORT is required (REPLYPORTNEEDED), a port the
// creator owns (BADITEM, NOTOWNER). The new message is held by its creator.
//
// (With no node, CreateSizedItem's own marking of the node -- n_ItemFlags |= ITEMNODE_NOTREADY,
// at -1 + 0x17 -- lands on the byte at 0x16 of the program's image, where its AIF header's
// image_ro_size is: the kernel's, not modelled.)
uint32_t pf_create_msg(ArmCpu& c, uint32_t tags, uint32_t size) {
    (void)c;
    if (size) return KERR_BADSIZE;
    uint32_t data = 0;
    for (uint32_t t = tags; t;) {                           // the kernel's tag search (0x1ae60)
        uint32_t tag = pf_r32(t), v = pf_r32(t + 4);
        t += 8;
        if (tag == 0) break;
        if (tag == TAG_JUMP) t = v;
        else if (tag == CREATEMSG_TAG_DATA_SIZE) { data = v; break; }
    }
    int32_t item;
    uint32_t m = new_node(MESSAGENODE, MSG_SIZE + data, &item);
    int32_t r = walk_tags(m, tags, [&](uint32_t tag, uint32_t v) -> int32_t {
        if (tag == CREATEMSG_TAG_REPLYPORT) pf_w32(m + MSG_REPLYPORT, v);
        else if (tag == CREATEMSG_TAG_MSG_IS_SMALL) set_flags(m, flags(m) | MESSAGE_SMALL);
        else if (tag == CREATEMSG_TAG_DATA_SIZE) {
            pf_w32(m + MSG_DATAPTRSIZE, v);
            pf_w32(m + MSG_DATAPTR, m + MSG_SIZE);
            set_flags(m, flags(m) | MESSAGE_PASS_BY_VALUE);
        } else return (int32_t)KERR_BADTAG;
        return 0;
    });
    if (r >= 0 && (flags(m) & (MESSAGE_SMALL | MESSAGE_PASS_BY_VALUE)) == (MESSAGE_SMALL | MESSAGE_PASS_BY_VALUE))
        r = (int32_t)KERR_BADTAGVAL;
    if (r >= 0) {
        uint32_t port = pf_r32(m + MSG_REPLYPORT);
        uint32_t p = port ? pf_check_item((int32_t)port, KERNELNODE, MSGPORTNODE) : 0;
        if (!port) r = (int32_t)KERR_REPLYPORTNEEDED;
        else if (!p) r = (int32_t)KERR_BADITEM;
        else if (pf_r32(p + 28) != task_item()) r = (int32_t)KERR_NOTOWNER;
    }
    if (r < 0) {
        pf_w32(m + 16, 0);
        pf_item_free(item);
        return (uint32_t)r;
    }
    pf_w32(m + MSG_HOLDER, task_item());
    pf_w32(m + 28, task_item());
    return (uint32_t)item;
}

// ---- passing them ------------------------------------------------------------------------------
// The kernel's SendSignal (0x19d40), as the send calls it for a port's owner: item 0 is the
// current task; the system's signals only from a privileged task, bit 31 never; the errors are not
// the send's.
static void signal_task(uint32_t item, uint32_t bits) {
    uint32_t t = item ? pf_check_item((int32_t)item, KERNELNODE, TASKNODE) : pf_current_task();
    if (!t) return;
    if ((bits & 0xff) && !(pf_r8(pf_current_task() + 11) & TASK_SUPER)) return;
    if (bits & 0x80000000u) return;
    pf_signal(t, bits);
}

// The send under SendMsg and ReplyMsg (0x185fc): a message already on a port is MSGSENT. The data
// and its size go in -- a pass-by-value message copies the data into its buffer -- the message on
// the port's list by priority, sent, held by the port; and the port's owner is signalled with the
// port's signal -- or msg_SigItem, when set, with it. A port of the OS's own hears it instead.
static int32_t send(uint32_t port, uint32_t msg, uint32_t data, uint32_t size) {
    if (flags(msg) & MESSAGE_SENT) return (int32_t)KERR_MSGSENT;
    pf_w32(msg + MSG_DATASIZE, size);
    if (!(flags(msg) & MESSAGE_PASS_BY_VALUE)) pf_w32(msg + MSG_DATAPTR, data);
    else {
        uint32_t buf = pf_r32(msg + MSG_DATAPTR);
        if (buf != data)
            for (uint32_t i = 0; i < size; ++i) pf_w8(buf + i, pf_r8(data + i));
    }
    pf_list_insert_from_tail(port + MP_MSGS, msg);
    set_flags(msg, flags(msg) | MESSAGE_SENT);
    int32_t pitem = (int32_t)pf_r32(port + 24);
    pf_w32(msg + MSG_HOLDER, (uint32_t)pitem);
    auto native = g_native.find(pitem);
    if (native != g_native.end()) native->second(pitem, (int32_t)pf_r32(msg + 24));
    else if (!pf_r32(msg + MSG_SIGITEM)) signal_task(pf_r32(port + 28), pf_r32(port + MP_SIGNAL));
    else signal_task(pf_r32(msg + MSG_SIGITEM), pf_r32(port + MP_SIGNAL));
    return 0;
}

// The kernel's readable check (0x125c4), as SendIO makes it too.
static bool readable(uint32_t p, uint32_t n) { return n <= ARM_MEM_SIZE && p + n <= ARM_MEM_SIZE; }

// swi 0x10010: Err SendMsg(Item port, Item msg, void* data, int32 size) -- 0x184d0: a port and a
// message (BADITEM) that the caller holds (NOTOWNER); msg_Result cleared; unless the message is
// small, the data must be readable (BADPTR), and for a pass-by-value message fit its buffer
// (BADSIZE). (SendSmallMsg is the same SWI, with two values for the data and its size.)
static void k_sendmsg(ArmCpu& c) {
    uint32_t port = pf_check_item((int32_t)c.r[0], KERNELNODE, MSGPORTNODE);
    uint32_t msg = pf_check_item((int32_t)c.r[1], KERNELNODE, MESSAGENODE);
    uint32_t data = c.r[2], size = c.r[3];
    if (!port || !msg) { c.r[0] = KERR_BADITEM; return; }
    if (pf_r32(msg + MSG_HOLDER) != task_item()) { c.r[0] = KERR_NOTOWNER; return; }
    pf_w32(msg + MSG_RESULT, 0);
    if (!(flags(msg) & MESSAGE_SMALL) && !readable(data, size)) { c.r[0] = KERR_BADPTR; return; }
    if ((flags(msg) & MESSAGE_PASS_BY_VALUE) && size > pf_r32(msg + MSG_DATAPTRSIZE)) { c.r[0] = KERR_BADSIZE; return; }
    c.r[0] = (uint32_t)send(port, msg, data, size);
}

// swi 0x10012: Err ReplyMsg(Item msg, int32 result, void* data, int32 size) -- 0x186b8: a message
// (BADITEM) whose reply port is still an item (NOREPLYPORT), not on a port (MSGSENT); replied,
// msg_Result set; the data checked as SendMsg's; then sent to the reply port. Who holds it is not
// asked. (ReplySmallMsg is the same SWI.)
int32_t pf_reply_msg(int32_t item, int32_t result, uint32_t data, uint32_t size) {
    uint32_t msg = pf_check_item(item, KERNELNODE, MESSAGENODE);
    if (!msg) return (int32_t)KERR_BADITEM;
    uint32_t port = pf_item_node((int32_t)pf_r32(msg + MSG_REPLYPORT));
    if (!port) return (int32_t)KERR_NOREPLYPORT;
    if (flags(msg) & (MESSAGE_SENT | MESSAGE_REPLIED)) return (int32_t)KERR_MSGSENT;
    set_flags(msg, flags(msg) | MESSAGE_REPLIED);
    pf_w32(msg + MSG_RESULT, (uint32_t)result);
    if (!(flags(msg) & MESSAGE_SMALL) && !readable(data, size)) return (int32_t)KERR_BADPTR;
    if ((flags(msg) & MESSAGE_PASS_BY_VALUE) && size > pf_r32(msg + MSG_DATAPTRSIZE)) return (int32_t)KERR_BADSIZE;
    return send(port, msg, data, size);
}

static void k_replymsg(ArmCpu& c) {
    c.r[0] = (uint32_t)pf_reply_msg((int32_t)c.r[0], (int32_t)c.r[1], c.r[2], c.r[3]);
}

// The message off its port, neither sent nor replied any more, held by `who`.
static void take(uint32_t msg, uint32_t who) {
    pf_list_rem_node(msg);
    set_flags(msg, flags(msg) & ~(MESSAGE_SENT | MESSAGE_REPLIED));
    pf_w32(msg + MSG_HOLDER, who);
}

// swi 0x10013: Item GetMsg(Item port) -- 0x18bd4: a port (BADITEM) the caller owns (NOTOWNER); the
// first message on it, taken, or 0 when there is none. A port of the OS's own is the OS's to read,
// and what it takes it holds itself (holder 0).
int32_t pf_get_msg(int32_t item) {
    uint32_t port = pf_check_item(item, KERNELNODE, MSGPORTNODE);
    if (!port) return (int32_t)KERR_BADITEM;
    bool native = g_native.count(item) != 0;
    if (!native && pf_r32(port + 28) != task_item()) return (int32_t)KERR_NOTOWNER;
    uint32_t first = pf_r32(port + MP_MSGS + PF_LIST_HEAD);
    if (first == port + MP_MSGS + PF_LIST_TAIL) return 0;
    take(first, native ? 0 : task_item());
    return (int32_t)pf_r32(first + 24);
}

static void k_getmsg(ArmCpu& c) {
    uint32_t port = pf_check_item((int32_t)c.r[0], KERNELNODE, MSGPORTNODE);
    if (port && g_native.count((int32_t)c.r[0])) pf_stop(c, "GetMsg on a port of the OS's own");
    c.r[0] = (uint32_t)pf_get_msg((int32_t)c.r[0]);
}

// swi 0x1000f: Item GetThisMsg(Item msg) -- 0x18b34: a message (BADITEM) held by the caller or by
// a port it owns (else NOTPRIV); taken if it is on a port. The message back.
static void k_getthismsg(ArmCpu& c) {
    uint32_t msg = pf_check_item((int32_t)c.r[0], KERNELNODE, MESSAGENODE);
    if (!msg) { c.r[0] = KERR_BADITEM; return; }
    uint32_t holder = pf_r32(msg + MSG_HOLDER), me = task_item();
    uint32_t port = pf_check_item((int32_t)holder, KERNELNODE, MSGPORTNODE);
    if (holder != me && !(port && pf_r32(port + 28) == me)) { c.r[0] = KERR_NOTPRIV; return; }
    if (flags(msg) & (MESSAGE_SENT | MESSAGE_REPLIED)) take(msg, me);
}

// Kernel -96: Item WaitPort(Item port, Item msg) -- 23.10's (0x87a0, user-mode code over the
// SWIs; the 1993 kernel has none): a port (BADITEM), and when msg is above 0 a message (BADITEM).
// While the port has messages: with no msg, GetMsg; with one held by the port, GetThisMsg -- else
// the port's signal waited for, and back to the test; a wait that brings SIGF_ABORT (or an error
// with that bit) is -1.
static void k_waitport(ArmCpu& c) {
    int32_t pi = (int32_t)c.r[0], mi = (int32_t)c.r[1];
    uint32_t port = pf_check_item(pi, KERNELNODE, MSGPORTNODE);
    if (!port) { c.r[0] = KERR_BADITEM; return; }
    uint32_t msg = 0;
    if (mi > 0 && !(msg = pf_check_item(mi, KERNELNODE, MESSAGENODE))) { c.r[0] = KERR_BADITEM; return; }
    for (;;) {
        if (pf_r32(port + MP_MSGS + PF_LIST_HEAD) != port + MP_MSGS + PF_LIST_TAIL) {
            if (!msg) {
                c.r[0] = (uint32_t)pf_get_msg(pi);
                return;
            }
            if (pf_r32(msg + MSG_HOLDER) == (uint32_t)pi) {
                c.r[0] = (uint32_t)mi;
                k_getthismsg(c);
                return;
            }
        }
        if ((uint32_t)pf_wait_signal(pf_r32(port + MP_SIGNAL)) & 4u) {   // SIGF_ABORT
            c.r[0] = 0xFFFFFFFFu;
            return;
        }
    }
}

// ---- deleting them -----------------------------------------------------------------------------
// A message (0x187f8): off its port if it is on one; its name given back.
void pf_delete_msg(uint32_t msg) {
    if (flags(msg) & MESSAGE_SENT) pf_list_rem_node(msg);
    pf_w32(msg + 16, 0);
}

// A port (0x1884c): each message on it taken off -- a reply simply dropped, a message sent to it
// replied to with BADITEM; then the port's signal freed from its owner, whether or not the kernel
// allocated it. (Neither sent nor replied cannot be on a port: the kernel stops there.)
void pf_delete_msgport(ArmCpu& c, uint32_t port) {
    for (uint32_t m; (m = pf_r32(port + MP_MSGS + PF_LIST_HEAD)) != port + MP_MSGS + PF_LIST_TAIL;) {
        pf_list_rem_node(m);
        uint32_t f = flags(m);
        set_flags(m, f & ~(MESSAGE_SENT | MESSAGE_REPLIED));
        if (f & MESSAGE_REPLIED) continue;
        if (!(f & MESSAGE_SENT)) pf_stop(c, "DeleteItem of a port: a message on it neither sent nor replied");
        pf_reply_msg((int32_t)pf_r32(m + 24), (int32_t)KERR_BADITEM, 0, 0);
    }
    // the kernel's FreeSignal for a task (0x19c2c): not the system's bits or bit 31, allocated ones
    uint32_t owner = pf_item_node((int32_t)pf_r32(port + 28));
    if (owner && pf_r8(owner + 9) == TASKNODE) {
        uint32_t sig = pf_r32(port + MP_SIGNAL), have = pf_r32(owner + T_ALLOCATEDSIGS);
        if (!(sig & 0x800000FFu) && !(sig & ~have)) pf_w32(owner + T_ALLOCATEDSIGS, have & ~sig);
    }
    g_native.erase((int32_t)pf_r32(port + 24));
    pf_w32(port + 16, 0);
}

// ---- the OS's own ------------------------------------------------------------------------------
int32_t pf_msgport_new(const char* name, PfPortFn on_msg) {
    int32_t item;
    uint32_t p = new_node(MSGPORTNODE, MP_SIZE, &item);
    pf_w32(p + 16, pf_os_string(name));
    pf_list_init(p + MP_MSGS, nullptr);
    g_native[item] = on_msg;
    return item;
}

int32_t pf_msg_new(int32_t reply_port, uint32_t data_size) {
    int32_t item;
    uint32_t m = new_node(MESSAGENODE, MSG_SIZE + data_size, &item);
    pf_w32(m + MSG_REPLYPORT, (uint32_t)reply_port);
    if (data_size) {
        pf_w32(m + MSG_DATAPTRSIZE, data_size);
        pf_w32(m + MSG_DATAPTR, m + MSG_SIZE);
        set_flags(m, flags(m) | MESSAGE_PASS_BY_VALUE);
    }
    return item;
}

int32_t pf_send_msg(int32_t port, int32_t msg, uint32_t data, uint32_t size) {
    uint32_t p = pf_check_item(port, KERNELNODE, MSGPORTNODE), m = pf_check_item(msg, KERNELNODE, MESSAGENODE);
    if (!p || !m) return (int32_t)KERR_BADITEM;
    pf_w32(m + MSG_RESULT, 0);
    return send(p, m, data, size);
}

void pf_msg_init() {
    g_native.clear();
    pf_on_swi(0x1000f, k_getthismsg);
    pf_on_slot(PF_KERNEL, -96, k_waitport);
    pf_on_swi(0x10010, k_sendmsg);
    pf_on_swi(0x10012, k_replymsg);
    pf_on_swi(0x10013, k_getmsg);
}
