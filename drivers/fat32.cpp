// drivers/fat32.cpp - Knail FAT32 filesystem driver
#include "fat32.hpp"
#include "syscall.hpp"
#include "vfs.hpp"
#include "heap.hpp"
#include "serial.hpp"
#include <stdint.h>

namespace fat32 {

// ── BPB (BIOS Parameter Block) ────────────────────────────────────────────
struct [[gnu::packed]] BPB {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entry_count;   // 0 for FAT32
    uint16_t total_sectors_16;   // 0 for FAT32
    uint8_t  media_type;
    uint16_t fat_size_16;        // 0 for FAT32
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    // FAT32 extended BPB
    uint32_t fat_size_32;        // sectors per FAT
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;       // usually 2
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];         // "FAT32   "
};

// ── Directory entry ───────────────────────────────────────────────────────
struct [[gnu::packed]] DirEntry {
    char     name[8];
    char     ext[3];
    uint8_t  attr;
    uint8_t  reserved;
    uint8_t  ctime_tenth;
    uint16_t ctime;
    uint16_t cdate;
    uint16_t adate;
    uint16_t cluster_hi;
    uint16_t mtime;
    uint16_t mdate;
    uint16_t cluster_lo;
    uint32_t size;
};

static constexpr uint8_t ATTR_READ_ONLY = 0x01;
static constexpr uint8_t ATTR_HIDDEN    = 0x02;
static constexpr uint8_t ATTR_SYSTEM    = 0x04;
static constexpr uint8_t ATTR_VOLUME_ID = 0x08;
static constexpr uint8_t ATTR_DIR       = 0x10;
static constexpr uint8_t ATTR_ARCHIVE   = 0x20;
static constexpr uint8_t ATTR_LFN       = 0x0F; // long filename marker
static constexpr uint8_t NTRES_LOWER_BASE = 0x08;
static constexpr uint8_t NTRES_LOWER_EXT  = 0x10;

// ── FAT32 mount state ─────────────────────────────────────────────────────
struct Fat32Mount {
    block::BlockDevice* dev;
    uint32_t fat_start;         // LBA of first FAT sector
    uint32_t data_start;        // LBA of first data sector (cluster 2)
    uint32_t sectors_per_cluster;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint32_t fat_size;          // sectors per FAT
    uint8_t  num_fats;
};

// ── FAT32 VNode private data ──────────────────────────────────────────────
struct Fat32Node {
    Fat32Mount* mount;
    uint32_t    first_cluster;
    bool        is_dir;
    uint32_t    dirent_sector;
    uint32_t    dirent_offset;
    bool        has_dirent;
};

// ── Helpers ───────────────────────────────────────────────────────────────
static uint32_t cluster_to_lba(Fat32Mount* m, uint32_t cluster) {
    return m->data_start + (cluster - 2) * m->sectors_per_cluster;
}

// Read one FAT entry for a given cluster number.
static uint32_t read_fat(Fat32Mount* m, uint32_t cluster) {
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = m->fat_start + fat_offset / 512;
    uint32_t byte_offset = fat_offset % 512;

    uint8_t buf[512];
    if (m->dev->read(m->dev, fat_sector, 1, buf) != E_OK)
        return 0x0FFFFFFF; // treat as EOC on error

    uint32_t val = *reinterpret_cast<uint32_t*>(buf + byte_offset);
    return val & 0x0FFFFFFF;
}

// Write one FAT entry.
static bool write_fat(Fat32Mount* m, uint32_t cluster, uint32_t value) {
    uint32_t fat_offset  = cluster * 4;
    uint32_t fat_sector  = m->fat_start + fat_offset / 512;
    uint32_t byte_offset = fat_offset % 512;

    uint8_t buf[512];
    if (m->dev->read(m->dev, fat_sector, 1, buf) != E_OK)
        return false;

    *reinterpret_cast<uint32_t*>(buf + byte_offset) =
        (value & 0x0FFFFFFF) |
        (*reinterpret_cast<uint32_t*>(buf + byte_offset) & 0xF0000000);

    // Write to all FATs
    for (uint8_t f = 0; f < m->num_fats; f++) {
        uint32_t lba = m->fat_start + f * m->fat_size + fat_offset / 512;
        if (m->dev->write(m->dev, lba, 1, buf) != E_OK)
            return false;
    }
    return true;
}

