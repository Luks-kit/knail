#pragma once
// include/vfs.hpp - Knail Virtual Filesystem layer
#include <stdint.h>
#include <stddef.h>
#include "syscall.hpp"
#include "types.hpp"

// ── Internal access flags (non-zero, never exposed to userspace) ──────────
static constexpr u64 VFS_READ   = (1 << 8);
static constexpr u64 VFS_WRITE  = (1 << 9);
static constexpr u64 VFS_APPEND = (1 << 10);

namespace vfs {

// ── Forward declarations ──────────────────────────────────────────────────
struct VNode;

// ── File operations vtable ────────────────────────────────────────────────
// Concrete filesystems (ramfs, fat32, ext2) fill this in.
struct FileOps {
    // Read len bytes from node at offset into buf. Returns bytes read.
    kResult<i64> (*read)   (VNode* node, u64 offset, void* buf, u64 len);
    // Write len bytes from buf into node at offset. Returns bytes written.
    kResult<i64> (*write)  (VNode* node, u64 offset, const void* buf, u64 len);
    // Called when last fd referencing this node is closed. Optional.
    void         (*close)  (VNode* node);
    // Optional: create child node under directory.
    VNode*       (*create) (VNode* dir, const char* name, u32 type);
    // Optional: unlink a child node from directory.
    kStatus      (*unlink) (VNode* dir, const char* name);
    // Iterate directory. index = which entry (0-based). Fills dirent.
    // Returns kStatus::ok() on success, kError::EndOfFile when exhausted.
    kStatus      (*readdir)(VNode* node, u64 index, Dirent* out);
};

// ── VNode ─────────────────────────────────────────────────────────────────
struct VNode {
    char     name[128];
    u32      type;            // VFS_TYPE_FILE or VFS_TYPE_DIR
    u64      size;            // bytes (files); 0 for dirs
    VNode*   parent;
    VNode*   children;        // linked list of children (dirs only)
    VNode*   next_sibling;    // next in parent's children list
    FileOps* ops;             // filesystem ops — null for ramfs handled inline
    void*    data;            // ramfs: heap buffer; real fs: fs-private ptr
    u64      data_capacity;   // allocated bytes in data buffer (ramfs)
    u32      refcount;        // number of open fds pointing here
    void*    priv;            // filesystem-private data (e.g. Fat32Node*)
};

// ── FileDescriptor ────────────────────────────────────────────────────────
struct FileDescriptor {
    VNode* node;
    u64    offset;      // current read/write position
    u64    flags;       // VFS_READ / VFS_WRITE / VFS_APPEND
    u64    dir_index;   // readdir cursor (dirs only)
};

// ── Mount point ───────────────────────────────────────────────────────────
struct Mount {
    char   path[128];   // e.g. "/" or "/dev"
    VNode* root;
    Mount* next;
};

// ── API ───────────────────────────────────────────────────────────────────

// Initialise VFS and create root ramfs mount at "/"
void init();

// Resolve an absolute path to a VNode.
kResult<VNode*> resolve(const char* path);

// Create a file or directory at path. type = VFS_TYPE_FILE or VFS_TYPE_DIR.
kResult<VNode*> create(const char* path, u32 type);

// Open a path with flags. Returns a heap-allocated FileDescriptor.
kResult<FileDescriptor*> open(const char* path, u64 flags);

// Close a FileDescriptor. Decrements refcount, frees if zero.
void close(FileDescriptor* fd);

// Read from an open fd. Returns bytes read.
kResult<i64> read(FileDescriptor* fd, void* buf, u64 len);

// Write to an open fd. Returns bytes written.
kResult<i64> write(FileDescriptor* fd, const void* buf, u64 len);

// Seek. whence = SEEK_SET / SEEK_CUR / SEEK_END. Returns new offset.
kResult<i64> seek(FileDescriptor* fd, i64 offset, u64 whence);

// Fill a StatBuf for a path.
kStatus stat(const char* path, StatBuf* out);

// Create directory (all parents must exist).
kStatus mkdir(const char* path);

// Remove a file (not a directory).
kStatus unlink(const char* path);

// Get/set cwd vnode for current task
VNode* get_cwd();
void   set_cwd(VNode* node);

// Build absolute path string for a vnode by walking up to root
void build_path(VNode* node, char* buf, usize max);

// Duplicate a FileDescriptor (new offset, same vnode, bumps refcount).
kResult<FileDescriptor*> dup(FileDescriptor* fd);

// Read one directory entry at fd's current dir_index. Advances dir_index.
// Returns kError::EndOfFile when done.
kStatus readdir(FileDescriptor* fd, Dirent* out);

// Create a pipe. read_out and write_out are the two ends.
kStatus pipe(FileDescriptor** read_out, FileDescriptor** write_out);

// Mount an external filesystem root at path.
kStatus mount(const char* path, VNode* fs_root);

} // namespace vfs
