#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>
#include "include/stdio.h"
#include "libc.h"
#include "naumi.h"

/* See include/stdio.h for what this deliberately does and doesn't support. */

#define WRITE_BUF_GROW 4096
#define FILE_PATH_MAX 64

struct FILE {
    int is_console; /* stdout/stderr: goes straight to SYS_WRITE(1, ...) */
    int is_write;   /* fopen()'d in a write mode */
    long fd;        /* SYS_OPEN fd, read mode only */
    unsigned char *wbuf; /* malloc'd, write mode only — flushed to disk on fclose() */
    size_t wlen, wcap;
    char path[FILE_PATH_MAX]; /* write mode only — fclose() needs it for SYS_FILE_WRITE */
    int eof;
};

static FILE console_stdout = { .is_console = 1, .fd = -1 };
static FILE console_stderr = { .is_console = 1, .fd = -1 };
FILE *stdout = &console_stdout;
FILE *stderr = &console_stderr;
FILE *stdin = NULL; /* nothing reads from it in this port */

static int mode_is_write(const char *mode) {
    return mode && (mode[0] == 'w' || mode[0] == 'a');
}

FILE *fopen(const char *path, const char *mode) {
    FILE *f = (FILE *)malloc(sizeof(FILE));
    if (!f) {
        return NULL;
    }
    f->is_console = 0;
    f->eof = 0;

    if (mode_is_write(mode)) {
        f->is_write = 1;
        f->fd = -1;
        f->wbuf = NULL;
        f->wlen = 0;
        f->wcap = 0;
        size_t n = 0;
        for (; path[n] != '\0' && n < FILE_PATH_MAX - 1; n++) {
            f->path[n] = path[n];
        }
        f->path[n] = '\0';
        return f;
    }

    long fd = sys_open(path);
    if (fd < 0) {
        free(f);
        return NULL;
    }
    f->is_write = 0;
    f->fd = fd;
    f->wbuf = NULL;
    return f;
}

int fclose(FILE *f) {
    if (!f || f->is_console) {
        return 0;
    }
    if (f->is_write) {
        sys_file_write(f->path, f->wbuf, (unsigned long)f->wlen);
        free(f->wbuf);
    } else {
        sys_close(f->fd);
    }
    free(f);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f || f->is_write || size == 0 || nmemb == 0) {
        return 0;
    }
    long want = (long)(size * nmemb);
    long got = sys_read(f->fd, ptr, (unsigned long)want);
    if (got < want) {
        f->eof = 1;
    }
    if (got <= 0) {
        return 0;
    }
    return (size_t)got / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *f) {
    if (!f) {
        return 0;
    }
    size_t n = size * nmemb;
    if (f->is_console) {
        sys_write(1, ptr, n);
        return nmemb;
    }
    if (!f->is_write) {
        return 0;
    }
    if (f->wlen + n > f->wcap) {
        size_t newcap = f->wcap ? f->wcap * 2 : WRITE_BUF_GROW;
        while (newcap < f->wlen + n) {
            newcap *= 2;
        }
        unsigned char *nb = (unsigned char *)realloc(f->wbuf, newcap);
        if (!nb) {
            return 0;
        }
        f->wbuf = nb;
        f->wcap = newcap;
    }
    memcpy(f->wbuf + f->wlen, ptr, n);
    f->wlen += n;
    return nmemb;
}

/* No real seeking (see include/stdio.h) — always reports success/0 rather
   than failing outright, since some callers only use ftell()==0 as "empty
   file" rather than treating failure as fatal. */
int fseek(FILE *f, long offset, int whence) {
    (void)f; (void)offset; (void)whence;
    return 0;
}

long ftell(FILE *f) {
    (void)f;
    return 0;
}

int fflush(FILE *f) {
    (void)f;
    return 0;
}

int fgetc(FILE *f) {
    if (!f || f->is_write) {
        return EOF;
    }
    unsigned char c;
    long got = sys_read(f->fd, &c, 1);
    if (got != 1) {
        f->eof = 1;
        return EOF;
    }
    return c;
}

