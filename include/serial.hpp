#pragma once
// include/serial.hpp - Knail COM1 serial driver

#include <stdint.h>

namespace serial {

static constexpr uint16_t COM1 = 0x3F8;

void    init(uint16_t port = COM1);
void    write_char(char c);
void    write(const char* str);
void    write_line(const char* str);
void    write_hex(uint64_t val);
void    write_dec(uint64_t val);
void write_int(int64_t val);
bool    received();
char    read_char();

} // namespace serial
