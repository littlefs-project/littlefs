/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#include "lfs.h"
#include "lfs_util.h"

#include <string.h>
#include <stdlib.h>
#include <assert.h>


/// Caching block device operations ///
static int lfs_cache_read(lfs_t *lfs, lfs_cache_t *rcache,
        const lfs_cache_t *pcache, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    assert(block < lfs->cfg->block_count);

    while (size > 0) {
        if (pcache && block == pcache->block && off >= pcache->off &&
                off < pcache->off + lfs->cfg->prog_size) {
            // is already in pcache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->prog_size - (off-pcache->off));
            memcpy(data, &pcache->buffer[off-pcache->off], diff);

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        if (block == rcache->block && off >= rcache->off &&
                off < rcache->off + lfs->cfg->read_size) {
            // is already in rcache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->read_size - (off-rcache->off));
            memcpy(data, &rcache->buffer[off-rcache->off], diff);

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        if (off % lfs->cfg->read_size == 0 && size >= lfs->cfg->read_size) {
            // bypass cache?
            lfs_size_t diff = size - (size % lfs->cfg->read_size);
            int err = lfs->cfg->read(lfs->cfg, block, off, data, diff);
            if (err) {
                return err;
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // load to cache, first condition can no longer fail
        rcache->block = block;
        rcache->off = off - (off % lfs->cfg->read_size);
        int err = lfs->cfg->read(lfs->cfg, rcache->block,
                rcache->off, rcache->buffer, lfs->cfg->read_size);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_cache_flush(lfs_t *lfs, lfs_cache_t *cache) {
    if (cache->block != 0xffffffff) {
        int err = lfs->cfg->prog(lfs->cfg, cache->block,
                cache->off, cache->buffer, lfs->cfg->prog_size);
        if (err) {
            return err;
        }

        cache->block = 0xffffffff;
    }

    return 0;
}

static int lfs_cache_prog(lfs_t *lfs, lfs_cache_t *cache, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    assert(block < lfs->cfg->block_count);

    while (size > 0) {
        if (block == cache->block && off >= cache->off &&
                off < cache->off + lfs->cfg->prog_size) {
            // is already in cache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->prog_size - (off-cache->off));
            memcpy(&cache->buffer[off-cache->off], data, diff);

            data += diff;
            off += diff;
            size -= diff;

            if (off % lfs->cfg->prog_size == 0) {
                // eagerly flush out cache if we fill up
                int err = lfs_cache_flush(lfs, cache);
                if (err) {
                    return err;
                }
            }

            continue;
        }

        // cache must have been flushed, either by programming and
        // entire block or manually flushing the cache
        assert(cache->block == 0xffffffff);

        if (off % lfs->cfg->prog_size == 0 &&
                size >= lfs->cfg->prog_size) {
            // bypass cache?
            lfs_size_t diff = size - (size % lfs->cfg->prog_size);
            int err = lfs->cfg->prog(lfs->cfg, block, off, data, diff);
            if (err) {
                return err;
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // prepare cache, first condition can no longer fail
        cache->block = block;
        cache->off = off - (off % lfs->cfg->prog_size);
    }

    return 0;
}


/// General lfs block device operations ///
static int lfs_read(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    // if we ever do more than writes to alternating pairs,
    // this may need to consider pcache
    return lfs_cache_read(lfs, &lfs->rcache, NULL,
            block, off, buffer, size);
}

static int lfs_prog(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    return lfs_cache_prog(lfs, &lfs->pcache,
            block, off, buffer, size);
}

static int lfs_erase(lfs_t *lfs, lfs_block_t block) {
    return lfs->cfg->erase(lfs->cfg, block);
}

static int lfs_sync(lfs_t *lfs) {
    int err = lfs_cache_flush(lfs, &lfs->pcache);
    if (err) {
        return err;
    }

    return lfs->cfg->sync(lfs->cfg);
}

static int lfs_cmp(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer) {
    const uint8_t *data = buffer;

    for (lfs_off_t i = 0; i < size; i++) {
        uint8_t c;
        int err = lfs_read(lfs, block, off+i, &c, 1);
        if (err) {
            return err;
        }

        if (c != data[i]) {
            return false;
        }
    }

    return true;
}


/// Block allocator ///
static int lfs_alloc_lookahead(void *p, lfs_block_t block) {
    lfs_t *lfs = p;

    lfs_block_t off = (block - lfs->free.start) % lfs->cfg->block_count;
    if (off < lfs->cfg->lookahead) {
        lfs->free.lookahead[off / 32] |= 1U << (off % 32);
    }

    return 0;
}

static int lfs_alloc_scan(lfs_t *lfs, lfs_block_t *block) {
    lfs_block_t end = lfs->free.start + lfs->cfg->block_count;

    while (true) {
        while (lfs->free.off < lfs->cfg->lookahead) {
            lfs_block_t off = lfs->free.off;
            lfs->free.off += 1;

            if (!(lfs->free.lookahead[off / 32] & (1U << (off % 32)))) {
                // found a free block
                *block = (lfs->free.start + off) % lfs->cfg->block_count;
                return 0;
            }
        }

        // could not find block
        lfs->free.start += lfs->cfg->lookahead;
        lfs->free.off = 0;
        if (lfs_scmp(lfs->free.start, end) > 0) {
            return LFS_ERR_NOSPC;
        }

        // find mask of free blocks from tree
        memset(lfs->free.lookahead, 0, lfs->cfg->lookahead/8);
        int err = lfs_traverse(lfs, lfs_alloc_lookahead, lfs);
        if (err) {
            return err;
        }
    }
}

static int lfs_alloc(lfs_t *lfs, lfs_block_t *block) {
    // try to scan for free block
    int err = lfs_alloc_scan(lfs, block);
    if (err != LFS_ERR_NOSPC) {
        return err;
    }

    // still can't allocate a block? check for orphans
    err = lfs_deorphan(lfs);
    if (err) {
        return err;
    }

    // scan again or die trying
    err = lfs_alloc_scan(lfs, block);
    if (err) {
        LFS_WARN("No more free space%s", "");
        return err;
    }

    return 0;
}


/// Metadata pair and directory operations ///
static inline void lfs_pairswap(lfs_block_t pair[2]) {
    lfs_block_t t = pair[0];
    pair[0] = pair[1];
    pair[1] = t;
}

static inline bool lfs_pairisnull(const lfs_block_t pair[2]) {
    return pair[0] == 0xffffffff || pair[1] == 0xffffffff;
}

static inline int lfs_paircmp(
        const lfs_block_t paira[2],
        const lfs_block_t pairb[2]) {
    return !(paira[0] == pairb[0] || paira[1] == pairb[1] ||
             paira[0] == pairb[1] || paira[1] == pairb[0]);
}

static int lfs_dir_alloc(lfs_t *lfs, lfs_dir_t *dir) {
    // allocate pair of dir blocks
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[i]);
        if (err) {
            return err;
        }
    }

    // we couldn't find unique blocks, we're out of space
    if (dir->pair[0] == dir->pair[1]) {
        return LFS_ERR_NOSPC;
    }

    // rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs_read(lfs, dir->pair[0], 0, &dir->d.rev, 4);
    if (err) {
        return err;
    }

    // set defaults
    dir->d.rev += 1;
    dir->d.size = sizeof(dir->d);
    dir->d.tail[0] = -1;
    dir->d.tail[1] = -1;
    dir->off = sizeof(dir->d);

    // don't write out yet, let caller take care of that
    return 0;
}

static int lfs_dir_fetch(lfs_t *lfs,
        lfs_dir_t *dir, const lfs_block_t pair[2]) {
    // copy out pair, otherwise may be aliasing dir
    const lfs_block_t tpair[2] = {pair[0], pair[1]};
    bool valid = false;

    // check both blocks for the most recent revision
    for (int i = 0; i < 2; i++) {
        struct lfs_disk_dir test;
        int err = lfs_read(lfs, tpair[i], 0, &test, sizeof(test));
        if (err) {
            return err;
        }

        if (valid && lfs_scmp(test.rev, dir->d.rev) < 0) {
            continue;
        }

        uint32_t crc = 0xffffffff;
        crc = lfs_crc(crc, &test, sizeof(test));

        for (lfs_off_t j = sizeof(test); j < lfs->cfg->block_size; j += 4) {
            uint32_t word;
            int err = lfs_read(lfs, tpair[i], j, &word, 4);
            if (err) {
                return err;
            }

            crc = lfs_crc(crc, &word, 4);
        }

        if (crc != 0) {
            continue;
        }

        valid = true;

        // setup dir in case it's valid
        dir->pair[0] = tpair[(i+0) % 2];
        dir->pair[1] = tpair[(i+1) % 2];
        dir->off = sizeof(dir->d);
        dir->d = test;
    }

    if (!valid) {
        LFS_ERROR("Corrupted dir pair at %d %d", tpair[0], tpair[1]);
        return LFS_ERR_CORRUPT;
    }

    return 0;
}

static int lfs_dir_commit(lfs_t *lfs, lfs_dir_t *dir,
        const lfs_entry_t *entry, const void *data) {
    dir->d.rev += 1;
    lfs_pairswap(dir->pair);

    int err = lfs_erase(lfs, dir->pair[0]);
    if (err) {
        return err;
    }

    uint32_t crc = 0xffffffff;
    crc = lfs_crc(crc, &dir->d, sizeof(dir->d));
    err = lfs_prog(lfs, dir->pair[0], 0, &dir->d, sizeof(dir->d));
    if (err) {
        return err;
    }

    lfs_off_t off = sizeof(dir->d);
    lfs_size_t size = 0x7fffffff & dir->d.size;
    while (off < size) {
        if (entry && off == entry->off) {
            crc = lfs_crc(crc, &entry->d, sizeof(entry->d));
            int err = lfs_prog(lfs, dir->pair[0],
                    off, &entry->d, sizeof(entry->d));
            if (err) {
                return err;
            }
            off += sizeof(entry->d);

            if (data) {
                crc = lfs_crc(crc, data, entry->d.len - sizeof(entry->d));
                int err = lfs_prog(lfs, dir->pair[0],
                        off, data, entry->d.len - sizeof(entry->d));
                if (err) {
                    return err;
                }
                off += entry->d.len - sizeof(entry->d);
            }
        } else {
            uint8_t data;
            int err = lfs_read(lfs, dir->pair[1], off, &data, 1);
            if (err) {
                return err;
            }

            crc = lfs_crc(crc, &data, 1);
            err = lfs_prog(lfs, dir->pair[0], off, &data, 1);
            if (err) {
                return err;
            }

            off += 1;
        }
    }

    while (off < lfs->cfg->block_size-4) {
        uint8_t data = 0xff;
        crc = lfs_crc(crc, &data, 1);
        err = lfs_prog(lfs, dir->pair[0], off, &data, 1);
        if (err) {
            return err;
        }

        off += 1;
    }

    err = lfs_prog(lfs, dir->pair[0], lfs->cfg->block_size-4, &crc, 4);
    if (err) {
        return err;
    }

    return lfs_sync(lfs);
}

static int lfs_dir_shift(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry) {
    dir->d.rev += 1;
    dir->d.size -= entry->d.len;
    lfs_pairswap(dir->pair);

    int err = lfs_erase(lfs, dir->pair[0]);
    if (err) {
        return err;
    }

    uint32_t crc = 0xffffffff;
    crc = lfs_crc(crc, &dir->d, sizeof(dir->d));
    err = lfs_prog(lfs, dir->pair[0], 0, &dir->d, sizeof(dir->d));
    if (err) {
        return err;
    }

    lfs_off_t woff = sizeof(dir->d);
    lfs_off_t roff = sizeof(dir->d);
    lfs_size_t size = 0x7fffffff & dir->d.size;
    while (woff < size) {
        if (roff == entry->off) {
            roff += entry->d.len;
        } else {
            uint8_t data;
            int err = lfs_read(lfs, dir->pair[1], roff, &data, 1);
            if (err) {
                return err;
            }

            crc = lfs_crc(crc, &data, 1);
            err = lfs_prog(lfs, dir->pair[0], woff, &data, 1);
            if (err) {
                return err;
            }

            woff += 1;
            roff += 1;
        }
    }

    while (woff < lfs->cfg->block_size-4) {
        uint8_t data = 0xff;
        crc = lfs_crc(crc, &data, 1);
        err = lfs_prog(lfs, dir->pair[0], woff, &data, 1);
        if (err) {
            return err;
        }


        woff += 1;
    }

    err = lfs_prog(lfs, dir->pair[0], lfs->cfg->block_size-4, &crc, 4);
    if (err) {
        return err;
    }

    return lfs_sync(lfs);
}

static int lfs_dir_append(lfs_t *lfs, lfs_dir_t *dir,
        lfs_entry_t *entry, const void *data) {
    // check if we fit, if top bit is set we do not and move on
    while (true) {
        if (dir->d.size + entry->d.len <= lfs->cfg->block_size - 4) {
            entry->off = dir->d.size;
            dir->d.size += entry->d.len;
            return lfs_dir_commit(lfs, dir, entry, data);
        }

        // we need to allocate a new dir block
        if (!(0x80000000 & dir->d.size)) {
            lfs_dir_t newdir;
            int err = lfs_dir_alloc(lfs, &newdir);
            if (err) {
                return err;
            }

            // our allocator doesn't track blocks before being appended,
            // so even if we found some blocks, they may not be unique
            if (entry->d.type == LFS_TYPE_DIR &&
                    lfs_paircmp(entry->d.u.dir, newdir.pair) == 0) {
                return LFS_ERR_NOSPC;
            }

            newdir.d.tail[0] = dir->d.tail[0];
            newdir.d.tail[1] = dir->d.tail[1];
            entry->off = newdir.d.size;
            newdir.d.size += entry->d.len;
            err = lfs_dir_commit(lfs, &newdir, entry, data);
            if (err) {
                return err;
            }

            dir->d.size |= 0x80000000;
            dir->d.tail[0] = newdir.pair[0];
            dir->d.tail[1] = newdir.pair[1];
            return lfs_dir_commit(lfs, dir, NULL, NULL);
        }

        int err = lfs_dir_fetch(lfs, dir, dir->d.tail);
        if (err) {
            return err;
        }
    }
}

static int lfs_dir_remove(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry) {
    // either shift out the one entry or remove the whole dir block
    if (dir->d.size == sizeof(dir->d)) {
        lfs_dir_t pdir;
        int err = lfs_dir_fetch(lfs, &pdir, lfs->root);
        if (err) {
            return err;
        }

        while (lfs_paircmp(pdir.d.tail, dir->pair) != 0) {
            int err = lfs_dir_fetch(lfs, &pdir, pdir.d.tail);
            if (err) {
                return err;
            }
        }

        if (!(pdir.d.size & 0x80000000)) {
            return lfs_dir_shift(lfs, dir, entry);
        } else {
            pdir.d.tail[0] = dir->d.tail[0];
            pdir.d.tail[1] = dir->d.tail[1];
            return lfs_dir_commit(lfs, &pdir, NULL, NULL);
        }
    } else {
        return lfs_dir_shift(lfs, dir, entry);
    }
}

static int lfs_dir_next(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry) {
    while (true) {
        if (dir->off + sizeof(entry->d) > (0x7fffffff & dir->d.size)) {
            if (!(0x80000000 & dir->d.size)) {
                entry->off = dir->off;
                return LFS_ERR_NOENT;
            }

            int err = lfs_dir_fetch(lfs, dir, dir->d.tail);
            if (err) {
                return err;
            }

            dir->off = sizeof(dir->d);
            dir->pos += sizeof(dir->d);
            continue;
        }

        int err = lfs_read(lfs, dir->pair[0], dir->off,
                &entry->d, sizeof(entry->d));
        if (err) {
            return err;
        }

        dir->off += entry->d.len;
        dir->pos += entry->d.len;
        if ((0xff & entry->d.type) == LFS_TYPE_REG ||
            (0xff & entry->d.type) == LFS_TYPE_DIR) {
            entry->off = dir->off - entry->d.len;
            return 0;
        }
    }
}

static int lfs_dir_find(lfs_t *lfs, lfs_dir_t *dir,
        lfs_entry_t *entry, const char **path) {
    const char *pathname = *path;
    size_t pathlen;

    while (true) {
    nextname:
        // skip slashes
        pathname += strspn(pathname, "/");
        pathlen = strcspn(pathname, "/");

        // skip '.' and root '..'
        if ((pathlen == 1 && memcmp(pathname, ".", 1) == 0) ||
            (pathlen == 2 && memcmp(pathname, "..", 2) == 0)) {
            pathname += pathlen;
            goto nextname;
        }

        // skip if matched by '..' in name
        const char *suffix = pathname + pathlen;
        size_t sufflen;
        int depth = 1;
        while (true) {
            suffix += strspn(suffix, "/");
            sufflen = strcspn(suffix, "/");
            if (sufflen == 0) {
                break;
            }

            if (sufflen == 2 && memcmp(suffix, "..", 2) == 0) {
                depth -= 1;
                if (depth == 0) {
                    pathname = suffix + sufflen;
                    goto nextname;
                }
            } else {
                depth += 1;
            }

            suffix += sufflen;
        }

        // find path
        while (true) {
            int err = lfs_dir_next(lfs, dir, entry);
            if (err) {
                return err;
            }

            if (entry->d.len - sizeof(entry->d) != pathlen) {
                continue;
            }

            int ret = lfs_cmp(lfs, dir->pair[0],
                    entry->off + sizeof(entry->d), pathlen, pathname);
            if (ret < 0) {
                return ret;
            }

            // Found match
            if (ret == true) {
                break;
            }
        }

        pathname += pathlen;
        pathname += strspn(pathname, "/");
        if (pathname[0] == '\0') {
            return 0;
        }

        // continue on if we hit a directory
        if (entry->d.type != LFS_TYPE_DIR) {
            return LFS_ERR_NOTDIR;
        }

        int err = lfs_dir_fetch(lfs, dir, entry->d.u.dir);
        if (err) {
            return err;
        }

        *path = pathname;
    }

    return 0;
}


/// Top level directory operations ///
int lfs_mkdir(lfs_t *lfs, const char *path) {
    // fetch parent directory
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    lfs_entry_t entry;
    err = lfs_dir_find(lfs, &cwd, &entry, &path);
    if (err != LFS_ERR_NOENT) {
        return err ? err : LFS_ERR_EXISTS;
    }

    // Build up new directory
    lfs_dir_t dir;
    err = lfs_dir_alloc(lfs, &dir);
    if (err) {
        return err;
    }
    dir.d.tail[0] = cwd.d.tail[0];
    dir.d.tail[1] = cwd.d.tail[1];

    err = lfs_dir_commit(lfs, &dir, NULL, NULL);
    if (err) {
        return err;
    }

    entry.d.type = LFS_TYPE_DIR;
    entry.d.len = sizeof(entry.d) + strlen(path);
    entry.d.u.dir[0] = dir.pair[0];
    entry.d.u.dir[1] = dir.pair[1];

    cwd.d.tail[0] = dir.pair[0];
    cwd.d.tail[1] = dir.pair[1];

    return lfs_dir_append(lfs, &cwd, &entry, path);
}

int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
    dir->pair[0] = lfs->root[0];
    dir->pair[1] = lfs->root[1];

    int err = lfs_dir_fetch(lfs, dir, dir->pair);
    if (err) {
        return err;
    }

    if (strspn(path, "/.") == strlen(path)) {
        // can only be something like '/././../.'
        dir->head[0] = dir->pair[0];
        dir->head[1] = dir->pair[1];
        dir->pos = sizeof(dir->d) - 2;
        dir->off = sizeof(dir->d);
        return 0;
    }

    lfs_entry_t entry;
    err = lfs_dir_find(lfs, dir, &entry, &path);
    if (err) {
        return err;
    } else if (entry.d.type != LFS_TYPE_DIR) {
        return LFS_ERR_NOTDIR;
    }

    err = lfs_dir_fetch(lfs, dir, entry.d.u.dir);
    if (err) {
        return err;
    }

    // setup head dir
    // special offset for '.' and '..'
    dir->head[0] = dir->pair[0];
    dir->head[1] = dir->pair[1];
    dir->pos = sizeof(dir->d) - 2;
    dir->off = sizeof(dir->d);
    return 0;
}

int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
    // Do nothing, dir is always synchronized
    return 0;
}

int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info) {
    memset(info, 0, sizeof(*info));

    // special offset for '.' and '..'
    if (dir->pos == sizeof(dir->d) - 2) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, ".");
        dir->pos += 1;
        return 1;
    } else if (dir->pos == sizeof(dir->d) - 1) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, "..");
        dir->pos += 1;
        return 1;
    }

    lfs_entry_t entry;
    int err = lfs_dir_next(lfs, dir, &entry);
    if (err) {
        return (err == LFS_ERR_NOENT) ? 0 : err;
    }

    info->type = entry.d.type & 0xff;
    if (info->type == LFS_TYPE_REG) {
        info->size = entry.d.u.file.size;
    }

    err = lfs_read(lfs, dir->pair[0], entry.off + sizeof(entry.d),
            info->name, entry.d.len - sizeof(entry.d));
    if (err) {
        return err;
    }

    return 1;
}

