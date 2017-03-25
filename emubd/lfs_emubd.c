/*
 * Block device emulated on standard files
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#include "emubd/lfs_emubd.h"
#include "emubd/lfs_cfg.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>


// Block device emulated on existing filesystem
int lfs_emubd_create(lfs_emubd_t *emu, const char *path) {
    memset(&emu->info, 0, sizeof(emu->info));
    memset(&emu->stats, 0, sizeof(emu->stats));

    // Allocate buffer for creating children files
    size_t pathlen = strlen(path);
    emu->path = malloc(pathlen + 1 + LFS_NAME_MAX + 1);
    if (!emu->path) {
        return -ENOMEM;
    }

    strcpy(emu->path, path);
    emu->path[pathlen] = '/';
    emu->path[pathlen + 1 + LFS_NAME_MAX] = '\0';
    emu->child = &emu->path[pathlen+1];
    strncpy(emu->child, "config", LFS_NAME_MAX);

    // Load config, erroring if it doesn't exist
    lfs_cfg_t cfg;
    int err = lfs_cfg_create(&cfg, emu->path);
    if (err) {
        return err;
    }

    emu->info.read_size  = lfs_cfg_getu(&cfg, "read_size",  0);
    emu->info.prog_size  = lfs_cfg_getu(&cfg, "prog_size",  0);
    emu->info.erase_size = lfs_cfg_getu(&cfg, "erase_size", 0);
    emu->info.total_size = lfs_cfg_getu(&cfg, "total_size", 0);

    lfs_cfg_destroy(&cfg);

    return 0;
}

void lfs_emubd_destroy(lfs_emubd_t *emu) {
    free(emu->path);
}

int lfs_emubd_read(lfs_emubd_t *emu, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, void *buffer) {
    uint8_t *data = buffer;

    // Check if read is valid
    if (!(off % emu->info.read_size == 0 &&
         size % emu->info.read_size == 0 &&
         ((uint64_t)block*emu->info.erase_size + off + size
          < emu->info.total_size))) {
        return -EINVAL;
    }

    // Zero out buffer for debugging
    memset(data, 0, size);

    // Iterate over blocks until enough data is read
    while (size > 0) {
        snprintf(emu->child, LFS_NAME_MAX, "%d", block);
        size_t count = lfs_min(emu->info.erase_size - off, size);

        FILE *f = fopen(emu->path, "rb");
        if (!f && errno != ENOENT) {
            return -errno;
        }

        if (f) {
            int err = fseek(f, off, SEEK_SET);
            if (err) {
                return -errno;
            }

            size_t res = fread(data, 1, count, f);
            if (res < count && !feof(f)) {
                return -errno;
            }

            err = fclose(f);
            if (err) {
                return -errno;
            }
        }

        size -= count;
        data += count;
        block += 1;
        off = 0;
    }

    emu->stats.read_count += 1;
    return 0;
}

int lfs_emubd_prog(lfs_emubd_t *emu, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer) {
    const uint8_t *data = buffer;

    // Check if write is valid
    if (!(off % emu->info.prog_size == 0 &&
         size % emu->info.prog_size == 0 &&
         ((uint64_t)block*emu->info.erase_size + off + size
          < emu->info.total_size))) {
        return -EINVAL;
    }

    // Iterate over blocks until enough data is read
    while (size > 0) {
        snprintf(emu->child, LFS_NAME_MAX, "%d", block);
        size_t count = lfs_min(emu->info.erase_size - off, size);

        FILE *f = fopen(emu->path, "r+b");
        if (!f && errno == ENOENT) {
            f = fopen(emu->path, "w+b");
            if (!f) {
                return -errno;
            }
        }

        int err = fseek(f, off, SEEK_SET);
        if (err) {
            return -errno;
        }

        size_t res = fwrite(data, 1, count, f);
        if (res < count) {
            return -errno;
        }

        err = fclose(f);
        if (err) {
            return -errno;
        }

        size -= count;
        data += count;
        block += 1;
        off = 0;
    }

    emu->stats.prog_count += 1;
    return 0;
}

int lfs_emubd_erase(lfs_emubd_t *emu, lfs_block_t block,
        lfs_off_t off, lfs_size_t size) {

    // Check if erase is valid
    if (!(off % emu->info.erase_size == 0 &&
         size % emu->info.erase_size == 0 &&
         ((uint64_t)block*emu->info.erase_size + off + size
          < emu->info.total_size))) {
        return -EINVAL;
    }

    // Iterate and erase blocks
    while (size > 0) {
        snprintf(emu->child, LFS_NAME_MAX, "%d", block);
        struct stat st;
        int err = stat(emu->path, &st);
        if (err && errno != ENOENT) {
            return -errno;
        }

        if (!err && S_ISREG(st.st_mode)) {
            int err = unlink(emu->path);
            if (err) {
                return -errno;
            }
        }

        size -= emu->info.erase_size;
        block += 1;
        off = 0;
    }

    emu->stats.erase_count += 1;
    return 0;
}

int lfs_emubd_sync(lfs_emubd_t *emu) {
    // Always in sync
    return 0;
}

int lfs_emubd_info(lfs_emubd_t *emu, struct lfs_bd_info *info) {
    *info = emu->info;
    return 0;
}

int lfs_emubd_stats(lfs_emubd_t *emu, struct lfs_bd_stats *stats) {
    *stats = emu->stats;
    return 0;
}


// Wrappers for void*s
static int lfs_emubd_bd_read(void *bd, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, void *buffer) {
    return lfs_emubd_read((lfs_emubd_t*)bd, block, off, size, buffer);
}

static int lfs_emubd_bd_prog(void *bd, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer) {
    return lfs_emubd_prog((lfs_emubd_t*)bd, block, off, size, buffer);
}

static int lfs_emubd_bd_erase(void *bd, lfs_block_t block,
        lfs_off_t off, lfs_size_t size) {
    return lfs_emubd_erase((lfs_emubd_t*)bd, block, off, size);
}

static int lfs_emubd_bd_sync(void *bd) {
    return lfs_emubd_sync((lfs_emubd_t*)bd);
}

static int lfs_emubd_bd_info(void *bd, struct lfs_bd_info *info) {
    return lfs_emubd_info((lfs_emubd_t*)bd, info);
}

const struct lfs_bd_ops lfs_emubd_ops = {
    .read = lfs_emubd_bd_read,
    .prog = lfs_emubd_bd_prog,
    .erase = lfs_emubd_bd_erase,
    .sync = lfs_emubd_bd_sync,
    .info = lfs_emubd_bd_info,
};

