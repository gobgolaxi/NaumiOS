#include <stdint.h>
#include <stddef.h>
#include "fat16.h"
#include "../drivers/virtio_blk.h"
#include "../mm/heap.h"

typedef struct __attribute__((packed)) {
    char name[8];
    char ext[3];
    uint8_t attr;
    uint8_t reserved;
    uint8_t create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t cluster_high; /* always 0 on real FAT16; FAT32-only field */
    uint16_t write_time;
    uint16_t write_date;
    uint16_t cluster_low;
    uint32_t file_size;
} fat_dirent_t;

#define ATTR_VOLUME_ID 0x08U
#define ATTR_DIRECTORY 0x10U
#define ATTR_LFN       0x0FU /* RO|HIDDEN|SYSTEM|VOLUME_ID all set */

#define DIRENT_FREE_REST 0x00U /* name[0]: no more entries after this */
#define DIRENT_DELETED   0xE5U

#define FAT16_EOC_MIN 0xFFF8U /* cluster values >= this mean end-of-chain */

#define MAX_NAME 13 /* "NAME.EXT\0" */
#define MAX_COMPONENT 13

static uint64_t partition_start;
static uint32_t bytes_per_sector;
static uint32_t sectors_per_cluster;
static uint32_t reserved_sectors;
static uint32_t num_fats;
static uint32_t root_entry_count;
static uint32_t fat_size16;

static uint64_t fat_start;      /* LBA, relative to partition_start already added */
static uint64_t root_dir_start;
static uint32_t root_dir_sectors;
static uint64_t data_start;

static int mounted;

static inline uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

int fat16_mount(uint64_t partition_start_lba) {
    mounted = 0;

    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return -1;
    }
    if (virtio_blk_read(partition_start_lba, buf, 1) != 0) {
        kfree(buf);
        return -1;
    }

    if (buf[510] != 0x55 || buf[511] != 0xAA) {
        kfree(buf);
        return -1;
    }

    bytes_per_sector = rd16(&buf[11]);
    sectors_per_cluster = buf[13];
    reserved_sectors = rd16(&buf[14]);
    num_fats = buf[16];
    root_entry_count = rd16(&buf[17]);
    fat_size16 = rd16(&buf[22]);

    kfree(buf);

    if (bytes_per_sector != VIRTIO_BLK_SECTOR_SIZE || sectors_per_cluster == 0 ||
        num_fats == 0 || root_entry_count == 0 || fat_size16 == 0) {
        return -1;
    }

    partition_start = partition_start_lba;
    fat_start = partition_start + reserved_sectors;
    root_dir_start = fat_start + (uint64_t)num_fats * fat_size16;
    root_dir_sectors = (root_entry_count * 32 + bytes_per_sector - 1) / bytes_per_sector;
    data_start = root_dir_start + root_dir_sectors;

    mounted = 1;
    return 0;
}

static uint64_t cluster_to_lba(uint32_t cluster) {
    return data_start + (uint64_t)(cluster - 2) * sectors_per_cluster;
}

static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t byte_off = cluster * 2;
    uint64_t sector = fat_start + byte_off / bytes_per_sector;
    uint32_t sector_off = byte_off % bytes_per_sector;

    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return FAT16_EOC_MIN; /* treat allocation failure as end-of-chain */
    }
    if (virtio_blk_read(sector, buf, 1) != 0) {
        kfree(buf);
        return FAT16_EOC_MIN;
    }

    uint32_t next = rd16(&buf[sector_off]);
    kfree(buf);
    return next;
}

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

/* Read-modify-write a single FAT16 entry. Mirrored to the second FAT copy
   too (num_fats is almost always 2 on a real FAT16 volume) — only the
   first copy is ever actually read back by this driver, but keeping both
   in sync is what every other FAT implementation does, and it's one extra
   sector write. */
