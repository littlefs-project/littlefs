/*
 * emubd - High-level emulating block device with many bells and
 * whistles for testing powerloss, wear, etc.
 *
 * Note emubd always backs the block device in RAM. Consider using
 * kiwibd if you need a block device larger than the available RAM on
 * the system.
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS3_EMUBD_H
#define LFS3_EMUBD_H

#include "lfs3.h"
#include "lfs3_util.h"


// Block device specific tracing
#ifndef LFS3_EMUBD_TRACE
#ifdef LFS3_EMUBD_YES_TRACE
#define LFS3_EMUBD_TRACE(...) LFS3_TRACE(__VA_ARGS__)
#else
#define LFS3_EMUBD_TRACE(...)
#endif
#endif

// Mode determining how "bad-blocks" behave during testing. This simulates
// some real-world circumstances such as progs not sticking (prog-noop),
// a readonly disk (erase-noop), ECC failures (read-error), and of course,
// random bit failures (prog-flip, read-flip)
typedef enum lfs3_emubd_badblock_behavior {
    LFS3_EMUBD_BADBLOCK_PROGERROR    = 0, // Error on prog
    LFS3_EMUBD_BADBLOCK_ERASEERROR   = 1, // Error on erase
    LFS3_EMUBD_BADBLOCK_READERROR    = 2, // Error on read
    LFS3_EMUBD_BADBLOCK_PROGNOOP     = 3, // Prog does nothing silently
    LFS3_EMUBD_BADBLOCK_ERASENOOP    = 4, // Erase does nothing silently
    LFS3_EMUBD_BADBLOCK_PROGFLIP     = 5, // Prog flips a bit
    LFS3_EMUBD_BADBLOCK_READFLIP     = 6, // Read flips a bit sometimes
    LFS3_EMUBD_BADBLOCK_MANUAL       = 7, // Bits require manual flipping
} lfs3_emubd_badblock_behavior_t;

// Mode determining how powerloss behaves during testing.
typedef enum lfs3_emubd_powerloss_behavior {
    LFS3_EMUBD_POWERLOSS_ATOMIC      = 0, // Progs are atomic
    LFS3_EMUBD_POWERLOSS_SOMEBITS    = 1, // One bit is progged
    LFS3_EMUBD_POWERLOSS_MOSTBITS    = 2, // All-but-one bit is progged
    LFS3_EMUBD_POWERLOSS_OOO         = 3, // Blocks are written out-of-order
    LFS3_EMUBD_POWERLOSS_METASTABLE  = 4, // Reads may flip a bit
} lfs3_emubd_powerloss_behavior_t;

// Type for measuring read/program/erase operations
typedef uint64_t lfs3_emubd_io_t;
typedef int64_t lfs3_emubd_sio_t;

// Type for measuring wear
typedef uint32_t lfs3_emubd_wear_t;
typedef int32_t lfs3_emubd_swear_t;

// Type for tracking power-cycles
typedef uint32_t lfs3_emubd_powercycles_t;
typedef int32_t lfs3_emubd_spowercycles_t;

// Type for delays in nanoseconds
typedef uint64_t lfs3_emubd_ns_t;
typedef int64_t lfs3_emubd_sns_t;

// emubd config, this is required for testing
struct lfs3_emubd_cfg {
    // 8-bit erase value to use for simulating erases. -1 simulates a noop
    // erase, which is faster than simulating a fixed erase value. -2 emulates
    // nor-masking, which is useful for testing other filesystems (littlefs
    // does _not_ rely on this!).
    int32_t erase_value;

    // Simulated read width, this is only used for simulated read timing
    // and emulates the physical read hardware on the device. Defaults
    // to 1 byte.
    lfs3_size_t read_width;

    // Simulated prog width, this is only used for simulated prog timing
    // and emulates the physical prog hardware on the device. Defaults
    // to 1 byte.
    lfs3_size_t prog_width;

    // Simulated erase width, this is only used for simulated erase timing
    // and emulates physical erase hardware on the device. Defaults to 1
    // byte.
    lfs3_size_t erase_width;

    // Simulated per-byte read timing in nanoseconds, this is added to
    // simtime each read call after aligning up to the necessary number
    // of read_widths to emulate the read operation.
    lfs3_emubd_ns_t read_timing;

    // Simulated per-byte prog timing in nanoseconds, this is added to
    // simtime each prog call after aligning up to the necessary number
    // of prog_widths to emulate the prog operation.
    lfs3_emubd_ns_t prog_timing;

    // Simulated per-byte erase timing in nanoseconds, this is added to
    // simtime each erase call after aligning up to the necessary number
    // of erase_widths to emulate the erase operation.
    lfs3_emubd_ns_t erase_timing;

    // Simulated per-byte read timing in nanoseconds, this ignores
    // read_width and can be used to simulate relevant bus overhead.
    lfs3_emubd_ns_t readed_timing;

    // Simulated per-byte prog timing in nanoseconds, this ignores
    // prog_width and can be used to simulate relevant bus overhead.
    lfs3_emubd_ns_t progged_timing;

    // Simulated per-byte erase timing in nanoseconds, this ignores
    // erase_width and can be used to simulate relevant bus overhead.
    lfs3_emubd_ns_t erased_timing;

    // Artificial read transaction delay in nanoseconds, there is no
    // purpose for this other than slowing down the simulation.
    lfs3_emubd_ns_t read_sleep;

    // Artificial prog transaction delay in nanoseconds, there is no
    // purpose for this other than slowing down the simulation.
    lfs3_emubd_ns_t prog_sleep;

    // Artificial erase transaction delay in nanoseconds, there is no
    // purpose for this other than slowing down the simulation.
    lfs3_emubd_ns_t erase_sleep;

    // Number of erase cycles before a block becomes "bad". The exact behavior
    // of bad blocks is controlled by badblock_behavior.
    uint32_t erase_cycles;

    // The mode determining how bad-blocks fail
    lfs3_emubd_badblock_behavior_t badblock_behavior;

    // Number of write operations (erase/prog) before triggering a powerloss.
    // power_cycles=0 disables this. The exact behavior of powerloss is
    // controlled by a combination of powerloss_behavior and powerloss_cb.
    lfs3_emubd_powercycles_t power_cycles;

    // The mode determining how powerloss affects disk
    lfs3_emubd_powerloss_behavior_t powerloss_behavior;

    // Function to call to emulate powerloss. The exact behavior of powerloss
    // is up to the runner to provide.
    void (*powerloss_cb)(void*);

    // Data for powerloss callback
    void *powerloss_data;

    // Seed for prng, which may be used for emulating failed progs. This does
    // not affect normal operation.
    uint32_t seed;
};

// A reference counted block
typedef struct lfs3_emubd_block {
    uint32_t rc;
    lfs3_emubd_wear_t wear;
    bool metastable;
    // sign(bad_bit)=0 => randomized on erase
    // sign(bad_bit)=1 => fixed
    lfs3_size_t bad_bit;

    uint8_t data[];
} lfs3_emubd_block_t;

// Disk mirror
typedef struct lfs3_emubd_disk {
    uint32_t rc;
    int fd;
    uint8_t *scratch;
} lfs3_emubd_disk_t;

// emubd state
typedef struct lfs3_emubd {
    // array of copy-on-write blocks
    lfs3_emubd_block_t **blocks;

    // sim state
    uint32_t paused;
    // amount read/progged/erased
    lfs3_emubd_io_t reads;
    lfs3_emubd_io_t progs;
    lfs3_emubd_io_t erases;
    lfs3_emubd_io_t readed;
    lfs3_emubd_io_t progged;
    lfs3_emubd_io_t erased;

    // some other test state
    uint32_t prng;
    lfs3_emubd_powercycles_t power_cycles;
    lfs3_emubd_block_t **ooo_before;
    lfs3_emubd_block_t **ooo_after;
    lfs3_emubd_disk_t *disk;

    const struct lfs3_emubd_cfg *cfg;
} lfs3_emubd_t;


/// Block device API ///

// Create an emulating block device using the geometry in lfs3_cfg
//
// If path is provided, emubd will mirror the block device in the file.
// This provides a way to view the current state of the block device,
// but does not eliminate the RAM requirement.
//
int lfs3_emubd_create(const struct lfs3_cfg *cfg, const char *path);
int lfs3_emubd_createcfg(const struct lfs3_cfg *cfg, const char *path,
        const struct lfs3_emubd_cfg *bdcfg);

// Clean up memory associated with block device
int lfs3_emubd_destroy(const struct lfs3_cfg *cfg);

// Read a block
int lfs3_emubd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs3_emubd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs3_emubd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block);

// Sync the block device
int lfs3_emubd_sync(const struct lfs3_cfg *cfg);


/// Additional emubd features for testing ///

// Get simulated runtime in nanoseconds
lfs3_emubd_sns_t lfs3_emubd_simtime(const struct lfs3_cfg *cfg);

// Reset simulation counters
int lfs3_emubd_simreset(const struct lfs3_cfg *cfg);

// Pause simulation counters
int lfs3_emubd_simpause(const struct lfs3_cfg *cfg);

// Resume simulation counters
int lfs3_emubd_simresume(const struct lfs3_cfg *cfg);

// Get total number of read transactions
lfs3_emubd_sio_t lfs3_emubd_reads(const struct lfs3_cfg *cfg);

// Get total number of prog transactions
lfs3_emubd_sio_t lfs3_emubd_progs(const struct lfs3_cfg *cfg);

// Get total number of erase transactions
lfs3_emubd_sio_t lfs3_emubd_erases(const struct lfs3_cfg *cfg);

// Get total amount of bytes read
lfs3_emubd_sio_t lfs3_emubd_readed(const struct lfs3_cfg *cfg);

// Get total amount of bytes programmed
lfs3_emubd_sio_t lfs3_emubd_progged(const struct lfs3_cfg *cfg);

// Get total amount of bytes erased
lfs3_emubd_sio_t lfs3_emubd_erased(const struct lfs3_cfg *cfg);

// Get simulated wear on a given block
lfs3_emubd_swear_t lfs3_emubd_wear(const struct lfs3_cfg *cfg,
        lfs3_block_t block);

// Manually set simulated wear on a given block
int lfs3_emubd_setwear(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_emubd_wear_t wear);

// Mark a block as bad, this is equivalent to setting wear to maximum
int lfs3_emubd_mkbad(const struct lfs3_cfg *cfg, lfs3_block_t block);

// Clear any simulated wear on a given block
int lfs3_emubd_mkgood(const struct lfs3_cfg *cfg, lfs3_block_t block);

// Get which bit failed, this changes on erase/powerloss unless manually set
lfs3_ssize_t lfs3_emubd_badbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block);

// Set which bit should fail in a given block
int lfs3_emubd_setbadbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_size_t bit);

// Randomize the bad bit on erase (the default)
int lfs3_emubd_randomizebadbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block);

// Mark a block as bad and which bit should fail
int lfs3_emubd_mkbadbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_size_t bit);

// Flip a bit in a given block, intended for emulating bit errors
int lfs3_emubd_flipbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_size_t bit);

// Flip all bits marked as bad
int lfs3_emubd_flip(const struct lfs3_cfg *cfg);

// Get the remaining power-cycles
lfs3_emubd_spowercycles_t lfs3_emubd_powercycles(
        const struct lfs3_cfg *cfg);

// Manually set the remaining power-cycles
int lfs3_emubd_setpowercycles(const struct lfs3_cfg *cfg,
        lfs3_emubd_powercycles_t power_cycles);

// Get a pseudo-random number from emubd's internal prng
uint32_t lfs3_emubd_prng(const struct lfs3_cfg *cfg);

// Set the current prng state
void lfs3_emubd_seed(const struct lfs3_cfg *cfg, uint32_t seed);

// Create a copy-on-write copy of the state of this block device
int lfs3_emubd_cpy(const struct lfs3_cfg *cfg, lfs3_emubd_t *copy);


#endif
