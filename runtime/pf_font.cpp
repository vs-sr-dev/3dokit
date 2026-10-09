// 3dokit runtime -- the Graphics folio's font: its 49 built-in characters, the calls that choose
// a font and its CCB, and DrawChar, DrawText8 and DrawText16.
//
// The font is the folio's own data: a Font, its CCB and PLUT, a missing character's word and the
// characters' images, in GRAPHIX's read-write data, which a program reaches through the pointers
// GetCurrentFont hands it. So the folio's image lies in the OS's memory, at PF_OS_IMAGES, as the
// loader leaves it -- unpacked by its own decompressor (pf_aif.cpp), its relocation list applied
// for that address, its zero-initialised data cleared -- and none of its code runs: what the code
// does is done here, on those bytes. The addresses in the comments are Doctor Hauzer's GRAPHIX
// 20.45 (build 72); each build's own addresses are in a table (kFonts), and a build without one
// has no font here (a program that calls for it stops). A version does not name a build: Escape
// from Monster Manor's GRAPHIX is 20.45 too, build 419, its font code the same and every address
// of it elsewhere.
//
// The characters are FontEntrys (graphics.h) in a binary search tree by ft_CharValue: a head node
// whose ft_GreaterBranch is the root, and every missing branch a "butt" node, both in the folio's
// data (gf_FontEntryHead, gf_FontEntryButt).
#include "pf.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

// FontEntry (graphics.h): a Node, then these; 0x2c bytes (GRAPHIX's allocation, 0x4048).
enum : uint32_t {
    FT_CHARVALUE = 0x14, FT_WIDTH = 0x18, FT_IMAGE = 0x1c, FT_IMAGEBYTECOUNT = 0x20,
    FT_LESSER = 0x24, FT_GREATER = 0x28, FT_SIZE = 0x2c,
    // Font: font_Height (u8), font_Flags (u8), font_CCB, font_FontEntries; 0xc bytes
    FONT_FLAGS = 1, FONT_CCB = 4, FONT_FONTENTRIES = 8, FONT_SIZE = 0xc,
    FONT_ASCII = 1, FONT_ASCII_UPPERCASE = 2, FONT_FILEBASED = 4, FONT_VERTICAL = 8,
    CCB_NEXTPTR = 4, CCB_SOURCEPTR = 8, CCB_PLUTPTR = 0xc, CCB_XPOS = 0x10, CCB_YPOS = 0x14,
    GC_PENX = 0x1c, GC_PENY = 0x20,
    GRAFERR_NO_FONT = 0xD55B9122u,
};

// One build's addresses: the folio's static data (offsets in its image) and the GrafFolio's
// font fields (graphics.h's, from gf_CurrentFontStream on; their offsets are the code's).
struct FontVersion {
    uint32_t version, build;        // the 3DO header's version, and the build from the image's build line
    uint32_t chars, nchars;         // the built-in characters: 12-byte records {value, width (u8), image}
    uint32_t image_bytes;           // each one's ft_ImageByteCount
    uint32_t font;                  // the built-in Font
    uint32_t static_font, static_ccb, static_plut;  // the current font's copies
    uint32_t head, butt;            // gf_FontEntryHead and gf_FontEntryButt's nodes
    uint32_t root;                  // the built-in font's tree, kept
    uint32_t missing;               // the word a missing character's CCB gets as its source
    uint32_t lru_name;              // "FontLeastRecentlyUsedList"
    uint32_t gf;                    // gf_CurrentFontStream
    uint32_t kernelbase, grafbase;  // the words the folio's code reads KernelBase and GrafBase from
};

