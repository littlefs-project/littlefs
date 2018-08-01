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
static int lfs_cache_read(lfs_t *lfs, lfs_cache_t *rcache,
        const lfs_cache_t *pcache, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    LFS_ASSERT(block != 0xffffffff);

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
        LFS_ASSERT(block < lfs->cfg->block_count);
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

static int lfs_cache_cmp(lfs_t *lfs, lfs_cache_t *rcache,
        const lfs_cache_t *pcache, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;

    for (lfs_off_t i = 0; i < size; i++) {
        uint8_t c;
        int err = lfs_cache_read(lfs, rcache, pcache,
                block, off+i, &c, 1);
        if (err) {
            return err;
        }

        if (c != data[i]) {
            return false;
        }
    }

    return true;
}

static int lfs_cache_crc(lfs_t *lfs, lfs_cache_t *rcache,
        const lfs_cache_t *pcache, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, uint32_t *crc) {
    for (lfs_off_t i = 0; i < size; i++) {
        uint8_t c;
        int err = lfs_cache_read(lfs, rcache, pcache,
                block, off+i, &c, 1);
        if (err) {
            return err;
        }

        lfs_crc(crc, &c, 1);
    }

    return 0;
}

static int lfs_cache_flush(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache) {
    if (pcache->block != 0xffffffff) {
        LFS_ASSERT(pcache->block < lfs->cfg->block_count);
        int err = lfs->cfg->prog(lfs->cfg, pcache->block,
                pcache->off, pcache->buffer, lfs->cfg->prog_size);
        if (err) {
            return err;
        }

        if (rcache) {
            int res = lfs_cache_cmp(lfs, rcache, NULL, pcache->block,
                    pcache->off, pcache->buffer, lfs->cfg->prog_size);
            if (res < 0) {
                return res;
            }

            if (!res) {
                return LFS_ERR_CORRUPT;
            }
        }

        pcache->block = 0xffffffff;
    }

    return 0;
}

static int lfs_cache_prog(lfs_t *lfs, lfs_cache_t *pcache,
        lfs_cache_t *rcache, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    LFS_ASSERT(block != 0xffffffff);
    LFS_ASSERT(off + size <= lfs->cfg->block_size);

    while (size > 0) {
        if (block == pcache->block && off >= pcache->off &&
                off < pcache->off + lfs->cfg->prog_size) {
            // is already in pcache?
            lfs_size_t diff = lfs_min(size,
                    lfs->cfg->prog_size - (off-pcache->off));
            memcpy(&pcache->buffer[off-pcache->off], data, diff);

            data += diff;
            off += diff;
            size -= diff;

            if (off % lfs->cfg->prog_size == 0) {
                // eagerly flush out pcache if we fill up
                int err = lfs_cache_flush(lfs, pcache, rcache);
                if (err) {
                    return err;
                }
            }

            continue;
        }

        // pcache must have been flushed, either by programming and
        // entire block or manually flushing the pcache
        LFS_ASSERT(pcache->block == 0xffffffff);

        if (off % lfs->cfg->prog_size == 0 &&
                size >= lfs->cfg->prog_size) {
            // bypass pcache?
            LFS_ASSERT(block < lfs->cfg->block_count);
            lfs_size_t diff = size - (size % lfs->cfg->prog_size);
            int err = lfs->cfg->prog(lfs->cfg, block, off, data, diff);
            if (err) {
                return err;
            }

            if (rcache) {
                int res = lfs_cache_cmp(lfs, rcache, NULL,
                        block, off, data, diff);
                if (res < 0) {
                    return res;
                }

                if (!res) {
                    return LFS_ERR_CORRUPT;
                }
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // prepare pcache, first condition can no longer fail
        pcache->block = block;
        pcache->off = off - (off % lfs->cfg->prog_size);
    }

    return 0;
}


/// General lfs block device operations ///
static int lfs_bd_read(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    return lfs_cache_read(lfs, &lfs->rcache, &lfs->pcache,
            block, off, buffer, size);
}

static int lfs_bd_prog(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    return lfs_cache_prog(lfs, &lfs->pcache, NULL,
            block, off, buffer, size);
}

static int lfs_bd_cmp(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    return lfs_cache_cmp(lfs, &lfs->rcache, NULL, block, off, buffer, size);
}

static int lfs_bd_crc(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, uint32_t *crc) {
    return lfs_cache_crc(lfs, &lfs->rcache, NULL, block, off, size, crc);
}

static int lfs_bd_erase(lfs_t *lfs, lfs_block_t block) {
    LFS_ASSERT(block < lfs->cfg->block_count);
    return lfs->cfg->erase(lfs->cfg, block);
}

static int lfs_bd_sync(lfs_t *lfs) {
    lfs->rcache.block = 0xffffffff;

    int err = lfs_cache_flush(lfs, &lfs->pcache, NULL);
    if (err) {
        return err;
    }

    return lfs->cfg->sync(lfs->cfg);
}


/// Internal operations predeclared here ///
int lfs_fs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data);
static int lfs_pred(lfs_t *lfs, const lfs_block_t dir[2], lfs_mdir_t *pdir);
static int32_t lfs_parent(lfs_t *lfs, const lfs_block_t dir[2],
        lfs_mdir_t *parent);
static int lfs_relocate(lfs_t *lfs,
        const lfs_block_t oldpair[2], lfs_block_t newpair[2]);
int lfs_scan(lfs_t *lfs);
int lfs_fixmove(lfs_t *lfs);
int lfs_forceconsistency(lfs_t *lfs);


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
            LFS_WARN("No more free space %d", lfs->free.i + lfs->free.off);
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

static inline bool lfs_pairsync(
        const lfs_block_t paira[2],
        const lfs_block_t pairb[2]) {
    return (paira[0] == pairb[0] && paira[1] == pairb[1]) ||
           (paira[0] == pairb[1] && paira[1] == pairb[0]);
}

static inline void lfs_pairfromle32(lfs_block_t *pair) {
    pair[0] = lfs_fromle32(pair[0]);
    pair[1] = lfs_fromle32(pair[1]);
}

static inline void lfs_pairtole32(lfs_block_t *pair) {
    pair[0] = lfs_tole32(pair[0]);
    pair[1] = lfs_tole32(pair[1]);
}

static void lfs_ctzfromle32(struct lfs_ctz *ctz) {
    ctz->head = lfs_fromle32(ctz->head);
    ctz->size = lfs_fromle32(ctz->size);
}

static void lfs_ctztole32(struct lfs_ctz *ctz) {
    ctz->head = lfs_tole32(ctz->head);
    ctz->size = lfs_tole32(ctz->size);
}


/// Entry tag operations ///
#define LFS_MKTAG(type, id, size) \
    (((uint32_t)(type) << 22) | ((uint32_t)(id) << 12) | (uint32_t)(size))

#define LFS_MKATTR(type, id, buffer, size, next) \
    &(const lfs_mattr_t){LFS_MKTAG(type, id, size), (buffer), (next)}

static inline bool lfs_tagisvalid(uint32_t tag) {
    return !(tag & 0x80000000);
}

static inline bool lfs_tagisuser(uint32_t tag) {
    return (tag & 0x40000000);
}

static inline uint16_t lfs_tagtype(uint32_t tag) {
    return (tag & 0x7fc00000) >> 22;
}

static inline uint16_t lfs_tagsubtype(uint32_t tag) {
    return (tag & 0x7c000000) >> 22;
}

static inline uint16_t lfs_tagid(uint32_t tag) {
    return (tag & 0x003ff000) >> 12;
}

static inline lfs_size_t lfs_tagsize(uint32_t tag) {
    return tag & 0x00000fff;
}

// operations on set of globals
static inline void lfs_globalxor(lfs_global_t *a, const lfs_global_t *b) {
    for (int i = 0; i < sizeof(lfs_global_t)/2; i++) {
        a->u16[i] ^= b->u16[i];
    }
}

static inline bool lfs_globaliszero(const lfs_global_t *a) {
    for (int i = 0; i < sizeof(lfs_global_t)/2; i++) {
        if (a->u16[i] != 0) {
            return false;
        }
    }
    return true;
}

static inline void lfs_globalzero(lfs_global_t *a) {
    memset(a->u16, 0x00, sizeof(lfs_global_t));
}

static inline void lfs_globalones(lfs_global_t *a) {
    memset(a->u16, 0xff, sizeof(lfs_global_t));
}

static inline void lfs_globalxormove(lfs_global_t *a,
        const lfs_block_t pair[2], uint16_t id) {
    a->u16[0] ^= id;
    for (int i = 0; i < sizeof(lfs_block_t[2])/2; i++) {
        a->u16[1+i] ^= ((uint16_t*)pair)[i];
    }
}

static inline void lfs_globalxordeorphaned(lfs_global_t *a, bool deorphaned) {
    a->u16[0] ^= deorphaned << 15;
}

static inline void lfs_globalfromle32(lfs_global_t *a) {
    a->u16[0] = lfs_fromle16(a->u16[0]);
    lfs_pairfromle32((lfs_block_t*)&a->u16[1]);
}

static inline void lfs_globaltole32(lfs_global_t *a) {
    a->u16[0] = lfs_tole16(a->u16[0]);
    lfs_pairtole32((lfs_block_t*)&a->u16[1]);
}

static inline const lfs_block_t *lfs_globalmovepair(const lfs_t *lfs) {
    return (const lfs_block_t*)&lfs->globals.u16[1];
}

static inline uint16_t lfs_globalmoveid(const lfs_t *lfs) {
    return 0x3ff & lfs->globals.u16[0];
}

static inline bool lfs_globalisdeorphaned(const lfs_t *lfs) {
    return 0x8000 & lfs->globals.u16[0];
}

static inline void lfs_globalmove(lfs_t *lfs,
        const lfs_block_t pair[2], uint16_t id) {
    lfs_global_t diff;
    lfs_globalzero(&diff);
    lfs_globalxormove(&diff, lfs_globalmovepair(lfs), lfs_globalmoveid(lfs));
    lfs_globalxormove(&diff, pair, id);
    lfs_globalfromle32(&lfs->locals);
    lfs_globalxor(&lfs->locals, &diff);
    lfs_globaltole32(&lfs->locals);
    lfs_globalxor(&lfs->globals, &diff);
}

static inline void lfs_globaldeorphaned(lfs_t *lfs, bool deorphaned) {
    deorphaned ^= lfs_globalisdeorphaned(lfs);
    lfs_globalfromle32(&lfs->locals);
    lfs_globalxordeorphaned(&lfs->locals, deorphaned);
    lfs_globaltole32(&lfs->locals);
    lfs_globalxordeorphaned(&lfs->globals, deorphaned);
}


// commit logic
struct lfs_commit {
    lfs_block_t block;
    lfs_off_t off;
    uint32_t ptag;
    uint32_t crc;

    lfs_off_t begin;
    lfs_off_t end;
};

struct lfs_diskoff {
    lfs_block_t block;
    lfs_off_t off;
};

static int32_t lfs_commitget(lfs_t *lfs, lfs_block_t block, lfs_off_t off,
        uint32_t tag, uint32_t getmask, uint32_t gettag, int32_t getdiff,
        void *buffer, bool stopatcommit) {
    // iterate over dir block backwards (for faster lookups)
    while (off >= 2*sizeof(tag)+lfs_tagsize(tag)) {
        off -= sizeof(tag)+lfs_tagsize(tag);

        if (lfs_tagtype(tag) == LFS_TYPE_CRC && stopatcommit) {
            break;
        } else if (lfs_tagtype(tag) == LFS_TYPE_DELETE) {
            if (lfs_tagid(tag) <= lfs_tagid(gettag + getdiff)) {
                getdiff += LFS_MKTAG(0, 1, 0);
            }
        } else if ((tag & getmask) == ((gettag + getdiff) & getmask)) {
            if (buffer) {
                lfs_size_t diff = lfs_min(
                        lfs_tagsize(gettag), lfs_tagsize(tag));
                int err = lfs_bd_read(lfs, block,
                        off+sizeof(tag), buffer, diff);
                if (err) {
                    return err;
                }

                memset((uint8_t*)buffer + diff, 0,
                        lfs_tagsize(gettag) - diff);
            }

            return tag - getdiff;
        }

        uint32_t ntag;
        int err = lfs_bd_read(lfs, block, off, &ntag, sizeof(ntag));
        if (err) {
            return err;
        }
        tag ^= lfs_fromle32(ntag);
    }

    return LFS_ERR_NOENT;
}

static int lfs_commitattrs(lfs_t *lfs, struct lfs_commit *commit,
        uint16_t id, const struct lfs_attr *attrs);

static int lfs_commitmove(lfs_t *lfs, struct lfs_commit *commit,
        uint16_t fromid, uint16_t toid,
        const lfs_mdir_t *dir, const lfs_mattr_t *attrs);

static int lfs_commitattr(lfs_t *lfs, struct lfs_commit *commit,
        uint32_t tag, const void *buffer) {
    if (lfs_tagtype(tag) == LFS_FROM_ATTRS) {
        // special case for custom attributes
        return lfs_commitattrs(lfs, commit,
                lfs_tagid(tag), buffer);
    } else if (lfs_tagtype(tag) == LFS_FROM_MOVE) {
        // special case for moves
        return lfs_commitmove(lfs, commit,
                lfs_tagsize(tag), lfs_tagid(tag),
                buffer, NULL); 
    }

    // check if we fit
    lfs_size_t size = lfs_tagsize(tag);
    if (commit->off + sizeof(tag)+size > commit->end) {
        return LFS_ERR_NOSPC;
    }

    // write out tag
    uint32_t ntag = lfs_tole32((tag & 0x7fffffff) ^ commit->ptag);
    lfs_crc(&commit->crc, &ntag, sizeof(ntag));
    int err = lfs_bd_prog(lfs, commit->block, commit->off,
            &ntag, sizeof(ntag));
    if (err) {
        return err;
    }
    commit->off += sizeof(ntag);

    if (!(tag & 0x80000000)) {
        // from memory
        lfs_crc(&commit->crc, buffer, size);
        err = lfs_bd_prog(lfs, commit->block, commit->off, buffer, size);
        if (err) {
            return err;
        }
    } else {
        // from disk
        const struct lfs_diskoff *disk = buffer;
        for (lfs_off_t i = 0; i < size; i++) {
            // rely on caching to make this efficient
            uint8_t dat;
            int err = lfs_bd_read(lfs, disk->block, disk->off+i, &dat, 1);
            if (err) {
                return err;
            }

            lfs_crc(&commit->crc, &dat, 1);
            err = lfs_bd_prog(lfs, commit->block, commit->off+i, &dat, 1);
            if (err) {
                return err;
            }
        }
    }

    commit->off += size;
    commit->ptag = tag & 0x7fffffff;
    return 0;
}

static int lfs_commitattrs(lfs_t *lfs, struct lfs_commit *commit,
        uint16_t id, const struct lfs_attr *attrs) {
    for (const struct lfs_attr *a = attrs; a; a = a->next) {
        int err = lfs_commitattr(lfs, commit,
                LFS_MKTAG(0x100 | a->type, id, a->size), a->buffer);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfs_commitmove(lfs_t *lfs, struct lfs_commit *commit,
        uint16_t fromid, uint16_t toid,
        const lfs_mdir_t *dir, const lfs_mattr_t *attrs) {
    // iterate through list and commits, only committing unique entries
    lfs_off_t off = dir->off;
    uint32_t ntag = dir->etag;
    while (attrs || off > sizeof(uint32_t)) {
        struct lfs_diskoff disk;
        uint32_t tag;
        const void *buffer;
        if (attrs) {
            tag = attrs->tag;
            buffer = attrs->buffer;
            attrs = attrs->next;
        } else {
            LFS_ASSERT(off > sizeof(ntag)+lfs_tagsize(ntag));
            off -= sizeof(ntag)+lfs_tagsize(ntag);

            tag = ntag;
            buffer = &disk;
            disk.block = dir->pair[0];
            disk.off = off + sizeof(tag);

            int err = lfs_bd_read(lfs, dir->pair[0], off, &ntag, sizeof(ntag));
            if (err) {
                return err;
            }

            ntag = lfs_fromle32(ntag);
            ntag ^= tag;
            tag |= 0x80000000;
        }

        if (lfs_tagtype(tag) == LFS_TYPE_DELETE && lfs_tagid(tag) <= fromid) {
            // something was deleted, we need to move around it
            fromid += 1;
        } else if (lfs_tagid(tag) != fromid) {
            // ignore non-matching ids
        } else {
            // check if type has already been committed
            int32_t res = lfs_commitget(lfs, commit->block,
                    commit->off, commit->ptag,
                    lfs_tagisuser(tag) ? 0x7ffff000 : 0x7c3ff000,
                    LFS_MKTAG(lfs_tagtype(tag), toid, 0),
                    0, NULL, true);
            if (res < 0 && res != LFS_ERR_NOENT) {
                return res;
            }

            if (res == LFS_ERR_NOENT) {
                // update id and commit, as we are currently unique
                int err = lfs_commitattr(lfs, commit,
                        (tag & 0xffc00fff) | LFS_MKTAG(0, toid, 0),
                        buffer);
                if (err) {
                    return err;
                }
            }
        }
    }

    return 0;
}

static int lfs_commitglobals(lfs_t *lfs, struct lfs_commit *commit,
        lfs_global_t *locals) {
    if (lfs_globaliszero(&lfs->locals)) {
        return 0;
    }

    lfs_globalxor(locals, &lfs->locals);
    int err = lfs_commitattr(lfs, commit,
            LFS_MKTAG(LFS_TYPE_GLOBALS, 0x3ff, sizeof(lfs_global_t)), locals);
    lfs_globalxor(locals, &lfs->locals);
    return err;
}

static int lfs_commitcrc(lfs_t *lfs, struct lfs_commit *commit) {
    // align to program units
    lfs_off_t off = lfs_alignup(commit->off + 2*sizeof(uint32_t),
            lfs->cfg->prog_size);

    // read erased state from next program unit
    uint32_t tag;
    int err = lfs_bd_read(lfs, commit->block, off, &tag, sizeof(tag));
    if (err) {
        return err;
    }

    // build crc tag
    tag = lfs_fromle32(tag);
    tag = (0x80000000 & ~tag) |
            LFS_MKTAG(LFS_TYPE_CRC, 0x3ff,
                off - (commit->off+sizeof(uint32_t)));

    // write out crc
    uint32_t footer[2];
    footer[0] = lfs_tole32(tag ^ commit->ptag);
    lfs_crc(&commit->crc, &footer[0], sizeof(footer[0]));
    footer[1] = lfs_tole32(commit->crc);
    err = lfs_bd_prog(lfs, commit->block, commit->off, footer, sizeof(footer));
    if (err) {
        return err;
    }
    commit->off += sizeof(tag)+lfs_tagsize(tag);
    commit->ptag = tag;

    // flush buffers
    err = lfs_bd_sync(lfs);
    if (err) {
        return err;
    }

    // successful commit, check checksum to make sure
    uint32_t crc = 0xffffffff;
    err = lfs_bd_crc(lfs, commit->block, commit->begin,
            commit->off-lfs_tagsize(tag)-commit->begin, &crc);
    if (err) {
        return err;
    }

    if (crc != commit->crc) {
        return LFS_ERR_CORRUPT;
    }

    return 0;
}

// internal dir operations
static int lfs_dir_alloc(lfs_t *lfs, lfs_mdir_t *dir,
        bool split, const lfs_block_t tail[2]) {
    // allocate pair of dir blocks (backwards, so we write to block 1 first)
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[(i+1)%2]);
        if (err) {
            return err;
        }
    }

    // rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs_bd_read(lfs, dir->pair[0], 0, &dir->rev, 4);
    dir->rev = lfs_fromle32(dir->rev);
    if (err) {
        return err;
    }

    // set defaults
    dir->off = sizeof(dir->rev);
    dir->etag = 0;
    dir->count = 0;
    dir->tail[0] = tail[0];
    dir->tail[1] = tail[1];
    dir->erased = false;
    dir->split = split;
    lfs_globalzero(&dir->locals);

    // don't write out yet, let caller take care of that
    return 0;
}

static int lfs_dir_compact(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_mattr_t *attrs,
        lfs_mdir_t *source, uint16_t begin, uint16_t end) {
    // save some state in case block is bad
    const lfs_block_t oldpair[2] = {dir->pair[1], dir->pair[0]};
    bool relocated = false;

    // There's nothing special about our global delta, so feed it back
    // into the global global delta
    lfs_globalxor(&lfs->locals, &dir->locals);
    lfs_globalzero(&dir->locals);

    // increment revision count
    dir->rev += 1;

    while (true) {
        // last complete id
        int16_t ack = -1;
        dir->count = end - begin;

        if (true) {
            // erase block to write to
            int err = lfs_bd_erase(lfs, dir->pair[1]);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // write out header
            uint32_t crc = 0xffffffff;
            uint32_t rev = lfs_tole32(dir->rev);
            lfs_crc(&crc, &rev, sizeof(rev));
            err = lfs_bd_prog(lfs, dir->pair[1], 0, &rev, sizeof(rev));
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // setup compaction
            struct lfs_commit commit = {
                .block = dir->pair[1],
                .off = sizeof(dir->rev),
                .crc = crc,
                .ptag = 0,

                // space is complicated, we need room for tail, crc, globals,
                // and we cap at half a block to give room for metadata updates
                .begin = 0,
                .end = lfs_min(
                    lfs_alignup(lfs->cfg->block_size/2, lfs->cfg->prog_size),
                    lfs->cfg->block_size - 34),
            };

            // commit with a move
            for (uint16_t id = begin; id < end; id++) {
                err = lfs_commitmove(lfs, &commit,
                        id, id - begin, source, attrs);
                if (err) {
                    if (err == LFS_ERR_NOSPC) {
                        goto split;
                    } else if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                ack = id;
            }

            // reopen reserved space at the end
            commit.end = lfs->cfg->block_size - 8;

            if (!relocated) {
                err = lfs_commitglobals(lfs, &commit, &dir->locals);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            if (!lfs_pairisnull(dir->tail)) {
                // commit tail, which may be new after last size check
                // TODO le32
                lfs_pairtole32(dir->tail);
                err = lfs_commitattr(lfs, &commit,
                        LFS_MKTAG(LFS_TYPE_TAIL + dir->split,
                            0x3ff, sizeof(dir->tail)), dir->tail);
                lfs_pairfromle32(dir->tail);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            err = lfs_commitcrc(lfs, &commit);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // successful compaction, swap dir pair to indicate most recent
            lfs_pairswap(dir->pair);
            dir->off = commit.off;
            dir->etag = commit.ptag;
            dir->erased = true;
        }
        break;

split:
        // commit no longer fits, need to split dir,
        // drop caches and create tail
        lfs->pcache.block = 0xffffffff;

        if (ack == -1) {
            // If we can't fit in this block, we won't fit in next block
            return LFS_ERR_NOSPC;
        }

        lfs_mdir_t tail;
        int err = lfs_dir_alloc(lfs, &tail, dir->split, dir->tail);
        if (err) {
            return err;
        }

        err = lfs_dir_compact(lfs, &tail, attrs, dir, ack+1, end);
        if (err) {
            return err;
        }

        end = ack+1;
        dir->tail[0] = tail.pair[0];
        dir->tail[1] = tail.pair[1];
        dir->split = true;
        continue;

relocate:
        //commit was corrupted
        LFS_DEBUG("Bad block at %d", dir->pair[1]);

        // drop caches and prepare to relocate block
        relocated = true;
        lfs->pcache.block = 0xffffffff;

        // can't relocate superblock, filesystem is now frozen
        if (lfs_paircmp(oldpair, (const lfs_block_t[2]){0, 1}) == 0) {
            LFS_WARN("Superblock %d has become unwritable", oldpair[1]);
            return LFS_ERR_CORRUPT;
        }

        // relocate half of pair
        err = lfs_alloc(lfs, &dir->pair[1]);
        if (err) {
            return err;
        }

        continue;
    }

    if (!relocated) {
        // successful commit, update globals
        lfs_globalxor(&dir->locals, &lfs->locals);
        lfs_globalzero(&lfs->locals);
    } else {
        // update references if we relocated
        LFS_DEBUG("Relocating %d %d to %d %d",
                oldpair[0], oldpair[1], dir->pair[0], dir->pair[1]);
        int err = lfs_relocate(lfs, oldpair, dir->pair);
        if (err) {
            return err;
        }
    }

    // update any dirs/files that are affected
    for (int i = 0; i < 2; i++) {
        for (lfs_file_t *f = ((lfs_file_t**)&lfs->files)[i]; f; f = f->next) {
            if (lfs_paircmp(f->pair, dir->pair) == 0 &&
                    f->id >= begin && f->id < end) {
                f->pair[0] = dir->pair[0];
                f->pair[1] = dir->pair[1];
                f->id -= begin;
            }
        }
    }

    return 0;
}

static int lfs_dir_commit(lfs_t *lfs, lfs_mdir_t *dir,
        const lfs_mattr_t *attrs) {
    lfs_mattr_t cancelattr;
    lfs_global_t canceldiff;
    lfs_globalzero(&canceldiff);
    if (lfs_paircmp(dir->pair, lfs_globalmovepair(lfs)) == 0) {
        // Wait, we have the move? Just cancel this out here
        // We need to, or else the move can become outdated
        lfs_globalxormove(&canceldiff,
                lfs_globalmovepair(lfs), lfs_globalmoveid(lfs));
        lfs_globalxormove(&canceldiff, 
                (lfs_block_t[2]){0xffffffff, 0xffffffff}, 0x3ff);
        lfs_globalfromle32(&lfs->locals);
        lfs_globalxor(&lfs->locals, &canceldiff);
        lfs_globaltole32(&lfs->locals);

        cancelattr.tag = LFS_MKTAG(LFS_TYPE_DELETE, lfs_globalmoveid(lfs), 0);
        cancelattr.next = attrs;
        attrs = &cancelattr;
    }

    // calculate new directory size
    uint32_t deletetag = 0xffffffff;
    for (const lfs_mattr_t *a = attrs; a; a = a->next) {
        if (lfs_tagid(a->tag) < 0x3ff && lfs_tagid(a->tag) >= dir->count) {
            dir->count = lfs_tagid(a->tag)+1;
        }

        if (lfs_tagtype(a->tag) == LFS_TYPE_DELETE) {
            LFS_ASSERT(dir->count > 0);
            dir->count -= 1;
            deletetag = a->tag;

            if (dir->count == 0) {
                // should we actually drop the directory block?
                lfs_mdir_t pdir;
                int err = lfs_pred(lfs, dir->pair, &pdir);
                if (err && err != LFS_ERR_NOENT) {
                    return err;
                }

                if (err != LFS_ERR_NOENT && pdir.split) {
                    // steal tail and global state
                    pdir.split = dir->split;
                    pdir.tail[0] = dir->tail[0];
                    pdir.tail[1] = dir->tail[1];
                    lfs_globalxor(&lfs->locals, &dir->locals);
                    return lfs_dir_commit(lfs, &pdir,
                            LFS_MKATTR(LFS_TYPE_TAIL + pdir.split, 0x3ff,
                                pdir.tail, sizeof(pdir.tail),
                            NULL));
                }
            }
        }
    }

    if (!dir->erased) {
compact:
        // fall back to compaction
        lfs->pcache.block = 0xffffffff;
        int err = lfs_dir_compact(lfs, dir, attrs, dir, 0, dir->count);
        if (err) {
            return err;
        }
    } else {
        // try to commit
        struct lfs_commit commit = {
            .block = dir->pair[0],
            .off = dir->off,
            .crc = 0xffffffff,
            .ptag = dir->etag,

            .begin = dir->off,
            .end = lfs->cfg->block_size - 8,
        };

        for (const lfs_mattr_t *a = attrs; a; a = a->next) {
            if (lfs_tagtype(a->tag) != LFS_TYPE_DELETE) {
                lfs_pairtole32(dir->tail);
                int err = lfs_commitattr(lfs, &commit, a->tag, a->buffer);
                lfs_pairfromle32(dir->tail);
                if (err) {
                    if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                        goto compact;
                    }
                    return err;
                }
            }
        }

        if (lfs_tagisvalid(deletetag)) {
            // special case for deletes, since order matters
            int err = lfs_commitattr(lfs, &commit, deletetag, NULL);
            if (err) {
                if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                    goto compact;
                }
                return err;
            }
        }

        int err = lfs_commitglobals(lfs, &commit, &dir->locals);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        err = lfs_commitcrc(lfs, &commit);
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
        lfs_globalxor(&dir->locals, &lfs->locals);
        lfs_globalzero(&lfs->locals);
    }

    // update globals that are affected
    lfs_globalxor(&lfs->globals, &canceldiff);

    // update any directories that are affected
    for (lfs_dir_t *d = lfs->dirs; d; d = d->next) {
        if (lfs_paircmp(d->m.pair, dir->pair) == 0) {
            d->m = *dir;
            if (d->id > lfs_tagid(deletetag)) {
                d->pos -= 1;
            }
        }
    }

    for (int i = 0; i < 2; i++) {
        for (lfs_file_t *f = ((lfs_file_t**)&lfs->files)[i]; f; f = f->next) {
            if (f->id == lfs_tagid(deletetag)) {
                f->pair[0] = 0xffffffff;
                f->pair[1] = 0xffffffff;
            } else if (f->id > lfs_tagid(deletetag)) {
                f->id -= 1;
            }
        }
    }

    return 0;
}

static int32_t lfs_dir_find(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2],
        uint32_t findmask, uint32_t findtag,
        const void *findbuffer) {
    dir->pair[0] = pair[0];
    dir->pair[1] = pair[1];
    int32_t foundtag = LFS_ERR_NOENT;

    // find the block with the most recent revision
    uint32_t rev[2];
    for (int i = 0; i < 2; i++) {
        int err = lfs_bd_read(lfs, dir->pair[i], 0, &rev[i], sizeof(rev[i]));
        if (err) {
            return err;
        }
        rev[i] = lfs_fromle32(rev[i]);
    }

    if (lfs_scmp(rev[1], rev[0]) > 0) {
        lfs_pairswap(dir->pair);
        lfs_pairswap(rev);
    }

    // load blocks and check crc
    for (int i = 0; i < 2; i++) {
        lfs_off_t off = sizeof(dir->rev);
        uint32_t ptag = 0;
        uint32_t crc = 0xffffffff;

        dir->rev = lfs_tole32(rev[0]);
        lfs_crc(&crc, &dir->rev, sizeof(dir->rev));
        dir->rev = lfs_fromle32(dir->rev);
        dir->off = 0;

        uint32_t tempfoundtag = foundtag;
        uint16_t tempcount = 0;
        lfs_block_t temptail[2] = {0xffffffff, 0xffffffff};
        bool tempsplit = false;
        lfs_global_t templocals;
        lfs_globalzero(&templocals);

        while (true) {
            // extract next tag
            uint32_t tag;
            int err = lfs_bd_read(lfs, dir->pair[0],
                    off, &tag, sizeof(tag));
            if (err) {
                return err;
            }

            lfs_crc(&crc, &tag, sizeof(tag));
            tag = lfs_fromle32(tag) ^ ptag;

            // next commit not yet programmed
            if (lfs_tagtype(ptag) == LFS_TYPE_CRC && !lfs_tagisvalid(tag)) {
                dir->erased = true;
                break;
            }

            // check we're in valid range
            if (off + sizeof(tag)+lfs_tagsize(tag) > lfs->cfg->block_size) {
                dir->erased = false;
                break;
            }

            if (lfs_tagtype(tag) == LFS_TYPE_CRC) {
                // check the crc attr
                uint32_t dcrc;
                int err = lfs_bd_read(lfs, dir->pair[0],
                        off+sizeof(tag), &dcrc, sizeof(dcrc));
                if (err) {
                    return err;
                }
                dcrc = lfs_fromle32(dcrc);

                if (crc != dcrc) {
                    dir->erased = false;
                    break;
                }

                foundtag = tempfoundtag;
                dir->off = off + sizeof(tag)+lfs_tagsize(tag);
                dir->etag = tag;
                dir->count = tempcount;
                dir->tail[0] = temptail[0];
                dir->tail[1] = temptail[1];
                dir->split = tempsplit;
                dir->locals = templocals;
                crc = 0xffffffff;
            } else {
                err = lfs_bd_crc(lfs, dir->pair[0],
                        off+sizeof(tag), lfs_tagsize(tag), &crc);
                if (err) {
                    return err;
                }

                if (lfs_tagid(tag) < 0x3ff && lfs_tagid(tag) >= tempcount) {
                    tempcount = lfs_tagid(tag)+1;
                }

                // TODO use subtype accross all of these?
                if (lfs_tagsubtype(tag) == LFS_TYPE_TAIL) {
                    tempsplit = (lfs_tagtype(tag) & 1);
                    err = lfs_bd_read(lfs, dir->pair[0], off+sizeof(tag),
                            temptail, sizeof(temptail));
                    if (err) {
                        return err;
                    }
                    lfs_pairfromle32(temptail);
                } else if (lfs_tagtype(tag) == LFS_TYPE_GLOBALS) {
                    err = lfs_bd_read(lfs, dir->pair[0], off+sizeof(tag),
                            &templocals, sizeof(templocals));
                    if (err) {
                        return err;
                    }
                } else if (lfs_tagtype(tag) == LFS_TYPE_DELETE) {
                    LFS_ASSERT(tempcount > 0);
                    tempcount -= 1;

                    if (lfs_tagid(tag) == lfs_tagid(tempfoundtag)) {
                        tempfoundtag = LFS_ERR_NOENT;
                    } else if (lfs_tagisvalid(tempfoundtag) &&
                            lfs_tagid(tag) < lfs_tagid(tempfoundtag)) {
                        tempfoundtag -= LFS_MKTAG(0, 1, 0);
                    }
                } else if ((tag & findmask) == (findtag & findmask)) {
                    int res = lfs_bd_cmp(lfs, dir->pair[0], off+sizeof(tag),
                            findbuffer, lfs_tagsize(tag));
                    if (res < 0) {
                        return res;
                    }

                    if (res) {
                        // found a match
                        tempfoundtag = tag;
                    }
                }
            }

            ptag = tag;
            off += sizeof(tag)+lfs_tagsize(tag);
        }

        // consider what we have good enough
        if (dir->off > 0) {
            // synthetic move
            if (lfs_paircmp(dir->pair, lfs_globalmovepair(lfs)) == 0) {
                if (lfs_globalmoveid(lfs) == lfs_tagid(foundtag)) {
                    foundtag = LFS_ERR_NOENT;
                } else if (lfs_tagisvalid(foundtag) &&
                        lfs_globalmoveid(lfs) < lfs_tagid(foundtag)) {
                    foundtag -= LFS_MKTAG(0, 1, 0);
                }
            }

            return foundtag;
        }

        // failed, try the other crc?
        lfs_pairswap(dir->pair);
        lfs_pairswap(rev);
    }

    LFS_ERROR("Corrupted dir pair at %d %d", dir->pair[0], dir->pair[1]);
    return LFS_ERR_CORRUPT;
}

static int lfs_dir_fetch(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2]) {
    int32_t res = lfs_dir_find(lfs, dir, pair, 0xffffffff, 0xffffffff, NULL);
    if (res < 0 && res != LFS_ERR_NOENT) {
        return res;
    }

    return 0;
}

static int32_t lfs_dir_get(lfs_t *lfs, lfs_mdir_t *dir,
        uint32_t getmask, uint32_t gettag, void *buffer) {
    int32_t getdiff = 0;
    if (lfs_paircmp(dir->pair, lfs_globalmovepair(lfs)) == 0 &&
            lfs_tagid(gettag) <= lfs_globalmoveid(lfs)) {
        // synthetic moves
        getdiff = LFS_MKTAG(0, 1, 0);
    }

    return lfs_commitget(lfs, dir->pair[0], dir->off, dir->etag,
            getmask, gettag, getdiff, buffer, false);
}

static int32_t lfs_dir_lookup(lfs_t *lfs, lfs_mdir_t *dir, const char **path) {
    // we reduce path to a single name if we can find it
    const char *name = *path;
    *path = NULL;

    // default to root dir
    int32_t tag = LFS_MKTAG(LFS_TYPE_DIR, 0x3ff, 0);
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
        if (lfs_tagtype(tag) != LFS_TYPE_DIR) {
            return LFS_ERR_NOTDIR;
        }

        // grab the entry data
        if (lfs_tagid(tag) != 0x3ff) {
            int32_t res = lfs_dir_get(lfs, dir, 0x7c3ff000,
                    LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tagid(tag), 8), pair);
            if (res < 0) {
                return res;
            }
            lfs_pairfromle32(pair);
        }

        // find entry matching name
        while (true) {
            tag = lfs_dir_find(lfs, dir, pair, 0x7c000fff,
                    LFS_MKTAG(LFS_TYPE_NAME, 0, namelen), name);
            if (tag < 0 && tag != LFS_ERR_NOENT) {
                return tag;
            }

            if (tag != LFS_ERR_NOENT) {
                // found it
                break;
            }

            if (!dir->split) {
                // couldn't find it
                return LFS_ERR_NOENT;
            }

            pair[0] = dir->tail[0];
            pair[1] = dir->tail[1];
        }

        // to next name
        name += namelen;
    }
}

