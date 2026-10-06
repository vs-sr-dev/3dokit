// 3dokit runtime -- memory: the MemHdrs, the OS's and the task's MemLists, and the allocator
// over them, built and run the way Portfolio's kernel does it (read in the 1993 kernel,
// System/Kernel/os_code v0.16, decompressed by its own code in 3dokit.armemu).
//
// A program reads these structures itself -- lib3DO's GetMemType reads the MemHdr FindMH
// returns, a game sums its t_FreeMemoryLists -- so they are mem.h's, in guest memory, and a
// free fragment is a node inside the free memory, as on the console:
//
//   MemHdr   one per kind of memory, on kb_MemHdrList: its range, its page sizes and
//            memh_FreePageBits, a bit set for every page nobody owns.
//   MemList  one per MemHdr on a list of them (kb_MemFreeLists for the OS,
//            t_FreeMemoryLists for a task): meml_OwnBits, the pages it owns, and meml_l,
//            the free parts of them in address order, each a MEMFREENODE whose n_Size is
//            its length.
//
// The machine is a retail 3DO: DRAM 0-0x1FFFFF in 64 pages of 32 KB, VRAM 0x200000-0x2FFFFF
// in 64 pages of 16 KB, whose VRAM page (the SPORT's) is 2 KB. DRAM is first on every list
// (priority 101 against VRAM's 100), so a request that names no kind of memory gets DRAM.
#include "pf.h"
#include <cstdio>
#include <cstdlib>

enum : uint32_t {
    // MemHdr (mem.h)
    MH_TYPES = 0x14, MH_PAGESIZE = 0x18, MH_PAGEMASK = 0x1c, MH_VRAMPAGESIZE = 0x20,
    MH_VRAMPAGEMASK = 0x24, MH_FREEPAGEBITS = 0x28, MH_MEMBASE = 0x2c, MH_MEMTOP = 0x30,
    MH_BITSSIZE = 0x34, MH_PAGESHIFT = 0x35, MH_VRAMPAGESHIFT = 0x36, MH_SIZE = 0x38,
    // MemList (mem.h)
    ML_TYPES = 0x14, ML_OWNBITS = 0x18, ML_WRITEBITS = 0x1c, ML_MEMHDR = 0x20, ML_LIST = 0x24,
    ML_SEMA4 = 0x28, ML_OWNBITSSIZE = 0x2c, ML_SIZE = 0x30,
    // node types (kernelnodes.h)
    KERNELNODE = 1, MEMFREENODE = 1, MEMHDRNODE = 3, TASKNODE = 5, MEMLISTNODE = 11,
    // what the kernel's list walks mask a request's flags with
    KINDS = MEMTYPE_VRAM | MEMTYPE_DMA | MEMTYPE_CEL,
    DRAM_TOP = 0x200000u,
};

static uint32_t kb(uint32_t field) { return pf_r32(pf_folio_base(PF_KERNEL) + field); }

uint32_t pf_current_task() { return kb(KB_CURRENTTASK); }
uint32_t pf_kernel_lists() { return kb(KB_MEMFREELISTS); }

#define FOR_NODES(n, l) for (uint32_t n = pf_r32((l) + PF_LIST_HEAD); n != (l) + PF_LIST_TAIL; n = pf_r32(n))

// ---- page bits: bit i of word i / 32 -------------------------------------------------------
static bool bit(uint32_t bits, uint32_t i) { return pf_r32(bits + 4 * (i >> 5)) >> (i & 31) & 1; }

static void set_bit(uint32_t bits, uint32_t i, bool on) {
    uint32_t a = bits + 4 * (i >> 5), w = pf_r32(a), m = 1u << (i & 31);
    pf_w32(a, on ? w | m : w & ~m);
}

// ---- finding ----------------------------------------------------------------------------
// The MemList on `lists` that manages MemHdr `mh`.
static uint32_t memlist_for(uint32_t lists, uint32_t mh) {
    FOR_NODES(ml, lists) if (pf_r32(ml + ML_MEMHDR) == mh) return ml;
    return 0;
}

