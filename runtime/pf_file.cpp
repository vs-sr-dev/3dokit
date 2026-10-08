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
// names one of them ("/cd-rom"); here "/" is the disc's root, and `$boot` is "/". The other
// filesystems mounted -- the NVRAM's ("/nvram") -- are named at the root before the disc's
// entries; their code (below, "linked-memory filesystems") is the File folio 20.30's, the third
// image of Doctor Hauzer's os_code, with its addresses.
#include "pf.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
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
    IOI_COMMAND = 0x34, IOI_OFFSET = 0x40, IOI_SEND_BUF = 0x44, IOI_SEND_LEN = 0x48, IOI_RECV_BUF = 0x4c,
    IOI_RECV_LEN = 0x50,
    IO_DONE = 1, IO_QUICK = 2, SIGF_IODONE = 8, MESSAGENODE = 9,
    CMD_WRITE = 0, CMD_READ = 1, CMD_STATUS = 2, FILECMD_GETPATH = 4,
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

// The linked-memory filesystems (the NVRAM, below): a place "/NAME" and what is under it, when
// NAME is a mounted one's, is on one, and not on the disc.
struct LmFs;
static LmFs* lm_of(const std::string& at);
static std::string lm_top(const std::string& name);         // "/NAME" of the one so named, or ""
static std::string lm_child(const std::string& dir, const std::string& name);
static void lm_run(LmFs& d, uint32_t ior, uint32_t file);
static uint32_t file_node(const std::string& at);

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
// kernel's FindNamedNode matches); "" when there is none. At the root a mounted filesystem's name
// comes first (the folio's root holds the filesystems: 20.30's walk, 0x3078), and a linked-memory
// directory's entries are its own (lm_child).
static std::string child_of(const std::string& at, const std::string& name) {
    if (at == "/") {
        std::string top = lm_top(name);
        if (!top.empty()) return top;
    }
    if (lm_of(at)) return lm_child(at, name);
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
    if (lm_of(at)) return pf_r32(file_node(at) + 0x54) & 1;     // fi_Flags' FILE_IS_DIRECTORY
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
    if (lm_of(at)) {
        std::fprintf(stderr, "File: \"%s\" has no File (a linked-memory filesystem's are made by its walk)\n", at.c_str());
        std::exit(3);
    }
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
// alternatives a name can hold ({a|b}) stop the run: not yet. In a linked-memory filesystem "^" is
// its root (0x2f44: the directory's filesystem's root).
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
        if (name == "^") { at = lm_of(at) ? at.substr(0, at.find('/', 1)) : "/"; continue; }
        if (!is_dir(at)) return (int32_t)FERR_NOTADIRECTORY;
        std::string next = child_of(at, name);
        if (next.empty()) return (int32_t)FERR_NOFILE;
        at = next;
    }
    return 0;
}

