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

// ── Hardware cursor ────────────────────────────────────────────────────────
static void update_cursor() {
    uint16_t pos = (uint16_t)(row * WIDTH + col);
    pic::outb(0x3D4, 0x0F); pic::outb(0x3D5, (uint8_t)(pos & 0xFF));
    pic::outb(0x3D4, 0x0E); pic::outb(0x3D5, (uint8_t)(pos >> 8));
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
    if (c == '\n') {
        col = 0;
        if (++row >= HEIGHT) scroll();
        update_cursor();
        return;
    }
    if (c == '\r') {
        col = 0;
        update_cursor();
        return;
    }
    if (c == '\b') {
        // Move back one cell and erase it
        if (col > 0) {
            col--;
        } else if (row > 0) {
            row--;
            col = WIDTH - 1;
        }
        put_entry(' ', color, col, row);
        update_cursor();
        return;
    }

    put_entry(c, color, col, row);
    if (++col >= WIDTH) {
        col = 0;
        if (++row >= HEIGHT) scroll();
    }
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
