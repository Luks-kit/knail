#pragma once
// include/idt.hpp - Knail IDT + exception handling

#include <stdint.h>

namespace idt {

struct [[gnu::packed]] Entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct [[gnu::packed]] Pointer {
    uint16_t limit;
    uint64_t base;
};

static constexpr uint8_t TRAP_GATE      = 0x8F;
static constexpr uint8_t INTERRUPT_GATE = 0x8E;

// Must match push order in isr_common (last pushed = lowest address = first field)
// Push order: rax, rbx, rcx, rdx, rsi, rdi, rbp, r8..r15
// So rsp points to r15 after all pushes — r15 is first field
struct [[gnu::packed]] Frame {
    // GP regs — in REVERSE push order (r15 pushed last = lowest addr)
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    // Our stubs push these
    uint64_t vector;
    uint64_t error_code;
    // CPU pushes these on interrupt
    uint64_t rip, cs, rflags, rsp, ss;
};

void init();
void set_handler(uint8_t vector, void* handler, uint8_t type_attr);
void set_irq_handler(uint8_t irq, void(*)());

} // namespace idt
