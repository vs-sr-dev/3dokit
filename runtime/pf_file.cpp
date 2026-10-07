// 3dokit runtime -- the File folio: paths and aliases, open files and their driver, the byte
// streams; and the disc, a directory on the host.
//
// The File folio is not on a disc: the console's ROM brings it (the FZ-1's, built in 1993, an AIF
// image at the ROM's 0x188a0 linked at 0: python -m 3dokit.rom). What is here is read in that
// code, and the addresses are its own. Its 14 SWIs run backwards from 0x654c, as the kernel's do
// (SWI 0, OpenDiskFile, is the table's last word, 0x3388; SWI 7, ChangeDirectory, 0x3514); its ten
// vectors (slot -4 at 0x6580 down) are user-mode library code -- the streams -- over those SWIs and
// the kernel's, which run here as the same calls into the runtime's kernel.
//
// The disc is the boot volume's root, a directory on the host (g_pf_disc_root). On the console
// "/" is the folio's own root, whose entries are the mounted filesystems, and the shell's `$boot`
// names one of them ("/cd-rom"); here "/" is the disc's root, and `$boot` is "/".
#include "pf.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

enum : uint32_t {
    // the folio's errors, as it builds them (operror.h's MakeErr: 'F' folio 'V', the standard class
    // below 0x100, the File folio's own -- filesystem.h's ER_Fs_ codes -- above)
    FERR_BADITEM = 0xD556F001u, FERR_NOMEM = 0xD556F006u, FERR_BADPTR = 0xD556F009u,
    FERR_BADCOMMAND = 0xD556F00Cu, FERR_BADNAME = 0xD556F00Eu,
    FERR_NOFILE = 0xD556F101u, FERR_NOTADIRECTORY = 0xD556F102u, FERR_FS_BADNAME = 0xD556F104u,
    // nodes
    FILEFOLIO = 3, FILENODE = 2, FILEALIASNODE = 3, DEVICENODE = 15,
    // File (filesystem.h): its ItemNode, then fi_FileName[32] and the rest; one avatar
    FI_NAME = 0x24, FI_PARENT = 0x48, FI_FLAGS = 0x54, FI_USECOUNT = 0x58, FI_BLOCKSIZE = 0x5c,
    FI_BYTECOUNT = 0x60, FI_BLOCKCOUNT = 0x64, FI_SIZE = 0x7c,
    FILE_IS_DIRECTORY = 1, FILE_IS_READONLY = 2, FILE_IS_FOR_FILESYSTEM = 4,
    BLOCK_SIZE = 2048,                      // FILESYSTEM_DEFAULT_BLOCKSIZE, a CD's
    // OpenFile (filesystem.h): a Device, then ofi_DeviceType and ofi_File
    DEV_IOREQSIZE = 0x3c, OFI_DEVICETYPE = 0x70, OFI_FILE = 0x74, OFI_SIZE = 0x98,
    FILE_DEVICE_OPENFILE = 2, FILEIOREQ_SIZE = 0x8c,
    // IOReq (io.h) and its IOInfo
    IO_DEV = 0x2c, IO_ACTUAL = 0x54, IO_FLAGS = 0x58, IO_ERROR = 0x5c, IO_MSGITEM = 0x68,
    IOI_COMMAND = 0x34, IOI_OFFSET = 0x40, IOI_RECV_BUF = 0x4c, IOI_RECV_LEN = 0x50,
    IO_DONE = 1, IO_QUICK = 2, SIGF_IODONE = 8, MESSAGENODE = 9,
    CMD_READ = 1, CMD_STATUS = 2, FILECMD_GETPATH = 4,
    // FileStatus (filesystem.h): a DeviceStatus (io.h), then fs_ByteCount
    FILESTATUS_SIZE = 0x28,
    // Stream (filestream.h)
    ST_OPENFILE = 0x00, ST_IOREQ = 0x04, ST_BUFFER = 0x08, ST_BUFFERLEN = 0x0c, ST_NEXT = 0x10,
    ST_AVAIL = 0x14, ST_FILEOFFSET = 0x18, ST_BLOCKSIZE = 0x1c, ST_BLOCKCOUNT = 0x20,
    ST_FILELENGTH = 0x24, ST_CURSOR = 0x28, ST_SEEKTO = 0x2c, ST_IOINPROGRESS = 0x30,
    ST_HADERROR = 0x31, ST_SEEKORIGIN = 0x34, ST_SIZE = 0x38,
    SEEK_SET_ = 1, SEEK_CUR_ = 2, SEEK_END_ = 3,
};

std::string g_pf_disc_root = ".";

static bool same_name(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return true;
}

static uint32_t task_item() { return pf_r32(pf_current_task() + 24); }

// ---- the disc --------------------------------------------------------------------------------
// A place on the disc is its path from the root, each name as the host spells it ("/" the root).
static fs::path host_of(const std::string& at) {
    fs::path p = g_pf_disc_root;
    if (at.size() > 1) p /= fs::path(at.substr(1));
    return p;
}

static std::string parent_of(const std::string& at) {
    size_t slash = at.find_last_of('/');
    return slash == 0 ? "/" : at.substr(0, slash);
}

