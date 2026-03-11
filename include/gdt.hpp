#pragma once
// include/gdt.hpp - Knail GDT + TSS

#include <stdint.h>
#include <stddef.h>
namespace gdt {

// ── Segment selectors ────────────────────────────────────────────────────────
static constexpr uint16_t KERNEL_CODE = 0x08;
static constexpr uint16_t KERNEL_DATA = 0x10;
static constexpr uint16_t USER_DATA   = 0x18;  // user mode (future)
static constexpr uint16_t USER_CODE   = 0x20;  // user mode (future)
static constexpr uint16_t TSS_SEG     = 0x28;  // TSS occupies two slots (16 bytes)

// ── TSS (64-bit) ─────────────────────────────────────────────────────────────
struct [[gnu::packed]] TSS {
    uint32_t reserved0;
    uint64_t rsp0;          // kernel stack pointer (ring 0)
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];        // interrupt stack table
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
};

// ── GDT entry (8 bytes) ───────────────────────────────────────────────────────
struct [[gnu::packed]] Entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;   // flags[7:4] + limit_high[3:0]
    uint8_t  base_high;
};

// ── System entry (16 bytes — used for TSS descriptor) ────────────────────────
struct [[gnu::packed]] SystemEntry {
    Entry    low;
    uint32_t base_upper;    // bits 63:32 of base
    uint32_t reserved;
};

// ── Full GDT ─────────────────────────────────────────────────────────────────
struct [[gnu::packed]] Table {
    Entry       null;           // 0x00
    Entry       kernel_code;    // 0x08
    Entry       kernel_data;    // 0x10
    Entry       user_data;      // 0x18
    Entry       user_code;      // 0x20
    SystemEntry tss;            // 0x28 (16 bytes)
};

// ── GDTR ────────────────────────────────────────────────────────────────────
struct [[gnu::packed]] Pointer {
    uint16_t limit;
    uint64_t base;
};

void init();
void set_kernel_stack(uint64_t rsp0);

} // namespace gdt
