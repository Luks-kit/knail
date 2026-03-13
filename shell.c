// user/shell.c - Knail interactive shell
// Build with: gcc -static -nostdlib -o shell shell.c


#include <stdint.h>
// ── syscall numbers ───────────────────────────────────────────────────────
#define SYS_READ     0
#define SYS_WRITE    1
#define SYS_OPEN     2
#define SYS_CLOSE    3
#define SYS_STAT     4
#define SYS_SEEK     8
#define SYS_MMAP     9
#define SYS_BRK      12
#define SYS_YIELD    24
#define SYS_EXIT     60
#define SYS_READDIR  78
#define SYS_GETCWD   79
#define SYS_CHDIR    80
#define SYS_MKDIR    83
#define SYS_UNLINK   87
#define SYS_GETTID   186
#define SYS_REBOOT   169

// ── reboot magic ──────────────────────────────────────────────────────────
#define REBOOT_MAGIC1      0xfee1dead
#define REBOOT_MAGIC2      0x28121969
#define REBOOT_CMD_POWEROFF 0x4321fedc
#define REBOOT_CMD_RESTART  0x01234567

// ── open flags ────────────────────────────────────────────────────────────
#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200

// ── syscall stubs ─────────────────────────────────────────────────────────
static long syscall0(long nr) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "a"(nr)
        : "rcx", "r11", "memory");
    return ret;
}
static long syscall1(long nr, long a) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "a"(nr), "D"(a)
        : "rcx", "r11", "memory");
    return ret;
}
static long syscall2(long nr, long a, long b) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "a"(nr), "D"(a), "S"(b)
        : "rcx", "r11", "memory");
    return ret;
}
static long syscall3(long nr, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall"
        : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return ret;
}

// ── minimal libc ─────────────────────────────────────────────────────────
static long write(int fd, const void* buf, long len) {
    return syscall3(SYS_WRITE, fd, (long)buf, len);
}
static long read(int fd, void* buf, long len) {
    return syscall3(SYS_READ, fd, (long)buf, len);
}
static long open(const char* path, int flags) {
    return syscall2(SYS_OPEN, (long)path, flags);
}
static long close(int fd) {
    return syscall1(SYS_CLOSE, fd);
}
static long mkdir(const char* path) {
    return syscall1(SYS_MKDIR, (long)path);
}
static long unlink(const char* path) {
    return syscall1(SYS_UNLINK, (long)path);
}
static long chdir(const char* path) {
    return syscall1(SYS_CHDIR, (long)path);
}
static long getcwd(char* buf, long size) {
    return syscall2(SYS_GETCWD, (long)buf, size);
}
static void exit(int code) {
    syscall1(SYS_EXIT, code);
    while(1);
}
static void poweroff() {
    syscall3(SYS_REBOOT, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_CMD_POWEROFF);
}
static void reboot() {
    syscall3(SYS_REBOOT, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_CMD_RESTART);
}

static int strlen(const char* s) {
    int n = 0; while (s[n]) n++; return n;
}
static void puts(const char* s) {
    write(1, s, strlen(s));
}
static void putchar(char c) {
    write(1, &c, 1);
}
static int strcmp(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}
static int strncmp(const char* a, const char* b, int n) {
    while (n-- && *a && *b && *a == *b) { a++; b++; }
    return n < 0 ? 0 : *a - *b;
}
static char* strcpy(char* dst, const char* src) {
    char* d = dst;
    while ((*d++ = *src++));
    return dst;
}
static int memset(void* dst, int c, long n) {
    char* d = (char*)dst;
    while (n--) *d++ = (char)c;
    return 0;
}

// ── number formatting ─────────────────────────────────────────────────────
static void print_dec(long n) {
    if (n < 0) { putchar('-'); n = -n; }
    if (n == 0) { putchar('0'); return; }
    char buf[32];
    int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) putchar(buf[--i]);
}

// ── line editor ───────────────────────────────────────────────────────────
static void itoa_back(char* buf, int* i, int n) {
    if (n >= 100) buf[(*i)++] = '0' + n / 100;
    if (n >=  10) buf[(*i)++] = '0' + (n / 10) % 10;
    buf[(*i)++] = '0' + n % 10;
}

