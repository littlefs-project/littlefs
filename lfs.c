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
int lfs_fs_traverse(lfs_t *lfs,
        int (*cb)(lfs_t*, void*, lfs_block_t), void *data);
static int lfs_pred(lfs_t *lfs, const lfs_block_t dir[2], lfs_mdir_t *pdir);
static int lfs_parent(lfs_t *lfs, const lfs_block_t dir[2],
        lfs_mdir_t *parent, lfs_mattr_t *attr);
static int lfs_relocate(lfs_t *lfs,
        const lfs_block_t oldpair[2], const lfs_block_t newpair[2]);
int lfs_scan(lfs_t *lfs);
int lfs_fixmove(lfs_t *lfs);
int lfs_deorphan(lfs_t *lfs);


/// Block allocator ///
static int lfs_alloc_lookahead(lfs_t *lfs, void *p, lfs_block_t block) {
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
        int err = lfs_fs_traverse(lfs, lfs_alloc_lookahead, NULL);
        if (err) {
            return err;
        }
    }
}

static void lfs_alloc_ack(lfs_t *lfs) {
    lfs->free.ack = lfs->cfg->block_count;
}


/// Endian swapping functions ///
//static void lfs_dir_fromle32(struct lfs_disk_dir *d) {
//    d->rev     = lfs_fromle32(d->rev);
//    d->size    = lfs_fromle32(d->size);
//    d->tail[0] = lfs_fromle32(d->tail[0]);
//    d->tail[1] = lfs_fromle32(d->tail[1]);
//}
//
//static void lfs_mdir_tole32(struct lfs_disk_dir *d) {
//    d->rev     = lfs_tole32(d->rev);
//    d->size    = lfs_tole32(d->size);
//    d->tail[0] = lfs_tole32(d->tail[0]);
//    d->tail[1] = lfs_tole32(d->tail[1]);
//}
//
//static void lfs_entry_fromle32(struct lfs_disk_entry *d) {
//    d->u.dir[0] = lfs_fromle32(d->u.dir[0]);
//    d->u.dir[1] = lfs_fromle32(d->u.dir[1]);
//}
//
//static void lfs_entry_tole32(struct lfs_disk_entry *d) {
//    d->u.dir[0] = lfs_tole32(d->u.dir[0]);
//    d->u.dir[1] = lfs_tole32(d->u.dir[1]);
//}

///*static*/ void lfs_superblock_fromle32(struct lfs_disk_superblock *d) {
//    d->root[0]     = lfs_fromle32(d->root[0]);
//    d->root[1]     = lfs_fromle32(d->root[1]);
//    d->block_size  = lfs_fromle32(d->block_size);
//    d->block_count = lfs_fromle32(d->block_count);
//    d->version     = lfs_fromle32(d->version);
//    d->inline_size = lfs_fromle32(d->inline_size);
//    d->attrs_size  = lfs_fromle32(d->attrs_size);
//    d->name_size   = lfs_fromle32(d->name_size);
//}
//
///*static*/ void lfs_superblock_tole32(struct lfs_disk_superblock *d) {
//    d->root[0]     = lfs_tole32(d->root[0]);
//    d->root[1]     = lfs_tole32(d->root[1]);
//    d->block_size  = lfs_tole32(d->block_size);
//    d->block_count = lfs_tole32(d->block_count);
//    d->version     = lfs_tole32(d->version);
//    d->inline_size = lfs_tole32(d->inline_size);
//    d->attrs_size  = lfs_tole32(d->attrs_size);
//    d->name_size   = lfs_tole32(d->name_size);
//}

/// Other struct functions ///
//static inline lfs_size_t lfs_entry_elen(const lfs_mattr_t *attr) {
//    return (lfs_size_t)(attr->d.elen) |
//        ((lfs_size_t)(attr->d.alen & 0xc0) << 2);
//}
//
//static inline lfs_size_t lfs_entry_alen(const lfs_mattr_t *attr) {
//    return attr->d.alen & 0x3f;
//}
//
//static inline lfs_size_t lfs_entry_nlen(const lfs_mattr_t *attr) {
//    return attr->d.nlen;
//}
//
//static inline lfs_size_t lfs_entry_size(const lfs_mattr_t *attr) {
//    return 4 + lfs_entry_elen(attr) +
//            lfs_entry_alen(attr) +
//            lfs_entry_nlen(attr);
//}


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

/// Entry tag operations ///
static inline lfs_tag_t lfs_mktag(
        uint16_t type, uint16_t id, lfs_size_t size) {
    return (type << 22) | (id << 12) | size;
}

static inline bool lfs_tag_isvalid(lfs_tag_t tag) {
    return !(tag & 0x80000000);
}

static inline bool lfs_tag_isuser(lfs_tag_t tag) {
    return (tag & 0x40000000);
}

static inline uint16_t lfs_tag_type(lfs_tag_t tag) {
    return (tag & 0x7fc00000) >> 22;
}

static inline uint16_t lfs_tag_subtype(lfs_tag_t tag) {
    return (tag & 0x7c000000) >> 22;
}

static inline uint16_t lfs_tag_id(lfs_tag_t tag) {
    return (tag & 0x003ff000) >> 12;
}

static inline lfs_size_t lfs_tag_size(lfs_tag_t tag) {
    return tag & 0x00000fff;
}

// operations on globals
static lfs_globals_t lfs_globals_xor(
        const lfs_globals_t *a, const lfs_globals_t *b) {
    lfs_globals_t res;
    res.move.pair[0] = a->move.pair[0] ^ b->move.pair[0];
    res.move.pair[1] = a->move.pair[1] ^ b->move.pair[1];
    res.move.id = a->move.id ^ b->move.id;
    return res;
}

static bool lfs_globals_iszero(const lfs_globals_t *a) {
    return (a->move.pair[0] == 0 && a->move.pair[1] == 0 && a->move.id == 0);
}


// commit logic
struct lfs_commit {
    lfs_block_t block;
    lfs_off_t off;
    lfs_off_t begin;
    lfs_off_t end;

    lfs_tag_t ptag;
    uint32_t crc;

    struct {
        uint16_t begin;
        uint16_t end;
    } filter;
};

// TODO predelcare
static int lfs_commit_move(lfs_t *lfs, struct lfs_commit *commit,
        uint16_t fromid, uint16_t toid,
        lfs_mdir_t *dir, lfs_mattrlist_t *list);

