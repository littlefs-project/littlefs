#include <string.h>
#include "lfs.h"

#define STORAGE_SIZE 1024*1024

static uint8_t disk_buffer[STORAGE_SIZE];

int min(int a, int b) {
    if(a < b) {
        return a;
    }
    return b;
}

static int read(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, void *buffer, lfs_size_t size) {
    memcpy(buffer, &disk_buffer[block*c->read_size + off], size);
    return 0;
}

static int prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    memcpy(&disk_buffer[block*cfg->read_size + off], buffer, size);
    return 0;
}

static int erase(const struct lfs_config *cfg, lfs_block_t block) {
    (void)cfg;
    (void)block;
    return 0;
}

static int sync(const struct lfs_config *cfg) {
    // sync is a noop
    (void)cfg;
    return 0;
}

// configuration of the filesystem is provided by this struct
const struct lfs_config filesystem_cfg = {
    // block device operations
    .read  = read,
    .prog  = prog,
    .erase = erase,
    .sync  = sync,

    // block device configuration
    .read_size = 1024,
    .prog_size = 1024,
    .block_size = 1024,
    .block_count = 1024,
    .cache_size = 1024,
    .lookahead_size = 1024,
    .block_cycles = 500,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if(size == 0) {
        return -1;
    }
    memset(disk_buffer, 0, sizeof(disk_buffer));

    // variables used by the filesystem
    lfs_t lfs;

    // Copy fuzzed data into fake storage device.
    memcpy(disk_buffer, data, min(size, STORAGE_SIZE));

    // mount the filesystem
    int err = lfs_mount(&lfs, &filesystem_cfg);

    if (err) {
      return 0;
    }

    // release any resources we were using
    lfs_unmount(&lfs);

    return 0;
}

#ifdef CUSTOM_MUTATOR

// Forward-declare the libFuzzer's mutator callback.
size_t LLVMFuzzerMutate(uint8_t *Data, size_t Size, size_t MaxSize);

size_t LLVMFuzzerCustomMutator(uint8_t *data, size_t size,
                                           size_t max_size, unsigned int seed) {
    (void)seed;
    memset(disk_buffer, 0, sizeof(disk_buffer));
    // variables used by the filesystem
    lfs_t lfs;

    // Copy fuzzed data into fake storage device.
    memcpy(disk_buffer, data, min(size, STORAGE_SIZE));

    // Mount the filesystem
    int err = lfs_mount(&lfs, &filesystem_cfg);

    // Reformat if we can't mount the filesystem
    if (err) {
        lfs_format(&lfs, &filesystem_cfg);
        lfs_mount(&lfs, &filesystem_cfg);
    }
    lfs_unmount(&lfs);
    memcpy(data, disk_buffer, min(STORAGE_SIZE, max_size));

    return LLVMFuzzerMutate(data, size, max_size);
}

#endif  // CUSTOM_MUTATOR
