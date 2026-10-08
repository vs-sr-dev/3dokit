// 3dokit runtime -- the DSP: the instruments the audio folio loads, as native code, the bus they
// mix into, the DMA channels that feed their FIFOs, and the sound that comes out.
//
// On the console the folio loads each instrument's DSP code (its .dsp file's DCOD) into the DSP,
// links the running ones into one program in the order of their priorities, and the DSP runs it
// once a sample frame, 44,100 times a second. Here each instrument the programs run so far load
// is that code transliterated, an instruction at a time (python -m 3dokit.dsp FILE --dis reads
// it), and runs once a frame in the same order. An instrument is known by its code, not its name:
// the code's words must be the ones transliterated (kModels' checksums), else the instrument
// makes no sound (and says so once).
//
// The arithmetic is the DSP's as the FreeDO emulator reads it (Opera carries it; read, not
// copied): an operand is 16 bits at the top of the ALU's word; the multiplier gives a * b * 2, or
// a times the accumulator's top 17 bits with the lowest cleared; adds and subtracts set the flags
// at bit 31; CLIP saturates on overflow; a result is written back as its top 16 bits. The ALU
// keeps 20 bits (the word's top ones): its increment is 0x1000 and CLIP's largest value
// 0x7FFFF000, as in FreeDO's reading, which also has a 32-bit variant (its default).
//
// The FIFOs are fed by CLIO's DMA as the folio programs it (pf_audio.cpp): a current chunk and a
// next one, reloaded from the next each time the current runs out -- so a loop repeats until the
// next is changed -- and an interrupt at each reload, where the folio's handler (AUDIOFOLIO
// 0x5854) writes the next chunk it has waiting and, when armed, signals its daemon, whose work
// (0x5cc0) is the folio's again (g_pf_dsp_ended). A sample word is 16 bits, big-endian. The
// folio's own silence (32 bytes it allocates at its start, unfilled: zeros here) stands for what
// follows a sample that ends. A FIFO whose DMA is off reads 0; the FIFO's few words of buffering
// are not modelled (a sample starts at once).
//
// The sound is made in the guest's time: before anything the folio changes (pf_dsp_sync, at each
// call and at each tick of the audio clock) every frame up to the guest's present is run, so a run
// sounds the same every time, at any host speed. It goes to a WAV file (pfboot --wav) and to
// pfboot's window.
#include "pf.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

void (*g_pf_audio_out)(const int16_t* lr, size_t frames);
void (*g_pf_dsp_ended)(int32_t ins, uint32_t rsrc);

