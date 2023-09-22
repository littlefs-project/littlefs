/*
 * Block device emulated in a file
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs2_filebd.h"

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#endif

int lfs2_filebd_create(const struct lfs2_config *cfg, const char *path,
        const struct lfs2_filebd_config *bdcfg) {
    LFS2_FILEBD_TRACE("lfs2_filebd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p}, "
                "\"%s\", "
                "%p {.read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".erase_size=%"PRIu32", .erase_count=%"PRIu32"})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            path,
            (void*)bdcfg,
            bdcfg->read_size, bdcfg->prog_size, bdcfg->erase_size,
            bdcfg->erase_count);
    lfs2_filebd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // open file
    #ifdef _WIN32
    bd->fd = open(path, O_RDWR | O_CREAT | O_BINARY, 0666);
    #else
    bd->fd = open(path, O_RDWR | O_CREAT, 0666);
    #endif

    if (bd->fd < 0) {
        int err = -errno;
        LFS2_FILEBD_TRACE("lfs2_filebd_create -> %d", err);
        return err;
    }

    LFS2_FILEBD_TRACE("lfs2_filebd_create -> %d", 0);
    return 0;
}

int lfs2_filebd_destroy(const struct lfs2_config *cfg) {
    LFS2_FILEBD_TRACE("lfs2_filebd_destroy(%p)", (void*)cfg);
    lfs2_filebd_t *bd = cfg->context;
    int err = close(bd->fd);
    if (err < 0) {
        err = -errno;
        LFS2_FILEBD_TRACE("lfs2_filebd_destroy -> %d", err);
        return err;
    }
    LFS2_FILEBD_TRACE("lfs2_filebd_destroy -> %d", 0);
    return 0;
}

int lfs2_filebd_read(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, void *buffer, lfs2_size_t size) {
    LFS2_FILEBD_TRACE("lfs2_filebd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs2_filebd_t *bd = cfg->context;

    // check if read is valid
    LFS2_ASSERT(block < bd->cfg->erase_count);
    LFS2_ASSERT(off  % bd->cfg->read_size == 0);
    LFS2_ASSERT(size % bd->cfg->read_size == 0);
    LFS2_ASSERT(off+size <= bd->cfg->erase_size);

    // zero for reproducibility (in case file is truncated)
    memset(buffer, 0, size);

    // read
    off_t res1 = lseek(bd->fd,
            (off_t)block*bd->cfg->erase_size + (off_t)off, SEEK_SET);
    if (res1 < 0) {
        int err = -errno;
        LFS2_FILEBD_TRACE("lfs2_filebd_read -> %d", err);
        return err;
    }

    ssize_t res2 = read(bd->fd, buffer, size);
    if (res2 < 0) {
        int err = -errno;
        LFS2_FILEBD_TRACE("lfs2_filebd_read -> %d", err);
        return err;
    }

    LFS2_FILEBD_TRACE("lfs2_filebd_read -> %d", 0);
    return 0;
}

int lfs2_filebd_prog(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, const void *buffer, lfs2_size_t size) {
    LFS2_FILEBD_TRACE("lfs2_filebd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs2_filebd_t *bd = cfg->context;

    // check if write is valid
    LFS2_ASSERT(block < bd->cfg->erase_count);
    LFS2_ASSERT(off  % bd->cfg->prog_size == 0);
    LFS2_ASSERT(size % bd->cfg->prog_size == 0);
    LFS2_ASSERT(off+size <= bd->cfg->erase_size);

    // program data
    off_t res1 = lseek(bd->fd,
            (off_t)block*bd->cfg->erase_size + (off_t)off, SEEK_SET);
    if (res1 < 0) {
        int err = -errno;
        LFS2_FILEBD_TRACE("lfs2_filebd_prog -> %d", err);
        return err;
    }

    ssize_t res2 = write(bd->fd, buffer, size);
    if (res2 < 0) {
        int err = -errno;
        LFS2_FILEBD_TRACE("lfs2_filebd_prog -> %d", err);
        return err;
    }

    LFS2_FILEBD_TRACE("lfs2_filebd_prog -> %d", 0);
    return 0;
}

int lfs2_filebd_erase(const struct lfs2_config *cfg, lfs2_block_t block) {
    LFS2_FILEBD_TRACE("lfs2_filebd_erase(%p, 0x%"PRIx32" (%"PRIu32"))",
            (void*)cfg, block, ((lfs2_file_t*)cfg->context)->cfg->erase_size);
    lfs2_filebd_t *bd = cfg->context;

    // check if erase is valid
    LFS2_ASSERT(block < bd->cfg->erase_count);

    // erase is a noop
    (void)block;

    LFS2_FILEBD_TRACE("lfs2_filebd_erase -> %d", 0);
    return 0;
}

int lfs2_filebd_sync(const struct lfs2_config *cfg) {
    LFS2_FILEBD_TRACE("lfs2_filebd_sync(%p)", (void*)cfg);

    // file sync
    lfs2_filebd_t *bd = cfg->context;
    #ifdef _WIN32
    int err = FlushFileBuffers((HANDLE) _get_osfhandle(bd->fd)) ? 0 : -1;
    #else
    int err = fsync(bd->fd);
    #endif
    if (err) {
        err = -errno;
        LFS2_FILEBD_TRACE("lfs2_filebd_sync -> %d", 0);
        return err;
    }

    LFS2_FILEBD_TRACE("lfs2_filebd_sync -> %d", 0);
    return 0;
}
