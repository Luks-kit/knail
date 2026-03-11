#pragma once
// include/scheduler.hpp - Knail preemptive round-robin scheduler
#include <stdint.h>
#include <stddef.h>
#include "vmm.hpp"

namespace fdtable { struct FDTable; } // forward declare to avoid circular include

namespace sched {

enum class State : uint8_t {
    Ready,
    Running,
    Blocked,
    Dead,
};

struct [[gnu::packed]] Context {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rbx;
    uint64_t rip;
};

struct Task {
    Context*  ctx;
    uint8_t*  stack;
    size_t    stack_size;
    uint8_t*  syscall_stack;
    State     state;
    uint32_t  tid;
    uint32_t  timeslice;
    uint32_t  quantum;
    Task*     next;
    char      name[32];
    vmm::AddressSpace    address_space;
    fdtable::FDTable*    fd_table;      // null for kernel tasks
};

static constexpr size_t   DEFAULT_STACK   = 16 * 1024;
static constexpr uint32_t DEFAULT_QUANTUM = 10;

void     init();
uint32_t spawn(void (*entry)(), const char* name,
               size_t stack_size = DEFAULT_STACK,
               uint32_t quantum  = DEFAULT_QUANTUM);
void     tick();
void     yield();
[[noreturn]] void exit();
Task*    current();
extern "C" void preempt_disable();
extern "C" void preempt_enable();
bool     preempt_enabled();
void     dump();
uint32_t fork_current(uint64_t user_rip, uint64_t user_rsp, uint64_t user_rflags);
uint32_t spawn_user(vmm::AddressSpace space,
                    const char* name,
                    uint64_t entry_point,
                    uint32_t quantum = DEFAULT_QUANTUM,
                    const char* argv[] = nullptr,
                    uint32_t argc = 0,
                    const char* envp[] = nullptr,
                    uint32_t envc = 0);

} // namespace sched
