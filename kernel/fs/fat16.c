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
