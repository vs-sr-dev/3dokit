// 3dokit runtime -- the Operator's "ram" device, and the NVRAM behind its unit 3.
//
// The device is the Operator's (Doctor Hauzer's 20.18, os_code's second image, linked at 0x20000;
// the 1993 and 23.10 Operators make one of the same name). Its driver has three commands (its
// table at 0x26a38: CMD_WRITE 0x21278, CMD_READ 0x21020, CMD_STATUS 0x2140c) and the device six
// units, each a span of the console's address space in blocks of one byte, set up when it is made
// (0x20e20) in a table of 16 bytes a unit at 0x269d0 (block count, bytes, base, block size): 0 a
// span of memory at 0x03701000 (0x2f000 bytes, none on some machines), 1 the ROM from 0x03028000
// (0xd8000), 2 the whole ROM (0x100000), 3 the NVRAM, 0x8000 bytes at 0x03140000, 4 a second ROM
// (0x200000 when there is one), 5 the ROM again. Only unit 3 is here: the others are the console's
// ROM and memory, which the runtime does not have, and a request for one stops the run.
//
// The NVRAM is battery-backed memory with a byte at each word (the driver moves it a byte a word,
// 0x211a4 and 0x2135c). Here it is 32 KB that outlive each program's boot -- the shell's programs
// each boot a fresh OS, and the NVRAM is the console's, not the OS's: blank (zeros) unless pfboot
// --nvram DIR names a directory, where it is the file nvram.bin, read at its first use and written
// again after every write to it: the device's 32,768 bytes in order.
#include "pf.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

enum : uint32_t {
    IO_CALLBACK = 0x30, IO_ACTUAL = 0x54, IO_ERROR = 0x5c,
    IOI_COMMAND = 0x34, IOI_UNIT = 0x36, IOI_OFFSET = 0x40, IOI_SEND_BUF = 0x44, IOI_SEND_LEN = 0x48,
    IOI_RECV_BUF = 0x4c, IOI_RECV_LEN = 0x50,
    CMD_WRITE = 0, CMD_READ = 1, CMD_STATUS = 2,
    KERR_NOTPRIV = 0xD57B9004u, KERR_BADCOMMAND = 0xD57B900Cu, KERR_BADIOARG = 0xD57B900Du,
    TASK_SUPER = 8,
    NVRAM_UNIT = 3, NVRAM_BYTES = 0x8000, NVRAM_ADDR = 0x03140000u,
    DS_USAGE_FILESYSTEM = 0x80000000u,
};

std::vector<uint8_t> g_nvram;
bool g_loaded;
std::string g_dir;

std::string file_of() { return g_dir + "/nvram.bin"; }

}  // namespace

void pf_nvram_dir(const char* dir) {
    g_dir = dir;
    g_loaded = false;
}

std::vector<uint8_t>& pf_nvram() {
    if (!g_loaded) {
        g_loaded = true;
        g_nvram.assign(NVRAM_BYTES, 0);
        if (!g_dir.empty()) {
            std::ifstream f(file_of(), std::ios::binary);
            std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            if (d.size() == NVRAM_BYTES) g_nvram = d;
            else if (!d.empty()) {
                std::fprintf(stderr, "%s: %zu bytes, not the NVRAM's %u\n", file_of().c_str(), d.size(), NVRAM_BYTES);
                std::exit(2);
            }
        }
    }
    return g_nvram;
}

void pf_nvram_written() {
    if (g_dir.empty()) return;
    std::ofstream f(file_of(), std::ios::binary);
    f.write((const char*)g_nvram.data(), (std::streamsize)g_nvram.size());
    if (!f) {
        std::fprintf(stderr, "%s: cannot write it\n", file_of().c_str());
        std::exit(2);
    }
}

