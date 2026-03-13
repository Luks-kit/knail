#pragma once
// include/fdtable.hpp - Per-task file descriptor table
//
// FD 0/1/2 (stdin/stdout/stderr) are reserved and handled by the
// Write/Read syscalls directly. Real VFS fds start at FD_FIRST (3).
#include <stdint.h>
#include "vfs.hpp"
#include "types.hpp"

namespace fdtable {

static constexpr u32 MAX_FDS   = 64;
static constexpr u32 FD_OFFSET = 3;   // first allocatable index

// ── FDTable ───────────────────────────────────────────────────────────────
struct FDTable {
    vfs::FileDescriptor* fds[MAX_FDS];  // fds[0] = kernel fd 3, fds[1] = fd 4...
    u32                  count;
};

// Allocate and zero a new FDTable.
FDTable* alloc();

// Free an FDTable, closing all open fds.
void free(FDTable* table);

// Install a FileDescriptor into the table.
// Returns the fd number (>= 3) on success, kError::Overflow if full.
kResult<i32> install(FDTable* table, vfs::FileDescriptor* file);

// Install a FileDescriptor at a specific fd number.
// Returns the fd number on success, kError::InvalidArgument if slot invalid/occupied.
kResult<i32> install_at(FDTable* table, vfs::FileDescriptor* file, i32 fd);

// Look up fd number.
// Returns the FileDescriptor on success, kError::BadFileDescriptor if not open.
kResult<vfs::FileDescriptor*> lookup(FDTable* table, u64 fd);

// Remove fd from table (does NOT close the FileDescriptor).
// Returns the FileDescriptor so the caller can close it.
// Returns kError::BadFileDescriptor if invalid.
kResult<vfs::FileDescriptor*> remove(FDTable* table, u64 fd);

// Clone a table (for fork/spawn). All vnodes get their refcounts bumped.
FDTable* clone(FDTable* src);

} // namespace fdtable
