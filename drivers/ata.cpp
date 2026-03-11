// drivers/ata.cpp - Knail PIO ATA driver (primary controller only)
#include "ata.hpp"
#include "block.hpp"
#include "serial.hpp"
#include "heap.hpp"
#include <stdint.h>

namespace ata {

// ── I/O port base addresses ───────────────────────────────────────────────
static constexpr uint16_t ATA_PRIMARY_BASE    = 0x1F0;
static constexpr uint16_t ATA_PRIMARY_CTRL    = 0x3F6;
static constexpr uint16_t ATA_SECONDARY_BASE  = 0x170;
static constexpr uint16_t ATA_SECONDARY_CTRL  = 0x376;

// ── Register offsets from base ────────────────────────────────────────────
static constexpr uint16_t REG_DATA       = 0; // 16-bit data
static constexpr uint16_t REG_ERROR      = 1; // read: error, write: features
static constexpr uint16_t REG_SECCOUNT   = 2;
static constexpr uint16_t REG_LBA_LO     = 3;
static constexpr uint16_t REG_LBA_MID    = 4;
static constexpr uint16_t REG_LBA_HI     = 5;
static constexpr uint16_t REG_DRIVE_HEAD = 6;
static constexpr uint16_t REG_STATUS     = 7; // read: status, write: command
static constexpr uint16_t REG_COMMAND    = 7;

// ── Status bits ───────────────────────────────────────────────────────────
static constexpr uint8_t STATUS_ERR  = (1 << 0);
static constexpr uint8_t STATUS_DRQ  = (1 << 3); // data request ready
static constexpr uint8_t STATUS_SRV  = (1 << 4);
static constexpr uint8_t STATUS_DF   = (1 << 5); // drive fault
static constexpr uint8_t STATUS_RDY  = (1 << 6);
static constexpr uint8_t STATUS_BSY  = (1 << 7);

// ── Commands ──────────────────────────────────────────────────────────────
static constexpr uint8_t CMD_READ_SECTORS      = 0x20; // LBA28 read
static constexpr uint8_t CMD_WRITE_SECTORS     = 0x30; // LBA28 write
static constexpr uint8_t CMD_FLUSH_CACHE       = 0xE7;
static constexpr uint8_t CMD_IDENTIFY          = 0xEC;

// ── Drive descriptors ─────────────────────────────────────────────────────
struct ATADrive {
    uint16_t base;       // I/O base port
    uint16_t ctrl;       // control port
    uint8_t  slave;      // 0=master, 1=slave
    uint64_t sectors;    // LBA28 sector count
    bool     present;
};

static ATADrive  drives[4];     // 0=pri-master 1=pri-slave 2=sec-master 3=sec-slave
static block::BlockDevice bdevs[4];

// ── Port helpers ──────────────────────────────────────────────────────────
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

// 400ns delay — read alt status 4 times
static void ata_delay(uint16_t ctrl) {
    inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl);
}

// Wait until BSY clears. Returns false on timeout or error.
static bool wait_ready(uint16_t base, uint16_t ctrl) {
    for (uint32_t i = 0; i < 100000; i++) {
        uint8_t status = inb(ctrl);
        if (status & STATUS_BSY) continue;
        if (status & (STATUS_ERR | STATUS_DF)) return false;
        if (status & STATUS_RDY) return true;
    }
    return false;
}

// Wait for DRQ (data ready) after a command.
static bool wait_drq(uint16_t base) {
    for (uint32_t i = 0; i < 100000; i++) {
        uint8_t status = inb(base + REG_STATUS);
        if (status & STATUS_ERR) return false;
        if (status & STATUS_DF)  return false;
        if (status & STATUS_DRQ) return true;
    }
    return false;
}

static bool wait_bsy_clear(uint16_t ctrl) {
    for (uint32_t i = 0; i < 100000; i++) {
        uint8_t s = inb(ctrl);
        if (!(s & STATUS_BSY)) return true;
    }
    return false;
}

