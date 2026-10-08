// 3dokit runtime -- the audio folio: its items (instrument templates, instruments, knobs,
// samples) and the SWIs and vector slots on them, as far as the programs run so far reach them.
// What a call checks, makes and returns is what the 1993 folio does (System/Folios/AUDIOFOLIO,
// V20.19, built 5 September 1993, decompressed by its own code in 3dokit.armemu; the addresses
// are its own). The DSP is pf_dsp.cpp's: the folio loads each instrument's code into the DSP,
// connects instruments by patching that code, writes knobs into the DSP's memory and programs the
// DMA that feeds the FIFOs; here an instrument is what its .dsp file says (3dokit.dsp reads the
// same files), every value the folio would write to the DSP is kept by resource and handed to the
// native instrument, and the DMA is programmed as the folio programs it.
//
// The items' nodes are the folio's: their sizes and n_Flags come from its node database (0xc04c),
// and the kernel's CreateItem makes them the caller's. What the folio keeps in a node after the
// ItemNode is private to it -- no SDK header describes it -- and no program run so far reads it,
// so it is kept on the host side instead.
#include "pf.h"
#include <cctype>
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
    TEMPLATE_NODE = 1, INSTRUMENT_NODE = 2, KNOB_NODE = 3, SAMPLE_NODE = 4, CUE_NODE = 5, ATTACHMENT_NODE = 7,
    // audio.h's tags (enum audio_folio_tags)
    AF_TAG_AMPLITUDE = 10, AF_TAG_RATE = 11, AF_TAG_NAME = 12, AF_TAG_PITCH = 14,
    AF_TAG_VELOCITY = 15, AF_TAG_TEMPLATE = 16, AF_TAG_INSTRUMENT = 17, AF_TAG_WIDTH = 22,
    AF_TAG_CHANNELS = 23, AF_TAG_FRAMES = 24, AF_TAG_BASENOTE = 25, AF_TAG_DETUNE = 26,
    AF_TAG_LOWNOTE = 27, AF_TAG_HIGHNOTE = 28, AF_TAG_LOWVELOCITY = 29, AF_TAG_HIGHVELOCITY = 30,
    AF_TAG_SUSTAINBEGIN = 31, AF_TAG_SUSTAINEND = 32, AF_TAG_RELEASEBEGIN = 33,
    AF_TAG_RELEASEEND = 34, AF_TAG_NUMBYTES = 35, AF_TAG_ADDRESS = 36, AF_TAG_SAMPLE = 37,
    AF_TAG_PRIORITY = 39, AF_TAG_SET_FLAGS = 40, AF_TAG_FREQUENCY = 42, AF_TAG_ENVELOPE = 43,
    AF_TAG_HOOKNAME = 44, AF_TAG_START_AT = 45, AF_TAG_SAMPLE_RATE = 46,
    AF_TAG_COMPRESSIONRATIO = 47, AF_TAG_COMPRESSIONTYPE = 48, AF_TAG_NUMBITS = 49,
    AF_TAG_DELAY_LINE = 57,
    AF_INSF_AUTOABANDON = 1, AF_INSF_LEGALFLAGS = 1,
    // a DSP resource's type (3dokit.dsp): what a knob writes, what a connection joins
    RSRC_KNOB = 1, RSRC_VARIABLE = 2, RSRC_IFIFO = 6, RSRC_OFIFO = 7,
};
// The folio's node database (CREATEFOLIO_TAG_NODEDATABASE): a size and n_Flags per node type,
// 0x90 being NODE_ITEMVALID | NODE_NAMEVALID.
// Each version's own: 1993's AUDIOFOLIO 20.19 (0xc04c), 20.27 (0xbf94: an instrument 0x64, type 6
// 0x78) and 23.10 (0xb9e8: a template 0x70, an instrument 0x64, a sample 0x9c, type 6 0x78); the
// versions between are unread. A node is made that size and cleared; the runtime fills the fields
// it reads in the 1993 folio.
static const uint32_t kNodeSize[] = {0, 0x54, 0x58, 0x34, 0x98, 0x38, 0x74, 0x54, 0x34};
static const uint32_t kNodeSize2027[] = {0, 0x54, 0x64, 0x34, 0x98, 0x38, 0x78, 0x54, 0x34};
static const uint32_t kNodeSize2310[] = {0, 0x70, 0x64, 0x34, 0x9c, 0x38, 0x78, 0x54, 0x34};
static uint32_t node_size(uint32_t type) {
    uint32_t v = pf_system_version("/System/Folios/AUDIOFOLIO");
    return (v >= PF_VERSION(23, 10) ? kNodeSize2310 : v >= PF_VERSION(20, 27) ? kNodeSize2027 : kNodeSize)[type];
}
static const uint32_t kNodeFlags = 0x90;

// The folio's errors (audio.h's MAKEAERR: AF_ERR_*).
enum : uint32_t {
    AF_ERR_BADITEM = 0xD52BF001u, AF_ERR_BADTAG = 0xD52BF002u, AF_ERR_BADTAGVAL = 0xD52BF003u,
    AF_ERR_NOFIFO = 0xD52BF102u, AF_ERR_NOKNOBS = 0xD52BF104u, AF_ERR_BADNAME = 0xD52BF105u,
    AF_ERR_BADCALCTYPE = 0xD52BF107u,
    AF_ERR_BADKNOBRSRC = 0xD52BF108u, AF_ERR_OUTOFRANGE = 0xD52BF117u,
    AF_ERR_UNIMPLEMENTED = 0xD52BF118u, AF_ERR_SECURITY = 0xD52BF11Bu, AF_ERR_AUDIOCLOSED = 0xD52BF11Du,
};

// The sample rate the folio converts frequencies with (its globals +0x48, set when it starts).
static const int32_t kSampleRate = 44100;

// Operamath's DivUF16 (its slot -12, 0x2420), which the folio calls for its rates: (n << 16) / d,
// or 0xFFFFFFFF when that does not fit in 32 bits or d is 0. Checked against the 1993 code.
static uint32_t div_uf16(uint32_t n, uint32_t d) {
    if (!d) return 0xFFFFFFFFu;
    uint64_t q = ((uint64_t)n << 16) / d;
    return q > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)q;
}
// Operamath's MulUF16 (its slot -4, 0x2840): (a * b) >> 16, its low 32 bits -- the 1993 code's
// three partial products, summed modulo 2^32, are exactly that.
static uint32_t mul_uf16(uint32_t a, uint32_t b) { return (uint32_t)((uint64_t)a * b >> 16); }

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
    PfDspCode code;                         // DCOD's words, past its 3-word header, and DRLC
    std::map<uint32_t, std::pair<PfDspCode, uint32_t>> imports;     // as PfDspTemplate's
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
    if (ch.count("DCOD")) {
        auto [ca, cb] = ch["DCOD"];
        if (ca + 12 <= cb) t.code.words.assign(d.begin() + (long)ca + 12, d.begin() + (long)cb);
    }
    if (ch.count("DRLC")) {
        auto [la, lb] = ch["DRLC"];
        for (size_t o = la; o + 16 <= lb; o += 16)
            t.code.relocs.push_back({be32(d, o), be32(d, o + 4), be32(d, o + 8), be32(d, o + 12)});
    }
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
    int32_t self;                           // its item (0 until made), the DSP's instrument
    int32_t tmpl;
    uint32_t flags;
    int state;                              // the node's +0x30: 0 allocated or abandoned, 1 stopped, 2 released, 3 started
    int dsp_state;                          // its DSP side's (+0x14 of the folio's record): above 1 running
    std::map<uint32_t, int32_t> value;      // what the folio wrote to each knob resource
    std::vector<Connection> inputs;
    // Per FIFO (a resource), the attachment it plays: the folio keeps it in its table of the DSP's
    // FIFOs (+0x18 of an entry, by the FIFO's number), set when an attachment starts, cleared when
    // it stops.
    std::map<int, int32_t> playing;
    uint32_t start_time = 0;                // the node's +0x60: the folio's time at the last start (23.10)
};
struct Knob {
    int32_t ins;
    int knob;                               // index in the template's knobs
};
// A sample's info, the fields the folio keeps in its node (the offsets are its own; GetAudioItemInfo,
// 0x37d8, reads them back by tag), at the folio's defaults (0x2c04). The node's other words (+0x3c,
// +0x40, +0x60, +0x64, +0x90) start at 0 and nothing here changes them; its list of attachments
// (+0x6c, "SampleRefs") and its place on the folio's "AudioSamples" (+0x31c) are not made.
struct Sample {
    uint32_t address = 0;                   // +0x24 AF_TAG_ADDRESS
    uint32_t frames = 0;                    // +0x28 AF_TAG_FRAMES
    int32_t sustain_begin = -1;             // +0x2c
    int32_t sustain_end = -1;               // +0x30
    int32_t release_begin = -1;             // +0x34
    int32_t release_end = -1;               // +0x38
    uint32_t numbytes = 0;                  // +0x48 AF_TAG_NUMBYTES
    uint32_t base_freq = 440u << 16;        // +0x4c AF_TAG_BASEFREQ: the frequency that plays it at its pitch
    uint8_t flags = 0;                      // +0x50: bit 0 the folio's memory (bits 0 and 1 a delay line's)
    uint8_t numbits = 16;                   // +0x51
    uint8_t width = 2;                      // +0x52 bytes
    uint8_t channels = 1;                   // +0x53
    uint8_t basenote = 60;                  // +0x54
    uint8_t detune = 0;                     // +0x55 (read back signed)
    uint8_t lownote = 0, highnote = 127;    // +0x56, +0x57
    uint8_t lowvelocity = 0, highvelocity = 127;    // +0x58, +0x59
    uint8_t compression_ratio = 1;          // +0x5a
    uint32_t data_offset = 0;               // +0x60: where in its AIFF file the data begins
    uint32_t ssnd_bytes = 0;                // +0x64: the data's bytes as its SSND chunk counts them
    uint32_t compression_type = 0;          // +0x68
    uint32_t rate = (uint32_t)kSampleRate << 16;    // +0x8c AF_TAG_SAMPLE_RATE, frac16 Hz
    uint32_t free_fn = 0;                   // +0x90: LoadSampleHere's free function, or 0
};

static std::map<int32_t, Template> g_templates;
static std::map<int32_t, Instrument> g_instruments;
static std::map<int32_t, Knob> g_knobs;
static std::map<int32_t, Sample> g_samples;
// An attachment (0x5d44): a sample to one of an instrument's FIFOs, the hook. The folio's node
// keeps the instrument (+0x2c), the sample (+0x30, its node +0x34), the hook's name (+0x38),
// AF_TAG_START_AT (+0x3c), the FIFO's DSP address (+0x40) and its state (+0x26).
struct Attachment {
    int32_t ins, sample;
    int rsrc;                               // the FIFO, a resource of the instrument's template
    uint32_t flags, start_at;
    int state;                              // +0x26: 0 made, 1 stopped, 2 released, 3 playing
    int32_t next;                           // LinkAttachments (+0x4c): what plays after it, 0 none
    int32_t cue = 0;                        // MonitorAttachment (+0x44): signalled at its end, 0 none
    int32_t cue_index = 0;                  // +0x48: CUE_AT_END (-2), the only one taken
};
static void signal_cue(int32_t& cue);       // with the cues, below
enum : uint32_t { AF_ATTF_NOAUTOSTART = 1, AF_ERR_NULLADDRESS = 0xD52BF119u };
static std::map<int32_t, Attachment> g_attachments;

static uint32_t task_item() { return pf_r32(pf_current_task() + 24); }
static int32_t audio_folio_item() { return (int32_t)pf_r32(pf_folio_base(PF_AUDIO) + 24); }

// The folio's guard at the top of most calls (0x40c, Kernel -128): AF_ERR_AUDIOCLOSED unless the
// calling task has the folio open.
static bool audio_open() { return pf_item_opened((int32_t)task_item(), audio_folio_item()) >= 0; }

// A node of the folio's for the caller (the kernel's CreateItem with the node database's entry).
static int32_t audio_item(uint32_t type, uint8_t pri = 0) {
    uint32_t n = pf_os_alloc(node_size(type));
    pf_w32(n + 12, node_size(type));
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
// the value the later targets start from; each target's resource must be a knob's. 23.10's
// (0x95f4) has a fifth: 4, v * 2^a / the sample rate -- a frequency to a phase step a bits up
// (Immercenary's BadSpire.ins, 8) -- shifted before the division when v * 2^a fits, else after it.
// Both folios divide with the compiler's signed division, which truncates.
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
            } else if (e.calc == 4 && e.a >= 0 && e.a < 31) {
                bool fits = v <= (int32_t)(0xFFFFFFFFu >> (e.a + 1));
                w = fits ? (int32_t)((uint32_t)v << e.a) / kSampleRate : (int32_t)((uint32_t)(v / kSampleRate) << e.a);
            } else return AF_ERR_BADCALCTYPE;
        }
        if (i == 0) {
            if (w > k.hi) w = k.hi;
            else if (w < k.lo) w = k.lo;
            v = w;
        }
        if (e.rsrc >= t.rsrc.size() || t.rsrc[e.rsrc].type != RSRC_KNOB) return AF_ERR_BADKNOBRSRC;
        ins.value[e.rsrc] = w;
        if (ins.self) pf_dsp_write(ins.self, e.rsrc, w);
    }
    return 0;
}

