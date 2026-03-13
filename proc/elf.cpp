// kernel/elf.cpp - ELF64 parser
#include "elf.hpp"
#include "vfs.hpp"
#include "heap.hpp"
#include "serial.hpp"
#include "types.hpp"

namespace elf {

kResult<Image> load(const char* path) {
    // ── Open file ─────────────────────────────────────────────────────────
    auto fd_res = vfs::open(path, O_READ);
    if (fd_res.is_err()) {
        serial::write("[ELF] cannot open: ");
        serial::write_line(path);
        return kResult<Image>::err(kError::NotFound);
    }
    vfs::FileDescriptor* fd = fd_res.value();

    u64 size = fd->node->size;
    if (size < 64) {
        serial::write_line("[ELF] file too small");
        vfs::close(fd);
        return kResult<Image>::err(kError::FilesystemCorrupt);
    }

    serial::write("[ELF] opening ");
    serial::write(path);
    serial::write(" size=");
    serial::write_dec(size);
    serial::write_line("");

    // ── Read entire file into heap buffer ─────────────────────────────────
    u8* buf = reinterpret_cast<u8*>(heap::kmalloc(size));
    if (!buf) {
        serial::write_line("[ELF] out of memory");
        vfs::close(fd);
        return kResult<Image>::err(kError::OutOfMemory);
    }

    u64 total = 0;
    while (total < size) {
        auto res = vfs::read(fd, buf + total, size - total);
        if (res.is_err()) {
            serial::write("[ELF] read error total=");
            serial::write_dec(total);
            serial::write(" size=");
            serial::write_dec(size);
            serial::write_line("");
            heap::kfree(buf);
            vfs::close(fd);
            return kResult<Image>::err(res.error());
        }
        if (res.value() == 0) break; // EOF before expected size
        total += (u64)res.value();
    }
    vfs::close(fd);

    if (total != size) {
        serial::write("[ELF] short read: got ");
        serial::write_dec(total);
        serial::write(" expected ");
        serial::write_dec(size);
        serial::write_line("");
        heap::kfree(buf);
        return kResult<Image>::err(kError::IOError);
    }

    // ── Validate ELF header ───────────────────────────────────────────────
    Elf64Header* eh = reinterpret_cast<Elf64Header*>(buf);

    auto fail = [&](const char* msg) -> kResult<Image> {
        serial::write_line(msg);
        heap::kfree(buf);
        return kResult<Image>::err(kError::FilesystemCorrupt);
    };

    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L'  || eh->ident[3] != 'F')
        return fail("[ELF] bad magic");
    if (eh->ident[4] != 2)
        return fail("[ELF] not 64-bit");
    if (eh->ident[5] != 1)
        return fail("[ELF] not little-endian");
    if (eh->type != ET_EXEC && eh->type != ET_DYN)
        return fail("[ELF] not executable");
    if (eh->machine != EM_X86_64)
        return fail("[ELF] not x86-64");
    if (eh->phentsize != sizeof(Elf64Phdr))
        return fail("[ELF] bad phentsize");
    if (eh->phnum == 0 || eh->phnum > 64)
        return fail("[ELF] bad phnum");
    if (eh->phoff + eh->phnum * sizeof(Elf64Phdr) > size)
        return fail("[ELF] phdrs outside file");

    // ── Walk program headers, collect PT_LOAD segments ────────────────────
    Image img;
    img.file_data     = buf;
    img.file_size     = size;
    img.entry         = eh->entry;
    img.segment_count = 0;

    Elf64Phdr* phdrs = reinterpret_cast<Elf64Phdr*>(buf + eh->phoff);
    for (u16 i = 0; i < eh->phnum; i++) {
        Elf64Phdr& ph = phdrs[i];

        if (ph.type == PT_INTERP) {
            serial::write_line("[ELF] warning: dynamic binary (PT_INTERP) — "
                               "dynamic linking not supported, load may fail");
        }
        if (ph.type != PT_LOAD) continue;

        if (ph.offset + ph.filesz > size)  return fail("[ELF] segment outside file");
        if (ph.memsz < ph.filesz)          return fail("[ELF] memsz < filesz");
        if (img.segment_count >= MAX_SEGMENTS) return fail("[ELF] too many segments");

        Segment& seg  = img.segments[img.segment_count++];
        seg.vaddr     = ph.vaddr;
        seg.filesz    = ph.filesz;
        seg.memsz     = ph.memsz;
        seg.flags     = ph.flags;
        seg.data      = buf + ph.offset;
    }

    if (img.segment_count == 0) return fail("[ELF] no loadable segments");

    serial::write("[ELF] loaded ");
    serial::write(path);
    serial::write(" entry=");
    serial::write_hex(img.entry);
    serial::write(" segments=");
    serial::write_dec(img.segment_count);
    serial::write_line("");

    return kResult<Image>::ok(img);
}

void free(Image& image) {
    if (image.file_data) {
        heap::kfree(image.file_data);
        image.file_data = nullptr;
    }
}

} // namespace elf
