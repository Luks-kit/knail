// Simple user-space init for Knail.
// Performs one-time bootstrapping and then idles.

#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_GETCWD 79
#define SYS_CHDIR  80
#define SYS_MKDIR  83
#define SYS_EXIT   60
#define SYS_YIELD  24

#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x200



static long syscall0(long nr) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(nr)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall1(long nr, long a1) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(nr), "D"(a1)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall2(long nr, long a1, long a2) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(nr), "D"(a1), "S"(a2)
        : "rcx", "r11", "memory");
    return r;
}

static long syscall3(long nr, long a1, long a2, long a3) {
    long r;
    __asm__ volatile("syscall"
        : "=a"(r)
        : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
        : "rcx", "r11", "memory");
    return r;
}

static long write_str(const char* s) {
    long len = 0;
    while (s[len]) len++;
    return syscall3(SYS_WRITE, 1, (long)s, len);
}

static void write_long(long n) {
    if (n < 0) {
        syscall3(SYS_WRITE, 1, (long)"-", 1);
        n = -n;
    }
    char buf[20];
    int i = 19;
    buf[i] = 0;
    if (n == 0) {
        syscall3(SYS_WRITE, 1, (long)"0", 1);
        return;
    }
    while (n > 0) {
        buf[--i] = '0' + (n % 10);
        n /= 10;
    }
    const char* s = buf + i;
    long len = 19 - i;
    syscall3(SYS_WRITE, 1, (long)s, len);
}

static void maybe_mkdir(const char* path) {
    // mode is currently ignored in kernel, pass 0755 anyway.
    (void)syscall2(SYS_MKDIR, (long)path, 0755);
}

static void write_file(const char* path, const char* content) {
    long fd = syscall2(SYS_OPEN, (long)path, O_CREAT | O_TRUNC | O_RDWR);
    if (fd >= 3) {
        long len = 0;
        while (content[len]) len++;
        (void)syscall3(SYS_WRITE, fd, (long)content, len);
        (void)syscall1(SYS_CLOSE, fd);
    } else {
        write_str("Failed to open file: ");
        write_str(path);
        write_str("; Error nr: ");
        write_long(-fd);
        write_str("\n");
        
    }
}

__attribute__((noreturn)) void _start(void) {
    char cwd[64];

    write_str("[init] starting setup\n");

    maybe_mkdir("/disk/etc");
    maybe_mkdir("/disk/tmp");
    maybe_mkdir("/disk/var");
    maybe_mkdir("/disk/var/log");
    maybe_mkdir("/disk/home");

    write_file("/disk/etc/motd", "Welcome to Knail.\n");
    write_file("/disk/etc/profile", "PATH=/disk\nTERM=knail\n");
    write_file("/disk/var/log/boot.log", "Knail init completed bootstrap.\n");

    if (syscall2(SYS_GETCWD, (long)cwd, sizeof(cwd)) >= 0) {
        write_str("[init] cwd=");
        write_str(cwd);
        write_str("\n");
    }

    (void)syscall1(SYS_CHDIR, (long)"/");
    write_str("[init] setup done, exiting\n");

    // unreachable
    syscall1(SYS_EXIT, 0);
    __builtin_unreachable();
}
