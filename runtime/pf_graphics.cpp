// 3dokit runtime -- the Graphics folio: its node's fields and its SWIs and vector slots, as far
// as the programs run so far reach them. What a field holds is what the 1993 folio
// (System/Folios/GRAPHIX, decompressed by its own code in 3dokit.armemu) puts there when it
// starts, and what a function does is what its code there does (the addresses are GRAPHIX's).
#include "pf.h"
#include <cstdio>
#include <cstdlib>
#include <vector>

// The folio's structures (graphics.h; the 1.2 and 1.3 headers agree, and the 1993 folio's stores
// put each field used here where they do). The node sizes are the folio's own node database
// (GRAPHIX 0x5430): its ScreenGroup ends at sg_Add_SG_Called and its VDL at vdl_DataSize, before
// the fields the headers add after 1993.
enum : uint32_t {
    NST_GRAPHICS = 2,
    TYPE_SCREENGROUP = 1, TYPE_SCREEN = 2, TYPE_BITMAP = 3, TYPE_VDL = 4,
    SG_SCREENHEIGHT = 0x28, SG_DISPLAYHEIGHT = 0x2c, SG_ADD_SG_CALLED = 0x50,
    SCR_SCREENGROUPPTR = 0x24, SCR_VDLPTR = 0x28, SCR_VDLITEM = 0x2c, SCR_VDLTYPE = 0x30,
    SCR_BITMAPLIST = 0x38, SCR_TEMPBITMAP = 0x78,
    BM_BUFFER = 0x24, BM_WIDTH = 0x28, BM_HEIGHT = 0x2c, BM_VERTICALOFFSET = 0x30, BM_FLAGS = 0x34,
    BM_CLIPWIDTH = 0x38, BM_CLIPHEIGHT = 0x3c, BM_WATCHDOGCTR = 0x48, BM_SYSMALLOC = 0x4c,
    BM_CECONTROL = 0x70, BM_REGCTL0 = 0x74, BM_REGCTL1 = 0x78, BM_REGCTL2 = 0x7c, BM_REGCTL3 = 0x80,
    VDL_SCREENPTR = 0x24, VDL_DATAPTR = 0x28, VDL_TYPE = 0x2c, VDL_DATASIZE = 0x30,
};
static const uint32_t kNodeSize[] = {0, 0x54, 0x7c, 0x84, 0x34};

// The folio's errors (graphics.h's GRAFERR_*, the values GRAPHIX builds).
enum : uint32_t {
    GRAFERR_BADITEM = 0xD55B9001u, GRAFERR_BADTAG = 0xD55B9002u, GRAFERR_BADTAGVAL = 0xD55B9003u,
    GRAFERR_NOMEM = 0xD55B9006u, GRAFERR_BADVDLTYPE = 0xD55B9116u, GRAFERR_INDEXRANGE = 0xD55B9117u,
    GRAFERR_BUFWIDTH = 0xD55B9118u, GRAFERR_VDLWIDTH = 0xD55B911Au, GRAFERR_NOTYET = 0xD55B911Bu,
    GRAFERR_MIXEDSCREENS = 0xD55B911Cu, GRAFERR_VDL_LENGTH = 0xD55B9121u,
    GRAFERR_BADDISPDIMS = 0xD55B9123u, GRAFERR_BADBITMAPSPEC = 0xD55B9124u,
    GRAFERR_INTERNALERROR = 0xD55B9125u, GRAFERR_SGINUSE = 0xD55B9126u, GRAFERR_SGNOTINUSE = 0xD55B9127u,
    GRAFERR_NOWRITEACCESS = 0xD55B9129u, GRAFERR_NOTOWNER = 0xD55B9012u,
};

static uint32_t graf(uint32_t field) { return pf_r32(pf_folio_base(PF_GRAPHICS) + field); }
static uint32_t task_item() { return pf_r32(pf_current_task() + 24); }

// A VDL entry's header ends with 32 colour words: entry i is i << 24 and the grey i * 255 / 31 in
// each channel (GRAPHIX's loop, a signed division). From `from` on; the address after them.
static uint32_t grey_ramp(uint32_t a, int from) {
    for (int i = from; i < 32; ++i, a += 4) {
        uint32_t v = (uint32_t)(i * 255 / 31);
        pf_w32(a, (uint32_t)i << 24 | v << 16 | v << 8 | v);
    }
    return a;
}

static void graphics_slots();
static void system_vdls();

// GRAPHIX's FIRQ at the vertical blank (0x50b4, "Graphics FIRQ", interrupt 1, priority 250):
// gf_VBLNumber up by 1, with bit 0 then set in an odd field (the field bit of CLIO's 0x3400034),
// which keeps it even in even fields as the fields alternate; then the field's VDL
// (gf_CurrentVDLEven or, in an odd field, gf_CurrentVDLOdd) into the display link -- the word
// gf_VDLDisplayLink points at.
static void write_frame(uint32_t vbl);

PfTimeFn g_pf_display_vbl;

// The fields counted so far, kept across a program's end: the folio, which on the console stays
// loaded from one program to the next, starts the next one's count from it.
static uint32_t g_vbl_number;

static void graphics_vbl(uint64_t when) {
    uint32_t g = pf_folio_base(PF_GRAPHICS), n = pf_r32(g + GF_VBLNUMBER) + 1;
    pf_w32(g + GF_VBLNUMBER, n);
    g_vbl_number = n;
    pf_w32(pf_r32(g + GF_VDLDISPLAYLINK), pf_r32(g + (n & 1 ? GF_CURRENTVDLODD : GF_CURRENTVDLEVEN)));
    if (g_pf_frames_dir && n >= g_pf_frames_first && n <= g_pf_frames_last && (n - g_pf_frames_first) % g_pf_frames_every == 0)
        write_frame(n);
    if (g_pf_display_vbl) g_pf_display_vbl(when);
}

// ---- what the display shows (pfboot --frames) --------------------------------------------------
// One field as the VDLs describe it (hardware.h's VDL words), from forced-first round to it
// again: each entry's DMA control word -- its lines (bits 0-8), the words after its 4-word header
// (9-14), VDL_LDPREV, VDL_LDCUR (the buffers from its second and third words), VDL_ENVIDDMA --
// then its link, then its words: a colour (bit 31 clear: pen 24-28, all three channels or the
// one bits 29-30 name) into the CLUT, which stays from entry to entry. A line with video DMA reads
// 320 pixels from the buffer in the 3DO's line pairs (two lines in a word, the even one in the
// high half; the pair after the next 1,280 bytes on) and gives each 5-bit channel its CLUT entry's
// colour. The picture starts at the pre-display entry: the lines before it are the system's in the
// vertical blank -- among them the VIRS line (the full entry over the VIRS page: the TV's
// vertical-interval reference, black, pen 1's yellow, pen 2's grey), which no TV shows -- and
// lines without video DMA are left out, so a screen of 240 lines gives 240. Not modelled: the
// display control words (interpolation, the background and transparency, bit 15 of a pixel),
// other widths, a relative link.
const char* g_pf_frames_dir;
uint32_t g_pf_frames_first = 0, g_pf_frames_last = 0xFFFFFFFFu, g_pf_frames_every = 1;
static std::vector<uint8_t> g_last_frame;

