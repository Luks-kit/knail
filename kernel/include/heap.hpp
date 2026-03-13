#pragma once
// include/heap.hpp - Knail kernel heap allocator
// Simple free-list allocator (first-fit) backed by VMM pages.
// Provides kmalloc / kfree / krealloc.

#include <stdint.h>
#include <stddef.h>

namespace heap {

// Kernel heap lives at this virtual address range
static constexpr uint64_t HEAP_START = 0xFFFF900000000000ULL;
static constexpr uint64_t HEAP_MAX   = 0xFFFF900040000000ULL; // 1 GiB max

void  init();
void* kmalloc(size_t size);
void* kcalloc(size_t count, size_t size);
void* krealloc(void* ptr, size_t new_size);
void  kfree(void* ptr);

// Debug: print heap stats to VGA
void dump_stats();

} // namespace heap