int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off) {
    // simply walk from head dir
    int err = lfs_dir_rewind(lfs, dir);
    if (err) {
        return err;
    }
    dir->pos = off;

    while (off > (0x7fffffff & dir->d.size)) {
        off -= 0x7fffffff & dir->d.size;
        if (!(0x80000000 & dir->d.size)) {
            return LFS_ERR_INVAL;
        }

        int err = lfs_dir_fetch(lfs, dir, dir->d.tail);
        if (err) {
            return err;
        }
    }

    dir->off = off;
    return 0;
}

lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir) {
    return dir->pos;
}

int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir) {
    // reload the head dir
    int err = lfs_dir_fetch(lfs, dir, dir->head);
    if (err) {
        return err;
    }

    dir->pair[0] = dir->head[0];
    dir->pair[1] = dir->head[1];
    dir->pos = sizeof(dir->d) - 2;
    dir->off = sizeof(dir->d);
    return 0;
}


/// File index list operations ///
static int lfs_index(lfs_t *lfs, lfs_off_t *off) {
    lfs_off_t i = 0;
    lfs_size_t words = lfs->cfg->block_size / 4;

    while (*off >= lfs->cfg->block_size) {
        i += 1;
        *off -= lfs->cfg->block_size;
        *off += 4*lfs_min(lfs_ctz(i)+1, words-1);
    }

    return i;
}