static int lfs_dir_getinfo(lfs_t *lfs, lfs_mdir_t *dir,
        int16_t id, struct lfs_info *info) {
    int32_t tag = lfs_dir_get(lfs, dir, 0x7c3ff000,
            LFS_MKTAG(LFS_TYPE_NAME, id, lfs->name_size+1), info->name);
    if (tag < 0) {
        return tag;
    }

    info->type = lfs_tagtype(tag);

    struct lfs_ctz ctz;
    tag = lfs_dir_get(lfs, dir, 0x7c3ff000,
            LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
    if (tag < 0) {
        return tag;
    }
    lfs_ctzfromle32(&ctz);

    if (lfs_tagtype(tag) == LFS_TYPE_CTZSTRUCT) {
        info->size = ctz.size;
    } else if (lfs_tagtype(tag) == LFS_TYPE_INLINESTRUCT) {
        info->size = lfs_tagsize(tag);
    }

    return 0;
}

/// Top level directory operations ///
int lfs_mkdir(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    lfs_mdir_t cwd;
    int32_t res = lfs_dir_lookup(lfs, &cwd, &path);
    if (!(res == LFS_ERR_NOENT && path)) {
        return (res < 0) ? res : LFS_ERR_EXIST;
    }

    // check that name fits
    lfs_size_t nlen = strlen(path);
    if (nlen > lfs->name_size) {
        return LFS_ERR_NAMETOOLONG;
    }

    // build up new directory
    lfs_alloc_ack(lfs);

    lfs_mdir_t dir;
    err = lfs_dir_alloc(lfs, &dir, false, cwd.tail);
    if (err) {
        return err;
    }

    err = lfs_dir_commit(lfs, &dir, NULL);
    if (err) {
        return err;
    }

    // get next slot and commit
    uint16_t id = cwd.count;
    cwd.tail[0] = dir.pair[0];
    cwd.tail[1] = dir.pair[1];
    lfs_pairtole32(dir.pair);
    err = lfs_dir_commit(lfs, &cwd,
            LFS_MKATTR(LFS_TYPE_DIR, id, path, nlen,
            LFS_MKATTR(LFS_TYPE_DIRSTRUCT, id, dir.pair, sizeof(dir.pair),
            LFS_MKATTR(LFS_TYPE_SOFTTAIL, 0x3ff, cwd.tail, sizeof(cwd.tail),
            NULL))));
    lfs_pairfromle32(dir.pair);
    if (err) {
        return err;
    }

    // TODO need ack here?
    lfs_alloc_ack(lfs);
    return 0;
}

int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
    int32_t tag = lfs_dir_lookup(lfs, &dir->m, &path);
    if (tag < 0) {
        return tag;
    }

    if (lfs_tagtype(tag) != LFS_TYPE_DIR) {
        return LFS_ERR_NOTDIR;
    }

    lfs_block_t pair[2];
    if (lfs_tagid(tag) == 0x3ff) {
        // handle root dir separately
        pair[0] = lfs->root[0];
        pair[1] = lfs->root[1];
    } else {
        // get dir pair from parent
        int32_t res = lfs_dir_get(lfs, &dir->m, 0x7c3ff000,
                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tagid(tag), 8), pair);
        if (res < 0) {
            return res;
        }
        lfs_pairfromle32(pair);
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

    // add to list of directories
    dir->next = lfs->dirs;
    lfs->dirs = dir;

    return 0;
}

