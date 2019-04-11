/*
 * Block device emulated on standard files
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS2_EMUBD_H
#define LFS2_EMUBD_H

#include "lfs2.h"
#include "lfs2_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Config options
#ifndef LFS2_EMUBD_READ_SIZE
#define LFS2_EMUBD_READ_SIZE 1
#endif

#ifndef LFS2_EMUBD_PROG_SIZE
#define LFS2_EMUBD_PROG_SIZE 1
#endif

#ifndef LFS2_EMUBD_ERASE_SIZE
#define LFS2_EMUBD_ERASE_SIZE 512
#endif

#ifndef LFS2_EMUBD_TOTAL_SIZE
#define LFS2_EMUBD_TOTAL_SIZE 524288
#endif


// The emu bd state
typedef struct lfs2_emubd {
    char *path;
    char *child;

    struct {
        uint64_t read_count;
        uint64_t prog_count;
        uint64_t erase_count;
    } stats;

    struct {
        lfs2_block_t blocks[4];
    } history;

    struct {
        uint32_t read_size;
        uint32_t prog_size;
        uint32_t block_size;
        uint32_t block_count;
    } cfg;
} lfs2_emubd_t;


// Create a block device using path for the directory to store blocks
int lfs2_emubd_create(const struct lfs2_config *cfg, const char *path);

// Clean up memory associated with emu block device
void lfs2_emubd_destroy(const struct lfs2_config *cfg);

// Read a block
int lfs2_emubd_read(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, void *buffer, lfs2_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs2_emubd_prog(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, const void *buffer, lfs2_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs2_emubd_erase(const struct lfs2_config *cfg, lfs2_block_t block);

// Sync the block device
int lfs2_emubd_sync(const struct lfs2_config *cfg);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