static int lfs_index_find(lfs_t *lfs,
        lfs_cache_t *rcache, const lfs_cache_t *pcache,
        lfs_block_t head, lfs_size_t size,
        lfs_size_t pos, lfs_block_t *block, lfs_off_t *off) {
    if (size == 0) {
        *block = -1;
        *off = 0;
        return 0;
    }

    lfs_off_t current = lfs_index(lfs, &(lfs_off_t){size-1});
    lfs_off_t target = lfs_index(lfs, &pos);
    lfs_size_t words = lfs->cfg->block_size / 4;

    while (current > target) {
        lfs_size_t skip = lfs_min(
                lfs_npw2(current-target+1) - 1,
                lfs_min(lfs_ctz(current)+1, words-1) - 1);

        int err = lfs_cache_read(lfs, rcache, pcache, head, 4*skip, &head, 4);
        if (err) {
            return err;
        }

        current -= 1 << skip;
    }

    *block = head;
    *off = pos;
    return 0;
}

static int lfs_index_extend(lfs_t *lfs,
        lfs_cache_t *rcache, lfs_cache_t *pcache,
        lfs_block_t head, lfs_size_t size,
        lfs_off_t *block, lfs_block_t *off) {
    // go ahead and grab a block
    int err = lfs_alloc(lfs, block);
    if (err) {
        return err;
    }

    err = lfs_erase(lfs, *block);
    if (err) {
        return err;
    }

    if (size == 0) {
        *off = 0;
        return 0;
    }

    size -= 1;
    lfs_off_t index = lfs_index(lfs, &size);
    size += 1;

    // just copy out the last block if it is incomplete
    if (size != lfs->cfg->block_size) {
        for (lfs_off_t i = 0; i < size; i++) {
            uint8_t data;
            int err = lfs_cache_read(lfs, rcache, NULL, head, i, &data, 1);
            if (err) {
                return err;
            }

            err = lfs_cache_prog(lfs, pcache, *block, i, &data, 1);
            if (err) {
                return err;
            }
        }

        *off = size;
        return 0;
    }

    // append block
    index += 1;
    lfs_size_t words = lfs->cfg->block_size / 4;
    lfs_size_t skips = lfs_min(lfs_ctz(index)+1, words-1);

    for (lfs_off_t i = 0; i < skips; i++) {
        int err = lfs_cache_prog(lfs, pcache, *block, 4*i, &head, 4);
        if (err) {
            return err;
        }

        if (i != skips-1) {
            err = lfs_cache_read(lfs, rcache, NULL, head, 4*i, &head, 4);
            if (err) {
                return err;
            }
        }
    }

    *off = 4*skips;
    return 0;
}

