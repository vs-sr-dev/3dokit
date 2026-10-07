// 3dokit runtime -- the audio folio: its items (instrument templates, instruments, knobs,
// samples) and the SWIs and vector slots on them, as far as the programs run so far reach them.
// What a call checks, makes and returns is what the 1993 folio does (System/Folios/AUDIOFOLIO,
// V20.19, built 5 September 1993, decompressed by its own code in 3dokit.armemu; the addresses
// are its own). What it does not do is the DSP: the folio loads each instrument's code into the
// DSP, connects instruments by patching that code and writes knobs into the DSP's memory; here an
// instrument is what its .dsp file says (3dokit.dsp reads the same files), and every value the
// folio would write to the DSP is kept by resource, for a native mixer to read.
//
// The items' nodes are the folio's: their sizes and n_Flags come from its node database (0xc04c),
// and the kernel's CreateItem makes them the caller's. What the folio keeps in a node after the
// ItemNode is private to it -- no SDK header describes it -- and no program run so far reads it,
// so it is kept on the host side instead.
#include "pf.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

enum : uint32_t {
    NST_AUDIO = 4,
    TEMPLATE_NODE = 1, INSTRUMENT_NODE = 2, KNOB_NODE = 3, SAMPLE_NODE = 4,
    // audio.h's tags (enum audio_folio_tags)
    AF_TAG_AMPLITUDE = 10, AF_TAG_RATE = 11, AF_TAG_NAME = 12, AF_TAG_PITCH = 14,
    AF_TAG_VELOCITY = 15, AF_TAG_TEMPLATE = 16, AF_TAG_INSTRUMENT = 17, AF_TAG_PRIORITY = 39,
    AF_TAG_SET_FLAGS = 40, AF_TAG_FREQUENCY = 42,
    AF_INSF_LEGALFLAGS = 1,
    // a DSP resource's type (3dokit.dsp): what a knob writes, what a connection joins
    RSRC_KNOB = 1, RSRC_VARIABLE = 2,
};
// The folio's node database (CREATEFOLIO_TAG_NODEDATABASE): a size and n_Flags per node type,
// 0x90 being NODE_ITEMVALID | NODE_NAMEVALID.
static const uint32_t kNodeSize[] = {0, 0x54, 0x58, 0x34, 0x98, 0x38, 0x74, 0x54, 0x34};
static const uint32_t kNodeFlags = 0x90;

// The folio's errors (audio.h's MAKEAERR: AF_ERR_*).
enum : uint32_t {
    AF_ERR_BADITEM = 0xD52BF001u, AF_ERR_BADTAG = 0xD52BF002u, AF_ERR_BADTAGVAL = 0xD52BF003u,
    AF_ERR_NOKNOBS = 0xD52BF104u, AF_ERR_BADNAME = 0xD52BF105u, AF_ERR_BADCALCTYPE = 0xD52BF107u,
    AF_ERR_BADKNOBRSRC = 0xD52BF108u, AF_ERR_AUDIOCLOSED = 0xD52BF11Du,
};

// The sample rate the folio converts frequencies with (its globals +0x48, set when it starts).
static const int32_t kSampleRate = 44100;

// ---- instruments, as their .dsp files describe them ----------------------------------------
// FORM 3INS { NAME, FORM DSPP { DHDR, DCOD, DRSC, DRLC, DNMS, DKNB } }: the resources (DRSC, 16
// bytes each: type, count; named in order by DNMS) and the knobs (DKNB, a list linked by offsets
// from the chunk's start: next, min, max, default, a count of targets, a 32-byte name, then per
// target its resource, its calculation type and two operands).
struct DspRsrc {
    std::string name;
    uint32_t type, count;
};
struct DspKnob {
    std::string name;
    int32_t lo, hi, dflt;
    struct Target { uint32_t rsrc; int32_t calc, a, b; };
    std::vector<Target> to;
};
struct Template {
    std::string path;                       // as the program named it
    std::vector<DspRsrc> rsrc;
    std::vector<DspKnob> knobs;
};

static uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return o + 4 <= d.size() ? (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3] : 0;
}

// Every chunk in [off, end), into FORMs: its tag, start and end.
static void iff_walk(const std::vector<uint8_t>& d, size_t off, size_t end,
                     std::map<std::string, std::pair<size_t, size_t>>& found) {
    while (off + 8 <= end) {
        std::string tag(d.begin() + (long)off, d.begin() + (long)off + 4);
        size_t n = be32(d, off + 4), stop = std::min(end, off + 8 + n);
        if (tag == "FORM") iff_walk(d, off + 12, stop, found);
        else if (!found.count(tag)) found[tag] = {off + 8, stop};
        off += 8 + n + (n & 1);
    }
}

