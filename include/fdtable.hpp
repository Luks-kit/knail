#pragma once
// include/fdtable.hpp - Per-task file descriptor table
//
// FD 0/1/2 (stdin/stdout/stderr) are reserved and handled by the
// Write/Read syscalls directly. Real VFS fds start at FD_FIRST (3).
#include <stdint.h>
#include "vfs.hpp"

namespace fdtable {

static constexpr uint32_t MAX_FDS   = 64;
static constexpr uint32_t FD_OFFSET = 3; // first allocatable index

// ── FDTable ───────────────────────────────────────────────────────────────
// One per task. Embedded in the Task struct (or heap-allocated and pointed to)
struct FDTable {
    vfs::FileDescriptor* fds[MAX_FDS]; // fds[0] = kernel fd 3, fds[1] = fd 4...
    uint32_t             count;        // number of open entries
};

// Allocate and zero a new FDTable.
FDTable* alloc();

// Free an FDTable, closing all open fds.
void free(FDTable* table);

// Install a FileDescriptor into the table. Returns the fd number (>= 3),
// or -1 if the table is full.
int32_t install(FDTable* table, vfs::FileDescriptor* file);


// Install a FileDescriptor at a specific fd number. Returns fd or -1.
int32_t install_at(FDTable* table, vfs::FileDescriptor* file, int32_t fd);

// Look up fd number. Returns null if not open or out of range.
vfs::FileDescriptor* lookup(FDTable* table, uint64_t fd);

// Remove fd from table (does NOT close the FileDescriptor).
// Returns the FileDescriptor so the caller can close it, or null if invalid.
vfs::FileDescriptor* remove(FDTable* table, uint64_t fd);

// Clone a table (for fork/spawn). All vnodes get their refcounts bumped.
FDTable* clone(FDTable* src);

} // namespace fdtable
