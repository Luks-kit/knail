#pragma once
// Shared syscall stubs for Knail test binaries
// Build: gcc -static -nostdlib -o test_foo test_foo.c
#include <stdint.h>

#define SYS_READ        0
#define SYS_WRITE       1
#define SYS_OPEN        2
#define SYS_CLOSE       3
#define SYS_STAT        4
#define SYS_FSTAT       5
#define SYS_SEEK        8
#define SYS_MMAP        9
#define SYS_MPROTECT    10
#define SYS_MUNMAP      11
#define SYS_BRK         12
#define SYS_IOCTL       16
#define SYS_PIPE        22
#define SYS_YIELD       24
#define SYS_DUP         32
#define SYS_DUP2        33
#define SYS_NANOSLEEP   35
#define SYS_GETPID      39
#define SYS_CLONE       56
#define SYS_FORK        57
#define SYS_EXECVE      59
#define SYS_EXIT        60
#define SYS_WAIT4       61
#define SYS_GETCWD      79
#define SYS_CHDIR       80
#define SYS_MKDIR       83
#define SYS_UNLINK      87
#define SYS_GETTIMEOFDAY 96
#define SYS_ARCH_PRCTL  158
#define SYS_GETTID      186
#define SYS_CLOCK_GETTIME 228
#define SYS_EXIT_GROUP  231

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_CREAT   0x40
#define O_TRUNC   0x200

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define TIOCGWINSZ  0x5413
#define TCGETS      0x5401
#define CLOCK_REALTIME   0
#define CLOCK_MONOTONIC  1

#define WNOHANG 1

static long _sc0(long n) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n):"rcx","r11","memory"); return r;
}
static long _sc1(long n, long a) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a):"rcx","r11","memory"); return r;
}
static long _sc2(long n, long a, long b) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b):"rcx","r11","memory"); return r;
}
static long _sc3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c):"rcx","r11","memory"); return r;
}
static long _sc4(long n, long a, long b, long c, long d) {
    long r;
    register long r10 __asm__("r10") = d;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10):"rcx","r11","memory");
    return r;
}
static long _sc6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile("syscall":"=a"(r):"a"(n),"D"(a),"S"(b),"d"(c),"r"(r10),"r"(r8),"r"(r9):"rcx","r11","memory");
    return r;
}

// ── minimal libc ──────────────────────────────────────────────────────────
static void _exit(int code) { _sc1(SYS_EXIT, code); __builtin_unreachable(); }

static int _strlen(const char* s) { int n=0; while(s[n]) n++; return n; }

static void _write(int fd, const char* s, int n) { _sc3(SYS_WRITE, fd, (long)s, n); }
static void _puts(const char* s)  { _write(1, s, _strlen(s)); }
static void _putc(char c)         { _write(1, &c, 1); }

static void _print_dec(long n) {
    if (n < 0) { _putc('-'); n = -n; }
    if (n == 0) { _putc('0'); return; }
    char buf[24]; int i = 0;
    while (n > 0) { buf[i++] = '0' + (n % 10); n /= 10; }
    while (i > 0) _putc(buf[--i]);
}

static void _print_hex(unsigned long n) {
    _puts("0x");
    char buf[16]; int i = 0;
    if (n == 0) { _putc('0'); return; }
    while (n > 0) {
        int d = n & 0xF;
        buf[i++] = d < 10 ? '0'+d : 'a'+d-10;
        n >>= 4;
    }
    while (i > 0) _putc(buf[--i]);
}

static void _pass(const char* name) {
    _puts("\033[32m[PASS]\033[0m "); _puts(name); _puts("\r\n");
}
static void _fail(const char* name, const char* reason) {
    _puts("\033[31m[FAIL]\033[0m "); _puts(name); _puts(": "); _puts(reason); _puts("\r\n");
}
static void _info(const char* label, long val) {
    _puts("      "); _puts(label); _puts(": "); _print_dec(val); _puts("\r\n");
}
static void _infoh(const char* label, unsigned long val) {
    _puts("      "); _puts(label); _puts(": "); _print_hex(val); _puts("\r\n");
}
static void _header(const char* name) {
    _puts("\r\n\033[1;36m=== "); _puts(name); _puts(" ===\033[0m\r\n");
}
