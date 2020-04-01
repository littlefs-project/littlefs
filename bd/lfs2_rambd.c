/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs2_rambd.h"

int lfs2_rambd_createcfg(const struct lfs2_config *cfg,
        const struct lfs2_rambd_config *bdcfg) {
    LFS2_RAMBD_TRACE("lfs2_rambd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "%p {.erase_value=%"PRId32", .buffer=%p})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            (void*)bdcfg, bdcfg->erase_value, bdcfg->buffer);
    lfs2_rambd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // allocate buffer?
    if (bd->cfg->buffer) {
        bd->buffer = bd->cfg->buffer;
    } else {
        bd->buffer = lfs2_malloc(cfg->block_size * cfg->block_count);
        if (!bd->buffer) {
            LFS2_RAMBD_TRACE("lfs2_rambd_createcfg -> %d", LFS2_ERR_NOMEM);
            return LFS2_ERR_NOMEM;
        }
    }

    // zero for reproducability?
    if (bd->cfg->erase_value != -1) {
        memset(bd->buffer, bd->cfg->erase_value,
                cfg->block_size * cfg->block_count);
    }

    LFS2_RAMBD_TRACE("lfs2_rambd_createcfg -> %d", 0);
    return 0;
}

int lfs2_rambd_create(const struct lfs2_config *cfg) {
    LFS2_RAMBD_TRACE("lfs2_rambd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count);
    static const struct lfs2_rambd_config defaults = {.erase_value=-1};
    int err = lfs2_rambd_createcfg(cfg, &defaults);
    LFS2_RAMBD_TRACE("lfs2_rambd_create -> %d", err);
    return err;
}

int lfs2_rambd_destroy(const struct lfs2_config *cfg) {
    LFS2_RAMBD_TRACE("lfs2_rambd_destroy(%p)", (void*)cfg);
    // clean up memory
    lfs2_rambd_t *bd = cfg->context;
    if (!bd->cfg->buffer) {
        lfs2_free(bd->buffer);
    }
    LFS2_RAMBD_TRACE("lfs2_rambd_destroy -> %d", 0);
    return 0;
}

int lfs2_rambd_read(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, void *buffer, lfs2_size_t size) {
    LFS2_RAMBD_TRACE("lfs2_rambd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs2_rambd_t *bd = cfg->context;

    // check if read is valid
    LFS2_ASSERT(off  % cfg->read_size == 0);
    LFS2_ASSERT(size % cfg->read_size == 0);
    LFS2_ASSERT(block < cfg->block_count);

    // read data
    memcpy(buffer, &bd->buffer[block*cfg->block_size + off], size);

    LFS2_RAMBD_TRACE("lfs2_rambd_read -> %d", 0);
    return 0;
}

int lfs2_rambd_prog(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, const void *buffer, lfs2_size_t size) {
    LFS2_RAMBD_TRACE("lfs2_rambd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs2_rambd_t *bd = cfg->context;

    // check if write is valid
    LFS2_ASSERT(off  % cfg->prog_size == 0);
    LFS2_ASSERT(size % cfg->prog_size == 0);
    LFS2_ASSERT(block < cfg->block_count);

    // check that data was erased? only needed for testing
    if (bd->cfg->erase_value != -1) {
        for (lfs2_off_t i = 0; i < size; i++) {
            LFS2_ASSERT(bd->buffer[block*cfg->block_size + off + i] ==
                    bd->cfg->erase_value);
        }
    }

    // program data
    memcpy(&bd->buffer[block*cfg->block_size + off], buffer, size);

    LFS2_RAMBD_TRACE("lfs2_rambd_prog -> %d", 0);
    return 0;
}

int lfs2_rambd_erase(const struct lfs2_config *cfg, lfs2_block_t block) {
    LFS2_RAMBD_TRACE("lfs2_rambd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs2_rambd_t *bd = cfg->context;

    // check if erase is valid
    LFS2_ASSERT(block < cfg->block_count);

    // erase, only needed for testing
    if (bd->cfg->erase_value != -1) {
        memset(&bd->buffer[block*cfg->block_size],
                bd->cfg->erase_value, cfg->block_size);
    }

    LFS2_RAMBD_TRACE("lfs2_rambd_erase -> %d", 0);
    return 0;
}

int lfs2_rambd_sync(const struct lfs2_config *cfg) {
    LFS2_RAMBD_TRACE("lfs2_rambd_sync(%p)", (void*)cfg);
    // sync does nothing because we aren't backed by anything real
    (void)cfg;
    LFS2_RAMBD_TRACE("lfs2_rambd_sync -> %d", 0);
    return 0;
}
