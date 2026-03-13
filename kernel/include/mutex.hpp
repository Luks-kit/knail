#pragma once
// include/mutex.hpp - Knail synchronization primitives

#include <stdint.h>

namespace sync {

// ── Spinlock ──────────────────────────────────────────────────────────────
// lock xchg is atomic on x86 without needing stdatomic — the LOCK prefix
// ensures the read-modify-write is indivisible across all cores.
struct Spinlock {
    volatile uint32_t flag = 0; // 0=free, 1=held

    void acquire() {
        while (true) {
            __asm__ volatile("cli");
            uint32_t old = 1;
            // LOCK XCHG: atomically swap flag with 1, get old value in old
            __asm__ volatile(
                "lock xchgl %0, %1"
                : "+r"(old), "+m"(flag)
                :: "memory"
            );
            if (old == 0) return; // we got it (flag was 0)
            __asm__ volatile("sti");
            __asm__ volatile("pause"); // hint to CPU we're spinning
        }
    }

    void release() {
        flag = 0;
        __asm__ volatile("sti" ::: "memory");
    }

    bool try_acquire() {
        __asm__ volatile("cli");
        uint32_t old = 1;
        __asm__ volatile(
            "lock xchgl %0, %1"
            : "+r"(old), "+m"(flag)
            :: "memory"
        );
        if (old == 0) return true;
        __asm__ volatile("sti");
        return false;
    }
};

// ── Mutex ─────────────────────────────────────────────────────────────────
// Sleeping mutex — blocks calling task rather than spinning.
// Must NOT be used from IRQ handlers.
struct Mutex {
    volatile int locked = 0;
    uint32_t     owner  = 0;
    Spinlock     guard;

    struct WaitNode {
        void*     task;
        WaitNode* next;
    };
    WaitNode* waiters = nullptr;

    void lock();
    void unlock();
    bool try_lock();
};

// ── ScopeLock ─────────────────────────────────────────────────────────────
template<typename T>
struct ScopeLock {
    T& lock_;
    explicit ScopeLock(T& l) : lock_(l) { lock_.acquire(); }
    ~ScopeLock()                         { lock_.release(); }
    ScopeLock(const ScopeLock&)            = delete;
    ScopeLock& operator=(const ScopeLock&) = delete;
};

template<>
struct ScopeLock<Mutex> {
    Mutex& lock_;
    explicit ScopeLock(Mutex& l) : lock_(l) { lock_.lock(); }
    ~ScopeLock()                              { lock_.unlock(); }
    ScopeLock(const ScopeLock&)               = delete;
    ScopeLock& operator=(const ScopeLock&)    = delete;
};

} // namespace sync