std::string pf_host_path(const char* path) {
    std::string at;
    if (walk(path, at) || lm_of(at)) return "";
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
// stack past the status, here nothing; io_Actual stays 0; CompleteIO, and 0. That is the 1993
// folio's (20.19, Crash 'n Burn's os_code too); from 20.30 (0x134c; 23.10's 0x1198 the same) the
// copy is as long as ioi_Recv, 0x28 at most. FILECMD_GETPATH (4)
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
    uint32_t file = pf_r32(pf_r32(ior + IO_DEV) + OFI_FILE);
    LmFs* lm = lm_of(g_places[file]);
    if (cmd > (lm ? 11u : 10u)) return (int32_t)FERR_BADCOMMAND;
    if (cmd == CMD_STATUS) {
        uint32_t buf = pf_r32(ior + IOI_RECV_BUF), flags = pf_r32(file + FI_FLAGS), n = FILESTATUS_SIZE;
        if (pf_os_code_version(2) >= PF_VERSION(20, 30)) n = std::min(n, pf_r32(ior + IOI_RECV_LEN));
        uint32_t st[FILESTATUS_SIZE / 4] = {0x05000300u, FILESTATUS_SIZE, pf_r32(file + FI_BLOCKSIZE),
                                            pf_r32(file + FI_BLOCKCOUNT), flags,
                                            flags & FILE_IS_READONLY ? 0x20000000u : 0, 0, 0, 0,
                                            pf_r32(file + FI_BYTECOUNT)};
        for (uint32_t i = 0; i < n; ++i) pf_w8(buf + i, st[i / 4] >> (24 - 8 * (i % 4)) & 0xff);
        pf_complete_io(ior);
        return 0;
    }
    if (cmd == FILECMD_GETPATH) {
        std::fprintf(stderr, "File: FILECMD_GETPATH: not yet\n");
        std::exit(3);
    }
    if (lm) {
        // 20.30's dispatch (0x109c, 0x1264) and the filesystem's queue (0x4b30): READ and WRITE of
        // whole blocks inside the file (else BADPTR, as the CD's), READDIR and READENTRY with a
        // buffer to receive into (else BADPTR), OPENENTRY done at once; a refusal is io_Error,
        // CompleteIO and SendIO's result. The request queued (IO_QUICK cleared), then run to its
        // end (lm_run) and completed.
        uint32_t bs = pf_r32(file + FI_BLOCKSIZE);
        if (cmd == CMD_READ || cmd == CMD_WRITE) {
            uint32_t len = pf_r32(ior + (cmd == CMD_READ ? IOI_RECV_LEN : IOI_SEND_LEN));
            if (len % bs || (pf_r32(file + FI_BLOCKCOUNT) - pf_r32(ior + IOI_OFFSET)) * bs < len)
                return refuse(ior, FERR_BADPTR);
        } else if (cmd == 3 || cmd == 5) {
            if (!pf_r32(ior + IOI_RECV_BUF)) return refuse(ior, FERR_BADPTR);
        } else if (cmd == 11) {
            pf_complete_io(ior);
            return 0;
        }
        pf_w32(ior + IO_FLAGS, pf_r32(ior + IO_FLAGS) & ~IO_QUICK);
        lm_run(*lm, ior, file);
        pf_complete_io(ior);
        return 0;
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

// ---- linked-memory filesystems: the NVRAM ----------------------------------------------------
// The NVRAM holds a linked-memory filesystem: its blocks in a ring, each beginning with a header --
// a fingerprint, the next block and the one before it, the block's length in blocks (its header's
// own included) and its header's -- and, in a file's, the entry: the file's bytes, an identifier,
// a type and a name of 32 characters; 0x40 bytes in all. The disc's own FORMAT
// (System/Programs, which LMADM runs on a blank NVRAM) lays a fresh one out: a label at the first
// block (a DiscLabel of 0x84 bytes, version 2), an anchor block after it -- the root directory's
// one avatar, which the label names -- and one free block for the rest, the anchor and the free
// block each the other's next and previous.
//
// What follows is the File folio 20.30's (os_code's third image, linked at 0), with its addresses:
// the mount, the walk through such a directory, CreateFile and DeleteFile, and the filesystem's own
// requests. A request is a run of steps (0x513c), each one transfer through the ram device -- a
// block's header into or out of one of two buffers, or file data -- after which the next step
// is taken (the request's end action, 0x4ff8). On the console the folio's daemon sends each
// transfer and the device does it; the NVRAM is memory, which its driver copies at once, and here
// the transfers are done one after the other there and then, and the request is complete when
// SendIO returns. The device's state from one request to the next and its buffers -- two headers
// and 0x100 bytes of data -- are kept as the folio keeps them (LinkedMemDisk +0x108 to +0x1c8): a
// header written back is written whole, fields left from an earlier request and all.
//
// The mount makes a FileSystem node and the root directory's File; the high-level device the
// folio also makes (its LinkedMemDisk) and its IOReq are not made here: their state is LmFs's.
enum : uint32_t {
    LM_FILE = 0xBE4F32A6u, LM_FREE = 0x7AA565BDu, LM_ANCHOR = 0x855A02B6u,     // the blocks' fingerprints
    H_FP = 0x00, H_FLINK = 0x04, H_BLINK = 0x08, H_COUNT = 0x0c, H_HDR = 0x10, H_BYTES = 0x14, H_UID = 0x18,
    H_TYPE = 0x1c, H_NAME = 0x20, H_SIZE = 0x40,
    // FileSystem (filesystem.h, as 20.30 fills it)
    FILESYSTEMNODE = 1, FS_SIZE = 0x64, FS_NAME = 0x24, FS_FLAGS = 0x48, FS_VOLBLOCKSIZE = 0x4c,
    FS_VOLBLOCKCOUNT = 0x50, FS_VOLID = 0x54, FS_RATIO = 0x5c, FS_ROOT = 0x60,
    // File's other fields (20.30)
    FI_FILESYSTEM = 0x44, FI_UNIQUEID = 0x4c, FI_TYPE = 0x50, FI_BURST = 0x68, FI_GAP = 0x6c, FI_LASTAVATAR = 0x70,
    FI_AVATAR = 0x78, FILE_TYPE_DIRECTORY = 0x2a646972,                   // '*dir'
    LM_ROOT_FLAGS = 0x2d,   // a directory, the filesystem's, that can be scanned and has entries
    // the label (discdata.h's DiscLabel)
    DL_SIZE = 0x84, DL_VERSION = 0x06, DL_ID = 0x28, DL_VOLID = 0x48, DL_BLOCKSIZE = 0x4c, DL_BLOCKCOUNT = 0x50,
    DL_ROOTBLOCKCOUNT = 0x58, DL_ROOTBLOCKSIZE = 0x5c, DL_LASTAVATAR = 0x60, DL_AVATARS = 0x64,
    // the commands
    FILECMD_READDIR = 3, FILECMD_READENTRY = 5, FILECMD_ALLOCBLOCKS = 6, FILECMD_SETEOF = 7,
    FILECMD_ADDENTRY = 8, FILECMD_DELETEENTRY = 9, FILECMD_SETTYPE = 10, FILECMD_OPENENTRY = 11,
    DE_SIZE = 0x48,                                     // a DirectoryEntry (directory.h)
    FERR_BADIOARG = 0xD556F00Du, FERR_NOFILESYSTEM = 0xD556F103u, FERR_BUSY = 0xD556F10Bu,
    FERR_DUPLICATE = 0xD556F10Cu, FERR_READONLY = 0xD556F10Du,
    KERR_BADIOARG = 0xD57B900Du,                        // the ram driver's
    RAM_NVRAM_UNIT = 3,
};

struct LmFs {
    std::string name;                   // the label's volume identifier: the filesystem's name
    uint32_t node = 0, root = 0;        // its FileSystem node, its root directory's File
    uint32_t unit = 0, offset = 0;      // the ram device's unit, and the filesystem's first block there
    uint32_t bs = 0, blocks = 0;        // the device's block size and count (+0xfc, +0x100)
    uint32_t chunk_max = 0, hdr = 0;    // 0x100 / bs, and a header's blocks: (bs + 0x3f) / bs (+0x138, +0x13c)
    // the device's state (+0x108 to +0x144): the last entry READDIR found and its index, the File of
    // the request, the headers' blocks (A's and B's), an entry's index, a block saved, where a
    // search began, the blocks wanted, data's place, blocks left and blocks a transfer, the step
    uint32_t dir_block = 0, dir_index = 0, file = 0, a = 0, count = 0, b = 0, saved = 0, start = 0;
    uint32_t want = 0, at = 0, left = 0, chunk = 0, state = 0;
    uint8_t ha[H_SIZE] = {}, hb[H_SIZE] = {}, data[0x100] = {};      // +0x148, +0x188, +0x1c8
    int last_io = 0;                    // the device's request's last transfer
};
static std::vector<std::unique_ptr<LmFs>> g_lm;     // mounted, in the folio's list's order
static uint32_t g_next_id;                          // the folio's identifier for an entry without one (+0xf4)

static uint32_t hw(const uint8_t* h, uint32_t o) {
    return (uint32_t)h[o] << 24 | (uint32_t)h[o + 1] << 16 | (uint32_t)h[o + 2] << 8 | h[o + 3];
}
static void hs(uint8_t* h, uint32_t o, uint32_t v) {
    for (uint32_t i = 0; i < 4; ++i) h[o + i] = (uint8_t)(v >> (24 - 8 * i));
}

// The folio's strncpy (0x6b54): up to n bytes, those after the string's end 0.
static void lm_strncpy(uint8_t* d, const uint8_t* s, uint32_t n) {
    bool end = false;
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = end ? 0 : s[i];
        if (!s[i]) end = true;
    }
}
static void lm_strncpy_guest(uint8_t* d, uint32_t s, uint32_t n) {
    bool end = false;
    for (uint32_t i = 0; i < n; ++i) {
        d[i] = end ? 0 : (uint8_t)pf_r8(s + i);
        if (!d[i]) end = true;
    }
}

// The folio's comparison of names (0x6cc0), each character through its toupper (0x6f30); here
// within the 32 bytes of a header's name.
static bool lm_same(const uint8_t* x, const uint8_t* y) {
    for (uint32_t i = 0; i < 32; ++i) {
        if (std::toupper(x[i]) != std::toupper(y[i])) return false;
        if (!x[i]) return true;
    }
    return true;
}

// The linked-memory filesystem a place is on ("/NAME" and below, NAME a mounted one's), or null.
static LmFs* lm_of(const std::string& at) {
    if (at.size() < 2 || at[0] != '/') return nullptr;
    size_t e = at.find('/', 1);
    std::string first = at.substr(1, e == std::string::npos ? std::string::npos : e - 1);
    for (auto& fs : g_lm)
        if (fs->name == first) return fs.get();
    return nullptr;
}

static std::string lm_top(const std::string& name) {
    for (auto& fs : g_lm)
        if (same_name(fs->name, name)) return "/" + fs->name;
    return "";
}

// A transfer of the folio's request to the ram device's unit (pf_nvram.cpp's driver: the request
// has a callback, so the NVRAM takes a write): `len` bytes at block `block`; the driver's error
// (BADIOARG for a block or a length outside the unit), else 0.
static uint32_t lm_dev(const LmFs& d, bool write, int32_t block, uint8_t* p, int32_t len) {
    std::vector<uint8_t>& nv = pf_nvram();
    if (block < 0) return KERR_BADIOARG;
    if (len == 0) return 0;
    if (len < 0 || (int32_t)((uint32_t)block * d.bs + (uint32_t)len) > (int32_t)(d.blocks * d.bs)) return KERR_BADIOARG;
    uint32_t at = (uint32_t)block * d.bs;
    if (write) {
        std::memcpy(&nv[at], p, (size_t)len);
        pf_nvram_written();
    } else {
        std::memcpy(p, &nv[at], (size_t)len);
    }
    return 0;
}

// A step's transfer (0x58e0-0x59ec): 1 a header into A from block a, 2 into B from block b, 3 A
// to a, 4 B to b, 0x40 bytes each; 5 `chunk` blocks of data into the buffer from a + at, 6 from
// it to b + at. The blocks are the device's own: the filesystem's first block is not added (the
// NVRAM's is 0).
static uint32_t lm_transfer(LmFs& d, int io) {
    switch (io) {
    case 1: return lm_dev(d, false, (int32_t)d.a, d.ha, H_SIZE);
    case 2: return lm_dev(d, false, (int32_t)d.b, d.hb, H_SIZE);
    case 3: return lm_dev(d, true, (int32_t)d.a, d.ha, H_SIZE);
    case 4: return lm_dev(d, true, (int32_t)d.b, d.hb, H_SIZE);
    case 5: return lm_dev(d, false, (int32_t)(d.a + d.at), d.data, (int32_t)(d.chunk * d.bs));
    case 6: return lm_dev(d, true, (int32_t)(d.b + d.at), d.data, (int32_t)(d.chunk * d.bs));
    }
    return 0;
}

// One step (0x513c), by the device's state: what it does to the headers and the request, the
// transfer it asks for (`io`, 0 for none) and the state after it (2 unless said). 1 while the
// request goes on, 0 when it is done, or its error. The steps, as the folio's switch has them:
//   3, 4, 5: grow a file where it is (ALLOCBLOCKS): A its block, B the next one -- after it, free
//      and long enough for the blocks wanted -- taken into A; else a free block found (0xb);
//   6 to 0xa: the block A, now a file's, cut to the blocks wanted when 0x60 or more are left over
//      (the rest a free block after it, its neighbours' links mended), and the File's block count
//      its length less the header; then a block left behind, if any, freed (0x11);
//   0xb, 0xc: the first free block long enough, from the anchor round (else the folio's 0xD556F009);
//      it becomes the file's: for ADDENTRY a new entry -- no bytes, identifier 0, type "    ",
//      the name --, for ALLOCBLOCKS the file's entry, and its data copied over (0xd, 0xe, 0x100
//      bytes at a time) and the old block given up (0xf: A and B change places, the File's avatar
//      is the new block);
//   0x11 to 0x16: block A freed, back to the first free block before it and every free block
//      after that joined to it;
//   0x17, 0x18: the directory's entries from the last one READDIR found (or from the anchor) round
//      to where it began: READDIR's by index, READENTRY's and DELETEENTRY's by name (NOFILE when
//      none); a DirectoryEntry for the caller, or the entry freed (0x11);
//   0x19, 0x1a: SETEOF, the entry's bytes and the File's (the File the last request was on: the
//      folio does not set it for this one); 0x1b, 0x1c: SETTYPE, the entry's type.
static int32_t lm_step(LmFs& d, uint32_t ior, int& io) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND), file = d.file, next = 2;
    int32_t ret = 1;
    io = 0;
    auto anchor = [&] { return pf_r32(pf_r32(pf_r32(file + FI_FILESYSTEM) + FS_ROOT) + FI_AVATAR); };
    auto find_free = [&] { d.b = anchor(); io = 2; next = 0xc; };                     // 0x544c
    auto free_a = [&] { d.dir_block = d.dir_index = 0; hs(d.ha, H_FP, LM_FREE); io = 3; next = 0x12; };  // 0x5634
    auto next_entry = [&] {                                                         // 0x5884
        d.a = hw(d.ha, H_FLINK);
        if (d.a == d.start) {
            ret = (int32_t)FERR_NOFILE;
            next = 1;
        } else {
            io = 1;
            next = 0x18;
        }
    };
    auto found_entry = [&] {                                                        // 0x57f4
        ret = 0;
        uint8_t de[DE_SIZE] = {};
        hs(de, 0x04, hw(d.ha, H_UID));
        hs(de, 0x08, hw(d.ha, H_TYPE));
        hs(de, 0x0c, pf_r32(file + FI_BLOCKSIZE));
        hs(de, 0x10, hw(d.ha, H_BYTES));
        hs(de, 0x14, hw(d.ha, H_COUNT) - hw(d.ha, H_HDR));
        hs(de, 0x18, 0xFFFFFFFFu);
        hs(de, 0x1c, 0xFFFFFFFFu);
        hs(de, 0x20, 1);
        lm_strncpy(de + 0x24, d.ha + H_NAME, 0x20);
        hs(de, 0x44, d.a);
        uint32_t n = std::min<uint32_t>(pf_r32(ior + IOI_RECV_LEN), DE_SIZE);
        for (uint32_t i = 0; i < n; ++i) pf_w8(pf_r32(ior + IOI_RECV_BUF) + i, de[i]);
        pf_w32(ior + IO_ACTUAL, n);
    };
    switch (d.state) {
    case 0: case 1:                                                                 // 0x52a0
        next = 0;
        ret = 0;
        break;
    case 2:                                                                         // 0x52ac
        ret = 0;
        break;
    case 3:                                                                         // 0x52b8
        d.saved = 0;
        if (cmd != FILECMD_ADDENTRY) {
            io = 1;
            next = 4;
        } else {
            find_free();
        }
        break;
    case 4:                                                                         // 0x52d8
        if (hw(d.ha, H_FLINK) >= d.a) {
            d.b = hw(d.ha, H_FLINK);
            io = 2;
            next = 5;
        } else {
            find_free();
        }
        break;
    case 5:                                                                         // 0x52f8
        if (hw(d.hb, H_FP) != LM_FREE || d.want - hw(d.ha, H_COUNT) > hw(d.hb, H_COUNT)) {
            find_free();
            break;
        }
        hs(d.ha, H_COUNT, hw(d.ha, H_COUNT) + hw(d.hb, H_COUNT));
        hs(d.ha, H_FLINK, hw(d.hb, H_FLINK));
        io = 3;
        next = 6;
        break;
    case 6: {                                                                       // 0x5340
        if (cmd == FILECMD_ADDENTRY) {
            std::memcpy(d.ha, d.hb, H_SIZE);
            d.a = d.b;
        }
        uint32_t excess = hw(d.ha, H_COUNT) - d.want;
        if (excess < 0x60) {
            d.b = hw(d.ha, H_FLINK);
            io = 2;
            next = 9;
        } else {
            d.b = d.a + d.want;
            hs(d.hb, H_FP, LM_FREE);
            hs(d.hb, H_COUNT, excess);
            hs(d.hb, H_FLINK, hw(d.ha, H_FLINK));
            hs(d.hb, H_BLINK, d.a);
            hs(d.hb, H_HDR, 0);
            io = 4;
            next = 7;
        }
        break;
    }
    case 7:                                                                         // 0x53c8
        hs(d.ha, H_COUNT, d.want);
        hs(d.ha, H_FLINK, d.b);
        io = 3;
        next = 8;
        break;
    case 8:                                                                         // 0x53e4
        d.b = hw(d.hb, H_FLINK);
        io = 2;
        next = 9;
        break;
    case 9:                                                                         // 0x53f8
        hs(d.hb, H_BLINK, d.b == hw(d.ha, H_FLINK) ? d.a : hw(d.ha, H_FLINK));
        io = 4;
        next = 0xa;
        break;
    case 0xa:                                                                       // 0x5418
        pf_w32(file + FI_BLOCKCOUNT, hw(d.ha, H_COUNT) - d.hdr);
        if (!d.saved) {
            next = 1;
            ret = 0;
        } else {
            d.a = d.saved;
            io = 1;
            next = 0x11;
        }
        break;
    case 0xb:                                                                       // 0x544c
        find_free();
        break;
    case 0xc:                                                                       // 0x5468
        if (hw(d.hb, H_FP) != LM_FREE || hw(d.hb, H_COUNT) < d.want) {
            d.b = hw(d.hb, H_FLINK);
            if (anchor() == d.b) {
                ret = (int32_t)FERR_BADPTR;                 // the folio's own, for a filesystem full
            } else {
                io = 2;
                next = 0xc;
            }
            break;
        }
        hs(d.hb, H_FP, LM_FILE);                                                    // 0x54b8
        hs(d.hb, H_HDR, d.hdr);
        if (cmd == FILECMD_ALLOCBLOCKS) {                                           // 0x550c
            hs(d.hb, H_BYTES, hw(d.ha, H_BYTES));
            hs(d.hb, H_TYPE, hw(d.ha, H_TYPE));
            hs(d.hb, H_UID, hw(d.ha, H_UID));
            lm_strncpy(d.hb + H_NAME, d.ha + H_NAME, 0x20);
            next = 0xd;
            d.at = hw(d.hb, H_HDR);
            d.left = (hw(d.hb, H_BYTES) + d.bs - 1) / d.bs;
            if ((int32_t)d.left <= 0) next = 0xf;
            io = 4;
            d.chunk = 0;
        } else if (cmd == FILECMD_ADDENTRY) {
            hs(d.hb, H_BYTES, 0);
            hs(d.hb, H_TYPE, 0x20202020u);
            hs(d.hb, H_UID, 0);
            lm_strncpy_guest(d.hb + H_NAME, pf_r32(ior + IOI_SEND_BUF), 0x20);
            io = 4;
            next = 6;
        }
        break;
    case 0xd:                                                                       // 0x556c
        d.at += d.chunk;
        d.chunk = d.left;
        if ((int32_t)d.chunk > (int32_t)d.chunk_max) d.chunk = d.chunk_max;
        io = 5;
        next = 0xe;
        break;
    case 0xe:                                                                       // 0x55a0
        io = 6;
        d.left -= d.chunk;
        next = (int32_t)d.left > 0 ? 0xd : 0xf;
        break;
    case 0xf: {                                                                     // 0x55c8
        uint8_t t[H_SIZE];
        std::memcpy(t, d.ha, H_SIZE);
        std::memcpy(d.ha, d.hb, H_SIZE);
        std::memcpy(d.hb, t, H_SIZE);
        hs(d.hb, H_FP, LM_FREE);
        d.saved = d.a;
        uint32_t was_b = d.b;
        d.b = d.a;
        d.a = was_b;
        pf_w32(file + FI_AVATAR, d.a);
        pf_w32(file + FI_BLOCKCOUNT, hw(d.ha, H_COUNT) - d.hdr);
        io = 4;
        next = 6;
        break;
    }
    case 0x10:                                                                      // 0x5440
        io = 1;
        next = 0x11;
        break;
    case 0x11:
        free_a();
        break;
    case 0x12:                                                                      // 0x5654
        if (hw(d.ha, H_FP) == LM_FREE) {
            d.a = hw(d.ha, H_BLINK);
            next = 0x12;
        } else {
            d.a = hw(d.ha, H_FLINK);
            next = 0x13;
        }
        io = 1;
        break;
    case 0x13:                                                                      // 0x567c
        d.b = hw(d.ha, H_FLINK);
        io = 2;
        next = 0x14;
        break;
    case 0x14:                                                                      // 0x5690
        if (hw(d.hb, H_FP) == LM_FREE) {
            hs(d.ha, H_COUNT, hw(d.ha, H_COUNT) + hw(d.hb, H_COUNT));
            hs(d.ha, H_FLINK, hw(d.hb, H_FLINK));
            d.b = hw(d.hb, H_FLINK);
            io = 2;
            next = 0x14;
        } else {
            hs(d.hb, H_BLINK, d.a);
            io = 4;
            next = 0x15;
        }
        break;
    case 0x15:                                                                      // 0x56e0
        io = 3;
        next = 0x16;
        break;
    case 0x16:                                                                      // 0x56ec
        ret = 0;
        break;
    case 0x17:                                                                      // 0x56f8
        d.count = d.dir_index;
        d.a = d.dir_block;
        if ((int32_t)d.a <= 0 || (int32_t)d.count <= 0) {
            d.a = anchor();
            d.count = 0;
        }
        d.start = d.a;
        io = 1;
        next = 0x18;
        break;
    case 0x18: {                                                                    // 0x5748
        uint32_t fp = hw(d.ha, H_FP);
        if (fp == LM_ANCHOR) {
            d.count = 0;
            next_entry();
            break;
        }
        if (fp != LM_FILE) {
            next_entry();
            break;
        }
        if (d.a != d.dir_block) {
            ++d.count;
            d.dir_index = d.count;
            d.dir_block = d.a;
        }
        if (cmd == FILECMD_DELETEENTRY && lm_same(d.ha + H_NAME, d.data)) free_a();
        else if (cmd == FILECMD_READDIR && d.count == pf_r32(ior + IOI_OFFSET)) found_entry();
        else if (cmd == FILECMD_READENTRY && lm_same(d.ha + H_NAME, d.data)) found_entry();
        else next_entry();
        break;
    }
    case 0x19:                                                                      // 0x58b0
        io = 1;
        next = 0x1a;
        break;
    case 0x1a: {                                                                    // 0x58bc
        uint32_t v = pf_r32(ior + IOI_OFFSET);
        pf_w32(file + FI_BYTECOUNT, v);
        hs(d.ha, H_BYTES, v);
        io = 3;
        next = 1;
        break;
    }
    case 0x1b:                                                                      // 0x58d4
        io = 1;
        next = 0x1c;
        break;
    case 0x1c:                                                                      // 0x5218
        hs(d.ha, H_TYPE, pf_r32(ior + IOI_OFFSET));
        io = 3;
        next = 1;
        break;
    }
    d.state = next;
    return ret;
}

