/*
 * bench_shim.h — injected into every Malardalen benchmark via -include.
 *
 * Two jobs:
 *
 *  1. Pull in <stdio.h> BEFORE neutralising printf.  The order matters: if the
 *     function-like macro were defined first, it would expand inside glibc's own
 *     declaration of printf and break the header.
 *
 *  2. Neutralise console output.  Most benchmarks already guard their printf
 *     calls with #ifdef DEBUG, and matmult.c is covered by -DUPPSALAWCET, so in
 *     practice nothing here fires today.  It stays as a safety net: a write(2)
 *     syscall inside the timed region would dwarf the benchmark itself and
 *     silently corrupt every number we report.
 *
 * The benchmark sources themselves are never modified.
 */
#ifndef BENCH_SHIM_H
#define BENCH_SHIM_H

#include <stdio.h>
#include <stdlib.h>

#define printf(...)     (0)
#define fprintf(...)    (0)
#define puts(s)         (0)
#define putchar(c)      (0)

#endif /* BENCH_SHIM_H */
