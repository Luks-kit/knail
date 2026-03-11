#pragma once
// include/vga.hpp

#include <stdint.h>
#include <stddef.h>

namespace vga {

static constexpr size_t WIDTH  = 80;
static constexpr size_t HEIGHT = 25;

enum class Color : uint8_t {
    Black        = 0,  Blue       = 1,  Green     = 2,  Cyan      = 3,
    Red          = 4,  Magenta    = 5,  Brown     = 6,  LightGray = 7,
    DarkGray     = 8,  LightBlue  = 9,  LightGreen= 10, LightCyan = 11,
    LightRed     = 12, Pink       = 13, Yellow    = 14, White     = 15,
};

constexpr uint8_t make_color(Color fg, Color bg) {
    return (uint8_t)fg | ((uint8_t)bg << 4);
}

// Call this first before anything else
void init();

void set_color(Color fg, Color bg);
void put_char(char c);
void write(const char* str);
void write_line(const char* str);
void write_hex(uint64_t val);
void write_dec(uint64_t val);

} // namespace vga
