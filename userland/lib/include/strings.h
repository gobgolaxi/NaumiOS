#ifndef NAUMI_SHIM_STRINGS_H
#define NAUMI_SHIM_STRINGS_H

/* Legacy BSD header some sources include just for strcasecmp/strncasecmp
   — both already declared in our <string.h> (real libc puts them there
   too on most modern systems; this project doesn't bother splitting them
   out separately). */
#include <string.h>

#endif