// Find a free cluster, mark it as EOC in the FAT. Returns 0 on failure.
static uint32_t alloc_cluster(Fat32Mount* m) {
    for (uint32_t c = 2; c < m->total_clusters + 2; c++) {
        if (read_fat(m, c) == 0) {
            write_fat(m, c, 0x0FFFFFFF); // mark EOC
            return c;
        }
    }
    return 0;
}

// Zero out all sectors in a cluster.
static bool zero_cluster(Fat32Mount* m, uint32_t cluster) {
    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    uint32_t lba = cluster_to_lba(m, cluster);
    for (uint32_t s = 0; s < m->sectors_per_cluster; s++) {
        if (m->dev->write(m->dev, lba + s, 1, buf) != E_OK)
            return false;
    }
    return true;
}

static bool is_eoc(uint32_t val) {
    return (val & 0x0FFFFFFF) >= 0x0FFFFFF8;
}

// ── 8.3 name helpers ──────────────────────────────────────────────────────
// Convert a DirEntry 8.3 name into a null-terminated string.
static void decode_83(const DirEntry& e, char* out) {
    int i = 0;
    // name part (strip trailing spaces)
    bool lower_base = (e.reserved & NTRES_LOWER_BASE) != 0;
    bool lower_ext  = (e.reserved & NTRES_LOWER_EXT) != 0;

    int name_len = 8;
    while (name_len > 0 && e.name[name_len - 1] == ' ') name_len--;
    for (int k = 0; k < name_len; k++) {
        char c = e.name[k];
        // lowercase
        if (lower_base && c >= 'A' && c <= 'Z') c += 32;
        out[i++] = c;
    }

    int ext_len = 3;
    while (ext_len > 0 && e.ext[ext_len - 1] == ' ') ext_len--;
    if (ext_len > 0) {
        out[i++] = '.';
        for (int k = 0; k < ext_len; k++) {
            char c = e.ext[k];
            if (lower_ext && c >= 'A' && c <= 'Z') c += 32;
            out[i++] = c;
        }
    }
    out[i] = 0;
}

// Encode a short filename into 8.3 format (uppercase, space-padded).
// Also set NT lowercase flags for nicer host-side display.
static void encode_83(const char* name, char out_name[8], char out_ext[3], uint8_t* out_ntres) {
    for (int i = 0; i < 8; i++) out_name[i] = ' ';
    for (int i = 0; i < 3; i++) out_ext[i]  = ' ';
    bool saw_base_lower = false;
    bool saw_ext_lower  = false;

    int i = 0, j = 0;
    while (name[j] && name[j] != '.') {
        if (i < 8) {
            char c = name[j];
            if (c >= 'a' && c <= 'z') { saw_base_lower = true; c -= 32; }
            out_name[i++] = c;
        }
        j++;
    }
    if (name[j] == '.') {
        j++; int k = 0;
        while (name[j] && k < 3) {
            char c = name[j++];
            if (c >= 'a' && c <= 'z') { saw_ext_lower = true; c -= 32; }
            out_ext[k++] = c;
        }
    }
    
    uint8_t ntres = 0;
    if (saw_base_lower) ntres |= NTRES_LOWER_BASE;
    if (saw_ext_lower)  ntres |= NTRES_LOWER_EXT;
    if (out_ntres) *out_ntres = ntres;

}

