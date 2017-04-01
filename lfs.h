/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_H
#define LFS_H

#include "lfs_config.h"
#include "lfs_bd.h"


// Data structures
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
    LFS_TYPE_REG = 1,
    LFS_TYPE_DIR = 2,
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


struct lfs_config {
    lfs_bd_t *bd;
    const struct lfs_bd_ops *bd_ops;

    lfs_size_t read_size;
    lfs_size_t prog_size;

    lfs_size_t block_size;
    lfs_size_t block_count;
};

struct lfs_info {
    uint8_t type;
    lfs_size_t size;
    char name[LFS_NAME_MAX+1];
};

typedef struct lfs_entry {
    lfs_block_t dir[2];
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
    lfs_block_t pair[2];
    struct lfs_disk_superblock {
        uint32_t rev;
        uint32_t size;
        lfs_block_t root[2];
        char magic[8];
        uint32_t block_size;
        uint32_t block_count;
    } d;
} lfs_superblock_t;

// Little filesystem type
typedef struct lfs {
    lfs_size_t read_size;   // size of read
    lfs_size_t prog_size;   // size of program
    lfs_size_t block_size;  // size of erase (block size)
    lfs_size_t block_count; // number of erasable blocks
    lfs_size_t words;       // number of 32-bit words that can fit in a block

    lfs_bd_t *bd;
    const struct lfs_bd_ops *bd_ops;

    lfs_block_t root[2];
    lfs_block_t cwd[2];
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

#endif
