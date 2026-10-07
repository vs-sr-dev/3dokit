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
    AF_TAG_VELOCITY = 15, AF_TAG_TEMPLATE = 16, AF_TAG_INSTRUMENT = 17, AF_TAG_WIDTH = 22,
    AF_TAG_CHANNELS = 23, AF_TAG_FRAMES = 24, AF_TAG_BASENOTE = 25, AF_TAG_DETUNE = 26,
    AF_TAG_LOWNOTE = 27, AF_TAG_HIGHNOTE = 28, AF_TAG_LOWVELOCITY = 29, AF_TAG_HIGHVELOCITY = 30,
    AF_TAG_SUSTAINBEGIN = 31, AF_TAG_SUSTAINEND = 32, AF_TAG_RELEASEBEGIN = 33,
    AF_TAG_RELEASEEND = 34, AF_TAG_NUMBYTES = 35, AF_TAG_ADDRESS = 36, AF_TAG_SAMPLE = 37,
    AF_TAG_PRIORITY = 39, AF_TAG_SET_FLAGS = 40, AF_TAG_FREQUENCY = 42, AF_TAG_SAMPLE_RATE = 46,
    AF_TAG_COMPRESSIONRATIO = 47, AF_TAG_COMPRESSIONTYPE = 48, AF_TAG_NUMBITS = 49,
    AF_TAG_DELAY_LINE = 57,
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
    int state;                              // 0 allocated, 3 started (the node's +0x30 in the folio)
    std::map<uint32_t, int32_t> value;      // what the folio wrote to each knob resource
    std::vector<Connection> inputs;
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

// A sample (0x3a3c): the kernel's item tags (vector 38, 0x1acf4), the folio's defaults, a first
// look for AF_TAG_SAMPLE (another sample's info copied) and AF_TAG_DELAY_LINE (memory of the
// folio's), the frames from the bytes, the tags as SetAudioItemInfo takes them, and the base
// frequency. Only made without tags so far.
static uint32_t create_sample(ArmCpu& c, const Tags& tags) {
    if (!tags.empty()) pf_stop(c, "a sample's tags at its creation: not yet");
    Sample s;
    s.frames = bytes_to_frames(c, s, s.numbytes);
    sample_base_freq(c, s);
    int32_t item = audio_item(SAMPLE_NODE);
    g_samples[item] = s;
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
    case TEMPLATE_NODE: case INSTRUMENT_NODE: case KNOB_NODE: case 5: c.r[0] = AF_ERR_UNIMPLEMENTED; break;
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

static void tick(uint64_t) {
    pf_w32(folio() + AF_TIME, pf_r32(folio() + AF_TIME) + 1);
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
    clock_init();
    pf_on_slot(PF_AUDIO, -60, a_getaudiorate);
    pf_on_slot(PF_AUDIO, -64, a_getaudioduration);
    pf_on_slot(PF_AUDIO, -76, a_ownaudioclock);
    pf_on_slot(PF_AUDIO, -80, a_disownaudioclock);
    pf_on_slot(PF_AUDIO, -168, a_getaudiotime);
    pf_on_swi(0x4000f, a_setaudiorate);
    pf_on_swi(0x40010, a_setaudioduration);
    pf_on_create(NST_AUDIO, audio_create);
    pf_on_slot(PF_AUDIO, -4, a_loadinstemplate);
    pf_on_slot(PF_AUDIO, -8, a_allocinstrument);
    pf_on_slot(PF_AUDIO, -16, a_grabknob);
    pf_on_swi(0x40000, a_tweakknob);
    pf_on_swi(0x40001, a_startinstrument);
    pf_on_swi(0x40008, a_connectinstruments);
    pf_on_swi(0x40011, a_tweakrawknob);
    pf_on_swi(0x4001b, a_setaudioiteminfo);
}
