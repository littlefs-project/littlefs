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
    LFS_ERROR_INVALID  = -7,
};

enum lfs_type {
    LFS_TYPE_REG = 1,
    LFS_TYPE_DIR = 2,
};

typedef struct lfs_free {
    lfs_disk_struct lfs_disk_free {
        lfs_ino_t head;
        lfs_word_t ioff;
        lfs_word_t icount;
        lfs_word_t rev;
    } d;
} lfs_free_t;

typedef struct lfs_dir {
    lfs_ino_t pair[2];
    lfs_off_t i;

    lfs_disk_struct lfs_disk_dir {
        lfs_word_t rev;
        lfs_size_t size;
        lfs_ino_t tail[2];

        struct lfs_disk_free free;
    } d;
} lfs_dir_t; 

typedef struct lfs_entry {
    lfs_ino_t dir[2];
    lfs_off_t off;

    lfs_disk_struct lfs_disk_entry {
        uint16_t type;
        uint16_t len;
        union {
            lfs_disk_struct {
                lfs_ino_t head;
                lfs_size_t size;
            } file;
            lfs_ino_t dir[2];
        } u;
    } d;
} lfs_entry_t;

typedef struct lfs_superblock {
    lfs_ino_t pair[2];
    lfs_disk_struct lfs_disk_superblock {
        lfs_word_t rev;
        uint32_t size;
        lfs_ino_t root[2];
        char magic[8];
        uint32_t block_size;
        uint32_t block_count;
    } d;
} lfs_superblock_t;

// Little filesystem type
typedef struct lfs {
    lfs_bd_t *bd;
    const struct lfs_bd_ops *ops;

    lfs_ino_t cwd[2];
    lfs_free_t free;
    struct lfs_bd_info info;
} lfs_t;

// Functions
lfs_error_t lfs_create(lfs_t *lfs, lfs_bd_t *bd, const struct lfs_bd_ops *bd_ops);
lfs_error_t lfs_format(lfs_t *lfs);
lfs_error_t lfs_mount(lfs_t *lfs);

lfs_error_t lfs_mkdir(lfs_t *lfs, const char *path);


#endif
