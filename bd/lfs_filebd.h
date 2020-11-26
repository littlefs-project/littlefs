/*
 * Block device emulated in a file
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_FILEBD_H
#define LFS_FILEBD_H

#include "lfs.h"

#ifdef __cplusplus
extern "C" {
#endif


// Block device specific tracing
#ifdef LFS_FILEBD_YES_TRACE
#define LFS_FILEBD_TRACE(...) LFS_TRACE(__VA_ARGS__)
#else
#define LFS_FILEBD_TRACE(...)
#endif

// filebd config (optional)
struct lfs_filebd_cfg {
    // Minimum size of block read. All read operations must be a
    // multiple of this value.
    lfs_size_t read_size;

    // Minimum size of block program. All program operations must be a
    // multiple of this value.
    lfs_size_t prog_size;

    // Size of an erasable block.
    lfs_size_t erase_size;

    // Number of erasable blocks on the device.
    lfs_size_t erase_count;

    // 8-bit erase value to use for simulating erases. -1 does not simulate
    // erases, which can speed up testing by avoiding all the extra block-device
    // operations to store the erase value.
    int32_t erase_value;
};

// filebd state
typedef struct lfs_filebd {
    int fd;
    const struct lfs_filebd_cfg *cfg;
} lfs_filebd_t;


// Create a file block device using the geometry in lfs_filebd_cfg
int lfs_filebd_createcfg(lfs_filebd_t *bd, const char *path,
        const struct lfs_filebd_cfg *cfg);

// Clean up memory associated with block device
int lfs_filebd_destroy(lfs_filebd_t *bd);

// Read a block
int lfs_filebd_read(lfs_filebd_t *bd, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs_filebd_prog(lfs_filebd_t *bd, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs_filebd_erase(lfs_filebd_t *bd, lfs_block_t block);

// Sync the block device
int lfs_filebd_sync(lfs_filebd_t *bd);


#ifdef __cplusplus
}
#endif

#endif
