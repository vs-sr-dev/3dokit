// 3dokit runtime -- the cel engine: what the Graphics folio's DrawCels sets going. GRAPHIX writes
// the bitmap's control word and its four REGCTL words into MADAM, hands it the first CCB and
// waits for the hardware to finish; here the hardware is this file. What it does is what the
// 3DO Graphics Programmer's Guide says the cel engine does (chapter 3, "Understanding the Cel
// Engine and SPORT", and chapter 5, "Using the Cel Engine"; the SDK's hardware.h names the bits).
// Where the guide is silent or contradicts itself, the Opera emulator's MADAM was read (3dokit.cel
// did the same): the arithmetic of the pixel processor, the projector's V and H bits, the blue LSB
// and the end of a packed row, and the origin a cel leaves behind. Each such place says so.
//
// The projector -- which frame pixels a stretched, turned or bent source pixel paints -- is the
// one 3DO's patent WO 94/10644 describes (fill_quad, below). What no program run so far draws
// stops the run instead of guessing.
#include "pf.h"
#include <algorithm>
#include <cstdio>
#include <utility>

namespace {

enum : uint32_t {
    CCB_SKIP = 0x80000000u, CCB_LAST = 0x40000000u, CCB_NPABS = 0x20000000u, CCB_SPABS = 0x10000000u,
    CCB_PPABS = 0x08000000u, CCB_LDSIZE = 0x04000000u, CCB_LDPRS = 0x02000000u, CCB_LDPPMP = 0x01000000u,
    CCB_LDPLUT = 0x00800000u, CCB_CCBPRE = 0x00400000u, CCB_YOXY = 0x00200000u, CCB_ACW = 0x00040000u,
    CCB_ACCW = 0x00020000u, CCB_TWD = 0x00010000u, CCB_MARIA = 0x00001000u, CCB_PXOR = 0x00000800u, CCB_USEAV = 0x00000400u,
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
//
// With the CCB's USEAV the AV bits are controls instead (the guide's table 4, chapter 5): bits 4-3
// the secondary source's divider (1, 2, 4; 3, the decoder's low bits, stops), bit 2 the output
// wrap preventer off (the result kept to its 5 bits instead of held within 0 to 31), bit 1 the
// secondary source sign-extended from 5 bits, bit 0 subtract instead of add. With PXOR the final
// stage XORs the primary source (its 5 bits) with the secondary instead of adding. How the stages
// combine is Opera's (PPROC): the secondary shifted down by its divider, negated as its complement
// plus one, sign-extended after that; the sum halved by 2D. Opera adds in 8 bits, so a large
// primary wraps negative there; here the sum is held within 0 to 31 whatever its size.
uint32_t ppmp(ArmCpu& c, uint32_t mode, uint32_t pix, uint32_t fb, uint32_t amv, uint32_t flags) {
    uint32_t s1 = mode >> 15 & 1, ms = mode >> 13 & 3, mf = mode >> 10 & 7, df = mode >> 8 & 3,
             s2 = mode >> 6 & 3, av = mode >> 1 & 31, d2 = mode & 1;
    if (ms >= 2) pf_stop(c, "the cel engine: a multiplier from the pixel's colour (PIXC MS 10, 11): not yet");
    bool useav = flags & CCB_USEAV, pxor = flags & CCB_PXOR;
    uint32_t sdv = useav ? av >> 3 : 0;
    bool nowrapguard = useav && (av & 4), xtend = useav && (av & 2), neg = useav && (av & 1);
    if (useav && (sdv == 3 || s2 == 1))
        pf_stop(c, "the cel engine: USEAV's secondary divider from the decoder, or AV as both source and control: not yet");
    if (pxor && neg) pf_stop(c, "the cel engine: PXOR with USEAV's subtraction: not yet");
    static const int shift[4] = {4, 1, 2, 3};
    uint32_t in = s1 ? fb : pix, out = 0;
    for (int k = 0; k < 3; ++k) {
        int sh = 10 - 5 * k;                    // red, green, blue
        uint32_t m = ms ? (amv >> (6 - 3 * k) & 7) + 1 : mf + 1;
        int32_t a = (int32_t)(((in >> sh & 31) * m) >> shift[df]);
        int32_t b = s2 == 0 ? 0 : s2 == 1 ? (int32_t)av : (int32_t)((s2 == 2 ? fb : pix) >> sh & 31);
        b >>= sdv;
        int32_t bop = neg ? ~b : pxor ? b ^ (a & 31) : b;
        if (xtend) bop = ((bop & 31) ^ 16) - 16;
        int32_t r = ((pxor ? 0 : a) + bop + (neg ? 1 : 0)) >> d2;
        r = nowrapguard ? r & 31 : r < 0 ? 0 : r > 31 ? 31 : r;
        out |= (uint32_t)r << sh;
    }
    return out;
}

// The projector: one source pixel's quadrilateral, its four corners a0 (top left), a1 (top right),
// a2 (bottom right), a3 (bottom left) in 16.16, onto the frame buffer -- what the hardware's Regis
// unit does, as 3DO's patent WO 94/10644 ("Spryte rendering system with improved corner
// calculating engine and improved polygon-paint engine", sections 5.1, 5.2 and 6.6.2) describes
// it. The corners' fractions are cut away (to the integer below); the polygon of the four integer
// points is walked down its left and right edges a row at a time, each edge's x at row y taken
// from its upper end as a Bresenham walk from -dy takes it (the upper end's x plus dx * (y - y0) /
// dy, the quotient cut toward 0); each row y from the top point down to, but not including, the
// bottom point paints the pixels from the left edge's x up to, but not including, the right
// edge's. So a pixel whose top-left corner lies inside is painted, the bottom-most row and the
// right-most pixel of a row not, and a polygon that collapses paints nothing. The Munkee unit's
// short cuts are Regis's own results. Which way the polygon turns is read, row by row, from the
// edge on its left: going up there (a3 to a0 on an unturned cel) is clockwise, which ACW allows;
// going down, counter-clockwise, which ACCW allows -- so the two halves of a bow tie can differ.
// Opera's arbitrary-quad path walks the same edges the same way but paints the right-most pixel too.
template <class Paint>
void fill_quad(const int32_t qx[4], const int32_t qy[4], uint32_t flags, Paint paint) {
    int32_t x[4], y[4];
    for (int k = 0; k < 4; ++k) { x[k] = qx[k] >> 16; y[k] = qy[k] >> 16; }
    int32_t top = std::min(std::min(y[0], y[1]), std::min(y[2], y[3]));
    int32_t bot = std::max(std::max(y[0], y[1]), std::max(y[2], y[3]));
    for (int32_t r = top; r < bot; ++r) {
        int32_t xs[4];
        bool down[4];
        int n = 0;
        for (int k = 0; k < 4; ++k) {
            int u = k, v = (k + 1) & 3;                 // the edge from corner u to corner v
            bool goes_down = y[v] > y[u];
            int hi = goes_down ? u : v, lo = goes_down ? v : u;
            if (y[hi] == y[lo] || r < y[hi] || r >= y[lo]) continue;
            xs[n] = x[hi] + (int32_t)((int64_t)(x[lo] - x[hi]) * (r - y[hi]) / (y[lo] - y[hi]));
            down[n++] = goes_down;
        }
        for (int i = 1; i < n; ++i)                     // by x, an edge going up first at a tie
            for (int j = i; j > 0 && (xs[j] < xs[j - 1] || (xs[j] == xs[j - 1] && !down[j] && down[j - 1])); --j) {
                std::swap(xs[j], xs[j - 1]);
                std::swap(down[j], down[j - 1]);
            }
        for (int i = 0; i + 1 < n; i += 2)
            if ((flags & (down[i] ? CCB_ACCW : CCB_ACW)))
                for (int32_t px = xs[i]; px < xs[i + 1]; ++px) paint(px, r);
    }
}

// One cel: its source pixels through the decoder and the pixel processor into the frame buffer.
// The corner engine lays out the grid of the pixels' corners: row j's top edge starts at the
// origin plus j times (VDX, VDY) and steps by (HDX, HDY), its bottom edge -- the next row's top --
// by those plus HDDX and HDDY. HDX and the rest are 12.20 and step a 16.16 position with their
// four lowest bits cut away; HDDX adds to HDX at its full 20 bits (the patent's sum unit, 6.5).
void draw(ArmCpu& c, const Cel& cel, const Target& t) {
    // PRE0's BGND bit: the guide calls bits 28-31 reserved; Opera never reads it; the SDK's own
    // libraries (Lib3DO's CreateBackdropCel, TextLib) set and clear it together with the CCB's BGND,
    // "don't skip 0-valued pixels, really, trust me". With the CCB's BGND also set both readings draw
    // the same pixels; alone, they would differ.
    if (cel.pre0 & PRE0_LITERAL) pf_stop(c, "the cel engine: PRE0's LITERAL bit: not yet");
    if ((cel.pre0 & PRE0_BGND) && !(cel.flags & CCB_BGND))
        pf_stop(c, "the cel engine: PRE0's BGND bit without the CCB's: not yet");
    if (cel.pre0 >> 24 & 15) pf_stop(c, "the cel engine: SKIPX: not yet");
    bool lrform = !(cel.flags & CCB_PACKED) && (cel.pre1 & PRE1_LRFORM);
    if (lrform && cel.bpp != 16) pf_stop(c, "the cel engine: an LRFORM cel not of 16 bits: not yet");
    if (cel.flags & CCB_TWD) pf_stop(c, "the cel engine: TWD: not yet");
    if (cel.flags & CCB_MARIA) pf_stop(c, "the cel engine: MARIA (no region fill): not yet");
    uint32_t pover = cel.flags >> 7 & 3;
    if (pover == 1) pf_stop(c, "the cel engine: POVER 01: not yet");
    uint32_t cec = t.cecontrol;
    if ((cec >> 30) == 2) pf_stop(c, "the cel engine: B15POS 10: not yet");
    bool packed = cel.flags & CCB_PACKED;
    uint32_t lsb = packed ? cec >> 20 & 3 : cel.pre1 >> 12 & 3;

    // The V and H bits a pixel is written with (Opera): the decoder's (PLUTPOS) or the origin's
    // sub-pixel position (the guide: the first fraction bit of x is H, of y V); swapped by SWAPHV
    // unless PRE1's NOSWAP; then B15POS (0, 1, or as it is) and B0POS (0, 1, the pixel processor's
    // blue LSB, or as it is). The control word's CFBDSUB is not modelled (nor is it in Opera).
    uint32_t origin_vh = ((uint32_t)g_ce.y >> 15 & 1) << 15 | ((uint32_t)g_ce.x >> 15 & 1);
    auto vh_out = [&](uint32_t dec, uint32_t pp) {
        uint32_t vh = cel.flags & CCB_PLUTPOS ? dec & 0x8001u : origin_vh;
        if ((cec & SWAPHV) && !(!packed && (cel.pre1 & PRE1_NOSWAP))) vh = vh >> 15 | (vh & 1) << 15;
        switch (cec >> 30) { case 0: vh &= ~0x8000u; break; case 1: vh |= 0x8000u; break; }
        switch (cec >> 28 & 3) {
        case 0: vh &= ~1u; break;
        case 1: vh |= 1; break;
        case 2: vh = (vh & ~1u) | (pp & 1); break;
        }
        return (pp & 0x7FFEu) | vh;
    };

    int rows = ((int)(cel.pre0 >> 6 & 0x3FF) + 1) * (lrform ? 2 : 1);    // LRFORM's VCNT counts pairs
    // The current source pixel's top-left and bottom-left corners, and the row's steps.
    int32_t sx = g_ce.x, sy = g_ce.y, hx = g_ce.hdx, hy = g_ce.hdy;
    int32_t tx = 0, ty = 0, bx = 0, by = 0, hx2 = 0, hy2 = 0;
    auto skip = [&](uint32_t n) {
        tx += (int32_t)n * (hx >> 4); ty += (int32_t)n * (hy >> 4);
        bx += (int32_t)n * (hx2 >> 4); by += (int32_t)n * (hy2 >> 4);
    };
    auto pixel = [&](uint32_t v) {
        uint32_t amv, dec = decode(c, cel, v, amv);
        int32_t qx[4] = {tx, tx + (hx >> 4), bx + (hx2 >> 4), bx};
        int32_t qy[4] = {ty, ty + (hy >> 4), by + (hy2 >> 4), by};
        skip(1);
        if (!(dec & 0x7FFF) && !(cel.flags & CCB_BGND)) return;     // transparent, before the LSB
        uint32_t pix = blue_lsb(dec, lsb);
        uint32_t pm = pover == 2 ? 0 : pover == 3 ? 1 : pix >> 15;
        fill_quad(qx, qy, cel.flags, [&](int32_t x, int32_t y) {
            if (x < 0 || x > t.xclip || y < 0 || y > t.yclip) return;
            uint32_t fb = blue_lsb(fb_read(t.read, t.width, x, y), cec >> 22 & 3);
            uint32_t pp = ppmp(c, pm ? g_ce.pixc >> 16 : g_ce.pixc & 0xFFFF, pix, fb, amv, cel.flags);
            if (!(pp & 0x7FFF) && !(cel.flags & CCB_NOBLK)) pp = 1 << 10;   // black is written as red 1
            fb_write(t.write, t.width, x, y, vh_out(dec, pp));
        });
    };

    uint32_t row = cel.src;
    for (int j = 0; j < rows; ++j) {
        tx = sx; ty = sy;
        bx = sx + g_ce.vdx; by = sy + g_ce.vdy;
        hx2 = hx + g_ce.hddx; hy2 = hy + g_ce.hddy;
        Bits in{row};
        if (packed) {
            // A row opens with its length in words less 2 (8 bits below 8 bpp, 10 from 8 up, in a
            // byte or two of their own), then packets: 2 bits of type (0 end of row, 1 literal, 2
            // transparent, 3 repeat), 6 of count less 1, the pixels. The row also ends where its
            // words do (Opera; the guide calls the end-of-row packet optional).
            uint32_t len = (cel.bpp >= 8 ? in.take(16) & 0x3FF : in.take(8)) + 2, end = len * 32;
            while (in.pos + 2 <= end) {
                uint32_t type = in.take(2);
                if (!type) break;
                uint32_t n = in.take(6) + 1;
                if (type == 2) { skip(n); continue; }
                uint32_t v = type == 3 ? in.take((unsigned)cel.bpp) : 0;
                for (uint32_t i = 0; i < n; ++i) pixel(type == 1 ? in.take((unsigned)cel.bpp) : v);
            }
            row += len * 4;
        } else {
            // TLHPCNT + 1 pixels a row, the rows WOFFSET + 2 words apart (WOFFSET(10) from 8 bpp up).
            // LRFORM takes a bitmap's line pairs as they lie: the words hold two rows, the even
            // row's pixel in the high half, the odd's in the low, and WOFFSET goes from pair to pair
            // (the guide's "The LRFORM Bit"; Opera's DrawLRCel reads them so).
            uint32_t w = (cel.pre1 & 0x7FF) + 1;
            uint32_t woff = (cel.bpp >= 8 ? cel.pre1 >> 16 & 0x3FF : cel.pre1 >> 24) + 2;
            if (lrform) {
                for (uint32_t i = 0; i < w; ++i) {
                    uint32_t v = pf_r32(row + i * 4);
                    pixel(j & 1 ? v & 0xFFFF : v >> 16);
                }
                if (j & 1) row += woff * 4;
            } else {
                for (uint32_t i = 0; i < w; ++i) pixel(in.take((unsigned)cel.bpp));
                row += woff * 4;
            }
        }
        sx += g_ce.vdx; sy += g_ce.vdy;
        hx = hx2; hy = hy2;
    }
    // What the cel leaves in the engine for one that does not load it: the origin below the last
    // row (Opera), and HDX and HDY as far as HDDX and HDDY took them (the patent's running sum;
    // Opera's arbitrary path keeps them so too).
    g_ce.x = sx;
    g_ce.y = sy;
    g_ce.hdx = hx;
    g_ce.hdy = hy;
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
            if (g_pf_trace) {
                pf_log("        cel %06x: flags %08x at (%d, %d), %d bpp %s%s, pre0 %08x, pixc %08x\n", ccb, f,
                       g_ce.x >> 16, g_ce.y >> 16, cel.bpp, cel.coded ? "coded" : "uncoded",
                       f & CCB_PACKED ? " packed" : "", cel.pre0, g_ce.pixc);
                if (g_ce.hdx != 1 << 20 || g_ce.hdy || g_ce.vdx || g_ce.vdy != 1 << 16 || g_ce.hddx || g_ce.hddy ||
                    (g_ce.x & 0xFFFF) || (g_ce.y & 0xFFFF))
                    pf_log("          xy %08x %08x, hdx %08x hdy %08x vdx %08x vdy %08x hddx %08x hddy %08x, pre1 %08x\n",
                           g_ce.x, g_ce.y, g_ce.hdx, g_ce.hdy, g_ce.vdx, g_ce.vdy, g_ce.hddx, g_ce.hddy, cel.pre1);
            }
            draw(c, cel, t);
        }
        if (f & CCB_LAST) break;
        ccb = pointer(ccb + 4, f & CCB_NPABS);
    }
}
