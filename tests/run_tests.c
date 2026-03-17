// run_tests.c - Test runner: fork+exec each test binary, report pass/fail
// Build and place at /disk/tests/run_tests
// Called from shell: run_tests
#include "syscall_stubs.h"

static const char* tests[] = {
    "/disk/tests/io",
    "/disk/tests/fork",
    "/disk/tests/mm",
    "/disk/tests/tls",
    "/disk/tests/time",
    "/disk/tests/ioctl",
    "/disk/tests/exec",  // last: it execves test_io internally
    0
};

static void run_one(const char* path) {
    _puts("\033[1;33m>>> "); _puts(path); _puts("\033[0m\r\n");

    long r = _sc0(SYS_FORK);
    if (r == 0) {
        const char* argv[2] = { path, 0 };
        const char* envp[1] = { 0 };
        long e = _sc3(SYS_EXECVE, (long)path, (long)argv, (long)envp);
        _puts("\033[31m[ERROR]\033[0m execve failed: ");
        _print_dec(e); _puts("\r\n");
        _exit(1);
    } else if (r > 0) {
        int status = 0;
        long waited = _sc4(SYS_WAIT4, -1, (long)&status, 0, 0);
        int code = (status >> 8) & 0xFF;
        if (code == 0) {
            _puts("\033[32m>>> PASSED\033[0m\r\n\r\n");
        } else {
            _puts("\033[31m>>> FAILED (exit "); _print_dec(code);
            _puts(")\033[0m\r\n\r\n");
        }
    } else {
        _puts("\033[31m[ERROR]\033[0m fork failed\r\n");
    }
}

void _start() {
    _puts("\033[1;35m");
    _puts("|==============================|\r\n");
    _puts("|   Knail Syscall Test Suite   |\r\n");
    _puts("|==============================|\r\n");
    _puts("\033[0m\r\n");

    for (int i = 0; tests[i]; i++)
        run_one(tests[i]);

    _puts("\033[1mAll tests complete.\033[0m\r\n");
    _exit(0);
}
