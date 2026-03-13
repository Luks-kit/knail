#pragma once
// include/ata.hpp - Knail PIO ATA driver
#include <stdint.h>
#include "block.hpp"
#include "types.hpp"

namespace ata {

// Detect and register all ATA drives. Call after block::init().
void init();

// Direct sector access (bypasses block registry — use block:: normally).
// drive: 0=primary master, 1=primary slave
kResult<i64> read_sectors (u8 drive, u64 lba, u32 count, void* buf);
kResult<i64> write_sectors(u8 drive, u64 lba, u32 count, const void* buf);

} // namespace ata
