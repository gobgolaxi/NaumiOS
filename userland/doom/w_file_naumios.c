#include <stddef.h>
#include "../lib/naumi.h"
#include "../lib/libc.h"
#include "../../third_party/doomgeneric/w_file.h"
#include "../../third_party/doomgeneric/z_zone.h"

/* WAD access backend for this platform, replacing w_file_stdc.c (which
   this port doesn't build): reads the whole WAD into a malloc'd buffer
   once via SYS_OPEN/SYS_READ, then answers every W_Read() as a plain
   memcpy from that buffer at the given offset. This sidesteps needing a
   real fseek()/ftell() — this project's SYS_READ only has a linear
   per-fd cursor (see sched_fd_read() in kernel/sched/sched.c), no seek
   syscall exists — the wad_file_class_t interface conveniently takes an
   explicit offset per read rather than assuming a persistent seek
   position, so "seeking" here is just indexing into memory. */

typedef struct {
    wad_file_t wad;
    unsigned char *data;
} naumios_wad_file_t;

extern wad_file_class_t naumios_wad_file;

static wad_file_t *W_Naumios_OpenFile(char *path) {
    long fd = sys_open(path);
    if (fd < 0) {
        return NULL;
    }

    size_t cap = 1024 * 1024;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) {
        sys_close(fd);
        return NULL;
    }

    size_t len = 0;
    for (;;) {
        if (len == cap) {
            size_t newcap = cap * 2;
            unsigned char *nb = (unsigned char *)realloc(buf, newcap);
            if (!nb) {
                free(buf);
                sys_close(fd);
                return NULL;
            }
            buf = nb;
            cap = newcap;
        }
        long got = sys_read(fd, buf + len, cap - len);
        if (got <= 0) {
            break; /* SYS_READ returns 0 once the fd's cursor reaches EOF */
        }
        len += (size_t)got;
    }
    sys_close(fd);

    naumios_wad_file_t *result = (naumios_wad_file_t *)Z_Malloc(sizeof(naumios_wad_file_t), PU_STATIC, 0);
    result->wad.file_class = &naumios_wad_file;
    result->wad.mapped = buf; /* genuinely resident in memory, not just seekable */
    result->wad.length = (unsigned int)len;
    result->data = buf;
    return &result->wad;
}

static void W_Naumios_CloseFile(wad_file_t *wad) {
    naumios_wad_file_t *f = (naumios_wad_file_t *)wad;
    free(f->data);
    Z_Free(f);
}

static size_t W_Naumios_Read(wad_file_t *wad, unsigned int offset, void *buffer, size_t buffer_len) {
    naumios_wad_file_t *f = (naumios_wad_file_t *)wad;
    if (offset >= f->wad.length) {
        return 0;
    }
    size_t avail = f->wad.length - offset;
    size_t n = buffer_len < avail ? buffer_len : avail;
    memcpy(buffer, f->data + offset, n);
    return n;
}

wad_file_class_t naumios_wad_file = {
    W_Naumios_OpenFile,
    W_Naumios_CloseFile,
    W_Naumios_Read,
};
