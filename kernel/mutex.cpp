// kernel/mutex.cpp - Knail Mutex implementation

#include "mutex.hpp"
#include "scheduler.hpp"

namespace sync {

// ── Mutex::lock ───────────────────────────────────────────────────────────
void Mutex::lock() {
    while (true) {
        guard.acquire();

        if (!locked) {
            // Lock is free — take it
            locked = 1;
            owner  = sched::current() ? sched::current()->tid : 0;
            guard.release();
            return;
        }

        // Lock is held — add ourselves to wait queue and block
        sched::Task* me = sched::current();

        WaitNode node;
        node.task = me;
        node.next = waiters;
        waiters   = &node;

        // Mark as blocked before releasing guard so we don't miss a wakeup
        if (me) me->state = sched::State::Blocked;

        guard.release();

        // Yield — scheduler will pick someone else
        // We'll be woken by unlock() which sets us back to Ready
        sched::yield();

        // Loop back and try again (may have been woken spuriously)
    }
}

// ── Mutex::unlock ─────────────────────────────────────────────────────────
void Mutex::unlock() {
    guard.acquire();

    locked = 0;
    owner  = 0;

    // Wake the first waiter
    if (waiters) {
        WaitNode* w = waiters;
        waiters = w->next;
        sched::Task* t = reinterpret_cast<sched::Task*>(w->task);
        if (t) t->state = sched::State::Ready;
    }

    guard.release();
}

// ── Mutex::try_lock ───────────────────────────────────────────────────────
bool Mutex::try_lock() {
    guard.acquire();
    if (!locked) {
        locked = 1;
        owner  = sched::current() ? sched::current()->tid : 0;
        guard.release();
        return true;
    }
    guard.release();
    return false;
}

} // namespace sync
