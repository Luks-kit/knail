// kernel/syscall.cpp - Knail syscall MSR setup + handler
#include "syscall.hpp"
#include "scheduler.hpp"
#include "fdtable.hpp"
#include "vfs.hpp"
#include "timer.hpp"
#include "vga.hpp"
#include "serial.hpp"


extern "C" void syscall_entry();

extern "C" {
uint64_t syscall_last_user_rip = 0;
uint64_t syscall_last_user_rsp = 0;
uint64_t syscall_last_user_rflags = 0;
uint64_t syscall_exec_rip = 0;
uint64_t syscall_exec_rsp = 0;
uint64_t syscall_exec_rflags = 0;
}

static volatile bool g_exec_pending = false;

extern "C" uint64_t syscall_consume_exec_pending() {
    if (g_exec_pending) { g_exec_pending = false; return 1; }
    return 0;
}


namespace syscall {

static constexpr uint32_t MSR_EFER      = 0xC0000080;
static constexpr uint32_t MSR_STAR      = 0xC0000081;
static constexpr uint32_t MSR_LSTAR     = 0xC0000082;
static constexpr uint32_t MSR_FMASK     = 0xC0000084;
static constexpr uint32_t MSR_KERNEL_GS = 0xC0000102;
static constexpr uint32_t MSR_GS_BASE   = 0xC0000101;

struct KernelGSBlock {
    uint64_t kernel_rsp;
    uint64_t user_rsp;
} __attribute__((packed));
static KernelGSBlock gs_block;

static volatile bool yield_pending = false;

struct SyscallArgs {
    uint64_t arg0;
    uint64_t arg1;
    uint64_t arg2;
    uint64_t arg3;
    uint64_t arg4;
};

using SyscallFn = int64_t(*)(const SyscallArgs&);

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile("wrmsr"
        :: "c"(msr), "a"((uint32_t)(val & 0xFFFFFFFF)),
           "d"((uint32_t)(val >> 32)));
}
static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void init() {
    uint64_t efer = rdmsr(MSR_EFER);
    wrmsr(MSR_EFER, efer | 1);
    uint64_t star = ((uint64_t)0x0008 << 32) |
                    ((uint64_t)0x0010 << 48);
    wrmsr(MSR_STAR,     star);
    wrmsr(MSR_LSTAR,    reinterpret_cast<uint64_t>(syscall_entry));
    wrmsr(MSR_FMASK,    (1 << 9) | (1 << 10));
    wrmsr(MSR_KERNEL_GS, reinterpret_cast<uint64_t>(&gs_block));
    wrmsr(MSR_GS_BASE,  0);
}

void update_kernel_stack(uint64_t rsp) {
    gs_block.kernel_rsp = rsp;
}

extern "C" void syscall_set_yield_pending() {
    yield_pending = true;
}
extern "C" uint64_t syscall_consume_yield_pending() {
    if (yield_pending) { yield_pending = false; return 1; }
    return 0;
}
extern "C" void syscall_do_yield() {
    sched::yield();
}

// ── fd_table helper ───────────────────────────────────────────────────────
// Returns the current task's fd table, or null if it's a kernel task.
static fdtable::FDTable* current_fdt() {
    sched::Task* t = sched::current();
    return t ? t->fd_table : nullptr;
}

static int64_t handle_exit(const SyscallArgs&) {
    // syscall_entry() increments preempt_count before calling us and
    // normally decrements it after return. Exit never returns, so we must
    // drop the preempt disable here or the whole system becomes
    // non-preemptible after the first exiting task.
    sched::preempt_enable();
    sched::exit();
    return 0;
}

static int64_t handle_get_tid(const SyscallArgs&) {
    return sched::current() ? (int64_t)sched::current()->tid : 0;
}

static int64_t handle_get_ticks(const SyscallArgs&) {
    return (int64_t)timer::ticks();
}

static int64_t handle_yield(const SyscallArgs&) {
    syscall_set_yield_pending();
    return E_OK;
}

