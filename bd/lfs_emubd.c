/*
 * Emulating block device, wraps filebd and rambd while providing a bunch
 * of hooks for testing littlefs in various conditions.
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "bd/lfs_emubd.h"

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

static lfs_emubd_block_t *lfs_emubd_incblock(lfs_emubd_block_t *block) {
    if (block) {
        block->rc += 1;
    }
    return block;
}

static void lfs_emubd_decblock(lfs_emubd_block_t *block) {
    if (block) {
        block->rc -= 1;
        if (block->rc == 0) {
            free(block);
        }
    }
}

static lfs_emubd_block_t *lfs_emubd_mutblock(
        const struct lfs_config *cfg,
        lfs_emubd_block_t *block) {
    if (block && block->rc == 1) {
        // rc == 1? can modify
        return block;

    } else if (block) {
        // rc > 1? need to create a copy
        lfs_emubd_block_t *block_ = malloc(
                sizeof(lfs_emubd_block_t) + cfg->block_size);
        if (!block_) {
            return NULL;
        }

        memcpy(block_, block,
                sizeof(lfs_emubd_block_t) + cfg->block_size);
        block_->rc = 1;

        lfs_emubd_decblock(block);
        return block_;

    } else {
        // no block? need to allocate
        lfs_emubd_block_t *block_ = malloc(
                sizeof(lfs_emubd_block_t) + cfg->block_size);
        if (!block_) {
            return NULL;
        }

        block_->rc = 1;
        block_->wear = 0;
        block_->metastable = false;
        block_->bad_bit = 0;

        // zero for consistency
        lfs_emubd_t *bd = cfg->context;
        memset(block_->data,
                (bd->cfg->erase_value != -1) ? bd->cfg->erase_value : 0,
                cfg->block_size);

        return block_;
    }
}


// prng used for some emulation things
static uint32_t lfs_emubd_prng(uint32_t *state) {
    // A simple xorshift32 generator, easily reproducible. Keep in mind
    // determinism is much more important than actual randomness here.
    uint32_t x = *state;
    // must be non-zero, use uintmax here so that seed=0 is different
    // from seed=1 and seed=range(0,n) makes a bit more sense
    if (x == 0) {
        x = -1;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}


// emubd create/destroy

int lfs_emubd_createcfg(const struct lfs_config *cfg, const char *path,
        const struct lfs_emubd_config *bdcfg) {
    LFS_EMUBD_TRACE("lfs_emubd_createcfg(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "\"%s\", "
                "%p {.erase_value=%"PRId32", .erase_cycles=%"PRIu32", "
                ".badblock_behavior=%"PRIu8", .power_cycles=%"PRIu32", "
                ".powerloss_behavior=%"PRIu8", .powerloss_cb=%p, "
                ".powerloss_data=%p, seed=%"PRIu32"})",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            path, (void*)bdcfg, bdcfg->erase_value, bdcfg->erase_cycles,
            bdcfg->badblock_behavior, bdcfg->power_cycles,
            bdcfg->powerloss_behavior, (void*)(uintptr_t)bdcfg->powerloss_cb,
            bdcfg->powerloss_data, bdcfg->seed);
    lfs_emubd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // setup testing things
    bd->blocks = NULL;
    bd->readed = 0;
    bd->proged = 0;
    bd->erased = 0;
    bd->prng = bd->cfg->seed;
    bd->power_cycles = bd->cfg->power_cycles;
    bd->ooo_before = NULL;
    bd->ooo_after = NULL;
    bd->disk = NULL;

    // allocate our block array, all blocks start as uninitialized
    bd->blocks = malloc(
            cfg->block_count * sizeof(lfs_emubd_block_t*));
    int err;
    if (!bd->blocks) {
        err = LFS_ERR_NOMEM;
        goto failed;
    }
    memset(bd->blocks, 0,
            cfg->block_count * sizeof(lfs_emubd_block_t*));

    // allocate extra block arrays to hold our ooo snapshots
    if (bd->cfg->powerloss_behavior == LFS_EMUBD_POWERLOSS_OOO) {
        bd->ooo_before = malloc(
                cfg->block_count * sizeof(lfs_emubd_block_t*));
        if (!bd->ooo_before) {
            err = LFS_ERR_NOMEM;
            goto failed;
        }
        memset(bd->ooo_before, 0,
                cfg->block_count * sizeof(lfs_emubd_block_t*));

        bd->ooo_after = malloc(
                cfg->block_count * sizeof(lfs_emubd_block_t*));
        if (!bd->ooo_after) {
            err = LFS_ERR_NOMEM;
            goto failed;
        }
        memset(bd->ooo_after, 0,
                cfg->block_count * sizeof(lfs_emubd_block_t*));
    }

    if (bd->cfg->disk_path) {
        bd->disk = malloc(sizeof(lfs_emubd_disk_t));
        if (!bd->disk) {
            err = LFS_ERR_NOMEM;
            goto failed;
        }
        bd->disk->rc = 1;
        bd->disk->fd = -1;
        bd->disk->scratch = NULL;

        #ifdef _WIN32
        bd->disk->fd = open(bd->cfg->disk_path,
                O_RDWR | O_CREAT | O_BINARY, 0666);
        #else
        bd->disk->fd = open(bd->cfg->disk_path,
                O_RDWR | O_CREAT, 0666);
        #endif
        if (bd->disk->fd < 0) {
            err = -errno;
            goto failed;
        }

        bd->disk->scratch = malloc(cfg->block_size);
        if (!bd->disk->scratch) {
            err = LFS_ERR_NOMEM;
            goto failed;
        }
        memset(bd->disk->scratch,
                (bd->cfg->erase_value != -1) ? bd->cfg->erase_value : 0,
                cfg->block_size);

        // go ahead and erase all of the disk, otherwise the file will not
        // match our internal representation
        for (size_t i = 0; i < cfg->block_count; i++) {
            ssize_t res = write(bd->disk->fd,
                    bd->disk->scratch,
                    cfg->block_size);
            if (res < 0) {
                err = -errno;
                goto failed;
            }
        }
    }

    LFS_EMUBD_TRACE("lfs_emubd_createcfg -> %d", 0);
    return 0;

failed:;
    LFS_EMUBD_TRACE("lfs_emubd_createcfg -> %d", err);
    // clean up memory
    free(bd->blocks);
    if (bd->cfg->powerloss_behavior == LFS_EMUBD_POWERLOSS_OOO) {
        free(bd->ooo_before);
        free(bd->ooo_after);
    }
    if (bd->disk) {
        if (bd->disk->fd != -1) {
            close(bd->disk->fd);
        }
        free(bd->disk->scratch);
        free(bd->disk);
    }
    return err;
}

int lfs_emubd_create(const struct lfs_config *cfg, const char *path) {
    LFS_EMUBD_TRACE("lfs_emubd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, "
                "\"%s\")",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            path);
    static const struct lfs_emubd_config defaults = {.erase_value=-1};
    int err = lfs_emubd_createcfg(cfg, path, &defaults);
    LFS_EMUBD_TRACE("lfs_emubd_create -> %d", err);
    return err;
}

int lfs_emubd_destroy(const struct lfs_config *cfg) {
    LFS_EMUBD_TRACE("lfs_emubd_destroy(%p)", (void*)cfg);
    lfs_emubd_t *bd = cfg->context;

    // decrement reference counts
    for (lfs_block_t i = 0; i < cfg->block_count; i++) {
        lfs_emubd_decblock(bd->blocks[i]);
    }
    free(bd->blocks);

    if (bd->cfg->powerloss_behavior == LFS_EMUBD_POWERLOSS_OOO) {
        for (lfs_block_t i = 0; i < cfg->block_count; i++) {
            lfs_emubd_decblock(bd->ooo_before[i]);
        }
        free(bd->ooo_before);

        for (lfs_block_t i = 0; i < cfg->block_count; i++) {
            lfs_emubd_decblock(bd->ooo_after[i]);
        }
        free(bd->ooo_after);
    }

    // clean up other resources 
    if (bd->disk) {
        bd->disk->rc -= 1;
        if (bd->disk->rc == 0) {
            close(bd->disk->fd);
            free(bd->disk->scratch);
            free(bd->disk);
        }
    }

    LFS_EMUBD_TRACE("lfs_emubd_destroy -> %d", 0);
    return 0;
}


// block device API

int lfs_emubd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_EMUBD_TRACE("lfs_emubd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_emubd_t *bd = cfg->context;

    // check if read is valid
    LFS_ASSERT(block < cfg->block_count);
    LFS_ASSERT(off  % cfg->read_size == 0);
    LFS_ASSERT(size % cfg->read_size == 0);
    LFS_ASSERT(off+size <= cfg->block_size);

    // get the block
    const lfs_emubd_block_t *b = bd->blocks[block];
    if (b) {
        // block bad?
        if (b->wear > bd->cfg->erase_cycles) {
            // erroring reads? error
            if (bd->cfg->badblock_behavior
                    == LFS_EMUBD_BADBLOCK_READERROR) {
                LFS_EMUBD_TRACE("lfs_emubd_read -> %d", LFS_ERR_CORRUPT);
                return LFS_ERR_CORRUPT;
            }
        }

        // read data
        memcpy(buffer, &b->data[off], size);

        // metastable? randomly decide if our bad bit flips
        if (b->metastable) {
            lfs_size_t bit = b->bad_bit & 0x7fffffff;
            if (bit/8 >= off
                    && bit/8 < off+size
                    && (lfs_emubd_prng(&bd->prng) & 1)) {
                ((uint8_t*)buffer)[(bit/8) - off] ^= 1 << (bit%8);
            }
        }

    // no block yet
    } else {
        // zero for consistency
        memset(buffer,
                (bd->cfg->erase_value != -1) ? bd->cfg->erase_value : 0,
                size);
    }   

    // track reads
    bd->readed += size;
    if (bd->cfg->read_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->read_sleep/1000000000,
                .tv_nsec=bd->cfg->read_sleep%1000000000},
            NULL);
        if (err) {
            err = -errno;
            LFS_EMUBD_TRACE("lfs_emubd_read -> %d", err);
            return err;
        }
    }

    LFS_EMUBD_TRACE("lfs_emubd_read -> %d", 0);
    return 0;
}

int lfs_emubd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_EMUBD_TRACE("lfs_emubd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_emubd_t *bd = cfg->context;

    // check if write is valid
    LFS_ASSERT(block < cfg->block_count);
    LFS_ASSERT(off  % cfg->prog_size == 0);
    LFS_ASSERT(size % cfg->prog_size == 0);
    LFS_ASSERT(off+size <= cfg->block_size);

    // were we erased properly?
    LFS_ASSERT(bd->blocks[block]);
    if (bd->cfg->erase_value != -1
            && bd->blocks[block]->wear <= bd->cfg->erase_cycles) {
        for (lfs_off_t i = 0; i < size; i++) {
            LFS_ASSERT(bd->blocks[block]->data[off+i] == bd->cfg->erase_value);
        }
    }

    // losing power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // emulating some bits? choose a random bit to flip
            if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_SOMEBITS) {
                // mutate the block
                lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", LFS_ERR_NOMEM);
                    return LFS_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // flip bit
                lfs_size_t bit = lfs_emubd_prng(&bd->prng)
                        % (cfg->prog_size*8);
                b->data[off + (bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size + (off_t)off,
                            SEEK_SET);
                    if (res1 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                        return err;
                    }

                    ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
                    if (res2 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                        return err;
                    }
                }

            // emulating most bits? prog data and choose a random bit
            // to flip
            } else if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_MOSTBITS) {
                // mutate the block
                lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", LFS_ERR_NOMEM);
                    return LFS_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // prog data
                memcpy(&b->data[off], buffer, size);

                // flip bit
                lfs_size_t bit = lfs_emubd_prng(&bd->prng)
                        % (cfg->prog_size*8);
                b->data[off + (bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size + (off_t)off,
                            SEEK_SET);
                    if (res1 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                        return err;
                    }

                    ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
                    if (res2 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                        return err;
                    }
                }

            // emulating out-of-order writes? revert everything unsynced
            // except for our current block
            } else if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_OOO) {
                for (lfs_block_t i = 0; i < cfg->block_count; i++) {
                    lfs_emubd_decblock(bd->ooo_after[i]);
                    bd->ooo_after[i] = lfs_emubd_incblock(bd->blocks[i]);

                    if (i != block && bd->blocks[i] != bd->ooo_before[i]) {
                        lfs_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs_emubd_incblock(bd->ooo_before[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                                return err;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                                return err;
                            }
                        }
                    }
                }

            // emulating metastability? prog data, choose a random bad bit,
            // and mark as metastable
            } else if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_METASTABLE) {
                // mutate the block
                lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", LFS_ERR_NOMEM);
                    return LFS_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // prog data
                memcpy(&b->data[off], buffer, size);

                // choose a new bad bit unless overridden
                if (!(0x80000000 & b->bad_bit)) {
                    b->bad_bit = lfs_emubd_prng(&bd->prng)
                            % (cfg->block_size*8);
                }

                // mark as metastable
                b->metastable = true;

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size + (off_t)off,
                            SEEK_SET);
                    if (res1 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                        return err;
                    }

                    ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
                    if (res2 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                        return err;
                    }
                }
            }

            // powerloss!
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);

            // oh, continuing? undo out-of-order write emulation
            if (bd->cfg->powerloss_behavior == LFS_EMUBD_POWERLOSS_OOO) {
                for (lfs_block_t i = 0; i < cfg->block_count; i++) {
                    if (bd->blocks[i] != bd->ooo_after[i]) {
                        lfs_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs_emubd_incblock(bd->ooo_after[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                                return err;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
                                return err;
                            }
                        }
                    }
                }
            }
        }
    }

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // block bad?
    if (b->wear > bd->cfg->erase_cycles) {
        // erroring progs? error
        if (bd->cfg->badblock_behavior
                == LFS_EMUBD_BADBLOCK_PROGERROR) {
            LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", LFS_ERR_CORRUPT);
            return LFS_ERR_CORRUPT;

        // noop progs? skip
        } else if (bd->cfg->badblock_behavior
                    == LFS_EMUBD_BADBLOCK_PROGNOOP
                || bd->cfg->badblock_behavior
                    == LFS_EMUBD_BADBLOCK_ERASENOOP) {
            goto progged;

        // progs flipping bits? flip our bad bit, exactly which bit
        // is chosen during erase
        } else if (bd->cfg->badblock_behavior
                    == LFS_EMUBD_BADBLOCK_PROGFLIP) {
            lfs_size_t bit = b->bad_bit & 0x7fffffff;
            if (bit/8 >= off && bit/8 < off+size) {
                memcpy(&b->data[off], buffer, size);
                b->data[bit/8] ^= 1 << (bit%8);
                goto progged;
            }

        // reads flipping bits? prog as normal but mark as metastable
        } else if (bd->cfg->badblock_behavior
                    == LFS_EMUBD_BADBLOCK_READFLIP) {
            memcpy(&b->data[off], buffer, size);
            b->metastable = true;
            goto progged;
        }
    }

    // prog data
    memcpy(&b->data[off], buffer, size);

    // clear any metastability
    b->metastable = false;

progged:;
    // mirror to disk file?
    if (bd->disk) {
        off_t res1 = lseek(bd->disk->fd,
                (off_t)block*cfg->block_size + (off_t)off,
                SEEK_SET);
        if (res1 < 0) {
            int err = -errno;
            LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
            return err;
        }

        ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
        if (res2 < 0) {
            int err = -errno;
            LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
            return err;
        }
    }

    // track progs
    bd->proged += size;
    if (bd->cfg->prog_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->prog_sleep/1000000000,
                .tv_nsec=bd->cfg->prog_sleep%1000000000},
            NULL);
        if (err) {
            err = -errno;
            LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", err);
            return err;
        }
    }

    LFS_EMUBD_TRACE("lfs_emubd_prog -> %d", 0);
    return 0;
}

int lfs_emubd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    LFS_EMUBD_TRACE("lfs_emubd_erase(%p, 0x%"PRIx32" (%"PRIu32"))",
            (void*)cfg, block, cfg->block_size);
    lfs_emubd_t *bd = cfg->context;

    // check if erase is valid
    LFS_ASSERT(block < cfg->block_count);

    // losing power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // emulating some bits? choose a random bit to flip
            if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_SOMEBITS) {
                // mutate the block
                lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", LFS_ERR_NOMEM);
                    return LFS_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // flip bit
                lfs_size_t bit = lfs_emubd_prng(&bd->prng)
                        % (cfg->block_size*8);
                b->data[(bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size,
                            SEEK_SET);
                    if (res1 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                        return err;
                    }

                    ssize_t res2 = write(bd->disk->fd,
                            b->data, cfg->block_size);
                    if (res2 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                        return err;
                    }
                }

            // emulating most bits? erase data and choose a random bit
            // to flip
            } else if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_MOSTBITS) {
                // mutate the block
                lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", LFS_ERR_NOMEM);
                    return LFS_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // emulate an erase value?
                if (bd->cfg->erase_value != -1) {
                    memset(b->data, bd->cfg->erase_value, cfg->block_size);
                }

                // flip bit
                lfs_size_t bit = lfs_emubd_prng(&bd->prng)
                        % (cfg->block_size*8);
                b->data[(bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size,
                            SEEK_SET);
                    if (res1 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                        return err;
                    }

                    ssize_t res2 = write(bd->disk->fd,
                            b->data, cfg->block_size);
                    if (res2 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                        return err;
                    }
                }

            // emulating out-of-order writes? revert everything unsynced
            // except for our current block
            } else if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_OOO) {
                for (lfs_block_t i = 0; i < cfg->block_count; i++) {
                    if (i != block && bd->blocks[i] != bd->ooo_before[i]) {
                        lfs_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs_emubd_incblock(bd->ooo_before[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                                return err;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                                return err;
                            }
                        }
                    }
                }

            // emulating metastability? erase data, choose a random bad bit,
            // and mark as metastable
            } else if (bd->cfg->powerloss_behavior
                    == LFS_EMUBD_POWERLOSS_METASTABLE) {
                // mutate the block
                lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", LFS_ERR_NOMEM);
                    return LFS_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // emulate an erase value?
                if (bd->cfg->erase_value != -1) {
                    memset(b->data, bd->cfg->erase_value, cfg->block_size);
                }

                // choose a new bad bit unless overridden
                if (!(0x80000000 & b->bad_bit)) {
                    b->bad_bit = lfs_emubd_prng(&bd->prng)
                            % (cfg->block_size*8);
                }

                // mark as metastable
                b->metastable = true;

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size,
                            SEEK_SET);
                    if (res1 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                        return err;
                    }

                    ssize_t res2 = write(bd->disk->fd,
                            b->data, cfg->block_size);
                    if (res2 < 0) {
                        int err = -errno;
                        LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                        return err;
                    }
                }
            }

            // powerloss!
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);

            // oh, continuing? undo out-of-order write emulation
            if (bd->cfg->powerloss_behavior == LFS_EMUBD_POWERLOSS_OOO) {
                for (lfs_block_t i = 0; i < cfg->block_count; i++) {
                    if (bd->blocks[i] != bd->ooo_after[i]) {
                        lfs_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs_emubd_incblock(bd->ooo_after[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                                return err;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                int err = -errno;
                                LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                                return err;
                            }
                        }
                    }
                }
            }
        }
    }

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // keep track of wear
    if (bd->cfg->erase_cycles && b->wear <= bd->cfg->erase_cycles) {
        b->wear += 1;
    }

    // block bad?
    if (b->wear > bd->cfg->erase_cycles) {
        // erroring erases? error
        if (bd->cfg->badblock_behavior
                == LFS_EMUBD_BADBLOCK_ERASEERROR) {
            LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", LFS_ERR_CORRUPT);
            return LFS_ERR_CORRUPT;

        // noop erases? skip
        } else if (bd->cfg->badblock_behavior
                == LFS_EMUBD_BADBLOCK_ERASENOOP) {
            goto erased;

        // flipping bits? if we're not manually overridden, choose a
        // new bad bit on erase, this makes it more likely to
        // eventually cause problems
        } else if (bd->cfg->badblock_behavior
                    == LFS_EMUBD_BADBLOCK_PROGFLIP
                || bd->cfg->badblock_behavior
                    == LFS_EMUBD_BADBLOCK_READFLIP) {
            if (!(0x80000000 & b->bad_bit)) {
                b->bad_bit = lfs_emubd_prng(&bd->prng)
                        % (cfg->block_size*8);
            }
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
                LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                return err;
            }

            ssize_t res2 = write(bd->disk->fd, b->data, cfg->block_size);
            if (res2 < 0) {
                int err = -errno;
                LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
                return err;
            }
        }
    }

    // clear any metastability
    b->metastable = false;

erased:;
    // track erases
    bd->erased += cfg->block_size;
    if (bd->cfg->erase_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->erase_sleep/1000000000,
                .tv_nsec=bd->cfg->erase_sleep%1000000000},
            NULL);
        if (err) {
            err = -errno;
            LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", err);
            return err;
        }
    }

    LFS_EMUBD_TRACE("lfs_emubd_erase -> %d", 0);
    return 0;
}

int lfs_emubd_sync(const struct lfs_config *cfg) {
    LFS_EMUBD_TRACE("lfs_emubd_sync(%p)", (void*)cfg);
    lfs_emubd_t *bd = cfg->context;

    // emulate out-of-order writes? save a snapshot on sync
    if (bd->cfg->powerloss_behavior == LFS_EMUBD_POWERLOSS_OOO) {
        for (size_t i = 0; i < cfg->block_count; i++) {
            lfs_emubd_decblock(bd->ooo_before[i]);
            bd->ooo_before[i] = lfs_emubd_incblock(bd->blocks[i]);
        }
    }

    LFS_EMUBD_TRACE("lfs_emubd_sync -> %d", 0);
    return 0;
}


/// Additional extended API for driving test features ///

int lfs_emubd_seed(const struct lfs_config *cfg, uint32_t seed) {
    LFS_EMUBD_TRACE("lfs_emubd_seed(%p, 0x%08"PRIx32")",
            (void*)cfg, seed);
    lfs_emubd_t *bd = cfg->context;

    bd->prng = seed;

    LFS_EMUBD_TRACE("lfs_emubd_seed -> %d", 0);
    return 0;
}

lfs_emubd_sio_t lfs_emubd_readed(const struct lfs_config *cfg) {
    LFS_EMUBD_TRACE("lfs_emubd_readed(%p)", (void*)cfg);
    lfs_emubd_t *bd = cfg->context;
    LFS_EMUBD_TRACE("lfs_emubd_readed -> %"PRIu64, bd->readed);
    return bd->readed;
}

lfs_emubd_sio_t lfs_emubd_proged(const struct lfs_config *cfg) {
    LFS_EMUBD_TRACE("lfs_emubd_proged(%p)", (void*)cfg);
    lfs_emubd_t *bd = cfg->context;
    LFS_EMUBD_TRACE("lfs_emubd_proged -> %"PRIu64, bd->proged);
    return bd->proged;
}

lfs_emubd_sio_t lfs_emubd_erased(const struct lfs_config *cfg) {
    LFS_EMUBD_TRACE("lfs_emubd_erased(%p)", (void*)cfg);
    lfs_emubd_t *bd = cfg->context;
    LFS_EMUBD_TRACE("lfs_emubd_erased -> %"PRIu64, bd->erased);
    return bd->erased;
}

int lfs_emubd_setreaded(const struct lfs_config *cfg, lfs_emubd_io_t readed) {
    LFS_EMUBD_TRACE("lfs_emubd_setreaded(%p, %"PRIu64")", (void*)cfg, readed);
    lfs_emubd_t *bd = cfg->context;
    bd->readed = readed;
    LFS_EMUBD_TRACE("lfs_emubd_setreaded -> %d", 0);
    return 0;
}

int lfs_emubd_setproged(const struct lfs_config *cfg, lfs_emubd_io_t proged) {
    LFS_EMUBD_TRACE("lfs_emubd_setproged(%p, %"PRIu64")", (void*)cfg, proged);
    lfs_emubd_t *bd = cfg->context;
    bd->proged = proged;
    LFS_EMUBD_TRACE("lfs_emubd_setproged -> %d", 0);
    return 0;
}

int lfs_emubd_seterased(const struct lfs_config *cfg, lfs_emubd_io_t erased) {
    LFS_EMUBD_TRACE("lfs_emubd_seterased(%p, %"PRIu64")", (void*)cfg, erased);
    lfs_emubd_t *bd = cfg->context;
    bd->erased = erased;
    LFS_EMUBD_TRACE("lfs_emubd_seterased -> %d", 0);
    return 0;
}

lfs_emubd_swear_t lfs_emubd_wear(const struct lfs_config *cfg,
        lfs_block_t block) {
    LFS_EMUBD_TRACE("lfs_emubd_wear(%p, %"PRIu32")", (void*)cfg, block);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // get the wear
    lfs_emubd_wear_t wear;
    const lfs_emubd_block_t *b = bd->blocks[block];
    if (b) {
        wear = b->wear;
    } else {
        wear = 0;
    }

    LFS_EMUBD_TRACE("lfs_emubd_wear -> %"PRIi32, wear);
    return wear;
}

int lfs_emubd_setwear(const struct lfs_config *cfg,
        lfs_block_t block, lfs_emubd_wear_t wear) {
    LFS_EMUBD_TRACE("lfs_emubd_setwear(%p, %"PRIu32", %"PRIi32")",
            (void*)cfg, block, wear);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_setwear -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = wear;

    LFS_EMUBD_TRACE("lfs_emubd_setwear -> %d", 0);
    return 0;
}

int lfs_emubd_markbad(const struct lfs_config *cfg,
        lfs_block_t block) {
    LFS_EMUBD_TRACE("lfs_emubd_markbad(%p, %"PRIu32")",
            (void*)cfg, block);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_markbad -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = -1;

    LFS_EMUBD_TRACE("lfs_emubd_markbad -> %d", 0);
    return 0;
}

int lfs_emubd_markgood(const struct lfs_config *cfg,
        lfs_block_t block) {
    LFS_EMUBD_TRACE("lfs_emubd_markgood(%p, %"PRIu32")",
            (void*)cfg, block);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_markgood -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = 0;

    LFS_EMUBD_TRACE("lfs_emubd_markgood -> %d", 0);
    return 0;
}

lfs_ssize_t lfs_emubd_badbit(const struct lfs_config *cfg,
        lfs_block_t block) {
    LFS_EMUBD_TRACE("lfs_emubd_badbit(%p, %"PRIu32")", (void*)cfg, block);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // get the bad bit
    lfs_size_t bad_bit;
    const lfs_emubd_block_t *b = bd->blocks[block];
    if (b) {
        bad_bit = 0x7fffffff & b->bad_bit;
    } else {
        bad_bit = 0;
    }

    LFS_EMUBD_TRACE("lfs_emubd_badbit -> %"PRIi32, bad_bit);
    return bad_bit;
}

int lfs_emubd_setbadbit(const struct lfs_config *cfg,
        lfs_block_t block, lfs_size_t bit) {
    LFS_EMUBD_TRACE("lfs_emubd_setbadbit(%p, %"PRIu32", %"PRIu32")",
            (void*)cfg, block, bit);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_setbadbit -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the bad bit and mark as fixed
    b->bad_bit = 0x80000000 | bit;

    LFS_EMUBD_TRACE("lfs_emubd_setbadbit -> %d", 0);
    return 0;
}

int lfs_emubd_randomizebadbit(const struct lfs_config *cfg,
        lfs_block_t block) {
    LFS_EMUBD_TRACE("lfs_emubd_randomizebadbit(%p, %"PRIu32")",
            (void*)cfg, block);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_randomizebadbit -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // mark the bad bit as randomized
    b->bad_bit &= ~0x80000000;

    LFS_EMUBD_TRACE("lfs_emubd_randomizebadbit -> %d", 0);
    return 0;
}

int lfs_emubd_markbadbit(const struct lfs_config *cfg,
        lfs_block_t block, lfs_size_t bit) {
    LFS_EMUBD_TRACE("lfs_emubd_markbadbit(%p, %"PRIu32", %"PRIu32")",
            (void*)cfg, block, bit);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_markbadbit -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = -1;
    // set the bad bit and mark as fixed
    b->bad_bit = 0x80000000 | bit;

    LFS_EMUBD_TRACE("lfs_emubd_markbadbit -> %d", 0);
    return 0;
}

int lfs_emubd_flipbit(const struct lfs_config *cfg,
        lfs_block_t block, lfs_size_t bit) {
    LFS_EMUBD_TRACE("lfs_emubd_flipbit(%p, %"PRIu32", %"PRIu32")",
            (void*)cfg, block, bit);
    lfs_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs_emubd_block_t *b = lfs_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        LFS_EMUBD_TRACE("lfs_emubd_flipbit -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // flip the bit
    b->data[bit/8] ^= 1 << (bit%8);

    LFS_EMUBD_TRACE("lfs_emubd_flipbit -> %d", 0);
    return 0;
}

lfs_emubd_spowercycles_t lfs_emubd_powercycles(
        const struct lfs_config *cfg) {
    LFS_EMUBD_TRACE("lfs_emubd_powercycles(%p)", (void*)cfg);
    lfs_emubd_t *bd = cfg->context;

    LFS_EMUBD_TRACE("lfs_emubd_powercycles -> %"PRIi32, bd->power_cycles);
    return bd->power_cycles;
}

int lfs_emubd_setpowercycles(const struct lfs_config *cfg,
        lfs_emubd_powercycles_t power_cycles) {
    LFS_EMUBD_TRACE("lfs_emubd_setpowercycles(%p, %"PRIi32")",
            (void*)cfg, power_cycles);
    lfs_emubd_t *bd = cfg->context;

    bd->power_cycles = power_cycles;

    LFS_EMUBD_TRACE("lfs_emubd_powercycles -> %d", 0);
    return 0;
}

int lfs_emubd_copy(const struct lfs_config *cfg, lfs_emubd_t *copy) {
    LFS_EMUBD_TRACE("lfs_emubd_copy(%p, %p)", (void*)cfg, (void*)copy);
    lfs_emubd_t *bd = cfg->context;

    // lazily copy over our block array
    copy->blocks = malloc(
            cfg->block_count * sizeof(lfs_emubd_block_t*));
    if (!copy->blocks) {
        LFS_EMUBD_TRACE("lfs_emubd_copy -> %d", LFS_ERR_NOMEM);
        return LFS_ERR_NOMEM;
    }
    for (lfs_block_t i = 0; i < cfg->block_count; i++) {
        copy->blocks[i] = lfs_emubd_incblock(bd->blocks[i]);
    }

    if (bd->cfg->powerloss_behavior == LFS_EMUBD_POWERLOSS_OOO) {
        copy->ooo_before = malloc(
                cfg->block_count * sizeof(lfs_emubd_block_t*));
        if (!copy->ooo_before) {
            LFS_EMUBD_TRACE("lfs_emubd_copy -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }
        for (lfs_block_t i = 0; i < cfg->block_count; i++) {
            copy->ooo_before[i] = lfs_emubd_incblock(bd->ooo_before[i]);
        }

        copy->ooo_after = malloc(
                cfg->block_count * sizeof(lfs_emubd_block_t*));
        if (!copy->ooo_after) {
            LFS_EMUBD_TRACE("lfs_emubd_copy -> %d", LFS_ERR_NOMEM);
            return LFS_ERR_NOMEM;
        }
        for (lfs_block_t i = 0; i < cfg->block_count; i++) {
            copy->ooo_after[i] = lfs_emubd_incblock(bd->ooo_after[i]);
        }
    }

    // other state
    copy->readed = bd->readed;
    copy->proged = bd->proged;
    copy->erased = bd->erased;
    copy->prng = bd->prng;
    copy->power_cycles = bd->power_cycles;
    copy->disk = bd->disk;
    if (copy->disk) {
        copy->disk->rc += 1;
    }
    copy->cfg = bd->cfg;

    LFS_EMUBD_TRACE("lfs_emubd_copy -> %d", 0);
    return 0;
}

