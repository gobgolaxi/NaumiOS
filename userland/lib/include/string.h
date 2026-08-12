#ifndef NAUMI_SHIM_STRING_H
#define NAUMI_SHIM_STRING_H

#include <stddef.h>

/* Real implementations live in userland/lib/libc.c — this just declares
   them under the standard header name so third-party sources (e.g.
   third_party/doomgeneric) that `#include <string.h>` find them. */

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

#endif
