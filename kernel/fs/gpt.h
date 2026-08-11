#ifndef GPT_H
#define GPT_H

#include <stdint.h>

/* Reads the GPT header (LBA 1) and the first partition table entry.
   Returns that partition's starting LBA, or 0 if there's no valid GPT
   signature or no partitions. Good enough for our own single-ESP-partition
   image (scripts/make-image.sh); a multi-partition disk would need a real
   type-GUID/name lookup instead of just "entry 0". */
uint64_t gpt_find_first_partition_lba(void);

#endif