// A request to the filesystem, on `file` (the open file's File), started (0x4cd0) and run to its
// end; its io_Error and io_Actual are the request's, and the caller completes it. READ and WRITE
// (0x4d90) are the caller's own transfer, to or from its buffer, at the file's data on the device
// -- its avatar plus ioi_Offset, in the filesystem's blocks, past the header, from the
// filesystem's first block -- and the request's io_Actual is what the device's was (a write to
// the NVRAM leaves it 0). The others set the device's state and take the first step:
// READDIR, READENTRY and DELETEENTRY (the name, 32 characters at most, in the data buffer cleared
// first) step 0x17; ALLOCBLOCKS step 3, the blocks wanted the File's, ioi_Offset's and a header's,
// from the file's avatar; SETEOF (when the File's blocks hold ioi_Offset bytes, else 0xD556F00D)
// step 0x19; ADDENTRY step 3, a header's blocks wanted; SETTYPE step 0x1b; any other is
// BADCOMMAND. Then each transfer done and the next step taken until one is the last (0x4ff8): a
// request whose last transfer was asked with step 1 after it is done without looking at that
// transfer's error; another transfer's error is the request's.
static void lm_run(LmFs& d, uint32_t ior, uint32_t file) {
    uint32_t cmd = pf_r8(ior + IOI_COMMAND);
    if (cmd == CMD_READ || cmd == CMD_WRITE) {
        d.state = 0;
        uint32_t fs = pf_r32(file + FI_FILESYSTEM);
        uint32_t block = ((pf_r32(file + FI_AVATAR) & 0xFFFFFF) + pf_r32(ior + IOI_OFFSET)) * pf_r32(fs + FS_RATIO) + d.hdr +
                         d.offset;
        bool write = cmd == CMD_WRITE;
        uint32_t buf = pf_r32(ior + (write ? IOI_SEND_BUF : IOI_RECV_BUF));
        int32_t len = (int32_t)pf_r32(ior + (write ? IOI_SEND_LEN : IOI_RECV_LEN));
        std::vector<uint8_t> t(len > 0 ? (size_t)len : 0);
        if (write)
            for (int32_t i = 0; i < len; ++i) t[(size_t)i] = (uint8_t)pf_r8(buf + (uint32_t)i);
        uint32_t err = lm_dev(d, write, (int32_t)block, t.data(), len);
        if (!err && !write) {
            for (int32_t i = 0; i < len; ++i) pf_w8(buf + (uint32_t)i, t[(size_t)i]);
            pf_w32(ior + IO_ACTUAL, pf_r32(ior + IO_ACTUAL) + (uint32_t)len);
        }
        pf_w32(ior + IO_ERROR, err);
        if (g_pf_trace)
            pf_log("        %s %d bytes at block %u of \"%s\": %08X\n", write ? "wrote" : "read", len, block,
                   g_places[file].c_str(), err);
        return;
    }
    switch (cmd) {
    case FILECMD_READDIR:                                                           // 0x4e90
        d.state = 0x17;
        d.file = file;
        break;
    case FILECMD_READENTRY: case FILECMD_DELETEENTRY: {                             // 0x4e64
        std::memset(d.data, 0, sizeof d.data);
        int32_t n = (int32_t)pf_r32(ior + IOI_SEND_LEN);
        lm_strncpy_guest(d.data, pf_r32(ior + IOI_SEND_BUF), (uint32_t)(n > 0x20 ? 0x20 : n < 0 ? 0 : n));
        d.state = 0x17;
        d.file = file;
        break;
    }
    case FILECMD_ALLOCBLOCKS:                                                       // 0x4e2c
        d.file = file;
        d.want = pf_r32(file + FI_BLOCKCOUNT) + pf_r32(ior + IOI_OFFSET) + d.hdr;
        d.state = 3;
        d.a = pf_r32(file + FI_AVATAR);
        break;
    case FILECMD_SETEOF:                                                            // 0x4eac
        if (pf_r32(file + FI_BLOCKCOUNT) * pf_r32(file + FI_BLOCKSIZE) < pf_r32(ior + IOI_OFFSET)) {
            pf_w32(ior + IO_ERROR, FERR_BADIOARG);
            return;
        }
        d.state = 0x19;
        d.a = pf_r32(file + FI_AVATAR);
        break;
    case FILECMD_ADDENTRY: {                                                        // 0x4e00
        d.file = file;
        uint32_t vbs = pf_r32(pf_r32(file + FI_FILESYSTEM) + FS_VOLBLOCKSIZE);
        d.want = (vbs + 0x3f) / vbs;
        d.state = 3;
        break;
    }
    case FILECMD_SETTYPE:                                                           // 0x4d70
        d.state = 0x1b;
        d.a = pf_r32(file + FI_AVATAR);
        break;
    default:
        pf_w32(ior + IO_ERROR, FERR_BADCOMMAND);
        return;
    }
    int io = 0;
    int32_t r = lm_step(d, ior, io);
    while (r > 0) {
        if (io) d.last_io = io;
        if (!d.last_io) {
            std::fprintf(stderr, "File: a linked-memory request whose first step asks for no transfer: not yet\n");
            std::exit(3);
        }
        uint32_t err = lm_transfer(d, d.last_io);
        if (d.state == 1) {                                 // 0x5034: done, whatever the transfer said
            d.state = 0;
            r = 0;
            break;
        }
        if (err) {
            d.state = 0;
            r = (int32_t)err;
            break;
        }
        r = lm_step(d, ior, io);
    }
    pf_w32(ior + IO_ERROR, (uint32_t)r);
    if (g_pf_trace)
        pf_log("        filesystem \"%s\": command %u on \"%s\": %08X\n", d.name.c_str(), cmd, g_places[file].c_str(),
               (uint32_t)r);
}

