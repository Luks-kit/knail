// kernel/pmm.cpp - Knail Physical Memory Manager

#include "pmm.hpp"
#include "serial.hpp"
namespace pmm {

static constexpr uint64_t MAX_FRAMES   = 1024 * 1024;
static constexpr uint64_t BITMAP_BYTES = MAX_FRAMES / 8; // 128 KiB

// Bitmap lives in .bss so it's covered by kernel_end and the PMM
// reservation loop — it will never hand out a frame that overlaps it.
static uint8_t  bitmap_storage[BITMAP_BYTES];
static uint8_t* bitmap       = bitmap_storage;
static uint64_t total_frames = 0;
static uint64_t used_frames  = 0;

static inline void set_used(uint64_t f) { bitmap[f/8] |=  (uint8_t)(1 << (f%8)); }
static inline void set_free(uint64_t f) { bitmap[f/8] &= ~(uint8_t)(1 << (f%8)); }
static inline bool is_used (uint64_t f) { return (bitmap[f/8] >> (f%8)) & 1; }

void init(uint32_t mb2_info_phys, uint64_t kernel_end) {
    kernel_end = (kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    // Start with everything marked used; free only what the memory map says is usable.
    for (uint64_t i = 0; i < BITMAP_BYTES; i++)
        bitmap[i] = 0xFF;

    uint64_t mb2_addr       = (uint64_t)(uint32_t)mb2_info_phys;
    uint32_t mb2_total_size = *reinterpret_cast<uint32_t*>(mb2_addr);

    if (mb2_total_size == 0 || mb2_total_size > 65536)
        return;

    // Walk multiboot2 tags
    uint8_t* tptr     = reinterpret_cast<uint8_t*>(mb2_addr + 8);
    uint8_t* tend     = reinterpret_cast<uint8_t*>(mb2_addr + mb2_total_size);
    uint64_t top_addr = 0;

    while (tptr < tend) {
        uint32_t tag_type = *reinterpret_cast<uint32_t*>(tptr);
        uint32_t tag_size = *reinterpret_cast<uint32_t*>(tptr + 4);

        if (tag_type == 0) break; // end tag
        if (tag_size == 0) break; // safety

        if (tag_type == MB2_TAG_MMAP) {
            uint32_t entry_size = *reinterpret_cast<uint32_t*>(tptr + 8);
            uint8_t* ep = tptr + 16;
            uint8_t* ee = tptr + tag_size;

            while (ep + sizeof(MB2MmapEntry) <= ee) {
                auto* e = reinterpret_cast<MB2MmapEntry*>(ep);
                uint64_t end = e->base + e->length;
                if (end > top_addr) top_addr = end;

                if (e->type == MB2_MMAP_USABLE) {
                    uint64_t fs = (e->base + PAGE_SIZE - 1) / PAGE_SIZE;
                    uint64_t fe = (e->base + e->length) / PAGE_SIZE;
                    for (uint64_t f = fs; f < fe && f < MAX_FRAMES; f++)
                        set_free(f);
                }
                ep += entry_size;
            }
        }

        tptr += (tag_size + 7) & ~7u;
    }

    total_frames = top_addr / PAGE_SIZE;
    if (total_frames > MAX_FRAMES) total_frames = MAX_FRAMES;

    static constexpr uint64_t IDENTITY_MAP_LIMIT_FRAMES = (1ULL << 30) / PAGE_SIZE; // 1 GiB / 4 KiB
    if (total_frames > IDENTITY_MAP_LIMIT_FRAMES)
        total_frames = IDENTITY_MAP_LIMIT_FRAMES;

    // Reserve frames 0 through kernel_end. kernel_end covers the kernel
    // binary + all .bss statics including bitmap_storage, TSS, IDT, stacks.
    uint64_t reserve_end = (kernel_end + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t f = 0; f < reserve_end; f++) set_used(f);

    // Tally used frames
    used_frames = 0;
    for (uint64_t f = 0; f < total_frames; f++)
        if (is_used(f)) used_frames++;
}

uint64_t alloc_frame() {
    for (uint64_t f = 1; f < total_frames; f++) {
        if (!is_used(f)) {
            set_used(f);
            used_frames++;
            return f * PAGE_SIZE;
        }
    }
    return 0;
}

void free_frame(uint64_t phys) {
    uint64_t f = phys / PAGE_SIZE;
    if (!f || f >= total_frames || !is_used(f)) return;
    set_free(f);
    used_frames--;
}

Stats stats() {
    uint64_t free_f = total_frames - used_frames;
    return Stats {
        .total_bytes  = total_frames * PAGE_SIZE,
        .usable_bytes = free_f * PAGE_SIZE,
        .used_frames  = used_frames,
        .free_frames  = free_f,
        .total_frames = total_frames,
    };
}

} // namespace pmm
