#pragma once
// include/elf.hpp - ELF64 parser
// Validates and extracts loadable segments from an ELF64 executable.
// Does not touch VMM, scheduler, or heap beyond its own Image struct.
#include <stdint.h>
#include <stddef.h>

namespace elf {

// ── ELF64 header ─────────────────────────────────────────────────────────
struct [[gnu::packed]] Elf64Header {
    uint8_t  ident[16];   // magic, class, data, version, OS/ABI
    uint16_t type;        // ET_EXEC=2, ET_DYN=3
    uint16_t machine;     // EM_X86_64=0x3E
    uint32_t version;
    uint64_t entry;       // virtual entry point
    uint64_t phoff;       // program header table offset
    uint64_t shoff;       // section header table offset (unused)
    uint32_t flags;
    uint16_t ehsize;      // size of this header (64)
    uint16_t phentsize;   // size of one program header entry (56)
    uint16_t phnum;       // number of program header entries
    uint16_t shentsize;
    uint16_t shnum;
    uint16_t shstrndx;
};

// ── ELF64 program header ──────────────────────────────────────────────────
struct [[gnu::packed]] Elf64Phdr {
    uint32_t type;    // PT_LOAD=1, PT_INTERP=3, PT_PHDR=6
    uint32_t flags;   // PF_X=1, PF_W=2, PF_R=4
    uint64_t offset;  // offset in file
    uint64_t vaddr;   // virtual address to load at
    uint64_t paddr;   // physical address (ignored)
    uint64_t filesz;  // bytes in file
    uint64_t memsz;   // bytes in memory (>= filesz, zero-fill remainder)
    uint64_t align;   // alignment (power of 2)
};

// ── Segment flags (from Elf64Phdr::flags) ────────────────────────────────
static constexpr uint32_t PF_X = 1; // execute
static constexpr uint32_t PF_W = 2; // write
static constexpr uint32_t PF_R = 4; // read

// ── Program header types ──────────────────────────────────────────────────
static constexpr uint32_t PT_LOAD    = 1;
static constexpr uint32_t PT_DYNAMIC = 2;
static constexpr uint32_t PT_INTERP  = 3;
static constexpr uint32_t PT_PHDR    = 6;

// ── ELF types ─────────────────────────────────────────────────────────────
static constexpr uint16_t ET_EXEC = 2;
static constexpr uint16_t ET_DYN  = 3;
static constexpr uint16_t EM_X86_64 = 0x3E;

// ── Max PT_LOAD segments we'll handle ────────────────────────────────────
static constexpr uint32_t MAX_SEGMENTS = 16;

// ── A parsed, validated ELF image ready for loading ──────────────────────
struct Segment {
    uint64_t vaddr;   // virtual address to map at
    uint64_t filesz;  // bytes to copy from data
    uint64_t memsz;   // total bytes to map (zero-fill filesz..memsz)
    uint32_t flags;   // PF_R | PF_W | PF_X
    uint8_t* data;    // pointer into file_data buffer
};

struct Image {
    uint64_t entry;                  // virtual entry point
    uint32_t segment_count;
    Segment  segments[MAX_SEGMENTS];
    uint8_t* file_data;              // heap buffer holding the whole file
    uint64_t file_size;
    bool     valid;
};

// ── API ───────────────────────────────────────────────────────────────────

// Load and validate an ELF64 executable from the VFS.
// Returns an Image with valid=true on success.
// On failure, valid=false and file_data is null.
// Caller must call elf::free(image) when done (after spawn has mapped it).
Image load(const char* path);

// Free the file buffer held by an Image.
void free(Image& image);

} // namespace elf
