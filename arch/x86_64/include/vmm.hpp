#pragma once
// include/vmm.hpp - Knail Virtual Memory Manager
// Manages x86-64 4-level page tables (PML4 -> PDPT -> PD -> PT)
// Each level has 512 entries, each entry is 8 bytes.
// Virtual address layout:
//   [63:48] sign extension
//   [47:39] PML4 index
//   [38:30] PDPT index
//   [29:21] PD index
//   [20:12] PT index
//   [11:0]  page offset

#include <stdint.h>
#include <stddef.h>

namespace vmm {

// ── Page flags ───────────────────────────────────────────────────────────
static constexpr uint64_t FLAG_PRESENT  = 1ULL << 0;
static constexpr uint64_t FLAG_WRITE    = 1ULL << 1;
static constexpr uint64_t FLAG_USER     = 1ULL << 2;
static constexpr uint64_t FLAG_HUGE     = 1ULL << 7;  // 2MiB page in PD
static constexpr uint64_t FLAG_NX       = 1ULL << 63; // no-execute

// Convenient combos
static constexpr uint64_t KERNEL_RW  = FLAG_PRESENT | FLAG_WRITE;
static constexpr uint64_t KERNEL_RO  = FLAG_PRESENT;
static constexpr uint64_t KERNEL_RWX = FLAG_PRESENT | FLAG_WRITE;
static constexpr uint64_t USER_RW    = FLAG_PRESENT | FLAG_WRITE | FLAG_USER;

// ── Page table entry ─────────────────────────────────────────────────────
using PTE = uint64_t;

// ── Page table (512 entries) ─────────────────────────────────────────────
struct alignas(4096) PageTable {
    PTE entries[512];
};

// ── Address space ────────────────────────────────────────────────────────
// Wraps a PML4 — one per process (or the kernel)
struct AddressSpace {
    PageTable* pml4;        // physical address of PML4
};

// ── Higher-half kernel map ────────────────────────────────────────────────
// We map the kernel at KERNEL_VIRT_BASE (0xFFFFFFFF80000000)
// and identity-map the first 4 GiB for early boot convenience.
static constexpr uint64_t KERNEL_VIRT_BASE = 0xFFFFFFFF80000000ULL;
static constexpr uint64_t PHYS_MAP_BASE    = 0xFFFF800000000000ULL; // direct phys map

// ── API ───────────────────────────────────────────────────────────────────

// Initialise VMM, set up and activate kernel address space
void init();

// Map a single 4 KiB page: virt -> phys with given flags
// Returns false if a frame couldn't be allocated for a page table
bool map_page(AddressSpace& space, uint64_t virt, uint64_t phys, uint64_t flags);

// Unmap a single page (does not free the physical frame it pointed to)
void unmap_page(AddressSpace& space, uint64_t virt);

// Translate virtual -> physical. Returns 0 if not mapped.
uint64_t virt_to_phys(AddressSpace& space, uint64_t virt);

// The kernel's address space
extern AddressSpace kernel_space;


// ── User address space constants ──────────────────────────────────────────
static constexpr uint64_t USER_CODE_BASE  = 0x0000000000400000ULL; // PML4[1] — separate from identity map
static constexpr uint64_t USER_STACK_TOP  = 0x0000007FFFFFF000ULL; // near top of lower half
static constexpr uint64_t USER_STACK_SIZE = 0x40000;                // 256 KiB

// Create a new user address space with kernel upper-half copied in
AddressSpace create_user_space();

// Deep-copy lower-half user mappings into a new address space.
AddressSpace clone_user_space(AddressSpace& src);

// Free all user-space page tables (not the physical frames of user data)
void destroy_user_space(AddressSpace& space);

// Activate an address space (load its PML4 into CR3)
void activate(AddressSpace& space);

} // namespace vmm

