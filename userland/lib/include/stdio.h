#ifndef NAUMI_SHIM_STDIO_H
#define NAUMI_SHIM_STDIO_H

#include <stddef.h>
#include <stdarg.h>

/* A small <stdio.h> for third-party sources (third_party/doomgeneric) that
   expect one — this project has no real filesystem writes yet (FAT16 is
   read-only, see kernel/fs/fat16.c) and no seekable read syscall (SYS_READ
   has a linear per-fd cursor only, see sched_fd_read()), so this is
   deliberately narrower than a real libc:
   - fopen() in a read mode opens a real file via SYS_OPEN; write modes
     succeed against an in-memory buffer that's simply discarded on
     fclose() — "writes never fail" without persisting anything, matching
     this project's current storage capabilities. Good enough for DOOM's
     config/savegame paths, which already tolerate a missing/failed file.
   - fseek()/ftell() are unimplemented no-ops (ftell always returns 0).
     Verified safe for this port: the one real caller that needed true
     seeking (stdio-backed WAD access) is replaced entirely by
     userland/doom/w_file_naumios.c, which reads the whole WAD into memory
     once and answers every "seek+read" as a plain memory offset instead.
     Nothing else in the files this port actually compiles calls fseek()
     on a file that exists. */

typedef struct FILE FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define EOF (-1)
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *f);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f);
int fseek(FILE *f, long offset, int whence);
long ftell(FILE *f);
int fflush(FILE *f);
int fgetc(FILE *f);
char *fgets(char *buf, int size, FILE *f);
int fputc(int c, FILE *f);
int fputs(const char *s, FILE *f);
int feof(FILE *f);
int putchar(int c);
int puts(const char *s);

int remove(const char *path);
int rename(const char *oldpath, const char *newpath);

int printf(const char *fmt, ...);
int fprintf(FILE *f, const char *fmt, ...);
int sprintf(char *buf, const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
int vfprintf(FILE *f, const char *fmt, va_list ap);
int vsprintf(char *buf, const char *fmt, va_list ap);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* Narrow: %d/%i (with 0x-prefix hex for %i)/%x/%X/%u/%c/%s/%% and literal
   text/whitespace matching — everything third_party/doomgeneric's config
   parser (its only caller) actually needs. No width/length modifiers, no
   %f. */
int sscanf(const char *str, const char *fmt, ...);

#endif
