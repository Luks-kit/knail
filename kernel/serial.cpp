        // drivers/serial.cpp - Knail COM1 serial driver
// In QEMU: run with -serial stdio to see output in terminal

#include "serial.hpp"
#include "pic.hpp"  // for outb/inb

namespace serial {

// ── UART register offsets (relative to base port) ─────────────────────────
static constexpr uint16_t DATA          = 0; // data register (R/W)
static constexpr uint16_t INT_ENABLE    = 1; // interrupt enable
static constexpr uint16_t BAUD_LOW      = 0; // baud divisor LSB  (DLAB=1)
static constexpr uint16_t BAUD_HIGH     = 1; // baud divisor MSB  (DLAB=1)
static constexpr uint16_t FIFO_CTRL     = 2; // FIFO control
static constexpr uint16_t LINE_CTRL     = 3; // line control (sets DLAB)
static constexpr uint16_t MODEM_CTRL    = 4; // modem control
static constexpr uint16_t LINE_STATUS   = 5; // line status

static uint16_t base_port = COM1;

// ── init ──────────────────────────────────────────────────────────────────
void init(uint16_t port) {
    base_port = port;

    pic::outb(port + INT_ENABLE, 0x00);  // disable interrupts

    pic::outb(port + LINE_CTRL,  0x80);  // enable DLAB to set baud rate
    pic::outb(port + BAUD_LOW,   0x03);  // 38400 baud (divisor = 3)
    pic::outb(port + BAUD_HIGH,  0x00);

    pic::outb(port + LINE_CTRL,  0x03);  // 8 bits, no parity, 1 stop bit
    pic::outb(port + FIFO_CTRL,  0xC7);  // enable FIFO, clear, 14-byte threshold
    pic::outb(port + MODEM_CTRL, 0x0B);  // RTS/DSR + aux output 2
}

// ── transmit_ready ────────────────────────────────────────────────────────
static bool transmit_ready() {
    return pic::inb(base_port + LINE_STATUS) & 0x20;
}

// ── write_char ────────────────────────────────────────────────────────────
void write_char(char c) {
    while (!transmit_ready());
    pic::outb(base_port + DATA, (uint8_t)c);
}

// ── write ─────────────────────────────────────────────────────────────────
void write(const char* str) {
    for (; *str; str++) {
        if (*str == '\n') write_char('\r');
        write_char(*str);
    }
}

// ── write_line ────────────────────────────────────────────────────────────
void write_line(const char* str) {
    write(str);
    write_char('\r');
    write_char('\n');
}

// ── write_hex ─────────────────────────────────────────────────────────────
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

// ── write_dec ─────────────────────────────────────────────────────────────
void write_dec(uint64_t val) {
    if (!val) { write_char('0'); return; }
    char buf[21]; buf[20] = 0;
    int i = 20;
    while (val && i > 0) { buf[--i] = '0' + (val % 10); val /= 10; }
    write(buf + i);
}

// write_int
void write_int(int64_t val) {
    if (val < 0) {
        write_char('-');
        write_dec((uint64_t)(-val));
    } else {
        write_dec((uint64_t)val);
    }
}

// ── received ──────────────────────────────────────────────────────────────
bool received() {
    return pic::inb(base_port + LINE_STATUS) & 0x01;
}

// ── read_char ─────────────────────────────────────────────────────────────
char read_char() {
    while (!received());
    return (char)pic::inb(base_port + DATA);
}

} // namespace serial
