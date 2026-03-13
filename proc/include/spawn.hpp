#pragma once
// include/spawn.hpp - Spawn a user process from a parsed ELF image
#include "elf.hpp"
#include "types.hpp"

namespace sched {

// Spawn a user task from a validated ELF image.
// Creates a fresh address space, maps all PT_LOAD segments,
// sets up the initial stack with argc/argv/envp/auxv,
// and enqueues the task. Returns tid on success.
kResult<u32> spawn_elf(const elf::Image& image,
                       const char* name,
                       const char* argv[] = nullptr,
                       u32         argc   = 0,
                       const char* envp[] = nullptr,
                       u32         envc   = 0);

} // namespace sched