// A resource's or knob's name against the one asked for. The 1993 folio compares them with strncmp,
// 32 characters (0x8764); 23.10's without regard to case, to the end (0xa964, both letters made
// upper case: its resources by 0x85d0, its knobs by 0x951c) -- Immercenary's GoodSpire.ins calls
// its output "OutPut" and the game connects "Output". The runtime takes 23.10's: every name the
// 1993 folio finds it finds too.
static bool same_name(const std::string& have, const char* want) {
    size_t i = 0;
    for (;; ++i) {
        unsigned char a = (unsigned char)(i < have.size() ? have[i] : 0), b = (unsigned char)want[i];
        if (std::toupper(a) != std::toupper(b)) return false;
        if (!a) return true;
    }
}

static const DspKnob* find_knob(const Template& t, const char* name, int* index = nullptr) {
    for (size_t i = 0; i < t.knobs.size(); ++i)
        if (same_name(t.knobs[i].name, name)) {
            if (index) *index = (int)i;
            return &t.knobs[i];
        }
    return nullptr;
}

// A resource of the template by name and type (0x8764; the names as same_name compares them).
static int find_rsrc(const Template& t, uint32_t type, const std::string& name) {
    for (size_t i = 0; i < t.rsrc.size(); ++i)
        if (t.rsrc[i].type == type && same_name(t.rsrc[i].name, name.c_str())) return (int)i;
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
    Instrument ins{0, tmpl, flags, 0, 0, {}, {}, {}};
    for (const auto& k : t.knobs)
        if (uint32_t err = tweak(c, ins, t, k, k.dflt, false)) return err;
    int32_t item = audio_item(INSTRUMENT_NODE, (uint8_t)pri);
    ins.self = item;
    PfDspTemplate dt{t.path, {}, {}, {}, t.code, t.imports};
    for (const auto& r : t.rsrc) {
        dt.names.push_back(r.name);
        dt.types.push_back(r.type);
        dt.counts.push_back(r.count);
    }
    pf_dsp_new(item, dt, (uint8_t)pri);
    for (auto [r, v] : ins.value) pf_dsp_write(item, r, v);
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

// A sample's bytes from its frames (0x9898) and its frames from its bytes (0x98d4): a frame is
// channels * width bytes, compressed by the ratio -- 1 as is, 2 a shift, else a division.
static uint32_t udiv(ArmCpu& c, uint32_t n, uint32_t d) {
    if (!d) pf_stop(c, "a sample's size divided by zero (the folio's division has no check)");
    return n / d;
}
static uint32_t frames_to_bytes(ArmCpu& c, const Sample& s, uint32_t frames) {
    uint32_t b = frames * s.channels * s.width;
    return s.compression_ratio == 1 ? b : s.compression_ratio == 2 ? b >> 1 : udiv(c, b, s.compression_ratio);
}
static uint32_t bytes_to_frames(ArmCpu& c, const Sample& s, uint32_t bytes) {
    uint32_t d = (uint32_t)s.channels * s.width;
    if (s.compression_ratio == 1) return udiv(c, bytes, d);
    if (s.compression_ratio == 2) return udiv(c, bytes, d) << 1;
    return udiv(c, s.compression_ratio * bytes, d);
}

// The folio's default tuning (0x68e8, the node at 0xc1c8 that its node +0x33c points at): twelve
// notes an octave from note 69 at 440 Hz, the frequencies of 69 to 80 at 0xc1fc (frac16).
static const uint32_t kTuning[12] = {
    0x1b80000, 0x1d229ec, 0x1ede220, 0x20b404a, 0x22a5d82, 0x24b545c,
    0x26e4104, 0x293414f, 0x2ba74db, 0x2e3fd25, 0x30ffdaa, 0x33e9c01,
};

// A note's frequency in that tuning (0x6980): its octave's entry, shifted up or down an octave at
// a time. Above, the folio stops at an index of 12 or less, so 93, 105 and the like read the word
// past the table (0xc22c, a variable of the folio's): not done here.
static uint32_t note_freq(ArmCpu& c, uint8_t note) {
    int n = note - 69, shift = 0;
    if (n >= 12) {
        do { ++shift; n -= 12; } while (n > 12);
        if (n == 12) pf_stop(c, "a sample's base note reads past the folio's tuning table");
        return kTuning[n] << shift;
    }
    if (n >= 0) return kTuning[n];
    do { ++shift; n += 12; } while (n < 0);
    return kTuning[n] >> shift;
}

// The sample's base frequency (0x396c): its base note's, times 44100 / its rate.
static void sample_base_freq(ArmCpu& c, Sample& s) {
    s.base_freq = mul_uf16(note_freq(c, s.basenote), div_uf16((uint32_t)kSampleRate << 16, s.rate));
}

// A sample's tags (0x347c, for creation and SetAudioItemInfo), each stored as it comes; a bad one
// returns at once, what came before it kept. WIDTH (0 to 2), CHANNELS (1 to 255) and NUMBITS (1 to
// 32, the width then its bytes rounded up) mean the frames are counted again from the bytes; FRAMES
// sets the bytes, NUMBYTES the frames, and given both they must agree. FRAMES, NUMBYTES and ADDRESS
// are refused for a delay line's (AF_ERR_SECURITY). Then a kernel check that the data lies below
// the top of memory (vector 40, 0x125c4), whose 0 or 1 the folio tests as an Err -- so never
// refuses; a sustain or release loop that begins past 0 must end at or before the last frame and
// not before it begins; and the base frequency again if the note or the rate came.
static uint32_t sample_set(ArmCpu& c, Sample& s, uint32_t tags) {
    bool refigure = false, have_frames = false, have_bytes = false, retune = false;
    uint32_t frames = 0, bytes = 0;
    for (uint32_t p = tags; p; p += 8) {
        uint32_t tag = pf_r32(p), v = pf_r32(p + 4);
        if (!tag) break;
        switch (tag) {
        case AF_TAG_NAME: case AF_TAG_SAMPLE: case AF_TAG_DELAY_LINE: break;
        case AF_TAG_WIDTH:
            if (v > 2) return AF_ERR_BADTAGVAL;
            s.width = (uint8_t)v;
            refigure = true;
            break;
        case AF_TAG_NUMBITS:
            if (v < 1 || v > 32) return AF_ERR_BADTAGVAL;
            s.numbits = (uint8_t)v;
            s.width = (uint8_t)((v + 7) >> 3);
            refigure = true;
            break;
        case AF_TAG_CHANNELS:
            if (v < 1 || v > 255) return AF_ERR_BADTAGVAL;
            s.channels = (uint8_t)v;
            refigure = true;
            break;
        case AF_TAG_FRAMES:
            if (s.flags & 2) return AF_ERR_SECURITY;
            frames = v;
            have_frames = true;
            break;
        case AF_TAG_NUMBYTES:
            if (s.flags & 2) return AF_ERR_SECURITY;
            bytes = v;
            have_bytes = true;
            break;
        case AF_TAG_ADDRESS:
            if (s.flags & 2) return AF_ERR_SECURITY;
            s.address = v;
            s.flags &= (uint8_t)~1u;
            break;
        case AF_TAG_BASENOTE: s.basenote = (uint8_t)v; retune = true; break;
        case AF_TAG_SAMPLE_RATE: s.rate = v; retune = true; break;
        case AF_TAG_DETUNE: s.detune = (uint8_t)v; break;
        case AF_TAG_LOWNOTE: s.lownote = (uint8_t)v; break;
        case AF_TAG_HIGHNOTE: s.highnote = (uint8_t)v; break;
        case AF_TAG_LOWVELOCITY: s.lowvelocity = (uint8_t)v; break;
        case AF_TAG_HIGHVELOCITY: s.highvelocity = (uint8_t)v; break;
        case AF_TAG_SUSTAINBEGIN: s.sustain_begin = (int32_t)v; break;
        case AF_TAG_SUSTAINEND: s.sustain_end = (int32_t)v; break;
        case AF_TAG_RELEASEBEGIN: s.release_begin = (int32_t)v; break;
        case AF_TAG_RELEASEEND: s.release_end = (int32_t)v; break;
        case AF_TAG_COMPRESSIONRATIO: s.compression_ratio = (uint8_t)v; break;
        case AF_TAG_COMPRESSIONTYPE: s.compression_type = v; break;
        default: return AF_ERR_BADTAG;
        }
    }
    if (have_frames) {
        s.frames = frames;
        s.numbytes = frames_to_bytes(c, s, frames);
        refigure = false;
    }
    if (have_bytes) {
        s.numbytes = bytes;
        s.frames = bytes_to_frames(c, s, bytes);
        refigure = false;
    }
    if (refigure) s.frames = bytes_to_frames(c, s, s.numbytes);
    if (have_frames && have_bytes && s.frames != frames) return AF_ERR_BADTAGVAL;
    int32_t last = (int32_t)s.frames;
    if (s.sustain_begin > 0 && (s.sustain_begin > s.sustain_end || s.sustain_end > last)) return AF_ERR_OUTOFRANGE;
    if (s.release_begin > 0 && (s.release_begin > s.release_end || s.release_end > last)) return AF_ERR_OUTOFRANGE;
    if (retune) sample_base_freq(c, s);
    return 0;
}

// A sample (0x3a3c): the kernel's item tags (vector 38, 0x1acf4: the name, priority, version,
// revision; the rest passed to the folio, which takes them all), the folio's defaults, a first
// look for AF_TAG_SAMPLE (another sample's info copied) and AF_TAG_DELAY_LINE (memory of the
// folio's), the frames from the bytes, the tags as SetAudioItemInfo takes them (an error there is
// the creation's), and the base frequency. An item tag would reach 0x347c and be AF_ERR_BADTAG
// there; neither they nor DELAY_LINE are done yet. AF_TAG_SAMPLE copies another sample's
// information (0x3b00: a delay line's refused, before and after, AF_ERR_SECURITY); the folio's
// check that the information may be read (vector -13) is not made.
// A sample's information as the folio keeps it, from +0x24 to +0x98 of its node: what
// AF_TAG_SAMPLE copies (0x3b00, the folio's bcopy of 0x74 bytes) and LoadSampleHere fills.
static void sample_info_write(uint32_t a, const Sample& s) {
    for (uint32_t k = 0x24; k < 0x98; k += 4) pf_w32(a + k, 0);
    pf_w32(a + 0x24, s.address);
    pf_w32(a + 0x28, s.frames);
    pf_w32(a + 0x2c, (uint32_t)s.sustain_begin);
    pf_w32(a + 0x30, (uint32_t)s.sustain_end);
    pf_w32(a + 0x34, (uint32_t)s.release_begin);
    pf_w32(a + 0x38, (uint32_t)s.release_end);
    pf_w32(a + 0x48, s.numbytes);
    pf_w32(a + 0x4c, s.base_freq);
    const uint8_t b[11] = {s.flags, s.numbits, s.width, s.channels, s.basenote, s.detune, s.lownote,
                           s.highnote, s.lowvelocity, s.highvelocity, s.compression_ratio};
    for (uint32_t k = 0; k < 11; ++k) pf_w8(a + 0x50 + k, b[k]);
    pf_w32(a + 0x60, s.data_offset);
    pf_w32(a + 0x64, s.ssnd_bytes);
    pf_w32(a + 0x68, s.compression_type);
    pf_w32(a + 0x8c, s.rate);
    pf_w32(a + 0x90, s.free_fn);
}
static void sample_info_read(uint32_t a, Sample& s) {
    s.address = pf_r32(a + 0x24);
    s.frames = pf_r32(a + 0x28);
    s.sustain_begin = (int32_t)pf_r32(a + 0x2c);
    s.sustain_end = (int32_t)pf_r32(a + 0x30);
    s.release_begin = (int32_t)pf_r32(a + 0x34);
    s.release_end = (int32_t)pf_r32(a + 0x38);
    s.numbytes = pf_r32(a + 0x48);
    s.base_freq = pf_r32(a + 0x4c);
    uint8_t* b[11] = {&s.flags, &s.numbits, &s.width, &s.channels, &s.basenote, &s.detune, &s.lownote,
                      &s.highnote, &s.lowvelocity, &s.highvelocity, &s.compression_ratio};
    for (uint32_t k = 0; k < 11; ++k) *b[k] = (uint8_t)pf_r8(a + 0x50 + k);
    s.data_offset = pf_r32(a + 0x60);
    s.ssnd_bytes = pf_r32(a + 0x64);
    s.compression_type = pf_r32(a + 0x68);
    s.rate = pf_r32(a + 0x8c);
    s.free_fn = pf_r32(a + 0x90);
}

static uint32_t create_sample(ArmCpu& c, const Tags& tags, uint32_t tag_ptr) {
    Sample s;
    for (auto [tag, v] : tags) {
        if (tag < 10 || tag >= 0xfe) pf_stop(c, "an item tag at a sample's creation: not yet");
        if (tag == AF_TAG_DELAY_LINE) pf_stop(c, "AF_TAG_DELAY_LINE: not yet");
        if (tag == AF_TAG_SAMPLE) {                 // another sample's information (0x3b00)
            if (s.flags & 2) return AF_ERR_SECURITY;
            sample_info_read(v, s);
            if (s.flags & 2) return AF_ERR_SECURITY;
        }
    }
    s.frames = bytes_to_frames(c, s, s.numbytes);
    if (tag_ptr)
        if (uint32_t err = sample_set(c, s, tag_ptr)) return err;
    sample_base_freq(c, s);
    int32_t item = audio_item(SAMPLE_NODE);
    g_samples[item] = s;
    if (g_pf_trace && tag_ptr)
        pf_log("        sample %d: %u frames, %u bytes at 0x%x, %u bits, rate 0x%x\n", item, s.frames, s.numbytes,
               s.address, (unsigned)s.numbits, s.rate);
    return (uint32_t)item;
}

// An attachment (0x5d44): the kernel's item tags (vector 38), then AF_TAG_INSTRUMENT,
// AF_TAG_SAMPLE (a sample, else AF_ERR_BADITEM), AF_TAG_ENVELOPE, AF_TAG_SET_FLAGS (bits 0 and 1
// only, else AF_ERR_BADTAGVAL; or'd in), AF_TAG_HOOKNAME (a copy of the name; 0 none),
// AF_TAG_START_AT; any other tag above 9 AF_ERR_BADTAG. The instrument must be an audio item: a
// template takes the attachment onto its own list, an instrument goes on, anything else is
// AF_ERR_BADITEM. With a sample: START_AT not below 0 and before the sample's last frame (any
// value if it has none), else AF_ERR_BADTAGVAL; the hook (0x8804) is the instrument's first FIFO
// (input or output, in the template's order) of that name, compared over 32 characters, or with
// no name its first one -- none: AF_ERR_NOFIFO; an output FIFO takes only a delay line's memory
// (0x7d68, else AF_ERR_SECURITY). Without a sample or an envelope the attachment is made all the
// same. Envelopes, templates and item tags are not done yet.
static uint32_t create_attachment(ArmCpu& c, const Tags& tags) {
    int32_t ins = -1, sample = -1;
    uint32_t flags = 0, start_at = 0, hook = 0;
    for (auto [tag, v] : tags) {
        if (tag == AF_TAG_INSTRUMENT) ins = (int32_t)v;
        else if (tag == AF_TAG_SAMPLE) {
            sample = (int32_t)v;
            if (!pf_check_item(sample, NST_AUDIO, SAMPLE_NODE)) return AF_ERR_BADITEM;
        } else if (tag == AF_TAG_ENVELOPE) pf_stop(c, "an envelope's attachment: not yet");
        else if (tag == AF_TAG_SET_FLAGS) {
            if (v & ~3u) return AF_ERR_BADTAGVAL;
            flags |= v;
        } else if (tag == AF_TAG_HOOKNAME) hook = v;
        else if (tag == AF_TAG_START_AT) start_at = v;
        else if (tag > 9) return AF_ERR_BADTAG;
        else pf_stop(c, "an item tag at an attachment's creation: not yet");
    }
    uint32_t n = pf_item_node(ins);
    if (!n || pf_r8(n + 8) != NST_AUDIO) return AF_ERR_BADITEM;
    if (pf_r8(n + 9) == TEMPLATE_NODE) pf_stop(c, "an attachment to a template: not yet");
    if (pf_r8(n + 9) != INSTRUMENT_NODE) return AF_ERR_BADITEM;
    int rsrc = -1;
    if (sample > 0) {
        const Sample& s = g_samples[sample];
        if ((int32_t)start_at < 0 || (start_at >= s.frames && s.frames)) return AF_ERR_BADTAGVAL;
        const Template& t = g_templates[g_instruments[ins].tmpl];
        std::string name = hook ? guest_string(hook) : std::string();
        for (size_t i = 0; i < t.rsrc.size() && rsrc < 0; ++i)
            if ((t.rsrc[i].type == RSRC_IFIFO || t.rsrc[i].type == RSRC_OFIFO) &&
                (!hook || !std::strncmp(t.rsrc[i].name.c_str(), name.c_str(), 32)))
                rsrc = (int)i;
        if (rsrc < 0) return AF_ERR_NOFIFO;
        if (t.rsrc[(size_t)rsrc].type == RSRC_OFIFO && !(s.flags & 2)) return AF_ERR_SECURITY;
        if (g_pf_trace) pf_log("        attach sample %d to instrument %d's \"%s\"\n", sample, ins, t.rsrc[(size_t)rsrc].name.c_str());
    }
    int32_t item = audio_item(ATTACHMENT_NODE);
    g_attachments[item] = {ins, sample, rsrc, flags, start_at, 0, 0};
    return (uint32_t)item;
}

static uint32_t create_cue(ArmCpu& c, const Tags& tags);     // with the audio clock, below
static void delete_cue(int32_t item);

// The folio's ir_Create: first the folio open (0x109c), then the node type's own routine.
static uint32_t create(ArmCpu& c, int type, const Tags& tags, uint32_t tag_ptr = 0) {
    if (!audio_open()) return AF_ERR_AUDIOCLOSED;
    switch (type) {
    case INSTRUMENT_NODE: return create_instrument(c, tags);
    case KNOB_NODE: return create_knob(c, tags);
    case SAMPLE_NODE: return create_sample(c, tags, tag_ptr);
    case CUE_NODE: return create_cue(c, tags);
    case ATTACHMENT_NODE: return create_attachment(c, tags);
    default: {
        char why[80];
        std::snprintf(why, sizeof why, "CreateItem of the audio folio's node type %d: not yet", type);
        pf_stop(c, why);
    }
    }
}

static uint32_t audio_create(ArmCpu& c, int type, uint32_t tags) { return create(c, type, read_tags(tags), tags); }

// ---- starting and stopping ------------------------------------------------------------------
// The states the folio keeps, and what it does to the DSP: its instruments in or out of the DSP's
// program, and the DMA of their FIFOs (pf_dsp.cpp). A chunk is given as the folio gives CLIO an
// address and a count (the count register holds the bytes less 4; here the bytes).

// A sample's bytes from its frames, as 0x9898 (frames_to_bytes) -- a ratio of 0, which the
// folio's division would not survive and no program sets, as 1.
static uint32_t bytes_of(const Sample& s, uint32_t frames) {
    uint32_t b = frames * s.channels * s.width;
    return s.compression_ratio <= 1 ? b : s.compression_ratio == 2 ? b >> 1 : b / s.compression_ratio;
}
// Where an attachment starts in its sample's data (0x7450): AF_TAG_START_AT's frames in.
static uint32_t start_addr(const Attachment& at, const Sample& s) {
    return at.start_at ? s.address + bytes_of(s, at.start_at) : s.address;
}
static bool is_attachment(int32_t a) { return a && pf_check_item(a, NST_AUDIO, ATTACHMENT_NODE); }

// What is linked after an attachment, forgetting a link to what is no longer one (0x795c).
static int32_t linked(Attachment& at) {
    if (at.next && !is_attachment(at.next)) at.next = 0;
    return at.next;
}

// 0x6578: the FIFO's interrupt armed to signal the daemon when the chunk now playing runs out --
// when the attachment has a cue (+0x44, MonitorAttachment), AF_ATTF_FATLADYSINGS (2) or a link.
static void arm(const Attachment& at) {
    if (at.cue || (at.flags & 2) || at.next) pf_dsp_dma_arm(at.ins, (uint32_t)at.rsrc);
}

// 0x76b8: the FIFO's next chunk the folio's silence.
static void next_silence(int32_t ins, int rsrc) { pf_dsp_dma_next(ins, (uint32_t)rsrc, PF_DSP_SILENCE, 32); }

// 0x76d0: after a release, the FIFO's next chunk -- the sample's release loop, or else what
// follows its sustain loop to its end (the silence when that is under 8 bytes); with neither loop,
// nothing.
static void next_after_release(int32_t ins, int rsrc, const Sample& s) {
    uint32_t addr, bytes;
    if (s.release_begin >= 0) {
        addr = s.address + bytes_of(s, (uint32_t)s.release_begin);
        bytes = bytes_of(s, (uint32_t)(s.release_end - s.release_begin));
    } else if (s.sustain_begin >= 0) {
        addr = s.address + bytes_of(s, (uint32_t)s.sustain_end);
        bytes = bytes_of(s, s.frames - (uint32_t)s.sustain_end);
    } else return;
    if (bytes < 8) next_silence(ins, rsrc);
    else pf_dsp_dma_next(ins, (uint32_t)rsrc, addr, bytes);
}

// 0x7860: an attachment queued as its FIFO's next chunk -- from where it starts to the end of its
// sustain loop, the loop waiting behind it (or the same with its release loop), or to its end.
static void queue_next(const Attachment& at) {
    const Sample& s = g_samples[at.sample];
    uint32_t from = start_addr(at, s);
    if (s.sustain_begin >= 0 || s.release_begin >= 0) {
        int32_t b = s.sustain_begin >= 0 ? s.sustain_begin : s.release_begin;
        int32_t e = s.sustain_begin >= 0 ? s.sustain_end : s.release_end;
        pf_dsp_dma_waiting(at.ins, (uint32_t)at.rsrc, s.address + bytes_of(s, (uint32_t)b), bytes_of(s, (uint32_t)(e - b)));
        pf_dsp_dma_next(at.ins, (uint32_t)at.rsrc, from, bytes_of(s, (uint32_t)e - at.start_at));
    } else pf_dsp_dma_next(at.ins, (uint32_t)at.rsrc, from, bytes_of(s, s.frames - at.start_at));
}

// An attachment started (0x74b8): its sample must have an address (else AF_ERR_NULLADDRESS) and 4
// bytes or more (else AF_ERR_OUTOFRANGE); it is its FIFO's (the folio's table, +0x18), and the
// FIFO's DMA set (0x9d24): with a sustain loop (or else a release loop) the sample to the loop's
// end and then the loop, over and over; with neither the sample to its end and then the silence --
// or what is linked after it (0x795c, 0x7860) -- and the interrupt armed. It is playing.
static uint32_t attachment_start(int32_t a) {
    Attachment& at = g_attachments[a];
    const Sample& s = g_samples[at.sample];
    if (!s.address) return AF_ERR_NULLADDRESS;
    if (s.numbytes < 4) return AF_ERR_OUTOFRANGE;
    g_instruments[at.ins].playing[at.rsrc] = a;
    uint32_t from = start_addr(at, s), r = (uint32_t)at.rsrc;
    bool loops = s.sustain_begin >= 0 || s.release_begin >= 0;
    if (loops) {
        int32_t b = s.sustain_begin >= 0 ? s.sustain_begin : s.release_begin;
        int32_t e = s.sustain_begin >= 0 ? s.sustain_end : s.release_end;
        pf_dsp_dma(at.ins, r, from, bytes_of(s, (uint32_t)e - at.start_at), s.address + bytes_of(s, (uint32_t)b),
                   bytes_of(s, (uint32_t)(e - b)));
    } else {
        pf_dsp_dma(at.ins, r, from, bytes_of(s, s.frames - at.start_at), PF_DSP_SILENCE, 32);
        if (int32_t n = linked(at)) queue_next(g_attachments[n]);
        arm(at);
    }
    at.state = 3;
    return 0;
}

// An attachment stopped (0x7cf8): when it is playing, the FIFO's waiting chunk and interrupt
// dropped (0x6648), its DMA off (0x9e34), the FIFO no one's, and it is stopped.
static void attachment_stop(int32_t a) {
    Attachment& at = g_attachments[a];
    if (at.state <= 1) return;
    pf_dsp_dma_quiet(at.ins, (uint32_t)at.rsrc);
    pf_dsp_dma_stop(at.ins, (uint32_t)at.rsrc);
    g_instruments[at.ins].playing.erase(at.rsrc);
    at.state = 1;
}

// An instrument's DSP side stopped (0x7be8), when it runs: out of the DSP's program, and every
// FIFO's playing attachment stopped, while it is still an attachment (the envelopes' attachments,
// a list of their own, are not made here).
static void dsp_stop(Instrument& ins) {
    if (ins.dsp_state <= 1) return;
    pf_dsp_run(ins.self, false);
    ins.dsp_state = 1;
    std::map<int, int32_t> was = ins.playing;
    for (auto [rsrc, a] : was)
        if (pf_check_item(a, NST_AUDIO, ATTACHMENT_NODE)) attachment_stop(a);
}

// swi 0x40003: Err StopInstrument(Item instrument, TagArg* tags) -- 0x1ddc: an instrument (else
// AF_ERR_BADITEM), no tags (else AF_ERR_BADTAG); neither the folio's open nor the owner asked.
// Started, it is stopped, its DSP side with it; AF_INSF_AUTOABANDON leaves it abandoned (0).
static uint32_t stop_instrument(int32_t item, uint32_t tags) {
    if (!pf_check_item(item, NST_AUDIO, INSTRUMENT_NODE)) return AF_ERR_BADITEM;
    if (tags) return AF_ERR_BADTAG;
    Instrument& ins = g_instruments[item];
    if (ins.state <= 1) return 0;
    ins.state = 1;
    dsp_stop(ins);
    if (ins.flags & AF_INSF_AUTOABANDON) ins.state = 0;
    return 0;
}
static void a_stopinstrument(ArmCpu& c) {
    pf_dsp_sync();
    c.r[0] = stop_instrument((int32_t)c.r[0], c.r[1]);
}

// An attachment released (0x79a0): its sample must have an address (else AF_ERR_NULLADDRESS); a
// link to what is no longer an attachment is forgotten (0x795c). With a release loop, that is the
// FIFO's next chunk (0x76d0). Else, when something is linked after it: a sustain loop beginning
// past frame 0 and ending before the sample does leaves the rest of the sample next and the link
// waiting behind it (0x76d0, 0x7810); otherwise the link is next (0x7860). With no link: the
// rest of the sample next and the silence waiting (0x65e8), or the silence next (0x76b8). Then the
// interrupt armed (0x6578), and the attachment released.
static uint32_t attachment_release(int32_t a) {
    Attachment& at = g_attachments[a];
    const Sample& s = g_samples[at.sample];
    if (!s.address) return AF_ERR_NULLADDRESS;
    bool rest = s.sustain_end != (int32_t)s.frames;
    int32_t n = linked(at);
    if (s.release_begin >= 0) next_after_release(at.ins, at.rsrc, s);
    else if (n) {
        if (s.sustain_begin > 0 && rest) {
            next_after_release(at.ins, at.rsrc, s);
            const Attachment& l = g_attachments[n];
            const Sample& ls = g_samples[l.sample];
            pf_dsp_dma_waiting(l.ins, (uint32_t)l.rsrc, start_addr(l, ls), bytes_of(ls, ls.frames - l.start_at));
        } else queue_next(g_attachments[n]);
    } else if (s.sustain_begin > 0 && rest) {
        next_after_release(at.ins, at.rsrc, s);
        pf_dsp_dma_waiting(at.ins, (uint32_t)at.rsrc, PF_DSP_SILENCE, 32);
    } else next_silence(at.ins, at.rsrc);
    arm(at);
    at.state = 2;
    return 0;
}

// 0x7788: a linked attachment taking over its FIFO (the table's +0x18) as the one before it ends:
// with no loop in its sample, what is linked after it queued (0x795c, 0x7860) or the silence
// (0x76b8), and the interrupt armed; it is playing.
static void take_over(int32_t a) {
    Attachment& at = g_attachments[a];
    const Sample& s = g_samples[at.sample];
    g_instruments[at.ins].playing[at.rsrc] = a;
    if (s.sustain_begin < 0 && s.release_begin < 0) {
        if (int32_t n = linked(at)) queue_next(g_attachments[n]);
        else next_silence(at.ins, at.rsrc);
        arm(at);
    }
    at.state = 3;
}

static uint32_t stop_instrument(int32_t item, uint32_t tags);

// The folio's daemon on an armed FIFO's signal (0x5cc0): the attachment that was the FIFO's, while
// it is still an attachment, is no longer, and ends (0x5c04): stopped; with AF_ATTF_FATLADYSINGS
// its instrument stopped (StopInstrument), else what is linked after it takes over (0x7788), or
// with no link the FIFO plays the silence (0x7674: the current chunk and the next); then its cue
// (+0x44) signalled, while it is still a cue (else forgotten).
static void fifo_ended(int32_t ins, uint32_t rsrc) {
    auto i = g_instruments.find(ins);
    if (i == g_instruments.end()) return;
    auto p = i->second.playing.find((int)rsrc);
    if (p == i->second.playing.end()) return;
    int32_t a = p->second;
    i->second.playing.erase(p);
    if (!is_attachment(a)) return;
    Attachment& at = g_attachments[a];
    at.state = 1;
    if (at.flags & 2) stop_instrument(at.ins, 0);
    else if (int32_t n = linked(at)) take_over(n);
    else pf_dsp_dma(ins, rsrc, PF_DSP_SILENCE, 32, PF_DSP_SILENCE, 32);
    if (at.cue) signal_cue(at.cue);
    if (g_pf_trace >= 2) pf_log("        attachment %d ends on instrument %d\n", a, ins);
}

// swi 0x40002: Err ReleaseInstrument(Item instrument, TagArg* tags) -- 0x1d54: an instrument
// (else AF_ERR_BADITEM), no tags (else AF_ERR_BADTAG); neither the folio's open nor the owner
// asked. Started, it is released, and its DSP side with it (0x7b1c): on each of its FIFOs, in the
// template's order, the attachment playing there, while it is still an attachment, released --
// the first error ends the round and is the call's result; then the envelopes' attachments (none
// made here); its DSP side's state becomes 2.
static uint32_t release_instrument(int32_t item, uint32_t tags) {
    if (!pf_check_item(item, NST_AUDIO, INSTRUMENT_NODE)) return AF_ERR_BADITEM;
    if (tags) return AF_ERR_BADTAG;
    Instrument& ins = g_instruments[item];
    if (ins.state <= 2) return 0;
    ins.state = 2;
    uint32_t err = 0;
    std::map<int, int32_t> was = ins.playing;
    for (auto [rsrc, a] : was)
        if (pf_check_item(a, NST_AUDIO, ATTACHMENT_NODE) && (int32_t)(err = attachment_release(a)) < 0) break;
    ins.dsp_state = 2;
    return err;
}
static void a_releaseinstrument(ArmCpu& c) {
    pf_dsp_sync();
    c.r[0] = release_instrument((int32_t)c.r[0], c.r[1]);
}

// ---- deleting items (the folio's ir_Delete, 0x1170, by node type) -----------------------------
// A knob (0x27a8): off its instrument's list of grabbed knobs (RemNode), and 0. An instrument
// (0x2294): every knob grabbed on it (the node's list at +0x34, in the order grabbed) deleted as
// by its owner (vector 34, 0x6a6c), then 0x8dc4: its DSP side stopped, every attachment on each
// of its FIFOs deleted the same way, then a list at +0x9c of its private data (nothing the
// programs run so far put there), its DSP resources and memory freed; and off its template's
// list while the template is an item. The results of those deletions are not read. An attachment
// (0x61cc): if it is playing, its instrument stopped (StopInstrument, whose result is the
// deletion's) and it too, then off its hook's list and its sample's "SampleRefs". FreeInstrument and ReleaseKnob are DeleteItem in the 1993 lib,
// DetachSample the folio's own glue for it. A sample (0x3d18): every attachment made with it
// stopped, then each deleted as by its owner (the first error is the deletion's, the rest left);
// a delay line's memory freed; then off the folio's "AudioSamples" -- unless its word at +0x40
// is above 0 (AF_ERR_INUSE; 0 here, as nothing changes it). An instrument that was a connection's source stays
// one in the instruments it fed, as in the folio, whose patched code goes on reading the freed
// DSP memory.
static int32_t audio_delete(ArmCpu& c, int type, int32_t item, uint32_t) {
    pf_dsp_sync();
    switch (type) {
    case TEMPLATE_NODE: {
        // 0x24b0: unless its word at +0x24 is above 0 (0 from its creation, 0x230c, and nothing
        // here changes it), the attachments made to the template (its DSP side's list at +0x2c;
        // none are made here) and then its instruments (the node's list at +0x34, in the order
        // made: AddTail) deleted as by their owners until one fails (0x6a6c); its DSP side and
        // memory freed, and off the folio's list of templates. 0 whatever happened.
        std::vector<int32_t> doomed;
        for (const auto& [i, v] : g_instruments)
            if (v.tmpl == item) doomed.push_back(i);
        for (int32_t i : doomed)
            if (pf_delete_item_as_owner(c, i) < 0) break;
        g_templates.erase(item);
        return 0;
    }
    case KNOB_NODE: g_knobs.erase(item); return 0;
    case INSTRUMENT_NODE: {
        std::vector<int32_t> doomed;
        for (const auto& [k, v] : g_knobs)
            if (v.ins == item) doomed.push_back(k);
        for (int32_t k : doomed) pf_delete_item_as_owner(c, k);
        dsp_stop(g_instruments[item]);
        doomed.clear();
        for (const auto& [a, v] : g_attachments)
            if (v.ins == item) doomed.push_back(a);
        for (int32_t a : doomed) pf_delete_item_as_owner(c, a);
        pf_dsp_delete(item);
        g_instruments.erase(item);
        return 0;
    }
    case SAMPLE_NODE: {
        std::vector<int32_t> refs;
        for (const auto& [a, v] : g_attachments)
            if (v.sample == item) refs.push_back(a);
        for (int32_t a : refs) attachment_stop(a);
        int32_t err = 0;
        for (int32_t a : refs)
            if ((err = pf_delete_item_as_owner(c, a)) < 0) break;
        if (g_samples[item].flags & 2) pf_stop(c, "deleting a delay line's sample: not yet");
        g_samples.erase(item);
        return err;
    }
    case CUE_NODE: delete_cue(item); return 0;
    case ATTACHMENT_NODE: {
        int32_t err = 0;
        if (pf_check_item(g_attachments[item].ins, NST_AUDIO, INSTRUMENT_NODE) && g_attachments[item].state > 1) {
            err = (int32_t)stop_instrument(g_attachments[item].ins, 0);
            attachment_stop(item);
        }
        g_attachments.erase(item);
        return err;
    }
    default: {
        char why[80];
        std::snprintf(why, sizeof why, "DeleteItem of the audio folio's node type %d: not yet", type);
        pf_stop(c, why);
    }
    }
}

// ---- the calls ------------------------------------------------------------------------------
// audio -4: Item LoadInsTemplate(char* name, Item aux) -- 0x1610: the folio open; aux must be 0;
// the file parsed as IFF (iffParseFile, in the caller's task, so from its current directory) and
// a template made of its FORM DSPP (CreateItem(MKNODEID(AUDIONODE, AUDIO_TEMPLATE_NODE),
// {AF_TAG_TEMPLATE, the parsed form}).
//
// 23.10's folio (its own AUDIOFOLIO, on the disc) opens an IFF file (0xb530) the same way for a
// name with a directory in it, and a bare name from the current directory first; failing that, from
// its own directory (AudioFolio +0x344, a copy of its -d option: "$audio" by default, 0x4c4) and in
// it the subdirectory the name's extension names (0xb070) -- "mixer2x2.dsp" from $audio/dsp, or
// from $audio when there is no such subdirectory. The 1993 folio opens the name as given, so this
// finds only what it would not. (LoadSample's stream still opens the name as given: not yet.)
static std::string iff_host_path(const std::string& name) {
    std::string host = pf_host_path(name.c_str());
    if (!host.empty() || name.find('/') != std::string::npos) return host;
    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos) {
        host = pf_host_path(("$audio/" + name.substr(dot + 1) + "/" + name).c_str());
        if (!host.empty()) return host;
    }
    return pf_host_path(("$audio/" + name).c_str());
}