// A request the folio makes itself (the walk's, DeleteFile's) on `file`: an IOReq of its own, its
// buffers in the OS's memory; its io_Error, and the DirectoryEntry it got into `de`.
static uint32_t lm_own_request(LmFs& d, uint32_t file, uint32_t cmd, const std::string& name, uint8_t* de = nullptr) {
    uint32_t ior = pf_os_alloc(0x70), entry = pf_os_alloc(DE_SIZE), s = pf_os_string(name.c_str());
    for (uint32_t i = 0; i < 0x70; i += 4) pf_w32(ior + i, 0);
    for (uint32_t i = 0; i < DE_SIZE; i += 4) pf_w32(entry + i, 0);
    pf_w8(ior + IOI_COMMAND, cmd);
    pf_w32(ior + IOI_SEND_BUF, s);
    pf_w32(ior + IOI_SEND_LEN, (uint32_t)name.size());
    if (de) {
        pf_w32(ior + IOI_RECV_BUF, entry);
        pf_w32(ior + IOI_RECV_LEN, DE_SIZE);
    }
    lm_run(d, ior, file);
    uint32_t err = pf_r32(ior + IO_ERROR);
    if (de)
        for (uint32_t i = 0; i < DE_SIZE; ++i) de[i] = (uint8_t)pf_r8(entry + i);
    pf_os_free(s);
    pf_os_free(entry);
    pf_os_free(ior);
    return err;
}