static int lfs_index_traverse(lfs_t *lfs,
        lfs_cache_t *rcache, const lfs_cache_t *pcache,
        lfs_block_t head, lfs_size_t size,
        int (*cb)(void*, lfs_block_t), void *data) {
    if (size == 0) {
        return 0;
    }

    lfs_off_t index = lfs_index(lfs, &(lfs_off_t){size-1});

    while (true) {
        int err = cb(data, head);
        if (err) {
            return err;
        }

        if (index == 0) {
            return 0;
        }

        err = lfs_cache_read(lfs, rcache, pcache, head, 0, &head, 4);
        if (err) {
            return err;
        }

        index -= 1;
    }

    return 0;
}


/// Top level file operations ///
int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    // Allocate entry for file if it doesn't exist
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    lfs_entry_t entry;
    err = lfs_dir_find(lfs, &cwd, &entry, &path);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    if (err == LFS_ERR_NOENT) {
        if (!(flags & LFS_O_CREAT)) {
            return LFS_ERR_NOENT;
        }

        // create entry to remember name
        entry.d.type = LFS_TYPE_REG;
        entry.d.len = sizeof(entry.d) + strlen(path);
        entry.d.u.file.head = -1;
        entry.d.u.file.size = 0;
        err = lfs_dir_append(lfs, &cwd, &entry, path);
        if (err) {
            return err;
        }
    } else if (entry.d.type == LFS_TYPE_DIR) {
        return LFS_ERR_ISDIR;
    } else if (flags & LFS_O_EXCL) {
        return LFS_ERR_EXISTS;
    }

    // setup file struct
    file->pair[0] = cwd.pair[0];
    file->pair[1] = cwd.pair[1];
    file->poff = entry.off;
    file->head = entry.d.u.file.head;
    file->size = entry.d.u.file.size;
    file->flags = flags;
    file->pos = 0;
    file->block = -1; // TODO rm me?

    if (flags & LFS_O_TRUNC) {
        file->head = -1;
        file->size = 0;
    }

    // allocate buffer if needed
    file->cache.block = 0xffffffff;
    if (lfs->cfg->file_buffer) {
        file->cache.buffer = lfs->cfg->file_buffer;
    } else if ((file->flags & 3) == LFS_O_RDONLY) {
        file->cache.buffer = malloc(lfs->cfg->read_size);
        if (!file->cache.buffer) {
            return LFS_ERR_NOMEM;
        }
    } else {
        file->cache.buffer = malloc(lfs->cfg->prog_size);
        if (!file->cache.buffer) {
            return LFS_ERR_NOMEM;
        }
    }

    // add to list of files
    file->next = lfs->files;
    lfs->files = file;

    return 0;
}