char *fgets(char *buf, int size, FILE *f) {
    if (!f || size <= 0) {
        return NULL;
    }
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF) {
            break;
        }
        buf[i++] = (char)c;
        if (c == '\n') {
            break;
        }
    }
    if (i == 0) {
        return NULL;
    }
    buf[i] = '\0';
    return buf;
}

int fputc(int c, FILE *f) {
    unsigned char ch = (unsigned char)c;
    fwrite(&ch, 1, 1, f);
    return c;
}

int fputs(const char *s, FILE *f) {
    size_t n = strlen(s);
    fwrite(s, 1, n, f);
    return (int)n;
}

int feof(FILE *f) {
    return f ? f->eof : 1;
}

int putchar(int c) {
    return fputc(c, stdout);
}

int puts(const char *s) {
    fputs(s, stdout);
    fputc('\n', stdout);
    return 0;
}

/* fat16.c has no delete primitive — nothing to actually do here, but this
   still needs to return successfully (not -1) rather than looking like a
   real failure to callers that check it (see rename() below: DOOM's own
   save path calls remove(finalname) before rename(tmp, finalname) purely
   to pre-clear a stale save; fat16_write_file() already overwrites an
   existing file on its own, so skipping the delete is harmless). */
int remove(const char *path) { (void)path; return 0; }

/* No rename primitive in fat16.c either, so this fakes one: read the
   whole source file into memory, write it back out under the new name,
   done — no attempt to remove the source (see remove() above for why
   that's fine for this port's one real caller, G_DoSaveGame() in
   g_game.c, which writes to a temp file and renames it to the real save
   slot name; the leftover temp file is otherwise harmless clutter, just
   overwritten on the next save). Was previously a no-op stub that always
   returned -1 — since G_DoSaveGame() doesn't check rename()'s return
   value, saves silently never reached their real filename and Load Game
   always found nothing. */
int rename(const char *oldpath, const char *newpath) {
    FILE *in = fopen(oldpath, "rb");
    if (!in) {
        return -1;
    }

    size_t cap = 8192, len = 0;
    unsigned char *buf = (unsigned char *)malloc(cap);
    if (!buf) {
        fclose(in);
        return -1;
    }
    for (;;) {
        if (len == cap) {
            cap *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, cap);
            if (!nb) {
                free(buf);
                fclose(in);
                return -1;
            }
            buf = nb;
        }
        size_t got = fread(buf + len, 1, cap - len, in);
        if (got == 0) {
            break;
        }
        len += got;
    }
    fclose(in);

    FILE *out = fopen(newpath, "wb");
    if (!out) {
        free(buf);
        return -1;
    }
    fwrite(buf, 1, len, out);
    fclose(out);
    free(buf);
    return 0;
}

/* ---- printf family: one core formatter (vsnprintf), everything else
   funnels through it into a bounded stack buffer. Supports flags
   (-,0,+,space,#), width, precision, length modifiers (h/hh/l/ll/z), and
   conversions d/i/u/x/X/o/c/s/p/n/%% — the set actually used across
   third_party/doomgeneric (verified: no %f reaches this in a live code
   path). ---- */

static void out_char(char **out, size_t *remaining, size_t *total, char c) {
    if (*remaining > 1) {
        **out = c;
        (*out)++;
        (*remaining)--;
    }
    (*total)++;
}