// The entry `name` of the linked-memory directory `dir` (a place): the folio's File for it when it
// has one (the walk looks in its list first, 0x2f68: a File of that parent and name, its
// information cached), else FILECMD_READENTRY to the directory (0x3114) -- its error is NOFILE,
// whatever it was -- and a File made of the DirectoryEntry (0x31a4): named as the walk names it,
// the directory's filesystem, the directory its parent (one use more), the entry's identifier (or
// the folio's next, from -1 down), type, flags, block size, bytes, blocks, burst and gap, its last
// avatar's index (the count less one) and its first avatar. FILECMD_OPENENTRY on it then is done at
// once (0x4b88). "" when there is none.
static std::string lm_child(const std::string& dir, const std::string& name) {
    for (const auto& f : g_files) {
        if (f.first.size() <= dir.size() + 1 || f.first.compare(0, dir.size() + 1, dir + "/") ||
            f.first.find('/', dir.size() + 1) != std::string::npos)
            continue;
        if (same_name(f.first.substr(dir.size() + 1), name)) return f.first;
    }
    LmFs* d = lm_of(dir);
    uint32_t dirfile = file_node(dir);
    uint8_t de[DE_SIZE];
    if (lm_own_request(*d, dirfile, FILECMD_READENTRY, name, de)) return "";
    pf_w32(dirfile + FI_USECOUNT, pf_r32(dirfile + FI_USECOUNT) + 1);
    std::string at = dir + "/" + name;
    uint32_t n = pf_os_alloc(FI_SIZE);
    for (uint32_t i = 0; i < FI_SIZE; i += 4) pf_w32(n + i, 0);
    pf_w32(n + 12, FI_SIZE);
    pf_item_new(n, FILEFOLIO, FILENODE, name.c_str());
    for (size_t i = 0; i < name.size() && i < 32; ++i) pf_w8(n + FI_NAME + (uint32_t)i, (uint8_t)name[i]);
    pf_w32(n + FI_FILESYSTEM, pf_r32(dirfile + FI_FILESYSTEM));
    pf_w32(n + FI_PARENT, dirfile);
    uint32_t id = hw(de, 0x04);
    pf_w32(n + FI_UNIQUEID, id ? id : g_next_id--);
    pf_w32(n + FI_FLAGS, hw(de, 0x00));
    pf_w32(n + FI_BURST, hw(de, 0x18));
    pf_w32(n + FI_GAP, hw(de, 0x1c));
    pf_w32(n + FI_BLOCKSIZE, hw(de, 0x0c));
    pf_w32(n + FI_BLOCKCOUNT, hw(de, 0x14));
    pf_w32(n + FI_BYTECOUNT, hw(de, 0x10));
    pf_w32(n + FI_LASTAVATAR, hw(de, 0x20) - 1);
    pf_w32(n + FI_AVATAR, hw(de, 0x44));
    pf_w32(n + FI_TYPE, hw(de, 0x08));
    g_files[at] = n;
    g_places[n] = at;
    return at;
}