int lfs_file_close(lfs_t *lfs, lfs_file_t *file) {
    int err = lfs_file_sync(lfs, file);

    // remove from list of files
    for (lfs_file_t **p = &lfs->files; *p; p = &(*p)->next) {
        if (*p == file) {
            *p = file->next;
            break;
        }
    }

    // clean up memory
    if (!lfs->cfg->file_buffer) {
        free(file->cache.buffer);
    }

    return err;
}

static int lfs_file_flush(lfs_t *lfs, lfs_file_t *file) {
    if (file->flags & LFS_F_READING) {
        // just drop read cache
        file->cache.block = 0xffffffff;
        file->flags &= ~LFS_F_READING;
    }

    if (file->flags & LFS_F_WRITING) {
        lfs_off_t pos = file->pos;

        // copy over anything after current branch
        lfs_file_t orig = {
            .head = file->head,
            .size = file->size,
            .flags = LFS_O_RDONLY,
            .pos = file->pos,
            .cache = lfs->rcache,
        };
        lfs->rcache.block = 0xffffffff;

        while (file->pos < file->size) {
            // copy over a byte at a time, leave it up to caching
            // to make this efficient
            uint8_t data;
            lfs_ssize_t res = lfs_file_read(lfs, &orig, &data, 1);
            if (res < 0) {
                return res;
            }

            res = lfs_file_write(lfs, file, &data, 1);
            if (res < 0) {
                return res;
            }

            // keep our reference to the rcache in sync
            if (lfs->rcache.block != 0xffffffff) {
                orig.cache.block = 0xffffffff;
                lfs->rcache.block = 0xffffffff;
            }
        }

        // write out what we have
        int err = lfs_cache_flush(lfs, &file->cache);
        if (err) {
            return err;
        }

        // actual file updates
        file->head = file->block;
        file->size = file->pos;
        file->flags &= ~LFS_F_WRITING;
        file->flags |= LFS_F_DIRTY;

        file->pos = pos;
    }

    return 0;
}