// The entry `name` of the directory `at`, matched without case (the folio's 0x61e0, as the
// kernel's FindNamedNode matches); "" when there is none.
static std::string child_of(const std::string& at, const std::string& name) {
    std::error_code ec;
    std::string found;
    for (const auto& e : fs::directory_iterator(host_of(at), ec)) {
        std::string n = e.path().filename().string();
        if (n == name) { found = n; break; }
        if (found.empty() && same_name(n, name)) found = n;
    }
    return found.empty() ? "" : (at == "/" ? "" : at) + "/" + found;
}

static bool is_dir(const std::string& at) {
    std::error_code ec;
    return fs::is_directory(host_of(at), ec);
}

// A File node for each place a path has met, as the folio keeps one in its list of files (it
// looks there first, 0x2984): the name, the parent, the flags a CD's entries have (a directory 7,
// a file 2: python -m 3dokit.disc --list), the block size, the bytes and blocks. The entry's type
// and identifier are the disc's, which the host's directory does not keep: 0. No FileSystem node
// is made (fi_FileSystem 0).
static std::map<std::string, uint32_t> g_files;         // place -> File node
static std::map<uint32_t, std::string> g_places;        // File node -> place

static uint32_t file_node(const std::string& at) {
    auto it = g_files.find(at);
    if (it != g_files.end()) return it->second;
    std::string name = at == "/" ? "" : at.substr(at.find_last_of('/') + 1);
    uint32_t parent = at == "/" ? 0 : file_node(parent_of(at));
    uint32_t n = pf_os_alloc(FI_SIZE);
    pf_w32(n + 12, FI_SIZE);
    pf_item_new(n, FILEFOLIO, FILENODE, name.c_str());
    for (size_t i = 0; i < name.size() && i < 31; ++i) pf_w8(n + FI_NAME + (uint32_t)i, (uint8_t)name[i]);
    pf_w32(n + FI_PARENT, parent ? parent : n);
    uint32_t bytes = 0;
    if (is_dir(at)) {
        pf_w32(n + FI_FLAGS, FILE_IS_DIRECTORY | FILE_IS_READONLY | FILE_IS_FOR_FILESYSTEM);
    } else {
        std::error_code ec;
        bytes = (uint32_t)fs::file_size(host_of(at), ec);
        pf_w32(n + FI_FLAGS, FILE_IS_READONLY);
    }
    pf_w32(n + FI_BLOCKSIZE, BLOCK_SIZE);
    pf_w32(n + FI_BYTECOUNT, bytes);
    pf_w32(n + FI_BLOCKCOUNT, (bytes + BLOCK_SIZE - 1) / BLOCK_SIZE);
    g_files[at] = n;
    g_places[n] = at;
    return n;
}

// ---- aliases ---------------------------------------------------------------------------------
// An alias belongs to a task (its FileFolioTaskData's list, 0x82c): CreateAlias makes one for the
// caller, and a path's `$name` looks in the current task's list, then in its owner's, and so on
// up (0x23b8). The program's owner is the shell, whose aliases are kept here under 0: those the
// disc's own shell makes at the start (shell_start, below).
static std::map<uint32_t, std::map<std::string, std::string>> g_aliases;   // task item -> lowercase name -> value

static std::string lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

static bool find_alias(const std::string& name, std::string& value) {
    std::string key = lower(name);
    std::set<uint32_t> seen;
    for (uint32_t task = pf_current_task(); task;) {
        uint32_t item = pf_r32(task + 24);
        auto t = g_aliases.find(item);
        if (t != g_aliases.end()) {
            auto a = t->second.find(key);
            if (a != t->second.end()) { value = a->second; return true; }
        }
        uint32_t owner = pf_r32(task + 28);
        if (!owner || owner == item || !seen.insert(owner).second) break;
        task = pf_item_node((int32_t)owner);
    }
    auto a = g_aliases[0].find(key);
    if (a == g_aliases[0].end()) return false;
    value = a->second;
    return true;
}

// ---- paths -----------------------------------------------------------------------------------
static std::string g_cwd = "/";                 // the current directory (one for every task here)

// The walk of a path (0x2614), from the current directory: a '/' goes back to the root (a leading
// one, or any doubled), "." stays, ".." goes up (the root is its own parent), "^" is the root of
// the filesystem -- here the disc's --, and any other name is an entry of the directory reached so
// far (NOTADIRECTORY if it is not one, NOFILE if there is no such entry); a name has at most 31
// characters (else BADNAME). A name that starts with '$' is an alias: the path is made again with
// the alias's value in its place and walked on from there (an unknown alias is BADNAME). The
// alternatives a name can hold ({a|b}) stop the run: not yet.
static int32_t walk(const std::string& path, std::string& at) {
    std::string p = path;
    at = g_cwd;
    int substitutions = 0;
    for (size_t i = 0; i < p.size();) {
        if (p[i] == '/') {
            at = "/";
            ++i;
            continue;
        }
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string name = p.substr(i, j - i);
        if (name.find_first_of("{|}") != std::string::npos) {
            std::fprintf(stderr, "File: a path with alternatives (\"%s\"): not yet\n", path.c_str());
            std::exit(3);
        }
        if (name.size() >= 32) return (int32_t)FERR_FS_BADNAME;
        if (name[0] == '$') {
            std::string value;
            if (!find_alias(name.substr(1), value)) return (int32_t)FERR_FS_BADNAME;
            if (++substitutions > 64) {
                std::fprintf(stderr, "File: the aliases in \"%s\" do not end\n", path.c_str());
                std::exit(3);
            }
            p = p.substr(0, i) + value + p.substr(j);
            continue;
        }
        i = j < p.size() ? j + 1 : j;
        if (name == ".") continue;
        if (name == "..") { at = parent_of(at); continue; }
        if (name == "^") { at = "/"; continue; }
        if (!is_dir(at)) return (int32_t)FERR_NOTADIRECTORY;
        std::string next = child_of(at, name);
        if (next.empty()) return (int32_t)FERR_NOFILE;
        at = next;
    }
    return 0;
}