static bool parse_dsp(const std::vector<uint8_t>& d, Template& t) {
    std::map<std::string, std::pair<size_t, size_t>> ch;
    iff_walk(d, 0, d.size(), ch);
    if (!ch.count("DRSC") || !ch.count("DNMS")) return false;
    auto [ra, rb] = ch["DRSC"];
    auto [na, nb] = ch["DNMS"];
    for (size_t o = ra, n = na; o + 16 <= rb; o += 16) {
        std::string name;
        while (n < nb && d[n]) name += (char)d[n++];
        ++n;
        t.rsrc.push_back({name, be32(d, o), be32(d, o + 4)});
    }
    if (ch.count("DKNB")) {
        auto [ka, kb] = ch["DKNB"];
        for (size_t o = ka;;) {
            if (o + 52 > kb) return false;
            DspKnob k;
            for (size_t i = 0; i < 32 && d[o + 20 + i]; ++i) k.name += (char)d[o + 20 + i];
            k.lo = (int32_t)be32(d, o + 4);
            k.hi = (int32_t)be32(d, o + 8);
            k.dflt = (int32_t)be32(d, o + 12);
            uint32_t n = be32(d, o + 16);
            if (o + 52 + 16 * (size_t)n > kb) return false;
            for (uint32_t i = 0; i < n; ++i) {
                size_t e = o + 52 + 16 * i;
                k.to.push_back({be32(d, e), (int32_t)be32(d, e + 4), (int32_t)be32(d, e + 8), (int32_t)be32(d, e + 12)});
            }
            t.knobs.push_back(k);
            uint32_t next = be32(d, o);
            if (!next) break;
            o = ka + next;
        }
    }
    return true;
}

// ---- the folio's items ----------------------------------------------------------------------
struct Connection {
    int32_t from;                           // the instrument whose variable is read
    uint32_t from_rsrc, to_rsrc;            // its resource, and the one of this instrument it feeds
};
struct Instrument {
    int32_t tmpl;
    uint32_t flags;
    int state;                              // 0 allocated, 3 started (the node's +0x30 in the folio)
    std::map<uint32_t, int32_t> value;      // what the folio wrote to each knob resource
    std::vector<Connection> inputs;
};
struct Knob {
    int32_t ins;
    int knob;                               // index in the template's knobs
};

static std::map<int32_t, Template> g_templates;
static std::map<int32_t, Instrument> g_instruments;
static std::map<int32_t, Knob> g_knobs;
static std::map<int32_t, int> g_samples;    // their info: the folio's defaults (0x2c04), unread yet

static uint32_t task_item() { return pf_r32(pf_current_task() + 24); }
static int32_t audio_folio_item() { return (int32_t)pf_r32(pf_folio_base(PF_AUDIO) + 24); }

// The folio's guard at the top of most calls (0x40c, Kernel -128): AF_ERR_AUDIOCLOSED unless the
// calling task has the folio open.
static bool audio_open() { return pf_item_opened((int32_t)task_item(), audio_folio_item()) >= 0; }

// A node of the folio's for the caller (the kernel's CreateItem with the node database's entry).
static int32_t audio_item(uint32_t type, uint8_t pri = 0) {
    uint32_t n = pf_os_alloc(kNodeSize[type]);
    pf_w32(n + 12, kNodeSize[type]);
    int32_t item = pf_item_new(n, NST_AUDIO, (int)type, nullptr);
    pf_w8(n + 10, pri);
    pf_w8(n + 11, kNodeFlags);
    pf_w32(n + 28, task_item());                            // n_Owner
    return item;
}

static std::string guest_string(uint32_t a) {
    char s[256];
    pf_cstring(a, s, sizeof s);
    return s;
}

// The knob's value, as the folio's tweak (0x9780) computes and writes it: per target, the value
// through its calculation (cooked only; 0 or raw: as it is; 1: v * a + b; 2: v * a / b;
// 3: v / the sample rate), the first target's result clamped to the knob's [min, max] and then
// the value the later targets start from; each target's resource must be a knob's.
static uint32_t tweak(ArmCpu& c, Instrument& ins, const Template& t, const DspKnob& k, int32_t v, bool cooked) {
    for (size_t i = 0; i < k.to.size(); ++i) {
        const auto& e = k.to[i];
        int32_t w = v;
        if (cooked && e.calc) {
            if (e.calc == 1) w = v * e.a + e.b;
            else if (e.calc == 2 || e.calc == 3) {
                int32_t num = e.calc == 2 ? v * e.a : v, den = e.calc == 2 ? e.b : kSampleRate;
                if (!den) pf_stop(c, "a knob's calculation divides by zero");
                w = num / den;
            } else return AF_ERR_BADCALCTYPE;
        }
        if (i == 0) {
            if (w > k.hi) w = k.hi;
            else if (w < k.lo) w = k.lo;
            v = w;
        }
        if (e.rsrc >= t.rsrc.size() || t.rsrc[e.rsrc].type != RSRC_KNOB) return AF_ERR_BADKNOBRSRC;
        ins.value[e.rsrc] = w;
    }
    return 0;
}

