/*
 * Block device emulated in a file
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS3_FILEBD_H
#define LFS3_FILEBD_H

#include "lfs3.h"
#include "lfs3_util.h"


// Block device specific tracing
#ifndef LFS3_FILEBD_TRACE
#ifdef LFS3_FILEBD_YES_TRACE
#define LFS3_FILEBD_TRACE(...) LFS3_TRACE(__VA_ARGS__)
#else
#define LFS3_FILEBD_TRACE(...)
#endif
#endif

// filebd state
typedef struct lfs3_filebd {
    int fd;
} lfs3_filebd_t;


// Create a file block device using the geometry in lfs3_config
int lfs3_filebd_create(const struct lfs3_config *cfg, const char *path);

// Clean up memory associated with block device
int lfs3_filebd_destroy(const struct lfs3_config *cfg);

// Read a block
int lfs3_filebd_read(const struct lfs3_config *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs3_filebd_prog(const struct lfs3_config *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs3_filebd_erase(const struct lfs3_config *cfg, lfs3_block_t block);

// Sync the block device
int lfs3_filebd_sync(const struct lfs3_config *cfg);


#endif
