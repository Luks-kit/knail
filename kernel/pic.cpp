// kernel/pic.cpp - Knail 8259 PIC driver

#include "pic.hpp"

namespace pic {

// ── PIC I/O ports ─────────────────────────────────────────────────────────
static constexpr uint16_t PIC1_CMD  = 0x20;
static constexpr uint16_t PIC1_DATA = 0x21;
static constexpr uint16_t PIC2_CMD  = 0xA0;
static constexpr uint16_t PIC2_DATA = 0xA1;

// ── Initialization Command Words ──────────────────────────────────────────
static constexpr uint8_t ICW1_INIT = 0x10; // begin init sequence
static constexpr uint8_t ICW1_ICW4 = 0x01; // ICW4 will follow
static constexpr uint8_t ICW4_8086 = 0x01; // 8086 mode

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    // Save current masks
    uint8_t mask1 = inb(PIC1_DATA);
    uint8_t mask2 = inb(PIC2_DATA);
    (void)mask1; (void)mask2;

    // Start init sequence (cascade mode)
    outb(PIC1_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD,  ICW1_INIT | ICW1_ICW4); io_wait();

    // ICW2: vector offsets
    outb(PIC1_DATA, IRQ_BASE_MASTER); io_wait(); // master: IRQ0 → vector 32
    outb(PIC2_DATA, IRQ_BASE_SLAVE);  io_wait(); // slave:  IRQ8 → vector 40

    // ICW3: cascade wiring
    outb(PIC1_DATA, 0x04); io_wait(); // master: slave on IRQ2 (bit 2)
    outb(PIC2_DATA, 0x02); io_wait(); // slave:  cascade identity = 2

    // ICW4: 8086 mode
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    // Mask ALL IRQs — drivers enable their own
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

// ── enable_irq ────────────────────────────────────────────────────────────
void enable_irq(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
        // Also unmask IRQ2 (cascade) on master
        uint8_t m = inb(PIC1_DATA);
        outb(PIC1_DATA, m & ~(1 << 2));
    }
    uint8_t mask = inb(port);
    outb(port, mask & ~(1 << irq));
}

// ── disable_irq ───────────────────────────────────────────────────────────
void disable_irq(uint8_t irq) {
    uint16_t port;
    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    uint8_t mask = inb(port);
    outb(port, mask | (1 << irq));
}

// ── eoi ───────────────────────────────────────────────────────────────────
void eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_CMD, 0x20); // EOI to slave
    outb(PIC1_CMD, 0x20);     // always EOI to master
}

} // namespace pic
