/*
 * Block device interface
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_BD_H
#define LFS_BD_H

#include "lfs_config.h"


// Opaque type for block devices
typedef void lfs_bd_t;

// Description of block devices
struct lfs_bd_info {
    lfs_size_t read_size;   // Size of readable block
    lfs_size_t prog_size;   // Size of programmable block
    lfs_size_t erase_size;  // Size of erase block

    uint64_t total_size; // Total size of the device
};

// Block device operations
//
// The little file system takes in a pointer to an opaque type
// and this struct, all operations are passed the opaque pointer
// which can be used to reference any state associated with the
// block device
struct lfs_bd_ops {
    // Read a block
    int (*read)(lfs_bd_t *bd, lfs_block_t block,
            lfs_off_t off, lfs_size_t size, void *buffer);

    // Program a block
    //
    // The block must have previously been erased.
    int (*prog)(lfs_bd_t *bd, lfs_block_t block,
            lfs_off_t off, lfs_size_t size, const void *buffer);

    // Erase a block
    //
    // A block must be erased before being programmed. The
    // state of an erased block is undefined.
    int (*erase)(lfs_bd_t *bd, lfs_block_t block,
            lfs_off_t off, lfs_size_t size);

    // Sync the block device
    int (*sync)(lfs_bd_t *bd);

    // Get a description of the block device
    //
    // Any unknown information may be left as zero
    int (*info)(lfs_bd_t *bd, struct lfs_bd_info *info);
};


#endif
