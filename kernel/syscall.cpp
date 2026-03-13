// kernel/syscall.cpp - Knail syscall MSR setup + handler
#include "syscall.hpp"
#include "scheduler.hpp"
#include "fdtable.hpp"
#include "vfs.hpp"
#include "timer.hpp"
#include "serial.hpp"
#include "types.hpp"

extern "C" void syscall_entry();

extern "C" {
u64 syscall_last_user_rip    = 0;
u64 syscall_last_user_rsp    = 0;
u64 syscall_last_user_rflags = 0;
u64 syscall_exec_rip         = 0;
u64 syscall_exec_rsp         = 0;
u64 syscall_exec_rflags      = 0;
}

static volatile bool g_exec_pending = false;

extern "C" u64 syscall_consume_exec_pending() {
    if (g_exec_pending) { g_exec_pending = false; return 1; }
    return 0;
}

namespace syscall {

static constexpr u32 MSR_EFER      = 0xC0000080;
static constexpr u32 MSR_STAR      = 0xC0000081;
static constexpr u32 MSR_LSTAR     = 0xC0000082;
static constexpr u32 MSR_FMASK     = 0xC0000084;
static constexpr u32 MSR_KERNEL_GS = 0xC0000102;
static constexpr u32 MSR_GS_BASE   = 0xC0000101;

struct KernelGSBlock {
    u64 kernel_rsp;
    u64 user_rsp;
} __attribute__((packed));

static KernelGSBlock gs_block;
static volatile bool yield_pending = false;

struct SyscallArgs {
    u64 arg0, arg1, arg2, arg3, arg4;
};

using SyscallFn = i64(*)(const SyscallArgs&);

static void wrmsr(u32 msr, u64 val) {
    __asm__ volatile("wrmsr"
        :: "c"(msr), "a"((u32)(val & 0xFFFFFFFF)),
           "d"((u32)(val >> 32)));
}

static u64 rdmsr(u32 msr) {
    u32 lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((u64)hi << 32) | lo;
}

void init() {
    u64 efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);
    u64 star = ((u64)0x0008 << 32) | ((u64)0x0010 << 48);
    wrmsr(MSR_STAR,      star);
    wrmsr(MSR_LSTAR,     reinterpret_cast<u64>(syscall_entry));
    wrmsr(MSR_FMASK,     (1 << 9) | (1 << 10));
    wrmsr(MSR_KERNEL_GS, reinterpret_cast<u64>(&gs_block));
    wrmsr(MSR_GS_BASE,   0);
}

void update_kernel_stack(u64 rsp) {
    gs_block.kernel_rsp = rsp;
}

extern "C" void syscall_set_yield_pending() { yield_pending = true; }

extern "C" u64 syscall_consume_yield_pending() {
    if (yield_pending) { yield_pending = false; return 1; }
    return 0;
}

extern "C" void syscall_do_yield() { sched::yield(); }

// ── helpers ───────────────────────────────────────────────────────────────

static fdtable::FDTable* current_fdt() {
    sched::Task* t = sched::current();
    return t ? t->fd_table : nullptr;
}

// Unwrap a kResult<vfs::FileDescriptor*> from fdtable::lookup/remove,
// returning the Linux errno on failure.
static i64 fdt_lookup(fdtable::FDTable* fdt, u64 fd, vfs::FileDescriptor*& out) {
    auto res = fdtable::lookup(fdt, fd);
    if (res.is_err()) return E_BADF;
    out = res.value();
    return E_OK;
}

static i64 fdt_remove(fdtable::FDTable* fdt, u64 fd, vfs::FileDescriptor*& out) {
    auto res = fdtable::remove(fdt, fd);
    if (res.is_err()) return E_BADF;
    out = res.value();
    return E_OK;
}

static i64 fdt_install(fdtable::FDTable* fdt, vfs::FileDescriptor* f) {
    auto res = fdtable::install(fdt, f);
    if (res.is_err()) return E_NOSPACE;
    return (i64)res.value();
}

static i64 fdt_install_at(fdtable::FDTable* fdt, vfs::FileDescriptor* f, i32 fd) {
    auto res = fdtable::install_at(fdt, f, fd);
    if (res.is_err()) return E_BADF;
    return (i64)res.value();
}

// ── exit ──────────────────────────────────────────────────────────────────

extern "C" [[noreturn]] void syscall_exit_unwind(int code);

static i64 handle_exit(const SyscallArgs& args) {
    sched::preempt_enable();
    syscall_exit_unwind(static_cast<int>(args.arg0));
    __builtin_unreachable();
}

// ── simple handlers ───────────────────────────────────────────────────────

static i64 handle_get_tid(const SyscallArgs&) {
    return sched::current() ? (i64)sched::current()->tid : 0;
}

static i64 handle_get_ticks(const SyscallArgs&) {
    return (i64)timer::ticks();
}

static i64 handle_yield(const SyscallArgs&) {
    syscall_set_yield_pending();
    return E_OK;
}

