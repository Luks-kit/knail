// drivers/block.cpp - Knail block device registry
#include "block.hpp"
#include "serial.hpp"
#include "types.hpp"

namespace block {

static BlockDevice* devices[MAX_DEVICES];
static u32          device_count = 0;

void init() {
    for (u32 i = 0; i < MAX_DEVICES; i++)
        devices[i] = nullptr;
    device_count = 0;
}

bool register_device(BlockDevice* dev) {
    if (device_count >= MAX_DEVICES) return false;
    devices[device_count++] = dev;
    serial::write("[block] registered ");
    serial::write(dev->name);
    serial::write(" sectors=");
    serial::write_dec(dev->sector_count);
    serial::write_line("");
    return true;
}

BlockDevice* get(u32 index) {
    if (index >= device_count) return nullptr;
    return devices[index];
}

BlockDevice* find(const char* name) {
    for (u32 i = 0; i < device_count; i++) {
        const char* a = devices[i]->name;
        const char* b = name;
        bool match = true;
        while (*a || *b) {
            if (*a != *b) { match = false; break; }
            a++; b++;
        }
        if (match) return devices[i];
    }
    return nullptr;
}

u32 count() { return device_count; }

} // namespace block
