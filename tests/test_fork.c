// test_fork.c - Tests: fork, wait4, getpid, gettid, exit code propagation
#include "syscall_stubs.h"

void _start() {
    _header("fork");

    // ── getpid / gettid ───────────────────────────────────────────────────
    {
        long pid = _sc0(SYS_GETPID);
        long tid = _sc0(SYS_GETTID);
        if (pid > 0) { _pass("getpid"); _info("pid", pid); }
        else _fail("getpid", "returned <= 0");
        if (tid > 0) { _pass("gettid"); _info("tid", tid); }
        else _fail("gettid", "returned <= 0");
    }

    // ── fork: child sees 0, parent sees child tid ─────────────────────────
    {
        long parent_pid = _sc0(SYS_GETPID);
        long r = _sc0(SYS_FORK);

        if (r == 0) {
            // Child
            long child_pid = _sc0(SYS_GETPID);
            if (child_pid != parent_pid) _pass("child: getpid differs from parent");
            else _fail("child: getpid", "same as parent");
            _exit(42); // distinctive exit code
        } else if (r > 0) {
            // Parent
            long child_tid = r;
            _pass("fork returned child tid");
            _info("child_tid", child_tid);

            // ── wait4 ─────────────────────────────────────────────────────
            int status = 0;
            long waited = _sc4(SYS_WAIT4, -1, (long)&status, 0, 0);
            if (waited == child_tid) {
                _pass("wait4 returned child tid");
                int exit_code = (status >> 8) & 0xFF;
                if (exit_code == 42) _pass("child exit code propagated");
                else { _fail("exit code", "expected 42"); _info("got", exit_code); }
            } else {
                _fail("wait4", "wrong tid");
                _info("expected", child_tid);
                _info("got", waited);
            }
        } else {
            _fail("fork", "returned negative");
            _info("r", r);
        }
    }

    // ── fork + child writes to separate fd ───────────────────────────────
    {
        long fd = _sc2(SYS_OPEN, (long)"/disk/tmp/fork_test.txt",
                       O_WRONLY | O_CREAT | O_TRUNC);
        if (fd < 0) { _fail("fork write test: open", "failed"); }
        else {
            long r = _sc0(SYS_FORK);
            if (r == 0) {
                _sc3(SYS_WRITE, fd, (long)"child\n", 6);
                _sc1(SYS_CLOSE, fd);
                _exit(0);
            } else {
                _sc1(SYS_CLOSE, fd); // parent closes its copy
                _sc4(SYS_WAIT4, -1, 0, 0, 0);
                // reopen and verify
                fd = _sc2(SYS_OPEN, (long)"/disk/tmp/fork_test.txt", O_RDONLY);
                if (fd >= 0) {
                    char buf[16] = {0};
                    long n = _sc3(SYS_READ, fd, (long)buf, sizeof(buf)-1);
                    _sc1(SYS_CLOSE, fd);
                    if (n == 6) _pass("child wrote to file");
                    else { _fail("child write verify", "wrong count"); _info("n", n); }
                }
            }
        }
    }

    _puts("\r\n");
    _exit(0);
}
