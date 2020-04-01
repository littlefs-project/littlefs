/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS2_RAMBD_H
#define LFS2_RAMBD_H

#include "lfs2.h"
#include "lfs2_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Block device specific tracing
#ifdef LFS2_RAMBD_YES_TRACE
#define LFS2_RAMBD_TRACE(...) LFS2_TRACE(__VA_ARGS__)
#else
#define LFS2_RAMBD_TRACE(...)
#endif

// rambd config (optional)
struct lfs2_rambd_config {
    // 8-bit erase value to simulate erasing with. -1 indicates no erase
    // occurs, which is still a valid block device
    int32_t erase_value;

    // Optional statically allocated buffer for the block device.
    void *buffer;
};

// rambd state
typedef struct lfs2_rambd {
    uint8_t *buffer;
    const struct lfs2_rambd_config *cfg;
} lfs2_rambd_t;


// Create a RAM block device using the geometry in lfs2_config
int lfs2_rambd_create(const struct lfs2_config *cfg);
int lfs2_rambd_createcfg(const struct lfs2_config *cfg,
        const struct lfs2_rambd_config *bdcfg);

// Clean up memory associated with block device
int lfs2_rambd_destroy(const struct lfs2_config *cfg);

// Read a block
int lfs2_rambd_read(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, void *buffer, lfs2_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs2_rambd_prog(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, const void *buffer, lfs2_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs2_rambd_erase(const struct lfs2_config *cfg, lfs2_block_t block);

// Sync the block device
int lfs2_rambd_sync(const struct lfs2_config *cfg);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