// A File of a linked-memory filesystem deleted (the folio's ir_Delete for one, 0xd94): its
// parent has one use less, and it is gone from the folio's list.
static void lm_forget(uint32_t file) {
    uint32_t parent = pf_r32(file + FI_PARENT);
    pf_w32(parent + FI_USECOUNT, pf_r32(parent + FI_USECOUNT) - 1);
    g_files.erase(g_places[file]);
    g_places.erase(file);
    pf_item_free((int32_t)pf_r32(file + 24));
}

// The mount (0x1e18) of the ram device's unit `unit` from block `offset`: the unit's status (the
// ram driver's: blocks of a byte), and nothing (0) for a unit of 0xe1 blocks or fewer; then a
// label looked for -- at the first block, then at 0xe1, then each 0x8012 blocks on, nine places
// in all -- whose record type is 1, whose five sync bytes are 0x5a, whose version is 1 or 2 and
// whose root directory has at most eight avatars; none is -1. A name already mounted is
// DuplicateFile. Version 2 is a linked-memory filesystem (0x2260; version 1, a CD's, is not
// here): the FileSystem named as the label names the volume, its block size, count and
// identifier the label's, and its root directory's File (0x22f8: of 0x80 bytes and a word more
// for each avatar after the first): named the same, the folio's root its parent (one use more),
// type '*dir', flags 0x2d, one use, the label's root block size, count and identifier, its bytes
// their product, the label's avatars. The FileSystem's item.
static uint32_t lm_mount(uint32_t unit, uint32_t offset) {
    if (unit != RAM_NVRAM_UNIT) {
        std::fprintf(stderr, "File: a mount of the ram device's unit %u (the console's ROM or memory): not yet\n", unit);
        std::exit(3);
    }
    LmFs probe;
    probe.bs = 1;
    probe.blocks = (uint32_t)pf_nvram().size();
    if (probe.blocks <= 0xe1) return 0;
    uint32_t lbytes = (probe.bs + 0x83) / probe.bs * probe.bs;
    std::vector<uint8_t> label(lbytes > DL_SIZE ? lbytes : DL_SIZE);
    bool found = false;
    uint32_t pos = 0;
    for (int32_t k = -1;;) {
        if (!lm_dev(probe, false, (int32_t)(pos + offset), label.data(), (int32_t)lbytes) && label[0] == 1 &&
            label[1] == 0x5a && label[2] == 0x5a && label[3] == 0x5a && label[4] == 0x5a && label[5] == 0x5a &&
            (label[DL_VERSION] == 1 || label[DL_VERSION] == 2) && hw(label.data(), DL_LASTAVATAR) <= 7)
            found = true;
        pos = k >= 0 ? pos + 0x8012 : 0xe1;
        if (++k > 7 || found) break;
    }
    if (!found) return 0xFFFFFFFFu;
    std::string name;
    for (uint32_t i = 0; i < 32 && label[DL_ID + i]; ++i) name += (char)label[DL_ID + i];
    for (const auto& fs : g_lm)
        if (same_name(fs->name, name)) return FERR_DUPLICATE;
    if (label[DL_VERSION] != 2) {
        std::fprintf(stderr, "File: an optimized (a CD's) filesystem on the ram device: not yet\n");
        std::exit(3);
    }
    auto d = std::make_unique<LmFs>(probe);
    d->name = name;
    d->unit = unit;
    d->offset = offset;
    d->chunk_max = 0x100 / d->bs;
    d->hdr = (d->bs + 0x3f) / d->bs;
    uint32_t fsn = pf_os_alloc(FS_SIZE);
    for (uint32_t i = 0; i < FS_SIZE; i += 4) pf_w32(fsn + i, 0);
    pf_w32(fsn + 12, FS_SIZE);
    int32_t item = pf_item_new(fsn, FILEFOLIO, FILESYSTEMNODE, name.c_str());
    uint32_t last = hw(label.data(), DL_LASTAVATAR), size = 0x80 + 4 * last;
    uint32_t root = pf_os_alloc(size);
    for (uint32_t i = 0; i < size; i += 4) pf_w32(root + i, 0);
    pf_w32(root + 12, size);
    pf_item_new(root, FILEFOLIO, FILENODE, name.c_str());
    for (uint32_t i = 0; i < 32; ++i) {
        pf_w8(fsn + FS_NAME + i, label[DL_ID + i]);
        pf_w8(root + FI_NAME + i, label[DL_ID + i]);
    }
    uint32_t top = file_node("/");
    pf_w32(root + FI_FILESYSTEM, fsn);
    pf_w32(root + FI_PARENT, top);
    pf_w32(top + FI_USECOUNT, pf_r32(top + FI_USECOUNT) + 1);
    pf_w32(root + FI_TYPE, FILE_TYPE_DIRECTORY);
    pf_w32(root + FI_FLAGS, LM_ROOT_FLAGS);
    pf_w32(root + FI_USECOUNT, 1);
    pf_w32(root + FI_BURST, 1);
    pf_w32(root + FI_BLOCKSIZE, hw(label.data(), DL_ROOTBLOCKSIZE));
    pf_w32(root + FI_BLOCKCOUNT, hw(label.data(), DL_ROOTBLOCKCOUNT));
    pf_w32(root + FI_BYTECOUNT, hw(label.data(), DL_ROOTBLOCKSIZE) * hw(label.data(), DL_ROOTBLOCKCOUNT));
    pf_w32(root + FI_UNIQUEID, hw(label.data(), DL_VOLID));
    pf_w32(root + FI_LASTAVATAR, last);
    for (uint32_t i = 0; i <= last; ++i) pf_w32(root + FI_AVATAR + 4 * i, hw(label.data(), DL_AVATARS + 4 * i));
    pf_w32(fsn + FS_VOLBLOCKSIZE, hw(label.data(), DL_BLOCKSIZE));
    pf_w32(fsn + FS_VOLBLOCKCOUNT, hw(label.data(), DL_BLOCKCOUNT));
    pf_w32(fsn + FS_VOLID, hw(label.data(), DL_VOLID));
    pf_w32(fsn + FS_RATIO, hw(label.data(), DL_BLOCKSIZE) / d->bs);
    pf_w32(fsn + FS_ROOT, root);
    d->node = fsn;
    d->root = root;
    g_files["/" + name] = root;
    g_places[root] = "/" + name;
    g_lm.push_back(std::move(d));
    if (g_pf_trace) pf_log("        mounted \"%s\", the ram device's unit %u from block %u\n", name.c_str(), unit, offset);
    return (uint32_t)item;
}