static bool name_matches(const DirEntry& e, const char* name) {
    char decoded[13];
    decode_83(e, decoded);
    const char* a = decoded;
    const char* b = name;
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool dir_find(Fat32Mount* m, uint32_t dir_cluster,
                     const char* name, DirEntry* out_de,
                     uint32_t* out_sector, uint32_t* out_offset);
static vfs::VNode* make_vnode(Fat32Mount* m, const DirEntry& de, uint32_t dirent_sector, uint32_t dirent_offset);


// ── Cluster chain traversal ───────────────────────────────────────────────
// Read bytes from a cluster chain starting at first_cluster.
static int64_t chain_read(Fat32Mount* m, uint32_t first_cluster,
                           uint64_t offset, void* buf, uint64_t len) {
    if (len == 0) return 0;
    uint32_t cluster_bytes = m->sectors_per_cluster * 512;
    uint64_t read_so_far   = 0;
    uint32_t cluster       = first_cluster;
    uint64_t cluster_idx   = offset / cluster_bytes;

    for (uint64_t i = 0; i < cluster_idx; i++) {
        cluster = read_fat(m, cluster);
        if (is_eoc(cluster) || cluster < 2) return (int64_t)read_so_far;
    }

    uint64_t intra = offset % cluster_bytes;
    uint8_t* dst = reinterpret_cast<uint8_t*>(buf);

    while (len > 0) {
        // Advance cluster when intra rolls past cluster boundary
        while (intra >= cluster_bytes) {
            intra  -= cluster_bytes;
            cluster = read_fat(m, cluster);
            if (is_eoc(cluster) || cluster < 2) return (int64_t)read_so_far;
        }
        if (is_eoc(cluster) || cluster < 2) break;

        uint32_t lba     = cluster_to_lba(m, cluster);
        uint32_t sec_off = intra / 512;
        uint32_t byte_in = intra % 512;

        uint8_t sector_buf[512];
        if (m->dev->read(m->dev, lba + sec_off, 1, sector_buf) != E_OK)
            break;

        uint64_t avail = 512 - byte_in;
        if (avail > len) avail = len;
        for (uint64_t i = 0; i < avail; i++)
            dst[read_so_far + i] = sector_buf[byte_in + i];

        read_so_far += avail;
        len         -= avail;
        intra       += avail;
    }

    return (int64_t)read_so_far;
}

// Write bytes into a cluster chain, extending if needed.
static int64_t chain_write(Fat32Mount* m, uint32_t* first_cluster_ptr,
                           uint64_t offset, const void* buf, uint64_t len) {
    if (len == 0) return 0;

    uint32_t cluster_bytes = m->sectors_per_cluster * 512;
    uint64_t written = 0;
    uint32_t cluster = *first_cluster_ptr;

    // ── Allocate first cluster if file empty
    if (cluster < 2) {
        cluster = alloc_cluster(m);
        if (!cluster) return E_NOSPACE;
        zero_cluster(m, cluster);
        *first_cluster_ptr = cluster;
    }

    uint32_t prev_cluster = 0;
    uint64_t cluster_idx = offset / cluster_bytes;

    // ── Phase 1: Walk/extend cluster chain to start offset
    for (uint64_t i = 0; i < cluster_idx; i++) {
        prev_cluster = cluster;
        uint32_t next = read_fat(m, cluster);
        if (is_eoc(next) || next < 2) {
            uint32_t newc = alloc_cluster(m);
            if (!newc) return E_NOSPACE;
            zero_cluster(m, newc);
            if (!write_fat(m, cluster, newc)) return E_INVAL;
            cluster = newc;
        } else {
            cluster = next;
        }
    }

    uint64_t intra = offset % cluster_bytes;
    const uint8_t* src = reinterpret_cast<const uint8_t*>(buf);

    // ── Phase 2: Write data
    while (len > 0) {
        uint32_t lba = cluster_to_lba(m, cluster);
        uint32_t sec_off = intra / 512;
        uint32_t byte_in = intra % 512;

        uint8_t sector_buf[512];
        if (m->dev->read(m->dev, lba + sec_off, 1, sector_buf) != E_OK)
            return written;

        uint64_t avail = 512 - byte_in;
        if (avail > len) avail = len;

        for (uint64_t i = 0; i < avail; i++)
            sector_buf[byte_in + i] = src[written + i];

        if (m->dev->write(m->dev, lba + sec_off, 1, sector_buf) != E_OK)
            return written;

        written += avail;
        len -= avail;
        intra += avail;

        // ── Advance to next cluster if needed
        if (intra >= cluster_bytes && len > 0) {
            intra = 0;
            prev_cluster = cluster;
            uint32_t next = read_fat(m, cluster);
            if (is_eoc(next) || next < 2) {
                uint32_t newc = alloc_cluster(m);
                if (!newc) return written;
                zero_cluster(m, newc);
                if (!write_fat(m, cluster, newc)) return E_INVAL;
                cluster = newc;
            } else {
                cluster = next;
            }
        }
    }

    return written;
}



static bool write_dirent_at(Fat32Mount* m, uint32_t sector, uint32_t offset, const DirEntry& de) {
    uint8_t buf[512];
    if (m->dev->read(m->dev, sector, 1, buf) != E_OK) return false;
    *reinterpret_cast<DirEntry*>(buf + offset) = de;
    return m->dev->write(m->dev, sector, 1, buf) == E_OK;
}

static bool find_free_dirent(Fat32Mount* m, uint32_t dir_cluster,
                             uint32_t* out_sector, uint32_t* out_offset) {
    uint32_t cluster = dir_cluster;
    while (cluster >= 2) {
        uint32_t lba = cluster_to_lba(m, cluster);
        for (uint32_t s = 0; s < m->sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (m->dev->read(m->dev, lba + s, 1, buf) != E_OK) return false;
            uint32_t eps = 512 / sizeof(DirEntry);
            for (uint32_t e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00 || (uint8_t)de->name[0] == 0xE5) {
                    *out_sector = lba + s;
                    *out_offset = e * sizeof(DirEntry);
                    return true;
                }
            }
        }
        uint32_t next = read_fat(m, cluster);
        if (is_eoc(next) || next < 2) break;
        cluster = next;
    }

    uint32_t newc = alloc_cluster(m);
    if (!newc) return false;
    if (!zero_cluster(m, newc)) return false;
    if (!write_fat(m, cluster, newc)) return false;

    *out_sector = cluster_to_lba(m, newc);
    *out_offset = 0;
    return true;
}


