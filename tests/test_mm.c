// test_mm.c - Tests: brk, mmap anonymous, munmap
#include "syscall_stubs.h"

#define MAP_ANONYMOUS 0x20
#define MAP_PRIVATE   0x02
#define PROT_READ     0x01
#define PROT_WRITE    0x02
#define PROT_NONE     0x00

void _start() {
    _header("mm");

    // ── brk: query current ────────────────────────────────────────────────
    {
        long cur = _sc1(SYS_BRK, 0);
        if (cur > 0) { _pass("brk query"); _infoh("brk", (unsigned long)cur); }
        else _fail("brk query", "returned 0");

        // extend by one page
        long new_brk = _sc1(SYS_BRK, cur + 0x1000);
        if (new_brk >= cur + 0x1000) {
            _pass("brk extend");
            // write to the new page
            char* p = (char*)cur;
            p[0] = 0xAB;
            if ((unsigned char)p[0] == 0xAB) _pass("brk write+read");
            else _fail("brk write+read", "readback mismatch");
        } else {
            _fail("brk extend", "brk did not advance");
            _infoh("new_brk", (unsigned long)new_brk);
        }
    }

    // ── mmap anonymous ────────────────────────────────────────────────────
    {
        long addr = _sc6(SYS_MMAP, 0, 0x2000,
                         PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS | MAP_PRIVATE,
                         -1, 0);
        if (addr < 0 || addr == 0) {
            _fail("mmap anon", "returned error/null");
            _info("addr", addr);
        } else {
            _pass("mmap anon");
            _infoh("addr", (unsigned long)addr);

            // write pattern and verify
            unsigned char* p = (unsigned char*)addr;
            for (int i = 0; i < 0x2000; i++) p[i] = (unsigned char)(i & 0xFF);
            int ok = 1;
            for (int i = 0; i < 0x2000; i++)
                if (p[i] != (unsigned char)(i & 0xFF)) { ok = 0; break; }
            if (ok) _pass("mmap write+read pattern");
            else _fail("mmap write+read", "pattern mismatch");

            // munmap
            long r = _sc2(SYS_MUNMAP, addr, 0x2000);
            if (r == 0) _pass("munmap");
            else { _fail("munmap", "nonzero return"); _info("r", r); }
        }
    }

    // ── mmap: two separate regions don't overlap ──────────────────────────
    {
        long a = _sc6(SYS_MMAP, 0, 0x1000,
                      PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
        long b = _sc6(SYS_MMAP, 0, 0x1000,
                      PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
        if (a > 0 && b > 0 && a != b) {
            _pass("two mmap regions distinct");
            _infoh("a", (unsigned long)a);
            _infoh("b", (unsigned long)b);
        } else {
            _fail("two mmap regions", "same or error");
        }
        if (a > 0) _sc2(SYS_MUNMAP, a, 0x1000);
        if (b > 0) _sc2(SYS_MUNMAP, b, 0x1000);
    }

    // ── mprotect stub (just verify it returns 0) ──────────────────────────
    {
        long a = _sc6(SYS_MMAP, 0, 0x1000,
                      PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
        if (a > 0) {
            long r = _sc3(SYS_MPROTECT, a, 0x1000, PROT_READ);
            if (r == 0) _pass("mprotect stub");
            else { _fail("mprotect", "nonzero"); _info("r", r); }
            _sc2(SYS_MUNMAP, a, 0x1000);
        }
    }

    _puts("\r\n");
    _exit(0);
}
