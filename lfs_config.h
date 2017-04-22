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
typedef uint32_t lfs_size_t;
typedef uint32_t lfs_off_t;

typedef int32_t  lfs_ssize_t;
typedef int32_t  lfs_soff_t;

typedef uint32_t lfs_block_t;

// Maximum length of file name
#ifndef LFS_NAME_MAX
#define LFS_NAME_MAX 255
#endif

// Logging operations
#include <stdio.h>
#define LFS_ERROR(fmt, ...) printf("lfs error: " fmt "\n", __VA_ARGS__)
#define LFS_WARN(fmt, ...)  printf("lfs warn: " fmt "\n", __VA_ARGS__)
#define LFS_INFO(fmt, ...)  printf("lfs info: " fmt "\n", __VA_ARGS__)


#endif
