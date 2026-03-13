#pragma once
// include/pic.hpp - Knail 8259 PIC driver
// Remaps IRQ0-15 to vectors 32-47 (above the 32 CPU exceptions)

#include <stdint.h>

namespace pic {

// ── Vector offsets after remapping ───────────────────────────────────────
static constexpr uint8_t IRQ_BASE_MASTER = 32;  // IRQ0–7  → vectors 32–39
static constexpr uint8_t IRQ_BASE_SLAVE  = 40;  // IRQ8–15 → vectors 40–47

// ── IRQ numbers ───────────────────────────────────────────────────────────
static constexpr uint8_t IRQ_TIMER    = 0;
static constexpr uint8_t IRQ_KEYBOARD = 1;
static constexpr uint8_t IRQ_SLAVE    = 2;  // cascade
static constexpr uint8_t IRQ_RTC      = 8;

// ── API ───────────────────────────────────────────────────────────────────

// Remap PIC and mask all IRQs
void init();

// Unmask a specific IRQ (0–15) so it can fire
void enable_irq(uint8_t irq);

// Mask a specific IRQ
void disable_irq(uint8_t irq);

// Send End-Of-Interrupt — MUST be called at end of every IRQ handler
void eoi(uint8_t irq);

// Port I/O helpers (used by other drivers too)
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void io_wait() {
    outb(0x80, 0); // write to unused port — gives PIC time to process
}

static inline uint8_t read_cmos(uint8_t reg) {
    outb(0x70, reg);
    return inb(0x71);
}

} // namespace pic
