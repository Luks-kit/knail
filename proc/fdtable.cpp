// kernel/fdtable.cpp - Per-task file descriptor table
#include "fdtable.hpp"
#include "heap.hpp"
#include "types.hpp"

namespace fdtable {

FDTable* alloc() {
    FDTable* t = reinterpret_cast<FDTable*>(heap::kmalloc(sizeof(FDTable)));
    if (!t) return nullptr;
    for (u32 i = 0; i < MAX_FDS; i++) t->fds[i] = nullptr;
    t->count = 0;
    return t;
}

void free(FDTable* table) {
    if (!table) return;
    for (u32 i = 0; i < MAX_FDS; i++) {
        if (table->fds[i]) {
            vfs::close(table->fds[i]);
            table->fds[i] = nullptr;
        }
    }
    heap::kfree(table);
}

kResult<i32> install(FDTable* table, vfs::FileDescriptor* file) {
    if (!table || !file) return kResult<i32>::err(kError::InvalidArgument);
    // Start at 3 — slots 0/1/2 are stdin/stdout/stderr
    for (u32 i = 3; i < MAX_FDS; i++) {
        if (!table->fds[i]) {
            table->fds[i] = file;
            table->count++;
            return kResult<i32>::ok((i32)i);
        }
    }
    return kResult<i32>::err(kError::OutOfFileDescriptors);
}

kResult<i32> install_at(FDTable* table, vfs::FileDescriptor* file, i32 fd) {
    if (!table || !file)    return kResult<i32>::err(kError::InvalidArgument);
    if (fd < 0)             return kResult<i32>::err(kError::InvalidArgument);
    u32 idx = (u32)fd;
    if (idx >= MAX_FDS)     return kResult<i32>::err(kError::InvalidArgument);
    if (table->fds[idx])    return kResult<i32>::err(kError::AlreadyExists);
    table->fds[idx] = file;
    table->count++;
    return kResult<i32>::ok(fd);
}

kResult<vfs::FileDescriptor*> lookup(FDTable* table, u64 fd) {
    if (!table)          return kResult<vfs::FileDescriptor*>::err(kError::InvalidArgument);
    if (fd >= MAX_FDS)   return kResult<vfs::FileDescriptor*>::err(kError::BadFileDescriptor);
    vfs::FileDescriptor* f = table->fds[fd];
    if (!f)              return kResult<vfs::FileDescriptor*>::err(kError::BadFileDescriptor);
    return kResult<vfs::FileDescriptor*>::ok(f);
}

kResult<vfs::FileDescriptor*> remove(FDTable* table, u64 fd) {
    if (!table)          return kResult<vfs::FileDescriptor*>::err(kError::InvalidArgument);
    if (fd >= MAX_FDS)   return kResult<vfs::FileDescriptor*>::err(kError::BadFileDescriptor);
    vfs::FileDescriptor* f = table->fds[fd];
    if (!f)              return kResult<vfs::FileDescriptor*>::err(kError::BadFileDescriptor);
    table->fds[fd] = nullptr;
    table->count--;
    return kResult<vfs::FileDescriptor*>::ok(f);
}

FDTable* clone(FDTable* src) {
    if (!src) return nullptr;
    FDTable* dst = alloc();
    if (!dst) return nullptr;
    for (u32 i = 0; i < MAX_FDS; i++) {
        if (!src->fds[i]) continue;
        auto res = vfs::dup(src->fds[i]);
        if (res.is_err()) {
            free(dst);
            return nullptr;
        }
        dst->fds[i] = res.value();
        dst->count++;
    }
    return dst;
}

} // namespace fdtable
