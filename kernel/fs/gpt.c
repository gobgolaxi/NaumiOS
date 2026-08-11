#include <stdint.h>
#include <stddef.h>
#include "gpt.h"
#include "../drivers/virtio_blk.h"
#include "../mm/heap.h"

typedef struct __attribute__((packed)) {
    char signature[8]; /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t disk_guid[16];
    uint64_t partition_entry_lba;
    uint32_t num_partition_entries;
    uint32_t partition_entry_size;
    uint32_t partition_array_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t type_guid[16];
    uint8_t unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attributes;
    uint16_t name_utf16[36];
} gpt_entry_t;

static int sig_ok(const char *sig) {
    static const char expect[8] = { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' };
    for (int i = 0; i < 8; i++) {
        if (sig[i] != expect[i]) {
            return 0;
        }
    }
    return 1;
}

uint64_t gpt_find_first_partition_lba(void) {
    uint8_t *buf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!buf) {
        return 0;
    }

    if (virtio_blk_read(1, buf, 1) != 0) {
        kfree(buf);
        return 0;
    }

    gpt_header_t *hdr = (gpt_header_t *)buf;
    if (!sig_ok(hdr->signature)) {
        kfree(buf);
        return 0;
    }

    uint64_t entry_lba = hdr->partition_entry_lba;
    uint32_t entry_size = hdr->partition_entry_size;
    kfree(buf);

    if (entry_size == 0 || entry_size > VIRTIO_BLK_SECTOR_SIZE) {
        return 0;
    }

    uint8_t *ebuf = kmalloc(VIRTIO_BLK_SECTOR_SIZE);
    if (!ebuf) {
        return 0;
    }
    if (virtio_blk_read(entry_lba, ebuf, 1) != 0) {
        kfree(ebuf);
        return 0;
    }

    gpt_entry_t *entry = (gpt_entry_t *)ebuf;
    uint64_t start = entry->first_lba;
    kfree(ebuf);
    return start;
}
