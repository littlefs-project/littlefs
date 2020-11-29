/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_RAMBD_H
#define LFS_RAMBD_H

#include "lfs.h"

#ifdef __cplusplus
extern "C" {
#endif


// Block device specific tracing
#ifdef LFS_RAMBD_YES_TRACE
#define LFS_RAMBD_TRACE(...) LFS_TRACE(__VA_ARGS__)
#else
#define LFS_RAMBD_TRACE(...)
#endif

// rambd config (optional)
struct lfs_rambd_cfg {
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

    // Optional statically allocated buffer for the block device.
    void *buffer;
};

// rambd state
typedef struct lfs_rambd {
    uint8_t *buffer;
    const struct lfs_rambd_cfg *cfg;
} lfs_rambd_t;


// Create a RAM block device using the geometry in lfs_cfg
int lfs_rambd_createcfg(lfs_rambd_t *bd,
        const struct lfs_rambd_cfg *cfg);

// Clean up memory associated with block device
int lfs_rambd_destroy(lfs_rambd_t *bd);

// Read a block
int lfs_rambd_read(lfs_rambd_t *bd, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs_rambd_prog(lfs_rambd_t *bd, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs_rambd_erase(lfs_rambd_t *bd, lfs_block_t block);

// Sync the block device
int lfs_rambd_sync(lfs_rambd_t *bd);


#ifdef __cplusplus
}
#endif

#endif