namespace {

// ---- the ALU ----------------------------------------------------------------------------------
const uint32_t kAlu = 0xFFFFF000u;                      // the 20 bits the ALU keeps
inline uint32_t word(int16_t v) { return (uint32_t)(uint16_t)v << 16; }     // an operand
inline int16_t wb(uint32_t y) { return (int16_t)(y >> 16); }               // a result written back
// The multiplier, both operands given (M2SEL): a * b * 2.
inline uint32_t mul(int16_t a, int16_t b) { return (uint32_t)((int64_t)a * b * 2) & kAlu; }
// The multiplier on the accumulator: a * ((y >> 15) & ~1).
inline uint32_t mul_acc(int16_t a, uint32_t y) { return (uint32_t)((int64_t)a * (((int32_t)y >> 15) & ~1)) & kAlu; }
inline bool add_carry(uint32_t a, uint32_t b, uint32_t y) { return ((a & b) | (a & ~y) | (b & ~y)) >> 31; }
inline bool add_overflow(uint32_t a, uint32_t b, uint32_t y) { return ((a & b & ~y) | (~a & ~b & y)) >> 31; }
// a + b with CLIP: on overflow the largest value of the sign the operands had.
inline uint32_t add_clip(uint32_t a, uint32_t b) {
    uint32_t y = a + b;
    return add_overflow(a, b, y) ? (y >> 31 ? 0x7FFFF000u : 0x80000000u) : y;
}

// ---- a FIFO and its DMA channel ---------------------------------------------------------------
struct Chunk {
    bool silence = false;                               // the folio's 32 bytes of silence
    uint32_t addr = 0, bytes = 0;
};
struct Fifo {
    bool on = false;                                    // the channel enabled
    Chunk cur, next;
    uint32_t pos = 0;                                   // bytes of the current chunk read
    bool has_next = false;
    // The folio's table of FIFOs (its globals +0xe8, 28 bytes each): a next chunk waiting for the
    // next interrupt (+4, +8, +0xc), and a signal to its daemon armed for it (+0x10).
    bool pending = false, armed = false;
    Chunk waiting;
};

// ---- the instruments ----------------------------------------------------------------------------
enum Kind { NONE, MIXER, SAMPLER, VARMONO8, DCSQXDHALFMONO, DCSQXDHALFSTEREO, ENVELOPE };

struct Model {
    const char* file;
    uint32_t sum;                                       // FNV-1a of the code words
    Kind kind;
    int inputs;                                         // a mixer's
    bool gated = false;                                 // the frame skipped while the FIFO is empty
};
// The code transliterated, from the 1993 library (System/Audio/dsp on Crash 'n Burn's disc).
// sampler.dsp's subroutine, oscupdownfp.dsp, is the folio's to load and is not checked here.
const Model kModels[] = {
    {"mixer8x2.dsp", 0xe40d8e2du, MIXER, 8},
    {"mixer4x2.dsp", 0x0bbc6b55u, MIXER, 4},
    {"sampler.dsp", 0x90af13d7u, SAMPLER, 0},
    {"varmono8.dsp", 0xcc3a4a27u, VARMONO8, 0},
    {"dcsqxdhalfmono.dsp", 0x136cf5d3u, DCSQXDHALFMONO, 0},
    // from 23.10's library (System/Audio/dsp on Immercenary's disc): mixer2x2 is mixer4x2's code
    // with two inputs
    {"mixer2x2.dsp", 0xf897e91eu, MIXER, 2},
    {"dcsqxdhalfstereo.dsp", 0x26daf914u, DCSQXDHALFSTEREO, 0},
    {"envelope.dsp", 0x70c12c2cu, ENVELOPE, 0},
    // 1993's code behind a test of its FIFO's status (SLEEP while it is empty)
    {"dcsqxdhalfmono.dsp", 0x7c904d9au, DCSQXDHALFMONO, 0, true},
};

struct Input { int32_t src; uint32_t rsrc; };

struct Unit {
    Kind kind = NONE;
    int inputs = 0;
    bool gated = false;
    uint8_t pri = 0;
    std::vector<std::string> names;
    std::vector<int16_t> mem;                           // per resource: its word in DSP memory
    std::map<uint32_t, Input> fed;                      // a resource another instrument's variable feeds
    std::map<uint32_t, Fifo> fifo;                      // by the FIFO's resource
    // sampler.dsp's registers in its ring (MYRB): R5 the phase, R6 and R7 the two samples
    int16_t r5 = 0, r6 = 0, r7 = 0;
    // the resources each model reads and writes, by name
    int out = -1, amp = -1, freq = -1, in_fifo = -1;
    int phase = -1, oldv = -1, newv = -1, hold = -1, toggle = -1;
    int prev = -1, square = -1, byte = -1, accum = -1, state = -1;
    // dcsqxdhalfstereo.dsp's: per side its byte, sum, new value, last value and output
    int sbyte[2] = {-1, -1}, saccum[2] = {-1, -1}, stemp[2] = {-1, -1}, sprev[2] = {-1, -1}, sout[2] = {-1, -1};
    // envelope.dsp's
    int ecur = -1, esrc = -1, etgt = -1, ephase = -1, eincr = -1, ereq = -1;
    std::vector<int> in, left, right;
};

std::map<int32_t, Unit> g_units;
std::vector<int32_t> g_order;                           // the running ones, as the DSP runs them
int16_t g_bus_l, g_bus_r;                               // I memory 0x106, 0x107: what the mixers add up
uint64_t g_frames;                                      // frames made since the boot
bool g_in_frame;
std::vector<int16_t> g_out;
std::set<std::string> g_told;

FILE* g_wav;
uint64_t g_wav_frames;

uint32_t fnv(const std::vector<uint8_t>& d) {
    uint32_t h = 2166136261u;
    for (uint8_t b : d) h = (h ^ b) * 16777619u;
    return h;
}

int rsrc_named(const Unit& u, const std::string& name) {
    for (size_t i = 0; i < u.names.size(); ++i)
        if (u.names[i] == name) return (int)i;
    return -1;
}

int16_t value(const Unit& u, int r) {
    if (r < 0) return 0;
    auto f = u.fed.find((uint32_t)r);
    if (f != u.fed.end()) {
        auto s = g_units.find(f->second.src);
        if (s != g_units.end() && f->second.rsrc < s->second.mem.size()) return s->second.mem[f->second.rsrc];
    }
    return u.mem[(size_t)r];
}

// A word from a FIFO: the next of its current chunk; when that has run out, the channel reloads
// from the next chunk and interrupts, and the folio's handler (0x5854) puts in the chunk it has
// waiting and signals its daemon if armed.
uint16_t fifo_read(int32_t item, Unit& u, int r) {
    Fifo& f = u.fifo[(uint32_t)r];
    if (!f.on) return 0;
    if (f.pos + 2 > f.cur.bytes) {
        if (!f.has_next) {
            f.on = false;
            return 0;
        }
        f.cur = f.next;
        f.pos = 0;
        if (f.pending) {
            f.pending = false;
            f.next = f.waiting;
        }
        if (f.armed) {
            f.armed = false;
            if (g_pf_dsp_ended) g_pf_dsp_ended(item, (uint32_t)r);
            if (!f.on) return 0;
        }
        if (f.cur.bytes < 2) return 0;
    }
    uint32_t a = f.cur.addr + f.pos;
    f.pos += 2;
    if (f.cur.silence) return 0;
    return (uint16_t)(pf_r8(a) << 8 | pf_r8(a + 1));
}

// mixerNx2.dsp: per side, the inputs times their gains summed with CLIP, then added to the bus
// with CLIP.
void run_mixer(Unit& u) {
    for (int side = 0; side < 2; ++side) {
        const std::vector<int>& g = side ? u.right : u.left;
        uint32_t y = mul(value(u, g[0]), value(u, u.in[0]));
        for (int i = 1; i < u.inputs; ++i) y = add_clip(mul(value(u, g[(size_t)i]), value(u, u.in[(size_t)i])), y);
        int16_t& bus = side ? g_bus_r : g_bus_l;
        bus = wb(add_clip(word(bus), y));
    }
}

// sampler.dsp with oscupdownfp.dsp: the phase steps by Frequency (0x8000 a sample a frame); past
// one sample (negative) one word comes in, past two (carry) two; the output is the two samples
// interpolated at the phase, times Amplitude.
void run_sampler(int32_t item, Unit& u) {
    int16_t fr = value(u, u.freq);
    uint32_t a = word(u.r5), b = word(fr), y = a + b;
    u.r5 = wb(y);
    if (y >> 31) {
        u.r5 = wb(word(u.r5) - 0x80000000u);
        u.r6 = u.r7;
        u.r7 = (int16_t)fifo_read(item, u, u.in_fifo);
    } else if (add_carry(a, b, y)) {
        u.r6 = (int16_t)fifo_read(item, u, u.in_fifo);
        u.r7 = (int16_t)fifo_read(item, u, u.in_fifo);
    }
    y = mul(u.r5, u.r6) - word(u.r6);
    y = mul(u.r7, u.r5) - y;
    u.mem[(size_t)u.out] = wb(mul_acc(value(u, u.amp), y));
}

// varmono8.dsp: the same with 8-bit samples, two to a word, the high byte first; Toggle is
// negative when no byte of the last word (SampleHold) is left.
void run_varmono8(int32_t item, Unit& u) {
    std::vector<int16_t>& m = u.mem;
    auto& ph = m[(size_t)u.phase];
    auto& ov = m[(size_t)u.oldv];
    auto& nv = m[(size_t)u.newv];
    auto& sh = m[(size_t)u.hold];
    auto& tg = m[(size_t)u.toggle];
    uint32_t a = word(ph), b = word(value(u, u.freq)), y = a + b;
    ph = wb(y);
    auto high = [](int16_t w) { return (int16_t)((uint16_t)w & 0xFF00); };
    auto low = [](int16_t w) { return (int16_t)(uint16_t)((uint16_t)w << 8); };
    if (y >> 31) {
        ov = nv;
        uint32_t t = word(tg) + 0x80000000u;
        tg = wb(t);
        if (t >> 31) nv = low(sh);
        else {
            sh = (int16_t)fifo_read(item, u, u.in_fifo);
            nv = high(sh);
        }
        y = word(ph) - 0x80000000u;
        ph = wb(y);
        y = mul_acc(ov, y) - word(ov);
        y = mul(nv, ph) - y;
    } else if (add_carry(a, b, y)) {
        if (tg < 0) {
            sh = (int16_t)fifo_read(item, u, u.in_fifo);
            ov = high(sh);
            nv = low(sh);
        } else {
            ov = low(sh);
            sh = (int16_t)fifo_read(item, u, u.in_fifo);
            nv = high(sh);
        }
        y = mul(ph, ov) - word(ov);
        y = mul(nv, ph) - y;
    } else {
        y = mul_acc(ov, y) - word(ov);
        y = mul(nv, ph) - y;
    }
    m[(size_t)u.out] = wb(mul_acc(value(u, u.amp), y));
}

// dcsqxdhalfmono.dsp: SDX2, a byte a sample at half the rate. On even frames the next byte (the
// high one of a new word, or the low one kept) is squared with its sign; an odd byte adds the
// square to the last value (with CLIP), an even one is the value; the frame plays the mean of the
// last value and the new one, and the odd frame after it the new one.
bool fifo_has(Unit& u, int r);

void run_dcsqxd(int32_t item, Unit& u) {
    if (u.gated && !fifo_has(u, u.in_fifo)) return;     // 23.10's: TRA InFIFO's status, BZ to SLEEP
    std::vector<int16_t>& m = u.mem;
    auto& cs = m[(size_t)u.state];
    auto& pv = m[(size_t)u.prev];
    auto& sq = m[(size_t)u.square];
    auto& by = m[(size_t)u.byte];
    auto& hd = m[(size_t)u.hold];
    auto& ac = m[(size_t)u.accum];
    cs = wb(word(cs) + word(1));
    uint32_t y;
    if (cs & 1) y = word(pv);
    else {
        if (cs & 2) {
            y = word(hd) << 8;
            by = wb(y);
        } else {
            hd = (int16_t)fifo_read(item, u, u.in_fifo);
            y = word(hd) & 0xFF000000u;
            by = wb(y);
        }
        if (y >> 31) y = 0u - y;
        sq = wb(mul_acc(by, y));
        if ((uint16_t)by & 0x100) y = add_clip(word(sq), word(ac));
        else y = word(sq);
        ac = wb(y);
        int16_t now = wb(y);
        y = ((uint32_t)((int32_t)y >> 1)) & kAlu;
        y = mul(pv, 0x4000) + y;
        pv = now;
    }
    m[(size_t)u.out] = wb(mul_acc(value(u, u.amp), y));
}

// A FIFO's status word, which a relocation of its own (mask 0x1020a00, bit 24 set) gives the
// code: here only whether there is a word to read -- in the current chunk, or a next to reload
// from -- as the FIFO's own buffering is not modelled.
bool fifo_has(Unit& u, int r) {
    Fifo& f = u.fifo[(uint32_t)r];
    return f.on && (f.pos + 2 <= f.cur.bytes || f.has_next);
}

// dcsqxdhalfstereo.dsp (23.10): SDX2 stereo at half the rate. Toggle steps by 0x8000 a frame; on a
// frame it goes negative, when the FIFO's status says it has a word (else nothing at all), the word
// comes in: its high byte the left channel's, its low byte (shifted up) the right's; each byte is
// squared with its sign, an odd byte adds the square to the side's sum (no CLIP, unlike the mono
// decoder), an even one is the sum; the side plays the mean of its last value and the new one, and
// the new one becomes its last value. On the other frames each side plays its last value.
void run_dcsqxd_stereo(int32_t item, Unit& u) {
    std::vector<int16_t>& m = u.mem;
    auto& tg = m[(size_t)u.toggle];
    uint32_t y = word(tg) + word((int16_t)0x8000);
    tg = wb(y);
    if (!(y >> 31)) {
        for (int s = 0; s < 2; ++s) m[(size_t)u.sout[s]] = wb(mul(m[(size_t)u.sprev[s]], value(u, u.amp)));
        return;
    }
    if (!fifo_has(u, u.in_fifo)) return;
    auto& hd = m[(size_t)u.hold];
    auto& sq = m[(size_t)u.square];
    hd = (int16_t)fifo_read(item, u, u.in_fifo);
    m[(size_t)u.sbyte[0]] = wb(word(hd) & 0xFF000000u);
    m[(size_t)u.sbyte[1]] = wb(word(hd) << 8);
    for (int s = 0; s < 2; ++s) {
        int16_t by = m[(size_t)u.sbyte[s]];
        auto& ac = m[(size_t)u.saccum[s]];
        y = word(by);
        if (y >> 31) y = 0u - y;
        sq = wb(mul_acc(by, y));
        y = ((uint16_t)by & 0x100) ? word(sq) + word(ac) : word(sq);
        ac = wb(y);
        m[(size_t)u.stemp[s]] = wb(y);
        y = ((uint32_t)((int32_t)y >> 1)) & kAlu;
        y = mul(m[(size_t)u.sprev[s]], 0x4000) + y;
        m[(size_t)u.sout[s]] = wb(mul_acc(value(u, u.amp), y));
        m[(size_t)u.sprev[s]] = m[(size_t)u.stemp[s]];
    }
}

// envelope.dsp (23.10): when Env.request is not Env.target, a new segment -- the current value
// its source, its phase 0, the request its target; else the phase steps by Env.incr (with CLIP)
// and the current value is the target times the phase less the source times (phase less one):
// from the source to the target, linearly, over 0x8000 / Env.incr frames. Output is the current
// value.
void run_envelope(Unit& u) {
    std::vector<int16_t>& m = u.mem;
    auto& cur = m[(size_t)u.ecur];
    auto& src = m[(size_t)u.esrc];
    auto& tgt = m[(size_t)u.etgt];
    auto& ph = m[(size_t)u.ephase];
    int16_t req = value(u, u.ereq);
    if (word(req) - word(tgt)) {
        src = cur;
        ph = 0;
        tgt = req;
    } else {
        uint32_t y = add_clip(word(ph), word(value(u, u.eincr)));
        ph = wb(y);
        y = mul_acc(src, y) - word(src);
        y = mul(tgt, ph) - y;
        cur = wb(y);
    }
    m[(size_t)u.out] = cur;
}

// One frame: head.dsp first (the bus to the DAC, and cleared), then the running instruments.
void frame() {
    g_out.push_back(g_bus_l);
    g_out.push_back(g_bus_r);
    g_bus_l = g_bus_r = 0;
    std::vector<int32_t> order = g_order;
    for (int32_t item : order) {
        auto it = g_units.find(item);
        if (it == g_units.end()) continue;
        Unit& u = it->second;
        switch (u.kind) {
        case MIXER: run_mixer(u); break;
        case SAMPLER: run_sampler(item, u); break;
        case VARMONO8: run_varmono8(item, u); break;
        case DCSQXDHALFMONO: run_dcsqxd(item, u); break;
        case DCSQXDHALFSTEREO: run_dcsqxd_stereo(item, u); break;
        case ENVELOPE: run_envelope(u); break;
        case NONE: break;
        }
    }
    ++g_frames;
}

void wav_header(FILE* f, uint64_t frames) {
    uint32_t data = (uint32_t)(frames * 4), riff = 36 + data;
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
                     0x44, 0xac, 0, 0, 0x10, 0xb1, 2, 0, 4, 0, 16, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0};
    for (int i = 0; i < 4; ++i) {
        h[4 + i] = (uint8_t)(riff >> (8 * i));
        h[40 + i] = (uint8_t)(data >> (8 * i));
    }
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof h, f);
    std::fseek(f, 0, SEEK_END);
}