// The subroutines an instrument imports (a resource of type 0x8000: sampler.dsp's OscUpDownFP),
// for the DSP's interpreter: each from the file named after it in lower case, "oscupdownfp.dsp",
// found as above, which exports it (type 0x4000, the resource's count its offset in the code).
// That is where the library keeps them; how the folio finds them is not read. One not found is
// left out, and the interpreter will not run the instrument.
static void load_imports(Template& t) {
    for (size_t i = 0; i < t.rsrc.size(); ++i) {
        if (t.rsrc[i].type != 0x8000) continue;
        std::string file = t.rsrc[i].name;
        for (char& ch : file) ch = (char)std::tolower((unsigned char)ch);
        std::string host = iff_host_path(file + ".dsp");
        if (host.empty()) continue;
        std::ifstream f(host, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        Template sub;
        if (!parse_dsp(d, sub)) continue;
        for (const auto& r : sub.rsrc)
            if (r.type == 0x4000 && same_name(r.name, t.rsrc[i].name.c_str()))
                t.imports[(uint32_t)i] = {sub.code, r.count};
    }
}

static void a_loadinstemplate(ArmCpu& c) {
    if (!audio_open()) { c.r[0] = AF_ERR_AUDIOCLOSED; return; }
    if (c.r[1]) { c.r[0] = AF_ERR_BADITEM; return; }
    std::string name = guest_string(c.r[0]), host = iff_host_path(name);
    if (g_pf_trace) pf_log("        LoadInsTemplate \"%s\" -> %s\n", name.c_str(), host.empty() ? "(none)" : host.c_str());
    if (host.empty()) pf_stop(c, "LoadInsTemplate: no such file (the folio's error for it: not yet read)");
    std::ifstream f(host, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    Template t;
    t.path = name;
    if (!parse_dsp(d, t)) pf_stop(c, "LoadInsTemplate: not a DSP instrument this runtime can read");
    load_imports(t);
    c.r[0] = (uint32_t)make_template(std::move(t));
}

// audio -92: Err UnloadInsTemplate(Item template) -- 0x1578: a template (else AF_ERR_BADITEM);
// each attachment made to it detached (a sample's, 0x3c78, or an envelope's, 0x5768; a hook of
// another kind ends it with AF_ERR_BADITEM) -- none are made here --; then DeleteItem, whose
// result is the call's.
static void a_unloadinstemplate(ArmCpu& c) {
    if (!pf_check_item((int32_t)c.r[0], NST_AUDIO, TEMPLATE_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    c.r[0] = (uint32_t)pf_delete_item(c, (int32_t)c.r[0]);
}

// audio -8: Item AllocInstrument(Item template, uint8 priority) -- 0x1af0:
// CreateItem(MKNODEID(AUDIONODE, AUDIO_INSTRUMENT_NODE), {AF_TAG_TEMPLATE, AF_TAG_PRIORITY}).
static void a_allocinstrument(ArmCpu& c) {
    c.r[0] = create(c, INSTRUMENT_NODE, {{AF_TAG_TEMPLATE, c.r[0]}, {AF_TAG_PRIORITY, c.r[1] & 0xFF}});
}

// audio -40: Item LoadInstrument(char* name, Item aux, uint8 priority) -- 0x14ec:
// LoadInsTemplate(name, aux), and when that gives a template, AllocInstrument(template,
// priority); else its error.
static void a_loadinstrument(ArmCpu& c) {
    uint32_t priority = c.r[2] & 0xFF;
    a_loadinstemplate(c);
    if ((int32_t)c.r[0] < 0) return;
    c.r[1] = priority;
    a_allocinstrument(c);
}

// audio -96: Err UnloadInstrument(Item instrument) -- 1993's 0x151c, 23.10's 0x116c, the same: an
// instrument (else AF_ERR_BADITEM); its template kept, the instrument deleted (DeleteItem), and
// when that went well UnloadInsTemplate of the template, whose result is the call's.
static void a_unloadinstrument(ArmCpu& c) {
    int32_t ins = (int32_t)c.r[0];
    if (!pf_check_item(ins, NST_AUDIO, INSTRUMENT_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    int32_t tmpl = g_instruments[ins].tmpl;
    int32_t err = pf_delete_item(c, ins);
    if (err < 0) { c.r[0] = (uint32_t)err; return; }
    c.r[0] = (uint32_t)tmpl;
    a_unloadinstemplate(c);
}

// ---- an AIFF file into a sample (LoadSample) -----------------------------------------------
// The folio's IFF reader (iffParseFile, 0xa6d8), in the caller's task: the file opened as a
// stream (OpenDiskStream(name, 0); the folio first changes to the name's directory and opens the
// rest, and changes back -- here the whole path is opened, as that comes to the same file), the
// FORM read (0xa500), its chunks (0xa0ec, 0xa188) handed to AUDIOFOLIO's AIFF handlers (0x2f64,
// 0x2fe8), the stream closed. Every read is of an even count (0xaa50: one more byte when odd) and
// must fit what is left of the enclosing chunk; after a handler the rest of its chunk is skipped
// to the chunk's end, made even (0xab04). The IFF errors: 0xE18BF101 not a FORM, 0xE18BF102 the
// file not opened, 0xE18BF105 a read past what is left.
enum : uint32_t {
    IFF_ERR_NOTFORM = 0xE18BF101u, IFF_ERR_OPEN = 0xE18BF102u, IFF_ERR_SHORT = 0xE18BF105u,
    AF_ERR_BADFILETYPE = 0xD52BF114u, AF_ERR_NOMEM = 0xD52BF006u,
    AF_ERR_TOOMANYMARKERS = 0xD52BF111u, AF_ERR_NOMARKER = 0xD52BF112u,
};
static uint32_t id4(const char* s) { return (uint32_t)s[0] << 24 | (uint32_t)s[1] << 16 | (uint32_t)s[2] << 8 | (uint32_t)s[3]; }
static uint32_t be16(uint32_t a) { return pf_r8(a) << 8 | pf_r8(a + 1); }

struct IffRead {
    const ArmCpu& c;
    uint32_t below, st;
    int32_t left;                               // what is left of the enclosing chunk
    uint32_t buf;                               // a chunk's bytes, on the caller's stack
    bool load = true;                           // the sample's data loaded (ScanSample: not)
};
static int32_t iff_read(IffRead& r, uint32_t dst, int32_t n) {
    if (n & 1) ++n;
    if (r.left - n < 0) return (int32_t)IFF_ERR_SHORT;
    int32_t got = pf_stream_read(r.c, r.below, r.st, dst, n);
    r.left -= n;
    return got;
}
static int32_t iff_skip(IffRead& r, int32_t n) {
    int32_t pos = pf_stream_seek(r.c, r.below, r.st, 0, 2), to = pos + n;
    if (to & 1) ++to;
    n = to - pos;
    if (r.left - n < 0) return (int32_t)IFF_ERR_SHORT;
    int32_t got = pf_stream_seek(r.c, r.below, r.st, n, 2);
    r.left -= n;
    return got;
}

// The 80-bit float of a COMM chunk as 16.16 (0x9f9c): 0 for 0; 0xFFFFFFFF for an infinity or
// anything 2^16 and up; else the mantissa's top word shifted down by 0x400e less the exponent
// (an ARM shift by a register: its low byte, 32 or more giving 0), negated for the sign.
static uint32_t ext80_fix16(uint32_t a) {
    uint32_t e = (pf_r8(a) & 0x7f) << 8 | pf_r8(a + 1), hi = pf_r32(a + 2), lo = pf_r32(a + 6);
    uint32_t v;
    if (!e && !hi && !lo) v = 0;
    else if (e == 0x7fff || (int32_t)(e - 0x400e) > 0) v = 0xFFFFFFFFu;
    else {
        uint32_t sh = (0x400e - e) & 0xff;
        v = sh >= 32 ? 0 : hi >> sh;
    }
    return pf_r8(a) & 0x80 ? 0u - v : v;
}

// The folio's AllocMem (1993's 0xb264, 23.10's 0xa048, the same code): n + 4 bytes from the task's
// lists, n in the first word, the word after it back; 0 when there is no room.
static uint32_t folio_alloc_mem(uint32_t n) {
    uint32_t p = pf_alloc_mem(pf_r32(pf_current_task() + T_FREEMEMORYLISTS), (int32_t)n + 4, MEMTYPE_DMA, true);
    if (p) { pf_w32(p, n); p += 4; }
    return p;
}

struct AiffMarkers { uint32_t count = 0; uint32_t id[32] = {}, pos[32] = {}; };

// AUDIOFOLIO's handler of a chunk of the AIFF form (0x2fe8). The chunk's bytes are read first,
// when there are some and no more than 500 (SSND reads its own); what reads more is skipped and
// its handler would look at what the stack held before -- stopped here instead.
static int32_t aiff_chunk(IffRead& r, Sample& s, AiffMarkers& mk, bool aifc, uint32_t alloc_fn, uint32_t id,
                          int32_t size) {
    const ArmCpu& c = r.c;
    uint32_t b = r.buf;
    bool known = id == id4("COMM") || id == id4("INST") || id == id4("MARK");
    if (id != id4("SSND") && size > 0) {
        int32_t got = size > 0x1f4 ? iff_skip(r, size) : iff_read(r, b, size);
        if (got < 0) return got;
    }
    if (known && (size <= 0 || size > 0x1f4))
        pf_stop(const_cast<ArmCpu&>(c), "an AIFF chunk the folio reads from a stale buffer: not done");
    if (id == id4("COMM")) {                                        // 0x31c4
        uint32_t w0 = pf_r32(b), w1 = pf_r32(b + 4);
        s.channels = (uint8_t)(w0 >> 16);
        s.frames = (w0 & 0xffff) << 16 | w1 >> 16;
        s.numbits = (uint8_t)w1;
        s.width = (uint8_t)(((s.numbits + 7) & 0xff) >> 3);
        s.rate = ext80_fix16(b + 8);
        if (aifc) s.compression_type = (pf_r32(b + 16) & 0xffff) << 16 | pf_r32(b + 20) >> 16;
        uint32_t t = s.compression_type;
        if (!t) s.compression_ratio = 1;
        else if (t == id4("ADP4")) s.compression_ratio = 4;
        else if (t == id4("SDX2")) s.compression_ratio = 2;
        else if (t == id4("SDX3")) s.compression_ratio = 3;
        else {
            s.compression_ratio = (uint8_t)((t & 0xff) - '0');
            if (s.compression_ratio < 1 || s.compression_ratio > 9) s.compression_ratio = 2;
        }
    } else if (id == id4("MARK")) {                                 // 0x32bc
        mk.count = pf_r32(b) >> 16;
        if ((int32_t)mk.count > 32) return (int32_t)AF_ERR_TOOMANYMARKERS;
        uint32_t p = b + 2;
        for (uint32_t i = 0; i < mk.count; ++i) {
            mk.id[i] = be16(p);
            mk.pos[i] = be16(p + 2) << 16 | be16(p + 4);
            p += pf_r8(p + 6) + 7;
            if (p & 1) ++p;
        }
    } else if (id == id4("INST")) {                                 // 0x3350
        s.basenote = (uint8_t)pf_r8(b);
        s.detune = (uint8_t)pf_r8(b + 1);
        s.lownote = (uint8_t)pf_r8(b + 2);
        s.highnote = (uint8_t)pf_r8(b + 3);
        s.lowvelocity = (uint8_t)pf_r8(b + 4);
        s.highvelocity = (uint8_t)pf_r8(b + 4);                     // the folio's: the low velocity's byte again
        auto at = [&](uint32_t mid, int32_t& out) {
            for (uint32_t i = 0; i < mk.count; ++i)
                if (mk.id[i] == mid) { out = (int32_t)mk.pos[i]; return true; }
            return false;
        };
        uint32_t w8 = pf_r32(b + 8), w12 = pf_r32(b + 12), w16 = pf_r32(b + 16);
        if (!(w8 >> 16)) s.sustain_begin = s.sustain_end = -1;
        else if (!at(w8 & 0xffff, s.sustain_begin) || !at(w12 >> 16, s.sustain_end)) return (int32_t)AF_ERR_NOMARKER;
        if (!(w12 & 0xffff)) s.release_begin = s.release_end = -1;
        else if (!at(w16 >> 16, s.release_begin) || !at(w16 & 0xffff, s.release_end)) return (int32_t)AF_ERR_NOMARKER;
    } else if (id == id4("SSND")) {                                 // 0x30ec
        int32_t got = iff_read(r, b, 8);
        (void)got;
        int32_t n = size - 8 - (int32_t)pf_r32(b);
        s.ssnd_bytes = (uint32_t)n;
        if (r.load) s.numbytes = (uint32_t)n;                       // the data is loaded
        else n = (int32_t)s.numbytes;                               // else a buffer of the size asked
        s.data_offset = (uint32_t)iff_skip(r, (int32_t)pf_r32(b));
        if (n <= 0) {
            int32_t e = iff_skip(r, (int32_t)s.ssnd_bytes);
            return e < 0 ? e : 0;
        }
        uint32_t p;
        if (alloc_fn) {
            p = pf_guest_call(c, alloc_fn, (uint32_t)n + 4, 0x100000);
            if (p) { pf_w32(p, (uint32_t)n); p += 4; }
        } else {
            p = folio_alloc_mem((uint32_t)n);
        }
        if (!p) return (int32_t)AF_ERR_NOMEM;
        s.address = p;
        s.flags |= 1;
        int32_t e = r.load ? iff_read(r, p, n) : iff_skip(r, (int32_t)s.ssnd_bytes);
        if (e < 0) return e;
    }
    return 0;                                                       // APPL, FVER and the rest
}

static uint32_t parse_aiff(ArmCpu& c, uint32_t name, Sample& s, uint32_t alloc_fn, bool load = true) {
    const uint32_t below = 0xb00;
    uint32_t st = pf_stream_open(c, below, name, 0);
    if (!st) return IFF_ERR_OPEN;
    IffRead r{c, below, st, 8, c.r[13] - 0xa00, load};
    AiffMarkers mk;
    int32_t err = 0;
    uint32_t h = r.buf + 0x200;
    if ((err = iff_read(r, h, 8)) >= 0) {
        uint32_t size = pf_r32(h + 4);
        if (pf_r32(h) != id4("FORM")) err = (int32_t)IFF_ERR_NOTFORM;
        else {
            if (size & 1) ++size;
            r.left = (int32_t)size;
            if ((err = iff_read(r, h, 4)) >= 0) {
                uint32_t type = pf_r32(h);
                bool aifc = type == id4("AIFC");
                if (!aifc && type != id4("AIFF")) err = (int32_t)AF_ERR_BADFILETYPE;
                else {
                    err = 0;
                    while (r.left > 0 && err >= 0) {                // 0xa188
                        if ((err = iff_read(r, h, 8)) < 0) break;
                        uint32_t id = pf_r32(h);
                        int32_t csize = (int32_t)pf_r32(h + 4);
                        if (id == id4("FORM") || id == id4("XREF"))
                            pf_stop(c, "an AIFF file with a FORM or XREF inside: not yet");
                        int32_t end = pf_stream_seek(c, below, st, 0, 2) + csize;
                        if (end & 1) ++end;
                        if ((err = aiff_chunk(r, s, mk, aifc, alloc_fn, id, csize)) < 0) break;
                        int32_t now = pf_stream_seek(c, below, st, 0, 2);
                        if (now != end) err = iff_skip(r, end - now);
                    }
                }
            }
        }
    }
    pf_stream_close(c, below, st);
    return err < 0 ? (uint32_t)err : 0;
}

// audio -12: Item LoadSample(char* name) -- 0x2878: LoadSampleHere(name, 0, 0) (0x2884): its
// allocator and free function both given or neither (else AF_ERR_OUTOFRANGE); the sample's
// information at its defaults (0x2c04), the AIFF file read into it (0x2d50), its data in memory
// from the caller's lists (AllocMem, MEMTYPE_DMA: a word of its size first) or the allocator; then
// CreateItem(sample, {AF_TAG_NAME name, AF_TAG_SAMPLE the information}), from the caller's stack.
// An error after the data's memory was had leaves it had, as the folio does.
static uint32_t load_sample_here(ArmCpu& c, uint32_t name, uint32_t alloc_fn, uint32_t free_fn) {
    if ((alloc_fn == 0) != (free_fn == 0)) return AF_ERR_OUTOFRANGE;
    Sample s;
    uint32_t err = parse_aiff(c, name, s, alloc_fn);
    if (g_pf_trace) pf_log("        LoadSample \"%s\" -> 0x%x\n", guest_string(name).c_str(), err);
    if ((int32_t)err < 0) return err;
    s.free_fn = free_fn;
    uint32_t tags = c.r[13] - 32 - 0xb0, info = tags + 0x18;
    sample_info_write(info, s);
    const uint32_t t[5] = {AF_TAG_NAME, name, AF_TAG_SAMPLE, info, 0};
    for (int k = 0; k < 5; ++k) pf_w32(tags + 4u * k, t[k]);
    return audio_create(c, SAMPLE_NODE, tags);
}
static void a_loadsample(ArmCpu& c) { c.r[0] = load_sample_here(c, c.r[0], 0, 0); }

// audio -48: Item ScanSample(char* name, int32 bufferSize) -- 1993's 0x2b78 (23.10's 0x2c5c makes
// it by tags, the size as tag 0x44): the AIFF read for its information as LoadSample reads it, but
// at its SSND chunk the data is not loaded -- a buffer of bufferSize bytes (the folio's AllocMem)
// when that is above 0, its size the sample's -- and the sample made of that and its name.
static void a_scansample(ArmCpu& c) {
    uint32_t name = c.r[0];
    Sample s;
    s.numbytes = c.r[1];
    uint32_t err = parse_aiff(c, name, s, 0, false);
    if (g_pf_trace) pf_log("        ScanSample \"%s\" -> 0x%x\n", guest_string(name).c_str(), err);
    if ((int32_t)err < 0) { c.r[0] = err; return; }
    uint32_t tags = c.r[13] - 32 - 0xb0, info = tags + 0x18;
    sample_info_write(info, s);
    const uint32_t t[5] = {AF_TAG_NAME, name, AF_TAG_SAMPLE, info, 0};
    for (int k = 0; k < 5; ++k) pf_w32(tags + 4u * k, t[k]);
    c.r[0] = audio_create(c, SAMPLE_NODE, tags);
}

// audio -44: Err UnloadSample(Item sample) -- 0x3c78: a sample (else AF_ERR_BADITEM); deleted; and
// when that went well and its data is the folio's (bit 0), the data given back -- to the free
// function it was loaded with (the address less 4 and the size word plus 4), or the folio's FreeMem
// (0xb2d4: the size before the address, to the caller's lists).
static void a_unloadsample(ArmCpu& c) {
    int32_t item = (int32_t)c.r[0];
    if (!pf_check_item(item, NST_AUDIO, SAMPLE_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    const Sample s = g_samples[item];
    int32_t err = pf_delete_item(c, item);
    if (s.address && !err && (s.flags & 1)) {
        uint32_t p = s.address - 4, size = pf_r32(p);
        if (s.free_fn) pf_guest_call(c, s.free_fn, p, size + 4);
        else if ((int32_t)size > 0)
            pf_free_mem(pf_r32(pf_current_task() + T_FREEMEMORYLISTS), p, (int32_t)size + 4);
        else pf_stop(c, "UnloadSample: data of the system's lists: not yet");
    }
    c.r[0] = (uint32_t)err;
}

// audio -56: Item MakeSample(uint32 numBytes, TagArg* tags) -- 1993's 0x29e4, 23.10's 0x27d4: a
// sample at the folio's defaults; with numBytes above 0, that many bytes of the folio's AllocMem
// for its data (AF_TAG_ADDRESS, AF_TAG_NUMBYTES, flags bit 0: the folio's, which UnloadSample gives
// back); the tags applied as SetAudioItemInfo applies them (sample_set; their error is MakeSample's,
// the memory given back); then the item made of that info, as AF_TAG_SAMPLE makes one. The two
// folios take the memory and the tags in opposite orders -- 1993 the tags first, then refusing
// (AF_ERR_BADTAGVAL) a NUMBYTES other than numBytes; 23.10 the memory first, keeping it aside when a
// tag then gives another ADDRESS -- so a tag that gives either with numBytes above 0 stops the
// run: not yet.
static void a_makesample(ArmCpu& c) {
    uint32_t n = c.r[0], tag_ptr = c.r[1];
    if (n && tag_ptr)
        for (auto [tag, v] : read_tags(tag_ptr)) {
            (void)v;
            if (tag == AF_TAG_ADDRESS || tag == AF_TAG_NUMBYTES)
                pf_stop(c, "MakeSample: numBytes and a tag that gives the address or the size: not yet");
        }
    Sample s;
    if (n) {
        if (!(s.address = folio_alloc_mem(n))) { c.r[0] = AF_ERR_NOMEM; return; }
        s.flags |= 1;
        s.numbytes = n;
    }
    if (tag_ptr)
        if (uint32_t err = sample_set(c, s, tag_ptr)) {
            if (s.address) pf_free_mem(pf_r32(pf_current_task() + T_FREEMEMORYLISTS), s.address - 4, (int32_t)n + 4);
            c.r[0] = err;
            return;
        }
    uint32_t tags = c.r[13] - 32 - 0xb0, info = tags + 0x18;
    sample_info_write(info, s);
    const uint32_t t[3] = {AF_TAG_SAMPLE, info, 0};
    for (int k = 0; k < 3; ++k) pf_w32(tags + 4u * k, t[k]);
    c.r[0] = audio_create(c, SAMPLE_NODE, tags);
}

// audio -16: Item GrabKnob(Item instrument, char* name) -- 0x2508:
// CreateItem(MKNODEID(AUDIONODE, AUDIO_KNOB_NODE), {AF_TAG_NAME, AF_TAG_INSTRUMENT}).
static void a_grabknob(ArmCpu& c) {
    c.r[0] = create(c, KNOB_NODE, {{AF_TAG_NAME, c.r[1]}, {AF_TAG_INSTRUMENT, c.r[0]}});
}

// audio -144: Item AttachSample(Item instrument, Item sample, char* hook) -- 0x3e34:
// CreateItem(MKNODEID(AUDIONODE, AUDIO_ATTACHMENT_NODE), {AF_TAG_INSTRUMENT, AF_TAG_SAMPLE, and
// AF_TAG_HOOKNAME when hook is not 0}). audio -148: Err DetachSample(Item attachment) -- 0x3e88:
// DeleteItem.
static void a_attachsample(ArmCpu& c) {
    Tags t = {{AF_TAG_INSTRUMENT, c.r[0]}, {AF_TAG_SAMPLE, c.r[1]}};
    if (c.r[2]) t.push_back({AF_TAG_HOOKNAME, c.r[2]});
    c.r[0] = create(c, ATTACHMENT_NODE, t);
}
static void a_detachsample(ArmCpu& c) { c.r[0] = (uint32_t)pf_delete_item(c, (int32_t)c.r[0]); }

// swi 0x40015: Err LinkAttachments(Item at1, Item at2) -- 0x63d4: at1 an attachment (else
// AF_ERR_BADITEM), at2 0 or an attachment (else the same); at2 is what plays after at1. Neither
// the folio's open nor the owner is asked. When at1 is playing, at2 is queued as its FIFO's next
// chunk (0x7860), or with no at2 the silence (0x76b8), and at1's interrupt armed (0x6578).
static void a_linkattachments(ArmCpu& c) {
    pf_dsp_sync();
    int32_t a1 = (int32_t)c.r[0], a2 = (int32_t)c.r[1];
    if (!pf_check_item(a1, NST_AUDIO, ATTACHMENT_NODE) || (a2 && !pf_check_item(a2, NST_AUDIO, ATTACHMENT_NODE))) {
        c.r[0] = AF_ERR_BADITEM;
        return;
    }
    Attachment& at = g_attachments[a1];
    at.next = a2;
    if (at.state > 1) {
        if (a2) queue_next(g_attachments[a2]);
        else next_silence(at.ins, at.rsrc);
        arm(at);
    }
    if (g_pf_trace) pf_log("        attachment %d then %d\n", a1, a2);
    c.r[0] = 0;
}

// swi 0x40012: Err StartAttachment(Item attachment, TagArg* tags) -- 1993's 0x62c0, 23.10's 0x60d4:
// an attachment (else AF_ERR_BADITEM), no tags (else AF_ERR_BADTAG), then started (0x74b8),
// whatever its instrument is doing. (23.10's start, 0x7488, told it comes from here, also resets the
// FIFO -- CLIO 0x03400300 and the FIFO's DSP word, 0x97dc -- before it sets the current and next
// chunks; the runtime's DMA set plays the current chunk from its start anyway.) swi 0x40013
// ReleaseAttachment (0x631c, 23.10's 0x611c) and swi 0x40014 StopAttachment (0x6378, 23.10's
// 0x6160) check the same and release (0x79a0) or stop (0x7cf8, 0) it.
static int32_t attachment_call(ArmCpu& c) {
    pf_dsp_sync();
    if (!pf_check_item((int32_t)c.r[0], NST_AUDIO, ATTACHMENT_NODE)) { c.r[0] = AF_ERR_BADITEM; return 0; }
    if (c.r[1]) { c.r[0] = AF_ERR_BADTAG; return 0; }
    return (int32_t)c.r[0];
}
static void a_startattachment(ArmCpu& c) {
    if (int32_t a = attachment_call(c)) c.r[0] = attachment_start(a);
}
static void a_releaseattachment(ArmCpu& c) {
    if (int32_t a = attachment_call(c)) c.r[0] = attachment_release(a);
}
static void a_stopattachment(ArmCpu& c) {
    if (int32_t a = attachment_call(c)) {
        attachment_stop(a);
        c.r[0] = 0;
    }
}

// swi 0x40000: Err TweakKnob(Item knob, int32 value) -- 0x27c8, and swi 0x40011 TweakRawKnob
// (0x281c), the same without the knob's calculation. Neither asks whether the folio is open.
static void tweak_knob(ArmCpu& c, bool cooked) {
    pf_dsp_sync();
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
// knob. Its DSP side, when running, is stopped first (before the tags, so a bad tag leaves it
// stopped). Then on each of its FIFOs, in the template's order, the first attachment (in the order
// made) not marked AF_ATTF_NOAUTOSTART starts -- whether it can is not the call's result -- and
// its DSP side runs; its node's state becomes 3.
static void a_startinstrument(ArmCpu& c) {
    pf_dsp_sync();
    if (!audio_open()) { c.r[0] = AF_ERR_AUDIOCLOSED; return; }
    int32_t item = (int32_t)c.r[0];
    if (!pf_check_item(item, NST_AUDIO, INSTRUMENT_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    Instrument& ins = g_instruments[item];
    const Template& t = g_templates[ins.tmpl];
    dsp_stop(ins);
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
    for (size_t i = 0; i < t.rsrc.size(); ++i) {
        if (t.rsrc[i].type != RSRC_IFIFO && t.rsrc[i].type != RSRC_OFIFO) continue;
        for (const auto& [a, v] : g_attachments)
            if (v.ins == item && v.rsrc == (int)i && !(v.flags & AF_ATTF_NOAUTOSTART)) {
                uint32_t err = attachment_start(a);
                if (g_pf_trace) pf_log("        attachment %d starts on \"%s\" -> 0x%x\n", a, t.rsrc[i].name.c_str(), err);
                break;
            }
    }
    pf_dsp_run(item, true);
    ins.dsp_state = 3;
    ins.state = 3;
    ins.start_time = pf_r32(pf_folio_base(PF_AUDIO) + 0x9c);    // 23.10's 0x1974: the folio's time (AF_TIME)
    c.r[0] = 0;
}

// swi 0x40008: Err ConnectInstruments(Item src, char* srcName, Item dst, char* dstName) -- 0x1c04
// and 0x8018: both instruments; the source's variable of that name, the destination's variable
// or else knob of that name, or AF_ERR_BADNAME. The folio patches the destination's code to read
// the source's variable; here the connection is written down.
static void a_connectinstruments(ArmCpu& c) {
    pf_dsp_sync();
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
    pf_dsp_connect(src, (uint32_t)a, dst, (uint32_t)b);
    c.r[0] = 0;
}

// swi 0x4000c: Err DisconnectInstruments(Item src, char* srcName, Item dst, char* dstName) --
// 0x1cac and 0x8130: both instruments (else AF_ERR_BADITEM), the names as ConnectInstruments
// finds them (else AF_ERR_BADNAME); then every place in the destination's code that reads that
// resource is put back as it was loaded (0xc000) -- whatever fed it, and whether anything did.
static void a_disconnectinstruments(ArmCpu& c) {
    pf_dsp_sync();
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
    if (g_pf_trace) pf_log("        disconnect %d \"%s\" -> %d \"%s\"\n", src, from.c_str(), dst, to.c_str());
    if (a < 0 || b < 0) { c.r[0] = AF_ERR_BADNAME; return; }
    d.inputs.erase(std::remove_if(d.inputs.begin(), d.inputs.end(),
                                  [&](const Connection& k) { return k.to_rsrc == (uint32_t)b; }),
                   d.inputs.end());
    pf_dsp_disconnect(dst, (uint32_t)b);
    c.r[0] = 0;
}

// audio -36: Err GetAudioItemInfo(Item item, TagArg* tags) -- 1993's 0x1324: an item of the
// folio's (else AF_ERR_BADITEM), then by its type: a sample's tags (0x37d8), a knob's (0x25f8), an
// attachment's (0x6074), any other 0xD52BF118. A sample's: each tag's value word replaced by the
// field it names -- WIDTH, CHANNELS, FRAMES, BASENOTE, DETUNE (signed), LOW/HIGHNOTE,
// LOW/HIGHVELOCITY, SUSTAIN/RELEASE BEGIN/END, NUMBYTES, ADDRESS, SAMPLE_RATE, COMPRESSIONRATIO,
// COMPRESSIONTYPE, NUMBITS, 51 the base frequency, 55 where the data begins in the file, 56 the
// SSND chunk's bytes; an item tag (up to 9) passed over; any other AF_ERR_BADTAG, the tags before
// it answered. (Knobs and attachments: not yet.)
static uint32_t sample_get(const Sample& s, uint32_t tags) {
    for (uint32_t p = tags; p; p += 8) {
        uint32_t tag = pf_r32(p), v;
        if (!tag) break;
        switch (tag) {
        case 22: v = s.width; break;
        case 23: v = s.channels; break;
        case 24: v = s.frames; break;
        case 25: v = s.basenote; break;
        case 26: v = (uint32_t)(int32_t)(int8_t)s.detune; break;
        case 27: v = s.lownote; break;
        case 28: v = s.highnote; break;
        case 29: v = s.lowvelocity; break;
        case 30: v = s.highvelocity; break;
        case 31: v = (uint32_t)s.sustain_begin; break;
        case 32: v = (uint32_t)s.sustain_end; break;
        case 33: v = (uint32_t)s.release_begin; break;
        case 34: v = (uint32_t)s.release_end; break;
        case 35: v = s.numbytes; break;
        case 36: v = s.address; break;
        case 46: v = s.rate; break;
        case 47: v = s.compression_ratio; break;
        case 48: v = s.compression_type; break;
        case 49: v = s.numbits; break;
        case 51: v = s.base_freq; break;
        case 55: v = s.data_offset; break;
        case 56: v = s.ssnd_bytes; break;
        default:
            if (tag > 9) return AF_ERR_BADTAG;
            continue;
        }
        pf_w32(p + 4, v);
    }
    return 0;
}

// An instrument's (23.10's 0x1884; the 1993 folio's answer is 0xD52BF118): AF_TAG_PRIORITY (39)
// the node's priority, AF_TAG_STATUS (60) its state (0 abandoned, 1 stopped, 2 released, 3
// started), AF_TAG_START_TIME (62) the folio's time when it last started; an item tag (up to 9)
// passed over; any other AF_ERR_BADTAG, the tags before it answered.
static uint32_t instrument_get(uint32_t node, const Instrument& ins, uint32_t tags) {
    for (uint32_t p = tags; p; p += 8) {
        uint32_t tag = pf_r32(p), v;
        if (!tag) break;
        if (tag == AF_TAG_PRIORITY) v = pf_r8(node + 10);
        else if (tag == 60) v = (uint32_t)ins.state;
        else if (tag == 62) v = ins.start_time;
        else if (tag > 9) return AF_ERR_BADTAG;
        else continue;
        pf_w32(p + 4, v);
    }
    return 0;
}

static void a_getaudioiteminfo(ArmCpu& c) {
    int32_t item = (int32_t)c.r[0];
    uint32_t n = pf_item_node(item);
    if (!n || pf_r8(n + 8) != NST_AUDIO) { c.r[0] = AF_ERR_BADITEM; return; }
    switch (pf_r8(n + 9)) {
    case SAMPLE_NODE: c.r[0] = sample_get(g_samples[item], c.r[1]); break;
    case INSTRUMENT_NODE: c.r[0] = instrument_get(n, g_instruments[item], c.r[1]); break;
    case KNOB_NODE: case ATTACHMENT_NODE: pf_stop(c, "GetAudioItemInfo of a knob or an attachment: not yet");
    default: c.r[0] = 0xD52BF118u;
    }
}

// swi 0x4001b: Err SetAudioItemInfo(Item item, TagArg* tags) -- 0x120c: the folio open; an item
// of the folio's (LocateItem, else AF_ERR_BADITEM); the kernel's check of 8 bytes at the tags
// (vector 40 again, never refusing); then by its node type: a sample's tags (0x347c), an
// envelope's (0x4fac), an attachment's (0x6088), a tuning's (0x6754); a template, an instrument,
// a knob or a cue AF_ERR_UNIMPLEMENTED. For a number that names no item 23.10's folio (0xc54)
// says AF_ERR_BADITEM; the 1993 folio goes on with a null node and reads its type at address 9,
// where the console has its kernel's SWI vector, a branch (0xEA00xxxx): type 0, which also ends
// in AF_ERR_BADITEM (0x1310). Immercenary asks it of item -1 (p, 0x2643c).
static void a_setaudioiteminfo(ArmCpu& c) {
    if (!audio_open()) { c.r[0] = AF_ERR_AUDIOCLOSED; return; }
    int32_t item = (int32_t)c.r[0];
    uint32_t n = pf_item_node(item);
    if (!n) { c.r[0] = AF_ERR_BADITEM; return; }
    if (pf_r8(n + 8) != NST_AUDIO) { c.r[0] = AF_ERR_BADITEM; return; }
    switch (pf_r8(n + 9)) {
    case TEMPLATE_NODE: case INSTRUMENT_NODE: case KNOB_NODE: case CUE_NODE: c.r[0] = AF_ERR_UNIMPLEMENTED; break;
    case SAMPLE_NODE: {
        Sample& s = g_samples[item];
        c.r[0] = sample_set(c, s, c.r[1]);
        if (g_pf_trace)
            pf_log("        sample %d: %u frames, %u bytes at 0x%x, %u bits, %u channel(s), note %u, rate 0x%x, "
                   "sustain %d to %d, base frequency 0x%x -> 0x%x\n", item, s.frames, s.numbytes, s.address,
                   (unsigned)s.numbits, (unsigned)s.channels, (unsigned)s.basenote, s.rate, s.sustain_begin,
                   s.sustain_end, s.base_freq, c.r[0]);
        break;
    }
    case ATTACHMENT_NODE: {
        // 0x6088 (23.10's 0x5ea8, the same): AF_TAG_SET_FLAGS and _CLEAR_FLAGS (41), bits 0 and 1
        // only (else AF_ERR_BADTAGVAL); AF_TAG_START_AT not below 0 and before the sample's last
        // frame (else AF_ERR_BADTAGVAL); the item tags (up to 9) passed over, any other AF_ERR_BADTAG.
        // A tag before the one that fails has taken.
        Attachment& a = g_attachments[item];
        c.r[0] = 0;
        for (auto [tag, v] : read_tags(c.r[1])) {
            if (tag == AF_TAG_SET_FLAGS || tag == 41) {
                if (v & ~3u) { c.r[0] = AF_ERR_BADTAGVAL; break; }
                a.flags = tag == AF_TAG_SET_FLAGS ? a.flags | v : a.flags & ~v;
            } else if (tag == AF_TAG_START_AT) {
                uint32_t frames = a.sample > 0 ? g_samples[a.sample].frames : 0;
                if ((int32_t)v < 0 || v >= frames) { c.r[0] = AF_ERR_BADTAGVAL; break; }
                a.start_at = v;
            } else if (tag > 9) { c.r[0] = AF_ERR_BADTAG; break; }
        }
        break;
    }
    case 6: case 8: pf_stop(c, "SetAudioItemInfo of an envelope or tuning: not yet");
    default: c.r[0] = AF_ERR_BADITEM;
    }
}

// ---- the audio clock --------------------------------------------------------------------------
// The DSP counts sample frames down from head.dsp's CountDown knob and interrupts at 0; the
// folio's handler (0x3e90, the FIRQ "AudioTimer" on interrupt 11) adds 1 to the audio time and,
// when a wake-up is due, signals its daemon, which runs the timer list (0x460c). Ticks are
// GetAudioDuration() frames of 44,100 Hz apart: 184 from the start (the daemon's SetAudioRate of
// 240 Hz, 0x45b0); a new duration counts from the next tick, as the DSP reloads its counter only
// at 0. The folio keeps its clock in its node, and so does the runtime: the time (+0x9c), the
// semaphores that guard its timer list (+0xd0) and its rate (+0xd8, the clock's owner), the
// duration (+0xe0). Not made: head.dsp's instrument and its CountDown knob (+0xd4), which the
// folio tweaks with each new duration -- a tweak that cannot fail or clamp within the durations
// SetAudioDuration lets through.
enum : uint32_t {
    AF_TIME = 0x9c, AF_TIMERLIST = 0xb0, AF_LISTSEM = 0xd0, AF_RATESEM = 0xd8, AF_DURATION = 0xe0,
    AF_ERR_INUSE = 0xD52BF10Fu,
};
static const uint32_t kDefaultRate = 240u << 16;
static uint64_t g_tick_frame;                   // the sample frame of the next tick, from the boot

static uint32_t folio() { return pf_folio_base(PF_AUDIO); }

// ---- the timer list and cues -------------------------------------------------------------------
// The timer list (+0xb0, "AudioTimer") holds nodes by the audio time they wait for: +0x24 the time,
// +0x28 the function the daemon calls, +0x2c the list while the node is on it (0 off it). The
// earliest wake-up is at +0xa0, wanted while +0xac is set. A cue (0x469c) is such a node with a
// signal of its maker's (+0x30) and its maker's task (+0x34, the Task); its function (0x4600) is
// the kernel's own SendSignal of that signal to that task. The folio keeps the function's address
// in the node, as here; the cue's is the only one run so far.
enum : uint32_t {
    AF_WAKE = 0xa0, AF_WAKEWANTED = 0xac,
    TN_TIME = 0x24, TN_FN = 0x28, TN_LIST = 0x2c, CUE_SIGNAL = 0x30, CUE_TASK = 0x34,
    kCueFn = 0x4600, AF_ERR_NOSIGNAL = 0xD52BF116u, AF_ERR_NOTOWNER = 0xD52BF11Cu,
};

// What the daemon does when signalled (0x460c): from the head, every node whose time has come (at
// or before now, unsigned) is taken off, marked off, and its function called; then the next
// wake-up is the head's time, when there is a head.
static void run_timers() {
    uint32_t f = folio(), now = pf_r32(f + AF_TIME), end = f + AF_TIMERLIST + PF_LIST_TAIL;
    for (uint32_t n = pf_r32(f + AF_TIMERLIST + PF_LIST_HEAD); n != end && pf_r32(n + TN_TIME) <= now;) {
        uint32_t next = pf_r32(n);
        pf_list_rem_node(n);
        pf_w32(n + TN_LIST, 0);
        if (pf_r32(n + TN_FN) == kCueFn) pf_signal(pf_r32(n + CUE_TASK), pf_r32(n + CUE_SIGNAL));
        n = next;
    }
    uint32_t head = pf_r32(f + AF_TIMERLIST + PF_LIST_HEAD);
    if (head != end) {
        pf_w32(f + AF_WAKE, pf_r32(head + TN_TIME));
        pf_w32(f + AF_WAKEWANTED, 1);
    }
}

// A node onto the list for `time` (0x40f8): one already on it is AF_ERR_INUSE. The wake-up moves
// to `time` when none is wanted or `time` comes first (as a signed difference); the node goes in
// before the first whose time is not earlier (the kernel's UniversalInsertNode, 0x150b8, with the
// folio's 0x40dc: before the first m for which m's time less the new one's is not negative), else
// at the end.
static uint32_t timer_add(uint32_t n, uint32_t time) {
    uint32_t f = folio(), list = f + AF_TIMERLIST;
    if (pf_r32(n + TN_LIST)) return AF_ERR_INUSE;
    if (!pf_r32(f + AF_WAKEWANTED) || (int32_t)(pf_r32(f + AF_WAKE) - time) > 0) pf_w32(f + AF_WAKE, time);
    pf_w32(f + AF_WAKEWANTED, 1);
    pf_w32(n + TN_TIME, time);
    uint32_t m = pf_r32(list + PF_LIST_HEAD);
    while (m != list + PF_LIST_TAIL && (int32_t)(pf_r32(m + TN_TIME) - time) < 0) m = pf_r32(m);
    pf_list_insert_before(m, n);
    pf_w32(n + TN_LIST, list);
    return 0;
}

// A cue (0x469c): the kernel's item tags (vector 38; the folio's own part of the walk, 0x6b68,
// takes anything), then any tag above 9 is AF_ERR_BADTAG; a signal of the caller's
// (AllocSignal(0), else AF_ERR_NOSIGNAL), the caller's task, off the list, the cue's function.
// No program run so far gives tags; item tags stop.
static uint32_t create_cue(ArmCpu& c, const Tags& tags) {
    for (auto [tag, v] : tags) {
        (void)v;
        if (tag <= 9) pf_stop(c, "an item tag at a cue's creation: not yet");
    }
    if (!tags.empty()) return AF_ERR_BADTAG;
    uint32_t sig = pf_alloc_signal(0);
    if (!sig) return AF_ERR_NOSIGNAL;
    int32_t item = audio_item(CUE_NODE);
    uint32_t n = pf_item_node(item);
    pf_w32(n + CUE_SIGNAL, sig);
    pf_w32(n + CUE_TASK, pf_current_task());
    pf_w32(n + TN_LIST, 0);
    pf_w32(n + TN_FN, kCueFn);
    return (uint32_t)item;
}

// A cue deleted (0x4798): with a kernel of version 0x13 or below -- the 1993 one is version 0 --
// its signal freed when the task deleting it owns it (FreeSignal, the deleter's own bits; its
// result unread), else left; off the list when on it; its signal word 0. With a later kernel
// (KernelBase's n_Version; Doctor Hauzer's AUDIOFOLIO 20.27, 0x4818) it is freed in its owner's
// task whoever deletes it -- LookupItem of the owner, and when there is one the kernel's FreeSignal
// of that task's bits (its result unread) -- as when a task is deleted with the cues it owns.
static void delete_cue(int32_t item) {
    uint32_t n = pf_item_node(item);
    if (pf_r8(pf_folio_base(PF_KERNEL) + 0x14) > 0x13) {
        if (uint32_t owner = pf_item_node((int32_t)pf_r32(n + 28)))
            pf_free_signal(pf_r32(n + CUE_SIGNAL), owner);
    } else if (pf_r32(n + 28) == task_item()) {
        pf_free_signal(pf_r32(n + CUE_SIGNAL));
    }
    if (pf_r32(n + TN_LIST)) {
        pf_list_rem_node(n);
        pf_w32(n + TN_LIST, 0);
    }
    pf_w32(n + CUE_SIGNAL, 0);
}

// An attachment's cue at its end (0x5c88): while it is a cue, its task sent its signal; else the
// attachment forgets it.
static void signal_cue(int32_t& cue) {
    if (uint32_t n = pf_check_item(cue, NST_AUDIO, CUE_NODE)) pf_signal(pf_r32(n + CUE_TASK), pf_r32(n + CUE_SIGNAL));
    else cue = 0;
}

// swi 0x40016: Err MonitorAttachment(Item attachment, Item cue, int32 index) -- 1993's 0x64e4,
// 23.10's 0x62ac: an attachment (else AF_ERR_BADITEM); no cue forgets the one it had; else a cue
// (else AF_ERR_BADITEM) at CUE_AT_END (-2), the only index taken (else 0xD52BF118), kept with it.
// It is signalled when the attachment ends; a FIFO armed before is not armed again here, as in
// the folio.
static void a_monitorattachment(ArmCpu& c) {
    int32_t a = (int32_t)c.r[0], cue = (int32_t)c.r[1], index = (int32_t)c.r[2];
    if (!pf_check_item(a, NST_AUDIO, ATTACHMENT_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    Attachment& at = g_attachments[a];
    c.r[0] = 0;
    if (!cue) { at.cue = 0; return; }
    if (!pf_check_item(cue, NST_AUDIO, CUE_NODE)) { c.r[0] = AF_ERR_BADITEM; return; }
    if (index != -2) { c.r[0] = 0xD52BF118u; return; }
    at.cue = cue;
    at.cue_index = index;
}

// swi 0x4000d: Err SignalAtTime(Item cue, AudioTime time) -- 0x41d8: the folio open; a cue (else
// AF_ERR_BADITEM) of the caller's (n_Owner, else AF_ERR_NOTOWNER); onto the timer list.
static uint32_t signal_at_time(int32_t cue, uint32_t time) {
    if (!audio_open()) return AF_ERR_AUDIOCLOSED;
    uint32_t n = pf_check_item(cue, NST_AUDIO, CUE_NODE);
    if (!n) return AF_ERR_BADITEM;
    if (pf_r32(n + 28) != task_item()) return AF_ERR_NOTOWNER;
    return timer_add(n, time);
}
static void a_signalattime(ArmCpu& c) { c.r[0] = signal_at_time((int32_t)c.r[0], c.r[1]); }

// audio -72: int32 GetCueSignal(Item cue) -- 0x428c: its signal, 0 when it is not a cue.
static uint32_t cue_signal(int32_t cue) {
    uint32_t n = pf_check_item(cue, NST_AUDIO, CUE_NODE);
    return n ? pf_r32(n + CUE_SIGNAL) : 0;
}
static void a_getcuesignal(ArmCpu& c) { c.r[0] = cue_signal((int32_t)c.r[0]); }

// audio -68: Err SleepUntilTime(Item cue, AudioTime time) -- 0x42bc, in the caller's task:
// SignalAtTime, whose error is the call's; then WaitSignal of the cue's signal, and 0.
static void a_sleepuntiltime(ArmCpu& c) {
    int32_t cue = (int32_t)c.r[0];
    uint32_t err = signal_at_time(cue, c.r[1]);
    if ((int32_t)err < 0) { c.r[0] = err; return; }
    pf_wait_signal(cue_signal(cue));
    c.r[0] = 0;
}

// audio -20: Err SleepAudioTicks(int32 ticks) -- Doctor Hauzer's AUDIOFOLIO 20.27, 0x4408, in the
// caller's task: CreateItem of a cue (MKNODEID(AUDIONODE, AUDIO_CUE_NODE), no tags), whose error
// is the call's; SleepUntilTime of it at the folio's time (+0x9c) plus ticks (0x43d0, as -68
// below); DeleteItem of the cue; SleepUntilTime's result.
static void a_sleepaudioticks(ArmCpu& c) {
    uint32_t ticks = c.r[0];
    int32_t cue = (int32_t)audio_create(c, CUE_NODE, 0);
    if (cue < 0) { c.r[0] = (uint32_t)cue; return; }
    c.r[0] = (uint32_t)cue;
    c.r[1] = pf_r32(folio() + AF_TIME) + ticks;
    a_sleepuntiltime(c);
    uint32_t err = c.r[0];
    pf_delete_item(c, cue);
    c.r[0] = err;
}

// The folio's FIRQ (0x3e90): the time up by 1; when a wake-up is wanted and has come (a signed
// difference), it signals the daemon (+0xa8, the bits at +0xa4) and wants none. The daemon, at the
// folio's high priority, then runs the list; here the list is run at once.
static void tick(uint64_t) {
    pf_dsp_sync();
    pf_w32(folio() + AF_TIME, pf_r32(folio() + AF_TIME) + 1);
    uint32_t f = folio();
    if (pf_r32(f + AF_WAKEWANTED) && (int32_t)(pf_r32(f + AF_WAKE) - pf_r32(f + AF_TIME)) <= 0) {
        pf_w32(f + AF_WAKEWANTED, 0);
        run_timers();
    }
    g_tick_frame += pf_r32(folio() + AF_DURATION);
    pf_at((g_tick_frame * 1000000000ull + 44099) / 44100, tick);
}

// swi 0x40010: Err SetAudioDuration(Item owner, uint32 frames) -- 0x435c: the owner must be the
// clock's semaphore (else AF_ERR_INUSE); 44 to 32767 frames (else AF_ERR_OUTOFRANGE).
static uint32_t set_duration(uint32_t owner, uint32_t frames) {
    if (owner != pf_r32(folio() + AF_RATESEM)) return AF_ERR_INUSE;
    if (frames < 0x2c || frames > 0x7fff) return AF_ERR_OUTOFRANGE;
    pf_w32(folio() + AF_DURATION, frames);
    return 0;
}
static void a_setaudioduration(ArmCpu& c) { c.r[0] = set_duration(c.r[0], c.r[1]); }

// swi 0x4000f: Err SetAudioRate(Item owner, frac16 rate) -- 0x43fc: the duration 44100 / rate,
// rounded (as frac16, then (q + 0x8000) >> 16, arithmetic), then SetAudioDuration's checks.
static uint32_t set_rate(uint32_t owner, uint32_t rate) {
    uint32_t q = div_uf16(44100u << 16, rate);
    return set_duration(owner, (uint32_t)((int32_t)(q + 0x8000u) >> 16));
}
static void a_setaudiorate(ArmCpu& c) { c.r[0] = set_rate(c.r[0], c.r[1]); }

// audio -60: frac16 GetAudioRate(void) -- 0x4450: 44100 / the duration, as frac16.
static void a_getaudiorate(ArmCpu& c) { c.r[0] = div_uf16(44100u << 16, pf_r32(folio() + AF_DURATION) << 16); }
// audio -64: uint32 GetAudioDuration(void) -- 0x4540.
static void a_getaudioduration(ArmCpu& c) { c.r[0] = pf_r32(folio() + AF_DURATION); }
// audio -168: AudioTime GetAudioTime(void) -- 0x4440.
static void a_getaudiotime(ArmCpu& c) { c.r[0] = pf_r32(folio() + AF_TIME); }

// audio -76: Item OwnAudioClock(void) -- 0x448c: the folio open; LockItem of the rate's semaphore
// without waiting: the semaphore's item, else AF_ERR_INUSE.
static void a_ownaudioclock(ArmCpu& c) {
    if (!audio_open()) { c.r[0] = AF_ERR_AUDIOCLOSED; return; }
    uint32_t sem = pf_r32(folio() + AF_RATESEM);
    c.r[0] = pf_lock_item((int32_t)sem, 0) > 0 ? sem : AF_ERR_INUSE;
}

// audio -80: Err DisownAudioClock(Item owner) -- 0x4510: the rate's semaphore (else
// AF_ERR_BADITEM), unlocked.
static void a_disownaudioclock(ArmCpu& c) {
    uint32_t sem = pf_r32(folio() + AF_RATESEM);
    c.r[0] = c.r[0] != sem ? AF_ERR_BADITEM : (uint32_t)pf_unlock_item((int32_t)sem);
}

// What the folio's start (0x3f08) and its daemon's (0x4550) leave: the time at 0, the timer list,
// the two semaphores, the clock at 240 Hz, and the first tick a duration from the boot.
static void clock_init() {
    uint32_t f = folio();
    pf_w32(f + AF_TIME, 0);
    pf_w32(f + AF_LISTSEM, (uint32_t)pf_semaphore_new("AFTimerListSem4"));
    pf_w32(f + AF_RATESEM, (uint32_t)pf_semaphore_new("AFTimerRateSem4"));
    pf_list_init(f + AF_TIMERLIST, "AudioTimer");
    pf_w32(f + AF_DURATION, (uint32_t)((int32_t)(div_uf16(44100u << 16, kDefaultRate) + 0x8000u) >> 16));
    g_tick_frame = pf_r32(f + AF_DURATION);
    pf_at((g_tick_frame * 1000000000ull + 44099) / 44100, tick);
}

void pf_audio_init() {
    g_templates.clear();
    g_instruments.clear();
    g_knobs.clear();
    g_samples.clear();
    g_attachments.clear();
    pf_dsp_init();
    g_pf_dsp_ended = fifo_ended;
    clock_init();
    pf_on_slot(PF_AUDIO, -60, a_getaudiorate);
    pf_on_slot(PF_AUDIO, -64, a_getaudioduration);
    pf_on_slot(PF_AUDIO, -20, a_sleepaudioticks);
    pf_on_slot(PF_AUDIO, -68, a_sleepuntiltime);
    pf_on_slot(PF_AUDIO, -72, a_getcuesignal);
    pf_on_swi(0x4000d, a_signalattime);
    pf_on_slot(PF_AUDIO, -76, a_ownaudioclock);
    pf_on_slot(PF_AUDIO, -80, a_disownaudioclock);
    pf_on_slot(PF_AUDIO, -168, a_getaudiotime);
    pf_on_swi(0x4000f, a_setaudiorate);
    pf_on_swi(0x40010, a_setaudioduration);
    pf_on_create(NST_AUDIO, audio_create);
    pf_on_delete(NST_AUDIO, audio_delete);
    pf_on_slot(PF_AUDIO, -4, a_loadinstemplate);
    pf_on_slot(PF_AUDIO, -8, a_allocinstrument);
    pf_on_slot(PF_AUDIO, -16, a_grabknob);
    pf_on_slot(PF_AUDIO, -40, a_loadinstrument);
    pf_on_slot(PF_AUDIO, -96, a_unloadinstrument);
    pf_on_slot(PF_AUDIO, -12, a_loadsample);
    pf_on_slot(PF_AUDIO, -48, a_scansample);
    pf_on_slot(PF_AUDIO, -36, a_getaudioiteminfo);
    pf_on_slot(PF_AUDIO, -56, a_makesample);
    pf_on_slot(PF_AUDIO, -44, a_unloadsample);
    pf_on_slot(PF_AUDIO, -92, a_unloadinstemplate);
    pf_on_slot(PF_AUDIO, -144, a_attachsample);
    pf_on_slot(PF_AUDIO, -148, a_detachsample);
    pf_on_swi(0x40000, a_tweakknob);
    pf_on_swi(0x40001, a_startinstrument);
    pf_on_swi(0x40002, a_releaseinstrument);
    pf_on_swi(0x40003, a_stopinstrument);
    pf_on_swi(0x40008, a_connectinstruments);
    pf_on_swi(0x4000c, a_disconnectinstruments);
    pf_on_swi(0x40011, a_tweakrawknob);
    pf_on_swi(0x40012, a_startattachment);
    pf_on_swi(0x40013, a_releaseattachment);
    pf_on_swi(0x40014, a_stopattachment);
    pf_on_swi(0x40015, a_linkattachments);
    pf_on_swi(0x40016, a_monitorattachment);
    pf_on_swi(0x4001b, a_setaudioiteminfo);
}
