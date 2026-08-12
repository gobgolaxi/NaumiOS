#ifndef NAUMI_SHIM_MATH_H
#define NAUMI_SHIM_MATH_H

/* Intentionally empty. third_party/doomgeneric includes <math.h> in a
   handful of files, but every call to a real transcendental function
   (atan/tan/sin in r_main.c) is inside `#if 0` — vanilla DOOM's angle
   tables are precomputed integer tables in tables.c instead, the float
   path is dead code left over from id's original comments ("UNUSED - now
   getting from tables.c"). No live code needs a real libm, so there's
   nothing to declare — and no floating point support to build one on top
   of anyway (the kernel and this toolchain both build with
   -mgeneral-regs-only, no FPU context saved across task switches). */

#endif
