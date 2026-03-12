// kernel/vfs.cpp - Knail VFS + ramfs implementation
#include "vfs.hpp"
#include "keyboard.hpp"
#include "heap.hpp"
#include "serial.hpp"
#include "vga.hpp" 
#include "scheduler.hpp"

// ── Internal string helpers ───────────────────────────────────────────────
static size_t kstrlen(const char* s) {
    size_t n = 0; while (s[n]) n++; return n;
}
static void kstrcpy(char* dst, const char* src, size_t max) {
    size_t i = 0;
    while (i + 1 < max && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}
static bool kstreq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

namespace vfs {

// ── Globals ───────────────────────────────────────────────────────────────
static VNode* vfs_root   = nullptr;
static Mount* mount_list = nullptr;

// ── VNode helpers ─────────────────────────────────────────────────────────
static VNode* alloc_vnode(const char* name, uint32_t type) {
    VNode* n = reinterpret_cast<VNode*>(heap::kmalloc(sizeof(VNode)));
    if (!n) return nullptr;
    // zero
    for (size_t i = 0; i < sizeof(VNode); i++)
        reinterpret_cast<uint8_t*>(n)[i] = 0;
    kstrcpy(n->name, name, sizeof(n->name));
    n->type = type;
    return n;
}

static void add_child(VNode* parent, VNode* child) {
    child->parent       = parent;
    child->next_sibling = parent->children;
    parent->children    = child;
}

// ── ramfs ops ─────────────────────────────────────────────────────────────
// For ramfs, data is a heap buffer that grows on write.

static int64_t ramfs_read(VNode* node, uint64_t offset,
                           void* buf, uint64_t len) {
    if (!node->data || offset >= node->size) return 0;
    uint64_t avail = node->size - offset;
    if (len > avail) len = avail;
    uint8_t* src = reinterpret_cast<uint8_t*>(node->data) + offset;
    uint8_t* dst = reinterpret_cast<uint8_t*>(buf);
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    return (int64_t)len;
}

static int64_t ramfs_write(VNode* node, uint64_t offset,
                            const void* buf, uint64_t len) {
    uint64_t end = offset + len;
    // Grow buffer if needed
    if (end > node->data_capacity) {
        uint64_t newcap = end * 2;
        if (newcap < 64) newcap = 64;
        void* newbuf = heap::kmalloc(newcap);
        if (!newbuf) return E_NOSPACE;
        // copy old
        uint8_t* nb = reinterpret_cast<uint8_t*>(newbuf);
        uint8_t* ob = reinterpret_cast<uint8_t*>(node->data);
        for (uint64_t i = 0; i < node->size; i++) nb[i] = ob[i];
        // zero new region
        for (uint64_t i = node->size; i < newcap; i++) nb[i] = 0;
        if (node->data) heap::kfree(node->data);
        node->data          = newbuf;
        node->data_capacity = newcap;
    }
    uint8_t* dst = reinterpret_cast<uint8_t*>(node->data) + offset;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(buf);
    for (uint64_t i = 0; i < len; i++) dst[i] = src[i];
    if (end > node->size) node->size = end;
    return (int64_t)len;
}

static int64_t ramfs_readdir(VNode* node, uint64_t index, Dirent* out) {
    VNode* child = node->children;
    uint64_t i = 0;
    while (child) {
        if (i == index) {
            kstrcpy(out->name, child->name, sizeof(out->name));
            out->type = child->type;
            return 1;  // means found ( EOF is zero ) 
        }
        i++;
        child = child->next_sibling;
    }
    return E_EOF;
}

static FileOps ramfs_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .close   = nullptr,
    .create  = nullptr,
    .unlink  = nullptr,
    .readdir = ramfs_readdir,
};

// ── tty (keyboard) device ─────────────────────────────────────────────────
static int64_t tty_read(VNode* /*node*/, uint64_t /*offset*/,
                         void* buf, uint64_t len) {
    if (!buf || len == 0) return E_FAULT;
    char* dst = reinterpret_cast<char*>(buf);
    uint64_t i = 0;
    while (i < len) {
        char c = keyboard::read_char(); // blocks until keypress
        dst[i++] = c;
        if (c == '\n' || c == '\r') break; // line-buffered
    }
    return (int64_t)i;
}

static int64_t tty_write(VNode* /*node*/, uint64_t /*offset*/,
                          const void* buf, uint64_t len) {
    const char* src = reinterpret_cast<const char*>(buf);
    for (uint64_t i = 0; i < len; i++) {
        vga::put_char(src[i]);
        serial::write_char(src[i]);
    }
    return (int64_t)len;
}

static FileOps tty_ops = {
    .read    = tty_read,
    .write   = tty_write,
    .close   = nullptr,
    .readdir = nullptr,
};

static VNode tty_node = {
    .name     = "tty",
    .type     = VFS_TYPE_FILE,
    .size     = 0,
    .ops      = &tty_ops,
};

// ── Path utilities ────────────────────────────────────────────────────────
// Split path into head component and remainder.
// e.g. "/foo/bar/baz" → head="foo", rest="/bar/baz"
static const char* path_next(const char* path, char* component, size_t max) {
    while (*path == '/') path++;
    if (!*path) { component[0] = 0; return path; }
    size_t i = 0;
    while (*path && *path != '/' && i + 1 < max)
        component[i++] = *path++;
    component[i] = 0;
    return path;
}

// ── resolve ───────────────────────────────────────────────────────────────
VNode* resolve(const char* path) {
    if (!path || !path[0]) return nullptr;

    VNode* node;
    if (path[0] == '/') {
        node = vfs_root;  // absolute
    } else {
        node = get_cwd(); // relative — start from cwd
        if (!node) node = vfs_root;
    }

    const char* p = path;
    if (path[0] == '/') p++; // skip leading slash

    char component[128];
    while (true) {
        p = path_next(p, component, sizeof(component));
        if (!component[0]) return node;
        if (!kstreq(component, ".")) {
            if (kstreq(component, "..")) {
                // Go up to parent
                if (node->parent) node = node->parent;
                continue;
            }
            if (node->type != VFS_TYPE_DIR) return nullptr;
            VNode* child = node->children;
            while (child) {
                if (kstreq(child->name, component)) { node = child; break; }
                child = child->next_sibling;
            }
            if (!child) return nullptr;
        }
    }
}

// Build absolute path string for a vnode by walking up to root
void build_path(VNode* node, char* buf, size_t max) {
    if (!node || node == vfs_root) {
        buf[0] = '/'; buf[1] = 0;
        return;
    }
    // Collect components by walking up
    const char* parts[32];
    int depth = 0;
    VNode* n = node;
    while (n && n != vfs_root && depth < 32) {
        parts[depth++] = n->name;
        n = n->parent;
    }
    // Build path string
    size_t pos = 0;
    for (int i = depth - 1; i >= 0 && pos < max - 2; i--) {
        buf[pos++] = '/';
        const char* part = parts[i];
        size_t j = 0;
        while (part[j] && pos < max - 1) buf[pos++] = part[j++];
    }
    buf[pos] = 0;
}

// Resolve parent directory of path. Fills leaf_name with the final component.
static VNode* resolve_parent(const char* path, char* leaf_name, size_t max) {
    size_t len = kstrlen(path);
    
    // Find last '/'
    size_t last_slash = 0;
    bool has_slash = false;
    for (size_t i = 0; i < len; i++) {
        if (path[i] == '/') { last_slash = i; has_slash = true; }
    }

    if (!has_slash) {
        // Bare name like "foo" — parent is cwd
        kstrcpy(leaf_name, path, max);
        VNode* cwd = get_cwd();
        return cwd ? cwd : vfs_root;
    }

    // Copy leaf component
    kstrcpy(leaf_name, path + last_slash + 1, max);

    // Build parent path string
    char parent_path[256];
    size_t plen = (last_slash == 0) ? 1 : last_slash;
    for (size_t i = 0; i < plen && i < 255; i++) parent_path[i] = path[i];
    parent_path[plen] = 0;

    return resolve(parent_path);
}

// ── create ────────────────────────────────────────────────────────────────
VNode* create(const char* path, uint32_t type) {
    char leaf[128];
    VNode* parent = resolve_parent(path, leaf, sizeof(leaf));
    if (!parent) return nullptr;
    if (parent->type != VFS_TYPE_DIR) return nullptr;
    // Check not already exists
    VNode* child = parent->children;
    while (child) {
        if (kstreq(child->name, leaf)) return nullptr; // E_EXIST
        child = child->next_sibling;
    }

    VNode* node = nullptr;
    if (parent->ops && parent->ops->create) {
        node = parent->ops->create(parent, leaf, type);
    } else {
        node = alloc_vnode(leaf, type);
        if (!node) return nullptr;
        node->ops = &ramfs_ops;
    }
    if (!node) return nullptr;

    add_child(parent, node);
    return node;
}

// ── Pipe ──────────────────────────────────────────────────────────────────
static constexpr uint64_t PIPE_BUF_SIZE = 4096;

struct PipeBuffer {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint64_t read_pos;
    uint64_t write_pos;
    uint64_t used;
    uint32_t refcount; // 2 = both ends open, 1 = one end closed, 0 = free
};

static int64_t pipe_read(VNode* node, uint64_t /*offset*/,
                          void* buf, uint64_t len) {
    PipeBuffer* pb = reinterpret_cast<PipeBuffer*>(node->data);
    if (!pb) return E_INVAL;
    if (pb->used == 0) return 0; // no data, would block — return 0 for now
    uint64_t to_read = len < pb->used ? len : pb->used;
    uint8_t* dst = reinterpret_cast<uint8_t*>(buf);
    for (uint64_t i = 0; i < to_read; i++) {
        dst[i] = pb->buf[pb->read_pos];
        pb->read_pos = (pb->read_pos + 1) % PIPE_BUF_SIZE;
    }
    pb->used -= to_read;
    return (int64_t)to_read;
}

static int64_t pipe_write(VNode* node, uint64_t /*offset*/,
                           const void* buf, uint64_t len) {
    PipeBuffer* pb = reinterpret_cast<PipeBuffer*>(node->data);
    if (!pb) return E_INVAL;
    uint64_t space = PIPE_BUF_SIZE - pb->used;
    uint64_t to_write = len < space ? len : space;
    if (to_write == 0) return E_NOSPACE;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(buf);
    for (uint64_t i = 0; i < to_write; i++) {
        pb->buf[pb->write_pos] = src[i];
        pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
    }
    pb->used += to_write;
    return (int64_t)to_write;
}

static void pipe_close(VNode* node) {
    PipeBuffer* pb = reinterpret_cast<PipeBuffer*>(node->data);
    if (!pb) return;
    pb->refcount--;
    if (pb->refcount == 0) {
        heap::kfree(pb);
        node->data = nullptr;
    }
}

static FileOps pipe_ops = {
    .read    = pipe_read,
    .write   = pipe_write,
    .close   = pipe_close,
    .create  = nullptr,
    .unlink  = nullptr,
    .readdir = nullptr,
};

int64_t pipe(FileDescriptor** read_out, FileDescriptor** write_out) {
    if (!read_out || !write_out) return E_FAULT;

    PipeBuffer* pb = reinterpret_cast<PipeBuffer*>(heap::kmalloc(sizeof(PipeBuffer)));
    if (!pb) return E_NOSPACE;
    for (size_t i = 0; i < sizeof(PipeBuffer); i++)
        reinterpret_cast<uint8_t*>(pb)[i] = 0;
    pb->refcount = 2;

    // Both ends share one VNode
    VNode* node = alloc_vnode("[pipe]", VFS_TYPE_FILE);
    if (!node) { heap::kfree(pb); return E_NOSPACE; }
    node->ops      = &pipe_ops;
    node->data     = pb;
    node->refcount = 2; // read end + write end

    FileDescriptor* rfd = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    FileDescriptor* wfd = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    if (!rfd || !wfd) {
        if (rfd) heap::kfree(rfd);
        if (wfd) heap::kfree(wfd);
        heap::kfree(node);
        heap::kfree(pb);
        return E_NOSPACE;
    }

    rfd->node      = node;
    rfd->offset    = 0;
    rfd->flags     = VFS_READ;
    rfd->dir_index = 0;

    wfd->node      = node;
    wfd->offset    = 0;
    wfd->flags     = VFS_WRITE;
    wfd->dir_index = 0;

    *read_out  = rfd;
    *write_out = wfd;
    return E_OK;
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    vfs_root = alloc_vnode("/", VFS_TYPE_DIR);
    vfs_root->ops = &ramfs_ops;
    // Create standard directories
    create("/dev",  VFS_TYPE_DIR);
    create("/tmp",  VFS_TYPE_DIR);
    create("/bin",  VFS_TYPE_DIR);
    
    // Register tty device
    VNode* dev = resolve("/dev");
    tty_node.parent       = dev;
    tty_node.next_sibling = dev->children;
    dev->children         = &tty_node;
    
    serial::write_line("[VFS] ramfs mounted at /");
}

// ── open ──────────────────────────────────────────────────────────────────
FileDescriptor* open(const char* path, uint64_t flags) {
    VNode* node = resolve(path);
    if (!node) {
        if (!(flags & O_CREATE)) return nullptr;
        node = create(path, VFS_TYPE_FILE);
        if (!node) return nullptr;
    }
    if ((flags & O_TRUNC) && node->type == VFS_TYPE_FILE) {
        if (node->data) { heap::kfree(node->data); node->data = nullptr; }
        node->size = 0;
        node->data_capacity = 0;
    }
    FileDescriptor* fd = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    if (!fd) return nullptr;

    // Translate Linux access mode (0=rdonly, 1=wronly, 2=rdwr)
    // into internal non-zero flags so bitmask checks work correctly.
    uint64_t accmode = flags & 3;
    uint64_t iflags  = flags; // preserve O_CREATE/O_TRUNC/O_APPEND bits
    if (accmode == O_RDWR)       iflags |= VFS_READ | VFS_WRITE;
    else if (accmode == O_WRITE) iflags |= VFS_WRITE;
    else                         iflags |= VFS_READ; // O_RDONLY=0, default

    if (flags & O_APPEND) iflags |= VFS_APPEND;

    fd->node      = node;
    fd->flags     = iflags;
    fd->dir_index = 0;
    fd->offset    = (iflags & VFS_APPEND) ? node->size : 0;
    node->refcount++;
    return fd;
}


// ── read ──────────────────────────────────────────────────────────────────

int64_t read(FileDescriptor* fd, void* buf, uint64_t len) {
    if (!fd || !buf) return E_FAULT;
    if (!(fd->flags & VFS_READ)) return E_PERM;
    if (fd->node->type == VFS_TYPE_DIR) return E_ISDIR;
    if (!fd->node->ops->read) return E_INVAL;
    int64_t n = fd->node->ops->read(fd->node, fd->offset, buf, len);
    if (n > 0) fd->offset += (uint64_t)n;
    return n;
}

// ── write ─────────────────────────────────────────────────────────────────
int64_t write(FileDescriptor* fd, const void* buf, uint64_t len) {
    if (!fd || !buf) return E_FAULT;
    if (!(fd->flags & VFS_WRITE)) return E_PERM;
    if (fd->node->type == VFS_TYPE_DIR) return E_ISDIR;
    if (!fd->node->ops->write) return E_INVAL;
    if (fd->flags & VFS_APPEND) fd->offset = fd->node->size;
    int64_t n = fd->node->ops->write(fd->node, fd->offset, buf, len);
    if (n > 0) fd->offset += (uint64_t)n;
    return n;
}

// ── close ─────────────────────────────────────────────────────────────────
void close(FileDescriptor* fd) {
    if (!fd) return;
    fd->node->refcount--;
    if (fd->node->refcount == 0 && fd->node->ops->close)
        fd->node->ops->close(fd->node);
    heap::kfree(fd);
}

// ── seek ──────────────────────────────────────────────────────────────────
int64_t seek(FileDescriptor* fd, int64_t offset, uint64_t whence) {
    if (!fd) return E_BADF;
    if (fd->node->type == VFS_TYPE_DIR) return E_ISDIR;
    int64_t base = 0;
    switch (whence) {
        case SEEK_SET: base = 0;                          break;
        case SEEK_CUR: base = (int64_t)fd->offset;        break;
        case SEEK_END: base = (int64_t)fd->node->size;    break;
        default: return E_INVAL;
    }
    int64_t newoff = base + offset;
    if (newoff < 0) return E_INVAL;
    fd->offset = (uint64_t)newoff;
    return newoff;
}

// ── stat ──────────────────────────────────────────────────────────────────
int64_t stat(const char* path, StatBuf* out) {
    if (!out) return E_FAULT;
    VNode* node = resolve(path);
    if (!node) return E_NOENT;
    for (size_t i = 0; i < sizeof(StatBuf); i++)
        reinterpret_cast<uint8_t*>(out)[i] = 0;
    out->st_size    = (int64_t)node->size;
    out->st_mode    = (node->type == VFS_TYPE_DIR)
                    ? (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
                    : (S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO);
    out->st_blksize = 512;
    out->st_blocks  = (int64_t)((node->size + 511) / 512);
    return E_OK;
}

// ── dup ──────────────────────────────────────────────────────────────────
FileDescriptor* dup(FileDescriptor* fd) {
    if (!fd) return nullptr;
    FileDescriptor* copy = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    if (!copy) return nullptr;
    copy->node      = fd->node;
    copy->offset    = fd->offset;
    copy->flags     = fd->flags;
    copy->dir_index = fd->dir_index;
    fd->node->refcount++;
    return copy;
}

// ── mkdir ─────────────────────────────────────────────────────────────────
int64_t mkdir(const char* path) {
    if (resolve(path)) return E_EXIST;
    VNode* node = create(path, VFS_TYPE_DIR);
    return node ? E_OK : E_NOENT;
}

// ── unlink ────────────────────────────────────────────────────────────────
int64_t unlink(const char* path) {
    VNode* node = resolve(path);
    if (!node) return E_NOENT;
    if (node->type == VFS_TYPE_DIR) return E_ISDIR;
    if (node->refcount > 0) return E_BUSY;
    // Remove from parent's children list
    VNode* parent = node->parent;
    if (!parent) return E_INVAL;

    if (parent->ops && parent->ops->unlink) {
        int64_t r = parent->ops->unlink(parent, node->name);
        if (r < 0) return r;
    }

    if (parent->children == node) {
        parent->children = node->next_sibling;
    } else {
        VNode* prev = parent->children;
        while (prev && prev->next_sibling != node) prev = prev->next_sibling;
        if (prev) prev->next_sibling = node->next_sibling;
    }
    if (node->data) heap::kfree(node->data);
    heap::kfree(node);
    return E_OK;
}

vfs::VNode* get_cwd() {
    sched::Task* t = sched::current();
    if (!t || !t->cwd) return vfs_root;
    return t->cwd;
}

void set_cwd(vfs::VNode* node) {
    sched::Task* t = sched::current();
    if (t) t->cwd = node;
}

// ── readdir ───────────────────────────────────────────────────────────────
int64_t readdir(FileDescriptor* fd, Dirent* out) {
    if (!fd || !out) return E_FAULT;
    if (fd->node->type != VFS_TYPE_DIR) return E_NOTDIR;
    if (!fd->node->ops->readdir) return E_INVAL;
    int64_t r = fd->node->ops->readdir(fd->node, fd->dir_index, out);
    if (r > 0) fd->dir_index++;
    return r;
}

// ── mount ─────────────────────────────────────────────────────────────────
int64_t mount(const char* path, VNode* fs_root) {
    VNode* mount_point = resolve(path);
    if (!mount_point) return E_NOENT;
    if (mount_point->type != VFS_TYPE_DIR) return E_NOTDIR;
    Mount* m = reinterpret_cast<Mount*>(heap::kmalloc(sizeof(Mount)));
    if (!m) return E_NOSPACE;
    kstrcpy(m->path, path, sizeof(m->path));
    m->root = fs_root;
    m->next = mount_list;
    mount_list = m;
    
    // Graft fs_root state into the mount point vnode so future operations
    // (create/write/readdir/etc) have the filesystem-private context.
    mount_point->children = fs_root->children;
    mount_point->ops      = fs_root->ops;
    mount_point->priv     = fs_root->priv;
    mount_point->data     = fs_root->data;
    mount_point->data_capacity = fs_root->data_capacity;
    mount_point->size     = fs_root->size;
    mount_point->type     = fs_root->type;

    // fs_root is only a temporary container used for mount wiring;
    // its private payload now belongs to mount_point.
    fs_root->priv = nullptr;
    fs_root->data = nullptr;

    VNode* child = mount_point->children;
    while (child) { child->parent = mount_point; child = child->next_sibling; }

    heap::kfree(fs_root);

    serial::write("[VFS] mounted at ");
    serial::write_line(path);
    return E_OK;
}

} // namespace vfs