static int64_t handle_spawn(const SyscallArgs& args) {
    void (*entry)() = reinterpret_cast<void(*)()>(args.arg0);
    const char* name = reinterpret_cast<const char*>(args.arg1);
    uint32_t tid = sched::spawn(entry, name ? name : "task");
    return tid ? (int64_t)tid : E_INVAL;
}

static int64_t handle_mmap(const SyscallArgs&) {
    // Stub — return ENOSYS until we implement user heap
    return E_NOSYS;
}

static int64_t handle_brk(const SyscallArgs&) {
    // Stub — return 0 (current brk) until we implement user heap
    return 0;
}

static int64_t handle_write(const SyscallArgs& args) {
    uint64_t fd     = args.arg0;
    const char* buf = reinterpret_cast<const char*>(args.arg1);
    uint64_t len    = args.arg2;
    if (!buf) return E_FAULT;
    if (fd == FD_STDOUT || fd == FD_STDERR) {
        for (uint64_t i = 0; i < len; i++) {
            vga::put_char(buf[i]);
            serial::write_char(buf[i]);
        }
        return (int64_t)len;
    }
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = fdtable::lookup(fdt, fd);
    if (!f) return E_BADF;
    return vfs::write(f, buf, len);
}

static int64_t handle_read(const SyscallArgs& args) {
    uint64_t fd  = args.arg0;
    char*    buf = reinterpret_cast<char*>(args.arg1);
    uint64_t len = args.arg2;
    if (!buf || len == 0) return E_FAULT;
    if (fd == FD_STDIN) return E_INVAL;
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = fdtable::lookup(fdt, fd);
    if (!f) return E_BADF;
    return vfs::read(f, buf, len);
}

static int64_t handle_open(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    uint64_t flags   = args.arg1;
    if (!path) return E_FAULT;
    // vfs::open handles the translation internally now —
    // just pass the raw Linux flags through directly
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = vfs::open(path, flags);
    if (!f) return E_NOENT;
    int32_t fd = fdtable::install(fdt, f);
    if (fd <= 0) { vfs::close(f); return E_NOSPACE; }
    return fd;
}

static int64_t handle_close(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = fdtable::remove(fdt, args.arg0);
    if (!f) return E_BADF;
    vfs::close(f);
    return E_OK;
}

static int64_t handle_seek(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = fdtable::lookup(fdt, args.arg0);
    if (!f) return E_BADF;
    return vfs::seek(f, (int64_t)args.arg1, args.arg2);
}

