// 3dokit runtime -- the cel engine: what the Graphics folio's DrawCels sets going. GRAPHIX writes
// the bitmap's control word and its four REGCTL words into MADAM, hands it the first CCB and
// waits for the hardware to finish; here the hardware is this file. What it does is what the
// 3DO Graphics Programmer's Guide says the cel engine does (chapter 3, "Understanding the Cel
// Engine and SPORT", and chapter 5, "Using the Cel Engine"; the SDK's hardware.h names the bits).
// Where the guide is silent or contradicts itself, the Opera emulator's MADAM was read (3dokit.cel
// did the same): the arithmetic of the pixel processor, the projector's V and H bits, the blue LSB
// and the end of a packed row, and the origin a cel leaves behind. Each such place says so.
//
// What no program run so far draws stops the run instead of guessing. Above all the projector
// draws only cels that land square on the frame buffer's pixels -- HDX 1, VDY 1, HDY, VDX, HDDX
// and HDDY 0, the origin on a whole pixel -- where each source pixel is one frame pixel whatever
// rule fills a stretched or turned one; that rule is not read yet.
#include "pf.h"
#include <cstdio>

namespace {

enum : uint32_t {
    CCB_SKIP = 0x80000000u, CCB_LAST = 0x40000000u, CCB_NPABS = 0x20000000u, CCB_SPABS = 0x10000000u,
    CCB_PPABS = 0x08000000u, CCB_LDSIZE = 0x04000000u, CCB_LDPRS = 0x02000000u, CCB_LDPPMP = 0x01000000u,
    CCB_LDPLUT = 0x00800000u, CCB_CCBPRE = 0x00400000u, CCB_YOXY = 0x00200000u, CCB_ACW = 0x00040000u,
    CCB_ACCW = 0x00020000u, CCB_TWD = 0x00010000u, CCB_PXOR = 0x00000800u, CCB_USEAV = 0x00000400u,
    CCB_PACKED = 0x00000200u, CCB_PLUTPOS = 0x00000040u, CCB_BGND = 0x00000020u, CCB_NOBLK = 0x00000010u,
    PRE0_LITERAL = 0x80000000u, PRE0_BGND = 0x40000000u, PRE0_UNCODED = 0x10u, PRE0_REP8 = 0x08u,
    PRE1_NOSWAP = 0x4000u, PRE1_LRFORM = 0x800u,
    SWAPHV = 0x08000000u,
};

// The engine's registers: what a CCB loads stays until another loads it, from cel to cel and from
// one DrawCels to the next.
struct Engine {
    int32_t x, y;                       // the origin, 16.16
    int32_t hdx, hdy, vdx, vdy, hddx, hddy;     // HDX, HDY, HDDX, HDDY 12.20; VDX, VDY 16.16
    uint32_t pixc;
    uint16_t plut[32];
};
Engine g_ce;

// Where a CCB's pointer leads: absolute, or relative to the word that holds it -- the target less
// the word's address less 4 (graphics.h's MakeCCBRelative). 24 bits (Opera).
uint32_t pointer(uint32_t field, bool absolute) {
    uint32_t v = pf_r32(field) & 0xFFFFFF;
    return absolute ? v : (field + v + 4) & 0xFFFFFF;
}

// The source data's bits, most significant first, from a word-aligned start.
struct Bits {
    uint32_t base, pos = 0;
    uint32_t take(unsigned n) {
        uint32_t v = 0;
        while (n) {
            uint32_t w = pf_r32(base + (pos >> 5) * 4), have = 32 - (pos & 31), k = n < have ? n : have;
            v = v << k | ((w >> (have - k)) & ((1u << k) - 1));
            pos += k;
            n -= k;
        }
        return v;
    }
};

struct Target {
    uint32_t cecontrol, write, read, width;     // the buffers and their width in pixels
    int32_t xclip, yclip;                       // the last pixel drawn into, each way
};

// A frame buffer pixel: the 3DO's line pairs, row y's pixel in the high half of the word at
// (y >> 1) * width + x when y is even, the low half when it is odd.
uint32_t fb_addr(uint32_t base, uint32_t width, int32_t x, int32_t y) {
    return base + ((uint32_t)(y >> 1) * width + (uint32_t)x) * 4;
}
uint32_t fb_read(uint32_t base, uint32_t width, int32_t x, int32_t y) {
    uint32_t w = pf_r32(fb_addr(base, width, x, y));
    return y & 1 ? w & 0xFFFF : w >> 16;
}
void fb_write(uint32_t base, uint32_t width, int32_t x, int32_t y, uint32_t v) {
    uint32_t a = fb_addr(base, width, x, y), w = pf_r32(a);
    pf_w32(a, y & 1 ? (w & 0xFFFF0000u) | v : (w & 0xFFFFu) | v << 16);
}

// The blue LSB going into the pixel processor (the control word's PDCLSB for a packed cel, the
// second preamble word's TLLSB for an unpacked one, and CFBDLSB for a frame buffer pixel): 0, bit
// 0 as it is, bit 4 or bit 5.
uint32_t blue_lsb(uint32_t p, uint32_t mode) {
    static const int from[4] = {-1, 0, 4, 5};
    uint32_t b = from[mode & 3] < 0 ? 0 : (p >> from[mode & 3]) & 1;
    return (p & ~1u) | b;
}

struct Cel {
    uint32_t flags, pre0, pre1, src;
    int bpp;                                    // 1, 2, 4, 6, 8, 16
    bool coded;
};

// The pixel decoder: a source pixel to a 16-bit colour (bit 15 the P-mode and V bit) and its three
// 3-bit multipliers (AMV, red 6-8, green 3-5, blue 0-2; 0x49 -- all 1 -- when the pixel has none).
// Coded pixels look up the PLUT: 1, 2 and 4 bits take the missing high index bits from the CCB's
// PLUTA; 6 bits the low five, bit 5 becoming bit 15; 8 bits the low five, the top three an AMV for
// all three; 16 bits the low five, bits 5-13 the AMVs, bit 15 kept. Uncoded 16 bits are the colour.
uint32_t decode(ArmCpu& c, const Cel& cel, uint32_t v, uint32_t& amv) {
    amv = 0x49;
    uint32_t pluta = cel.flags & 15;
    switch (cel.bpp) {
    case 1: return g_ce.plut[(pluta << 1 | v) & 31];
    case 2: return g_ce.plut[((pluta & 14) << 1 | v) & 31];
    case 4: return g_ce.plut[((pluta & 8) << 1 | v) & 31];
    case 6: return (g_ce.plut[v & 31] & 0x7FFFu) | (v >> 5 & 1) << 15;
    case 8:
        if (!cel.coded) pf_stop(c, "the cel engine: an uncoded 8-bit cel: not yet");
        amv = (v >> 5) * 0x49;
        return g_ce.plut[v & 31];
    default:
        if (!cel.coded) return v;
        amv = (v >> 11 & 7) << 6 | (v >> 8 & 7) << 3 | (v >> 5 & 7);
        return (g_ce.plut[v & 31] & 0x7FFFu) | (v & 0x8000u);
    }
}

// The pixel processor, one P-mode (a half of PIXC) on each 5-bit component: the primary source
// (1S: the decoded pixel or the frame buffer's) times the multiplier (MS 00: MF + 1; 01: the
// pixel's AMV + 1), shifted down by the divider (DF: 16, 2, 4, 8), plus the secondary source (2S:
// 0, AV, the frame buffer's, the pixel's), then halved when 2D is set, then held within 0 to 31.
// Each stage drops its fraction (Opera; the guide gives no rounding).
uint32_t ppmp(ArmCpu& c, uint32_t mode, uint32_t pix, uint32_t fb, uint32_t amv) {
    uint32_t s1 = mode >> 15 & 1, ms = mode >> 13 & 3, mf = mode >> 10 & 7, df = mode >> 8 & 3,
             s2 = mode >> 6 & 3, av = mode >> 1 & 31, d2 = mode & 1;
    if (ms >= 2) pf_stop(c, "the cel engine: a multiplier from the pixel's colour (PIXC MS 10, 11): not yet");
    static const int shift[4] = {4, 1, 2, 3};
    uint32_t in = s1 ? fb : pix, out = 0;
    for (int k = 0; k < 3; ++k) {
        int sh = 10 - 5 * k;                    // red, green, blue
        uint32_t m = ms ? (amv >> (6 - 3 * k) & 7) + 1 : mf + 1;
        int32_t a = (int32_t)(((in >> sh & 31) * m) >> shift[df]);
        int32_t b = s2 == 0 ? 0 : s2 == 1 ? (int32_t)av : (int32_t)((s2 == 2 ? fb : pix) >> sh & 31);
        int32_t r = (a + b) >> d2;
        out |= (uint32_t)(r > 31 ? 31 : r) << sh;
    }
    return out;
}

// One cel: its source pixels through the decoder and the pixel processor into the frame buffer.
void draw(ArmCpu& c, const Cel& cel, const Target& t) {
    if (cel.pre0 & (PRE0_LITERAL | PRE0_BGND)) pf_stop(c, "the cel engine: PRE0's LITERAL or BGND bit: not yet");
    if (cel.pre0 >> 24 & 15) pf_stop(c, "the cel engine: SKIPX: not yet");
    if (!(cel.flags & CCB_PACKED) && (cel.pre1 & PRE1_LRFORM)) pf_stop(c, "the cel engine: an LRFORM cel: not yet");
    if (cel.flags & (CCB_PXOR | CCB_USEAV)) pf_stop(c, "the cel engine: PXOR or USEAV: not yet");
    if (cel.flags & CCB_TWD) pf_stop(c, "the cel engine: TWD: not yet");
    if ((cel.flags & (CCB_ACW | CCB_ACCW)) != (CCB_ACW | CCB_ACCW))
        pf_stop(c, "the cel engine: a cel without both ACW and ACCW (which way a square cel turns is not read): not yet");
    if (g_ce.hdx != 1 << 20 || g_ce.hdy || g_ce.vdx || g_ce.vdy != 1 << 16 || g_ce.hddx || g_ce.hddy ||
        (g_ce.x & 0xFFFF) || (g_ce.y & 0xFFFF))
        pf_stop(c, "the cel engine: a cel stretched, turned or off the pixel grid (the projector's fill rule): not yet");
    uint32_t pover = cel.flags >> 7 & 3;
    if (pover == 1) pf_stop(c, "the cel engine: POVER 01: not yet");
    uint32_t cec = t.cecontrol;
    if ((cec >> 30) == 2) pf_stop(c, "the cel engine: B15POS 10: not yet");
    bool packed = cel.flags & CCB_PACKED;
    uint32_t lsb = packed ? cec >> 20 & 3 : cel.pre1 >> 12 & 3;

    // The V and H bits a pixel is written with (Opera): the decoder's (PLUTPOS) or the origin's
    // sub-pixel position -- here always on a whole pixel, so 0; swapped by SWAPHV unless PRE1's
    // NOSWAP; then B15POS (0, 1, or as it is) and B0POS (0, 1, the pixel processor's blue LSB, or
    // as it is). The control word's CFBDSUB is not modelled (nor is it in Opera).
    auto vh_out = [&](uint32_t dec, uint32_t pp) {
        uint32_t vh = cel.flags & CCB_PLUTPOS ? dec & 0x8001u : 0;
        if ((cec & SWAPHV) && !(!packed && (cel.pre1 & PRE1_NOSWAP))) vh = vh >> 15 | (vh & 1) << 15;
        switch (cec >> 30) { case 0: vh &= ~0x8000u; break; case 1: vh |= 0x8000u; break; }
        switch (cec >> 28 & 3) {
        case 0: vh &= ~1u; break;
        case 1: vh |= 1; break;
        case 2: vh = (vh & ~1u) | (pp & 1); break;
        }
        return (pp & 0x7FFEu) | vh;
    };

    int32_t x0 = g_ce.x >> 16, y = g_ce.y >> 16;
    int rows = (int)(cel.pre0 >> 6 & 0x3FF) + 1;
    auto pixel = [&](int32_t x, uint32_t v) {
        uint32_t amv, dec = decode(c, cel, v, amv);
        if (x < 0 || x > t.xclip || y < 0 || y > t.yclip) return;
        if (!(dec & 0x7FFF) && !(cel.flags & CCB_BGND)) return;     // transparent, before the LSB
        uint32_t pix = blue_lsb(dec, lsb);
        uint32_t fb = blue_lsb(fb_read(t.read, t.width, x, y), cec >> 22 & 3);
        uint32_t pm = pover == 2 ? 0 : pover == 3 ? 1 : pix >> 15;
        uint32_t pp = ppmp(c, pm ? g_ce.pixc >> 16 : g_ce.pixc & 0xFFFF, pix, fb, amv);
        if (!(pp & 0x7FFF) && !(cel.flags & CCB_NOBLK)) pp = 1 << 10;   // black is written as red 1
        fb_write(t.write, t.width, x, y, vh_out(dec, pp));
    };

    uint32_t row = cel.src;
    for (int j = 0; j < rows; ++j, ++y) {
        Bits in{row};
        if (packed) {
            // A row opens with its length in words less 2 (8 bits below 8 bpp, 10 from 8 up, in a
            // byte or two of their own), then packets: 2 bits of type (0 end of row, 1 literal, 2
            // transparent, 3 repeat), 6 of count less 1, the pixels. The row also ends where its
            // words do (Opera; the guide calls the end-of-row packet optional).
            uint32_t len = (cel.bpp >= 8 ? in.take(16) & 0x3FF : in.take(8)) + 2, end = len * 32;
            int32_t x = x0;
            while (in.pos + 2 <= end) {
                uint32_t type = in.take(2);
                if (!type) break;
                uint32_t n = in.take(6) + 1;
                if (type == 2) { x += (int32_t)n; continue; }
                uint32_t v = type == 3 ? in.take((unsigned)cel.bpp) : 0;
                for (uint32_t i = 0; i < n; ++i, ++x) pixel(x, type == 1 ? in.take((unsigned)cel.bpp) : v);
            }
            row += len * 4;
        } else {
            // TLHPCNT + 1 pixels a row, the rows WOFFSET + 2 words apart (WOFFSET(10) from 8 bpp up).
            uint32_t w = (cel.pre1 & 0x7FF) + 1;
            uint32_t woff = (cel.bpp >= 8 ? cel.pre1 >> 16 & 0x3FF : cel.pre1 >> 24) + 2;
            for (uint32_t i = 0; i < w; ++i) pixel(x0 + (int32_t)i, in.take((unsigned)cel.bpp));
            row += woff * 4;
        }
    }
    // The origin left for a cel that does not load one: below the last row (Opera).
    g_ce.x += g_ce.vdx * rows;
    g_ce.y += g_ce.vdy * rows;
}

} // namespace

