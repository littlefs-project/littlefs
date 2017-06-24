/*
 * lfs utility functions
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_UTIL_H
#define LFS_UTIL_H

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>


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

void lfs_crc(uint32_t *crc, const void *buffer, size_t size);


// Logging functions
#define LFS_DEBUG(fmt, ...) printf("lfs debug: " fmt "\n", __VA_ARGS__)
#define LFS_WARN(fmt, ...)  printf("lfs warn: " fmt "\n", __VA_ARGS__)
#define LFS_ERROR(fmt, ...) printf("lfs error: " fmt "\n", __VA_ARGS__)


#endif
