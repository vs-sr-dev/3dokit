// 3dokit runtime -- pfboot: run a recompiled 3DO program on the Portfolio
// runtime, tracing its OS calls.
//
//     pfboot PROGRAM [--trace N] [--lenient] [--max-calls N] [--snap N DIR] [--disc DIR]
//                    [--frames DIR [--frames-at FIRST[-LAST][/EVERY]]] [--pad BUTTONS@FIELD[xN][/E][+H]]...
//                    [--window [--record FILE]]
//
// PROGRAM is the AIF file the build was recompiled from (its module must be
// in this build). --trace 0 is quiet, 1 (the default) every OS call, 2 also
// every access to the OS's memory. --lenient lets an OS call that is not
// implemented return 0 instead of stopping: a preview of what the program
// calls next, not a run to trust. --snap N DIR stops after the N-th OS call,
// with the memory before and after it and the call in DIR (pf_memtest.cpp;
// python -m 3dokit.pfcheck replays it on the 1993 OS). --disc DIR is the
// disc's root, where the program's files are; by default the program's own
// directory. --frames DIR writes what the display shows, at each vertical
// blank that changes it, as a PPM (pf_graphics.cpp); --frames-at FIRST[-LAST][/EVERY]
// looks only at the fields from FIRST to LAST, every EVERY-th of them (a long race
// writes some 230 KB a field). --pad BUTTONS@FIELD[xN][/E][+H]
// presses the first Control Pad's BUTTONS (up down left right a b c start x l r,
// joined by +) at field FIELD (gf_VBLNumber), N times (4 by default), every E
// fields (30, half a second), each held for H fields (6 by default) and then
// released; given +H and no xN it is one press (a@7600+600: A held from field
// 7600 to 8199). --pad may be given more than once (pf_event.cpp). --window
// (a pfboot built with SDL3) shows the display in a window, in real time, with
// the keyboard and a gamepad as the pad as well (pf_window.cpp); --record FILE
// writes the presses made there as --pad options that replay them.
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
#include <functional>
#include <vector>

#ifdef TDK_WINDOW
int pf_window_run(const char* title, const char* record, const std::function<int()>& run);   // pf_window.cpp
#endif

static uint32_t be32(const std::vector<uint8_t>& d, size_t o) {
    return (uint32_t)d[o] << 24 | (uint32_t)d[o + 1] << 16 | (uint32_t)d[o + 2] << 8 | d[o + 3];
}

// --pad BUTTONS@FIELD[xN][/E][+H]: the buttons' bits (event.h's ControlPadEventData), scheduled.
static bool pad_option(const char* spec) {
    std::string s = spec;
    size_t at = s.find('@');
    if (at == std::string::npos) return false;
    uint32_t bits = 0;
    for (size_t b = 0; b < at;) {
        size_t e = s.find('+', b);
        if (e == std::string::npos || e > at) e = at;
        std::string name = s.substr(b, e - b);
        bool known = false;
        for (uint32_t bit = 0x80000000u; bit; bit >>= 1)
            if (*pf_pad_button_name(bit) && name == pf_pad_button_name(bit)) { bits |= bit; known = true; }
        if (!known) return false;
        b = e + 1;
    }
    char* p;
    unsigned long long first = std::strtoull(s.c_str() + at + 1, &p, 10);
    long count = -1, every = 30, hold = 6;
    if (*p == 'x') count = std::strtol(p + 1, &p, 10);
    if (*p == '/') every = std::strtol(p + 1, &p, 10);
    if (*p == '+') {
        hold = std::strtol(p + 1, &p, 10);
        if (count < 0) count = 1;   // a hold is one press unless told otherwise
    }
    if (count < 0) count = 4;
    if (*p || !bits || count < 1 || hold < 1 || (count > 1 && every <= hold)) return false;
    pf_pad_press(bits, first, (int)count, (int)every, (int)hold);
    return true;
}

// --frames-at FIRST[-LAST][/EVERY]: the fields --frames looks at.
static bool frames_at_option(const char* spec) {
    char* p;
    unsigned long first = std::strtoul(spec, &p, 10), last = 0xFFFFFFFFul, every = 1;
    if (p == spec) return false;
    if (*p == '-') last = std::strtoul(p + 1, &p, 10);
    if (*p == '/') every = std::strtoul(p + 1, &p, 10);
    if (*p || last < first || every < 1) return false;
    g_pf_frames_first = (uint32_t)first;
    g_pf_frames_last = (uint32_t)last;
    g_pf_frames_every = (uint32_t)every;
    return true;
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
                             "                      [--frames DIR [--frames-at FIRST[-LAST][/EVERY]]]\n"
                             "                      [--pad BUTTONS@FIELD[xN][/E][+H]]... [--window [--record FILE]]\n"
                             "       pfboot PROGRAM --memtest DIR [--ops N] [--seed S]\n");
        return 2;
    }
    const char* memtest = nullptr;
    const char* disc = nullptr;
    const char* record = nullptr;
    bool window = false;
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
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) g_pf_frames_dir = argv[++i];
        else if (!std::strcmp(argv[i], "--frames-at") && i + 1 < argc) {
            if (!frames_at_option(argv[++i])) {
                std::fprintf(stderr, "--frames-at %s: FIRST[-LAST][/EVERY]\n", argv[i]);
                return 2;
            }
        }
        else if (!std::strcmp(argv[i], "--pad") && i + 1 < argc) {
            if (!pad_option(argv[++i])) {
                std::fprintf(stderr, "--pad %s: BUTTONS@FIELD[xN][/E][+H], BUTTONS of up down left right a b c start x l r\n", argv[i]);
                return 2;
            }
        }
        else if (!std::strcmp(argv[i], "--window")) window = true;
        else if (!std::strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
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
    if (window) {
#ifdef TDK_WINDOW
        return pf_window_run(argv[1], record, [&] { return pf_run(d.data(), stub, ro + rw + bss, entry); });
#else
        std::fprintf(stderr, "--window: this pfboot was built without SDL3\n");
        return 2;
#endif
    }
    if (record) {
        std::fprintf(stderr, "--record: only with --window\n");
        return 2;
    }
    return pf_run(d.data(), stub, ro + rw + bss, entry);
}
