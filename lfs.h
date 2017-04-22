/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_H
#define LFS_H

#include "lfs_config.h"


// The littefs constants
enum lfs_error {
    LFS_ERROR_OK       = 0,
    LFS_ERROR_CORRUPT  = -3,
    LFS_ERROR_NO_ENTRY = -4,
    LFS_ERROR_EXISTS   = -5,
    LFS_ERROR_NOT_DIR  = -6,
    LFS_ERROR_IS_DIR   = -7,
    LFS_ERROR_INVALID  = -8,
    LFS_ERROR_NO_SPACE = -9,
};

enum lfs_type {
    LFS_TYPE_REG        = 0x01,
    LFS_TYPE_DIR        = 0x02,
    LFS_TYPE_SUPERBLOCK = 0x10,
};

enum lfs_open_flags {
    LFS_O_RDONLY = 0,
    LFS_O_WRONLY = 1,
    LFS_O_RDWR   = 2,
    LFS_O_CREAT  = 0x020,
    LFS_O_EXCL   = 0x040,
    LFS_O_TRUNC  = 0x080,
    LFS_O_APPEND = 0x100,
    LFS_O_SYNC   = 0x200,
};


// Configuration provided during initialization of the littlefs
struct lfs_config {
    // Opaque user provided context
    void *context;

    // Read a region in a block
    int (*read)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, lfs_size_t size, void *buffer);

    // Program a region in a block. The block must have previously
    // been erased.
    int (*prog)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, lfs_size_t size, const void *buffer);

    // Erase a block. A block must be erased before being programmed.
    // The state of an erased block is undefined.
    int (*erase)(const struct lfs_config *c, lfs_block_t block);

    // Sync the state of the underlying block device
    int (*sync)(const struct lfs_config *c);

    // Minimum size of a read. This may be larger than the physical
    // read size to cache reads from the block device.
    lfs_size_t read_size;

    // Minimum size of a program. This may be larger than the physical
    // program size to cache programs to the block device.
    lfs_size_t prog_size;

    // Size of an erasable block.
    lfs_size_t block_size;

    // Number of erasable blocks on the device.
    lfs_size_t block_count;
};

// File info structure
struct lfs_info {
    // Type of the file, either REG or DIR
    uint8_t type;

    // Size of the file, only valid for REG files
    lfs_size_t size;

    // Name of the file stored as a null-terminated string
    char name[LFS_NAME_MAX+1];
};


// Internal data structures
typedef struct lfs_entry {
    lfs_block_t pair[2];
    lfs_off_t off;

    struct lfs_disk_entry {
        uint16_t type;
        uint16_t len;
        union {
            struct {
                lfs_block_t head;
                lfs_size_t size;
            } file;
            lfs_block_t dir[2];
        } u;
    } d;
} lfs_entry_t;

typedef struct lfs_file {
    lfs_block_t head;
    lfs_size_t size;

    lfs_block_t wblock;
    uint32_t windex;

    lfs_block_t rblock;
    uint32_t rindex;
    lfs_off_t roff;

    struct lfs_entry entry;
} lfs_file_t;

typedef struct lfs_dir {
    lfs_block_t pair[2];
    lfs_off_t off;

    struct lfs_disk_dir {
        uint32_t rev;
        lfs_size_t size;
        lfs_block_t tail[2];
    } d;
} lfs_dir_t;

typedef struct lfs_superblock {
    lfs_block_t dir[2]; //TODO rm me?
    lfs_off_t off;

    struct lfs_disk_superblock {
        uint16_t type;
        uint16_t len;
        uint32_t version;
        char magic[8];
        uint32_t block_size;
        uint32_t block_count;
        lfs_block_t root[2];
    } d;
} lfs_superblock_t;


// Little filesystem type
typedef struct lfs {
    const struct lfs_config *cfg;
    lfs_size_t words;       // number of 32-bit words that can fit in a block

    lfs_block_t root[2];
    struct {
        lfs_block_t begin;
        lfs_block_t end;
    } free;

    uint32_t lookahead[LFS_CFG_LOOKAHEAD/32];
} lfs_t;


// Functions
int lfs_format(lfs_t *lfs, const struct lfs_config *config);
int lfs_mount(lfs_t *lfs, const struct lfs_config *config);
int lfs_unmount(lfs_t *lfs);

int lfs_remove(lfs_t *lfs, const char *path);
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath);
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info);

int lfs_mkdir(lfs_t *lfs, const char *path);
int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path);
int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir);
int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info);

int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags);
int lfs_file_close(lfs_t *lfs, lfs_file_t *file);
lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size);
lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size);

int lfs_deorphan(lfs_t *lfs);
int lfs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data);


#endif