static void fat_write_next_cluster(uint32_t cluster, uint16_t value) {
    uint32_t byte_off = cluster * 2;
    uint64_t sector = fat_start + byte_off / bytes_per_sector;
    uint32_t sector_off = byte_off % bytes_per_sector;

    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return;
    }
    if (virtio_blk_read(sector, buf, 1) != 0) {
        kfree(buf);
        return;
    }
    wr16(&buf[sector_off], value);
    virtio_blk_write(sector, buf, 1);
    if (num_fats > 1) {
        virtio_blk_write(sector + fat_size16, buf, 1);
    }
    kfree(buf);
}

/* Scans the FAT for the first free (0x0000) entry, marks it end-of-chain
   immediately (so a second call before the caller links it anywhere else
   doesn't hand out the same cluster twice), and returns its number, or 0
   if the volume is full. Linear from cluster 2 — this project's ~64MB
   image only has a few thousand clusters, and cluster allocation only
   happens for save files and small config writes, not a hot path. */
static uint32_t fat_alloc_cluster(void) {
    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return 0;
    }
    uint32_t entries_per_sector = bytes_per_sector / 2;

    for (uint32_t s = 0; s < fat_size16; s++) {
        if (virtio_blk_read(fat_start + s, buf, 1) != 0) {
            kfree(buf);
            return 0;
        }
        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint32_t cluster = s * entries_per_sector + i;
            if (cluster < 2) {
                continue; /* 0 and 1 are reserved, never real data clusters */
            }
            if (rd16(&buf[i * 2]) == 0x0000) {
                kfree(buf);
                fat_write_next_cluster(cluster, (uint16_t)FAT16_EOC_MIN);
                return cluster;
            }
        }
    }
    kfree(buf);
    return 0; /* volume full */
}

/* Writes `data`/`size` into a cluster chain, reusing `existing_first`'s
   clusters where possible (in order) and allocating new ones as needed;
   frees any leftover clusters if the chain was longer than the new data
   needs (overwriting with a smaller file). `existing_first` of 0 means
   "no existing chain, allocate one from scratch". Returns 0 on success
   (with `*out_first` set to the chain's first cluster) or -1 on failure —
   note a failure partway through can leave some clusters allocated but
   not linked into the final chain; this project has no fsck, and that's
   an acceptable state to leave a best-effort save write in. */
static int write_cluster_chain(uint32_t existing_first, const uint8_t *data, uint32_t size, uint32_t *out_first) {
    uint32_t cluster_bytes = sectors_per_cluster * bytes_per_sector;
    uint32_t needed = (size + cluster_bytes - 1) / cluster_bytes;
    if (needed == 0) {
        needed = 1; /* even a zero-byte file gets one cluster, for simplicity */
    }

    uint8_t *cbuf = kmalloc(cluster_bytes);
    if (!cbuf) {
        return -1;
    }

    uint32_t prev = 0;
    uint32_t cur = existing_first;
    uint32_t first = 0;
    uint32_t remaining = size;
    const uint8_t *src = data;

    for (uint32_t i = 0; i < needed; i++) {
        uint32_t c;
        if (cur >= 2 && cur < FAT16_EOC_MIN) {
            c = cur;
            cur = fat_next_cluster(cur);
        } else {
            c = fat_alloc_cluster();
            if (c == 0) {
                kfree(cbuf);
                return -1;
            }
        }
        if (first == 0) {
            first = c;
        }
        if (prev != 0) {
            fat_write_next_cluster(prev, (uint16_t)c);
        }

        uint32_t take = remaining < cluster_bytes ? remaining : cluster_bytes;
        uint32_t j = 0;
        for (; j < take; j++) {
            cbuf[j] = src[j];
        }
        for (; j < cluster_bytes; j++) {
            cbuf[j] = 0;
        }
        if (virtio_blk_write(cluster_to_lba(c), cbuf, sectors_per_cluster) != 0) {
            kfree(cbuf);
            return -1;
        }
        src += take;
        remaining -= take;
        prev = c;
    }
    fat_write_next_cluster(prev, (uint16_t)FAT16_EOC_MIN);

    /* Free whatever's left of a chain that was longer than we needed. */
    while (cur >= 2 && cur < FAT16_EOC_MIN) {
        uint32_t next = fat_next_cluster(cur);
        fat_write_next_cluster(cur, 0x0000);
        cur = next;
    }

    kfree(cbuf);
    *out_first = first;
    return 0;
}

