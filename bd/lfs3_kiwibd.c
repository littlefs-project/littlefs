/*
 * kiwibd - A lightweight variant of emubd, useful for emulating large
 * disks backed by a file or in RAM.
 *
 * Unlike emubd, file-backed disks are _not_ mirrored in RAM. kiwibd has
 * fewer features than emubd, prioritizing speed for benchmarking.
 *
 *
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "bd/lfs3_kiwibd.h"

#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>



// low-level flash memory emulation

// read data
static inline void lfs3_kiwibd_memread(const struct lfs3_cfg *cfg,
        void *restrict dst, const void *restrict src, size_t size) {
    (void)cfg;
    memcpy(dst, src, size);
}

static inline void lfs3_kiwibd_memprog(const struct lfs3_cfg *cfg,
        void *restrict dst, const void *restrict src, size_t size) {
    lfs3_kiwibd_t *bd = cfg->context;
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

static inline void lfs3_kiwibd_memerase(const struct lfs3_cfg *cfg,
        void *restrict dst, size_t size) {
    lfs3_kiwibd_t *bd = cfg->context;
    // emulating erase value?
    if (bd->cfg->erase_value != -1) {
        memset(dst,
                (bd->cfg->erase_value >= 0)
                    ? bd->cfg->erase_value
                    : 0xff,
                size);
    }
}

// this is slightly different from lfs3_kiwibd_memerase in that we use
// lfs3_kiwibd_memzero when we need to unconditionally zero memory
static inline void lfs3_kiwibd_memzero(const struct lfs3_cfg *cfg,
        void *restrict dst, size_t size) {
    lfs3_kiwibd_t *bd = cfg->context;
    memset(dst,
            (bd->cfg->erase_value == -1) ? 0
                : (bd->cfg->erase_value >= 0) ? bd->cfg->erase_value
                : (bd->cfg->erase_value == -2) ? 0xff
                : 0,
            size);
}



// kiwibd create/destroy

int lfs3_kiwibd_createcfg(const struct lfs3_cfg *cfg, const char *path,
        const struct lfs3_kiwibd_cfg *bdcfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_createcfg("
                "%p {"
                    ".context=%p, "
                    ".read=%p, "
                    ".prog=%p, "
                    ".erase=%p, "
                    ".sync=%p, "
                    ".read_size=%"PRIu32", "
                    ".prog_size=%"PRIu32", "
                    ".block_size=%"PRIu32", "
                    ".block_count=%"PRIu32"}, "
                "\"%s\", "
                "%p {"
                    ".erase_value=%"PRId32", "
                    ".buffer=%p, "
                    ".read_sleep=%"PRIu64", "
                    ".prog_sleep=%"PRIu64", "
                    ".erase_sleep=%"PRIu64"})",
            (void*)cfg,
            cfg->context,
            (void*)(uintptr_t)cfg->read,
            (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase,
            (void*)(uintptr_t)cfg->sync,
            cfg->read_size,
            cfg->prog_size,
            cfg->block_size,
            cfg->block_count,
            path,
            (void*)bdcfg,
            bdcfg->erase_value,
            bdcfg->buffer,
            bdcfg->read_sleep,
            bdcfg->prog_sleep,
            bdcfg->erase_sleep);
    lfs3_kiwibd_t *bd = cfg->context;
    bd->cfg = bdcfg;

    // setup some initial state
    bd->paused = false;
    bd->reads = 0;
    bd->progs = 0;
    bd->erases = 0;
    bd->readed = 0;
    bd->progged = 0;
    bd->erased = 0;
    bd->fd = -1;
    if (path) {
        bd->u.scratch = NULL;
    } else {
        bd->u.mem = NULL;
    }
    int err;

    // if we have a path, try to open the backing file
    if (path) {
        bd->fd = open(path, O_RDWR | O_CREAT, 0666);
        if (bd->fd < 0) {
            err = -errno;
            goto failed;
        }

        // allocate a scratch buffer to help with zeroing/masking/etc
        bd->u.scratch = malloc(cfg->block_size);
        if (!bd->u.scratch) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }

        // zero for reproducibility
        lfs3_kiwibd_memzero(cfg, bd->u.scratch, cfg->block_size);
        for (lfs3_block_t i = 0; i < cfg->block_count; i++) {
            ssize_t res = write(bd->fd,
                    bd->u.scratch,
                    cfg->block_size);
            if (res < 0) {
                err = -errno;
                goto failed;
            }
        }

    // otherwise, try to malloc a big memory array
    } else {
        bd->u.mem = malloc((size_t)cfg->block_size * cfg->block_count);
        if (!bd->u.mem) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }

        // zero for reproducibility
        lfs3_kiwibd_memzero(cfg, bd->u.mem,
                (size_t)cfg->block_size * cfg->block_count);
    }

    LFS3_KIWIBD_TRACE("lfs3_kiwibd_createcfg -> %d", 0);
    return 0;

failed:;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_createcfg -> %d", err);
    // clean up memory
    if (bd->fd >= 0) {
        close(bd->fd);
        free(bd->u.scratch);
    } else {
        free(bd->u.mem);
    }
    return err;
}

int lfs3_kiwibd_create(const struct lfs3_cfg *cfg, const char *path) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_create("
                "%p {"
                    ".context=%p, "
                    ".read=%p, "
                    ".prog=%p, "
                    ".erase=%p, "
                    ".sync=%p, "
                    ".read_size=%"PRIu32", "
                    ".prog_size=%"PRIu32", "
                    ".block_size=%"PRIu32", "
                    ".block_count=%"PRIu32"}, "
                "\"%s\")",
            (void*)cfg,
            cfg->context,
            (void*)(uintptr_t)cfg->read,
            (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase,
            (void*)(uintptr_t)cfg->sync,
            cfg->read_size,
            cfg->prog_size,
            cfg->block_size,
            cfg->block_count,
            path);
    static const struct lfs3_kiwibd_cfg defaults = {.erase_value=-1};
    int err = lfs3_kiwibd_createcfg(cfg, path, &defaults);
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_create -> %d", err);
    return err;
}

int lfs3_kiwibd_destroy(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_destroy(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;

    // clean up memory
    if (bd->fd >= 0) {
        close(bd->fd);
        free(bd->u.scratch);
    } else {
        free(bd->u.mem);
    }

    LFS3_KIWIBD_TRACE("lfs3_kiwibd_destroy -> %d", 0);
    return 0;
}


// block device API

int lfs3_kiwibd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_read(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs3_kiwibd_t *bd = cfg->context;

    // check if read is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->read_size == 0);
    LFS3_ASSERT(size % cfg->read_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // read in file?
    if (bd->fd >= 0) {
        lfs3_kiwibd_memerase(cfg,
                bd->u.scratch,
                cfg->block_size);

        off_t res = lseek(bd->fd,
                (off_t)block*cfg->block_size + (off_t)off,
                SEEK_SET);
        if (res < 0) {
            int err = -errno;
            LFS3_KIWIBD_TRACE("lfs3_kiwibd_read -> %d", err);
            return err;
        }

        ssize_t res_ = read(bd->fd, buffer, size);
        if (res_ < 0) {
            int err = -errno;
            LFS3_KIWIBD_TRACE("lfs3_kiwibd_read -> %d", err);
            return err;
        }

    // read in RAM?
    } else {
        lfs3_kiwibd_memread(cfg,
                buffer,
                &bd->u.mem[(size_t)block*cfg->block_size + (size_t)off],
                size);
    }

    // track reads
    if (!bd->paused) {
        bd->reads += (lfs3_alignup(off + size,
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
            err = -errno;
            LFS3_KIWIBD_TRACE("lfs3_kiwibd_read -> %d", err);
            return err;
        }
    }

    LFS3_KIWIBD_TRACE("lfs3_kiwibd_read -> %d", 0);
    return 0;
}

int lfs3_kiwibd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog(%p, "
                "0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs3_kiwibd_t *bd = cfg->context;

    // check if write is valid
    LFS3_ASSERT(block < cfg->block_count);
    LFS3_ASSERT(off  % cfg->prog_size == 0);
    LFS3_ASSERT(size % cfg->prog_size == 0);
    LFS3_ASSERT(off+size <= cfg->block_size);

    // prog in file?
    if (bd->fd >= 0) {
        // were we erased properly?
        if (bd->cfg->erase_value >= 0) {
            off_t res = lseek(bd->fd,
                    (off_t)block*cfg->block_size + (off_t)off,
                    SEEK_SET);
            if (res < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }

            ssize_t res_ = read(bd->fd, bd->u.scratch, size);
            if (res_ < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }

            for (lfs3_off_t i = 0; i < size; i++) {
                LFS3_ASSERT(bd->u.scratch[i] == bd->cfg->erase_value);
            }
        }

        // masking progs?
        if (bd->cfg->erase_value == -2) {
            off_t res = lseek(bd->fd,
                    (off_t)block*cfg->block_size + (off_t)off,
                    SEEK_SET);
            if (res < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }

            ssize_t res_ = read(bd->fd, bd->u.scratch, size);
            if (res_ < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }

            lfs3_kiwibd_memprog(cfg, bd->u.scratch, buffer, size);

            res = lseek(bd->fd,
                    (off_t)block*cfg->block_size + (off_t)off,
                    SEEK_SET);
            if (res < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }

            res_ = write(bd->fd, bd->u.scratch, size);
            if (res_ < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }

        // normal progs?
        } else {
            off_t res = lseek(bd->fd,
                    (off_t)block*cfg->block_size + (off_t)off,
                    SEEK_SET);
            if (res < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }

            ssize_t res_ = write(bd->fd, buffer, size);
            if (res_ < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
                return err;
            }
        }

    // prog in RAM?
    } else {
        // were we erased properly?
        if (bd->cfg->erase_value >= 0) {
            for (lfs3_off_t i = 0; i < size; i++) {
                LFS3_ASSERT(
                        bd->u.mem[(size_t)block*cfg->block_size + (size_t)off]
                            == bd->cfg->erase_value);
            }
        }

        lfs3_kiwibd_memprog(cfg,
                &bd->u.mem[(size_t)block*cfg->block_size + (size_t)off],
                buffer,
                size);
    }

    // track progs
    if (!bd->paused) {
        bd->progs += (lfs3_alignup(off + size,
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
            err = -errno;
            LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", err);
            return err;
        }
    }

    LFS3_KIWIBD_TRACE("lfs3_kiwibd_prog -> %d", 0);
    return 0;
}

int lfs3_kiwibd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_erase(%p, 0x%"PRIx32" (%"PRIu32"))",
            (void*)cfg, block, cfg->block_size);
    lfs3_kiwibd_t *bd = cfg->context;

    // check if erase is valid
    LFS3_ASSERT(block < cfg->block_count);

    // emulate an erase value?
    if (bd->cfg->erase_value != -1) {
        // erase in file?
        if (bd->fd >= 0) {
            off_t res = lseek(bd->fd,
                    (off_t)block*cfg->block_size,
                    SEEK_SET);
            if (res < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_erase -> %d", err);
                return err;
            }

            lfs3_kiwibd_memerase(cfg,
                    bd->u.scratch,
                    cfg->block_size);

            ssize_t res_ = write(bd->fd,
                    bd->u.scratch,
                    cfg->block_size);
            if (res_ < 0) {
                int err = -errno;
                LFS3_KIWIBD_TRACE("lfs3_kiwibd_erase -> %d", err);
                return err;
            }

        // erase in RAM?
        } else {
            lfs3_kiwibd_memerase(cfg,
                    &bd->u.mem[(size_t)block*cfg->block_size],
                    cfg->block_size);
        }
    }

erased:;
    // track erases
    if (!bd->paused) {
        bd->erases += lfs3_alignup(cfg->block_size,
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
            err = -errno;
            LFS3_KIWIBD_TRACE("lfs3_kiwibd_erase -> %d", err);
            return err;
        }
    }

    LFS3_KIWIBD_TRACE("lfs3_kiwibd_erase -> %d", 0);
    return 0;
}

int lfs3_kiwibd_sync(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_sync(%p)", (void*)cfg);

    // in theory we could actually sync here, but if our goal is
    // performance, why bother?
    //
    // filebd may be a better block device is your goal is actual
    // storage

    // sync is a noop
    (void)cfg;

    LFS3_KIWIBD_TRACE("lfs3_kiwibd_sync -> %d", 0);
    return 0;
}


/// Additional kiwibd features ///

lfs3_kiwibd_sns_t lfs3_kiwibd_simtime(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simtime(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;

    // error if all possible timings are zero
    if (bd->cfg->read_timing == 0
            && bd->cfg->prog_timing == 0
            && bd->cfg->erase_timing == 0
            && bd->cfg->readed_timing == 0
            && bd->cfg->progged_timing == 0
            && bd->cfg->erased_timing == 0) {
        LFS3_KIWIBD_TRACE("lfs3_kiwibd_simtime -> %d", LFS3_ERR_NOTSUP);
        return LFS3_ERR_NOTSUP;
    }

    lfs3_kiwibd_ns_t ns
            = (bd->cfg->read_timing * bd->reads*bd->cfg->read_width)
            + (bd->cfg->prog_timing * bd->progs*bd->cfg->prog_width)
            + (bd->cfg->erase_timing * bd->erases*bd->cfg->erase_width)
            + (bd->cfg->readed_timing * bd->readed)
            + (bd->cfg->progged_timing * bd->progged)
            + (bd->cfg->erased_timing * bd->erased);

    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simtime -> %"PRIu64, ns);
    return ns;
}

int lfs3_kiwibd_simreset(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simreset(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    bd->reads = 0;
    bd->progs = 0;
    bd->erases = 0;
    bd->readed = 0;
    bd->progged = 0;
    bd->erased = 0;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simreset -> %d", 0);
    return 0;
}

int lfs3_kiwibd_simpause(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simpause(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    bd->paused += 1;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simpause -> %d", 0);
    return 0;
}

int lfs3_kiwibd_simresume(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simresume(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    LFS3_ASSERT(bd->paused);
    bd->paused -= 1;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_simresume -> %d", 0);
    return 0;
}

lfs3_kiwibd_sio_t lfs3_kiwibd_reads(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_reads(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_reads -> %"PRIu64, bd->reads);
    return bd->reads;
}

lfs3_kiwibd_sio_t lfs3_kiwibd_progs(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_progs(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_progs -> %"PRIu64, bd->progs);
    return bd->progs;
}

lfs3_kiwibd_sio_t lfs3_kiwibd_erases(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_erases(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_erases -> %"PRIu64, bd->erases);
    return bd->erases;
}

lfs3_kiwibd_sio_t lfs3_kiwibd_readed(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_readed(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_readed -> %"PRIu64, bd->readed);
    return bd->readed;
}

lfs3_kiwibd_sio_t lfs3_kiwibd_progged(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_progged(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_progged -> %"PRIu64, bd->progged);
    return bd->progged;
}

lfs3_kiwibd_sio_t lfs3_kiwibd_erased(const struct lfs3_cfg *cfg) {
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_erased(%p)", (void*)cfg);
    lfs3_kiwibd_t *bd = cfg->context;
    LFS3_KIWIBD_TRACE("lfs3_kiwibd_erased -> %"PRIu64, bd->erased);
    return bd->erased;
}