static void move_cursor(int n, char dir) {
    if (n <= 0) return;
    char seq[16];
    int i = 0;
    seq[i++] = '\033';
    seq[i++] = '[';
    itoa_back(seq, &i, n);
    seq[i++] = dir;
    write(1, seq, i);
}

static void redraw(const char* buf, int len, int cur) {
    puts("\033[u");              // restore to saved position (start of input)
    puts("\033[K");              // erase to end of line
    if (len > 0) write(1, buf, len);
    if (cur < len) move_cursor(len - cur, 'D');  // reposition within input
}

static int read_line(char* buf, int max) {
    int len = 0;
    int cur = 0;

    while (1) {
        char c;
        if (read(0, &c, 1) <= 0) continue;

        if (c == '\r' || c == '\n') {
            write(1, "\r\n", 2);
            buf[len] = 0;
            return len;
        }

        if (c == '\b' || c == 127) {
            if (cur > 0) {
                cur--;
                len--;
                for (int i = cur; i < len; i++)
                    buf[i] = buf[i + 1];
                if (cur == len) {
                    write(1, "\b \b", 3);  // at end, simple erase
                } else {
                    redraw(buf, len, cur); // mid-line, full redraw
                }
            }
            continue;
        }

        if (c == '\033') {
            char seq[2];
            if (read(0, &seq[0], 1) <= 0) continue;
            if (seq[0] != '[')             continue;
            if (read(0, &seq[1], 1) <= 0) continue;

            if (seq[1] >= '0' && seq[1] <= '9') {
                char tmp;
                read(0, &tmp, 1);
                if (seq[1] == '3' && tmp == '~' && cur < len) {
                    for (int i = cur; i < len - 1; i++)
                        buf[i] = buf[i + 1];
                    len--;
                    redraw(buf, len, cur);
                }
                continue;
            }

            switch (seq[1]) {
                case 'C': if (cur < len) { cur++; move_cursor(1, 'C'); } break;
                case 'D': if (cur > 0)   { cur--; move_cursor(1, 'D'); } break;
                case 'H': if (cur > 0)   { move_cursor(cur, 'D'); cur = 0; } break;
                case 'F': if (cur < len) { move_cursor(len - cur, 'C'); cur = len; } break;
                case 'A': break;
                case 'B': break;
            }
            continue;
        }

        if (c < 32) continue;

        if (len < max - 1) {
            for (int i = len; i > cur; i--)
                buf[i] = buf[i - 1];
            buf[cur++] = c;
            len++;
            if (cur == len) {
                write(1, &c, 1);       // appending at end, just echo
            } else {
                redraw(buf, len, cur); // mid-line insert, full redraw
            }
        }
    }
}

// ── argument parser ───────────────────────────────────────────────────────
// Splits line into argv in-place. Returns argc.
static int parse_args(char* line, char* argv[], int max_args) {
    int argc = 0;
    char* p = line;
    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    return argc;
}

// ── dirent for getdents ───────────────────────────────────────────────────
typedef struct {
    char    name[128];
    uint32_t type;         // VFS_TYPE_DIR or VFS_TYPE_FILE — must match kernel Dirent
} Dirent;

#define VFS_TYPE_FILE 1
#define VFS_TYPE_DIR  2

#define DT_DIR 4
#define DT_REG 8

// ── built-in commands ─────────────────────────────────────────────────────
static void cmd_help() {
    puts("Knail shell commands:\r\n"
         "  help              - show this\r\n"
         "  echo [args...]    - print arguments\r\n"
         "  pwd               - print working directory\r\n"
         "  cd <path>         - change directory\r\n"
         "  ls [path]         - list directory\r\n"
         "  cat <file>        - print file contents\r\n"
         "  mkdir <path>      - create directory\r\n"
         "  rm <file>         - remove file\r\n"
         "  write <f> <text>  - write text to file\r\n"
         "  tid               - print task ID\r\n"
         "  poweroff          - shut down\r\n"
         "  reboot            - restart\r\n"
         "  clear             - clear screen\r\n");
}

static void cmd_echo(int argc, char* argv[]) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) putchar(' ');
        puts(argv[i]);
    }
    puts("\r\n");
}

static void cmd_pwd() {
    char buf[256];
    if (getcwd(buf, sizeof(buf)) > 0)
        puts(buf);
    else
        puts("/");
    puts("\r\n");
}

