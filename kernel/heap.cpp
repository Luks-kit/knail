// kernel/heap.cpp
// Heap is protected by a spinlock — kmalloc/kfree are safe from any task - Knail kernel heap allocator
//
// Layout of each allocation:
//
//   [ BlockHeader | ---- user data ---- ]
//
// Free blocks are kept in a singly-linked free list sorted by address.
// Adjacent free blocks are coalesced on kfree.
// New pages are mapped from the VMM when the free list is exhausted.

#include "heap.hpp"
#include "vmm.hpp"
#include "pmm.hpp"
#include "vga.hpp"
#include "mutex.hpp"

static sync::Spinlock heap_lock;

namespace heap {

// ── Block header (sits immediately before user data) ──────────────────────
struct BlockHeader {
    size_t       size;      // size of user data (not including header)
    bool         free;
    BlockHeader* next;      // next block in address order (free or used)
    BlockHeader* prev;      // previous block
    uint32_t     magic;     // 0xBEEFCAFE = valid, detect corruption
};

static constexpr uint32_t HEAP_MAGIC  = 0xBEEFCAFE;
static constexpr size_t   HEADER_SIZE = sizeof(BlockHeader);
static constexpr size_t   MIN_SPLIT   = 32; // don't split if remainder < this

// ── State ─────────────────────────────────────────────────────────────────
static BlockHeader* head      = nullptr;  // first block (address order)
static uint64_t     heap_end  = HEAP_START; // next unmapped page
static size_t       alloc_count = 0;
static size_t       free_count  = 0;

// ── Expand heap by mapping more pages ────────────────────────────────────
static BlockHeader* expand(size_t needed) {
    // Round up to page boundary
    size_t bytes = needed + HEADER_SIZE;
    size_t pages = (bytes + pmm::PAGE_SIZE - 1) / pmm::PAGE_SIZE;
    if (pages < 1) pages = 1;

    if (heap_end + pages * pmm::PAGE_SIZE > HEAP_MAX)
        return nullptr; // out of heap space

    for (size_t i = 0; i < pages; i++) {
        uint64_t phys = pmm::alloc_frame();
        if (!phys) return nullptr;
        vmm::map_page(vmm::kernel_space,
                      heap_end + i * pmm::PAGE_SIZE,
                      phys, vmm::KERNEL_RW);
    }

    auto* blk  = reinterpret_cast<BlockHeader*>(heap_end);
    blk->size  = pages * pmm::PAGE_SIZE - HEADER_SIZE;
    blk->free  = true;
    blk->next  = nullptr;
    blk->prev  = nullptr;
    blk->magic = HEAP_MAGIC;

    heap_end += pages * pmm::PAGE_SIZE;

    // Insert at end of block list
    if (!head) {
        head = blk;
    } else {
        BlockHeader* cur = head;
        while (cur->next) cur = cur->next;
        cur->next = blk;
        blk->prev = cur;
    }

    return blk;
}

// ── Coalesce blk with its successor if both free ─────────────────────────
static void coalesce(BlockHeader* blk) {
    if (!blk || !blk->free) return;
    BlockHeader* next = blk->next;
    if (next && next->free && next->magic == HEAP_MAGIC) {
        blk->size += HEADER_SIZE + next->size;
        blk->next  = next->next;
        if (next->next) next->next->prev = blk;
    }
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    heap_end    = HEAP_START;
    head        = nullptr;
    alloc_count = 0;
    free_count  = 0;
    // Pre-map one page so the heap is immediately usable
    expand(0);
}

// ── kmalloc ───────────────────────────────────────────────────────────────
void* kmalloc(size_t size) {
    sync::ScopeLock<sync::Spinlock> lock(heap_lock);
    if (size == 0) return nullptr;

    // Align to 16 bytes
    size = (size + 15) & ~(size_t)15;

    // First-fit search
    BlockHeader* blk = head;
    while (blk) {
        if (blk->magic != HEAP_MAGIC) return nullptr; // corruption
        if (blk->free && blk->size >= size) break;
        blk = blk->next;
    }

    // No fit — expand
    if (!blk) {
        blk = expand(size);
        if (!blk) return nullptr;
    }

    // Split if remainder is large enough
    if (blk->size >= size + HEADER_SIZE + MIN_SPLIT) {
        auto* split  = reinterpret_cast<BlockHeader*>(
                           reinterpret_cast<uint8_t*>(blk) + HEADER_SIZE + size);
        split->size  = blk->size - size - HEADER_SIZE;
        split->free  = true;
        split->magic = HEAP_MAGIC;
        split->next  = blk->next;
        split->prev  = blk;
        if (blk->next) blk->next->prev = split;
        blk->next    = split;
        blk->size    = size;
    }

    blk->free = false;
    alloc_count++;
    return reinterpret_cast<uint8_t*>(blk) + HEADER_SIZE;
}

// ── kcalloc ───────────────────────────────────────────────────────────────
void* kcalloc(size_t count, size_t size) {
    sync::ScopeLock<sync::Spinlock> lock(heap_lock);
    size_t total = count * size;
    void*  ptr   = kmalloc(total);
    if (ptr) {
        uint8_t* p = reinterpret_cast<uint8_t*>(ptr);
        for (size_t i = 0; i < total; i++) p[i] = 0;
    }
    return ptr;
}

// ── krealloc ──────────────────────────────────────────────────────────────
void* krealloc(void* ptr, size_t new_size) {
    // Lock once for the whole operation — don't call kmalloc/kfree (would deadlock)
    sync::ScopeLock<sync::Spinlock> lock(heap_lock);

    if (!ptr || !new_size) return nullptr;

    auto* blk = reinterpret_cast<BlockHeader*>(
                    reinterpret_cast<uint8_t*>(ptr) - HEADER_SIZE);
    if (blk->magic != HEAP_MAGIC) return nullptr;
    if (blk->size >= new_size) return ptr;

    // Inline alloc (duplicate of kmalloc body, lock already held)
    size_t aligned = (new_size + 15) & ~(size_t)15;
    BlockHeader* b = head;
    while (b) {
        if (b->free && b->size >= aligned) {
            b->free = false;
            // split if room
            if (b->size >= aligned + HEADER_SIZE + 32) {
                auto* nb = reinterpret_cast<BlockHeader*>(
                    reinterpret_cast<uint8_t*>(b) + HEADER_SIZE + aligned);
                nb->size  = b->size - aligned - HEADER_SIZE;
                nb->free  = true;
                nb->magic = HEAP_MAGIC;
                nb->next  = b->next;
                nb->prev  = b;
                if (b->next) b->next->prev = nb;
                b->next = nb;
                b->size = aligned;
            }
            void* newptr = reinterpret_cast<uint8_t*>(b) + HEADER_SIZE;
            // copy
            uint8_t* src = reinterpret_cast<uint8_t*>(ptr);
            uint8_t* dst = reinterpret_cast<uint8_t*>(newptr);
            size_t n = blk->size < aligned ? blk->size : aligned;
            for (size_t i = 0; i < n; i++) dst[i] = src[i];
            // free old inline
            blk->free = true;
            return newptr;
        }
        b = b->next;
    }
    return nullptr; // out of memory
}

// ── kfree ─────────────────────────────────────────────────────────────────
void kfree(void* ptr) {
    sync::ScopeLock<sync::Spinlock> lock(heap_lock);
    if (!ptr) return;

    auto* blk = reinterpret_cast<BlockHeader*>(
                    reinterpret_cast<uint8_t*>(ptr) - HEADER_SIZE);

    if (blk->magic != HEAP_MAGIC) return; // bad pointer / corruption
    if (blk->free) return;                // double free — ignore

    blk->free = true;
    free_count++;

    // Coalesce with next, then with prev
    coalesce(blk);
    if (blk->prev) coalesce(blk->prev);
}

// ── dump_stats ────────────────────────────────────────────────────────────
void dump_stats() {
    size_t free_bytes = 0, used_bytes = 0, blocks = 0;
    BlockHeader* cur = head;
    while (cur) {
        blocks++;
        if (cur->free) free_bytes += cur->size;
        else           used_bytes += cur->size;
        cur = cur->next;
    }
    vga::write("heap: blocks=");  vga::write_dec(blocks);
    vga::write(" used=");         vga::write_dec(used_bytes);
    vga::write("B free=");        vga::write_dec(free_bytes);
    vga::write("B allocs=");      vga::write_dec(alloc_count);
    vga::write(" frees=");        vga::write_dec(free_count);
    vga::write_line("");
}

} // namespace heap
