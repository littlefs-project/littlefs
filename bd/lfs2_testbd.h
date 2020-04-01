/*
 * Testing block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS2_TESTBD_H
#define LFS2_TESTBD_H

#include "lfs2.h"
#include "lfs2_util.h"
#include "bd/lfs2_rambd.h"
#include "bd/lfs2_filebd.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Block device specific tracing
#ifdef LFS2_TESTBD_YES_TRACE
#define LFS2_TESTBD_TRACE(...) LFS2_TRACE(__VA_ARGS__)
#else
#define LFS2_TESTBD_TRACE(...)
#endif

// Mode determining how "bad blocks" behave during testing. This simulates
// some real-world circumstances such as progs not sticking (prog-noop),
// a readonly disk (erase-noop), and ECC failures (read-error).
//
// Not that read-noop is not allowed. Read _must_ return a consistent (but
// may be arbitrary) value on every read.
enum lfs2_testbd_badblock_behavior {
    LFS2_TESTBD_BADBLOCK_PROGERROR,
    LFS2_TESTBD_BADBLOCK_ERASEERROR,
    LFS2_TESTBD_BADBLOCK_READERROR,
    LFS2_TESTBD_BADBLOCK_PROGNOOP,
    LFS2_TESTBD_BADBLOCK_ERASENOOP,
};

// Type for measuring wear
typedef uint32_t lfs2_testbd_wear_t;
typedef int32_t  lfs2_testbd_swear_t;

// testbd config, this is required for testing
struct lfs2_testbd_config {
    // 8-bit erase value to use for simulating erases. -1 does not simulate
    // erases, which can speed up testing by avoiding all the extra block-device
    // operations to store the erase value.
    int32_t erase_value;

    // Number of erase cycles before a block becomes "bad". The exact behavior
    // of bad blocks is controlled by the badblock_mode.
    uint32_t erase_cycles;

    // The mode determining how bad blocks fail
    uint8_t badblock_behavior;

    // Number of write operations (erase/prog) before forcefully killing
    // the program with exit. Simulates power-loss. 0 disables.
    uint32_t power_cycles;

    // Optional buffer for RAM block device.
    void *buffer;

    // Optional buffer for wear
    void *wear_buffer;
};

// testbd state
typedef struct lfs2_testbd {
    union {
        struct {
            lfs2_filebd_t bd;
            struct lfs2_filebd_config cfg;
        } file;
        struct {
            lfs2_rambd_t bd;
            struct lfs2_rambd_config cfg;
        } ram;
    } u;

    bool persist;
    uint32_t power_cycles;
    lfs2_testbd_wear_t *wear;

    const struct lfs2_testbd_config *cfg;
} lfs2_testbd_t;


/// Block device API ///

// Create a test block device using the geometry in lfs2_config
//
// Note that filebd is used if a path is provided, if path is NULL
// testbd will use rambd which can be much faster.
int lfs2_testbd_create(const struct lfs2_config *cfg, const char *path);
int lfs2_testbd_createcfg(const struct lfs2_config *cfg, const char *path,
        const struct lfs2_testbd_config *bdcfg);

// Clean up memory associated with block device
int lfs2_testbd_destroy(const struct lfs2_config *cfg);

// Read a block
int lfs2_testbd_read(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, void *buffer, lfs2_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs2_testbd_prog(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, const void *buffer, lfs2_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs2_testbd_erase(const struct lfs2_config *cfg, lfs2_block_t block);

// Sync the block device
int lfs2_testbd_sync(const struct lfs2_config *cfg);


/// Additional extended API for driving test features ///

// Get simulated wear on a given block
lfs2_testbd_swear_t lfs2_testbd_getwear(const struct lfs2_config *cfg,
        lfs2_block_t block);

// Manually set simulated wear on a given block
int lfs2_testbd_setwear(const struct lfs2_config *cfg,
        lfs2_block_t block, lfs2_testbd_wear_t wear);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