/* "NAME    EXT" (11 raw bytes, space-padded) -> "NAME.EXT" or "NAME". */
static void format_name(const fat_dirent_t *e, char *out, size_t out_size) {
    size_t n = 0;
    for (int i = 0; i < 8 && e->name[i] != ' '; i++) {
        if (n + 1 < out_size) {
            out[n++] = e->name[i];
        }
    }
    if (e->ext[0] != ' ') {
        if (n + 1 < out_size) {
            out[n++] = '.';
        }
        for (int i = 0; i < 3 && e->ext[i] != ' '; i++) {
            if (n + 1 < out_size) {
                out[n++] = e->ext[i];
            }
        }
    }
    out[n] = '\0';
}

static int name_matches(const fat_dirent_t *e, const char *want) {
    char formatted[MAX_NAME];
    format_name(e, formatted, sizeof(formatted));

    const char *a = formatted;
    const char *b = want;
    while (*a && *b) {
        char ca = (*a >= 'a' && *a <= 'z') ? (char)(*a - 32) : *a;
        char cb = (*b >= 'a' && *b <= 'z') ? (char)(*b - 32) : *b;
        if (ca != cb) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == '\0' && *b == '\0';
}

/* A "directory" is either the root (fixed-size, its own reserved sectors —
   no cluster chain) or any other directory (stored exactly like a file's
   contents: a normal cluster chain of 32-byte entries). Every directory
   operation below funnels through this one iterator so root vs.
   subdirectory is a detail this function hides. `cb` returning nonzero
   stops iteration early (used by the "find one entry" callers). */
typedef int (*raw_visitor_t)(const fat_dirent_t *e, void *ctx);

static void iterate_dir(int is_root, uint32_t start_cluster, raw_visitor_t cb, void *ctx) {
    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return;
    }

    if (is_root) {
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            if (virtio_blk_read(root_dir_start + s, buf, 1) != 0) {
                break;
            }
            for (uint32_t i = 0; i < bytes_per_sector / sizeof(fat_dirent_t); i++) {
                const fat_dirent_t *e = (const fat_dirent_t *)(buf + i * sizeof(fat_dirent_t));
                if ((uint8_t)e->name[0] == DIRENT_FREE_REST) {
                    kfree(buf);
                    return;
                }
                if (cb(e, ctx)) {
                    kfree(buf);
                    return;
                }
            }
        }
    } else {
        uint32_t cluster = start_cluster;
        while (cluster >= 2 && cluster < FAT16_EOC_MIN) {
            for (uint32_t s = 0; s < sectors_per_cluster; s++) {
                if (virtio_blk_read(cluster_to_lba(cluster) + s, buf, 1) != 0) {
                    kfree(buf);
                    return;
                }
                for (uint32_t i = 0; i < bytes_per_sector / sizeof(fat_dirent_t); i++) {
                    const fat_dirent_t *e = (const fat_dirent_t *)(buf + i * sizeof(fat_dirent_t));
                    if ((uint8_t)e->name[0] == DIRENT_FREE_REST) {
                        kfree(buf);
                        return;
                    }
                    if (cb(e, ctx)) {
                        kfree(buf);
                        return;
                    }
                }
            }
            cluster = fat_next_cluster(cluster);
        }
    }

    kfree(buf);
}

typedef struct {
    const char *want;
    fat_dirent_t result;
    int found;
} find_ctx_t;

static int find_cb(const fat_dirent_t *e, void *ctx_) {
    find_ctx_t *ctx = (find_ctx_t *)ctx_;
    uint8_t first = (uint8_t)e->name[0];
    if (first == DIRENT_DELETED || e->attr == ATTR_LFN || (e->attr & ATTR_VOLUME_ID)) {
        return 0;
    }
    if (name_matches(e, ctx->want)) {
        ctx->result = *e;
        ctx->found = 1;
        return 1;
    }
    return 0;
}

