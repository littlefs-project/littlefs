/*
 * lfs utility functions
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_UTIL_H
#define LFS_UTIL_H

#include "lfs_config.h"
#include <stdlib.h>


// Builtin functions
static inline uint32_t lfs_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static inline uint32_t lfs_min(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static inline uint32_t lfs_ctz(uint32_t a) {
    return __builtin_ctz(a);
}

static inline uint32_t lfs_npw2(uint32_t a) {
    return 32 - __builtin_clz(a-1);
}

static inline int lfs_scmp(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}

uint32_t lfs_crc(uint32_t crc, lfs_size_t size, const void *buffer);


#endif
