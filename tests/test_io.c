// test_io.c - Tests: write, open, read, close, seek, stat, getcwd, fstat
#include "syscall_stubs.h"

struct stat_buf {
    unsigned long st_dev, st_ino;
    unsigned long st_nlink;
    unsigned int  st_mode, st_uid, st_gid;
    unsigned int  _pad;
    unsigned long st_rdev, st_size, st_blksize, st_blocks;
};

void _start() {
    _header("io");

    // ── write ─────────────────────────────────────────────────────────────
    {
        char hello[] = "hello\r\n";
        long r = _sc3(SYS_WRITE, 1, (long)hello, 7);
        if (r == 7) _pass("write stdout");
        else { _fail("write stdout", "wrong count"); _info("got", r); }
    }

    // ── open + read ───────────────────────────────────────────────────────
    {
        long fd = _sc2(SYS_OPEN, (long)"/disk/etc/motd", O_RDONLY);
        if (fd < 0) {
            _fail("open /disk/etc/motd", "returned error");
            _info("fd", fd);
        } else {
            char buf[64] = {0};
            long n = _sc3(SYS_READ, fd, (long)buf, sizeof(buf)-1);
            if (n > 0) {
                _pass("open+read motd");
                _puts("      content: "); _puts(buf); _puts("\r\n");
            } else {
                _fail("read motd", "zero/negative");
                _info("n", n);
            }

            // ── seek ──────────────────────────────────────────────────────
            long pos = _sc3(SYS_SEEK, fd, 0, 0); // SEEK_SET to 0
            if (pos == 0) _pass("seek SEEK_SET");
            else { _fail("seek", "expected 0"); _info("pos", pos); }

            // ── fstat ─────────────────────────────────────────────────────
            struct stat_buf sb = {0};
            long r = _sc2(SYS_FSTAT, fd, (long)&sb);
            if (r == 0 && sb.st_size > 0) {
                _pass("fstat");
                _info("size", (long)sb.st_size);
            } else {
                _fail("fstat", "bad result");
                _info("r", r); _info("size", (long)sb.st_size);
            }

            _sc1(SYS_CLOSE, fd);
        }
    }

    // ── stat ──────────────────────────────────────────────────────────────
    {
        struct stat_buf sb = {0};
        long r = _sc2(SYS_STAT, (long)"/disk/etc", (long)&sb);
        if (r == 0) _pass("stat /disk/etc");
        else { _fail("stat", "error"); _info("r", r); }
    }

    // ── getcwd ────────────────────────────────────────────────────────────
    {
        char buf[128] = {0};
        long r = _sc2(SYS_GETCWD, (long)buf, sizeof(buf));
        if (r > 0) {
            _pass("getcwd");
            _puts("      cwd: "); _puts(buf); _puts("\r\n");
        } else { _fail("getcwd", "error"); _info("r", r); }
    }

    // ── open nonexistent ─────────────────────────────────────────────────
    {
        long fd = _sc2(SYS_OPEN, (long)"/disk/does/not/exist", O_RDONLY);
        if (fd < 0) _pass("open nonexistent returns error");
        else { _fail("open nonexistent", "should have failed"); _sc1(SYS_CLOSE, fd); }
    }

    // ── write + creat ─────────────────────────────────────────────────────
    {
        long fd = _sc2(SYS_OPEN, (long)"/disk/tmp/test_io.txt",
                       O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) { _fail("open creat", "error"); _info("fd", fd); }
        else {
            long w = _sc3(SYS_WRITE, fd, (long)"knail test\n", 11);
            _sc1(SYS_CLOSE, fd);
            if (w == 11) _pass("write creat");
            else { _fail("write creat", "short write"); _info("w", w); }

            // read it back
            fd = _sc2(SYS_OPEN, (long)"/disk/tmp/test_io.txt", O_RDONLY);
            if (fd >= 0) {
                char buf[32] = {0};
                long n = _sc3(SYS_READ, fd, (long)buf, sizeof(buf)-1);
                _sc1(SYS_CLOSE, fd);
                if (n == 11) _pass("readback written file");
                else { _fail("readback", "wrong count"); _info("n", n); }
            }
        }
    }

    _puts("\r\n");
    _exit(0);
}