static i64 handle_spawn(const SyscallArgs& args) {
    void (*entry)() = reinterpret_cast<void(*)()>(args.arg0);
    const char* name = reinterpret_cast<const char*>(args.arg1);
    auto res = sched::spawn(entry, name ? name : "task");
    return res.is_ok() ? (i64)res.value() : E_INVAL;
}

static i64 handle_mmap(const SyscallArgs&)  { return E_NOSYS; }
static i64 handle_brk(const SyscallArgs&)   { return 0; }

// ── I/O ───────────────────────────────────────────────────────────────────

static i64 handle_write(const SyscallArgs& args) {
    u64         fd  = args.arg0;
    const char* buf = reinterpret_cast<const char*>(args.arg1);
    u64         len = args.arg2;
    if (!buf) return E_FAULT;

    if (fd == FD_STDERR)
        for (u64 i = 0; i < len; i++) serial::write_char(buf[i]);

    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_lookup(fdt, fd, f)) return e;

    auto res = vfs::write(f, buf, len);
    return res.is_ok() ? res.value() : (i64)E_INVAL;
}

static i64 handle_read(const SyscallArgs& args) {
    u64   fd  = args.arg0;
    char* buf = reinterpret_cast<char*>(args.arg1);
    u64   len = args.arg2;
    if (!buf || len == 0) return E_FAULT;

    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_lookup(fdt, fd, f)) return e;

    auto res = vfs::read(f, buf, len);
    return res.is_ok() ? res.value() : (i64)E_INVAL;
}

// ── open / close / seek ───────────────────────────────────────────────────

static i64 handle_open(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    u64         flags = args.arg1;
    if (!path) return E_FAULT;

    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    auto f_res = vfs::open(path, flags);
    if (f_res.is_err()) return E_NOENT;

    i64 fd = fdt_install(fdt, f_res.value());
    if (fd < 0) { vfs::close(f_res.value()); return E_NOSPACE; }
    return fd;
}

static i64 handle_close(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_remove(fdt, args.arg0, f)) return e;

    vfs::close(f);
    return E_OK;
}

static i64 handle_seek(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_lookup(fdt, args.arg0, f)) return e;

    auto res = vfs::seek(f, (i64)args.arg1, args.arg2);
    return res.is_ok() ? res.value() : E_INVAL;
}

// ── stat / fstat ──────────────────────────────────────────────────────────

static void fill_stat(StatBuf* out, vfs::VNode* node) {
    for (usize i = 0; i < sizeof(StatBuf); i++)
        reinterpret_cast<u8*>(out)[i] = 0;
    out->st_size    = (i64)node->size;
    out->st_mode    = (node->type == VFS_TYPE_DIR)
                    ? (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
                    : (S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO);
    out->st_blksize = 512;
    out->st_blocks  = (i64)((node->size + 511) / 512);
}

static i64 handle_stat(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    StatBuf*    out  = reinterpret_cast<StatBuf*>(args.arg1);
    if (!path || !out) return E_FAULT;

    auto res = vfs::resolve(path);
    if (res.is_err()) return E_NOENT;

    fill_stat(out, res.value());
    return E_OK;
}

static i64 handle_fstat(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_lookup(fdt, args.arg0, f)) return e;

    StatBuf* out = reinterpret_cast<StatBuf*>(args.arg1);
    if (!out) return E_FAULT;

    fill_stat(out, f->node);
    return E_OK;
}

// ── directory ops ─────────────────────────────────────────────────────────

static i64 handle_readdir(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_lookup(fdt, args.arg0, f)) return e;

    Dirent* out = reinterpret_cast<Dirent*>(args.arg1);
    if (!out) return E_FAULT;

    auto res = vfs::readdir(f, out);
    return res.is_ok() ? E_OK : E_FAULT;
}

static i64 handle_mkdir(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    if (!path) return E_FAULT;
    auto res = vfs::mkdir(path);
    return res.is_ok() ? E_OK : E_INVAL;
}

static i64 handle_unlink(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    if (!path) return E_FAULT;
    auto res = vfs::unlink(path);
    return res.is_ok() ? E_OK : E_NOENT;
}

// ── dup / dup2 / pipe ─────────────────────────────────────────────────────

static i64 handle_dup(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_lookup(fdt, args.arg0, f)) return e;

    auto dup_res = vfs::dup(f);
    if (dup_res.is_err()) return E_NOSPACE;

    i64 fd = fdt_install(fdt, dup_res.value());
    if (fd < 0) { vfs::close(dup_res.value()); return E_NOSPACE; }
    return fd;
}

static i64 handle_dup2(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    u64 oldfd = args.arg0;
    u64 newfd = args.arg1;
    if (oldfd == newfd) return (i64)newfd;

    vfs::FileDescriptor* f = nullptr;
    if (i64 e = fdt_lookup(fdt, oldfd, f)) return e;

    // Close newfd if already open
    vfs::FileDescriptor* existing = nullptr;
    if (fdtable::remove(fdt, newfd).is_ok())
        vfs::close(existing);

    auto dup_res = vfs::dup(f);
    if (dup_res.is_err()) return E_NOSPACE;

    i64 fd = fdt_install_at(fdt, dup_res.value(), (i32)newfd);
    if (fd < 0) { vfs::close(dup_res.value()); return E_NOSPACE; }
    return fd;
}