std::string pf_host_path(const char* path) {
    std::string at;
    if (walk(path, at)) return "";
    return host_of(at).string();
}

// ---- open files ------------------------------------------------------------------------------
// An open file is a device (0x24d8): CreateItem of an OpenFile -- the folio's driver, the File's
// name, priority 1, its IOReqs FileIOReqs (0x8c bytes) --, made on the task's behalf (the task's
// own, which the folio briefly makes privileged), ofi_DeviceType FILE_DEVICE_OPENFILE, ofi_File
// the File (one more use), and opened by the task.
static int32_t file_dispatch(uint32_t ior);
static int32_t file_closed(uint32_t dev);
static std::set<uint32_t> g_open_files;         // the OpenFile devices

static int32_t open_file(const std::string& at) {
    uint32_t file = file_node(at);
    std::string name = at == "/" ? "" : at.substr(at.find_last_of('/') + 1);
    uint32_t dev = pf_device_new(name.c_str(), 0, file_dispatch, file_closed, OFI_SIZE);
    int32_t item = (int32_t)pf_r32(dev + 24);
    pf_w8(dev + 10, 1);
    pf_w32(dev + 28, task_item());
    pf_w32(dev + DEV_IOREQSIZE, FILEIOREQ_SIZE);
    pf_w8(dev + OFI_DEVICETYPE, FILE_DEVICE_OPENFILE);
    pf_w32(dev + OFI_FILE, file);
    pf_w32(file + FI_USECOUNT, pf_r32(file + FI_USECOUNT) + 1);
    g_open_files.insert(dev);
    pf_open_item(item);
    return item;
}

// The device's delete hook (0x2438): the File has one use less. (The folio would also delete an
// internal IOReq and give back a registered buffer, which nothing here makes.)
static int32_t file_closed(uint32_t dev) {
    uint32_t file = pf_r32(dev + OFI_FILE);
    pf_w32(file + FI_USECOUNT, pf_r32(file + FI_USECOUNT) - 1);
    g_open_files.erase(dev);
    return 0;
}

// swi 0x30000: Item OpenDiskFile(char* path) -- 0x3388: the path walked (0x31cc) and the file
// opened, or the walk's error.
static int32_t open_disk_file(const char* path) {
    std::string at;
    int32_t err = walk(path, at);
    int32_t r = err ? err : open_file(at);
    if (g_pf_trace) pf_log("        OpenDiskFile \"%s\" -> %s %08X\n", path, err ? "error" : at.c_str(), (uint32_t)r);
    return r;
}

static void f_opendiskfile(ArmCpu& c) {
    char path[256];
    pf_cstring(c.r[0], path, sizeof path);
    c.r[0] = (uint32_t)open_disk_file(path);
}

// swi 0x30001: Err CloseDiskFile(Item file) -- 0x349c: an OpenFile device of the folio's driver,
// else BADITEM; CloseItem, then DeleteItem.
static int32_t close_disk_file(ArmCpu& c, int32_t item) {
    uint32_t dev = pf_check_item(item, 1, DEVICENODE);
    if (!dev || !g_open_files.count(dev) || pf_r8(dev + OFI_DEVICETYPE) != FILE_DEVICE_OPENFILE)
        return (int32_t)FERR_BADITEM;
    pf_close_item(item);
    return pf_delete_item(c, item);
}

static void f_closediskfile(ArmCpu& c) { c.r[0] = (uint32_t)close_disk_file(c, (int32_t)c.r[0]); }

// ---- the open files' driver ------------------------------------------------------------------
// The folio's own dispatch (0xd9c; the kernel's SendIO ends by jumping to it, and returns what it
// returns): a command above 10 is BADCOMMAND, refused. CMD_STATUS (0x107c) is answered at once:
// a FileStatus of the File -- ds_DriverIdentity 5, ds_FamilyCode 3, ds_MaximumStatusSize 0x28, the
// block size and count, fi_Flags as ds_DeviceFlagWord, DS_USAGE_READONLY (0x20000000) in the
// usage flags for a read-only file, fs_ByteCount -- copied to ioi_Recv: 0x28 bytes when it is
// shorter (the folio's max where a min was meant), and when it is longer the folio copies its
// stack past the status, here nothing; io_Actual stays 0; CompleteIO, and 0. FILECMD_GETPATH (4)
// stops the run: not yet. Every other command goes to the filesystem's queue, a CD's (0x117c):
// only CMD_READ, of whole blocks (else BADPTR), queued -- IO_QUICK cleared -- for the file daemon;
// a refusal is io_Error, CompleteIO, and SendIO's result. The daemon reads the blocks (a CD's
// FileIOReq end action, 0x17c0: io_Actual the bytes the drive moved, whole blocks) -- here at the
// next safe point, as the CD's reading time is not modelled. A read past the file's last block
// stops the run: not yet.
static std::vector<uint32_t> g_reads;           // queued reads, in order