static int find_in_dir(int is_root, uint32_t start_cluster, const char *name, fat_dirent_t *out) {
    find_ctx_t ctx = { .want = name, .found = 0 };
    iterate_dir(is_root, start_cluster, find_cb, &ctx);
    if (!ctx.found) {
        return -1;
    }
    *out = ctx.result;
    return 0;
}

/* Splits `path` on '/' and walks it from the root, descending into each
   non-final component (which must be a directory) and looking the final
   component up in whatever directory that lands in. Empty path (after
   stripping leading slashes) fails — callers needing "root itself" handle
   that case separately (see fat16_list_dir). */
static int resolve_path(const char *path, fat_dirent_t *out) {
    int is_root = 1;
    uint32_t dir_cluster = 0;

    const char *p = path;
    if (p[0] == '.' && p[1] == '/') {
        p += 2; /* DOOM's own paths are all "./name" (configdir="."), a
                    plain relative name is what we actually store */
    }
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return -1;
    }

    for (;;) {
        char component[MAX_COMPONENT];
        size_t n = 0;
        while (p[n] && p[n] != '/' && n < sizeof(component) - 1) {
            component[n] = p[n];
            n++;
        }
        component[n] = '\0';
        p += n;
        while (*p == '/') {
            p++;
        }

        fat_dirent_t e;
        if (find_in_dir(is_root, dir_cluster, component, &e) != 0) {
            return -1;
        }

        if (*p == '\0') {
            *out = e;
            return 0;
        }
        if (!(e.attr & ATTR_DIRECTORY)) {
            return -1; /* tried to descend through a file */
        }
        is_root = 0;
        dir_cluster = e.cluster_low;
    }
}

/* Splits off the final path component (the "leaf") and resolves
   everything before it as a directory (root if there's nothing before
   it) — the shape every write operation needs: "where does this go, and
   under what name". `leaf_out` points into a caller-owned buffer, not
   into `path`, since callers uppercase/reuse it afterward. Returns -1 if
   a non-final component doesn't exist or isn't a directory, or the path
   is empty/root itself (nothing to create *as* — every write needs a
   name). */
static int resolve_parent(const char *path, int *is_root_out, uint32_t *dir_cluster_out,
                           char *leaf_out, size_t leaf_cap) {
    const char *p = path;
    if (p[0] == '.' && p[1] == '/') {
        p += 2;
    }
    while (*p == '/') {
        p++;
    }
    if (*p == '\0') {
        return -1;
    }

    int is_root = 1;
    uint32_t dir_cluster = 0;

    for (;;) {
        char component[MAX_COMPONENT];
        size_t n = 0;
        while (p[n] && p[n] != '/' && n < sizeof(component) - 1) {
            component[n] = p[n];
            n++;
        }
        component[n] = '\0';
        const char *rest = p + n;
        while (*rest == '/') {
            rest++;
        }

        if (*rest == '\0') {
            size_t m = 0;
            for (; component[m] != '\0' && m < leaf_cap - 1; m++) {
                leaf_out[m] = component[m];
            }
            leaf_out[m] = '\0';
            *is_root_out = is_root;
            *dir_cluster_out = dir_cluster;
            return 0;
        }

        fat_dirent_t e;
        if (find_in_dir(is_root, dir_cluster, component, &e) != 0 || !(e.attr & ATTR_DIRECTORY)) {
            return -1;
        }
        is_root = 0;
        dir_cluster = e.cluster_low;
        p = rest;
    }
}

typedef enum { SCAN_CONTINUE, SCAN_MATCH, SCAN_FREE, SCAN_ERROR } scan_result_t;

/* Scans one directory sector for either a name match or the first free
   (never-used or deleted) slot. Free-slot detection doesn't keep scanning
   the rest of the sector for a *later* match past a deleted (0xE5) entry
   — this project has no delete/unlink, so 0xE5 entries never actually
   occur here; only the 0x00 "nothing after this was ever used" case
   matters in practice, and that one genuinely does mean stop. */
