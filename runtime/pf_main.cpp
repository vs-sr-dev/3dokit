// 3dokit runtime -- pfboot: run a recompiled 3DO program on the Portfolio
// runtime, tracing its OS calls.
//
//     pfboot PROGRAM [--trace N] [--lenient] [--max-calls N] [--snap N DIR] [--disc DIR]
//
// PROGRAM is the AIF file the build was recompiled from (its module must be
// in this build). --trace 0 is quiet, 1 (the default) every OS call, 2 also
// every access to the OS's memory. --lenient lets an OS call that is not
// implemented return 0 instead of stopping: a preview of what the program
// calls next, not a run to trust. --snap N DIR stops after the N-th OS call,
// with the memory before and after it and the call in DIR (pf_memtest.cpp;
// python -m 3dokit.pfcheck replays it on the 1993 OS). --disc DIR is the
// disc's root, where the program's files are; by default the program's own
// directory.
//
//     pfboot PROGRAM --memtest DIR [--ops N] [--seed S]
//
// boots the same way but runs the allocator's test instead of the program
// (pf_memtest.cpp; python -m 3dokit.pfcheck replays it on the 1993 kernel).
#include "pf.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

static uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
}

// The target of the BL at `at` in the AIF header, or 0.
static uint32_t bl_target(const std::vector<uint8_t>& d, uint32_t at) {
    uint32_t w = be32(d, at);
    if (w >> 24 != 0xEB) return 0;
    int32_t off = (int32_t)(w << 8) >> 8;
    return at + 8 + (uint32_t)off * 4;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: pfboot PROGRAM [--trace N] [--lenient] [--max-calls N] [--snap N DIR] [--disc DIR]\n"
                             "       pfboot PROGRAM --memtest DIR [--ops N] [--seed S]\n");
        return 2;
    }
    const char* memtest = nullptr;
    const char* disc = nullptr;
    int ops = 2000;
    uint32_t seed = 1;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--trace") && i + 1 < argc) g_pf_trace = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--max-calls") && i + 1 < argc) g_pf_max_calls = std::strtoull(argv[++i], nullptr, 0);
        else if (!std::strcmp(argv[i], "--lenient")) g_pf_lenient = true;
        else if (!std::strcmp(argv[i], "--snap") && i + 2 < argc) {
            g_pf_snap_call = std::strtoull(argv[++i], nullptr, 0);
            g_pf_snap_dir = argv[++i];
        }
        else if (!std::strcmp(argv[i], "--disc") && i + 1 < argc) disc = argv[++i];
        else if (!std::strcmp(argv[i], "--memtest") && i + 1 < argc) memtest = argv[++i];
        else if (!std::strcmp(argv[i], "--ops") && i + 1 < argc) ops = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = (uint32_t)std::strtoul(argv[++i], nullptr, 0);
    }
    if (disc) g_pf_disc_root = disc;
    else {
        std::string prog = argv[1];
        size_t slash = prog.find_last_of("/\\");
        g_pf_disc_root = slash == std::string::npos ? "." : prog.substr(0, slash);
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 0x100 || be32(d, 0x10) != 0xEF000011u) {
        std::fprintf(stderr, "%s: not an AIF image\n", argv[1]);
        return 2;
    }
    uint32_t ro = be32(d, 0x14), rw = be32(d, 0x18), bss = be32(d, 0x20);
    uint32_t stub = bl_target(d, 0x04);
    if (!stub) stub = ro + rw;
    uint32_t entry = bl_target(d, 0x0C);
    // the image to its relocation stub (aif.py: the stub can sit past ro + rw),
    // then the zero-initialised data
    if (memtest) {
        if (int bad = pf_boot(d.data(), stub, ro + rw + bss)) return bad;
        return pf_memtest(memtest, ops, seed);
    }
    return pf_run(d.data(), stub, ro + rw + bss, entry);
}
