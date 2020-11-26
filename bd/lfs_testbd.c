/*
 * Testing block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "bd/lfs_testbd.h"

#include <stdlib.h>


int lfs_testbd_createcfg(lfs_testbd_t *bd, const char *path,
        const struct lfs_testbd_cfg *cfg) {
    LFS_TESTBD_TRACE("lfs_testbd_createcfg(%p, \"%s\", %p {"
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".erase_size=%"PRIu32", .erase_count=%"PRIu32", "
                ".erase_value=%"PRId32", .erase_cycles=%"PRIu32", "
                ".badblock_behavior=%"PRIu8", .power_cycles=%"PRIu32", "
                ".buffer=%p, .wear_buffer=%p})",
            (void*)bd, path, (void*)cfg,
            cfg->read_size, cfg->prog_size, cfg->erase_size, cfg->erase_count,
            cfg->erase_value, cfg->erase_cycles,
            cfg->badblock_behavior, cfg->power_cycles,
            cfg->buffer, cfg->wear_buffer);
    bd->cfg = cfg;

    // setup testing things
    bd->persist = path;
    bd->power_cycles = bd->cfg->power_cycles;

    if (bd->cfg->erase_cycles) {
        if (bd->cfg->wear_buffer) {
            bd->wear = bd->cfg->wear_buffer;
        } else {
            bd->wear = lfs_malloc(sizeof(lfs_testbd_wear_t)*cfg->erase_count);
            if (!bd->wear) {
                LFS_TESTBD_TRACE("lfs_testbd_createcfg -> %d", LFS_ERR_NOMEM);
                return LFS_ERR_NOMEM;
            }
        }

        memset(bd->wear, 0, sizeof(lfs_testbd_wear_t) * bd->cfg->erase_count);
    }

    // create underlying block device
    if (bd->persist) {
        int err = lfs_filebd_createcfg(&bd->impl.filebd, path,
                bd->cfg->filebd_cfg);
        LFS_TESTBD_TRACE("lfs_testbd_createcfg -> %d", err);
        return err;
    } else {
        int err = lfs_rambd_createcfg(&bd->impl.rambd,
                bd->cfg->rambd_cfg);
        LFS_TESTBD_TRACE("lfs_testbd_createcfg -> %d", err);
        return err;
    }
}

int lfs_testbd_destroy(lfs_testbd_t *bd) {
    LFS_TESTBD_TRACE("lfs_testbd_destroy(%p)", (void*)bd);
    if (bd->cfg->erase_cycles && !bd->cfg->wear_buffer) {
        lfs_free(bd->wear);
    }

    if (bd->persist) {
        int err = lfs_filebd_destroy(&bd->impl.filebd);
        LFS_TESTBD_TRACE("lfs_testbd_destroy -> %d", err);
        return err;
    } else {
        int err = lfs_rambd_destroy(&bd->impl.rambd);
        LFS_TESTBD_TRACE("lfs_testbd_destroy -> %d", err);
        return err;
    }
}

/// Internal mapping to block devices ///
static int lfs_testbd_rawread(lfs_testbd_t *bd, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    if (bd->persist) {
        return lfs_filebd_read(&bd->impl.filebd, block, off, buffer, size);
    } else {
        return lfs_rambd_read(&bd->impl.rambd, block, off, buffer, size);
    }
}

static int lfs_testbd_rawprog(lfs_testbd_t *bd, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    if (bd->persist) {
        return lfs_filebd_prog(&bd->impl.filebd, block, off, buffer, size);
    } else {
        return lfs_rambd_prog(&bd->impl.rambd, block, off, buffer, size);
    }
}

static int lfs_testbd_rawerase(lfs_testbd_t *bd,
        lfs_block_t block) {
    if (bd->persist) {
        return lfs_filebd_erase(&bd->impl.filebd, block);
    } else {
        return lfs_rambd_erase(&bd->impl.rambd, block);
    }
}

static int lfs_testbd_rawsync(lfs_testbd_t *bd) {
    if (bd->persist) {
        return lfs_filebd_sync(&bd->impl.filebd);
    } else {
        return lfs_rambd_sync(&bd->impl.rambd);
    }
}

/// block device API ///
int lfs_testbd_read(lfs_testbd_t *bd, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_TESTBD_TRACE("lfs_testbd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)bd, block, off, buffer, size);

    // check if read is valid
    LFS_ASSERT(off  % bd->cfg->read_size == 0);
    LFS_ASSERT(size % bd->cfg->read_size == 0);
    LFS_ASSERT(block < bd->cfg->erase_count);

    // block bad?
    if (bd->cfg->erase_cycles && bd->wear[block] >= bd->cfg->erase_cycles &&
            bd->cfg->badblock_behavior == LFS_TESTBD_BADBLOCK_READERROR) {
        LFS_TESTBD_TRACE("lfs_testbd_read -> %d", LFS_ERR_CORRUPT);
        return LFS_ERR_CORRUPT;
    }

    // read
    int err = lfs_testbd_rawread(bd, block, off, buffer, size);
    LFS_TESTBD_TRACE("lfs_testbd_read -> %d", err);
    return err;
}

int lfs_testbd_prog(lfs_testbd_t *bd, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_TESTBD_TRACE("lfs_testbd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)bd, block, off, buffer, size);

    // check if write is valid
    LFS_ASSERT(off  % bd->cfg->prog_size == 0);
    LFS_ASSERT(size % bd->cfg->prog_size == 0);
    LFS_ASSERT(block < bd->cfg->erase_count);

    // block bad?
    if (bd->cfg->erase_cycles && bd->wear[block] >= bd->cfg->erase_cycles) {
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

    // prog
    int err = lfs_testbd_rawprog(bd, block, off, buffer, size);
    if (err) {
        LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", err);
        return err;
    }

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // sync to make sure we persist the last changes
            assert(lfs_testbd_rawsync(bd) == 0);
            // simulate power loss
            exit(33);
        }
    }

    LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", 0);
    return 0;
}

int lfs_testbd_erase(lfs_testbd_t *bd, lfs_block_t block) {
    LFS_TESTBD_TRACE("lfs_testbd_erase(%p, 0x%"PRIx32")", (void*)bd, block);

    // check if erase is valid
    LFS_ASSERT(block < bd->cfg->erase_count);

    // block bad?
    if (bd->cfg->erase_cycles) {
        if (bd->wear[block] >= bd->cfg->erase_cycles) {
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
            bd->wear[block] += 1;
        }
    }

    // erase
    int err = lfs_testbd_rawerase(bd, block);
    if (err) {
        LFS_TESTBD_TRACE("lfs_testbd_erase -> %d", err);
        return err;
    }

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // sync to make sure we persist the last changes
            assert(lfs_testbd_rawsync(bd) == 0);
            // simulate power loss
            exit(33);
        }
    }

    LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", 0);
    return 0;
}

int lfs_testbd_sync(lfs_testbd_t *bd) {
    LFS_TESTBD_TRACE("lfs_testbd_sync(%p)", (void*)bd);
    int err = lfs_testbd_rawsync(bd);
    LFS_TESTBD_TRACE("lfs_testbd_sync -> %d", err);
    return err;
}


/// simulated wear operations ///
lfs_testbd_swear_t lfs_testbd_getwear(lfs_testbd_t *bd,
        lfs_block_t block) {
    LFS_TESTBD_TRACE("lfs_testbd_getwear(%p, %"PRIu32")", (void*)bd, block);

    // check if block is valid
    LFS_ASSERT(bd->cfg->erase_cycles);
    LFS_ASSERT(block < bd->cfg->erase_count);

    LFS_TESTBD_TRACE("lfs_testbd_getwear -> %"PRIu32, bd->wear[block]);
    return bd->wear[block];
}

int lfs_testbd_setwear(lfs_testbd_t *bd,
        lfs_block_t block, lfs_testbd_wear_t wear) {
    LFS_TESTBD_TRACE("lfs_testbd_setwear(%p, %"PRIu32")", (void*)bd, block);

    // check if block is valid
    LFS_ASSERT(bd->cfg->erase_cycles);
    LFS_ASSERT(block < bd->cfg->erase_count);

    bd->wear[block] = wear;

    LFS_TESTBD_TRACE("lfs_testbd_setwear -> %d", 0);
    return 0;
}