namespace {

int32_t refuse(uint32_t ior, uint32_t err) {
    pf_w32(ior + IO_ERROR, err);
    return 1;
}

// The kernel's dispatch (0x1468c) refuses a command past the driver's three (BADCOMMAND); a unit
// other than the NVRAM stops the run. Each command is done at once.
//
// CMD_WRITE (0x21278): to the NVRAM only from a privileged task or by a request with a callback
// (the OS's own; else NOTPRIV); ioi_Offset in blocks, not negative, and ioi_Send's length not
// negative and inside the unit (each else BADIOARG); nothing for no bytes; the bytes copied. For
// the NVRAM io_Actual is left as it was (the driver sets it for unit 0 only).
//
// CMD_READ (0x21020): the same checks of ioi_Offset and ioi_Recv's length; the bytes copied,
// io_Actual their count.
//
// CMD_STATUS (0x2140c): a receive buffer of at least 8 bytes (else BADIOARG); a DeviceStatus of
// 0x28 bytes on the driver's stack -- ds_DriverIdentity 3, ds_MaximumStatusSize 0x24, the block
// size and count, ds_DeviceUsageFlags DS_USAGE_FILESYSTEM (and read-only for units 1, 2, 4 and
// 5; unit 2 has neither), the unit's base at +0x24 -- copied for as much of it as the buffer
// takes, io_Actual that much. Its words at +0x1c and +0x20 the driver never writes: whatever its
// stack held, here 0.
int32_t ram_dispatch(uint32_t ior) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND), unit = pf_r8(ior + IOI_UNIT);
    if (cmd > CMD_STATUS) return refuse(ior, KERR_BADCOMMAND);
    if (unit != NVRAM_UNIT) {
        std::fprintf(stderr, "ram: command %u on unit %u (the console's ROM or memory): not yet\n", cmd, unit);
        std::exit(3);
    }
    std::vector<uint8_t>& nv = pf_nvram();
    if (cmd == CMD_STATUS) {
        uint32_t buf = pf_r32(ior + IOI_RECV_BUF);
        int32_t len = (int32_t)pf_r32(ior + IOI_RECV_LEN);
        if (len < 8) return refuse(ior, KERR_BADIOARG);
        uint8_t st[0x28] = {3, 0, 0, 0};
        auto put = [&](uint32_t at, uint32_t v) {
            for (int i = 0; i < 4; ++i) st[at + (uint32_t)i] = (uint8_t)(v >> (24 - 8 * i));
        };
        put(0x04, 0x24);
        put(0x08, 1);
        put(0x0c, NVRAM_BYTES);
        put(0x14, DS_USAGE_FILESYSTEM);
        put(0x24, NVRAM_ADDR);
        uint32_t n = len > 0x28 ? 0x28u : (uint32_t)len;
        for (uint32_t i = 0; i < n; ++i) pf_w8(buf + i, st[i]);
        pf_w32(ior + IO_ACTUAL, n);
        return 1;
    }
    if (cmd == CMD_WRITE && !(pf_r8(pf_current_task() + 11) & TASK_SUPER) && !pf_r32(ior + IO_CALLBACK))
        return refuse(ior, KERR_NOTPRIV);
    int32_t offset = (int32_t)pf_r32(ior + IOI_OFFSET);
    uint32_t buf = pf_r32(ior + (cmd == CMD_WRITE ? IOI_SEND_BUF : IOI_RECV_BUF));
    int32_t len = (int32_t)pf_r32(ior + (cmd == CMD_WRITE ? IOI_SEND_LEN : IOI_RECV_LEN));
    if (offset < 0) return refuse(ior, KERR_BADIOARG);
    if (len == 0) return 1;
    if (len < 0 || (int32_t)((uint32_t)offset + (uint32_t)len) > (int32_t)NVRAM_BYTES) return refuse(ior, KERR_BADIOARG);
    if (cmd == CMD_WRITE) {
        for (int32_t i = 0; i < len; ++i) nv[(uint32_t)(offset + i)] = (uint8_t)pf_r8(buf + (uint32_t)i);
        pf_nvram_written();
    } else {
        for (int32_t i = 0; i < len; ++i) pf_w8(buf + (uint32_t)i, nv[(uint32_t)(offset + i)]);
        pf_w32(ior + IO_ACTUAL, (uint32_t)len);
    }
    if (g_pf_trace >= 2)
        pf_log("        ram: %s %d bytes at %d of the NVRAM\n", cmd == CMD_WRITE ? "wrote" : "read", len, offset);
    return 1;
}

}  // namespace

// The device, made with the Operator's others (0x214f0: its driver, then the device, units 0 to 5).
static uint32_t g_ram;

void pf_ram_init() { g_ram = pf_device_new("ram", 5, ram_dispatch); }

uint32_t pf_ram_device() { return g_ram; }
