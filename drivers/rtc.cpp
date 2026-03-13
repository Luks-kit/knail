#pragma once
// include/rtc.hpp - Knail RTC (CMOS Real-Time Clock) driver
#include "pic.hpp"
#include "types.hpp"

namespace rtc {

// Read a raw CMOS register value.
u8 read_cmos(u8 reg);

// Decode a BCD-encoded byte to binary.
static inline u8 bcd_decode(u8 val) {
    return (val & 0x0F) + ((val >> 4) * 10);
}

// Read the current date/time from the RTC.
// Waits for any in-progress update to complete before reading.
// Handles BCD vs binary mode automatically.
kResult<kDateTime> read_datetime();

} // namespace rtc
