/*
 * Testing block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs_testbd.h"

#include <stdlib.h>


// access to lazily-allocated/copy-on-write blocks
//
// Note we can only modify a block if we have exclusive access to it (rc == 1)
//

// TODO
__attribute__((unused))
static void lfs_testbd_incblock(lfs_testbd_t *bd, lfs_block_t block) {
    if (bd->blocks[block]) {
        bd->blocks[block]->rc += 1;
    }
}

static void lfs_testbd_decblock(lfs_testbd_t *bd, lfs_block_t block) {
    if (bd->blocks[block]) {
        bd->blocks[block]->rc -= 1;
        if (bd->blocks[block]->rc == 0) {
            free(bd->blocks[block]);
            bd->blocks[block] = NULL;
        }
    }
}

static const lfs_testbd_block_t *lfs_testbd_getblock(lfs_testbd_t *bd,
        lfs_block_t block) {
    return bd->blocks[block];
}

static lfs_testbd_block_t *lfs_testbd_mutblock(lfs_testbd_t *bd,
        lfs_block_t block, lfs_size_t block_size) {
    if (bd->blocks[block] && bd->blocks[block]->rc == 1) {
        // rc == 1? can modify
        return bd->blocks[block];

    } else if (bd->blocks[block]) {
        // rc > 1? need to create a copy
        lfs_testbd_block_t *b = malloc(
                sizeof(lfs_testbd_block_t) + block_size);
        if (!b) {
            return NULL;
        }

        memcpy(b, bd->blocks[block], sizeof(lfs_testbd_block_t) + block_size);
        b->rc = 1;

        lfs_testbd_decblock(bd, block);
        bd->blocks[block] = b;
        return b;

    } else {
        // no block? need to allocate
        lfs_testbd_block_t *b = malloc(
                sizeof(lfs_testbd_block_t) + block_size);
        if (!b) {
            return NULL;
        }

        b->rc = 1;
        b->wear = 0;

        // zero for consistency
        memset(b->data,
                (bd->cfg->erase_value != -1) ? bd->cfg->erase_value : 0,
                block_size);

        bd->blocks[block] = b;
        return b;
    }
}


// testbd create/destroy

int lfs_testbd_createcfg(const struct lfs_config *cfg, const char *path,
        const struct lfs_testbd_config *bdcfg) {
    LFS_TESTBD_TRACE("lfs_testbd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "\"%s\", "
                "%p {.erase_value=%"PRId32", .erase_cycles=%"PRIu32", "
                ".badblock_behavior=%"PRIu8", .power_cycles=%"PRIu32", "
                ".powerloss_behavior=%"PRIu8", .powerloss_cb=%p, "
                ".powerloss_data=%p, .track_branches=%d})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            path, (void*)bdcfg, bdcfg->erase_value, bdcfg->erase_cycles,
            bdcfg->badblock_behavior, bdcfg->power_cycles,
            bdcfg->powerloss_behavior, (void*)(uintptr_t)bdcfg->powerloss_cb,
            bdcfg->powerloss_data, bdcfg->track_branches);
    lfs_testbd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // allocate our block array, all blocks start as uninitialized
    bd->blocks = malloc(cfg->block_count * sizeof(lfs_testbd_block_t*));
    if (!bd->blocks) {
        LFS_TESTBD_TRACE("lfs_testbd_createcfg -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    memset(bd->blocks, 0, cfg->block_count * sizeof(lfs_testbd_block_t*));

    // setup testing things
    bd->power_cycles = bd->cfg->power_cycles;
    bd->branches = NULL;
    bd->branch_capacity = 0;
    bd->branch_count = 0;

    LFS_TESTBD_TRACE("lfs_testbd_createcfg -> %d", 0);
    return 0;
}

int lfs_testbd_create(const struct lfs_config *cfg, const char *path) {
    LFS_TESTBD_TRACE("lfs_testbd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "\"%s\")",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            path);
    static const struct lfs_testbd_config defaults = {.erase_value=-1};
    int err = lfs_testbd_createcfg(cfg, path, &defaults);
    LFS_TESTBD_TRACE("lfs_testbd_create -> %d", err);
    return err;
}

int lfs_testbd_destroy(const struct lfs_config *cfg) {
    LFS_TESTBD_TRACE("lfs_testbd_destroy(%p)", (void*)cfg);
    lfs_testbd_t *bd = cfg->context;

    // decrement reference counts
    for (lfs_block_t i = 0; i < cfg->block_count; i++) {
        lfs_testbd_decblock(bd, i);
    }

    // free memory
    free(bd->blocks);
    free(bd->branches);

    LFS_TESTBD_TRACE("lfs_testbd_destroy -> %d", 0);
    return 0;
}



///// Internal mapping to block devices ///
//static int lfs_testbd_rawread(const struct lfs_config *cfg, lfs_block_t block,
//        lfs_off_t off, void *buffer, lfs_size_t size) {
//    lfs_testbd_t *bd = cfg->context;
//    if (bd->persist) {
//        return lfs_filebd_read(cfg, block, off, buffer, size);
//    } else {
//        return lfs_rambd_read(cfg, block, off, buffer, size);
//    }
//}
//
//static int lfs_testbd_rawprog(const struct lfs_config *cfg, lfs_block_t block,
//        lfs_off_t off, const void *buffer, lfs_size_t size) {
//    lfs_testbd_t *bd = cfg->context;
//    if (bd->persist) {
//        return lfs_filebd_prog(cfg, block, off, buffer, size);
//    } else {
//        return lfs_rambd_prog(cfg, block, off, buffer, size);
//    }
//}
//
//static int lfs_testbd_rawerase(const struct lfs_config *cfg,
//        lfs_block_t block) {
//    lfs_testbd_t *bd = cfg->context;
//    if (bd->persist) {
//        return lfs_filebd_erase(cfg, block);
//    } else {
//        return lfs_rambd_erase(cfg, block);
//    }
//}
//
//static int lfs_testbd_rawsync(const struct lfs_config *cfg) {
//    lfs_testbd_t *bd = cfg->context;
//    if (bd->persist) {
//        return lfs_filebd_sync(cfg);
//    } else {
//        return lfs_rambd_sync(cfg);
//    }
//}



// block device API

int lfs_testbd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_TESTBD_TRACE("lfs_testbd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_testbd_t *bd = cfg->context;

    // check if read is valid
    LFS_ASSERT(block < cfg->block_count);
    LFS_ASSERT(off  % cfg->read_size == 0);
    LFS_ASSERT(size % cfg->read_size == 0);
    LFS_ASSERT(off+size <= cfg->block_size);

    // get the block
    const lfs_testbd_block_t *b = lfs_testbd_getblock(bd, block);
    if (b) {
        // block bad?
        if (bd->cfg->erase_cycles && b->wear >= bd->cfg->erase_cycles &&
                bd->cfg->badblock_behavior == LFS_TESTBD_BADBLOCK_READERROR) {
            LFS_TESTBD_TRACE("lfs_testbd_read -> %d", LFS_ERR_CORRUPT);
            return LFS_ERR_CORRUPT;
        }

        // read data
        memcpy(buffer, &b->data[off], size);
    } else {
        // zero for consistency
        memset(buffer,
                (bd->cfg->erase_value != -1) ? bd->cfg->erase_value : 0,
                size);
    }

    LFS_TESTBD_TRACE("lfs_testbd_read -> %d", 0);
    return 0;
}

int lfs_testbd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_TESTBD_TRACE("lfs_testbd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_testbd_t *bd = cfg->context;

    // check if write is valid
    LFS_ASSERT(block < cfg->block_count);
    LFS_ASSERT(off  % cfg->prog_size == 0);
    LFS_ASSERT(size % cfg->prog_size == 0);
    LFS_ASSERT(off+size <= cfg->block_size);

    // get the block
    lfs_testbd_block_t *b = lfs_testbd_mutblock(bd, block, cfg->block_size);
    if (!b) {
        LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }

    // block bad?
    if (bd->cfg->erase_cycles && b->wear >= bd->cfg->erase_cycles) {
        if (bd->cfg->badblock_behavior ==
                LFS_TESTBD_BADBLOCK_PROGERROR) {
            LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", LFS_ERR_CORRUPT);
            return LFS_ERR_CORRUPT;
        } else if (bd->cfg->badblock_behavior ==
                LFS_TESTBD_BADBLOCK_PROGNOOP ||
                bd->cfg->badblock_behavior ==
                LFS_TESTBD_BADBLOCK_ERASENOOP) {
            LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", 0);
            return 0;
        }
    }

    // were we erased properly?
    if (bd->cfg->erase_value != -1) {
        for (lfs_off_t i = 0; i < size; i++) {
            LFS_ASSERT(b->data[off+i] == bd->cfg->erase_value);
        }
    }

    // prog data
    memcpy(&b->data[off], buffer, size);

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // simulate power loss
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);
        }
    }

//    // track power-loss branch?
//    if (bd->cfg->track_branches) {
//        int err = lfs_testbd_trackbranch(bd);
//        if (err) {
//            LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", err);
//            return err;
//        }
//    }

    LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", 0);
    return 0;
}

int lfs_testbd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    LFS_TESTBD_TRACE("lfs_testbd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs_testbd_t *bd = cfg->context;

    // check if erase is valid
    LFS_ASSERT(block < cfg->block_count);

    // get the block
    lfs_testbd_block_t *b = lfs_testbd_mutblock(bd, block, cfg->block_size);
    if (!b) {
        LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }

    // block bad?
    if (bd->cfg->erase_cycles) {
        if (b->wear >= bd->cfg->erase_cycles) {
            if (bd->cfg->badblock_behavior ==
                    LFS_TESTBD_BADBLOCK_ERASEERROR) {
                LFS_TESTBD_TRACE("lfs_testbd_erase -> %d", LFS_ERR_CORRUPT);
                return LFS_ERR_CORRUPT;
            } else if (bd->cfg->badblock_behavior ==
                    LFS_TESTBD_BADBLOCK_ERASENOOP) {
                LFS_TESTBD_TRACE("lfs_testbd_erase -> %d", 0);
                return 0;
            }
        } else {
            // mark wear
            b->wear += 1;
        }
    }

    // emulate an erase value?
    if (bd->cfg->erase_value != -1) {
        memset(b->data, bd->cfg->erase_value, cfg->block_size);
    }

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // simulate power loss
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);
        }
    }

//    // track power-loss branch?
//    if (bd->cfg->track_branches) {
//        int err = lfs_testbd_trackbranch(bd);
//        if (err) {
//            LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", err);
//            return err;
//        }
//    }

    LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", 0);
    return 0;
}

int lfs_testbd_sync(const struct lfs_config *cfg) {
    LFS_TESTBD_TRACE("lfs_testbd_sync(%p)", (void*)cfg);

    // do nothing
    (void)cfg;

    LFS_TESTBD_TRACE("lfs_testbd_sync -> %d", 0);
    return 0;
}


// simulated wear operations

lfs_testbd_swear_t lfs_testbd_getwear(const struct lfs_config *cfg,
        lfs_block_t block) {
    LFS_TESTBD_TRACE("lfs_testbd_getwear(%p, %"PRIu32")", (void*)cfg, block);
    lfs_testbd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // get the wear
    lfs_testbd_wear_t wear;
    const lfs_testbd_block_t *b = lfs_testbd_getblock(bd, block);
    if (b) {
        wear = b->wear;
    } else {
        wear = 0;
    }

    LFS_TESTBD_TRACE("lfs_testbd_getwear -> %"PRIu32, wear);
    return wear;
}

int lfs_testbd_setwear(const struct lfs_config *cfg,
        lfs_block_t block, lfs_testbd_wear_t wear) {
    LFS_TESTBD_TRACE("lfs_testbd_setwear(%p, %"PRIu32")", (void*)cfg, block);
    lfs_testbd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // set the wear
    lfs_testbd_block_t *b = lfs_testbd_mutblock(bd, block, cfg->block_size);
    if (!b) {
        LFS_TESTBD_TRACE("lfs_testbd_setwear -> %"PRIu32, LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    b->wear = wear;

    LFS_TESTBD_TRACE("lfs_testbd_setwear -> %"PRIu32, 0);
    return 0;
}

lfs_testbd_spowercycles_t lfs_testbd_getpowercycles(
        const struct lfs_config *cfg) {
    LFS_TESTBD_TRACE("lfs_testbd_getpowercycles(%p)", (void*)cfg);
    lfs_testbd_t *bd = cfg->context;

    LFS_TESTBD_TRACE("lfs_testbd_getpowercycles -> %"PRIi32, bd->power_cycles);
    return bd->power_cycles;
}

int lfs_testbd_setpowercycles(const struct lfs_config *cfg,
        lfs_testbd_powercycles_t power_cycles) {
    LFS_TESTBD_TRACE("lfs_testbd_setpowercycles(%p, %"PRIi32")",
            (void*)cfg, power_cycles);
    lfs_testbd_t *bd = cfg->context;

    bd->power_cycles = power_cycles;

    LFS_TESTBD_TRACE("lfs_testbd_getpowercycles -> %d", 0);
    return 0;
}

//int lfs_testbd_getbranch(const struct lfs_config *cfg,
//        lfs_testbd_powercycles_t branch, lfs_testbd_t *bd) {
//    LFS_TESTBD_TRACE("lfs_testbd_getbranch(%p, %zu, %p)",
//            (void*)cfg, branch, bd);
//    lfs_testbd_t *bd = cfg->context;
//
//    // TODO
//
//    LFS_TESTBD_TRACE("lfs_testbd_getbranch -> %d", 0);
//    return 0;
//}

lfs_testbd_spowercycles_t lfs_testbd_getbranchcount(
        const struct lfs_config *cfg) {
    LFS_TESTBD_TRACE("lfs_testbd_getbranchcount(%p)", (void*)cfg);
    lfs_testbd_t *bd = cfg->context;

    LFS_TESTBD_TRACE("lfs_testbd_getbranchcount -> %"PRIu32, bd->branch_count);
    return bd->branch_count;
}
