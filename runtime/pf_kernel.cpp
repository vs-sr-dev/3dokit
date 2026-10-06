// 3dokit runtime -- the Kernel folio: its SWIs and vector slots, as far as
// the programs run so far reach them. Every function is the SDK's by number
// (3dokit.sdk); what it does is what the SDK documents, checked against a
// program's use of it.
#include "pf.h"
#include <cstdio>
#include <cstring>
#include <string>

// printf over guest memory: the format at `fmt`, the arguments from APCS
// argument `first` on. Enough of C's conversions for the OS's debug output.
static std::string guest_printf(const ArmCpu& c, uint32_t fmt, int first) {
    std::string out;
    int arg = first;
    for (uint32_t p = fmt;; ++p) {
        char ch = (char)ld8(p);
        if (!ch) break;
        if (ch != '%') { out += ch; continue; }
        std::string spec = "%";
        for (;;) {
            ch = (char)ld8(++p);
            if (!ch) return out;
            if (std::strchr("-+ #0123456789.", ch)) { spec += ch; continue; }
            if (ch == 'l' || ch == 'h') continue;   // every integer is 32 bits
            break;
        }
        char buf[512];
        if (ch == '%') { out += '%'; continue; }
        if (ch == 's') {
            char s[256];
            pf_cstring(pf_arg(c, arg++), s, sizeof s);
            std::snprintf(buf, sizeof buf, (spec + 's').c_str(), s);
        } else if (std::strchr("dicuxXop", ch)) {
            uint32_t v = pf_arg(c, arg++);
            if (ch == 'p') { spec += "08X"; ch = 'X'; spec.pop_back(); }
            if (ch == 'd' || ch == 'i') std::snprintf(buf, sizeof buf, (spec + 'd').c_str(), (int32_t)v);
            else std::snprintf(buf, sizeof buf, (spec + ch).c_str(), v);
        } else {
            std::snprintf(buf, sizeof buf, "%%%c", ch);
        }
        out += buf;
    }
    return out;
}

// swi 0x1000e: kprintf(const char* fmt, ...) -- the debug console
static void k_kprintf(ArmCpu& c) {
    std::string s = guest_printf(c, c.r[0], 1);
    pf_log("        kprintf: %s%s", s.c_str(), s.empty() || s.back() != '\n' ? "\n" : "");
    c.r[0] = (uint32_t)s.size();
}

// Kernel -120: what the AIF startup calls before main, with argc and argv
// in r0 and r1, and whose r0 and r1 main then receives. No SDK header or
// library names it; until a program shows it does more, it hands them on.
static void k_startup(ArmCpu& c) { (void)c; }

void pf_kernel_init() {
    pf_on_swi(0x1000e, k_kprintf);
    pf_on_slot(PF_KERNEL, -120, k_startup);
}