void wav_close() {
    if (!g_wav) return;
    wav_header(g_wav, g_wav_frames);
    std::fclose(g_wav);
    g_wav = nullptr;
}

} // namespace

// pfboot --wav FILE: the sound, 44,100 Hz, 16-bit stereo.
bool pf_dsp_wav(const char* path) {
    g_wav = std::fopen(path, "wb");
    if (!g_wav) return false;
    wav_header(g_wav, 0);
    std::atexit(wav_close);
    return true;
}

// The folio's start: no instrument, nothing on the bus. The frames made go on counting (pfboot
// --boot starts a fresh OS for each program; the guest's clock goes on).
void pf_dsp_init() {
    g_units.clear();
    g_order.clear();
    g_bus_l = g_bus_r = 0;
}

void pf_dsp_new(int32_t ins, const std::string& file, const std::vector<std::string>& names,
                const std::vector<uint8_t>& code, uint8_t pri) {
    Unit u;
    u.pri = pri;
    u.names = names;
    u.mem.assign(names.size(), 0);
    uint32_t sum = fnv(code);
    std::string base = file.substr(file.find_last_of("/\\") + 1);
    for (char& ch : base) ch = (char)std::tolower((unsigned char)ch);
    for (const Model& m : kModels)
        if (base == m.file && m.sum == sum) {
            u.kind = m.kind;
            u.inputs = m.inputs;
            u.gated = m.gated;
        }
    if (u.kind == NONE && !g_told.count(base)) {
        g_told.insert(base);
        std::fprintf(stderr, "pfboot: the DSP instrument %s (code 0x%08x) has no native model here: silent\n",
                     base.c_str(), sum);
    }
    u.out = rsrc_named(u, "Output");
    u.amp = rsrc_named(u, "Amplitude");
    u.freq = rsrc_named(u, "Frequency");
    u.in_fifo = rsrc_named(u, "InFIFO");
    u.phase = rsrc_named(u, "Phase");
    u.oldv = rsrc_named(u, "OldVal");
    u.newv = rsrc_named(u, "NewVal");
    u.toggle = rsrc_named(u, "Toggle");
    u.hold = rsrc_named(u, u.kind == DCSQXDHALFMONO || u.kind == DCSQXDHALFSTEREO ? "dc_hold" : "SampleHold");
    for (int s = 0; s < 2; ++s) {
        const std::string side = s ? "right" : "left", Side = s ? "Right" : "Left";
        u.sbyte[s] = rsrc_named(u, "dc_" + side + "byte");
        u.saccum[s] = rsrc_named(u, "dc_" + side + "accum");
        u.stemp[s] = rsrc_named(u, "Temp" + Side);
        u.sprev[s] = rsrc_named(u, "Prev" + Side);
        u.sout[s] = rsrc_named(u, Side + "Output");
    }
    u.prev = rsrc_named(u, "PrevValue");
    u.square = rsrc_named(u, "dc_square");
    u.byte = rsrc_named(u, "dc_byte");
    u.accum = rsrc_named(u, "dc_accum");
    u.state = rsrc_named(u, "CurState");
    u.ecur = rsrc_named(u, "Env.current");
    u.esrc = rsrc_named(u, "Env.source");
    u.etgt = rsrc_named(u, "Env.target");
    u.ephase = rsrc_named(u, "Env.phase");
    u.eincr = rsrc_named(u, "Env.incr");
    u.ereq = rsrc_named(u, "Env.request");
    for (int i = 0; i < u.inputs; ++i) {
        u.in.push_back(rsrc_named(u, "Input" + std::to_string(i)));
        u.left.push_back(rsrc_named(u, "LeftGain" + std::to_string(i)));
        u.right.push_back(rsrc_named(u, "RightGain" + std::to_string(i)));
    }
    g_units[ins] = std::move(u);
}

