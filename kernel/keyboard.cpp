// drivers/keyboard.cpp - Knail PS/2 keyboard driver
// Handles IRQ1, translates scancodes to ASCII, maintains a key event buffer

#include "keyboard.hpp"
#include "pic.hpp"
#include "scheduler.hpp"
#include "idt.hpp"

namespace keyboard {

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

static void push_event(KeyEvent e) {
    size_t next = (buf_head + 1) % BUF_SIZE;
    if (next == buf_tail) return; // buffer full, drop
    buf[buf_head] = e;
    buf_head = next;
}

// ── IRQ1 handler ──────────────────────────────────────────────────────────
extern "C" void keyboard_handler() {
    uint8_t sc = pic::inb(PS2_DATA);
    bool released = sc & 0x80;
    uint8_t key   = sc & 0x7F;

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
        while (!has_event()) sched::yield();
        KeyEvent e = pop_event();
        if (e.pressed && (e.ascii || e.scancode == 0x0E)) // 0x0E = backspace scancode
            return e.ascii ? e.ascii : '\b';
    }
}

} // namespace keyboard
