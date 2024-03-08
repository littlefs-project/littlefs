/*
 * Emulating block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS2_EMUBD_H
#define LFS2_EMUBD_H

#include "lfs2.h"
#include "lfs2_util.h"
#include "bd/lfs2_rambd.h"
#include "bd/lfs2_filebd.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Block device specific tracing
#ifndef LFS2_EMUBD_TRACE
#ifdef LFS2_EMUBD_YES_TRACE
#define LFS2_EMUBD_TRACE(...) LFS2_TRACE(__VA_ARGS__)
#else
#define LFS2_EMUBD_TRACE(...)
#endif
#endif

// Mode determining how "bad-blocks" behave during testing. This simulates
// some real-world circumstances such as progs not sticking (prog-noop),
// a readonly disk (erase-noop), and ECC failures (read-error).
//
// Not that read-noop is not allowed. Read _must_ return a consistent (but
// may be arbitrary) value on every read.
typedef enum lfs2_emubd_badblock_behavior {
    LFS2_EMUBD_BADBLOCK_PROGERROR  = 0, // Error on prog
    LFS2_EMUBD_BADBLOCK_ERASEERROR = 1, // Error on erase
    LFS2_EMUBD_BADBLOCK_READERROR  = 2, // Error on read
    LFS2_EMUBD_BADBLOCK_PROGNOOP   = 3, // Prog does nothing silently
    LFS2_EMUBD_BADBLOCK_ERASENOOP  = 4, // Erase does nothing silently
} lfs2_emubd_badblock_behavior_t;

// Mode determining how power-loss behaves during testing. For now this
// only supports a noop behavior, leaving the data on-disk untouched.
typedef enum lfs2_emubd_powerloss_behavior {
    LFS2_EMUBD_POWERLOSS_NOOP = 0, // Progs are atomic
    LFS2_EMUBD_POWERLOSS_OOO  = 1, // Blocks are written out-of-order
} lfs2_emubd_powerloss_behavior_t;

// Type for measuring read/program/erase operations
typedef uint64_t lfs2_emubd_io_t;
typedef int64_t lfs2_emubd_sio_t;

// Type for measuring wear
typedef uint32_t lfs2_emubd_wear_t;
typedef int32_t lfs2_emubd_swear_t;

// Type for tracking power-cycles
typedef uint32_t lfs2_emubd_powercycles_t;
typedef int32_t lfs2_emubd_spowercycles_t;

// Type for delays in nanoseconds
typedef uint64_t lfs2_emubd_sleep_t;
typedef int64_t lfs2_emubd_ssleep_t;

// emubd config, this is required for testing
struct lfs2_emubd_config {
    // Minimum size of a read operation in bytes.
    lfs2_size_t read_size;

    // Minimum size of a program operation in bytes.
    lfs2_size_t prog_size;

    // Size of an erase operation in bytes.
    lfs2_size_t erase_size;

    // Number of erase blocks on the device.
    lfs2_size_t erase_count;

    // 8-bit erase value to use for simulating erases. -1 does not simulate
    // erases, which can speed up testing by avoiding the extra block-device
    // operations to store the erase value.
    int32_t erase_value;

    // Number of erase cycles before a block becomes "bad". The exact behavior
    // of bad blocks is controlled by badblock_behavior.
    uint32_t erase_cycles;

    // The mode determining how bad-blocks fail
    lfs2_emubd_badblock_behavior_t badblock_behavior;

    // Number of write operations (erase/prog) before triggering a power-loss.
    // power_cycles=0 disables this. The exact behavior of power-loss is
    // controlled by a combination of powerloss_behavior and powerloss_cb.
    lfs2_emubd_powercycles_t power_cycles;

    // The mode determining how power-loss affects disk
    lfs2_emubd_powerloss_behavior_t powerloss_behavior;

    // Function to call to emulate power-loss. The exact behavior of power-loss
    // is up to the runner to provide.
    void (*powerloss_cb)(void*);

    // Data for power-loss callback
    void *powerloss_data;

    // True to track when power-loss could have occured. Note this involves 
    // heavy memory usage!
    bool track_branches;

    // Path to file to use as a mirror of the disk. This provides a way to view
    // the current state of the block device.
    const char *disk_path;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs2_emubd_sleep_t read_sleep;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs2_emubd_sleep_t prog_sleep;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs2_emubd_sleep_t erase_sleep;
};

// A reference counted block
typedef struct lfs2_emubd_block {
    uint32_t rc;
    lfs2_emubd_wear_t wear;

    uint8_t data[];
} lfs2_emubd_block_t;

// Disk mirror
typedef struct lfs2_emubd_disk {
    uint32_t rc;
    int fd;
    uint8_t *scratch;
} lfs2_emubd_disk_t;

// emubd state
typedef struct lfs2_emubd {
    // array of copy-on-write blocks
    lfs2_emubd_block_t **blocks;

    // some other test state
    lfs2_emubd_io_t readed;
    lfs2_emubd_io_t proged;
    lfs2_emubd_io_t erased;
    lfs2_emubd_powercycles_t power_cycles;
    lfs2_ssize_t ooo_block;
    lfs2_emubd_block_t *ooo_data;
    lfs2_emubd_disk_t *disk;

    const struct lfs2_emubd_config *cfg;
} lfs2_emubd_t;


/// Block device API ///

// Create an emulating block device using the geometry in lfs2_config
int lfs2_emubd_create(const struct lfs2_config *cfg,
        const struct lfs2_emubd_config *bdcfg);

// Clean up memory associated with block device
int lfs2_emubd_destroy(const struct lfs2_config *cfg);

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


/// Additional extended API for driving test features ///

// A CRC of a block for debugging purposes
int lfs2_emubd_crc(const struct lfs2_config *cfg,
        lfs2_block_t block, uint32_t *crc);

// A CRC of the entire block device for debugging purposes
int lfs2_emubd_bdcrc(const struct lfs2_config *cfg, uint32_t *crc);

// Get total amount of bytes read
lfs2_emubd_sio_t lfs2_emubd_readed(const struct lfs2_config *cfg);

// Get total amount of bytes programmed
lfs2_emubd_sio_t lfs2_emubd_proged(const struct lfs2_config *cfg);

// Get total amount of bytes erased
lfs2_emubd_sio_t lfs2_emubd_erased(const struct lfs2_config *cfg);

// Manually set amount of bytes read
int lfs2_emubd_setreaded(const struct lfs2_config *cfg, lfs2_emubd_io_t readed);

// Manually set amount of bytes programmed
int lfs2_emubd_setproged(const struct lfs2_config *cfg, lfs2_emubd_io_t proged);

// Manually set amount of bytes erased
int lfs2_emubd_seterased(const struct lfs2_config *cfg, lfs2_emubd_io_t erased);

// Get simulated wear on a given block
lfs2_emubd_swear_t lfs2_emubd_wear(const struct lfs2_config *cfg,
        lfs2_block_t block);

// Manually set simulated wear on a given block
int lfs2_emubd_setwear(const struct lfs2_config *cfg,
        lfs2_block_t block, lfs2_emubd_wear_t wear);

// Get the remaining power-cycles
lfs2_emubd_spowercycles_t lfs2_emubd_powercycles(
        const struct lfs2_config *cfg);

// Manually set the remaining power-cycles
int lfs2_emubd_setpowercycles(const struct lfs2_config *cfg,
        lfs2_emubd_powercycles_t power_cycles);

// Create a copy-on-write copy of the state of this block device
int lfs2_emubd_copy(const struct lfs2_config *cfg, lfs2_emubd_t *copy);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