int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
    // remove from list of directories
    for (lfs_dir_t **p = &lfs->dirs; *p; p = &(*p)->next) {
        if (*p == dir) {
            *p = dir->next;
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

// TODO does this work?
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

            int err = lfs_dir_fetch(lfs, &dir->m, dir->m.tail);
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
static int lfs_ctzindex(lfs_t *lfs, lfs_off_t *off) {
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

static int lfs_ctzfind(lfs_t *lfs,
        lfs_cache_t *rcache, const lfs_cache_t *pcache,
        lfs_block_t head, lfs_size_t size,
        lfs_size_t pos, lfs_block_t *block, lfs_off_t *off) {
    if (size == 0) {
        *block = 0xffffffff;
        *off = 0;
        return 0;
    }

    lfs_off_t current = lfs_ctzindex(lfs, &(lfs_off_t){size-1});
    lfs_off_t target = lfs_ctzindex(lfs, &pos);

    while (current > target) {
        lfs_size_t skip = lfs_min(
                lfs_npw2(current-target+1) - 1,
                lfs_ctz(current));

        int err = lfs_cache_read(lfs, rcache, pcache, head, 4*skip, &head, 4);
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

static int lfs_ctzextend(lfs_t *lfs,
        lfs_cache_t *rcache, lfs_cache_t *pcache,
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

        if (true) {
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
            lfs_off_t index = lfs_ctzindex(lfs, &size);
            size += 1;

            // just copy out the last block if it is incomplete
            if (size != lfs->cfg->block_size) {
                for (lfs_off_t i = 0; i < size; i++) {
                    uint8_t data;
                    err = lfs_cache_read(lfs, rcache, NULL,
                            head, i, &data, 1);
                    if (err) {
                        return err;
                    }

                    err = lfs_cache_prog(lfs, pcache, rcache,
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
                err = lfs_cache_prog(lfs, pcache, rcache,
                        nblock, 4*i, &head, 4);
                head = lfs_fromle32(head);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                if (i != skips-1) {
                    err = lfs_cache_read(lfs, rcache, NULL,
                            head, 4*i, &head, 4);
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
        }

relocate:
        LFS_DEBUG("Bad block at %d", nblock);

        // just clear cache and try a new block
        pcache->block = 0xffffffff;
    }
}

static int lfs_ctztraverse(lfs_t *lfs,
        lfs_cache_t *rcache, const lfs_cache_t *pcache,
        lfs_block_t head, lfs_size_t size,
        int (*cb)(void*, lfs_block_t), void *data) {
    if (size == 0) {
        return 0;
    }

    lfs_off_t index = lfs_ctzindex(lfs, &(lfs_off_t){size-1});

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
        err = lfs_cache_read(lfs, rcache, pcache, head, 0, &heads, count*4);
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
        int err = lfs_forceconsistency(lfs);
        if (err) {
            return err;
        }
    }

    // allocate entry for file if it doesn't exist
    lfs_mdir_t cwd;
    int32_t tag = lfs_dir_lookup(lfs, &cwd, &path);
    if (tag < 0 && !(tag == LFS_ERR_NOENT && path)) {
        return tag;
    }

    if (tag == LFS_ERR_NOENT) {
        if (!(flags & LFS_O_CREAT)) {
            return LFS_ERR_NOENT;
        }

        // check that name fits
        lfs_size_t nlen = strlen(path);
        if (nlen > lfs->name_size) {
            return LFS_ERR_NAMETOOLONG;
        }

        // get next slot and create entry to remember name
        // TODO do we need to make file registered to list to catch updates from this commit? ie if id/cwd change
        // TODO don't use inline struct? just leave it out?
        uint16_t id = cwd.count;
        int err = lfs_dir_commit(lfs, &cwd,
                LFS_MKATTR(LFS_TYPE_REG, id, path, nlen,
                LFS_MKATTR(LFS_TYPE_INLINESTRUCT, id, NULL, 0,
                NULL)));
        if (err) {
            return err;
        }

        // TODO eh AHHHHHHHHHHHHHH
        if (id >= cwd.count) {
            // catch updates from a compact in the above commit
            id -= cwd.count;
            cwd.pair[0] = cwd.tail[0];
            cwd.pair[1] = cwd.tail[1];
        }

        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, id, 0);
    } else if (flags & LFS_O_EXCL) {
        return LFS_ERR_EXIST;
    } else if (lfs_tagtype(tag) != LFS_TYPE_REG) {
        return LFS_ERR_ISDIR;
    } else if (flags & LFS_O_TRUNC) {
        // truncate if requested
        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, lfs_tagid(tag), 0);
        flags |= LFS_F_DIRTY;
    } else {
        // try to load what's on disk, if it's inlined we'll fix it later
        tag = lfs_dir_get(lfs, &cwd, 0x7c3ff000,
                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tagid(tag), 8), &file->ctz);
        if (tag < 0) {
            return tag;
        }
        lfs_ctzfromle32(&file->ctz);
    }

    // setup file struct
    file->cfg = cfg;
    file->pair[0] = cwd.pair[0];
    file->pair[1] = cwd.pair[1];
    file->id = lfs_tagid(tag);
    file->flags = flags;
    file->pos = 0;

    // fetch attrs
    for (const struct lfs_attr *a = file->cfg->attrs; a; a = a->next) {
        if ((file->flags & 3) != LFS_O_WRONLY) {
            int32_t res = lfs_dir_get(lfs, &cwd, 0x7ffff000,
                    LFS_MKTAG(0x100 | a->type, file->id, a->size), a->buffer);
            if (res < 0 && res != LFS_ERR_NOENT) {
                return res;
            }
        }

        if ((file->flags & 3) != LFS_O_RDONLY) {
            if (a->size > lfs->attr_size) {
                return LFS_ERR_NOSPC;
            }

            file->flags |= LFS_F_DIRTY;
        }
    }

    // allocate buffer if needed
    file->cache.block = 0xffffffff;
    if (file->cfg->buffer) {
        file->cache.buffer = file->cfg->buffer;
    } else if ((file->flags & 3) == LFS_O_RDONLY) {
        file->cache.buffer = lfs_malloc(lfs->cfg->read_size);
        if (!file->cache.buffer) {
            return LFS_ERR_NOMEM;
        }
    } else {
        file->cache.buffer = lfs_malloc(lfs->cfg->prog_size);
        if (!file->cache.buffer) {
            return LFS_ERR_NOMEM;
        }
    }

    if (lfs_tagtype(tag) == LFS_TYPE_INLINESTRUCT) {
        // load inline files
        file->ctz.head = 0xfffffffe;
        file->ctz.size = lfs_tagsize(tag);
        file->flags |= LFS_F_INLINE;
        file->cache.block = file->ctz.head;
        file->cache.off = 0;

        // don't always read (may be new/trunc file)
        if (file->ctz.size > 0) {
            int32_t res = lfs_dir_get(lfs, &cwd, 0x7c3ff000,
                    LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tagid(tag), file->ctz.size),
                    file->cache.buffer);
            if (res < 0) {
                lfs_free(file->cache.buffer);
                return res;
            }
        }
    }

    // add to list of files
    file->next = lfs->files;
    lfs->files = file;

    return 0;
}

int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    static const struct lfs_file_config defaults = {0};
    return lfs_file_opencfg(lfs, file, path, flags, &defaults);
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
    if (file->cfg->buffer) {
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
            err = lfs_cache_read(lfs, &lfs->rcache, &file->cache,
                    file->block, i, &data, 1);
            if (err) {
                return err;
            }

            err = lfs_cache_prog(lfs, &lfs->pcache, &lfs->rcache,
                    nblock, i, &data, 1);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }

        // copy over new state of file
        memcpy(file->cache.buffer, lfs->pcache.buffer, lfs->cfg->prog_size);
        file->cache.block = lfs->pcache.block;
        file->cache.off = lfs->pcache.off;
        lfs->pcache.block = 0xffffffff;

        file->block = nblock;
        return 0;

relocate:
        continue;
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
            lfs->rcache.block = 0xffffffff;

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
                    orig.cache.block = 0xffffffff;
                    lfs->rcache.block = 0xffffffff;
                }
            }

            // write out what we have
            while (true) {
                int err = lfs_cache_flush(lfs, &file->cache, &lfs->rcache);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                break;

relocate:
                LFS_DEBUG("Bad block at %d", file->block);
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
                !lfs_pairisnull(file->pair)) {
            // update dir entry
            // TODO keep list of dirs including these guys for no
            // need of another reload?
            lfs_mdir_t cwd;
            err = lfs_dir_fetch(lfs, &cwd, file->pair);
            if (err) {
                return err;
            }

            uint16_t type;
            const void *buffer;
            lfs_size_t size;
            if (file->flags & LFS_F_INLINE) {
                // inline the whole file
                type = LFS_TYPE_INLINESTRUCT;
                buffer = file->cache.buffer;
                size = file->ctz.size;
            } else {
                // update the ctz reference
                type = LFS_TYPE_CTZSTRUCT;
                buffer = &file->ctz;
                size = sizeof(file->ctz);
            }

            // commit file data and attributes
            lfs_ctztole32(&file->ctz);
            int err = lfs_dir_commit(lfs, &cwd,
                    LFS_MKATTR(type, file->id, buffer, size,
                    LFS_MKATTR(LFS_FROM_ATTRS, file->id, file->cfg->attrs, 0,
                    NULL)));
            lfs_ctzfromle32(&file->ctz);
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
                int err = lfs_ctzfind(lfs, &file->cache, NULL,
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
            file->pos + nsize >= lfs->inline_size) {
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
                    int err = lfs_ctzfind(lfs, &file->cache, NULL,
                            file->ctz.head, file->ctz.size,
                            file->pos-1, &file->block, &file->off);
                    if (err) {
                        file->flags |= LFS_F_ERRED;
                        return err;
                    }

                    // mark cache as dirty since we may have read data into it
                    file->cache.block = 0xffffffff;
                }

                // extend file with new blocks
                lfs_alloc_ack(lfs);
                int err = lfs_ctzextend(lfs, &lfs->rcache, &file->cache,
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
            int err = lfs_cache_prog(lfs, &file->cache, &lfs->rcache,
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
        err = lfs_ctzfind(lfs, &file->cache, NULL,
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

//int lfs_file_getattrs(lfs_t *lfs, lfs_file_t *file,
//        const struct lfs_attr *attrs, int count) {
//    // set to null in case we can't find the attrs (missing file?)
//    for (int j = 0; j < count; j++) {
//        memset(attrs[j].buffer, 0, attrs[j].size);
//    }
//
//    // load from disk if we haven't already been deleted
//    if (!lfs_pairisnull(file->pair)) {
//        lfs_mdir_t cwd;
//        int err = lfs_dir_fetch(lfs, &cwd, file->pair);
//        if (err) {
//            return err;
//        }
//
//        lfs_mattr_t entry = {.off = file->pairoff};
//        err = lfs_dir_get(lfs, &cwd, entry.off, &entry.d, 4);
//        if (err) {
//            return err;
//        }
//        entry.size = lfs_entry_size(&entry);
//
//        err = lfs_dir_getattrs(lfs, &cwd, &entry, attrs, count);
//        if (err) {
//            return err;
//        }
//    }
//
//    // override an attrs we have stored locally
//    for (int i = 0; i < file->attrcount; i++) {
//        for (int j = 0; j < count; j++) {
//            if (attrs[j].type == file->attrs[i].type) {
//                if (attrs[j].size < file->attrs[i].size) {
//                    return LFS_ERR_RANGE;
//                }
//
//                memset(attrs[j].buffer, 0, attrs[j].size);
//                memcpy(attrs[j].buffer,
//                        file->attrs[i].buffer, file->attrs[i].size);
//            }
//        }
//    }
//
//    return 0;
//}

//int lfs_file_setattrs(lfs_t *lfs, lfs_file_t *file,
//        const struct lfs_attr *attrs, int count) {
//    if ((file->flags & 3) == LFS_O_RDONLY) {
//        return LFS_ERR_BADF;
//    }
//
//    // at least make sure attributes fit
//    if (!lfs_pairisnull(file->pair)) {
//        lfs_mdir_t cwd;
//        int err = lfs_dir_fetch(lfs, &cwd, file->pair);
//        if (err) {
//            return err;
//        }
//
//        lfs_mattr_t entry = {.off = file->pairoff};
//        err = lfs_dir_get(lfs, &cwd, entry.off, &entry.d, 4);
//        if (err) {
//            return err;
//        }
//        entry.size = lfs_entry_size(&entry);
//
//        lfs_ssize_t res = lfs_dir_checkattrs(lfs, &cwd, &entry, attrs, count);
//        if (res < 0) {
//            return res;
//        }
//    }
//
//    // just tack to the file, will be written at sync time
//    file->attrs = attrs;
//    file->attrcount = count;
//    file->flags |= LFS_F_DIRTY;
//
//    return 0;
//}


/// General fs operations ///
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info) {
    lfs_mdir_t cwd;
    // TODO pass to getinfo?
    int32_t tag = lfs_dir_lookup(lfs, &cwd, &path);
    if (tag < 0) {
        return tag;
    }

    if (lfs_tagid(tag) == 0x3ff) {
        // special case for root
        strcpy(info->name, "/");
        info->type = LFS_TYPE_DIR;
        return 0;
    }

    return lfs_dir_getinfo(lfs, &cwd, lfs_tagid(tag), info);
}

int lfs_remove(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    int err = lfs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    lfs_mdir_t cwd;
    err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    int32_t tag = lfs_dir_lookup(lfs, &cwd, &path);
    if (tag < 0) {
        return tag;
    }

    lfs_mdir_t dir;
    if (lfs_tagtype(tag) == LFS_TYPE_DIR) {
        // must be empty before removal
        lfs_block_t pair[2];
        int32_t res = lfs_dir_get(lfs, &cwd, 0x7c3ff000,
                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tagid(tag), 8), pair);
        if (res < 0) {
            return res;
        }
        lfs_pairfromle32(pair);

        int err = lfs_dir_fetch(lfs, &dir, pair);
        if (err) {
            return err;
        }

        // TODO lfs_dir_empty?
        if (dir.count > 0 || dir.split) {
            return LFS_ERR_NOTEMPTY;
        }

        // mark fs as orphaned
        lfs_globaldeorphaned(lfs, false);
    }

    // delete the entry
    err = lfs_dir_commit(lfs, &cwd,
            LFS_MKATTR(LFS_TYPE_DELETE, lfs_tagid(tag), NULL, 0,
            NULL));
    if (err) {
        return err;
    }

    if (lfs_tagtype(tag) == LFS_TYPE_DIR) {
        int err = lfs_pred(lfs, dir.pair, &cwd);
        if (err) {
            return err;
        }

        // fix orphan
        lfs_globaldeorphaned(lfs, true);

        // steal state
        // TODO test for global state stealing?
        cwd.tail[0] = dir.tail[0];
        cwd.tail[1] = dir.tail[1];
        lfs_globalxor(&lfs->locals, &dir.locals);
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
    int err = lfs_forceconsistency(lfs);
    if (err) {
        return err;
    }

    // find old entry
    lfs_mdir_t oldcwd;
    int32_t oldtag = lfs_dir_lookup(lfs, &oldcwd, &oldpath);
    if (oldtag < 0) {
        return oldtag;
    }

    // find new entry
    lfs_mdir_t newcwd;
    int32_t prevtag = lfs_dir_lookup(lfs, &newcwd, &newpath);
    if (prevtag < 0 && prevtag != LFS_ERR_NOENT) {
        return prevtag;
    }

    uint16_t newid = lfs_tagid(prevtag);
    //bool prevexists = (prevtag != LFS_ERR_NOENT);
    //bool samepair = (lfs_paircmp(oldcwd.pair, newcwd.pair) == 0);

    lfs_mdir_t prevdir;
    if (prevtag != LFS_ERR_NOENT) {
        // check that we have same type
        if (lfs_tagtype(prevtag) != lfs_tagtype(oldtag)) {
            return LFS_ERR_ISDIR;
        }

        if (lfs_tagtype(prevtag) == LFS_TYPE_DIR) {
            // must be empty before removal
            lfs_block_t prevpair[2];
            int32_t res = lfs_dir_get(lfs, &newcwd, 0x7c3ff000,
                    LFS_MKTAG(LFS_TYPE_STRUCT, newid, 8), prevpair);
            if (res < 0) {
                return res;
            }
            lfs_pairfromle32(prevpair);

            // must be empty before removal
            int err = lfs_dir_fetch(lfs, &prevdir, prevpair);
            if (err) {
                return err;
            }

            if (prevdir.count > 0 || prevdir.split) {
                return LFS_ERR_NOTEMPTY;
            }

            // mark fs as orphaned
            lfs_globaldeorphaned(lfs, false);
        }
    } else {
        // check that name fits
        lfs_size_t nlen = strlen(newpath);
        if (nlen > lfs->name_size) {
            return LFS_ERR_NAMETOOLONG;
        }

        // get next id
        newid = newcwd.count;
    }

    // create move to fix later
    lfs_globalmove(lfs, oldcwd.pair, lfs_tagid(oldtag));

    // move over all attributes
    err = lfs_dir_commit(lfs, &newcwd,
            LFS_MKATTR(lfs_tagtype(oldtag), newid, newpath, strlen(newpath),
            LFS_MKATTR(LFS_FROM_MOVE, newid, &oldcwd, lfs_tagid(oldtag),
            NULL)));
    if (err) {
        return err;
    }

    // let commit clean up after move (if we're different! otherwise move
    // logic already fixed it for us)
    if (lfs_paircmp(oldcwd.pair, newcwd.pair) != 0) {
        err = lfs_dir_commit(lfs, &oldcwd, NULL);
        if (err) {
            return err;
        }
    }

    if (prevtag != LFS_ERR_NOENT && lfs_tagtype(prevtag) == LFS_TYPE_DIR) {
        int err = lfs_pred(lfs, prevdir.pair, &newcwd);
        if (err) {
            return err;
        }

        // fix orphan
        lfs_globaldeorphaned(lfs, true);

        // steal state
        // TODO test for global state stealing?
        newcwd.tail[0] = prevdir.tail[0];
        newcwd.tail[1] = prevdir.tail[1];
        lfs_globalxor(&lfs->locals, &prevdir.locals);
        err = lfs_dir_commit(lfs, &newcwd,
                LFS_MKATTR(LFS_TYPE_SOFTTAIL, 0x3ff,
                    newcwd.tail, sizeof(newcwd.tail),
                NULL));
        if (err) {
            return err;
        }
    }

    return 0;
    

//    if (samepair) {
//        // update pair if newcwd == oldcwd
//        oldcwd = newcwd;
//    }
//
//    err = fix
//
//    // remove old entry
//    //printf("RENAME DELETE %d %d %d\n", oldcwd.pair[0], oldcwd.pair[1], oldid);
//    err = lfs_dir_delete(lfs, &oldcwd, oldid);
//    if (err) {
//        return err;
//    }
//
//    // if we were a directory, find pred, replace tail
//    // TODO can this just deorphan?
//    if (prevexists && lfs_tagsubtype(prevattr.tag) == LFS_TYPE_DIR) {
//        err = lfs_forceconsistency(lfs);
//        if (err) {
//            return err;
//        }
//    }
//
    return 0;
}

lfs_ssize_t lfs_getattr(lfs_t *lfs, const char *path,
        uint8_t type, void *buffer, lfs_size_t size) {
    lfs_mdir_t cwd;
    int32_t res = lfs_dir_lookup(lfs, &cwd, &path);
    if (res < 0) {
        return res;
    }

    res = lfs_dir_get(lfs, &cwd, 0x7ffff000,
            LFS_MKTAG(0x100 | type, lfs_tagid(res),
                lfs_min(size, lfs->attr_size)), buffer);
    if (res < 0) {
        if (res == LFS_ERR_NOENT) {
            return LFS_ERR_NOATTR;
        }
        return res;
    }

    return lfs_tagsize(res);
}

int lfs_setattr(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size) {
    if (size > lfs->attr_size) {
        return LFS_ERR_NOSPC;
    }

    lfs_mdir_t cwd;
    int32_t res = lfs_dir_lookup(lfs, &cwd, &path);
    if (res < 0) {
        return res;
    }

    return lfs_dir_commit(lfs, &cwd,
        LFS_MKATTR(0x100 | type, lfs_tagid(res), buffer, size,
        NULL));
}

lfs_ssize_t lfs_fs_getattr(lfs_t *lfs,
        uint8_t type, void *buffer, lfs_size_t size) {
    lfs_mdir_t superdir;
    int err = lfs_dir_fetch(lfs, &superdir, (const lfs_block_t[2]){0, 1});
    if (err) {
        return err;
    }

    int32_t res = lfs_dir_get(lfs, &superdir, 0x7ffff000,
            LFS_MKTAG(0x100 | type, 0,
                lfs_min(size, lfs->attr_size)), buffer);
    if (res < 0) {
        if (res == LFS_ERR_NOENT) {
            return LFS_ERR_NOATTR;
        }
        return res;
    }

    return lfs_tagsize(res);
}

int lfs_fs_setattr(lfs_t *lfs,
        uint8_t type, const void *buffer, lfs_size_t size) {
    if (size > lfs->attr_size) {
        return LFS_ERR_NOSPC;
    }

    lfs_mdir_t superdir;
    int err = lfs_dir_fetch(lfs, &superdir, (const lfs_block_t[2]){0, 1});
    if (err) {
        return err;
    }

    return lfs_dir_commit(lfs, &superdir,
        LFS_MKATTR(0x100 | type, 0, buffer, size,
        NULL));
}

//
//
//
//        const struct lfs_attr *attrs, int count) {
//    lfs_mdir_t cwd;
//    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
//    if (err) {
//        return err;
//    }
//
//    lfs_mattr_t entry;
//    err = lfs_dir_lookup(lfs, &cwd, &entry, &path);
//    if (err) {
//        return err;
//    }
//
//    return lfs_dir_getattrs(lfs, &cwd, &entry, attrs, count);
//}
//
//int lfs_setattrs(lfs_t *lfs, const char *path,
//        const struct lfs_attr *attrs, int count) {
//    lfs_mdir_t cwd;
//    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
//    if (err) {
//        return err;
//    }
//
//    lfs_mattr_t entry;
//    err = lfs_dir_lookup(lfs, &cwd, &entry, &path);
//    if (err) {
//        return err;
//    }
//
//    return lfs_dir_setattrs(lfs, &cwd, &entry, attrs, count);
//}


/// Filesystem operations ///
static inline void lfs_superblockfromle32(lfs_superblock_t *superblock) {
    superblock->version     = lfs_fromle32(superblock->version);
    superblock->block_size  = lfs_fromle32(superblock->block_size);
    superblock->block_count = lfs_fromle32(superblock->block_count);
    superblock->inline_size = lfs_fromle32(superblock->inline_size);
    superblock->attr_size   = lfs_fromle32(superblock->attr_size);
    superblock->name_size   = lfs_fromle32(superblock->name_size);
}

static inline void lfs_superblocktole32(lfs_superblock_t *superblock) {
    superblock->version     = lfs_tole32(superblock->version);
    superblock->block_size  = lfs_tole32(superblock->block_size);
    superblock->block_count = lfs_tole32(superblock->block_count);
    superblock->inline_size = lfs_tole32(superblock->inline_size);
    superblock->attr_size   = lfs_tole32(superblock->attr_size);
    superblock->name_size   = lfs_tole32(superblock->name_size);
}

static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg) {
    lfs->cfg = cfg;

    // setup read cache
    lfs->rcache.block = 0xffffffff;
    if (lfs->cfg->read_buffer) {
        lfs->rcache.buffer = lfs->cfg->read_buffer;
    } else {
        lfs->rcache.buffer = lfs_malloc(lfs->cfg->read_size);
        if (!lfs->rcache.buffer) {
            return LFS_ERR_NOMEM;
        }
    }

    // setup program cache
    lfs->pcache.block = 0xffffffff;
    if (lfs->cfg->prog_buffer) {
        lfs->pcache.buffer = lfs->cfg->prog_buffer;
    } else {
        lfs->pcache.buffer = lfs_malloc(lfs->cfg->prog_size);
        if (!lfs->pcache.buffer) {
            return LFS_ERR_NOMEM;
        }
    }

    // setup lookahead, round down to nearest 32-bits
    LFS_ASSERT(lfs->cfg->lookahead % 32 == 0);
    LFS_ASSERT(lfs->cfg->lookahead > 0);
    if (lfs->cfg->lookahead_buffer) {
        lfs->free.buffer = lfs->cfg->lookahead_buffer;
    } else {
        lfs->free.buffer = lfs_malloc(lfs->cfg->lookahead/8);
        if (!lfs->free.buffer) {
            return LFS_ERR_NOMEM;
        }
    }

    // check that program and read sizes are multiples of the block size
    LFS_ASSERT(lfs->cfg->prog_size % lfs->cfg->read_size == 0);
    LFS_ASSERT(lfs->cfg->block_size % lfs->cfg->prog_size == 0);

    // check that the block size is large enough to fit ctz pointers
    LFS_ASSERT(4*lfs_npw2(0xffffffff / (lfs->cfg->block_size-2*4))
            <= lfs->cfg->block_size);

    // check that the size limits are sane
    LFS_ASSERT(lfs->cfg->inline_size <= LFS_INLINE_MAX);
    LFS_ASSERT(lfs->cfg->inline_size <= lfs->cfg->read_size);
    lfs->inline_size = lfs->cfg->inline_size;
    if (!lfs->inline_size) {
        lfs->inline_size = lfs_min(LFS_INLINE_MAX, lfs->cfg->read_size);
    }

    LFS_ASSERT(lfs->cfg->attr_size <= LFS_ATTR_MAX);
    lfs->attr_size = lfs->cfg->attr_size;
    if (!lfs->attr_size) {
        lfs->attr_size = LFS_ATTR_MAX;
    }

    LFS_ASSERT(lfs->cfg->name_size <= LFS_NAME_MAX);
    lfs->name_size = lfs->cfg->name_size;
    if (!lfs->name_size) {
        lfs->name_size = LFS_NAME_MAX;
    }

    // setup default state
    lfs->root[0] = 0xffffffff;
    lfs->root[1] = 0xffffffff;
    lfs->files = NULL;
    lfs->dirs = NULL;
    lfs_globalones(&lfs->globals);
    lfs_globalzero(&lfs->locals);

    return 0;
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

    // create superblock dir
    lfs_mdir_t dir;
    err = lfs_dir_alloc(lfs, &dir, false,
            (const lfs_block_t[2]){0xffffffff, 0xffffffff});
    if (err) {
        return err;
    }

    // write root directory
    lfs_mdir_t root;
    err = lfs_dir_alloc(lfs, &root, false,
            (const lfs_block_t[2]){0xffffffff, 0xffffffff});
    if (err) {
        return err;
    }

    err = lfs_dir_commit(lfs, &root, NULL);
    if (err) {
        return err;
    }

    lfs->root[0] = root.pair[0];
    lfs->root[1] = root.pair[1];
    dir.tail[0] = lfs->root[0];
    dir.tail[1] = lfs->root[1];

    // write one superblock
    lfs_superblock_t superblock = {
        .magic = {"littlefs"},
        .version = LFS_DISK_VERSION,

        .block_size  = lfs->cfg->block_size,
        .block_count = lfs->cfg->block_count,
        .inline_size = lfs->inline_size,
        .attr_size   = lfs->attr_size,
        .name_size   = lfs->name_size,
    };

    lfs_superblocktole32(&superblock);
    lfs_pairtole32(lfs->root);
    err = lfs_dir_commit(lfs, &dir,
            LFS_MKATTR(LFS_TYPE_SUPERBLOCK, 0, &superblock, sizeof(superblock),
            LFS_MKATTR(LFS_TYPE_DIRSTRUCT, 0, lfs->root, sizeof(lfs->root),
            NULL)));
    lfs_pairfromle32(lfs->root);
    lfs_superblockfromle32(&superblock);
    if (err) {
        return err;
    }

    // sanity check that fetch works
    err = lfs_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
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
    lfs->free.off = 0;
    lfs->free.size = 0;
    lfs->free.i = 0;
    lfs_alloc_ack(lfs);

    // load superblock
    lfs_mdir_t dir;
    err = lfs_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
    if (err) {
        if (err == LFS_ERR_CORRUPT) {
            LFS_ERROR("Invalid superblock at %d %d", 0, 1);
        }
        return err;
    }

    lfs_superblock_t superblock;
    int32_t res = lfs_dir_get(lfs, &dir, 0x7ffff000,
            LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, sizeof(superblock)),
            &superblock);
    if (res < 0) {
        return res;
    }
    lfs_superblockfromle32(&superblock);

    if (memcmp(superblock.magic, "littlefs", 8) != 0) {
        LFS_ERROR("Invalid superblock at %d %d", 0, 1);
        return LFS_ERR_CORRUPT;
    }

    uint16_t major_version = (0xffff & (superblock.version >> 16));
    uint16_t minor_version = (0xffff & (superblock.version >>  0));
    if ((major_version != LFS_DISK_VERSION_MAJOR ||
         minor_version > LFS_DISK_VERSION_MINOR)) {
        LFS_ERROR("Invalid version %d.%d", major_version, minor_version);
        return LFS_ERR_INVAL;
    }

    res = lfs_dir_get(lfs, &dir, 0x7ffff000,
            LFS_MKTAG(LFS_TYPE_DIRSTRUCT, 0, sizeof(lfs->root)),
            &lfs->root);
    if (res < 0) {
        return res;
    }
    lfs_pairfromle32(lfs->root);

    if (superblock.inline_size) {
        if (superblock.inline_size > lfs->inline_size) {
            LFS_ERROR("Unsupported inline size (%d > %d)",
                    superblock.inline_size, lfs->inline_size);
            return LFS_ERR_INVAL;
        }

        lfs->inline_size = superblock.inline_size;
    }

    if (superblock.attr_size) {
        if (superblock.attr_size > lfs->attr_size) {
            LFS_ERROR("Unsupported attr size (%d > %d)",
                    superblock.attr_size, lfs->attr_size);
            return LFS_ERR_INVAL;
        }

        lfs->attr_size = superblock.attr_size;
    }

    if (superblock.name_size) {
        if (superblock.name_size > lfs->name_size) {
            LFS_ERROR("Unsupported name size (%d > %d)",
                    superblock.name_size, lfs->name_size);
            return LFS_ERR_INVAL;
        }

        lfs->name_size = superblock.name_size;
    }

    // scan for any global updates
    err = lfs_scan(lfs);
    if (err) {
        return err;
    }

    return 0;
}

int lfs_unmount(lfs_t *lfs) {
    return lfs_deinit(lfs);
}


/// Internal filesystem filesystem operations ///
int lfs_fs_traverse(lfs_t *lfs,
        int (*cb)(void *data, lfs_block_t block), void *data) {
    if (lfs_pairisnull(lfs->root)) {
        return 0;
    }

    // iterate over metadata pairs
    lfs_mdir_t dir = {.tail = {0, 1}};
    while (!lfs_pairisnull(dir.tail)) {
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
            int32_t tag = lfs_dir_get(lfs, &dir, 0x7c3ff000,
                    LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
            if (tag < 0) {
                if (tag == LFS_ERR_NOENT) {
                    continue;
                }
                return tag;
            }
            lfs_ctzfromle32(&ctz);

            if (lfs_tagtype(tag) == LFS_TYPE_CTZSTRUCT) {
                int err = lfs_ctztraverse(lfs, &lfs->rcache, NULL,
                        ctz.head, ctz.size, cb, data);
                if (err) {
                    return err;
                }
            }
        }
    }

    // iterate over any open files
    for (lfs_file_t *f = lfs->files; f; f = f->next) {
        if ((f->flags & LFS_F_DIRTY) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctztraverse(lfs, &lfs->rcache, &f->cache,
                    f->ctz.head, f->ctz.size, cb, data);
            if (err) {
                return err;
            }
        }

        if ((f->flags & LFS_F_WRITING) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctztraverse(lfs, &lfs->rcache, &f->cache,
                    f->block, f->pos, cb, data);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}
/*
int lfs_fs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data) {
    if (lfs_pairisnull(lfs->root)) {
        return 0;
    }

    // iterate over metadata pairs
    lfs_block_t cwd[2] = {0, 1};

    while (true) {
        for (int i = 0; i < 2; i++) {
            int err = cb(data, cwd[i]);
            if (err) {
                return err;
            }
        }

        lfs_mdir_t dir;
        int err = lfs_dir_fetch(lfs, &dir, cwd);
        if (err) {
            return err;
        }

        // iterate over contents
        lfs_mattr_t entry;
        while (dir.off + sizeof(entry.d) <= (0x7fffffff & dir.d.size)-4) {
            err = lfs_dir_get(lfs, &dir,
                    dir.off, &entry.d, sizeof(entry.d));
            lfs_entry_fromle32(&entry.d);
            if (err) {
                return err;
            }

            dir.off += lfs_entry_size(&entry);
            if ((0x70 & entry.d.type) == LFS_TYPE_CTZSTRUCT) {
                err = lfs_ctztraverse(lfs, &lfs->rcache, NULL,
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
        if ((f->flags & LFS_F_DIRTY) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctztraverse(lfs, &lfs->rcache, &f->cache,
                    f->head, f->size, cb, data);
            if (err) {
                return err;
            }
        }

        if ((f->flags & LFS_F_WRITING) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctztraverse(lfs, &lfs->rcache, &f->cache,
                    f->block, f->pos, cb, data);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}
*/
static int lfs_pred(lfs_t *lfs, const lfs_block_t pair[2], lfs_mdir_t *pdir) {
    if (lfs_pairisnull(lfs->root)) {
        return LFS_ERR_NOENT;
    }

    // iterate over all directory directory entries
    pdir->tail[0] = 0;
    pdir->tail[1] = 1;
    while (!lfs_pairisnull(pdir->tail)) {
        if (lfs_paircmp(pdir->tail, pair) == 0) {
            //return true; // TODO should we return true only if pred is part of dir?
            return 0;
        }

        int err = lfs_dir_fetch(lfs, pdir, pdir->tail);
        if (err) {
            return err;
        }
    }

    return LFS_ERR_NOENT;
}

static int32_t lfs_parent(lfs_t *lfs, const lfs_block_t pair[2],
        lfs_mdir_t *parent) {
    if (lfs_pairisnull(lfs->root)) {
        return LFS_ERR_NOENT;
    }

    // search for both orderings so we can reuse the find function
    lfs_block_t child[2] = {pair[0], pair[1]};
    lfs_pairtole32(child);
    for (int i = 0; i < 2; i++) {
        // iterate over all directory directory entries
        parent->tail[0] = 0;
        parent->tail[1] = 1;
        while (!lfs_pairisnull(parent->tail)) {
            int32_t tag = lfs_dir_find(lfs, parent, parent->tail, 0x7fc00fff,
                    LFS_MKTAG(LFS_TYPE_DIRSTRUCT, 0, sizeof(child)),
                    child);
            if (tag != LFS_ERR_NOENT) {
                return tag;
            }
        }

        lfs_pairswap(child);
    }

    return LFS_ERR_NOENT;
}

// TODO rename to lfs_dir_relocate?
static int lfs_relocate(lfs_t *lfs,
        const lfs_block_t oldpair[2], lfs_block_t newpair[2]) {
    // TODO name lfs_dir_relocate?
    // find parent
    lfs_mdir_t parent;
    int32_t tag = lfs_parent(lfs, oldpair, &parent);
    if (tag < 0 && tag != LFS_ERR_NOENT) {
        return tag;
    }

    if (tag != LFS_ERR_NOENT) {
        // update disk, this creates a desync
        lfs_pairtole32(newpair);
        int err = lfs_dir_commit(lfs, &parent,
                &(lfs_mattr_t){.tag=tag, .buffer=newpair});
        lfs_pairfromle32(newpair);
        if (err) {
            return err;
        }

        // update internal root
        if (lfs_paircmp(oldpair, lfs->root) == 0) {
            LFS_DEBUG("Relocating root %d %d", newpair[0], newpair[1]);
            lfs->root[0] = newpair[0];
            lfs->root[1] = newpair[1];
        }

        // clean up bad block, which should now be a desync
        return lfs_forceconsistency(lfs);
    }

    // find pred
    int err = lfs_pred(lfs, oldpair, &parent);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    // if we can't find dir, it must be new
    if (err != LFS_ERR_NOENT) {
        // just replace bad pair, no desync can occur
        parent.tail[0] = newpair[0];
        parent.tail[1] = newpair[1];
        int err = lfs_dir_commit(lfs, &parent,
                LFS_MKATTR(LFS_TYPE_TAIL + parent.split, 0x3ff,
                    parent.tail, sizeof(parent.tail),
                NULL));
        if (err) {
            return err;
        }
    }

    return 0;
}

int lfs_scan(lfs_t *lfs) {
    if (lfs_pairisnull(lfs->root)) {
        return 0;
    }

    // iterate over all directory directory entries
    lfs_mdir_t dir = {.tail = {0, 1}};
    while (!lfs_pairisnull(dir.tail)) {
        int err = lfs_dir_fetch(lfs, &dir, dir.tail);
        if (err) {
            return err;
        }

        // xor together indirect deletes
        lfs_globalxor(&lfs->locals, &dir.locals);
    }

    // update littlefs with globals
    // TODO does this only run once?
    // TODO Should we inline this into init??
    lfs_globalfromle32(&lfs->locals);
    lfs_globalxor(&lfs->globals, &lfs->locals);
    lfs_globalzero(&lfs->locals);
    if (!lfs_pairisnull(lfs_globalmovepair(lfs))) {
        LFS_DEBUG("Found move %d %d %d",
                lfs_globalmovepair(lfs)[0],
                lfs_globalmovepair(lfs)[1],
                lfs_globalmoveid(lfs));
    }

    return 0;
}

int lfs_forceconsistency(lfs_t *lfs) {
    if (!lfs_globalisdeorphaned(lfs)) {
        // Fix any orphans
        lfs_mdir_t pdir = {.split = true};
        lfs_mdir_t dir = {.tail = {0, 1}};

        // iterate over all directory directory entries
        while (!lfs_pairisnull(dir.tail)) {
            int err = lfs_dir_fetch(lfs, &dir, dir.tail);
            if (err) {
                return err;
            }

            // check head blocks for orphans
            if (!pdir.split) {
                // check if we have a parent
                lfs_mdir_t parent;
                int32_t tag = lfs_parent(lfs, pdir.tail, &parent);
                if (tag < 0 && tag != LFS_ERR_NOENT) {
                    return tag;
                }

                if (tag == LFS_ERR_NOENT) {
                    // we are an orphan
                    LFS_DEBUG("Found orphan %d %d",
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
                int32_t res = lfs_dir_get(lfs, &parent, 0x7ffff000, tag, pair);
                if (res < 0) {
                    return res;
                }
                lfs_pairfromle32(pair);

                if (!lfs_pairsync(pair, pdir.tail)) {
                    // we have desynced
                    LFS_DEBUG("Found half-orphan %d %d", pair[0], pair[1]);

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

        // mark orphan as fixed
        lfs_globaldeorphaned(lfs, false);
    }

    if (lfs_globalmoveid(lfs) != 0x3ff) {
        // Fix bad moves
        LFS_DEBUG("Fixing move %d %d %d", // TODO move to just deorphan?
                lfs_globalmovepair(lfs)[0],
                lfs_globalmovepair(lfs)[1],
                lfs_globalmoveid(lfs));

        // fetch and delete the moved entry
        lfs_mdir_t movedir;
        int err = lfs_dir_fetch(lfs, &movedir, lfs_globalmovepair(lfs));
        if (err) {
            return err;
        }

        // rely on cancel logic inside commit
        err = lfs_dir_commit(lfs, &movedir, NULL);
        if (err) {
            return err;
        }
    }

    return 0;
}

/// External filesystem filesystem operations ///
//int lfs_fs_getattrs(lfs_t *lfs, const struct lfs_attr *attrs, int count) {
//    lfs_mdir_t dir;
//    int err = lfs_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
//    if (err) {
//        return err;
//    }
//
//    lfs_mattr_t entry = {.off = sizeof(dir.d)};
//    err = lfs_dir_get(lfs, &dir, entry.off, &entry.d, 4);
//    if (err) {
//        return err;
//    }
//    entry.size = lfs_entry_size(&entry);
//
//    if (err != LFS_ERR_NOENT) {
//        if (!err) {
//            break;
//        }
//        return err;
//    }
//
//    lfs_mdir_t cwd;
//    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
//    if (err) {
//        return err;
//    }
//
//    lfs_mattr_t entry;
//    err = lfs_dir_lookup(lfs, &cwd, &entry, &path);
//    if (err) {
//        return err;
//    }
//
//    return lfs_dir_getinfo(lfs, &cwd, &entry, info);
//    return lfs_dir_getattrs(lfs, &dir, &entry, attrs, count);
//}
//
//int lfs_fs_setattrs(lfs_t *lfs, const struct lfs_attr *attrs, int count) {
//    lfs_mdir_t dir;
//    int err = lfs_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
//    if (err) {
//        return err;
//    }
//
//    lfs_mattr_t entry = {.off = sizeof(dir.d)};
//    err = lfs_dir_get(lfs, &dir, entry.off, &entry.d, 4);
//    if (err) {
//        return err;
//    }
//    entry.size = lfs_entry_size(&entry);
//
//    return lfs_dir_setattrs(lfs, &dir, &entry, attrs, count);
//}

// TODO need lfs?
static int lfs_fs_size_count(void *p, lfs_block_t block) {
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
