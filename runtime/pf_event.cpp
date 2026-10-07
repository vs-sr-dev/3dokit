// 3dokit runtime -- the event broker, at its message boundary: what the disc's
// System/Tasks/eventbroker ("Event Broker as of Sat Aug 14 1993", started by startopera; a
// compressed AIF image, read unpacked with python -m 3dokit.aif --decompress) does with the
// messages its listeners send it and the ones it sends them. The addresses are that image's.
//
// The broker reads the Control Port through a device ("controlport", CONTROLPORTCMD_READWRITE once
// a field) whose driver is on neither the disc nor in the ROM's Operator; the pads it decodes from
// the port's bits are here simply the runtime's pad: the first Control Pad on the port (pod 1,
// position 1, the first pad of its kind), whose buttons pf_pad_press schedules and pf_pad_live
// adds from the host (pfboot's window), read once a field at the vertical blank. So what is modelled
// is the broker above its driverlets: its port, its listeners, the focus, the event records.
//
// * Its port, "eventbroker" (0x650); the broker task runs at priority 199 (0x5e8), above any
//   program, so it answers a message as soon as it is sent, and reports a field's events as soon
//   as the field's data is in: here, at once and at the vertical blank.
// * A request (0xe6c) is a message with a reply port and an EventBrokerHeader (event.h) at a
//   word-aligned msg_DataPtr, at least 4 bytes; anything else is replied to with the broker's
//   BADPTR, and a message without a reply port is dropped. EB_Configure is the one modelled: the
//   other flavours (GetListeners, SetFocus, GetFocus, the pods') stop the run, not yet.
// * EB_Configure (0xff4): the listener of the message's reply port, made on its first configure
//   (0x1458: at the head of the list, its queue at most 3 messages in transit), takes the
//   ConfigurationRequest's category, trigger and capture masks; on the first configure a focus
//   listener (LC_FocusListener, LC_FocusUI) takes the focus from whoever has it and moves to the
//   head of the list. The reply is the result 0 and, for a pass-by-value message, an
//   EB_ConfigureReply header (otherwise no data).
// * Each field (0x23b0 on): a listener hears when the focus changed for it, or, for the one with
//   the focus and for observers, when its trigger mask meets an event of the field; the message
//   is an EB_EventRecord: the header, the frames (an EventQueueOverflow first when its queue had
//   filled, GivingFocus or LosingFocus when it asked for them, then the pads'), and an empty frame
//   to end them. With its queue full nothing is sent and the overflow is remembered.
// * The pad (its driverlet, 0x2ad8): each field the buttons that went down, came up, and the
//   state, ControlButtonPressed, Released, Update when they changed, and Arrived always; a
//   listener gets the frames of those in its trigger or capture mask (word 0), and an Update
//   after an overflow whatever its mask, to catch up. A frame (0x14fc) is ef_ByteCount 0x20, the
//   field as ef_SystemTimeStamp, the event, the pod's number, position and generic number, and the
//   button bits; ef_Trigger is left 0.
// * The messages are the broker's own, pass-by-value, with its port as their reply port (0x1570):
//   one that comes back is free again (0xdb4) and the listener has one fewer in transit; a new one
//   is made only when no free one is big enough (sizes in 16s).
#include "pf.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

enum : uint32_t {
    // event.h
    EB_CONFIGURE = 1, EB_CONFIGUREREPLY = 2, EB_EVENTRECORD = 3,
    LC_NOSEEUM = 0, LC_FOCUSLISTENER = 1, LC_OBSERVER = 2, LC_FOCUSUI = 3,
    EVENTNUM_CONTROLBUTTONPRESSED = 1, EVENTNUM_CONTROLBUTTONRELEASED = 2,
    EVENTNUM_CONTROLBUTTONUPDATE = 3, EVENTNUM_CONTROLBUTTONARRIVED = 4,
    EVENTNUM_GIVINGFOCUS = 19, EVENTNUM_LOSINGFOCUS = 20, EVENTNUM_EVENTQUEUEOVERFLOW = 72,
    EVENTBIT0_GIVINGFOCUS = 0x2000, EVENTBIT0_LOSINGFOCUS = 0x1000,
    EVENT_QUEUE_DEFAULT = 3,
    // a ConfigurationRequest
    CR_CATEGORY = 4, CR_TRIGGERMASK = 8, CR_CAPTUREMASK = 0x28, CR_QUEUEMAX = 0x48,
    // an EventFrame, a pad's: 0x1c bytes and the button bits
    EF_BYTECOUNT = 0, EF_SYSTEMTIMESTAMP = 8, EF_EVENTNUMBER = 0x10, EF_PODNUMBER = 0x11,
    EF_PODPOSITION = 0x12, EF_GENERICPOSITION = 0x13, EF_EVENTDATA = 0x1c, EF_HEADER = 0x1c,
    // msgport.h
    MSG_REPLYPORT = 0x24, MSG_DATAPTR = 0x2c, MSG_DATASIZE = 0x30, MSG_DATAPTRSIZE = 0x38,
    MESSAGE_PASS_BY_VALUE = 8,
    // the runtime's pad: the first pod on the port, the first pad
    PAD_POD = 1, PAD_POSITION = 1, PAD_GENERIC = 1,
};

