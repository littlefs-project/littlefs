/*
 * Configuration and type definitions
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_CONFIG_H
#define LFS_CONFIG_H

#include <stdint.h>

// Type definitions
typedef uint64_t lfs_lword_t;
typedef uint32_t lfs_word_t;
typedef uint16_t lfs_hword_t;

typedef lfs_word_t lfs_size_t;
typedef lfs_word_t lfs_off_t;
typedef int lfs_error_t;

typedef lfs_lword_t lfs_lsize_t;
typedef lfs_word_t  lfs_ino_t;
typedef lfs_hword_t lfs_ioff_t;

// Maximum length of file name
#define LFS_NAME_MAX 255

// Builtin functions
static inline lfs_word_t lfs_max(lfs_word_t a, lfs_word_t b) {
    return (a > b) ? a : b;
}

static inline lfs_word_t lfs_min(lfs_word_t a, lfs_word_t b) {
    return (a < b) ? a : b;
}

#endif
