// 3dokit runtime -- the DSP: the instruments the audio folio loads, as native code, the bus they
// mix into, the DMA channels that feed their FIFOs, and the sound that comes out.
//
// On the console the folio loads each instrument's DSP code (its .dsp file's DCOD) into the DSP,
// links the running ones into one program in the order of their priorities, and the DSP runs it
// once a sample frame, 44,100 times a second. Here each instrument runs once a frame in the same
// order, one of two ways. The library's instruments the programs run so far load have a model:
// their code transliterated, an instruction at a time (python -m 3dokit.dsp FILE --dis reads it),
// known by its code, not its name (kModels' checksums). Every other instrument -- a game's own,
// as Immercenary's spires, or one of the library's no model was written for -- runs from its
// code through the DSP's interpreter (below), which pfboot --dsp-code uses for every instrument
// and --dsp-check holds every model against, frame for frame.
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
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
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
enum Kind { NONE, MIXER, SAMPLER, VARMONO8, DCSQXDHALFMONO, DCSQXDHALFSTEREO, ENVELOPE, FIXEDMONO, DIRECTOUT, DCSQXDMONO, HALFMONO8, NOISE };

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
    {"fixedmonosample.dsp", 0x379b0cfau, FIXEDMONO, 0},
    {"directout.dsp", 0x48203189u, DIRECTOUT, 0},
    {"dcsqxdmono.dsp", 0xcbf1a686u, DCSQXDMONO, 0, true},
    {"halfmono8.dsp", 0x9affb1e7u, HALFMONO8, 0, true},
    {"noise.dsp", 0x547cb548u, NOISE, 0},
};

struct Input { int32_t src; uint32_t rsrc; };