int lfs_file_sync(lfs_t *lfs, lfs_file_t *file) {
    int err = lfs_file_flush(lfs, file);
    if (err) {
        return err;
    }

    if ((file->flags & LFS_F_DIRTY) && !lfs_pairisnull(file->pair)) {
        // update dir entry
        lfs_dir_t cwd;
        int err = lfs_dir_fetch(lfs, &cwd, file->pair);
        if (err) {
            return err;
        }

        lfs_entry_t entry = {.off = file->poff};
        err = lfs_read(lfs, cwd.pair[0], entry.off,
                &entry.d, sizeof(entry.d));
        if (err) {
            return err;
        }

        if (entry.d.type != LFS_TYPE_REG) {
            // sanity check valid entry
            return LFS_ERR_INVAL;
        }

        entry.d.u.file.head = file->head;
        entry.d.u.file.size = file->size;

        err = lfs_dir_commit(lfs, &cwd, &entry, NULL);
        if (err) {
            return err;
        }

        file->flags &= ~LFS_F_DIRTY;
    }

    return 0;
}

lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    lfs_size_t nsize = size;

    if ((file->flags & 3) == LFS_O_WRONLY) {
        return LFS_ERR_INVAL;
    }

    if (file->flags & LFS_F_WRITING) {
        // flush out any writes
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }

    size = lfs_min(size, file->size - file->pos);
    nsize = size;

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & LFS_F_READING) ||
                file->off == lfs->cfg->block_size) {
            int err = lfs_index_find(lfs, &file->cache, NULL,
                    file->head, file->size,
                    file->pos, &file->block, &file->off);
            if (err) {
                return err;
            }

            file->flags |= LFS_F_READING;
        }

        // read as much as we can in current block
        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
        int err = lfs_cache_read(lfs, &file->cache, NULL,
                file->block, file->off, data, diff);
        if (err) {
            return err;
        }

        file->pos += diff;
        file->off += diff;
        data += diff;
        nsize -= diff;
    }

    return size;
}

lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    lfs_size_t nsize = size;

    if ((file->flags & 3) == LFS_O_RDONLY) {
        return LFS_ERR_INVAL;
    }

    if (file->flags & LFS_F_READING) {
        // drop any reads
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }

    if ((file->flags & LFS_O_APPEND) && file->pos < file->size) {
        file->pos = file->size;
    }

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & LFS_F_WRITING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & LFS_F_WRITING)) {
                // find out which block we're extending from
                int err = lfs_index_find(lfs, &file->cache, NULL,
                        file->head, file->size,
                        file->pos, &file->block, &file->off);
                if (err) {
                    return err;
                }

                // mark cache as dirty since we may have read data into it
                file->cache.block = 0xffffffff;
                file->flags |= LFS_F_WRITING;
            }

            // extend file with new blocks
            int err = lfs_index_extend(lfs, &lfs->rcache, &file->cache,
                    file->block, file->pos,
                    &file->block, &file->off);
            if (err) {
                return err;
            }
        }

        // program as much as we can in current block
        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
        int err = lfs_cache_prog(lfs, &file->cache,
                file->block, file->off, data, diff);
        if (err) {
            return err;
        }

        file->pos += diff;
        file->off += diff;
        data += diff;
        nsize -= diff;
    }

    return size;
}

lfs_soff_t lfs_file_seek(lfs_t *lfs, lfs_file_t *file,
        lfs_soff_t off, int whence) {
    // write out everything beforehand, may be noop if rdonly
    int err = lfs_file_flush(lfs, file);
    if (err) {
        return err;
    }

    // update pos
    lfs_off_t pos = file->pos;

    if (whence == LFS_SEEK_SET) {
        file->pos = off;
    } else if (whence == LFS_SEEK_CUR) {
        file->pos = file->pos + off;
    } else if (whence == LFS_SEEK_END) {
        file->pos = file->size + off;
    }

    return pos;
}

lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file) {
    return file->pos;
}

int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file) {
    lfs_soff_t res = lfs_file_seek(lfs, file, 0, LFS_SEEK_SET);
    if (res < 0) {
        return res;
    }

    return 0;
}

lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file) {
    return lfs_max(file->pos, file->size);
}


/// General fs oprations ///
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info) {
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    lfs_entry_t entry;
    err = lfs_dir_find(lfs, &cwd, &entry, &path);
    if (err) {
        return err;
    }

    memset(info, 0, sizeof(*info));
    info->type = entry.d.type & 0xff;
    if (info->type == LFS_TYPE_REG) {
        info->size = entry.d.u.file.size;
    }

    err = lfs_read(lfs, cwd.pair[0], entry.off + sizeof(entry.d),
            info->name, entry.d.len - sizeof(entry.d));
    if (err) {
        return err;
    }

    return 0;
}

int lfs_remove(lfs_t *lfs, const char *path) {
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    lfs_entry_t entry;
    err = lfs_dir_find(lfs, &cwd, &entry, &path);
    if (err) {
        return err;
    }

    lfs_dir_t dir;
    if (entry.d.type == LFS_TYPE_DIR) {
        // must be empty before removal, checking size
        // without masking top bit checks for any case where
        // dir is not empty
        int err = lfs_dir_fetch(lfs, &dir, entry.d.u.dir);
        if (err) {
            return err;
        } else if (dir.d.size != sizeof(dir.d)) {
            return LFS_ERR_INVAL;
        }
    }

    // remove the entry
    err = lfs_dir_remove(lfs, &cwd, &entry);
    if (err) {
        return err;
    }

    // shift over any files that are affected
    for (lfs_file_t *f = lfs->files; f; f = f->next) {
        if (lfs_paircmp(f->pair, cwd.pair) == 0) {
            if (f->poff == entry.off) {
                f->pair[0] = 0xffffffff;
                f->pair[1] = 0xffffffff;
            } else if (f->poff > entry.off) {
                f->poff -= entry.d.len;
            }
        }
    }

    // if we were a directory, just run a deorphan step, this should
    // collect us, although is expensive
    if (entry.d.type == LFS_TYPE_DIR) {
        int err = lfs_deorphan(lfs);
        if (err) {
            return err;
        }
    }

    return 0;
}

