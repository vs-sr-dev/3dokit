// 3dokit runtime -- the Kernel folio: its SWIs and vector slots, as far as
// the programs run so far reach them. Every function is the SDK's by number
// (3dokit.sdk); what it does is what the SDK documents, checked against a
// program's use of it.
#include "pf.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

// The C library's printf, as the 1993 kernel's vfprintf (0x1a184, Norcroft's) runs it over
// guest memory: the format at `fmt`, each argument a word from `next`, each character out
// through `put`; the count of characters back. Flags - + space # 0, a width and a precision
// (either may be *), h (a short: the word's low half, signed for d and i) and l, and the
// conversions d i u o x X c s p n, with ANSI's meaning, which the 1993 code follows; %p is eight
// hex digits. e f g E G take a double and go to the caller's own fp_display, which nothing here
// calls yet: they stop. Any other character after % is printed as itself, padded to the width.
template <class Next, class Put>
static uint32_t format(const ArmCpu& c, uint32_t fmt, Next next, Put put) {
    uint32_t count = 0;
    auto out = [&](const char* s, size_t n) {
        for (size_t i = 0; i < n; ++i) put((uint8_t)s[i]);
        count += (uint32_t)n;
    };
    for (uint32_t p = fmt;; ++p) {
        char ch = (char)pf_r8(p);
        if (!ch) break;
        if (ch != '%') { out(&ch, 1); continue; }
        std::string flags;
        int width = 0, prec = -1;
        bool half = false;
        for (ch = (char)pf_r8(++p); ch && std::strchr("-+ #0", ch); ch = (char)pf_r8(++p)) flags += ch;
        if (ch == '*') {
            int32_t w = (int32_t)next();
            if (w < 0) { flags += '-'; w = -w; }
            width = w;
            ch = (char)pf_r8(++p);
        } else {
            for (; ch >= '0' && ch <= '9'; ch = (char)pf_r8(++p)) width = width * 10 + (ch - '0');
        }
        if (ch == '.') {
            ch = (char)pf_r8(++p);
            if (ch == '*') {
                prec = (int32_t)next();
                ch = (char)pf_r8(++p);
            } else {
                for (prec = 0; ch >= '0' && ch <= '9'; ch = (char)pf_r8(++p)) prec = prec * 10 + (ch - '0');
            }
            if (prec < 0) prec = -1;
        }
        if (ch == 'l' || ch == 'L') ch = (char)pf_r8(++p);
        else if (ch == 'h') { half = true; ch = (char)pf_r8(++p); }
        if (!ch) break;
        std::string spec = "%" + flags + (width ? std::to_string(width) : "") +
                           (prec >= 0 ? "." + std::to_string(prec) : "");
        char buf[600];
        int n;
        if (ch == 'd' || ch == 'i') {
            uint32_t v = next();
            n = std::snprintf(buf, sizeof buf, (spec + 'd').c_str(), half ? (int32_t)(int16_t)v : (int32_t)v);
        } else if (ch == 'u' || ch == 'o' || ch == 'x' || ch == 'X') {
            uint32_t v = next();
            n = std::snprintf(buf, sizeof buf, (spec + ch).c_str(), half ? v & 0xFFFFu : v);
        } else if (ch == 'p') {
            n = std::snprintf(buf, sizeof buf, ("%" + flags + (width ? std::to_string(width) : "") + ".8x").c_str(), next());
        } else if (ch == 'c') {
            n = std::snprintf(buf, sizeof buf, (spec + 'c').c_str(), (int)(next() & 0xFF));
        } else if (ch == 's') {
            uint32_t s = next();
            std::string str;
            for (uint32_t i = 0; prec < 0 || (int)i < prec; ++i) {
                char k = (char)pf_r8(s + i);
                if (!k) break;
                str += k;
            }
            n = std::snprintf(buf, sizeof buf, (spec + 's').c_str(), str.c_str());
        } else if (ch == 'n') {
            pf_w32(next(), count);
            continue;
        } else if (std::strchr("eEfgG", ch)) {
            pf_stop(const_cast<ArmCpu&>(c), "printf: a floating-point conversion: not yet");
        } else {
            n = std::snprintf(buf, sizeof buf, (spec + 'c').c_str(), ch);
        }
        out(buf, (size_t)std::min(n, (int)sizeof buf - 1));
    }
    return count;
}

