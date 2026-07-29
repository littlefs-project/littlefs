/*
 * Some extra bench helpers
 *
 */
#ifndef BENCH_HELPERS_H
#define BENCH_HELPERS_H

#include "runners/bench_runner.h"


// warm up the filesystem
//
// this writes a 1 block file 2*block_count times to get things into a
// good state for benchmarking
int bench_helpers_warmup(lfs3_t *lfs3);


// populate the filesystem with static files
//
// useful for static vs dynamic wear-leveling, metadata pressure, etc
int bench_helpers_populate(lfs3_t *lfs3,
        lfs3_off_t static_count, lfs3_off_t static_size);


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
