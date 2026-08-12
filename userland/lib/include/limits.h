#ifndef NAUMI_SHIM_LIMITS_H
#define NAUMI_SHIM_LIMITS_H

/* Standard C limit macros for a LP64 AArch64 target (char is 8-bit signed,
   int 32-bit, long/pointer 64-bit — this toolchain's actual ABI). Written
   directly rather than relying on GCC's own <limits.h>: that one is meant
   to supplement a real libc's copy via `#include_next <limits.h>`, which
   has nothing to chain to under -nostdinc (no system limits.h exists) and
   fails to compile. */

#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535

#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U

#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL

#define LLONG_MIN (-9223372036854775807LL - 1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL

#define PATH_MAX 260 /* generous relative to this project's own MAX_PATH (32) */

#endif