// The broker's errors: MakeErr(ER_FOLI, 'E''B'... as the image builds them (0xf14cd000 + n).
enum : uint32_t { EB_ERR_NOMEM = 0xF14CD008u, EB_ERR_BADPTR = 0xF14CD009u };

struct Listener {
    int32_t port;                   // +0x18
    uint32_t category;              // +0x1c
    uint32_t trigger[8], capture[8];
    uint32_t queue_max = EVENT_QUEUE_DEFAULT, in_transit = 0;   // +0x60, +0x61
    bool focus = false, last_focus = false;                     // +0x62, +0x63
    bool overflow = false;                                      // +0x64
};

static int32_t g_port;
static std::vector<Listener*> g_listeners;          // the broker's list, head first
static Listener* g_focus;
static std::vector<int32_t> g_free;                 // messages back, head first
static std::map<int32_t, Listener*> g_pending;      // messages out, and to whom

// The pad: what pf_pad_press scheduled, and its buttons at the last field.
struct Press { uint32_t bits; uint64_t first; int count, every, hold; };
static std::vector<Press> g_presses;
static uint32_t g_pad;

void pf_pad_press(uint32_t bits, uint64_t first, int count, int every, int hold) {
    g_presses.push_back({bits, first, count, every, hold});
}

// The pad from the host (pf_pad_live), and the record of its presses.
static std::atomic<uint32_t> g_live{0};
static std::mutex g_record_lock;
static FILE* g_record;
static uint32_t g_record_was, g_record_field;
static uint32_t g_record_since[32];

const char* pf_pad_button_name(uint32_t bit) {
    static const struct { const char* name; uint32_t bit; } kButtons[] = {
        {"down", 0x80000000u}, {"up", 0x40000000u}, {"right", 0x20000000u}, {"left", 0x10000000u},
        {"a", 0x08000000u}, {"b", 0x04000000u}, {"c", 0x02000000u}, {"start", 0x01000000u},
        {"x", 0x00800000u}, {"r", 0x00400000u}, {"l", 0x00200000u},
    };
    for (const auto& k : kButtons)
        if (k.bit == bit) return k.name;
    return "";
}

void pf_pad_live(uint32_t bits) { g_live.store(bits); }

bool pf_pad_record_open(const char* path) {
    std::lock_guard<std::mutex> l(g_record_lock);
    g_record = std::fopen(path, "w");
    return g_record != nullptr;
}

// The live buttons at `field`: each one that came up written as the press it was.
static void record(uint32_t now, uint32_t field, bool all_up) {
    std::lock_guard<std::mutex> l(g_record_lock);
    if (!g_record) return;
    if (all_up) field = g_record_field;
    g_record_field = field;
    for (int i = 0; i < 32; ++i) {
        uint32_t bit = 1u << i;
        if ((now & bit) && !(g_record_was & bit) && !all_up) g_record_since[i] = field;
        if ((g_record_was & bit) && (!(now & bit) || all_up)) {
            std::fprintf(g_record, "--pad %s@%u+%u\n", pf_pad_button_name(bit), g_record_since[i],
                         field - g_record_since[i] ? field - g_record_since[i] : 1);
            std::fflush(g_record);
        }
    }
    g_record_was = all_up ? 0 : now;
}

void pf_pad_record_close() {
    record(0, 0, true);
    std::lock_guard<std::mutex> l(g_record_lock);
    if (g_record) std::fclose(g_record);
    g_record = nullptr;
}

