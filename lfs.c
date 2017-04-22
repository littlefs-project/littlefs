/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#include "lfs.h"
#include "lfs_util.h"

#include <string.h>
#include <stdbool.h>
#include <stdlib.h>


/// Block device operations ///
static int lfs_bd_flush(lfs_t *lfs) {
    if (lfs->pcache.off != -1) {
        int err = lfs->cfg->prog(lfs->cfg, lfs->pcache.block,
                lfs->pcache.off, lfs->cfg->prog_size,
                lfs->pcache.buffer);
        if (err) {
            return err;
        }

        lfs->pcache.off = -1;
    }

    return 0;
}

static int lfs_bd_read(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, void *buffer) {
    uint8_t *data = buffer;

    // flush overlapping programs
    while (size > 0) {
        if (block == lfs->pcache.block && off >= lfs->pcache.off &&
                off < lfs->pcache.off + lfs->cfg->prog_size) {
            // is already in cache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->prog_size - (off-lfs->pcache.off));
            memcpy(data, &lfs->pcache.buffer[off-lfs->pcache.off], diff);

            data += diff;
            off += diff;
            size -= diff;
            continue;
        } else if (block == lfs->rcache.block && off >= lfs->rcache.off &&
                off < lfs->rcache.off + lfs->cfg->read_size) {
            // is already in cache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->read_size - (off-lfs->rcache.off));
            memcpy(data, &lfs->rcache.buffer[off-lfs->rcache.off], diff);

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // write out pending programs
        int err = lfs_bd_flush(lfs);
        if (err) {
            return err;
        }

        if (off % lfs->cfg->read_size == 0 &&
                size >= lfs->cfg->read_size) {
            // bypass cache?
            lfs_size_t diff = size - (size % lfs->cfg->read_size);
            int err = lfs->cfg->read(lfs->cfg, block, off, diff, data);
            if (err) {
                return err;
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // load to cache, first condition can no longer fail
        lfs->rcache.block = block;
        lfs->rcache.off = off - (off % lfs->cfg->read_size);
        // TODO remove reading, should be unnecessary
        err = lfs->cfg->read(lfs->cfg, lfs->rcache.block,
                lfs->rcache.off, lfs->cfg->read_size,
                lfs->rcache.buffer);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_bd_prog(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer) {
    const uint8_t *data = buffer;

    if (block == lfs->rcache.block) {
        // invalidate read cache
        lfs->rcache.off = -1;
    }

    while (size > 0) {
        if (block == lfs->pcache.block && off >= lfs->pcache.off &&
                off < lfs->pcache.off + lfs->cfg->prog_size) {
            // is already in cache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->prog_size - (off-lfs->pcache.off));
            memcpy(&lfs->pcache.buffer[off-lfs->pcache.off], data, diff);

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // write out pending programs
        int err = lfs_bd_flush(lfs);
        if (err) {
            return err;
        }

        if (off % lfs->cfg->prog_size == 0 &&
                size >= lfs->cfg->prog_size) {
            // bypass cache?
            lfs_size_t diff = size - (size % lfs->cfg->prog_size);
            int err = lfs->cfg->prog(lfs->cfg, block, off, diff, data);
            if (err) {
                return err;
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // prepare cache, first condition can no longer fail
        lfs->pcache.block = block;
        lfs->pcache.off = off - (off % lfs->cfg->prog_size);
        err = lfs->cfg->read(lfs->cfg, lfs->pcache.block,
                lfs->pcache.off, lfs->cfg->prog_size,
                lfs->pcache.buffer);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_bd_erase(lfs_t *lfs, lfs_block_t block) {
    return lfs->cfg->erase(lfs->cfg, block);
}

static int lfs_bd_sync(lfs_t *lfs) {
    int err = lfs_bd_flush(lfs);
    if (err) {
        return err;
    }

    return lfs->cfg->sync(lfs->cfg);
}

static int lfs_bd_cmp(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer) {
    const uint8_t *data = buffer;

    for (lfs_off_t i = 0; i < size; i++) {
        uint8_t c;
        int err = lfs_bd_read(lfs, block, off+i, 1, &c);
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

    lfs_block_t off = block - lfs->free.begin;
    if (off < LFS_CFG_LOOKAHEAD) {
        lfs->lookahead[off / 32] |= 1U << (off % 32);
    }

    return 0;
}

static int lfs_alloc_stride(void *p, lfs_block_t block) {
    lfs_t *lfs = p;

    lfs_block_t noff = block - lfs->free.begin;
    lfs_block_t off = lfs->free.end - lfs->free.begin;
    if (noff < off) {
        lfs->free.end = noff + lfs->free.begin;
    }

    return 0;
}

static int lfs_alloc_scan(lfs_t *lfs) {
    lfs_block_t start = lfs->free.begin;

    while (true) {
        // mask out blocks in lookahead region
        memset(lfs->lookahead, 0, sizeof(lfs->lookahead));
        int err = lfs_traverse(lfs, lfs_alloc_lookahead, lfs);
        if (err) {
            return err;
        }

        // check if we've found a free block
        for (uint32_t off = 0; off < LFS_CFG_LOOKAHEAD; off++) {
            if (lfs->lookahead[off / 32] & (1U << (off % 32))) {
                continue;
            }

            // found free block, now find stride of free blocks
            // since this is relatively cheap (stress on relatively)
            lfs->free.begin += off;
            lfs->free.end = lfs->cfg->block_count; // before superblock

            // find maximum stride in tree
            return lfs_traverse(lfs, lfs_alloc_stride, lfs);
        }

        // continue to next lookahead unless we've searched the whole device
        if (start-1 - lfs->free.begin < LFS_CFG_LOOKAHEAD) {
            return 0;
        }

        // continue to next lookahead region
        lfs->free.begin += LFS_CFG_LOOKAHEAD;
    }
}

static int lfs_alloc(lfs_t *lfs, lfs_block_t *block) {
    // If we don't remember any free blocks we will need to start searching
    if (lfs->free.begin == lfs->free.end) {
        int err = lfs_alloc_scan(lfs);
        if (err) {
            return err;
        }

        if (lfs->free.begin == lfs->free.end) {
            // Still can't allocate a block? check for orphans
            int err = lfs_deorphan(lfs);
            if (err) {
                return err;
            }

            err = lfs_alloc_scan(lfs);
            if (err) {
                return err;
            }

            if (lfs->free.begin == lfs->free.end) {
                // Ok, it's true, we're out of space
                return LFS_ERROR_NO_SPACE;
            }
        }
    }

    // Take first available block
    *block = lfs->free.begin;
    lfs->free.begin += 1;
    return 0;
}

static int lfs_alloc_erased(lfs_t *lfs, lfs_block_t *block) {
    // TODO rm me?
    int err = lfs_alloc(lfs, block);
    if (err) {
        return err;
    }

    return lfs_bd_erase(lfs, *block);
}


/// Index list operations ///

// Next index offset
static lfs_off_t lfs_indexnext(lfs_t *lfs, lfs_off_t ioff) {
    ioff += 1;
    while (ioff % lfs->words == 0) {
        ioff += lfs_min(lfs_ctz(ioff/lfs->words + 1), lfs->words-1) + 1;
    }

    return ioff;
}

static lfs_off_t lfs_indexfrom(lfs_t *lfs, lfs_off_t off) {
    lfs_off_t i = 0;
    while (off > lfs->cfg->block_size) {
        i = lfs_indexnext(lfs, i);
        off -= lfs->cfg->block_size;
    }

    return i;
}

// Find index in index chain given its index offset
static int lfs_index_find(lfs_t *lfs, lfs_block_t head,
        lfs_size_t icount, lfs_off_t ioff, lfs_block_t *block) {
    lfs_off_t iitarget = ioff / lfs->words;
    lfs_off_t iicurrent = (icount-1) / lfs->words;

    while (iitarget != iicurrent) {
        lfs_size_t skip = lfs_min(
                lfs_min(lfs_ctz(iicurrent+1), lfs->words-1),
                lfs_npw2((iitarget ^ iicurrent)+1)-1);

        int err = lfs_bd_read(lfs, head, 4*skip, 4, &head);
        if (err) {
            return err;
        }

        iicurrent -= 1 << skip;
    }

    return lfs_bd_read(lfs, head, 4*(ioff % lfs->words), 4, block);
}

// Append index to index chain, updates head and icount
static int lfs_index_append(lfs_t *lfs, lfs_block_t *headp,
        lfs_size_t *icountp, lfs_block_t block) {
    lfs_block_t head = *headp;
    lfs_size_t ioff = *icountp - 1;

    ioff += 1;

    while (ioff % lfs->words == 0) {
        lfs_block_t nhead;
        int err = lfs_alloc_erased(lfs, &nhead);
        if (err) {
            return err;
        }

        lfs_off_t skips = lfs_min(
                lfs_ctz(ioff/lfs->words + 1), lfs->words-2) + 1;
        for (lfs_off_t i = 0; i < skips; i++) {
            err = lfs_bd_prog(lfs, nhead, 4*i, 4, &head);
            if (err) {
                return err;
            }

            if (head && i != skips-1) {
                err = lfs_bd_read(lfs, head, 4*i, 4, &head);
                if (err) {
                    return err;
                }
            }
        }

        ioff += skips;
        head = nhead;
    }

    int err = lfs_bd_prog(lfs, head, 4*(ioff % lfs->words), 4, &block);
    if (err) {
        return err;
    }

    *headp = head;
    *icountp = ioff + 1;
    return 0;
}

static int lfs_index_traverse(lfs_t *lfs, lfs_block_t head,
        lfs_size_t icount, int (*cb)(void*, lfs_block_t), void *data) {
    lfs_off_t iicurrent = (icount-1) / lfs->words;

    while (iicurrent > 0) {
        int err = cb(data, head);
        if (err) {
            return err;
        }

        lfs_size_t skip = lfs_min(lfs_ctz(iicurrent+1), lfs->words-1);
        for (lfs_off_t i = skip; i < lfs->words; i++) {
            lfs_block_t block;
            int err = lfs_bd_read(lfs, head, 4*i, 4, &block);
            if (err) {
                return err;
            }

            err = cb(data, block);
            if (err) {
                return err;
            }
        }

        err = lfs_bd_read(lfs, head, 0, 4, &head);
        if (err) {
            return err;
        }

        iicurrent -= 1;
    }

    int err = cb(data, head);
    if (err) {
        return err;
    }

    for (lfs_off_t i = 0; i < lfs->words; i++) {
        lfs_block_t block;
        int err = lfs_bd_read(lfs, head, 4*i, 4, &block);
        if (err) {
            return err;
        }

        err = cb(data, block);
        if (err) {
            return err;
        }
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
    return !pair[0] || !pair[1];
}

static inline int lfs_paircmp(
        const lfs_block_t paira[2],
        const lfs_block_t pairb[2]) {
    return !((paira[0] == pairb[0] && paira[1] == pairb[1]) ||
             (paira[0] == pairb[1] && paira[1] == pairb[0]));
}

static int lfs_dir_alloc(lfs_t *lfs, lfs_dir_t *dir) {
    // Allocate pair of dir blocks
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[i]);
        if (err) {
            return err;
        }
    }

    // Rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs_bd_read(lfs, dir->pair[0], 0, 4, &dir->d.rev);
    if (err) {
        return err;
    }

    // Set defaults
    dir->d.rev += 1;
    dir->d.size = sizeof(dir->d);
    dir->d.tail[0] = 0;
    dir->d.tail[1] = 0;
    dir->off = sizeof(dir->d);

    // Don't write out yet, let caller take care of that
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
        int err = lfs_bd_read(lfs, tpair[i], 0, sizeof(test), &test);
        if (err) {
            return err;
        }

        if (valid && lfs_scmp(test.rev, dir->d.rev) < 0) {
            continue;
        }

        uint32_t crc = 0xffffffff;
        crc = lfs_crc(crc, sizeof(test), &test);

        for (lfs_off_t j = sizeof(test); j < lfs->cfg->block_size; j += 4) {
            uint32_t word;
            int err = lfs_bd_read(lfs, tpair[i], j, 4, &word);
            if (err) {
                return err;
            }

            crc = lfs_crc(crc, 4, &word);
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
        return LFS_ERROR_CORRUPT;
    }

    return 0;
}

static int lfs_dir_commit(lfs_t *lfs, lfs_dir_t *dir,
        const lfs_entry_t *entry, const void *data) {
    dir->d.rev += 1;
    lfs_pairswap(dir->pair);

    int err = lfs_bd_erase(lfs, dir->pair[0]);
    if (err) {
        return err;
    }

    uint32_t crc = 0xffffffff;
    crc = lfs_crc(crc, sizeof(dir->d), &dir->d);
    err = lfs_bd_prog(lfs, dir->pair[0], 0, sizeof(dir->d), &dir->d);
    if (err) {
        return err;
    }

    lfs_off_t off = sizeof(dir->d);
    lfs_size_t size = 0x7fffffff & dir->d.size;
    while (off < size) {
        if (entry && off == entry->off) {
            crc = lfs_crc(crc, sizeof(entry->d), &entry->d);
            int err = lfs_bd_prog(lfs, dir->pair[0],
                    off, sizeof(entry->d), &entry->d);
            if (err) {
                return err;
            }
            off += sizeof(entry->d);

            if (data) {
                crc = lfs_crc(crc, entry->d.len - sizeof(entry->d), data);
                int err = lfs_bd_prog(lfs, dir->pair[0],
                        off, entry->d.len - sizeof(entry->d), data);
                if (err) {
                    return err;
                }
                off += entry->d.len - sizeof(entry->d);
            }
        } else {
            uint8_t data;
            int err = lfs_bd_read(lfs, dir->pair[1], off, 1, &data);
            if (err) {
                return err;
            }

            crc = lfs_crc(crc, 1, &data);
            err = lfs_bd_prog(lfs, dir->pair[0], off, 1, &data);
            if (err) {
                return err;
            }

            off += 1;
        }
    }

    while (off < lfs->cfg->block_size-4) {
        uint8_t data = 0xff;
        crc = lfs_crc(crc, 1, &data);
        err = lfs_bd_prog(lfs, dir->pair[0], off, 1, &data);
        if (err) {
            return err;
        }

        off += 1;
    }

    err = lfs_bd_prog(lfs, dir->pair[0], lfs->cfg->block_size-4, 4, &crc);
    if (err) {
        return err;
    }

    return lfs_bd_sync(lfs);
}

static int lfs_dir_shift(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry) {
    dir->d.rev += 1;
    dir->d.size -= entry->d.len;
    lfs_pairswap(dir->pair);

    int err = lfs_bd_erase(lfs, dir->pair[0]);
    if (err) {
        return err;
    }

    uint32_t crc = 0xffffffff;
    crc = lfs_crc(crc, sizeof(dir->d), &dir->d);
    err = lfs_bd_prog(lfs, dir->pair[0], 0, sizeof(dir->d), &dir->d);
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
            int err = lfs_bd_read(lfs, dir->pair[1], roff, 1, &data);
            if (err) {
                return err;
            }

            crc = lfs_crc(crc, 1, (void*)&data);
            err = lfs_bd_prog(lfs, dir->pair[0], woff, 1, &data);
            if (err) {
                return err;
            }

            woff += 1;
            roff += 1;
        }
    }

    while (woff < lfs->cfg->block_size-4) {
        uint8_t data = 0xff;
        crc = lfs_crc(crc, 1, &data);
        err = lfs_bd_prog(lfs, dir->pair[0], woff, 1, &data);
        if (err) {
            return err;
        }


        woff += 1;
    }

    err = lfs_bd_prog(lfs, dir->pair[0], lfs->cfg->block_size-4, 4, &crc);
    if (err) {
        return err;
    }

    return lfs_bd_sync(lfs);
}

static int lfs_dir_append(lfs_t *lfs, lfs_dir_t *dir,
        lfs_entry_t *entry, const void *data) {
    // check if we fit, if top bit is set we do not and move on
    while (true) {
        if (dir->d.size + entry->d.len <= lfs->cfg->block_size - 4) {
            entry->pair[0] = dir->pair[0];
            entry->pair[1] = dir->pair[1];
            entry->off = dir->d.size;
            dir->d.size += entry->d.len;
            return lfs_dir_commit(lfs, dir, entry, data);
        }

        if (!(0x80000000 & dir->d.size)) {
            lfs_dir_t newdir;
            int err = lfs_dir_alloc(lfs, &newdir);
            if (err) {
                return err;
            }

            newdir.d.tail[0] = dir->d.tail[0];
            newdir.d.tail[1] = dir->d.tail[1];
            entry->pair[0] = newdir.pair[0];
            entry->pair[1] = newdir.pair[1];
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

        // TODO easier check for head block? (common case)
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
        if ((0x7fffffff & dir->d.size) - dir->off < sizeof(entry->d)) {
            if (!(dir->d.size >> 31)) {
                entry->pair[0] = dir->pair[0];
                entry->pair[1] = dir->pair[1];
                entry->off = dir->off;
                return LFS_ERROR_NO_ENTRY;
            }

            int err = lfs_dir_fetch(lfs, dir, dir->d.tail);
            if (err) {
                return err;
            }

            dir->off = sizeof(dir->d);
            continue;
        }

        int err = lfs_bd_read(lfs, dir->pair[0], dir->off,
                sizeof(entry->d), &entry->d);
        if (err) {
            return err;
        }

        dir->off += entry->d.len;
        if ((0xff & entry->d.type) == LFS_TYPE_REG ||
            (0xff & entry->d.type) == LFS_TYPE_DIR) {
            entry->pair[0] = dir->pair[0];
            entry->pair[1] = dir->pair[1];
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

            int ret = lfs_bd_cmp(lfs, dir->pair[0],
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
            return LFS_ERROR_NOT_DIR;
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
    if (err != LFS_ERROR_NO_ENTRY) {
        return err ? err : LFS_ERROR_EXISTS;
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
    } else if (strcmp(path, "/") == 0) {
        // special offset for '.' and '..'
        dir->off = sizeof(dir->d) - 2;
        return 0;
    }

    lfs_entry_t entry;
    err = lfs_dir_find(lfs, dir, &entry, &path);
    if (err) {
        return err;
    } else if (entry.d.type != LFS_TYPE_DIR) {
        return LFS_ERROR_NOT_DIR;
    }

    err = lfs_dir_fetch(lfs, dir, entry.d.u.dir);
    if (err) {
        return err;
    }

    // special offset for '.' and '..'
    dir->off = sizeof(dir->d) - 2;
    return 0;
}

int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
    // Do nothing, dir is always synchronized
    return 0;
}

int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info) {
    memset(info, 0, sizeof(*info));

    if (dir->off == sizeof(dir->d) - 2) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, ".");
        dir->off += 1;
        return 1;
    } else if (dir->off == sizeof(dir->d) - 1) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, "..");
        dir->off += 1;
        return 1;
    }

    lfs_entry_t entry;
    int err = lfs_dir_next(lfs, dir, &entry);
    if (err) {
        return (err == LFS_ERROR_NO_ENTRY) ? 0 : err;
    }

    info->type = entry.d.type & 0xff;
    if (info->type == LFS_TYPE_REG) {
        info->size = entry.d.u.file.size;
    }

    err = lfs_bd_read(lfs, dir->pair[0], entry.off + sizeof(entry.d),
            entry.d.len - sizeof(entry.d), info->name);
    if (err) {
        return err;
    }

    return 1;
}


/// Top level file operations ///
int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    // Allocate entry for file if it doesn't exist
    // TODO check open files
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    err = lfs_dir_find(lfs, &cwd, &file->entry, &path);
    if (err && !((flags & LFS_O_CREAT) && err == LFS_ERROR_NO_ENTRY)) {
        return err;
    } else if (err != LFS_ERROR_NO_ENTRY &&
            file->entry.d.type == LFS_TYPE_DIR) {
        return LFS_ERROR_IS_DIR;
    }

    if ((flags & LFS_O_CREAT) && err == LFS_ERROR_NO_ENTRY) {
        // create entry to remember name
        file->entry.d.type = 1;
        file->entry.d.len = sizeof(file->entry.d) + strlen(path);
        file->entry.d.u.file.head = 0;
        file->entry.d.u.file.size = 0;
        int err = lfs_dir_append(lfs, &cwd, &file->entry, path);
        if (err) {
            return err;
        }
    }

    file->head = file->entry.d.u.file.head;
    file->size = file->entry.d.u.file.size;
    file->windex = lfs_indexfrom(lfs, file->size);
    file->rblock = 0;
    file->rindex = 0;
    file->roff = 0;

    // TODO do this lazily in write?
    // TODO cow the head i/d block
    if (file->size < lfs->cfg->block_size) {
        file->wblock = file->head;
    } else {
        int err = lfs_index_find(lfs, file->head, file->windex,
                file->windex, &file->wblock);
        if (err) {
            return err;
        }
    }

    return 0;
}

int lfs_file_close(lfs_t *lfs, lfs_file_t *file) {
    // Store file
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, file->entry.pair);
    if (err) {
        return err;
    }

    file->entry.d.u.file.head = file->head;
    file->entry.d.u.file.size = file->size;

    return lfs_dir_commit(lfs, &cwd, &file->entry, NULL);
}

lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    lfs_size_t nsize = size;

    while (nsize > 0) {
        lfs_off_t woff = file->size % lfs->cfg->block_size;

        if (file->size == 0) {
            int err = lfs_alloc_erased(lfs, &file->wblock);
            if (err) {
                return err;
            }

            file->head = file->wblock;
            file->windex = 0;
        } else if (woff == 0) {
            int err = lfs_alloc_erased(lfs, &file->wblock);
            if (err) {
                return err;
            }

            err = lfs_index_append(lfs, &file->head,
                    &file->windex, file->wblock);
            if (err) {
                return err;
            }
        }

        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - woff);
        int err = lfs_bd_prog(lfs, file->wblock, woff, diff, data);
        if (err) {
            return err;
        }

        file->size += diff;
        data += diff;
        nsize -= diff;
    }

    return size;
}

lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    lfs_size_t nsize = size;

    while (nsize > 0 && file->roff < file->size) {
        lfs_off_t roff = file->roff % lfs->cfg->block_size;

        // TODO cache index blocks
        if (file->size < lfs->cfg->block_size) {
            file->rblock = file->head;
        } else if (roff == 0) {
            int err = lfs_index_find(lfs, file->head, file->windex,
                    file->rindex, &file->rblock);
            if (err) {
                return err;
            }

            file->rindex = lfs_indexnext(lfs, file->rindex);
        }

        lfs_size_t diff = lfs_min(
                lfs_min(nsize, file->size-file->roff),
                lfs->cfg->block_size - roff);
        int err = lfs_bd_read(lfs, file->rblock, roff, diff, data);
        if (err) {
            return err;
        }

        file->roff += diff;
        data += diff;
        nsize -= diff;
    }

    return size - nsize;
}


/// Generic filesystem operations ///
static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg) {
    lfs->cfg = cfg;
    lfs->words = lfs->cfg->block_size / sizeof(uint32_t);
    lfs->rcache.off = -1;
    lfs->pcache.off = -1;

    if (lfs->cfg->read_buffer) {
        lfs->rcache.buffer = lfs->cfg->read_buffer;
    } else {
        lfs->rcache.buffer = malloc(lfs->cfg->read_size);
        if (!lfs->rcache.buffer) {
            return LFS_ERROR_NO_MEM;
        }
    }

    if (lfs->cfg->prog_buffer) {
        lfs->pcache.buffer = lfs->cfg->prog_buffer;
    } else {
        lfs->pcache.buffer = malloc(lfs->cfg->prog_size);
        if (!lfs->pcache.buffer) {
            return LFS_ERROR_NO_MEM;
        }
    }

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

    return 0;
}