const FontVersion kFonts[] = {
    // GRAPHIX 20.45 build 72 (Doctor Hauzer): the start 0x3f8c, the characters' records 0x407c
    {PF_VERSION(20, 45), 72, 0x7ef8, 49, 0x60, 0x6a9c, 0x69e4, 0x69a0, 0x8268, 0x69f0, 0x6a1c, 0x699c, 0x7e90,
     0x3850, 0xd0, 0x63c8, 0x6978},
    // GRAPHIX 20.45 build 419 (Escape from Monster Manor): the same code 0x2c4 on (the start
    // 0x4250, the records 0x4340), its data 0x544 on but for the PLUT, 0x878c
    {PF_VERSION(20, 45), 419, 0x843c, 49, 0x60, 0x6fe0, 0x6f28, 0x6ee4, 0x878c, 0x6f34, 0x6f60, 0x6ee0, 0x83d4,
     0x3b14, 0xd0, 0x68c4, 0x6a38},
};

// The build of a folio's image, placed in [from, to): the number after "<version>.<revision>." in
// its build line ("graphix 20.45.419 05/10/94 22:11:11 stan port1_3"); 0 when it has none.
uint32_t image_build(uint32_t from, uint32_t to, uint32_t version) {
    char key[16];
    std::snprintf(key, sizeof key, "%u.%u.", version >> 8, version & 0xff);
    size_t n = std::strlen(key);
    for (uint32_t a = from; a + n < to; ++a) {
        size_t i = 0;
        while (i < n && pf_r8(a + (uint32_t)i) == (uint8_t)key[i]) ++i;
        uint32_t d = a + (uint32_t)n, b = 0;
        if (i < n || !std::isdigit((int)pf_r8(d))) continue;
        while (std::isdigit((int)pf_r8(d))) b = b * 10 + (pf_r8(d++) - '0');
        return b;
    }
    return 0;
}

// The GrafFolio's fields, from gf_CurrentFontStream (graphics.h).
enum : uint32_t {
    GF_CURRENTFONTSTREAM = 0x00, GF_FILEFONTCACHESIZE = 0x04, GF_FILEFONTCACHEALLOC = 0x08,
    GF_FILEFONTCACHE = 0x0c, GF_FONTENTRYHEAD = 0x10, GF_FONTENTRYBUTT = 0x14, GF_FONTLRULIST = 0x18,
    GF_FILEFONTFLAGS = 0x38, GF_FONTBASECHAR = 0x3c, GF_FONTMAXCHAR = 0x40, GF_CURRENTFONT = 0x44,
    GF_CHARARRAYOFFSET = 0x48, GF_FILEFONTCACHEUSED = 0x4c,
};

const FontVersion* g_font;          // this disc's GRAPHIX, when its addresses are known
uint32_t g_img;                     // where its image lies

uint32_t gf(uint32_t field) { return pf_folio_base(PF_GRAPHICS) + g_font->gf + field; }
uint32_t at(uint32_t offset) { return g_img + offset; }

void copy(uint32_t to, uint32_t from, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) pf_w8(to + i, pf_r8(from + i));
}

// 0x3800: the butt's branches to itself, the head's ft_GreaterBranch to the butt (an empty tree)
// and its ft_CharValue -1, and InitList of gf_FontLRUList with the folio's name.
void empty_tree() {
    uint32_t butt = pf_r32(gf(GF_FONTENTRYBUTT));
    pf_w32(butt + FT_LESSER, butt);
    pf_w32(butt + FT_GREATER, butt);
    uint32_t head = pf_r32(gf(GF_FONTENTRYHEAD));
    pf_w32(head + FT_GREATER, pf_r32(gf(GF_FONTENTRYBUTT)));
    pf_w32(pf_r32(gf(GF_FONTENTRYHEAD)) + FT_CHARVALUE, 0xFFFFFFFFu);
    pf_list_init(gf(GF_FONTLRULIST), nullptr);
    pf_w32(gf(GF_FONTLRULIST) + 16, at(g_font->lru_name));
}

