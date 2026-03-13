#include "ata.hpp"
#include "block.hpp"
#include "serial.hpp"
#include "types.hpp"

namespace ata {

// ── I/O port base addresses ───────────────────────────────────────────────
static constexpr u16 ATA_PRIMARY_BASE   = 0x1F0;
static constexpr u16 ATA_PRIMARY_CTRL   = 0x3F6;
static constexpr u16 ATA_SECONDARY_BASE = 0x170;
static constexpr u16 ATA_SECONDARY_CTRL = 0x376;

// ── Register offsets from base ────────────────────────────────────────────
static constexpr u16 REG_DATA       = 0;
static constexpr u16 REG_ERROR      = 1;
static constexpr u16 REG_SECCOUNT   = 2;
static constexpr u16 REG_LBA_LO     = 3;
static constexpr u16 REG_LBA_MID    = 4;
static constexpr u16 REG_LBA_HI     = 5;
static constexpr u16 REG_DRIVE_HEAD = 6;
static constexpr u16 REG_STATUS     = 7;
static constexpr u16 REG_COMMAND    = 7;

// ── Status bits ───────────────────────────────────────────────────────────
static constexpr u8 STATUS_ERR = (1 << 0);
static constexpr u8 STATUS_DRQ = (1 << 3);
static constexpr u8 STATUS_SRV = (1 << 4);
static constexpr u8 STATUS_DF  = (1 << 5);
static constexpr u8 STATUS_RDY = (1 << 6);
static constexpr u8 STATUS_BSY = (1 << 7);

// ── Commands ──────────────────────────────────────────────────────────────
static constexpr u8 CMD_READ_SECTORS  = 0x20;
static constexpr u8 CMD_WRITE_SECTORS = 0x30;
static constexpr u8 CMD_FLUSH_CACHE   = 0xE7;
static constexpr u8 CMD_IDENTIFY      = 0xEC;

// ── Drive descriptors ─────────────────────────────────────────────────────
struct ATADrive {
    u16  base;
    u16  ctrl;
    u8   slave;
    u64  sectors;
    bool present;
};

static ATADrive          drives[4];
static block::BlockDevice bdevs[4];

// ── Port helpers ──────────────────────────────────────────────────────────
static inline void outb(u16 port, u8 val) {
    __asm__ volatile("outb %0, %1" :: "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline u16 inw(u16 port) {
    u16 val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
static inline void outw(u16 port, u16 val) {
    __asm__ volatile("outw %0, %1" :: "a"(val), "Nd"(port));
}

static void ata_delay(u16 ctrl) {
    inb(ctrl); inb(ctrl); inb(ctrl); inb(ctrl);
}

static bool wait_ready(u16 base, u16 ctrl) {
    for (u32 i = 0; i < 100000; i++) {
        u8 status = inb(ctrl);
        if (status & STATUS_BSY) continue;
        if (status & (STATUS_ERR | STATUS_DF)) return false;
        if (status & STATUS_RDY) return true;
    }
    return false;
}

static bool wait_drq(u16 base) {
    for (u32 i = 0; i < 100000; i++) {
        u8 status = inb(base + REG_STATUS);
        if (status & STATUS_ERR) return false;
        if (status & STATUS_DF)  return false;
        if (status & STATUS_DRQ) return true;
    }
    return false;
}

static bool wait_bsy_clear(u16 ctrl) {
    for (u32 i = 0; i < 100000; i++) {
        if (!(inb(ctrl) & STATUS_BSY)) return true;
    }
    return false;
}

// ── LBA28 setup ───────────────────────────────────────────────────────────
static void setup_lba28(const ATADrive& drv, u64 lba, u8 count) {
    u16 base = drv.base;
    outb(base + REG_DRIVE_HEAD,
         0xE0 | (drv.slave << 4) | ((lba >> 24) & 0x0F));
    ata_delay(drv.ctrl);
    for (u32 i = 0; i < 1000000; i++) {
        u8 s = inb(drv.ctrl);
        if (!(s & (STATUS_BSY | STATUS_DRQ))) break;
    }
    outb(base + REG_ERROR,    0);
    outb(base + REG_SECCOUNT, count);
    outb(base + REG_LBA_LO,   (u8)(lba));
    outb(base + REG_LBA_MID,  (u8)(lba >> 8));
    outb(base + REG_LBA_HI,   (u8)(lba >> 16));
}

static bool wait_drq_or_err(u16 base) {
    for (u32 i = 0; i < 1000000; i++) {
        u8 s = inb(base + REG_STATUS);
        if (s & STATUS_BSY) continue;
        if (s & (STATUS_ERR | STATUS_DF)) return false;
        if (s & STATUS_DRQ) return true;
    }
    return false;
}

// ── read_sectors ──────────────────────────────────────────────────────────
kResult<i64> read_sectors(u8 drive_idx, u64 lba, u32 count, void* buf) {
    if (drive_idx >= 4 || !drives[drive_idx].present)
        return kResult<i64>::err(kError::DeviceNotReady);
    if (count == 0)
        return kResult<i64>::ok(0);

    ATADrive& drv = drives[drive_idx];
    u16*      dst = reinterpret_cast<u16*>(buf);

    for (u32 s = 0; s < count; s++) {
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            if (!wait_bsy_clear(drv.ctrl)) continue;
            setup_lba28(drv, lba + s, 1);
            outb(drv.base + REG_COMMAND, CMD_READ_SECTORS);
            ata_delay(drv.ctrl);
            if (!wait_drq_or_err(drv.base)) continue;
            for (int i = 0; i < 256; i++)
                dst[s * 256 + i] = inw(drv.base + REG_DATA);
            u8 st = inb(drv.base + REG_STATUS);
            if (!(st & (STATUS_ERR | STATUS_DF))) ok = true;
        }
        if (!ok) return kResult<i64>::err(kError::IOError);
    }
    return kResult<i64>::ok((i64)count);
}

// ── write_sectors ─────────────────────────────────────────────────────────
kResult<i64> write_sectors(u8 drive_idx, u64 lba, u32 count, const void* buf) {
    if (drive_idx >= 4 || !drives[drive_idx].present)
        return kResult<i64>::err(kError::DeviceNotReady);
    if (count == 0)
        return kResult<i64>::ok(0);

    ATADrive&     drv = drives[drive_idx];
    const u16*    src = reinterpret_cast<const u16*>(buf);

    for (u32 s = 0; s < count; s++) {
        bool ok = false;
        for (int attempt = 0; attempt < 4 && !ok; attempt++) {
            if (!wait_bsy_clear(drv.ctrl)) continue;
            setup_lba28(drv, lba + s, 1);
            outb(drv.base + REG_COMMAND, CMD_WRITE_SECTORS);
            ata_delay(drv.ctrl);
            if (!wait_drq_or_err(drv.base)) continue;
            for (int i = 0; i < 256; i++)
                outw(drv.base + REG_DATA, src[s * 256 + i]);
            ata_delay(drv.ctrl);
            if (!wait_bsy_clear(drv.ctrl)) continue;
            u8 st = inb(drv.base + REG_STATUS);
            if (!(st & (STATUS_ERR | STATUS_DF))) ok = true;
        }
        if (!ok) return kResult<i64>::err(kError::IOError);
    }

    outb(drives[drive_idx].base + REG_COMMAND, CMD_FLUSH_CACHE);
    if (!wait_bsy_clear(drives[drive_idx].ctrl))
        return kResult<i64>::err(kError::IOError);

    return kResult<i64>::ok((i64)count);
}

// ── Block device vtable callbacks ─────────────────────────────────────────
static kResult<i64> bdev_read(block::BlockDevice* dev, u64 lba,
                               u32 count, void* buf) {
    return read_sectors((u8)(u64)dev->priv, lba, count, buf);
}

static kStatus bdev_write(block::BlockDevice* dev, u64 lba,
                           u32 count, const void* buf) {
    auto res = write_sectors((u8)(u64)dev->priv, lba, count, buf);
    if (res.is_err()) return kStatus::err(res.error());
    return kStatus::ok();
}

// ── identify ──────────────────────────────────────────────────────────────
static bool identify(ATADrive& drv) {
    if (!wait_ready(drv.base, drv.ctrl)) return false;

    outb(drv.base + REG_DRIVE_HEAD, 0xA0 | (drv.slave << 4));
    ata_delay(drv.ctrl);
    outb(drv.base + REG_SECCOUNT, 0);
    outb(drv.base + REG_LBA_LO,   0);
    outb(drv.base + REG_LBA_MID,  0);
    outb(drv.base + REG_LBA_HI,   0);
    outb(drv.base + REG_COMMAND,  CMD_IDENTIFY);
    ata_delay(drv.ctrl);

    if (inb(drv.base + REG_STATUS) == 0) return false;

    for (u32 i = 0; i < 100000; i++) {
        if (!(inb(drv.base + REG_STATUS) & STATUS_BSY)) break;
    }
    if (inb(drv.base + REG_LBA_MID) || inb(drv.base + REG_LBA_HI))
        return false;

    for (u32 i = 0; i < 100000; i++) {
        u8 s = inb(drv.base + REG_STATUS);
        if (s & STATUS_ERR) return false;
        if (s & STATUS_DRQ) break;
    }

    u16 ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = inw(drv.base + REG_DATA);

    drv.sectors = (u32)ident[60] | ((u32)ident[61] << 16);
    drv.present = (drv.sectors > 0);
    return drv.present;
}

// ── init ──────────────────────────────────────────────────────────────────
void init() {
    drives[0] = { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   0, 0, false };
    drives[1] = { ATA_PRIMARY_BASE,   ATA_PRIMARY_CTRL,   1, 0, false };
    drives[2] = { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 0, 0, false };
    drives[3] = { ATA_SECONDARY_BASE, ATA_SECONDARY_CTRL, 1, 0, false };

    static const char* names[] = { "hda", "hdb", "hdc", "hdd" };

    for (u8 i = 0; i < 4; i++) {
        if (!identify(drives[i])) continue;

        bdevs[i].read         = bdev_read;
        bdevs[i].write        = bdev_write;
        bdevs[i].sector_count = drives[i].sectors;
        bdevs[i].sector_size  = 512;
        bdevs[i].priv         = reinterpret_cast<void*>((u64)i);

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

} // namespace ata/ drivers/ata.cpp - Knail PIO ATA driver (primary controller only)