static uint32_t pad_at(uint64_t field) {
    uint32_t live = g_live.load();
    record(live, (uint32_t)field, false);
    uint32_t bits = live;
    for (const Press& p : g_presses)
        for (int k = 0; k < p.count; ++k) {
            uint64_t at = p.first + (uint64_t)k * (uint64_t)p.every;
            if (field >= at && field < at + (uint64_t)p.hold) bits |= p.bits;
        }
    return bits;
}

[[noreturn]] static void not_yet(const char* what) {
    std::fflush(stdout);
    std::fprintf(stderr, "event broker: %s: not yet\n", what);
    std::exit(3);
}

static Listener* listener_of(int32_t port) {
    for (Listener* l : g_listeners)
        if (l->port == port) return l;
    return nullptr;
}

static void to_head(Listener* l) {
    for (size_t i = 0; i < g_listeners.size(); ++i)
        if (g_listeners[i] == l) { g_listeners.erase(g_listeners.begin() + (long)i); break; }
    g_listeners.insert(g_listeners.begin(), l);
}

// ---- requests ----------------------------------------------------------------------------------
static void configure(int32_t msg, uint32_t m, int32_t reply_port, uint32_t data) {
    Listener* l = listener_of(reply_port);
    bool fresh = !l;
    if (fresh) {
        l = new Listener{};
        l->port = reply_port;
        g_listeners.insert(g_listeners.begin(), l);
    }
    l->category = pf_r32(data + CR_CATEGORY);
    for (int i = 0; i < 8; ++i) {
        l->trigger[i] = pf_r32(data + CR_TRIGGERMASK + 4u * i);
        l->capture[i] = pf_r32(data + CR_CAPTUREMASK + 4u * i);
    }
    // The broker stores a cr_QueueMax of 1 to 20 as a word over its byte fields (0x107c: `str` at
    // +0x60), which leaves the queue at 0 and the focus bytes cleared: not followed here yet.
    int32_t qmax = (int32_t)pf_r32(data + CR_QUEUEMAX);
    if (qmax > 0 && qmax <= 20) not_yet("a configure with cr_QueueMax");
    if (fresh && (l->category == LC_FOCUSLISTENER || l->category == LC_FOCUSUI)) {
        if (g_focus) g_focus->focus = false;
        to_head(l);
        g_focus = l;
        l->focus = true;
    }
    if (g_pf_trace)
        pf_log("        event broker: configure from port %d, category %u, trigger %08x, capture %08x\n", reply_port,
               l->category, l->trigger[0], l->capture[0]);
    if (pf_r8(m + 11) & MESSAGE_PASS_BY_VALUE) {
        uint32_t buf = pf_r32(m + MSG_DATAPTR);
        pf_w32(buf, EB_CONFIGUREREPLY);
        pf_reply_msg(msg, 0, buf, 4);
    } else {
        pf_reply_msg(msg, 0, 0, 0);
    }
}

// A message at the broker's port (the main loop's GetMsg, 0xd88): one of its own, back from a
// listener, or a request.
static void on_msg(int32_t port, int32_t) {
    for (int32_t msg; (msg = pf_get_msg(port)) > 0;) {
        auto back = g_pending.find(msg);
        if (back != g_pending.end()) {
            if (back->second->in_transit) --back->second->in_transit;
            g_pending.erase(back);
            g_free.insert(g_free.begin(), msg);
            continue;
        }
        uint32_t m = pf_item_node(msg);
        int32_t reply_port = (int32_t)pf_r32(m + MSG_REPLYPORT);
        if (!reply_port) continue;
        uint32_t data = pf_r32(m + MSG_DATAPTR), size = pf_r32(m + MSG_DATASIZE);
        if (!data || (data & 3) || size < 4) {
            pf_reply_msg(msg, (int32_t)EB_ERR_BADPTR, 0, 0);
            continue;
        }
        uint32_t flavor = pf_r32(data);
        if (flavor == EB_CONFIGURE) configure(msg, m, reply_port, data);
        else {
            char why[64];
            std::snprintf(why, sizeof why, "a request of flavour %u", flavor);
            not_yet(why);
        }
    }
}