// swi 0x1000e: kprintf(const char* fmt, ...) -- the debug console
static void k_kprintf(ArmCpu& c) {
    int arg = 1;
    std::string s;
    c.r[0] = format(c, c.r[0], [&] { return pf_arg(c, arg++); }, [&](uint8_t ch) { s += (char)ch; });
    pf_log("        kprintf: %s%s", s.c_str(), s.empty() || s.back() != '\n' ? "\n" : "");
}

// Kernel -84: int32 vfprintf(FILE* f, const char* fmt, va_list args, fp_display, putc) -- the C
// library's printf core (0x1a184), which a program's own printf, sprintf and the rest call with
// their FILE and their putc(ch, f): every character goes to the program's putc. The count back.
static void k_vfprintf(ArmCpu& c) {
    uint32_t file = c.r[0], va = c.r[2], put = pf_arg(c, 4);
    c.r[0] = format(c, c.r[1], [&] { uint32_t v = pf_r32(va); va += 4; return v; },
                    [&](uint8_t ch) { pf_guest_call(c, put, ch, file); });
}

// Kernel -120: what the AIF startup calls before main, with argc and argv
// in r0 and r1, and whose r0 and r1 main then receives. No SDK header or
// library names it. In the 1993 kernel (os_code v0.16, at 0x10ea0) it is the
// command line's parser: when the word at the top of the stack is not 0 the
// loader left the command line there, and it is split at spaces into argv
// words built below it, sp moved down past them. The loader here leaves no
// command line (the word is 0), and then argc and argv go on untouched.
static void k_startup(ArmCpu& c) { (void)c; }

// Kernel -52: void* memset(void* p, int c, size_t n) -- the destination back
static void k_memset(ArmCpu& c) {
    uint32_t p = c.r[0], v = c.r[1] & 0xff;
    for (uint32_t i = 0; i < c.r[2]; ++i) pf_w8(p + i, v);
}

// Kernel -56: void* memcpy(void* d, const void* s, size_t n) -- the 1993 kernel's (0x1130c)
// copies backwards when the source is below the destination: a memmove. The destination back.
static void k_memcpy(ArmCpu& c) {
    uint32_t d = c.r[0], s = c.r[1], n = c.r[2];
    if (s < d) {
        for (uint32_t i = n; i-- > 0;) pf_w8(d + i, pf_r8(s + i));
    } else if (s > d) {
        for (uint32_t i = 0; i < n; ++i) pf_w8(d + i, pf_r8(s + i));
    }
}

// ---- lists (list.h) ------------------------------------------------------------------------
// What the kernel's own InitList, AddHead, AddTail, InsertNodeFromTail and RemNode do. The
// anchor's two halves are pseudo-nodes: the first node's n_Prev is the list + 0x14, the last
// node's n_Next the list + 0x18, so linking before or after any node needs no special case.
void pf_list_init(uint32_t l, const char* name) {
    pf_w8(l + 8, 1);                                // KERNELNODE
    pf_w8(l + 9, 2);                                // LISTNODE
    pf_w8(l + 11, 0x80);                            // NODE_NAMEVALID
    pf_w32(l + 12, PF_LIST_SIZE);
    pf_w32(l + 16, name ? pf_os_string(name) : 0);
    pf_w32(l + PF_LIST_HEAD, l + PF_LIST_TAIL);
    pf_w32(l + PF_LIST_TAIL, 0);
    pf_w32(l + PF_LIST_LAST, l + PF_LIST_HEAD);
}

void pf_list_insert_before(uint32_t at, uint32_t n) {
    uint32_t prev = pf_r32(at + 4);
    pf_w32(n, at);
    pf_w32(n + 4, prev);
    pf_w32(prev, n);
    pf_w32(at + 4, n);
}