static const DspKnob* find_knob(const Template& t, const char* name, int* index = nullptr) {
    for (size_t i = 0; i < t.knobs.size(); ++i)
        if (!std::strncmp(t.knobs[i].name.c_str(), name, 32)) {
            if (index) *index = (int)i;
            return &t.knobs[i];
        }
    return nullptr;
}

// A resource of the template by name and type (0x8764: names compared as strncmp, 32 characters).
static int find_rsrc(const Template& t, uint32_t type, const std::string& name) {
    for (size_t i = 0; i < t.rsrc.size(); ++i)
        if (t.rsrc[i].type == type && !std::strncmp(t.rsrc[i].name.c_str(), name.c_str(), 32)) return (int)i;
    return -1;
}

// ---- making items (the folio's ir_Create, 0x1048, by node type) -----------------------------
// A template (0x230c) from a parsed .dsp file; LoadInsTemplate hands it over.
static int32_t make_template(Template&& t) {
    int32_t item = audio_item(TEMPLATE_NODE);
    g_templates[item] = std::move(t);
    return item;
}

// A creation's tags, up to TAG_END: from the caller's TagArgs, or the ones a folio call makes.
typedef std::vector<std::pair<uint32_t, uint32_t>> Tags;

static Tags read_tags(uint32_t p) {
    Tags t;
    for (; p; p += 8) {
        uint32_t tag = pf_r32(p);
        if (!tag) break;
        t.push_back({tag, pf_r32(p + 4)});
    }
    return t;
}

// An instrument (0x2104): AF_TAG_TEMPLATE, AF_TAG_PRIORITY (0 to 255, 100 when not given; the
// node's n_Priority), AF_TAG_SET_FLAGS (AF_INSF_AUTOABANDON only). Every knob starts at its
// default, written raw (0x8cf0).
static uint32_t create_instrument(ArmCpu& c, const Tags& tags) {
    int32_t tmpl = 0;
    uint32_t pri = 100, flags = 0;
    for (auto [tag, v] : tags) {
        if (tag == AF_TAG_TEMPLATE) tmpl = (int32_t)v;
        else if (tag == AF_TAG_PRIORITY) {
            if ((int32_t)v < 0 || (int32_t)v > 255) return AF_ERR_BADTAGVAL;
            pri = v;
        } else if (tag == AF_TAG_SET_FLAGS) {
            if (v & ~AF_INSF_LEGALFLAGS) return AF_ERR_BADTAGVAL;
            flags = v;
        } else pf_stop(c, "an instrument's tag other than TEMPLATE, PRIORITY, SET_FLAGS: not yet");
    }
    if (!pf_check_item(tmpl, NST_AUDIO, TEMPLATE_NODE)) return AF_ERR_BADITEM;
    const Template& t = g_templates[tmpl];
    Instrument ins{tmpl, flags, 0, {}, {}};
    for (const auto& k : t.knobs)
        if (uint32_t err = tweak(c, ins, t, k, k.dflt, false)) return err;
    int32_t item = audio_item(INSTRUMENT_NODE, (uint8_t)pri);
    g_instruments[item] = std::move(ins);
    return (uint32_t)item;
}

// A knob (0x2684): AF_TAG_NAME and AF_TAG_INSTRUMENT, both required; the name is one of the
// instrument's knobs (0x967c).
static uint32_t create_knob(ArmCpu& c, const Tags& tags) {
    uint32_t name = 0;
    int32_t ins = 0;
    for (auto [tag, v] : tags) {
        if (tag == AF_TAG_NAME) name = v;
        else if (tag == AF_TAG_INSTRUMENT) ins = (int32_t)v;
        else if (tag > 9) return AF_ERR_BADTAG;
        else pf_stop(c, "a knob's item tag: not yet");
    }
    if (!name) return AF_ERR_BADNAME;
    if (!pf_check_item(ins, NST_AUDIO, INSTRUMENT_NODE)) return AF_ERR_BADITEM;
    const Template& t = g_templates[g_instruments[ins].tmpl];
    if (t.knobs.empty()) return AF_ERR_NOKNOBS;
    int index;
    std::string s = guest_string(name);
    if (!find_knob(t, s.c_str(), &index)) return AF_ERR_BADNAME;
    if (g_pf_trace) pf_log("        knob \"%s\" of instrument %d\n", s.c_str(), ins);
    int32_t item = audio_item(KNOB_NODE);
    g_knobs[item] = {ins, index};
    return (uint32_t)item;
}