// 0x3c44: a FontEntry into the tree, down from the root by ft_CharValue (signed: at least a node's
// value goes greater), its own branches the butt.
void insert(uint32_t e) {
    int32_t v = (int32_t)pf_r32(e + FT_CHARVALUE);
    uint32_t parent = pf_r32(gf(GF_FONTENTRYHEAD)), n = pf_r32(parent + FT_GREATER);
    uint32_t butt = pf_r32(gf(GF_FONTENTRYBUTT));
    while (n != butt) {
        parent = n;
        n = pf_r32(n + (v >= (int32_t)pf_r32(n + FT_CHARVALUE) ? FT_GREATER : FT_LESSER));
    }
    pf_w32(e + FT_LESSER, butt);
    pf_w32(e + FT_GREATER, pf_r32(gf(GF_FONTENTRYBUTT)));
    pf_w32(parent + (v >= (int32_t)pf_r32(parent + FT_CHARVALUE) ? FT_GREATER : FT_LESSER), e);
}

// 0x3a18: a tree's missing branches (0) made the butt, from `n` down.
void relink(uint32_t n) {
    for (;;) {
        uint32_t butt = pf_r32(gf(GF_FONTENTRYBUTT));
        if (n == butt) return;
        uint32_t lesser = pf_r32(n + FT_LESSER);
        if (!lesser) pf_w32(n + FT_LESSER, butt);
        else relink(lesser);
        uint32_t greater = pf_r32(n + FT_GREATER);
        if (!greater) {
            pf_w32(n + FT_GREATER, pf_r32(gf(GF_FONTENTRYBUTT)));
            return;
        }
        n = greater;
    }
}

// 0x386c: a CCB as the current font's -- its 0x44 bytes into the folio's own CCB, which then
// points at itself and at the folio's PLUT; the 0x40 bytes of `plut` there when there is one.
void set_ccb(uint32_t ccb, uint32_t plut) {
    uint32_t s = at(g_font->static_ccb);
    copy(s, ccb, 0x44);
    pf_w32(s + CCB_PLUTPTR, at(g_font->static_plut));
    pf_w32(s + CCB_NEXTPTR, s);
    if (plut) copy(at(g_font->static_plut), plut, 0x40);
}

// SWI 14 (0x3920): gf_CurrentFontStream 0, the tree emptied, the built-in Font current, and
// gf_FileFontFlags' bit 0 (a file's font) cleared.
void reset_font() {
    pf_w32(gf(GF_CURRENTFONTSTREAM), 0);
    empty_tree();
    pf_w32(gf(GF_CURRENTFONT), at(g_font->font));
    pf_w32(gf(GF_FILEFONTFLAGS), pf_r32(gf(GF_FILEFONTFLAGS)) & ~1u);
}

// SWI 27 (0x3bd8): a font in memory made current -- the tree emptied, then the font's own tree
// hung from the head and relinked; gf_FileFontFlags' bit 0 cleared; the Font's 0xc bytes into
// the folio's own (0x38c4), with the folio's CCB, and the font's CCB and PLUT copied there; the
// folio's Font current (0x390c), FONT_FILEBASED cleared in it. 0.
uint32_t open_ram_font(uint32_t font) {
    empty_tree();
    pf_w32(pf_r32(gf(GF_FONTENTRYHEAD)) + FT_GREATER, pf_r32(font + FONT_FONTENTRIES));
    relink(pf_r32(font + FONT_FONTENTRIES));
    pf_w32(gf(GF_FILEFONTFLAGS), pf_r32(gf(GF_FILEFONTFLAGS)) & ~1u);
    uint32_t s = at(g_font->static_font);
    copy(s, font, FONT_SIZE);
    pf_w32(s + FONT_CCB, at(g_font->static_ccb));
    uint32_t ccb = pf_r32(font + FONT_CCB);
    set_ccb(ccb, pf_r32(ccb + CCB_PLUTPTR));
    pf_w32(gf(GF_CURRENTFONT), s);
    pf_w8(s + FONT_FLAGS, pf_r8(s + FONT_FLAGS) & ~FONT_FILEBASED);
    return 0;
}

void need_font(ArmCpu& c) {
    if (g_font) return;
    char why[96];
    uint32_t v = pf_system_version("/System/Folios/GRAPHIX");
    std::snprintf(why, sizeof why, "the font of GRAPHIX %u.%u: its addresses not read yet", v >> 8, v & 0xff);
    pf_stop(c, why);
}

