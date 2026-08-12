#ifndef NAUMI_SHIM_UNISTD_H
#define NAUMI_SHIM_UNISTD_H

/* Intentionally empty — third_party/doomgeneric's i_system.c includes this
   only for isatty()/fileno(), both inside a dead `#if ORIGCODE` branch
   (ORIGCODE is never defined; the live path is a hardcoded `return 0`). */

#endif