// The first MemList on `lists` with every kind of memory `flags` names.
static uint32_t memlist_of_kind(uint32_t lists, uint32_t flags) {
    uint32_t k = flags & KINDS;
    FOR_NODES(ml, lists) if ((pf_r32(ml + ML_TYPES) & k) == k) return ml;
    return 0;
}

// The MemList on `lists` whose memory holds `p`.
static uint32_t memlist_holding(uint32_t lists, uint32_t p) {
    FOR_NODES(ml, lists) {
        uint32_t mh = pf_r32(ml + ML_MEMHDR);
        if ((int32_t)pf_r32(mh + MH_MEMBASE) <= (int32_t)p && (int32_t)p < (int32_t)pf_r32(mh + MH_MEMTOP))
            return ml;
    }
    return 0;
}

// Kernel -100: MemHdr* FindMH(void* p) -- the MemHdr whose range holds p, or NULL
uint32_t pf_find_mh(uint32_t p) {
    uint32_t l = kb(KB_MEMHDRLIST);
    FOR_NODES(mh, l)
        if ((int32_t)pf_r32(mh + MH_MEMBASE) <= (int32_t)p && (int32_t)p < (int32_t)pf_r32(mh + MH_MEMTOP))
            return mh;
    return 0;
}

// The unit an allocation is aligned to: the MemHdr's page, or for VRAM its VRAM page unless
// MEMTYPE_SYSTEMPAGESIZE asks for the other.
static uint32_t align_unit(uint32_t mh, uint32_t flags) {
    if ((flags & MEMTYPE_VRAM) && !(flags & MEMTYPE_SYSTEMPAGESIZE)) return pf_r32(mh + MH_VRAMPAGESIZE);
    return pf_r32(mh + MH_PAGESIZE);
}

// Kernel -60: int32 GetPageSize(uint32 flags) -- of the OS's memory of that kind
uint32_t pf_page_size(uint32_t flags) {
    uint32_t ml = memlist_of_kind(kb(KB_MEMFREELISTS), flags);
    return ml ? align_unit(pf_r32(ml + ML_MEMHDR), flags) : 0;
}

// ---- pages ----------------------------------------------------------------------------------
// `n` pages of `mh` in a row. For a task the lowest free run, owned and writable by its
// MemList for `mh`; for the OS (task 0) the highest, owned by the OS's. A bank select limits
// the search to one half: BANK1 the lower when the MemHdr has two banks, anything else the
// upper. Returns the run's address, or 0.
static uint32_t alloc_pages(uint32_t mh, uint32_t task, int32_t n, uint32_t bank) {
    int32_t total = pf_r8(mh + MH_BITSSIZE) * 32, lo = 0, hi = total + 1 - n;
    uint32_t ml = memlist_for(task ? pf_r32(task + T_FREEMEMORYLISTS) : kb(KB_MEMFREELISTS), mh);
    if (!ml) return 0;
    if (bank) {
        if (bank & MEMTYPE_BANK1) {
            if (pf_r32(mh + MH_TYPES) & MEMTYPE_BANK2) hi = total / 2 + 1 - n;
        } else {
            lo = total / 2;
        }
    }
    uint32_t free = pf_r32(mh + MH_FREEPAGEBITS);
    auto run_free = [&](int32_t p) {
        for (int32_t i = 0; i < n; ++i)
            if (!bit(free, p + i)) return false;
        return true;
    };
    int32_t page = -1;
    if (task) {
        for (int32_t p = lo; p < hi && page < 0; ++p)
            if (run_free(p)) page = p;
    } else {
        for (int32_t p = hi - 1; p >= lo && page < 0; --p)
            if (run_free(p)) page = p;
    }
    if (page < 0) return 0;
    for (int32_t i = 0; i < n; ++i) {
        set_bit(free, page + i, false);
        set_bit(pf_r32(ml + ML_OWNBITS), page + i, true);
        if (task) set_bit(pf_r32(ml + ML_WRITEBITS), page + i, true);
    }
    return pf_r32(mh + MH_MEMBASE) + (uint32_t)page * pf_r32(mh + MH_PAGESIZE);
}

