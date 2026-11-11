/*
 * emubd - High-level emulating block device with many bells and
 * whistles for testing powerloss, wear, etc.
 *
 * Note emubd always backs the block device in RAM. Consider using
 * kiwibd if you need a block device larger than the available RAM on
 * the system.
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "bd/lfs3_emubd.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#endif


// low-level flash memory emulation

// read data
static inline void lfs3_emubd_memread(const struct lfs3_cfg *cfg,
        void *restrict dst, const void *restrict src, size_t size) {
    (void)cfg;
    memcpy(dst, src, size);
}

static inline void lfs3_emubd_memprog(const struct lfs3_cfg *cfg,
        void *restrict dst, const void *restrict src, size_t size) {
    lfs3_emubd_t *bd = cfg->context;
    // emulating nor-masking?
    if (bd->cfg->erase_value == -2) {
        uint8_t *dst_ = dst;
        const uint8_t *src_ = src;
        for (size_t i = 0; i < size; i++) {
            dst_[i] &= src_[i];
        }
    } else {
        memcpy(dst, src, size);
    }
}

static inline void lfs3_emubd_memerase(const struct lfs3_cfg *cfg,
        void *restrict dst, size_t size) {
    lfs3_emubd_t *bd = cfg->context;
    // emulating erase value?
    if (bd->cfg->erase_value != -1) {
        memset(dst,
                (bd->cfg->erase_value >= 0)
                    ? bd->cfg->erase_value
                    : 0xff,
                size);
    }
}

// this is slightly different from lfs3_emubd_memerase in that we use
// lfs3_emubd_memzero when we need to unconditionally zero memory
static inline void lfs3_emubd_memzero(const struct lfs3_cfg *cfg,
        void *restrict dst, size_t size) {
    lfs3_emubd_t *bd = cfg->context;
    memset(dst,
            (bd->cfg->erase_value == -1) ? 0
                : (bd->cfg->erase_value >= 0) ? bd->cfg->erase_value
                : (bd->cfg->erase_value == -2) ? 0xff
                : 0,
            size);
}


// access to lazily-allocated/copy-on-write blocks
//
// note we can only modify a block if we have exclusive access to
// it (rc == 1)
//

static lfs3_emubd_block_t *lfs3_emubd_incblock(lfs3_emubd_block_t *block) {
    if (block) {
        block->rc += 1;
    }
    return block;
}

static void lfs3_emubd_decblock(lfs3_emubd_block_t *block) {
    if (block) {
        block->rc -= 1;
        if (block->rc == 0) {
            free(block);
        }
    }
}

static lfs3_emubd_block_t *lfs3_emubd_mutblock(
        const struct lfs3_cfg *cfg,
        lfs3_emubd_block_t *block) {
    if (block && block->rc == 1) {
        // rc == 1? can modify
        return block;

    } else if (block) {
        // rc > 1? need to create a copy
        lfs3_emubd_block_t *block_ = malloc(
                sizeof(lfs3_emubd_block_t) + cfg->block_size);
        if (!block_) {
            return NULL;
        }

        memcpy(block_, block,
                sizeof(lfs3_emubd_block_t) + cfg->block_size);
        block_->rc = 1;

        lfs3_emubd_decblock(block);
        return block_;

    } else {
        // no block? need to allocate
        lfs3_emubd_block_t *block_ = malloc(
                sizeof(lfs3_emubd_block_t) + cfg->block_size);
        if (!block_) {
            return NULL;
        }

        block_->rc = 1;
        block_->wear = 0;
        block_->metastable = false;
        block_->bad_bit = 0;

        // zero for consistency
        lfs3_emubd_memzero(cfg, block_->data, cfg->block_size);

        return block_;
    }
}


// prng used for some emulation things
static uint32_t lfs3_emubd_prng_(uint32_t *state) {
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

int lfs3_emubd_createcfg(const struct lfs3_cfg *cfg, const char *path,
        const struct lfs3_emubd_cfg *bdcfg) {
    lfs3_emubd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // setup testing things
    bd->blocks = NULL;
    bd->paused = false;
    bd->reads = 0;
    bd->progs = 0;
    bd->erases = 0;
    bd->wreads = 0;
    bd->wprogs = 0;
    bd->werases = 0;
    bd->readed = 0;
    bd->progged = 0;
    bd->erased = 0;
    bd->prng = bd->cfg->seed;
    bd->power_cycles = bd->cfg->power_cycles;
    bd->ooo_before = NULL;
    bd->ooo_after = NULL;
    bd->disk = NULL;

    // allocate our block array, all blocks start as uninitialized
    bd->blocks = malloc(
            cfg->block_count * sizeof(lfs3_emubd_block_t*));
    int err;
    if (!bd->blocks) {
        err = LFS3_ERR_NOMEM;
        goto failed;
    }
    memset(bd->blocks, 0,
            cfg->block_count * sizeof(lfs3_emubd_block_t*));

    // allocate extra block arrays to hold our ooo snapshots
    if (bd->cfg->powerloss_behavior == LFS3_EMUBD_POWERLOSS_OOO) {
        bd->ooo_before = malloc(
                cfg->block_count * sizeof(lfs3_emubd_block_t*));
        if (!bd->ooo_before) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
        memset(bd->ooo_before, 0,
                cfg->block_count * sizeof(lfs3_emubd_block_t*));

        bd->ooo_after = malloc(
                cfg->block_count * sizeof(lfs3_emubd_block_t*));
        if (!bd->ooo_after) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
        memset(bd->ooo_after, 0,
                cfg->block_count * sizeof(lfs3_emubd_block_t*));
    }

    if (path) {
        bd->disk = malloc(sizeof(lfs3_emubd_disk_t));
        if (!bd->disk) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
        bd->disk->rc = 1;
        bd->disk->fd = -1;
        bd->disk->scratch = NULL;

        #ifdef _WIN32
        bd->disk->fd = open(path, O_RDWR | O_CREAT | O_BINARY, 0666);
        #else
        bd->disk->fd = open(path, O_RDWR | O_CREAT, 0666);
        #endif
        if (bd->disk->fd < 0) {
            err = -errno;
            goto failed;
        }

        bd->disk->scratch = malloc(cfg->block_size);
        if (!bd->disk->scratch) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
        lfs3_emubd_memzero(cfg, bd->disk->scratch, cfg->block_size);

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

    return 0;

failed:;
    // clean up memory
    free(bd->blocks);
    if (bd->cfg->powerloss_behavior == LFS3_EMUBD_POWERLOSS_OOO) {
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

int lfs3_emubd_create(const struct lfs3_cfg *cfg, const char *path) {
    static const struct lfs3_emubd_cfg defaults = {.erase_value=-1};
    return lfs3_emubd_createcfg(cfg, path, &defaults);
}

int lfs3_emubd_destroy(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    // decrement reference counts
    for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
        lfs3_emubd_decblock(bd->blocks[i]);
    }
    free(bd->blocks);

    if (bd->cfg->powerloss_behavior == LFS3_EMUBD_POWERLOSS_OOO) {
        for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
            lfs3_emubd_decblock(bd->ooo_before[i]);
        }
        free(bd->ooo_before);

        for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
            lfs3_emubd_decblock(bd->ooo_after[i]);
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

    return 0;
}


// block device API

int lfs3_emubd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size) {
    lfs3_emubd_t *bd = cfg->context;

    // check if read is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->read_size == 0);
    LFS3_ASSERT(size % cfg->read_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // get the block
    const lfs3_emubd_block_t *b = bd->blocks[block];
    if (b) {
        // block bad?
        if (b->wear > bd->cfg->erase_cycles) {
            // erroring reads? error
            if (bd->cfg->badblock_behavior
                        == LFS3_EMUBD_BADBLOCK_READCORRUPT
                    || (bd->cfg->badblock_behavior
                            == LFS3_EMUBD_BADBLOCK_READQUASICORRUPT
                        && (lfs3_emubd_prng_(&bd->prng) & 1))) {
                return LFS3_ERR_CORRUPT;
            } else if (LFS3_IFDEF_CONDEMN(
                    bd->cfg->badblock_behavior
                            == LFS3_EMUBD_BADBLOCK_READBAD
                        || (bd->cfg->badblock_behavior
                                == LFS3_EMUBD_BADBLOCK_READQUASIBAD
                            && (lfs3_emubd_prng_(&bd->prng) & 1)),
                    false)) {
                #ifdef LFS3_CONDEMN
                return LFS3_ERR_BAD;
                #endif
            }
        }

        // read data
        lfs3_emubd_memread(cfg, buffer, &b->data[off], size);

        // metastable? randomly decide if our bad bit flips
        if (b->metastable) {
            lfs3_size_t bit = b->bad_bit & 0x7fffffff;
            if (bit/8 >= off
                    && bit/8 < off+size
                    && (lfs3_emubd_prng_(&bd->prng) & 1)) {
                ((uint8_t*)buffer)[(bit/8) - off] ^= 1 << (bit%8);
            }
        }

    // no block yet
    } else {
        // zero for consistency
        lfs3_emubd_memzero(cfg, buffer, size);
    }   

    // track reads
    if (!bd->paused) {
        bd->reads += 1;
        bd->wreads += (lfs3_alignup(off + size,
                        lfs3_max(bd->cfg->read_width, 1))
                    - lfs3_aligndown(off,
                        lfs3_max(bd->cfg->read_width, 1)))
                / lfs3_max(bd->cfg->read_width, 1);
        bd->readed += size;
    }
    if (bd->cfg->read_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->read_sleep/1000000000,
                .tv_nsec=bd->cfg->read_sleep%1000000000},
            NULL);
        if (err) {
            return -errno;
        }
    }

    // block bad?
    if (b && b->wear > bd->cfg->erase_cycles) {
        // warninging reads? error
        if (LFS3_IFDEF_REPAIR(
                bd->cfg->badblock_behavior
                        == LFS3_EMUBD_BADBLOCK_READDAMAGED
                    || (bd->cfg->badblock_behavior
                            == LFS3_EMUBD_BADBLOCK_READQUASIDAMAGED
                        && (lfs3_emubd_prng_(&bd->prng) & 1)),
                false)) {
            #ifdef LFS3_REPAIR
            return LFS3_ERR_DAMAGED;
            #endif
        } else if (LFS3_IFDEF_CONDEMN(
                bd->cfg->badblock_behavior
                        == LFS3_EMUBD_BADBLOCK_READCONDEMNED
                    || (bd->cfg->badblock_behavior
                            == LFS3_EMUBD_BADBLOCK_READQUASICONDEMNED
                        && (lfs3_emubd_prng_(&bd->prng) & 1)),
                false)) {
            #ifdef LFS3_CONDEMN
            return LFS3_ERR_CONDEMNED;
            #endif
        }
    }
    return 0;
}

int lfs3_emubd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size) {
    lfs3_emubd_t *bd = cfg->context;

    // check if write is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->prog_size == 0);
    LFS3_ASSERT(size % cfg->prog_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // were we erased properly?
    LFS3_ASSERT(bd->blocks[block]);
    if (bd->cfg->erase_value >= 0
            && bd->blocks[block]->wear <= bd->cfg->erase_cycles) {
        for (lfs3_off_t i = 0; i < size; i++) {
            LFS3_ASSERT(bd->blocks[block]->data[off+i] == bd->cfg->erase_value);
        }
    }

    // losing power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // emulating some bits? choose a random bit to flip
            if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_SOMEBITS) {
                // mutate the block
                lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    return LFS3_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // flip bit
                lfs3_size_t bit = lfs3_emubd_prng_(&bd->prng)
                        % (cfg->prog_size*8);
                b->data[off + (bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size + (off_t)off,
                            SEEK_SET);
                    if (res1 < 0) {
                        return -errno;
                    }

                    ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
                    if (res2 < 0) {
                        return -errno;
                    }
                }

            // emulating most bits? prog data and choose a random bit
            // to flip
            } else if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_MOSTBITS) {
                // mutate the block
                lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    return LFS3_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // prog data
                lfs3_emubd_memprog(cfg, &b->data[off], buffer, size);

                // flip bit
                lfs3_size_t bit = lfs3_emubd_prng_(&bd->prng)
                        % (cfg->prog_size*8);
                b->data[off + (bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size + (off_t)off,
                            SEEK_SET);
                    if (res1 < 0) {
                        return -errno;
                    }

                    ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
                    if (res2 < 0) {
                        return -errno;
                    }
                }

            // emulating out-of-order writes? revert everything unsynced
            // except for our current block
            } else if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_OOO) {
                for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
                    lfs3_emubd_decblock(bd->ooo_after[i]);
                    bd->ooo_after[i] = lfs3_emubd_incblock(bd->blocks[i]);

                    if (i != block && bd->blocks[i] != bd->ooo_before[i]) {
                        lfs3_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs3_emubd_incblock(bd->ooo_before[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                return -errno;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                return -errno;
                            }
                        }
                    }
                }

            // emulating metastability? prog data, choose a random bad bit,
            // and mark as metastable
            } else if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_METASTABLE) {
                // mutate the block
                lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    return LFS3_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // prog data
                lfs3_emubd_memprog(cfg, &b->data[off], buffer, size);

                // choose a new bad bit unless overridden
                if (!(0x80000000 & b->bad_bit)) {
                    b->bad_bit = lfs3_emubd_prng_(&bd->prng)
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
                        return -errno;
                    }

                    ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
                    if (res2 < 0) {
                        return -errno;
                    }
                }
            }

            // powerloss!
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);

            // oh, continuing? undo out-of-order write emulation
            if (bd->cfg->powerloss_behavior == LFS3_EMUBD_POWERLOSS_OOO) {
                for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
                    if (bd->blocks[i] != bd->ooo_after[i]) {
                        lfs3_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs3_emubd_incblock(bd->ooo_after[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                return -errno;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                return -errno;
                            }
                        }
                    }
                }
            }
        }
    }

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // block bad?
    if (b->wear > bd->cfg->erase_cycles) {
        // erroring progs? error
        if (bd->cfg->badblock_behavior
                == LFS3_EMUBD_BADBLOCK_PROGCORRUPT) {
            return LFS3_ERR_CORRUPT;

        } else if (LFS3_IFDEF_CONDEMN(
                bd->cfg->badblock_behavior
                        == LFS3_EMUBD_BADBLOCK_PROGBAD,
                false)) {
            #ifdef LFS3_CONDEMN
            return LFS3_ERR_BAD;
            #endif

        // noop progs? skip
        } else if (bd->cfg->badblock_behavior
                    == LFS3_EMUBD_BADBLOCK_PROGNOOP
                || bd->cfg->badblock_behavior
                    == LFS3_EMUBD_BADBLOCK_ERASENOOP) {
            goto progged;

        // progs flipping bits? flip our bad bit, exactly which bit
        // is chosen during erase
        } else if (bd->cfg->badblock_behavior
                    == LFS3_EMUBD_BADBLOCK_PROGFLIP) {
            lfs3_size_t bit = b->bad_bit & 0x7fffffff;
            if (bit/8 >= off && bit/8 < off+size) {
                // prog data
                lfs3_emubd_memprog(cfg, &b->data[off], buffer, size);
                b->data[bit/8] ^= 1 << (bit%8);
                goto progged;
            }

        // reads flipping bits? prog as normal but mark as metastable
        } else if (bd->cfg->badblock_behavior
                    == LFS3_EMUBD_BADBLOCK_READQUASIFLIP) {
            // prog data
            lfs3_emubd_memprog(cfg, &b->data[off], buffer, size);
            b->metastable = true;
            goto progged;
        }
    }

    // prog data
    lfs3_emubd_memprog(cfg, &b->data[off], buffer, size);

    // clear any metastability
    b->metastable = false;

progged:;
    // mirror to disk file?
    if (bd->disk) {
        off_t res1 = lseek(bd->disk->fd,
                (off_t)block*cfg->block_size + (off_t)off,
                SEEK_SET);
        if (res1 < 0) {
            return -errno;
        }

        ssize_t res2 = write(bd->disk->fd, &b->data[off], size);
        if (res2 < 0) {
            return -errno;
        }
    }

    // track progs
    if (!bd->paused) {
        bd->progs += 1;
        bd->wprogs += (lfs3_alignup(off + size,
                        lfs3_max(bd->cfg->prog_width, 1))
                    - lfs3_aligndown(off,
                        lfs3_max(bd->cfg->prog_width, 1)))
                / lfs3_max(bd->cfg->prog_width, 1);
        bd->progged += size;
    }
    if (bd->cfg->prog_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->prog_sleep/1000000000,
                .tv_nsec=bd->cfg->prog_sleep%1000000000},
            NULL);
        if (err) {
            return -errno;
        }
    }

    // block bad?
    if (b && b->wear > bd->cfg->erase_cycles) {
        // warninging progs? error
        if (LFS3_IFDEF_REPAIR(
                bd->cfg->badblock_behavior
                    == LFS3_EMUBD_BADBLOCK_PROGDAMAGED,
                false)) {
            #ifdef LFS3_REPAIR
            return LFS3_ERR_DAMAGED;
            #endif
        } else if (LFS3_IFDEF_CONDEMN(
                bd->cfg->badblock_behavior
                    == LFS3_EMUBD_BADBLOCK_PROGCONDEMNED,
                false)) {
            #ifdef LFS3_CONDEMN
            return LFS3_ERR_CONDEMNED;
            #endif
        }
    }
    return 0;
}

int lfs3_emubd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block) {
    lfs3_emubd_t *bd = cfg->context;

    // check if erase is valid
    LFS3_ASSERT(block < cfg->block_count);

    // losing power?
    if (bd->power_cycles > 0) {
        bd->power_cycles -= 1;
        if (bd->power_cycles == 0) {
            // emulating some bits? choose a random bit to flip
            if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_SOMEBITS) {
                // mutate the block
                lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    return LFS3_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // flip bit
                lfs3_size_t bit = lfs3_emubd_prng_(&bd->prng)
                        % (cfg->block_size*8);
                b->data[(bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size,
                            SEEK_SET);
                    if (res1 < 0) {
                        return -errno;
                    }

                    ssize_t res2 = write(bd->disk->fd,
                            b->data, cfg->block_size);
                    if (res2 < 0) {
                        return -errno;
                    }
                }

            // emulating most bits? erase data and choose a random bit
            // to flip
            } else if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_MOSTBITS) {
                // mutate the block
                lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    return LFS3_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // emulate an erase value?
                if (bd->cfg->erase_value != -1) {
                    lfs3_emubd_memerase(cfg, b->data, cfg->block_size);
                }

                // flip bit
                lfs3_size_t bit = lfs3_emubd_prng_(&bd->prng)
                        % (cfg->block_size*8);
                b->data[(bit/8)] ^= 1 << (bit%8);

                // mirror to disk file?
                if (bd->disk) {
                    off_t res1 = lseek(bd->disk->fd,
                            (off_t)block*cfg->block_size,
                            SEEK_SET);
                    if (res1 < 0) {
                        return -errno;
                    }

                    ssize_t res2 = write(bd->disk->fd,
                            b->data, cfg->block_size);
                    if (res2 < 0) {
                        return -errno;
                    }
                }

            // emulating out-of-order writes? revert everything unsynced
            // except for our current block
            } else if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_OOO) {
                for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
                    if (i != block && bd->blocks[i] != bd->ooo_before[i]) {
                        lfs3_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs3_emubd_incblock(bd->ooo_before[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                return -errno;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                return -errno;
                            }
                        }
                    }
                }

            // emulating metastability? erase data, choose a random bad bit,
            // and mark as metastable
            } else if (bd->cfg->powerloss_behavior
                    == LFS3_EMUBD_POWERLOSS_METASTABLE) {
                // mutate the block
                lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg,
                        bd->blocks[block]);
                if (!b) {
                    return LFS3_ERR_NOMEM;
                }
                bd->blocks[block] = b;

                // emulate an erase value?
                if (bd->cfg->erase_value != -1) {
                    lfs3_emubd_memerase(cfg, b->data, cfg->block_size);
                }

                // choose a new bad bit unless overridden
                if (!(0x80000000 & b->bad_bit)) {
                    b->bad_bit = lfs3_emubd_prng_(&bd->prng)
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
                        return -errno;
                    }

                    ssize_t res2 = write(bd->disk->fd,
                            b->data, cfg->block_size);
                    if (res2 < 0) {
                        return -errno;
                    }
                }
            }

            // powerloss!
            bd->cfg->powerloss_cb(bd->cfg->powerloss_data);

            // oh, continuing? undo out-of-order write emulation
            if (bd->cfg->powerloss_behavior == LFS3_EMUBD_POWERLOSS_OOO) {
                for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
                    if (bd->blocks[i] != bd->ooo_after[i]) {
                        lfs3_emubd_decblock(bd->blocks[i]);
                        bd->blocks[i] = lfs3_emubd_incblock(bd->ooo_after[i]);

                        // mirror to disk file?
                        if (bd->disk) {
                            off_t res1 = lseek(bd->disk->fd,
                                    (off_t)i*cfg->block_size,
                                    SEEK_SET);
                            if (res1 < 0) {
                                return -errno;
                            }

                            ssize_t res2 = write(bd->disk->fd,
                                    (bd->blocks[i])
                                        ? bd->blocks[i]->data
                                        : bd->disk->scratch,
                                    cfg->block_size);
                            if (res2 < 0) {
                                return -errno;
                            }
                        }
                    }
                }
            }
        }
    }

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
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
                == LFS3_EMUBD_BADBLOCK_ERASECORRUPT) {
            return LFS3_ERR_CORRUPT;

        } else if (LFS3_IFDEF_CONDEMN(
                bd->cfg->badblock_behavior
                        == LFS3_EMUBD_BADBLOCK_ERASEBAD,
                false)) {
            #ifdef LFS3_CONDEMN
            return LFS3_ERR_BAD;
            #endif

        // noop erases? skip
        } else if (bd->cfg->badblock_behavior
                == LFS3_EMUBD_BADBLOCK_ERASENOOP) {
            goto erased;

        // flipping bits? if we're not manually overridden, choose a
        // new bad bit on erase, this makes it more likely to
        // eventually cause problems
        } else {
            if (!(0x80000000 & b->bad_bit)) {
                b->bad_bit = lfs3_emubd_prng_(&bd->prng)
                        % (cfg->block_size*8);
            }
        }
    }

    // emulate an erase value?
    if (bd->cfg->erase_value != -1) {
        lfs3_emubd_memerase(cfg, b->data, cfg->block_size);

        // mirror to disk file?
        if (bd->disk) {
            off_t res1 = lseek(bd->disk->fd,
                    (off_t)block*cfg->block_size,
                    SEEK_SET);
            if (res1 < 0) {
                return -errno;
            }

            ssize_t res2 = write(bd->disk->fd, b->data, cfg->block_size);
            if (res2 < 0) {
                return -errno;
            }
        }
    }

    // clear any metastability
    b->metastable = false;

erased:;
    // track erases
    if (!bd->paused) {
        bd->erases += 1;
        bd->werases += lfs3_alignup(cfg->block_size,
                    lfs3_max(bd->cfg->erase_width, 1))
                / lfs3_max(bd->cfg->erase_width, 1);
        bd->erased += cfg->block_size;
    }
    if (bd->cfg->erase_sleep) {
        int err = nanosleep(&(struct timespec){
                .tv_sec=bd->cfg->erase_sleep/1000000000,
                .tv_nsec=bd->cfg->erase_sleep%1000000000},
            NULL);
        if (err) {
            return -errno;
        }
    }

    // block bad?
    if (b && b->wear > bd->cfg->erase_cycles) {
        // warninging erases? error
        if (LFS3_IFDEF_REPAIR(
                bd->cfg->badblock_behavior
                    == LFS3_EMUBD_BADBLOCK_ERASEDAMAGED,
                false)) {
            #ifdef LFS3_REPAIR
            return LFS3_ERR_DAMAGED;
            #endif
        } else if (LFS3_IFDEF_CONDEMN(
                bd->cfg->badblock_behavior
                        == LFS3_EMUBD_BADBLOCK_ERASECONDEMNED,
                false)) {
            #ifdef LFS3_CONDEMN
            return LFS3_ERR_CONDEMNED;
            #endif
        }
    }
    return 0;
}

int lfs3_emubd_sync(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    // emulate out-of-order writes? save a snapshot on sync
    if (bd->cfg->powerloss_behavior == LFS3_EMUBD_POWERLOSS_OOO) {
        for (size_t i = 0; i < cfg->block_count; i++) {
            lfs3_emubd_decblock(bd->ooo_before[i]);
            bd->ooo_before[i] = lfs3_emubd_incblock(bd->blocks[i]);
        }
    }

    return 0;
}


/// Additional emubd features for testing ///

lfs3_emubd_sns_t lfs3_emubd_simtime(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    // error if all possible timings are zero
    if (bd->cfg->read_timing == 0
            && bd->cfg->prog_timing == 0
            && bd->cfg->erase_timing == 0
            && bd->cfg->read_wtiming == 0
            && bd->cfg->prog_wtiming == 0
            && bd->cfg->erase_wtiming == 0
            && bd->cfg->read_utiming == 0
            && bd->cfg->prog_utiming == 0
            && bd->cfg->erase_utiming == 0) {
        return LFS3_ERR_NOTSUP;
    }

    return (bd->cfg->read_timing * bd->reads)
            + (bd->cfg->prog_timing * bd->progs)
            + (bd->cfg->erase_timing * bd->erases)
            + (bd->cfg->read_wtiming * bd->wreads)
            + (bd->cfg->prog_wtiming * bd->wprogs)
            + (bd->cfg->erase_wtiming * bd->werases)
            + (bd->cfg->read_utiming * bd->readed)
            + (bd->cfg->prog_utiming * bd->progged)
            + (bd->cfg->erase_utiming * bd->erased);
}

int lfs3_emubd_simreset(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    bd->reads = 0;
    bd->progs = 0;
    bd->erases = 0;
    bd->wreads = 0;
    bd->wprogs = 0;
    bd->werases = 0;
    bd->readed = 0;
    bd->progged = 0;
    bd->erased = 0;
    return 0;
}

int lfs3_emubd_simpause(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    bd->paused += 1;
    return 0;
}

int lfs3_emubd_simresume(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    LFS3_ASSERT(bd->paused);
    bd->paused -= 1;
    return 0;
}

lfs3_emubd_sio_t lfs3_emubd_reads(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->reads;
}

lfs3_emubd_sio_t lfs3_emubd_progs(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->progs;
}

lfs3_emubd_sio_t lfs3_emubd_erases(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->erases;
}

lfs3_emubd_sio_t lfs3_emubd_wreads(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->wreads;
}

lfs3_emubd_sio_t lfs3_emubd_wprogs(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->wprogs;
}

lfs3_emubd_sio_t lfs3_emubd_werases(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->werases;
}

lfs3_emubd_sio_t lfs3_emubd_readed(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->readed;
}

lfs3_emubd_sio_t lfs3_emubd_progged(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->progged;
}

lfs3_emubd_sio_t lfs3_emubd_erased(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->erased;
}

lfs3_emubd_swear_t lfs3_emubd_wear(const struct lfs3_cfg *cfg,
        lfs3_block_t block) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // get the wear
    lfs3_emubd_wear_t wear;
    const lfs3_emubd_block_t *b = bd->blocks[block];
    if (b) {
        wear = b->wear;
    } else {
        wear = 0;
    }

    return wear;
}

int lfs3_emubd_setwear(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_emubd_wear_t wear) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // no block? no wear? no reason to allocate
    if ((!bd->blocks[block] && wear == 0)
            // matching wear? no reason to allocate
            || (bd->blocks[block] && bd->blocks[block]->wear == wear)) {
        return 0;
    }

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = wear;

    return 0;
}

int lfs3_emubd_mkbad(const struct lfs3_cfg *cfg,
        lfs3_block_t block) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = -1;

    // choose a bad bit now in case this block is never erased
    if (!(0x80000000 & b->bad_bit)) {
        b->bad_bit = lfs3_emubd_prng_(&bd->prng)
                % (cfg->block_size*8);
    }

    return 0;
}

int lfs3_emubd_mkgood(const struct lfs3_cfg *cfg,
        lfs3_block_t block) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // no block? no reason to allocate
    if (!bd->blocks[block]
            // matching wear? no reason to allocate
            || bd->blocks[block]->wear == 0) {
        return 0;
    }

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = 0;

    return 0;
}

lfs3_ssize_t lfs3_emubd_badbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // get the bad bit
    lfs3_size_t bad_bit;
    const lfs3_emubd_block_t *b = bd->blocks[block];
    if (b) {
        bad_bit = 0x7fffffff & b->bad_bit;
    } else {
        bad_bit = 0;
    }

    return bad_bit;
}

int lfs3_emubd_setbadbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_size_t bit) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the bad bit and mark as fixed
    b->bad_bit = 0x80000000 | bit;

    return 0;
}

int lfs3_emubd_randomizebadbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // mark the bad bit as randomized
    b->bad_bit &= ~0x80000000;

    return 0;
}

int lfs3_emubd_mkbadbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_size_t bit) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // set the wear
    b->wear = -1;
    // set the bad bit and mark as fixed
    b->bad_bit = 0x80000000 | bit;

    return 0;
}

int lfs3_emubd_flipbit_(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_size_t bit) {
    lfs3_emubd_t *bd = cfg->context;

    // check if block is valid
    LFS3_ASSERT(block < cfg->block_count);

    // mutate the block
    lfs3_emubd_block_t *b = lfs3_emubd_mutblock(cfg, bd->blocks[block]);
    if (!b) {
        return LFS3_ERR_NOMEM;
    }
    bd->blocks[block] = b;

    // flip the bit
    b->data[bit/8] ^= 1 << (bit%8);

    // mirror to disk file?
    if (bd->disk) {
        off_t res1 = lseek(bd->disk->fd,
                (off_t)block*cfg->block_size + (off_t)(bit/8),
                SEEK_SET);
        if (res1 < 0) {
            int err = -errno;
            return err;
        }

        ssize_t res2 = write(bd->disk->fd, &b->data[bit/8], 1);
        if (res2 < 0) {
            int err = -errno;
            return err;
        }
    }

    return 0;
}


int lfs3_emubd_flipbit(const struct lfs3_cfg *cfg,
        lfs3_block_t block, lfs3_size_t bit) {
    // flip the bit
    int err = lfs3_emubd_flipbit_(cfg, block, bit);
    if (err) {
        return err;
    }

    return 0;
}

int lfs3_emubd_flip(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    // flip all bits in bad blocks, make sure not to allocate blocks we
    // don't need
    for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
        const lfs3_emubd_block_t *b = bd->blocks[i];
        if (b && b->wear > bd->cfg->erase_cycles) {
            int err = lfs3_emubd_flipbit_(cfg, i, b->bad_bit & 0x7fffffff);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

lfs3_emubd_spowercycles_t lfs3_emubd_powercycles(
        const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;
    return bd->power_cycles;
}

int lfs3_emubd_setpowercycles(const struct lfs3_cfg *cfg,
        lfs3_emubd_powercycles_t power_cycles) {
    lfs3_emubd_t *bd = cfg->context;

    bd->power_cycles = power_cycles;
    return 0;
}

void lfs3_emubd_seed(const struct lfs3_cfg *cfg, uint32_t seed) {
    lfs3_emubd_t *bd = cfg->context;

    bd->prng = seed;
}

uint32_t lfs3_emubd_prng(const struct lfs3_cfg *cfg) {
    lfs3_emubd_t *bd = cfg->context;

    return lfs3_emubd_prng_(&bd->prng);
}


int lfs3_emubd_cpy(const struct lfs3_cfg *cfg, lfs3_emubd_t *copy) {
    lfs3_emubd_t *bd = cfg->context;

    // lazily copy over our block array
    copy->blocks = malloc(
            cfg->block_count * sizeof(lfs3_emubd_block_t*));
    if (!copy->blocks) {
        return LFS3_ERR_NOMEM;
    }
    for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
        copy->blocks[i] = lfs3_emubd_incblock(bd->blocks[i]);
    }

    if (bd->cfg->powerloss_behavior == LFS3_EMUBD_POWERLOSS_OOO) {
        copy->ooo_before = malloc(
                cfg->block_count * sizeof(lfs3_emubd_block_t*));
        if (!copy->ooo_before) {
            return LFS3_ERR_NOMEM;
        }
        for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
            copy->ooo_before[i] = lfs3_emubd_incblock(bd->ooo_before[i]);
        }

        copy->ooo_after = malloc(
                cfg->block_count * sizeof(lfs3_emubd_block_t*));
        if (!copy->ooo_after) {
            return LFS3_ERR_NOMEM;
        }
        for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
            copy->ooo_after[i] = lfs3_emubd_incblock(bd->ooo_after[i]);
        }
    }

    // other state
    copy->paused = bd->paused;
    copy->reads = bd->reads;
    copy->progs = bd->progs;
    copy->erases = bd->erases;
    copy->wreads = bd->wreads;
    copy->wprogs = bd->wprogs;
    copy->werases = bd->werases;
    copy->readed = bd->readed;
    copy->progged = bd->progged;
    copy->erased = bd->erased;
    copy->prng = bd->prng;
    copy->power_cycles = bd->power_cycles;
    copy->disk = bd->disk;
    if (copy->disk) {
        copy->disk->rc += 1;
    }
    copy->cfg = bd->cfg;

    return 0;
}

