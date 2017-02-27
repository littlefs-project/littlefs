/*
 * Block device emulated on standard files
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_EMUBD_H
#define LFS_EMUBD_H

#include "lfs_config.h"
#include "lfs_bd.h"


// Stats for debugging and optimization
struct lfs_bd_stats {
    lfs_lword_t read_count;
    lfs_lword_t write_count;
    lfs_lword_t erase_count;
};

// The emu bd state
typedef struct lfs_emubd {
    char *path;
    char *child;
    struct lfs_bd_info info;
    struct lfs_bd_stats stats;
} lfs_emubd_t;


// Create a block device using path for the directory to store blocks
lfs_error_t lfs_emubd_create(lfs_emubd_t *emu, const char *path);

// Clean up memory associated with emu block device
void lfs_emubd_destroy(lfs_emubd_t *emu);

// Read a block
lfs_error_t lfs_emubd_read(lfs_emubd_t *bd, uint8_t *buffer,
        lfs_ino_t ino, lfs_off_t off, lfs_size_t size);

// Program a block
//
// The block must have previously been erased.
lfs_error_t lfs_emubd_write(lfs_emubd_t *bd, const uint8_t *buffer,
        lfs_ino_t ino, lfs_off_t off, lfs_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
lfs_error_t lfs_emubd_erase(lfs_emubd_t *bd,
        lfs_ino_t ino, lfs_off_t off, lfs_size_t size);

// Sync the block device
lfs_error_t lfs_emubd_sync(lfs_emubd_t *bd);

// Get a description of the block device
//
// Any unknown information may be left unmodified
lfs_error_t lfs_emubd_info(lfs_emubd_t *bd, struct lfs_bd_info *info);

// Get stats of operations on the block device
//
// Used for debugging and optimizations
lfs_error_t lfs_emubd_stats(lfs_emubd_t *bd, struct lfs_bd_stats *stats);

// Block device operations
extern const struct lfs_bd_ops lfs_emubd_ops;


#endif
