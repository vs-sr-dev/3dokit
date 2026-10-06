// 3dokit runtime -- the recompiler's self-test: recompiled functions run on
// the vectors 3dokit/recomp/selftest.py recorded with the interpreter
// (armemu), and every result compared.
//
//     selftest VECTORS.txt ...
//
// A vectors file is lines of:
//     image BASE PATH        load a file into guest memory at BASE (hex)
//     module NAME            activate that module (its image must be in memory)
//     func ENTRY LABEL       the function the next vectors call
//     v IN... OUT...         16 hex words each: r0-r14, cpsr
//     mem CRC                crc32 of guest memory (3 MB) after the function's vectors
// Memory is shared by all vectors in order, as in the interpreter's run.
#include "arm60.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static const uint32_t kSentinel = 0xFFFFFFF0u;     // armemu.RETURN_SENTINEL
static const char* kNames[16] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "sb",
                                 "sl", "fp", "ip", "sp", "lr", "cpsr"};

// The stub services end the run on an OS call or a bad access: say which
// vector it was.
static std::string g_where;
static void where() {
    if (!g_where.empty()) std::fprintf(stderr, "  during %s\n", g_where.c_str());
}

static void put(ArmCpu& c, const uint32_t* w) {
    for (int i = 0; i < 15; ++i) c.r[i] = w[i];
    arm_set_flags(c, w[15]);
}

static void get(const ArmCpu& c, uint32_t* w) {
    for (int i = 0; i < 15; ++i) w[i] = c.r[i];
    w[15] = arm_cpsr(c);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: selftest VECTORS.txt ...\n");
        return 2;
    }
    std::atexit(where);
    long total_funcs = 0, total_vec = 0, total_bad = 0;
    for (int fi = 1; fi < argc; ++fi) {
        std::ifstream in(argv[fi]);
        if (!in) { std::fprintf(stderr, "cannot read %s\n", argv[fi]); return 2; }
        std::string line, label;
        uint32_t entry = 0;
        long funcs = 0, vec = 0, bad = 0, fbad = 0, fvec = 0;
        std::memset(g_arm_mem, 0, ARM_MEM_SIZE);
        while (std::getline(in, line)) {
            std::istringstream ss(line);
            std::string kw;
            ss >> kw;
            if (kw.empty() || kw[0] == '#') continue;
            if (kw == "image") {
                std::string base, path;
                ss >> base >> std::ws;
                std::getline(ss, path);
                std::ifstream img(path, std::ios::binary);
                std::vector<char> data((std::istreambuf_iterator<char>(img)), std::istreambuf_iterator<char>());
                uint32_t b = (uint32_t)std::stoul(base, nullptr, 16);
                if (data.empty() || b + data.size() > ARM_MEM_SIZE) {
                    std::fprintf(stderr, "cannot load %s at %s\n", path.c_str(), base.c_str());
                    return 2;
                }
                std::memcpy(g_arm_mem + b, data.data(), data.size());
            } else if (kw == "module") {
                std::string name;
                ss >> name;
                const ArmModule* m = arm_module(name.c_str());
                if (!m) { std::fprintf(stderr, "no module %s in this build\n", name.c_str()); return 2; }
                if (arm_identify() != m) {
                    std::fprintf(stderr, "the image in memory is not module %s\n", name.c_str());
                    return 2;
                }
                arm_activate(m);
            } else if (kw == "func") {
                std::string e;
                ss >> e >> std::ws;
                std::getline(ss, label);
                entry = (uint32_t)std::stoul(e, nullptr, 16);
                ++funcs; fbad = 0; fvec = 0;
            } else if (kw == "v") {
                uint32_t w[32], got[16];
                for (auto& x : w) { std::string h; ss >> h; x = (uint32_t)std::stoul(h, nullptr, 16); }
                ArmCpu c{};
                put(c, w);
                c.r[14] = kSentinel; c.r[15] = entry; c.pc = 0; c.budget = 1 << 20;
                g_where = std::string(argv[fi]) + ": " + label + ", vector " + std::to_string(fvec + 1);
                arm_call(c, entry);
                g_where.clear();
                get(c, got);
                ++vec; ++fvec;
                bool ok = c.pc == kSentinel && !std::memcmp(got, w + 16, sizeof got);
                if (!ok) {
                    ++bad;
                    if (fbad++ < 3) {
                        std::printf("FAIL %08X %s, vector %ld:", entry, label.c_str(), fvec);
                        if (c.pc != kSentinel) std::printf(" returned to %08X", c.pc);
                        for (int i = 0; i < 16; ++i)
                            if (got[i] != w[16 + i]) std::printf(" %s=%08X (want %08X)", kNames[i], got[i], w[16 + i]);
                        std::printf("\n      in:");
                        for (int i = 0; i < 16; ++i) std::printf(" %08X", w[i]);
                        std::printf("\n");
                    }
                }
            } else if (kw == "mem") {
                std::string h;
                ss >> h;
                uint32_t want = (uint32_t)std::stoul(h, nullptr, 16);
                uint32_t crc = arm_crc32(g_arm_mem, ARM_MEM_SIZE);
                if (crc != want) {
                    ++bad;
                    std::printf("FAIL %08X %s: memory differs after its vectors\n", entry, label.c_str());
                    break;      // the rest of the file starts from the interpreter's memory
                }
            }
        }
        std::printf("%s: %ld functions, %ld vectors, %ld failures\n", argv[fi], funcs, vec, bad);
        total_funcs += funcs; total_vec += vec; total_bad += bad;
    }
    if (argc > 2)
        std::printf("total: %ld functions, %ld vectors, %ld failures\n", total_funcs, total_vec, total_bad);
    return total_bad ? 1 : 0;
}