int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath) {
    // find old entry
    lfs_dir_t oldcwd;
    int err = lfs_dir_fetch(lfs, &oldcwd, lfs->root);
    if (err) {
        return err;
    }

    lfs_entry_t oldentry;
    err = lfs_dir_find(lfs, &oldcwd, &oldentry, &oldpath);
    if (err) {
        return err;
    }

    // allocate new entry
    lfs_dir_t newcwd;
    err = lfs_dir_fetch(lfs, &newcwd, lfs->root);
    if (err) {
        return err;
    }

    lfs_entry_t preventry;
    err = lfs_dir_find(lfs, &newcwd, &preventry, &newpath);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    bool prevexists = (err != LFS_ERR_NOENT);

    // must have same type
    if (prevexists && preventry.d.type != oldentry.d.type) {
        return LFS_ERR_INVAL;
    }

    lfs_dir_t dir;
    if (prevexists && preventry.d.type == LFS_TYPE_DIR) {
        // must be empty before removal, checking size
        // without masking top bit checks for any case where
        // dir is not empty
        int err = lfs_dir_fetch(lfs, &dir, preventry.d.u.dir);
        if (err) {
            return err;
        } else if (dir.d.size != sizeof(dir.d)) {
            return LFS_ERR_INVAL;
        }
    }

    // move to new location
    lfs_entry_t newentry = preventry;
    newentry.d = oldentry.d;
    newentry.d.len = sizeof(newentry.d) + strlen(newpath);

    if (prevexists) {
        int err = lfs_dir_commit(lfs, &newcwd, &newentry, newpath);
        if (err) {
            return err;
        }
    } else {
        int err = lfs_dir_append(lfs, &newcwd, &newentry, newpath);
        if (err) {
            return err;
        }
    }

    // fetch again in case newcwd == oldcwd
    err = lfs_dir_fetch(lfs, &oldcwd, oldcwd.pair);
    if (err) {
        return err;
    }

    err = lfs_dir_find(lfs, &oldcwd, &oldentry, &oldpath);
    if (err) {
        return err;
    }

    // remove from old location
    err = lfs_dir_remove(lfs, &oldcwd, &oldentry);
    if (err) {
        return err;
    }

    // shift over any files that are affected
    for (lfs_file_t *f = lfs->files; f; f = f->next) {
        if (lfs_paircmp(f->pair, oldcwd.pair) == 0) {
            if (f->poff == oldentry.off) {
                f->pair[0] = 0xffffffff;
                f->pair[1] = 0xffffffff;
            } else if (f->poff > oldentry.off) {
                f->poff -= oldentry.d.len;
            }
        }
    }

    // if we were a directory, just run a deorphan step, this should
    // collect us, although is expensive
    if (prevexists && preventry.d.type == LFS_TYPE_DIR) {
        int err = lfs_deorphan(lfs);
        if (err) {
            return err;
        }
    }

    return 0;
}


/// Filesystem operations ///
static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg) {
    lfs->cfg = cfg;

    // setup read cache
    lfs->rcache.block = 0xffffffff;
    if (lfs->cfg->read_buffer) {
        lfs->rcache.buffer = lfs->cfg->read_buffer;
    } else {
        lfs->rcache.buffer = malloc(lfs->cfg->read_size);
        if (!lfs->rcache.buffer) {
            return LFS_ERR_NOMEM;
        }
    }

    // setup program cache
    lfs->pcache.block = 0xffffffff;
    if (lfs->cfg->prog_buffer) {
        lfs->pcache.buffer = lfs->cfg->prog_buffer;
    } else {
        lfs->pcache.buffer = malloc(lfs->cfg->prog_size);
        if (!lfs->pcache.buffer) {
            return LFS_ERR_NOMEM;
        }
    }

    // setup lookahead
    if (lfs->cfg->lookahead_buffer) {
        lfs->free.lookahead = lfs->cfg->lookahead_buffer;
    } else {
        lfs->free.lookahead = malloc(lfs->cfg->lookahead/8);
        if (!lfs->free.lookahead) {
            return LFS_ERR_NOMEM;
        }
    }

    // setup files as an empty list
    lfs->files = NULL;

    return 0;
}

static int lfs_deinit(lfs_t *lfs) {
    // Free allocated memory
    if (!lfs->cfg->read_buffer) {
        free(lfs->rcache.buffer);
    }

    if (!lfs->cfg->prog_buffer) {
        free(lfs->pcache.buffer);
    }

    if (!lfs->cfg->lookahead_buffer) {
        free(lfs->free.lookahead);
    }

    return 0;
}

int lfs_format(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    // Create free lookahead
    memset(lfs->free.lookahead, 0, lfs->cfg->lookahead/8);
    lfs->free.start = 0;
    lfs->free.off = 0;

    // Create superblock dir
    lfs_dir_t superdir;
    err = lfs_dir_alloc(lfs, &superdir);
    if (err) {
        return err;
    }

    // Write root directory
    lfs_dir_t root;
    err = lfs_dir_alloc(lfs, &root);
    if (err) {
        return err;
    }

    err = lfs_dir_commit(lfs, &root, NULL, NULL);
    if (err) {
        return err;
    }

    lfs->root[0] = root.pair[0];
    lfs->root[1] = root.pair[1];

    // Write superblocks
    lfs_superblock_t superblock = {
        .off = sizeof(superdir.d),
        .d.type = LFS_TYPE_SUPERBLOCK,
        .d.len = sizeof(superblock.d),
        .d.version = 0x00000001,
        .d.magic = {"littlefs"},
        .d.block_size  = lfs->cfg->block_size,
        .d.block_count = lfs->cfg->block_count,
        .d.root = {lfs->root[0], lfs->root[1]},
    };
    superdir.d.tail[0] = root.pair[0];
    superdir.d.tail[1] = root.pair[1];
    superdir.d.size += sizeof(superdir.d);

    for (int i = 0; i < 2; i++) {
        // Write both pairs for extra safety, do some finagling to pretend
        // the superblock is an entry
        int err = lfs_dir_commit(lfs, &superdir,
                (const lfs_entry_t*)&superblock,
                (const struct lfs_disk_entry*)&superblock.d + 1);
        if (err) {
            LFS_ERROR("Failed to write superblock at %d", superdir.pair[0]);
            return err;
        }
    }

    // sanity check that fetch works
    err = lfs_dir_fetch(lfs, &superdir, (const lfs_block_t[2]){0, 1});
    if (err) {
        return err;
    }

    return lfs_deinit(lfs);
}