int lfs_format(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    // Create free list
    lfs->free.begin = 0;
    lfs->free.end = lfs->cfg->block_count-1;

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

    lfs_dir_t dir;
    lfs_superblock_t superblock;
    err = lfs_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
    if (!err) {
        err = lfs_bd_read(lfs, dir.pair[0],
                sizeof(dir.d), sizeof(superblock.d), &superblock.d);
    }

    if (err == LFS_ERROR_CORRUPT ||
            memcmp(superblock.d.magic, "littlefs", 8) != 0) {
        LFS_ERROR("Invalid superblock at %d %d", dir.pair[0], dir.pair[1]);
        return LFS_ERROR_CORRUPT;
    }

    if (superblock.d.version > 0x0000ffff) {
        LFS_ERROR("Invalid version %d.%d\n",
                0xffff & (superblock.d.version >> 16),
                0xffff & (superblock.d.version >> 0));
    }

    lfs->root[0] = superblock.d.root[0];
    lfs->root[1] = superblock.d.root[1];

    return err;
}

int lfs_unmount(lfs_t *lfs) {
    return lfs_deinit(lfs);
}

int lfs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data) {
    // iterate over metadata pairs
    lfs_dir_t dir;
    lfs_file_t file;
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
        while ((0x7fffffff & dir.d.size) >= dir.off + sizeof(file.entry.d)) {
            int err = lfs_bd_read(lfs, dir.pair[0], dir.off,
                    sizeof(file.entry.d), &file.entry.d);
            if (err) {
                return err;
            }

            dir.off += file.entry.d.len;
            if ((0xf & file.entry.d.type) == LFS_TYPE_REG) {
                if (file.entry.d.u.file.size < lfs->cfg->block_size) {
                    int err = cb(data, file.entry.d.u.file.head);
                    if (err) {
                        return err;
                    }
                } else {
                    int err = lfs_index_traverse(lfs,
                            file.entry.d.u.file.head,
                            lfs_indexfrom(lfs, file.entry.d.u.file.size),
                            cb, data);
                    if (err) {
                        return err;
                    }
                }
            }
        }

        cwd[0] = dir.d.tail[0];
        cwd[1] = dir.d.tail[1];

        if (!cwd[0]) {
            return 0;
        }
    }
}

