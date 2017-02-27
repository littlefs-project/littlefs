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

struct lfs_free_list {
    lfs_ino_t head;
    lfs_word_t ioff;
    lfs_word_t icount;
    lfs_word_t rev;
};

typedef struct lfs {
    lfs_bd_t *bd;
    const struct lfs_bd_ops *ops;

    struct lfs_free_list free;
    struct lfs_bd_info info;
} lfs_t;

lfs_error_t lfs_create(lfs_t *lfs, lfs_bd_t *bd, const struct lfs_bd_ops *bd_ops);
lfs_error_t lfs_format(lfs_t *lfs);

#endif
