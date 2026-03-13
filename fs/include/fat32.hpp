#pragma once
// include/fat32.hpp - Knail FAT32 filesystem driver
#include <stdint.h>
#include "block.hpp"
#include "vfs.hpp"
#include "types.hpp"

namespace fat32 {

// Mount the FAT32 filesystem on dev at vfs_path.
// vfs_path must already exist as a directory in the VFS tree.
kStatus mount(block::BlockDevice* dev, const char* vfs_path);

} // namespace fat32
