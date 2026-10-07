// 3dokit runtime -- the File folio: its SWIs and vector slots, as far as the
// programs run so far reach them; and the disc, a directory on the host.
#include "pf.h"
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

std::string g_pf_disc_root = ".";
static std::string g_cwd = "/";                 // the current directory
static int32_t g_cwd_item;

// A program's path as the disc's: `$boot` is the disc the program was started from (its root
// here), a path that starts with / is from that root too, and any other from the current
// directory. Without trailing slashes.
static std::string disc_path(const std::string& path) {
    std::string p = path;
    if (p.rfind("$boot", 0) == 0) p = "/" + p.substr(5);
    else if (p.empty() || p[0] != '/') p = g_cwd + (g_cwd.back() == '/' ? "" : "/") + p;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p.empty() ? "/" : p;
}

static bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

std::string pf_host_path(const char* path) {
    std::string p = disc_path(path);
    fs::path at = g_pf_disc_root;
    std::error_code ec;
    for (size_t i = 1; i < p.size();) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") { at = at.parent_path(); continue; }
        fs::path next = at / part;
        if (!fs::exists(next, ec)) {
            next.clear();
            for (const auto& e : fs::directory_iterator(at, ec))
                if (same_name(e.path().filename().string(), part)) { next = e.path(); break; }
            if (next.empty()) return "";
        }
        at = next;
    }
    return at.string();
}

// swi 0x30007: Item ChangeDirectory(char* path) -- the directory's item.
static void f_changedirectory(ArmCpu& c) {
    char path[256];
    pf_cstring(c.r[0], path, sizeof path);
    g_cwd = disc_path(path);
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