// The pages under [p, p + size) back to the system, from the MemList on `lists` that owns them
// (ControlMem's MEMC_GIVE to no task).
static void give_pages(uint32_t lists, uint32_t p, uint32_t size) {
    uint32_t ml = memlist_holding(lists, p);
    if (!ml) return;
    uint32_t mh = pf_r32(ml + ML_MEMHDR), base = pf_r32(mh + MH_MEMBASE), shift = pf_r8(mh + MH_PAGESHIFT);
    uint32_t own = pf_r32(ml + ML_OWNBITS), write = pf_r32(ml + ML_WRITEBITS);
    for (uint32_t pg = (p - base) >> shift; pg <= (p + size - 1 - base) >> shift; ++pg) {
        if (!bit(own, pg)) continue;
        set_bit(own, pg, false);
        if (write) set_bit(write, pg, false);
        set_bit(pf_r32(mh + MH_FREEPAGEBITS), pg, true);
    }
}

// swi 0x1000d: void* AllocMemBlocks(int32 size, uint32 flags) -- whole pages of the first
// MemHdr of the kinds asked that has a run free, the block's length in its first word.
// MEMTYPE_TASKMEM makes them the current task's (only from a MemHdr it has a MemList for).
static uint32_t alloc_blocks(int32_t size, uint32_t flags) {
    uint32_t k = flags & KINDS, task = flags & MEMTYPE_TASKMEM ? pf_current_task() : 0;
    uint32_t l = kb(KB_MEMHDRLIST);
    FOR_NODES(mh, l) {
        if ((pf_r32(mh + MH_TYPES) & k) != k) continue;
        if (task && !memlist_for(pf_r32(task + T_FREEMEMORYLISTS), mh)) continue;
        int32_t n = (int32_t)(size + pf_r32(mh + MH_PAGEMASK)) >> pf_r8(mh + MH_PAGESHIFT);
        if (uint32_t p = alloc_pages(mh, task, n, flags & (MEMTYPE_BANKSELECT | MEMTYPE_BANK1 | MEMTYPE_BANK2))) {
            pf_w32(p, (uint32_t)n * pf_r32(mh + MH_PAGESIZE));
            return p;
        }
    }
    return 0;
}

// ---- fragments ------------------------------------------------------------------------------
// [p, p + size) into a MemList's free nodes, in address order, merged with its neighbours.
static void free_to_memlist(uint32_t ml, uint32_t p, uint32_t size) {
    if (!p || !size) return;
    uint32_t l = pf_r32(ml + ML_LIST);
    pf_w8(p + 8, KERNELNODE);
    pf_w8(p + 9, MEMFREENODE);
    pf_w32(p + 12, size);
    uint32_t at = pf_r32(l + PF_LIST_HEAD);
    while (at != l + PF_LIST_TAIL && !(p < at)) at = pf_r32(at);
    pf_list_insert_before(at, p);
    uint32_t prev = pf_r32(p + 4);
    if (prev != l + PF_LIST_HEAD && prev + pf_r32(prev + 12) == p) {
        pf_list_rem_node(p);
        pf_w32(prev + 12, pf_r32(prev + 12) + pf_r32(p + 12));
        p = prev;
    }
    uint32_t next = pf_r32(p);
    if (next != l + PF_LIST_TAIL && p + pf_r32(p + 12) == next) {
        pf_list_rem_node(next);
        pf_w32(p + 12, pf_r32(p + 12) + pf_r32(next + 12));
    }
}

