// 3dokit runtime -- the kernel's error texts: GetSysErr, over the ErrorText items the folios make.
//
// An Err (operror.h) is 1 in its top bit, then an object's id -- three characters of six bits,
// 25-30, 19-24, 13-18 --, a severity (11-12), an environment (9-10), a bit for an extended error
// (8) and the error's number (0-7). GetSysErr writes it as text: the three characters, the
// severity's, environment's and kind's names, then the kernel's own text for a standard error,
// or the text an ErrorText item of that object gives for an extended one (a folio makes one at
// its start: CreateItem of an ERRORTEXTNODE, with its object's id, its count and a table of
// strings). Here the kernel's tables and strings and the folios' tables are read from the disc's
// own images (unpacked by pf_aif), never written into the runtime: the kernel's at the addresses
// of its version (kKernels), each folio's tag list at the addresses of its own (kErrorTexts).
// The addresses in the comments are Doctor Hauzer's kernel 20.21.
#include "pf.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t be32(const std::vector<uint8_t>& d, size_t a) {
    return a + 4 <= d.size() ? (uint32_t)d[a] << 24 | (uint32_t)d[a + 1] << 16 | (uint32_t)d[a + 2] << 8 | d[a + 3] : 0;
}

// One of os_code's images, unpacked, from its base.
struct Image {
    uint32_t base = 0, version = 0;
    std::vector<uint8_t> bytes;
    uint32_t word(uint32_t a) const { return be32(bytes, a - base); }
    std::string str(uint32_t a) const {
        std::string s;
        for (uint32_t o = a - base; o < bytes.size() && bytes[o]; ++o) s += (char)bytes[o];
        return s;
    }
};

// The kernel's GetSysErr: its tables (20.21: 0x1aacc).
struct KernelErr {
    uint32_t version;
    uint32_t chars;                 // 64 characters, an id's six bits each
    uint32_t severities, environments, kinds;   // 4, 4 and 2 pointers to names
    uint32_t standard, nstandard;   // the standard errors' texts
    uint32_t minus1, no_error, no_errornum, open, close;     // its own strings, in its code
};
const KernelErr kKernels[] = {
    {PF_VERSION(20, 21), 0x1b9b8, 0x1b9f8, 0x1ba08, 0x1ba18, 0x1ba20, 0x13, 0x1ab28, 0x1ab5c, 0x1ab84, 0x1ad6c, 0x1ad74},
};

// A folio's ErrorText, made at its start: its image in os_code (by its version) and the tag list
// it gives CreateItem -- the File folio 20.30's at 0xf8c, made at 0x9d0.
struct FolioErr {
    uint32_t version, tags;
};
const FolioErr kErrorTexts[] = {
    {PF_VERSION(20, 30), 0xf8c},
};
enum : uint32_t { ERRTEXT_TAG_OBJID = 10, ERRTEXT_TAG_MAXERR = 11, ERRTEXT_TAG_TABLE = 12 };

struct ErrorText {
    uint32_t id, count;
    std::vector<std::string> texts;
};

struct Errs {
    std::string root;
    const KernelErr* k = nullptr;
    Image kernel;
    std::vector<ErrorText> texts;   // in the order the kernel's list has them
};
Errs g_errs;