// The cel engine from `ccb` on, into the bitmap the registers describe: the control word, REGCTL0
// (the buffers' widths: in each byte, as GRAPHIX's table of widths has them, the low nibble 1, 2, 4
// or 8 for 32, 512, 256 or 1024 pixels and the high nibble 1, 2 or 8 for 64, 128 or 1024, added),
// REGCTL1 (the last column and row drawn into, 11 bits each, the row's from bit 16), REGCTL2 (the
// buffer read) and REGCTL3 (the buffer written). Then CCB by CCB until one with LAST: SKIP passes
// one by; the flags say which words follow XPOS and YPOS (HDX to VDY, HDDX and HDDY, PIXC, then
// the preamble when CCBPRE), what is loaded, and whether the preamble opens the source data
// instead. LDPLUT loads 4 words of PLUT for 1 and 2 bits a pixel, 8 for 4, 16 for more.
void pf_cel_draw(ArmCpu& c, uint32_t cecontrol, const uint32_t regctl[4], uint32_t ccb) {
    static const uint32_t lo[16] = {0, 32, 512, 0, 256, 0, 0, 0, 1024}, hi[16] = {0, 64, 128, 0, 0, 0, 0, 0, 1024};
    uint32_t r0 = regctl[0];
    if ((r0 & 0xFF) != (r0 >> 8 & 0xFF) || (r0 >> 16)) pf_stop(c, "the cel engine: a read width other than the write width: not yet");
    Target t{cecontrol, regctl[3], regctl[2], lo[r0 & 15] + hi[r0 >> 4 & 15],
             (int32_t)(regctl[1] & 0x7FF), (int32_t)(regctl[1] >> 16 & 0x7FF)};
    if (!t.width) pf_stop(c, "the cel engine: a REGCTL0 not in GRAPHIX's table: not yet");
    for (int guard = 0; ccb; ++guard) {
        if (guard > 100000) pf_stop(c, "the cel engine: a CCB list that does not end");
        uint32_t f = pf_r32(ccb);
        if (!(f & CCB_SKIP)) {
            uint32_t p = ccb + 16;
            if (f & CCB_YOXY) { g_ce.x = (int32_t)pf_r32(p); g_ce.y = (int32_t)pf_r32(p + 4); }
            p += 8;
            if (f & CCB_LDSIZE) {
                g_ce.hdx = (int32_t)pf_r32(p); g_ce.hdy = (int32_t)pf_r32(p + 4);
                g_ce.vdx = (int32_t)pf_r32(p + 8); g_ce.vdy = (int32_t)pf_r32(p + 12);
                p += 16;
            }
            if (f & CCB_LDPRS) { g_ce.hddx = (int32_t)pf_r32(p); g_ce.hddy = (int32_t)pf_r32(p + 4); p += 8; }
            if (f & CCB_LDPPMP) { g_ce.pixc = pf_r32(p); p += 4; }
            Cel cel{f, 0, 0, pointer(ccb + 8, f & CCB_SPABS), 0, false};
            uint32_t pre = f & CCB_CCBPRE ? p : cel.src;
            cel.pre0 = pf_r32(pre);
            if (!(f & CCB_PACKED)) cel.pre1 = pf_r32(pre + 4);
            if (!(f & CCB_CCBPRE)) cel.src += f & CCB_PACKED ? 4 : 8;
            static const int depth[8] = {0, 1, 2, 4, 6, 8, 16, 0};
            cel.bpp = depth[cel.pre0 & 7];
            if (!cel.bpp) pf_stop(c, "the cel engine: a preamble's reserved depth");
            cel.coded = cel.bpp < 8 || !(cel.pre0 & PRE0_UNCODED);
            if (f & CCB_LDPLUT) {
                uint32_t plut = pointer(ccb + 12, f & CCB_PPABS), words = cel.bpp <= 2 ? 4 : cel.bpp == 4 ? 8 : 16;
                for (uint32_t i = 0; i < words; ++i) {
                    uint32_t w = pf_r32(plut + 4 * i);
                    g_ce.plut[2 * i] = (uint16_t)(w >> 16);
                    g_ce.plut[2 * i + 1] = (uint16_t)w;
                }
            }
            if (g_pf_trace)
                pf_log("        cel %06x: flags %08x at (%d, %d), %d bpp %s%s, pre0 %08x, pixc %08x\n", ccb, f,
                       g_ce.x >> 16, g_ce.y >> 16, cel.bpp, cel.coded ? "coded" : "uncoded",
                       f & CCB_PACKED ? " packed" : "", cel.pre0, g_ce.pixc);
            draw(c, cel, t);
        }
        if (f & CCB_LAST) break;
        ccb = pointer(ccb + 4, f & CCB_NPABS);
    }
}
