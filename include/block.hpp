#pragma once
// include/block.hpp - Knail block device abstraction
#include <stdint.h>
#include <stddef.h>
#include "syscall.hpp"

namespace block {

static constexpr uint64_t SECTOR_SIZE = 512;

// ── Block device vtable ───────────────────────────────────────────────────
struct BlockDevice {
    // Read count sectors starting at lba into buf.
    // Returns E_OK or negative errno.
    int64_t (*read) (BlockDevice* dev, uint64_t lba,
                     uint32_t count, void* buf);
    // Write count sectors from buf starting at lba.
    // Returns E_OK or negative errno.
    int64_t (*write)(BlockDevice* dev, uint64_t lba,
                     uint32_t count, const void* buf);

    uint64_t sector_count;  // total sectors on device
    uint32_t sector_size;   // bytes per sector (always 512 for ATA)
    char     name[16];      // e.g. "hda", "hdb"
    void*    priv;          // driver-private data (ATA: drive index)
};

// ── Registry ──────────────────────────────────────────────────────────────
static constexpr uint32_t MAX_DEVICES = 4;

void         init();
bool         register_device(BlockDevice* dev);
BlockDevice* get(uint32_t index);      // 0-based
BlockDevice* find(const char* name);   // by name
uint32_t     count();

} // namespace block
