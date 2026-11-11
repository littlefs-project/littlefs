/*
 * Block device emulated in a file
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs3_filebd.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#endif

int lfs3_filebd_create(const struct lfs3_cfg *cfg, const char *path) {
    lfs3_filebd_t *bd = cfg->context;

    // open file
    #ifdef _WIN32
    bd->fd = open(path, O_RDWR | O_CREAT | O_BINARY, 0666);
    #else
    bd->fd = open(path, O_RDWR | O_CREAT, 0666);
    #endif

    if (bd->fd < 0) {
        return -errno;
    }

    return 0;
}

int lfs3_filebd_destroy(const struct lfs3_cfg *cfg) {
    lfs3_filebd_t *bd = cfg->context;

    int err = close(bd->fd);
    if (err < 0) {
        return -errno;
    }

    return 0;
}

int lfs3_filebd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size) {
    lfs3_filebd_t *bd = cfg->context;

    // check if read is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->read_size == 0);
    LFS3_ASSERT(size % cfg->read_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // zero for reproducibility (in case file is truncated)
    memset(buffer, 0, size);

    // read
    off_t res1 = lseek(bd->fd,
            (off_t)block*cfg->block_size + (off_t)off, SEEK_SET);
    if (res1 < 0) {
        return -errno;
    }

    ssize_t res2 = read(bd->fd, buffer, size);
    if (res2 < 0) {
        return -errno;
    }

    return 0;
}

int lfs3_filebd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size) {
    lfs3_filebd_t *bd = cfg->context;

    // check if write is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->prog_size == 0);
    LFS3_ASSERT(size % cfg->prog_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // program data
    off_t res1 = lseek(bd->fd,
            (off_t)block*cfg->block_size + (off_t)off, SEEK_SET);
    if (res1 < 0) {
        return -errno;
    }

    ssize_t res2 = write(bd->fd, buffer, size);
    if (res2 < 0) {
        return -errno;
    }

    return 0;
}

int lfs3_filebd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block) {
    // check if erase is valid
    LFS3_ASSERT(block < cfg->block_count);

    // erase is a noop
    (void)cfg;
    (void)block;

    return 0;
}

int lfs3_filebd_sync(const struct lfs3_cfg *cfg) {
    lfs3_filebd_t *bd = cfg->context;

    // file sync
    #ifdef _WIN32
    int err = FlushFileBuffers((HANDLE) _get_osfhandle(bd->fd)) ? 0 : -1;
    #else
    int err = fsync(bd->fd);
    #endif
    if (err) {
        return -errno;
    }

    return 0;
}

