#ifndef NAUMI_USERLIBC_H
#define NAUMI_USERLIBC_H

#include <stddef.h>

/* A small freestanding libc subset for NaumiOS userland — this toolchain
   builds with -nostdlib -ffreestanding, so there is no real libc; these
   are actual implementations (userland/lib/libc.c), not declarations of
   something provided elsewhere. Every userland program gets this for
   free (see the Makefile: every .c file under userland/lib is compiled
   into every userland program's build alongside its own sources).

   malloc()/free() are backed by SYS_SBRK (see naumi.h) — a simple
   address-ordered free-list allocator, the same design as the kernel's
   own kmalloc() (kernel/mm/heap.c), just growing a per-process heap
   instead of physical memory directly. */

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int val, size_t n);
void *memmove(void *dst, const void *src, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
char *strcat(char *dst, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);

int abs(int n);
int atoi(const char *s);
void exit(int status);
void abort(void);
char *getenv(const char *name);
int system(const char *command);
int mkdir(const char *path, unsigned int mode);

#endif