// ── LBA28 setup ───────────────────────────────────────────────────────────
static void setup_lba28(const ATADrive& drv, uint64_t lba, uint8_t count) {
    uint16_t base = drv.base;
    outb(base + REG_DRIVE_HEAD,
         0xE0 | (drv.slave << 4) | ((lba >> 24) & 0x0F));
    ata_delay(drv.ctrl);
    
    // Wait for BSY+DRQ both clear — use longer timeout
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t s = inb(drv.ctrl);
        if (!(s & (STATUS_BSY | STATUS_DRQ))) break;
    }
    
    outb(base + REG_ERROR,    0);
    outb(base + REG_SECCOUNT, count);
    outb(base + REG_LBA_LO,   (uint8_t)(lba));
    outb(base + REG_LBA_MID,  (uint8_t)(lba >> 8));
    outb(base + REG_LBA_HI,   (uint8_t)(lba >> 16));
}

static bool wait_drq_or_err(uint16_t base) {
    // Wait for command completion and data request.
    // Use primary status register so the controller state is acknowledged.
    for (uint32_t i = 0; i < 1000000; i++) {
        uint8_t s = inb(base + REG_STATUS);
        if (s & STATUS_BSY) continue;
        if (s & (STATUS_ERR | STATUS_DF)) return false;
        if (s & STATUS_DRQ) return true;
    }
    return false;
}



// ── read_sectors ────────────────────────────────────────────────────────── 
int64_t read_sectors(uint8_t drive_idx, uint64_t lba,
                     uint32_t count, void* buf) {
    if (drive_idx >= 4 || !drives[drive_idx].present) return E_BADF;
    if (count == 0) return E_OK;
    ATADrive& drv = drives[drive_idx];
    uint16_t* dst = reinterpret_cast<uint16_t*>(buf);

    for (uint32_t s = 0; s < count; s++) {
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            // Wait for drive to be truly idle before setup
            if (!wait_bsy_clear(drv.ctrl)) continue;

            setup_lba28(drv, lba + s, 1);
            outb(drv.base + REG_COMMAND, CMD_READ_SECTORS);

            // 400ns delay, then wait for BSY→DRQ
            ata_delay(drv.ctrl);
            if (!wait_drq_or_err(drv.base)) continue;

            for (int i = 0; i < 256; i++)
                dst[s * 256 + i] = inw(drv.base + REG_DATA);

            // Final status read to acknowledge transfer completion.
            uint8_t st = inb(drv.base + REG_STATUS);
            if (!(st & (STATUS_ERR | STATUS_DF))) ok = true;
        }
        if (!ok) return E_INVAL;
    }
    return E_OK;
}

// ── write_sectors ─────────────────────────────────────────────────────────
int64_t write_sectors(uint8_t drive_idx, uint64_t lba,
                      uint32_t count, const void* buf) {
    if (drive_idx >= 4 || !drives[drive_idx].present)
        return E_BADF;

    if (count == 0)
        return E_OK;

    ATADrive& drv = drives[drive_idx];
    const uint16_t* src = reinterpret_cast<const uint16_t*>(buf);
    for (uint32_t s = 0; s < count; s++) {
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            // Ensure drive is idle
            if (!wait_bsy_clear(drv.ctrl)) continue;
            setup_lba28(drv, lba + s, 1);
            outb(drv.base + REG_COMMAND, CMD_WRITE_SECTORS);
            ata_delay(drv.ctrl);
            // Wait for data request
            if (!wait_drq_or_err(drv.base)) continue;
            // Write 256 words (512 bytes)
            for (int i = 0; i < 256; i++)
                outw(drv.base + REG_DATA, src[s * 256 + i]);

            ata_delay(drv.ctrl);  // same 400ns for wait to completion

            if (!wait_bsy_clear(drv.ctrl)) continue;  // now actually waits for completion
            uint8_t st = inb(drv.base + REG_STATUS);
            if (st & (STATUS_ERR | STATUS_DF)) continue;
            ok = true;        
        }

        if (!ok) return E_INVAL;
    }

    outb(drv.base + REG_COMMAND, CMD_FLUSH_CACHE);

    if (!wait_bsy_clear(drv.ctrl))
        return E_INVAL;

    return E_OK;
}


