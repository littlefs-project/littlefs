/*
 * Emulating block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_EMUBD_H
#define LFS_EMUBD_H

#include "lfs.h"
#include "lfs_util.h"
#include "bd/lfs_rambd.h"
#include "bd/lfs_filebd.h"

#ifdef __cplusplus
extern "C"
{
#endif


// Block device specific tracing
#ifndef LFS_EMUBD_TRACE
#ifdef LFS_EMUBD_YES_TRACE
#define LFS_EMUBD_TRACE(...) LFS_TRACE(__VA_ARGS__)
#else
#define LFS_EMUBD_TRACE(...)
#endif
#endif

// Mode determining how "bad-blocks" behave during testing. This simulates
// some real-world circumstances such as progs not sticking (prog-noop),
// a readonly disk (erase-noop), ECC failures (read-error), and of course,
// random bit failures (prog-flip, read-flip)
typedef enum lfs_emubd_badblock_behavior {
    LFS_EMUBD_BADBLOCK_PROGERROR    = 0, // Error on prog
    LFS_EMUBD_BADBLOCK_ERASEERROR   = 1, // Error on erase
    LFS_EMUBD_BADBLOCK_READERROR    = 2, // Error on read
    LFS_EMUBD_BADBLOCK_PROGNOOP     = 3, // Prog does nothing silently
    LFS_EMUBD_BADBLOCK_ERASENOOP    = 4, // Erase does nothing silently
    LFS_EMUBD_BADBLOCK_PROGFLIP     = 5, // Prog flips a bit
    LFS_EMUBD_BADBLOCK_READFLIP     = 6, // Read flips a bit sometimes
} lfs_emubd_badblock_behavior_t;

// Mode determining how power-loss behaves during testing.
typedef enum lfs_emubd_powerloss_behavior {
    LFS_EMUBD_POWERLOSS_NOOP        = 0, // Progs are atomic
    LFS_EMUBD_POWERLOSS_SOMEBITS    = 1, // One bit is progged
    LFS_EMUBD_POWERLOSS_MOSTBITS    = 2, // All-but-one bit is progged
    LFS_EMUBD_POWERLOSS_OOO         = 3, // Blocks are written out-of-order
    LFS_EMUBD_POWERLOSS_METASTABLE  = 4, // Reads may flip a bit
} lfs_emubd_powerloss_behavior_t;

// Type for measuring read/program/erase operations
typedef uint64_t lfs_emubd_io_t;
typedef int64_t lfs_emubd_sio_t;

// Type for measuring wear
typedef uint32_t lfs_emubd_wear_t;
typedef int32_t lfs_emubd_swear_t;

// Type for tracking power-cycles
typedef uint32_t lfs_emubd_powercycles_t;
typedef int32_t lfs_emubd_spowercycles_t;

// Type for delays in nanoseconds
typedef uint64_t lfs_emubd_sleep_t;
typedef int64_t lfs_emubd_ssleep_t;

// emubd config, this is required for testing
struct lfs_emubd_config {
    // 8-bit erase value to use for simulating erases. -1 simulates a noop
    // erase, which is faster than simulating a fixed erase value.
    int32_t erase_value;

    // Number of erase cycles before a block becomes "bad". The exact behavior
    // of bad blocks is controlled by badblock_behavior.
    uint32_t erase_cycles;

    // The mode determining how bad-blocks fail
    lfs_emubd_badblock_behavior_t badblock_behavior;

    // Number of write operations (erase/prog) before triggering a power-loss.
    // power_cycles=0 disables this. The exact behavior of power-loss is
    // controlled by a combination of powerloss_behavior and powerloss_cb.
    lfs_emubd_powercycles_t power_cycles;

    // The mode determining how power-loss affects disk
    lfs_emubd_powerloss_behavior_t powerloss_behavior;

    // Function to call to emulate power-loss. The exact behavior of power-loss
    // is up to the runner to provide.
    void (*powerloss_cb)(void*);

    // Data for power-loss callback
    void *powerloss_data;

    // Seed for prng, which may be used for emulating failed progs. This does
    // not affect normal operation.
    uint32_t seed;

    // Path to file to use as a mirror of the disk. This provides a way to view
    // the current state of the block device.
    const char *disk_path;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs_emubd_sleep_t read_sleep;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs_emubd_sleep_t prog_sleep;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs_emubd_sleep_t erase_sleep;
};

// A reference counted block
typedef struct lfs_emubd_block {
    uint32_t rc;
    lfs_emubd_wear_t wear;
    bool metastable;
    // sign(bad_bit)=0 => randomized on erase
    // sign(bad_bit)=1 => fixed
    lfs_size_t bad_bit;

    uint8_t data[];
} lfs_emubd_block_t;

// Disk mirror
typedef struct lfs_emubd_disk {
    uint32_t rc;
    int fd;
    uint8_t *scratch;
} lfs_emubd_disk_t;

// emubd state
typedef struct lfs_emubd {
    // array of copy-on-write blocks
    lfs_emubd_block_t **blocks;

    // some other test state
    lfs_emubd_io_t readed;
    lfs_emubd_io_t proged;
    lfs_emubd_io_t erased;
    uint32_t prng;
    lfs_emubd_powercycles_t power_cycles;
    lfs_emubd_block_t **ooo_before;
    lfs_emubd_block_t **ooo_after;
    lfs_emubd_disk_t *disk;

    const struct lfs_emubd_config *cfg;
} lfs_emubd_t;


/// Block device API ///

// Create an emulating block device using the geometry in lfs_config
//
// Note that filebd is used if a path is provided, if path is NULL
// emubd will use rambd which can be much faster.
int lfs_emubd_create(const struct lfs_config *cfg, const char *path);
int lfs_emubd_createcfg(const struct lfs_config *cfg, const char *path,
        const struct lfs_emubd_config *bdcfg);

// Clean up memory associated with block device
int lfs_emubd_destroy(const struct lfs_config *cfg);

// Read a block
int lfs_emubd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs_emubd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs_emubd_erase(const struct lfs_config *cfg, lfs_block_t block);

// Sync the block device
int lfs_emubd_sync(const struct lfs_config *cfg);


/// Additional extended API for driving test features ///

// Set the current prng state
int lfs_emubd_seed(const struct lfs_config *cfg, uint32_t seed);

// Get total amount of bytes read
lfs_emubd_sio_t lfs_emubd_readed(const struct lfs_config *cfg);

// Get total amount of bytes programmed
lfs_emubd_sio_t lfs_emubd_proged(const struct lfs_config *cfg);

// Get total amount of bytes erased
lfs_emubd_sio_t lfs_emubd_erased(const struct lfs_config *cfg);

// Manually set amount of bytes read
int lfs_emubd_setreaded(const struct lfs_config *cfg, lfs_emubd_io_t readed);

// Manually set amount of bytes programmed
int lfs_emubd_setproged(const struct lfs_config *cfg, lfs_emubd_io_t proged);

// Manually set amount of bytes erased
int lfs_emubd_seterased(const struct lfs_config *cfg, lfs_emubd_io_t erased);

// Get simulated wear on a given block
lfs_emubd_swear_t lfs_emubd_wear(const struct lfs_config *cfg,
        lfs_block_t block);

// Manually set simulated wear on a given block
int lfs_emubd_setwear(const struct lfs_config *cfg,
        lfs_block_t block, lfs_emubd_wear_t wear);

// Mark a block as bad, this is equivalent to setting wear to maximum
int lfs_emubd_markbad(const struct lfs_config *cfg, lfs_block_t block);

// Clear any simulated wear on a given block
int lfs_emubd_markgood(const struct lfs_config *cfg, lfs_block_t block);

// Get which bit failed, this changes on erase/power-loss unless manually set
lfs_ssize_t lfs_emubd_badbit(const struct lfs_config *cfg,
        lfs_block_t block);

// Set which bit should fail in a given block
int lfs_emubd_setbadbit(const struct lfs_config *cfg,
        lfs_block_t block, lfs_size_t bit);

// Randomize the bad bit on erase (the default)
int lfs_emubd_randomizebadbit(const struct lfs_config *cfg,
        lfs_block_t block);

// Mark a block as bad and which bit should fail
int lfs_emubd_markbadbit(const struct lfs_config *cfg,
        lfs_block_t block, lfs_size_t bit);

// Flip a bit in a given block, intended for emulating bit errors
int lfs_emubd_flipbit(const struct lfs_config *cfg,
        lfs_block_t block, lfs_size_t bit);

// Get the remaining power-cycles
lfs_emubd_spowercycles_t lfs_emubd_powercycles(
        const struct lfs_config *cfg);

// Manually set the remaining power-cycles
int lfs_emubd_setpowercycles(const struct lfs_config *cfg,
        lfs_emubd_powercycles_t power_cycles);

// Create a copy-on-write copy of the state of this block device
int lfs_emubd_copy(const struct lfs_config *cfg, lfs_emubd_t *copy);


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
