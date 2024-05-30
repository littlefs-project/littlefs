/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs.h"
#include "lfs_util.h"


//// TODO do we still need these?
//// some constants used throughout the code
//#define LFS_BLOCK_NULL ((lfs_block_t)-1)
//#define LFS_BLOCK_INLINE ((lfs_block_t)-2)

// TODO do we still need these?
enum {
    LFS_OK_RELOCATED = 1,
    LFS_OK_DROPPED   = 2,
    LFS_OK_ORPHANED  = 3,
};

// internally used disk-comparison enum
//
// note LT < EQ < GT
enum lfs_scmp {
    LFS_CMP_LT = 0, // disk < query
    LFS_CMP_EQ = 1, // disk = query
    LFS_CMP_GT = 2, // disk > query
};

typedef int lfs_scmp_t;


/// Simple bd wrappers (asserts go here) ///

static int lfsr_bd_read__(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        void *buffer, lfs_size_t size) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);
    // must be aligned
    LFS_ASSERT(off % lfs->cfg->read_size == 0);
    LFS_ASSERT(size % lfs->cfg->read_size == 0);

    // bd read
    int err = lfs->cfg->read(lfs->cfg, block, off, buffer, size);
    LFS_ASSERT(err <= 0);
    if (err) {
        LFS_DEBUG("Bad read 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                block, off, size, err);
        return err;
    }

    return 0;
}

static int lfsr_bd_prog__(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);
    // must be aligned
    LFS_ASSERT(off % lfs->cfg->prog_size == 0);
    LFS_ASSERT(size % lfs->cfg->prog_size == 0);

    // bd prog
    int err = lfs->cfg->prog(lfs->cfg, block, off, buffer, size);
    LFS_ASSERT(err <= 0);
    if (err) {
        LFS_DEBUG("Bad prog 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                block, off, size, err);
        return err;
    }

    return 0;
}

static int lfsr_bd_erase__(lfs_t *lfs, lfs_block_t block) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);

    // bd erase
    int err = lfs->cfg->erase(lfs->cfg, block);
    LFS_ASSERT(err <= 0);
    if (err) {
        LFS_DEBUG("Bad erase 0x%"PRIx32" (%d)",
                block, err);
        return err;
    }

    return 0;
}

static int lfsr_bd_sync__(lfs_t *lfs) {
    // bd sync
    int err = lfs->cfg->sync(lfs->cfg);
    LFS_ASSERT(err <= 0);
    if (err) {
        LFS_DEBUG("Bad sync (%d)", err);
        return err;
    }

    return 0;
}


/// Caching block device operations ///

static inline void lfsr_bd_droprcache(lfs_t *lfs) {
    lfs->rcache.size = 0;
}

static inline void lfsr_bd_droppcache(lfs_t *lfs) {
    lfs->pcache.size = 0;
}

// caching read that lends you a buffer
//
// note hint has two conveniences:
//  0 => minimal caching
// -1 => maximal caching
static int lfsr_bd_readnext(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfs_size_t size,
        const uint8_t **buffer_, lfs_size_t *size_) {
    // check for in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_size_t hint_ = lfs_max(hint, size); // make sure hint >= size
    while (true) {
        lfs_size_t d = hint_;

        // already in pcache?
        if (block == lfs->pcache.block
                && off < lfs->pcache.off + lfs->pcache.size) {
            if (off >= lfs->pcache.off) {
                *buffer_ = &lfs->pcache.buffer[off-lfs->pcache.off];
                *size_ = lfs_min(
                        lfs_min(size, d),
                        lfs->pcache.size - (off-lfs->pcache.off));
                return 0;
            }

            // pcache takes priority
            d = lfs_min(d, lfs->pcache.off - off);
        }

        // already in rcache?
        if (block == lfs->rcache.block
                && off < lfs->rcache.off + lfs->rcache.size
                && off >= lfs->rcache.off) {
            *buffer_ = &lfs->rcache.buffer[off-lfs->rcache.off];
            *size_ = lfs_min(
                    lfs_min(size, d),
                    lfs->rcache.size - (off-lfs->rcache.off));
            return 0;
        }

        // drop rcache in case read fails
        lfsr_bd_droprcache(lfs);

        // load into rcache, above conditions can no longer fail
        //
        // note it's ok if we overlap the pcache a bit, pcache always
        // takes priority until flush, which updates the rcache
        lfs_size_t off__ = lfs_aligndown(off, lfs->cfg->read_size);
        lfs_size_t size__ = lfs_alignup(
                lfs_min(
                    // watch out for overflow when hint_=-1!
                    (off-off__) + lfs_min(
                        lfs_min(hint_, d),
                        lfs->cfg->block_size - off),
                    lfs->cfg->rcache_size),
                lfs->cfg->read_size);
        int err = lfsr_bd_read__(lfs, block, off__,
                lfs->rcache.buffer, size__);
        if (err) {
            return err;
        }

        lfs->rcache.block = block;
        lfs->rcache.off = off__;
        lfs->rcache.size = size__;
    }
}

// caching read
//
// note hint has two conveniences:
//  0 => minimal caching
// -1 => maximal caching
static int lfsr_bd_read(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        void *buffer, lfs_size_t size) {
    // check for in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_size_t off_ = off;
    lfs_size_t hint_ = lfs_max(hint, size); // make sure hint >= size
    uint8_t *buffer_ = buffer;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        lfs_size_t d = size_;

        // already in pcache?
        if (block == lfs->pcache.block
                && off_ < lfs->pcache.off + lfs->pcache.size) {
            if (off_ >= lfs->pcache.off) {
                const uint8_t *buffer__;
                lfs_size_t size__;
                int err = lfsr_bd_readnext(lfs, block, off_, hint_, d,
                        &buffer__, &size__);
                if (err) {
                    return err;
                }

                lfs_memcpy(buffer_, buffer__, size__);

                off_ += size__;
                hint_ -= size__;
                buffer_ += size__;
                size_ -= size__;
                continue;
            }

            // pcache takes priority
            d = lfs_min(d, lfs->pcache.off - off_);
        }

        // already in rcache?
        if (block == lfs->rcache.block
                && off_ < lfs->rcache.off + lfs->rcache.size) {
            if (off_ >= lfs->rcache.off) {
                const uint8_t *buffer__;
                lfs_size_t size__;
                int err = lfsr_bd_readnext(lfs, block, off_, hint_, d,
                        &buffer__, &size__);
                if (err) {
                    return err;
                }

                lfs_memcpy(buffer_, buffer__, size__);

                off_ += size__;
                hint_ -= size__;
                buffer_ += size__;
                size_ -= size__;
                continue;
            }

            // rcache takes priority
            d = lfs_min(d, lfs->rcache.off - off_);
        }

        // bypass rcache?
        if (off_ % lfs->cfg->read_size == 0
                && d >= lfs_min(hint_, lfs->cfg->rcache_size)
                && d >= lfs->cfg->read_size) {
            d = lfs_aligndown(d, lfs->cfg->read_size);
            int err = lfsr_bd_read__(lfs, block, off_, buffer_, d);
            if (err) {
                return err;
            }

            off_ += d;
            hint_ -= d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // read into rcache, above conditions can no longer fail
        //
        // don't use d here! rcache is going to be dropped
        const uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_readnext(lfs, block, off_, hint_, size_,
                &buffer__, &size__);
        if (err) {
            return err;
        }
    }

    return 0;
}

// needed in lfsr_bd_prog_ for prog validation
static lfs_scmp_t lfsr_bd_cmp(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        const void *buffer, lfs_size_t size);

// low-level prog stuff
static int lfsr_bd_prog_(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *cksum_) {
    // prog to disk
    int err = lfsr_bd_prog__(lfs, block, off, buffer, size);
    if (err) {
        return err;
    }

    // check progs?
    if (lfs->cfg->check_progs) {
        // pcache should have been dropped at this point
        LFS_ASSERT(lfs->pcache.size == 0);

        // invalidate rcache, we're going to clobber it anyways
        lfsr_bd_droprcache(lfs);

        lfs_scmp_t cmp = lfsr_bd_cmp(lfs, block, off, 0,
                buffer, size);
        if (cmp < 0) {
            return cmp;
        }

        if (cmp != LFS_CMP_EQ) {
            LFS_DEBUG("Bad prog 0x%"PRIx32".%"PRIx32" %"PRIu32" (checked)",
                    block, off, size);
            return LFS_ERR_CORRUPT;
        }
    }

    // update rcache if we can
    if (block == lfs->rcache.block
            && off <= lfs->rcache.off + lfs->rcache.size) {
        lfs->rcache.off = lfs_min(off, lfs->rcache.off);
        lfs->rcache.size = lfs_min(
                (off-lfs->rcache.off) + size,
                lfs->cfg->rcache_size);
        lfs_memcpy(
                &lfs->rcache.buffer[off-lfs->rcache.off],
                buffer,
                lfs->rcache.size - (off-lfs->rcache.off));
    }

    // keep track of the last flushed cksum
    if (cksum_) {
        lfs->pcksum = *cksum_;
    }

    return 0;
}

// flush the pcache
static int lfsr_bd_flush(lfs_t *lfs, uint32_t *cksum_) {
    if (lfs->pcache.size != 0) {
        // must be in-bounds
        LFS_ASSERT(lfs->pcache.block < lfs->cfg->block_count);
        // must be aligned
        LFS_ASSERT(lfs->pcache.off % lfs->cfg->prog_size == 0);
        lfs_size_t size = lfs_alignup(lfs->pcache.size, lfs->cfg->prog_size);

        // make this cache available, if we error anything in this cache
        // would be useless anyways
        lfsr_bd_droppcache(lfs);

        // flush
        int err = lfsr_bd_prog_(lfs, lfs->pcache.block,
                lfs->pcache.off, lfs->pcache.buffer, size,
                cksum_);
        if (err) {
            return err;
        }
    }

    return 0;
}

// caching prog that lends you a buffer
//
// with optional checksum
static int lfsr_bd_prognext(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        lfs_size_t size,
        uint8_t **buffer_, lfs_size_t *size_,
        uint32_t *cksum_) {
    // check for in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    while (true) {
        // active pcache?
        if (lfs->pcache.block == block
                && lfs->pcache.size != 0) {
            // fits in pcache?
            if (off < lfs->pcache.off + lfs->cfg->pcache_size) {
                // you can't prog backwards silly
                LFS_ASSERT(off >= lfs->pcache.off);

                // expand the pcache?
                lfs->pcache.size = lfs_min(
                        (off-lfs->pcache.off) + size,
                        lfs->cfg->pcache_size);

                *buffer_ = &lfs->pcache.buffer[off-lfs->pcache.off];
                *size_ = lfs_min(
                        size,
                        lfs->pcache.size - (off-lfs->pcache.off));
                return 0;
            }

            // flush pcache?
            int err = lfsr_bd_flush(lfs, cksum_);
            if (err) {
                return err;
            }
        }

        // move the pcache, above conditions can no longer fail
        lfs->pcache.block = block;
        lfs->pcache.off = lfs_aligndown(off, lfs->cfg->prog_size);
        lfs->pcache.size = lfs_min(
                (off-lfs->pcache.off) + size,
                lfs->cfg->pcache_size);

        // zero to avoid any information leaks
        lfs_memset(lfs->pcache.buffer, 0xff, lfs->cfg->pcache_size);

        // discard any overlapping rcache
        if (block == lfs->rcache.block
                && off < lfs->rcache.off + lfs->rcache.size) {
            lfs->rcache.size = lfs_max(off, lfs->rcache.off) - lfs->rcache.off;
        }
    }
}

// caching prog
//
// with optional checksum
static int lfsr_bd_prog(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *cksum_) {
    // check for in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_size_t off_ = off;
    const uint8_t *buffer_ = buffer;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        // fits in pcache?
        if (block == lfs->pcache.block
                && off_ < lfs->pcache.off + lfs->cfg->pcache_size
                && lfs->pcache.size != 0) {
            // you can't prog backwards silly
            LFS_ASSERT(off_ >= lfs->pcache.off);

            uint8_t *buffer__;
            lfs_size_t size__;
            int err = lfsr_bd_prognext(lfs, block, off_, size_,
                    &buffer__, &size__,
                    cksum_);
            if (err) {
                return err;
            }

            lfs_memcpy(buffer__, buffer_, size__);

            off_ += size__;
            buffer_ += size__;
            size_ -= size__;
            continue;
        }

        // bypass pcache?
        if (off_ % lfs->cfg->prog_size == 0
                && size_ >= lfs->cfg->pcache_size) {
            // flush our pcache first, some devices don't support
            // out-of-order progs in a block
            int err = lfsr_bd_flush(lfs, cksum_);
            if (err) {
                return err;
            }

            lfs_size_t d = lfs_aligndown(size_, lfs->cfg->prog_size);
            err = lfsr_bd_prog_(lfs, block, off_, buffer_, d,
                    cksum_);
            if (err) {
                return err;
            }

            off_ += d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // flush pcache, above conditions can no longer fail
        uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_prognext(lfs, block, off_, size_,
                &buffer__, &size__,
                cksum_);
        if (err) {
            return err;
        }
    }

    // optional checksum
    if (cksum_) {
        *cksum_ = lfs_crc32c(*cksum_, buffer, size);
    }

    return 0;
}

// unprog can undo a pending prog as long as it's still in our pcache
//
// this is useful for aligning progs retroactively
static int lfsr_bd_unprog(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        lfs_size_t size,
        uint32_t *cksum_) {
    // we don't really use these, but they should match the pcache, the
    // compiler should optimize these away anyways
    LFS_ASSERT(block == lfs->pcache.block);
    LFS_ASSERT(off == lfs->pcache.off + lfs->pcache.size);
    // we can't unprog flushed progs
    LFS_ASSERT(lfs->pcache.size >= size);

    // unprog our prog
    lfs->pcache.size -= size;

    if (cksum_) {
        // recalculate cksum from the last flush
        *cksum_ = lfs_crc32c(
                // no flush yet?
                (lfs->pcache.off == 0) ? 0 : lfs->pcksum,
                lfs->pcache.buffer, lfs->pcache.size);
    }

    return 0;
}

static int lfsr_bd_sync(lfs_t *lfs) {
    // make sure we flush any caches
    int err = lfsr_bd_flush(lfs, NULL);
    if (err) {
        return err;
    }

    return lfsr_bd_sync__(lfs);
}

static int lfsr_bd_erase(lfs_t *lfs, lfs_block_t block) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);

    // invalidate any relevant caches
    if (lfs->pcache.block == block) {
        lfsr_bd_droppcache(lfs);
    }
    if (lfs->rcache.block == block) {
        lfsr_bd_droprcache(lfs);
    }

    return lfsr_bd_erase__(lfs, block);
}


// other block device utils

static int lfsr_bd_cksum(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfs_size_t size,
        uint32_t *cksum_) {
    // check for in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_size_t off_ = off;
    lfs_size_t hint_ = lfs_max(hint, size); // make sure hint >= size
    lfs_size_t size_ = size;
    while (size_ > 0) {
        const uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_readnext(lfs, block, off_, hint_, size_,
                &buffer__, &size__);
        if (err) {
            return err;
        }

        *cksum_ = lfs_crc32c(*cksum_, buffer__, size__);

        off_ += size__;
        hint_ -= size__;
        size_ -= size__;
    }

    return 0;
}

static lfs_scmp_t lfsr_bd_cmp(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint, 
        const void *buffer, lfs_size_t size) {
    // check for in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_size_t off_ = off;
    lfs_size_t hint_ = lfs_max(hint, size); // make sure hint >= size
    const uint8_t *buffer_ = buffer;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        const uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_readnext(lfs, block, off_, hint_, size_,
                &buffer__, &size__);
        if (err) {
            return err;
        }

        int res = lfs_memcmp(buffer__, buffer_, size__);
        if (res != 0) {
            return (res < 0) ? LFS_CMP_LT : LFS_CMP_GT;
        }

        off_ += size__;
        hint_ -= size__;
        buffer_ += size__;
        size_ -= size__;
    }

    return LFS_CMP_EQ;
}

static int lfsr_bd_cpy(lfs_t *lfs,
        lfs_block_t dst_block, lfs_size_t dst_off,
        lfs_block_t src_block, lfs_size_t src_off, lfs_size_t hint,
        lfs_size_t size,
        uint32_t *cksum_) {
    // we don't really use hint here because we go through our pcache
    (void)hint;

    // check for in-bounds
    LFS_ASSERT(dst_block < lfs->cfg->block_count);
    if (dst_off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }
    LFS_ASSERT(src_block < lfs->cfg->block_count);
    if (src_off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_size_t dst_off_ = dst_off;
    lfs_size_t src_off_ = src_off;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        // prefer the pcache here to avoid rcache conflicts with prog
        // validation, if we're lucky we might even be able to avoid
        // clobbering the rcache at all
        uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_prognext(lfs, dst_block, dst_off_, size_,
                &buffer__, &size__,
                cksum_);
        if (err) {
            return err;
        }

        err = lfsr_bd_read(lfs, src_block, src_off_, 0,
                buffer__, size__);
        if (err) {
            return err;
        }

        // optional checksum
        if (cksum_) {
            *cksum_ = lfs_crc32c(*cksum_, buffer__, size__);
        }

        dst_off_ += size__;
        src_off_ += size__;
        size_ -= size__;
    }

    return 0;
}

static int lfsr_bd_set(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        uint8_t c, lfs_size_t size,
        uint32_t *cksum_) {
    // check for in-bounds
    LFS_ASSERT(block < lfs->cfg->block_count);
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_size_t off_ = off;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_prognext(lfs, block, off_, size_,
                &buffer__, &size__,
                cksum_);
        if (err) {
            return err;
        }

        lfs_memset(buffer__, c, size__);

        // optional checksum
        if (cksum_) {
            *cksum_ = lfs_crc32c(*cksum_, buffer__, size__);
        }

        off_ += size__;
        size_ -= size__;
    }

    return 0;
}



/// Small type-level utilities ///

//// operations on block pairs
//static inline void lfs_pair_swap(lfs_block_t pair[2]) {
//    lfs_block_t t = pair[0];
//    pair[0] = pair[1];
//    pair[1] = t;
//}
//
//static inline bool lfs_pair_isnull(const lfs_block_t pair[2]) {
//    return pair[0] == LFS_BLOCK_NULL || pair[1] == LFS_BLOCK_NULL;
//}
//
//static inline int lfs_pair_cmp(
//        const lfs_block_t paira[2],
//        const lfs_block_t pairb[2]) {
//    return !(paira[0] == pairb[0] || paira[1] == pairb[1] ||
//             paira[0] == pairb[1] || paira[1] == pairb[0]);
//}
//
//static inline bool lfs_pair_issync(
//        const lfs_block_t paira[2],
//        const lfs_block_t pairb[2]) {
//    return (paira[0] == pairb[0] && paira[1] == pairb[1]) ||
//           (paira[0] == pairb[1] && paira[1] == pairb[0]);
//}
//
//static inline void lfs_pair_fromle32(lfs_block_t pair[2]) {
//    pair[0] = lfs_fromle32(pair[0]);
//    pair[1] = lfs_fromle32(pair[1]);
//}
//
//#ifndef LFS_READONLY
//static inline void lfs_pair_tole32(lfs_block_t pair[2]) {
//    pair[0] = lfs_tole32(pair[0]);
//    pair[1] = lfs_tole32(pair[1]);
//}
//#endif
//
//// operations on 32-bit entry tags
//typedef uint32_t lfs_tag_t;
//typedef int32_t lfs_stag_t;
//
//#define LFS_MKTAG(type, id, size) 
//    (((lfs_tag_t)(type) << 20) | ((lfs_tag_t)(id) << 10) | (lfs_tag_t)(size))
//
//#define LFS_MKTAG_IF(cond, type, id, size) 
//    ((cond) ? LFS_MKTAG(type, id, size) : LFS_MKTAG(LFS_FROM_NOOP, 0, 0))
//
//#define LFS_MKTAG_IF_ELSE(cond, type1, id1, size1, type2, id2, size2) 
//    ((cond) ? LFS_MKTAG(type1, id1, size1) : LFS_MKTAG(type2, id2, size2))
//
//static inline bool lfs_tag_isvalid(lfs_tag_t tag) {
//    return !(tag & 0x80000000);
//}
//
//static inline bool lfs_tag_isdelete(lfs_tag_t tag) {
//    return ((int32_t)(tag << 22) >> 22) == -1;
//}
//
//static inline uint16_t lfs_tag_type1(lfs_tag_t tag) {
//    return (tag & 0x70000000) >> 20;
//}
//
//static inline uint16_t lfs_tag_type2(lfs_tag_t tag) {
//    return (tag & 0x78000000) >> 20;
//}
//
//static inline uint16_t lfs_tag_type3(lfs_tag_t tag) {
//    return (tag & 0x7ff00000) >> 20;
//}
//
//static inline uint8_t lfs_tag_chunk(lfs_tag_t tag) {
//    return (tag & 0x0ff00000) >> 20;
//}
//
//static inline int8_t lfs_tag_splice(lfs_tag_t tag) {
//    return (int8_t)lfs_tag_chunk(tag);
//}
//
//static inline uint16_t lfs_tag_id(lfs_tag_t tag) {
//    return (tag & 0x000ffc00) >> 10;
//}
//
//static inline lfs_size_t lfs_tag_size(lfs_tag_t tag) {
//    return tag & 0x000003ff;
//}
//
//static inline lfs_size_t lfs_tag_dsize(lfs_tag_t tag) {
//    return sizeof(tag) + lfs_tag_size(tag + lfs_tag_isdelete(tag));
//}


// 16-bit metadata tags
enum lfsr_tag {
    // the null tag is reserved
    LFSR_TAG_NULL           = 0x0000,

    // config tags
    LFSR_TAG_CONFIG         = 0x0000,
    LFSR_TAG_MAGIC          = 0x0003,
    LFSR_TAG_VERSION        = 0x0004,
    LFSR_TAG_RCOMPAT        = 0x0005,
    LFSR_TAG_WCOMPAT        = 0x0006,
    LFSR_TAG_OCOMPAT        = 0x0007,
    LFSR_TAG_GEOMETRY       = 0x0009,
    LFSR_TAG_NAMELIMIT      = 0x000c,
    LFSR_TAG_FILELIMIT      = 0x000d,

    // global-state tags
    LFSR_TAG_GDELTA         = 0x0100,
    LFSR_TAG_GRMDELTA       = 0x0100,

    // name tags
    LFSR_TAG_NAME           = 0x0200,
    LFSR_TAG_REG            = 0x0201,
    LFSR_TAG_DIR            = 0x0202,
    LFSR_TAG_BOOKMARK       = 0x0204,
    LFSR_TAG_ORPHAN         = 0x0205,

    // struct tags
    LFSR_TAG_STRUCT         = 0x0300,
    LFSR_TAG_DATA           = 0x0300,
    LFSR_TAG_BLOCK          = 0x0304,
    LFSR_TAG_BSHRUB         = 0x0308,
    LFSR_TAG_BTREE          = 0x030c,
    LFSR_TAG_MROOT          = 0x0311,
    LFSR_TAG_MDIR           = 0x0315,
    LFSR_TAG_MTREE          = 0x031c,
    LFSR_TAG_DID            = 0x0320,
    LFSR_TAG_BRANCH         = 0x032c,

    // user/sys attributes
    LFSR_TAG_UATTR          = 0x0400,
    LFSR_TAG_SATTR          = 0x0600,

    // shrub tags belong to secondary trees
    LFSR_TAG_SHRUB          = 0x1000,

    // alt pointers form the inner nodes of our rbyd trees
    LFSR_TAG_ALT            = 0x4000,
    LFSR_TAG_B              = 0x0000,
    LFSR_TAG_R              = 0x2000,
    LFSR_TAG_LE             = 0x0000,
    LFSR_TAG_GT             = 0x1000,

    // checksum tags
    LFSR_TAG_CKSUM          = 0x3000,
    LFSR_TAG_PERTURB        = 0x3100,
    LFSR_TAG_ECKSUM         = 0x3200,

    // in-device only tags, these should never get written to disk
    LFSR_TAG_INTERNAL       = 0x0800,
    LFSR_TAG_MOVE           = 0x0800,
    LFSR_TAG_SHRUBALLOC     = 0x0801,
    LFSR_TAG_SHRUBCOMMIT    = 0x0802,
    LFSR_TAG_SHRUBTRUNK     = 0x0803,

    // some in-device only tag modifiers
    LFSR_TAG_RM             = 0x8000,
    LFSR_TAG_GROW           = 0x4000,
    LFSR_TAG_SUP            = 0x2000,
    LFSR_TAG_SUB            = 0x1000,
};

// some other tag encodings with their own subfields
#define LFSR_TAG_ALT(c, d, key) \
    (LFSR_TAG_ALT \
        | (0x2000 & (c)) \
        | (0x1000 & (d)) \
        | (0x0fff & (lfsr_tag_t)(key)))

#define LFSR_TAG_UATTR(attr) \
    (LFSR_TAG_UATTR \
        | ((0x80 & (lfsr_tag_t)(attr)) << 1) \
        | (0x7f & (lfsr_tag_t)(attr)))

#define LFSR_TAG_SATTR(attr) \
    (LFSR_TAG_SATTR \
        | ((0x80 & (lfsr_tag_t)(attr)) << 1) \
        | (0x7f & (lfsr_tag_t)(attr)))

// tag type operations
static inline lfsr_tag_t lfsr_tag_mode(lfsr_tag_t tag) {
    return tag & 0xf000;
}

static inline lfsr_tag_t lfsr_tag_suptype(lfsr_tag_t tag) {
    return tag & 0xff00;
}

static inline uint8_t lfsr_tag_subtype(lfsr_tag_t tag) {
    return tag & 0x00ff;
}

static inline lfsr_tag_t lfsr_tag_key(lfsr_tag_t tag) {
    return tag & 0x0fff;
}

static inline lfsr_tag_t lfsr_tag_supkey(lfsr_tag_t tag) {
    return tag & 0x0f00;
}

static inline lfsr_tag_t lfsr_tag_subkey(lfsr_tag_t tag) {
    return tag & 0x00ff;
}

static inline bool lfsr_tag_isalt(lfsr_tag_t tag) {
    return tag & LFSR_TAG_ALT;
}

static inline bool lfsr_tag_isshrub(lfsr_tag_t tag) {
    return tag & LFSR_TAG_SHRUB;
}

static inline bool lfsr_tag_istrunk(lfsr_tag_t tag) {
    return lfsr_tag_mode(tag) != LFSR_TAG_CKSUM;
}

static inline bool lfsr_tag_isinternal(lfsr_tag_t tag) {
    return tag & LFSR_TAG_INTERNAL;
}

static inline bool lfsr_tag_isrm(lfsr_tag_t tag) {
    return tag & LFSR_TAG_RM;
}

static inline bool lfsr_tag_isgrow(lfsr_tag_t tag) {
    return tag & LFSR_TAG_GROW;
}

static inline bool lfsr_tag_issup(lfsr_tag_t tag) {
    return tag & LFSR_TAG_SUP;
}

static inline bool lfsr_tag_issub(lfsr_tag_t tag) {
    return tag & LFSR_TAG_SUB;
}

// alt operations
static inline bool lfsr_tag_isblack(lfsr_tag_t tag) {
    return !(tag & LFSR_TAG_R);
}

static inline bool lfsr_tag_isred(lfsr_tag_t tag) {
    return tag & LFSR_TAG_R;
}

static inline bool lfsr_tag_isle(lfsr_tag_t tag) {
    return !(tag & LFSR_TAG_GT);
}

static inline bool lfsr_tag_isgt(lfsr_tag_t tag) {
    return tag & LFSR_TAG_GT;
}

static inline bool lfsr_tag_isa(lfsr_tag_t tag) {
    return (tag & 0x1fff) == (LFSR_TAG_GT | 0);
}

static inline bool lfsr_tag_isn(lfsr_tag_t tag) {
    return (tag & 0x1fff) == (LFSR_TAG_LE | 0);
}

static inline lfsr_tag_t lfsr_tag_isparallel(lfsr_tag_t a, lfsr_tag_t b) {
    return (a & LFSR_TAG_GT) == (b & LFSR_TAG_GT);
}

static inline bool lfsr_tag_follow(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_srid_t rid, lfsr_tag_t tag) {
    // null tags break the following logic for altns/altas
    LFS_ASSERT(lfsr_tag_key(tag) != 0);

    if (lfsr_tag_isgt(alt)) {
        return rid > upper_rid - (lfsr_srid_t)weight - 1
                || (rid == upper_rid - (lfsr_srid_t)weight - 1
                    && lfsr_tag_key(tag) > lfsr_tag_key(alt));
    } else {
        return rid < lower_rid + (lfsr_srid_t)weight - 1
                || (rid == lower_rid + (lfsr_srid_t)weight - 1
                    && lfsr_tag_key(tag) <= lfsr_tag_key(alt));
    }
}

static inline bool lfsr_tag_follow2(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_srid_t rid, lfsr_tag_t tag) {
    if (lfsr_tag_isred(alt2) && lfsr_tag_isparallel(alt, alt2)) {
        weight += weight2;
    }

    return lfsr_tag_follow(alt, weight, lower_rid, upper_rid, rid, tag);
}

static inline void lfsr_tag_flip(
        lfsr_tag_t *alt, lfsr_rid_t *weight,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid) {
    *alt = *alt ^ LFSR_TAG_GT;
    *weight = (upper_rid - lower_rid) - *weight;
}

static inline void lfsr_tag_flip2(
        lfsr_tag_t *alt, lfsr_rid_t *weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid) {
    if (lfsr_tag_isred(alt2)) {
        *weight += weight2;
    }

    lfsr_tag_flip(alt, weight, lower_rid, upper_rid);
}

static inline void lfsr_tag_trim(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_srid_t *lower_rid, lfsr_srid_t *upper_rid,
        lfsr_tag_t *lower_tag, lfsr_tag_t *upper_tag) {
    LFS_ASSERT((lfsr_srid_t)weight >= 0);
    if (lfsr_tag_isgt(alt)) {
        *upper_rid -= weight;
        if (upper_tag && !lfsr_tag_isn(alt)) {
            *upper_tag = alt + 1;
        }
    } else {
        *lower_rid += weight;
        if (lower_tag && !lfsr_tag_isn(alt)) {
            *lower_tag = alt;
        }
    }
}

static inline void lfsr_tag_trim2(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t *lower_rid, lfsr_srid_t *upper_rid,
        lfsr_tag_t *lower_tag, lfsr_tag_t *upper_tag) {
    if (lfsr_tag_isred(alt2)) {
        lfsr_tag_trim(
                alt2, weight2,
                lower_rid, upper_rid,
                lower_tag, upper_tag);
    }

    lfsr_tag_trim(
            alt, weight,
            lower_rid, upper_rid,
            lower_tag, upper_tag);
}

static inline bool lfsr_tag_unreachable(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_tag_t lower_tag, lfsr_tag_t upper_tag) {
    if (lfsr_tag_isgt(alt)) {
        return !lfsr_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                upper_rid-1, upper_tag-1);
    } else {
        return !lfsr_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                lower_rid-1, lower_tag+1);
    }
}

static inline bool lfsr_tag_unreachable2(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_tag_t lower_tag, lfsr_tag_t upper_tag) {
    if (lfsr_tag_isred(alt2)) {
        lfsr_tag_trim(
                alt2, weight2,
                &lower_rid, &upper_rid,
                &lower_tag, &upper_tag);
    }

    return lfsr_tag_unreachable(
            alt, weight,
            lower_rid, upper_rid,
            lower_tag, upper_tag);
}

static inline bool lfsr_tag_diverging(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_srid_t a_rid, lfsr_tag_t a_tag,
        lfsr_srid_t b_rid, lfsr_tag_t b_tag) {
    return lfsr_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                a_rid, a_tag)
            ^ lfsr_tag_follow(
                alt, weight,
                lower_rid, upper_rid,
                b_rid, b_tag);
}

static inline bool lfsr_tag_diverging2(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_srid_t a_rid, lfsr_tag_t a_tag,
        lfsr_srid_t b_rid, lfsr_tag_t b_tag) {
    return lfsr_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_rid, upper_rid,
                a_rid, a_tag)
            ^ lfsr_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_rid, upper_rid,
                b_rid, b_tag);
}


// support for encoding/decoding tags on disk

// tag encoding:
// .---+---+---+- -+- -+- -+- -+---+- -+- -+- -.  tag:    1 be16    2 bytes
// |  tag  | weight            | size          |  weight: 1 leb128  <=5 bytes
// '---+---+---+- -+- -+- -+- -+---+- -+- -+- -'  size:   1 leb128  <=4 bytes
//                                                total:            <=11 bytes
#define LFSR_TAG_DSIZE (2+5+4)

static lfs_ssize_t lfsr_bd_readtag_(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfsr_tag_t *tag_, lfsr_rid_t *weight_, lfs_size_t *size_,
        uint32_t *cksum_) {
    // read the largest possible tag size
    uint8_t tag_buf[LFSR_TAG_DSIZE];
    lfs_size_t tag_dsize = lfs_min32(LFSR_TAG_DSIZE, lfs->cfg->block_size-off);
    if (tag_dsize < 4) {
        return LFS_ERR_CORRUPT;
    }

    int err = lfsr_bd_read(lfs, block, off, hint, tag_buf, tag_dsize);
    if (err) {
        LFS_ASSERT(err < 0);
        return err;
    }

    lfsr_tag_t tag
            = ((lfsr_tag_t)tag_buf[0] << 8)
            | ((lfsr_tag_t)tag_buf[1] << 0);
    lfs_ssize_t d = 2;

    lfsr_rid_t weight;
    lfs_ssize_t d_ = lfs_fromleb128(&weight, &tag_buf[d], tag_dsize-d);
    if (d_ < 0) {
        return d_;
    }
    // weights should be limited to 31-bits
    if (weight > 0x7fffffff) {
        return LFS_ERR_CORRUPT;
    }
    d += d_;

    lfs_size_t size;
    d_ = lfs_fromleb128(&size, &tag_buf[d], tag_dsize-d);
    if (d_ < 0) {
        return d_;
    }
    // sizes should be limited to 28-bits
    if (size > 0x0fffffff) {
        return LFS_ERR_CORRUPT;
    }
    d += d_;

    // optional checksum
    if (cksum_) {
        // ignore the valid bit when calculating checksums
        *cksum_ ^= tag_buf[0] & 0x80;
        *cksum_ = lfs_crc32c(*cksum_, tag_buf, d);
    }

    // save what we found
    *tag_ = tag;
    *weight_ = weight;
    *size_ = size;
    return d;
}

// clear the valid bit, since most readtag calls don't care
static lfs_ssize_t lfsr_bd_readtag(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfsr_tag_t *tag_, lfsr_rid_t *weight_, lfs_size_t *size_,
        uint32_t *cksum_) {
    lfs_ssize_t d = lfsr_bd_readtag_(lfs, block, off, hint,
            tag_, weight_, size_, cksum_);
    if (d < 0) {
        return d;
    }

    if (tag_) {
        *tag_ &= 0x7fff;
    }
    return d;
}

static lfs_ssize_t lfsr_bd_progtag(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off,
        lfsr_tag_t tag, lfsr_rid_t weight, lfs_size_t size,
        uint32_t *cksum_) {
    // bit 7 is reserved for future subtype extensions
    LFS_ASSERT(!(tag & 0x80));
    // weight should not exceed 31-bits
    LFS_ASSERT(weight <= 0x7fffffff);
    // size should not exceed 28-bits
    LFS_ASSERT(size <= 0x0fffffff);

    // encode into a be16 and pair of leb128s
    uint8_t tag_buf[LFSR_TAG_DSIZE];
    tag_buf[0] = (uint8_t)(tag >> 8);
    tag_buf[1] = (uint8_t)(tag >> 0);
    lfs_ssize_t d = 2;

    lfs_ssize_t d_ = lfs_toleb128(weight, &tag_buf[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfs_toleb128(size, &tag_buf[d], 4);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    // ignore the valid bit when calculating checksums
    if (cksum_) {
        *cksum_ ^= tag_buf[0] & 0x80;
    }
    int err = lfsr_bd_prog(lfs, block, off, tag_buf, d,
            cksum_);
    if (err) {
        LFS_ASSERT(err < 0);
        return err;
    }

    return d;
}


/// lfsr_data_t stuff ///

#define LFSR_DATA_ONDISK 0x80000000

#define LFSR_DATA_NULL() \
    ((lfsr_data_t){ \
        .size=0, \
        .u.buffer=NULL})

#define LFSR_DATA_DISK(_block, _off, _size) \
    ((lfsr_data_t){ \
        .size=LFSR_DATA_ONDISK | (_size), \
        .u.disk.block=_block, \
        .u.disk.off=_off})

#define LFSR_DATA_BUF(_buffer, _size) \
    ((lfsr_data_t){ \
        .size=_size, \
        .u.buffer=(const void*)(_buffer)})

// data helpers
static inline bool lfsr_data_ondisk(lfsr_data_t data) {
    return data.size & LFSR_DATA_ONDISK;
}

static inline bool lfsr_data_isbuf(lfsr_data_t data) {
    return !(data.size & LFSR_DATA_ONDISK);
}

static inline lfs_size_t lfsr_data_size(lfsr_data_t data) {
    return data.size & ~LFSR_DATA_ONDISK;
}

static lfsr_data_t lfsr_data_slice(lfsr_data_t data,
        lfs_ssize_t off, lfs_ssize_t size) {
    // limit our off/size to data range, note the use of unsigned casts
    // here to treat -1 as unbounded
    lfs_size_t off_ = lfs_min32(
            lfs_smax32(off, 0),
            lfsr_data_size(data));
    lfs_size_t size_ = lfs_min32(
            (lfs_size_t)size,
            lfsr_data_size(data) - off_);

    // on-disk?
    if (lfsr_data_ondisk(data)) {
        data.u.disk.off += off_;
        data.size = LFSR_DATA_ONDISK | size_;

    // buffer?
    } else {
        data.u.buffer += off_;
        data.size = size_;
    }

    return data;
}

static lfsr_data_t lfsr_data_truncate(lfsr_data_t data, lfs_size_t size) {
    return lfsr_data_slice(data, -1, size);
}

static lfsr_data_t lfsr_data_fruncate(lfsr_data_t data, lfs_size_t size) {
    return lfsr_data_slice(data,
            lfsr_data_size(data) - lfs_min32(
                size,
                lfsr_data_size(data)),
            -1);
}


// data <-> bd interactions

// lfsr_data_read* operations update the lfsr_data_t, effectively
// consuming the data

static lfs_ssize_t lfsr_data_read(lfs_t *lfs, lfsr_data_t *data,
        void *buffer, lfs_size_t size) {
    // limit our size to data range
    lfs_size_t d = lfs_min32(size, lfsr_data_size(*data));

    // on-disk?
    if (lfsr_data_ondisk(*data)) {
        int err = lfsr_bd_read(lfs, data->u.disk.block, data->u.disk.off,
                // note our hint includes the full data range
                lfsr_data_size(*data),
                buffer, d);
        if (err) {
            LFS_ASSERT(err < 0);
            return err;
        }

    // buffer?
    } else {
        lfs_memcpy(buffer, data->u.buffer, d);
    }

    *data = lfsr_data_slice(*data, d, -1);
    return d;
}

static int lfsr_data_readle32(lfs_t *lfs, lfsr_data_t *data,
        uint32_t *word) {
    uint8_t buf[4];
    lfs_ssize_t d = lfsr_data_read(lfs, data, buf, 4);
    if (d < 0) {
        return d;
    }

    // truncated?
    if (d < 4) {
        return LFS_ERR_CORRUPT;
    }

    *word = lfs_fromle32_(buf);
    return 0;
}

// note all leb128s in our system reserve the sign bit
static int lfsr_data_readleb128(lfs_t *lfs, lfsr_data_t *data,
        uint32_t *word_) {
    // note we make sure not to update our data offset until after leb128
    // decoding
    lfsr_data_t data_ = *data;

    // for 32-bits we can assume worst-case leb128 size is 5-bytes
    uint8_t buf[5];
    lfs_ssize_t d = lfsr_data_read(lfs, &data_, buf, 5);
    if (d < 0) {
        return d;
    }

    d = lfs_fromleb128(word_, buf, d);
    if (d < 0) {
        return d;
    }
    // all leb128s in our system reserve the sign bit
    if (*word_ > 0x7fffffff) {
        return LFS_ERR_CORRUPT;
    }

    *data = lfsr_data_slice(*data, d, -1);
    return 0;
}

// a little-leb128 in our system is truncated to align nicely
//
// for 32-bit words, little-leb128s are truncated to 28-bits, so the
// resulting leb128 encoding fits nicely in 4-bytes
static inline int lfsr_data_readlleb128(lfs_t *lfs, lfsr_data_t *data,
        uint32_t *word_) {
    // just call readleb128 here
    int err = lfsr_data_readleb128(lfs, data, word_);
    if (err) {
        return err;
    }
    // little-leb128s should be limited to 28-bits
    if (*word_ > 0x0fffffff) {
        return LFS_ERR_CORRUPT;
    }

    return 0;
}

static lfs_scmp_t lfsr_data_cmp(lfs_t *lfs, lfsr_data_t data,
        const void *buffer, lfs_size_t size) {
    // compare common prefix
    lfs_size_t d = lfs_min32(size, lfsr_data_size(data));

    // on-disk?
    if (lfsr_data_ondisk(data)) {
        int cmp = lfsr_bd_cmp(lfs, data.u.disk.block, data.u.disk.off, 0,
                buffer, d);
        if (cmp != LFS_CMP_EQ) {
            return cmp;
        }

    // buffer?
    } else {
        int cmp = lfs_memcmp(data.u.buffer, buffer, d);
        if (cmp < 0) {
            return LFS_CMP_LT;
        } else if (cmp > 0) {
            return LFS_CMP_GT;
        }
    }

    // if data is equal, check for size mismatch
    if (lfsr_data_size(data) < size) {
        return LFS_CMP_LT;
    } else if (lfsr_data_size(data) > size) {
        return LFS_CMP_GT;
    } else {
        return LFS_CMP_EQ;
    }
}

static lfs_scmp_t lfsr_data_namecmp(lfs_t *lfs, lfsr_data_t data,
        lfsr_did_t did, const char *name, lfs_size_t name_size) {
    // first compare the did
    lfsr_did_t did_;
    int err = lfsr_data_readleb128(lfs, &data, &did_);
    if (err) {
        LFS_ASSERT(err < 0);
        return err;
    }

    if (did_ < did) {
        return LFS_CMP_LT;
    } else if (did_ > did) {
        return LFS_CMP_GT;
    }

    // then compare the actual name
    return lfsr_data_cmp(lfs, data, name, name_size);
}

static int lfsr_bd_progdata(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfsr_data_t data,
        uint32_t *cksum_) {
    // on-disk?
    if (lfsr_data_ondisk(data)) {
        int err = lfsr_bd_cpy(lfs, block, off,
                data.u.disk.block, data.u.disk.off, lfsr_data_size(data),
                lfsr_data_size(data),
                cksum_);
        if (err) {
            return err;
        }

    // buffer?
    } else {
        int err = lfsr_bd_prog(lfs, block, off,
                data.u.buffer, data.size,
                cksum_);
        if (err) {
            return err;
        }
    }

    return 0;
}

// we can also treat leb128/lleb128 encoding has a high-level operation,
// which is useful for building attrs

#define LFSR_LEB128_DSIZE  5

#define LFSR_DATA_LEB128_(_word, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromleb128(_word, _buffer)}.d)

#define LFSR_DATA_LEB128(_word) \
    LFSR_DATA_LEB128_(_word, (uint8_t[LFSR_LEB128_DSIZE]){0})

static inline lfsr_data_t lfsr_data_fromleb128(uint32_t word,
        uint8_t buffer[static LFSR_LEB128_DSIZE]) {
    // leb128s should not exceed 31-bits
    LFS_ASSERT(word <= 0x7fffffff);

    lfs_ssize_t d = lfs_toleb128(word, buffer, LFSR_LEB128_DSIZE);
    LFS_ASSERT(d >= 0);
    return LFSR_DATA_BUF(buffer, d);
}

#define LFSR_LLEB128_DSIZE 4

#define LFSR_DATA_LLEB128_(_word, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromlleb128(_word, _buffer)}.d)

#define LFSR_DATA_LLEB128(_word) \
    LFSR_DATA_LLEB128_(_word, (uint8_t[LFSR_LLEB128_DSIZE]){0})

static inline lfsr_data_t lfsr_data_fromlleb128(uint32_t word,
        uint8_t buffer[static LFSR_LLEB128_DSIZE]) {
    // little-leb128s should not exceed 28-bits
    LFS_ASSERT(word <= 0x0fffffff);

    lfs_ssize_t d = lfs_toleb128(word, buffer, LFSR_LLEB128_DSIZE);
    LFS_ASSERT(d >= 0);
    return LFSR_DATA_BUF(buffer, d);
}


// operations on attribute lists

typedef struct lfsr_attr {
    lfsr_tag_t tag;
    int16_t count;
    lfsr_srid_t weight;
    // sign(size)=0 => single in-RAM buffer
    // sign(size)=1 => multiple concatenated datas
    // special tags => other things
    const void *cat;
} lfsr_attr_t;

#define LFSR_ATTR_(_tag, _weight, _cat, _count) \
    ((lfsr_attr_t){ \
        .tag=_tag, \
        .count=(uint16_t){_count}, \
        .weight=_weight, \
        .cat=_cat})

#define LFSR_ATTR(_tag, _weight, _data) \
    ((struct {lfsr_attr_t a;}){lfsr_attr(_tag, _weight, _data)}.a)

static inline lfsr_attr_t lfsr_attr(
        lfsr_tag_t tag, lfsr_srid_t weight, lfsr_data_t data) {
    // only simple data works here
    LFS_ASSERT(lfsr_data_isbuf(data));
    LFS_ASSERT(lfsr_data_size(data) <= 0x7fff);
    return (lfsr_attr_t){
        .tag=tag,
        .count=lfsr_data_size(data),
        .weight=weight,
        .cat=data.u.buffer};
}

#define LFSR_ATTR_CAT_(_tag, _weight, _datas, _data_count) \
    ((lfsr_attr_t){ \
        .tag=_tag, \
        .count=-(uint16_t){_data_count}, \
        .weight=_weight, \
        .cat=_datas})

#define LFSR_ATTR_CAT(_tag, _weight, ...) \
    LFSR_ATTR_CAT_( \
        _tag, \
        _weight, \
        (const lfsr_data_t[]){__VA_ARGS__}, \
        sizeof((const lfsr_data_t[]){__VA_ARGS__}) / sizeof(lfsr_data_t))

#define LFSR_ATTR_NOOP() \
    LFSR_ATTR_(LFSR_TAG_NULL, 0, NULL, 0)

// create an attribute list
#define LFSR_ATTRS(...) \
    (const lfsr_attr_t[]){__VA_ARGS__}, \
    sizeof((const lfsr_attr_t[]){__VA_ARGS__}) / sizeof(lfsr_attr_t)

// cat helpers
static inline lfs_size_t lfsr_cat_size(const void *cat, int16_t count) {
    // this gets a bit complicated for concatenated data
    if (count >= 0) {
        return count;

    } else {
        const lfsr_data_t *datas = cat;
        lfs_size_t data_count = -count;
        lfs_size_t size = 0;
        for (lfs_size_t i = 0; i < data_count; i++) {
            size += lfsr_data_size(datas[i]);
        }
        return size;
    }
}

// cat <-> bd interactions
static int lfsr_bd_progcat(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off,
        const void *cat, int16_t count,
        uint32_t *cksum_) {
    // direct buffer?
    if (count >= 0) {
        return lfsr_bd_prog(lfs, block, off, cat, count,
                cksum_);

    // indirect concatenated data?
    } else {
        const lfsr_data_t *datas = cat;
        lfs_size_t data_count = -count;
        for (lfs_size_t i = 0; i < data_count; i++) {
            int err = lfsr_bd_progdata(lfs, block, off, datas[i],
                    cksum_);
            if (err) {
                return err;
            }

            off += lfsr_data_size(datas[i]);
        }
        return 0;
    }
}

// other attr helpers
static inline bool lfsr_attr_isnoop(lfsr_attr_t attr) {
    // noop attrs must have zero weight
    LFS_ASSERT(attr.tag || attr.weight == 0);
    return !attr.tag;
}

static inline bool lfsr_attr_isinsert(lfsr_attr_t attr) {
    return !lfsr_tag_isgrow(attr.tag) && attr.weight > 0;
}

static inline lfsr_srid_t lfsr_attr_nextrid(lfsr_attr_t attr,
        lfsr_srid_t rid) {
    if (lfsr_attr_isinsert(attr)) {
        return rid + attr.weight-1;
    } else {
        return rid + attr.weight;
    }
}

static inline lfs_size_t lfsr_attr_size(lfsr_attr_t attr) {
    return lfsr_cat_size(attr.cat, attr.count);
}

// special attrs - here be hacks

// special case for passing names, we need to cat but we don't need the
// full lfsr_data_t
typedef struct lfsr_data_name {
    lfsr_data_t did_data;
    lfs_size_t name_size;
    const uint8_t *name;
} lfsr_data_name_t;

#define LFSR_ATTR_NAME(_tag, _weight, _did, _name, _name_size) \
    LFSR_ATTR_CAT_( \
        _tag, \
        _weight, \
        ((lfsr_data_t*)&(lfsr_data_name_t){ \
            .did_data=LFSR_DATA_LEB128(_did), \
            .name_size=_name_size, \
            .name=(const void*)(_name)}), \
        2)

// hacky attrs - these end up handled as special cases in high-level
// commit layers

// a move of all attrs from an mdir entry
#define LFSR_ATTR_MOVE(_tag, _weight, _mdir) \
    LFSR_ATTR_(_tag, _weight, (const lfsr_mdir_t*){_mdir}, 0)

// a grm update, note this is mutable! we may update the grm during
// mdir commits
#define LFSR_ATTR_GRM(_tag, _weight, _grm) \
    LFSR_ATTR_(_tag, _weight, (const lfsr_grm_t*){_grm}, 0)

// writing to an unrelated trunk in the rbyd
typedef struct lfsr_shrubcommit lfsr_shrubcommit_t;
#define LFSR_ATTR_SHRUBCOMMIT(_tag, _weight, \
        _shrub, _rid, _attrs, _attr_count) \
    LFSR_ATTR_(_tag, _weight, \
        (&(const lfsr_shrubcommit_t){ \
            .shrub=_shrub, \
            .rid=_rid, \
            .attrs=_attrs, \
            .attr_count=_attr_count}), \
        0)

#define LFSR_ATTR_SHRUBTRUNK(_tag, _weight, _shrub) \
    LFSR_ATTR_(_tag, _weight, (const lfsr_shrub_t*){_shrub}, 0)



// generalized info returned by traveral functions
typedef struct lfsr_tinfo {
    lfsr_tag_t tag;
    union {
        lfsr_data_t data;
        lfsr_mdir_t mdir;
        lfsr_rbyd_t rbyd;
        lfsr_bptr_t bptr;
    } u;
} lfsr_tinfo_t;


//struct lfsr_attr_from {
//    const lfsr_rbyd_t *rbyd;
//    const struct lfsr_attr *attrs;
//    lfs_size_t start;
//};
//
//#define LFSR_ATTR_FROM(_id, _rbyd, _attrs, _start, _stop, _next) 
//    LFSR_ATTR(FROM, _id, 
//        (&(const struct lfsr_attr_from){_rbyd, _attrs, _start}), 
//        (_stop)-(_start), _next)
//
//#define LFS_MKRATTR_(...)
//    (&(const struct lfsr_attr){__VA_ARGS__})
//
//#define LFS_MKRATTR(type1, type2, id, buffer, size, next)
//    (&(const struct lfsr_attr){
//        LFS_MKRTAG(type1, type2, id),
//        buffer, size, next})
//
//#define LFS_MKRRMATTR(type1, type2, id, next)
//    (&(const struct lfsr_attr){
//        LFS_MKRRMTAG(type1, type2, id),
//        NULL, 0, next})



//// find state when looking up by name
//typedef struct lfsr_find {
//    // what to search for
//    const char *name;
//    lfs_size_t name_size;
//
//    // if found, the tag/id will be placed in found_tag/found_id,
//    // otherwise found_tag will be zero and found_id will be set to
//    // the largest, smaller id (a good place to insert)
//    lfs_ssize_t predicted_id;
//    lfs_ssize_t found_id;
//    lfsr_tag_t predicted_tag;
//    lfsr_tag_t found_tag;
//} lfsr_find_t;



//// operations on global state
//static inline void lfs_gstate_xor(lfs_gstate_t *a, const lfs_gstate_t *b) {
//    for (int i = 0; i < 3; i++) {
//        ((uint32_t*)a)[i] ^= ((const uint32_t*)b)[i];
//    }
//}
//
//static inline bool lfs_gstate_iszero(const lfs_gstate_t *a) {
//    for (int i = 0; i < 3; i++) {
//        if (((uint32_t*)a)[i] != 0) {
//            return false;
//        }
//    }
//    return true;
//}
//
//#ifndef LFS_READONLY
//static inline bool lfs_gstate_hasorphans(const lfs_gstate_t *a) {
//    return lfs_tag_size(a->tag);
//}
//
//static inline uint8_t lfs_gstate_getorphans(const lfs_gstate_t *a) {
//    return lfs_tag_size(a->tag);
//}
//
//static inline bool lfs_gstate_hasmove(const lfs_gstate_t *a) {
//    return lfs_tag_type1(a->tag);
//}
//#endif
//
//static inline bool lfs_gstate_hasmovehere(const lfs_gstate_t *a,
//        const lfs_block_t *pair) {
//    return lfs_tag_type1(a->tag) && lfs_pair_cmp(a->pair, pair) == 0;
//}
//
//static inline void lfs_gstate_fromle32(lfs_gstate_t *a) {
//    a->tag     = lfs_fromle32(a->tag);
//    a->pair[0] = lfs_fromle32(a->pair[0]);
//    a->pair[1] = lfs_fromle32(a->pair[1]);
//}
//
//#ifndef LFS_READONLY
//static inline void lfs_gstate_tole32(lfs_gstate_t *a) {
//    a->tag     = lfs_tole32(a->tag);
//    a->pair[0] = lfs_tole32(a->pair[0]);
//    a->pair[1] = lfs_tole32(a->pair[1]);
//}
//#endif
//
//// operations on forward-CRCs used to track erased state
//struct lfs_fcrc {
//    lfs_size_t size;
//    uint32_t crc;
//};
//
//static void lfs_fcrc_fromle32(struct lfs_fcrc *fcrc) {
//    fcrc->size = lfs_fromle32(fcrc->size);
//    fcrc->crc = lfs_fromle32(fcrc->crc);
//}
//
//#ifndef LFS_READONLY
//static void lfs_fcrc_tole32(struct lfs_fcrc *fcrc) {
//    fcrc->size = lfs_tole32(fcrc->size);
//    fcrc->crc = lfs_tole32(fcrc->crc);
//}
//#endif


// erased-state checksum
typedef struct lfsr_ecksum {
    // cksize=-1 indicates no ecksum
    lfs_ssize_t cksize;
    uint32_t cksum;
} lfsr_ecksum_t;

// erased-state checksum on-disk encoding

// ecksum encoding:
// .---+- -+- -+- -.  cksize: 1 leb128  <=4 bytes
// | cksize        |  cksum:  1 le32    4 bytes
// +---+- -+- -+- -+  total:            <=8 bytes
// |     cksum     |
// '---+---+---+---'
//
#define LFSR_ECKSUM_DSIZE (4+4)

#define LFSR_DATA_ECKSUM_(_ecksum, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromecksum(_ecksum, _buffer)}.d)

#define LFSR_DATA_ECKSUM(_ecksum) \
    LFSR_DATA_ECKSUM_(_ecksum, (uint8_t[LFSR_ECKSUM_DSIZE]){0})

static lfsr_data_t lfsr_data_fromecksum(const lfsr_ecksum_t *ecksum,
        uint8_t buffer[static LFSR_ECKSUM_DSIZE]) {
    // you shouldn't try to encode a not-ecksum, that doesn't make sense
    LFS_ASSERT(ecksum->cksize != -1);
    // cksize should not exceed 28-bits
    LFS_ASSERT((lfs_size_t)ecksum->cksize <= 0x0fffffff);

    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfs_toleb128(ecksum->cksize, &buffer[d], 4);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    lfs_tole32_(ecksum->cksum, &buffer[d]);
    d += 4;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readecksum(lfs_t *lfs, lfsr_data_t *data,
        lfsr_ecksum_t *ecksum) {
    int err = lfsr_data_readlleb128(lfs, data, (lfs_size_t*)&ecksum->cksize);
    if (err) {
        return err;
    }

    err = lfsr_data_readle32(lfs, data, &ecksum->cksum);
    if (err) {
        return err;
    }

    return 0;
}


// block pointer things

// bptr encoding:
// .---+- -+- -+- -.      size:   1 leb128  <=4 bytes
// | size          |      block:  1 leb128  <=5 bytes
// +---+- -+- -+- -+- -.  off:    1 leb128  <=4 bytes
// | block             |  cksize: 1 leb128  <=4 bytes
// +---+- -+- -+- -+- -'  cksum:  1 le32    4 bytes
// | off           |      total:            <=21 bytes
// +---+- -+- -+- -+
// | cksize        |
// +---+- -+- -+- -+
// |     cksum     |
// '---+---+---+---'
//
#define LFSR_BPTR_DSIZE (4+5+4+4+4)

#define LFSR_DATA_BPTR_(_bptr, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_frombptr(_bptr, _buffer)}.d)

#define LFSR_DATA_BPTR(_bptr) \
    LFSR_DATA_BPTR_(_bptr, (uint8_t[LFSR_BPTR_DSIZE]){0})

static lfsr_data_t lfsr_data_frombptr(const lfsr_bptr_t *bptr,
        uint8_t buffer[static LFSR_BPTR_DSIZE]) {
    // size should not exceed 28-bits
    LFS_ASSERT(lfsr_data_size(bptr->data) <= 0x0fffffff);
    // block should not exceed 31-bits
    LFS_ASSERT(bptr->data.u.disk.block <= 0x7fffffff);
    // off should not exceed 28-bits
    LFS_ASSERT(bptr->data.u.disk.off <= 0x0fffffff);
    // cksize should not exceed 28-bits
    LFS_ASSERT(bptr->cksize <= 0x0fffffff);
    lfs_ssize_t d = 0;

    // write the block, offset, size
    lfs_ssize_t d_ = lfs_toleb128(lfsr_data_size(bptr->data), &buffer[d], 4);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(bptr->data.u.disk.block, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(bptr->data.u.disk.off, &buffer[d], 4);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    // write the cksize, cksum
    d_ = lfs_toleb128(bptr->cksize, &buffer[d], 4);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    lfs_tole32_(bptr->cksum, &buffer[d]);
    d += 4;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readbptr(lfs_t *lfs, lfsr_data_t *data,
        lfsr_bptr_t *bptr) {
    // read the block, offset, size
    int err = lfsr_data_readlleb128(lfs, data, &bptr->data.size);
    if (err) {
        return err;
    }

    err = lfsr_data_readleb128(lfs, data, &bptr->data.u.disk.block);
    if (err) {
        return err;
    }

    err = lfsr_data_readlleb128(lfs, data, &bptr->data.u.disk.off);
    if (err) {
        return err;
    }

    // read the cksize, cksum
    err = lfsr_data_readlleb128(lfs, data, &bptr->cksize);
    if (err) {
        return err;
    }

    err = lfsr_data_readle32(lfs, data, &bptr->cksum);
    if (err) {
        return err;
    }

    // all bptrs have this flag set, this is used to differentiate
    // bptrs from btrees in files
    bptr->data.size |= LFSR_DATA_ONDISK;
    return 0;
}



//// other endianness operations
//static void lfs_ctz_fromle32(struct lfs_ctz *ctz) {
//    ctz->head = lfs_fromle32(ctz->head);
//    ctz->size = lfs_fromle32(ctz->size);
//}
//
//#ifndef LFS_READONLY
//static void lfs_ctz_tole32(struct lfs_ctz *ctz) {
//    ctz->head = lfs_tole32(ctz->head);
//    ctz->size = lfs_tole32(ctz->size);
//}
//#endif
//
//static inline void lfs_superblock_fromle32(lfs_superblock_t *superblock) {
//    superblock->version     = lfs_fromle32(superblock->version);
//    superblock->block_size  = lfs_fromle32(superblock->block_size);
//    superblock->block_count = lfs_fromle32(superblock->block_count);
//    superblock->name_max    = lfs_fromle32(superblock->name_max);
//    superblock->file_max    = lfs_fromle32(superblock->file_max);
//    superblock->attr_max    = lfs_fromle32(superblock->attr_max);
//}
//
//#ifndef LFS_READONLY
//static inline void lfs_superblock_tole32(lfs_superblock_t *superblock) {
//    superblock->version     = lfs_tole32(superblock->version);
//    superblock->block_size  = lfs_tole32(superblock->block_size);
//    superblock->block_count = lfs_tole32(superblock->block_count);
//    superblock->name_max    = lfs_tole32(superblock->name_max);
//    superblock->file_max    = lfs_tole32(superblock->file_max);
//    superblock->attr_max    = lfs_tole32(superblock->attr_max);
//}
//#endif
//
//#ifndef LFS_NO_ASSERT
//static bool lfs_mlist_isopen(struct lfs_mlist *head,
//        struct lfs_mlist *node) {
//    for (struct lfs_mlist **p = &head; *p; p = &(*p)->next) {
//        if (*p == (struct lfs_mlist*)node) {
//            return true;
//        }
//    }
//
//    return false;
//}
//#endif
//
//static void lfs_mlist_remove(lfs_t *lfs, struct lfs_mlist *mlist) {
//    for (struct lfs_mlist **p = &lfs->mlist; *p; p = &(*p)->next) {
//        if (*p == mlist) {
//            *p = (*p)->next;
//            break;
//        }
//    }
//}
//
//static void lfs_mlist_append(lfs_t *lfs, struct lfs_mlist *mlist) {
//    mlist->next = lfs->mlist;
//    lfs->mlist = mlist;
//}


/// Internal operations predeclared here ///
//#ifndef LFS_READONLY
//static int lfs_dir_commit(lfs_t *lfs, lfs_mdir_t *dir,
//        const struct lfs_mattr *attrs, int attrcount);
//static int lfs_dir_compact(lfs_t *lfs,
//        lfs_mdir_t *dir, const struct lfs_mattr *attrs, int attrcount,
//        lfs_mdir_t *source, uint16_t begin, uint16_t end);
//static lfs_ssize_t lfs_file_flushedwrite(lfs_t *lfs, lfs_file_t *file,
//        const void *buffer, lfs_size_t size);
//static lfs_ssize_t lfs_file_rawwrite(lfs_t *lfs, lfs_file_t *file,
//        const void *buffer, lfs_size_t size);
//static int lfs_file_rawsync(lfs_t *lfs, lfs_file_t *file);
//static int lfs_file_outline(lfs_t *lfs, lfs_file_t *file);
//static int lfs_file_flush(lfs_t *lfs, lfs_file_t *file);
//
//static int lfs_fs_deorphan(lfs_t *lfs, bool powerloss);
//static int lfs_fs_preporphans(lfs_t *lfs, int8_t orphans);
//static void lfs_fs_prepmove(lfs_t *lfs,
//        uint16_t id, const lfs_block_t pair[2]);
//static int lfs_fs_pred(lfs_t *lfs, const lfs_block_t dir[2],
//        lfs_mdir_t *pdir);
//static lfs_stag_t lfs_fs_parent(lfs_t *lfs, const lfs_block_t dir[2],
//        lfs_mdir_t *parent);
//static int lfs_fs_forceconsistency(lfs_t *lfs);
//#endif
//
//#ifdef LFS_MIGRATE
//static int lfs1_traverse(lfs_t *lfs,
//        int (*cb)(void*, lfs_block_t), void *data);
//#endif
//
//static int lfs_dir_rawrewind(lfs_t *lfs, lfs_dir_t *dir);
//
//static lfs_ssize_t lfs_file_flushedread(lfs_t *lfs, lfs_file_t *file,
//        void *buffer, lfs_size_t size);
//static lfs_ssize_t lfs_file_rawread(lfs_t *lfs, lfs_file_t *file,
//        void *buffer, lfs_size_t size);
//static int lfs_file_rawclose(lfs_t *lfs, lfs_file_t *file);
//static lfs_soff_t lfs_file_rawsize(lfs_t *lfs, lfs_file_t *file);
//
//static lfs_ssize_t lfs_fs_rawsize(lfs_t *lfs);
//static int lfs_fs_rawtraverse(lfs_t *lfs,
//        int (*cb)(void *data, lfs_block_t block), void *data,
//        bool includeorphans);

//static int lfs_deinit(lfs_t *lfs);
//static int lfs_rawunmount(lfs_t *lfs);


// predeclare block allocator functions
static int lfs_alloc(lfs_t *lfs, lfs_block_t *block, bool erase);
static void lfs_alloc_ckpoint(lfs_t *lfs);


/// Red-black-yellow Dhara tree operations ///

#define LFSR_RBYD_ISSHRUB 0x80000000
#define LFSR_RBYD_PARITY  0x80000000

// helper functions
static inline bool lfsr_rbyd_isshrub(const lfsr_rbyd_t *rbyd) {
    return rbyd->trunk & LFSR_RBYD_ISSHRUB;
}

static inline lfs_size_t lfsr_rbyd_trunk(const lfsr_rbyd_t *rbyd) {
    return rbyd->trunk & ~LFSR_RBYD_ISSHRUB;
}

static inline bool lfsr_rbyd_isfetched(const lfsr_rbyd_t *rbyd) {
    return !lfsr_rbyd_trunk(rbyd) || rbyd->eoff;
}

static inline bool lfsr_rbyd_parity(const lfsr_rbyd_t *rbyd) {
    return rbyd->eoff >> (8*sizeof(lfs_size_t)-1);
}

static inline lfs_size_t lfsr_rbyd_eoff(const lfsr_rbyd_t *rbyd) {
    return rbyd->eoff & ~LFSR_RBYD_PARITY;
}

static inline int lfsr_rbyd_cmp(
        const lfsr_rbyd_t *a,
        const lfsr_rbyd_t *b) {
    if (a->blocks[0] != b->blocks[0]) {
        return a->blocks[0] - b->blocks[0];
    } else {
        return a->trunk - b->trunk;
    }
}


// allocate an rbyd block
static int lfsr_rbyd_alloc(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    *rbyd = (lfsr_rbyd_t){.weight=0, .trunk=0, .eoff=0, .cksum=0};
    int err = lfs_alloc(lfs, &rbyd->blocks[0], true);
    if (err) {
        return err;
    }

    return 0;
}

static int lfsr_rbyd_fetch(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_block_t block, lfs_size_t trunk) {
    // set up some initial state
    rbyd->blocks[0] = block;
    rbyd->trunk = (trunk & LFSR_RBYD_ISSHRUB) | 0;
    rbyd->eoff = 0;

    // ignore the shrub bit here
    trunk &= ~LFSR_RBYD_ISSHRUB;

    // checksum the revision count to get the cksum started
    uint32_t cksum = 0;
    int err = lfsr_bd_cksum(lfs, block, 0, -1, sizeof(uint32_t),
            &cksum);
    if (err) {
        return err;
    }

    // temporary state until we validate a cksum
    uint32_t cksum_ = cksum;
    bool parity_ = lfs_parity(cksum);
    lfs_size_t off = sizeof(uint32_t);
    lfs_size_t trunk_ = 0;
    lfs_size_t trunk__ = 0;
    lfsr_rid_t weight = 0;
    lfsr_rid_t weight_ = 0;

    // assume unerased until proven otherwise
    lfsr_ecksum_t ecksum = {.cksize=-1};

    // scan tags, checking valid bits, cksums, etc
    while (off < lfs->cfg->block_size
            && (!trunk || lfsr_rbyd_eoff(rbyd) <= trunk)) {
        lfsr_tag_t tag;
        lfsr_rid_t weight__;
        lfs_size_t size;
        uint32_t cksum__ = cksum_;
        lfs_ssize_t d = lfsr_bd_readtag_(lfs, block, off, -1,
                &tag, &weight__, &size, &cksum__);
        if (d < 0) {
            if (d == LFS_ERR_CORRUPT) {
                break;
            }
            return d;
        }
        lfs_size_t off_ = off + d;

        // parity mismatch?
        if ((tag >> 15) != parity_) {
            break;
        }
        tag &= 0x7fff;
        parity_ ^= lfs_parity(cksum_ ^ cksum__);
        cksum_ = cksum__;

        // tag goes out of range?
        if (!lfsr_tag_isalt(tag) && off_ + size > lfs->cfg->block_size) {
            break;
        }

        // take care of cksum
        if (!lfsr_tag_isalt(tag)) {
            // not an end-of-commit cksum
            if (lfsr_tag_suptype(tag) != LFSR_TAG_CKSUM) {
                // cksum the entry, hopefully leaving it in the cache
                uint32_t cksum__ = cksum_;
                err = lfsr_bd_cksum(lfs, block, off_, -1, size,
                        &cksum__);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }
                parity_ ^= lfs_parity(cksum_ ^ cksum__);
                cksum_ = cksum__;

                // found an ecksum? save for later
                if (tag == LFSR_TAG_ECKSUM) {
                    err = lfsr_data_readecksum(lfs,
                            &LFSR_DATA_DISK(block, off_,
                                lfs->cfg->block_size - off_),
                            &ecksum);
                    if (err && err != LFS_ERR_CORRUPT) {
                        return err;
                    }

                    // TODO ignore?? why not break?
                    // ignore malformed ecksums
                    if (err == LFS_ERR_CORRUPT) {
                        ecksum.cksize = -1;
                    }
                }

            // is an end-of-commit cksum
            } else {
                uint32_t cksum__ = 0;
                err = lfsr_bd_read(lfs, block, off_, -1,
                        &cksum__, sizeof(uint32_t));
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }
                cksum__ = lfs_fromle32_(&cksum__);

                if (cksum_ != cksum__) {
                    // uh oh, cksums don't match
                    break;
                }

                // save what we've found so far
                rbyd->eoff
                        = ((lfs_size_t)parity_ << (8*sizeof(lfs_size_t)-1))
                        | (off_ + size);
                rbyd->cksum = cksum;
                rbyd->trunk = (LFSR_RBYD_ISSHRUB & rbyd->trunk) | trunk_;
                rbyd->weight = weight;

                // revert to data checksum
                cksum_ = cksum;
            }
        }

        // found a trunk of a tree?
        if (lfsr_tag_istrunk(tag)
                && (!trunk || off <= trunk || trunk__)) {
            // start of trunk?
            if (!trunk__) {
                // keep track of trunk's entry point
                trunk__ = off;
                // reset weight
                weight_ = 0;
            }

            // derive weight of the tree from alt pointers
            //
            // NOTE we can't check for overflow/underflow here because we
            // may be overeagerly parsing an invalid commit, it's ok for
            // this to overflow/underflow as long as we throw it out later
            // on a bad cksum
            weight_ += weight__;

            // end of trunk?
            if (!lfsr_tag_isalt(tag)) {
                // update data checksum
                cksum = cksum_;
                // update trunk and weight, unless we are a shrub trunk
                if (!lfsr_tag_isshrub(tag) || trunk__ == trunk) {
                    trunk_ = trunk__;
                    weight = weight_;
                }
                trunk__ = 0;
            }
        }

        // skip data
        if (!lfsr_tag_isalt(tag)) {
            off_ += size;
        }

        off = off_;
    }

    // no valid commits?
    if (!lfsr_rbyd_trunk(rbyd)) {
        return LFS_ERR_CORRUPT;
    }

    // did we end on a valid commit? we may have erased-state
    bool erased = false;
    if (lfsr_rbyd_eoff(rbyd) < lfs->cfg->block_size
            && lfsr_rbyd_eoff(rbyd) % lfs->cfg->prog_size == 0
            && ecksum.cksize != -1) {
        uint8_t e = 0;
        err = lfsr_bd_read(lfs,
                rbyd->blocks[0], lfsr_rbyd_eoff(rbyd), ecksum.cksize,
                &e, 1);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        // the next valid bit must _not_ match, or a commit was attempted
        if ((e >> 7) != lfsr_rbyd_parity(rbyd)) {
            // check that erased-state matches our checksum, if this fails
            // most likely a write was interrupted
            uint32_t ecksum_ = 0;
            if (err != LFS_ERR_CORRUPT) {
                ecksum_ = lfs_crc32c(0, &e, 1);
            }
            err = lfsr_bd_cksum(lfs,
                    rbyd->blocks[0], lfsr_rbyd_eoff(rbyd)+1, 0,
                    ecksum.cksize-1,
                    &ecksum_);
            if (err && err != LFS_ERR_CORRUPT) {
                return err;
            }

            // found erased-state?
            erased = (ecksum_ == ecksum.cksum);
        }
    }
    if (!erased) {
        rbyd->eoff = -1;
    }

    return 0;
}

// a more aggressive fetch when checksum is known
static int lfsr_rbyd_fetchvalidate(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_block_t block, lfs_size_t trunk, lfsr_rid_t weight,
        uint32_t cksum) {
    int err = lfsr_rbyd_fetch(lfs, rbyd, block, trunk);
    if (err) {
        if (err == LFS_ERR_CORRUPT) {
            LFS_ERROR("Found corrupted rbyd 0x%"PRIx32".%"PRIx32", "
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
        LFS_ERROR("Found rbyd cksum mismatch rbyd 0x%"PRIx32".%"PRIx32", "
                "cksum %08"PRIx32" (!= %08"PRIx32")",
                rbyd->blocks[0], lfsr_rbyd_trunk(rbyd), rbyd->cksum, cksum);
        return LFS_ERR_CORRUPT;
    }

    // if trunk/weight mismatch _after_ cksums match, that's not a storage
    // error, that's a programming error
    LFS_ASSERT(lfsr_rbyd_trunk(rbyd) == trunk);
    LFS_ASSERT(rbyd->weight == weight);
    return 0;
}


static int lfsr_rbyd_lookupnext(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag,
        lfsr_srid_t *rid_, lfsr_tag_t *tag_, lfsr_rid_t *weight_,
        lfsr_data_t *data_) {
    // these bits should be clear at this point
    LFS_ASSERT(lfsr_tag_mode(tag) == 0);

    // make sure we never look up zero tags, the way we create
    // unreachable tags has a hole here
    tag = lfs_max16(tag, 0x1);

    // out of bounds? no trunk yet?
    if (rid >= (lfsr_srid_t)rbyd->weight || !lfsr_rbyd_trunk(rbyd)) {
        return LFS_ERR_NOENT;
    }

    // keep track of bounds as we descend down the tree
    lfs_size_t branch = lfsr_rbyd_trunk(rbyd);
    lfsr_srid_t lower_rid = 0;
    lfsr_srid_t upper_rid = rbyd->weight;

    // descend down tree
    while (true) {
        lfsr_tag_t alt;
        lfsr_rid_t weight;
        lfs_size_t jump;
        lfs_ssize_t d = lfsr_bd_readtag(lfs,
                rbyd->blocks[0], branch, 0,
                &alt, &weight, &jump, NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfsr_tag_isalt(alt)) {
            lfs_size_t branch_ = branch + d;

            // take alt?
            if (lfsr_tag_follow(
                    alt, weight,
                    lower_rid, upper_rid,
                    rid, tag)) {
                lfsr_tag_flip(
                        &alt, &weight,
                        lower_rid, upper_rid);
                branch_ = branch - jump;
            }

            lfsr_tag_trim(
                    alt, weight,
                    &lower_rid, &upper_rid,
                    NULL, NULL);
            LFS_ASSERT(branch_ != branch);
            branch = branch_;

        // found end of tree?
        } else {
            // update the tag rid
            lfsr_srid_t rid__ = upper_rid-1;
            lfsr_tag_t tag__ = lfsr_tag_key(alt);

            // not what we're looking for?
            if (!tag__
                    || rid__ < rid
                    || (rid__ == rid && tag__ < tag)) {
                return LFS_ERR_NOENT;
            }

            // save what we found
            // TODO how many of these need to be conditional?
            if (rid_) {
                *rid_ = rid__;
            }
            if (tag_) {
                *tag_ = tag__;
            }
            if (weight_) {
                *weight_ = upper_rid - lower_rid;
            }
            if (data_) {
                *data_ = LFSR_DATA_DISK(rbyd->blocks[0], branch + d, jump);
            }
            return 0;
        }
    }
}

static int lfsr_rbyd_lookup(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag,
        lfsr_data_t *data_) {
    lfsr_srid_t rid_;
    lfsr_tag_t tag_;
    int err = lfsr_rbyd_lookupnext(lfs, rbyd, rid, tag,
            &rid_, &tag_, NULL, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if (rid_ != rid || tag_ != tag) {
        return LFS_ERR_NOENT;
    }

    return 0;
}

static int lfsr_rbyd_sublookup(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // looking up a wide tag with subtype is probably a mistake
    LFS_ASSERT(lfsr_tag_subtype(tag) == 0);

    lfsr_srid_t rid_;
    lfsr_tag_t tag__;
    int err = lfsr_rbyd_lookupnext(lfs, rbyd, rid, tag,
            &rid_, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // the difference between lookup and sublookup is we accept any
    // subtype of the requested tag
    if (rid_ != rid || lfsr_tag_suptype(tag__) != tag) {
        return LFS_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}

static int lfsr_rbyd_suplookup(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_srid_t rid_;
    lfsr_tag_t tag__;
    int err = lfsr_rbyd_lookupnext(lfs, rbyd, rid, 0,
            &rid_, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // the difference between lookup and suplookup is we accept any tag
    if (rid_ != rid) {
        return LFS_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}



// append a revision count
//
// this is optional, if not called revision count defaults to 0 (for btrees)
static int lfsr_rbyd_appendrev(lfs_t *lfs, lfsr_rbyd_t *rbyd, uint32_t rev) {
    // should only be called before any tags are written
    LFS_ASSERT(rbyd->eoff == 0);
    LFS_ASSERT(rbyd->cksum == 0);

    // revision count stored as le32, we don't use a leb128 encoding as we
    // intentionally allow the revision count to overflow
    uint8_t rev_buf[sizeof(uint32_t)];
    lfs_tole32_(rev, &rev_buf);

    uint32_t cksum_ = rbyd->cksum;
    int err = lfsr_bd_prog(lfs, rbyd->blocks[0], lfsr_rbyd_eoff(rbyd),
            &rev_buf, sizeof(uint32_t),
            &cksum_);
    if (err) {
        return err;
    }

    // update eoff, xor cksum parity
    rbyd->eoff
            += ((lfs_size_t)lfs_parity(rbyd->cksum ^ cksum_)
                << (8*sizeof(lfs_size_t)-1))
            + sizeof(uint32_t);
    rbyd->cksum = cksum_;
    return 0;
}

// other low-level appends
static int lfsr_rbyd_appendtag(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_tag_t tag, lfsr_rid_t weight, lfs_size_t size) {
    // include the previous tag parity
    tag ^= (lfsr_tag_t)lfsr_rbyd_parity(rbyd) << 15;

    uint32_t cksum_ = rbyd->cksum;
    lfs_ssize_t d = lfsr_bd_progtag(lfs,
            rbyd->blocks[0], lfsr_rbyd_eoff(rbyd),
            tag, weight, size,
            &cksum_);
    if (d < 0) {
        return d;
    }

    // update eoff, xor cksum parity
    rbyd->eoff
            += ((lfs_size_t)lfs_parity(rbyd->cksum ^ cksum_)
                << (8*sizeof(lfs_size_t)-1))
            + d;
    rbyd->cksum = cksum_;
    return 0;
}

static int lfsr_rbyd_appendcat(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        const void *cat, int16_t count) {
    uint32_t cksum_ = rbyd->cksum;
    int err = lfsr_bd_progcat(lfs, rbyd->blocks[0], lfsr_rbyd_eoff(rbyd),
            cat, count,
            &cksum_);
    if (err) {
        return err;
    }

    // update eoff, xor cksum parity
    rbyd->eoff
            += ((lfs_size_t)lfs_parity(rbyd->cksum ^ cksum_)
                << (8*sizeof(lfs_size_t)-1))
            + lfsr_cat_size(cat, count);
    rbyd->cksum = cksum_;
    return 0;
}

static int lfsr_rbyd_appendattr_(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_attr_t attr) {
    int err = lfsr_rbyd_appendtag(lfs, rbyd,
            attr.tag, attr.weight, lfsr_attr_size(attr));
    if (err) {
        return err;
    }

    err = lfsr_rbyd_appendcat(lfs, rbyd, attr.cat, attr.count);
    if (err) {
        return err;
    }

    return 0;
}

// checks before we append
static int lfsr_rbyd_prepareappend(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));

    // we can't do anything if we're not erased
    if (lfsr_rbyd_eoff(rbyd) >= lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    // make sure every rbyd starts with a revision count
    if (rbyd->eoff == 0) {
        int err = lfsr_rbyd_appendrev(lfs, rbyd, 0);
        if (err) {
            return err;
        }
    }

    return 0;
}

// helper functions for managing the 3-element fifo used in
// lfsr_rbyd_appendattr
typedef struct lfsr_alt {
    lfsr_tag_t alt;
    lfsr_rid_t weight;
    lfs_size_t jump;
} lfsr_alt_t;

static int lfsr_rbyd_p_flush(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_alt_t p[static 3],
        int count) {
    // write out some number of alt pointers in our queue
    for (int i = 0; i < count; i++) {
        if (p[3-1-i].alt) {
            // change to a relative jump at the last minute
            lfsr_tag_t alt = p[3-1-i].alt;
            lfsr_rid_t weight = p[3-1-i].weight;
            lfs_size_t jump = (p[3-1-i].jump)
                    ? lfsr_rbyd_eoff(rbyd) - p[3-1-i].jump
                    : 0;

            int err = lfsr_rbyd_appendtag(lfs, rbyd, alt, weight, jump);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

static inline int lfsr_rbyd_p_push(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_alt_t p[static 3],
        lfsr_tag_t alt, lfsr_rid_t weight, lfs_size_t jump) {
    int err = lfsr_rbyd_p_flush(lfs, rbyd, p, 1);
    if (err) {
        return err;
    }

    lfs_memmove(p+1, p, 2*sizeof(lfsr_alt_t));
    p[0].alt = alt;
    p[0].weight = weight;
    p[0].jump = jump;
    return 0;
}

static inline void lfsr_rbyd_p_pop(
        lfsr_alt_t p[static 3]) {
    lfs_memmove(p, p+1, 2*sizeof(lfsr_alt_t));
    p[2].alt = 0;
}

static void lfsr_rbyd_p_recolor(
        lfsr_alt_t p[static 3]) {
    // propagate a red edge upwards
    p[0].alt &= ~LFSR_TAG_R;

    if (p[1].alt) {
        p[1].alt |= LFSR_TAG_R;

        // alt-never? we can prune this now
        if (lfsr_tag_isn(p[1].alt)) {
            p[1] = p[2];
            p[2].alt = 0;

        // reorder so that top two edges always go in the same direction
        } else if (lfsr_tag_isred(p[2].alt)) {
            if (lfsr_tag_isparallel(p[1].alt, p[2].alt)) {
                // no reorder needed
            } else if (lfsr_tag_isparallel(p[0].alt, p[2].alt)) {
                lfsr_tag_t alt_ = p[1].alt;
                lfsr_rid_t weight_ = p[1].weight;
                lfs_size_t jump_ = p[1].jump;
                p[1].alt = p[0].alt | LFSR_TAG_R;
                p[1].weight = p[0].weight;
                p[1].jump = p[0].jump;
                p[0].alt = alt_ & ~LFSR_TAG_R;
                p[0].weight = weight_;
                p[0].jump = jump_;
            } else if (lfsr_tag_isparallel(p[0].alt, p[1].alt)) {
                lfsr_tag_t alt_ = p[2].alt;
                lfsr_rid_t weight_ = p[2].weight;
                lfs_size_t jump_ = p[2].jump;
                p[2].alt = p[1].alt | LFSR_TAG_R;
                p[2].weight = p[1].weight;
                p[2].jump = p[1].jump;
                p[1].alt = p[0].alt | LFSR_TAG_R;
                p[1].weight = p[0].weight;
                p[1].jump = p[0].jump;
                p[0].alt = alt_ & ~LFSR_TAG_R;
                p[0].weight = weight_;
                p[0].jump = jump_;
            } else {
                LFS_UNREACHABLE();
            }
        }
    }
}

// core rbyd algorithm
static int lfsr_rbyd_appendattr(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_attr_t attr) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));
    // tag must not be internal at this point
    LFS_ASSERT(!lfsr_tag_isinternal(attr.tag));
    // bit 7 is reserved for future subtype extensions
    LFS_ASSERT(!(attr.tag & 0x80));
    // you can't delete more than what's in the rbyd
    LFS_ASSERT(attr.weight >= -(lfsr_srid_t)rbyd->weight);

    // ignore noops
    if (lfsr_attr_isnoop(attr)) {
        return 0;
    }

    // begin appending
    int err = lfsr_rbyd_prepareappend(lfs, rbyd);
    if (err) {
        return err;
    }

    // figure out what range of tags we're operating on
    lfsr_srid_t a_rid;
    lfsr_srid_t b_rid;
    lfsr_tag_t a_tag;
    lfsr_tag_t b_tag;
    if (!lfsr_tag_isgrow(attr.tag) && attr.weight != 0) {
        if (attr.weight > 0) {
            LFS_ASSERT(rid <= (lfsr_srid_t)rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid -= 1;
            a_rid = rid + 1;
            b_rid = rid + 1;
        } else {
            LFS_ASSERT(rid < (lfsr_srid_t)rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid += 1;
            a_rid = rid - lfs_smax32(-attr.weight, 0);
            b_rid = rid;
        }

        a_tag = 0;
        b_tag = 0;

    } else {
        LFS_ASSERT(rid < (lfsr_srid_t)rbyd->weight);

        a_rid = rid - lfs_smax32(-attr.weight, 0);
        b_rid = rid;

        // note both normal and rm wide-tags have the same bounds, really it's
        // the normal non-wide-tags that are an outlier here
        if (lfsr_tag_issup(attr.tag)) {
            a_tag = 0x000;
            b_tag = 0xf00;
        } else if (lfsr_tag_issub(attr.tag)) {
            a_tag = lfsr_tag_supkey(attr.tag);
            b_tag = lfsr_tag_supkey(attr.tag) + 0x100;
        } else if (lfsr_tag_isrm(attr.tag)) {
            a_tag = lfsr_tag_key(attr.tag);
            b_tag = lfsr_tag_key(attr.tag) + 1;
        } else {
            a_tag = lfsr_tag_key(attr.tag);
            b_tag = lfsr_tag_key(attr.tag);
        }
    }
    a_tag = lfs_max16(a_tag, 0x1);
    b_tag = lfs_max16(b_tag, 0x1);

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
    lfsr_srid_t d_rid = 0;
    lfsr_tag_t d_tag = 0;

    // follow the current trunk
    lfs_size_t branch = lfsr_rbyd_trunk(rbyd);

trunk:;
    // the new trunk starts here
    lfs_size_t trunk_ = lfsr_rbyd_eoff(rbyd);

    // keep track of bounds as we descend down the tree
    //
    // this gets a bit confusing as we also may need to keep
    // track of both the lower and upper bounds of diverging paths
    // in the case of range deletions
    lfsr_srid_t lower_rid = 0;
    lfsr_srid_t upper_rid = rbyd->weight;
    lfsr_tag_t lower_tag = 0x000;
    lfsr_tag_t upper_tag = 0xf00;

    // no trunk yet?
    if (!branch) {
        goto leaf;
    }

    // queue of pending alts we can emulate rotations with
    lfsr_alt_t p[3] = {{0}, {0}, {0}};
    // keep track of the last incoming branch for yellow splits
    lfs_size_t y_branch = 0;
    // keep track of the tag we find at the end of the trunk
    lfsr_tag_t tag_ = 0;

    // descend down tree, building alt pointers
    while (true) {
        // keep track of incoming branch
        if (lfsr_tag_isblack(p[0].alt)) {
            y_branch = branch;
        }

        // read the alt pointer
        lfsr_tag_t alt;
        lfsr_rid_t weight;
        lfs_size_t jump;
        lfs_ssize_t d = lfsr_bd_readtag(lfs,
                rbyd->blocks[0], branch, 0,
                &alt, &weight, &jump, NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfsr_tag_isalt(alt)) {
            // make jump absolute
            jump = branch - jump;
            lfs_size_t branch_ = branch + d;

            // yellow alts should be parallel
            LFS_ASSERT(!(lfsr_tag_isred(alt) && lfsr_tag_isred(p[0].alt))
                    || lfsr_tag_isparallel(alt, p[0].alt));

            // take black alt? needs a flip
            //   <b           >b
            // .-'|  =>     .-'|
            // 1  2      1  2  1
            if (lfsr_tag_follow2(
                    alt, weight,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    a_rid, a_tag)) {
                lfsr_tag_flip2(
                        &alt, &weight,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid);
                lfs_swap32(&jump, &branch_);
            }

            // should've taken red alt? needs a flip
            //      <r              >r
            // .----'|            .-'|
            // |    <b  =>        | >b
            // |  .-'|         .--|-'|
            // 1  2  3      1  2  3  1
            if (lfsr_tag_isred(p[0].alt)
                    && lfsr_tag_follow(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        a_rid, a_tag)) {
                lfs_swap16(&p[0].alt, &alt);
                lfs_swap32(&p[0].weight, &weight);
                lfs_swap32(&p[0].jump, &jump);
                alt = (alt & ~LFSR_TAG_R) | (p[0].alt & LFSR_TAG_R);
                p[0].alt |= LFSR_TAG_R;

                lfsr_tag_flip2(
                        &alt, &weight,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid);
                lfs_swap32(&jump, &branch_);
            }

            // do bounds want to take different paths? begin diverging
            //                            >b                    <b
            //                          .-'|                  .-'|
            //         <b  =>           | nb  =>             nb  |
            //    .----'|      .--------|--'      .-----------'  |
            //   <b    <b      |       <b         |             nb
            // .-'|  .-'|      |     .-'|         |        .-----'
            // 1  2  3  4      1  2  3  4  x      1  2  3  4  x  x
            bool diverging = lfsr_tag_diverging2(
                    alt, weight,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    a_rid, a_tag,
                    b_rid, b_tag);
            bool diverging_red = lfsr_tag_isred(p[0].alt)
                    && lfsr_tag_diverging(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        a_rid, a_tag,
                        b_rid, b_tag);
            if (!diverged
                    // diverging black?
                    && (lfsr_tag_isblack(alt)
                        // give up if we find a yellow alt
                        || lfsr_tag_isred(p[0].alt))
                    && (diverging || diverging_red)) {
                diverged = true;

                // both diverging? collapse
                //      <r              >b
                // .----'|            .-'|
                // |    <b  =>        |  |
                // |  .-'|      .-----|--'
                // 1  2  3      1  2  3  x
                if (diverging && diverging_red) {
                    LFS_ASSERT(a_rid < b_rid || a_tag < b_tag);
                    LFS_ASSERT(lfsr_tag_isparallel(alt, p[0].alt));

                    p[0].alt = alt | LFSR_TAG_R;
                    p[0].weight += weight;
                    weight = 0;
                }

                // diverging upper? stitch together both trunks
                //            >b                    <b
                //          .-'|                  .-'|
                //          | nb  =>             nb  |
                // .--------|--'      .-----------'  |
                // |       <b         |             nb
                // |     .-'|         |        .-----'
                // 1  2  3  4  x      1  2  3  4  x  x
                if (a_rid > b_rid || a_tag > b_tag) {
                    lfsr_tag_trim2(
                            alt, weight,
                            p[0].alt, p[0].weight,
                            &lower_rid, &upper_rid,
                            &lower_tag, &upper_tag);

                    // stitch together both trunks
                    err = lfsr_rbyd_p_push(lfs, rbyd, p,
                            LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_LE, d_tag),
                            d_rid - (lower_rid - weight),
                            jump);
                    if (err) {
                        return err;
                    }

                    // continue to next alt
                    branch = branch_;
                    continue;
                }
            // diverged?
            //    :            :
            //   <b  =>       nb
            // .-'|         .--'
            // 3  4      3  4  x
            } else if (diverged && diverging) {
                // trim so alt is pruned
                lfsr_tag_trim(
                        alt, weight,
                        &lower_rid, &upper_rid,
                        &lower_tag, &upper_tag);
                weight = 0;
            }

            // prune?
            //
            // note if only yellow pruning this could be much simpler

            // prune unreachable red alts
            //            <b                    >b
            //          .-'|                  .-'|
            //         <y  |                  |  |
            // .-------'|  |                  |  |
            // |       <r  |  =>              | >b
            // |  .----'   |         .--------|-'|
            // |  |       <b         |       <b  |
            // |  |  .----'|         |  .----'|  |
            // 1  2  3  4  4      1  2  3  4  4  1
            if (lfsr_tag_isred(p[0].alt)
                    && lfsr_tag_unreachable(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)) {
                alt &= ~LFSR_TAG_R;
                lfsr_rbyd_p_pop(p);
            }

            // prune other unreachable alts
            //            <b                    >b
            //          .-'|                  .-'|
            //         <y  |                  | <b
            // .-------'|  |      .-----------|-'|
            // |       <r  |  =>  |           |  |
            // |  .----'   |      |           |  |
            // |  |       <b      |          <b  |
            // |  |  .----'|      |     .----'|  |
            // 1  2  3  4  4      1  2  3  4  4  2
            if (lfsr_tag_unreachable2(
                    alt, weight,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    lower_tag, upper_tag)) {
                // prune unreachable recolorable alts
                //       :               :
                //      <r  =>          <b
                // .----'|      .-------'|
                // |    <b      |        |
                // |  .-'|      |  .-----'
                // 1  2  3      1  2  3  x
                if (lfsr_tag_isred(p[0].alt)) {
                    alt = p[0].alt & ~LFSR_TAG_R;
                    weight = p[0].weight;
                    jump = p[0].jump;
                    lfsr_rbyd_p_pop(p);

                // prune unreachable root alts and red alts
                //       :               :
                //      <r  =>          <b
                // .----'|         .----'|
                // |    <b         |     |
                // |  .-'|         |  .--'
                // 3  4  5      3  4  5  x
                } else if (!p[0].alt || lfsr_tag_isred(alt)) {
                    branch = branch_;
                    continue;

                // convert unreachable non-root black alts into alt-nevers,
                // if we prune these it would break the color balance of
                // our tree
                //    :            :
                //   <b  =>       nb
                // .-'|         .--'
                // 3  4      3  4  x
                } else {
                    alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_LE, 0);
                    weight = 0;
                    jump = 0;
                }
            }

            // two reds makes a yellow, split?
            //
            // note we've lost the original yellow edge because of flips, but
            // we know the red edge is the only branch_ > branch
            if (lfsr_tag_isred(alt) && lfsr_tag_isred(p[0].alt)) {
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
                    if (jump > branch) {
                        lfs_swap16(&p[0].alt, &alt);
                        lfs_swap32(&p[0].weight, &weight);
                        lfs_swap32(&p[0].jump, &jump);
                    }
                    alt &= ~LFSR_TAG_R;

                    lfsr_tag_trim(
                            p[0].alt, p[0].weight,
                            &lower_rid, &upper_rid,
                            &lower_tag, &upper_tag);
                    lfsr_rbyd_p_recolor(p);

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
                    LFS_ASSERT(y_branch != 0);
                    p[0].alt = alt;
                    p[0].weight += weight;
                    p[0].jump = y_branch;

                    lfsr_tag_trim(
                            p[0].alt, p[0].weight,
                            &lower_rid, &upper_rid,
                            &lower_tag, &upper_tag);
                    lfsr_rbyd_p_recolor(p);

                    branch = branch_;
                    continue;
                }
            }

            // red alt? we need to read the rest of the 2-3-4 node
            if (lfsr_tag_isred(alt)) {
                // undo flip temporarily
                if (branch_ < branch) {
                    lfsr_tag_flip2(
                            &alt, &weight,
                            p[0].alt, p[0].weight,
                            lower_rid, upper_rid);
                    lfs_swap32(&jump, &branch_);
                }

            // black alt? terminate 2-3-4 nodes
            } else {
                // trim alts from our current bounds
                lfsr_tag_trim2(
                        alt, weight,
                        p[0].alt, p[0].weight,
                        &lower_rid, &upper_rid,
                        &lower_tag, &upper_tag);
            }

            // push alt onto our queue
            err = lfsr_rbyd_p_push(lfs, rbyd, p,
                    alt, weight, jump);
            if (err) {
                return err;
            }

            // continue to next alt
            LFS_ASSERT(branch_ != branch);
            branch = branch_;
            continue;

        // found end of tree?
        } else {
            // update the found tag
            tag_ = lfsr_tag_key(alt);

            // the last alt should always end up black
            LFS_ASSERT(lfsr_tag_isblack(p[0].alt));

            if (diverged) {
                // diverged lower trunk? move on to upper trunk
                if (a_rid < b_rid || a_tag < b_tag) {
                    // keep track of the lower diverged bound
                    d_rid = lower_rid;
                    d_tag = lower_tag;

                    // flush any pending alts
                    err = lfsr_rbyd_p_flush(lfs, rbyd, p, 3);
                    if (err) {
                        return err;
                    }

                    // terminate diverged trunk with an unreachable tag
                    err = lfsr_rbyd_appendattr_(lfs, rbyd, LFSR_ATTR(
                            (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0)
                                | LFSR_TAG_NULL,
                            0,
                            LFSR_DATA_NULL()));
                    if (err) {
                        return err;
                    }

                    // swap tag/rid and move on to upper trunk
                    diverged = false;
                    branch = trunk_;
                    lfs_swap16(&a_tag, &b_tag);
                    lfs_sswap32(&a_rid, &b_rid);
                    goto trunk;

                } else {
                    // use the lower diverged bound for leaf weight
                    // calculation
                    lower_rid = d_rid;
                    lower_tag = d_tag;
                }
            }

            goto stem;
        }
    }

stem:;
    // split leaf nodes?
    //
    // note we bias the weights here so that lfsr_rbyd_lookupnext
    // always finds the next biggest tag
    //
    // note also if tag_ is null, we found a removed tag that we should just
    // prune
    //
    // this gets real messy because we have a lot of special behavior built in:
    // - default         => split if tags mismatch
    // - weight>0, !grow => split if tags mismatch or we're inserting a new tag
    // - wide-bit set    => split if suptype of tags mismatch
    // - rm-bit set      => never split, but emit alt-always tags, making our
    //                      tag effectively unreachable
    //
    lfsr_tag_t alt = 0;
    lfsr_rid_t weight = 0;
    if (tag_
            && (upper_rid-1 < rid-lfs_smax32(-attr.weight, 0)
                || (upper_rid-1 == rid-lfs_smax32(-attr.weight, 0)
                    && ((!lfsr_tag_isgrow(attr.tag) && attr.weight > 0)
                        || (!lfsr_tag_issup(attr.tag)
                            && lfsr_tag_supkey(tag_)
                                < lfsr_tag_supkey(attr.tag))
                        || (!lfsr_tag_issup(attr.tag)
                            && !lfsr_tag_issub(attr.tag)
                            && lfsr_tag_key(tag_)
                                < lfsr_tag_key(attr.tag)))))) {
        if (lfsr_tag_isrm(attr.tag) || !lfsr_tag_key(attr.tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_GT, lower_tag);
            weight = upper_rid - lower_rid + attr.weight;
            upper_rid -= weight;
        } else {
            // split less than
            alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_LE, tag_);
            weight = upper_rid - lower_rid;
            lower_rid += weight;
        }

    } else if (tag_
            && (upper_rid-1 > rid
                || (upper_rid-1 == rid
                    && ((!lfsr_tag_isgrow(attr.tag) && attr.weight > 0)
                        || (!lfsr_tag_issup(attr.tag)
                            && lfsr_tag_supkey(tag_)
                                > lfsr_tag_supkey(attr.tag))
                        || (!lfsr_tag_issup(attr.tag)
                            && !lfsr_tag_issub(attr.tag)
                            && lfsr_tag_key(tag_)
                                > lfsr_tag_key(attr.tag)))))) {
        if (lfsr_tag_isrm(attr.tag) || !lfsr_tag_key(attr.tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_GT, lower_tag);
            weight = upper_rid - lower_rid + attr.weight;
            upper_rid -= weight;
        } else {
            // split greater than
            alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_GT, attr.tag);
            weight = upper_rid - (rid+1);
            upper_rid -= weight;
        }
    }

    if (alt) {
        err = lfsr_rbyd_p_push(lfs, rbyd, p,
                alt, weight, branch);
        if (err) {
            return err;
        }

        // introduce a red edge
        lfsr_rbyd_p_recolor(p);
    }

    // flush any pending alts
    err = lfsr_rbyd_p_flush(lfs, rbyd, p, 3);
    if (err) {
        return err;
    }

leaf:;
    // write the actual tag
    //
    // note we always need a non-alt to terminate the trunk, otherwise we
    // can't find trunks during fetch
    err = lfsr_rbyd_appendattr_(lfs, rbyd, LFSR_ATTR_(
            // mark as shrub if we are a shrub
            (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0)
                // rm => null, otherwise strip off control bits
                | ((lfsr_tag_isrm(attr.tag))
                    ? LFSR_TAG_NULL
                    : lfsr_tag_key(attr.tag)),
            upper_rid - lower_rid + attr.weight,
            attr.cat, attr.count));
    if (err) {
        return err;
    }

    // update the trunk and weight
    rbyd->trunk = (rbyd->trunk & LFSR_RBYD_ISSHRUB) | trunk_;
    rbyd->weight += attr.weight;
    return 0;
}

static int lfsr_rbyd_appendcksum(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    // begin appending
    int err = lfsr_rbyd_prepareappend(lfs, rbyd);
    if (err) {
        return err;
    }

    // save the data checksum
    uint32_t cksum = rbyd->cksum;

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
    lfs_size_t off_ = lfs_alignup(
            lfsr_rbyd_eoff(rbyd) + 2+1+1+4+4 + 2+1+4+4,
            lfs->cfg->prog_size);

    // space for ecksum?
    uint8_t e = 0;
    if (off_ < lfs->cfg->block_size) {
        // read the leading byte in case we need to perturb the next tag
        err = lfsr_bd_read(lfs,
                rbyd->blocks[0], off_, lfs->cfg->prog_size,
                &e, 1);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        // calculate the erased-state checksum
        lfsr_ecksum_t ecksum;
        ecksum.cksize = lfs->cfg->prog_size;
        ecksum.cksum = 0;
        if (err != LFS_ERR_CORRUPT) {
            ecksum.cksum = lfs_crc32c(0, &e, 1);
        }
        err = lfsr_bd_cksum(lfs,
                rbyd->blocks[0], off_+1, ecksum.cksize-1,
                ecksum.cksize-1,
                &ecksum.cksum);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        uint8_t ecksum_buf[LFSR_ECKSUM_DSIZE];
        err = lfsr_rbyd_appendattr_(lfs, rbyd, LFSR_ATTR(
                LFSR_TAG_ECKSUM, 0, LFSR_DATA_ECKSUM_(&ecksum, ecksum_buf)));
        if (err) {
            return err;
        }

    // at least space for a cksum?
    } else if (lfsr_rbyd_eoff(rbyd) + 2+1+4+4 <= lfs->cfg->block_size) {
        // note this implicitly marks the rbyd as unerased
        off_ = lfs->cfg->block_size;

    // not even space for a cksum? we can't finish the commit
    } else {
        return LFS_ERR_RANGE;
    }

    // build end-of-commit cksum
    //
    // note padding-size depends on leb-encoding depends on padding-size
    // depends leb-encoding depends on... to get around this catch-22 we
    // just always write a fully-expanded leb128 encoding
    uint8_t cksum_buf[2+1+4+4];
    cksum_buf[0] = (uint8_t)(LFSR_TAG_CKSUM >> 8);
    cksum_buf[1] = (uint8_t)(LFSR_TAG_CKSUM >> 0)
            // include tag parity in the perturb bits
            | ((uint8_t)lfsr_rbyd_parity(rbyd) << 1);
    cksum_buf[2] = 0;

    lfs_size_t padding = off_ - (lfsr_rbyd_eoff(rbyd) + 2+1+4);
    cksum_buf[3] = 0x80 | (0x7f & (padding >>  0));
    cksum_buf[4] = 0x80 | (0x7f & (padding >>  7));
    cksum_buf[5] = 0x80 | (0x7f & (padding >> 14));
    cksum_buf[6] = 0x00 | (0x7f & (padding >> 21));

    // calculate checksum before tag parity
    uint32_t cksum_ = lfs_crc32c(rbyd->cksum, cksum_buf, 2+1+4);
    // xor in the tag parity
    cksum_buf[0] ^= (uint8_t)lfsr_rbyd_parity(rbyd) << 7;
    // find the new parity
    bool parity_ = lfsr_rbyd_parity(rbyd) ^ lfs_parity(rbyd->cksum ^ cksum_);
    // and intentionally perturb the commit so the next tag appears invalid
    if ((e >> 7) == parity_) {
        cksum_buf[1] ^= 0x01;
        cksum_ ^= 0xef306b19;
        parity_ ^= 0x1;
    }
    lfs_tole32_(cksum_, &cksum_buf[2+1+4]);

    err = lfsr_bd_prog(lfs, rbyd->blocks[0], lfsr_rbyd_eoff(rbyd),
            cksum_buf, 2+1+4+4,
            NULL);
    if (err) {
        return err;
    }

    // flush our caches, finalizing the commit on-disk
    err = lfsr_bd_sync(lfs);
    if (err) {
        return err;
    }

    // update the eoff and parity
    rbyd->eoff
            = ((lfs_size_t)parity_ << (8*sizeof(lfs_size_t)-1))
            | off_;
    // revert to data checksum
    rbyd->cksum = cksum;
    return 0;
}

static int lfsr_rbyd_appendattrs(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // append each tag to the tree
    for (lfs_size_t i = 0; i < attr_count; i++) {
        // treat inserts after the first tag as though they are splits,
        // sequential inserts don't really make sense otherwise
        if (i > 0 && lfsr_attr_isinsert(attrs[i])) {
            rid += 1;
        }

        // don't write tags outside of the requested range
        if (rid >= start_rid
                // note the use of rid+1 and unsigned comparison here to
                // treat end_rid=-1 as "unbounded" in such a way that rid=-1
                // is still included
                && (lfs_size_t)(rid + 1) <= (lfs_size_t)end_rid) {
            int err = lfsr_rbyd_appendattr(lfs, rbyd,
                    rid - lfs_smax32(start_rid, 0),
                    attrs[i]);
            if (err) {
                return err;
            }
        }

        // we need to make sure we keep start_rid/end_rid updated with
        // weight changes
        if (rid < start_rid) {
            start_rid += attrs[i].weight;
        }
        if (rid < end_rid) {
            end_rid += attrs[i].weight;
        }

        // adjust rid
        rid = lfsr_attr_nextrid(attrs[i], rid);
    }

    return 0;
}

static int lfsr_rbyd_commit(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // append each tag to the tree
    int err = lfsr_rbyd_appendattrs(lfs, rbyd, rid, -1, -1,
            attrs, attr_count);
    if (err) {
        return err;
    }

    // append a cksum, finalizing the commit
    err = lfsr_rbyd_appendcksum(lfs, rbyd);
    if (err) {
        return err;
    }

    return 0;
}


// Calculate the maximum possible disk usage required by this rbyd after
// compaction. This uses a conservative estimate so the actual on-disk cost
// should be smaller.
//
// This also returns a good split_rid in case the rbyd needs to be split.
//
// TODO do we need to include commit overhead here?
static lfs_ssize_t lfsr_rbyd_estimate(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        lfsr_srid_t *split_rid_) {
    // calculate dsize by starting from the outside ids and working inwards,
    // this naturally gives us a split rid
    //
    // TODO adopt this a/b naming scheme in lfsr_rbyd_appendattr?
    lfsr_srid_t a_rid = start_rid;
    lfsr_srid_t b_rid = lfs_min32(rbyd->weight, end_rid);
    lfs_size_t a_dsize = 0;
    lfs_size_t b_dsize = 0;
    lfs_size_t rbyd_dsize = 0;

    while (a_rid != b_rid) {
        if (a_dsize > b_dsize
                // bias so lower dsize >= upper dsize
                || (a_dsize == b_dsize && a_rid > b_rid)) {
            lfs_sswap32(&a_rid, &b_rid);
            lfs_swap32(&a_dsize, &b_dsize);
        }

        if (a_rid > b_rid) {
            a_rid -= 1;
        }

        lfsr_tag_t tag = 0;
        lfsr_rid_t weight = 0;
        lfs_size_t dsize_ = 0;
        while (true) {
            lfsr_srid_t rid_;
            lfsr_rid_t weight_;
            lfsr_data_t data;
            int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                    a_rid, tag+1,
                    &rid_, &tag, &weight_, &data);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                LFS_ASSERT(err < 0);
                return err;
            }
            if (rid_ > a_rid+lfs_smax32(weight_-1, 0)) {
                break;
            }

            // keep track of rid and weight
            a_rid = rid_;
            weight += weight_;

            // include the cost of this tag
            dsize_ += lfs->attr_estimate + lfsr_data_size(data);
        }

        if (a_rid == -1) {
            rbyd_dsize += dsize_;
        } else {
            a_dsize += dsize_;
        }

        if (a_rid < b_rid) {
            a_rid += 1;
        } else {
            a_rid -= lfs_smax32(weight-1, 0);
        }
    }

    if (split_rid_) {
        *split_rid_ = a_rid;
    }

    return rbyd_dsize + a_dsize + b_dsize;
}

// appends a raw tag as a part of compaction, note these must
// be appended in order!
//
// also note attr.weight here is total weight not delta weight
static int lfsr_rbyd_appendcompactattr(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_attr_t attr) {
    // begin appending
    int err = lfsr_rbyd_prepareappend(lfs, rbyd);
    if (err) {
        return err;
    }

    // write the tag
    err = lfsr_rbyd_appendattr_(lfs, rbyd, LFSR_ATTR_(
            (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0) | attr.tag,
            attr.weight,
            attr.cat, attr.count));
    if (err) {
        return err;
    }

    return 0;
}

static int lfsr_rbyd_appendcompactrbyd(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_rbyd_t *rbyd, lfsr_srid_t start_rid, lfsr_srid_t end_rid) {
    // copy over tags in the rbyd in order
    lfsr_srid_t rid = start_rid;
    lfsr_tag_t tag = 0;
    while (true) {
        lfsr_rid_t weight;
        lfsr_data_t data;
        int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                rid, tag+1,
                &rid, &tag, &weight, &data);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }
        // end of range? note the use of rid+1 and unsigned comparison here to
        // treat end_rid=-1 as "unbounded" in such a way that rid=-1 is still
        // included
        if ((lfs_size_t)(rid + 1) > (lfs_size_t)end_rid) {
            break;
        }

        // write the tag
        err = lfsr_rbyd_appendcompactattr(lfs, rbyd_, LFSR_ATTR_CAT_(
                tag, weight, &data, 1));
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_rbyd_appendcompaction(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_size_t off) {
    // begin appending
    int err = lfsr_rbyd_prepareappend(lfs, rbyd);
    if (err) {
        return err;
    }

    // clamp offset to be after the revision count
    off = lfs_max32(off, sizeof(uint32_t));

    // empty rbyd? write a null tag so our trunk can still point to something
    if (lfsr_rbyd_eoff(rbyd) == off) {
        err = lfsr_rbyd_appendtag(lfs, rbyd,
                // mark as shrub if we are a shrub
                (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0)
                    | LFSR_TAG_NULL,
                0,
                0);
        if (err) {
            return err;
        }

        rbyd->trunk = (rbyd->trunk & LFSR_RBYD_ISSHRUB) | off;
        rbyd->weight = 0;
        return 0;
    }

    // connect every other trunk together, building layers of a perfectly
    // balanced binary tree upwards until we have a single trunk
    lfs_size_t layer = off;
    lfsr_rid_t weight = 0;
    while (true) {
        lfs_size_t layer_ = lfsr_rbyd_eoff(rbyd);
        off = layer;
        while (off < layer_) {
            // connect two trunks together with a new binary trunk
            for (int i = 0; i < 2 && off < layer_; i++) {
                lfs_size_t trunk = off;
                lfsr_tag_t tag = 0;
                weight = 0;
                while (true) {
                    lfsr_tag_t tag__;
                    lfsr_rid_t weight__;
                    lfs_size_t size__;
                    lfs_ssize_t d = lfsr_bd_readtag(lfs,
                            rbyd->blocks[0], off, layer_ - off,
                            &tag__, &weight__, &size__, NULL);
                    if (d < 0) {
                        return d;
                    }
                    off += d;

                    // skip any data
                    if (!lfsr_tag_isalt(tag__)) {
                        off += size__;
                    }

                    // ignore shrub trunks, unless we are actually compacting
                    // a shrub tree
                    if (!lfsr_tag_isalt(tag__)
                            && lfsr_tag_isshrub(tag__)
                            && !lfsr_rbyd_isshrub(rbyd)) {
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
                    if (tag__ & ~LFSR_TAG_SHRUB) {
                        tag = tag__;
                    }

                    // did we hit a tag that terminates our trunk?
                    if (!lfsr_tag_isalt(tag__)) {
                        break;
                    }
                }

                // do we only have one trunk? we must be done
                if (trunk == layer && off >= layer_) {
                    goto done;
                }

                // connect with an altle
                //
                // note we can't use an altas here, we need to encode the
                // exact tag so we know the largest tag when building the
                // next layer
                err = lfsr_rbyd_appendtag(lfs, rbyd,
                        LFSR_TAG_ALT(
                            (i == 0 && off < layer_)
                                ? LFSR_TAG_R
                                : LFSR_TAG_B,
                            LFSR_TAG_LE,
                            tag),
                        weight,
                        lfsr_rbyd_eoff(rbyd) - trunk);
                if (err) {
                    return err;
                }
            }

            // terminate with a null tag
            err = lfsr_rbyd_appendtag(lfs, rbyd,
                    // mark as shrub if we are a shrub
                    (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0)
                        | LFSR_TAG_NULL,
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
    rbyd->trunk = (rbyd->trunk & LFSR_RBYD_ISSHRUB) | layer;
    rbyd->weight = weight;

    return 0;
}

static int lfsr_rbyd_compact(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_rbyd_t *rbyd, lfsr_srid_t start_rid, lfsr_srid_t end_rid) {
    // append rbyd
    int err = lfsr_rbyd_appendcompactrbyd(lfs, rbyd_,
            rbyd, start_rid, end_rid);
    if (err) {
        return err;
    }

    // compact
    err = lfsr_rbyd_appendcompaction(lfs, rbyd_, 0);
    if (err) {
        return err;
    }

    return 0;
}

// append a secondary "shrub" tree
static int lfsr_rbyd_appendshrub(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        const lfsr_shrub_t *shrub) {
    // keep track of the start of the new tree
    lfs_size_t off = lfsr_rbyd_eoff(rbyd);
    // mark as shrub
    rbyd->trunk |= LFSR_RBYD_ISSHRUB;

    // compact our shrub
    int err = lfsr_rbyd_appendcompactrbyd(lfs, rbyd,
            (const lfsr_rbyd_t*)shrub, -1, -1);
    if (err) {
        return err;
    }

    err = lfsr_rbyd_appendcompaction(lfs, rbyd, off);
    if (err) {
        return err;
    }

    return 0;
}


// some low-level name things
//
// names in littlefs are tuples of directory-ids + ascii/utf8 strings

// binary search an rbyd for a name, leaving the rid_/tag_/weight_/data_
// with the best matching name if not found
static lfs_scmp_t lfsr_rbyd_namelookup(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_did_t did, const char *name, lfs_size_t name_size,
        lfsr_srid_t *rid_,
        lfsr_tag_t *tag_, lfsr_rid_t *weight_, lfsr_data_t *data_) {
    // empty rbyd? leave it up to upper layers to handle this
    if (rbyd->weight == 0) {
        return LFS_ERR_NOENT;
    }

    // binary search for our name
    lfsr_srid_t lower_rid = 0;
    lfsr_srid_t upper_rid = rbyd->weight;
    lfs_scmp_t cmp;
    while (lower_rid < upper_rid) {
        lfsr_tag_t tag__;
        lfsr_srid_t rid__;
        lfsr_rid_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                // lookup ~middle rid, note we may end up in the middle
                // of a weighted rid with this
                lower_rid + (upper_rid-1-lower_rid)/2, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            LFS_ASSERT(err < 0);
            return err;
        }

        // if we have no name, treat this rid as always lt
        if (lfsr_tag_suptype(tag__) != LFSR_TAG_NAME) {
            cmp = LFS_CMP_LT;

        // compare names
        } else {
            cmp = lfsr_data_namecmp(lfs, data__, did, name, name_size);
            if (cmp < 0) {
                return cmp;
            }
        }

        // bisect search space
        if (cmp > LFS_CMP_EQ) {
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

        } else if (cmp < LFS_CMP_EQ) {
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
            return LFS_CMP_EQ;
        }
    }

    // no match, return if found name was lt/gt expect
    //
    // this will always be lt unless all rids are gt
    return (lower_rid == 0) ? LFS_CMP_GT : LFS_CMP_LT;
}



/// B-tree operations ///

#define LFSR_BTREE_NULL() ((lfsr_btree_t){.weight=0, .trunk=0})

// convenience operations
static inline int lfsr_btree_cmp(
        const lfsr_btree_t *a,
        const lfsr_btree_t *b) {
    return lfsr_rbyd_cmp(a, b);
}


// branch on-disk encoding

// branch encoding:
// .---+- -+- -+- -+- -.  block: 1 leb128  <=5 bytes
// | block             |  trunk: 1 leb128  <=4 bytes
// +---+- -+- -+- -+- -'  cksum: 1 le32    4 bytes
// | trunk         |      total:           <=13 bytes
// +---+- -+- -+- -+
// |     cksum     |
// '---+---+---+---'
//
#define LFSR_BRANCH_DSIZE (5+4+4)

#define LFSR_DATA_BRANCH_(_branch, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_frombranch(_branch, _buffer)}.d)

#define LFSR_DATA_BRANCH(_branch) \
    LFSR_DATA_BRANCH_(_branch, (uint8_t[LFSR_BRANCH_DSIZE]){0})

static lfsr_data_t lfsr_data_frombranch(const lfsr_rbyd_t *branch,
        uint8_t buffer[static LFSR_BRANCH_DSIZE]) {
    // block should not exceed 31-bits
    LFS_ASSERT(branch->blocks[0] <= 0x7fffffff);
    // trunk should not exceed 28-bits
    LFS_ASSERT(lfsr_rbyd_trunk(branch) <= 0x0fffffff);
    lfs_ssize_t d = 0;

    lfs_ssize_t d_ = lfs_toleb128(branch->blocks[0], &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(lfsr_rbyd_trunk(branch), &buffer[d], 4);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    lfs_tole32_(branch->cksum, &buffer[d]);
    d += 4;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readbranch(lfs_t *lfs, lfsr_data_t *data,
        lfsr_bid_t weight,
        lfsr_rbyd_t *branch) {
    // setting off to 0 here will trigger asserts if we try to append
    // without fetching first
    branch->eoff = 0;
    branch->weight = weight;

    int err = lfsr_data_readleb128(lfs, data, &branch->blocks[0]);
    if (err) {
        return err;
    }

    err = lfsr_data_readlleb128(lfs, data, &branch->trunk);
    if (err) {
        return err;
    }

    err = lfsr_data_readle32(lfs, data, &branch->cksum);
    if (err) {
        return err;
    }

    return 0;
}


// btree on-disk encoding
//
// this is the same as the branch on-disk econding, but prefixed with the
// btree's weight

// btree encoding:
// .---+- -+- -+- -+- -.  weight: 1 leb128  <=5 bytes
// | weight            |  block:  1 leb128  <=5 bytes
// +---+- -+- -+- -+- -+  trunk:  1 leb128  <=4 bytes
// | block             |  cksum:  1 le32    4 bytes
// +---+- -+- -+- -+- -'  total:            <=18 bytes
// | trunk         |
// +---+- -+- -+- -+
// |     cksum     |
// '---+---+---+---'
//
#define LFSR_BTREE_DSIZE (5+LFSR_BRANCH_DSIZE)

#define LFSR_DATA_BTREE_(_btree, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_frombtree(_btree, _buffer)}.d)

#define LFSR_DATA_BTREE(_btree) \
    LFSR_DATA_BTREE_(_btree, (uint8_t[LFSR_BTREE_DSIZE]){0})

static lfsr_data_t lfsr_data_frombtree(const lfsr_btree_t *btree,
        uint8_t buffer[static LFSR_BTREE_DSIZE]) {
    // weight should not exceed 31-bits
    LFS_ASSERT(btree->weight <= 0x7fffffff);
    lfs_ssize_t d = 0;

    lfs_ssize_t d_ = lfs_toleb128(btree->weight, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    lfsr_data_t data = lfsr_data_frombranch(btree, &buffer[d]);
    d += lfsr_data_size(data);

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readbtree(lfs_t *lfs, lfsr_data_t *data,
        lfsr_btree_t *btree) {
    lfsr_bid_t weight;
    int err = lfsr_data_readleb128(lfs, data, &weight);
    if (err) {
        return err;
    }

    err = lfsr_data_readbranch(lfs, data, weight, btree);
    if (err) {
        return err;
    }

    return 0;
}


// core btree operations

static int lfsr_btree_lookupnext_(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_rbyd_t *rbyd_, lfsr_srid_t *rid_,
        lfsr_tag_t *tag_, lfsr_bid_t *weight_, lfsr_data_t *data_) {
    // descend down the btree looking for our bid
    lfsr_rbyd_t branch = *btree;
    lfsr_srid_t rid = bid;
    while (true) {
        // each branch is a pair of optional name + on-disk structure
        lfsr_srid_t rid__;
        lfsr_tag_t tag__;
        lfsr_rid_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, &branch, rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_sublookup(lfs, &branch, rid__, LFSR_TAG_STRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFSR_TAG_BRANCH) {
            // adjust rid with subtree's weight
            rid -= (rid__ - (weight__-1));

            // fetch the next branch
            err = lfsr_data_readbranch(lfs, &data__, weight__, &branch);
            if (err) {
                return err;
            }

        // found our bid
        } else {
            // TODO how many of these should be conditional?
            if (bid_) {
                *bid_ = bid + (rid__ - rid);
            }
            if (rbyd_) {
                *rbyd_ = branch;
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
            return 0;
        }
    }
}

static int lfsr_btree_lookupnext(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bid_t *weight_,
        lfsr_data_t *data_) {
    return lfsr_btree_lookupnext_(lfs, btree, bid,
            bid_, NULL, NULL, tag_, weight_, data_);
}

static int lfsr_btree_lookup(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_bid_t bid,
        lfsr_tag_t *tag_, lfsr_bid_t *weight_, lfsr_data_t *data_) {
    lfsr_bid_t bid_;
    int err = lfsr_btree_lookupnext(lfs, btree, bid,
            &bid_, tag_, weight_, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest bid, all we need to do is fail if it
    // picks up the wrong bid
    if (bid_ != bid) {
        return LFS_ERR_NOENT;
    }

    return 0;
}

// TODO should lfsr_btree_lookupnext/lfsr_btree_parent be deduplicated?
static int lfsr_btree_parent(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_bid_t bid, const lfsr_rbyd_t *child,
        lfsr_rbyd_t *rbyd_, lfsr_srid_t *rid_) {
    // we should only call this when we actually have parents
    LFS_ASSERT(bid < (lfsr_bid_t)btree->weight);
    LFS_ASSERT(lfsr_rbyd_cmp(btree, child) != 0);

    // descend down the btree looking for our rid
    lfsr_rbyd_t branch = *btree;
    lfsr_srid_t rid = bid;
    while (true) {
        // each branch is a pair of optional name + on-disk structure
        lfsr_srid_t rid__;
        lfsr_tag_t tag__;
        lfsr_rid_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, &branch, rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_sublookup(lfs, &branch, rid__, LFSR_TAG_STRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // didn't find our child?
        if (tag__ != LFSR_TAG_BRANCH) {
            return LFS_ERR_NOENT;
        }

        // adjust rid with subtree's weight
        rid -= (rid__ - (weight__-1));

        // fetch the next branch
        lfsr_rbyd_t branch_;
        err = lfsr_data_readbranch(lfs, &data__, weight__, &branch_);
        if (err) {
            return err;
        }

        // found our child?
        if (lfsr_rbyd_cmp(&branch_, child) == 0) {
            // TODO how many of these should be conditional?
            if (rbyd_) {
                *rbyd_ = branch;
            }
            if (rid_) {
                *rid_ = rid__;
            }
            return 0;
        }

        branch = branch_;
    }
}


// extra state needed for non-terminating lfsr_btree_commit_ calls
typedef struct lfsr_btree_scratch {
    lfsr_attr_t attrs[4];
    lfsr_data_t split_data;
    uint8_t buf[2*LFSR_BRANCH_DSIZE];
} lfsr_btree_scratch_t;

// core btree algorithm
//
// this commits up to the root, but stops if:
// 1. we need a new root
// 2. we have a shrub root
//
static int lfsr_btree_commit_(lfs_t *lfs, lfsr_btree_t *btree,
        lfsr_btree_scratch_t *scratch,
        lfsr_bid_t *bid_,
        const lfsr_attr_t **attrs_, lfs_size_t *attr_count_) {
    lfsr_bid_t bid = *bid_;
    LFS_ASSERT(bid <= (lfsr_bid_t)btree->weight);
    const lfsr_attr_t *attrs = *attrs_;
    lfs_size_t attr_count = *attr_count_;

    // lookup in which leaf our bids resides
    //
    // for lfsr_btree_commit operations to work out, we need to
    // limit our bid to an rid in the tree, which is what this min
    // is doing
    lfsr_rbyd_t rbyd = *btree;
    lfsr_srid_t rid = bid;
    if (btree->weight > 0) {
        lfsr_srid_t rid_;
        int err = lfsr_btree_lookupnext_(lfs, btree,
                lfs_min32(bid, btree->weight-1),
                &bid, &rbyd, &rid_, NULL, NULL, NULL);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // adjust bid to point to the zero-most rid
        bid -= rid_;
        rid -= bid;
    }

    // tail-recursively commit to btree
    while (true) {
        // we will always need our parent, so go ahead and find it
        lfsr_rbyd_t parent = {.trunk=0, .weight=0};
        lfsr_srid_t pid = 0;
        // are we root?
        if (rbyd.blocks[0] == btree->blocks[0]
                || !lfsr_rbyd_trunk(&rbyd)) {
            // new root? shrub root? yield the final root commit to
            // higher-level btree/bshrub logic
            if (!lfsr_rbyd_trunk(&rbyd)
                    || lfsr_rbyd_isshrub(btree)) {
                *bid_ = rid;
                *attrs_ = attrs;
                *attr_count_ = attr_count;
                return (!lfsr_rbyd_trunk(&rbyd)) ? LFS_ERR_RANGE : 0;
            }

            // mark btree as unerased in case of failure, our btree rbyd and
            // root rbyd can diverge if there's a split, but we would have
            // marked the old root as unerased earlier anyways
            btree->eoff = -1;

        } else {
            int err = lfsr_btree_parent(lfs, btree, bid, &rbyd,
                    &parent, &pid);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
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
        if (!lfsr_rbyd_isfetched(&rbyd)) {
            int err = lfsr_rbyd_fetchvalidate(lfs, &rbyd,
                    rbyd.blocks[0], lfsr_rbyd_trunk(&rbyd), rbyd.weight,
                    rbyd.cksum);
            if (err) {
                return err;
            }
        }

        // is rbyd erased? can we sneak our commit into any remaining
        // erased bytes? note that the btree trunk field prevents this from
        // interacting with other references to the rbyd
        lfsr_rbyd_t rbyd_ = rbyd;
        int err = lfsr_rbyd_commit(lfs, &rbyd_, rid,
                attrs, attr_count);
        if (err) {
            if (err == LFS_ERR_RANGE || err == LFS_ERR_CORRUPT) {
                goto compact;
            }
            return err;
        }

        goto recurse;

    compact:;
        // estimate our compacted size
        lfsr_srid_t split_rid;
        lfs_ssize_t estimate = lfsr_rbyd_estimate(lfs, &rbyd, -1, -1,
                &split_rid);
        if (estimate < 0) {
            return estimate;
        }

        // are we too big? need to split?
        if ((lfs_size_t)estimate > lfs->cfg->block_size/2) {
            // need to split
            goto split;
        }

        // before we compact, can we merge with our siblings?
        lfsr_rbyd_t sibling;
        if ((lfs_size_t)estimate <= lfs->cfg->block_size/4
                // no parent? can't merge
                && lfsr_rbyd_trunk(&parent)) {
            // try the right sibling
            if (pid+1 < (lfsr_srid_t)parent.weight) {
                // try looking up the sibling
                lfsr_srid_t sibling_rid;
                lfsr_tag_t sibling_tag;
                lfsr_rid_t sibling_weight;
                lfsr_data_t sibling_data;
                err = lfsr_rbyd_lookupnext(lfs, &parent,
                        pid+1, LFSR_TAG_NAME,
                        &sibling_rid, &sibling_tag, &sibling_weight,
                        &sibling_data);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                if (sibling_tag == LFSR_TAG_NAME) {
                    err = lfsr_rbyd_sublookup(lfs, &parent,
                            sibling_rid, LFSR_TAG_STRUCT,
                            &sibling_tag, &sibling_data);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_NOENT);
                        return err;
                    }
                }

                LFS_ASSERT(sibling_tag == LFSR_TAG_BRANCH);
                err = lfsr_data_readbranch(lfs, &sibling_data, sibling_weight,
                        &sibling);
                if (err) {
                    return err;
                }

                // estimate if our sibling will fit
                lfs_ssize_t sibling_estimate = lfsr_rbyd_estimate(lfs,
                        &sibling, -1, -1,
                        NULL);
                if (sibling_estimate < 0) {
                    return sibling_estimate;
                }

                // fits? try to merge
                if ((lfs_size_t)(estimate + sibling_estimate)
                        < lfs->cfg->block_size/2) {
                    goto merge;
                }
            }

            // try the left sibling
            if (pid-(lfsr_srid_t)rbyd.weight >= 0) {
                // try looking up the sibling
                lfsr_srid_t sibling_rid;
                lfsr_tag_t sibling_tag;
                lfsr_rid_t sibling_weight;
                lfsr_data_t sibling_data;
                err = lfsr_rbyd_lookupnext(lfs, &parent,
                        pid-rbyd.weight, LFSR_TAG_NAME,
                        &sibling_rid, &sibling_tag, &sibling_weight,
                        &sibling_data);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                if (sibling_tag == LFSR_TAG_NAME) {
                    err = lfsr_rbyd_sublookup(lfs, &parent,
                            sibling_rid, LFSR_TAG_STRUCT,
                            &sibling_tag, &sibling_data);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_NOENT);
                        return err;
                    }
                }

                LFS_ASSERT(sibling_tag == LFSR_TAG_BRANCH);
                err = lfsr_data_readbranch(lfs, &sibling_data, sibling_weight,
                        &sibling);
                if (err) {
                    return err;
                }

                // estimate if our sibling will fit
                lfs_ssize_t sibling_estimate = lfsr_rbyd_estimate(lfs,
                        &sibling, -1, -1,
                        NULL);
                if (sibling_estimate < 0) {
                    return sibling_estimate;
                }

                // fits? try to merge
                if ((lfs_size_t)(estimate + sibling_estimate)
                        < lfs->cfg->block_size/2) {
                    // if we're merging our left sibling, swap our rbyds
                    // so our sibling is on the right
                    bid -= sibling.weight;
                    rid += sibling.weight;
                    pid -= rbyd.weight;

                    rbyd_ = sibling;
                    sibling = rbyd;
                    rbyd = rbyd_;

                    goto merge;
                }
            }
        }

    compact_relocate:;
        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // try to compact
        err = lfsr_rbyd_compact(lfs, &rbyd_, &rbyd, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto compact_relocate;
            }
            return err;
        }

        // append any pending attrs, it's up to upper
        // layers to make sure these always fit
        err = lfsr_rbyd_commit(lfs, &rbyd_, rid,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto compact_relocate;
            }
            return err;
        }

        goto recurse;

    split:;
        // we should have something to split here
        LFS_ASSERT(split_rid > 0
                && split_rid < (lfsr_srid_t)rbyd.weight);

    split_relocate_l:;
        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // copy over tags < split_rid
        err = lfsr_rbyd_compact(lfs, &rbyd_, &rbyd, -1, split_rid);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

        // append pending attrs < split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendattrs(lfs, &rbyd_, rid, -1, split_rid,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

        // finalize commit
        err = lfsr_rbyd_appendcksum(lfs, &rbyd_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

    split_relocate_r:;
        // allocate a sibling
        err = lfsr_rbyd_alloc(lfs, &sibling);
        if (err) {
            return err;
        }

        // copy over tags >= split_rid
        err = lfsr_rbyd_compact(lfs, &sibling, &rbyd, split_rid, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }

        // append pending attrs >= split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendattrs(lfs, &sibling, rid, split_rid, -1,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }

        // finalize commit
        err = lfsr_rbyd_appendcksum(lfs, &sibling);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }

        // did one of our siblings drop to zero? yes this can happen! revert
        // to a normal commit in that case
        if (rbyd_.weight == 0 || sibling.weight == 0) {
            if (rbyd_.weight == 0) {
                rbyd_ = sibling;
            }
            goto recurse;
        }

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending attrs in case
        // they introduce a new name!
        lfsr_tag_t split_tag;
        err = lfsr_rbyd_lookupnext(lfs, &sibling, 0, LFSR_TAG_NAME,
                NULL, &split_tag, NULL, &scratch->split_data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // prepare commit to parent, tail recursing upwards
        LFS_ASSERT(rbyd_.weight > 0);
        LFS_ASSERT(sibling.weight > 0);
        attr_count = 0;
        // new root?
        if (!lfsr_rbyd_trunk(&parent)) {
            scratch->attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_BRANCH, +rbyd_.weight,
                    LFSR_DATA_BRANCH_(
                        &rbyd_,
                        &scratch->buf[0*LFSR_BRANCH_DSIZE]));
            scratch->attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_BRANCH, +sibling.weight,
                    LFSR_DATA_BRANCH_(
                        &sibling,
                        &scratch->buf[1*LFSR_BRANCH_DSIZE]));
            if (lfsr_tag_suptype(split_tag) == LFSR_TAG_NAME) {
                scratch->attrs[attr_count++] = LFSR_ATTR_CAT_(
                        LFSR_TAG_NAME, 0,
                        &scratch->split_data, 1);
            }
        // split root?
        } else {
            bid -= pid - (rbyd.weight-1);
            scratch->attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_BRANCH, 0,
                    LFSR_DATA_BRANCH_(
                        &rbyd_,
                        &scratch->buf[0*LFSR_BRANCH_DSIZE]));
            if (rbyd_.weight != rbyd.weight) {
                scratch->attrs[attr_count++] = LFSR_ATTR(
                        LFSR_TAG_GROW, -rbyd.weight + rbyd_.weight,
                        LFSR_DATA_NULL());
            }
            scratch->attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_BRANCH, +sibling.weight,
                    LFSR_DATA_BRANCH_(
                        &sibling,
                        &scratch->buf[1*LFSR_BRANCH_DSIZE]));
            if (lfsr_tag_suptype(split_tag) == LFSR_TAG_NAME) {
                scratch->attrs[attr_count++] = LFSR_ATTR_CAT_(
                        LFSR_TAG_NAME, 0,
                        &scratch->split_data, 1);
            }
        }
        attrs = scratch->attrs;

        rbyd = parent;
        rid = pid;
        continue;

    merge:;
    merge_relocate:;
        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // merge the siblings together
        err = lfsr_rbyd_appendcompactrbyd(lfs, &rbyd_, &rbyd, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        err = lfsr_rbyd_appendcompactrbyd(lfs, &rbyd_, &sibling, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        err = lfsr_rbyd_appendcompaction(lfs, &rbyd_, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        // append any pending attrs, it's up to upper
        // layers to make sure these always fit
        err = lfsr_rbyd_commit(lfs, &rbyd_, rid,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        // we must have a parent at this point, but is our parent the root
        // and is the root degenerate?
        LFS_ASSERT(lfsr_rbyd_trunk(&parent));
        if (rbyd.weight+sibling.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            *btree = rbyd_;
            *attr_count_ = 0;
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        LFS_ASSERT(rbyd_.weight > 0);
        attr_count = 0;
        bid -= pid - (rbyd.weight-1);
        scratch->attrs[attr_count++] = LFSR_ATTR(
                LFSR_TAG_RM, -sibling.weight, LFSR_DATA_NULL());
        scratch->attrs[attr_count++] = LFSR_ATTR(
                LFSR_TAG_BRANCH, 0,
                LFSR_DATA_BRANCH_(&rbyd_, scratch->buf));
        if (rbyd_.weight != rbyd.weight) {
            scratch->attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_GROW, -rbyd.weight + rbyd_.weight,
                    LFSR_DATA_NULL());
        }
        attrs = scratch->attrs;

        rbyd = parent;
        rid = pid + sibling.weight;
        continue;

    recurse:;
        // done?
        if (!lfsr_rbyd_trunk(&parent)) {
            LFS_ASSERT(bid == 0);
            *btree = rbyd_;
            *attr_count_ = 0;
            return 0;
        }

        // is our parent the root and is the root degenerate?
        if (rbyd.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            *btree = rbyd_;
            *attr_count_ = 0;
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        //
        // note that since we defer merges to compaction time, we can
        // end up removing an rbyd here
        attr_count = 0;
        bid -= pid - (rbyd.weight-1);
        if (rbyd_.weight == 0) {
            scratch->attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_RM, -rbyd.weight, LFSR_DATA_NULL());
        } else {
            scratch->attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_BRANCH, 0,
                    LFSR_DATA_BRANCH_(&rbyd_, scratch->buf));
            if (rbyd_.weight != rbyd.weight) {
                scratch->attrs[attr_count++] = LFSR_ATTR(
                        LFSR_TAG_GROW, -rbyd.weight + rbyd_.weight,
                        LFSR_DATA_NULL());
            }
        }
        attrs = scratch->attrs;

        rbyd = parent;
        rid = pid;
        continue;
    }
}

// this is atomic
static int lfsr_btree_commit(lfs_t *lfs, lfsr_btree_t *btree,
        lfsr_bid_t bid, const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // try to commit to the btree
    lfsr_btree_scratch_t scratch;
    int err = lfsr_btree_commit_(lfs, btree, &scratch,
            &bid, &attrs, &attr_count);
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }

    // needs a new root?
    if (err == LFS_ERR_RANGE) {
        LFS_ASSERT(attr_count > 0);

    relocate:;
        lfsr_rbyd_t rbyd;
        err = lfsr_rbyd_alloc(lfs, &rbyd);
        if (err) {
            return err;
        }

        err = lfsr_rbyd_commit(lfs, &rbyd, bid, attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        *btree = rbyd;
    }

    LFS_ASSERT(lfsr_rbyd_trunk(btree));
    return 0;
}

// lookup in a btree by name
static lfs_scmp_t lfsr_btree_namelookup(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_did_t did, const char *name, lfs_size_t name_size,
        lfsr_bid_t *bid_,
        lfsr_tag_t *tag_, lfsr_bid_t *weight_, lfsr_data_t *data_) {
    // an empty tree?
    if (btree->weight == 0) {
        return LFS_ERR_NOENT;
    }

    // descend down the btree looking for our name
    lfsr_rbyd_t branch = *btree;
    lfsr_bid_t bid = 0;
    while (true) {
        // lookup our name in the rbyd via binary search
        lfsr_srid_t rid__;
        lfsr_rid_t weight__;
        lfs_scmp_t cmp = lfsr_rbyd_namelookup(lfs, &branch,
                did, name, name_size,
                &rid__, NULL, &weight__, NULL);
        if (cmp < 0) {
            LFS_ASSERT(cmp != LFS_ERR_NOENT);
            return cmp;
        }

        // the name may not match exactly, but indicates which branch to follow
        lfsr_tag_t tag__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_sublookup(lfs, &branch, rid__, LFSR_TAG_STRUCT,
                &tag__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            LFS_ASSERT(err < 0);
            return err;
        }

        // found another branch
        if (tag__ == LFSR_TAG_BRANCH) {
            // update our bid
            bid += rid__ - (weight__-1);

            // fetch the next branch
            err = lfsr_data_readbranch(lfs, &data__, weight__, &branch);
            if (err) {
                LFS_ASSERT(err < 0);
                return err;
            }

        // found our rid
        } else {
            // TODO how many of these should be conditional?
            if (bid_) {
                *bid_ = bid + rid__;
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

// incremental btree traversal
//
// note this is different from iteration, iteration should use
// lfsr_btree_lookupnext, traversal includes inner btree nodes
typedef struct lfsr_btraversal {
    lfsr_bid_t bid;
    lfsr_srid_t rid;
    lfsr_rbyd_t branch;
} lfsr_btraversal_t;

#define LFSR_BTRAVERSAL() \
    ((lfsr_btraversal_t){ \
        .bid=0, \
        .rid=0, \
        .branch.trunk=0, \
        .branch.weight=0})

static int lfsr_btree_traverse_(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_btraversal_t *t,
        lfsr_bid_t *bid_, lfsr_tinfo_t *tinfo_) {
    // explicitly traverse the root even if weight=0
    if (t->branch.trunk == 0
            // unless we don't even have a root yet
            && lfsr_rbyd_trunk(btree) != 0
            // or are a shrub
            && !lfsr_rbyd_isshrub(btree)) {
        t->rid = t->bid;
        t->branch = *btree;

        // traverse the root
        if (t->rid == 0) {
            if (bid_) {
                *bid_ = btree->weight-1;
            }
            if (tinfo_) {
                tinfo_->tag = LFSR_TAG_BRANCH;
                tinfo_->u.rbyd = t->branch;
            }
            return 0;
        }
    }

    // need to restart from the root?
    if (t->rid >= (lfsr_srid_t)t->branch.weight) {
        t->rid = t->bid;
        t->branch = *btree;
    }

    // descend down the tree
    while (true) {
        lfsr_srid_t rid__;
        lfsr_tag_t tag__;
        lfsr_rid_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, &t->branch, t->rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_sublookup(lfs, &t->branch, rid__, LFSR_TAG_STRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFSR_TAG_BRANCH) {
            // adjust rid with subtree's weight
            t->rid -= (rid__ - (weight__-1));

            // fetch the next branch
            err = lfsr_data_readbranch(lfs, &data__, weight__,
                    &t->branch);
            if (err) {
                return err;
            }
            LFS_ASSERT((lfsr_bid_t)t->branch.weight == weight__);

            // return inner btree nodes if this is the first time we've
            // seen them
            if (t->rid == 0) {
                if (bid_) {
                    *bid_ = t->bid + (rid__ - t->rid);
                }
                if (tinfo_) {
                    tinfo_->tag = LFSR_TAG_BRANCH;
                    tinfo_->u.rbyd = t->branch;
                }
                return 0;
            }

        // found our bid
        } else {
            // move on to the next rid
            //
            // note this effectively traverses a full leaf without redoing
            // the btree walk
            lfsr_bid_t bid__ = t->bid + (rid__ - t->rid);
            t->bid = bid__ + 1;
            t->rid = rid__ + 1;

            if (bid_) {
                *bid_ = bid__;
            }
            if (tinfo_) {
                tinfo_->tag = tag__;
                tinfo_->u.data = data__;
            }
            return 0;
        }
    }
}

static int lfsr_btree_traverse(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_btraversal_t *t,
        lfsr_bid_t *bid_, lfsr_tinfo_t *tinfo_) {
    return lfsr_btree_traverse_(lfs, btree, t,
            bid_, tinfo_);
}



/// metadata-id things ///

#define LFSR_MID(_lfs, _bid, _rid) \
    (((_bid) & ~((1 << (_lfs)->mdir_bits)-1)) + (_rid))

static inline lfsr_sbid_t lfsr_mid_bid(const lfs_t *lfs, lfsr_smid_t mid) {
    return mid | ((1 << lfs->mdir_bits) - 1);
}

static inline lfsr_srid_t lfsr_mid_rid(const lfs_t *lfs, lfsr_smid_t mid) {
    // bit of a strange mapping, but we want to preserve mid=-1 => rid=-1
    return (mid >> (8*sizeof(lfsr_smid_t)-1))
            | (mid & ((1 << lfs->mdir_bits) - 1));
}


/// metadata-pointer things ///

// the mroot anchor, mdir 0x{0,1} is the entry point into the filesystem
#define LFSR_MPTR_MROOTANCHOR() ((const lfsr_mptr_t){{0, 1}})

static inline int lfsr_mptr_cmp(
        const lfsr_mptr_t *a,
        const lfsr_mptr_t *b) {
    // note these can be in either order
    if (lfs_max32(a->blocks[0], a->blocks[1])
            != lfs_max32(b->blocks[0], b->blocks[1])) {
        return lfs_max32(a->blocks[0], a->blocks[1])
                - lfs_max32(b->blocks[0], b->blocks[1]);
    } else {
        return lfs_min32(a->blocks[0], a->blocks[1])
                - lfs_min32(b->blocks[0], b->blocks[1]);
    }
}

static inline bool lfsr_mptr_ismrootanchor(const lfsr_mptr_t *mptr) {
    // mrootanchor is always at 0x{0,1}
    // just check that the first block is in mroot anchor range
    return mptr->blocks[0] <= 1;
}

// mptr encoding:
// .---+- -+- -+- -+- -.  blocks: 2 leb128s  <=2x5 bytes
// | block x 2         |  total:             <=10 bytes
// +                   +
// |                   |
// '---+- -+- -+- -+- -'
//
#define LFSR_MPTR_DSIZE (5+5)

#define LFSR_DATA_MPTR_(_mptr, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_frommptr(_mptr, _buffer)}.d)

#define LFSR_DATA_MPTR(_mptr) \
    LFSR_DATA_MPTR_(_mptr, (uint8_t[LFSR_MPTR_DSIZE]){0})

static lfsr_data_t lfsr_data_frommptr(const lfsr_mptr_t *mptr,
        uint8_t buffer[static LFSR_MPTR_DSIZE]) {
    // blocks should not exceed 31-bits
    LFS_ASSERT(mptr->blocks[0] <= 0x7fffffff);
    LFS_ASSERT(mptr->blocks[1] <= 0x7fffffff);

    lfs_ssize_t d = 0;
    for (int i = 0; i < 2; i++) {
        lfs_ssize_t d_ = lfs_toleb128(mptr->blocks[i], &buffer[d], 5);
        LFS_ASSERT(d_ >= 0);
        d += d_;
    }

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readmptr(lfs_t *lfs, lfsr_data_t *data,
        lfsr_mptr_t *mptr) {
    for (int i = 0; i < 2; i++) {
        int err = lfsr_data_readleb128(lfs, data, &mptr->blocks[i]);
        if (err) {
            return err;
        }
    }

    return 0;
}


// track opened mdirs to keep state in-sync
static bool lfsr_opened_isopen(lfs_t *lfs, const lfsr_opened_t *o) {
    for (lfsr_opened_t *o_ = lfs->opened; o_; o_ = o_->next) {
        if (o_ == o) {
            return true;
        }
    }

    return false;
}

static void lfsr_opened_add(lfs_t *lfs, lfsr_opened_t *o) {
    LFS_ASSERT(!lfsr_opened_isopen(lfs, o));
    o->next = lfs->opened;
    lfs->opened = o;
}

static void lfsr_opened_remove(lfs_t *lfs, lfsr_opened_t *o) {
    LFS_ASSERT(lfsr_opened_isopen(lfs, o));
    for (lfsr_opened_t **o_ = &lfs->opened; *o_; o_ = &(*o_)->next) {
        if (*o_ == o) {
            *o_ = (*o_)->next;
            break;
        }
    }
}

static bool lfsr_mid_isopen(lfs_t *lfs, lfsr_smid_t mid) {
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        // we really only care about regular open files here, all
        // others are either transient (dirs) or fake (orphans)
        if (o->type == LFS_TYPE_REG && o->mdir.mid == mid) {
            return true;
        }
    }

    return false;
}



/// shrub/sprout things ///

// needed in shrub/sprout/mdir/etc
static inline bool lfsr_bshrub_isbnull(const lfsr_bshrub_t *bshrub);
static inline bool lfsr_bshrub_isbsprout(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub);
static inline bool lfsr_bshrub_isbptr(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub);
static inline bool lfsr_bshrub_isbshrub(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub);
static inline bool lfsr_bshrub_isbtree(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub);
static inline bool lfsr_bshrub_isbnullorbsproutorbptr(
        const lfsr_bshrub_t *bshrub);
static inline bool lfsr_bshrub_isbshruborbtree(
        const lfsr_bshrub_t *bshrub);

// sprout things
static inline int lfsr_sprout_cmp(
        const lfsr_sprout_t *a,
        const lfsr_sprout_t *b) {
    // big assumption for sprouts, we convert straight to bshrubs,
    // and never leave sliced sprouts in our files, so we don't need
    // to compare the size
    LFS_ASSERT(a->u.disk.block != b->u.disk.block
            || a->u.disk.off != b->u.disk.off
            || lfsr_data_size(*a) == lfsr_data_size(*b));
    if (a->u.disk.block != b->u.disk.block) {
        return a->u.disk.block - b->u.disk.block;
    } else {
        return a->u.disk.off - b->u.disk.off;
    }
}

// these are used in mdir compaction
static lfs_ssize_t lfsr_sprout_estimate(lfs_t *lfs,
        const lfsr_sprout_t *sprout) {
    // only include the last reference
    const lfsr_sprout_t *last = NULL;
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        lfsr_file_t *file_ = (lfsr_file_t*)o;
        if (file_->o.type == LFS_TYPE_REG
                && lfsr_bshrub_isbsprout(&file_->o.mdir, &file_->bshrub)
                && lfsr_sprout_cmp(&file_->bshrub.u.bsprout, sprout) == 0) {
            last = &file_->bshrub.u.bsprout;
        }
    }
    if (last && sprout != last) {
        return 0;
    }

    return LFSR_TAG_DSIZE + lfsr_data_size(*sprout);
}

static int lfsr_sprout_compact(lfs_t *lfs, const lfsr_rbyd_t *rbyd_,
        lfsr_sprout_t *sprout_, const lfsr_sprout_t *sprout) {
    // this gets a bit weird, since upper layers need to do the actual
    // compaction, we just update internal state here

    // this is a bit tricky since we don't know the tag size,
    // but we have just enough info
    lfsr_sprout_t sprout__ = LFSR_DATA_DISK(
            rbyd_->blocks[0],
            rbyd_->eoff - lfsr_data_size(*sprout),
            lfsr_data_size(*sprout));

    // stage any opened inlined files with their new location so we
    // can update these later if our commit is a success
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        lfsr_file_t *file_ = (lfsr_file_t*)o;
        if (file_->o.type == LFS_TYPE_REG
                && lfsr_bshrub_isbsprout(&file_->o.mdir, &file_->bshrub)
                && lfsr_sprout_cmp(
                    &file_->bshrub.u.bsprout,
                    sprout) == 0) {
            file_->bshrub_.u.bsprout = sprout__;
        }
    }

    *sprout_ = sprout__;
    return 0;
}


// shrub things

#define LFSR_SHRUB_NULL(_block) \
    ((lfsr_shrub_t){ \
        .weight=0, \
        .blocks[0]=_block, \
        .trunk=LFSR_RBYD_ISSHRUB | 0, \
        /* force estimate recalculation */ \
        .estimate=-1})

// helper functions
static inline bool lfsr_shrub_isshrub(const lfsr_shrub_t *shrub) {
    return lfsr_rbyd_isshrub((const lfsr_rbyd_t*)shrub);
}

static inline lfs_size_t lfsr_shrub_trunk(const lfsr_shrub_t *shrub) {
    return lfsr_rbyd_trunk((const lfsr_rbyd_t*)shrub);
}

static inline int lfsr_shrub_cmp(
        const lfsr_shrub_t *a,
        const lfsr_shrub_t *b) {
    return lfsr_rbyd_cmp(
            (const lfsr_rbyd_t*)a,
            (const lfsr_rbyd_t*)b);
}

// shrub on-disk encoding

// shrub encoding:
// .---+- -+- -+- -+- -.  weight: 1 leb128  <=5 bytes
// | weight            |  trunk:  1 leb128  <=4 bytes
// +---+- -+- -+- -+- -'  total:            <=9 bytes
// | trunk         |
// '---+- -+- -+- -'
//
#define LFSR_SHRUB_DSIZE (5+4)

#define LFSR_DATA_SHRUB_(_rbyd, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromshrub(_rbyd, _buffer)}.d)

#define LFSR_DATA_SHRUB(_rbyd) \
    LFSR_DATA_SHRUB_(_rbyd, (uint8_t[LFSR_SHRUB_DSIZE]){0})

static lfsr_data_t lfsr_data_fromshrub(const lfsr_shrub_t *shrub,
        uint8_t buffer[static LFSR_SHRUB_DSIZE]) {
    // shrub trunks should never be null
    LFS_ASSERT(lfsr_shrub_trunk(shrub) != 0);
    // weight should not exceed 31-bits
    LFS_ASSERT(shrub->weight <= 0x7fffffff);
    // trunk should not exceed 28-bits
    LFS_ASSERT(lfsr_shrub_trunk(shrub) <= 0x0fffffff);
    lfs_ssize_t d = 0;

    // just write the trunk and weight, the rest of the rbyd is contextual
    lfs_ssize_t d_ = lfs_toleb128(shrub->weight, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(lfsr_shrub_trunk(shrub),
            &buffer[d], 4);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readshrub(lfs_t *lfs, lfsr_data_t *data,
        const lfsr_mdir_t *mdir,
        lfsr_shrub_t *shrub) {
    // copy the mdir block
    shrub->blocks[0] = mdir->rbyd.blocks[0];
    // force estimate recalculation if we write to this shrub
    shrub->estimate = -1;

    int err = lfsr_data_readleb128(lfs, data, &shrub->weight);
    if (err) {
        return err;
    }

    err = lfsr_data_readlleb128(lfs, data, &shrub->trunk);
    if (err) {
        return err;
    }
    // shrub trunks should never be null
    LFS_ASSERT(lfsr_shrub_trunk(shrub));

    // set the shrub bit in our trunk
    shrub->trunk |= LFSR_RBYD_ISSHRUB;
    return 0;
}

// these are used in mdir commit/compaction
static lfs_ssize_t lfsr_shrub_estimate(lfs_t *lfs,
        const lfsr_shrub_t *shrub) {
    // only include the last reference
    const lfsr_shrub_t *last = NULL;
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        lfsr_file_t *file_ = (lfsr_file_t*)o;
        if (file_->o.type == LFS_TYPE_REG
                && lfsr_bshrub_isbshrub(&file_->o.mdir, &file_->bshrub)
                && lfsr_shrub_cmp(&file_->bshrub.u.bshrub, shrub) == 0) {
            last = &file_->bshrub.u.bshrub;
        }
    }
    if (last && shrub != last) {
        return 0;
    }

    return lfsr_rbyd_estimate(lfs, (const lfsr_rbyd_t*)shrub, -1, -1,
            NULL);
}

static int lfsr_shrub_compact(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        lfsr_shrub_t *shrub_, const lfsr_shrub_t *shrub) {
    // save our current trunk/weight
    lfs_size_t trunk = rbyd_->trunk;
    lfsr_srid_t weight = rbyd_->weight;

    // compact our bshrub
    int err = lfsr_rbyd_appendshrub(lfs, rbyd_, shrub);
    if (err) {
        return err;
    }

    // stage any opened shrubs with their new location so we can
    // update these later if our commit is a success
    //
    // this should include our current bshrub
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        lfsr_file_t *file_ = (lfsr_file_t*)o;
        if (file_->o.type == LFS_TYPE_REG
                && lfsr_bshrub_isbshrub(&file_->o.mdir, &file_->bshrub)
                && lfsr_shrub_cmp(&file_->bshrub.u.bshrub, shrub) == 0) {
            file_->bshrub_.u.bshrub.blocks[0] = rbyd_->blocks[0];
            file_->bshrub_.u.bshrub.trunk = rbyd_->trunk;
            file_->bshrub_.u.bshrub.weight = rbyd_->weight;
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

// this is needed to sneak shrub commits into mdir commits
struct lfsr_shrubcommit {
    lfsr_shrub_t *shrub;
    lfsr_srid_t rid;
    const lfsr_attr_t *attrs;
    lfs_size_t attr_count;
};

static int lfsr_shrub_commit(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        lfsr_shrub_t *shrub, lfsr_srid_t rid,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // swap out our trunk/weight temporarily, note we're
    // operating on a copy so if this fails we shouldn't mess
    // things up too much
    //
    // it is important that these rbyds share eoff/cksum/etc
    lfs_size_t trunk = rbyd_->trunk;
    lfsr_srid_t weight = rbyd_->weight;
    rbyd_->trunk = shrub->trunk;
    rbyd_->weight = shrub->weight;

    // append any bshrub attributes
    int err = lfsr_rbyd_appendattrs(lfs, rbyd_, rid, -1, -1,
            attrs, attr_count);
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


/// Global-state things ///

static inline bool lfsr_gdelta_iszero(
        const uint8_t *gdelta, lfs_size_t size) {
    return lfs_memcchr(gdelta, 0, size) == NULL;
}

static inline lfs_size_t lfsr_gdelta_size(
        const uint8_t *gdelta, lfs_size_t size) {
    // truncate based on number of trailing zeros
    while (size > 0 && gdelta[size-1] == 0) {
        size -= 1;
    }

    return size;
}

static inline void lfsr_gdelta_xor(
        uint8_t *a, const uint8_t *b, lfs_size_t size) {
    lfs_memxor(a, b, size);
}


// grm (global remove) things
static inline bool lfsr_grm_hasrm(const lfsr_grm_t *grm) {
    return grm->mids[0] != -1;
}

static inline uint8_t lfsr_grm_count(const lfsr_grm_t *grm) {
    return (grm->mids[0] != -1) + (grm->mids[1] != -1);
}

static inline void lfsr_grm_push(lfsr_grm_t *grm, lfsr_smid_t mid) {
    LFS_ASSERT(grm->mids[1] == -1);
    grm->mids[1] = grm->mids[0];
    grm->mids[0] = mid;
}

static inline void lfsr_grm_pop(lfsr_grm_t *grm) {
    grm->mids[0] = grm->mids[1];
    grm->mids[1] = -1;
}

static inline bool lfsr_grm_ispending(const lfsr_grm_t *grm,
        lfsr_smid_t mid) {
    return grm->mids[0] == mid || grm->mids[1] == mid;
}

#define LFSR_DATA_GRM_(_grm, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromgrm(_grm, _buffer)}.d)

#define LFSR_DATA_GRM(_grm) \
    LFSR_DATA_GRM_(_grm, (uint8_t[LFSR_GRM_DSIZE]){0})

static lfsr_data_t lfsr_data_fromgrm(const lfsr_grm_t *grm,
        uint8_t buffer[static LFSR_GRM_DSIZE]) {
    // make sure to zero so we don't leak any info
    lfs_memset(buffer, 0, LFSR_GRM_DSIZE);

    // first encode the number of grms, this can be 0, 1, or 2 and may
    // be extended to a general purpose leb128 type field in the future
    uint8_t mode = lfsr_grm_count(grm);
    lfs_ssize_t d = 0;
    buffer[d] = mode;
    d += 1;

    for (uint8_t i = 0; i < mode; i++) {
        lfs_ssize_t d_ = lfs_toleb128(grm->mids[i], &buffer[d], 5);
        LFS_ASSERT(d_ >= 0);
        d += d_;
    }

    return LFSR_DATA_BUF(buffer, lfsr_gdelta_size(buffer, LFSR_GRM_DSIZE));
}

// required by lfsr_data_readgrm
static inline lfsr_mid_t lfsr_mtree_weight(const lfsr_mtree_t *mtree);

static int lfsr_data_readgrm(lfs_t *lfs, lfsr_data_t *data,
        lfsr_grm_t *grm) {
    // clear first
    grm->mids[0] = -1;
    grm->mids[1] = -1;

    // first read the mode field
    uint8_t mode;
    lfs_ssize_t d = lfsr_data_read(lfs, data, &mode, 1);
    if (d < 0) {
        return d;
    }
    LFS_ASSERT(d == 1);

    // unknown mode? return an error, we may be able to mount read-only
    if (mode > 2) {
        return LFS_ERR_INVAL;
    }

    for (uint8_t i = 0; i < mode; i++) {
        int err = lfsr_data_readleb128(lfs, data, (lfsr_mid_t*)&grm->mids[i]);
        if (err) {
            return err;
        }
        LFS_ASSERT((lfsr_mid_t)grm->mids[i] < lfs_max32(
                lfsr_mtree_weight(&lfs->mtree),
                1 << lfs->mdir_bits));
    }

    return 0;
}

// some mdir-related gstate things we need
static void lfsr_fs_flushgdelta(lfs_t *lfs) {
    lfs_memset(lfs->grm_d, 0, LFSR_GRM_DSIZE);
}

static void lfsr_fs_preparegdelta(lfs_t *lfs) {
    // first flush everything
    lfsr_fs_flushgdelta(lfs);

    // any pending grms?
    lfsr_data_fromgrm(&lfs->grm, lfs->grm_d);

    // xor with current gstate to find our initial gdelta
    lfsr_gdelta_xor(lfs->grm_d, lfs->grm_p, LFSR_GRM_DSIZE);
}

static void lfsr_fs_revertgdelta(lfs_t *lfs) {
    // revert gstate to on-disk state
    int err = lfsr_data_readgrm(lfs,
            &LFSR_DATA_BUF(lfs->grm_p, LFSR_GRM_DSIZE),
            &lfs->grm);
    LFS_ASSERT(!err);
}

static void lfsr_fs_commitgdelta(lfs_t *lfs) {
    // commit any pending gdeltas
    lfsr_data_fromgrm(&lfs->grm, lfs->grm_p);
}

// append and consume any pending gstate
static int lfsr_rbyd_appendgdelta(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    // need grm delta?
    if (!lfsr_gdelta_iszero(lfs->grm_d, LFSR_GRM_DSIZE)) {
        // make sure to xor any existing delta
        lfsr_data_t data;
        int err = lfsr_rbyd_lookup(lfs, rbyd, -1, LFSR_TAG_GRMDELTA,
                &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        uint8_t grm_d[LFSR_GRM_DSIZE];
        lfs_memset(grm_d, 0, LFSR_GRM_DSIZE);
        if (err != LFS_ERR_NOENT) {
            lfs_ssize_t d = lfsr_data_read(lfs, &data, grm_d, LFSR_GRM_DSIZE);
            if (d < 0) {
                return d;
            }
        }

        lfsr_gdelta_xor(grm_d, lfs->grm_d, LFSR_GRM_DSIZE);

        // append to our rbyd, replacing any existing delta
        lfs_size_t size = lfsr_gdelta_size(grm_d, LFSR_GRM_DSIZE);
        err = lfsr_rbyd_appendattr(lfs, rbyd, -1, LFSR_ATTR(
                // opportunistically remove this tag if delta is all zero
                (size == 0)
                    ? LFSR_TAG_RM | LFSR_TAG_GRMDELTA
                    : LFSR_TAG_GRMDELTA, 0,
                LFSR_DATA_BUF(grm_d, size)));
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_fs_consumegdelta(lfs_t *lfs, const lfsr_mdir_t *mdir) {
    // consume any grm deltas
    lfsr_data_t data;
    int err = lfsr_rbyd_lookup(lfs, &mdir->rbyd, -1, LFSR_TAG_GRMDELTA,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    if (err != LFS_ERR_NOENT) {
        uint8_t grm_d[LFSR_GRM_DSIZE];
        lfs_ssize_t d = lfsr_data_read(lfs, &data, grm_d, LFSR_GRM_DSIZE);
        if (d < 0) {
            return d;
        }

        lfsr_gdelta_xor(lfs->grm_d, grm_d, d);
    }

    return 0;
}


/// Revision count things ///

// in mdirs, our revision count is broken down into three parts:
//
//   vvvvrrrr rrrrrrnn nnnnnnnn nnnnnnnn
//   '-.''----.----''---------.--------'
//     '------|---------------|---------- 4-bit relocation revision
//            '---------------|---------- recycle-bits recycle counter
//                            '---------- pseudorandom nonce

static inline uint32_t lfsr_rev_init(lfs_t *lfs, uint32_t rev) {
    // we really only care about the top revision bits here
    rev &= ~((1 << 28)-1);
    // increment revision
    rev += 1 << 28;
    // xor in a pseudorandom nonce
    rev ^= ((1 << (28-lfs_smax32(lfs->recycle_bits, 0)))-1) & lfs->seed;
    return rev;
}

static inline bool lfsr_rev_needsrelocation(lfs_t *lfs, uint32_t rev) {
    if (lfs->recycle_bits == -1) {
        return false;
    }

    // does out recycle counter overflow?
    uint32_t rev_ = rev + (1 << (28-lfs_smax32(lfs->recycle_bits, 0)));
    return (rev_ >> 28) != (rev >> 28);
}

static inline uint32_t lfsr_rev_inc(lfs_t *lfs, uint32_t rev) {
    // increment recycle counter/revision
    rev += 1 << (28-lfs_smax32(lfs->recycle_bits, 0));
    // xor in a pseudorandom nonce
    rev ^= ((1 << (28-lfs_smax32(lfs->recycle_bits, 0)))-1) & lfs->seed;
    return rev;
}



/// Metadata pair stuff ///

// mdir convenience functions
static inline const lfsr_mptr_t *lfsr_mdir_mptr(const lfsr_mdir_t *mdir) {
    return (const lfsr_mptr_t*)mdir->rbyd.blocks;
}

static inline int lfsr_mdir_cmp(const lfsr_mdir_t *a, const lfsr_mdir_t *b) {
    return lfsr_mptr_cmp(lfsr_mdir_mptr(a), lfsr_mdir_mptr(b));
}

static inline bool lfsr_mdir_ismrootanchor(const lfsr_mdir_t *mdir) {
    return lfsr_mptr_ismrootanchor(lfsr_mdir_mptr(mdir));
}

// mdir operations
static int lfsr_mdir_fetch(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfsr_smid_t mid, const lfsr_mptr_t *mptr) {
    // create a copy of blocks, this is so we can swap the blocks
    // to keep track of the current revision, this also prevents issues
    // if blocks points to the blocks in the mdir
    lfs_block_t blocks_[2] = {mptr->blocks[0], mptr->blocks[1]};
    // read both revision counts, try to figure out which block
    // has the most recent revision
    uint32_t revs[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        int err = lfsr_bd_read(lfs, blocks_[0], 0, 0,
                &revs[0], sizeof(uint32_t));
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        revs[i] = lfs_fromle32_(&revs[i]);

        if (i == 0
                || err == LFS_ERR_CORRUPT
                || lfs_scmp(revs[1], revs[0]) > 0) {
            lfs_swap32(&blocks_[0], &blocks_[1]);
            lfs_swap32(&revs[0], &revs[1]);
        }
    }

    // try to fetch rbyds in the order of most recent to least recent
    for (int i = 0; i < 2; i++) {
        int err = lfsr_rbyd_fetch(lfs, &mdir->rbyd, blocks_[0], 0);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        if (err != LFS_ERR_CORRUPT) {
            mdir->mid = mid;
            // keep track of other block for compactions
            mdir->rbyd.blocks[1] = blocks_[1];
            return 0;
        }

        lfs_swap32(&blocks_[0], &blocks_[1]);
        lfs_swap32(&revs[0], &revs[1]);
    }

    // could not find a non-corrupt rbyd
    return LFS_ERR_CORRUPT;
}

static int lfsr_mdir_lookupnext(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_srid_t rid__;
    lfsr_tag_t tag__;
    int err = lfsr_rbyd_lookupnext(lfs, &mdir->rbyd,
            lfsr_mid_rid(lfs, mdir->mid), tag,
            &rid__, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // this is very similar to lfsr_rbyd_lookupnext, but we error if
    // lookupnext would change mids
    if (rid__ != lfsr_mid_rid(lfs, mdir->mid)) {
        return LFS_ERR_NOENT;
    }

    // intercept pending grms here and pretend they're orphaned files
    //
    // fortunately pending grms/orphaned files have roughly the same
    // semantics, and it's easier to manage the implied mid gap in
    // higher-levels
    if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME
            && lfsr_grm_ispending(&lfs->grm, mdir->mid)) {
        tag__ = LFSR_TAG_ORPHAN;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}

static int lfsr_mdir_lookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag,
        lfsr_data_t *data_) {
    lfsr_tag_t tag_;
    int err = lfsr_mdir_lookupnext(lfs, mdir, tag,
            &tag_, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if (tag_ != tag) {
        return LFS_ERR_NOENT;
    }

    return 0;
}

static int lfsr_mdir_sublookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // looking up a wide tag with subtype is probably a mistake
    LFS_ASSERT(lfsr_tag_subtype(tag) == 0);

    lfsr_tag_t tag__;
    int err = lfsr_mdir_lookupnext(lfs, mdir, tag,
            &tag__, data_);
    if (err) {
        return err;
    }

    // the difference between lookup and sublookup is we accept any
    // subtype of the requested tag
    if (lfsr_tag_suptype(tag__) != tag) {
        return LFS_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}

static int lfsr_mdir_suplookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_tag_t tag__;
    int err = lfsr_mdir_lookupnext(lfs, mdir, 0,
            &tag__, data_);
    if (err) {
        return err;
    }

    // the difference between lookup and sublookup is we accept any tag
    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}



/// Metadata-tree things ///

// the mtree is the core tree of mdirs in littlefs

#define LFSR_MTREE_ISMPTR 0x80000000

#define LFSR_MTREE_NULL() ((lfsr_mtree_t){ \
    .u.weight=(LFSR_MTREE_ISMPTR | 0)})

#define LFSR_MTREE_MPTR(_mptr, _weight) ((lfsr_mtree_t){ \
    .u.mptr.weight=(LFSR_MTREE_ISMPTR | (_weight)), \
    .u.mptr.mptr=_mptr})

static inline bool lfsr_mtree_isnull(const lfsr_mtree_t *mtree) {
    return mtree->u.weight == (LFSR_MTREE_ISMPTR | 0);
}

static inline bool lfsr_mtree_ismptr(const lfsr_mtree_t *mtree) {
    return mtree->u.weight & LFSR_MTREE_ISMPTR;
}

static inline bool lfsr_mtree_isbtree(const lfsr_mtree_t *mtree) {
    return !(mtree->u.weight & LFSR_MTREE_ISMPTR);
}

static inline lfsr_mid_t lfsr_mtree_weight(const lfsr_mtree_t *mtree) {
    return mtree->u.weight & ~LFSR_MTREE_ISMPTR;
}

static inline int lfsr_mtree_cmp(
        const lfsr_mtree_t *a,
        const lfsr_mtree_t *b) {
    if (a->u.weight != b->u.weight) {
        return a->u.weight - b->u.weight;
    } else if (lfsr_mtree_isnull(a)) {
        return 0;
    } else if (lfsr_mtree_ismptr(a)) {
        return lfsr_mptr_cmp(&a->u.mptr.mptr, &b->u.mptr.mptr);
    } else {
        return lfsr_btree_cmp(&a->u.btree, &b->u.btree);
    }
}

static int lfsr_mtree_lookup(lfs_t *lfs, const lfsr_mtree_t *mtree,
        lfsr_smid_t mid,
        lfsr_mdir_t *mdir_) {
    // looking up mroot?
    if (lfsr_mtree_isnull(mtree)) {
        LFS_ASSERT(mid >= 0);
        LFS_ASSERT(mid < (1 << lfs->mdir_bits));
        mdir_->mid = mid;
        mdir_->rbyd = lfs->mroot.rbyd;
        return 0;

    // looking up direct mdir?
    } else if (lfsr_mtree_ismptr(mtree)) {
        LFS_ASSERT(mid >= 0);
        LFS_ASSERT(mid < (1 << lfs->mdir_bits));

        // fetch mdir
        return lfsr_mdir_fetch(lfs, mdir_, mid, &mtree->u.mptr.mptr);

    // look up mdir in actual mtree
    } else {
        LFS_ASSERT(mid >= 0);
        LFS_ASSERT(mid < (lfsr_smid_t)lfsr_mtree_weight(mtree));
        lfsr_bid_t bid;
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_btree_lookupnext(lfs, &mtree->u.btree,
                mid,
                &bid, &tag, NULL, &data);
        if (err) {
            return err;
        }
        LFS_ASSERT((lfsr_sbid_t)bid == lfsr_mid_bid(lfs, mid));
        LFS_ASSERT(tag == LFSR_TAG_MDIR);

        // decode mdir
        lfsr_mptr_t mptr;
        err = lfsr_data_readmptr(lfs, &data, &mptr);
        if (err) {
            return err;
        }

        // fetch mdir
        return lfsr_mdir_fetch(lfs, mdir_, mid, &mptr);
    }
}

static int lfsr_mtree_seek(lfs_t *lfs, const lfsr_mtree_t *mtree,
        lfsr_mdir_t *mdir, lfs_off_t off) {
    // upper layers should handle removed mdirs
    LFS_ASSERT(mdir->mid >= 0);

    while (true) {
        // calculate new mid, be careful to avoid rid overflow
        lfsr_bid_t bid = lfsr_mid_bid(lfs, mdir->mid);
        lfsr_srid_t rid = lfsr_mid_rid(lfs, mdir->mid) + off;
        // lookup mdirs until we find our rid, we need to do this because
        // we don't know how many rids are in each mdir until we fetch
        while (rid >= (lfsr_srid_t)mdir->rbyd.weight) {
            // end of mtree?
            if (bid+(1 << lfs->mdir_bits) >= lfsr_mtree_weight(mtree)) {
                // if we hit the end of the mtree, park the mdir so all future
                // seeks return noent
                mdir->mid = bid + (1 << lfs->mdir_bits);
                return LFS_ERR_NOENT;
            }

            bid += (1 << lfs->mdir_bits);
            rid -= mdir->rbyd.weight;
            int err = lfsr_mtree_lookup(lfs, mtree, bid, mdir);
            if (err) {
                return err;
            }
        }

        mdir->mid = LFSR_MID(lfs, bid, rid);
        return 0;
    }
}


/// Mdir commit logic ///

// this is the gooey atomic center of littlefs
//
// any mutation must go through lfsr_mdir_commit to persist on disk
//
// this makes lfsr_mdir_commit also responsible for propagating changes
// up through the mtree/mroot chain, and through any internal structures,
// making lfsr_mdir_commit quite involved and a bit of a mess.

// low-level mdir operations needed by lfsr_mdir_commit
static int lfsr_mdir_alloc__(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfsr_smid_t mid, bool all) {
    // assign the mid
    mdir->mid = mid;

    if (all) {
        // allocate one block without an erase
        int err = lfs_alloc(lfs, &mdir->rbyd.blocks[1], false);
        if (err) {
            return err;
        }
    }

    // read the new revision count
    //
    // we use whatever is on-disk to avoid needing to rewrite the
    // redund block
    uint32_t rev;
    int err = lfsr_bd_read(lfs, mdir->rbyd.blocks[1], 0, 0,
            &rev, sizeof(uint32_t));
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    rev = (err != LFS_ERR_CORRUPT) ? lfs_fromle32_(&rev) : 0;
    // reset recycle bits in revision count and increment
    rev = lfsr_rev_init(lfs, rev);

relocate:;
    // allocate another block with an erase
    err = lfs_alloc(lfs, &mdir->rbyd.blocks[0], true);
    if (err) {
        return err;
    }
    mdir->rbyd.weight = 0;
    mdir->rbyd.trunk = 0;
    mdir->rbyd.eoff = 0;
    mdir->rbyd.cksum = 0;

    // write our revision count
    err = lfsr_rbyd_appendrev(lfs, &mdir->rbyd, rev);
    if (err) {
        // bad prog? try another block
        if (err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    return 0;
}

static int lfsr_mdir_swap__(lfs_t *lfs, lfsr_mdir_t *mdir_,
        const lfsr_mdir_t *mdir, bool force) {
    // assign the mid
    mdir_->mid = mdir->mid;

    // first thing we need to do is read our current revision count
    uint32_t rev;
    int err = lfsr_bd_read(lfs, mdir->rbyd.blocks[0], 0, 0,
            &rev, sizeof(uint32_t));
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    rev = (err != LFS_ERR_CORRUPT) ? lfs_fromle32_(&rev) : 0;
    // increment our revision count
    rev = lfsr_rev_inc(lfs, rev);

    // decide if we need to relocate
    if (!force && lfsr_rev_needsrelocation(lfs, rev)) {
        return LFS_ERR_NOSPC;
    }

    // swap our blocks
    mdir_->rbyd.blocks[0] = mdir->rbyd.blocks[1];
    mdir_->rbyd.blocks[1] = mdir->rbyd.blocks[0];
    mdir_->rbyd.weight = 0;
    mdir_->rbyd.trunk = 0;
    mdir_->rbyd.eoff = 0;
    mdir_->rbyd.cksum = 0;

    // erase, preparing for compact
    err = lfsr_bd_erase(lfs, mdir_->rbyd.blocks[0]);
    if (err) {
        return err;
    }

    // increment our revision count and write it to our rbyd
    err = lfsr_rbyd_appendrev(lfs, &mdir_->rbyd, rev);
    if (err) {
        return err;
    }

    return 0;
}

// low-level mdir commit, does not handle mtree/mlist/compaction/etc
static int lfsr_mdir_commit__(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        lfsr_smid_t mid, const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // try to append a commit
    lfsr_rbyd_t rbyd_ = mdir->rbyd;
    // mark as erased in case of failure
    mdir->rbyd.eoff = -1;

    // since we only ever commit to one mid or split, we can ignore the
    // entire attr-list if our mid is out of range
    lfsr_srid_t rid = lfsr_mid_rid(lfs, mid);
    if (rid >= start_rid
            // note the use of rid+1 and unsigned comparison here to
            // treat end_rid=-1 as "unbounded" in such a way that rid=-1
            // is still included
            && (lfs_size_t)(rid + 1) <= (lfs_size_t)end_rid) {

        for (lfs_size_t i = 0; i < attr_count; i++) {
            // we just happen to never split in an mdir commit
            LFS_ASSERT(!(i > 0 && lfsr_attr_isinsert(attrs[i])));

            // move tags copy over any tags associated with the source's rid
            // TODO can this be deduplicated with lfsr_mdir_compact__ more?
            // it _really_ wants to be deduplicated
            if (attrs[i].tag == LFSR_TAG_MOVE) {
                // weighted moves are not supported
                LFS_ASSERT(attrs[i].weight == 0);
                const lfsr_mdir_t *mdir__ = attrs[i].cat;

                // skip the name tag, this is always replaced by upper layers
                lfsr_tag_t tag = LFSR_TAG_STRUCT-1;
                while (true) {
                    lfsr_data_t data;
                    int err = lfsr_mdir_lookupnext(lfs, mdir__, tag+1,
                            &tag, &data);
                    if (err) {
                        if (err == LFS_ERR_NOENT) {
                            break;
                        }
                        return err;
                    }

                    // found an inlined sprout? we can just copy this like
                    // normal but we need to update any opened inlined files
                    if (tag == LFSR_TAG_DATA) {
                        err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                                rid - lfs_smax32(start_rid, 0),
                                LFSR_ATTR_CAT_(tag, 0, &data, 1));
                        if (err) {
                            return err;
                        }

                        err = lfsr_sprout_compact(lfs, &rbyd_, &data,
                                &data);
                        if (err) {
                            return err;
                        }

                    // found an inlined shrub? we need to compact the shrub
                    // as well to bring it along with us
                    } else if (tag == LFSR_TAG_BSHRUB) {
                        lfsr_shrub_t shrub;
                        err = lfsr_data_readshrub(lfs, &data, mdir__,
                                &shrub);
                        if (err) {
                            return err;
                        }

                        // compact our bshrub
                        err = lfsr_shrub_compact(lfs, &rbyd_, &shrub,
                                &shrub);
                        if (err) {
                            return err;
                        }

                        // write our new shrub tag
                        uint8_t shrub_buf[LFSR_SHRUB_DSIZE];
                        err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                                rid - lfs_smax32(start_rid, 0),
                                LFSR_ATTR(
                                    LFSR_TAG_BSHRUB, 0,
                                    LFSR_DATA_SHRUB_(&shrub, shrub_buf)));
                        if (err) {
                            return err;
                        }

                    // append the attr
                    } else {
                        err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                                rid - lfs_smax32(start_rid, 0),
                                LFSR_ATTR_CAT_(tag, 0, &data, 1));
                        if (err) {
                            return err;
                        }
                    }
                }

                // we're not quite done! we also need to bring over any
                // unsynced files
                for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
                    lfsr_file_t *file = (lfsr_file_t*)o;
                    // belongs to our mid?
                    if (file->o.type != LFS_TYPE_REG
                            || file->o.mdir.mid != mdir__->mid) {
                        continue;
                    }

                    // inlined sprout?
                    if (lfsr_bshrub_isbsprout(&file->o.mdir, &file->bshrub)
                            // only compact once, first compact should stage
                            // the new block
                            && file->bshrub_.u.bsprout.u.disk.block
                                != rbyd_.blocks[0]) {
                        int err = lfsr_rbyd_appendcompactattr(lfs, &rbyd_,
                                LFSR_ATTR_CAT_(
                                    LFSR_TAG_SHRUB | LFSR_TAG_DATA, 0,
                                    &file->bshrub.u.bsprout, 1));
                        if (err) {
                            return err;
                        }

                        err = lfsr_sprout_compact(lfs, &rbyd_,
                                &file->bshrub_.u.bsprout,
                                &file->bshrub.u.bsprout);
                        if (err) {
                            return err;
                        }

                    // inlined shrub?
                    } else if (lfsr_bshrub_isbshrub(
                                &file->o.mdir, &file->bshrub)
                            // only compact once, first compact should stage
                            // the new block
                            && file->bshrub_.u.bshrub.blocks[0]
                                != rbyd_.blocks[0]) {
                        int err = lfsr_shrub_compact(lfs, &rbyd_,
                                &file->bshrub_.u.bshrub,
                                &file->bshrub.u.bshrub);
                        if (err) {
                            return err;
                        }
                    }
                }

            // shrub tags append a set of attributes to an unrelated trunk
            // in our rbyd
            } else if (attrs[i].tag == LFSR_TAG_SHRUBALLOC
                    || attrs[i].tag == LFSR_TAG_SHRUBCOMMIT) {
                const lfsr_shrubcommit_t *bshrubcommit = attrs[i].cat;

                // SHRUBALLOC is roughly the same as SHRUBCOMMIT but also
                // resets the shrub, we need to do this here so bshrub root
                // extensions are atomic
                if (attrs[i].tag == LFSR_TAG_SHRUBALLOC) {
                    bshrubcommit->shrub->blocks[0] = rbyd_.blocks[0];
                    bshrubcommit->shrub->trunk = LFSR_RBYD_ISSHRUB | 0;
                    bshrubcommit->shrub->weight = 0;
                }

                int err = lfsr_shrub_commit(lfs, &rbyd_,
                        bshrubcommit->shrub,
                        bshrubcommit->rid,
                        bshrubcommit->attrs,
                        bshrubcommit->attr_count);
                if (err) {
                    return err;
                }

            // lazily encode inlined trunks in case they change underneath
            // us due to mdir compactions
            //
            // TODO should we preserve mode for all of these?
            // TODO should we do the same for sprouts?
            } else if (lfsr_tag_key(attrs[i].tag) == LFSR_TAG_SHRUBTRUNK) {
                const lfsr_shrub_t *shrub = attrs[i].cat;

                uint8_t shrub_buf[LFSR_SHRUB_DSIZE];
                int err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                        rid - lfs_smax32(start_rid, 0),
                        LFSR_ATTR(
                            lfsr_tag_mode(attrs[i].tag) | LFSR_TAG_BSHRUB,
                            attrs[i].weight,
                            // note we use the staged trunk here
                            LFSR_DATA_SHRUB_(shrub, shrub_buf)));
                if (err) {
                    return err;
                }

            // write out normal tags normally
            } else {
                LFS_ASSERT(!lfsr_tag_isinternal(attrs[i].tag));

                int err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                        rid - lfs_smax32(start_rid, 0),
                        attrs[i]);
                if (err) {
                    return err;
                }
            }

            // adjust rid
            rid = lfsr_attr_nextrid(attrs[i], rid);
        }
    }

    // abort the commit if our weight dropped to zero!
    //
    // If we finish the commit it becomes immediately visible, but we really
    // need to atomically remove this mdir from the mtree. Leave the actual
    // remove up to upper layers.
    if (rbyd_.weight == 0
            // unless we are an mroot
            && !(mdir->mid == -1 || lfsr_mdir_cmp(mdir, &lfs->mroot) == 0)) {
        // mark weight as zero, but note! we can no longer read from this mdir
        // as our pcache may be clobbered
        mdir->rbyd.weight = 0;
        return LFS_ERR_NOENT;
    }

    // append any gstate?
    if (start_rid == -1) {
        int err = lfsr_rbyd_appendgdelta(lfs, &rbyd_);
        if (err) {
            return err;
        }
    }

    // finalize commit
    int err = lfsr_rbyd_appendcksum(lfs, &rbyd_);
    if (err) {
        return err;
    }

    // success? flush gstate?
    if (start_rid == -1) {
        lfsr_fs_flushgdelta(lfs);
    }

    mdir->rbyd = rbyd_;
    return 0;
}

// TODO do we need to include commit overhead here?
static lfs_ssize_t lfsr_mdir_estimate__(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        lfsr_srid_t *split_rid_) {
    // yet another function that is just begging to be deduplicated, but we
    // can't because it would be recursive
    //
    // this is basically the same as lfsr_rbyd_estimate, except we assume all
    // rids have weight 1 and have extra handling for opened files, shrubs, etc

    // calculate dsize by starting from the outside ids and working inwards,
    // this naturally gives us a split rid
    lfsr_srid_t a_rid = start_rid;
    lfsr_srid_t b_rid = lfs_min32(mdir->rbyd.weight, end_rid);
    lfs_size_t a_dsize = 0;
    lfs_size_t b_dsize = 0;
    lfs_size_t mdir_dsize = 0;

    while (a_rid != b_rid) {
        if (a_dsize > b_dsize
                // bias so lower dsize >= upper dsize
                || (a_dsize == b_dsize && a_rid > b_rid)) {
            lfs_sswap32(&a_rid, &b_rid);
            lfs_swap32(&a_dsize, &b_dsize);
        }

        if (a_rid > b_rid) {
            a_rid -= 1;
        }

        lfsr_tag_t tag = 0;
        lfs_size_t dsize_ = 0;
        while (true) {
            lfsr_srid_t rid_;
            lfsr_data_t data;
            int err = lfsr_rbyd_lookupnext(lfs, &mdir->rbyd,
                    a_rid, tag+1,
                    &rid_, &tag, NULL, &data);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                LFS_ASSERT(err < 0);
                return err;
            }
            if (rid_ != a_rid) {
                break;
            }

            // special handling for sprouts, just to avoid duplicate cost
            if (tag == LFSR_TAG_DATA) {
                lfs_ssize_t dsize__ = lfsr_sprout_estimate(lfs, &data);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += lfs->attr_estimate + dsize__;

            // special handling for shrub trunks, we need to include the
            // compacted cost of the shrub in our estimate
            //
            // this is what would make lfsr_rbyd_estimate recursive, and
            // why we need a second function...
            //
            } else if (tag == LFSR_TAG_BSHRUB) {
                // include the cost of this trunk
                dsize_ += LFSR_SHRUB_DSIZE;

                lfsr_shrub_t shrub;
                err = lfsr_data_readshrub(lfs, &data, mdir, &shrub);
                if (err) {
                    LFS_ASSERT(err < 0);
                    return err;
                }

                lfs_ssize_t dsize__ = lfsr_shrub_estimate(lfs, &shrub);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += lfs->attr_estimate + dsize__;

            } else {
                // include the cost of this tag
                dsize_ += lfs->attr_estimate + lfsr_data_size(data);
            }
        }

        // include any opened+unsynced inlined files
        //
        // this is O(n^2), but littlefs is unlikely to have many open
        // files, I suppose if this becomes a problem we could sort
        // opened files by mid
        for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
            lfsr_file_t *file = (lfsr_file_t*)o;
            // belongs to our mdir + rid?
            if (file->o.type != LFS_TYPE_REG
                    || lfsr_mdir_cmp(&file->o.mdir, mdir) != 0
                    || lfsr_mid_rid(lfs, file->o.mdir.mid) != a_rid) {
                continue;
            }

            // inlined sprout?
            if (lfsr_bshrub_isbsprout(&file->o.mdir, &file->bshrub)) {
                lfs_ssize_t dsize__ = lfsr_sprout_estimate(lfs,
                        &file->bshrub.u.bsprout);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += dsize__;

            // inlined shrub?
            } else if (lfsr_bshrub_isbshrub(&file->o.mdir, &file->bshrub)) {
                lfs_ssize_t dsize__ = lfsr_shrub_estimate(lfs,
                        &file->bshrub.u.bshrub);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += dsize__;
            }
        }

        if (a_rid == -1) {
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

static int lfsr_mdir_compact__(lfs_t *lfs, lfsr_mdir_t *mdir_,
        const lfsr_mdir_t *mdir, lfsr_srid_t start_rid, lfsr_srid_t end_rid) {
    // this is basically the same as lfsr_rbyd_compact, but with special
    // handling for inlined trees.
    //
    // it's really tempting to deduplicate this via recursion! but we
    // can't do that here
    //
    // TODO this true?
    // note that any inlined updates here depend on the pre-commit state
    // (btree), not the staged state (btree_), this is important,
    // we can't trust btree_ after a failed commit

    // copy over tags in the rbyd in order
    lfsr_srid_t rid = start_rid;
    lfsr_tag_t tag = 0;
    while (true) {
        lfsr_rid_t weight;
        lfsr_data_t data;
        int err = lfsr_rbyd_lookupnext(lfs, &mdir->rbyd,
                rid, tag+1,
                &rid, &tag, &weight, &data);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }
        // end of range? note the use of rid+1 and unsigned comparison here to
        // treat end_rid=-1 as "unbounded" in such a way that rid=-1 is still
        // included
        if ((lfs_size_t)(rid + 1) > (lfs_size_t)end_rid) {
            break;
        }

        // found an inlined sprout? we can just copy this like normal but
        // we need to update any opened inlined files
        if (tag == LFSR_TAG_DATA) {
            err = lfsr_rbyd_appendcompactattr(lfs, &mdir_->rbyd,
                    LFSR_ATTR_CAT_(tag, weight, &data, 1));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

            err = lfsr_sprout_compact(lfs, &mdir_->rbyd, &data,
                    &data);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

        // found an inlined shrub? we need to compact the shrub as well to
        // bring it along with us
        } else if (tag == LFSR_TAG_BSHRUB) {
            lfsr_shrub_t shrub;
            err = lfsr_data_readshrub(lfs, &data, mdir,
                    &shrub);
            if (err) {
                return err;
            }

            // compact our shrub
            err = lfsr_shrub_compact(lfs, &mdir_->rbyd, &shrub,
                    &shrub);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

            // write the new shrub tag
            uint8_t shrub_buf[LFSR_SHRUB_DSIZE];
            err = lfsr_rbyd_appendcompactattr(lfs, &mdir_->rbyd,
                    LFSR_ATTR(
                        tag, weight,
                        LFSR_DATA_SHRUB_(&shrub, shrub_buf)));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

        } else {
            // write the tag
            err = lfsr_rbyd_appendcompactattr(lfs, &mdir_->rbyd,
                    LFSR_ATTR_CAT_(tag, weight, &data, 1));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }
        }
    }

    int err = lfsr_rbyd_appendcompaction(lfs, &mdir_->rbyd, 0);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // we're not quite done! we also need to bring over any unsynced files
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        lfsr_file_t *file = (lfsr_file_t*)o;
        // belongs to our mdir?
        if (file->o.type != LFS_TYPE_REG
                || lfsr_mdir_cmp(&file->o.mdir, mdir) != 0
                || lfsr_mid_rid(lfs, file->o.mdir.mid) < start_rid
                || (lfsr_rid_t)lfsr_mid_rid(lfs, file->o.mdir.mid)
                    >= (lfsr_rid_t)end_rid) {
            continue;
        }

        // inlined sprout?
        if (lfsr_bshrub_isbsprout(&file->o.mdir, &file->bshrub)
                // only compact once, first compact should stage the new block
                && file->bshrub_.u.bsprout.u.disk.block
                    != mdir_->rbyd.blocks[0]) {
            err = lfsr_rbyd_appendcompactattr(lfs, &mdir_->rbyd,
                    LFSR_ATTR_CAT_(
                        LFSR_TAG_SHRUB | LFSR_TAG_DATA, 0,
                        &file->bshrub.u.bsprout, 1));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

            err = lfsr_sprout_compact(lfs, &mdir_->rbyd,
                    &file->bshrub_.u.bsprout, &file->bshrub.u.bsprout);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

        // inlined shrub?
        } else if (lfsr_bshrub_isbshrub(&file->o.mdir, &file->bshrub)
                // only compact once, first compact should stage the new block
                && file->bshrub_.u.bshrub.blocks[0]
                    != mdir_->rbyd.blocks[0]) {
            err = lfsr_shrub_compact(lfs, &mdir_->rbyd,
                    &file->bshrub_.u.bshrub, &file->bshrub.u.bshrub);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }
        }
    }

    return 0;
}

// mid-level mdir commit, this one will at least compact on overflow
static int lfsr_mdir_commit_(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        lfsr_srid_t *split_rid_,
        lfsr_smid_t mid, const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // try to commit
    int err = lfsr_mdir_commit__(lfs, mdir, start_rid, end_rid,
            mid, attrs, attr_count);
    if (err) {
        if (err == LFS_ERR_RANGE || err == LFS_ERR_CORRUPT) {
            goto compact;
        }
        return err;
    }

    return 0;

compact:;
    // can't commit, try to compact

    // check if we're within our compaction threshold
    lfs_ssize_t estimate = lfsr_mdir_estimate__(lfs, mdir, start_rid, end_rid,
            split_rid_);
    if (estimate < 0) {
        return estimate;
    }

    // TODO do we need to include mdir commit overhead here? in rbyd_estimate?
    if ((lfs_size_t)estimate > lfs->cfg->block_size/2) {
        return LFS_ERR_RANGE;
    }

    // swap blocks, increment revision count
    lfsr_mdir_t mdir_;
    err = lfsr_mdir_swap__(lfs, &mdir_, mdir, false);
    if (err && err != LFS_ERR_NOSPC
            && err != LFS_ERR_CORRUPT) {
        return err;
    }

    bool overcompactable = (err != LFS_ERR_CORRUPT);
    bool all = true;
relocate:;
    // relocate? bad prog? ok, try allocating a new mdir
    if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
        err = lfsr_mdir_alloc__(lfs, &mdir_, mdir->mid, all);
        if (err && !(err == LFS_ERR_NOSPC && overcompactable)) {
            return err;
        }
        all = false;

        // no more blocks? wear-leveling falls apart here, but we can try
        // without relocating
        if (err == LFS_ERR_NOSPC) {
            LFS_WARN("Overcompacting mdir %"PRId32" "
                    "0x{%"PRIx32",%"PRIx32"}",
                    mdir->mid >> lfs->mdir_bits,
                    mdir->rbyd.blocks[0], mdir->rbyd.blocks[1]);
            overcompactable = false;

            err = lfsr_mdir_swap__(lfs, &mdir_, mdir, true);
            if (err) {
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }
        }
    }

    // compact our mdir
    err = lfsr_mdir_compact__(lfs, &mdir_, mdir, start_rid, end_rid);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        // bad prog? try another block
        if (err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    // update mdir, we need to propagate mdir changes if commit fails
    *mdir = mdir_;

    // now try to commit again
    //
    // upper layers should make sure this can't fail by limiting the
    // maximum commit size
    err = lfsr_mdir_commit__(lfs, mdir, start_rid, end_rid,
            mid, attrs, attr_count);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        // bad prog? try another block
        if (err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    return 0;
}

static int lfsr_mroot_parent(lfs_t *lfs, const lfsr_mptr_t *mptr,
        lfsr_mdir_t *mparent_) {
    // we only call this when we actually have parents
    LFS_ASSERT(!lfsr_mptr_ismrootanchor(mptr));

    // scan list of mroots for our requested pair
    lfsr_mptr_t mptr_ = LFSR_MPTR_MROOTANCHOR();
    while (true) {
        // fetch next possible superblock
        lfsr_mdir_t mdir;
        int err = lfsr_mdir_fetch(lfs, &mdir, -1, &mptr_);
        if (err) {
            return err;
        }

        // lookup next mroot
        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_MROOT,
                &data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // decode mdir
        err = lfsr_data_readmptr(lfs, &data, &mptr_);
        if (err) {
            return err;
        }

        // found our child?
        if (lfsr_mptr_cmp(&mptr_, mptr) == 0) {
            *mparent_ = mdir;
            return 0;
        }
    }
}

// high-level mdir commit
//
// this is atomic and updates any opened mdirs, lfs_t, etc
//
// note that if an error occurs, any gstate is reverted to the on-disk
// state
//
static int lfsr_mdir_commit(lfs_t *lfs, lfsr_mdir_t *mdir,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // non-mroot mdirs must have weight
    LFS_ASSERT(lfsr_mdir_cmp(mdir, &lfs->mroot) == 0
            || mdir->rbyd.weight > 0);
    // rid in-bounds?
    LFS_ASSERT(lfsr_mid_rid(lfs, mdir->mid)
            <= (lfsr_srid_t)mdir->rbyd.weight);
    // lfs->mroot must have mid=-1
    LFS_ASSERT(lfs->mroot.mid == -1);

    // play out any attrs that affect our grm _before_ committing to disk,
    // keep in mind we revert to on-disk gstate if we run into an error
    lfsr_smid_t mid_ = mdir->mid;
    for (lfs_size_t i = 0; i < attr_count; i++) {
        // automatically create grms for new bookmarks
        if (attrs[i].tag == LFSR_TAG_BOOKMARK) {
            lfsr_grm_push(&lfs->grm, mid_);

        // adjust pending grms?
        } else {
            for (int j = 0; j < 2; j++) {
                if (lfsr_mid_bid(lfs, lfs->grm.mids[j])
                            == lfsr_mid_bid(lfs, mid_)
                        && lfs->grm.mids[j] >= mid_) {
                    // deleting a pending grm doesn't really make sense
                    LFS_ASSERT(lfs->grm.mids[j] >= mid_ - attrs[i].weight);

                    // adjust the grm
                    lfs->grm.mids[j] += attrs[i].weight;
                }
            }
        }

        // adjust mid
        mid_ = lfsr_attr_nextrid(attrs[i], mid_);
    }

    // setup any pending gdeltas
    lfsr_fs_preparegdelta(lfs);

    // create a copy
    lfsr_mdir_t mdir_[2];
    mdir_[0] = *mdir;
    // mark our mdir as unerased in case we fail
    mdir->rbyd.eoff = -1;
    // mark any copies of our mdir as unerased in case we fail
    if (lfsr_mdir_cmp(mdir, &lfs->mroot) == 0) {
        lfs->mroot.rbyd.eoff = -1;
    }
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        if (lfsr_mdir_cmp(&o->mdir, mdir) == 0) {
            o->mdir.rbyd.eoff = -1;
        }

        // stage any bsprouts/bshrubs
        if (o->type == LFS_TYPE_REG) {
            lfsr_file_t *file = (lfsr_file_t*)o;
            file->bshrub_ = file->bshrub;
        }
    }

    // attempt to commit/compact the mdir normally
    lfsr_srid_t split_rid;
    int err = lfsr_mdir_commit_(lfs, &mdir_[0], -1, -1, &split_rid,
            mdir->mid, attrs, attr_count);
    if (err && err != LFS_ERR_RANGE
            && err != LFS_ERR_NOENT) {
        goto failed;
    }

    // handle possible mtree updates, this gets a bit messy
    lfsr_mdir_t mroot_ = lfs->mroot;
    if (lfsr_mdir_cmp(mdir, &lfs->mroot) == 0) {
        mroot_.rbyd = mdir_[0].rbyd;
    }
    lfsr_mtree_t mtree_ = lfs->mtree;
    lfsr_smid_t mdelta = 0;
    // need to split?
    if (err == LFS_ERR_RANGE) {
        // this should not happen unless we can't fit our mroot's metadata
        LFS_ASSERT(lfsr_mdir_cmp(mdir, &lfs->mroot) != 0
                || lfsr_mtree_isnull(&lfs->mtree));

        // if we're not the mroot, we need to consume the gstate so
        // we don't lose any info during the split
        //
        // we do this here so we don't have to worry about corner cases
        // with dropping mdirs during a split
        if (lfsr_mdir_cmp(mdir, &lfs->mroot) != 0) {
            err = lfsr_fs_consumegdelta(lfs, mdir);
            if (err) {
                goto failed;
            }
        }

        for (int i = 0; i < 2; i++) {
            // order the split compacts so that that mdir containing our mid
            // is committed last, this is a bit of a hack but necessary so
            // shrubs are staged correctly
            bool left = lfsr_mid_rid(lfs, mdir->mid) < split_rid;

            bool all = true;
        split_relocate:;
            // alloc and compact into new mdirs
            err = lfsr_mdir_alloc__(lfs, &mdir_[i^left],
                    lfs_smax32(mdir->mid, 0), all);
            if (err) {
                goto failed;
            }
            all = false;

            err = lfsr_mdir_compact__(lfs, &mdir_[i^left],
                    mdir,
                    ((i^left) == 0) ?         0 : split_rid,
                    ((i^left) == 0) ? split_rid :        -1);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto split_relocate;
                }
                goto failed;
            }

            err = lfsr_mdir_commit__(lfs, &mdir_[i^left],
                    ((i^left) == 0) ?         0 : split_rid,
                    ((i^left) == 0) ? split_rid :        -1,
                    mdir->mid, attrs, attr_count);
            if (err && err != LFS_ERR_NOENT) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto split_relocate;
                }
                goto failed;
            }
        }

        // adjust our sibling's mid after committing attrs
        mdir_[1].mid += (1 << lfs->mdir_bits);

        LFS_DEBUG("Splitting mdir %"PRId32" "
                "0x{%"PRIx32",%"PRIx32"} "
                "-> 0x{%"PRIx32",%"PRIx32"}, "
                "0x{%"PRIx32",%"PRIx32"}",
                mdir->mid >> lfs->mdir_bits,
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
                mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1],
                mdir_[1].rbyd.blocks[0], mdir_[1].rbyd.blocks[1]);

        // because of defered commits, children can be reduced to zero
        // when splitting, need to catch this here

        // both siblings reduced to zero
        if (mdir_[0].rbyd.weight == 0 && mdir_[1].rbyd.weight == 0) {
            LFS_DEBUG("Dropping mdir %"PRId32" "
                    "0x{%"PRIx32",%"PRIx32"}",
                    mdir_[0].mid >> lfs->mdir_bits,
                    mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1]);
            LFS_DEBUG("Dropping mdir %"PRId32" "
                    "0x{%"PRIx32",%"PRIx32"}",
                    mdir_[1].mid >> lfs->mdir_bits,
                    mdir_[1].rbyd.blocks[0], mdir_[1].rbyd.blocks[1]);
            goto dropped;

        // one sibling reduced to zero
        } else if (mdir_[0].rbyd.weight == 0) {
            LFS_DEBUG("Dropping mdir %"PRId32" "
                    "0x{%"PRIx32",%"PRIx32"}",
                    mdir_[0].mid >> lfs->mdir_bits,
                    mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1]);
            mdir_[0].rbyd = mdir_[1].rbyd;
            goto relocated;

        // other sibling reduced to zero
        } else if (mdir_[1].rbyd.weight == 0) {
            LFS_DEBUG("Dropping mdir %"PRId32" "
                    "0x{%"PRIx32",%"PRIx32"}",
                    mdir_[1].mid >> lfs->mdir_bits,
                    mdir_[1].rbyd.blocks[0], mdir_[1].rbyd.blocks[1]);
            goto relocated;
        }

        // no siblings reduced to zero, update our mtree
        mdelta = +(1 << lfs->mdir_bits);

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending attrs in
        // case they introduce a new name!
        lfsr_data_t split_data;
        err = lfsr_rbyd_sublookup(lfs, &mdir_[1].rbyd, 0, LFSR_TAG_NAME,
                NULL, &split_data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            goto failed;
        }

        // new mtree?
        if (lfsr_mtree_ismptr(&lfs->mtree)) {
            mtree_.u.btree = LFSR_BTREE_NULL();

            uint8_t mdir_buf[2*LFSR_MPTR_DSIZE];
            err = lfsr_btree_commit(lfs, &mtree_.u.btree,
                    0, LFSR_ATTRS(
                        LFSR_ATTR(
                            LFSR_TAG_MDIR, +(1 << lfs->mdir_bits),
                            LFSR_DATA_MPTR_(
                                lfsr_mdir_mptr(&mdir_[0]),
                                &mdir_buf[0*LFSR_MPTR_DSIZE])),
                        LFSR_ATTR_CAT_(
                            LFSR_TAG_NAME, +(1 << lfs->mdir_bits),
                            &split_data, 1),
                        LFSR_ATTR(
                            LFSR_TAG_MDIR, 0,
                            LFSR_DATA_MPTR_(
                                lfsr_mdir_mptr(&mdir_[1]),
                                &mdir_buf[1*LFSR_MPTR_DSIZE]))));
            if (err) {
                goto failed;
            }

        // update our mtree
        } else {
            // mark as unerased in case of failure
            lfs->mtree.u.btree.eoff = -1;

            uint8_t mdir_buf[2*LFSR_MPTR_DSIZE];
            err = lfsr_btree_commit(lfs, &mtree_.u.btree,
                    lfsr_mid_bid(lfs, mdir->mid), LFSR_ATTRS(
                        LFSR_ATTR(
                            LFSR_TAG_MDIR, 0,
                            LFSR_DATA_MPTR_(
                                lfsr_mdir_mptr(&mdir_[0]),
                                &mdir_buf[0*LFSR_MPTR_DSIZE])),
                        LFSR_ATTR_CAT_(
                            LFSR_TAG_NAME, +(1 << lfs->mdir_bits),
                            &split_data, 1),
                        LFSR_ATTR(
                            LFSR_TAG_MDIR, 0,
                            LFSR_DATA_MPTR_(
                                lfsr_mdir_mptr(&mdir_[1]),
                                &mdir_buf[1*LFSR_MPTR_DSIZE]))));
            if (err) {
                goto failed;
            }
        }

    // need to drop?
    } else if (err == LFS_ERR_NOENT) {
        LFS_DEBUG("Dropping mdir %"PRId32" "
                "0x{%"PRIx32",%"PRIx32"}",
                mdir->mid >> lfs->mdir_bits,
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1]);

        // consume gstate so we don't lose any info
        err = lfsr_fs_consumegdelta(lfs, mdir);
        if (err) {
            goto failed;
        }

    dropped:;
        mdelta = -(1 << lfs->mdir_bits);

        // we should never drop a direct mdir, because we always have our
        // root bookmark
        LFS_ASSERT(!lfsr_mtree_ismptr(&lfs->mtree));

        // mark as unerased in case of failure
        lfs->mtree.u.btree.eoff = -1;

        // update our mtree
        err = lfsr_btree_commit(lfs, &mtree_.u.btree,
                lfsr_mid_bid(lfs, mdir->mid), LFSR_ATTRS(
                    LFSR_ATTR(
                        LFSR_TAG_RM, -(1 << lfs->mdir_bits),
                        LFSR_DATA_NULL())));
        if (err) {
            goto failed;
        }

    // need to relocate?
    } else if (lfsr_mdir_cmp(&mdir_[0], mdir) != 0
            && lfsr_mdir_cmp(mdir, &lfs->mroot) != 0) {
        LFS_DEBUG("Relocating mdir %"PRId32" "
                "0x{%"PRIx32",%"PRIx32"} -> 0x{%"PRIx32",%"PRIx32"}",
                mdir->mid >> lfs->mdir_bits,
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
                mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1]);

    relocated:;
        // new mtree?
        if (lfsr_mtree_ismptr(&lfs->mtree)) {
            mtree_ = LFSR_MTREE_MPTR(
                    *lfsr_mdir_mptr(&mdir_[0]),
                    1 << lfs->mdir_bits);

        } else {
            // mark as unerased in case of failure
            lfs->mtree.u.btree.eoff = -1;

            // update our mtree
            uint8_t mdir_buf[LFSR_MPTR_DSIZE];
            err = lfsr_btree_commit(lfs, &mtree_.u.btree,
                    lfsr_mid_bid(lfs, mdir->mid), LFSR_ATTRS(
                        LFSR_ATTR(
                            LFSR_TAG_MDIR, 0,
                            LFSR_DATA_MPTR_(
                                lfsr_mdir_mptr(&mdir_[0]),
                                mdir_buf))));
            if (err) {
                goto failed;
            }
        }
    }

    // patch any pending grms
    //
    // Assuming we already xored our gdelta with the grm, we first
    // need to xor the grm out of the gdelta. We can't just zero
    // the gdelta because we may have picked up extra gdelta from
    // split/dropped mdirs
    //
    // gd' = gd xor (grm' xor grm)
    //
    uint8_t grm_d[LFSR_GRM_DSIZE];
    lfsr_data_t data = lfsr_data_fromgrm(&lfs->grm, grm_d);
    lfsr_gdelta_xor(lfs->grm_d, grm_d, lfsr_data_size(data));

    // patch our grm
    for (int j = 0; j < 2; j++) {
        if (lfsr_mid_bid(lfs, lfs->grm.mids[j])
                == lfsr_mid_bid(lfs, lfs_smax32(mdir->mid, 0))) {
            if (lfsr_mid_rid(lfs, lfs->grm.mids[j])
                    >= (lfsr_srid_t)mdir_[0].rbyd.weight) {
                lfs->grm.mids[j]
                        += (1 << lfs->mdir_bits) - mdir_[0].rbyd.weight;
            }
        } else if (lfs->grm.mids[j] > mdir->mid) {
            lfs->grm.mids[j] += mdelta;
        }
    }

    // xor our patch into our gdelta
    data = lfsr_data_fromgrm(&lfs->grm, grm_d);
    lfsr_gdelta_xor(lfs->grm_d, grm_d, lfsr_data_size(data));

    // need to update mtree?
    if (lfsr_mtree_cmp(&mtree_, &lfs->mtree) != 0) {
        // mtree should never go to zero since we always have a root bookmark
        LFS_ASSERT(lfsr_mtree_weight(&mtree_) > 0);

        // mark any copies of our mroot as unerased
        lfs->mroot.rbyd.eoff = -1;
        for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
            if (lfsr_mdir_cmp(&o->mdir, &lfs->mroot) == 0) {
                o->mdir.rbyd.eoff = -1;
            }
        }

        // commit new mtree into our mroot
        //
        // note end_rid=0 here will delete any files leftover from a split
        // in our mroot
        uint8_t mtree_buf[LFS_MAX(LFSR_MPTR_DSIZE, LFSR_BTREE_DSIZE)];
        err = lfsr_mdir_commit_(lfs, &mroot_, -1, 0, NULL, -1, LFSR_ATTRS(
                (lfsr_mtree_ismptr(&mtree_))
                    ? LFSR_ATTR(
                        LFSR_TAG_SUB | LFSR_TAG_MDIR, 0,
                        LFSR_DATA_MPTR_(&mtree_.u.mptr.mptr, mtree_buf))
                    : LFSR_ATTR(
                        LFSR_TAG_SUB | LFSR_TAG_MTREE, 0,
                        LFSR_DATA_BTREE_(&mtree_.u.btree, mtree_buf))));
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            goto failed;
        }
    }

    // need to update mroot chain?
    if (lfsr_mdir_cmp(&mroot_, &lfs->mroot) != 0) {
        // tail recurse, updating mroots until a commit sticks
        lfsr_mdir_t mrootchild = lfs->mroot;
        lfsr_mdir_t mrootchild_ = mroot_;
        while (lfsr_mdir_cmp(&mrootchild_, &mrootchild) != 0
                && !lfsr_mdir_ismrootanchor(&mrootchild)) {
            // find the mroot's parent
            lfsr_mdir_t mrootparent_;
            err = lfsr_mroot_parent(lfs, lfsr_mdir_mptr(&mrootchild),
                    &mrootparent_);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                goto failed;
            }

            LFS_DEBUG("Relocating mroot 0x{%"PRIx32",%"PRIx32"} "
                    "-> 0x{%"PRIx32",%"PRIx32"}",
                    mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                    mrootchild_.rbyd.blocks[0], mrootchild_.rbyd.blocks[1]);

            mrootchild = mrootparent_;

            // commit mrootchild
            uint8_t mrootchild_buf[LFSR_MPTR_DSIZE];
            err = lfsr_mdir_commit_(lfs, &mrootparent_, -1, -1, NULL,
                    -1, LFSR_ATTRS(
                        LFSR_ATTR(
                            LFSR_TAG_MROOT, 0,
                            LFSR_DATA_MPTR_(
                                lfsr_mdir_mptr(&mrootchild_),
                                mrootchild_buf))));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                LFS_ASSERT(err != LFS_ERR_NOENT);
                goto failed;
            }

            mrootchild_ = mrootparent_;
        }

        // no more mroot parents? uh oh, need to extend mroot chain
        if (lfsr_mdir_cmp(&mrootchild_, &mrootchild) != 0) {
            // mrootchild should be our previous mroot anchor at this point
            LFS_ASSERT(lfsr_mdir_ismrootanchor(&mrootchild));
            LFS_DEBUG("Extending mroot 0x{%"PRIx32",%"PRIx32"}"
                    " -> 0x{%"PRIx32",%"PRIx32"}"
                    ", 0x{%"PRIx32",%"PRIx32"}",
                    mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                    mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                    mrootchild_.rbyd.blocks[0], mrootchild_.rbyd.blocks[1]);

            // commit the new mroot anchor
            lfsr_mdir_t mrootanchor_;
            err = lfsr_mdir_swap__(lfs, &mrootanchor_, &mrootchild, true);
            if (err) {
                goto failed;
            }

            uint8_t mrootchild_buf[LFSR_MPTR_DSIZE];
            err = lfsr_mdir_commit__(lfs, &mrootanchor_, -1, -1,
                    -1, LFSR_ATTRS(
                        LFSR_ATTR(
                            LFSR_TAG_MAGIC, 0,
                            LFSR_DATA_BUF("littlefs", 8)),
                        LFSR_ATTR(
                            LFSR_TAG_MROOT, 0,
                            LFSR_DATA_MPTR_(
                                lfsr_mdir_mptr(&mrootchild_),
                                mrootchild_buf))));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                LFS_ASSERT(err != LFS_ERR_NOENT);
                goto failed;
            }
        }
    }

    // gstate must have been committed by a lower-level function at this point
    LFS_ASSERT(lfsr_gdelta_iszero(lfs->grm_d, LFSR_GRM_DSIZE));

    // success? update in-device state, we must not error at this point

    // toss our cksum into the filesystem seed for pseudorandom numbers
    if (mdelta >= 0) {
        lfs->seed ^= mdir_[0].rbyd.cksum;
    }
    if (mdelta > 0) {
        lfs->seed ^= mdir_[1].rbyd.cksum;
    }

    // update any gstate changes
    lfsr_fs_commitgdelta(lfs);

    // play out any attrs that affect internal state
    mid_ = mdir->mid;
    for (lfs_size_t i = 0; i < attr_count; i++) {
        // adjust any opened mdirs
        for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
            // adjust opened mdirs?
            if (lfsr_mdir_cmp(&o->mdir, mdir) == 0
                    && o->mdir.mid >= mid_) {
                // removed?
                if (o->mdir.mid < mid_ - attrs[i].weight) {
                    // we should not be removing opened regular files
                    LFS_ASSERT(o->type != LFS_TYPE_REG);
                    if (o->type == LFS_TYPE_DIR) {
                        ((lfsr_dir_t*)o)->pos
                                += (mid_ - attrs[i].weight) - o->mdir.mid;
                    }
                    o->mdir.mid = mid_;
                } else {
                    o->mdir.mid += attrs[i].weight;
                }
            }
        }

        // adjust mid
        mid_ = lfsr_attr_nextrid(attrs[i], mid_);
    }

    // update any staged bsprouts/bshrubs
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        if (o->type == LFS_TYPE_REG) {
            lfsr_file_t *file = (lfsr_file_t*)o;
            file->bshrub = file->bshrub_;
        }
    }

    // update internal mdir state
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        // avoid double updating the current mdir
        if (&o->mdir == mdir) {
            continue;
        }

        // update any splits/drops
        if (lfsr_mdir_cmp(&o->mdir, mdir) == 0) {
            LFS_ASSERT(mdir->mid != -1 || mdir == &lfs->mroot);
            if (mdelta > 0
                    && lfsr_mid_rid(lfs, o->mdir.mid)
                        >= (lfsr_srid_t)mdir_[0].rbyd.weight) {
                o->mdir.mid += (1 << lfs->mdir_bits) - mdir_[0].rbyd.weight;
                o->mdir.rbyd = mdir_[1].rbyd;
            } else {
                o->mdir.rbyd = mdir_[0].rbyd;
            }
        } else if (o->mdir.mid > mdir->mid) {
            o->mdir.mid += mdelta;
        }
    }

    // update mdir to follow requested rid
    LFS_ASSERT(mdir->mid != -1 || mdir == &lfs->mroot);
    if (mdelta > 0
            && lfsr_mid_rid(lfs, mdir->mid)
                >= (lfsr_srid_t)mdir_[0].rbyd.weight) {
        mdir->mid += (1 << lfs->mdir_bits) - mdir_[0].rbyd.weight;
        mdir->rbyd = mdir_[1].rbyd;
    } else {
        mdir->rbyd = mdir_[0].rbyd;
    }

    // update mroot and mtree
    lfs->mroot = mroot_;
    lfs->mtree = mtree_;

    return 0;

failed:;
    // revert gstate to on-disk state
    lfsr_fs_revertgdelta(lfs);
    return err;
}



/// Path/name lookup stuff ///

// lookup names in an mdir
//
// if not found, rid will be the best place to insert
static int lfsr_mdir_namelookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_did_t did, const char *name, lfs_size_t name_size,
        lfsr_smid_t *mid_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // default to mid_ = 0, this blanket assignment is the only way to
    // keep GCC happy
    if (mid_) {
        *mid_ = 0;
    }

    // empty mdir?
    if (mdir->rbyd.weight == 0) {
        return LFS_ERR_NOENT;
    }

    lfsr_srid_t rid;
    lfsr_tag_t tag;
    lfs_scmp_t cmp = lfsr_rbyd_namelookup(lfs, &mdir->rbyd,
            did, name, name_size,
            &rid, &tag, NULL, data_);
    if (cmp < 0) {
        LFS_ASSERT(cmp != LFS_ERR_NOENT);
        return cmp;
    }

    // adjust mid if necessary
    //
    // note missing mids end up pointing to the next mid
    lfsr_smid_t mid = LFSR_MID(lfs,
            mdir->mid,
            (cmp < LFS_CMP_EQ) ? rid+1 : rid);

    // intercept pending grms here and pretend they're orphaned files
    //
    // fortunately pending grms/orphaned files have roughly the same
    // semantics, and it's easier to manage the implied mid gap in
    // higher-levels
    if (lfsr_grm_ispending(&lfs->grm, mid)) {
        tag = LFSR_TAG_ORPHAN;
    }

    if (mid_) {
        *mid_ = mid;
    }
    if (tag_) {
        *tag_ = tag;
    }
    return (cmp == LFS_CMP_EQ) ? 0 : LFS_ERR_NOENT;
}

// lookup names in our mtree
//
// if not found, rid will be the best place to insert
static int lfsr_mtree_namelookup(lfs_t *lfs, const lfsr_mtree_t *mtree,
        lfsr_did_t did, const char *name, lfs_size_t name_size,
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // do we only have mroot?
    lfsr_mdir_t mdir;
    if (lfsr_mtree_isnull(mtree)) {
        mdir = lfs->mroot;
        // treat inlined mdir as mid=0
        mdir.mid = 0;

    // direct mdir?
    } else if (lfsr_mtree_ismptr(mtree)) {
        int err = lfsr_mdir_fetch(lfs, &mdir, 0, &mtree->u.mptr.mptr);
        if (err) {
            return err;
        }

    // lookup name in actual mtree
    } else {
        lfsr_bid_t bid;
        lfsr_tag_t tag;
        lfsr_bid_t weight;
        lfsr_data_t data;
        lfs_scmp_t cmp = lfsr_btree_namelookup(lfs, &mtree->u.btree,
                did, name, name_size,
                &bid, &tag, &weight, &data);
        if (cmp < 0) {
            LFS_ASSERT(cmp != LFS_ERR_NOENT);
            return cmp;
        }
        LFS_ASSERT(tag == LFSR_TAG_MDIR);
        LFS_ASSERT(weight == (1U << lfs->mdir_bits));

        // decode mdir
        lfsr_mptr_t mptr;
        int err = lfsr_data_readmptr(lfs, &data, &mptr);
        if (err) {
            return err;
        }

        // fetch mdir
        err = lfsr_mdir_fetch(lfs, &mdir, bid-(weight-1), &mptr);
        if (err) {
            return err;
        }
    }

    // and finally lookup name in our mdir
    lfsr_smid_t mid;
    int err = lfsr_mdir_namelookup(lfs, &mdir,
            did, name, name_size,
            &mid, tag_, data_);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    // update mdir with best place to insert even if we fail
    mdir.mid = mid;
    if (mdir_) {
        *mdir_ = mdir;
    }

    return err;
}


// special directory-ids
enum {
    LFSR_DID_ROOT = 0,
};

// lookup full paths in our mtree
//
// note the errors here are a bit weird, because paths can have some weird
// corner-cases during lookup, and we want to report all the different
// conditions:
//
// - 0      => path is valid, file NOT found
// - EXIST  => path is valid, file found
// - INVAL  => path is valid, but points to root
// - NOENT  => path is NOT valid, intermediate dir missing
// - NOTDIR => path is NOT valid, intermediate dir is not a dir
//
// if not found, mdir_/did_/name_ will at least be set up
// with what should be the parent
static int lfsr_mtree_pathlookup(lfs_t *lfs, const lfsr_mtree_t *mtree,
        const char *path,
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_,
        lfsr_did_t *did_, const char **name_, lfs_size_t *name_size_) {
    // setup root
    lfsr_mdir_t mdir = {.mid = -1};
    lfsr_tag_t tag = LFSR_TAG_DIR;
    lfsr_did_t did = LFSR_DID_ROOT;
    
    // we reduce path to a single name if we can find it
    const char *name = path;
    lfs_size_t name_size = 0;
    while (true) {
        // skip slashes
        path += lfs_strspn(path, "/");
        lfs_size_t name_size__ = lfs_strcspn(path, "/");

        // skip '.' and root '..'
        if ((name_size__ == 1 && lfs_memcmp(path, ".", 1) == 0)
                || (name_size__ == 2 && lfs_memcmp(path, "..", 2) == 0)) {
            path += name_size__;
            goto next;
        }

        // skip if matched by '..' in name
        const char *suffix = path + name_size__;
        lfs_size_t suffix_size;
        int depth = 1;
        while (true) {
            suffix += lfs_strspn(suffix, "/");
            suffix_size = lfs_strcspn(suffix, "/");
            if (suffix_size == 0) {
                break;
            }

            if (suffix_size == 2 && lfs_memcmp(suffix, "..", 2) == 0) {
                depth -= 1;
                if (depth == 0) {
                    path = suffix + suffix_size;
                    goto next;
                }
            } else {
                depth += 1;
            }

            suffix += suffix_size;
        }

        // found end of path, we must be done parsing our path now
        if (path[0] == '\0') {
            if (mdir_) {
                *mdir_ = mdir;
            }
            if (tag_) {
                *tag_ = tag;
            }
            if (did_) {
                *did_ = did;
            }
            if (name_) {
                *name_ = name;
            }
            if (name_size_) {
                *name_size_ = name_size;
            }
            // the root dir doesn't have an mdir really, so it's always
            // a special case
            return (mdir.mid == -1)
                    ? LFS_ERR_INVAL
                    : LFS_ERR_EXIST;
        }

        // found another name
        name = path;
        name_size = name_size__;

        // only continue if we hit a directory
        if (tag != LFSR_TAG_DIR) {
            return (tag == LFSR_TAG_ORPHAN)
                    ? LFS_ERR_NOENT
                    : LFS_ERR_NOTDIR;
        }

        // read the next did from the mdir if this is not the root
        if (mdir.mid != -1) {
            lfsr_data_t data;
            int err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_DID,
                    &data);
            if (err) {
                return err;
            }

            err = lfsr_data_readleb128(lfs, &data, &did);
            if (err) {
                return err;
            }
        }

        // lookup up this name in the mtree
        int err = lfsr_mtree_namelookup(lfs, mtree,
                did, name, name_size,
                &mdir, &tag, NULL);
        if (err) {
            // report where to insert if we are the last name in our path
            if (err == LFS_ERR_NOENT && lfs_strchr(name, '/') == NULL) {
                if (mdir_) {
                    *mdir_ = mdir;
                }
                if (tag_) {
                    *tag_ = tag;
                }
                if (did_) {
                    *did_ = did;
                }
                if (name_) {
                    *name_ = name;
                }
                if (name_size_) {
                    *name_size_ = name_size;
                }
                return 0;
            }
            return err;
        }

        // go on to next name
        path += name_size;
    next:;
    }
}



/// Traversal stuff ///

// incremental filesystem traversal
typedef struct lfsr_traversal {
    // core traversal state
    uint8_t flags;
    uint8_t state;
    union {
        // cycle detection state, only valid when traversing the mroot chain
        struct {
            lfsr_mptr_t mptr;
            lfs_block_t step;
            uint8_t power;
        } mtortoise;
        // btree traversal state, only valid when traversing the mtree
        lfsr_btraversal_t mt;
        // opened file state, only valid when traversing opened files
        const lfsr_opened_t *o;
    } u;
    // we really don't want to pay the RAM cost for a full file,
    // so only store the relevant bits, is this a hack? yes
    struct {
        lfsr_opened_t o;
        const struct lfs_file_config *cfg;
        lfsr_bshrub_t bshrub;
    } file;
    lfsr_btraversal_t bt;
} lfsr_traversal_t;

enum {
    // traverse all blocks in the filesystem
    LFSR_TRAVERSAL_ALL = 0x1,
    // validate checksums while traversing
    LFSR_TRAVERSAL_VALIDATE = 0x2,
};

// traversing littlefs is a bit complex, so we use a state machine to keep
// track of where we are
enum {
    LFSR_TRAVERSAL_MROOTANCHOR  = 0,
    LFSR_TRAVERSAL_MROOTCHAIN   = 1,
    LFSR_TRAVERSAL_MTREE        = 2,
    LFSR_TRAVERSAL_MDIR         = 3,
    LFSR_TRAVERSAL_MDIRBTREE    = 4,
    LFSR_TRAVERSAL_OPENED       = 5,
    LFSR_TRAVERSAL_OPENEDBTREE  = 6,
    LFSR_TRAVERSAL_DONE         = 7,
};

#define LFSR_TRAVERSAL(_flags) \
    ((lfsr_traversal_t){ \
        .flags=_flags, \
        .state=LFSR_TRAVERSAL_MROOTANCHOR, \
        .u.mtortoise.mptr={{0, 0}}, \
        .u.mtortoise.step=0, \
        .u.mtortoise.power=0})

static inline bool lfsr_traversal_isall(const lfsr_traversal_t *t) {
    return t->flags & LFSR_TRAVERSAL_ALL;
}

static inline bool lfsr_traversal_isvalidate(const lfsr_traversal_t *t) {
    return t->flags & LFSR_TRAVERSAL_VALIDATE;
}

// needed in lfsr_traversal_read
static int lfsr_bshrub_traverse(lfs_t *lfs, const lfsr_file_t *file,
        lfsr_btraversal_t *t,
        lfsr_bid_t *bid_, lfsr_tinfo_t *tinfo_);

static int lfsr_traversal_read(lfs_t *lfs, lfsr_traversal_t *t,
        lfsr_tinfo_t *tinfo_) {
    while (true) {
        switch (t->state) {
        // start with the mrootanchor 0x{0,1}
        //
        // note we make sure to include all mroots in our mroot chain!
        //
        case LFSR_TRAVERSAL_MROOTANCHOR:;
            // fetch the first mroot 0x{0,1}
            int err = lfsr_mdir_fetch(lfs, &t->file.o.mdir,
                    -1, &LFSR_MPTR_MROOTANCHOR());
            if (err) {
                return err;
            }

            // transition to traversing the mroot chain
            t->state = LFSR_TRAVERSAL_MROOTCHAIN;

            if (tinfo_) {
                tinfo_->tag = LFSR_TAG_MDIR;
                tinfo_->u.mdir = t->file.o.mdir;
            }
            return 0;

        // traverse the mroot chain, checking for mroot/mtree/mdir
        case LFSR_TRAVERSAL_MROOTCHAIN:;
            // lookup mroot, if we find one this is a fake mroot
            lfsr_tag_t tag;
            lfsr_data_t data;
            err = lfsr_mdir_sublookup(lfs, &t->file.o.mdir,
                    LFSR_TAG_STRUCT,
                    &tag, &data);
            if (err) {
                // if we have no mtree/mdir (inlined mdir), we need to traverse
                // any files in our mroot next
                if (err == LFS_ERR_NOENT) {
                    t->file.o.mdir.mid = 0;
                    t->state = LFSR_TRAVERSAL_MDIR;
                    continue;
                }
                return err;
            }

            // found a new mroot
            if (tag == LFSR_TAG_MROOT) {
                lfsr_mptr_t mptr;
                err = lfsr_data_readmptr(lfs, &data, &mptr);
                if (err) {
                    return err;
                }

                // detect cycles with Brent's algorithm
                //
                // note we only check for cycles in the mroot chain, the btree
                // inner nodes require checksums of their pointers, so creating
                // a valid cycle is actually quite difficult
                //
                if (lfsr_mptr_cmp(&mptr, &t->u.mtortoise.mptr) == 0) {
                    LFS_ERROR("Cycle detected during mtree traversal "
                            "0x{%"PRIx32",%"PRIx32"}",
                            mptr.blocks[0],
                            mptr.blocks[1]);
                    return LFS_ERR_CORRUPT;
                }
                if (t->u.mtortoise.step == (1U << t->u.mtortoise.power)) {
                    t->u.mtortoise.mptr = mptr;
                    t->u.mtortoise.step = 0;
                    t->u.mtortoise.power += 1;
                }
                t->u.mtortoise.step += 1;

                // fetch this mroot
                err = lfsr_mdir_fetch(lfs, &t->file.o.mdir, -1, &mptr);
                if (err) {
                    return err;
                }

                if (tinfo_) {
                    tinfo_->tag = LFSR_TAG_MDIR;
                    tinfo_->u.mdir = t->file.o.mdir;
                }
                return 0;

            // found an mdir?
            } else if (tag == LFSR_TAG_MDIR) {
                // fetch this mdir
                lfsr_mptr_t mptr;
                err = lfsr_data_readmptr(lfs, &data, &mptr);
                if (err) {
                    return err;
                }

                err = lfsr_mdir_fetch(lfs, &t->file.o.mdir, 0, &mptr);
                if (err) {
                    return err;
                }

                // transition to mdir traversal next
                t->state = LFSR_TRAVERSAL_MDIR;

                if (tinfo_) {
                    tinfo_->tag = LFSR_TAG_MDIR;
                    tinfo_->u.mdir = t->file.o.mdir;
                }
                return 0;

            // found an mtree?
            } else if (tag == LFSR_TAG_MTREE) {
                // read the root of the mtree and return it, lfs->mtree may not
                // be initialized yet
                lfsr_btree_t mtree;
                err = lfsr_data_readbtree(lfs, &data, &mtree);
                if (err) {
                    return err;
                }

                // validate our btree nodes if requested, this just means we
                // need to do a full rbyd fetch and make sure the checksums
                // match
                if (lfsr_traversal_isvalidate(t)) {
                    err = lfsr_rbyd_fetchvalidate(lfs, &mtree,
                            mtree.blocks[0], mtree.trunk, mtree.weight,
                            mtree.cksum);
                    if (err) {
                        return err;
                    }
                }

                // transition to traversing the mtree
                t->state = LFSR_TRAVERSAL_MTREE;
                t->u.mt = LFSR_BTRAVERSAL();

                if (tinfo_) {
                    tinfo_->tag = LFSR_TAG_BRANCH;
                    tinfo_->u.rbyd = mtree;
                }
                return 0;

            } else {
                LFS_ERROR("Weird mtree entry? 0x%"PRIx32, tag);
                return LFS_ERR_CORRUPT;
            }

        // traverse the mtree, including both inner btree nodes and mdirs
        case LFSR_TRAVERSAL_MTREE:;
            // no mtree? transition to traversing any opened mdirs
            if (lfsr_mtree_ismptr(&lfs->mtree)) {
                t->u.o = lfs->opened;
                t->state = LFSR_TRAVERSAL_OPENED;
                continue;
            }

            // traverse through the mtree
            lfsr_bid_t bid;
            lfsr_tinfo_t tinfo;
            err = lfsr_btree_traverse(lfs, &lfs->mtree.u.btree,
                    &t->u.mt,
                    &bid, &tinfo);
            if (err) {
                // end of mtree? transition to traversing any opened mdirs
                if (err == LFS_ERR_NOENT) {
                    t->u.o = lfs->opened;
                    t->state = LFSR_TRAVERSAL_OPENED;
                    continue;
                }
                return err;
            }

            // wait is this the mtree's root? skip this, we assume we've already
            // seen it above (this gets a bit weird because 1. mtree may be
            // uninitialized in mountinited and 2. stack really matters since
            // we're at the bottom of lfs_alloc)
            if (tinfo.tag == LFSR_TAG_BRANCH
                    && tinfo.u.rbyd.blocks[0] == lfs->mtree.u.btree.blocks[0]) {
                continue;
            }

            // inner btree nodes already decoded
            if (tinfo.tag == LFSR_TAG_BRANCH) {
                // validate our btree nodes if requested, this just means we
                // need to do a full rbyd fetch and make sure the checksums
                // match
                if (lfsr_traversal_isvalidate(t)) {
                    err = lfsr_rbyd_fetchvalidate(lfs, &tinfo.u.rbyd,
                            tinfo.u.rbyd.blocks[0], tinfo.u.rbyd.trunk,
                            tinfo.u.rbyd.weight,
                            tinfo.u.rbyd.cksum);
                    if (err) {
                        return err;
                    }
                }

                if (tinfo_) {
                    *tinfo_ = tinfo;
                }
                return 0;

            // fetch mdir if we're on a leaf
            } else if (tinfo.tag == LFSR_TAG_MDIR) {
                lfsr_mptr_t mptr;
                err = lfsr_data_readmptr(lfs, &tinfo.u.data, &mptr);
                if (err) {
                    return err;
                }

                err = lfsr_mdir_fetch(lfs, &t->file.o.mdir,
                        LFSR_MID(lfs, bid, 0),
                        &mptr);
                if (err) {
                    return err;
                }

                // transition to mdir traversal next
                t->state = LFSR_TRAVERSAL_MDIR;

                if (tinfo_) {
                    tinfo_->tag = LFSR_TAG_MDIR;
                    tinfo_->u.mdir = t->file.o.mdir;
                }
                return 0;

            } else {
                LFS_ERROR("Weird mtree entry? 0x%"PRIx32, tinfo.tag);
                return LFS_ERR_CORRUPT;
            }

        // scan for blocks/btrees in the current mdir
        case LFSR_TRAVERSAL_MDIR:;
            // not traversing all blocks? have we exceeded our mdir's weight?
            // return to mtree traversal
            if (!lfsr_traversal_isall(t)
                    || lfsr_mid_rid(lfs, t->file.o.mdir.mid)
                        >= (lfsr_srid_t)t->file.o.mdir.rbyd.weight) {
                t->state = LFSR_TRAVERSAL_MTREE;
                continue;
            }

            // do we have a block/btree?
            err = lfsr_mdir_lookupnext(lfs, &t->file.o.mdir, LFSR_TAG_DATA,
                    &tag, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            // found a direct block?
            if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BLOCK) {
                err = lfsr_data_readbptr(lfs, &data, &t->file.bshrub.u.bptr);
                if (err) {
                    return err;
                }

            // found a bshrub (inlined btree)?
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
                err = lfsr_data_readshrub(lfs, &data, &t->file.o.mdir,
                        &t->file.bshrub.u.bshrub);
                if (err) {
                    return err;
                }

            // found a btree?
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BTREE) {
                err = lfsr_data_readbtree(lfs, &data,
                        &t->file.bshrub.u.btree);
                if (err) {
                    return err;
                }

            // no? continue to next file
            } else {
                t->file.o.mdir.mid += 1;
                continue;
            }

            // start traversing
            t->bt = LFSR_BTRAVERSAL();
            t->state = LFSR_TRAVERSAL_MDIRBTREE;
            continue;

        // scan for blocks/btrees in our opened file list
        case LFSR_TRAVERSAL_OPENED:;
            // not traversing all blocks? reached end of opened file list?
            if (!lfsr_traversal_isall(t) || !t->u.o) {
                t->state = LFSR_TRAVERSAL_DONE;
                continue;
            }

            // skip non-files
            if (t->u.o->type != LFS_TYPE_REG) {
                t->u.o = t->u.o->next;
                continue;
            }

            // start traversing the file
            const lfsr_file_t *file = (const lfsr_file_t*)t->u.o;
            t->file.o.mdir = file->o.mdir;
            t->file.bshrub = file->bshrub;
            t->bt = LFSR_BTRAVERSAL();
            t->state = LFSR_TRAVERSAL_OPENEDBTREE;
            continue;

        // traverse any file btrees, including both inner btree nodes and
        // block pointers
        case LFSR_TRAVERSAL_MDIRBTREE:;
        case LFSR_TRAVERSAL_OPENEDBTREE:;
            // traverse through our file
            err = lfsr_bshrub_traverse(lfs, (const lfsr_file_t*)&t->file,
                    &t->bt,
                    NULL, &tinfo);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    // end of btree? go to next file
                    if (t->state == LFSR_TRAVERSAL_MDIRBTREE) {
                        t->file.o.mdir.mid += 1;
                        t->state = LFSR_TRAVERSAL_MDIR;
                        continue;
                    } else if (t->state == LFSR_TRAVERSAL_OPENEDBTREE) {
                        t->u.o = t->u.o->next;
                        t->state = LFSR_TRAVERSAL_OPENED;
                        continue;
                    } else {
                        LFS_UNREACHABLE();
                    }
                }
                return err;
            }

            // found an inner btree node?
            if (tinfo.tag == LFSR_TAG_BRANCH) {
                // validate our btree nodes if requested, this just means we
                // need to do a full rbyd fetch and make sure the checksums
                // match
                if (lfsr_traversal_isvalidate(t)) {
                    err = lfsr_rbyd_fetchvalidate(lfs, &tinfo.u.rbyd,
                            tinfo.u.rbyd.blocks[0], tinfo.u.rbyd.trunk,
                            tinfo.u.rbyd.weight,
                            tinfo.u.rbyd.cksum);
                    if (err) {
                        return err;
                    }
                }

                if (tinfo_) {
                    *tinfo_ = tinfo;
                }
                return 0;

            // found inlined data? ignore this
            } else if (tinfo.tag == LFSR_TAG_DATA) {
                continue;

            // found an indirect block?
            } else if (tinfo.tag == LFSR_TAG_BLOCK) {
                // TODO validate?

                if (tinfo_) {
                    *tinfo_ = tinfo;
                }
                return 0;

            } else {
                LFS_UNREACHABLE();
            }

        case LFSR_TRAVERSAL_DONE:;
            return LFS_ERR_NOENT;

        default:;
            LFS_UNREACHABLE();
        }
    }
}


/// Superblock things ///

//// TODO rm?
//// These are all leb128s, but we can expect smaller encodings
//// if we assume the version.
////
//// - 7-bit major_version => 1 byte leb128 (worst case)
//// - 7-bit minor_version => 1 byte leb128 (worst case)
//// - 7-bit cksum_type    => 1 byte leb128 (worst case)
//// - 7-bit flags         => 1 byte leb128 (worst case)
//// - 32-bit block_size   => 5 byte leb128 (worst case)
//// - 32-bit block_count  => 5 byte leb128 (worst case)
//// - 7-bit utag_limit    => 1 byte leb128 (worst case)
//// - 32-bit mtree_limit  => 5 byte leb128 (worst case)
//// - 32-bit attr_limit   => 5 byte leb128 (worst case)
//// - 32-bit name_limit   => 5 byte leb128 (worst case)
//// - 32-bit file_limit   => 5 byte leb128 (worst case)
////                       => 33 bytes total
//// 
//#define LFSR_SUPERCONFIG_DSIZE (1+1+1+1+5+5+1+5+5+5+5)
//
//#define LFSR_DATA_FROMSUPERCONFIG(_lfs, _buffer) 
//    lfsr_data_fromsuperconfig(_lfs, _buffer)
//
//static lfsr_data_t lfsr_data_fromsuperconfig(lfs_t *lfs,
//        uint8_t buffer[static LFSR_SUPERCONFIG_DSIZE]) {
//    // TODO most of these should also be in the lfs_config/lfs_t structs
//
//    // note we take a shortcut for for single-byte leb128s, but these
//    // are still leb128s! the top bit must be zero!
//
//    // on-disk major version
//    buffer[0] = LFS_DISK_VERSION_MAJOR;
//    // on-disk minor version
//    buffer[1] = LFS_DISK_VERSION_MINOR;
//    // on-disk cksum type
//    buffer[2] = 2;
//    // on-disk flags
//    buffer[3] = 0;
//
//    // on-disk block size
//    lfs_ssize_t d = 4;
//    lfs_ssize_t d_ = lfs_toleb128(lfs->cfg->block_size, &buffer[d], 5);
//    LFS_ASSERT(d_ >= 0);
//    d += d_;
//
//    // on-disk block count
//    d_ = lfs_toleb128(lfs->cfg->block_count, &buffer[d], 5);
//    LFS_ASSERT(d_ >= 0);
//    d += d_;
//
//    // on-disk mleaf limit
//    d_ = lfs_toleb128(lfsr_mleafweight(lfs)-1, &buffer[d], 5);
//    LFS_ASSERT(d_ >= 0);
//    d += d_;
//
//    // on-disk utag limit
//    buffer[d] = 0x7f;
//    d += 1;
//
//    // on-disk attr limit
//    d_ = lfs_toleb128(0x7fffffff, &buffer[d], 5);
//    LFS_ASSERT(d_ >= 0);
//    d += d_;
//
//    // on-disk name limit
//    d_ = lfs_toleb128(0xff, &buffer[d], 5);
//    LFS_ASSERT(d_ >= 0);
//    d += d_;
//
//    // on-disk file limit
//    d_ = lfs_toleb128(0x7fffffff, &buffer[d], 5);
//    LFS_ASSERT(d_ >= 0);
//    d += d_;
//
//    return LFSR_DATA_BUF(buffer, d);
//}

// compatibility flags
//
// - RCOMPAT => Must understand to read the filesystem
// - WCOMPAT => Must understand to write to the filesystem
// - OCOMPAT => Don't need to understand, we don't really use these
//
// note, "understanding" does not necessarily mean support
//
enum lfsr_rcompat {
    LFSR_RCOMPAT_NONSTANDARD    = 0x0001,
    LFSR_RCOMPAT_MLEAF          = 0x0002,
    LFSR_RCOMPAT_MTREE          = 0x0008,
    LFSR_RCOMPAT_BSPROUT        = 0x0010,
    LFSR_RCOMPAT_BLEAF          = 0x0020,
    LFSR_RCOMPAT_BSHRUB         = 0x0040,
    LFSR_RCOMPAT_BTREE          = 0x0080,
    LFSR_RCOMPAT_GRM            = 0x0100,
    // internal
    LFSR_RCOMPAT_OVERFLOW       = 0x8000,
};

#define LFSR_RCOMPAT_COMPAT \
    (LFSR_RCOMPAT_MLEAF \
        | LFSR_RCOMPAT_MTREE \
        | LFSR_RCOMPAT_BSPROUT \
        | LFSR_RCOMPAT_BLEAF \
        | LFSR_RCOMPAT_BSHRUB \
        | LFSR_RCOMPAT_BTREE \
        | LFSR_RCOMPAT_GRM)

enum lfsr_wcompat {
    LFSR_WCOMPAT_NONSTANDARD    = 0x0001,
    // internal
    LFSR_WCOMPAT_OVERFLOW       = 0x8000,
};

#define LFSR_WCOMPAT_COMPAT 0

enum lfsr_ocompat {
    LFSR_OCOMPAT_NONSTANDARD    = 0x0001,
    // internal
    LFSR_OCOMPAT_OVERFLOW       = 0x8000,
};

#define LFSR_OCOMPAT_COMPAT 0

typedef uint16_t lfsr_rcompat_t;
typedef uint16_t lfsr_wcompat_t;
typedef uint16_t lfsr_ocompat_t;

static inline bool lfsr_rcompat_isincompat(lfsr_rcompat_t rcompat) {
    return rcompat != LFSR_RCOMPAT_COMPAT;
}

static inline bool lfsr_wcompat_isincompat(lfsr_wcompat_t wcompat) {
    return wcompat != LFSR_WCOMPAT_COMPAT;
}

static inline bool lfsr_ocompat_isincompat(lfsr_ocompat_t ocompat) {
    return ocompat != LFSR_OCOMPAT_COMPAT;
}

// compat flags on-disk encoding
//
// little-endian, truncated bits must be assumed zero

#define LFSR_DATA_RCOMPAT(_rcompat) \
    LFSR_DATA_BUF(((uint8_t[]){ \
        (((_rcompat) >> 0) & 0xff), \
        (((_rcompat) >> 8) & 0xff)}), 2)

static int lfsr_data_readrcompat(lfs_t *lfs, lfsr_data_t *data,
        lfsr_rcompat_t *rcompat) {
    // allow truncated rcompat flags
    uint8_t buf[2] = {0};
    lfs_ssize_t d = lfsr_data_read(lfs, data, buf, 2);
    if (d < 0) {
        return d;
    }
    *rcompat = lfs_fromle16_(buf);

    // if any out-of-range flags are set, set the internal overflow bit,
    // this is a compromise in correctness and and compat-flag complexity
    //
    // we don't really care about performance here
    while (lfsr_data_size(*data) > 0) {
        lfs_scmp_t cmp = lfsr_data_cmp(lfs, *data, (uint8_t[]){0}, 1);
        if (cmp < 0) {
            return cmp;
        }

        if (cmp != LFS_CMP_EQ) {
            *rcompat |= LFSR_RCOMPAT_OVERFLOW;
        }

        *data = lfsr_data_slice(*data, d, -1);
    }

    return 0;
}

// all the compat parsing is basically the same, so try to reuse code
#define LFSR_DATA_WCOMPAT(_wcompat) LFSR_DATA_RCOMPAT(_wcompat)

static int lfsr_data_readwcompat(lfs_t *lfs, lfsr_data_t *data,
        lfsr_wcompat_t *wcompat) {
    return lfsr_data_readrcompat(lfs, data, wcompat);
}


// disk geometry
//
// note these are stored minus 1 to avoid overflow issues
typedef struct lfsr_geometry {
    lfs_off_t block_size;
    lfs_off_t block_count;
} lfsr_geometry_t;

// geometry encoding
// .---+- -+- -+- -.      block_size:  1 leb128  <=4 bytes
// | block_size    |      block_count: 1 leb128  <=5 bytes
// +---+- -+- -+- -+- -.  total:                 <=9 bytes
// | block_count       |
// '---+- -+- -+- -+- -'
#define LFSR_GEOMETRY_DSIZE (4+5)

#define LFSR_DATA_GEOMETRY_(_geometry, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromgeometry(_geometry, _buffer)}.d)

#define LFSR_DATA_GEOMETRY(_geometry) \
    LFSR_DATA_GEOMETRY_(_geometry, (uint8_t[LFSR_GEOMETRY_DSIZE]){0})

static lfsr_data_t lfsr_data_fromgeometry(const lfsr_geometry_t *geometry,
        uint8_t buffer[static LFSR_GEOMETRY_DSIZE]) {
    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfs_toleb128(geometry->block_size-1, &buffer[d], 4);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(geometry->block_count-1, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readgeometry(lfs_t *lfs, lfsr_data_t *data,
        lfsr_geometry_t *geometry) {
    int err = lfsr_data_readlleb128(lfs, data, &geometry->block_size);
    if (err) {
        return err;
    }

    err = lfsr_data_readleb128(lfs, data, &geometry->block_count);
    if (err) {
        return err;
    }

    geometry->block_size += 1;
    geometry->block_count += 1;
    return 0;
}


/// Filesystem init functions ///

static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg);
static int lfs_deinit(lfs_t *lfs);

static int lfsr_mountmroot(lfs_t *lfs, const lfsr_mdir_t *mroot) {
    // check the disk version
    uint8_t version[2] = {0, 0};
    lfsr_data_t data;
    int err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_VERSION,
            &data);
    if (err) {
        if (err == LFS_ERR_NOENT) {
            LFS_ERROR("No littlefs version found");
            return LFS_ERR_INVAL;
        }
        return err;
    }
    lfs_ssize_t d = lfsr_data_read(lfs, &data, version, 2);
    if (d < 0) {
        return err;
    }

    if (version[0] != LFS_DISK_VERSION_MAJOR
            || version[1] > LFS_DISK_VERSION_MINOR) {
        LFS_ERROR("Incompatible version v%"PRId32".%"PRId32
                " (!= v%"PRId32".%"PRId32")",
                version[0],
                version[1],
                LFS_DISK_VERSION_MAJOR,
                LFS_DISK_VERSION_MINOR);
        return LFS_ERR_INVAL;
    }

    // check for any rcompatflags, we must understand these to read
    // the filesystem
    lfsr_rcompat_t rcompat = 0;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_RCOMPAT,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err != LFS_ERR_NOENT) {
        err = lfsr_data_readrcompat(lfs, &data, &rcompat);
        if (err) {
            return err;
        }
    }

    if (lfsr_rcompat_isincompat(rcompat)) {
        LFS_ERROR("Incompatible rcompat flags 0x%0"PRIx16
                " (!= 0x%0"PRIx16")",
                rcompat,
                LFSR_RCOMPAT_COMPAT);
        return LFS_ERR_INVAL;
    }

    // check for any wcompatflags, we must understand these to write
    // the filesystem
    lfsr_wcompat_t wcompat = 0;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_WCOMPAT,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err != LFS_ERR_NOENT) {
        err = lfsr_data_readwcompat(lfs, &data, &wcompat);
        if (err) {
            return err;
        }
    }

    // TODO switch to readonly?
    if (lfsr_wcompat_isincompat(wcompat)) {
        LFS_ERROR("Incompatible wcompat flags 0x%0"PRIx16
                " (!= 0x%0"PRIx16")",
                wcompat,
                LFSR_WCOMPAT_COMPAT);
        return LFS_ERR_INVAL;
    }

    // we don't bother to check for any ocompatflags, we would just
    // ignore these anyways

    // check the on-disk geometry
    lfsr_geometry_t geometry;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_GEOMETRY,
            &data);
    if (err) {
        if (err == LFS_ERR_NOENT) {
            LFS_ERROR("No geometry found");
            return LFS_ERR_INVAL;
        }
        return err;
    }
    err = lfsr_data_readgeometry(lfs, &data, &geometry);
    if (err) {
        return err;
    }

    if (geometry.block_size != lfs->cfg->block_size) {
        LFS_ERROR("Incompatible block size %"PRId32" (!= %"PRId32")",
                geometry.block_size,
                lfs->cfg->block_size);
        return LFS_ERR_INVAL;
    }

    if (geometry.block_count != lfs->cfg->block_count) {
        LFS_ERROR("Incompatible block count %"PRId32" (!= %"PRId32")",
                geometry.block_count,
                lfs->cfg->block_count);
        return LFS_ERR_INVAL;
    }

    // read the name limit
    lfs_size_t name_limit = 0xff;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_NAMELIMIT,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err != LFS_ERR_NOENT) {
        err = lfsr_data_readleb128(lfs, &data, &name_limit);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        if (err == LFS_ERR_CORRUPT) {
            name_limit = -1;
        }
    }

    if (name_limit > lfs->name_limit) {
        LFS_ERROR("Incompatible name limit (%"PRId32" > %"PRId32")",
                name_limit,
                lfs->name_limit);
        return LFS_ERR_INVAL;
    }

    lfs->name_limit = name_limit;

    // read the file limit
    lfs_off_t file_limit = 0x7fffffff;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_FILELIMIT,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err != LFS_ERR_NOENT) {
        err = lfsr_data_readleb128(lfs, &data, &file_limit);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        if (err == LFS_ERR_CORRUPT) {
            file_limit = -1;
        }
    }

    if (file_limit > lfs->file_limit) {
        LFS_ERROR("Incompatible file limit (%"PRId32" > %"PRId32")",
                file_limit,
                lfs->file_limit);
        return LFS_ERR_INVAL;
    }

    lfs->file_limit = file_limit;

    // check for unknown configs
    lfsr_tag_t tag;
    err = lfsr_mdir_lookupnext(lfs, mroot, LFSR_TAG_FILELIMIT+1,
            &tag, NULL);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    if (err != LFS_ERR_NOENT
            && lfsr_tag_suptype(tag) == LFSR_TAG_CONFIG) {
        LFS_ERROR("Unknown config 0x%04"PRIx16,
                tag);
        return LFS_ERR_INVAL;
    }

    return 0;
}

static int lfsr_mountinited(lfs_t *lfs) {
    // zero gdeltas, we'll read these from our mdirs
    lfsr_fs_flushgdelta(lfs);

    // default to no mtree, this is allowed and implies all files are inlined
    // in the mroot
    lfs->mtree = LFSR_MTREE_NULL();

    // traverse the mtree rooted at mroot 0x{1,0}
    //
    // we do validate btree inner nodes here, how can we trust our
    // mdirs are valid if we haven't checked the btree inner nodes at
    // least once?
    lfsr_traversal_t t = LFSR_TRAVERSAL(LFSR_TRAVERSAL_VALIDATE);
    while (true) {
        lfsr_tinfo_t tinfo;
        int err = lfsr_traversal_read(lfs, &t, &tinfo);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }

        // found an mdir?
        if (tinfo.tag == LFSR_TAG_MDIR) {
            // found an mroot?
            if (tinfo.u.mdir.mid == -1) {
                // check for the magic string, all mroot should have this
                lfsr_data_t data;
                int err = lfsr_mdir_lookup(lfs, &tinfo.u.mdir, LFSR_TAG_MAGIC,
                        &data);
                if (err) {
                    if (err == LFS_ERR_NOENT) {
                        LFS_ERROR("No littlefs magic found");
                        return LFS_ERR_INVAL;
                    }
                    return err;
                }

                // treat corrupted magic as no magic
                lfs_scmp_t cmp = lfsr_data_cmp(lfs, data, "littlefs", 8);
                if (cmp < 0) {
                    return cmp;
                }
                if (cmp != LFS_CMP_EQ) {
                    LFS_ERROR("No littlefs magic found");
                    return LFS_ERR_INVAL;
                }

                // are we the last mroot?
                err = lfsr_mdir_lookup(lfs, &tinfo.u.mdir, LFSR_TAG_MROOT,
                        NULL);
                if (err && err != LFS_ERR_NOENT) {
                    return err;
                }
                if (err == LFS_ERR_NOENT) {
                    // track active mroot
                    lfs->mroot = tinfo.u.mdir;

                    // mount/validate config in active mroot
                    err = lfsr_mountmroot(lfs, &lfs->mroot);
                    if (err) {
                        return err;
                    }
                }

            } else {
                // found a direct mdir? keep track of this
                if (lfsr_mtree_isnull(&lfs->mtree)) {
                    lfs->mtree = LFSR_MTREE_MPTR(
                            *lfsr_mdir_mptr(&tinfo.u.mdir),
                            (1 << lfs->mdir_bits));
                }
            }

            // toss our cksum into the filesystem seed for pseudorandom
            // numbers
            lfs->seed ^= tinfo.u.mdir.rbyd.cksum;

            // collect any gdeltas from this mdir
            err = lfsr_fs_consumegdelta(lfs, &tinfo.u.mdir);
            if (err) {
                return err;
            }

            // check for any orphaned files
            for (lfs_size_t rid = 0; rid < tinfo.u.mdir.rbyd.weight; rid++) {
                err = lfsr_rbyd_lookup(lfs, &tinfo.u.mdir.rbyd,
                        rid, LFSR_TAG_ORPHAN,
                        NULL);
                if (err && err != LFS_ERR_NOENT) {
                    return err;
                }

                // found an orphaned file?
                if (err != LFS_ERR_NOENT) {
                    LFS_DEBUG("Found orphaned file "
                            "%"PRId32".%"PRId32,
                            lfsr_mid_bid(lfs, tinfo.u.mdir.mid)
                                >> lfs->mdir_bits,
                            rid);
                    lfs->hasorphans = true;
                }
            }

        // found an mtree inner-node?
        } else if (tinfo.tag == LFSR_TAG_BRANCH) {
            // found the root of the mtree? keep track of this
            if (lfsr_mtree_isnull(&lfs->mtree)) {
                lfs->mtree.u.btree = tinfo.u.rbyd;
            }

        } else {
            LFS_UNREACHABLE();
        }
    }

    // once we've mounted and derived a pseudo-random seed, initialize our
    // block allocator
    //
    // the purpose of this is to avoid bad wear patterns such as always 
    // allocating blocks near the beginning of disk after a power-loss
    //
    lfs->lookahead.start = lfs->seed % lfs->cfg->block_count;

    // TODO should the consumegdelta above take gstate/gdelta as a parameter?
    // keep track of the current gstate on disk
    lfs_memcpy(lfs->grm_p, lfs->grm_d, LFSR_GRM_DSIZE);

    // decode grm so we can report any removed files as missing
    int err = lfsr_data_readgrm(lfs,
            &LFSR_DATA_BUF(lfs->grm_p, LFSR_GRM_DSIZE),
            &lfs->grm);
    if (err) {
        // TODO switch to read-only?
        return err;
    }

    if (lfsr_grm_hasrm(&lfs->grm)) {
        // found pending grms? this should only happen if we lost power
        if (lfsr_grm_count(&lfs->grm) == 2) {
            LFS_DEBUG("Found pending grm "
                    "%"PRId32".%"PRId32" %"PRId32".%"PRId32,
                    lfsr_mid_bid(lfs, lfs->grm.mids[0]) >> lfs->mdir_bits,
                    lfsr_mid_rid(lfs, lfs->grm.mids[0]),
                    lfsr_mid_bid(lfs, lfs->grm.mids[1]) >> lfs->mdir_bits,
                    lfsr_mid_rid(lfs, lfs->grm.mids[1]));
        } else if (lfsr_grm_count(&lfs->grm) == 1) {
            LFS_DEBUG("Found pending grm %"PRId32".%"PRId32,
                    lfsr_mid_bid(lfs, lfs->grm.mids[0]) >> lfs->mdir_bits,
                    lfsr_mid_rid(lfs, lfs->grm.mids[0]));
        }
    }

    return 0;
}

static int lfsr_formatinited(lfs_t *lfs) {
    for (int i = 0; i < 2; i++) {
        // write superblock to both rbyds in the root mroot to hopefully
        // avoid mounting an older filesystem on disk
        lfsr_rbyd_t rbyd = {.blocks[0]=i, .eoff=0, .trunk=0};

        int err = lfsr_bd_erase(lfs, rbyd.blocks[0]);
        if (err) {
            return err;
        }

        // the initial revision count is arbitrary, but it's nice to have
        // something here to tell the initial mroot apart from btree nodes
        // (rev=0), it's also useful for start with -1 and 0 in the upper
        // bits to help test overflow/sequence comparison
        uint32_t rev = (((uint32_t)i-1) << 28)
                | (((1 << (28-lfs_smax32(lfs->recycle_bits, 0)))-1)
                    & 0x00216968);
        err = lfsr_rbyd_appendrev(lfs, &rbyd, rev);
        if (err) {
            return err;
        }

        // our initial superblock contains a couple things:
        // - our magic string, "littlefs"
        // - any format-time configuration
        // - the root's bookmark tag, which reserves did = 0 for the root
        err = lfsr_rbyd_commit(lfs, &rbyd, -1, LFSR_ATTRS(
                LFSR_ATTR(
                    LFSR_TAG_MAGIC, 0,
                    LFSR_DATA_BUF("littlefs", 8)),
                LFSR_ATTR(
                    LFSR_TAG_VERSION, 0,
                    LFSR_DATA_BUF(((const uint8_t[2]){
                        LFS_DISK_VERSION_MAJOR,
                        LFS_DISK_VERSION_MINOR}), 2)),
                LFSR_ATTR(
                    LFSR_TAG_RCOMPAT, 0,
                    LFSR_DATA_RCOMPAT(LFSR_RCOMPAT_COMPAT)),
                LFSR_ATTR(
                    LFSR_TAG_GEOMETRY, 0,
                    LFSR_DATA_GEOMETRY((&(lfsr_geometry_t){
                        lfs->cfg->block_size,
                        lfs->cfg->block_count}))),
                LFSR_ATTR(
                    LFSR_TAG_NAMELIMIT, 0,
                    LFSR_DATA_LLEB128(lfs->name_limit)),
                LFSR_ATTR(
                    LFSR_TAG_FILELIMIT, 0,
                    LFSR_DATA_LEB128(lfs->file_limit)),
                LFSR_ATTR(
                    LFSR_TAG_BOOKMARK, +1,
                    LFSR_DATA_LEB128(0))));
        if (err) {
            return err;
        }
    }

    // test that mount works with our formatted disk
    int err = lfsr_mountinited(lfs);
    if (err) {
        return err;
    }

    return 0;
}

int lfsr_mount(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    err = lfsr_mountinited(lfs);
    if (err) {
        // make sure we clean up on error
        lfs_deinit(lfs);
        return err;
    }

    // TODO this should use any configured values
    LFS_DEBUG("Mounted littlefs v%"PRId32".%"PRId32" "
            "%"PRId32"x%"PRId32" "
            "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" "
            "w%"PRId32".%"PRId32,
            LFS_DISK_VERSION_MAJOR,
            LFS_DISK_VERSION_MINOR,
            lfs->cfg->block_size,
            lfs->cfg->block_count,
            lfs->mroot.rbyd.blocks[0],
            lfs->mroot.rbyd.blocks[1],
            lfsr_rbyd_trunk(&lfs->mroot.rbyd),
            lfsr_mtree_weight(&lfs->mtree) >> lfs->mdir_bits,
            1 << lfs->mdir_bits);

    return 0;
}

int lfsr_unmount(lfs_t *lfs) {
    return lfs_deinit(lfs);
}

int lfsr_format(lfs_t *lfs, const struct lfs_config *cfg) {
    int err = lfs_init(lfs, cfg);
    if (err) {
        return err;
    }

    LFS_DEBUG("Formatting littlefs v%"PRId32".%"PRId32" "
            "%"PRId32"x%"PRId32,
            LFS_DISK_VERSION_MAJOR,
            LFS_DISK_VERSION_MINOR,
            lfs->cfg->block_size,
            lfs->cfg->block_count);

    err = lfsr_formatinited(lfs);
    if (err) {
        // make sure we clean up on error
        lfs_deinit(lfs);
        return err;
    }

    return lfs_deinit(lfs);
}



/// Block allocator ///

// Allocations should call this when all allocated blocks are committed to
// the filesystem, either in the mtree or in tracked mdirs. After a
// checkpoint, the block allocator may realloc any untracked blocks.
static void lfs_alloc_ckpoint(lfs_t *lfs) {
    lfs->lookahead.ckpoint = lfs->cfg->block_count;
}

static inline void lfs_alloc_setinuse(lfs_t *lfs, lfs_block_t block) {
    // translate to lookahead-relative
    lfs_block_t rel = ((block + lfs->cfg->block_count) - lfs->lookahead.start)
            % lfs->cfg->block_count;
    if (rel < lfs->lookahead.size) {
        // mark as in-use
        lfs->lookahead.buffer[rel / 8] |= 1 << (rel % 8);
    }
}

static int lfs_alloc(lfs_t *lfs, lfs_block_t *block, bool erase) {
    while (true) {
        // scan our lookahead buffer for free blocks
        while (lfs->lookahead.next < lfs->lookahead.size) {
            if (!(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                    & (1 << (lfs->lookahead.next % 8)))) {
                // found a free block
                *block = (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->cfg->block_count;

                // erase requested?
                if (erase) {
                    int err = lfsr_bd_erase(lfs, *block);
                    if (err) {
                        // bad erase? try another block
                        if (err == LFS_ERR_CORRUPT) {
                            goto next;
                        }
                        return err;
                    }
                }

                // eagerly find next free block to maximize how many blocks
                // lfs_alloc_ckpoint makes available for scanning
                while (true) {
                    lfs->lookahead.next += 1;
                    lfs->lookahead.ckpoint -= 1;

                    if (lfs->lookahead.next >= lfs->lookahead.size
                            || !(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                                & (1 << (lfs->lookahead.next % 8)))) {
                        break;
                    }
                }

                return 0;
            }

        next:;
            lfs->lookahead.next += 1;
            lfs->lookahead.ckpoint -= 1;
        }

        // In order to keep our block allocator from spinning forever when our
        // filesystem is full, we mark points where there are no in-flight
        // allocations with a checkpoint before starting a set of allocaitons.
        //
        // If we've looked at all blocks since the last checkpoint, we report
        // the filesystem as out of storage.
        //
        if (lfs->lookahead.ckpoint <= 0) {
            LFS_ERROR("No more free space 0x%"PRIx32,
                    (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->cfg->block_count);
            return LFS_ERR_NOSPC;
        }

        // No blocks in our lookahead buffer, we need to scan the filesystem for
        // unused blocks in the next lookahead window.
        //
        // note we limit the lookahead window to at most the amount of blocks
        // checkpointed, this prevents the above math from underflowing
        //
        lfs->lookahead.start += lfs->lookahead.size;
        lfs->lookahead.next = 0;
        lfs->lookahead.size = lfs_min32(
                8*lfs->cfg->lookahead_size,
                lfs->lookahead.ckpoint);
        lfs_memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);

        // traverse the filesystem, building up knowledge of what blocks are
        // in use in our lookahead window
        lfsr_traversal_t t = LFSR_TRAVERSAL(LFSR_TRAVERSAL_ALL);
        while (true) {
            lfsr_tinfo_t tinfo;
            int err = lfsr_traversal_read(lfs, &t, &tinfo);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }

            // TODO add block pointers here?

            // mark any blocks we see at in-use, including any btree/mdir blocks
            if (tinfo.tag == LFSR_TAG_MDIR) {
                lfs_alloc_setinuse(lfs, tinfo.u.mdir.rbyd.blocks[1]);
                lfs_alloc_setinuse(lfs, tinfo.u.mdir.rbyd.blocks[0]);

            } else if (tinfo.tag == LFSR_TAG_BRANCH) {
                lfs_alloc_setinuse(lfs, tinfo.u.rbyd.blocks[0]);

            } else if (tinfo.tag == LFSR_TAG_BLOCK) {
                lfs_alloc_setinuse(lfs, tinfo.u.bptr.data.u.disk.block);

            } else {
                LFS_UNREACHABLE();
            }
        }
    }
}


/// Other filesystem traversal things  ///

lfs_ssize_t lfsr_fs_size(lfs_t *lfs) {
    lfs_size_t count = 0;
    lfsr_traversal_t t = LFSR_TRAVERSAL(LFSR_TRAVERSAL_ALL);
    while (true) {
        lfsr_tinfo_t tinfo;
        int err = lfsr_traversal_read(lfs, &t, &tinfo);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }

        // TODO add block pointers here?

        // count the number of blocks we see, yes this may result in duplicates
        if (tinfo.tag == LFSR_TAG_MDIR) {
            count += 2;

        } else if (tinfo.tag == LFSR_TAG_BRANCH) {
            count += 1;

        } else if (tinfo.tag == LFSR_TAG_BLOCK) {
            count += 1;

        } else {
            LFS_UNREACHABLE();
        }
    }

    return count;
}



/// Prepare the filesystem for mutation ///

static int lfsr_fs_fixgrm(lfs_t *lfs) {
    while (lfsr_grm_hasrm(&lfs->grm)) {
        // find our mdir
        lfsr_mdir_t mdir;
        LFS_ASSERT(lfs->grm.mids[0] < lfs_smax32(
                lfsr_mtree_weight(&lfs->mtree),
                1 << lfs->mdir_bits));
        int err = lfsr_mtree_lookup(lfs, &lfs->mtree, lfs->grm.mids[0],
                &mdir);
        if (err) {
            return err;
        }

        // we also use grm to track orphans that need to be cleaned up,
        // which means it may not match the on-disk state, which means
        // we need to revert manually on error
        lfsr_grm_t grm_p = lfs->grm;

        // mark grm as taken care of
        lfsr_grm_pop(&lfs->grm);

        // remove the rid while also updating our grm
        err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
                LFSR_ATTR(LFSR_TAG_RM, -1, LFSR_DATA_NULL())));
        if (err) {
            // revert grm manually
            lfs->grm = grm_p;
            return err;
        }
    }

    return 0;
}

static int lfsr_fs_fixorphans(lfs_t *lfs) {
    // traverse the filesystem and remove any orphaned files
    //
    // note this never takes longer than lfsr_mount
    //
    lfsr_mdir_t mdir;
    int err = lfsr_mtree_lookup(lfs, &lfs->mtree, 0, &mdir);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_NOENT);
        return err;
    }

    while (true) {
        // is this mid opened? skip
        if (!lfsr_mid_isopen(lfs, mdir.mid)) {
            // are we an orphaned file?
            err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_ORPHAN,
                    NULL);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT) {
                // remove orphaned file
                err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
                        LFSR_ATTR(LFSR_TAG_RM, -1, LFSR_DATA_NULL())));
                if (err) {
                    return err;
                }

                // seek in case our mdir was dropped
                err = lfsr_mtree_seek(lfs, &lfs->mtree, &mdir, 0);
                if (err) {
                    if (err == LFS_ERR_NOENT) {
                        break;
                    }
                    return err;
                }

                continue;
            }
        }

        // lookup next entry
        err = lfsr_mtree_seek(lfs, &lfs->mtree, &mdir, 1);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }
    }

    lfs->hasorphans = false;
    return 0;
}

static int lfsr_fs_preparemutation(lfs_t *lfs) {
    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);

    // fix pending grms
    bool inconsistent = false;
    if (lfsr_grm_hasrm(&lfs->grm)) {
        if (lfsr_grm_count(&lfs->grm) == 2) {
            LFS_DEBUG("Fixing grm "
                    "%"PRId32".%"PRId32" %"PRId32".%"PRId32"...",
                    lfsr_mid_bid(lfs, lfs->grm.mids[0]) >> lfs->mdir_bits,
                    lfsr_mid_rid(lfs, lfs->grm.mids[0]),
                    lfsr_mid_bid(lfs, lfs->grm.mids[1]) >> lfs->mdir_bits,
                    lfsr_mid_rid(lfs, lfs->grm.mids[1]));
        } else {
            LFS_DEBUG("Fixing grm %"PRId32".%"PRId32,
                    lfsr_mid_bid(lfs, lfs->grm.mids[0]) >> lfs->mdir_bits,
                    lfsr_mid_rid(lfs, lfs->grm.mids[0]));
        }
        inconsistent = true;

        int err = lfsr_fs_fixgrm(lfs);
        if (err) {
            return err;
        }

        // checkpoint the allocator again since fixgrm completed
        // some work
        lfs_alloc_ckpoint(lfs);
    }

    // fix orphaned files
    //
    // this must happen after fixgrm, since removing orphaned files risks
    // outdating the grm
    //
    if (lfs->hasorphans) {
        LFS_DEBUG("Fixing orphans...");
        inconsistent = true;

        int err = lfsr_fs_fixorphans(lfs);
        if (err) {
            return err;
        }

        // checkpoint the allocator again since fixorphans completed
        // some work
        lfs_alloc_ckpoint(lfs);
    }

    if (inconsistent) {
        LFS_DEBUG("littlefs is now consistent");
    }
    return 0;
}


/// Directory operations ///

// needed in lfsr_mkdir
static inline bool lfsr_f_iszombie(uint32_t flags);

int lfsr_mkdir(lfs_t *lfs, const char *path) {
    // prepare our filesystem for writing
    int err = lfsr_fs_preparemutation(lfs);
    if (err) {
        return err;
    }

    // lookup our parent
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    lfsr_did_t did;
    const char *name;
    lfs_size_t name_size;
    err = lfsr_mtree_pathlookup(lfs, &lfs->mtree, path,
            &mdir, &tag,
            &did, &name, &name_size);
    if (err && err != LFS_ERR_EXIST) {
        return err;
    }
    // already exists? note orphans don't really exist
    bool exists = (err == LFS_ERR_EXIST);
    if (exists && tag != LFSR_TAG_ORPHAN) {
        return LFS_ERR_EXIST;
    }

    // check that name fits
    if (name_size > lfs->name_limit) {
        return LFS_ERR_NAMETOOLONG;
    }

    // Our directory needs an arbitrary directory-id. To find one with
    // hopefully few collisions, we checksum our full path, but this is
    // arbitrary.
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
    // needs 3t+4 bytes for tag+alts (see our attr_estimate). And, if
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
    lfsr_did_t dmask = (1 << lfs_min32(
            lfs_nlog2(lfsr_mtree_weight(&lfs->mtree))
                + lfs_nlog2(lfs->cfg->block_size/32),
            31)) - 1;
    lfsr_did_t did_ = lfs_crc32c(0, path, lfs_strlen(path)) & dmask;

    // Check if we have a collision. If we do, search for the next
    // available did
    while (true) {
        err = lfsr_mtree_namelookup(lfs, &lfs->mtree,
                did_, NULL, 0,
                &mdir, NULL, NULL);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
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
    // This is done automatically by lfsr_mdir_commit to avoid issues with
    // mid updates, since the mid technically doesn't exist yet...

    // commit our bookmark and a grm to self-remove in case of powerloss
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
            LFSR_ATTR(LFSR_TAG_BOOKMARK, +1, LFSR_DATA_LEB128(did_))));
    if (err) {
        return err;
    }
    LFS_ASSERT(lfs->grm.mids[0] == mdir.mid);

    // committing our bookmark may have changed the mid of our metadata entry,
    // we need to look it up again, we can at least avoid the full path walk
    err = lfsr_mtree_namelookup(lfs, &lfs->mtree,
            did, name, name_size,
            &mdir, NULL, NULL);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    LFS_ASSERT((exists) ? !err : err == LFS_ERR_NOENT);

    // commit our new directory into our parent, zeroing the grm in the
    // process
    lfsr_grm_pop(&lfs->grm);
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
            LFSR_ATTR_NAME(
                LFSR_TAG_SUP | LFSR_TAG_DIR, (!exists) ? +1 : 0,
                did, name, name_size),
            LFSR_ATTR(LFSR_TAG_DID, 0, LFSR_DATA_LEB128(did_))));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        // mark any clobbered orphans as zombied
        if (exists
                && o->type == LFS_TYPE_REG
                && o->mdir.mid == mdir.mid) {
            o->flags = (o->flags & ~LFS_F_ORPHAN)
                    | LFS_F_ZOMBIE
                    | LFS_F_UNSYNC
                    | LFS_O_DESYNC;

        // update dir positions
        } else if (!exists
                && o->type == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == did
                && o->mdir.mid >= mdir.mid) {
            ((lfsr_dir_t*)o)->pos += 1;
        }
    }

    return 0;
}

int lfsr_remove(lfs_t *lfs, const char *path) {
    // prepare our filesystem for writing
    int err = lfsr_fs_preparemutation(lfs);
    if (err) {
        return err;
    }

    // lookup our entry
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    lfsr_did_t did;
    const char *name;
    lfs_size_t name_size;
    err = lfsr_mtree_pathlookup(lfs, &lfs->mtree, path,
            &mdir, &tag,
            &did, &name, &name_size);
    if (err && err != LFS_ERR_EXIST) {
        return err;
    }
    // doesn't exist? note orphans don't really exist
    if (!err || tag == LFSR_TAG_ORPHAN) {
        return LFS_ERR_NOENT;
    }

    // if we're removing a directory, we need to also remove the
    // bookmark entry
    lfsr_did_t did_ = 0;
    if (tag == LFSR_TAG_DIR) {
        // first lets figure out the did
        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_DID,
                &data);
        if (err) {
            return err;
        }

        err = lfsr_data_readleb128(lfs, &data, &did_);
        if (err) {
            return err;
        }

        // then lookup the bookmark entry
        lfsr_mdir_t bookmark_mdir;
        err = lfsr_mtree_namelookup(lfs, &lfs->mtree,
                did_, NULL, 0,
                &bookmark_mdir, NULL, NULL);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }
        lfsr_mid_t bookmark_mid = bookmark_mdir.mid;

        // check that the directory is empty
        err = lfsr_mtree_seek(lfs, &lfs->mtree, &bookmark_mdir, 1);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        if (err != LFS_ERR_NOENT) {
            lfsr_tag_t bookmark_tag;
            err = lfsr_mdir_sublookup(lfs, &bookmark_mdir, LFSR_TAG_NAME,
                    &bookmark_tag, NULL);
            if (err) {
                return err;
            }

            if (bookmark_tag != LFSR_TAG_BOOKMARK) {
                return LFS_ERR_NOTEMPTY;
            }
        }

        // create a grm to remove the bookmark entry
        lfs->grm.mids[0] = bookmark_mid;
    }

    // are we removing an opened file?
    bool zombie = lfsr_mid_isopen(lfs, mdir.mid);

    // remove the metadata entry
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
            // create an orphan if zombied
            //
            // we use a create+delete here to also clear any attrs
            // and trim the entry size
            (zombie)
                ? LFSR_ATTR_NAME(
                    LFSR_TAG_SUP | LFSR_TAG_ORPHAN, 0,
                    did, name, name_size)
                : LFSR_ATTR(
                    LFSR_TAG_RM, -1, LFSR_DATA_NULL())));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        // mark any clobbered orphans as zombied orphans
        if (zombie
                && o->type == LFS_TYPE_REG
                && o->mdir.mid == mdir.mid) {
            o->flags |= LFS_F_ORPHAN
                    | LFS_F_ZOMBIE
                    | LFS_F_UNSYNC
                    | LFS_O_DESYNC;

        // mark any removed dirs as zombies
        } else if (did_
                && o->type == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == did_) {
            o->flags |= LFS_F_ZOMBIE;

        // update dir positions
        } else if (o->type == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == did
                && o->mdir.mid >= mdir.mid) {
            ((lfsr_dir_t*)o)->pos -= 1;
        }
    }

    // if we were a directory, we need to clean up, fortunately we can leave
    // this up to lfsr_fs_fixgrm
    return lfsr_fs_fixgrm(lfs);
}

int lfsr_rename(lfs_t *lfs, const char *old_path, const char *new_path) {
    // prepare our filesystem for writing
    int err = lfsr_fs_preparemutation(lfs);
    if (err) {
        return err;
    }

    // lookup old entry
    lfsr_mdir_t old_mdir;
    lfsr_tag_t old_tag;
    lfsr_did_t old_did;
    err = lfsr_mtree_pathlookup(lfs, &lfs->mtree, old_path,
            &old_mdir, &old_tag,
            &old_did, NULL, NULL);
    if (err && err != LFS_ERR_EXIST) {
        return err;
    }
    // doesn't exist? note orphans don't really exist
    if (!err || old_tag == LFSR_TAG_ORPHAN) {
        return LFS_ERR_NOENT;
    }

    // lookup new entry
    lfsr_mdir_t new_mdir;
    lfsr_tag_t new_tag;
    lfsr_did_t new_did;
    const char *new_name;
    lfs_size_t new_name_size;
    err = lfsr_mtree_pathlookup(lfs, &lfs->mtree, new_path,
            &new_mdir, &new_tag,
            &new_did, &new_name, &new_name_size);
    if (err && err != LFS_ERR_EXIST) {
        return err;
    }
    // already exists?
    bool exists = (err == LFS_ERR_EXIST);
    lfsr_did_t new_did_ = 0;

    // there are a few cases we need to watch out for
    if (!exists) {
        // check that name fits
        if (new_name_size > lfs->name_limit) {
            return LFS_ERR_NAMETOOLONG;
        }

    } else {
        // renaming different types is an error
        //
        // unless we found a orphan, these don't really exist
        if (old_tag != new_tag && new_tag != LFSR_TAG_ORPHAN) {
            return (new_tag == LFSR_TAG_DIR)
                    ? LFS_ERR_ISDIR
                    : LFS_ERR_NOTDIR;
        }

        // TODO is it? is this check necessary?
        // renaming to ourself is a noop
        if (old_mdir.mid == new_mdir.mid) {
            return 0;
        }

        // if our destination is a directory, we will be implicitly removing
        // the directory, we need to create a grm for this
        if (new_tag == LFSR_TAG_DIR) {
            // TODO deduplicate the isempty check with lfsr_remove?
            // first lets figure out the did
            lfsr_data_t data;
            err = lfsr_mdir_lookup(lfs, &new_mdir, LFSR_TAG_DID,
                    &data);
            if (err) {
                return err;
            }

            err = lfsr_data_readleb128(lfs, &data, &new_did_);
            if (err) {
                return err;
            }

            // then lookup the bookmark entry
            lfsr_mdir_t bookmark_mdir;
            err = lfsr_mtree_namelookup(lfs, &lfs->mtree,
                    new_did_, NULL, 0,
                    &bookmark_mdir, NULL, NULL);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
            lfsr_mid_t bookmark_mid = bookmark_mdir.mid;

            // check that the directory is empty
            err = lfsr_mtree_seek(lfs, &lfs->mtree, &bookmark_mdir, 1);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT) {
                lfsr_tag_t bookmark_tag;
                err = lfsr_mdir_sublookup(lfs, &bookmark_mdir, LFSR_TAG_NAME,
                        &bookmark_tag, NULL);
                if (err) {
                    return err;
                }

                if (bookmark_tag != LFSR_TAG_BOOKMARK) {
                    return LFS_ERR_NOTEMPTY;
                }
            }

            // mark bookmark entry for removal with a grm
            lfs->grm.mids[1] = bookmark_mid;
        }
    }

    // mark old entry for removal with a grm
    lfs->grm.mids[0] = old_mdir.mid;

    // rename our entry, copying all tags associated with the old rid to the
    // new rid, while also marking the old rid for removal
    err = lfsr_mdir_commit(lfs, &new_mdir, LFSR_ATTRS(
            LFSR_ATTR_NAME(
                LFSR_TAG_SUP | old_tag, (!exists) ? +1 : 0,
                new_did, new_name, new_name_size),
            LFSR_ATTR_MOVE(LFSR_TAG_MOVE, 0, &old_mdir)));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        // mark any clobbered orphans as zombied
        if (exists
                && o->type == LFS_TYPE_REG
                && o->mdir.mid == new_mdir.mid) {
            o->flags = (o->flags & ~LFS_F_ORPHAN)
                    | LFS_F_ZOMBIE
                    | LFS_F_UNSYNC
                    | LFS_O_DESYNC;

        // update moved files with the new mdir
        } else if (o->type == LFS_TYPE_REG
                && o->mdir.mid == lfs->grm.mids[0]) {
            o->mdir = new_mdir;

        // mark any removed dirs as zombies
        } else if (new_did_
                && o->type == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == new_did_) {
            o->flags |= LFS_F_ZOMBIE;

        // update dir positions
        } else if (o->type == LFS_TYPE_DIR) {
            if (!exists
                    && ((lfsr_dir_t*)o)->did == new_did
                    && o->mdir.mid >= new_mdir.mid) {
                ((lfsr_dir_t*)o)->pos += 1;
            }

            if (((lfsr_dir_t*)o)->did == old_did
                    && o->mdir.mid >= lfs->grm.mids[0]) {
                ((lfsr_dir_t*)o)->pos -= 1;
            }
        }
    }

    // we need to clean up any pending grms, fortunately we can leave
    // this up to lfsr_fs_fixgrm
    return lfsr_fs_fixgrm(lfs);
}

// this just populates the info struct based on what we found
static int lfsr_stat_(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag, lfsr_data_t name,
        struct lfs_info *info) {
    // get file type from the tag
    info->type = lfsr_tag_subtype(tag);

    // read the file name
    LFS_ASSERT(lfsr_data_size(name) <= LFS_NAME_MAX);
    lfs_ssize_t name_size = lfsr_data_read(lfs, &name,
            info->name, LFS_NAME_MAX);
    if (name_size < 0) {
        return name_size;
    }
    info->name[name_size] = '\0';

    // get file size if we're a regular file, this gets a bit messy
    // because of the different file representations
    info->size = 0;
    if (tag == LFSR_TAG_REG) {
        // inlined?
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_mdir_lookupnext(lfs, mdir, LFSR_TAG_DATA,
                &tag, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // may be a sprout (simple inlined data)
        if (err != LFS_ERR_NOENT && tag == LFSR_TAG_DATA) {
            info->size = lfsr_data_size(data);

        // or a block/bshrub/btree, size is always first field here
        } else if (err != LFS_ERR_NOENT
                && (tag == LFSR_TAG_BLOCK
                    || tag == LFSR_TAG_BSHRUB
                    || tag == LFSR_TAG_BTREE)) {
            err = lfsr_data_readleb128(lfs, &data, &info->size);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

int lfsr_stat(lfs_t *lfs, const char *path, struct lfs_info *info) {
    // lookup our entry
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    const char *name;
    lfs_size_t name_size;
    int err = lfsr_mtree_pathlookup(lfs, &lfs->mtree, path,
            &mdir, &tag,
            NULL, &name, &name_size);
    if (err && err != LFS_ERR_EXIST && err != LFS_ERR_INVAL) {
        return err;
    }
    // doesn't exist? note orphans don't really exist
    if (!err || tag == LFSR_TAG_ORPHAN) {
        return LFS_ERR_NOENT;
    }

    // special case for root
    if (err == LFS_ERR_INVAL) {
        lfs_strcpy(info->name, "/");
        info->type = LFS_TYPE_DIR;
        info->size = 0;
        return 0;
    }

    // fill out our info struct
    return lfsr_stat_(lfs, &mdir,
            tag, LFSR_DATA_BUF(name, name_size),
            info);
}

int lfsr_dir_open(lfs_t *lfs, lfsr_dir_t *dir, const char *path) {
    // setup dir state
    dir->o.type = LFS_TYPE_DIR;
    dir->o.flags = 0;

    // lookup our directory
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    int err = lfsr_mtree_pathlookup(lfs, &lfs->mtree, path,
            &mdir, &tag,
            NULL, NULL, NULL);
    if (err && err != LFS_ERR_EXIST && err != LFS_ERR_INVAL) {
        return err;
    }
    // doesn't exist? note orphans don't really exist
    if (!err || tag == LFSR_TAG_ORPHAN) {
        return LFS_ERR_NOENT;
    }

    // read our did from the mdir, unless we're root
    if (err == LFS_ERR_INVAL) {
        dir->did = 0;

    } else {
        // not a directory?
        if (tag != LFSR_TAG_DIR) {
            return LFS_ERR_NOTDIR;
        }

        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_DID,
                &data);
        if (err) {
            return err;
        }

        err = lfsr_data_readleb128(lfs, &data, &dir->did);
        if (err) {
            return err;
        }
    }

    // let rewind initialize the pos state
    err = lfsr_dir_rewind(lfs, dir);
    if (err) {
        return err;
    }

    // add to tracked mdirs
    lfsr_opened_add(lfs, &dir->o);
    return 0;
}

int lfsr_dir_close(lfs_t *lfs, lfsr_dir_t *dir) {
    // remove from tracked mdirs
    lfsr_opened_remove(lfs, &dir->o);
    return 0;
}

int lfsr_dir_read(lfs_t *lfs, lfsr_dir_t *dir, struct lfs_info *info) {
    // was our dir removed?
    if (lfsr_f_iszombie(dir->o.flags)) {
        return LFS_ERR_NOENT;
    }

    // handle dots specially
    if (dir->pos == 0) {
        lfs_strcpy(info->name, ".");
        info->type = LFS_TYPE_DIR;
        info->size = 0;
        dir->pos += 1;
        return 0;
    } else if (dir->pos == 1) {
        lfs_strcpy(info->name, "..");
        info->type = LFS_TYPE_DIR;
        info->size = 0;
        dir->pos += 1;
        return 0;
    }

    // seek in case our mdir was dropped
    int err = lfsr_mtree_seek(lfs, &lfs->mtree, &dir->o.mdir, 0);
    if (err) {
        return err;
    }

    while (true) {
        // lookup the next name tag
        lfsr_tag_t tag;
        lfsr_data_t data;
        err = lfsr_mdir_sublookup(lfs, &dir->o.mdir, LFSR_TAG_NAME,
                &tag, &data);
        if (err) {
            return err;
        }

        // get the did
        lfsr_did_t did;
        err = lfsr_data_readleb128(lfs, &data, &did);
        if (err) {
            return err;
        }

        // did mismatch? this terminates the dir read
        if (did != dir->did) {
            return LFS_ERR_NOENT;
        }

        // skip orphans, we pretend these don't exist
        if (tag != LFSR_TAG_ORPHAN) {
            // fill out our info struct
            err = lfsr_stat_(lfs, &dir->o.mdir, tag, data,
                    info);
            if (err) {
                return err;
            }
        }

        // eagerly look up the next entry
        err = lfsr_mtree_seek(lfs, &lfs->mtree, &dir->o.mdir, 1);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        dir->pos += 1;

        if (tag != LFSR_TAG_ORPHAN) {
            return 0;
        }
    }
}

int lfsr_dir_seek(lfs_t *lfs, lfsr_dir_t *dir, lfs_soff_t off) {
    // do nothing if removed
    if (lfsr_f_iszombie(dir->o.flags)) {
        return 0;
    }

    // first rewind
    int err = lfsr_dir_rewind(lfs, dir);
    if (err) {
        return err;
    }

    // then seek to the requested offset, we leave it up to lfsr_mtree_seek
    // to make this efficient
    //
    // note the -2 to adjust for dot entries
    if (off > 2) {
        err = lfsr_mtree_seek(lfs, &lfs->mtree, &dir->o.mdir, off - 2);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
    }
    dir->pos = off;

    return 0;
}

lfs_soff_t lfsr_dir_tell(lfs_t *lfs, lfsr_dir_t *dir) {
    (void)lfs;
    return dir->pos;
}

int lfsr_dir_rewind(lfs_t *lfs, lfsr_dir_t *dir) {
    // do nothing if removed
    if (lfsr_f_iszombie(dir->o.flags)) {
        return 0;
    }

    // reset pos
    dir->pos = 0;

    // lookup our bookmark in the mtree
    int err = lfsr_mtree_namelookup(lfs, &lfs->mtree,
            dir->did, NULL, 0,
            &dir->o.mdir, NULL, NULL);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_NOENT);
        return err;
    }

    // eagerly lookup the next entry
    err = lfsr_mtree_seek(lfs, &lfs->mtree, &dir->o.mdir, 1);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    return 0;
}





/// File operations ///

#define LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR 0x80000000

#define LFSR_BSHRUB_BNULL() \
        ((lfsr_bshrub_t){.u.size=(LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR | 0)})

static inline bool lfsr_bshrub_isbnull(const lfsr_bshrub_t *bshrub) {
    return (lfs_size_t)bshrub->u.size
            == (LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR | 0);
}

static inline bool lfsr_bshrub_isbsprout(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub) {
    return (lfs_size_t)bshrub->u.size
                > (LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR | 0)
            && bshrub->u.bsprout.u.disk.block == mdir->rbyd.blocks[0];
}

static inline bool lfsr_bshrub_isbptr(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub) {
    return (lfs_size_t)bshrub->u.size
                > (LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR | 0)
            && bshrub->u.bsprout.u.disk.block != mdir->rbyd.blocks[0];
}

static inline bool lfsr_bshrub_isbshrub(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub) {
    return !(bshrub->u.size & LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR)
            && bshrub->u.bshrub.blocks[0] == mdir->rbyd.blocks[0];
}

static inline bool lfsr_bshrub_isbtree(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub) {
    return !(bshrub->u.size & LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR)
            && bshrub->u.bshrub.blocks[0] != mdir->rbyd.blocks[0];
}

static inline bool lfsr_bshrub_isbnullorbsproutorbptr(
        const lfsr_bshrub_t *bshrub) {
    return bshrub->u.size & LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR;
}

static inline bool lfsr_bshrub_isbshruborbtree(
        const lfsr_bshrub_t *bshrub) {
    return !(bshrub->u.size & LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR);
}

// the on-disk size/weight lines up to the same word across all unions
static inline lfs_off_t lfsr_bshrub_size(const lfsr_bshrub_t *bshrub) {
    return bshrub->u.size & ~LFSR_BSHRUB_ISBNULLORBSPROUTORBPTR;
}

// flag things
static inline bool lfsr_o_isrdonly(uint32_t flags) {
    return (flags & 3) == LFS_O_RDONLY;
}

static inline bool lfsr_o_iswronly(uint32_t flags) {
    return (flags & 3) == LFS_O_WRONLY;
}

static inline bool lfsr_o_iscreat(uint32_t flags) {
    return flags & LFS_O_CREAT;
}

static inline bool lfsr_o_isexcl(uint32_t flags) {
    return flags & LFS_O_EXCL;
}

static inline bool lfsr_o_istrunc(uint32_t flags) {
    return flags & LFS_O_TRUNC;
}

static inline bool lfsr_o_isappend(uint32_t flags) {
    return flags & LFS_O_APPEND;
}

static inline bool lfsr_o_issync(uint32_t flags) {
    return flags & LFS_O_SYNC;
}

static inline bool lfsr_o_isdesync(uint32_t flags) {
    return flags & LFS_O_DESYNC;
}

static inline bool lfsr_o_isflush(uint32_t flags) {
    return flags & LFS_O_FLUSH;
}

static inline bool lfsr_f_isunflush(uint32_t flags) {
    return flags & LFS_F_UNFLUSH;
}

static inline bool lfsr_f_isunsync(uint32_t flags) {
    return flags & LFS_F_UNSYNC;
}

static inline bool lfsr_f_isorphan(uint32_t flags) {
    return flags & LFS_F_ORPHAN;
}

static inline bool lfsr_f_iszombie(uint32_t flags) {
    return flags & LFS_F_ZOMBIE;
}

// other file helpers
static inline lfs_size_t lfsr_file_buffersize(lfs_t *lfs,
        const lfsr_file_t *file) {
    return (file->cfg->buffer_size)
            ? file->cfg->buffer_size
            : lfs->cfg->fbuffer_size;
}

static inline lfs_size_t lfsr_file_inlinesize(lfs_t *lfs,
        const lfsr_file_t *file) {
    return lfs_min32(
            lfsr_file_buffersize(lfs, file),
            lfs_min32(
                lfs->cfg->inline_size,
                lfs->cfg->fragment_size));
}

static inline lfs_off_t lfsr_file_size_(const lfsr_file_t *file) {
    return lfs_max32(
            file->buffer.pos + file->buffer.size,
            lfsr_bshrub_size(&file->bshrub));
}

// file operations

// needed in lfsr_file_opencfg
static lfs_ssize_t lfsr_bshrub_read(lfs_t *lfs, const lfsr_file_t *file,
        lfs_off_t pos, uint8_t *buffer, lfs_size_t size);

int lfsr_file_opencfg(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs_file_config *cfg) {
    // don't allow the forbidden mode!
    LFS_ASSERT((flags & 3) != 3);
    // these flags require a writable file
    LFS_ASSERT(!lfsr_o_isrdonly(flags) || !lfsr_o_iscreat(flags));
    LFS_ASSERT(!lfsr_o_isrdonly(flags) || !lfsr_o_isexcl(flags));
    LFS_ASSERT(!lfsr_o_isrdonly(flags) || !lfsr_o_istrunc(flags));
    LFS_ASSERT(!lfsr_o_isrdonly(flags) || !lfsr_o_isappend(flags));
    // these flags are internal and shouldn't be provided by the user
    LFS_ASSERT(!lfsr_f_isunflush(flags));
    LFS_ASSERT(!lfsr_f_isunsync(flags));
    LFS_ASSERT(!lfsr_f_isorphan(flags));

    if (!lfsr_o_isrdonly(flags)) {
        // prepare our filesystem for writing
        int err = lfsr_fs_preparemutation(lfs);
        if (err) {
            return err;
        }
    }

    // setup file state
    file->o.type = LFS_TYPE_REG;
    file->o.flags = flags;
    file->cfg = cfg;
    file->pos = 0;
    file->eblock = 0;
    file->eoff = -1;
    // default data state
    file->bshrub = LFSR_BSHRUB_BNULL();

    // lookup our parent
    lfsr_tag_t tag;
    lfsr_did_t did;
    const char *name;
    lfs_size_t name_size;
    int err = lfsr_mtree_pathlookup(lfs, &lfs->mtree, path,
            &file->o.mdir, &tag,
            &did, &name, &name_size);
    if (err && err != LFS_ERR_EXIST) {
        return err;
    }

    // creating a new entry?
    if (!err || tag == LFSR_TAG_ORPHAN) {
        if (!lfsr_o_iscreat(flags)) {
            return LFS_ERR_NOENT;
        }
        LFS_ASSERT(!lfsr_o_isrdonly(flags));

        // check that name fits
        if (name_size > lfs->name_limit) {
            return LFS_ERR_NAMETOOLONG;
        }

        // create an orphan entry if we don't have one, this reserves the
        // mid until first sync
        if (!err) {
            err = lfsr_mdir_commit(lfs, &file->o.mdir, LFSR_ATTRS(
                    LFSR_ATTR_NAME(
                        LFSR_TAG_ORPHAN, +1,
                        did, name, name_size)));
            if (err) {
                return err;
            }

            // update dir positions
            for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
                if (o->type == LFS_TYPE_DIR
                        && ((lfsr_dir_t*)o)->did == did
                        && o->mdir.mid >= file->o.mdir.mid) {
                    ((lfsr_dir_t*)o)->pos += 1;
                }
            }
        }

        // mark as unsynced and orphaned, we need to convert to reg file
        // on first sync
        file->o.flags |= LFS_F_UNSYNC | LFS_F_ORPHAN;

    } else {
        if (lfsr_o_isexcl(flags)) {
            // oh, we really wanted to create a new entry
            return LFS_ERR_EXIST;
        }

        // wrong type?
        if (tag != LFSR_TAG_REG) {
            return LFS_ERR_ISDIR;
        }

        // if we're truncating don't bother to read any state, we're
        // just going to truncate after all
        if (!lfsr_o_istrunc(flags)) {
            // read any inlined state
            lfsr_tag_t tag;
            lfsr_data_t data;
            err = lfsr_mdir_lookupnext(lfs, &file->o.mdir, LFSR_TAG_DATA,
                    &tag, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            // TODO the above clobbers data on failure, which is why we can't
            // lookup into the inlined data directly. Should this be avoided?
            // Should we at least be consistent in this codebase?

            // may be a sprout (simple inlined data)
            if (err != LFS_ERR_NOENT && tag == LFSR_TAG_DATA) {
                file->bshrub.u.bsprout = data;

            // or a direct block
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BLOCK) {
                err = lfsr_data_readbptr(lfs, &data,
                        &file->bshrub.u.bptr);
                if (err) {
                    return err;
                }

            // or a bshrub (inlined btree)
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
                err = lfsr_data_readshrub(lfs, &data, &file->o.mdir,
                        &file->bshrub.u.bshrub);
                if (err) {
                    return err;
                }

            // or a btree
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BTREE) {
                err = lfsr_data_readbtree(lfs, &data, &file->bshrub.u.btree);
                if (err) {
                    return err;
                }
            }
        }
    }

    // allocate buffer if necessary
    if (file->cfg->buffer) {
        file->buffer.buffer = file->cfg->buffer;
    } else {
        file->buffer.buffer = lfs_malloc(lfsr_file_buffersize(lfs, file));
        if (!file->buffer.buffer) {
            return LFS_ERR_NOMEM;
        }
    }
    file->buffer.pos = 0;
    file->buffer.size = 0;

    // if our file is small, try to keep the whole thing in our buffer
    if (lfsr_bshrub_size(&file->bshrub) <= lfsr_file_inlinesize(lfs, file)) {
        lfs_ssize_t d = lfsr_bshrub_read(lfs, file,
                0, file->buffer.buffer, lfsr_bshrub_size(&file->bshrub));
        if (d < 0) {
            err = d;
            goto failed;
        }

        // small files remain perpetually unflushed
        file->o.flags |= LFS_F_UNFLUSH;
        file->buffer.pos = 0;
        file->buffer.size = lfsr_bshrub_size(&file->bshrub);
        file->bshrub = LFSR_BSHRUB_BNULL();
    }

    // add to tracked mdirs
    lfsr_opened_add(lfs, &file->o);
    return 0;

failed:;
    // clean up memory
    if (!file->cfg->buffer) {
        lfs_free(file->buffer.buffer);
    }

    return err;
}

// default file config
static const struct lfs_file_config lfsr_file_defaults = {0};

int lfsr_file_open(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags) {
    return lfsr_file_opencfg(lfs, file, path, flags, &lfsr_file_defaults);
}

// needed in lfsr_file_close
int lfsr_file_sync(lfs_t *lfs, lfsr_file_t *file);

int lfsr_file_close(lfs_t *lfs, lfsr_file_t *file) {
    // don't call lfsr_file_sync if we're readonly or desynced
    int err = 0;
    if (!lfsr_o_isrdonly(file->o.flags)
            && !lfsr_o_isdesync(file->o.flags)) {
        err = lfsr_file_sync(lfs, file);
    }

    // remove from tracked mdirs
    lfsr_opened_remove(lfs, &file->o);

    // clean up memory
    if (!file->cfg->buffer) {
        lfs_free(file->buffer.buffer);
    }

    // are we orphaning a file?
    //
    // make sure we check _after_ removing ourselves
    if (lfsr_f_isorphan(file->o.flags)
            && !lfsr_mid_isopen(lfs, file->o.mdir.mid)) {
        // this gets a bit messy, since we're not able to write to the
        // filesystem if we're rdonly or desynced, fortunately we have
        // a few tricks

        // first try to push onto our grm queue
        if (lfsr_grm_count(&lfs->grm) < 2) {
            lfsr_grm_push(&lfs->grm, file->o.mdir.mid);

        // fallback to just marking the filesystem as orphaned
        } else {
            lfs->hasorphans = true;
        }
    }

    return err;
}

// low-level file operations

// find a tight upper bound on the _full_ bshrub size, this includes
// any on-disk bshrubs, and all pending bshrubs
static lfs_ssize_t lfsr_bshrub_estimate(lfs_t *lfs, const lfsr_file_t *file) {
    lfs_size_t estimate = 0;

    // include all unique sprouts/shrubs related to our file,
    // including the on-disk sprout/shrub
    lfsr_tag_t tag;
    lfsr_data_t data;
    int err = lfsr_mdir_lookupnext(lfs, &file->o.mdir, LFSR_TAG_DATA,
            &tag, &data);
    if (err && err != LFS_ERR_NOENT) {
        LFS_ASSERT(err < 0);
        return err;
    }

    if (err != LFS_ERR_NOENT && tag == LFSR_TAG_DATA) {
        lfs_ssize_t dsize = lfsr_sprout_estimate(lfs, &data);
        if (dsize < 0) {
            return dsize;
        }
        estimate += dsize;

    } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
        lfsr_shrub_t shrub;
        err = lfsr_data_readshrub(lfs, &data, &file->o.mdir,
                &shrub);
        if (err) {
            LFS_ASSERT(err < 0);
            return err;
        }

        lfs_ssize_t dsize = lfsr_shrub_estimate(lfs, &shrub);
        if (dsize < 0) {
            return dsize;
        }
        estimate += dsize;
    }

    // this includes our current shrub
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        lfsr_file_t *file_ = (lfsr_file_t*)o;
        if (file_->o.type == LFS_TYPE_REG
                && file_->o.mdir.mid == file->o.mdir.mid) {
            if (lfsr_bshrub_isbsprout(&file_->o.mdir, &file_->bshrub)) {
                lfs_ssize_t dsize = lfsr_sprout_estimate(lfs,
                        &file_->bshrub.u.bsprout);
                if (dsize < 0) {
                    return dsize;
                }
                estimate += dsize;

            } else if (lfsr_bshrub_isbshrub(&file_->o.mdir, &file_->bshrub)) {
                lfs_ssize_t dsize = lfsr_shrub_estimate(lfs,
                        &file_->bshrub.u.bshrub);
                if (dsize < 0) {
                    return dsize;
                }
                estimate += dsize;
            }
        }
    }

    return estimate;
}

static int lfsr_bshrub_lookupnext(lfs_t *lfs, const lfsr_file_t *file,
        lfs_off_t pos,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bid_t *weight_,
        lfsr_bptr_t *bptr_) {
    if (pos >= lfsr_bshrub_size(&file->bshrub)) {
        return LFS_ERR_NOENT;
    }
    // the above size check should make this impossible
    LFS_ASSERT(!lfsr_bshrub_isbnull(&file->bshrub));

    // inlined sprout?
    if (lfsr_bshrub_isbsprout(&file->o.mdir, &file->bshrub)) {
        if (bid_) {
            *bid_ = lfsr_data_size(file->bshrub.u.bsprout)-1;
        }
        if (tag_) {
            *tag_ = LFSR_TAG_DATA;
        }
        if (weight_) {
            *weight_ = lfsr_data_size(file->bshrub.u.bsprout);
        }
        if (bptr_) {
            bptr_->data = file->bshrub.u.bsprout;
        }
        return 0;

    // block pointer?
    } else if (lfsr_bshrub_isbptr(&file->o.mdir, &file->bshrub)) {
        if (bid_) {
            *bid_ = lfsr_data_size(file->bshrub.u.bptr.data)-1;
        }
        if (tag_) {
            *tag_ = LFSR_TAG_BLOCK;
        }
        if (weight_) {
            *weight_ = lfsr_data_size(file->bshrub.u.bptr.data);
        }
        if (bptr_) {
            *bptr_ = file->bshrub.u.bptr;
        }
        return 0;

    // bshrub/btree?
    } else if (lfsr_bshrub_isbshruborbtree(&file->bshrub)) {
        lfsr_bid_t bid;
        lfsr_rbyd_t rbyd;
        lfsr_srid_t rid;
        lfsr_tag_t tag;
        lfsr_bid_t weight;
        lfsr_data_t data;
        int err = lfsr_btree_lookupnext_(lfs, &file->bshrub.u.btree, pos,
                &bid, &rbyd, &rid, &tag, &weight, &data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }
        LFS_ASSERT(tag == LFSR_TAG_DATA
                || tag == LFSR_TAG_BLOCK);

        if (bid_) {
            *bid_ = bid;
        }
        if (tag_) {
            *tag_ = tag;
        }
        if (weight_) {
            *weight_ = weight;
        }
        if (bptr_) {
            // decode bptrs
            if (tag == LFSR_TAG_DATA) {
                bptr_->data = data;
            } else {
                err = lfsr_data_readbptr(lfs, &data, bptr_);
                if (err) {
                    return err;
                }
            }
            LFS_ASSERT(lfsr_data_size(bptr_->data) <= weight);
        }
        return 0;

    } else {
        LFS_UNREACHABLE();
    }
}

static int lfsr_bshrub_traverse(lfs_t *lfs, const lfsr_file_t *file,
        lfsr_btraversal_t *t,
        lfsr_bid_t *bid_, lfsr_tinfo_t *tinfo_) {
    // bnull/bsprout do nothing
    if (lfsr_bshrub_isbnull(&file->bshrub)
            || lfsr_bshrub_isbsprout(&file->o.mdir, &file->bshrub)) {
        return LFS_ERR_NOENT;
    }

    // block pointer?
    if (lfsr_bshrub_isbptr(&file->o.mdir, &file->bshrub)) {
        if (t->bid > 0) {
            return LFS_ERR_NOENT;
        }

        if (bid_) {
            *bid_ = lfsr_data_size(file->bshrub.u.bptr.data)-1;
        }
        if (tinfo_) {
            tinfo_->tag = LFSR_TAG_BLOCK;
            tinfo_->u.bptr = file->bshrub.u.bptr;
        }
        return 0;

    // bshrub/btree?
    } else if (lfsr_bshrub_isbshruborbtree(&file->bshrub)) {
        int err = lfsr_btree_traverse_(lfs, &file->bshrub.u.btree, t,
                bid_, tinfo_);
        if (err) {
            return err;
        }

        // decode bptrs
        if (tinfo_ && tinfo_->tag == LFSR_TAG_BLOCK) {
            lfsr_bptr_t bptr;
            err = lfsr_data_readbptr(lfs, &tinfo_->u.data,
                    &bptr);
            if (err) {
                return err;
            }
            tinfo_->u.bptr = bptr;
        }
        return 0;

    } else {
        LFS_UNREACHABLE();
    }
}

static lfs_ssize_t lfsr_bshrub_readnext(lfs_t *lfs, const lfsr_file_t *file,
        lfs_off_t pos, uint8_t *buffer, lfs_size_t size) {
    lfs_off_t pos_ = pos;
    // read one btree entry
    lfsr_bid_t bid;
    lfsr_tag_t tag;
    lfsr_bid_t weight;
    lfsr_bptr_t bptr;
    int err = lfsr_bshrub_lookupnext(lfs, file, pos_,
            &bid, &tag, &weight, &bptr);
    if (err) {
        return err;
    }

    // any data on disk?
    if (pos_ < bid-(weight-1) + lfsr_data_size(bptr.data)) {
        // note one important side-effect here is a strict
        // data hint
        lfs_ssize_t d = lfs_min32(
                size,
                lfsr_data_size(bptr.data)
                    - (pos_ - (bid-(weight-1))));
        lfsr_data_t slice = lfsr_data_slice(bptr.data,
                pos_ - (bid-(weight-1)),
                d);
        d = lfsr_data_read(lfs, &slice,
                buffer, d);
        if (d < 0) {
            return d;
        }

        pos_ += d;
        buffer += d;
        size -= d;
    }

    // found a hole? fill with zeros
    lfs_ssize_t d = lfs_min32(size, bid+1 - pos_);
    lfs_memset(buffer, 0, d);

    pos_ += d;
    buffer += d;
    size -= d;

    return pos_ - pos;
}

static lfs_ssize_t lfsr_bshrub_read(lfs_t *lfs, const lfsr_file_t *file,
        lfs_off_t pos, uint8_t *buffer, lfs_size_t size) {
    lfs_off_t pos_ = pos;
    while (size > 0 && pos_ < lfsr_bshrub_size(&file->bshrub)) {
        lfs_ssize_t d = lfsr_bshrub_readnext(lfs, file,
                pos_, buffer, size);
        if (d < 0) {
            LFS_ASSERT(d != LFS_ERR_NOENT);
            return d;
        }

        pos_ += d;
        buffer += d;
        size -= d;
    }

    return pos_ - pos;
}

// this is atomic
static int lfsr_bshrub_commit(lfs_t *lfs, lfsr_file_t *file,
        lfsr_bid_t bid, const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // file must be a bshrub/btree here
    LFS_ASSERT(lfsr_bshrub_isbshruborbtree(&file->bshrub));

    // before we touch anything, we need to mark all other btree references
    // as unerased
    if (lfsr_bshrub_isbtree(&file->o.mdir, &file->bshrub)) {
        for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
            lfsr_file_t *file_ = (lfsr_file_t*)o;
            if (file_->o.type == LFS_TYPE_REG
                    && file_ != file
                    && lfsr_bshrub_isbshruborbtree(&file_->bshrub)
                    && lfsr_btree_cmp(
                        &file_->bshrub.u.btree,
                        &file->bshrub.u.btree) == 0) {
                // mark as unerased
                file_->bshrub.u.btree.eoff = -1;
            }
        }
    }

    // try to commit to the btree
    lfsr_btree_scratch_t scratch;
    int err = lfsr_btree_commit_(lfs, &file->bshrub.u.btree, &scratch,
            &bid, &attrs, &attr_count);
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }
    LFS_ASSERT(!err || attr_count > 0);
    bool alloc = (err == LFS_ERR_RANGE);

    // when btree is shrubbed, lfsr_btree_commit_ stops at the root
    // and returns with pending attrs
    if (attr_count > 0) {
        // we need to prevent our shrub from overflowing our mdir somehow
        //
        // maintaining an accurate estimate is tricky and error-prone,
        // but recalculating an estimate every commit is expensive
        //
        // Instead, we keep track of an estimate of how many bytes have
        // been progged to the shrub since the last estimate, and recalculate
        // the estimate when this overflows our shrub_size. This mirrors how
        // block_size and rbyds interact, and amortizes the estimate cost.

        // figure out how much data this commit progs
        lfs_size_t commit_estimate = 0;
        for (lfs_size_t i = 0; i < attr_count; i++) {
            // only include tag overhead if tag is not a grow/rm tag
            if (!lfsr_tag_isgrow(attrs[i].tag)
                    && !lfsr_tag_isrm(attrs[i].tag)) {
                commit_estimate += lfs->attr_estimate;
            }
            commit_estimate += lfsr_attr_size(attrs[i]);
        }

        // does our estimate exceed our shrub_size? need to recalculate an
        // accurate estimate
        lfs_ssize_t estimate = (alloc)
                ? (lfs_size_t)-1
                : file->bshrub.u.bshrub.estimate;
        // this double condition avoids overflow issues
        if ((lfs_size_t)estimate > lfs->cfg->shrub_size
                || estimate + commit_estimate > lfs->cfg->shrub_size) {
            estimate = lfsr_bshrub_estimate(lfs, file);
            if (estimate < 0) {
                return estimate;
            }

            // two cases where we evict:
            // - overlow shrub_size/2 - don't penalize for commits here
            // - overlow shrub_size - must include commits or we risk overflow
            //
            // the 1/2 here prevents runaway performance with the shrub is
            // near full, but it's a heuristic, so including the commit would
            // just be mean
            //
            if ((lfs_size_t)estimate > lfs->cfg->shrub_size/2
                    || estimate + commit_estimate > lfs->cfg->shrub_size) {
                goto relocate;
            }
        }

        // include our pending commit in the new estimate
        estimate += commit_estimate;

        // commit to shrub
        int err = lfsr_mdir_commit(lfs, &file->o.mdir, LFSR_ATTRS(
                LFSR_ATTR_SHRUBCOMMIT(
                    (alloc)
                        ? LFSR_TAG_SHRUBALLOC
                        : LFSR_TAG_SHRUBCOMMIT, 0,
                    &file->bshrub_.u.bshrub, bid,
                    attrs, attr_count)));
        if (err) {
            return err;
        }
        LFS_ASSERT(file->bshrub.u.bshrub.blocks[0]
                == file->o.mdir.rbyd.blocks[0]);

        // update _all_ shrubs with the new estimate
        for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
            lfsr_file_t *file_ = (lfsr_file_t*)o;
            if (file_->o.type == LFS_TYPE_REG
                    && file_->o.mdir.mid == file->o.mdir.mid
                    && lfsr_bshrub_isbshrub(&file_->o.mdir, &file_->bshrub)) {
                file_->bshrub.u.bshrub.estimate = estimate;
            }
        }
        LFS_ASSERT(file->bshrub.u.bshrub.estimate == (lfs_size_t)estimate);

        return 0;
    }

    LFS_ASSERT(lfsr_shrub_trunk(&file->bshrub.u.bshrub));
    return 0;

relocate:;
    // convert to btree
    lfsr_rbyd_t rbyd;
    err = lfsr_rbyd_alloc(lfs, &rbyd);
    if (err) {
        return err;
    }

    // note this may be a new root
    if (!alloc) {
        err = lfsr_rbyd_compact(lfs, &rbyd,
                &file->bshrub.u.btree, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }
    }

    err = lfsr_rbyd_commit(lfs, &rbyd, bid,
            attrs, attr_count);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        // bad prog? try another block
        if (err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    file->bshrub.u.btree = rbyd;
    return 0;
}

static int lfsr_file_carve(lfs_t *lfs, lfsr_file_t *file,
        lfs_off_t pos, lfs_off_t weight, lfsr_attr_t attr) {
    // Note! This function has some rather special constraints:
    //
    // 1. We must never allow our btree size to overflow, even temporarily.
    //
    // 2. We must not lose track of bptrs until we no longer need them, to
    //    prevent incorrect allocation from the block allocator.
    //
    // 3. We should avoid copying data fragments as much as possible.
    //
    // These requirements end up conflicting a bit...
    //
    // The second requirement isn't strictly necessary if we track temporary
    // copies during file writes, but it is nice to prove this constraint is
    // possible in case we ever don't track temporary copies.

    // try to merge commits where possible
    lfsr_bid_t bid = lfsr_bshrub_size(&file->bshrub);
    lfsr_attr_t attrs[5];
    lfs_size_t attr_count = 0;
    union {
        lfsr_data_t data;
        uint8_t buf[LFSR_BPTR_DSIZE];
    } left;
    union {
        lfsr_data_t data;
        uint8_t buf[LFSR_BPTR_DSIZE];
    } right;

    // always convert to bshrub/btree when this function is called
    if (!lfsr_bshrub_isbshruborbtree(&file->bshrub)) {
        // this does risk losing our sprout/leaf if there is an error,
        // but note that's already a risk with how file carve deletes
        // data before insertion
        if (lfsr_bshrub_isbsprout(&file->o.mdir, &file->bshrub)) {
            attrs[attr_count++] = LFSR_ATTR_CAT_(
                    LFSR_TAG_DATA, +lfsr_bshrub_size(&file->bshrub),
                    &file->bshrub.u.bsprout, 1);
        } else if (lfsr_bshrub_isbptr(&file->o.mdir, &file->bshrub)) {
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_BLOCK, +lfsr_bshrub_size(&file->bshrub),
                    LFSR_DATA_BPTR_(&file->bshrub.u.bptr, left.buf));
        }

        file->bshrub.u.bshrub = LFSR_SHRUB_NULL(file->o.mdir.rbyd.blocks[0]);

        if (attr_count > 0) {
            LFS_ASSERT(attr_count <= sizeof(attrs)/sizeof(lfsr_attr_t));

            int err = lfsr_bshrub_commit(lfs, file, 0, attrs, attr_count);
            if (err) {
                return err;
            }
        }

        attr_count = 0;
    }

    // need a hole?
    if (pos > lfsr_bshrub_size(&file->bshrub)) {
        // can we coalesce?
        if (lfsr_bshrub_size(&file->bshrub) > 0) {
            bid = lfs_min32(bid, lfsr_bshrub_size(&file->bshrub)-1);
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_GROW, +(pos - lfsr_bshrub_size(&file->bshrub)),
                    LFSR_DATA_NULL());

        // new hole
        } else {
            bid = lfs_min32(bid, lfsr_bshrub_size(&file->bshrub));
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_DATA, +(pos - lfsr_bshrub_size(&file->bshrub)),
                    LFSR_DATA_NULL());
        }
    }

    // try to carve any existing data
    lfsr_attr_t right_attr_ = {.tag=0};
    while (pos < lfsr_bshrub_size(&file->bshrub)) {
        lfsr_tag_t tag_;
        lfsr_bid_t weight_;
        lfsr_bptr_t bptr_;
        int err = lfsr_bshrub_lookupnext(lfs, file, pos,
                &bid, &tag_, &weight_, &bptr_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // note, an entry can be both a left and right sibling
        lfsr_data_t left_slice_ = lfsr_data_slice(bptr_.data,
                -1,
                pos - (bid-(weight_-1)));
        lfsr_data_t right_slice_ = lfsr_data_slice(bptr_.data,
                pos+weight - (bid-(weight_-1)),
                -1);

        // left sibling needs carving but falls underneath our
        // crystallization threshold? break into fragments
        while (tag_ == LFSR_TAG_BLOCK
                && lfsr_data_size(left_slice_) > lfs->cfg->fragment_size
                && lfsr_data_size(left_slice_) < lfs->cfg->crystal_thresh) {
            bptr_.data = lfsr_data_slice(bptr_.data,
                    lfs->cfg->fragment_size,
                    -1);
            
            err = lfsr_bshrub_commit(lfs, file, bid, LFSR_ATTRS(
                    LFSR_ATTR_CAT(
                        LFSR_TAG_GROW | LFSR_TAG_SUB | LFSR_TAG_DATA,
                            -(weight_ - lfs->cfg->fragment_size),
                        lfsr_data_truncate(left_slice_,
                                lfs->cfg->fragment_size)),
                    LFSR_ATTR(
                        LFSR_TAG_BLOCK, +(weight_ - lfs->cfg->fragment_size),
                        LFSR_DATA_BPTR_(&bptr_, left.buf))));
            if (err) {
                return err;
            }

            weight_ -= lfs->cfg->fragment_size;
            left_slice_ = lfsr_data_slice(bptr_.data,
                    -1,
                    pos - (bid-(weight_-1)));
        }

        // right sibling needs carving but falls underneath our
        // crystallization threshold? break into fragments
        while (tag_ == LFSR_TAG_BLOCK
                && lfsr_data_size(right_slice_) > lfs->cfg->fragment_size
                && lfsr_data_size(right_slice_) < lfs->cfg->crystal_thresh) {
            bptr_.data = lfsr_data_slice(bptr_.data,
                    -1,
                    lfsr_data_size(bptr_.data) - lfs->cfg->fragment_size);

            err = lfsr_bshrub_commit(lfs, file, bid, LFSR_ATTRS(
                    LFSR_ATTR(
                        LFSR_TAG_GROW | LFSR_TAG_SUB | LFSR_TAG_BLOCK,
                            -(weight_ - lfsr_data_size(bptr_.data)),
                        LFSR_DATA_BPTR_(&bptr_, right.buf)),
                    LFSR_ATTR_CAT(
                        LFSR_TAG_DATA,
                            +(weight_ - lfsr_data_size(bptr_.data)),
                        lfsr_data_fruncate(right_slice_,
                            lfs->cfg->fragment_size))));
            if (err) {
                return err;
            }

            bid -= (weight_-lfsr_data_size(bptr_.data));
            weight_ -= (weight_-lfsr_data_size(bptr_.data));
            right_slice_ = lfsr_data_slice(bptr_.data,
                    pos+weight - (bid-(weight_-1)),
                    -1);
        }

        // found left sibling?
        if (bid-(weight_-1) < pos) {
            // can we get away with a grow attribute?
            if (lfsr_data_size(bptr_.data) == lfsr_data_size(left_slice_)) {
                attrs[attr_count++] = LFSR_ATTR(
                        LFSR_TAG_GROW, -(bid+1 - pos), LFSR_DATA_NULL());

            // carve fragment?
            } else if (tag_ == LFSR_TAG_DATA) {
                left.data = left_slice_;
                attrs[attr_count++] = LFSR_ATTR_CAT_(
                        LFSR_TAG_GROW | LFSR_TAG_SUB | LFSR_TAG_DATA,
                            -(bid+1 - pos),
                        &left.data, 1);

            // carve bptr?
            } else if (tag_ == LFSR_TAG_BLOCK) {
                attrs[attr_count++] = LFSR_ATTR(
                        LFSR_TAG_GROW | LFSR_TAG_SUB | LFSR_TAG_BLOCK,
                            -(bid+1 - pos),
                        LFSR_DATA_BPTR_(
                            (&(lfsr_bptr_t){
                                .data = left_slice_,
                                .cksize = bptr_.cksize,
                                .cksum = bptr_.cksum}),
                            left.buf));

            } else {
                LFS_UNREACHABLE();
            }

        // completely overwriting this entry?
        } else {
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_RM, -weight_, LFSR_DATA_NULL());
        }

        // spans more than one entry? we can't do everything in one commit,
        // so commit what we have and move on to next entry
        if (pos+weight > bid+1) {
            LFS_ASSERT(lfsr_data_size(right_slice_) == 0);
            LFS_ASSERT(attr_count <= sizeof(attrs)/sizeof(lfsr_attr_t));

            err = lfsr_bshrub_commit(lfs, file, bid,
                    attrs, attr_count);
            if (err) {
                return err;
            }

            attr.weight += lfs_min32(weight, bid+1 - pos);
            weight -= lfs_min32(weight, bid+1 - pos);
            attr_count = 0;
            continue;
        }

        // found right sibling?
        if (pos+weight < bid+1) {
            // can we coalesce a hole?
            if (lfsr_data_size(right_slice_) == 0) {
                attr.weight += bid+1 - (pos+weight);

            // carve fragment?
            } else if (tag_ == LFSR_TAG_DATA) {
                right.data = right_slice_;
                right_attr_ = LFSR_ATTR_CAT_(
                        tag_,
                        bid+1 - (pos+weight),
                        &right.data, 1);

            // carve bptr?
            } else if (tag_ == LFSR_TAG_BLOCK) {
                right_attr_ = LFSR_ATTR(
                        tag_,
                        bid+1 - (pos+weight),
                        LFSR_DATA_BPTR_(
                            (&(lfsr_bptr_t){
                                .data = right_slice_,
                                .cksize = bptr_.cksize,
                                .cksum = bptr_.cksum}),
                            right.buf));

            } else {
                LFS_UNREACHABLE();
            }
        }

        attr.weight += lfs_min32(weight, bid+1 - pos);
        weight -= lfs_min32(weight, bid+1 - pos);
        break;
    }

    // append our data
    if (weight + attr.weight > 0) {
        // can we coalesce a hole?
        if (lfsr_attr_size(attr) == 0 && pos > 0) {
            bid = lfs_min32(bid, lfsr_bshrub_size(&file->bshrub)-1);
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_GROW, +(weight + attr.weight),
                    LFSR_DATA_NULL());

        // need a new hole?
        } else if (lfsr_attr_size(attr) == 0) {
            bid = lfs_min32(bid, lfsr_bshrub_size(&file->bshrub));
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_DATA, +(weight + attr.weight),
                    LFSR_DATA_NULL());

        // append new fragment/bptr?
        } else {
            bid = lfs_min32(bid, lfsr_bshrub_size(&file->bshrub));
            attrs[attr_count++] = LFSR_ATTR_(
                    attr.tag, +(weight + attr.weight),
                    attr.cat, attr.count);
        }
    }

    // and don't forget the right sibling
    if (right_attr_.tag) {
        attrs[attr_count++] = right_attr_;
    }

    // commit pending attrs
    if (attr_count > 0) {
        LFS_ASSERT(attr_count <= sizeof(attrs)/sizeof(lfsr_attr_t));

        int err = lfsr_bshrub_commit(lfs, file, bid,
                attrs, attr_count);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_file_flush_(lfs_t *lfs, lfsr_file_t *file,
        lfs_off_t pos, const uint8_t *buffer, lfs_size_t size) {
    // we can skip some btree lookups if we know we are aligned from a
    // previous iteration, we already do way too many btree lookups
    bool aligned = false;

    // iteratively write blocks
    while (size > 0) {
        // first we need to figure out our current crystal, we do this
        // heuristically.
        //
        // note that we may end up including holes in our crystal, but this
        // is fine. we don't want small holes breaking up blocks anyways

        // default to arbitrary alignment
        lfs_off_t crystal_start = pos;
        lfs_off_t crystal_end = pos + size;
        lfs_off_t block_start;
        lfsr_bptr_t bptr;

        // within our tree? find left crystal neighbor
        if (pos > 0
                && lfs->cfg->crystal_thresh > 0
                && (lfs_soff_t)(pos - (lfs->cfg->crystal_thresh-1))
                    < (lfs_soff_t)lfsr_bshrub_size(&file->bshrub)
                && lfsr_bshrub_size(&file->bshrub) > 0
                // don't bother to lookup left after the first block
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            int err = lfsr_bshrub_lookupnext(lfs, file,
                    lfs_smax32(pos - (lfs->cfg->crystal_thresh-1), 0),
                    &bid, &tag, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // if left crystal neighbor is a fragment and there is no hole
            // between our own crystal and our neighbor, include as a part
            // of our crystal
            if (tag == LFSR_TAG_DATA
                    && bid-(weight-1)+lfsr_data_size(bptr.data)
                        >= pos - (lfs->cfg->crystal_thresh-1)) {
                crystal_start = bid-(weight-1);

            // otherwise our neighbor determines our crystal boundary
            } else {
                crystal_start = lfs_min32(bid+1, pos);

                // wait, found erased-state?
                if (tag == LFSR_TAG_BLOCK
                        && bptr.data.u.disk.block == file->eblock
                        && bptr.data.u.disk.off + lfsr_data_size(bptr.data)
                            == file->eoff
                        // not clobbering data?
                        && crystal_start - (bid-(weight-1))
                            >= lfsr_data_size(bptr.data)
                        // enough for prog alignment?
                        && crystal_end - crystal_start
                            >= lfs->cfg->prog_size) {
                    // mark as unerased in case of failure
                    file->eblock = 0;
                    file->eoff = -1;

                    // try to use erased-state
                    block_start = bid-(weight-1);
                    goto compact;
                }
            }
        }

        // if we haven't already exceeded our crystallization threshold,
        // find right crystal neighbor
        if (crystal_end - crystal_start < lfs->cfg->crystal_thresh
                && lfsr_bshrub_size(&file->bshrub) > 0) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            int err = lfsr_bshrub_lookupnext(lfs, file,
                    lfs_min32(
                        crystal_start + (lfs->cfg->crystal_thresh-1),
                        lfsr_bshrub_size(&file->bshrub)-1),
                    &bid, &tag, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // if right crystal neighbor is a fragment, include as a part
            // of our crystal
            if (tag == LFSR_TAG_DATA) {
                crystal_end = lfs_max32(
                        bid-(weight-1)+lfsr_data_size(bptr.data),
                        pos + size);

            // otherwise treat as crystal boundary
            } else {
                crystal_end = lfs_max32(
                        bid-(weight-1),
                        pos + size);
            }
        }

        // below our crystallization threshold? fallback to writing fragments
        if (crystal_end - crystal_start < lfs->cfg->crystal_thresh
                // enough for prog alignment?
                || crystal_end - crystal_start < lfs->cfg->prog_size) {
            goto fragment;
        }

        // exceeded our crystallization threshold? compact into a new block

        // before we can compact we need to figure out the best block
        // alignment, we use the entry immediately to the left of our
        // crystal for this
        block_start = crystal_start;
        if (crystal_start > 0
                && lfsr_bshrub_size(&file->bshrub) > 0
                // don't bother to lookup left after the first block
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            int err = lfsr_bshrub_lookupnext(lfs, file,
                    lfs_min32(
                        crystal_start-1,
                        lfsr_bshrub_size(&file->bshrub)-1),
                    &bid, &tag, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // is our left neighbor in the same block?
            if (crystal_start - (bid-(weight-1))
                        < lfs->cfg->block_size
                    && lfsr_data_size(bptr.data) > 0) {
                block_start = bid-(weight-1);

                // wait, found erased-state?
                if (tag == LFSR_TAG_BLOCK
                        && bptr.data.u.disk.block == file->eblock
                        && bptr.data.u.disk.off + lfsr_data_size(bptr.data)
                            == file->eoff
                        // not clobbering data?
                        && crystal_start - (bid-(weight-1))
                            >= lfsr_data_size(bptr.data)) {
                    // mark as unerased in case of failure
                    file->eblock = 0;
                    file->eoff = -1;

                    // try to use erased-state
                    goto compact;
                }

            // no? is our left neighbor at least our left block neighbor?
            // align to block alignment
            } else if (crystal_start - (bid-(weight-1))
                        < 2*lfs->cfg->block_size
                    && lfsr_data_size(bptr.data) > 0) {
                block_start = bid-(weight-1) + lfs->cfg->block_size;
            }
        }

    relocate:;
        // allocate a new block
        //
        // note if we relocate, we rewrite the entire block from block_start
        // using what we can find in our tree
        int err = lfs_alloc(lfs, &bptr.data.u.disk.block, true);
        if (err) {
            return err;
        }

        bptr.data = LFSR_DATA_DISK(bptr.data.u.disk.block, 0, 0);
        bptr.cksize = 0;
        bptr.cksum = 0;

    compact:;
        // compact data into our block
        //
        // eagerly merge any right neighbors we see unless that would
        // put us over our block size
        lfs_off_t pos_ = block_start + lfsr_data_size(bptr.data);
        while (pos_ < lfs_min32(
                block_start
                    + (lfs->cfg->block_size - bptr.data.u.disk.off),
                lfs_max32(
                    pos + size,
                    lfsr_bshrub_size(&file->bshrub)))) {
            // keep track of the next highest priority data offset
            lfs_ssize_t d = lfs_min32(
                    block_start
                        + (lfs->cfg->block_size - bptr.data.u.disk.off),
                    lfs_max32(
                        pos + size,
                        lfsr_bshrub_size(&file->bshrub))) - pos_;

            // any data in our buffer?
            if (pos_ < pos + size && size > 0) {
                if (pos_ >= pos) {
                    lfs_ssize_t d_ = lfs_min32(
                            d,
                            size - (pos_ - pos));
                    err = lfsr_bd_prog(lfs, bptr.data.u.disk.block,
                            bptr.cksize,
                            &buffer[pos_ - pos], d_,
                            &bptr.cksum);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_RANGE);
                        // bad prog? try another block
                        if (err == LFS_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }

                    pos_ += d_;
                    bptr.cksize += d_;
                    d -= d_;
                }

                // buffered data takes priority
                d = lfs_min32(d, pos - pos_);
            }

            // any data on disk?
            if (pos_ < lfsr_bshrub_size(&file->bshrub)) {
                lfsr_bid_t bid_;
                lfsr_tag_t tag_;
                lfsr_bid_t weight_;
                lfsr_bptr_t bptr_;
                err = lfsr_bshrub_lookupnext(lfs, file, pos_,
                        &bid_, &tag_, &weight_, &bptr_);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                // make sure to include all of our crystal, or else this
                // loop may never terminate
                if (bid_-(weight_-1) >= crystal_end
                        // is this data a pure hole? stop early to better
                        // leverage erased-state in sparse files
                        && (pos_ >= bid_-(weight_-1)
                                + lfsr_data_size(bptr_.data)
                            // does this data exceed our block_size?
                            // stop early to try to avoid messing up
                            // block alignment
                            || bid_-(weight_-1) + lfsr_data_size(bptr_.data)
                                    - block_start
                                > lfs->cfg->block_size)) {
                    break;
                }

                if (pos_ < bid_-(weight_-1) + lfsr_data_size(bptr_.data)) {
                    // note one important side-effect here is a strict
                    // data hint
                    lfs_ssize_t d_ = lfs_min32(
                            d,
                            lfsr_data_size(bptr_.data)
                                - (pos_ - (bid_-(weight_-1))));
                    err = lfsr_bd_progdata(lfs, bptr.data.u.disk.block,
                            bptr.cksize,
                            lfsr_data_slice(bptr_.data,
                                pos_ - (bid_-(weight_-1)),
                                d_),
                            &bptr.cksum);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_RANGE);
                        // bad prog? try another block
                        if (err == LFS_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }

                    pos_ += d_;
                    bptr.cksize += d_;
                    d -= d_;
                }

                // found a hole? just make sure next leaf takes priority
                d = lfs_min32(d, bid_+1 - pos_);
            }

            // found a hole? fill with zeros
            err = lfsr_bd_set(lfs, bptr.data.u.disk.block, bptr.cksize,
                    0, d,
                    &bptr.cksum);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            pos_ += d;
            bptr.cksize += d;
        }

        // A bit of a hack here, we need to truncate our block to prog_size
        // alignment to avoid padding issues. Doing this retroactively to
        // the pcache greatly simplifies the above loop, though we may end
        // up reading more than is strictly necessary.
        lfs_ssize_t d = bptr.cksize % lfs->cfg->prog_size;
        if (d != 0) {
            err = lfsr_bd_unprog(lfs, bptr.data.u.disk.block, bptr.cksize,
                    d,
                    &bptr.cksum);
            if (err) {
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            bptr.cksize -= d;
        }

        // TODO validate?
        // finalize our write
        err = lfsr_bd_flush(lfs, &bptr.cksum);
        if (err) {
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // prepare our block pointer
        LFS_ASSERT(bptr.cksize > 0);
        LFS_ASSERT(bptr.cksize <= lfs->cfg->block_size);
        bptr.data = LFSR_DATA_DISK(
                bptr.data.u.disk.block,
                bptr.data.u.disk.off,
                bptr.cksize - bptr.data.u.disk.off);
        lfs_off_t block_end = block_start + lfsr_data_size(bptr.data);

        // and write it into our tree
        uint8_t bptr_buf[LFSR_BPTR_DSIZE];
        err = lfsr_file_carve(lfs, file,
                block_start, block_end - block_start,
                LFSR_ATTR(
                    LFSR_TAG_BLOCK, 0,
                    LFSR_DATA_BPTR_(&bptr, bptr_buf)));
        if (err) {
            return err;
        }

        // keep track of any remaining erased-state
        if (bptr.cksize < lfs->cfg->block_size) {
            file->eblock = bptr.data.u.disk.block;
            file->eoff = bptr.cksize;
        }

        // note compacting fragments -> blocks may not actually make any
        // progress on flushing the buffer on the first pass
        d = lfs_max32(pos, block_end) - pos;
        pos += d;
        buffer += lfs_min32(d, size);
        size -= lfs_min32(d, size);
        aligned = true;
    }

fragment:;
    // iteratively write fragments (inlined leaves)
    while (size > 0) {
        // truncate to our fragment size
        lfs_off_t fragment_start = pos;
        lfs_off_t fragment_end = fragment_start + lfs_min32(
                size,
                lfs->cfg->fragment_size);

        lfsr_data_t datas[3];
        lfs_size_t data_count = 0;

        // do we have a left sibling?
        if (fragment_start > 0
                && lfsr_bshrub_size(&file->bshrub) >= fragment_start
                // don't bother to lookup left after first fragment
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_bshrub_lookupnext(lfs, file,
                    fragment_start-1,
                    &bid, &tag, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // can we coalesce?
            if (bid-(weight-1) + lfsr_data_size(bptr.data) >= fragment_start
                    && fragment_end - (bid-(weight-1))
                        <= lfs->cfg->fragment_size) {
                datas[data_count++] = lfsr_data_truncate(bptr.data,
                        fragment_start - (bid-(weight-1)));

                fragment_start = bid-(weight-1);
                fragment_end = fragment_start + lfs_min32(
                        fragment_end - (bid-(weight-1)),
                        lfs->cfg->fragment_size);
            }
        }

        // append our new data
        datas[data_count++] = LFSR_DATA_BUF(
                buffer,
                fragment_end - pos);

        // do we have a right sibling?
        //
        // note this may the same as our left sibling 
        if (fragment_end < lfsr_bshrub_size(&file->bshrub)
                // don't bother to lookup right if fragment is already full
                && fragment_end - fragment_start < lfs->cfg->fragment_size) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_bshrub_lookupnext(lfs, file,
                    fragment_end,
                    &bid, &tag, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // can we coalesce?
            if (fragment_end < bid-(weight-1) + lfsr_data_size(bptr.data)
                    && bid-(weight-1) + lfsr_data_size(bptr.data)
                            - fragment_start
                        <= lfs->cfg->fragment_size) {
                datas[data_count++] = lfsr_data_fruncate(bptr.data,
                        bid-(weight-1) + lfsr_data_size(bptr.data)
                            - fragment_end);

                fragment_end = fragment_start + lfs_min32(
                        bid-(weight-1) + lfsr_data_size(bptr.data)
                            - fragment_start,
                        lfs->cfg->fragment_size);
            }
        }

        // make sure we didn't overflow our data buffer
        LFS_ASSERT(data_count <= 3);

        // once we've figured out what fragment to write, carve it into
        // our tree
        int err = lfsr_file_carve(lfs, file,
                fragment_start, fragment_end - fragment_start,
                LFSR_ATTR_CAT_(
                    LFSR_TAG_DATA, 0,
                    datas, data_count));
        if (err && err != LFS_ERR_RANGE) {
            return err;
        }

        // to next fragment
        lfs_ssize_t d = fragment_end - pos;
        pos += d;
        buffer += lfs_min32(d, size);
        size -= lfs_min32(d, size);
        aligned = true;
    }

    return 0;
}

// our high-level file operations

lfs_ssize_t lfsr_file_read(lfs_t *lfs, lfsr_file_t *file,
        void *buffer, lfs_size_t size) {
    // can't read from writeonly files
    LFS_ASSERT(!lfsr_o_iswronly(file->o.flags));
    LFS_ASSERT(file->pos + size <= 0x7fffffff);

    lfs_off_t pos_ = file->pos;
    uint8_t *buffer_ = buffer;
    while (size > 0 && pos_ < lfsr_file_size_(file)) {
        // keep track of the next highest priority data offset
        lfs_ssize_t d = lfs_min32(size, lfsr_file_size_(file) - pos_);

        // any data in our buffer?
        if (pos_ < file->buffer.pos + file->buffer.size
                && file->buffer.size != 0) {
            if (pos_ >= file->buffer.pos) {
                lfs_ssize_t d_ = lfs_min32(
                        d,
                        file->buffer.size - (pos_ - file->buffer.pos));
                lfs_memcpy(buffer_,
                        &file->buffer.buffer[pos_ - file->buffer.pos],
                        d_);

                pos_ += d_;
                buffer_ += d_;
                size -= d_;
                d -= d_;
                continue;
            }

            // buffered data takes priority
            d = lfs_min32(d, file->buffer.pos - pos_);
        }

        // any data in our btree?
        if (pos_ < lfsr_bshrub_size(&file->bshrub)) {
            // bypass buffer?
            if ((lfs_size_t)d >= lfsr_file_buffersize(lfs, file)) {
                lfs_ssize_t d_ = lfsr_bshrub_readnext(lfs, file,
                        pos_, buffer_, d);
                if (d_ < 0) {
                    LFS_ASSERT(d_ != LFS_ERR_NOENT);
                    return d_;
                }

                pos_ += d_;
                buffer_ += d_;
                size -= d_;
                continue;
            }

            // buffer in use? we need to flush it
            //
            // note that flush does not change the actual file data, so if
            // a read fails it's ok to fall back to our flushed state
            //
            if (lfsr_f_isunflush(file->o.flags)) {
                int err = lfsr_file_flush(lfs, file);
                if (err) {
                    return err;
                }
                file->buffer.pos = 0;
                file->buffer.size = 0;
            }

            // try to fill our buffer with some data
            lfs_ssize_t d_ = lfsr_bshrub_readnext(lfs, file,
                    pos_, file->buffer.buffer, d);
            if (d_ < 0) {
                LFS_ASSERT(d != LFS_ERR_NOENT);
                return d_;
            }
            file->buffer.pos = pos_;
            file->buffer.size = d_;
            continue;
        }

        // found a hole? fill with zeros
        lfs_memset(buffer_, 0, d);
        
        pos_ += d;
        buffer_ += d;
        size -= d;
    }

    // update file and return amount read
    lfs_size_t read = pos_ - file->pos;
    file->pos = pos_;
    return read;
}

lfs_ssize_t lfsr_file_write(lfs_t *lfs, lfsr_file_t *file,
        const void *buffer, lfs_size_t size) {
    // can't write to readonly files
    LFS_ASSERT(!lfsr_o_isrdonly(file->o.flags));

    // would this write make our file larger than our file limit?
    if (size > lfs->file_limit - file->pos) {
        return LFS_ERR_FBIG;
    }

    // size=0 is a bit special and is guaranteed to have no effects on the
    // underlying file, this means no updating file pos or file size
    //
    // since we need to test for this, just return early
    if (size == 0) {
        return 0;
    }

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);
    int err;

    // update pos if we are appending
    lfs_off_t pos = file->pos;
    if (lfsr_o_isappend(file->o.flags)) {
        pos = lfsr_file_size_(file);
    }

    // if we're a small file, we may need to append zeros
    if (pos > lfsr_file_size_(file)
            && pos <= lfsr_file_inlinesize(lfs, file)) {
        LFS_ASSERT(lfsr_f_isunflush(file->o.flags));
        LFS_ASSERT(lfsr_file_size_(file) == file->buffer.size);
        lfs_memset(&file->buffer.buffer[file->buffer.size],
                0,
                pos - file->buffer.size);
        file->buffer.size = pos;
    }

    const uint8_t *buffer_ = buffer;
    lfs_size_t written = 0;
    while (size > 0) {
        // bypass buffer?
        //
        // note we flush our buffer before bypassing writes, this isn't
        // strictly necessary, but enforces a more intuitive write order
        // and avoids weird cases with low-level write heuristics
        //
        if (!lfsr_f_isunflush(file->o.flags)
                && size >= lfsr_file_buffersize(lfs, file)) {
            err = lfsr_file_flush_(lfs, file,
                    pos, buffer_, size);
            if (err) {
                goto failed;
            }

            // after success, fill our buffer with the tail of our write
            //
            // note we need to clear the buffer anyways to avoid any
            // out-of-date data
            file->buffer.pos = pos + size - lfsr_file_buffersize(lfs, file);
            lfs_memcpy(file->buffer.buffer,
                    &buffer_[size - lfsr_file_buffersize(lfs, file)],
                    lfsr_file_buffersize(lfs, file));
            file->buffer.size = lfsr_file_buffersize(lfs, file);

            written += size;
            pos += size;
            buffer_ += size;
            size -= size;
            continue;
        }

        // try to fill our buffer
        //
        // This is a bit delicate, since our buffer contains both old and
        // new data, but note:
        //
        // 1. We only write to yet unused buffer memory.
        //
        // 2. Bypassing the buffer above means we only write to the
        //    buffer once, and flush at most twice.
        //
        if (!lfsr_f_isunflush(file->o.flags)
                || (pos >= file->buffer.pos
                    && pos <= file->buffer.pos + file->buffer.size
                    && pos
                        < file->buffer.pos
                            + lfsr_file_buffersize(lfs, file))) {
            // unused buffer? we can move it where we need it
            if (!lfsr_f_isunflush(file->o.flags)) {
                file->buffer.pos = pos;
                file->buffer.size = 0;
            }

            lfs_size_t d = lfs_min32(
                    size,
                    lfsr_file_buffersize(lfs, file)
                        - (pos - file->buffer.pos));
            lfs_memcpy(&file->buffer.buffer[pos - file->buffer.pos],
                    buffer_,
                    d);
            file->buffer.size = lfs_max32(
                    file->buffer.size,
                    pos+d - file->buffer.pos);

            file->o.flags |= LFS_F_UNFLUSH;
            written += d;
            pos += d;
            buffer_ += d;
            size -= d;
            continue;
        }

        // flush our buffer so the above can't fail
        err = lfsr_file_flush_(lfs, file,
                file->buffer.pos, file->buffer.buffer, file->buffer.size);
        if (err) {
            goto failed;
        }
        file->o.flags &= ~LFS_F_UNFLUSH;
    }

    // mark as unsynced
    file->o.flags |= LFS_F_UNSYNC;
    // update our pos
    file->pos = pos;

    // flush if requested
    //
    // this seems unreachable, but it's possible if we transition from
    // a small file to a non-small file
    if (lfsr_o_isflush(file->o.flags)) {
        err = lfsr_file_flush(lfs, file);
        if (err) {
            goto failed;
        }
    }

    // sync if requested
    if (lfsr_o_issync(file->o.flags)) {
        err = lfsr_file_sync(lfs, file);
        if (err) {
            goto failed;
        }
    }

    return written;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_flush(lfs_t *lfs, lfsr_file_t *file) {
    // readonly files should do nothing
    LFS_ASSERT(!lfsr_o_isrdonly(file->o.flags)
            || !lfsr_f_isunflush(file->o.flags)
            || lfsr_file_size_(file) <= lfsr_file_inlinesize(lfs, file));

    // do nothing if our file is already flushed
    if (!lfsr_f_isunflush(file->o.flags)) {
        return 0;
    }

    // do nothing if our file is small
    //
    // note this means small files remain perpetually unflushed
    if (lfsr_file_size_(file) <= lfsr_file_inlinesize(lfs, file)) {
        // our file must reside entirely in our buffer
        LFS_ASSERT(file->buffer.pos == 0);
        return 0;
    }

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);
    int err;

    // flush our buffer if it contains any unwritten data
    if (lfsr_f_isunflush(file->o.flags)
            && file->buffer.size != 0) {
        // flush
        err = lfsr_file_flush_(lfs, file,
                file->buffer.pos, file->buffer.buffer, file->buffer.size);
        if (err) {
            goto failed;
        }
    }

    // mark as flushed
    file->o.flags &= ~LFS_F_UNFLUSH;
    return 0;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_sync(lfs_t *lfs, lfsr_file_t *file) {
    // removed? we can't sync
    if (lfsr_f_iszombie(file->o.flags)) {
        return LFS_ERR_NOENT;
    }

    // first flush any data in our buffer, this is a noop if already
    // flushed
    //
    // note that flush does not change the actual file data, so if
    // flush succeeds but mdir commit fails it's ok to fall back to
    // our flushed state
    //
    int err = lfsr_file_flush(lfs, file);
    if (err) {
        goto failed;
    }

    // note because of small-file caching and our current write
    // strategy, we never actually end up with only a direct data
    // or bptr
    //
    // this is convenient because bptrs are a bit annoying to commit
    LFS_ASSERT(!lfsr_bshrub_isbsprout(&file->o.mdir, &file->bshrub));
    LFS_ASSERT(!lfsr_bshrub_isbptr(&file->o.mdir, &file->bshrub));
    // small files should start as zero, const prop should optimize this out
    LFS_ASSERT(!lfsr_f_isunflush(file->o.flags)
            || file->buffer.pos == 0);
    // small files/btree should be exclusive here
    LFS_ASSERT(!lfsr_f_isunflush(file->o.flags)
            || lfsr_bshrub_size(&file->bshrub) == 0);
    // small files must be inlined entirely in our buffer
    LFS_ASSERT(!lfsr_f_isunflush(file->o.flags)
            || file->buffer.size <= lfsr_file_inlinesize(lfs, file));
    // orphaned files must be unsynced
    LFS_ASSERT(!lfsr_f_isorphan(file->o.flags)
            || lfsr_f_isunsync(file->o.flags));

    // don't write to disk if our disk is already in-sync
    if (lfsr_f_isunsync(file->o.flags)) {
        // readonly files should do nothing
        //
        // but readonly files _can_ end up unsynced, in the roundabout
        // case where:
        //
        // 1. a file is opened rdonly + desync
        // 2. the same file is opened and written to
        // 3. we try to sync our original file handle
        //
        // the best thing we can do in this case is return an error
        if (lfsr_o_isrdonly(file->o.flags)) {
            err = LFS_ERR_INVAL;
            goto failed;
        }

        // checkpoint the allocator again
        lfs_alloc_ckpoint(lfs);

        // commit our file's metadata
        lfsr_attr_t attrs[2];
        lfs_size_t attr_count = 0;
        lfsr_data_t name_data;
        uint8_t buf[LFSR_BTREE_DSIZE];

        // not created yet? need to convert orphan to normal file
        if (lfsr_f_isorphan(file->o.flags)) {
            err = lfsr_mdir_lookup(lfs, &file->o.mdir, LFSR_TAG_ORPHAN,
                    &name_data);
            if (err) {
                // we must have an orphan at this point
                LFS_ASSERT(err != LFS_ERR_NOENT);
                goto failed;
            }

            attrs[attr_count++] = LFSR_ATTR_CAT_(
                    LFSR_TAG_SUB | LFSR_TAG_REG, 0,
                    &name_data, 1);
        }

        // commit the file state

        // null? no attr?
        if (lfsr_f_isunflush(file->o.flags) && file->buffer.size == 0) {
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_RM | LFSR_TAG_SUB | LFSR_TAG_STRUCT, 0,
                    LFSR_DATA_NULL());
        // small file inlined in mdir?
        } else if (lfsr_f_isunflush(file->o.flags)) {
            attrs[attr_count++] = LFSR_ATTR_CAT_(
                    LFSR_TAG_SUB | LFSR_TAG_DATA, 0,
                    (const lfsr_data_t*)&file->buffer, 1);
        // bshrub?
        } else if (lfsr_bshrub_isbshrub(&file->o.mdir, &file->bshrub)) {
            attrs[attr_count++] = LFSR_ATTR_SHRUBTRUNK(
                    LFSR_TAG_SUB | LFSR_TAG_SHRUBTRUNK, 0,
                    &file->bshrub_.u.bshrub);
        // btree?
        } else if (lfsr_bshrub_isbtree(&file->o.mdir, &file->bshrub)) {
            attrs[attr_count++] = LFSR_ATTR(
                    LFSR_TAG_SUB | LFSR_TAG_BTREE, 0,
                    LFSR_DATA_BTREE_(&file->bshrub.u.btree, buf));
        } else {
            LFS_UNREACHABLE();
        }

        LFS_ASSERT(attr_count <= sizeof(attrs)/sizeof(lfsr_attr_t));

        err = lfsr_mdir_commit(lfs, &file->o.mdir,
                attrs, attr_count);
        if (err) {
            goto failed;
        }
    }

    // but do update other file handles
    for (lfsr_opened_t *o = lfs->opened; o; o = o->next) {
        lfsr_file_t *file_ = (lfsr_file_t*)o;
        if (file_->o.type == LFS_TYPE_REG
                && file_->o.mdir.mid == file->o.mdir.mid
                // don't double update
                && file_ != file) {
            // notify all files of creation
            file_->o.flags &= ~LFS_F_ORPHAN;

            // mark desynced files an unsynced
            if (lfsr_o_isdesync(file_->o.flags)) {
                file_->o.flags |= LFS_F_UNSYNC;

            // update synced files
            } else {
                file_->o.flags &= ~LFS_F_UNSYNC;
                if (lfsr_f_isunflush(file->o.flags)) {
                    file_->o.flags |= LFS_F_UNFLUSH;
                } else {
                    file_->o.flags &= ~LFS_F_UNFLUSH;
                }
                file_->bshrub = file->bshrub;
                file_->buffer.pos = file->buffer.pos;
                LFS_ASSERT(file->buffer.size
                        <= lfsr_file_buffersize(lfs, file));
                lfs_memcpy(file_->buffer.buffer,
                        file->buffer.buffer,
                        file->buffer.size);
                file_->buffer.size = file->buffer.size;
            }
        }
    }

    // mark as synced
    file->o.flags &= ~LFS_F_UNSYNC & ~LFS_F_ORPHAN & ~LFS_O_DESYNC;
    return 0;

failed:;
    file->o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_desync(lfs_t *lfs, lfsr_file_t *file) {
    (void)lfs;
    file->o.flags |= LFS_O_DESYNC;
    return 0;
}

lfs_soff_t lfsr_file_seek(lfs_t *lfs, lfsr_file_t *file,
        lfs_soff_t off, uint8_t whence) {
    // TODO check for out-of-range?

    // figure out our new file position
    lfs_off_t pos_;
    if (whence == LFS_SEEK_SET) {
        pos_ = off;
    } else if (whence == LFS_SEEK_CUR) {
        pos_ = file->pos + off;
    } else if (whence == LFS_SEEK_END) {
        pos_ = lfsr_file_size_(file) + off;
    } else {
        LFS_UNREACHABLE();
    }

    // out of range?
    if (pos_ > lfs->file_limit) {
        return LFS_ERR_INVAL;
    }

    // update file position
    file->pos = pos_;
    return pos_;
}

lfs_soff_t lfsr_file_tell(lfs_t *lfs, lfsr_file_t *file) {
    (void)lfs;
    return file->pos;
}

lfs_soff_t lfsr_file_rewind(lfs_t *lfs, lfsr_file_t *file) {
    (void)lfs;
    file->pos = 0;
    return 0;
}

lfs_soff_t lfsr_file_size(lfs_t *lfs, lfsr_file_t *file) {
    (void)lfs;
    return lfsr_file_size_(file);
}

int lfsr_file_truncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size_) {
    // exceeds our file limit?
    if (size_ > lfs->file_limit) {
        return LFS_ERR_FBIG;
    }

    // do nothing if our size does not change
    lfs_off_t size = lfsr_file_size_(file);
    if (lfsr_file_size_(file) == size_) {
        return 0;
    }

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);
    int err;

    // does our file become small?
    if (size_ <= lfsr_file_inlinesize(lfs, file)) {
        // if our data is not already in our buffer we unfortunately
        // need to flush so our buffer is available to hold everything
        if (file->buffer.pos > 0
                || file->buffer.size < lfs_min32(
                    size_,
                    lfsr_bshrub_size(&file->bshrub))) {
            err = lfsr_file_flush(lfs, file);
            if (err) {
                goto failed;
            }
            file->buffer.pos = 0;
            file->buffer.size = 0;

            lfs_ssize_t d = lfsr_bshrub_read(lfs, file,
                    0, file->buffer.buffer, size_);
            if (d < 0) {
                err = d;
                goto failed;
            }
            file->buffer.pos = 0;
            file->buffer.size = size_;
        }

        // we may need to zero some of our buffer
        if (size_ > file->buffer.size) {
            lfs_memset(&file->buffer.buffer[file->buffer.size],
                    0,
                    size_ - file->buffer.size);
        }

        // small files remain perpetually unflushed
        file->o.flags |= LFS_F_UNFLUSH;
        file->buffer.pos = 0;
        file->buffer.size = size_;
        file->bshrub = LFSR_BSHRUB_BNULL();

    // truncate our file normally
    } else {
        // truncate our btree 
        err = lfsr_file_carve(lfs, file,
                lfs_min32(size, size_), size - lfs_min32(size, size_),
                LFSR_ATTR(
                    LFSR_TAG_DATA, +size_ - size,
                    LFSR_DATA_NULL()));
        if (err) {
            goto failed;
        }

        // truncate our buffer
        file->buffer.pos = lfs_min32(file->buffer.pos, size_);
        file->buffer.size = lfs_min32(
                file->buffer.size,
                size_ - lfs_min32(file->buffer.pos, size_));
    }

    // mark as unsynced
    file->o.flags |= LFS_F_UNSYNC;

    // flush if requested
    //
    // this seems unreachable, but it's possible if we transition from
    // a small file to a non-small file
    if (lfsr_o_isflush(file->o.flags)) {
        err = lfsr_file_flush(lfs, file);
        if (err) {
            goto failed;
        }
    }

    // sync if requested
    if (lfsr_o_issync(file->o.flags)) {
        err = lfsr_file_sync(lfs, file);
        if (err) {
            goto failed;
        }
    }

    return 0;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_fruncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size_) {
    // exceeds our file limit?
    if (size_ > lfs->file_limit) {
        return LFS_ERR_FBIG;
    }

    // do nothing if our size does not change
    lfs_off_t size = lfsr_file_size_(file);
    if (size == size_) {
        return 0;
    }

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);
    int err;

    // does our file become small?
    if (size_ <= lfsr_file_inlinesize(lfs, file)) {
        // if our data is not already in our buffer we unfortunately
        // need to flush so our buffer is available to hold everything
        if (file->buffer.pos + file->buffer.size
                    < lfsr_bshrub_size(&file->bshrub)
                || file->buffer.size < lfs_min32(
                    size_,
                    lfsr_bshrub_size(&file->bshrub))) {
            err = lfsr_file_flush(lfs, file);
            if (err) {
                goto failed;
            }
            file->buffer.pos = 0;
            file->buffer.size = 0;

            lfs_ssize_t d = lfsr_bshrub_read(lfs, file,
                    lfsr_bshrub_size(&file->bshrub) - lfs_min32(
                        size_,
                        lfsr_bshrub_size(&file->bshrub)),
                    file->buffer.buffer, size_);
            if (d < 0) {
                err = d;
                goto failed;
            }
            file->buffer.pos = 0;
            file->buffer.size = size_;
        }

        // we may need to move the data in our buffer
        if (file->buffer.size > size_) {
            lfs_memmove(file->buffer.buffer,
                    &file->buffer.buffer[file->buffer.size - size_],
                    file->buffer.size);
        }
        // we may need to zero some of our buffer
        if (size_ > file->buffer.size) {
            lfs_memmove(&file->buffer.buffer[size_ - file->buffer.size],
                    file->buffer.buffer,
                    file->buffer.size);
            lfs_memset(file->buffer.buffer,
                    0,
                    size_ - file->buffer.size);
        }

        // small files remain perpetually unflushed
        file->o.flags |= LFS_F_UNFLUSH;
        file->buffer.pos = 0;
        file->buffer.size = size_;
        file->bshrub = LFSR_BSHRUB_BNULL();

    // fruncate our file normally
    } else {
        // fruncate our btree
        err = lfsr_file_carve(lfs, file,
                0, lfs_smax32(size - size_, 0),
                LFSR_ATTR(
                    LFSR_TAG_DATA, +size_ - size,
                    LFSR_DATA_NULL()));
        if (err) {
            goto failed;
        }

        // fruncate our buffer
        lfs_memmove(file->buffer.buffer,
                &file->buffer.buffer[lfs_min32(
                    lfs_smax32(
                        size - size_ - file->buffer.pos,
                        0),
                    file->buffer.size)],
                file->buffer.size - lfs_min32(
                    lfs_smax32(
                        size - size_ - file->buffer.pos,
                        0),
                    file->buffer.size));
        file->buffer.size -= lfs_min32(
                lfs_smax32(
                    size - size_ - file->buffer.pos,
                    0),
                file->buffer.size);
        file->buffer.pos -= lfs_smin32(
                size - size_,
                file->buffer.pos);
    }

    // mark as unsynced
    file->o.flags |= LFS_F_UNSYNC;

    // flush if requested
    //
    // this seems unreachable, but it's possible if we transition from
    // a small file to a non-small file
    if (lfsr_o_isflush(file->o.flags)) {
        err = lfsr_file_flush(lfs, file);
        if (err) {
            goto failed;
        }
    }

    // sync if requested
    if (lfsr_o_issync(file->o.flags)) {
        err = lfsr_file_sync(lfs, file);
        if (err) {
            goto failed;
        }
    }

    return 0;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->o.flags |= LFS_O_DESYNC;
    return err;
}





///// Metadata pair and directory operations ///
//static lfs_stag_t lfs_dir_getslice(lfs_t *lfs, const lfs_mdir_t *dir,
//        lfs_tag_t gmask, lfs_tag_t gtag,
//        lfs_off_t goff, void *gbuffer, lfs_size_t gsize) {
//    lfs_off_t off = dir->off;
//    lfs_tag_t ntag = dir->etag;
//    lfs_stag_t gdiff = 0;
//
//    if (lfs_gstate_hasmovehere(&lfs->gdisk, dir->pair) &&
//            lfs_tag_id(gmask) != 0 &&
//            lfs_tag_id(lfs->gdisk.tag) <= lfs_tag_id(gtag)) {
//        // synthetic moves
//        gdiff -= LFS_MKTAG(0, 1, 0);
//    }
//
//    // iterate over dir block backwards (for faster lookups)
//    while (off >= sizeof(lfs_tag_t) + lfs_tag_dsize(ntag)) {
//        off -= lfs_tag_dsize(ntag);
//        lfs_tag_t tag = ntag;
//        int err = lfs_bd_read(lfs,
//                NULL, &lfs->rcache, sizeof(ntag),
//                dir->pair[0], off, &ntag, sizeof(ntag));
//        if (err) {
//            return err;
//        }
//
//        ntag = (lfs_frombe32(ntag) ^ tag) & 0x7fffffff;
//
//        if (lfs_tag_id(gmask) != 0 &&
//                lfs_tag_type1(tag) == LFS_TYPE_SPLICE &&
//                lfs_tag_id(tag) <= lfs_tag_id(gtag - gdiff)) {
//            if (tag == (LFS_MKTAG(LFS_TYPE_CREATE, 0, 0) |
//                    (LFS_MKTAG(0, 0x3ff, 0) & (gtag - gdiff)))) {
//                // found where we were created
//                return LFS_ERR_NOENT;
//            }
//
//            // move around splices
//            gdiff += LFS_MKTAG(0, lfs_tag_splice(tag), 0);
//        }
//
//        if ((gmask & tag) == (gmask & (gtag - gdiff))) {
//            if (lfs_tag_isdelete(tag)) {
//                return LFS_ERR_NOENT;
//            }
//
//            lfs_size_t diff = lfs_min(lfs_tag_size(tag), gsize);
//            err = lfs_bd_read(lfs,
//                    NULL, &lfs->rcache, diff,
//                    dir->pair[0], off+sizeof(tag)+goff, gbuffer, diff);
//            if (err) {
//                return err;
//            }
//
//            memset((uint8_t*)gbuffer + diff, 0, gsize - diff);
//
//            return tag + gdiff;
//        }
//    }
//
//    return LFS_ERR_NOENT;
//}
//
//static lfs_stag_t lfs_dir_get(lfs_t *lfs, const lfs_mdir_t *dir,
//        lfs_tag_t gmask, lfs_tag_t gtag, void *buffer) {
//    return lfs_dir_getslice(lfs, dir,
//            gmask, gtag,
//            0, buffer, lfs_tag_size(gtag));
//}
//
//static int lfs_dir_getread(lfs_t *lfs, const lfs_mdir_t *dir,
//        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
//        lfs_tag_t gmask, lfs_tag_t gtag,
//        lfs_off_t off, void *buffer, lfs_size_t size) {
//    uint8_t *data = buffer;
//    if (off+size > lfs->cfg->block_size) {
//        return LFS_ERR_CORRUPT;
//    }
//
//    while (size > 0) {
//        lfs_size_t diff = size;
//
//        if (pcache && pcache->block == LFS_BLOCK_INLINE &&
//                off < pcache->off + pcache->size) {
//            if (off >= pcache->off) {
//                // is already in pcache?
//                diff = lfs_min(diff, pcache->size - (off-pcache->off));
//                memcpy(data, &pcache->buffer[off-pcache->off], diff);
//
//                data += diff;
//                off += diff;
//                size -= diff;
//                continue;
//            }
//
//            // pcache takes priority
//            diff = lfs_min(diff, pcache->off-off);
//        }
//
//        if (rcache->block == LFS_BLOCK_INLINE &&
//                off < rcache->off + rcache->size) {
//            if (off >= rcache->off) {
//                // is already in rcache?
//                diff = lfs_min(diff, rcache->size - (off-rcache->off));
//                memcpy(data, &rcache->buffer[off-rcache->off], diff);
//
//                data += diff;
//                off += diff;
//                size -= diff;
//                continue;
//            }
//
//            // rcache takes priority
//            diff = lfs_min(diff, rcache->off-off);
//        }
//
//        // load to cache, first condition can no longer fail
//        rcache->block = LFS_BLOCK_INLINE;
//        rcache->off = lfs_aligndown(off, lfs->cfg->read_size);
//        rcache->size = lfs_min(lfs_alignup(off+hint, lfs->cfg->read_size),
//                lfs->cfg->cache_size);
//        int err = lfs_dir_getslice(lfs, dir, gmask, gtag,
//                rcache->off, rcache->buffer, rcache->size);
//        if (err < 0) {
//            return err;
//        }
//    }
//
//    return 0;
//}
//
//#ifndef LFS_READONLY
//static int lfs_dir_traverse_filter(void *p,
//        lfs_tag_t tag, const void *buffer) {
//    lfs_tag_t *filtertag = p;
//    (void)buffer;
//
//    // which mask depends on unique bit in tag structure
//    uint32_t mask = (tag & LFS_MKTAG(0x100, 0, 0))
//            ? LFS_MKTAG(0x7ff, 0x3ff, 0)
//            : LFS_MKTAG(0x700, 0x3ff, 0);
//
//    // check for redundancy
//    if ((mask & tag) == (mask & *filtertag) ||
//            lfs_tag_isdelete(*filtertag) ||
//            (LFS_MKTAG(0x7ff, 0x3ff, 0) & tag) == (
//                LFS_MKTAG(LFS_TYPE_DELETE, 0, 0) |
//                    (LFS_MKTAG(0, 0x3ff, 0) & *filtertag))) {
//        *filtertag = LFS_MKTAG(LFS_FROM_NOOP, 0, 0);
//        return true;
//    }
//
//    // check if we need to adjust for created/deleted tags
//    if (lfs_tag_type1(tag) == LFS_TYPE_SPLICE &&
//            lfs_tag_id(tag) <= lfs_tag_id(*filtertag)) {
//        *filtertag += LFS_MKTAG(0, lfs_tag_splice(tag), 0);
//    }
//
//    return false;
//}
//#endif
//
//#ifndef LFS_READONLY
//// maximum recursive depth of lfs_dir_traverse, the deepest call:
////
//// traverse with commit
//// '-> traverse with move
////     '-> traverse with filter
////
//#define LFS_DIR_TRAVERSE_DEPTH 3
//
//struct lfs_dir_traverse {
//    const lfs_mdir_t *dir;
//    lfs_off_t off;
//    lfs_tag_t ptag;
//    const struct lfs_mattr *attrs;
//    int attrcount;
//
//    lfs_tag_t tmask;
//    lfs_tag_t ttag;
//    uint16_t begin;
//    uint16_t end;
//    int16_t diff;
//
//    int (*cb)(void *data, lfs_tag_t tag, const void *buffer);
//    void *data;
//
//    lfs_tag_t tag;
//    const void *buffer;
//    struct lfs_diskoff disk;
//};
//
//static int lfs_dir_traverse(lfs_t *lfs,
//        const lfs_mdir_t *dir, lfs_off_t off, lfs_tag_t ptag,
//        const struct lfs_mattr *attrs, int attrcount,
//        lfs_tag_t tmask, lfs_tag_t ttag,
//        uint16_t begin, uint16_t end, int16_t diff,
//        int (*cb)(void *data, lfs_tag_t tag, const void *buffer), void *data) {
//    // This function in inherently recursive, but bounded. To allow tool-based
//    // analysis without unnecessary code-cost we use an explicit stack
//    struct lfs_dir_traverse stack[LFS_DIR_TRAVERSE_DEPTH-1];
//    unsigned sp = 0;
//    int res;
//
//    // iterate over directory and attrs
//    lfs_tag_t tag;
//    const void *buffer;
//    struct lfs_diskoff disk;
//    while (true) {
//        {
//            if (off+lfs_tag_dsize(ptag) < dir->off) {
//                off += lfs_tag_dsize(ptag);
//                int err = lfs_bd_read(lfs,
//                        NULL, &lfs->rcache, sizeof(tag),
//                        dir->pair[0], off, &tag, sizeof(tag));
//                if (err) {
//                    return err;
//                }
//
//                tag = (lfs_frombe32(tag) ^ ptag) | 0x80000000;
//                disk.block = dir->pair[0];
//                disk.off = off+sizeof(lfs_tag_t);
//                buffer = &disk;
//                ptag = tag;
//            } else if (attrcount > 0) {
//                tag = attrs[0].tag;
//                buffer = attrs[0].buffer;
//                attrs += 1;
//                attrcount -= 1;
//            } else {
//                // finished traversal, pop from stack?
//                res = 0;
//                break;
//            }
//
//            // do we need to filter?
//            lfs_tag_t mask = LFS_MKTAG(0x7ff, 0, 0);
//            if ((mask & tmask & tag) != (mask & tmask & ttag)) {
//                continue;
//            }
//
//            if (lfs_tag_id(tmask) != 0) {
//                LFS_ASSERT(sp < LFS_DIR_TRAVERSE_DEPTH);
//                // recurse, scan for duplicates, and update tag based on
//                // creates/deletes
//                stack[sp] = (struct lfs_dir_traverse){
//                    .dir        = dir,
//                    .off        = off,
//                    .ptag       = ptag,
//                    .attrs      = attrs,
//                    .attrcount  = attrcount,
//                    .tmask      = tmask,
//                    .ttag       = ttag,
//                    .begin      = begin,
//                    .end        = end,
//                    .diff       = diff,
//                    .cb         = cb,
//                    .data       = data,
//                    .tag        = tag,
//                    .buffer     = buffer,
//                    .disk       = disk,
//                };
//                sp += 1;
//
//                tmask = 0;
//                ttag = 0;
//                begin = 0;
//                end = 0;
//                diff = 0;
//                cb = lfs_dir_traverse_filter;
//                data = &stack[sp-1].tag;
//                continue;
//            }
//        }
//
//popped:
//        // in filter range?
//        if (lfs_tag_id(tmask) != 0 &&
//                !(lfs_tag_id(tag) >= begin && lfs_tag_id(tag) < end)) {
//            continue;
//        }
//
//        // handle special cases for mcu-side operations
//        if (lfs_tag_type3(tag) == LFS_FROM_NOOP) {
//            // do nothing
//        } else if (lfs_tag_type3(tag) == LFS_FROM_MOVE) {
//            // Without this condition, lfs_dir_traverse can exhibit an
//            // extremely expensive O(n^3) of nested loops when renaming.
//            // This happens because lfs_dir_traverse tries to filter tags by
//            // the tags in the source directory, triggering a second
//            // lfs_dir_traverse with its own filter operation.
//            //
//            // traverse with commit
//            // '-> traverse with filter
//            //     '-> traverse with move
//            //         '-> traverse with filter
//            //
//            // However we don't actually care about filtering the second set of
//            // tags, since duplicate tags have no effect when filtering.
//            //
//            // This check skips this unnecessary recursive filtering explicitly,
//            // reducing this runtime from O(n^3) to O(n^2).
//            if (cb == lfs_dir_traverse_filter) {
//                continue;
//            }
//
//            // recurse into move
//            stack[sp] = (struct lfs_dir_traverse){
//                .dir        = dir,
//                .off        = off,
//                .ptag       = ptag,
//                .attrs      = attrs,
//                .attrcount  = attrcount,
//                .tmask      = tmask,
//                .ttag       = ttag,
//                .begin      = begin,
//                .end        = end,
//                .diff       = diff,
//                .cb         = cb,
//                .data       = data,
//                .tag        = LFS_MKTAG(LFS_FROM_NOOP, 0, 0),
//            };
//            sp += 1;
//
//            uint16_t fromid = lfs_tag_size(tag);
//            uint16_t toid = lfs_tag_id(tag);
//            dir = buffer;
//            off = 0;
//            ptag = 0xffffffff;
//            attrs = NULL;
//            attrcount = 0;
//            tmask = LFS_MKTAG(0x600, 0x3ff, 0);
//            ttag = LFS_MKTAG(LFS_TYPE_STRUCT, 0, 0);
//            begin = fromid;
//            end = fromid+1;
//            diff = toid-fromid+diff;
//        } else if (lfs_tag_type3(tag) == LFS_FROM_USERATTRS) {
//            for (unsigned i = 0; i < lfs_tag_size(tag); i++) {
//                const struct lfs_attr *a = buffer;
//                res = cb(data, LFS_MKTAG(LFS_TYPE_USERATTR + a[i].type,
//                        lfs_tag_id(tag) + diff, a[i].size), a[i].buffer);
//                if (res < 0) {
//                    return res;
//                }
//
//                if (res) {
//                    break;
//                }
//            }
//        } else {
//            res = cb(data, tag + LFS_MKTAG(0, diff, 0), buffer);
//            if (res < 0) {
//                return res;
//            }
//
//            if (res) {
//                break;
//            }
//        }
//    }
//
//    if (sp > 0) {
//        // pop from the stack and return, fortunately all pops share
//        // a destination
//        dir         = stack[sp-1].dir;
//        off         = stack[sp-1].off;
//        ptag        = stack[sp-1].ptag;
//        attrs       = stack[sp-1].attrs;
//        attrcount   = stack[sp-1].attrcount;
//        tmask       = stack[sp-1].tmask;
//        ttag        = stack[sp-1].ttag;
//        begin       = stack[sp-1].begin;
//        end         = stack[sp-1].end;
//        diff        = stack[sp-1].diff;
//        cb          = stack[sp-1].cb;
//        data        = stack[sp-1].data;
//        tag         = stack[sp-1].tag;
//        buffer      = stack[sp-1].buffer;
//        disk        = stack[sp-1].disk;
//        sp -= 1;
//        goto popped;
//    } else {
//        return res;
//    }
//}
//#endif
//
//static lfs_stag_t lfs_dir_fetchmatch(lfs_t *lfs,
//        lfs_mdir_t *dir, const lfs_block_t pair[2],
//        lfs_tag_t fmask, lfs_tag_t ftag, uint16_t *id,
//        int (*cb)(void *data, lfs_tag_t tag, const void *buffer), void *data) {
//    // we can find tag very efficiently during a fetch, since we're already
//    // scanning the entire directory
//    lfs_stag_t besttag = -1;
//
//    // if either block address is invalid we return LFS_ERR_CORRUPT here,
//    // otherwise later writes to the pair could fail
//    if (pair[0] >= lfs->cfg->block_count || pair[1] >= lfs->cfg->block_count) {
//        return LFS_ERR_CORRUPT;
//    }
//
//    // find the block with the most recent revision
//    uint32_t revs[2] = {0, 0};
//    int r = 0;
//    for (int i = 0; i < 2; i++) {
//        int err = lfs_bd_read(lfs,
//                NULL, &lfs->rcache, sizeof(revs[i]),
//                pair[i], 0, &revs[i], sizeof(revs[i]));
//        revs[i] = lfs_fromle32(revs[i]);
//        if (err && err != LFS_ERR_CORRUPT) {
//            return err;
//        }
//
//        if (err != LFS_ERR_CORRUPT &&
//                lfs_scmp(revs[i], revs[(i+1)%2]) > 0) {
//            r = i;
//        }
//    }
//
//    dir->pair[0] = pair[(r+0)%2];
//    dir->pair[1] = pair[(r+1)%2];
//    dir->rev = revs[(r+0)%2];
//    dir->off = 0; // nonzero = found some commits
//
//    // now scan tags to fetch the actual dir and find possible match
//    for (int i = 0; i < 2; i++) {
//        lfs_off_t off = 0;
//        lfs_tag_t ptag = 0xffffffff;
//
//        uint16_t tempcount = 0;
//        lfs_block_t temptail[2] = {LFS_BLOCK_NULL, LFS_BLOCK_NULL};
//        bool tempsplit = false;
//        lfs_stag_t tempbesttag = besttag;
//
//        // assume not erased until proven otherwise
//        bool maybeerased = false;
//        bool hasfcrc = false;
//        struct lfs_fcrc fcrc;
//
//        dir->rev = lfs_tole32(dir->rev);
//        uint32_t crc = lfs_crc(0xffffffff, &dir->rev, sizeof(dir->rev));
//        dir->rev = lfs_fromle32(dir->rev);
//
//        while (true) {
//            // extract next tag
//            lfs_tag_t tag;
//            off += lfs_tag_dsize(ptag);
//            int err = lfs_bd_read(lfs,
//                    NULL, &lfs->rcache, lfs->cfg->block_size,
//                    dir->pair[0], off, &tag, sizeof(tag));
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    // can't continue?
//                    break;
//                }
//                return err;
//            }
//
//            crc = lfs_crc(crc, &tag, sizeof(tag));
//            tag = lfs_frombe32(tag) ^ ptag;
//
//            // next commit not yet programmed?
//            if (!lfs_tag_isvalid(tag)) {
//                maybeerased = true;
//                break;
//            // out of range?
//            } else if (off + lfs_tag_dsize(tag) > lfs->cfg->block_size) {
//                break;
//            }
//
//            ptag = tag;
//
//            if (lfs_tag_type2(tag) == LFS_TYPE_CCRC) {
//                // check the crc attr
//                uint32_t dcrc;
//                err = lfs_bd_read(lfs,
//                        NULL, &lfs->rcache, lfs->cfg->block_size,
//                        dir->pair[0], off+sizeof(tag), &dcrc, sizeof(dcrc));
//                if (err) {
//                    if (err == LFS_ERR_CORRUPT) {
//                        break;
//                    }
//                    return err;
//                }
//                dcrc = lfs_fromle32(dcrc);
//
//                if (crc != dcrc) {
//                    break;
//                }
//
//                // reset the next bit if we need to
//                ptag ^= (lfs_tag_t)(lfs_tag_chunk(tag) & 1U) << 31;
//
//                // toss our crc into the filesystem seed for
//                // pseudorandom numbers, note we use another crc here
//                // as a collection function because it is sufficiently
//                // random and convenient
//                lfs->seed = lfs_crc(lfs->seed, &crc, sizeof(crc));
//
//                // update with what's found so far
//                besttag = tempbesttag;
//                dir->off = off + lfs_tag_dsize(tag);
//                dir->etag = ptag;
//                dir->count = tempcount;
//                dir->tail[0] = temptail[0];
//                dir->tail[1] = temptail[1];
//                dir->split = tempsplit;
//
//                // reset crc
//                crc = 0xffffffff;
//                continue;
//            }
//
//            // fcrc is only valid when last tag was a crc
//            hasfcrc = false;
//
//            // crc the entry first, hopefully leaving it in the cache
//            err = lfs_bd_crc(lfs,
//                    NULL, &lfs->rcache, lfs->cfg->block_size,
//                    dir->pair[0], off+sizeof(tag),
//                    lfs_tag_dsize(tag)-sizeof(tag), &crc);
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    break;
//                }
//                return err;
//            }
//
//            // directory modification tags?
//            if (lfs_tag_type1(tag) == LFS_TYPE_NAME) {
//                // increase count of files if necessary
//                if (lfs_tag_id(tag) >= tempcount) {
//                    tempcount = lfs_tag_id(tag) + 1;
//                }
//            } else if (lfs_tag_type1(tag) == LFS_TYPE_SPLICE) {
//                tempcount += lfs_tag_splice(tag);
//
//                if (tag == (LFS_MKTAG(LFS_TYPE_DELETE, 0, 0) |
//                        (LFS_MKTAG(0, 0x3ff, 0) & tempbesttag))) {
//                    tempbesttag |= 0x80000000;
//                } else if (tempbesttag != -1 &&
//                        lfs_tag_id(tag) <= lfs_tag_id(tempbesttag)) {
//                    tempbesttag += LFS_MKTAG(0, lfs_tag_splice(tag), 0);
//                }
//            } else if (lfs_tag_type1(tag) == LFS_TYPE_TAIL) {
//                tempsplit = (lfs_tag_chunk(tag) & 1);
//
//                err = lfs_bd_read(lfs,
//                        NULL, &lfs->rcache, lfs->cfg->block_size,
//                        dir->pair[0], off+sizeof(tag), &temptail, 8);
//                if (err) {
//                    if (err == LFS_ERR_CORRUPT) {
//                        break;
//                    }
//                    return err;
//                }
//                lfs_pair_fromle32(temptail);
//            } else if (lfs_tag_type3(tag) == LFS_TYPE_FCRC) {
//                err = lfs_bd_read(lfs,
//                        NULL, &lfs->rcache, lfs->cfg->block_size,
//                        dir->pair[0], off+sizeof(tag),
//                        &fcrc, sizeof(fcrc));
//                if (err) {
//                    if (err == LFS_ERR_CORRUPT) {
//                        break;
//                    }
//                }
//
//                lfs_fcrc_fromle32(&fcrc);
//                hasfcrc = true;
//            }
//
//            // found a match for our fetcher?
//            if ((fmask & tag) == (fmask & ftag)) {
//                int res = cb(data, tag, &(struct lfs_diskoff){
//                        dir->pair[0], off+sizeof(tag)});
//                if (res < 0) {
//                    if (res == LFS_ERR_CORRUPT) {
//                        break;
//                    }
//                    return res;
//                }
//
//                if (res == LFS_CMP_EQ) {
//                    // found a match
//                    tempbesttag = tag;
//                } else if ((LFS_MKTAG(0x7ff, 0x3ff, 0) & tag) ==
//                        (LFS_MKTAG(0x7ff, 0x3ff, 0) & tempbesttag)) {
//                    // found an identical tag, but contents didn't match
//                    // this must mean that our besttag has been overwritten
//                    tempbesttag = -1;
//                } else if (res == LFS_CMP_GT &&
//                        lfs_tag_id(tag) <= lfs_tag_id(tempbesttag)) {
//                    // found a greater match, keep track to keep things sorted
//                    tempbesttag = tag | 0x80000000;
//                }
//            }
//        }
//
//        // found no valid commits?
//        if (dir->off == 0) {
//            // try the other block?
//            lfs_pair_swap(dir->pair);
//            dir->rev = revs[(r+1)%2];
//            continue;
//        }
//
//        // did we end on a valid commit? we may have an erased block
//        dir->erased = false;
//        if (maybeerased && hasfcrc && dir->off % lfs->cfg->prog_size == 0) {
//            // check for an fcrc matching the next prog's erased state, if
//            // this failed most likely a previous prog was interrupted, we
//            // need a new erase
//            uint32_t fcrc_ = 0xffffffff;
//            int err = lfs_bd_crc(lfs,
//                    NULL, &lfs->rcache, lfs->cfg->block_size,
//                    dir->pair[0], dir->off, fcrc.size, &fcrc_);
//            if (err && err != LFS_ERR_CORRUPT) {
//                return err;
//            }
//
//            // found beginning of erased part?
//            dir->erased = (fcrc_ == fcrc.crc);
//        }
//
//        // synthetic move
//        if (lfs_gstate_hasmovehere(&lfs->gdisk, dir->pair)) {
//            if (lfs_tag_id(lfs->gdisk.tag) == lfs_tag_id(besttag)) {
//                besttag |= 0x80000000;
//            } else if (besttag != -1 &&
//                    lfs_tag_id(lfs->gdisk.tag) < lfs_tag_id(besttag)) {
//                besttag -= LFS_MKTAG(0, 1, 0);
//            }
//        }
//
//        // found tag? or found best id?
//        if (id) {
//            *id = lfs_min(lfs_tag_id(besttag), dir->count);
//        }
//
//        if (lfs_tag_isvalid(besttag)) {
//            return besttag;
//        } else if (lfs_tag_id(besttag) < dir->count) {
//            return LFS_ERR_NOENT;
//        } else {
//            return 0;
//        }
//    }
//
//    LFS_ERROR("Corrupted dir pair at {0x%"PRIx32", 0x%"PRIx32"}",
//            dir->pair[0], dir->pair[1]);
//    return LFS_ERR_CORRUPT;
//}
//
//static int lfs_dir_fetch(lfs_t *lfs,
//        lfs_mdir_t *dir, const lfs_block_t pair[2]) {
//    // note, mask=-1, tag=-1 can never match a tag since this
//    // pattern has the invalid bit set
//    return (int)lfs_dir_fetchmatch(lfs, dir, pair,
//            (lfs_tag_t)-1, (lfs_tag_t)-1, NULL, NULL, NULL);
//}
//
//static int lfs_dir_getgstate(lfs_t *lfs, const lfs_mdir_t *dir,
//        lfs_gstate_t *gstate) {
//    lfs_gstate_t temp;
//    lfs_stag_t res = lfs_dir_get(lfs, dir, LFS_MKTAG(0x7ff, 0, 0),
//            LFS_MKTAG(LFS_TYPE_MOVESTATE, 0, sizeof(temp)), &temp);
//    if (res < 0 && res != LFS_ERR_NOENT) {
//        return res;
//    }
//
//    if (res != LFS_ERR_NOENT) {
//        // xor together to find resulting gstate
//        lfs_gstate_fromle32(&temp);
//        lfs_gstate_xor(gstate, &temp);
//    }
//
//    return 0;
//}
//
//static int lfs_dir_getinfo(lfs_t *lfs, lfs_mdir_t *dir,
//        uint16_t id, struct lfs_info *info) {
//    if (id == 0x3ff) {
//        // special case for root
//        strcpy(info->name, "/");
//        info->type = LFS_TYPE_DIR;
//        return 0;
//    }
//
//    lfs_stag_t tag = lfs_dir_get(lfs, dir, LFS_MKTAG(0x780, 0x3ff, 0),
//            LFS_MKTAG(LFS_TYPE_NAME, id, lfs->name_max+1), info->name);
//    if (tag < 0) {
//        return (int)tag;
//    }
//
//    info->type = lfs_tag_type3(tag);
//
//    struct lfs_ctz ctz;
//    tag = lfs_dir_get(lfs, dir, LFS_MKTAG(0x700, 0x3ff, 0),
//            LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
//    if (tag < 0) {
//        return (int)tag;
//    }
//    lfs_ctz_fromle32(&ctz);
//
//    if (lfs_tag_type3(tag) == LFS_TYPE_CTZSTRUCT) {
//        info->size = ctz.size;
//    } else if (lfs_tag_type3(tag) == LFS_TYPE_INLINESTRUCT) {
//        info->size = lfs_tag_size(tag);
//    }
//
//    return 0;
//}
//
//struct lfs_dir_find_match {
//    lfs_t *lfs;
//    const void *name;
//    lfs_size_t size;
//};
//
//static int lfs_dir_find_match(void *data,
//        lfs_tag_t tag, const void *buffer) {
//    struct lfs_dir_find_match *name = data;
//    lfs_t *lfs = name->lfs;
//    const struct lfs_diskoff *disk = buffer;
//
//    // compare with disk
//    lfs_size_t diff = lfs_min(name->size, lfs_tag_size(tag));
//    int res = lfs_bd_cmp(lfs,
//            NULL, &lfs->rcache, diff,
//            disk->block, disk->off, name->name, diff);
//    if (res != LFS_CMP_EQ) {
//        return res;
//    }
//
//    // only equal if our size is still the same
//    if (name->size != lfs_tag_size(tag)) {
//        return (name->size < lfs_tag_size(tag)) ? LFS_CMP_LT : LFS_CMP_GT;
//    }
//
//    // found a match!
//    return LFS_CMP_EQ;
//}
//
//static lfs_stag_t lfs_dir_find(lfs_t *lfs, lfs_mdir_t *dir,
//        const char **path, uint16_t *id) {
//    // we reduce path to a single name if we can find it
//    const char *name = *path;
//    if (id) {
//        *id = 0x3ff;
//    }
//
//    // default to root dir
//    lfs_stag_t tag = LFS_MKTAG(LFS_TYPE_DIR, 0x3ff, 0);
//    dir->tail[0] = lfs->root[0];
//    dir->tail[1] = lfs->root[1];
//
//    while (true) {
//nextname:
//        // skip slashes
//        name += strspn(name, "/");
//        lfs_size_t namelen = strcspn(name, "/");
//
//        // skip '.' and root '..'
//        if ((namelen == 1 && memcmp(name, ".", 1) == 0) ||
//            (namelen == 2 && memcmp(name, "..", 2) == 0)) {
//            name += namelen;
//            goto nextname;
//        }
//
//        // skip if matched by '..' in name
//        const char *suffix = name + namelen;
//        lfs_size_t sufflen;
//        int depth = 1;
//        while (true) {
//            suffix += strspn(suffix, "/");
//            sufflen = strcspn(suffix, "/");
//            if (sufflen == 0) {
//                break;
//            }
//
//            if (sufflen == 2 && memcmp(suffix, "..", 2) == 0) {
//                depth -= 1;
//                if (depth == 0) {
//                    name = suffix + sufflen;
//                    goto nextname;
//                }
//            } else {
//                depth += 1;
//            }
//
//            suffix += sufflen;
//        }
//
//        // found path
//        if (name[0] == '\0') {
//            return tag;
//        }
//
//        // update what we've found so far
//        *path = name;
//
//        // only continue if we hit a directory
//        if (lfs_tag_type3(tag) != LFS_TYPE_DIR) {
//            return LFS_ERR_NOTDIR;
//        }
//
//        // grab the entry data
//        if (lfs_tag_id(tag) != 0x3ff) {
//            lfs_stag_t res = lfs_dir_get(lfs, dir, LFS_MKTAG(0x700, 0x3ff, 0),
//                    LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), dir->tail);
//            if (res < 0) {
//                return res;
//            }
//            lfs_pair_fromle32(dir->tail);
//        }
//
//        // find entry matching name
//        while (true) {
//            tag = lfs_dir_fetchmatch(lfs, dir, dir->tail,
//                    LFS_MKTAG(0x780, 0, 0),
//                    LFS_MKTAG(LFS_TYPE_NAME, 0, namelen),
//                     // are we last name?
//                    (strchr(name, '/') == NULL) ? id : NULL,
//                    lfs_dir_find_match, &(struct lfs_dir_find_match){
//                        lfs, name, namelen});
//            if (tag < 0) {
//                return tag;
//            }
//
//            if (tag) {
//                break;
//            }
//
//            if (!dir->split) {
//                return LFS_ERR_NOENT;
//            }
//        }
//
//        // to next name
//        name += namelen;
//    }
//}
//
//// commit logic
//struct lfs_commit {
//    lfs_block_t block;
//    lfs_off_t off;
//    lfs_tag_t ptag;
//    uint32_t crc;
//
//    lfs_off_t begin;
//    lfs_off_t end;
//};
//
//#ifndef LFS_READONLY
//static int lfs_dir_commitprog(lfs_t *lfs, struct lfs_commit *commit,
//        const void *buffer, lfs_size_t size) {
//    int err = lfs_bd_prog(lfs,
//            &lfs->pcache, &lfs->rcache, false,
//            commit->block, commit->off ,
//            (const uint8_t*)buffer, size);
//    if (err) {
//        return err;
//    }
//
//    commit->crc = lfs_crc(commit->crc, buffer, size);
//    commit->off += size;
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_commitattr(lfs_t *lfs, struct lfs_commit *commit,
//        lfs_tag_t tag, const void *buffer) {
//    // check if we fit
//    lfs_size_t dsize = lfs_tag_dsize(tag);
//    if (commit->off + dsize > commit->end) {
//        return LFS_ERR_NOSPC;
//    }
//
//    // write out tag
//    lfs_tag_t ntag = lfs_tobe32((tag & 0x7fffffff) ^ commit->ptag);
//    int err = lfs_dir_commitprog(lfs, commit, &ntag, sizeof(ntag));
//    if (err) {
//        return err;
//    }
//
//    if (!(tag & 0x80000000)) {
//        // from memory
//        err = lfs_dir_commitprog(lfs, commit, buffer, dsize-sizeof(tag));
//        if (err) {
//            return err;
//        }
//    } else {
//        // from disk
//        const struct lfs_diskoff *disk = buffer;
//        for (lfs_off_t i = 0; i < dsize-sizeof(tag); i++) {
//            // rely on caching to make this efficient
//            uint8_t dat;
//            err = lfs_bd_read(lfs,
//                    NULL, &lfs->rcache, dsize-sizeof(tag)-i,
//                    disk->block, disk->off+i, &dat, 1);
//            if (err) {
//                return err;
//            }
//
//            err = lfs_dir_commitprog(lfs, commit, &dat, 1);
//            if (err) {
//                return err;
//            }
//        }
//    }
//
//    commit->ptag = tag & 0x7fffffff;
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//
//static int lfs_dir_commitcrc(lfs_t *lfs, struct lfs_commit *commit) {
//    // align to program units
//    //
//    // this gets a bit complex as we have two types of crcs:
//    // - 5-word crc with fcrc to check following prog (middle of block)
//    // - 2-word crc with no following prog (end of block)
//    const lfs_off_t end = lfs_alignup(
//            lfs_min(commit->off + 5*sizeof(uint32_t), lfs->cfg->block_size),
//            lfs->cfg->prog_size);
//
//    lfs_off_t off1 = 0;
//    uint32_t crc1 = 0;
//
//    // create crc tags to fill up remainder of commit, note that
//    // padding is not crced, which lets fetches skip padding but
//    // makes committing a bit more complicated
//    while (commit->off < end) {
//        lfs_off_t noff = (
//                lfs_min(end - (commit->off+sizeof(lfs_tag_t)), 0x3fe)
//                + (commit->off+sizeof(lfs_tag_t)));
//        // too large for crc tag? need padding commits
//        if (noff < end) {
//            noff = lfs_min(noff, end - 5*sizeof(uint32_t));
//        }
//
//        // space for fcrc?
//        uint8_t eperturb = -1;
//        if (noff >= end && noff <= lfs->cfg->block_size - lfs->cfg->prog_size) {
//            // first read the leading byte, this always contains a bit
//            // we can perturb to avoid writes that don't change the fcrc
//            int err = lfs_bd_read(lfs,
//                    NULL, &lfs->rcache, lfs->cfg->prog_size,
//                    commit->block, noff, &eperturb, 1);
//            if (err && err != LFS_ERR_CORRUPT) {
//                return err;
//            }
//
//            // find the expected fcrc, don't bother avoiding a reread
//            // of the eperturb, it should still be in our cache
//            struct lfs_fcrc fcrc = {.size=lfs->cfg->prog_size, .crc=0xffffffff};
//            err = lfs_bd_crc(lfs,
//                    NULL, &lfs->rcache, lfs->cfg->prog_size,
//                    commit->block, noff, fcrc.size, &fcrc.crc);
//            if (err && err != LFS_ERR_CORRUPT) {
//                return err;
//            }
//
//            lfs_fcrc_tole32(&fcrc);
//            err = lfs_dir_commitattr(lfs, commit,
//                    LFS_MKTAG(LFS_TYPE_FCRC, 0x3ff, sizeof(struct lfs_fcrc)),
//                    &fcrc);
//            if (err) {
//                return err;
//            }
//        }
//
//        // build commit crc
//        struct {
//            lfs_tag_t tag;
//            uint32_t crc;
//        } ccrc;
//        lfs_tag_t ntag = LFS_MKTAG(
//                LFS_TYPE_CCRC + (((uint8_t)~eperturb) >> 7), 0x3ff,
//                noff - (commit->off+sizeof(lfs_tag_t)));
//        ccrc.tag = lfs_tobe32(ntag ^ commit->ptag);
//        commit->crc = lfs_crc(commit->crc, &ccrc.tag, sizeof(lfs_tag_t));
//        ccrc.crc = lfs_tole32(commit->crc);
//
//        int err = lfs_bd_prog(lfs,
//                &lfs->pcache, &lfs->rcache, false,
//                commit->block, commit->off, &ccrc, sizeof(ccrc));
//        if (err) {
//            return err;
//        }
//
//        // keep track of non-padding checksum to verify
//        if (off1 == 0) {
//            off1 = commit->off + sizeof(lfs_tag_t);
//            crc1 = commit->crc;
//        }
//
//        commit->off = noff;
//        // perturb valid bit?
//        commit->ptag = ntag ^ ((0x80 & ~eperturb) << 24);
//        // reset crc for next commit
//        commit->crc = 0xffffffff;
//
//        // manually flush here since we don't prog the padding, this confuses
//        // the caching layer
//        if (noff >= end || noff >= lfs->pcache.off + lfs->cfg->cache_size) {
//            // flush buffers
//            int err = lfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
//            if (err) {
//                return err;
//            }
//        }
//    }
//
//    // successful commit, check checksums to make sure
//    //
//    // note that we don't need to check padding commits, worst
//    // case if they are corrupted we would have had to compact anyways
//    lfs_off_t off = commit->begin;
//    uint32_t crc = 0xffffffff;
//    int err = lfs_bd_crc(lfs,
//            NULL, &lfs->rcache, off1+sizeof(uint32_t),
//            commit->block, off, off1-off, &crc);
//    if (err) {
//        return err;
//    }
//
//    // check non-padding commits against known crc
//    if (crc != crc1) {
//        return LFS_ERR_CORRUPT;
//    }
//
//    // make sure to check crc in case we happen to pick
//    // up an unrelated crc (frozen block?)
//    err = lfs_bd_crc(lfs,
//            NULL, &lfs->rcache, sizeof(uint32_t),
//            commit->block, off1, sizeof(uint32_t), &crc);
//    if (err) {
//        return err;
//    }
//
//    if (crc != 0) {
//        return LFS_ERR_CORRUPT;
//    }
//
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_alloc(lfs_t *lfs, lfs_mdir_t *dir) {
//    // allocate pair of dir blocks (backwards, so we write block 1 first)
//    for (int i = 0; i < 2; i++) {
//        int err = lfs_alloc(lfs, &dir->pair[(i+1)%2]);
//        if (err) {
//            return err;
//        }
//    }
//
//    // zero for reproducibility in case initial block is unreadable
//    dir->rev = 0;
//
//    // rather than clobbering one of the blocks we just pretend
//    // the revision may be valid
//    int err = lfs_bd_read(lfs,
//            NULL, &lfs->rcache, sizeof(dir->rev),
//            dir->pair[0], 0, &dir->rev, sizeof(dir->rev));
//    dir->rev = lfs_fromle32(dir->rev);
//    if (err && err != LFS_ERR_CORRUPT) {
//        return err;
//    }
//
//    // to make sure we don't immediately evict, align the new revision count
//    // to our block_cycles modulus, see lfs_dir_compact for why our modulus
//    // is tweaked this way
//    if (lfs->cfg->block_cycles > 0) {
//        dir->rev = lfs_alignup(dir->rev, ((lfs->cfg->block_cycles+1)|1));
//    }
//
//    // set defaults
//    dir->off = sizeof(dir->rev);
//    dir->etag = 0xffffffff;
//    dir->count = 0;
//    dir->tail[0] = LFS_BLOCK_NULL;
//    dir->tail[1] = LFS_BLOCK_NULL;
//    dir->erased = false;
//    dir->split = false;
//
//    // don't write out yet, let caller take care of that
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_drop(lfs_t *lfs, lfs_mdir_t *dir, lfs_mdir_t *tail) {
//    // steal state
//    int err = lfs_dir_getgstate(lfs, tail, &lfs->gdelta);
//    if (err) {
//        return err;
//    }
//
//    // steal tail
//    lfs_pair_tole32(tail->tail);
//    err = lfs_dir_commit(lfs, dir, LFS_MKATTRS(
//            {LFS_MKTAG(LFS_TYPE_TAIL + tail->split, 0x3ff, 8), tail->tail}));
//    lfs_pair_fromle32(tail->tail);
//    if (err) {
//        return err;
//    }
//
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_split(lfs_t *lfs,
//        lfs_mdir_t *dir, const struct lfs_mattr *attrs, int attrcount,
//        lfs_mdir_t *source, uint16_t split, uint16_t end) {
//    // create tail metadata pair
//    lfs_mdir_t tail;
//    int err = lfs_dir_alloc(lfs, &tail);
//    if (err) {
//        return err;
//    }
//
//    tail.split = dir->split;
//    tail.tail[0] = dir->tail[0];
//    tail.tail[1] = dir->tail[1];
//
//    // note we don't care about LFS_OK_RELOCATED
//    int res = lfs_dir_compact(lfs, &tail, attrs, attrcount, source, split, end);
//    if (res < 0) {
//        return res;
//    }
//
//    dir->tail[0] = tail.pair[0];
//    dir->tail[1] = tail.pair[1];
//    dir->split = true;
//
//    // update root if needed
//    if (lfs_pair_cmp(dir->pair, lfs->root) == 0 && split == 0) {
//        lfs->root[0] = tail.pair[0];
//        lfs->root[1] = tail.pair[1];
//    }
//
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_commit_size(void *p, lfs_tag_t tag, const void *buffer) {
//    lfs_size_t *size = p;
//    (void)buffer;
//
//    *size += lfs_tag_dsize(tag);
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//struct lfs_dir_commit_commit {
//    lfs_t *lfs;
//    struct lfs_commit *commit;
//};
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_commit_commit(void *p, lfs_tag_t tag, const void *buffer) {
//    struct lfs_dir_commit_commit *commit = p;
//    return lfs_dir_commitattr(commit->lfs, commit->commit, tag, buffer);
//}
//#endif
//
//#ifndef LFS_READONLY
//static bool lfs_dir_needsrelocation(lfs_t *lfs, lfs_mdir_t *dir) {
//    // If our revision count == n * block_cycles, we should force a relocation,
//    // this is how littlefs wear-levels at the metadata-pair level. Note that we
//    // actually use (block_cycles+1)|1, this is to avoid two corner cases:
//    // 1. block_cycles = 1, which would prevent relocations from terminating
//    // 2. block_cycles = 2n, which, due to aliasing, would only ever relocate
//    //    one metadata block in the pair, effectively making this useless
//    return (lfs->cfg->block_cycles > 0
//            && ((dir->rev + 1) % ((lfs->cfg->block_cycles+1)|1) == 0));
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_compact(lfs_t *lfs,
//        lfs_mdir_t *dir, const struct lfs_mattr *attrs, int attrcount,
//        lfs_mdir_t *source, uint16_t begin, uint16_t end) {
//    // save some state in case block is bad
//    bool relocated = false;
//    bool tired = lfs_dir_needsrelocation(lfs, dir);
//
//    // increment revision count
//    dir->rev += 1;
//
//    // do not proactively relocate blocks during migrations, this
//    // can cause a number of failure states such: clobbering the
//    // v1 superblock if we relocate root, and invalidating directory
//    // pointers if we relocate the head of a directory. On top of
//    // this, relocations increase the overall complexity of
//    // lfs_migration, which is already a delicate operation.
//#ifdef LFS_MIGRATE
//    if (lfs->lfs1) {
//        tired = false;
//    }
//#endif
//
//    if (tired && lfs_pair_cmp(dir->pair, (const lfs_block_t[2]){0, 1}) != 0) {
//        // we're writing too much, time to relocate
//        goto relocate;
//    }
//
//    // begin loop to commit compaction to blocks until a compact sticks
//    while (true) {
//        {
//            // setup commit state
//            struct lfs_commit commit = {
//                .block = dir->pair[1],
//                .off = 0,
//                .ptag = 0xffffffff,
//                .crc = 0xffffffff,
//
//                .begin = 0,
//                .end = (lfs->cfg->metadata_max ?
//                    lfs->cfg->metadata_max : lfs->cfg->block_size) - 8,
//            };
//
//            // erase block to write to
//            int err = lfs_bd_erase(lfs, dir->pair[1]);
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    goto relocate;
//                }
//                return err;
//            }
//
//            // write out header
//            dir->rev = lfs_tole32(dir->rev);
//            err = lfs_dir_commitprog(lfs, &commit,
//                    &dir->rev, sizeof(dir->rev));
//            dir->rev = lfs_fromle32(dir->rev);
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    goto relocate;
//                }
//                return err;
//            }
//
//            // traverse the directory, this time writing out all unique tags
//            err = lfs_dir_traverse(lfs,
//                    source, 0, 0xffffffff, attrs, attrcount,
//                    LFS_MKTAG(0x400, 0x3ff, 0),
//                    LFS_MKTAG(LFS_TYPE_NAME, 0, 0),
//                    begin, end, -begin,
//                    lfs_dir_commit_commit, &(struct lfs_dir_commit_commit){
//                        lfs, &commit});
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    goto relocate;
//                }
//                return err;
//            }
//
//            // commit tail, which may be new after last size check
//            if (!lfs_pair_isnull(dir->tail)) {
//                lfs_pair_tole32(dir->tail);
//                err = lfs_dir_commitattr(lfs, &commit,
//                        LFS_MKTAG(LFS_TYPE_TAIL + dir->split, 0x3ff, 8),
//                        dir->tail);
//                lfs_pair_fromle32(dir->tail);
//                if (err) {
//                    if (err == LFS_ERR_CORRUPT) {
//                        goto relocate;
//                    }
//                    return err;
//                }
//            }
//
//            // bring over gstate?
//            lfs_gstate_t delta = {0};
//            if (!relocated) {
//                lfs_gstate_xor(&delta, &lfs->gdisk);
//                lfs_gstate_xor(&delta, &lfs->gstate);
//            }
//            lfs_gstate_xor(&delta, &lfs->gdelta);
//            delta.tag &= ~LFS_MKTAG(0, 0, 0x3ff);
//
//            err = lfs_dir_getgstate(lfs, dir, &delta);
//            if (err) {
//                return err;
//            }
//
//            if (!lfs_gstate_iszero(&delta)) {
//                lfs_gstate_tole32(&delta);
//                err = lfs_dir_commitattr(lfs, &commit,
//                        LFS_MKTAG(LFS_TYPE_MOVESTATE, 0x3ff,
//                            sizeof(delta)), &delta);
//                if (err) {
//                    if (err == LFS_ERR_CORRUPT) {
//                        goto relocate;
//                    }
//                    return err;
//                }
//            }
//
//            // complete commit with crc
//            err = lfs_dir_commitcrc(lfs, &commit);
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    goto relocate;
//                }
//                return err;
//            }
//
//            // successful compaction, swap dir pair to indicate most recent
//            LFS_ASSERT(commit.off % lfs->cfg->prog_size == 0);
//            lfs_pair_swap(dir->pair);
//            dir->count = end - begin;
//            dir->off = commit.off;
//            dir->etag = commit.ptag;
//            // update gstate
//            lfs->gdelta = (lfs_gstate_t){0};
//            if (!relocated) {
//                lfs->gdisk = lfs->gstate;
//            }
//        }
//        break;
//
//relocate:
//        // commit was corrupted, drop caches and prepare to relocate block
//        relocated = true;
//        lfs_cache_drop(lfs, &lfs->pcache);
//        if (!tired) {
//            LFS_DEBUG("Bad block at 0x%"PRIx32, dir->pair[1]);
//        }
//
//        // can't relocate superblock, filesystem is now frozen
//        if (lfs_pair_cmp(dir->pair, (const lfs_block_t[2]){0, 1}) == 0) {
//            LFS_WARN("Superblock 0x%"PRIx32" has become unwritable",
//                    dir->pair[1]);
//            return LFS_ERR_NOSPC;
//        }
//
//        // relocate half of pair
//        int err = lfs_alloc(lfs, &dir->pair[1]);
//        if (err && (err != LFS_ERR_NOSPC || !tired)) {
//            return err;
//        }
//
//        tired = false;
//        continue;
//    }
//
//    return relocated ? LFS_OK_RELOCATED : 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_splittingcompact(lfs_t *lfs, lfs_mdir_t *dir,
//        const struct lfs_mattr *attrs, int attrcount,
//        lfs_mdir_t *source, uint16_t begin, uint16_t end) {
//    while (true) {
//        // find size of first split, we do this by halving the split until
//        // the metadata is guaranteed to fit
//        //
//        // Note that this isn't a true binary search, we never increase the
//        // split size. This may result in poorly distributed metadata but isn't
//        // worth the extra code size or performance hit to fix.
//        lfs_size_t split = begin;
//        while (end - split > 1) {
//            lfs_size_t size = 0;
//            int err = lfs_dir_traverse(lfs,
//                    source, 0, 0xffffffff, attrs, attrcount,
//                    LFS_MKTAG(0x400, 0x3ff, 0),
//                    LFS_MKTAG(LFS_TYPE_NAME, 0, 0),
//                    split, end, -split,
//                    lfs_dir_commit_size, &size);
//            if (err) {
//                return err;
//            }
//
//            // space is complicated, we need room for:
//            //
//            // - tail:         4+2*4 = 12 bytes
//            // - gstate:       4+3*4 = 16 bytes
//            // - move delete:  4     = 4 bytes
//            // - crc:          4+4   = 8 bytes
//            //                 total = 40 bytes
//            //
//            // And we cap at half a block to avoid degenerate cases with
//            // nearly-full metadata blocks.
//            //
//            if (end - split < 0xff
//                    && size <= lfs_min(
//                        lfs->cfg->block_size - 40,
//                        lfs_alignup(
//                            (lfs->cfg->metadata_max
//                                ? lfs->cfg->metadata_max
//                                : lfs->cfg->block_size)/2,
//                            lfs->cfg->prog_size))) {
//                break;
//            }
//
//            split = split + ((end - split) / 2);
//        }
//
//        if (split == begin) {
//            // no split needed
//            break;
//        }
//
//        // split into two metadata pairs and continue
//        int err = lfs_dir_split(lfs, dir, attrs, attrcount,
//                source, split, end);
//        if (err && err != LFS_ERR_NOSPC) {
//            return err;
//        }
//
//        if (err) {
//            // we can't allocate a new block, try to compact with degraded
//            // performance
//            LFS_WARN("Unable to split {0x%"PRIx32", 0x%"PRIx32"}",
//                    dir->pair[0], dir->pair[1]);
//            break;
//        } else {
//            end = split;
//        }
//    }
//
//    if (lfs_dir_needsrelocation(lfs, dir)
//            && lfs_pair_cmp(dir->pair, (const lfs_block_t[2]){0, 1}) == 0) {
//        // oh no! we're writing too much to the superblock,
//        // should we expand?
//        lfs_ssize_t size = lfs_fs_rawsize(lfs);
//        if (size < 0) {
//            return size;
//        }
//
//        // do we have extra space? littlefs can't reclaim this space
//        // by itself, so expand cautiously
//        if ((lfs_size_t)size < lfs->cfg->block_count/2) {
//            LFS_DEBUG("Expanding superblock at rev %"PRIu32, dir->rev);
//            int err = lfs_dir_split(lfs, dir, attrs, attrcount,
//                    source, begin, end);
//            if (err && err != LFS_ERR_NOSPC) {
//                return err;
//            }
//
//            if (err) {
//                // welp, we tried, if we ran out of space there's not much
//                // we can do, we'll error later if we've become frozen
//                LFS_WARN("Unable to expand superblock");
//            } else {
//                end = begin;
//            }
//        }
//    }
//
//    return lfs_dir_compact(lfs, dir, attrs, attrcount, source, begin, end);
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_relocatingcommit(lfs_t *lfs, lfs_mdir_t *dir,
//        const lfs_block_t pair[2],
//        const struct lfs_mattr *attrs, int attrcount,
//        lfs_mdir_t *pdir) {
//    int state = 0;
//
//    // calculate changes to the directory
//    bool hasdelete = false;
//    for (int i = 0; i < attrcount; i++) {
//        if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_CREATE) {
//            dir->count += 1;
//        } else if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_DELETE) {
//            LFS_ASSERT(dir->count > 0);
//            dir->count -= 1;
//            hasdelete = true;
//        } else if (lfs_tag_type1(attrs[i].tag) == LFS_TYPE_TAIL) {
//            dir->tail[0] = ((lfs_block_t*)attrs[i].buffer)[0];
//            dir->tail[1] = ((lfs_block_t*)attrs[i].buffer)[1];
//            dir->split = (lfs_tag_chunk(attrs[i].tag) & 1);
//            lfs_pair_fromle32(dir->tail);
//        }
//    }
//
//    // should we actually drop the directory block?
//    if (hasdelete && dir->count == 0) {
//        LFS_ASSERT(pdir);
//        int err = lfs_fs_pred(lfs, dir->pair, pdir);
//        if (err && err != LFS_ERR_NOENT) {
//            return err;
//        }
//
//        if (err != LFS_ERR_NOENT && pdir->split) {
//            state = LFS_OK_DROPPED;
//            goto fixmlist;
//        }
//    }
//
//    if (dir->erased) {
//        // try to commit
//        struct lfs_commit commit = {
//            .block = dir->pair[0],
//            .off = dir->off,
//            .ptag = dir->etag,
//            .crc = 0xffffffff,
//
//            .begin = dir->off,
//            .end = (lfs->cfg->metadata_max ?
//                lfs->cfg->metadata_max : lfs->cfg->block_size) - 8,
//        };
//
//        // traverse attrs that need to be written out
//        lfs_pair_tole32(dir->tail);
//        int err = lfs_dir_traverse(lfs,
//                dir, dir->off, dir->etag, attrs, attrcount,
//                0, 0, 0, 0, 0,
//                lfs_dir_commit_commit, &(struct lfs_dir_commit_commit){
//                    lfs, &commit});
//        lfs_pair_fromle32(dir->tail);
//        if (err) {
//            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
//                goto compact;
//            }
//            return err;
//        }
//
//        // commit any global diffs if we have any
//        lfs_gstate_t delta = {0};
//        lfs_gstate_xor(&delta, &lfs->gstate);
//        lfs_gstate_xor(&delta, &lfs->gdisk);
//        lfs_gstate_xor(&delta, &lfs->gdelta);
//        delta.tag &= ~LFS_MKTAG(0, 0, 0x3ff);
//        if (!lfs_gstate_iszero(&delta)) {
//            err = lfs_dir_getgstate(lfs, dir, &delta);
//            if (err) {
//                return err;
//            }
//
//            lfs_gstate_tole32(&delta);
//            err = lfs_dir_commitattr(lfs, &commit,
//                    LFS_MKTAG(LFS_TYPE_MOVESTATE, 0x3ff,
//                        sizeof(delta)), &delta);
//            if (err) {
//                if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
//                    goto compact;
//                }
//                return err;
//            }
//        }
//
//        // finalize commit with the crc
//        err = lfs_dir_commitcrc(lfs, &commit);
//        if (err) {
//            if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
//                goto compact;
//            }
//            return err;
//        }
//
//        // successful commit, update dir
//        LFS_ASSERT(commit.off % lfs->cfg->prog_size == 0);
//        dir->off = commit.off;
//        dir->etag = commit.ptag;
//        // and update gstate
//        lfs->gdisk = lfs->gstate;
//        lfs->gdelta = (lfs_gstate_t){0};
//
//        goto fixmlist;
//    }
//
//compact:
//    // fall back to compaction
//    lfs_cache_drop(lfs, &lfs->pcache);
//
//    state = lfs_dir_splittingcompact(lfs, dir, attrs, attrcount,
//            dir, 0, dir->count);
//    if (state < 0) {
//        return state;
//    }
//
//    goto fixmlist;
//
//fixmlist:;
//    // this complicated bit of logic is for fixing up any active
//    // metadata-pairs that we may have affected
//    //
//    // note we have to make two passes since the mdir passed to
//    // lfs_dir_commit could also be in this list, and even then
//    // we need to copy the pair so they don't get clobbered if we refetch
//    // our mdir.
//    lfs_block_t oldpair[2] = {pair[0], pair[1]};
//    for (struct lfs_mlist *d = lfs->mlist; d; d = d->next) {
//        if (lfs_pair_cmp(d->m.pair, oldpair) == 0) {
//            d->m = *dir;
//            if (d->m.pair != pair) {
//                for (int i = 0; i < attrcount; i++) {
//                    if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_DELETE &&
//                            d->id == lfs_tag_id(attrs[i].tag)) {
//                        d->m.pair[0] = LFS_BLOCK_NULL;
//                        d->m.pair[1] = LFS_BLOCK_NULL;
//                    } else if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_DELETE &&
//                            d->id > lfs_tag_id(attrs[i].tag)) {
//                        d->id -= 1;
//                        if (d->type == LFS_TYPE_DIR) {
//                            ((lfs_dir_t*)d)->pos -= 1;
//                        }
//                    } else if (lfs_tag_type3(attrs[i].tag) == LFS_TYPE_CREATE &&
//                            d->id >= lfs_tag_id(attrs[i].tag)) {
//                        d->id += 1;
//                        if (d->type == LFS_TYPE_DIR) {
//                            ((lfs_dir_t*)d)->pos += 1;
//                        }
//                    }
//                }
//            }
//
//            while (d->id >= d->m.count && d->m.split) {
//                // we split and id is on tail now
//                d->id -= d->m.count;
//                int err = lfs_dir_fetch(lfs, &d->m, d->m.tail);
//                if (err) {
//                    return err;
//                }
//            }
//        }
//    }
//
//    return state;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_orphaningcommit(lfs_t *lfs, lfs_mdir_t *dir,
//        const struct lfs_mattr *attrs, int attrcount) {
//    // check for any inline files that aren't RAM backed and
//    // forcefully evict them, needed for filesystem consistency
//    for (lfs_file_t *f = (lfs_file_t*)lfs->mlist; f; f = f->next) {
//        if (dir != &f->m && lfs_pair_cmp(f->m.pair, dir->pair) == 0 &&
//                f->type == LFS_TYPE_REG && (f->flags & LFS_F_INLINE) &&
//                f->ctz.size > lfs->cfg->cache_size) {
//            int err = lfs_file_outline(lfs, f);
//            if (err) {
//                return err;
//            }
//
//            err = lfs_file_flush(lfs, f);
//            if (err) {
//                return err;
//            }
//        }
//    }
//
//    lfs_block_t lpair[2] = {dir->pair[0], dir->pair[1]};
//    lfs_mdir_t ldir = *dir;
//    lfs_mdir_t pdir;
//    int state = lfs_dir_relocatingcommit(lfs, &ldir, dir->pair,
//            attrs, attrcount, &pdir);
//    if (state < 0) {
//        return state;
//    }
//
//    // update if we're not in mlist, note we may have already been
//    // updated if we are in mlist
//    if (lfs_pair_cmp(dir->pair, lpair) == 0) {
//        *dir = ldir;
//    }
//
//    // commit was successful, but may require other changes in the
//    // filesystem, these would normally be tail recursive, but we have
//    // flattened them here avoid unbounded stack usage
//
//    // need to drop?
//    if (state == LFS_OK_DROPPED) {
//        // steal state
//        int err = lfs_dir_getgstate(lfs, dir, &lfs->gdelta);
//        if (err) {
//            return err;
//        }
//
//        // steal tail, note that this can't create a recursive drop
//        lpair[0] = pdir.pair[0];
//        lpair[1] = pdir.pair[1];
//        lfs_pair_tole32(dir->tail);
//        state = lfs_dir_relocatingcommit(lfs, &pdir, lpair, LFS_MKATTRS(
//                    {LFS_MKTAG(LFS_TYPE_TAIL + dir->split, 0x3ff, 8),
//                        dir->tail}),
//                NULL);
//        lfs_pair_fromle32(dir->tail);
//        if (state < 0) {
//            return state;
//        }
//
//        ldir = pdir;
//    }
//
//    // need to relocate?
//    bool orphans = false;
//    while (state == LFS_OK_RELOCATED) {
//        LFS_DEBUG("Relocating {0x%"PRIx32", 0x%"PRIx32"} "
//                    "-> {0x%"PRIx32", 0x%"PRIx32"}",
//                lpair[0], lpair[1], ldir.pair[0], ldir.pair[1]);
//        state = 0;
//
//        // update internal root
//        if (lfs_pair_cmp(lpair, lfs->root) == 0) {
//            lfs->root[0] = ldir.pair[0];
//            lfs->root[1] = ldir.pair[1];
//        }
//
//        // update internally tracked dirs
//        for (struct lfs_mlist *d = lfs->mlist; d; d = d->next) {
//            if (lfs_pair_cmp(lpair, d->m.pair) == 0) {
//                d->m.pair[0] = ldir.pair[0];
//                d->m.pair[1] = ldir.pair[1];
//            }
//
//            if (d->type == LFS_TYPE_DIR &&
//                    lfs_pair_cmp(lpair, ((lfs_dir_t*)d)->head) == 0) {
//                ((lfs_dir_t*)d)->head[0] = ldir.pair[0];
//                ((lfs_dir_t*)d)->head[1] = ldir.pair[1];
//            }
//        }
//
//        // find parent
//        lfs_stag_t tag = lfs_fs_parent(lfs, lpair, &pdir);
//        if (tag < 0 && tag != LFS_ERR_NOENT) {
//            return tag;
//        }
//
//        bool hasparent = (tag != LFS_ERR_NOENT);
//        if (tag != LFS_ERR_NOENT) {
//            // note that if we have a parent, we must have a pred, so this will
//            // always create an orphan
//            int err = lfs_fs_preporphans(lfs, +1);
//            if (err) {
//                return err;
//            }
//
//            // fix pending move in this pair? this looks like an optimization but
//            // is in fact _required_ since relocating may outdate the move.
//            uint16_t moveid = 0x3ff;
//            if (lfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
//                moveid = lfs_tag_id(lfs->gstate.tag);
//                LFS_DEBUG("Fixing move while relocating "
//                        "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
//                        pdir.pair[0], pdir.pair[1], moveid);
//                lfs_fs_prepmove(lfs, 0x3ff, NULL);
//                if (moveid < lfs_tag_id(tag)) {
//                    tag -= LFS_MKTAG(0, 1, 0);
//                }
//            }
//
//            lfs_block_t ppair[2] = {pdir.pair[0], pdir.pair[1]};
//            lfs_pair_tole32(ldir.pair);
//            state = lfs_dir_relocatingcommit(lfs, &pdir, ppair, LFS_MKATTRS(
//                        {LFS_MKTAG_IF(moveid != 0x3ff,
//                            LFS_TYPE_DELETE, moveid, 0), NULL},
//                        {tag, ldir.pair}),
//                    NULL);
//            lfs_pair_fromle32(ldir.pair);
//            if (state < 0) {
//                return state;
//            }
//
//            if (state == LFS_OK_RELOCATED) {
//                lpair[0] = ppair[0];
//                lpair[1] = ppair[1];
//                ldir = pdir;
//                orphans = true;
//                continue;
//            }
//        }
//
//        // find pred
//        int err = lfs_fs_pred(lfs, lpair, &pdir);
//        if (err && err != LFS_ERR_NOENT) {
//            return err;
//        }
//        LFS_ASSERT(!(hasparent && err == LFS_ERR_NOENT));
//
//        // if we can't find dir, it must be new
//        if (err != LFS_ERR_NOENT) {
//            if (lfs_gstate_hasorphans(&lfs->gstate)) {
//                // next step, clean up orphans
//                err = lfs_fs_preporphans(lfs, -hasparent);
//                if (err) {
//                    return err;
//                }
//            }
//
//            // fix pending move in this pair? this looks like an optimization
//            // but is in fact _required_ since relocating may outdate the move.
//            uint16_t moveid = 0x3ff;
//            if (lfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
//                moveid = lfs_tag_id(lfs->gstate.tag);
//                LFS_DEBUG("Fixing move while relocating "
//                        "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
//                        pdir.pair[0], pdir.pair[1], moveid);
//                lfs_fs_prepmove(lfs, 0x3ff, NULL);
//            }
//
//            // replace bad pair, either we clean up desync, or no desync occured
//            lpair[0] = pdir.pair[0];
//            lpair[1] = pdir.pair[1];
//            lfs_pair_tole32(ldir.pair);
//            state = lfs_dir_relocatingcommit(lfs, &pdir, lpair, LFS_MKATTRS(
//                        {LFS_MKTAG_IF(moveid != 0x3ff,
//                            LFS_TYPE_DELETE, moveid, 0), NULL},
//                        {LFS_MKTAG(LFS_TYPE_TAIL + pdir.split, 0x3ff, 8),
//                            ldir.pair}),
//                    NULL);
//            lfs_pair_fromle32(ldir.pair);
//            if (state < 0) {
//                return state;
//            }
//
//            ldir = pdir;
//        }
//    }
//
//    return orphans ? LFS_OK_ORPHANED : 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_dir_commit(lfs_t *lfs, lfs_mdir_t *dir,
//        const struct lfs_mattr *attrs, int attrcount) {
//    int orphans = lfs_dir_orphaningcommit(lfs, dir, attrs, attrcount);
//    if (orphans < 0) {
//        return orphans;
//    }
//
//    if (orphans) {
//        // make sure we've removed all orphans, this is a noop if there
//        // are none, but if we had nested blocks failures we may have
//        // created some
//        int err = lfs_fs_deorphan(lfs, false);
//        if (err) {
//            return err;
//        }
//    }
//
//    return 0;
//}
//#endif
//
//
///// Top level directory operations ///
//#ifndef LFS_READONLY
//static int lfs_rawmkdir(lfs_t *lfs, const char *path) {
//    // deorphan if we haven't yet, needed at most once after poweron
//    int err = lfs_fs_forceconsistency(lfs);
//    if (err) {
//        return err;
//    }
//
//    struct lfs_mlist cwd;
//    cwd.next = lfs->mlist;
//    uint16_t id;
//    err = lfs_dir_find(lfs, &cwd.m, &path, &id);
//    if (!(err == LFS_ERR_NOENT && id != 0x3ff)) {
//        return (err < 0) ? err : LFS_ERR_EXIST;
//    }
//
//    // check that name fits
//    lfs_size_t nlen = strlen(path);
//    if (nlen > lfs->name_max) {
//        return LFS_ERR_NAMETOOLONG;
//    }
//
//    // build up new directory
//    lfs_alloc_ack(lfs);
//    lfs_mdir_t dir;
//    err = lfs_dir_alloc(lfs, &dir);
//    if (err) {
//        return err;
//    }
//
//    // find end of list
//    lfs_mdir_t pred = cwd.m;
//    while (pred.split) {
//        err = lfs_dir_fetch(lfs, &pred, pred.tail);
//        if (err) {
//            return err;
//        }
//    }
//
//    // setup dir
//    lfs_pair_tole32(pred.tail);
//    err = lfs_dir_commit(lfs, &dir, LFS_MKATTRS(
//            {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8), pred.tail}));
//    lfs_pair_fromle32(pred.tail);
//    if (err) {
//        return err;
//    }
//
//    // current block not end of list?
//    if (cwd.m.split) {
//        // update tails, this creates a desync
//        err = lfs_fs_preporphans(lfs, +1);
//        if (err) {
//            return err;
//        }
//
//        // it's possible our predecessor has to be relocated, and if
//        // our parent is our predecessor's predecessor, this could have
//        // caused our parent to go out of date, fortunately we can hook
//        // ourselves into littlefs to catch this
//        cwd.type = 0;
//        cwd.id = 0;
//        lfs->mlist = &cwd;
//
//        lfs_pair_tole32(dir.pair);
//        err = lfs_dir_commit(lfs, &pred, LFS_MKATTRS(
//                {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8), dir.pair}));
//        lfs_pair_fromle32(dir.pair);
//        if (err) {
//            lfs->mlist = cwd.next;
//            return err;
//        }
//
//        lfs->mlist = cwd.next;
//        err = lfs_fs_preporphans(lfs, -1);
//        if (err) {
//            return err;
//        }
//    }
//
//    // now insert into our parent block
//    lfs_pair_tole32(dir.pair);
//    err = lfs_dir_commit(lfs, &cwd.m, LFS_MKATTRS(
//            {LFS_MKTAG(LFS_TYPE_CREATE, id, 0), NULL},
//            {LFS_MKTAG(LFS_TYPE_DIR, id, nlen), path},
//            {LFS_MKTAG(LFS_TYPE_DIRSTRUCT, id, 8), dir.pair},
//            {LFS_MKTAG_IF(!cwd.m.split,
//                LFS_TYPE_SOFTTAIL, 0x3ff, 8), dir.pair}));
//    lfs_pair_fromle32(dir.pair);
//    if (err) {
//        return err;
//    }
//
//    return 0;
//}
//#endif
//
//static int lfs_dir_rawopen(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
//    lfs_stag_t tag = lfs_dir_find(lfs, &dir->m, &path, NULL);
//    if (tag < 0) {
//        return tag;
//    }
//
//    if (lfs_tag_type3(tag) != LFS_TYPE_DIR) {
//        return LFS_ERR_NOTDIR;
//    }
//
//    lfs_block_t pair[2];
//    if (lfs_tag_id(tag) == 0x3ff) {
//        // handle root dir separately
//        pair[0] = lfs->root[0];
//        pair[1] = lfs->root[1];
//    } else {
//        // get dir pair from parent
//        lfs_stag_t res = lfs_dir_get(lfs, &dir->m, LFS_MKTAG(0x700, 0x3ff, 0),
//                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), pair);
//        if (res < 0) {
//            return res;
//        }
//        lfs_pair_fromle32(pair);
//    }
//
//    // fetch first pair
//    int err = lfs_dir_fetch(lfs, &dir->m, pair);
//    if (err) {
//        return err;
//    }
//
//    // setup entry
//    dir->head[0] = dir->m.pair[0];
//    dir->head[1] = dir->m.pair[1];
//    dir->id = 0;
//    dir->pos = 0;
//
//    // add to list of mdirs
//    dir->type = LFS_TYPE_DIR;
//    lfs_mlist_append(lfs, (struct lfs_mlist *)dir);
//
//    return 0;
//}
//
//static int lfs_dir_rawclose(lfs_t *lfs, lfs_dir_t *dir) {
//    // remove from list of mdirs
//    lfs_mlist_remove(lfs, (struct lfs_mlist *)dir);
//
//    return 0;
//}
//
//static int lfs_dir_rawread(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info) {
//    memset(info, 0, sizeof(*info));
//
//    // special offset for '.' and '..'
//    if (dir->pos == 0) {
//        info->type = LFS_TYPE_DIR;
//        strcpy(info->name, ".");
//        dir->pos += 1;
//        return true;
//    } else if (dir->pos == 1) {
//        info->type = LFS_TYPE_DIR;
//        strcpy(info->name, "..");
//        dir->pos += 1;
//        return true;
//    }
//
//    while (true) {
//        if (dir->id == dir->m.count) {
//            if (!dir->m.split) {
//                return false;
//            }
//
//            int err = lfs_dir_fetch(lfs, &dir->m, dir->m.tail);
//            if (err) {
//                return err;
//            }
//
//            dir->id = 0;
//        }
//
//        int err = lfs_dir_getinfo(lfs, &dir->m, dir->id, info);
//        if (err && err != LFS_ERR_NOENT) {
//            return err;
//        }
//
//        dir->id += 1;
//        if (err != LFS_ERR_NOENT) {
//            break;
//        }
//    }
//
//    dir->pos += 1;
//    return true;
//}
//
//static int lfs_dir_rawseek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off) {
//    // simply walk from head dir
//    int err = lfs_dir_rawrewind(lfs, dir);
//    if (err) {
//        return err;
//    }
//
//    // first two for ./..
//    dir->pos = lfs_min(2, off);
//    off -= dir->pos;
//
//    // skip superblock entry
//    dir->id = (off > 0 && lfs_pair_cmp(dir->head, lfs->root) == 0);
//
//    while (off > 0) {
//        int diff = lfs_min(dir->m.count - dir->id, off);
//        dir->id += diff;
//        dir->pos += diff;
//        off -= diff;
//
//        if (dir->id == dir->m.count) {
//            if (!dir->m.split) {
//                return LFS_ERR_INVAL;
//            }
//
//            err = lfs_dir_fetch(lfs, &dir->m, dir->m.tail);
//            if (err) {
//                return err;
//            }
//
//            dir->id = 0;
//        }
//    }
//
//    return 0;
//}
//
//static lfs_soff_t lfs_dir_rawtell(lfs_t *lfs, lfs_dir_t *dir) {
//    (void)lfs;
//    return dir->pos;
//}
//
//static int lfs_dir_rawrewind(lfs_t *lfs, lfs_dir_t *dir) {
//    // reload the head dir
//    int err = lfs_dir_fetch(lfs, &dir->m, dir->head);
//    if (err) {
//        return err;
//    }
//
//    dir->id = 0;
//    dir->pos = 0;
//    return 0;
//}
//
//
///// File index list operations ///
//static int lfs_ctz_index(lfs_t *lfs, lfs_off_t *off) {
//    lfs_off_t size = *off;
//    lfs_off_t b = lfs->cfg->block_size - 2*4;
//    lfs_off_t i = size / b;
//    if (i == 0) {
//        return 0;
//    }
//
//    i = (size - 4*(lfs_popc(i-1)+2)) / b;
//    *off = size - b*i - 4*lfs_popc(i);
//    return i;
//}
//
//static int lfs_ctz_find(lfs_t *lfs,
//        const lfs_cache_t *pcache, lfs_cache_t *rcache,
//        lfs_block_t head, lfs_size_t size,
//        lfs_size_t pos, lfs_block_t *block, lfs_off_t *off) {
//    if (size == 0) {
//        *block = LFS_BLOCK_NULL;
//        *off = 0;
//        return 0;
//    }
//
//    lfs_off_t current = lfs_ctz_index(lfs, &(lfs_off_t){size-1});
//    lfs_off_t target = lfs_ctz_index(lfs, &pos);
//
//    while (current > target) {
//        lfs_size_t skip = lfs_min(
//                lfs_npw2(current-target+1) - 1,
//                lfs_ctz(current));
//
//        int err = lfs_bd_read(lfs,
//                pcache, rcache, sizeof(head),
//                head, 4*skip, &head, sizeof(head));
//        head = lfs_fromle32(head);
//        if (err) {
//            return err;
//        }
//
//        current -= 1 << skip;
//    }
//
//    *block = head;
//    *off = pos;
//    return 0;
//}
//
//#ifndef LFS_READONLY
//static int lfs_ctz_extend(lfs_t *lfs,
//        lfs_cache_t *pcache, lfs_cache_t *rcache,
//        lfs_block_t head, lfs_size_t size,
//        lfs_block_t *block, lfs_off_t *off) {
//    while (true) {
//        // go ahead and grab a block
//        lfs_block_t nblock;
//        int err = lfs_alloc(lfs, &nblock);
//        if (err) {
//            return err;
//        }
//
//        {
//            err = lfs_bd_erase(lfs, nblock);
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    goto relocate;
//                }
//                return err;
//            }
//
//            if (size == 0) {
//                *block = nblock;
//                *off = 0;
//                return 0;
//            }
//
//            lfs_size_t noff = size - 1;
//            lfs_off_t index = lfs_ctz_index(lfs, &noff);
//            noff = noff + 1;
//
//            // just copy out the last block if it is incomplete
//            if (noff != lfs->cfg->block_size) {
//                for (lfs_off_t i = 0; i < noff; i++) {
//                    uint8_t data;
//                    err = lfs_bd_read(lfs,
//                            NULL, rcache, noff-i,
//                            head, i, &data, 1);
//                    if (err) {
//                        return err;
//                    }
//
//                    err = lfs_bd_prog(lfs,
//                            pcache, rcache, true,
//                            nblock, i, &data, 1);
//                    if (err) {
//                        if (err == LFS_ERR_CORRUPT) {
//                            goto relocate;
//                        }
//                        return err;
//                    }
//                }
//
//                *block = nblock;
//                *off = noff;
//                return 0;
//            }
//
//            // append block
//            index += 1;
//            lfs_size_t skips = lfs_ctz(index) + 1;
//            lfs_block_t nhead = head;
//            for (lfs_off_t i = 0; i < skips; i++) {
//                nhead = lfs_tole32(nhead);
//                err = lfs_bd_prog(lfs, pcache, rcache, true,
//                        nblock, 4*i, &nhead, 4);
//                nhead = lfs_fromle32(nhead);
//                if (err) {
//                    if (err == LFS_ERR_CORRUPT) {
//                        goto relocate;
//                    }
//                    return err;
//                }
//
//                if (i != skips-1) {
//                    err = lfs_bd_read(lfs,
//                            NULL, rcache, sizeof(nhead),
//                            nhead, 4*i, &nhead, sizeof(nhead));
//                    nhead = lfs_fromle32(nhead);
//                    if (err) {
//                        return err;
//                    }
//                }
//            }
//
//            *block = nblock;
//            *off = 4*skips;
//            return 0;
//        }
//
//relocate:
//        LFS_DEBUG("Bad block at 0x%"PRIx32, nblock);
//
//        // just clear cache and try a new block
//        lfs_cache_drop(lfs, pcache);
//    }
//}
//#endif
//
//static int lfs_ctz_traverse(lfs_t *lfs,
//        const lfs_cache_t *pcache, lfs_cache_t *rcache,
//        lfs_block_t head, lfs_size_t size,
//        int (*cb)(void*, lfs_block_t), void *data) {
//    if (size == 0) {
//        return 0;
//    }
//
//    lfs_off_t index = lfs_ctz_index(lfs, &(lfs_off_t){size-1});
//
//    while (true) {
//        int err = cb(data, head);
//        if (err) {
//            return err;
//        }
//
//        if (index == 0) {
//            return 0;
//        }
//
//        lfs_block_t heads[2];
//        int count = 2 - (index & 1);
//        err = lfs_bd_read(lfs,
//                pcache, rcache, count*sizeof(head),
//                head, 0, &heads, count*sizeof(head));
//        heads[0] = lfs_fromle32(heads[0]);
//        heads[1] = lfs_fromle32(heads[1]);
//        if (err) {
//            return err;
//        }
//
//        for (int i = 0; i < count-1; i++) {
//            err = cb(data, heads[i]);
//            if (err) {
//                return err;
//            }
//        }
//
//        head = heads[count-1];
//        index -= count;
//    }
//}
//
//
///// Top level file operations ///
//static int lfs_file_rawopencfg(lfs_t *lfs, lfs_file_t *file,
//        const char *path, int flags,
//        const struct lfs_file_config *cfg) {
//#ifndef LFS_READONLY
//    // deorphan if we haven't yet, needed at most once after poweron
//    if ((flags & LFS_O_WRONLY) == LFS_O_WRONLY) {
//        int err = lfs_fs_forceconsistency(lfs);
//        if (err) {
//            return err;
//        }
//    }
//#else
//    LFS_ASSERT((flags & LFS_O_RDONLY) == LFS_O_RDONLY);
//#endif
//
//    // setup simple file details
//    int err;
//    file->cfg = cfg;
//    file->flags = flags;
//    file->pos = 0;
//    file->off = 0;
//    file->cache.buffer = NULL;
//
//    // allocate entry for file if it doesn't exist
//    lfs_stag_t tag = lfs_dir_find(lfs, &file->m, &path, &file->id);
//    if (tag < 0 && !(tag == LFS_ERR_NOENT && file->id != 0x3ff)) {
//        err = tag;
//        goto cleanup;
//    }
//
//    // get id, add to list of mdirs to catch update changes
//    file->m.type = LFS_TYPE_REG;
//    lfs_mlist_append(lfs, (struct lfs_mlist *)file);
//
//#ifdef LFS_READONLY
//    if (tag == LFS_ERR_NOENT) {
//        err = LFS_ERR_NOENT;
//        goto cleanup;
//#else
//    if (tag == LFS_ERR_NOENT) {
//        if (!(flags & LFS_O_CREAT)) {
//            err = LFS_ERR_NOENT;
//            goto cleanup;
//        }
//
//        // check that name fits
//        lfs_size_t nlen = strlen(path);
//        if (nlen > lfs->name_max) {
//            err = LFS_ERR_NAMETOOLONG;
//            goto cleanup;
//        }
//
//        // get next slot and create entry to remember name
//        err = lfs_dir_commit(lfs, &file->m, LFS_MKATTRS(
//                {LFS_MKTAG(LFS_TYPE_CREATE, file->id, 0), NULL},
//                {LFS_MKTAG(LFS_TYPE_REG, file->id, nlen), path},
//                {LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0), NULL}));
//
//        // it may happen that the file name doesn't fit in the metadata blocks, e.g., a 256 byte file name will
//        // not fit in a 128 byte block.
//        err = (err == LFS_ERR_NOSPC) ? LFS_ERR_NAMETOOLONG : err;
//        if (err) {
//            goto cleanup;
//        }
//
//        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, 0);
//    } else if (flags & LFS_O_EXCL) {
//        err = LFS_ERR_EXIST;
//        goto cleanup;
//#endif
//    } else if (lfs_tag_type3(tag) != LFS_TYPE_REG) {
//        err = LFS_ERR_ISDIR;
//        goto cleanup;
//#ifndef LFS_READONLY
//    } else if (flags & LFS_O_TRUNC) {
//        // truncate if requested
//        tag = LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0);
//        file->flags |= LFS_F_DIRTY;
//#endif
//    } else {
//        // try to load what's on disk, if it's inlined we'll fix it later
//        tag = lfs_dir_get(lfs, &file->m, LFS_MKTAG(0x700, 0x3ff, 0),
//                LFS_MKTAG(LFS_TYPE_STRUCT, file->id, 8), &file->ctz);
//        if (tag < 0) {
//            err = tag;
//            goto cleanup;
//        }
//        lfs_ctz_fromle32(&file->ctz);
//    }
//
//    // fetch attrs
//    for (unsigned i = 0; i < file->cfg->attr_count; i++) {
//        // if opened for read / read-write operations
//        if ((file->flags & LFS_O_RDONLY) == LFS_O_RDONLY) {
//            lfs_stag_t res = lfs_dir_get(lfs, &file->m,
//                    LFS_MKTAG(0x7ff, 0x3ff, 0),
//                    LFS_MKTAG(LFS_TYPE_USERATTR + file->cfg->attrs[i].type,
//                        file->id, file->cfg->attrs[i].size),
//                        file->cfg->attrs[i].buffer);
//            if (res < 0 && res != LFS_ERR_NOENT) {
//                err = res;
//                goto cleanup;
//            }
//        }
//
//#ifndef LFS_READONLY
//        // if opened for write / read-write operations
//        if ((file->flags & LFS_O_WRONLY) == LFS_O_WRONLY) {
//            if (file->cfg->attrs[i].size > lfs->attr_max) {
//                err = LFS_ERR_NOSPC;
//                goto cleanup;
//            }
//
//            file->flags |= LFS_F_DIRTY;
//        }
//#endif
//    }
//
//    // allocate buffer if needed
//    if (file->cfg->buffer) {
//        file->cache.buffer = file->cfg->buffer;
//    } else {
//        file->cache.buffer = lfs_malloc(lfs->cfg->cache_size);
//        if (!file->cache.buffer) {
//            err = LFS_ERR_NOMEM;
//            goto cleanup;
//        }
//    }
//
//    // zero to avoid information leak
//    lfs_cache_zero(lfs, &file->cache);
//
//    if (lfs_tag_type3(tag) == LFS_TYPE_INLINESTRUCT) {
//        // load inline files
//        file->ctz.head = LFS_BLOCK_INLINE;
//        file->ctz.size = lfs_tag_size(tag);
//        file->flags |= LFS_F_INLINE;
//        file->cache.block = file->ctz.head;
//        file->cache.off = 0;
//        file->cache.size = lfs->cfg->cache_size;
//
//        // don't always read (may be new/trunc file)
//        if (file->ctz.size > 0) {
//            lfs_stag_t res = lfs_dir_get(lfs, &file->m,
//                    LFS_MKTAG(0x700, 0x3ff, 0),
//                    LFS_MKTAG(LFS_TYPE_STRUCT, file->id,
//                        lfs_min(file->cache.size, 0x3fe)),
//                    file->cache.buffer);
//            if (res < 0) {
//                err = res;
//                goto cleanup;
//            }
//        }
//    }
//
//    return 0;
//
//cleanup:
//    // clean up lingering resources
//#ifndef LFS_READONLY
//    file->flags |= LFS_F_ERRED;
//#endif
//    lfs_file_rawclose(lfs, file);
//    return err;
//}
//
//#ifndef LFS_NO_MALLOC
//static int lfs_file_rawopen(lfs_t *lfs, lfs_file_t *file,
//        const char *path, int flags) {
//    static const struct lfs_file_config defaults = {0};
//    int err = lfs_file_rawopencfg(lfs, file, path, flags, &defaults);
//    return err;
//}
//#endif
//
//static int lfs_file_rawclose(lfs_t *lfs, lfs_file_t *file) {
//#ifndef LFS_READONLY
//    int err = lfs_file_rawsync(lfs, file);
//#else
//    int err = 0;
//#endif
//
//    // remove from list of mdirs
//    lfs_mlist_remove(lfs, (struct lfs_mlist*)file);
//
//    // clean up memory
//    if (!file->cfg->buffer) {
//        lfs_free(file->cache.buffer);
//    }
//
//    return err;
//}
//
//
//#ifndef LFS_READONLY
//static int lfs_file_relocate(lfs_t *lfs, lfs_file_t *file) {
//    while (true) {
//        // just relocate what exists into new block
//        lfs_block_t nblock;
//        int err = lfs_alloc(lfs, &nblock);
//        if (err) {
//            return err;
//        }
//
//        err = lfs_bd_erase(lfs, nblock);
//        if (err) {
//            if (err == LFS_ERR_CORRUPT) {
//                goto relocate;
//            }
//            return err;
//        }
//
//        // either read from dirty cache or disk
//        for (lfs_off_t i = 0; i < file->off; i++) {
//            uint8_t data;
//            if (file->flags & LFS_F_INLINE) {
//                err = lfs_dir_getread(lfs, &file->m,
//                        // note we evict inline files before they can be dirty
//                        NULL, &file->cache, file->off-i,
//                        LFS_MKTAG(0xfff, 0x1ff, 0),
//                        LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0),
//                        i, &data, 1);
//                if (err) {
//                    return err;
//                }
//            } else {
//                err = lfs_bd_read(lfs,
//                        &file->cache, &lfs->rcache, file->off-i,
//                        file->block, i, &data, 1);
//                if (err) {
//                    return err;
//                }
//            }
//
//            err = lfs_bd_prog(lfs,
//                    &lfs->pcache, &lfs->rcache, true,
//                    nblock, i, &data, 1);
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    goto relocate;
//                }
//                return err;
//            }
//        }
//
//        // copy over new state of file
//        memcpy(file->cache.buffer, lfs->pcache.buffer, lfs->cfg->cache_size);
//        file->cache.block = lfs->pcache.block;
//        file->cache.off = lfs->pcache.off;
//        file->cache.size = lfs->pcache.size;
//        lfs_cache_zero(lfs, &lfs->pcache);
//
//        file->block = nblock;
//        file->flags |= LFS_F_WRITING;
//        return 0;
//
//relocate:
//        LFS_DEBUG("Bad block at 0x%"PRIx32, nblock);
//
//        // just clear cache and try a new block
//        lfs_cache_drop(lfs, &lfs->pcache);
//    }
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_file_outline(lfs_t *lfs, lfs_file_t *file) {
//    file->off = file->pos;
//    lfs_alloc_ack(lfs);
//    int err = lfs_file_relocate(lfs, file);
//    if (err) {
//        return err;
//    }
//
//    file->flags &= ~LFS_F_INLINE;
//    return 0;
//}
//#endif
//
//static int lfs_file_flush(lfs_t *lfs, lfs_file_t *file) {
//    if (file->flags & LFS_F_READING) {
//        if (!(file->flags & LFS_F_INLINE)) {
//            lfs_cache_drop(lfs, &file->cache);
//        }
//        file->flags &= ~LFS_F_READING;
//    }
//
//#ifndef LFS_READONLY
//    if (file->flags & LFS_F_WRITING) {
//        lfs_off_t pos = file->pos;
//
//        if (!(file->flags & LFS_F_INLINE)) {
//            // copy over anything after current branch
//            lfs_file_t orig = {
//                .ctz.head = file->ctz.head,
//                .ctz.size = file->ctz.size,
//                .flags = LFS_O_RDONLY,
//                .pos = file->pos,
//                .cache = lfs->rcache,
//            };
//            lfs_cache_drop(lfs, &lfs->rcache);
//
//            while (file->pos < file->ctz.size) {
//                // copy over a byte at a time, leave it up to caching
//                // to make this efficient
//                uint8_t data;
//                lfs_ssize_t res = lfs_file_flushedread(lfs, &orig, &data, 1);
//                if (res < 0) {
//                    return res;
//                }
//
//                res = lfs_file_flushedwrite(lfs, file, &data, 1);
//                if (res < 0) {
//                    return res;
//                }
//
//                // keep our reference to the rcache in sync
//                if (lfs->rcache.block != LFS_BLOCK_NULL) {
//                    lfs_cache_drop(lfs, &orig.cache);
//                    lfs_cache_drop(lfs, &lfs->rcache);
//                }
//            }
//
//            // write out what we have
//            while (true) {
//                int err = lfs_bd_flush(lfs, &file->cache, &lfs->rcache, true);
//                if (err) {
//                    if (err == LFS_ERR_CORRUPT) {
//                        goto relocate;
//                    }
//                    return err;
//                }
//
//                break;
//
//relocate:
//                LFS_DEBUG("Bad block at 0x%"PRIx32, file->block);
//                err = lfs_file_relocate(lfs, file);
//                if (err) {
//                    return err;
//                }
//            }
//        } else {
//            file->pos = lfs_max(file->pos, file->ctz.size);
//        }
//
//        // actual file updates
//        file->ctz.head = file->block;
//        file->ctz.size = file->pos;
//        file->flags &= ~LFS_F_WRITING;
//        file->flags |= LFS_F_DIRTY;
//
//        file->pos = pos;
//    }
//#endif
//
//    return 0;
//}
//
//#ifndef LFS_READONLY
//static int lfs_file_rawsync(lfs_t *lfs, lfs_file_t *file) {
//    if (file->flags & LFS_F_ERRED) {
//        // it's not safe to do anything if our file errored
//        return 0;
//    }
//
//    int err = lfs_file_flush(lfs, file);
//    if (err) {
//        file->flags |= LFS_F_ERRED;
//        return err;
//    }
//
//
//    if ((file->flags & LFS_F_DIRTY) &&
//            !lfs_pair_isnull(file->m.pair)) {
//        // update dir entry
//        uint16_t type;
//        const void *buffer;
//        lfs_size_t size;
//        struct lfs_ctz ctz;
//        if (file->flags & LFS_F_INLINE) {
//            // inline the whole file
//            type = LFS_TYPE_INLINESTRUCT;
//            buffer = file->cache.buffer;
//            size = file->ctz.size;
//        } else {
//            // update the ctz reference
//            type = LFS_TYPE_CTZSTRUCT;
//            // copy ctz so alloc will work during a relocate
//            ctz = file->ctz;
//            lfs_ctz_tole32(&ctz);
//            buffer = &ctz;
//            size = sizeof(ctz);
//        }
//
//        // commit file data and attributes
//        err = lfs_dir_commit(lfs, &file->m, LFS_MKATTRS(
//                {LFS_MKTAG(type, file->id, size), buffer},
//                {LFS_MKTAG(LFS_FROM_USERATTRS, file->id,
//                    file->cfg->attr_count), file->cfg->attrs}));
//        if (err) {
//            file->flags |= LFS_F_ERRED;
//            return err;
//        }
//
//        file->flags &= ~LFS_F_DIRTY;
//    }
//
//    return 0;
//}
//#endif
//
//static lfs_ssize_t lfs_file_flushedread(lfs_t *lfs, lfs_file_t *file,
//        void *buffer, lfs_size_t size) {
//    uint8_t *data = buffer;
//    lfs_size_t nsize = size;
//
//    if (file->pos >= file->ctz.size) {
//        // eof if past end
//        return 0;
//    }
//
//    size = lfs_min(size, file->ctz.size - file->pos);
//    nsize = size;
//
//    while (nsize > 0) {
//        // check if we need a new block
//        if (!(file->flags & LFS_F_READING) ||
//                file->off == lfs->cfg->block_size) {
//            if (!(file->flags & LFS_F_INLINE)) {
//                int err = lfs_ctz_find(lfs, NULL, &file->cache,
//                        file->ctz.head, file->ctz.size,
//                        file->pos, &file->block, &file->off);
//                if (err) {
//                    return err;
//                }
//            } else {
//                file->block = LFS_BLOCK_INLINE;
//                file->off = file->pos;
//            }
//
//            file->flags |= LFS_F_READING;
//        }
//
//        // read as much as we can in current block
//        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
//        if (file->flags & LFS_F_INLINE) {
//            int err = lfs_dir_getread(lfs, &file->m,
//                    NULL, &file->cache, lfs->cfg->block_size,
//                    LFS_MKTAG(0xfff, 0x1ff, 0),
//                    LFS_MKTAG(LFS_TYPE_INLINESTRUCT, file->id, 0),
//                    file->off, data, diff);
//            if (err) {
//                return err;
//            }
//        } else {
//            int err = lfs_bd_read(lfs,
//                    NULL, &file->cache, lfs->cfg->block_size,
//                    file->block, file->off, data, diff);
//            if (err) {
//                return err;
//            }
//        }
//
//        file->pos += diff;
//        file->off += diff;
//        data += diff;
//        nsize -= diff;
//    }
//
//    return size;
//}
//
//static lfs_ssize_t lfs_file_rawread(lfs_t *lfs, lfs_file_t *file,
//        void *buffer, lfs_size_t size) {
//    LFS_ASSERT((file->flags & LFS_O_RDONLY) == LFS_O_RDONLY);
//
//#ifndef LFS_READONLY
//    if (file->flags & LFS_F_WRITING) {
//        // flush out any writes
//        int err = lfs_file_flush(lfs, file);
//        if (err) {
//            return err;
//        }
//    }
//#endif
//
//    return lfs_file_flushedread(lfs, file, buffer, size);
//}
//
//
//#ifndef LFS_READONLY
//static lfs_ssize_t lfs_file_flushedwrite(lfs_t *lfs, lfs_file_t *file,
//        const void *buffer, lfs_size_t size) {
//    const uint8_t *data = buffer;
//    lfs_size_t nsize = size;
//
//    if ((file->flags & LFS_F_INLINE) &&
//            lfs_max(file->pos+nsize, file->ctz.size) >
//            lfs_min(0x3fe, lfs_min(
//                lfs->cfg->cache_size,
//                (lfs->cfg->metadata_max ?
//                    lfs->cfg->metadata_max : lfs->cfg->block_size) / 8))) {
//        // inline file doesn't fit anymore
//        int err = lfs_file_outline(lfs, file);
//        if (err) {
//            file->flags |= LFS_F_ERRED;
//            return err;
//        }
//    }
//
//    while (nsize > 0) {
//        // check if we need a new block
//        if (!(file->flags & LFS_F_WRITING) ||
//                file->off == lfs->cfg->block_size) {
//            if (!(file->flags & LFS_F_INLINE)) {
//                if (!(file->flags & LFS_F_WRITING) && file->pos > 0) {
//                    // find out which block we're extending from
//                    int err = lfs_ctz_find(lfs, NULL, &file->cache,
//                            file->ctz.head, file->ctz.size,
//                            file->pos-1, &file->block, &file->off);
//                    if (err) {
//                        file->flags |= LFS_F_ERRED;
//                        return err;
//                    }
//
//                    // mark cache as dirty since we may have read data into it
//                    lfs_cache_zero(lfs, &file->cache);
//                }
//
//                // extend file with new blocks
//                lfs_alloc_ack(lfs);
//                int err = lfs_ctz_extend(lfs, &file->cache, &lfs->rcache,
//                        file->block, file->pos,
//                        &file->block, &file->off);
//                if (err) {
//                    file->flags |= LFS_F_ERRED;
//                    return err;
//                }
//            } else {
//                file->block = LFS_BLOCK_INLINE;
//                file->off = file->pos;
//            }
//
//            file->flags |= LFS_F_WRITING;
//        }
//
//        // program as much as we can in current block
//        lfs_size_t diff = lfs_min(nsize, lfs->cfg->block_size - file->off);
//        while (true) {
//            int err = lfs_bd_prog(lfs, &file->cache, &lfs->rcache, true,
//                    file->block, file->off, data, diff);
//            if (err) {
//                if (err == LFS_ERR_CORRUPT) {
//                    goto relocate;
//                }
//                file->flags |= LFS_F_ERRED;
//                return err;
//            }
//
//            break;
//relocate:
//            err = lfs_file_relocate(lfs, file);
//            if (err) {
//                file->flags |= LFS_F_ERRED;
//                return err;
//            }
//        }
//
//        file->pos += diff;
//        file->off += diff;
//        data += diff;
//        nsize -= diff;
//
//        lfs_alloc_ack(lfs);
//    }
//
//    return size;
//}
//
//static lfs_ssize_t lfs_file_rawwrite(lfs_t *lfs, lfs_file_t *file,
//        const void *buffer, lfs_size_t size) {
//    LFS_ASSERT((file->flags & LFS_O_WRONLY) == LFS_O_WRONLY);
//
//    if (file->flags & LFS_F_READING) {
//        // drop any reads
//        int err = lfs_file_flush(lfs, file);
//        if (err) {
//            return err;
//        }
//    }
//
//    if ((file->flags & LFS_O_APPEND) && file->pos < file->ctz.size) {
//        file->pos = file->ctz.size;
//    }
//
//    if (file->pos + size > lfs->file_max) {
//        // Larger than file limit?
//        return LFS_ERR_FBIG;
//    }
//
//    if (!(file->flags & LFS_F_WRITING) && file->pos > file->ctz.size) {
//        // fill with zeros
//        lfs_off_t pos = file->pos;
//        file->pos = file->ctz.size;
//
//        while (file->pos < pos) {
//            lfs_ssize_t res = lfs_file_flushedwrite(lfs, file, &(uint8_t){0}, 1);
//            if (res < 0) {
//                return res;
//            }
//        }
//    }
//
//    lfs_ssize_t nsize = lfs_file_flushedwrite(lfs, file, buffer, size);
//    if (nsize < 0) {
//        return nsize;
//    }
//
//    file->flags &= ~LFS_F_ERRED;
//    return nsize;
//}
//#endif
//
//static lfs_soff_t lfs_file_rawseek(lfs_t *lfs, lfs_file_t *file,
//        lfs_soff_t off, int whence) {
//    // find new pos
//    lfs_off_t npos = file->pos;
//    if (whence == LFS_SEEK_SET) {
//        npos = off;
//    } else if (whence == LFS_SEEK_CUR) {
//        if ((lfs_soff_t)file->pos + off < 0) {
//            return LFS_ERR_INVAL;
//        } else {
//            npos = file->pos + off;
//        }
//    } else if (whence == LFS_SEEK_END) {
//        lfs_soff_t res = lfs_file_rawsize(lfs, file) + off;
//        if (res < 0) {
//            return LFS_ERR_INVAL;
//        } else {
//            npos = res;
//        }
//    }
//
//    if (npos > lfs->file_max) {
//        // file position out of range
//        return LFS_ERR_INVAL;
//    }
//
//    if (file->pos == npos) {
//        // noop - position has not changed
//        return npos;
//    }
//
//    // if we're only reading and our new offset is still in the file's cache
//    // we can avoid flushing and needing to reread the data
//    if (
//#ifndef LFS_READONLY
//        !(file->flags & LFS_F_WRITING)
//#else
//        true
//#endif
//            ) {
//        int oindex = lfs_ctz_index(lfs, &(lfs_off_t){file->pos});
//        lfs_off_t noff = npos;
//        int nindex = lfs_ctz_index(lfs, &noff);
//        if (oindex == nindex
//                && noff >= file->cache.off
//                && noff < file->cache.off + file->cache.size) {
//            file->pos = npos;
//            file->off = noff;
//            return npos;
//        }
//    }
//
//    // write out everything beforehand, may be noop if rdonly
//    int err = lfs_file_flush(lfs, file);
//    if (err) {
//        return err;
//    }
//
//    // update pos
//    file->pos = npos;
//    return npos;
//}
//
//#ifndef LFS_READONLY
//static int lfs_file_rawtruncate(lfs_t *lfs, lfs_file_t *file, lfs_off_t size) {
//    LFS_ASSERT((file->flags & LFS_O_WRONLY) == LFS_O_WRONLY);
//
//    if (size > LFS_FILE_MAX) {
//        return LFS_ERR_INVAL;
//    }
//
//    lfs_off_t pos = file->pos;
//    lfs_off_t oldsize = lfs_file_rawsize(lfs, file);
//    if (size < oldsize) {
//        // need to flush since directly changing metadata
//        int err = lfs_file_flush(lfs, file);
//        if (err) {
//            return err;
//        }
//
//        // lookup new head in ctz skip list
//        err = lfs_ctz_find(lfs, NULL, &file->cache,
//                file->ctz.head, file->ctz.size,
//                size, &file->block, &file->off);
//        if (err) {
//            return err;
//        }
//
//        // need to set pos/block/off consistently so seeking back to
//        // the old position does not get confused
//        file->pos = size;
//        file->ctz.head = file->block;
//        file->ctz.size = size;
//        file->flags |= LFS_F_DIRTY | LFS_F_READING;
//    } else if (size > oldsize) {
//        // flush+seek if not already at end
//        lfs_soff_t res = lfs_file_rawseek(lfs, file, 0, LFS_SEEK_END);
//        if (res < 0) {
//            return (int)res;
//        }
//
//        // fill with zeros
//        while (file->pos < size) {
//            res = lfs_file_rawwrite(lfs, file, &(uint8_t){0}, 1);
//            if (res < 0) {
//                return (int)res;
//            }
//        }
//    }
//
//    // restore pos
//    lfs_soff_t res = lfs_file_rawseek(lfs, file, pos, LFS_SEEK_SET);
//    if (res < 0) {
//      return (int)res;
//    }
//
//    return 0;
//}
//#endif
//
//static lfs_soff_t lfs_file_rawtell(lfs_t *lfs, lfs_file_t *file) {
//    (void)lfs;
//    return file->pos;
//}
//
//static int lfs_file_rawrewind(lfs_t *lfs, lfs_file_t *file) {
//    lfs_soff_t res = lfs_file_rawseek(lfs, file, 0, LFS_SEEK_SET);
//    if (res < 0) {
//        return (int)res;
//    }
//
//    return 0;
//}
//
//static lfs_soff_t lfs_file_rawsize(lfs_t *lfs, lfs_file_t *file) {
//    (void)lfs;
//
//#ifndef LFS_READONLY
//    if (file->flags & LFS_F_WRITING) {
//        return lfs_max(file->pos, file->ctz.size);
//    }
//#endif
//
//    return file->ctz.size;
//}
//
//
///// General fs operations ///
//static int lfs_rawstat(lfs_t *lfs, const char *path, struct lfs_info *info) {
//    lfs_mdir_t cwd;
//    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
//    if (tag < 0) {
//        return (int)tag;
//    }
//
//    return lfs_dir_getinfo(lfs, &cwd, lfs_tag_id(tag), info);
//}
//
//#ifndef LFS_READONLY
//static int lfs_rawremove(lfs_t *lfs, const char *path) {
//    // deorphan if we haven't yet, needed at most once after poweron
//    int err = lfs_fs_forceconsistency(lfs);
//    if (err) {
//        return err;
//    }
//
//    lfs_mdir_t cwd;
//    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
//    if (tag < 0 || lfs_tag_id(tag) == 0x3ff) {
//        return (tag < 0) ? (int)tag : LFS_ERR_INVAL;
//    }
//
//    struct lfs_mlist dir;
//    dir.next = lfs->mlist;
//    if (lfs_tag_type3(tag) == LFS_TYPE_DIR) {
//        // must be empty before removal
//        lfs_block_t pair[2];
//        lfs_stag_t res = lfs_dir_get(lfs, &cwd, LFS_MKTAG(0x700, 0x3ff, 0),
//                LFS_MKTAG(LFS_TYPE_STRUCT, lfs_tag_id(tag), 8), pair);
//        if (res < 0) {
//            return (int)res;
//        }
//        lfs_pair_fromle32(pair);
//
//        err = lfs_dir_fetch(lfs, &dir.m, pair);
//        if (err) {
//            return err;
//        }
//
//        if (dir.m.count > 0 || dir.m.split) {
//            return LFS_ERR_NOTEMPTY;
//        }
//
//        // mark fs as orphaned
//        err = lfs_fs_preporphans(lfs, +1);
//        if (err) {
//            return err;
//        }
//
//        // I know it's crazy but yes, dir can be changed by our parent's
//        // commit (if predecessor is child)
//        dir.type = 0;
//        dir.id = 0;
//        lfs->mlist = &dir;
//    }
//
//    // delete the entry
//    err = lfs_dir_commit(lfs, &cwd, LFS_MKATTRS(
//            {LFS_MKTAG(LFS_TYPE_DELETE, lfs_tag_id(tag), 0), NULL}));
//    if (err) {
//        lfs->mlist = dir.next;
//        return err;
//    }
//
//    lfs->mlist = dir.next;
//    if (lfs_tag_type3(tag) == LFS_TYPE_DIR) {
//        // fix orphan
//        err = lfs_fs_preporphans(lfs, -1);
//        if (err) {
//            return err;
//        }
//
//        err = lfs_fs_pred(lfs, dir.m.pair, &cwd);
//        if (err) {
//            return err;
//        }
//
//        err = lfs_dir_drop(lfs, &cwd, &dir.m);
//        if (err) {
//            return err;
//        }
//    }
//
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_rawrename(lfs_t *lfs, const char *oldpath, const char *newpath) {
//    // deorphan if we haven't yet, needed at most once after poweron
//    int err = lfs_fs_forceconsistency(lfs);
//    if (err) {
//        return err;
//    }
//
//    // find old entry
//    lfs_mdir_t oldcwd;
//    lfs_stag_t oldtag = lfs_dir_find(lfs, &oldcwd, &oldpath, NULL);
//    if (oldtag < 0 || lfs_tag_id(oldtag) == 0x3ff) {
//        return (oldtag < 0) ? (int)oldtag : LFS_ERR_INVAL;
//    }
//
//    // find new entry
//    lfs_mdir_t newcwd;
//    uint16_t newid;
//    lfs_stag_t prevtag = lfs_dir_find(lfs, &newcwd, &newpath, &newid);
//    if ((prevtag < 0 || lfs_tag_id(prevtag) == 0x3ff) &&
//            !(prevtag == LFS_ERR_NOENT && newid != 0x3ff)) {
//        return (prevtag < 0) ? (int)prevtag : LFS_ERR_INVAL;
//    }
//
//    // if we're in the same pair there's a few special cases...
//    bool samepair = (lfs_pair_cmp(oldcwd.pair, newcwd.pair) == 0);
//    uint16_t newoldid = lfs_tag_id(oldtag);
//
//    struct lfs_mlist prevdir;
//    prevdir.next = lfs->mlist;
//    if (prevtag == LFS_ERR_NOENT) {
//        // check that name fits
//        lfs_size_t nlen = strlen(newpath);
//        if (nlen > lfs->name_max) {
//            return LFS_ERR_NAMETOOLONG;
//        }
//
//        // there is a small chance we are being renamed in the same
//        // directory/ to an id less than our old id, the global update
//        // to handle this is a bit messy
//        if (samepair && newid <= newoldid) {
//            newoldid += 1;
//        }
//    } else if (lfs_tag_type3(prevtag) != lfs_tag_type3(oldtag)) {
//        return LFS_ERR_ISDIR;
//    } else if (samepair && newid == newoldid) {
//        // we're renaming to ourselves??
//        return 0;
//    } else if (lfs_tag_type3(prevtag) == LFS_TYPE_DIR) {
//        // must be empty before removal
//        lfs_block_t prevpair[2];
//        lfs_stag_t res = lfs_dir_get(lfs, &newcwd, LFS_MKTAG(0x700, 0x3ff, 0),
//                LFS_MKTAG(LFS_TYPE_STRUCT, newid, 8), prevpair);
//        if (res < 0) {
//            return (int)res;
//        }
//        lfs_pair_fromle32(prevpair);
//
//        // must be empty before removal
//        err = lfs_dir_fetch(lfs, &prevdir.m, prevpair);
//        if (err) {
//            return err;
//        }
//
//        if (prevdir.m.count > 0 || prevdir.m.split) {
//            return LFS_ERR_NOTEMPTY;
//        }
//
//        // mark fs as orphaned
//        err = lfs_fs_preporphans(lfs, +1);
//        if (err) {
//            return err;
//        }
//
//        // I know it's crazy but yes, dir can be changed by our parent's
//        // commit (if predecessor is child)
//        prevdir.type = 0;
//        prevdir.id = 0;
//        lfs->mlist = &prevdir;
//    }
//
//    if (!samepair) {
//        lfs_fs_prepmove(lfs, newoldid, oldcwd.pair);
//    }
//
//    // move over all attributes
//    err = lfs_dir_commit(lfs, &newcwd, LFS_MKATTRS(
//            {LFS_MKTAG_IF(prevtag != LFS_ERR_NOENT,
//                LFS_TYPE_DELETE, newid, 0), NULL},
//            {LFS_MKTAG(LFS_TYPE_CREATE, newid, 0), NULL},
//            {LFS_MKTAG(lfs_tag_type3(oldtag), newid, strlen(newpath)), newpath},
//            {LFS_MKTAG(LFS_FROM_MOVE, newid, lfs_tag_id(oldtag)), &oldcwd},
//            {LFS_MKTAG_IF(samepair,
//                LFS_TYPE_DELETE, newoldid, 0), NULL}));
//    if (err) {
//        lfs->mlist = prevdir.next;
//        return err;
//    }
//
//    // let commit clean up after move (if we're different! otherwise move
//    // logic already fixed it for us)
//    if (!samepair && lfs_gstate_hasmove(&lfs->gstate)) {
//        // prep gstate and delete move id
//        lfs_fs_prepmove(lfs, 0x3ff, NULL);
//        err = lfs_dir_commit(lfs, &oldcwd, LFS_MKATTRS(
//                {LFS_MKTAG(LFS_TYPE_DELETE, lfs_tag_id(oldtag), 0), NULL}));
//        if (err) {
//            lfs->mlist = prevdir.next;
//            return err;
//        }
//    }
//
//    lfs->mlist = prevdir.next;
//    if (prevtag != LFS_ERR_NOENT
//            && lfs_tag_type3(prevtag) == LFS_TYPE_DIR) {
//        // fix orphan
//        err = lfs_fs_preporphans(lfs, -1);
//        if (err) {
//            return err;
//        }
//
//        err = lfs_fs_pred(lfs, prevdir.m.pair, &newcwd);
//        if (err) {
//            return err;
//        }
//
//        err = lfs_dir_drop(lfs, &newcwd, &prevdir.m);
//        if (err) {
//            return err;
//        }
//    }
//
//    return 0;
//}
//#endif
//
//static lfs_ssize_t lfs_rawgetattr(lfs_t *lfs, const char *path,
//        uint8_t type, void *buffer, lfs_size_t size) {
//    lfs_mdir_t cwd;
//    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
//    if (tag < 0) {
//        return tag;
//    }
//
//    uint16_t id = lfs_tag_id(tag);
//    if (id == 0x3ff) {
//        // special case for root
//        id = 0;
//        int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
//        if (err) {
//            return err;
//        }
//    }
//
//    tag = lfs_dir_get(lfs, &cwd, LFS_MKTAG(0x7ff, 0x3ff, 0),
//            LFS_MKTAG(LFS_TYPE_USERATTR + type,
//                id, lfs_min(size, lfs->attr_max)),
//            buffer);
//    if (tag < 0) {
//        if (tag == LFS_ERR_NOENT) {
//            return LFS_ERR_NOATTR;
//        }
//
//        return tag;
//    }
//
//    return lfs_tag_size(tag);
//}
//
//#ifndef LFS_READONLY
//static int lfs_commitattr(lfs_t *lfs, const char *path,
//        uint8_t type, const void *buffer, lfs_size_t size) {
//    lfs_mdir_t cwd;
//    lfs_stag_t tag = lfs_dir_find(lfs, &cwd, &path, NULL);
//    if (tag < 0) {
//        return tag;
//    }
//
//    uint16_t id = lfs_tag_id(tag);
//    if (id == 0x3ff) {
//        // special case for root
//        id = 0;
//        int err = lfs_dir_fetch(lfs, &cwd, lfs->root);
//        if (err) {
//            return err;
//        }
//    }
//
//    return lfs_dir_commit(lfs, &cwd, LFS_MKATTRS(
//            {LFS_MKTAG(LFS_TYPE_USERATTR + type, id, size), buffer}));
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_rawsetattr(lfs_t *lfs, const char *path,
//        uint8_t type, const void *buffer, lfs_size_t size) {
//    if (size > lfs->attr_max) {
//        return LFS_ERR_NOSPC;
//    }
//
//    return lfs_commitattr(lfs, path, type, buffer, size);
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_rawremoveattr(lfs_t *lfs, const char *path, uint8_t type) {
//    return lfs_commitattr(lfs, path, type, NULL, 0x3ff);
//}
//#endif
//

/// Filesystem operations ///
static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg) {
    // TODO this all needs to be cleaned up
    lfs->cfg = cfg;
    int err = 0;

    // validate that the lfs-cfg sizes were initiated properly before
    // performing any arithmetic logics with them
    LFS_ASSERT(lfs->cfg->read_size != 0);
    LFS_ASSERT(lfs->cfg->prog_size != 0);
    LFS_ASSERT(lfs->cfg->rcache_size != 0);
    LFS_ASSERT(lfs->cfg->pcache_size != 0);

    // cache sizes must be a multiple of their operation sizes
    LFS_ASSERT(lfs->cfg->rcache_size % lfs->cfg->read_size == 0);
    LFS_ASSERT(lfs->cfg->pcache_size % lfs->cfg->prog_size == 0);

    // block_size must be a multiple of both prog/read size
    LFS_ASSERT(lfs->cfg->block_size % lfs->cfg->read_size == 0);
    LFS_ASSERT(lfs->cfg->block_size % lfs->cfg->prog_size == 0);

    // block_size is currently limited to 28-bits
    LFS_ASSERT(lfs->cfg->block_size <= 0x0fffffff);

//    // check that the block size is large enough to fit ctz pointers
//    LFS_ASSERT(4*lfs_npw2(0xffffffff / (lfs->cfg->block_size-2*4))
//            <= lfs->cfg->block_size);
//
//    // block_cycles = 0 is no longer supported.
//    //
//    // block_cycles is the number of erase cycles before littlefs evicts
//    // metadata logs as a part of wear leveling. Suggested values are in the
//    // range of 100-1000, or set block_cycles to -1 to disable block-level
//    // wear-leveling.
//    LFS_ASSERT(lfs->cfg->block_cycles != 0);

    // inline_size must be <= block_size/4
    LFS_ASSERT(lfs->cfg->inline_size <= lfs->cfg->block_size/4);
    // shrub_size must be <= block_size/4
    LFS_ASSERT(lfs->cfg->shrub_size <= lfs->cfg->block_size/4);
    // fragment_size must be <= block_size/4
    LFS_ASSERT(lfs->cfg->fragment_size <= lfs->cfg->block_size/4);

    // setup read cache
    lfs->rcache.block = 0;
    lfs->rcache.off = 0;
    lfs->rcache.size = 0;
    if (lfs->cfg->rcache_buffer) {
        lfs->rcache.buffer = lfs->cfg->rcache_buffer;
    } else {
        lfs->rcache.buffer = lfs_malloc(lfs->cfg->rcache_size);
        if (!lfs->rcache.buffer) {
            err = LFS_ERR_NOMEM;
            goto failed;
        }
    }

    // setup program cache
    lfs->pcache.block = 0;
    lfs->pcache.off = 0;
    lfs->pcache.size = 0;
    if (lfs->cfg->pcache_buffer) {
        lfs->pcache.buffer = lfs->cfg->pcache_buffer;
    } else {
        lfs->pcache.buffer = lfs_malloc(lfs->cfg->pcache_size);
        if (!lfs->pcache.buffer) {
            err = LFS_ERR_NOMEM;
            goto failed;
        }
    }

    // setup lookahead buffer, note mount finishes initializing this after
    // we establish a decent pseudo-random seed
    LFS_ASSERT(lfs->cfg->lookahead_size > 0);
    if (lfs->cfg->lookahead_buffer) {
        lfs->lookahead.buffer = lfs->cfg->lookahead_buffer;
    } else {
        lfs->lookahead.buffer = lfs_malloc(lfs->cfg->lookahead_size);
        if (!lfs->lookahead.buffer) {
            err = LFS_ERR_NOMEM;
            goto failed;
        }
    }
    lfs->lookahead.start = 0;
    lfs->lookahead.size = 0;
    lfs->lookahead.next = 0;
    lfs->lookahead.ckpoint = 0;

    // check that the size limits are sane
    LFS_ASSERT(lfs->cfg->name_limit <= LFS_NAME_MAX);
    lfs->name_limit = lfs->cfg->name_limit;
    if (!lfs->name_limit) {
        lfs->name_limit = LFS_NAME_MAX;
    }

    LFS_ASSERT(lfs->cfg->file_limit <= LFS_FILE_MAX);
    lfs->file_limit = lfs->cfg->file_limit;
    if (!lfs->file_limit) {
        lfs->file_limit = LFS_FILE_MAX;
    }

    // setup default state
    lfs->seed = 0;

//    lfs->root[0] = LFS_BLOCK_NULL;
//    lfs->root[1] = LFS_BLOCK_NULL;
//    lfs->mlist = NULL;
//    lfs->gdisk = (lfs_gstate_t){0};
//    lfs->gstate = (lfs_gstate_t){0};
//    lfs->gdelta = (lfs_gstate_t){0};
//#ifdef LFS_MIGRATE
//    lfs->lfs1 = NULL;
//#endif

    // TODO maybe reorganize this function?

    lfs->hasorphans = false;

    // TODO do we need to recalculate these after mount?

    // find the number of bits to use for recycle counters
    //
    // Add 1, to include the initial erase, multiply by 2, since we
    // alternate which metadata block we erase each compaction, and limit
    // to 28-bits so we always have some bits to determine the most recent
    // revision.
    if (lfs->cfg->block_recycles != -1) {
        lfs->recycle_bits = lfs_min(
                lfs_nlog2(2*(lfs->cfg->block_recycles+1)+1)-1,
                28);
    } else {
        lfs->recycle_bits = -1;
    }

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
    // Note this is different from LFSR_TAG_DSIZE, which is the worst case
    // tag encoding at compile-time.
    //
    uint8_t tag_estimate
            = 2
            + (lfs_nlog2(lfs->file_limit+1)+7-1)/7
            + (lfs_nlog2(lfs->cfg->block_size)+7-1)/7;
    LFS_ASSERT(tag_estimate <= LFSR_TAG_DSIZE);
    lfs->attr_estimate = 3*tag_estimate + 4;

    // calculate the number of bits we need to reserve for mdir rids
    //
    // Worst case (or best case?) each metadata entry is a single tag. In
    // theory each entry also needs a name, but with power-of-two rounding,
    // this is negligible
    //
    // Assuming a _perfect_ compaction algorithm (requires unbounded RAM),
    // each tag also needs ~1 alt, this gives us:
    //
    //       block_size   block_size
    //   m = ---------- = ----------
    //          a_inf         2t
    //
    // Assuming t=4 bytes, the minimum tag encoding:
    //
    //       block_size   block_size
    //   m = ---------- = ----------
    //           2*4           8
    //
    // Note we can't assume ~1/2 block utilization here, as an mdir may
    // temporarily fill with more mids before compaction occurs.
    //
    // Note note our actual compaction algorithm is not perfect, and
    // requires 3t+4 bytes per tag, or with t=4 bytes => ~block_size/12
    // metadata entries per block. But we intentionally don't leverage this
    // to maintain compatibility with a theoretical perfect implementation.
    //
    lfs->mdir_bits = lfs_nlog2(lfs->cfg->block_size/8);

    // zero linked-list of opened mdirs
    lfs->opened = NULL;

    // zero gstate
    lfs_memset(lfs->grm_p, 0, LFSR_GRM_DSIZE);
    lfs_memset(lfs->grm_d, 0, LFSR_GRM_DSIZE);

    return 0;

failed:;
    lfs_deinit(lfs);
    return err;
}

static int lfs_deinit(lfs_t *lfs) {
    // free allocated memory
    if (!lfs->cfg->rcache_buffer) {
        lfs_free(lfs->rcache.buffer);
    }

    if (!lfs->cfg->pcache_buffer) {
        lfs_free(lfs->pcache.buffer);
    }

    if (!lfs->cfg->lookahead_buffer) {
        lfs_free(lfs->lookahead.buffer);
    }

    return 0;
}

//#ifndef LFS_READONLY
//static int lfs_rawformat(lfs_t *lfs, const struct lfs_config *cfg) {
//    int err = 0;
//    {
//        err = lfs_init(lfs, cfg);
//        if (err) {
//            return err;
//        }
//
//        // create free lookahead
//        memset(lfs->free.buffer, 0, lfs->cfg->lookahead_size);
//        lfs->free.off = 0;
//        lfs->free.size = lfs_min(8*lfs->cfg->lookahead_size,
//                lfs->cfg->block_count);
//        lfs->free.i = 0;
//        lfs_alloc_ack(lfs);
//
//        // create root dir
//        lfs_mdir_t root;
//        err = lfs_dir_alloc(lfs, &root);
//        if (err) {
//            goto cleanup;
//        }
//
//        // write one superblock
//        lfs_superblock_t superblock = {
//            .version     = LFS_DISK_VERSION,
//            .block_size  = lfs->cfg->block_size,
//            .block_count = lfs->cfg->block_count,
//            .name_max    = lfs->name_max,
//            .file_max    = lfs->file_max,
//            .attr_max    = lfs->attr_max,
//        };
//
//        lfs_superblock_tole32(&superblock);
//        err = lfs_dir_commit(lfs, &root, LFS_MKATTRS(
//                {LFS_MKTAG(LFS_TYPE_CREATE, 0, 0), NULL},
//                {LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, 8), "littlefs"},
//                {LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
//                    &superblock}));
//        if (err) {
//            goto cleanup;
//        }
//
//        // force compaction to prevent accidentally mounting any
//        // older version of littlefs that may live on disk
//        root.erased = false;
//        err = lfs_dir_commit(lfs, &root, NULL, 0);
//        if (err) {
//            goto cleanup;
//        }
//
//        // sanity check that fetch works
//        err = lfs_dir_fetch(lfs, &root, (const lfs_block_t[2]){0, 1});
//        if (err) {
//            goto cleanup;
//        }
//    }
//
//cleanup:
//    lfs_deinit(lfs);
//    return err;
//
//}
//#endif
//
//static int lfs_rawmount(lfs_t *lfs, const struct lfs_config *cfg) {
//    int err = lfs_init(lfs, cfg);
//    if (err) {
//        return err;
//    }
//
//    // scan directory blocks for superblock and any global updates
//    lfs_mdir_t dir = {.tail = {0, 1}};
//    lfs_block_t tortoise[2] = {LFS_BLOCK_NULL, LFS_BLOCK_NULL};
//    lfs_size_t tortoise_i = 1;
//    lfs_size_t tortoise_period = 1;
//    while (!lfs_pair_isnull(dir.tail)) {
//        // detect cycles with Brent's algorithm
//        if (lfs_pair_issync(dir.tail, tortoise)) {
//            LFS_ERROR("Cycle detected in tail list");
//            err = LFS_ERR_CORRUPT;
//            goto cleanup;
//        }
//        if (tortoise_i == tortoise_period) {
//            tortoise[0] = dir.tail[0];
//            tortoise[1] = dir.tail[1];
//            tortoise_i = 0;
//            tortoise_period *= 2;
//        }
//        tortoise_i += 1;
//
//        // fetch next block in tail list
//        lfs_stag_t tag = lfs_dir_fetchmatch(lfs, &dir, dir.tail,
//                LFS_MKTAG(0x7ff, 0x3ff, 0),
//                LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, 8),
//                NULL,
//                lfs_dir_find_match, &(struct lfs_dir_find_match){
//                    lfs, "littlefs", 8});
//        if (tag < 0) {
//            err = tag;
//            goto cleanup;
//        }
//
//        // has superblock?
//        if (tag && !lfs_tag_isdelete(tag)) {
//            // update root
//            lfs->root[0] = dir.pair[0];
//            lfs->root[1] = dir.pair[1];
//
//            // grab superblock
//            lfs_superblock_t superblock;
//            tag = lfs_dir_get(lfs, &dir, LFS_MKTAG(0x7ff, 0x3ff, 0),
//                    LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
//                    &superblock);
//            if (tag < 0) {
//                err = tag;
//                goto cleanup;
//            }
//            lfs_superblock_fromle32(&superblock);
//
//            // check version
//            uint16_t major_version = (0xffff & (superblock.version >> 16));
//            uint16_t minor_version = (0xffff & (superblock.version >>  0));
//            if ((major_version != LFS_DISK_VERSION_MAJOR ||
//                 minor_version > LFS_DISK_VERSION_MINOR)) {
//                LFS_ERROR("Invalid version v%"PRIu16".%"PRIu16,
//                        major_version, minor_version);
//                err = LFS_ERR_INVAL;
//                goto cleanup;
//            }
//
//            // check superblock configuration
//            if (superblock.name_max) {
//                if (superblock.name_max > lfs->name_max) {
//                    LFS_ERROR("Unsupported name_max (%"PRIu32" > %"PRIu32")",
//                            superblock.name_max, lfs->name_max);
//                    err = LFS_ERR_INVAL;
//                    goto cleanup;
//                }
//
//                lfs->name_max = superblock.name_max;
//            }
//
//            if (superblock.file_max) {
//                if (superblock.file_max > lfs->file_max) {
//                    LFS_ERROR("Unsupported file_max (%"PRIu32" > %"PRIu32")",
//                            superblock.file_max, lfs->file_max);
//                    err = LFS_ERR_INVAL;
//                    goto cleanup;
//                }
//
//                lfs->file_max = superblock.file_max;
//            }
//
//            if (superblock.attr_max) {
//                if (superblock.attr_max > lfs->attr_max) {
//                    LFS_ERROR("Unsupported attr_max (%"PRIu32" > %"PRIu32")",
//                            superblock.attr_max, lfs->attr_max);
//                    err = LFS_ERR_INVAL;
//                    goto cleanup;
//                }
//
//                lfs->attr_max = superblock.attr_max;
//            }
//
//            if (superblock.block_count != lfs->cfg->block_count) {
//                LFS_ERROR("Invalid block count (%"PRIu32" != %"PRIu32")",
//                        superblock.block_count, lfs->cfg->block_count);
//                err = LFS_ERR_INVAL;
//                goto cleanup;
//            }
//
//            if (superblock.block_size != lfs->cfg->block_size) {
//                LFS_ERROR("Invalid block size (%"PRIu32" != %"PRIu32")",
//                        superblock.block_size, lfs->cfg->block_size);
//                err = LFS_ERR_INVAL;
//                goto cleanup;
//            }
//        }
//
//        // has gstate?
//        err = lfs_dir_getgstate(lfs, &dir, &lfs->gstate);
//        if (err) {
//            goto cleanup;
//        }
//    }
//
//    // found superblock?
//    if (lfs_pair_isnull(lfs->root)) {
//        err = LFS_ERR_INVAL;
//        goto cleanup;
//    }
//
//    // update littlefs with gstate
//    if (!lfs_gstate_iszero(&lfs->gstate)) {
//        LFS_DEBUG("Found pending gstate 0x%08"PRIx32"%08"PRIx32"%08"PRIx32,
//                lfs->gstate.tag,
//                lfs->gstate.pair[0],
//                lfs->gstate.pair[1]);
//    }
//    lfs->gstate.tag += !lfs_tag_isvalid(lfs->gstate.tag);
//    lfs->gdisk = lfs->gstate;
//
//    // setup free lookahead, to distribute allocations uniformly across
//    // boots, we start the allocator at a random location
//    lfs->free.off = lfs->seed % lfs->cfg->block_count;
//    lfs_alloc_drop(lfs);
//
//    return 0;
//
//cleanup:
//    lfs_rawunmount(lfs);
//    return err;
//}
//
//static int lfs_rawunmount(lfs_t *lfs) {
//    return lfs_deinit(lfs);
//}
//
//
///// Filesystem filesystem operations ///
//int lfs_fs_rawtraverse(lfs_t *lfs,
//        int (*cb)(void *data, lfs_block_t block), void *data,
//        bool includeorphans) {
//    // iterate over metadata pairs
//    lfs_mdir_t dir = {.tail = {0, 1}};
//
//#ifdef LFS_MIGRATE
//    // also consider v1 blocks during migration
//    if (lfs->lfs1) {
//        int err = lfs1_traverse(lfs, cb, data);
//        if (err) {
//            return err;
//        }
//
//        dir.tail[0] = lfs->root[0];
//        dir.tail[1] = lfs->root[1];
//    }
//#endif
//
//    lfs_block_t tortoise[2] = {LFS_BLOCK_NULL, LFS_BLOCK_NULL};
//    lfs_size_t tortoise_i = 1;
//    lfs_size_t tortoise_period = 1;
//    while (!lfs_pair_isnull(dir.tail)) {
//        // detect cycles with Brent's algorithm
//        if (lfs_pair_issync(dir.tail, tortoise)) {
//            LFS_WARN("Cycle detected in tail list");
//            return LFS_ERR_CORRUPT;
//        }
//        if (tortoise_i == tortoise_period) {
//            tortoise[0] = dir.tail[0];
//            tortoise[1] = dir.tail[1];
//            tortoise_i = 0;
//            tortoise_period *= 2;
//        }
//        tortoise_i += 1;
//
//        for (int i = 0; i < 2; i++) {
//            int err = cb(data, dir.tail[i]);
//            if (err) {
//                return err;
//            }
//        }
//
//        // iterate through ids in directory
//        int err = lfs_dir_fetch(lfs, &dir, dir.tail);
//        if (err) {
//            return err;
//        }
//
//        for (uint16_t id = 0; id < dir.count; id++) {
//            struct lfs_ctz ctz;
//            lfs_stag_t tag = lfs_dir_get(lfs, &dir, LFS_MKTAG(0x700, 0x3ff, 0),
//                    LFS_MKTAG(LFS_TYPE_STRUCT, id, sizeof(ctz)), &ctz);
//            if (tag < 0) {
//                if (tag == LFS_ERR_NOENT) {
//                    continue;
//                }
//                return tag;
//            }
//            lfs_ctz_fromle32(&ctz);
//
//            if (lfs_tag_type3(tag) == LFS_TYPE_CTZSTRUCT) {
//                err = lfs_ctz_traverse(lfs, NULL, &lfs->rcache,
//                        ctz.head, ctz.size, cb, data);
//                if (err) {
//                    return err;
//                }
//            } else if (includeorphans &&
//                    lfs_tag_type3(tag) == LFS_TYPE_DIRSTRUCT) {
//                for (int i = 0; i < 2; i++) {
//                    err = cb(data, (&ctz.head)[i]);
//                    if (err) {
//                        return err;
//                    }
//                }
//            }
//        }
//    }
//
//#ifndef LFS_READONLY
//    // iterate over any open files
//    for (lfs_file_t *f = (lfs_file_t*)lfs->mlist; f; f = f->next) {
//        if (f->type != LFS_TYPE_REG) {
//            continue;
//        }
//
//        if ((f->flags & LFS_F_DIRTY) && !(f->flags & LFS_F_INLINE)) {
//            int err = lfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
//                    f->ctz.head, f->ctz.size, cb, data);
//            if (err) {
//                return err;
//            }
//        }
//
//        if ((f->flags & LFS_F_WRITING) && !(f->flags & LFS_F_INLINE)) {
//            int err = lfs_ctz_traverse(lfs, &f->cache, &lfs->rcache,
//                    f->block, f->pos, cb, data);
//            if (err) {
//                return err;
//            }
//        }
//    }
//#endif
//
//    return 0;
//}
//
//#ifndef LFS_READONLY
//static int lfs_fs_pred(lfs_t *lfs,
//        const lfs_block_t pair[2], lfs_mdir_t *pdir) {
//    // iterate over all directory directory entries
//    pdir->tail[0] = 0;
//    pdir->tail[1] = 1;
//    lfs_block_t tortoise[2] = {LFS_BLOCK_NULL, LFS_BLOCK_NULL};
//    lfs_size_t tortoise_i = 1;
//    lfs_size_t tortoise_period = 1;
//    while (!lfs_pair_isnull(pdir->tail)) {
//        // detect cycles with Brent's algorithm
//        if (lfs_pair_issync(pdir->tail, tortoise)) {
//            LFS_WARN("Cycle detected in tail list");
//            return LFS_ERR_CORRUPT;
//        }
//        if (tortoise_i == tortoise_period) {
//            tortoise[0] = pdir->tail[0];
//            tortoise[1] = pdir->tail[1];
//            tortoise_i = 0;
//            tortoise_period *= 2;
//        }
//        tortoise_i += 1;
//
//        if (lfs_pair_cmp(pdir->tail, pair) == 0) {
//            return 0;
//        }
//
//        int err = lfs_dir_fetch(lfs, pdir, pdir->tail);
//        if (err) {
//            return err;
//        }
//    }
//
//    return LFS_ERR_NOENT;
//}
//#endif
//
//#ifndef LFS_READONLY
//struct lfs_fs_parent_match {
//    lfs_t *lfs;
//    const lfs_block_t pair[2];
//};
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_fs_parent_match(void *data,
//        lfs_tag_t tag, const void *buffer) {
//    struct lfs_fs_parent_match *find = data;
//    lfs_t *lfs = find->lfs;
//    const struct lfs_diskoff *disk = buffer;
//    (void)tag;
//
//    lfs_block_t child[2];
//    int err = lfs_bd_read(lfs,
//            &lfs->pcache, &lfs->rcache, lfs->cfg->block_size,
//            disk->block, disk->off, &child, sizeof(child));
//    if (err) {
//        return err;
//    }
//
//    lfs_pair_fromle32(child);
//    return (lfs_pair_cmp(child, find->pair) == 0) ? LFS_CMP_EQ : LFS_CMP_LT;
//}
//#endif
//
//#ifndef LFS_READONLY
//static lfs_stag_t lfs_fs_parent(lfs_t *lfs, const lfs_block_t pair[2],
//        lfs_mdir_t *parent) {
//    // use fetchmatch with callback to find pairs
//    parent->tail[0] = 0;
//    parent->tail[1] = 1;
//    lfs_block_t tortoise[2] = {LFS_BLOCK_NULL, LFS_BLOCK_NULL};
//    lfs_size_t tortoise_i = 1;
//    lfs_size_t tortoise_period = 1;
//    while (!lfs_pair_isnull(parent->tail)) {
//        // detect cycles with Brent's algorithm
//        if (lfs_pair_issync(parent->tail, tortoise)) {
//            LFS_WARN("Cycle detected in tail list");
//            return LFS_ERR_CORRUPT;
//        }
//        if (tortoise_i == tortoise_period) {
//            tortoise[0] = parent->tail[0];
//            tortoise[1] = parent->tail[1];
//            tortoise_i = 0;
//            tortoise_period *= 2;
//        }
//        tortoise_i += 1;
//
//        lfs_stag_t tag = lfs_dir_fetchmatch(lfs, parent, parent->tail,
//                LFS_MKTAG(0x7ff, 0, 0x3ff),
//                LFS_MKTAG(LFS_TYPE_DIRSTRUCT, 0, 8),
//                NULL,
//                lfs_fs_parent_match, &(struct lfs_fs_parent_match){
//                    lfs, {pair[0], pair[1]}});
//        if (tag && tag != LFS_ERR_NOENT) {
//            return tag;
//        }
//    }
//
//    return LFS_ERR_NOENT;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_fs_preporphans(lfs_t *lfs, int8_t orphans) {
//    LFS_ASSERT(lfs_tag_size(lfs->gstate.tag) > 0x000 || orphans >= 0);
//    LFS_ASSERT(lfs_tag_size(lfs->gstate.tag) < 0x3ff || orphans <= 0);
//    lfs->gstate.tag += orphans;
//    lfs->gstate.tag = ((lfs->gstate.tag & ~LFS_MKTAG(0x800, 0, 0)) |
//            ((uint32_t)lfs_gstate_hasorphans(&lfs->gstate) << 31));
//
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static void lfs_fs_prepmove(lfs_t *lfs,
//        uint16_t id, const lfs_block_t pair[2]) {
//    lfs->gstate.tag = ((lfs->gstate.tag & ~LFS_MKTAG(0x7ff, 0x3ff, 0)) |
//            ((id != 0x3ff) ? LFS_MKTAG(LFS_TYPE_DELETE, id, 0) : 0));
//    lfs->gstate.pair[0] = (id != 0x3ff) ? pair[0] : 0;
//    lfs->gstate.pair[1] = (id != 0x3ff) ? pair[1] : 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_fs_demove(lfs_t *lfs) {
//    if (!lfs_gstate_hasmove(&lfs->gdisk)) {
//        return 0;
//    }
//
//    // Fix bad moves
//    LFS_DEBUG("Fixing move {0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16,
//            lfs->gdisk.pair[0],
//            lfs->gdisk.pair[1],
//            lfs_tag_id(lfs->gdisk.tag));
//
//    // no other gstate is supported at this time, so if we found something else
//    // something most likely went wrong in gstate calculation
//    LFS_ASSERT(lfs_tag_type3(lfs->gdisk.tag) == LFS_TYPE_DELETE);
//
//    // fetch and delete the moved entry
//    lfs_mdir_t movedir;
//    int err = lfs_dir_fetch(lfs, &movedir, lfs->gdisk.pair);
//    if (err) {
//        return err;
//    }
//
//    // prep gstate and delete move id
//    uint16_t moveid = lfs_tag_id(lfs->gdisk.tag);
//    lfs_fs_prepmove(lfs, 0x3ff, NULL);
//    err = lfs_dir_commit(lfs, &movedir, LFS_MKATTRS(
//            {LFS_MKTAG(LFS_TYPE_DELETE, moveid, 0), NULL}));
//    if (err) {
//        return err;
//    }
//
//    return 0;
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_fs_deorphan(lfs_t *lfs, bool powerloss) {
//    if (!lfs_gstate_hasorphans(&lfs->gstate)) {
//        return 0;
//    }
//
//    int8_t found = 0;
//
//    // Check for orphans in two separate passes:
//    // - 1 for half-orphans (relocations)
//    // - 2 for full-orphans (removes/renames)
//    //
//    // Two separate passes are needed as half-orphans can contain outdated
//    // references to full-orphans, effectively hiding them from the deorphan
//    // search.
//    //
//    int pass = 0;
//    while (pass < 2) {
//        // Fix any orphans
//        lfs_mdir_t pdir = {.split = true, .tail = {0, 1}};
//        lfs_mdir_t dir;
//        bool moreorphans = false;
//
//        // iterate over all directory directory entries
//        while (!lfs_pair_isnull(pdir.tail)) {
//            int err = lfs_dir_fetch(lfs, &dir, pdir.tail);
//            if (err) {
//                return err;
//            }
//
//            // check head blocks for orphans
//            if (!pdir.split) {
//                // check if we have a parent
//                lfs_mdir_t parent;
//                lfs_stag_t tag = lfs_fs_parent(lfs, pdir.tail, &parent);
//                if (tag < 0 && tag != LFS_ERR_NOENT) {
//                    return tag;
//                }
//
//                if (pass == 0 && tag != LFS_ERR_NOENT) {
//                    lfs_block_t pair[2];
//                    lfs_stag_t state = lfs_dir_get(lfs, &parent,
//                            LFS_MKTAG(0x7ff, 0x3ff, 0), tag, pair);
//                    if (state < 0) {
//                        return state;
//                    }
//                    lfs_pair_fromle32(pair);
//
//                    if (!lfs_pair_issync(pair, pdir.tail)) {
//                        // we have desynced
//                        LFS_DEBUG("Fixing half-orphan "
//                                "{0x%"PRIx32", 0x%"PRIx32"} "
//                                "-> {0x%"PRIx32", 0x%"PRIx32"}",
//                                pdir.tail[0], pdir.tail[1], pair[0], pair[1]);
//
//                        // fix pending move in this pair? this looks like an
//                        // optimization but is in fact _required_ since
//                        // relocating may outdate the move.
//                        uint16_t moveid = 0x3ff;
//                        if (lfs_gstate_hasmovehere(&lfs->gstate, pdir.pair)) {
//                            moveid = lfs_tag_id(lfs->gstate.tag);
//                            LFS_DEBUG("Fixing move while fixing orphans "
//                                    "{0x%"PRIx32", 0x%"PRIx32"} 0x%"PRIx16"\n",
//                                    pdir.pair[0], pdir.pair[1], moveid);
//                            lfs_fs_prepmove(lfs, 0x3ff, NULL);
//                        }
//
//                        lfs_pair_tole32(pair);
//                        state = lfs_dir_orphaningcommit(lfs, &pdir, LFS_MKATTRS(
//                                {LFS_MKTAG_IF(moveid != 0x3ff,
//                                    LFS_TYPE_DELETE, moveid, 0), NULL},
//                                {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8),
//                                    pair}));
//                        lfs_pair_fromle32(pair);
//                        if (state < 0) {
//                            return state;
//                        }
//
//                        found += 1;
//
//                        // did our commit create more orphans?
//                        if (state == LFS_OK_ORPHANED) {
//                            moreorphans = true;
//                        }
//
//                        // refetch tail
//                        continue;
//                    }
//                }
//
//                // note we only check for full orphans if we may have had a
//                // power-loss, otherwise orphans are created intentionally
//                // during operations such as lfs_mkdir
//                if (pass == 1 && tag == LFS_ERR_NOENT && powerloss) {
//                    // we are an orphan
//                    LFS_DEBUG("Fixing orphan {0x%"PRIx32", 0x%"PRIx32"}",
//                            pdir.tail[0], pdir.tail[1]);
//
//                    // steal state
//                    err = lfs_dir_getgstate(lfs, &dir, &lfs->gdelta);
//                    if (err) {
//                        return err;
//                    }
//
//                    // steal tail
//                    lfs_pair_tole32(dir.tail);
//                    int state = lfs_dir_orphaningcommit(lfs, &pdir, LFS_MKATTRS(
//                            {LFS_MKTAG(LFS_TYPE_TAIL + dir.split, 0x3ff, 8),
//                                dir.tail}));
//                    lfs_pair_fromle32(dir.tail);
//                    if (state < 0) {
//                        return state;
//                    }
//
//                    found += 1;
//
//                    // did our commit create more orphans?
//                    if (state == LFS_OK_ORPHANED) {
//                        moreorphans = true;
//                    }
//
//                    // refetch tail
//                    continue;
//                }
//            }
//
//            pdir = dir;
//        }
//
//        pass = moreorphans ? 0 : pass+1;
//    }
//
//    // mark orphans as fixed
//    return lfs_fs_preporphans(lfs, -lfs_min(
//            lfs_gstate_getorphans(&lfs->gstate),
//            found));
//}
//#endif
//
//#ifndef LFS_READONLY
//static int lfs_fs_forceconsistency(lfs_t *lfs) {
//    int err = lfs_fs_demove(lfs);
//    if (err) {
//        return err;
//    }
//
//    err = lfs_fs_deorphan(lfs, true);
//    if (err) {
//        return err;
//    }
//
//    return 0;
//}
//#endif
//
//static int lfs_fs_size_count(void *p, lfs_block_t block) {
//    (void)block;
//    lfs_size_t *size = p;
//    *size += 1;
//    return 0;
//}
//
//static lfs_ssize_t lfs_fs_rawsize(lfs_t *lfs) {
//    lfs_size_t size = 0;
//    int err = lfs_fs_rawtraverse(lfs, lfs_fs_size_count, &size, false);
//    if (err) {
//        return err;
//    }
//
//    return size;
//}
//
//#ifdef LFS_MIGRATE
//////// Migration from littelfs v1 below this //////
//
///// Version info ///
//
//// Software library version
//// Major (top-nibble), incremented on backwards incompatible changes
//// Minor (bottom-nibble), incremented on feature additions
//#define LFS1_VERSION 0x00010007
//#define LFS1_VERSION_MAJOR (0xffff & (LFS1_VERSION >> 16))
//#define LFS1_VERSION_MINOR (0xffff & (LFS1_VERSION >>  0))
//
//// Version of On-disk data structures
//// Major (top-nibble), incremented on backwards incompatible changes
//// Minor (bottom-nibble), incremented on feature additions
//#define LFS1_DISK_VERSION 0x00010001
//#define LFS1_DISK_VERSION_MAJOR (0xffff & (LFS1_DISK_VERSION >> 16))
//#define LFS1_DISK_VERSION_MINOR (0xffff & (LFS1_DISK_VERSION >>  0))
//
//
///// v1 Definitions ///
//
//// File types
//enum lfs1_type {
//    LFS1_TYPE_REG        = 0x11,
//    LFS1_TYPE_DIR        = 0x22,
//    LFS1_TYPE_SUPERBLOCK = 0x2e,
//};
//
//typedef struct lfs1 {
//    lfs_block_t root[2];
//} lfs1_t;
//
//typedef struct lfs1_entry {
//    lfs_off_t off;
//
//    struct lfs1_disk_entry {
//        uint8_t type;
//        uint8_t elen;
//        uint8_t alen;
//        uint8_t nlen;
//        union {
//            struct {
//                lfs_block_t head;
//                lfs_size_t size;
//            } file;
//            lfs_block_t dir[2];
//        } u;
//    } d;
//} lfs1_entry_t;
//
//typedef struct lfs1_dir {
//    struct lfs1_dir *next;
//    lfs_block_t pair[2];
//    lfs_off_t off;
//
//    lfs_block_t head[2];
//    lfs_off_t pos;
//
//    struct lfs1_disk_dir {
//        uint32_t rev;
//        lfs_size_t size;
//        lfs_block_t tail[2];
//    } d;
//} lfs1_dir_t;
//
//typedef struct lfs1_superblock {
//    lfs_off_t off;
//
//    struct lfs1_disk_superblock {
//        uint8_t type;
//        uint8_t elen;
//        uint8_t alen;
//        uint8_t nlen;
//        lfs_block_t root[2];
//        uint32_t block_size;
//        uint32_t block_count;
//        uint32_t version;
//        char magic[8];
//    } d;
//} lfs1_superblock_t;
//
//
///// Low-level wrappers v1->v2 ///
//static void lfs1_crc(uint32_t *crc, const void *buffer, size_t size) {
//    *crc = lfs_crc(*crc, buffer, size);
//}
//
//static int lfs1_bd_read(lfs_t *lfs, lfs_block_t block,
//        lfs_off_t off, void *buffer, lfs_size_t size) {
//    // if we ever do more than writes to alternating pairs,
//    // this may need to consider pcache
//    return lfs_bd_read(lfs, &lfs->pcache, &lfs->rcache, size,
//            block, off, buffer, size);
//}
//
//static int lfs1_bd_crc(lfs_t *lfs, lfs_block_t block,
//        lfs_off_t off, lfs_size_t size, uint32_t *crc) {
//    for (lfs_off_t i = 0; i < size; i++) {
//        uint8_t c;
//        int err = lfs1_bd_read(lfs, block, off+i, &c, 1);
//        if (err) {
//            return err;
//        }
//
//        lfs1_crc(crc, &c, 1);
//    }
//
//    return 0;
//}
//
//
///// Endian swapping functions ///
//static void lfs1_dir_fromle32(struct lfs1_disk_dir *d) {
//    d->rev     = lfs_fromle32(d->rev);
//    d->size    = lfs_fromle32(d->size);
//    d->tail[0] = lfs_fromle32(d->tail[0]);
//    d->tail[1] = lfs_fromle32(d->tail[1]);
//}
//
//static void lfs1_dir_tole32(struct lfs1_disk_dir *d) {
//    d->rev     = lfs_tole32(d->rev);
//    d->size    = lfs_tole32(d->size);
//    d->tail[0] = lfs_tole32(d->tail[0]);
//    d->tail[1] = lfs_tole32(d->tail[1]);
//}
//
//static void lfs1_entry_fromle32(struct lfs1_disk_entry *d) {
//    d->u.dir[0] = lfs_fromle32(d->u.dir[0]);
//    d->u.dir[1] = lfs_fromle32(d->u.dir[1]);
//}
//
//static void lfs1_entry_tole32(struct lfs1_disk_entry *d) {
//    d->u.dir[0] = lfs_tole32(d->u.dir[0]);
//    d->u.dir[1] = lfs_tole32(d->u.dir[1]);
//}
//
//static void lfs1_superblock_fromle32(struct lfs1_disk_superblock *d) {
//    d->root[0]     = lfs_fromle32(d->root[0]);
//    d->root[1]     = lfs_fromle32(d->root[1]);
//    d->block_size  = lfs_fromle32(d->block_size);
//    d->block_count = lfs_fromle32(d->block_count);
//    d->version     = lfs_fromle32(d->version);
//}
//
//
/////// Metadata pair and directory operations ///
//static inline lfs_size_t lfs1_entry_size(const lfs1_entry_t *entry) {
//    return 4 + entry->d.elen + entry->d.alen + entry->d.nlen;
//}
//
//static int lfs1_dir_fetch(lfs_t *lfs,
//        lfs1_dir_t *dir, const lfs_block_t pair[2]) {
//    // copy out pair, otherwise may be aliasing dir
//    const lfs_block_t tpair[2] = {pair[0], pair[1]};
//    bool valid = false;
//
//    // check both blocks for the most recent revision
//    for (int i = 0; i < 2; i++) {
//        struct lfs1_disk_dir test;
//        int err = lfs1_bd_read(lfs, tpair[i], 0, &test, sizeof(test));
//        lfs1_dir_fromle32(&test);
//        if (err) {
//            if (err == LFS_ERR_CORRUPT) {
//                continue;
//            }
//            return err;
//        }
//
//        if (valid && lfs_scmp(test.rev, dir->d.rev) < 0) {
//            continue;
//        }
//
//        if ((0x7fffffff & test.size) < sizeof(test)+4 ||
//            (0x7fffffff & test.size) > lfs->cfg->block_size) {
//            continue;
//        }
//
//        uint32_t crc = 0xffffffff;
//        lfs1_dir_tole32(&test);
//        lfs1_crc(&crc, &test, sizeof(test));
//        lfs1_dir_fromle32(&test);
//        err = lfs1_bd_crc(lfs, tpair[i], sizeof(test),
//                (0x7fffffff & test.size) - sizeof(test), &crc);
//        if (err) {
//            if (err == LFS_ERR_CORRUPT) {
//                continue;
//            }
//            return err;
//        }
//
//        if (crc != 0) {
//            continue;
//        }
//
//        valid = true;
//
//        // setup dir in case it's valid
//        dir->pair[0] = tpair[(i+0) % 2];
//        dir->pair[1] = tpair[(i+1) % 2];
//        dir->off = sizeof(dir->d);
//        dir->d = test;
//    }
//
//    if (!valid) {
//        LFS_ERROR("Corrupted dir pair at {0x%"PRIx32", 0x%"PRIx32"}",
//                tpair[0], tpair[1]);
//        return LFS_ERR_CORRUPT;
//    }
//
//    return 0;
//}
//
//static int lfs1_dir_next(lfs_t *lfs, lfs1_dir_t *dir, lfs1_entry_t *entry) {
//    while (dir->off + sizeof(entry->d) > (0x7fffffff & dir->d.size)-4) {
//        if (!(0x80000000 & dir->d.size)) {
//            entry->off = dir->off;
//            return LFS_ERR_NOENT;
//        }
//
//        int err = lfs1_dir_fetch(lfs, dir, dir->d.tail);
//        if (err) {
//            return err;
//        }
//
//        dir->off = sizeof(dir->d);
//        dir->pos += sizeof(dir->d) + 4;
//    }
//
//    int err = lfs1_bd_read(lfs, dir->pair[0], dir->off,
//            &entry->d, sizeof(entry->d));
//    lfs1_entry_fromle32(&entry->d);
//    if (err) {
//        return err;
//    }
//
//    entry->off = dir->off;
//    dir->off += lfs1_entry_size(entry);
//    dir->pos += lfs1_entry_size(entry);
//    return 0;
//}
//
///// littlefs v1 specific operations ///
//int lfs1_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data) {
//    if (lfs_pair_isnull(lfs->lfs1->root)) {
//        return 0;
//    }
//
//    // iterate over metadata pairs
//    lfs1_dir_t dir;
//    lfs1_entry_t entry;
//    lfs_block_t cwd[2] = {0, 1};
//
//    while (true) {
//        for (int i = 0; i < 2; i++) {
//            int err = cb(data, cwd[i]);
//            if (err) {
//                return err;
//            }
//        }
//
//        int err = lfs1_dir_fetch(lfs, &dir, cwd);
//        if (err) {
//            return err;
//        }
//
//        // iterate over contents
//        while (dir.off + sizeof(entry.d) <= (0x7fffffff & dir.d.size)-4) {
//            err = lfs1_bd_read(lfs, dir.pair[0], dir.off,
//                    &entry.d, sizeof(entry.d));
//            lfs1_entry_fromle32(&entry.d);
//            if (err) {
//                return err;
//            }
//
//            dir.off += lfs1_entry_size(&entry);
//            if ((0x70 & entry.d.type) == (0x70 & LFS1_TYPE_REG)) {
//                err = lfs_ctz_traverse(lfs, NULL, &lfs->rcache,
//                        entry.d.u.file.head, entry.d.u.file.size, cb, data);
//                if (err) {
//                    return err;
//                }
//            }
//        }
//
//        // we also need to check if we contain a threaded v2 directory
//        lfs_mdir_t dir2 = {.split=true, .tail={cwd[0], cwd[1]}};
//        while (dir2.split) {
//            err = lfs_dir_fetch(lfs, &dir2, dir2.tail);
//            if (err) {
//                break;
//            }
//
//            for (int i = 0; i < 2; i++) {
//                err = cb(data, dir2.pair[i]);
//                if (err) {
//                    return err;
//                }
//            }
//        }
//
//        cwd[0] = dir.d.tail[0];
//        cwd[1] = dir.d.tail[1];
//
//        if (lfs_pair_isnull(cwd)) {
//            break;
//        }
//    }
//
//    return 0;
//}
//
//static int lfs1_moved(lfs_t *lfs, const void *e) {
//    if (lfs_pair_isnull(lfs->lfs1->root)) {
//        return 0;
//    }
//
//    // skip superblock
//    lfs1_dir_t cwd;
//    int err = lfs1_dir_fetch(lfs, &cwd, (const lfs_block_t[2]){0, 1});
//    if (err) {
//        return err;
//    }
//
//    // iterate over all directory directory entries
//    lfs1_entry_t entry;
//    while (!lfs_pair_isnull(cwd.d.tail)) {
//        err = lfs1_dir_fetch(lfs, &cwd, cwd.d.tail);
//        if (err) {
//            return err;
//        }
//
//        while (true) {
//            err = lfs1_dir_next(lfs, &cwd, &entry);
//            if (err && err != LFS_ERR_NOENT) {
//                return err;
//            }
//
//            if (err == LFS_ERR_NOENT) {
//                break;
//            }
//
//            if (!(0x80 & entry.d.type) &&
//                 memcmp(&entry.d.u, e, sizeof(entry.d.u)) == 0) {
//                return true;
//            }
//        }
//    }
//
//    return false;
//}
//
///// Filesystem operations ///
//static int lfs1_mount(lfs_t *lfs, struct lfs1 *lfs1,
//        const struct lfs_config *cfg) {
//    int err = 0;
//    {
//        err = lfs_init(lfs, cfg);
//        if (err) {
//            return err;
//        }
//
//        lfs->lfs1 = lfs1;
//        lfs->lfs1->root[0] = LFS_BLOCK_NULL;
//        lfs->lfs1->root[1] = LFS_BLOCK_NULL;
//
//        // setup free lookahead
//        lfs->free.off = 0;
//        lfs->free.size = 0;
//        lfs->free.i = 0;
//        lfs_alloc_ack(lfs);
//
//        // load superblock
//        lfs1_dir_t dir;
//        lfs1_superblock_t superblock;
//        err = lfs1_dir_fetch(lfs, &dir, (const lfs_block_t[2]){0, 1});
//        if (err && err != LFS_ERR_CORRUPT) {
//            goto cleanup;
//        }
//
//        if (!err) {
//            err = lfs1_bd_read(lfs, dir.pair[0], sizeof(dir.d),
//                    &superblock.d, sizeof(superblock.d));
//            lfs1_superblock_fromle32(&superblock.d);
//            if (err) {
//                goto cleanup;
//            }
//
//            lfs->lfs1->root[0] = superblock.d.root[0];
//            lfs->lfs1->root[1] = superblock.d.root[1];
//        }
//
//        if (err || memcmp(superblock.d.magic, "littlefs", 8) != 0) {
//            LFS_ERROR("Invalid superblock at {0x%"PRIx32", 0x%"PRIx32"}",
//                    0, 1);
//            err = LFS_ERR_CORRUPT;
//            goto cleanup;
//        }
//
//        uint16_t major_version = (0xffff & (superblock.d.version >> 16));
//        uint16_t minor_version = (0xffff & (superblock.d.version >>  0));
//        if ((major_version != LFS1_DISK_VERSION_MAJOR ||
//             minor_version > LFS1_DISK_VERSION_MINOR)) {
//            LFS_ERROR("Invalid version v%d.%d", major_version, minor_version);
//            err = LFS_ERR_INVAL;
//            goto cleanup;
//        }
//
//        return 0;
//    }
//
//cleanup:
//    lfs_deinit(lfs);
//    return err;
//}
//
//static int lfs1_unmount(lfs_t *lfs) {
//    return lfs_deinit(lfs);
//}
//
///// v1 migration ///
//static int lfs_rawmigrate(lfs_t *lfs, const struct lfs_config *cfg) {
//    struct lfs1 lfs1;
//    int err = lfs1_mount(lfs, &lfs1, cfg);
//    if (err) {
//        return err;
//    }
//
//    {
//        // iterate through each directory, copying over entries
//        // into new directory
//        lfs1_dir_t dir1;
//        lfs_mdir_t dir2;
//        dir1.d.tail[0] = lfs->lfs1->root[0];
//        dir1.d.tail[1] = lfs->lfs1->root[1];
//        while (!lfs_pair_isnull(dir1.d.tail)) {
//            // iterate old dir
//            err = lfs1_dir_fetch(lfs, &dir1, dir1.d.tail);
//            if (err) {
//                goto cleanup;
//            }
//
//            // create new dir and bind as temporary pretend root
//            err = lfs_dir_alloc(lfs, &dir2);
//            if (err) {
//                goto cleanup;
//            }
//
//            dir2.rev = dir1.d.rev;
//            dir1.head[0] = dir1.pair[0];
//            dir1.head[1] = dir1.pair[1];
//            lfs->root[0] = dir2.pair[0];
//            lfs->root[1] = dir2.pair[1];
//
//            err = lfs_dir_commit(lfs, &dir2, NULL, 0);
//            if (err) {
//                goto cleanup;
//            }
//
//            while (true) {
//                lfs1_entry_t entry1;
//                err = lfs1_dir_next(lfs, &dir1, &entry1);
//                if (err && err != LFS_ERR_NOENT) {
//                    goto cleanup;
//                }
//
//                if (err == LFS_ERR_NOENT) {
//                    break;
//                }
//
//                // check that entry has not been moved
//                if (entry1.d.type & 0x80) {
//                    int moved = lfs1_moved(lfs, &entry1.d.u);
//                    if (moved < 0) {
//                        err = moved;
//                        goto cleanup;
//                    }
//
//                    if (moved) {
//                        continue;
//                    }
//
//                    entry1.d.type &= ~0x80;
//                }
//
//                // also fetch name
//                char name[LFS_NAME_MAX+1];
//                memset(name, 0, sizeof(name));
//                err = lfs1_bd_read(lfs, dir1.pair[0],
//                        entry1.off + 4+entry1.d.elen+entry1.d.alen,
//                        name, entry1.d.nlen);
//                if (err) {
//                    goto cleanup;
//                }
//
//                bool isdir = (entry1.d.type == LFS1_TYPE_DIR);
//
//                // create entry in new dir
//                err = lfs_dir_fetch(lfs, &dir2, lfs->root);
//                if (err) {
//                    goto cleanup;
//                }
//
//                uint16_t id;
//                err = lfs_dir_find(lfs, &dir2, &(const char*){name}, &id);
//                if (!(err == LFS_ERR_NOENT && id != 0x3ff)) {
//                    err = (err < 0) ? err : LFS_ERR_EXIST;
//                    goto cleanup;
//                }
//
//                lfs1_entry_tole32(&entry1.d);
//                err = lfs_dir_commit(lfs, &dir2, LFS_MKATTRS(
//                        {LFS_MKTAG(LFS_TYPE_CREATE, id, 0), NULL},
//                        {LFS_MKTAG_IF_ELSE(isdir,
//                            LFS_TYPE_DIR, id, entry1.d.nlen,
//                            LFS_TYPE_REG, id, entry1.d.nlen),
//                                name},
//                        {LFS_MKTAG_IF_ELSE(isdir,
//                            LFS_TYPE_DIRSTRUCT, id, sizeof(entry1.d.u),
//                            LFS_TYPE_CTZSTRUCT, id, sizeof(entry1.d.u)),
//                                &entry1.d.u}));
//                lfs1_entry_fromle32(&entry1.d);
//                if (err) {
//                    goto cleanup;
//                }
//            }
//
//            if (!lfs_pair_isnull(dir1.d.tail)) {
//                // find last block and update tail to thread into fs
//                err = lfs_dir_fetch(lfs, &dir2, lfs->root);
//                if (err) {
//                    goto cleanup;
//                }
//
//                while (dir2.split) {
//                    err = lfs_dir_fetch(lfs, &dir2, dir2.tail);
//                    if (err) {
//                        goto cleanup;
//                    }
//                }
//
//                lfs_pair_tole32(dir2.pair);
//                err = lfs_dir_commit(lfs, &dir2, LFS_MKATTRS(
//                        {LFS_MKTAG(LFS_TYPE_SOFTTAIL, 0x3ff, 8), dir1.d.tail}));
//                lfs_pair_fromle32(dir2.pair);
//                if (err) {
//                    goto cleanup;
//                }
//            }
//
//            // Copy over first block to thread into fs. Unfortunately
//            // if this fails there is not much we can do.
//            LFS_DEBUG("Migrating {0x%"PRIx32", 0x%"PRIx32"} "
//                        "-> {0x%"PRIx32", 0x%"PRIx32"}",
//                    lfs->root[0], lfs->root[1], dir1.head[0], dir1.head[1]);
//
//            err = lfs_bd_erase(lfs, dir1.head[1]);
//            if (err) {
//                goto cleanup;
//            }
//
//            err = lfs_dir_fetch(lfs, &dir2, lfs->root);
//            if (err) {
//                goto cleanup;
//            }
//
//            for (lfs_off_t i = 0; i < dir2.off; i++) {
//                uint8_t dat;
//                err = lfs_bd_read(lfs,
//                        NULL, &lfs->rcache, dir2.off,
//                        dir2.pair[0], i, &dat, 1);
//                if (err) {
//                    goto cleanup;
//                }
//
//                err = lfs_bd_prog(lfs,
//                        &lfs->pcache, &lfs->rcache, true,
//                        dir1.head[1], i, &dat, 1);
//                if (err) {
//                    goto cleanup;
//                }
//            }
//
//            err = lfs_bd_flush(lfs, &lfs->pcache, &lfs->rcache, true);
//            if (err) {
//                goto cleanup;
//            }
//        }
//
//        // Create new superblock. This marks a successful migration!
//        err = lfs1_dir_fetch(lfs, &dir1, (const lfs_block_t[2]){0, 1});
//        if (err) {
//            goto cleanup;
//        }
//
//        dir2.pair[0] = dir1.pair[0];
//        dir2.pair[1] = dir1.pair[1];
//        dir2.rev = dir1.d.rev;
//        dir2.off = sizeof(dir2.rev);
//        dir2.etag = 0xffffffff;
//        dir2.count = 0;
//        dir2.tail[0] = lfs->lfs1->root[0];
//        dir2.tail[1] = lfs->lfs1->root[1];
//        dir2.erased = false;
//        dir2.split = true;
//
//        lfs_superblock_t superblock = {
//            .version     = LFS_DISK_VERSION,
//            .block_size  = lfs->cfg->block_size,
//            .block_count = lfs->cfg->block_count,
//            .name_max    = lfs->name_max,
//            .file_max    = lfs->file_max,
//            .attr_max    = lfs->attr_max,
//        };
//
//        lfs_superblock_tole32(&superblock);
//        err = lfs_dir_commit(lfs, &dir2, LFS_MKATTRS(
//                {LFS_MKTAG(LFS_TYPE_CREATE, 0, 0), NULL},
//                {LFS_MKTAG(LFS_TYPE_SUPERBLOCK, 0, 8), "littlefs"},
//                {LFS_MKTAG(LFS_TYPE_INLINESTRUCT, 0, sizeof(superblock)),
//                    &superblock}));
//        if (err) {
//            goto cleanup;
//        }
//
//        // sanity check that fetch works
//        err = lfs_dir_fetch(lfs, &dir2, (const lfs_block_t[2]){0, 1});
//        if (err) {
//            goto cleanup;
//        }
//
//        // force compaction to prevent accidentally mounting v1
//        dir2.erased = false;
//        err = lfs_dir_commit(lfs, &dir2, NULL, 0);
//        if (err) {
//            goto cleanup;
//        }
//    }
//
//cleanup:
//    lfs1_unmount(lfs);
//    return err;
//}
//
//#endif
//
//
///// Public API wrappers ///
//
//// Here we can add tracing/thread safety easily
//
//// Thread-safe wrappers if enabled
//#ifdef LFS_THREADSAFE
//#define LFS_LOCK(cfg)   cfg->lock(cfg)
//#define LFS_UNLOCK(cfg) cfg->unlock(cfg)
//#else
//#define LFS_LOCK(cfg)   ((void)cfg, 0)
//#define LFS_UNLOCK(cfg) ((void)cfg)
//#endif
//
//// Public API
//#ifndef LFS_READONLY
//int lfs_format(lfs_t *lfs, const struct lfs_config *cfg) {
//    int err = LFS_LOCK(cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_format(%p, %p {.context=%p, "
//                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
//                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
//                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
//                ".block_cycles=%"PRIu32", .cache_size=%"PRIu32", "
//                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
//                ".prog_buffer=%p, .lookahead_buffer=%p, "
//                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
//                ".attr_max=%"PRIu32"})",
//            (void*)lfs, (void*)cfg, cfg->context,
//            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
//            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
//            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
//            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
//            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
//            cfg->name_max, cfg->file_max, cfg->attr_max);
//
//    err = lfs_rawformat(lfs, cfg);
//
//    LFS_TRACE("lfs_format -> %d", err);
//    LFS_UNLOCK(cfg);
//    return err;
//}
//#endif
//
//int lfs_mount(lfs_t *lfs, const struct lfs_config *cfg) {
//    int err = LFS_LOCK(cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_mount(%p, %p {.context=%p, "
//                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
//                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
//                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
//                ".block_cycles=%"PRIu32", .cache_size=%"PRIu32", "
//                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
//                ".prog_buffer=%p, .lookahead_buffer=%p, "
//                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
//                ".attr_max=%"PRIu32"})",
//            (void*)lfs, (void*)cfg, cfg->context,
//            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
//            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
//            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
//            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
//            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
//            cfg->name_max, cfg->file_max, cfg->attr_max);
//
//    err = lfs_rawmount(lfs, cfg);
//
//    LFS_TRACE("lfs_mount -> %d", err);
//    LFS_UNLOCK(cfg);
//    return err;
//}
//
//int lfs_unmount(lfs_t *lfs) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_unmount(%p)", (void*)lfs);
//
//    err = lfs_rawunmount(lfs);
//
//    LFS_TRACE("lfs_unmount -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//#ifndef LFS_READONLY
//int lfs_remove(lfs_t *lfs, const char *path) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_remove(%p, \"%s\")", (void*)lfs, path);
//
//    err = lfs_rawremove(lfs, path);
//
//    LFS_TRACE("lfs_remove -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//#ifndef LFS_READONLY
//int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_rename(%p, \"%s\", \"%s\")", (void*)lfs, oldpath, newpath);
//
//    err = lfs_rawrename(lfs, oldpath, newpath);
//
//    LFS_TRACE("lfs_rename -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_stat(%p, \"%s\", %p)", (void*)lfs, path, (void*)info);
//
//    err = lfs_rawstat(lfs, path, info);
//
//    LFS_TRACE("lfs_stat -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//lfs_ssize_t lfs_getattr(lfs_t *lfs, const char *path,
//        uint8_t type, void *buffer, lfs_size_t size) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_getattr(%p, \"%s\", %"PRIu8", %p, %"PRIu32")",
//            (void*)lfs, path, type, buffer, size);
//
//    lfs_ssize_t res = lfs_rawgetattr(lfs, path, type, buffer, size);
//
//    LFS_TRACE("lfs_getattr -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//
//#ifndef LFS_READONLY
//int lfs_setattr(lfs_t *lfs, const char *path,
//        uint8_t type, const void *buffer, lfs_size_t size) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_setattr(%p, \"%s\", %"PRIu8", %p, %"PRIu32")",
//            (void*)lfs, path, type, buffer, size);
//
//    err = lfs_rawsetattr(lfs, path, type, buffer, size);
//
//    LFS_TRACE("lfs_setattr -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//#ifndef LFS_READONLY
//int lfs_removeattr(lfs_t *lfs, const char *path, uint8_t type) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_removeattr(%p, \"%s\", %"PRIu8")", (void*)lfs, path, type);
//
//    err = lfs_rawremoveattr(lfs, path, type);
//
//    LFS_TRACE("lfs_removeattr -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//#ifndef LFS_NO_MALLOC
//int lfs_file_open(lfs_t *lfs, lfs_file_t *file, const char *path, int flags) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_open(%p, %p, \"%s\", %x)",
//            (void*)lfs, (void*)file, path, flags);
//    LFS_ASSERT(!lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    err = lfs_file_rawopen(lfs, file, path, flags);
//
//    LFS_TRACE("lfs_file_open -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//int lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file,
//        const char *path, int flags,
//        const struct lfs_file_config *cfg) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_opencfg(%p, %p, \"%s\", %x, %p {"
//                 ".buffer=%p, .attrs=%p, .attr_count=%"PRIu32"})",
//            (void*)lfs, (void*)file, path, flags,
//            (void*)cfg, cfg->buffer, (void*)cfg->attrs, cfg->attr_count);
//    LFS_ASSERT(!lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    err = lfs_file_rawopencfg(lfs, file, path, flags, cfg);
//
//    LFS_TRACE("lfs_file_opencfg -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//int lfs_file_close(lfs_t *lfs, lfs_file_t *file) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_close(%p, %p)", (void*)lfs, (void*)file);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    err = lfs_file_rawclose(lfs, file);
//
//    LFS_TRACE("lfs_file_close -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//#ifndef LFS_READONLY
//int lfs_file_sync(lfs_t *lfs, lfs_file_t *file) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_sync(%p, %p)", (void*)lfs, (void*)file);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    err = lfs_file_rawsync(lfs, file);
//
//    LFS_TRACE("lfs_file_sync -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
//        void *buffer, lfs_size_t size) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_read(%p, %p, %p, %"PRIu32")",
//            (void*)lfs, (void*)file, buffer, size);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    lfs_ssize_t res = lfs_file_rawread(lfs, file, buffer, size);
//
//    LFS_TRACE("lfs_file_read -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//
//#ifndef LFS_READONLY
//lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
//        const void *buffer, lfs_size_t size) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_write(%p, %p, %p, %"PRIu32")",
//            (void*)lfs, (void*)file, buffer, size);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    lfs_ssize_t res = lfs_file_rawwrite(lfs, file, buffer, size);
//
//    LFS_TRACE("lfs_file_write -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//#endif
//
//lfs_soff_t lfs_file_seek(lfs_t *lfs, lfs_file_t *file,
//        lfs_soff_t off, int whence) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_seek(%p, %p, %"PRId32", %d)",
//            (void*)lfs, (void*)file, off, whence);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    lfs_soff_t res = lfs_file_rawseek(lfs, file, off, whence);
//
//    LFS_TRACE("lfs_file_seek -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//
//#ifndef LFS_READONLY
//int lfs_file_truncate(lfs_t *lfs, lfs_file_t *file, lfs_off_t size) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_truncate(%p, %p, %"PRIu32")",
//            (void*)lfs, (void*)file, size);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    err = lfs_file_rawtruncate(lfs, file, size);
//
//    LFS_TRACE("lfs_file_truncate -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_tell(%p, %p)", (void*)lfs, (void*)file);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    lfs_soff_t res = lfs_file_rawtell(lfs, file);
//
//    LFS_TRACE("lfs_file_tell -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//
//int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_rewind(%p, %p)", (void*)lfs, (void*)file);
//
//    err = lfs_file_rawrewind(lfs, file);
//
//    LFS_TRACE("lfs_file_rewind -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_file_size(%p, %p)", (void*)lfs, (void*)file);
//    LFS_ASSERT(lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)file));
//
//    lfs_soff_t res = lfs_file_rawsize(lfs, file);
//
//    LFS_TRACE("lfs_file_size -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//
//#ifndef LFS_READONLY
//int lfs_mkdir(lfs_t *lfs, const char *path) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_mkdir(%p, \"%s\")", (void*)lfs, path);
//
//    err = lfs_rawmkdir(lfs, path);
//
//    LFS_TRACE("lfs_mkdir -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//#endif
//
//int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_dir_open(%p, %p, \"%s\")", (void*)lfs, (void*)dir, path);
//    LFS_ASSERT(!lfs_mlist_isopen(lfs->mlist, (struct lfs_mlist*)dir));
//
//    err = lfs_dir_rawopen(lfs, dir, path);
//
//    LFS_TRACE("lfs_dir_open -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_dir_close(%p, %p)", (void*)lfs, (void*)dir);
//
//    err = lfs_dir_rawclose(lfs, dir);
//
//    LFS_TRACE("lfs_dir_close -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_dir_read(%p, %p, %p)",
//            (void*)lfs, (void*)dir, (void*)info);
//
//    err = lfs_dir_rawread(lfs, dir, info);
//
//    LFS_TRACE("lfs_dir_read -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_dir_seek(%p, %p, %"PRIu32")",
//            (void*)lfs, (void*)dir, off);
//
//    err = lfs_dir_rawseek(lfs, dir, off);
//
//    LFS_TRACE("lfs_dir_seek -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_dir_tell(%p, %p)", (void*)lfs, (void*)dir);
//
//    lfs_soff_t res = lfs_dir_rawtell(lfs, dir);
//
//    LFS_TRACE("lfs_dir_tell -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//
//int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_dir_rewind(%p, %p)", (void*)lfs, (void*)dir);
//
//    err = lfs_dir_rawrewind(lfs, dir);
//
//    LFS_TRACE("lfs_dir_rewind -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//lfs_ssize_t lfs_fs_size(lfs_t *lfs) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_fs_size(%p)", (void*)lfs);
//
//    lfs_ssize_t res = lfs_fs_rawsize(lfs);
//
//    LFS_TRACE("lfs_fs_size -> %"PRId32, res);
//    LFS_UNLOCK(lfs->cfg);
//    return res;
//}
//
//int lfs_fs_traverse(lfs_t *lfs, int (*cb)(void *, lfs_block_t), void *data) {
//    int err = LFS_LOCK(lfs->cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_fs_traverse(%p, %p, %p)",
//            (void*)lfs, (void*)(uintptr_t)cb, data);
//
//    err = lfs_fs_rawtraverse(lfs, cb, data, true);
//
//    LFS_TRACE("lfs_fs_traverse -> %d", err);
//    LFS_UNLOCK(lfs->cfg);
//    return err;
//}
//
//#ifdef LFS_MIGRATE
//int lfs_migrate(lfs_t *lfs, const struct lfs_config *cfg) {
//    int err = LFS_LOCK(cfg);
//    if (err) {
//        return err;
//    }
//    LFS_TRACE("lfs_migrate(%p, %p {.context=%p, "
//                ".read=%p, .prog=%p, .erase=%p, .sync=%p, "
//                ".read_size=%"PRIu32", .prog_size=%"PRIu32", "
//                ".block_size=%"PRIu32", .block_count=%"PRIu32", "
//                ".block_cycles=%"PRIu32", .cache_size=%"PRIu32", "
//                ".lookahead_size=%"PRIu32", .read_buffer=%p, "
//                ".prog_buffer=%p, .lookahead_buffer=%p, "
//                ".name_max=%"PRIu32", .file_max=%"PRIu32", "
//                ".attr_max=%"PRIu32"})",
//            (void*)lfs, (void*)cfg, cfg->context,
//            (void*)(uintptr_t)cfg->read, (void*)(uintptr_t)cfg->prog,
//            (void*)(uintptr_t)cfg->erase, (void*)(uintptr_t)cfg->sync,
//            cfg->read_size, cfg->prog_size, cfg->block_size, cfg->block_count,
//            cfg->block_cycles, cfg->cache_size, cfg->lookahead_size,
//            cfg->read_buffer, cfg->prog_buffer, cfg->lookahead_buffer,
//            cfg->name_max, cfg->file_max, cfg->attr_max);
//
//    err = lfs_rawmigrate(lfs, cfg);
//
//    LFS_TRACE("lfs_migrate -> %d", err);
//    LFS_UNLOCK(cfg);
//    return err;
//}
//#endif

