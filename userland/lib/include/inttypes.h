#ifndef NAUMI_SHIM_INTTYPES_H
#define NAUMI_SHIM_INTTYPES_H

/* third_party/doomgeneric's doomtype.h includes this only as a
   pre-C99-stdint.h fallback ("some old Solaris only has inttypes.h") and
   never uses anything inttypes.h adds beyond stdint.h (format macros,
   strtoimax, etc) — this project's <stdint.h> comes straight from the
   toolchain's own freestanding headers, so there's nothing extra to add. */
#include <stdint.h>

#endif