int lfs_mount(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    // setup free lookahead
    lfs->free.start = -lfs->cfg->lookahead;
    lfs->free.off = lfs->cfg->lookahead;

    // load superblock
    lfs_dir_t dir;
    lfs_superblock_t superblock;
    err = lfs_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
    if (!err) {
        err = lfs_read(lfs, dir.pair[0],
                sizeof(dir.d), &superblock.d, sizeof(superblock.d));

        lfs->root[0] = superblock.d.root[0];
        lfs->root[1] = superblock.d.root[1];
    }

    if (err == LFS_ERR_CORRUPT ||
            memcmp(superblock.d.magic, "littlefs", 8) != 0) {
        LFS_ERROR("Invalid superblock at %d %d", dir.pair[0], dir.pair[1]);
        return LFS_ERR_CORRUPT;
    }

    if (superblock.d.version > 0x0000ffff) {
        LFS_ERROR("Invalid version %d.%d\n",
                0xffff & (superblock.d.version >> 16),
                0xffff & (superblock.d.version >> 0));
        return LFS_ERR_INVAL;
    }

    return err;
}

int lfs_unmount(lfs_t *lfs) {
    return lfs_deinit(lfs);
}


/// Littlefs specific operations ///
int lfs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data) {
    // iterate over metadata pairs
    lfs_dir_t dir;
    lfs_entry_t entry;
    lfs_block_t cwd[2] = {0, 1};

    while (true) {
        for (int i = 0; i < 2; i++) {
            int err = cb(data, cwd[i]);
            if (err) {
                return err;
            }
        }

        int err = lfs_dir_fetch(lfs, &dir, cwd);
        if (err) {
            return err;
        }

        // iterate over contents
        while ((0x7fffffff & dir.d.size) >= dir.off + sizeof(entry.d)) {
            int err = lfs_read(lfs, dir.pair[0], dir.off,
                    &entry.d, sizeof(entry.d));
            if (err) {
                return err;
            }

            dir.off += entry.d.len;
            if ((0xf & entry.d.type) == LFS_TYPE_REG) {
                int err = lfs_index_traverse(lfs, &lfs->rcache, NULL,
                        entry.d.u.file.head, entry.d.u.file.size, cb, data);
                if (err) {
                    return err;
                }
            }
        }

        cwd[0] = dir.d.tail[0];
        cwd[1] = dir.d.tail[1];

        if (lfs_pairisnull(cwd)) {
            break;
        }
    }

    // iterate over any open files
    for (lfs_file_t *f = lfs->files; f; f = f->next) {
        if (f->flags & LFS_F_DIRTY) {
            int err = lfs_index_traverse(lfs, &lfs->rcache, &f->cache,
                    f->head, f->size, cb, data);
            if (err) {
                return err;
            }
        }

        if (f->flags & LFS_F_WRITING) {
            int err = lfs_index_traverse(lfs, &lfs->rcache, &f->cache,
                    f->block, f->pos, cb, data);
            if (err) {
                return err;
            }
        }
    }
    
    return 0;
}

static int lfs_parent(lfs_t *lfs, const lfs_block_t dir[2]) {
    // iterate over all directory directory entries
    lfs_dir_t parent = {
        .d.tail[0] = lfs->root[0],
        .d.tail[1] = lfs->root[1],
    };

    while (true) {
        lfs_entry_t entry;
        int err = lfs_dir_fetch(lfs, &parent, parent.d.tail);
        if (err) {
            return err;
        }

        while (true) {
            int err = lfs_dir_next(lfs, &parent, &entry);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err == LFS_ERR_NOENT) {
                break;
            }

            if ((0xf & entry.d.type) == LFS_TYPE_DIR &&
                    lfs_paircmp(entry.d.u.dir, dir) == 0) {
                return true;
            }
        }

        if (lfs_pairisnull(parent.d.tail)) {
            return false;
        }
    }
}

int lfs_deorphan(lfs_t *lfs) {
    // iterate over all directories
    lfs_dir_t pdir;
    lfs_dir_t cdir;

    // skip root
    int err = lfs_dir_fetch(lfs, &pdir, lfs->root);
    if (err) {
        return err;
    }

    while (!lfs_pairisnull(pdir.d.tail)) {
        int err = lfs_dir_fetch(lfs, &cdir, pdir.d.tail);
        if (err) {
            return err;
        }

        // only check head blocks
        if (!(0x80000000 & pdir.d.size)) {
            // check if we have a parent
            int parent = lfs_parent(lfs, pdir.d.tail);
            if (parent < 0) {
                return parent;
            }

            if (!parent) {
                // we are an orphan
                LFS_DEBUG("Orphan %d %d", pdir.d.tail[0], pdir.d.tail[1]);

                pdir.d.tail[0] = cdir.d.tail[0];
                pdir.d.tail[1] = cdir.d.tail[1];

                err = lfs_dir_commit(lfs, &pdir, NULL, NULL);
                if (err) {
                    return err;
                }

                break;
            }
        }

        memcpy(&pdir, &cdir, sizeof(pdir));
    }

    return 0;
}