static void cmd_cd(int argc, char* argv[]) {
    if (argc < 2) { puts("usage: cd <path>\r\n"); return; }
    long r = chdir(argv[1]);
    if (r < 0) {
        puts("cd: no such directory: ");
        puts(argv[1]);
        puts("\r\n");
    }
}

static void cmd_ls(int argc, char* argv[]) {
    char cwd[128];
    getcwd(cwd, sizeof(cwd));
    const char* path = (argc >= 2) ? argv[1] : cwd;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { puts("ls: cannot open\r\n"); return; }

    Dirent d;
    while (1) {
        long r = syscall2(SYS_READDIR, fd, (long)&d);
        if (r < 0) break;  // E_EOF or error
        if (d.type == VFS_TYPE_DIR) puts("\033[1;36m");
        else                        puts("\033[0m");
        puts(d.name);
        puts("    ");
    }
    puts ("\r\n");
    close(fd);
}

static void cmd_cat(int argc, char* argv[]) {
    if (argc < 2) { puts("usage: cat <file>\r\n"); return; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        puts("cat: cannot open: "); puts(argv[1]); puts("\r\n");
        return;
    }
    char buf[512];
    long n;
    while ((n = read(fd, buf, sizeof(buf)) ) > 0)
        write(1, buf, n);
    close(fd);
    puts("\r\n");
}

static void cmd_mkdir(int argc, char* argv[]) {
    if (argc < 2) { puts("usage: mkdir <path>\r\n"); return; }
    long r = mkdir(argv[1]);
    if (r < 0) { puts("mkdir: failed\r\n"); putchar('0' - r); puts("\r\n"); }
}

static void cmd_rm(int argc, char* argv[]) {
    if (argc < 2) { puts("usage: rm <file>\r\n"); return; }
    long r = unlink(argv[1]);
    if (r < 0) { puts("rm: failed\r\n"); }
}

static void cmd_write(int argc, char* argv[]) {
    if (argc < 3) { puts("usage: write <file> <text>\r\n"); return; }
    int fd = open(argv[1], O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) { puts("write: cannot open\r\n"); return; }
    write(fd, argv[2], strlen(argv[2]));
    write(fd, "\n", 1);
    close(fd);
}

static void cmd_tid() {
    puts("tid=");
    print_dec(syscall0(SYS_GETTID));
    puts("\r\n");
}

static void cmd_clear() {
    // ANSI clear screen
    puts("\033[2J\033[H");
}

// ── prompt ────────────────────────────────────────────────────────────────
static void print_prompt() {
    char cwd[256];
    long r = getcwd(cwd, sizeof(cwd));
    puts("\033[32mknail\033[0m:");
    if (r > 0) puts(cwd);
    else       puts("/");
    puts("\033[34m$\033[0m ");
    puts("\033[s");   // save cursor — start of input
}

// ── main ──────────────────────────────────────────────────────────────────
void _start() {
    puts("\033[1;36mKnail Shell\033[0m\r\n");
    puts("Type 'help' for commands.\r\n\r\n");
    
    char line[256];
    char* argv[16];

    while (1) {
        print_prompt();

        int len = read_line(line, sizeof(line));
        if (len == 0) continue;

        int argc = parse_args(line, argv, 16);
        if (argc == 0) continue;

        if      (!strcmp(argv[0], "help"))    cmd_help();
        else if (!strcmp(argv[0], "echo"))    cmd_echo(argc, argv);
        else if (!strcmp(argv[0], "pwd"))     cmd_pwd();
        else if (!strcmp(argv[0], "cd"))      cmd_cd(argc, argv);
        else if (!strcmp(argv[0], "ls"))      cmd_ls(argc, argv);
        else if (!strcmp(argv[0], "cat"))     cmd_cat(argc, argv);
        else if (!strcmp(argv[0], "mkdir"))   cmd_mkdir(argc, argv);
        else if (!strcmp(argv[0], "rm"))      cmd_rm(argc, argv);
        else if (!strcmp(argv[0], "write"))   cmd_write(argc, argv);
        else if (!strcmp(argv[0], "tid"))     cmd_tid();
        else if (!strcmp(argv[0], "clear"))   cmd_clear();
        else if (!strcmp(argv[0], "poweroff"))poweroff();
        else if (!strcmp(argv[0], "reboot"))  reboot();
        else {
            puts("unknown command: ");
            puts(argv[0]);
            puts("\r\n");
        }
    }
}