static bool display_field(std::vector<uint8_t>& rgb, int& lines) {
    uint32_t first = graf(GF_VDLFORCEDFIRST), pre = graf(GF_VDLPREDISPLAY), e = first;
    uint8_t clut[32][3] = {};
    uint32_t cur = 0;
    bool odd = false, picture = false;
    rgb.clear();
    lines = 0;
    for (int entries = 0; entries < 256; ++entries) {
        uint32_t w0 = pf_r32(e), n = w0 & 0x1ff, len = (w0 >> 9) & 0x3f;
        if (w0 & 0x00040000u) return false;                 // VDL_RELSEL
        if (e == pre) picture = true;
        if (w0 & 0x00010000u) { cur = pf_r32(e + 4); odd = false; }
        for (uint32_t i = 0; i < len; ++i) {
            uint32_t v = pf_r32(e + 16 + 4 * i);
            if (v & 0x80000000u) continue;
            uint32_t pen = (v >> 24) & 31, sel = (v >> 29) & 3;
            uint8_t ch[3] = {(uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
            for (int k = 0; k < 3; ++k)
                if (sel == 0 || sel == 3 - (uint32_t)k) clut[pen][k] = ch[k];
        }
        for (uint32_t l = 0; l < n && (w0 & 0x00200000u) && picture; ++l) {
            if (++lines > 512) return false;
            for (uint32_t x = 0; x < 320; ++x) {
                uint32_t w = pf_r32(cur + 4 * x), p = odd ? w & 0xffff : w >> 16;
                rgb.push_back(clut[(p >> 10) & 31][0]);
                rgb.push_back(clut[(p >> 5) & 31][1]);
                rgb.push_back(clut[p & 31][2]);
            }
            if (odd) cur += 1280;
            odd = !odd;
        }
        e = pf_r32(e + 12);
        if (e == first) return true;
    }
    return false;
}

bool pf_display_field(std::vector<uint8_t>& rgb, int& lines) { return display_field(rgb, lines); }

static void write_frame(uint32_t vbl) {
    std::vector<uint8_t> rgb;
    int lines;
    if (!display_field(rgb, lines) || rgb == g_last_frame) return;
    g_last_frame = rgb;
    char path[1024];
    std::snprintf(path, sizeof path, "%s/vbl%06u.ppm", g_pf_frames_dir, vbl);
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n320 %d\n255\n", lines);
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
    }
}

void pf_graphics_init() {
    graphics_slots();
    pf_on_vbl(graphics_vbl);
    uint32_t g = pf_folio_base(PF_GRAPHICS), kl = pf_kernel_lists();
    uint32_t page = pf_page_size(MEMTYPE_VRAM);             // the VRAM page, 2 KB
    pf_w32(g + GF_VBLNUMBER, g_vbl_number);
    pf_w32(g + GF_VRAMPAGESIZE, page);
    pf_w32(g + GF_DEFAULTDISPLAYWIDTH, 320);
    pf_w32(g + GF_DEFAULTDISPLAYHEIGHT, 240);
    pf_w32(g + GF_VBLTIME, 16684);                          // microseconds
    pf_w32(g + GF_VBLFREQ, 60);
    // Two VRAM pages of the OS's: the VIRS page, cleared, then two runs of words from
    // +0x44 (148 of 0x04210421, 73 of 0x08420842), and the zero page after it, which lib3DO
    // reads to learn which VRAM bank to put screens in (GetBankBits).
    uint32_t virs = pf_alloc_mem(kl, (int32_t)(2 * page), MEMTYPE_VRAM | MEMTYPE_STARTPAGE, false);
    if (!virs) {
        std::fprintf(stderr, "the Graphics folio: no VRAM for its VIRS page\n");
        std::exit(3);
    }
    for (uint32_t i = 0; i < 2 * page; i += 4) pf_w32(virs + i, 0);
    uint32_t a = virs + 0x44;
    for (int i = 0; i < 148; ++i, a += 4) pf_w32(a, 0x04210421u);
    for (int i = 0; i < 73; ++i, a += 4) pf_w32(a, 0x08420842u);
    pf_w32(g + GF_VIRSPAGE, virs);
    pf_w32(g + GF_ZEROPAGE, virs + page);
    // --snap 0 is this step: GRAPHIX's 0x41b4, replayed by python -m 3dokit.pfcheck
    if (g_pf_snap_dir && !g_pf_snap_call) {
        ArmCpu c{};
        pf_snap_before(c, "init Graphics vdls");
        system_vdls();
        pf_snap_after(c);
    }
    system_vdls();
}

// The system's VDLs (0x41b4), in two blocks of the OS's VRAM. The display's chain is
// forced-first -> a full entry over the VIRS page -> pre-display -> (the screens' VDLs, or the
// blank one) -> post-display -> forced-first again; gf_VDLDisplayLink is the word of pre-display
// that DisplayScreen points elsewhere. The folio then gives forced-first to the display
// hardware, which nothing here models.
static void system_vdls() {
    uint32_t g = pf_folio_base(PF_GRAPHICS), kl = pf_kernel_lists();
    uint32_t virs = graf(GF_VIRSPAGE), zero = graf(GF_ZEROPAGE), a;
    uint32_t blk = pf_alloc_mem(kl, 0x180, MEMTYPE_VRAM | MEMTYPE_DMA, false);
    uint32_t blank = pf_alloc_mem(kl, 0xa0, MEMTYPE_VRAM | MEMTYPE_DMA, false);
    if (!blk || !blank) {
        std::fprintf(stderr, "the Graphics folio: no VRAM for its VDLs\n");
        std::exit(3);
    }
    uint32_t forced = blk, pre = blk + 0x20, post = blk + 0xc0, full = blk + 0xe0;
    pf_w32(g + GF_VDLFORCEDFIRST, forced);
    pf_w32(g + GF_VDLPREDISPLAY, pre);
    pf_w32(g + GF_VDLPOSTDISPLAY, post);
    pf_w32(g + GF_CURRENTVDLODD, blank);
    pf_w32(g + GF_CURRENTVDLEVEN, blank);
    pf_w32(g + GF_VDLBLANK, blank);
    const uint32_t forced_words[8] = {0x0001840du, zero, zero, full, 0xE1000000u, 0xE1000000u, 0xE1000000u, 0xE1000000u};
    for (int i = 0; i < 8; ++i) pf_w32(forced + 4 * i, forced_words[i]);
    pf_w32(pre, 0x00004402u);
    pf_w32(pre + 4, zero);
    pf_w32(pre + 8, zero);
    pf_w32(g + GF_VDLDISPLAYLINK, pre + 12);
    pf_w32(pre + 12, blank);
    a = grey_ramp(pre + 16, 0);
    pf_w32(a, 0xE0000000u);
    pf_w32(a + 4, 0xC001002Du);
    const uint32_t post_words[8] = {0x00000400u, zero, zero, forced, 0xC01F83F4u, 0xE1000000u, 0xE1000000u, 0xE1000000u};
    for (int i = 0; i < 8; ++i) pf_w32(post + 4 * i, post_words[i]);
    const uint32_t full_words[7] = {0x0021C401u, virs, virs, pre, 0, 0x01A0B539u, 0x026D6D6Du};
    for (int i = 0; i < 7; ++i) pf_w32(full + 4 * i, full_words[i]);
    a = grey_ramp(full + 28, 3);
    pf_w32(a, 0xE0000000u);
    pf_w32(a + 4, 0xC001002Du);
    // the blank VDL: the start of VRAM (the first MemHdr of VRAM's base), all colours black
    uint32_t vram = pf_r32(pf_find_mh(virs) + 0x2c);        // memh_MemBase
    pf_w32(blank, 0x0023C4F0u);
    pf_w32(blank + 4, vram);
    pf_w32(blank + 8, vram);
    pf_w32(blank + 12, post);
    for (int i = 0; i < 32; ++i) pf_w32(blank + 16 + 4 * i, (uint32_t)i << 24);
    pf_w32(blank + 144, 0xE0000000u);
    pf_w32(blank + 148, 0xC001002Cu);
}

// ---- items -----------------------------------------------------------------------------------
// A node of the folio's, made for the current task (CreateSizedItem in the folio's SWIs): the
// kernel's own memory, cleared, n_Size from the node database, the task its owner.
static int32_t graf_item(uint32_t type) {
    uint32_t n = pf_os_alloc(kNodeSize[type]);
    pf_w32(n + 12, kNodeSize[type]);
    int32_t item = pf_item_new(n, NST_GRAPHICS, (int)type, nullptr);
    pf_w32(n + 28, task_item());                            // n_Owner
    return item;
}

// What the folio's calls on an item check first: CheckItem, then that the caller owns it (the
// folio also lets a task that opened the item through; nothing here shares items yet).
static uint32_t own_item(ArmCpu& c, uint32_t item, uint32_t type) {
    uint32_t n = pf_check_item((int32_t)item, NST_GRAPHICS, (int)type);
    if (n && pf_r32(n + 28) != task_item()) pf_stop(c, "the Graphics folio: an item of another task");
    return n;
}

// ---- screen groups ---------------------------------------------------------------------------
// CreateScreenGroup's arguments, in the order of their tags (CSG_TAG_DISPLAYHEIGHT 1 to
// CSG_TAG_SPORTBITS 11): the block the user half builds on the caller's stack for SWI 50.
enum { CSG_DISPLAYHEIGHT, CSG_SCREENCOUNT, CSG_SCREENHEIGHT, CSG_BITMAPCOUNT, CSG_WIDTHS,
       CSG_HEIGHTS, CSG_BUFFERS, CSG_VDLTYPE, CSG_VDLPTRS, CSG_VDLLENGTHS, CSG_SPORTBITS, CSG_N };

// A VDL entry's display control word for a bitmap `width` pixels wide, or 0 for a width the
// VDL cannot show.
static uint32_t vdl_width_bits(uint32_t width) {
    switch (width) {
    case 320: return 0xC001002Cu;
    case 384: return 0xC001002Cu | 0x800000u;
    case 512: return 0xC001002Cu | 0x1000000u;
    case 640: return 0xC001002Cu | 0x1800000u;
    case 1024: return 0xC001002Cu | 0x2000000u;
    default: return 0;
    }
}

// A bitmap's REGCTL0 (the cel engine's buffer width) for `width`, or 0 for another width.
static uint32_t regctl0(uint32_t width) {
    static const uint32_t widths[][2] = {
        {32, 0x0101}, {64, 0x1010}, {96, 0x1111}, {128, 0x2020}, {160, 0x2121}, {256, 0x0404},
        {320, 0x1414}, {384, 0x2424}, {512, 0x0202}, {576, 0x1212}, {640, 0x2222}, {1024, 0x0808},
        {1056, 0x8181}, {1088, 0x1818}, {1152, 0x2828}, {1280, 0x8484}, {1536, 0x8282}, {2048, 0x8888},
    };
    for (const auto& w : widths)
        if (w[0] == width) return w[1];
    return 0;
}

// SWI 50, the supervisor half (0x27a0): the group, then per screen its Screen, its VDL and its
// Bitmap. Whatever the bitmap count, it makes one bitmap a screen (it sets the count to 1).
// Only the VDL type the programs run so far use (VDLTYPE_SIMPLE, the default) is done; the
// caller's own VDLs and VDLTYPE_FULL are read but not yet written. On an error the folio deletes
// the group, which nothing here does yet: its items stay.
static uint32_t csg_supervisor(ArmCpu& c, uint32_t items, uint32_t* arg, bool sys_malloc) {
    int32_t group = graf_item(TYPE_SCREENGROUP);
    uint32_t sg = pf_item_node(group);
    pf_w32(sg + SG_DISPLAYHEIGHT, arg[CSG_DISPLAYHEIGHT]);
    pf_w32(sg + SG_SCREENHEIGHT, arg[CSG_SCREENHEIGHT]);
    pf_w32(sg + SG_ADD_SG_CALLED, 0);
    arg[CSG_BITMAPCOUNT] = 1;
    uint32_t buffers = arg[CSG_BUFFERS];
    for (int32_t s = 0; s < (int32_t)arg[CSG_SCREENCOUNT]; ++s) {
        int32_t screen = graf_item(TYPE_SCREEN);
        pf_w32(items + 4 * (uint32_t)s, (uint32_t)screen);
        uint32_t scr = pf_item_node(screen);
        pf_w32(scr + SCR_SCREENGROUPPTR, sg);
        // the VDL: one entry a bitmap, chained by their fourth words, the last to post-display
        if (arg[CSG_VDLPTRS]) pf_stop(c, "CreateScreenGroup: the caller's VDLs (CSG_TAG_VDLPTR_ARRAY): not yet");
        uint32_t type = arg[CSG_VDLTYPE];
        if (type == 1) pf_stop(c, "CreateScreenGroup: VDLTYPE_FULL: not yet");
        if (type - 1 > 4) return GRAFERR_BADTAGVAL;
        if (type != 4) return GRAFERR_NOTYET;
        const uint32_t words = 38;
        uint32_t head = 0, link = 0, widths = arg[CSG_WIDTHS], bp = buffers;
        for (int32_t b = 0; b < (int32_t)arg[CSG_BITMAPCOUNT]; ++b) {
            uint32_t width = widths ? pf_r32(widths) : graf(GF_DEFAULTDISPLAYWIDTH);
            if (widths) widths += 4;
            uint32_t e = pf_alloc_mem(pf_kernel_lists(), (int32_t)(4 * words), MEMTYPE_VRAM | MEMTYPE_DMA, false);
            if (!e) return GRAFERR_NOMEM;
            if (b) pf_w32(link, e);
            else head = e;
            pf_w32(e, 0x002180F0u | (words - 4) << 9);      // 240 lines, the header's length
            pf_w32(e + 4, pf_r32(bp));                      // this frame's buffer and the last's
            pf_w32(e + 8, pf_r32(bp));
            bp += 4;
            link = e + 12;
            uint32_t ctl = vdl_width_bits(width);
            if (!ctl) return GRAFERR_VDLWIDTH;
            pf_w32(e + 16, ctl);
            pf_w32(grey_ramp(e + 20, 0), 0xE0000000u);
        }
        if (link) pf_w32(link, graf(GF_VDLPOSTDISPLAY));
        int32_t vi = graf_item(TYPE_VDL);
        uint32_t vdl = pf_item_node(vi);
        pf_w32(vdl + VDL_SCREENPTR, scr);
        pf_w32(scr + SCR_VDLPTR, vdl);
        pf_w32(scr + SCR_VDLITEM, (uint32_t)vi);
        pf_w32(vdl + VDL_TYPE, type);
        pf_w32(vdl + VDL_DATAPTR, head);
        pf_w32(vdl + VDL_DATASIZE, 4 * words);
        pf_w32(scr + SCR_VDLTYPE, type);
        pf_list_init(scr + SCR_BITMAPLIST, "ScreenBitmapList");
        // the bitmaps, one under the other; the folio keeps the last in scr_TempBitmap and puts
        // none on scr_BitmapList
        uint32_t heights = arg[CSG_HEIGHTS], offset = 0;
        widths = arg[CSG_WIDTHS];
        for (int32_t b = 0; b < (int32_t)arg[CSG_BITMAPCOUNT]; ++b) {
            int32_t bi = graf_item(TYPE_BITMAP);
            uint32_t bm = pf_item_node(bi);
            uint32_t width = widths ? pf_r32(widths) : graf(GF_DEFAULTDISPLAYWIDTH);
            if (widths) widths += 4;
            uint32_t height = heights ? pf_r32(heights) : arg[CSG_SCREENHEIGHT];
            if (heights) heights += 4;
            if (!buffers) return GRAFERR_INTERNALERROR;
            pf_w32(bm + BM_SYSMALLOC, sys_malloc ? 1 : 0);   // 23.10's 0x3684: the user half's buffers
            uint32_t buf = pf_r32(buffers);
            buffers += 4;
            pf_w32(bm + BM_BUFFER, buf);
            pf_w32(scr + SCR_TEMPBITMAP, bm);
            pf_w32(bm + BM_WIDTH, width);
            pf_w32(bm + BM_HEIGHT, height);
            pf_w32(bm + BM_CLIPWIDTH, width);
            pf_w32(bm + BM_CLIPHEIGHT, height);
            pf_w32(bm + BM_FLAGS, 0);
            pf_w32(bm + BM_WATCHDOGCTR, 0xF424);           // 62,500
            if (pf_task_can_write(pf_current_task(), buf, (int32_t)(height * width * 2)) < 0)
                return GRAFERR_NOWRITEACCESS;
            uint32_t r0 = regctl0(width);
            if (!r0) return GRAFERR_BUFWIDTH;
            pf_w32(bm + BM_REGCTL0, r0);
            pf_w32(bm + BM_REGCTL1, (pf_r32(bm + BM_CLIPWIDTH) - 1) | (pf_r32(bm + BM_CLIPHEIGHT) - 1) << 16);
            pf_w32(bm + BM_REGCTL2, buf);
            pf_w32(bm + BM_REGCTL3, buf);
            pf_w32(bm + BM_CECONTROL, 0xE1500000u);
            pf_w32(bm + BM_VERTICALOFFSET, offset);
            offset += height;
        }
    }
    return (uint32_t)group;
}

// Graphics -48: Item CreateScreenGroup(Item* screens, TagArg* tags) -- the screens' items into
// `screens`, the group's back. The user half (0x3e44) runs in the caller's mode: the tags over
// the folio's defaults, checked, and unless the caller gives the buffers, the array of their
// pointers and the buffers themselves from the caller's own memory lists (VRAM for the cel
// engine; with SPORT bits whole VRAM pages, the first one starting a page in that bank). Then
// SWI 50. (It first asks IsItemOpened of the folio: a program that has the folio's vectors has
// opened it here.)
static void g_createscreengroup(ArmCpu& c) {
    uint32_t arg[CSG_N] = {};
    arg[CSG_DISPLAYHEIGHT] = arg[CSG_SCREENHEIGHT] = graf(GF_DEFAULTDISPLAYHEIGHT);
    arg[CSG_SCREENCOUNT] = 2;
    arg[CSG_BITMAPCOUNT] = 1;
    arg[CSG_VDLTYPE] = 4;
    for (uint32_t t = c.r[1]; t; t += 8) {
        uint32_t tag = pf_r32(t);
        if (!tag) break;
        if (tag - 1 >= CSG_N) {
            c.r[0] = GRAFERR_BADTAG;
            return;
        }
        arg[tag - 1] = pf_r32(t + 4);
    }
    int32_t dh = (int32_t)arg[CSG_DISPLAYHEIGHT], bitmaps = (int32_t)arg[CSG_BITMAPCOUNT];
    int32_t screens = (int32_t)arg[CSG_SCREENCOUNT];
    uint32_t err = 0;
    if (dh < 1 || dh > (int32_t)graf(GF_DEFAULTDISPLAYHEIGHT)) err = GRAFERR_BADDISPDIMS;
    else if (arg[CSG_VDLPTRS] && !arg[CSG_VDLLENGTHS]) err = GRAFERR_VDL_LENGTH;
    else if (screens < 1 || (int32_t)arg[CSG_SCREENHEIGHT] < dh) err = GRAFERR_BADDISPDIMS;
    else if (bitmaps < 0 || (bitmaps > 1 && !arg[CSG_HEIGHTS])) err = GRAFERR_BADBITMAPSPEC;
    if (err) {
        c.r[0] = err;
        return;
    }
    // No buffers given: the user half allocates them, and a table of them. 23.10's (graphix 0x4320)
    // then has the folio mark each bitmap's buffer as its own (bm_SysMalloc, 0x3684), which
    // DeleteScreenGroup gives back, and gives the table back once the group is made (0x46e0) --
    // even when it was not; the 1993 folio does neither.
    bool later = pf_os_release() >= 23;
    uint32_t lists = pf_r32(pf_current_task() + T_FREEMEMORYLISTS), table = 0;
    int32_t table_size = (int32_t)((uint32_t)(bitmaps * screens) << 2);
    auto give_table_back = [&] { if (table && later) pf_free_mem(lists, table, table_size); };
    if (!arg[CSG_BUFFERS]) {
        uint32_t p = pf_alloc_mem(lists, table_size, 0, true);
        if (!p) {
            c.r[0] = GRAFERR_NOMEM;
            return;
        }
        table = p;
        arg[CSG_BUFFERS] = p;
        for (int32_t s = 0; s < screens; ++s) {
            uint32_t widths = arg[CSG_WIDTHS], heights = arg[CSG_HEIGHTS];
            uint32_t width = graf(GF_DEFAULTDISPLAYWIDTH), height = graf(GF_DEFAULTDISPLAYHEIGHT);
            for (int32_t b = 0; b < bitmaps; ++b, p += 4) {
                if (heights) { height = pf_r32(heights); heights += 4; }
                if (widths) { width = pf_r32(widths); widths += 4; }
                uint32_t size = width * 2 * height, flags = MEMTYPE_VRAM | MEMTYPE_CEL;
                if (arg[CSG_SPORTBITS]) {
                    uint32_t page = graf(GF_VRAMPAGESIZE);
                    size = (page - 1 + size) / page * page;
                    flags |= arg[CSG_SPORTBITS] | MEMTYPE_STARTPAGE;
                }
                uint32_t buf = pf_alloc_mem(lists, (int32_t)size, flags, true);
                pf_w32(p, buf);
                if (!buf) {
                    c.r[0] = GRAFERR_NOMEM;
                    give_table_back();
                    return;
                }
            }
        }
    }
    c.r[0] = csg_supervisor(c, c.r[0], arg, table && later);
    give_table_back();
}

// Graphics -104: Err AddScreenGroup(Item group, TagArg* tags) -- SWI 17 (0x31dc): marks the
// group added, once; the tags are not read.
static void g_addscreengroup(ArmCpu& c) {
    uint32_t sg = own_item(c, c.r[0], TYPE_SCREENGROUP);
    if (!sg) c.r[0] = GRAFERR_BADITEM;
    else if (pf_r32(sg + SG_ADD_SG_CALLED)) c.r[0] = GRAFERR_SGINUSE;
    else {
        pf_w32(sg + SG_ADD_SG_CALLED, 1);
        c.r[0] = 0;
    }
}

// Graphics -108: Err RemoveScreenGroup(Item group) -- SWI 18 (0x3278; 23.10's graphix, 0x39a0,
// the same): the group checked as AddScreenGroup checks it, and then 0 when it was added,
// GRAFERR_SGNOTINUSE when not. Neither folio clears sg_Add_SG_Called or changes the display.
static void g_removescreengroup(ArmCpu& c) {
    uint32_t sg = own_item(c, c.r[0], TYPE_SCREENGROUP);
    if (!sg) c.r[0] = GRAFERR_BADITEM;
    else c.r[0] = pf_r32(sg + SG_ADD_SG_CALLED) ? 0 : GRAFERR_SGNOTINUSE;
}

// The folio's ir_Delete (23.10's graphix 0x2398; the 1993 folio's DeleteScreenGroup is not
// implemented, GRAFERR_NOTYET, so a program that deletes a group is a later one's): a screen
// group (0x20b8) leaves the folio's list of them; a screen (0x2188) whose VDL is the one shown,
// even or odd field, gives the display the blank VDL in both; a bitmap (0x22e0) nothing more; a
// VDL (0x5380) gives its data back to the OS's lists. Each also frees its list of the tasks it is
// shared with, which the runtime does not keep.
static int32_t graphics_delete(ArmCpu& c, int type, int32_t item, uint32_t task) {
    (void)c;
    (void)task;
    uint32_t n = pf_item_node(item), g = pf_folio_base(PF_GRAPHICS);
    if (type == (int)TYPE_SCREEN) {
        uint32_t data = pf_r32(pf_r32(n + SCR_VDLPTR) + VDL_DATAPTR);
        if (pf_r32(g + GF_CURRENTVDLEVEN) == data || pf_r32(g + GF_CURRENTVDLODD) == data) {
            pf_w32(g + GF_CURRENTVDLEVEN, graf(GF_VDLBLANK));
            pf_w32(g + GF_CURRENTVDLODD, graf(GF_VDLBLANK));
        }
    } else if (type == (int)TYPE_VDL) {
        if (uint32_t data = pf_r32(n + VDL_DATAPTR)) pf_free_mem(pf_kernel_lists(), data, (int32_t)pf_r32(n + VDL_DATASIZE));
    }
    return 0;
}

// Graphics -92: Err DeleteScreenGroup(Item group) -- 23.10's graphix 0x4724, in the caller's mode:
// RemoveScreenGroup (its result not looked at); then for each screen of the group, each bitmap on
// the screen's list -- its buffer given back to the task's lists when the folio allocated it
// (bm_SysMalloc), the bitmap deleted -- then the screen's VDL and the screen deleted; then the
// group; DeleteItem's result back. The runtime's groups are the 1993 folio's: no list of their
// screens (the screens are the items that point at the group, in the order they were made), and
// a screen's bitmap in scr_TempBitmap rather than on its list.
static void g_deletescreengroup(ArmCpu& c) {
    int32_t group = (int32_t)c.r[0];
    uint32_t sg = pf_check_item(group, NST_GRAPHICS, TYPE_SCREENGROUP);
    if (!sg) pf_stop(c, "DeleteScreenGroup of no screen group: not yet");
    g_removescreengroup(c);
    for (int32_t i = 1; i < pf_item_count(); ++i) {
        uint32_t scr = pf_check_item(i, NST_GRAPHICS, TYPE_SCREEN);
        if (!scr || pf_r32(scr + SCR_SCREENGROUPPTR) != sg) continue;
        std::vector<uint32_t> bitmaps;
        for (uint32_t b = pf_r32(scr + SCR_BITMAPLIST + PF_LIST_HEAD); b != scr + SCR_BITMAPLIST + PF_LIST_TAIL; b = pf_r32(b))
            bitmaps.push_back(b);
        if (uint32_t t = pf_r32(scr + SCR_TEMPBITMAP)) bitmaps.push_back(t);
        for (uint32_t bm : bitmaps) {
            if (pf_r32(bm + BM_SYSMALLOC))
                pf_free_mem(pf_r32(pf_current_task() + T_FREEMEMORYLISTS), pf_r32(bm + BM_BUFFER),
                            (int32_t)(pf_r32(bm + BM_WIDTH) * pf_r32(bm + BM_HEIGHT) * 2));
            pf_delete_item(c, (int32_t)pf_r32(bm + 24));
        }
        pf_delete_item(c, (int32_t)pf_r32(scr + SCR_VDLITEM));
        pf_delete_item(c, i);
    }
    c.r[0] = (uint32_t)pf_delete_item(c, group);
}

// The GrafCon's setters, code that runs in the caller (23.10's graphix): Graphics -96
// SetFGPen(GrafCon* gc, Color c) -- 0x2b10, gc_FGPen (+0x14); -100 SetBGPen -- 0x2b18, gc_BGPen
// (+0x18); -120 MoveTo(GrafCon* gc, Coord x, Coord y) -- 0x2b04, gc_PenX and gc_PenY (+0x1c, +0x20).
static void g_setfgpen(ArmCpu& c) { pf_w32(c.r[0] + 0x14, c.r[1]); }
static void g_setbgpen(ArmCpu& c) { pf_w32(c.r[0] + 0x18, c.r[1]); }
static void g_moveto(ArmCpu& c) {
    pf_w32(c.r[0] + 0x1c, c.r[1]);
    pf_w32(c.r[0] + 0x20, c.r[2]);
}

// Enable/DisableHAVG and Enable/DisableVAVG(Item screen) -- SWIs 5 to 8 (0x1620): the
// horizontal and vertical averaging bits (4 and 8) of the display control word of the screen
// VDL's first entry.
static void averaging(ArmCpu& c, uint32_t set, uint32_t clear) {
    uint32_t scr = own_item(c, c.r[0], TYPE_SCREEN);
    if (!scr) {
        c.r[0] = GRAFERR_BADITEM;
        return;
    }
    uint32_t w = pf_r32(pf_r32(scr + SCR_VDLPTR) + VDL_DATAPTR) + 16;
    pf_w32(w, (pf_r32(w) | set) & ~clear);
    c.r[0] = 0;
}

static void g_enablevavg(ArmCpu& c) { averaging(c, 8, 0); }
static void g_disablevavg(ArmCpu& c) { averaging(c, 0, 8); }
static void g_enablehavg(ArmCpu& c) { averaging(c, 4, 0); }
static void g_disablehavg(ArmCpu& c) { averaging(c, 0, 4); }

// ---- colours and the display -----------------------------------------------------------------
// A screen's node, if the caller owns it, for the calls that check ownership after CheckItem.
static void must_own(ArmCpu& c, uint32_t n) {
    if (pf_r32(n + 28) != task_item()) pf_stop(c, "the Graphics folio: an item of another task");
}

// SWI 13 (0x1fc8): Err SetScreenColors(Item screen, uint32* entries, int32 count) -- the screen
// (CheckItem, its owner), its VDL of type VDLTYPE_SIMPLE (else GRAFERR_BADVDLTYPE); then each
// entry, index (its top byte) at most 32 (else GRAFERR_INDEXRANGE, the earlier ones written) and
// its 24-bit colour, into the colour words of the VDL's first entry (from its sixth word): index
// 32 is the background word, 0xE0000000 | the colour. Only the first entry -- the screen's first
// bitmap's -- is written.
template <class Entry>
static uint32_t set_screen_colors(ArmCpu& c, uint32_t item, int32_t count, Entry entry) {
    uint32_t scr = pf_check_item((int32_t)item, NST_GRAPHICS, TYPE_SCREEN);
    if (!scr) return GRAFERR_BADITEM;
    must_own(c, scr);
    if (pf_r32(scr + SCR_VDLTYPE) != 4) return GRAFERR_BADVDLTYPE;
    for (int32_t i = 0; i < count; ++i) {
        uint32_t e = entry(i), index = e >> 24;
        if (index > 32) return GRAFERR_INDEXRANGE;
        uint32_t w = (index == 32 ? 0xE0000000u : index << 24) | (e & 0xFFFFFF);
        pf_w32(pf_r32(pf_r32(scr + SCR_VDLPTR) + VDL_DATAPTR) + 20 + 4 * index, w);
    }
    return 0;
}

// Graphics -88: SetScreenColors -- SWI 13. Graphics -80: Err SetScreenColor(Item screen,
// uint32 entry) -- SWI 9 (0x1fa0): the one entry, from the SWI's own stack.
static void g_setscreencolors(ArmCpu& c) {
    uint32_t entries = c.r[1];
    c.r[0] = set_screen_colors(c, c.r[0], (int32_t)c.r[2], [&](int32_t i) { return pf_r32(entries + 4u * (uint32_t)i); });
}
static void g_setscreencolor(ArmCpu& c) {
    uint32_t e = c.r[1];
    if (g_pf_trace) pf_log("        colour %u: 0x%06x\n", e >> 24, e & 0xFFFFFF);
    c.r[0] = set_screen_colors(c, c.r[0], 1, [&](int32_t) { return e; });
}

// Graphics -84: Err ResetScreenColors(Item screen) -- SWI 10 (0x20e8): SetScreenColor of the grey
// ramp, entry i being i * 255 / 31 in each channel, stopping at the first error.
static void g_resetscreencolors(ArmCpu& c) {
    uint32_t item = c.r[0];
    for (uint32_t i = 0; i < 32; ++i) {
        uint32_t v = i * 255 / 31;
        c.r[0] = item;
        c.r[1] = i << 24 | v << 16 | v << 8 | v;
        g_setscreencolor(c);
        if ((int32_t)c.r[0] < 0) return;
    }
    c.r[0] = 0;
}

// Graphics -160: Err DisplayScreen(Item screen0, Item screen1) -- SWI 45 (0x307c): both screens
// (screen1 0: screen0 again), CheckItem'd, then each owned; the first's group (none:
// GRAFERR_INTERNALERROR) an item still (else GRAFERR_SGNOTINUSE -- AddScreenGroup is not asked
// for), the second's the same group (else GRAFERR_MIXEDSCREENS); then the first VDL's data into
// gf_CurrentVDLEven and the second's into gf_CurrentVDLOdd, which the next blanks link in.
static void g_displayscreen(ArmCpu& c) {
    uint32_t s0 = pf_check_item((int32_t)c.r[0], NST_GRAPHICS, TYPE_SCREEN);
    uint32_t s1 = c.r[1] ? pf_check_item((int32_t)c.r[1], NST_GRAPHICS, TYPE_SCREEN) : s0;
    if (!s0 || !s1) { c.r[0] = GRAFERR_BADITEM; return; }
    must_own(c, s0);
    must_own(c, s1);
    uint32_t sg = pf_r32(s0 + SCR_SCREENGROUPPTR);
    if (!sg) { c.r[0] = GRAFERR_INTERNALERROR; return; }
    if (!pf_check_item((int32_t)pf_r32(sg + 24), NST_GRAPHICS, TYPE_SCREENGROUP)) { c.r[0] = GRAFERR_SGNOTINUSE; return; }
    if (pf_r32(s1 + SCR_SCREENGROUPPTR) != sg) { c.r[0] = GRAFERR_MIXEDSCREENS; return; }
    uint32_t g = pf_folio_base(PF_GRAPHICS);
    pf_w32(g + GF_CURRENTVDLEVEN, pf_r32(pf_r32(s0 + SCR_VDLPTR) + VDL_DATAPTR));
    pf_w32(g + GF_CURRENTVDLODD, pf_r32(pf_r32(s1 + SCR_VDLPTR) + VDL_DATAPTR));
    if (g_pf_trace) pf_log("        display %d and %d\n", (int32_t)c.r[0], (int32_t)c.r[1]);
    c.r[0] = 0;
}

// ---- the cel engine --------------------------------------------------------------------------
// Graphics -172: Err DrawCels(Item bitmap, CCB* ccb) -- SWI 39 (0x1298): the bitmap (CheckItem,
// else GRAFERR_BADITEM); not the caller's, it must have it open (ItemOpened, else
// GRAFERR_NOTOWNER). Then, under the folio's semaphore, the bitmap's control word and REGCTL0-3
// into MADAM, the CCB, the cel engine started, and a wait for it with a watchdog of
// bm_WatchDogCtr (GRAFERR_CELTIMEOUT when it fires); here the engine runs to its end at once.
static void g_drawcels(ArmCpu& c) {
    uint32_t bm = pf_check_item((int32_t)c.r[0], NST_GRAPHICS, TYPE_BITMAP);
    if (!bm) { c.r[0] = GRAFERR_BADITEM; return; }
    if (pf_r32(bm + 28) != task_item() && pf_item_opened((int32_t)task_item(), (int32_t)c.r[0]) < 0) {
        c.r[0] = GRAFERR_NOTOWNER;
        return;
    }
    const uint32_t regctl[4] = {pf_r32(bm + BM_REGCTL0), pf_r32(bm + BM_REGCTL1), pf_r32(bm + BM_REGCTL2),
                                pf_r32(bm + BM_REGCTL3)};
    pf_cel_draw(c, pf_r32(bm + BM_CECONTROL), regctl, c.r[1]);
    c.r[0] = 0;
}

// Graphics -152: Err SetCEControl(Item bitmap, int32 word, int32 mask) -- SWI 41 (0x10e4): the
// bitmap (CheckItem, else GRAFERR_BADITEM), the caller's or open (else GRAFERR_NOTOWNER); the
// bits of bm_CEControl under the mask become the word's; 0.
static void g_setcecontrol(ArmCpu& c) {
    uint32_t bm = pf_check_item((int32_t)c.r[0], NST_GRAPHICS, TYPE_BITMAP);
    if (!bm) { c.r[0] = GRAFERR_BADITEM; return; }
    if (pf_r32(bm + 28) != task_item() && pf_item_opened((int32_t)task_item(), (int32_t)c.r[0]) < 0) {
        c.r[0] = GRAFERR_NOTOWNER;
        return;
    }
    pf_w32(bm + BM_CECONTROL, (pf_r32(bm + BM_CECONTROL) & ~c.r[2]) | (c.r[1] & c.r[2]));
    c.r[0] = 0;
}

// Graphics -132: Err DrawTo(Item bitmap, GrafCon* gc, Coord x, Coord y) -- SWI 33 (0x2648; 23.10's
// graphix 0x2d78, the same): a line from the pen to (x, y) as one cel the folio builds on its
// stack and draws with DrawCels -- a single 16-bit uncoded pixel of gc_FGPen (preamble 0x16 and 0,
// the pixel in the high half of the next word), CCB flags 0x57260030 (LAST, SPABS, LDSIZE, LDPRS,
// LDPPMP, YOXY, ACW, ACCW, BGND, NOBLK), PIXC 0x1f401f40, width and height 1. It runs downward,
// from whichever end is higher: VDX and VDY the line, in 16.16. A line at least as wide as tall
// steps its pixel by HDY -1 (12.20) and grows by one pixel each way (VDY + 1, VDX away from 0 by
// 1, the start moved right by 1 when VDX is negative); a steeper one by HDX 1, VDX away from 0 by
// 1, VDY + 1. The pen moves to (x, y); DrawCels' result back.
static void g_drawto(ArmCpu& c) {
    static uint32_t s_block;
    if (!s_block) s_block = pf_os_alloc(0x50);
    uint32_t gc = c.r[1];
    int32_t x = (int32_t)c.r[2], y = (int32_t)c.r[3];
    int32_t penx = (int32_t)pf_r32(gc + 0x1c), peny = (int32_t)pf_r32(gc + 0x20);
    uint32_t b = s_block, ccb = b + 0xc;
    for (uint32_t i = 0; i < 0x50; i += 4) pf_w32(b + i, 0);
    pf_w32(b + 0, 0x16);
    pf_w32(b + 8, pf_r32(gc + 0x14) << 16);
    pf_w32(ccb + 0x00, 0x57260030u);
    pf_w32(ccb + 0x08, b);
    pf_w32(ccb + 0x30, 0x1f401f40u);
    pf_w32(ccb + 0x3c, 1);
    pf_w32(ccb + 0x40, 1);
    auto fx = [](int32_t v) { return (int32_t)((uint32_t)v << 16); };    // 16.16
    int32_t sx, sy, vdx, vdy;
    if (y >= peny) {
        sx = fx(penx); sy = fx(peny); vdx = fx(x) - sx; vdy = fx(y) - sy;
    } else {
        sx = fx(x); sy = fx(y); vdx = fx(penx) - sx; vdy = fx(peny) - sy;
    }
    int32_t hdx, hdy;
    if ((vdx < 0 ? -vdx : vdx) >= vdy) {
        hdx = 0;
        hdy = (int32_t)0xFFF00000u;
        vdy += 0x10000;
        if (vdx >= 0) vdx += 0x10000;
        else { vdx -= 0x10000; sx += 0x10000; }
    } else {
        hdx = 0x100000;
        hdy = 0;
        vdx += vdx < 0 ? -0x10000 : 0x10000;
        vdy += 0x10000;
    }
    pf_w32(ccb + 0x10, (uint32_t)sx);
    pf_w32(ccb + 0x14, (uint32_t)sy);
    pf_w32(ccb + 0x18, (uint32_t)hdx);
    pf_w32(ccb + 0x1c, (uint32_t)hdy);
    pf_w32(ccb + 0x20, (uint32_t)vdx);
    pf_w32(ccb + 0x24, (uint32_t)vdy);
    pf_w32(gc + 0x1c, (uint32_t)x);
    pf_w32(gc + 0x20, (uint32_t)y);
    c.r[1] = ccb;
    g_drawcels(c);
}

// Graphics -60: Err SetClipOrigin(Item bitmap, int32 x, int32 y) -- SWI 3 (0x1e98): the bitmap
// (CheckItem, else GRAFERR_BADITEM), the caller's or open (else GRAFERR_NOTOWNER); y made even;
// the clip rectangle must lie inside the bitmap -- x and y at least 0, x + bm_ClipWidth at most
// bm_Width, y + bm_ClipHeight at most bm_Height (else GRAFERR_BADCLIP). The buffer's address of
// (x, y) in the line pairs becomes the cel engine's write address (REGCTL3), and its read address
// (REGCTL2) too when that was the write address; bm_ClipX and bm_ClipY take x and y. The clip's
// size (REGCTL1) stays.
enum : uint32_t { BM_CLIPX = 0x40, BM_CLIPY = 0x44, GRAFERR_BADCLIP = 0xD55B9115u };
static void g_setcliporigin(ArmCpu& c) {
    uint32_t bm = pf_check_item((int32_t)c.r[0], NST_GRAPHICS, TYPE_BITMAP);
    if (!bm) { c.r[0] = GRAFERR_BADITEM; return; }
    if (pf_r32(bm + 28) != task_item() && pf_item_opened((int32_t)task_item(), (int32_t)c.r[0]) < 0) {
        c.r[0] = GRAFERR_NOTOWNER;
        return;
    }
    int32_t x = (int32_t)c.r[1], y = (int32_t)c.r[2] & ~1;
    int32_t w = (int32_t)pf_r32(bm + BM_WIDTH), h = (int32_t)pf_r32(bm + BM_HEIGHT);
    if (x < 0 || x + (int32_t)pf_r32(bm + BM_CLIPWIDTH) > w || y < 0 || y + (int32_t)pf_r32(bm + BM_CLIPHEIGHT) > h) {
        c.r[0] = GRAFERR_BADCLIP;
        return;
    }
    uint32_t a = pf_r32(bm + BM_BUFFER) + (uint32_t)(y * w + 2 * x) * 2;
    if (pf_r32(bm + BM_REGCTL2) == pf_r32(bm + BM_REGCTL3)) pf_w32(bm + BM_REGCTL2, a);
    pf_w32(bm + BM_CLIPX, (uint32_t)x);
    pf_w32(bm + BM_CLIPY, (uint32_t)y);
    pf_w32(bm + BM_REGCTL3, a);
    c.r[0] = 0;
}

// Graphics -112: Err SetClipWidth(Item bitmap, int32 w) -- SWI 19 (0x1cfc), and -116 SetClipHeight
// (Item bitmap, int32 h) -- SWI 20 (0x1dc8): the bitmap as SetClipOrigin takes it; the size above
// 0, and the clip's origin plus it at most bm_Width (bm_Height), else GRAFERR_BADCLIP; then
// bm_ClipWidth (bm_ClipHeight) and REGCTL1, the last column and row the engine draws into.
static void set_clip_size(ArmCpu& c, bool height) {
    uint32_t bm = pf_check_item((int32_t)c.r[0], NST_GRAPHICS, TYPE_BITMAP);
    if (!bm) { c.r[0] = GRAFERR_BADITEM; return; }
    if (pf_r32(bm + 28) != task_item() && pf_item_opened((int32_t)task_item(), (int32_t)c.r[0]) < 0) {
        c.r[0] = GRAFERR_NOTOWNER;
        return;
    }
    int32_t n = (int32_t)c.r[1];
    if (n <= 0 || n + (int32_t)pf_r32(bm + (height ? BM_CLIPY : BM_CLIPX)) > (int32_t)pf_r32(bm + (height ? BM_HEIGHT : BM_WIDTH))) {
        c.r[0] = GRAFERR_BADCLIP;
        return;
    }
    pf_w32(bm + (height ? BM_CLIPHEIGHT : BM_CLIPWIDTH), (uint32_t)n);
    pf_w32(bm + BM_REGCTL1, (pf_r32(bm + BM_CLIPWIDTH) - 1) | (pf_r32(bm + BM_CLIPHEIGHT) - 1) << 16);
    c.r[0] = 0;
}
static void g_setclipwidth(ArmCpu& c) { set_clip_size(c, false); }
static void g_setclipheight(ArmCpu& c) { set_clip_size(c, true); }

// GRAPHIX's division (0x424, Norcroft's __rt_sdiv unrolled): n / d truncated toward zero, the
// quotient's bits 30 to 0 found by restoring subtraction on the magnitudes -- so d = 0 gives
// 0x7FFFFFFF with the sign of n, and nothing stops.
static uint32_t graphix_sdiv(uint32_t d, uint32_t n) {
    bool neg_n = n >= 0x80000000u, neg_q = ((d ^ n) >> 31) != 0;
    uint32_t r = neg_n ? 0u - n : n, dd = d >= 0x80000000u ? 0u - d : d, q = 0;
    for (int k = 30; k >= 0; --k) {
        bool take = (r >> k) >= dd;
        if (take) r -= dd << k;
        q = q << 1 | (take ? 1u : 0u);
    }
    return neg_q ? 0u - q : q;
}

// Graphics -4: void MapCel(CCB* ccb, Point quad[4]) -- GRAPHIX 0x14f4: the cel's corner at the
// quad's first point (x and y the low 16 bits of each, as 16.16 with 0x8000 added), its first row
// along quad[1] - quad[0] by ccb_Width (HDX, HDY: 12.20), its first column along quad[3] -
// quad[0] by ccb_Height (VDX, VDY: 16.16), and the rows' change along quad[2] - quad[3] -
// quad[1] + quad[0] by width times height (HDDX, HDDY: 12.20). r0 is left as the last quotient.
enum : uint32_t {
    CCB_XPOS = 0x10, CCB_YPOS = 0x14, CCB_HDX = 0x18, CCB_HDY = 0x1c, CCB_VDX = 0x20, CCB_VDY = 0x24,
    CCB_HDDX = 0x28, CCB_HDDY = 0x2c, CCB_WIDTH = 0x3c, CCB_HEIGHT = 0x40,
};
static void g_mapcel(ArmCpu& c) {
    uint32_t ccb = c.r[0], q = c.r[1];
    auto x = [q](int i) { return pf_r32(q + 8u * i); };
    auto y = [q](int i) { return pf_r32(q + 8u * i + 4); };
    uint32_t w = pf_r32(ccb + CCB_WIDTH), h = pf_r32(ccb + CCB_HEIGHT);
    pf_w32(ccb + CCB_XPOS, (x(0) & 0xFFFFu) << 16 | 0x8000u);
    pf_w32(ccb + CCB_YPOS, (y(0) & 0xFFFFu) << 16 | 0x8000u);
    pf_w32(ccb + CCB_HDX, graphix_sdiv(w, (x(1) - x(0)) << 20));
    pf_w32(ccb + CCB_HDY, graphix_sdiv(w, (y(1) - y(0)) << 20));
    pf_w32(ccb + CCB_VDX, graphix_sdiv(h, (x(3) - x(0)) << 16));
    pf_w32(ccb + CCB_VDY, graphix_sdiv(h, (y(3) - y(0)) << 16));
    pf_w32(ccb + CCB_HDDX, graphix_sdiv(w * h, (x(2) - x(3) - x(1) + x(0)) << 20));
    uint32_t hddy = graphix_sdiv(w * h, (y(2) - y(3) - y(1) + y(0)) << 20);
    pf_w32(ccb + CCB_HDDY, hddy);
    c.r[0] = hddy;
}

// Graphics -192: Err QueryGraphics(int32 tag, void* value) -- 23.10's (graphix 0x60c4; the 1993
// folio's slot is GRAFERR_NOTYET for anything): per tag a word into *value, then 0 -- 1 the field
// rate (gf +0xc8), 2 a field's time in microseconds (+0xc4), 3 the fields so far (23.10 keeps its
// own count, 0x91c8; here gf_VBLNumber), 4 and 5 the default display's width and height (+0x84,
// +0x88), 6 the display type (+0x140, which 1993's GrafFolio lacks: 1, DI_TYPE_NTSC, the console
// the runtime is -- 2 and 3 are PAL); tag 0 writes nothing, any other is GRAFERR_BADTAG.
static void g_querygraphics(ArmCpu& c) {
    uint32_t v;
    switch (c.r[0]) {
    case 0: c.r[0] = 0; return;
    case 1: v = graf(GF_VBLFREQ); break;
    case 2: v = graf(GF_VBLTIME); break;
    case 3: v = graf(GF_VBLNUMBER); break;
    case 4: v = graf(GF_DEFAULTDISPLAYWIDTH); break;
    case 5: v = graf(GF_DEFAULTDISPLAYHEIGHT); break;
    case 6: v = 1; break;
    default: c.r[0] = GRAFERR_BADTAG; return;
    }
    pf_w32(c.r[1], v);
    c.r[0] = 0;
}

static void graphics_slots() {
    pf_on_slot(PF_GRAPHICS, -192, g_querygraphics);
    pf_on_slot(PF_GRAPHICS, -4, g_mapcel);
    pf_on_slot(PF_GRAPHICS, -48, g_createscreengroup);
    pf_on_slot(PF_GRAPHICS, -60, g_setcliporigin);
    pf_on_slot(PF_GRAPHICS, -112, g_setclipwidth);
    pf_on_slot(PF_GRAPHICS, -116, g_setclipheight);
    pf_on_slot(PF_GRAPHICS, -64, g_enablevavg);
    pf_on_slot(PF_GRAPHICS, -68, g_disablevavg);
    pf_on_slot(PF_GRAPHICS, -72, g_enablehavg);
    pf_on_slot(PF_GRAPHICS, -76, g_disablehavg);
    pf_on_slot(PF_GRAPHICS, -80, g_setscreencolor);
    pf_on_slot(PF_GRAPHICS, -84, g_resetscreencolors);
    pf_on_slot(PF_GRAPHICS, -88, g_setscreencolors);
    pf_on_slot(PF_GRAPHICS, -104, g_addscreengroup);
    pf_on_slot(PF_GRAPHICS, -108, g_removescreengroup);
    pf_on_slot(PF_GRAPHICS, -92, g_deletescreengroup);
    pf_on_slot(PF_GRAPHICS, -96, g_setfgpen);
    pf_on_slot(PF_GRAPHICS, -100, g_setbgpen);
    pf_on_slot(PF_GRAPHICS, -120, g_moveto);
    pf_on_slot(PF_GRAPHICS, -152, g_setcecontrol);
    pf_on_slot(PF_GRAPHICS, -132, g_drawto);
    pf_on_delete(NST_GRAPHICS, graphics_delete);
    pf_on_slot(PF_GRAPHICS, -160, g_displayscreen);
    pf_on_slot(PF_GRAPHICS, -172, g_drawcels);
}
