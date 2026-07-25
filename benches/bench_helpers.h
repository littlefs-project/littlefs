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


// interesting usage info
struct bench_helpers_usage {
    uintmax_t usage;
    uintmax_t mdir;
    uintmax_t btree;
    uintmax_t data;
};

// find tight disk usage
int bench_helpers_usage(lfs3_t *lfs3, struct bench_helpers_usage *usage);


#endif
