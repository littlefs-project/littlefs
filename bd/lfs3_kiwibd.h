/*
 * kiwibd - A lightweight variant of emubd, useful for emulating large
 * disks backed by a file or in RAM.
 *
 * Unlike emubd, file-backed disks are _not_ mirrored in RAM. kiwibd has
 * fewer features than emubd, prioritizing speed for benchmarking.
 *
 *
 */
#ifndef LFS3_KIWIBD_H
#define LFS3_KIWIBD_H

#include "lfs3.h"
#include "lfs3_util.h"


// Block device specific tracing
#ifndef LFS3_KIWIBD_TRACE
#ifdef LFS3_KIWIBD_YES_TRACE
#define LFS3_KIWIBD_TRACE(...) LFS3_TRACE(__VA_ARGS__)
#else
#define LFS3_KIWIBD_TRACE(...)
#endif
#endif

// Type for measuring read/program/erase operations
typedef uint64_t lfs3_kiwibd_io_t;
typedef int64_t lfs3_kiwibd_sio_t;

// Type for delays in nanoseconds
typedef uint64_t lfs3_kiwibd_sleep_t;
typedef int64_t lfs3_kiwibd_ssleep_t;

// kiwibd config, this is required for testing
struct lfs3_kiwibd_cfg {
    // 8-bit erase value to use for simulating erases. -1 simulates a noop
    // erase, which is faster than simulating a fixed erase value. -2 emulates
    // nor-masking, which is useful for testing other filesystems (littlefs
    // does _not_ rely on this!).
    int32_t erase_value;

    // Optional statically allocated buffer for the block device. Ignored
    // if disk_path is provided.
    void *buffer;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs3_kiwibd_sleep_t read_sleep;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs3_kiwibd_sleep_t prog_sleep;

    // Artificial delay in nanoseconds, there is no purpose for this other
    // than slowing down the simulation.
    lfs3_kiwibd_sleep_t erase_sleep;
};

// kiwibd state
typedef struct lfs3_kiwibd {
    // backing disk
    int fd;
    union {
        uint8_t *scratch;
        uint8_t *mem;
    } u;

    // amount read/progged/erased
    lfs3_kiwibd_io_t readed;
    lfs3_kiwibd_io_t proged;
    lfs3_kiwibd_io_t erased;

    const struct lfs3_kiwibd_cfg *cfg;
} lfs3_kiwibd_t;


/// Block device API ///

// Create a kiwibd using the geometry in lfs3_cfg
//
// If path is provided, kiwibd will use the file to back the block
// device, allowing emulation of block devices > available RAM.
//
int lfs3_kiwibd_create(const struct lfs3_cfg *cfg, const char *path);
int lfs3_kiwibd_createcfg(const struct lfs3_cfg *cfg, const char *path,
        const struct lfs3_kiwibd_cfg *bdcfg);

// Clean up memory associated with block device
int lfs3_kiwibd_destroy(const struct lfs3_cfg *cfg);

// Read a block
int lfs3_kiwibd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size);

// Program a block
//
// The block must have previously been erased.
int lfs3_kiwibd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size);

// Erase a block
//
// A block must be erased before being programmed. The
// state of an erased block is undefined.
int lfs3_kiwibd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block);

// Sync the block device
int lfs3_kiwibd_sync(const struct lfs3_cfg *cfg);


/// Additional kiwibd features ///

// Get total amount of bytes read
lfs3_kiwibd_sio_t lfs3_kiwibd_readed(const struct lfs3_cfg *cfg);

// Get total amount of bytes programmed
lfs3_kiwibd_sio_t lfs3_kiwibd_proged(const struct lfs3_cfg *cfg);

// Get total amount of bytes erased
lfs3_kiwibd_sio_t lfs3_kiwibd_erased(const struct lfs3_cfg *cfg);

// Manually set amount of bytes read
int lfs3_kiwibd_setreaded(const struct lfs3_cfg *cfg,
        lfs3_kiwibd_io_t readed);

// Manually set amount of bytes programmed
int lfs3_kiwibd_setproged(const struct lfs3_cfg *cfg,
        lfs3_kiwibd_io_t proged);

// Manually set amount of bytes erased
int lfs3_kiwibd_seterased(const struct lfs3_cfg *cfg,
        lfs3_kiwibd_io_t erased);


#endif