static int lfs_commit_commit(lfs_t *lfs,
        struct lfs_commit *commit, lfs_mattr_t attr) {
    // filter out ids
    if (lfs_tag_id(attr.tag) < 0x3ff && (
            lfs_tag_id(attr.tag) < commit->filter.begin ||
            lfs_tag_id(attr.tag) >= commit->filter.end)) {
        return 0;
    }

    // special cases
    if (lfs_tag_type(attr.tag) == LFS_FROM_DIR) {
        return lfs_commit_move(lfs, commit,
                lfs_tag_size(attr.tag), lfs_tag_id(attr.tag),
                attr.u.dir, NULL); 
    }

    uint16_t id = lfs_tag_id(attr.tag) - commit->filter.begin;
    attr.tag = lfs_mktag(0, id, 0) | (attr.tag & 0xffc00fff);

    // check if we fit
    lfs_size_t size = lfs_tag_size(attr.tag);
    if (commit->off + sizeof(lfs_tag_t)+size > commit->end) {
        return LFS_ERR_NOSPC;
    }

    // write out tag
    // TODO rm me
    //printf("tag w %#010x (%x:%x %03x %03x %03x)\n", attr.tag, commit->block, commit->off+sizeof(lfs_tag_t), lfs_tag_type(attr.tag), lfs_tag_id(attr.tag), lfs_tag_size(attr.tag));
    lfs_tag_t tag = lfs_tole32((attr.tag & 0x7fffffff) ^ commit->ptag);
    lfs_crc(&commit->crc, &tag, sizeof(tag));
    int err = lfs_bd_prog(lfs, commit->block, commit->off, &tag, sizeof(tag));
    if (err) {
        return err;
    }
    commit->off += sizeof(tag);

    if (!(attr.tag & 0x80000000)) {
        // from memory
        lfs_crc(&commit->crc, attr.u.buffer, size);
        err = lfs_bd_prog(lfs, commit->block, commit->off,
                attr.u.buffer, size);
        if (err) {
            return err;
        }
    } else {
        // from disk
        for (lfs_off_t i = 0; i < size; i++) {
            uint8_t dat;
            int err = lfs_bd_read(lfs,
                    attr.u.d.block, attr.u.d.off+i, &dat, 1);
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
    commit->ptag = attr.tag & 0x7fffffff; // TODO do this once

    return 0;
}

static int lfs_commit_crc(lfs_t *lfs, struct lfs_commit *commit) {
    // align to program units
    lfs_off_t noff = lfs_alignup(
            commit->off + 2*sizeof(uint32_t), lfs->cfg->prog_size);

    // read erased state from next program unit
    lfs_tag_t tag;
    int err = lfs_bd_read(lfs, commit->block, noff, &tag, sizeof(tag));
    if (err) {
        return err;
    }

    // build crc tag
    tag = (0x80000000 & ~lfs_fromle32(tag)) |
            lfs_mktag(LFS_TYPE_CRC, 0x3ff,
                noff - (commit->off+sizeof(uint32_t)));

    // write out crc
    //printf("tag w %#010x (%x:%x %03x %03x %03x)\n", tag, commit->block, commit->off+sizeof(tag), lfs_tag_type(tag), lfs_tag_id(tag), lfs_tag_size(tag));
    uint32_t footer[2];
    footer[0] = lfs_tole32(tag ^ commit->ptag);
    lfs_crc(&commit->crc, &footer[0], sizeof(footer[0]));
    footer[1] = lfs_tole32(commit->crc);
    err = lfs_bd_prog(lfs, commit->block, commit->off,
            footer, sizeof(footer));
    if (err) {
        return err;
    }
    commit->off += sizeof(tag)+lfs_tag_size(tag);
    commit->ptag = tag;

    // flush buffers
    err = lfs_bd_sync(lfs);
    if (err) {
        return err;
    }

    // successful commit, check checksum to make sure
    uint32_t crc = 0xffffffff;
    err = lfs_bd_crc(lfs, commit->block, commit->begin,
            commit->off-lfs_tag_size(tag) - commit->begin, &crc);
    if (err) {
        return err;
    }

    if (crc != commit->crc) {
        return LFS_ERR_CORRUPT;
    }

    return 0;
}

static int lfs_commit_list(lfs_t *lfs, struct lfs_commit *commit,
        lfs_mattrlist_t *list) {
    for (; list; list = list->next) {
        int err = lfs_commit_commit(lfs, commit, list->e);
        if (err) {
            return err;
        }
    }

    return 0;
}


// committer for moves
// TODO rename?
struct lfs_commit_move {
    lfs_mdir_t *dir; // TODO need dir?
    struct {
        uint16_t from;
        uint16_t to;
    } id;

    struct lfs_commit *commit;
};


// TODO redeclare
static int lfs_dir_traverse(lfs_t *lfs, lfs_mdir_t *dir,
        int (*cb)(lfs_t *lfs, void *data, lfs_mattr_t attr),
        void *data);
static int lfs_dir_get(lfs_t *lfs, lfs_mdir_t *dir,
        uint32_t mask, lfs_mattr_t *attr);

static int lfs_commit_movescan(lfs_t *lfs, void *p, lfs_mattr_t attr) {
    struct lfs_commit_move *move = p;

    if (lfs_tag_type(attr.tag) == LFS_TYPE_DELETE &&
            lfs_tag_id(attr.tag) <= move->id.from) {
        // something was deleted, we need to move around it
        move->id.from += 1;
        return 0;
    }

    if (lfs_tag_id(attr.tag) != move->id.from) {
        // ignore non-matching ids
        return 0;
    }

    // check if type has already been committed
    int err = lfs_dir_get(lfs,
            &(lfs_mdir_t){
                .pair[0]=move->commit->block,
                .off=move->commit->off,
                .etag=move->commit->ptag,
                .stop_at_commit=true},
            lfs_tag_isuser(attr.tag) ? 0x7ffff000 : 0x7c3ff000,
            &(lfs_mattr_t){
                lfs_mktag(lfs_tag_type(attr.tag),
                    move->id.to - move->commit->filter.begin, 0)}); // TODO can all these filter adjustments be consolidated?
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    if (err != LFS_ERR_NOENT) {
        // already committed
        return 0;
    }

    // update id and commit, as we are currently unique
    attr.tag = lfs_mktag(0, move->id.to, 0) | (attr.tag & 0xffc00fff);
    return lfs_commit_commit(lfs, move->commit, attr);
}

static int lfs_commit_move(lfs_t *lfs, struct lfs_commit *commit,
        uint16_t fromid, uint16_t toid,
        lfs_mdir_t *dir, lfs_mattrlist_t *list) {
    struct lfs_commit_move move = {
        .id.from = fromid,
        .id.to = toid,
        .commit = commit,
    };

    for (; list; list = list->next) {
        int err = lfs_commit_movescan(lfs, &move, list->e);
        if (err) {
            return err;
        }
    }

    int err = lfs_dir_traverse(lfs, dir, lfs_commit_movescan, &move);
    if (err) {
        return err;
    }

    return 0;
}

static int lfs_commit_globals(lfs_t *lfs, struct lfs_commit *commit,
        const lfs_globals_t *source, const lfs_globals_t *diff) {
    if (lfs_globals_iszero(diff)) {
        return 0;
    }

    // TODO check performance/complexity of different strategies here
    lfs_globals_t res = lfs_globals_xor(source, diff);
    int err = lfs_commit_commit(lfs, commit, (lfs_mattr_t){
            lfs_mktag(LFS_TYPE_GLOBALS, 0x3ff, sizeof(res)),
            .u.buffer=&res});
    if (err) {
        return err;
    }

    return 0;
}

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
    dir->globals = (lfs_globals_t){0};

    // don't write out yet, let caller take care of that
    return 0;
}

static int lfs_dir_fetchwith(lfs_t *lfs,
        lfs_mdir_t *dir, const lfs_block_t pair[2],
        int (*cb)(lfs_t *lfs, void *data, lfs_mattr_t attr), void *data) {
    dir->pair[0] = pair[0];
    dir->pair[1] = pair[1];
    dir->stop_at_commit = false;

    // find the block with the most recent revision
    uint32_t rev[2];
    for (int i = 0; i < 2; i++) {
        int err = lfs_bd_read(lfs, dir->pair[i], 0, &rev[i], sizeof(rev[i]));
        rev[i] = lfs_fromle32(rev[i]);
        if (err) {
            return err;
        }
    }

    if (lfs_scmp(rev[1], rev[0]) > 0) {
        lfs_pairswap(dir->pair);
        lfs_pairswap(rev);
    }

    // load blocks and check crc
    for (int i = 0; i < 2; i++) {
        lfs_off_t off = sizeof(dir->rev);
        lfs_tag_t ptag = 0;
        uint32_t crc = 0xffffffff;
        dir->tail[0] = 0xffffffff;
        dir->tail[1] = 0xffffffff;
        dir->count = 0;
        dir->split = false;
        dir->globals = (lfs_globals_t){0};

        dir->rev = lfs_tole32(rev[0]);
        lfs_crc(&crc, &dir->rev, sizeof(dir->rev));
        dir->rev = lfs_fromle32(dir->rev);

        lfs_mdir_t temp = *dir;

        while (true) {
            // extract next tag
            lfs_tag_t tag;
            int err = lfs_bd_read(lfs, temp.pair[0], off, &tag, sizeof(tag));
            if (err) {
                return err;
            }

            lfs_crc(&crc, &tag, sizeof(tag));
            tag = lfs_fromle32(tag) ^ ptag;

            // next commit not yet programmed
            if (lfs_tag_type(ptag) == LFS_TYPE_CRC && !lfs_tag_isvalid(tag)) {
                // synthetic move
                if (lfs_paircmp(dir->pair, lfs->globals.move.pair) == 0
                        && cb) {
                    int err = cb(lfs, data, (lfs_mattr_t){
                            lfs_mktag(LFS_TYPE_DELETE,
                                lfs->globals.move.id, 0)});
                    if (err) {
                        return err;
                    }
                }

                dir->erased = true;
                return 0;
            }

            // check we're in valid range
            if (off + sizeof(tag)+lfs_tag_size(tag) > lfs->cfg->block_size) {
                break;
            }

            //printf("tag r %#010x (%x:%x %03x %03x %03x)\n", tag, temp.pair[0], off+sizeof(tag), lfs_tag_type(tag), lfs_tag_id(tag), lfs_tag_size(tag));
            if (lfs_tag_type(tag) == LFS_TYPE_CRC) {
                // check the crc attr
                uint32_t dcrc;
                int err = lfs_bd_read(lfs, temp.pair[0],
                        off+sizeof(tag), &dcrc, sizeof(dcrc));
                if (err) {
                    return err;
                }

                if (crc != lfs_fromle32(dcrc)) {
                    if (off == sizeof(temp.rev)) {
                        // try other block
                        break;
                    } else {
                        // snythetic move
                        // TODO combine with above?
                        if (lfs_paircmp(dir->pair, lfs->globals.move.pair) == 0
                                && cb) {
                            int err = cb(lfs, data, (lfs_mattr_t){
                                    lfs_mktag(LFS_TYPE_DELETE,
                                        lfs->globals.move.id, 0)});
                            if (err) {
                                return err;
                            }
                        }

                        // consider what we have good enough
                        dir->erased = false;
                        return 0;
                    }
                }

                temp.off = off + sizeof(tag)+lfs_tag_size(tag);
                temp.etag = tag;
                crc = 0xffffffff;
                *dir = temp;

                // TODO simplify this?
                if (cb) {
                    err = cb(lfs, data, (lfs_mattr_t){
                            (tag | 0x80000000),
                            .u.d.block=temp.pair[0],
                            .u.d.off=off+sizeof(tag)});
                    if (err) {
                        return err;
                    }
                }
            } else {
                // TODO crc before callback???
                err = lfs_bd_crc(lfs, temp.pair[0],
                        off+sizeof(tag), lfs_tag_size(tag), &crc);
                if (err) {
                    return err;
                }

                if (lfs_tag_subtype(tag) == LFS_TYPE_TAIL) {
                    temp.split = (lfs_tag_type(tag) & 1);
                    err = lfs_bd_read(lfs, temp.pair[0], off+sizeof(tag),
                            temp.tail, sizeof(temp.tail));
                    if (err) {
                        return err;
                    }
                } else if (lfs_tag_type(tag) == LFS_TYPE_GLOBALS) {
                    err = lfs_bd_read(lfs, temp.pair[0], off+sizeof(tag),
                            &temp.globals, sizeof(temp.globals));
                    if (err) {
                        return err;
                    }
                } else {
                    if (lfs_tag_id(tag) < 0x3ff &&
                            lfs_tag_id(tag) >= temp.count) {
                        temp.count = lfs_tag_id(tag)+1;
                    }

                    if (lfs_tag_type(tag) == LFS_TYPE_DELETE) {
                        temp.count -= 1;
                    }

                    if (cb) {
                        err = cb(lfs, data, (lfs_mattr_t){
                                (tag | 0x80000000),
                                .u.d.block=temp.pair[0],
                                .u.d.off=off+sizeof(tag)});
                        if (err) {
                            return err;
                        }
                    }
                }
            }

            ptag = tag;
            off += sizeof(tag)+lfs_tag_size(tag);
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
    return lfs_dir_fetchwith(lfs, dir, pair, NULL, NULL);
}

static int lfs_dir_traverse(lfs_t *lfs, lfs_mdir_t *dir,
        int (*cb)(lfs_t *lfs, void *data, lfs_mattr_t attr), void *data) {
    // iterate over dir block backwards (for faster lookups)
    lfs_block_t block = dir->pair[0];
    lfs_off_t off = dir->off;
    lfs_tag_t tag = dir->etag;

    // synthetic move
    if (lfs_paircmp(dir->pair, lfs->globals.move.pair) == 0) {
        int err = cb(lfs, data, (lfs_mattr_t){
                lfs_mktag(LFS_TYPE_DELETE, lfs->globals.move.id, 0)});
        if (err) {
            return err;
        }
    }

    while (off != sizeof(uint32_t)) {
        // TODO rm me
        //printf("tag r %#010x (%x:%x %03x %03x %03x)\n", tag, block, off-lfs_tag_size(tag), lfs_tag_type(tag), lfs_tag_id(tag), lfs_tag_size(tag));

        // TODO hmm
        if (lfs_tag_type(tag) == LFS_TYPE_CRC) {
            if (dir->stop_at_commit) {
                break;
            }
        } else {
            int err = cb(lfs, data, (lfs_mattr_t){
                    (0x80000000 | tag),
                    .u.d.block=block,
                    .u.d.off=off-lfs_tag_size(tag)});
            if (err) {
                return err;
            }
        }

        LFS_ASSERT(off > sizeof(tag)+lfs_tag_size(tag));
        off -= sizeof(tag)+lfs_tag_size(tag);

        lfs_tag_t ntag;
        int err = lfs_bd_read(lfs, block, off, &ntag, sizeof(ntag));
        if (err) {
            return err;
        }

        tag ^= lfs_fromle32(ntag);
    }

    return 0;
}

static int lfs_dir_compact(lfs_t *lfs, lfs_mdir_t *dir, lfs_mattrlist_t *list,
        lfs_mdir_t *source, uint16_t begin, uint16_t end) {
    // save some state in case block is bad
    const lfs_block_t oldpair[2] = {dir->pair[1], dir->pair[0]};
    bool relocated = false;

    // There's nothing special about our global delta, so feed it back
    // into the global global delta
    // TODO IMMENSE HMM globals get bleed into from above, need to be fixed after commits due to potential moves
    lfs_globals_t gtemp = dir->globals; // TODO hmm, why did we have different variables then?

    lfs->diff = lfs_globals_xor(&lfs->diff, &dir->globals);
    dir->globals = (lfs_globals_t){0};

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

                // space is complicated, we need room for tail, crc, idelete,
                // and we keep cap at around half a block
                .begin = 0,
                .end = lfs_min(
                        lfs_alignup(lfs->cfg->block_size / 2,
                            lfs->cfg->prog_size),
                        lfs->cfg->block_size - 5*sizeof(uint32_t)),
                .crc = crc,
                .ptag = 0,

                // filter out ids
                .filter.begin = begin,
                .filter.end = end,
            };

            if (!relocated) {
                err = lfs_commit_globals(lfs, &commit,
                        &dir->globals, &lfs->diff);
                if (err) {
                    if (err == LFS_ERR_NOSPC) {
                        goto split;
                    } else if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            // commit with a move
            for (uint16_t id = begin; id < end; id++) {
                err = lfs_commit_move(lfs, &commit, id, id, source, list);
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
            commit.end = lfs->cfg->block_size - 2*sizeof(uint32_t);

            if (!lfs_pairisnull(dir->tail)) {
                // commit tail, which may be new after last size check
                // TODO le32
                err = lfs_commit_commit(lfs, &commit, (lfs_mattr_t){
                        lfs_mktag(LFS_TYPE_TAIL + dir->split,
                            0x3ff, sizeof(dir->tail)),
                        .u.buffer=dir->tail});
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }
            }

            err = lfs_commit_crc(lfs, &commit);
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

        lfs_mdir_t tail;
        int err = lfs_dir_alloc(lfs, &tail, dir->split, dir->tail);
        if (err) {
            return err;
        }

        err = lfs_dir_compact(lfs, &tail, list, dir, ack+1, end);
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

    if (relocated) {
        // update references if we relocated
        LFS_DEBUG("Relocating %d %d to %d %d",
                oldpair[0], oldpair[1], dir->pair[0], dir->pair[1]);
        int err = lfs_relocate(lfs, oldpair, dir->pair);
        if (err) {
            return err;
        }
    } else {
        lfs->globals = lfs_globals_xor(&lfs->globals, &lfs->diff);
        lfs->diff = (lfs_globals_t){0};
    }

    lfs->globals = lfs_globals_xor(&lfs->globals, &gtemp); // TODO hmm, why did we have different variables then?

    return 0;
}

static int lfs_dir_commit(lfs_t *lfs, lfs_mdir_t *dir, lfs_mattrlist_t *list) {
    while (true) {
        if (!dir->erased) {
            // not erased, must compact
            goto compact;
        }

        struct lfs_commit commit = {
            .block = dir->pair[0],
            .begin = dir->off,
            .off = dir->off,
            .end = lfs->cfg->block_size - 2*sizeof(uint32_t),
            .crc = 0xffffffff,
            .ptag = dir->etag,
            .filter.begin = 0,
            .filter.end = 0x3ff,
        };

        int err = lfs_commit_list(lfs, &commit, list);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        err = lfs_commit_globals(lfs, &commit, &dir->globals, &lfs->diff);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        err = lfs_commit_crc(lfs, &commit);
        if (err) {
            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        // successful commit, lets update dir
        dir->off = commit.off;
        dir->etag = commit.ptag;
//        // TODO hm
//        dir->globals = lfs_globals_xor(&dir->globals, &lfs->diff);
        lfs->globals = lfs_globals_xor(&lfs->globals, &lfs->diff);
        lfs->diff = (lfs_globals_t){0};
        break;

compact:
        lfs->pcache.block = 0xffffffff;
        err = lfs_dir_compact(lfs, dir, list, dir, 0, dir->count);
        if (err) {
            return err;
        }
        break;
    }

    // update any directories that are affected
    // TODO what about pairs? what if we're splitting??
    for (lfs_dir_t *d = lfs->dirs; d; d = d->next) {
        if (lfs_paircmp(d->m.pair, dir->pair) == 0) {
            d->m = *dir;
        }
    }

    // TODO what if we relocated the block containing the move?
    return 0;
}

static int lfs_dir_append(lfs_t *lfs, lfs_mdir_t *dir, uint16_t *id) {
    *id = dir->count;
    dir->count += 1;
    return 0;
}

static int lfs_dir_delete(lfs_t *lfs, lfs_mdir_t *dir, uint16_t id) {
    dir->count -= 1;

    // check if we should drop the directory block
    if (dir->count == 0) {
        lfs_mdir_t pdir;
        int res = lfs_pred(lfs, dir->pair, &pdir);
        if (res < 0) {
            return res;
        }

        if (res && pdir.split) {
            // steal tail, and global state
            pdir.split = dir->split;
            pdir.tail[0] = dir->tail[0];
            pdir.tail[1] = dir->tail[1];
            lfs->diff = dir->globals;
            lfs->globals = lfs_globals_xor(&lfs->globals, &dir->globals);

            int err = lfs_dir_commit(lfs, &pdir, &(lfs_mattrlist_t){
                    {lfs_mktag(LFS_TYPE_TAIL + pdir.split,
                        0x3ff, sizeof(pdir.tail)),
                     .u.buffer=pdir.tail}});
            return err;
        }
    }

    int err = lfs_dir_commit(lfs, dir, &(lfs_mattrlist_t){
            {lfs_mktag(LFS_TYPE_DELETE, id, 0)}});
    if (err) {
        return err;
    }

    // shift over any dirs/files that are affected
    for (lfs_dir_t *d = lfs->dirs; d; d = d->next) {
        if (lfs_paircmp(d->m.pair, dir->pair) == 0) {
            if (d->id > id) {
                d->id -= 1;
                d->pos -= 1;
            }
        }
    }

    for (lfs_file_t *f = lfs->files; f; f = f->next) {
        if (lfs_paircmp(f->pair, dir->pair) == 0) {
            if (f->id == id) {
                f->pair[0] = 0xffffffff;
                f->pair[1] = 0xffffffff;
            } else if (f->id > id) {
                f->id -= 1;
            }
        }
    }

    return 0;
}

struct lfs_dir_get {
    uint32_t mask;
    lfs_tag_t tag;
    lfs_mattr_t *attr;
};

static int lfs_dir_getscan(lfs_t *lfs, void *p, lfs_mattr_t attr) {
    struct lfs_dir_get *get = p;

    if ((attr.tag & get->mask) == (get->tag & get->mask)) {
        *get->attr = attr;
        return true;
    } else if (lfs_tag_type(attr.tag) == LFS_TYPE_DELETE) {
        if (lfs_tag_id(attr.tag) <= lfs_tag_id(get->tag)) {
            get->tag += lfs_mktag(0, 1, 0);
        }
    }

    return false;
}

static int lfs_dir_get(lfs_t *lfs, lfs_mdir_t *dir,
        uint32_t mask, lfs_mattr_t *attr) {
    uint16_t id = lfs_tag_id(attr->tag);
    int res = lfs_dir_traverse(lfs, dir, lfs_dir_getscan,
            &(struct lfs_dir_get){mask, attr->tag, attr});
    if (res < 0) {
        return res;
    }

    if (!res) {
        return LFS_ERR_NOENT;
    }

    attr->tag = lfs_mktag(0, id, 0) | (attr->tag & 0xffc00fff);
    return 0;
}

static int lfs_dir_getbuffer(lfs_t *lfs, lfs_mdir_t *dir,
        uint32_t mask, lfs_mattr_t *attr) {
    void *buffer = attr->u.buffer;
    lfs_size_t size = lfs_tag_size(attr->tag);
    int err = lfs_dir_get(lfs, dir, mask, attr);
    if (err) {
        return err;
    }

    lfs_size_t diff = lfs_min(size, lfs_tag_size(attr->tag));
    memset((uint8_t*)buffer + diff, 0, size - diff);
    err = lfs_bd_read(lfs, attr->u.d.block, attr->u.d.off, buffer, diff);
    if (err) {
        return err;
    }

    if (lfs_tag_size(attr->tag) > size) {
        return LFS_ERR_RANGE;
    }

    return 0;
}

static int lfs_dir_getentry(lfs_t *lfs, lfs_mdir_t *dir,
        uint32_t mask, lfs_tag_t tag, lfs_mattr_t *attr) {
    attr->tag = tag | sizeof(attr->u);
    attr->u.buffer = &attr->u;
    int err = lfs_dir_getbuffer(lfs, dir, mask, attr);
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }

    return 0;
}

static int lfs_dir_getinfo(lfs_t *lfs, lfs_mdir_t *dir,
        int16_t id, struct lfs_info *info) {
    lfs_mattr_t attr = {
        lfs_mktag(LFS_TYPE_NAME, id, lfs->name_size+1),
        .u.buffer=info->name,
    };

    int err = lfs_dir_getbuffer(lfs, dir, 0x7c3ff000, &attr);
    if (err) {
        return err;
    }

    info->type = lfs_tag_type(attr.tag);

    err = lfs_dir_getentry(lfs, dir, 0x7c3ff000,
            lfs_mktag(LFS_TYPE_STRUCT, id, 0), &attr);
    if (err) {
        return err;
    }

    if (lfs_tag_type(attr.tag) == LFS_STRUCT_CTZ) {
        info->size = attr.u.ctz.size;
    } else if (lfs_tag_type(attr.tag) == LFS_STRUCT_INLINE) {
        info->size = lfs_tag_size(attr.tag);
    }

    return 0;
}

struct lfs_dir_find {
    uint32_t mask;
    lfs_tag_t tag;
    const void *buffer;
    lfs_tag_t foundtag;
    lfs_tag_t temptag;
};

static int lfs_dir_findscan(lfs_t *lfs, void *p, lfs_mattr_t attr) {
    struct lfs_dir_find *find = p;

    if ((attr.tag & find->mask) == (find->tag & find->mask)) {
        int res = lfs_bd_cmp(lfs, attr.u.d.block, attr.u.d.off,
                find->buffer, lfs_tag_size(attr.tag));
        if (res < 0) {
            return res;
        }

        if (res) {
            // found a match
            find->temptag = attr.tag;
        }
    } else if (lfs_tag_type(attr.tag) == LFS_TYPE_DELETE) {
        if (lfs_tag_id(attr.tag) == lfs_tag_id(find->temptag)) {
            find->temptag = 0xffffffff;
        } else if (lfs_tag_id(find->temptag) < 0x3ff &&
                lfs_tag_id(attr.tag) < lfs_tag_id(find->temptag)) {
            find->temptag -= lfs_mktag(0, 1, 0);
        }
    } else if (lfs_tag_type(attr.tag) == LFS_TYPE_CRC) {
        find->foundtag = find->temptag;
    }

    return 0;
}

static int lfs_dir_find(lfs_t *lfs, lfs_mdir_t *dir, const lfs_block_t *pair,
        uint32_t mask, lfs_tag_t tag,
        const void *buffer, lfs_tag_t *foundtag) {
    struct lfs_dir_find find = {
        .mask = mask,
        .tag = tag,
        .buffer = buffer,
        .foundtag = 0xffffffff,
        .temptag = 0xffffffff,
    };

    int err = lfs_dir_fetchwith(lfs, dir, pair, lfs_dir_findscan, &find);
    if (err) {
        return err;
    }

    if (find.foundtag == 0xffffffff) {
        return LFS_ERR_NOENT;
    }

    *foundtag = find.foundtag;
    return 0;
}
        

// TODO drop others, make this only return id, also make get take in only entry to populate (with embedded tag)
static int lfs_dir_lookup(lfs_t *lfs, lfs_mdir_t *dir,
        const char **path, uint16_t *id, uint8_t *type) {
    lfs_mattr_t attr = {
        .u.pair[0] = lfs->root[0],
        .u.pair[1] = lfs->root[1],
    };

    const char *name = *path;
    lfs_size_t namelen;

    while (true) {
    nextname:
        // skip slashes
        name += strspn(name, "/");
        namelen = strcspn(name, "/");

        // special case for root dir
        if (name[0] == '\0') {
            // Return ISDIR when we hit root
            // TODO change this to -1 or 0x3ff?
            *type = LFS_TYPE_DIR;
            return LFS_ERR_ISDIR;
        }

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

        // update what we've found
        *path = name;

        // find path
        while (true) {
            lfs_tag_t foundtag = -1;
            int err = lfs_dir_find(lfs, dir, attr.u.pair,
                    0x7c000fff, lfs_mktag(LFS_TYPE_NAME, 0, namelen),
                    name, &foundtag);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT) {
                // found it
                *id = lfs_tag_id(foundtag);
                *type = lfs_tag_type(foundtag);
                break;
            }

            if (!dir->split) {
                return LFS_ERR_NOENT;
            }

            attr.u.pair[0] = dir->tail[0];
            attr.u.pair[1] = dir->tail[1];
        }

        name += namelen;
        name += strspn(name, "/");
        if (name[0] == '\0') {
            return 0;
        }

        // don't continue on if we didn't hit a directory
        // TODO update with what's on master?
        if (*type != LFS_TYPE_DIR) {
            return LFS_ERR_NOTDIR;
        }

        // TODO optimize grab for inline files and like?
        // TODO would this mean more code?
        // grab the entry data
        int err = lfs_dir_getentry(lfs, dir, 0x7c3ff000,
                lfs_mktag(LFS_TYPE_STRUCT, *id, 0), &attr);
        if (err) {
            return err;
        }
    }
}

/// Top level directory operations ///
int lfs_mkdir(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    if (!lfs->deorphaned) {
        int err = lfs_deorphan(lfs);
        if (err) {
            return err;
        }
    }

    lfs_mdir_t cwd;
    int err = lfs_dir_lookup(lfs, &cwd, &path, &(uint16_t){0}, &(uint8_t){0});
    if (err != LFS_ERR_NOENT || strchr(path, '/') != NULL) {
        if (!err || err == LFS_ERR_ISDIR) {
            return LFS_ERR_EXIST;
        }
        return err;
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
    uint16_t id;
    err = lfs_dir_append(lfs, &cwd, &id);
    if (err) {
        return err;
    }

    cwd.tail[0] = dir.pair[0];
    cwd.tail[1] = dir.pair[1];
    err = lfs_dir_commit(lfs, &cwd, &(lfs_mattrlist_t){
            {lfs_mktag(LFS_TYPE_DIR, id, nlen),
                .u.buffer=(void*)path}, &(lfs_mattrlist_t){
            {lfs_mktag(LFS_STRUCT_DIR, id, sizeof(dir.pair)),
                .u.buffer=dir.pair}, &(lfs_mattrlist_t){
            {lfs_mktag(LFS_TYPE_SOFTTAIL, 0x3ff, sizeof(cwd.tail)),
                .u.buffer=cwd.tail}}}});

    // TODO need ack here?
    lfs_alloc_ack(lfs);
    return 0;
}

int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
    uint16_t id;
    uint8_t type;
    int err = lfs_dir_lookup(lfs, &dir->m, &path, &id, &type);
    if (err && err != LFS_ERR_ISDIR) {
        return err;
    }

    if (type != LFS_TYPE_DIR) {
        return LFS_ERR_NOTDIR;
    }

    lfs_mattr_t attr;
    if (err == LFS_ERR_ISDIR) {
        // handle root dir separately
        attr.u.pair[0] = lfs->root[0];
        attr.u.pair[1] = lfs->root[1];
    } else {
        // get dir pair from parent
        err = lfs_dir_getentry(lfs, &dir->m, 0x7c3ff000,
                lfs_mktag(LFS_TYPE_STRUCT, id, 0), &attr);
        if (err) {
            return err;
        }
    }

    // fetch first pair
    err = lfs_dir_fetch(lfs, &dir->m, attr.u.pair);
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
        lfs_cache_t *rcache, const lfs_cache_t *pcache,
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

static int lfs_ctz_extend(lfs_t *lfs,
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
            lfs_off_t index = lfs_ctz_index(lfs, &size);
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

static int lfs_ctz_traverse(lfs_t *lfs,
        lfs_cache_t *rcache, const lfs_cache_t *pcache,
        lfs_block_t head, lfs_size_t size,
        int (*cb)(lfs_t*, void*, lfs_block_t), void *data) {
    if (size == 0) {
        return 0;
    }

    lfs_off_t index = lfs_ctz_index(lfs, &(lfs_off_t){size-1});

    while (true) {
        int err = cb(lfs, data, head);
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
            err = cb(lfs, data, heads[i]);
            if (err) {
                return err;
            }
        }

        head = heads[count-1];
        index -= count;
    }
}


/// Top level file operations ///
int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    // deorphan if we haven't yet, needed at most once after poweron
    if ((flags & 3) != LFS_O_RDONLY && !lfs->deorphaned) {
        int err = lfs_deorphan(lfs);
        if (err) {
            return err;
        }
    }

    // allocate entry for file if it doesn't exist
    lfs_mdir_t cwd;
    uint16_t id;
    uint8_t type;
    int err = lfs_dir_lookup(lfs, &cwd, &path, &id, &type);
    if (err && (err != LFS_ERR_NOENT || strchr(path, '/') != NULL) &&
            err != LFS_ERR_ISDIR) {
        return err;
    }

    lfs_mattr_t attr;
    if (err == LFS_ERR_NOENT) {
        if (!(flags & LFS_O_CREAT)) {
            return LFS_ERR_NOENT;
        }

        // check that name fits
        lfs_size_t nlen = strlen(path);
        if (nlen > lfs->name_size) {
            return LFS_ERR_NAMETOOLONG;
        }

        // get next slot and create entry to remember name
        err = lfs_dir_append(lfs, &cwd, &id);
        if (err) {
            return err;
        }

        // TODO do we need to make file registered to list to catch updates from this commit? ie if id/cwd change
        err = lfs_dir_commit(lfs, &cwd, &(lfs_mattrlist_t){
                {lfs_mktag(LFS_TYPE_REG, id, nlen),
                    .u.buffer=(void*)path}, &(lfs_mattrlist_t){
                {lfs_mktag(LFS_STRUCT_INLINE, id, 0)}}});
        if (err) {
            return err;
        }

        // TODO eh
        if (id >= cwd.count) {
            // catch updates from a compact in the above commit
            id -= cwd.count;
            cwd.pair[0] = cwd.tail[0];
            cwd.pair[1] = cwd.tail[1];
        }

        attr.tag = lfs_mktag(LFS_STRUCT_INLINE, id, 0);
    } else {
        if (type != LFS_TYPE_REG) {
            return LFS_ERR_ISDIR;
        } else if (flags & LFS_O_EXCL) {
            return LFS_ERR_EXIST;
        }

        attr.tag = lfs_mktag(LFS_TYPE_STRUCT, id, 0);
        err = lfs_dir_get(lfs, &cwd, 0x7c3ff000, &attr);
        if (err) {
            return err;
        }
    }

    // setup file struct
    file->pair[0] = cwd.pair[0];
    file->pair[1] = cwd.pair[1];
    file->id = id;
    file->flags = flags;
    file->pos = 0;
    file->attrs = NULL;

    // allocate buffer if needed
    file->cache.block = 0xffffffff;
    if (lfs->cfg->file_buffer) {
        file->cache.buffer = lfs->cfg->file_buffer;
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

    if (lfs_tag_type(attr.tag) == LFS_STRUCT_INLINE) {
        // load inline files
        file->head = 0xfffffffe;
        file->size = lfs_tag_size(attr.tag);
        file->flags |= LFS_F_INLINE;
        file->cache.block = file->head;
        file->cache.off = 0;
        // don't always read (may be new file)
        if (file->size > 0) {
            err = lfs_bd_read(lfs, attr.u.d.block, attr.u.d.off,
                    file->cache.buffer, file->size);
            if (err) {
                lfs_free(file->cache.buffer);
                return err;
            }
        }
    } else {
        // use ctz list from entry
        err = lfs_bd_read(lfs, attr.u.d.block, attr.u.d.off,
                &file->head, 2*sizeof(uint32_t));
    }

    // truncate if requested
    if (flags & LFS_O_TRUNC) {
        if (file->size != 0) {
            file->flags |= LFS_F_DIRTY;
        }

        file->head = 0xfffffffe;
        file->size = 0;
        file->flags |= LFS_F_INLINE;
        file->cache.block = file->head;
        file->cache.off = 0;
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
        lfs_free(file->cache.buffer);
    }

    return err;
}

static int lfs_file_relocate(lfs_t *lfs, lfs_file_t *file) {
relocate:;
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
            file->size = lfs_max(file->pos, file->size);
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

        // either update the references or inline the whole file
        if (!(file->flags & LFS_F_INLINE)) {
            int err = lfs_dir_commit(lfs, &cwd, &(lfs_mattrlist_t){
                    {lfs_mktag(LFS_STRUCT_CTZ,
                        file->id, 2*sizeof(uint32_t)), .u.buffer=&file->head},
                    file->attrs});
            if (err) {
                return err;
            }
        } else {
            int err = lfs_dir_commit(lfs, &cwd, &(lfs_mattrlist_t){
                    {lfs_mktag(LFS_STRUCT_INLINE,
                        file->id, file->size), .u.buffer=file->cache.buffer},
                    file->attrs});
            if (err) {
                return err;
            }
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
        return LFS_ERR_BADF;
    }

    if (file->flags & LFS_F_WRITING) {
        // flush out any writes
        int err = lfs_file_flush(lfs, file);
        if (err) {
            return err;
        }
    }

    if (file->pos >= file->size) {
        // eof if past end
        return 0;
    }

    size = lfs_min(size, file->size - file->pos);
    nsize = size;

    while (nsize > 0) {
        // check if we need a new block
        if (!(file->flags & LFS_F_READING) ||
                file->off == lfs->cfg->block_size) {
            if (!(file->flags & LFS_F_INLINE)) {
                int err = lfs_ctz_find(lfs, &file->cache, NULL,
                        file->head, file->size,
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

    if ((file->flags & LFS_O_APPEND) && file->pos < file->size) {
        file->pos = file->size;
    }

    if (!(file->flags & LFS_F_WRITING) && file->pos > file->size) {
        // fill with zeros
        lfs_off_t pos = file->pos;
        file->pos = file->size;

        while (file->pos < pos) {
            lfs_ssize_t res = lfs_file_write(lfs, file, &(uint8_t){0}, 1);
            if (res < 0) {
                return res;
            }
        }
    }

    if ((file->flags & LFS_F_INLINE) &&
            file->pos + nsize >= lfs->cfg->inline_size) {
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
                    int err = lfs_ctz_find(lfs, &file->cache, NULL,
                            file->head, file->size,
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
                int err = lfs_ctz_extend(lfs, &lfs->rcache, &file->cache,
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
        if (off < 0 && (lfs_off_t)-off > file->size) {
            return LFS_ERR_INVAL;
        }

        file->pos = file->size + off;
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
        err = lfs_ctz_find(lfs, &file->cache, NULL,
                file->head, file->size,
                size, &file->head, &(lfs_off_t){0});
        if (err) {
            return err;
        }

        file->size = size;
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
        return lfs_max(file->pos, file->size);
    } else {
        return file->size;
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
    uint16_t id;
    // TODO pass to getinfo?
    int err = lfs_dir_lookup(lfs, &cwd, &path, &id, &(uint8_t){0});
    if (err && err != LFS_ERR_ISDIR) {
        return err;
    }

    if (err == LFS_ERR_ISDIR) {
        // special case for root
        strcpy(info->name, "/");
        info->type = LFS_TYPE_DIR;
        return 0;
    }

    return lfs_dir_getinfo(lfs, &cwd, id, info);
}

int lfs_remove(lfs_t *lfs, const char *path) {
    // deorphan if we haven't yet, needed at most once after poweron
    if (!lfs->deorphaned) {
        int err = lfs_deorphan(lfs);
        if (err) {
            return err;
        }
    }

    lfs_mdir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
    if (err) {
        return err;
    }

    uint16_t id;
    uint8_t type;
    err = lfs_dir_lookup(lfs, &cwd, &path, &id, &type);
    if (err) {
        return err;
    }

    lfs_mdir_t dir;
    if (type == LFS_TYPE_DIR) {
        // must be empty before removal
        lfs_mattr_t attr;
        err = lfs_dir_getentry(lfs, &cwd, 0x7c3ff000,
                lfs_mktag(LFS_TYPE_STRUCT, id, 0), &attr);
        if (err) {
            return err;
        }

        err = lfs_dir_fetch(lfs, &dir, attr.u.pair);
        if (err) {
            return err;
        }

        // TODO lfs_dir_empty?
        if (dir.count > 0 || dir.split) {
            return LFS_ERR_NOTEMPTY;
        }
    }

    // delete the entry
    err = lfs_dir_delete(lfs, &cwd, id);
    if (err) {
        return err;
    }

    if (type == LFS_TYPE_DIR) {
        int res = lfs_pred(lfs, dir.pair, &cwd);
        if (res < 0) {
            return res;
        }

        LFS_ASSERT(res); // must have pred
        cwd.tail[0] = dir.tail[0];
        cwd.tail[1] = dir.tail[1];
        err = lfs_dir_commit(lfs, &cwd, &(lfs_mattrlist_t){
                {lfs_mktag(LFS_TYPE_SOFTTAIL, 0x3ff, sizeof(cwd.tail)),
                    .u.buffer=cwd.tail}});
    }

    return 0;
}

int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath) {
    // deorphan if we haven't yet, needed at most once after poweron
    if (!lfs->deorphaned) {
        int err = lfs_deorphan(lfs);
        if (err) {
            return err;
        }
    }

    // find old entry
    lfs_mdir_t oldcwd;
    uint16_t oldid;
    uint8_t oldtype;
    int err = lfs_dir_lookup(lfs, &oldcwd, &oldpath, &oldid, &oldtype);
    if (err) {
        return err;
    }

    // find new entry
    lfs_mdir_t newcwd;
    uint16_t newid;
    uint8_t prevtype;
    err = lfs_dir_lookup(lfs, &newcwd, &newpath, &newid, &prevtype);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    bool prevexists = (err != LFS_ERR_NOENT);
    //bool samepair = (lfs_paircmp(oldcwd.pair, newcwd.pair) == 0);

    lfs_mdir_t prevdir;
    if (prevexists) {
        // check that we have same type
        if (prevtype != oldtype) {
            return LFS_ERR_ISDIR;
        }

        if (prevtype == LFS_TYPE_DIR) {
            // must be empty before removal
            lfs_mattr_t prevattr;
            err = lfs_dir_getentry(lfs, &newcwd, 0x7c3ff000,
                    lfs_mktag(LFS_TYPE_STRUCT, newid, 0), &prevattr);
            if (err) {
                return err;
            }

            // must be empty before removal
            err = lfs_dir_fetch(lfs, &prevdir, prevattr.u.pair);
            if (err) {
                return err;
            }

            if (prevdir.count > 0 || prevdir.split) {
                return LFS_ERR_NOTEMPTY;
            }
        }
    } else {
        // check that name fits
        lfs_size_t nlen = strlen(newpath);
        if (nlen > lfs->name_size) {
            return LFS_ERR_NAMETOOLONG;
        }

        // get next id
        err = lfs_dir_append(lfs, &newcwd, &newid);
        if (err) {
            return err;
        }
    }

    // create move to fix later
    lfs->diff.move.pair[0] = oldcwd.pair[0] ^ lfs->globals.move.pair[0];
    lfs->diff.move.pair[1] = oldcwd.pair[1] ^ lfs->globals.move.pair[1];
    lfs->diff.move.id      = oldid          ^ lfs->globals.move.id;

    // move over all attributes
    err = lfs_dir_commit(lfs, &newcwd, &(lfs_mattrlist_t){
            {lfs_mktag(oldtype, newid, strlen(newpath)),
                .u.buffer=(void*)newpath}, &(lfs_mattrlist_t){
            {lfs_mktag(LFS_FROM_DIR, newid, oldid),
                .u.dir=&oldcwd}}});
    if (err) {
        return err;
    }

    // clean up after ourselves
    err = lfs_fixmove(lfs);
    if (err) {
        return err;
    }

    if (prevexists && prevtype == LFS_TYPE_DIR) {
        int res = lfs_pred(lfs, prevdir.pair, &newcwd);
        if (res < 0) {
            return res;
        }

        // TODO test for global state stealing?
        // steal global state
        lfs->globals = lfs_globals_xor(&lfs->globals, &prevdir.globals);

        LFS_ASSERT(res); // must have pred
        newcwd.tail[0] = prevdir.tail[0];
        newcwd.tail[1] = prevdir.tail[1];
        err = lfs_dir_commit(lfs, &newcwd, &(lfs_mattrlist_t){
                {lfs_mktag(LFS_TYPE_SOFTTAIL, 0x3ff, sizeof(newcwd.tail)),
                    .u.buffer=newcwd.tail}});
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
//    if (prevexists && lfs_tag_subtype(prevattr.tag) == LFS_TYPE_DIR) {
//        err = lfs_deorphan(lfs);
//        if (err) {
//            return err;
//        }
//    }
//
    return 0;
}

//int lfs_getattrs(lfs_t *lfs, const char *path,
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

    LFS_ASSERT(lfs->cfg->attrs_size <= LFS_ATTRS_MAX);
    lfs->attrs_size = lfs->cfg->attrs_size;
    if (!lfs->attrs_size) {
        lfs->attrs_size = LFS_ATTRS_MAX;
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
    lfs->deorphaned = false;
    lfs->globals.move.pair[0] = 0xffffffff;
    lfs->globals.move.pair[1] = 0xffffffff;
    lfs->globals.move.id = 0x3ff;
    lfs->diff = (lfs_globals_t){0};

    // scan for any global updates
    // TODO rm me? need to grab any inits
    int err = lfs_scan(lfs);
    if (err) {
        return err;
    }

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
        .attrs_size  = lfs->attrs_size,
        .name_size   = lfs->name_size,
    };

    dir.count += 1;
    err = lfs_dir_commit(lfs, &dir, &(lfs_mattrlist_t){
            {lfs_mktag(LFS_TYPE_SUPERBLOCK, 0, sizeof(superblock)),
                .u.buffer=&superblock}, &(lfs_mattrlist_t){
            {lfs_mktag(LFS_STRUCT_DIR, 0, sizeof(lfs->root)),
                .u.buffer=lfs->root}}});
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
    err = lfs_dir_getbuffer(lfs, &dir, 0x7ffff000, &(lfs_mattr_t){
            lfs_mktag(LFS_TYPE_SUPERBLOCK, 0, sizeof(superblock)),
            .u.buffer=&superblock});
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }

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

    err = lfs_dir_getbuffer(lfs, &dir, 0x7ffff000, &(lfs_mattr_t){
            lfs_mktag(LFS_STRUCT_DIR, 0, sizeof(lfs->root)),
            .u.buffer=lfs->root});
    if (err) {
        return err;
    }

    if (superblock.inline_size) {
        if (superblock.inline_size > lfs->inline_size) {
            LFS_ERROR("Unsupported inline size (%d > %d)",
                    superblock.inline_size, lfs->inline_size);
            return LFS_ERR_INVAL;
        }

        lfs->inline_size = superblock.inline_size;
    }

    if (superblock.attrs_size) {
        if (superblock.attrs_size > lfs->attrs_size) {
            LFS_ERROR("Unsupported attrs size (%d > %d)",
                    superblock.attrs_size, lfs->attrs_size);
            return LFS_ERR_INVAL;
        }

        lfs->attrs_size = superblock.attrs_size;
    }

    if (superblock.name_size) {
        if (superblock.name_size > lfs->name_size) {
            LFS_ERROR("Unsupported name size (%d > %d)",
                    superblock.name_size, lfs->name_size);
            return LFS_ERR_INVAL;
        }

        lfs->name_size = superblock.name_size;
    }

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
        int (*cb)(lfs_t *lfs, void *data, lfs_block_t block), void *data) {
    if (lfs_pairisnull(lfs->root)) {
        return 0;
    }

    // iterate over metadata pairs
    lfs_mdir_t dir = {.tail = {0, 1}};
    while (!lfs_pairisnull(dir.tail)) {
        for (int i = 0; i < 2; i++) {
            int err = cb(lfs, data, dir.tail[i]);
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
            lfs_mattr_t attr;
            int err = lfs_dir_getentry(lfs, &dir, 0x7c3ff000,
                    lfs_mktag(LFS_TYPE_STRUCT, id, 0), &attr);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    continue;
                }
                return err;
            }

            if (lfs_tag_type(attr.tag) == LFS_STRUCT_CTZ) {
                err = lfs_ctz_traverse(lfs, &lfs->rcache, NULL,
                        attr.u.ctz.head, attr.u.ctz.size, cb, data);
                if (err) {
                    return err;
                }
            }
        }
    }

    // iterate over any open files
    for (lfs_file_t *f = lfs->files; f; f = f->next) {
        if ((f->flags & LFS_F_DIRTY) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctz_traverse(lfs, &lfs->rcache, &f->cache,
                    f->head, f->size, cb, data);
            if (err) {
                return err;
            }
        }

        if ((f->flags & LFS_F_WRITING) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctz_traverse(lfs, &lfs->rcache, &f->cache,
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
            if ((0x70 & entry.d.type) == LFS_STRUCT_CTZ) {
                err = lfs_ctz_traverse(lfs, &lfs->rcache, NULL,
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
            int err = lfs_ctz_traverse(lfs, &lfs->rcache, &f->cache,
                    f->head, f->size, cb, data);
            if (err) {
                return err;
            }
        }

        if ((f->flags & LFS_F_WRITING) && !(f->flags & LFS_F_INLINE)) {
            int err = lfs_ctz_traverse(lfs, &lfs->rcache, &f->cache,
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
    // iterate over all directory directory entries
    pdir->tail[0] = 0;
    pdir->tail[1] = 1;
    while (!lfs_pairisnull(pdir->tail)) {
        if (lfs_paircmp(pdir->tail, pair) == 0) {
            return true; // TODO should we return true only if pred is part of dir?
        }

        int err = lfs_dir_fetch(lfs, pdir, pdir->tail);
        if (err) {
            return err;
        }
    }

    return false;
}

static int lfs_parent(lfs_t *lfs, const lfs_block_t pair[2],
        lfs_mdir_t *parent, lfs_mattr_t *attr) {
    // search for both orderings so we can reuse the find function
    lfs_block_t child[2] = {pair[0], pair[1]};

    for (int i = 0; i < 2; i++) {
        // iterate over all directory directory entries
        parent->tail[0] = 0;
        parent->tail[1] = 1;
        while (!lfs_pairisnull(parent->tail)) {
            lfs_tag_t foundtag = -1;
            int err = lfs_dir_find(lfs, parent, parent->tail,
                    0x7fc00fff, lfs_mktag(LFS_STRUCT_DIR, 0, sizeof(child)),
                    child, &foundtag);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT) {
                // found our parent
                int err = lfs_dir_getentry(lfs, parent,
                        0x7ffff000, foundtag, attr);
                if (err) {
                    return err;
                }

                return true;
            }
        }

        lfs_pairswap(child);
    }

    return false;
}

// TODO rename to lfs_dir_relocate?
static int lfs_relocate(lfs_t *lfs,
        const lfs_block_t oldpair[2], const lfs_block_t newpair[2]) {
    // find parent
    lfs_mdir_t parent;
    lfs_mattr_t attr;
    int res = lfs_parent(lfs, oldpair, &parent, &attr);
    if (res < 0) {
        return res;
    }

    if (res) {
        // update disk, this creates a desync
        attr.u.pair[0] = newpair[0];
        attr.u.pair[1] = newpair[1];
        int err = lfs_dir_commit(lfs, &parent, &(lfs_mattrlist_t){attr});
        if (err) {
            return err;
        }

        // update internal root
        if (lfs_paircmp(oldpair, lfs->root) == 0) {
            LFS_DEBUG("Relocating root %d %d", newpair[0], newpair[1]);
            lfs->root[0] = newpair[0];
            lfs->root[1] = newpair[1];
        }

        // TODO update dir list!!?

        // clean up bad block, which should now be a desync
        return lfs_deorphan(lfs);
    }

    // find pred
    res = lfs_pred(lfs, oldpair, &parent);
    if (res < 0) {
        return res;
    }

    if (res) {
        // just replace bad pair, no desync can occur
        parent.tail[0] = newpair[0];
        parent.tail[1] = newpair[1];
        int err = lfs_dir_commit(lfs, &parent, &(lfs_mattrlist_t){
                {lfs_mktag(LFS_TYPE_TAIL + parent.split, // TODO hm
                    0x3ff, sizeof(lfs_block_t[2])),
                    .u.pair[0]=newpair[0], .u.pair[1]=newpair[1]}});
        if (err) {
            return err;
        }
    }

    // shift over any dirs/files that are affected
    for (int i = 0; i < 2; i++) {
        for (lfs_dir_t *d = ((void*[2]){lfs->dirs, lfs->files})[i];
                d; d = d->next) {
            if (lfs_paircmp(d->m.pair, oldpair) == 0) {
                d->m.pair[0] = newpair[0];
                d->m.pair[1] = newpair[1];
            }
        }
    }

    // couldn't find dir, must be new
    return 0;
}

int lfs_scan(lfs_t *lfs) {
    if (lfs_pairisnull(lfs->root)) { // TODO rm me
        return 0;
    }

    lfs_mdir_t dir = {.tail = {0, 1}};
    lfs_globals_t globals = {{{0xffffffff, 0xffffffff}, 0x3ff}};

    // iterate over all directory directory entries
    while (!lfs_pairisnull(dir.tail)) {
        int err = lfs_dir_fetch(lfs, &dir, dir.tail);
        if (err) {
            return err;
        }

        // xor together indirect deletes
        globals = lfs_globals_xor(&globals, &dir.globals);
    }

    // update littlefs with globals
    lfs->globals = globals;
    lfs->diff = (lfs_globals_t){0};
    if (!lfs_pairisnull(lfs->globals.move.pair)) {
        LFS_DEBUG("Found move %d %d %d",
                lfs->globals.move.pair[0],
                lfs->globals.move.pair[1],
                lfs->globals.move.id);
    }

    return 0;
}

int lfs_fixmove(lfs_t *lfs) {
    LFS_DEBUG("Fixing move %d %d %d", // TODO move to just deorphan
            lfs->globals.move.pair[0],
            lfs->globals.move.pair[1],
            lfs->globals.move.id);

    // mark global state to clear move entry
    lfs->diff.move.pair[0] = 0xffffffff ^ lfs->globals.move.pair[0];
    lfs->diff.move.pair[1] = 0xffffffff ^ lfs->globals.move.pair[1];
    lfs->diff.move.id      = 0x3ff      ^ lfs->globals.move.id;

    // fetch and delete the moved entry
    lfs_mdir_t movedir;
    int err = lfs_dir_fetch(lfs, &movedir, lfs->globals.move.pair);
    if (err) {
        return err;
    }

    err = lfs_dir_delete(lfs, &movedir, lfs->globals.move.id);
    if (err) {
        return err;
    }

    return 0;
}

int lfs_deorphan(lfs_t *lfs) {
    lfs->deorphaned = true;
    if (lfs_pairisnull(lfs->root)) { // TODO rm me?
        return 0;
    }

    // Fix bad moves
    if (!lfs_pairisnull(lfs->globals.move.pair)) {
        int err = lfs_fixmove(lfs);
        if (err) {
            return err;
        }
    }

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
            lfs_mattr_t attr;
            int res = lfs_parent(lfs, pdir.tail, &parent, &attr);
            if (res < 0) {
                return res;
            }

            if (!res) {
                // we are an orphan
                LFS_DEBUG("Found orphan %d %d",
                        pdir.tail[0], pdir.tail[1]);

                pdir.tail[0] = dir.tail[0];
                pdir.tail[1] = dir.tail[1];
                err = lfs_dir_commit(lfs, &pdir, &(lfs_mattrlist_t){
                        {lfs_mktag(LFS_TYPE_SOFTTAIL,
                            0x3ff, sizeof(pdir.tail)),
                            .u.buffer=pdir.tail}});
                if (err) {
                    return err;
                }

                break;
            }

            if (!lfs_pairsync(attr.u.pair, pdir.tail)) {
                // we have desynced
                LFS_DEBUG("Found half-orphan %d %d",
                        attr.u.pair[0], attr.u.pair[1]);

                pdir.tail[0] = attr.u.pair[0];
                pdir.tail[1] = attr.u.pair[1];
                err = lfs_dir_commit(lfs, &pdir, &(lfs_mattrlist_t){
                        {lfs_mktag(LFS_TYPE_SOFTTAIL,
                            0x3ff, sizeof(pdir.tail)),
                            .u.buffer=pdir.tail}});
                if (err) {
                    return err;
                }

                break;
            }
        }

        memcpy(&pdir, &dir, sizeof(pdir));
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

//static int lfs_fs_size_count(void *p, lfs_block_t block) {
//    lfs_size_t *size = p;
//    *size += 1;
//    return 0;
//}
//
//lfs_ssize_t lfs_fs_size(lfs_t *lfs) {
//    lfs_size_t size = 0;
//    int err = lfs_fs_traverse(lfs, lfs_fs_size_count, &size);
//    if (err) {
//        return err;
//    }
//
//    return size;
//}