// Graphics -44: Err ResetCurrentFont(void) -- user code (0x3970): SWI 14, then GRAFERR_NO_FONT if
// the built-in font has no tree, else SWI 27 on the built-in Font.
void g_resetcurrentfont(ArmCpu& c) {
    need_font(c);
    reset_font();
    c.r[0] = pf_r32(at(g_font->root)) ? open_ram_font(at(g_font->font)) : GRAFERR_NO_FONT;
}

// Graphics -144: Font* GetCurrentFont(void) -- SWI 37 (0x3a08): gf_CurrentFont.
void g_getcurrentfont(ArmCpu& c) {
    need_font(c);
    c.r[0] = pf_r32(gf(GF_CURRENTFONT));
}

// Graphics -140: Err SetCurrentFontCCB(CCB* ccb) -- SWI 36 (0x39e4): the CCB, with its own PLUT,
// as the current font's (0x386c). 0.
void g_setcurrentfontccb(ArmCpu& c) {
    need_font(c);
    set_ccb(c.r[0], pf_r32(c.r[0] + CCB_PLUTPTR));
    c.r[0] = 0;
}

// SWI 31 (0x3d84): Err DrawChar(GrafCon* gc, Item bitmap, u32 character). With FONT_ASCII and
// FONT_ASCII_UPPERCASE, a to z become A to Z. The character looked for in the tree (unsigned: a
// greater value goes greater) -- found, and a file's font, it goes to the end of gf_FontLRUList.
// The current font's CCB at the pen (16.16); its source the character's image, and the pen on by
// its width, or by font_Height downwards with FONT_VERTICAL; a missing character's source is the
// word at 0x7e90 and its width 8. Then DrawCels(bitmap, the CCB) -- SWI 39, its result back.
void draw_char(ArmCpu& c, uint32_t gc, uint32_t bitmap, uint32_t ch) {
    uint32_t font = pf_r32(gf(GF_CURRENTFONT)), ccb = pf_r32(font + FONT_CCB), flags = pf_r8(font + FONT_FLAGS);
    if ((flags & FONT_ASCII) && (flags & FONT_ASCII_UPPERCASE) && ch >= 0x61 && ch <= 0x7a) ch -= 0x20;
    uint32_t n = pf_r32(pf_r32(gf(GF_FONTENTRYHEAD)) + FT_GREATER), butt = pf_r32(gf(GF_FONTENTRYBUTT)), e = 0;
    while (n != butt) {
        uint32_t v = pf_r32(n + FT_CHARVALUE);
        if (ch == v) {
            if (pf_r32(gf(GF_FILEFONTFLAGS)) & 1) {
                pf_list_rem_node(n);
                pf_list_add_tail(gf(GF_FONTLRULIST), n);
            }
            e = n;
            break;
        }
        n = pf_r32(n + (ch >= v ? FT_GREATER : FT_LESSER));
    }
    pf_w32(ccb + CCB_XPOS, pf_r32(gc + GC_PENX) << 16);
    pf_w32(ccb + CCB_YPOS, pf_r32(gc + GC_PENY) << 16);
    bool vertical = pf_r8(pf_r32(gf(GF_CURRENTFONT)) + FONT_FLAGS) & FONT_VERTICAL;
    if (e) pf_w32(ccb + CCB_SOURCEPTR, pf_r32(e + FT_IMAGE));
    else {
        // the word itself, not its address: 0x800001c1 in 20.45's data, no address of the folio's
        char why[96];
        std::snprintf(why, sizeof why, "DrawChar: character 0x%x is not in the font (the folio's source would be 0x%08x)",
                      ch, pf_r32(at(g_font->missing)));
        pf_stop(c, why);
    }
    if (vertical) pf_w32(gc + GC_PENY, pf_r32(gc + GC_PENY) + pf_r8(pf_r32(gf(GF_CURRENTFONT))));
    else pf_w32(gc + GC_PENX, pf_r32(gc + GC_PENX) + (e ? pf_r32(e + FT_WIDTH) : 8));
    c.r[0] = bitmap;
    c.r[1] = ccb;
    pf_draw_cels(c);
}

