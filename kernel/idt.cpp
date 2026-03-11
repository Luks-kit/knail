// kernel/idt.cpp - Knail IDT + exception handler

#include "idt.hpp"
#include "vga.hpp"
#include "serial.hpp"

namespace idt {

// ── IDT storage (256 entries) ─────────────────────────────────────────────
static Entry   idt_entries[256];
static Pointer idt_ptr;

// ── Exception names ───────────────────────────────────────────────────────
static const char* exception_names[32] = {
    "#DE Divide Error",
    "#DB Debug",
    "Non-Maskable Interrupt",
    "#BP Breakpoint",
    "#OF Overflow",
    "#BR Bound Range Exceeded",
    "#UD Invalid Opcode",
    "#NM Device Not Available",
    "#DF Double Fault",
    "Coprocessor Segment Overrun",
    "#TS Invalid TSS",
    "#NP Segment Not Present",
    "#SS Stack-Segment Fault",
    "#GP General Protection Fault",
    "#PF Page Fault",
    "Reserved",
    "#MF x87 Floating-Point",
    "#AC Alignment Check",
    "#MC Machine Check",
    "#XM SIMD Floating-Point",
    "#VE Virtualisation",
    "#CP Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "#SX Security Exception",
    "Reserved",
};

// ── set_handler ───────────────────────────────────────────────────────────
void set_handler(uint8_t vector, void* handler, uint8_t type_attr) {
    uint64_t addr = reinterpret_cast<uint64_t>(handler);
    idt_entries[vector] = Entry {
        .offset_low  = (uint16_t)(addr & 0xFFFF),
        .selector    = 0x08,            // KERNEL_CODE
        .ist         = 0,
        .type_attr   = type_attr,
        .offset_mid  = (uint16_t)((addr >> 16) & 0xFFFF),
        .offset_high = (uint32_t)((addr >> 32) & 0xFFFFFFFF),
        .reserved    = 0,
    };
}

// ── External stubs from idt.asm ───────────────────────────────────────────
extern "C" void idt_flush(Pointer*);
extern "C" void* isr_stub_table[32];

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    // Zero all entries
    for (int i = 0; i < 256; i++) {
        idt_entries[i] = Entry{};
    }

    // Install stubs for all 32 CPU exceptions
    for (int i = 0; i < 32; i++) {
        set_handler((uint8_t)i,
                    isr_stub_table[i],
                    INTERRUPT_GATE);
    }

    idt_ptr.limit = sizeof(idt_entries) - 1;
    idt_ptr.base  = reinterpret_cast<uint64_t>(idt_entries);
    idt_flush(&idt_ptr);

    // Double fault (#DF, vector 8) must use IST1 so the CPU always has a
    // valid stack even when the fault was caused by a bad RSP.
    idt_entries[8].ist = 1;
}

} // namespace idt

// ── exception_handler (called from isr_common in idt.asm) ─────────────────
extern "C" void exception_handler(idt::Frame* f) {
    const char* name = (f->vector < 32)
                     ? idt::exception_names[f->vector]
                     : "Unknown";

    vga::set_color(vga::Color::White, vga::Color::Red);
    vga::write_line("                                                                                ");
    vga::write("  KNAIL PANIC - ");
    vga::write(name);
    vga::write_line("                                                          ");
    vga::write_line("                                                                                ");

    vga::set_color(vga::Color::LightRed, vga::Color::Black);
    vga::write("  vector=");    vga::write_dec(f->vector);
    vga::write("  err=");       vga::write_hex(f->error_code);
    vga::write("  rip=");       vga::write_hex(f->rip);
    vga::write("  cs=");        vga::write_hex(f->cs);
    vga::write_line("");

    vga::write("  rsp=");  vga::write_hex(f->rsp);
    vga::write("  rbp=");  vga::write_hex(f->rbp);
    vga::write("  rflags="); vga::write_hex(f->rflags);
    vga::write_line("");

    vga::write("  rax=");  vga::write_hex(f->rax);
    vga::write("  rbx=");  vga::write_hex(f->rbx);
    vga::write("  rcx=");  vga::write_hex(f->rcx);
    vga::write("  rdx=");  vga::write_hex(f->rdx);
    vga::write_line("");

    vga::write("  rsi=");  vga::write_hex(f->rsi);
    vga::write("  rdi=");  vga::write_hex(f->rdi);
    vga::write("  r8=");   vga::write_hex(f->r8);
    vga::write("  r9=");   vga::write_hex(f->r9);
    vga::write_line("");

    // Page fault extra info
    if (f->vector == 14) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        vga::write("  #PF cr2="); vga::write_hex(cr2);
        vga::write("  [");
        vga::write(f->error_code & 1 ? "protection " : "not-present ");
        vga::write(f->error_code & 2 ? "write "      : "read ");
        vga::write(f->error_code & 4 ? "user"        : "kernel");
        vga::write_line("]");
    }

    vga::set_color(vga::Color::Yellow, vga::Color::Black);
    vga::write_line("\n  System halted.");

    // Also dump to serial so we see it even if VGA is corrupt/overwritten
    serial::write_line("\n--- EXCEPTION ---");
    serial::write("vector="); serial::write_dec(f->vector);
    serial::write(" err=");   serial::write_hex(f->error_code);
    serial::write(" rip=");   serial::write_hex(f->rip);
    serial::write(" cs=");    serial::write_hex(f->cs);
    serial::write_line("");
    serial::write("rsp=");    serial::write_hex(f->rsp);
    serial::write(" rflags=");serial::write_hex(f->rflags);
    serial::write_line("");
    if (f->vector == 14) {
        uint64_t cr2b;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2b));
        serial::write("#PF cr2="); serial::write_hex(cr2b);
        serial::write(" err=");    serial::write_hex(f->error_code);
        serial::write_line("");
    }

    for (;;) __asm__ volatile("cli; hlt");
}

namespace idt {

// ── IRQ dispatch table ────────────────────────────────────────────────────
extern "C" void* irq_stub_table[16];

using IRQHandler = void(*)();
IRQHandler irq_handlers[16] = {};

void set_irq_handler(uint8_t irq, IRQHandler handler) {
    if (irq >= 16) return;
    irq_handlers[irq] = handler;
    set_handler(32 + irq, irq_stub_table[irq], INTERRUPT_GATE);
}

} // namespace idt

extern "C" void irq_dispatch(idt::Frame* f) {
    uint8_t irq = (uint8_t)(f->vector - 32);
    if (irq < 16 && idt::irq_handlers[irq])
        idt::irq_handlers[irq]();
}