static const char kFill[] = "iamaduck";

// A read done: the file's bytes, and past its end, up to the end of its last block, what the
// disc's mastering left there -- the Opera mastering tool's fill, the eight letters "iamaduck"
// over and over, by the byte's place in its block (every file of the two discs checked, but
// rom_tags and OMF2097's BannerScreen). Not every disc has it: of five surveyed, Alone in the Dark
// has no fill at all, and its files' tails hold whatever the master's memory held -- which only
// the disc's image can give.
static void read_blocks(uint32_t ior) {
    uint32_t file = pf_r32(pf_r32(ior + IO_DEV) + OFI_FILE);
    uint32_t bs = pf_r32(file + FI_BLOCKSIZE), bytes = pf_r32(file + FI_BYTECOUNT);
    uint32_t at = pf_r32(ior + IOI_OFFSET) * bs, buf = pf_r32(ior + IOI_RECV_BUF);
    uint32_t len = pf_r32(ior + IOI_RECV_LEN);
    std::vector<uint8_t> d(len);
    uint32_t have = at < bytes ? std::min(len, bytes - at) : 0;
    if (have) {
        std::ifstream f(host_of(g_places[file]), std::ios::binary);
        f.seekg(at);
        f.read((char*)d.data(), have);
    }
    for (uint32_t i = have; i < len; ++i) d[i] = (uint8_t)kFill[(at + i) % bs % 8];
    for (uint32_t i = 0; i < len; ++i) pf_w8(buf + i, d[i]);
    pf_w32(ior + IO_ACTUAL, pf_r32(ior + IO_ACTUAL) + len);
}

static void reads_done(uint64_t) {
    std::vector<uint32_t> now;
    now.swap(g_reads);
    for (uint32_t ior : now) {
        read_blocks(ior);
        pf_complete_io(ior);
    }
}

static int32_t refuse(uint32_t ior, uint32_t err) {
    pf_w32(ior + IO_ERROR, err);
    pf_complete_io(ior);
    return (int32_t)err;
}

static int32_t file_dispatch(uint32_t ior) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND);
    if (cmd > 10) return (int32_t)FERR_BADCOMMAND;
    uint32_t file = pf_r32(pf_r32(ior + IO_DEV) + OFI_FILE);
    if (cmd == CMD_STATUS) {
        uint32_t buf = pf_r32(ior + IOI_RECV_BUF), flags = pf_r32(file + FI_FLAGS);
        for (uint32_t i = 0; i < FILESTATUS_SIZE; i += 4) pf_w32(buf + i, 0);
        pf_w8(buf + 0, 5);
        pf_w8(buf + 2, 3);
        pf_w32(buf + 0x04, FILESTATUS_SIZE);
        pf_w32(buf + 0x08, pf_r32(file + FI_BLOCKSIZE));
        pf_w32(buf + 0x0c, pf_r32(file + FI_BLOCKCOUNT));
        pf_w32(buf + 0x10, flags);
        pf_w32(buf + 0x14, flags & FILE_IS_READONLY ? 0x20000000u : 0);
        pf_w32(buf + 0x24, pf_r32(file + FI_BYTECOUNT));
        pf_complete_io(ior);
        return 0;
    }
    if (cmd == FILECMD_GETPATH) {
        std::fprintf(stderr, "File: FILECMD_GETPATH: not yet\n");
        std::exit(3);
    }
    if (cmd != CMD_READ) return refuse(ior, FERR_BADCOMMAND);
    uint32_t bs = pf_r32(file + FI_BLOCKSIZE), len = pf_r32(ior + IOI_RECV_LEN);
    if (len % bs) return refuse(ior, FERR_BADPTR);
    if ((uint64_t)pf_r32(ior + IOI_OFFSET) + len / bs > pf_r32(file + FI_BLOCKCOUNT)) {
        std::fprintf(stderr, "File: a read of %u blocks from block %u of \"%s\", past its %u: not yet\n",
                     len / bs, pf_r32(ior + IOI_OFFSET), g_places[file].c_str(), pf_r32(file + FI_BLOCKCOUNT));
        std::exit(3);
    }
    pf_w32(ior + IO_FLAGS, pf_r32(ior + IO_FLAGS) & ~IO_QUICK);
    if (g_reads.empty()) pf_at(pf_now(), reads_done);
    g_reads.push_back(ior);
    return 0;
}

// ---- the folio's SWIs ------------------------------------------------------------------------
// swi 0x30007: Item ChangeDirectory(char* path) -- 0x3514: the path walked from the current
// directory, which it becomes; the File's item, or the walk's error. (The folio does not ask for
// a directory.)
static void f_changedirectory(ArmCpu& c) {
    char path[256];
    pf_cstring(c.r[0], path, sizeof path);
    std::string at;
    int32_t err = walk(path, at);
    if (!err) {
        uint32_t old = file_node(g_cwd), now = file_node(at);
        pf_w32(old + FI_USECOUNT, pf_r32(old + FI_USECOUNT) - 1);
        pf_w32(now + FI_USECOUNT, pf_r32(now + FI_USECOUNT) + 1);
        g_cwd = at;
        c.r[0] = pf_r32(now + 24);
    } else {
        c.r[0] = (uint32_t)err;
    }
    if (g_pf_trace) pf_log("        ChangeDirectory \"%s\" -> %s\n", path, err ? "error" : g_cwd.c_str());
}

