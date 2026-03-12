// drivers/keyboard.cpp - Knail PS/2 keyboard driver
// Handles IRQ1, translates scancodes to ASCII, maintains a key event buffer

#include "keyboard.hpp"
#include "pic.hpp"
#include "scheduler.hpp"
#include "serial.hpp"
#include "idt.hpp"
#include <stdint.h>

namespace keyboard {

static constexpr uint8_t KEY_UP    = 0xC8; // 0x80 + 0x48
static constexpr uint8_t KEY_DOWN  = 0xD0;
static constexpr uint8_t KEY_LEFT  = 0xCB;
static constexpr uint8_t KEY_RIGHT = 0xCD;
static constexpr uint8_t KEY_HOME  = 0xC7;
static constexpr uint8_t KEY_END   = 0xCF;
static constexpr uint8_t KEY_PGUP  = 0xC9;
static constexpr uint8_t KEY_PGDN  = 0xD1;
static constexpr uint8_t KEY_DEL   = 0xD3;
static constexpr uint8_t KEY_INS   = 0xD2;

// ── PS/2 ports ────────────────────────────────────────────────────────────
static constexpr uint16_t PS2_DATA   = 0x60;
static constexpr uint16_t PS2_STATUS = 0x64;

// ── US QWERTY scancode set 1 → ASCII (lowercase) ─────────────────────────
static const char scancode_map[128] = {
    0,    0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0,    '\\','z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*',  0,   ' ',
    // rest 0
};

// ── Shift map ─────────────────────────────────────────────────────────────
static const char scancode_shift[128] = {
    0,    0,   '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,    '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*',  0,   ' ',
};

// ── Special scancodes ─────────────────────────────────────────────────────
static constexpr uint8_t SC_LSHIFT = 0x2A;
static constexpr uint8_t SC_RSHIFT = 0x36;
static constexpr uint8_t SC_LCTRL  = 0x1D;
static constexpr uint8_t SC_LALT   = 0x38;
static constexpr uint8_t SC_CAPS   = 0x3A;

// ── State ─────────────────────────────────────────────────────────────────
static bool shift_held = false;
static bool ctrl_held  = false;
static bool alt_held   = false;
static bool caps_lock  = false;

// ── Circular event buffer ─────────────────────────────────────────────────
static KeyEvent buf[BUF_SIZE];
static volatile size_t buf_head = 0;
static volatile size_t buf_tail = 0;

static sched::Task* waiting_task = nullptr;

static void push_event(KeyEvent e) {
    size_t next = (buf_head + 1) % BUF_SIZE;
    if (next == buf_tail) return;
    buf[buf_head] = e;
    buf_head = next;
    // Wake any blocked reader
    if (waiting_task) {
        sched::Task* t = waiting_task;
        waiting_task = nullptr;
        sched::wake(t);
    }
}

// ── IRQ1 handler ──────────────────────────────────────────────────────────
static bool e0_prefix = false;

extern "C" void keyboard_handler() {
    uint8_t sc = pic::inb(PS2_DATA);

    if (sc == 0xE0) { e0_prefix = true; pic::eoi(pic::IRQ_KEYBOARD); return; }

    bool released = sc & 0x80;
    uint8_t key   = sc & 0x7F;
    
    if (e0_prefix) {
        e0_prefix = false;
        // Handle extended keys
        switch (key) {
            case 0x48: 
                push_event({0, (uint8_t)(0x80 + key), !released, shift_held, ctrl_held, alt_held}); 
                break; // up
            case 0x50: 
                push_event({0, (uint8_t)(0x81 + key), !released, shift_held, ctrl_held, alt_held}); 
                break; // down
            case 0x4B: 
                push_event({0, (uint8_t)(0x82 + key), !released, shift_held, ctrl_held, alt_held}); 
                break; // left
            case 0x4D: 
                push_event({0, (uint8_t)(0x83 + key), !released, shift_held, ctrl_held, alt_held}); 
                break; // right
            case 0x1D: ctrl_held = !released; break; // right ctrl
            case 0x38: alt_held  = !released; break; // right alt
        }
        goto eoi;
    }


    // Update modifier state
    if (key == SC_LSHIFT || key == SC_RSHIFT) { shift_held = !released; goto eoi; }
    if (key == SC_LCTRL)                       { ctrl_held  = !released; goto eoi; }
    if (key == SC_LALT)                        { alt_held   = !released; goto eoi; }
    if (key == SC_CAPS && !released)           { caps_lock  = !caps_lock; goto eoi; }

    {
        bool use_shift = shift_held ^ caps_lock;
        char ascii = (key < 128)
                   ? (use_shift ? scancode_shift[key] : scancode_map[key])
                   : 0;

        push_event(KeyEvent {
            .ascii    = ascii,
            .scancode = key,
            .pressed  = !released,
            .shift    = shift_held,
            .ctrl     = ctrl_held,
            .alt      = alt_held,
        });
    }

eoi:
    pic::eoi(pic::IRQ_KEYBOARD);
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    shift_held = false;
    ctrl_held  = false;
    alt_held   = false;
    caps_lock  = false;
    buf_head   = 0;
    buf_tail   = 0;

    idt::set_irq_handler(pic::IRQ_KEYBOARD,
                         reinterpret_cast<void(*)()>(keyboard_handler));
    pic::enable_irq(pic::IRQ_KEYBOARD);
}

// ── has_event ─────────────────────────────────────────────────────────────
bool has_event() {
    return buf_head != buf_tail;
}

// ── pop_event ─────────────────────────────────────────────────────────────
KeyEvent pop_event() {
    if (!has_event()) return KeyEvent{};
    KeyEvent e = buf[buf_tail];
    buf_tail = (buf_tail + 1) % BUF_SIZE;
    return e;
}

// ── read_char ─────────────────────────────────────────────────────────────
char read_char() {
    while (true) {
        // Disable IRQs while we check + set waiting_task atomically
        __asm__ volatile("cli");
        
        if (has_event()) {
            __asm__ volatile("sti");
            KeyEvent e = pop_event();
            if (e.pressed && e.ascii)
                return e.ascii;
            continue;
        }

        // No event — set ourselves as waiter before re-enabling IRQs
        waiting_task = sched::current();
        __asm__ volatile("sti");
        sched::block_current();
        waiting_task = nullptr;  //  clear it here after being woken
    }
}

} // namespace keyboard
