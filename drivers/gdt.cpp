// kernel/gdt.cpp - Knail GDT + TSS implementation

#include "gdt.hpp"

namespace gdt {

// ── Access byte flags ────────────────────────────────────────────────────────
static constexpr uint8_t PRESENT    = 1 << 7;
static constexpr uint8_t DPL0       = 0 << 5;  // ring 0
static constexpr uint8_t DPL3       = 3 << 5;  // ring 3
static constexpr uint8_t NONSYSTEM  = 1 << 4;  // code/data (not system)
static constexpr uint8_t EXEC       = 1 << 3;  // executable
static constexpr uint8_t DC         = 1 << 2;  // direction/conforming
static constexpr uint8_t RW         = 1 << 1;  // readable/writable
static constexpr uint8_t TSS_TYPE   = 0x09;    // 64-bit available TSS

// ── Granularity byte flags ────────────────────────────────────────────────────
static constexpr uint8_t GRAN_4K    = 1 << 7;  // page granularity
static constexpr uint8_t LONG_MODE  = 1 << 5;  // 64-bit code segment
static constexpr uint8_t SIZE_32    = 1 << 6;  // 32-bit (for data)

// ── Static storage ────────────────────────────────────────────────────────────
static Table   gdt;
static TSS     tss;
static Pointer gdtr;

// Dedicated stack for double fault / NMI IST entries
static uint8_t ist1_stack[4096];
static uint8_t ist2_stack[4096];

// ── Helpers ───────────────────────────────────────────────────────────────────
static Entry make_entry(uint32_t base, uint32_t limit,
                        uint8_t access, uint8_t gran) {
    return Entry {
        .limit_low   = (uint16_t)(limit & 0xFFFF),
        .base_low    = (uint16_t)(base  & 0xFFFF),
        .base_mid    = (uint8_t)((base  >> 16) & 0xFF),
        .access      = access,
        .granularity = (uint8_t)((gran & 0xF0) | ((limit >> 16) & 0x0F)),
        .base_high   = (uint8_t)((base  >> 24) & 0xFF),
    };
}

static void set_tss_descriptor(uint64_t base, uint32_t limit) {
    gdt.tss.low = make_entry(
        (uint32_t)(base & 0xFFFFFFFF),
        limit,
        PRESENT | TSS_TYPE,   // access
        0                     // byte granularity, no flags
    );
    gdt.tss.base_upper = (uint32_t)(base >> 32);
    gdt.tss.reserved   = 0;
}

// ── External ASM stubs (in gdt.asm) ──────────────────────────────────────────
extern "C" void gdt_flush(Pointer* gdtr);
extern "C" void tss_flush(uint16_t selector);

// ── init ─────────────────────────────────────────────────────────────────────
void init() {
    // Null descriptor
    gdt.null = make_entry(0, 0, 0, 0);

    // Kernel code: base=0, limit=max, 64-bit, ring 0, executable, readable
    gdt.kernel_code = make_entry(0, 0xFFFFF,
        PRESENT | DPL0 | NONSYSTEM | EXEC | RW,
        GRAN_4K | LONG_MODE);

    // Kernel data: base=0, limit=max, ring 0, writable
    gdt.kernel_data = make_entry(0, 0xFFFFF,
        PRESENT | DPL0 | NONSYSTEM | RW,
        GRAN_4K | SIZE_32);

    // User data: ring 3
    gdt.user_data = make_entry(0, 0xFFFFF,
        PRESENT | DPL3 | NONSYSTEM | RW,
        GRAN_4K | SIZE_32);

    // User code: ring 3, 64-bit
    gdt.user_code = make_entry(0, 0xFFFFF,
        PRESENT | DPL3 | NONSYSTEM | EXEC | RW,
        GRAN_4K | LONG_MODE);

    // TSS
    for (size_t i = 0; i < sizeof(TSS); i++)
        reinterpret_cast<uint8_t*>(&tss)[i] = 0;
    tss.iopb_offset = sizeof(TSS);  // no IOPB
    tss.ist[0] = reinterpret_cast<uint64_t>(ist1_stack + sizeof(ist1_stack)); // IST1 for #DF
    tss.ist[1] = reinterpret_cast<uint64_t>(ist2_stack + sizeof(ist2_stack)); // IST2 for #NMI
    set_tss_descriptor(reinterpret_cast<uint64_t>(&tss), sizeof(TSS) - 1);

    // Load GDT
    gdtr.limit = sizeof(Table) - 1;
    gdtr.base  = reinterpret_cast<uint64_t>(&gdt);
    gdt_flush(&gdtr);

    // Load TSS
    tss_flush(TSS_SEG);
}

// Debug: print IST addresses so we can verify they're sane
void debug_ist() {
    // Can't use serial here (called too early), but we can call after serial::init()
    // Just expose the values via a public function
}

uint64_t get_ist1_addr() {
    return reinterpret_cast<uint64_t>(ist1_stack + sizeof(ist1_stack));
}
uint64_t get_tss_ist0() { return tss.ist[0]; }

// ── set_kernel_stack ──────────────────────────────────────────────────────────
void set_kernel_stack(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}

} // namespace gdt

// Exported for debug: return the IST1 value from the TSS
extern "C" uint64_t gdt_get_ist1() {
    return gdt::tss.ist[0];
}
