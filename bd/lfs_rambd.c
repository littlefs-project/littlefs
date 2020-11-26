/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs_rambd.h"

int lfs_rambd_createcfg(lfs_rambd_t *bd,
        const struct lfs_rambd_cfg *cfg) {
    LFS_RAMBD_TRACE("lfs_filebd_createcfg(%p, %p {"
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".erase_size=%"PRIu32", .erase_count=%"PRIu32", "
                ".erase_value=%"PRId32", .buffer=%p})",
            (void*)bd, (void*)cfg,
            cfg->read_size, cfg->prog_size, cfg->erase_size, cfg->erase_count,
            cfg->erase_value, cfg->buffer);

    // copy over config
    bd->cfg = *cfg;

    // allocate buffer?
    if (bd->cfg.buffer) {
        bd->buffer = bd->cfg.buffer;
    } else {
        bd->buffer = lfs_malloc(bd->cfg.erase_size * bd->cfg.erase_count);
        if (!bd->buffer) {
            LFS_RAMBD_TRACE("lfs_rambd_createcfg -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }
    }

    // zero for reproducability?
    if (bd->cfg.erase_value != -1) {
        memset(bd->buffer, bd->cfg.erase_value,
                bd->cfg.erase_size * bd->cfg.erase_count);
    }

    LFS_RAMBD_TRACE("lfs_rambd_createcfg -> %d", 0);
    return 0;
}

int lfs_rambd_destroy(lfs_rambd_t *bd) {
    LFS_RAMBD_TRACE("lfs_rambd_destroy(%p)", (void*)bd);
    // clean up memory
    if (!bd->cfg.buffer) {
        lfs_free(bd->buffer);
    }
    LFS_RAMBD_TRACE("lfs_rambd_destroy -> %d", 0);
    return 0;
}

int lfs_rambd_read(lfs_rambd_t *bd, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_RAMBD_TRACE("lfs_rambd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)bd, block, off, buffer, size);

    // check if read is valid
    LFS_ASSERT(off  % bd->cfg.read_size == 0);
    LFS_ASSERT(size % bd->cfg.read_size == 0);
    LFS_ASSERT(block < bd->cfg.erase_count);

    // read data
    memcpy(buffer, &bd->buffer[block*bd->cfg.erase_size + off], size);

    LFS_RAMBD_TRACE("lfs_rambd_read -> %d", 0);
    return 0;
}

int lfs_rambd_prog(lfs_rambd_t *bd, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_RAMBD_TRACE("lfs_rambd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)bd, block, off, buffer, size);

    // check if write is valid
    LFS_ASSERT(off  % bd->cfg.prog_size == 0);
    LFS_ASSERT(size % bd->cfg.prog_size == 0);
    LFS_ASSERT(block < bd->cfg.erase_count);

    // check that data was erased? only needed for testing
    if (bd->cfg.erase_value != -1) {
        for (lfs_off_t i = 0; i < size; i++) {
            LFS_ASSERT(bd->buffer[block*bd->cfg.erase_size + off + i] ==
                    bd->cfg.erase_value);
        }
    }

    // program data
    memcpy(&bd->buffer[block*bd->cfg.erase_size + off], buffer, size);

    LFS_RAMBD_TRACE("lfs_rambd_prog -> %d", 0);
    return 0;
}

int lfs_rambd_erase(lfs_rambd_t *bd, lfs_block_t block) {
    LFS_RAMBD_TRACE("lfs_rambd_erase(%p, 0x%"PRIx32")", (void*)bd, block);

    // check if erase is valid
    LFS_ASSERT(block < bd->cfg.erase_count);

    // erase, only needed for testing
    if (bd->cfg.erase_value != -1) {
        memset(&bd->buffer[block*bd->cfg.erase_size],
                bd->cfg.erase_value, bd->cfg.erase_size);
    }

    LFS_RAMBD_TRACE("lfs_rambd_erase -> %d", 0);
    return 0;
}

int lfs_rambd_sync(lfs_rambd_t *bd) {
    LFS_RAMBD_TRACE("lfs_rambd_sync(%p)", (void*)bd);
    // sync does nothing because we aren't backed by anything real
    (void)bd;
    LFS_RAMBD_TRACE("lfs_rambd_sync -> %d", 0);
    return 0;
}
