#pragma once
// include/scheduler.hpp - Knail preemptive round-robin scheduler
#include <stdint.h>
#include <stddef.h>
#include "vmm.hpp"
#include "vfs.hpp"
#include "types.hpp"

namespace fdtable { struct FDTable; }

namespace sched {

enum class State : u8 {
    Ready,
    Running,
    Blocked,
    Dead,
};

struct [[gnu::packed]] Context {
    u64 r15, r14, r13, r12, r11, r10, r9, r8;
    u64 rbp, rbx;
    u64 rip;
};

struct Task {
    Context*  ctx;
    u8*       stack;
    usize     stack_size;
    u8*       syscall_stack;
    State     state;
    u32       tid;
    u32       parent_tid;
    i32       exit_code;
    u32       timeslice;
    u32       quantum;
    Task*     next;
    char      name[32];
    vmm::AddressSpace  address_space;
    vfs::VNode*        cwd;
    vfs::VNode*        root;
    fdtable::FDTable*  fd_table;
};

static constexpr usize DEFAULT_STACK   = 16 * 1024;
static constexpr u32   DEFAULT_QUANTUM = 10;

void init();

kResult<u32> spawn(void (*entry)(), const char* name,
                   usize stack_size = DEFAULT_STACK,
                   u32   quantum    = DEFAULT_QUANTUM);

void tick();
void yield();
[[noreturn]] void exit(i32 code);

// Returns kError::NotFound if no children, otherwise tid+code via out params.
kStatus      wait(u32* out_tid, i32* out_code);

Task*        current();
void         block_current();
void         wake(Task* t);

extern "C" void preempt_disable();
extern "C" void preempt_enable();
bool         preempt_enabled();

void         dump();

kResult<u32> fork_current(u64 user_rip, u64 user_rsp, u64 user_rflags);

kResult<u32> spawn_user(vmm::AddressSpace space,
                        const char* name,
                        u64 entry_point,
                        u32 quantum         = DEFAULT_QUANTUM,
                        const char* argv[]  = nullptr,
                        u32 argc            = 0,
                        const char* envp[]  = nullptr,
                        u32 envc            = 0);

} // namespace sched
