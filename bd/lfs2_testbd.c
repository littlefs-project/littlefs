/*
 * Testing block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs2_testbd.h"

#include <stdlib.h>


int lfs2_testbd_createcfg(const struct lfs2_config *cfg, const char *path,
        const struct lfs2_testbd_config *bdcfg) {
    LFS2_TESTBD_TRACE("lfs2_testbd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "\"%s\", "
                "%p {.erase_value=%"PRId32", .erase_cycles=%"PRIu32", "
                ".badblock_behavior=%"PRIu8", .power_cycles=%"PRIu32", "
                ".buffer=%p, .wear_buffer=%p})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            path, (void*)bdcfg, bdcfg->erase_value, bdcfg->erase_cycles,
            bdcfg->badblock_behavior, bdcfg->power_cycles,
            bdcfg->buffer, bdcfg->wear_buffer);
    lfs2_testbd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // setup testing things
    bd->persist = path;
    bd->power_cycles = bd->cfg->power_cycles;

    if (bd->cfg->erase_cycles) {
        if (bd->cfg->wear_buffer) {
            bd->wear = bd->cfg->wear_buffer;
        } else {
            bd->wear = lfs2_malloc(sizeof(lfs2_testbd_wear_t)*cfg->block_count);
            if (!bd->wear) {
                LFS2_TESTBD_TRACE("lfs2_testbd_createcfg -> %d", LFS2_ERR_NOMEM);
                return LFS2_ERR_NOMEM;
            }
        }

        memset(bd->wear, 0, sizeof(lfs2_testbd_wear_t) * cfg->block_count);
    }

    // create underlying block device
    if (bd->persist) {
        bd->u.file.cfg = (struct lfs2_filebd_config){
            .erase_value = bd->cfg->erase_value,
        };
        int err = lfs2_filebd_createcfg(cfg, path, &bd->u.file.cfg);
        LFS2_TESTBD_TRACE("lfs2_testbd_createcfg -> %d", err);
        return err;
    } else {
        bd->u.ram.cfg = (struct lfs2_rambd_config){
            .erase_value = bd->cfg->erase_value,
            .buffer = bd->cfg->buffer,
        };
        int err = lfs2_rambd_createcfg(cfg, &bd->u.ram.cfg);
        LFS2_TESTBD_TRACE("lfs2_testbd_createcfg -> %d", err);
        return err;
    }
}

int lfs2_testbd_create(const struct lfs2_config *cfg, const char *path) {
    LFS2_TESTBD_TRACE("lfs2_testbd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "\"%s\")",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            path);
    static const struct lfs2_testbd_config defaults = {.erase_value=-1};
    int err = lfs2_testbd_createcfg(cfg, path, &defaults);
    LFS2_TESTBD_TRACE("lfs2_testbd_create -> %d", err);
    return err;
}

int lfs2_testbd_destroy(const struct lfs2_config *cfg) {
    LFS2_TESTBD_TRACE("lfs2_testbd_destroy(%p)", (void*)cfg);
    lfs2_testbd_t *bd = cfg->context;
    if (bd->cfg->erase_cycles && !bd->cfg->wear_buffer) {
        lfs2_free(bd->wear);
    }

    if (bd->persist) {
        int err = lfs2_filebd_destroy(cfg);
        LFS2_TESTBD_TRACE("lfs2_testbd_destroy -> %d", err);
        return err;
    } else {
        int err = lfs2_rambd_destroy(cfg);
        LFS2_TESTBD_TRACE("lfs2_testbd_destroy -> %d", err);
        return err;
    }
}

/// Internal mapping to block devices ///
static int lfs2_testbd_rawread(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, void *buffer, lfs2_size_t size) {
    lfs2_testbd_t *bd = cfg->context;
    if (bd->persist) {
        return lfs2_filebd_read(cfg, block, off, buffer, size);
    } else {
        return lfs2_rambd_read(cfg, block, off, buffer, size);
    }
}

static int lfs2_testbd_rawprog(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, const void *buffer, lfs2_size_t size) {
    lfs2_testbd_t *bd = cfg->context;
    if (bd->persist) {
        return lfs2_filebd_prog(cfg, block, off, buffer, size);
    } else {
        return lfs2_rambd_prog(cfg, block, off, buffer, size);
    }
}

static int lfs2_testbd_rawerase(const struct lfs2_config *cfg,
        lfs2_block_t block) {
    lfs2_testbd_t *bd = cfg->context;
    if (bd->persist) {
        return lfs2_filebd_erase(cfg, block);
    } else {
        return lfs2_rambd_erase(cfg, block);
    }
}

static int lfs2_testbd_rawsync(const struct lfs2_config *cfg) {
    lfs2_testbd_t *bd = cfg->context;
    if (bd->persist) {
        return lfs2_filebd_sync(cfg);
    } else {
        return lfs2_rambd_sync(cfg);
    }
}

/// block device API ///
int lfs2_testbd_read(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, void *buffer, lfs2_size_t size) {
    LFS2_TESTBD_TRACE("lfs2_testbd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs2_testbd_t *bd = cfg->context;

    // check if read is valid
    LFS2_ASSERT(off  % cfg->read_size == 0);
    LFS2_ASSERT(size % cfg->read_size == 0);
    LFS2_ASSERT(block < cfg->block_count);

    // block bad?
    if (bd->cfg->erase_cycles && bd->wear[block] >= bd->cfg->erase_cycles &&
            bd->cfg->badblock_behavior == LFS2_TESTBD_BADBLOCK_READERROR) {
        LFS2_TESTBD_TRACE("lfs2_testbd_read -> %d", LFS2_ERR_CORRUPT);
        return LFS2_ERR_CORRUPT;
    }

    // read
    int err = lfs2_testbd_rawread(cfg, block, off, buffer, size);
    LFS2_TESTBD_TRACE("lfs2_testbd_read -> %d", err);
    return err;
}

int lfs2_testbd_prog(const struct lfs2_config *cfg, lfs2_block_t block,
        lfs2_off_t off, const void *buffer, lfs2_size_t size) {
    LFS2_TESTBD_TRACE("lfs2_testbd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs2_testbd_t *bd = cfg->context;

    // check if write is valid
    LFS2_ASSERT(off  % cfg->prog_size == 0);
    LFS2_ASSERT(size % cfg->prog_size == 0);
    LFS2_ASSERT(block < cfg->block_count);

    // block bad?
    if (bd->cfg->erase_cycles && bd->wear[block] >= bd->cfg->erase_cycles) {
        if (bd->cfg->badblock_behavior ==
                LFS2_TESTBD_BADBLOCK_PROGERROR) {
            LFS2_TESTBD_TRACE("lfs2_testbd_prog -> %d", LFS2_ERR_CORRUPT);
            return LFS2_ERR_CORRUPT;
        } else if (bd->cfg->badblock_behavior ==
                LFS2_TESTBD_BADBLOCK_PROGNOOP ||
                bd->cfg->badblock_behavior ==
                LFS2_TESTBD_BADBLOCK_ERASENOOP) {
            LFS2_TESTBD_TRACE("lfs2_testbd_prog -> %d", 0);
            return 0;
        }
    }

    // prog
    int err = lfs2_testbd_rawprog(cfg, block, off, buffer, size);
    if (err) {
        LFS2_TESTBD_TRACE("lfs2_testbd_prog -> %d", err);
        return err;
    }

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // sync to make sure we persist the last changes
            assert(lfs2_testbd_rawsync(cfg) == 0);
            // simulate power loss
            exit(33);
        }
    }

    LFS2_TESTBD_TRACE("lfs2_testbd_prog -> %d", 0);
    return 0;
}

int lfs2_testbd_erase(const struct lfs2_config *cfg, lfs2_block_t block) {
    LFS2_TESTBD_TRACE("lfs2_testbd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs2_testbd_t *bd = cfg->context;

    // check if erase is valid
    LFS2_ASSERT(block < cfg->block_count);

    // block bad?
    if (bd->cfg->erase_cycles) {
        if (bd->wear[block] >= bd->cfg->erase_cycles) {
            if (bd->cfg->badblock_behavior ==
                    LFS2_TESTBD_BADBLOCK_ERASEERROR) {
                LFS2_TESTBD_TRACE("lfs2_testbd_erase -> %d", LFS2_ERR_CORRUPT);
                return LFS2_ERR_CORRUPT;
            } else if (bd->cfg->badblock_behavior ==
                    LFS2_TESTBD_BADBLOCK_ERASENOOP) {
                LFS2_TESTBD_TRACE("lfs2_testbd_erase -> %d", 0);
                return 0;
            }
        } else {
            // mark wear
            bd->wear[block] += 1;
        }
    }

    // erase
    int err = lfs2_testbd_rawerase(cfg, block);
    if (err) {
        LFS2_TESTBD_TRACE("lfs2_testbd_erase -> %d", err);
        return err;
    }

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // sync to make sure we persist the last changes
            assert(lfs2_testbd_rawsync(cfg) == 0);
            // simulate power loss
            exit(33);
        }
    }

    LFS2_TESTBD_TRACE("lfs2_testbd_prog -> %d", 0);
    return 0;
}

int lfs2_testbd_sync(const struct lfs2_config *cfg) {
    LFS2_TESTBD_TRACE("lfs2_testbd_sync(%p)", (void*)cfg);
    int err = lfs2_testbd_rawsync(cfg);
    LFS2_TESTBD_TRACE("lfs2_testbd_sync -> %d", err);
    return err;
}


/// simulated wear operations ///
lfs2_testbd_swear_t lfs2_testbd_getwear(const struct lfs2_config *cfg,
        lfs2_block_t block) {
    LFS2_TESTBD_TRACE("lfs2_testbd_getwear(%p, %"PRIu32")", (void*)cfg, block);
    lfs2_testbd_t *bd = cfg->context;

    // check if block is valid
    LFS2_ASSERT(bd->cfg->erase_cycles);
    LFS2_ASSERT(block < cfg->block_count);

    LFS2_TESTBD_TRACE("lfs2_testbd_getwear -> %"PRIu32, bd->wear[block]);
    return bd->wear[block];
}

int lfs2_testbd_setwear(const struct lfs2_config *cfg,
        lfs2_block_t block, lfs2_testbd_wear_t wear) {
    LFS2_TESTBD_TRACE("lfs2_testbd_setwear(%p, %"PRIu32")", (void*)cfg, block);
    lfs2_testbd_t *bd = cfg->context;

    // check if block is valid
    LFS2_ASSERT(bd->cfg->erase_cycles);
    LFS2_ASSERT(block < cfg->block_count);

    bd->wear[block] = wear;

    LFS2_TESTBD_TRACE("lfs2_testbd_setwear -> %d", 0);
    return 0;
}
