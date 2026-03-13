// fs/fat32.cpp - Knail FAT32 filesystem driver
#include "fat32.hpp"
#include "syscall.hpp"
#include "vfs.hpp"
#include "heap.hpp"
#include "serial.hpp"
#include "types.hpp"

namespace fat32 {

// ── BPB ───────────────────────────────────────────────────────────────────
struct [[gnu::packed]] BPB {
    u8   jmp[3];
    char oem[8];
    u16  bytes_per_sector;
    u8   sectors_per_cluster;
    u16  reserved_sectors;
    u8   num_fats;
    u16  root_entry_count;
    u16  total_sectors_16;
    u8   media_type;
    u16  fat_size_16;
    u16  sectors_per_track;
    u16  num_heads;
    u32  hidden_sectors;
    u32  total_sectors_32;
    // FAT32 extended
    u32  fat_size_32;
    u16  ext_flags;
    u16  fs_version;
    u32  root_cluster;
    u16  fs_info_sector;
    u16  backup_boot_sector;
    u8   reserved[12];
    u8   drive_number;
    u8   reserved1;
    u8   boot_signature;
    u32  volume_id;
    char volume_label[11];
    char fs_type[8];
};

// ── Directory entry ───────────────────────────────────────────────────────
struct [[gnu::packed]] DirEntry {
    char name[8];
    char ext[3];
    u8   attr;
    u8   reserved;
    u8   ctime_tenth;
    u16  ctime;
    u16  cdate;
    u16  adate;
    u16  cluster_hi;
    u16  mtime;
    u16  mdate;
    u16  cluster_lo;
    u32  size;
};

static constexpr u8 ATTR_READ_ONLY = 0x01;
static constexpr u8 ATTR_HIDDEN    = 0x02;
static constexpr u8 ATTR_SYSTEM    = 0x04;
static constexpr u8 ATTR_VOLUME_ID = 0x08;
static constexpr u8 ATTR_DIR       = 0x10;
static constexpr u8 ATTR_ARCHIVE   = 0x20;
static constexpr u8 ATTR_LFN       = 0x0F;
static constexpr u8 NTRES_LOWER_BASE = 0x08;
static constexpr u8 NTRES_LOWER_EXT  = 0x10;

// ── Mount state ───────────────────────────────────────────────────────────
struct Fat32Mount {
    block::BlockDevice* dev;
    u32 fat_start;
    u32 data_start;
    u32 sectors_per_cluster;
    u32 root_cluster;
    u32 total_clusters;
    u32 fat_size;
    u8  num_fats;
};

// ── VNode private data ────────────────────────────────────────────────────
struct Fat32Node {
    Fat32Mount* mount;
    u32         first_cluster;
    bool        is_dir;
    u32         dirent_sector;
    u32         dirent_offset;
    bool        has_dirent;
};

// ── Helpers ───────────────────────────────────────────────────────────────
static u32 cluster_to_lba(Fat32Mount* m, u32 cluster) {
    return m->data_start + (cluster - 2) * m->sectors_per_cluster;
}

// dev->read now returns kResult<i64> — helper returns raw bool for internal use
static bool dev_read(Fat32Mount* m, u32 lba, u8* buf) {
    return m->dev->read(m->dev, lba, 1, buf).is_ok();
}

static bool dev_write(Fat32Mount* m, u32 lba, const u8* buf) {
    return m->dev->write(m->dev, lba, 1, buf).is_ok();
}

static u32 read_fat(Fat32Mount* m, u32 cluster) {
    u32 fat_offset  = cluster * 4;
    u32 fat_sector  = m->fat_start + fat_offset / 512;
    u32 byte_offset = fat_offset % 512;
    u8  buf[512];
    if (!dev_read(m, fat_sector, buf)) return 0x0FFFFFFF;
    u32 val = *reinterpret_cast<u32*>(buf + byte_offset);
    return val & 0x0FFFFFFF;
}

static bool write_fat(Fat32Mount* m, u32 cluster, u32 value) {
    u32 fat_offset  = cluster * 4;
    u32 fat_sector  = m->fat_start + fat_offset / 512;
    u32 byte_offset = fat_offset % 512;
    u8  buf[512];
    if (!dev_read(m, fat_sector, buf)) return false;
    *reinterpret_cast<u32*>(buf + byte_offset) =
        (value & 0x0FFFFFFF) |
        (*reinterpret_cast<u32*>(buf + byte_offset) & 0xF0000000);
    for (u8 f = 0; f < m->num_fats; f++) {
        u32 lba = m->fat_start + f * m->fat_size + fat_offset / 512;
        if (!dev_write(m, lba, buf)) return false;
    }
    return true;
}

static u32 alloc_cluster(Fat32Mount* m) {
    for (u32 c = 2; c < m->total_clusters + 2; c++) {
        if (read_fat(m, c) == 0) {
            write_fat(m, c, 0x0FFFFFFF);
            return c;
        }
    }
    return 0;
}

static bool zero_cluster(Fat32Mount* m, u32 cluster) {
    u8  buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    u32 lba = cluster_to_lba(m, cluster);
    for (u32 s = 0; s < m->sectors_per_cluster; s++) {
        if (!dev_write(m, lba + s, buf)) return false;
    }
    return true;
}

static bool is_eoc(u32 val) { return (val & 0x0FFFFFFF) >= 0x0FFFFFF8; }

// ── 8.3 name helpers ──────────────────────────────────────────────────────
static void decode_83(const DirEntry& e, char* out) {
    int  i          = 0;
    bool lower_base = (e.reserved & NTRES_LOWER_BASE) != 0;
    bool lower_ext  = (e.reserved & NTRES_LOWER_EXT)  != 0;
    int name_len = 8;
    while (name_len > 0 && e.name[name_len - 1] == ' ') name_len--;
    for (int k = 0; k < name_len; k++) {
        char c = e.name[k];
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

static void encode_83(const char* name, char out_name[8], char out_ext[3], u8* out_ntres) {
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
    u8 ntres = 0;
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

// Forward declarations
static bool dir_find(Fat32Mount* m, u32 dir_cluster, const char* name,
                     DirEntry* out_de, u32* out_sector, u32* out_offset);
static vfs::VNode* make_vnode(Fat32Mount* m, const DirEntry& de,
                               u32 dirent_sector, u32 dirent_offset);

// ── Cluster chain read ────────────────────────────────────────────────────
static int64_t chain_read(Fat32Mount* m, u32 first_cluster,
                           u64 offset, void* buf, u64 len) {
    if (len == 0) return 0;
    u32  cluster_bytes = m->sectors_per_cluster * 512;
    u64  read_so_far   = 0;
    u32  cluster       = first_cluster;
    u64  cluster_idx   = offset / cluster_bytes;

    for (u64 i = 0; i < cluster_idx; i++) {
        cluster = read_fat(m, cluster);
        if (is_eoc(cluster) || cluster < 2) return (int64_t)read_so_far;
    }

    u64  intra = offset % cluster_bytes;
    u8*  dst   = reinterpret_cast<u8*>(buf);

    while (len > 0) {
        while (intra >= cluster_bytes) {
            intra  -= cluster_bytes;
            cluster = read_fat(m, cluster);
            if (is_eoc(cluster) || cluster < 2) return (int64_t)read_so_far;
        }
        if (is_eoc(cluster) || cluster < 2) break;

        u32 lba      = cluster_to_lba(m, cluster);
        u32 sec_off  = intra / 512;
        u32 byte_in  = intra % 512;
        u8  sector_buf[512];
        if (!dev_read(m, lba + sec_off, sector_buf)) break;

        u64 avail = 512 - byte_in;
        if (avail > len) avail = len;
        for (u64 i = 0; i < avail; i++) dst[read_so_far + i] = sector_buf[byte_in + i];
        read_so_far += avail;
        len         -= avail;
        intra       += avail;
    }
    return (int64_t)read_so_far;
}

// ── Cluster chain write ───────────────────────────────────────────────────
static int64_t chain_write(Fat32Mount* m, u32* first_cluster_ptr,
                            u64 offset, const void* buf, u64 len) {
    if (len == 0) return 0;
    u32 cluster_bytes = m->sectors_per_cluster * 512;
    u64 written       = 0;
    u32 cluster       = *first_cluster_ptr;

    if (cluster < 2) {
        cluster = alloc_cluster(m);
        if (!cluster) return E_NOSPACE;
        zero_cluster(m, cluster);
        *first_cluster_ptr = cluster;
    }

    u64 cluster_idx = offset / cluster_bytes;
    for (u64 i = 0; i < cluster_idx; i++) {
        u32 next = read_fat(m, cluster);
        if (is_eoc(next) || next < 2) {
            u32 newc = alloc_cluster(m);
            if (!newc) return E_NOSPACE;
            zero_cluster(m, newc);
            if (!write_fat(m, cluster, newc)) return E_INVAL;
            cluster = newc;
        } else {
            cluster = next;
        }
    }

    u64        intra = offset % cluster_bytes;
    const u8*  src   = reinterpret_cast<const u8*>(buf);

    while (len > 0) {
        u32 lba     = cluster_to_lba(m, cluster);
        u32 sec_off = intra / 512;
        u32 byte_in = intra % 512;
        u8  sector_buf[512];
        if (!dev_read(m, lba + sec_off, sector_buf)) return written;

        u64 avail = 512 - byte_in;
        if (avail > len) avail = len;
        for (u64 i = 0; i < avail; i++) sector_buf[byte_in + i] = src[written + i];
        if (!dev_write(m, lba + sec_off, sector_buf)) return written;

        written += avail;
        len     -= avail;
        intra   += avail;

        if (intra >= cluster_bytes && len > 0) {
            intra = 0;
            u32 next = read_fat(m, cluster);
            if (is_eoc(next) || next < 2) {
                u32 newc = alloc_cluster(m);
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

static bool write_dirent_at(Fat32Mount* m, u32 sector, u32 offset, const DirEntry& de) {
    u8 buf[512];
    if (!dev_read(m, sector, buf)) return false;
    *reinterpret_cast<DirEntry*>(buf + offset) = de;
    return dev_write(m, sector, buf);
}

static bool find_free_dirent(Fat32Mount* m, u32 dir_cluster,
                              u32* out_sector, u32* out_offset) {
    u32 cluster = dir_cluster;
    while (cluster >= 2) {
        u32 lba = cluster_to_lba(m, cluster);
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u8 buf[512];
            if (!dev_read(m, lba + s, buf)) return false;
            u32 eps = 512 / sizeof(DirEntry);
            for (u32 e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00 || (u8)de->name[0] == 0xE5) {
                    *out_sector = lba + s;
                    *out_offset = e * sizeof(DirEntry);
                    return true;
                }
            }
        }
        u32 next = read_fat(m, cluster);
        if (is_eoc(next) || next < 2) break;
        cluster = next;
    }
    u32 newc = alloc_cluster(m);
    if (!newc || !zero_cluster(m, newc) || !write_fat(m, cluster, newc))
        return false;
    *out_sector = cluster_to_lba(m, newc);
    *out_offset = 0;
    return true;
}

// ── VFS ops ───────────────────────────────────────────────────────────────
static kResult<i64> fat_read(vfs::VNode* node, u64 offset, void* buf, u64 len) {
    Fat32Node* fn = reinterpret_cast<Fat32Node*>(node->priv);
    if (offset >= node->size) return kResult<i64>::ok(0);
    u64 avail = node->size - offset;
    if (len > avail) len = avail;
    return kResult<i64>::ok(chain_read(fn->mount, fn->first_cluster, offset, buf, len));
}

static kResult<i64> fat_write(vfs::VNode* node, u64 offset, const void* buf, u64 len) {
    Fat32Node* fn      = reinterpret_cast<Fat32Node*>(node->priv);
    i64        written = chain_write(fn->mount, &fn->first_cluster, offset, buf, len);
    if (written <= 0) return kResult<i64>::err(kError::IOError);

    u64 end = offset + written;
    if (end > node->size) node->size = end;

    if (fn->has_dirent) {
        u8 sector_buf[512];
        if (dev_read(fn->mount, fn->dirent_sector, sector_buf)) {
            DirEntry* de = reinterpret_cast<DirEntry*>(sector_buf + fn->dirent_offset);
            de->size       = static_cast<u32>(node->size);
            de->cluster_hi = static_cast<u16>((fn->first_cluster >> 16) & 0xFFFF);
            de->cluster_lo = static_cast<u16>(fn->first_cluster & 0xFFFF);
            dev_write(fn->mount, fn->dirent_sector, sector_buf);
        }
    }
    return kResult<i64>::ok(written);
}

static vfs::VNode* fat_create(vfs::VNode* dir, const char* name, uint32_t type) {
    Fat32Node*  dfn = reinterpret_cast<Fat32Node*>(dir->priv);
    Fat32Mount* m   = dfn->mount;

    DirEntry existing{};
    u32 ex_sec = 0, ex_off = 0;
    if (dir_find(m, dfn->first_cluster, name, &existing, &ex_sec, &ex_off))
        return nullptr;

    u32 sec = 0, off = 0;
    if (!find_free_dirent(m, dfn->first_cluster, &sec, &off)) return nullptr;

    DirEntry de{};
    for (usize i = 0; i < sizeof(DirEntry); i++) reinterpret_cast<u8*>(&de)[i] = 0;
    encode_83(name, de.name, de.ext, &de.reserved);
    de.attr = (type == VFS_TYPE_DIR) ? ATTR_DIR : ATTR_ARCHIVE;
    de.size = 0;

    u32 first_cluster = 0;
    if (type == VFS_TYPE_DIR) {
        first_cluster = alloc_cluster(m);
        if (!first_cluster || !zero_cluster(m, first_cluster)) return nullptr;

        u8 buf[512];
        for (u32 i = 0; i < 512; i++) buf[i] = 0;
        DirEntry* dot    = reinterpret_cast<DirEntry*>(buf);
        DirEntry* dotdot = reinterpret_cast<DirEntry*>(buf + sizeof(DirEntry));
        for (int i = 0; i < 8; i++) { dot->name[i] = ' '; dotdot->name[i] = ' '; }
        for (int i = 0; i < 3; i++) { dot->ext[i]  = ' '; dotdot->ext[i]  = ' '; }
        dot->name[0] = '.';
        dotdot->name[0] = '.'; dotdot->name[1] = '.';
        dot->attr    = ATTR_DIR;
        dotdot->attr = ATTR_DIR;
        dot->cluster_hi    = static_cast<u16>((first_cluster >> 16) & 0xFFFF);
        dot->cluster_lo    = static_cast<u16>(first_cluster & 0xFFFF);
        u32 parent_cluster = dfn->first_cluster;
        dotdot->cluster_hi = static_cast<u16>((parent_cluster >> 16) & 0xFFFF);
        dotdot->cluster_lo = static_cast<u16>(parent_cluster & 0xFFFF);
        u32 lba = cluster_to_lba(m, first_cluster);
        if (!dev_write(m, lba, buf)) return nullptr;
    }

    de.cluster_hi = static_cast<u16>((first_cluster >> 16) & 0xFFFF);
    de.cluster_lo = static_cast<u16>(first_cluster & 0xFFFF);
    if (!write_dirent_at(m, sec, off, de)) return nullptr;

    return make_vnode(m, de, sec, off);
}

static kStatus fat_unlink(vfs::VNode* dir, const char* name) {
    Fat32Node*  dfn = reinterpret_cast<Fat32Node*>(dir->priv);
    Fat32Mount* m   = dfn->mount;

    DirEntry de{};
    u32 sec = 0, off = 0;
    if (!dir_find(m, dfn->first_cluster, name, &de, &sec, &off))
        return kStatus::err(kError::NotFound);
    if (de.attr & ATTR_DIR)
        return kStatus::err(kError::IsDirectory);

    u8 buf[512];
    if (!dev_read(m, sec, buf))  return kStatus::err(kError::IOError);
    buf[off] = 0xE5;
    if (!dev_write(m, sec, buf)) return kStatus::err(kError::IOError);

    u32 cl = ((u32)de.cluster_hi << 16) | de.cluster_lo;
    while (cl >= 2 && !is_eoc(cl)) {
        u32 next = read_fat(m, cl);
        if (!write_fat(m, cl, 0)) return kStatus::err(kError::IOError);
        cl = next;
    }
    if (cl >= 2 && is_eoc(cl)) {
        if (!write_fat(m, cl, 0)) return kStatus::err(kError::IOError);
    }
    return kStatus::ok();
}

static kStatus fat_readdir(vfs::VNode* node, u64 index, Dirent* out) {
    Fat32Node*  fn       = reinterpret_cast<Fat32Node*>(node->priv);
    Fat32Mount* m        = fn->mount;
    u64         real_idx = 0;
    u32         cluster  = fn->first_cluster;

    while (!is_eoc(cluster) && cluster >= 2) {
        u32 lba = cluster_to_lba(m, cluster);
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u8 buf[512];
            if (!dev_read(m, lba + s, buf)) return kStatus::err(kError::IOError);
            u32 eps = 512 / sizeof(DirEntry);
            for (u32 e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00)       return kStatus::err(kError::EndOfFile);
                if ((u8)de->name[0] == 0xE5)   continue;
                if (de->attr == ATTR_LFN)       continue;
                if (de->attr & ATTR_VOLUME_ID)  continue;
                if (de->name[0] == '.')         continue;
                if (real_idx == index) {
                    decode_83(*de, out->name);
                    out->type = (de->attr & ATTR_DIR) ? VFS_TYPE_DIR : VFS_TYPE_FILE;
                    return kStatus::ok();
                }
                real_idx++;
            }
        }
        cluster = read_fat(m, cluster);
    }
    return kStatus::err(kError::EndOfFile);
}

static void fat_close(vfs::VNode* /*node*/) {}

static vfs::FileOps fat_ops = {
    .read    = fat_read,
    .write   = fat_write,
    .close   = fat_close,
    .create  = fat_create,
    .unlink  = fat_unlink,
    .readdir = fat_readdir,
};

// ── dir_find ─────────────────────────────────────────────────────────────
static bool dir_find(Fat32Mount* m, u32 dir_cluster, const char* name,
                     DirEntry* out_de, u32* out_sector, u32* out_offset) {
    u32 cluster = dir_cluster;
    while (!is_eoc(cluster) && cluster >= 2) {
        u32 lba = cluster_to_lba(m, cluster);
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u8 buf[512];
            if (!dev_read(m, lba + s, buf)) return false;
            u32 eps = 512 / sizeof(DirEntry);
            for (u32 e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00)      return false;
                if ((u8)de->name[0] == 0xE5)  continue;
                if (de->attr == ATTR_LFN)      continue;
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

// ── make_vnode ────────────────────────────────────────────────────────────
static vfs::VNode* make_vnode(Fat32Mount* m, const DirEntry& de,
                               u32 dirent_sector, u32 dirent_offset) {
    char name[13];
    decode_83(de, name);

    vfs::VNode* node = reinterpret_cast<vfs::VNode*>(heap::kmalloc(sizeof(vfs::VNode)));
    if (!node) return nullptr;
    for (usize i = 0; i < sizeof(vfs::VNode); i++)
        reinterpret_cast<u8*>(node)[i] = 0;
    for (int i = 0; i < 127 && name[i]; i++) node->name[i] = name[i];

    u32  cluster = ((u32)de.cluster_hi << 16) | de.cluster_lo;
    bool is_dir  = (de.attr & ATTR_DIR) != 0;
    node->type   = is_dir ? VFS_TYPE_DIR : VFS_TYPE_FILE;
    node->size   = is_dir ? 0 : de.size;
    node->ops    = &fat_ops;

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

// ── populate_dir ─────────────────────────────────────────────────────────
static void populate_dir(Fat32Mount* m, u32 dir_cluster, vfs::VNode* parent) {
    u32 cluster = dir_cluster;
    while (!is_eoc(cluster) && cluster >= 2) {
        u32 lba = cluster_to_lba(m, cluster);
        for (u32 s = 0; s < m->sectors_per_cluster; s++) {
            u8 buf[512];
            if (!dev_read(m, lba + s, buf)) return;
            u32 eps = 512 / sizeof(DirEntry);
            for (u32 e = 0; e < eps; e++) {
                DirEntry* de = reinterpret_cast<DirEntry*>(buf) + e;
                if (de->name[0] == 0x00)          return;
                if ((u8)de->name[0] == 0xE5)      continue;
                if (de->attr == ATTR_LFN)          continue;
                if (de->attr & ATTR_VOLUME_ID)     continue;
                if (de->name[0] == '.')            continue;

                vfs::VNode* child = make_vnode(m, *de, lba + s, e * sizeof(DirEntry));
                if (!child) continue;

                child->parent       = parent;
                child->next_sibling = parent->children;
                parent->children    = child;

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
kStatus mount(block::BlockDevice* dev, const char* vfs_path) {
    u8 buf[512];
    if (dev->read(dev, 0, 1, buf).is_err()) {
        serial::write_line("[FAT32] failed to read BPB");
        return kStatus::err(kError::IOError);
    }

    BPB* bpb = reinterpret_cast<BPB*>(buf);
    if (bpb->bytes_per_sector != 512) {
        serial::write_line("[FAT32] unsupported sector size");
        return kStatus::err(kError::DeviceError);
    }
    if (bpb->fat_size_32 == 0) {
        serial::write_line("[FAT32] not FAT32 (fat_size_32=0)");
        return kStatus::err(kError::FilesystemCorrupt);
    }

    Fat32Mount* m = reinterpret_cast<Fat32Mount*>(heap::kmalloc(sizeof(Fat32Mount)));
    if (!m) return kStatus::err(kError::OutOfMemory);

    m->dev                 = dev;
    m->fat_start           = bpb->reserved_sectors;
    m->fat_size            = bpb->fat_size_32;
    m->num_fats            = bpb->num_fats;
    m->sectors_per_cluster = bpb->sectors_per_cluster;
    m->root_cluster        = bpb->root_cluster;
    m->data_start          = bpb->reserved_sectors + bpb->num_fats * bpb->fat_size_32;
    m->total_clusters      = (bpb->total_sectors_32 - m->data_start)
                           / bpb->sectors_per_cluster;

    serial::write("[FAT32] mounting on "); serial::write(vfs_path);
    serial::write(" root_cluster=");      serial::write_dec(m->root_cluster);
    serial::write(" data_start=");        serial::write_dec(m->data_start);
    serial::write_line("");

    // Build root VNode
    vfs::VNode* root = reinterpret_cast<vfs::VNode*>(heap::kmalloc(sizeof(vfs::VNode)));
    if (!root) { heap::kfree(m); return kStatus::err(kError::OutOfMemory); }
    for (usize i = 0; i < sizeof(vfs::VNode); i++)
        reinterpret_cast<u8*>(root)[i] = 0;
    root->name[0] = '/'; root->name[1] = 0;
    root->type    = VFS_TYPE_DIR;
    root->ops     = &fat_ops;

    Fat32Node* fn = reinterpret_cast<Fat32Node*>(heap::kmalloc(sizeof(Fat32Node)));
    if (!fn) { heap::kfree(root); heap::kfree(m); return kStatus::err(kError::OutOfMemory); }
    fn->mount         = m;
    fn->first_cluster = m->root_cluster;
    fn->is_dir        = true;
    fn->dirent_sector = 0;
    fn->dirent_offset = 0;
    fn->has_dirent    = false;
    root->priv        = fn;

    populate_dir(m, m->root_cluster, root);

    return vfs::mount(vfs_path, root);
}

} // namespace fat32
