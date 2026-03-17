// test_ioctl.c - Tests: ioctl (TIOCGWINSZ, TCGETS), fcntl, pipe, dup, dup2
#include "syscall_stubs.h"

struct winsize  { unsigned short ws_row, ws_col, ws_xpixel, ws_ypixel; };
struct termios  {
    unsigned int c_iflag, c_oflag, c_cflag, c_lflag;
    unsigned char c_line, c_cc[19];
    unsigned int c_ispeed, c_ospeed;
};

#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4

void _start() {
    _header("ioctl");

    // ── TIOCGWINSZ ────────────────────────────────────────────────────────
    {
        struct winsize ws = {0};
        long r = _sc3(SYS_IOCTL, 1, TIOCGWINSZ, (long)&ws);
        if (r == 0 && ws.ws_row == 24 && ws.ws_col == 80) {
            _pass("TIOCGWINSZ");
            _info("rows", ws.ws_row); _info("cols", ws.ws_col);
        } else {
            _fail("TIOCGWINSZ", "bad values");
            _info("r", r); _info("rows", ws.ws_row); _info("cols", ws.ws_col);
        }
    }

    // ── TCGETS ────────────────────────────────────────────────────────────
    {
        struct termios t = {0};
        long r = _sc3(SYS_IOCTL, 1, TCGETS, (long)&t);
        if (r == 0 && t.c_ispeed == 15) {
            _pass("TCGETS");
            _info("ispeed", t.c_ispeed);
        } else {
            _fail("TCGETS", "bad values");
            _info("r", r); _info("ispeed", t.c_ispeed);
        }
    }

    // ── fcntl F_GETFL ─────────────────────────────────────────────────────
    {
        long r = _sc2(SYS_IOCTL + 56, 1, F_GETFL); // fcntl is 72
        // use raw syscall number
        long r2;
        __asm__ volatile("syscall" : "=a"(r2)
            : "a"(72L), "D"(1L), "S"((long)F_GETFL)
            : "rcx","r11","memory");
        if (r2 >= 0) { _pass("fcntl F_GETFL"); _info("flags", r2); }
        else { _fail("fcntl F_GETFL", "error"); _info("r", r2); }
    }

    // ── pipe ─────────────────────────────────────────────────────────────
    {
        int fds[2] = {-1, -1};
        long r = _sc1(SYS_PIPE, (long)fds);
        if (r == 0 && fds[0] >= 0 && fds[1] >= 0) {
            _pass("pipe");
            _info("rfd", fds[0]); _info("wfd", fds[1]);

            // write through pipe, read back
            long w = _sc3(SYS_WRITE, fds[1], (long)"pipedata", 8);
            char buf[16] = {0};
            long n = _sc3(SYS_READ, fds[0], (long)buf, 8);
            if (w == 8 && n == 8) _pass("pipe write+read");
            else { _fail("pipe write+read", "count mismatch"); _info("w",w); _info("n",n); }

            _sc1(SYS_CLOSE, fds[0]);
            _sc1(SYS_CLOSE, fds[1]);
        } else {
            _fail("pipe", "error");
            _info("r", r); _info("fds0", fds[0]); _info("fds1", fds[1]);
        }
    }

    // ── dup ───────────────────────────────────────────────────────────────
    {
        int fds[2] = {-1,-1};
        _sc1(SYS_PIPE, (long)fds);
        long dup_fd = _sc1(SYS_DUP, fds[1]);
        if (dup_fd >= 0 && dup_fd != fds[1]) {
            _pass("dup");
            // write via dup, read via original rfd
            _sc3(SYS_WRITE, dup_fd, (long)"dup!", 4);
            char buf[8] = {0};
            long n = _sc3(SYS_READ, fds[0], (long)buf, 4);
            if (n == 4) _pass("dup write+read");
            else { _fail("dup write+read", "wrong count"); _info("n", n); }
            _sc1(SYS_CLOSE, dup_fd);
        } else { _fail("dup", "error"); _info("dup_fd", dup_fd); }
        _sc1(SYS_CLOSE, fds[0]);
        _sc1(SYS_CLOSE, fds[1]);
    }

    // ── dup2 ──────────────────────────────────────────────────────────────
    {
        int fds[2] = {-1,-1};
        _sc1(SYS_PIPE, (long)fds);
        // dup write-end onto fd 10
        long r = _sc2(SYS_DUP2, fds[1], 10);
        if (r == 10) {
            _pass("dup2");
            _sc3(SYS_WRITE, 10, (long)"dup2!", 5);
            char buf[8] = {0};
            long n = _sc3(SYS_READ, fds[0], (long)buf, 5);
            if (n == 5) _pass("dup2 write+read");
            else { _fail("dup2 write+read", "wrong count"); _info("n", n); }
            _sc1(SYS_CLOSE, 10);
        } else { _fail("dup2", "wrong fd returned"); _info("r", r); }
        _sc1(SYS_CLOSE, fds[0]);
        _sc1(SYS_CLOSE, fds[1]);
    }

    _puts("\r\n");
    _exit(0);
}
