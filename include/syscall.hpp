#pragma once
// include/syscall.hpp - Knail syscall interface (Linux x86-64 ABI compatible)
//
// Syscall numbers match Linux x86-64 so binaries compiled with a standard
// x86_64-linux-gnu toolchain work without modification.
// syscall number in RAX, args in RDI, RSI, RDX, R10, R8, R9
// Return value in RAX (negative errno on error, matching Linux convention)
#include <stdint.h>
#include <stddef.h>

// ── Syscall numbers (Linux x86-64) ────────────────────────────────────────
enum class Syscall : uint64_t {
    Read        = 0,   // read(fd, buf, len)
    Write       = 1,   // write(fd, buf, len)
    Open        = 2,   // open(path, flags, mode)
    Close       = 3,   // close(fd)
    Stat        = 4,   // stat(path, statbuf*)
    Seek        = 8,   // lseek(fd, offset, whence)
    Mmap        = 9,   // mmap(addr, len, prot, flags, fd, off) — stub for now
    Fstat       = 5,
    Dup         = 32,
    Dup2        = 33,
    Pipe        = 22,
    Getcwd      = 79,
    Chdir       = 80,
    Brk         = 12,  // brk(addr) — stub for now
    Readdir     = 78,  // getdents(fd, dirent*, count) — simplified
    GetTid      = 186, // gettid()
    Reboot      = 169,      // reboot()
    Yield       = 24,  // sched_yield()
    Fork        = 57,  // fork()
    Execve      = 59,  // execve(path, argv, envp)
    Exit        = 60,  // exit(code)
    ExitGroup   = 231, // exit_group(code) — same as exit for now
    Mkdir       = 83,  // mkdir(path, mode)
    Unlink      = 87,  // unlink(path)
    GetTicks    = 228, // clock_gettime — repurposed for ticks for now
    Spawn       = 220, // clone — repurposed for spawn for now
};

// ── Open flags (Linux values) ─────────────────────────────────────────────
static constexpr uint64_t O_READ   = 0;        // O_RDONLY
static constexpr uint64_t O_WRITE  = 1;        // O_WRONLY
static constexpr uint64_t O_RDWR   = 2;        // O_RDWR
static constexpr uint64_t O_CREATE = 0x40;     // O_CREAT
static constexpr uint64_t O_TRUNC  = 0x200;    // O_TRUNC
static constexpr uint64_t O_APPEND = 0x400;    // O_APPEND

// ── Seek whence (Linux values) ────────────────────────────────────────────
static constexpr uint64_t SEEK_SET = 0;
static constexpr uint64_t SEEK_CUR = 1;
static constexpr uint64_t SEEK_END = 2;

// ── Well-known file descriptors ───────────────────────────────────────────
static constexpr uint64_t FD_STDIN  = 0;
static constexpr uint64_t FD_STDOUT = 1;
static constexpr uint64_t FD_STDERR = 2;
static constexpr uint64_t FD_FIRST  = 3;

// ── stat buffer (matches Linux struct stat for x86-64) ────────────────────
struct [[gnu::packed]] StatBuf {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

// ── Linux getdents64 dirent (used by Readdir) ─────────────────────────────
struct [[gnu::packed]] LinuxDirent {
    uint64_t d_ino;
    int64_t  d_off;
    uint16_t d_reclen;
    uint8_t  d_type;
    char     d_name[256];
};

// ── st_mode values ────────────────────────────────────────────────────────
static constexpr uint32_t S_IFREG  = 0x8000; // regular file
static constexpr uint32_t S_IFDIR  = 0x4000; // directory
static constexpr uint32_t S_IRWXU  = 0x01C0; // user  rwx
static constexpr uint32_t S_IRWXG  = 0x0038; // group rwx
static constexpr uint32_t S_IRWXO  = 0x0007; // other rwx

// ── d_type values ─────────────────────────────────────────────────────────
static constexpr uint8_t DT_REG  = 8;
static constexpr uint8_t DT_DIR  = 4;

// ── Node types (internal VFS — unchanged) ────────────────────────────────
static constexpr uint32_t VFS_TYPE_FILE = 1;
static constexpr uint32_t VFS_TYPE_DIR  = 2;

// ── Dirent (internal VFS — unchanged) ────────────────────────────────────
struct Dirent {
    char     name[128];
    uint32_t type;
    uint32_t reserved;
};

// ── Errno values (Linux-compatible negated) ───────────────────────────────
static constexpr int64_t E_OK      =   0;
static constexpr int64_t E_PERM    =  -1;  // EPERM
static constexpr int64_t E_NOENT   =  -2;  // ENOENT
static constexpr int64_t E_INVAL   = -22;  // EINVAL
static constexpr int64_t E_BADF    =  -9;  // EBADF
static constexpr int64_t E_FAULT   = -14;  // EFAULT
static constexpr int64_t E_NOSYS   = -38;  // ENOSYS
static constexpr int64_t E_EXIST   = -17;  // EEXIST
static constexpr int64_t E_NOTDIR  = -20;  // ENOTDIR
static constexpr int64_t E_ISDIR   = -21;  // EISDIR
static constexpr int64_t E_NOSPACE = -28;  // ENOSPC
static constexpr int64_t E_EOF     =   0;  // EOF = 0 bytes read in Linux
static constexpr int64_t E_BUSY    = -16;  // EBUSY

namespace syscall {
void init();
void update_kernel_stack(uint64_t rsp);
extern "C" int64_t syscall_handler(uint64_t nr,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4);
} // namespace syscall
