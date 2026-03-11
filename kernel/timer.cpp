// kernel/timer.cpp - Knail PIT timer driver

#include "timer.hpp"
#include "pic.hpp"
#include "idt.hpp"
#include "scheduler.hpp"

namespace timer {

static constexpr uint16_t PIT_CHANNEL0 = 0x40;
static constexpr uint16_t PIT_CMD      = 0x43;

static volatile uint64_t tick_count = 0;
static uint32_t          tick_hz    = 0;

extern "C" void timer_handler() {
    tick_count++;
    pic::eoi(pic::IRQ_TIMER);
    sched::tick(); // preempt if timeslice expired
}

void init(uint32_t hz) {
    tick_hz    = hz;
    tick_count = 0;

    uint32_t divisor = PIT_BASE_HZ / hz;
    if (divisor > 0xFFFF) divisor = 0xFFFF;
    if (divisor < 1)      divisor = 1;

    pic::outb(PIT_CMD,      0x36);
    pic::outb(PIT_CHANNEL0, (uint8_t)(divisor & 0xFF));
    pic::outb(PIT_CHANNEL0, (uint8_t)((divisor >> 8) & 0xFF));

    idt::set_irq_handler(pic::IRQ_TIMER,
                    reinterpret_cast<void(*)()>(timer_handler));

    pic::enable_irq(pic::IRQ_TIMER);
}

uint64_t ticks() { return tick_count; }
uint64_t ms()    { return tick_count * 1000 / tick_hz; }

} // namespace timer
