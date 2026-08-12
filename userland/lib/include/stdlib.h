#ifndef NAUMI_SHIM_STDLIB_H
#define NAUMI_SHIM_STDLIB_H

#include <stddef.h>

/* Real implementations live in userland/lib/libc.c. Only what
   third_party/doomgeneric actually needs (see the scoping notes in
   userland/doom/README.md) — this is not an attempt at a complete
   <stdlib.h>. Notably absent: rand/srand/qsort/strtol (unused — DOOM's
   own m_random.c has a deterministic table-based PRNG and never calls
   libc's). */

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

int abs(int n);
int atoi(const char *s);

/* Never returns. Exits the whole process via SYS_EXIT, same as
   sys_exit() — there's no notion of "returning to a caller" past this. */
void exit(int status);
void abort(void);

/* No environment variables exist; always NULL. */
char *getenv(const char *name);

/* No shell to exec anything in; always fails. */
int system(const char *command);

#endif