static scan_result_t scan_sector_for_dirent(uint8_t *buf, uint64_t lba, const char *name,
                                             fat_dirent_t *match_out, uint32_t *offset_out) {
    if (virtio_blk_read(lba, buf, 1) != 0) {
        return SCAN_ERROR;
    }
    for (uint32_t i = 0; i < bytes_per_sector / sizeof(fat_dirent_t); i++) {
        const fat_dirent_t *e = (const fat_dirent_t *)(buf + i * sizeof(fat_dirent_t));
        uint8_t first = (uint8_t)e->name[0];
        if (first == DIRENT_FREE_REST || first == DIRENT_DELETED) {
            *offset_out = i * (uint32_t)sizeof(fat_dirent_t);
            return SCAN_FREE;
        }
        if (e->attr == ATTR_LFN || (e->attr & ATTR_VOLUME_ID)) {
            continue;
        }
        if (name_matches(e, name)) {
            *match_out = *e;
            *offset_out = i * (uint32_t)sizeof(fat_dirent_t);
            return SCAN_MATCH;
        }
    }
    return SCAN_CONTINUE;
}

/* Finds `name` in the directory at (is_root, dir_cluster), or — if it
   doesn't exist — the sector/offset of a free slot to create it at,
   extending the directory's own cluster chain with a fresh zeroed
   cluster if none of its existing sectors have room (root can't be
   extended — it's a fixed-size run reserved at format time; returns -1
   if it's completely full). Either way, the caller gets back exactly
   "which 32-byte slot do I read-modify-write" — *out_existed
   distinguishes an update from a fresh create. */
static int find_or_create_dirent(int is_root, uint32_t dir_cluster, const char *name,
                                  fat_dirent_t *out_existing, int *out_existed,
                                  uint64_t *out_sector, uint32_t *out_offset) {
    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return -1;
    }

    uint64_t free_lba = 0;
    uint32_t free_off = 0;
    int have_free = 0;

    if (is_root) {
        for (uint32_t s = 0; s < root_dir_sectors; s++) {
            uint64_t lba = root_dir_start + s;
            fat_dirent_t match;
            uint32_t off;
            scan_result_t r = scan_sector_for_dirent(buf, lba, name, &match, &off);
            if (r == SCAN_ERROR) {
                kfree(buf);
                return -1;
            }
            if (r == SCAN_MATCH) {
                *out_existing = match;
                *out_existed = 1;
                *out_sector = lba;
                *out_offset = off;
                kfree(buf);
                return 0;
            }
            if (r == SCAN_FREE && !have_free) {
                have_free = 1;
                free_lba = lba;
                free_off = off;
            }
        }
        if (!have_free) {
            kfree(buf);
            return -1; /* root directory full */
        }
        *out_existed = 0;
        *out_sector = free_lba;
        *out_offset = free_off;
        kfree(buf);
        return 0;
    }

    uint32_t cluster = dir_cluster;
    uint32_t prev_cluster = 0;
    while (cluster >= 2 && cluster < FAT16_EOC_MIN) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint64_t lba = cluster_to_lba(cluster) + s;
            fat_dirent_t match;
            uint32_t off;
            scan_result_t r = scan_sector_for_dirent(buf, lba, name, &match, &off);
            if (r == SCAN_ERROR) {
                kfree(buf);
                return -1;
            }
            if (r == SCAN_MATCH) {
                *out_existing = match;
                *out_existed = 1;
                *out_sector = lba;
                *out_offset = off;
                kfree(buf);
                return 0;
            }
            if (r == SCAN_FREE && !have_free) {
                have_free = 1;
                free_lba = lba;
                free_off = off;
            }
        }
        prev_cluster = cluster;
        cluster = fat_next_cluster(cluster);
    }

    if (!have_free) {
        uint32_t newc = fat_alloc_cluster();
        if (newc == 0) {
            kfree(buf);
            return -1;
        }
        for (uint32_t i = 0; i < bytes_per_sector; i++) {
            buf[i] = 0;
        }
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (virtio_blk_write(cluster_to_lba(newc) + s, buf, 1) != 0) {
                kfree(buf);
                return -1;
            }
        }
        fat_write_next_cluster(newc, (uint16_t)FAT16_EOC_MIN);
        if (prev_cluster != 0) {
            fat_write_next_cluster(prev_cluster, (uint16_t)newc);
        }
        have_free = 1;
        free_lba = cluster_to_lba(newc);
        free_off = 0;
    }
    *out_existed = 0;
    *out_sector = free_lba;
    *out_offset = free_off;
    kfree(buf);
    return 0;
}

