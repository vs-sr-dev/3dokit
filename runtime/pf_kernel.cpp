// 3dokit runtime -- the Kernel folio: its SWIs and vector slots, as far as
// the programs run so far reach them. Every function is the SDK's by number
// (3dokit.sdk); what it does is what the SDK documents, checked against a
// program's use of it.
#include "pf.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

// swi 0x10005: Item OpenItem(Item found, void* args) -- the item, opened
static void k_openitem(ArmCpu& c) {
    if (!pf_item_node((int32_t)c.r[0])) c.r[0] = (uint32_t)PF_ERR_BADITEM;
}

// swi 0x10008: Err CloseItem(Item)
static void k_closeitem(ArmCpu& c) {
    c.r[0] = pf_item_node((int32_t)c.r[0]) ? 0 : (uint32_t)PF_ERR_BADITEM;
}

// Kernel -48: void* LookupItem(Item) -- the node, or NULL
static void k_lookupitem(ArmCpu& c) { c.r[0] = pf_item_node((int32_t)c.r[0]); }

void pf_kernel_init() {
    g_items.assign(1, 0);
    pf_on_swi(0x1000e, k_kprintf);
    pf_on_swi(0x10004, k_finditem);
    pf_on_swi(0x10005, k_openitem);
    pf_on_swi(0x10008, k_closeitem);
    pf_on_slot(PF_KERNEL, -120, k_startup);
    pf_on_slot(PF_KERNEL, -48, k_lookupitem);
    pf_on_slot(PF_KERNEL, -52, k_memset);
    pf_on_slot(PF_KERNEL, -56, k_memcpy);
    // the folios a program finds by name: MKNODEID(KERNELNODE, FOLIONODE)
    for (int f = PF_GRAPHICS; f < PF_NFOLIOS; ++f)
        pf_item_new(pf_folio_base((PfFolio)f), 1, 4, g_pf_folio_names[f]);
}