static i64 handle_pipe(const SyscallArgs& args) {
    i32* fds = reinterpret_cast<i32*>(args.arg0);
    if (!fds) return E_FAULT;

    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;

    vfs::FileDescriptor* read_end  = nullptr;
    vfs::FileDescriptor* write_end = nullptr;
    auto pipe_res = vfs::pipe(&read_end, &write_end);
    if (pipe_res.is_err()) return E_NOSPACE;

    i64 rfd = fdt_install(fdt, read_end);
    if (rfd < 0) { vfs::close(read_end); vfs::close(write_end); return E_NOSPACE; }

    i64 wfd = fdt_install(fdt, write_end);
    if (wfd < 0) {
        fdtable::remove(fdt, (u64)rfd);
        vfs::close(read_end);
        vfs::close(write_end);
        return E_NOSPACE;
    }

    fds[0] = (i32)rfd;
    fds[1] = (i32)wfd;
    return E_OK;
}

// ── cwd ───────────────────────────────────────────────────────────────────

static i64 handle_getcwd(const SyscallArgs& args) {
    char* buf  = reinterpret_cast<char*>(args.arg0);
    u64   size = args.arg1;
    if (!buf || size == 0) return E_FAULT;

    vfs::VNode* cwd = vfs::get_cwd();
    vfs::build_path(cwd, buf, size);

    usize len = 0;
    while (buf[len]) len++;
    return (i64)len;
}

static i64 handle_chdir(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    if (!path) return E_FAULT;

    auto res = vfs::resolve(path);
    if (res.is_err()) return E_NOENT;
    if (res.value()->type != VFS_TYPE_DIR) return E_NOTDIR;

    vfs::set_cwd(res.value());
    return E_OK;
}

// ── reboot ────────────────────────────────────────────────────────────────

static i64 handle_reboot(const SyscallArgs& args) {
    static constexpr u32 LINUX_REBOOT_MAGIC1        = 0xfee1dead;
    static constexpr u32 LINUX_REBOOT_MAGIC2        = 0x28121969;
    static constexpr u32 LINUX_REBOOT_CMD_POWER_OFF = 0x4321fedc;
    static constexpr u32 LINUX_REBOOT_CMD_RESTART   = 0x01234567;

    if (args.arg0 != LINUX_REBOOT_MAGIC1 || args.arg1 != LINUX_REBOOT_MAGIC2)
        return E_INVAL;

    switch (args.arg2) {
        case LINUX_REBOOT_CMD_POWER_OFF:
            __asm__ volatile("outw %0, %1" :: "a"((u16)0x2000), "d"((u16)0x604));
            break;
        case LINUX_REBOOT_CMD_RESTART:
            __asm__ volatile("outw %0, %1" :: "a"((u16)0x0),    "d"((u16)0x604));
            break;
        default:
            return E_INVAL;
    }
    return E_OK;
}

// ── dispatch table ────────────────────────────────────────────────────────

struct SyscallRoute {
    Syscall   nr;
    SyscallFn fn;
};

static constexpr SyscallRoute kRoutes[] = {
    {Syscall::Read,      handle_read},
    {Syscall::Write,     handle_write},
    {Syscall::Open,      handle_open},
    {Syscall::Close,     handle_close},
    {Syscall::Stat,      handle_stat},
    {Syscall::Fstat,     handle_fstat},
    {Syscall::Seek,      handle_seek},
    {Syscall::Mmap,      handle_mmap},
    {Syscall::Brk,       handle_brk},
    {Syscall::Yield,     handle_yield},
    {Syscall::Exit,      handle_exit},
    {Syscall::ExitGroup, handle_exit},
    {Syscall::Readdir,   handle_readdir},
    {Syscall::Mkdir,     handle_mkdir},
    {Syscall::Unlink,    handle_unlink},
    {Syscall::GetTid,    handle_get_tid},
    {Syscall::Spawn,     handle_spawn},
    {Syscall::GetTicks,  handle_get_ticks},
    {Syscall::Reboot,    handle_reboot},
    {Syscall::Dup,       handle_dup},
    {Syscall::Dup2,      handle_dup2},
    {Syscall::Pipe,      handle_pipe},
    {Syscall::Getcwd,    handle_getcwd},
    {Syscall::Chdir,     handle_chdir},
};

static i64 dispatch_syscall(Syscall nr, const SyscallArgs& args) {
    for (const SyscallRoute& route : kRoutes)
        if (route.nr == nr) return route.fn(args);
    return E_NOSYS;
}

// ── syscall_handler ───────────────────────────────────────────────────────

extern "C" i64 syscall_handler(u64 nr,
                                u64 arg0, u64 arg1,
                                u64 arg2, u64 arg3,
                                u64 arg4) {
    SyscallArgs args{arg0, arg1, arg2, arg3, arg4};
    return dispatch_syscall(static_cast<Syscall>(nr), args);
}

} // namespace syscall