// swi 0x3000b: Item CreateAlias(char* name, char* value) -- 0x3684: a name of at most 31
// characters and a value of at most 255, else BADNAME; an alias node (FILEALIASNODE: its ItemNode,
// a_Value, the value) for the caller, replacing one of the same name.
static void f_createalias(ArmCpu& c) {
    char name[256], value[512];
    size_t nn = c.r[0] ? pf_cstring(c.r[0], name, sizeof name) : 0;
    size_t nv = c.r[1] ? pf_cstring(c.r[1], value, sizeof value) : 0;
    if (!c.r[0] || !c.r[1] || nn > 31 || nv > 255) { c.r[0] = FERR_BADNAME; return; }
    uint32_t n = pf_os_alloc(0x28 + (uint32_t)nv + 1);
    pf_w32(n + 12, 0x28 + (uint32_t)nv + 1);
    int32_t item = pf_item_new(n, FILEFOLIO, FILEALIASNODE, name);
    pf_w32(n + 0x24, n + 0x28);
    for (size_t i = 0; i <= nv; ++i) pf_w8(n + 0x28 + (uint32_t)i, (uint8_t)value[i]);
    pf_w32(n + 28, task_item());
    g_aliases[task_item()][lower(name)] = value;
    if (g_pf_trace) pf_log("        CreateAlias %s = \"%s\"\n", name, value);
    c.r[0] = (uint32_t)item;
}

// ---- streams ---------------------------------------------------------------------------------
// The folio's byte streams (filestream.h), user-mode code over an open file and one IOReq: a
// buffer of whole blocks the next blocks are read into ahead of the reader. Each function is the
// folio's own, step for step; what they ask of the OS -- AllocMem from the task's lists,
// OpenDiskFile, CreateIOReq, SendIO, WaitSignal, DeleteItem, CloseDiskFile -- is the runtime's same
// calls. Their IOInfos and the file's status are on the caller's stack, where the folio's frames
// put them (SendIO wants a receive buffer the task may write).
static uint32_t rd(uint32_t st, uint32_t f) { return pf_r32(st + f); }
static void wr(uint32_t st, uint32_t f, uint32_t v) { pf_w32(st + f, v); }

// Kernel -56, the kernel's memcpy (a memmove)
static void copy(uint32_t d, uint32_t s, uint32_t n) {
    if (s < d) for (uint32_t i = n; i-- > 0;) pf_w8(d + i, pf_r8(s + i));
    else if (s > d) for (uint32_t i = 0; i < n; ++i) pf_w8(d + i, pf_r8(s + i));
}

static void set_info(uint32_t info, uint32_t cmd, uint32_t flags, uint32_t offset, uint32_t recv, uint32_t len) {
    for (uint32_t i = 0; i < 32; i += 4) pf_w32(info + i, 0);
    pf_w8(info + 0, cmd);
    pf_w8(info + 1, flags);
    pf_w32(info + 12, offset);
    pf_w32(info + 24, recv);
    pf_w32(info + 28, len);
}

// The folio's own CheckIO (0x6230), WaitIO (0x6254) and DoIO (0x62cc): WaitIO returns at once for
// a quick request, else waits for SIGF_IODONE until the request is done (a reply port: not yet), and
// is 0; DoIO asks for a quick request, and is SendIO's error or WaitIO's 0.
static bool check_io(int32_t item) { return pf_r32(pf_item_node(item) + IO_FLAGS) & IO_DONE; }

static int32_t wait_io(ArmCpu& c, int32_t item) {
    uint32_t ior = pf_item_node(item);
    if (pf_r32(ior + IO_FLAGS) & IO_QUICK) return 0;
    uint32_t m = pf_item_node((int32_t)pf_r32(ior + IO_MSGITEM));
    if (m && pf_r8(m + 9) == MESSAGENODE) pf_stop(c, "a stream's WaitIO on a reply port: not yet");
    while (!check_io(item)) pf_wait_signal(SIGF_IODONE);
    return 0;
}

static int32_t do_io(ArmCpu& c, int32_t item, uint32_t info) {
    pf_w8(info + 1, pf_r8(info + 1) | IO_QUICK);
    int32_t r = pf_send_io(c, item, info);
    return r < 0 ? r : wait_io(c, item);
}

static int32_t ior_item(uint32_t st) { return (int32_t)pf_r32(rd(st, ST_IOREQ) + 24); }

