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

enum lfs_open_flags {
    LFS_O_RDONLY = 0,
    LFS_O_WRONLY = 1,
    LFS_O_RDWR   = 2,
    LFS_O_CREAT  = 0x0040,
    LFS_O_EXCL   = 0x0080,
    LFS_O_TRUNC  = 0x0200,
    LFS_O_APPEND = 0x0400,
    LFS_O_SYNC   = 0x1000,
};

typedef struct lfs_free {
    lfs_word_t begin;
    lfs_word_t off;
    lfs_word_t end;

    lfs_disk_struct lfs_disk_free {
        lfs_word_t rev;
        lfs_ino_t head;
        lfs_word_t off;
        lfs_word_t end;
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

typedef struct lfs_file {
    lfs_ino_t head;
    lfs_size_t size;

    lfs_ino_t wblock;
    lfs_word_t windex;

    lfs_ino_t rblock;
    lfs_word_t rindex;
    lfs_off_t roff;

    struct lfs_entry entry;
} lfs_file_t;

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

lfs_error_t lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags);
lfs_error_t lfs_file_close(lfs_t *lfs, lfs_file_t *file);
lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size);
lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size);

#endif
