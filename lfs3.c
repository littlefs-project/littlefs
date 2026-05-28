/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs3.h"
#include "lfs3_util.h"


// TODO should lfs3_scmp_t/lfs3_sbool_t be moved to lfs3.h?
// TODO should all typedefs be moved to lfs3.h?
// TODO wait, should they actually go in lfs3_util.h?

// internally used disk-comparison enum
//
// note LT < EQ < GT
enum lfs3_cmp {
    LFS3_CMP_LT = 0, // disk < query
    LFS3_CMP_EQ = 1, // disk = query
    LFS3_CMP_GT = 2, // disk > query
};

typedef int lfs3_scmp_t;

// this is just a hint that the function returns a bool + err union
typedef int lfs3_sbool_t;


/// Simple bd wrappers (asserts go here) ///

static int lfs3_bd_read___(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        void *buffer, lfs3_size_t size) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);
    // must be read aligned
    LFS3_ASSERT(off % lfs3->cfg->read_size == 0);
    LFS3_ASSERT(size % lfs3->cfg->read_size == 0);

    // bd read
    int err = lfs3->cfg->read(lfs3->cfg, block, off, buffer, size);
    LFS3_ASSERT(err <= 0);
    return err;
}

#ifndef LFS3_RDONLY
static int lfs3_bd_prog___(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        const void *buffer, lfs3_size_t size) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);
    // must be prog aligned
    LFS3_ASSERT(off % lfs3->cfg->prog_size == 0);
    LFS3_ASSERT(size % lfs3->cfg->prog_size == 0);

    // bd prog
    int err = lfs3->cfg->prog(lfs3->cfg, block, off, buffer, size);
    LFS3_ASSERT(err <= 0);
    return err;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_erase___(lfs3_t *lfs3, lfs3_block_t block) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);

    // bd erase
    int err = lfs3->cfg->erase(lfs3->cfg, block);
    LFS3_ASSERT(err <= 0);
    return err;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_sync___(lfs3_t *lfs3) {
    // bd sync
    int err = lfs3->cfg->sync(lfs3->cfg);
    LFS3_ASSERT(err <= 0);
    return err;
}
#endif


/// Some eviction stuff we need for bd operations ///

// sticky block repair flags
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_DAMAGED       0x01 // Bd read was damaged
#define LFS3_REPAIR_CONDEMNED     0x02 // Bd read was condemned
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
static inline bool lfs3_repair_isdamaged(uint32_t flags) {
    return flags & LFS3_REPAIR_DAMAGED;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
static inline bool lfs3_repair_iscondemned(uint32_t flags) {
    return flags & LFS3_REPAIR_CONDEMNED;
}
#endif

// eviction stuff
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_EVICT_ISBAD 0x80000000
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_EVICT_ISDATA 0x80000000
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_evict_isbad(const lfs3_evict_t *evict) {
    return evict->block & LFS3_EVICT_ISBAD;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline lfs3_block_t lfs3_evict_block(const lfs3_evict_t *evict) {
    return evict->block & ~LFS3_EVICT_ISBAD;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_evict_isdata(const lfs3_evict_t *evict) {
    return evict->block_ & LFS3_EVICT_ISDATA;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline lfs3_block_t lfs3_evict_block_(const lfs3_evict_t *evict) {
    return evict->block_ & ~LFS3_EVICT_ISDATA;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline void lfs3_evict_discard(lfs3_t *lfs3) {
    lfs3->evictqueue.count = 0;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static lfs3_evict_t *lfs3_evict_eviction(lfs3_t *lfs3,
        lfs3_block_t block) {
    for (lfs3_size_t i = 0; i < lfs3->evictqueue.count; i++) {
        if (lfs3_evict_block(&lfs3->evictqueue.queue[i]) == block) {
            return &lfs3->evictqueue.queue[i];
        }
    }

    return NULL;
}
#endif

// returns a bool, but also takes const
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_evict_needseviction(const lfs3_t *lfs3,
        lfs3_block_t block) {
    return lfs3_evict_eviction((lfs3_t*)lfs3, block);
}
#endif

// some block eviction flags used in lfs3_evict_push
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_EVICT_BAD  0x80000000 // Block is bad
#define LFS3_EVICT_DATA 0x40000000 // Block is definitely data
#endif

// needed in lfs3_evict_push
static inline uint8_t lfs3_o_type(uint32_t flags);
static void lfs3_mtrv_damage(lfs3_mtrv_t *mtrv);

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static lfs3_evict_t *lfs3_evict_push(lfs3_t *lfs3,
        lfs3_block_t block, uint32_t flags) {
    // already in evictqueue?
    lfs3_evict_t *evict = lfs3_evict_eviction(lfs3, block);
    if (evict) {
        goto found;
    }

    // add to evictqueue if we have a slot available
    if (lfs3->evictqueue.count < lfs3->cfg->evictqueue_count) {
        evict = &lfs3->evictqueue.queue[
                lfs3->evictqueue.count++];
        evict->block = block;
        evict->block_ = 0;

        // if we enqueued a new block, mark any potentially repairing
        // traversals as damaged, otherwise don't bother because there's
        // nothing we can do
        //
        // note this also avoids marking traversals as damaged while
        // they are attempting repairs
        for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
            if (lfs3_o_type(h->flags) == LFS3_type_TRV
                    || lfs3_o_type(h->flags) == LFS3_type_GC) {
                lfs3_mtrv_damage((lfs3_mtrv_t*)h);
            }
        }

        goto found;
    }

    // quietly discard blocks if evictqueue is full
    LFS3_WARN("Evict queue overflowed 0x%"PRIx32" (%"PRIu32" > %"PRIu32")",
            block,
            lfs3->evictqueue.count+1,
            lfs3->cfg->evictqueue_count);
    return NULL;

found:;
    // note these flags all only change the behavior of completed
    // traversals, so we don't need to set the damaged flag here

    // or bad bits
    evict->block |= ((flags & LFS3_EVICT_BAD) ? LFS3_EVICT_ISBAD : 0);
    // or data bits
    evict->block_ |= ((flags & LFS3_EVICT_DATA) ? LFS3_EVICT_ISDATA : 0);
    // make sure evict/repair flags are set
    lfs3->flags |= ((flags & LFS3_EVICT_DATA)
            ? LFS3_gc_EVICTDATA
            : LFS3_gc_EVICTMETA);
    return evict;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static void lfs3_evict_flush(lfs3_t *lfs3, uint32_t flags) {
    // delete and shift relevant eviction entries
    lfs3_size_t count_ = 0;
    for (lfs3_size_t i = 0; i < lfs3->evictqueue.count; i++) {
        // this basically boils down to only keeping data evictions when
        // not evicting data
        if (lfs3_evict_isdata(&lfs3->evictqueue.queue[i])
                && !(flags & LFS3_EVICT_DATA)) {
            lfs3->evictqueue.queue[count_++] = lfs3->evictqueue.queue[i];
        }
    }
    lfs3->evictqueue.count = count_;

    // clear the relevant evict/repair flags
    lfs3->flags &= ~(
            LFS3_gc_EVICTMETA
                | ((flags & LFS3_EVICT_DATA) ? LFS3_gc_EVICTDATA : 0));
}
#endif


/// Low-level bd operations ///

// intercept block evictions, ckprogs, etc

// bd-level flags
#define LFS3_BD_RELAX   0x00000001 // Don't evict corrupt data
#define LFS3_BD_DATA    0x40000000 // A hint that we're reading data
#define LFS3_BD_ALIGN   0x00000002 // Align cksums to prog boundaries
#define LFS3_BD_PERTURB 0x80000000 // Perturb valid bit in tags

static inline bool lfs3_bd_isrelax(uint32_t flags) {
    return flags & LFS3_BD_RELAX;
}

static inline bool lfs3_bd_isdata(uint32_t flags) {
    return flags & LFS3_BD_DATA;
}

static inline bool lfs3_bd_isalign(uint32_t flags) {
    return flags & LFS3_BD_ALIGN;
}

static inline bool lfs3_bd_perturb(uint32_t flags) {
    return flags & LFS3_BD_PERTURB;
}

// needed in lfs3_bd_read__
static inline bool lfs3_f_isgbmap(uint32_t flags);

static int lfs3_bd_read__(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        void *buffer, lfs3_size_t size, uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);
    // must be read aligned
    LFS3_ASSERT(off % lfs3->cfg->read_size == 0);
    LFS3_ASSERT(size % lfs3->cfg->read_size == 0);

    // read from disk
    int err = lfs3_bd_read___(lfs3, block, off, buffer, size);
    if (err && err != LFS3_ERR_DAMAGED
            && err != LFS3_ERR_CONDEMNED) {
        if (!lfs3_bd_isrelax(flags)) {
            LFS3_INFO("Bad read 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                    block, off, size, err);
        }
        // bad? push onto our evictqueue as a block to avoid
        if (err == LFS3_ERR_BAD) {
            #if !defined(LFS3_RDONLY) \
                    && defined(LFS3_REPAIR) \
                    && defined(LFS3_GBMAP)
            if (!lfs3_bd_isrelax(flags) && lfs3_f_isgbmap(lfs3->flags)) {
                lfs3_evict_push(lfs3, block,
                        LFS3_EVICT_BAD | (flags & LFS3_BD_DATA));
            }
            #endif
            return LFS3_ERR_CORRUPT;
        }
        return err;
    }

    // damaged? condemned? push onto our evictqueue as a block to repair
    if (err == LFS3_ERR_DAMAGED
            || err == LFS3_ERR_CONDEMNED) {
        #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
        if (!lfs3_bd_isrelax(flags)) {
            // try not to spam damaged warnings
            lfs3_evict_t *evict = lfs3_evict_eviction(lfs3, block);
            if (err == LFS3_ERR_CONDEMNED
                    && (!evict || !lfs3_evict_isbad(evict))) {
                LFS3_INFO("Condemned read "
                            "0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                        block, off, size, err);
            } else if (!evict) {
                LFS3_INFO("Damaged read "
                            "0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                        block, off, size, err);
            }
        }

        // these flags are useful for upper layers, especially when
        // relaxed
        lfs3->repair_flags
                |= ((err == LFS3_ERR_CONDEMNED) ? LFS3_REPAIR_CONDEMNED : 0)
                    | LFS3_REPAIR_DAMAGED;

        if (!lfs3_bd_isrelax(flags)) {
            lfs3_evict_push(lfs3, block,
                    ((err == LFS3_ERR_CONDEMNED) ? LFS3_EVICT_BAD : 0)
                        | (flags & LFS3_BD_DATA));
        }
        #endif
    }

    return 0;
}

// needed in lfs3_bd_prog__ for prog validation
#ifdef LFS3_CKPROGS
static inline bool lfs3_m_isckprogs(uint32_t flags);
#endif
static lfs3_scmp_t lfs3_bd_cmp(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        const void *buffer, lfs3_size_t size, uint32_t flags);

// low-level prog stuff
#ifndef LFS3_RDONLY
static int lfs3_bd_prog__(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        const void *buffer, lfs3_size_t size, uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);
    // must be prog aligned
    LFS3_ASSERT(off % lfs3->cfg->prog_size == 0);
    LFS3_ASSERT(size % lfs3->cfg->prog_size == 0);

    // prog to disk
    int err = lfs3_bd_prog___(lfs3, block, off, buffer, size);
    if (err) {
        if (!lfs3_bd_isrelax(flags)) {
            LFS3_INFO("Bad prog 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                    block, off, size, err);
        }
        // damaged/condemned vs corrupt/bad are two subtly different
        // situations (for damaged/condemned the prog succeeded), but
        // either way we don't trust the data at this point so we treat
        // them the same
        if (err == LFS3_ERR_DAMAGED) {
            return LFS3_ERR_CORRUPT;
        }
        // condemned/bad? push onto our evictqueue as a block to avoid
        if (err == LFS3_ERR_CONDEMNED
                || err == LFS3_ERR_BAD) {
            #if defined(LFS3_REPAIR) && defined(LFS3_GBMAP)
            if (!lfs3_bd_isrelax(flags) && lfs3_f_isgbmap(lfs3->flags)) {
                // note we treat all bad progs/erases as metadata, we
                // abandon these so it doesn't really matter
                lfs3_evict_push(lfs3, block, LFS3_EVICT_BAD);
            }
            #endif
            return LFS3_ERR_CORRUPT;
        }
        return err;
    }

    // checking progs?
    #ifdef LFS3_CKPROGS
    if (lfs3_m_isckprogs(lfs3->flags)) {
        // pcache should have been dropped at this point
        LFS3_ASSERT(lfs3->pcache.size == 0);

        // invalidate rcache, we're going to clobber it anyways
        lfs3_bd_droprcache(lfs3);

        lfs3_scmp_t cmp = lfs3_bd_cmp(lfs3, block, off, 0,
                buffer, size);
        if (cmp < 0) {
            return cmp;
        }

        if (cmp != LFS3_CMP_EQ) {
            LFS3_WARN("Found ckprog mismatch 0x%"PRIx32".%"PRIx32" %"PRId32,
                    block, off, size);
            return LFS3_ERR_CORRUPT;
        }
    }
    #endif

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_erase__(lfs3_t *lfs3, lfs3_block_t block,
        uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);

    // erase on disk
    int err = lfs3_bd_erase___(lfs3, block);
    if (err) {
        if (!lfs3_bd_isrelax(flags)) {
            LFS3_INFO("Bad erase 0x%"PRIx32" (%d)",
                    block, err);
        }
        // damaged/condemned vs corrupt/bad are two subtly different
        // situations (for damaged/condemned the erase succeeded), but
        // either way we don't trust the data at this point so we treat
        // them the same
        if (err == LFS3_ERR_DAMAGED) {
            return LFS3_ERR_CORRUPT;
        }
        // condemned/bad? push onto our evictqueue as a block to avoid
        if (err == LFS3_ERR_CONDEMNED
                || err == LFS3_ERR_BAD) {
            #if defined(LFS3_REPAIR) && defined(LFS3_GBMAP)
            if (!lfs3_bd_isrelax(flags) && lfs3_f_isgbmap(lfs3->flags)) {
                // note we treat all bad progs/erases as metadata, we
                // abandon these so it doesn't really matter
                lfs3_evict_push(lfs3, block, LFS3_EVICT_BAD);
            }
            #endif
            return LFS3_ERR_CORRUPT;
        }
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_sync__(lfs3_t *lfs3, uint32_t flags) {
    // sync into disk
    int err = lfs3_bd_sync___(lfs3);
    if (err && !lfs3_bd_isrelax(flags)) {
        LFS3_INFO("Bad sync (%d)", err);
    }
    return err;
}
#endif


/// High-level caching bd operations ///

static inline void lfs3_bd_droprcache(lfs3_t *lfs3) {
    lfs3->rcache.size = 0;
}

#ifndef LFS3_RDONLY
static inline void lfs3_bd_droppcache(lfs3_t *lfs3) {
    lfs3->pcache.size = 0;
}
#endif

// caching read that lends you a buffer
//
// note hint has two conveniences:
//  0 => minimal caching
// -1 => maximal caching
static int lfs3_bd_readnext(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t size, uint32_t flags,
        const uint8_t **buffer_, lfs3_size_t *size_) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t hint_ = lfs3_max(hint, size); // make sure hint >= size
    while (true) {
        lfs3_size_t d = hint_;

        // already in pcache?
        #ifndef LFS3_RDONLY
        if (block == lfs3->pcache.block
                && off < lfs3->pcache.off + lfs3->pcache.size) {
            if (off >= lfs3->pcache.off) {
                *buffer_ = &lfs3->pcache.buffer[off-lfs3->pcache.off];
                *size_ = lfs3_min(
                        lfs3_min(d, size),
                        lfs3->pcache.size - (off-lfs3->pcache.off));
                return 0;
            }

            // pcache takes priority
            d = lfs3_min(d, lfs3->pcache.off - off);
        }
        #endif

        // already in rcache?
        if (block == lfs3->rcache.block
                && off < lfs3->rcache.off + lfs3->rcache.size
                && off >= lfs3->rcache.off) {
            *buffer_ = &lfs3->rcache.buffer[off-lfs3->rcache.off];
            *size_ = lfs3_min(
                    lfs3_min(d, size),
                    lfs3->rcache.size - (off-lfs3->rcache.off));
            return 0;
        }

        // drop rcache in case read fails
        lfs3_bd_droprcache(lfs3);

        // load into rcache, above conditions can no longer fail
        //
        // note it's ok if we overlap the pcache a bit, pcache always
        // takes priority until flush, which updates the rcache
        lfs3_size_t off__ = lfs3_aligndown(off, lfs3->cfg->read_size);
        lfs3_size_t size__ = lfs3_alignup(
                lfs3_min(
                    // watch out for overflow when hint_=-1!
                    (off-off__) + lfs3_min(
                        d,
                        lfs3->cfg->block_size - off),
                    lfs3->cfg->rcache_size),
                lfs3->cfg->read_size);
        int err = lfs3_bd_read__(lfs3, block, off__,
                lfs3->rcache.buffer, size__, flags);
        if (err) {
            return err;
        }

        lfs3->rcache.block = block;
        lfs3->rcache.off = off__;
        lfs3->rcache.size = size__;
    }
}

// caching read
//
// note hint has two conveniences:
//  0 => minimal caching
// -1 => maximal caching
static int lfs3_bd_read(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        void *buffer, lfs3_size_t size, uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t off_ = off;
    lfs3_size_t hint_ = lfs3_max(hint, size); // make sure hint >= size
    uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        lfs3_size_t d = hint_;

        // already in pcache?
        #ifndef LFS3_RDONLY
        if (block == lfs3->pcache.block
                && off_ < lfs3->pcache.off + lfs3->pcache.size) {
            if (off_ >= lfs3->pcache.off) {
                d = lfs3_min(
                        lfs3_min(d, size_),
                        lfs3->pcache.size - (off_-lfs3->pcache.off));
                lfs3_memcpy(buffer_,
                        &lfs3->pcache.buffer[off_-lfs3->pcache.off],
                        d);

                off_ += d;
                hint_ -= d;
                buffer_ += d;
                size_ -= d;
                continue;
            }

            // pcache takes priority
            d = lfs3_min(d, lfs3->pcache.off - off_);
        }
        #endif

        // already in rcache?
        if (block == lfs3->rcache.block
                && off_ < lfs3->rcache.off + lfs3->rcache.size) {
            if (off_ >= lfs3->rcache.off) {
                d = lfs3_min(
                        lfs3_min(d, size_),
                        lfs3->rcache.size - (off_-lfs3->rcache.off));
                lfs3_memcpy(buffer_,
                        &lfs3->rcache.buffer[off_-lfs3->rcache.off],
                        d);

                off_ += d;
                hint_ -= d;
                buffer_ += d;
                size_ -= d;
                continue;
            }

            // rcache takes priority
            d = lfs3_min(d, lfs3->rcache.off - off_);
        }

        // bypass rcache?
        if (off_ % lfs3->cfg->read_size == 0
                && lfs3_min(d, size_) >= lfs3_min(hint_, lfs3->cfg->rcache_size)
                && lfs3_min(d, size_) >= lfs3->cfg->read_size) {
            d = lfs3_aligndown(size_, lfs3->cfg->read_size);
            int err = lfs3_bd_read__(lfs3, block, off_, buffer_, d, flags);
            if (err) {
                return err;
            }

            off_ += d;
            hint_ -= d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // drop rcache in case read fails
        lfs3_bd_droprcache(lfs3);

        // load into rcache, above conditions can no longer fail
        //
        // note it's ok if we overlap the pcache a bit, pcache always
        // takes priority until flush, which updates the rcache
        lfs3_size_t off__ = lfs3_aligndown(off_, lfs3->cfg->read_size);
        lfs3_size_t size__ = lfs3_alignup(
                lfs3_min(
                    // watch out for overflow when hint_=-1!
                    (off_-off__) + lfs3_min(
                        lfs3_min(hint_, d),
                        lfs3->cfg->block_size - off_),
                    lfs3->cfg->rcache_size),
                lfs3->cfg->read_size);
        int err = lfs3_bd_read__(lfs3, block, off__,
                lfs3->rcache.buffer, size__, flags);
        if (err) {
            return err;
        }

        lfs3->rcache.block = block;
        lfs3->rcache.off = off__;
        lfs3->rcache.size = size__;
    }

    return 0;
}

// last-minute caching prog stuff
#ifndef LFS3_RDONLY
static int lfs3_bd_prog_(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        const void *buffer, lfs3_size_t size, uint32_t flags,
        uint32_t *cksum) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    // prog to disk
    int err = lfs3_bd_prog__(lfs3, block, off, buffer, size, flags);
    if (err) {
        return err;
    }

    // update rcache if we can
    if (block == lfs3->rcache.block
            && off <= lfs3->rcache.off + lfs3->rcache.size) {
        lfs3->rcache.off = lfs3_min(off, lfs3->rcache.off);
        lfs3->rcache.size = lfs3_min(
                (off-lfs3->rcache.off) + size,
                lfs3->cfg->rcache_size);
        lfs3_memcpy(&lfs3->rcache.buffer[off-lfs3->rcache.off],
                buffer,
                lfs3->rcache.size - (off-lfs3->rcache.off));
    }

    // optional prog-aligned checksum
    if (cksum && lfs3_bd_isalign(flags)) {
        *cksum = lfs3_crc32c(*cksum, buffer, size);
    }

    return 0;
}
#endif

// needed in lfs3_bd_prognext
static int lfs3_bd_flush(lfs3_t *lfs3, uint32_t flags, uint32_t *cksum);

// caching prog that lends you a buffer
//
// with optional checksum
#ifndef LFS3_RDONLY
static int lfs3_bd_prognext(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        lfs3_size_t size, uint32_t flags,
        uint8_t **buffer_, lfs3_size_t *size_,
        uint32_t *cksum) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    while (true) {
        // active pcache?
        if (lfs3->pcache.size != 0) {
            // wait, wrong block? this must be a leftover pcache due to
            // an error, discard
            if (lfs3->pcache.block != block) {
                lfs3_bd_droppcache(lfs3);
                continue;
            }

            // fits in pcache?
            if (off < lfs3->pcache.off + lfs3->cfg->pcache_size) {
                // you can't prog backwards silly
                LFS3_ASSERT(off >= lfs3->pcache.off);

                // expand the pcache?
                lfs3->pcache.size = lfs3_min(
                        (off-lfs3->pcache.off) + size,
                        lfs3->cfg->pcache_size);

                *buffer_ = &lfs3->pcache.buffer[off-lfs3->pcache.off];
                *size_ = lfs3_min(
                        size,
                        lfs3->pcache.size - (off-lfs3->pcache.off));
                return 0;
            }

            // flush pcache?
            int err = lfs3_bd_flush(lfs3, flags, cksum);
            if (err) {
                return err;
            }
        }

        // move the pcache, above conditions can no longer fail
        lfs3->pcache.block = block;
        lfs3->pcache.off = lfs3_aligndown(off, lfs3->cfg->prog_size);
        lfs3->pcache.size = lfs3_min(
                (off-lfs3->pcache.off) + size,
                lfs3->cfg->pcache_size);

        // zero to avoid any information leaks
        lfs3_memset(lfs3->pcache.buffer, 0xff, lfs3->cfg->pcache_size);
    }
}
#endif

// caching prog
//
// with optional checksum
#ifndef LFS3_RDONLY
static int lfs3_bd_prog(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        const void *buffer, lfs3_size_t size, uint32_t flags,
        uint32_t *cksum) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t off_ = off;
    const uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        // active pcache?
        if (lfs3->pcache.size != 0) {
            // wait, wrong block? this must be a leftover pcache due to
            // an error, discard
            if (lfs3->pcache.block != block) {
                lfs3_bd_droppcache(lfs3);
                continue;
            }

            // fits in pcache?
            if (off_ < lfs3->pcache.off + lfs3->cfg->pcache_size) {
                // you can't prog backwards silly
                LFS3_ASSERT(off_ >= lfs3->pcache.off);

                // expand the pcache?
                lfs3->pcache.size = lfs3_min(
                        (off_-lfs3->pcache.off) + size_,
                        lfs3->cfg->pcache_size);

                lfs3_size_t d = lfs3_min(
                        size_,
                        lfs3->pcache.size - (off_-lfs3->pcache.off));
                lfs3_memcpy(&lfs3->pcache.buffer[off_-lfs3->pcache.off],
                        buffer_,
                        d);

                off_ += d;
                buffer_ += d;
                size_ -= d;
                continue;
            }

            // flush pcache?
            //
            // flush even if we're bypassing pcache, some devices don't
            // support out-of-order progs in a block
            int err = lfs3_bd_flush(lfs3, flags, cksum);
            if (err) {
                return err;
            }
        }

        // bypass pcache?
        if (off_ % lfs3->cfg->prog_size == 0
                && size_ >= lfs3->cfg->pcache_size) {
            lfs3_size_t d = lfs3_aligndown(size_, lfs3->cfg->prog_size);
            int err = lfs3_bd_prog_(lfs3, block, off_, buffer_, d, flags,
                    cksum);
            if (err) {
                return err;
            }

            off_ += d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // move the pcache, above conditions can no longer fail
        lfs3->pcache.block = block;
        lfs3->pcache.off = lfs3_aligndown(off_, lfs3->cfg->prog_size);
        lfs3->pcache.size = lfs3_min(
                (off_-lfs3->pcache.off) + size_,
                lfs3->cfg->pcache_size);

        // zero to avoid any information leaks
        lfs3_memset(lfs3->pcache.buffer, 0xff, lfs3->cfg->pcache_size);
    }

    // optional checksum
    if (cksum && !lfs3_bd_isalign(flags)) {
        *cksum = lfs3_crc32c(*cksum, buffer, size);
    }

    return 0;
}
#endif

// flush the pcache
#ifndef LFS3_RDONLY
static int lfs3_bd_flush(lfs3_t *lfs3, uint32_t flags, uint32_t *cksum) {
    if (lfs3->pcache.size != 0) {
        // must be in-bounds
        LFS3_ASSERT(lfs3->pcache.block < lfs3->block_count);
        // must be aligned
        LFS3_ASSERT(lfs3->pcache.off % lfs3->cfg->prog_size == 0);
        lfs3_size_t size = lfs3_alignup(
                lfs3->pcache.size,
                lfs3->cfg->prog_size);

        // make this cache available, if we error anything in this cache
        // would be useless anyways
        lfs3_bd_droppcache(lfs3);

        // flush
        int err = lfs3_bd_prog_(lfs3, lfs3->pcache.block,
                lfs3->pcache.off, lfs3->pcache.buffer, size, flags,
                cksum);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_sync(lfs3_t *lfs3, uint32_t flags) {
    // make sure we flush any caches
    int err = lfs3_bd_flush(lfs3, flags, NULL);
    if (err) {
        return err;
    }

    return lfs3_bd_sync__(lfs3, flags);
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_erase(lfs3_t *lfs3, lfs3_block_t block, uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);

    // invalidate any relevant caches
    if (lfs3->pcache.block == block) {
        lfs3_bd_droppcache(lfs3);
    }
    if (lfs3->rcache.block == block) {
        lfs3_bd_droprcache(lfs3);
    }

    return lfs3_bd_erase__(lfs3, block, flags);
}
#endif


// other block device utils

static int lfs3_bd_cksum(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t size, uint32_t flags,
        uint32_t *cksum) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t off_ = off;
    lfs3_size_t hint_ = lfs3_max(hint, size); // make sure hint >= size
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        const uint8_t *buffer__;
        lfs3_size_t size__;
        int err = lfs3_bd_readnext(lfs3, block, off_, hint_, size_, flags,
                &buffer__, &size__);
        if (err) {
            return err;
        }

        *cksum = lfs3_crc32c(*cksum, buffer__, size__);

        off_ += size__;
        hint_ -= size__;
        size_ -= size__;
    }

    return 0;
}

static lfs3_scmp_t lfs3_bd_cmp(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        const void *buffer, lfs3_size_t size, uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t off_ = off;
    lfs3_size_t hint_ = lfs3_max(hint, size); // make sure hint >= size
    const uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        const uint8_t *buffer__;
        lfs3_size_t size__;
        int err = lfs3_bd_readnext(lfs3, block, off_, hint_, size_, flags,
                &buffer__, &size__);
        if (err) {
            return err;
        }

        int cmp = lfs3_memcmp(buffer__, buffer_, size__);
        if (cmp != 0) {
            return (cmp < 0) ? LFS3_CMP_LT : LFS3_CMP_GT;
        }

        off_ += size__;
        hint_ -= size__;
        buffer_ += size__;
        size_ -= size__;
    }

    return LFS3_CMP_EQ;
}

#ifndef LFS3_RDONLY
static int lfs3_bd_cpy(lfs3_t *lfs3,
        lfs3_block_t dst_block, lfs3_size_t dst_off,
        lfs3_block_t src_block, lfs3_size_t src_off, lfs3_size_t hint,
        lfs3_size_t size, uint32_t flags,
        uint32_t *cksum) {
    // must be in-bounds
    LFS3_ASSERT(dst_block < lfs3->block_count);
    LFS3_ASSERT(dst_off+size <= lfs3->cfg->block_size);
    LFS3_ASSERT(src_block < lfs3->block_count);
    LFS3_ASSERT(src_off+size <= lfs3->cfg->block_size);

    lfs3_size_t dst_off_ = dst_off;
    lfs3_size_t src_off_ = src_off;
    lfs3_size_t hint_ = lfs3_max(hint, size); // make sure hint >= size
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        // prefer the pcache here to avoid rcache conflicts with prog
        // validation, if we're lucky we might even be able to avoid
        // clobbering the rcache at all
        uint8_t *buffer__;
        lfs3_size_t size__;
        int err = lfs3_bd_prognext(lfs3, dst_block, dst_off_, size_, flags,
                &buffer__, &size__,
                cksum);
        if (err) {
            return err;
        }

        err = lfs3_bd_read(lfs3, src_block, src_off_, hint_,
                buffer__, size__, flags);
        if (err) {
            return err;
        }

        // optional checksum
        if (cksum && !lfs3_bd_isalign(flags)) {
            *cksum = lfs3_crc32c(*cksum, buffer__, size__);
        }

        dst_off_ += size__;
        src_off_ += size__;
        hint_ -= size__;
        size_ -= size__;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_set(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        uint8_t c, lfs3_size_t size, uint32_t flags,
        uint32_t *cksum) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t off_ = off;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        uint8_t *buffer__;
        lfs3_size_t size__;
        int err = lfs3_bd_prognext(lfs3, block, off_, size_, flags,
                &buffer__, &size__,
                cksum);
        if (err) {
            return err;
        }

        lfs3_memset(buffer__, c, size__);

        // optional checksum
        if (cksum && !lfs3_bd_isalign(flags)) {
            *cksum = lfs3_crc32c(*cksum, buffer__, size__);
        }

        off_ += size__;
        size_ -= size__;
    }

    return 0;
}
#endif


// lfs3_ptail_t stuff
//
// ptail tracks the most recent trunk's parity so we can parity-check
// if it hasn't been written to disk yet

#if !defined(LFS3_RDONLY) && defined(LFS3_CKMETAPARITY)
#define LFS3_PTAIL_PARITY 0x80000000
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_CKMETAPARITY)
static inline bool lfs3_ptail_parity(const lfs3_t *lfs3) {
    return lfs3->ptail.off & LFS3_PTAIL_PARITY;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_CKMETAPARITY)
static inline lfs3_size_t lfs3_ptail_off(const lfs3_t *lfs3) {
    return lfs3->ptail.off & ~LFS3_PTAIL_PARITY;
}
#endif


// checked read helpers

#ifdef LFS3_CKDATACKSUMS
static int lfs3_bd_ckprefix(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t cksize, uint32_t cksum, uint32_t flags,
        lfs3_size_t *hint_,
        uint32_t *cksum__) {
    (void)cksum;
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(cksize <= lfs3->cfg->block_size);

    // make sure hint includes our prefix/suffix
    lfs3_size_t hint__ = lfs3_max(
            // watch out for overflow when hint=-1!
            off + lfs3_min(
                hint,
                lfs3->cfg->block_size - off),
            cksize);

    // checksum any prefixed data
    int err = lfs3_bd_cksum(lfs3,
            block, 0, hint__,
            off, flags,
            cksum__);
    if (err) {
        return err;
    }

    // return adjusted hint, note we clamped this to a positive range
    // earlier, otherwise we'd have real problems with hint=-1!
    *hint_ = hint__ - off;
    return 0;
}
#endif

#ifdef LFS3_CKDATACKSUMS
static int lfs3_bd_cksuffix(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t cksize, uint32_t cksum, uint32_t flags,
        uint32_t cksum__) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(cksize <= lfs3->cfg->block_size);

    // checksum any suffixed data
    int err = lfs3_bd_cksum(lfs3,
            block, off, hint,
            cksize - off, flags,
            &cksum__);
    if (err) {
        return err;
    }

    // do checksums match?
    if (cksum__ != cksum) {
        LFS3_ERROR("Found ckdatacksums mismatch "
                    "0x%"PRIx32".%"PRIx32" %"PRId32", "
                    "cksum %08"PRIx32" (!= %08"PRIx32")",
                block, 0, cksize,
                cksum__, cksum);
        return LFS3_ERR_CORRUPT;
    }

    return 0;
}
#endif


// checked read functions

// caching read with parity/checksum checks
//
// the main downside of checking reads is we need to read all data that
// contributes to the relevant parity/checksum, this may be
// significantly more than the data we actually end up using
//
#ifdef LFS3_CKDATACKSUMS
static int lfs3_bd_ckread(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t cksize, uint32_t cksum,
        void *buffer, lfs3_size_t size, uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(cksize <= lfs3->cfg->block_size);
    // read should fit in ck info
    LFS3_ASSERT(off+size <= cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs3_size_t hint_;
    int err = lfs3_bd_ckprefix(lfs3, block, off, hint,
            cksize, cksum, flags,
            &hint_,
            &cksum__);
    if (err) {
        return err;
    }

    // read and checksum the data we're interested in
    err = lfs3_bd_read(lfs3,
            block, off, hint_,
            buffer, size, flags);
    if (err) {
        return err;
    }

    cksum__ = lfs3_crc32c(cksum__, buffer, size);

    // checksum any suffixed data and validate
    err = lfs3_bd_cksuffix(lfs3, block, off+size, hint_-size,
            cksize, cksum, flags,
            cksum__);
    if (err) {
        return err;
    }

    return 0;
}
#endif

// these could probably be a bit better deduplicated with their
// unchecked counterparts, but we don't generally use both at the same
// time
//
// we'd also need to worry about early termination in lfs3_bd_cmp/ckcmp

#ifdef LFS3_CKDATACKSUMS
static lfs3_scmp_t lfs3_bd_ckcmp(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t cksize, uint32_t cksum,
        const void *buffer, lfs3_size_t size, uint32_t flags) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(cksize <= lfs3->cfg->block_size);
    // read should fit in ck info
    LFS3_ASSERT(off+size <= cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs3_size_t hint_;
    int err = lfs3_bd_ckprefix(lfs3, block, off, hint,
            cksize, cksum, flags,
            &hint_,
            &cksum__);
    if (err) {
        return err;
    }

    // compare the data while simultaneously updating the checksum
    lfs3_size_t off_ = off;
    lfs3_size_t hint__ = hint_ - off;
    const uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;
    int cmp = LFS3_CMP_EQ;
    while (size_ > 0) {
        const uint8_t *buffer__;
        lfs3_size_t size__;
        err = lfs3_bd_readnext(lfs3, block, off_, hint__, size_, flags,
                &buffer__, &size__);
        if (err) {
            return err;
        }

        cksum__ = lfs3_crc32c(cksum__, buffer__, size__);

        if (cmp == LFS3_CMP_EQ) {
            int cmp_ = lfs3_memcmp(buffer__, buffer_, size__);
            if (cmp_ != 0) {
                cmp = (cmp_ < 0) ? LFS3_CMP_LT : LFS3_CMP_GT;
            }
        }

        off_ += size__;
        hint__ -= size__;
        buffer_ += size__;
        size_ -= size__;
    }

    // checksum any suffixed data and validate
    err = lfs3_bd_cksuffix(lfs3, block, off+size, hint_-size,
            cksize, cksum, flags,
            cksum__);
    if (err) {
        return err;
    }

    return cmp;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_CKDATACKSUMS)
static int lfs3_bd_ckcpy(lfs3_t *lfs3,
        lfs3_block_t dst_block, lfs3_size_t dst_off,
        lfs3_block_t src_block, lfs3_size_t src_off, lfs3_size_t hint,
        lfs3_size_t size,
        lfs3_size_t src_cksize, uint32_t src_cksum,
        uint32_t flags,
        uint32_t *cksum) {
    // must be in-bounds
    LFS3_ASSERT(dst_block < lfs3->block_count);
    LFS3_ASSERT(dst_off+size <= lfs3->cfg->block_size);
    LFS3_ASSERT(src_block < lfs3->block_count);
    LFS3_ASSERT(src_cksize <= lfs3->cfg->block_size);
    // read should fit in ck info
    LFS3_ASSERT(src_off+size <= src_cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs3_size_t hint_;
    int err = lfs3_bd_ckprefix(lfs3, src_block, src_off, hint,
            src_cksize, src_cksum, flags,
            &hint_,
            &cksum__);
    if (err) {
        return err;
    }

    // copy the data while simultaneously updating our checksum
    lfs3_size_t dst_off_ = dst_off;
    lfs3_size_t src_off_ = src_off;
    lfs3_size_t hint__ = hint_;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        // prefer the pcache here to avoid rcache conflicts with prog
        // validation, if we're lucky we might even be able to avoid
        // clobbering the rcache at all
        uint8_t *buffer__;
        lfs3_size_t size__;
        err = lfs3_bd_prognext(lfs3, dst_block, dst_off_, size_, flags,
                &buffer__, &size__,
                cksum);
        if (err) {
            return err;
        }

        err = lfs3_bd_read(lfs3, src_block, src_off_, hint__,
                buffer__, size__, flags);
        if (err) {
            return err;
        }

        // validating checksum
        cksum__ = lfs3_crc32c(cksum__, buffer__, size__);

        // optional prog checksum
        if (cksum && !lfs3_bd_isalign(flags)) {
            *cksum = lfs3_crc32c(*cksum, buffer__, size__);
        }

        dst_off_ += size__;
        src_off_ += size__;
        hint__ -= size__;
        size_ -= size__;
    }

    // checksum any suffixed data and validate
    err = lfs3_bd_cksuffix(lfs3, src_block, src_off+size, hint_-size,
            src_cksize, src_cksum, flags,
            cksum__);
    if (err) {
        return err;
    }

    return 0;
}
#endif


/// Some quick decoders ///

// should probably prefer the lfs3_data_* functions, but these can be
// useful when low-level hints/flags are needed

static int lfs3_bd_readle32(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint, uint32_t flags,
        uint32_t *word_) {
    // truncated?
    if (lfs3->cfg->block_size-off < 4) {
        return LFS3_ERR_CORRUPT;
    }

    int err = lfs3_bd_read(lfs3, block, off, hint,
            word_, 4, flags);
    if (err) {
        return err;
    }

    *word_ = lfs3_fromle32(word_);
    return 0;
}

// note all leb128s in our system reserve the sign bit
static lfs3_ssize_t lfs3_bd_readleb128(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint, uint32_t flags,
        uint32_t *word_) {
    // for 32-bits we can assume worst-case leb128 size is 5-bytes
    uint8_t buf[5];
    int err = lfs3_bd_read(lfs3, block, off, hint,
            buf, lfs3->cfg->block_size-off, flags);
    if (err) {
        return err;
    }

    lfs3_ssize_t d = lfs3_fromleb128(word_, buf, lfs3->cfg->block_size-off);
    if (d < 0) {
        return d;
    }
    // all leb128s in our system reserve the sign bit
    if (*word_ > 0x7fffffff) {
        return LFS3_ERR_CORRUPT;
    }

    return d;
}

// a little-leb128 in our system is truncated to align nicely
//
// for 32-bit words, little-leb128s are truncated to 28-bits, so the
// resulting leb128 encoding fits nicely in 4-bytes
static lfs3_ssize_t lfs3_bd_readlleb128(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint, uint32_t flags,
        uint32_t *word_) {
    // just call readleb128 here
    lfs3_ssize_t d = lfs3_bd_readleb128(lfs3, block, off, hint, flags,
            word_);
    if (d < 0) {
        return d;
    }
    // little-leb128s should be limited to 28-bits
    if (*word_ > 0x0fffffff) {
        return LFS3_ERR_CORRUPT;
    }

    return d;
}



/// Tags - lfs3_tag_t stuff ///

// tag type operations
static inline lfs3_tag_t lfs3_tag_mode(lfs3_tag_t tag) {
    return tag & 0xf000;
}

static inline lfs3_tag_t lfs3_tag_suptype(lfs3_tag_t tag) {
    return tag & 0xff00;
}

static inline uint8_t lfs3_tag_subtype(lfs3_tag_t tag) {
    return tag & 0x00ff;
}

static inline lfs3_tag_t lfs3_tag_key(lfs3_tag_t tag) {
    return tag & 0x0fff;
}

static inline lfs3_tag_t lfs3_tag_supkey(lfs3_tag_t tag) {
    return tag & 0x0f00;
}

static inline lfs3_tag_t lfs3_tag_subkey(lfs3_tag_t tag) {
    return tag & 0x00ff;
}

static inline uint8_t lfs3_tag_redund(lfs3_tag_t tag) {
    return tag & 0x0003;
}

static inline bool lfs3_tag_isalt(lfs3_tag_t tag) {
    return tag & LFS3_TAG_ALT;
}

static inline bool lfs3_tag_isshrub(lfs3_tag_t tag) {
    return tag & LFS3_TAG_SHRUB;
}

static inline bool lfs3_tag_istrunk(lfs3_tag_t tag) {
    return lfs3_tag_mode(tag) != LFS3_TAG_CKSUM;
}

static inline uint8_t lfs3_tag_phase(lfs3_tag_t tag) {
    return tag & LFS3_TAG_PHASE;
}

static inline bool lfs3_tag_perturb(lfs3_tag_t tag) {
    return tag & LFS3_TAG_PERTURB;
}

static inline bool lfs3_tag_isrm(lfs3_tag_t tag) {
    return tag & LFS3_tag_RM;
}

static inline bool lfs3_tag_isgrow(lfs3_tag_t tag) {
    return tag & LFS3_tag_GROW;
}

static inline bool lfs3_tag_ismask0(lfs3_tag_t tag) {
    return ((tag >> 12) & 0x3) == 0;
}

static inline bool lfs3_tag_ismask2(lfs3_tag_t tag) {
    return ((tag >> 12) & 0x3) == 1;
}

static inline bool lfs3_tag_ismask8(lfs3_tag_t tag) {
    return ((tag >> 12) & 0x3) == 2;
}

static inline bool lfs3_tag_ismask12(lfs3_tag_t tag) {
    return ((tag >> 12) & 0x3) == 3;
}

static inline lfs3_tag_t lfs3_tag_mask(lfs3_tag_t tag) {
    // this is based off the parity impl in Sean Eron Anderson's Bit
    // Twiddling Hacks, who attributes the idea to Mathew Hendry
    //
    // basically the idea is to encode a small lookup table in an
    // integer, and extract using a shift + mask
    //
    //                             .-- LFS3_tag_MASK0
    //                            .|-- LFS3_tag_MASK2
    //                           .||-- LFS3_tag_MASK8
    //                          .|||-- LFS3_tag_MASK12
    //                          vvvv
    return 0x0fff & (-1U << ((0xc820 >> (4*((tag >> 12) & 0x3))) & 0xf));
    //     '--.-'      ^                   '--------.--------'
    //     key mask  gcc complains w/o this     mask bits
}

// alt operations
static inline bool lfs3_tag_isblack(lfs3_tag_t tag) {
    return !(tag & LFS3_TAG_R);
}

static inline bool lfs3_tag_isred(lfs3_tag_t tag) {
    return tag & LFS3_TAG_R;
}

static inline bool lfs3_tag_isle(lfs3_tag_t tag) {
    return !(tag & LFS3_TAG_GT);
}

static inline bool lfs3_tag_isgt(lfs3_tag_t tag) {
    return tag & LFS3_TAG_GT;
}

static inline lfs3_tag_t lfs3_tag_isparallel(lfs3_tag_t a, lfs3_tag_t b) {
    return (a & LFS3_TAG_GT) == (b & LFS3_TAG_GT);
}

static inline bool lfs3_tag_follow(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid,
        lfs3_srid_t rid, lfs3_tag_t tag) {
    // null tags break the following logic for unreachable alts
    LFS3_ASSERT(lfs3_tag_key(tag) != 0);

    if (lfs3_tag_isgt(alt)) {
        return rid > upper_rid - (lfs3_srid_t)weight - 1
                || (rid == upper_rid - (lfs3_srid_t)weight - 1
                    && lfs3_tag_key(tag) > lfs3_tag_key(alt));
    } else {
        return rid < lower_rid + (lfs3_srid_t)weight - 1
                || (rid == lower_rid + (lfs3_srid_t)weight - 1
                    && lfs3_tag_key(tag) <= lfs3_tag_key(alt));
    }
}

static inline bool lfs3_tag_follow2(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_tag_t alt2, lfs3_rid_t weight2,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid,
        lfs3_srid_t rid, lfs3_tag_t tag) {
    if (lfs3_tag_isred(alt2) && lfs3_tag_isparallel(alt, alt2)) {
        weight += weight2;
    }

    return lfs3_tag_follow(alt, weight, lower_rid, upper_rid, rid, tag);
}

static inline void lfs3_tag_flip(
        lfs3_tag_t *alt, lfs3_rid_t *weight,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid) {
    *alt = *alt ^ LFS3_TAG_GT;
    *weight = (upper_rid - lower_rid) - *weight;
}

static inline void lfs3_tag_flip2(
        lfs3_tag_t *alt, lfs3_rid_t *weight,
        lfs3_tag_t alt2, lfs3_rid_t weight2,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid) {
    if (lfs3_tag_isred(alt2)) {
        *weight += weight2;
    }

    lfs3_tag_flip(alt, weight, lower_rid, upper_rid);
}

static inline void lfs3_tag_trim(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_srid_t *lower_rid, lfs3_srid_t *upper_rid,
        lfs3_tag_t *lower_tag, lfs3_tag_t *upper_tag) {
    if (lfs3_tag_isgt(alt)) {
        *upper_rid -= weight;
        if (upper_tag) {
            *upper_tag = alt + 1;
        }
    } else {
        *lower_rid += weight;
        if (lower_tag) {
            *lower_tag = alt;
        }
    }
}

static inline void lfs3_tag_trim2(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_tag_t alt2, lfs3_rid_t weight2,
        lfs3_srid_t *lower_rid, lfs3_srid_t *upper_rid,
        lfs3_tag_t *lower_tag, lfs3_tag_t *upper_tag) {
    if (lfs3_tag_isred(alt2)) {
        lfs3_tag_trim(
                alt2, weight2,
                lower_rid, upper_rid,
                lower_tag, upper_tag);
    }

    lfs3_tag_trim(
            alt, weight,
            lower_rid, upper_rid,
            lower_tag, upper_tag);
}

static inline bool lfs3_tag_unreachable(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid,
        lfs3_tag_t lower_tag, lfs3_tag_t upper_tag) {
    if (lfs3_tag_isgt(alt)) {
        return !lfs3_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                upper_rid-1, upper_tag-1);
    } else {
        return !lfs3_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                lower_rid-1, lower_tag+1);
    }
}

static inline bool lfs3_tag_unreachable2(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_tag_t alt2, lfs3_rid_t weight2,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid,
        lfs3_tag_t lower_tag, lfs3_tag_t upper_tag) {
    if (lfs3_tag_isred(alt2)) {
        lfs3_tag_trim(
                alt2, weight2,
                &lower_rid, &upper_rid,
                &lower_tag, &upper_tag);
    }

    return lfs3_tag_unreachable(
            alt, weight,
            lower_rid, upper_rid,
            lower_tag, upper_tag);
}

static inline bool lfs3_tag_diverging(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid,
        lfs3_srid_t a_rid, lfs3_tag_t a_tag,
        lfs3_srid_t b_rid, lfs3_tag_t b_tag) {
    return lfs3_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                a_rid, a_tag)
            != lfs3_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                b_rid, b_tag);
}

static inline bool lfs3_tag_diverging2(
        lfs3_tag_t alt, lfs3_rid_t weight,
        lfs3_tag_t alt2, lfs3_rid_t weight2,
        lfs3_srid_t lower_rid, lfs3_srid_t upper_rid,
        lfs3_srid_t a_rid, lfs3_tag_t a_tag,
        lfs3_srid_t b_rid, lfs3_tag_t b_tag) {
    return lfs3_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_rid, upper_rid,
                a_rid, a_tag)
            != lfs3_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_rid, upper_rid,
                b_rid, b_tag);
}


// support for encoding/decoding tags on disk

// needed in lfs3_bd_readtag
#ifdef LFS3_CKMETAPARITY
static inline bool lfs3_m_isckparity(uint32_t flags);
#endif

static lfs3_ssize_t lfs3_bd_readtag(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint, uint32_t flags,
        lfs3_tag_t *tag_, lfs3_rid_t *weight_, lfs3_size_t *size_,
        uint32_t *cksum) {
    // read the largest possible tag size
    uint8_t tag_buf[LFS3_TAG_DSIZE];
    lfs3_size_t tag_dsize = lfs3_min(
            LFS3_TAG_DSIZE,
            lfs3->cfg->block_size-off);
    if (tag_dsize < 4) {
        return LFS3_ERR_CORRUPT;
    }

    int err = lfs3_bd_read(lfs3, block, off, hint,
            tag_buf, tag_dsize, flags);
    if (err) {
        return err;
    }

    // check the valid bit?
    if (cksum) {
        // on-disk, the tag's valid bit must reflect the parity of the
        // preceding data
        //
        // fortunately crc32cs are parity-preserving, so this is the
        // same as the parity of the checksum
        if ((tag_buf[0] >> 7) != lfs3_parity(*cksum)) {
            return LFS3_ERR_CORRUPT;
        }
    }

    lfs3_tag_t tag
            = ((lfs3_tag_t)tag_buf[0] << 8)
            | ((lfs3_tag_t)tag_buf[1] << 0);
    lfs3_ssize_t d = 2;

    lfs3_rid_t weight;
    lfs3_ssize_t d_ = lfs3_fromleb128(&weight, &tag_buf[d], tag_dsize-d);
    if (d_ < 0) {
        return d_;
    }
    // weights should be limited to 31-bits
    if (weight > 0x7fffffff) {
        return LFS3_ERR_CORRUPT;
    }
    d += d_;

    lfs3_size_t size;
    d_ = lfs3_fromleb128(&size, &tag_buf[d], tag_dsize-d);
    if (d_ < 0) {
        return d_;
    }
    // sizes should be limited to 28-bits
    if (size > 0x0fffffff) {
        return LFS3_ERR_CORRUPT;
    }
    d += d_;

    // check our tag does not go out of bounds
    if (!lfs3_tag_isalt(tag) && off+d + size > lfs3->cfg->block_size) {
        return LFS3_ERR_CORRUPT;
    }

    // check the parity if we're checking parity
    //
    // this requires reading all of the data as well, but with any luck
    // the data will stick around in the cache
    #ifdef LFS3_CKMETAPARITY
    if (lfs3_m_isckparity(lfs3->flags)
            // don't bother checking parity if we're already calculating
            // a checksum
            && !cksum) {
        // checksum the tag, including our valid bit
        uint32_t cksum_ = lfs3_crc32c(0, tag_buf, d);

        // checksum the data, if we have any
        lfs3_size_t hint_ = hint - lfs3_min(d, hint);
        lfs3_size_t d_ = d;
        if (!lfs3_tag_isalt(tag)) {
            err = lfs3_bd_cksum(lfs3,
                    // make sure hint includes our pesky parity byte
                    block, off+d_, lfs3_max(hint_, size+1),
                    size, flags,
                    &cksum_);
            if (err) {
                return err;
            }

            hint_ -= lfs3_min(size, hint_);
            d_ += size;
        }

        // pesky parity byte
        if (off+d_ > lfs3->cfg->block_size-1) {
            return LFS3_ERR_CORRUPT;
        }

        // read the pesky parity byte
        //
        // _usually_, the byte following a tag contains the tag's parity
        //
        // unless we're in the middle of building a commit, where things get
        // tricky... to avoid problems with not-yet-written parity bits
        // ptail tracks the most recent trunk's parity
        //

        // parity in in ptail?
        bool parity;
        if (LFS3_IFDEF_RDONLY(
                false,
                block == lfs3->ptail.block
                    && off+d_ == lfs3_ptail_off(lfs3))) {
            #ifndef LFS3_RDONLY
            parity = lfs3_ptail_parity(lfs3);
            #endif

        // parity on disk?
        } else {
            uint8_t p;
            err = lfs3_bd_read(lfs3, block, off+d_, hint_,
                    &p, 1, flags);
            if (err) {
                return err;
            }

            parity = p >> 7;
        }

        // does parity match?
        if (lfs3_parity(cksum_) != parity) {
            LFS3_ERROR("Found ckparity mismatch "
                        "0x%"PRIx32".%"PRIx32" %"PRId32", "
                        "parity %01"PRIx32" (!= %01"PRIx32")",
                    block, off, d_,
                    lfs3_parity(cksum_), parity);
            return LFS3_ERR_CORRUPT;
        }
    }
    #endif

    // optional checksum
    if (cksum) {
        // exclude valid bit from checksum
        *cksum ^= tag_buf[0] & 0x00000080;
        // calculate checksum
        *cksum = lfs3_crc32c(*cksum, tag_buf, d);
    }

    // save what we found, clearing the valid bit, we don't need it
    // anymore
    *tag_ = tag & 0x7fff;
    *weight_ = weight;
    *size_ = size;
    return d;
}

#ifndef LFS3_RDONLY
static lfs3_ssize_t lfs3_bd_progtag(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off,
        lfs3_tag_t tag, lfs3_rid_t weight, lfs3_size_t size, uint32_t flags,
        uint32_t *cksum) {
    // we set the valid bit here
    LFS3_ASSERT(!(tag & 0x8000));
    // bit 7 is reserved for future subtype extensions
    LFS3_ASSERT(!(tag & 0x80));
    // weight should not exceed 31-bits
    LFS3_ASSERT(weight <= 0x7fffffff);
    // size should not exceed 28-bits
    LFS3_ASSERT(size <= 0x0fffffff);

    // set the valid bit to the parity of the current checksum, inverted
    // if the perturb bit is set, and exclude from the next checksum
    LFS3_ASSERT(cksum);
    bool v = lfs3_parity(*cksum) ^ lfs3_bd_perturb(flags);
    tag |= (lfs3_tag_t)v << 15;
    *cksum ^= (uint32_t)v << 7;

    // encode into a be16 and pair of leb128s
    uint8_t tag_buf[LFS3_TAG_DSIZE];
    tag_buf[0] = (uint8_t)(tag >> 8);
    tag_buf[1] = (uint8_t)(tag >> 0);
    lfs3_ssize_t d = 2;

    lfs3_ssize_t d_ = lfs3_toleb128(weight, &tag_buf[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfs3_toleb128(size, &tag_buf[d], 4);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    int err = lfs3_bd_prog(lfs3, block, off, tag_buf, d, flags,
            cksum);
    if (err) {
        return err;
    }

    return d;
}
#endif


/// Data - lfs3_data_t stuff ///

#define LFS3_DATA_ONDISK 0x80000000
#define LFS3_DATA_ISHOLE 0x40000000
#define LFS3_DATA_ISBPTR 0x40000000

#ifdef LFS3_CKDATACKSUMS
#define LFS3_DATA_ISERASED 0x80000000
#endif

#define LFS3_DATA_NULL() \
    ((lfs3_data_t){ \
        .weight=0, \
        .off=0, \
        .u.buffer=NULL})

#define LFS3_DATA_BUF(_buffer, _size) \
    ((lfs3_data_t){ \
        .weight=_size, \
        .off=0, \
        .u.buffer=(const void*)(_buffer)})

#define LFS3_DATA_HOLE(_weight) \
    ((lfs3_data_t){ \
        .weight=_weight, \
        .off=LFS3_DATA_ISHOLE})

#define LFS3_DATA_DISK(_block, _off, _size) \
    ((lfs3_data_t){ \
        .weight=_size, \
        .off=LFS3_DATA_ONDISK | (_off), \
        .u.disk.block=_block})

// data helpers
static inline bool lfs3_data_ondisk(const lfs3_data_t *data) {
    return data->off & LFS3_DATA_ONDISK;
}

static inline bool lfs3_data_isbuf(const lfs3_data_t *data) {
    return !(data->off & (LFS3_DATA_ONDISK | LFS3_DATA_ISHOLE));
}

static inline bool lfs3_data_ishole(const lfs3_data_t *data) {
    return (data->off & (LFS3_DATA_ONDISK | LFS3_DATA_ISHOLE))
            == LFS3_DATA_ISHOLE;
}

static inline bool lfs3_data_isfragment(const lfs3_data_t *data) {
    return !(data->off & LFS3_DATA_ISBPTR);
}

static inline bool lfs3_data_isbptr(const lfs3_data_t *data) {
    return (data->off & (LFS3_DATA_ONDISK | LFS3_DATA_ISBPTR))
            == (LFS3_DATA_ONDISK | LFS3_DATA_ISBPTR);
}

static inline uint32_t lfs3_data_flags(const lfs3_data_t *data) {
    // the data -> bd flags mapping is pretty simple:
    // - isfragment => metadata
    // - isbptr     => data
    // and wow, they're even the same bit, what a coincidence
    return data->off & LFS3_DATA_ISBPTR;
}

static inline lfs3_size_t lfs3_data_off(const lfs3_data_t *data) {
    return data->off & ~(LFS3_DATA_ONDISK | LFS3_DATA_ISBPTR);
}

static inline lfs3_size_t lfs3_data_size(const lfs3_data_t *data) {
    // this is just a hint that size should be <= lfs3_size_t
    // i.e., not a hole
    return data->weight;
}

#ifdef LFS3_CKDATACKSUMS
static inline lfs3_size_t lfs3_data_cksize(const lfs3_data_t *data) {
    return data->u.disk.cksize & ~LFS3_DATA_ISERASED;
}
#endif

// data slicing
static inline void lfs3_data_slice(lfs3_data_t *data,
        lfs3_ssize_t off, lfs3_ssize_t size) {
    // limit our off/size to data range, note the use of unsigned casts
    // here to treat -1 as unbounded
    lfs3_size_t off_ = lfs3_min(
            lfs3_smax(off, 0),
            lfs3_data_size(data));
    lfs3_size_t size_ = lfs3_min(
            (lfs3_size_t)size,
            lfs3_data_size(data) - off_);

    // for extra cool points we use a common off field, so we don't need
    // to figure out the type when slicing
    //
    // Note, though, this is a bit of a problem for holes (in bptrs),
    // where off can overflow. We don't care about off in holes, but if
    // the overflow messed with the type bits that'd be bad.
    //
    // Holes are the only reason we need this mask.
    data->off = (data->off & (LFS3_DATA_ONDISK | LFS3_DATA_ISBPTR))
            | ((data->off + off_) & ~(LFS3_DATA_ONDISK | LFS3_DATA_ISBPTR));
    data->weight = size_;
}

static inline lfs3_data_t lfs3_data_fromslice(const lfs3_data_t *data,
        lfs3_ssize_t off, lfs3_ssize_t size) {
    lfs3_data_t data_ = *data;
    lfs3_data_slice(&data_, off, size);
    return data_;
}

// this macro provides an lvalue for use in other macros, but compound
// literals currently optimize poorly, so measure before use and consider
// just using lfs3_data_slice instead
#define LFS3_DATA_SLICE(_data, _off, _size) \
    ((struct {lfs3_data_t d;}){lfs3_data_fromslice(_data, _off, _size)}.d)


// data <-> bd interactions

// lfs3_data_read* operations update the lfs3_data_t, effectively
// consuming the data

// needed in lfs3_data_read and friends
#ifdef LFS3_CKDATACKSUMS
static inline bool lfs3_m_isckdatacksums(uint32_t flags);
#endif

static lfs3_ssize_t lfs3_data_read(lfs3_t *lfs3, lfs3_data_t *data,
        void *buffer, lfs3_size_t size) {
    // we shouldn't end up with holes here
    LFS3_ASSERT(!lfs3_data_ishole(data));
    // limit our size to data range
    lfs3_size_t d = lfs3_min(size, lfs3_data_size(data));

    // on-disk?
    if (lfs3_data_ondisk(data)) {
        // validating data cksums?
        if (LFS3_IFDEF_CKDATACKSUMS(
                lfs3_m_isckdatacksums(lfs3->flags)
                    && lfs3_data_isbptr(data),
                false)) {
            #ifdef LFS3_CKDATACKSUMS
            int err = lfs3_bd_ckread(lfs3,
                    data->u.disk.block,
                    lfs3_data_off(data),
                    // note our hint includes the full data range
                    lfs3_data_size(data),
                    lfs3_data_cksize(data), data->u.disk.cksum,
                    buffer, d,
                    lfs3_data_flags(data));
            if (err) {
                return err;
            }
            #endif

        } else {
            int err = lfs3_bd_read(lfs3,
                    data->u.disk.block,
                    lfs3_data_off(data),
                    // note our hint includes the full data range
                    lfs3_data_size(data),
                    buffer, d,
                    lfs3_data_flags(data));
            if (err) {
                return err;
            }
        }

    // buffer?
    } else {
        lfs3_memcpy(buffer,
                data->u.buffer + lfs3_data_off(data),
                d);
    }

    lfs3_data_slice(data, d, -1);
    return d;
}

static int lfs3_data_readle32(lfs3_t *lfs3, lfs3_data_t *data,
        uint32_t *word_) {
    lfs3_ssize_t d = lfs3_data_read(lfs3, data, word_, 4);
    if (d < 0) {
        return d;
    }
    // truncated?
    if (d < 4) {
        return LFS3_ERR_CORRUPT;
    }

    *word_ = lfs3_fromle32(word_);
    return 0;
}

// note all leb128s in our system reserve the sign bit
static int lfs3_data_readleb128(lfs3_t *lfs3, lfs3_data_t *data,
        uint32_t *word_) {
    // note we make sure not to update our data offset until after leb128
    // decoding
    lfs3_data_t data_ = *data;

    // for 32-bits we can assume worst-case leb128 size is 5-bytes
    uint8_t buf[5];
    lfs3_ssize_t d = lfs3_data_read(lfs3, &data_, buf, 5);
    if (d < 0) {
        return d;
    }

    d = lfs3_fromleb128(word_, buf, d);
    if (d < 0) {
        return d;
    }
    // all leb128s in our system reserve the sign bit
    if (*word_ > 0x7fffffff) {
        return LFS3_ERR_CORRUPT;
    }

    lfs3_data_slice(data, d, -1);
    return 0;
}

// a little-leb128 in our system is truncated to align nicely
//
// for 32-bit words, little-leb128s are truncated to 28-bits, so the
// resulting leb128 encoding fits nicely in 4-bytes
static int lfs3_data_readlleb128(lfs3_t *lfs3, lfs3_data_t *data,
        uint32_t *word_) {
    // just call readleb128 here
    int err = lfs3_data_readleb128(lfs3, data, word_);
    if (err) {
        return err;
    }
    // little-leb128s should be limited to 28-bits
    if (*word_ > 0x0fffffff) {
        return LFS3_ERR_CORRUPT;
    }

    return 0;
}

static lfs3_scmp_t lfs3_data_cmp(lfs3_t *lfs3, const lfs3_data_t *data,
        const void *buffer, lfs3_size_t size) {
    // we shouldn't end up with holes here
    LFS3_ASSERT(!lfs3_data_ishole(data));
    // compare common prefix
    lfs3_size_t d = lfs3_min(size, lfs3_data_size(data));

    // on-disk?
    if (lfs3_data_ondisk(data)) {
        // validating data cksums?
        if (LFS3_IFDEF_CKDATACKSUMS(
                lfs3_m_isckdatacksums(lfs3->flags)
                    && lfs3_data_isbptr(data),
                false)) {
            #ifdef LFS3_CKDATACKSUMS
            int cmp = lfs3_bd_ckcmp(lfs3,
                    // note the 0 hint, we don't usually use any
                    // following data
                    data->u.disk.block, lfs3_data_off(data), 0,
                    lfs3_data_cksize(data), data->u.disk.cksum,
                    buffer, d,
                    lfs3_data_flags(data));
            if (cmp != LFS3_CMP_EQ) {
                return cmp;
            }
            #endif

        } else {
            int cmp = lfs3_bd_cmp(lfs3,
                    // note the 0 hint, we don't usually use any
                    // following data
                    data->u.disk.block, lfs3_data_off(data), 0,
                    buffer, d,
                    lfs3_data_flags(data));
            if (cmp != LFS3_CMP_EQ) {
                return cmp;
            }
        }

    // buffer?
    } else {
        int cmp = lfs3_memcmp(data->u.buffer + lfs3_data_off(data),
                buffer,
                d);
        if (cmp < 0) {
            return LFS3_CMP_LT;
        } else if (cmp > 0) {
            return LFS3_CMP_GT;
        }
    }

    // if data is equal, check for size mismatch
    if (lfs3_data_size(data) < size) {
        return LFS3_CMP_LT;
    } else if (lfs3_data_size(data) > size) {
        return LFS3_CMP_GT;
    } else {
        return LFS3_CMP_EQ;
    }
}

static lfs3_scmp_t lfs3_data_namecmp(lfs3_t *lfs3, const lfs3_data_t *data,
        lfs3_did_t did, const char *name, lfs3_size_t name_len) {
    // first compare the did
    lfs3_data_t data_ = *data;
    lfs3_did_t did_;
    int err = lfs3_data_readleb128(lfs3, &data_, &did_);
    if (err) {
        return err;
    }

    if (did_ < did) {
        return LFS3_CMP_LT;
    } else if (did_ > did) {
        return LFS3_CMP_GT;
    }

    // then compare the actual name
    return lfs3_data_cmp(lfs3, &data_, name, name_len);
}

#ifndef LFS3_RDONLY
static int lfs3_bd_progdata(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off,
        const lfs3_data_t *data, uint32_t flags,
        uint32_t *cksum) {
    // we shouldn't end up with holes here
    LFS3_ASSERT(!lfs3_data_ishole(data));

    // on-disk?
    if (lfs3_data_ondisk(data)) {
        // validating data cksums?
        if (LFS3_IFDEF_CKDATACKSUMS(
                lfs3_m_isckdatacksums(lfs3->flags)
                    && lfs3_data_isbptr(data),
                false)) {
            #ifdef LFS3_CKDATACKSUMS
            int err = lfs3_bd_ckcpy(lfs3, block, off,
                    data->u.disk.block,
                    lfs3_data_off(data),
                    lfs3_data_size(data),
                    lfs3_data_size(data),
                    lfs3_data_cksize(data), data->u.disk.cksum,
                    flags | lfs3_data_flags(data),
                    cksum);
            if (err) {
                return err;
            }
            #endif

        } else {
            int err = lfs3_bd_cpy(lfs3, block, off,
                    data->u.disk.block,
                    lfs3_data_off(data),
                    lfs3_data_size(data),
                    lfs3_data_size(data),
                    flags | lfs3_data_flags(data),
                    cksum);
            if (err) {
                return err;
            }
        }

    // buffer?
    } else {
        int err = lfs3_bd_prog(lfs3, block, off,
                data->u.buffer + lfs3_data_off(data),
                lfs3_data_size(data),
                flags,
                cksum);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif


// macros for le32/leb128/lleb128 encoding, these are useful for
// building rattrs

#ifndef LFS3_RDONLY
static inline lfs3_data_t lfs3_data_fromle32(uint32_t word,
        uint8_t buffer[static LFS3_LE32_DSIZE]) {
    lfs3_tole32(word, buffer);
    return LFS3_DATA_BUF(buffer, LFS3_LE32_DSIZE);
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_data_t lfs3_data_fromleb128(uint32_t word,
        uint8_t buffer[static LFS3_LEB128_DSIZE]) {
    // leb128s should not exceed 31-bits
    LFS3_ASSERT(word <= 0x7fffffff);

    lfs3_ssize_t d = lfs3_toleb128(word, buffer, LFS3_LEB128_DSIZE);
    if (d < 0) {
        LFS3_UNREACHABLE();
    }

    return LFS3_DATA_BUF(buffer, d);
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_data_t lfs3_data_fromlleb128(uint32_t word,
        uint8_t buffer[static LFS3_LLEB128_DSIZE]) {
    // little-leb128s should not exceed 28-bits
    LFS3_ASSERT(word <= 0x0fffffff);

    lfs3_ssize_t d = lfs3_toleb128(word, buffer, LFS3_LLEB128_DSIZE);
    if (d < 0) {
        LFS3_UNREACHABLE();
    }

    return LFS3_DATA_BUF(buffer, d);
}
#endif


/// Rattrs - lfs3_rattr_t stuff ///

// operations on rbyd attribute lists, rattrs have basically evolved
// into a full isa

// our core rbyd attribute type
//
//   wwll llff ffff ffff tttt tttt tttt tttt
//    ^'-.-''-----.----' :                 :
//    '--|--------|------:-----------------:-- compressed weight
//   ::  '--------|------:-----------------:-- total len - 1
//   ::           '------:-----------------:-- from encoder
//   ::     :          : rgmm kkkk +kkk kkkk
//   11 => w=-1        : ^^ ^ '-.' '---.---'
//   00 => w=0         : '|-|---|------|------ rm bit
//   01 => w=+1        :  '-|---|------|------ grow bit
//   10 => w=arg       :    '---|------|------ mask bits
//          :          :   ::   '------|------ tag suptype
//          ff cccc cccc   ::          '------ tag subtype
//          11 ffff ffcc   ::
//          '----.---'     00 => mask0  (---- ---- ----)
//             '-|-.---'   01 => mask2  (---- ---- --11)
//   from -------' |       10 => mask8  (---- 1111 1111)
//   count --------'       11 => mask12 (1111 1111 1111)
//
typedef uintptr_t lfs3_rattr_t;

// rattr from encoders
enum lfs3_from {
    LFS3_FROM_NIL       = 0x000, // -- ++++ ++++ (count unused)
    LFS3_FROM_LBUF      = 0x100, // -1 cccc cccc
    LFS3_FROM_NAME      = 0x200, // 1- ++++ ++++

    LFS3_FROM_BUF       = 0x300, // 11 ---- --++
    LFS3_FROM_GRAFT     = 0x304, // 11 ---- -1cc
    LFS3_FROM_DATA      = 0x308, // 11 ---- 1-cc

    LFS3_FROM_LE32      = 0x30c, // 11 ---- 11++
    LFS3_FROM_LEB128    = 0x310, // 11 ---1 --++
    LFS3_FROM_LLEB128   = 0x314, // 11 ---1 -1++

    LFS3_FROM_ECKSUM    = 0x318, // 11 ---1 1-++
    LFS3_FROM_BRANCH    = 0x31c, // 11 ---1 11++
    LFS3_FROM_BTREE     = 0x320, // 11 --1- --++
    LFS3_FROM_SHRUB     = 0x324, // 11 --1- -1++
    LFS3_FROM_MPTR      = 0x328, // 11 --1- 1-++
    LFS3_FROM_BPTR      = 0x32c, // 11 --1- 11++
    LFS3_FROM_COMPAT    = 0x330, // 11 --11 --++
    LFS3_FROM_GEOMETRY  = 0x334, // 11 --11 -1++
};

typedef uint16_t lfs3_from_t;

// initial rattr macros
#define LFS3_RATTR___(_tag, _weight, _arg_count, _from, _from_count) \
    (((lfs3_rattr_t)(_weight) << 30) \
        | ((lfs3_rattr_t)((((_weight) == -2) ? 1 : 0)+(_arg_count)) << 26) \
        | ((lfs3_rattr_t)(_from) << 16) \
        | ((lfs3_rattr_t)(_from_count) << 16) \
        | ((lfs3_rattr_t)(_tag) << 0))

#define LFS3_RATTR_5(_tag, _weight, _arg_count, _from, _from_count) \
    LFS3_RATTR___(_tag, _weight, _arg_count, _from, \
        (_from_count) - (((_from) >= 0x300) ? 1 : 0))

#define LFS3_RATTR_4(_tag, _weight, _arg_count, _from) \
    LFS3_RATTR___(_tag, _weight, _arg_count, _from, 0)

#define LFS3_RATTR_3(_tag, _weight, _arg_count) \
    LFS3_RATTR___(_tag, _weight, _arg_count, LFS3_FROM_NIL, 0)

#define LFS3_RATTR_N_(_0, _1, _2, _3, _4, _n, ...) _n
#define LFS3_RATTR_N(...) LFS3_RATTR_N_(__VA_ARGS__, 5, 4, 3, 2, 1)

#define LFS3_RATTR__(n) \
    LFS3_RATTR_##n

#define LFS3_RATTR_(n, ...) \
    LFS3_RATTR__(n)(__VA_ARGS__)

#define LFS3_RATTR(...) \
    LFS3_RATTR_(LFS3_RATTR_N(__VA_ARGS__), __VA_ARGS__)

// some rattr macros with special behavior
#define LFS3_RATTR_NOOP(_arg_count) \
    LFS3_RATTR_5(LFS3_tag_NOOP, 0, _arg_count, LFS3_FROM_NIL, 0)

// extended rattr macros
#define LFS3_RATTR_WEIGHT(_weight) \
    ((lfs3_rattr_t)(lfs3_srid_t){_weight})

#define LFS3_RATTR_ARG(_arg) \
    ((lfs3_rattr_t)(_arg))

// null rattr terminates rattr lists
#define LFS3_RATTR_NULL ((lfs3_rattr_t)0)

// rattr helpers
#ifndef LFS3_RDONLY
static inline lfs3_srid_t lfs3_rattr_weight_(lfs3_rattr_t rattr) {
    return (lfs3_srid_t)rattr >> 30;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_ssize_t lfs3_rattr_weightcount_(lfs3_rattr_t rattr) {
    return (lfs3_rattr_weight_(rattr) == -2) ? 1 : 0;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_size_t lfs3_rattr_len_(lfs3_rattr_t rattr) {
    return 1 + (0xf & (rattr >> 26));
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_ssize_t lfs3_rattr_argcount_(lfs3_rattr_t rattr) {
    return lfs3_rattr_len_(rattr) - lfs3_rattr_weightcount_(rattr) - 1;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_from_t lfs3_rattr_from_(lfs3_rattr_t rattr) {
    return 0x3ff & (rattr >> 16);
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_tag_t lfs3_rattr_tag_(lfs3_rattr_t rattr) {
    return 0xffff & rattr;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isinternal_(lfs3_rattr_t rattr) {
    return lfs3_tag_suptype(lfs3_rattr_tag_(rattr)) == LFS3_TAG_INTERNAL;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isrm_(lfs3_rattr_t rattr) {
    return lfs3_tag_isrm(lfs3_rattr_tag_(rattr));
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isgrow_(lfs3_rattr_t rattr) {
    return lfs3_tag_isgrow(lfs3_rattr_tag_(rattr));
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_srid_t lfs3_rattr_weight(const lfs3_rattr_t *rattr) {
    lfs3_srid_t weight = lfs3_rattr_weight_(rattr[0]);
    if (weight == -2) {
        return rattr[1];
    } else {
        return weight;
    }
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_size_t lfs3_rattr_len(const lfs3_rattr_t *rattr) {
    return lfs3_rattr_len_(rattr[0]);
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_ssize_t lfs3_rattr_argcount(const lfs3_rattr_t *rattr) {
    return lfs3_rattr_argcount_(rattr[0]);
}
#endif

#ifndef LFS3_RDONLY
static inline const lfs3_rattr_t *lfs3_rattr_args(const lfs3_rattr_t *rattr) {
    lfs3_srid_t weight = lfs3_rattr_weight_(rattr[0]);
    if (weight == -2) {
        return &rattr[2];
    } else {
        return &rattr[1];
    }
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_rattr_t lfs3_rattr_arg(const lfs3_rattr_t *rattr,
        lfs3_size_t i) {
    return lfs3_rattr_args(rattr)[i];
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_from_t lfs3_rattr_from(const lfs3_rattr_t *rattr) {
    return lfs3_rattr_from_(rattr[0]);
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_tag_t lfs3_rattr_tag(const lfs3_rattr_t *rattr) {
    return lfs3_rattr_tag_(rattr[0]);
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isinternal(const lfs3_rattr_t *rattr) {
    return lfs3_rattr_isinternal_(rattr[0]);
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isrm(const lfs3_rattr_t *rattr) {
    return lfs3_rattr_isrm_(rattr[0]);
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isgrow(const lfs3_rattr_t *rattr) {
    return lfs3_rattr_isgrow_(rattr[0]);
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isinsert(const lfs3_rattr_t *rattr) {
    return !lfs3_rattr_isgrow(rattr)
            && lfs3_rattr_weight(rattr) > 0;
}
#endif

#ifndef LFS3_RDONLY
static inline const lfs3_rattr_t *lfs3_rattr_next(const lfs3_rattr_t *rattr,
        lfs3_srid_t *rid) {
    // a len of zero here is probably a bug
    LFS3_ASSERT(lfs3_rattr_len(rattr) > 0);

    // adjust rid?
    if (rid) {
        lfs3_srid_t weight = lfs3_rattr_weight(rattr);
        if (!lfs3_rattr_isgrow(rattr) && weight > 0) {
            *rid = *rid + weight-1;
        } else {
            *rid = *rid + weight;
        }
    }

    // return next rattr
    return rattr + lfs3_rattr_len(rattr);
}
#endif

// from helpers
#ifndef LFS3_RDONLY
static inline lfs3_from_t lfs3_from_from8(lfs3_from_t from) {
    return 0x300 & from;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_from_t lfs3_from_from2(lfs3_from_t from) {
    return 0x3fc & from;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_size_t lfs3_from_fromcount8(lfs3_from_t from) {
    return 0x0ff & from;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_size_t lfs3_from_fromcount2(lfs3_from_t from) {
    return (0x003 & from)+1;
}
#endif


// operations on custom attribute lists
//
// a slightly different struct because it's user facing

static inline lfs3_ssize_t lfs3_attr_size(const struct lfs3_attr *attr) {
    // we default to the buffer_size if a mutable size is not provided
    if (attr->size) {
        return *attr->size;
    } else {
        return attr->buffer_size;
    }
}

static inline bool lfs3_attr_isnoattr(const struct lfs3_attr *attr) {
    return lfs3_attr_size(attr) == LFS3_ERR_NOATTR;
}

static lfs3_scmp_t lfs3_attr_cmp(lfs3_t *lfs3, const struct lfs3_attr *attr,
        const lfs3_data_t *data) {
    // note data=NULL => NOATTR
    if (!data) {
        return (lfs3_attr_isnoattr(attr)) ? LFS3_CMP_EQ : LFS3_CMP_GT;
    } else {
        if (lfs3_attr_isnoattr(attr)) {
            return LFS3_CMP_LT;
        } else {
            return lfs3_data_cmp(lfs3, data,
                    attr->buffer,
                    lfs3_attr_size(attr));
        }
    }
}



/// Block allocator definitions ///

// the block allocator is humorously cyclic in its definition
//
// to avoid the mess of redeclaring flags and things, just declare
// everything we need here

// block allocator flags
#define LFS3_ALLOC_ERASE    0x000000001 // Please erase the block
#define LFS3_ALLOC_CLAIM    0x000000002 // Claim erased state

static inline bool lfs3_alloc_iserase(uint32_t flags) {
    return flags & LFS3_ALLOC_ERASE;
}

static inline bool lfs3_alloc_isclaim(uint32_t flags) {
    return flags & LFS3_ALLOC_CLAIM;
}

// checkpoint the allocator
//
// operations that need to alloc should call this when all in-use blocks
// are tracked, either by the filesystem or an opened mdir
//
// blocks are allocated at most once, and never reallocated, between
// checkpoints
#if !defined(LFS3_RDONLY)
static inline int lfs3_alloc_ckpoint(lfs3_t *lfs3);
#endif

// discard any lookahead state, this is necessary if block_count changes
#ifndef LFS3_RDONLY
static inline void lfs3_alloc_discard(lfs3_t *lfs3);
#endif

// allocate a block
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_alloc(lfs3_t *lfs3, uint32_t flags);
#endif

// allocate a block and sync gbmap if necessary
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_allocwith(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        uint32_t flags);
#endif



/// Block pointer things ///

#define LFS3_BPTR_ONDISK LFS3_DATA_ONDISK
#define LFS3_BPTR_ISHOLE LFS3_DATA_ISHOLE
#define LFS3_BPTR_ISBPTR LFS3_DATA_ISBPTR

#ifndef LFS3_RDONLY
#define LFS3_BPTR_ISERASED 0x80000000
#endif

static inline void lfs3_bptr_discard(lfs3_bptr_t *bptr) {
    bptr->d = LFS3_DATA_NULL();
}

#ifndef LFS3_RDONLY
static inline void lfs3_bptr_claim(lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMS
    bptr->d.u.disk.cksize &= ~LFS3_BPTR_ISERASED;
    #else
    bptr->cksize &= ~LFS3_BPTR_ISERASED;
    #endif
}
#endif

static inline bool lfs3_bptr_ondisk(const lfs3_bptr_t *bptr) {
    return lfs3_data_ondisk(&bptr->d);
}

static inline bool lfs3_bptr_isbuf(const lfs3_bptr_t *bptr) {
    return lfs3_data_isbuf(&bptr->d);
}

static inline bool lfs3_bptr_ishole(const lfs3_bptr_t *bptr) {
    return lfs3_data_ishole(&bptr->d);
}

static inline bool lfs3_bptr_isfragment(const lfs3_bptr_t *bptr) {
    return lfs3_data_isfragment(&bptr->d);
}

static inline bool lfs3_bptr_isbptr(const lfs3_bptr_t *bptr) {
    return lfs3_data_isbptr(&bptr->d);
}

static inline lfs3_block_t lfs3_bptr_block(const lfs3_bptr_t *bptr) {
    return bptr->d.u.disk.block;
}

static inline lfs3_size_t lfs3_bptr_off(const lfs3_bptr_t *bptr) {
    return lfs3_data_off(&bptr->d);
}

static inline lfs3_size_t lfs3_bptr_size(const lfs3_bptr_t *bptr) {
    // this is just a hint that size should be <= lfs3_size_t
    // i.e., not a hole
    return lfs3_data_size(&bptr->d);
}

static inline lfs3_size_t lfs3_bptr_estimate(const lfs3_bptr_t *bptr) {
    if (lfs3_bptr_ishole(bptr)) {
        return 0;
    } else if (lfs3_bptr_isfragment(bptr)) {
        return lfs3_bptr_size(bptr);
    } else {
        return LFS3_BPTR_DSIZE;
    }
}

// checked reads adds ck info to lfs3_data_t that we don't want to
// unnecessarily duplicate, this makes accessing ck info annoyingly
// messy...
#ifndef LFS3_RDONLY
static inline bool lfs3_bptr_iserased(const lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMS
    return bptr->d.u.disk.cksize & LFS3_BPTR_ISERASED;
    #else
    return bptr->cksize & LFS3_BPTR_ISERASED;
    #endif
}
#endif

static inline lfs3_size_t lfs3_bptr_cksize(const lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMS
    return LFS3_IFDEF_RDONLY(
            bptr->d.u.disk.cksize,
            bptr->d.u.disk.cksize & ~LFS3_BPTR_ISERASED);
    #else
    return LFS3_IFDEF_RDONLY(
            bptr->cksize,
            bptr->cksize & ~LFS3_BPTR_ISERASED);
    #endif
}

static inline uint32_t lfs3_bptr_cksum(const lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMS
    return bptr->d.u.disk.cksum;
    #else
    return bptr->cksum;
    #endif
}

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline lfs3_evict_t *lfs3_evict_evictionbptr(lfs3_t *lfs3,
        const lfs3_bptr_t *bptr) {
    return lfs3_evict_eviction(lfs3, lfs3_bptr_block(bptr));
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_evict_needsevictionbptr(const lfs3_t *lfs3,
        const lfs3_bptr_t *bptr) {
    return lfs3_evict_evictionbptr((lfs3_t*)lfs3, bptr);
}
#endif

// slice a bptr in-place
static inline void lfs3_bptr_slice(lfs3_bptr_t *bptr,
        lfs3_ssize_t off, lfs3_ssize_t size) {
    lfs3_data_slice(&bptr->d, off, size);
}

static inline void lfs3_bptr_fromslice(
        lfs3_bptr_t *bptr_, const lfs3_bptr_t *bptr,
        lfs3_ssize_t off, lfs3_ssize_t size) {
    *bptr_ = *bptr;
    lfs3_bptr_slice(bptr_, off, size);
}

// bptr on-disk encoding
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_frombptr(const lfs3_bptr_t *bptr,
        uint8_t buffer[static LFS3_BPTR_DSIZE]) {
    // we should actually be a bptr
    LFS3_ASSERT(lfs3_bptr_isbptr(bptr));
    // size should not exceed 28-bits
    LFS3_ASSERT(lfs3_bptr_size(bptr) <= 0x0fffffff);
    // block should not exceed 31-bits
    LFS3_ASSERT(lfs3_bptr_block(bptr) <= 0x7fffffff);
    // off should not exceed 28-bits
    LFS3_ASSERT(lfs3_bptr_off(bptr) <= 0x0fffffff);
    // cksize should not exceed 28-bits
    LFS3_ASSERT(lfs3_bptr_cksize(bptr) <= 0x0fffffff);
    lfs3_ssize_t d = 0;

    // write the block, offset, size
    lfs3_ssize_t d_ = lfs3_toleb128(lfs3_bptr_size(bptr), &buffer[d], 4);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    d_ = lfs3_toleb128(lfs3_bptr_block(bptr), &buffer[d], 5);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    d_ = lfs3_toleb128(lfs3_bptr_off(bptr), &buffer[d], 4);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    // write the cksize, cksum
    d_ = lfs3_toleb128(lfs3_bptr_cksize(bptr), &buffer[d], 4);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    lfs3_tole32(lfs3_bptr_cksum(bptr), &buffer[d]);
    d += 4;

    return LFS3_DATA_BUF(buffer, d);
}
#endif

static int lfs3_data_readbptr(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_bptr_t *bptr) {
    // read the block, offset, size
    int err = lfs3_data_readlleb128(lfs3, data, &bptr->d.weight);
    if (err) {
        return err;
    }

    err = lfs3_data_readleb128(lfs3, data, &bptr->d.u.disk.block);
    if (err) {
        return err;
    }

    err = lfs3_data_readlleb128(lfs3, data, &bptr->d.off);
    if (err) {
        return err;
    }

    // read the cksize, cksum
    err = lfs3_data_readlleb128(lfs3, data,
            LFS3_IFDEF_CKDATACKSUMS(
                &bptr->d.u.disk.cksize,
                &bptr->cksize));
    if (err) {
        return err;
    }

    err = lfs3_data_readle32(lfs3, data,
            LFS3_IFDEF_CKDATACKSUMS(
                &bptr->d.u.disk.cksum,
                &bptr->cksum));
    if (err) {
        return err;
    }

    // mark as on-disk + cksum
    bptr->d.off |= LFS3_DATA_ONDISK | LFS3_DATA_ISBPTR;
    return 0;
}

// needed in lfs3_data_fetchbptr
#ifdef LFS3_CKFETCHES
static inline bool lfs3_m_isckfetches(uint32_t flags);
#endif
static int lfs3_bptr_ck(lfs3_t *lfs3, const lfs3_bptr_t *bptr);

static int lfs3_data_fetchbptr(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_bptr_t *bptr) {
    // decode bptr and fetch
    int err = lfs3_data_readbptr(lfs3, data,
            bptr);
    if (err) {
        return err;
    }

    // checking fetches?
    #ifdef LFS3_CKFETCHES
    if (lfs3_m_isckfetches(lfs3->flags)) {
        err = lfs3_bptr_ck(lfs3, bptr);
        if (err) {
            return err;
        }
    }
    #endif

    return 0;
}

// allocate a bptr
#ifndef LFS3_RDONLY
static int lfs3_bptr_alloc(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        lfs3_bptr_t *bptr) {
    lfs3_sblock_t block = lfs3_allocwith(lfs3, mdir,
            LFS3_ALLOC_ERASE | LFS3_ALLOC_CLAIM);
    if (block < 0) {
        return block;
    }

    bptr->d.weight = 0;
    bptr->d.u.disk.block = block;
    bptr->d.off = LFS3_BPTR_ONDISK | LFS3_BPTR_ISBPTR | 0;
    // mark as erased
    LFS3_IFDEF_CKDATACKSUMS(
            bptr->d.u.disk.cksize,
            bptr->cksize) = LFS3_BPTR_ISERASED | 0;
    LFS3_IFDEF_CKDATACKSUMS(
            bptr->d.u.disk.cksum,
            bptr->cksum) = 0;
    return 0;
}
#endif

// check the contents of a bptr
static int lfs3_bptr_ck(lfs3_t *lfs3, const lfs3_bptr_t *bptr) {
    uint32_t cksum = 0;
    int err = lfs3_bd_cksum(lfs3,
            lfs3_bptr_block(bptr), 0, 0,
            lfs3_bptr_cksize(bptr), LFS3_BD_DATA,
            &cksum);
    if (err) {
        return err;
    }

    // test that our cksum matches what's expected
    if (cksum != lfs3_bptr_cksum(bptr)) {
        LFS3_ERROR("Found bptr cksum mismatch "
                    "0x%"PRIx32".%"PRIx32" %"PRId32", "
                    "cksum %08"PRIx32" (!= %08"PRIx32")",
                lfs3_bptr_block(bptr), 0,
                lfs3_bptr_cksize(bptr),
                cksum, lfs3_bptr_cksum(bptr));
        return LFS3_ERR_CORRUPT;
    }

    return 0;
}

// evict a bptr, copying it into a new a block
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static int lfs3_bptr_evict(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        lfs3_bptr_t *bptr) {
relocate:;
    // allocate a new block
    //
    // this does potentially commit to the mdir to claim the block,
    // but that should be ok as long as our evict queue is set up
    // correctly...
    lfs3_sblock_t block = lfs3_allocwith(lfs3, mdir,
            LFS3_ALLOC_ERASE | LFS3_ALLOC_CLAIM);
    if (block < 0) {
        return block;
    }

    // Note we have a couple conflicting goals here.
    //
    // Ideally, we'd just copy the referenced data slice, leverage new
    // erased-state, etc. But, thanks to dags, we don't know how many
    // bptrs reference this block, and which bptr has the largest
    // cksize. We could try to find the largest cksize first, but that
    // would add complexity and risk O(FS^2) performance.
    //
    // So the best we can do is copy the entire block, and then patch
    // bptrs to reference the relevant slices.
    //
    // Fortunately, eviction should be a rare event, and not worth
    // optimizing/complicating for.
    //
    // As a plus, this avoids all the possible alignment issues:
    // crystallization block alignment, cross-driver erased prog
    // alignment, etc.

    // copy the entire block
    //
    // we don't really have a cksum we can use to validate this, but we
    // still have CKPROGS, and future reads will have the relevant
    // cksize+cksum, so we shouldn't be violating CKDATACKSUMS even
    // though the block changed
    int err = lfs3_bd_cpy(lfs3, block, 0,
            lfs3_bptr_block(bptr), 0, -1, lfs3->cfg->block_size, 0,
            NULL);
    if (err) {
        // bad prog? try another block
        if (err == LFS3_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    // finalize our write
    err = lfs3_bd_flush(lfs3, 0, NULL);
    if (err) {
        // bad prog? try another block
        if (err == LFS3_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    // update bptr
    bptr->d.u.disk.block = block;
    // clear the erased flag
    LFS3_IFDEF_CKDATACKSUMS(
            bptr->d.u.disk.cksize,
            bptr->cksize) &= ~LFS3_BPTR_ISERASED;
    return 0;
}
#endif




/// Erased-state checksum stuff ///

#ifndef LFS3_RDONLY
static inline bool lfs3_ecksum_isecksum(const lfs3_ecksum_t *ecksum) {
    return ecksum->cksize != -1;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_ecksum(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_off_t off, lfs3_size_t hint, uint32_t flags,
        lfs3_ecksum_t *ecksum) {
    // keep track of prog size
    ecksum->cksize = lfs3->cfg->prog_size;
    // checksum erased state
    ecksum->cksum = 0;
    return lfs3_bd_cksum(lfs3,
            block, off, hint,
            lfs3->cfg->prog_size, flags,
            &ecksum->cksum);
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_ecksum_ck(lfs3_t *lfs3, const lfs3_ecksum_t *ecksum,
        lfs3_block_t block, lfs3_off_t off) {
    // you shouldn't try to check a not-ecksum, that doesn't make sense
    LFS3_ASSERT(lfs3_ecksum_isecksum(ecksum));
    uint32_t cksum_ = 0;
    int err = lfs3_bd_cksum(lfs3,
            block, off, 0,
            ecksum->cksize, LFS3_BD_RELAX,
            &cksum_);
    if (err) {
        return err;
    }

    return (cksum_ != ecksum->cksum) ? LFS3_ERR_CORRUPT : 0;
}
#endif

#ifndef LFS3_RDONLY
static inline int lfs3_ecksum_cmp(
        const lfs3_ecksum_t *a,
        const lfs3_ecksum_t *b) {
    // cmp cksize first, note this cmps both not-ecksums and
    // not-not-ecksum cksizes
    if (a->cksize != b->cksize) {
        return a->cksize - b->cksize;
    // only cmp cksum if not-not-ecksum
    } else if (lfs3_ecksum_isecksum(a)) {
        return a->cksum - b->cksum;
    } else {
        return 0;
    }
}
#endif

// erased-state checksum on-disk encoding
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_fromecksum(const lfs3_ecksum_t *ecksum,
        uint8_t buffer[static LFS3_ECKSUM_DSIZE]) {
    // you shouldn't try to encode a not-ecksum, that doesn't make sense
    LFS3_ASSERT(lfs3_ecksum_isecksum(ecksum));
    // cksize should not exceed 28-bits
    LFS3_ASSERT((lfs3_size_t)ecksum->cksize <= 0x0fffffff);

    lfs3_ssize_t d = 0;
    lfs3_ssize_t d_ = lfs3_toleb128(ecksum->cksize, &buffer[d], 4);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    lfs3_tole32(ecksum->cksum, &buffer[d]);
    d += 4;

    return LFS3_DATA_BUF(buffer, d);
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_data_readecksum(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_ecksum_t *ecksum) {
    int err = lfs3_data_readlleb128(lfs3, data, (lfs3_size_t*)&ecksum->cksize);
    if (err) {
        return err;
    }

    err = lfs3_data_readle32(lfs3, data, &ecksum->cksum);
    if (err) {
        return err;
    }

    return 0;
}
#endif

// we sometimes need to read ecksums in weird situations
#ifndef LFS3_RDONLY
static lfs3_ssize_t lfs3_bd_readecksum(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint, uint32_t flags,
        lfs3_ecksum_t *ecksum_) {
    // just read into buffer and pass to lfs3_data_readecksum
    uint8_t ecksum_buf[LFS3_ECKSUM_DSIZE];
    lfs3_size_t ecksum_dsize = lfs3_min(
            LFS3_ECKSUM_DSIZE,
            lfs3->cfg->block_size-off);
    int err = lfs3_bd_read(lfs3, block, off, hint,
            ecksum_buf, ecksum_dsize, flags);
    if (err) {
        return err;
    }

    lfs3_data_t ecksum_data = LFS3_DATA_BUF(ecksum_buf, ecksum_dsize);
    err = lfs3_data_readecksum(lfs3, &ecksum_data,
            ecksum_);
    if (err) {
        return err;
    }

    return ecksum_dsize - lfs3_data_size(&ecksum_data);
}

#endif



/// Red-black-yellow Dhara tree operations ///

#define LFS3_RBYD_ISSHRUB 0x80000000
#define LFS3_RBYD_PERTURB 0x80000000

// helper functions
static void lfs3_rbyd_init(lfs3_rbyd_t *rbyd, lfs3_block_t block) {
    rbyd->blocks[0] = block;
    rbyd->trunk = 0;
    rbyd->weight = 0;
    #ifndef LFS3_RDONLY
    rbyd->eoff = 0;
    rbyd->cksum = 0;
    #endif
}

#ifndef LFS3_RDONLY
static inline void lfs3_rbyd_claim(lfs3_rbyd_t *rbyd) {
    // mark as needing fetch
    rbyd->eoff = 0;
}
#endif

static inline bool lfs3_rbyd_isshrub(const lfs3_rbyd_t *rbyd) {
    return rbyd->trunk & LFS3_RBYD_ISSHRUB;
}

static inline lfs3_size_t lfs3_rbyd_trunk(const lfs3_rbyd_t *rbyd) {
    return rbyd->trunk & ~LFS3_RBYD_ISSHRUB;
}

#ifndef LFS3_RDONLY
static inline bool lfs3_rbyd_isfetched(const lfs3_rbyd_t *rbyd) {
    return !lfs3_rbyd_trunk(rbyd) || rbyd->eoff;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rbyd_isperturb(const lfs3_rbyd_t *rbyd) {
    return rbyd->eoff & LFS3_RBYD_PERTURB;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_size_t lfs3_rbyd_eoff(const lfs3_rbyd_t *rbyd) {
    return rbyd->eoff & ~LFS3_RBYD_PERTURB;
}
#endif

static inline int lfs3_rbyd_cmp(
        const lfs3_rbyd_t *a,
        const lfs3_rbyd_t *b) {
    if (a->blocks[0] != b->blocks[0]) {
        return a->blocks[0] - b->blocks[0];
    } else {
        return a->trunk - b->trunk;
    }
}

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline lfs3_evict_t *lfs3_evict_evictionrbyd(lfs3_t *lfs3,
        const lfs3_rbyd_t *rbyd) {
    return lfs3_evict_eviction(lfs3, rbyd->blocks[0]);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_evict_needsevictionrbyd(const lfs3_t *lfs3,
        const lfs3_rbyd_t *rbyd) {
    return lfs3_evict_evictionrbyd((lfs3_t*)lfs3, rbyd);
}
#endif


// allocate an rbyd block
#ifndef LFS3_RDONLY
static int lfs3_rbyd_alloc(lfs3_t *lfs3, lfs3_rbyd_t *rbyd) {
    lfs3_sblock_t block = lfs3_alloc(lfs3, LFS3_ALLOC_ERASE);
    if (block < 0) {
        return block;
    }

    lfs3_rbyd_init(rbyd, block);
    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_ckecksum(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        const lfs3_ecksum_t *ecksum) {
    // check that the ecksum looks right
    if (lfs3_rbyd_eoff(rbyd) + ecksum->cksize >= lfs3->cfg->block_size
            || lfs3_rbyd_eoff(rbyd) % lfs3->cfg->prog_size != 0) {
        return LFS3_ERR_CORRUPT;
    }

    // the next valid bit must _not_ match, or a commit was attempted,
    // this should hopefully stay in our cache
    uint8_t e;
    int err = lfs3_bd_read(lfs3,
            rbyd->blocks[0], lfs3_rbyd_eoff(rbyd), ecksum->cksize,
            &e, 1, 0);
    if (err) {
        return err;
    }

    if (((e >> 7)^lfs3_rbyd_isperturb(rbyd)) == lfs3_parity(rbyd->cksum)) {
        return LFS3_ERR_CORRUPT;
    }

    // check that erased-state matches our checksum, if this fails
    // most likely a write was interrupted
    return lfs3_ecksum_ck(lfs3, ecksum,
            rbyd->blocks[0], lfs3_rbyd_eoff(rbyd));
}
#endif

// rbyd fetch flags
#define LFS3_RBYD_RELAX         0x00000001 // Don't evict corrupt data
#define LFS3_RBYD_QUICKFETCH    0x00000004 // Only fetch the trunk

static inline bool lfs3_rbyd_isrelax(uint32_t flags) {
    return flags & LFS3_RBYD_RELAX;
}

static inline bool lfs3_rbyd_isquickfetch(uint32_t flags) {
    return flags & LFS3_RBYD_QUICKFETCH;
}

// optional height calculation for debugging rbyd balance
typedef struct lfs3_rheight {
    lfs3_size_t height;
    lfs3_size_t bheight;
} lfs3_rheight_t;

// needed in lfs3_rbyd_fetch_ if debugging rbyd balance
static lfs3_stag_t lfs3_rbyd_lookupnext_(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_srid_t *rid_, lfs3_rid_t *weight_, lfs3_data_t *data_,
        lfs3_rheight_t *rheight_);

// fetch an rbyd
static int lfs3_rbyd_fetch_(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_block_t block, lfs3_ssize_t trunk, uint32_t flags,
        uint32_t *gcksumdelta_) {
    // set up some initial state
    rbyd->blocks[0] = block;
    rbyd->trunk = 0;
    rbyd->weight = 0;
    #ifndef LFS3_RDONLY
    rbyd->eoff = 0;
    #endif

    // if we're quick fetching, we can start from the trunk,
    // otherwise we start from 0 and try to find the trunk
    lfs3_size_t off_ = (lfs3_rbyd_isquickfetch(flags))
            ? (lfs3_size_t)trunk
            : sizeof(uint32_t);

    // keep track of last commit off and perturb bit
    lfs3_size_t eoff = 0;
    bool perturb = false;
    // and damaged/condemned bits, if repairing
    #if !defined(LF33_RDONLY) && defined(LFS3_REPAIR)
    uint8_t repair_flags = lfs3->repair_flags;
    #endif

    // checksum the revision count to get the cksum started
    uint32_t cksum_ = 0;
    if (!lfs3_rbyd_isquickfetch(flags)) {
        int err = lfs3_bd_cksum(lfs3, block, 0, -1, sizeof(uint32_t), flags,
                &cksum_);
        if (err) {
            return err;
        }
    }

    // temporary state until we validate a cksum
    uint32_t cksum__ = cksum_;
    lfs3_size_t trunk_ = 0;
    lfs3_size_t trunk__ = 0;
    lfs3_rid_t weight_ = 0;
    lfs3_rid_t weight__ = 0;

    // assume unerased until proven otherwise
    #ifndef LFS3_RDONLY
    lfs3_ecksum_t ecksum = {.cksize=-1};
    lfs3_ecksum_t ecksum_ = {.cksize=-1};
    #endif

    // also find gcksumdelta, though this is only used by mdirs
    uint32_t gcksumdelta = 0;

    // scan tags, checking valid bits, cksums, etc
    while (off_ < lfs3->cfg->block_size
            && eoff <= (lfs3_size_t)trunk) {
        // read next tag
        lfs3_tag_t tag;
        lfs3_rid_t weight;
        lfs3_size_t size;
        lfs3_ssize_t d = lfs3_bd_readtag(lfs3, block, off_, -1, flags,
                &tag, &weight, &size,
                (lfs3_rbyd_isquickfetch(flags)) ? NULL : &cksum__);
        if (d < 0 && LFS3_IFDEF_EVICT(
                d != LFS3_ERR_DAMAGED
                    && d != LFS3_ERR_CONDEMNED,
                true)) {
            if (d == LFS3_ERR_CORRUPT) {
                break;
            }
            return d;
        }
        lfs3_size_t off__ = off_ + d;

        // readtag should already check we're in-bounds
        LFS3_ASSERT(lfs3_tag_isalt(tag)
                || off__ + size <= lfs3->cfg->block_size);

        // take care of cksum
        if (!lfs3_tag_isalt(tag)) {
            // not an end-of-commit cksum
            if (lfs3_tag_suptype(tag) != LFS3_TAG_CKSUM) {
                if (!lfs3_rbyd_isquickfetch(flags)) {
                    // cksum the entry, hopefully leaving it in the cache
                    int err = lfs3_bd_cksum(lfs3, block, off__, -1,
                            size, flags,
                            &cksum__);
                    if (err) {
                        if (err == LFS3_ERR_CORRUPT) {
                            break;
                        }
                        return err;
                    }
                }

                // found an ecksum? save for later
                if (LFS3_IFDEF_RDONLY(
                        false,
                        tag == LFS3_TAG_ECKSUM)) {
                    #ifndef LFS3_RDONLY
                    d = lfs3_bd_readecksum(lfs3, block, off__, -1, flags,
                            &ecksum_);
                    if (d < 0) {
                        if (d == LFS3_ERR_CORRUPT) {
                            break;
                        }
                        return d;
                    }
                    #endif

                // found gcksumdelta? save for later
                } else if (tag == LFS3_TAG_GCKSUMDELTA) {
                    int err = lfs3_bd_readle32(lfs3, block, off__, -1, flags,
                            &gcksumdelta);
                    if (err) {
                        if (err == LFS3_ERR_CORRUPT) {
                            break;
                        }
                        return err;
                    }
                }

            // is an end-of-commit cksum
            } else {
                // truncated checksum?
                if (size < sizeof(uint32_t)) {
                    break;
                }

                // check phase
                if (lfs3_tag_phase(tag) != (block & 0x3)) {
                    // uh oh, phase doesn't match, mounted incorrectly?
                    break;
                }

                // check checksum, unless we're recklessly quick fetching
                if (!lfs3_rbyd_isquickfetch(flags)) {
                    uint32_t cksum___;
                    int err = lfs3_bd_readle32(lfs3, block, off__, -1, flags,
                            &cksum___);
                    if (err) {
                        if (err == LFS3_ERR_CORRUPT) {
                            break;
                        }
                        return err;
                    }

                    if (cksum__ != cksum___) {
                        // uh oh, checksums don't match
                        break;
                    }
                }

                // save what we've found so far
                eoff = off__ + size;
                rbyd->trunk = trunk_;
                rbyd->weight = weight_;
                if (!lfs3_rbyd_isquickfetch(flags)) {
                    rbyd->cksum = cksum_;
                }
                if (gcksumdelta_) {
                    *gcksumdelta_ = gcksumdelta;
                }
                gcksumdelta = 0;

                // update perturb bit
                perturb = lfs3_tag_perturb(tag);

                // update damaged/condemned bits
                #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
                repair_flags |= lfs3->repair_flags;
                #endif

                #ifndef LFS3_RDONLY
                rbyd->eoff
                        = ((lfs3_size_t)perturb << (8*sizeof(lfs3_size_t)-1))
                        | eoff;
                ecksum = ecksum_;
                ecksum_.cksize = -1;
                #endif

                // revert to canonical checksum and perturb if necessary
                cksum__ = cksum_ ^ ((perturb) ? LFS3_CRC32C_ODDZERO : 0);
            }
        }

        // found a trunk?
        if (lfs3_tag_istrunk(tag)) {
            if (off_ <= (lfs3_size_t)trunk || trunk__) {
                // start of trunk?
                if (!trunk__) {
                    // keep track of trunk's entry point
                    trunk__ = off_;
                    // reset weight
                    weight__ = 0;
                }

                // derive weight of the tree from alt pointers
                //
                // NOTE we can't check for overflow/underflow here because we
                // may be overeagerly parsing an invalid commit, it's ok for
                // this to overflow/underflow as long as we throw it out later
                // on a bad cksum
                weight__ += weight;

                // end of trunk?
                if (!lfs3_tag_isalt(tag)) {
                    // update trunk and weight, unless we are a shrub trunk,
                    // this prevents fetching shrub trunks, but why would
                    // you want to fetch a shrub trunk?
                    if (!lfs3_tag_isshrub(tag)) {
                        trunk_ = trunk__;
                        weight_ = weight__;
                    }
                    trunk__ = 0;
                }
            }

            // update canonical checksum, xoring out any perturb
            // state, we don't want erased-state affecting our
            // canonical checksum
            cksum_ = cksum__ ^ ((perturb) ? LFS3_CRC32C_ODDZERO : 0);
        }

        // skip data
        if (!lfs3_tag_isalt(tag)) {
            off__ += size;
        }

        off_ = off__;
    }

    // no valid commits?
    if (!lfs3_rbyd_trunk(rbyd)) {
        return LFS3_ERR_CORRUPT;
    }

    // did we end on a valid commit? we may have erased-state
    #ifndef LFS3_RDONLY
    bool erased = false;
    if (lfs3_ecksum_isecksum(&ecksum)) {
        // check the erased-state checksum
        int err = lfs3_rbyd_ckecksum(lfs3, rbyd, &ecksum);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }

        // found valid erased-state?
        erased = (err != LFS3_ERR_CORRUPT);
    }

    // used eoff=-1 to indicate when there is no erased-state
    if (!erased) {
        rbyd->eoff = -1;
    }
    #endif

    // revert damaged/condemned flags to last valid commit
    #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
    lfs3->repair_flags = repair_flags;
    #endif

    #ifdef LFS3_DBGRBYDFETCHES
    if (lfs3_rbyd_isquickfetch(flags)) {
        LFS3_DEBUG("Quickfetched rbyd 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                    "eoff %"PRId32", cksum %"PRIx32,
                rbyd->blocks[0], lfs3_rbyd_trunk(rbyd),
                rbyd->weight,
                LFS3_IFDEF_RDONLY(
                    -1,
                    (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size)
                        ? -1
                        : (lfs3_ssize_t)lfs3_rbyd_eoff(rbyd)),
                rbyd->cksum);
    } else {
        LFS3_DEBUG("Fetched rbyd 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                    "eoff %"PRId32", cksum %"PRIx32,
                rbyd->blocks[0], lfs3_rbyd_trunk(rbyd),
                rbyd->weight,
                LFS3_IFDEF_RDONLY(
                    -1,
                    (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size)
                        ? -1
                        : (lfs3_ssize_t)lfs3_rbyd_eoff(rbyd)),
                rbyd->cksum);
    }
    #endif

    // debugging rbyd balance? check that all branches in the rbyd have
    // the same height
    #ifdef LFS3_DBGRBYDBALANCE
    lfs3_srid_t rid = -1;
    lfs3_stag_t tag = 0;
    lfs3_size_t min_height = -1;
    lfs3_size_t max_height = 0;
    lfs3_size_t min_bheight = -1;
    lfs3_size_t max_bheight = 0;
    while (true) {
        lfs3_rheight_t rheight;
        tag = lfs3_rbyd_lookupnext_(lfs3, rbyd,
                rid, tag+1,
                &rid, NULL, NULL,
                &rheight);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }

        // find the min/max height and bheight
        min_height = lfs3_min(min_height, rheight.height);
        max_height = lfs3_max(max_height, rheight.height);
        min_bheight = lfs3_min(min_bheight, rheight.bheight);
        max_bheight = lfs3_max(max_bheight, rheight.bheight);
    }
    min_height = (min_height == (lfs3_size_t)-1) ? 0 : min_height;
    min_bheight = (min_bheight == (lfs3_size_t)-1) ? 0 : min_bheight;
    LFS3_DEBUG("Fetched rbyd 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "height %"PRId32"-%"PRId32", "
                "bheight %"PRId32"-%"PRId32,
            rbyd->blocks[0], lfs3_rbyd_trunk(rbyd),
            rbyd->weight,
            min_height, max_height,
            min_bheight, max_bheight);
    // all branches in the rbyd should have the same bheight
    LFS3_ASSERT(max_bheight == min_bheight);
    // this limits alt height to no worse than 2*bheight+2 (2*bheight+1
    // for normal appends, 2*bheight+2 with range removals)
    LFS3_ASSERT(max_height <= 2*min_height+2);
    #endif

    return 0;
}

static int lfs3_rbyd_fetch(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_block_t block, lfs3_size_t trunk) {
    // why would you try to fetch a shrub?
    LFS3_ASSERT(trunk == (lfs3_size_t)-1
            || !(trunk & LFS3_RBYD_ISSHRUB));

    return lfs3_rbyd_fetch_(lfs3, rbyd, block, trunk, 0,
            NULL);
}

// a more reckless fetch when checksum is known
//
// this just finds the eoff/perturb/ecksum for the current trunk to
// enable reckless commits
static int lfs3_rbyd_quickfetch(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_block_t block, lfs3_size_t trunk,
        uint32_t cksum) {
    // quickfetch requires a known trunk
    LFS3_ASSERT(trunk != 0);
    // why would you try to fetch a shrub?
    LFS3_ASSERT(!(trunk & LFS3_RBYD_ISSHRUB));

    // the only thing quick fetch can't figure out is the checksum
    rbyd->cksum = cksum;

    int err = lfs3_rbyd_fetch_(lfs3, rbyd, block, trunk, LFS3_RBYD_QUICKFETCH,
            NULL);
    if (err) {
        return err;
    }

    // quick fetch should leave the cksum unaffected
    LFS3_ASSERT(rbyd->cksum == cksum);
    return 0;
}

// a more aggressive fetch when checksum is known
static int lfs3_rbyd_ckfetch(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_block_t block, lfs3_size_t trunk,
        uint32_t cksum) {
    // ckfetch without a known trunk is probably an error
    LFS3_ASSERT(trunk != 0);
    // why would you try to fetch a shrub?
    LFS3_ASSERT(!(trunk & LFS3_RBYD_ISSHRUB));

    int err = lfs3_rbyd_fetch(lfs3, rbyd, block, trunk);
    if (err) {
        if (err == LFS3_ERR_CORRUPT) {
            LFS3_ERROR("Found corrupted rbyd 0x%"PRIx32".%"PRIx32", "
                        "cksum %08"PRIx32,
                    block, trunk, cksum);
        }
        return err;
    }

    // test that our cksum matches what's expected
    //
    // it should be noted that this is very unlikely to happen without the
    // above fetch failing, since that would require the rbyd to have the
    // same trunk and pass its internal cksum
    if (rbyd->cksum != cksum) {
        LFS3_ERROR("Found rbyd cksum mismatch 0x%"PRIx32".%"PRIx32", "
                    "cksum %08"PRIx32" (!= %08"PRIx32")",
                rbyd->blocks[0], lfs3_rbyd_trunk(rbyd),
                rbyd->cksum, cksum);
        return LFS3_ERR_CORRUPT;
    }

    // if trunk/weight mismatch _after_ cksums match, that's not a storage
    // error, that's a programming error
    LFS3_ASSERT(lfs3_rbyd_trunk(rbyd) == trunk);
    return 0;
}

// fetch for mutating
//
// this does one of the following:
// 1. nothing, if already fetched
// 2. ckfetch, if checking fetches (LFS3_M_CKFETCHES)
// 3. quickfetch, so we have enough info to mutate
static int lfs3_rbyd_mkfetched(lfs3_t *lfs3, lfs3_rbyd_t *rbyd) {
    // why would you try to fetch a shrub?
    LFS3_ASSERT(!lfs3_rbyd_isshrub(rbyd));

    // already fetched?
    if (lfs3_rbyd_isfetched(rbyd)) {
        return 0;

    // checking fetches?
    } else if (LFS3_IFDEF_CKFETCHES(
            lfs3_m_isckfetches(lfs3->flags),
            false)) {
    #ifdef LFS3_CKFETCHES
        return lfs3_rbyd_ckfetch(lfs3, rbyd,
                rbyd->blocks[0], lfs3_rbyd_trunk(rbyd),
                rbyd->cksum);
    #endif

    // if we're not checking fetches, we can get away with a quick fetch
    } else {
        return lfs3_rbyd_quickfetch(lfs3, rbyd,
                rbyd->blocks[0], lfs3_rbyd_trunk(rbyd),
                rbyd->cksum);
    }
}


// our core rbyd lookup algorithm
//
// finds the next rid+tag such that rid_+tag_ >= rid+tag
static lfs3_stag_t lfs3_rbyd_lookupnext_(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_srid_t *rid_, lfs3_rid_t *weight_, lfs3_data_t *data_,
        lfs3_rheight_t *rheight_) {
    (void)rheight_;
    // these bits should be clear at this point
    LFS3_ASSERT(lfs3_tag_mode(tag) == 0);

    // make sure we never look up zero tags, the way we create
    // unreachable tags has a hole here
    tag = lfs3_max(tag, 0x1);

    // out of bounds? no trunk yet?
    if (rid >= (lfs3_srid_t)rbyd->weight || !lfs3_rbyd_trunk(rbyd)) {
        return LFS3_ERR_NOENT;
    }

    // optionally find height/bheight for debugging rbyd balance
    #ifdef LFS3_DBGRBYDBALANCE
    if (rheight_) {
        rheight_->height = 0;
        rheight_->bheight = 0;
    }
    #endif

    // keep track of bounds as we descend down the tree
    lfs3_size_t branch = lfs3_rbyd_trunk(rbyd);
    lfs3_srid_t lower_rid = 0;
    lfs3_srid_t upper_rid = rbyd->weight;

    // descend down tree
    while (true) {
        lfs3_tag_t alt;
        lfs3_rid_t weight;
        lfs3_size_t jump;
        lfs3_ssize_t d = lfs3_bd_readtag(lfs3,
                rbyd->blocks[0], branch, 0, 0,
                &alt, &weight, &jump,
                NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfs3_tag_isalt(alt)) {
            lfs3_size_t branch_ = branch + d;

            // keep track of height for debugging
            #ifdef LFS3_DBGRBYDBALANCE
            if (rheight_) {
                rheight_->height += 1;

                // only count black+followed alts towards bheight
                if (lfs3_tag_isblack(alt)
                        || lfs3_tag_follow(
                            alt, weight,
                            lower_rid, upper_rid,
                            rid, tag)) {
                    rheight_->bheight += 1;
                }
            }
            #endif

            // take alt?
            if (lfs3_tag_follow(
                    alt, weight,
                    lower_rid, upper_rid,
                    rid, tag)) {
                lfs3_tag_flip(
                        &alt, &weight,
                        lower_rid, upper_rid);
                branch_ = branch - jump;
            }

            lfs3_tag_trim(
                    alt, weight,
                    &lower_rid, &upper_rid,
                    NULL, NULL);
            LFS3_ASSERT(branch_ != branch);
            branch = branch_;

        // found end of tree?
        } else {
            // update the tag rid
            lfs3_srid_t rid__ = upper_rid-1;
            lfs3_tag_t tag__ = lfs3_tag_key(alt);

            // not what we're looking for?
            if (!tag__
                    || rid__ < rid
                    || (rid__ == rid && tag__ < tag)) {
                return LFS3_ERR_NOENT;
            }

            // save what we found
            // TODO how many of these need to be conditional?
            if (rid_) {
                *rid_ = rid__;
            }
            if (weight_) {
                *weight_ = upper_rid - lower_rid;
            }
            if (data_) {
                *data_ = LFS3_DATA_DISK(rbyd->blocks[0], branch + d, jump);
            }
            return tag__;
        }
    }
}


// finds the next rid_+tag_ such that rid_+tag_ >= rid+tag
static lfs3_stag_t lfs3_rbyd_lookupnext(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_srid_t *rid_, lfs3_rid_t *weight_, lfs3_data_t *data_) {
    return lfs3_rbyd_lookupnext_(lfs3, rbyd, rid, tag,
            rid_, weight_, data_,
            NULL);
}

// lookup assumes a known rid
static lfs3_stag_t lfs3_rbyd_lookup(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_data_t *data_) {
    lfs3_srid_t rid__;
    lfs3_stag_t tag__ = lfs3_rbyd_lookupnext(lfs3, rbyd,
            rid, lfs3_tag_key(tag),
            &rid__, NULL, data_);
    if (tag__ < 0) {
        return tag__;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if (rid__ != rid
            || (tag__ & lfs3_tag_mask(tag)) != (tag & lfs3_tag_mask(tag))) {
        return LFS3_ERR_NOENT;
    }

    return tag__;
}



// rbyd append operations


// needed in lfs3_rbyd_appendrev
static inline bool lfs3_m_isrevperturb(uint32_t flags);
static inline bool lfs3_m_isrevnoise(uint32_t flags);

// append a revision count
//
// this is optional, if not called revision count defaults
// to ~0 (+noise/perturb/debug, for btrees)
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendrev(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        uint32_t rev) {
    // should only be called before any tags are written
    LFS3_ASSERT(rbyd->eoff == 0);
    LFS3_ASSERT(rbyd->cksum == 0);

    // perturb first bit?
    //
    // this ensures at least one big changes in the new rbyd, and is
    // necessary to invalidate any ecksums
    #ifdef LFS3_REVPERTURB
    if (lfs3_m_isrevperturb(lfs3->flags)) {
        uint8_t e = 0;
        int err = lfs3_bd_read(lfs3,
                rbyd->blocks[0], 0, LFS3_BD_RELAX,
                &e, 1, 0);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }

        // perturb!
        rev = (rev & ~0x80) | (~e & 0x80);
    }
    #endif

    // xor in pseudorandom noise?
    //
    // this reduces chance of checksum collisions due to filesystem
    // bugs, but is otherwise unnecessary, note we really don't want
    // this enabled during testing!
    #ifdef LFS3_REVNOISE
    if (lfs3_m_isrevnoise(lfs3->flags)) {
        rev ^= ~(~((1 << (28-lfs3_smax(lfs3->recycle_bits, 0)))-1)
                    | 0xff)
                // we need to use gcksum_p because we have be in the
                // middle of updating the gcksum
                & lfs3->gcksum_p;
    }
    #endif

    // revision count stored as le32, we don't use a leb128 encoding as we
    // intentionally allow the revision count to overflow
    uint8_t rev_buf[sizeof(uint32_t)];
    lfs3_tole32(rev, &rev_buf);

    int err = lfs3_bd_prog(lfs3,
            rbyd->blocks[0], lfs3_rbyd_eoff(rbyd),
            &rev_buf, sizeof(uint32_t), 0,
            &rbyd->cksum);
    if (err) {
        return err;
    }

    rbyd->eoff += sizeof(uint32_t);
    return 0;
}
#endif

// other low-level appends
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendtag(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_tag_t tag, lfs3_rid_t weight, lfs3_size_t size) {
    // tag must not be internal at this point
    LFS3_ASSERT(lfs3_tag_suptype(tag) != LFS3_TAG_INTERNAL);
    // bit 7 is reserved for future subtype extensions
    LFS3_ASSERT(!(tag & 0x80));

    // do we fit?
    if (lfs3_rbyd_eoff(rbyd) + LFS3_TAG_DSIZE
            > lfs3->cfg->block_size) {
        return LFS3_ERR_RANGE;
    }

    lfs3_ssize_t d = lfs3_bd_progtag(lfs3,
            rbyd->blocks[0], lfs3_rbyd_eoff(rbyd),
            tag, weight, size, rbyd->eoff & LFS3_BD_PERTURB,
            &rbyd->cksum);
    if (d < 0) {
        return d;
    }

    rbyd->eoff += d;

    // keep track of most recent parity
    #ifdef LFS3_CKMETAPARITY
    lfs3->ptail.block = rbyd->blocks[0];
    lfs3->ptail.off
            = ((lfs3_size_t)(
                    lfs3_parity(rbyd->cksum) ^ lfs3_rbyd_isperturb(rbyd)
                ) << (8*sizeof(lfs3_size_t)-1))
            | lfs3_rbyd_eoff(rbyd);
    #endif

    return 0;
}
#endif

// needed in lfs3_rbyd_appendrattr_
static inline lfs3_size_t lfs3_path_namelen(const char *path);
static lfs3_data_t lfs3_data_frombranch(const lfs3_rbyd_t *branch,
        uint8_t buffer[static LFS3_BRANCH_DSIZE]);
static lfs3_data_t lfs3_data_frombtree(const lfs3_btree_t *btree,
        uint8_t buffer[static LFS3_BTREE_DSIZE]);
static lfs3_data_t lfs3_data_fromshrub(const lfs3_shrub_t *shrub,
        uint8_t buffer[static LFS3_SHRUB_DSIZE]);
static lfs3_data_t lfs3_data_frommptr(const lfs3_block_t mptr[static 2],
        uint8_t buffer[static LFS3_MPTR_DSIZE]);
static lfs3_data_t lfs3_data_fromcompat(lfs3_compat_t compat,
        uint8_t buffer[static LFS3_COMPAT_DSIZE]);
static lfs3_data_t lfs3_data_fromgeometry(const lfs3_geometry_t *geometry,
        uint8_t buffer[static LFS3_GEOMETRY_DSIZE]);

// encode rattrs
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendrattr_(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_tag_t tag, lfs3_srid_t weight, lfs3_from_t from,
        const lfs3_rattr_t *args) {
    // tag must not be internal at this point
    LFS3_ASSERT(lfs3_tag_suptype(tag) != LFS3_TAG_INTERNAL);
    // bit 7 is reserved for future subtype extensions
    LFS3_ASSERT(!(tag & 0x80));

    // encode lazy tags?
    //
    // we encode most tags lazily as this heavily reduces stack usage,
    // though this does make things less gc-able at compile time
    //
    const lfs3_data_t *datas;
    lfs3_size_t data_count;
    struct {
        union {
            lfs3_data_t data;

            struct {
                lfs3_data_t datas[2];
                uint8_t buf[LFS3_LEB128_DSIZE];
            } name;

            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_LE32_DSIZE];
            } le32;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_LEB128_DSIZE];
            } leb128;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_LLEB128_DSIZE];
            } lleb128;

            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_ECKSUM_DSIZE];
            } ecksum;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_BRANCH_DSIZE];
            } branch;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_BTREE_DSIZE];
            } btree;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_SHRUB_DSIZE];
            } shrub;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_MPTR_DSIZE];
            } mptr;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_BPTR_DSIZE];
            } bptr;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_COMPAT_DSIZE];
            } compat;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_GEOMETRY_DSIZE];
            } geometry;
        } u;
    } ctx;

    // nil data?
    if (lfs3_from_from8(from) == LFS3_FROM_NIL) {
        datas = NULL;
        data_count = 0;

    // little buffer?
    } else if (lfs3_from_from8(from) == LFS3_FROM_LBUF) {
        ctx.u.data = LFS3_DATA_BUF(
                (const uint8_t*)args[0],
                lfs3_from_fromcount8(from));
        datas = &ctx.u.data;
        data_count = 1;

    // name?
    } else if (lfs3_from_from8(from) == LFS3_FROM_NAME) {
        ctx.u.name.datas[0] = lfs3_data_fromleb128(args[0], ctx.u.name.buf);
        ctx.u.name.datas[1] = LFS3_DATA_BUF(
                (const char*)args[1],
                lfs3_path_namelen((const char*)args[1]));
        datas = ctx.u.name.datas;
        data_count = 2;

    } else {
        // big buffer?
        if (lfs3_from_from2(from) == LFS3_FROM_BUF) {
            ctx.u.data = LFS3_DATA_BUF((const uint8_t*)args[0], args[1]);
            datas = &ctx.u.data;
            data_count = 1;

        // immediate data?
        } else if (lfs3_from_from2(from) == LFS3_FROM_GRAFT) {
            datas = (const lfs3_data_t*)args;
            data_count = lfs3_from_fromcount2(from);

        // indirect concatenated data?
        } else if (lfs3_from_from2(from) == LFS3_FROM_DATA) {
            datas = (const lfs3_data_t*)args[0];
            data_count = lfs3_from_fromcount2(from);

        // le32?
        } else if (lfs3_from_from2(from) == LFS3_FROM_LE32) {
            ctx.u.le32.data = lfs3_data_fromle32(args[0],
                    ctx.u.le32.buf);
            datas = &ctx.u.le32.data;
            data_count = 1;

        // leb128? little-leb128?
        } else if (lfs3_from_from2(from) == LFS3_FROM_LEB128
                || lfs3_from_from2(from) == LFS3_FROM_LLEB128) {
            // leb128s should not exceed 31-bits
            LFS3_ASSERT(args[0] <= 0x7fffffff);
            // little-leb128s should not exceed 28-bits
            LFS3_ASSERT(lfs3_from_from2(from) != LFS3_FROM_LLEB128
                    || args[0] <= 0x0fffffff);
            ctx.u.leb128.data = lfs3_data_fromleb128(args[0],
                    ctx.u.leb128.buf);
            datas = &ctx.u.leb128.data;
            data_count = 1;

        // ecksum?
        } else if (lfs3_from_from2(from) == LFS3_FROM_ECKSUM) {
            ctx.u.ecksum.data = lfs3_data_fromecksum(
                    &(lfs3_ecksum_t){
                        args[0],
                        args[1]},
                    ctx.u.ecksum.buf);
            datas = &ctx.u.ecksum.data;
            data_count = 1;

        // branch?
        } else if (lfs3_from_from2(from) == LFS3_FROM_BRANCH) {
            ctx.u.branch.data = lfs3_data_frombranch(
                    &(const lfs3_rbyd_t){
                        .blocks[0] = args[0],
                        .trunk = args[1],
                        .cksum = args[2]},
                    ctx.u.ecksum.buf);
            datas = &ctx.u.branch.data;
            data_count = 1;

        // btree?
        } else if (lfs3_from_from2(from) == LFS3_FROM_BTREE) {
            ctx.u.btree.data = lfs3_data_frombtree(
                    (const lfs3_btree_t*)args[0],
                    ctx.u.btree.buf);
            datas = &ctx.u.btree.data;
            data_count = 1;

        // shrub trunk?
        } else if (lfs3_from_from2(from) == LFS3_FROM_SHRUB) {
            // note unlike the other lazy tags, we _need_ to lazily encode
            // shrub trunks, since they change underneath us during mdir
            // compactions, relocations, etc
            ctx.u.shrub.data = lfs3_data_fromshrub(
                    (const lfs3_shrub_t*)args[0],
                    ctx.u.shrub.buf);
            datas = &ctx.u.shrub.data;
            data_count = 1;

        // mptr?
        } else if (lfs3_from_from2(from) == LFS3_FROM_MPTR) {
            ctx.u.mptr.data = lfs3_data_frommptr(
                    (const lfs3_block_t*)args[0],
                    ctx.u.mptr.buf);
            datas = &ctx.u.mptr.data;
            data_count = 1;

        // bptr?
        } else if (lfs3_from_from2(from) == LFS3_FROM_BPTR) {
            ctx.u.bptr.data = lfs3_data_frombptr(
                    (const lfs3_bptr_t*)args[0],
                    ctx.u.bptr.buf);
            datas = &ctx.u.bptr.data;
            data_count = 1;

        // compat flags?
        } else if (lfs3_from_from2(from) == LFS3_FROM_COMPAT) {
            ctx.u.compat.data = lfs3_data_fromcompat(
                    args[0],
                    ctx.u.compat.buf);
            datas = &ctx.u.compat.data;
            data_count = 1;

        // geometry?
        } else if (lfs3_from_from2(from) == LFS3_FROM_GEOMETRY) {
            ctx.u.geometry.data = lfs3_data_fromgeometry(
                    (const lfs3_geometry_t*)args[0],
                    ctx.u.geometry.buf);
            datas = &ctx.u.geometry.data;
            data_count = 1;

        } else {
            LFS3_UNREACHABLE();
        }
    }

    // now everything should be raw data, either in-ram or on-disk

    // find the concatenated size
    lfs3_size_t size = 0;
    for (lfs3_size_t i = 0; i < data_count; i++) {
        size += lfs3_data_size(&datas[i]);
    }

    // do we fit?
    if (lfs3_rbyd_eoff(rbyd) + LFS3_TAG_DSIZE + size
            > lfs3->cfg->block_size) {
        return LFS3_ERR_RANGE;
    }

    // append tag
    int err = lfs3_rbyd_appendtag(lfs3, rbyd,
            tag, weight, size);
    if (err) {
        return err;
    }

    // append data
    for (lfs3_size_t i = 0; i < data_count; i++) {
        err = lfs3_bd_progdata(lfs3,
                rbyd->blocks[0], lfs3_rbyd_eoff(rbyd), &datas[i], 0,
                &rbyd->cksum);
        if (err) {
            return err;
        }

        rbyd->eoff += lfs3_data_size(&datas[i]);
    }

    // keep track of most recent parity
    #ifdef LFS3_CKMETAPARITY
    lfs3->ptail.block = rbyd->blocks[0];
    lfs3->ptail.off
            = ((lfs3_size_t)(
                    lfs3_parity(rbyd->cksum) ^ lfs3_rbyd_isperturb(rbyd)
                ) << (8*sizeof(lfs3_size_t)-1))
            | lfs3_rbyd_eoff(rbyd);
    #endif

    return 0;
}
#endif

// checks before we append
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendinit(lfs3_t *lfs3, lfs3_rbyd_t *rbyd) {
    // must fetch before mutating!
    LFS3_ASSERT(lfs3_rbyd_isfetched(rbyd));

    // we can't do anything if we're not erased
    if (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size) {
        return LFS3_ERR_RANGE;
    }

    // make sure every rbyd starts with a revision count
    if (rbyd->eoff == 0) {
        // we only fallback to this when updating btrees, so default to
        // btree debug bits, they're called low-effort after all
        int err = lfs3_rbyd_appendrev(lfs3, rbyd, 'b');
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

// helper functions for managing the 3-element fifo used in
// lfs3_rbyd_appendrattr
#ifndef LFS3_RDONLY
typedef struct lfs3_alt {
    lfs3_tag_t alt;
    lfs3_rid_t weight;
    lfs3_size_t jump;
} lfs3_alt_t;
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_p_flush(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_alt_t p[static 3],
        int count) {
    // write out some number of alt pointers in our queue
    for (int i = 0; i < count; i++) {
        if (p[3-1-i].alt) {
            // jump=0 represents an unreachable alt, we do write out
            // unreachable alts sometimes in order to maintain the
            // balance of the tree
            LFS3_ASSERT(p[3-1-i].jump || lfs3_tag_isblack(p[3-1-i].alt));
            lfs3_tag_t alt = p[3-1-i].alt;
            lfs3_rid_t weight = p[3-1-i].weight;
            // change to a relative jump at the last minute
            lfs3_size_t jump = (p[3-1-i].jump)
                    ? lfs3_rbyd_eoff(rbyd) - p[3-1-i].jump
                    : 0;

            int err = lfs3_rbyd_appendtag(lfs3, rbyd, alt, weight, jump);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static inline int lfs3_rbyd_p_push(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_alt_t p[static 3],
        lfs3_tag_t alt, lfs3_rid_t weight, lfs3_size_t jump) {
    // jump should actually be in the rbyd
    LFS3_ASSERT(jump < lfs3_rbyd_eoff(rbyd));

    int err = lfs3_rbyd_p_flush(lfs3, rbyd, p, 1);
    if (err) {
        return err;
    }

    lfs3_memmove(p+1, p, 2*sizeof(lfs3_alt_t));
    p[0].alt = alt;
    p[0].weight = weight;
    p[0].jump = jump;
    return 0;
}
#endif

#ifndef LFS3_RDONLY
static inline void lfs3_rbyd_p_pop(
        lfs3_alt_t p[static 3]) {
    lfs3_memmove(p, p+1, 2*sizeof(lfs3_alt_t));
    p[2].alt = 0;
}
#endif

#ifndef LFS3_RDONLY
static void lfs3_rbyd_p_recolor(
        lfs3_alt_t p[static 3]) {
    // propagate a red edge upwards
    p[0].alt &= ~LFS3_TAG_R;

    if (p[1].alt) {
        p[1].alt |= LFS3_TAG_R;

        // unreachable alt? we can prune this now
        if (!p[1].jump) {
            p[1] = p[2];
            p[2].alt = 0;

        // reorder so that top two edges always go in the same direction
        } else if (lfs3_tag_isred(p[2].alt)) {
            if (lfs3_tag_isparallel(p[1].alt, p[2].alt)) {
                // no reorder needed
            } else if (lfs3_tag_isparallel(p[0].alt, p[2].alt)) {
                lfs3_tag_t alt_ = p[1].alt;
                lfs3_rid_t weight_ = p[1].weight;
                lfs3_size_t jump_ = p[1].jump;
                p[1].alt = p[0].alt | LFS3_TAG_R;
                p[1].weight = p[0].weight;
                p[1].jump = p[0].jump;
                p[0].alt = alt_ & ~LFS3_TAG_R;
                p[0].weight = weight_;
                p[0].jump = jump_;
            } else if (lfs3_tag_isparallel(p[0].alt, p[1].alt)) {
                lfs3_tag_t alt_ = p[2].alt;
                lfs3_rid_t weight_ = p[2].weight;
                lfs3_size_t jump_ = p[2].jump;
                p[2].alt = p[1].alt | LFS3_TAG_R;
                p[2].weight = p[1].weight;
                p[2].jump = p[1].jump;
                p[1].alt = p[0].alt | LFS3_TAG_R;
                p[1].weight = p[0].weight;
                p[1].jump = p[0].jump;
                p[0].alt = alt_ & ~LFS3_TAG_R;
                p[0].weight = weight_;
                p[0].jump = jump_;
            } else {
                LFS3_UNREACHABLE();
            }
        }
    }
}
#endif

// our core rbyd append algorithm
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendrattr(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, const lfs3_rattr_t *rattr) {
    // must fetch before mutating!
    LFS3_ASSERT(lfs3_rbyd_isfetched(rbyd));
    // ignore noops
    if (lfs3_rattr_tag(rattr) == LFS3_tag_NOOP) {
        return 0;
    }
    // tag must not be internal at this point
    LFS3_ASSERT(!lfs3_rattr_isinternal(rattr));
    // bit 7 is reserved for future subtype extensions
    LFS3_ASSERT(!(lfs3_rattr_tag(rattr) & 0x80));

    // begin appending
    int err = lfs3_rbyd_appendinit(lfs3, rbyd);
    if (err) {
        return err;
    }

    // figure out what range of tags we're operating on
    lfs3_tag_t tag = lfs3_rattr_tag(rattr);
    lfs3_srid_t weight = lfs3_rattr_weight(rattr);
    lfs3_srid_t a_rid;
    lfs3_srid_t b_rid;
    lfs3_tag_t a_tag;
    lfs3_tag_t b_tag;
    if (!lfs3_tag_isgrow(tag) && weight != 0) {
        if (weight > 0) {
            LFS3_ASSERT(rid <= (lfs3_srid_t)rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid -= 1;
            a_rid = rid + 1;
            b_rid = rid + 1;
        } else {
            // limit range removes to rbyd weight, normally we would reject
            // out-of-bound ranges, but this was the easiest way to implement
            // range removes across btree splits without mutating rattrs
            lfs3_srid_t rid_ = lfs3_min(rid+1, rbyd->weight)-1;
            lfs3_srid_t weight_ = -lfs3_min(-weight - (rid - rid_), rid_+1);
            rid = rid_;
            weight = weight_;
            LFS3_ASSERT(rid < (lfs3_srid_t)rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid += 1;
            a_rid = rid - -weight;
            b_rid = rid;
        }

        a_tag = 0;
        b_tag = 0;

    } else {
        LFS3_ASSERT(rid < (lfs3_srid_t)rbyd->weight);
        LFS3_ASSERT(weight >= -(lfs3_srid_t)rbyd->weight);

        a_rid = rid - lfs3_smax(-weight, 0);
        b_rid = rid;

        // note both normal and rm wide-tags have the same bounds, really it's
        // the normal non-wide-tags that are an outlier here
        if (lfs3_tag_ismask12(tag)) {
            a_tag = 0x000;
            b_tag = 0xfff;
        } else if (lfs3_tag_ismask8(tag)) {
            a_tag = (tag & 0xf00);
            b_tag = (tag & 0xf00) + 0x100;
        } else if (lfs3_tag_ismask2(tag)) {
            a_tag = (tag & 0xffc);
            b_tag = (tag & 0xffc) + 0x004;
        } else if (lfs3_tag_isrm(tag)) {
            a_tag = lfs3_tag_key(tag);
            b_tag = lfs3_tag_key(tag) + 1;
        } else {
            a_tag = lfs3_tag_key(tag);
            b_tag = lfs3_tag_key(tag);
        }
    }
    a_tag = lfs3_max(a_tag, 0x1);
    b_tag = lfs3_max(b_tag, 0x1);

    // keep track of diverged state
    //
    // this is only used if we operate on a range of tags, in which case
    // we may need to write two trunks
    //
    // to pull this off, we make two passes:
    // 1. to write the common trunk + diverged-lower trunk
    // 2. to write the common trunk + diverged-upper trunk, stitching the
    //    two diverged trunks together where they diverged
    //
    bool diverged = false;
    lfs3_tag_t d_tag = 0;
    lfs3_srid_t d_weight = 0;

    // follow the current trunk
    lfs3_size_t branch = lfs3_rbyd_trunk(rbyd);

trunk:;
    // the new trunk starts here
    lfs3_size_t trunk_ = lfs3_rbyd_eoff(rbyd);

    // keep track of bounds as we descend down the tree
    //
    // this gets a bit confusing as we also may need to keep
    // track of both the lower and upper bounds of diverging paths
    // in the case of range deletions
    lfs3_srid_t lower_rid = 0;
    lfs3_srid_t upper_rid = rbyd->weight;
    lfs3_tag_t lower_tag = 0x000;
    lfs3_tag_t upper_tag = 0xfff;

    // no trunk yet?
    if (!branch) {
        goto leaf;
    }

    // queue of pending alts we can emulate rotations with
    lfs3_alt_t p[3] = {{0}, {0}, {0}};
    // keep track of the last incoming branch for yellow splits
    lfs3_size_t y_branch = 0;
    // keep track of the tag we find at the end of the trunk
    lfs3_tag_t tag_ = 0;

    // descend down tree, building alt pointers
    while (true) {
        // keep track of incoming branch
        if (lfs3_tag_isblack(p[0].alt)) {
            y_branch = branch;
        }

        // read the alt pointer
        lfs3_tag_t alt_;
        lfs3_rid_t weight_;
        lfs3_size_t jump_;
        lfs3_ssize_t d = lfs3_bd_readtag(lfs3,
                rbyd->blocks[0], branch, 0, 0,
                &alt_, &weight_, &jump_,
                NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfs3_tag_isalt(alt_)) {
            // make jump absolute
            jump_ = branch - jump_;
            lfs3_size_t branch_ = branch + d;

            // in expected bounds?
            LFS3_ASSERT((lfs3_srid_t)weight_ <= upper_rid - lower_rid);
            // yellow alts should be parallel
            LFS3_ASSERT(!(lfs3_tag_isred(alt_) && lfs3_tag_isred(p[0].alt))
                    || lfs3_tag_isparallel(alt_, p[0].alt));

            // take alt? needs a flip
            //   <b           >b
            // .-'|  =>     .-'|
            // 1  2      1  2  1
            if (lfs3_tag_follow2(
                    alt_, weight_,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    a_rid, a_tag)) {
                lfs3_tag_flip2(
                        &alt_, &weight_,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid);
                LFS3_SWAP(lfs3_size_t, &jump_, &branch_);
            }

            // should've taken red alt? needs a flip
            //      <r              >r
            // .----'|            .-'|
            // |    <b  =>        | >b
            // |  .-'|         .--|-'|
            // 1  2  3      1  2  3  1
            if (lfs3_tag_isred(p[0].alt)
                    && lfs3_tag_follow(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        a_rid, a_tag)) {
                LFS3_SWAP(lfs3_tag_t, &p[0].alt, &alt_);
                LFS3_SWAP(lfs3_rid_t, &p[0].weight, &weight_);
                LFS3_SWAP(lfs3_size_t, &p[0].jump, &jump_);
                alt_ = (alt_ & ~LFS3_TAG_R) | (p[0].alt & LFS3_TAG_R);
                p[0].alt |= LFS3_TAG_R;

                lfs3_tag_flip2(
                        &alt_, &weight_,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid);
                LFS3_SWAP(lfs3_size_t, &jump_, &branch_);
            }

            // do bounds want to take different paths? begin diverging
            //                            >b                    <b
            //                          .-'|                  .-'|
            //         <b  =>           | nb  =>             nb  |
            //    .----'|      .--------|--'      .-----------'  |
            //   <b    <b      |       <b         |             nb
            // .-'|  .-'|      |     .-'|         |        .-----'
            // 1  2  3  4      1  2  3  4  x      1  2  3  4  x  x
            bool diverging = lfs3_tag_diverging2(
                    alt_, weight_,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    a_rid, a_tag,
                    b_rid, b_tag);
            bool r_diverging = lfs3_tag_isred(p[0].alt)
                    && lfs3_tag_diverging(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        a_rid, a_tag,
                        b_rid, b_tag);
            if (!diverged) {
                // both diverging? collapse
                //      <r              >b
                // .----'|            .-'|
                // |    <b  =>        |  |
                // |  .-'|      .-----|--'
                // 1  2  3      1  2  3  x
                if (diverging && r_diverging) {
                    LFS3_ASSERT(a_rid < b_rid || a_tag < b_tag);
                    LFS3_ASSERT(lfs3_tag_isparallel(alt_, p[0].alt));

                    // adjust b_rid temporarily to ignore collapsed weight
                    d_weight += weight_;
                    upper_rid -= weight_;
                    b_rid -= weight_;

                    weight_ = p[0].weight;
                    jump_ = p[0].jump;
                    lfs3_rbyd_p_pop(p);

                    r_diverging = false;
                }

                // diverging? start trimming inner alts
                if ((diverging || r_diverging)
                        // diverging black?
                        && (lfs3_tag_isblack(alt_)
                            // give up if we find a yellow alt
                            || (lfs3_tag_isred(p[0].alt)))) {
                    diverged = true;

                    // diverging lower? revert collapsed b_rid
                    //                            >b
                    //                          .-'|
                    //         <b  =>           | nb
                    //    .----'|      .--------|--'
                    //   <b    <b      |       <b
                    // .-'|  .-'|      |     .-'|
                    // 1  2  3  4      1  2  3  4  x
                    if (a_rid < b_rid || a_tag < b_tag) {
                        b_rid += d_weight;

                    // diverging upper? stitch together both trunks
                    //            >b                    <b
                    //          .-'|                  .-'|
                    //          | nb  =>             nb  |
                    // .--------|--'      .-----------'  |
                    // |       <b         |             nb
                    // |     .-'|         |        .-----'
                    // 1  2  3  4  x      1  2  3  4  x  x
                    } else {
                        LFS3_ASSERT(!r_diverging);

                        alt_ = LFS3_TAG_ALT(
                            alt_ & LFS3_TAG_R,
                            LFS3_TAG_LE,
                            d_tag);
                        weight_ -= d_weight;
                        lower_rid += d_weight;
                    }
                }

            } else {
                // diverged? trim so alt will be pruned
                //   <b  =>       nb
                // .-'|         .--'
                // 3  4      3  4  x
                if (diverging) {
                    d_weight += weight_;

                    lfs3_tag_trim(
                            alt_, weight_,
                            &lower_rid, &upper_rid,
                            &lower_tag, &upper_tag);
                    weight_ = 0;
                }
            }

            // note we need to prioritize yellow-split pruning here,
            // which unfortunately makes this logic a bit of a mess

            // prune unreachable yellow-split yellow alts
            //            <b                    >b
            //          .-'|                  .-'|
            //         <y  |                  |  |
            // .-------'|  |                  |  |
            // |       <r  |  =>              | >b
            // |  .----'   |         .--------|-'|
            // |  |       <b         |       <b  |
            // |  |  .----'|         |  .----'|  |
            // 1  2  3  4  4      1  2  3  4  4  1
            if (lfs3_tag_isred(p[0].alt)
                    && lfs3_tag_unreachable(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)
                    && p[0].jump > branch) {
                alt_ &= ~LFS3_TAG_R;
                lfs3_rbyd_p_pop(p);

            // prune unreachable yellow-split red alts
            //            <b                    >b
            //          .-'|                  .-'|
            //         <y  |                  | <b
            // .-------'|  |      .-----------|-'|
            // |       <r  |  =>  |           |  |
            // |  .----'   |      |           |  |
            // |  |       <b      |          <b  |
            // |  |  .----'|      |     .----'|  |
            // 1  2  3  4  4      1  2  3  4  4  2
            } else if (lfs3_tag_isred(p[0].alt)
                    && lfs3_tag_unreachable2(
                        alt_, weight_,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)
                    && jump_ > branch) {
                alt_ = p[0].alt & ~LFS3_TAG_R;
                weight_ = p[0].weight;
                jump_ = p[0].jump;
                lfs3_rbyd_p_pop(p);
            }

            // we should have pruned unreachable red alts earlier
            LFS3_ASSERT(!lfs3_tag_isred(p[0].alt)
                    || !lfs3_tag_unreachable(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag));

            // prune unreachable alts
            if (lfs3_tag_unreachable2(
                    alt_, weight_,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    lower_tag, upper_tag)) {
                // root alts are a special case that we can prune
                // immediately
                //         <b  =>             <b
                //    .----'|            .----'|
                //   <b    <b            |     |
                // .-'|  .-'|            |  .--'
                // 1  3  4  5      1  3  4  5  x
                if (!p[0].alt) {
                    branch = branch_;
                    continue;

                // prune unreachable red alts if black alts are still
                // reachable, we do this early to avoid issues with
                // negative weights
                //      <r  =>          <b
                // .----'|         .----'|
                // |    <b         |     |
                // |  .-'|         |  .--'
                // 1  2  3      1  2  3  x
                } else if (lfs3_tag_isred(alt_) && branch_ >= branch) {
                    branch = branch_;
                    continue;

                // prune unreachable recolorable alts
                //      <r  =>          <b
                // .----'|      .-------'|
                // |    <b      |        |
                // |  .-'|      |  .-----'
                // 1  2  3      1  2  3  x
                } else if (lfs3_tag_isred(p[0].alt)) {
                    LFS3_ASSERT(jump_ < branch);
                    alt_ = (p[0].alt & ~LFS3_TAG_R) | (alt_ & LFS3_TAG_R);
                    weight_ = p[0].weight;
                    jump_ = p[0].jump;
                    lfs3_rbyd_p_pop(p);

                // we can't prune non-root black alts or we risk
                // breaking the color balance of our tree, so instead
                // we just mark these alts as unreachable (jump=0), and
                // collapse them if we propagate a red edge later
                //   <b  =>       nb
                // .-'|         .--'
                // 3  4      3  4  x
                //
                // we do this to red alts as well, but only if the whole
                // edge is unreachable, otherwise we'd risk negative
                // weight mess trying to follow unreachable red alts
                //      <r  =>          nb
                // .----'|               |
                // |    <b               |
                // |  .-'|            .--'
                // 1  2  3      1  2  3  x
                } else {
                    alt_ = LFS3_TAG_ALT(
                            LFS3_TAG_B,
                            LFS3_TAG_LE,
                            (diverged && (a_rid > b_rid || a_tag > b_tag))
                                ? d_tag
                                : lower_tag);
                    LFS3_ASSERT(weight_ == 0);
                    // jump_=0 also asserts the alt is unreachable (or
                    // else we loop indefinitely), and uses the minimum
                    // alt encoding
                    jump_ = 0;
                }
            }

            // two reds makes a yellow, split?
            //
            // note we've lost the original yellow edge because of flips, but
            // we know the red edge is the only branch_ > branch
            if (lfs3_tag_isred(alt_) && lfs3_tag_isred(p[0].alt)) {
                // if we take the red or yellow alt we can just point
                // to the black alt
                //         <y                 >b
                // .-------'|               .-'|
                // |       <r               | >b
                // |  .----'|  =>     .-----|-'|
                // |  |    <b         |    <b  |
                // |  |  .-'|         |  .-'|  |
                // 1  2  3  4      1  2  3  4  1
                if (branch_ < branch) {
                    if (jump_ > branch) {
                        LFS3_SWAP(lfs3_tag_t, &p[0].alt, &alt_);
                        LFS3_SWAP(lfs3_rid_t, &p[0].weight, &weight_);
                        LFS3_SWAP(lfs3_size_t, &p[0].jump, &jump_);
                    }
                    alt_ &= ~LFS3_TAG_R;

                    lfs3_tag_trim(
                            p[0].alt, p[0].weight,
                            &lower_rid, &upper_rid,
                            &lower_tag, &upper_tag);
                    lfs3_rbyd_p_recolor(p);

                // otherwise we need to point to the yellow alt and
                // prune later
                //                            <b
                //                          .-'|
                //         <y              <y  |
                // .-------'|      .-------'|  |
                // |       <r  =>  |       <r  |
                // |  .----'|      |  .----'   |
                // |  |    <b      |  |       <b
                // |  |  .-'|      |  |  .----'|
                // 1  2  3  4      1  2  3  4  4
                } else {
                    LFS3_ASSERT(y_branch != 0);
                    p[0].alt = alt_;
                    p[0].weight += weight_;
                    p[0].jump = y_branch;

                    lfs3_tag_trim(
                            p[0].alt, p[0].weight,
                            &lower_rid, &upper_rid,
                            &lower_tag, &upper_tag);
                    lfs3_rbyd_p_recolor(p);

                    branch = branch_;
                    continue;
                }
            }

            // red alt? we need to read the rest of the 2-3-4 node
            if (lfs3_tag_isred(alt_)) {
                // undo flip temporarily
                if (branch_ < branch) {
                    lfs3_tag_flip2(
                            &alt_, &weight_,
                            p[0].alt, p[0].weight,
                            lower_rid, upper_rid);
                    LFS3_SWAP(lfs3_size_t, &jump_, &branch_);
                }

            // black alt? terminate 2-3-4 nodes
            } else {
                // trim alts from our current bounds
                lfs3_tag_trim2(
                        alt_, weight_,
                        p[0].alt, p[0].weight,
                        &lower_rid, &upper_rid,
                        &lower_tag, &upper_tag);
            }

            // push alt onto our queue
            err = lfs3_rbyd_p_push(lfs3, rbyd, p,
                    alt_, weight_, jump_);
            if (err) {
                return err;
            }

            // continue to next alt
            LFS3_ASSERT(branch_ != branch);
            branch = branch_;
            continue;

        // found end of tree?
        } else {
            // update the found tag
            tag_ = lfs3_tag_key(alt_);

            // the last alt should always end up black
            LFS3_ASSERT(lfs3_tag_isblack(p[0].alt));

            if (diverged) {
                // diverged lower trunk? move on to upper trunk
                if (a_rid < b_rid || a_tag < b_tag) {
                    // keep track of the lower diverged bound
                    d_tag = lower_tag;
                    d_weight += upper_rid - lower_rid;

                    // flush any pending alts
                    err = lfs3_rbyd_p_flush(lfs3, rbyd, p, 3);
                    if (err) {
                        return err;
                    }

                    // terminate diverged trunk with an unreachable tag
                    err = lfs3_rbyd_appendtag(lfs3, rbyd,
                            (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0)
                                | LFS3_TAG_NULL,
                            0, 0);
                    if (err) {
                        return err;
                    }

                    // swap tag/rid and move on to upper trunk
                    diverged = false;
                    branch = trunk_;
                    LFS3_SWAP(lfs3_tag_t, &a_tag, &b_tag);
                    LFS3_SWAP(lfs3_srid_t, &a_rid, &b_rid);
                    goto trunk;

                } else {
                    // use the lower diverged bound for leaf weight
                    // calculation
                    lower_rid -= d_weight;
                    lower_tag = d_tag;
                }
            }

            goto stem;
        }
    }

stem:;
    // split leaf nodes?
    //
    // note we bias the weights here so that lfs3_rbyd_lookupnext
    // always finds the next biggest tag
    //
    // note also if tag_ is null, we found a removed tag that we should just
    // prune
    //
    // this gets real messy because we have a lot of special behavior built in:
    // - default         => split if tags mismatch
    // - weight>0, !grow => split if tags mismatch or we're inserting a new tag
    // - rm-bit set      => never split, but emit alt-always tags, making our
    //                      tag effectively unreachable
    //
    lfs3_tag_t alt_ = 0;
    lfs3_rid_t weight_ = 0;
    if (tag_
            && (upper_rid-1 < rid-lfs3_smax(-weight, 0)
                || (upper_rid-1 == rid-lfs3_smax(-weight, 0)
                    && ((!lfs3_tag_isgrow(tag) && weight > 0)
                        || ((tag_ & lfs3_tag_mask(tag))
                            < (tag & lfs3_tag_mask(tag))))))) {
        if (lfs3_tag_isrm(tag) || !lfs3_tag_key(tag)) {
            // if removed, make our tag unreachable
            alt_ = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_GT, lower_tag);
            weight_ = upper_rid - lower_rid + weight;
            upper_rid -= weight_;
        } else {
            // split less than
            alt_ = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_LE, tag_);
            weight_ = upper_rid - lower_rid;
            lower_rid += weight_;
        }

    } else if (tag_
            && (upper_rid-1 > rid
                || (upper_rid-1 == rid
                    && ((!lfs3_tag_isgrow(tag) && weight > 0)
                        || ((tag_ & lfs3_tag_mask(tag))
                            > (tag & lfs3_tag_mask(tag))))))) {
        if (lfs3_tag_isrm(tag) || !lfs3_tag_key(tag)) {
            // if removed, make our tag unreachable
            alt_ = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_GT, lower_tag);
            weight_ = upper_rid - lower_rid + weight;
            upper_rid -= weight_;
        } else {
            // split greater than
            alt_ = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_GT, tag);
            weight_ = upper_rid - (rid+1);
            upper_rid -= weight_;
        }
    }

    if (alt_) {
        err = lfs3_rbyd_p_push(lfs3, rbyd, p,
                alt_, weight_, branch);
        if (err) {
            return err;
        }

        // introduce a red edge
        lfs3_rbyd_p_recolor(p);
    }

    // flush any pending alts
    err = lfs3_rbyd_p_flush(lfs3, rbyd, p, 3);
    if (err) {
        return err;
    }

leaf:;
    // write the actual tag
    //
    // note we always need a non-alt to terminate the trunk, otherwise we
    // can't find trunks during fetch
    err = lfs3_rbyd_appendrattr_(lfs3, rbyd,
            // mark as shrub if we are a shrub
            (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0)
                // rm => null, otherwise strip off control bits
                | ((lfs3_tag_isrm(tag))
                    ? LFS3_TAG_NULL
                    : lfs3_tag_key(tag)),
            upper_rid - lower_rid + weight,
            lfs3_rattr_from(rattr),
            lfs3_rattr_args(rattr));
    if (err) {
        return err;
    }

    // update the trunk and weight
    rbyd->trunk = (rbyd->trunk & LFS3_RBYD_ISSHRUB) | trunk_;
    rbyd->weight += weight;
    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendcksum_(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        uint32_t cksum) {
    // align to the next prog unit
    //
    // this gets a bit complicated as we have two types of cksums:
    //
    // - 9-word cksum with ecksum to check following prog (middle of block):
    //   .---+---+---+---.              ecksum tag:        1 be16    2 bytes
    //   |  tag  | 0 |siz|              ecksum weight (0): 1 leb128  1 byte
    //   +---+---+---+---+              ecksum size:       1 leb128  1 byte
    //   | ecksize       |              ecksum cksize:     1 leb128  <=4 bytes
    //   +---+- -+- -+- -+              ecksum cksum:      1 le32    4 bytes
    //   |    ecksum     |
    //   +---+---+---+---+- -+- -+- -.  cksum tag:         1 be16    2 bytes
    //   |  tag  | 0 | size          |  cksum weight (0):  1 leb128  1 byte
    //   +---+---+---+---+- -+- -+- -'  cksum size:        1 leb128  <=4 bytes
    //   |     cksum     |              cksum cksum:       1 le32    4 bytes
    //   '---+---+---+---'              total:                       <=23 bytes
    //
    // - 4-word cksum with no following prog (end of block):
    //   .---+---+---+---+- -+- -+- -.  cksum tag:         1 be16    2 bytes
    //   |  tag  | 0 | size          |  cksum weight (0):  1 leb128  1 byte
    //   +---+---+---+---+- -+- -+- -'  cksum size:        1 leb128  <=4 bytes
    //   |     cksum     |              cksum cksum:       1 le32    4 bytes
    //   '---+---+---+---'              total:                       <=11 bytes
    //
    lfs3_size_t off_ = lfs3_alignup(
            lfs3_rbyd_eoff(rbyd) + 2+1+1+4+4 + 2+1+4+4,
            lfs3->cfg->prog_size);

    // space for ecksum?
    bool perturb = false;
    if (off_ < lfs3->cfg->block_size) {
        // read the leading byte in case we need to perturb the next commit,
        // this should hopefully stay in our cache
        uint8_t e = 0;
        int err = lfs3_bd_read(lfs3,
                rbyd->blocks[0], off_, lfs3->cfg->prog_size,
                &e, 1, LFS3_BD_RELAX);
        if (err && err != LFS3_ERR_CORRUPT
                && LFS3_IFDEF_EVICT(
                    err != LFS3_ERR_DAMAGED
                        && err != LFS3_ERR_CONDEMNED,
                    true)) {
            return err;
        }

        // we don't want the next commit to appear as valid, so we
        // intentionally perturb the commit if this happens, this is
        // roughly equivalent to inverting all tags' valid bits
        perturb = ((e >> 7) == lfs3_parity(cksum));

        // calculate the erased-state checksum
        lfs3_ecksum_t ecksum;
        err = lfs3_bd_ecksum(lfs3,
                rbyd->blocks[0], off_, lfs3->cfg->prog_size,
                LFS3_BD_RELAX,
                &ecksum);
        if (err && err != LFS3_ERR_CORRUPT
                && LFS3_IFDEF_EVICT(
                    err != LFS3_ERR_DAMAGED
                        && err != LFS3_ERR_CONDEMNED,
                    true)) {
            return err;
        }

        err = lfs3_rbyd_appendrattr_(lfs3, rbyd, 
                LFS3_TAG_ECKSUM, 0,
                LFS3_FROM_ECKSUM, (const lfs3_rattr_t[]){
                    LFS3_RATTR_ARG(ecksum.cksize),
                    LFS3_RATTR_ARG(ecksum.cksum)});
        if (err) {
            return err;
        }

    // at least space for a cksum?
    } else if (lfs3_rbyd_eoff(rbyd) + 2+1+4+4 <= lfs3->cfg->block_size) {
        // note this implicitly marks the rbyd as unerased
        off_ = lfs3->cfg->block_size;

    // not even space for a cksum? we can't finish the commit
    } else {
        return LFS3_ERR_RANGE;
    }

    // build the end-of-commit checksum tag
    //
    // note padding-size depends on leb-encoding depends on padding-size
    // depends leb-encoding depends on... to get around this catch-22 we
    // just always write a fully-expanded leb128 encoding
    //
    bool v = lfs3_parity(rbyd->cksum) ^ lfs3_rbyd_isperturb(rbyd);
    uint8_t cksum_buf[2+1+4+4];
    cksum_buf[0] = (uint8_t)(LFS3_TAG_CKSUM >> 8)
            // set the valid bit to the cksum parity
            | ((uint8_t)v << 7);
    cksum_buf[1] = (uint8_t)(LFS3_TAG_CKSUM >> 0)
            // set the perturb bit so next commit is invalid
            | ((uint8_t)perturb << 2)
            // include the lower 2 bits of the block address to help
            // with resynchronization
            | (rbyd->blocks[0] & 0x3);
    cksum_buf[2] = 0;

    lfs3_size_t padding = off_ - (lfs3_rbyd_eoff(rbyd) + 2+1+4);
    cksum_buf[3] = 0x80 | (0x7f & (padding >>  0));
    cksum_buf[4] = 0x80 | (0x7f & (padding >>  7));
    cksum_buf[5] = 0x80 | (0x7f & (padding >> 14));
    cksum_buf[6] = 0x00 | (0x7f & (padding >> 21));

    // exclude the valid bit
    uint32_t cksum_ = rbyd->cksum ^ ((uint32_t)v << 7);
    // calculate the commit checksum
    cksum_ = lfs3_crc32c(cksum_, cksum_buf, 2+1+4);
    // and perturb, perturbing the commit checksum avoids a perturb hole
    // after the last valid bit
    //
    // note the odd-parity zero preserves our position in the crc32c
    // ring while only changing the parity
    cksum_ ^= (lfs3_rbyd_isperturb(rbyd)) ? LFS3_CRC32C_ODDZERO : 0;
    lfs3_tole32(cksum_, &cksum_buf[2+1+4]);

    // prog, when this lands on disk commit is committed
    int err = lfs3_bd_prog(lfs3, rbyd->blocks[0], lfs3_rbyd_eoff(rbyd),
            cksum_buf, 2+1+4+4, 0,
            NULL);
    if (err) {
        return err;
    }

    // flush any pending progs
    err = lfs3_bd_flush(lfs3, 0, NULL);
    if (err) {
        return err;
    }

    // update the eoff and perturb
    rbyd->eoff
            = ((lfs3_size_t)perturb << (8*sizeof(lfs3_size_t)-1))
            | off_;
    // revert to canonical checksum
    rbyd->cksum = cksum;

    // if our commit exceeds the configured compaction threshold, set
    // LFS3_I_COMPACTMETA
    //
    // somewhere, an rbyd can be compacted
    if (lfs3_rbyd_eoff(rbyd)
            > ((lfs3->cfg->gc_compactmeta_thresh)
                ? lfs3->cfg->gc_compactmeta_thresh
                : lfs3->cfg->block_size - lfs3->cfg->block_size/8)) {
        lfs3->flags |= LFS3_I_COMPACTMETA;
    }

    #ifdef LFS3_DBGRBYDCOMMITS
    LFS3_DEBUG("Committed rbyd 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "eoff %"PRId32", cksum %"PRIx32,
                rbyd->blocks[0], lfs3_rbyd_trunk(rbyd),
                rbyd->weight,
                (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size)
                    ? -1
                    : (lfs3_ssize_t)lfs3_rbyd_eoff(rbyd),
                rbyd->cksum);
    #endif
    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendcksum(lfs3_t *lfs3, lfs3_rbyd_t *rbyd) {
    // begin appending
    int err = lfs3_rbyd_appendinit(lfs3, rbyd);
    if (err) {
        return err;
    }

    // append checksum stuff
    return lfs3_rbyd_appendcksum_(lfs3, rbyd, rbyd->cksum);
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendrattrs(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_srid_t start_rid, lfs3_srid_t end_rid,
        const lfs3_rattr_t *rattrs) {
    // append each tag to the tree
    for (const lfs3_rattr_t *r = rattrs; *r; r = lfs3_rattr_next(r, &rid)) {
        // treat inserts after the first tag as though they are splits,
        // sequential inserts don't really make sense otherwise
        if (r > rattrs && lfs3_rattr_isinsert(r)) {
            rid += 1;
        }

        // don't write tags outside of the requested range
        if (rid >= start_rid
                // note the use of rid+1 and unsigned comparison here to
                // treat end_rid=-1 as "unbounded" in such a way that rid=-1
                // is still included
                && (lfs3_size_t)(
                        // we also use the lowest rid here so that range
                        // removes work across btree splits
                        (rid - lfs3_smax(-lfs3_rattr_weight(r)-1, 0))
                            + 1)
                    <= (lfs3_size_t)end_rid) {
            int err = lfs3_rbyd_appendrattr(lfs3, rbyd,
                    rid - lfs3_smax(start_rid, 0),
                    r);
            if (err) {
                return err;
            }
        }

        // we need to make sure we keep start_rid/end_rid updated with
        // weight changes
        if (rid < start_rid) {
            start_rid += lfs3_rattr_weight(r);
        }
        if (rid < end_rid) {
            end_rid += lfs3_rattr_weight(r);
        }
    }

    return 0;
}

static int lfs3_rbyd_commit(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, const lfs3_rattr_t *rattrs) {
    // append each tag to the tree
    int err = lfs3_rbyd_appendrattrs(lfs3, rbyd, rid, -1, -1, rattrs);
    if (err) {
        return err;
    }

    // append a cksum, finalizing the commit
    err = lfs3_rbyd_appendcksum(lfs3, rbyd);
    if (err) {
        return err;
    }

    return 0;
}
#endif


// Calculate the maximum possible disk usage required by this rbyd after
// compaction. This uses a conservative estimate so the actual on-disk
// cost should be smaller.
//
// This also returns a good split_rid in case the rbyd needs to be
// split.
//
// Note we use simple constants for tags included in shrubs, to help
// simplify shrub estimate updates.
//
// TODO do we need to include commit overhead here?
#ifndef LFS3_RDONLY
static lfs3_ssize_t lfs3_rbyd_estimate(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t start_rid, lfs3_srid_t end_rid,
        lfs3_srid_t *split_rid_) {
    // calculate dsize by starting from the outside ids and working inwards,
    // this naturally gives us a split rid
    //
    // TODO adopt this a/b naming scheme in lfs3_rbyd_appendrattr?
    lfs3_srid_t a_rid = start_rid;
    lfs3_srid_t b_rid = lfs3_min(rbyd->weight, end_rid);
    lfs3_size_t a_dsize = 0;
    lfs3_size_t b_dsize = 0;
    lfs3_size_t rbyd_dsize = 0;

    while (a_rid != b_rid) {
        if (a_dsize > b_dsize
                // bias so lower dsize >= upper dsize
                || (a_dsize == b_dsize && a_rid > b_rid)) {
            LFS3_SWAP(lfs3_srid_t, &a_rid, &b_rid);
            LFS3_SWAP(lfs3_size_t, &a_dsize, &b_dsize);
        }

        if (a_rid > b_rid) {
            a_rid -= 1;
        }

        lfs3_stag_t tag = 0;
        lfs3_rid_t weight = 0;
        lfs3_size_t dsize_ = 0;
        while (true) {
            lfs3_srid_t rid_;
            lfs3_rid_t weight_;
            lfs3_data_t data;
            tag = lfs3_rbyd_lookupnext(lfs3, rbyd,
                    a_rid, tag+1,
                    &rid_, &weight_, &data);
            if (tag < 0) {
                if (tag == LFS3_ERR_NOENT) {
                    break;
                }
                return tag;
            }
            if (rid_ > a_rid+lfs3_smax(weight_-1, 0)) {
                break;
            }

            // keep track of rid and weight
            a_rid = rid_;
            weight += weight_;

            // include the cost of this tag

            // bit of a hack, but we need to be more conservative around
            // tags included in shrub estimates, as we don't want to
            // read the on-disk encoding just to update estimates
            // correctly
            //
            // fortunately, only a handful of tags can actually end up
            // in shrubs

            // shrubbed branch?
            if (lfs3_rbyd_isshrub(rbyd) && tag == LFS3_TAG_BRANCH) {
                dsize_ += lfs3->rattr_estimate + LFS3_BRANCH_DSIZE;
            // shrubbed bptr?
            } else if (lfs3_rbyd_isshrub(rbyd) && tag == LFS3_TAG_BLOCK) {
                dsize_ += lfs3->rattr_estimate + LFS3_BPTR_DSIZE;
            // everything else, including shrubbed data
            } else {
                dsize_ += lfs3->rattr_estimate + lfs3_data_size(&data);
            }
        }

        if (a_rid == -1) {
            rbyd_dsize += dsize_;
        } else {
            a_dsize += dsize_;
        }

        if (a_rid < b_rid) {
            a_rid += 1;
        } else {
            a_rid -= lfs3_smax(weight-1, 0);
        }
    }

    if (split_rid_) {
        *split_rid_ = a_rid;
    }

    return rbyd_dsize + a_dsize + b_dsize;
}
#endif

// appends a raw tag as a part of compaction, note these must
// be appended in order!
//
// also note rattr.weight here is total weight not delta weight
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendcompactrattr(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        const lfs3_rattr_t *rattr) {
    // begin appending
    int err = lfs3_rbyd_appendinit(lfs3, rbyd);
    if (err) {
        return err;
    }

    // write the tag
    err = lfs3_rbyd_appendrattr_(lfs3, rbyd,
            (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0)
                | lfs3_rattr_tag(rattr),
            lfs3_rattr_weight(rattr),
            lfs3_rattr_from(rattr),
            lfs3_rattr_args(rattr));
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendcompactrbyd(lfs3_t *lfs3, lfs3_rbyd_t *rbyd_,
        const lfs3_rbyd_t *rbyd, lfs3_srid_t start_rid, lfs3_srid_t end_rid) {
    // copy over tags in the rbyd in order
    lfs3_srid_t rid = start_rid;
    lfs3_stag_t tag = 0;
    while (true) {
        lfs3_rid_t weight;
        lfs3_data_t data;
        tag = lfs3_rbyd_lookupnext(lfs3, rbyd,
                rid, tag+1,
                &rid, &weight, &data);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }
        // end of range? note the use of rid+1 and unsigned comparison here to
        // treat end_rid=-1 as "unbounded" in such a way that rid=-1 is still
        // included
        if ((lfs3_size_t)(rid + 1) > (lfs3_size_t)end_rid) {
            break;
        }

        // write the tag
        int err = lfs3_rbyd_appendcompactrattr(lfs3, rbyd_,
                (const lfs3_rattr_t[]){
                    LFS3_RATTR(tag, -2, 1, LFS3_FROM_DATA),
                    LFS3_RATTR_WEIGHT(weight),
                    LFS3_RATTR_ARG(&data)});
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendcompaction(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_size_t off) {
    // begin appending
    int err = lfs3_rbyd_appendinit(lfs3, rbyd);
    if (err) {
        return err;
    }

    // clamp offset to be after the revision count
    off = lfs3_max(off, sizeof(uint32_t));

    // empty rbyd? write a null tag so our trunk can still point to something
    if (lfs3_rbyd_eoff(rbyd) == off) {
        err = lfs3_rbyd_appendtag(lfs3, rbyd,
                // mark as shrub if we are a shrub
                (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0)
                    | LFS3_TAG_NULL,
                0,
                0);
        if (err) {
            return err;
        }

        rbyd->trunk = (rbyd->trunk & LFS3_RBYD_ISSHRUB) | off;
        rbyd->weight = 0;
        return 0;
    }

    // connect every other trunk together, building layers of a perfectly
    // balanced binary tree upwards until we have a single trunk
    lfs3_size_t layer = off;
    lfs3_rid_t weight = 0;
    lfs3_tag_t tag_ = 0;
    while (true) {
        lfs3_size_t layer_ = lfs3_rbyd_eoff(rbyd);
        off = layer;
        while (off < layer_) {
            // connect two trunks together with a new binary trunk
            for (int i = 0; i < 2 && off < layer_; i++) {
                lfs3_size_t trunk = off;
                lfs3_tag_t tag = 0;
                weight = 0;
                while (true) {
                    lfs3_tag_t tag__;
                    lfs3_rid_t weight__;
                    lfs3_size_t size__;
                    lfs3_ssize_t d = lfs3_bd_readtag(lfs3,
                            rbyd->blocks[0], off, layer_ - off, 0,
                            &tag__, &weight__, &size__,
                            NULL);
                    if (d < 0) {
                        return d;
                    }
                    off += d;

                    // skip any data
                    if (!lfs3_tag_isalt(tag__)) {
                        off += size__;
                    }

                    // ignore shrub trunks, unless we are actually compacting
                    // a shrub tree
                    if (!lfs3_tag_isalt(tag__)
                            && lfs3_tag_isshrub(tag__)
                            && !lfs3_rbyd_isshrub(rbyd)) {
                        trunk = off;
                        weight = 0;
                        continue;
                    }

                    // keep track of trunk's trunk and weight
                    weight += weight__;

                    // keep track of the last non-null tag in our trunk.
                    // Because of how we construct each layer, the last
                    // non-null tag is the largest tag in that part of
                    // the tree
                    if (tag__ & ~LFS3_TAG_SHRUB) {
                        tag = tag__;
                    }

                    // did we hit a tag that terminates our trunk?
                    if (!lfs3_tag_isalt(tag__)) {
                        break;
                    }
                }

                // do we only have one trunk? we must be done
                if (trunk == layer && off >= layer_) {
                    goto done;
                }

                // connect with an altle/altgt
                //
                // note we need to use altles for all but the last tag
                // so we know the largest tag when building the next
                // layer, but for that last tag we need an altgt so
                // future appends maintain the balance of the tree
                err = lfs3_rbyd_appendtag(lfs3, rbyd,
                        (off < layer_)
                            ? LFS3_TAG_ALT(
                                (i == 0) ? LFS3_TAG_R : LFS3_TAG_B,
                                LFS3_TAG_LE,
                                tag)
                            : LFS3_TAG_ALT(
                                LFS3_TAG_B,
                                LFS3_TAG_GT,
                                tag_),
                        weight,
                        lfs3_rbyd_eoff(rbyd) - trunk);
                if (err) {
                    return err;
                }

                // keep track of the previous tag for altgts
                tag_ = tag;
            }

            // terminate with a null tag
            err = lfs3_rbyd_appendtag(lfs3, rbyd,
                    // mark as shrub if we are a shrub
                    (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0)
                        | LFS3_TAG_NULL,
                    0,
                    0);
            if (err) {
                return err;
            }
        }

        layer = layer_;
    }

done:;
    // done! just need to update our trunk. Note we could have no trunks
    // after compaction. Leave this to upper layers to take care of this.
    rbyd->trunk = (rbyd->trunk & LFS3_RBYD_ISSHRUB) | layer;
    rbyd->weight = weight;

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_rbyd_compact(lfs3_t *lfs3, lfs3_rbyd_t *rbyd_,
        const lfs3_rbyd_t *rbyd, lfs3_srid_t start_rid, lfs3_srid_t end_rid) {
    // append rbyd
    int err = lfs3_rbyd_appendcompactrbyd(lfs3, rbyd_,
            rbyd, start_rid, end_rid);
    if (err) {
        return err;
    }

    // compact
    err = lfs3_rbyd_appendcompaction(lfs3, rbyd_, 0);
    if (err) {
        return err;
    }

    return 0;
}
#endif

// append a secondary "shrub" tree
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendshrub(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        const lfs3_shrub_t *shrub) {
    // keep track of the start of the new tree
    lfs3_size_t off = lfs3_rbyd_eoff(rbyd);
    // mark as shrub
    rbyd->trunk |= LFS3_RBYD_ISSHRUB;

    // compact our shrub
    int err = lfs3_rbyd_appendcompactrbyd(lfs3, rbyd,
            shrub, -1, -1);
    if (err) {
        return err;
    }

    err = lfs3_rbyd_appendcompaction(lfs3, rbyd, off);
    if (err) {
        return err;
    }

    return 0;
}
#endif


// some low-level name things
//
// names in littlefs are tuples of directory-ids + ascii/utf8 strings

// binary search an rbyd for a name, leaving the rid_/tag_/weight_/data_
// with the best matching name if not found
static lfs3_scmp_t lfs3_rbyd_namelookup(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_srid_t *rid_, lfs3_tag_t *tag_, lfs3_rid_t *weight_,
        lfs3_data_t *data_) {
    // empty rbyd? leave it up to upper layers to handle this
    if (rbyd->weight == 0) {
        return LFS3_ERR_NOENT;
    }

    // compiler needs this to be happy about initialization in callers
    if (rid_) {
        *rid_ = 0;
    }
    if (tag_) {
        *tag_ = 0;
    }
    if (weight_) {
        *weight_ = 0;
    }

    // binary search for our name
    lfs3_srid_t lower_rid = 0;
    lfs3_srid_t upper_rid = rbyd->weight;
    lfs3_scmp_t cmp;
    while (lower_rid < upper_rid) {
        lfs3_srid_t rid__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        lfs3_stag_t tag__ = lfs3_rbyd_lookupnext(lfs3, rbyd,
                // lookup ~middle rid, note we may end up in the middle
                // of a weighted rid with this
                lower_rid + (upper_rid-1-lower_rid)/2, 0,
                &rid__, &weight__, &data__);
        if (tag__ < 0) {
            LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
            return tag__;
        }

        // if we have no name, treat this rid as always lt
        if (lfs3_tag_suptype(tag__) != LFS3_TAG_NAME) {
            cmp = LFS3_CMP_LT;

        // compare names
        } else {
            cmp = lfs3_data_namecmp(lfs3, &data__, did, name, name_len);
            if (cmp < 0) {
                return cmp;
            }
        }

        // bisect search space
        if (cmp > LFS3_CMP_EQ) {
            upper_rid = rid__ - (weight__-1);

            // only keep track of best-match rids > our target if we haven't
            // seen an rid < our target
            if (lower_rid == 0) {
                if (rid_) {
                    *rid_ = rid__;
                }
                if (tag_) {
                    *tag_ = tag__;
                }
                if (weight_) {
                    *weight_ = weight__;
                }
                if (data_) {
                    *data_ = data__;
                }
            }

        } else if (cmp < LFS3_CMP_EQ) {
            lower_rid = rid__ + 1;

            // keep track of best-matching rid < our target
            if (rid_) {
                *rid_ = rid__;
            }
            if (tag_) {
                *tag_ = tag__;
            }
            if (weight_) {
                *weight_ = weight__;
            }
            if (data_) {
                *data_ = data__;
            }

        } else {
            // found a match?
            if (rid_) {
                *rid_ = rid__;
            }
            if (tag_) {
                *tag_ = tag__;
            }
            if (weight_) {
                *weight_ = weight__;
            }
            if (data_) {
                *data_ = data__;
            }
            return LFS3_CMP_EQ;
        }
    }

    // no match, return if found name was lt/gt expect
    //
    // this will always be lt unless all rids are gt
    return (lower_rid == 0) ? LFS3_CMP_GT : LFS3_CMP_LT;
}




/// B-tree operations ///

// create an empty btree
static void lfs3_btree_init(lfs3_btree_t *btree) {
    btree->weight = 0;
    btree->blocks[0] = -1;
    btree->trunk = 0;
}

// convenience operations
#ifndef LFS3_RDONLY
static inline void lfs3_btree_claim(lfs3_btree_t *btree) {
    // note we don't claim shrubs, as this would clobber shrub estimates
    if (!lfs3_rbyd_isshrub(btree)) {
        lfs3_rbyd_claim(btree);
    }
}
#endif

static inline int lfs3_btree_cmp(
        const lfs3_btree_t *a,
        const lfs3_btree_t *b) {
    return lfs3_rbyd_cmp(a, b);
}

// claim all btrees known to the system
//
// note this doesn't, and can't, include any stack allocated btrees
#ifndef LFS3_RDONLY
static void lfs3_mtree_claimbtree(lfs3_t *lfs3, const lfs3_btree_t *btree) {
    // claim the mtree
    if (&lfs3->mtree != btree
            && lfs3->mtree.blocks[0] == btree->blocks[0]) {
        lfs3_btree_claim(&lfs3->mtree);
    }

    // claim gbmap snapshots
    #ifdef LFS3_GBMAP
    if (&lfs3->gbmap.b != btree
            && lfs3->gbmap.b.blocks[0] == btree->blocks[0]) {
        lfs3_btree_claim(&lfs3->gbmap.b);
    }
    if (&lfs3->gbmap.b_p != btree
            && lfs3->gbmap.b_p.blocks[0] == btree->blocks[0]) {
        lfs3_btree_claim(&lfs3->gbmap.b_p);
    }
    #endif

    // claim file btrees/bshrubs
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && &((lfs3_file_t*)h)->bshrub != btree
                && ((lfs3_file_t*)h)->bshrub.blocks[0] == btree->blocks[0]) {
            lfs3_btree_claim(&((lfs3_file_t*)h)->bshrub);
        }
    }
}
#endif


// branch on-disk encoding
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_frombranch(const lfs3_rbyd_t *branch,
        uint8_t buffer[static LFS3_BRANCH_DSIZE]) {
    // block should not exceed 31-bits
    LFS3_ASSERT(branch->blocks[0] <= 0x7fffffff);
    // trunk should not exceed 28-bits
    LFS3_ASSERT(lfs3_rbyd_trunk(branch) <= 0x0fffffff);
    lfs3_ssize_t d = 0;

    lfs3_ssize_t d_ = lfs3_toleb128(branch->blocks[0], &buffer[d], 5);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    d_ = lfs3_toleb128(lfs3_rbyd_trunk(branch), &buffer[d], 4);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    lfs3_tole32(branch->cksum, &buffer[d]);
    d += 4;

    return LFS3_DATA_BUF(buffer, d);
}
#endif

static int lfs3_data_readbranch(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_bid_t weight,
        lfs3_rbyd_t *branch) {
    // setting eoff to 0 here will trigger asserts if we try to append
    // without fetching first
    #ifndef LFS3_RDONLY
    branch->eoff = 0;
    #endif

    branch->weight = weight;

    int err = lfs3_data_readleb128(lfs3, data, &branch->blocks[0]);
    if (err) {
        return err;
    }

    err = lfs3_data_readlleb128(lfs3, data, &branch->trunk);
    if (err) {
        return err;
    }

    err = lfs3_data_readle32(lfs3, data, &branch->cksum);
    if (err) {
        return err;
    }

    return 0;
}

static int lfs3_data_fetchbranch(lfs3_t *lfs3,
        lfs3_data_t *data, lfs3_bid_t weight,
        lfs3_rbyd_t *branch) {
    // decode branch and fetch
    int err = lfs3_data_readbranch(lfs3, data, weight,
            branch);
    if (err) {
        return err;
    }

    // checking fetches?
    #ifdef LFS3_CKFETCHES
    if (lfs3_m_isckfetches(lfs3->flags)) {
        int err = lfs3_rbyd_ckfetch(lfs3, branch,
                branch->blocks[0], lfs3_rbyd_trunk(branch),
                branch->cksum);
        if (err) {
            return err;
        }
        LFS3_ASSERT(branch->weight == weight);
    }
    #endif

    return 0;
}


// btree on-disk encoding
//
// this is the same as the branch on-disk econding, but prefixed with the
// btree's weight
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_frombtree(const lfs3_btree_t *btree,
        uint8_t buffer[static LFS3_BTREE_DSIZE]) {
    // weight should not exceed 31-bits
    LFS3_ASSERT(btree->weight <= 0x7fffffff);
    lfs3_ssize_t d = 0;

    lfs3_ssize_t d_ = lfs3_toleb128(btree->weight, &buffer[d], 5);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    lfs3_data_t data = lfs3_data_frombranch(btree, &buffer[d]);
    d += lfs3_data_size(&data);

    return LFS3_DATA_BUF(buffer, d);
}
#endif

static int lfs3_data_readbtree(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_btree_t *btree) {
    lfs3_bid_t weight;
    int err = lfs3_data_readleb128(lfs3, data, &weight);
    if (err) {
        return err;
    }

    err = lfs3_data_readbranch(lfs3, data, weight, btree);
    if (err) {
        return err;
    }

    return 0;
}

static int lfs3_data_fetchbtree(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_btree_t *btree) {
    // decode btree and fetch
    int err = lfs3_data_readbtree(lfs3, data,
            btree);
    if (err) {
        return err;
    }

    // checking fetches?
    #ifdef LFS3_CKFETCHES
    if (lfs3_m_isckfetches(lfs3->flags)) {
        lfs3_bid_t weight = btree->weight;
        int err = lfs3_rbyd_ckfetch(lfs3, btree,
                btree->blocks[0], lfs3_rbyd_trunk(btree),
                btree->cksum);
        if (err) {
            return err;
        }
        LFS3_ASSERT(btree->weight == weight);
    }
    #endif

    #ifdef LFS3_DBGBTREEFETCHES
    LFS3_DEBUG("Fetched btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            btree->blocks[0], lfs3_rbyd_trunk(btree),
            btree->weight,
            btree->cksum);
    #endif
    return 0;
}


// core btree operations

// lookup rbyd/rid containing a given bid
static lfs3_stag_t lfs3_btree_lookupnext_(lfs3_t *lfs3,
        const lfs3_btree_t *btree,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_rbyd_t *rbyd_, lfs3_srid_t *rid_,
        lfs3_bid_t *weight_, lfs3_data_t *data_) {
    // descend down the btree looking for our bid
    lfs3_bid_t bid__ = btree->weight-1;
    *rbyd_ = *btree;
    while (true) {
        // lookup our bid in the rbyd
        lfs3_srid_t rid__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        lfs3_stag_t tag__ = lfs3_rbyd_lookupnext(lfs3, rbyd_,
                bid - (bid__-(rbyd_->weight-1)), 0,
                &rid__, &weight__, &data__);
        if (tag__ < 0) {
            return tag__;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            tag__ = lfs3_rbyd_lookup(lfs3, rbyd_, rid__, LFS3_TAG_BRANCH,
                    &data__);
            if (tag__ < 0) {
                LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
                return tag__;
            }
        }

        // found another branch
        if (tag__ == LFS3_TAG_BRANCH) {
            // adjust bid__ with subtree's weight
            bid__ = (bid__-(rbyd_->weight-1)) + rid__;

            // fetch the next branch
            int err = lfs3_data_fetchbranch(lfs3, &data__, weight__,
                    rbyd_);
            if (err) {
                return err;
            }

        // found our bid
        } else {
            // TODO how many of these should be conditional?
            if (bid_) {
                *bid_ = (bid__-(rbyd_->weight-1)) + rid__;
            }
            if (rid_) {
                *rid_ = rid__;
            }
            if (weight_) {
                *weight_ = weight__;
            }
            if (data_) {
                *data_ = data__;
            }
            return tag__;
        }
    }
}

static lfs3_stag_t lfs3_btree_lookupnext(lfs3_t *lfs3,
        const lfs3_btree_t *btree,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    lfs3_rbyd_t rbyd__;
    return lfs3_btree_lookupnext_(lfs3, btree, bid,
            bid_, &rbyd__, NULL, weight_, data_);
}

// lfs3_btree_lookup assumes a known bid, matching lfs3_rbyd_lookup's
// behavior, if you don't care about the exact bid either first call
// lfs3_btree_lookupnext
static lfs3_stag_t lfs3_btree_lookup(lfs3_t *lfs3,
        const lfs3_btree_t *btree,
        lfs3_bid_t bid, lfs3_tag_t tag,
        lfs3_data_t *data_) {
    // lookup rbyd in btree
    lfs3_bid_t bid__;
    lfs3_rbyd_t rbyd__;
    lfs3_srid_t rid__;
    lfs3_stag_t tag__ = lfs3_btree_lookupnext_(lfs3, btree, bid,
            &bid__, &rbyd__, &rid__, NULL, NULL);
    if (tag__ < 0) {
        return tag__;
    }

    // lookup finds the next-smallest bid, all we need to do is fail
    // if it picks up the wrong bid
    if (bid__ != bid) {
        return LFS3_ERR_NOENT;
    }

    // lookup tag in rbyd
    return lfs3_rbyd_lookup(lfs3, &rbyd__, rid__, tag,
            data_);
}

#ifndef LFS3_RDONLY
static int lfs3_btree_parent(lfs3_t *lfs3, const lfs3_btree_t *btree,
        lfs3_bid_t bid, const lfs3_rbyd_t *child,
        lfs3_rbyd_t *parent_, lfs3_srid_t *pid_) {
    // we should only call this when we actually have parents
    LFS3_ASSERT(bid <= btree->weight);
    LFS3_ASSERT(lfs3_rbyd_cmp(btree, child) != 0);

    // descend down the btree looking for our bid
    lfs3_bid_t bid__ = btree->weight-1;
    *parent_ = *btree;
    while (true) {
        // each branch is a pair of optional name + on-disk structure
        lfs3_srid_t rid__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        lfs3_stag_t tag__ = lfs3_rbyd_lookupnext(lfs3, parent_,
                lfs3_min(bid, btree->weight-1)
                    - (bid__-(parent_->weight-1)), 0,
                &rid__, &weight__, &data__);
        if (tag__ < 0) {
            LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
            return tag__;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            tag__ = lfs3_rbyd_lookup(lfs3, parent_, rid__, LFS3_TAG_BRANCH,
                    &data__);
            if (tag__ < 0) {
                LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
                return tag__;
            }
        }

        // didn't find our child?
        if (tag__ != LFS3_TAG_BRANCH) {
            return LFS3_ERR_NOENT;
        }

        // adjust bid__ with subtree's weight
        bid__ = (bid__-(parent_->weight-1)) + rid__;

        // fetch the next branch
        lfs3_rbyd_t child__;
        int err = lfs3_data_readbranch(lfs3, &data__, weight__, &child__);
        if (err) {
            return err;
        }

        // found our child?
        if (lfs3_rbyd_cmp(&child__, child) == 0) {
            // TODO how many of these should be conditional?
            if (pid_) {
                *pid_ = rid__;
            }
            return 0;
        }

        *parent_ = child__;
    }
}
#endif


// extra state needed for non-terminating lfs3_btree_commit__ calls
#ifndef LFS3_RDONLY
typedef struct lfs3_bcommit {
    // pending commit, this is updated as lfs3_btree_commit__ recurses
    lfs3_bid_t bid;
    const lfs3_rattr_t *rattrs;
    lfs3_ssize_t shestimate;

    // scratch space for lfs3_btree_commit__ state that needs to persist
    // until the root is committed
    lfs3_rattr_t rscratch[16];
} lfs3_bcommit_t;
#endif

// needed in lfs3_btree_commit__
static inline uint32_t lfs3_rev_btree(lfs3_t *lfs3);

// core btree algorithm
//
// this commits up to the root, but stops if:
// 1. we need a new root    => LFS3_ERR_RANGE
// 2. we hit a shrub root   => LFS3_ERR_EXIST
//
// ---
//
// note! all non-bid-0 name updates must be via splits!
//
// This is because our btrees contain vestigial names, i.e. our inner
// nodes may contain names no longer in the tree. This simplifies
// lfs3_btree_commit__, but means insert-before-bid+1 is _not_ the same
// as insert-after-bid when named btrees are involved:
//
//     .-----f-----.    insert-after-d     .-------f-----.
//   .-b--.     .--j-.        =>         .-b---.      .--j-.
//   |   .-.   .-.   |                   |   .---.   .-.   |
//   a   c d   h i   k                   a   c d e   h i   k
//                                               ^
//                      insert-before-h
//                            =>           .-----f-------.
//                                       .-b--.      .---j-.
//                                       |   .-.   .---.   |
//                                       a   c d   g h i   k
//                                                 ^
//
// The problem is that lfs3_btree_commit__ needs to find the same leaf
// rbyd as lfs3_btree_namelookup, and potentially insert-before the
// first rid or insert-after the last rid.
//
// Instead of separate insert-before/after flags, we make the first tag
// in a commit insert-before, and all following non-grow tags
// insert-after (splits).
//
#ifndef LFS3_RDONLY
static int lfs3_btree_commit__(lfs3_t *lfs3,
        lfs3_btree_t *btree_, lfs3_btree_t *btree,
        lfs3_rbyd_t *rbyd, lfs3_srid_t rid,
        lfs3_bcommit_t *bcommit) {
    LFS3_ASSERT(bcommit->bid <= btree->weight);

    // before committing, claim any matching btrees we know about
    //
    // is this overkill? probably, but hey, better safe than sorry,
    // claiming things here reduces the chance of forgetting to claim
    // things in above layers
    lfs3_mtree_claimbtree(lfs3, btree);

    // tail-recursively commit to btree
    lfs3_rbyd_t *const child = rbyd;
    lfs3_rbyd_t *const child_ = btree_;
    while (true) {
        // we will always need our parent, so go ahead and find it
        lfs3_rbyd_t parent = {.trunk=0, .weight=0};
        lfs3_srid_t pid = 0;
        // new root? shrub root? yield the final root commit to
        // higher-level btree/bshrub logic
        if (!lfs3_rbyd_trunk(child) || lfs3_rbyd_isshrub(child)) {
            bcommit->bid = rid;
            return (!lfs3_rbyd_trunk(child))
                    ? LFS3_ERR_RANGE
                    : LFS3_ERR_EXIST;

        // are we root?
        } else if (child->blocks[0] == btree->blocks[0]) {
            // mark btree as unfetched in case of failure, our btree rbyd and
            // root rbyd can diverge if there's a split, but we would have
            // marked the old root as unfetched earlier anyways
            lfs3_btree_claim(btree);

        // need to lookup child's parent
        } else {
            int err = lfs3_btree_parent(lfs3, btree, bcommit->bid, child,
                    &parent, &pid);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }
        }

        // fetch our rbyd so we can mutate it
        //
        // note that some paths lead this to being a newly allocated rbyd,
        // these will fail to fetch so we need to check that this rbyd is
        // unfetched
        //
        // a funny benefit is we cache the root of our btree this way
        int err = lfs3_rbyd_mkfetched(lfs3, child);
        if (err) {
            return err;
        }

        // is rbyd erased? can we sneak our commit into any remaining
        // erased bytes? note that the btree trunk field prevents this from
        // interacting with other references to the rbyd
        *child_ = *child;
        err = lfs3_rbyd_commit(lfs3, child_, rid,
                bcommit->rattrs);
        if (err) {
            if (err == LFS3_ERR_RANGE || err == LFS3_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

    recurse:;
        // propagate successful commits

        // done?
        if (!lfs3_rbyd_trunk(&parent)) {
            // update the root
            // (note btree_ == child_)
            return 0;
        }

        // is our parent the root and is the root degenerate?
        if (child->weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            // (note btree_ == child_)
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        //
        // note that since we defer merges to compaction time, we can
        // end up removing an rbyd here
        bcommit->bid -= pid - (child->weight-1);
        bcommit->rattrs = bcommit->rscratch;
        bcommit->shestimate = 0;
        lfs3_rattr_t *r = bcommit->rscratch;
        // drop child?
        if (child_->weight == 0) {
            // drop child
            *r++ = LFS3_RATTR(LFS3_tag_RM, -2, 0);
            *r++ = LFS3_RATTR_WEIGHT(-child->weight);
            bcommit->shestimate -= lfs3->rattr_estimate + LFS3_BRANCH_DSIZE;
        } else {
            // update child
            *r++ = LFS3_RATTR(LFS3_TAG_BRANCH, 0, 3, LFS3_FROM_BRANCH);
            *r++ = LFS3_RATTR_ARG(child_->blocks[0]);
            *r++ = LFS3_RATTR_ARG(child_->trunk);
            *r++ = LFS3_RATTR_ARG(child_->cksum);
            if (child_->weight != child->weight) {
                *r++ = LFS3_RATTR(LFS3_tag_GROW, -2, 0);
                *r++ = LFS3_RATTR_ARG(-child->weight + child_->weight);
            }
        }
        *r++ = LFS3_RATTR_NULL;
        LFS3_ASSERT((lfs3_size_t)(r-bcommit->rscratch)
                <= sizeof(bcommit->rscratch)/sizeof(lfs3_rattr_t));

        // recurse!
        *child = parent;
        rid = pid;
        continue;

    compact:;
        // estimate our compacted size
        lfs3_srid_t split_rid;
        lfs3_ssize_t estimate = lfs3_rbyd_estimate(lfs3, child, -1, -1,
                &split_rid);
        if (estimate < 0) {
            return estimate;
        }

        // are we too big? need to split?
        if ((lfs3_size_t)estimate > lfs3->cfg->block_size/2) {
            // need to split
            goto split;
        }

        // before we compact, can we merge with our siblings?
        lfs3_rbyd_t sibling;
        if ((lfs3_size_t)estimate <= lfs3->cfg->block_size/4
                // no parent? can't merge
                && lfs3_rbyd_trunk(&parent)) {
            // try the right sibling
            if (pid+1 < (lfs3_srid_t)parent.weight) {
                // try looking up the sibling
                lfs3_srid_t sibling_rid;
                lfs3_rid_t sibling_weight;
                lfs3_data_t sibling_data;
                lfs3_stag_t sibling_tag = lfs3_rbyd_lookupnext(lfs3, &parent,
                        pid+1, 0,
                        &sibling_rid, &sibling_weight, &sibling_data);
                if (sibling_tag < 0) {
                    LFS3_ASSERT(sibling_tag != LFS3_ERR_NOENT);
                    return sibling_tag;
                }

                // if we found a bname, lookup the branch
                if (sibling_tag == LFS3_TAG_BNAME) {
                    sibling_tag = lfs3_rbyd_lookup(lfs3, &parent,
                            sibling_rid, LFS3_TAG_BRANCH,
                            &sibling_data);
                    if (sibling_tag < 0) {
                        LFS3_ASSERT(sibling_tag != LFS3_ERR_NOENT);
                        return sibling_tag;
                    }
                }

                LFS3_ASSERT(sibling_tag == LFS3_TAG_BRANCH);
                err = lfs3_data_fetchbranch(lfs3,
                        &sibling_data, sibling_weight,
                        &sibling);
                if (err) {
                    return err;
                }

                // estimate if our sibling will fit
                lfs3_ssize_t sibling_estimate = lfs3_rbyd_estimate(lfs3,
                        &sibling, -1, -1,
                        NULL);
                if (sibling_estimate < 0) {
                    return sibling_estimate;
                }

                // fits? try to merge
                if ((lfs3_size_t)(estimate + sibling_estimate)
                        < lfs3->cfg->block_size/2) {
                    goto merge;
                }
            }

            // try the left sibling
            if (pid-(lfs3_srid_t)child->weight >= 0) {
                // try looking up the sibling
                lfs3_srid_t sibling_rid;
                lfs3_rid_t sibling_weight;
                lfs3_data_t sibling_data;
                lfs3_stag_t sibling_tag = lfs3_rbyd_lookupnext(lfs3, &parent,
                        pid-child->weight, 0,
                        &sibling_rid, &sibling_weight, &sibling_data);
                if (sibling_tag < 0) {
                    LFS3_ASSERT(sibling_tag != LFS3_ERR_NOENT);
                    return sibling_tag;
                }

                // if we found a bname, lookup the branch
                if (sibling_tag == LFS3_TAG_BNAME) {
                    sibling_tag = lfs3_rbyd_lookup(lfs3, &parent,
                            sibling_rid, LFS3_TAG_BRANCH,
                            &sibling_data);
                    if (sibling_tag < 0) {
                        LFS3_ASSERT(sibling_tag != LFS3_ERR_NOENT);
                        return sibling_tag;
                    }
                }

                LFS3_ASSERT(sibling_tag == LFS3_TAG_BRANCH);
                err = lfs3_data_fetchbranch(lfs3,
                        &sibling_data, sibling_weight,
                        &sibling);
                if (err) {
                    return err;
                }

                // estimate if our sibling will fit
                lfs3_ssize_t sibling_estimate = lfs3_rbyd_estimate(lfs3,
                        &sibling, -1, -1,
                        NULL);
                if (sibling_estimate < 0) {
                    return sibling_estimate;
                }

                // fits? try to merge
                if ((lfs3_size_t)(estimate + sibling_estimate)
                        < lfs3->cfg->block_size/2) {
                    // if we're merging our left sibling, swap our rbyds
                    // so our sibling is on the right
                    bcommit->bid -= sibling.weight;
                    rid += sibling.weight;
                    pid -= child->weight;

                    *child_ = sibling;
                    sibling = *child;
                    *child = *child_;

                    goto merge;
                }
            }
        }

    relocate:;
        // allocate a new rbyd
        err = lfs3_rbyd_alloc(lfs3, child_);
        if (err) {
            return err;
        }

        // try to compact
        err = lfs3_rbyd_compact(lfs3, child_, child, -1, -1);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // append any pending rattrs, it's up to upper
        // layers to make sure these always fit
        err = lfs3_rbyd_commit(lfs3, child_, rid,
                bcommit->rattrs);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        goto recurse;

    split:;
        // we should have something to split here
        LFS3_ASSERT(split_rid > 0
                && split_rid < (lfs3_srid_t)child->weight);

    split_relocate_l:;
        // allocate a new rbyd
        err = lfs3_rbyd_alloc(lfs3, child_);
        if (err) {
            return err;
        }

        // copy over tags < split_rid
        err = lfs3_rbyd_compact(lfs3, child_, child, -1, split_rid);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

        // append pending rattrs < split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfs3_rbyd_appendrattrs(lfs3, child_, rid, -1, split_rid,
                bcommit->rattrs);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

        // finalize commit
        err = lfs3_rbyd_appendcksum(lfs3, child_);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

    split_relocate_r:;
        // allocate a sibling
        err = lfs3_rbyd_alloc(lfs3, &sibling);
        if (err) {
            return err;
        }

        // copy over tags >= split_rid
        err = lfs3_rbyd_compact(lfs3, &sibling, child, split_rid, -1);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }

        // append pending rattrs >= split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfs3_rbyd_appendrattrs(lfs3, &sibling, rid, split_rid, -1,
                bcommit->rattrs);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }

        // finalize commit
        err = lfs3_rbyd_appendcksum(lfs3, &sibling);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }

        // did one of our siblings drop to zero? yes this can happen! revert
        // to a normal commit in that case
        if (child_->weight == 0 || sibling.weight == 0) {
            if (child_->weight == 0) {
                *child_ = sibling;
            }
            goto recurse;
        }

    split_recurse:;
        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending rattrs in case
        // they introduce a new name!
        lfs3_data_t split_name;
        lfs3_stag_t split_tag = lfs3_rbyd_lookupnext(lfs3, &sibling, 0, 0,
                NULL, NULL, &split_name);
        if (split_tag < 0) {
            LFS3_ASSERT(split_tag != LFS3_ERR_NOENT);
            return split_tag;
        }

        // prepare commit to parent, tail recursing upwards
        LFS3_ASSERT(child_->weight > 0);
        LFS3_ASSERT(sibling.weight > 0);
        // don't worry about bid if new root, we discard it anyways
        bcommit->bid -= pid - (child->weight-1);
        bcommit->rattrs = bcommit->rscratch;
        bcommit->shestimate = 0;
        r = bcommit->rscratch;

        // new root?
        if (!lfs3_rbyd_trunk(&parent)) {
            // new child
            *r++ = LFS3_RATTR(LFS3_TAG_BRANCH, -2, 3, LFS3_FROM_BRANCH);
            *r++ = LFS3_RATTR_WEIGHT(+child_->weight);
            *r++ = LFS3_RATTR_ARG(child_->blocks[0]);
            *r++ = LFS3_RATTR_ARG(child_->trunk);
            *r++ = LFS3_RATTR_ARG(child_->cksum);
            bcommit->shestimate += lfs3->rattr_estimate + LFS3_BRANCH_DSIZE;
        // split root?
        } else {
            // update child
            *r++ = LFS3_RATTR(LFS3_TAG_BRANCH, 0, 3, LFS3_FROM_BRANCH);
            *r++ = LFS3_RATTR_ARG(child_->blocks[0]);
            *r++ = LFS3_RATTR_ARG(child_->trunk);
            *r++ = LFS3_RATTR_ARG(child_->cksum);
            if (child_->weight != child->weight) {
                *r++ = LFS3_RATTR(LFS3_tag_GROW, -2, 0);
                *r++ = LFS3_RATTR_WEIGHT(-child->weight + child_->weight);
            }
        }
        // new sibling
        *r++ = LFS3_RATTR(LFS3_TAG_BRANCH, -2, 3, LFS3_FROM_BRANCH);
        *r++ = LFS3_RATTR_WEIGHT(+sibling.weight);
        *r++ = LFS3_RATTR_ARG(sibling.blocks[0]);
        *r++ = LFS3_RATTR_ARG(sibling.trunk);
        *r++ = LFS3_RATTR_ARG(sibling.cksum);
        if (lfs3_tag_suptype(split_tag) == LFS3_TAG_NAME) {
            *r++ = LFS3_RATTR(LFS3_TAG_BNAME, 0, 3, LFS3_FROM_GRAFT);
            *(lfs3_data_t*)r = split_name;
            r += 3;
        }
        // we don't care about the name cost in our estimate only
        // because bshrubs never include names
        bcommit->shestimate += lfs3->rattr_estimate + LFS3_BRANCH_DSIZE;
        *r++ = LFS3_RATTR_NULL;
        LFS3_ASSERT((lfs3_size_t)(r-bcommit->rscratch)
                <= sizeof(bcommit->rscratch)/sizeof(lfs3_rattr_t));

        // recurse!
        *child = parent;
        rid = pid;
        continue;

    merge:;
    merge_relocate:;
        // allocate a new rbyd
        err = lfs3_rbyd_alloc(lfs3, child_);
        if (err) {
            return err;
        }

        // merge the siblings together
        err = lfs3_rbyd_appendcompactrbyd(lfs3, child_, child, -1, -1);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        err = lfs3_rbyd_appendcompactrbyd(lfs3, child_, &sibling, -1, -1);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        err = lfs3_rbyd_appendcompaction(lfs3, child_, 0);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        // append any pending rattrs, it's up to upper
        // layers to make sure these always fit
        err = lfs3_rbyd_commit(lfs3, child_, rid,
                bcommit->rattrs);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

    merge_recurse:;
        // we must have a parent at this point, but is our parent the root
        // and is the root degenerate?
        LFS3_ASSERT(lfs3_rbyd_trunk(&parent));
        if (child->weight+sibling.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            // (note btree_ == child_)
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        LFS3_ASSERT(child_->weight > 0);
        bcommit->bid -= pid - (child->weight-1);
        bcommit->rattrs = bcommit->rscratch;
        bcommit->shestimate = 0;
        r = bcommit->rscratch;
        // merge sibling
        *r++ = LFS3_RATTR(LFS3_tag_RM, -2, 0);
        *r++ = LFS3_RATTR_WEIGHT(-sibling.weight);
        bcommit->shestimate -= lfs3->rattr_estimate + LFS3_BRANCH_DSIZE;
        // update child
        *r++ = LFS3_RATTR(LFS3_TAG_BRANCH, 0, 3, LFS3_FROM_BRANCH);
        *r++ = LFS3_RATTR_ARG(child_->blocks[0]);
        *r++ = LFS3_RATTR_ARG(child_->trunk);
        *r++ = LFS3_RATTR_ARG(child_->cksum);
        if (child_->weight != child->weight) {
            *r++ = LFS3_RATTR(LFS3_tag_GROW, -2, 0);
            *r++ = LFS3_RATTR_ARG(-child->weight + child_->weight);
        }
        *r++ = LFS3_RATTR_NULL;
        LFS3_ASSERT((lfs3_size_t)(r-bcommit->rscratch)
                <= sizeof(bcommit->rscratch)/sizeof(lfs3_rattr_t));

        // recurse!
        *child = parent;
        rid = pid + sibling.weight;
        continue;
    }
}
#endif

// commit/alloc a new btree root
#ifndef LFS3_RDONLY
static int lfs3_btree_commitroot_(lfs3_t *lfs3,
        lfs3_btree_t *btree_, lfs3_btree_t *btree,
        const lfs3_bcommit_t *bcommit) {
relocate:;
    int err = lfs3_rbyd_alloc(lfs3, btree_);
    if (err) {
        return err;
    }

    // bshrubs may call this just to migrate rattrs to a btree
    if (lfs3_rbyd_isshrub(btree)) {
        err = lfs3_rbyd_compact(lfs3, btree_, btree, -1, -1);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }
    }

    err = lfs3_rbyd_commit(lfs3, btree_,
            bcommit->bid, bcommit->rattrs);
    if (err) {
        LFS3_ASSERT(err != LFS3_ERR_RANGE);
        // bad prog? try another block
        if (err == LFS3_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    return 0;
}
#endif

// commit to a specific rbyd in a btree, this is atomic
#ifndef LFS3_RDONLY
static int lfs3_btree_commit_(lfs3_t *lfs3, lfs3_btree_t *btree,
        lfs3_bid_t bid, lfs3_rbyd_t *rbyd, lfs3_srid_t rid,
        const lfs3_rattr_t *rattrs) {
    LFS3_ASSERT(bid <= btree->weight);

    // try to commit to the btree
    lfs3_btree_t btree_;
    lfs3_bcommit_t bcommit; // do _not_ fully init this
    bcommit.bid = bid;
    bcommit.rattrs = rattrs;
    int err = lfs3_btree_commit__(lfs3, &btree_, btree,
            rbyd, rid, &bcommit);
    if (err && err != LFS3_ERR_RANGE) {
        LFS3_ASSERT(err != LFS3_ERR_EXIST);
        return err;
    }

    // needs a new root?
    if (err == LFS3_ERR_RANGE) {
        err = lfs3_btree_commitroot_(lfs3, &btree_, btree,
                &bcommit);
        if (err) {
            return err;
        }
    }

    // update the btree
    *btree = btree_;

    LFS3_ASSERT(lfs3_rbyd_trunk(btree));
    #ifdef LFS3_DBGBTREECOMMITS
    LFS3_DEBUG("Committed btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            btree->blocks[0], lfs3_rbyd_trunk(btree),
            btree->weight,
            btree->cksum);
    #endif
    return 0;
}
#endif

// commit to a btree, this is atomic
#ifndef LFS3_RDONLY
static int lfs3_btree_commit(lfs3_t *lfs3, lfs3_btree_t *btree,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs) {
    LFS3_ASSERT(bid <= btree->weight);

    // lookup which leaf our bid resides
    lfs3_rbyd_t rbyd = *btree;
    lfs3_srid_t rid = bid;
    if (btree->weight > 0) {
        lfs3_bid_t bid__;
        lfs3_srid_t rid__;
        lfs3_stag_t tag__ = lfs3_btree_lookupnext_(lfs3, btree,
                // for lfs3_btree_commit__ operations to work out, we
                // need to limit our bid to an rid in the tree, which
                // is what this min is doing
                lfs3_min(bid, btree->weight-1),
                &bid__, &rbyd, &rid__, NULL, NULL);
        if (tag__ < 0) {
            LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
            return tag__;
        }

        // adjust rid
        LFS3_ASSERT(bid >= bid__ - rid__);
        rid = bid - (bid__ - rid__);
    }

    // tail-recursively commit to the btree
    return lfs3_btree_commit_(lfs3, btree, bid, &rbyd, rid, rattrs);
}
#endif

// compact a specific rbyd, this is atomic
#ifndef LFS3_RDONLY
static int lfs3_btree_compact_(lfs3_t *lfs3, lfs3_btree_t *btree,
        lfs3_bid_t bid, lfs3_rbyd_t *rbyd) {
    LFS3_ASSERT(bid < btree->weight);

    // the easiest way to do this is to just mark the rbyd as unerased
    // and call lfs3_btree_commit_
    rbyd->eoff = -1;
    return lfs3_btree_commit_(lfs3, btree, bid, rbyd, rbyd->weight-1,
            (const lfs3_rattr_t[]){
                LFS3_RATTR_NULL});
}
#endif

// lookup in a btree by name
static lfs3_scmp_t lfs3_btree_namelookup_(lfs3_t *lfs3,
        const lfs3_btree_t *btree,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_bid_t *bid_, lfs3_rbyd_t *rbyd_, lfs3_srid_t *rid_,
        lfs3_tag_t *tag_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    // an empty tree?
    if (btree->weight == 0) {
        return LFS3_ERR_NOENT;
    }

    // compiler needs this to be happy about initialization in callers
    if (bid_) {
        *bid_ = 0;
    }
    if (rid_) {
        *rid_ = 0;
    }
    if (tag_) {
        *tag_ = 0;
    }
    if (weight_) {
        *weight_ = 0;
    }

    // descend down the btree looking for our name
    lfs3_bid_t bid__ = btree->weight-1;
    *rbyd_ = *btree;
    while (true) {
        // lookup our name in the rbyd via binary search
        lfs3_srid_t rid__;
        lfs3_stag_t tag__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        lfs3_scmp_t cmp = lfs3_rbyd_namelookup(lfs3, rbyd_,
                did, name, name_len,
                &rid__, (lfs3_tag_t*)&tag__, &weight__, &data__);
        if (cmp < 0) {
            LFS3_ASSERT(cmp != LFS3_ERR_NOENT);
            return cmp;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            tag__ = lfs3_rbyd_lookup(lfs3, rbyd_, rid__,
                    LFS3_tag_MASK8 | LFS3_TAG_STRUCT,
                    &data__);
            if (tag__ < 0) {
                LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
                return tag__;
            }
        }

        // found another branch
        if (tag__ == LFS3_TAG_BRANCH) {
            // adjust bid__ with subtree's weight
            bid__ = (bid__-(rbyd_->weight-1)) + rid__;

            // fetch the next branch
            int err = lfs3_data_fetchbranch(lfs3, &data__, weight__,
                    rbyd_);
            if (err) {
                return err;
            }

        // found our rid
        } else {
            // TODO how many of these should be conditional?
            if (bid_) {
                *bid_ = (bid__-(rbyd_->weight-1)) + rid__;
            }
            if (rid_) {
                *rid_ = rid__;
            }
            if (tag_) {
                *tag_ = tag__;
            }
            if (weight_) {
                *weight_ = weight__;
            }
            if (data_) {
                *data_ = data__;
            }
            return cmp;
        }
    }
}

static lfs3_scmp_t lfs3_btree_namelookup(lfs3_t *lfs3,
        const lfs3_btree_t *btree,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_bid_t *bid_, lfs3_tag_t *tag_, lfs3_bid_t *weight_,
        lfs3_data_t *data_) {
    lfs3_rbyd_t rbyd__;
    return lfs3_btree_namelookup_(lfs3, btree, did, name, name_len,
            bid_, &rbyd__, NULL, tag_, weight_, data_);
}

// incremental btree traversal
//
// unlike lfs3_btree_lookupnext, this includes inner btree nodes

static void lfs3_btrv_init(lfs3_btrv_t *btrv) {
    btrv->bid = -1;
}

// seek to a specific bid, restarting from the root
//
// useful for resuming traversal after mutating the btree
static void lfs3_btrv_seek(lfs3_btrv_t *btrv, lfs3_sbid_t bid) {
    btrv->bid = bid;
    btrv->rid = -1;
}

static lfs3_stag_t lfs3_btree_traverse(lfs3_t *lfs3,
        const lfs3_btree_t *btree,
        lfs3_btrv_t *btrv,
        lfs3_sbid_t *bid_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    // restart from the root?
    if (btrv->bid == -1
            // end of rbyd? rid=-1?
            || (lfs3_rid_t)btrv->rid >= btrv->rbyd.weight
            // rbyd is a shrub
            //
            // we do this unconditionally when rbyd is a shrub to avoid
            // bshrub root traversals falling out-of-sync under mutation
            || lfs3_rbyd_isshrub(&btrv->rbyd)) {
        // end of traversal?
        if (btrv->bid >= (lfs3_sbid_t)btree->weight) {
            return LFS3_ERR_NOENT;
        }

        // restart from the root
        btrv->rbyd = *btree;
        btrv->rid = btrv->bid;

        // explicitly traverse the root even if weight=0
        if (btrv->bid == -1) {
            btrv->bid += 1;
            btrv->rid += 1;

            // unless we don't even have a root yet
            if (lfs3_rbyd_trunk(btree) != 0
                    // or are a shrub
                    && !lfs3_rbyd_isshrub(btree)) {
                if (bid_) {
                    *bid_ = btree->weight-1;
                }
                if (weight_) {
                    *weight_ = btree->weight;
                }
                if (data_) {
                    // note we point data_ at the actual root here! this
                    // avoids redundant fetches if the traversal fetches
                    // btree nodes
                    data_->u.buffer = (const uint8_t*)btree;
                }
                return LFS3_TAG_BRANCH;
            }
        }
    }

    // descend down the tree
    while (true) {
        lfs3_srid_t rid__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        lfs3_stag_t tag__ = lfs3_rbyd_lookupnext(lfs3, &btrv->rbyd,
                btrv->rid, 0,
                &rid__, &weight__, &data__);
        if (tag__ < 0) {
            return tag__;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            tag__ = lfs3_rbyd_lookup(lfs3, &btrv->rbyd,
                    rid__, LFS3_TAG_BRANCH,
                    &data__);
            if (tag__ < 0) {
                LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
                return tag__;
            }
        }

        // found another branch
        if (tag__ == LFS3_TAG_BRANCH) {
            // adjust rid with subtree's weight
            btrv->rid -= (rid__ - (weight__-1));

            // fetch the next branch
            int err = lfs3_data_fetchbranch(lfs3, &data__, weight__,
                    &btrv->rbyd);
            if (err) {
                return err;
            }

            // return inner btree nodes if this is the first time we've
            // seen them
            if (btrv->rid == 0) {
                if (bid_) {
                    *bid_ = btrv->bid + (rid__ - btrv->rid);
                }
                if (weight_) {
                    *weight_ = weight__;
                }
                if (data_) {
                    data_->u.buffer = (const uint8_t*)&btrv->rbyd;
                }
                return LFS3_TAG_BRANCH;
            }

        // found our bid
        } else {
            // move on to the next rid
            //
            // note this effectively traverses a full leaf without redoing
            // the btree walk
            lfs3_bid_t bid__ = btrv->bid + (rid__ - btrv->rid);
            btrv->bid = bid__ + 1;
            btrv->rid = rid__ + 1;

            if (bid_) {
                *bid_ = bid__;
            }
            if (weight_) {
                *weight_ = weight__;
            }
            if (data_) {
                *data_ = data__;
            }
            return tag__;
        }
    }
}




/// B-shrub operations ///

// shrub things

// helper functions
static inline bool lfs3_shrub_isshrub(const lfs3_shrub_t *shrub) {
    return lfs3_rbyd_isshrub(shrub);
}

static inline lfs3_size_t lfs3_shrub_trunk(const lfs3_shrub_t *shrub) {
    return lfs3_rbyd_trunk(shrub);
}

#ifndef LFS3_RDONLY
static inline bool lfs3_shrub_isfetched(const lfs3_shrub_t *shrub) {
    return lfs3_rbyd_isfetched(shrub);
}
#endif

static inline lfs3_file_t *lfs3_shrub_file(lfs3_shrub_t *shrub) {
    lfs3_file_t f;
    return (lfs3_file_t*)((uint8_t*)shrub
            - ((uint8_t*)&f.bshrub - (uint8_t*)&f));
}

static inline int lfs3_shrub_cmp(
        const lfs3_shrub_t *a,
        const lfs3_shrub_t *b) {
    return lfs3_rbyd_cmp(a, b);
}

// shrub on-disk encoding
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_fromshrub(const lfs3_shrub_t *shrub,
        uint8_t buffer[static LFS3_SHRUB_DSIZE]) {
    // shrub trunks should never be null
    LFS3_ASSERT(lfs3_shrub_trunk(shrub) != 0);
    // weight should not exceed 31-bits
    LFS3_ASSERT(shrub->weight <= 0x7fffffff);
    // trunk should not exceed 28-bits
    LFS3_ASSERT(lfs3_shrub_trunk(shrub) <= 0x0fffffff);
    lfs3_ssize_t d = 0;

    // just write the trunk and weight, the rest of the rbyd is implied
    // by the containing mdir
    lfs3_ssize_t d_ = lfs3_toleb128(shrub->weight, &buffer[d], 5);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    d_ = lfs3_toleb128(lfs3_shrub_trunk(shrub),
            &buffer[d], 4);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    return LFS3_DATA_BUF(buffer, d);
}
#endif

static int lfs3_data_readshrub(lfs3_t *lfs3,
        const lfs3_mdir_t *mdir, lfs3_data_t *data,
        lfs3_shrub_t *shrub) {
    // copy the mdir block
    shrub->blocks[0] = mdir->r.blocks[0];
    #ifndef LFS3_RDONLY
    shrub->eoff = 0;
    #endif

    int err = lfs3_data_readleb128(lfs3, data, &shrub->weight);
    if (err) {
        return err;
    }

    err = lfs3_data_readlleb128(lfs3, data, &shrub->trunk);
    if (err) {
        return err;
    }
    // shrub trunks should never be null
    LFS3_ASSERT(lfs3_shrub_trunk(shrub));

    // set the shrub bit in our trunk
    shrub->trunk |= LFS3_RBYD_ISSHRUB;
    return 0;
}

// fetching for shrubs just means finding the shrub estimate
//
// we don't store this on-disk as it depends on a lot of internal
// variables, worst-case leb128 encoding, etc, and would be easily
// broken by driver changes
//
// note, unlike rbyds, we never need to discard this once fetched
#ifndef LFS3_RDONLY
static int lfs3_shrub_fetch(lfs3_t *lfs3, lfs3_shrub_t *shrub) {
    lfs3_ssize_t estimate = lfs3_rbyd_estimate(lfs3, shrub, -1, -1,
            NULL);
    if (estimate < 0) {
        return estimate;
    }

    // we abuse eoff here because we're not using it for anything else
    shrub->eoff = estimate;
    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_shrub_mkfetched(lfs3_t *lfs3, lfs3_shrub_t *shrub) {
    // already fetched?
    if (lfs3_shrub_isfetched(shrub)) {
        return 0;

    // fetch the shrub estimate
    } else {
        return lfs3_shrub_fetch(lfs3, shrub);
    }
}
#endif

// these are used in mdir commit/compaction

// true if this is the last or only shrub reference in our handles
#ifndef LFS3_RDONLY
static bool lfs3_shrub_islast(const lfs3_t *lfs3, const lfs3_shrub_t *shrub) {
    const lfs3_shrub_t *last = NULL;
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && lfs3_shrub_cmp(&((lfs3_file_t*)h)->bshrub, shrub) == 0) {
            last = &((lfs3_file_t*)h)->bshrub;
        }
    }

    return !last || shrub == last;
}
#endif

// sneak shrub commits into mdir commits
#ifndef LFS3_RDONLY
static int lfs3_shrub_commit(lfs3_t *lfs3, lfs3_rbyd_t *rbyd_,
        lfs3_shrub_t *shrub, lfs3_srid_t rid,
        const lfs3_rattr_t *rattrs) {
    // swap out our trunk/weight temporarily, note we're
    // operating on a copy so if this fails we shouldn't mess
    // things up too much
    //
    // it is important that these rbyds share eoff/cksum/etc
    lfs3_size_t trunk = rbyd_->trunk;
    lfs3_srid_t weight = rbyd_->weight;
    rbyd_->trunk = shrub->trunk;
    rbyd_->weight = shrub->weight;

    // append any shrub attributes
    int err = lfs3_rbyd_appendrattrs(lfs3, rbyd_, rid, -1, -1, rattrs);
    if (err) {
        return err;
    }

    // restore mdir to the main trunk/weight
    shrub->trunk = rbyd_->trunk;
    shrub->weight = rbyd_->weight;
    rbyd_->trunk = trunk;
    rbyd_->weight = weight;
    return 0;
}
#endif

// compact a shrub, and stage any opened shrubs
#ifndef LFS3_RDONLY
static int lfs3_shrub_compact(lfs3_t *lfs3, lfs3_rbyd_t *rbyd_,
        lfs3_shrub_t *shrub_, const lfs3_shrub_t *shrub) {
    // save our current trunk/weight
    lfs3_size_t trunk = rbyd_->trunk;
    lfs3_srid_t weight = rbyd_->weight;

    // compact our shrub
    int err = lfs3_rbyd_appendshrub(lfs3, rbyd_, shrub);
    if (err) {
        return err;
    }

    // stage any opened shrubs with their new location so we can
    // update these later if our commit is a success
    //
    // this should include our current bshrub
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && lfs3_shrub_cmp(&((lfs3_file_t*)h)->bshrub, shrub) == 0) {
            ((lfs3_file_t*)h)->bshrub_.blocks[0] = rbyd_->blocks[0];
            ((lfs3_file_t*)h)->bshrub_.trunk = rbyd_->trunk;
            ((lfs3_file_t*)h)->bshrub_.weight = rbyd_->weight;
        }
    }

    // revert rbyd trunk/weight
    shrub_->blocks[0] = rbyd_->blocks[0];
    shrub_->trunk = rbyd_->trunk;
    shrub_->weight = rbyd_->weight;
    rbyd_->trunk = trunk;
    rbyd_->weight = weight;
    return 0;
}
#endif


// ok, actual bshrub things

// create a non-existant bshrub
static void lfs3_bshrub_init(lfs3_bshrub_t *bshrub) {
    // set up a null bshrub
    bshrub->weight = 0;
    bshrub->blocks[0] = -1;
    bshrub->trunk = 0;
    #ifndef LFS3_RDONLY
    bshrub->eoff = 0;
    #endif
}

static inline bool lfs3_bshrub_isbshrub(const lfs3_bshrub_t *bshrub) {
    return lfs3_shrub_isshrub(bshrub);
}

static inline bool lfs3_bshrub_isbtree(const lfs3_bshrub_t *bshrub) {
    return !lfs3_shrub_isshrub(bshrub);
}

static inline lfs3_file_t *lfs3_bshrub_file(lfs3_btree_t *bshrub) {
    return lfs3_shrub_file(bshrub);
}

static inline int lfs3_bshrub_cmp(
        const lfs3_bshrub_t *a,
        const lfs3_bshrub_t *b) {
    return lfs3_shrub_cmp(a, b);
}

// needed in lfs3_mdir_fetchbshrub
static lfs3_stag_t lfs3_mdir_lookup(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag,
        lfs3_data_t *data_);

// fetch the bshrub/btree attatched to the current mdir+mid, if there
// is one
static int lfs3_mdir_fetchbshrub(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_bshrub_t *bshrub_) {
    // lookup the file struct, if there is one
    lfs3_data_t data;
    lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, mdir,
            LFS3_tag_MASK8 | LFS3_TAG_STRUCT,
            &data);
    if (tag < 0) {
        return tag;
    }

    // found a bshrub? (inlined btree)
    if (tag == LFS3_TAG_BSHRUB) {
        return lfs3_data_readshrub(lfs3, mdir, &data,
                bshrub_);

    // found a btree?
    } else if (tag == LFS3_TAG_BTREE) {
        return lfs3_data_fetchbtree(lfs3, &data,
                bshrub_);

    // we can run into other structs, dids in lfs3_mtree_traverse for
    // example, just ignore these for now
    } else {
        return LFS3_ERR_NOENT;
    }
}

// bshrub lookup functions
static lfs3_stag_t lfs3_bshrub_lookupnext_(lfs3_t *lfs3,
        const lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_rbyd_t *rbyd_, lfs3_srid_t *rid_,
        lfs3_bid_t *weight_, lfs3_data_t *data_) {
    return lfs3_btree_lookupnext_(lfs3, bshrub, bid,
            bid_, rbyd_, rid_, weight_, data_);
}

static lfs3_stag_t lfs3_bshrub_lookupnext(lfs3_t *lfs3,
        const lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    return lfs3_btree_lookupnext(lfs3, bshrub, bid,
            bid_, weight_, data_);
}

static lfs3_stag_t lfs3_bshrub_lookup(lfs3_t *lfs3,
        const lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid, lfs3_tag_t tag,
        lfs3_data_t *data_) {
    return lfs3_btree_lookup(lfs3, bshrub, bid, tag,
            data_);
}

static lfs3_stag_t lfs3_bshrub_traverse(lfs3_t *lfs3,
        const lfs3_bshrub_t *bshrub,
        lfs3_btrv_t *btrv,
        lfs3_sbid_t *bid_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    return lfs3_btree_traverse(lfs3, bshrub, btrv,
            bid_, weight_, data_);
}

// needed in lfs3_bshrub_commitroot_
static bool lfs3_handle_isopen(const lfs3_t *lfs3, const lfs3_handle_t *h);
static inline bool lfs3_o_needssync(uint32_t flags);
#ifndef LFS3_RDONLY
static int lfs3_mdir_commit(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        const lfs3_rattr_t *rattrs);
#endif

// commit to the bshrub root, i.e. the bshrub's shrub
#ifndef LFS3_RDONLY
static int lfs3_bshrub_commitroot_(lfs3_t *lfs3, lfs3_bshrub_t *bshrub,
        const lfs3_bcommit_t *bcommit) {
    // bshrubs must be tracked here
    lfs3_file_t *file = lfs3_bshrub_file(bshrub);
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    LFS3_ASSERT(lfs3_o_type(file->h.flags) == LFS3_TYPE_REG);
    // note that we may _not_ be a bshrub, if we're transition from
    // btree -> bshrub

    // before committing, we need to figure out if we will overflow the
    // configured shrub_size
    //
    // this is where our shrub estimate and bcommit->shestimate become
    // important, we don't have much to go on beside what we track
    // ourselves
    //
    // we don't store this on-disk as it depends on a lot of internal
    // variables, worst-case leb128 encoding, etc, and would be easily
    // broken by driver changes
    //
    // but we do use simple constants (see lfs3_rbyd_estimate), to make
    // it easy to manipulate

    // include our current shrub estimate, if we have one
    lfs3_size_t shestimate = 0;
    if (lfs3_bshrub_isbshrub(bshrub)) {
        // fetch shrub estimate if we haven't already
        int err = lfs3_shrub_mkfetched(lfs3, bshrub);
        if (err) {
            return err;
        }

        // we abuse eoff here because we're not using it for
        // anything else
        shestimate = lfs3_sadd(shestimate, bshrub->eoff);
    }

    // check if this shrub + any not-in-sync shrub references will
    // exceed our shrub_size
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == file->h.mdir.mid
                // don't double include our shrub
                && !lfs3_shrub_cmp(&((lfs3_file_t*)h)->bshrub, bshrub) == 0
                // only include not-in-sync shrubs
                && lfs3_o_needssync(h->flags)
                && lfs3_shrub_isshrub(&((lfs3_file_t*)h)->bshrub)
                // deduplicate shrubs, yes this can happen with desynced
                // rdonly shrubs
                && lfs3_shrub_islast(lfs3, &((lfs3_file_t*)h)->bshrub)) {
            // make sure the shrub estimate is fetched
            //
            // yes not-in-sync shrubs may be unfetched, things gets a
            // bit weird for desynced rdonly shrubs
            int err = lfs3_shrub_mkfetched(lfs3,
                    &((lfs3_file_t*)h)->bshrub);
            if (err) {
                return err;
            }

            // we abuse eoff here because we're not using it for
            // anything else
            shestimate = lfs3_sadd(shestimate,
                    ((lfs3_file_t*)h)->bshrub.eoff);
        }
    }

    // estimating a negative shrub? that shouldn't happen
    LFS3_ASSERT(bcommit->shestimate >= -(lfs3_ssize_t)shestimate);
    LFS3_ASSERT(bcommit->shestimate
            >= -(lfs3_ssize_t)((lfs3_bshrub_isbshrub(&file->bshrub))
                ? file->bshrub.eoff
                : 0));
    // uh oh, too big?
    if (lfs3_sadd(shestimate, bcommit->shestimate) > lfs3->cfg->shrub_size) {
        return LFS3_ERR_RANGE;
    }

    // commit to shrub
    //
    // note we do _not_ checkpoint the allocator here, blocks may be
    // in-flight!
    int err = lfs3_mdir_commit(lfs3, &file->h.mdir, (const lfs3_rattr_t[]){
            LFS3_RATTR(LFS3_tag_SHRUBCOMMIT, 0, 4),
            LFS3_RATTR_ARG(&file->bshrub),
            LFS3_RATTR_ARG(bcommit->bid),
            LFS3_RATTR_ARG(bcommit->rattrs),
            LFS3_RATTR_ARG(bcommit->shestimate),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }
    LFS3_ASSERT(file->bshrub.blocks[0] == file->h.mdir.r.blocks[0]);
    LFS3_ASSERT((lfs3_ssize_t)file->bshrub.eoff >= 0);

    return 0;
}
#endif

// commit to a specific rbyd in a bshrub, this is atomic
#ifndef LFS3_RDONLY
static int lfs3_bshrub_commit_(lfs3_t *lfs3, lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid, lfs3_rbyd_t *rbyd, lfs3_srid_t rid,
        const lfs3_rattr_t *rattrs, lfs3_ssize_t shestimate) {
    // bshrubs must be tracked here
    lfs3_file_t *file = lfs3_bshrub_file(bshrub);
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    LFS3_ASSERT(lfs3_o_type(file->h.flags) == LFS3_TYPE_REG);
    // bid in range?
    LFS3_ASSERT(bid <= file->bshrub.weight);

    // try to commit to the btree
    lfs3_bcommit_t bcommit; // do _not_ fully init this
    bcommit.bid = bid;
    bcommit.rattrs = rattrs;
    bcommit.shestimate = shestimate;
    int err = lfs3_btree_commit__(lfs3, &file->bshrub_, &file->bshrub,
            rbyd, rid, &bcommit);
    if (err && err != LFS3_ERR_RANGE
            && err != LFS3_ERR_EXIST) {
        return err;
    }

    // when btree is shrubbed or split, lfs3_btree_commit__ stops at the
    // root and returns with pending rattrs
    //
    // note that bshrubs can't go straight to splitting, bshrubs are
    // always converted to btrees first, which can't fail (shrub < 1/2
    // block + commit < 1/2 block)
    if (err == LFS3_ERR_RANGE
            || err == LFS3_ERR_EXIST) {
        // bshrubs can't go straight to splitting
        LFS3_ASSERT(!lfs3_bshrub_isbshrub(&file->bshrub)
                || err != LFS3_ERR_RANGE);

        // try to commit to shrub root
        err = lfs3_bshrub_commitroot_(lfs3, &file->bshrub,
                &bcommit);
        if (err && err != LFS3_ERR_RANGE) {
            return err;
        }

        // if we don't fit, convert to btree
        if (err == LFS3_ERR_RANGE) {
            err = lfs3_btree_commitroot_(lfs3, &file->bshrub_, &file->bshrub,
                    &bcommit);
            if (err) {
                return err;
            }
        }
    }

    // update the bshrub/btree
    file->bshrub = file->bshrub_;

    LFS3_ASSERT(lfs3_shrub_trunk(&file->bshrub));
    #ifdef LFS3_DBGBTREECOMMITS
    if (lfs3_bshrub_isbshrub(&file->bshrub)) {
        LFS3_DEBUG("Committed bshrub "
                    "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32,
                file->h.mdir.r.blocks[0], file->h.mdir.r.blocks[1],
                lfs3_shrub_trunk(&file->bshrub),
                file->bshrub.weight);
    } else {
        LFS3_DEBUG("Committed btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                    "cksum %"PRIx32,
                file->bshrub.blocks[0], lfs3_shrub_trunk(&file->bshrub),
                file->bshrub.weight,
                file->bshrub.cksum);
    }
    #endif
    return 0;
}
#endif

// commit to a bshrub, this is atomic
#ifndef LFS3_RDONLY
static int lfs3_bshrub_commit(lfs3_t *lfs3, lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid,
        const lfs3_rattr_t *rattrs, lfs3_ssize_t shestimate) {
    // bshrubs must be tracked here
    lfs3_file_t *file = lfs3_bshrub_file(bshrub);
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    LFS3_ASSERT(lfs3_o_type(file->h.flags) == LFS3_TYPE_REG);
    // bid in range?
    LFS3_ASSERT(bid <= file->bshrub.weight);

    // lookup which leaf our bid resides
    lfs3_rbyd_t rbyd = file->bshrub;
    lfs3_srid_t rid = bid;
    if (file->bshrub.weight > 0) {
        lfs3_bid_t bid__;
        lfs3_srid_t rid__;
        lfs3_stag_t tag__ = lfs3_btree_lookupnext_(lfs3, &file->bshrub,
                // for lfs3_btree_commit__ operations to work out, we
                // need to limit our bid to an rid in the tree, which
                // is what this min is doing
                lfs3_min(bid, file->bshrub.weight-1),
                &bid__, &rbyd, &rid__, NULL, NULL);
        if (tag__ < 0) {
            LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
            return tag__;
        }

        // adjust rid
        LFS3_ASSERT(bid >= bid__ - rid__);
        rid = bid - (bid__ - rid__);
    }

    // tail-recursively commit to the bshrub
    return lfs3_bshrub_commit_(lfs3, &file->bshrub,
            bid, &rbyd, rid, rattrs, shestimate);
}
#endif

// compact a specific rbyd, this is atomic
#ifndef LFS3_RDONLY
static int lfs3_bshrub_compact_(lfs3_t *lfs3, lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid, lfs3_rbyd_t *rbyd) {
    // bshrubs must be tracked here
    lfs3_file_t *file = lfs3_bshrub_file(bshrub);
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    LFS3_ASSERT(lfs3_o_type(file->h.flags) == LFS3_TYPE_REG);
    // bid in range?
    LFS3_ASSERT(bid < file->bshrub.weight);

    // the easiest way to do this is to just mark the rbyd as unerased
    // and call lfs3_btree_commit_
    rbyd->eoff = -1;
    return lfs3_bshrub_commit_(lfs3, &file->bshrub,
            bid, rbyd, rbyd->weight-1, (const lfs3_rattr_t[]){
                LFS3_RATTR_NULL},
            0);
}
#endif




/// Metadata-id things ///

#define LFS3_MID(_lfs, _bid, _rid) \
    (((_bid) & ~((1 << (_lfs)->mbits)-1)) + (_rid))

static inline lfs3_sbid_t lfs3_mbid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    return mid | ((1 << lfs3->mbits) - 1);
}

static inline lfs3_srid_t lfs3_mrid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    // bit of a strange mapping, but we want to preserve mid<=-1 => rid=-1
    return (mid >> (8*sizeof(lfs3_smid_t)-1))
            | (mid & ((1 << lfs3->mbits) - 1));
}

// these should only be used for logging
static inline lfs3_sbid_t lfs3_dbgmbid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    if (lfs3->mtree.weight == 0) {
        return -1;
    } else {
        return mid >> lfs3->mbits;
    }
}

static inline lfs3_srid_t lfs3_dbgmrid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    return lfs3_mrid(lfs3, mid);
}


/// Metadata-pointer things ///

// the mroot anchor, mdir 0x{0,1} is the entry point into the filesystem
#define LFS3_MPTR_MROOTANCHOR() ((const lfs3_block_t[2]){0, 1})

static inline int lfs3_mptr_cmp(
        const lfs3_block_t a[static 2],
        const lfs3_block_t b[static 2]) {
    // note these can be in either order
    if (lfs3_max(a[0], a[1]) != lfs3_max(b[0], b[1])) {
        return lfs3_max(a[0], a[1]) - lfs3_max(b[0], b[1]);
    } else {
        return lfs3_min(a[0], a[1]) - lfs3_min(b[0], b[1]);
    }
}

static inline bool lfs3_mptr_ismrootanchor(
        const lfs3_block_t mptr[static 2]) {
    // mrootanchor is always at 0x{0,1}
    // just check that the first block is in mroot anchor range
    return mptr[0] <= 1;
}

// mptr on-disk encoding
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_frommptr(const lfs3_block_t mptr[static 2],
        uint8_t buffer[static LFS3_MPTR_DSIZE]) {
    // blocks should not exceed 31-bits
    LFS3_ASSERT(mptr[0] <= 0x7fffffff);
    LFS3_ASSERT(mptr[1] <= 0x7fffffff);

    lfs3_ssize_t d = 0;
    for (int i = 0; i < 2; i++) {
        lfs3_ssize_t d_ = lfs3_toleb128(mptr[i], &buffer[d], 5);
        if (d_ < 0) {
            LFS3_UNREACHABLE();
        }
        d += d_;
    }

    return LFS3_DATA_BUF(buffer, d);
}
#endif

static int lfs3_data_readmptr(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_block_t mptr[static 2]) {
    for (int i = 0; i < 2; i++) {
        int err = lfs3_data_readleb128(lfs3, data, &mptr[i]);
        if (err) {
            return err;
        }
    }

    return 0;
}



/// Various flag things ///

// open flags
static inline uint32_t lfs3_o_mode(uint32_t flags) {
    return flags & LFS3_O_MODE;
}

static inline bool lfs3_o_isrdonly(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return lfs3_o_mode(flags) == LFS3_O_RDONLY;
    #else
    return true;
    #endif
}

static inline bool lfs3_o_iswronly(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return lfs3_o_mode(flags) == LFS3_O_WRONLY;
    #else
    return false;
    #endif
}

#ifndef LFS3_RDONLY
static inline bool lfs3_o_iscreat(uint32_t flags) {
    return flags & LFS3_O_CREAT;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_o_isexcl(uint32_t flags) {
    return flags & LFS3_O_EXCL;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_o_istrunc(uint32_t flags) {
    return flags & LFS3_O_TRUNC;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_o_isappend(uint32_t flags) {
    return flags & LFS3_O_APPEND;
}
#endif

static inline bool lfs3_o_isflush(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_FLUSH
    return true;
    #else
    return flags & LFS3_O_FLUSH;
    #endif
}

static inline bool lfs3_o_issync(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_SYNC
    return true;
    #else
    return flags & LFS3_O_SYNC;
    #endif
}

static inline bool lfs3_o_isdesync(uint32_t flags) {
    return flags & LFS3_O_DESYNC;
}

// internal open flags
static inline bool lfs3_o_isset(uint32_t flags) {
    return flags & LFS3_o_SET;
}

static inline uint8_t lfs3_o_type(uint32_t flags) {
    return flags >> 28;
}

static inline uint32_t lfs3_o_typeflags(uint8_t type) {
    return (uint32_t)type << 28;
}

static inline void lfs3_o_settype(uint32_t *flags, uint8_t type) {
    *flags = (*flags & ~LFS3_o_TYPE) | lfs3_o_typeflags(type);
}

static inline bool lfs3_o_iszombie(uint32_t flags) {
    return flags & LFS3_o_ZOMBIE;
}

static inline bool lfs3_o_needscreat(uint32_t flags) {
    return flags & LFS3_o_NEEDSCREAT;
}

static inline bool lfs3_o_needssync(uint32_t flags) {
    return flags & LFS3_o_NEEDSSYNC;
}

static inline bool lfs3_o_needscryst(uint32_t flags) {
    return flags & LFS3_o_NEEDSCRYST;
}

static inline bool lfs3_o_needsgraft(uint32_t flags) {
    return flags & LFS3_o_NEEDSGRAFT;
}

static inline bool lfs3_o_needsflush(uint32_t flags) {
    return flags & LFS3_o_NEEDSFLUSH;
}

// custom attr flags
static inline bool lfs3_a_islazy(uint32_t flags) {
    return flags & LFS3_A_LAZY;
}

// format flags
#ifdef LFS3_GBMAP
static inline bool lfs3_f_isgbmap(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_GBMAP
    return true;
    #else
    return flags & LFS3_F_GBMAP;
    #endif
}
#endif

// mount flags
static inline bool lfs3_m_isrdonly(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_M_RDONLY;
    #else
    return true;
    #endif
}

#ifdef LFS3_REVPERTURB
static inline bool lfs3_m_isrevperturb(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_REVPERTURB
    return true;
    #else
    return flags & LFS3_M_REVPERTURB;
    #endif
}
#endif

#ifdef LFS3_REVNOISE
static inline bool lfs3_m_isrevnoise(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_REVNOISE
    return true;
    #else
    return flags & LFS3_M_REVNOISE;
    #endif
}
#endif

#ifdef LFS3_CKPROGS
static inline bool lfs3_m_isckprogs(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_CKPROGS
    return true;
    #else
    return flags & LFS3_M_CKPROGS;
    #endif
}
#endif

#ifdef LFS3_CKFETCHES
static inline bool lfs3_m_isckfetches(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_CKFETCHES
    return true;
    #else
    return flags & LFS3_M_CKFETCHES;
    #endif
}
#endif

#ifdef LFS3_CKMETAPARITY
static inline bool lfs3_m_isckparity(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_CKMETAPARITY
    return true;
    #else
    return flags & LFS3_M_CKMETAPARITY;
    #endif
}
#endif

#ifdef LFS3_CKDATACKSUMS
static inline bool lfs3_m_isckdatacksums(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_CKDATACKSUMS
    return true;
    #else
    return flags & LFS3_M_CKDATACKSUMS;
    #endif
}
#endif

// traversal flags
static inline bool lfs3_t_isrdonly(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_T_RDONLY;
    #else
    return true;
    #endif
}

static inline bool lfs3_t_ismtreeonly(uint32_t flags) {
    return flags & LFS3_T_MTREEONLY;
}

static inline bool lfs3_t_isexcl(uint32_t flags) {
    return flags & LFS3_T_EXCL;
}

static inline bool lfs3_t_isckmeta(uint32_t flags) {
    return flags & LFS3_T_CKMETA;
}

static inline bool lfs3_t_isckdata(uint32_t flags) {
    return flags & LFS3_T_CKDATA;
}

// internal traversal flags
static inline uint8_t lfs3_t_btype(uint32_t flags) {
    return (flags >> 16) & 0xf;
}

static inline uint32_t lfs3_t_btypeflags(uint8_t btype) {
    return (uint32_t)btype << 16;
}

static inline void lfs3_t_setbtype(uint32_t *flags, uint8_t btype) {
    *flags = (*flags & ~LFS3_t_BTYPE) | lfs3_t_btypeflags(btype);
}

static inline bool lfs3_t_isckpointed(uint32_t flags) {
    return flags & LFS3_t_CKPOINTED;
}

static inline bool lfs3_t_isdirty(uint32_t flags) {
    return flags & LFS3_t_DIRTY;
}

static inline bool lfs3_t_isstale(uint32_t flags) {
    return flags & LFS3_t_STALE;
}

static inline bool lfs3_t_isdamaged(uint32_t flags) {
    return flags & LFS3_t_DAMAGED;
}

// gc flags
#ifndef LFS3_RDONLY
static inline bool lfs3_gc_ismkconsistent(uint32_t flags) {
    return flags & LFS3_GC_MKCONSISTENT;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_gc_islookahead(uint32_t flags) {
    return flags & LFS3_GC_LOOKAHEAD;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
static inline bool lfs3_gc_ispreerase(uint32_t flags) {
    return flags & LFS3_GC_PREERASE;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_gc_iscompactmeta(uint32_t flags) {
    return flags & LFS3_GC_COMPACTMETA;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
static inline bool lfs3_gc_isrepairmeta(uint32_t flags) {
    return flags & LFS3_GC_REPAIRMETA;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
static inline bool lfs3_gc_isrepairdata(uint32_t flags) {
    return flags & LFS3_GC_REPAIRDATA;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_gc_isevictmeta(uint32_t flags) {
    return flags & LFS3_gc_EVICTMETA;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_gc_isevictdata(uint32_t flags) {
    return flags & LFS3_gc_EVICTDATA;
}
#endif

// internal gc flags
//
// note the step flags are the same as the normal flags, but shifted
#ifndef LFS3_RDONLY
static inline bool lfs3_gc_ismkconsistenting(uint32_t flags) {
    return flags & LFS3_gc_MKCONSISTENTING;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_gc_islookaheading(uint32_t flags) {
    return flags & LFS3_gc_LOOKAHEADING;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
static inline bool lfs3_gc_ispreeraseing(uint32_t flags) {
    return flags & LFS3_gc_PREERASEING;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_gc_iscompactmetaing(uint32_t flags) {
    return flags & LFS3_gc_COMPACTMETAING;
}
#endif

static inline bool lfs3_gc_isckmetaing(uint32_t flags) {
    return flags & LFS3_gc_CKMETAING;
}

static inline bool lfs3_gc_isckdataing(uint32_t flags) {
    return flags & LFS3_gc_CKDATAING;
}

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_gc_isevictmetaing(uint32_t flags) {
    return flags & LFS3_gc_EVICTMETAING;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_gc_isevictdataing(uint32_t flags) {
    return flags & LFS3_gc_EVICTDATAING;
}
#endif

// mkbad flags
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_mkbad_isevict(uint32_t flags) {
    return flags & LFS3_MKBAD_EVICT;
}
#endif



/// Handles - opened mdir things ///

// we maintain an invasive linked-list of all opened mdirs in order to
// keep metadata state in-sync
//
// each handle stores its type in the flags field, and can be casted to
// update type-specific state

static bool lfs3_handle_isopen(const lfs3_t *lfs3, const lfs3_handle_t *h) {
    for (lfs3_handle_t *h_ = lfs3->handles; h_; h_ = h_->next) {
        if (h_ == h) {
            return true;
        }
    }

    return false;
}

static void lfs3_handle_open(lfs3_t *lfs3, lfs3_handle_t *h) {
    LFS3_ASSERT(!lfs3_handle_isopen(lfs3, h));
    // add to opened list
    h->next = lfs3->handles;
    lfs3->handles = h;
}

static bool lfs3_handle_close(lfs3_t *lfs3, lfs3_handle_t *h) {
    // remove from opened list
    for (lfs3_handle_t **h_ = &lfs3->handles; *h_; h_ = &(*h_)->next) {
        if (*h_ == h) {
            *h_ = (*h_)->next;
            return true;
        }
    }

    return false;
}

// check if a given mid is open
static bool lfs3_mid_isopen(const lfs3_t *lfs3,
        lfs3_smid_t mid, uint32_t mask) {
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        // we really only care about regular open files here, all
        // others are either transient (dirs) or fake (orphans)
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == mid
                // allow caller to ignore files with specific flags
                && !(h->flags & ~mask)) {
            return true;
        }
    }

    return false;
}

// traversal things
//
// we use the traversal handle itself as a cursor in the handle list,
// this avoids entangling too many pointers at the cost of needing more
// iterations through the handle list

static void lfs3_handle_rewind(lfs3_t *lfs3, lfs3_handle_t *h) {
    bool entangled = lfs3_handle_close(lfs3, h);
    h->next = lfs3->handles;
    if (entangled) {
        lfs3->handles = h;
    }
}

// seek _after_ h_
static void lfs3_handle_seek(lfs3_t *lfs3, lfs3_handle_t *h,
        lfs3_handle_t **h_) {
    bool entangled = lfs3_handle_close(lfs3, h);
    h->next = *h_;
    if (entangled) {
        *h_ = h;
    }
}



/// Global-state things ///

// grm (global remove) things
static inline lfs3_size_t lfs3_grm_count(const lfs3_grm_t *grm) {
    return (grm->queue[0] != 0) + (grm->queue[1] != 0);
}

#ifndef LFS3_RDONLY
static inline void lfs3_grm_discard(lfs3_grm_t *grm) {
    grm->queue[0] = 0;
    grm->queue[1] = 0;
}
#endif

#ifndef LFS3_RDONLY
static inline void lfs3_grm_push(lfs3_grm_t *grm, lfs3_mid_t mid) {
    // note mid=0.0 always maps to the root bookmark and should never
    // be grmed
    LFS3_ASSERT(mid != 0);
    LFS3_ASSERT(grm->queue[1] == 0);
    grm->queue[1] = grm->queue[0];
    grm->queue[0] = mid;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_mid_t lfs3_grm_pop(lfs3_grm_t *grm) {
    lfs3_smid_t mid = grm->queue[0];
    grm->queue[0] = grm->queue[1];
    grm->queue[1] = 0;
    return mid;
}
#endif

static inline bool lfs3_grm_hasmid(const lfs3_grm_t *grm, lfs3_mid_t mid) {
    return mid != 0 && (grm->queue[0] == mid || grm->queue[1] == mid);
}

#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_fromgrm(const lfs3_grm_t *grm,
        uint8_t buffer[static LFS3_GRM_DSIZE]) {
    // make sure to zero so we don't leak any info
    lfs3_memset(buffer, 0, LFS3_GRM_DSIZE);

    // encode grms
    lfs3_size_t count = lfs3_grm_count(grm);
    lfs3_ssize_t d = 0;
    for (lfs3_size_t i = 0; i < count; i++) {
        lfs3_ssize_t d_ = lfs3_toleb128(grm->queue[i], &buffer[d], 5);
        if (d_ < 0) {
            LFS3_UNREACHABLE();
        }
        d += d_;
    }

    return LFS3_DATA_BUF(buffer, lfs3_memlen(buffer, LFS3_GRM_DSIZE));
}
#endif

// required by lfs3_data_readgrm
static inline lfs3_mid_t lfs3_mtree_weight(lfs3_t *lfs3);

static int lfs3_data_readgrm(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_grm_t *grm) {
    // clear first
    grm->queue[0] = 0;
    grm->queue[1] = 0;

    // decode grms, these are terminated by either a null (mid=0) or the
    // size of the grm buffer
    for (lfs3_size_t i = 0; i < 2; i++) {
        lfs3_mid_t mid;
        int err = lfs3_data_readleb128(lfs3, data, &mid);
        if (err) {
            return err;
        }

        // null grm?
        if (!mid) {
            break;
        }

        // grm inside mtree?
        LFS3_ASSERT(mid < lfs3_mtree_weight(lfs3));
        grm->queue[i] = mid;
    }

    return 0;
}

// predeclarations of other gstate, needed below
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static lfs3_data_t lfs3_data_fromgbmap(const lfs3_gbmap_t *gbmap,
        uint8_t buffer[static LFS3_GBMAP_DSIZE]);
#endif
#ifdef LFS3_GBMAP
static int lfs3_data_readgbmap(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_gbmap_t *gbmap);
#endif


// some mdir-related gstate things we need

// zero any pending gdeltas
static void lfs3_fs_discardgdelta(lfs3_t *lfs3) {
    // TODO one cool trick would be to make these all contiguous so
    // zeroing is one memset

    // zero the gcksumdelta
    lfs3->gcksum_d = 0;

    // zero the grmdelta
    lfs3_memset(lfs3->grm_d, 0, LFS3_GRM_DSIZE);

    // zero the gbmapdelta
    //
    // note we do this unconditionally! before figuring out if littlefs
    // actually configured to use the gbmap
    #ifdef LFS3_GBMAP
    lfs3_memset(lfs3->gbmap_d, 0, LFS3_GBMAP_DSIZE);
    #endif
}

// commit any pending gdeltas
#ifndef LFS3_RDONLY
static void lfs3_fs_commitgdelta(lfs3_t *lfs3) {
    // keep track of the on-disk gcksum
    lfs3->gcksum_p = lfs3->gcksum;

    // keep track of the on-disk grm
    lfs3_data_fromgrm(&lfs3->grm, lfs3->grm_p);

    #ifdef LFS3_GBMAP
    // keep track of the on-disk gbmap
    if (lfs3_f_isgbmap(lfs3->flags)) {
        // keep track of both the committed gstate and btree for
        // traversals
        lfs3->gbmap.b_p = lfs3->gbmap.b;
        lfs3_data_fromgbmap(&lfs3->gbmap, lfs3->gbmap_p);

    // if disabled, we still want to keep track of the on-disk gstate
    // in case the user wants to re-enable the gbmap
    } else {
        lfs3_memxor(lfs3->gbmap_p, lfs3->gbmap_d, LFS3_GBMAP_DSIZE);
    }
    #endif
}
#endif

// append and consume any pending gstate
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendgdelta(lfs3_t *lfs3, lfs3_rbyd_t *rbyd) {
    // note gcksums are a special case and handled directly in
    // lfs3_mdir_commit__/lfs3_rbyd_appendcksum_

    // pending grm state?
    uint8_t grmdelta_[LFS3_GRM_DSIZE];
    lfs3_data_fromgrm(&lfs3->grm, grmdelta_);
    lfs3_memxor(grmdelta_, lfs3->grm_p, LFS3_GRM_DSIZE);
    lfs3_memxor(grmdelta_, lfs3->grm_d, LFS3_GRM_DSIZE);

    if (lfs3_memlen(grmdelta_, LFS3_GRM_DSIZE) != 0) {
        // make sure to xor any existing delta
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_rbyd_lookup(lfs3, rbyd, -1, LFS3_TAG_GRMDELTA,
                &data);
        if (tag < 0 && tag != LFS3_ERR_NOENT) {
            return tag;
        }

        uint8_t grmdelta[LFS3_GRM_DSIZE];
        lfs3_memset(grmdelta, 0, LFS3_GRM_DSIZE);
        if (tag != LFS3_ERR_NOENT) {
            lfs3_ssize_t d = lfs3_data_read(lfs3, &data,
                    grmdelta, LFS3_GRM_DSIZE);
            if (d < 0) {
                return d;
            }
        }

        lfs3_memxor(grmdelta_, grmdelta, LFS3_GRM_DSIZE);

        // append to our rbyd, replacing any existing delta
        lfs3_size_t size = lfs3_memlen(grmdelta_, LFS3_GRM_DSIZE);
        int err = lfs3_rbyd_appendrattr(lfs3, rbyd, -1, (const lfs3_rattr_t[]){
                // opportunistically remove this tag if delta is all zero
                (size == 0)
                    ? LFS3_RATTR(LFS3_tag_RM | LFS3_TAG_GRMDELTA, 0, 1)
                    : LFS3_RATTR(LFS3_TAG_GRMDELTA, 0, 1,
                        LFS3_FROM_LBUF, size),
                LFS3_RATTR_ARG(grmdelta_)});
        if (err) {
            return err;
        }
    }

    // pending gbmap state?
    #ifdef LFS3_GBMAP
    if (lfs3_f_isgbmap(lfs3->flags)) {
        // lookahead and gbmap window offsets should always be in sync
        //
        // we probably don't need the duplicate fields, but it certainly
        // makes the code simpler
        LFS3_ASSERT(lfs3->gbmap.window == lfs3->lookahead.window);

        uint8_t gbmapdelta_[LFS3_GBMAP_DSIZE];
        lfs3_data_fromgbmap(&lfs3->gbmap, gbmapdelta_);
        lfs3_memxor(gbmapdelta_, lfs3->gbmap_p, LFS3_GBMAP_DSIZE);
        lfs3_memxor(gbmapdelta_, lfs3->gbmap_d, LFS3_GBMAP_DSIZE);

        if (lfs3_memlen(gbmapdelta_, LFS3_GBMAP_DSIZE) != 0) {
            // make sure to xor any existing delta
            lfs3_data_t data;
            lfs3_stag_t tag = lfs3_rbyd_lookup(lfs3, rbyd,
                    -1, LFS3_TAG_GBMAPDELTA,
                    &data);
            if (tag < 0 && tag != LFS3_ERR_NOENT) {
                return tag;
            }

            uint8_t gbmapdelta[LFS3_GBMAP_DSIZE];
            lfs3_memset(gbmapdelta, 0, LFS3_GBMAP_DSIZE);
            if (tag != LFS3_ERR_NOENT) {
                lfs3_ssize_t d = lfs3_data_read(lfs3, &data,
                        gbmapdelta, LFS3_GBMAP_DSIZE);
                if (d < 0) {
                    return d;
                }
            }

            lfs3_memxor(gbmapdelta_, gbmapdelta, LFS3_GBMAP_DSIZE);

            // append to our rbyd, replacing any existing delta
            lfs3_size_t size = lfs3_memlen(gbmapdelta_, LFS3_GBMAP_DSIZE);
            int err = lfs3_rbyd_appendrattr(lfs3, rbyd,
                    -1, (const lfs3_rattr_t[]){
                        // opportunistically remove this tag if delta is
                        // all zero
                        (size == 0)
                            ? LFS3_RATTR(
                                LFS3_tag_RM | LFS3_TAG_GBMAPDELTA, 0, 1)
                            : LFS3_RATTR(
                                LFS3_TAG_GBMAPDELTA, 0, 1,
                                LFS3_FROM_LBUF, size),
                        LFS3_RATTR_ARG(gbmapdelta_)});
            if (err) {
                return err;
            }
        }
    }
    #endif

    return 0;
}
#endif

static int lfs3_fs_consumegdelta(lfs3_t *lfs3, const lfs3_mdir_t *mdir) {
    // consume any gcksum deltas
    lfs3->gcksum_d ^= mdir->gcksumdelta;

    // consume any grm deltas
    lfs3_data_t data;
    lfs3_stag_t tag = lfs3_rbyd_lookup(lfs3, &mdir->r, -1, LFS3_TAG_GRMDELTA,
            &data);
    if (tag < 0 && tag != LFS3_ERR_NOENT) {
        return tag;
    }

    if (tag != LFS3_ERR_NOENT) {
        uint8_t grmdelta[LFS3_GRM_DSIZE];
        lfs3_ssize_t d = lfs3_data_read(lfs3, &data,
                grmdelta, LFS3_GRM_DSIZE);
        if (d < 0) {
            return d;
        }

        lfs3_memxor(lfs3->grm_d, grmdelta, d);
    }

    // consume any gbmap deltas
    //
    // note we do this unconditionally! before figuring out if littlefs
    // actually configured to use the gbmap
    #ifdef LFS3_GBMAP
    tag = lfs3_rbyd_lookup(lfs3, &mdir->r, -1, LFS3_TAG_GBMAPDELTA,
            &data);
    if (tag != LFS3_ERR_NOENT) {
        uint8_t gbmapdelta[LFS3_GBMAP_DSIZE];
        lfs3_ssize_t d = lfs3_data_read(lfs3, &data,
                gbmapdelta, LFS3_GBMAP_DSIZE);
        if (d < 0) {
            return d;
        }

        lfs3_memxor(lfs3->gbmap_d, gbmapdelta, d);
    }
    #endif

    return 0;
}



/// Revision count things ///

// note! the only strict requirement for revision counts is that the
// most recent block has the most recent revision count, all drivers
// must be ok with _simple_ 32-bit counters!
//
// with that said, this driver crams a few more _optional_ features
// into those 32-bits:
//
//   vvvv---- -------- -------- -ddddddd
//   vvvvrrrr rrrrrr-- -------- -ddddddd
//   vvvvrrrr rrrrrrnn nnnnnnnn pddddddd
//   '-.''----.----''----.----' ^'--.--'
//     '------|----------|------|---|---- 4-bit relocation revision
//            '----------|------|---|---- recycle-bits recycle counter
//                       '------|---|---- pseudorandom noise (if revnoise)
//                              '---|---- perturb bit (if revperturb)
//                                  '---- low-effort debug bits
//                               11-1---  - h = mroot anchor
//                               11-11-1  - m = mdir
//                               11---1-  - b = btree node
//

#ifndef LFS3_RDONLY
static inline uint32_t lfs3_rev_init(lfs3_t *lfs3, uint32_t rev) {
    (void)lfs3;
    // we really only care about the top revision bits here, increment
    return (rev & ~((1 << 28)-1)) + (1 << 28);
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rev_needsrelocation(lfs3_t *lfs3, uint32_t rev) {
    if (lfs3->recycle_bits == -1) {
        return false;
    }

    // does out recycle counter overflow?
    uint32_t rev_ = rev + (2 << (28-lfs3_smax(lfs3->recycle_bits, 0)));
    //                     ^ note the +2! this ensures we overflow after
    //                       an odd number of recycles, otherwise we'd
    //                       only ever relocate one block in mdirs
    return (rev_ >> 28) != (rev >> 28);
}
#endif

#ifndef LFS3_RDONLY
static inline uint32_t lfs3_rev_inc(lfs3_t *lfs3, uint32_t rev) {
    // increment recycle counter/revision
    //
    // this mess increments by 2 unless we _don't_ overflow, in effect
    // it implements a mod (2^n)-1 counter, the goal is to avoid
    // multiple needsrelocation triggers (see above)
    uint32_t rev_ = rev + (2 << (28-lfs3_smax(lfs3->recycle_bits, 0)));
    if ((rev_ >> 28) == (rev >> 28)) {
        rev_ -= 1 << (28-lfs3_smax(lfs3->recycle_bits, 0));
    }
    return rev_;
}
#endif



/// Metadata-pair stuff ///

// mdir convenience functions
#ifndef LFS3_RDONLY
static inline void lfs3_mdir_claim(lfs3_mdir_t *mdir) {
    // mark erased state as invalid, we only fallback on this if a
    // commit fails, and at that point it's unlikely we'll be able to
    // reuse the block
    mdir->r.eoff = -1;
}
#endif

static inline int lfs3_mdir_cmp(const lfs3_mdir_t *a, const lfs3_mdir_t *b) {
    return lfs3_mptr_cmp(a->r.blocks, b->r.blocks);
}

static inline bool lfs3_mdir_ismrootanchor(const lfs3_mdir_t *mdir) {
    return lfs3_mptr_ismrootanchor(mdir->r.blocks);
}

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline lfs3_evict_t *lfs3_evict_evictionmdir(lfs3_t *lfs3,
        const lfs3_mdir_t *mdir) {
    for (lfs3_size_t i = 0; i < 2; i++) {
        lfs3_evict_t *evict = lfs3_evict_eviction(lfs3, mdir->r.blocks[i]);
        if (evict) {
            return evict;
        }
    }
    return NULL;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static inline bool lfs3_evict_needsevictionmdir(const lfs3_t *lfs3,
        const lfs3_mdir_t *mdir) {
    return lfs3_evict_evictionmdir((lfs3_t*)lfs3, mdir);
}
#endif

static inline void lfs3_mdir_sync(lfs3_mdir_t *a, const lfs3_mdir_t *b) {
    // copy over everything but the mid
    a->r = b->r;
    a->gcksumdelta = b->gcksumdelta;
}

// mdir operations
static int lfs3_mdir_fetch(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        lfs3_smid_t mid, const lfs3_block_t mptr[static 2]) {
    // create a copy of the mptr, both so we can swap the blocks to keep
    // track of the current revision, and to prevents issues if mptr
    // references the blocks in the mdir
    lfs3_block_t blocks[2] = {mptr[0], mptr[1]};
    // read both revision counts, try to figure out which block
    // has the most recent revision
    uint32_t revs[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        int err = lfs3_bd_readle32(lfs3, blocks[0], 0, 0, LFS3_BD_RELAX,
                &revs[0]);
        if (err < 0 && err != LFS3_ERR_CORRUPT) {
            return err;
        }
        if (err == LFS3_ERR_CORRUPT) {
            revs[0] = 0;
        }

        if (i == 0
                || err == LFS3_ERR_CORRUPT
                || lfs3_scmp(revs[1], revs[0]) > 0) {
            LFS3_SWAP(lfs3_block_t, &blocks[0], &blocks[1]);
            LFS3_SWAP(uint32_t, &revs[0], &revs[1]);
        }
    }

    // try to fetch rbyds in the order of most recent to least recent
    for (int i = 0; i < 2; i++) {
        // reset damaged/condemned bits
        #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
        lfs3->repair_flags = 0;
        #endif

        // try to fetch
        int err = lfs3_rbyd_fetch_(lfs3, &mdir->r,
                blocks[0], -1, LFS3_RBYD_RELAX,
                &mdir->gcksumdelta);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }

        // success?
        if (err != LFS3_ERR_CORRUPT) {
            mdir->mid = mid;
            // keep track of other block for compactions
            mdir->r.blocks[1] = blocks[1];
            #ifdef LFS3_DBGMDIRFETCHES
            LFS3_DEBUG("Fetched mdir %"PRId32" "
                        "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32", "
                        "cksum %"PRIx32,
                    lfs3_dbgmbid(lfs3, mdir->mid),
                    mdir->r.blocks[0], mdir->r.blocks[1],
                    lfs3_rbyd_trunk(&mdir->r),
                    mdir->r.weight,
                    mdir->r.cksum);
            #endif

            // was the rbyd damaged?
            //
            // we use these sticky bits to avoid needing to thread a
            // whole bunch of nuanced error codes through
            // lfs3_rbyd_fetch_ + bd operations
            #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
            if (lfs3_repair_isdamaged(lfs3->repair_flags)
                    || lfs3_repair_iscondemned(lfs3->repair_flags)) {
                // try not to spam damaged warnings
                lfs3_evict_t *evict = lfs3_evict_evictionrbyd(lfs3, &mdir->r);
                if (lfs3_repair_iscondemned(lfs3->repair_flags)
                        && (!evict || !lfs3_evict_isbad(evict))) {
                    LFS3_INFO("Condemned mdir %"PRId32" "
                                "0x{%"PRIx32",%"PRIx32"} (%d)",
                            lfs3_dbgmbid(lfs3, mdir->mid),
                            mdir->r.blocks[0], mdir->r.blocks[1],
                            (lfs3_repair_iscondemned(lfs3->repair_flags))
                                ? LFS3_ERR_CONDEMNED
                                : LFS3_ERR_DAMAGED);
                } else if (!evict) {
                    LFS3_INFO("Damaged mdir %"PRId32" "
                                "0x{%"PRIx32",%"PRIx32"} (%d)",
                            lfs3_dbgmbid(lfs3, mdir->mid),
                            mdir->r.blocks[0], mdir->r.blocks[1],
                            (lfs3_repair_iscondemned(lfs3->repair_flags))
                                ? LFS3_ERR_CONDEMNED
                                : LFS3_ERR_DAMAGED);
                }

                lfs3_evict_push(lfs3, mdir->r.blocks[0],
                        ((lfs3_repair_iscondemned(lfs3->repair_flags))
                            ? LFS3_EVICT_BAD
                            : 0));
            }
            #endif

            return 0;
        }

        LFS3_SWAP(lfs3_block_t, &blocks[0], &blocks[1]);
        LFS3_SWAP(uint32_t, &revs[0], &revs[1]);
    }

    // could not find a non-corrupt rbyd
    return LFS3_ERR_CORRUPT;
}

static int lfs3_data_fetchmdir(lfs3_t *lfs3,
        lfs3_data_t *data, lfs3_smid_t mid,
        lfs3_mdir_t *mdir) {
    // decode mptr and fetch
    int err = lfs3_data_readmptr(lfs3, data,
            mdir->r.blocks);
    if (err) {
        return err;
    }

    return lfs3_mdir_fetch(lfs3, mdir, mid, mdir->r.blocks);
}

static lfs3_tag_t lfs3_mdir_nametag(const lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_smid_t mid, lfs3_tag_t tag) {
    (void)mdir;
    // intercept pending grms here and pretend they're orphaned
    // stickynotes
    //
    // fortunately pending grms/orphaned stickynotes have roughly the
    // same semantics, and this makes it easier to manage the implied
    // mid gap in higher-levels
    if (lfs3_grm_hasmid(&lfs3->grm, mid)) {
        return LFS3_tag_ORPHAN;

    // if we find a stickynote, check to see if there are any open
    // in-sync file handles to decide if it really exists
    } else if (tag == LFS3_TAG_STICKYNOTE
            && !lfs3_mid_isopen(lfs3, mid,
                ~LFS3_o_ZOMBIE & ~LFS3_O_DESYNC)) {
        return LFS3_tag_ORPHAN;

    // map unknown types -> LFS3_tag_UNKNOWN, this simplifies higher
    // levels and prevents collisions with internal types
    //
    // Note future types should probably come with WCOMPAT flags, and be
    // at least reported on non-supporting filesystems
    } else if (tag < LFS3_TAG_REG || tag > LFS3_TAG_BOOKMARK) {
        return LFS3_tag_UNKNOWN;
    }

    return tag;
}

static lfs3_stag_t lfs3_mdir_lookupnext(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag,
        lfs3_data_t *data_) {
    lfs3_srid_t rid__;
    lfs3_stag_t tag__ = lfs3_rbyd_lookupnext(lfs3, &mdir->r,
            lfs3_mrid(lfs3, mdir->mid), tag,
            &rid__, NULL, data_);
    if (tag__ < 0) {
        return tag__;
    }

    // this is very similar to lfs3_rbyd_lookupnext, but we error if
    // lookupnext would change mids
    if (rid__ != lfs3_mrid(lfs3, mdir->mid)) {
        return LFS3_ERR_NOENT;
    }

    // map name tags to understood types
    if (lfs3_tag_suptype(tag__) == LFS3_TAG_NAME) {
        tag__ = lfs3_mdir_nametag(lfs3, mdir, mdir->mid, tag__);
    }

    return tag__;
}

static lfs3_stag_t lfs3_mdir_lookup(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag,
        lfs3_data_t *data_) {
    lfs3_stag_t tag__ = lfs3_mdir_lookupnext(lfs3, mdir, lfs3_tag_key(tag),
            data_);
    if (tag__ < 0) {
        return tag__;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if ((tag__ & lfs3_tag_mask(tag)) != (tag & lfs3_tag_mask(tag))) {
        return LFS3_ERR_NOENT;
    }

    return tag__;
}



/// Metadata-tree things ///

static inline lfs3_mid_t lfs3_mtree_weight(lfs3_t *lfs3) {
    return lfs3_max(lfs3->mtree.weight, 1 << lfs3->mbits);
}

// lookup mdir containing a given mid
static int lfs3_mtree_lookup(lfs3_t *lfs3, lfs3_smid_t mid,
        lfs3_mdir_t *mdir_) {
    // looking up mid=-1 is probably a mistake
    LFS3_ASSERT(mid >= 0);

    // out of bounds?
    if ((lfs3_mid_t)mid >= lfs3_mtree_weight(lfs3)) {
        return LFS3_ERR_NOENT;
    }

    // looking up mroot?
    if (lfs3->mtree.weight == 0) {
        // treat inlined mdir as mid=0
        mdir_->mid = mid;
        lfs3_mdir_sync(mdir_, &lfs3->mroot);
        return 0;

    // look up mdir in actual mtree
    } else {
        lfs3_bid_t bid;
        lfs3_srid_t rid;
        lfs3_bid_t weight;
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_btree_lookupnext_(lfs3, &lfs3->mtree, mid,
                &bid, &mdir_->r, &rid, &weight, &data);
        if (tag < 0) {
            LFS3_ASSERT(tag != LFS3_ERR_NOENT);
            return tag;
        }
        LFS3_ASSERT((lfs3_sbid_t)bid == lfs3_mbid(lfs3, mid));
        LFS3_ASSERT(weight == (lfs3_bid_t)(1 << lfs3->mbits));
        LFS3_ASSERT(tag == LFS3_TAG_MNAME
                || tag == LFS3_TAG_MDIR);

        // if we found an mname, lookup the mdir
        if (tag == LFS3_TAG_MNAME) {
            tag = lfs3_rbyd_lookup(lfs3, &mdir_->r, rid, LFS3_TAG_MDIR,
                    &data);
            if (tag < 0) {
                LFS3_ASSERT(tag != LFS3_ERR_NOENT);
                return tag;
            }
        }

        // fetch mdir
        return lfs3_data_fetchmdir(lfs3, &data, mid,
                mdir_);
    }
}

#ifndef LFS3_RDONLY
static int lfs3_mtree_commit(lfs3_t *lfs3, lfs3_btree_t *mtree,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs) {
    return lfs3_btree_commit(lfs3, mtree, bid, rattrs);
}
#endif



/// Mdir commit logic ///

// this is the gooey atomic center of littlefs
//
// any mutation must go through lfs3_mdir_commit to persist on disk
//
// this makes lfs3_mdir_commit also responsible for propagating changes
// up through the mtree/mroot chain, and through any internal structures,
// making lfs3_mdir_commit quite involved and a bit of a mess.

// low-level mdir operations needed by lfs3_mdir_commit
#ifndef LFS3_RDONLY
static int lfs3_mdir_alloc__(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        lfs3_smid_t mid, bool partial) {
    // assign the mid
    mdir->mid = mid;
    // default to zero gcksumdelta
    mdir->gcksumdelta = 0;

    if (!partial) {
        // allocate one block without an erase
        lfs3_sblock_t block = lfs3_alloc(lfs3, 0);
        if (block < 0) {
            return block;
        }
        mdir->r.blocks[1] = block;
    }

    // read the new revision count
    //
    // we use whatever is on-disk to avoid needing to rewrite the
    // redund block
    uint32_t rev;
    int err = lfs3_bd_readle32(lfs3, mdir->r.blocks[1], 0, 0, LFS3_BD_RELAX,
            &rev);
    if (err && err != LFS3_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    if (err == LFS3_ERR_CORRUPT) {
        rev = 0;
    }
    // reset recycle bits in revision count, add low-effort debug bits
    rev = lfs3_rev_init(lfs3, rev) | 'm';

relocate:;
    // allocate another block with an erase
    lfs3_sblock_t block = lfs3_alloc(lfs3, LFS3_ALLOC_ERASE);
    if (block < 0) {
        return block;
    }
    mdir->r.blocks[0] = block;
    mdir->r.weight = 0;
    mdir->r.trunk = 0;
    mdir->r.eoff = 0;
    mdir->r.cksum = 0;

    // write our revision count
    err = lfs3_rbyd_appendrev(lfs3, &mdir->r, rev);
    if (err) {
        // bad prog? try another block
        if (err == LFS3_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mdir_swap__(lfs3_t *lfs3, lfs3_mdir_t *mdir_,
        const lfs3_mdir_t *mdir, bool force) {
    // assign the mid
    mdir_->mid = mdir->mid;
    // reset to zero gcksumdelta, upper layers should handle this
    mdir_->gcksumdelta = 0;

    // first thing we need to do is read our current revision count
    uint32_t rev;
    int err = lfs3_bd_readle32(lfs3, mdir->r.blocks[0], 0, 0, LFS3_BD_RELAX,
            &rev);
    if (err && err != LFS3_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    if (err == LFS3_ERR_CORRUPT) {
        rev = 0;
    }

    // decide if we need to relocate
    if (!force && lfs3_rev_needsrelocation(lfs3, rev)) {
        return LFS3_ERR_NOSPC;
    }

    // swap our blocks
    mdir_->r.blocks[0] = mdir->r.blocks[1];
    mdir_->r.blocks[1] = mdir->r.blocks[0];
    mdir_->r.weight = 0;
    mdir_->r.trunk = 0;
    mdir_->r.eoff = 0;
    mdir_->r.cksum = 0;

    // erase, preparing for compact
    err = lfs3_bd_erase(lfs3, mdir_->r.blocks[0], 0);
    if (err) {
        return err;
    }

    // increment our revision count and write it to our rbyd
    err = lfs3_rbyd_appendrev(lfs3, &mdir_->r,
            lfs3_rev_inc(lfs3, rev));
    if (err) {
        return err;
    }

    return 0;
}
#endif

// low-level mdir commit, does not handle mtree/mlist/compaction/etc
#ifndef LFS3_RDONLY
static int lfs3_mdir_commit__(lfs3_t *lfs3, lfs3_mdir_t *mdir_,
        lfs3_srid_t start_rid, lfs3_srid_t end_rid,
        lfs3_smid_t mid, const lfs3_rattr_t *rattrs) {
    // since we only ever commit to one mid or split, we can ignore the
    // entire rattr-list if our mid is out of range
    lfs3_srid_t rid = lfs3_mrid(lfs3, mid);
    if (rid >= start_rid
            // note the use of rid+1 and unsigned comparison here to
            // treat end_rid=-1 as "unbounded" in such a way that rid=-1
            // is still included
            && (lfs3_size_t)(rid + 1) <= (lfs3_size_t)end_rid) {

        for (const lfs3_rattr_t *r = rattrs;
                *r;
                r = lfs3_rattr_next(r, &rid)) {
            // we just happen to never split in an mdir commit
            LFS3_ASSERT(!(r > rattrs && lfs3_rattr_isinsert(r)));

            // nested rattr list? we only support simple rattrs here
            if (lfs3_rattr_tag(r) == LFS3_tag_RATTRS) {
                const lfs3_rattr_t *rattrs_
                        = (const lfs3_rattr_t*)lfs3_rattr_arg(r, 0);

                // recurse once
                int err = lfs3_rbyd_appendrattrs(lfs3, &mdir_->r,
                        rid, start_rid, end_rid,
                        rattrs_);
                if (err) {
                    return err;
                }

            // shrub tags append a set of attributes to an unrelated trunk
            // in our rbyd
            } else if (lfs3_rattr_tag(r) == LFS3_tag_SHRUBCOMMIT) {
                lfs3_shrub_t *shrub_ = (lfs3_shrub_t*)lfs3_rattr_arg(r, 0);
                lfs3_srid_t bid_ = lfs3_rattr_arg(r, 1);
                const lfs3_rattr_t *rattrs_
                        = (const lfs3_rattr_t*)lfs3_rattr_arg(r, 2);
                lfs3_ssize_t shestimate_ = lfs3_rattr_arg(r, 3);

                // reset shrub if it doesn't live in our block, this happens
                // when converting from a btree
                lfs3_file_t *file_ = lfs3_shrub_file(shrub_);
                if (!lfs3_shrub_isshrub(&file_->bshrub)) {
                    file_->bshrub_.blocks[0] = mdir_->r.blocks[0];
                    file_->bshrub_.trunk = LFS3_RBYD_ISSHRUB | 0;
                    file_->bshrub_.weight = 0;
                }

                // commit to shrub
                int err = lfs3_shrub_commit(lfs3, &mdir_->r, &file_->bshrub_,
                        bid_, rattrs_);
                if (err) {
                    return err;
                }

                // update shestimate with estimated shestimate delta
                file_->bshrub_.eoff
                        = ((lfs3_shrub_isshrub(&file_->bshrub))
                            ? file_->bshrub.eoff
                            : 0)
                        + shestimate_;

            // push a new grm, this tag lets us push grms atomically when
            // creating new mids
            } else if (lfs3_rattr_tag(r) == LFS3_tag_GRMPUSH) {
                // do nothing here, this is handled up in lfs3_mdir_commit

            // pop a grm, this just lets lfs3_mdir_commit revert things
            // easily
            } else if (lfs3_rattr_tag(r) == LFS3_tag_GRMPOP) {
                // do nothing here, this is handled up in lfs3_mdir_commit

            // move tags copy over any tags associated with the source's rid
            // TODO can this be deduplicated with lfs3_mdir_compact__ more?
            // it _really_ wants to be deduplicated
            } else if (lfs3_rattr_tag(r) == LFS3_tag_MOVE) {
                const lfs3_mdir_t *mdir__
                        = (const lfs3_mdir_t*)lfs3_rattr_arg(r, 0);

                // skip the name tag, this is always replaced by upper layers
                lfs3_stag_t tag = LFS3_TAG_STRUCT-1;
                while (true) {
                    lfs3_data_t data;
                    tag = lfs3_mdir_lookupnext(lfs3, mdir__, tag+1,
                            &data);
                    if (tag < 0) {
                        if (tag == LFS3_ERR_NOENT) {
                            break;
                        }
                        return tag;
                    }

                    // found an inlined shrub? we need to compact the shrub
                    // as well to bring it along with us
                    if (tag == LFS3_TAG_BSHRUB) {
                        lfs3_shrub_t shrub;
                        int err = lfs3_data_readshrub(lfs3, mdir__, &data,
                                &shrub);
                        if (err) {
                            return err;
                        }

                        // compact our shrub
                        err = lfs3_shrub_compact(lfs3, &mdir_->r, &shrub,
                                &shrub);
                        if (err) {
                            return err;
                        }

                        // write our new shrub tag
                        err = lfs3_rbyd_appendrattr(lfs3, &mdir_->r,
                                rid - lfs3_smax(start_rid, 0),
                                (const lfs3_rattr_t[]){
                                    LFS3_RATTR(LFS3_TAG_BSHRUB, 0, 1,
                                        LFS3_FROM_SHRUB),
                                    LFS3_RATTR_ARG(&shrub)});
                        if (err) {
                            return err;
                        }

                    // append the rattr
                    } else {
                        int err = lfs3_rbyd_appendrattr(lfs3, &mdir_->r,
                                rid - lfs3_smax(start_rid, 0),
                                (const lfs3_rattr_t[]){
                                    LFS3_RATTR(tag, 0, 1, LFS3_FROM_DATA),
                                    LFS3_RATTR_ARG(&data)});
                        if (err) {
                            return err;
                        }
                    }
                }

                // we're not quite done! we also need to bring over any
                // unsynced files
                for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
                    if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                            // belongs to our mid?
                            && h->mdir.mid == mdir__->mid
                            // is a shrub?
                            && lfs3_shrub_isshrub(&((lfs3_file_t*)h)->bshrub)
                            // only compact once, first compact should
                            // stage the new block
                            && ((lfs3_file_t*)h)->bshrub_.blocks[0]
                                != mdir_->r.blocks[0]) {
                        int err = lfs3_shrub_compact(lfs3, &mdir_->r,
                                &((lfs3_file_t*)h)->bshrub_,
                                &((lfs3_file_t*)h)->bshrub);
                        if (err) {
                            return err;
                        }
                    }
                }

            // custom attributes need to be reencoded into our tag format
            } else if (lfs3_rattr_tag(r) == LFS3_tag_ATTRS) {
                const struct lfs3_attr *attrs_
                        = (const struct lfs3_attr*)lfs3_rattr_arg(r, 0);
                lfs3_size_t attr_count_ = lfs3_rattr_arg(r, 1);

                for (lfs3_size_t j = 0; j < attr_count_; j++) {
                    // skip readonly attrs and lazy attrs
                    if (lfs3_o_isrdonly(attrs_[j].flags)) {
                        continue;
                    }

                    // first lets check if the attr changed, we don't want
                    // to append attrs unless we have to
                    lfs3_data_t data;
                    lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, mdir_,
                            LFS3_TAG_ATTR(attrs_[j].type),
                            &data);
                    if (tag < 0 && tag != LFS3_ERR_NOENT) {
                        return tag;
                    }

                    // does disk match our attr?
                    lfs3_scmp_t cmp = lfs3_attr_cmp(lfs3, &attrs_[j],
                            (tag != LFS3_ERR_NOENT) ? &data : NULL);
                    if (cmp < 0) {
                        return cmp;
                    }

                    if (cmp == LFS3_CMP_EQ) {
                        continue;
                    }

                    // append the custom attr
                    int err = lfs3_rbyd_appendrattr(lfs3, &mdir_->r,
                            rid - lfs3_smax(start_rid, 0),
                            (const lfs3_rattr_t[]){
                                // removing or updating?
                                (lfs3_attr_isnoattr(&attrs_[j]))
                                    ? LFS3_RATTR(
                                        LFS3_tag_RM
                                            | LFS3_TAG_ATTR(attrs_[j].type),
                                        0, 2)
                                    : LFS3_RATTR(
                                        LFS3_TAG_ATTR(attrs_[j].type),
                                        0, 2,
                                        LFS3_FROM_BUF),
                                LFS3_RATTR_ARG(attrs_[j].buffer),
                                LFS3_RATTR_ARG(lfs3_attr_size(&attrs_[j]))});
                    if (err) {
                        return err;
                    }
                }

            // write out normal tags normally
            } else {
                int err = lfs3_rbyd_appendrattr(lfs3, &mdir_->r,
                        rid - lfs3_smax(start_rid, 0),
                        r);
                if (err) {
                    return err;
                }
            }
        }
    }

    // abort the commit if our weight dropped to zero!
    //
    // If we finish the commit it becomes immediately visible, but we really
    // need to atomically remove this mdir from the mtree. Leave the actual
    // remove up to upper layers.
    if (mdir_->r.weight == 0
            // unless we are an mroot
            && !(mdir_->mid <= -1
                || lfs3_mdir_cmp(mdir_, &lfs3->mroot) == 0)) {
        // note! we can no longer read from this mdir as our pcache may
        // be clobbered
        return LFS3_ERR_NOENT;
    }

    // append any gstate?
    if (start_rid <= -2) {
        int err = lfs3_rbyd_appendgdelta(lfs3, &mdir_->r);
        if (err) {
            return err;
        }
    }

    // save our canonical cksum
    //
    // note this is before we calculate gcksumdelta, otherwise
    // everything would get all self-referential
    uint32_t cksum = mdir_->r.cksum;

    // append gkcsumdelta?
    if (start_rid <= -2) {
        // figure out changes to our gcksumdelta
        mdir_->gcksumdelta ^= lfs3_crc32c_cube(lfs3->gcksum_p)
                ^ lfs3_crc32c_cube(lfs3->gcksum ^ cksum)
                ^ lfs3->gcksum_d;

        int err = lfs3_rbyd_appendrattr_(lfs3, &mdir_->r,
                LFS3_TAG_GCKSUMDELTA, 0,
                LFS3_FROM_LE32, (const lfs3_rattr_t[]){
                    LFS3_RATTR_ARG(mdir_->gcksumdelta)});
        if (err) {
            return err;
        }
    }

    // finalize commit
    int err = lfs3_rbyd_appendcksum_(lfs3, &mdir_->r, cksum);
    if (err) {
        return err;
    }

    // success?

    // xor our new cksum
    lfs3->gcksum ^= mdir_->r.cksum;

    return 0;
}
#endif

// TODO do we need to include commit overhead here?
#ifndef LFS3_RDONLY
static lfs3_ssize_t lfs3_mdir_estimate__(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_srid_t start_rid, lfs3_srid_t end_rid,
        lfs3_srid_t *split_rid_) {
    // yet another function that is just begging to be deduplicated, but we
    // can't because it would be recursive
    //
    // this is basically the same as lfs3_rbyd_estimate, except we assume all
    // rids have weight 1 and have extra handling for opened files, shrubs, etc

    // calculate dsize by starting from the outside ids and working inwards,
    // this naturally gives us a split rid
    lfs3_srid_t a_rid = lfs3_smax(start_rid, -1);
    lfs3_srid_t b_rid = lfs3_min(mdir->r.weight, end_rid);
    lfs3_size_t a_dsize = 0;
    lfs3_size_t b_dsize = 0;
    lfs3_size_t mdir_dsize = 0;

    while (a_rid != b_rid) {
        if (a_dsize > b_dsize
                // bias so lower dsize >= upper dsize
                || (a_dsize == b_dsize && a_rid > b_rid)) {
            LFS3_SWAP(lfs3_srid_t, &a_rid, &b_rid);
            LFS3_SWAP(lfs3_size_t, &a_dsize, &b_dsize);
        }

        if (a_rid > b_rid) {
            a_rid -= 1;
        }

        lfs3_stag_t tag = 0;
        lfs3_size_t dsize_ = 0;
        while (true) {
            lfs3_srid_t rid_;
            lfs3_data_t data;
            tag = lfs3_rbyd_lookupnext(lfs3, &mdir->r,
                    a_rid, tag+1,
                    &rid_, NULL, &data);
            if (tag < 0) {
                if (tag == LFS3_ERR_NOENT) {
                    break;
                }
                return tag;
            }
            if (rid_ != a_rid) {
                break;
            }

            // special handling for shrub trunks, we need to include the
            // compacted cost of the shrub in our estimate
            //
            // this is what would make lfs3_rbyd_estimate recursive, and
            // why we need a second function...
            //
            if (tag == LFS3_TAG_BSHRUB) {
                // include the cost of the shrub pointer
                dsize_ += lfs3->mattr_estimate + LFS3_SHRUB_DSIZE;

                lfs3_shrub_t shrub;
                int err = lfs3_data_readshrub(lfs3, mdir, &data,
                        &shrub);
                if (err) {
                    return err;
                }

                // only include shrub cost if this is the last reference
                if (lfs3_shrub_islast(lfs3, &shrub)) {
                    lfs3_ssize_t dsize__ = lfs3_rbyd_estimate(lfs3,
                            &shrub, -1, -1,
                            NULL);
                    if (dsize__ < 0) {
                        return dsize__;
                    }
                    dsize_ += dsize__;
                }

            } else {
                // include the cost of this tag
                dsize_ += lfs3->mattr_estimate + lfs3_data_size(&data);
            }
        }

        // include any opened+unsynced inlined files
        //
        // this is O(n^2), but littlefs is unlikely to have many open
        // files, I suppose if this becomes a problem we could sort
        // opened files by mid
        for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
            if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                    // belongs to our mdir + rid?
                    && lfs3_mdir_cmp(&h->mdir, mdir) == 0
                    && lfs3_mrid(lfs3, h->mdir.mid) == a_rid
                    // is a shrub?
                    && lfs3_shrub_isshrub(&((lfs3_file_t*)h)->bshrub)
                    // only include shrub cost if this is the last
                    // reference
                    && lfs3_shrub_islast(lfs3, &((lfs3_file_t*)h)->bshrub)) {
                lfs3_ssize_t dsize__ = lfs3_rbyd_estimate(lfs3,
                        &((lfs3_file_t*)h)->bshrub, -1, -1,
                        NULL);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += dsize__;
            }
        }

        if (a_rid <= -1) {
            mdir_dsize += dsize_;
        } else {
            a_dsize += dsize_;
        }

        if (a_rid < b_rid) {
            a_rid += 1;
        }
    }

    if (split_rid_) {
        *split_rid_ = a_rid;
    }

    return mdir_dsize + a_dsize + b_dsize;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mdir_compact__(lfs3_t *lfs3,
        lfs3_mdir_t *mdir_, const lfs3_mdir_t *mdir,
        lfs3_srid_t start_rid, lfs3_srid_t end_rid) {
    // this is basically the same as lfs3_rbyd_compact, but with special
    // handling for inlined trees.
    //
    // it's really tempting to deduplicate this via recursion! but we
    // can't do that here

    // assume we keep any gcksumdelta, this will get fixed the first time
    // we commit anything
    if (start_rid <= -2) {
        mdir_->gcksumdelta = mdir->gcksumdelta;
    }

    // copy over tags in the rbyd in order
    lfs3_srid_t rid = lfs3_smax(start_rid, -1);
    lfs3_stag_t tag = 0;
    while (true) {
        lfs3_rid_t weight;
        lfs3_data_t data;
        tag = lfs3_rbyd_lookupnext(lfs3, &mdir->r,
                rid, tag+1,
                &rid, &weight, &data);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }
        // end of range? note the use of rid+1 and unsigned comparison here to
        // treat end_rid=-1 as "unbounded" in such a way that rid=-1 is still
        // included
        if ((lfs3_size_t)(rid + 1) > (lfs3_size_t)end_rid) {
            break;
        }

        // found an inlined shrub? we need to compact the shrub as well to
        // bring it along with us
        if (tag == LFS3_TAG_BSHRUB) {
            lfs3_shrub_t shrub;
            int err = lfs3_data_readshrub(lfs3, mdir, &data,
                    &shrub);
            if (err) {
                return err;
            }

            // compact our shrub
            err = lfs3_shrub_compact(lfs3, &mdir_->r, &shrub,
                    &shrub);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                return err;
            }

            // write the new shrub tag
            LFS3_ASSERT(weight == 0);
            err = lfs3_rbyd_appendcompactrattr(lfs3, &mdir_->r,
                    (const lfs3_rattr_t[]){
                        LFS3_RATTR(tag, 0, 1, LFS3_FROM_SHRUB),
                        LFS3_RATTR_ARG(&shrub)});
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                return err;
            }

        } else {
            // write the tag
            int err = lfs3_rbyd_appendcompactrattr(lfs3, &mdir_->r,
                    (const lfs3_rattr_t[]){
                        LFS3_RATTR(tag, -2, 1, LFS3_FROM_DATA),
                        LFS3_RATTR_WEIGHT(weight),
                        LFS3_RATTR_ARG(&data)});
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                return err;
            }
        }
    }

    int err = lfs3_rbyd_appendcompaction(lfs3, &mdir_->r, 0);
    if (err) {
        LFS3_ASSERT(err != LFS3_ERR_RANGE);
        return err;
    }

    // we're not quite done! we also need to bring over any unsynced files
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                // belongs to our mdir?
                && lfs3_mdir_cmp(&h->mdir, mdir) == 0
                && lfs3_mrid(lfs3, h->mdir.mid) >= start_rid
                && (lfs3_rid_t)lfs3_mrid(lfs3, h->mdir.mid)
                    < (lfs3_rid_t)end_rid
                // is a shrub?
                && lfs3_shrub_isshrub(&((lfs3_file_t*)h)->bshrub)
                // only compact once, first compact should
                // stage the new block
                && ((lfs3_file_t*)h)->bshrub_.blocks[0]
                    != mdir_->r.blocks[0]) {
            int err = lfs3_shrub_compact(lfs3, &mdir_->r,
                    &((lfs3_file_t*)h)->bshrub_,
                    &((lfs3_file_t*)h)->bshrub);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                return err;
            }
        }
    }

    return 0;
}
#endif

// mid-level mdir commit, this one will at least compact on overflow
#ifndef LFS3_RDONLY
static int lfs3_mdir_commit_(lfs3_t *lfs3,
        lfs3_mdir_t *mdir_, lfs3_mdir_t *mdir,
        lfs3_srid_t start_rid, lfs3_srid_t end_rid,
        lfs3_srid_t *split_rid_,
        lfs3_smid_t mid, const lfs3_rattr_t *rattrs) {
    // make a copy
    *mdir_ = *mdir;
    // mark our mdir as unerased in case we fail
    lfs3_mdir_claim(mdir);
    // mark any copies of our mdir as unerased in case we fail
    if (lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0) {
        lfs3_mdir_claim(&lfs3->mroot);
    }
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_mdir_cmp(&h->mdir, mdir) == 0) {
            lfs3_mdir_claim(&h->mdir);
        }
    }

    // try to commit
    int err = lfs3_mdir_commit__(lfs3, mdir_, start_rid, end_rid,
            mid, rattrs);
    if (err) {
        if (err == LFS3_ERR_RANGE || err == LFS3_ERR_CORRUPT) {
            goto compact;
        }
        return err;
    }
    return 0;

compact:;
    // can't commit, can we compact?
    bool relocated = false;
    bool overrecyclable = true;

    // check if we're within our compaction threshold
    lfs3_ssize_t estimate = lfs3_mdir_estimate__(lfs3, mdir,
            start_rid, end_rid,
            split_rid_);
    if (estimate < 0) {
        return estimate;
    }

    // TODO do we need to include mdir commit overhead here? in rbyd_estimate?
    if ((lfs3_size_t)estimate > lfs3->cfg->block_size/2) {
        return LFS3_ERR_RANGE;
    }

    // are we being evicted? definitely shouldn't try compacting, jump
    // straight to relocating
    #ifdef LFS3_EVICT
    if (lfs3_evict_needsevictionmdir(lfs3, mdir)
            // well, not if we're the mroot anchor, evicting the mroot
            // anchor doesn't accomplish anything, but we can at least
            // compact
            && !lfs3_mdir_ismrootanchor(mdir)) {
        goto relocate;
    }
    #endif

    // swap blocks, increment revision count
    err = lfs3_mdir_swap__(lfs3, mdir_, mdir, false);
    if (err) {
        if (err == LFS3_ERR_NOSPC || err == LFS3_ERR_CORRUPT) {
            overrecyclable &= (err != LFS3_ERR_CORRUPT);
            goto relocate;
        }
        return err;
    }

    while (true) {
        // try to compact
        #ifdef LFS3_DBGMDIRCOMMITS
        LFS3_DEBUG("Compacting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "-> 0x{%"PRIx32",%"PRIx32"}",
                lfs3_dbgmbid(lfs3, mdir->mid),
                mdir->r.blocks[0], mdir->r.blocks[1],
                mdir_->r.blocks[0], mdir_->r.blocks[1]);
        #endif

        // don't copy over gstate if relocating
        lfs3_srid_t start_rid_ = start_rid;
        if (relocated) {
            start_rid_ = lfs3_smax(start_rid_, -1);
        }

        // compact our mdir
        err = lfs3_mdir_compact__(lfs3, mdir_, mdir, start_rid_, end_rid);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                overrecyclable &= relocated;
                goto relocate;
            }
            return err;
        }

        // now try to commit again
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfs3_mdir_commit__(lfs3, mdir_, start_rid_, end_rid,
                mid, rattrs);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                overrecyclable &= relocated;
                goto relocate;
            }
            return err;
        }

        // consume gcksumdelta if relocated
        if (relocated) {
            lfs3->gcksum_d ^= mdir->gcksumdelta;
        }
        return 0;

    relocate:;
        // needs relocation? bad prog? ok, try allocating a new mdir
        err = lfs3_mdir_alloc__(lfs3, mdir_, mdir->mid, relocated);
        if (err && !(err == LFS3_ERR_NOSPC && overrecyclable)) {
            return err;
        }
        relocated = true;

        // no more blocks? wear-leveling falls apart here, but we can try
        // without relocating
        if (err == LFS3_ERR_NOSPC) {
            LFS3_WARN("Overrecycling mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfs3_dbgmbid(lfs3, mdir->mid),
                    mdir->r.blocks[0], mdir->r.blocks[1]);
            relocated = false;
            overrecyclable = false;

            err = lfs3_mdir_swap__(lfs3, mdir_, mdir, true);
            if (err) {
                // bad prog? can't do much here, mdir stuck
                if (err == LFS3_ERR_CORRUPT) {
                    LFS3_ERROR("Stuck mdir 0x{%"PRIx32",%"PRIx32"}",
                            mdir->r.blocks[0],
                            mdir->r.blocks[1]);
                    return LFS3_ERR_NOSPC;
                }
                return err;
            }
        }
    }
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mroot_parent(lfs3_t *lfs3, const lfs3_block_t mptr[static 2],
        lfs3_mdir_t *mparent_) {
    // we only call this when we actually have parents
    LFS3_ASSERT(!lfs3_mptr_ismrootanchor(mptr));

    // scan list of mroots for our requested pair
    lfs3_block_t mptr_[2] = {
            LFS3_MPTR_MROOTANCHOR()[0],
            LFS3_MPTR_MROOTANCHOR()[1]};
    while (true) {
        // fetch next possible superblock
        lfs3_mdir_t mdir;
        int err = lfs3_mdir_fetch(lfs3, &mdir, -1, mptr_);
        if (err) {
            return err;
        }

        // lookup next mroot
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, &mdir, LFS3_TAG_MROOT,
                &data);
        if (tag < 0) {
            LFS3_ASSERT(tag != LFS3_ERR_NOENT);
            return tag;
        }

        // decode mdir
        err = lfs3_data_readmptr(lfs3, &data, mptr_);
        if (err) {
            return err;
        }

        // found our child?
        if (lfs3_mptr_cmp(mptr_, mptr) == 0) {
            *mparent_ = mdir;
            return 0;
        }
    }
}
#endif

// needed in lfs3_mdir_commit
static inline void lfs3_file_discardleaf(lfs3_file_t *file);

// high-level mdir commit
//
// this is atomic and updates any opened mdirs, lfs3_t, etc
//
// note that if an error occurs, any gstate is reverted to the on-disk
// state
//
#ifndef LFS3_RDONLY
static int lfs3_mdir_commit(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        const lfs3_rattr_t *rattrs) {
    // non-mroot mdirs must have weight
    LFS3_ASSERT(mdir->mid <= -1
            // note inlined mdirs are mroots with mid != -1
            || lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0
            || mdir->r.weight > 0);
    // rid in-bounds?
    LFS3_ASSERT(lfs3_mrid(lfs3, mdir->mid)
            <= (lfs3_srid_t)mdir->r.weight);
    // lfs3->mroot must have mid=-1
    LFS3_ASSERT(lfs3->mroot.mid == -1);

    // save our grm in case we fail
    //
    // note that we can't use lfs3->grm_p here, as our grm may not match
    // the on-disk state thanks to orphaned readonly files
    lfs3_grm_t grm_p = lfs3->grm;

    // play out any rattrs that affect our grm _before_ committing to
    // disk, this is where reverting things on error is handy
    lfs3_smid_t mid_ = lfs3_smax(mdir->mid, -1);
    for (const lfs3_rattr_t *r = rattrs; *r; r = lfs3_rattr_next(r, &mid_)) {
        // push a new grm, this tag lets us push grms atomically when
        // creating new mids
        if (lfs3_rattr_tag(r) == LFS3_tag_GRMPUSH) {
            lfs3_grm_push(&lfs3->grm, mid_);

        // pop a grm, this just lets lfs3_mdir_commit revert things
        // easily
        } else if (lfs3_rattr_tag(r) == LFS3_tag_GRMPOP) {
            lfs3_grm_pop(&lfs3->grm);

        // adjust pending grms?
        } else {
            for (int j = 0; j < 2; j++) {
                if (lfs3_mbid(lfs3, lfs3->grm.queue[j])
                            == lfs3_mbid(lfs3, mid_)
                        && (lfs3_smid_t)lfs3->grm.queue[j] >= mid_) {
                    // deleting a pending grm doesn't really make sense
                    LFS3_ASSERT((lfs3_smid_t)lfs3->grm.queue[j]
                            >= mid_ - lfs3_rattr_weight(r));

                    // adjust the grm
                    lfs3->grm.queue[j] += lfs3_rattr_weight(r);
                }
            }
        }
    }

    // flush gdeltas
    lfs3_fs_discardgdelta(lfs3);

    // xor our old cksum
    lfs3->gcksum ^= mdir->r.cksum;

    // stage any shrubs
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG) {
            // a shrub outside of its mdir means something has gone
            // horribly wrong
            LFS3_ASSERT(!lfs3_shrub_isshrub(&((lfs3_file_t*)h)->bshrub)
                    || ((lfs3_file_t*)h)->bshrub.blocks[0]
                        == h->mdir.r.blocks[0]);
            ((lfs3_file_t*)h)->bshrub_ = ((lfs3_file_t*)h)->bshrub;
        }
    }

    // attempt to commit/compact the mdir normally
    lfs3_mdir_t mdir_[2];
    lfs3_srid_t split_rid;
    int err = lfs3_mdir_commit_(lfs3, &mdir_[0], mdir, -2, -2,
            &split_rid,
            mdir->mid, rattrs);
    if (err && err != LFS3_ERR_RANGE
            && err != LFS3_ERR_NOENT) {
        goto failed;
    }

    // keep track of any mroot changes
    lfs3_mdir_t mroot_ = lfs3->mroot;
    if (!err && lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0) {
        lfs3_mdir_sync(&mroot_, &mdir_[0]);
    }

    // handle possible mtree updates, this gets a bit messy
    lfs3_smid_t mdelta = 0;
    lfs3_btree_t mtree_ = lfs3->mtree;
    // need to split?
    if (err == LFS3_ERR_RANGE) {
        // this should not happen unless we can't fit our mroot's metadata
        LFS3_ASSERT(lfs3_mdir_cmp(mdir, &lfs3->mroot) != 0
                || lfs3->mtree.weight == 0);

        // if we're not the mroot, we need to consume the gstate so
        // we don't lose any info during the split
        //
        // we do this here so we don't have to worry about corner cases
        // with dropping mdirs during a split
        if (lfs3_mdir_cmp(mdir, &lfs3->mroot) != 0) {
            err = lfs3_fs_consumegdelta(lfs3, mdir);
            if (err) {
                goto failed;
            }
        }

        for (int i = 0; i < 2; i++) {
            // order the split compacts so that that mdir containing our mid
            // is committed last, this is a bit of a hack but necessary so
            // shrubs are staged correctly
            bool l = (lfs3_mrid(lfs3, mdir->mid) < split_rid);

            bool relocated = false;
        split_relocate:;
            // alloc and compact into new mdirs
            err = lfs3_mdir_alloc__(lfs3, &mdir_[i^l],
                    lfs3_smax(mdir->mid, 0), relocated);
            if (err) {
                goto failed;
            }
            relocated = true;

            err = lfs3_mdir_compact__(lfs3, &mdir_[i^l],
                    mdir,
                    ((i^l) == 0) ?         0 : split_rid,
                    ((i^l) == 0) ? split_rid :        -1);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS3_ERR_CORRUPT) {
                    goto split_relocate;
                }
                goto failed;
            }

            err = lfs3_mdir_commit__(lfs3, &mdir_[i^l],
                    ((i^l) == 0) ?         0 : split_rid,
                    ((i^l) == 0) ? split_rid :        -1,
                    mdir->mid, rattrs);
            if (err && err != LFS3_ERR_NOENT) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS3_ERR_CORRUPT) {
                    goto split_relocate;
                }
                goto failed;
            }
            // empty? set weight to zero
            if (err == LFS3_ERR_NOENT) {
                mdir_[i^l].r.weight = 0;
            }
        }

        // adjust our sibling's mid after committing rattrs
        mdir_[1].mid += (1 << lfs3->mbits);

        LFS3_INFO("Splitting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "-> 0x{%"PRIx32",%"PRIx32"}, 0x{%"PRIx32",%"PRIx32"}",
                lfs3_dbgmbid(lfs3, mdir->mid),
                mdir->r.blocks[0], mdir->r.blocks[1],
                mdir_[0].r.blocks[0], mdir_[0].r.blocks[1],
                mdir_[1].r.blocks[0], mdir_[1].r.blocks[1]);

        // because of defered commits, children can be reduced to zero
        // when splitting, need to catch this here

        // both siblings reduced to zero
        if (mdir_[0].r.weight == 0 && mdir_[1].r.weight == 0) {
            LFS3_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfs3_dbgmbid(lfs3, mdir_[0].mid),
                    mdir_[0].r.blocks[0], mdir_[0].r.blocks[1]);
            LFS3_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfs3_dbgmbid(lfs3, mdir_[1].mid),
                    mdir_[1].r.blocks[0], mdir_[1].r.blocks[1]);
            goto dropped;

        // one sibling reduced to zero
        } else if (mdir_[0].r.weight == 0) {
            LFS3_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfs3_dbgmbid(lfs3, mdir_[0].mid),
                    mdir_[0].r.blocks[0], mdir_[0].r.blocks[1]);
            lfs3_mdir_sync(&mdir_[0], &mdir_[1]);
            goto relocated;

        // other sibling reduced to zero
        } else if (mdir_[1].r.weight == 0) {
            LFS3_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfs3_dbgmbid(lfs3, mdir_[1].mid),
                    mdir_[1].r.blocks[0], mdir_[1].r.blocks[1]);
            goto relocated;
        }

        // no siblings reduced to zero, update our mtree
        mdelta = +(1 << lfs3->mbits);

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending rattrs in
        // case they introduce a new name!
        lfs3_data_t split_name;
        lfs3_stag_t split_tag = lfs3_rbyd_lookup(lfs3, &mdir_[1].r, 0,
                LFS3_tag_MASK8 | LFS3_TAG_NAME,
                &split_name);
        if (split_tag < 0) {
            LFS3_ASSERT(split_tag != LFS3_ERR_NOENT);
            err = split_tag;
            goto failed;
        }

        // new mtree?
        if (lfs3->mtree.weight == 0) {
            lfs3_btree_init(&mtree_);

            err = lfs3_mtree_commit(lfs3, &mtree_,
                    0, (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_TAG_MDIR, -2, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_WEIGHT(+(1 << lfs3->mbits)),
                        LFS3_RATTR_ARG(mdir_[0].r.blocks),
                        LFS3_RATTR(LFS3_TAG_MNAME, -2, 1, LFS3_FROM_DATA),
                        LFS3_RATTR_WEIGHT(+(1 << lfs3->mbits)),
                        LFS3_RATTR_ARG(&split_name),
                        LFS3_RATTR(LFS3_TAG_MDIR, 0, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_ARG(mdir_[1].r.blocks),
                        LFS3_RATTR_NULL});
            if (err) {
                goto failed;
            }

        // update our mtree
        } else {
            err = lfs3_mtree_commit(lfs3, &mtree_,
                    lfs3_mbid(lfs3, mdir->mid), (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_TAG_MDIR, 0, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_ARG(mdir_[0].r.blocks),
                        LFS3_RATTR(LFS3_TAG_MNAME, -2, 1, LFS3_FROM_DATA),
                        LFS3_RATTR_WEIGHT(+(1 << lfs3->mbits)),
                        LFS3_RATTR_ARG(&split_name),
                        LFS3_RATTR(LFS3_TAG_MDIR, 0, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_ARG(mdir_[1].r.blocks),
                        LFS3_RATTR_NULL});
            if (err) {
                goto failed;
            }
        }

    // need to drop?
    } else if (err == LFS3_ERR_NOENT) {
        LFS3_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                lfs3_dbgmbid(lfs3, mdir->mid),
                mdir->r.blocks[0], mdir->r.blocks[1]);
        // set weight to zero
        mdir_[0].r.weight = 0;

        // consume gstate so we don't lose any info
        err = lfs3_fs_consumegdelta(lfs3, mdir);
        if (err) {
            goto failed;
        }

    dropped:;
        mdelta = -(1 << lfs3->mbits);

        // how can we drop if we have no mtree?
        LFS3_ASSERT(lfs3->mtree.weight != 0);

        // update our mtree
        err = lfs3_mtree_commit(lfs3, &mtree_,
                lfs3_mbid(lfs3, mdir->mid), (const lfs3_rattr_t[]){
                    LFS3_RATTR(LFS3_tag_RM, -2, 0),
                    LFS3_RATTR_WEIGHT(-(1 << lfs3->mbits)),
                    LFS3_RATTR_NULL});
        if (err) {
            goto failed;
        }

    // need to relocate?
    } else if (lfs3_mdir_cmp(&mdir_[0], mdir) != 0
            && lfs3_mdir_cmp(mdir, &lfs3->mroot) != 0
            && mdir->mid > -1) {
        LFS3_INFO("Relocating mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "-> 0x{%"PRIx32",%"PRIx32"}",
                lfs3_dbgmbid(lfs3, mdir->mid),
                mdir->r.blocks[0], mdir->r.blocks[1],
                mdir_[0].r.blocks[0], mdir_[0].r.blocks[1]);

    relocated:;
        // new mtree?
        if (lfs3->mtree.weight == 0) {
            lfs3_btree_init(&mtree_);

            err = lfs3_mtree_commit(lfs3, &mtree_,
                    0, (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_TAG_MDIR, -2, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_WEIGHT(+(1 << lfs3->mbits)),
                        LFS3_RATTR_ARG(mdir_[0].r.blocks),
                        LFS3_RATTR_NULL});
            if (err) {
                goto failed;
            }

        // update our mtree
        } else {
            err = lfs3_mtree_commit(lfs3, &mtree_,
                    lfs3_mbid(lfs3, mdir->mid), (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_TAG_MDIR, 0, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_ARG(mdir_[0].r.blocks),
                        LFS3_RATTR_NULL});
            if (err) {
                goto failed;
            }
        }
    }

    // patch any pending grms
    for (int j = 0; j < 2; j++) {
        if (lfs3_mbid(lfs3, lfs3->grm.queue[j])
                == lfs3_mbid(lfs3, lfs3_smax(mdir->mid, 0))) {
            if (mdelta > 0
                    && lfs3_mrid(lfs3, lfs3->grm.queue[j])
                        >= (lfs3_srid_t)mdir_[0].r.weight) {
                lfs3->grm.queue[j]
                        += (1 << lfs3->mbits) - mdir_[0].r.weight;
            }
        } else if ((lfs3_smid_t)lfs3->grm.queue[j] > mdir->mid) {
            lfs3->grm.queue[j] += mdelta;
        }
    }

    // need to update mtree?
    if (lfs3_btree_cmp(&mtree_, &lfs3->mtree) != 0) {
        // mtree should never go to zero since we always have a root bookmark
        LFS3_ASSERT(mtree_.weight > 0);

        // make sure mtree/mroot changes are on-disk before committing
        // metadata
        err = lfs3_bd_sync(lfs3, 0);
        if (err) {
            goto failed;
        }

        // xor mroot's cksum if we haven't already
        if (lfs3_mdir_cmp(mdir, &lfs3->mroot) != 0) {
            lfs3->gcksum ^= lfs3->mroot.r.cksum;
        }

        // commit new mtree into our mroot
        //
        // note end_rid=0 here will delete any files leftover from a split
        // in our mroot
        err = lfs3_mdir_commit_(lfs3, &mroot_, &lfs3->mroot, -2, 0,
                NULL,
                -1, (const lfs3_rattr_t[]){
                    LFS3_RATTR(LFS3_tag_MASK8 | LFS3_TAG_MTREE, 0, 1,
                        LFS3_FROM_BTREE),
                    LFS3_RATTR_ARG(&mtree_),
                    // were we committing to the mroot? include any -1 rattrs
                    (mdir->mid <= -1)
                        ? LFS3_RATTR(LFS3_tag_RATTRS, 0, 1)
                        : LFS3_RATTR_NOOP(1),
                    LFS3_RATTR_ARG(rattrs),
                    LFS3_RATTR_NULL});
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            goto failed;
        }
    }

    // need to update mroot chain?
    if (lfs3_mdir_cmp(&mroot_, &lfs3->mroot) != 0
            || (mdir->mid <= -1 && lfs3_mdir_cmp(mdir, &lfs3->mroot) != 0)) {
        // tail recurse, updating mroots until a commit sticks
        lfs3_mdir_t mrootchild;
        lfs3_mdir_t mrootchild_;
        if (lfs3_mdir_cmp(&mroot_, &lfs3->mroot) != 0) {
            mrootchild = lfs3->mroot;
            mrootchild_ = mroot_;
        } else {
            mrootchild = *mdir;
            mrootchild_ = mdir_[0];
        }
        while (lfs3_mdir_cmp(&mrootchild_, &mrootchild) != 0
                && !lfs3_mdir_ismrootanchor(&mrootchild)) {
            // find the mroot's parent
            lfs3_mdir_t mrootparent;
            err = lfs3_mroot_parent(lfs3, mrootchild.r.blocks,
                    &mrootparent);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                goto failed;
            }

            LFS3_INFO("Relocating mroot 0x{%"PRIx32",%"PRIx32"} "
                        "-> 0x{%"PRIx32",%"PRIx32"}",
                    mrootchild.r.blocks[0], mrootchild.r.blocks[1],
                    mrootchild_.r.blocks[0], mrootchild_.r.blocks[1]);

            // make sure mtree/mroot changes are on-disk before committing
            // metadata
            err = lfs3_bd_sync(lfs3, 0);
            if (err) {
                goto failed;
            }

            // xor mrootparent's cksum
            lfs3->gcksum ^= mrootparent.r.cksum;

            // commit mrootchild
            lfs3_mdir_t mrootparent_;
            err = lfs3_mdir_commit_(lfs3, &mrootparent_, &mrootparent, -2, -2,
                    NULL,
                    -1, (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_TAG_MROOT, 0, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_ARG(mrootchild_.r.blocks),
                        LFS3_RATTR_NULL});
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                goto failed;
            }

            mrootchild = mrootparent;
            mrootchild_ = mrootparent_;
        }

        // no more mroot parents? uh oh, need to extend mroot chain
        if (lfs3_mdir_cmp(&mrootchild_, &mrootchild) != 0) {
            // mrootchild should be our previous mroot anchor at this point
            LFS3_ASSERT(lfs3_mdir_ismrootanchor(&mrootchild));
            LFS3_INFO("Extending mroot 0x{%"PRIx32",%"PRIx32"}"
                        " -> 0x{%"PRIx32",%"PRIx32"}, 0x{%"PRIx32",%"PRIx32"}",
                    mrootchild.r.blocks[0], mrootchild.r.blocks[1],
                    mrootchild.r.blocks[0], mrootchild.r.blocks[1],
                    mrootchild_.r.blocks[0], mrootchild_.r.blocks[1]);

            // make sure mtree/mroot changes are on-disk before committing
            // metadata
            err = lfs3_bd_sync(lfs3, 0);
            if (err) {
                goto failed;
            }

            // commit the new mroot anchor
            lfs3_mdir_t mrootanchor_;
            err = lfs3_mdir_swap__(lfs3, &mrootanchor_, &mrootchild, true);
            if (err) {
                // bad prog? can't do much here, mroot stuck
                if (err == LFS3_ERR_CORRUPT) {
                    LFS3_ERROR("Stuck mroot 0x{%"PRIx32",%"PRIx32"}",
                            mrootanchor_.r.blocks[0],
                            mrootanchor_.r.blocks[1]);
                    return LFS3_ERR_NOSPC;
                }
                goto failed;
            }

            err = lfs3_mdir_commit__(lfs3, &mrootanchor_, -2, -2,
                    -1, (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_TAG_MAGIC, 0, 1, LFS3_FROM_LBUF, 8),
                        LFS3_RATTR_ARG("littlefs"),
                        LFS3_RATTR(LFS3_TAG_MROOT, 0, 1, LFS3_FROM_MPTR),
                        LFS3_RATTR_ARG(mrootchild_.r.blocks),
                        LFS3_RATTR_NULL});
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                // bad prog? can't do much here, mroot stuck
                if (err == LFS3_ERR_CORRUPT) {
                    LFS3_ERROR("Stuck mroot 0x{%"PRIx32",%"PRIx32"}",
                            mrootanchor_.r.blocks[0],
                            mrootanchor_.r.blocks[1]);
                    return LFS3_ERR_NOSPC;
                }
                goto failed;
            }
        }
    }

    // sync on-disk state
    err = lfs3_bd_sync(lfs3, 0);
    if (err) {
        return err;
    }

    ///////////////////////////////////////////////////////////////////////
    // success? update in-device state, we must not error at this point! //
    ///////////////////////////////////////////////////////////////////////

    // play out any rattrs that affect internal state
    mid_ = lfs3_smax(mdir->mid, -1);
    for (const lfs3_rattr_t *r = rattrs; *r; r = lfs3_rattr_next(r, &mid_)) {
        // adjust any opened mdirs
        for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
            // adjust opened mdirs?
            if (lfs3_mdir_cmp(&h->mdir, mdir) == 0
                    && h->mdir.mid >= mid_) {
                // removed?
                if (h->mdir.mid < mid_ - lfs3_rattr_weight(r)) {
                    // opened files should turn into stickynote, not
                    // have their mid removed
                    LFS3_ASSERT(lfs3_o_type(h->flags) != LFS3_TYPE_REG);
                    // so we only need to zombie dirs really
                    if (lfs3_o_type(h->flags) == LFS3_TYPE_DIR) {
                        h->flags |= LFS3_o_ZOMBIE;
                    }
                    h->mdir.mid = mid_;
                } else {
                    h->mdir.mid += lfs3_rattr_weight(r);
                }
            }
        }
    }

    // update internal mdir state
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        // avoid double updating the current mdir
        if (&h->mdir == mdir) {
            continue;
        }

        // update any mroots, this clobbers chain mroots but that's
        // better than letting them point to garbage
        if (h->mdir.mid <= -1) {
            lfs3_mdir_sync(&h->mdir, &mroot_);
        // update any splits/drops
        } else if (lfs3_mdir_cmp(&h->mdir, mdir) == 0) {
            if (mdelta > 0
                    && lfs3_mrid(lfs3, h->mdir.mid)
                        >= (lfs3_srid_t)mdir_[0].r.weight) {
                h->mdir.mid += (1 << lfs3->mbits) - mdir_[0].r.weight;
                lfs3_mdir_sync(&h->mdir, &mdir_[1]);
            } else {
                lfs3_mdir_sync(&h->mdir, &mdir_[0]);
            }
        } else if (h->mdir.mid > mdir->mid) {
            h->mdir.mid += mdelta;
        }
    }

    // update mdir to follow requested rid
    if (mdir->mid <= -1 && lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0) {
        lfs3_mdir_sync(mdir, &mroot_);
    } else if (mdelta > 0
            && lfs3_mrid(lfs3, mdir->mid)
                >= (lfs3_srid_t)mdir_[0].r.weight) {
        mdir->mid += (1 << lfs3->mbits) - mdir_[0].r.weight;
        lfs3_mdir_sync(mdir, &mdir_[1]);
    } else {
        lfs3_mdir_sync(mdir, &mdir_[0]);
    }

    // update mroot and mtree
    lfs3_mdir_sync(&lfs3->mroot, &mroot_);
    lfs3->mtree = mtree_;

    // update any staged bshrubs
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        // update the shrub
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG) {
            // if we moved a shrub, we also need to discard any leaves
            // that moved
            if (((lfs3_file_t*)h)->bshrub_.blocks[0]
                        != ((lfs3_file_t*)h)->bshrub.blocks[0]
                    && lfs3_bptr_block(&((lfs3_file_t*)h)->leaf.bptr)
                        == ((lfs3_file_t*)h)->bshrub.blocks[0]) {
                lfs3_file_discardleaf((lfs3_file_t*)h);
            }

            ((lfs3_file_t*)h)->bshrub = ((lfs3_file_t*)h)->bshrub_;
        }
    }

    // update any gstate changes
    lfs3_fs_commitgdelta(lfs3);

    #ifdef LFS3_DBGMDIRCOMMITS
    LFS3_DEBUG("Committed mdir %"PRId32" "
                "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            lfs3_dbgmbid(lfs3, mdir->mid),
            mdir->r.blocks[0], mdir->r.blocks[1],
            lfs3_rbyd_trunk(&mdir->r),
            mdir->r.weight,
            mdir->r.cksum);
    #endif
    return 0;

failed:;
    // revert any grm changes
    lfs3->grm = grm_p;
    // revert to the on-disk gcksum
    lfs3->gcksum = lfs3->gcksum_p;
    // note we do _not_ revert the on-disk gbmap
    //
    // if we did, any in-flight state would be lost
    return err;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mdir_compact(lfs3_t *lfs3, lfs3_mdir_t *mdir) {
    // the easiest way to do this is to just mark mdir as unerased
    // and call lfs3_mdir_commit
    mdir->r.eoff = -1;
    return lfs3_mdir_commit(lfs3, mdir, (const lfs3_rattr_t[]){
            LFS3_RATTR_NULL});
}
#endif



/// Mtree path/name lookup ///

// lookup names in an mdir
//
// if not found, mid will be the best place to insert
static lfs3_stag_t lfs3_mdir_namelookup(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_smid_t *mid_, lfs3_data_t *data_) {
    // lookup name in mdir
    lfs3_srid_t rid;
    lfs3_tag_t tag;
    lfs3_scmp_t cmp = lfs3_rbyd_namelookup(lfs3, &mdir->r,
            did, name, name_len,
            &rid, &tag, NULL, data_);
    if (cmp < 0) {
        // we need this in case mdir is empty
        if (mid_) {
            *mid_ = 0;
        }
        return cmp;
    }

    // adjust mid if necessary
    //
    // note missing mids end up pointing to the next mid
    lfs3_smid_t mid = LFS3_MID(lfs3,
            mdir->mid,
            (cmp < LFS3_CMP_EQ) ? rid+1 : rid);

    // map name tags to understood types
    tag = lfs3_mdir_nametag(lfs3, mdir, mid, tag);

    if (mid_) {
        *mid_ = mid;
    }
    return (cmp == LFS3_CMP_EQ)
            ? tag
            : LFS3_ERR_NOENT;
}

// lookup names in our mtree
//
// if not found, mid will be the best place to insert
static lfs3_stag_t lfs3_mtree_namelookup(lfs3_t *lfs3,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_mdir_t *mdir_, lfs3_data_t *data_) {
    // do we only have mroot?
    if (lfs3->mtree.weight == 0) {
        // treat inlined mdir as mid=0
        mdir_->mid = 0;
        lfs3_mdir_sync(mdir_, &lfs3->mroot);

    // lookup name in actual mtree
    } else {
        lfs3_bid_t bid;
        lfs3_srid_t rid;
        lfs3_stag_t tag;
        lfs3_bid_t weight;
        lfs3_data_t data;
        lfs3_scmp_t cmp = lfs3_btree_namelookup_(lfs3, &lfs3->mtree,
                did, name, name_len,
                &bid, &mdir_->r, &rid, (lfs3_tag_t*)&tag, &weight, &data);
        if (cmp < 0) {
            LFS3_ASSERT(cmp != LFS3_ERR_NOENT);
            return cmp;
        }
        LFS3_ASSERT(weight == (lfs3_bid_t)(1 << lfs3->mbits));
        LFS3_ASSERT(tag == LFS3_TAG_MNAME
                || tag == LFS3_TAG_MDIR);

        // if we found an mname, lookup the mdir
        if (tag == LFS3_TAG_MNAME) {
            tag = lfs3_rbyd_lookup(lfs3, &mdir_->r, rid, LFS3_TAG_MDIR,
                    &data);
            if (tag < 0) {
                LFS3_ASSERT(tag != LFS3_ERR_NOENT);
                return tag;
            }
        }

        // fetch the mdir
        int err = lfs3_data_fetchmdir(lfs3, &data, bid-((1 << lfs3->mbits)-1),
                mdir_);
        if (err) {
            return err;
        }
    }

    // and lookup name in our mdir
    lfs3_smid_t mid;
    lfs3_stag_t tag = lfs3_mdir_namelookup(lfs3, mdir_,
            did, name, name_len,
            &mid, data_);
    if (tag < 0 && tag != LFS3_ERR_NOENT) {
        return tag;
    }

    // update mdir with best place to insert even if we fail
    mdir_->mid = mid;
    return tag;
}


// special directory-ids
enum {
    LFS3_DID_ROOT = 0,
};

// some operations on paths
static inline lfs3_size_t lfs3_path_namelen(const char *path) {
    return lfs3_strcspn(path, "/");
}

static inline bool lfs3_path_islast(const char *path) {
    lfs3_size_t name_len = lfs3_path_namelen(path);
    return path[name_len + lfs3_strspn(path + name_len, "/")] == '\0';
}

static inline bool lfs3_path_isdir(const char *path) {
    return path[lfs3_path_namelen(path)] != '\0';
}

// lookup a full path in our mtree, updating the path as we descend
//
// the errors get a bit subtle here, and rely on what ends up in the
// path/mdir:
// - tag                                        => file found
// - LFS3_TAG_DIR, mdir.mid>=0                  => dir found
// - LFS3_TAG_DIR, mdir.mid=-1                  => root found
// - LFS3_ERR_NOENT, islast(path), !isdir(path) => file not found
// - LFS3_ERR_NOENT, islast(path), isdir(path)  => dir not found
// - LFS3_ERR_NOENT, !islast(path)              => parent not found
// - LFS3_ERR_NOTDIR                            => parent not a dir
//
// if not found, mdir/did_ will be set to the parent's mdir/did, all
// ready for file creation
//
static lfs3_stag_t lfs3_mtree_pathlookup(lfs3_t *lfs3, const char **path,
        lfs3_mdir_t *mdir_, lfs3_did_t *did_) {
    // setup root
    *mdir_ = lfs3->mroot;
    lfs3_stag_t tag = LFS3_TAG_DIR;
    lfs3_did_t did = LFS3_DID_ROOT;
    
    // we reduce path to a single name if we can find it
    const char *path_ = *path;

    // empty paths are not allowed
    if (path_[0] == '\0') {
        return LFS3_ERR_INVAL;
    }

    while (true) {
        // skip slashes if we're a directory
        if (tag == LFS3_TAG_DIR) {
            path_ += lfs3_strspn(path_, "/");
        }
        lfs3_size_t name_len = lfs3_strcspn(path_, "/");

        // skip '.'
        if (name_len == 1 && lfs3_memcmp(path_, ".", 1) == 0) {
            path_ += name_len;
            goto next;
        }

        // error on unmatched '..', trying to go above root, eh?
        if (name_len == 2 && lfs3_memcmp(path_, "..", 2) == 0) {
            return LFS3_ERR_INVAL;
        }

        // skip if matched by '..' in name
        const char *suffix = path_ + name_len;
        lfs3_size_t suffix_len;
        int depth = 1;
        while (true) {
            suffix += lfs3_strspn(suffix, "/");
            suffix_len = lfs3_strcspn(suffix, "/");
            if (suffix_len == 0) {
                break;
            }

            if (suffix_len == 1 && lfs3_memcmp(suffix, ".", 1) == 0) {
                // noop
            } else if (suffix_len == 2 && lfs3_memcmp(suffix, "..", 2) == 0) {
                depth -= 1;
                if (depth == 0) {
                    path_ = suffix + suffix_len;
                    goto next;
                }
            } else {
                depth += 1;
            }

            suffix += suffix_len;
        }

        // found end of path, we must be done parsing our path now
        if (path_[0] == '\0') {
            if (did_) {
                *did_ = did;
            }
            return tag;
        }

        // only continue if we hit a directory
        if (tag != LFS3_TAG_DIR) {
            return (tag == LFS3_tag_ORPHAN)
                    ? LFS3_ERR_NOENT
                    : LFS3_ERR_NOTDIR;
        }

        // read the next did from the mdir if this is not the root
        if (mdir_->mid != -1) {
            lfs3_data_t data;
            tag = lfs3_mdir_lookup(lfs3, mdir_, LFS3_TAG_DID,
                    &data);
            if (tag < 0) {
                return tag;
            }

            int err = lfs3_data_readleb128(lfs3, &data, &did);
            if (err) {
                return err;
            }
        }

        // update path as we parse
        *path = path_;

        // lookup up this name in the mtree
        tag = lfs3_mtree_namelookup(lfs3, did, path_, name_len,
                mdir_, NULL);
        if (tag < 0 && tag != LFS3_ERR_NOENT) {
            return tag;
        }

        if (tag == LFS3_ERR_NOENT) {
            // keep track of where to insert if we can't find path
            if (did_) {
                *did_ = did;
            }
            return LFS3_ERR_NOENT;
        }

        // go on to next name
        path_ += name_len;
    next:;
    }
}



/// Mtree traversal ///

// special metadata ids
enum {
    // mids < -1 are used to encode traversal states
    LFS3_MID_MROOTANCHOR = -5,
    LFS3_MID_MTREE       = -4,
    LFS3_MID_GBMAP       = -3,
    LFS3_MID_GBMAP_P     = -2,
    // mid = -1 is reserved for mdir-level tags
    // mids >= -1 represent mid-level tags
};

// special btree ids
enum {
    // bids < -1 are used to encode traversal states
    LFS3_BID_MDIR = -2,
    // bids >= -1 map to btrv steps
};

static void lfs3_mtrv_init(lfs3_mtrv_t *mtrv, uint32_t flags) {
    // start at the mroot anchor
    mtrv->h.mdir.mid = LFS3_MID_MROOTANCHOR;
    mtrv->u.btrv.bid = LFS3_BID_MDIR;
    mtrv->h.flags = lfs3_o_typeflags(LFS3_type_TRV) | flags;
    mtrv->h.mdir.r.weight = 0;
    mtrv->h.mdir.r.blocks[0] = -1;
    mtrv->h.mdir.r.blocks[1] = -1;
    mtrv->gcksum = 0;
}

static void lfs3_mtrv_ckpoint(lfs3_mtrv_t *mtrv) {
    // mark as ckpointed and dirty
    mtrv->h.flags |= LFS3_t_CKPOINTED | LFS3_t_DIRTY | LFS3_t_STALE;

    // when tracked, our mdir should be kept in-sync, but we need to
    // discard any btrees/bshrubs that may fall out-of-date
    //
    // this may revisit seen blocks, but that's ok because this was
    // always possible due to CoW references
    mtrv->u.btrv.bid = LFS3_BID_MDIR;
}

static void lfs3_mtrv_damage(lfs3_mtrv_t *mtrv) {
    // mark as damaged
    mtrv->h.flags |= LFS3_t_DAMAGED;
}

// low-level traversal _only_ finds blocks
static lfs3_stag_t lfs3_mtree_traverse_(lfs3_t *lfs3, lfs3_mtrv_t *mtrv,
        lfs3_bptr_t *bptr_) {
again:;
    // fetch a btree/bshrub?
    if (mtrv->u.btrv.bid == LFS3_BID_MDIR) {
        // default to null btree
        lfs3_btree_init(&mtrv->btree);
        // reset our position in the opened handles
        //
        // after traversing on-disk bshrubs/btrees, we'll need
        // to traverse any open bshrubs/btrees
        lfs3_handle_rewind(lfs3, &mtrv->h);

        // fetch mroot anchor (mdir 0x{0,1})?
        if (mtrv->h.mdir.mid == LFS3_MID_MROOTANCHOR) {
            int err = lfs3_mdir_fetch(lfs3, &mtrv->h.mdir,
                    LFS3_MID_MTREE, LFS3_MPTR_MROOTANCHOR());
            if (err) {
                return err;
            }

            // setup mtortoise to detect cycles
            mtrv->u.mtortoise.blocks[0] = mtrv->h.mdir.r.blocks[0];
            mtrv->u.mtortoise.blocks[1] = mtrv->h.mdir.r.blocks[1];
            mtrv->u.mtortoise.dist = 0;
            mtrv->u.mtortoise.nlog2 = 0;

            // traverse the mroot anchor
            bptr_->d.u.buffer = (const uint8_t*)&mtrv->h.mdir;
            return LFS3_TAG_MDIR;

        // try to fetch either another mroot in the mroot chain, or
        // the mtree if we find it
        } else if (mtrv->h.mdir.mid == LFS3_MID_MTREE) {
            lfs3_data_t data;
            lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, &mtrv->h.mdir,
                    LFS3_tag_MASK8 | LFS3_TAG_STRUCT,
                    &data);
            if (tag < 0 && tag != LFS3_ERR_NOENT) {
                return tag;
            }

            // found a new mroot?
            if (tag == LFS3_TAG_MROOT) {
                // fetch this mroot
                int err = lfs3_data_fetchmdir(lfs3, &data, LFS3_MID_MTREE,
                        &mtrv->h.mdir);
                if (err) {
                    return err;
                }

                // detect cycles with Brent's algorithm
                //
                // note we only check for cycles in the mroot chain, the
                // btree inner nodes require checksums of their pointers,
                // so creating a valid cycle is actually quite difficult
                //
                if (lfs3_mptr_cmp(
                        mtrv->h.mdir.r.blocks,
                        mtrv->u.mtortoise.blocks) == 0) {
                    LFS3_ERROR("Cycle detected during mtree traversal "
                                "0x{%"PRIx32",%"PRIx32"}",
                            mtrv->h.mdir.r.blocks[0],
                            mtrv->h.mdir.r.blocks[1]);
                    return LFS3_ERR_CORRUPT;
                }
                if (mtrv->u.mtortoise.dist
                        == (1U << mtrv->u.mtortoise.nlog2)) {
                    mtrv->u.mtortoise.blocks[0] = mtrv->h.mdir.r.blocks[0];
                    mtrv->u.mtortoise.blocks[1] = mtrv->h.mdir.r.blocks[1];
                    mtrv->u.mtortoise.dist = 0;
                    mtrv->u.mtortoise.nlog2 += 1;
                }
                mtrv->u.mtortoise.dist += 1;

                bptr_->d.u.buffer = (const uint8_t*)&mtrv->h.mdir;
                return LFS3_TAG_MDIR;

            // found an mtree?
            } else if (tag == LFS3_TAG_MTREE) {
                // fetch the root of the mtree
                int err = lfs3_data_fetchbtree(lfs3, &data,
                        &mtrv->btree);
                if (err) {
                    return err;
                }

            // found something else?
            } else if (tag != LFS3_ERR_NOENT) {
                LFS3_ERROR("Weird mroot entry? 0x%"PRIx32, tag);
                return LFS3_ERR_CORRUPT;
            }

        // traverse the gbmap if we have one
        } else if (LFS3_IFDEF_GBMAP(
                mtrv->h.mdir.mid == LFS3_MID_GBMAP
                    && lfs3_f_isgbmap(lfs3->flags)
                    && !lfs3_t_ismtreeonly(mtrv->h.flags),
                false)) {
            #ifdef LFS3_GBMAP
            mtrv->btree = lfs3->gbmap.b;
            #endif

        // traverse on-disk gbmap if it doesn't match our in-RAM
        // snapshot
        //
        // we need to include this in case the gbmap is rebuilt
        // multiple times before an mdir commit
        } else if (LFS3_IFDEF_GBMAP(
                mtrv->h.mdir.mid == LFS3_MID_GBMAP_P
                    && lfs3_f_isgbmap(lfs3->flags)
                    && !lfs3_t_ismtreeonly(mtrv->h.flags)
                    && lfs3_btree_cmp(
                        &lfs3->gbmap.b_p,
                        &lfs3->gbmap.b) != 0,
                false)) {
            #ifdef LFS3_GBMAP
            mtrv->btree = lfs3->gbmap.b_p;
            #endif

        // fetch the next btree/bshrub
        } else if (mtrv->h.mdir.mid >= 0
                && !lfs3_t_ismtreeonly(mtrv->h.flags)) {
            // try to fetch bshrub/btree, if we don't find one
            // that's ok
            int err = lfs3_mdir_fetchbshrub(lfs3, &mtrv->h.mdir,
                    &mtrv->btree);
            if (err && err != LFS3_ERR_NOENT) {
                return err;
            }
        }

        mtrv->u.btrv.bid = -1;
    }

    // traverse any btrees/bshrubs we find
    LFS3_ASSERT(mtrv->u.btrv.bid >= -1);
    while (true) {
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_btree_traverse(lfs3,
                &mtrv->btree, &mtrv->u.btrv,
                NULL, NULL, &data);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }

        // found an inner btree node?
        if (tag == LFS3_TAG_BRANCH) {
            bptr_->d = data;
            return LFS3_TAG_BRANCH;

        // found an indirect block?
        } else if (tag == LFS3_TAG_BLOCK) {
            int err = lfs3_data_readbptr(lfs3, &data,
                    bptr_);
            if (err) {
                return err;
            }

            return LFS3_TAG_BLOCK;
        }
    }

    // done with this btree/bshrub? search our opened handle list
    // for any unsynced bshrubs/btrees related to this mid
    //
    // yes this grows potentially O(n^2) in-ram, but do we care?
    //
    // note we can skip this when rdonly, which saves a bit of code
    if (!lfs3_m_isrdonly(lfs3->flags)) {
        for (lfs3_handle_t *h = mtrv->h.next; h; h = h->next) {
            // found one?
            if (h->mdir.mid == mtrv->h.mdir.mid
                    && lfs3_o_type(h->flags) == LFS3_TYPE_REG
                    && lfs3_o_needssync(h->flags)) {
                // found one!
                const lfs3_file_t *file = (const lfs3_file_t*)h;
                mtrv->btree = file->bshrub;
                mtrv->u.btrv.bid = -1;

                // move our handle to make progress
                //
                // this looks scary with lfs3_handle_seek running in
                // O(n), but, because we only visit each unique mid +
                // handle once, in total this should still run O(n^2)
                // in-ram
                lfs3_handle_seek(lfs3, &mtrv->h, &h->next);

                // wait, do we have an ungrafted leaf?
                if (lfs3_o_needsgraft(file->h.flags)) {
                    *bptr_ = file->leaf.bptr;
                    return LFS3_TAG_BLOCK;
                }

                goto again;
            }
        }
    }

    // done with mtree? exit early to avoid mid overflow
    if (mtrv->h.mdir.mid >= (lfs3_smid_t)lfs3_mtree_weight(lfs3)) {
        return LFS3_ERR_NOENT;
    // done with mdir? move to next mdir
    } else if (mtrv->h.mdir.mid >= 0
            && (lfs3_t_ismtreeonly(mtrv->h.flags)
                || lfs3_mrid(lfs3, mtrv->h.mdir.mid)
                    >= (lfs3_srid_t)mtrv->h.mdir.r.weight-1)) {
        mtrv->h.mdir.mid = lfs3_mbid(lfs3, mtrv->h.mdir.mid) + 1;
    // done with mid? move to next mid
    } else {
        mtrv->h.mdir.mid += 1;
    }
    mtrv->u.btrv.bid = LFS3_BID_MDIR;

    // fetch mdirs on first access
    //
    // note lfs3_mrid maps mids<=-1 => rid=-1
    if (lfs3_mrid(lfs3, mtrv->h.mdir.mid) == 0) {
        int err = lfs3_mtree_lookup(lfs3, mtrv->h.mdir.mid,
                &mtrv->h.mdir);
        if (err) {
            return err;
        }

        // traverse this mdir, but don't repeat the mroot
        if (lfs3->mtree.weight != 0) {
            bptr_->d.u.buffer = (const uint8_t*)&mtrv->h.mdir;
            return LFS3_TAG_MDIR;
        }
    }

    goto again;
}

// needed in lfs3_mtree_traverse
static void lfs3_alloc_setinusebptr(lfs3_t *lfs3,
        lfs3_tag_t tag, const lfs3_bptr_t *bptr);

// high-level immutable traversal, handle extra features here,
// but no mutation! (we're called in lfs3_alloc, so things would end up
// recursive, which would be a bit bad!)
static lfs3_stag_t lfs3_mtree_traverse(lfs3_t *lfs3, lfs3_mtrv_t *mtrv,
        lfs3_bptr_t *bptr_) {
    // traverse!
    lfs3_stag_t tag = lfs3_mtree_traverse_(lfs3, mtrv,
            bptr_);
    if (tag < 0) {
        // end of traversal?
        if (tag == LFS3_ERR_NOENT) {
            goto eot;
        }
        return tag;
    }

    // validate mdirs? mdir checksums are already validated in
    // lfs3_mdir_fetch, but this doesn't prevent rollback issues, where
    // the most recent commit is corrupted but a previous outdated
    // commit appears valid
    //
    // this is where the gcksum comes in, which we can recalculate to
    // check if the filesystem state on-disk is as expected
    //
    // we also compare mdir checksums with any open mdirs to try to
    // avoid traversing any outdated bshrubs/btrees
    if (tag == LFS3_TAG_MDIR
            && (lfs3_t_isckmeta(mtrv->h.flags)
                || lfs3_t_isckdata(mtrv->h.flags))) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr_->d.u.buffer;

        // check cksum matches our mroot
        if (lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0
                && mdir->r.cksum != lfs3->mroot.r.cksum) {
            LFS3_ERROR("Found mroot cksum mismatch "
                        "0x{%"PRIx32",%"PRIx32"}, "
                        "cksum %08"PRIx32" (!= %08"PRIx32")",
                    mdir->r.blocks[0],
                    mdir->r.blocks[1],
                    mdir->r.cksum,
                    lfs3->mroot.r.cksum);
            return LFS3_ERR_CORRUPT;
        }

        // check cksum matches any open mdirs
        for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
            if (lfs3_mdir_cmp(&h->mdir, mdir) == 0
                    && h->mdir.r.cksum != mdir->r.cksum) {
                LFS3_ERROR("Found mdir cksum mismatch %"PRId32" "
                            "0x{%"PRIx32",%"PRIx32"}, "
                            "cksum %08"PRIx32" (!= %08"PRIx32")",
                        lfs3_dbgmbid(lfs3, mdir->mid),
                        mdir->r.blocks[0],
                        mdir->r.blocks[1],
                        mdir->r.cksum,
                        h->mdir.r.cksum);
                return LFS3_ERR_CORRUPT;
            }
        }

        // recalculate gcksum
        mtrv->gcksum ^= mdir->r.cksum;
    }

    // validate btree nodes?
    //
    // this may end up revalidating some btree nodes when ckfetches
    // is enabled, but we need to revalidate cached btree nodes or
    // we risk missing errors in ckmeta scans
    if (tag == LFS3_TAG_BRANCH
            && (lfs3_t_isckmeta(mtrv->h.flags)
                || lfs3_t_isckdata(mtrv->h.flags))) {
        lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr_->d.u.buffer;
        int err = lfs3_rbyd_ckfetch(lfs3, rbyd,
                rbyd->blocks[0], rbyd->trunk,
                rbyd->cksum);
        if (err) {
            return err;
        }
    }

    // validate data blocks?
    if (tag == LFS3_TAG_BLOCK
            && lfs3_t_isckdata(mtrv->h.flags)) {
        int err = lfs3_bptr_ck(lfs3, bptr_);
        if (err) {
            return err;
        }
    }

    return tag;

eot:;
    // compare gcksum with in-RAM gcksum
    if ((lfs3_t_isckmeta(mtrv->h.flags)
                || lfs3_t_isckdata(mtrv->h.flags))
            && !lfs3_t_isckpointed(mtrv->h.flags)
            && mtrv->gcksum != lfs3->gcksum) {
        LFS3_ERROR("Found gcksum mismatch, cksum %08"PRIx32" (!= %08"PRIx32")",
                mtrv->gcksum,
                lfs3->gcksum);
        return LFS3_ERR_CORRUPT;
    }

    // was ckmeta/ckdata successful? we only consider our filesystem
    // checked if we weren't mutated
    if ((lfs3_t_isckmeta(mtrv->h.flags)
                || lfs3_t_isckdata(mtrv->h.flags))
            && !lfs3_t_ismtreeonly(mtrv->h.flags)
            && !lfs3_t_isckpointed(mtrv->h.flags)) {
        lfs3->flags &= ~LFS3_I_CKMETA;
    }
    if (lfs3_t_isckdata(mtrv->h.flags)
            && !lfs3_t_ismtreeonly(mtrv->h.flags)
            && !lfs3_t_isckpointed(mtrv->h.flags)) {
        lfs3->flags &= ~LFS3_I_CKDATA;
    }

    return LFS3_ERR_NOENT;
}



/// Mtree-level gc work ///

static void lfs3_mgc_init(lfs3_mgc_t *mgc, uint32_t flags) {
    lfs3_mtrv_init(&mgc->t, lfs3_o_typeflags(LFS3_type_GC) | flags);
}

// compact/evict mdirs
#ifndef LFS3_RDONLY
static int lfs3_mgc_compactmdir(lfs3_t *lfs3, lfs3_mgc_t *mgc,
        lfs3_mdir_t *mdir) {
    (void)mgc;
    // checkpoint the allocator
    int err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        return err;
    }

    if (LFS3_IFDEF_EVICT(
            lfs3_evict_needsevictionmdir(lfs3, mdir),
            false)) {
        LFS3_INFO("Evicting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                lfs3_dbgmbid(lfs3, mdir->mid),
                mdir->r.blocks[0], mdir->r.blocks[1]);
    } else {
        LFS3_INFO("Compacting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "(%"PRId32" > %"PRId32")",
                lfs3_dbgmbid(lfs3, mdir->mid),
                mdir->r.blocks[0], mdir->r.blocks[1],
                lfs3_rbyd_eoff(&mdir->r),
                (lfs3->cfg->gc_compactmeta_thresh)
                    ? lfs3->cfg->gc_compactmeta_thresh
                    : lfs3->cfg->block_size
                        - lfs3->cfg->block_size/8);
    }

    // compact/evict the mdir
    //
    // mdir compaction/eviction take the same path up until
    // lfs3_mdir_commit_, which changes behavior if it's in the
    // eviction window
    return lfs3_mdir_compact(lfs3, mdir);
}
#endif

// needed in lfs3_mgc_compactbtree
static inline void lfs3_alloc_ckpoint_(lfs3_t *lfs3);
static inline bool lfs3_alloc_cansyncgbmap(const lfs3_t *lfs3);
static int lfs3_alloc_syncgbmap(lfs3_t *lfs3);

// compact/evict btree nodes
#ifndef LFS3_RDONLY
static int lfs3_mgc_compactbtree(lfs3_t *lfs3, lfs3_mgc_t *mgc,
        lfs3_rbyd_t *rbyd) {
    (void)rbyd;
    // grab bid before checkpointing the allocator
    //
    // note using btrv.bid here is a bit of hack, for non-btree
    // nodes it usually points to the _next_ bid, so would need to
    // be adjusted (cough cough bptr eviction cough cough)
    lfs3_bid_t bid = mgc->t.u.btrv.bid;

    // checkpoint the lookahead buffer, but avoid repopulating
    // the gbmap, for a couple reasons:
    //
    // 1. we may be compacting a btree node in the gbmap,
    //    repopulating would be counterproductive and messy
    //
    // 2. even if we're not in the gbmap, when repairing blocks
    //    we _really_ don't want to write more than is necessary
    //
    lfs3_alloc_ckpoint_(lfs3);

    // mtree?
    if (mgc->t.h.mdir.mid == LFS3_MID_MTREE) {
        if (LFS3_IFDEF_EVICT(
                lfs3_evict_needsevictionrbyd(lfs3, rbyd),
                false)) {
            LFS3_INFO("Evicting mtree rbyd 0x%"PRIx32".%"PRIx32,
                    rbyd->blocks[0],
                    lfs3_rbyd_trunk(rbyd));
        } else {
            LFS3_INFO("Compacting mtree rbyd 0x%"PRIx32".%"PRIx32" "
                        "(%"PRId32" > %"PRId32")",
                    rbyd->blocks[0],
                    lfs3_rbyd_trunk(rbyd),
                    (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size)
                        ? -1
                        : (lfs3_ssize_t)lfs3_rbyd_eoff(rbyd),
                    (lfs3->cfg->gc_compactmeta_thresh)
                        ? lfs3->cfg->gc_compactmeta_thresh
                        : lfs3->cfg->block_size
                            - lfs3->cfg->block_size/8);
        }

        // lfs3_bshrub_compact_ mutates the rbyd, which may point at
        // the root, so we need to use a copy
        LFS3_ASSERT(lfs3_rbyd_cmp(&mgc->t.u.btrv.rbyd, rbyd) == 0);
        // compact/evict the btree node
        int err = lfs3_btree_compact_(lfs3, &mgc->t.btree,
                bid, &mgc->t.u.btrv.rbyd);
        if (err) {
            return err;
        }

        // commit into mroot
        LFS3_ASSERT(lfs3_mdir_cmp(&mgc->t.h.mdir, &lfs3->mroot) == 0);
        err = lfs3_mdir_commit(lfs3, &mgc->t.h.mdir,
                (const lfs3_rattr_t[]){
                    LFS3_RATTR(LFS3_tag_MASK8 | LFS3_TAG_MTREE, 0, 1,
                        LFS3_FROM_BTREE),
                    LFS3_RATTR_ARG(&mgc->t.btree),
                    LFS3_RATTR_NULL});
        if (err) {
            return err;
        }

        // update the mtree
        lfs3->mtree = mgc->t.btree;

        // if we succeed, resume btrv at the current bid
        //
        // we may need to rewalk the current btree trunk, but this at
        // least avoids O(n^2) behavior
        lfs3_btrv_seek(&mgc->t.u.btrv, bid);

    // in gbmap?
    } else if (LFS3_IFDEF_GBMAP(
            mgc->t.h.mdir.mid == LFS3_MID_GBMAP,
            false)) {
    #ifdef LFS3_GBMAP
        if (LFS3_IFDEF_EVICT(
                lfs3_evict_needsevictionrbyd(lfs3, rbyd),
                false)) {
            LFS3_INFO("Evicting gbmap rbyd 0x%"PRIx32".%"PRIx32,
                    rbyd->blocks[0],
                    lfs3_rbyd_trunk(rbyd));
        } else {
            LFS3_INFO("Compacting gbmap rbyd 0x%"PRIx32".%"PRIx32" "
                        "(%"PRId32" > %"PRId32")",
                    rbyd->blocks[0],
                    lfs3_rbyd_trunk(rbyd),
                    (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size)
                        ? -1
                        : (lfs3_ssize_t)lfs3_rbyd_eoff(rbyd),
                    (lfs3->cfg->gc_compactmeta_thresh)
                        ? lfs3->cfg->gc_compactmeta_thresh
                        : lfs3->cfg->block_size
                            - lfs3->cfg->block_size/8);
        }

        // lfs3_bshrub_compact_ mutates the rbyd, which may point at
        // the root, so we need to use a copy
        LFS3_ASSERT(lfs3_rbyd_cmp(&mgc->t.u.btrv.rbyd, rbyd) == 0);
        // compact/evict the btree node
        int err = lfs3_btree_compact_(lfs3, &mgc->t.btree,
                bid, &mgc->t.u.btrv.rbyd);
        if (err) {
            return err;
        }

        // stage gbmap before calling lfs3_alloc_syncgbmap
        lfs3->gbmap.b = mgc->t.btree;

        // sync gbmap
        err = lfs3_alloc_syncgbmap(lfs3);
        if (err) {
            return err;
        }

        // if we succeed, resume btrv at the current bid
        //
        // we may need to rewalk the current btree trunk, but this at
        // least avoids O(n^2) behavior
        lfs3_btrv_seek(&mgc->t.u.btrv, bid);
    #endif

    // in gbmap_p?
    } else if (LFS3_IFDEF_GBMAP(
            mgc->t.h.mdir.mid == LFS3_MID_GBMAP_P,
            false)) {
    #ifdef LFS3_GBMAP
        // if we're in gbmap_p, just force sync the gbmap to
        // disk, we can't mutate gbmap_p as it's in the past,
        // and there's no real reason to keep it around
        LFS3_ASSERT(lfs3_alloc_cansyncgbmap(lfs3));
        // sync gbmap
        int err = lfs3_alloc_syncgbmap(lfs3);
        if (err) {
            return err;
        }

        // _don't_ resume btrv here, the gbmap_p is no more
    #endif

    // in a file?
    } else {
        if (LFS3_IFDEF_EVICT(
                lfs3_evict_needsevictionrbyd(lfs3, rbyd),
                false)) {
            if (lfs3_bshrub_isbshrub(&mgc->t.btree)) {
                LFS3_INFO("Evicting bshrub rbyd 0x%"PRIx32".%"PRIx32,
                        rbyd->blocks[0],
                        lfs3_rbyd_trunk(rbyd));
            } else {
                LFS3_INFO("Evicting btree rbyd 0x%"PRIx32".%"PRIx32,
                        rbyd->blocks[0],
                        lfs3_rbyd_trunk(rbyd));
            }
        } else {
            if (lfs3_bshrub_isbshrub(&mgc->t.btree)) {
                LFS3_INFO("Compacting bshrub rbyd "
                            "0x%"PRIx32".%"PRIx32" "
                            "(%"PRId32" > %"PRId32")",
                        rbyd->blocks[0],
                        lfs3_rbyd_trunk(rbyd),
                        (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size)
                            ? -1
                            : (lfs3_ssize_t)lfs3_rbyd_eoff(rbyd),
                        (lfs3->cfg->gc_compactmeta_thresh)
                            ? lfs3->cfg->gc_compactmeta_thresh
                            : lfs3->cfg->block_size
                                - lfs3->cfg->block_size/8);
            } else {
                LFS3_INFO("Compacting btree rbyd "
                            "0x%"PRIx32".%"PRIx32" "
                            "(%"PRId32" > %"PRId32")",
                        rbyd->blocks[0],
                        lfs3_rbyd_trunk(rbyd),
                        (lfs3_rbyd_eoff(rbyd) >= lfs3->cfg->block_size)
                            ? -1
                            : (lfs3_ssize_t)lfs3_rbyd_eoff(rbyd),
                        (lfs3->cfg->gc_compactmeta_thresh)
                            ? lfs3->cfg->gc_compactmeta_thresh
                            : lfs3->cfg->block_size
                                - lfs3->cfg->block_size/8);
            }
        }

        // this gets messy if we're a a bshrub, the easiest
        // option is to just create an ad-hoc file handle on the
        // stack
        //
        // counterintuitively this uses less ram than putting
        // the file handle in lfs3_mgc_t, because we're not on
        // the stack hot-path
        lfs3_file_t file;
        file.h.flags = lfs3_o_typeflags(LFS3_TYPE_REG)
                | LFS3_O_WRONLY
                | LFS3_o_NEEDSSYNC;
        file.h.mdir = mgc->t.h.mdir;
        file.bshrub = mgc->t.btree;

        // start tracking, this is the important bit
        lfs3_handle_open(lfs3, &file.h);
        // lfs3_bshrub_compact_ mutates the rbyd, which may point at
        // the root, so we need to use a copy
        LFS3_ASSERT(lfs3_rbyd_cmp(&mgc->t.u.btrv.rbyd, rbyd) == 0);
        // compact/evict the btree node
        int err = lfs3_bshrub_compact_(lfs3, &file.bshrub,
                bid, &mgc->t.u.btrv.rbyd);
        if (err) {
            lfs3_handle_close(lfs3, &file.h);
            return err;
        }

        // commit into the relevant mdir, if there is one
        //
        // for on-disk bshrubs, mgc is normally first in the
        // handle list, but we just opened a new handle
        if (file.h.next == &mgc->t.h) {
            err = lfs3_mdir_commit(lfs3, &mgc->t.h.mdir,
                    (const lfs3_rattr_t[]){
                        (lfs3_bshrub_isbshrub(&file.bshrub))
                            ? LFS3_RATTR(
                                LFS3_tag_MASK8 | LFS3_TAG_BSHRUB, 0, 1,
                                LFS3_FROM_SHRUB)
                            : LFS3_RATTR(
                                LFS3_tag_MASK8 | LFS3_TAG_BTREE, 0, 1,
                                LFS3_FROM_BTREE),
                        (lfs3_bshrub_isbshrub(&file.bshrub))
                            ? LFS3_RATTR_ARG(&file.bshrub_)
                            : LFS3_RATTR_ARG(&file.bshrub),
                        LFS3_RATTR_NULL});
            if (err) {
                lfs3_handle_close(lfs3, &file.h);
                return err;
            }
        }

        // update btree, shrub migration makes the old btree
        // out-of-date, so no reason to keep it around
        mgc->t.btree = file.bshrub;
        lfs3_handle_close(lfs3, &file.h);

        // update any open btree/bshrub references
        //
        // lfs3_mdir_commit eagerly migrates all shrubs during
        // compaction, so we need to trust the sync flag here
        for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
            if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                    && h->mdir.mid == mgc->t.h.mdir.mid
                    // in-sync? mgc should be the first handle in our
                    // handle list
                    && ((lfs3->handles == &mgc->t.h
                            && !lfs3_o_needssync(h->flags))
                        // unsynced? lfs3_mtree_traverse_ eagerly seeks
                        // to the next handle, so our mgc should be
                        // immediately after the handle we're operating
                        // on
                        //
                        // hacky, but the way shrubs are staged leaves
                        // us with few options
                        || h->next == &mgc->t.h)) {
                ((lfs3_file_t*)h)->bshrub = mgc->t.btree;

                // we also need to discard any fragments that
                // may be in our btree/bshrub
                if (!lfs3_bptr_isbptr(&((lfs3_file_t*)h)->leaf.bptr)) {
                    lfs3_file_discardleaf((lfs3_file_t*)h);
                }
            }
        }

        // if we succeed, resume btrv at the current bid
        //
        // we may need to rewalk the current btree trunk, but this at
        // least avoids O(n^2) behavior
        lfs3_btrv_seek(&mgc->t.u.btrv, bid);
    }

    return 0;
}
#endif

// evict data blocks
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
static int lfs3_mgc_evictbptr(lfs3_t *lfs3, lfs3_mgc_t *mgc,
        lfs3_bptr_t *bptr) {
    // grab bid/rid before checkpointing the allocator
    //
    // note using btrv.bid here is a bit of hack, for non-btree
    // nodes it points to the _next_ bid, so we need to adjust
    // it
    lfs3_sbid_t bid = mgc->t.u.btrv.bid - 1;
    lfs3_srid_t rid = mgc->t.u.btrv.rid - 1;

    // checkpoint the lookahead buffer, but avoid repopulating
    // the gbmap, when repairing blocks we _really_ don't want
    // to write more than is necessary
    lfs3_alloc_ckpoint_(lfs3);

    // did we already allocate a new block for this block? try to
    // deduplicate dags
    lfs3_evict_t *evict = lfs3_evict_evictionbptr(lfs3, bptr);
    if (!lfs3_evict_block_(evict)) {
        // allocate + evict the bptr
        int err = lfs3_bptr_evict(lfs3, &mgc->t.h.mdir, bptr);
        if (err) {
            return err;
        }

        // keep track of new block to deduplicate dags
        evict->block_ = LFS3_EVICT_ISDATA | lfs3_bptr_block(bptr);

        LFS3_INFO("Evicting bptr 0x%"PRIx32" -> 0x%"PRIx32,
                lfs3_evict_block(evict),
                lfs3_evict_block_(evict));
    }
    // update bptr
    bptr->d.u.disk.block = lfs3_evict_block_(evict);
    // clear the erased flag
    LFS3_IFDEF_CKDATACKSUMS(
            bptr->d.u.disk.cksize,
            bptr->cksize) &= ~LFS3_BPTR_ISERASED;

    // ok, that was the easy part, now we need to commit the bptr into
    // the btree, if there is one

    // in a btree?
    if (bid >= 0) {
        // TODO can we deduplicate some of this between here and
        // lfs3_mgc_compactbtree?

        // this gets messy if we're a a bshrub, the easiest
        // option is to just create an ad-hoc file handle on the
        // stack
        //
        // counterintuitively this uses less ram than putting
        // the file handle in lfs3_mgc_t, because we're not on
        // the stack hot-path
        lfs3_file_t file;
        file.h.flags = lfs3_o_typeflags(LFS3_TYPE_REG)
                | LFS3_O_WRONLY
                | LFS3_o_NEEDSSYNC;
        file.h.mdir = mgc->t.h.mdir;
        file.bshrub = mgc->t.btree;

        // start tracking, this is the important bit
        lfs3_handle_open(lfs3, &file.h);
        // commit the bptr into the tree
        int err = lfs3_bshrub_commit_(lfs3, &file.bshrub,
                bid, &mgc->t.u.btrv.rbyd, rid, (const lfs3_rattr_t[]){
                    LFS3_RATTR(LFS3_tag_MASK8 | LFS3_TAG_BLOCK, 0, 1,
                        LFS3_FROM_BPTR),
                    LFS3_RATTR_ARG(bptr),
                    LFS3_RATTR_NULL},
                0);
        if (err) {
            lfs3_handle_close(lfs3, &file.h);
            return err;
        }

        // commit into the relevant mdir, if there is one
        //
        // for on-disk bshrubs, mgc is normally first in the
        // handle list, but we just opened a new handle
        if (file.h.next == &mgc->t.h) {
            err = lfs3_mdir_commit(lfs3, &mgc->t.h.mdir,
                    (const lfs3_rattr_t[]){
                        (lfs3_bshrub_isbshrub(&file.bshrub))
                            ? LFS3_RATTR(
                                LFS3_tag_MASK8 | LFS3_TAG_BSHRUB, 0, 1,
                                LFS3_FROM_SHRUB)
                            : LFS3_RATTR(
                                LFS3_tag_MASK8 | LFS3_TAG_BTREE, 0, 1,
                                LFS3_FROM_BTREE),
                        (lfs3_bshrub_isbshrub(&file.bshrub))
                            ? LFS3_RATTR_ARG(&file.bshrub_)
                            : LFS3_RATTR_ARG(&file.bshrub),
                        LFS3_RATTR_NULL});
            if (err) {
                lfs3_handle_close(lfs3, &file.h);
                return err;
            }
        }

        // update btree, shrub migration makes the old btree
        // out-of-date, so no reason to keep it around
        mgc->t.btree = file.bshrub;
        lfs3_handle_close(lfs3, &file.h);

        // update any open btree/bshrub references
        //
        // lfs3_mdir_commit eagerly migrates all shrubs during
        // compaction, so we need to trust the sync flag here
        for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
            if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                    && h->mdir.mid == mgc->t.h.mdir.mid
                    // in-sync? mgc should be the first handle in our
                    // handle list
                    && ((lfs3->handles == &mgc->t.h
                            && !lfs3_o_needssync(h->flags))
                        // unsynced? lfs3_mtree_traverse_ eagerly seeks
                        // to the next handle, so our mgc should be
                        // immediately after the handle we're operating
                        // on
                        //
                        // hacky, but the way shrubs are staged leaves
                        // us with few options
                        || h->next == &mgc->t.h)) {
                ((lfs3_file_t*)h)->bshrub = mgc->t.btree;

                // we also need to discard any fragments that
                // may be in our btree/bshrub
                if (!lfs3_bptr_isbptr(&((lfs3_file_t*)h)->leaf.bptr)) {
                    lfs3_file_discardleaf((lfs3_file_t*)h);
                }
            }
        }

        // if we succeed, resume btrv at the current bid
        //
        // we may need to rewalk the current btree trunk, but this at
        // least avoids O(n^2) behavior
        lfs3_btrv_seek(&mgc->t.u.btrv, bid);
    }

    // update any open bptr references
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && lfs3_bptr_block(&((lfs3_file_t*)h)->leaf.bptr)
                    == lfs3_evict_block(evict)) {
            LFS3_ASSERT(lfs3_bptr_isbptr(&((lfs3_file_t*)h)->leaf.bptr));

            // if we're ungrafted, update the bptr with the new block
            if (lfs3_o_needsgraft(h->flags)) {
                // update bptr
                ((lfs3_file_t*)h)->leaf.bptr.d.u.disk.block
                        = lfs3_evict_block_(evict);
                // clear the erased flag
                LFS3_IFDEF_CKDATACKSUMS(
                            ((lfs3_file_t*)h)->leaf.bptr.d.u.disk.cksize,
                            ((lfs3_file_t*)h)->leaf.bptr.cksize)
                        &= ~LFS3_BPTR_ISERASED;
                // mark as crystallized
                h->flags &= ~LFS3_o_NEEDSCRYST;

            // otherwise the best we can do is discard and force reads
            // to find the bptr in the btree/bshrub again
            //
            // even if the blocks match, we can't be sure we didn't find
            // a dag, which risks out-of-sync issues with references in
            // the btree/bshrub
            } else {
                lfs3_file_discardleaf((lfs3_file_t*)h);
            }
        }
    }

    return 0;
}
#endif

// needed in lfs3_mtree_gc
static int lfs3_mtree_fixorphansmdir(lfs3_t *lfs3, lfs3_mdir_t *mdir);
static inline bool lfs3_alloc_canlookahead(const lfs3_t *lfs3);
static inline bool lfs3_alloc_canlookgbmap(const lfs3_t *lfs3);
static inline void lfs3_alloc_discard_(lfs3_t *lfs3);
static void lfs3_alloc_adopt(lfs3_t *lfs3, lfs3_block_t known);
static int lfs3_gbmap_discardunknown(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_block_t window, lfs3_block_t known);
static int lfs3_gbmap_setbptr(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_tag_t tag, const lfs3_bptr_t *bptr,
        lfs3_tag_t tag_);
static int lfs3_alloc_adoptgbmap(lfs3_t *lfs3,
        const lfs3_btree_t *gbmap, lfs3_block_t known);

// mid-level mutating traversal, handle extra features that require
// mutation here
//
// every call to lfs3_mtree_gc represents ~1 step of gc work
static int lfs3_mtree_gc(lfs3_t *lfs3, lfs3_mgc_t *mgc) {
    // start of traversal?
    if (mgc->t.h.mdir.mid == LFS3_MID_MROOTANCHOR) {
        #ifndef LFS3_RDONLY
        // setup lookahead stuff
        if (lfs3_gc_islookaheading(mgc->t.h.flags)
                && !lfs3_t_ismtreeonly(mgc->t.h.flags)
                && !lfs3_t_isckpointed(mgc->t.h.flags)) {
            // create a new gbmap snapshot?
            //
            // unfortunate it's not really possible to repopulate both
            // the lookahead buffer and gbmap at the same time, so
            // decide on one here
            if (LFS3_IFDEF_GBMAP(
                    lfs3_f_isgbmap(lfs3->flags)
                        // don't try to repopulate if above
                        // gc_lookgbmap_thresh, unlike the lookahead
                        // buffer, repopulating the gbmap writes to
                        // disk!
                        && lfs3_alloc_canlookgbmap(lfs3)
                        // prioritize the lookahead buffer if
                        // gc_lookahead_thresh is triggered
                        && !lfs3_alloc_canlookahead(lfs3),
                    false)) {
                #ifdef LFS3_GBMAP
                // lfs3_gbmap_discardunknown may allocate, so checkpoint
                // the lookahead buffer
                lfs3_alloc_ckpoint_(lfs3);

                // create a copy of the gbmap
                mgc->gbmap_ = lfs3->gbmap.b;

                // mark any in-use blocks as free
                //
                // we do this instead of creating a new gbmap to
                // (1) preserve any known erased/bad info and (2) try to
                // best use any in-btree erased-state
                //
                // note this discards in-use blocks in both the known
                // and unknown regions, we need to do this for the same
                // reason we discard the lookahead buffer, to avoid
                // clogging things up when gc is called frequently
                LFS3_ASSERT(lfs3->lookahead.ckpoint >= lfs3->gbmap.known);
                int err = lfs3_gbmap_discardunknown(lfs3, &mgc->gbmap_,
                        lfs3->gbmap.window, lfs3->gbmap.known);
                if (err) {
                    return err;
                }
                #endif

            // checkpoint+discard the lookahead buffer to maximize any
            // lookahead scans
            } else  {
                // checkpoint the lookahead buffer
                lfs3_alloc_ckpoint_(lfs3);

                // discard the current lookahead buffer
                //
                // note the discard is important, we need to discard
                // stale lookahead state or we risk clogging things up
                // if gc is called frequently
                //
                // consider repeatedly rewriting a file and calling gc
                // after every write, the lookahead buffer would fill up
                // with new blocks without noticing the old blocks are
                // no longer in use
                lfs3_alloc_discard_(lfs3);

                #ifdef LFS3_GBMAP
                // use weight=0 to indicate we're populating the
                // lookahead buffer and not the gbmap
                mgc->gbmap_.weight = 0;
                #endif
            }

            // keep our own ckpointed flag clear
            mgc->t.h.flags &= ~LFS3_t_CKPOINTED & ~LFS3_t_DIRTY;
        }
        #endif
    }

    // traverse!
    lfs3_bptr_t bptr;
    lfs3_stag_t tag = lfs3_mtree_traverse(lfs3, &mgc->t,
            &bptr);
    if (tag < 0) {
        // end of traversal?
        if (tag == LFS3_ERR_NOENT) {
            goto eot;
        }
        return tag;
    }

    #ifndef LFS3_RDONLY
    // note the order matters here!
    //
    // | 1. evict/repair before anything else!
    // | 2. mkconsistent
    // | 3. compactmeta (after mkconsistent)
    // v 4. lookahead (no reason to do earlier)
    //

    // evicting mdirs?
    #ifdef LFS3_EVICT
    if (tag == LFS3_TAG_MDIR
            && (lfs3_gc_isevictmetaing(mgc->t.h.flags)
                || lfs3_gc_isevictdataing(mgc->t.h.flags))
            && lfs3_evict_needsevictionmdir(lfs3,
                (lfs3_mdir_t*)bptr.d.u.buffer)) {
        // this takes the same code path as mdir compaction, with
        // lfs3_mdir_commit_ changing behavior if it's in the eviction
        // window
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr.d.u.buffer;
        uint32_t dirty = mgc->t.h.flags;
        int err = lfs3_mgc_compactmdir(lfs3, mgc, mdir);
        if (err) {
            return err;
        }

        // reset dirty/damaged flags
        mgc->t.h.flags &= ~(LFS3_t_DIRTY | LFS3_t_DAMAGED) | dirty;
        // we don't need to return early with mdirs
    }
    #endif

    // evicting btree nodes?
    #ifdef LFS3_EVICT
    if (tag == LFS3_TAG_BRANCH
            && (lfs3_gc_isevictmetaing(mgc->t.h.flags)
                || lfs3_gc_isevictdataing(mgc->t.h.flags))
            && lfs3_evict_needsevictionrbyd(lfs3,
                (lfs3_rbyd_t*)bptr.d.u.buffer)) {
        // this is humorously the same operation btree compaction
        lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr.d.u.buffer;
        uint32_t dirty = mgc->t.h.flags;
        int err = lfs3_mgc_compactbtree(lfs3, mgc, rbyd);
        if (err) {
            return err;
        }

        // reset dirty/damaged flags
        mgc->t.h.flags &= ~(LFS3_t_DIRTY | LFS3_t_DAMAGED) | dirty;
        // return early, traversal needs to restart
        return 0;
    }
    #endif

    // evicting data blocks?
    #ifdef LFS3_EVICT
    if (tag == LFS3_TAG_BLOCK
            && lfs3_gc_isevictdataing(mgc->t.h.flags)
            && lfs3_evict_needsevictionbptr(lfs3, &bptr)) {
        uint32_t dirty = mgc->t.h.flags;
        int err = lfs3_mgc_evictbptr(lfs3, mgc, &bptr);
        if (err) {
            return err;
        }

        // reset dirty/damaged flags
        mgc->t.h.flags &= ~(LFS3_t_DIRTY | LFS3_t_DAMAGED) | dirty;
        // return early, traversal needs to restart
        return 0;
    }
    #endif

    // mkconsistencing mdirs?
    if (tag == LFS3_TAG_MDIR
            && lfs3_gc_ismkconsistenting(mgc->t.h.flags)
            && lfs3_gc_ismkconsistent(lfs3->flags)) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr.d.u.buffer;
        // grm queue should be flushed before calling lfs3_mtree_gc
        LFS3_ASSERT(lfs3_grm_count(&lfs3->grm) == 0);

        uint32_t dirty = mgc->t.h.flags;
        // fix any orphans in the mdir
        int err = lfs3_mtree_fixorphansmdir(lfs3, mdir);
        if (err) {
            return err;
        }

        // reset dirty flag
        mgc->t.h.flags &= ~LFS3_t_DIRTY | dirty;

        // did this drop our mdir?
        if (mdir->mid >= 0 && mdir->r.weight == 0) {
            // bit of a hack, but rewind one mid and continue traversal
            mgc->t.h.mdir.mid -= 1;
            mgc->t.u.btrv.bid = LFS3_BID_MDIR;
            // return early, traversal needs to restart
            return 0;
        }
        // we don't need to return early with mdirs
    }

    // compacting mdirs?
    if (tag == LFS3_TAG_MDIR
            && lfs3_gc_iscompactmetaing(mgc->t.h.flags)
            // exceed compaction threshold?
            && lfs3_rbyd_eoff(&((lfs3_mdir_t*)bptr.d.u.buffer)->r)
                > ((lfs3->cfg->gc_compactmeta_thresh)
                    ? lfs3->cfg->gc_compactmeta_thresh
                    : lfs3->cfg->block_size - lfs3->cfg->block_size/8)) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr.d.u.buffer;
        uint32_t dirty = mgc->t.h.flags;
        int err = lfs3_mgc_compactmdir(lfs3, mgc, mdir);
        if (err) {
            return err;
        }

        // reset dirty flag
        mgc->t.h.flags &= ~LFS3_t_DIRTY | dirty;
        // we don't need to return early with mdirs
    }

    // evicting/compacting btree nodes?
    //
    // these are humorously the same operation
    if (tag == LFS3_TAG_BRANCH
            && lfs3_gc_iscompactmetaing(mgc->t.h.flags)) {
        // need to fetch
        int err = lfs3_rbyd_mkfetched(lfs3, (lfs3_rbyd_t*)bptr.d.u.buffer);
        if (err) {
            return err;
        }

        // exceeds compaction threshold?
        if (lfs3_rbyd_eoff((lfs3_rbyd_t*)bptr.d.u.buffer)
                > ((lfs3->cfg->gc_compactmeta_thresh)
                    ? lfs3->cfg->gc_compactmeta_thresh
                    : lfs3->cfg->block_size - lfs3->cfg->block_size/8)) {
            lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr.d.u.buffer;
            uint32_t dirty = mgc->t.h.flags;
            int err = lfs3_mgc_compactbtree(lfs3, mgc, rbyd);
            if (err) {
                return err;
            }

            // reset dirty flag
            mgc->t.h.flags &= ~LFS3_t_DIRTY | dirty;
            // return early, traversal needs to restart
            return 0;
        }
    }

    // mark in-use blocks?
    if (lfs3_gc_islookaheading(mgc->t.h.flags)
            && !lfs3_t_ismtreeonly(mgc->t.h.flags)
            && !lfs3_t_isckpointed(mgc->t.h.flags)) {
        // mark in-use blocks in gbmap?
        if (LFS3_IFDEF_GBMAP(mgc->gbmap_.weight != 0, false)) {
            #ifdef LFS3_GBMAP
            int err = lfs3_gbmap_setbptr(lfs3, &mgc->gbmap_, tag, &bptr,
                    LFS3_TAG_BMINUSE);
            if (err) {
                return err;
            }
            #endif

        // mark in-use blocks in lookahead buffer?
        } else {
            lfs3_alloc_setinusebptr(lfs3, tag, &bptr);
        }
    }
    #endif

    return 0;

eot:;
    #ifndef LFS3_RDONLY
    // was lookahead scan successful?
    if (lfs3_gc_islookaheading(mgc->t.h.flags)
            && !lfs3_t_ismtreeonly(mgc->t.h.flags)
            && !lfs3_t_isckpointed(mgc->t.h.flags)) {
        // was gbmap scan successful?
        if (LFS3_IFDEF_GBMAP(mgc->gbmap_.weight != 0, false)) {
            #ifdef LFS3_GBMAP
            int err = lfs3_alloc_adoptgbmap(lfs3, &mgc->gbmap_,
                    lfs3->lookahead.ckpoint);
            if (err) {
                return err;
            }
            #endif

        // was lookahead scan successful?
        } else {
            lfs3_alloc_adopt(lfs3, lfs3->lookahead.ckpoint);
        }
    }

    // was mkconsistent successful?
    if (lfs3_gc_ismkconsistenting(mgc->t.h.flags)
            && !lfs3_t_isdirty(mgc->t.h.flags)) {
        lfs3->flags &= ~LFS3_I_MKCONSISTENT;
    }

    // was compaction successful? note we may need multiple passes if
    // we want to be sure everything is compacted
    if (lfs3_gc_iscompactmetaing(mgc->t.h.flags)
            && !lfs3_t_isckpointed(mgc->t.h.flags)) {
        lfs3->flags &= ~LFS3_I_COMPACTMETA;
    }

    // was repair successful?
    #ifdef LFS3_EVICT
    if ((lfs3_gc_isevictmetaing(mgc->t.h.flags)
                || lfs3_gc_isevictdataing(mgc->t.h.flags))
            && !lfs3_t_isdirty(mgc->t.h.flags)
            && !LFS3_IFDEF_REPAIR(
                lfs3_t_isdamaged(mgc->t.h.flags),
                false)) {
        lfs3_evict_flush(lfs3,
                (lfs3_gc_isevictdataing(mgc->t.h.flags))
                    ? LFS3_EVICT_DATA
                    : 0);
    }
    #endif
    #endif

    return LFS3_ERR_NOENT;
}

// needed in lfs3_mgc_gc
static int lfs3_mtree_fixgrm(lfs3_t *lfs3);
static inline bool lfs3_alloc_canpreerase(const lfs3_t *lfs3);
static int lfs3_alloc_preerase(lfs3_t *lfs3);

// high-level core gc logic
//
// runs the mutating traversal until all work is completed, which may
// take multiple passes
//
// note this code looks much worse than it actually is! most of these
// massive macro messes compile into small constants
static lfs3_sblock_t lfs3_mgc_gc(lfs3_t *lfs3, lfs3_mgc_t *mgc,
        lfs3_sblock_t steps) {
    // i here is best effort, we may make multiple passes, so we
    // saturate to avoid any overflow issues
    lfs3_block_t i = 0;
    for (; steps < 0 || i < lfs3_max(steps, 1); i = lfs3_ssadd(i, 1)) {
        // do we have any pending traversal work?
        uint32_t t = ((mgc->t.h.flags
                        // mkconsistent implies repairmeta+repairdata
                        | LFS3_IFDEF_REPAIR(
                            (lfs3_gc_ismkconsistent(mgc->t.h.flags))
                                ? LFS3_GC_REPAIRMETA | LFS3_GC_REPAIRDATA
                                : 0,
                            0)
                        // ckdata implies ckmeta
                        | ((mgc->t.h.flags & LFS3_GC_CKDATA) >> 1)
                        // evict/repairdata implies evict/repairmeta
                        | LFS3_IFDEF_EVICT(
                            (mgc->t.h.flags & LFS3_gc_EVICTDATA) >> 1,
                            0))
                    // mask with pending flags
                    & lfs3->flags
                    & (LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                        | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                        | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                        | LFS3_GC_CKMETA
                        | LFS3_GC_CKDATA
                        | LFS3_IFDEF_RDONLY(0,
                            LFS3_IFDEF_EVICT(LFS3_gc_EVICTMETA, 0))
                        | LFS3_IFDEF_RDONLY(0,
                            LFS3_IFDEF_EVICT(LFS3_gc_EVICTDATA, 0))))
                // this weird shift is to let us mask out any
                // flags that change
                >> 8;

        // prioritize known repair work above anything else
        //
        // we want to trust the filesystem as little as possible in this
        // state
        #if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
        if (lfs3_gc_isevictmetaing(t)
                || lfs3_gc_isevictdataing(t)) {
            t &= ~(LFS3_gc_MKCONSISTENTING
                    | LFS3_gc_LOOKAHEADING
                    | LFS3_gc_COMPACTMETAING
                    // ckmeta/data is questionable here, but useful for
                    // immediately aborting failed ckmeta/data
                    // traversals when repairs are possible
                    | LFS3_gc_CKMETAING
                    | LFS3_gc_CKDATAING);
        }
        #endif

        // prioritize lookahead/gbmap before any work that may need
        // to allocate (except repairs, repairs have the highest
        // priority)
        #ifndef LFS3_RDONLY
        if (lfs3_gc_islookaheading(t)) {
            t &= ~(LFS3_gc_MKCONSISTENTING
                    | LFS3_gc_COMPACTMETAING);
        }
        #endif

        // prioritize grms above other mkconsistency work
        //
        // we need to flush the grm queue before other mkconsistency
        // work as orphans can be grmed and we don't support that
        #ifndef LFS3_RDONLY
        if (lfs3_gc_ismkconsistenting(t)
                && lfs3_grm_count(&lfs3->grm) > 0) {
            t &= ~(LFS3_gc_MKCONSISTENTING
                    // also compactmeta, because mkconsistent can
                    // uncompact things
                    | LFS3_gc_COMPACTMETAING);
        }
        #endif

        // pending traversal work?
        if (t) {
            // will this traversal still make progress? no? start a new
            // traversal
            if (!(t
                    // mask out any flags that changed
                    & mgc->t.h.flags
                    // don't bother with lookahead/gbmap if we've
                    // ckpointed
                    & ~LFS3_IFDEF_RDONLY(0,
                        (lfs3_t_isckpointed(mgc->t.h.flags))
                            ? LFS3_gc_LOOKAHEADING
                            : 0)
                    // abort and restart repairs if we were damaged
                    // mid-traversal
                    & ~LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_REPAIR(
                            (lfs3_t_isdamaged(mgc->t.h.flags))
                                ? LFS3_gc_EVICTMETAING
                                    | LFS3_gc_EVICTDATAING
                                : 0,
                            0))
                    // we let the other flags continue even if damaged,
                    // as they can at least make incremental
                    // improvements to the filesystem state
                    )) {
                lfs3_mgc_init(mgc, t
                        | (mgc->t.h.flags
                            & ~(LFS3_T_MTREEONLY
                                | LFS3_t_DIRTY
                                | LFS3_t_CKPOINTED
                                | LFS3_t_DAMAGED
                                | LFS3_IFDEF_RDONLY(0, LFS3_gc_MKCONSISTENTING)
                                | LFS3_IFDEF_RDONLY(0, LFS3_gc_LOOKAHEADING)
                                | LFS3_IFDEF_RDONLY(0, LFS3_gc_COMPACTMETAING)
                                | LFS3_gc_CKMETAING
                                | LFS3_gc_CKDATAING
                                | LFS3_IFDEF_RDONLY(0,
                                    LFS3_IFDEF_EVICT(
                                        LFS3_gc_EVICTMETAING, 0))
                                | LFS3_IFDEF_RDONLY(0,
                                    LFS3_IFDEF_EVICT(
                                        LFS3_gc_EVICTDATAING, 0)))));
            }

            // mask out any flags that changed
            //
            // note that even though our current API prevents flags from
            // changing mid-traversal, lfs3->flags can be updated by
            // other filesystem operations
            mgc->t.h.flags &= t
                    | ~(LFS3_IFDEF_RDONLY(0, LFS3_gc_MKCONSISTENTING)
                        | LFS3_IFDEF_RDONLY(0, LFS3_gc_LOOKAHEADING)
                        | LFS3_IFDEF_RDONLY(0, LFS3_gc_COMPACTMETAING)
                        | LFS3_gc_CKMETAING
                        | LFS3_gc_CKDATAING
                        | LFS3_IFDEF_RDONLY(0,
                            LFS3_IFDEF_EVICT(LFS3_gc_EVICTMETAING, 0))
                        | LFS3_IFDEF_RDONLY(0,
                            LFS3_IFDEF_EVICT(LFS3_gc_EVICTDATAING, 0)));

            // do we really need a full traversal?
            if (!(mgc->t.h.flags
                    & (LFS3_IFDEF_RDONLY(0, LFS3_gc_LOOKAHEADING)
                        // TODO probably not compactmeta if we don't
                        // support eviction in the future
                        | LFS3_gc_COMPACTMETAING
                        | LFS3_gc_CKMETAING
                        | LFS3_gc_CKDATAING
                        | LFS3_IFDEF_RDONLY(0,
                            LFS3_IFDEF_EVICT(LFS3_gc_EVICTMETAING, 0))
                        | LFS3_IFDEF_RDONLY(0,
                            LFS3_IFDEF_EVICT(LFS3_gc_EVICTDATAING, 0))))) {
                mgc->t.h.flags |= LFS3_T_MTREEONLY;
            }

            // progress gc
            int err = lfs3_mtree_gc(lfs3, mgc);
            if (err && err != LFS3_ERR_NOENT) {
                // reset traversal if we run into any errors
                mgc->t.h.flags
                        &= ~(LFS3_IFDEF_RDONLY(0, LFS3_gc_MKCONSISTENTING)
                            | LFS3_IFDEF_RDONLY(0, LFS3_gc_LOOKAHEADING)
                            | LFS3_IFDEF_RDONLY(0, LFS3_gc_COMPACTMETAING)
                            | LFS3_gc_CKMETAING
                            | LFS3_gc_CKDATAING
                            | LFS3_IFDEF_RDONLY(0,
                                LFS3_IFDEF_EVICT(LFS3_gc_EVICTMETAING, 0))
                            | LFS3_IFDEF_RDONLY(0,
                                LFS3_IFDEF_EVICT(LFS3_gc_EVICTDATAING, 0)));
                return err;
            }

            // end of traversal?
            if (err == LFS3_ERR_NOENT) {
                mgc->t.h.flags
                        &= ~(LFS3_IFDEF_RDONLY(0, LFS3_gc_MKCONSISTENTING)
                            | LFS3_IFDEF_RDONLY(0, LFS3_gc_LOOKAHEADING)
                            | LFS3_IFDEF_RDONLY(0, LFS3_gc_COMPACTMETAING)
                            | LFS3_gc_CKMETAING
                            | LFS3_gc_CKDATAING
                            | LFS3_IFDEF_RDONLY(0,
                                LFS3_IFDEF_EVICT(LFS3_gc_EVICTMETAING, 0))
                            | LFS3_IFDEF_RDONLY(0,
                                LFS3_IFDEF_EVICT(LFS3_gc_EVICTDATAING, 0)));
            }

        // check for any grms, note the above logic prioritizes this
        // over other mkconsistency work
        } else if (LFS3_IFDEF_RDONLY(false,
                lfs3_gc_ismkconsistent(mgc->t.h.flags)
                    && lfs3_grm_count(&lfs3->grm) > 0)) {
            #ifndef LFS3_RDONLY
            // fix pending grms
            uint32_t dirty = mgc->t.h.flags;
            int err = lfs3_mtree_fixgrm(lfs3);
            if (err) {
                return err;
            }
            // reset dirty flag
            mgc->t.h.flags &= ~LFS3_t_DIRTY | dirty;
            #endif

        // lower priority, but can we preerase blocks?
        } else if (LFS3_IFDEF_RDONLY(
                false,
                LFS3_IFDEF_PREERASE(
                    lfs3_gc_ispreerase(mgc->t.h.flags)
                        && lfs3_alloc_canpreerase(lfs3),
                    false))) {
            #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
            // checkpoint the lookahead buffer, we need to signal that
            // we're mutating, but try not to mess with the gbmap more
            // than is necessary
            uint32_t dirty = mgc->t.h.flags;
            lfs3_alloc_ckpoint_(lfs3);

            // preerase
            int err = lfs3_alloc_preerase(lfs3);
            if (err && err != LFS3_ERR_NOENT) {
                return err;
            }

            // reset dirty flag
            mgc->t.h.flags &= ~LFS3_t_DIRTY | dirty;
            #endif

        // if we have nothing else to do, try to commit the gbmap to
        // disk so it's recoverable if we lose power
        } else if (LFS3_IFDEF_RDONLY(
                false,
                LFS3_IFDEF_GBMAP(
                    (lfs3_gc_islookahead(mgc->t.h.flags)
                            || LFS3_IFDEF_PREERASE(
                                lfs3_gc_ispreerase(mgc->t.h.flags),
                                0))
                        && lfs3_alloc_cansyncgbmap(lfs3),
                    false))) {
            #if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
            // checkpoint the lookahead buffer, we need to signal that
            // we're mutating, but try not to mess with the gbmap more
            // than is necessary
            uint32_t dirty = mgc->t.h.flags;
            lfs3_alloc_ckpoint_(lfs3);

            // sync gbmap
            int err = lfs3_alloc_syncgbmap(lfs3);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // reset dirty flag
            mgc->t.h.flags &= ~LFS3_t_DIRTY | dirty;
            #endif

        // nothing to do at all? guess we're done
        } else {
            break;
        }
    }

    return i;
}

// high-level blocking gc, a simple mgc wrapper
//
// this just calls lfs3_mgc_gc with unbounded steps
static int lfs3_fs_gc_(lfs3_t *lfs3, uint32_t flags) {
    // if cking, set needs-ck flags, this has the side-effect of
    // signaling ck work is incomplete if we encounter an error, which
    // is probably a good thing
    lfs3->flags |= flags & (LFS3_I_CKMETA | LFS3_I_CKDATA);

    // run lfs3_mgc_gc to completion
    lfs3_mgc_t mgc;
    lfs3_mgc_init(&mgc, flags);
    lfs3_handle_open(lfs3, &mgc.t.h);
    lfs3_sblock_t steps = lfs3_mgc_gc(lfs3, &mgc, -1);
    if (steps < 0) {
        lfs3_handle_close(lfs3, &mgc.t.h);
        return steps;
    }
    lfs3_handle_close(lfs3, &mgc.t.h);

    return 0;
}


// consistency stuff

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
static int lfs3_fs_mkrepaired(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

    // nothing to repair?
    if (!lfs3_gc_isrepairmeta(lfs3->flags)
            && !lfs3_gc_isrepairdata(lfs3->flags)) {
        return 0;
    }

    // repair
    return lfs3_fs_gc_(lfs3, LFS3_GC_REPAIRMETA | LFS3_GC_REPAIRDATA);
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mtree_fixgrm(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

    if (lfs3_grm_count(&lfs3->grm) == 2) {
        LFS3_INFO("Fixing grm %"PRId32".%"PRId32" %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[1]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[1]));
    } else if (lfs3_grm_count(&lfs3->grm) == 1) {
        LFS3_INFO("Fixing grm %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]));
    }

    while (lfs3_grm_count(&lfs3->grm) > 0) {
        // find our mdir
        lfs3_mdir_t mdir;
        int err = lfs3_mtree_lookup(lfs3, lfs3->grm.queue[0],
                &mdir);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // checkpoint the allocator
        err = lfs3_alloc_ckpoint(lfs3);
        if (err) {
            return err;
        }

        // remove the rid while atomically updating our grm
        err = lfs3_mdir_commit(lfs3, &mdir, (const lfs3_rattr_t[]){
                LFS3_RATTR(LFS3_tag_GRMPOP, 0, 0),
                LFS3_RATTR(LFS3_tag_RM, -1, 0),
                LFS3_RATTR_NULL});
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mtree_fixorphansmdir(lfs3_t *lfs3, lfs3_mdir_t *mdir) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));
    // grm queue should be flushed before calling
    // lfs3_mtree_fixorphansmdir
    LFS3_ASSERT(lfs3_grm_count(&lfs3->grm) == 0);
    // save the current mid
    lfs3_mid_t mid = mdir->mid;

    // iterate through mids looking for orphans
    mdir->mid = LFS3_MID(lfs3, mdir->mid, 0);
    int err;
    while (lfs3_mrid(lfs3, mdir->mid) < (lfs3_srid_t)mdir->r.weight) {
        // is this mid open? well we're not an orphan then, skip
        //
        // note we can't rely on lfs3_mdir_lookup's internal orphan
        // checks as we also need to treat desynced/zombied files as
        // non-orphans
        if (lfs3_mid_isopen(lfs3, mdir->mid, -1)) {
            mdir->mid += 1;
            continue;
        }

        // is this mid marked as a stickynote?
        lfs3_stag_t tag = lfs3_rbyd_lookup(lfs3, &mdir->r,
                lfs3_mrid(lfs3, mdir->mid), LFS3_TAG_STICKYNOTE,
                NULL);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                mdir->mid += 1;
                continue;
            }
            err = tag;
            goto failed;
        }

        // we found an orphaned stickynote, remove
        LFS3_INFO("Fixing orphaned stickynote %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, mdir->mid),
                lfs3_dbgmrid(lfs3, mdir->mid));

        // checkpoint the allocator
        err = lfs3_alloc_ckpoint(lfs3);
        if (err) {
            return err;
        }

        // remove the orphaned stickynote
        err = lfs3_mdir_commit(lfs3, mdir, (const lfs3_rattr_t[]){
                LFS3_RATTR(LFS3_tag_RM, -1, 0),
                LFS3_RATTR_NULL});
        if (err) {
            goto failed;
        }
    }

    // restore the current mid
    mdir->mid = mid;
    return 0;

failed:;
    // restore the current mid
    mdir->mid = mid;
    return err;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mtree_fixorphans(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));
    // grm queue should be flushed before calling lfs3_mtree_fixorphans
    LFS3_ASSERT(lfs3_grm_count(&lfs3->grm) == 0);

    // LFS3_gc_MKCONSISTENTING really just removes orphans
    //
    // note we don't need to track this handle because we're only
    // mkconsistencing, most other operations need to be tracked to
    // catch dirty/ckpointed bits
    lfs3_mgc_t mgc;
    lfs3_mgc_init(&mgc,
            LFS3_GC_WRONLY | LFS3_T_MTREEONLY | LFS3_gc_MKCONSISTENTING);
    while (true) {
        int err = lfs3_mtree_gc(lfs3, &mgc);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
        }
    }

    return 0;
}
#endif

// prepare the filesystem for mutation
#ifndef LFS3_RDONLY
int lfs3_fs_mkconsistent(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

    // repair any known damage
    #ifdef LFS3_REPAIR
    int err = lfs3_fs_mkrepaired(lfs3);
    if (err) {
        return err;
    }
    #endif

    // fix pending grms
    if (lfs3_grm_count(&lfs3->grm) > 0) {
        int err = lfs3_mtree_fixgrm(lfs3);
        if (err) {
            return err;
        }
    }

    // fix orphaned stickynotes
    //
    // this must happen after fixgrm, since removing orphaned
    // stickynotes risks outdating the grm
    //
    if (lfs3_gc_ismkconsistent(lfs3->flags)) {
        int err = lfs3_mtree_fixorphans(lfs3);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif




/// Optional on-disk block map ///

#ifdef LFS3_GBMAP
static void lfs3_gbmap_init(lfs3_gbmap_t *gbmap) {
    gbmap->window = 0;
    gbmap->known = 0;
    #ifndef LFS3_RDONLY
    gbmap->next = 0;
    #endif
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    gbmap->ecksum.cksize = -1;
    gbmap->preeraser.known = 0;
    gbmap->preeraser.count = 0;
    #endif
    lfs3_btree_init(&gbmap->b);
    lfs3_btree_init(&gbmap->b_p);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static lfs3_data_t lfs3_data_fromgbmap(const lfs3_gbmap_t *gbmap,
        uint8_t buffer[static LFS3_GBMAP_DSIZE]) {
    // window should not exceed 31-bits
    LFS3_ASSERT(gbmap->window <= 0x7fffffff);
    // known should not exceed 31-bits
    LFS3_ASSERT(gbmap->known <= 0x7fffffff);

    // make sure to zero so we don't leak any info
    lfs3_memset(buffer, 0, LFS3_GBMAP_DSIZE);

    lfs3_ssize_t d = 0;
    lfs3_ssize_t d_ = lfs3_toleb128(gbmap->window, &buffer[d], 5);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    d_ = lfs3_toleb128(gbmap->known, &buffer[d], 5);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    lfs3_data_t data = lfs3_data_frombranch(&gbmap->b, &buffer[d]);
    d += lfs3_data_size(&data);

    return LFS3_DATA_BUF(buffer, lfs3_memlen(buffer, LFS3_GBMAP_DSIZE));
}
#endif

#ifdef LFS3_GBMAP
static int lfs3_data_readgbmap(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_gbmap_t *gbmap) {
    int err = lfs3_data_readleb128(lfs3, data, &gbmap->window);
    if (err) {
        return err;
    }

    err = lfs3_data_readleb128(lfs3, data, &gbmap->known);
    if (err) {
        return err;
    }

    err = lfs3_data_readbranch(lfs3, data, lfs3->block_count,
            &gbmap->b);
    if (err) {
        return err;
    }

    // we don't save free, so assume zero at first
    gbmap->next = 0;
    // keep track of the committed gbmap for traversals
    gbmap->b_p = gbmap->b;
    return 0;
}
#endif

// on-disk global block-map operations

#ifdef LFS3_GBMAP
static lfs3_stag_t lfs3_gbmap_lookupnext(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_bid_t *weight_, lfs3_ecksum_t *ecksum_) {
    lfs3_data_t data;
    lfs3_stag_t tag = lfs3_btree_lookupnext(lfs3, gbmap, bid,
            bid_, weight_, &data);
    if (tag < 0) {
        return tag;
    }

    if (ecksum_) {
        if (tag == LFS3_TAG_BMERASED && lfs3_data_size(&data) > 0) {
            int err = lfs3_data_readecksum(lfs3, &data,
                    ecksum_);
            if (err) {
                return err;
            }
        } else {
            ecksum_->cksize = -1;
        }
    }
    return tag;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_gbmap_commit(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs) {
    return lfs3_btree_commit(lfs3, gbmap, bid, rattrs);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
// note while this does takes a weight, it's limited to only a single
// range, cross-range sets are not currently not supported
//
// the purpose of weight is really just to provide a shortcut for bulk
// clearing ranges in lfs3_alloc_lookgbmap
static int lfs3_gbmap_set__(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_block_t block, lfs3_block_t weight,
        lfs3_tag_t tag, const lfs3_ecksum_t *ecksum) {
    // lookup gbmap range
    lfs3_bid_t bid__;
    lfs3_bid_t weight__;
    lfs3_ecksum_t ecksum__;
    lfs3_stag_t tag__ = lfs3_gbmap_lookupnext(lfs3, gbmap, block,
            &bid__, &weight__, &ecksum__);
    if (tag__ < 0) {
        LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
        return tag__;
    }

    // wait, already set to expected type? guess we're done
    if (tag__ == tag && lfs3_ecksum_cmp(&ecksum__, ecksum) == 0) {
        return 0;
    }

    // temporary copy, we definitely _don't_ want to leave this in a
    // weird state on error
    lfs3_btree_t gbmap_ = *gbmap;
    // mark as unfetched in case of error
    lfs3_btree_claim(gbmap);

    // weight of new range
    lfs3_bid_t weight_ = weight;

    // can we merge with right neighbor?
    //
    // note if we're in the middle of a range we should never need to
    // merge
    if (block == bid__
            && block < lfs3->block_count-1) {
        lfs3_bid_t r_bid;
        lfs3_bid_t r_weight;
        lfs3_ecksum_t r_ecksum;
        lfs3_stag_t r_tag = lfs3_gbmap_lookupnext(lfs3, gbmap, block+1,
                &r_bid, &r_weight, &r_ecksum);
        if (r_tag < 0) {
            LFS3_ASSERT(r_tag != LFS3_ERR_NOENT);
            return r_tag;
        }
        LFS3_ASSERT(r_weight == r_bid - block);

        if (r_tag == tag && lfs3_ecksum_cmp(&r_ecksum, ecksum) == 0) {
            // delete to prepare merge
            int err = lfs3_gbmap_commit(lfs3, &gbmap_,
                    r_bid, (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_tag_RM, -2, 0),
                        LFS3_RATTR_WEIGHT(-r_weight),
                        LFS3_RATTR_NULL});
            if (err) {
                return err;
            }

            // merge
            weight_ += r_weight;
        }
    }

    // can we merge with left neighbor?
    //
    // note if we're in the middle of a range we should never need to
    // merge
    if (block-(weight-1) == bid__-(weight__-1)
            && block-(weight-1) > 0) {
        lfs3_bid_t l_bid;
        lfs3_bid_t l_weight;
        lfs3_ecksum_t l_ecksum;
        lfs3_stag_t l_tag = lfs3_gbmap_lookupnext(lfs3, gbmap, block-weight,
                &l_bid, &l_weight, &l_ecksum);
        if (l_tag < 0) {
            LFS3_ASSERT(l_tag != LFS3_ERR_NOENT);
            return l_tag;
        }
        LFS3_ASSERT(l_bid == block-weight);

        if (l_tag == tag && lfs3_ecksum_cmp(&l_ecksum, ecksum) == 0) {
            // delete to prepare merge
            int err = lfs3_gbmap_commit(lfs3, &gbmap_,
                    l_bid, (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_tag_RM, -2, 0),
                        LFS3_RATTR_WEIGHT(-l_weight),
                        LFS3_RATTR_NULL});
            if (err) {
                return err;
            }

            // merge
            weight_ += l_weight;
            // adjust target block/bid
            block -= l_weight;
            bid__ -= l_weight;
        }
    }

    // commit new range
    //
    // if we're in the middle of a range, we need to split and inject
    // the new range, possibly creating two neighbors in the process
    //
    // we do this in a bit of a weird order due to how our commit API
    // works
    int err = lfs3_gbmap_commit(lfs3, &gbmap_, bid__, (const lfs3_rattr_t[]){
            (bid__-(weight__-1) < block-(weight-1))
                ? LFS3_RATTR(LFS3_tag_GROW, -2, 0)
                : LFS3_RATTR(LFS3_tag_RM, -2, 0),
            LFS3_RATTR_WEIGHT(-((bid__+1) - (block-(weight-1)))),
            (lfs3_ecksum_isecksum(ecksum))
                ? LFS3_RATTR(tag, -2, 2, LFS3_FROM_ECKSUM)
                : LFS3_RATTR(tag, -2, 2),
            LFS3_RATTR_WEIGHT(+weight_),
            LFS3_RATTR_ARG(ecksum->cksize),
            LFS3_RATTR_ARG(ecksum->cksum),
            (bid__ > block)
                ? (lfs3_ecksum_isecksum(&ecksum__))
                    ? LFS3_RATTR(tag__, -2, 2, LFS3_FROM_ECKSUM)
                    : LFS3_RATTR(tag__, -2, 2)
                : LFS3_RATTR_NOOP(3),
            LFS3_RATTR_WEIGHT(+(bid__ - block)),
            LFS3_RATTR_ARG(ecksum__.cksize),
            LFS3_RATTR_ARG(ecksum__.cksum),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }

    // done!
    *gbmap = gbmap_;
    return 0;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static const lfs3_ecksum_t lfs3_gbmap_defaultecksum = {.cksize=-1};
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_gbmap_set_(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_block_t block, lfs3_block_t weight,
        lfs3_tag_t tag, const lfs3_ecksum_t *ecksum) {
    return lfs3_gbmap_set__(lfs3, gbmap, block, weight, tag,
            // default to not-ecksum if NULL
            (ecksum) ? ecksum : &lfs3_gbmap_defaultecksum);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_gbmap_set(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_block_t block, lfs3_tag_t tag, const lfs3_ecksum_t *ecksum) {
    return lfs3_gbmap_set_(lfs3, gbmap, block, 1, tag, ecksum);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_gbmap_setbptr(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_tag_t tag, const lfs3_bptr_t *bptr,
        lfs3_tag_t tag_) {
    const lfs3_block_t *blocks;
    lfs3_size_t block_count;
    if (tag == LFS3_TAG_MDIR) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr->d.u.buffer;
        blocks = mdir->r.blocks;
        block_count = 2;

    } else if (tag == LFS3_TAG_BRANCH) {
        lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr->d.u.buffer;
        blocks = rbyd->blocks;
        block_count = 1;

    } else if (tag == LFS3_TAG_BLOCK) {
        blocks = &bptr->d.u.disk.block;
        block_count = 1;

    } else {
        LFS3_UNREACHABLE();
    }

    for (lfs3_size_t i = 0; i < block_count; i++) {
        int err = lfs3_gbmap_set(lfs3, gbmap, blocks[i], tag_, NULL);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
// note this is not completely atomic, but worst case we just end up with
// only some ranges zeroed
static int lfs3_gbmap_discardunknown(lfs3_t *lfs3, lfs3_btree_t *gbmap,
        lfs3_block_t window, lfs3_block_t known) {
    // we need to discard in-use blocks in both the known and unknown
    // regions to avoid clogging things up when gc is called frequently,
    // see the comments in lfs3_mtree_gc for why
    //
    // but the known window is still important for keeping track of
    // pre-erased blocks
    lfs3_block_t block__ = -1;
    while (true) {
        lfs3_block_t weight__;
        lfs3_stag_t tag__ = lfs3_gbmap_lookupnext(lfs3, gbmap, block__+1,
                &block__, &weight__, NULL);
        if (tag__ < 0) {
            if (tag__ == LFS3_ERR_NOENT) {
                break;
            }
            return tag__;
        }

        // translate to known-window-relative
        lfs3_block_t block___
                = ((block__-(weight__-1)) + lfs3->block_count - window)
                % lfs3->block_count;

        // mark in-use/unknown-erased ranges as free
        if (tag__ == LFS3_TAG_BMINUSE
                || (tag__ == LFS3_TAG_BMERASED && block___ >= known)) {
            // if erased limit to region in unknown window, potentially
            // slicing the range if necessary
            //
            // ugh, this ended up quite complicated!
            if (tag__ == LFS3_TAG_BMERASED) {
                lfs3_sblock_t d = lfs3_min(
                        weight__,
                        lfs3->block_count - block___);
                block__ = block__-(weight__-1) + d-1;
                weight__ = d;
            }

            // mark as free
            int err = lfs3_gbmap_set_(lfs3, gbmap, block__, weight__,
                    LFS3_TAG_BMFREE, NULL);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}
#endif



/// Block allocator ///

// needed in lfs3_alloc_ckpoint_
#ifndef LFS3_RDONLY
static void lfs3_trv_ckpoint_(lfs3_t *lfs3, lfs3_trv_t *trv);
#endif

// checkpoint only the lookahead buffer
#ifndef LFS3_RDONLY
static inline void lfs3_alloc_ckpoint_(lfs3_t *lfs3) {
    // set ckpoint = disk size
    lfs3->lookahead.ckpoint = lfs3->block_count;

    // ckpoint traversals, marking them as ckpointed + dirty and
    // resetting any btrv state
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_type_TRV
                || lfs3_o_type(h->flags) == LFS3_type_GC) {
            lfs3_mtrv_ckpoint((lfs3_mtrv_t*)h);
        }
    }
}
#endif

// needed in lfs3_alloc_ckpoint
static int lfs3_alloc_lookgbmap(lfs3_t *lfs3);

// checkpoint the allocator
//
// operations that need to alloc should call this when all in-use blocks
// are tracked, either by the filesystem or an opened handle
//
// blocks are allocated at most once, and never reallocated, between
// checkpoints
#ifndef LFS3_RDONLY
static inline int lfs3_alloc_ckpoint(lfs3_t *lfs3) {
    // checkpoint the allocator
    lfs3_alloc_ckpoint_(lfs3);

    #ifdef LFS3_GBMAP
    // do we need to repopulate the gbmap?
    if (lfs3_f_isgbmap(lfs3->flags)
            && lfs3->gbmap.known < lfs3_min(
                lfs3->cfg->lookgbmap_thresh,
                lfs3->block_count)) {
        int err = lfs3_alloc_lookgbmap(lfs3);
        if (err) {
            return err;
        }

        // checkpoint the allocator again
        lfs3_alloc_ckpoint_(lfs3);
    }
    #endif

    return 0;
}
#endif

// can we repopulate the lookahead buffer?
#ifndef LFS3_RDONLY
static inline bool lfs3_alloc_canlookahead(const lfs3_t *lfs3) {
    // below gc_lookahead_thresh?
    return lfs3_max(
                lfs3->lookahead.known,
                // don't bother if we have more information in our
                // gbmap, in theory the lookahead buffer is rarely used
                // if a gbmap is present
                LFS3_IFDEF_GBMAP(
                    (lfs3_f_isgbmap(lfs3->flags))
                        ? lfs3->gbmap.known
                        : 0,
                    0))
            <= lfs3_min(
                lfs3->cfg->gc_lookahead_thresh,
                lfs3_min(
                    8*lfs3->cfg->lookahead_size-1,
                    lfs3->block_count-1));
}
#endif

// can we repopulate the gbmap?
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static inline bool lfs3_alloc_canlookgbmap(const lfs3_t *lfs3) {
    // do we even have a gbmap?
    return lfs3_f_isgbmap(lfs3->flags)
            // below gc_lookgbmap_thresh?
            && lfs3->gbmap.known
                <= lfs3_min(
                    lfs3_max(
                        lfs3->cfg->gc_lookgbmap_thresh,
                        lfs3->cfg->lookgbmap_thresh),
                    lfs3->block_count-1);
}
#endif

// can we pre-erase?
#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
static inline bool lfs3_alloc_canpreerase(const lfs3_t *lfs3) {
    // do we even have a gbmap?
    return lfs3_f_isgbmap(lfs3->flags)
            // have we pre-erased enough blocks?
            && lfs3->gbmap.preeraser.count
                < (lfs3_block_t)lfs3->cfg->gc_preerase_count
            // are there any more blocks in our known window?
            && lfs3->gbmap.preeraser.known
                < lfs3->gbmap.known;
}
#endif

// is gbmap out-of-sync with disk?
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static inline bool lfs3_alloc_cansyncgbmap(const lfs3_t *lfs3) {
    // do we even have a gbmap?
    if (!lfs3_f_isgbmap(lfs3->flags)) {
        return false;
    }

    // just compare the on-disk encoding
    uint8_t gbmap_[LFS3_GBMAP_DSIZE];
    lfs3_data_fromgbmap(&lfs3->gbmap, gbmap_);
    return memcmp(gbmap_, lfs3->gbmap_p, LFS3_GBMAP_DSIZE) != 0;
}
#endif

// discard lookahead state
#ifndef LFS3_RDONLY
static inline void lfs3_alloc_discard_(lfs3_t *lfs3) {
    // set things to zero, note we don't mess with the window offset,
    // to avoid messing with wear-leveling
    lfs3->lookahead.known = 0;
    lfs3_memset(lfs3->lookahead.buffer, 0, lfs3->cfg->lookahead_size);
}
#endif

// discard any lookahead/gbmap state, this is necessary if block_count
// changes
#ifndef LFS3_RDONLY
static inline void lfs3_alloc_discard(lfs3_t *lfs3) {
    // discard lookahead state
    lfs3_alloc_discard_(lfs3);

    // discard the gbmap window
    #ifdef LFS3_GBMAP
    lfs3->gbmap.known = 0;
    lfs3->gbmap.next = 0;
    #endif
}
#endif

// mark a block as in-use
#ifndef LFS3_RDONLY
static void lfs3_alloc_setinuse(lfs3_t *lfs3, lfs3_block_t block) {
    // translate to lookahead-relative
    lfs3_block_t block_
            = (block + lfs3->block_count - lfs3->lookahead.window)
            % lfs3->block_count;

    if (block_ < 8*lfs3->cfg->lookahead_size) {
        // mark as in-use
        lfs3->lookahead.buffer[
                    ((lfs3->lookahead.off + block_) / 8)
                        % lfs3->cfg->lookahead_size]
                |= 1 << ((lfs3->lookahead.off + block_) % 8);
    }
}
#endif

// mark some filesystem object as in-use
#ifndef LFS3_RDONLY
static void lfs3_alloc_setinusebptr(lfs3_t *lfs3,
        lfs3_tag_t tag, const lfs3_bptr_t *bptr) {
    if (tag == LFS3_TAG_MDIR) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr->d.u.buffer;
        lfs3_alloc_setinuse(lfs3, mdir->r.blocks[0]);
        lfs3_alloc_setinuse(lfs3, mdir->r.blocks[1]);

    } else if (tag == LFS3_TAG_BRANCH) {
        lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr->d.u.buffer;
        lfs3_alloc_setinuse(lfs3, rbyd->blocks[0]);

    } else if (tag == LFS3_TAG_BLOCK) {
        lfs3_alloc_setinuse(lfs3, lfs3_bptr_block(bptr));

    } else {
        LFS3_UNREACHABLE();
    }
}
#endif

// needed in lfs3_alloc_adopt
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_alloc_findfree(lfs3_t *lfs3,
        lfs3_ecksum_t *ecksum_);
#endif

// mark any not-in-use blocks as free
#ifndef LFS3_RDONLY
static void lfs3_alloc_adopt(lfs3_t *lfs3, lfs3_block_t known) {
    // make lookahead buffer usable
    lfs3->lookahead.known = lfs3_min(
            8*lfs3->cfg->lookahead_size,
            known);

    // eagerly find the next free block so lookahead scans can make
    // the most progress
    lfs3_sblock_t block = lfs3_alloc_findfree(lfs3, NULL);
    if (block < 0 && block != LFS3_ERR_NOSPC) {
        // scanning the lookahead buffer shouldn't error
        LFS3_UNREACHABLE();
    }

    // signal that lookahead is full, unless we also want a gbmap
    if (LFS3_IFDEF_GBMAP(
            !(lfs3_f_isgbmap(lfs3->flags)
                && lfs3_alloc_canlookgbmap(lfs3)),
            true)) {
        lfs3->flags &= ~LFS3_I_LOOKAHEAD;
    }
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_alloc_adoptgbmap(lfs3_t *lfs3,
        const lfs3_btree_t *gbmap, lfs3_block_t known) {
    // adopt new gbmap
    lfs3->gbmap.known = known;
    lfs3->gbmap.b = *gbmap;
    // reset preeraser, don't worry this should rediscover any
    // erased ranges in the gbmap
    #ifdef LFS3_PREERASE
    lfs3->gbmap.preeraser.known = 0;
    lfs3->gbmap.preeraser.count = 0;
    #endif

    // eagerly find the next free block so lookgbmap scans can make
    // the most progress
    lfs3_sblock_t block = lfs3_alloc_findfree(lfs3, NULL);
    if (block < 0 && block != LFS3_ERR_NOSPC) {
        return block;
    }

    // signal that gbmap is full
    lfs3->flags &= ~LFS3_I_LOOKAHEAD;
    return 0;
}
#endif

// increment lookahead buffer
#ifndef LFS3_RDONLY
static void lfs3_alloc_inc(lfs3_t *lfs3) {
    // clear lookahead as we increment
    lfs3->lookahead.buffer[lfs3->lookahead.off / 8]
            &= ~(1 << (lfs3->lookahead.off % 8));

    // increment window/off
    lfs3->lookahead.window = (lfs3->lookahead.window + 1) % lfs3->block_count;
    lfs3->lookahead.off = (lfs3->lookahead.off + 1)
            % (8*lfs3->cfg->lookahead_size);
    // decrement size
    lfs3->lookahead.known -= lfs3_min(1, lfs3->lookahead.known);
    // decrement ckpoint
    lfs3->lookahead.ckpoint -= 1;

    // decrement gbmap known window
    #ifdef LFS3_GBMAP
    if (lfs3_f_isgbmap(lfs3->flags)) {
        lfs3->gbmap.window = (lfs3->gbmap.window + 1) % lfs3->block_count;
        lfs3->gbmap.known -= lfs3_min(1, lfs3->gbmap.known);
        #ifdef LFS3_PREERASE
        lfs3->gbmap.preeraser.known
                -= lfs3_min(1, lfs3->gbmap.preeraser.known);
        #endif
        if (lfs3->gbmap.next > 0) {
            lfs3->gbmap.next -= 1;
            #ifdef LFS3_PREERASE
            if (lfs3_ecksum_isecksum(&lfs3->gbmap.ecksum)) {
                lfs3->gbmap.preeraser.count
                        -= lfs3_min(1, lfs3->gbmap.preeraser.count);
            }
            #endif
        } else if (lfs3->gbmap.next < 0) {
            lfs3->gbmap.next += 1;
        }
    }
    #endif

    // signal that lookahead/gbmap is no longer full
    if (lfs3_alloc_canlookahead(lfs3)
            || LFS3_IFDEF_GBMAP(lfs3_alloc_canlookgbmap(lfs3), false)) {
        lfs3->flags |= LFS3_I_LOOKAHEAD;
    }
}
#endif

// find next free block in lookahead/gbmap, if there is one
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_alloc_findfree(lfs3_t *lfs3,
        lfs3_ecksum_t *ecksum_) {
    (void)ecksum_;
    while (true) {
        // known block in our gbmap?
        if (LFS3_IFDEF_GBMAP(
                lfs3_f_isgbmap(lfs3->flags) && lfs3->gbmap.known > 0,
                false)) {
            #ifdef LFS3_GBMAP
            // need to look up known block info
            if (lfs3->gbmap.next == 0) {
                lfs3_block_t block;
                lfs3_stag_t tag = lfs3_gbmap_lookupnext(lfs3, &lfs3->gbmap.b,
                        lfs3->gbmap.window,
                        &block, NULL,
                        LFS3_IFDEF_PREERASE(&lfs3->gbmap.ecksum, NULL));
                if (tag < 0) {
                    return tag;
                }
                lfs3_block_t d = lfs3_min(
                        (block+1) - lfs3->gbmap.window,
                        lfs3->gbmap.known);

                // free? erased?
                //
                // well, we can only use erased if pre-erase and
                // revperturb is enabled
                if (tag == LFS3_TAG_BMFREE
                        || LFS3_IFDEF_PREERASE(
                            tag == LFS3_TAG_BMERASED
                                && lfs3_m_isrevperturb(lfs3->flags),
                            false)) {
                    lfs3->gbmap.next = +d;

                // in-use? bad? erased? treat as in-use
                } else {
                    lfs3->gbmap.next = -d;
                }
            }

            // free block in our gbmap?
            if (lfs3->gbmap.next > 0) {
                // found a free block
                #ifdef LFS3_PREERASE
                if (ecksum_) {
                    *ecksum_ = lfs3->gbmap.ecksum;
                }
                #endif
                return lfs3->gbmap.window;
            }
            #endif

        // known block in our lookahead buffer?
        } else if (lfs3->lookahead.known > 0) {
            // free block in our lookahead buffer?
            if (!(lfs3->lookahead.buffer[lfs3->lookahead.off / 8]
                    & (1 << (lfs3->lookahead.off % 8)))) {
                // found a free block
                #ifdef LFS3_PREERASE
                if (ecksum_) {
                    ecksum_->cksize = -1;
                }
                #endif
                return lfs3->lookahead.window;
            }

        // out of known blocks
        } else {
            return LFS3_ERR_NOSPC;
        }

        lfs3_alloc_inc(lfs3);
    }
}
#endif

// allocate a block
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_alloc__(lfs3_t *lfs3, uint32_t flags,
        lfs3_ecksum_t *ecksum_) {
    (void)flags;
    while (true) {
        // scan our lookahead/gbmap for free blocks
        lfs3_sblock_t block = lfs3_alloc_findfree(lfs3,
                ecksum_);
        if (block < 0 && block != LFS3_ERR_NOSPC) {
            return block;
        }

        if (block != LFS3_ERR_NOSPC) {
            // we should never alloc blocks 0x{0,1}
            LFS3_ASSERT(block != 0 && block != 1);

            // eagerly find the next free block to maximize how many blocks
            // lfs3_alloc_ckpoint makes available for scanning
            lfs3_alloc_inc(lfs3);
            lfs3_sblock_t block_ = lfs3_alloc_findfree(lfs3, NULL);
            if (block_ < 0 && block_ != LFS3_ERR_NOSPC) {
                return block_;
            }

            return block;
        }

        // in order to keep our block allocator from spinning forever when our
        // filesystem is full, we mark points where there are no in-flight
        // allocations with a checkpoint before starting a set of allocations
        //
        // if we've looked at all blocks since the last checkpoint, we report
        // the filesystem as out of storage
        //
        if (lfs3->lookahead.ckpoint <= 0) {
            LFS3_ERROR("No more free space "
                        "(lookahead %"PRId32"/%"PRId32")",
                    lfs3->lookahead.known,
                    lfs3->block_count);
            return LFS3_ERR_NOSPC;
        }

        // no known blocks? fallback to scanning the filesystem
        //
        // traverse the filesystem, building up knowledge of what blocks are
        // in-use in the next lookahead window
        //
        lfs3_mtrv_t mtrv;
        lfs3_mtrv_init(&mtrv, LFS3_T_RDONLY | LFS3_gc_LOOKAHEADING);
        while (true) {
            lfs3_bptr_t bptr;
            lfs3_stag_t tag = lfs3_mtree_traverse(lfs3, &mtrv,
                    &bptr);
            if (tag < 0) {
                if (tag == LFS3_ERR_NOENT) {
                    break;
                }
                return tag;
            }

            // track in-use blocks
            lfs3_alloc_setinusebptr(lfs3, tag, &bptr);
        }

        // mark anything not seen as free
        lfs3_alloc_adopt(lfs3, lfs3->lookahead.ckpoint);
    }
}
#endif

// alloc and optionally erase a block
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_alloc_(lfs3_t *lfs3, uint32_t flags,
        lfs3_ecksum_t *ecksum_) {
    // we need ecksum to be non-null here, hey this is an internal
    // API anyways
    #ifdef LFS3_PREERASE
    LFS3_ASSERT(ecksum_);
    #endif

    while (true) {
        lfs3_sblock_t block = lfs3_alloc__(lfs3, flags,
                ecksum_);
        if (block < 0) {
            return block;
        }

        // hmmm, are we being evicted?
        //
        // reallocating the block would be a tad bit counterproductive
        #ifdef LFS3_EVICT
        if (lfs3_evict_needseviction(lfs3, block)) {
            continue;
        }
        #endif

        // erase requested?
        if (lfs3_alloc_iserase(flags)) {
            // pre-erased?
            if (LFS3_IFDEF_PREERASE(
                    lfs3_ecksum_isecksum(ecksum_),
                    false)) {
                #ifdef LFS3_PREERASE
                // check ecksum
                int err = lfs3_ecksum_ck(lfs3, ecksum_, block, 0);
                if (err && err != LFS3_ERR_CORRUPT) {
                    return err;
                }

                // good to go!
                if (err != LFS3_ERR_CORRUPT) {
                    #ifdef LFS3_DBGALLOCS
                    LFS3_DEBUG("Allocated block 0x%"PRIx32", "
                                "lookahead %"PRId32"/%"PRId32,
                            block,
                            lfs3->lookahead.known,
                            lfs3->block_count);
                    #endif
                    return block;
                }

                // try another block in case we have other preerased blocks
                continue;
                #endif
            }

            // needs an explicit erase
            int err = lfs3_bd_erase(lfs3, block, 0);
            if (err) {
                // bad erase? try another block
                if (err == LFS3_ERR_CORRUPT) {
                    continue;
                }
                return err;
            }
        }

        #ifdef LFS3_DBGALLOCS
        LFS3_DEBUG("Allocated block 0x%"PRIx32", "
                    "lookahead %"PRId32"/%"PRId32,
                block,
                lfs3->lookahead.known,
                lfs3->block_count);
        #endif
        return block;
    }
}
#endif

// allocate a block
//
// preerase: caller is responsible for perturbing erased-state
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_alloc(lfs3_t *lfs3, uint32_t flags) {
    #ifdef LFS3_PREERASE
    lfs3_ecksum_t ecksum_;
    #endif
    return lfs3_alloc_(lfs3, flags,
            LFS3_IFDEF_PREERASE(
                &ecksum_,
                NULL));
}
#endif

// needed in lfs3_allocwith
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_alloc_syncgbmap(lfs3_t *lfs3);
#endif

// allocate a block and sync gbmap if necessary
//
// preerase: gbmap is synced if necessary, no perturb needed
#ifndef LFS3_RDONLY
static lfs3_sblock_t lfs3_allocwith(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        uint32_t flags) {
    (void)mdir;
    #ifdef LFS3_PREERASE
    lfs3_ecksum_t ecksum_;
    #endif
    lfs3_sblock_t block = lfs3_alloc_(lfs3, flags,
            LFS3_IFDEF_PREERASE(
                &ecksum_,
                NULL));
    if (block < 0) {
        return block;
    }

    #ifdef LFS3_PREERASE
    // need to claim?
    if (lfs3_alloc_isclaim(flags)
            && lfs3_ecksum_isecksum(&ecksum_)) {
        LFS3_ASSERT(lfs3_alloc_cansyncgbmap(lfs3));
        // lfs3_mdir_commit implicitly commits any pending gbmap state
        //
        // we use the mdir here to try to distribute gbmap updates
        // around the filesystem
        //
        // note we need to not lfs3_alloc_ckpoint! the block we just
        // allocated is still very much in-flight!
        int err = lfs3_mdir_commit(lfs3, mdir, (const lfs3_rattr_t[]){
                LFS3_RATTR_NULL});
        if (err) {
            return err;
        }
    }
    #endif

    return block;
}
#endif

// rebuild the gbmap
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_alloc_lookgbmap(lfs3_t *lfs3) {
    LFS3_INFO("Repopulating gbmap (gbmap %"PRId32"/%"PRId32")",
            lfs3->gbmap.known,
            lfs3->block_count);

    // create a copy of the gbmap
    lfs3_btree_t gbmap_ = lfs3->gbmap.b;

    // mark any in-use blocks as free
    //
    // we do this instead of creating a new gbmap to (1) preserve any
    // known erased/bad info and (2) try to best use any in-btree
    // erased-state
    LFS3_ASSERT(lfs3->lookahead.ckpoint >= lfs3->gbmap.known);
    int err = lfs3_gbmap_discardunknown(lfs3, &gbmap_,
            lfs3->gbmap.window, lfs3->gbmap.known);
    if (err) {
        return err;
    }

    // traverse the filesystem, building up knowledge of what blocks are
    // in-use
    lfs3_mtrv_t mtrv;
    lfs3_mtrv_init(&mtrv, LFS3_T_RDONLY);
    while (true) {
        lfs3_bptr_t bptr;
        lfs3_stag_t tag = lfs3_mtree_traverse(lfs3, &mtrv,
                &bptr);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }

        // track in-use blocks
        err = lfs3_gbmap_setbptr(lfs3, &gbmap_, tag, &bptr,
                LFS3_TAG_BMINUSE);
        if (err) {
            return err;
        }
    }

    // update gbmap with what we found
    //
    // we don't commit this to disk immediately, instead we piggypack on
    // the next mdir commit, most writes terminate in an mdir commit so
    // this avoids extra writing at a risk of needing to repopulate the
    // gbmap if we lose power
    //
    return lfs3_alloc_adoptgbmap(lfs3, &gbmap_, lfs3->lookahead.ckpoint);
}
#endif

// try to pre-erase _one_ block
#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
static int lfs3_alloc_preerase(lfs3_t *lfs3) {
    while (lfs3->gbmap.preeraser.known < lfs3->gbmap.known) {
        // lookup next known block
        lfs3_block_t block = (lfs3->gbmap.window + lfs3->gbmap.preeraser.known)
                % lfs3->block_count;
        lfs3_bid_t block__;
        lfs3_stag_t tag__ = lfs3_gbmap_lookupnext(lfs3, &lfs3->gbmap.b, block,
                &block__, NULL, NULL);
        if (tag__ < 0) {
            LFS3_ASSERT(tag__ != LFS3_ERR_NOENT);
            return tag__;
        }
        lfs3_block_t d = lfs3_min(
                block__+1 - block,
                lfs3->gbmap.known - lfs3->gbmap.preeraser.known);

        // not free?
        if (tag__ != LFS3_TAG_BMFREE) {
            // wait, already erased?
            if (tag__ == LFS3_TAG_BMERASED) {
                lfs3->gbmap.preeraser.count += d;
            }
            lfs3->gbmap.preeraser.known += d;
            continue;
        }

        // erase!
        int err = lfs3_bd_erase(lfs3, block, 0);
        if (err) {
            return err;
        }

        // calculate erased-state checksum
        lfs3_ecksum_t ecksum;
        err = lfs3_bd_ecksum(lfs3, block, 0, 0, LFS3_BD_RELAX,
                &ecksum);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }

        // commit into gbmap
        //
        // note this relies on lfs3_gbmap_commit being atomic
        err = lfs3_gbmap_set(lfs3, &lfs3->gbmap.b, block,
                LFS3_TAG_BMERASED, &ecksum);
        if (err) {
            return err;
        }

        // successful pre-erase, kinda
        //
        // we're only actually successful if the gbmap didn't allocate
        // the block we were trying to erase
        if (((block + lfs3->block_count - lfs3->gbmap.window)
                    % lfs3->block_count)
                < lfs3->gbmap.known) {
            // increment preeraser
            lfs3->gbmap.preeraser.count += 1;
            lfs3->gbmap.preeraser.known += 1;
            // if we're in the gbmap's next range, force the allocator to
            // refetch ecksums
            if (lfs3->gbmap.preeraser.known <= lfs3_abs(lfs3->gbmap.next)) {
                lfs3->gbmap.next = 0;
            }
        }
        return 0;
    }

    return LFS3_ERR_NOENT;
}
#endif

// commit gbmap to disk
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_alloc_syncgbmap(lfs3_t *lfs3) {
    // noop if already in sync
    if (!lfs3_alloc_cansyncgbmap(lfs3)) {
        return 0;
    }

    // lfs3_mdir_commit implicitly commits any pending gbmap state
    return lfs3_mdir_commit(lfs3, &lfs3->mroot, (const lfs3_rattr_t[]){
            LFS3_RATTR_NULL});
}
#endif




/// Directory operations ///

#ifndef LFS3_RDONLY
int lfs3_mkdir(lfs3_t *lfs3, const char *path) {
    // prepare our filesystem for writing
    int err = lfs3_fs_mkconsistent(lfs3);
    if (err) {
        return err;
    }

    // lookup our parent
    lfs3_mdir_t mdir;
    lfs3_did_t did;
    lfs3_stag_t tag = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, &did);
    if (tag < 0
            && !(tag == LFS3_ERR_NOENT
                && lfs3_path_islast(path))) {
        return tag;
    }
    // already exists? pretend orphans don't exist
    if (tag != LFS3_ERR_NOENT && tag != LFS3_tag_ORPHAN) {
        return LFS3_ERR_EXIST;
    }

    // check that name fits
    const char *name = path;
    lfs3_size_t name_len = lfs3_path_namelen(path);
    if (name_len > lfs3->name_limit) {
        return LFS3_ERR_NAMETOOLONG;
    }

    // find an arbitrary directory-id (did)
    //
    // This could be anything, but we want to have few collisions while
    // also being deterministic. Here we use the checksum of the
    // filename xored with the parent's did.
    //
    //   did = parent_did xor crc32c(name)
    //
    // We use crc32c here not because it is a good hash function, but
    // because it is convenient. The did doesn't need to be reproducible
    // so this isn't a compatibility concern.
    //
    // We also truncate to make better use of our leb128 encoding. This is
    // somewhat arbitrary, but if we truncate too much we risk increasing
    // the number of collisions, so we want to aim for ~2x the number dids
    // in the system:
    //
    //   dmask = 2*dids
    //
    // But we don't actually know how many dids are in the system.
    // Fortunately, we can guess an upper bound based on the number of
    // mdirs in the mtree:
    //
    //               mdirs
    //   dmask = 2 * -----
    //                 d
    //
    // Worst case (or best case?) each directory needs 1 name tag, 1 did
    // tag, and 1 bookmark. With our current compaction strategy, each tag
    // needs 3t+4 bytes for tag+alts (see our rattr_estimate). And, if
    // we assume ~1/2 block utilization due to our mdir split threshold, we
    // can multiply everything by 2:
    //
    //   d = 3 * (3t+4) * 2 = 18t + 24
    //
    // Assuming t=4 bytes, the minimum tag encoding:
    //
    //   d = 18*4 + 24 = 96 bytes
    //
    // Rounding down to a power-of-two (again this is all arbitrary), gives
    // us ~64 bytes per directory:
    //
    //               mdirs   mdirs
    //   dmask = 2 * ----- = -----
    //                 64      32
    //
    // This is a nice number because for common NOR flash geometry,
    // 4096/32 = 128, so a filesystem with a single mdir encodes dids in a
    // single byte.
    //
    // Note we also need to be careful to catch integer overflow.
    //
    lfs3_did_t dmask
            = (1 << lfs3_min(
                lfs3_nlog2(lfs3_mtree_weight(lfs3) >> lfs3->mbits)
                    + lfs3_nlog2(lfs3->cfg->block_size/32),
                31)
            ) - 1;
    lfs3_did_t did_ = (did ^ lfs3_crc32c(0, name, name_len)) & dmask;

    // check if we have a collision, if we do, search for the next
    // available did
    while (true) {
        lfs3_stag_t tag_ = lfs3_mtree_namelookup(lfs3, did_, NULL, 0,
                &mdir, NULL);
        if (tag_ < 0) {
            if (tag_ == LFS3_ERR_NOENT) {
                break;
            }
            return tag_;
        }

        // try the next did
        did_ = (did_ + 1) & dmask;
    }

    // found a good did, now to commit to the mtree
    //
    // A problem: we need to create both:
    // 1. the metadata entry
    // 2. the bookmark entry
    //
    // To do this atomically, we first create the bookmark entry with a grm
    // to delete-self in case of powerloss, then create the metadata entry
    // while atomically cancelling the grm.
    //
    // This is done automatically by lfs3_mdir_commit to avoid issues with
    // mid updates, since the mid technically doesn't exist yet...

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        return err;
    }

    // commit our bookmark and a grm to self-remove in case of powerloss
    err = lfs3_mdir_commit(lfs3, &mdir, (const lfs3_rattr_t[]){
            LFS3_RATTR(LFS3_TAG_BOOKMARK, +1, 1, LFS3_FROM_LEB128),
            LFS3_RATTR_ARG(did_),
            LFS3_RATTR(LFS3_tag_GRMPUSH, 0, 0),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }
    LFS3_ASSERT((lfs3_smid_t)lfs3->grm.queue[0] == mdir.mid);

    // committing our bookmark may have changed the mid of our metadata entry,
    // we need to look it up again, we can at least avoid the full path walk
    lfs3_stag_t tag_ = lfs3_mtree_namelookup(lfs3, did, name, name_len,
            &mdir, NULL);
    if (tag_ < 0 && tag_ != LFS3_ERR_NOENT) {
        return tag_;
    }
    LFS3_ASSERT((tag != LFS3_ERR_NOENT)
            ? tag_ >= 0
            : tag_ == LFS3_ERR_NOENT);

    // commit our new directory into our parent, zeroing the grm in the
    // process
    err = lfs3_mdir_commit(lfs3, &mdir, (const lfs3_rattr_t[]){
            LFS3_RATTR(
                LFS3_tag_MASK12 | LFS3_TAG_DIR,
                (tag == LFS3_ERR_NOENT) ? +1 : 0, 2,
                LFS3_FROM_NAME),
            LFS3_RATTR_ARG(did),
            LFS3_RATTR_ARG(name),
            LFS3_RATTR(LFS3_TAG_DID, 0, 1, LFS3_FROM_LEB128),
            LFS3_RATTR_ARG(did_),
            LFS3_RATTR(LFS3_tag_GRMPOP, 0, 0),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }

    // update in-device state
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        // mark any clobbered uncreats as zombied
        if (tag != LFS3_ERR_NOENT
                && lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == mdir.mid) {
            h->flags = (h->flags & ~LFS3_o_NEEDSCREAT)
                    | LFS3_o_ZOMBIE
                    | LFS3_o_NEEDSSYNC
                    | LFS3_O_DESYNC;

        // update dir positions
        } else if (tag == LFS3_ERR_NOENT
                && lfs3_o_type(h->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)h)->did == did
                && h->mdir.mid >= mdir.mid) {
            ((lfs3_dir_t*)h)->pos += 1;
        }
    }

    return 0;
}
#endif

// push a bookmark to grm, but only if the directory is empty
#ifndef LFS3_RDONLY
static int lfs3_grm_pushbookmark(lfs3_t *lfs3, lfs3_grm_t *grm,
        lfs3_did_t did) {
    // first lookup the bookmark entry
    lfs3_mdir_t bookmark_mdir;
    lfs3_stag_t tag = lfs3_mtree_namelookup(lfs3, did, NULL, 0,
            &bookmark_mdir, NULL);
    if (tag < 0) {
        LFS3_ASSERT(tag != LFS3_ERR_NOENT);
        return tag;
    }
    lfs3_mid_t bookmark_mid = bookmark_mdir.mid;

    // check that the directory is empty
    bookmark_mdir.mid += 1;
    if (lfs3_mrid(lfs3, bookmark_mdir.mid)
            >= (lfs3_srid_t)bookmark_mdir.r.weight) {
        tag = lfs3_mtree_lookup(lfs3,
                lfs3_mbid(lfs3, bookmark_mdir.mid-1) + 1,
                &bookmark_mdir);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                // no more mdirs, must be empty
                goto empty;
            }
            return tag;
        }
    }

    lfs3_data_t data;
    tag = lfs3_mdir_lookup(lfs3, &bookmark_mdir,
            LFS3_tag_MASK8 | LFS3_TAG_NAME,
            &data);
    if (tag < 0) {
        LFS3_ASSERT(tag != LFS3_ERR_NOENT);
        return tag;
    }

    lfs3_did_t did_;
    int err = lfs3_data_readleb128(lfs3, &data, &did_);
    if (err) {
        return err;
    }

    if (did_ == did) {
        return LFS3_ERR_NOTEMPTY;
    }

    // is empty
empty:;
    lfs3_grm_push(grm, bookmark_mid);
    return 0;
}
#endif

#ifndef LFS3_RDONLY
int lfs3_remove(lfs3_t *lfs3, const char *path) {
    // prepare our filesystem for writing
    int err = lfs3_fs_mkconsistent(lfs3);
    if (err) {
        return err;
    }

    // lookup our entry
    lfs3_mdir_t mdir;
    lfs3_did_t did;
    lfs3_stag_t tag = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, &did);
    if (tag < 0) {
        return tag;
    }
    // pretend orphans don't exist
    if (tag == LFS3_tag_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // trying to remove the root dir?
    if (mdir.mid == -1) {
        return LFS3_ERR_BUSY;
    }

    // if we're removing a directory, we need to also remove the
    // bookmark entry
    lfs3_did_t did_ = 0;
    if (tag == LFS3_TAG_DIR) {
        // first lets figure out the did
        lfs3_data_t data_;
        lfs3_stag_t tag_ = lfs3_mdir_lookup(lfs3, &mdir, LFS3_TAG_DID,
                &data_);
        if (tag_ < 0) {
            return tag_;
        }

        err = lfs3_data_readleb128(lfs3, &data_, &did_);
        if (err) {
            return err;
        }

        // is dir empty? mark bookmark for removal with grm
        err = lfs3_grm_pushbookmark(lfs3, &lfs3->grm, did_);
        if (err) {
            return err;
        }
    }

    // are we removing an opened file?
    bool zombie = lfs3_mid_isopen(lfs3, mdir.mid, -1);

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    // remove the metadata entry
    err = lfs3_mdir_commit(lfs3, &mdir, (const lfs3_rattr_t[]){
            // create a stickynote if zombied
            //
            // we use a create+delete here to also clear any rattrs
            // and trim the entry size
            (zombie)
                ? LFS3_RATTR(LFS3_tag_MASK12 | LFS3_TAG_STICKYNOTE, 0, 2,
                    LFS3_FROM_NAME)
                : LFS3_RATTR(LFS3_tag_RM, -1, 2),
            LFS3_RATTR_ARG(did),
            LFS3_RATTR_ARG(path),
            LFS3_RATTR_NULL});
    if (err) {
        goto failed;
    }

    // update in-device state
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        // mark any clobbered uncreats as zombied
        if (zombie
                && lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == mdir.mid) {
            h->flags |= LFS3_o_NEEDSCREAT
                    | LFS3_o_ZOMBIE
                    | LFS3_o_NEEDSSYNC
                    | LFS3_O_DESYNC;

        // mark any removed dirs as zombied
        } else if (did_
                && lfs3_o_type(h->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)h)->did == did_) {
            h->flags |= LFS3_o_ZOMBIE;

        // update dir positions
        } else if (lfs3_o_type(h->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)h)->did == did
                && h->mdir.mid >= mdir.mid) {
            if (lfs3_o_iszombie(h->flags)) {
                h->flags &= ~LFS3_o_ZOMBIE;
            } else {
                ((lfs3_dir_t*)h)->pos -= 1;
            }
        }
    }

    // if we were a directory, we need to clean up, fortunately we can leave
    // this up to lfs3_mtree_fixgrm
    err = lfs3_mtree_fixgrm(lfs3);
    if (err) {
        // TODO is this the right thing to do? we should probably still
        // propagate errors to the user
        //
        // we did complete the remove, so we shouldn't error here, best
        // we can do is log this
        LFS3_WARN("Failed to clean up grm (%d)", err);
    }

    return 0;

failed:;
    // make sure grm queue is empty if we fail
    lfs3_grm_discard(&lfs3->grm);
    return err;
}
#endif

#ifndef LFS3_RDONLY
int lfs3_rename(lfs3_t *lfs3, const char *old_path, const char *new_path) {
    // prepare our filesystem for writing
    int err = lfs3_fs_mkconsistent(lfs3);
    if (err) {
        return err;
    }

    // lookup old entry
    lfs3_mdir_t old_mdir;
    lfs3_did_t old_did;
    lfs3_stag_t old_tag = lfs3_mtree_pathlookup(lfs3, &old_path,
            &old_mdir, &old_did);
    if (old_tag < 0) {
        return old_tag;
    }
    // pretend orphans don't exist
    if (old_tag == LFS3_tag_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // trying to rename the root?
    if (old_mdir.mid == -1) {
        return LFS3_ERR_BUSY;
    }

    // lookup new entry
    lfs3_mdir_t new_mdir;
    lfs3_did_t new_did;
    lfs3_stag_t new_tag = lfs3_mtree_pathlookup(lfs3, &new_path,
            &new_mdir, &new_did);
    if (new_tag < 0
            && !(new_tag == LFS3_ERR_NOENT
                && lfs3_path_islast(new_path))) {
        return new_tag;
    }

    // there are a few cases we need to watch out for
    lfs3_did_t new_did_ = 0;
    if (new_tag == LFS3_ERR_NOENT) {
        // if we're a file, don't allow trailing slashes
        if (old_tag != LFS3_TAG_DIR && lfs3_path_isdir(new_path)) {
              return LFS3_ERR_NOTDIR;
        }

        // check that name fits
        if (lfs3_path_namelen(new_path) > lfs3->name_limit) {
            return LFS3_ERR_NAMETOOLONG;
        }

    } else {
        // trying to rename the root?
        if (new_mdir.mid == -1) {
            return LFS3_ERR_BUSY;
        }

        // we allow reg <-> stickynote renaming, but renaming a non-dir
        // to a dir and a dir to a non-dir is an error
        if (old_tag != LFS3_TAG_DIR && new_tag == LFS3_TAG_DIR) {
            return LFS3_ERR_ISDIR;
        }
        if (old_tag == LFS3_TAG_DIR
                && new_tag != LFS3_TAG_DIR
                // pretend orphans don't exist
                && new_tag != LFS3_tag_ORPHAN) {
            return LFS3_ERR_NOTDIR;
        }

        // renaming to ourself is a noop
        if (old_mdir.mid == new_mdir.mid) {
            return 0;
        }

        // if our destination is a directory, we will be implicitly removing
        // the directory, we need to create a grm for this
        if (new_tag == LFS3_TAG_DIR) {
            // first lets figure out the did
            lfs3_data_t data_;
            lfs3_stag_t tag_ = lfs3_mdir_lookup(lfs3, &new_mdir, LFS3_TAG_DID,
                    &data_);
            if (tag_ < 0) {
                return tag_;
            }

            err = lfs3_data_readleb128(lfs3, &data_, &new_did_);
            if (err) {
                return err;
            }

            // is dir empty? mark bookmark for removal with grm
            err = lfs3_grm_pushbookmark(lfs3, &lfs3->grm, new_did_);
            if (err) {
                return err;
            }
        }
    }

    if (old_tag == LFS3_tag_UNKNOWN) {
        // lookup the actual tag
        old_tag = lfs3_rbyd_lookup(lfs3, &old_mdir.r,
                lfs3_mrid(lfs3, old_mdir.mid), LFS3_tag_MASK8 | LFS3_TAG_NAME,
                NULL);
        if (old_tag < 0) {
            return old_tag;
        }
    }

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    // mark old entry for removal with a grm
    lfs3_grm_push(&lfs3->grm, old_mdir.mid);

    // rename our entry, copying all tags associated with the old rid to the
    // new rid, while also marking the old rid for removal
    err = lfs3_mdir_commit(lfs3, &new_mdir, (const lfs3_rattr_t[]){
            LFS3_RATTR(
                LFS3_tag_MASK12 | old_tag,
                (new_tag == LFS3_ERR_NOENT) ? +1 : 0, 2,
                LFS3_FROM_NAME),
            LFS3_RATTR_ARG(new_did),
            LFS3_RATTR_ARG(new_path),
            LFS3_RATTR(LFS3_tag_MOVE, 0, 1),
            LFS3_RATTR_ARG(&old_mdir),
            LFS3_RATTR_NULL});
    if (err) {
        goto failed;
    }

    // update in-device state
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        // mark any clobbered uncreats as zombied
        if (new_tag != LFS3_ERR_NOENT
                && lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == new_mdir.mid) {
            h->flags = (h->flags & ~LFS3_o_NEEDSCREAT)
                    | LFS3_o_ZOMBIE
                    | LFS3_o_NEEDSSYNC
                    | LFS3_O_DESYNC;

        // update moved files with the new mdir
        } else if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == (lfs3_smid_t)lfs3->grm.queue[0]) {
            h->mdir = new_mdir;

        // mark any removed dirs as zombied
        } else if (new_did_
                && lfs3_o_type(h->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)h)->did == new_did_) {
            h->flags |= LFS3_o_ZOMBIE;

        // update dir positions
        } else if (lfs3_o_type(h->flags) == LFS3_TYPE_DIR) {
            if (new_tag == LFS3_ERR_NOENT
                    && ((lfs3_dir_t*)h)->did == new_did
                    && h->mdir.mid >= new_mdir.mid) {
                ((lfs3_dir_t*)h)->pos += 1;
            }

            if (((lfs3_dir_t*)h)->did == old_did
                    && h->mdir.mid >= (lfs3_smid_t)lfs3->grm.queue[0]) {
                if (h->mdir.mid == (lfs3_smid_t)lfs3->grm.queue[0]) {
                    h->mdir.mid += 1;
                } else {
                    ((lfs3_dir_t*)h)->pos -= 1;
                }
            }
        }
    }

    // we need to clean up any pending grms, fortunately we can leave
    // this up to lfs3_mtree_fixgrm
    err = lfs3_mtree_fixgrm(lfs3);
    if (err) {
        // TODO is this the right thing to do? we should probably still
        // propagate errors to the user
        //
        // we did complete the remove, so we shouldn't error here, best
        // we can do is log this
        LFS3_WARN("Failed to clean up grm (%d)", err);
    }

    return 0;

failed:;
    // make sure grm queue is empty if we fail
    lfs3_grm_discard(&lfs3->grm);
    return err;
}
#endif

// this just populates the info struct based on what we found
static int lfs3_stat_(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag, lfs3_data_t name,
        struct lfs3_info *info) {
    // get file type from the tag
    info->type = lfs3_tag_subtype(tag);

    // read the file name
    LFS3_ASSERT(lfs3_data_size(&name) <= LFS3_NAME_MAX);
    lfs3_ssize_t name_len = lfs3_data_read(lfs3, &name,
            info->name, LFS3_NAME_MAX);
    if (name_len < 0) {
        return name_len;
    }
    info->name[name_len] = '\0';

    // default size to zero
    info->size = 0;

    // get file size if we're a regular file
    if (tag == LFS3_TAG_REG) {
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, mdir,
                LFS3_tag_MASK8 | LFS3_TAG_STRUCT,
                &data);
        if (tag < 0 && tag != LFS3_ERR_NOENT) {
            return tag;
        }

        if (tag != LFS3_ERR_NOENT) {
            // in bshrubs/btrees, size is always the first field
            int err = lfs3_data_readleb128(lfs3, &data, &info->size);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

int lfs3_stat(lfs3_t *lfs3, const char *path, struct lfs3_info *info) {
    // lookup our entry
    lfs3_mdir_t mdir;
    lfs3_stag_t tag = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, NULL);
    if (tag < 0) {
        return tag;
    }
    // pretend orphans don't exist
    if (tag == LFS3_tag_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // special case for root
    if (mdir.mid == -1) {
        lfs3_strcpy(info->name, "/");
        info->type = LFS3_TYPE_DIR;
        info->size = 0;
        return 0;
    }

    // fill out our info struct
    return lfs3_stat_(lfs3, &mdir,
            tag, LFS3_DATA_BUF(path, lfs3_path_namelen(path)),
            info);
}

// needed in lfs3_dir_open
static int lfs3_dir_rewind_(lfs3_t *lfs3, lfs3_dir_t *dir);

int lfs3_dir_open(lfs3_t *lfs3, lfs3_dir_t *dir, const char *path) {
    // already open?
    LFS3_ASSERT(!lfs3_handle_isopen(lfs3, &dir->h));

    // setup dir state
    dir->h.flags = lfs3_o_typeflags(LFS3_TYPE_DIR);

    // lookup our directory
    lfs3_mdir_t mdir;
    lfs3_stag_t tag = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, NULL);
    if (tag < 0) {
        return tag;
    }
    // pretend orphans don't exist
    if (tag == LFS3_tag_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // read our did from the mdir, unless we're root
    if (mdir.mid == -1) {
        dir->did = 0;

    } else {
        // not a directory?
        if (tag != LFS3_TAG_DIR) {
            return LFS3_ERR_NOTDIR;
        }

        lfs3_data_t data_;
        lfs3_stag_t tag_ = lfs3_mdir_lookup(lfs3, &mdir, LFS3_TAG_DID,
                &data_);
        if (tag_ < 0) {
            return tag_;
        }

        int err = lfs3_data_readleb128(lfs3, &data_, &dir->did);
        if (err) {
            return err;
        }
    }

    // let rewind initialize the pos state
    int err = lfs3_dir_rewind_(lfs3, dir);
    if (err) {
        return err;
    }

    // add to tracked mdirs
    lfs3_handle_open(lfs3, &dir->h);
    return 0;
}

int lfs3_dir_close(lfs3_t *lfs3, lfs3_dir_t *dir) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &dir->h));

    // remove from tracked mdirs
    lfs3_handle_close(lfs3, &dir->h);
    return 0;
}

int lfs3_dir_read(lfs3_t *lfs3, lfs3_dir_t *dir, struct lfs3_info *info) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &dir->h));

    // was our dir removed?
    if (lfs3_o_iszombie(dir->h.flags)) {
        return LFS3_ERR_NOENT;
    }

    // handle dots specially
    if (dir->pos == 0) {
        lfs3_strcpy(info->name, ".");
        info->type = LFS3_TYPE_DIR;
        info->size = 0;
        dir->pos += 1;
        return 0;
    } else if (dir->pos == 1) {
        lfs3_strcpy(info->name, "..");
        info->type = LFS3_TYPE_DIR;
        info->size = 0;
        dir->pos += 1;
        return 0;
    }

    while (true) {
        // next mdir?
        if (lfs3_mrid(lfs3, dir->h.mdir.mid)
                >= (lfs3_srid_t)dir->h.mdir.r.weight) {
            int err = lfs3_mtree_lookup(lfs3,
                    lfs3_mbid(lfs3, dir->h.mdir.mid-1) + 1,
                    &dir->h.mdir);
            if (err) {
                return err;
            }
        }

        // lookup the next name tag
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, &dir->h.mdir,
                LFS3_tag_MASK8 | LFS3_TAG_NAME,
                &data);
        if (tag < 0) {
            return tag;
        }

        // get the did
        lfs3_did_t did;
        int err = lfs3_data_readleb128(lfs3, &data, &did);
        if (err) {
            return err;
        }

        // did mismatch? this terminates the dir read
        if (did != dir->did) {
            return LFS3_ERR_NOENT;
        }

        // skip orphans, we pretend these don't exist
        if (tag == LFS3_tag_ORPHAN) {
            dir->h.mdir.mid += 1;
            continue;
        }

        // fill out our info struct
        err = lfs3_stat_(lfs3, &dir->h.mdir, tag, data,
                info);
        if (err) {
            return err;
        }

        // eagerly set to next entry
        dir->h.mdir.mid += 1;
        dir->pos += 1;
        return 0;
    }
}

int lfs3_dir_seek(lfs3_t *lfs3, lfs3_dir_t *dir, lfs3_soff_t off) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &dir->h));

    // do nothing if removed
    if (lfs3_o_iszombie(dir->h.flags)) {
        return 0;
    }

    // first rewind
    int err = lfs3_dir_rewind_(lfs3, dir);
    if (err) {
        return err;
    }

    // then seek to the requested offset
    //
    // note the -2 to adjust for dot entries
    lfs3_off_t off_ = off - 2;
    while (off_ > 0) {
        // next mdir?
        if (lfs3_mrid(lfs3, dir->h.mdir.mid)
                >= (lfs3_srid_t)dir->h.mdir.r.weight) {
            int err = lfs3_mtree_lookup(lfs3,
                    lfs3_mbid(lfs3, dir->h.mdir.mid-1) + 1,
                    &dir->h.mdir);
            if (err) {
                if (err == LFS3_ERR_NOENT) {
                    break;
                }
                return err;
            }
        }

        lfs3_off_t d = lfs3_min(
                off_,
                dir->h.mdir.r.weight
                    - lfs3_mrid(lfs3, dir->h.mdir.mid));
        dir->h.mdir.mid += d;
        off_ -= d;
    }

    dir->pos = off;
    return 0;
}

lfs3_soff_t lfs3_dir_tell(lfs3_t *lfs3, lfs3_dir_t *dir) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &dir->h));

    return dir->pos;
}

static int lfs3_dir_rewind_(lfs3_t *lfs3, lfs3_dir_t *dir) {
    // do nothing if removed
    if (lfs3_o_iszombie(dir->h.flags)) {
        return 0;
    }

    // lookup our bookmark in the mtree
    lfs3_stag_t tag = lfs3_mtree_namelookup(lfs3, dir->did, NULL, 0,
            &dir->h.mdir, NULL);
    if (tag < 0) {
        LFS3_ASSERT(tag != LFS3_ERR_NOENT);
        return tag;
    }

    // eagerly set to next entry
    dir->h.mdir.mid += 1;
    // reset pos
    dir->pos = 0;
    return 0;
}

int lfs3_dir_rewind(lfs3_t *lfs3, lfs3_dir_t *dir) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &dir->h));

    return lfs3_dir_rewind_(lfs3, dir);
}




/// Custom attribute stuff ///

static int lfs3_lookupattr(lfs3_t *lfs3, const char *path, uint8_t type,
        lfs3_mdir_t *mdir_, lfs3_data_t *data_) {
    // lookup our entry
    lfs3_stag_t tag = lfs3_mtree_pathlookup(lfs3, &path,
            mdir_, NULL);
    if (tag < 0) {
        return tag;
    }
    // pretend orphans don't exist
    if (tag == LFS3_tag_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // lookup our attr
    tag = lfs3_mdir_lookup(lfs3, mdir_, LFS3_TAG_ATTR(type),
            data_);
    if (tag < 0) {
        if (tag == LFS3_ERR_NOENT) {
            return LFS3_ERR_NOATTR;
        }
        return tag;
    }

    return 0;
}

lfs3_ssize_t lfs3_getattr(lfs3_t *lfs3, const char *path, uint8_t type,
        void *buffer, lfs3_size_t size) {
    // lookup our attr
    lfs3_mdir_t mdir;
    lfs3_data_t data;
    int err = lfs3_lookupattr(lfs3, path, type,
            &mdir, &data);
    if (err) {
        return err;
    }

    // read the attr
    return lfs3_data_read(lfs3, &data, buffer, size);
}

lfs3_ssize_t lfs3_sizeattr(lfs3_t *lfs3, const char *path, uint8_t type) {
    // lookup our attr
    lfs3_mdir_t mdir;
    lfs3_data_t data;
    int err = lfs3_lookupattr(lfs3, path, type,
            &mdir, &data);
    if (err) {
        return err;
    }

    // return the attr size
    return lfs3_data_size(&data);
}

#ifndef LFS3_RDONLY
int lfs3_setattr(lfs3_t *lfs3, const char *path, uint8_t type,
        const void *buffer, lfs3_size_t size) {
    // prepare our filesystem for writing
    int err = lfs3_fs_mkconsistent(lfs3);
    if (err) {
        return err;
    }

    // lookup our attr
    lfs3_mdir_t mdir;
    lfs3_data_t data;
    err = lfs3_lookupattr(lfs3, path, type,
            &mdir, &data);
    if (err && err != LFS3_ERR_NOATTR) {
        return err;
    }

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        return err;
    }

    // commit our attr
    err = lfs3_mdir_commit(lfs3, &mdir, (const lfs3_rattr_t[]){
            LFS3_RATTR(LFS3_TAG_ATTR(type), 0, 2, LFS3_FROM_BUF),
            LFS3_RATTR_ARG(buffer),
            LFS3_RATTR_ARG(size),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }

    // update any opened files tracking custom attrs
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (!(lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == mdir.mid
                && !lfs3_o_isdesync(h->flags))) {
            continue;
        }

        lfs3_file_t *file = (lfs3_file_t*)h;
        for (lfs3_size_t i = 0; i < file->cfg->attr_count; i++) {
            if (!(file->cfg->attrs[i].type == type
                    && !lfs3_o_iswronly(file->cfg->attrs[i].flags))) {
                continue;
            }

            lfs3_size_t d = lfs3_min(size, file->cfg->attrs[i].buffer_size);
            lfs3_memcpy(file->cfg->attrs[i].buffer, buffer, d);
            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = d;
            }
        }
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
int lfs3_removeattr(lfs3_t *lfs3, const char *path, uint8_t type) {
    // prepare our filesystem for writing
    int err = lfs3_fs_mkconsistent(lfs3);
    if (err) {
        return err;
    }

    // lookup our attr
    lfs3_mdir_t mdir;
    err = lfs3_lookupattr(lfs3, path, type,
            &mdir, NULL);
    if (err) {
        return err;
    }

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        return err;
    }

    // commit our removal
    err = lfs3_mdir_commit(lfs3, &mdir, (const lfs3_rattr_t[]){
            LFS3_RATTR(LFS3_tag_RM | LFS3_TAG_ATTR(type), 0, 0),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }

    // update any opened files tracking custom attrs
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (!(lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == mdir.mid
                && !lfs3_o_isdesync(h->flags))) {
            continue;
        }

        lfs3_file_t *file = (lfs3_file_t*)h;
        for (lfs3_size_t i = 0; i < file->cfg->attr_count; i++) {
            if (!(file->cfg->attrs[i].type == type
                    && !lfs3_o_iswronly(file->cfg->attrs[i].flags))) {
                continue;
            }

            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = LFS3_ERR_NOATTR;
            }
        }
    }

    return 0;
}
#endif




/// File operations ///

// file helpers

static inline void lfs3_file_discardcache(lfs3_file_t *file) {
    file->h.flags &= ~LFS3_o_NEEDSFLUSH;
    file->cache.pos = 0;
    file->cache.size = 0;
}

static inline void lfs3_file_discardleaf(lfs3_file_t *file) {
    file->h.flags &= ~LFS3_o_NEEDSCRYST & ~LFS3_o_NEEDSGRAFT;
    file->leaf.pos = 0;
    lfs3_bptr_discard(&file->leaf.bptr);
}

static inline void lfs3_file_discardbshrub(lfs3_file_t *file) {
    lfs3_bshrub_init(&file->bshrub);
}

static inline lfs3_size_t lfs3_file_fcachesize(lfs3_t *lfs3,
        const lfs3_file_t *file) {
    return (file->cfg->fcache_buffer || file->cfg->fcache_size)
            ? file->cfg->fcache_size
            : lfs3->cfg->fcache_size;
}

static inline lfs3_off_t lfs3_file_size_(const lfs3_file_t *file) {
    return lfs3_max(
            file->cache.pos + file->cache.size,
            lfs3_max(
                file->leaf.pos + file->leaf.bptr.d.weight,
                file->bshrub.weight));
}



// file operations

static void lfs3_file_init(lfs3_file_t *file, uint32_t flags,
        const struct lfs3_file_cfg *cfg) {
    file->cfg = cfg;
    file->h.flags = lfs3_o_typeflags(LFS3_TYPE_REG) | flags;
    file->pos = 0;
    lfs3_file_discardcache(file);
    lfs3_file_discardleaf(file);
    lfs3_file_discardbshrub(file);
}

static int lfs3_file_fetch(lfs3_t *lfs3, lfs3_file_t *file, uint32_t flags) {
    // don't bother reading disk if we're not created or truncating
    if (!LFS3_IFDEF_RDONLY(
            false,
            lfs3_o_needscreat(flags) || lfs3_o_istrunc(flags))) {
        // fetch the file's bshrub/btree, if there is one
        //
        // first read into a temporary bshrub/btree, to avoid leaving
        // the file's bshrub/btree undefined on error
        lfs3_bshrub_t bshrub;
        int err = lfs3_mdir_fetchbshrub(lfs3, &file->h.mdir,
                &bshrub);
        if (err && err != LFS3_ERR_NOENT) {
            return err;
        }
        if (err != LFS3_ERR_NOENT) {
            // update with found bshrub/btree
            file->bshrub = bshrub;
        }

        // mark as in-sync
        file->h.flags &= ~LFS3_o_NEEDSSYNC;
    }

    // try to fetch any custom attributes
    for (lfs3_size_t i = 0; i < file->cfg->attr_count; i++) {
        // skip writeonly attrs
        if (lfs3_o_iswronly(file->cfg->attrs[i].flags)) {
            continue;
        }

        // don't bother reading disk if we're not created yet
        if (lfs3_o_needscreat(flags)) {
            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = LFS3_ERR_NOATTR;
            }
            continue;
        }

        // lookup the attr
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, &file->h.mdir,
                LFS3_TAG_ATTR(file->cfg->attrs[i].type),
                &data);
        if (tag < 0 && tag != LFS3_ERR_NOENT) {
            return tag;
        }

        // read the attr, if it exists
        if (tag == LFS3_ERR_NOENT
                // awkward case here if buffer_size is LFS3_ERR_NOATTR
                || file->cfg->attrs[i].buffer_size == LFS3_ERR_NOATTR) {
            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = LFS3_ERR_NOATTR;
            }
        } else {
            lfs3_ssize_t d = lfs3_data_read(lfs3, &data,
                    file->cfg->attrs[i].buffer,
                    file->cfg->attrs[i].buffer_size);
            if (d < 0) {
                return d;
            }

            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = d;
            }
        }
    }

    return 0;
}

// needed in lfs3_file_opencfg
static void lfs3_file_close_(lfs3_t *lfs3, lfs3_file_t *file);
#ifndef LFS3_RDONLY
static int lfs3_file_sync_(lfs3_t *lfs3, lfs3_file_t *file,
        const lfs3_rattr_t *rname);
#endif
static int lfs3_file_ck(lfs3_t *lfs3, lfs3_file_t *file, uint32_t flags);

static int lfs3_file_opencfg_(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs3_file_cfg *cfg) {
    #ifndef LFS3_RDONLY
    if (!lfs3_o_isrdonly(flags)) {
        // prepare our filesystem for writing
        int err = lfs3_fs_mkconsistent(lfs3);
        if (err) {
            return err;
        }
    }
    #endif

    // setup file state
    lfs3_file_init(file,
            // mounted with LFS3_M_FLUSH/SYNC? implies LFS3_O_FLUSH/SYNC
            flags | (lfs3->flags & (LFS3_M_FLUSH | LFS3_M_SYNC)),
            cfg);

    // allocate cache if necessary
    //
    // though note lfs3_set passes data via the file cache, so make sure
    // not to clobber it if LFS3_o_SET is set
    if (lfs3_o_isset(file->h.flags)) {
        file->h.flags |= LFS3_o_NEEDSFLUSH;
        file->cache.buffer = file->cfg->fcache_buffer;
        file->cache.pos = 0;
        file->cache.size = file->cfg->fcache_size;
    } else if (file->cfg->fcache_buffer) {
        file->cache.buffer = file->cfg->fcache_buffer;
    } else {
        file->cache.buffer = lfs3_malloc(lfs3_file_fcachesize(lfs3, file));
        if (!file->cache.buffer) {
            return LFS3_ERR_NOMEM;
        }
    }

    int err;
    // lookup our parent
    lfs3_did_t did;
    lfs3_stag_t tag = lfs3_mtree_pathlookup(lfs3, &path,
            &file->h.mdir, &did);
    if (tag < 0
            && !(tag == LFS3_ERR_NOENT
                && lfs3_path_islast(path))) {
        err = tag;
        goto failed;
    }

    // creating a new entry?
    if (LFS3_IFDEF_RDONLY(
            false,
            (tag == LFS3_ERR_NOENT || tag == LFS3_tag_ORPHAN)
                && lfs3_o_iscreat(file->h.flags))) {
        #ifndef LFS3_RDONLY
        // we'd better not be rdonly
        LFS3_ASSERT(!lfs3_o_isrdonly(file->h.flags));

        // we're a file, don't allow trailing slashes
        if (lfs3_path_isdir(path)) {
            err = LFS3_ERR_NOTDIR;
            goto failed;
        }

        // check that name fits
        if (lfs3_path_namelen(path) > lfs3->name_limit) {
            err = LFS3_ERR_NAMETOOLONG;
            goto failed;
        }

        // if stickynote, mark as uncreated + unsync
        if (tag != LFS3_ERR_NOENT) {
            file->h.flags |= LFS3_o_NEEDSCREAT | LFS3_o_NEEDSSYNC;
        }
        #endif

    // existing entry?
    } else if (!(tag == LFS3_ERR_NOENT || tag == LFS3_tag_ORPHAN)) {
        #ifndef LFS3_RDONLY
        // wanted to create a new entry?
        if (lfs3_o_isexcl(file->h.flags)) {
            err = LFS3_ERR_EXIST;
            goto failed;
        }
        #endif

        // wrong type?
        if (tag == LFS3_TAG_DIR) {
            err = LFS3_ERR_ISDIR;
            goto failed;
        }
        if (tag == LFS3_tag_UNKNOWN) {
            err = LFS3_ERR_NOTSUP;
            goto failed;
        }

        #ifndef LFS3_RDONLY
        // if stickynote, mark as uncreated + unsync
        if (tag == LFS3_TAG_STICKYNOTE) {
            file->h.flags |= LFS3_o_NEEDSCREAT | LFS3_o_NEEDSSYNC;
        }

        // if truncating, mark as unsync
        if (lfs3_o_istrunc(file->h.flags)) {
            file->h.flags |= LFS3_o_NEEDSSYNC;
        }
        #endif

    // no?
    } else {
        err = LFS3_ERR_NOENT;
        goto failed;
    }

    // need to create an entry?
    #ifndef LFS3_RDONLY
    if (tag == LFS3_ERR_NOENT) {
        // checkpoint the allocator
        err = lfs3_alloc_ckpoint(lfs3);
        if (err) {
            goto failed;
        }

        // small file set? can we atomically commit everything in one
        // commit? currently this is only possible via lfs3_set
        if (lfs3_o_isset(file->h.flags)
                && file->cache.size <= lfs3->cfg->shrub_size
                && file->cache.size <= lfs3->cfg->fragment_size
                && file->cache.size < lfs3_max(lfs3->cfg->crystal_thresh, 1)) {
            // we need to mark as unsync for sync to do anything
            file->h.flags |= LFS3_o_NEEDSSYNC;

            err = lfs3_file_sync_(lfs3, file, (const lfs3_rattr_t[]){
                    LFS3_RATTR(LFS3_TAG_REG, +1, 2, LFS3_FROM_NAME),
                    LFS3_RATTR_ARG(did),
                    LFS3_RATTR_ARG(path),
                    LFS3_RATTR_NULL});
            if (err) {
                goto failed;
            }

        } else {
            // create a stickynote entry if we don't have one, this
            // reserves the mid until first sync
            err = lfs3_mdir_commit(lfs3, &file->h.mdir,
                    (const lfs3_rattr_t[]){
                        LFS3_RATTR(LFS3_TAG_STICKYNOTE, +1, 2, LFS3_FROM_NAME),
                        LFS3_RATTR_ARG(did),
                        LFS3_RATTR_ARG(path),
                        LFS3_RATTR_NULL});
            if (err) {
                goto failed;
            }

            // mark as uncreated + unsync
            file->h.flags |= LFS3_o_NEEDSCREAT | LFS3_o_NEEDSSYNC;
        }

        // update dir positions
        for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
            if (lfs3_o_type(h->flags) == LFS3_TYPE_DIR
                    && ((lfs3_dir_t*)h)->did == did
                    && h->mdir.mid >= file->h.mdir.mid) {
                ((lfs3_dir_t*)h)->pos += 1;
            }
        }
    }
    #endif

    // fetch the file struct and custom attrs
    err = lfs3_file_fetch(lfs3, file, file->h.flags);
    if (err) {
        goto failed;
    }

    // add to tracked mdirs
    lfs3_handle_open(lfs3, &file->h);

    // check metadata/data for errors?
    if (file->h.flags & (
            LFS3_O_CKMETA
                | LFS3_O_CKDATA)) {
        err = lfs3_file_ck(lfs3, file,
                file->h.flags & (
                    LFS3_O_CKMETA
                        | LFS3_O_CKDATA));
        if (err) {
            goto failed;
        }
    }

    return 0;

failed:;
    // clean up resources
    lfs3_file_close_(lfs3, file);
    return err;
}

int lfs3_file_opencfg(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs3_file_cfg *cfg) {
    // already open?
    LFS3_ASSERT(!lfs3_handle_isopen(lfs3, &file->h));
    // don't allow the forbidden mode!
    LFS3_ASSERT(lfs3_o_mode(flags) != 3);
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_O_RDONLY
                | LFS3_IFDEF_RDONLY(0, LFS3_O_WRONLY)
                | LFS3_IFDEF_RDONLY(0, LFS3_O_RDWR)
                | LFS3_IFDEF_RDONLY(0, LFS3_O_CREAT)
                | LFS3_IFDEF_RDONLY(0, LFS3_O_EXCL)
                | LFS3_IFDEF_RDONLY(0, LFS3_O_TRUNC)
                | LFS3_IFDEF_RDONLY(0, LFS3_O_APPEND)
                | LFS3_O_FLUSH
                | LFS3_O_SYNC
                | LFS3_O_DESYNC
                | LFS3_O_CKMETA
                | LFS3_O_CKDATA)) == 0);
    // writeable files require a writeable filesystem
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags) || lfs3_o_isrdonly(flags));
    // these flags require a readable file
    LFS3_ASSERT(!lfs3_o_iswronly(flags) || !lfs3_t_isckmeta(flags));
    LFS3_ASSERT(!lfs3_o_iswronly(flags) || !lfs3_t_isckdata(flags));
    // these flags require a writable file
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(!lfs3_o_isrdonly(flags) || !lfs3_o_iscreat(flags));
    LFS3_ASSERT(!lfs3_o_isrdonly(flags) || !lfs3_o_isexcl(flags));
    LFS3_ASSERT(!lfs3_o_isrdonly(flags) || !lfs3_o_istrunc(flags));
    for (lfs3_size_t i = 0; i < cfg->attr_count; i++) {
        // these flags require a writable attr
        LFS3_ASSERT(!lfs3_o_isrdonly(cfg->attrs[i].flags)
                || !lfs3_o_iscreat(cfg->attrs[i].flags));
        LFS3_ASSERT(!lfs3_o_isrdonly(cfg->attrs[i].flags)
                || !lfs3_o_isexcl(cfg->attrs[i].flags));
    }
    #endif

    return lfs3_file_opencfg_(lfs3, file, path, flags,
            cfg);
}


// default file config
static const struct lfs3_file_cfg lfs3_file_defaultcfg = {0};

int lfs3_file_open(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags) {
    return lfs3_file_opencfg(lfs3, file, path, flags,
            &lfs3_file_defaultcfg);
}

// clean up resources
static void lfs3_file_close_(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    // remove from tracked mdirs
    lfs3_handle_close(lfs3, &file->h);

    // clean up memory
    if (!file->cfg->fcache_buffer) {
        lfs3_free(file->cache.buffer);
    }

    // are we orphaning a file?
    //
    // make sure we check _after_ removing ourselves
    #ifndef LFS3_RDONLY
    if (lfs3_o_needscreat(file->h.flags)
            && !lfs3_mid_isopen(lfs3, file->h.mdir.mid, -1)) {
        // this can only happen in a rdwr filesystem
        LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

        // this gets a bit messy, since we're not able to write to the
        // filesystem if we're rdonly or desynced, fortunately we have
        // a few tricks

        // first try to push onto our grm queue
        if (lfs3_grm_count(&lfs3->grm) < 2) {
            lfs3_grm_push(&lfs3->grm, file->h.mdir.mid);

        // fallback to just marking the filesystem as inconsistent
        } else {
            lfs3->flags |= LFS3_I_MKCONSISTENT;
        }
    }
    #endif
}

// needed in lfs3_file_close
int lfs3_file_sync(lfs3_t *lfs3, lfs3_file_t *file);

int lfs3_file_close(lfs3_t *lfs3, lfs3_file_t *file) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    // don't call lfs3_file_sync if we're readonly or desynced
    int err = 0;
    if (!lfs3_o_isrdonly(file->h.flags)
            && !lfs3_o_isdesync(file->h.flags)) {
        err = lfs3_file_sync(lfs3, file);
    }

    // clean up resources
    lfs3_file_close_(lfs3, file);

    return err;
}

// low-level file reading

static int lfs3_file_lookupnext_(lfs3_t *lfs3, const lfs3_file_t *file,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_rbyd_t *rbyd_, lfs3_srid_t *rid_,
        lfs3_bptr_t *bptr_) {
    lfs3_bid_t weight;
    lfs3_data_t data;
    lfs3_stag_t tag = lfs3_bshrub_lookupnext_(lfs3, &file->bshrub, bid,
            bid_, rbyd_, rid_, &weight, &data);
    if (tag < 0) {
        return tag;
    }

    // hole? (no data)
    if (tag == LFS3_TAG_HOLE) {
        bptr_->d = LFS3_DATA_HOLE(weight);

    // fragment? (inlined data)
    } else if (tag == LFS3_TAG_DATA) {
        bptr_->d = data;

    // bptr?
    } else if (tag == LFS3_TAG_BLOCK) {
        int err = lfs3_data_fetchbptr(lfs3, &data,
                bptr_);
        if (err) {
            return err;
        }

    } else {
        LFS3_UNREACHABLE();
    }

    // weight/size mismatch?
    LFS3_ASSERT(bptr_->d.weight == weight);
    return 0;
}

static int lfs3_file_lookupnext(lfs3_t *lfs3, const lfs3_file_t *file,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_bptr_t *bptr_) {
    lfs3_rbyd_t rbyd__;
    return lfs3_file_lookupnext_(lfs3, file, bid,
            bid_, &rbyd__, NULL, bptr_);
}

static int lfs3_file_read_(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t buffer_pos, const uint8_t *buffer, lfs3_size_t buffer_size,
        lfs3_off_t pos,
        lfs3_data_t *data_) {
    // out-of-bounds?
    //
    // note we can't use lfs3_file_size_ here, because buffer may not
    // yet be flushed
    lfs3_off_t file_size = lfs3_max(
            buffer_pos + buffer_size,
            lfs3_max(
                file->leaf.pos + file->leaf.bptr.d.weight,
                file->bshrub.weight));
    if (pos >= file_size) {
        return LFS3_ERR_NOENT;
    }

    // keep track of the next highest priority data offset
    lfs3_off_t d = file_size;

    // any data in our buffer?
    if (pos < buffer_pos + buffer_size && buffer_size > 0) {
        if (pos >= buffer_pos) {
            d = lfs3_min(d, (buffer_pos + buffer_size) - pos);
            *data_ = LFS3_DATA_BUF(&buffer[pos - buffer_pos], d);
            return 0;
        }

        // buffered data takes priority
        d = lfs3_min(d, buffer_pos - pos);
    }

    // any data in our leaf?
    if (pos < file->leaf.pos + file->leaf.bptr.d.weight) {
        if (pos >= file->leaf.pos) {
            d = lfs3_min(
                    d,
                    (file->leaf.pos + file->leaf.bptr.d.weight) - pos);
            // note one important side-effect here is a strict data hint
            *data_ = lfs3_data_fromslice(&file->leaf.bptr.d,
                    pos - file->leaf.pos,
                    d);
            return 0;
        }

        // leaf takes priority
        d = lfs3_min(d, file->leaf.pos - pos);
    }

    // any data in our btree?
    if (pos < file->bshrub.weight) {
        lfs3_bid_t bid__;
        lfs3_bptr_t bptr__;
        int err = lfs3_file_lookupnext(lfs3, file, pos,
                &bid__, &bptr__);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // as an optimization, we track the most recently read leaf to
        // avoid repeated lookups
        //
        // but only if we're not crystallizing!
        if (!lfs3_o_needscryst(file->h.flags)
                && !lfs3_o_needsgraft(file->h.flags)) {
            file->leaf.pos = bid__-(bptr__.d.weight-1);
            file->leaf.bptr = bptr__;
        }

        d = lfs3_min(d, bid__+1 - pos);
        // note one important side-effect here is a strict data hint
        *data_ = lfs3_data_fromslice(&bptr__.d,
                pos - (bid__-(bptr__.d.weight-1)),
                d);
        return 0;
    }

    // found a hole?
    *data_ = LFS3_DATA_HOLE(d);
    return 0;
}

// high-level file reading

lfs3_ssize_t lfs3_file_read(lfs3_t *lfs3, lfs3_file_t *file,
        void *buffer, lfs3_size_t size) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    // can't read from writeonly files
    LFS3_ASSERT(!lfs3_o_iswronly(file->h.flags));
    LFS3_ASSERT(file->pos + size <= 0x7fffffff);

    lfs3_off_t pos_ = file->pos;
    uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;
    while (size_ > 0 && pos_ < lfs3_file_size_(file)) {
        // any data in our cache?
        if (pos_ >= file->cache.pos
                && pos_ < file->cache.pos + file->cache.size) {
            lfs3_size_t d = lfs3_min(
                    size_,
                    (file->cache.pos + file->cache.size) - pos_);
            lfs3_memcpy(buffer_,
                    &file->cache.buffer[pos_ - file->cache.pos],
                    d);

            pos_ += d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // any data on-disk?
        //
        // we move our leaf/cache around to try to optimize future reads
        // here, so we need to make sure any pending writes are settled
        //
        // we could be a bit tighter with our logic here (don't flush if
        // we bypass cache, don't need to move leaf, etc), but that
        // would make this logic significantly more complicated
        //
        // if you want efficient rw, just open two handles
        //
        if (!lfs3_o_needsflush(file->h.flags)
                // flush also takes care of these, and as a plus we
                // don't need to worry about out-of-date leaves in the
                // btree
                && !lfs3_o_needscryst(file->h.flags)
                && !lfs3_o_needsgraft(file->h.flags)) {
            lfs3_data_t data__;
            int err = lfs3_file_read_(lfs3, file,
                    file->cache.pos, file->cache.buffer, file->cache.size,
                    pos_,
                    &data__);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // any data on-disk?
            if (!lfs3_data_ishole(&data__)) {
                // bypass cache?
                if (size_ >= lfs3_file_fcachesize(lfs3, file)) {
                    lfs3_ssize_t d = lfs3_data_read(lfs3, &data__,
                            buffer_, size_);
                    if (d < 0) {
                        return d;
                    }

                    pos_ += d;
                    buffer_ += d;
                    size_ -= d;
                    continue;
                }

                // try to fill our cache with some data
                lfs3_ssize_t d = lfs3_data_read(lfs3, &data__,
                        file->cache.buffer, lfs3_file_fcachesize(lfs3, file));
                if (d < 0) {
                    return d;
                }

                file->cache.pos = pos_;
                file->cache.size = d;
                continue;
            }

            // found a hole? fill with zeros
            lfs3_ssize_t d = lfs3_min(data__.weight, size_);
            lfs3_memset(buffer_, 0, d);

            pos_ += d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // flush our cache so the above can't fail
        //
        // note that flush does not change the actual file data, so if
        // a read fails it's ok to fall back to our flushed state
        //
        int err = lfs3_file_flush(lfs3, file);
        if (err) {
            return err;
        }
        lfs3_file_discardcache(file);
    }

    // update file and return amount read
    lfs3_size_t read = pos_ - file->pos;
    file->pos = pos_;
    return read;
}

// low-level file writing

// graft bptr/fragments into our bshrub/btree
#ifndef LFS3_RDONLY
static int lfs3_file_graft__(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t pos, lfs3_off_t cut, const lfs3_bptr_t *bptr) {
    // note! we must never allow our btree size to overflow, even
    // temporarily

    // cutting the entire tree? revert to no bshrub/btree
    if (pos == 0
            && file->bshrub.weight
                    - lfs3_min(cut, file->bshrub.weight)
                    + bptr->d.weight
                == 0) {
        lfs3_file_discardbshrub(file);
        return 0;
    }

    // we may need to make multiple commits if weight spans multiple
    // leaf blocks
    lfs3_off_t pos_ = pos;
    lfs3_off_t cut_ = lfs3_min(
            cut,
            // limit cut to size of tree
            file->bshrub.weight - lfs3_min(pos_, file->bshrub.weight));
    lfs3_bptr_t bptr_ = *bptr;

    // keep track of if we are aligned
    bool aligned_ = false;

    while (pos_ > file->bshrub.weight
            || cut_ > 0
            || bptr_.d.weight > 0) {
        // but try to use as few commits where possible
        lfs3_bid_t l_bid = lfs3_min(pos_, file->bshrub.weight);
        lfs3_rbyd_t l_rbyd = file->bshrub;
        lfs3_srid_t l_rid = lfs3_min(pos_, file->bshrub.weight);
        lfs3_off_t l_cut = 0;
        lfs3_bptr_t l_bptr;
        l_bptr.d = LFS3_DATA_NULL();
        lfs3_bptr_t r_bptr;
        r_bptr.d = LFS3_DATA_NULL();

        // how much we'll be able to cut/grow this commit
        lfs3_off_t dcut = 0;
        lfs3_off_t dgrow = bptr_.d.weight;

        // keep track of how each cut affects our shrub estimate
        lfs3_ssize_t shestimate = 0;

        // needs a new hole?
        if (pos_ > file->bshrub.weight) {
            lfs3_off_t l_hole = pos_ - file->bshrub.weight;
            // can we merge a new hole?
            if (lfs3_bptr_ishole(&bptr_)) {
                pos_ -= l_hole;
                bptr_.d.weight += l_hole;
                dgrow += l_hole;

            // needs a new hole?
            } else {
                l_bptr.d = LFS3_DATA_HOLE(l_hole);
            }
        }

        lfs3_off_t poke = lfs3_smax(
                lfs3_smin(
                    (aligned_) ? pos_ : pos_-1,
                    file->bshrub.weight-1),
                0);
        while (poke < lfs3_min(pos_+cut_+1, file->bshrub.weight)) {
            lfs3_bid_t bid__;
            lfs3_srid_t rid__;
            lfs3_bptr_t bptr__;
            int err = lfs3_file_lookupnext_(lfs3, file,
                    poke,
                    &bid__, &l_rbyd, &rid__, &bptr__);
            if (err) {
                return err;
            }

            // adjust l_rid
            //
            // note this shift will always be valid unless we change
            // rbyds, which we explicitly don't because then everything
            // else would break
            LFS3_ASSERT(l_bid >= bid__ - rid__);
            l_rid = l_bid - (bid__ - rid__);

            // we need to cut anything that's overlapping
            bool snip = bid__-(bptr__.d.weight-1) < pos_+cut_
                    && bid__+1 > pos_;

            // found left sibling?
            if (bid__-(bptr__.d.weight-1) < pos_) {
                lfs3_off_t l_slice
                        = pos_ - (bid__-(bptr__.d.weight-1));
                // can we merge a new hole left?
                if (bid__+1 < pos_
                        && lfs3_bptr_ishole(&bptr__)) {
                    l_bptr.d = LFS3_DATA_HOLE(l_slice);
                    snip = true;
                }

                // can we merge a hole? may include new hole
                if (lfs3_bptr_ishole(&bptr_)
                        && lfs3_bptr_ishole(&bptr__)) {
                    pos_ -= l_slice;
                    bptr_.d.weight += l_slice;
                    cut_ += l_slice;
                    dgrow += l_slice;
                    snip = true;

                // can we merge a fragment?
                } else if (lfs3_bptr_isfragment(&bptr_)
                        && lfs3_bptr_isfragment(&bptr__)
                        // not if there's a hole!
                        && bid__+1 >= pos_
                        // or if we're already a full fragment
                        && l_slice < lfs3->cfg->fragment_size) {
                    pos_ -= l_slice;
                    l_bptr.d = lfs3_data_fromslice(&bptr__.d,
                            -1,
                            l_slice);
                    l_bptr.d.weight = -l_bptr.d.weight;
                    cut_ += l_slice;
                    dgrow += l_slice;
                    snip = true;

                // need to slice?
                } else if (bid__+1 > pos_) {
                    lfs3_bptr_fromslice(&l_bptr, &bptr__,
                            -1,
                            l_slice);
                    snip = true;
                }
            }

            // found right sibling?
            if (bid__+1 > pos_+cut_) {
                lfs3_off_t r_slice = bid__+1 - (pos_+cut_);
                // can we merge a hole?
                if (lfs3_bptr_ishole(&bptr_)
                        && lfs3_bptr_ishole(&bptr__)) {
                    bptr_.d.weight += r_slice;
                    dgrow += r_slice;
                    snip = true;

                // can we merge a fragment?
                } else if (lfs3_bptr_isfragment(&bptr_)
                        && lfs3_bptr_isfragment(&bptr__)
                        // unlike left sibling, we don't bother merging if
                        // things won't fit in a single fragment
                        && dgrow + (bid__+1 - (pos_+cut_))
                            <= lfs3->cfg->fragment_size) {
                    r_bptr.d = lfs3_data_fromslice(&bptr__.d,
                            lfs3_data_size(&bptr__.d) - r_slice,
                            -1);
                    r_bptr.d.weight = -r_bptr.d.weight;
                    dgrow += r_slice;
                    snip = true;

                // need to slice?
                } else if (bid__-(bptr__.d.weight-1) < pos_+cut_) {
                    lfs3_bptr_fromslice(&r_bptr, &bptr__,
                            bptr__.d.weight - r_slice,
                            -1);
                    snip = true;
                }
            }

            // overlapping? merging? need to cut
            if (snip) {
                l_bid = bid__;
                l_rid = rid__;
                l_cut += bptr__.d.weight;
                dcut += lfs3_min(
                        lfs3_min(
                            bptr__.d.weight,
                            bid__+1 - pos_),
                        cut_ - dcut);
                shestimate -= lfs3->rattr_estimate
                        + lfs3_bptr_estimate(&bptr__);
            }

            // stop here if we've reached the end of a leaf rbyd, we
            // can't commit to multiple leaves simultaneously, so this
            // is the best we can do
            //
            // As a consequence, we will never merge fragments across
            // leaf rbyds, but this is actually a good thing! Otherwise
            // we'd have to worry about the underlying blocks being
            // reallocated before the graft finishes (consider rbyds
            // with single fragments). I don't think it's possible to
            // merge cross-rbyd fragments atomically.
            //
            // The staging shrub doesn't help here as we need it to
            // restart commits during mdir compactions, etc. If we
            // wanted to track everything for cross-rbyd fragment
            // merging, I think we'd need either 3 shrubs or some
            // other hack.
            //
            // Note this is not a problem for bptrs because we
            // explicitly track crystallizing blocks in file->leaf.
            //
            if (rid__+1 == (lfs3_srid_t)l_rbyd.weight
                    && bid__+1 < lfs3_min(pos_+cut_+1, file->bshrub.weight)
                    && snip) {
                // if we stop early, limit how much we grow to how much
                // we cut to avoid overflow issues
                if (!lfs3_bptr_isbptr(&bptr_)) {
                    dgrow = lfs3_min(dgrow, dcut);
                // if we're a bptr, just don't graft anything until
                // last commit
                } else {
                    dgrow = 0;
                }
                break;
            }

            // increment poke
            poke = bid__ + 1;
        }

        // limit fragment data to:
        // 1. fragment size
        // 2. cut size, to avoid overflow issues
        if (lfs3_bptr_isfragment(&bptr_)) {
            dgrow = lfs3_min(dgrow, lfs3->cfg->fragment_size);
        }

        // build graft commit
        lfs3_rattr_t rattrs[14];
        lfs3_rattr_t *r = rattrs;

        // need to cut tree?
        if (l_cut) {
            *r++ = LFS3_RATTR(LFS3_tag_RM, -2, 0);
            *r++ = LFS3_RATTR_WEIGHT(-l_cut);
        }

        // left sibling?
        if ((lfs3_soff_t)l_bptr.d.weight > 0) {
            // left hole?
            if (lfs3_bptr_ishole(&l_bptr)) {
                *r++ = LFS3_RATTR(LFS3_TAG_HOLE, -2, 0);
                *r++ = LFS3_RATTR_WEIGHT(+l_bptr.d.weight);

            // left fragment?
            } else if (lfs3_bptr_isfragment(&l_bptr)) {
                *r++ = LFS3_RATTR(LFS3_TAG_DATA, -2, 1, LFS3_FROM_DATA);
                *r++ = LFS3_RATTR_WEIGHT(+l_bptr.d.weight);
                *r++ = LFS3_RATTR_ARG(&l_bptr);

            // left bptr?
            } else {
                *r++ = LFS3_RATTR(LFS3_TAG_BLOCK, -2, 1, LFS3_FROM_BPTR);
                *r++ = LFS3_RATTR_WEIGHT(+l_bptr.d.weight);
                *r++ = LFS3_RATTR_ARG(&l_bptr);
            }
            shestimate += lfs3->rattr_estimate + lfs3_bptr_estimate(&l_bptr);
        }

        // graft?
        if (dgrow) {
            // graft hole?
            if (lfs3_bptr_ishole(&bptr_)) {
                *r++ = LFS3_RATTR(LFS3_TAG_HOLE, -2, 0);
                *r++ = LFS3_RATTR_WEIGHT(+dgrow);
                shestimate += lfs3->rattr_estimate;

            // graft fragment?
            } else if (lfs3_bptr_isfragment(&bptr_)) {
                lfs3_size_t count
                        = ((lfs3_soff_t)l_bptr.d.weight < 0)
                        + 1
                        + ((lfs3_soff_t)r_bptr.d.weight < 0);
                *r++ = LFS3_RATTR(LFS3_TAG_DATA, -2, 3*count,
                        LFS3_FROM_GRAFT, count);
                *r++ = LFS3_RATTR_WEIGHT(+dgrow);

                // merge and slice data
                //
                // note we don't need to worry about: (1) left data,
                // because we only merge left if left < fragment size,
                // and (2) right data, because we only merge right if
                // everything would fit
                lfs3_data_t *d = (lfs3_data_t*)r;
                if ((lfs3_soff_t)l_bptr.d.weight < 0) {
                    *d = l_bptr.d;
                    d->weight = -d->weight;
                    d++;
                }
                *d++ = lfs3_data_fromslice(&bptr_.d,
                        -1,
                        dgrow - lfs3_smax(-(lfs3_soff_t)l_bptr.d.weight, 0));
                if ((lfs3_soff_t)r_bptr.d.weight < 0) {
                    *d = r_bptr.d;
                    d->weight = -d->weight;
                    d++;
                }

                LFS3_ASSERT(
                        ((count > 0)
                                ? lfs3_data_size(&((lfs3_data_t*)r)[0])
                                : 0)
                            + ((count > 1)
                                ? lfs3_data_size(&((lfs3_data_t*)r)[1])
                                : 0)
                            + ((count > 2)
                                ? lfs3_data_size(&((lfs3_data_t*)r)[2])
                                : 0)
                        == dgrow);
                LFS3_ASSERT(dgrow <= lfs3->cfg->fragment_size);
                r += 3*count;
                shestimate += lfs3->rattr_estimate + dgrow;

            // graft bptr?
            } else {
                *r++ = LFS3_RATTR(LFS3_TAG_BLOCK, -2, 1, LFS3_FROM_BPTR);
                *r++ = LFS3_RATTR_WEIGHT(+dgrow);
                *r++ = LFS3_RATTR_ARG(&bptr_);
                shestimate += lfs3->rattr_estimate + LFS3_BPTR_DSIZE;
            }
        }

        // right sibling?
        if ((lfs3_soff_t)r_bptr.d.weight > 0) {
            // right hole?
            if (lfs3_bptr_ishole(&r_bptr)) {
                *r++ = LFS3_RATTR(LFS3_TAG_HOLE, -2, 0);
                *r++ = LFS3_RATTR_WEIGHT(+r_bptr.d.weight);

            // right fragment?
            } else if (lfs3_bptr_isfragment(&r_bptr)) {
                *r++ = LFS3_RATTR(LFS3_TAG_DATA, -2, 1, LFS3_FROM_DATA);
                *r++ = LFS3_RATTR_WEIGHT(+r_bptr.d.weight);
                *r++ = LFS3_RATTR_ARG(&r_bptr);

            // right bptr?
            } else {
                *r++ = LFS3_RATTR(LFS3_TAG_BLOCK, -2, 1, LFS3_FROM_BPTR);
                *r++ = LFS3_RATTR_WEIGHT(+r_bptr.d.weight);
                *r++ = LFS3_RATTR_ARG(&r_bptr);
            }
            shestimate += lfs3->rattr_estimate + lfs3_bptr_estimate(&r_bptr);
        }

        // commit pending rattrs
        if (r > rattrs) {
            *r++ = LFS3_RATTR_NULL;
            LFS3_ASSERT((lfs3_size_t)(r-rattrs)
                    <= sizeof(rattrs)/sizeof(lfs3_rattr_t));

            int err = lfs3_bshrub_commit_(lfs3, &file->bshrub,
                    l_bid, &l_rbyd, l_rid, rattrs, shestimate);
            if (err) {
                return err;
            }
        }

        // update graft state
        pos_ += dgrow;
        cut_ -= dcut;
        lfs3_bptr_slice(&bptr_,
                dgrow - lfs3_smax(-(lfs3_soff_t)l_bptr.d.weight, 0),
                -1);

        // we should be aligned now
        aligned_ = true;
    }

    return 0;
}
#endif

// graft any ungrafted leaves
#ifndef LFS3_RDONLY
static int lfs3_file_graft_(lfs3_t *lfs3, lfs3_file_t *file) {
    // do nothing if our file is already grafted
    if (!lfs3_o_needsgraft(file->h.flags)) {
        return 0;
    }
    // ungrafted files must be unsynced
    LFS3_ASSERT(lfs3_o_needssync(file->h.flags));

    // graft into the tree
    int err = lfs3_file_graft__(lfs3, file,
            file->leaf.pos, file->leaf.bptr.d.weight,
            &file->leaf.bptr);
    if (err) {
        return err;
    }

    // mark as grafted
    file->h.flags &= ~LFS3_o_NEEDSGRAFT;
    return 0;
}
#endif

// note the slightly unique behavior when crystal_min=-1:
// - crystal_min=-1 => crystal_min=crystal_max
// - crystal_max=-1 => crystal_max=unbounded
//
// this helps avoid duplicate arguments with tight crystal bounds, if
// you really want to crystallize as little as possible, use
// crystal_min=0
//
#ifndef LFS3_RDONLY
// this LFS3_NOINLINE is to force lfs3_file_crystallize__ off the stack
// hot-path
LFS3_NOINLINE
static int lfs3_file_crystallize__(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t buffer_pos, const uint8_t *buffer, lfs3_size_t buffer_size,
        lfs3_off_t block_pos,
        lfs3_ssize_t crystal_min, lfs3_ssize_t crystal_max) {
    // align to prog_size, limit to block_size and theoretical file size
    lfs3_off_t crystal_limit = lfs3_min(
            block_pos + lfs3_min(
                lfs3_aligndown(
                    (lfs3_off_t)crystal_max,
                    lfs3_min(
                        lfs3->cfg->prog_size,
                        lfs3_max(lfs3->cfg->crystal_thresh, 1))),
                lfs3->cfg->block_size),
            lfs3_max(
                buffer_pos + buffer_size,
                file->bshrub.weight));

    // resuming crystallization? or do we need to allocate a new block?
    if (!lfs3_o_needscryst(file->h.flags)) {
        goto relocate;
    }

    // only blocks can be uncrystallized
    LFS3_ASSERT(lfs3_bptr_isbptr(&file->leaf.bptr));
    LFS3_ASSERT(lfs3_bptr_iserased(&file->leaf.bptr));

    // uncrystallized blocks shouldn't be truncated or anything
    LFS3_ASSERT(file->leaf.pos - lfs3_bptr_off(&file->leaf.bptr)
            == block_pos);
    LFS3_ASSERT(lfs3_bptr_off(&file->leaf.bptr)
                + lfs3_bptr_size(&file->leaf.bptr)
            == lfs3_bptr_cksize(&file->leaf.bptr));

    // before we write, claim the erased state!
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h != &file->h
                && lfs3_bptr_block(&((lfs3_file_t*)h)->leaf.bptr)
                    == lfs3_bptr_block(&file->leaf.bptr)) {
            lfs3_bptr_claim(&((lfs3_file_t*)h)->leaf.bptr);
        }
    }

    // copy things in case we hit an error
    lfs3_sblock_t block_ = lfs3_bptr_block(&file->leaf.bptr);
    lfs3_size_t off_ = lfs3_bptr_off(&file->leaf.bptr);
    lfs3_off_t pos_ = block_pos
            + lfs3_bptr_off(&file->leaf.bptr)
            + lfs3_bptr_size(&file->leaf.bptr);
    uint32_t cksum_ = lfs3_bptr_cksum(&file->leaf.bptr);
    while (true) {
        // crystallize data into our block
        //
        // i.e. eagerly merge any right neighbors unless that would put
        // us over our crystal_size/block_size
        while (pos_ < crystal_limit) {
            lfs3_data_t data__;
            int err = lfs3_file_read_(lfs3, file,
                    buffer_pos, buffer, buffer_size,
                    pos_,
                    &data__);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // is this data a pure hole? stop early to (FUTURE)
            // better leverage erased-state in sparse files, and to
            // try to avoid writing a bunch of unnecessary zeros
            if ((lfs3_data_ishole(&data__)
                        // does this data exceed our block_size? also
                        // stop early to try to avoid messing up
                        // block alignment
                        || pos_+data__.weight - block_pos
                            > lfs3->cfg->block_size)
                    // but make sure to include all of the requested
                    // crystal if explicit, otherwise above loops
                    // may never terminate
                    && (lfs3_soff_t)(pos_ - block_pos)
                        >= (lfs3_soff_t)lfs3_min(
                            crystal_min,
                            crystal_max)) {
                // if we hit this condition, mark as crystallized,
                // attempting resume crystallization will not make
                // progress
                file->h.flags &= ~LFS3_o_NEEDSCRYST;
                break;
            }

            // slice the data we care about
            lfs3_data_slice(&data__, -1, crystal_limit - pos_);

            // any data on-disk?
            if (!lfs3_data_ishole(&data__)) {
                int err = lfs3_bd_progdata(lfs3, block_, pos_ - block_pos,
                        &data__, LFS3_BD_ALIGN,
                        &cksum_);
                if (err) {
                    LFS3_ASSERT(err != LFS3_ERR_RANGE);
                    // bad prog? try another block
                    if (err == LFS3_ERR_CORRUPT) {
                        goto relocate;
                    }
                    return err;
                }

                pos_ += data__.weight;
                continue;
            }

            // found a hole? fill with zeros
            err = lfs3_bd_set(lfs3, block_, pos_ - block_pos,
                    0, data__.weight, LFS3_BD_ALIGN,
                    &cksum_);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS3_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            pos_ += data__.weight;
        }

        // if we're fully crystallized, mark as crystallized
        //
        // note some special conditions may also clear this flag in the
        // above loop
        //
        // and don't worry, we can still resume crystallization if we
        // write to the tracked erased state
        if (pos_ - block_pos == lfs3->cfg->block_size
                || pos_ == lfs3_max(
                    buffer_pos + buffer_size,
                    file->bshrub.weight)) {
            file->h.flags &= ~LFS3_o_NEEDSCRYST;
        }

        // a bit of a hack here, we need to truncate our block to
        // prog_size alignment to avoid padding issues
        //
        // doing this retroactively to the pcache greatly simplifies the
        // above loop, though we may end up reading more than is
        // strictly necessary
        //
        // note we _don't_ do this if prog alignment violates
        // crystal_thresh, as this would prevent crystallization when
        // crystal_thresh < prog_size, it's a weird case, but this is
        // useful for small blocks
        lfs3_size_t d = (pos_ - block_pos) % lfs3->cfg->prog_size;
        if (d < lfs3_max(lfs3->cfg->crystal_thresh, 1)) {
            lfs3->pcache.size -= d;
            pos_ -= d;
        }

        // finalize our write
        int err = lfs3_bd_flush(lfs3, LFS3_BD_ALIGN,
                &cksum_);
        if (err) {
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // and update the leaf bptr
        LFS3_ASSERT(pos_ - block_pos >= off_);
        LFS3_ASSERT(pos_ - block_pos <= lfs3->cfg->block_size);
        file->leaf.pos = block_pos + off_;
        file->leaf.bptr.d.weight = pos_ - file->leaf.pos;
        file->leaf.bptr.d.u.disk.block = block_;
        file->leaf.bptr.d.off = LFS3_BPTR_ONDISK | LFS3_BPTR_ISBPTR | off_;
        LFS3_IFDEF_CKDATACKSUMS(
                file->leaf.bptr.d.u.disk.cksize,
                file->leaf.bptr.cksize) = (
                    // mark as erased, unless crystal_thresh prevented
                    // prog alignment
                    ((pos_ - block_pos) % lfs3->cfg->prog_size == 0)
                            ? LFS3_BPTR_ISERASED
                            : 0)
                        | (pos_ - block_pos);
        LFS3_IFDEF_CKDATACKSUMS(
                file->leaf.bptr.d.u.disk.cksum,
                file->leaf.bptr.cksum) = cksum_;

        // mark as ungrafted
        file->h.flags |= LFS3_o_NEEDSGRAFT;
        return 0;

    relocate:;
        // allocate a new block
        //
        // if we relocate, we rewrite the entire block from block_pos
        // using what we can find in our tree/leaf/cache
        //
        block_ = lfs3_allocwith(lfs3, &file->h.mdir,
                LFS3_ALLOC_ERASE | LFS3_ALLOC_CLAIM);
        if (block_ < 0) {
            return block_;
        }

        off_ = 0;
        pos_ = block_pos;
        cksum_ = 0;

        // mark as uncrystallized and ungrafted
        file->h.flags |= LFS3_o_NEEDSCRYST;
    }
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_file_crystallize_(lfs3_t *lfs3, lfs3_file_t *file) {
    // do nothing if our file is already crystallized
    if (!lfs3_o_needscryst(file->h.flags)) {
        return 0;
    }
    // uncrystallized files must be unsynced
    LFS3_ASSERT(lfs3_o_needssync(file->h.flags));

    // finish crystallizing
    int err = lfs3_file_crystallize__(lfs3, file, 0, NULL, 0,
            file->leaf.pos - lfs3_bptr_off(&file->leaf.bptr), -1, -1);
    if (err) {
        return err;
    }

    // we should have crystallized
    LFS3_ASSERT(!lfs3_o_needscryst(file->h.flags));
    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_file_write_(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t pos, const uint8_t *buffer, lfs3_size_t size) {
    // we may need to graft multiple blocks/fragments
    lfs3_off_t pos_ = pos;
    const uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;

    // we can skip some btree lookups if we know we are aligned from a
    // previous iteration, we already do way too many btree lookups
    bool aligned_ = false;

    // if crystallization is disabled, just skip to writing fragments
    if (lfs3->cfg->crystal_thresh > lfs3->cfg->block_size) {
        goto fragment;
    }

    // iteratively write blocks
    while (size_ > 0) {
        // mid-crystallization? can we just resume crystallizing?
        //
        // note that the threshold to resume crystallization (prog_size),
        // is usually much lower than the threshold to start
        // crystallization (crystal_thresh)
        lfs3_off_t block_start = file->leaf.pos
                - lfs3_bptr_off(&file->leaf.bptr);
        lfs3_off_t block_end = file->leaf.pos
                + lfs3_bptr_size(&file->leaf.bptr);
        if (lfs3_bptr_isbptr(&file->leaf.bptr)
                && lfs3_bptr_iserased(&file->leaf.bptr)
                && pos_ >= block_end
                && pos_ < block_start + lfs3->cfg->block_size
                // if we're more than a crystal away, graft and check crystal
                // heuristic before resuming
                && pos_ - block_end < lfs3_max(lfs3->cfg->crystal_thresh, 1)
                // need to bail if we can't meet prog alignment
                && (pos_ + size_) - block_end >= lfs3_min(
                    lfs3->cfg->prog_size,
                    lfs3->cfg->crystal_thresh)) {
            // mark as uncrystallized to avoid allocating a new block
            file->h.flags |= LFS3_o_NEEDSCRYST;
            // crystallize
            int err = lfs3_file_crystallize__(lfs3, file, pos_, buffer_, size_,
                    block_start, -1, (pos_ + size_) - block_start);
            if (err) {
                return err;
            }

            // update buffer state
            lfs3_ssize_t d = lfs3_max(
                    file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr),
                    pos_) - pos_;
            pos_ += d;
            buffer_ += lfs3_min(d, size_);
            size_ -= lfs3_min(d, size_);

            // we should be aligned now
            aligned_ = true;
            continue;
        }

        // if we can't resume crystallization, make sure any incomplete
        // crystals are at least grafted into the tree
        int err = lfs3_file_graft_(lfs3, file);
        if (err) {
            return err;
        }

        // before we can start writing, we need to figure out if we have
        // enough fragments to start crystallizing
        //
        // we do this heuristically, by looking up our worst-case
        // crystal neighbors and using them as bounds for our current
        // crystal
        //
        // note this can end up including holes in our crystals, but
        // that's ok, we probably don't want small holes preventing
        // crystallization anyways

        // default to arbitrary alignment
        lfs3_off_t crystal_start = pos_;
        lfs3_off_t crystal_end = pos_ + size_;

        // if we haven't already exceeded our crystallization threshold,
        // find left crystal neighbor
        lfs3_off_t poke = lfs3_smax(
                crystal_start - (lfs3->cfg->crystal_thresh-1),
                0);
        if (crystal_end - crystal_start < lfs3->cfg->crystal_thresh
                && crystal_start > 0
                && poke < file->bshrub.weight
                // don't bother looking up left after the first block
                && !aligned_) {
            lfs3_bid_t bid;
            lfs3_bptr_t bptr;
            err = lfs3_file_lookupnext(lfs3, file, poke,
                    &bid, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // if left crystal neighbor is a fragment and there is no
            // obvious hole between our own crystal and our neighbor,
            // include as a part of our crystal
            //
            // holes can be quite large and shouldn't trigger
            // crystallization
            if (lfs3_bptr_isfragment(&bptr)) {
                crystal_start = bid-(bptr.d.weight-1);

            // otherwise our neighbor determines our crystal boundary
            } else {
                crystal_start = lfs3_min(bid+1, crystal_start);
            }
        }

        // if we haven't already exceeded our crystallization threshold,
        // find right crystal neighbor
        poke = lfs3_min(
                crystal_start + (lfs3->cfg->crystal_thresh-1),
                file->bshrub.weight-1);
        if (crystal_end - crystal_start < lfs3->cfg->crystal_thresh
                && crystal_end < file->bshrub.weight) {
            lfs3_bid_t bid;
            lfs3_bptr_t bptr;
            err = lfs3_file_lookupnext(lfs3, file, poke,
                    &bid, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // if right crystal neighbor is a fragment, include as a
            // part of our crystal
            if (lfs3_bptr_isfragment(&bptr)) {
                crystal_end = lfs3_max(
                        bid+1,
                        crystal_end);

            // otherwise treat as crystal boundary
            } else {
                crystal_end = lfs3_max(
                        bid-(bptr.d.weight-1),
                        crystal_end);
            }
        }

        // now that we have our crystal guess, we need to decide how to
        // write to the file

        // below our crystallization threshold? fallback to writing fragments
        //
        // note as long as crystal_thresh >= prog_size, this also ensures we
        // have enough for prog alignment
        if (crystal_end - crystal_start < lfs3->cfg->crystal_thresh) {
            goto fragment;
        }

        // exceeded crystallization threshold? we need to allocate a
        // new block

        // can we resume crystallizing with the fragments on disk?
        block_start = file->leaf.pos
                - lfs3_bptr_off(&file->leaf.bptr);
        block_end = file->leaf.pos
                + lfs3_bptr_size(&file->leaf.bptr);
        if (lfs3_bptr_isbptr(&file->leaf.bptr)
                && lfs3_bptr_iserased(&file->leaf.bptr)
                && crystal_start >= block_end
                && crystal_start < block_start + lfs3->cfg->block_size) {
            // mark as uncrystallized
            file->h.flags |= LFS3_o_NEEDSCRYST;
            // crystallize
            err = lfs3_file_crystallize__(lfs3, file, pos_, buffer_, size_,
                    block_start, -1, crystal_end - block_start);
            if (err) {
                return err;
            }

            // update buffer state, this may or may not make progress
            lfs3_ssize_t d = lfs3_max(
                    file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr),
                    pos_) - pos_;
            pos_ += d;
            buffer_ += lfs3_min(d, size_);
            size_ -= lfs3_min(d, size_);

            // we should be aligned now
            aligned_ = true;
            continue;
        }

        // if we're mid-crystallization, finish crystallizing the block
        // and graft it into our bshrub/btree
        err = lfs3_file_crystallize_(lfs3, file);
        if (err) {
            return err;
        }

        err = lfs3_file_graft_(lfs3, file);
        if (err) {
            return err;
        }

        // before we can crystallize we need to figure out the best
        // block alignment, we use the entry immediately to the left of
        // our crystal for this
        if (crystal_start > 0
                && file->bshrub.weight > 0
                // don't bother to lookup left after the first block
                && !aligned_) {
            lfs3_bid_t bid;
            lfs3_bptr_t bptr;
            err = lfs3_file_lookupnext(lfs3, file,
                    lfs3_min(
                        crystal_start-1,
                        file->bshrub.weight-1),
                    &bid, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // is our left neighbor in the same block?
            //
            // note we use the actual block start here! not the sliced
            // view! this avoids excessive recrystallizations when
            // fruncating
            if (!lfs3_bptr_ishole(&bptr)
                    && crystal_start
                        - (bid-(bptr.d.weight-1)-(
                            (lfs3_bptr_isbptr(&bptr))
                                ? lfs3_bptr_off(&bptr)
                                : 0))
                        < lfs3->cfg->block_size) {
                // include in block alignment
                crystal_start = bid-(bptr.d.weight-1);

            // no? is our left neighbor at least our left block neighbor?
            // align to block alignment
            } else if (!lfs3_bptr_ishole(&bptr)
                    && crystal_start
                        - (bid-(bptr.d.weight-1)-(
                            (lfs3_bptr_isbptr(&bptr))
                                ? lfs3_bptr_off(&bptr)
                                : 0))
                        < 2*lfs3->cfg->block_size) {
                // this should not be possible for fragments, because
                // fragment_size < block_size
                LFS3_ASSERT(lfs3_bptr_isbptr(&bptr));
                // align to block alignment
                crystal_start = bid-(bptr.d.weight-1)
                        - lfs3_bptr_off(&bptr)
                        + lfs3->cfg->block_size;
            }
        }

        // start crystallizing!
        //
        // lfs3_file_crystallize__ handles block allocation/relocation
        err = lfs3_file_crystallize__(lfs3, file, pos_, buffer_, size_,
                crystal_start, -1, crystal_end - crystal_start);
        if (err) {
            return err;
        }

        // update buffer state, this may or may not make progress
        lfs3_ssize_t d = lfs3_max(
                file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr),
                pos_) - pos_;
        pos_ += d;
        buffer_ += lfs3_min(d, size_);
        size_ -= lfs3_min(d, size_);

        // we should be aligned now
        aligned_ = true;
    }

    return 0;

fragment:;
    // crystals should be grafted before we write any fragments
    LFS3_ASSERT(!lfs3_o_needsgraft(file->h.flags));

    // do we need to discard our leaf?
    //
    // - we need to discard fragments in case the underlying rbyd
    //   compacts
    // - we need to discard overwritten blocks
    // - but we really want to keep non-overwritten blocks in case
    //   they contain erased-state!
    //
    // note we need to discard before attempting to graft since a
    // single graft may be split up into multiple commits
    //
    // unfortunately we don't know where our fragment will end up
    // until after the commit, so we can't track it in our leaf
    // quite yet
    if (!lfs3_bptr_isbptr(&file->leaf.bptr)
            || (pos_ < file->leaf.pos + file->leaf.bptr.d.weight
                && pos_ + size_ > file->leaf.pos)) {
        lfs3_file_discardleaf(file);
    }

    // graft fragments into tree
    lfs3_bptr_t bptr_;
    bptr_.d = LFS3_DATA_BUF(buffer_, size_);
    return lfs3_file_graft__(lfs3, file,
            pos_, size_,
            &bptr_);
}
#endif


// high-level file writing

#ifndef LFS3_RDONLY
lfs3_ssize_t lfs3_file_write(lfs3_t *lfs3, lfs3_file_t *file,
        const void *buffer, lfs3_size_t size) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    // can't write to readonly files
    LFS3_ASSERT(!lfs3_o_isrdonly(file->h.flags));

    // size=0 is a bit special and is guaranteed to have no effects on the
    // underlying file, this means no updating file pos or file size
    //
    // since we need to test for this, just return early
    if (size == 0) {
        return 0;
    }

    // would this write make our file larger than our file limit?
    int err;
    if (size > lfs3->file_limit - file->pos) {
        err = LFS3_ERR_FBIG;
        goto failed;
    }

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    // mark as unsynced in case we fail
    file->h.flags |= LFS3_o_NEEDSSYNC;

    // update pos if we are appending
    if (lfs3_o_isappend(file->h.flags)) {
        file->pos = lfs3_file_size_(file);
    }

    lfs3_off_t pos_ = file->pos;
    const uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        // bypass cache?
        //
        // note we flush our cache before bypassing writes, this isn't
        // strictly necessary, but enforces a more intuitive write order
        // and avoids weird cases with low-level write heuristics
        //
        if (!lfs3_o_needsflush(file->h.flags)
                && size_ >= lfs3_file_fcachesize(lfs3, file)) {
            err = lfs3_file_write_(lfs3, file,
                    pos_, buffer_, size_);
            if (err) {
                goto failed;
            }

            // after success, fill our cache with the tail of our write
            //
            // note we need to clear the cache anyways to avoid any
            // out-of-date data
            file->cache.pos = pos_ + size_ - lfs3_file_fcachesize(lfs3, file);
            lfs3_memcpy(file->cache.buffer,
                    &buffer_[size_ - lfs3_file_fcachesize(lfs3, file)],
                    lfs3_file_fcachesize(lfs3, file));
            file->cache.size = lfs3_file_fcachesize(lfs3, file);

            file->h.flags &= ~LFS3_o_NEEDSFLUSH;
            pos_ += size_;
            buffer_ += size_;
            size_ -= size_;
            continue;
        }

        // try to fill our cache
        //
        // this is a bit delicate, since our cache contains both old and
        // new data, but note:
        //
        // 1. we only write to yet unused cache memory
        //
        // 2. bypassing the cache above means we only write to the
        //    cache once, and flush at most twice
        //
        if (!lfs3_o_needsflush(file->h.flags)
                || (pos_ >= file->cache.pos
                    && pos_ <= file->cache.pos + file->cache.size
                    && pos_
                        < file->cache.pos
                            + lfs3_file_fcachesize(lfs3, file))) {
            // unused cache? we can move it where we need it
            if (!lfs3_o_needsflush(file->h.flags)) {
                file->cache.pos = pos_;
                file->cache.size = 0;
            }

            lfs3_size_t d = lfs3_min(
                    size_,
                    lfs3_file_fcachesize(lfs3, file)
                        - (pos_ - file->cache.pos));
            lfs3_memcpy(&file->cache.buffer[pos_ - file->cache.pos],
                    buffer_,
                    d);
            file->cache.size = lfs3_max(
                    file->cache.size,
                    pos_+d - file->cache.pos);

            file->h.flags |= LFS3_o_NEEDSFLUSH;
            pos_ += d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // flush our cache so the above can't fail
        err = lfs3_file_write_(lfs3, file,
                file->cache.pos, file->cache.buffer, file->cache.size);
        if (err) {
            goto failed;
        }
        file->h.flags &= ~LFS3_o_NEEDSFLUSH;
    }

    // update pos
    lfs3_size_t written = pos_ - file->pos;
    file->pos = pos_;

    // flush if requested
    if (lfs3_o_isflush(file->h.flags)) {
        err = lfs3_file_flush(lfs3, file);
        if (err) {
            goto failed;
        }
    }

    // sync if requested
    if (lfs3_o_issync(file->h.flags)) {
        err = lfs3_file_sync(lfs3, file);
        if (err) {
            goto failed;
        }
    }

    return written;

failed:;
    // mark as desync so lfs3_file_close doesn't write to disk
    file->h.flags |= LFS3_O_DESYNC;
    return err;
}
#endif

int lfs3_file_flush(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    // do nothing if our file is already flushed, crystallized,
    // and grafted
    if (!lfs3_o_needsflush(file->h.flags)
            && !lfs3_o_needscryst(file->h.flags)
            && !lfs3_o_needsgraft(file->h.flags)) {
        return 0;
    }
    // unflushed/uncrystallized files must be unsynced
    LFS3_ASSERT(lfs3_o_needssync(file->h.flags));
    // unflushed files can't be readonly
    LFS3_ASSERT(!lfs3_o_isrdonly(file->h.flags));

    // checkpoint the allocator
    int err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    #ifndef LFS3_RDONLY
    // flush our cache
    if (lfs3_o_needsflush(file->h.flags)) {
        err = lfs3_file_write_(lfs3, file,
                file->cache.pos, file->cache.buffer, file->cache.size);
        if (err) {
            goto failed;
        }

        // mark as flushed
        file->h.flags &= ~LFS3_o_NEEDSFLUSH;
    }

    // and crystallize/graft our leaf
    err = lfs3_file_crystallize_(lfs3, file);
    if (err) {
        goto failed;
    }

    err = lfs3_file_graft_(lfs3, file);
    if (err) {
        goto failed;
    }
    #endif

    return 0;

    #ifndef LFS3_RDONLY
failed:;
    // mark as desync so lfs3_file_close doesn't write to disk
    file->h.flags |= LFS3_O_DESYNC;
    return err;
    #endif
}

#ifndef LFS3_RDONLY
// this LFS3_NOINLINE is to force lfs3_file_sync_ off the stack hot-path
LFS3_NOINLINE
static int lfs3_file_sync_(lfs3_t *lfs3, lfs3_file_t *file,
        const lfs3_rattr_t *rname) {
    // build a commit of any pending file metadata
    lfs3_rattr_t rattrs[15];
    lfs3_rattr_t shrub_rattrs[5];

    // uncreated files must be unsync
    LFS3_ASSERT(!lfs3_o_needscreat(file->h.flags)
            || lfs3_o_needssync(file->h.flags));
    // small unflushed files must be unsync
    LFS3_ASSERT(!lfs3_o_needsflush(file->h.flags)
            || lfs3_o_needssync(file->h.flags));
    // uncrystallized leaves should've been flushed or discarded
    LFS3_ASSERT(!lfs3_o_needscryst(file->h.flags));
    LFS3_ASSERT(!lfs3_o_needsgraft(file->h.flags));

    // pending metadata changes?
    lfs3_rattr_t *r = rattrs;
    if (lfs3_o_needssync(file->h.flags)) {
        // explicit name?
        if (rname) {
            *r++ = LFS3_RATTR(LFS3_tag_RATTRS, +1, 1);
            *r++ = LFS3_RATTR_ARG(rname);

        // not created yet? need to convert to normal file
        } else if (lfs3_o_needscreat(file->h.flags)) {
            // convert stickynote -> reg file
            lfs3_data_t name_data;
            lfs3_stag_t name_tag = lfs3_rbyd_lookup(lfs3, &file->h.mdir.r,
                    lfs3_mrid(lfs3, file->h.mdir.mid), LFS3_TAG_STICKYNOTE,
                    &name_data);
            if (name_tag < 0) {
                // orphan flag but no stickynote tag?
                LFS3_ASSERT(name_tag != LFS3_ERR_NOENT);
                return name_tag;
            }

            *r++ = LFS3_RATTR(LFS3_tag_MASK8 | LFS3_TAG_REG, 0, 3,
                    LFS3_FROM_GRAFT);
            *(lfs3_data_t*)r = name_data;
            r += 3;
        }

        // pending small file flush?
        if (lfs3_o_needsflush(file->h.flags)) {
            // this only works if the file is entirely in our cache
            LFS3_ASSERT(file->cache.pos == 0);
            LFS3_ASSERT(file->cache.size == lfs3_file_size_(file));

            // discard any lingering bshrub state
            lfs3_file_discardbshrub(file);

            // build a small shrub commit
            if (file->cache.size > 0) {
                lfs3_rattr_t *shrub_r = shrub_rattrs;
                *shrub_r++ = LFS3_RATTR(LFS3_TAG_DATA, -2, 2, LFS3_FROM_BUF);
                *shrub_r++ = LFS3_RATTR_WEIGHT(+file->cache.size);
                *shrub_r++ = LFS3_RATTR_ARG(file->cache.buffer);
                *shrub_r++ = LFS3_RATTR_ARG(file->cache.size);
                *shrub_r++ = LFS3_RATTR_NULL;
                LFS3_ASSERT((lfs3_size_t)(shrub_r-shrub_rattrs)
                        <= sizeof(shrub_rattrs)/sizeof(lfs3_rattr_t));

                *r++ = LFS3_RATTR(LFS3_tag_SHRUBCOMMIT, 0, 4);
                *r++ = LFS3_RATTR_ARG(&file->bshrub);
                *r++ = LFS3_RATTR_ARG(0);
                *r++ = LFS3_RATTR_ARG(shrub_rattrs);
                *r++ = LFS3_RATTR_ARG(
                        +lfs3->rattr_estimate + file->cache.size);
            }
        }

        // make sure data is on-disk before committing metadata
        if (lfs3_file_size_(file) > 0
                && !lfs3_o_needsflush(file->h.flags)) {
            int err = lfs3_bd_sync(lfs3, 0);
            if (err) {
                return err;
            }
        }

        // zero size files should have no bshrub/btree
        LFS3_ASSERT(lfs3_file_size_(file) > 0 || !file->bshrub.trunk);

        // no bshrub/btree?
        if (lfs3_file_size_(file) == 0) {
            *r++ = LFS3_RATTR(
                    LFS3_tag_RM | LFS3_tag_MASK8 | LFS3_TAG_STRUCT,
                    0, 0);
        // bshrub?
        } else if (lfs3_bshrub_isbshrub(&file->bshrub)
                || lfs3_o_needsflush(file->h.flags)) {
            *r++ = LFS3_RATTR(
                    LFS3_tag_MASK8 | LFS3_TAG_BSHRUB,
                    0, 1,
                    LFS3_FROM_SHRUB);
            // note we use the staged trunk here
            *r++ = LFS3_RATTR_ARG(&file->bshrub_);
        // btree?
        } else if (lfs3_bshrub_isbtree(&file->bshrub)) {
            *r++ = LFS3_RATTR(
                    LFS3_tag_MASK8 | LFS3_TAG_BTREE,
                    0, 1,
                    LFS3_FROM_BTREE);
            *r++ = LFS3_RATTR_ARG(&file->bshrub);
        } else {
            LFS3_UNREACHABLE();
        }
    }

    // pending custom attributes?
    //
    // this gets real messy, since users can change custom attributes
    // whenever they want without informing littlefs, the best we can do
    // is read from disk to manually check if any attributes changed
    bool attrs = lfs3_o_needssync(file->h.flags);
    if (!attrs) {
        for (lfs3_size_t i = 0; i < file->cfg->attr_count; i++) {
            // skip readonly attrs and lazy attrs
            if (lfs3_o_isrdonly(file->cfg->attrs[i].flags)
                    || lfs3_a_islazy(file->cfg->attrs[i].flags)) {
                continue;
            }

            // lookup the attr
            lfs3_data_t data;
            lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, &file->h.mdir,
                    LFS3_TAG_ATTR(file->cfg->attrs[i].type),
                    &data);
            if (tag < 0 && tag != LFS3_ERR_NOENT) {
                return tag;
            }

            // does disk match our attr?
            lfs3_scmp_t cmp = lfs3_attr_cmp(lfs3, &file->cfg->attrs[i],
                    (tag != LFS3_ERR_NOENT) ? &data : NULL);
            if (cmp < 0) {
                return cmp;
            }

            if (cmp != LFS3_CMP_EQ) {
                attrs = true;
                break;
            }
        }
    }
    if (attrs) {
        // need to append custom attributes
        *r++ = LFS3_RATTR(LFS3_tag_ATTRS, 0, 2);
        *r++ = LFS3_RATTR_ARG(file->cfg->attrs);
        *r++ = LFS3_RATTR_ARG(file->cfg->attr_count);
    }

    // pending metadata? looks like we need to write to disk
    if (r > rattrs) {
        // make sure we don't overflow our rattr buffer
        *r++ = LFS3_RATTR_NULL;
        LFS3_ASSERT((lfs3_size_t)(r-rattrs)
                <= sizeof(rattrs)/sizeof(lfs3_rattr_t));

        // and commit!
        int err = lfs3_mdir_commit(lfs3, &file->h.mdir, rattrs);
        if (err) {
            return err;
        }
    }

    // update in-device state
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_TYPE_REG
                && h->mdir.mid == file->h.mdir.mid
                // don't double update
                && h != &file->h) {
            lfs3_file_t *file_ = (lfs3_file_t*)h;
            // notify all files of creation
            file_->h.flags &= ~LFS3_o_NEEDSCREAT;

            // mark desynced files an unsynced
            if (lfs3_o_isdesync(file_->h.flags)) {
                file_->h.flags |= LFS3_o_NEEDSSYNC;

            // update synced files
            } else {
                // update flags
                file_->h.flags &= ~LFS3_o_NEEDSSYNC
                        & ~LFS3_o_NEEDSFLUSH
                        & ~LFS3_o_NEEDSCRYST
                        & ~LFS3_o_NEEDSGRAFT;
                // update shrubs
                file_->bshrub = file->bshrub;
                // update leaves
                file_->leaf = file->leaf;

                // update caches
                //
                // note we need to be careful if caches have different
                // sizes, prefer the most recent data in this case
                lfs3_size_t d = file->cache.size - lfs3_min(
                        lfs3_file_fcachesize(lfs3, file_),
                        file->cache.size);
                file_->cache.pos = file->cache.pos + d;
                lfs3_memcpy(file_->cache.buffer,
                        file->cache.buffer + d,
                        file->cache.size - d);
                file_->cache.size = file->cache.size - d;

                // update any custom attrs
                for (lfs3_size_t i = 0; i < file->cfg->attr_count; i++) {
                    if (lfs3_o_isrdonly(file->cfg->attrs[i].flags)) {
                        continue;
                    }

                    for (lfs3_size_t j = 0; j < file_->cfg->attr_count; j++) {
                        if (!(file_->cfg->attrs[j].type
                                    == file->cfg->attrs[i].type
                                && !lfs3_o_iswronly(
                                    file_->cfg->attrs[j].flags))) {
                            continue;
                        }

                        if (lfs3_attr_isnoattr(&file->cfg->attrs[i])) {
                            if (file_->cfg->attrs[j].size) {
                                *file_->cfg->attrs[j].size = LFS3_ERR_NOATTR;
                            }
                        } else {
                            lfs3_size_t d = lfs3_min(
                                    lfs3_attr_size(&file->cfg->attrs[i]),
                                    file_->cfg->attrs[j].buffer_size);
                            lfs3_memcpy(file_->cfg->attrs[j].buffer,
                                    file->cfg->attrs[i].buffer,
                                    d);
                            if (file_->cfg->attrs[j].size) {
                                *file_->cfg->attrs[j].size = d;
                            }
                        }
                    }
                }
            }
        }
    }

    // mark as synced
    file->h.flags &= ~LFS3_o_NEEDSCREAT
            & ~LFS3_o_NEEDSSYNC
            & ~LFS3_o_NEEDSFLUSH
            & ~LFS3_o_NEEDSCRYST
            & ~LFS3_o_NEEDSGRAFT;
    return 0;
}
#endif

int lfs3_file_sync(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    // removed? ignore sync requests
    if (lfs3_o_iszombie(file->h.flags)) {
        return 0;
    }

    #ifndef LFS3_RDONLY
    // can we get away with a small file flush?
    //
    // this merges the data flush with metadata sync in a single commit
    // if the file is small enough to fit in the cache
    int err;
    if (file->cache.size == lfs3_file_size_(file)
            && file->cache.size <= lfs3->cfg->shrub_size
            && file->cache.size <= lfs3->cfg->fragment_size
            && file->cache.size < lfs3_max(lfs3->cfg->crystal_thresh, 1)) {
        // discard any overwritten leaves, this also clears the
        // LFS3_o_NEEDSCRYST and LFS3_o_NEEDSGRAFT flags
        lfs3_file_discardleaf(file);

    // flush any data in our cache, this is a noop if already flushed
    //
    // note that flush does not change the actual file data, so if
    // flush succeeds but mdir commit fails it's ok to fall back to
    // our flushed state
    } else {
        err = lfs3_file_flush(lfs3, file);
        if (err) {
            goto failed;
        }
    }

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    // commit any pending metadata to disk
    //
    // the use of a second function here is mainly to isolate the
    // stack costs of lfs3_file_flush and lfs3_file_sync_
    //
    err = lfs3_file_sync_(lfs3, file, NULL);
    if (err) {
        goto failed;
    }
    #endif

    // clear desync flag
    file->h.flags &= ~LFS3_O_DESYNC;
    return 0;

    #ifndef LFS3_RDONLY
failed:;
    file->h.flags |= LFS3_O_DESYNC;
    return err;
    #endif
}

int lfs3_file_desync(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    (void)file;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    #ifndef LFS3_RDONLY
    // mark as desynced
    file->h.flags |= LFS3_O_DESYNC;
    #endif
    return 0;
}

int lfs3_file_resync(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    (void)file;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    #ifndef LFS3_RDONLY
    // removed? we can't resync
    int err;
    if (lfs3_o_iszombie(file->h.flags)) {
        err = LFS3_ERR_NOENT;
        goto failed;
    }

    // do nothing if already in-sync
    if (lfs3_o_needssync(file->h.flags)) {
        // discard cached state
        lfs3_file_discardbshrub(file);
        lfs3_file_discardcache(file);
        lfs3_file_discardleaf(file);

        // refetch the file struct from disk
        err = lfs3_file_fetch(lfs3, file,
                // don't truncate again!
                file->h.flags & ~LFS3_O_TRUNC);
        if (err) {
            goto failed;
        }
    }
    #endif

    // clear desync flag
    file->h.flags &= ~LFS3_O_DESYNC;
    return 0;

    #ifndef LFS3_RDONLY
failed:;
    file->h.flags |= LFS3_O_DESYNC;
    return err;
    #endif
}

// other file operations

lfs3_soff_t lfs3_file_seek(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_soff_t off, uint32_t whence) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    // TODO check for out-of-range?

    // figure out our new file position
    lfs3_off_t pos_;
    if (whence == LFS3_SEEK_SET) {
        pos_ = off;
    } else if (whence == LFS3_SEEK_CUR) {
        pos_ = file->pos + off;
    } else if (whence == LFS3_SEEK_END) {
        pos_ = lfs3_file_size_(file) + off;
    } else {
        LFS3_UNREACHABLE();
    }

    // out of range?
    if (pos_ > lfs3->file_limit) {
        return LFS3_ERR_INVAL;
    }

    // update file position
    file->pos = pos_;
    return pos_;
}

lfs3_soff_t lfs3_file_tell(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    return file->pos;
}

lfs3_soff_t lfs3_file_rewind(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    file->pos = 0;
    return 0;
}

lfs3_soff_t lfs3_file_size(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));

    return lfs3_file_size_(file);
}

#ifndef LFS3_RDONLY
int lfs3_file_truncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size_) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    // can't write to readonly files
    LFS3_ASSERT(!lfs3_o_isrdonly(file->h.flags));

    // do nothing if our size does not change
    lfs3_off_t size = lfs3_file_size_(file);
    if (lfs3_file_size_(file) == size_) {
        return 0;
    }

    // exceeds our file limit?
    int err;
    if (size_ > lfs3->file_limit) {
        err = LFS3_ERR_FBIG;
        goto failed;
    }

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    // mark as unsynced in case we fail
    file->h.flags |= LFS3_o_NEEDSSYNC;

    // make sure any incomplete are at least grafted
    err = lfs3_file_graft_(lfs3, file);
    if (err) {
        goto failed;
    }

    // truncate our btree
    lfs3_bptr_t bptr_;
    bptr_.d = LFS3_DATA_HOLE(lfs3_smax(size_ - size, 0));
    err = lfs3_file_graft__(lfs3, file,
            lfs3_min(size, size_), lfs3_smax(size - size_, 0),
            &bptr_);
    if (err) {
        goto failed;
    }

    // truncate our leaf
    //
    // note we don't unconditionally discard to match fruncate, where we
    // _really_ don't want to discard erased-state
    lfs3_bptr_slice(&file->leaf.bptr,
            -1,
            size_ - lfs3_min(file->leaf.pos, size_));
    file->leaf.pos = lfs3_min(file->leaf.pos, size_);
    // mark as crystallized if this truncates our erased-state
    if (lfs3_bptr_isbptr(&file->leaf.bptr)
            && lfs3_bptr_off(&file->leaf.bptr)
                    + lfs3_bptr_size(&file->leaf.bptr)
                < lfs3_bptr_cksize(&file->leaf.bptr)) {
        lfs3_bptr_claim(&file->leaf.bptr);
        file->h.flags &= ~LFS3_o_NEEDSCRYST;
    }
    // discard if our leaf is a fragment or completely truncated, we
    // can't rely on any in-bshrub/btree state
    if (!lfs3_bptr_isbptr(&file->leaf.bptr)
            || lfs3_bptr_size(&file->leaf.bptr) == 0) {
        lfs3_file_discardleaf(file);
    }

    // truncate our cache
    file->cache.size = lfs3_min(
            file->cache.size,
            size_ - lfs3_min(file->cache.pos, size_));
    file->cache.pos = lfs3_min(file->cache.pos, size_);
    // mark as flushed if this completely truncates our cache
    if (file->cache.size == 0) {
        lfs3_file_discardcache(file);
    }

    return 0;

failed:;
    // mark as desync so lfs3_file_close doesn't write to disk
    file->h.flags |= LFS3_O_DESYNC;
    return err;
}
#endif

#ifndef LFS3_RDONLY
int lfs3_file_fruncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size_) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    // can't write to readonly files
    LFS3_ASSERT(!lfs3_o_isrdonly(file->h.flags));

    // do nothing if our size does not change
    lfs3_off_t size = lfs3_file_size_(file);
    if (size == size_) {
        return 0;
    }

    // exceeds our file limit?
    int err;
    if (size_ > lfs3->file_limit) {
        err = LFS3_ERR_FBIG;
        goto failed;
    }

    // checkpoint the allocator
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    // mark as unsynced in case we fail
    file->h.flags |= LFS3_o_NEEDSSYNC;

    // make sure any incomplete are at least grafted
    err = lfs3_file_graft_(lfs3, file);
    if (err) {
        goto failed;
    }

    // fruncate our btree
    lfs3_bptr_t bptr_;
    bptr_.d = LFS3_DATA_HOLE(lfs3_smax(size_ - size, 0));
    err = lfs3_file_graft__(lfs3, file,
            0, lfs3_smax(size - size_, 0),
            &bptr_);
    if (err) {
        goto failed;
    }

    // fruncate our leaf
    //
    // note we _really_ don't want to discard erased-state if possible,
    // as fruncate is intended for logging operations, otherwise we'd
    // just unconditionally discard the leaf and avoid this hassle
    lfs3_bptr_slice(&file->leaf.bptr,
            lfs3_min(
                lfs3_smax(
                    size - size_ - file->leaf.pos,
                    0),
                lfs3_bptr_size(&file->leaf.bptr)),
            -1);
    file->leaf.pos -= lfs3_smin(
            size - size_,
            file->leaf.pos);
    // discard if our leaf is a fragment or completely truncated, we
    // can't rely on any in-bshrub/btree state
    if (!lfs3_bptr_isbptr(&file->leaf.bptr)
            || lfs3_bptr_size(&file->leaf.bptr) == 0) {
        lfs3_file_discardleaf(file);
    }

    // fruncate our cache
    lfs3_memmove(file->cache.buffer,
            &file->cache.buffer[lfs3_min(
                lfs3_smax(
                    size - size_ - file->cache.pos,
                    0),
                file->cache.size)],
            file->cache.size - lfs3_min(
                lfs3_smax(
                    size - size_ - file->cache.pos,
                    0),
                file->cache.size));
    file->cache.size -= lfs3_min(
            lfs3_smax(
                size - size_ - file->cache.pos,
                0),
            file->cache.size);
    file->cache.pos -= lfs3_smin(
            size - size_,
            file->cache.pos);
    // mark as flushed if this completely truncates our cache
    if (file->cache.size == 0) {
        lfs3_file_discardcache(file);
    }

    // fruncate _does_ update pos, to keep the same pos relative to end
    // of file
    //
    // yes, this is inconsistent from truncate, but we can't let pos go
    // negative
    //
    // or is it? arguably truncate keeps pos relative to front of file,
    // and fruncate is just doing to inverse
    file->pos -= lfs3_smin(
            size - size_,
            file->pos);

    return 0;

failed:;
    // mark as desync so lfs3_file_close doesn't write to disk
    file->h.flags |= LFS3_O_DESYNC;
    return err;
}
#endif

// common file check function
static int lfs3_file_ck(lfs3_t *lfs3, lfs3_file_t *file, uint32_t flags) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    // unknown ck flags? note only some gc flags work on files
    LFS3_ASSERT((flags & ~(
            LFS3_O_CKMETA
                | LFS3_O_CKDATA)) == 0);
    // these flags require a readable file
    LFS3_ASSERT(!lfs3_o_iswronly(flags) || !lfs3_t_isckmeta(flags));
    LFS3_ASSERT(!lfs3_o_iswronly(flags) || !lfs3_t_isckdata(flags));

    // validate ungrafted data block?
    if (lfs3_t_isckdata(flags)
            && lfs3_o_needsgraft(file->h.flags)) {
        LFS3_ASSERT(lfs3_bptr_isbptr(&file->leaf.bptr));
        int err = lfs3_bptr_ck(lfs3, &file->leaf.bptr);
        if (err) {
            return err;
        }
    }

    // traverse the file's bshrub/btree
    lfs3_btrv_t btrv;
    lfs3_btrv_init(&btrv);
    while (true) {
        lfs3_data_t data;
        lfs3_stag_t tag = lfs3_bshrub_traverse(lfs3, &file->bshrub, &btrv,
                NULL, NULL, &data);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }

        // validate btree nodes?
        //
        // this may end up revalidating some btree nodes when ckfetches
        // is enabled, but we need to revalidate cached btree nodes or
        // we risk missing errors in ckmeta scans
        if ((lfs3_t_isckmeta(flags)
                    || lfs3_t_isckdata(flags))
                && tag == LFS3_TAG_BRANCH) {
            lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)data.u.buffer;
            int err = lfs3_rbyd_ckfetch(lfs3, rbyd,
                    rbyd->blocks[0], rbyd->trunk,
                    rbyd->cksum);
            if (err) {
                return err;
            }
        }

        // validate data blocks?
        if (lfs3_t_isckdata(flags)
                && tag == LFS3_TAG_BLOCK) {
            lfs3_bptr_t bptr;
            int err = lfs3_data_readbptr(lfs3, &data,
                    &bptr);
            if (err) {
                return err;
            }

            err = lfs3_bptr_ck(lfs3, &bptr);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

int lfs3_file_ckmeta(lfs3_t *lfs3, lfs3_file_t *file) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    // can't read from writeonly files
    LFS3_ASSERT(!lfs3_o_iswronly(file->h.flags));

    return lfs3_file_ck(lfs3, file, LFS3_O_CKMETA);
}

int lfs3_file_ckdata(lfs3_t *lfs3, lfs3_file_t *file) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &file->h));
    // can't read from writeonly files
    LFS3_ASSERT(!lfs3_o_iswronly(file->h.flags));

    return lfs3_file_ck(lfs3, file, LFS3_O_CKDATA);
}



/// Simple key-value API ///

// a simple key-value API is easier to use if your file fits in RAM, and
// if that's all you need you can potentially compile-out the more
// advanced file operations

// kv file config, we need to explicitly disable the file cache
static const struct lfs3_file_cfg lfs3_file_kvcfg = {
    // TODO is this the best way to do this?
    .fcache_buffer = (uint8_t*)1,
    .fcache_size = 0,
};

lfs3_ssize_t lfs3_get(lfs3_t *lfs3, const char *path,
        void *buffer, lfs3_size_t size) {
    // we just use the file API here, but with no cache so all reads
    // bypass the cache
    lfs3_file_t file;
    int err = lfs3_file_opencfg_(lfs3, &file, path, LFS3_O_RDONLY,
            &lfs3_file_kvcfg);
    if (err) {
        return err;
    }

    lfs3_ssize_t size_ = lfs3_file_read(lfs3, &file, buffer, size);

    // unconditionally close
    err = lfs3_file_close(lfs3, &file);
    // we didn't allocate anything, so this can't fail
    LFS3_ASSERT(!err);

    return size_;
}

lfs3_ssize_t lfs3_size(lfs3_t *lfs3, const char *path) {
    // we just use the file API here, but with no cache so all reads
    // bypass the cache
    lfs3_file_t file;
    int err = lfs3_file_opencfg_(lfs3, &file, path, LFS3_O_RDONLY,
            &lfs3_file_kvcfg);
    if (err) {
        return err;
    }

    lfs3_ssize_t size_ = lfs3_file_size_(&file);

    // unconditionally close
    err = lfs3_file_close(lfs3, &file);
    // we didn't allocate anything, so this can't fail
    LFS3_ASSERT(!err);

    return size_;
}

#ifndef LFS3_RDONLY
int lfs3_set(lfs3_t *lfs3, const char *path,
        const void *buffer, lfs3_size_t size) {
    // LFS3_o_SET is a special flag specifically to make lfs3_set atomic
    // when possible
    //
    // - if we need to reserve the mid _and_ we're small, everything is
    //   committed/broadcasted in lfs3_file_opencfg
    //
    // - otherwise (exists? stickynote?), we flush/sync/broadcast
    //   normally in lfs3_file_close, lfs3_file_sync has its own logic
    //   to try to commit small files atomically
    //
    struct lfs3_file_cfg cfg = {
        .fcache_buffer = (uint8_t*)buffer,
        .fcache_size = size,
    };
    lfs3_file_t file;
    int err = lfs3_file_opencfg_(lfs3, &file, path,
            LFS3_O_WRONLY | LFS3_O_CREAT | LFS3_O_TRUNC | LFS3_o_SET,
            &cfg);
    if (err) {
        return err;
    }

    // let close do any remaining work
    return lfs3_file_close(lfs3, &file);
}
#endif




/// High-level filesystem operations ///

// needed in lfs3_init
static int lfs3_deinit(lfs3_t *lfs3);

// initialize littlefs state, assert on bad configuration
static int lfs3_init(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_cfg *cfg) {
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_M_RDWR)
                | LFS3_M_RDONLY
                | LFS3_M_FLUSH
                | LFS3_M_SYNC
                | LFS3_IFDEF_REVPERTURB(LFS3_M_REVPERTURB, 0)
                | LFS3_IFDEF_REVNOISE(LFS3_M_REVNOISE, 0)
                | LFS3_IFDEF_CKPROGS(LFS3_M_CKPROGS, 0)
                | LFS3_IFDEF_CKFETCHES(LFS3_M_CKFETCHES, 0)
                | LFS3_IFDEF_CKMETAPARITY(LFS3_M_CKMETAPARITY, 0)
                | LFS3_IFDEF_CKDATACKSUMS(LFS3_M_CKDATACKSUMS, 0)
                | LFS3_IFDEF_GBMAP(LFS3_F_GBMAP, 0))) == 0);
    // TODO this all needs to be cleaned up
    lfs3->cfg = cfg;
    int err = 0;

    // validate that the lfs3-cfg sizes were initiated properly before
    // performing any arithmetic logics with them
    LFS3_ASSERT(lfs3->cfg->read_size != 0);
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(lfs3->cfg->prog_size != 0);
    #endif
    LFS3_ASSERT(lfs3->cfg->rcache_size != 0);
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(lfs3->cfg->pcache_size != 0);
    #endif

    // cache sizes must be a multiple of their operation sizes
    LFS3_ASSERT(lfs3->cfg->rcache_size % lfs3->cfg->read_size == 0);
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(lfs3->cfg->pcache_size % lfs3->cfg->prog_size == 0);
    #endif

    // block_size must be a multiple of both prog/read size
    LFS3_ASSERT(lfs3->cfg->block_size % lfs3->cfg->read_size == 0);
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(lfs3->cfg->block_size % lfs3->cfg->prog_size == 0);
    #endif

    // block_size is currently limited to 28-bits
    LFS3_ASSERT(lfs3->cfg->block_size <= 0x0fffffff);

    // check gc stuff
    #ifdef LFS3_GC
    // unknown gc flags?
    LFS3_ASSERT((lfs3->cfg->gc_flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                | LFS3_GC_CKMETA
                | LFS3_GC_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0)))) == 0);

    // check that gc_compactmeta_thresh makes sense
    //
    // metadata can't be compacted below block_size/2, and metadata can't
    // exceed a block
    LFS3_ASSERT(lfs3->cfg->gc_compactmeta_thresh == 0
            || lfs3->cfg->gc_compactmeta_thresh >= lfs3->cfg->block_size/2);
    LFS3_ASSERT(lfs3->cfg->gc_compactmeta_thresh == (lfs3_size_t)-1
            || lfs3->cfg->gc_compactmeta_thresh <= lfs3->cfg->block_size);
    #endif

    #ifndef LFS3_RDONLY
    // shrub_size must be <= block_size/8
    LFS3_ASSERT(lfs3->cfg->shrub_size <= lfs3->cfg->block_size/8);
    // fragment_size must be <= block_size/4
    LFS3_ASSERT(lfs3->cfg->fragment_size <= lfs3->cfg->block_size/4);
    #endif

    // TODO move this to mount?
    // setup flags
    lfs3->flags = flags
            // assume we contain orphans until proven otherwise
            | LFS3_IFDEF_RDONLY(0, LFS3_I_MKCONSISTENT)
            // default to lookaheadable
            | LFS3_IFDEF_RDONLY(0, LFS3_I_LOOKAHEAD)
            // default to assuming we need compaction somewhere, worst
            // case this just makes lfs3_fs_gc read more than is
            // strictly needed
            | LFS3_IFDEF_RDONLY(0, LFS3_I_COMPACTMETA)
            // default to needing a ckmeta/ckdata scan
            | LFS3_I_CKMETA
            | LFS3_I_CKDATA;

    // copy block_count so we can mutate it
    lfs3->block_count = lfs3->cfg->block_count;

    // setup read cache
    lfs3->rcache.block = 0;
    lfs3->rcache.off = 0;
    lfs3->rcache.size = 0;
    if (lfs3->cfg->rcache_buffer) {
        lfs3->rcache.buffer = lfs3->cfg->rcache_buffer;
    } else {
        lfs3->rcache.buffer = lfs3_malloc(lfs3->cfg->rcache_size);
        if (!lfs3->rcache.buffer) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
    }

    // setup program cache
    #ifndef LFS3_RDONLY
    lfs3->pcache.block = 0;
    lfs3->pcache.off = 0;
    lfs3->pcache.size = 0;
    if (lfs3->cfg->pcache_buffer) {
        lfs3->pcache.buffer = lfs3->cfg->pcache_buffer;
    } else {
        lfs3->pcache.buffer = lfs3_malloc(lfs3->cfg->pcache_size);
        if (!lfs3->pcache.buffer) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
    }
    #endif

    // setup ptail, nothing should actually check off=0
    #ifdef LFS3_CKMETAPARITY
    lfs3->ptail.block = 0;
    lfs3->ptail.off = 0;
    #endif

    // setup lookahead buffer, note mount finishes initializing this after
    // we establish a decent pseudo-random seed
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(lfs3->cfg->lookahead_size > 0);
    if (lfs3->cfg->lookahead_buffer) {
        lfs3->lookahead.buffer = lfs3->cfg->lookahead_buffer;
    } else {
        lfs3->lookahead.buffer = lfs3_malloc(lfs3->cfg->lookahead_size);
        if (!lfs3->lookahead.buffer) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
    }
    lfs3->lookahead.window = 0;
    lfs3->lookahead.off = 0;
    lfs3->lookahead.known = 0;
    lfs3->lookahead.ckpoint = 0;
    lfs3_alloc_discard_(lfs3);
    #endif

    // check that the size limits are sane
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(lfs3->cfg->name_limit <= LFS3_NAME_MAX);
    lfs3->name_limit = lfs3->cfg->name_limit;
    if (!lfs3->name_limit) {
        lfs3->name_limit = LFS3_NAME_MAX;
    }

    LFS3_ASSERT(lfs3->cfg->file_limit <= LFS3_FILE_MAX);
    lfs3->file_limit = lfs3->cfg->file_limit;
    if (!lfs3->file_limit) {
        lfs3->file_limit = LFS3_FILE_MAX;
    }
    #endif

    // TODO do we need to recalculate these after mount?

    // find the number of bits to use for recycle counters
    //
    // Add 1 for the initial erase, and multiply by 2 since we alternate
    // which metadata block we erase each compaction.
    //
    // We're currently limited to 20-bits to keep space for
    // perturb/low-effort debug bits, though this could be relaxed
    // in the future (though >28 bits may cause problems since we need
    // some bits to tell revisions apart).
    //
    #ifndef LFS3_RDONLY
    if (lfs3->cfg->block_recycles != -1) {
        lfs3->recycle_bits = lfs3_nlog2(2*(lfs3->cfg->block_recycles+1)+1)-1;
        LFS3_ASSERT(lfs3->recycle_bits <= 20);
    } else {
        lfs3->recycle_bits = -1;
    }
    #endif

    // TODO should we just use constants for these to save on code size?

    // calculate the upper-bound cost of a single rbyd attr after compaction
    //
    // Note that with rebalancing during compaction, we know the number
    // of inner nodes is roughly the same as the number of tags. Unfortunately,
    // our inner node encoding is rather poor, requiring 2 alts and terminating
    // with a 4-byte null tag:
    //
    //   a_0 = 3t + 4
    //
    // If we could build each trunk perfectly, we could get this down to only
    // 1 alt per tag. But this would require unbounded RAM:
    //
    //   a_inf = 2t
    //
    // Or, if you build a bounded number of layers perfectly:
    //
    //         2t   3t + 4
    //   a_1 = -- + ------
    //          2      2
    //
    //   a_n = 2t*(1-2^-n) + (3t + 4)*2^-n
    //
    // But this would be a tradeoff in code complexity.
    //
    // The worst-case tag encoding, t, depends on our size-limit and
    // block-size. The weight can never exceed size-limit, and the size/jump
    // field can never exceed a single block:
    //
    //   t = 2 + log128(file_limit+1) + log128(block_size)
    //
    // But file_limit isn't something we change often, so we can just use
    // LFS3_LEB128_DSIZE to simplify things.
    //
    // Note this is different from LFS3_TAG_DSIZE, which is the worst case
    // tag encoding at compile-time.
    //
    #ifndef LFS3_RDONLY
    uint8_t tag_estimate
            = 2
            + LFS3_LEB128_DSIZE
            + (lfs3_nlog2(lfs3->cfg->block_size)+7-1)/7;
    LFS3_ASSERT(tag_estimate <= LFS3_TAG_DSIZE);
    lfs3->rattr_estimate = 3*tag_estimate + 4;
    #endif

    // calculate the upper-bound cost of a single mdir attr after compaction
    //
    // This is the same as rattr_estimate, except we can assume a weight<=1.
    //
    #ifndef LFS3_RDONLY
    tag_estimate
            = 2
            + 1
            + (lfs3_nlog2(lfs3->cfg->block_size)+7-1)/7;
    LFS3_ASSERT(tag_estimate <= LFS3_TAG_DSIZE);
    lfs3->mattr_estimate = 3*tag_estimate + 4;
    #endif

    // calculate the number of bits we need to reserve for mdir rids
    //
    // Worst case (or best case?) each metadata entry is a single tag. In
    // theory each entry also needs a did+name, but with power-of-two
    // rounding, this is negligible
    //
    // Assuming a _perfect_ compaction algorithm (requires unbounded RAM),
    // each tag also needs ~1 alt, this gives us:
    //
    //           block_size   block_size
    //   mrids = ---------- = ----------
    //              a_inf         2t
    //
    // Assuming t=4 bytes, the minimum tag encoding:
    //
    //           block_size   block_size
    //   mrids = ---------- = ----------
    //               2*4           8
    //
    // Note we can't assume ~1/2 block utilization here, as an mdir may
    // temporarily fill with more mids before compaction occurs.
    //
    // Rounding up to the nearest power of two:
    //
    //                (block_size)
    //   mbits = nlog2(----------) = nlog2(block_size) - 3
    //                (     8    )
    //
    // Note if you divide before the nlog2, make sure to use ceiling
    // division for compatibility if block_size is not aligned to 8 bytes.
    //
    // Note note our actual compaction algorithm is not perfect, and
    // requires 3t+4 bytes per tag, or with t=4 bytes => ~block_size/12
    // metadata entries per block. But we intentionally don't leverage this
    // to maintain compatibility with a theoretical perfect implementation.
    //
    lfs3->mbits = lfs3_nlog2(lfs3->cfg->block_size) - 3;

    // zero linked-list of opened mdirs
    lfs3->handles = NULL;

    // TODO are these zeros accomplished by zerogdelta in mountinited?
    // should the zerogdelta be dropped?
    // TODO should we just call zerogdelta here?

    // zero gstate
    lfs3->gcksum = 0;
    #ifndef LFS3_RDONLY
    lfs3->gcksum_p = 0;
    lfs3->gcksum_d = 0;
    #endif

    lfs3_grm_discard(&lfs3->grm);
    #ifndef LFS3_RDONLY
    lfs3_memset(lfs3->grm_p, 0, LFS3_GRM_DSIZE);
    lfs3_memset(lfs3->grm_d, 0, LFS3_GRM_DSIZE);
    #endif

    // setup other global gbmap state
    #ifdef LFS3_GBMAP
    lfs3_gbmap_init(&lfs3->gbmap);
    lfs3_memset(lfs3->gbmap_p, 0, LFS3_GBMAP_DSIZE);
    lfs3_memset(lfs3->gbmap_d, 0, LFS3_GBMAP_DSIZE);
    #endif

    // setup null evict queue
    #if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
    if (lfs3->cfg->evictqueue_array) {
        lfs3->evictqueue.queue = lfs3->cfg->evictqueue_array;
    } else {
        lfs3->evictqueue.queue = lfs3_malloc(
                lfs3->cfg->evictqueue_count*sizeof(lfs3_evict_t));
        if (!lfs3->evictqueue.queue) {
            err = LFS3_ERR_NOMEM;
            goto failed;
        }
    }
    lfs3_evict_discard(lfs3);
    #endif

    // setup gc state
    #ifdef LFS3_GC
    lfs3_mgc_init(&lfs3->gc, lfs3->cfg->gc_flags);
    lfs3_handle_open(lfs3, &lfs3->gc.t.h);
    #endif

    return 0;

failed:;
    lfs3_deinit(lfs3);
    return err;
}

static int lfs3_deinit(lfs3_t *lfs3) {
    // free allocated memory
    if (!lfs3->cfg->rcache_buffer) {
        lfs3_free(lfs3->rcache.buffer);
    }

    #ifndef LFS3_RDONLY
    if (!lfs3->cfg->pcache_buffer) {
        lfs3_free(lfs3->pcache.buffer);
    }
    #endif

    #ifndef LFS3_RDONLY
    if (!lfs3->cfg->lookahead_buffer) {
        lfs3_free(lfs3->lookahead.buffer);
    }
    #endif

    #if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
    if (!lfs3->cfg->evictqueue_array) {
        lfs3_free(lfs3->evictqueue.queue);
    }
    #endif

    return 0;
}



/// Mount/unmount ///

// compat flags things

#define LFS3_COMPAT(rcompat, wcompat) \
    (((wcompat) << 16) | (rcompat))

static inline lfs3_compat_t lfs3_compat_rcompat(lfs3_compat_t compat) {
    return 0xffff & compat;
}

static inline lfs3_compat_t lfs3_compat_wcompat(lfs3_compat_t compat) {
    return 0xffff & (compat >> 16);
}

static inline bool lfs3_compat_isgbmap(lfs3_compat_t compat) {
    return lfs3_compat_wcompat(compat) & LFS3_WCOMPAT_GBMAP;
}

// figure out what compat flags the current fs configuration needs
static inline lfs3_compat_t lfs3_fs_compat(const lfs3_t *lfs3) {
    (void)lfs3;
    return LFS3_COMPAT(
            LFS3_RCOMPAT_GRM
                | LFS3_RCOMPAT_STICKYNOTE,
            LFS3_WCOMPAT_GCKSUM
                | LFS3_WCOMPAT_DIR
                | LFS3_IFDEF_GBMAP(
                    (lfs3_f_isgbmap(lfs3->flags))
                        ? LFS3_WCOMPAT_GBMAP
                        : 0,
                    0));
}

static inline lfs3_compat_t lfs3_fs_rmask(const lfs3_t *lfs3) {
    (void)lfs3;
    return LFS3_COMPAT(
            0xffff,
            0);
}

static inline lfs3_compat_t lfs3_fs_wmask(const lfs3_t *lfs3) {
    (void)lfs3;
    return LFS3_COMPAT(
            0,
            0xffff & ~(
                // we can ignore the gbmap flag if we support both modes
                LFS3_IFYES_GBMAP(0, LFS3_WCOMPAT_GBMAP, 0)));
}

// compat flags on-disk encoding
//
// we need to use the smallest leb128 encoding when writing to disk,
// otherwise detecting overflow would be tricky
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_fromcompat(lfs3_compat_t compat,
        uint8_t buffer[static LFS3_COMPAT_DSIZE]) {
    lfs3_ssize_t d = 0;
    lfs3_ssize_t d_ = lfs3_toleb128(lfs3_compat_rcompat(compat),
            &buffer[d], 1);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    d_ = lfs3_toleb128(lfs3_compat_wcompat(compat),
            &buffer[d], 1);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    return LFS3_DATA_BUF(buffer, d);
}
#endif

static int lfs3_data_readcompat(lfs3_t *lfs3, lfs3_data_t *data,
        uint32_t *compat) {
    // try to read compat flags, not we may:
    // - fail to read flags, but set LFS3_*_OVERFLOW
    // - not read wcompat, but set LFS3_rcompat_OVERFLOW
    //
    // this is ok as long as LFS3_*_OVERFLOW is set in whichever compat
    // flags is the most restricting, we don't need wcompat if we can't
    // even mount rdonly, for example

    // read rcompat flags
    uint32_t rcompat;
    int err = lfs3_data_readleb128(lfs3, data, &rcompat);
    if (err && err != LFS3_ERR_CORRUPT) {
        return err;
    }
    if (err == LFS3_ERR_CORRUPT) {
        rcompat = -1;
    }
    // if we overflowed, set LFS3_*_OVERFLOW and stop parsing
    if (rcompat >= LFS3_rcompat_OVERFLOW) {
        rcompat = LFS3_rcompat_OVERFLOW
                | (rcompat & (LFS3_rcompat_OVERFLOW-1));
        goto done;
    }

    // read wcompat flags
    uint32_t wcompat;
    err = lfs3_data_readleb128(lfs3, data, &wcompat);
    if (err && err != LFS3_ERR_CORRUPT) {
        return err;
    }
    if (err == LFS3_ERR_CORRUPT) {
        wcompat = -1;
    }
    // if we overflowed, set LFS3_*_OVERFLOW and stop parsing
    if (wcompat >= LFS3_wcompat_OVERFLOW) {
        wcompat = LFS3_wcompat_OVERFLOW
                | (wcompat & (LFS3_wcompat_OVERFLOW-1));
        goto done;
    }

done:;
    *compat = LFS3_COMPAT(rcompat, wcompat);
    return 0;
}


// disk geometry things

// geometry on-disk encoding
//
// these are stored minus 1 to avoid overflow issues
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_fromgeometry(const lfs3_geometry_t *geometry,
        uint8_t buffer[static LFS3_GEOMETRY_DSIZE]) {
    lfs3_ssize_t d = 0;
    lfs3_ssize_t d_ = lfs3_toleb128(geometry->block_size-1, &buffer[d], 4);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    d_ = lfs3_toleb128(geometry->block_count-1, &buffer[d], 5);
    if (d_ < 0) {
        LFS3_UNREACHABLE();
    }
    d += d_;

    return LFS3_DATA_BUF(buffer, d);
}
#endif

static int lfs3_data_readgeometry(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_geometry_t *geometry) {
    int err = lfs3_data_readlleb128(lfs3, data, &geometry->block_size);
    if (err) {
        return err;
    }

    err = lfs3_data_readleb128(lfs3, data, &geometry->block_count);
    if (err) {
        return err;
    }

    geometry->block_size += 1;
    geometry->block_count += 1;
    return 0;
}

static int lfs3_mountmroot(lfs3_t *lfs3, const lfs3_mdir_t *mroot) {
    // check the disk version
    uint8_t version[2] = {0, 0};
    lfs3_data_t data;
    lfs3_stag_t tag = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_VERSION,
            &data);
    if (tag < 0 && tag != LFS3_ERR_NOENT) {
        return tag;
    }
    if (tag != LFS3_ERR_NOENT) {
        lfs3_ssize_t d = lfs3_data_read(lfs3, &data, version, 2);
        if (d < 0) {
            return d;
        }
    }

    if (version[0] != LFS3_DISK_VERSION_MAJOR
            || version[1] > LFS3_DISK_VERSION_MINOR) {
        LFS3_ERROR("Incompatible version v%"PRId32".%"PRId32" "
                    "(!= v%"PRId32".%"PRId32")",
                version[0],
                version[1],
                LFS3_DISK_VERSION_MAJOR,
                LFS3_DISK_VERSION_MINOR);
        return LFS3_ERR_NOTSUP;
    }

    // check the on-disk compat flags
    lfs3_compat_t compat = 0;
    tag = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_COMPAT,
            &data);
    if (tag < 0 && tag != LFS3_ERR_NOENT) {
        return tag;
    }
    if (tag != LFS3_ERR_NOENT) {
        int err = lfs3_data_readcompat(lfs3, &data, &compat);
        if (err) {
            return err;
        }
    }

    // check rcompat flags - we must understand these to read the
    // filesystem
    lfs3_compat_t compat_ = lfs3_fs_compat(lfs3);
    lfs3_compat_t rmask_ = lfs3_fs_rmask(lfs3);
    if ((compat & rmask_) != (compat_ & rmask_)) {
        LFS3_ERROR("Incompatible rcompat flags r%"PRIx32" "
                    "(!= r%"PRIx32" & 0x%"PRIx32")",
                lfs3_compat_rcompat(compat),
                lfs3_compat_rcompat(compat_),
                rmask_);
        return LFS3_ERR_NOTSUP;
    }

    // check wcompat flags - we must understand these to write to the
    // filesystem
    if (!lfs3_m_isrdonly(lfs3->flags)) {
        lfs3_compat_t wmask_ = lfs3_fs_wmask(lfs3);
        if ((compat & wmask_) != (compat_ & wmask_)) {
            LFS3_ERROR("Incompatible wcompat flags w%"PRIx32" "
                        "(!= w%"PRIx32" & 0x%"PRIx32")",
                    lfs3_compat_wcompat(compat),
                    lfs3_compat_wcompat(compat_),
                    wmask_);
            return LFS3_ERR_NOTSUP;
        }
    }

    #ifdef LFS3_GBMAP
    // using the gbmap?
    if (lfs3_compat_isgbmap(compat)) {
        lfs3->flags |= LFS3_I_GBMAP;
    }
    #endif

    // check the on-disk geometry
    lfs3_geometry_t geometry;
    tag = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_GEOMETRY,
            &data);
    if (tag < 0) {
        if (tag == LFS3_ERR_NOENT) {
            LFS3_ERROR("No geometry found");
            return LFS3_ERR_INVAL;
        }
        return tag;
    }
    int err = lfs3_data_readgeometry(lfs3, &data, &geometry);
    if (err) {
        return err;
    }

    // either block_size matches or it doesn't, we don't support variable
    // block_sizes
    if (geometry.block_size != lfs3->cfg->block_size) {
        LFS3_ERROR("Incompatible block size %"PRId32" (!= %"PRId32")",
                geometry.block_size,
                lfs3->cfg->block_size);
        return LFS3_ERR_NOTSUP;
    }

    // on-disk block_count must be <= configured block_count
    if (geometry.block_count > lfs3->cfg->block_count) {
        LFS3_ERROR("Incompatible block count %"PRId32" (> %"PRId32")",
                geometry.block_count,
                lfs3->cfg->block_count);
        return LFS3_ERR_NOTSUP;
    }

    lfs3->block_count = geometry.block_count;

    // read the name limit
    lfs3_size_t name_limit = 0xff;
    tag = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_NAMELIMIT,
            &data);
    if (tag < 0 && tag != LFS3_ERR_NOENT) {
        return tag;
    }
    if (tag != LFS3_ERR_NOENT) {
        err = lfs3_data_readleb128(lfs3, &data, &name_limit);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }
        if (err == LFS3_ERR_CORRUPT) {
            name_limit = -1;
        }
    }

    if (name_limit > lfs3->name_limit) {
        LFS3_ERROR("Incompatible name limit %"PRId32" (> %"PRId32")",
                name_limit,
                lfs3->name_limit);
        return LFS3_ERR_NOTSUP;
    }

    lfs3->name_limit = name_limit;

    // read the file limit
    lfs3_off_t file_limit = 0x7fffffff;
    tag = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_FILELIMIT,
            &data);
    if (tag < 0 && tag != LFS3_ERR_NOENT) {
        return tag;
    }
    if (tag != LFS3_ERR_NOENT) {
        err = lfs3_data_readleb128(lfs3, &data, &file_limit);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }
        if (err == LFS3_ERR_CORRUPT) {
            file_limit = -1;
        }
    }

    if (file_limit > lfs3->file_limit) {
        LFS3_ERROR("Incompatible file limit %"PRId32" (> %"PRId32")",
                file_limit,
                lfs3->file_limit);
        return LFS3_ERR_NOTSUP;
    }

    lfs3->file_limit = file_limit;

    return 0;
}

static int lfs3_mountinited(lfs3_t *lfs3) {
    // TODO should these be in lfs3_init?

    // mark mroot as invalid to prevent lfs3_mtree_traverse from getting
    // confused
    lfs3->mroot.mid = -1;
    lfs3->mroot.r.blocks[0] = -1;
    lfs3->mroot.r.blocks[1] = -1;

    // default to no mtree, this is allowed and implies all files are
    // inlined in the mroot
    lfs3_btree_init(&lfs3->mtree);

    // zero gcksum/gdeltas, we'll read these from our mdirs
    lfs3->gcksum = 0;
    lfs3_fs_discardgdelta(lfs3);

    // traverse the mtree rooted at mroot 0x{1,0}
    //
    // we do validate btree inner nodes here, how can we trust our
    // mdirs are valid if we haven't checked the btree inner nodes at
    // least once?
    lfs3_mtrv_t mtrv;
    lfs3_mtrv_init(&mtrv, LFS3_T_RDONLY | LFS3_T_MTREEONLY | LFS3_T_CKMETA);
    while (true) {
        lfs3_bptr_t bptr;
        lfs3_stag_t tag = lfs3_mtree_traverse(lfs3, &mtrv,
                &bptr);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }

        // found an mdir?
        if (tag == LFS3_TAG_MDIR) {
            lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr.d.u.buffer;
            // found an mroot?
            if (mdir->mid <= -1) {
                // check for the magic string, all mroot should have this
                lfs3_data_t data_;
                lfs3_stag_t tag_ = lfs3_mdir_lookup(lfs3, mdir, LFS3_TAG_MAGIC,
                        &data_);
                if (tag_ < 0) {
                    if (tag_ == LFS3_ERR_NOENT) {
                        LFS3_ERROR("No littlefs magic found");
                        return LFS3_ERR_CORRUPT;
                    }
                    return tag_;
                }

                // treat corrupted magic as no magic
                lfs3_scmp_t cmp = lfs3_data_cmp(lfs3, &data_, "littlefs", 8);
                if (cmp < 0) {
                    return cmp;
                }
                if (cmp != LFS3_CMP_EQ) {
                    LFS3_ERROR("No littlefs magic found");
                    return LFS3_ERR_CORRUPT;
                }

                // are we the last mroot?
                tag_ = lfs3_mdir_lookup(lfs3, mdir, LFS3_TAG_MROOT,
                        NULL);
                if (tag_ < 0 && tag_ != LFS3_ERR_NOENT) {
                    return tag_;
                }
                if (tag_ == LFS3_ERR_NOENT) {
                    // track active mroot
                    lfs3_mdir_sync(&lfs3->mroot, mdir);

                    // mount/validate config in active mroot
                    int err = lfs3_mountmroot(lfs3, &lfs3->mroot);
                    if (err) {
                        return err;
                    }
                }
            }

            // build gcksum out of mdir cksums
            lfs3->gcksum ^= mdir->r.cksum;

            // collect any gdeltas from this mdir
            int err = lfs3_fs_consumegdelta(lfs3, mdir);
            if (err) {
                return err;
            }

        // found an mtree inner-node?
        } else if (tag == LFS3_TAG_BRANCH) {
            lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr.d.u.buffer;
            // found the root of the mtree? keep track of this
            if (lfs3->mtree.weight == 0) {
                lfs3->mtree = *rbyd;
            }

        } else {
            LFS3_UNREACHABLE();
        }
    }

    // validate gcksum by comparing its cube against the gcksumdeltas
    //
    // The use of cksum^3 here is important to avoid trivial
    // gcksumdeltas. If we use a linear function (cksum, crc32c(cksum),
    // cksum^2, etc), the state of the filesystem cancels out when
    // calculating a new gcksumdelta:
    //
    //   d_i = t(g') - t(g)
    //   d_i = t(g + c_i) - t(g)
    //   d_i = t(g) + t(c_i) - t(g)
    //   d_i = t(c_i)
    //
    // Using cksum^3 prevents this from happening:
    //
    //   d_i = (g + c_i)^3 - g^3
    //   d_i = (g + c_i)(g + c_i)(g + c_i) - g^3
    //   d_i = (g^2 + gc_i + gc_i + c_i^2)(g + c_i) - g^3
    //   d_i = (g^2 + c_i^2)(g + c_i) - g^3
    //   d_i = g^3 + gc_i^2 + g^2c_i + c_i^3 - g^3
    //   d_i = gc_i^2 + g^2c_i + c_i^3
    //
    // cksum^3 also has some other nice properties, providing a perfect
    // 1->1 mapping of t(g) in 2^31 fields, and losing at most 3-bits of
    // info when calculating d_i.
    //
    if (lfs3_crc32c_cube(lfs3->gcksum) != lfs3->gcksum_d) {
        LFS3_ERROR("Found gcksum mismatch, cksum^3 %08"PRIx32" "
                    "(!= %08"PRIx32")",
                lfs3_crc32c_cube(lfs3->gcksum),
                lfs3->gcksum_d);
        return LFS3_ERR_CORRUPT;
    }

    // keep track of the current gcksum
    #ifndef LFS3_RDONLY
    lfs3->gcksum_p = lfs3->gcksum;
    #endif

    // TODO should the consumegdelta above take gstate/gdelta as a parameter?
    // keep track of the current gstate on disk
    #ifndef LFS3_RDONLY
    lfs3_memcpy(lfs3->grm_p, lfs3->grm_d, LFS3_GRM_DSIZE);
    #ifdef LFS3_GBMAP
    lfs3_memcpy(lfs3->gbmap_p, lfs3->gbmap_d, LFS3_GBMAP_DSIZE);
    #endif
    #endif

    // decode grm so we can report any removed files as missing
    int err = lfs3_data_readgrm(lfs3,
            &LFS3_DATA_BUF(lfs3->grm_d, LFS3_GRM_DSIZE),
            &lfs3->grm);
    if (err) {
        // TODO switch to read-only?
        return err;
    }

    // found pending grms? this should only happen if we lost power
    if (lfs3_grm_count(&lfs3->grm) == 2) {
        LFS3_INFO("Found pending grm %"PRId32".%"PRId32" %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[1]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[1]));
    } else if (lfs3_grm_count(&lfs3->grm) == 1) {
        LFS3_INFO("Found pending grm %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]));
    }

    #ifndef LFS3_RDONLY
    if (LFS3_IFDEF_GBMAP(
            lfs3_f_isgbmap(lfs3->flags),
            false)) {
        #ifdef LFS3_GBMAP
        // decode the global block-map
        err = lfs3_data_readgbmap(lfs3,
                &LFS3_DATA_BUF(lfs3->gbmap_d, LFS3_GBMAP_DSIZE),
                &lfs3->gbmap);
        if (err) {
            // TODO switch to read-only?
            return err;
        }

        // if we have a gbmap, position our lookahead buffer at the last
        // known gbmap window
        lfs3->lookahead.window = lfs3->gbmap.window;

        // update lookahead flag, we only signal our gbmap is
        // repopulatable if known window <= gc_lookgbmap_thresh,
        // unfortunately the way the gbmap feeds itself this rarely
        // includes the entire disk
        lfs3->flags = (lfs3->flags & ~LFS3_I_LOOKAHEAD)
                | ((lfs3_alloc_canlookahead(lfs3)
                        || lfs3_alloc_canlookgbmap(lfs3))
                    ? LFS3_I_LOOKAHEAD
                    : 0);
        #endif

    } else {
        // if we don't have a gbmap, position our lookahead buffer
        // pseudo-randomly using our gcksum as a prng
        //
        // the purpose of this is to avoid bad wear patterns such as always 
        // allocating blocks near the beginning of disk after a powerloss
        //
        lfs3->lookahead.window = lfs3->gcksum % lfs3->block_count;
    }
    #endif

    return 0;
}

// needed in lfs3_mount
static int lfs3_fs_gc_(lfs3_t *lfs3, uint32_t flags);

int lfs3_mount(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_cfg *cfg) {
    #ifdef LFS3_YES_RDONLY
    flags |= LFS3_M_RDONLY;
    #endif
    #ifdef LFS3_YES_FLUSH
    flags |= LFS3_M_FLUSH;
    #endif
    #ifdef LFS3_YES_SYNC
    flags |= LFS3_M_SYNC;
    #endif
    #ifdef LFS3_YES_REVPERTURB
    flags |= LFS3_M_REVPERTURB;
    #endif
    #ifdef LFS3_YES_REVNOISE
    flags |= LFS3_M_REVNOISE;
    #endif
    #ifdef LFS3_YES_CKPROGS
    flags |= LFS3_M_CKPROGS;
    #endif
    #ifdef LFS3_YES_CKFETCHES
    flags |= LFS3_M_CKFETCHES;
    #endif
    #ifdef LFS3_YES_CKMETAPARITY
    flags |= LFS3_M_CKMETAPARITY;
    #endif
    #ifdef LFS3_YES_CKDATACKSUMS
    flags |= LFS3_M_CKDATACKSUMS;
    #endif

    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_M_RDWR)
                | LFS3_M_RDONLY
                | LFS3_M_FLUSH
                | LFS3_M_SYNC
                | LFS3_IFDEF_REVPERTURB(LFS3_M_REVPERTURB, 0)
                | LFS3_IFDEF_REVNOISE(LFS3_M_REVNOISE, 0)
                | LFS3_IFDEF_CKPROGS(LFS3_M_CKPROGS, 0)
                | LFS3_IFDEF_CKFETCHES(LFS3_M_CKFETCHES, 0)
                | LFS3_IFDEF_CKMETAPARITY(LFS3_M_CKMETAPARITY, 0)
                | LFS3_IFDEF_CKDATACKSUMS(LFS3_M_CKDATACKSUMS, 0)
                | LFS3_IFDEF_RDONLY(0, LFS3_M_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_M_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_M_PREERASE, 0))
                | LFS3_IFDEF_RDONLY(0, LFS3_M_COMPACTMETA)
                | LFS3_M_CKMETA
                | LFS3_M_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_M_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_M_REPAIRDATA, 0)))) == 0);
    // these flags require a writable filesystem
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_gc_ismkconsistent(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_gc_islookahead(flags));
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_gc_ispreerase(flags));
    #endif
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_gc_iscompactmeta(flags));
    #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_gc_isrepairmeta(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_gc_isrepairdata(flags));
    #endif
    #endif
    // we can't use preerased blocks without revperturb, so this is
    // likely a mistake
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    LFS3_ASSERT(lfs3_m_isrevperturb(flags) || !lfs3_gc_ispreerase(flags));
    #endif

    int err = lfs3_init(lfs3,
            flags & (
                LFS3_IFDEF_RDONLY(0, LFS3_M_RDWR)
                    | LFS3_M_RDONLY
                    | LFS3_M_FLUSH
                    | LFS3_M_SYNC
                    | LFS3_IFDEF_REVPERTURB(LFS3_M_REVPERTURB, 0)
                    | LFS3_IFDEF_REVNOISE(LFS3_M_REVNOISE, 0)
                    | LFS3_IFDEF_CKPROGS(LFS3_M_CKPROGS, 0)
                    | LFS3_IFDEF_CKFETCHES(LFS3_M_CKFETCHES, 0)
                    | LFS3_IFDEF_CKMETAPARITY(LFS3_M_CKMETAPARITY, 0)
                    | LFS3_IFDEF_CKDATACKSUMS(LFS3_M_CKDATACKSUMS, 0)),
            cfg);
    if (err) {
        return err;
    }

    err = lfs3_mountinited(lfs3);
    if (err) {
        goto failed;
    }

    // run gc if requested
    if (flags & (
            LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                | LFS3_GC_CKMETA
                | LFS3_GC_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0)))) {
        err = lfs3_fs_gc_(lfs3, flags & (
                LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                    | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                    | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                    | LFS3_GC_CKMETA
                    | LFS3_GC_CKDATA
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0))));
        if (err) {
            goto failed;
        }
    }

    // TODO this should use any configured values
    LFS3_INFO("Mounted littlefs v%"PRId32".%"PRId32" %"PRId32"x%"PRId32" "
                "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32".%"PRId32", "
                "cksum %08"PRIx32,
            LFS3_DISK_VERSION_MAJOR,
            LFS3_DISK_VERSION_MINOR,
            lfs3->cfg->block_size,
            lfs3->block_count,
            lfs3->mroot.r.blocks[0],
            lfs3->mroot.r.blocks[1],
            lfs3_rbyd_trunk(&lfs3->mroot.r),
            lfs3->mtree.weight >> lfs3->mbits,
            1 << lfs3->mbits,
            lfs3->gcksum);

    return 0;

failed:;
    // make sure we clean up on error
    lfs3_deinit(lfs3);
    return err;
}

int lfs3_unmount(lfs3_t *lfs3) {
    // all files/dirs should be closed before lfs3_unmount
    LFS3_ASSERT(lfs3->handles == NULL
            // except maybe our gc traversal handle
            || LFS3_IFDEF_GC(
                (lfs3->handles == &lfs3->gc.t.h
                    && lfs3->gc.t.h.next == NULL),
                false));

    return lfs3_deinit(lfs3);
}



/// Format ///

#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
static int lfs3_formatgbmap(lfs3_t *lfs3) {
    // TODO should we try multiple blocks?
    //
    // TODO if we try multiple blocks we should update test_badblocks
    // to test block 3 when gbmap is present
    //
    // assume we can write gbmap to block 2
    lfs3->gbmap.window = 3 % lfs3->block_count;
    lfs3->gbmap.known = lfs3->block_count;
    lfs3->gbmap.b.blocks[0] = 2;
    lfs3->gbmap.b.trunk = 0;
    lfs3->gbmap.b.weight = 0;
    lfs3->gbmap.b.eoff = 0;
    lfs3->gbmap.b.cksum = 0;

    int err = lfs3_bd_erase(lfs3, lfs3->gbmap.b.blocks[0], 0);
    if (err) {
        return err;
    }

    err = lfs3_rbyd_commit(lfs3, &lfs3->gbmap.b, 0, (const lfs3_rattr_t[]){
            // blocks 0..3 - in-use
            LFS3_RATTR(LFS3_TAG_BMINUSE, -2, 0),
            LFS3_RATTR_WEIGHT(+3),
            // blocks 3..block_count - free
            (lfs3->block_count > 3)
                ? LFS3_RATTR(LFS3_TAG_BMFREE, -2, 0)
                : LFS3_RATTR_NOOP(1),
            LFS3_RATTR_WEIGHT(+(lfs3->block_count - 3)),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_formatinited(lfs3_t *lfs3) {
    int err;
    // create an initial gbmap
    #ifdef LFS3_GBMAP
    if (lfs3_f_isgbmap(lfs3->flags)) {
        err = lfs3_formatgbmap(lfs3);
        if (err) {
            return err;
        }

        // go ahead and encode it for writing to disk
        lfs3_data_fromgbmap(&lfs3->gbmap, lfs3->gbmap_d);
    }
    #endif

    for (int i = 0; i < 2; i++) {
        // write superblock to both rbyds in the root mroot to hopefully
        // avoid mounting an older filesystem on disk
        lfs3_rbyd_t rbyd;
        rbyd.blocks[0] = i;
        rbyd.trunk = 0;
        rbyd.weight = 0;
        rbyd.eoff = 0;
        rbyd.cksum = 0;

        err = lfs3_bd_erase(lfs3, rbyd.blocks[0], 0);
        if (err) {
            return err;
        }

        // the initial revision count is arbitrary, but it's nice to have
        // something here to tell the initial mroot apart from btree nodes
        // (rev=0), it's also useful to start with -1 and 0 in the upper
        // bits to help test overflow/sequence comparison
        uint32_t rev = (((uint32_t)i-1) << 28)
                | (((1 << (28-lfs3_smax(lfs3->recycle_bits, 0)))-1)
                    & 0x00216968);
        err = lfs3_rbyd_appendrev(lfs3, &rbyd, rev);
        if (err) {
            return err;
        }

        // our initial superblock contains a couple things:
        // - our magic string, "littlefs"
        // - any format-time configuration
        // - the root's bookmark tag, which reserves did=0 for the root
        //
        err = lfs3_rbyd_appendrattrs(lfs3, &rbyd, -1, -1, -1,
                (const lfs3_rattr_t[]){
                    // magic + various config
                    LFS3_RATTR(LFS3_TAG_MAGIC, 0, 1, LFS3_FROM_LBUF, 8),
                    LFS3_RATTR_ARG("littlefs"),
                    LFS3_RATTR(LFS3_TAG_VERSION, 0, 1, LFS3_FROM_LBUF, 2),
                    LFS3_RATTR_ARG(((const uint8_t[2]){
                        LFS3_DISK_VERSION_MAJOR,
                        LFS3_DISK_VERSION_MINOR})),
                    LFS3_RATTR(LFS3_TAG_COMPAT, 0, 1, LFS3_FROM_COMPAT),
                    LFS3_RATTR_ARG(lfs3_fs_compat(lfs3)),
                    LFS3_RATTR(LFS3_TAG_GEOMETRY, 0, 1, LFS3_FROM_GEOMETRY),
                    LFS3_RATTR_ARG((&(const lfs3_geometry_t){
                        lfs3->cfg->block_size,
                        lfs3->cfg->block_count})),
                    LFS3_RATTR(LFS3_TAG_NAMELIMIT, 0, 1, LFS3_FROM_LLEB128),
                    LFS3_RATTR_ARG(lfs3->name_limit),
                    LFS3_RATTR(LFS3_TAG_FILELIMIT, 0, 1, LFS3_FROM_LEB128),
                    LFS3_RATTR_ARG(lfs3->file_limit),
                    // include on-disk gbmap?
                    #ifdef LFS3_GBMAP
                    (lfs3_f_isgbmap(lfs3->flags))
                        ? LFS3_RATTR(LFS3_TAG_GBMAPDELTA, 0, 1,
                            LFS3_FROM_LBUF,
                            lfs3_memlen(lfs3->gbmap_d, LFS3_GBMAP_DSIZE))
                        : LFS3_RATTR_NOOP(1),
                    LFS3_RATTR_ARG(&lfs3->gbmap_d),
                    #endif
                    // root did=0
                    LFS3_RATTR(LFS3_TAG_BOOKMARK, +1, 1, LFS3_FROM_LEB128),
                    LFS3_RATTR_ARG(0),
                    LFS3_RATTR_NULL});
        if (err) {
            return err;
        }

        // append initial gcksum
        uint32_t cksum = rbyd.cksum;
        err = lfs3_rbyd_appendrattr_(lfs3, &rbyd,
                LFS3_TAG_GCKSUMDELTA, 0,
                LFS3_FROM_LE32, (const lfs3_rattr_t[]){
                    LFS3_RATTR_ARG(lfs3_crc32c_cube(cksum))});
        if (err) {
            return err;
        }

        // and commit
        err = lfs3_rbyd_appendcksum_(lfs3, &rbyd, cksum);
        if (err) {
            return err;
        }
    }

    // sync on-disk state
    err = lfs3_bd_sync(lfs3, 0);
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
int lfs3_format(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_cfg *cfg) {
    #ifdef LFS3_YES_GBMAP
    flags |= LFS3_F_GBMAP;
    #endif
    #ifdef LFS3_YES_REVPERTURB
    flags |= LFS3_F_REVPERTURB;
    #endif
    #ifdef LFS3_YES_REVNOISE
    flags |= LFS3_F_REVNOISE;
    #endif
    #ifdef LFS3_YES_CKPROGS
    flags |= LFS3_F_CKPROGS;
    #endif
    #ifdef LFS3_YES_CKFETCHES
    flags |= LFS3_F_CKFETCHES;
    #endif
    #ifdef LFS3_YES_CKMETAPARITY
    flags |= LFS3_F_CKMETAPARITY;
    #endif
    #ifdef LFS3_YES_CKDATACKSUMS
    flags |= LFS3_F_CKDATACKSUMS;
    #endif

    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_F_RDWR
                | LFS3_IFDEF_GBMAP(LFS3_F_GBMAP, 0)
                | LFS3_IFDEF_REVPERTURB(LFS3_F_REVPERTURB, 0)
                | LFS3_IFDEF_REVNOISE(LFS3_F_REVNOISE, 0)
                | LFS3_IFDEF_CKPROGS(LFS3_F_CKPROGS, 0)
                | LFS3_IFDEF_CKFETCHES(LFS3_F_CKFETCHES, 0)
                | LFS3_IFDEF_CKMETAPARITY(LFS3_F_CKMETAPARITY, 0)
                | LFS3_IFDEF_CKDATACKSUMS(LFS3_F_CKDATACKSUMS, 0)
                | LFS3_F_MKCONSISTENT
                | LFS3_F_LOOKAHEAD
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_F_PREERASE, 0))
                | LFS3_F_COMPACTMETA
                | LFS3_F_CKMETA
                | LFS3_F_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_F_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_F_REPAIRDATA, 0)))) == 0);
    // we can't use preerased blocks without revperturb, so this is
    // likely a mistake
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    LFS3_ASSERT(lfs3_m_isrevperturb(flags) || !lfs3_gc_ispreerase(flags));
    #endif

    int err = lfs3_init(lfs3,
            flags & (
                LFS3_F_RDWR
                    | LFS3_IFDEF_GBMAP(LFS3_F_GBMAP, 0)
                    | LFS3_IFDEF_REVPERTURB(LFS3_F_REVPERTURB, 0)
                    | LFS3_IFDEF_REVNOISE(LFS3_F_REVNOISE, 0)
                    | LFS3_IFDEF_CKPROGS(LFS3_F_CKPROGS, 0)
                    | LFS3_IFDEF_CKFETCHES(LFS3_F_CKFETCHES, 0)
                    | LFS3_IFDEF_CKMETAPARITY(LFS3_F_CKMETAPARITY, 0)
                    | LFS3_IFDEF_CKDATACKSUMS(LFS3_F_CKDATACKSUMS, 0)),
            cfg);
    if (err) {
        return err;
    }

    LFS3_INFO("Formatting littlefs v%"PRId32".%"PRId32" %"PRId32"x%"PRId32,
            LFS3_DISK_VERSION_MAJOR,
            LFS3_DISK_VERSION_MINOR,
            lfs3->cfg->block_size,
            lfs3->block_count);

    err = lfs3_formatinited(lfs3);
    if (err) {
        goto failed;
    }

    // test that mount works with our formatted disk
    err = lfs3_mountinited(lfs3);
    if (err) {
        goto failed;
    }

    // run gc if requested
    if (flags & (
            LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                | LFS3_GC_CKMETA
                | LFS3_GC_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0)))) {
        err = lfs3_fs_gc_(lfs3, flags & (
                LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                    | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                    | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                    | LFS3_GC_CKMETA
                    | LFS3_GC_CKDATA
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0))));
        if (err) {
            goto failed;
        }
    }

    return lfs3_deinit(lfs3);

failed:;
    // make sure we clean up on error
    lfs3_deinit(lfs3);
    return err;
}
#endif



/// Other filesystem things  ///

// note lfs3_fs_stat should never go to disk
int lfs3_fs_stat(lfs3_t *lfs3, struct lfs3_fsinfo *fsinfo) {
    // return various filesystem flags
    fsinfo->flags = (lfs3->flags & (
                LFS3_I_RDONLY
                    | LFS3_I_FLUSH
                    | LFS3_I_SYNC
                    | LFS3_IFDEF_REVPERTURB(LFS3_I_REVPERTURB, 0)
                    | LFS3_IFDEF_REVNOISE(LFS3_I_REVNOISE, 0)
                    | LFS3_IFDEF_CKPROGS(LFS3_I_CKPROGS, 0)
                    | LFS3_IFDEF_CKFETCHES(LFS3_I_CKFETCHES, 0)
                    | LFS3_IFDEF_CKMETAPARITY(LFS3_I_CKMETAPARITY, 0)
                    | LFS3_IFDEF_CKDATACKSUMS(LFS3_I_CKDATACKSUMS, 0)
                    | LFS3_IFDEF_RDONLY(0, LFS3_I_MKCONSISTENT)
                    | LFS3_IFDEF_RDONLY(0, LFS3_I_LOOKAHEAD)
                    | LFS3_IFDEF_RDONLY(0, LFS3_I_COMPACTMETA)
                    | LFS3_I_CKMETA
                    | LFS3_I_CKDATA
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_REPAIR(LFS3_I_REPAIRMETA, 0))
                    | LFS3_IFDEF_RDONLY(0,
                        LFS3_IFDEF_REPAIR(LFS3_I_REPAIRDATA, 0))
                    | LFS3_IFDEF_GBMAP(LFS3_I_GBMAP, 0)))
            // LFS3_I_MKCONSISTENT is a bit of a special case,
            // internally it strictly indicates untracked orphans, but
            // externally it also includes any pending grms
            | LFS3_IFDEF_RDONLY(0,
                (lfs3_grm_count(&lfs3->grm) > 0)
                    ? LFS3_I_MKCONSISTENT
                    : 0)
            // LFS3_I_PREERASE we're just lazy about, since it
            // depends on both gc_preerase_count and preerase vs free
            // known windows
            | LFS3_IFDEF_RDONLY(0,
                LFS3_IFDEF_PREERASE(
                    (lfs3_alloc_canpreerase(lfs3))
                        ? LFS3_I_PREERASE
                        : 0, 0));

    // return filesystem config, this may come from disk
    fsinfo->block_size = lfs3->cfg->block_size;
    fsinfo->block_count = lfs3->block_count;
    fsinfo->name_limit = lfs3->name_limit;
    fsinfo->file_limit = lfs3->file_limit;

    return 0;
}

lfs3_sblock_t lfs3_fs_usage(lfs3_t *lfs3) {
    lfs3_block_t count = 0;
    lfs3_mtrv_t mtrv;
    lfs3_mtrv_init(&mtrv, LFS3_T_RDONLY);
    while (true) {
        lfs3_bptr_t bptr;
        lfs3_stag_t tag = lfs3_mtree_traverse(lfs3, &mtrv,
                &bptr);
        if (tag < 0) {
            if (tag == LFS3_ERR_NOENT) {
                break;
            }
            return tag;
        }

        // count the number of blocks we see, yes this may result in duplicates
        if (tag == LFS3_TAG_MDIR) {
            count += 2;

        } else if (tag == LFS3_TAG_BRANCH) {
            count += 1;

        } else if (tag == LFS3_TAG_BLOCK) {
            count += 1;

        } else {
            LFS3_UNREACHABLE();
        }
    }

    return count;
}

// get the filesystem checksum
int lfs3_fs_cksum(lfs3_t *lfs3, uint32_t *cksum) {
    *cksum = lfs3->gcksum;
    return 0;
}

// blocking filesystem ck/repair functions

int lfs3_fs_ckmeta(lfs3_t *lfs3) {
    return lfs3_fs_gc_(lfs3, LFS3_GC_CKMETA);
}

int lfs3_fs_ckdata(lfs3_t *lfs3) {
    return lfs3_fs_gc_(lfs3, LFS3_GC_CKDATA);
}

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
int lfs3_fs_repairmeta(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

    return lfs3_fs_gc_(lfs3, LFS3_GC_REPAIRMETA);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
int lfs3_fs_repairdata(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

    return lfs3_fs_gc_(lfs3, LFS3_GC_REPAIRDATA);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
int lfs3_fs_ckrepairmeta(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

    return lfs3_fs_gc_(lfs3, LFS3_GC_CKMETA | LFS3_GC_REPAIRMETA);
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
int lfs3_fs_ckrepairdata(lfs3_t *lfs3) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

    return lfs3_fs_gc_(lfs3, LFS3_GC_CKDATA | LFS3_GC_REPAIRDATA);
}
#endif

// incremental filesystem gc
//
// perform any pending janitorial work
#ifdef LFS3_GC
lfs3_sblock_t lfs3_fs_gc(lfs3_t *lfs3) {
    // unknown gc flags?
    LFS3_ASSERT((lfs3->cfg->gc_flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                | LFS3_GC_CKMETA
                | LFS3_GC_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0)))) == 0);
    // these flags require a writable filesystem
    //
    // we don't check this in lfs3_init to avoid cfg headache when
    // mounting rdonly
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_ismkconsistent(lfs3->cfg->gc_flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_islookahead(lfs3->cfg->gc_flags));
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_ispreerase(lfs3->cfg->gc_flags));
    #endif
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_iscompactmeta(lfs3->cfg->gc_flags));
    #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_isrepairmeta(lfs3->cfg->gc_flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_isrepairdata(lfs3->cfg->gc_flags));
    #endif
    // we can't use preerased blocks without revperturb, so this is
    // likely a mistake
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    LFS3_ASSERT(lfs3_m_isrevperturb(lfs3->flags)
            || !lfs3_gc_ispreerase(lfs3->cfg->gc_flags));
    #endif

    // run gc a configurable number of steps
    return lfs3_mgc_gc(lfs3, &lfs3->gc, lfs3->cfg->gc_steps);
}
#endif

// unperform janitorial work
int lfs3_fs_unck(lfs3_t *lfs3, uint32_t flags) {
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                | LFS3_GC_CKMETA
                | LFS3_GC_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0)))) == 0);

    // reset the requested flags
    lfs3->flags |= flags;

    // mark any ongoing traversals as dirty to avoid clearing flags
    // after only half a traversal
    //
    // lfs3_fs_gc will terminate early if it discovers it can no longer
    // make progress
    for (lfs3_handle_t *h = lfs3->handles; h; h = h->next) {
        if (lfs3_o_type(h->flags) == LFS3_type_TRV
                || lfs3_o_type(h->flags) == LFS3_type_GC) {
            h->flags |= LFS3_t_CKPOINTED | LFS3_t_DIRTY;
        }
    }

    return 0;
}


// attempt to grow the filesystem
#ifndef LFS3_RDONLY
int lfs3_fs_grow(lfs3_t *lfs3, lfs3_size_t block_count_) {
    // Note we do _not_ call lfs3_fs_mkconsistent here, or we risk
    // locking up our filesystem trying to fix grms/orphans when we
    // could grow.
    //
    // Ideally we should always be able to recover a stuck filesystem
    // with lfs3_fs_grow.
    //
    // We should be ok not calling lfs3_fs_mkconsistent as long as we
    // don't create/delete mids and patch grms in lfs3_mdir_commit.
    //
    // This may not match user's expectations, but at the same time, we
    // generally want to avoid unnecessary work in functions that can be
    // used to rescue a filesystem.

    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));
    // shrinking the filesystem is not supported
    LFS3_ASSERT(block_count_ >= lfs3->block_count);

    // do nothing if block_count doesn't change
    if (block_count_ == lfs3->block_count) {
        return 0;
    }

    LFS3_INFO("Growing littlefs %"PRId32"x%"PRId32" -> %"PRId32"x%"PRId32,
            lfs3->cfg->block_size, lfs3->block_count,
            lfs3->cfg->block_size, block_count_);

    // keep track of our current block_count in case we fail
    lfs3_size_t block_count = lfs3->block_count;

    // we can use the new blocks immediately as long as the commit
    // with the new block_count is atomic
    lfs3->block_count = block_count_;
    // discard stale lookahead buffer/gbmap
    lfs3_alloc_discard(lfs3);
    int err;

    // checkpoint the lookahead buffer, but _not_ the gbmap, we
    // can't repopulate the gbmap until we've resized it
    lfs3_alloc_ckpoint_(lfs3);

    // grow the gbmap if we have one
    //
    // note this won't actually be committed to disk until mdir commit
    #ifdef LFS3_GBMAP
    if (lfs3_f_isgbmap(lfs3->flags)) {
        // if the last range is free, we can extend it, otherwise we
        // need a new range
        lfs3_stag_t tag = lfs3_gbmap_lookupnext(lfs3, &lfs3->gbmap.b,
                block_count-1,
                NULL, NULL, NULL);
        if (tag < 0) {
            LFS3_ASSERT(tag != LFS3_ERR_NOENT);
            err = tag;
            goto failed;
        }

        // we don't need a copy because this is atomic, and mdir commit
        // reverts to the on-disk state if it fails
        err = lfs3_gbmap_commit(lfs3, &lfs3->gbmap.b,
                (tag == LFS3_TAG_BMFREE) ? block_count-1 : block_count,
                (const lfs3_rattr_t[]){
                    (tag == LFS3_TAG_BMFREE)
                        ? LFS3_RATTR(LFS3_tag_GROW, -2, 0)
                        : LFS3_RATTR(LFS3_TAG_BMFREE, -2, 0),
                    LFS3_RATTR_WEIGHT(+(block_count_ - block_count)),
                    LFS3_RATTR_NULL});
        if (err) {
            goto failed;
        }
    }
    #endif

    // update our on-disk config
    err = lfs3_mdir_commit(lfs3, &lfs3->mroot, (const lfs3_rattr_t[]){
            LFS3_RATTR(LFS3_TAG_GEOMETRY, 0, 1, LFS3_FROM_GEOMETRY),
            LFS3_RATTR_ARG((&(const lfs3_geometry_t){
                lfs3->cfg->block_size,
                block_count_})),
            LFS3_RATTR_NULL});
    if (err) {
        goto failed;
    }

    return 0;

failed:;
    // restore block_count
    lfs3->block_count = block_count;
    // discard clobbered lookahead buffer
    lfs3_alloc_discard(lfs3);
    // revert to the previous gbmap
    #ifdef LFS3_GBMAP
    if (lfs3_f_isgbmap(lfs3->flags)) {
        lfs3->gbmap.b = lfs3->gbmap.b_p;
    }
    #endif

    return err;
}
#endif

// enable the global on-disk block-map
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP) && !defined(LFS3_YES_GBMAP)
int lfs3_fs_mkgbmap(lfs3_t *lfs3) {
    // Note we do _not_ call lfs3_fs_mkconsistent here.
    //
    // We should be ok not calling lfs3_fs_mkconsistent as long as we
    // don't create/delete mids and patch grms in lfs3_mdir_commit.
    //
    // This may not match user's expectations, but at the same time, we
    // generally want to avoid unnecessary work in functions that can be
    // used to rescue a filesystem.

    // error if we already have a gbmap
    if (lfs3_f_isgbmap(lfs3->flags)) {
        return LFS3_ERR_EXIST;
    }

    // checkpoint the lookahead buffer, the gbmap doesn't exist yet
    lfs3_alloc_ckpoint_(lfs3);

    // create an empty gbmap, let lfs3_alloc_ckpoint populate it
    lfs3_gbmap_init(&lfs3->gbmap);

    int err = lfs3_gbmap_commit(lfs3, &lfs3->gbmap.b,
            0, (const lfs3_rattr_t[]){
                LFS3_RATTR(LFS3_TAG_BMFREE, -2, 0),
                LFS3_RATTR_WEIGHT(+lfs3->block_count),
                LFS3_RATTR_NULL});
    if (err) {
        goto failed;
    }

    // go ahead and mark gbmap as in-use internally
    lfs3->flags |= LFS3_F_GBMAP;

    // sync gbmap/lookahead windows, note this needs to happen after any
    // block allocation in lfs3_gbmap_commit
    lfs3->gbmap.window = lfs3->lookahead.window;

    // checkpoint the allocator again, this should trigger a
    // repopulation scan
    err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        goto failed;
    }

    // mark the gbmap as in-use on-disk while atomically committing the
    // gbmap into gstate
    lfs3_compat_t compat_ = lfs3_fs_compat(lfs3);
    compat_ |= LFS3_COMPAT(0, LFS3_WCOMPAT_GBMAP);

    err = lfs3_mdir_commit(lfs3, &lfs3->mroot, (const lfs3_rattr_t[]){
            LFS3_RATTR(LFS3_TAG_COMPAT, 0, 1, LFS3_FROM_COMPAT),
            LFS3_RATTR_ARG(compat_),
            LFS3_RATTR_NULL});
    if (err) {
        goto failed;
    }

    return 0;

failed:;
    // if we failed clear the gbmap bit and reset the gbmap to be safe
    lfs3->flags &= ~LFS3_F_GBMAP;
    lfs3_gbmap_init(&lfs3->gbmap);
    return err;
}
#endif

// disable the global on-disk block-map
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP) && !defined(LFS3_YES_GBMAP)
int lfs3_fs_rmgbmap(lfs3_t *lfs3) {
    // Note we do _not_ call lfs3_fs_mkconsistent here.
    //
    // We should be ok not calling lfs3_fs_mkconsistent as long as we
    // don't create/delete mids and patch grms in lfs3_mdir_commit.
    //
    // This may not match user's expectations, but at the same time, we
    // generally want to avoid unnecessary work in functions that can be
    // used to rescue a filesystem.

    // error if we already don't have a gbmap
    if (!lfs3_f_isgbmap(lfs3->flags)) {
        return LFS3_ERR_NOENT;
    }

    // checkpoint the allocator
    int err = lfs3_alloc_ckpoint(lfs3);
    if (err) {
        return err;
    }

    // removing the gbmap is relatively easy, we just need to mark the
    // gbmap as not in use
    //
    // this leaves garbage gdeltas around, but these should be cleaned
    // up implicitly as mdirs are compacted
    lfs3_compat_t compat_ = lfs3_fs_compat(lfs3);
    compat_ &= ~LFS3_COMPAT(0, LFS3_WCOMPAT_GBMAP);

    err = lfs3_mdir_commit(lfs3, &lfs3->mroot, (const lfs3_rattr_t[]){
            LFS3_RATTR(LFS3_TAG_COMPAT, 0, 1, LFS3_FROM_COMPAT),
            LFS3_RATTR_ARG(compat_),
            LFS3_RATTR_NULL});
    if (err) {
        return err;
    }

    // on success mark gbmap as not-in-use internally
    lfs3->flags &= ~LFS3_F_GBMAP;
    return 0;
}
#endif


// mark a block as bad, and/or evict from the filesystem
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
int lfs3_fs_mkbad(lfs3_t *lfs3, lfs3_block_t block, uint32_t flags) {
    // Note we do _not_ call lfs3_fs_mkconsistent here.
    //
    // We should be ok not calling lfs3_fs_mkconsistent as long as we
    // don't create/delete mids and patch grms in lfs3_mdir_commit.
    //
    // This may not match user's expectations, but at the same time, we
    // generally want to avoid unnecessary work in functions that can be
    // used to rescue a filesystem.

    // unknown mkbad flags?
    LFS3_ASSERT((flags & ~(
            LFS3_MKBAD_EVICT)) == 0);

    // out-of-bounds?
    if (block >= lfs3->block_count) {
        return LFS3_ERR_NOENT;
    }

    // evict?
    int err;
    if (lfs3_mkbad_isevict(flags)) {
        // littlefs can't function without blocks 0x{0,1}, so reject
        // these
        if (block == 0 || block == 1) {
            return LFS3_ERR_BUSY;
        }
        // block eviction needs an evict queue with at least one entry
        LFS3_ASSERT(lfs3->cfg->evictqueue_count >= 1);

        // repair any known damage first, we need the evict queue
        #ifdef LFS3_REPAIR
        int err = lfs3_fs_mkrepaired(lfs3);
        if (err) {
            return err;
        }
        #endif
        // eviction queue should be empty now
        LFS3_ASSERT(lfs3->evictqueue.count == 0);

        // put our block on the evict queue, this sidechannel tells the
        // rest of the filesystem what blocks to avoid
        lfs3_evict_push(lfs3, block, LFS3_EVICT_DATA);

        // run gc to evict the block
        //
        // the allocator is also aware of the evict window, so we can
        // always do this in one pass
        lfs3_mgc_t mgc;
        lfs3_mgc_init(&mgc, LFS3_gc_EVICTMETA | LFS3_gc_EVICTDATA);
        lfs3_handle_open(lfs3, &mgc.t.h);
        lfs3_sblock_t steps = lfs3_mgc_gc(lfs3, &mgc, -1);
        if (steps < 0) {
            lfs3_handle_close(lfs3, &mgc.t.h);
            return steps;
        }
        lfs3_handle_close(lfs3, &mgc.t.h);
    }

    return 0;

failed:;
    // make sure eviction queue is null
    lfs3_evict_discard(lfs3);
    return err;
}
#endif



/// High-level filesystem traversal ///

int lfs3_trv_open(lfs3_t *lfs3, lfs3_trv_t *trv, uint32_t flags) {
    // already open?
    LFS3_ASSERT(!lfs3_handle_isopen(lfs3, &trv->t.h));
    // only rdonly is allowed here
    LFS3_ASSERT(lfs3_o_mode(flags) == LFS3_T_RDONLY);
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_T_RDONLY
                | LFS3_T_MTREEONLY
                | LFS3_T_EXCL
                | LFS3_T_CKMETA
                | LFS3_T_CKDATA)) == 0);
    // some flags don't make sense when only traversing the mtree
    LFS3_ASSERT(!lfs3_t_ismtreeonly(flags) || !lfs3_t_isckdata(flags));

    // setup traversal state
    lfs3_mtrv_init(&trv->t, flags | LFS3_t_STALE);

    // add to tracked mdirs
    lfs3_handle_open(lfs3, &trv->t.h);
    return 0;
}

int lfs3_trv_close(lfs3_t *lfs3, lfs3_trv_t *trv) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &trv->t.h));

    // remove from tracked mdirs
    lfs3_handle_close(lfs3, &trv->t.h);
    return 0;
}

int lfs3_trv_read(lfs3_t *lfs3, lfs3_trv_t *trv,
        struct lfs3_tinfo *tinfo) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &trv->t.h));

    // filesystem modified? excl? terminate early
    if (lfs3_t_isexcl(trv->t.h.flags)
            && lfs3_t_isdirty(trv->t.h.flags)) {
        return LFS3_ERR_BUSY;
    }

    // discard current block queue?
    if (lfs3_t_isstale(trv->t.h.flags)) {
        trv->blocks[0] = -1;
        trv->blocks[1] = -1;
        trv->t.h.flags &= ~LFS3_t_STALE;
    }

    while (true) {
        // some redund blocks left over?
        if (trv->blocks[0] != -1) {
            // write our traversal info
            tinfo->btype = lfs3_t_btype(trv->t.h.flags);
            tinfo->block = trv->blocks[0];

            trv->blocks[0] = trv->blocks[1];
            trv->blocks[1] = -1;
            return 0;
        }

        // find next block
        lfs3_bptr_t bptr;
        lfs3_stag_t tag = lfs3_mtree_traverse(lfs3, &trv->t,
                &bptr);
        if (tag < 0) {
            return tag;
        }

        // ignore new stale flags
        trv->t.h.flags &= ~LFS3_t_STALE;

        // figure out type/blocks
        if (tag == LFS3_TAG_MDIR) {
            lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr.d.u.buffer;
            lfs3_t_setbtype(&trv->t.h.flags, LFS3_BTYPE_MDIR);
            trv->blocks[0] = mdir->r.blocks[0];
            trv->blocks[1] = mdir->r.blocks[1];

        } else if (tag == LFS3_TAG_BRANCH) {
            lfs3_t_setbtype(&trv->t.h.flags, LFS3_BTYPE_BTREE);
            lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr.d.u.buffer;
            trv->blocks[0] = rbyd->blocks[0];
            trv->blocks[1] = -1;

        } else if (tag == LFS3_TAG_BLOCK) {
            lfs3_t_setbtype(&trv->t.h.flags, LFS3_BTYPE_DATA);
            trv->blocks[0] = lfs3_bptr_block(&bptr);
            trv->blocks[1] = -1;

        } else {
            LFS3_UNREACHABLE();
        }
    }
}

int lfs3_trv_rewind(lfs3_t *lfs3, lfs3_trv_t *trv) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &trv->t.h));
    // reset traversal
    lfs3_mtrv_init(&trv->t,
            (trv->t.h.flags & ~(
                    LFS3_t_DIRTY
                        | LFS3_t_CKPOINTED
                        | LFS3_t_DAMAGED))
                | LFS3_t_STALE);
    return 0;
}


/// High-level filesystem gc ///

int lfs3_gc_open(lfs3_t *lfs3, lfs3_gc_t *gc, uint32_t flags) {
    // already open?
    LFS3_ASSERT(!lfs3_handle_isopen(lfs3, &gc->gc.t.h));
    // only wronly is allowed here
    LFS3_ASSERT(lfs3_o_mode(flags) == LFS3_GC_WRONLY);
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_GC_WRONLY
                | LFS3_GC_EXCL
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0))
                | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA)
                | LFS3_GC_CKMETA
                | LFS3_GC_CKDATA
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRMETA, 0))
                | LFS3_IFDEF_RDONLY(0,
                    LFS3_IFDEF_REPAIR(LFS3_GC_REPAIRDATA, 0)))) == 0);
    // these flags require a writable filesystem
    #ifndef LFS3_RDONLY
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_ismkconsistent(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_islookahead(flags));
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_ispreerase(flags));
    #endif
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_iscompactmeta(flags));
    #if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_isrepairmeta(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags)
            || !lfs3_gc_isrepairdata(flags));
    #endif
    #endif
    // we can't use preerased blocks without revperturb, so this is
    // likely a mistake
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    LFS3_ASSERT(lfs3_m_isrevperturb(lfs3->flags)
            || !lfs3_gc_ispreerase(flags));
    #endif

    // setup gc state
    lfs3_mgc_init(&gc->gc, flags);

    // add to tracked mdirs
    lfs3_handle_open(lfs3, &gc->gc.t.h);
    return 0;
}

int lfs3_gc_close(lfs3_t *lfs3, lfs3_gc_t *gc) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &gc->gc.t.h));

    // remove from tracked mdirs
    lfs3_handle_close(lfs3, &gc->gc.t.h);
    return 0;
}

lfs3_sblock_t lfs3_gc_write(lfs3_t *lfs3, lfs3_gc_t *gc,
        lfs3_sblock_t steps) {
    LFS3_ASSERT(lfs3_handle_isopen(lfs3, &gc->gc.t.h));

    // filesystem modified? excl? terminate early
    if (lfs3_t_isexcl(gc->gc.t.h.flags)
            && lfs3_t_isdirty(gc->gc.t.h.flags)) {
        return LFS3_ERR_BUSY;
    }

    // run gc
    return lfs3_mgc_gc(lfs3, &gc->gc, steps);
}



/*
 *                   ####
 *                ######## #####    #####
 *              ################# ########  ####
 *        #### ### ############################## ####
 *      #############-#### ##_#########################
 *     ########'#########_\'|######_####################
 *      ##### ##########_.. \ .'#_._#### #/#_##########
 *    ################_    \ v .'_____#_.'.'_ ###########
 *   ################_ "'. |  .""  _________ '"-##########
 *   ########"#####..--.  /    .-"'|  | |   '"-####'######
 *     #############    \     /  .---.|_|_    ###########
 *      ###########     |    /  -|        |-   ########
 *                      )    |  -|littlefs|-
 *                      |    |  -|   v3   |-
 *                      |    |   '--------'
 *                      |   ||     ' ' '
 *        -------------/-/---\\----------------------
 */