// File -4: Stream* OpenDiskStream(char* name, int32 bSize) -- 0x4e40: the Stream (0x38 bytes,
// MEMTYPE_FILL, from the task's lists), the file opened, an IOReq on it, the file's status (DoIO of
// CMD_STATUS); the buffer: -bSize blocks (at most the file's), or bSize rounded up to whole blocks,
// or by default FILESTREAM_BUFFER_MIN_BLOCKS (2; 1 for a file of two blocks or fewer), at least
// 256 bytes, MEMTYPE_DMA from the task's lists; then the first read sent (as much of the file as
// the buffer holds) and left running. NULL when any step fails, everything before it undone.
static void f_opendiskstream(ArmCpu& c) {
    uint32_t name = c.r[0];
    int32_t bsize = (int32_t)c.r[1];
    uint32_t lists = pf_r32(pf_current_task() + T_FREEMEMORYLISTS);
    uint32_t sp = c.r[13] - 0x28 - 0x58, status = sp + 0x10, info = sp + 0x38;
    c.r[0] = 0;
    uint32_t st = pf_alloc_mem(lists, ST_SIZE, MEMTYPE_FILL, true);
    if (!st) return;
    char path[256];
    pf_cstring(name, path, sizeof path);
    int32_t file = open_disk_file(path);
    if (file < 0) { pf_free_mem(lists, st, ST_SIZE); return; }
    int32_t ior = pf_create_ioreq(c, file);
    if (ior < 0) {
        close_disk_file(c, file);
        pf_free_mem(lists, st, ST_SIZE);
        return;
    }
    set_info(info, CMD_STATUS, IO_QUICK, 0, status, FILESTATUS_SIZE);
    pf_w32(status + 8, BLOCK_SIZE);             // what the folio puts there first
    pf_w32(status + 12, 0);
    uint32_t len = 0, buf = 0;
    if (!do_io(c, ior, info)) {
        uint32_t bs = pf_r32(status + 8), count = pf_r32(status + 12);
        uint32_t least = count <= 2 ? 1 : 2;
        if (bsize < 0) {
            uint32_t n = (uint32_t)-bsize;
            len = bs * (n <= count ? n : count);
        } else if (bsize == 0) {
            len = least * bs;
        } else {
            uint32_t up = (uint32_t)bsize + bs - 1;
            len = up - up % bs;
            if (!len) len = least * bs;
        }
        if (len < 0x100) len = 0x100;
        buf = pf_alloc_mem(lists, (int32_t)len, MEMTYPE_DMA, true);
        if (buf) {
            wr(st, ST_OPENFILE, (uint32_t)file);
            wr(st, ST_IOREQ, pf_item_node(ior));
            wr(st, ST_BUFFER, buf);
            wr(st, ST_BUFFERLEN, len);
            wr(st, ST_NEXT, 0);
            wr(st, ST_AVAIL, 0);
            wr(st, ST_FILEOFFSET, 0);
            wr(st, ST_CURSOR, 0);
            wr(st, ST_BLOCKSIZE, bs);
            wr(st, ST_BLOCKCOUNT, count);
            wr(st, ST_FILELENGTH, pf_r32(status + 0x24));
            wr(st, ST_SEEKORIGIN, 0);
            set_info(info, CMD_READ, 0, 0, buf, bs * count >= len ? len : bs * count);
            if (!pf_send_io(c, ior, info)) {
                pf_w8(st + ST_IOINPROGRESS, 1);
                pf_w8(st + ST_HADERROR, 0);
                c.r[0] = st;
                return;
            }
            pf_free_mem(lists, buf, (int32_t)len);
        }
    }
    pf_delete_item(c, ior);
    close_disk_file(c, file);
    pf_free_mem(lists, st, ST_SIZE);
}

// File -16: void CloseDiskStream(Stream*) -- 0x5094: a read still running waited for; the IOReq
// deleted, the file closed, the buffer and the Stream given back.
static void f_closediskstream(ArmCpu& c) {
    uint32_t st = c.r[0], lists = pf_r32(pf_current_task() + T_FREEMEMORYLISTS);
    if (pf_r8(st + ST_IOINPROGRESS)) wait_io(c, ior_item(st));
    pf_delete_item(c, ior_item(st));
    close_disk_file(c, (int32_t)rd(st, ST_OPENFILE));
    pf_free_mem(lists, rd(st, ST_BUFFER), (int32_t)rd(st, ST_BUFFERLEN));
    pf_free_mem(lists, st, ST_SIZE);
}

// File -12: int32 SeekDiskStream(Stream*, int32 offset, enum SeekOrigin whence) -- 0x55f4: from
// the start (SEEK_SET), from where the cursor is or a seek still pending would put it (SEEK_CUR),
// or back from the end (SEEK_END: the length less the offset); outside the file, or another
// whence, is -1. The seek only waits, in st_SeekTo and st_SeekOrigin, for the next read; the new
// position back.
static void f_seekdiskstream(ArmCpu& c) {
    uint32_t st = c.r[0], whence = c.r[2];
    int32_t pos = (int32_t)c.r[1];
    if (whence == SEEK_CUR_) {
        uint32_t o = rd(st, ST_SEEKORIGIN);
        if (o == 0) pos += (int32_t)rd(st, ST_CURSOR);
        else if (o <= 3) pos += (int32_t)rd(st, ST_SEEKTO);
    } else if (whence == SEEK_END_) {
        pos = (int32_t)rd(st, ST_FILELENGTH) - pos;
    } else if (whence != SEEK_SET_) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    if (pos < 0 || pos > (int32_t)rd(st, ST_FILELENGTH)) { c.r[0] = 0xFFFFFFFFu; return; }
    wr(st, ST_SEEKTO, (uint32_t)pos);
    wr(st, ST_SEEKORIGIN, whence);
    c.r[0] = (uint32_t)pos;
}