// swi 0x30004: Item MountFileSystem(Item device, int32 unit, uint32 blockOffset) -- 0x2770: a
// device (else -1), mounted (the ram device's; any other stops the run: not yet).
static void f_mountfilesystem(ArmCpu& c) {
    uint32_t dev = pf_check_item((int32_t)c.r[0], 1, DEVICENODE);
    if (!dev) {
        c.r[0] = 0xFFFFFFFFu;
        return;
    }
    if (dev != pf_ram_device()) pf_stop(c, "MountFileSystem: a device other than the ram device: not yet");
    c.r[0] = lm_mount(c.r[1] & 0xff, c.r[2]);
    if (g_pf_trace) pf_log("        MountFileSystem unit %u from %u -> %08X\n", c.r[1] & 0xff, c.r[2], c.r[0]);
}

// swi 0x3000d: Err DismountFileSystem(char* name) -- 0x25d8: the name (a leading '/' passed over)
// a mounted filesystem's, else NOFILESYSTEM; every File of it no one uses deleted, and BUSY when
// any other one is used than its root, or its root by more than its own use; then the root and
// the filesystem gone. 0.
static void f_dismountfilesystem(ArmCpu& c) {
    char path[256];
    pf_cstring(c.r[0], path, sizeof path);
    const char* name = path[0] == '/' ? path + 1 : path;
    LmFs* d = nullptr;
    size_t at = 0;
    for (; at < g_lm.size(); ++at)
        if (same_name(g_lm[at]->name, name)) {
            d = g_lm[at].get();
            break;
        }
    if (!d) {
        c.r[0] = FERR_NOFILESYSTEM;
        return;
    }
    std::string top = "/" + d->name;
    uint32_t used = 0;
    for (bool again = true; again;) {
        again = false;
        used = 0;
        for (const auto& f : g_files) {
            if (f.first != top && f.first.compare(0, top.size() + 1, top + "/")) continue;
            if (pf_r32(f.second + FI_USECOUNT)) {
                ++used;
            } else {
                lm_forget(f.second);
                again = true;
                break;
            }
        }
    }
    if (used > 1 || pf_r32(d->root + FI_USECOUNT) > 1) {
        c.r[0] = FERR_BUSY;
        return;
    }
    pf_w32(d->root + FI_USECOUNT, 0);
    lm_forget(d->root);
    pf_item_free((int32_t)pf_r32(d->node + 24));
    g_lm.erase(g_lm.begin() + (long)at);
    if (g_pf_trace) pf_log("        DismountFileSystem \"%s\"\n", name);
    c.r[0] = 0;
}

// swi 0x30009: Item CreateFile(char* path) -- 0x403c: the path walked in its creating mode (0x2b1c
// with 1): when its last name is not in its directory (FILECMD_READENTRY fails), FILECMD_ADDENTRY
// of it there (0x38b4) -- its error, if any, is CreateFile's -- and the entry read again and its
// File made (0x3104); a name that is there is DuplicateFile (0x39d8). The new File's item (the
// walk's use of it given back). Here the last name is walked apart: one with an alias in it, or a
// directory of the disc, stops the run (not yet).
static void f_createfile(ArmCpu& c) {
    char path[256];
    pf_cstring(c.r[0], path, sizeof path);
    std::string p = path, dir, name;
    size_t slash = p.find_last_of('/');
    if (slash == std::string::npos) {
        name = p;
    } else {
        dir = slash == 0 ? "/" : p.substr(0, slash);
        name = p.substr(slash + 1);
    }
    std::string at = g_cwd;
    int32_t err = dir.empty() ? 0 : walk(dir, at);
    if (!err && (name.empty() || name[0] == '$')) pf_stop(c, "CreateFile: a path whose last name is an alias: not yet");
    if (!err && !is_dir(at)) err = (int32_t)FERR_NOTADIRECTORY;
    if (!err && name.size() >= 32) err = (int32_t)FERR_FS_BADNAME;
    if (!err && !lm_of(at)) pf_stop(c, "CreateFile in a directory of the disc: not yet");
    std::string made;
    if (!err && !lm_child(at, name).empty()) err = (int32_t)FERR_DUPLICATE;
    if (!err) err = (int32_t)lm_own_request(*lm_of(at), file_node(at), FILECMD_ADDENTRY, name);
    if (!err) {
        made = lm_child(at, name);
        if (made.empty()) err = (int32_t)FERR_NOFILE;
    }
    c.r[0] = err ? (uint32_t)err : pf_r32(file_node(made) + 24);
    if (g_pf_trace) pf_log("        CreateFile \"%s\" -> %08X\n", path, c.r[0]);
}

