/*
 * Block device emulated in a file
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs_filebd.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

int lfs_filebd_createcfg(lfs_filebd_t *bd, const char *path,
        const struct lfs_filebd_cfg *cfg) {
    LFS_FILEBD_TRACE("lfs_filebd_createcfg(%p, \"%s\", %p {"
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".erase_size=%"PRIu32", .erase_count=%"PRIu32", "
                ".erase_value=%"PRId32"})",
            (void*)bd, path, (void*)cfg,
            cfg->read_size, cfg->prog_size, cfg->erase_size, cfg->erase_count,
            cfg->erase_value);

    // copy over config
    bd->cfg = *cfg;

    // open file
    bd->fd = open(path, O_RDWR | O_CREAT, 0666);
    if (bd->fd < 0) {
        int err = -errno;
        LFS_FILEBD_TRACE("lfs_filebd_createcfg -> %d", err);
        return err;
    }

    LFS_FILEBD_TRACE("lfs_filebd_createcfg -> %d", 0);
    return 0;
}

int lfs_filebd_destroy(lfs_filebd_t *bd) {
    LFS_FILEBD_TRACE("lfs_filebd_destroy(%p)", (void*)bd);
    int err = close(bd->fd);
    if (err < 0) {
        err = -errno;
        LFS_FILEBD_TRACE("lfs_filebd_destroy -> %d", err);
        return err;
    }
    LFS_FILEBD_TRACE("lfs_filebd_destroy -> %d", 0);
    return 0;
}

int lfs_filebd_read(lfs_filebd_t *bd, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_FILEBD_TRACE("lfs_filebd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)bd, block, off, buffer, size);

    // check if read is valid
    LFS_ASSERT(off  % bd->cfg.read_size == 0);
    LFS_ASSERT(size % bd->cfg.read_size == 0);
    LFS_ASSERT(block < bd->cfg.erase_count);

    // zero for reproducability (in case file is truncated)
    if (bd->cfg.erase_value != -1) {
        memset(buffer, bd->cfg.erase_value, size);
    }

    // read
    off_t res1 = lseek(bd->fd,
            (off_t)block*bd->cfg.erase_size + (off_t)off, SEEK_SET);
    if (res1 < 0) {
        int err = -errno;
        LFS_FILEBD_TRACE("lfs_filebd_read -> %d", err);
        return err;
    }

    ssize_t res2 = read(bd->fd, buffer, size);
    if (res2 < 0) {
        int err = -errno;
        LFS_FILEBD_TRACE("lfs_filebd_read -> %d", err);
        return err;
    }

    LFS_FILEBD_TRACE("lfs_filebd_read -> %d", 0);
    return 0;
}

int lfs_filebd_prog(lfs_filebd_t *bd, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_FILEBD_TRACE("lfs_filebd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)bd, block, off, buffer, size);

    // check if write is valid
    LFS_ASSERT(off  % bd->cfg.prog_size == 0);
    LFS_ASSERT(size % bd->cfg.prog_size == 0);
    LFS_ASSERT(block < bd->cfg.erase_count);

    // check that data was erased? only needed for testing
    if (bd->cfg.erase_value != -1) {
        off_t res1 = lseek(bd->fd,
                (off_t)block*bd->cfg.erase_size + (off_t)off, SEEK_SET);
        if (res1 < 0) {
            int err = -errno;
            LFS_FILEBD_TRACE("lfs_filebd_prog -> %d", err);
            return err;
        }

        for (lfs_off_t i = 0; i < size; i++) {
            uint8_t c;
            ssize_t res2 = read(bd->fd, &c, 1);
            if (res2 < 0) {
                int err = -errno;
                LFS_FILEBD_TRACE("lfs_filebd_prog -> %d", err);
                return err;
            }

            LFS_ASSERT(c == bd->cfg.erase_value);
        }
    }

    // program data
    off_t res1 = lseek(bd->fd,
            (off_t)block*bd->cfg.erase_size + (off_t)off, SEEK_SET);
    if (res1 < 0) {
        int err = -errno;
        LFS_FILEBD_TRACE("lfs_filebd_prog -> %d", err);
        return err;
    }

    ssize_t res2 = write(bd->fd, buffer, size);
    if (res2 < 0) {
        int err = -errno;
        LFS_FILEBD_TRACE("lfs_filebd_prog -> %d", err);
        return err;
    }

    LFS_FILEBD_TRACE("lfs_filebd_prog -> %d", 0);
    return 0;
}

int lfs_filebd_erase(lfs_filebd_t *bd, lfs_block_t block) {
    LFS_FILEBD_TRACE("lfs_filebd_erase(%p, 0x%"PRIx32")", (void*)bd, block);

    // check if erase is valid
    LFS_ASSERT(block < bd->cfg.erase_count);

    // erase, only needed for testing
    if (bd->cfg.erase_value != -1) {
        off_t res1 = lseek(bd->fd, (off_t)block*bd->cfg.erase_size, SEEK_SET);
        if (res1 < 0) {
            int err = -errno;
            LFS_FILEBD_TRACE("lfs_filebd_erase -> %d", err);
            return err;
        }

        for (lfs_off_t i = 0; i < bd->cfg.erase_size; i++) {
            ssize_t res2 = write(bd->fd, &(uint8_t){bd->cfg.erase_value}, 1);
            if (res2 < 0) {
                int err = -errno;
                LFS_FILEBD_TRACE("lfs_filebd_erase -> %d", err);
                return err;
            }
        }
    }

    LFS_FILEBD_TRACE("lfs_filebd_erase -> %d", 0);
    return 0;
}

int lfs_filebd_sync(lfs_filebd_t *bd) {
    LFS_FILEBD_TRACE("lfs_filebd_sync(%p)", (void*)bd);

    // file sync
    int err = fsync(bd->fd);
    if (err) {
        err = -errno;
        LFS_FILEBD_TRACE("lfs_filebd_sync -> %d", 0);
        return err;
    }

    LFS_FILEBD_TRACE("lfs_filebd_sync -> %d", 0);
    return 0;
}
