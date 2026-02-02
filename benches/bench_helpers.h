/*
 * Some extra bench helpers
 *
 */
#ifndef BENCH_HELPERS_H
#define BENCH_HELPERS_H

#include "runners/bench_runner.h"


// warm up the filesystem
//
// this writes a 1 block file 2*block_count times to get it into a good
// state for benchmarking
int bench_helpers_warmup(lfs3_t *lfs3);


// find tight disk usage
uintmax_t bench_helpers_usage(lfs3_t *lfs3);


#endif
