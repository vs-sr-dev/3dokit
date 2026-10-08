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
    FERR_ABORTED = 0xD556F00Au, FERR_BADCOMMAND = 0xD556F00Cu, FERR_BADNAME = 0xD556F00Eu,
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
static void file_abort(uint32_t ior);
static int32_t file_closed(uint32_t dev);
static std::set<uint32_t> g_open_files;         // the OpenFile devices

static int32_t open_file(const std::string& at) {
    uint32_t file = file_node(at);
    std::string name = at == "/" ? "" : at.substr(at.find_last_of('/') + 1);
    uint32_t dev = pf_device_new(name.c_str(), 0, file_dispatch, file_closed, OFI_SIZE);
    pf_device_abort(dev, file_abort);
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
// FileIOReq end action, 0x17c0: io_Actual the bytes the drive moved, whole blocks). A read past
// the file's last block stops the run: not yet.
//
// The drive's time: the console's is a double-speed CD-ROM drive, 150 blocks of 2048 bytes a
// second, and it reads one request at a time, in the order they came. So a read is done when the
// drive has moved its bytes at that rate, starting when it is asked or when the drive is done
// with the reads before it, whichever is later. A seek, a spin-up and the drive's own buffer are
// not modelled: a read costs its bytes and nothing else. (Without this a game's loading takes no
// time at all, and code that races its loading against the display -- Immercenary's loading tube,
// whose fade must end before the two signals its loading sends -- runs as it never does.)
struct Read { uint32_t ior; uint64_t done; };
static std::vector<Read> g_reads;               // queued reads, in order
static uint64_t g_drive_free;                   // when the drive is done with them
static const uint64_t kDriveBytesPerSecond = 150 * 2048;

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

// The reads the drive has finished by `when`, in order.
static void reads_done(uint64_t when) {
    while (!g_reads.empty() && g_reads.front().done <= when) {
        uint32_t ior = g_reads.front().ior;
        g_reads.erase(g_reads.begin());
        read_blocks(ior);
        pf_complete_io(ior);
    }
}

// The file driver's drv_AbortIO (23.10's 0xe9c: the File's filesystem's own, a CD's 0x18ec), with
// interrupts off: a read still in the filesystem's queue gets the folio's ABORTED (0xD556F00A),
// leaves the queue and is completed. The one the drive is reading the folio stops at the drive
// instead (the CD device's AbortIO of its own request, whose end then completes this one): here
// it is ended the same way, and the drive is free at once -- the reads after it start now.
static void file_abort(uint32_t ior) {
    size_t i = 0;
    while (i < g_reads.size() && g_reads[i].ior != ior) ++i;
    if (i == g_reads.size()) return;
    g_reads.erase(g_reads.begin() + (long)i);
    pf_w32(ior + IO_ERROR, FERR_ABORTED);
    pf_complete_io(ior);
    if (i == 0) {
        g_drive_free = pf_now();
        for (Read& r : g_reads) {
            uint32_t len = pf_r32(r.ior + IOI_RECV_LEN);
            g_drive_free += (uint64_t)len * 1000000000ull / kDriveBytesPerSecond;
            r.done = g_drive_free;
            pf_at(r.done, reads_done);
        }
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
    if (g_pf_trace >= 2)
        pf_log("        read %u blocks from block %u of \"%s\" into %08X\n", len / bs, pf_r32(ior + IOI_OFFSET),
               g_places[file].c_str(), pf_r32(ior + IOI_RECV_BUF));
    uint64_t start = std::max(pf_now(), g_drive_free);
    g_drive_free = start + (uint64_t)len * 1000000000ull / kDriveBytesPerSecond;
    g_reads.push_back({ior, g_drive_free});
    pf_at(g_drive_free, reads_done);
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

// swi 0x30008: Item GetDirectory(char* pathBuf, int pathBufLen) -- 0x3598: with a buffer, the
// buffer checked (ValidateMem for the current task, its error returned) and the current
// directory's path written into it (0x32c0); the directory's item. The path is the names from
// the folio's root down, each after a '/', and "" for the root itself: their lengths, a '/' each
// and the NUL are counted first, and a path that does not fit is -1 with nothing written. The
// folio's root is "/" here (above), so a path never names the mounted filesystem -- the console
// would say "/cd-rom/..." -- and ChangeDirectory takes it back to the same place.
static int32_t get_directory(uint32_t buf, int32_t len) {
    uint32_t dir = file_node(g_cwd), root = file_node("/");
    if (!buf) return (int32_t)pf_r32(dir + 24);
    int32_t err = pf_task_can_write(pf_current_task(), buf, len);
    if (err) return err;
    auto name_len = [](uint32_t f) {
        uint32_t n = 0;
        while (pf_r8(f + FI_NAME + n)) ++n;
        return (int32_t)n;
    };
    int32_t need = 1;
    for (uint32_t f = dir; f && f != root; f = pf_r32(f + FI_PARENT)) need += name_len(f) + 1;
    if (need > len) return -1;
    pf_w8(buf + (uint32_t)--need, 0);
    for (uint32_t f = dir; f && f != root; f = pf_r32(f + FI_PARENT)) {
        int32_t n = name_len(f);
        need -= n;
        for (int32_t i = 0; i < n; ++i) pf_w8(buf + (uint32_t)(need + i), pf_r8(f + FI_NAME + (uint32_t)i));
        pf_w8(buf + (uint32_t)--need, '/');
    }
    return (int32_t)pf_r32(dir + 24);
}

static void f_getdirectory(ArmCpu& c) {
    c.r[0] = (uint32_t)get_directory(c.r[0], (int32_t)c.r[1]);
    if (g_pf_trace) pf_log("        GetDirectory -> %s %08X\n", g_cwd.c_str(), c.r[0]);
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

// The streams for the OS's own code: the program's calls, on a copy of its registers.
static uint32_t stream_call(const ArmCpu& c, uint32_t below, void (*fn)(ArmCpu&), uint32_t r0, uint32_t r1,
                            uint32_t r2) {
    ArmCpu s = c;
    s.r[13] = c.r[13] - below;
    s.r[0] = r0;
    s.r[1] = r1;
    s.r[2] = r2;
    fn(s);
    return s.r[0];
}
uint32_t pf_stream_open(const ArmCpu& c, uint32_t below, uint32_t name, int32_t bsize) {
    return stream_call(c, below, f_opendiskstream, name, (uint32_t)bsize, 0);
}
int32_t pf_stream_read(const ArmCpu& c, uint32_t below, uint32_t st, uint32_t dst, int32_t n) {
    return (int32_t)stream_call(c, below, f_readdiskstream, st, dst, (uint32_t)n);
}
int32_t pf_stream_seek(const ArmCpu& c, uint32_t below, uint32_t st, int32_t offset, uint32_t whence) {
    return (int32_t)stream_call(c, below, f_seekdiskstream, st, (uint32_t)offset, whence);
}
void pf_stream_close(const ArmCpu& c, uint32_t below, uint32_t st) {
    stream_call(c, below, f_closediskstream, st, 0, 0);
}

// ---- code loaded and run ---------------------------------------------------------------------
// 23.10's File folio (the third AIF in its os_code, at 0x10064; the ROM's 1993 folio has none of
// these vectors) loads code with one routine, 0x6fac(path, priority, program, CodeHandle*):
// LoadCode is it with no priority and not a program (0x7498). Step for step: the file opened, an
// IOReq on it; a buffer of one block (MEMTYPE_DMA, the task's lists; of the blocks that make 128
// bytes when a block is smaller) read with DoIO -- its error, else the IOReq's io_Error, stops
// it; an AIF image or the folio's own error (0xD57B9118): at least 128 bytes read, `swi 0x11` at
// 0x10, ro at least 128, rw and bss not negative. The image needs ro + rw + bss (rounded up to 16)
// or the file's bytes, the larger: for code that many and 16 more from the task's lists (DMA),
// the image 16 bytes in -- the allocator's first word, the block's length, is what UnloadCode
// gives back. The first block is copied in; the rest read straight into place, but for a last
// block that would run past the image, read into the buffer and its part copied. The image past
// the file's bytes is filled with memset's 0x0d (the folio passes 0xdeadf00d); the buffer given
// back, the IOReq deleted, the file closed, and the image (or 0) left in the CodeHandle; 0, or
// the first error -- the image given back on one.
//
// The code is recompiled: the image just read, before it has relocated itself, is the module whose
// read-only area it is (arm_identify), and is loaded there (arm_load) -- its code's every address
// is then the image's own. Code no module matches stops the run.
enum : uint32_t { LOADERR_NOTAIF = 0xD57B9118u, KERR_NOMEM = 0xD57B9006u, MEMSET_FILL = 0x0Du };

static std::map<uint32_t, const ArmModule*> g_loaded_code;     // image -> its module

// The image just read, before it has relocated itself, as the recompiled module whose read-only
// area it is, loaded there; false (and said) when no module matches or that one is loaded already.
static bool identify(const char* path, uint32_t img) {
    const ArmModule* m = arm_identify(img);
    if (!m) {
        std::fprintf(stderr, "\"%s\": no recompiled module matches this code\n", path);
        return false;
    }
    if (!arm_load(m, img)) {
        std::fprintf(stderr, "\"%s\": module %s is loaded already: not yet\n", path, m->name);
        return false;
    }
    g_loaded_code[img] = m;
    return true;
}

// LoadProgram's half (0x6fac with `program` set). The file is the command line's first word (up to
// a space, at most 255 characters, 0x7178). The image is whole pages of the task's own
// (AllocMemBlocks, MEMTYPE_TASKMEM | MEMTYPE_DMA) as large as it needs, at the block's start, and
// it is the block's length that the reads and the fill run to. Once the IOReq is deleted, a task
// is made of it (CreateItem of a TASKNODE, 0x736c): named as the file is, at the priority given
// (none for LoadProgram's -1: CreateTask's TAG_NOP), its image (CREATETASK_TAG_AIF), the file's
// bytes (_IMAGESZ) and the whole command line (_CMDSTR), and for the File folio the file's
// directory as its current and program directories (0x3000a, 0x3000b); then the file is closed.
// Its item, or the first error -- the pages given back on one (ControlMem's MEMC_GIVE, 0x7448).
static int32_t load(ArmCpu& c, uint32_t cmd, int32_t priority, bool program, uint32_t& image) {
    char path[256];
    pf_cstring(cmd, path, sizeof path);
    image = 0;
    uint32_t lists = pf_r32(pf_current_task() + T_FREEMEMORYLISTS);
    uint32_t info = c.r[13] - 0x6c;                     // the loader's frame: sp + 0x154
    char first_word[256];
    if (program) {
        size_t i = 0;
        while (i < 255 && path[i] && path[i] != ' ') {
            first_word[i] = path[i];
            ++i;
        }
        first_word[i] = 0;
    }
    const char* name = program ? first_word : path;
    int32_t file = open_disk_file(name);
    if (file < 0) return file;
    int32_t ior = pf_create_ioreq(c, file);
    int32_t err = 0;
    uint32_t img = 0, need = 0, bytes = 0, f = pf_r32(pf_item_node(file) + OFI_FILE);
    if (ior < 0) {
        err = ior;
    } else {
        uint32_t bs = pf_r32(f + FI_BLOCKSIZE), count = pf_r32(f + FI_BLOCKCOUNT), chunk = 1;
        bytes = pf_r32(f + FI_BYTECOUNT);
        uint32_t ioreq = pf_item_node(ior);
        if (bytes < 0x80) {
            err = (int32_t)LOADERR_NOTAIF;
        } else {
            if (bs < 0x80) chunk = (bs + 0x7f) / bs;
            uint32_t first = bs * chunk, offset = 0;
            uint32_t buf = pf_alloc_mem(lists, (int32_t)first, MEMTYPE_DMA, true);
            if (!buf) {
                err = (int32_t)KERR_NOMEM;
            } else {
                auto read = [&](uint32_t to, uint32_t len) {
                    set_info(info, CMD_READ, 0, offset, to, len);
                    int32_t r = do_io(c, ior, info);
                    return r < 0 ? r : (int32_t)pf_r32(ioreq + IO_ERROR);
                };
                err = read(buf, first);
                offset += chunk;
                if (!err) {
                    uint32_t ro = pf_r32(buf + 0x14), rw = pf_r32(buf + 0x18), bss = pf_r32(buf + 0x20);
                    if (pf_r32(ioreq + IO_ACTUAL) < 0x80 || pf_r32(buf + 0x10) != 0xEF000011u ||
                        (int32_t)ro < 0x80 || (int32_t)rw < 0 || (int32_t)bss < 0)
                        err = (int32_t)LOADERR_NOTAIF;
                    else
                        need = std::max(ro + rw + ((bss + 15) & ~15u), bytes);
                }
                if (!err) {
                    if (program) {
                        img = pf_alloc_blocks((int32_t)need, MEMTYPE_DMA | MEMTYPE_TASKMEM);
                        if (img) need = pf_r32(img);
                    } else {
                        img = pf_alloc_mem(lists, (int32_t)(need + 16), MEMTYPE_DMA, true);
                        if (img) img += 16;
                    }
                    if (!img) err = (int32_t)KERR_NOMEM;
                }
                if (!err) {
                    if (need <= first) {
                        copy(img, buf, need);
                    } else {
                        copy(img, buf, first);
                        bool last_apart = bs * count > need;
                        int32_t direct = (int32_t)count - (int32_t)chunk - (last_apart ? 1 : 0);
                        if (direct > 0) {
                            err = read(img + first, bs * (uint32_t)direct);
                            offset += (uint32_t)direct;
                        }
                        if (last_apart && !err) {
                            err = read(buf, bs);
                            copy(img + bs * (count - 1), buf, need % bs);
                        }
                    }
                    if (!err && bytes < need)
                        for (uint32_t i = bytes; i < need; ++i) pf_w8(img + i, MEMSET_FILL);
                }
                pf_free_mem(lists, buf, (int32_t)first);
            }
        }
        pf_delete_item(c, ior);
    }
    int32_t task = 0;
    if (err >= 0 && program) {
        if (!identify(name, img)) std::exit(3);
        if (g_pf_trace)
            pf_log("        LoadProgram \"%s\": module %s at %08X, %u bytes\n", name, g_loaded_code[img]->name, img, need);
        uint32_t dir = pf_r32(pf_r32(f + FI_PARENT) + 24);
        const uint32_t t[] = {1, f + FI_NAME, priority < 0 ? 0xffu : 2u, (uint32_t)priority, 0x13, img, 0x12, bytes,
                              0x14, cmd, 0x3000a, dir, 0x3000b, dir, 0};
        uint32_t tags = pf_os_alloc(sizeof t);
        for (size_t i = 0; i < sizeof t / 4; ++i) pf_w32(tags + 4u * (uint32_t)i, t[i]);
        err = task = (int32_t)pf_create_task(c, tags);
        pf_os_free(tags);
    }
    if (err < 0 && img) {
        if (program) pf_give_pages(img, need);
        else pf_free_mem(lists, img - 16, (int32_t)(need + 16));
        img = 0;
    }
    close_disk_file(c, file);
    if (err >= 0 && img && !program) {
        if (!identify(name, img)) std::exit(3);
        if (g_pf_trace) pf_log("        LoadCode \"%s\": module %s at %08X, %u bytes\n", path, g_loaded_code[img]->name, img, need);
    }
    image = img;
    return program ? (err < 0 ? err : task) : err;
}

// A program's image gone with its task: its recompiled module no longer loaded there.
void pf_unload_image(uint32_t image) {
    auto it = g_loaded_code.find(image);
    if (it == g_loaded_code.end()) return;
    arm_unload(it->second);
    g_loaded_code.erase(it);
}

// File -44: Err LoadCode(char* name, CodeHandle* code)
static void f_loadcode(ArmCpu& c) {
    uint32_t handle = c.r[1], image;
    int32_t err = load(c, c.r[0], -1, false, image);
    if (handle) pf_w32(handle, image);
    c.r[0] = (uint32_t)err;
}

// File -20: Item LoadProgram(char* cmdLine) -- 0x74f4: no priority, the creator's.
static void f_loadprogram(ArmCpu& c) {
    uint32_t image;
    c.r[0] = (uint32_t)load(c, c.r[0], -1, true, image);
}

// File -24: Item LoadProgramPrio(char* cmdLine, int32 priority) -- 0x74e8.
static void f_loadprogramprio(ArmCpu& c) {
    uint32_t image;
    c.r[0] = (uint32_t)load(c, c.r[0], (int32_t)c.r[1], true, image);
}

// File -48: void UnloadCode(CodeHandle code) -- 0x74a8: the block the image is 16 bytes into,
// its length its first word, given back to the task's lists (nothing for 0).
static void f_unloadcode(ArmCpu& c) {
    uint32_t code = c.r[0];
    if (code) {
        uint32_t lists = pf_r32(pf_current_task() + T_FREEMEMORYLISTS);
        pf_free_mem(lists, code - 16, (int32_t)pf_r32(code - 16));
        auto it = g_loaded_code.find(code);
        if (it != g_loaded_code.end()) {
            arm_unload(it->second);
            g_loaded_code.erase(it);
        }
    }
    c.r[0] = 0;
}

// File -52: int32 ExecuteAsSubroutine(CodeHandle code, int32 argc, char** argv) -- 0x7504: no
// code is the kernel's NOMEM. The image's word 0x10, its `swi 0x11`, becomes `ldmia sp!, {r1-r12,
// lr, pc}`; then (0x174) r1-r12, lr and pc pushed, argc in r5, argv in r6, KernelBase in r7, and
// the image entered at its start: its header's four words call, in turn, its decompression, its
// self-relocation (which makes its own word 0x04 a no-op), its zero-init and its entry, and the
// word at 0x10 returns to the caller with the entry's r0, r1-r12 and lr as they were. The header
// is read here, word by word as it stands in memory (code that changes itself is not
// recompiled); what each BL calls is the program's own recompiled code. The pc pushed is the
// folio's own address, which the runtime does not have: 0.
static void f_executeassubroutine(ArmCpu& c) {
    uint32_t code = c.r[0];
    if (!code) { c.r[0] = KERR_NOMEM; return; }
    pf_w32(code + 0x10, 0xE8BDDFFEu);
    uint32_t sp = c.r[13] - 56;
    for (int i = 1; i <= 12; ++i) pf_w32(sp + 4u * (i - 1), c.r[i]);
    pf_w32(sp + 48, c.r[14]);
    pf_w32(sp + 52, 0);
    ArmCpu s = c;
    s.r[13] = sp;
    s.r[5] = c.r[1];
    s.r[6] = c.r[2];
    s.r[7] = pf_folio_base(PF_KERNEL);
    for (uint32_t off = 0; off < 0x10; off += 4) {
        uint32_t w = pf_r32(code + off);
        if (w == 0xE1A00000u) continue;                 // mov r0, r0
        if (w >> 24 != 0xEB) pf_stop(c, "ExecuteAsSubroutine: a header word that is no BL or no-op: not yet");
        uint32_t disp = w & 0xFFFFFF;
        uint32_t t = code + off + 8 + 4 * (disp & 0x800000 ? disp - 0x1000000 : disp);
        s.r[14] = code + off + 4;
        arm_call(s, t);
        if (s.pc != code + off + 4) arm_bad_return(s, code + off + 4);
    }
    if (pf_r32(code + 0x10) != 0xE8BDDFFEu) pf_stop(c, "ExecuteAsSubroutine: the header's return changed");
    for (int i = 1; i <= 12; ++i) c.r[i] = pf_r32(sp + 4u * (i - 1));
    c.r[14] = pf_r32(sp + 48);
    c.r[0] = s.r[0];
    c.n = s.n; c.z = s.z; c.c = s.c; c.v = s.v;
    c.budget = s.budget;
}

// ---- the shell's start -----------------------------------------------------------------------
// The program's aliases come from the shell that starts it: the disc's own (System/Tasks/shell,
// 1993) makes `alias boot /` and the boot filesystem's name, goes to `$boot`, and runs the script
// ^/system/scripts/startopera, which makes aliases and runs other scripts -- a disc's AppStartup
// among them, the place its own aliases go ("alias exdir $boot"). Here the scripts are read for
// their aliases: a line `alias NAME VALUE` makes one, a line naming a script (a file on the disc
// that is not an AIF image) runs it, once; every other line -- a program, a folio, the shell's
// other commands -- is the runtime's own business or none.
//
// A line ends at the first '&', '@' or '%' in it, as the shell cuts it (each with strchr): '&' a
// program sent to the background, and in the 1994 shell (23.10's System/Tasks/shell, 0x544) '@' a
// program loaded once and kept (0x370) and '%' a script (0x704) -- its own start runs
// "^/system/scripts/startopera%", and Immercenary's startopera names "$tasks/eventbroker@" and
// "$boot/AppStartup%". The 1993 shell knows '&' and '#' only.
static void shell_cut(std::string& line) {
    size_t cut = line.find_first_of("&@%");
    if (cut != std::string::npos) line.erase(cut);
}

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
        shell_cut(line);
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

// ---- the shell, running the disc (pfboot --boot) ---------------------------------------------
// The same scripts carried out line by line, as the console's shell does: the words of a line up
// to one beginning with '#' (and to its first '&', '@' or '%', above); `alias NAME VALUE`; the
// shell's own commands that touch nothing here (bg, fg, bgkill, killkprintf, minmem) passed over
// -- `bg` and `fg` set whether the programs after them are sent to the background, a '&' sends
// one, and a '#' anywhere in a line asks for it to be waited for (Crash 'n Burn's "^/ex #": both
// shells cut the line there and clear the flag); every program here is waited for, which is what
// the console shows of those discs -- ; a name walked as the File folio walks it: an AIF
// image is a program -- the OS's own (under /System: the daemons, the folios, the event broker)
// are the runtime's and are not run; any other is run until it ends, then the next line --, any
// other file a script, run there and then (as its last line, in its place: a disc's scripts can
// name each other for ever, as Crash 'n Burn's runme1 and runme2 do). What is none of these is
// reported and passed over.
static int (*g_shell_run)(const std::string& host);
static int g_shell_result;

static bool is_aif_file(const std::string& host) {
    std::ifstream f(host, std::ios::binary);
    char h[0x14] = {};
    f.read(h, sizeof h);
    return f.gcount() == sizeof h && (uint8_t)h[0x10] == 0xEF && (uint8_t)h[0x13] == 0x11;
}

static std::vector<std::vector<std::string>> script_lines(const std::string& at) {
    std::ifstream f(host_of(at), std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::vector<std::vector<std::string>> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find_first_of("\r\n", i);
        if (j == std::string::npos) j = text.size();
        std::string line = text.substr(i, j - i);
        i = j + 1;
        shell_cut(line);
        std::vector<std::string> words;
        for (size_t k = 0; k < line.size();) {
            size_t s = line.find_first_not_of(" \t", k);
            if (s == std::string::npos) break;
            size_t e = line.find_first_of(" \t", s);
            if (e == std::string::npos) e = line.size();
            if (line[s] == '#') break;
            words.push_back(line.substr(s, e - s));
            k = e;
        }
        if (!words.empty()) out.push_back(words);
    }
    return out;
}

static void shell_carry_out(std::string at, int depth) {
    if (depth > 64) {
        std::fprintf(stderr, "the shell: scripts nested more than 64 deep at %s\n", at.c_str());
        std::exit(3);
    }
    for (;;) {
        std::vector<std::vector<std::string>> lines = script_lines(at);
        std::string next;
        for (size_t n = 0; n < lines.size(); ++n) {
            const std::vector<std::string>& w = lines[n];
            const std::string cmd = lower(w[0]);
            if (cmd == "alias") {
                if (w.size() >= 3) g_aliases[0][lower(w[1])] = w[2];
                continue;
            }
            if (cmd == "bg" || cmd == "fg" || cmd == "bgkill" || cmd == "killkprintf" || cmd == "minmem") continue;
            std::string to;
            g_cwd = "/";                                // the shell's own directory, $boot
            if (walk(w[0], to) || is_dir(to)) {
                pf_log("the shell: %s: no such program or script here\n", w[0].c_str());
                continue;
            }
            std::string host = host_of(to).string();
            if (is_aif_file(host)) {
                if (lower(to).rfind("/system/", 0) == 0) continue;     // the OS's own: the runtime's
                pf_log("the shell: %s (%s)\n", w[0].c_str(), to.c_str());
                g_shell_result = g_shell_run(host);
                pf_log("the shell: %s ended, %d\n", w[0].c_str(), g_shell_result);
                continue;
            }
            if (n + 1 == lines.size()) { next = to; break; }           // a script as the last line
            shell_carry_out(to, depth + 1);
        }
        if (next.empty()) return;
        at = next;
    }
}

int pf_shell_boot(int (*run)(const std::string& host)) {
    g_shell_run = run;
    g_cwd = "/";
    g_aliases.clear();
    g_aliases[0]["boot"] = "/";
    std::string at;
    if (!walk("^/system/scripts/startopera", at)) shell_carry_out(at, 0);
    else pf_log("the shell: no ^/system/scripts/startopera\n");
    g_cwd = "/";
    if (!walk("$boot/LaunchMe", at) && !is_dir(at)) {
        pf_log("the shell: $boot/LaunchMe\n");
        g_shell_result = run(host_of(at).string());
    }
    return g_shell_result;
}

void pf_file_init() {
    g_cwd = "/";
    g_files.clear();
    g_places.clear();
    g_aliases.clear();
    g_open_files.clear();
    g_reads.clear();
    g_loaded_code.clear();
    shell_start();
    if (g_pf_trace) {
        pf_log("shell aliases:");
        for (const auto& a : g_aliases[0]) pf_log(" %s=%s", a.first.c_str(), a.second.c_str());
        pf_log("\n");
    }
    pf_on_swi(0x30000, f_opendiskfile);
    pf_on_swi(0x30001, f_closediskfile);
    pf_on_swi(0x30007, f_changedirectory);
    pf_on_swi(0x30008, f_getdirectory);
    pf_on_swi(0x3000b, f_createalias);
    pf_on_slot(PF_FILE, -4, f_opendiskstream);
    pf_on_slot(PF_FILE, -8, f_readdiskstream);
    pf_on_slot(PF_FILE, -12, f_seekdiskstream);
    pf_on_slot(PF_FILE, -16, f_closediskstream);
    pf_on_slot(PF_FILE, -20, f_loadprogram);
    pf_on_slot(PF_FILE, -24, f_loadprogramprio);
    pf_on_slot(PF_FILE, -44, f_loadcode);
    pf_on_slot(PF_FILE, -48, f_unloadcode);
    pf_on_slot(PF_FILE, -52, f_executeassubroutine);
}