// swi 0x3000a: Err DeleteFile(char* path) -- 0x4190: the path walked (one use more of its File);
// a File used by anyone else is BUSY, one whose directory is read-only (a CD's) READONLY -- each
// with the walk's use given back; then FILECMD_DELETEENTRY of its name to its directory (0x4288):
// its error leaves the walk's use; done, the File is deleted. 0 or the error.
static void f_deletefile(ArmCpu& c) {
    char path[256];
    pf_cstring(c.r[0], path, sizeof path);
    std::string at;
    int32_t err = walk(path, at);
    if (!err) {
        uint32_t f = file_node(at);
        pf_w32(f + FI_USECOUNT, pf_r32(f + FI_USECOUNT) + 1);
        uint32_t parent = pf_r32(f + FI_PARENT);
        if (pf_r32(f + FI_USECOUNT) > 1) err = (int32_t)FERR_BUSY;
        else if (pf_r32(parent + FI_FLAGS) & FILE_IS_READONLY) err = (int32_t)FERR_READONLY;
        if (err) {
            pf_w32(f + FI_USECOUNT, pf_r32(f + FI_USECOUNT) - 1);
        } else {
            LmFs* d = lm_of(at);
            if (!d) pf_stop(c, "DeleteFile of a file of the disc in a directory that is not read-only: not yet");
            char name[33] = {};
            for (uint32_t i = 0; i < 32; ++i) name[i] = (char)pf_r8(f + FI_NAME + i);
            err = (int32_t)lm_own_request(*d, parent, FILECMD_DELETEENTRY, name);
            if (!err) {
                pf_w32(f + FI_USECOUNT, pf_r32(f + FI_USECOUNT) - 1);
                lm_forget(f);
            }
        }
    }
    c.r[0] = (uint32_t)err;
    if (g_pf_trace) pf_log("        DeleteFile \"%s\" -> %08X\n", path, c.r[0]);
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
// are the runtime's and are not run, but for the System directory's programs (System/Programs:
// lmadm, which startopera runs, format, which lmadm runs), which run when this build has their
// modules and are passed over when it has not; any other is run until it ends, then the next
// line --, any other file a script, run there and then (as its last line, in its place: a disc's
// scripts can name each other for ever, as Crash 'n Burn's runme1 and runme2 do). What is none of
// these is reported and passed over. A program named with arguments is given its line, the words
// with a space between each two, as the task the shell starts gets it; one named alone gets none
// (and its argv[0] is its module's name).
static int (*g_shell_run)(const std::string& host, const std::string& cmdline);
static int g_shell_result;

// Whether this build has the module recompiled from the program in `host` (the test arm_identify
// makes: the crc32 of its read-only area as the file holds it).
static bool has_module(const std::string& host) {
    std::ifstream f(host, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    for (int i = 0; i < g_arm_nmodules; ++i) {
        const ArmModule* m = g_arm_modules[i];
        if (m->size <= d.size() && arm_crc32(d.data(), m->size) == m->crc) return true;
    }
    return false;
}

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
                if (lower(to).rfind("/system/", 0) == 0 &&                  // the OS's own: the runtime's
                    (lower(to).rfind("/system/programs/", 0) != 0 || !has_module(host)))
                    continue;
                std::string cmdline;
                for (size_t i = 0; w.size() > 1 && i < w.size(); ++i) cmdline += (i ? " " : "") + w[i];
                pf_log("the shell: %s (%s)\n", w[0].c_str(), to.c_str());
                g_shell_result = g_shell_run(host, cmdline);
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

int pf_shell_boot(int (*run)(const std::string& host, const std::string& cmdline)) {
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
        g_shell_result = run(host_of(at).string(), "");
    }
    return g_shell_result;
}

// The disc's OS release: the version byte of its System/Kernel/os_code's 3DO header (the AIF after
// the 16-byte boot header, +0x80 + 0x14) -- 23 on Immercenary's disc (23.10), 20 on Doctor
// Hauzer's (20.21), 0 on Crash 'n Burn's (1993, whose kernel has no header); 0 when the disc has
// none. Where a later folio differs from the 1993 one the runtime follows, the release says which.
uint32_t pf_os_release() {
    static std::string s_root;
    static uint32_t s_release;
    if (s_root != g_pf_disc_root) {
        s_root = g_pf_disc_root;
        s_release = 0;
        std::string host = pf_host_path("/System/Kernel/os_code");
        if (!host.empty()) {
            std::ifstream f(host, std::ios::binary);
            f.seekg(0xa4);
            char v = 0;
            if (f.read(&v, 1)) s_release = (uint8_t)v;
        }
    }
    return s_release;
}

uint32_t pf_system_version(const char* path) {
    static std::string s_root;
    static std::map<std::string, uint32_t> s_versions;
    if (s_root != g_pf_disc_root) {
        s_root = g_pf_disc_root;
        s_versions.clear();
    }
    auto it = s_versions.find(path);
    if (it != s_versions.end()) return it->second;
    uint32_t version = 0;
    std::string host = pf_host_path(path);
    if (!host.empty()) {
        std::ifstream f(host, std::ios::binary);
        f.seekg(0x94);
        unsigned char v[2] = {0, 0};
        if (f.read((char*)v, 2)) version = PF_VERSION(v[0], v[1]);
    }
    return s_versions[path] = version;
}

void pf_file_init() {
    g_cwd = "/";
    g_files.clear();
    g_places.clear();
    g_aliases.clear();
    g_open_files.clear();
    g_reads.clear();
    g_loaded_code.clear();
    g_lm.clear();
    g_next_id = 0xFFFFFFFFu;
    shell_start();
    if (g_pf_trace) {
        pf_log("shell aliases:");
        for (const auto& a : g_aliases[0]) pf_log(" %s=%s", a.first.c_str(), a.second.c_str());
        pf_log("\n");
    }
    pf_on_swi(0x30000, f_opendiskfile);
    pf_on_swi(0x30001, f_closediskfile);
    pf_on_swi(0x30004, f_mountfilesystem);
    pf_on_swi(0x30007, f_changedirectory);
    pf_on_swi(0x30008, f_getdirectory);
    pf_on_swi(0x30009, f_createfile);
    pf_on_swi(0x3000a, f_deletefile);
    pf_on_swi(0x3000b, f_createalias);
    pf_on_swi(0x3000d, f_dismountfilesystem);
    pf_on_slot(PF_FILE, -4, f_opendiskstream);
    pf_on_slot(PF_FILE, -8, f_readdiskstream);
    pf_on_slot(PF_FILE, -12, f_seekdiskstream);
    pf_on_slot(PF_FILE, -16, f_closediskstream);
    pf_on_slot(PF_FILE, -20, f_loadprogram);
    pf_on_slot(PF_FILE, -24, f_loadprogramprio);
    pf_on_slot(PF_FILE, -44, f_loadcode);
    pf_on_slot(PF_FILE, -48, f_unloadcode);
    pf_on_slot(PF_FILE, -52, f_executeassubroutine);
    // The folio's daemon, at its start (0x48b8), asks every device's every unit for its status
    // and mounts those that hold a filesystem (DS_USAGE_FILESYSTEM; offset 0x96 for a CD drive,
    // 0 else): of the ram device's, units 0, 1, 3, 4 and 5. Here unit 3 alone, the NVRAM -- whose
    // label, when it has one, names the filesystem ("nvram" as LMADM and FORMAT make it).
    lm_mount(RAM_NVRAM_UNIT, 0);
}