// Graphics -128: DrawChar -- SWI 31.
void g_drawchar(ArmCpu& c) {
    need_font(c);
    draw_char(c, c.r[0], c.r[1], c.r[2]);
}

// Graphics -148: Err DrawText8(GrafCon* gc, Item bitmap, const u8* text) -- SWI 38 (0x3998), and
// -36 DrawText16(..., u16* text) -- SWI 29 (0x3f34): DrawChar for each character up to a 0, or
// up to the first that does not return 0; that result, else 0.
void draw_text(ArmCpu& c, bool wide) {
    need_font(c);
    uint32_t gc = c.r[0], bitmap = c.r[1], text = c.r[2];
    if (g_pf_trace) {
        std::string s;
        for (uint32_t p = text;; p += wide ? 2 : 1) {
            uint32_t ch = wide ? pf_r8(p) << 8 | pf_r8(p + 1) : pf_r8(p);
            if (!ch || s.size() > 200) break;
            if (ch >= 0x20 && ch < 0x7f) s += (char)ch;
            else {
                char hex[12];
                std::snprintf(hex, sizeof hex, "\\x%02x", ch);
                s += hex;
            }
        }
        pf_log("        text \"%s\" at %d,%d\n", s.c_str(), (int32_t)pf_r32(gc + GC_PENX), (int32_t)pf_r32(gc + GC_PENY));
    }
    uint32_t r = 0;
    for (;;) {
        uint32_t ch = wide ? pf_r8(text) << 8 | pf_r8(text + 1) : pf_r8(text);
        text += wide ? 2 : 1;
        if (!ch || r) break;
        draw_char(c, gc, bitmap, ch);
        r = c.r[0];
    }
    c.r[0] = r;
}

void g_drawtext8(ArmCpu& c) { draw_text(c, false); }
void g_drawtext16(ArmCpu& c) { draw_text(c, true); }

uint32_t be32(const std::vector<uint8_t>& d, size_t a) {
    return (uint32_t)d[a] << 24 | (uint32_t)d[a + 1] << 16 | (uint32_t)d[a + 2] << 8 | d[a + 3];
}

// The folio's image as the loader leaves it at `base` (aif.py's relocated): the bytes up to its
// relocation stub, its zero-initialised data cleared, each word its list names moved by `base`.
// The OS's memory past them is where its FontEntrys go. False when the image will not unpack.
bool place_image(const std::string& host, uint32_t base, uint32_t& end) {
    static std::string s_host;
    static std::vector<uint8_t> s_image;
    static std::vector<uint32_t> s_relocs;
    static uint32_t s_size;
    if (s_host != host) {
        s_host = host;
        s_image.clear();
        s_relocs.clear();
        std::ifstream f(host, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()), out;
        std::string why = pf_aif_unpack(d, out);
        if (!why.empty()) {
            std::fprintf(stderr, "%s: %s\n", host.c_str(), why.c_str());
            return false;
        }
        uint32_t w = be32(out, 4), stub = 8 + 4 + (uint32_t)((int32_t)(w << 8) >> 8) * 4;
        uint32_t ro = be32(out, 0x14), rw = be32(out, 0x18), bss = be32(out, 0x20);
        if (w >> 24 != 0xEB || stub > out.size()) {
            std::fprintf(stderr, "%s: no relocation stub\n", host.c_str());
            return false;
        }
        for (uint32_t o = stub + 184;; o += 4) {
            if (o + 4 > out.size()) {
                std::fprintf(stderr, "%s: the relocation list runs off the image\n", host.c_str());
                return false;
            }
            uint32_t r = be32(out, o);
            if (r == 0xFFFFFFFFu) break;
            s_relocs.push_back(r);
        }
        s_image.assign(out.begin(), out.begin() + stub);
        s_size = std::max(stub, ro + rw + bss);
    }
    if (s_image.empty()) return false;
    if (s_size + 49 * FT_SIZE > PF_OS_BASE + PF_OS_SIZE - base) {
        std::fprintf(stderr, "%s: no room for it in the OS's memory\n", host.c_str());
        return false;
    }
    for (uint32_t i = 0; i < s_size; ++i) pf_w8(base + i, i < s_image.size() ? s_image[i] : 0);
    for (uint32_t r : s_relocs) pf_w32(base + r, pf_r32(base + r) + base);
    end = base + ((s_size + 3) & ~3u);
    return true;
}

}  // namespace