/* Fills in the 8.3 name/ext fields (uppercased, space-padded) and the
   fixed timestamp/attribute fields a real FAT16 entry needs but this
   project's own reader never looks at. Does *not* touch cluster_low or
   file_size — callers set those themselves (the two things that actually
   differ between a fresh file and a directory). */
static void init_dirent_name(fat_dirent_t *e, const char *leaf) {
    for (int i = 0; i < 8; i++) {
        e->name[i] = ' ';
    }
    for (int i = 0; i < 3; i++) {
        e->ext[i] = ' ';
    }
    /* A dot at position 0 (a Unix-style ".name" dotfile) is kept as part
       of the name rather than treated as the name/ext separator — this
       driver never receives one in practice (see the NaumiOS port note in
       third_party/doomgeneric/m_config.c next to SAVEGAME/), but doing it
       this way rather than silently truncating to an empty name costs
       nothing and avoids a landmine if that ever changes. */
    int ni = 0;
    const char *s = leaf;
    if (*s == '.') {
        e->name[ni++] = '.';
        s++;
    }
    for (; *s && *s != '.' && ni < 8; s++) {
        e->name[ni++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
    }
    while (*s && *s != '.') {
        s++;
    }
    if (*s == '.') {
        s++;
        int ei = 0;
        for (; *s && ei < 3; s++) {
            e->ext[ei++] = (*s >= 'a' && *s <= 'z') ? (char)(*s - 32) : *s;
        }
    }
    e->reserved = 0;
    e->create_time_tenth = 0;
    e->create_time = 0;
    e->create_date = 0;
    e->access_date = 0;
    e->cluster_high = 0;
    e->write_time = 0;
    e->write_date = 0;
}

int fat16_write_file(const char *path, const uint8_t *data, uint32_t size) {
    if (!mounted) {
        return -1;
    }

    int is_root;
    uint32_t dir_cluster;
    char leaf[MAX_COMPONENT];
    if (resolve_parent(path, &is_root, &dir_cluster, leaf, sizeof(leaf)) != 0) {
        return -1;
    }

    fat_dirent_t existing;
    int existed;
    uint64_t sector;
    uint32_t offset;
    if (find_or_create_dirent(is_root, dir_cluster, leaf, &existing, &existed, &sector, &offset) != 0) {
        return -1;
    }
    if (existed && (existing.attr & ATTR_DIRECTORY)) {
        return -1; /* can't overwrite a directory with a file */
    }

    uint32_t existing_first = existed ? existing.cluster_low : 0;
    uint32_t new_first;
    if (write_cluster_chain(existing_first, data, size, &new_first) != 0) {
        return -1;
    }

    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return -1;
    }
    if (virtio_blk_read(sector, buf, 1) != 0) {
        kfree(buf);
        return -1;
    }
    fat_dirent_t *e = (fat_dirent_t *)(buf + offset);
    if (!existed) {
        init_dirent_name(e, leaf);
        e->attr = 0;
    }
    e->cluster_low = (uint16_t)new_first;
    e->file_size = size;

    int ok = virtio_blk_write(sector, buf, 1) == 0;
    kfree(buf);
    return ok ? 0 : -1;
}

