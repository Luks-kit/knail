// test_time.c - Tests: gettimeofday, clock_gettime (REALTIME + MONOTONIC), nanosleep
#include "syscall_stubs.h"

struct timeval  { long tv_sec; long tv_usec; };
struct timespec { long tv_sec; long tv_nsec; };

void _start() {
    _header("time");

    // ── gettimeofday ──────────────────────────────────────────────────────
    {
        struct timeval tv = {0, 0};
        long r = _sc2(SYS_GETTIMEOFDAY, (long)&tv, 0);
        if (r == 0 && tv.tv_sec > 0) {
            _pass("gettimeofday");
            _info("tv_sec", tv.tv_sec);
            _info("tv_usec", tv.tv_usec);
        } else {
            _fail("gettimeofday", "bad return");
            _info("r", r); _info("tv_sec", tv.tv_sec);
        }
    }

    // ── clock_gettime CLOCK_REALTIME ──────────────────────────────────────
    {
        struct timespec ts = {0, 0};
        long r = _sc2(SYS_CLOCK_GETTIME, CLOCK_REALTIME, (long)&ts);
        if (r == 0 && ts.tv_sec > 0) {
            _pass("clock_gettime REALTIME");
            _info("tv_sec", ts.tv_sec);
        } else {
            _fail("clock_gettime REALTIME", "bad return");
            _info("r", r); _info("tv_sec", ts.tv_sec);
        }
    }

    // ── clock_gettime CLOCK_MONOTONIC ─────────────────────────────────────
    {
        struct timespec t1 = {0, 0}, t2 = {0, 0};
        _sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        _sc0(SYS_YIELD);
        _sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);

        long ok = (t2.tv_sec > t1.tv_sec) ||
                  (t2.tv_sec == t1.tv_sec && t2.tv_nsec >= t1.tv_nsec);
        if (ok) {
            _pass("clock_gettime MONOTONIC advances");
            _info("t1_sec", t1.tv_sec); _info("t1_nsec", t1.tv_nsec);
            _info("t2_sec", t2.tv_sec); _info("t2_nsec", t2.tv_nsec);
        } else {
            _fail("MONOTONIC", "did not advance");
        }
    }

    // ── nanosleep ─────────────────────────────────────────────────────────
    {
        struct timespec t1 = {0,0}, t2 = {0,0};
        struct timespec req = {0, 100000000}; // 100ms = 10 ticks at 100Hz
        struct timespec rem = {0, 0};
        _sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t1);
        long r = _sc2(SYS_NANOSLEEP, (long)&req, (long)&rem);
        _sc2(SYS_CLOCK_GETTIME, CLOCK_MONOTONIC, (long)&t2);

        if (r == 0) _pass("nanosleep returned 0");
        else { _fail("nanosleep", "nonzero return"); _info("r", r); }

        long elapsed_ns = (t2.tv_sec - t1.tv_sec) * 1000000000L
                        + (t2.tv_nsec - t1.tv_nsec);
        if (elapsed_ns >= 80000000L) { // at least ~80ms (some slack)
            _pass("nanosleep slept expected duration");
        } else {
            _fail("nanosleep duration", "too short");
            _info("elapsed_ns", elapsed_ns);
        }
    }

    _puts("\r\n");
    _exit(0);
}
