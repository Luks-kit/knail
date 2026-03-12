#pragma once
// include/vfs.hpp - Knail Virtual Filesystem layer
#include <stdint.h>
#include <stddef.h>
#include "syscall.hpp"

// ── Internal access flags (non-zero, never exposed to userspace) ──────────
static constexpr uint64_t VFS_READ   = (1 << 8);
static constexpr uint64_t VFS_WRITE  = (1 << 9);
static constexpr uint64_t VFS_APPEND = (1 << 10);

namespace vfs {

// ── Forward declarations ──────────────────────────────────────────────────
struct VNode;

// ── File operations vtable ────────────────────────────────────────────────
// Concrete filesystems (ramfs, fat32, ext2) fill this in.
// All ops return >= 0 on success, negative errno on failure.
struct FileOps {
    // Read len bytes from node at offset into buf. Returns bytes read.
    int64_t (*read) (VNode* node, uint64_t offset, void* buf, uint64_t len);
    // Write len bytes from buf into node at offset. Returns bytes written.
    int64_t (*write)(VNode* node, uint64_t offset, const void* buf, uint64_t len);
    // Called when last fd referencing this node is closed. Optional.
    void    (*close)(VNode* node);
    // Optional: create child node under directory.
    VNode*  (*create)(VNode* dir, const char* name, uint32_t type);
    // Optional: unlink a child node from directory.
    int64_t (*unlink)(VNode* dir, const char* name);
    // Iterate directory. index = which entry (0-based). Fills dirent.
    // Returns E_OK on success, E_EOF when no more entries.
    int64_t (*readdir)(VNode* node, uint64_t index, Dirent* out);
};

// ── VNode ─────────────────────────────────────────────────────────────────
// One per file or directory in the VFS tree.
struct VNode {
    char        name[128];
    uint32_t    type;           // VFS_TYPE_FILE or VFS_TYPE_DIR
    uint64_t    size;           // bytes (files); 0 for dirs
    VNode*      parent;
    VNode*      children;       // linked list of children (dirs only)
    VNode*      next_sibling;   // next in parent's children list
    FileOps*    ops;            // filesystem ops — null for ramfs handled inline
    void*       data;           // ramfs: heap buffer; real fs: fs-private ptr
    uint64_t    data_capacity;  // allocated bytes in data buffer (ramfs)
    uint32_t    refcount;       // number of open fds pointing here
    void*       priv;           // filesystem-private data (e.g. Fat32Node*)
};

// ── FileDescriptor ────────────────────────────────────────────────────────
struct FileDescriptor {
    VNode*   node;
    uint64_t offset;    // current read/write position
    uint64_t flags;     // O_READ / O_WRITE / O_APPEND
    uint64_t dir_index; // readdir cursor (dirs only)
};

// ── Mount point ───────────────────────────────────────────────────────────
struct Mount {
    char     path[128]; // e.g. "/" or "/dev"
    VNode*   root;      // root vnode of this mount
    Mount*   next;
};

// ── API ───────────────────────────────────────────────────────────────────

// Initialise VFS and create root ramfs mount at "/"
void init();

// Resolve an absolute path to a VNode. Returns null if not found.
VNode* resolve(const char* path);

// Create a file or directory at path. type = VFS_TYPE_FILE or VFS_TYPE_DIR.
// Returns the new VNode or null on failure.
VNode* create(const char* path, uint32_t type);

// Open a path with flags. Returns a heap-allocated FileDescriptor or null.
FileDescriptor* open(const char* path, uint64_t flags);

// Close a FileDescriptor. Decrements refcount, frees if zero.
void close(FileDescriptor* fd);

// Read from an open fd. Returns bytes read or negative errno.
int64_t read(FileDescriptor* fd, void* buf, uint64_t len);

// Write to an open fd. Returns bytes written or negative errno.
int64_t write(FileDescriptor* fd, const void* buf, uint64_t len);

// Seek. whence = SEEK_SET / SEEK_CUR / SEEK_END.
// Returns new offset or negative errno.
int64_t seek(FileDescriptor* fd, int64_t offset, uint64_t whence);

// Fill a StatBuf for a path. Returns E_OK or negative errno.
int64_t stat(const char* path, StatBuf* out);

// Create directory (all parents must exist). Returns E_OK or negative errno.
int64_t mkdir(const char* path);

// Remove a file (not a directory). Returns E_OK or negative errno.
int64_t unlink(const char* path);

// Get/set cwd vnode for current task
vfs::VNode* get_cwd();
void        set_cwd(vfs::VNode* node);

// Build absolute path string for a vnode by walking up to root
void build_path(VNode* node, char* buf, size_t max);

// Duplicate a FileDescriptor (new offset, same vnode, bumps refcount).
FileDescriptor* dup(FileDescriptor* fd);

// Read one directory entry at fd's current dir_index. Advances dir_index.
// Returns E_OK, or E_EOF when done.
int64_t readdir(FileDescriptor* fd, Dirent* out);


// Create a pipe. read_out and write_out are the two ends.
int64_t pipe(FileDescriptor** read_out, FileDescriptor** write_out);

// Mount an external filesystem root at path.
// The path must already exist as a directory in the VFS tree.
int64_t mount(const char* path, VNode* fs_root);

} // namespace vfs