// ── VFS ops implementation ────────────────────────────────────────────────
static int64_t fat_read(vfs::VNode* node, uint64_t offset,
                         void* buf, uint64_t len) {
    Fat32Node* fn = reinterpret_cast<Fat32Node*>(node->priv);
    if (offset >= node->size) return 0;
    uint64_t avail = node->size - offset;
    if (len > avail) len = avail;
    return chain_read(fn->mount, fn->first_cluster, offset, buf, len);
}

// FAT32 write wrapper that updates DirEntry
static int64_t fat_write(vfs::VNode* node, uint64_t offset,
                         const void* buf, uint64_t len) {
    Fat32Node* fn = reinterpret_cast<Fat32Node*>(node->priv);

    int64_t written = chain_write(fn->mount, &fn->first_cluster, offset, buf, len);
    if (written <= 0) return written;

    // Update VNode size
    uint64_t end = offset + written;
    if (end > node->size) node->size = end;

    // Update directory entry
    if (fn->has_dirent) {
        uint8_t sector_buf[512];
        if (fn->mount->dev->read(fn->mount->dev, fn->dirent_sector, 1, sector_buf) == E_OK) {
            DirEntry* de = reinterpret_cast<DirEntry*>(sector_buf + fn->dirent_offset);
            de->size = static_cast<uint32_t>(node->size);
            de->cluster_hi = static_cast<uint16_t>((fn->first_cluster >> 16) & 0xFFFF);
            de->cluster_lo = static_cast<uint16_t>(fn->first_cluster & 0xFFFF);
            fn->mount->dev->write(fn->mount->dev, fn->dirent_sector, 1, sector_buf);
        }
    }

    return written;
}

