// fs/vfs.cpp - Knail VFS + ramfs implementation
#include "vfs.hpp"
#include "keyboard.hpp"
#include "heap.hpp"
#include "serial.hpp"
#include "types.hpp"
#include "vga.hpp"
#include "scheduler.hpp"

// ── Internal string helpers ───────────────────────────────────────────────
static usize kstrlen(const char* s) {
    usize n = 0; while (s[n]) n++; return n;
}
static void kstrcpy(char* dst, const char* src, usize max) {
    usize i = 0;
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
static VNode* alloc_vnode(const char* name, u32 type) {
    VNode* n = reinterpret_cast<VNode*>(heap::kmalloc(sizeof(VNode)));
    if (!n) return nullptr;
    for (usize i = 0; i < sizeof(VNode); i++)
        reinterpret_cast<u8*>(n)[i] = 0;
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

static kResult<i64> ramfs_read(VNode* node, u64 offset, void* buf, u64 len) {
    if (!node->data || offset >= node->size) return kResult<i64>::ok(0);
    u64       avail = node->size - offset;
    if (len > avail) len = avail;
    u8*       dst = reinterpret_cast<u8*>(buf);
    const u8* src = reinterpret_cast<const u8*>(node->data) + offset;
    for (u64 i = 0; i < len; i++) dst[i] = src[i];
    return kResult<i64>::ok((i64)len);
}

static kResult<i64> ramfs_write(VNode* node, u64 offset, const void* buf, u64 len) {
    u64 end = offset + len;
    if (end > node->data_capacity) {
        u64 newcap = end * 2;
        if (newcap < 64) newcap = 64;
        void* newbuf = heap::kmalloc(newcap);
        if (!newbuf) return kResult<i64>::err(kError::OutOfMemory);
        u8* nb = reinterpret_cast<u8*>(newbuf);
        u8* ob = reinterpret_cast<u8*>(node->data);
        for (u64 i = 0; i < node->size; i++) nb[i] = ob[i];
        for (u64 i = node->size; i < newcap; i++) nb[i] = 0;
        if (node->data) heap::kfree(node->data);
        node->data          = newbuf;
        node->data_capacity = newcap;
    }
    u8*       dst = reinterpret_cast<u8*>(node->data) + offset;
    const u8* src = reinterpret_cast<const u8*>(buf);
    for (u64 i = 0; i < len; i++) dst[i] = src[i];
    if (end > node->size) node->size = end;
    return kResult<i64>::ok((i64)len);
}

static kStatus ramfs_readdir(VNode* node, u64 index, Dirent* out) {
    VNode* child = node->children;
    u64    i     = 0;
    while (child) {
        if (i == index) {
            kstrcpy(out->name, child->name, sizeof(out->name));
            out->type = child->type;
            return kStatus::ok();
        }
        i++;
        child = child->next_sibling;
    }
    return kStatus::err(kError::EndOfFile);
}

static FileOps ramfs_ops = {
    .read    = ramfs_read,
    .write   = ramfs_write,
    .close   = nullptr,
    .create  = nullptr,
    .unlink  = nullptr,
    .readdir = ramfs_readdir,
};

// ── tty ───────────────────────────────────────────────────────────────────
static char tty_pushback[8];
static int  tty_pb_len  = 0;
static int  tty_pb_head = 0;

static void tty_push_seq(const char* seq) {
    for (int i = 0; seq[i] && tty_pb_len < (int)sizeof(tty_pushback); i++) {
        int slot = (tty_pb_head + tty_pb_len) % (int)sizeof(tty_pushback);
        tty_pushback[slot] = seq[i];
        tty_pb_len++;
    }
}

static char tty_pb_pop() {
    char c = tty_pushback[tty_pb_head];
    tty_pb_head = (tty_pb_head + 1) % (int)sizeof(tty_pushback);
    tty_pb_len--;
    return c;
}

static kResult<i64> tty_read(VNode* /*node*/, u64 /*offset*/,
                              void* buf, u64 len) {
    if (!buf || len == 0) return kResult<i64>::err(kError::InvalidArgument);
    char* dst = reinterpret_cast<char*>(buf);
    u64   i   = 0;
    while (i < len) {
        while (tty_pb_len > 0 && i < len)
            dst[i++] = tty_pb_pop();
        if (i == len) break;
        keyboard::KeyEvent e = keyboard::read_event();
        if (!e.pressed) continue;
        if (e.ascii) {
            dst[i++] = e.ascii;
            if (e.ascii == '\n' || e.ascii == '\r') break;
            continue;
        }
        const char* seq = nullptr;
        switch (e.scancode) {
            case keyboard::KEY_UP:    seq = "\x1b[A";  break;
            case keyboard::KEY_DOWN:  seq = "\x1b[B";  break;
            case keyboard::KEY_RIGHT: seq = "\x1b[C";  break;
            case keyboard::KEY_LEFT:  seq = "\x1b[D";  break;
            case keyboard::KEY_HOME:  seq = "\x1b[H";  break;
            case keyboard::KEY_END:   seq = "\x1b[F";  break;
            case keyboard::KEY_DEL:   seq = "\x1b[3~"; break;
            case keyboard::KEY_PGUP:  seq = "\x1b[5~"; break;
            case keyboard::KEY_PGDN:  seq = "\x1b[6~"; break;
        }
        if (seq) tty_push_seq(seq);
    }
    return kResult<i64>::ok((i64)i);
}

static kResult<i64> tty_write(VNode* /*node*/, u64 /*offset*/,
                               const void* buf, u64 len) {
    const char* src = reinterpret_cast<const char*>(buf);
    for (u64 i = 0; i < len; i++) vga::put_char(src[i]);
    return kResult<i64>::ok((i64)len);
}

static FileOps tty_ops = {
    .read    = tty_read,
    .write   = tty_write,
    .close   = nullptr,
    .readdir = nullptr,
};

static VNode tty_node = {
    .name = "tty",
    .type = VFS_TYPE_FILE,
    .size = 0,
    .ops  = &tty_ops,
};

// ── Path utilities ────────────────────────────────────────────────────────
static const char* path_next(const char* path, char* component, usize max) {
    while (*path == '/') path++;
    if (!*path) { component[0] = 0; return path; }
    usize i = 0;
    while (*path && *path != '/' && i + 1 < max)
        component[i++] = *path++;
    component[i] = 0;
    return path;
}

// ── resolve (internal) ────────────────────────────────────────────────────
static VNode* resolve_internal(const char* path) {
    if (!path || !path[0]) return nullptr;
    VNode* node = (path[0] == '/') ? vfs_root : get_cwd();
    if (!node) node = vfs_root;

    const char* p = (path[0] == '/') ? path + 1 : path;
    char component[128];
    while (true) {
        p = path_next(p, component, sizeof(component));
        if (!component[0]) return node;
        if (kstreq(component, ".")) continue;
        if (kstreq(component, "..")) {
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

// ── Public API ────────────────────────────────────────────────────────────

kResult<VNode*> resolve(const char* path) {
    VNode* n = resolve_internal(path);
    if (!n) return kResult<VNode*>::err(kError::NotFound);
    return kResult<VNode*>::ok(n);
}

void build_path(VNode* node, char* buf, usize max) {
    if (!node || node == vfs_root) {
        buf[0] = '/'; buf[1] = 0;
        return;
    }
    const char* parts[32];
    int depth = 0;
    VNode* n = node;
    while (n && n != vfs_root && depth < 32) {
        parts[depth++] = n->name;
        n = n->parent;
    }
    usize pos = 0;
    for (int i = depth - 1; i >= 0 && pos < max - 2; i--) {
        buf[pos++] = '/';
        const char* part = parts[i];
        usize j = 0;
        while (part[j] && pos < max - 1) buf[pos++] = part[j++];
    }
    buf[pos] = 0;
}

static VNode* resolve_parent(const char* path, char* leaf_name, usize max) {
    usize len = kstrlen(path);
    usize last_slash = 0;
    bool  has_slash  = false;
    for (usize i = 0; i < len; i++) {
        if (path[i] == '/') { last_slash = i; has_slash = true; }
    }
    if (!has_slash) {
        kstrcpy(leaf_name, path, max);
        VNode* cwd = get_cwd();
        return cwd ? cwd : vfs_root;
    }
    kstrcpy(leaf_name, path + last_slash + 1, max);
    char parent_path[256];
    usize plen = (last_slash == 0) ? 1 : last_slash;
    for (usize i = 0; i < plen && i < 255; i++) parent_path[i] = path[i];
    parent_path[plen] = 0;
    return resolve_internal(parent_path);
}

// ── create (internal helper + public) ─────────────────────────────────────
static VNode* create_internal(const char* path, u32 type) {
    char   leaf[128];
    VNode* parent = resolve_parent(path, leaf, sizeof(leaf));
    if (!parent || parent->type != VFS_TYPE_DIR) return nullptr;

    VNode* child = parent->children;
    while (child) {
        if (kstreq(child->name, leaf)) return nullptr;
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

kResult<VNode*> create(const char* path, u32 type) {
    VNode* n = create_internal(path, type);
    if (!n) return kResult<VNode*>::err(kError::IOError);
    return kResult<VNode*>::ok(n);
}

// ── Pipe ──────────────────────────────────────────────────────────────────
static constexpr u64 PIPE_BUF_SIZE = 4096;

struct PipeBuffer {
    u8   buf[PIPE_BUF_SIZE];
    u64  read_pos;
    u64  write_pos;
    u64  used;
    u32  refcount;
};

static kResult<i64> pipe_read(VNode* node, u64 /*offset*/, void* buf, u64 len) {
    PipeBuffer* pb = reinterpret_cast<PipeBuffer*>(node->data);
    if (!pb) return kResult<i64>::err(kError::InvalidArgument);
    if (pb->used == 0) return kResult<i64>::ok(0);
    u64 to_read = len < pb->used ? len : pb->used;
    u8* dst = reinterpret_cast<u8*>(buf);
    for (u64 i = 0; i < to_read; i++) {
        dst[i] = pb->buf[pb->read_pos];
        pb->read_pos = (pb->read_pos + 1) % PIPE_BUF_SIZE;
    }
    pb->used -= to_read;
    return kResult<i64>::ok((i64)to_read);
}

static kResult<i64> pipe_write(VNode* node, u64 /*offset*/, const void* buf, u64 len) {
    PipeBuffer* pb = reinterpret_cast<PipeBuffer*>(node->data);
    if (!pb) return kResult<i64>::err(kError::InvalidArgument);
    u64 space    = PIPE_BUF_SIZE - pb->used;
    u64 to_write = len < space ? len : space;
    if (to_write == 0) return kResult<i64>::err(kError::NoSpace);
    const u8* src = reinterpret_cast<const u8*>(buf);
    for (u64 i = 0; i < to_write; i++) {
        pb->buf[pb->write_pos] = src[i];
        pb->write_pos = (pb->write_pos + 1) % PIPE_BUF_SIZE;
    }
    pb->used += to_write;
    return kResult<i64>::ok((i64)to_write);
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

kStatus pipe(FileDescriptor** read_out, FileDescriptor** write_out) {
    if (!read_out || !write_out) return kStatus::err(kError::InvalidArgument);

    PipeBuffer* pb = reinterpret_cast<PipeBuffer*>(heap::kmalloc(sizeof(PipeBuffer)));
    if (!pb) return kStatus::err(kError::OutOfMemory);
    for (usize i = 0; i < sizeof(PipeBuffer); i++)
        reinterpret_cast<u8*>(pb)[i] = 0;
    pb->refcount = 2;

    VNode* node = alloc_vnode("[pipe]", VFS_TYPE_FILE);
    if (!node) { heap::kfree(pb); return kStatus::err(kError::OutOfMemory); }
    node->ops      = &pipe_ops;
    node->data     = pb;
    node->refcount = 2;

    FileDescriptor* rfd = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    FileDescriptor* wfd = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    if (!rfd || !wfd) {
        if (rfd) heap::kfree(rfd);
        if (wfd) heap::kfree(wfd);
        heap::kfree(node);
        heap::kfree(pb);
        return kStatus::err(kError::OutOfMemory);
    }

    rfd->node = node; rfd->offset = 0; rfd->flags = VFS_READ;  rfd->dir_index = 0;
    wfd->node = node; wfd->offset = 0; wfd->flags = VFS_WRITE; wfd->dir_index = 0;

    *read_out  = rfd;
    *write_out = wfd;
    return kStatus::ok();
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    vfs_root      = alloc_vnode("/", VFS_TYPE_DIR);
    vfs_root->ops = &ramfs_ops;

    create_internal("/dev", VFS_TYPE_DIR);
    create_internal("/tmp", VFS_TYPE_DIR);
    create_internal("/bin", VFS_TYPE_DIR);

    VNode* dev        = resolve_internal("/dev");
    tty_node.parent       = dev;
    tty_node.next_sibling = dev->children;
    dev->children         = &tty_node;

    serial::write_line("[VFS] ramfs mounted at /");
}

// ── open ──────────────────────────────────────────────────────────────────
kResult<FileDescriptor*> open(const char* path, u64 flags) {
    VNode* node = resolve_internal(path);
    if (!node) {
        if (!(flags & O_CREATE))
            return kResult<FileDescriptor*>::err(kError::NotFound);
        node = create_internal(path, VFS_TYPE_FILE);
        if (!node)
            return kResult<FileDescriptor*>::err(kError::IOError);
    }

    if ((flags & O_TRUNC) && node->type == VFS_TYPE_FILE) {
        if (node->data) { heap::kfree(node->data); node->data = nullptr; }
        node->size          = 0;
        node->data_capacity = 0;
    }

    FileDescriptor* fd = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    if (!fd) return kResult<FileDescriptor*>::err(kError::OutOfMemory);

    u64 accmode = flags & 3;
    u64 iflags  = flags;
    if (accmode == O_RDWR)       iflags |= VFS_READ | VFS_WRITE;
    else if (accmode == O_WRITE) iflags |= VFS_WRITE;
    else                         iflags |= VFS_READ;
    if (flags & O_APPEND) iflags |= VFS_APPEND;

    fd->node      = node;
    fd->flags     = iflags;
    fd->dir_index = 0;
    fd->offset    = (iflags & VFS_APPEND) ? node->size : 0;
    node->refcount++;

    return kResult<FileDescriptor*>::ok(fd);
}

// ── close ─────────────────────────────────────────────────────────────────
void close(FileDescriptor* fd) {
    if (!fd) return;
    fd->node->refcount--;
    if (fd->node->refcount == 0 && fd->node->ops && fd->node->ops->close)
        fd->node->ops->close(fd->node);
    heap::kfree(fd);
}

// ── read ──────────────────────────────────────────────────────────────────
kResult<i64> read(FileDescriptor* fd, void* buf, u64 len) {
    if (!fd || !buf)                              return kResult<i64>::err(kError::InvalidArgument);
    if (!(fd->flags & VFS_READ))                  return kResult<i64>::err(kError::PermissionDenied);
    if (fd->node->type == VFS_TYPE_DIR)           return kResult<i64>::err(kError::IsDirectory);
    if (!fd->node->ops || !fd->node->ops->read)   return kResult<i64>::err(kError::IOError);
    auto res = fd->node->ops->read(fd->node, fd->offset, buf, len);
    if (res.is_ok() && res.value() > 0) fd->offset += (u64)res.value();
    return res;
}

// ── write ─────────────────────────────────────────────────────────────────
kResult<i64> write(FileDescriptor* fd, const void* buf, u64 len) {
    if (!fd || !buf)                              return kResult<i64>::err(kError::InvalidArgument);
    if (!(fd->flags & VFS_WRITE))                 return kResult<i64>::err(kError::PermissionDenied);
    if (fd->node->type == VFS_TYPE_DIR)           return kResult<i64>::err(kError::IsDirectory);
    if (!fd->node->ops || !fd->node->ops->write)  return kResult<i64>::err(kError::IOError);
    if (fd->flags & VFS_APPEND) fd->offset = fd->node->size;
    auto res = fd->node->ops->write(fd->node, fd->offset, buf, len);
    if (res.is_ok() && res.value() > 0) fd->offset += (u64)res.value();
    return res;
}

// ── seek ──────────────────────────────────────────────────────────────────
kResult<i64> seek(FileDescriptor* fd, i64 offset, u64 whence) {
    if (!fd)                            return kResult<i64>::err(kError::BadFileDescriptor);
    if (fd->node->type == VFS_TYPE_DIR) return kResult<i64>::err(kError::IsDirectory);
    i64 base = 0;
    switch (whence) {
        case SEEK_SET: base = 0;                       break;
        case SEEK_CUR: base = (i64)fd->offset;         break;
        case SEEK_END: base = (i64)fd->node->size;     break;
        default: return kResult<i64>::err(kError::InvalidArgument);
    }
    i64 newoff = base + offset;
    if (newoff < 0) return kResult<i64>::err(kError::InvalidArgument);
    fd->offset = (u64)newoff;
    return kResult<i64>::ok(newoff);
}

// ── stat ──────────────────────────────────────────────────────────────────
kStatus stat(const char* path, StatBuf* out) {
    if (!out) return kStatus::err(kError::InvalidArgument);
    VNode* node = resolve_internal(path);
    if (!node) return kStatus::err(kError::NotFound);
    for (usize i = 0; i < sizeof(StatBuf); i++)
        reinterpret_cast<u8*>(out)[i] = 0;
    out->st_size    = (i64)node->size;
    out->st_mode    = (node->type == VFS_TYPE_DIR)
                    ? (S_IFDIR | S_IRWXU | S_IRWXG | S_IRWXO)
                    : (S_IFREG | S_IRWXU | S_IRWXG | S_IRWXO);
    out->st_blksize = 512;
    out->st_blocks  = (i64)((node->size + 511) / 512);
    return kStatus::ok();
}

// ── dup ───────────────────────────────────────────────────────────────────
kResult<FileDescriptor*> dup(FileDescriptor* fd) {
    if (!fd) return kResult<FileDescriptor*>::err(kError::InvalidArgument);
    FileDescriptor* copy = reinterpret_cast<FileDescriptor*>(
        heap::kmalloc(sizeof(FileDescriptor)));
    if (!copy) return kResult<FileDescriptor*>::err(kError::OutOfMemory);
    copy->node      = fd->node;
    copy->offset    = fd->offset;
    copy->flags     = fd->flags;
    copy->dir_index = fd->dir_index;
    fd->node->refcount++;
    return kResult<FileDescriptor*>::ok(copy);
}

// ── mkdir ─────────────────────────────────────────────────────────────────
kStatus mkdir(const char* path) {
    if (resolve_internal(path)) return kStatus::err(kError::AlreadyExists);
    VNode* node = create_internal(path, VFS_TYPE_DIR);
    if (!node) return kStatus::err(kError::NotFound);
    return kStatus::ok();
}

// ── unlink ────────────────────────────────────────────────────────────────
kStatus unlink(const char* path) {
    VNode* node = resolve_internal(path);
    if (!node)                      return kStatus::err(kError::NotFound);
    if (node->type == VFS_TYPE_DIR) return kStatus::err(kError::IsDirectory);
    if (node->refcount > 0)         return kStatus::err(kError::Busy);

    VNode* parent = node->parent;
    if (!parent)                    return kStatus::err(kError::InvalidArgument);

    if (parent->ops && parent->ops->unlink) {
        auto r = parent->ops->unlink(parent, node->name);
        if (r.is_err()) return r;
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
    return kStatus::ok();
}

// ── cwd ───────────────────────────────────────────────────────────────────
VNode* get_cwd() {
    sched::Task* t = sched::current();
    if (!t || !t->cwd) return vfs_root;
    return t->cwd;
}

void set_cwd(VNode* node) {
    sched::Task* t = sched::current();
    if (t) t->cwd = node;
}

// ── readdir ───────────────────────────────────────────────────────────────
kStatus readdir(FileDescriptor* fd, Dirent* out) {
    if (!fd || !out)                                  return kStatus::err(kError::InvalidArgument);
    if (fd->node->type != VFS_TYPE_DIR)               return kStatus::err(kError::NotADirectory);
    if (!fd->node->ops || !fd->node->ops->readdir)    return kStatus::err(kError::IOError);
    auto res = fd->node->ops->readdir(fd->node, fd->dir_index, out);
    if (res.is_ok()) { fd->dir_index++; return kStatus::ok(); }
    return res;
}

// ── mount ─────────────────────────────────────────────────────────────────
kStatus mount(const char* path, VNode* fs_root) {
    VNode* mount_point = resolve_internal(path);
    if (!mount_point)                        return kStatus::err(kError::NotFound);
    if (mount_point->type != VFS_TYPE_DIR)   return kStatus::err(kError::NotADirectory);

    Mount* m = reinterpret_cast<Mount*>(heap::kmalloc(sizeof(Mount)));
    if (!m) return kStatus::err(kError::OutOfMemory);
    kstrcpy(m->path, path, sizeof(m->path));
    m->root = fs_root;
    m->next = mount_list;
    mount_list = m;

    mount_point->children      = fs_root->children;
    mount_point->ops           = fs_root->ops;
    mount_point->priv          = fs_root->priv;
    mount_point->data          = fs_root->data;
    mount_point->data_capacity = fs_root->data_capacity;
    mount_point->size          = fs_root->size;
    mount_point->type          = fs_root->type;

    fs_root->priv = nullptr;
    fs_root->data = nullptr;

    VNode* child = mount_point->children;
    while (child) { child->parent = mount_point; child = child->next_sibling; }

    heap::kfree(fs_root);

    serial::write("[VFS] mounted at ");
    serial::write_line(path);
    return kStatus::ok();
}

} // namespace vfs
