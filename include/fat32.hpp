#pragma once
// include/fat32.hpp - Knail FAT32 filesystem driver
#include <stdint.h>
#include "block.hpp"
#include "vfs.hpp"

namespace fat32 {

// Mount the FAT32 filesystem on dev at vfs_path.
// vfs_path must already exist as a directory in the VFS tree.
// Returns E_OK or negative errno.
int64_t mount(block::BlockDevice* dev, const char* vfs_path);

} // namespace fat32