void pf_list_add_head(uint32_t l, uint32_t n) { pf_list_insert_before(pf_r32(l + PF_LIST_HEAD), n); }
void pf_list_add_tail(uint32_t l, uint32_t n) { pf_list_insert_before(l + PF_LIST_TAIL, n); }

// After the last node whose n_Priority is at least the new one's.
void pf_list_insert_from_tail(uint32_t l, uint32_t n) {
    uint32_t at = pf_r32(l + PF_LIST_LAST);
    while (at != l + PF_LIST_HEAD && pf_r8(at + 10) < pf_r8(n + 10)) at = pf_r32(at + 4);
    pf_list_insert_before(pf_r32(at), n);
}

void pf_list_rem_node(uint32_t n) {
    uint32_t next = pf_r32(n), prev = pf_r32(n + 4);
    if (!next) return;
    pf_w32(prev, next);
    pf_w32(next + 4, prev);
    pf_w32(n, 0);
}

// ---- items ---------------------------------------------------------------------------------
// An item number is an index into this table; 0 is no item. The node's
// ItemNode header is the truth for its type and name.
static std::vector<uint32_t> g_items(1, 0);

int32_t pf_item_new(uint32_t node, int subsys, int type, const char* name) {
    int32_t item = (int32_t)g_items.size();
    g_items.push_back(node);
    pf_w8(node + 8, subsys);
    pf_w8(node + 9, type);
    pf_w8(node + 11, 0x10 | (name ? 0x80 : 0));             // NODE_ITEMVALID, NODE_NAMEVALID
    if (!pf_r32(node + 12)) pf_w32(node + 12, PF_ITEMNODE_SIZE);
    pf_w32(node + 16, name ? pf_os_string(name) : 0);
    pf_w32(node + 24, (uint32_t)item);
    return item;
}

uint32_t pf_item_node(int32_t item) {
    return item > 0 && item < (int32_t)g_items.size() ? g_items[item] : 0;
}

int32_t pf_item_count() { return (int32_t)g_items.size(); }

static bool same_name(uint32_t a, const char* b) {
    for (;; ++a, ++b) {
        int x = std::tolower((int)pf_r8(a)), y = std::tolower((unsigned char)*b);
        if (x != y) return false;
        if (!x) return true;
    }
}

// swi 0x10004: Item FindItem(int32 ctype, TagArg* tags) -- ctype is
// MKNODEID(subsystem, type); the tags give TAG_ITEM_NAME (1)
static void k_finditem(ArmCpu& c) {
    uint32_t ctype = c.r[0];
    char name[64] = "";
    for (uint32_t t = c.r[1]; t; t += 8) {
        uint32_t tag = pf_r32(t), arg = pf_r32(t + 4);
        if (tag == 0) break;
        if (tag == 1) pf_cstring(arg, name, sizeof name);
        else pf_stop(c, "FindItem: a tag other than TAG_ITEM_NAME");
    }
    if (g_pf_trace) pf_log("        FindItem %#x \"%s\"\n", ctype, name);
    for (int32_t i = 1; i < (int32_t)g_items.size(); ++i) {
        uint32_t n = g_items[i];
        if (pf_r8(n + 8) == (ctype >> 8 & 0xFF) && pf_r8(n + 9) == (ctype & 0xFF) &&
            (!name[0] || (pf_r32(n + 16) && same_name(pf_r32(n + 16), name)))) {
            c.r[0] = (uint32_t)i;
            return;
        }
    }
    c.r[0] = (uint32_t)PF_ERR_NOTFOUND;
}

// A device counts its openers (dev_OpenCnt, +0x28: the kernel's OpenDevice and CloseDevice,
// 0x14b08 and 0x14b54).
static void count_opens(uint32_t n, int by) {
    if (pf_r8(n + 8) == 1 && pf_r8(n + 9) == 15) pf_w32(n + 0x28, pf_r32(n + 0x28) + (uint32_t)by);
}

