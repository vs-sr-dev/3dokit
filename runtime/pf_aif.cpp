// 3dokit runtime -- a compressed AIF image unpacked by native code.
//
// Every compressed image of the System trees read so far (28 of them on three discs: AUDIOFOLIO,
// GRAPHIX, eventbroker, shell, os_code and misc_code, from 1993 to 23.10) ends in the same
// decompressor, 0x188 bytes that the BL at the image's 0x00 calls (at 0x5c90 in Doctor Hauzer's
// GRAPHIX 20.45, whose addresses the comments give). aif.py runs that code in armemu; this is its
// transliteration, so that the runtime can lay a folio's own data in guest memory. It does what
// the code does, in the order it does it, on the same bytes in the same places:
//
//   0x5c90  b over four words: +0x04 (1 on every image), +0x08 the decompressor's offset from the
//           base, which is where the packed data stops; +0x0c where the unpacked words end,
//           0x100 + 4 x their count (not read); +0x10 how far beyond the decompressor the data
//           is moved
//   0x5ca4  the decompressor's code from 0x5cec copied to base + [+0x08] + [+0x10] and run there
//   0x5cf4  the packed data (0x100 up to the decompressor) moved up, a word at a time from its
//           end, to end where the copied code begins
//   0x5d10  its first word: how many words to unpack; then the table (0x5d50) and the words
//           (0x5da0), unpacked to base + 0x100 -- over the header's end and the moved data's start
//   0x5d3c  the NOP into the BL's place at 0x00, and back to the header at 0x04
//
// The 3DO's memory is big-endian, so a word stored by the code is its four bytes in the order
// they were made; the buffer here holds them as the guest's memory would.
#include "pf.h"

#include <cstring>

namespace {

uint32_t be32(const std::vector<uint8_t>& d, size_t at) {
    return (uint32_t)d[at] << 24 | (uint32_t)d[at + 1] << 16 | (uint32_t)d[at + 2] << 8 | d[at + 3];
}

void put32(std::vector<uint8_t>& d, size_t at, uint32_t v) {
    d[at] = (uint8_t)(v >> 24);
    d[at + 1] = (uint8_t)(v >> 16);
    d[at + 2] = (uint8_t)(v >> 8);
    d[at + 3] = (uint8_t)v;
}

uint32_t crc32(const uint8_t* p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; ++k) c = c >> 1 ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

enum : uint32_t {
    NOP = 0xE1A00000u,
    EXIT = 0xEF000011u,
    DECOMP_SIZE = 0x188,            // 0x5c90..0x5e18
    DECOMP_COPIED = 0x5c,           // the code from 0x5cec on is what is copied and run
    DECOMP_CRC = 0xCB71152Cu,       // CRC-32 of the 0x188 bytes, the four words at +4..+0x14 as 0
    TABLE_RECORDS = 0x60,           // 0x5d14: 0x60 records of two words, 0x300 bytes
};

}  // namespace

