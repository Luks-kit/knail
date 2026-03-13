// kernel/vmm.cpp - Knail Virtual Memory Manager

#include "vmm.hpp"
#include "pmm.hpp"
#include "serial.hpp"
#include "vga.hpp"

namespace vmm {

AddressSpace kernel_space;

// ── Address decomposition ─────────────────────────────────────────────────
static inline uint64_t pml4_idx(uint64_t v) { return (v >> 39) & 0x1FF; }
static inline uint64_t pdpt_idx(uint64_t v) { return (v >> 30) & 0x1FF; }
static inline uint64_t pd_idx  (uint64_t v) { return (v >> 21) & 0x1FF; }
static inline uint64_t pt_idx  (uint64_t v) { return (v >> 12) & 0x1FF; }

static inline uint64_t entry_phys(PTE e) {
    return e & 0x000FFFFFFFFFF000ULL;
}

// ── Allocate and zero a page table ───────────────────────────────────────
static PageTable* alloc_table() {
    uint64_t phys = pmm::alloc_frame();
    if (!phys) return nullptr;
    auto* t = reinterpret_cast<PageTable*>(phys);
    for (int i = 0; i < 512; i++) t->entries[i] = 0;
    return t;
}

// ── Get or create a child table, splitting huge pages if needed ───────────
static PageTable* get_or_create(PTE* entry, uint64_t base_phys, uint64_t flags) {
    if (*entry & FLAG_PRESENT) {

         // If caller wants a user mapping, parent table entries in the walk
        // must also have USER set or ring-3 access will fault.
        if (flags & FLAG_USER)
            *entry |= FLAG_USER;
        // If this is a huge page entry we must split it into 4K pages
        if (*entry & FLAG_HUGE) {
            PageTable* pt = alloc_table();
            if (!pt) return nullptr;

            uint64_t huge_phys = entry_phys(*entry);
            uint64_t eflags    = *entry & 0xFFF & ~(uint64_t)FLAG_HUGE;

            // Fill PT with 512 x 4K mappings covering the same 2MiB
            for (int i = 0; i < 512; i++)
                pt->entries[i] = (huge_phys + (uint64_t)i * 0x1000) | eflags;

            uint64_t split_flags = FLAG_PRESENT | FLAG_WRITE;
            if (flags & FLAG_USER) split_flags |= FLAG_USER;
            *entry = reinterpret_cast<uint64_t>(pt) | split_flags;
        }
        return reinterpret_cast<PageTable*>(entry_phys(*entry));
    }

    PageTable* child = alloc_table();
    if (!child) return nullptr;
    (void)base_phys;
    *entry = reinterpret_cast<uint64_t>(child) | flags;
    return child;
}

// ── map_page ──────────────────────────────────────────────────────────────
bool map_page(AddressSpace& space, uint64_t virt, uint64_t phys, uint64_t flags) {
    PageTable* pml4 = space.pml4;
    uint64_t table_flags = FLAG_PRESENT | FLAG_WRITE;
    if (flags & FLAG_USER) table_flags |= FLAG_USER;

    PageTable* pdpt = get_or_create(&pml4->entries[pml4_idx(virt)], 0, table_flags);
    if (!pdpt) { serial::write_line("[VMM] map_page: pdpt alloc failed"); return false; }

    PageTable* pd = get_or_create(&pdpt->entries[pdpt_idx(virt)], 0, table_flags);
    if (!pd) { serial::write_line("[VMM] map_page: pd alloc failed"); return false; }

    PageTable* pt = get_or_create(&pd->entries[pd_idx(virt)], 0, table_flags);
    if (!pt) { serial::write_line("[VMM] map_page: pt alloc failed"); return false; }


    pt->entries[pt_idx(virt)] = (phys & 0x000FFFFFFFFFF000ULL) | flags;
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
    return true;
}

// ── unmap_page ────────────────────────────────────────────────────────────
void unmap_page(AddressSpace& space, uint64_t virt) {
    PageTable* pml4 = space.pml4;

    PTE& l4e = pml4->entries[pml4_idx(virt)];
    if (!(l4e & FLAG_PRESENT)) return;

    auto* pdpt = reinterpret_cast<PageTable*>(entry_phys(l4e));
    PTE& l3e = pdpt->entries[pdpt_idx(virt)];
    if (!(l3e & FLAG_PRESENT)) return;

    auto* pd = reinterpret_cast<PageTable*>(entry_phys(l3e));
    PTE& l2e = pd->entries[pd_idx(virt)];
    if (!(l2e & FLAG_PRESENT)) return;

    auto* pt = reinterpret_cast<PageTable*>(entry_phys(l2e));
    pt->entries[pt_idx(virt)] = 0;

    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");
}

// ── virt_to_phys ──────────────────────────────────────────────────────────
uint64_t virt_to_phys(AddressSpace& space, uint64_t virt) {
    PageTable* pml4 = space.pml4;

    PTE l4e = pml4->entries[pml4_idx(virt)];
    if (!(l4e & FLAG_PRESENT)){ 
        serial::write("PML4 level not found: "); 
        serial::write_hex(l4e);
        serial::write_line("");
        return 0; 
    }

    PTE l3e = reinterpret_cast<PageTable*>(entry_phys(l4e))->entries[pdpt_idx(virt)];
    if (!(l3e & FLAG_PRESENT)) { 
        serial::write("PML3 level not found: "); 
        serial::write_hex(l3e);
        serial::write_line("");
        return 0; 
    }

    PTE l2e = reinterpret_cast<PageTable*>(entry_phys(l3e))->entries[pd_idx(virt)];
    if (!(l2e & FLAG_PRESENT)) { 
        serial::write("PML2 level not found: "); 
        serial::write_dec(l2e); 
        serial::write_line("");
        return 0; 
    }
    if (l2e & FLAG_HUGE) return entry_phys(l2e) + (virt & 0x1FFFFF);

    PTE l1e = reinterpret_cast<PageTable*>(entry_phys(l2e))->entries[pt_idx(virt)];
    if (!(l1e & FLAG_PRESENT)) { 
        serial::write("PML1 level not found: "); 
        serial::write_dec(l1e);
        serial::write_line("");
        return 0; }

    return entry_phys(l1e) + (virt & 0xFFF);
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    PageTable* pml4 = alloc_table();
    if (!pml4) {
        vga::write_line("[!!] VMM: PMM returned null — out of frames!");
        return;
    }
    kernel_space.pml4 = pml4;

    // Identity-map first 1 GiB via 2 MiB huge pages (PML4[0])
    PageTable* pdpt = alloc_table();
    pml4->entries[0] = reinterpret_cast<uint64_t>(pdpt) | FLAG_PRESENT | FLAG_WRITE;

    PageTable* pd = alloc_table();
    pdpt->entries[0] = reinterpret_cast<uint64_t>(pd) | FLAG_PRESENT | FLAG_WRITE;

    for (int i = 0; i < 512; i++)
        pd->entries[i] = ((uint64_t)i * 0x200000) | FLAG_PRESENT | FLAG_WRITE | FLAG_HUGE;

    // Map kernel at KERNEL_VIRT_BASE 0xFFFFFFFF80000000 (PML4[511], PDPT[510])
    PageTable* hi_pdpt = alloc_table();
    pml4->entries[511] = reinterpret_cast<uint64_t>(hi_pdpt) | FLAG_PRESENT | FLAG_WRITE;

    PageTable* hi_pd = alloc_table();
    hi_pdpt->entries[510] = reinterpret_cast<uint64_t>(hi_pd) | FLAG_PRESENT | FLAG_WRITE;

    for (int i = 0; i < 512; i++)
        hi_pd->entries[i] = ((uint64_t)i * 0x200000) | FLAG_PRESENT | FLAG_WRITE | FLAG_HUGE;

    // Activate
    __asm__ volatile("mov %0, %%cr3" :: "r"(reinterpret_cast<uint64_t>(pml4)) : "memory");
}

// ── create_user_space ─────────────────────────────────────────────────────
// Allocates a fresh PML4 and copies the kernel's upper-half PML4 entries
// into it so that kernel mappings are visible after a syscall without
// needing to switch CR3.
AddressSpace create_user_space() {
    AddressSpace space;
    space.pml4 = alloc_table();
    if (!space.pml4) return space;

    // Zero lower half (user space — starts empty)
    for (int i = 0; i < 256; i++)
        space.pml4->entries[i] = 0;
    
    // Seed PML4[0] with a private copy of the kernel's 0..1GiB identity map.
    // This keeps kernel physical-pointer access working after CR3 switch,
    // while still allowing us to safely customize low user mappings (e.g. 0x400000)
    // without mutating kernel_space page tables.
    PageTable* low_pdpt = alloc_table();
    if (!low_pdpt) return space;
    PageTable* low_pd = alloc_table();
    if (!low_pd) {
        pmm::free_frame(reinterpret_cast<uint64_t>(low_pdpt));
        return space;
    }

    space.pml4->entries[0] = reinterpret_cast<uint64_t>(low_pdpt) | FLAG_PRESENT | FLAG_WRITE;
    low_pdpt->entries[0] = reinterpret_cast<uint64_t>(low_pd) | FLAG_PRESENT | FLAG_WRITE;
    for (int i = 0; i < 512; i++)
        low_pd->entries[i] = ((uint64_t)i * 0x200000) | FLAG_PRESENT | FLAG_WRITE | FLAG_HUGE;

    // Copy upper half from kernel PML4 (entries 256–511)
    // This gives the user process access to all kernel mappings
    // after a syscall, without CR3 switch overhead.
    for (int i = 256; i < 512; i++)
        space.pml4->entries[i] = kernel_space.pml4->entries[i];

    return space;
}


AddressSpace clone_user_space(AddressSpace& src) {
    AddressSpace dst = create_user_space();
    if (!dst.pml4 || !src.pml4) return dst;

    for (int pml4_i = 0; pml4_i < 256; pml4_i++) {
        PTE pml4e = src.pml4->entries[pml4_i];
        if (!(pml4e & FLAG_PRESENT) || (pml4e & FLAG_HUGE)) continue;

        PageTable* src_pdpt = reinterpret_cast<PageTable*>(pml4e & ~0xFFFULL);
        for (int pdpt_i = 0; pdpt_i < 512; pdpt_i++) {
            PTE pdpte = src_pdpt->entries[pdpt_i];
            if (!(pdpte & FLAG_PRESENT) || (pdpte & FLAG_HUGE)) continue;

            PageTable* src_pd = reinterpret_cast<PageTable*>(pdpte & ~0xFFFULL);
            for (int pd_i = 0; pd_i < 512; pd_i++) {
                PTE pde = src_pd->entries[pd_i];
                if (!(pde & FLAG_PRESENT) || (pde & FLAG_HUGE)) continue;

                PageTable* src_pt = reinterpret_cast<PageTable*>(pde & ~0xFFFULL);
                for (int pt_i = 0; pt_i < 512; pt_i++) {
                    PTE pte = src_pt->entries[pt_i];
                    if (!(pte & FLAG_PRESENT)) continue;
                    if (!(pte & FLAG_USER)) continue;

                    uint64_t va = ((uint64_t)pml4_i << 39)
                                | ((uint64_t)pdpt_i << 30)
                                | ((uint64_t)pd_i << 21)
                                | ((uint64_t)pt_i << 12);

                    uint64_t src_phys = pte & ~0xFFFULL;
                    uint64_t dst_phys = pmm::alloc_frame();
                    if (!dst_phys) {
                        destroy_user_space(dst);
                        return {nullptr};
                    }
                    if (!map_page(dst, va, dst_phys, pte & (FLAG_PRESENT | FLAG_WRITE | FLAG_USER | FLAG_NX))) {
                        pmm::free_frame(dst_phys);
                        destroy_user_space(dst);
                        return {nullptr};
                    }

                    uint8_t* srcp = reinterpret_cast<uint8_t*>(src_phys);
                    uint8_t* dstp = reinterpret_cast<uint8_t*>(dst_phys);
                    for (uint32_t i = 0; i < 4096; i++) dstp[i] = srcp[i];
                }
            }
        }
    }

    return dst;
}

// ── destroy_user_space ────────────────────────────────────────────────────
// Frees page table pages for the lower half only.
// Does NOT free the physical frames that user pages point to —
// that's the process's job (or will be when we have proper mm).
void destroy_user_space(AddressSpace& space) {
    if (!space.pml4) return;
    // Free lower-half PDPT pages
    for (int i = 0; i < 256; i++) {
        PTE e = space.pml4->entries[i];
        if (!(e & FLAG_PRESENT)) continue;
        PageTable* pdpt = reinterpret_cast<PageTable*>(e & ~0xFFFULL);
        for (int j = 0; j < 512; j++) {
            PTE pe = pdpt->entries[j];
            if (!(pe & FLAG_PRESENT) || (pe & FLAG_HUGE)) continue;
            PageTable* pd = reinterpret_cast<PageTable*>(pe & ~0xFFFULL);
            for (int k = 0; k < 512; k++) {
                PTE pde = pd->entries[k];
                if (!(pde & FLAG_PRESENT) || (pde & FLAG_HUGE)) continue;
                pmm::free_frame(pde & ~0xFFFULL);
            }
            pmm::free_frame(pe & ~0xFFFULL);
        }
        pmm::free_frame(e & ~0xFFFULL);
    }
    pmm::free_frame(reinterpret_cast<uint64_t>(space.pml4));
    space.pml4 = nullptr;
}

// ── activate ──────────────────────────────────────────────────────────────
// Sync upper-half BEFORE loading CR3, then switch.
// space.pml4 is a physical address accessible via identity map (< 1GiB).
// We must do the copy while the current (kernel) CR3 is still active,
// because after loading the user CR3 those physical pointers may fault.
void activate(AddressSpace& space) {
    if (space.pml4 && space.pml4 != kernel_space.pml4) {
        // [0]       = low-half identity map for kernel physical ptr access.
        //             create_user_space() seeds this with a private copy.
        //             If it's absent (legacy spaces), fall back to kernel entry[0].
        // [256-511] = kernel upper half (heap, vmm, code)
        // [1-255]   = user mappings — left as-is (populated by map_page)
        if (!(space.pml4->entries[0] & FLAG_PRESENT))
            space.pml4->entries[0] = kernel_space.pml4->entries[0];

        for (int i = 256; i < 512; i++)
            space.pml4->entries[i] = kernel_space.pml4->entries[i];
    }
    // Now switch — upper half is current, safe to run kernel code after this
    __asm__ volatile(
        "mov %0, %%cr3"
        :: "r"(reinterpret_cast<uint64_t>(space.pml4))
        : "memory"
    );
}

} // namespace vmm