static vfs::VNode* fat_create(vfs::VNode* dir, const char* name, uint32_t type) {
    Fat32Node* dfn = reinterpret_cast<Fat32Node*>(dir->priv);
    Fat32Mount* m = dfn->mount;

    DirEntry existing{};
    uint32_t ex_sec = 0, ex_off = 0;
    if (dir_find(m, dfn->first_cluster, name, &existing, &ex_sec, &ex_off))
        return nullptr;

    uint32_t sec = 0, off = 0;
    if (!find_free_dirent(m, dfn->first_cluster, &sec, &off)) return nullptr;

    DirEntry de{};
    for (size_t i = 0; i < sizeof(DirEntry); i++) reinterpret_cast<uint8_t*>(&de)[i] = 0;
    encode_83(name, de.name, de.ext, &de.reserved);
    de.attr = (type == VFS_TYPE_DIR) ? ATTR_DIR : ATTR_ARCHIVE;
    de.size = 0;

    uint32_t first_cluster = 0;
    if (type == VFS_TYPE_DIR) {
        first_cluster = alloc_cluster(m);
        if (!first_cluster) return nullptr;
        if (!zero_cluster(m, first_cluster)) return nullptr;

        // Build '.' and '..' entries in new directory cluster.
        uint8_t buf[512];
        for (uint32_t i = 0; i < 512; i++) buf[i] = 0;
        DirEntry* dot = reinterpret_cast<DirEntry*>(buf + 0);
        DirEntry* dotdot = reinterpret_cast<DirEntry*>(buf + sizeof(DirEntry));
        for (int i = 0; i < 8; i++) { dot->name[i] = ' '; dotdot->name[i] = ' '; }
        for (int i = 0; i < 3; i++) { dot->ext[i] = ' '; dotdot->ext[i] = ' '; }
        dot->name[0] = '.';
        dotdot->name[0] = '.'; dotdot->name[1] = '.';
        dot->attr = ATTR_DIR;
        dotdot->attr = ATTR_DIR;
        dot->cluster_hi = static_cast<uint16_t>((first_cluster >> 16) & 0xFFFF);
        dot->cluster_lo = static_cast<uint16_t>(first_cluster & 0xFFFF);

        uint32_t parent_cluster = dfn->first_cluster;
        dotdot->cluster_hi = static_cast<uint16_t>((parent_cluster >> 16) & 0xFFFF);
        dotdot->cluster_lo = static_cast<uint16_t>(parent_cluster & 0xFFFF);

        uint32_t lba = cluster_to_lba(m, first_cluster);
        if (m->dev->write(m->dev, lba, 1, buf) != E_OK) return nullptr;
    }

    de.cluster_hi = static_cast<uint16_t>((first_cluster >> 16) & 0xFFFF);
    de.cluster_lo = static_cast<uint16_t>(first_cluster & 0xFFFF);
    if (!write_dirent_at(m, sec, off, de)) return nullptr;

    vfs::VNode* node = make_vnode(m, de, sec, off);
    return node;
}

static int64_t fat_unlink(vfs::VNode* dir, const char* name) {
    Fat32Node* dfn = reinterpret_cast<Fat32Node*>(dir->priv);
    Fat32Mount* m = dfn->mount;
    DirEntry de{};
    uint32_t sec = 0, off = 0;
    if (!dir_find(m, dfn->first_cluster, name, &de, &sec, &off)) return E_NOENT;
    if (de.attr & ATTR_DIR) return E_ISDIR;

    // Mark deleted in directory table.
    uint8_t buf[512];
    if (m->dev->read(m->dev, sec, 1, buf) != E_OK) return E_INVAL;
    buf[off] = 0xE5;
    if (m->dev->write(m->dev, sec, 1, buf) != E_OK) return E_INVAL;

    // Free cluster chain.
    uint32_t cl = ((uint32_t)de.cluster_hi << 16) | de.cluster_lo;
    while (cl >= 2 && !is_eoc(cl)) {
        uint32_t next = read_fat(m, cl);
        if(!write_fat(m, cl, 0)) return E_INVAL;
        cl = next;
    }
    if (cl >= 2 && is_eoc(cl)) { if (!write_fat(m, cl, 0)) return E_INVAL; }
    return E_OK;
}