// `size` bytes from a MemList's free nodes, first fit in address order, from the bottom of
// the node. MEMTYPE_STARTPAGE starts the block on a page (align_unit), MEMTYPE_INPAGE keeps
// it inside one; what is left of the node goes back as one or two nodes.
static uint32_t alloc_from_memlist(uint32_t ml, uint32_t size, uint32_t flags) {
    uint32_t unit = align_unit(pf_r32(ml + ML_MEMHDR), flags), mask = unit - 1;
    bool inpage = flags & MEMTYPE_INPAGE, align = flags & (MEMTYPE_INPAGE | MEMTYPE_STARTPAGE);
    if (inpage && size > unit) return 0;
    uint32_t l = pf_r32(ml + ML_LIST);
    FOR_NODES(n, l) {
        uint32_t len = pf_r32(n + 12);
        if ((int32_t)len < (int32_t)size) continue;
        uint32_t end = n + len;
        if (!(n & mask) || !align || (inpage && (n & ~mask) == (end & ~mask))) {
            pf_list_rem_node(n);
            free_to_memlist(ml, n + size, len - size);
            return n;
        }
        uint32_t at = (n + mask) & ~mask;
        if ((int32_t)(end - (at + size)) < 0) continue;
        pf_w32(n + 12, at - n);
        free_to_memlist(ml, at + size, end - (at + size));
        return at;
    }
    return 0;
}

// ---- the lists' functions ---------------------------------------------------------------

// Kernel -28: void* AllocMemFromMemLists(List* l, int32 size, uint32 flags). The size is
// rounded up to 16. Each MemList of the kinds asked is tried; when its free nodes cannot
// serve, whole free pages go back to the system (ScavengeMem), pages come from
// AllocMemBlocks -- the task's for a caller in user mode, the OS's from the top otherwise --
// into the pool, and it is tried once more (once more again after SystemScavengeMem, the
// OS's own pools scavenged, in user mode). MEMTYPE_MYPOOL forbids the new pages, and once
// set here it stays set for the MemLists after. The block is filled with
// the flags' low byte under MEMTYPE_FILL; otherwise its first word is its length.
uint32_t pf_alloc_mem(uint32_t lists, int32_t size, uint32_t flags, bool user) {
    uint32_t len = (uint32_t)(size + 15) & ~15u, p = 0;
    if (!len) return 0;
    uint32_t k = flags & ((uint32_t)KINDS | MEMTYPE_DRAM);
    for (uint32_t ml = pf_r32(lists + PF_LIST_HEAD); ml != lists + PF_LIST_TAIL && !p; ml = pf_r32(ml)) {
        if ((pf_r32(ml + ML_TYPES) & k) != k) continue;
        for (int tries = 0;;) {
            if ((p = alloc_from_memlist(ml, len, flags))) break;
            if (tries == 1 && user) {                   // SystemScavengeMem, then again
                ++tries;
                pf_scavenge(false);
                continue;
            }
            if (flags & MEMTYPE_MYPOOL) break;
            ++tries;
            pf_scavenge(user);
            uint32_t b = alloc_blocks((int32_t)len, user ? flags | MEMTYPE_TASKMEM : flags);
            if (!b) break;
            pf_free_mem(lists, b, (int32_t)pf_r32(b));
            flags |= MEMTYPE_MYPOOL;
        }
    }
    if (p) {
        if (flags & MEMTYPE_FILL) {
            for (uint32_t i = 0; i < len; ++i) pf_w8(p + i, flags & 0xff);
        } else {
            pf_w32(p, len);
        }
    }
    return p;
}

// Kernel -32: void FreeMemToMemLists(List* l, void* p, int32 size) -- to the MemList whose
// memory holds p, the size rounded up to 16
void pf_free_mem(uint32_t lists, uint32_t p, int32_t size) {
    if (!p || !size) return;
    uint32_t ml = memlist_holding(lists, p);
    if (!ml || (p & 15)) {
        std::fflush(stdout);
        std::fprintf(stderr, "FreeMemToMemLists: %08X is no block of these lists\n", p);
        std::exit(3);
    }
    free_to_memlist(ml, p, (uint32_t)(size + 15) & ~15u);
}