int fat16_mkdir(const char *path) {
    if (!mounted) {
        return -1;
    }

    int is_root;
    uint32_t dir_cluster;
    char leaf[MAX_COMPONENT];
    if (resolve_parent(path, &is_root, &dir_cluster, leaf, sizeof(leaf)) != 0) {
        return -1;
    }

    fat_dirent_t existing;
    int existed;
    uint64_t sector;
    uint32_t offset;
    if (find_or_create_dirent(is_root, dir_cluster, leaf, &existing, &existed, &sector, &offset) != 0) {
        return -1;
    }
    if (existed) {
        return (existing.attr & ATTR_DIRECTORY) ? 0 : -1; /* mkdir-if-missing: fine if it's already a dir */
    }

    uint32_t newc = fat_alloc_cluster();
    if (newc == 0) {
        return -1;
    }
    uint8_t *zero = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!zero) {
        return -1;
    }
    for (uint32_t i = 0; i < bytes_per_sector; i++) {
        zero[i] = 0;
    }
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (virtio_blk_write(cluster_to_lba(newc) + s, zero, 1) != 0) {
            kfree(zero);
            return -1;
        }
    }
    kfree(zero);
    fat_write_next_cluster(newc, (uint16_t)FAT16_EOC_MIN);

    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return -1;
    }
    if (virtio_blk_read(sector, buf, 1) != 0) {
        kfree(buf);
        return -1;
    }
    fat_dirent_t *e = (fat_dirent_t *)(buf + offset);
    init_dirent_name(e, leaf);
    e->attr = ATTR_DIRECTORY;
    e->cluster_low = (uint16_t)newc;
    e->file_size = 0;

    int ok = virtio_blk_write(sector, buf, 1) == 0;
    kfree(buf);
    return ok ? 0 : -1;
}

typedef struct {
    fat16_visitor_t visit;
} list_ctx_t;

static int list_cb(const fat_dirent_t *e, void *ctx_) {
    list_ctx_t *ctx = (list_ctx_t *)ctx_;
    uint8_t first = (uint8_t)e->name[0];
    if (first == DIRENT_DELETED || e->attr == ATTR_LFN || (e->attr & ATTR_VOLUME_ID)) {
        return 0;
    }
    char name[MAX_NAME];
    format_name(e, name, sizeof(name));
    ctx->visit(name, e->file_size, (e->attr & ATTR_DIRECTORY) != 0);
    return 0;
}

void fat16_list_dir(const char *path, fat16_visitor_t visit) {
    if (!mounted) {
        return;
    }

    int is_root = 1;
    uint32_t cluster = 0;

    if (path && path[0] != '\0') {
        fat_dirent_t e;
        if (resolve_path(path, &e) != 0 || !(e.attr & ATTR_DIRECTORY)) {
            return;
        }
        is_root = 0;
        cluster = e.cluster_low;
    }

    list_ctx_t ctx = { .visit = visit };
    iterate_dir(is_root, cluster, list_cb, &ctx);
}

int fat16_read_file(const char *path, uint8_t **out_data, uint32_t *out_size) {
    if (!mounted) {
        return -1;
    }

    fat_dirent_t e;
    if (resolve_path(path, &e) != 0 || (e.attr & ATTR_DIRECTORY)) {
        return -1;
    }

    uint32_t size = e.file_size;
    uint8_t *data = kmalloc(size > 0 ? size : 1);
    if (!data) {
        return -1;
    }

    uint32_t cluster = e.cluster_low;
    uint32_t cluster_bytes = sectors_per_cluster * bytes_per_sector;
    uint32_t remaining = size;
    uint8_t *cursor = data;

    uint8_t *cbuf = kmalloc(cluster_bytes);
    if (!cbuf) {
        kfree(data);
        return -1;
    }

    while (remaining > 0 && cluster >= 2 && cluster < FAT16_EOC_MIN) {
        if (virtio_blk_read(cluster_to_lba(cluster), cbuf, sectors_per_cluster) != 0) {
            kfree(cbuf);
            kfree(data);
            return -1;
        }

        uint32_t take = remaining < cluster_bytes ? remaining : cluster_bytes;
        for (uint32_t i = 0; i < take; i++) {
            cursor[i] = cbuf[i];
        }
        cursor += take;
        remaining -= take;

        cluster = fat_next_cluster(cluster);
    }

    kfree(cbuf);

    *out_data = data;
    *out_size = size;
    return 0;
}
