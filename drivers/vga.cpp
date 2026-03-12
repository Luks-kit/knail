// drivers/vga.cpp - Knail VGA text mode driver

#include "vga.hpp"
#include "pic.hpp"
#include "mutex.hpp"

static sync::Spinlock vga_lock;

namespace vga {

static uint16_t* const BUFFER = reinterpret_cast<uint16_t*>(0xB8000);

static size_t  col   = 0;
static size_t  row   = 0;
static uint8_t color = make_color(Color::LightGray, Color::Black);
static size_t saved_row = 0;
static size_t saved_col = 0;

// ANSI escape parser state
static enum class EscState : uint8_t {
    Normal,
    Esc,      // saw 0x1B
    Bracket,  // saw 0x1B [
    Params,   // accumulating parameter digits/semicolons
} esc_state = EscState::Normal;

static uint8_t esc_params[8] = {};
static uint8_t esc_param_count = 0;

static Color ansi_to_vga_fg(uint8_t code) {
    switch (code) {
        case 30: return Color::Black;
        case 31: return Color::Red;
        case 32: return Color::Green;
        case 33: return Color::Brown;
        case 34: return Color::Blue;
        case 35: return Color::Magenta;
        case 36: return Color::Cyan;
        case 37: return Color::LightGray;
        case 90: return Color::DarkGray;
        case 91: return Color::LightRed;
        case 92: return Color::LightGreen;
        case 93: return Color::Yellow;
        case 94: return Color::LightBlue;
        case 95: return Color::Pink;
        case 96: return Color::LightCyan;
        case 97: return Color::White;
        default: return Color::LightGray;
    }
}
static Color ansi_to_vga_bg(uint8_t code) {
    // BG codes are FG+10
    return ansi_to_vga_fg(code - 10);
}

static void apply_sgr() {
    if (esc_param_count == 0) {
        // \033[m — reset
        color = make_color(Color::LightGray, Color::Black);
        return;
    }
    Color fg = (Color)(color & 0x0F);
    Color bg = (Color)((color >> 4) & 0x0F);
    bool bright = false;

    for (uint8_t i = 0; i < esc_param_count; i++) {
        uint8_t p = esc_params[i];
        if (p == 0)                          { fg = Color::LightGray; bg = Color::Black; bright = false; }
        else if (p == 1)                     { bright = true; }
        else if (p >= 30 && p <= 37)         { fg = ansi_to_vga_fg(p); }
        else if (p >= 90 && p <= 97)         { fg = ansi_to_vga_fg(p); }
        else if (p >= 40 && p <= 47)         { bg = ansi_to_vga_bg(p); }
        else if (p >= 100 && p <= 107)       { bg = ansi_to_vga_fg(p - 60); }
    }
    // Bold/bright shifts fg to bright variant if it isn't already
    if (bright && (uint8_t)fg < 8) fg = (Color)((uint8_t)fg + 8);
    color = make_color(fg, bg);
}

// ── Hardware cursor ────────────────────────────────────────────────────────
static void update_cursor() {
    uint16_t pos = (uint16_t)(row * WIDTH + col);
    pic::outb(0x3D4, 0x0F); pic::outb(0x3D5, (uint8_t)(pos & 0xFF));
    pic::outb(0x3D4, 0x0E); pic::outb(0x3D5, (uint8_t)(pos >> 8));
}

void clear() {
    uint16_t blank = (color << 8) | ' ';

    for (size_t i = 0; i < WIDTH * HEIGHT; i++)
        BUFFER[i] = blank;

    row = 0;
    col = 0;
    update_cursor();
}

static void put_entry(char c, uint8_t col_, size_t x, size_t y) {
    BUFFER[y * WIDTH + x] = (uint16_t)c | ((uint16_t)col_ << 8);
}

void init() {
    col   = 0;
    row   = 0;
    color = make_color(Color::LightGray, Color::Black);
    for (size_t y = 0; y < HEIGHT; y++)
        for (size_t x = 0; x < WIDTH; x++)
            put_entry(' ', color, x, y);

    // Enable cursor, set scanline 13–15 (thin underline at bottom of cell)
    pic::outb(0x3D4, 0x0A); pic::outb(0x3D5, (pic::inb(0x3D5) & 0xC0) | 13);
    pic::outb(0x3D4, 0x0B); pic::outb(0x3D5, (pic::inb(0x3D5) & 0xE0) | 15);

    update_cursor();
}

void set_color(Color fg, Color bg) {
    color = make_color(fg, bg);
}

static void scroll() {
    for (size_t y = 1; y < HEIGHT; y++)
        for (size_t x = 0; x < WIDTH; x++)
            BUFFER[(y-1)*WIDTH + x] = BUFFER[y*WIDTH + x];
    for (size_t x = 0; x < WIDTH; x++)
        put_entry(' ', color, x, HEIGHT - 1);
    row = HEIGHT - 1;
}

static void _put_char(char c) {
    switch (esc_state) {
    case EscState::Normal:
        if (c == '\033') { esc_state = EscState::Esc; return; }
        break; // fall through to normal rendering below

    case EscState::Esc:
        if (c == '[') { 
            esc_state = EscState::Bracket;
            esc_param_count = 0;
            for (auto& p : esc_params) p = 0;
        } else {
            esc_state = EscState::Normal; // unknown, bail
        }
        return;

    case EscState::Bracket:
        esc_state = EscState::Params;
        // fall through — first char after '[' is a param digit or final byte
        [[fallthrough]];

    case EscState::Params:
        if (c >= '0' && c <= '9') {
            if (esc_param_count < 8)
                esc_params[esc_param_count] = 
                    esc_params[esc_param_count] * 10 + (c - '0');
            return;
        }
        if (c == ';') {
            if (esc_param_count < 7) esc_param_count++;
            return;
        }
        if (esc_param_count < 8) esc_param_count++;
        if (c == 'm') apply_sgr();
        if (c == 'J') {
            if (esc_params[0] == 2) clear();
        }
        // Add these:
        if (c == 'A') {  // cursor up
            size_t n = esc_params[0] ? esc_params[0] : 1;
            row = (row >= n) ? row - n : 0;
            update_cursor();
        }
        if (c == 'B') {  // cursor down
            size_t n = esc_params[0] ? esc_params[0] : 1;
            row = (row + n < HEIGHT) ? row + n : HEIGHT - 1;
            update_cursor();
        }
        if (c == 'C') {  // cursor forward
            size_t n = esc_params[0] ? esc_params[0] : 1;
            col = (col + n < WIDTH) ? col + n : WIDTH - 1;
            update_cursor();
        }
        if (c == 'D') {  // cursor back
            size_t n = esc_params[0] ? esc_params[0] : 1;
            col = (col >= n) ? col - n : 0;
            update_cursor();
        }
        if (c == 'H' || c == 'f') {  // cursor position \x1b[row;colH (1-indexed)
            row = esc_params[0] ? esc_params[0] - 1 : 0;
            col = esc_params[1] ? esc_params[1] - 1 : 0;
            if (row >= HEIGHT) row = HEIGHT - 1;
            if (col >= WIDTH)  col = WIDTH  - 1;
        }
        if (c == 'K') {  // erase in line
            if (esc_params[0] == 0) {  // cursor to end
                for (size_t x = col; x < WIDTH; x++)
                    put_entry(' ', color, x, row);
            } else if (esc_params[0] == 1) {  // start to cursor
                for (size_t x = 0; x <= col; x++)
                    put_entry(' ', color, x, row);
            } else if (esc_params[0] == 2) {  // whole line
                for (size_t x = 0; x < WIDTH; x++)
                    put_entry(' ', color, x, row);
            }
        }
        if (c == 's') { saved_row = row; saved_col = col; update_cursor(); }
        if (c == 'u') { row = saved_row; col = saved_col; update_cursor(); }
        esc_state = EscState::Normal;  


        return;
    }

    // Normal character rendering (unchanged from before)
    if (c == '\n') {
        col = 0;
        if (++row >= HEIGHT) scroll();
        update_cursor();
        return;
    }
    if (c == '\r') { col = 0; update_cursor(); return; }
    if (c == '\b') {
        if (col > 0) col--;
        else if (row > 0) { row--; col = WIDTH - 1; }
        put_entry(' ', color, col, row);
        update_cursor();
        return;
    }
    put_entry(c, color, col, row);
    if (++col >= WIDTH) { col = 0; if (++row >= HEIGHT) scroll(); }
    update_cursor();
}


// ── Public API — all acquire spinlock for atomic multi-char writes ─────────
void put_char(char c) {
    sync::ScopeLock<sync::Spinlock> lock(vga_lock);
    _put_char(c);
}

void write(const char* str) {
    sync::ScopeLock<sync::Spinlock> lock(vga_lock);
    for (; *str; str++) _put_char(*str);
}

void write_line(const char* str) {
    sync::ScopeLock<sync::Spinlock> lock(vga_lock);
    for (; *str; str++) _put_char(*str);
    _put_char('\n');
}

void write_hex(uint64_t val) {
    const char* digits = "0123456789ABCDEF";
    char buf[19];
    buf[0] = '0'; buf[1] = 'x'; buf[18] = 0;
    for (int i = 17; i >= 2; i--) {
        buf[i] = digits[val & 0xF];
        val >>= 4;
    }
    write(buf);
}

void write_dec(uint64_t val) {
    if (val == 0) { put_char('0'); return; }
    char buf[21]; buf[20] = 0;
    int i = 20;
    while (val && i > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    write(buf + i);
}

} // namespace vga