static int64_t fat_readdir(vfs::VNode* node, uint64_t index,
                            Dirent* out) {
    Fat32Node* fn = reinterpret_cast<Fat32Node*>(node->priv);
    Fat32Mount* m = fn->mount;

    // Walk entries, skipping LFN and deleted/empty
    uint64_t real_idx = 0;
    uint32_t cluster  = fn->first_cluster;

    while (!is_eoc(cluster) && cluster >= 2) {
        uint32_t lba = cluster_to_lba(m, cluster);
        for (uint32_t s = 0; s < m->sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (m->dev->read(m->dev, lba + s, 1, buf) != E_OK)
                return E_INVAL;
            uint32_t eps = 512 / sizeof(DirEntry);
            for (uint32_t e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00) return E_EOF; // end of dir
                if ((uint8_t)de->name[0] == 0xE5) continue; // deleted
                if (de->attr == ATTR_LFN) continue;
                if (de->attr & ATTR_VOLUME_ID) continue;
                // Skip . and ..
                if (de->name[0] == '.' ) continue;

                if (real_idx == index) {
                    decode_83(*de, out->name);
                    out->type = (de->attr & ATTR_DIR)
                                ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                    return E_OK;
                }
                real_idx++;
            }
        }
        cluster = read_fat(m, cluster);
    }
    return E_EOF;
}

static void fat_close(vfs::VNode* /*node*/) {
    // VNodes remain resident in the mounted in-memory tree.
    // Do not free node->priv on close; that would invalidate future opens.
}

static vfs::FileOps fat_ops = {
    .read    = fat_read,
    .write   = fat_write,
    .close   = fat_close,
    .create  = fat_create,
    .unlink  = fat_unlink,
    .readdir = fat_readdir,
};

// ── Directory lookup ──────────────────────────────────────────────────────
// Find a named entry in a directory cluster chain.
// Fills out_de and returns true if found.
static bool dir_find(Fat32Mount* m, uint32_t dir_cluster,
                     const char* name, DirEntry* out_de,
                     uint32_t* out_sector, uint32_t* out_offset) {
    uint32_t cluster = dir_cluster;
    while (!is_eoc(cluster) && cluster >= 2) {
        uint32_t lba = cluster_to_lba(m, cluster);
        for (uint32_t s = 0; s < m->sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (m->dev->read(m->dev, lba + s, 1, buf) != E_OK)
                return false;
            uint32_t eps = 512 / sizeof(DirEntry);
            for (uint32_t e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00) return false;
                if ((uint8_t)de->name[0] == 0xE5) continue;
                if (de->attr == ATTR_LFN) continue;
                if (de->attr & ATTR_VOLUME_ID) continue;
                if (name_matches(*de, name)) {
                    *out_de     = *de;
                    *out_sector = lba + s;
                    *out_offset = e * sizeof(DirEntry);
                    return true;
                }
            }
        }
        cluster = read_fat(m, cluster);
    }
    return false;
}

// Build a VNode for a DirEntry.
static vfs::VNode* make_vnode(Fat32Mount* m, const DirEntry& de, uint32_t dirent_sector, uint32_t dirent_offset) {   
    char name[13];
    decode_83(de, name);

    vfs::VNode* node = reinterpret_cast<vfs::VNode*>(
        heap::kmalloc(sizeof(vfs::VNode)));
    if (!node) return nullptr;
    for (size_t i = 0; i < sizeof(vfs::VNode); i++)
        reinterpret_cast<uint8_t*>(node)[i] = 0;

    // Copy name
    for (int i = 0; i < 127 && name[i]; i++) node->name[i] = name[i];

    uint32_t cluster = ((uint32_t)de.cluster_hi << 16) | de.cluster_lo;
    bool is_dir = (de.attr & ATTR_DIR) != 0;

    node->type = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    node->size = is_dir ? 0 : de.size;
    node->ops  = &fat_ops;

    Fat32Node* fn = reinterpret_cast<Fat32Node*>(heap::kmalloc(sizeof(Fat32Node)));
    if (!fn) { heap::kfree(node); return nullptr; }
    fn->mount         = m;
    fn->first_cluster = cluster;
    fn->is_dir        = is_dir;
    fn->dirent_sector = dirent_sector;
    fn->dirent_offset = dirent_offset;
    fn->has_dirent    = true;
    node->priv        = fn;

    return node;
}

