/*
 * The little filesystem
 *
 * Copyright (c) 2017 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "lfs.h"
#include "lfs_util.h"


/// Caching block device operations ///
static inline void lfs_cache_drop(lfs_t *lfs, lfs_cache_t *rcache) {
    // do not zero, cheaper if cache is readonly or only going to be
    // written with identical data (during relocates)
    (void)lfs;
    rcache->block = 0xffffffff;
}

static inline void lfs_cache_zero(lfs_t *lfs, lfs_cache_t *pcache) {
    // zero to avoid information leak
    memset(pcache->buffer, 0xff, lfs->cfg->prog_size);
    pcache->block = 0xffffffff;
}

static int lfs_bd_read(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    LFS_ASSERT(block != 0xffffffff);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_CORRUPT;
    }

    while (size > 0) {
        if (pcache && block == pcache->block &&
                off >= pcache->off &&
                off < pcache->off + pcache->size) {
            // is already in pcache?
            lfs_size_t diff = lfs_min(size, pcache->size - (off-pcache->off));
            memcpy(data, &pcache->buffer[off-pcache->off], diff);

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        if (block == rcache->block &&
                off >= rcache->off &&
                off < rcache->off + rcache->size) {
            // is already in rcache?
            lfs_size_t diff = lfs_min(size, rcache->size - (off-rcache->off));
            if (pcache && block == pcache->block) {
                diff = lfs_min(diff, pcache->off - off);
            }
            memcpy(data, &rcache->buffer[off-rcache->off], diff);

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        if (size >= hint && off % lfs->cfg->read_size == 0 &&
                size >= lfs->cfg->read_size) {
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
        LFS_ASSERT(block < lfs->cfg->block_count);
        rcache->block = block;
        rcache->off = lfs_aligndown(off, lfs->cfg->prog_size);
        rcache->size = lfs_min(lfs_min(
                lfs_alignup(off+hint, lfs->cfg->prog_size),
                lfs->cfg->block_size) - rcache->off, lfs->cfg->cache_size);
        int err = lfs->cfg->read(lfs->cfg, rcache->block,
                rcache->off, rcache->buffer, rcache->size);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_bd_cmp(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;

    for (lfs_off_t i = 0; i < size; i++) {
        uint8_t dat;
        int err = lfs_bd_read(lfs,
                pcache, rcache, hint-i,
                block, off+i, &dat, 1);
        if (err) {
            return err;
        }

        if (dat != data[i]) {
            return false;
        }
    }

    return true;
}

static int lfs_bd_flush(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate) {
    if (pcache->block != 0xffffffff && pcache->block != 0xfffffffe) {
        LFS_ASSERT(pcache->block < lfs->cfg->block_count);
        lfs_size_t diff = lfs_alignup(pcache->size, lfs->cfg->prog_size);
        int err = lfs->cfg->prog(lfs->cfg, pcache->block,
                pcache->off, pcache->buffer, diff);
        if (err) {
            return err;
        }

        if (validate) {
            // check data on disk
            lfs_cache_drop(lfs, rcache);
            int res = lfs_bd_cmp(lfs,
                    NULL, rcache, diff,
                    pcache->block, pcache->off, pcache->buffer, diff);
            if (res < 0) {
                return res;
            }

            if (!res) {
                return LFS_ERR_CORRUPT;
            }
        }

        lfs_cache_zero(lfs, pcache);
    }

    return 0;
}

static int lfs_bd_sync(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate) {
    lfs_cache_drop(lfs, rcache);

    int err = lfs_bd_flush(lfs, pcache, rcache, validate);
    if (err) {
        return err;
    }

    return lfs->cfg->sync(lfs->cfg);
}

static int lfs_bd_prog(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate,
        lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    LFS_ASSERT(block != 0xffffffff);
    LFS_ASSERT(off + size <= lfs->cfg->block_size);

    while (size > 0) {
        if (block == pcache->block &&
                off >= pcache->off &&
                off < pcache->off + lfs->cfg->cache_size) {
            // already fits in pcache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->cache_size - (off-pcache->off));
            memcpy(&pcache->buffer[off-pcache->off], data, diff);

            data += diff;
            off += diff;
            size -= diff;

            pcache->size = off - pcache->off;
            if (pcache->size == lfs->cfg->cache_size) {
                // eagerly flush out pcache if we fill up
                int err = lfs_bd_flush(lfs, pcache, rcache, validate);
                if (err) {
                    return err;
                }
            }

            continue;
        }

        // pcache must have been flushed, either by programming and
        // entire block or manually flushing the pcache
        LFS_ASSERT(pcache->block == 0xffffffff);

        // prepare pcache, first condition can no longer fail
        pcache->block = block;
        pcache->off = lfs_aligndown(off, lfs->cfg->prog_size);
        pcache->size = 0;
    }

    return 0;
}

static int lfs_bd_erase(lfs_t *lfs, lfs_block_t block) {
    LFS_ASSERT(block < lfs->cfg->block_count);
    return lfs->cfg->erase(lfs->cfg, block);
}


/// Small type-level utilities ///
// operations on block pairs
static inline void lfs_pair_swap(lfs_block_t pair[2]) {
    lfs_block_t t = pair[0];
    pair[0] = pair[1];
    pair[1] = t;
}

static inline bool lfs_pair_isnull(const lfs_block_t pair[2]) {
    return pair[0] == 0xffffffff || pair[1] == 0xffffffff;
}

static inline int lfs_pair_cmp(
        const lfs_block_t paira[2],
        const lfs_block_t pairb[2]) {
    return !(paira[0] == pairb[0] || paira[1] == pairb[1] ||
             paira[0] == pairb[1] || paira[1] == pairb[0]);
}

static inline bool lfs_pair_sync(
        const lfs_block_t paira[2],
        const lfs_block_t pairb[2]) {
    return (paira[0] == pairb[0] && paira[1] == pairb[1]) ||
           (paira[0] == pairb[1] && paira[1] == pairb[0]);
}

static inline void lfs_pair_fromle32(lfs_block_t pair[2]) {
    pair[0] = lfs_fromle32(pair[0]);
    pair[1] = lfs_fromle32(pair[1]);
}

static inline void lfs_pair_tole32(lfs_block_t pair[2]) {
    pair[0] = lfs_tole32(pair[0]);
    pair[1] = lfs_tole32(pair[1]);
}

// operations on 32-bit entry tags
typedef uint32_t lfs_tag_t;
typedef int32_t lfs_stag_t;

#define LFS_MKTAG(type, id, size) \
    (((lfs_tag_t)(type) << 22) | ((lfs_tag_t)(id) << 12) | (lfs_tag_t)(size))

static inline bool lfs_tag_isvalid(lfs_tag_t tag) {
    return !(tag & 0x80000000);
}

static inline bool lfs_tag_isuser(lfs_tag_t tag) {
    return (tag & 0x40000000);
}

static inline bool lfs_tag_isdelete(lfs_tag_t tag) {
    return (tag & 0x00000fff) == 0xfff;
}

static inline uint16_t lfs_tag_type(lfs_tag_t tag) {
    return (tag & 0x7fc00000) >> 22;
}

static inline uint16_t lfs_tag_subtype(lfs_tag_t tag) {
    return ((tag & 0x7c000000) >> 26) << 4;
}

static inline uint16_t lfs_tag_id(lfs_tag_t tag) {
    return (tag & 0x003ff000) >> 12;
}

static inline lfs_size_t lfs_tag_size(lfs_tag_t tag) {
    return tag & 0x00000fff;
}

static inline lfs_size_t lfs_tag_dsize(lfs_tag_t tag) {
    return sizeof(tag) + lfs_tag_size(tag + lfs_tag_isdelete(tag));
}

// operations on attributes in attribute lists
struct lfs_mattr {
    lfs_tag_t tag;
    const void *buffer;
    const struct lfs_mattr *next;
};

#define LFS_MKATTR(type, id, buffer, size, next) \
    &(const struct lfs_mattr){LFS_MKTAG(type, id, size), (buffer), (next)}

struct lfs_diskoff {
    lfs_block_t block;
    lfs_off_t off;
};

// operations on set of globals
static inline void lfs_global_xor(lfs_global_t *a, const lfs_global_t *b) {
    for (int i = 0; i < sizeof(lfs_global_t)/4; i++) {
        a->u32[i] ^= b->u32[i];
    }
}

static inline bool lfs_global_iszero(const lfs_global_t *a) {
    for (int i = 0; i < sizeof(lfs_global_t)/4; i++) {
        if (a->u32[i] != 0) {
            return false;
        }
    }
    return true;
}

static inline void lfs_global_zero(lfs_global_t *a) {
    memset(a, 0, sizeof(lfs_global_t));
}

static inline void lfs_global_fromle32(lfs_global_t *a) {
    lfs_pair_fromle32(a->l.movepair);
    a->l.moveid = lfs_fromle16(a->l.moveid);
}

static inline void lfs_global_tole32(lfs_global_t *a) {
    lfs_pair_tole32(a->l.movepair);
    a->l.moveid = lfs_tole16(a->l.moveid);
}

static inline void lfs_global_move(lfs_t *lfs,
        const lfs_block_t pair[2], uint16_t id) {
    lfs_global_t diff;
    lfs_global_zero(&diff);
    diff.l.movepair[0] ^= lfs->globals.g.movepair[0] ^ pair[0];
    diff.l.movepair[1] ^= lfs->globals.g.movepair[1] ^ pair[1];
    diff.l.moveid      ^= lfs->globals.g.moveid      ^ id;
    lfs_global_fromle32(&lfs->locals);
    lfs_global_xor(&lfs->locals, &diff);
    lfs_global_tole32(&lfs->locals);
    lfs_global_xor(&lfs->globals, &diff);
}

static inline void lfs_global_orphans(lfs_t *lfs, int8_t orphans) {
    lfs->locals.l.deorphaned ^= (lfs->globals.g.orphans == 0);
    lfs->locals.l.deorphaned ^= (lfs->globals.g.orphans + orphans == 0);
    lfs->globals.g.orphans += orphans;
}

// other endianness operations
static void lfs_ctz_fromle32(struct lfs_ctz *ctz) {
    ctz->head = lfs_fromle32(ctz->head);
    ctz->size = lfs_fromle32(ctz->size);
}

static void lfs_ctz_tole32(struct lfs_ctz *ctz) {
    ctz->head = lfs_tole32(ctz->head);
    ctz->size = lfs_tole32(ctz->size);
}

static inline void lfs_superblock_fromle32(lfs_superblock_t *superblock) {
    superblock->version     = lfs_fromle32(superblock->version);
    superblock->block_size  = lfs_fromle32(superblock->block_size);
    superblock->block_count = lfs_fromle32(superblock->block_count);
    superblock->inline_max  = lfs_fromle32(superblock->inline_max);
    superblock->attr_max    = lfs_fromle32(superblock->attr_max);
    superblock->name_max    = lfs_fromle32(superblock->name_max);
}

static inline void lfs_superblock_tole32(lfs_superblock_t *superblock) {
    superblock->version     = lfs_tole32(superblock->version);
    superblock->block_size  = lfs_tole32(superblock->block_size);
    superblock->block_count = lfs_tole32(superblock->block_count);
    superblock->inline_max  = lfs_tole32(superblock->inline_max);
    superblock->attr_max    = lfs_tole32(superblock->attr_max);
    superblock->name_max    = lfs_tole32(superblock->name_max);
}


/// Internal operations predeclared here ///
static int lfs_fs_pred(lfs_t *lfs, const lfs_block_t dir[2],
        lfs_mdir_t *pdir);
static lfs_stag_t lfs_fs_parent(lfs_t *lfs, const lfs_block_t dir[2],
        lfs_mdir_t *parent);
static int lfs_fs_relocate(lfs_t *lfs,
        const lfs_block_t oldpair[2], lfs_block_t newpair[2]);
static int lfs_fs_forceconsistency(lfs_t *lfs);
static int lfs_deinit(lfs_t *lfs);


/// Block allocator ///
static int lfs_alloc_lookahead(void *p, lfs_block_t block) {
    lfs_t *lfs = (lfs_t*)p;
    lfs_block_t off = ((block - lfs->free.off)
            + lfs->cfg->block_count) % lfs->cfg->block_count;

    if (off < lfs->free.size) {
        lfs->free.buffer[off / 32] |= 1U << (off % 32);
    }

    return 0;
}

static int lfs_alloc(lfs_t *lfs, lfs_block_t *block) {
    while (true) {
        while (lfs->free.i != lfs->free.size) {
            lfs_block_t off = lfs->free.i;
            lfs->free.i += 1;
            lfs->free.ack -= 1;

            if (!(lfs->free.buffer[off / 32] & (1U << (off % 32)))) {
                // found a free block
                *block = (lfs->free.off + off) % lfs->cfg->block_count;

                // eagerly find next off so an alloc ack can
                // discredit old lookahead blocks
                while (lfs->free.i != lfs->free.size &&
                        (lfs->free.buffer[lfs->free.i / 32]
                            & (1U << (lfs->free.i % 32)))) {
                    lfs->free.i += 1;
                    lfs->free.ack -= 1;
                }

                return 0;
            }
        }

        // check if we have looked at all blocks since last ack
        if (lfs->free.ack == 0) {
            LFS_WARN("No more free space %"PRIu32,
                    lfs->free.i + lfs->free.off);
            return LFS_ERR_NOSPC;
        }

        lfs->free.off = (lfs->free.off + lfs->free.size)
                % lfs->cfg->block_count;
        lfs->free.size = lfs_min(lfs->cfg->lookahead, lfs->free.ack);
        lfs->free.i = 0;

        // find mask of free blocks from tree
        memset(lfs->free.buffer, 0, lfs->cfg->lookahead/8);
        int err = lfs_fs_traverse(lfs, lfs_alloc_lookahead, lfs);
        if (err) {
            return err;
        }
    }
}

static void lfs_alloc_ack(lfs_t *lfs) {
    lfs->free.ack = lfs->cfg->block_count;
}


/// Metadata pair and directory operations ///
static int lfs_dir_traverse(lfs_t *lfs,
        const lfs_mdir_t *dir, const struct lfs_mattr *attrs,
        lfs_tag_t matchmask, lfs_tag_t matchtag, lfs_stag_t matchdiff,
        int (*cb)(void *data, lfs_tag_t tag, const void *buffer), void *data) {
    lfs_block_t block = dir->pair[0];
    lfs_off_t off = dir->off;
    lfs_tag_t ntag = dir->etag;
    bool lastcommit = false;
    matchtag += matchdiff;

    // iterate over dir block backwards (for faster lookups)
    while (attrs || off >= sizeof(lfs_tag_t) + lfs_tag_dsize(ntag)) {
        lfs_tag_t tag;
        const void *buffer;
        struct lfs_diskoff disk;
        if (attrs) {
            tag = attrs->tag;
            buffer = attrs->buffer;
            attrs = attrs->next;
        } else {
            off -= lfs_tag_dsize(ntag);

            tag = ntag;
            buffer = &disk;
            disk.block = block;
            disk.off = off + sizeof(tag);

            int err = lfs_bd_read(lfs,
                    &lfs->pcache, &lfs->rcache, sizeof(ntag),
                    block, off, &ntag, sizeof(ntag));
            if (err) {
                return err;
            }

            ntag = lfs_fromle32(ntag) ^ tag;
            tag |= 0x80000000;
        }

        if (lfs_tag_subtype(tag) == LFS_TYPE_CRC) {
            lastcommit = 2 & lfs_tag_type(tag);
        } else if (lfs_tag_subtype(tag) == LFS_TYPE_DELETE) {
            // something was deleted, need to move around it
            if (lfs_tag_id(tag) <= lfs_tag_id(matchtag - matchdiff)) {
                matchdiff -= LFS_MKTAG(0, 1, 0);
            }
        }

        if ((tag & matchmask) == ((matchtag - matchdiff) & matchmask) &&
                !(lfs_tag_isdelete(tag) && lastcommit)) {
            int res = cb(data, tag + matchdiff, buffer);
            if (res) {
                return res;
            }
        }

        if (lfs_tag_subtype(tag) == LFS_TYPE_NAME) {
            // found where something was created
            if (lfs_tag_id(tag) == lfs_tag_id(matchtag - matchdiff)) {
                break;
            } else if (lfs_tag_id(tag) < lfs_tag_id(matchtag - matchdiff)) {
                matchdiff += LFS_MKTAG(0, 1, 0);
            }
        }
    }

    return 0;
}

static lfs_stag_t lfs_dir_fetchmatch(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2],        
        lfs_tag_t matchmask, lfs_tag_t matchtag,
        int (*cb)(void *data, lfs_tag_t tag, const void *buffer), void *data) {
    // find the block with the most recent revision
    uint32_t revs[2];
    int r = 0;
    for (int i = 0; i < 2; i++) {
        int err = lfs_bd_read(lfs,
                &lfs->pcache, &lfs->rcache, sizeof(revs[i]),
                pair[i], 0, &revs[i], sizeof(revs[i]));
        revs[i] = lfs_fromle32(revs[i]);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        if (lfs_scmp(revs[i], revs[(i+1)%2]) > 0 || err == LFS_ERR_CORRUPT) {
            r = i;
        }
    }

    // now fetch the actual dir (and find match)
    lfs_stag_t foundtag = LFS_ERR_NOENT;
    dir->pair[0] = pair[0];
    dir->pair[1] = pair[1];
    dir->off = 0;
    if (r != 0) {
        lfs_pair_swap(dir->pair);
        lfs_pair_swap(revs);
    }

    // scan tags and check crcs
    for (int i = 0; i < 2; i++) {
        lfs_block_t block = dir->pair[0];
        lfs_off_t off = sizeof(uint32_t);
        lfs_tag_t ptag = 0xffffffff;

        lfs_tag_t tempfoundtag = foundtag;
        lfs_mdir_t temp = {
            .pair = {dir->pair[0], dir->pair[1]},
            .rev = revs[0],
            .tail = {0xffffffff, 0xffffffff},
            .split = false,
            .count = 0,
        };
        lfs_global_zero(&temp.locals);

        temp.rev = lfs_tole32(temp.rev);
        uint32_t crc = lfs_crc(0xffffffff, &temp.rev, sizeof(temp.rev));
        temp.rev = lfs_fromle32(temp.rev);

        while (true) {
            // extract next tag
            lfs_tag_t tag;
            int err = lfs_bd_read(lfs,
                    &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
                    block, off, &tag, sizeof(tag));
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    // can't continue?
                    dir->erased = false;
                    break;
                }
                return err;
            }

            crc = lfs_crc(crc, &tag, sizeof(tag));
            tag = lfs_fromle32(tag) ^ ptag;

            // next commit not yet programmed
            if (!lfs_tag_isvalid(tag)) {
                dir->erased = (lfs_tag_subtype(ptag) == LFS_TYPE_CRC);
                break;
            }

            // check we're in valid range
            if (off + lfs_tag_dsize(tag) > lfs->cfg->block_size) {
                dir->erased = false;
                break;
            }

            if (lfs_tag_subtype(tag) == LFS_TYPE_CRC) {
                // check the crc attr
                uint32_t dcrc;
                err = lfs_bd_read(lfs,
                        &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
                        block, off+sizeof(tag), &dcrc, sizeof(dcrc));
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        dir->erased = false;
                        break;
                    }
                    return err;
                }
                dcrc = lfs_fromle32(dcrc);

                if (crc != dcrc) {
                    dir->erased = false;
                    break;
                }

                // reset the next bit if we need to
                tag ^= (lfs_tag_type(tag) & 1) << 31;
                lfs->seed ^= crc;
                crc = 0xffffffff;

                // update with what's found so far
                foundtag = tempfoundtag;
                *dir = temp;
                dir->off = off + lfs_tag_dsize(tag);
                dir->etag = tag;
            } else {
                // crc the entry first, leaving it in the cache
                for (lfs_off_t j = sizeof(tag); j < lfs_tag_dsize(tag); j++) {
                    uint8_t dat;
                    err = lfs_bd_read(lfs,
                            NULL, &lfs->rcache, lfs->cfg->block_size,
                            block, off+j, &dat, 1);
                    if (err) {
                        if (err == LFS_ERR_CORRUPT) {
                            dir->erased = false;
                            break;
                        }
                        return err;
                    }

                    crc = lfs_crc(crc, &dat, 1);
                }

                // check for special tags
                if (lfs_tag_subtype(tag) == LFS_TYPE_NAME) {
                    temp.count += 1;

                    if (lfs_tag_isvalid(tempfoundtag) &&
                            lfs_tag_id(tag) <= lfs_tag_id(tempfoundtag)) {
                        tempfoundtag += LFS_MKTAG(0, 1, 0);
                    }
                } else if (lfs_tag_subtype(tag) == LFS_TYPE_DELETE) {
                    LFS_ASSERT(temp.count > 0);
                    temp.count -= 1;

                    if (lfs_tag_id(tag) == lfs_tag_id(tempfoundtag)) {
                        tempfoundtag = LFS_ERR_NOENT;
                    } else if (lfs_tag_isvalid(tempfoundtag) &&
                            lfs_tag_id(tag) < lfs_tag_id(tempfoundtag)) {
                        tempfoundtag -= LFS_MKTAG(0, 1, 0);
                    }
                } else if (lfs_tag_subtype(tag) == LFS_TYPE_TAIL) {
                    temp.split = (lfs_tag_type(tag) & 1);
                    err = lfs_bd_read(lfs,
                            &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
                            block, off+sizeof(tag),
                            &temp.tail, sizeof(temp.tail));
                    if (err) {
                        if (err == LFS_ERR_CORRUPT) {
                            dir->erased = false;
                            break;
                        }
                    }
                    lfs_pair_fromle32(temp.tail);
                } else if (lfs_tag_subtype(tag) == LFS_TYPE_GLOBALS) {
                    temp.locals.l.deorphaned = (lfs_tag_type(tag) & 1);
                    err = lfs_bd_read(lfs,
                            &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
                            block, off+sizeof(tag),
                            &temp.locals, 10);
                    if (err) {
                        if (err == LFS_ERR_CORRUPT) {
                            dir->erased = false;
                            break;
                        }
                    }
                }

                if ((tag & matchmask) == (matchtag & matchmask)) {
                    // found a match?
                    if (lfs_tag_isdelete(tag)) {
                        tempfoundtag = LFS_ERR_NOENT;
                    } else if (cb) {
                        int res = cb(data, tag, &(struct lfs_diskoff){
                                block, off+sizeof(tag)});
                        if (res < 0) {
                            if (res == LFS_ERR_CORRUPT) {
                                dir->erased = false;
                                break;
                            }
                            return res;
                        }

                        if (res) {
                            tempfoundtag = tag;
                        }
                    }
                }
            }

            ptag = tag;
            off += lfs_tag_dsize(tag);
        }

        // consider what we have good enough
        if (dir->off > 0) {
            // synthetic move
            if (lfs_pair_cmp(dir->pair, lfs->globals.g.movepair) == 0) {
                if (lfs->globals.g.moveid == lfs_tag_id(foundtag)) {
                    foundtag = LFS_ERR_NOENT;
                } else if (lfs_tag_isvalid(foundtag) &&
                        lfs->globals.g.moveid < lfs_tag_id(foundtag)) {
                    foundtag -= LFS_MKTAG(0, 1, 0);
                }
            }

            return foundtag;
        }

        // failed, try the other crc?
        lfs_pair_swap(dir->pair);
        lfs_pair_swap(revs);
    }

    LFS_ERROR("Corrupted dir pair at %"PRIu32" %"PRIu32,
            dir->pair[0], dir->pair[1]);
    return LFS_ERR_CORRUPT;
}

static int lfs_dir_fetch(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2]) {
    lfs_stag_t res = lfs_dir_fetchmatch(lfs, dir, pair, 0, 0, NULL, NULL);
    if (res < 0 && res != LFS_ERR_NOENT) {
        return res;
    }

    return 0;
}

static lfs_stag_t lfs_dir_findmatch(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2], bool fs,
        lfs_tag_t findmask, lfs_tag_t findtag,
        int (*cb)(void *data, lfs_tag_t tag, const void *buffer), void *data) {
    dir->split = true;
    dir->tail[0] = pair[0];
    dir->tail[1] = pair[1];
    while ((dir->split || fs) && !lfs_pair_isnull(dir->tail)) {
        lfs_stag_t tag = lfs_dir_fetchmatch(lfs, dir, dir->tail,
                findmask, findtag, cb, data);
        if (tag != LFS_ERR_NOENT) {
            return tag;
        }
    }

    return LFS_ERR_NOENT;
}

struct lfs_dir_get_match {
    lfs_t *lfs;
    void *buffer;
    lfs_size_t size;
    bool compacting;
};

static int lfs_dir_get_match(void *data,
        lfs_tag_t tag, const void *buffer) {
    struct lfs_dir_get_match *get = data;
    lfs_t *lfs = get->lfs;
    const struct lfs_diskoff *disk = buffer;

    if (lfs_tag_isdelete(tag) && !get->compacting) {
        return LFS_ERR_NOENT;
    }

    if (get->buffer) {
        lfs_size_t diff = lfs_min(lfs_tag_size(tag), get->size);
        int err = lfs_bd_read(lfs,
                &lfs->pcache, &lfs->rcache, diff,
                disk->block, disk->off, get->buffer, diff);
        if (err) {
            return err;
        }

        memset((uint8_t*)get->buffer + diff, 0, get->size - diff);
    }

    return tag & 0x7fffffff;
}

static lfs_stag_t lfs_dir_get(lfs_t *lfs, lfs_mdir_t *dir,
        lfs_tag_t getmask, lfs_tag_t gettag, void *buffer) {
    lfs_stag_t getdiff = 0;
    if (lfs_pair_cmp(dir->pair, lfs->globals.g.movepair) == 0 &&
            lfs_tag_id(gettag) <= lfs->globals.g.moveid) {
        // synthetic moves
        gettag += LFS_MKTAG(0, 1, 0);
        getdiff -= LFS_MKTAG(0, 1, 0);
    }

    lfs_stag_t res = lfs_dir_traverse(lfs, dir, NULL,
            getmask, gettag, getdiff,
            lfs_dir_get_match, &(struct lfs_dir_get_match){
                lfs, buffer, lfs_tag_size(gettag)});
    if (res < 0) {
        return res;
    }

    return res ? res : LFS_ERR_NOENT;
}

// commit logic
struct lfs_commit {
    lfs_block_t block;
    lfs_off_t off;
    lfs_tag_t ptag;
    uint32_t crc;

    lfs_off_t begin;
    lfs_off_t end;
    lfs_off_t ack;
};

static int lfs_commit_prog(lfs_t *lfs, struct lfs_commit *commit,
        const void *buffer, lfs_size_t size) {
    lfs_off_t skip = lfs_min(lfs_max(commit->ack, commit->off)
            - commit->off, size);
    int err = lfs_bd_prog(lfs,
            &lfs->pcache, &lfs->rcache, false,
            commit->block, commit->off + skip,
            (const uint8_t*)buffer + skip, size - skip);
    if (err) {
        return err;
    }

    commit->crc = lfs_crc(commit->crc, buffer, size);
    commit->off += size;
    commit->ack = lfs_max(commit->off, commit->ack);
    return 0;
}

static int lfs_commit_attr(lfs_t *lfs, struct lfs_commit *commit,
        lfs_tag_t tag, const void *buffer);

struct lfs_commit_move_match {
    lfs_t *lfs;
    struct lfs_commit *commit;
    int pass;
};

static int lfs_commit_move_match(void *data,
        lfs_tag_t tag, const void *buffer) {
    struct lfs_commit_move_match *move = data;
    lfs_t *lfs = move->lfs;
    struct lfs_commit *commit = move->commit;

    if (move->pass == 0) {
        if (lfs_tag_subtype(tag) != LFS_TYPE_NAME) {
            return 0;
        }
    } else {
        // check if type has already been committed
        lfs_stag_t res = lfs_dir_traverse(lfs, &(const lfs_mdir_t){
                    .pair[0] = commit->block,
                    .off = commit->off,
                    .etag = commit->ptag}, NULL,
                lfs_tag_isuser(tag) ? 0x7ffff000 : 0x7c3ff000, tag, 0,
                lfs_dir_get_match, &(struct lfs_dir_get_match){
                    lfs, NULL, 0, true});
        if (res < 0 && res != LFS_ERR_NOENT) {
            return res;
        }

        if (res > 0) {
            return 0;
        }
    }

    // update id and commit, as we are currently unique
    return lfs_commit_attr(lfs, commit, tag, buffer);
}

static int lfs_commit_move(lfs_t *lfs, struct lfs_commit *commit, int pass,
        lfs_tag_t frommask, lfs_tag_t fromtag, lfs_stag_t fromdiff,
        const lfs_mdir_t *dir, const struct lfs_mattr *attrs) {
    return lfs_dir_traverse(lfs, dir, attrs,
            frommask, fromtag, fromdiff,
            lfs_commit_move_match, &(struct lfs_commit_move_match){
                lfs, commit, pass});
}

static int lfs_commit_userattrs(lfs_t *lfs, struct lfs_commit *commit,
        uint16_t id, const struct lfs_attr *attrs) {
    for (const struct lfs_attr *a = attrs; a; a = a->next) {
        int err = lfs_commit_attr(lfs, commit,
                LFS_MKTAG(0x100 | a->type, id, a->size), a->buffer);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_commit_attr(lfs_t *lfs, struct lfs_commit *commit,
        lfs_tag_t tag, const void *buffer) {
    if (lfs_tag_type(tag) == LFS_FROM_MOVE) {
        // special case for moves
        return lfs_commit_move(lfs, commit, 1,
                0x003ff000, LFS_MKTAG(0, lfs_tag_size(tag), 0),
                LFS_MKTAG(0, lfs_tag_id(tag), 0) -
                    LFS_MKTAG(0, lfs_tag_size(tag), 0),
                buffer, NULL);
    } else if (lfs_tag_type(tag) == LFS_FROM_USERATTRS) {
        // special case for custom attributes
        return lfs_commit_userattrs(lfs, commit,
                lfs_tag_id(tag), buffer);
    }

    // check if we fit
    lfs_size_t dsize = lfs_tag_dsize(tag);
    if (commit->off + dsize > commit->end) {
        return LFS_ERR_NOSPC;
    }

    // write out tag
    lfs_tag_t ntag = lfs_tole32((tag & 0x7fffffff) ^ commit->ptag);
    int err = lfs_commit_prog(lfs, commit, &ntag, sizeof(ntag));
    if (err) {
        return err;
    }

    if (!(tag & 0x80000000)) {
        // from memory
        err = lfs_commit_prog(lfs, commit, buffer, dsize-sizeof(tag));
        if (err) {
            return err;
        }
    } else {
        // from disk
        const struct lfs_diskoff *disk = buffer;
        for (lfs_off_t i = 0; i < dsize-sizeof(tag); i++) {
            // rely on caching to make this efficient
            uint8_t dat;
            err = lfs_bd_read(lfs,
                    &lfs->pcache, &lfs->rcache, dsize-sizeof(tag)-i,
                    disk->block, disk->off+i, &dat, 1);
            if (err) {
                return err;
            }

            err = lfs_commit_prog(lfs, commit, &dat, 1);
            if (err) {
                return err;
            }
        }
    }

    commit->ptag = tag & 0x7fffffff;
    return 0;
}

static int lfs_commit_globals(lfs_t *lfs, struct lfs_commit *commit,
        lfs_global_t *locals) {
    if (lfs_global_iszero(&lfs->locals)) {
        return 0;
    }

    lfs_global_xor(locals, &lfs->locals);
    int err = lfs_commit_attr(lfs, commit,
            LFS_MKTAG(LFS_TYPE_GLOBALS + locals->l.deorphaned, 0x3ff, 10),
            locals);
    lfs_global_xor(locals, &lfs->locals);
    return err;
}

static int lfs_commit_crc(lfs_t *lfs, struct lfs_commit *commit,
        bool compacting) {
    // align to program units
    lfs_off_t off = lfs_alignup(commit->off + 2*sizeof(uint32_t),
            lfs->cfg->prog_size);

    // read erased state from next program unit
    lfs_tag_t tag;
    int err = lfs_bd_read(lfs,
            &lfs->pcache, &lfs->rcache, sizeof(tag),
            commit->block, off, &tag, sizeof(tag));
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }

    // build crc tag
    bool reset = ~lfs_fromle32(tag) >> 31;
    tag = LFS_MKTAG(LFS_TYPE_CRC + (compacting << 1) + reset,
            0x3ff, off - (commit->off+sizeof(lfs_tag_t)));

    // write out crc
    uint32_t footer[2];
    footer[0] = lfs_tole32(tag ^ commit->ptag);
    commit->crc = lfs_crc(commit->crc, &footer[0], sizeof(footer[0]));
    footer[1] = lfs_tole32(commit->crc);
    err = lfs_bd_prog(lfs,
            &lfs->pcache, &lfs->rcache, false,
            commit->block, commit->off, &footer, sizeof(footer));
    if (err) {
        return err;
    }
    commit->off += sizeof(tag)+lfs_tag_size(tag);
    commit->ptag = tag ^ (reset << 31);

    // flush buffers
    err = lfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
    if (err) {
        return err;
    }

    // successful commit, check checksum to make sure
    uint32_t crc = 0xffffffff;
    lfs_size_t size = commit->off - lfs_tag_size(tag) - commit->begin;
    for (lfs_off_t i = 0; i < size; i++) {
        // leave it up to caching to make this efficient
        uint8_t dat;
        err = lfs_bd_read(lfs,
                NULL, &lfs->rcache, size-i,
                commit->block, commit->begin+i, &dat, 1);
        if (err) {
            return err;
        }

        crc = lfs_crc(crc, &dat, 1);
    }

    if (err) {
        return err;
    }

    if (crc != commit->crc) {
        return LFS_ERR_CORRUPT;
    }

    return 0;
}

static int lfs_dir_alloc(lfs_t *lfs, lfs_mdir_t *dir) {
    // allocate pair of dir blocks (backwards, so we write block 1 first)
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[(i+1)%2]);
        if (err) {
            return err;
        }
    }

    // rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs_bd_read(lfs,
            &lfs->pcache, &lfs->rcache, sizeof(dir->rev),
            dir->pair[0], 0, &dir->rev, sizeof(dir->rev));
    if (err) {
        return err;
    }

    dir->rev = lfs_fromle32(dir->rev);
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }

    // set defaults
    dir->off = sizeof(dir->rev);
    dir->etag = 0xffffffff;
    dir->count = 0;
    dir->tail[0] = 0xffffffff;
    dir->tail[1] = 0xffffffff;
    dir->erased = false;
    dir->split = false;
    lfs_global_zero(&dir->locals);

    // don't write out yet, let caller take care of that
    return 0;
}

static int lfs_dir_compact(lfs_t *lfs,
        lfs_mdir_t *dir, const struct lfs_mattr *attrs,
        lfs_mdir_t *source, uint16_t begin, uint16_t end) {
    // save some state in case block is bad
    const lfs_block_t oldpair[2] = {dir->pair[1], dir->pair[0]};
    bool relocated = false;

    // There's nothing special about our global delta, so feed it back
    // into the global global delta
    lfs_global_xor(&lfs->locals, &dir->locals);
    lfs_global_zero(&dir->locals);

    while (true) {
        // setup compaction
        bool splitted = false;
        bool exhausted = false;

        struct lfs_commit commit;
        commit.block = dir->pair[1];
        commit.ack = 0;

commit:
        // setup erase state
        exhausted = false;
        dir->count = end - begin;
        int16_t ackid = -1;

        // setup commit state
        commit.off = 0;
        commit.crc = 0xffffffff;
        commit.ptag = 0xffffffff;

        // space is complicated, we need room for tail, crc, globals,
        // cleanup delete, and we cap at half a block to give room
        // for metadata updates
        commit.begin = 0;
        commit.end = lfs_min(
             lfs_alignup(lfs->cfg->block_size/2, lfs->cfg->prog_size),
             lfs->cfg->block_size - 38);

        if (!splitted) {
            // increment revision count
            dir->rev += 1;
            if (lfs->cfg->block_cycles &&
                    dir->rev % lfs->cfg->block_cycles == 0) {
                if (lfs_pair_cmp(dir->pair,
                        (const lfs_block_t[2]){0, 1}) == 0) {
                    // we're writing too much to the superblock,
                    // should we expand?
                    lfs_ssize_t res = lfs_fs_size(lfs);
                    if (res < 0) {
                        return res;
                    }

                    // do we have enough space to expand?
                    if (res < lfs->cfg->block_count/2) {
                        LFS_DEBUG("Expanding superblock at rev %"PRIu32,
                                dir->rev);
                        exhausted = true;
                        goto split;
                    }
                } else {
                    // we're writing too much, time to relocate
                    exhausted = true;
                    goto relocate;
                }
            }

            // erase block to write to
            int err = lfs_bd_erase(lfs, dir->pair[1]);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        // write out header
        uint32_t rev = lfs_tole32(dir->rev);
        int err = lfs_commit_prog(lfs, &commit, &rev, sizeof(rev));
        if (err) {
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // commit with a move
        for (uint16_t id = begin; id < end || commit.off < commit.ack; id++) {
            for (int pass = 0; pass < 2; pass++) {
                err = lfs_commit_move(lfs, &commit, pass,
                        0x003ff000, LFS_MKTAG(0, id, 0),
                        -LFS_MKTAG(0, begin, 0),
                        source, attrs);
                if (err && !(splitted && err == LFS_ERR_NOSPC)) {
                    if (err == LFS_ERR_NOSPC) {
                        goto split;
                    } else if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            ackid = id;
        }

        // reopen reserved space at the end
        commit.end = lfs->cfg->block_size - 8;

        if (ackid >= end) {
            // extra garbage attributes were written out during split,
            // need to clean up
            err = lfs_commit_attr(lfs, &commit,
                    LFS_MKTAG(LFS_TYPE_DELETE, ackid, 0), NULL);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        if (!relocated) {
            // commit any globals, unless we're relocating,
            // in which case our parent will steal our globals
            err = lfs_commit_globals(lfs, &commit, &dir->locals);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        if (!lfs_pair_isnull(dir->tail)) {
            // commit tail, which may be new after last size check
            lfs_pair_tole32(dir->tail);
            err = lfs_commit_attr(lfs, &commit,
                    LFS_MKTAG(LFS_TYPE_TAIL + dir->split,
                        0x3ff, sizeof(dir->tail)), dir->tail);
            lfs_pair_fromle32(dir->tail);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        err = lfs_commit_crc(lfs, &commit, true);
        if (err) {
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // successful compaction, swap dir pair to indicate most recent
        lfs_pair_swap(dir->pair);
        dir->off = commit.off;
        dir->etag = commit.ptag;
        dir->erased = true;
        break;

split:
        // commit no longer fits, need to split dir,
        // drop caches and create tail
        splitted = !exhausted;
        if (lfs->pcache.block != 0xffffffff) {
            commit.ack -= lfs->pcache.size;
            lfs_cache_drop(lfs, &lfs->pcache);
        }

        if (!exhausted && ackid < 0) {
            // If we can't fit in this block, we won't fit in next block
            return LFS_ERR_NOSPC;
        }

        lfs_mdir_t tail;
        err = lfs_dir_alloc(lfs, &tail);
        if (err) {
            return err;
        }

        tail.split = dir->split;
        tail.tail[0] = dir->tail[0];
        tail.tail[1] = dir->tail[1];

        err = lfs_dir_compact(lfs, &tail, attrs, source, ackid+1, end);
        if (err) {
            return err;
        }

        end = ackid+1;
        dir->tail[0] = tail.pair[0];
        dir->tail[1] = tail.pair[1];
        dir->split = true;

        if (exhausted) {
            lfs->root[0] = tail.pair[0];
            lfs->root[1] = tail.pair[1];
        }

        goto commit;

relocate:
        // commit was corrupted, drop caches and prepare to relocate block
        relocated = true;
        lfs_cache_drop(lfs, &lfs->pcache);
        if (!exhausted) {
            LFS_DEBUG("Bad block at %"PRIu32, dir->pair[1]);
        }

        // can't relocate superblock, filesystem is now frozen
        if (lfs_pair_cmp(oldpair, (const lfs_block_t[2]){0, 1}) == 0) {
            LFS_WARN("Superblock %"PRIu32" has become unwritable", oldpair[1]);
            return LFS_ERR_NOSPC;
        }

        // relocate half of pair
        err = lfs_alloc(lfs, &dir->pair[1]);
        if (err && (err != LFS_ERR_NOSPC && !exhausted)) {
            return err;
        }

        continue;
    }

    if (!relocated) {
        // successful commit, update globals
        lfs_global_xor(&dir->locals, &lfs->locals);
        lfs_global_zero(&lfs->locals);
    } else {
        // update references if we relocated
        LFS_DEBUG("Relocating %"PRIu32" %"PRIu32" to %"PRIu32" %"PRIu32,
                oldpair[0], oldpair[1], dir->pair[0], dir->pair[1]);
        int err = lfs_fs_relocate(lfs, oldpair, dir->pair);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_dir_commit(lfs_t *lfs, lfs_mdir_t *dir,
        const struct lfs_mattr *attrs) {
    struct lfs_mattr cancelattr;
    lfs_global_t canceldiff;
    lfs_global_zero(&canceldiff);
    if (lfs_pair_cmp(dir->pair, lfs->globals.g.movepair) == 0) {
        // Wait, we have the move? Just cancel this out here
        // We need to, or else the move can become outdated
        canceldiff.l.movepair[0] ^= lfs->globals.g.movepair[0] ^ 0xffffffff;
        canceldiff.l.movepair[1] ^= lfs->globals.g.movepair[1] ^ 0xffffffff;
        canceldiff.l.moveid      ^= lfs->globals.g.moveid      ^ 0x3ff;
        lfs_global_fromle32(&lfs->locals);
        lfs_global_xor(&lfs->locals, &canceldiff);
        lfs_global_tole32(&lfs->locals);

        cancelattr.tag = LFS_MKTAG(LFS_TYPE_DELETE, lfs->globals.l.moveid, 0);
        cancelattr.next = attrs;
        attrs = &cancelattr;
    }

    // calculate new directory size
    lfs_tag_t deletetag = 0xffffffff;
    int attrcount = 0;
    for (const struct lfs_mattr *a = attrs; a; a = a->next) {
        if (lfs_tag_subtype(a->tag) == LFS_TYPE_NAME) {
            dir->count += 1;
        } else if (lfs_tag_subtype(a->tag) == LFS_TYPE_DELETE) {
            LFS_ASSERT(dir->count > 0);
            dir->count -= 1;
            deletetag = a->tag;

            if (dir->count == 0) {
                // should we actually drop the directory block?
                lfs_mdir_t pdir;
                int err = lfs_fs_pred(lfs, dir->pair, &pdir);
                if (err && err != LFS_ERR_NOENT) {
                    return err;
                }

                if (err != LFS_ERR_NOENT && pdir.split) {
                    // steal tail and global state
                    pdir.split = dir->split;
                    pdir.tail[0] = dir->tail[0];
                    pdir.tail[1] = dir->tail[1];
                    lfs_global_xor(&lfs->locals, &dir->locals);
                    return lfs_dir_commit(lfs, &pdir,
                            LFS_MKATTR(LFS_TYPE_TAIL + pdir.split, 0x3ff,
                                pdir.tail, sizeof(pdir.tail),
                            NULL));
                }
            }
        }

        attrcount += 1;
    }

    while (true) {
        if (!dir->erased) {
            goto compact;
        }

        // try to commit
        struct lfs_commit commit = {
            .block = dir->pair[0],
            .off = dir->off,
            .crc = 0xffffffff,
            .ptag = dir->etag,

            .begin = dir->off,
            .end = lfs->cfg->block_size - 8,
            .ack = 0,
        };

        // iterate over commits backwards, this lets us "append" commits
        for (int i = 0; i < attrcount; i++) {
            const struct lfs_mattr *a = attrs;
            for (int j = 0; j < attrcount-i-1; j++) {
                a = a->next;
            }

            lfs_pair_tole32(dir->tail);
            int err = lfs_commit_attr(lfs, &commit, a->tag, a->buffer);
            lfs_pair_fromle32(dir->tail);
            if (err) {
                if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                    goto compact;
                }
                return err;
            }
        }

        int err = lfs_commit_globals(lfs, &commit, &dir->locals);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        err = lfs_commit_crc(lfs, &commit, false);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        // successful commit, update dir
        dir->off = commit.off;
        dir->etag = commit.ptag;
        // successful commit, update globals
        lfs_global_xor(&dir->locals, &lfs->locals);
        lfs_global_zero(&lfs->locals);
        break;

compact:
        // fall back to compaction
        lfs_cache_drop(lfs, &lfs->pcache);
        err = lfs_dir_compact(lfs, dir, attrs, dir, 0, dir->count);
        if (err) {
            return err;
        }

        break;
    }

    // update globals that are affected
    lfs_global_xor(&lfs->globals, &canceldiff);

    // update any directories that are affected
    lfs_mdir_t copy = *dir;

    // two passes, once for things that aren't us, and one
    // for things that are
    for (struct lfs_mlist *d = lfs->mlist; d; d = d->next) {
        if (lfs_pair_cmp(d->m.pair, copy.pair) == 0) {
            d->m = *dir;
            if (d->id == lfs_tag_id(deletetag)) {
                d->m.pair[0] = 0xffffffff;
                d->m.pair[1] = 0xffffffff;
            } else if (d->id > lfs_tag_id(deletetag)) {
                d->id -= 1;
                if (d->type == LFS_TYPE_DIR) {
                    ((lfs_dir_t*)d)->pos -= 1;
                }
            }

            while (d->id >= d->m.count && d->m.split) {
                // we split and id is on tail now
                d->id -= d->m.count;
                int err = lfs_dir_fetch(lfs, &d->m, d->m.tail);
                if (err) {
                    return err;
                }
            }
        }
    }

    return 0;
}

struct lfs_dir_find_match {
    lfs_t *lfs;
    const void *name;
    lfs_size_t size;
};

static int lfs_dir_find_match(void *data,
        lfs_tag_t tag, const void *buffer) {
    struct lfs_dir_find_match *name = data;
    lfs_t *lfs = name->lfs;
    const struct lfs_diskoff *disk = buffer;
    (void)tag;

    return lfs_bd_cmp(lfs,
            NULL, &lfs->rcache, name->size,
            disk->block, disk->off, name->name, name->size);
}

static lfs_stag_t lfs_dir_find(lfs_t *lfs,
        lfs_mdir_t *dir, const char **path) {
    // we reduce path to a single name if we can find it
    const char *name = *path;
    *path = NULL;

    // default to root dir
    lfs_stag_t tag = LFS_MKTAG(LFS_TYPE_DIR, 0x3ff, 0);
    lfs_block_t pair[2] = {lfs->root[0], lfs->root[1]};

    while (true) {
nextname:
        // skip slashes
        name += strspn(name, "/");
        lfs_size_t namelen = strcspn(name, "/");

        // skip '.' and root '..'
        if ((namelen == 1 && memcmp(name, ".", 1) == 0) ||
            (namelen == 2 && memcmp(name, "..", 2) == 0)) {
            name += namelen;
            goto nextname;
        }

        // skip if matched by '..' in name
        const char *suffix = name + namelen;
        lfs_size_t sufflen;
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
                    name = suffix + sufflen;
                    goto nextname;
                }
            } else {
                depth += 1;
            }

            suffix += sufflen;
        }

        // found path
        if (name[0] == '\0') {
            return tag;
        }

        // update what we've found if path is only a name
        if (strchr(name, '/') == NULL) {
            *path = name;
        }

        // only continue if we hit a directory
        if (lfs_tag_type(tag) != LFS_TYPE_DIR) {
            return LFS_ERR_NOTDIR;
        }

        // grab the entry data
        if (lfs_tag_id(tag) != 0x3ff) {
            lfs_stag_t res = lfs_dir_get(lfs, dir, 0x7c3ff000,
                    LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), pair);
            if (res < 0) {
                return res;
            }
            lfs_pair_fromle32(pair);
        }

        // find entry matching name
        tag = lfs_dir_findmatch(lfs, dir, pair, false, 0x7c000fff,
                LFS_MKTAG(LFS_TYPE_NAME, 0, namelen),
                lfs_dir_find_match, &(struct lfs_dir_find_match){
                    lfs, name, namelen});
        if (tag < 0) {
            return tag;
        }

        // to next name
        name += namelen;
    }
}

static int lfs_dir_getinfo(lfs_t *lfs, lfs_mdir_t *dir,
        uint16_t id, struct lfs_info *info) {
    if (id == 0x3ff) {
        // special case for root
        strcpy(info->name, "/");
        info->type = LFS_TYPE_DIR;
        return 0;
    }

    lfs_stag_t tag = lfs_dir_get(lfs, dir, 0x7c3ff000,
            LFS_MKTAG(LFS_TYPE_NAME, id, lfs->name_max+1), info->name);
    if (tag < 0) {
        return tag;
    }

    info->type = lfs_tag_type(tag);

    struct lfs_ctz ctz;
    tag = lfs_dir_get(lfs, dir, 0x7c3ff000,
            LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
    if (tag < 0) {
        return tag;
    }
    lfs_ctz_fromle32(&ctz);

    if (lfs_tag_type(tag) == LFS_TYPE_CTZSTRUCT) {
        info->size = ctz.size;
    } else if (lfs_tag_type(tag) == LFS_TYPE_INLINESTRUCT) {
        info->size = lfs_tag_size(tag);
    }

    return 0;
}

/// Top level directory operations ///
int lfs_mkdir(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    lfs_mdir_t cwd;
    lfs_stag_t res = lfs_dir_find(lfs, &cwd, &path);
    if (!(res == LFS_ERR_NOENT && path)) {
        return (res < 0) ? res : LFS_ERR_EXIST;
    }

    // check that name fits
    lfs_size_t nlen = strlen(path);
    if (nlen > lfs->name_max) {
        return LFS_ERR_NAMETOOLONG;
    }

    // build up new directory
    lfs_alloc_ack(lfs);

    lfs_mdir_t dir;
    err = lfs_dir_alloc(lfs, &dir);
    if (err) {
        return err;
    }

    dir.tail[0] = cwd.tail[0];
    dir.tail[1] = cwd.tail[1];
    err = lfs_dir_commit(lfs, &dir, NULL);
    if (err) {
        return err;
    }

    // get next slot and commit
    uint16_t id = cwd.count;
    cwd.tail[0] = dir.pair[0];
    cwd.tail[1] = dir.pair[1];
    lfs_pair_tole32(dir.pair);
    err = lfs_dir_commit(lfs, &cwd,
            LFS_MKATTR(LFS_TYPE_SOFTTAIL, 0x3ff, cwd.tail, sizeof(cwd.tail),
            LFS_MKATTR(LFS_TYPE_DIRSTRUCT, id, dir.pair, sizeof(dir.pair),
            LFS_MKATTR(LFS_TYPE_DIR, id, path, nlen,
            NULL))));
    lfs_pair_fromle32(dir.pair);
    if (err) {
        return err;
    }

    return 0;
}

int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
    lfs_stag_t tag = lfs_dir_find(lfs, &dir->m, &path);
    if (tag < 0) {
        return tag;
    }

    if (lfs_tag_type(tag) != LFS_TYPE_DIR) {
        return LFS_ERR_NOTDIR;
    }

    lfs_block_t pair[2];
    if (lfs_tag_id(tag) == 0x3ff) {
        // handle root dir separately
        pair[0] = lfs->root[0];
        pair[1] = lfs->root[1];
    } else {
        // get dir pair from parent
        lfs_stag_t res = lfs_dir_get(lfs, &dir->m, 0x7c3ff000,
                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), pair);
        if (res < 0) {
            return res;
        }
        lfs_pair_fromle32(pair);
    }

    // fetch first pair
    int err = lfs_dir_fetch(lfs, &dir->m, pair);
    if (err) {
        return err;
    }

    // setup entry
    dir->head[0] = dir->m.pair[0];
    dir->head[1] = dir->m.pair[1];
    dir->id = 0;
    dir->pos = 0;

    // add to list of mdirs
    dir->type = LFS_TYPE_DIR;
    dir->next = (lfs_dir_t*)lfs->mlist;
    lfs->mlist = (struct lfs_mlist*)dir;

    return 0;
}

int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
    // remove from list of mdirs
    for (struct lfs_mlist **p = &lfs->mlist; *p; p = &(*p)->next) {
        if (*p == (struct lfs_mlist*)dir) {
            *p = (*p)->next;
            break;
        }
    }

    return 0;
}

int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info) {
    memset(info, 0, sizeof(*info));

    // special offset for '.' and '..'
    if (dir->pos == 0) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, ".");
        dir->pos += 1;
        return 1;
    } else if (dir->pos == 1) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, "..");
        dir->pos += 1;
        return 1;
    }

    while (true) {
        if (dir->id == dir->m.count) {
            if (!dir->m.split) {
                return false;
            }

            int err = lfs_dir_fetch(lfs, &dir->m, dir->m.tail);
            if (err) {
                return err;
            }

            dir->id = 0;
        }

        int err = lfs_dir_getinfo(lfs, &dir->m, dir->id, info);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        dir->id += 1;
        if (err != LFS_ERR_NOENT) {
            break;
        }
    }

    dir->pos += 1;
    return true;
}

int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off) {
    // simply walk from head dir
    int err = lfs_dir_rewind(lfs, dir);
    if (err) {
        return err;
    }

    // first two for ./..
    dir->pos = lfs_min(2, off);
    off -= dir->pos;

    while (off != 0) {
        dir->id = lfs_min(dir->m.count, off);
        dir->pos += dir->id;
        off -= dir->id;

        if (dir->id == dir->m.count) {
            if (!dir->m.split) {
                return LFS_ERR_INVAL;
            }

            err = lfs_dir_fetch(lfs, &dir->m, dir->m.tail);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir) {
    (void)lfs;
    return dir->pos;
}

int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir) {
    // reload the head dir
    int err = lfs_dir_fetch(lfs, &dir->m, dir->head);
    if (err) {
        return err;
    }

    dir->m.pair[0] = dir->head[0];
    dir->m.pair[1] = dir->head[1];
    dir->id = 0;
    dir->pos = 0;
    return 0;
}


/// File index list operations ///
static int lfs_ctz_index(lfs_t *lfs, lfs_off_t *off) {
    lfs_off_t size = *off;
    lfs_off_t b = lfs->cfg->block_size - 2*4;
    lfs_off_t i = size / b;
    if (i == 0) {
        return 0;
    }

    i = (size - 4*(lfs_popc(i-1)+2)) / b;
    *off = size - b*i - 4*lfs_popc(i);
    return i;
}

static int lfs_ctz_find(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache,
        lfs_block_t head, lfs_size_t size,
        lfs_size_t pos, lfs_block_t *block, lfs_off_t *off) {
    if (size == 0) {
        *block = 0xffffffff;
        *off = 0;
        return 0;
    }

    lfs_off_t current = lfs_ctz_index(lfs, &(lfs_off_t){size-1});
    lfs_off_t target = lfs_ctz_index(lfs, &pos);

    while (current > target) {
        lfs_size_t skip = lfs_min(
                lfs_npw2(current-target+1) - 1,
                lfs_ctz(current));

        int err = lfs_bd_read(lfs,
                pcache, rcache, sizeof(head),
                head, 4*skip, &head, sizeof(head));
        head = lfs_fromle32(head);
        if (err) {
            return err;
        }

        LFS_ASSERT(head >= 2 && head <= lfs->cfg->block_count);
        current -= 1 << skip;
    }

    *block = head;
    *off = pos;
    return 0;
}

static int lfs_ctz_extend(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache,
        lfs_block_t head, lfs_size_t size,
        lfs_block_t *block, lfs_off_t *off) {
    while (true) {
        // go ahead and grab a block
        lfs_block_t nblock;
        int err = lfs_alloc(lfs, &nblock);
        if (err) {
            return err;
        }
        LFS_ASSERT(nblock >= 2 && nblock <= lfs->cfg->block_count);

        err = lfs_bd_erase(lfs, nblock);
        if (err) {
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        if (size == 0) {
            *block = nblock;
            *off = 0;
            return 0;
        }

        size -= 1;
        lfs_off_t index = lfs_ctz_index(lfs, &size);
        size += 1;

        // just copy out the last block if it is incomplete
        if (size != lfs->cfg->block_size) {
            for (lfs_off_t i = 0; i < size; i++) {
                uint8_t data;
                err = lfs_bd_read(lfs,
                        NULL, rcache, size-i,
                        head, i, &data, 1);
                if (err) {
                    return err;
                }

                err = lfs_bd_prog(lfs,
                        pcache, rcache, true,
                        nblock, i, &data, 1);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            *block = nblock;
            *off = size;
            return 0;
        }

        // append block
        index += 1;
        lfs_size_t skips = lfs_ctz(index) + 1;

        for (lfs_off_t i = 0; i < skips; i++) {
            head = lfs_tole32(head);
            err = lfs_bd_prog(lfs, pcache, rcache, true,
                    nblock, 4*i, &head, 4);
            head = lfs_fromle32(head);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            if (i != skips-1) {
                err = lfs_bd_read(lfs,
                        NULL, rcache, sizeof(head),
                        head, 4*i, &head, sizeof(head));
                head = lfs_fromle32(head);
                if (err) {
                    return err;
                }
            }

            LFS_ASSERT(head >= 2 && head <= lfs->cfg->block_count);
        }

        *block = nblock;
        *off = 4*skips;
        return 0;

relocate:
        LFS_DEBUG("Bad block at %"PRIu32, nblock);

        // just clear cache and try a new block
        lfs_cache_drop(lfs, pcache);
    }
}

static int lfs_ctz_traverse(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache,
        lfs_block_t head, lfs_size_t size,
        int (*cb)(void*, lfs_block_t), void *data) {
    if (size == 0) {
        return 0;
    }

    lfs_off_t index = lfs_ctz_index(lfs, &(lfs_off_t){size-1});

    while (true) {
        int err = cb(data, head);
        if (err) {
            return err;
        }

        if (index == 0) {
            return 0;
        }

        lfs_block_t heads[2];
        int count = 2 - (index & 1);
        err = lfs_bd_read(lfs,
                pcache, rcache, count*sizeof(head),
                head, 0, &heads, count*sizeof(head));
        heads[0] = lfs_fromle32(heads[0]);
        heads[1] = lfs_fromle32(heads[1]);
        if (err) {
            return err;
        }

        for (int i = 0; i < count-1; i++) {
            err = cb(data, heads[i]);
            if (err) {
                return err;
            }
        }

        head = heads[count-1];
        index -= count;
    }
}


/// Top level file operations ///
int lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags,
        const struct lfs_file_config *cfg) {
    // deorphan if we haven't yet, needed at most once after poweron
    if ((flags & 3) != LFS_O_RDONLY) {
        int err = lfs_fs_forceconsistency(lfs);
        if (err) {
            return err;
        }
    }

    // setup simple file details
    int err = 0;
    file->cfg = cfg;
    file->flags = flags;
    file->pos = 0;
    file->cache.buffer = NULL;

    // allocate entry for file if it doesn't exist
    lfs_stag_t tag = lfs_dir_find(lfs, &file->m, &path);
    if (tag < 0 && !(tag == LFS_ERR_NOENT && path)) {
        err = tag;
        goto cleanup;
    }

    // get id, add to list of mdirs to catch update changes
    file->id = lfs_tag_id(tag);
    file->type = LFS_TYPE_REG;
    file->next = (lfs_file_t*)lfs->mlist;
    lfs->mlist = (struct lfs_mlist*)file;

    if (tag == LFS_ERR_NOENT) {
        if (!(flags & LFS_O_CREAT)) {
            err = LFS_ERR_NOENT;
            goto cleanup;
        }

        // check that name fits
        lfs_size_t nlen = strlen(path);
        if (nlen > lfs->name_max) {
            err = LFS_ERR_NAMETOOLONG;
            goto cleanup;
        }

        // get next slot and create entry to remember name
        file->id = file->m.count;
        err = lfs_dir_commit(lfs, &file->m,
                LFS_MKATTR(LFS_TYPE_INLINESTRUCT, file->id, NULL, 0,
                LFS_MKATTR(LFS_TYPE_REG, file->id, path, nlen,
                NULL)));
        if (err) {
            err = LFS_ERR_NAMETOOLONG;
            goto cleanup;
        }

        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, 0);
    } else if (flags & LFS_O_EXCL) {
        err = LFS_ERR_EXIST;
        goto cleanup;
    } else if (lfs_tag_type(tag) != LFS_TYPE_REG) {
        err = LFS_ERR_ISDIR;
        goto cleanup;
    } else if (flags & LFS_O_TRUNC) {
        // truncate if requested
        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0);
        file->flags |= LFS_F_DIRTY;
    } else {
        // try to load what's on disk, if it's inlined we'll fix it later
        tag = lfs_dir_get(lfs, &file->m, 0x7c3ff000,
                LFS_MKTAG(LFS_TYPE_STRUCT, file->id, 8), &file->ctz);
        if (tag < 0) {
            err = tag;
            goto cleanup;
        }
        lfs_ctz_fromle32(&file->ctz);
    }

    // fetch attrs
    for (const struct lfs_attr *a = file->cfg->attrs; a; a = a->next) {
        if ((file->flags & 3) != LFS_O_WRONLY) {
            lfs_stag_t res = lfs_dir_get(lfs, &file->m, 0x7ffff000,
                    LFS_MKTAG(0x100 | a->type, file->id, a->size), a->buffer);
            if (res < 0 && res != LFS_ERR_NOENT) {
                err = res;
                goto cleanup;
            }
        }

        if ((file->flags & 3) != LFS_O_RDONLY) {
            if (a->size > lfs->attr_max) {
                err = LFS_ERR_NOSPC;
                goto cleanup;
            }

            file->flags |= LFS_F_DIRTY;
        }
    }

    // allocate buffer if needed
    if (file->cfg->buffer) {
        file->cache.buffer = file->cfg->buffer;
    } else {
        file->cache.buffer = lfs_malloc(lfs->cfg->cache_size);
        if (!file->cache.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // zero to avoid information leak
    lfs_cache_zero(lfs, &file->cache);

    if (lfs_tag_type(tag) == LFS_TYPE_INLINESTRUCT) {
        // load inline files
        file->ctz.head = 0xfffffffe;
        file->ctz.size = lfs_tag_size(tag);
        file->flags |= LFS_F_INLINE;
        file->cache.block = file->ctz.head;
        file->cache.off = 0;
        file->cache.size = lfs->cfg->cache_size;

        // don't always read (may be new/trunc file)
        if (file->ctz.size > 0) {
            lfs_stag_t res = lfs_dir_get(lfs, &file->m, 0x7c3ff000,
                    LFS_MKTAG(LFS_TYPE_STRUCT, file->id, file->ctz.size),
                    file->cache.buffer);
            if (res < 0) {
                err = res;
                goto cleanup;
            }
        }
    }

    return 0;

cleanup:
    // clean up lingering resources
    file->flags |= LFS_F_ERRED;
    lfs_file_close(lfs, file);
    return err;
}

int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    static const struct lfs_file_config defaults = {0};
    return lfs_file_opencfg(lfs, file, path, flags, &defaults);
}

int lfs_file_close(lfs_t *lfs, lfs_file_t *file) {
    int err = lfs_file_sync(lfs, file);

    // remove from list of mdirs
    for (struct lfs_mlist **p = &lfs->mlist; *p; p = &(*p)->next) {
        if (*p == (struct lfs_mlist*)file) {
            *p = (*p)->next;
            break;
        }
    }

    // clean up memory
    if (!file->cfg->buffer) {
        lfs_free(file->cache.buffer);
    }

    return err;
}

static int lfs_file_relocate(lfs_t *lfs, lfs_file_t *file) {
    while (true) {
        // just relocate what exists into new block
        lfs_block_t nblock;
        int err = lfs_alloc(lfs, &nblock);
        if (err) {
            return err;
        }

        err = lfs_bd_erase(lfs, nblock);
        if (err) {
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // either read from dirty cache or disk
        for (lfs_off_t i = 0; i < file->off; i++) {
            uint8_t data;
            err = lfs_bd_read(lfs,
                    &file->cache, &lfs->rcache, file->off-i,
                    file->block, i, &data, 1);
            if (err) {
                return err;
            }

            err = lfs_bd_prog(lfs,
                    &lfs->pcache, &lfs->rcache, true,
                    nblock, i, &data, 1);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        // copy over new state of file
        memcpy(file->cache.buffer, lfs->pcache.buffer, lfs->cfg->cache_size);
        file->cache.block = lfs->pcache.block;
        file->cache.off = lfs->pcache.off;
        file->cache.size = lfs->pcache.size;
        lfs_cache_zero(lfs, &lfs->pcache);

        file->block = nblock;
        return 0;

relocate:
        LFS_DEBUG("Bad block at %"PRIu32, nblock);

        // just clear cache and try a new block
        lfs_cache_drop(lfs, &lfs->pcache);
    }
}

static int lfs_file_flush(lfs_t *lfs, lfs_file_t *file) {
    if (file->flags & LFS_F_READING) {
        file->flags &= ~LFS_F_READING;
    }

    if (file->flags & LFS_F_WRITING) {
        lfs_off_t pos = file->pos;

        if (!(file->flags & LFS_F_INLINE)) {
            // copy over anything after current branch
            lfs_file_t orig = {
                .ctz.head = file->ctz.head,
                .ctz.size = file->ctz.size,
                .flags = LFS_O_RDONLY,
                .pos = file->pos,
                .cache = lfs->rcache,
            };
            lfs_cache_drop(lfs, &lfs->rcache);

            while (file->pos < file->ctz.size) {
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
                    lfs_cache_drop(lfs, &orig.cache);
                    lfs_cache_drop(lfs, &lfs->rcache);
                }
            }

            // write out what we have
            while (true) {
                int err = lfs_bd_flush(lfs,
                        &file->cache, &lfs->rcache, true);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                break;

relocate:
                LFS_DEBUG("Bad block at %"PRIu32, file->block);
                err = lfs_file_relocate(lfs, file);
                if (err) {
                    return err;
                }
            }
        } else {
            file->ctz.size = lfs_max(file->pos, file->ctz.size);
        }

        // actual file updates
        file->ctz.head = file->block;
        file->ctz.size = file->pos;
        file->flags &= ~LFS_F_WRITING;
        file->flags |= LFS_F_DIRTY;

        file->pos = pos;
    }

    return 0;
}

int lfs_file_sync(lfs_t *lfs, lfs_file_t *file) {
    while (true) {
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }

        if ((file->flags & LFS_F_DIRTY) &&
                !(file->flags & LFS_F_ERRED) &&
                !lfs_pair_isnull(file->m.pair)) {
            // update dir entry
            uint16_t type;
            const void *buffer;
            lfs_size_t size;
            struct lfs_ctz ctz;
            if (file->flags & LFS_F_INLINE) {
                // inline the whole file
                type = LFS_TYPE_INLINESTRUCT;
                buffer = file->cache.buffer;
                size = file->ctz.size;
            } else {
                // update the ctz reference
                type = LFS_TYPE_CTZSTRUCT;
                // copy ctz so alloc will work during a relocate
                ctz = file->ctz;
                lfs_ctz_tole32(&ctz);
                buffer = &ctz;
                size = sizeof(ctz);
            }

            // commit file data and attributes
            err = lfs_dir_commit(lfs, &file->m,
                    LFS_MKATTR(LFS_FROM_USERATTRS,
                        file->id, file->cfg->attrs, 0,
                    LFS_MKATTR(type, file->id, buffer, size,
                    NULL)));
            if (err) {
                if (err == LFS_ERR_NOSPC && (file->flags & LFS_F_INLINE)) {
                    goto relocate;
                }
                return err;
            }

            file->flags &= ~LFS_F_DIRTY;
        }

        return 0;

relocate:
        // inline file doesn't fit anymore
        file->block = 0xfffffffe;
        file->off = file->pos;

        lfs_alloc_ack(lfs);
        err = lfs_file_relocate(lfs, file);
        if (err) {
            return err;
        }

        file->flags &= ~LFS_F_INLINE;
        file->flags |= LFS_F_WRITING;
    }
}

lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    lfs_size_t nsize = size;

    if ((file->flags & 3) == LFS_O_WRONLY) {
        return LFS_ERR_BADF;
    }

    if (file->flags & LFS_F_WRITING) {
        // flush out any writes
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }

    if (file->pos >= file->ctz.size) {
        // eof if past end
        return 0;
    }

    size = lfs_min(size, file->ctz.size - file->pos);
    nsize = size;

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & LFS_F_READING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & LFS_F_INLINE)) {
                int err = lfs_ctz_find(lfs, NULL, &file->cache,
                        file->ctz.head, file->ctz.size,
                        file->pos, &file->block, &file->off);
                if (err) {
                    return err;
                }
            } else {
                file->block = 0xfffffffe;
                file->off = file->pos;
            }

            file->flags |= LFS_F_READING;
        }

        // read as much as we can in current block
        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
        int err = lfs_bd_read(lfs,
                NULL, &file->cache, lfs->cfg->block_size,
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
        return LFS_ERR_BADF;
    }

    if (file->flags & LFS_F_READING) {
        // drop any reads
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }

    if ((file->flags & LFS_O_APPEND) && file->pos < file->ctz.size) {
        file->pos = file->ctz.size;
    }

    if (!(file->flags & LFS_F_WRITING) && file->pos > file->ctz.size) {
        // fill with zeros
        lfs_off_t pos = file->pos;
        file->pos = file->ctz.size;

        while (file->pos < pos) {
            lfs_ssize_t res = lfs_file_write(lfs, file, &(uint8_t){0}, 1);
            if (res < 0) {
                return res;
            }
        }
    }

    if ((file->flags & LFS_F_INLINE) &&
            file->pos + nsize > lfs->inline_max) {
        // inline file doesn't fit anymore
        file->block = 0xfffffffe;
        file->off = file->pos;

        lfs_alloc_ack(lfs);
        int err = lfs_file_relocate(lfs, file);
        if (err) {
            file->flags |= LFS_F_ERRED;
            return err;
        }

        file->flags &= ~LFS_F_INLINE;
        file->flags |= LFS_F_WRITING;
    }

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & LFS_F_WRITING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & LFS_F_INLINE)) {
                if (!(file->flags & LFS_F_WRITING) && file->pos > 0) {
                    // find out which block we're extending from
                    int err = lfs_ctz_find(lfs, NULL, &file->cache,
                            file->ctz.head, file->ctz.size,
                            file->pos-1, &file->block, &file->off);
                    if (err) {
                        file->flags |= LFS_F_ERRED;
                        return err;
                    }

                    // mark cache as dirty since we may have read data into it
                    lfs_cache_zero(lfs, &file->cache);
                }

                // extend file with new blocks
                lfs_alloc_ack(lfs);
                int err = lfs_ctz_extend(lfs, &file->cache, &lfs->rcache,
                        file->block, file->pos,
                        &file->block, &file->off);
                if (err) {
                    file->flags |= LFS_F_ERRED;
                    return err;
                }
            } else {
                file->block = 0xfffffffe;
                file->off = file->pos;
            }

            file->flags |= LFS_F_WRITING;
        }

        // program as much as we can in current block
        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
        while (true) {
            int err = lfs_bd_prog(lfs, &file->cache, &lfs->rcache, true,
                    file->block, file->off, data, diff);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                file->flags |= LFS_F_ERRED;
                return err;
            }

            break;
relocate:
            err = lfs_file_relocate(lfs, file);
            if (err) {
                file->flags |= LFS_F_ERRED;
                return err;
            }
        }

        file->pos += diff;
        file->off += diff;
        data += diff;
        nsize -= diff;

        lfs_alloc_ack(lfs);
    }

    file->flags &= ~LFS_F_ERRED;
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
    if (whence == LFS_SEEK_SET) {
        file->pos = off;
    } else if (whence == LFS_SEEK_CUR) {
        if (off < 0 && (lfs_off_t)-off > file->pos) {
            return LFS_ERR_INVAL;
        }

        file->pos = file->pos + off;
    } else if (whence == LFS_SEEK_END) {
        if (off < 0 && (lfs_off_t)-off > file->ctz.size) {
            return LFS_ERR_INVAL;
        }

        file->pos = file->ctz.size + off;
    }

    return file->pos;
}

int lfs_file_truncate(lfs_t *lfs, lfs_file_t *file, lfs_off_t size) {
    if ((file->flags & 3) == LFS_O_RDONLY) {
        return LFS_ERR_BADF;
    }

    lfs_off_t oldsize = lfs_file_size(lfs, file);
    if (size < oldsize) {
        // need to flush since directly changing metadata
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }

        // lookup new head in ctz skip list
        err = lfs_ctz_find(lfs, NULL, &file->cache,
                file->ctz.head, file->ctz.size,
                size, &file->ctz.head, &(lfs_off_t){0});
        if (err) {
            return err;
        }

        file->ctz.size = size;
        file->flags |= LFS_F_DIRTY;
    } else if (size > oldsize) {
        lfs_off_t pos = file->pos;

        // flush+seek if not already at end
        if (file->pos != oldsize) {
            int err = lfs_file_seek(lfs, file, 0, LFS_SEEK_END);
            if (err < 0) {
                return err;
            }
        }

        // fill with zeros
        while (file->pos < size) {
            lfs_ssize_t res = lfs_file_write(lfs, file, &(uint8_t){0}, 1);
            if (res < 0) {
                return res;
            }
        }

        // restore pos
        int err = lfs_file_seek(lfs, file, pos, LFS_SEEK_SET);
        if (err < 0) {
            return err;
        }
    }

    return 0;
}

lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file) {
    (void)lfs;
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
    (void)lfs;
    if (file->flags & LFS_F_WRITING) {
        return lfs_max(file->pos, file->ctz.size);
    } else {
        return file->ctz.size;
    }
}


/// General fs operations ///
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info) {
    lfs_mdir_t cwd;
    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path);
    if (tag < 0) {
        return tag;
    }

    return lfs_dir_getinfo(lfs, &cwd, lfs_tag_id(tag), info);
}

int lfs_remove(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    lfs_mdir_t cwd;
    err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path);
    if (tag < 0) {
        return tag;
    }

    lfs_mdir_t dir;
    if (lfs_tag_type(tag) == LFS_TYPE_DIR) {
        // must be empty before removal
        lfs_block_t pair[2];
        lfs_stag_t res = lfs_dir_get(lfs, &cwd, 0x7c3ff000,
                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), pair);
        if (res < 0) {
            return res;
        }
        lfs_pair_fromle32(pair);

        err = lfs_dir_fetch(lfs, &dir, pair);
        if (err) {
            return err;
        }

        if (dir.count > 0 || dir.split) {
            return LFS_ERR_NOTEMPTY;
        }

        // mark fs as orphaned
        lfs_global_orphans(lfs, +1);
    }

    // delete the entry
    err = lfs_dir_commit(lfs, &cwd,
            LFS_MKATTR(LFS_TYPE_DELETE, lfs_tag_id(tag), NULL, 0,
            NULL));
    if (err) {
        return err;
    }

    if (lfs_tag_type(tag) == LFS_TYPE_DIR) {
        // fix orphan
        lfs_global_orphans(lfs, -1);

        err = lfs_fs_pred(lfs, dir.pair, &cwd);
        if (err) {
            return err;
        }

        // steal state
        cwd.tail[0] = dir.tail[0];
        cwd.tail[1] = dir.tail[1];
        lfs_global_xor(&lfs->locals, &dir.locals);
        err = lfs_dir_commit(lfs, &cwd,
                LFS_MKATTR(LFS_TYPE_SOFTTAIL, 0x3ff,
                    cwd.tail, sizeof(cwd.tail),
                NULL));
        if (err) {
            return err;
        }
    }

    return 0;
}

int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_fs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // find old entry
    lfs_mdir_t oldcwd;
    lfs_stag_t oldtag = lfs_dir_find(lfs, &oldcwd, &oldpath);
    if (oldtag < 0) {
        return oldtag;
    }

    // find new entry
    lfs_mdir_t newcwd;
    lfs_stag_t prevtag = lfs_dir_find(lfs, &newcwd, &newpath);
    if (prevtag < 0 && prevtag != LFS_ERR_NOENT) {
        return prevtag;
    }

    uint16_t newid = lfs_tag_id(prevtag);

    lfs_mdir_t prevdir;
    if (prevtag == LFS_ERR_NOENT) {
        // check that name fits
        lfs_size_t nlen = strlen(newpath);
        if (nlen > lfs->name_max) {
            return LFS_ERR_NAMETOOLONG;
        }

        // get next id
        newid = newcwd.count;
    } else if (lfs_tag_type(prevtag) != lfs_tag_type(oldtag)) {
        return LFS_ERR_ISDIR;
    } else if (lfs_tag_type(prevtag) == LFS_TYPE_DIR) {
        // must be empty before removal
        lfs_block_t prevpair[2];
        lfs_stag_t res = lfs_dir_get(lfs, &newcwd, 0x7c3ff000,
                LFS_MKTAG(LFS_TYPE_STRUCT, newid, 8), prevpair);
        if (res < 0) {
            return res;
        }
        lfs_pair_fromle32(prevpair);

        // must be empty before removal
        err = lfs_dir_fetch(lfs, &prevdir, prevpair);
        if (err) {
            return err;
        }

        if (prevdir.count > 0 || prevdir.split) {
            return LFS_ERR_NOTEMPTY;
        }

        // mark fs as orphaned
        lfs_global_orphans(lfs, +1);
    }

    // create move to fix later
    lfs_global_move(lfs, oldcwd.pair, lfs_tag_id(oldtag));

    // move over all attributes
    err = lfs_dir_commit(lfs, &newcwd,
            LFS_MKATTR(LFS_FROM_MOVE, newid, &oldcwd, lfs_tag_id(oldtag),
            LFS_MKATTR(lfs_tag_type(oldtag), newid, newpath, strlen(newpath),
            (prevtag != LFS_ERR_NOENT)
                ? LFS_MKATTR(LFS_TYPE_DELETE, newid, NULL, 0, NULL)
                : NULL)));
    if (err) {
        return err;
    }

    // let commit clean up after move (if we're different! otherwise move
    // logic already fixed it for us)
    if (lfs_pair_cmp(oldcwd.pair, newcwd.pair) != 0) {
        err = lfs_dir_commit(lfs, &oldcwd, NULL);
        if (err) {
            return err;
        }
    }

    if (prevtag != LFS_ERR_NOENT && lfs_tag_type(prevtag) == LFS_TYPE_DIR) {
        // fix orphan
        lfs_global_orphans(lfs, -1);

        err = lfs_fs_pred(lfs, prevdir.pair, &newcwd);
        if (err) {
            return err;
        }

        // steal state
        newcwd.tail[0] = prevdir.tail[0];
        newcwd.tail[1] = prevdir.tail[1];
        lfs_global_xor(&lfs->locals, &prevdir.locals);
        err = lfs_dir_commit(lfs, &newcwd,
                LFS_MKATTR(LFS_TYPE_SOFTTAIL, 0x3ff,
                    newcwd.tail, sizeof(newcwd.tail),
                NULL));
        if (err) {
            return err;
        }
    }

    return 0;
}

lfs_ssize_t lfs_getattr(lfs_t *lfs, const char *path,
        uint8_t type, void *buffer, lfs_size_t size) {
    lfs_mdir_t cwd;
    lfs_stag_t res = lfs_dir_find(lfs, &cwd, &path);
    if (res < 0) {
        return res;
    }

    uint16_t id = lfs_tag_id(res);
    if (id == 0x3ff) {
        // special case for root
        id = 0;
        int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
        if (err) {
            return err;
        }
    }

    res = lfs_dir_get(lfs, &cwd, 0x7ffff000,
            LFS_MKTAG(0x100 | type, id, lfs_min(size, lfs->attr_max)),
            buffer);
    if (res < 0) {
        if (res == LFS_ERR_NOENT) {
            return LFS_ERR_NOATTR;
        }
        return res;
    }

    return lfs_tag_size(res);
}

static int lfs_commitattr(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size) {
    lfs_mdir_t cwd;
    lfs_stag_t res = lfs_dir_find(lfs, &cwd, &path);
    if (res < 0) {
        return res;
    }

    uint16_t id = lfs_tag_id(res);
    if (id == 0x3ff) {
        // special case for root
        id = 0;
        int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
        if (err) {
            return err;
        }
    }

    return lfs_dir_commit(lfs, &cwd,
            LFS_MKATTR(0x100 | type, id, buffer, size,
            NULL));
}

int lfs_setattr(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size) {
    if (size > lfs->attr_max) {
        return LFS_ERR_NOSPC;
    }

    return lfs_commitattr(lfs, path, type, buffer, size);
}

int lfs_removeattr(lfs_t *lfs, const char *path, uint8_t type) {
    return lfs_commitattr(lfs, path, type, NULL, LFS_ATTR_MAX+1);
}


/// Filesystem operations ///
static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg) {
    lfs->cfg = cfg;
    int err = 0;

    // check that block size is a multiple of cache size is a multiple
    // of prog and read sizes
    LFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->read_size == 0);
    LFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->prog_size == 0);
    LFS_ASSERT(lfs->cfg->block_size % lfs->cfg->cache_size == 0);

    // check that the block size is large enough to fit ctz pointers
    LFS_ASSERT(4*lfs_npw2(0xffffffff / (lfs->cfg->block_size-2*4))
            <= lfs->cfg->block_size);

    // setup read cache
    if (lfs->cfg->read_buffer) {
        lfs->rcache.buffer = lfs->cfg->read_buffer;
    } else {
        lfs->rcache.buffer = lfs_malloc(lfs->cfg->cache_size);
        if (!lfs->rcache.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // setup program cache
    if (lfs->cfg->prog_buffer) {
        lfs->pcache.buffer = lfs->cfg->prog_buffer;
    } else {
        lfs->pcache.buffer = lfs_malloc(lfs->cfg->cache_size);
        if (!lfs->pcache.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // zero to avoid information leaks
    lfs_cache_zero(lfs, &lfs->rcache);
    lfs_cache_zero(lfs, &lfs->pcache);

    // setup lookahead, must be multiple of 32-bits
    LFS_ASSERT(lfs->cfg->lookahead % 32 == 0);
    LFS_ASSERT(lfs->cfg->lookahead > 0);
    if (lfs->cfg->lookahead_buffer) {
        lfs->free.buffer = lfs->cfg->lookahead_buffer;
    } else {
        lfs->free.buffer = lfs_malloc(lfs->cfg->lookahead/8);
        if (!lfs->free.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }

    // check that the size limits are sane
    LFS_ASSERT(lfs->cfg->inline_max <= LFS_INLINE_MAX);
    LFS_ASSERT(lfs->cfg->inline_max <= lfs->cfg->cache_size);
    lfs->inline_max = lfs->cfg->inline_max;
    if (!lfs->inline_max) {
        lfs->inline_max = lfs_min(LFS_INLINE_MAX, lfs->cfg->cache_size);
    }

    LFS_ASSERT(lfs->cfg->attr_max <= LFS_ATTR_MAX);
    lfs->attr_max = lfs->cfg->attr_max;
    if (!lfs->attr_max) {
        lfs->attr_max = LFS_ATTR_MAX;
    }

    LFS_ASSERT(lfs->cfg->name_max <= LFS_NAME_MAX);
    lfs->name_max = lfs->cfg->name_max;
    if (!lfs->name_max) {
        lfs->name_max = LFS_NAME_MAX;
    }

    // setup default state
    lfs->root[0] = 0xffffffff;
    lfs->root[1] = 0xffffffff;
    lfs->mlist = NULL;
    lfs->seed = 0;
    lfs->globals.g.movepair[0] = 0xffffffff;
    lfs->globals.g.movepair[1] = 0xffffffff;
    lfs->globals.g.moveid = 0x3ff;
    lfs->globals.g.orphans = 0;
    lfs_global_zero(&lfs->locals);

    return 0;

cleanup:
    lfs_deinit(lfs);
    return err;
}

static int lfs_deinit(lfs_t *lfs) {
    // free allocated memory
    if (!lfs->cfg->read_buffer) {
        lfs_free(lfs->rcache.buffer);
    }

    if (!lfs->cfg->prog_buffer) {
        lfs_free(lfs->pcache.buffer);
    }

    if (!lfs->cfg->lookahead_buffer) {
        lfs_free(lfs->free.buffer);
    }

    return 0;
}

int lfs_format(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    // create free lookahead
    memset(lfs->free.buffer, 0, lfs->cfg->lookahead/8);
    lfs->free.off = 0;
    lfs->free.size = lfs_min(lfs->cfg->lookahead, lfs->cfg->block_count);
    lfs->free.i = 0;
    lfs_alloc_ack(lfs);

    // create root dir
    lfs_mdir_t root;
    err = lfs_dir_alloc(lfs, &root);
    if (err) {
        goto cleanup;
    }

    // write one superblock
    lfs_superblock_t superblock = {
        .magic = {"littlefs"},
        .version = LFS_DISK_VERSION,

        .block_size  = lfs->cfg->block_size,
        .block_count = lfs->cfg->block_count,
        .attr_max    = lfs->attr_max,
        .name_max    = lfs->name_max,
        .inline_max  = lfs->inline_max,
    };

    lfs_superblock_tole32(&superblock);
    err = lfs_dir_commit(lfs, &root,
            LFS_MKATTR(LFS_TYPE_ROOT, 0, &superblock, sizeof(superblock),
            NULL));
    if (err) {
        goto cleanup;
    }

    // sanity check that fetch works
    err = lfs_dir_fetch(lfs, &root, (const lfs_block_t[2]){0, 1});
    if (err) {
        goto cleanup;
    }

cleanup:
    lfs_deinit(lfs);
    return err;
}

int lfs_mount(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    // find root/superblock
    lfs_mdir_t root;
    lfs_superblock_t superblock;
    lfs_stag_t tag = lfs_dir_findmatch(lfs,
            &root, (const lfs_block_t[2]){0, 1}, false, 0x7fc00000,
            LFS_MKTAG(LFS_TYPE_ROOT, 0, 8),
            lfs_dir_find_match, &(struct lfs_dir_find_match){
                lfs, "littlefs", 8});
    if (tag < 0) {
        err = tag;
        goto cleanup;
    }

    lfs_stag_t res = lfs_dir_get(lfs, &root, 0x7f800000,
            LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, sizeof(superblock)),
            &superblock);
    if (res < 0) {
        err = res;
        goto cleanup;
    }
    lfs_superblock_fromle32(&superblock);

    lfs->root[0] = root.pair[0];
    lfs->root[1] = root.pair[1];

    // check version
    uint16_t major_version = (0xffff & (superblock.version >> 16));
    uint16_t minor_version = (0xffff & (superblock.version >>  0));
    if ((major_version != LFS_DISK_VERSION_MAJOR ||
         minor_version > LFS_DISK_VERSION_MINOR)) {
        LFS_ERROR("Invalid version %"PRIu32".%"PRIu32,
                major_version, minor_version);
        err = LFS_ERR_INVAL;
        goto cleanup;
    }

    // check superblock configuration
    if (superblock.attr_max) {
        if (superblock.attr_max > lfs->attr_max) {
            LFS_ERROR("Unsupported attr_max (%"PRIu32" > %"PRIu32")",
                    superblock.attr_max, lfs->attr_max);
            err = LFS_ERR_INVAL;
            goto cleanup;
        }

        lfs->attr_max = superblock.attr_max;
    }

    if (superblock.name_max) {
        if (superblock.name_max > lfs->name_max) {
            LFS_ERROR("Unsupported name_max (%"PRIu32" > %"PRIu32")",
                    superblock.name_max, lfs->name_max);
            err = LFS_ERR_INVAL;
            goto cleanup;
        }

        lfs->name_max = superblock.name_max;
    }

    if (superblock.inline_max) {
        if (superblock.inline_max > lfs->inline_max) {
            LFS_ERROR("Unsupported inline_max (%"PRIu32" > %"PRIu32")",
                    superblock.inline_max, lfs->inline_max);
            err = LFS_ERR_INVAL;
            goto cleanup;
        }

        lfs->inline_max = superblock.inline_max;
    }

    // scan for any global updates
    lfs_mdir_t dir = {.tail = {0, 1}};
    while (!lfs_pair_isnull(dir.tail)) {
        err = lfs_dir_fetch(lfs, &dir, dir.tail);
        if (err) {
            err = LFS_ERR_INVAL;
            goto cleanup;
        }

        // xor together indirect deletes
        lfs_global_xor(&lfs->locals, &dir.locals);
    }

    // update littlefs with globals
    lfs_global_fromle32(&lfs->locals);
    lfs_global_xor(&lfs->globals, &lfs->locals);
    lfs_global_zero(&lfs->locals);
    if (!lfs_pair_isnull(lfs->globals.g.movepair)) {
        LFS_DEBUG("Found move %"PRIu32" %"PRIu32" %"PRIu32,
                lfs->globals.g.movepair[0],
                lfs->globals.g.movepair[1],
                lfs->globals.g.moveid);
    }

    // setup free lookahead
    lfs->free.off = lfs->seed % lfs->cfg->block_size;
    lfs->free.size = 0;
    lfs->free.i = 0;
    lfs_alloc_ack(lfs);

    return 0;

cleanup:
    lfs_unmount(lfs);
    return err;
}

int lfs_unmount(lfs_t *lfs) {
    return lfs_deinit(lfs);
}


/// Filesystem filesystem operations ///
int lfs_fs_traverse(lfs_t *lfs,
        int (*cb)(void *data, lfs_block_t block), void *data) {
    if (lfs_pair_isnull(lfs->root)) {
        return 0;
    }

    // iterate over metadata pairs
    lfs_mdir_t dir = {.tail = {0, 1}};
    while (!lfs_pair_isnull(dir.tail)) {
        for (int i = 0; i < 2; i++) {
            int err = cb(data, dir.tail[i]);
            if (err) {
                return err;
            }
        }

        // iterate through ids in directory
        int err = lfs_dir_fetch(lfs, &dir, dir.tail);
        if (err) {
            return err;
        }

        for (uint16_t id = 0; id < dir.count; id++) {
            struct lfs_ctz ctz;
            lfs_stag_t tag = lfs_dir_get(lfs, &dir, 0x7c3ff000,
                    LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
            if (tag < 0) {
                if (tag == LFS_ERR_NOENT) {
                    continue;
                }
                return tag;
            }
            lfs_ctz_fromle32(&ctz);

            if (lfs_tag_type(tag) == LFS_TYPE_CTZSTRUCT) {
                err = lfs_ctz_traverse(lfs, NULL, &lfs->rcache,
                        ctz.head, ctz.size, cb, data);
                if (err) {
                    return err;
                }
            }
        }
    }

    // iterate over any open files
    for (lfs_file_t *f = (lfs_file_t*)lfs->mlist; f; f = f->next) {
        if (f->type != LFS_TYPE_REG) {
            continue;
        }

        if ((f->flags & LFS_F_DIRTY) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
                    f->ctz.head, f->ctz.size, cb, data);
            if (err) {
                return err;
            }
        }

        if ((f->flags & LFS_F_WRITING) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
                    f->block, f->pos, cb, data);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

static int lfs_fs_pred(lfs_t *lfs,
        const lfs_block_t pair[2], lfs_mdir_t *pdir) {
    if (lfs_pair_isnull(lfs->root)) {
        return LFS_ERR_NOENT;
    }

    // iterate over all directory directory entries
    pdir->tail[0] = 0;
    pdir->tail[1] = 1;
    while (!lfs_pair_isnull(pdir->tail)) {
        if (lfs_pair_cmp(pdir->tail, pair) == 0) {
            return 0;
        }

        int err = lfs_dir_fetch(lfs, pdir, pdir->tail);
        if (err) {
            return err;
        }
    }

    return LFS_ERR_NOENT;
}

struct lfs_fs_parent_match {
    lfs_t *lfs;
    const lfs_block_t pair[2];
};

static int lfs_fs_parent_match(void *data,
        lfs_tag_t tag, const void *buffer) {
    struct lfs_fs_parent_match *find = data;
    lfs_t *lfs = find->lfs;
    const struct lfs_diskoff *disk = buffer;
    (void)tag;

    lfs_block_t child[2];
    int err = lfs_bd_read(lfs,
            &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
            disk->block, disk->off, &child, sizeof(child));
    if (err) {
        return err;
    }

    lfs_pair_fromle32(child);
    return (lfs_pair_cmp(child, find->pair) == 0);
}

static lfs_stag_t lfs_fs_parent(lfs_t *lfs, const lfs_block_t pair[2],
        lfs_mdir_t *parent) {
    if (lfs_pair_isnull(lfs->root)) {
        return LFS_ERR_NOENT;
    }

    // search for both orderings so we can reuse the find function
    for (int i = 0; i < 2; i++) {
        struct lfs_fs_parent_match match = {lfs, {pair[0], pair[1]}};
        lfs_stag_t tag = lfs_dir_findmatch(lfs, parent,
                (const lfs_block_t[2]){0, 1}, true, 0x7fc00fff,
                LFS_MKTAG(LFS_TYPE_DIRSTRUCT, 0, 8),
                lfs_fs_parent_match, &match);
        if (tag != LFS_ERR_NOENT) {
            return tag;
        }
    }

    return LFS_ERR_NOENT;
}

static int lfs_fs_relocate(lfs_t *lfs,
        const lfs_block_t oldpair[2], lfs_block_t newpair[2]) {
    // update internal root
    if (lfs_pair_cmp(oldpair, lfs->root) == 0) {
        LFS_DEBUG("Relocating root %"PRIu32" %"PRIu32,
                newpair[0], newpair[1]);
        lfs->root[0] = newpair[0];
        lfs->root[1] = newpair[1];
    }

    // update internally tracked dirs
    for (struct lfs_mlist *d = lfs->mlist; d; d = d->next) {
        if (lfs_pair_cmp(oldpair, d->m.pair) == 0) {
            d->m.pair[0] = newpair[0];
            d->m.pair[1] = newpair[1];
        }
    }

    // find parent
    lfs_mdir_t parent;
    lfs_stag_t tag = lfs_fs_parent(lfs, oldpair, &parent);
    if (tag < 0 && tag != LFS_ERR_NOENT) {
        return tag;
    }

    if (tag != LFS_ERR_NOENT) {
        // update disk, this creates a desync
        lfs_global_orphans(lfs, +1);

        lfs_pair_tole32(newpair);
        int err = lfs_dir_commit(lfs, &parent,
                &(struct lfs_mattr){.tag=tag, .buffer=newpair});
        lfs_pair_fromle32(newpair);
        if (err) {
            return err;
        }

        // next step, clean up orphans
        lfs_global_orphans(lfs, -1);
    }

    // find pred
    int err = lfs_fs_pred(lfs, oldpair, &parent);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    // if we can't find dir, it must be new
    if (err != LFS_ERR_NOENT) {
        // replace bad pair, either we clean up desync, or no desync occured
        parent.tail[0] = newpair[0];
        parent.tail[1] = newpair[1];
        err = lfs_dir_commit(lfs, &parent,
                LFS_MKATTR(LFS_TYPE_TAIL + parent.split, 0x3ff,
                    parent.tail, sizeof(parent.tail),
                NULL));
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_fs_deorphan(lfs_t *lfs) {
    // Fix any orphans
    lfs_mdir_t pdir = {.split = true};
    lfs_mdir_t dir = {.tail = {0, 1}};

    // iterate over all directory directory entries
    while (!lfs_pair_isnull(dir.tail)) {
        int err = lfs_dir_fetch(lfs, &dir, dir.tail);
        if (err) {
            return err;
        }

        // check head blocks for orphans
        if (!pdir.split) {
            // check if we have a parent
            lfs_mdir_t parent;
            lfs_stag_t tag = lfs_fs_parent(lfs, pdir.tail, &parent);
            if (tag < 0 && tag != LFS_ERR_NOENT) {
                return tag;
            }

            if (tag == LFS_ERR_NOENT) {
                // we are an orphan
                LFS_DEBUG("Fixing orphan %"PRIu32" %"PRIu32,
                        pdir.tail[0], pdir.tail[1]);

                pdir.tail[0] = dir.tail[0];
                pdir.tail[1] = dir.tail[1];
                err = lfs_dir_commit(lfs, &pdir,
                        LFS_MKATTR(LFS_TYPE_SOFTTAIL, 0x3ff,
                            pdir.tail, sizeof(pdir.tail),
                        NULL));
                if (err) {
                    return err;
                }

                break;
            }

            lfs_block_t pair[2];
            lfs_stag_t res = lfs_dir_get(lfs, &parent, 0x7ffff000, tag, pair);
            if (res < 0) {
                return res;
            }
            lfs_pair_fromle32(pair);

            if (!lfs_pair_sync(pair, pdir.tail)) {
                // we have desynced
                LFS_DEBUG("Fixing half-orphan %"PRIu32" %"PRIu32,
                        pair[0], pair[1]);

                pdir.tail[0] = pair[0];
                pdir.tail[1] = pair[1];
                err = lfs_dir_commit(lfs, &pdir,
                        LFS_MKATTR(LFS_TYPE_SOFTTAIL, 0x3ff,
                            pdir.tail, sizeof(pdir.tail),
                        NULL));
                if (err) {
                    return err;
                }

                break;
            }
        }

        memcpy(&pdir, &dir, sizeof(pdir));
    }

    // mark orphans as fixed
    lfs_global_orphans(lfs, -lfs->globals.g.orphans);
    return 0;
}

static int lfs_fs_demove(lfs_t *lfs) {
    // Fix bad moves
    LFS_DEBUG("Fixing move %"PRIu32" %"PRIu32" %"PRIu32,
            lfs->globals.g.movepair[0],
            lfs->globals.g.movepair[1],
            lfs->globals.g.moveid);

    // fetch and delete the moved entry
    lfs_mdir_t movedir;
    int err = lfs_dir_fetch(lfs, &movedir, lfs->globals.g.movepair);
    if (err) {
        return err;
    }

    // rely on cancel logic inside commit
    err = lfs_dir_commit(lfs, &movedir, NULL);
    if (err) {
        return err;
    }

    return 0;
}

static int lfs_fs_forceconsistency(lfs_t *lfs) {
    if (lfs->globals.g.orphans) {
        int err = lfs_fs_deorphan(lfs);
        if (err) {
            return err;
        }
    }

    if (lfs->globals.g.moveid != 0x3ff) {
        int err = lfs_fs_demove(lfs);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_fs_size_count(void *p, lfs_block_t block) {
    (void)block;
    lfs_size_t *size = p;
    *size += 1;
    return 0;
}

lfs_ssize_t lfs_fs_size(lfs_t *lfs) {
    lfs_size_t size = 0;
    int err = lfs_fs_traverse(lfs, lfs_fs_size_count, &size);
    if (err) {
        return err;
    }

    return size;
}
