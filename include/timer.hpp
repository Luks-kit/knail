#pragma once
// include/timer.hpp - Knail PIT (8253) timer driver
// Fires IRQ0 at a configurable frequency

#include <stdint.h>

namespace timer {

static constexpr uint32_t PIT_BASE_HZ = 1193182; // PIT input frequency

// Initialise timer to fire at `hz` interrupts per second
void init(uint32_t hz = 100);

// Returns total ticks since boot
uint64_t ticks();

// Returns milliseconds since boot
uint64_t ms();

// IRQ0 handler — called from IDT
extern "C" void timer_handler();

} // namespace timer
