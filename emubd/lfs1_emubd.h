/*
 * Block device emulated on standard files
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS1_EMUBD_H
#define LFS1_EMUBD_H

#include "lfs1.h"
#include "lfs1_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Config options
#ifndef LFS1_EMUBD_READ_SIZE
#define LFS1_EMUBD_READ_SIZE 1
#endif

#ifndef LFS1_EMUBD_PROG_SIZE
#define LFS1_EMUBD_PROG_SIZE 1
#endif

#ifndef LFS1_EMUBD_ERASE_SIZE
#define LFS1_EMUBD_ERASE_SIZE 512
#endif

#ifndef LFS1_EMUBD_TOTAL_SIZE
#define LFS1_EMUBD_TOTAL_SIZE 524288
#endif


// The emu bd state
typedef struct lfs1_emubd {
    char *path;
    char *child;

    struct {
        uint64_t read_count;
        uint64_t prog_count;
        uint64_t erase_count;
    } stats;

    struct {
        uint32_t read_size;
        uint32_t prog_size;
        uint32_t block_size;
        uint32_t block_count;
    } cfg;
} lfs1_emubd_t;


// Create a block device using path for the directory to store blocks
int lfs1_emubd_create(const struct lfs1_config *cfg, const char *path);

// Clean up memory associated with emu block device
void lfs1_emubd_destroy(const struct lfs1_config *cfg);

// Read a block
int lfs1_emubd_read(const struct lfs1_config *cfg, lfs1_block_t block,
        lfs1_off_t off, void *buffer, lfs1_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs1_emubd_prog(const struct lfs1_config *cfg, lfs1_block_t block,
        lfs1_off_t off, const void *buffer, lfs1_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs1_emubd_erase(const struct lfs1_config *cfg, lfs1_block_t block);

// Sync the block device
int lfs1_emubd_sync(const struct lfs1_config *cfg);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