struct Unit {
    std::string file;                                   // its .dsp file's name, in lower case
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
    // directout.dsp's
    int ileft = -1, iright = -1;
    // dcsqxdmono.dsp's
    int dtoggle = -1;
    std::vector<int> in, left, right;
    // The interpreter's: the instrument's code and memory as it sees them (below), whether it runs
    // from them, its resources' words past their first, its ring's registers.
    std::shared_ptr<const struct Image> image;
    bool interp = false;
    std::vector<std::vector<int16_t>> more;
    std::array<int16_t, 32> regs{};
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
uint16_t fifo_take(int32_t item, Unit& u, int r) {
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

std::vector<uint16_t>* g_fifo_log;                      // --dsp-check: the words a model reads
uint16_t fifo_read(int32_t item, Unit& u, int r) {
    uint16_t v = fifo_take(item, u, r);
    if (g_fifo_log) g_fifo_log->push_back(v);
    return v;
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

// fixedmonosample.dsp (23.10): a word from the FIFO every frame, times Amplitude -- a sample at
// the DSP's own rate, with no status test (an empty FIFO reads 0).
void run_fixedmono(int32_t item, Unit& u) {
    int16_t s = (int16_t)fifo_read(item, u, u.in_fifo);
    u.mem[(size_t)u.out] = wb(mul(s, value(u, u.amp)));
}

// directout.dsp (23.10): InputLeft and InputRight added to the bus with CLIP, as a mixer's sums.
void run_directout(Unit& u) {
    g_bus_l = wb(add_clip(word(g_bus_l), word(value(u, u.ileft))));
    g_bus_r = wb(add_clip(word(g_bus_r), word(value(u, u.iright))));
}

// dcsqxdmono.dsp (23.10): SDX2 mono at the full rate, a byte a frame. Nothing at all while the
// FIFO's status says it is empty. dc_toggle steps by 0x8000: on a frame it goes negative a new word
// comes in (dc_hold) and its high byte is the frame's, otherwise the low byte of the one held; the
// byte squared with its sign, an odd byte adding the square to the sum (with CLIP), an even one the
// sum; Output the sum times Amplitude.
void run_dcsqxd_full(int32_t item, Unit& u) {
    if (!fifo_has(u, u.in_fifo)) return;
    std::vector<int16_t>& m = u.mem;
    auto& tg = m[(size_t)u.dtoggle];
    auto& sq = m[(size_t)u.square];
    auto& by = m[(size_t)u.byte];
    auto& hd = m[(size_t)u.hold];
    auto& ac = m[(size_t)u.accum];
    uint32_t y = word(tg) + word((int16_t)0x8000);
    tg = wb(y);
    if (y >> 31) {
        hd = (int16_t)fifo_read(item, u, u.in_fifo);
        y = word(hd) & 0xFF000000u;
    } else {
        y = word(hd) << 8;
    }
    by = wb(y);
    if (y >> 31) y = 0u - y;
    sq = wb(mul_acc(by, y));
    y = ((uint16_t)by & 0x100) ? add_clip(word(sq), word(ac)) : word(sq);
    ac = wb(y);
    m[(size_t)u.out] = wb(mul_acc(value(u, u.amp), y));
}

// halfmono8.dsp (23.10): 8-bit samples at half the rate, two to a word, the high byte first.
// Nothing at all while the FIFO's status says it is empty. CurState counts the frames: on the
// first of four a new word comes in (SampleHold) and the frame plays the mean of the last value
// and its high byte; on the second the high byte is the last value, and plays; the third the mean
// of it and the low byte; the fourth the low byte, the last value now. Output times Amplitude.
void run_halfmono8(int32_t item, Unit& u) {
    if (!fifo_has(u, u.in_fifo)) return;
    std::vector<int16_t>& m = u.mem;
    auto& cs = m[(size_t)u.state];
    auto& sh = m[(size_t)u.hold];
    auto& pv = m[(size_t)u.prev];
    cs = wb(word(cs) + word(1));
    uint32_t y;
    bool low = cs & 2;
    if (!(cs & 1)) {
        if (!low) {
            sh = (int16_t)fifo_read(item, u, u.in_fifo);
            y = word(sh) & 0xFF000000u;
        } else {
            y = word(sh) << 8;
        }
        y = ((uint32_t)((int32_t)y >> 1)) & kAlu;
        y = mul(pv, 0x4000) + y;
    } else {
        y = low ? word(sh) << 8 : word(sh) & 0xFF000000u;
        pv = wb(y);
    }
    m[(size_t)u.out] = wb(mul_acc(value(u, u.amp), y));
}

// noise.dsp (23.10): the DSP's noise register (I memory 0x0EA, a new value each read) times
// Amplitude. The hardware's sequence is not known (Opera reads a generic generator there): a fixed
// xorshift32's top 16 bits, so that a run sounds the same every time.
uint32_t g_noise = 0x2545F491u;
uint16_t noise_next() {
    g_noise ^= g_noise << 13;
    g_noise ^= g_noise >> 17;
    g_noise ^= g_noise << 5;
    return (uint16_t)(g_noise >> 16);
}
void run_noise(Unit& u) {
    u.mem[(size_t)u.out] = wb(mul((int16_t)noise_next(), value(u, u.amp)));
}

void run_model(int32_t item, Unit& u);

// ---- the interpreter --------------------------------------------------------------------------
// An instrument run from its code, an instruction at a time, as the FreeDO emulator reads the DSP
// (Opera carries it; read, not copied): what plays the instruments no model was written for
// (Immercenary's spires), and what pfboot --dsp-check holds each model against.
//
// Each instrument sees the DSP's data memory as the folio would lay it out for it alone: its
// variables and knobs in I memory from 0x110, a resource's words in a row; its ring's registers at
// RBASE 8 (0x008-0x00f, 0x108-0x10f, 0x208-0x20f, 0x308-0x30f, by RMAP); its input FIFOs at 0x0f0
// + n and their status at 0x0d0 + n, its output FIFOs at 0x3f0 + n and theirs at 0x0e0 + n. Its
// code is at 0 and the subroutines it imports after it, the relocations applied as 3dokit.dsp
// reads them: a chain of words given a resource's address, an offset given the code's place,
// RBASE given the ring. Every other address is the DSP's own, shared: 0x0ea the noise register,
// 0x106 and 0x107 the bus, the rest of I memory (0x102, the decoders' and BadSpire's scratch).
// Where the folio really puts things is not read here; nothing in the code can tell.
//
// The DSP cannot write EI memory (below 0x100: the knobs, the FIFOs, the registers there), and
// here not a knob either; a FIFO's status reads 2 when it has a word to read (as Opera's), else 0;
// an output FIFO takes nothing, and EO memory (0x300 on) is not kept.
//
// An ALU instruction loads its operands in order into the latches it asks for -- the multiplier's
// one or two, then the ALU's A and B, then the shifter's -- less those OP_MASK holds; a result is
// written, as its top 16 bits, to the operand marked write-back, else to the operand left over
// when there is one. The multiplier on the accumulator is a * ((y >> 15) & ~1) on either ALU
// input (Opera's B input clears the bit after the product: one multiplier, read as A's). ADDC and
// SUBB take the carry as B (0x10000), and as A, when A is the multiplier on the accumulator, the
// first latch or 0. Every ALU instruction sets N, Z (the top 16 bits) and the exact zero (bits 12
// to 15), and clears C and V unless it adds or subtracts; then the shifter: its code, the 16 for a
// logical operation added, or an operand's value for "<<op".

const int kSteps = 4096;                                // a frame's instructions, at most
const uint32_t kRing = 0x008;                           // RBASE's address for an instrument's ring

enum Cell : uint8_t { C_DSP, C_RSRC, C_KNOB, C_REG, C_IFIFO, C_ISTAT, C_OFIFO, C_OSTAT };
struct Place {
    Cell kind = C_DSP;
    uint16_t rsrc = 0, word = 0;                        // the resource and its word; a register's index
};
struct Image {
    std::vector<uint16_t> code;                         // N memory, 0x400 words
    std::array<Place, 0x400> at;                        // the data memory, as the instrument sees it
};

struct Cpu {
    uint32_t y = 0;                                     // the accumulator
    bool n = false, z = false, c = false, v = false, x = false;      // x: the exact zero
    int16_t m1 = 0, m2 = 0, a1 = 0, a2 = 0;             // the latches: the multiplier's, the ALU's
    uint8_t opmask = 0x1f;                              // the latches loaded: 16 m1, 8 m2, 4 a1, 2 a2, 1 the shifter
    uint32_t rbase = 0;
    int rmap = 0;
};
Cpu g_cpu;
std::array<int16_t, 0x400> g_imem;                      // the DSP's own I memory

// --dsp-check's: the FIFO words the model read, which the code reads back in their order, and the
// DSP's I memory as it was before the code ran.
const std::vector<uint16_t>* g_replay;
size_t g_replay_at;
bool g_replay_short;
std::vector<std::pair<uint32_t, int16_t>> g_undo;

inline bool sub_carry(uint32_t a, uint32_t b, uint32_t y) { return ((a & ~b) | (a & ~y) | (~b & ~y)) >> 31; }
inline bool sub_overflow(uint32_t a, uint32_t b, uint32_t y) { return ((a & ~b & ~y) | (~a & b & y)) >> 31; }

// A register's address: R0-R7 and R8-R15 in banks RMAP picks, XORed with RBASE.
uint32_t reg_addr(unsigned r) {
    bool x = r >> 2 & 1, y = r >> 3 & 1, b;
    switch (g_cpu.rmap) {
    case 4: b = y; break;
    case 5: b = !y; break;
    case 6: b = x && y; break;
    case 7: b = x || y; break;
    default: b = x; break;
    }
    return ((r & 7) | (uint32_t)b << 8 | (r >> 3) << 9) ^ g_cpu.rbase;
}

uint16_t rd(int32_t item, Unit& u, uint32_t a) {
    a &= 0x3ff;
    const Place& p = u.image->at[a];
    switch (p.kind) {
    case C_RSRC:
    case C_KNOB: return (uint16_t)(p.word ? u.more[p.rsrc][p.word - 1u] : value(u, p.rsrc));
    case C_REG: return (uint16_t)u.regs[p.word];
    case C_IFIFO:
        if (g_replay) {
            if (g_replay_at < g_replay->size()) return (*g_replay)[g_replay_at++];
            g_replay_short = true;
            return 0;
        }
        return fifo_read(item, u, p.rsrc);
    case C_ISTAT: return fifo_has(u, p.rsrc) ? 2 : 0;
    case C_OFIFO:
    case C_OSTAT: return 0;
    case C_DSP: break;
    }
    if (a == 0x0ea) return noise_next();
    if (a == 0x106) return (uint16_t)g_bus_l;
    if (a == 0x107) return (uint16_t)g_bus_r;
    if (a >= 0x100 && a < 0x300) return (uint16_t)g_imem[a];
    return 0;                                           // the hardware's other words: not modelled
}

void wr(Unit& u, uint32_t a, uint16_t v) {
    a &= 0x3ff;
    if (a < 0x100) return;
    const Place& p = u.image->at[a];
    if (p.kind == C_RSRC) (p.word ? u.more[p.rsrc][p.word - 1u] : u.mem[p.rsrc]) = (int16_t)v;
    else if (p.kind == C_REG) u.regs[p.word] = (int16_t)v;
    else if (p.kind != C_DSP) return;
    else if (a == 0x106) g_bus_l = (int16_t)v;
    else if (a == 0x107) g_bus_r = (int16_t)v;
    else if (a < 0x300) {
        if (g_replay) g_undo.push_back({a, g_imem[a]});
        g_imem[a] = (int16_t)v;
    }
}

// An immediate operand: 13 bits, signed, shifted up 3 when bit 13 is set.
uint16_t immediate(uint16_t o) {
    int32_t v = (int32_t)(o & 0x1fff) - (o & 0x1000 ? 0x2000 : 0);
    return (uint16_t)(o & 0x2000 ? v * 8 : v);
}

// MOVE's and MOVEREG's one operand: of three registers only the first (R3), of two the second.
uint16_t operand1(int32_t item, Unit& u, uint32_t& pc) {
    uint16_t o = u.image->code[pc++ & 0x3ff], v;
    if (!(o & 0x8000)) {
        v = rd(item, u, reg_addr(o >> 10 & 0xf));
        return o >> 14 & 1 ? rd(item, u, v) : v;
    }
    switch (o >> 13) {
    case 4:
        v = rd(item, u, o & 0x3ff);
        return o & 0x400 ? rd(item, u, v) : v;
    case 5:
        v = rd(item, u, reg_addr(o & 0xf));
        return o & 0x10 ? rd(item, u, v) : v;
    default: return immediate(o);
    }
}

void alu(int32_t item, Unit& u, uint16_t w, uint32_t& pc) {
    Cpu& k = g_cpu;
    unsigned muxa = w >> 10 & 3, muxb = w >> 8 & 3, op = w >> 4 & 0xf, n = w >> 13 & 3;
    bool m2sel = w >> 12 & 1;
    unsigned req = (w & 0xf) == 8 ? 1 : 0;
    for (unsigned m : {muxa, muxb}) req |= m == 1 ? 4 : m == 2 ? 2 : m == 3 ? (m2sel ? 24 : 16) : 0;
    // The operands. With no count but latches to load, four.
    unsigned want = n ? n : req ? 4 : 0, got = 0;
    uint16_t ops[6];
    uint32_t last = 0, marked = 0;
    while (got < want) {
        uint16_t o = u.image->code[pc++ & 0x3ff];
        if (!(o & 0x8000)) {                            // three registers, R3, R2, R1
            for (int sh : {10, 5, 0}) {
                last = reg_addr(o >> sh & 0xf);
                uint16_t v = rd(item, u, last);
                ops[got++] = o >> (sh + 4) & 1 ? rd(item, u, v) : v;
            }
        } else if (o >> 13 == 4) {                      // an address
            last = o & 0x3ff;
            uint16_t v = rd(item, u, last);
            ops[got++] = o & 0x400 ? rd(item, u, v) : v;
            if (o & 0x800) marked = last;
        } else if (o >> 13 == 5) {                      // one register, or two
            auto one = [&](unsigned r, bool di, bool mark) {
                last = reg_addr(r);
                if (di) last = rd(item, u, last);
                ops[got++] = rd(item, u, last);
                if (mark) marked = last;
            };
            if (o & 0x400) one(o >> 5 & 0xf, o >> 9 & 1, o >> 12 & 1);
            one(o & 0xf, o >> 4 & 1, o >> 11 & 1);
        } else {
            ops[got] = immediate(o);
            last = ops[got++];
        }
    }
    req &= k.opmask;
    unsigned i = 0;
    if (req & 16) k.m1 = (int16_t)ops[i++];
    if (req & 8) k.m2 = (int16_t)ops[i++];
    if (req & 4) k.a1 = (int16_t)ops[i++];
    if (req & 2) k.a2 = (int16_t)ops[i++];
    unsigned bs = (w & 0xf) | (op & 8) << 1;
    if (req & 1) bs = ops[i++];
    uint32_t dest = got != i && !marked ? last : marked;

    auto product = [&]() -> uint32_t {
        return m2sel ? mul(k.m1, k.m2) : (uint32_t)((int64_t)k.m1 * (((int32_t)k.y >> 15) & ~1)) & kAlu;
    };
    bool carry_in = op == 3 || op == 5;
    auto input = [&](unsigned m) -> uint32_t {
        return m == 0 ? k.y : m == 1 ? word(k.a1) : m == 2 ? word(k.a2) : product();
    };
    uint32_t a = muxa == 3 && !m2sel && carry_in ? (k.c ? word(k.m1) : 0) : input(muxa);
    uint32_t b = carry_in ? (k.c ? 0x10000u : 0) : input(muxb);
    uint32_t y = a;
    k.c = k.v = false;
    switch (op) {
    case 1: y = 0u - b; k.c = sub_carry(0, b, y); k.v = sub_overflow(0, b, y); break;
    case 2: case 3: y = a + b; k.c = add_carry(a, b, y); k.v = add_overflow(a, b, y); break;
    case 4: case 5: y = a - b; k.c = sub_carry(a, b, y); k.v = sub_overflow(a, b, y); break;
    case 6: y = a + 0x1000; k.c = add_carry(a, 0x1000, y); k.v = add_overflow(a, 0x1000, y); break;
    case 7: y = a - 0x1000; k.c = sub_carry(a, 0x1000, y); k.v = sub_overflow(a, 0x1000, y); break;
    case 9: y = a ^ kAlu; break;
    case 10: y = a & b; break;
    case 11: y = (a & b) ^ kAlu; break;
    case 12: y = a | b; break;
    case 13: y = (a | b) ^ kAlu; break;
    case 14: y = a ^ b; break;
    case 15: y = a ^ b ^ kAlu; break;
    default: break;                                     // TRA, TRL
    }
    k.z = !(y & 0xFFFF0000u);
    k.n = y >> 31;
    k.x = !(y & 0xF000u);
    static const int kShift[] = {0, 1, 2, 3, 4, 5, 8};
    static const int kRight[] = {16, 8, 5, 4, 3, 2, 1};
    if ((bs & 15) >= 1 && (bs & 15) <= 6 && bs < 32) y <<= kShift[bs & 15];
    else if (bs == 7 || bs == 23) {                     // CLIP
        if (k.v) y = k.n ? 0x7FFFF000u : 0x80000000u;
    } else if (bs == 8 || bs == 24) {                   // the top 16 bits rotated left by one
        k.c = y >> 31;
        y = (y << 1 & 0xFFFE0000u) | (k.c ? 0x10000u : 0) | (y & 0xF000u);
    } else if (bs >= 9 && bs <= 15) y = (uint32_t)((int32_t)y >> kRight[bs - 9]) & kAlu;
    else if (bs >= 25 && bs <= 31) y = (y >> kRight[bs - 25]) & kAlu;
    k.y = y;
    if (dest) wr(u, dest, (uint16_t)(y >> 16));
}

// A conditional branch's bits 10-14 (two masks, a select, a mode; 3dokit.dsp's _condition):
// modes 1 and 2, the masked flags (N and V, or C and Z) all set, or all clear; mode 1 with no
// mask, the exact zero or not; mode 3, LT LE GE GT on N, V and Z, HI LS on C and Z, the exact
// tests.
bool taken(unsigned b) {
    const Cpu& k = g_cpu;
    bool m0 = b & 1, m1 = b >> 1 & 1, sel = b >> 2 & 1, mode0 = b >> 3 & 1, mode1 = b >> 4 & 1;
    if (mode0 != mode1) {
        if (m0 || m1) return (!m1 || (sel ? k.c : k.n) == mode0) && (!m0 || (sel ? k.z : k.v) == mode0);
        if (!mode0) return false;
        return sel != (k.z && k.x);
    }
    if (!mode0) return false;
    if (!sel) return ((k.n != k.v) || (k.z && m0)) != m1;
    if (!m1) return (k.c && !k.z) != m0;
    return k.x != m0;
}

// The instrument's frame, from its entry to SLEEP; false when it does not get there.
bool run_code(int32_t item, Unit& u) {
    Cpu& k = g_cpu;
    const std::vector<uint16_t>& code = u.image->code;
    uint32_t pc = 0, rts = 0;
    for (int steps = 0; steps < kSteps; ++steps) {
        uint16_t w = code[pc++ & 0x3ff];
        if (!(w & 0x8000)) {
            alu(item, u, w, pc);
            continue;
        }
        unsigned op = w >> 7 & 0xff;
        uint32_t to = w & 0x3ff;
        if (op == 7) return true;                       // SLEEP: the frame's end
        if (op == 1) pc = k.y >> 16 & 0x3ff;            // BAC
        else if (op == 2) k.rbase = (w & 0x3f) << 2;    // RBASE
        else if (op == 3) k.rmap = w & 7;               // RMAP
        else if (op == 4) pc = rts;                     // RTS
        else if (op == 5) k.opmask = ~w & 0x1f;         // OP_MASK
        else if (op < 8) continue;                      // NOP
        else if (op < 16 || (op >= 24 && op < 32)) pc = to;          // JUMP, BFM
        else if (op < 24) {                             // JSR
            rts = pc;
            pc = to;
        } else if (op < 48) {                           // MOVEREG
            uint16_t v = operand1(item, u, pc);
            uint32_t a = reg_addr(w & 0xf);
            wr(u, w & 0x10 ? rd(item, u, a) : a, v);
        } else if (op < 64) {                           // MOVE
            uint16_t v = operand1(item, u, pc);
            wr(u, w & 0x400 ? rd(item, u, to) : to, v);
        } else if (taken(w >> 10 & 0x1f)) pc = to;
    }
    return false;
}

// The instrument's image from its template: its resources given addresses, its code and the
// subroutines it imports placed and relocated. Null when all went well, else why not.
const char* place(const PfDspTemplate& t, Image& im) {
    im.code.assign(0x400, 0x8380);                      // N memory past the code: SLEEP
    std::vector<uint32_t> addr(t.names.size(), 0);
    uint32_t next = 0x110;
    unsigned nin = 0, nout = 0;
    bool ring = false;
    for (size_t i = 0; i < t.names.size(); ++i) {
        uint32_t ty = t.types[i], n = ty == 2 || ty == 3 ? std::max<uint32_t>(t.counts[i], 1) : 1;
        uint16_t r = (uint16_t)i;
        switch (ty) {
        case 1: case 2: case 3: case 9: case 10:        // knobs, variables, the ADCs (read 0)
            while (next <= 0x20f && next + n > 0x208) ++next;          // past the ring's registers
            if (next + n > 0x300) return "its resources do not fit in I memory";
            addr[i] = next;
            for (uint32_t w = 0; w < n; ++w) im.at[next + w] = {ty == 1 ? C_KNOB : C_RSRC, r, (uint16_t)w};
            next += n;
            break;
        case 5:
            if (ring) return "it has two rings";
            ring = true;
            addr[i] = kRing;
            for (uint32_t bank = 0; bank < 4; ++bank)
                for (uint32_t w = 0; w < 8; ++w) im.at[bank << 8 | (kRing + w)] = {C_REG, 0, (uint16_t)(bank * 8 + w)};
            break;
        case 6:
            if (nin == 13) return "it has more than 13 input FIFOs";
            addr[i] = 0x0f0 + nin;
            im.at[0x0f0 + nin] = {C_IFIFO, r, 0};
            im.at[0x0d0 + nin++] = {C_ISTAT, r, 0};
            break;
        case 7:
            if (nout == 4) return "it has more than 4 output FIFOs";
            addr[i] = 0x3f0 + nout;
            im.at[0x3f0 + nout] = {C_OFIFO, r, 0};
            im.at[0x0e0 + nout++] = {C_OSTAT, r, 0};
            break;
        case 0: case 8: case 0x4000: case 0x8000: break;
        default: return "it has a resource of a type not known";
        }
    }
    auto load = [&](const PfDspCode& c, size_t base) -> const char* {
        size_t n = c.words.size() / 2;
        if (base + n > 0x400) return "its code does not fit in N memory";
        for (size_t w = 0; w < n; ++w) im.code[base + w] = (uint16_t)(c.words[2 * w] << 8 | c.words[2 * w + 1]);
        for (const auto& [mask, zero, idx, off] : c.relocs)
            if (mask == 0x10a00 && off < n) {           // an offset in the code: the code's place added
                uint16_t& x = im.code[base + off];
                x = (uint16_t)((x & 0xfc00) | ((x + base) & 0x3ff));
            }
        return nullptr;
    };
    if (const char* why = load(t.code, 0)) return why;
    size_t end = t.code.words.size() / 2;
    for (size_t i = 0; i < t.names.size(); ++i) {
        if (t.types[i] != 0x8000) continue;
        auto it = t.imports.find((uint32_t)i);
        if (it == t.imports.end()) return "a subroutine it imports was not found";
        for (const auto& rl : it->second.first.relocs)
            if (rl[0] != 0x10a00) return "a subroutine it imports has a relocation of a kind not known";
        if (const char* why = load(it->second.first, end)) return why;
        addr[i] = (uint32_t)end + it->second.second;
        end += it->second.first.words.size() / 2;
    }
    for (const auto& [mask, zero, idx, off] : t.code.relocs) {
        if (idx >= t.names.size() || off >= t.code.words.size() / 2) return "a relocation out of its range";
        uint32_t ty = t.types[idx], a = addr[idx];
        if (mask == 0x10a00) continue;
        if (mask == 0x600) {                            // RBASE: the ring
            if (ty != 5) return "an RBASE relocation not to a ring";
            im.code[off] = (uint16_t)((im.code[off] & ~0x3f) | a >> 2);
            continue;
        }
        if ((mask & 0xffffff) != 0x20a00 || mask >> 24 > 1) return "a relocation of a kind not known";
        if (mask >> 24) {                               // a FIFO's status
            if (ty == 6) a = 0x0d0 + (a - 0x0f0);
            else if (ty == 7) a = 0x0e0 + (a - 0x3f0);
            else return "a status relocation not to a FIFO";
        } else if (ty == 0 || ty == 8 || ty == 0x4000) return "a relocation to a resource with no address";
        for (uint32_t w = off, guard = 0; guard < 0x400; ++guard) {         // the chain
            uint16_t& x = im.code[w];
            uint32_t link = x & 0x3ff;
            x = (uint16_t)((x & 0xfc00) | a);
            if (!link) break;
            w = link;
        }
    }
    return nullptr;
}

// pfboot --dsp-check: a modelled instrument's frame run twice from the same state, by its model
// and from its code, and the two held against each other: every resource's word, the ring's
// registers against sampler.dsp's (R5 the phase, R6 and R7 its samples), the bus, the noise
// register's sequence, and the FIFO's words, which the code reads back in the order the model
// read them. The model's frame is the one kept.
struct Tally {
    uint64_t frames = 0, differ = 0;
};
std::map<std::string, Tally> g_tally;

void check_report() {
    for (const auto& [file, t] : g_tally)
        std::fprintf(stderr, "pfboot: --dsp-check: %-22s %10llu frames, %llu differ\n", file.c_str(),
                     (unsigned long long)t.frames, (unsigned long long)t.differ);
}

void check_frame(int32_t item, Unit& u) {
    if (g_tally.empty()) std::atexit(check_report);
    Tally& tally = g_tally[u.file];
    ++tally.frames;
    std::vector<int16_t> mem0 = u.mem;
    std::vector<std::vector<int16_t>> more0 = u.more;
    std::map<uint32_t, Fifo> fifo0 = u.fifo;
    int16_t bl0 = g_bus_l, br0 = g_bus_r, r5 = u.r5, r6 = u.r6, r7 = u.r7;
    uint32_t noise0 = g_noise;
    std::vector<uint16_t> log;
    g_fifo_log = &log;
    run_model(item, u);
    g_fifo_log = nullptr;

    std::vector<int16_t> mem1 = std::move(u.mem);
    std::vector<std::vector<int16_t>> more1 = std::move(u.more);
    std::map<uint32_t, Fifo> fifo1 = std::move(u.fifo);
    int16_t bl1 = g_bus_l, br1 = g_bus_r;
    uint32_t noise1 = g_noise;
    u.mem = mem0;
    u.more = more0;
    u.fifo = fifo0;
    if (u.kind == SAMPLER) {
        u.regs[13] = r5;
        u.regs[14] = r6;
        u.regs[15] = r7;
    }
    g_bus_l = bl0;
    g_bus_r = br0;
    g_noise = noise0;
    Cpu cpu = g_cpu;
    g_replay = &log;
    g_replay_at = 0;
    g_replay_short = false;
    bool slept = run_code(item, u);
    g_replay = nullptr;

    char why[160] = "";
    if (!slept) std::snprintf(why, sizeof why, "the code runs %d instructions without SLEEP", kSteps);
    else if (g_replay_short || g_replay_at != log.size())
        std::snprintf(why, sizeof why, "the model reads %zu FIFO words, the code %s", log.size(),
                      g_replay_short ? "more" : std::to_string(g_replay_at).c_str());
    for (size_t r = 0; !*why && r < mem1.size(); ++r)
        if (u.mem[r] != mem1[r] || u.more[r] != more1[r])
            std::snprintf(why, sizeof why, "%s: the model %d, the code %d", u.names[r].c_str(), mem1[r], u.mem[r]);
    if (!*why && (g_bus_l != bl1 || g_bus_r != br1))
        std::snprintf(why, sizeof why, "the bus: the model %d %d, the code %d %d", bl1, br1, g_bus_l, g_bus_r);
    if (!*why && g_noise != noise1) std::snprintf(why, sizeof why, "the noise register read a different number of times");
    if (!*why && u.kind == SAMPLER && (u.regs[13] != u.r5 || u.regs[14] != u.r6 || u.regs[15] != u.r7))
        std::snprintf(why, sizeof why, "R5 R6 R7: the model %d %d %d, the code %d %d %d", u.r5, u.r6, u.r7, u.regs[13],
                      u.regs[14], u.regs[15]);
    if (*why && !tally.differ++)
        std::fprintf(stderr, "pfboot: --dsp-check: %s differs at frame %llu: %s\n", u.file.c_str(),
                     (unsigned long long)g_frames, why);

    for (auto it = g_undo.rbegin(); it != g_undo.rend(); ++it) g_imem[it->first] = it->second;
    g_undo.clear();
    g_cpu = cpu;
    u.mem = std::move(mem1);
    u.more = std::move(more1);
    u.fifo = std::move(fifo1);
    g_bus_l = bl1;
    g_bus_r = br1;
    g_noise = noise1;
}

void run_model(int32_t item, Unit& u) {
        switch (u.kind) {
        case MIXER: run_mixer(u); break;
        case SAMPLER: run_sampler(item, u); break;
        case VARMONO8: run_varmono8(item, u); break;
        case DCSQXDHALFMONO: run_dcsqxd(item, u); break;
        case DCSQXDHALFSTEREO: run_dcsqxd_stereo(item, u); break;
        case ENVELOPE: run_envelope(u); break;
        case FIXEDMONO: run_fixedmono(item, u); break;
        case DIRECTOUT: run_directout(u); break;
        case DCSQXDMONO: run_dcsqxd_full(item, u); break;
        case HALFMONO8: run_halfmono8(item, u); break;
        case NOISE: run_noise(u); break;
        case NONE: break;
        }
}

// One frame: head.dsp first (the bus to the DAC, and cleared), then the running instruments, each
// by its model or from its code. The DSP's own state starts the frame as Opera starts it: the
// accumulator and the flags clear, RBASE and RMAP 0, OP_MASK none; the latches go on.
void frame() {
    g_out.push_back(g_bus_l);
    g_out.push_back(g_bus_r);
    g_bus_l = g_bus_r = 0;
    Cpu& k = g_cpu;
    k.y = 0;
    k.n = k.z = k.c = k.v = k.x = false;
    k.rbase = 0;
    k.rmap = 0;
    k.opmask = 0x1f;
    std::vector<int32_t> order = g_order;
    for (int32_t item : order) {
        auto it = g_units.find(item);
        if (it == g_units.end()) continue;
        Unit& u = it->second;
        if (u.interp) {
            if (!run_code(item, u)) {
                std::fprintf(stderr, "pfboot: the DSP instrument %s ran %d instructions without SLEEP: silent from now\n",
                             u.file.c_str(), kSteps);
                u.interp = false;
            }
        } else if (g_pf_dsp_check && u.image && u.kind != NONE) check_frame(item, u);
        else run_model(item, u);
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
    g_imem.fill(0);
    g_cpu = Cpu();
}

bool g_pf_dsp_code, g_pf_dsp_check;

// A new instrument: by its model when one was written for its code, else from its code. Silent,
// and saying so once, when neither can be.
void pf_dsp_new(int32_t ins, const PfDspTemplate& t, uint8_t pri) {
    Unit u;
    u.pri = pri;
    u.names = t.names;
    u.mem.assign(t.names.size(), 0);
    u.more.resize(t.names.size());
    for (size_t r = 0; r < t.names.size(); ++r)
        if ((t.types[r] == 2 || t.types[r] == 3) && t.counts[r] > 1) u.more[r].assign(t.counts[r] - 1, 0);
    uint32_t sum = fnv(t.code.words);
    std::string base = t.file.substr(t.file.find_last_of("/\\") + 1);
    for (char& ch : base) ch = (char)std::tolower((unsigned char)ch);
    u.file = base;
    for (const Model& m : kModels)
        if (base == m.file && m.sum == sum) {
            u.kind = m.kind;
            u.inputs = m.inputs;
            u.gated = m.gated;
        }
    auto image = std::make_shared<Image>();
    const char* why = place(t, *image);
    if (!why) u.image = image;
    u.interp = u.image && (g_pf_dsp_code || u.kind == NONE);
    if (!g_told.count(base) && (!why ? u.interp && g_pf_trace : u.kind == NONE || g_pf_dsp_code || g_pf_dsp_check)) {
        g_told.insert(base);
        if (why) std::fprintf(stderr, "pfboot: the DSP instrument %s (code 0x%08x) cannot run from its code: %s%s\n",
                              base.c_str(), sum, why, u.kind == NONE ? "; silent" : "");
        else pf_log("pfboot: the DSP instrument %s (code 0x%08x) runs from its code\n", base.c_str(), sum);
    }
    u.out = rsrc_named(u, "Output");
    u.amp = rsrc_named(u, "Amplitude");
    u.freq = rsrc_named(u, "Frequency");
    u.in_fifo = rsrc_named(u, "InFIFO");
    u.phase = rsrc_named(u, "Phase");
    u.oldv = rsrc_named(u, "OldVal");
    u.newv = rsrc_named(u, "NewVal");
    u.toggle = rsrc_named(u, "Toggle");
    u.hold = rsrc_named(u, u.kind == DCSQXDHALFMONO || u.kind == DCSQXDHALFSTEREO || u.kind == DCSQXDMONO ? "dc_hold"
                                                                                                      : "SampleHold");
    u.dtoggle = rsrc_named(u, "dc_toggle");
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
    u.ileft = rsrc_named(u, "InputLeft");
    u.iright = rsrc_named(u, "InputRight");
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
