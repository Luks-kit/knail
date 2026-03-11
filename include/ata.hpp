#pragma once
// include/ata.hpp - Knail PIO ATA driver
#include <stdint.h>
#include "block.hpp"

namespace ata {

// Detect and register all ATA drives. Call after block::init().
void init();

// Direct sector access (bypasses block registry — use block:: normally).
// drive: 0=primary master, 1=primary slave
int64_t read_sectors (uint8_t drive, uint64_t lba,
                      uint32_t count, void* buf);
int64_t write_sectors(uint8_t drive, uint64_t lba,
                      uint32_t count, const void* buf);

} // namespace ata
