/*
 * Block device emulated on standard files
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_EMUBD_H
#define LFS_EMUBD_H

#include "lfs_config.h"
#include "lfs_util.h"
#include "lfs_bd.h"


// Stats for debugging and optimization
struct lfs_bd_stats {
    uint64_t read_count;
    uint64_t prog_count;
    uint64_t erase_count;
};

// The emu bd state
typedef struct lfs_emubd {
    char *path;
    char *child;
    struct lfs_bd_info info;
    struct lfs_bd_stats stats;
} lfs_emubd_t;


// Create a block device using path for the directory to store blocks
int lfs_emubd_create(lfs_emubd_t *emu, const char *path);

// Clean up memory associated with emu block device
void lfs_emubd_destroy(lfs_emubd_t *emu);

// Read a block
int lfs_emubd_read(lfs_emubd_t *bd, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, void *buffer);

// Program a block
//
// The block must have previously been erased.
int lfs_emubd_prog(lfs_emubd_t *bd, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs_emubd_erase(lfs_emubd_t *bd, lfs_block_t block,
        lfs_off_t off, lfs_size_t size);

// Sync the block device
int lfs_emubd_sync(lfs_emubd_t *bd);

// Get a description of the block device
//
// Any unknown information may be left unmodified
int lfs_emubd_info(lfs_emubd_t *bd, struct lfs_bd_info *info);

// Get stats of operations on the block device
//
// Used for debugging and optimizations
int lfs_emubd_stats(lfs_emubd_t *bd, struct lfs_bd_stats *stats);

// Block device operations
extern const struct lfs_bd_ops lfs_emubd_ops;


#endif