// Kernel -44: int32 ScavengeMem() -- every whole page free in the DRAM and VRAM pools back to
// the system: the task's pools in user mode, the OS's otherwise (SystemScavengeMem). The
// kernel finds them by allocating page after page from the pool alone. 1 if any page went.
int32_t pf_scavenge(bool user) {
    static const uint32_t kinds[2] = {MEMTYPE_DRAM, MEMTYPE_VRAM | MEMTYPE_SYSTEMPAGESIZE};
    uint32_t lists = user ? pf_r32(pf_current_task() + T_FREEMEMORYLISTS) : pf_kernel_lists();
    int32_t any = 0;
    for (uint32_t kind : kinds) {
        uint32_t page = pf_page_size(kind);
        uint32_t f = kind | MEMTYPE_INPAGE | MEMTYPE_STARTPAGE | MEMTYPE_MYPOOL;
        while (uint32_t p = pf_alloc_mem(lists, (int32_t)page, f, user)) {
            any = 1;
            give_pages(lists, p, 4);
        }
    }
    return any;
}

static void k_allocmemfrommemlists(ArmCpu& c) { c.r[0] = pf_alloc_mem(c.r[0], (int32_t)c.r[1], c.r[2], true); }
static void k_freememtomemlists(ArmCpu& c) { pf_free_mem(c.r[0], c.r[1], (int32_t)c.r[2]); }
static void k_findmh(ArmCpu& c) { c.r[0] = pf_find_mh(c.r[0]); }
static void k_getpagesize(ArmCpu& c) { c.r[0] = pf_page_size(c.r[0]); }
static void k_allocmemblocks(ArmCpu& c) { c.r[0] = alloc_blocks((int32_t)c.r[0], c.r[1]); }
static void k_scavengemem(ArmCpu& c) { c.r[0] = (uint32_t)pf_scavenge(true); }

// ---- the boot ------------------------------------------------------------------------------
static uint32_t new_memhdr(const char* name, int pri, uint32_t types, uint32_t base, int bits_words,
                           int shift, int vram_shift) {
    uint32_t mh = pf_os_alloc(MH_SIZE), bits = pf_os_alloc(4u * bits_words);
    pf_w8(mh + 8, KERNELNODE);
    pf_w8(mh + 9, MEMHDRNODE);
    pf_w8(mh + 10, pri);
    pf_w8(mh + 11, 0x80);
    pf_w32(mh + 12, MH_SIZE);
    pf_w32(mh + 16, pf_os_string(name));
    pf_w32(mh + MH_TYPES, types);
    pf_w32(mh + MH_PAGESIZE, 1u << shift);
    pf_w32(mh + MH_PAGEMASK, (1u << shift) - 1);
    pf_w32(mh + MH_VRAMPAGESIZE, 1u << vram_shift);
    pf_w32(mh + MH_VRAMPAGEMASK, (1u << vram_shift) - 1);
    pf_w32(mh + MH_FREEPAGEBITS, bits);
    pf_w32(mh + MH_MEMBASE, base);
    pf_w32(mh + MH_MEMTOP, base + (32u * bits_words << shift));
    pf_w8(mh + MH_BITSSIZE, bits_words);
    pf_w8(mh + MH_PAGESHIFT, shift);
    pf_w8(mh + MH_VRAMPAGESHIFT, vram_shift);
    for (int i = 0; i < bits_words; ++i) pf_w32(bits + 4u * i, 0xFFFFFFFFu);   // every page free
    return mh;
}

static uint32_t new_memlist(uint32_t mh, const char* name, bool writable) {
    uint32_t ml = pf_os_alloc(ML_SIZE), l = pf_os_alloc(PF_LIST_SIZE);
    uint32_t words = pf_r8(mh + MH_BITSSIZE);
    pf_list_init(l, name);
    pf_w8(ml + 8, KERNELNODE);
    pf_w8(ml + 9, MEMLISTNODE);
    pf_w8(ml + 10, 100);
    pf_w8(ml + 11, 0x80);
    pf_w32(ml + 12, ML_SIZE);
    pf_w32(ml + 16, pf_os_string(name));
    pf_w32(ml + ML_TYPES, pf_r32(mh + MH_TYPES));
    pf_w32(ml + ML_OWNBITS, pf_os_alloc(4 * words));
    pf_w32(ml + ML_WRITEBITS, writable ? pf_os_alloc(4 * words) : 0);
    pf_w32(ml + ML_MEMHDR, mh);
    pf_w32(ml + ML_LIST, l);
    pf_w8(ml + ML_OWNBITSSIZE, words);
    return ml;
}

