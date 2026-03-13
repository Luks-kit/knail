#pragma once
// include/pmm.hpp - Knail Physical Memory Manager
// Bitmap allocator: each bit = one 4 KiB frame
// 0 = free, 1 = used

#include <stdint.h>
#include <stddef.h>

namespace pmm {

static constexpr uint64_t PAGE_SIZE = 4096;

// ── Multiboot2 structures (just what we need) ─────────────────────────────
struct [[gnu::packed]] MB2Tag {
    uint32_t type;
    uint32_t size;
};

struct [[gnu::packed]] MB2Info {
    uint32_t total_size;
    uint32_t reserved;
    // followed by tags
};

// Multiboot2 memory map tag (type 6)
struct [[gnu::packed]] MB2MmapTag {
    uint32_t type;          // 6
    uint32_t size;
    uint32_t entry_size;
    uint32_t entry_version;
    // followed by entries
};

struct [[gnu::packed]] MB2MmapEntry {
    uint64_t base;
    uint64_t length;
    uint32_t type;          // 1 = usable RAM
    uint32_t reserved;
};

static constexpr uint32_t MB2_TAG_MMAP = 6;
static constexpr uint32_t MB2_MMAP_USABLE = 1;

// ── Stats ─────────────────────────────────────────────────────────────────
struct Stats {
    uint64_t total_bytes;
    uint64_t usable_bytes;
    uint64_t used_frames;
    uint64_t free_frames;
    uint64_t total_frames;
};

// ── API ───────────────────────────────────────────────────────────────────

// Parse multiboot2 info and set up the bitmap.
// kernel_end = first physical address past the kernel image.
void init(uint32_t mb2_info_phys, uint64_t kernel_end);

// Allocate one physical frame. Returns physical address, or 0 on failure.
uint64_t alloc_frame();

// Free a previously allocated frame.
void free_frame(uint64_t phys);

// Query stats
Stats stats();

} // namespace pmm
