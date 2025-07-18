/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS3_RAMBD_H
#define LFS3_RAMBD_H

#include "lfs3.h"
#include "lfs3_util.h"


// Block device specific tracing
#ifndef LFS3_RAMBD_TRACE
#ifdef LFS3_RAMBD_YES_TRACE
#define LFS3_RAMBD_TRACE(...) LFS3_TRACE(__VA_ARGS__)
#else
#define LFS3_RAMBD_TRACE(...)
#endif
#endif

// rambd config (optional)
struct lfs3_rambd_cfg {
    // Optional statically allocated buffer for the block device.
    void *buffer;
};

// rambd state
typedef struct lfs3_rambd {
    uint8_t *buffer;
    const struct lfs3_rambd_cfg *cfg;
} lfs3_rambd_t;


// Create a RAM block device using the geometry in lfs3_cfg
int lfs3_rambd_create(const struct lfs3_cfg *cfg);
int lfs3_rambd_createcfg(const struct lfs3_cfg *cfg,
        const struct lfs3_rambd_cfg *bdcfg);

// Clean up memory associated with block device
int lfs3_rambd_destroy(const struct lfs3_cfg *cfg);

// Read a block
int lfs3_rambd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs3_rambd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs3_rambd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block);

// Sync the block device
int lfs3_rambd_sync(const struct lfs3_cfg *cfg);


#endif
