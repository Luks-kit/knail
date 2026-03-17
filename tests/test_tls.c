// test_tls.c - Tests: arch_prctl ARCH_SET_FS / ARCH_GET_FS, context switch preservation
#include "syscall_stubs.h"

void _start() {
    _header("tls");

    // ── ARCH_SET_FS + ARCH_GET_FS roundtrip ───────────────────────────────
    {
        // Use a stack variable as a fake TLS block
        unsigned long tls_block[4] = { 0xDEADBEEF, 0xCAFEBABE, 0, 0 };
        // Per ABI: FS points to itself at offset 0 for self-pointer
        tls_block[0] = (unsigned long)tls_block;

        long r = _sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, (long)tls_block);
        if (r == 0) _pass("ARCH_SET_FS");
        else { _fail("ARCH_SET_FS", "error"); _info("r", r); }

        unsigned long readback = 0;
        r = _sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&readback);
        if (r == 0 && readback == (unsigned long)tls_block) {
            _pass("ARCH_GET_FS roundtrip");
            _infoh("fs_base", readback);
        } else {
            _fail("ARCH_GET_FS", "mismatch or error");
            _infoh("expected", (unsigned long)tls_block);
            _infoh("got", readback);
        }

        // Verify %fs segment actually works
        unsigned long via_fs;
        __asm__ volatile("mov %%fs:0, %0" : "=r"(via_fs));
        if (via_fs == (unsigned long)tls_block) _pass("FS segment reads self-pointer");
        else { _fail("FS segment read", "wrong value"); _infoh("got", via_fs); }
    }

    // ── FS preserved across yield ─────────────────────────────────────────
    {
        unsigned long tls_block[4];
        tls_block[0] = (unsigned long)tls_block;
        unsigned long sentinel = 0x1234567890ABCDEFULL;
        tls_block[1] = sentinel;

        _sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, (long)tls_block);
        _sc0(SYS_YIELD);

        unsigned long readback = 0;
        _sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&readback);
        if (readback == (unsigned long)tls_block) _pass("FS preserved across yield");
        else { _fail("FS after yield", "clobbered"); _infoh("got", readback); }
    }

    // ── FS preserved across fork ──────────────────────────────────────────
    {
        unsigned long tls_parent[4];
        tls_parent[0] = (unsigned long)tls_parent;
        tls_parent[1] = 0xABCDABCDABCDABCDULL;

        _sc2(SYS_ARCH_PRCTL, ARCH_SET_FS, (long)tls_parent);

        long r = _sc0(SYS_FORK);
        if (r == 0) {
            unsigned long child_fs = 0;
            _sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&child_fs);
            // Child gets a copy of parent's fs_base value — but it points
            // into parent's stack, so we only check the value was copied
            if (child_fs == (unsigned long)tls_parent) _exit(0);
            else _exit(1);
        } else if (r > 0) {
            int status = 0;
            _sc4(SYS_WAIT4, -1, (long)&status, 0, 0);
            int code = (status >> 8) & 0xFF;
            if (code == 0) _pass("FS copied to child on fork");
            else _fail("FS fork copy", "child saw wrong value");

            // verify parent's FS unchanged
            unsigned long parent_fs = 0;
            _sc2(SYS_ARCH_PRCTL, ARCH_GET_FS, (long)&parent_fs);
            if (parent_fs == (unsigned long)tls_parent) _pass("parent FS unchanged after fork");
            else _fail("parent FS after fork", "clobbered");
        }
    }

    _puts("\r\n");
    _exit(0);
}
