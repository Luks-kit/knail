#pragma once
// include/keyboard.hpp - Knail PS/2 keyboard driver

#include <stdint.h>
#include <stddef.h>

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

KeyEvent read_event();

// IRQ1 handler
extern "C" void keyboard_handler();

} // namespace keyboard
