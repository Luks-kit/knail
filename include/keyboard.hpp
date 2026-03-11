#pragma once
// include/keyboard.hpp - Knail PS/2 keyboard driver

#include <stdint.h>
#include <stddef.h>

namespace keyboard {

// ── Key event ─────────────────────────────────────────────────────────────
struct KeyEvent {
    char     ascii;     // printable char, or 0 for non-printable
    uint8_t  scancode;
    bool     pressed;   // true=keydown, false=keyup
    bool     shift;
    bool     ctrl;
    bool     alt;
};

// ── Circular key buffer ───────────────────────────────────────────────────
static constexpr size_t BUF_SIZE = 64;

void init();

// Returns true if a key event is available
bool has_event();

// Pop next key event (check has_event first)
KeyEvent pop_event();

// Block until a printable key is pressed, return its ascii char
char read_char();

// IRQ1 handler
extern "C" void keyboard_handler();

} // namespace keyboard
