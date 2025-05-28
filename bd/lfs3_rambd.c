/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs3_rambd.h"

int lfs3_rambd_createcfg(const struct lfs3_config *cfg,
        const struct lfs3_rambd_config *bdcfg) {
    LFS3_RAMBD_TRACE("lfs3_rambd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "%p {.buffer=%p})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            (void*)bdcfg, bdcfg->buffer);
    lfs3_rambd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // allocate buffer?
    if (bd->cfg->buffer) {
        bd->buffer = bd->cfg->buffer;
    } else {
        bd->buffer = lfs3_malloc(cfg->block_size * cfg->block_count);
        if (!bd->buffer) {
            LFS3_RAMBD_TRACE("lfs3_rambd_createcfg -> %d", LFS3_ERR_NOMEM);
            return LFS3_ERR_NOMEM;
        }
    }

    // zero for reproducibility
    memset(bd->buffer, 0, cfg->block_size * cfg->block_count);

    LFS3_RAMBD_TRACE("lfs3_rambd_createcfg -> %d", 0);
    return 0;
}

int lfs3_rambd_create(const struct lfs3_config *cfg) {
    LFS3_RAMBD_TRACE("lfs3_rambd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count);
    static const struct lfs3_rambd_config defaults = {0};
    int err = lfs3_rambd_createcfg(cfg, &defaults);
    LFS3_RAMBD_TRACE("lfs3_rambd_create -> %d", err);
    return err;
}

int lfs3_rambd_destroy(const struct lfs3_config *cfg) {
    LFS3_RAMBD_TRACE("lfs3_rambd_destroy(%p)", (void*)cfg);
    // clean up memory
    lfs3_rambd_t *bd = cfg->context;
    if (!bd->cfg->buffer) {
        lfs3_free(bd->buffer);
    }
    LFS3_RAMBD_TRACE("lfs3_rambd_destroy -> %d", 0);
    return 0;
}

int lfs3_rambd_read(const struct lfs3_config *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size) {
    LFS3_RAMBD_TRACE("lfs3_rambd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs3_rambd_t *bd = cfg->context;

    // check if read is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->read_size == 0);
    LFS3_ASSERT(size % cfg->read_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // read data
    memcpy(buffer, &bd->buffer[block*cfg->block_size + off], size);

    LFS3_RAMBD_TRACE("lfs3_rambd_read -> %d", 0);
    return 0;
}

int lfs3_rambd_prog(const struct lfs3_config *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size) {
    LFS3_RAMBD_TRACE("lfs3_rambd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs3_rambd_t *bd = cfg->context;

    // check if write is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->prog_size == 0);
    LFS3_ASSERT(size % cfg->prog_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // program data
    memcpy(&bd->buffer[block*cfg->block_size + off], buffer, size);

    LFS3_RAMBD_TRACE("lfs3_rambd_prog -> %d", 0);
    return 0;
}

int lfs3_rambd_erase(const struct lfs3_config *cfg, lfs3_block_t block) {
    LFS3_RAMBD_TRACE("lfs3_rambd_erase(%p, 0x%"PRIx32" (%"PRIu32"))",
            (void*)cfg, block, cfg->block_size);

    // check if erase is valid
    LFS3_ASSERT(block < cfg->block_count);

    // erase is a noop
    (void)block;

    LFS3_RAMBD_TRACE("lfs3_rambd_erase -> %d", 0);
    return 0;
}

int lfs3_rambd_sync(const struct lfs3_config *cfg) {
    LFS3_RAMBD_TRACE("lfs3_rambd_sync(%p)", (void*)cfg);

    // sync is a noop
    (void)cfg;

    LFS3_RAMBD_TRACE("lfs3_rambd_sync -> %d", 0);
    return 0;
}