// A knob's value, as the folio writes it into DSP memory: its low 16 bits.
void pf_dsp_write(int32_t ins, uint32_t rsrc, int32_t v) {
    auto it = g_units.find(ins);
    if (it != g_units.end() && rsrc < it->second.mem.size()) it->second.mem[rsrc] = (int16_t)v;
}

void pf_dsp_connect(int32_t src, uint32_t src_rsrc, int32_t dst, uint32_t dst_rsrc) {
    auto it = g_units.find(dst);
    if (it != g_units.end()) it->second.fed[dst_rsrc] = {src, src_rsrc};
}

void pf_dsp_disconnect(int32_t dst, uint32_t dst_rsrc) {
    auto it = g_units.find(dst);
    if (it != g_units.end()) it->second.fed.erase(dst_rsrc);
}

// Into the DSP's program or out of it: the folio's list of running instruments (0xc2d8) is by
// priority, a new one after those of its own (the kernel's UniversalInsertNode, 0xb950).
void pf_dsp_run(int32_t ins, bool on) {
    for (size_t i = 0; i < g_order.size(); ++i)
        if (g_order[i] == ins) {
            g_order.erase(g_order.begin() + (long)i);
            break;
        }
    if (!on) return;
    auto it = g_units.find(ins);
    if (it == g_units.end()) return;
    size_t at = 0;
    while (at < g_order.size() && g_units[g_order[at]].pri >= it->second.pri) ++at;
    g_order.insert(g_order.begin() + (long)at, ins);
}

