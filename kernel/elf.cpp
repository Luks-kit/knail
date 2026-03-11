// kernel/elf.cpp - ELF64 parser
#include "elf.hpp"
#include "vfs.hpp"
#include "heap.hpp"
#include "serial.hpp"
#include "syscall.hpp"

namespace elf {

static bool kstreq(const char* a, const char* b) {
    while (*a && *b) { if (*a != *b) return false; a++; b++; }
    return *a == *b;
}

Image load(const char* path) {
    Image img;
    img.valid      = false;
    img.file_data  = nullptr;
    img.file_size  = 0;
    img.entry      = 0;
    img.segment_count = 0;

    // ── Open file ─────────────────────────────────────────────────────────
    vfs::FileDescriptor* fd = vfs::open(path, O_READ);
    if (!fd) {
        serial::write("[ELF] cannot open: ");
        serial::write_line(path);
        return img;
    }

    uint64_t size = fd->node->size;
    if (size < 64) {
        serial::write_line("[ELF] file too small");
        vfs::close(fd);
        return img;
    }
    serial::write("[ELF] opening ");
    serial::write(path);
    serial::write(" size=");
    serial::write_dec(size);
    serial::write_line("");

    // ── Read entire file into heap buffer ─────────────────────────────────
    uint8_t* buf = reinterpret_cast<uint8_t*>(heap::kmalloc(size));
    if (!buf) {
        serial::write_line("[ELF] out of memory");
        vfs::close(fd);
        return img;
    }

    uint64_t total = 0;
    while (total < size) {
        int64_t n = vfs::read(fd, buf + total, size - total);
        if (n < 0) {
            serial::write("[ELF] read error n=");
            serial::write_int(n);
            serial::write(" total=");
            serial::write_dec(total);
            serial::write(" size=");
            serial::write_dec(size);
            serial::write_line("");
            heap::kfree(buf);
            vfs::close(fd);
            return img;
        }
        if (n == 0) break; // EOF before expected size
        total += (uint64_t)n;
    }

    vfs::close(fd);

    if (total != size) {
        serial::write("[ELF] short read: got ");
        serial::write_dec(total);
        serial::write(" expected ");
        serial::write_dec(size);
        serial::write_line("");
        heap::kfree(buf);
        return img;
    }
    img.file_data = buf;
    img.file_size = size;

    // ── Validate ELF header ───────────────────────────────────────────────
    Elf64Header* eh = reinterpret_cast<Elf64Header*>(buf);

    // Magic: 0x7F 'E' 'L' 'F'
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' ||
        eh->ident[2] != 'L'  || eh->ident[3] != 'F') {
        serial::write_line("[ELF] bad magic");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }
    // Class: 64-bit
    if (eh->ident[4] != 2) {
        serial::write_line("[ELF] not 64-bit");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }
    // Data: little-endian
    if (eh->ident[5] != 1) {
        serial::write_line("[ELF] not little-endian");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }
    // Type: ET_EXEC or ET_DYN
    if (eh->type != ET_EXEC && eh->type != ET_DYN) {
        serial::write_line("[ELF] not executable");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }
    // Machine: x86-64
    if (eh->machine != EM_X86_64) {
        serial::write_line("[ELF] not x86-64");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }
    // Program header sanity
    if (eh->phentsize != sizeof(Elf64Phdr)) {
        serial::write_line("[ELF] bad phentsize");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }
    if (eh->phnum == 0 || eh->phnum > 64) {
        serial::write_line("[ELF] bad phnum");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }
    if (eh->phoff + eh->phnum * sizeof(Elf64Phdr) > size) {
        serial::write_line("[ELF] phdrs outside file");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }

    img.entry = eh->entry;

    // ── Walk program headers, collect PT_LOAD segments ────────────────────
    Elf64Phdr* phdrs = reinterpret_cast<Elf64Phdr*>(buf + eh->phoff);

    for (uint16_t i = 0; i < eh->phnum; i++) {
        Elf64Phdr& ph = phdrs[i];

        // Warn on dynamic linking — we only support static for now
        if (ph.type == PT_INTERP) {
            serial::write_line("[ELF] warning: dynamic binary (PT_INTERP) — "
                               "dynamic linking not supported, load may fail");
        }

        if (ph.type != PT_LOAD) continue;

        // Validate segment
        if (ph.offset + ph.filesz > size) {
            serial::write_line("[ELF] segment outside file");
            heap::kfree(buf);
            img.file_data = nullptr;
            img.valid     = false;
            return img;
        }
        if (ph.memsz < ph.filesz) {
            serial::write_line("[ELF] memsz < filesz");
            heap::kfree(buf);
            img.file_data = nullptr;
            img.valid     = false;
            return img;
        }
        if (img.segment_count >= MAX_SEGMENTS) {
            serial::write_line("[ELF] too many segments");
            heap::kfree(buf);
            img.file_data = nullptr;
            img.valid     = false;
            return img;
        }

        Segment& seg   = img.segments[img.segment_count++];
        seg.vaddr      = ph.vaddr;
        seg.filesz     = ph.filesz;
        seg.memsz      = ph.memsz;
        seg.flags      = ph.flags;
        seg.data       = buf + ph.offset;
    }

    if (img.segment_count == 0) {
        serial::write_line("[ELF] no loadable segments");
        heap::kfree(buf);
        img.file_data = nullptr;
        return img;
    }

    serial::write("[ELF] loaded ");
    serial::write(path);
    serial::write(" entry=");
    serial::write_hex(img.entry);
    serial::write(" segments=");
    serial::write_dec(img.segment_count);
    serial::write_line("");

    img.valid = true;
    return img;
}

void free(Image& image) {
    if (image.file_data) {
        heap::kfree(image.file_data);
        image.file_data = nullptr;
    }
    image.valid = false;
}

} // namespace elf
