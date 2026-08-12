#include <stddef.h>
#include "libc.h"
#include "naumi.h"

int errno = 0; /* see include/errno.h — never set to anything else */

#define ALIGN 16UL
#define GROW_MIN (16UL * 1024) /* bytes requested per heap growth, minimum */

/* Address-ordered intrusive free list — identical design to the kernel's
   own kmalloc() (kernel/mm/heap.c), just growing this process's own heap
   (via SYS_SBRK) instead of physical memory directly. Blocks are only
   ever appended (from heap_grow(), in increasing address order — sbrk()
   is monotonic) or split in place, so list order always matches address
   order; coalesce() in free() depends on that. */
typedef struct block {
    size_t size; /* usable payload size, not including this header */
    int free;
    struct block *next;
    struct block *prev;
} block_t;

static block_t *heap_head;
static block_t *heap_tail;

static size_t align_up(size_t n, size_t a) {
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

static int heap_grow(size_t min_size) {
    size_t want = min_size + sizeof(block_t);
    if (want < GROW_MIN) {
        want = GROW_MIN;
    }
    long base = sys_sbrk((long)want);
    if (base < 0) {
        return 0;
    }
    block_t *b = (block_t *)(void *)(unsigned long)base;
    b->size = want - sizeof(block_t);
    b->free = 1;
    append_block(b);
    return 1;
}

void *malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }
    size = align_up(size, ALIGN);

    for (;;) {
        for (block_t *b = heap_head; b; b = b->next) {
            if (!b->free || b->size < size) {
                continue;
            }

            if (b->size >= size + sizeof(block_t) + ALIGN) {
                block_t *rem = (block_t *)((unsigned char *)b + sizeof(block_t) + size);
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
            return (void *)((unsigned char *)b + sizeof(block_t));
        }

        if (!heap_grow(size)) {
            return NULL; /* out of memory */
        }
    }
}

static void coalesce(block_t *b) {
    if (b->next && b->next->free &&
        (unsigned char *)b + sizeof(block_t) + b->size == (unsigned char *)b->next) {
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

void free(void *ptr) {
    if (!ptr) {
        return;
    }
    block_t *b = (block_t *)((unsigned char *)ptr - sizeof(block_t));
    b->free = 1;
    coalesce(b);
    if (b->prev && b->prev->free) {
        coalesce(b->prev);
    }
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }
    block_t *b = (block_t *)((unsigned char *)ptr - sizeof(block_t));
    if (b->size >= size) {
        return ptr;
    }
    void *n = malloc(size);
    if (!n) {
        return NULL;
    }
    memcpy(n, ptr, b->size);
    free(ptr);
    return n;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void *memset(void *dst, int val, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    for (size_t i = 0; i < n; i++) {
        d[i] = (unsigned char)val;
    }
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    if (d == s || n == 0) {
        return dst;
    }
    if (d < s) {
        for (size_t i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (size_t i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *pa = (const unsigned char *)a;
    const unsigned char *pb = (const unsigned char *)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) {
            return (int)pa[i] - (int)pb[i];
        }
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) {
        n++;
    }
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || a[i] == '\0') {
            return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        }
    }
    return 0;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) { }
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strcat(char *dst, const char *src) {
    strcpy(dst + strlen(dst), src);
    return dst;
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c) {
            return (void *)(p + i);
        }
    }
    return NULL;
}

char *strchr(const char *s, int c) {
    for (; *s; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
    }
    return (c == '\0') ? (char *)s : NULL;
}

char *strrchr(const char *s, int c) {
    const char *last = (c == '\0') ? s + strlen(s) : NULL;
    for (; *s; s++) {
        if (*s == (char)c) {
            last = s;
        }
    }
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) {
        return (char *)haystack;
    }
    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) {
            return (char *)haystack;
        }
    }
    return NULL;
}

char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) {
        memcpy(p, s, n);
    }
    return p;
}

static int lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = lower((unsigned char)a[i]);
        int cb = lower((unsigned char)b[i]);
        if (ca != cb || a[i] == '\0') {
            return ca - cb;
        }
    }
    return 0;
}

int abs(int n) {
    return n < 0 ? -n : n;
}

int atoi(const char *s) {
    int sign = 1;
    long v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') {
        s++;
    }
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return (int)(sign * v);
}

void exit(int status) {
    (void)status;
    sys_exit();
}

void abort(void) {
    sys_exit();
}

char *getenv(const char *name) {
    (void)name;
    return NULL; /* no environment variables exist */
}

int system(const char *command) {
    (void)command;
    return -1; /* no shell to exec anything in */
}

int mkdir(const char *path, unsigned int mode) {
    (void)mode;
    return (int)sys_mkdir(path);
}