// What each task has open: the 1993 kernel keeps an opened item in the task's resource table
// with ITEM_WAS_OPENED (0x4000) set, once per OpenItem, and CloseItem takes one out.
static std::map<int32_t, std::multiset<int32_t>> g_opened;

static int32_t task_item() { return (int32_t)pf_r32(pf_current_task() + 24); }

// swi 0x10005: Item OpenItem(Item found, void* args) -- the item, opened
static void k_openitem(ArmCpu& c) {
    uint32_t n = pf_item_node((int32_t)c.r[0]);
    if (!n) c.r[0] = (uint32_t)PF_ERR_BADITEM;
    else {
        count_opens(n, 1);
        g_opened[task_item()].insert((int32_t)c.r[0]);
    }
}

// swi 0x10008: Err CloseItem(Item)
static void k_closeitem(ArmCpu& c) {
    uint32_t n = pf_item_node((int32_t)c.r[0]);
    if (n) {
        count_opens(n, -1);
        auto& open = g_opened[task_item()];
        auto it = open.find((int32_t)c.r[0]);
        if (it != open.end()) open.erase(it);
    }
    c.r[0] = n ? 0 : (uint32_t)PF_ERR_BADITEM;
}

// Kernel -128: Err ItemOpened(Item task, Item it) -- os_code 0x138ec: CheckItem of the task
// (KERNELNODE, TASKNODE), else BADITEM; then 0 when its resource table holds the item as opened,
// else NOTFOUND.
int32_t pf_item_opened(int32_t task, int32_t item) {
    if (!pf_check_item(task, 1, 5)) return (int32_t)0xD57B9001u;
    auto it = g_opened.find(task);
    return it != g_opened.end() && it->second.count(item) ? 0 : (int32_t)0xD57B9005u;
}

static void k_itemopened(ArmCpu& c) { c.r[0] = (uint32_t)pf_item_opened((int32_t)c.r[0], (int32_t)c.r[1]); }

// Kernel -48: void* LookupItem(Item) -- the node, or NULL
static void k_lookupitem(ArmCpu& c) { c.r[0] = pf_item_node((int32_t)c.r[0]); }

// Kernel -64: void* CheckItem(Item, uint8 subsys, uint8 type) -- LookupItem, then NULL unless
// the node's n_SubsysType and n_Type are those (os_code 0x12bf8)
uint32_t pf_check_item(int32_t item, int subsys, int type) {
    uint32_t n = pf_item_node(item);
    return n && pf_r8(n + 8) == (uint32_t)(subsys & 0xFF) && pf_r8(n + 9) == (uint32_t)(type & 0xFF) ? n : 0;
}

static void k_checkitem(ArmCpu& c) { c.r[0] = pf_check_item((int32_t)c.r[0], (int)c.r[1], (int)c.r[2]); }

void pf_kernel_init() {
    g_items.assign(1, 0);
    g_opened.clear();
    pf_on_swi(0x1000e, k_kprintf);
    pf_on_swi(0x10004, k_finditem);
    pf_on_swi(0x10005, k_openitem);
    pf_on_swi(0x10008, k_closeitem);
    pf_on_slot(PF_KERNEL, -120, k_startup);
    pf_on_slot(PF_KERNEL, -48, k_lookupitem);
    pf_on_slot(PF_KERNEL, -52, k_memset);
    pf_on_slot(PF_KERNEL, -56, k_memcpy);
    pf_on_slot(PF_KERNEL, -64, k_checkitem);
    pf_on_slot(PF_KERNEL, -84, k_vfprintf);
    pf_on_slot(PF_KERNEL, -128, k_itemopened);
    // the folios a program finds by name: MKNODEID(KERNELNODE, FOLIONODE)
    for (int f = PF_GRAPHICS; f < PF_NFOLIOS; ++f)
        pf_item_new(pf_folio_base((PfFolio)f), 1, 4, g_pf_folio_names[f]);
}
