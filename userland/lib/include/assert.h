#ifndef NAUMI_SHIM_ASSERT_H
#define NAUMI_SHIM_ASSERT_H

/* Always compiled out — third-party sources (third_party/doomgeneric) use
   assert() as an internal-invariant debug aid; this project has no
   assert-failure reporting path (no stderr-with-abort convention), so
   trust the invariant and drop the check rather than build one. */
#define assert(expr) ((void)0)

#endif
