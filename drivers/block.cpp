// drivers/block.cpp - Knail block device registry
#include "block.hpp"
#include "serial.hpp"

namespace block {

static BlockDevice* devices[MAX_DEVICES];
static uint32_t     device_count = 0;

void init() {
    for (uint32_t i = 0; i < MAX_DEVICES; i++)
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

BlockDevice* get(uint32_t index) {
    if (index >= device_count) return nullptr;
    return devices[index];
}

BlockDevice* find(const char* name) {
    for (uint32_t i = 0; i < device_count; i++) {
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

uint32_t count() { return device_count; }

} // namespace block
