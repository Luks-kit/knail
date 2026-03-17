// test_exec.c - Tests: execve (fork + exec test_io, verify it runs)
// Expects /disk/tests/test_io to exist on the disk image.
#include "syscall_stubs.h"

void _start() {
    _header("exec");

    long parent_pid = _sc0(SYS_GETPID);

    // ── fork + execve test_io ─────────────────────────────────────────────
    {
        long r = _sc0(SYS_FORK);
        if (r == 0) {
            // Child: exec test_io
            const char* argv[] = { "/disk/tests/io", 0 };
            const char* envp[] = { 0 };
            long e = _sc3(SYS_EXECVE, (long)"/disk/tests/io",
                          (long)argv, (long)envp);
            // Only reached if execve failed
            _puts("\033[31m[FAIL]\033[0m execve returned: ");
            _print_dec(e); _puts("\r\n");
            _exit(1);
        } else if (r > 0) {
            long child_tid = r;
            int status = 0;
            long waited = _sc4(SYS_WAIT4, -1, (long)&status, 0, 0);
            if (waited == child_tid) {
                int code = (status >> 8) & 0xFF;
                if (code == 0) _pass("execve: child exited 0");
                else { _fail("execve: child exit code", "nonzero"); _info("code", code); }
                _pass("execve: wait4 matched child");
            } else {
                _fail("execve: wait4", "wrong tid");
                _info("expected", child_tid); _info("got", waited);
            }
        } else {
            _fail("execve test: fork", "failed");
            _info("r", r);
        }
    }

    // ── execve nonexistent path ───────────────────────────────────────────
    {
        long r = _sc0(SYS_FORK);
        if (r == 0) {
            const char* argv[] = { "/disk/no/such/binary", 0 };
            const char* envp[] = { 0 };
            long e = _sc3(SYS_EXECVE, (long)"/disk/no/such/binary",
                          (long)argv, (long)envp);
            // execve should fail and return here
            if (e < 0) _exit(99); // signal "execve correctly failed"
            _exit(1);
        } else if (r > 0) {
            int status = 0;
            _sc4(SYS_WAIT4, -1, (long)&status, 0, 0);
            int code = (status >> 8) & 0xFF;
            if (code == 99) _pass("execve nonexistent returns error");
            else { _fail("execve nonexistent", "unexpected exit"); _info("code", code); }
        }
    }

    _puts("\r\n");
    _exit(0);
}