static int lfs_parent(lfs_t *lfs, const lfs_block_t dir[2]) {
    // iterate over all directory directory entries
    lfs_dir_t parent = {
        .d.tail[0] = lfs->root[0],
        .d.tail[1] = lfs->root[1],
    };

    while (parent.d.tail[0]) {
        lfs_entry_t entry;
        int err = lfs_dir_fetch(lfs, &parent, parent.d.tail);
        if (err) {
            return err;
        }

        while (true) {
            int err = lfs_dir_next(lfs, &parent, &entry);
            if (err && err != LFS_ERROR_NO_ENTRY) {
                return err;
            }

            if (err == LFS_ERROR_NO_ENTRY) {
                break;
            }

            if ((0xf & entry.d.type) == LFS_TYPE_DIR &&
                    lfs_paircmp(entry.d.u.dir, dir) == 0) {
                return true;
            }
        }
    }

    return false;
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

    while (pdir.d.tail[0]) {
        int err = lfs_dir_fetch(lfs, &cdir, pdir.d.tail);
        if (err) {
            return err;
        }

        // check if we have a parent
        int parent = lfs_parent(lfs, pdir.d.tail);
        if (parent < 0) {
            return parent;
        }

        if (!parent) {
            // we are an orphan
            LFS_INFO("Orphan %d %d", pdir.d.tail[0], pdir.d.tail[1]);

            pdir.d.tail[0] = cdir.d.tail[0];
            pdir.d.tail[1] = cdir.d.tail[1];

            err = lfs_dir_commit(lfs, &pdir, NULL, NULL);
            if (err) {
                return err;
            }

            break;
        }

        memcpy(&pdir, &cdir, sizeof(pdir));
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
            return LFS_ERROR_INVALID;
        }
    }

    // remove the entry
    err = lfs_dir_remove(lfs, &cwd, &entry);
    if (err) {
        return err;
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
    if (err && err != LFS_ERROR_NO_ENTRY) {
        return err;
    }
    bool prevexists = (err != LFS_ERROR_NO_ENTRY);

    // must have same type
    if (prevexists && preventry.d.type != oldentry.d.type) {
        return LFS_ERROR_INVALID;
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
            return LFS_ERROR_INVALID;
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
    // TODO handle this better?
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

    // TODO abstract out info assignment
    memset(info, 0, sizeof(*info));
    info->type = entry.d.type & 0xff;
    if (info->type == LFS_TYPE_REG) {
        info->size = entry.d.u.file.size;
    }

    err = lfs_bd_read(lfs, cwd.pair[0], entry.off + sizeof(entry.d),
            entry.d.len - sizeof(entry.d), info->name);
    if (err) {
        return err;
    }

    return 0;
}