// A sample (0x3a3c), as yet only without tags: the folio's defaults, which nothing reads yet.
static uint32_t create_sample(ArmCpu& c, const Tags& tags) {
    if (!tags.empty()) pf_stop(c, "a sample's tags: not yet");
    int32_t item = audio_item(SAMPLE_NODE);
    g_samples[item] = 0;
    return (uint32_t)item;
}

// The folio's ir_Create: first the folio open (0x109c), then the node type's own routine.
static uint32_t create(ArmCpu& c, int type, const Tags& tags) {
    if (!audio_open()) return AF_ERR_AUDIOCLOSED;
    switch (type) {
    case INSTRUMENT_NODE: return create_instrument(c, tags);
    case KNOB_NODE: return create_knob(c, tags);
    case SAMPLE_NODE: return create_sample(c, tags);
    default: {
        char why[80];
        std::snprintf(why, sizeof why, "CreateItem of the audio folio's node type %d: not yet", type);
        pf_stop(c, why);
    }
    }
}

static uint32_t audio_create(ArmCpu& c, int type, uint32_t tags) { return create(c, type, read_tags(tags)); }

// ---- the calls ------------------------------------------------------------------------------
// audio -4: Item LoadInsTemplate(char* name, Item aux) -- 0x1610: the folio open; aux must be 0;
// the file parsed as IFF (iffParseFile, in the caller's task, so from its current directory) and
// a template made of its FORM DSPP (CreateItem(MKNODEID(AUDIONODE, AUDIO_TEMPLATE_NODE),
// {AF_TAG_TEMPLATE, the parsed form}).
static void a_loadinstemplate(ArmCpu& c) {
    if (!audio_open()) { c.r[0] = AF_ERR_AUDIOCLOSED; return; }
    if (c.r[1]) { c.r[0] = AF_ERR_BADITEM; return; }
    std::string name = guest_string(c.r[0]), host = pf_host_path(name.c_str());
    if (g_pf_trace) pf_log("        LoadInsTemplate \"%s\" -> %s\n", name.c_str(), host.empty() ? "(none)" : host.c_str());
    if (host.empty()) pf_stop(c, "LoadInsTemplate: no such file (the folio's error for it: not yet read)");
    std::ifstream f(host, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Template t;
    t.path = name;
    if (!parse_dsp(d, t)) pf_stop(c, "LoadInsTemplate: not a DSP instrument this runtime can read");
    c.r[0] = (uint32_t)make_template(std::move(t));
}

// audio -8: Item AllocInstrument(Item template, uint8 priority) -- 0x1af0:
// CreateItem(MKNODEID(AUDIONODE, AUDIO_INSTRUMENT_NODE), {AF_TAG_TEMPLATE, AF_TAG_PRIORITY}).
static void a_allocinstrument(ArmCpu& c) {
    c.r[0] = create(c, INSTRUMENT_NODE, {{AF_TAG_TEMPLATE, c.r[0]}, {AF_TAG_PRIORITY, c.r[1] & 0xFF}});
}

// audio -16: Item GrabKnob(Item instrument, char* name) -- 0x2508:
// CreateItem(MKNODEID(AUDIONODE, AUDIO_KNOB_NODE), {AF_TAG_NAME, AF_TAG_INSTRUMENT}).
static void a_grabknob(ArmCpu& c) {
    c.r[0] = create(c, KNOB_NODE, {{AF_TAG_NAME, c.r[1]}, {AF_TAG_INSTRUMENT, c.r[0]}});
}

// swi 0x40000: Err TweakKnob(Item knob, int32 value) -- 0x27c8, and swi 0x40011 TweakRawKnob
// (0x281c), the same without the knob's calculation. Neither asks whether the folio is open.
static void tweak_knob(ArmCpu& c, bool cooked) {
    if (!pf_check_item((int32_t)c.r[0], NST_AUDIO, KNOB_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    const Knob& k = g_knobs[(int32_t)c.r[0]];
    Instrument& ins = g_instruments[k.ins];
    const Template& t = g_templates[ins.tmpl];
    c.r[0] = tweak(c, ins, t, t.knobs[(size_t)k.knob], (int32_t)c.r[1], cooked);
}
static void a_tweakknob(ArmCpu& c) { tweak_knob(c, true); }
static void a_tweakrawknob(ArmCpu& c) { tweak_knob(c, false); }

// swi 0x40001: Err StartInstrument(Item instrument, TagArg* tags) -- 0x1b68 and 0x7148: the folio
// open; AF_TAG_RATE (the Frequency knob, raw) or AF_TAG_FREQUENCY (cooked), AF_TAG_AMPLITUDE or
// AF_TAG_VELOCITY (velocity << 8: the Amplitude knob, cooked), each when the instrument has that
// knob; then its attachments start, then the instrument. Its node's state becomes 3.
static void a_startinstrument(ArmCpu& c) {
    if (!audio_open()) { c.r[0] = AF_ERR_AUDIOCLOSED; return; }
    int32_t item = (int32_t)c.r[0];
    if (!pf_check_item(item, NST_AUDIO, INSTRUMENT_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    Instrument& ins = g_instruments[item];
    const Template& t = g_templates[ins.tmpl];
    int32_t freq = 0, amp = -1;
    bool have_freq = false, cooked_freq = false;
    for (uint32_t p = c.r[1]; p; p += 8) {
        uint32_t tag = pf_r32(p), v = pf_r32(p + 4);
        if (!tag) break;
        if (tag == AF_TAG_AMPLITUDE) amp = (int32_t)v;
        else if (tag == AF_TAG_VELOCITY) amp = (int32_t)(v << 8);
        else if (tag == AF_TAG_RATE) { freq = (int32_t)v; have_freq = true; cooked_freq = false; }
        else if (tag == AF_TAG_FREQUENCY) { freq = (int32_t)v; have_freq = true; cooked_freq = true; }
        else if (tag == AF_TAG_PITCH) pf_stop(c, "StartInstrument: AF_TAG_PITCH: not yet");
        else { c.r[0] = AF_ERR_BADTAG; return; }
    }
    if (const DspKnob* k = find_knob(t, "Frequency"); k && have_freq) tweak(c, ins, t, *k, freq, cooked_freq);
    if (const DspKnob* k = find_knob(t, "Amplitude"); k && amp >= 0) tweak(c, ins, t, *k, amp, true);
    if (g_pf_trace) pf_log("        StartInstrument %d (%s)\n", item, t.path.c_str());
    ins.state = 3;
    c.r[0] = 0;
}

// swi 0x40008: Err ConnectInstruments(Item src, char* srcName, Item dst, char* dstName) -- 0x1c04
// and 0x8018: both instruments; the source's variable of that name, the destination's variable
// or else knob of that name, or AF_ERR_BADNAME. The folio patches the destination's code to read
// the source's variable; here the connection is written down.
static void a_connectinstruments(ArmCpu& c) {
    int32_t src = (int32_t)c.r[0], dst = (int32_t)c.r[2];
    if (!pf_check_item(src, NST_AUDIO, INSTRUMENT_NODE) || !pf_check_item(dst, NST_AUDIO, INSTRUMENT_NODE)) {
        c.r[0] = AF_ERR_BADITEM;
        return;
    }
    std::string from = guest_string(c.r[1]), to = guest_string(c.r[3]);
    const Template& ts = g_templates[g_instruments[src].tmpl];
    Instrument& d = g_instruments[dst];
    const Template& td = g_templates[d.tmpl];
    int a = find_rsrc(ts, RSRC_VARIABLE, from), b = find_rsrc(td, RSRC_VARIABLE, to);
    if (b < 0) b = find_rsrc(td, RSRC_KNOB, to);
    if (g_pf_trace) pf_log("        connect %d \"%s\" -> %d \"%s\"\n", src, from.c_str(), dst, to.c_str());
    if (a < 0 || b < 0) { c.r[0] = AF_ERR_BADNAME; return; }
    d.inputs.push_back({src, (uint32_t)a, (uint32_t)b});
    c.r[0] = 0;
}

void pf_audio_init() {
    g_templates.clear();
    g_instruments.clear();
    g_knobs.clear();
    g_samples.clear();
    pf_on_create(NST_AUDIO, audio_create);
    pf_on_slot(PF_AUDIO, -4, a_loadinstemplate);
    pf_on_slot(PF_AUDIO, -8, a_allocinstrument);
    pf_on_slot(PF_AUDIO, -16, a_grabknob);
    pf_on_swi(0x40000, a_tweakknob);
    pf_on_swi(0x40001, a_startinstrument);
    pf_on_swi(0x40008, a_connectinstruments);
    pf_on_swi(0x40011, a_tweakrawknob);
}
