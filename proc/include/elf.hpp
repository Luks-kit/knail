#pragma once
// include/elf.hpp - ELF64 parser
// Validates and extracts loadable segments from an ELF64 executable.
// Does not touch VMM, scheduler, or heap beyond its own Image struct.
#include <stdint.h>
#include <stddef.h>
#include "types.hpp"

namespace elf {

// ── ELF64 header ─────────────────────────────────────────────────────────
struct [[gnu::packed]] Elf64Header {
    u8  ident[16];   // magic, class, data, version, OS/ABI
    u16 type;        // ET_EXEC=2, ET_DYN=3
    u16 machine;     // EM_X86_64=0x3E
    u32 version;
    u64 entry;       // virtual entry point
    u64 phoff;       // program header table offset
    u64 shoff;       // section header table offset (unused)
    u32 flags;
    u16 ehsize;      // size of this header (64)
    u16 phentsize;   // size of one program header entry (56)
    u16 phnum;       // number of program header entries
    u16 shentsize;
    u16 shnum;
    u16 shstrndx;
};

// ── ELF64 program header ──────────────────────────────────────────────────
struct [[gnu::packed]] Elf64Phdr {
    u32 type;    // PT_LOAD=1, PT_INTERP=3, PT_PHDR=6
    u32 flags;   // PF_X=1, PF_W=2, PF_R=4
    u64 offset;  // offset in file
    u64 vaddr;   // virtual address to load at
    u64 paddr;   // physical address (ignored)
    u64 filesz;  // bytes in file
    u64 memsz;   // bytes in memory (>= filesz, zero-fill remainder)
    u64 align;   // alignment (power of 2)
};

// ── Segment flags ─────────────────────────────────────────────────────────
static constexpr u32 PF_X = 1;
static constexpr u32 PF_W = 2;
static constexpr u32 PF_R = 4;

// ── Program header types ──────────────────────────────────────────────────
static constexpr u32 PT_LOAD    = 1;
static constexpr u32 PT_DYNAMIC = 2;
static constexpr u32 PT_INTERP  = 3;
static constexpr u32 PT_PHDR    = 6;

// ── ELF types ─────────────────────────────────────────────────────────────
static constexpr u16 ET_EXEC    = 2;
static constexpr u16 ET_DYN     = 3;
static constexpr u16 EM_X86_64  = 0x3E;

// ── Max PT_LOAD segments ──────────────────────────────────────────────────
static constexpr u32 MAX_SEGMENTS = 16;

// ── Parsed, validated ELF image ──────────────────────────────────────────
struct Segment {
    u64  vaddr;   // virtual address to map at
    u64  filesz;  // bytes to copy from data
    u64  memsz;   // total bytes to map (zero-fill filesz..memsz)
    u32  flags;   // PF_R | PF_W | PF_X
    u8*  data;    // pointer into file_data buffer
};

struct Image {
    u64     entry;                    // virtual entry point
    u32     segment_count;
    Segment segments[MAX_SEGMENTS];
    u8*     file_data;                // heap buffer holding the whole file
    u64     file_size;
    // NOTE: no 'valid' field — callers check kResult::is_ok() instead
};

// ── API ───────────────────────────────────────────────────────────────────
// Load and validate an ELF64 executable from the VFS.
// Returns kResult<Image> — check is_ok() before using the image.
// Caller must call elf::free(image) when done.
kResult<Image> load(const char* path);

// Free the file buffer held by an Image.
void free(Image& image);

} // namespace elf