// ── Block device vtable callbacks ─────────────────────────────────────────
static int64_t bdev_read(block::BlockDevice* dev, uint64_t lba,
                          uint32_t count, void* buf) {
    return read_sectors((uint8_t)(uint64_t)dev->priv, lba, count, buf);
}
static int64_t bdev_write(block::BlockDevice* dev, uint64_t lba,
                           uint32_t count, const void* buf) {
    return write_sectors((uint8_t)(uint64_t)dev->priv, lba, count, buf);
}

// ── identify ──────────────────────────────────────────────────────────────
static bool identify(ATADrive& drv) {
    if (!wait_ready(drv.base, drv.ctrl)) return false;

    // Select drive
    outb(drv.base + REG_DRIVE_HEAD, 0xA0 | (drv.slave << 4));
    ata_delay(drv.ctrl);

    // Zero LBA registers — if non-zero after IDENTIFY, not ATA
    outb(drv.base + REG_SECCOUNT, 0);
    outb(drv.base + REG_LBA_LO,   0);
    outb(drv.base + REG_LBA_MID,  0);
    outb(drv.base + REG_LBA_HI,   0);

    outb(drv.base + REG_COMMAND, CMD_IDENTIFY);
    ata_delay(drv.ctrl);

    // If status=0, no drive
    if (inb(drv.base + REG_STATUS) == 0) return false;

    // Wait for BSY to clear
    for (uint32_t i = 0; i < 100000; i++) {
        uint8_t s = inb(drv.base + REG_STATUS);
        if (!(s & STATUS_BSY)) break;
    }

    // Check for ATAPI (LBA mid/hi non-zero = not ATA)
    if (inb(drv.base + REG_LBA_MID) || inb(drv.base + REG_LBA_HI))
        return false;

    // Wait for DRQ or ERR
    for (uint32_t i = 0; i < 100000; i++) {
        uint8_t s = inb(drv.base + REG_STATUS);
        if (s & STATUS_ERR) return false;
        if (s & STATUS_DRQ) break;
    }

    // Read 256 words of identify data
    uint16_t ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = inw(drv.base + REG_DATA);

    // LBA28 sector count is at words 60-61
    drv.sectors = (uint32_t)ident[60] | ((uint32_t)ident[61] << 16);
    drv.present = (drv.sectors > 0);
    return drv.present;
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    // Set up drive descriptors
    drives[0] = { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   0, 0, false };
    drives[1] = { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   1, 0, false };
    drives[2] = { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 0, 0, false };
    drives[3] = { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 1, 0, false };

    static const char* names[] = { "hda", "hdb", "hdc", "hdd" };

    for (uint8_t i = 0; i < 4; i++) {
        if (!identify(drives[i])) continue;

        bdevs[i].read         = bdev_read;
        bdevs[i].write        = bdev_write;
        bdevs[i].sector_count = drives[i].sectors;
        bdevs[i].sector_size  = 512;
        bdevs[i].priv         = reinterpret_cast<void*>((uint64_t)i);

        // Copy name
        const char* n = names[i];
        for (int j = 0; j < 15 && n[j]; j++) bdevs[i].name[j] = n[j];
        bdevs[i].name[15] = 0;

        block::register_device(&bdevs[i]);

        serial::write("[ATA] found ");
        serial::write(names[i]);
        serial::write(" sectors=");
        serial::write_dec(drives[i].sectors);
        serial::write(" (");
        serial::write_dec(drives[i].sectors / 2048);
        serial::write(" MiB)\n");
    }
}

} // namespace ata
