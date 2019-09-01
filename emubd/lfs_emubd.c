/*
 * Block device emulated on standard files
 *
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "emubd/lfs_emubd.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <stdbool.h>
#include <inttypes.h>


// Emulated block device utils
static inline void lfs_emubd_tole32(lfs_emubd_t *emu) {
    emu->cfg.read_size     = lfs_tole32(emu->cfg.read_size);
    emu->cfg.prog_size     = lfs_tole32(emu->cfg.prog_size);
    emu->cfg.block_size    = lfs_tole32(emu->cfg.block_size);
    emu->cfg.block_count   = lfs_tole32(emu->cfg.block_count);

    emu->stats.read_count  = lfs_tole32(emu->stats.read_count);
    emu->stats.prog_count  = lfs_tole32(emu->stats.prog_count);
    emu->stats.erase_count = lfs_tole32(emu->stats.erase_count);

    for (unsigned i = 0; i < sizeof(emu->history.blocks) /
            sizeof(emu->history.blocks[0]); i++) {
        emu->history.blocks[i] = lfs_tole32(emu->history.blocks[i]);
    }
}

static inline void lfs_emubd_fromle32(lfs_emubd_t *emu) {
    emu->cfg.read_size     = lfs_fromle32(emu->cfg.read_size);
    emu->cfg.prog_size     = lfs_fromle32(emu->cfg.prog_size);
    emu->cfg.block_size    = lfs_fromle32(emu->cfg.block_size);
    emu->cfg.block_count   = lfs_fromle32(emu->cfg.block_count);

    emu->stats.read_count  = lfs_fromle32(emu->stats.read_count);
    emu->stats.prog_count  = lfs_fromle32(emu->stats.prog_count);
    emu->stats.erase_count = lfs_fromle32(emu->stats.erase_count);

    for (unsigned i = 0; i < sizeof(emu->history.blocks) /
            sizeof(emu->history.blocks[0]); i++) {
        emu->history.blocks[i] = lfs_fromle32(emu->history.blocks[i]);
    }
}


// Block device emulated on existing filesystem
int lfs_emubd_create(const struct lfs_config *cfg, const char *path) {
    LFS_TRACE("lfs_emubd_create(%p {.context=%p, "
                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
                ".block_size=%"PRIu32", .block_count=%"PRIu32"}, \"%s\")",
            (void*)cfg, cfg->context,
            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
            path);
    lfs_emubd_t *emu = cfg->context;
    emu->cfg.read_size   = cfg->read_size;
    emu->cfg.prog_size   = cfg->prog_size;
    emu->cfg.block_size  = cfg->block_size;
    emu->cfg.block_count = cfg->block_count;

    // Allocate buffer for creating children files
    size_t pathlen = strlen(path);
    emu->path = malloc(pathlen + 1 + LFS_NAME_MAX + 1);
    if (!emu->path) {
        int err = -ENOMEM;
        LFS_TRACE("lfs_emubd_create -> %"PRId32, err);
        return err;
    }

    strcpy(emu->path, path);
    emu->path[pathlen] = '/';
    emu->child = &emu->path[pathlen+1];
    memset(emu->child, '\0', LFS_NAME_MAX+1);

    // Create directory if it doesn't exist
    int err = mkdir(path, 0777);
    if (err && errno != EEXIST) {
        err = -errno;
        LFS_TRACE("lfs_emubd_create -> %"PRId32, err);
        return err;
    }

    // Load stats to continue incrementing
    snprintf(emu->child, LFS_NAME_MAX, ".stats");
    FILE *f = fopen(emu->path, "r");
    if (!f) {
        memset(&emu->stats, LFS_EMUBD_ERASE_VALUE, sizeof(emu->stats));
    } else {
        size_t res = fread(&emu->stats, sizeof(emu->stats), 1, f);
        lfs_emubd_fromle32(emu);
        if (res < 1) {
            err = -errno;
            LFS_TRACE("lfs_emubd_create -> %"PRId32, err);
            fclose(f);
            return err;
        }

        err = fclose(f);
        if (err) {
            err = -errno;
            LFS_TRACE("lfs_emubd_create -> %"PRId32, err);
            return err;
        }
    }

    // Load history
    snprintf(emu->child, LFS_NAME_MAX, ".history");
    f = fopen(emu->path, "r");
    if (!f) {
        memset(&emu->history, 0, sizeof(emu->history));
    } else {
        size_t res = fread(&emu->history, sizeof(emu->history), 1, f);
        lfs_emubd_fromle32(emu);
        if (res < 1) {
            err = -errno;
            LFS_TRACE("lfs_emubd_create -> %"PRId32, err);
            fclose(f);
            return err;
        }

        err = fclose(f);
        if (err) {
            err = -errno;
            LFS_TRACE("lfs_emubd_create -> %"PRId32, err);
            return err;
        }
    }

    LFS_TRACE("lfs_emubd_create -> %"PRId32, 0);
    return 0;
}

void lfs_emubd_destroy(const struct lfs_config *cfg) {
    LFS_TRACE("lfs_emubd_destroy(%p)", (void*)cfg);
    lfs_emubd_sync(cfg);

    lfs_emubd_t *emu = cfg->context;
    free(emu->path);
    LFS_TRACE("lfs_emubd_destroy -> %s", "void");
}

int lfs_emubd_read(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    LFS_TRACE("lfs_emubd_read(%p, 0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_emubd_t *emu = cfg->context;
    uint8_t *data = buffer;

    // Check if read is valid
    assert(off  % cfg->read_size == 0);
    assert(size % cfg->read_size == 0);
    assert(block < cfg->block_count);

    // Zero out buffer for debugging
    memset(data, 0, size);

    // Read data
    snprintf(emu->child, LFS_NAME_MAX, "%" PRIx32, block);

    FILE *f = fopen(emu->path, "rb");
    if (!f && errno != ENOENT) {
        int err = -errno;
        LFS_TRACE("lfs_emubd_read -> %d", err);
        return err;
    }

    if (f) {
        int err = fseek(f, off, SEEK_SET);
        if (err) {
            err = -errno;
            LFS_TRACE("lfs_emubd_read -> %d", err);
            fclose(f);
            return err;
        }

        size_t res = fread(data, 1, size, f);
        if (res < size && !feof(f)) {
            err = -errno;
            LFS_TRACE("lfs_emubd_read -> %d", err);
            fclose(f);
            return err;
        }

        err = fclose(f);
        if (err) {
            err = -errno;
            LFS_TRACE("lfs_emubd_read -> %d", err);
            return err;
        }
    }

    emu->stats.read_count += size;
    LFS_TRACE("lfs_emubd_read -> %d", 0);
    return 0;
}

int lfs_emubd_prog(const struct lfs_config *cfg, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    LFS_TRACE("lfs_emubd_prog(%p, 0x%"PRIx32", %"PRIu32", %p, %"PRIu32")",
            (void*)cfg, block, off, buffer, size);
    lfs_emubd_t *emu = cfg->context;
    const uint8_t *data = buffer;

    // Check if write is valid
    assert(off  % cfg->prog_size == 0);
    assert(size % cfg->prog_size == 0);
    assert(block < cfg->block_count);

    // Program data
    snprintf(emu->child, LFS_NAME_MAX, "%" PRIx32, block);

    FILE *f = fopen(emu->path, "r+b");
    if (!f) {
        int err = (errno == EACCES) ? 0 : -errno;
        LFS_TRACE("lfs_emubd_prog -> %d", err);
        return err;
    }

    // Check that file was erased
    assert(f);

    int err = fseek(f, off, SEEK_SET);
    if (err) {
        err = -errno;
        LFS_TRACE("lfs_emubd_prog -> %d", err);
        fclose(f);
        return err;
    }

    size_t res = fwrite(data, 1, size, f);
    if (res < size) {
        err = -errno;
        LFS_TRACE("lfs_emubd_prog -> %d", err);
        fclose(f);
        return err;
    }

    err = fseek(f, off, SEEK_SET);
    if (err) {
        err = -errno;
        LFS_TRACE("lfs_emubd_prog -> %d", err);
        fclose(f);
        return err;
    }

    uint8_t dat;
    res = fread(&dat, 1, 1, f);
    if (res < 1) {
        err = -errno;
        LFS_TRACE("lfs_emubd_prog -> %d", err);
        fclose(f);
        return err;
    }

    err = fclose(f);
    if (err) {
        err = -errno;
        LFS_TRACE("lfs_emubd_prog -> %d", err);
        return err;
    }

    // update history and stats
    if (block != emu->history.blocks[0]) {
        memmove(&emu->history.blocks[1], &emu->history.blocks[0],
                sizeof(emu->history) - sizeof(emu->history.blocks[0]));
        emu->history.blocks[0] = block;
    }

    emu->stats.prog_count += size;
    LFS_TRACE("lfs_emubd_prog -> %d", 0);
    return 0;
}

int lfs_emubd_erase(const struct lfs_config *cfg, lfs_block_t block) {
    LFS_TRACE("lfs_emubd_erase(%p, 0x%"PRIx32")", (void*)cfg, block);
    lfs_emubd_t *emu = cfg->context;

    // Check if erase is valid
    assert(block < cfg->block_count);

    // Erase the block
    snprintf(emu->child, LFS_NAME_MAX, "%" PRIx32, block);
    struct stat st;
    int err = stat(emu->path, &st);
    if (err && errno != ENOENT) {
        err = -errno;
        LFS_TRACE("lfs_emubd_erase -> %d", err);
        return err;
    }

    if (!err && S_ISREG(st.st_mode) && (S_IWUSR & st.st_mode)) {
        err = unlink(emu->path);
        if (err) {
            err = -errno;
            LFS_TRACE("lfs_emubd_erase -> %d", err);
            return err;
        }
    }

    if (err || (S_ISREG(st.st_mode) && (S_IWUSR & st.st_mode))) {
        FILE *f = fopen(emu->path, "w");
        if (!f) {
            err = -errno;
            LFS_TRACE("lfs_emubd_erase -> %d", err);
            return err;
        }

        err = fclose(f);
        if (err) {
            err = -errno;
            LFS_TRACE("lfs_emubd_erase -> %d", err);
            return err;
        }
    }

    emu->stats.erase_count += cfg->block_size;
    LFS_TRACE("lfs_emubd_erase -> %d", 0);
    return 0;
}

int lfs_emubd_sync(const struct lfs_config *cfg) {
    LFS_TRACE("lfs_emubd_sync(%p)", (void*)cfg);
    lfs_emubd_t *emu = cfg->context;

    // Just write out info/stats for later lookup
    snprintf(emu->child, LFS_NAME_MAX, ".config");
    FILE *f = fopen(emu->path, "w");
    if (!f) {
        int err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        return err;
    }

    lfs_emubd_tole32(emu);
    size_t res = fwrite(&emu->cfg, sizeof(emu->cfg), 1, f);
    lfs_emubd_fromle32(emu);
    if (res < 1) {
        int err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        fclose(f);
        return err;
    }

    int err = fclose(f);
    if (err) {
        err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        return err;
    }

    snprintf(emu->child, LFS_NAME_MAX, ".stats");
    f = fopen(emu->path, "w");
    if (!f) {
        err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        return err;
    }

    lfs_emubd_tole32(emu);
    res = fwrite(&emu->stats, sizeof(emu->stats), 1, f);
    lfs_emubd_fromle32(emu);
    if (res < 1) {
        err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        fclose(f);
        return err;
    }

    err = fclose(f);
    if (err) {
        err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        return err;
    }

    snprintf(emu->child, LFS_NAME_MAX, ".history");
    f = fopen(emu->path, "w");
    if (!f) {
        err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        return err;
    }

    lfs_emubd_tole32(emu);
    res = fwrite(&emu->history, sizeof(emu->history), 1, f);
    lfs_emubd_fromle32(emu);
    if (res < 1) {
        err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        fclose(f);
        return err;
    }

    err = fclose(f);
    if (err) {
        err = -errno;
        LFS_TRACE("lfs_emubd_sync -> %d", err);
        return err;
    }

    LFS_TRACE("lfs_emubd_sync -> %d", 0);
    return 0;
}