// File -8: int32 ReadDiskStream(Stream*, char* buffer, int32 nBytes) -- 0x5110, its own branches
// for labels: the read running, if any, taken in (waited for when a seek is pending); a pending
// seek made, inside the buffer or by starting again at the seek's block; then the bytes copied out
// of the buffer, up to the end of the file; what is still wanted read straight into the caller's
// buffer in whole blocks, and a last part block through the stream's buffer; and the next blocks
// sent for while the buffer has room. The bytes read back, or -1 when the read ahead cannot be
// sent.
static void f_readdiskstream(ArmCpu& c) {
    uint32_t st = c.r[0], dst = c.r[1], orig = c.r[1];
    int32_t n = (int32_t)c.r[2];
    uint32_t info = c.r[13] - 0x28 - 0x2c + 4;
    int32_t bs = 0, d = 0, take = 0, err = 0, room = 0, at = 0, len = 0;
    uint32_t o = rd(st, ST_SEEKORIGIN);
    if (o > 3) pf_log("        kprintf: Seek value corrupted: %x\n", o);
again:                                                      // 0x5154
    if (pf_r8(st + ST_IOINPROGRESS)) {
        if (rd(st, ST_SEEKORIGIN) == 0) {
            if (!check_io(ior_item(st))) goto seek;
        } else {
            wait_io(c, ior_item(st));
        }
    taken_in:                                               // 0x5180
        {
            uint32_t ior = rd(st, ST_IOREQ), actual = pf_r32(ior + IO_ACTUAL);
            wr(st, ST_AVAIL, rd(st, ST_AVAIL) + actual);
            wr(st, ST_FILEOFFSET, rd(st, ST_FILEOFFSET) + actual);
            pf_w8(st + ST_IOINPROGRESS, 0);
            if (pf_r32(ior + IO_ERROR)) pf_w8(st + ST_HADERROR, 1);
        }
    }
seek:                                                       // 0x51bc
    if (rd(st, ST_SEEKORIGIN)) {
        int32_t to = (int32_t)rd(st, ST_SEEKTO), cur = (int32_t)rd(st, ST_CURSOR);
        bs = (int32_t)rd(st, ST_BLOCKSIZE);
        d = to - cur;
        bool inside = d >= 0 ? d < (int32_t)rd(st, ST_AVAIL) : to / bs == cur / bs;
        if (!inside) {                                      // 0x5370
            wr(st, ST_FILEOFFSET, (uint32_t)(to - to % bs));
            wr(st, ST_CURSOR, (uint32_t)(to - to % bs));
            wr(st, ST_AVAIL, 0);
            wr(st, ST_NEXT, 0);
            goto ahead;
        }
        wr(st, ST_AVAIL, (uint32_t)((int32_t)rd(st, ST_AVAIL) - d));      // 0x521c
        wr(st, ST_CURSOR, (uint32_t)(cur + d));
        wr(st, ST_NEXT, (uint32_t)(((int32_t)rd(st, ST_NEXT) + d) % (int32_t)rd(st, ST_BUFFERLEN)));
        wr(st, ST_SEEKORIGIN, 0);
    }
    {                                                       // 0x524c
        int32_t left = (int32_t)rd(st, ST_FILELENGTH) - (int32_t)rd(st, ST_CURSOR);
        if (n > left) n = left;
        if (n <= 0) { c.r[0] = 0; return; }
    }
    take = (int32_t)rd(st, ST_BUFFERLEN) - (int32_t)rd(st, ST_NEXT);
    if (take <= n && take <= (int32_t)rd(st, ST_AVAIL)) {    // the buffer's end, then its start
        copy(dst, rd(st, ST_BUFFER) + rd(st, ST_NEXT), (uint32_t)take);
        wr(st, ST_NEXT, 0);
        wr(st, ST_AVAIL, rd(st, ST_AVAIL) - (uint32_t)take);
        wr(st, ST_CURSOR, rd(st, ST_CURSOR) + (uint32_t)take);
        n -= take;
        dst += (uint32_t)take;
    }
    if (n == 0) goto ahead;                                 // 0x52c4
    take = n > (int32_t)rd(st, ST_AVAIL) ? (int32_t)rd(st, ST_AVAIL) : n;
    if (take > 0) {
        copy(dst, rd(st, ST_BUFFER) + rd(st, ST_NEXT), (uint32_t)take);
        wr(st, ST_NEXT, rd(st, ST_NEXT) + (uint32_t)take);
        wr(st, ST_AVAIL, rd(st, ST_AVAIL) - (uint32_t)take);
        wr(st, ST_CURSOR, rd(st, ST_CURSOR) + (uint32_t)take);
        n -= take;
        dst += (uint32_t)take;
        if (n == 0) goto ahead;
    }
    if (pf_r8(st + ST_HADERROR)) goto done;                 // 0x532c
    if (pf_r8(st + ST_IOINPROGRESS)) {                      // 0x5344
        wait_io(c, ior_item(st));
        goto taken_in;
    }
    bs = (int32_t)rd(st, ST_BLOCKSIZE);
    while (n >= bs) {                                       // 0x5424, 0x5398: whole blocks, straight
        int32_t whole = n - n % bs;
        set_info(info, CMD_READ, 0, rd(st, ST_FILEOFFSET) / (uint32_t)bs, dst, (uint32_t)whole);
        err = do_io(c, ior_item(st), info);
        uint32_t actual = pf_r32(rd(st, ST_IOREQ) + IO_ACTUAL);
        n -= (int32_t)actual;
        dst += actual;
        wr(st, ST_FILEOFFSET, rd(st, ST_FILEOFFSET) + actual);
        wr(st, ST_CURSOR, rd(st, ST_CURSOR) + actual);
        if (err) {
            pf_w8(st + ST_HADERROR, 1);
            goto done;
        }
        bs = (int32_t)rd(st, ST_BLOCKSIZE);
    }
    wr(st, ST_NEXT, 0);
    if (n > 0) {                                            // 0x5440: a last part block
        set_info(info, CMD_READ, 0, rd(st, ST_FILEOFFSET) / (uint32_t)bs, rd(st, ST_BUFFER), (uint32_t)bs);
        err = do_io(c, ior_item(st), info);
        uint32_t actual = pf_r32(rd(st, ST_IOREQ) + IO_ACTUAL);
        take = (int32_t)actual > n ? n : (int32_t)actual;
        copy(dst, rd(st, ST_BUFFER), (uint32_t)take);
        wr(st, ST_FILEOFFSET, rd(st, ST_FILEOFFSET) + actual);
        wr(st, ST_NEXT, (uint32_t)take);
        wr(st, ST_AVAIL, (uint32_t)(bs - take));
        wr(st, ST_CURSOR, rd(st, ST_CURSOR) + (uint32_t)take);
        dst += (uint32_t)take;
        if (err) pf_w8(st + ST_HADERROR, 1);
    }
ahead:                                                      // 0x54f0: read ahead
    if (pf_r8(st + ST_IOINPROGRESS)) goto done;
    bs = (int32_t)rd(st, ST_BLOCKSIZE);
    len = (int32_t)rd(st, ST_BUFFERLEN);
    room = (len - (int32_t)rd(st, ST_AVAIL)) / bs;
    if (room > 0) {
        int32_t left = (int32_t)rd(st, ST_BLOCKCOUNT) - (int32_t)(rd(st, ST_FILEOFFSET) / (uint32_t)bs);
        if (left <= 0) goto done;
        if (left < room) room = left;
        at = (int32_t)rd(st, ST_NEXT) + (int32_t)rd(st, ST_AVAIL);
        if (at >= len) {
            at -= len;
        } else {
            int32_t to_end = (len - at) / bs;
            if (to_end < room) room = to_end;
        }
        set_info(info, CMD_READ, 0, rd(st, ST_FILEOFFSET) / (uint32_t)bs, rd(st, ST_BUFFER) + (uint32_t)at,
                 (uint32_t)(bs * room));
        if (pf_send_io(c, ior_item(st), info)) { c.r[0] = 0xFFFFFFFFu; return; }
        pf_w8(st + ST_IOINPROGRESS, 1);
    }
    if (rd(st, ST_SEEKORIGIN) && n > 0) goto again;         // 0x55d4
done:                                                       // 0x55e8
    c.r[0] = dst - orig;
}

