// 3dokit runtime -- tests of the runtime against the OS it reimplements: the allocator's, and a
// snapshot of any one OS call.
//
// After the boot, a random run of the memory calls a program and the OS make -- allocations
// of every kind and alignment, in user mode from the task's lists and in supervisor mode from
// the OS's, frees of what is live, ScavengeMem -- is made here and written down with the
// guest memory before and after. `python -m 3dokit.pfcheck` replays the same run on the 1993
// kernel's own code (in 3dokit.armemu, over the memory before) and compares every result and
// the memory after, byte for byte.
#include "pf.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static uint32_t g_rng;

static uint32_t rnd(uint32_t n) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng % n;
}

static bool dump(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return false;
    std::fwrite(g_arm_mem, 1, ARM_MEM_SIZE, f);
    std::fwrite(pf_os_memory(), 1, PF_OS_SIZE, f);
    return std::fclose(f) == 0;
}

int pf_memtest(const char* dir, int ops, uint32_t seed) {
    g_rng = seed ? seed : 1;
    std::string d = dir;
    if (!dump(d + "/before.bin")) {
        std::fprintf(stderr, "cannot write %s/before.bin\n", dir);
        return 2;
    }
    FILE* f = std::fopen((d + "/ops.txt").c_str(), "w");
    if (!f) return 2;
    struct Block { uint32_t lists, p; int32_t size; bool user; };
    std::vector<Block> live;
    static const uint32_t kinds[] = {
        0, MEMTYPE_DRAM, MEMTYPE_VRAM, MEMTYPE_CEL, MEMTYPE_DMA | MEMTYPE_CEL, MEMTYPE_VRAM | MEMTYPE_CEL,
        MEMTYPE_VRAM | MEMTYPE_STARTPAGE, MEMTYPE_DRAM | MEMTYPE_STARTPAGE, MEMTYPE_VRAM | MEMTYPE_INPAGE,
        MEMTYPE_DRAM | MEMTYPE_INPAGE, MEMTYPE_VRAM | MEMTYPE_STARTPAGE | MEMTYPE_SYSTEMPAGESIZE,
        MEMTYPE_FILL | 0x5a, MEMTYPE_VRAM | MEMTYPE_FILL, MEMTYPE_MYPOOL,
        MEMTYPE_VRAM | MEMTYPE_BANKSELECT | MEMTYPE_BANK1, MEMTYPE_VRAM | MEMTYPE_BANKSELECT | MEMTYPE_BANK2,
    };
    uint32_t task = pf_r32(pf_current_task() + T_FREEMEMORYLISTS), os = pf_kernel_lists();
    int allocs = 0, frees = 0, scav = 0;
    for (int i = 0; i < ops; ++i) {
        uint32_t r = rnd(100);
        if (r < 50 || live.empty()) {
            bool user = rnd(8) != 0;
            uint32_t flags = kinds[rnd(sizeof kinds / sizeof *kinds)];
            int32_t size;
            switch (rnd(4)) {
            case 0: size = 1 + (int32_t)rnd(64); break;
            case 1: size = 1 + (int32_t)rnd(4096); break;
            case 2: size = 1 + (int32_t)rnd(40000); break;
            default: size = 1 + (int32_t)rnd(200000); break;
            }
            uint32_t lists = user ? task : os;
            uint32_t p = pf_alloc_mem(lists, size, flags, user);
            std::fprintf(f, "alloc %c %08X %d %08X -> %08X\n", user ? 'U' : 'K', lists, size, flags, p);
            if (p) live.push_back({lists, p, size, user});
            ++allocs;
        } else if (r < 92) {
            size_t k = rnd((uint32_t)live.size());
            Block b = live[k];
            live.erase(live.begin() + (long)k);
            pf_free_mem(b.lists, b.p, b.size);
            std::fprintf(f, "free %c %08X %08X %d\n", b.user ? 'U' : 'K', b.lists, b.p, b.size);
            ++frees;
        } else {
            bool user = rnd(4) != 0;
            int32_t any = pf_scavenge(user);
            std::fprintf(f, "scavenge %c -> %d\n", user ? 'U' : 'K', any);
            ++scav;
        }
    }
    std::fclose(f);
    if (!dump(d + "/after.bin")) return 2;
    std::printf("memtest: %d allocations, %d frees, %d scavenges, %zu blocks live, in %s\n",
                allocs, frees, scav, live.size(), dir);
    return 0;
}

// ---- one call ------------------------------------------------------------------------------
unsigned long long g_pf_snap_call;
const char* g_pf_snap_dir;

static FILE* snap_file(const char* name, const char* mode) {
    std::string path = std::string(g_pf_snap_dir) + "/" + name;
    FILE* f = std::fopen(path.c_str(), mode);
    if (!f) {
        std::fprintf(stderr, "cannot write %s\n", path.c_str());
        std::exit(2);
    }
    return f;
}

void pf_snap_before(const ArmCpu& c, const char* call) {
    if (!dump(std::string(g_pf_snap_dir) + "/before.bin")) {
        std::fprintf(stderr, "cannot write %s/before.bin\n", g_pf_snap_dir);
        std::exit(2);
    }
    FILE* f = snap_file("call.txt", "w");
    std::fprintf(f, "call %s\n", call);
    std::fprintf(f, "regs");
    for (int i = 0; i < 16; ++i) std::fprintf(f, " %08X", c.r[i]);
    uint32_t task = pf_current_task();
    std::fprintf(f, "\ntask %08X %d\nosnext %08X\n", task, (int)pf_r32(task + 24), pf_os_next());
    for (int32_t i = 1; i < pf_item_count(); ++i) std::fprintf(f, "item %d %08X\n", (int)i, pf_item_node(i));
    std::fclose(f);
}

void pf_snap_after(const ArmCpu& c) {
    if (!dump(std::string(g_pf_snap_dir) + "/after.bin")) {
        std::fprintf(stderr, "cannot write %s/after.bin\n", g_pf_snap_dir);
        std::exit(2);
    }
    FILE* f = snap_file("call.txt", "a");
    std::fprintf(f, "result %08X\n", c.r[0]);
    std::fclose(f);
    pf_log("snapshot of OS call %llu in %s\n", g_pf_snap_call, g_pf_snap_dir);
    std::exit(0);
}
