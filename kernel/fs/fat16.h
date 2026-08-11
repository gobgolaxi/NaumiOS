#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include <stddef.h>

/* Parses the boot sector at `partition_start_lba` (BPB + FAT16-specific
   extended fields) and validates it looks like FAT16. Returns 0 on
   success, -1 otherwise. Only ever mounts one filesystem at a time
   (matches this project's single-disk, single-partition setup so far). */
int fat16_mount(uint64_t partition_start_lba);

/* Visits every non-LFN, non-volume-label entry of the directory at `path`
   (e.g. "bin", "bin/sub"; NULL or "" means the root). `name` is
   "NAME.EXT" or "NAME" (trailing spaces trimmed, uppercase, as stored on
   disk). No-op (nothing visited) if `path` doesn't resolve to a directory. */
typedef void (*fat16_visitor_t)(const char *name, uint32_t size, int is_dir);
void fat16_list_dir(const char *path, fat16_visitor_t visit);

/* Looks up `path` (case-insensitive 8.3 per component, e.g. "hello.txt" or
   "bin/cat.elf") starting from the root, reads its full contents into a
   freshly kmalloc'd buffer, and writes the pointer/length to out_data and
   out_size. Caller kfree()s the returned pointer. Returns 0 on success, -1
   if any path component isn't found, a non-final component isn't a
   directory, or on I/O error. */
int fat16_read_file(const char *path, uint8_t **out_data, uint32_t *out_size);

#endif
