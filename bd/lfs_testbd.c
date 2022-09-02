/*
 * Testing block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "bd/lfs_testbd.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif


// access to lazily-allocated/copy-on-write blocks
//
// Note we can only modify a block if we have exclusive access to it (rc == 1)
//

static lfs_testbd_block_t *lfs_testbd_incblock(lfs_testbd_block_t *block) {
    if (block) {
        block->rc += 1;
    }
    return block;
}

static void lfs_testbd_decblock(lfs_testbd_block_t *block) {
    if (block) {
        block->rc -= 1;
        if (block->rc == 0) {
            free(block);
        }
    }
}

static lfs_testbd_block_t *lfs_testbd_mutblock(
        const struct lfs_config *cfg,
        lfs_testbd_block_t **block) {
    lfs_testbd_block_t *block_ = *block;
    if (block_ && block_->rc == 1) {
        // rc == 1? can modify
        return block_;

    } else if (block_) {
        // rc > 1? need to create a copy
        lfs_testbd_block_t *nblock = malloc(
                sizeof(lfs_testbd_block_t) + cfg->block_size);
        if (!nblock) {
            return NULL;
        }

        memcpy(nblock, block_,
                sizeof(lfs_testbd_block_t) + cfg->block_size);
        nblock->rc = 1;

        lfs_testbd_decblock(block_);
        *block = nblock;
        return nblock;

    } else {
        // no block? need to allocate
        lfs_testbd_block_t *nblock = malloc(
                sizeof(lfs_testbd_block_t) + cfg->block_size);
        if (!nblock) {
            return NULL;
        }

        nblock->rc = 1;
        nblock->wear = 0;

        // zero for consistency
        lfs_testbd_t *bd = cfg->context;
        memset(nblock->data,
                (bd->cfg->erase_value != -1) ? bd->cfg->erase_value : 0,
                cfg->block_size);

        *block = nblock;
        return nblock;
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
    bd->disk = NULL;

    if (bd->cfg->disk_path) {
        bd->disk = malloc(sizeof(lfs_testbd_disk_t));
        if (!bd->disk) {
            LFS_TESTBD_TRACE("lfs_testbd_createcfg -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }
        bd->disk->rc = 1;
        bd->disk->scratch = NULL;

        #ifdef _WIN32
        bd->disk->fd = open(bd->cfg->disk_path,
                O_RDWR | O_CREAT | O_BINARY, 0666);
        #else
        bd->disk->fd = open(bd->cfg->disk_path,
                O_RDWR | O_CREAT, 0666);
        #endif
        if (bd->disk->fd < 0) {
            int err = -errno;
            LFS_TESTBD_TRACE("lfs_testbd_create -> %d", err);
            return err;
        }

        // if we're emulating erase values, we can keep a block around in
        // memory of just the erase state to speed up emulated erases
        if (bd->cfg->erase_value != -1) {
            bd->disk->scratch = malloc(cfg->block_size);
            if (!bd->disk->scratch) {
                LFS_TESTBD_TRACE("lfs_testbd_createcfg -> %d", LFS_ERR_NOMEM);
                return LFS_ERR_NOMEM;
            }
            memset(bd->disk->scratch,
                    bd->cfg->erase_value,
                    cfg->block_size);
        }
    }

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
        lfs_testbd_decblock(bd->blocks[i]);
    }
    free(bd->blocks);

    // clean up other resources 
    if (bd->disk) {
        bd->disk->rc -= 1;
        if (bd->disk->rc == 0) {
            close(bd->disk->fd);
            free(bd->disk->scratch);
            free(bd->disk);
        }
    }

    LFS_TESTBD_TRACE("lfs_testbd_destroy -> %d", 0);
    return 0;
}



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
    const lfs_testbd_block_t *b = bd->blocks[block];
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

    if (bd->cfg->read_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->read_sleep/1000000000,
                .tv_nsec=bd->cfg->read_sleep%1000000000},
            NULL);
        if (err) {
            err = -errno;
            LFS_TESTBD_TRACE("lfs_testbd_read -> %d", err);
            return err;
        }
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
    lfs_testbd_block_t *b = lfs_testbd_mutblock(cfg, &bd->blocks[block]);
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

    // mirror to disk file?
    if (bd->disk) {
        off_t res1 = lseek(bd->disk->fd,
                (off_t)block*cfg->block_size + (off_t)off,
                SEEK_SET);
        if (res1 < 0) {
            int err = -errno;
            LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", err);
            return err;
        }

        ssize_t res2 = write(bd->disk->fd, buffer, size);
        if (res2 < 0) {
            int err = -errno;
            LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", err);
            return err;
        }
    }

    if (bd->cfg->prog_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->prog_sleep/1000000000,
                .tv_nsec=bd->cfg->prog_sleep%1000000000},
            NULL);
        if (err) {
            err = -errno;
            LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", err);
            return err;
        }
    }

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // simulate power loss
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);
        }
    }

    LFS_TESTBD_TRACE("lfs_testbd_prog -> %d", 0);
    return 0;
}

int lfs_testbd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    LFS_TESTBD_TRACE("lfs_testbd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs_testbd_t *bd = cfg->context;

    // check if erase is valid
    LFS_ASSERT(block < cfg->block_count);

    // get the block
    lfs_testbd_block_t *b = lfs_testbd_mutblock(cfg, &bd->blocks[block]);
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

        // mirror to disk file?
        if (bd->disk) {
            off_t res1 = lseek(bd->disk->fd,
                    (off_t)block*cfg->block_size,
                    SEEK_SET);
            if (res1 < 0) {
                int err = -errno;
                LFS_TESTBD_TRACE("lfs_testbd_erase -> %d", err);
                return err;
            }

            ssize_t res2 = write(bd->disk->fd,
                    bd->disk->scratch,
                    cfg->block_size);
            if (res2 < 0) {
                int err = -errno;
                LFS_TESTBD_TRACE("lfs_testbd_erase -> %d", err);
                return err;
            }
        }
    }

    if (bd->cfg->erase_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->erase_sleep/1000000000,
                .tv_nsec=bd->cfg->erase_sleep%1000000000},
            NULL);
        if (err) {
            err = -errno;
            LFS_TESTBD_TRACE("lfs_testbd_erase -> %d", err);
            return err;
        }
    }

    // lose power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // simulate power loss
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);
        }
    }

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
    const lfs_testbd_block_t *b = bd->blocks[block];
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
    lfs_testbd_block_t *b = lfs_testbd_mutblock(cfg, &bd->blocks[block]);
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

int lfs_testbd_copy(const struct lfs_config *cfg, lfs_testbd_t *copy) {
    LFS_TESTBD_TRACE("lfs_testbd_copy(%p, %p)", (void*)cfg, (void*)copy);
    lfs_testbd_t *bd = cfg->context;

    // lazily copy over our block array
    copy->blocks = malloc(cfg->block_count * sizeof(lfs_testbd_block_t*));
    if (!copy->blocks) {
        LFS_TESTBD_TRACE("lfs_testbd_copy -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }

    for (size_t i = 0; i < cfg->block_count; i++) {
        copy->blocks[i] = lfs_testbd_incblock(bd->blocks[i]);
    }

    // other state
    copy->power_cycles = bd->power_cycles;
    copy->disk = bd->disk;
    if (copy->disk) {
        copy->disk->rc += 1;
    }
    copy->cfg = bd->cfg;

    LFS_TESTBD_TRACE("lfs_testbd_copy -> %d", 0);
    return 0;
}

