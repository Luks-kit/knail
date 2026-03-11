#pragma once
// include/spawn.hpp - Spawn a user process from a parsed ELF image
#include <stdint.h>
#include "elf.hpp"

namespace sched {

// Spawn a user task from a validated ELF image.
// Creates a fresh address space, maps all PT_LOAD segments,
// sets up the initial stack with argc/argv/envp/auxv,
// and enqueues the task. Returns tid or 0 on failure.
uint32_t spawn_elf(const elf::Image& image,
                   const char* name,
                   const char* argv[] = nullptr,
                   uint32_t    argc   = 0,
                   const char* envp[] = nullptr,
                   uint32_t    envc   = 0);

} // namespace sched
