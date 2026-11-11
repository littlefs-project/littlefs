/*
 * Block device emulated in RAM
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs3_rambd.h"

int lfs3_rambd_createcfg(const struct lfs3_cfg *cfg,
        const struct lfs3_rambd_cfg *bdcfg) {
    lfs3_rambd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // allocate buffer?
    if (bd->cfg->buffer) {
        bd->mem = bd->cfg->buffer;
    } else {
        bd->mem = lfs3_malloc(cfg->block_size * cfg->block_count);
        if (!bd->mem) {
            return LFS3_ERR_NOMEM;
        }
    }

    // zero for reproducibility
    memset(bd->mem, 0, cfg->block_size * cfg->block_count);

    return 0;
}

int lfs3_rambd_create(const struct lfs3_cfg *cfg) {
    static const struct lfs3_rambd_cfg defaults = {0};
    return lfs3_rambd_createcfg(cfg, &defaults);
}

int lfs3_rambd_destroy(const struct lfs3_cfg *cfg) {
    lfs3_rambd_t *bd = cfg->context;

    // clean up memory
    if (!bd->cfg->buffer) {
        lfs3_free(bd->mem);
    }

    return 0;
}

int lfs3_rambd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size) {
    lfs3_rambd_t *bd = cfg->context;

    // check if read is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->read_size == 0);
    LFS3_ASSERT(size % cfg->read_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // read data
    memcpy(buffer, &bd->mem[block*cfg->block_size + off], size);

    return 0;
}

int lfs3_rambd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size) {
    lfs3_rambd_t *bd = cfg->context;

    // check if write is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->prog_size == 0);
    LFS3_ASSERT(size % cfg->prog_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // program data
    memcpy(&bd->mem[block*cfg->block_size + off], buffer, size);

    return 0;
}

int lfs3_rambd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block) {
    // check if erase is valid
    LFS3_ASSERT(block < cfg->block_count);

    // erase is a noop
    (void)cfg;
    (void)block;

    return 0;
}

int lfs3_rambd_sync(const struct lfs3_cfg *cfg) {
    // sync is a noop
    (void)cfg;

    return 0;
}
