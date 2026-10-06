// 3dokit runtime -- the Graphics folio: its node's fields and its SWIs and vector slots, as far
// as the programs run so far reach them. What a field holds is what the 1993 folio
// (System/Folios/GRAPHIX, decompressed by its own code in 3dokit.armemu) puts there when it
// starts.
#include "pf.h"
#include <cstdio>
#include <cstdlib>

void pf_graphics_init() {
    uint32_t g = pf_folio_base(PF_GRAPHICS);
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
    uint32_t virs = pf_alloc_mem(pf_kernel_lists(), (int32_t)(2 * page), MEMTYPE_VRAM | MEMTYPE_STARTPAGE, false);
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
}
