// 3dokit runtime -- the Graphics folio: its node's fields and its SWIs and vector slots, as far
// as the programs run so far reach them. What a field holds is what the 1993 folio
// (System/Folios/GRAPHIX, decompressed by its own code in 3dokit.armemu) puts there when it
// starts, and what a function does is what its code there does (the addresses are GRAPHIX's).
#include "pf.h"
#include <cstdio>
#include <cstdlib>

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
    GRAFERR_NOMEM = 0xD55B9006u, GRAFERR_BUFWIDTH = 0xD55B9118u, GRAFERR_VDLWIDTH = 0xD55B911Au,
    GRAFERR_NOTYET = 0xD55B911Bu, GRAFERR_VDL_LENGTH = 0xD55B9121u, GRAFERR_BADDISPDIMS = 0xD55B9123u,
    GRAFERR_BADBITMAPSPEC = 0xD55B9124u, GRAFERR_INTERNALERROR = 0xD55B9125u,
    GRAFERR_SGINUSE = 0xD55B9126u, GRAFERR_NOWRITEACCESS = 0xD55B9129u,
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

void pf_graphics_init() {
    graphics_slots();
    uint32_t g = pf_folio_base(PF_GRAPHICS), kl = pf_kernel_lists();
    uint32_t page = pf_page_size(MEMTYPE_VRAM);             // the VRAM page, 2 KB
    pf_w32(g + GF_VBLNUMBER, 0);
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
static uint32_t csg_supervisor(ArmCpu& c, uint32_t items, uint32_t* arg) {
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
            pf_w32(bm + BM_SYSMALLOC, 0);
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
    if (!arg[CSG_BUFFERS]) {
        uint32_t lists = pf_r32(pf_current_task() + T_FREEMEMORYLISTS);
        uint32_t p = pf_alloc_mem(lists, (int32_t)((uint32_t)(bitmaps * screens) << 2), 0, true);
        if (!p) {
            c.r[0] = GRAFERR_NOMEM;
            return;
        }
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
                    return;
                }
            }
        }
    }
    c.r[0] = csg_supervisor(c, c.r[0], arg);
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

static void graphics_slots() {
    pf_on_slot(PF_GRAPHICS, -48, g_createscreengroup);
    pf_on_slot(PF_GRAPHICS, -64, g_enablevavg);
    pf_on_slot(PF_GRAPHICS, -68, g_disablevavg);
    pf_on_slot(PF_GRAPHICS, -72, g_enablehavg);
    pf_on_slot(PF_GRAPHICS, -76, g_disablehavg);
    pf_on_slot(PF_GRAPHICS, -104, g_addscreengroup);
}
