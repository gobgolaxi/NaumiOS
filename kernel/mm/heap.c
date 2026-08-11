#include <stdint.h>
#include <stddef.h>
#include "heap.h"
#include "pmm.h"

#define ALIGN 16UL
#define GROW_PAGES 4 /* pages requested per heap growth, when contiguous */

/* Address-ordered intrusive free list. Blocks are only ever appended (from
   heap_grow(), in increasing physical order — pmm never frees, so its
   bump-cursor is monotonic and later grows land at higher addresses) or
   split in place, so list order always matches address order. Coalescing
   in kfree() depends on that invariant. */
typedef struct block {
    size_t size; /* usable payload size, not including this header */
    int free;
    struct block *next;
    struct block *prev;
} block_t;

static block_t *heap_head;
static block_t *heap_tail;

static inline size_t align_up(size_t n, size_t a) {
    return (n + (a - 1)) & ~(a - 1);
}

static void append_block(block_t *b) {
    b->prev = heap_tail;
    b->next = NULL;
    if (heap_tail) {
        heap_tail->next = b;
    } else {
        heap_head = b;
    }
    heap_tail = b;
}

/* Requests enough pages from pmm to satisfy at least `min_size`, appending
   one new free block per contiguous physical run (pmm makes no contiguity
   guarantee across separate calls, so a break just starts a new block). */
static int heap_grow(size_t min_size) {
    size_t need_pages = (min_size + sizeof(block_t) + PAGE_SIZE - 1) / PAGE_SIZE;
    if (need_pages < GROW_PAGES) {
        need_pages = GROW_PAGES;
    }

    uint64_t hhdm = pmm_hhdm_offset();
    uint64_t run_start_phys = 0;
    uint64_t run_pages = 0;
    uint64_t prev_phys = 0;

    for (size_t i = 0; i < need_pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0) {
            break; /* out of physical memory; use whatever run we already have */
        }

        if (run_pages > 0 && phys == prev_phys + PAGE_SIZE) {
            run_pages++;
        } else {
            if (run_pages > 0) {
                block_t *b = (block_t *)(hhdm + run_start_phys);
                b->size = run_pages * PAGE_SIZE - sizeof(block_t);
                b->free = 1;
                append_block(b);
            }
            run_start_phys = phys;
            run_pages = 1;
        }
        prev_phys = phys;
    }

    if (run_pages > 0) {
        block_t *b = (block_t *)(hhdm + run_start_phys);
        b->size = run_pages * PAGE_SIZE - sizeof(block_t);
        b->free = 1;
        append_block(b);
        return 1;
    }
    return 0;
}

void heap_init(void) {
    heap_head = NULL;
    heap_tail = NULL;
}

void *kmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size = align_up(size, ALIGN);

    for (;;) {
        for (block_t *b = heap_head; b; b = b->next) {
            if (!b->free || b->size < size) {
                continue;
            }

            /* Split off the remainder if there's enough room for it to be
               a usable block on its own. */
            if (b->size >= size + sizeof(block_t) + ALIGN) {
                block_t *rem = (block_t *)((uint8_t *)b + sizeof(block_t) + size);
                rem->size = b->size - size - sizeof(block_t);
                rem->free = 1;
                rem->next = b->next;
                rem->prev = b;
                if (b->next) {
                    b->next->prev = rem;
                } else {
                    heap_tail = rem;
                }
                b->next = rem;
                b->size = size;
            }

            b->free = 0;
            return (void *)((uint8_t *)b + sizeof(block_t));
        }

        if (!heap_grow(size)) {
            return NULL; /* out of memory */
        }
    }
}

/* Merges `b` with its physically-adjacent next block, if that one's free
   too. Address-adjacency is checked explicitly rather than assumed from
   list-adjacency, so this can't wrongly merge across two separate
   heap_grow() runs that just happen to sit next to each other in the
   list. */
static void coalesce(block_t *b) {
    if (b->next && b->next->free &&
        (uint8_t *)b + sizeof(block_t) + b->size == (uint8_t *)b->next) {
        b->size += sizeof(block_t) + b->next->size;
        block_t *dead = b->next;
        b->next = dead->next;
        if (b->next) {
            b->next->prev = b;
        } else {
            heap_tail = b;
        }
    }
}

void kfree(void *ptr) {
    if (!ptr) {
        return;
    }
    block_t *b = (block_t *)((uint8_t *)ptr - sizeof(block_t));
    b->free = 1;
    coalesce(b);
    if (b->prev && b->prev->free) {
        coalesce(b->prev);
    }
}
