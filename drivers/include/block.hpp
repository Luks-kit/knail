#pragma once
// include/block.hpp - Knail block device abstraction
#include <stdint.h>
#include <stddef.h>
#include "types.hpp"

namespace block {

static constexpr u64 SECTOR_SIZE = 512;

// ── Block device vtable ───────────────────────────────────────────────────
struct BlockDevice {
    // Read count sectors starting at lba into buf.
    // Returns sector count read on success, kError on failure.
    kResult<i64> (*read) (BlockDevice* dev, u64 lba, u32 count, void* buf);
    // Write count sectors from buf starting at lba.
    // Returns kStatus.
    kStatus      (*write)(BlockDevice* dev, u64 lba, u32 count, const void* buf);

    u64  sector_count;   // total sectors on device
    u32  sector_size;    // bytes per sector (always 512 for ATA)
    char name[16];       // e.g. "hda", "hdb"
    void* priv;          // driver-private data (ATA: drive index)
};

// ── Registry ──────────────────────────────────────────────────────────────
static constexpr u32 MAX_DEVICES = 4;

void         init();
bool         register_device(BlockDevice* dev);
BlockDevice* get(u32 index);           // 0-based
BlockDevice* find(const char* name);   // by name
u32          count();

} // namespace block