// ---- the shell's start -----------------------------------------------------------------------
// The program's aliases come from the shell that starts it: the disc's own (System/Tasks/shell,
// 1993) makes `alias boot /` and the boot filesystem's name, goes to `$boot`, and runs the script
// ^/system/scripts/startopera, which makes aliases and runs other scripts -- a disc's AppStartup
// among them, the place its own aliases go ("alias exdir $boot"). Here the scripts are read for
// their aliases: a line `alias NAME VALUE` makes one, a line naming a script (a file on the disc
// that is not an AIF image) runs it, once; every other line -- a program, a folio, the shell's
// other commands -- is the runtime's own business or none.
static void shell_script(const std::string& at, std::set<std::string>& ran) {
    if (!ran.insert(at).second) return;
    std::ifstream f(host_of(at), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (text.size() >= 0x14 && (uint8_t)text[0x10] == 0xEF && (uint8_t)text[0x13] == 0x11) return;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find_first_of("\r\n", i);
        if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        i = j + 1;
        std::vector<std::string> words;
        for (size_t k = 0; k < line.size();) {
            size_t s = line.find_first_not_of(" \t", k);
            if (s == std::string::npos) break;
            size_t e = line.find_first_of(" \t", s);
            if (e == std::string::npos) e = line.size();
            words.push_back(line.substr(s, e - s));
            k = e;
        }
        if (words.empty() || words[0][0] == '#') continue;
        if (words[0] == "alias" && words.size() >= 3) {
            g_aliases[0][lower(words[1])] = words[2];
            continue;
        }
        std::string next;
        if (!walk(words[0], next) && !is_dir(next)) shell_script(next, ran);
    }
}

static void shell_start() {
    g_aliases[0]["boot"] = "/";
    std::string at;
    std::set<std::string> ran;
    if (!walk("^/system/scripts/startopera", at)) shell_script(at, ran);
}

void pf_file_init() {
    g_cwd = "/";
    g_files.clear();
    g_places.clear();
    g_aliases.clear();
    g_open_files.clear();
    g_reads.clear();
    shell_start();
    if (g_pf_trace) {
        pf_log("shell aliases:");
        for (const auto& a : g_aliases[0]) pf_log(" %s=%s", a.first.c_str(), a.second.c_str());
        pf_log("\n");
    }
    pf_on_swi(0x30000, f_opendiskfile);
    pf_on_swi(0x30001, f_closediskfile);
    pf_on_swi(0x30007, f_changedirectory);
    pf_on_swi(0x3000b, f_createalias);
    pf_on_slot(PF_FILE, -4, f_opendiskstream);
    pf_on_slot(PF_FILE, -8, f_readdiskstream);
    pf_on_slot(PF_FILE, -12, f_seekdiskstream);
    pf_on_slot(PF_FILE, -16, f_closediskstream);
}