std::string pf_aif_unpack(const std::vector<uint8_t>& file, std::vector<uint8_t>& out) {
    // the image itself, or what follows os_code's 16-byte boot header (aif.py's unwrap)
    size_t skip = 0;
    if (!(file.size() >= 0x100 && be32(file, 0x10) == EXIT) && file.size() >= 0x110 && be32(file, 0x20) == EXIT)
        skip = 16;
    if (file.size() < skip + 0x100 || be32(file, skip + 0x10) != EXIT) return "not an AIF image";
    std::vector<uint8_t> mem(file.begin() + (ptrdiff_t)skip, file.end());
    uint32_t w0 = be32(mem, 0);
    if (w0 == NOP) return "not compressed";
    if (w0 >> 24 != 0xEB) return "word 0 is neither a NOP nor a BL";
    uint32_t dec = 8 + (uint32_t)((int32_t)(w0 << 8) >> 8) * 4;     // the BL's target
    if (dec < 0x100 || dec + DECOMP_SIZE > mem.size()) return "the decompressor lies outside the file";
    {
        uint8_t code[DECOMP_SIZE];
        std::memcpy(code, mem.data() + dec, DECOMP_SIZE);
        std::memset(code + 4, 0, 16);
        if (crc32(code, DECOMP_SIZE) != DECOMP_CRC) return "a decompressor the runtime has not read";
    }
    uint32_t packed_end = be32(mem, dec + 0x08), gap = be32(mem, dec + 0x10);
    // 0x5ce0: the words moved are [+0x08] - 0x100 bytes ending at the decompressor itself (0x5cc0)
    if (packed_end < 0x104 || packed_end > dec || (packed_end & 3) || (gap & 3) || gap > 0x1000000)
        return "its parameters are not an image's";
    uint32_t moved = packed_end - 0x100;
    uint32_t code_at = packed_end + gap;                            // 0x5cac..0x5cb8
    if (code_at < dec) return "its parameters are not an image's";
    // 0x5cd0 copies upwards: a destination inside what is still to be copied repeats the code's
    // first (code_at - dec - 0x5c) bytes over the rest, and what runs there is not the decoder.
    // Immercenary's ja.language (+0x10 = 0xcc) is such an image; armemu runs off memory on it.
    if (code_at > dec + DECOMP_COPIED && code_at < dec + DECOMP_SIZE)
        return "the decompressor's copy of itself overwrites itself: its code would not unpack it";
    if (mem.size() < code_at + (DECOMP_SIZE - DECOMP_COPIED)) mem.resize(code_at + (DECOMP_SIZE - DECOMP_COPIED));
    // 0x5cd0: the code copied, a word at a time, from 0x5cec to 0x5e18
    for (uint32_t i = DECOMP_COPIED; i < DECOMP_SIZE; i += 4) put32(mem, code_at + i - DECOMP_COPIED, be32(mem, dec + i));
    // 0x5cf8: the data moved down from its last word, to end below the copied code
    for (uint32_t src = dec - 4, dst = code_at - 4, n = moved; n; n -= 4, src -= 4, dst -= 4)
        put32(mem, dst, be32(mem, src));
    uint32_t r0 = code_at - moved;                                  // 0x5d08
    uint32_t words = be32(mem, r0);                                 // 0x5d10
    r0 += 4;
    // the end of the copied code is as far as the words could be unpacked before they overran it
    if (words > (code_at - 0x100) / 4) return "more words than the space below the decompressor";
    // 0x5d50: the table, 0x300 bytes on the stack, each byte from the data or 0 by a flag bit
    uint8_t table[TABLE_RECORDS * 8];
    uint32_t t = 0;
    for (uint32_t rec = 0; rec < TABLE_RECORDS; ++rec) {
        if (r0 >= mem.size()) return "the table runs off the data";
        uint32_t flags = mem[r0++];                                 // 0x5d58
        for (int b = 0; b < 8; ++b, flags >>= 1) {                  // 0x5d60, 0x5d70: lsr #1, carry clear: a byte
            if (flags & 1) table[t++] = 0;
            else {
                if (r0 >= mem.size()) return "the table runs off the data";
                table[t++] = mem[r0++];
            }
        }
    }
    // 0x5da0: each word four bytes; a byte's 2-bit code is 0 for a byte of the data, else the
    // byte in the code's row of the table (256 bytes a row) at the byte before it -- across the
    // words, from 0 at the start (ip)
    uint32_t r1 = r0, r3 = 0x100, prev = 0;
    for (uint32_t n = words; n; --n) {
        if (r1 >= mem.size()) return "the words run off the data";
        uint32_t ctl = mem[r1++], w = 0;                            // 0x5dac
        for (int b = 0; b < 4; ++b) {
            uint32_t code = ctl >> (2 * b) & 3, v;
            if (code) v = table[(code - 1) << 8 | prev];            // 0x5db4..: ldrneb [r0 - 0x100 + ip]
            else {
                if (r1 >= mem.size()) return "the words run off the data";
                v = mem[r1++];                                      // ldreqb [r1], #1
            }
            w = w << 8 | v;
            prev = v;
        }
        put32(mem, r3, w);                                          // 0x5e08
        r3 += 4;
    }
    put32(mem, 0, NOP);                                             // 0x5d3c
    mem.resize(0x100 + (size_t)words * 4);
    out.swap(mem);
    return "";
}
