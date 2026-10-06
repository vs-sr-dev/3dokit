// 3dokit runtime -- the File folio: its SWIs and vector slots, as far as the
// programs run so far reach them.
#include "pf.h"
#include <string>

static std::string g_cwd = "/";                 // the current directory
static int32_t g_cwd_item;

// swi 0x30007: Item ChangeDirectory(char* path) -- the directory's item.
// `$boot` is the disc the program was started from: its root here.
static void f_changedirectory(ArmCpu& c) {
    char path[256];
    pf_cstring(c.r[0], path, sizeof path);
    std::string p = path;
    if (p.rfind("$boot", 0) == 0) p = "/" + p.substr(5);
    else if (p.empty() || p[0] != '/') p = g_cwd + (g_cwd.back() == '/' ? "" : "/") + p;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    g_cwd = p.empty() ? "/" : p;
    // a node of the File folio (filesystem.h: FILEFOLIO 3, FILENODE 2)
    if (!g_cwd_item) g_cwd_item = pf_item_new(pf_os_alloc(PF_ITEMNODE_SIZE), 3, 2, nullptr);
    if (g_pf_trace) pf_log("        ChangeDirectory \"%s\" -> %s\n", path, g_cwd.c_str());
    c.r[0] = (uint32_t)g_cwd_item;
}

void pf_file_init() {
    g_cwd = "/";
    g_cwd_item = 0;
    pf_on_swi(0x30007, f_changedirectory);
}