// os_code's images, unpacked: each AIF header in the file (after the 16-byte boot header, SWI
// 0x11 at +0x10 and a BL at +0x0c), to the next.
std::vector<Image> os_images() {
    std::vector<Image> out;
    std::string host = pf_host_path("/System/Kernel/os_code");
    if (host.empty()) return out;
    std::ifstream f(host, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<size_t> at;
    for (size_t o = 0; o + 0x100 <= d.size(); o += 4)
        if (be32(d, o + 0x10) == 0xEF000011u && be32(d, o + 0x0c) >> 24 == 0xEB) at.push_back(o);
    for (size_t i = 0; i < at.size(); ++i) {
        std::vector<uint8_t> part(d.begin() + (ptrdiff_t)at[i], i + 1 < at.size() ? d.begin() + (ptrdiff_t)at[i + 1] : d.end());
        Image im;
        if (!pf_aif_unpack(part, im.bytes).empty()) im.bytes = part;     // not compressed
        im.base = be32(im.bytes, 0x28);
        im.version = im.bytes.size() > 0x96 ? PF_VERSION(im.bytes[0x94], im.bytes[0x95]) : 0;
        out.push_back(std::move(im));
    }
    return out;
}

const Errs& errs() {
    if (g_errs.root == g_pf_disc_root) return g_errs;
    g_errs = Errs();
    g_errs.root = g_pf_disc_root;
    std::vector<Image> ims = os_images();
    for (const Image& im : ims) {
        for (const KernelErr& k : kKernels)
            if (!g_errs.k && im.base == 0x10000 && im.version == k.version) {
                g_errs.k = &k;
                g_errs.kernel = im;
            }
        for (const FolioErr& fe : kErrorTexts) {
            if (im.base || im.version != fe.version) continue;
            ErrorText t{0, 0, {}};
            uint32_t table = 0;
            for (uint32_t a = fe.tags; be32(im.bytes, a); a += 8) {
                uint32_t tag = be32(im.bytes, a), v = be32(im.bytes, a + 4);
                if (tag == ERRTEXT_TAG_OBJID) t.id = v;
                else if (tag == ERRTEXT_TAG_MAXERR) t.count = v & 0xff;     // a byte in the node (+0x28)
                else if (tag == ERRTEXT_TAG_TABLE) table = v;
            }
            for (uint32_t i = 0; i < t.count; ++i) t.texts.push_back(im.str(im.word(table + 4 * i)));
            g_errs.texts.push_back(t);
        }
    }
    return g_errs;
}

// Kernel -88: int32 GetSysErr(char* buf, int32 n, Err err) -- 0x1aacc. A buffer of 0x80 bytes
// on its stack, cleared: -1, an Err without its top bit, and number 0 each have a text of the
// kernel's (0x1ab28, 0x1ab5c, 0x1ab84); else the id's three characters, then the severity's, the
// environment's and the kind's names; then, for a standard error the kernel's text, for an
// extended one the text of the first ErrorText of that id, if it has the number -- copied after
// the names to at most 0x7f bytes from the names' start -- else the number in three digits
// between two strings of the kernel's (0x1ad6c, 0x1ad74). The text and its 0 to buf, cut to n bytes (its last a 0); that count back.
void k_getsyserr(ArmCpu& c) {
    const Errs& e = errs();
    if (!e.k) pf_stop(c, "GetSysErr: this disc's kernel's tables are not read yet");
    const KernelErr& k = *e.k;
    const Image& im = e.kernel;
    uint32_t ubuf = c.r[0], err = c.r[2];
    int32_t n = (int32_t)c.r[1];
    if (n < 1) pf_stop(c, "GetSysErr: a buffer of no bytes (the kernel would write before it)");
    char buf[0x90] = {};            // the text's 0x80 bytes, then the words the code keeps after them
    uint32_t num = err & 0xff;
    auto put = [&](char* to, const std::string& s, size_t max) { std::memcpy(to, s.c_str(), std::min(max, s.size() + 1)); };
    if (err == 0xFFFFFFFFu) put(buf, im.str(k.minus1), 0x16);
    else if (!(err & 0x80000000u)) put(buf, im.str(k.no_error), 9);
    else if (!num) put(buf, im.str(k.no_errornum), 0xc);
    else {
        uint32_t c1 = err >> 25 & 0x3f, c2 = err >> 19 & 0x3f, c3 = err >> 13 & 0x3f;
        uint32_t sev = err >> 11 & 3, env = err >> 9 & 3, ext = err >> 8 & 1;
        buf[0] = (char)im.bytes[k.chars - im.base + c1];
        buf[1] = (char)im.bytes[k.chars - im.base + c2];
        buf[2] = (char)im.bytes[k.chars - im.base + c3];
        char* r5 = buf + 3;
        std::string names = im.str(im.word(k.severities + 4 * sev)) + im.str(im.word(k.environments + 4 * env)) +
                            im.str(im.word(k.kinds + 4 * ext));
        put(r5, names, 0x7d);
        const std::vector<std::string>* texts = nullptr;
        std::vector<std::string> standard;
        uint32_t count = 0;
        if (!ext) {
            for (uint32_t i = 0; i < k.nstandard; ++i) standard.push_back(im.str(im.word(k.standard + 4 * i)));
            texts = &standard;
            count = k.nstandard;
        } else {
            uint32_t id = c1 << 12 | c2 << 6 | c3;
            bool found = false;
            for (const ErrorText& t : e.texts)
                if (t.id == id) {
                    texts = &t.texts;
                    count = t.count;
                    found = true;
                    break;
                }
            if (!found) {
                char why[96];
                std::snprintf(why, sizeof why, "GetSysErr: no ErrorText of object %c%c%c read (its folio's tags)",
                              buf[0], buf[1], buf[2]);
                pf_stop(c, why);
            }
        }
        if (count > num && texts) {
            size_t len = std::strlen(r5);
            put(r5 + len, (*texts)[num], 0x7f - len);
        } else {
            const char digits[4] = {(char)('0' + num / 100), (char)('0' + num % 100 / 10), (char)('0' + num % 10), 0};
            std::string tail = im.str(k.open) + digits + im.str(k.close);
            put(r5 + std::strlen(r5), tail, 0x7d - std::strlen(r5));
        }
    }
    int32_t len = (int32_t)std::strlen(buf) + 1;
    if (n < len) buf[n - 1] = 0;
    else n = len;
    for (int32_t i = 0; i < n; ++i) pf_w8(ubuf + (uint32_t)i, (uint8_t)buf[i]);
    if (g_pf_trace) pf_log("        GetSysErr: \"%s\"\n", buf);
    c.r[0] = (uint32_t)n;
}

}  // namespace

void pf_err_init() {
    pf_on_slot(PF_KERNEL, -88, k_getsyserr);
}

// os_code's images' own versions, in the file's order -- the kernel, the Operator, the File folio --
// 0 past the last; read once a disc.
uint32_t pf_os_code_version(int index) {
    static std::string s_root;
    static std::vector<uint32_t> s_versions;
    if (s_root != g_pf_disc_root) {
        s_root = g_pf_disc_root;
        s_versions.clear();
        for (const Image& im : os_images()) s_versions.push_back(im.version);
    }
    return index >= 0 && index < (int)s_versions.size() ? s_versions[(size_t)index] : 0;
}
