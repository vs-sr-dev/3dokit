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
static const uint32_t kNodeSize[] = {0, 0x54, 0x58, 0x34, 0x98, 0x38, 0x74, 0x54, 0x34};
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
    int state;                              // the node's +0x30: 0 allocated or abandoned, 1 stopped, 3 started
    int dsp_state;                          // its DSP side's (+0x14 of the folio's record): above 1 running
    std::map<uint32_t, int32_t> value;      // what the folio wrote to each knob resource
    std::vector<Connection> inputs;
    // Per FIFO (a resource), the attachment it plays: the folio keeps it in its table of the DSP's
    // FIFOs (+0x18 of an entry, by the FIFO's number), set when an attachment starts, cleared when
    // it stops.
    std::map<int, int32_t> playing;
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
    uint8_t flags = 0;                      // +0x50: bits 0 and 1 set for a delay line's memory
    uint8_t numbits = 16;                   // +0x51
    uint8_t width = 2;                      // +0x52 bytes
    uint8_t channels = 1;                   // +0x53
    uint8_t basenote = 60;                  // +0x54
    uint8_t detune = 0;                     // +0x55 (read back signed)
    uint8_t lownote = 0, highnote = 127;    // +0x56, +0x57
    uint8_t lowvelocity = 0, highvelocity = 127;    // +0x58, +0x59
    uint8_t compression_ratio = 1;          // +0x5a
    uint32_t compression_type = 0;          // +0x68
    uint32_t rate = (uint32_t)kSampleRate << 16;    // +0x8c AF_TAG_SAMPLE_RATE, frac16 Hz
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
    int state;                              // +0x26: 0 made, 1 stopped, 3 playing
    int32_t next;                           // LinkAttachments (+0x4c): what plays after it, 0 none
};
enum : uint32_t { AF_ATTF_NOAUTOSTART = 1, AF_ERR_NULLADDRESS = 0xD52BF119u };
static std::map<int32_t, Attachment> g_attachments;

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
    Instrument ins{tmpl, flags, 0, 0, {}, {}, {}};
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
// there; neither they nor SAMPLE and DELAY_LINE are done yet.
static uint32_t create_sample(ArmCpu& c, const Tags& tags, uint32_t tag_ptr) {
    for (auto [tag, v] : tags) {
        (void)v;
        if (tag < 10 || tag >= 0xfe) pf_stop(c, "an item tag at a sample's creation: not yet");
        if (tag == AF_TAG_SAMPLE || tag == AF_TAG_DELAY_LINE) pf_stop(c, "AF_TAG_SAMPLE or DELAY_LINE: not yet");
    }
    Sample s;
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
// What the folio does to the DSP on each is not done here; the states it keeps are.

// An attachment started (0x74b8): its sample must have an address (else AF_ERR_NULLADDRESS) and 4
// bytes or more (else AF_ERR_OUTOFRANGE); then the FIFO plays it, and it is playing. A sample
// without a sustain or release loop plays once: the folio looks at what is linked after it (0x795c,
// forgetting a link to what is no longer an attachment) to queue it in the DSP.
static uint32_t attachment_start(int32_t a) {
    Attachment& at = g_attachments[a];
    const Sample& s = g_samples[at.sample];
    if (!s.address) return AF_ERR_NULLADDRESS;
    if (s.numbytes < 4) return AF_ERR_OUTOFRANGE;
    g_instruments[at.ins].playing[at.rsrc] = a;
    if (s.sustain_begin < 0 && s.release_begin < 0 && at.next && !pf_check_item(at.next, NST_AUDIO, ATTACHMENT_NODE))
        at.next = 0;
    at.state = 3;
    return 0;
}

// An attachment stopped (0x7cf8): when it is playing, its FIFO plays nothing and it is stopped.
static void attachment_stop(int32_t a) {
    Attachment& at = g_attachments[a];
    if (at.state <= 1) return;
    g_instruments[at.ins].playing.erase(at.rsrc);
    at.state = 1;
}

// An instrument's DSP side stopped (0x7be8), when it runs: every FIFO's playing attachment
// stopped, while it is still an attachment (the envelopes' attachments, a list of their own, are
// not made here).
static void dsp_stop(Instrument& ins) {
    if (ins.dsp_state <= 1) return;
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
static void a_stopinstrument(ArmCpu& c) { c.r[0] = stop_instrument((int32_t)c.r[0], c.r[1]); }

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
    switch (type) {
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
// the folio's open nor the owner is asked. When at1 is playing the folio also links (or unlinks)
// the two in the DSP (0x7860, 0x76b8), which is not done here.
static void a_linkattachments(ArmCpu& c) {
    int32_t a1 = (int32_t)c.r[0], a2 = (int32_t)c.r[1];
    if (!pf_check_item(a1, NST_AUDIO, ATTACHMENT_NODE) || (a2 && !pf_check_item(a2, NST_AUDIO, ATTACHMENT_NODE))) {
        c.r[0] = AF_ERR_BADITEM;
        return;
    }
    Attachment& at = g_attachments[a1];
    at.next = a2;
    if (g_pf_trace) pf_log("        attachment %d then %d\n", a1, a2);
    c.r[0] = 0;
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
// knob. Its DSP side, when running, is stopped first (before the tags, so a bad tag leaves it
// stopped). Then on each of its FIFOs, in the template's order, the first attachment (in the order
// made) not marked AF_ATTF_NOAUTOSTART starts -- whether it can is not the call's result -- and
// its DSP side runs; its node's state becomes 3.
static void a_startinstrument(ArmCpu& c) {
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
    ins.dsp_state = 3;
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

// swi 0x4000c: Err DisconnectInstruments(Item src, char* srcName, Item dst, char* dstName) --
// 0x1cac and 0x8130: both instruments (else AF_ERR_BADITEM), the names as ConnectInstruments
// finds them (else AF_ERR_BADNAME); then every place in the destination's code that reads that
// resource is put back as it was loaded (0xc000) -- whatever fed it, and whether anything did.
static void a_disconnectinstruments(ArmCpu& c) {
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
    c.r[0] = 0;
}

// swi 0x4001b: Err SetAudioItemInfo(Item item, TagArg* tags) -- 0x120c: the folio open; an item
// of the folio's (LocateItem, else AF_ERR_BADITEM); the kernel's check of 8 bytes at the tags
// (vector 40 again, never refusing); then by its node type: a sample's tags (0x347c), an
// envelope's (0x4fac), an attachment's (0x6088), a tuning's (0x6754); a template, an instrument,
// a knob or a cue AF_ERR_UNIMPLEMENTED. For a number that names no item the folio goes on with a
// null node and reads its type at address 9: stopped here.
static void a_setaudioiteminfo(ArmCpu& c) {
    if (!audio_open()) { c.r[0] = AF_ERR_AUDIOCLOSED; return; }
    int32_t item = (int32_t)c.r[0];
    uint32_t n = pf_item_node(item);
    if (!n) pf_stop(c, "SetAudioItemInfo of no item (the folio reads a null node)");
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
    case 6: case 7: case 8: pf_stop(c, "SetAudioItemInfo of an envelope, attachment or tuning: not yet");
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
// result unread), else left; off the list when on it; its signal word 0.
static void delete_cue(int32_t item) {
    uint32_t n = pf_item_node(item);
    if (pf_r32(n + 28) == task_item()) pf_free_signal(pf_r32(n + CUE_SIGNAL));
    if (pf_r32(n + TN_LIST)) {
        pf_list_rem_node(n);
        pf_w32(n + TN_LIST, 0);
    }
    pf_w32(n + CUE_SIGNAL, 0);
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

// The folio's FIRQ (0x3e90): the time up by 1; when a wake-up is wanted and has come (a signed
// difference), it signals the daemon (+0xa8, the bits at +0xa4) and wants none. The daemon, at the
// folio's high priority, then runs the list; here the list is run at once.
static void tick(uint64_t) {
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
    clock_init();
    pf_on_slot(PF_AUDIO, -60, a_getaudiorate);
    pf_on_slot(PF_AUDIO, -64, a_getaudioduration);
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
    pf_on_slot(PF_AUDIO, -144, a_attachsample);
    pf_on_slot(PF_AUDIO, -148, a_detachsample);
    pf_on_swi(0x40000, a_tweakknob);
    pf_on_swi(0x40001, a_startinstrument);
    pf_on_swi(0x40003, a_stopinstrument);
    pf_on_swi(0x40008, a_connectinstruments);
    pf_on_swi(0x4000c, a_disconnectinstruments);
    pf_on_swi(0x40011, a_tweakrawknob);
    pf_on_swi(0x40015, a_linkattachments);
    pf_on_swi(0x4001b, a_setaudioiteminfo);
}