// Pages [from, to) the task's, as if it had allocated them.
static void claim(uint32_t ml, uint32_t from, uint32_t to) {
    uint32_t mh = pf_r32(ml + ML_MEMHDR), shift = pf_r8(mh + MH_PAGESHIFT);
    for (uint32_t pg = from >> shift; pg < (to + (1u << shift) - 1) >> shift; ++pg) {
        set_bit(pf_r32(mh + MH_FREEPAGEBITS), pg, false);
        set_bit(pf_r32(ml + ML_OWNBITS), pg, true);
        set_bit(pf_r32(ml + ML_WRITEBITS), pg, true);
    }
}

void pf_mem_init(const char* task_name, uint32_t image_end, uint32_t stack_base) {
    uint32_t kbase = pf_folio_base(PF_KERNEL);
    uint32_t hdrs = pf_os_alloc(PF_LIST_SIZE), frees = pf_os_alloc(PF_LIST_SIZE);
    pf_list_init(hdrs, "MemHdr");
    pf_list_init(frees, "MemFreeLists");
    pf_w32(kbase + KB_MEMHDRLIST, hdrs);
    pf_w32(kbase + KB_MEMFREELISTS, frees);

    // the kernel's: VRAM's MemHdr and MemList added at the tail, DRAM's by priority, ahead
    uint32_t vram = new_memhdr("VRAM memory", 100,
                               MEMTYPE_BANKSELECT | MEMTYPE_BANK1 | MEMTYPE_CEL | MEMTYPE_DMA | MEMTYPE_VRAM,
                               0x200000, 2, 14, 11);
    uint32_t dram = new_memhdr("DRAM memory", 101, MEMTYPE_DRAM | MEMTYPE_CEL | MEMTYPE_DMA, 0, 2, 15, 15);
    pf_list_add_tail(hdrs, vram);
    pf_list_insert_from_tail(hdrs, dram);
    pf_list_add_tail(frees, new_memlist(vram, "VRAM Free List", false));
    uint32_t dml = new_memlist(dram, "DRAM Free List", false);
    pf_w8(dml + 10, 101);
    pf_list_insert_from_tail(frees, dml);

    // the task: CreateTask's lists, DRAM's at the head
    uint32_t task = pf_os_alloc(PF_TASK_SIZE);
    pf_w32(task + 12, PF_TASK_SIZE);
    pf_item_new(task, KERNELNODE, TASKNODE, task_name);
    uint32_t tl = pf_os_alloc(PF_LIST_SIZE);
    pf_list_init(tl, "List of Free Mem Lists");
    pf_w32(task + T_FREEMEMORYLISTS, tl);
    pf_list_add_tail(tl, new_memlist(vram, "task vram ml", true));
    uint32_t tdram = new_memlist(dram, "task dram ml", true);
    pf_list_add_head(tl, tdram);
    pf_w32(task + T_STACKBASE, stack_base);
    pf_w32(task + T_STACKSIZE, DRAM_TOP - stack_base);
    pf_w32(kbase + KB_CURRENTTASK, task);

    // the program's memory: its pages, and what its last page holds past it free
    claim(tdram, 0, image_end);
    claim(tdram, stack_base, DRAM_TOP);
    uint32_t from = (image_end + 15) & ~15u, page = pf_r32(dram + MH_PAGESIZE);
    uint32_t to = (image_end + page - 1) & ~(page - 1);
    if (to > from && to <= stack_base) free_to_memlist(tdram, from, to - from);

    pf_on_slot(PF_KERNEL, -28, k_allocmemfrommemlists);
    pf_on_slot(PF_KERNEL, -32, k_freememtomemlists);
    pf_on_slot(PF_KERNEL, -44, k_scavengemem);
    pf_on_slot(PF_KERNEL, -60, k_getpagesize);
    pf_on_slot(PF_KERNEL, -100, k_findmh);
    pf_on_swi(0x1000d, k_allocmemblocks);
}
