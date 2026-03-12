// kernel/fdtable.cpp - Per-task file descriptor table
#include "fdtable.hpp"
#include "heap.hpp"

namespace fdtable {

FDTable* alloc() {
    FDTable* t = reinterpret_cast<FDTable*>(heap::kmalloc(sizeof(FDTable)));
    if (!t) return nullptr;
    for (uint32_t i = 0; i < MAX_FDS; i++) t->fds[i] = nullptr;
    t->count = 0;
    return t;
}

void free(FDTable* table) {
    if (!table) return;
    for (uint32_t i = 0; i < MAX_FDS; i++) {
        if (table->fds[i]) {
            vfs::close(table->fds[i]);
            table->fds[i] = nullptr;
        }
    }
    heap::kfree(table);
}

int32_t install(FDTable* table, vfs::FileDescriptor* file) {
    if (!table || !file) return -1;
    for (uint32_t i = 2; i < MAX_FDS; i++) { // optimization starting at 2, since fds[0..2] is just stdin/out/err
        if (!table->fds[i]) {
            table->fds[i] = file;
            table->count++;
            return (int32_t)i;
        }
    }
    return -1; // table full
}

int32_t install_at(FDTable* table, vfs::FileDescriptor* file, int32_t fd) {
    if (!table || !file) return -1;
    if (fd < 0) return -1;
    uint32_t idx = (uint32_t)fd;
    if (idx >= MAX_FDS) return -1;
    if (table->fds[idx]) return -1; // already occupied, caller must remove first
    table->fds[idx] = file;
    table->count++;
    return fd;
}

vfs::FileDescriptor* lookup(FDTable* table, uint64_t fd) {
    if (!table) return nullptr;
    if (fd < 0) return nullptr;
    uint64_t idx = fd;
    if (idx >= MAX_FDS) return nullptr;
    return table->fds[idx];
}

vfs::FileDescriptor* remove(FDTable* table, uint64_t fd) {
    if (!table) return nullptr;
    if (fd < 0) return nullptr;
    uint64_t idx = fd;
    if (idx >= MAX_FDS) return nullptr;
    vfs::FileDescriptor* f = table->fds[idx];
    if (f) { table->fds[idx] = nullptr; table->count--; }
    return f;
}

FDTable* clone(FDTable* src) {
    if (!src) return nullptr;
    FDTable* dst = alloc();
    if (!dst) return nullptr;
    for (uint32_t i = 0; i < MAX_FDS; i++) {
        if (src->fds[i]) {
            dst->fds[i] = vfs::dup(src->fds[i]);
            if (!dst->fds[i]) {
                // dup failed — free what we've built so far and bail
                free(dst);
                return nullptr;
            }
            dst->count++;
        }
    }
    return dst;
}

} // namespace fdtable