static void out_str_n(char **out, size_t *remaining, size_t *total, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        out_char(out, remaining, total, s[i]);
    }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    char *out = buf;
    size_t remaining = size;
    size_t total = 0;

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            out_char(&out, &remaining, &total, *p);
            continue;
        }
        p++;
        if (*p == '\0') {
            break;
        }
        if (*p == '%') {
            out_char(&out, &remaining, &total, '%');
            continue;
        }

        int left_align = 0, zero_pad = 0, plus = 0, space = 0;
        for (;;) {
            if (*p == '-') { left_align = 1; p++; }
            else if (*p == '0') { zero_pad = 1; p++; }
            else if (*p == '+') { plus = 1; p++; }
            else if (*p == ' ') { space = 1; p++; }
            else if (*p == '#') { p++; }
            else break;
        }

        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        int prec = -1;
        if (*p == '.') {
            p++;
            prec = 0;
            while (*p >= '0' && *p <= '9') { prec = prec * 10 + (*p - '0'); p++; }
        }

        int is_long = 0, is_longlong = 0;
        if (*p == 'l') {
            p++; is_long = 1;
            if (*p == 'l') { p++; is_longlong = 1; }
        } else if (*p == 'h') {
            p++;
            if (*p == 'h') { p++; }
        } else if (*p == 'z') {
            p++; is_long = 1;
        }

        char conv = *p;
        char numbuf[32];
        int numlen = 0;
        int neg = 0;
        const char *strp = NULL;
        int strn = 0;

        switch (conv) {
        case 'd': case 'i': {
            long v = is_longlong ? (long)va_arg(ap, long long)
                    : is_long ? va_arg(ap, long)
                    : (long)va_arg(ap, int);
            unsigned long uv;
            if (v < 0) { neg = 1; uv = (unsigned long)(-v); } else { uv = (unsigned long)v; }
            char tmp[24]; int n = 0;
            if (uv == 0) { tmp[n++] = '0'; }
            while (uv > 0) { tmp[n++] = (char)('0' + uv % 10); uv /= 10; }
            for (int i = 0; i < n; i++) { numbuf[i] = tmp[n - 1 - i]; }
            numlen = n;
            break;
        }
        case 'u': case 'x': case 'X': case 'o': {
            unsigned long v = is_longlong ? (unsigned long)va_arg(ap, unsigned long long)
                     : is_long ? va_arg(ap, unsigned long)
                     : (unsigned long)va_arg(ap, unsigned int);
            int base = (conv == 'u') ? 10 : (conv == 'o') ? 8 : 16;
            const char *digits = (conv == 'X') ? "0123456789ABCDEF" : "0123456789abcdef";
            char tmp[24]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            while (v > 0) { tmp[n++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
            for (int i = 0; i < n; i++) { numbuf[i] = tmp[n - 1 - i]; }
            numlen = n;
            break;
        }
        case 'c': {
            numbuf[0] = (char)va_arg(ap, int);
            numlen = 1;
            break;
        }
        case 's': {
            strp = va_arg(ap, const char *);
            if (!strp) { strp = "(null)"; }
            int l = 0;
            while (strp[l] && (prec < 0 || l < prec)) { l++; }
            strn = l;
            break;
        }
        case 'p': {
            unsigned long v = (unsigned long)(uintptr_t)va_arg(ap, void *);
            char tmp[20]; int n = 0;
            if (v == 0) { tmp[n++] = '0'; }
            while (v > 0) { tmp[n++] = "0123456789abcdef"[v % 16]; v /= 16; }
            numbuf[0] = '0'; numbuf[1] = 'x';
            for (int i = 0; i < n; i++) { numbuf[2 + i] = tmp[n - 1 - i]; }
            numlen = n + 2;
            break;
        }
        case 'n':
            (void)va_arg(ap, int *);
            continue;
        default:
            out_char(&out, &remaining, &total, '%');
            out_char(&out, &remaining, &total, conv);
            continue;
        }

        /* Precision on an integer conversion means "minimum digit count,
           zero-padded" — independent of (and overriding) the '0' flag,
           which only controls WIDTH padding. Without this, "%.3d" with 33
           produced "33" instead of "033" (the exact bug that broke
           third_party/doomgeneric's HUD font lump names, built as
           "STCFN%.3d" — WAD lookups for "STCFN33" failed because the real
           lump is "STCFN033"). */
        if (prec >= 0 && (conv == 'd' || conv == 'i' || conv == 'u' ||
                           conv == 'x' || conv == 'X' || conv == 'o')) {
            if (prec == 0 && numlen == 1 && numbuf[0] == '0') {
                numlen = 0; /* precision 0 + value 0 => no digits at all */
            } else if (numlen < prec) {
                int shift = prec - numlen;
                for (int i = numlen - 1; i >= 0; i--) { numbuf[i + shift] = numbuf[i]; }
                for (int i = 0; i < shift; i++) { numbuf[i] = '0'; }
                numlen = prec;
            }
            zero_pad = 0; /* precision satisfied; further WIDTH padding uses spaces, not zeros */
        }

        if (conv == 's') {
            int pad = width - strn;
            if (!left_align) { for (int i = 0; i < pad; i++) { out_char(&out, &remaining, &total, ' '); } }
            out_str_n(&out, &remaining, &total, strp, strn);
            if (left_align) { for (int i = 0; i < pad; i++) { out_char(&out, &remaining, &total, ' '); } }
        } else {
            char sign_char = neg ? '-' : plus ? '+' : space ? ' ' : 0;
            int sign_len = sign_char ? 1 : 0;
            int pad = width - numlen - sign_len;
            if (!left_align && !zero_pad) { for (int i = 0; i < pad; i++) { out_char(&out, &remaining, &total, ' '); } }
            if (sign_char) { out_char(&out, &remaining, &total, sign_char); }
            if (!left_align && zero_pad) { for (int i = 0; i < pad; i++) { out_char(&out, &remaining, &total, '0'); } }
            out_str_n(&out, &remaining, &total, numbuf, numlen);
            if (left_align) { for (int i = 0; i < pad; i++) { out_char(&out, &remaining, &total, ' '); } }
        }
    }

    if (remaining > 0) {
        *out = '\0';
    } else if (size > 0) {
        buf[size - 1] = '\0';
    }
    return (int)total;
}

int vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsnprintf(buf, (size_t)-1, fmt, ap);
}

int vfprintf(FILE *f, const char *fmt, va_list ap) {
    char buf[512];
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    fwrite(buf, 1, (size_t)(n < (int)sizeof(buf) - 1 ? n : (int)sizeof(buf) - 1), f);
    return n;
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vprintf(fmt, ap);
    va_end(ap);
    return n;
}

int fprintf(FILE *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    return (c | 0x20) - 'a' + 10;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char *s = str;
    int count = 0;

    for (const char *f = fmt; *f; f++) {
        if (*f == '%') {
            f++;
            if (*f == '\0') {
                break;
            }
            while (*f >= '0' && *f <= '9') {
                f++; /* width: parsed but not enforced */
            }
            while (*s == ' ' || *s == '\t' || *s == '\n') {
                s++;
            }

            if (*f == 'd' || *f == 'i') {
                int neg = 0;
                unsigned long v = 0;
                int any = 0;
                if (*s == '-') { neg = 1; s++; } else if (*s == '+') { s++; }
                if (*f == 'i' && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
                    s += 2;
                    while (isxdigit((unsigned char)*s)) { v = v * 16 + (unsigned)hexval(*s); s++; any = 1; }
                } else {
                    while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; any = 1; }
                }
                if (!any) { break; }
                *va_arg(ap, int *) = (int)(neg ? -(long)v : (long)v);
                count++;
            } else if (*f == 'x' || *f == 'X') {
                unsigned long v = 0;
                int any = 0;
                while (isxdigit((unsigned char)*s)) { v = v * 16 + (unsigned)hexval(*s); s++; any = 1; }
                if (!any) { break; }
                *va_arg(ap, unsigned int *) = (unsigned int)v;
                count++;
            } else if (*f == 'u') {
                unsigned long v = 0;
                int any = 0;
                while (*s >= '0' && *s <= '9') { v = v * 10 + (unsigned)(*s - '0'); s++; any = 1; }
                if (!any) { break; }
                *va_arg(ap, unsigned int *) = (unsigned int)v;
                count++;
            } else if (*f == 'c') {
                if (!*s) { break; }
                *va_arg(ap, char *) = *s++;
                count++;
            } else if (*f == 's') {
                char *dst = va_arg(ap, char *);
                int n = 0;
                while (*s && *s != ' ' && *s != '\t' && *s != '\n') { dst[n++] = *s++; }
                dst[n] = '\0';
                if (n == 0) { break; }
                count++;
            } else if (*f == '%') {
                if (*s != '%') { break; }
                s++;
            }
        } else if (*f == ' ' || *f == '\t' || *f == '\n') {
            while (*s == ' ' || *s == '\t' || *s == '\n') {
                s++;
            }
        } else {
            if (*s != *f) {
                break;
            }
            s++;
        }
    }

    va_end(ap);
    return count;
}