// An instrument freed: out of the program. What it fed goes on reading its last words, as the
// patched code goes on reading the freed DSP memory.
void pf_dsp_delete(int32_t ins) {
    pf_dsp_run(ins, false);
    auto it = g_units.find(ins);
    if (it != g_units.end())
        for (auto& [r, f] : it->second.fifo) f.on = false;
}

static Fifo* fifo_of(int32_t ins, uint32_t rsrc) {
    auto it = g_units.find(ins);
    return it == g_units.end() ? nullptr : &it->second.fifo[rsrc];
}
static Chunk chunk(uint32_t addr, uint32_t bytes) {
    Chunk c;
    c.silence = addr == PF_DSP_SILENCE;
    c.addr = c.silence ? 0 : addr;
    c.bytes = c.silence ? 32 : bytes;
    return c;
}

// The folio's 0x9d24: the channel stopped, its current and next chunks set, and on.
void pf_dsp_dma(int32_t ins, uint32_t rsrc, uint32_t addr, uint32_t bytes, uint32_t naddr, uint32_t nbytes) {
    if (Fifo* f = fifo_of(ins, rsrc)) {
        f->cur = chunk(addr, bytes);
        f->next = chunk(naddr, nbytes);
        f->pos = 0;
        f->has_next = true;
        f->on = true;
    }
}
// 0x9cb8: the next chunk only, and on.
void pf_dsp_dma_next(int32_t ins, uint32_t rsrc, uint32_t naddr, uint32_t nbytes) {
    if (Fifo* f = fifo_of(ins, rsrc)) {
        f->next = chunk(naddr, nbytes);
        f->has_next = true;
        f->on = true;
    }
}
// 0x9e34: the channel off.
void pf_dsp_dma_stop(int32_t ins, uint32_t rsrc) {
    if (Fifo* f = fifo_of(ins, rsrc)) f->on = false;
}
// 0x65e8: a next chunk for the handler to put in at the next interrupt.
void pf_dsp_dma_waiting(int32_t ins, uint32_t rsrc, uint32_t addr, uint32_t bytes) {
    if (Fifo* f = fifo_of(ins, rsrc)) {
        f->waiting = chunk(addr, bytes);
        f->pending = true;
    }
}
// 0x6578's signal armed for the next interrupt, and 0x6648: neither the waiting chunk nor it.
void pf_dsp_dma_arm(int32_t ins, uint32_t rsrc) {
    if (Fifo* f = fifo_of(ins, rsrc)) f->armed = true;
}
void pf_dsp_dma_quiet(int32_t ins, uint32_t rsrc) {
    if (Fifo* f = fifo_of(ins, rsrc)) f->pending = f->armed = false;
}

// Every frame up to the guest's present, and what they make to the WAV and the window.
void pf_dsp_sync() {
    if (g_in_frame) return;
    g_in_frame = true;
    uint64_t due = pf_now() / 1000 * 441 / 10000;      // 44,100 frames a second of guest time
    while (g_frames < due) frame();
    g_in_frame = false;
    if (g_out.empty()) return;
    size_t n = g_out.size() / 2;
    if (g_wav) {
        std::fwrite(g_out.data(), 4, n, g_wav);
        g_wav_frames += n;
    }
    if (g_pf_audio_out) g_pf_audio_out(g_out.data(), n);
    g_out.clear();
}