// ---- each field --------------------------------------------------------------------------------
// An event record as the broker builds it on its stack: words, the header first.
struct Record {
    std::vector<uint32_t> w{EB_EVENTRECORD};
    uint32_t field;
    void frame(uint32_t event, bool pad, uint32_t bits) {
        size_t at = w.size();
        w.resize(at + (EF_HEADER + (pad ? 4 : 0)) / 4, 0);
        w[at + EF_BYTECOUNT / 4] = EF_HEADER + (pad ? 4 : 0);
        w[at + EF_SYSTEMTIMESTAMP / 4] = field;
        w[at + EF_EVENTNUMBER / 4] = event << 24 | (pad ? PAD_POD << 16 | PAD_POSITION << 8 | PAD_GENERIC : 0);
        if (pad) w[at + EF_EVENTDATA / 4] = bits;
    }
};

static void broker_vbl(uint64_t) {
    uint32_t field = pf_r32(pf_folio_base(PF_GRAPHICS) + GF_VBLNUMBER);
    uint32_t now = pad_at(field), was = g_pad;
    g_pad = now;
    uint32_t down = now & ~was, up = was & ~now;
    // the pad's events this field (0x2c1c), in word 0 of the masks
    uint32_t ready = down ? (up ? 0xF0000000u : 0xB0000000u) : (up ? 0x70000000u : 0x10000000u);
    std::vector<Listener*> all = g_listeners;
    for (Listener* l : all) {
        if (l->category == LC_NOSEEUM) continue;
        bool focus_changed = l->focus != l->last_focus;
        if (!focus_changed && l->category == LC_FOCUSLISTENER && l != g_focus) continue;
        bool hears = focus_changed || l->overflow ||
                     ((l->category == LC_OBSERVER || l == g_focus) && (l->trigger[0] & ready));
        if (!hears) continue;
        Record r;
        r.field = field;
        if (l->overflow) r.frame(EVENTNUM_EVENTQUEUEOVERFLOW, false, 0);
        if (focus_changed) {
            l->last_focus = l->focus;
            if (l->focus && (l->trigger[0] & EVENTBIT0_GIVINGFOCUS)) r.frame(EVENTNUM_GIVINGFOCUS, false, 0);
            if (!l->focus && (l->trigger[0] & EVENTBIT0_LOSINGFOCUS)) r.frame(EVENTNUM_LOSINGFOCUS, false, 0);
        }
        uint32_t want = (l->trigger[0] | l->capture[0]) & ready;
        if (want & 0x80000000u) r.frame(EVENTNUM_CONTROLBUTTONPRESSED, true, down);
        if (want & 0x40000000u) r.frame(EVENTNUM_CONTROLBUTTONRELEASED, true, up);
        if (l->overflow || (want & 0x20000000u)) r.frame(EVENTNUM_CONTROLBUTTONUPDATE, true, now);
        if (want & 0x10000000u) r.frame(EVENTNUM_CONTROLBUTTONARRIVED, true, now);
        r.w.push_back(0);                                   // the empty frame
        if (r.w.size() <= 2) continue;                      // nothing to report
        if (l->in_transit >= l->queue_max) {
            l->overflow = true;
            continue;
        }
        uint32_t size = 4 * (uint32_t)r.w.size(), need = (size + 15) & ~15u;
        int32_t msg = 0;
        for (size_t i = 0; i < g_free.size() && !msg; ++i)
            if (pf_r32(pf_item_node(g_free[i]) + MSG_DATAPTRSIZE) >= need) {
                msg = g_free[i];
                g_free.erase(g_free.begin() + (long)i);
            }
        if (!msg) msg = pf_msg_new(g_port, need);
        uint32_t buf = pf_r32(pf_item_node(msg) + MSG_DATAPTR);
        for (size_t i = 0; i < r.w.size(); ++i) pf_w32(buf + 4 * (uint32_t)i, r.w[i]);
        if (pf_send_msg(l->port, msg, buf, size) < 0) not_yet("a listener whose port is gone");
        g_pending[msg] = l;
        l->overflow = false;
        ++l->in_transit;
        if (g_pf_trace)
            pf_log("        event broker: field %u, pad %08x: %u bytes to port %d\n", field, now, size, l->port);
    }
}

void pf_event_init() {
    for (Listener* l : g_listeners) delete l;
    g_listeners.clear();
    g_focus = nullptr;
    g_free.clear();
    g_pending.clear();
    g_pad = 0;
    g_port = pf_msgport_new("eventbroker", on_msg);
    pf_on_vbl(broker_vbl);
}
