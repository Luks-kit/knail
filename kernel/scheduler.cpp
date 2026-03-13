// kernel/scheduler.cpp - Knail preemptive round-robin scheduler
#include "scheduler.hpp"
#include "fdtable.hpp"
#include "heap.hpp"
#include "syscall.hpp"
#include "vfs.hpp"
#include "vga.hpp"
#include "serial.hpp"
#include "pmm.hpp"
#include "gdt.hpp"
#include "vmm.hpp"
#include "elf.hpp"
#include "spawn.hpp"
#include "types.hpp"

// AT auxv constants — used by spawn_elf
static constexpr u64 AT_NULL   = 0;
static constexpr u64 AT_PAGESZ = 6;
static constexpr u64 AT_ENTRY  = 9;
static constexpr u64 AT_UID    = 11;
static constexpr u64 AT_EUID   = 12;
static constexpr u64 AT_GID    = 13;
static constexpr u64 AT_EGID   = 14;

namespace syscall { void update_kernel_stack(u64 rsp); }

namespace sched {

static inline u64 irq_save_disable() {
    u64 flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static inline void irq_restore(u64 flags) {
    __asm__ volatile("push %0; popfq" :: "r"(flags) : "memory", "cc");
}

extern "C" void switch_context(Context** old_ctx, Context** new_ctx);
extern "C" void user_task_trampoline();

static Task*    run_queue    = nullptr;
static Task*    current_task = nullptr;
static Task*    zombie_list  = nullptr;
static u32      next_tid     = 1;
static volatile int preempt_count = 0;

static Task    idle_task;
static u8      idle_stack[4096];

static void task_entry_trampoline() {
    u64 entry_addr;
    __asm__ volatile("mov %%r15, %0" : "=r"(entry_addr));
    reinterpret_cast<void(*)()>(entry_addr)();
    sched::exit(0);
}

static Task* alloc_task() {
    return reinterpret_cast<Task*>(heap::kmalloc(sizeof(Task)));
}

static void reap_zombies() {
    while (zombie_list) {
        Task* dead = zombie_list;
        zombie_list = zombie_list->next;
        if (dead->stack)         heap::kfree(dead->stack);
        if (dead->syscall_stack) heap::kfree(dead->syscall_stack);
        heap::kfree(dead);
    }
}

static void enqueue(Task* t) {
    if (!run_queue) {
        t->next   = t;
        run_queue = t;
    } else {
        t->next         = run_queue->next;
        run_queue->next = t;
    }
}

static void unlink_task(Task* victim) {
    if (!run_queue || !victim) return;
    Task* prev = run_queue;
    while (prev->next != victim && prev->next != run_queue)
        prev = prev->next;
    if (prev->next != victim) return;
    if (victim->next == victim) {
        run_queue = nullptr;
    } else {
        prev->next = victim->next;
        if (run_queue == victim) run_queue = prev;
    }
    victim->next = nullptr;
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    idle_task.state         = State::Running;
    idle_task.tid           = 0;
    idle_task.quantum       = 1;
    idle_task.timeslice     = 1;
    idle_task.stack         = idle_stack;
    idle_task.stack_size    = sizeof(idle_stack);
    idle_task.syscall_stack = nullptr;
    idle_task.fd_table      = nullptr;
    idle_task.next          = &idle_task;
    idle_task.name[0] = 'i'; idle_task.name[1] = 'd';
    idle_task.name[2] = 'l'; idle_task.name[3] = 'e';
    idle_task.name[4] = 0;
    idle_task.address_space.pml4 = nullptr;
    idle_task.cwd = nullptr;

    u64* sp = reinterpret_cast<u64*>(idle_stack + sizeof(idle_stack));
    *(--sp) = 0; // rip
    *(--sp) = 0; // r15
    *(--sp) = 0; // r14
    *(--sp) = 0; // r13
    *(--sp) = 0; // r12
    *(--sp) = 0; // r11
    *(--sp) = 0; // r10
    *(--sp) = 0; // r9
    *(--sp) = 0; // r8
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    idle_task.ctx = reinterpret_cast<Context*>(sp);

    run_queue    = &idle_task;
    current_task = &idle_task;
}

// ── spawn ─────────────────────────────────────────────────────────────────
kResult<u32> spawn(void (*entry)(), const char* name,
                   usize stack_size, u32 quantum) {
    Task* t = alloc_task();
    if (!t) return kResult<u32>::err(kError::OutOfMemory);

    u8* stack = reinterpret_cast<u8*>(heap::kmalloc(stack_size));
    if (!stack) {
        heap::kfree(t);
        return kResult<u32>::err(kError::OutOfMemory);
    }

    t->stack         = stack;
    t->stack_size    = stack_size;
    t->syscall_stack = nullptr;
    t->fd_table      = nullptr;
    t->state         = State::Ready;
    t->tid           = next_tid++;
    t->quantum       = quantum;
    t->timeslice     = quantum;
    t->next          = nullptr;
    t->address_space.pml4 = nullptr;

    usize i = 0;
    while (name[i] && i < 31) { t->name[i] = name[i]; i++; }
    t->name[i] = 0;

    u64* sp = reinterpret_cast<u64*>(stack + stack_size);
    *(--sp) = reinterpret_cast<u64>(task_entry_trampoline);
    *(--sp) = reinterpret_cast<u64>(entry); // r15
    *(--sp) = 0; // r14
    *(--sp) = 0; // r13
    *(--sp) = 0; // r12
    *(--sp) = 0; // r11
    *(--sp) = 0; // r10
    *(--sp) = 0; // r9
    *(--sp) = 0; // r8
    *(--sp) = 0; // rbp
    *(--sp) = 0; // rbx
    t->ctx = reinterpret_cast<Context*>(sp);

    u64 irq_flags = irq_save_disable();
    enqueue(t);
    irq_restore(irq_flags);

    return kResult<u32>::ok(t->tid);
}

// ── pick_next ─────────────────────────────────────────────────────────────
static Task* pick_next() {
    Task* start = run_queue;
    Task* t     = start->next;
    do {
        if (t->state == State::Ready || t->state == State::Running)
            return t;
        t = t->next;
    } while (t != start);
    return &idle_task;
}

// ── do_switch ─────────────────────────────────────────────────────────────
static void do_switch(Task* next) {
    if (next == current_task) return;
    Task* prev = current_task;
    if (prev->state == State::Running)
        prev->state = State::Ready;
    next->state     = State::Running;
    next->timeslice = next->quantum;
    run_queue       = next;
    current_task    = next;

    gdt::set_kernel_stack(
        reinterpret_cast<u64>(next->stack + next->stack_size));

    u64 syscall_top = next->syscall_stack
        ? reinterpret_cast<u64>(next->syscall_stack + DEFAULT_STACK)
        : reinterpret_cast<u64>(next->stack + next->stack_size);
    syscall::update_kernel_stack(syscall_top);

    if (next->address_space.pml4)
        vmm::activate(next->address_space);
    else
        vmm::activate(vmm::kernel_space);

    switch_context(&prev->ctx, &next->ctx);
}

// ── preemption control ────────────────────────────────────────────────────
extern "C" void preempt_disable() { preempt_count++; }
extern "C" void preempt_enable()  { if (preempt_count > 0) preempt_count--; }
bool preempt_enabled() { return preempt_count == 0; }

// ── tick ──────────────────────────────────────────────────────────────────
void tick() {
    reap_zombies();
    if (!current_task) return;
    if (current_task->timeslice > 0)
        current_task->timeslice--;
    if (current_task->timeslice == 0 && preempt_count == 0)
        do_switch(pick_next());
}

// ── yield ─────────────────────────────────────────────────────────────────
void yield() {
    u64 irq_flags = irq_save_disable();
    reap_zombies();
    current_task->timeslice = 0;
    do_switch(pick_next());
    irq_restore(irq_flags);
}

// ── exit ──────────────────────────────────────────────────────────────────
[[noreturn]] void exit(i32 code) {
    u64 irq_flags = irq_save_disable();
    Task* dead = current_task;
    dead->exit_code = code;
    dead->state     = State::Dead;

    if (dead->fd_table) {
        fdtable::free(dead->fd_table);
        dead->fd_table = nullptr;
    }

    // Reparent children to init (tid=1)
    Task* t = run_queue;
    if (t) {
        Task* start = t;
        do {
            if (t->parent_tid == dead->tid)
                t->parent_tid = 1;
            t = t->next;
        } while (t != start);
    }

    // Wake parent if blocked waiting
    if (dead->parent_tid) {
        Task* t = run_queue;
        if (t) {
            Task* start = t;
            do {
                if (t->tid == dead->parent_tid && t->state == State::Blocked) {
                    t->state = State::Ready;
                    break;
                }
                t = t->next;
            } while (t != start);
        }
    }

    unlink_task(dead);
    dead->next  = zombie_list;
    zombie_list = dead;

    if (!run_queue) {
        irq_restore(irq_flags);
        for (;;) __asm__ volatile("hlt");
    }

    do_switch(pick_next());
    irq_restore(irq_flags);
    for (;;) __asm__ volatile("hlt");
}

extern "C" [[noreturn]] void sched_exit(int code) {
    sched::exit(static_cast<i32>(code));
}

void block_current() {
    u64 irq_flags = irq_save_disable();
    current_task->state = State::Blocked;
    do_switch(pick_next());
    irq_restore(irq_flags);
}

void wake(Task* t) {
    if (!t) return;
    if (t->state != State::Blocked) return;
    t->state = State::Ready;
    if (current_task == &idle_task)
        do_switch(t);
}

// ── wait ──────────────────────────────────────────────────────────────────
kStatus wait(u32* out_tid, i32* out_code) {
    while (true) {
        u64 irq_flags = irq_save_disable();
        bool any_child = false;

        // Check zombie list for already-dead children
        Task* prev = nullptr;
        Task* z    = zombie_list;
        while (z) {
            if (z->parent_tid == current_task->tid) {
                if (prev) prev->next = z->next;
                else      zombie_list = z->next;

                u32 tid  = z->tid;
                i32 code = z->exit_code;

                if (z->stack)         heap::kfree(z->stack);
                if (z->syscall_stack) heap::kfree(z->syscall_stack);
                heap::kfree(z);

                irq_restore(irq_flags);
                if (out_tid)  *out_tid  = tid;
                if (out_code) *out_code = code;
                return kStatus::ok();
            }
            any_child = true;
            prev = z;
            z    = z->next;
        }

        // Check run_queue for living children
        if (run_queue) {
            Task* start = run_queue;
            Task* t     = start;
            do {
                if (t->parent_tid == current_task->tid) {
                    any_child = true;
                    break;
                }
                t = t->next;
            } while (t != start);
        }

        if (!any_child) {
            irq_restore(irq_flags);
            return kStatus::err(kError::NotFound); // ECHILD equivalent
        }

        current_task->state = State::Blocked;
        do_switch(pick_next());
        irq_restore(irq_flags);
    }
}

// ── current ───────────────────────────────────────────────────────────────
Task* current() { return current_task; }

// ── dump ──────────────────────────────────────────────────────────────────
void dump() {
    u64 irq_flags = irq_save_disable();
    static const char* state_names[] = { "Ready", "Running", "Blocked", "Dead" };

    vga::set_color(vga::Color::LightCyan, vga::Color::Black);
    vga::write_line("--- Task list ---");
    serial::write_line("--- Task list ---");

    if (!run_queue) {
        vga::write_line("  (empty)");
        irq_restore(irq_flags);
        return;
    }

    Task*  start = &idle_task;
    Task*  t     = start;
    u32    seen  = 0;
    do {
        if (!t || seen > 64) break;
        seen++;
        u8          st        = (u8)t->state;
        const char* state_str = (st < 4) ? state_names[st] : "???";

        vga::write("  tid=");   vga::write_dec(t->tid);
        vga::write("  [");      vga::write(state_str); vga::write("]");
        vga::write("  slice="); vga::write_dec(t->timeslice);
        vga::write("  next=");  vga::write_hex((u64)t->next);
        vga::write("  ");
        for (int i = 0; i < 31 && t->name[i]; i++) vga::put_char(t->name[i]);
        vga::write_line("");

        serial::write("  tid=");  serial::write_dec(t->tid);
        serial::write("  [");     serial::write(state_str);
        serial::write("] next="); serial::write_hex((u64)t->next);
        serial::write_line("");

        t = t->next;
    } while (t && t != start);

    irq_restore(irq_flags);
}

// ── fork_current ──────────────────────────────────────────────────────────
kResult<u32> fork_current(u64 user_rip, u64 user_rsp, u64 user_rflags) {
    Task* parent = current_task;
    if (!parent || !parent->address_space.pml4)
        return kResult<u32>::err(kError::InvalidArgument);

    vmm::AddressSpace child_space = vmm::clone_user_space(parent->address_space);
    if (!child_space.pml4)
        return kResult<u32>::err(kError::OutOfMemory);

    Task* child = alloc_task();
    if (!child) {
        vmm::destroy_user_space(child_space);
        return kResult<u32>::err(kError::OutOfMemory);
    }

    u8* kstack  = reinterpret_cast<u8*>(heap::kmalloc(DEFAULT_STACK));
    u8* scstack = reinterpret_cast<u8*>(heap::kmalloc(DEFAULT_STACK));
    if (!kstack || !scstack) {
        if (kstack)  heap::kfree(kstack);
        if (scstack) heap::kfree(scstack);
        heap::kfree(child);
        vmm::destroy_user_space(child_space);
        return kResult<u32>::err(kError::OutOfMemory);
    }

    fdtable::FDTable* fdt = fdtable::clone(parent->fd_table);
    if (!fdt) {
        heap::kfree(scstack);
        heap::kfree(kstack);
        heap::kfree(child);
        vmm::destroy_user_space(child_space);
        return kResult<u32>::err(kError::OutOfMemory);
    }

    child->stack         = kstack;
    child->stack_size    = DEFAULT_STACK;
    child->syscall_stack = scstack;
    child->fd_table      = fdt;
    child->state         = State::Ready;
    child->tid           = next_tid++;
    child->quantum       = parent->quantum;
    child->timeslice     = parent->quantum;
    child->next          = nullptr;
    child->address_space = child_space;

    usize i = 0;
    while (parent->name[i] && i < 31) { child->name[i] = parent->name[i]; i++; }
    child->name[i] = 0;

    u64* ksp = reinterpret_cast<u64*>(kstack + DEFAULT_STACK);
    *(--ksp) = (u64)(gdt::USER_DATA | 3); // ss
    *(--ksp) = user_rsp;                  // rsp
    *(--ksp) = user_rflags;               // rflags
    *(--ksp) = (u64)(gdt::USER_CODE | 3); // cs
    *(--ksp) = user_rip;                  // rip
    *(--ksp) = reinterpret_cast<u64>(user_task_trampoline);
    *(--ksp) = 0; // r15
    *(--ksp) = 0; // r14
    *(--ksp) = 0; // r13
    *(--ksp) = 0; // r12
    *(--ksp) = 0; // r11
    *(--ksp) = 0; // r10
    *(--ksp) = 0; // r9
    *(--ksp) = 0; // r8
    *(--ksp) = 0; // rbp
    *(--ksp) = 0; // rbx
    child->ctx = reinterpret_cast<Context*>(ksp);

    u64 irq_flags = irq_save_disable();
    enqueue(child);
    irq_restore(irq_flags);

    return kResult<u32>::ok(child->tid);
}

} // namespace sched

// ── init_stdio ────────────────────────────────────────────────────────────
static void init_stdio(fdtable::FDTable* fdt) {
    auto tty_r = vfs::open("/dev/tty", VFS_READ);
    auto tty_w = vfs::open("/dev/tty", VFS_WRITE);
    auto tty_e = vfs::open("/dev/tty", VFS_WRITE);

    if (tty_r.is_ok()) fdtable::install_at(fdt, tty_r.value(), 0);
    if (tty_w.is_ok()) fdtable::install_at(fdt, tty_w.value(), 1);
    if (tty_e.is_ok()) fdtable::install_at(fdt, tty_e.value(), 2);
}

// ── spawn_user ────────────────────────────────────────────────────────────
kResult<u32> sched::spawn_user(vmm::AddressSpace space,
                                const char* name,
                                u64 entry_point,
                                u32 quantum,
                                const char** argv,
                                u32 argc,
                                const char** envp,
                                u32 envc) {
    static constexpr u64 STACK_BUF_SIZE    = 4096;
    static constexpr u32 MAX_STACK_STRINGS = 32;

    // Map user stack pages
    for (u64 off = 0; off < vmm::USER_STACK_SIZE; off += 0x1000) {
        u64 frame = pmm::alloc_frame();
        if (!frame) return kResult<u32>::err(kError::OutOfMemory);
        if (!vmm::map_page(space,
                           vmm::USER_STACK_TOP - vmm::USER_STACK_SIZE + off,
                           frame,
                           vmm::FLAG_PRESENT | vmm::FLAG_WRITE | vmm::FLAG_USER))
            return kResult<u32>::err(kError::OutOfMemory);
    }

    // Build argc/argv/envp/auxv stack image in kernel buffer
    u8* stack_buf = reinterpret_cast<u8*>(heap::kmalloc(STACK_BUF_SIZE));
    if (!stack_buf) return kResult<u32>::err(kError::OutOfMemory);
    for (u32 i = 0; i < STACK_BUF_SIZE; i++) stack_buf[i] = 0;

    u8*  buf_top = stack_buf + STACK_BUF_SIZE;
    u64* sp      = reinterpret_cast<u64*>(buf_top);

    auto to_user_va = [&](void* buf_ptr) -> u64 {
        u64 offset = (u64)buf_top - (u64)buf_ptr;
        return vmm::USER_STACK_TOP - offset;
    };

    auto count_strings = [](const char* const* strs, u32 explicit_count) -> u32 {
        if (!strs) return 0;
        if (explicit_count > 0) return explicit_count;
        u32 count = 0;
        while (strs[count] && count < 32) count++;
        return count;
    };

    auto push_strings = [&](const char* const* src,
                            u32 requested_count,
                            u64* out_ptrs) -> u32 {
        u32 count = count_strings(src, requested_count);
        if (count > MAX_STACK_STRINGS) count = MAX_STACK_STRINGS;
        for (i32 i = (i32)count - 1; i >= 0; i--) {
            const char* str = src[i] ? src[i] : "";
            usize len = 0;
            while (str[len]) len++;
            len++;
            u8* sp8 = reinterpret_cast<u8*>(sp) - len;
            if (sp8 < stack_buf) return 0;
            for (usize j = 0; j < len; j++) sp8[j] = str[j];
            sp = reinterpret_cast<u64*>(reinterpret_cast<u64>(sp8) & ~0x7ULL);
            out_ptrs[i] = to_user_va(sp8);
        }
        return count;
    };

    u64 argv_ptrs[MAX_STACK_STRINGS] = {0};
    u64 envp_ptrs[MAX_STACK_STRINGS] = {0};
    u32 real_argc = push_strings(argv, argc, argv_ptrs);
    u32 real_envc = push_strings(envp, envc, envp_ptrs);

    sp = reinterpret_cast<u64*>(reinterpret_cast<u64>(sp) & ~0xFULL);

    *(--sp) = 0;            *(--sp) = AT_NULL;
    *(--sp) = 0x1000;       *(--sp) = AT_PAGESZ;
    *(--sp) = entry_point;  *(--sp) = AT_ENTRY;
    *(--sp) = 0;            *(--sp) = AT_EGID;
    *(--sp) = 0;            *(--sp) = AT_GID;
    *(--sp) = 0;            *(--sp) = AT_EUID;
    *(--sp) = 0;            *(--sp) = AT_UID;
    *(--sp) = 0;
    for (i32 i = (i32)real_envc - 1; i >= 0; i--) *(--sp) = envp_ptrs[i];
    *(--sp) = 0;
    for (i32 i = (i32)real_argc - 1; i >= 0; i--) *(--sp) = argv_ptrs[i];
    *(--sp) = real_argc;

    u64 user_sp = to_user_va(sp);
    u64 used    = (u64)buf_top - (u64)sp;

    vmm::activate(space);
    u8* user_stack_dst = reinterpret_cast<u8*>(vmm::USER_STACK_TOP - used);
    u8* stack_src      = reinterpret_cast<u8*>(sp);
    for (u64 i = 0; i < used; i++) user_stack_dst[i] = stack_src[i];
    vmm::activate(vmm::kernel_space);

    heap::kfree(stack_buf);

    Task* t = reinterpret_cast<Task*>(heap::kmalloc(sizeof(Task)));
    if (!t) return kResult<u32>::err(kError::OutOfMemory);

    u8* kstack  = reinterpret_cast<u8*>(heap::kmalloc(DEFAULT_STACK));
    if (!kstack) {
        heap::kfree(t);
        return kResult<u32>::err(kError::OutOfMemory);
    }

    u8* scstack = reinterpret_cast<u8*>(heap::kmalloc(DEFAULT_STACK));
    if (!scstack) {
        heap::kfree(kstack);
        heap::kfree(t);
        return kResult<u32>::err(kError::OutOfMemory);
    }

    fdtable::FDTable* fdt = fdtable::alloc();
    if (!fdt) {
        heap::kfree(scstack);
        heap::kfree(kstack);
        heap::kfree(t);
        return kResult<u32>::err(kError::OutOfMemory);
    }

    t->stack         = kstack;
    t->stack_size    = DEFAULT_STACK;
    t->syscall_stack = scstack;
    t->fd_table      = fdt;
    t->state         = State::Ready;
    t->tid           = next_tid++;
    t->quantum       = quantum;
    t->timeslice     = quantum;
    t->next          = nullptr;
    t->address_space = space;

    auto cwd_res = vfs::resolve("/");
    t->cwd = cwd_res.is_ok() ? cwd_res.value() : nullptr;

    init_stdio(t->fd_table);

    usize i = 0;
    while (name[i] && i < 31) { t->name[i] = name[i]; i++; }
    t->name[i] = 0;

    u64* ksp = reinterpret_cast<u64*>(kstack + DEFAULT_STACK);
    *(--ksp) = (u64)(gdt::USER_DATA | 3); // ss
    *(--ksp) = user_sp;                   // rsp
    *(--ksp) = 0x202;                     // rflags
    *(--ksp) = (u64)(gdt::USER_CODE | 3); // cs
    *(--ksp) = entry_point;               // rip
    *(--ksp) = reinterpret_cast<u64>(user_task_trampoline);
    *(--ksp) = 0; // r15
    *(--ksp) = 0; // r14
    *(--ksp) = 0; // r13
    *(--ksp) = 0; // r12
    *(--ksp) = 0; // r11
    *(--ksp) = 0; // r10
    *(--ksp) = 0; // r9
    *(--ksp) = 0; // r8
    *(--ksp) = 0; // rbp
    *(--ksp) = 0; // rbx
    t->ctx = reinterpret_cast<Context*>(ksp);

    u64 irq_flags = irq_save_disable();
    enqueue(t);
    irq_restore(irq_flags);

    return kResult<u32>::ok(t->tid);
}

// ── spawn_elf ─────────────────────────────────────────────────────────────
kResult<u32> sched::spawn_elf(const elf::Image& image,
                               const char* name,
                               const char** argv,
                               u32 argc,
                               const char** envp,
                               u32 envc) {
    vmm::AddressSpace space = vmm::create_user_space();
    if (!space.pml4)
        return kResult<u32>::err(kError::OutOfMemory);

    // Map ELF segments
    for (u32 si = 0; si < image.segment_count; si++) {
        const elf::Segment& seg = image.segments[si];
        u64 vstart = seg.vaddr & ~0xFFFULL;
        u64 vend   = (seg.vaddr + seg.memsz + 0xFFFULL) & ~0xFFFULL;
        for (u64 va = vstart; va < vend; va += 0x1000) {
            u64 frame = pmm::alloc_frame();
            if (!frame) {
                vmm::destroy_user_space(space);
                return kResult<u32>::err(kError::OutOfMemory);
            }
            if (!vmm::map_page(space, va, frame,
                               vmm::FLAG_PRESENT | vmm::FLAG_WRITE | vmm::FLAG_USER)) {
                vmm::destroy_user_space(space);
                return kResult<u32>::err(kError::OutOfMemory);
            }
        }
    }

    vmm::activate(space);
    for (u32 si = 0; si < image.segment_count; si++) {
        const elf::Segment& seg = image.segments[si];
        u8* dst = reinterpret_cast<u8*>(seg.vaddr);
        for (u64 j = 0; j < seg.memsz; j++) dst[j] = 0;
        for (u64 j = 0; j < seg.filesz; j++) dst[j] = seg.data[j];
    }
    vmm::activate(vmm::kernel_space);

    auto tid_res = sched::spawn_user(space, name, image.entry, DEFAULT_QUANTUM,
                                     argv, argc, envp, envc);
    if (tid_res.is_err()) {
        vmm::destroy_user_space(space);
        return tid_res;
    }

    return tid_res;
}
