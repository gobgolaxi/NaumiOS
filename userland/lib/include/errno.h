#ifndef NAUMI_SHIM_ERRNO_H
#define NAUMI_SHIM_ERRNO_H

/* This project's fopen() (userland/lib/stdio.c) never distinguishes "not
   found" from "it's a directory" — there's no directory-vs-file concept
   getting in the way of a plain SYS_OPEN — so errno is simply never set to
   EISDIR, only ever left at 0. Exists purely so third-party sources
   (third_party/doomgeneric's m_misc.c: M_FileExists()) that check
   `errno == EISDIR` after a failed fopen() compile and behave correctly
   (always "not a directory either" in our case). */

extern int errno;
#define EISDIR 21

#endif