// The folio's start (20.45: 0x3f8c, from 0x15d4): the GrafFolio's font fields -- the cache size
// 0x4000, the head and the butt -- the built-in Font current, the tree emptied (0x3800); then
// (0x407c) each built-in character a FontEntry (0x4014: 0x2c bytes from the kernel's allocator,
// ft_CharValue, ft_Width, ft_Image and ft_ImageByteCount 0x60) put in the tree (0x3c44), and the
// root kept, in the folio's word and as the built-in Font's font_FontEntries. The kernel's
// allocator here is the OS's memory just past the image, not pf_os_alloc's: no other allocation
// moves for them.
void pf_font_init() {
    pf_on_slot(PF_GRAPHICS, -44, g_resetcurrentfont);
    pf_on_slot(PF_GRAPHICS, -144, g_getcurrentfont);
    pf_on_slot(PF_GRAPHICS, -140, g_setcurrentfontccb);
    pf_on_slot(PF_GRAPHICS, -128, g_drawchar);
    pf_on_slot(PF_GRAPHICS, -148, g_drawtext8);
    pf_on_slot(PF_GRAPHICS, -36, g_drawtext16);
    g_font = nullptr;
    uint32_t v = pf_system_version("/System/Folios/GRAPHIX");
    if (std::none_of(std::begin(kFonts), std::end(kFonts), [v](const FontVersion& k) { return k.version == v; })) return;
    uint32_t end;
    g_img = PF_OS_IMAGES;
    if (!place_image(pf_host_path("/System/Folios/GRAPHIX"), g_img, end)) return;
    uint32_t build = image_build(g_img, end, v);
    const FontVersion* f = nullptr;
    for (const FontVersion& k : kFonts)
        if (k.version == v && k.build == build) f = &k;
    if (!f) return;
    g_font = f;
    pf_w32(at(f->kernelbase), pf_folio_base(PF_KERNEL));     // its start, 0x110
    pf_w32(at(f->grafbase), pf_folio_base(PF_GRAPHICS));
    pf_w32(gf(GF_CURRENTFONTSTREAM), 0);
    pf_w32(gf(GF_FILEFONTCACHESIZE), 0x4000);
    pf_w32(gf(GF_FILEFONTCACHEALLOC), 0);
    pf_w32(gf(GF_FILEFONTCACHEUSED), 0);
    pf_w32(gf(GF_FILEFONTCACHE), 0);
    pf_w32(gf(GF_FONTENTRYHEAD), at(f->head));
    pf_w32(gf(GF_FONTENTRYBUTT), at(f->butt));
    pf_w32(gf(GF_FILEFONTFLAGS), 0);
    pf_w32(gf(GF_CURRENTFONT), at(f->font));
    empty_tree();
    for (uint32_t i = 0; i < f->nchars; ++i, end += FT_SIZE) {
        uint32_t rec = at(f->chars + 12 * i);
        pf_w32(end + FT_IMAGEBYTECOUNT, f->image_bytes);
        pf_w32(end + FT_IMAGE, pf_r32(rec + 8));
        pf_w32(end + FT_WIDTH, pf_r8(rec + 4));
        pf_w32(end + FT_CHARVALUE, pf_r32(rec));
        insert(end);
    }
    uint32_t root = pf_r32(pf_r32(gf(GF_FONTENTRYHEAD)) + FT_GREATER);
    pf_w32(at(f->root), root);
    pf_w32(at(f->font) + FONT_FONTENTRIES, root);
}