// ── Recursively populate VFS tree from a FAT32 directory ─────────────────
static void populate_dir(Fat32Mount* m, uint32_t dir_cluster,
                          vfs::VNode* parent) {
    uint32_t cluster = dir_cluster;
    while (!is_eoc(cluster) && cluster >= 2) {
        uint32_t lba = cluster_to_lba(m, cluster);
        for (uint32_t s = 0; s < m->sectors_per_cluster; s++) {
            uint8_t buf[512];
            if (m->dev->read(m->dev, lba + s, 1, buf) != E_OK) return;
            uint32_t eps = 512 / sizeof(DirEntry);
            for (uint32_t e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00) return;
                if ((uint8_t)de->name[0] == 0xE5) continue;
                if (de->attr == ATTR_LFN) continue;
                if (de->attr & ATTR_VOLUME_ID) continue;
                if (de->name[0] == '.') continue;

                vfs::VNode* child = make_vnode(m, *de, lba + s, e * sizeof(DirEntry)); 
                if (!child) continue;                                
                // Link into VFS tree
                child->parent       = parent;
                child->next_sibling = parent->children;
                parent->children    = child;

                // Recurse into subdirectories
                if (de->attr & ATTR_DIR) {
                    Fat32Node* fn = reinterpret_cast<Fat32Node*>(child->priv);
                    populate_dir(m, fn->first_cluster, child);
                }
            }
        }
        cluster = read_fat(m, cluster);
    }
}

// ── mount ─────────────────────────────────────────────────────────────────
int64_t mount(block::BlockDevice* dev, const char* vfs_path) {
    // Read sector 0 — BPB
    uint8_t buf[512];
    if (dev->read(dev, 0, 1, buf) != E_OK) {
        serial::write_line("[FAT32] failed to read BPB");
        return E_INVAL;
    }

    BPB* bpb = reinterpret_cast<BPB*>(buf);

    // Sanity checks
    if (bpb->bytes_per_sector != 512) {
        serial::write_line("[FAT32] unsupported sector size");
        return E_INVAL;
    }
    if (bpb->fat_size_32 == 0) {
        serial::write_line("[FAT32] not FAT32 (fat_size_32=0)");
        return E_INVAL;
    }

    Fat32Mount* m = reinterpret_cast<Fat32Mount*>(heap::kmalloc(sizeof(Fat32Mount)));
    if (!m) return E_NOSPACE;

    m->dev                  = dev;
    m->fat_start            = bpb->reserved_sectors;
    m->fat_size             = bpb->fat_size_32;
    m->num_fats             = bpb->num_fats;
    m->sectors_per_cluster  = bpb->sectors_per_cluster;
    m->root_cluster         = bpb->root_cluster;
    m->data_start           = bpb->reserved_sectors
                            + bpb->num_fats * bpb->fat_size_32;
    m->total_clusters       = (bpb->total_sectors_32 - m->data_start)
                            / bpb->sectors_per_cluster;

    serial::write("[FAT32] mounting on ");
    serial::write(vfs_path);
    serial::write(" root_cluster=");
    serial::write_dec(m->root_cluster);
    serial::write(" data_start=");
    serial::write_dec(m->data_start);
    serial::write_line("");

    // Build root VNode
    vfs::VNode* root = reinterpret_cast<vfs::VNode*>(
        heap::kmalloc(sizeof(vfs::VNode)));
    if (!root) { heap::kfree(m); return E_NOSPACE; }
    for (size_t i = 0; i < sizeof(vfs::VNode); i++)
        reinterpret_cast<uint8_t*>(root)[i] = 0;

    root->name[0] = '/'; root->name[1] = 0;
    root->type    = VFS_TYPE_DIR;
    root->ops     = &fat_ops;

    Fat32Node* fn = reinterpret_cast<Fat32Node*>(heap::kmalloc(sizeof(Fat32Node)));
    if (!fn) { heap::kfree(root); heap::kfree(m); return E_NOSPACE; }
    fn->mount         = m;
    fn->first_cluster = m->root_cluster;
    fn->is_dir        = true;
    fn->dirent_sector = 0;
    fn->dirent_offset = 0;
    fn->has_dirent    = false;
    root->priv        = fn;

    // Eagerly populate the VFS tree from FAT32 root
    populate_dir(m, m->root_cluster, root);

    // Graft into VFS at vfs_path
    return vfs::mount(vfs_path, root);
}

} // namespace fat32