static int64_t handle_stat(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    StatBuf*    out  = reinterpret_cast<StatBuf*>(args.arg1);
    if (!path || !out) return E_FAULT;
    vfs::VNode* node = vfs::resolve(path);
    if (!node) return E_NOENT;
    // Zero the whole struct first
    for (size_t i = 0; i < sizeof(StatBuf); i++)
        reinterpret_cast<uint8_t*>(out)[i] = 0;
    out->st_size    = (int64_t)node->size;
    out->st_mode    = (node->type == VFS_TYPE_DIR)
                    ? (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
                    : (S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO);
    out->st_blksize = 512;
    out->st_blocks  = (int64_t)((node->size + 511) / 512);
    return E_OK;
}

static int64_t handle_mkdir(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    if (!path) return E_FAULT;
    return vfs::mkdir(path);
}

static int64_t handle_unlink(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    if (!path) return E_FAULT;
    return vfs::unlink(path);
}

static int64_t handle_readdir(const SyscallArgs& args) {
    // Implemented as Linux getdents64(fd, buf, count)
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = fdtable::lookup(fdt, args.arg0);
    if (!f) return E_BADF;
    LinuxDirent* out   = reinterpret_cast<LinuxDirent*>(args.arg1);
    uint64_t     count = args.arg2;
    if (!out || count == 0) return E_FAULT;
    // Fill as many dirents as fit in count bytes
    uint64_t written = 0;
    while (written + sizeof(LinuxDirent) <= count) {
        Dirent d;
        int64_t r = vfs::readdir(f, &d);
        if (r == E_EOF || r < 0) break;
        LinuxDirent* ld = reinterpret_cast<LinuxDirent*>(
            reinterpret_cast<uint8_t*>(out) + written);
        ld->d_ino    = 1;
        ld->d_off    = (int64_t)(written + sizeof(LinuxDirent));
        ld->d_reclen = sizeof(LinuxDirent);
        ld->d_type   = (d.type == VFS_TYPE_DIR) ? DT_DIR : DT_REG;
        // Copy name
        int i = 0;
        while (d.name[i] && i < 255) { ld->d_name[i] = d.name[i]; i++; }
        ld->d_name[i] = 0;
        written += sizeof(LinuxDirent);
    }
    return written > 0 ? (int64_t)written : E_OK;
}

// ── fstat ─────────────────────────────────────────────────────────────────
static int64_t handle_fstat(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = fdtable::lookup(fdt, args.arg0);
    if (!f) return E_BADF;
    StatBuf* out = reinterpret_cast<StatBuf*>(args.arg1);
    if (!out) return E_FAULT;
    for (size_t i = 0; i < sizeof(StatBuf); i++)
        reinterpret_cast<uint8_t*>(out)[i] = 0;
    vfs::VNode* node = f->node;
    out->st_size    = (int64_t)node->size;
    out->st_mode    = (node->type == VFS_TYPE_DIR)
                    ? (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
                    : (S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO);
    out->st_blksize = 512;
    out->st_blocks  = (int64_t)((node->size + 511) / 512);
    return E_OK;
}

// ── dup ───────────────────────────────────────────────────────────────────
static int64_t handle_dup(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* f = fdtable::lookup(fdt, args.arg0);
    if (!f) return E_BADF;
    vfs::FileDescriptor* copy = vfs::dup(f);
    if (!copy) return E_NOSPACE;
    int32_t fd = fdtable::install(fdt, copy);
    if (fd < 0) { vfs::close(copy); return E_NOSPACE; }
    return fd;
}

// ── dup2 ──────────────────────────────────────────────────────────────────
static int64_t handle_dup2(const SyscallArgs& args) {
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    uint64_t oldfd = args.arg0;
    uint64_t newfd = args.arg1;
    if (oldfd == newfd) return (int64_t)newfd;
    vfs::FileDescriptor* f = fdtable::lookup(fdt, oldfd);
    if (!f) return E_BADF;
    // Close newfd if already open
    vfs::FileDescriptor* existing = fdtable::remove(fdt, newfd);
    if (existing) vfs::close(existing);
    vfs::FileDescriptor* copy = vfs::dup(f);
    if (!copy) return E_NOSPACE;
    int32_t fd = fdtable::install_at(fdt, copy, (int32_t)newfd);
    if (fd < 0) { vfs::close(copy); return E_NOSPACE; }
    return fd;
}

// ── pipe ──────────────────────────────────────────────────────────────────
static int64_t handle_pipe(const SyscallArgs& args) {
    int32_t* fds = reinterpret_cast<int32_t*>(args.arg0);
    if (!fds) return E_FAULT;
    fdtable::FDTable* fdt = current_fdt();
    if (!fdt) return E_BADF;
    vfs::FileDescriptor* read_end  = nullptr;
    vfs::FileDescriptor* write_end = nullptr;
    if (vfs::pipe(&read_end, &write_end) < 0) return E_NOSPACE;
    int32_t rfd = fdtable::install(fdt, read_end);
    if (rfd < 0) { vfs::close(read_end); vfs::close(write_end); return E_NOSPACE; }
    int32_t wfd = fdtable::install(fdt, write_end);
    if (wfd < 0) {
        fdtable::remove(fdt, rfd); vfs::close(read_end);
        vfs::close(write_end); return E_NOSPACE;
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return E_OK;
}

// ── getcwd ────────────────────────────────────────────────────────────────
static int64_t handle_getcwd(const SyscallArgs& args) {
    char*    buf  = reinterpret_cast<char*>(args.arg0);
    uint64_t size = args.arg1;
    if (!buf || size == 0) return E_FAULT;
    // For now every process is at root — extend when chdir is tracked per-task
    const char* cwd = "/";
    size_t len = 0;
    while (cwd[len]) len++;
    len++;
    if (len > size) return E_INVAL;
    for (size_t i = 0; i < len; i++) buf[i] = cwd[i];
    return (int64_t)reinterpret_cast<uint64_t>(buf);
}

// ── chdir ─────────────────────────────────────────────────────────────────
static int64_t handle_chdir(const SyscallArgs& args) {
    const char* path = reinterpret_cast<const char*>(args.arg0);
    if (!path) return E_FAULT;
    vfs::VNode* node = vfs::resolve(path);
    if (!node) return E_NOENT;
    if (node->type != VFS_TYPE_DIR) return E_INVAL;
    return E_OK;
}

static int64_t handle_reboot(const SyscallArgs& args) {
    static constexpr uint32_t LINUX_REBOOT_MAGIC1  = 0xfee1dead;
    static constexpr uint32_t LINUX_REBOOT_MAGIC2  = 0x28121969;
    static constexpr uint32_t LINUX_REBOOT_CMD_POWER_OFF = 0x4321fedc;
    static constexpr uint32_t LINUX_REBOOT_CMD_RESTART   = 0x01234567;

    if (args.arg0 != LINUX_REBOOT_MAGIC1 || args.arg1 != LINUX_REBOOT_MAGIC2)
        return E_INVAL;

    switch (args.arg2) {
        case LINUX_REBOOT_CMD_POWER_OFF:
            __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x2000), "d"((uint16_t)0x604));
            break;
        case LINUX_REBOOT_CMD_RESTART:
            __asm__ volatile("outw %0, %1" :: "a"((uint16_t)0x0), "d"((uint16_t)0x604));
            break;
        default:
            return E_INVAL;
    }
    return E_OK;
}

struct SyscallRoute {
    Syscall nr;
    SyscallFn fn;
};

static constexpr SyscallRoute kRoutes[] = {
    {Syscall::Read,      handle_read},
    {Syscall::Write,     handle_write},
    {Syscall::Open,      handle_open},
    {Syscall::Close,     handle_close},
    {Syscall::Stat,      handle_stat},
    {Syscall::Seek,      handle_seek},
    {Syscall::Mmap,      handle_mmap},
    {Syscall::Brk,       handle_brk},
    {Syscall::Yield,     handle_yield},
    {Syscall::Exit,      handle_exit},
    {Syscall::Readdir,   handle_readdir},
    {Syscall::Mkdir,     handle_mkdir},
    {Syscall::Unlink,    handle_unlink},
    {Syscall::GetTid,    handle_get_tid},
    {Syscall::Spawn,     handle_spawn},
    {Syscall::GetTicks,  handle_get_ticks},
    {Syscall::ExitGroup, handle_exit},
    {Syscall::Reboot, handle_reboot},
    {Syscall::Fstat,  handle_fstat},
    {Syscall::Dup,    handle_dup},
    {Syscall::Dup2,   handle_dup2},
    {Syscall::Pipe,   handle_pipe},
    {Syscall::Getcwd, handle_getcwd},
    {Syscall::Chdir,  handle_chdir},
};

static int64_t dispatch_syscall(Syscall nr, const SyscallArgs& args) {
    for (const SyscallRoute& route : kRoutes) {
        if (route.nr == nr) return route.fn(args);
    }
    return E_NOSYS;
}

// ── syscall_handler ───────────────────────────────────────────────────────
extern "C" int64_t syscall_handler(uint64_t nr,
                                    uint64_t arg0, uint64_t arg1,
                                    uint64_t arg2, uint64_t arg3,
                                    uint64_t arg4) {
    SyscallArgs args{arg0, arg1, arg2, arg3, arg4};
    return dispatch_syscall(static_cast<Syscall>(nr), args);
}


} // namespace syscall
