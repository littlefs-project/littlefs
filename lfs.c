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

// this is just a hint that the function returns a bool + err union
typedef int lfs_sbool_t;


/// Simple bd wrappers (asserts go here) ///

static int lfsr_bd_read__(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        void *buffer, lfs_size_t size) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);
    // must be aligned
    LFS_ASSERT(off % lfs->cfg->read_size == 0);
    LFS_ASSERT(size % lfs->cfg->read_size == 0);

    // bd read
    int err = lfs->cfg->read(lfs->cfg, block, off, buffer, size);
    LFS_ASSERT(err <= 0);
    if (err) {
        LFS_INFO("Bad read 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                block, off, size, err);
        return err;
    }

    return 0;
}

static int lfsr_bd_prog__(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);
    // must be aligned
    LFS_ASSERT(off % lfs->cfg->prog_size == 0);
    LFS_ASSERT(size % lfs->cfg->prog_size == 0);

    // bd prog
    int err = lfs->cfg->prog(lfs->cfg, block, off, buffer, size);
    LFS_ASSERT(err <= 0);
    if (err) {
        LFS_INFO("Bad prog 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                block, off, size, err);
        return err;
    }

    return 0;
}

static int lfsr_bd_erase__(lfs_t *lfs, lfs_block_t block) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);

    // bd erase
    int err = lfs->cfg->erase(lfs->cfg, block);
    LFS_ASSERT(err <= 0);
    if (err) {
        LFS_INFO("Bad erase 0x%"PRIx32" (%d)",
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
        LFS_INFO("Bad sync (%d)", err);
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
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

    lfs_size_t hint_ = lfs_max(hint, size); // make sure hint >= size
    while (true) {
        lfs_size_t d = hint_;

        // already in pcache?
        if (block == lfs->pcache.block
                && off < lfs->pcache.off + lfs->pcache.size) {
            if (off >= lfs->pcache.off) {
                *buffer_ = &lfs->pcache.buffer[off-lfs->pcache.off];
                *size_ = lfs_min(
                        lfs_min(d, size),
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
                    lfs_min(d, size),
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
                        d,
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
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

    lfs_size_t off_ = off;
    lfs_size_t hint_ = lfs_max(hint, size); // make sure hint >= size
    uint8_t *buffer_ = buffer;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        lfs_size_t d = hint_;

        // already in pcache?
        if (block == lfs->pcache.block
                && off_ < lfs->pcache.off + lfs->pcache.size) {
            if (off_ >= lfs->pcache.off) {
                d = lfs_min(
                        lfs_min(d, size_),
                        lfs->pcache.size - (off_-lfs->pcache.off));
                lfs_memcpy(buffer_,
                        &lfs->pcache.buffer[off_-lfs->pcache.off],
                        d);

                off_ += d;
                hint_ -= d;
                buffer_ += d;
                size_ -= d;
                continue;
            }

            // pcache takes priority
            d = lfs_min(d, lfs->pcache.off - off_);
        }

        // already in rcache?
        if (block == lfs->rcache.block
                && off_ < lfs->rcache.off + lfs->rcache.size) {
            if (off_ >= lfs->rcache.off) {
                d = lfs_min(
                        lfs_min(d, size_),
                        lfs->rcache.size - (off_-lfs->rcache.off));
                lfs_memcpy(buffer_,
                        &lfs->rcache.buffer[off_-lfs->rcache.off],
                        d);

                off_ += d;
                hint_ -= d;
                buffer_ += d;
                size_ -= d;
                continue;
            }

            // rcache takes priority
            d = lfs_min(d, lfs->rcache.off - off_);
        }

        // bypass rcache?
        if (off_ % lfs->cfg->read_size == 0
                && lfs_min(d, size_) >= lfs_min(hint_, lfs->cfg->rcache_size)
                && lfs_min(d, size_) >= lfs->cfg->read_size) {
            d = lfs_aligndown(size_, lfs->cfg->read_size);
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

        // drop rcache in case read fails
        lfsr_bd_droprcache(lfs);

        // load into rcache, above conditions can no longer fail
        //
        // note it's ok if we overlap the pcache a bit, pcache always
        // takes priority until flush, which updates the rcache
        lfs_size_t off__ = lfs_aligndown(off_, lfs->cfg->read_size);
        lfs_size_t size__ = lfs_alignup(
                lfs_min(
                    // watch out for overflow when hint_=-1!
                    (off_-off__) + lfs_min(
                        lfs_min(hint_, d),
                        lfs->cfg->block_size - off_),
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

    return 0;
}

// needed in lfsr_bd_prog_ for prog validation
#ifdef LFS_CKPROGS
static inline bool lfsr_m_isckprogs(uint32_t flags);
#endif
static lfs_scmp_t lfsr_bd_cmp(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        const void *buffer, lfs_size_t size);

// low-level prog stuff
static int lfsr_bd_prog_(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

    // prog to disk
    int err = lfsr_bd_prog__(lfs, block, off, buffer, size);
    if (err) {
        return err;
    }

    #ifdef LFS_CKPROGS
    // checking progs?
    if (lfsr_m_isckprogs(lfs->flags)) {
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
            LFS_WARN("Found ckprog mismatch 0x%"PRIx32".%"PRIx32" %"PRId32,
                    block, off, size);
            return LFS_ERR_CORRUPT;
        }
    }
    #endif

    // update rcache if we can
    if (block == lfs->rcache.block
            && off <= lfs->rcache.off + lfs->rcache.size) {
        lfs->rcache.off = lfs_min(off, lfs->rcache.off);
        lfs->rcache.size = lfs_min(
                (off-lfs->rcache.off) + size,
                lfs->cfg->rcache_size);
        lfs_memcpy(&lfs->rcache.buffer[off-lfs->rcache.off],
                buffer,
                lfs->rcache.size - (off-lfs->rcache.off));
    }

    // optional aligned checksum
    if (cksum && align) {
        *cksum = lfs_crc32c(*cksum, buffer, size);
    }

    return 0;
}

// flush the pcache
static int lfsr_bd_flush(lfs_t *lfs, uint32_t *cksum, bool align) {
    if (lfs->pcache.size != 0) {
        // must be in-bounds
        LFS_ASSERT(lfs->pcache.block < lfs->block_count);
        // must be aligned
        LFS_ASSERT(lfs->pcache.off % lfs->cfg->prog_size == 0);
        lfs_size_t size = lfs_alignup(lfs->pcache.size, lfs->cfg->prog_size);

        // make this cache available, if we error anything in this cache
        // would be useless anyways
        lfsr_bd_droppcache(lfs);

        // flush
        int err = lfsr_bd_prog_(lfs, lfs->pcache.block,
                lfs->pcache.off, lfs->pcache.buffer, size,
                cksum, align);
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
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

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
            int err = lfsr_bd_flush(lfs, cksum, align);
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
    }
}

// caching prog
//
// with optional checksum
static int lfsr_bd_prog(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

    lfs_size_t off_ = off;
    const uint8_t *buffer_ = buffer;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        // active pcache?
        if (lfs->pcache.block == block
                && lfs->pcache.size != 0) {
            // fits in pcache?
            if (off_ < lfs->pcache.off + lfs->cfg->pcache_size) {
                // you can't prog backwards silly
                LFS_ASSERT(off_ >= lfs->pcache.off);

                // expand the pcache?
                lfs->pcache.size = lfs_min(
                        (off_-lfs->pcache.off) + size_,
                        lfs->cfg->pcache_size);

                lfs_size_t d = lfs_min(
                        size_,
                        lfs->pcache.size - (off_-lfs->pcache.off));
                lfs_memcpy(&lfs->pcache.buffer[off_-lfs->pcache.off],
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
            int err = lfsr_bd_flush(lfs, cksum, align);
            if (err) {
                return err;
            }
        }

        // bypass pcache?
        if (off_ % lfs->cfg->prog_size == 0
                && size_ >= lfs->cfg->pcache_size) {
            lfs_size_t d = lfs_aligndown(size_, lfs->cfg->prog_size);
            int err = lfsr_bd_prog_(lfs, block, off_, buffer_, d,
                    cksum, align);
            if (err) {
                return err;
            }

            off_ += d;
            buffer_ += d;
            size_ -= d;
            continue;
        }

        // move the pcache, above conditions can no longer fail
        lfs->pcache.block = block;
        lfs->pcache.off = lfs_aligndown(off_, lfs->cfg->prog_size);
        lfs->pcache.size = lfs_min(
                (off_-lfs->pcache.off) + size_,
                lfs->cfg->pcache_size);

        // zero to avoid any information leaks
        lfs_memset(lfs->pcache.buffer, 0xff, lfs->cfg->pcache_size);
    }

    // optional checksum
    if (cksum && !align) {
        *cksum = lfs_crc32c(*cksum, buffer, size);
    }

    return 0;
}

static int lfsr_bd_sync(lfs_t *lfs) {
    // make sure we flush any caches
    int err = lfsr_bd_flush(lfs, NULL, false);
    if (err) {
        return err;
    }

    return lfsr_bd_sync__(lfs);
}

static int lfsr_bd_erase(lfs_t *lfs, lfs_block_t block) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);

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
        uint32_t *cksum) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

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

        *cksum = lfs_crc32c(*cksum, buffer__, size__);

        off_ += size__;
        hint_ -= size__;
        size_ -= size__;
    }

    return 0;
}

static lfs_scmp_t lfsr_bd_cmp(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint, 
        const void *buffer, lfs_size_t size) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

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

        int cmp = lfs_memcmp(buffer__, buffer_, size__);
        if (cmp != 0) {
            return (cmp < 0) ? LFS_CMP_LT : LFS_CMP_GT;
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
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS_ASSERT(dst_block < lfs->block_count);
    LFS_ASSERT(dst_off+size <= lfs->cfg->block_size);
    LFS_ASSERT(src_block < lfs->block_count);
    LFS_ASSERT(src_off+size <= lfs->cfg->block_size);

    lfs_size_t dst_off_ = dst_off;
    lfs_size_t src_off_ = src_off;
    lfs_size_t hint_ = lfs_max(hint, size); // make sure hint >= size
    lfs_size_t size_ = size;
    while (size_ > 0) {
        // prefer the pcache here to avoid rcache conflicts with prog
        // validation, if we're lucky we might even be able to avoid
        // clobbering the rcache at all
        uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_prognext(lfs, dst_block, dst_off_, size_,
                &buffer__, &size__,
                cksum, align);
        if (err) {
            return err;
        }

        err = lfsr_bd_read(lfs, src_block, src_off_, hint_,
                buffer__, size__);
        if (err) {
            return err;
        }

        // optional checksum
        if (cksum && !align) {
            *cksum = lfs_crc32c(*cksum, buffer__, size__);
        }

        dst_off_ += size__;
        src_off_ += size__;
        hint_ -= size__;
        size_ -= size__;
    }

    return 0;
}

static int lfsr_bd_set(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        uint8_t c, lfs_size_t size,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(off+size <= lfs->cfg->block_size);

    lfs_size_t off_ = off;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        uint8_t *buffer__;
        lfs_size_t size__;
        int err = lfsr_bd_prognext(lfs, block, off_, size_,
                &buffer__, &size__,
                cksum, align);
        if (err) {
            return err;
        }

        lfs_memset(buffer__, c, size__);

        // optional checksum
        if (cksum && !align) {
            *cksum = lfs_crc32c(*cksum, buffer__, size__);
        }

        off_ += size__;
        size_ -= size__;
    }

    return 0;
}



// lfsr_ptail_t stuff
//
// ptail tracks the most recent trunk's parity so we can parity-check
// if it hasn't been written to disk yet

#ifdef LFS_CKPARITY
#define LFSR_PTAIL_PARITY 0x80000000
#endif

#ifdef LFS_CKPARITY
static inline bool lfsr_ptail_parity(const lfsr_ptail_t *ptail) {
    return ptail->off & LFSR_PTAIL_PARITY;
}
#endif

#ifdef LFS_CKPARITY
static inline lfs_size_t lfsr_ptail_off(const lfsr_ptail_t *ptail) {
    return ptail->off & ~LFSR_PTAIL_PARITY;
}
#endif


// checked read helpers

#ifdef LFS_CKDATACKSUMS
static int lfsr_bd_ckprefix(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfs_size_t cksize, uint32_t cksum,
        lfs_size_t *hint_,
        uint32_t *cksum__) {
    (void)cksum;
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(cksize <= lfs->cfg->block_size);

    // make sure hint includes our prefix/suffix
    lfs_size_t hint__ = lfs_max(
            // watch out for overflow when hint=-1!
            off + lfs_min(
                hint,
                lfs->cfg->block_size - off),
            cksize);

    // checksum any prefixed data
    int err = lfsr_bd_cksum(lfs,
            block, 0, hint__,
            off,
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

#ifdef LFS_CKDATACKSUMS
static int lfsr_bd_cksuffix(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfs_size_t cksize, uint32_t cksum,
        uint32_t cksum__) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(cksize <= lfs->cfg->block_size);

    // checksum any suffixed data
    int err = lfsr_bd_cksum(lfs,
            block, off, hint,
            cksize - off,
            &cksum__);
    if (err) {
        return err;
    }

    // do checksums match?
    if (cksum__ != cksum) {
        LFS_ERROR("Found ckdatacksums mismatch "
                    "0x%"PRIx32".%"PRIx32" %"PRId32", "
                    "cksum %08"PRIx32" (!= %08"PRIx32")",
                block, 0, cksize,
                cksum__, cksum);
        return LFS_ERR_CORRUPT;
    }

    return 0;
}
#endif


// checked read functions

#ifdef LFS_CKDATACKSUMS
// caching read with parity/checksum checks
//
// the main downside of checking reads is we need to read all data that
// contributes to the relevant parity/checksum, this may be
// significantly more than the data we actually end up using
//
static int lfsr_bd_readck(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        void *buffer, lfs_size_t size,
        lfs_size_t cksize, uint32_t cksum) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(cksize <= lfs->cfg->block_size);
    // read should fit in ck info
    LFS_ASSERT(off+size <= cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs_size_t hint_;
    int err = lfsr_bd_ckprefix(lfs, block, off, hint,
            cksize, cksum,
            &hint_,
            &cksum__);
    if (err) {
        return err;
    }

    // read and checksum the data we're interested in
    err = lfsr_bd_read(lfs,
            block, off, hint_,
            buffer, size);
    if (err) {
        return err;
    }

    cksum__ = lfs_crc32c(cksum__, buffer, size);

    // checksum any suffixed data and validate
    err = lfsr_bd_cksuffix(lfs, block, off+size, hint_-size,
            cksize, cksum,
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
// we'd also need to worry about early termination in lfsr_bd_cmp/cmpck

#ifdef LFS_CKDATACKSUMS
static lfs_scmp_t lfsr_bd_cmpck(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        const void *buffer, lfs_size_t size,
        lfs_size_t cksize, uint32_t cksum) {
    // must be in-bounds
    LFS_ASSERT(block < lfs->block_count);
    LFS_ASSERT(cksize <= lfs->cfg->block_size);
    // read should fit in ck info
    LFS_ASSERT(off+size <= cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs_size_t hint_;
    int err = lfsr_bd_ckprefix(lfs, block, off, hint,
            cksize, cksum,
            &hint_,
            &cksum__);
    if (err) {
        return err;
    }

    // compare the data while simultaneously updating the checksum
    lfs_size_t off_ = off;
    lfs_size_t hint__ = hint_ - off;
    const uint8_t *buffer_ = buffer;
    lfs_size_t size_ = size;
    int cmp = LFS_CMP_EQ;
    while (size_ > 0) {
        const uint8_t *buffer__;
        lfs_size_t size__;
        err = lfsr_bd_readnext(lfs, block, off_, hint__, size_,
                &buffer__, &size__);
        if (err) {
            return err;
        }

        cksum__ = lfs_crc32c(cksum__, buffer__, size__);

        if (cmp == LFS_CMP_EQ) {
            int cmp_ = lfs_memcmp(buffer__, buffer_, size__);
            if (cmp_ != 0) {
                cmp = (cmp_ < 0) ? LFS_CMP_LT : LFS_CMP_GT;
            }
        }

        off_ += size__;
        hint__ -= size__;
        buffer_ += size__;
        size_ -= size__;
    }

    // checksum any suffixed data and validate
    err = lfsr_bd_cksuffix(lfs, block, off+size, hint_-size,
            cksize, cksum,
            cksum__);
    if (err) {
        return err;
    }

    return cmp;
}
#endif

#ifdef LFS_CKDATACKSUMS
static int lfsr_bd_cpyck(lfs_t *lfs,
        lfs_block_t dst_block, lfs_size_t dst_off,
        lfs_block_t src_block, lfs_size_t src_off, lfs_size_t hint,
        lfs_size_t size,
        lfs_size_t src_cksize, uint32_t src_cksum,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS_ASSERT(dst_block < lfs->block_count);
    LFS_ASSERT(dst_off+size <= lfs->cfg->block_size);
    LFS_ASSERT(src_block < lfs->block_count);
    LFS_ASSERT(src_cksize <= lfs->cfg->block_size);
    // read should fit in ck info
    LFS_ASSERT(src_off+size <= src_cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs_size_t hint_;
    int err = lfsr_bd_ckprefix(lfs, src_block, src_off, hint,
            src_cksize, src_cksum,
            &hint_,
            &cksum__);
    if (err) {
        return err;
    }

    // copy the data while simultaneously updating our checksum
    lfs_size_t dst_off_ = dst_off;
    lfs_size_t src_off_ = src_off;
    lfs_size_t hint__ = hint_;
    lfs_size_t size_ = size;
    while (size_ > 0) {
        // prefer the pcache here to avoid rcache conflicts with prog
        // validation, if we're lucky we might even be able to avoid
        // clobbering the rcache at all
        uint8_t *buffer__;
        lfs_size_t size__;
        err = lfsr_bd_prognext(lfs, dst_block, dst_off_, size_,
                &buffer__, &size__,
                cksum, align);
        if (err) {
            return err;
        }

        err = lfsr_bd_read(lfs, src_block, src_off_, hint__,
                buffer__, size__);
        if (err) {
            return err;
        }

        // validating checksum
        cksum__ = lfs_crc32c(cksum__, buffer__, size__);

        // optional prog checksum
        if (cksum && !align) {
            *cksum = lfs_crc32c(*cksum, buffer__, size__);
        }

        dst_off_ += size__;
        src_off_ += size__;
        hint__ -= size__;
        size_ -= size__;
    }

    // checksum any suffixed data and validate
    err = lfsr_bd_cksuffix(lfs, src_block, src_off+size, hint_-size,
            src_cksize, src_cksum,
            cksum__);
    if (err) {
        return err;
    }

    return 0;
}
#endif





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

/// lfsr_tag_t stuff ///

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
    LFSR_TAG_STICKYNOTE     = 0x0205,

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
    LFSR_TAG_ATTR           = 0x0400,
    LFSR_TAG_UATTR          = 0x0400,
    LFSR_TAG_SATTR          = 0x0500,

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
    LFSR_TAG_P              = 0x0001,
    LFSR_TAG_NOTE           = 0x3100,
    LFSR_TAG_ECKSUM         = 0x3200,
    LFSR_TAG_GCKSUMDELTA    = 0x3300,

    // in-device only tags, these should never get written to disk
    LFSR_TAG_INTERNAL       = 0x0800,
    LFSR_TAG_RATTRS         = 0x0800,
    LFSR_TAG_SHRUBCOMMIT    = 0x0801,
    LFSR_TAG_MOVE           = 0x0802,
    LFSR_TAG_ATTRS          = 0x0803,

    // some in-device only tag modifiers
    LFSR_TAG_RM             = 0x8000,
    LFSR_TAG_GROW           = 0x4000,
    LFSR_TAG_MASK0          = 0x0000,
    LFSR_TAG_MASK2          = 0x1000,
    LFSR_TAG_MASK8          = 0x2000,
    LFSR_TAG_MASK12         = 0x3000,
};

// some other tag encodings with their own subfields
#define LFSR_TAG_ALT(c, d, key) \
    (LFSR_TAG_ALT \
        | (0x2000 & (c)) \
        | (0x1000 & (d)) \
        | (0x0fff & (lfsr_tag_t)(key)))

#define LFSR_TAG_ATTR(attr) \
    (LFSR_TAG_ATTR \
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

static inline lfsr_tag_t lfsr_tag_nonredund(lfsr_tag_t tag) {
    return tag & 0xfffc;
}

static inline lfsr_tag_t lfsr_tag_redund(lfsr_tag_t tag) {
    return tag & 0x0003;
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

static inline bool lfsr_tag_p(lfsr_tag_t tag) {
    return tag & LFSR_TAG_P;
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

static inline bool lfsr_tag_ismask0(lfsr_tag_t tag) {
    return ((tag >> 12) & 0x3) == 0;
}

static inline bool lfsr_tag_ismask2(lfsr_tag_t tag) {
    return ((tag >> 12) & 0x3) == 1;
}

static inline bool lfsr_tag_ismask8(lfsr_tag_t tag) {
    return ((tag >> 12) & 0x3) == 2;
}

static inline bool lfsr_tag_ismask12(lfsr_tag_t tag) {
    return ((tag >> 12) & 0x3) == 3;
}

static const uint16_t lfsr_tag_masktable[4] = {
    0x0fff, 0x0ffc, 0x0f00, 0x0000
};

static inline lfsr_tag_t lfsr_tag_mask(lfsr_tag_t tag) {
    return lfsr_tag_masktable[(tag >> 12) & 0x3];
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

static inline lfsr_tag_t lfsr_tag_isparallel(lfsr_tag_t a, lfsr_tag_t b) {
    return (a & LFSR_TAG_GT) == (b & LFSR_TAG_GT);
}

static inline bool lfsr_tag_follow(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_srid_t rid, lfsr_tag_t tag) {
    // null tags break the following logic for unreachable alts
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
            != lfsr_tag_follow(
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
            != lfsr_tag_follow2(
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

// needed in lfsr_bd_readtag
#ifdef LFS_CKPARITY
static inline bool lfsr_m_isckparity(uint32_t flags);
#endif

static lfs_ssize_t lfsr_bd_readtag(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfsr_tag_t *tag_, lfsr_rid_t *weight_, lfs_size_t *size_,
        uint32_t *cksum) {
    // read the largest possible tag size
    uint8_t tag_buf[LFSR_TAG_DSIZE];
    lfs_size_t tag_dsize = lfs_min(LFSR_TAG_DSIZE, lfs->cfg->block_size-off);
    if (tag_dsize < 4) {
        return LFS_ERR_CORRUPT;
    }

    int err = lfsr_bd_read(lfs, block, off, hint,
            tag_buf, tag_dsize);
    if (err < 0) {
        return err;
    }

    // check the valid bit?
    if (cksum) {
        // on-disk, the tag's valid bit must reflect the parity of the
        // preceding data
        //
        // fortunately crc32cs are parity-preserving, so this is the
        // same as the parity of the checksum
        if ((tag_buf[0] >> 7) != lfs_parity(*cksum)) {
            return LFS_ERR_CORRUPT;
        }
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

    // check our tag does not go out of bounds
    if (!lfsr_tag_isalt(tag) && off+d + size > lfs->cfg->block_size) {
        return LFS_ERR_CORRUPT;
    }

    #ifdef LFS_CKPARITY
    // check the parity if we're checking parity
    //
    // this requires reading all of the data as well, but with any luck
    // the data will stick around in the cache
    if (lfsr_m_isckparity(lfs->flags)
            // don't bother checking parity if we're already calculating
            // a checksum
            && !cksum) {
        // checksum the tag, including our valid bit
        uint32_t cksum_ = lfs_crc32c(0, tag_buf, d);

        // checksum the data, if we have any
        lfs_size_t hint_ = hint - lfs_min(d, hint);
        lfs_size_t d_ = d;
        if (!lfsr_tag_isalt(tag)) {
            err = lfsr_bd_cksum(lfs,
                    // make sure hint includes our pesky parity byte
                    block, off+d_, lfs_max(hint_, size+1),
                    size,
                    &cksum_);
            if (err) {
                return err;
            }

            hint_ -= lfs_min(size, hint_);
            d_ += size;
        }

        // pesky parity byte
        if (off+d_ > lfs->cfg->block_size-1) {
            return LFS_ERR_CORRUPT;
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
        if (block == lfs->ptail.block
                && off+d_ == lfsr_ptail_off(&lfs->ptail)) {
            parity = lfsr_ptail_parity(&lfs->ptail);

        // parity on disk?
        } else {
            uint8_t p;
            err = lfsr_bd_read(lfs, block, off+d_, hint_,
                    &p, 1);
            if (err) {
                return err;
            }

            parity = p >> 7;
        }

        // does parity match?
        if (lfs_parity(cksum_) != parity) {
            LFS_ERROR("Found ckparity mismatch "
                        "0x%"PRIx32".%"PRIx32" %"PRId32", "
                        "parity %01"PRIx32" (!= %01"PRIx32")",
                    block, off, d_,
                    lfs_parity(cksum_), parity);
            return LFS_ERR_CORRUPT;
        }
    }
    #endif

    // optional checksum
    if (cksum) {
        // exclude valid bit from checksum
        *cksum ^= tag_buf[0] & 0x00000080;
        // calculate checksum
        *cksum = lfs_crc32c(*cksum, tag_buf, d);
    }

    // save what we found, clearing the valid bit, we don't need it
    // anymore
    *tag_ = tag & 0x7fff;
    *weight_ = weight;
    *size_ = size;
    return d;
}

static lfs_ssize_t lfsr_bd_progtag(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, bool perturb,
        lfsr_tag_t tag, lfsr_rid_t weight, lfs_size_t size,
        uint32_t *cksum, bool align) {
    // we set the valid bit here
    LFS_ASSERT(!(tag & 0x8000));
    // bit 7 is reserved for future subtype extensions
    LFS_ASSERT(!(tag & 0x80));
    // weight should not exceed 31-bits
    LFS_ASSERT(weight <= 0x7fffffff);
    // size should not exceed 28-bits
    LFS_ASSERT(size <= 0x0fffffff);

    // set the valid bit to the parity of the current checksum, inverted
    // if the perturb bit is set, and exclude from the next checksum
    LFS_ASSERT(cksum);
    bool v = lfs_parity(*cksum) ^ perturb;
    tag |= (lfsr_tag_t)v << 15;
    *cksum ^= (uint32_t)v << 7;

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

    int err = lfsr_bd_prog(lfs, block, off, tag_buf, d,
            cksum, align);
    if (err < 0) {
        return err;
    }

    return d;
}


/// lfsr_data_t stuff ///

#define LFSR_DATA_ONDISK 0x80000000
#define LFSR_DATA_ISBPTR 0x40000000

#define LFSR_DATA_NULL() \
    ((lfsr_data_t){ \
        .size=0, \
        .u.buffer=NULL})

#define LFSR_DATA_BUF(_buffer, _size) \
    ((lfsr_data_t){ \
        .size=_size, \
        .u.buffer=(const void*)(_buffer)})

#define LFSR_DATA_DISK(_block, _off, _size) \
    ((lfsr_data_t){ \
        .size=LFSR_DATA_ONDISK | (_size), \
        .u.disk.block=_block, \
        .u.disk.off=_off})

// data helpers
static inline bool lfsr_data_ondisk(lfsr_data_t data) {
    return data.size & LFSR_DATA_ONDISK;
}

static inline bool lfsr_data_isbuf(lfsr_data_t data) {
    return !(data.size & LFSR_DATA_ONDISK);
}

static inline bool lfsr_data_isbptr(lfsr_data_t data) {
    return data.size & LFSR_DATA_ISBPTR;
}

static inline lfs_size_t lfsr_data_size(lfsr_data_t data) {
    return data.size & ~(LFSR_DATA_ONDISK | LFSR_DATA_ISBPTR);
}

// data slicing
#define LFSR_DATA_SLICE(_data, _off, _size) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromslice(_data, _off, _size)}.d)

LFS_FORCEINLINE
static inline lfsr_data_t lfsr_data_fromslice(lfsr_data_t data,
        lfs_ssize_t off, lfs_ssize_t size) {
    // limit our off/size to data range, note the use of unsigned casts
    // here to treat -1 as unbounded
    lfs_size_t off_ = lfs_min(
            lfs_smax(off, 0),
            lfsr_data_size(data));
    lfs_size_t size_ = lfs_min(
            (lfs_size_t)size,
            lfsr_data_size(data) - off_);

    // on-disk?
    if (lfsr_data_ondisk(data)) {
        data.u.disk.off += off_;
        data.size -= lfsr_data_size(data) - size_;

    // buffer?
    } else {
        data.u.buffer += off_;
        data.size -= lfsr_data_size(data) - size_;
    }

    return data;
}

#define LFSR_DATA_TRUNCATE(_data, _size) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromtruncate(_data, _size)}.d)

LFS_FORCEINLINE
static inline lfsr_data_t lfsr_data_fromtruncate(lfsr_data_t data,
        lfs_size_t size) {
    return LFSR_DATA_SLICE(data, -1, size);
}

#define LFSR_DATA_FRUNCATE(_data, _size) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromfruncate(_data, _size)}.d)

LFS_FORCEINLINE
static inline lfsr_data_t lfsr_data_fromfruncate(lfsr_data_t data,
        lfs_size_t size) {
    return LFSR_DATA_SLICE(data,
            lfsr_data_size(data) - lfs_min(
                size,
                lfsr_data_size(data)),
            -1);
}


// data <-> bd interactions

// lfsr_data_read* operations update the lfsr_data_t, effectively
// consuming the data

// needed in lfsr_data_read and friends
#ifdef LFS_CKDATACKSUMS
static inline bool lfsr_m_isckdatacksums(uint32_t flags);
#endif

static lfs_ssize_t lfsr_data_read(lfs_t *lfs, lfsr_data_t *data,
        void *buffer, lfs_size_t size) {
    // limit our size to data range
    lfs_size_t d = lfs_min(size, lfsr_data_size(*data));

    // on-disk?
    if (lfsr_data_ondisk(*data)) {
        // validating data cksums?
        if (LFS_IFDEF_CKDATACKSUMS(
                lfsr_m_isckdatacksums(lfs->flags)
                    && lfsr_data_isbptr(*data),
                false)) {
            #ifdef LFS_CKDATACKSUMS
            int err = lfsr_bd_readck(lfs,
                    data->u.disk.block, data->u.disk.off,
                    // note our hint includes the full data range
                    lfsr_data_size(*data),
                    buffer, d,
                    data->u.disk.cksize, data->u.disk.cksum);
            if (err < 0) {
                return err;
            }
            #endif

        } else {
            int err = lfsr_bd_read(lfs,
                    data->u.disk.block, data->u.disk.off,
                    // note our hint includes the full data range
                    lfsr_data_size(*data),
                    buffer, d);
            if (err < 0) {
                return err;
            }
        }

    // buffer?
    } else {
        lfs_memcpy(buffer, data->u.buffer, d);
    }

    *data = LFSR_DATA_SLICE(*data, d, -1);
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

    *data = LFSR_DATA_SLICE(*data, d, -1);
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
    lfs_size_t d = lfs_min(size, lfsr_data_size(data));

    // on-disk?
    if (lfsr_data_ondisk(data)) {
        // validating data cksums?
        if (LFS_IFDEF_CKDATACKSUMS(
                lfsr_m_isckdatacksums(lfs->flags)
                    && lfsr_data_isbptr(data),
                false)) {
            #ifdef LFS_CKDATACKSUMS
            int cmp = lfsr_bd_cmpck(lfs,
                    // note the 0 hint, we don't usually use any
                    // following data
                    data.u.disk.block, data.u.disk.off, 0,
                    buffer, d,
                    data.u.disk.cksize, data.u.disk.cksum);
            if (cmp != LFS_CMP_EQ) {
                return cmp;
            }
            #endif

        } else {
            int cmp = lfsr_bd_cmp(lfs,
                    // note the 0 hint, we don't usually use any
                    // following data
                    data.u.disk.block, data.u.disk.off, 0,
                    buffer, d);
            if (cmp != LFS_CMP_EQ) {
                return cmp;
            }
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
        lfsr_did_t did, const char *name, lfs_size_t name_len) {
    // first compare the did
    lfsr_did_t did_;
    int err = lfsr_data_readleb128(lfs, &data, &did_);
    if (err < 0) {
        return err;
    }

    if (did_ < did) {
        return LFS_CMP_LT;
    } else if (did_ > did) {
        return LFS_CMP_GT;
    }

    // then compare the actual name
    return lfsr_data_cmp(lfs, data, name, name_len);
}

static int lfsr_bd_progdata(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfsr_data_t data,
        uint32_t *cksum, bool align) {
    // on-disk?
    if (lfsr_data_ondisk(data)) {
        // validating data cksums?
        if (LFS_IFDEF_CKDATACKSUMS(
                lfsr_m_isckdatacksums(lfs->flags)
                    && lfsr_data_isbptr(data),
                false)) {
            #ifdef LFS_CKDATACKSUMS
            int err = lfsr_bd_cpyck(lfs, block, off,
                    data.u.disk.block, data.u.disk.off, lfsr_data_size(data),
                    lfsr_data_size(data),
                    data.u.disk.cksize, data.u.disk.cksum,
                    cksum, align);
            if (err) {
                return err;
            }
            #endif

        } else {
            int err = lfsr_bd_cpy(lfs, block, off,
                    data.u.disk.block, data.u.disk.off, lfsr_data_size(data),
                    lfsr_data_size(data),
                    cksum, align);
            if (err) {
                return err;
            }
        }

    // buffer?
    } else {
        int err = lfsr_bd_prog(lfs, block, off,
                data.u.buffer, data.size,
                cksum, align);
        if (err) {
            return err;
        }
    }

    return 0;
}


// macros for le32/leb128/lleb128 encoding, these are useful for
// building rattrs

// le32 encoding:
// .---+---+---+---.  total: 1 le32  4 bytes
// |     le32      |
// '---+---+---+---'
//
#define LFSR_LE32_DSIZE 4

static inline lfsr_data_t lfsr_data_fromle32(uint32_t word,
        uint8_t buffer[static LFSR_LE32_DSIZE]) {
    lfs_tole32_(word, buffer);
    return LFSR_DATA_BUF(buffer, LFSR_LE32_DSIZE);
}

// leb128 encoding:
// .---+- -+- -+- -+- -.  total: 1 leb128  <=5 bytes
// |      leb128       |
// '---+- -+- -+- -+- -'
//
#define LFSR_LEB128_DSIZE 5

static inline lfsr_data_t lfsr_data_fromleb128(uint32_t word,
        uint8_t buffer[static LFSR_LEB128_DSIZE]) {
    // leb128s should not exceed 31-bits
    LFS_ASSERT(word <= 0x7fffffff);

    lfs_ssize_t d = lfs_toleb128(word, buffer, LFSR_LEB128_DSIZE);
    if (d < 0) {
        LFS_UNREACHABLE();
    }

    return LFSR_DATA_BUF(buffer, d);
}

// lleb128 encoding:
// .---+- -+- -+- -.  total: 1 leb128  <=4 bytes
// |    lleb128    |
// '---+- -+- -+- -'
//
#define LFSR_LLEB128_DSIZE 4

static inline lfsr_data_t lfsr_data_fromlleb128(uint32_t word,
        uint8_t buffer[static LFSR_LLEB128_DSIZE]) {
    // little-leb128s should not exceed 28-bits
    LFS_ASSERT(word <= 0x0fffffff);

    lfs_ssize_t d = lfs_toleb128(word, buffer, LFSR_LLEB128_DSIZE);
    if (d < 0) {
        LFS_UNREACHABLE();
    }

    return LFSR_DATA_BUF(buffer, d);
}


// we need to at least define DSIZE/DATA macros here

// ecksum encoding:
// .---+- -+- -+- -.  cksize: 1 leb128  <=4 bytes
// | cksize        |  cksum:  1 le32    4 bytes
// +---+- -+- -+- -+  total:            <=8 bytes
// |     cksum     |
// '---+---+---+---'
//
#define LFSR_ECKSUM_DSIZE (4+4)

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

// shrub encoding:
// .---+- -+- -+- -+- -.  weight: 1 leb128  <=5 bytes
// | weight            |  trunk:  1 leb128  <=4 bytes
// +---+- -+- -+- -+- -'  total:            <=9 bytes
// | trunk         |
// '---+- -+- -+- -'
//
#define LFSR_SHRUB_DSIZE (5+4)

// mptr encoding:
// .---+- -+- -+- -+- -.  blocks: 2 leb128s  <=2x5 bytes
// | block x 2         |  total:             <=10 bytes
// +                   +
// |                   |
// '---+- -+- -+- -+- -'
//
#define LFSR_MPTR_DSIZE (5+5)

// geometry encoding
// .---+- -+- -+- -.      block_size:  1 leb128  <=4 bytes
// | block_size    |      block_count: 1 leb128  <=5 bytes
// +---+- -+- -+- -+- -.  total:                 <=9 bytes
// | block_count       |
// '---+- -+- -+- -+- -'
#define LFSR_GEOMETRY_DSIZE (4+5)


// operations on attribute lists

// our core attribute type
typedef struct lfsr_rattr {
    lfsr_tag_t tag;
    // ignoring lazy/special tags
    // sign(count)=0 => in-RAM buffer or estimate for lazy tags
    // sign(count)=1 => multiple concatenated datas
    int16_t count;
    lfsr_srid_t weight;
    union {
        const uint8_t *buffer;
        const lfsr_data_t *datas;
        uint32_t le32;
        uint32_t leb128;
        uint32_t lleb128;
        const void *etc;
    } u;
} lfsr_rattr_t;

// low-level attr macro
#define LFSR_RATTR_(_tag, _weight, _u, _count) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=_count, \
        .weight=_weight, \
        .u=_u})

// high-level attr macros
#define LFSR_RATTR(_tag, _weight) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.datas=NULL})

#define LFSR_RATTR_BUF(_tag, _weight, _buffer, _size) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=(uint16_t){_size}, \
        .weight=_weight, \
        .u.buffer=(const void*)(_buffer)})

#define LFSR_RATTR_DATA(_tag, _weight, _data) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=-1, \
        .weight=_weight, \
        .u.datas=_data})

#define LFSR_RATTR_CAT_(_tag, _weight, _datas, _data_count) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=-(uint16_t){_data_count}, \
        .weight=_weight, \
        .u.datas=_datas})

#define LFSR_RATTR_CAT(_tag, _weight, ...) \
    LFSR_RATTR_CAT_( \
        _tag, \
        _weight, \
        ((const lfsr_data_t[]){__VA_ARGS__}), \
        sizeof((const lfsr_data_t[]){__VA_ARGS__}) / sizeof(lfsr_data_t))

#define LFSR_RATTR_NOOP() \
    ((lfsr_rattr_t){ \
        .tag=LFSR_TAG_NULL, \
        .count=0, \
        .weight=0, \
        .u.buffer=NULL})

// as convenience we lazily encode single le32/leb128/lleb128 attrs
//
// this also avoids needing a stack allocation for these attrs
#define LFSR_RATTR_LE32(_tag, _weight, _le32) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.le32=_le32})

#define LFSR_RATTR_LEB128(_tag, _weight, _leb128) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.leb128=_leb128})

#define LFSR_RATTR_LLEB128(_tag, _weight, _lleb128) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.lleb128=_lleb128})

// helper macro for did + name pairs
typedef struct lfsr_name {
    uint32_t did;
    const char *name;
    lfs_size_t name_len;
} lfsr_name_t;

#define LFSR_RATTR_NAME_(_tag, _weight, _name) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfsr_name_t*){_name}})

#define LFSR_RATTR_NAME(_tag, _weight, _did, _name, _name_len) \
    LFSR_RATTR_NAME_( \
        _tag, \
        _weight, \
        (&(const lfsr_name_t){ \
            .did=_did, \
            .name=_name, \
            .name_len=_name_len}))

// macros for other lazily encoded attrs
#define LFSR_RATTR_GEOMETRY(_tag, _weight, _geometry) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfsr_geometry_t*){_geometry}})

// note the LFSR_BPTR_DSIZE hint so shrub estimates work
#define LFSR_RATTR_BPTR(_tag, _weight, _bptr) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=LFSR_BPTR_DSIZE, \
        .weight=_weight, \
        .u.etc=(const lfsr_bptr_t*){_bptr}})

#define LFSR_RATTR_SHRUB(_tag, _weight, _shrub) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfsr_shrub_t*){_shrub}})

#define LFSR_RATTR_BTREE(_tag, _weight, _btree) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfsr_btree_t*){_btree}})

#define LFSR_RATTR_MPTR(_tag, _weight, _mptr) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfs_block_t*){_mptr}})

#define LFSR_RATTR_ECKSUM(_tag, _weight, _ecksum) \
    ((lfsr_rattr_t){ \
        .tag=_tag, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfsr_ecksum_t*){_ecksum}})

// these are special attrs that trigger unique behavior in
// lfsr_mdir_commit__
#define LFSR_RATTR_RATTRS(_rattrs, _rattr_count) \
    ((lfsr_rattr_t){ \
        .tag=LFSR_TAG_RATTRS, \
        .count=(uint16_t){_rattr_count}, \
        .weight=0, \
        .u.etc=(const lfsr_rattr_t*){_rattrs}})

#define LFSR_RATTR_SHRUBCOMMIT(_shrubcommit) \
    ((lfsr_rattr_t){ \
        .tag=LFSR_TAG_SHRUBCOMMIT, \
        .count=0, \
        .weight=0, \
        .u.etc=(const lfsr_shrubcommit_t*){_shrubcommit}})

#define LFSR_RATTR_MOVE(_move) \
    ((lfsr_rattr_t){ \
        .tag=LFSR_TAG_MOVE, \
        .count=0, \
        .weight=0, \
        .u.etc=(const lfsr_mdir_t*){_move}})

#define LFSR_RATTR_ATTRS(_attrs, _attr_count) \
    ((lfsr_rattr_t){ \
        .tag=LFSR_TAG_ATTRS, \
        .count=(uint16_t){_attr_count}, \
        .weight=0, \
        .u.etc=(const struct lfs_attr*){_attrs}})

// create an attribute list
#define LFSR_RATTRS(...) \
    (const lfsr_rattr_t[]){__VA_ARGS__}, \
    sizeof((const lfsr_rattr_t[]){__VA_ARGS__}) / sizeof(lfsr_rattr_t)

// rattr helpers
static inline bool lfsr_rattr_isnoop(lfsr_rattr_t rattr) {
    // noop rattrs must have zero weight
    LFS_ASSERT(rattr.tag || rattr.weight == 0);
    return !rattr.tag;
}

static inline bool lfsr_rattr_isinsert(lfsr_rattr_t rattr) {
    return !lfsr_tag_isgrow(rattr.tag) && rattr.weight > 0;
}

static inline lfsr_srid_t lfsr_rattr_nextrid(lfsr_rattr_t rattr,
        lfsr_srid_t rid) {
    if (lfsr_rattr_isinsert(rattr)) {
        return rid + rattr.weight-1;
    } else {
        return rid + rattr.weight;
    }
}

static inline lfsr_tag_t lfsr_rattr_dtag(lfsr_rattr_t rattr) {
    // lazily tag encoding can be bypassed with explicit data, this is
    // necessary to allow copies during compaction, relocation, etc
    if (rattr.count >= 0) {
        return rattr.tag;
    } else {
        return LFSR_TAG_DATA;
    }
}

static inline lfs_size_t lfsr_rattr_dsize(lfsr_rattr_t rattr) {
    // note this does not include the tag size
    //
    // this gets a bit complicated for concatenated data
    if (rattr.count >= 0) {
        return rattr.count;
    } else {
        const lfsr_data_t *datas = rattr.u.datas;
        lfs_size_t data_count = -rattr.count;
        lfs_size_t size = 0;
        for (lfs_size_t i = 0; i < data_count; i++) {
            size += lfsr_data_size(datas[i]);
        }
        return size;
    }
}



// operations on custom attribute lists
//
// a slightly different struct because it's user facing

static inline lfs_ssize_t lfsr_attr_size(const struct lfs_attr *attr) {
    // we default to the buffer_size if a mutable size is not provided
    if (attr->size) {
        return *attr->size;
    } else {
        return attr->buffer_size;
    }
}

static inline bool lfsr_attr_isnoattr(const struct lfs_attr *attr) {
    return lfsr_attr_size(attr) == LFS_ERR_NOATTR;
}

static lfs_scmp_t lfsr_attr_cmp(lfs_t *lfs, const struct lfs_attr *attr,
        const lfsr_data_t *data) {
    // note data=NULL => NOATTR
    if (!data) {
        return (lfsr_attr_isnoattr(attr)) ? LFS_CMP_EQ : LFS_CMP_GT;
    } else {
        if (lfsr_attr_isnoattr(attr)) {
            return LFS_CMP_LT;
        } else {
            return lfsr_data_cmp(lfs, *data,
                    attr->buffer,
                    lfsr_attr_size(attr));
        }
    }
}



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
static lfsr_data_t lfsr_data_fromecksum(const lfsr_ecksum_t *ecksum,
        uint8_t buffer[static LFSR_ECKSUM_DSIZE]) {
    // you shouldn't try to encode a not-ecksum, that doesn't make sense
    LFS_ASSERT(ecksum->cksize != -1);
    // cksize should not exceed 28-bits
    LFS_ASSERT((lfs_size_t)ecksum->cksize <= 0x0fffffff);

    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfs_toleb128(ecksum->cksize, &buffer[d], 4);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
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

static void lfsr_bptr_init(lfsr_bptr_t *bptr,
        lfsr_data_t data, lfs_size_t cksize, uint32_t cksum) {
    // make sure the bptr flag is set
    LFS_ASSERT(lfsr_data_ondisk(data));
    bptr->data.size = data.size | LFSR_DATA_ISBPTR;
    bptr->data.u.disk.block = data.u.disk.block;
    bptr->data.u.disk.off = data.u.disk.off;
    #ifdef LFS_CKDATACKSUMS
    bptr->data.u.disk.cksize = cksize;
    bptr->data.u.disk.cksum = cksum;
    #else
    bptr->cksize = cksize;
    bptr->cksum = cksum;
    #endif
}

static inline bool lfsr_bptr_isbptr(const lfsr_bptr_t *bptr) {
    return lfsr_data_isbptr(bptr->data);
}

// checked reads adds ck info to lfsr_data_t that we don't want to
// unnecessarily duplicate, this makes accessing ck info annoyingly
// messy...
static inline lfs_size_t lfsr_bptr_cksize(const lfsr_bptr_t *bptr) {
    #ifdef LFS_CKDATACKSUMS
    return bptr->data.u.disk.cksize;
    #else
    return bptr->cksize;
    #endif
}

static inline uint32_t lfsr_bptr_cksum(const lfsr_bptr_t *bptr) {
    #ifdef LFS_CKDATACKSUMS
    return bptr->data.u.disk.cksum;
    #else
    return bptr->cksum;
    #endif
}

// bptr on-disk encoding
static lfsr_data_t lfsr_data_frombptr(const lfsr_bptr_t *bptr,
        uint8_t buffer[static LFSR_BPTR_DSIZE]) {
    // size should not exceed 28-bits
    LFS_ASSERT(lfsr_data_size(bptr->data) <= 0x0fffffff);
    // block should not exceed 31-bits
    LFS_ASSERT(bptr->data.u.disk.block <= 0x7fffffff);
    // off should not exceed 28-bits
    LFS_ASSERT(bptr->data.u.disk.off <= 0x0fffffff);
    // cksize should not exceed 28-bits
    LFS_ASSERT(lfsr_bptr_cksize(bptr) <= 0x0fffffff);
    lfs_ssize_t d = 0;

    // write the block, offset, size
    lfs_ssize_t d_ = lfs_toleb128(lfsr_data_size(bptr->data), &buffer[d], 4);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    d_ = lfs_toleb128(bptr->data.u.disk.block, &buffer[d], 5);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    d_ = lfs_toleb128(bptr->data.u.disk.off, &buffer[d], 4);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    // write the cksize, cksum
    d_ = lfs_toleb128(lfsr_bptr_cksize(bptr), &buffer[d], 4);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    lfs_tole32_(lfsr_bptr_cksum(bptr), &buffer[d]);
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
    err = lfsr_data_readlleb128(lfs, data,
            LFS_IFDEF_CKDATACKSUMS(
                &bptr->data.u.disk.cksize,
                &bptr->cksize));
    if (err) {
        return err;
    }

    err = lfsr_data_readle32(lfs, data,
            LFS_IFDEF_CKDATACKSUMS(
                &bptr->data.u.disk.cksum,
                &bptr->cksum));
    if (err) {
        return err;
    }

    // mark as on-disk + cksum
    bptr->data.size |= LFSR_DATA_ONDISK | LFSR_DATA_ISBPTR;
    return 0;
}

// check the contents of a bptr
static int lfsr_bptr_ck(lfs_t *lfs, const lfsr_bptr_t *bptr) {
    uint32_t cksum = 0;
    int err = lfsr_bd_cksum(lfs,
            bptr->data.u.disk.block, 0, 0,
            lfsr_bptr_cksize(bptr),
            &cksum);
    if (err) {
        return err;
    }

    // test that our cksum matches what's expected
    if (cksum != lfsr_bptr_cksum(bptr)) {
        LFS_ERROR("Found bptr cksum mismatch "
                    "0x%"PRIx32".%"PRIx32" %"PRId32", "
                    "cksum %08"PRIx32" (!= %08"PRIx32")",
                bptr->data.u.disk.block, 0,
                lfsr_bptr_cksize(bptr),
                cksum, lfsr_bptr_cksum(bptr));
        return LFS_ERR_CORRUPT;
    }

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


/// Red-black-yellow Dhara tree operations ///

#define LFSR_RBYD_ISSHRUB 0x80000000
#define LFSR_RBYD_ISPERTURB 0x80000000

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

static inline bool lfsr_rbyd_isperturb(const lfsr_rbyd_t *rbyd) {
    return rbyd->eoff & LFSR_RBYD_ISPERTURB;
}

static inline lfs_size_t lfsr_rbyd_eoff(const lfsr_rbyd_t *rbyd) {
    return rbyd->eoff & ~LFSR_RBYD_ISPERTURB;
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


// needed in lfsr_rbyd_alloc
static lfs_sblock_t lfs_alloc(lfs_t *lfs, bool erase);

// allocate an rbyd block
static int lfsr_rbyd_alloc(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    lfs_sblock_t block = lfs_alloc(lfs, true);
    if (block < 0) {
        return block;
    }

    rbyd->blocks[0] = block;
    rbyd->trunk = 0;
    rbyd->weight = 0;
    rbyd->eoff = 0;
    rbyd->cksum = 0;
    return 0;
}

static int lfsr_rbyd_ckecksum(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        const lfsr_ecksum_t *ecksum) {
    // check that the ecksum looks right
    if (lfsr_rbyd_eoff(rbyd) + ecksum->cksize >= lfs->cfg->block_size
            || lfsr_rbyd_eoff(rbyd) % lfs->cfg->prog_size != 0) {
        return LFS_ERR_CORRUPT;
    }

    // the next valid bit must _not_ match, or a commit was attempted,
    // this should hopefully stay in our cache
    uint8_t e;
    int err = lfsr_bd_read(lfs,
            rbyd->blocks[0], lfsr_rbyd_eoff(rbyd), ecksum->cksize,
            &e, 1);
    if (err) {
        return err;
    }

    if (((e >> 7)^lfsr_rbyd_isperturb(rbyd)) == lfs_parity(rbyd->cksum)) {
        return LFS_ERR_CORRUPT;
    }

    // check that erased-state matches our checksum, if this fails
    // most likely a write was interrupted
    uint32_t ecksum_ = 0;
    err = lfsr_bd_cksum(lfs,
            rbyd->blocks[0], lfsr_rbyd_eoff(rbyd), 0,
            ecksum->cksize,
            &ecksum_);
    if (err) {
        return err;
    }

    // found erased-state?
    return (ecksum_ == ecksum->cksum) ? 0 : LFS_ERR_CORRUPT;
}

// needed in lfsr_rbyd_fetch_ if debugging rbyd balance
static int lfsr_rbyd_lookupnext_(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag,
        lfsr_srid_t *rid_, lfsr_tag_t *tag_, lfsr_rid_t *weight_,
        lfsr_data_t *data_,
        lfs_size_t *height_, lfs_size_t *bheight_);

// fetch an rbyd
static int lfsr_rbyd_fetch_(lfs_t *lfs,
        lfsr_rbyd_t *rbyd, uint32_t *gcksumdelta,
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
    lfs_size_t off = sizeof(uint32_t);
    lfs_size_t trunk_ = 0;
    lfs_size_t trunk__ = 0;
    lfsr_rid_t weight = 0;
    lfsr_rid_t weight_ = 0;

    // assume unerased until proven otherwise
    lfsr_ecksum_t ecksum = {.cksize=-1};
    lfsr_ecksum_t ecksum_ = {.cksize=-1};

    // also find gcksumdelta, though this is only used by mdirs
    uint32_t gcksumdelta_ = 0;

    // scan tags, checking valid bits, cksums, etc
    while (off < lfs->cfg->block_size
            && (!trunk || lfsr_rbyd_eoff(rbyd) <= trunk)) {
        // read next tag
        lfsr_tag_t tag;
        lfsr_rid_t weight__;
        lfs_size_t size;
        lfs_ssize_t d = lfsr_bd_readtag(lfs, block, off, -1,
                &tag, &weight__, &size,
                &cksum_);
        if (d < 0) {
            if (d == LFS_ERR_CORRUPT) {
                break;
            }
            return d;
        }
        lfs_size_t off_ = off + d;

        // readtag should already check we're in-bounds
        LFS_ASSERT(lfsr_tag_isalt(tag)
                || off_ + size <= lfs->cfg->block_size);

        // take care of cksum
        if (!lfsr_tag_isalt(tag)) {
            // not an end-of-commit cksum
            if (lfsr_tag_suptype(tag) != LFSR_TAG_CKSUM) {
                // cksum the entry, hopefully leaving it in the cache
                err = lfsr_bd_cksum(lfs, block, off_, -1, size,
                        &cksum_);
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }

                // found an ecksum? save for later
                if (tag == LFSR_TAG_ECKSUM) {
                    err = lfsr_data_readecksum(lfs,
                            &LFSR_DATA_DISK(block, off_,
                                // note this size is to make the hint do
                                // what we want
                                lfs->cfg->block_size - off_),
                            &ecksum_);
                    if (err) {
                        if (err == LFS_ERR_CORRUPT) {
                            break;
                        }
                        return err;
                    }

                // found gcksumdelta? save for later
                } else if (tag == LFSR_TAG_GCKSUMDELTA) {
                    err = lfsr_data_readle32(lfs,
                            &LFSR_DATA_DISK(block, off_,
                                // note this size is to make the hint do
                                // what we want
                                lfs->cfg->block_size - off_),
                            &gcksumdelta_);
                    if (err) {
                        if (err == LFS_ERR_CORRUPT) {
                            break;
                        }
                        return err;
                    }
                }

            // is an end-of-commit cksum
            } else {
                // truncate checksum?
                if (size < sizeof(uint32_t)) {
                    break;
                }

                // check checksum
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
                    // uh oh, checksums don't match
                    break;
                }

                // save what we've found so far
                rbyd->eoff
                        = ((lfs_size_t)lfsr_tag_p(tag)
                            << (8*sizeof(lfs_size_t)-1))
                        | (off_ + size);
                rbyd->cksum = cksum;
                rbyd->trunk = (LFSR_RBYD_ISSHRUB & rbyd->trunk) | trunk_;
                rbyd->weight = weight;
                ecksum = ecksum_;
                ecksum_.cksize = -1;
                if (gcksumdelta) {
                    *gcksumdelta = gcksumdelta_;
                }
                gcksumdelta_ = 0;

                // revert to canonical checksum and perturb if necessary
                cksum_ = cksum
                        ^ ((lfsr_rbyd_isperturb(rbyd))
                            ? LFS_CRC32C_ODDZERO
                            : 0);
            }
        }

        // found a trunk?
        if (lfsr_tag_istrunk(tag)) {
            if (!(trunk && off > trunk && !trunk__)) {
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
                    // update trunk and weight, unless we are a shrub trunk
                    if (!lfsr_tag_isshrub(tag) || trunk__ == trunk) {
                        trunk_ = trunk__;
                        weight = weight_;
                    }
                    trunk__ = 0;
                }
            }

            // update canonical checksum, xoring out any perturb
            // state, we don't want erased-state affecting our
            // canonical checksum
            cksum = cksum_
                    ^ ((lfsr_rbyd_isperturb(rbyd))
                        ? LFS_CRC32C_ODDZERO
                        : 0);
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
    if (ecksum.cksize != -1) {
        // check the erased-state checksum
        err = lfsr_rbyd_ckecksum(lfs, rbyd, &ecksum);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        // found valid erased-state?
        erased = (err != LFS_ERR_CORRUPT);
    }

    // used eoff=-1 to indicate when there is no erased-state
    if (!erased) {
        rbyd->eoff = -1;
    }

    #ifdef LFS_DBGRBYDFETCHES
    LFS_DEBUG("Fetched rbyd 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "eoff %"PRId32", cksum %"PRIx32,
            rbyd->blocks[0], lfsr_rbyd_trunk(rbyd),
            rbyd->weight,
            (lfsr_rbyd_eoff(rbyd) >= lfs->cfg->block_size)
                ? -1
                : (lfs_ssize_t)lfsr_rbyd_eoff(rbyd),
            rbyd->cksum);
    #endif

    // debugging rbyd balance? check that all branches in the rbyd have
    // the same height
    #ifdef LFS_DBGRBYDBALANCE
    lfsr_srid_t rid = -1;
    lfsr_tag_t tag = 0;
    lfs_size_t min_height = 0;
    lfs_size_t max_height = 0;
    lfs_size_t min_bheight = 0;
    lfs_size_t max_bheight = 0;
    while (true) {
        lfs_size_t height;
        lfs_size_t bheight;
        int err = lfsr_rbyd_lookupnext_(lfs, rbyd,
                rid, tag+1,
                &rid, &tag, NULL, NULL,
                &height, &bheight);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }

        // find the min/max height and bheight
        min_height = (min_height) ? lfs_min(min_height, height) : height;
        max_height = (max_height) ? lfs_max(max_height, height) : height;
        min_bheight = (min_bheight) ? lfs_min(min_bheight, bheight) : bheight;
        max_bheight = (max_bheight) ? lfs_max(max_bheight, bheight) : bheight;
    }
    LFS_DEBUG("Fetched rbyd 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "height %"PRId32"-%"PRId32", "
                "bheight %"PRId32"-%"PRId32,
            rbyd->blocks[0], lfsr_rbyd_trunk(rbyd),
            rbyd->weight,
            min_height, max_height,
            min_bheight, max_bheight);
    // all branches in the rbyd should have the same bheight
    LFS_ASSERT(max_bheight == min_bheight);
    // this limits alt height to no worse than 2*bheight+2 (2*bheight+1
    // for normal appends, 2*bheight+2 with range removals)
    LFS_ASSERT(max_height <= 2*min_height+2);
    #endif

    return 0;
}

static int lfsr_rbyd_fetch(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_block_t block, lfs_size_t trunk) {
    return lfsr_rbyd_fetch_(lfs, rbyd, NULL, block, trunk);
}

// a more aggressive fetch when checksum is known
static int lfsr_rbyd_fetchck(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_block_t block, lfs_size_t trunk,
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
        LFS_ERROR("Found rbyd cksum mismatch 0x%"PRIx32".%"PRIx32", "
                    "cksum %08"PRIx32" (!= %08"PRIx32")",
                rbyd->blocks[0], lfsr_rbyd_trunk(rbyd),
                rbyd->cksum, cksum);
        return LFS_ERR_CORRUPT;
    }

    // if trunk/weight mismatch _after_ cksums match, that's not a storage
    // error, that's a programming error
    LFS_ASSERT(lfsr_rbyd_trunk(rbyd) == trunk);
    return 0;
}


// our core rbyd lookup algorithm
//
// finds the next rid+tag such that rid_+tag_ >= rid+tag
static int lfsr_rbyd_lookupnext_(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag,
        lfsr_srid_t *rid_, lfsr_tag_t *tag_, lfsr_rid_t *weight_,
        lfsr_data_t *data_,
        lfs_size_t *height_, lfs_size_t *bheight_) {
    // these bits should be clear at this point
    LFS_ASSERT(lfsr_tag_mode(tag) == 0);

    // make sure we never look up zero tags, the way we create
    // unreachable tags has a hole here
    tag = lfs_max(tag, 0x1);

    // out of bounds? no trunk yet?
    if (rid >= (lfsr_srid_t)rbyd->weight || !lfsr_rbyd_trunk(rbyd)) {
        return LFS_ERR_NOENT;
    }

    // optionally find height/bheight for debugging rbyd balance
    if (height_) {
        *height_ = 0;
    }
    if (bheight_) {
        *bheight_ = 0;
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
                &alt, &weight, &jump,
                NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfsr_tag_isalt(alt)) {
            lfs_size_t branch_ = branch + d;

            // keep track of height for debugging
            if (height_) {
                *height_ += 1;
            }
            if (bheight_
                    // only count black+followed alts towards bheight
                    && (lfsr_tag_isblack(alt)
                        || lfsr_tag_follow(
                            alt, weight,
                            lower_rid, upper_rid,
                            rid, tag))) {
                *bheight_ += 1;
            }

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


// finds the next rid+tag such that rid_+tag_ >= rid+tag
static int lfsr_rbyd_lookupnext(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag,
        lfsr_srid_t *rid_, lfsr_tag_t *tag_, lfsr_rid_t *weight_,
        lfsr_data_t *data_) {
    return lfsr_rbyd_lookupnext_(lfs, rbyd, rid, tag,
            rid_, tag_, weight_, data_,
            NULL, NULL);
}

// lookup assumes a known rid
static int lfsr_rbyd_lookup(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_srid_t rid__;
    lfsr_tag_t tag__;
    int err = lfsr_rbyd_lookupnext(lfs, rbyd, rid, lfsr_tag_key(tag),
            &rid__, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if (rid__ != rid
            || (tag__ & lfsr_tag_mask(tag)) != (tag & lfsr_tag_mask(tag))) {
        return LFS_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}



// rbyd append operations

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

    int err = lfsr_bd_prog(lfs,
            rbyd->blocks[0], lfsr_rbyd_eoff(rbyd),
            &rev_buf, sizeof(uint32_t),
            &rbyd->cksum, false);
    if (err) {
        return err;
    }

    rbyd->eoff += sizeof(uint32_t);
    return 0;
}

// other low-level appends
static int lfsr_rbyd_appendtag(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_tag_t tag, lfsr_rid_t weight, lfs_size_t size) {
    // tag must not be internal at this point
    LFS_ASSERT(!lfsr_tag_isinternal(tag));
    // bit 7 is reserved for future subtype extensions
    LFS_ASSERT(!(tag & 0x80));

    // do we fit?
    if (lfsr_rbyd_eoff(rbyd) + LFSR_TAG_DSIZE
            > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    lfs_ssize_t d = lfsr_bd_progtag(lfs,
            rbyd->blocks[0], lfsr_rbyd_eoff(rbyd), lfsr_rbyd_isperturb(rbyd),
            tag, weight, size,
            &rbyd->cksum, false);
    if (d < 0) {
        return d;
    }

    rbyd->eoff += d;

    #ifdef LFS_CKPARITY
    // keep track of most recent parity
    lfs->ptail.block = rbyd->blocks[0];
    lfs->ptail.off
            = ((lfs_size_t)(
                    lfs_parity(rbyd->cksum) ^ lfsr_rbyd_isperturb(rbyd)
                ) << (8*sizeof(lfs_size_t)-1))
            | lfsr_rbyd_eoff(rbyd);
    #endif

    return 0;
}

// needed in lfsr_rbyd_appendrattr_
typedef struct lfsr_geometry lfsr_geometry_t;
static lfsr_data_t lfsr_data_fromgeometry(const lfsr_geometry_t *geometry,
        uint8_t buffer[static LFSR_GEOMETRY_DSIZE]);
static lfsr_data_t lfsr_data_frombptr(const lfsr_bptr_t *bptr,
        uint8_t buffer[static LFSR_BPTR_DSIZE]);
static lfsr_data_t lfsr_data_fromshrub(const lfsr_shrub_t *shrub,
        uint8_t buffer[static LFSR_SHRUB_DSIZE]);
static lfsr_data_t lfsr_data_frombtree(const lfsr_btree_t *btree,
        uint8_t buffer[static LFSR_BTREE_DSIZE]);
static lfsr_data_t lfsr_data_frommptr(const lfs_block_t mptr[static 2],
        uint8_t buffer[static LFSR_MPTR_DSIZE]);

// our core rbyd append algorithm
static int lfsr_rbyd_appendrattr_(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_rattr_t rattr) {
    // tag must not be internal at this point
    LFS_ASSERT(!lfsr_tag_isinternal(rattr.tag));
    // bit 7 is reserved for future subtype extensions
    LFS_ASSERT(!(rattr.tag & 0x80));

    // encode lazy tags?
    //
    // we encode most tags lazily as this heavily reduces stack usage,
    // though this does make us less gc-able at compile time
    lfs_size_t size;
    const void *data;
    int16_t count;
    struct {
        // uh, there's probably a better way to do this, but I'm not
        // sure what it is
        union {
            uint8_t buf[LFS_MAX(
                    LFSR_LE32_DSIZE,
                    LFS_MAX(
                        LFSR_LEB128_DSIZE,
                        LFS_MAX(
                            LFSR_GEOMETRY_DSIZE,
                            LFS_MAX(
                                LFSR_BPTR_DSIZE,
                                LFS_MAX(
                                    LFSR_SHRUB_DSIZE,
                                    LFS_MAX(
                                        LFSR_BTREE_DSIZE,
                                        LFS_MAX(
                                            LFSR_MPTR_DSIZE,
                                            LFSR_ECKSUM_DSIZE)))))))];
            struct {
                lfsr_data_t datas[2];
                uint8_t buf[LFSR_LEB128_DSIZE];
            } name;
        } u;
    } ctx;
    switch (lfsr_rattr_dtag(rattr)) {
    // le32?
    case LFSR_TAG_RCOMPAT:;
    case LFSR_TAG_WCOMPAT:;
    case LFSR_TAG_OCOMPAT:;
    case LFSR_TAG_GCKSUMDELTA:;
        lfsr_data_t data_ = lfsr_data_fromle32(rattr.u.le32, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // leb128?
    case LFSR_TAG_NAMELIMIT:;
    case LFSR_TAG_FILELIMIT:;
    case LFSR_TAG_BOOKMARK:;
    case LFSR_TAG_DID:;
        // leb128s should not exceed 31-bits
        LFS_ASSERT(rattr.u.leb128 <= 0x7fffffff);
        // little-leb128s should not exceed 28-bits
        LFS_ASSERT(rattr.tag != LFSR_TAG_NAMELIMIT
                || rattr.u.leb128 <= 0x0fffffff);
        data_ = lfsr_data_fromleb128(rattr.u.leb128, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // geometry?
    case LFSR_TAG_GEOMETRY:;
        data_ = lfsr_data_fromgeometry(rattr.u.etc, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // name?
    case LFSR_TAG_NAME:;
    case LFSR_TAG_REG:;
    case LFSR_TAG_DIR:;
    case LFSR_TAG_STICKYNOTE:;
        const lfsr_name_t *name = rattr.u.etc;
        ctx.u.name.datas[0] = lfsr_data_fromleb128(name->did, ctx.u.name.buf);
        ctx.u.name.datas[1] = LFSR_DATA_BUF(name->name, name->name_len);
        size = lfsr_data_size(ctx.u.name.datas[0]) + name->name_len;
        data = &ctx.u.name.datas;
        count = -2;
        break;

    // bptr?
    case LFSR_TAG_BLOCK:;
    case LFSR_TAG_SHRUB | LFSR_TAG_BLOCK:;
        data_ = lfsr_data_frombptr(rattr.u.etc, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // shrub trunk?
    case LFSR_TAG_BSHRUB:;
        // note unlike the other lazy tags, we _need_ to lazily encode
        // shrub trunks, since they change underneath us during mdir
        // compactions, relocations, etc
        data_ = lfsr_data_fromshrub(rattr.u.etc, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // btree?
    case LFSR_TAG_BTREE:;
    case LFSR_TAG_MTREE:;
        data_ = lfsr_data_frombtree(rattr.u.etc, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // mptr?
    case LFSR_TAG_MROOT:;
    case LFSR_TAG_MDIR:;
        data_ = lfsr_data_frommptr(rattr.u.etc, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // ecksum?
    case LFSR_TAG_ECKSUM:;
        data_ = lfsr_data_fromecksum(rattr.u.etc, ctx.u.buf);
        size = lfsr_data_size(data_);
        data = ctx.u.buf;
        count = size;
        break;

    // default to raw data
    default:;
        size = lfsr_rattr_dsize(rattr);
        data = rattr.u.datas;
        count = rattr.count;
        break;
    }

    // do we fit?
    if (lfsr_rbyd_eoff(rbyd) + LFSR_TAG_DSIZE + size
            > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    // append tag
    int err = lfsr_rbyd_appendtag(lfs, rbyd,
            rattr.tag, rattr.weight, size);
    if (err) {
        return err;
    }

    // direct buffer?
    if (count >= 0) {
        err = lfsr_bd_prog(lfs,
                rbyd->blocks[0], lfsr_rbyd_eoff(rbyd), data, count,
                &rbyd->cksum, false);
        if (err) {
            return err;
        }

        rbyd->eoff += count;

    // indirect concatenated data?
    } else {
        const lfsr_data_t *datas = data;
        lfs_size_t data_count = -count;
        for (lfs_size_t i = 0; i < data_count; i++) {
            err = lfsr_bd_progdata(lfs,
                    rbyd->blocks[0], lfsr_rbyd_eoff(rbyd), datas[i],
                    &rbyd->cksum, false);
            if (err) {
                return err;
            }

            rbyd->eoff += lfsr_data_size(datas[i]);
        }
    }

    #ifdef LFS_CKPARITY
    // keep track of most recent parity
    lfs->ptail.block = rbyd->blocks[0];
    lfs->ptail.off
            = ((lfs_size_t)(
                    lfs_parity(rbyd->cksum) ^ lfsr_rbyd_isperturb(rbyd)
                ) << (8*sizeof(lfs_size_t)-1))
            | lfsr_rbyd_eoff(rbyd);
    #endif

    return 0;
}

// checks before we append
static int lfsr_rbyd_appendinit(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
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
// lfsr_rbyd_appendrattr
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
            // jump=0 represents an unreachable alt, we do write out
            // unreachable alts sometimes in order to maintain the
            // balance of the tree
            LFS_ASSERT(p[3-1-i].jump || lfsr_tag_isblack(p[3-1-i].alt));
            lfsr_tag_t alt = p[3-1-i].alt;
            lfsr_rid_t weight = p[3-1-i].weight;
            // change to a relative jump at the last minute
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

        // unreachable alt? we can prune this now
        if (!p[1].jump) {
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
static int lfsr_rbyd_appendrattr(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_rattr_t rattr) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));
    // tag must not be internal at this point
    LFS_ASSERT(!lfsr_tag_isinternal(rattr.tag));
    // bit 7 is reserved for future subtype extensions
    LFS_ASSERT(!(rattr.tag & 0x80));
    // you can't delete more than what's in the rbyd
    LFS_ASSERT(rattr.weight >= -(lfsr_srid_t)rbyd->weight);

    // ignore noops
    if (lfsr_rattr_isnoop(rattr)) {
        return 0;
    }

    // begin appending
    int err = lfsr_rbyd_appendinit(lfs, rbyd);
    if (err) {
        return err;
    }

    // figure out what range of tags we're operating on
    lfsr_srid_t a_rid;
    lfsr_srid_t b_rid;
    lfsr_tag_t a_tag;
    lfsr_tag_t b_tag;
    if (!lfsr_tag_isgrow(rattr.tag) && rattr.weight != 0) {
        if (rattr.weight > 0) {
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
            a_rid = rid - lfs_smax(-rattr.weight, 0);
            b_rid = rid;
        }

        a_tag = 0;
        b_tag = 0;

    } else {
        LFS_ASSERT(rid < (lfsr_srid_t)rbyd->weight);

        a_rid = rid - lfs_smax(-rattr.weight, 0);
        b_rid = rid;

        // note both normal and rm wide-tags have the same bounds, really it's
        // the normal non-wide-tags that are an outlier here
        if (lfsr_tag_ismask12(rattr.tag)) {
            a_tag = 0x000;
            b_tag = 0xfff;
        } else if (lfsr_tag_ismask8(rattr.tag)) {
            a_tag = (rattr.tag & 0xf00);
            b_tag = (rattr.tag & 0xf00) + 0x100;
        } else if (lfsr_tag_ismask2(rattr.tag)) {
            a_tag = (rattr.tag & 0xffc);
            b_tag = (rattr.tag & 0xffc) + 0x004;
        } else if (lfsr_tag_isrm(rattr.tag)) {
            a_tag = lfsr_tag_key(rattr.tag);
            b_tag = lfsr_tag_key(rattr.tag) + 1;
        } else {
            a_tag = lfsr_tag_key(rattr.tag);
            b_tag = lfsr_tag_key(rattr.tag);
        }
    }
    a_tag = lfs_max(a_tag, 0x1);
    b_tag = lfs_max(b_tag, 0x1);

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
    lfsr_tag_t d_tag = 0;
    lfsr_srid_t d_weight = 0;

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
    lfsr_tag_t upper_tag = 0xfff;

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
                &alt, &weight, &jump,
                NULL);
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
                LFS_SWAP(lfs_size_t, &jump, &branch_);
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
                LFS_SWAP(lfsr_tag_t, &p[0].alt, &alt);
                LFS_SWAP(lfsr_rid_t, &p[0].weight, &weight);
                LFS_SWAP(lfs_size_t, &p[0].jump, &jump);
                alt = (alt & ~LFSR_TAG_R) | (p[0].alt & LFSR_TAG_R);
                p[0].alt |= LFSR_TAG_R;

                lfsr_tag_flip2(
                        &alt, &weight,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid);
                LFS_SWAP(lfs_size_t, &jump, &branch_);
            }

            // do bounds want to take different paths? begin diverging
            //                            >b                    <b
            //                          .-'|                  .-'|
            //         <b  =>           | nb  =>             nb  |
            //    .----'|      .--------|--'      .-----------'  |
            //   <b    <b      |       <b         |             nb
            // .-'|  .-'|      |     .-'|         |        .-----'
            // 1  2  3  4      1  2  3  4  x      1  2  3  4  x  x
            bool diverging_b = lfsr_tag_diverging2(
                    alt, weight,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    a_rid, a_tag,
                    b_rid, b_tag);
            bool diverging_r = lfsr_tag_isred(p[0].alt)
                    && lfsr_tag_diverging(
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
                if (diverging_b && diverging_r) {
                    LFS_ASSERT(a_rid < b_rid || a_tag < b_tag);
                    LFS_ASSERT(lfsr_tag_isparallel(alt, p[0].alt));

                    weight += p[0].weight;
                    jump = p[0].jump;
                    lfsr_rbyd_p_pop(p);

                    diverging_r = false;
                }

                // diverging? start trimming inner alts
                //                            >b
                //                          .-'|
                //         <b  =>           | nb
                //    .----'|      .--------|--'
                //   <b    <b      |       <b
                // .-'|  .-'|      |     .-'|
                // 1  2  3  4      1  2  3  4  x
                if ((diverging_b || diverging_r)
                        // diverging black?
                        && (lfsr_tag_isblack(alt)
                            // give up if we find a yellow alt
                            || (lfsr_tag_isred(p[0].alt)))) {
                    diverged = true;

                    // diverging upper? stitch together both trunks
                    //            >b                    <b
                    //          .-'|                  .-'|
                    //          | nb  =>             nb  |
                    // .--------|--'      .-----------'  |
                    // |       <b         |             nb
                    // |     .-'|         |        .-----'
                    // 1  2  3  4  x      1  2  3  4  x  x
                    if (a_rid > b_rid || a_tag > b_tag) {
                        LFS_ASSERT(!diverging_r);

                        alt = LFSR_TAG_ALT(
                            alt & LFSR_TAG_R,
                            LFSR_TAG_LE,
                            d_tag);
                        weight -= d_weight;
                        lower_rid += d_weight;
                    }
                }

            } else {
                // diverged? trim so alt will be pruned
                //   <b  =>       nb
                // .-'|         .--'
                // 3  4      3  4  x
                if (diverging_b) {
                    lfsr_tag_trim(
                            alt, weight,
                            &lower_rid, &upper_rid,
                            &lower_tag, &upper_tag);
                    weight = 0;
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
            if (lfsr_tag_isred(p[0].alt)
                    && lfsr_tag_unreachable(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)
                    && p[0].jump > branch) {
                alt &= ~LFSR_TAG_R;
                lfsr_rbyd_p_pop(p);

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
            } else if (lfsr_tag_isred(p[0].alt)
                    && lfsr_tag_unreachable2(
                        alt, weight,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)
                    && jump > branch) {
                alt = p[0].alt & ~LFSR_TAG_R;
                weight = p[0].weight;
                jump = p[0].jump;
                lfsr_rbyd_p_pop(p);
            }

            // prune red alts
            if (lfsr_tag_isred(p[0].alt)
                    && lfsr_tag_unreachable(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)) {
                // prune unreachable recolorable alts
                //      <r  =>          <b
                // .----'|         .----'|
                // |    <b         |     |
                // |  .-'|         |  .--'
                // 1  2  3      1  2  3  x
                LFS_ASSERT(p[0].jump < branch);
                lfsr_rbyd_p_pop(p);
            }

            // prune black alts
            if (lfsr_tag_unreachable2(
                    alt, weight,
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

                // prune unreachable recolorable alts
                //      <r  =>          <b
                // .----'|      .-------'|
                // |    <b      |        |
                // |  .-'|      |  .-----'
                // 1  2  3      1  2  3  x
                } else if (lfsr_tag_isred(p[0].alt)) {
                    LFS_ASSERT(jump < branch);
                    alt = (p[0].alt & ~LFSR_TAG_R) | (alt & LFSR_TAG_R);
                    weight = p[0].weight;
                    jump = p[0].jump;
                    lfsr_rbyd_p_pop(p);

                // we can't prune non-root black alts or we risk
                // breaking the color balance of our tree, so instead
                // we just mark these alts as unreachable (jump=0), and
                // collapse them if we propagate a red edge later
                //   <b  =>       nb
                // .-'|         .--'
                // 3  4      3  4  x
                } else if (lfsr_tag_isblack(alt)) {
                    alt = LFSR_TAG_ALT(
                            LFSR_TAG_B,
                            LFSR_TAG_LE,
                            (diverged && (a_rid > b_rid || a_tag > b_tag))
                                ? d_tag
                                : lower_tag);
                    LFS_ASSERT(weight == 0);
                    // jump=0 also asserts the alt is unreachable (or
                    // else we loop indefinitely), and uses the minimum
                    // alt encoding
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
                        LFS_SWAP(lfsr_tag_t, &p[0].alt, &alt);
                        LFS_SWAP(lfsr_rid_t, &p[0].weight, &weight);
                        LFS_SWAP(lfs_size_t, &p[0].jump, &jump);
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
                    LFS_SWAP(lfs_size_t, &jump, &branch_);
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
                    d_tag = lower_tag;
                    d_weight = upper_rid - lower_rid;

                    // flush any pending alts
                    err = lfsr_rbyd_p_flush(lfs, rbyd, p, 3);
                    if (err) {
                        return err;
                    }

                    // terminate diverged trunk with an unreachable tag
                    err = lfsr_rbyd_appendrattr_(lfs, rbyd, LFSR_RATTR(
                            (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0)
                                | LFSR_TAG_NULL,
                            0));
                    if (err) {
                        return err;
                    }

                    // swap tag/rid and move on to upper trunk
                    diverged = false;
                    branch = trunk_;
                    LFS_SWAP(lfsr_tag_t, &a_tag, &b_tag);
                    LFS_SWAP(lfsr_srid_t, &a_rid, &b_rid);
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
    // note we bias the weights here so that lfsr_rbyd_lookupnext
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
    lfsr_tag_t alt = 0;
    lfsr_rid_t weight = 0;
    if (tag_
            && (upper_rid-1 < rid-lfs_smax(-rattr.weight, 0)
                || (upper_rid-1 == rid-lfs_smax(-rattr.weight, 0)
                    && ((!lfsr_tag_isgrow(rattr.tag) && rattr.weight > 0)
                        || ((tag_ & lfsr_tag_mask(rattr.tag))
                            < (rattr.tag & lfsr_tag_mask(rattr.tag))))))) {
        if (lfsr_tag_isrm(rattr.tag) || !lfsr_tag_key(rattr.tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_GT, lower_tag);
            weight = upper_rid - lower_rid + rattr.weight;
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
                    && ((!lfsr_tag_isgrow(rattr.tag) && rattr.weight > 0)
                        || ((tag_ & lfsr_tag_mask(rattr.tag))
                            > (rattr.tag & lfsr_tag_mask(rattr.tag))))))) {
        if (lfsr_tag_isrm(rattr.tag) || !lfsr_tag_key(rattr.tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_GT, lower_tag);
            weight = upper_rid - lower_rid + rattr.weight;
            upper_rid -= weight;
        } else {
            // split greater than
            alt = LFSR_TAG_ALT(LFSR_TAG_B, LFSR_TAG_GT, rattr.tag);
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
    err = lfsr_rbyd_appendrattr_(lfs, rbyd, LFSR_RATTR_(
            // mark as shrub if we are a shrub
            (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0)
                // rm => null, otherwise strip off control bits
                | ((lfsr_tag_isrm(rattr.tag))
                    ? LFSR_TAG_NULL
                    : lfsr_tag_key(rattr.tag)),
            upper_rid - lower_rid + rattr.weight,
            rattr.u, rattr.count));
    if (err) {
        return err;
    }

    // update the trunk and weight
    rbyd->trunk = (rbyd->trunk & LFSR_RBYD_ISSHRUB) | trunk_;
    rbyd->weight += rattr.weight;
    return 0;
}

static int lfsr_rbyd_appendcksum_(lfs_t *lfs, lfsr_rbyd_t *rbyd,
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
    lfs_size_t off_ = lfs_alignup(
            lfsr_rbyd_eoff(rbyd) + 2+1+1+4+4 + 2+1+4+4,
            lfs->cfg->prog_size);

    // space for ecksum?
    bool perturb = false;
    if (off_ < lfs->cfg->block_size) {
        // read the leading byte in case we need to perturb the next commit,
        // this should hopefully stay in our cache
        uint8_t e = 0;
        int err = lfsr_bd_read(lfs,
                rbyd->blocks[0], off_, lfs->cfg->prog_size,
                &e, 1);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        // we don't want the next commit to appear as valid, so we
        // intentionally perturb the commit if this happens, this is
        // roughly equivalent to inverting all tags' valid bits
        perturb = ((e >> 7) == lfs_parity(cksum));

        // calculate the erased-state checksum
        uint32_t ecksum = 0;
        err = lfsr_bd_cksum(lfs,
                rbyd->blocks[0], off_, lfs->cfg->prog_size,
                lfs->cfg->prog_size,
                &ecksum);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        err = lfsr_rbyd_appendrattr_(lfs, rbyd, LFSR_RATTR_ECKSUM(
                LFSR_TAG_ECKSUM, 0,
                (&(lfsr_ecksum_t){
                    .cksize=lfs->cfg->prog_size,
                    .cksum=ecksum})));
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

    // build the end-of-commit checksum tag
    //
    // note padding-size depends on leb-encoding depends on padding-size
    // depends leb-encoding depends on... to get around this catch-22 we
    // just always write a fully-expanded leb128 encoding
    //
    bool v = lfs_parity(rbyd->cksum) ^ lfsr_rbyd_isperturb(rbyd);
    uint8_t cksum_buf[2+1+4+4];
    cksum_buf[0] = (uint8_t)(LFSR_TAG_CKSUM >> 8)
            // set the valid bit to the cksum parity
            | ((uint8_t)v << 7);
    cksum_buf[1] = (uint8_t)(LFSR_TAG_CKSUM >> 0)
            // set the perturb bit so next commit is invalid
            | ((uint8_t)perturb << 0);
    cksum_buf[2] = 0;

    lfs_size_t padding = off_ - (lfsr_rbyd_eoff(rbyd) + 2+1+4);
    cksum_buf[3] = 0x80 | (0x7f & (padding >>  0));
    cksum_buf[4] = 0x80 | (0x7f & (padding >>  7));
    cksum_buf[5] = 0x80 | (0x7f & (padding >> 14));
    cksum_buf[6] = 0x00 | (0x7f & (padding >> 21));

    // exclude the valid bit
    uint32_t cksum_ = rbyd->cksum ^ ((uint32_t)v << 7);
    // calculate the commit checksum
    cksum_ = lfs_crc32c(cksum_, cksum_buf, 2+1+4);
    // and perturb, perturbing the commit checksum avoids a perturb hole
    // after the last valid bit
    //
    // note the odd-parity zero preserves our position in the crc32c
    // ring while only changing the parity
    cksum_ ^= (lfsr_rbyd_isperturb(rbyd)) ? LFS_CRC32C_ODDZERO : 0;
    lfs_tole32_(cksum_, &cksum_buf[2+1+4]);

    // prog, when this lands on disk commit is committed
    int err = lfsr_bd_prog(lfs, rbyd->blocks[0], lfsr_rbyd_eoff(rbyd),
            cksum_buf, 2+1+4+4,
            NULL, false);
    if (err) {
        return err;
    }

    // flush any pending progs
    err = lfsr_bd_flush(lfs, NULL, false);
    if (err) {
        return err;
    }

    // update the eoff and perturb
    rbyd->eoff
            = ((lfs_size_t)perturb << (8*sizeof(lfs_size_t)-1))
            | off_;
    // revert to canonical checksum
    rbyd->cksum = cksum;

    #ifdef LFS_DBGRBYDCOMMITS
    LFS_DEBUG("Committed rbyd 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "eoff %"PRId32", cksum %"PRIx32,
                rbyd->blocks[0], lfsr_rbyd_trunk(rbyd),
                rbyd->weight,
                (lfsr_rbyd_eoff(rbyd) >= lfs->cfg->block_size)
                    ? -1
                    : (lfs_ssize_t)lfsr_rbyd_eoff(rbyd),
                rbyd->cksum);
    #endif
    return 0;
}

static int lfsr_rbyd_appendcksum(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    // begin appending
    int err = lfsr_rbyd_appendinit(lfs, rbyd);
    if (err) {
        return err;
    }

    // append checksum stuff
    return lfsr_rbyd_appendcksum_(lfs, rbyd, rbyd->cksum);
}

static int lfsr_rbyd_appendrattrs(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    // append each tag to the tree
    for (lfs_size_t i = 0; i < rattr_count; i++) {
        // treat inserts after the first tag as though they are splits,
        // sequential inserts don't really make sense otherwise
        if (i > 0 && lfsr_rattr_isinsert(rattrs[i])) {
            rid += 1;
        }

        // don't write tags outside of the requested range
        if (rid >= start_rid
                // note the use of rid+1 and unsigned comparison here to
                // treat end_rid=-1 as "unbounded" in such a way that rid=-1
                // is still included
                && (lfs_size_t)(rid + 1) <= (lfs_size_t)end_rid) {
            int err = lfsr_rbyd_appendrattr(lfs, rbyd,
                    rid - lfs_smax(start_rid, 0),
                    rattrs[i]);
            if (err) {
                return err;
            }
        }

        // we need to make sure we keep start_rid/end_rid updated with
        // weight changes
        if (rid < start_rid) {
            start_rid += rattrs[i].weight;
        }
        if (rid < end_rid) {
            end_rid += rattrs[i].weight;
        }

        // adjust rid
        rid = lfsr_rattr_nextrid(rattrs[i], rid);
    }

    return 0;
}

static int lfsr_rbyd_commit(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    // append each tag to the tree
    int err = lfsr_rbyd_appendrattrs(lfs, rbyd, rid, -1, -1,
            rattrs, rattr_count);
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
    // TODO adopt this a/b naming scheme in lfsr_rbyd_appendrattr?
    lfsr_srid_t a_rid = start_rid;
    lfsr_srid_t b_rid = lfs_min(rbyd->weight, end_rid);
    lfs_size_t a_dsize = 0;
    lfs_size_t b_dsize = 0;
    lfs_size_t rbyd_dsize = 0;

    while (a_rid != b_rid) {
        if (a_dsize > b_dsize
                // bias so lower dsize >= upper dsize
                || (a_dsize == b_dsize && a_rid > b_rid)) {
            LFS_SWAP(lfsr_srid_t, &a_rid, &b_rid);
            LFS_SWAP(lfs_size_t, &a_dsize, &b_dsize);
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
            if (err < 0) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }
            if (rid_ > a_rid+lfs_smax(weight_-1, 0)) {
                break;
            }

            // keep track of rid and weight
            a_rid = rid_;
            weight += weight_;

            // include the cost of this tag
            dsize_ += lfs->rattr_estimate + lfsr_data_size(data);
        }

        if (a_rid == -1) {
            rbyd_dsize += dsize_;
        } else {
            a_dsize += dsize_;
        }

        if (a_rid < b_rid) {
            a_rid += 1;
        } else {
            a_rid -= lfs_smax(weight-1, 0);
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
// also note rattr.weight here is total weight not delta weight
static int lfsr_rbyd_appendcompactrattr(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_rattr_t rattr) {
    // begin appending
    int err = lfsr_rbyd_appendinit(lfs, rbyd);
    if (err) {
        return err;
    }

    // write the tag
    err = lfsr_rbyd_appendrattr_(lfs, rbyd, LFSR_RATTR_(
            (lfsr_rbyd_isshrub(rbyd) ? LFSR_TAG_SHRUB : 0) | rattr.tag,
            rattr.weight,
            rattr.u, rattr.count));
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
        err = lfsr_rbyd_appendcompactrattr(lfs, rbyd_, LFSR_RATTR_DATA(
                tag, weight, &data));
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_rbyd_appendcompaction(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_size_t off) {
    // begin appending
    int err = lfsr_rbyd_appendinit(lfs, rbyd);
    if (err) {
        return err;
    }

    // clamp offset to be after the revision count
    off = lfs_max(off, sizeof(uint32_t));

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
    lfsr_tag_t tag_ = 0;
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
                            &tag__, &weight__, &size__,
                            NULL);
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

                // connect with an altle/altgt
                //
                // note we need to use altles for all but the last tag
                // so we know the largest tag when building the next
                // layer, but for that last tag we need an altgt so
                // future appends maintain the balance of the tree
                err = lfsr_rbyd_appendtag(lfs, rbyd,
                        (off < layer_)
                            ? LFSR_TAG_ALT(
                                (i == 0) ? LFSR_TAG_R : LFSR_TAG_B,
                                LFSR_TAG_LE,
                                tag)
                            : LFSR_TAG_ALT(
                                LFSR_TAG_B,
                                LFSR_TAG_GT,
                                tag_),
                        weight,
                        lfsr_rbyd_eoff(rbyd) - trunk);
                if (err) {
                    return err;
                }

                // keep track of the previous tag for altgts
                tag_ = tag;
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
            shrub, -1, -1);
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
        lfsr_did_t did, const char *name, lfs_size_t name_len,
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
        if (err < 0) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // if we have no name, treat this rid as always lt
        if (lfsr_tag_suptype(tag__) != LFSR_TAG_NAME) {
            cmp = LFS_CMP_LT;

        // compare names
        } else {
            cmp = lfsr_data_namecmp(lfs, data__, did, name, name_len);
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

// create an empty btree
static void lfsr_btree_init(lfsr_btree_t *btree) {
    btree->weight = 0;
    btree->blocks[0] = -1;
    btree->trunk = 0;
}

// convenience operations
static inline int lfsr_btree_cmp(
        const lfsr_btree_t *a,
        const lfsr_btree_t *b) {
    return lfsr_rbyd_cmp(a, b);
}


// branch on-disk encoding
static lfsr_data_t lfsr_data_frombranch(const lfsr_rbyd_t *branch,
        uint8_t buffer[static LFSR_BRANCH_DSIZE]) {
    // block should not exceed 31-bits
    LFS_ASSERT(branch->blocks[0] <= 0x7fffffff);
    // trunk should not exceed 28-bits
    LFS_ASSERT(lfsr_rbyd_trunk(branch) <= 0x0fffffff);
    lfs_ssize_t d = 0;

    lfs_ssize_t d_ = lfs_toleb128(branch->blocks[0], &buffer[d], 5);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    d_ = lfs_toleb128(lfsr_rbyd_trunk(branch), &buffer[d], 4);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    lfs_tole32_(branch->cksum, &buffer[d]);
    d += 4;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readbranch(lfs_t *lfs,
        lfsr_data_t *data, lfsr_bid_t weight,
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

// needed in lfsr_branch_fetch
#ifdef LFS_CKFETCHES
static inline bool lfsr_m_isckfetches(uint32_t flags);
#endif

static int lfsr_branch_fetch(lfs_t *lfs, lfsr_rbyd_t *branch,
        lfs_block_t block, lfs_size_t trunk, lfsr_bid_t weight,
        uint32_t cksum) {
    (void)lfs;
    branch->blocks[0] = block;
    branch->trunk = trunk;
    branch->weight = weight;
    branch->eoff = 0;
    branch->cksum = cksum;

    #ifdef LFS_CKFETCHES
    // checking fetches?
    if (lfsr_m_isckfetches(lfs->flags)) {
        int err = lfsr_rbyd_fetchck(lfs, branch,
                branch->blocks[0], lfsr_rbyd_trunk(branch),
                branch->cksum);
        if (err) {
            return err;
        }
        LFS_ASSERT(branch->weight == weight);
    }
    #endif

    return 0;
}

static int lfsr_data_fetchbranch(lfs_t *lfs,
        lfsr_data_t *data, lfsr_bid_t weight,
        lfsr_rbyd_t *branch) {
    // decode branch and fetch
    int err = lfsr_data_readbranch(lfs, data, weight,
            branch);
    if (err) {
        return err;
    }

    return lfsr_branch_fetch(lfs, branch,
            branch->blocks[0], branch->trunk, branch->weight,
            branch->cksum);
}


// btree on-disk encoding
//
// this is the same as the branch on-disk econding, but prefixed with the
// btree's weight
static lfsr_data_t lfsr_data_frombtree(const lfsr_btree_t *btree,
        uint8_t buffer[static LFSR_BTREE_DSIZE]) {
    // weight should not exceed 31-bits
    LFS_ASSERT(btree->weight <= 0x7fffffff);
    lfs_ssize_t d = 0;

    lfs_ssize_t d_ = lfs_toleb128(btree->weight, &buffer[d], 5);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
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

static int lfsr_btree_fetch(lfs_t *lfs, lfsr_btree_t *btree,
        lfs_block_t block, lfs_size_t trunk, lfsr_bid_t weight,
        uint32_t cksum) {
    // btree/branch fetch really are the same once we know the weight
    int err = lfsr_branch_fetch(lfs, btree,
            block, trunk, weight,
            cksum);
    if (err) {
        return err;
    }

    #ifdef LFS_DBGBTREEFETCHES
    LFS_DEBUG("Fetched btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            btree->blocks[0], lfsr_rbyd_trunk(btree),
            btree->weight,
            btree->cksum);
    #endif
    return 0;
}

static int lfsr_data_fetchbtree(lfs_t *lfs, lfsr_data_t *data,
        lfsr_btree_t *btree) {
    // decode btree and fetch
    int err = lfsr_data_readbtree(lfs, data,
            btree);
    if (err) {
        return err;
    }

    return lfsr_btree_fetch(lfs, btree,
            btree->blocks[0], btree->trunk, btree->weight,
            btree->cksum);
}

// lookup rbyd/rid containing a given bid
static int lfsr_btree_lookupleaf(lfs_t *lfs, const lfsr_btree_t *btree,
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
            err = lfsr_rbyd_lookup(lfs, &branch, rid__,
                    LFSR_TAG_MASK8 | LFSR_TAG_STRUCT,
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
            err = lfsr_data_fetchbranch(lfs, &data__, weight__,
                    &branch);
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

// non-leaf lookups discard the rbyd info, which can be a bit more
// convenient, but may make commits more costly
static int lfsr_btree_lookupnext(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bid_t *weight_,
        lfsr_data_t *data_) {
    return lfsr_btree_lookupleaf(lfs, btree, bid,
            bid_, NULL, NULL, tag_, weight_, data_);
}

// lfsr_btree_lookup assumes a known bid, matching lfsr_rbyd_lookup's
// behavior, if you don't care about the exact bid either first call
// lfsr_btree_lookupnext, or lfsr_btree_lookupleaf + lfsr_rbyd_lookup
static int lfsr_btree_lookup(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_bid_t bid, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // lookup rbyd in btree
    lfsr_bid_t bid_;
    lfsr_rbyd_t rbyd_;
    lfsr_srid_t rid_;
    int err = lfsr_btree_lookupleaf(lfs, btree, bid,
            &bid_, &rbyd_, &rid_, NULL, NULL, NULL);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest bid, all we need to do is fail if it
    // picks up the wrong bid
    if (bid_ != bid) {
        return LFS_ERR_NOENT;
    }

    // lookup tag in rbyd
    return lfsr_rbyd_lookup(lfs, &rbyd_, rid_, tag,
            tag_, data_);
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
            err = lfsr_rbyd_lookup(lfs, &branch, rid__,
                    LFSR_TAG_MASK8 | LFSR_TAG_STRUCT,
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

        err = lfsr_branch_fetch(lfs, &branch_,
                branch_.blocks[0], branch_.trunk, branch_.weight,
                branch_.cksum);
        if (err) {
            return err;
        }

        branch = branch_;
    }
}


// extra state needed for non-terminating lfsr_btree_commit_ calls
typedef struct lfsr_bctx {
    lfsr_rattr_t rattrs[4];
    lfsr_data_t split_name;
    uint8_t buf[2*LFSR_BRANCH_DSIZE];
} lfsr_bctx_t;

// core btree algorithm
//
// this commits up to the root, but stops if:
// 1. we need a new root
// 2. we have a shrub root
//
static int lfsr_btree_commit_(lfs_t *lfs, lfsr_btree_t *btree,
        lfsr_bctx_t *bctx,
        lfsr_bid_t *bid,
        const lfsr_rattr_t **rattrs, lfs_size_t *rattr_count) {
    lfsr_bid_t bid_ = *bid;
    LFS_ASSERT(bid_ <= (lfsr_bid_t)btree->weight);
    const lfsr_rattr_t *rattrs_ = *rattrs;
    lfs_size_t rattr_count_ = *rattr_count;

    // lookup which leaf our bid resides
    //
    // for lfsr_btree_commit_ operations to work out, we need to
    // limit our bid to an rid in the tree, which is what this min
    // is doing
    lfsr_rbyd_t rbyd_ = *btree;
    lfsr_srid_t rid_ = bid_;
    if (btree->weight > 0) {
        lfsr_srid_t rid__;
        int err = lfsr_btree_lookupleaf(lfs, btree,
                lfs_min(bid_, btree->weight-1),
                &bid_, &rbyd_, &rid__, NULL, NULL, NULL);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // adjust rid
        rid_ -= (bid_-rid__);
    }

    // tail-recursively commit to btree
    while (true) {
        // we will always need our parent, so go ahead and find it
        lfsr_rbyd_t parent = {.trunk=0, .weight=0};
        lfsr_srid_t pid = 0;
        // are we root?
        if (!lfsr_rbyd_trunk(&rbyd_)
                || rbyd_.blocks[0] == btree->blocks[0]) {
            // new root? shrub root? yield the final root commit to
            // higher-level btree/bshrub logic
            if (!lfsr_rbyd_trunk(&rbyd_)
                    || lfsr_rbyd_isshrub(btree)) {
                *bid = rid_;
                *rattrs = rattrs_;
                *rattr_count = rattr_count_;
                return (!lfsr_rbyd_trunk(&rbyd_)) ? LFS_ERR_RANGE : 0;
            }

            // mark btree as unerased in case of failure, our btree rbyd and
            // root rbyd can diverge if there's a split, but we would have
            // marked the old root as unerased earlier anyways
            btree->eoff = -1;

        } else {
            int err = lfsr_btree_parent(lfs, btree, bid_, &rbyd_,
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
        if (!lfsr_rbyd_isfetched(&rbyd_)) {
            int err = lfsr_rbyd_fetchck(lfs, &rbyd_,
                    rbyd_.blocks[0], lfsr_rbyd_trunk(&rbyd_),
                    rbyd_.cksum);
            if (err) {
                return err;
            }
        }

        // is rbyd erased? can we sneak our commit into any remaining
        // erased bytes? note that the btree trunk field prevents this from
        // interacting with other references to the rbyd
        lfsr_rbyd_t rbyd__ = rbyd_;
        int err = lfsr_rbyd_commit(lfs, &rbyd__, rid_,
                rattrs_, rattr_count_);
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
        lfs_ssize_t estimate = lfsr_rbyd_estimate(lfs, &rbyd_, -1, -1,
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
                    err = lfsr_rbyd_lookup(lfs, &parent, sibling_rid,
                            LFSR_TAG_MASK8 | LFSR_TAG_STRUCT,
                            &sibling_tag, &sibling_data);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_NOENT);
                        return err;
                    }
                }

                LFS_ASSERT(sibling_tag == LFSR_TAG_BRANCH);
                err = lfsr_data_fetchbranch(lfs, &sibling_data, sibling_weight,
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
            if (pid-(lfsr_srid_t)rbyd_.weight >= 0) {
                // try looking up the sibling
                lfsr_srid_t sibling_rid;
                lfsr_tag_t sibling_tag;
                lfsr_rid_t sibling_weight;
                lfsr_data_t sibling_data;
                err = lfsr_rbyd_lookupnext(lfs, &parent,
                        pid-rbyd_.weight, LFSR_TAG_NAME,
                        &sibling_rid, &sibling_tag, &sibling_weight,
                        &sibling_data);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                if (sibling_tag == LFSR_TAG_NAME) {
                    err = lfsr_rbyd_lookup(lfs, &parent, sibling_rid,
                            LFSR_TAG_MASK8 | LFSR_TAG_STRUCT,
                            &sibling_tag, &sibling_data);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_NOENT);
                        return err;
                    }
                }

                LFS_ASSERT(sibling_tag == LFSR_TAG_BRANCH);
                err = lfsr_data_fetchbranch(lfs, &sibling_data, sibling_weight,
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
                    bid_ -= sibling.weight;
                    rid_ += sibling.weight;
                    pid -= rbyd_.weight;

                    rbyd__ = sibling;
                    sibling = rbyd_;
                    rbyd_ = rbyd__;

                    goto merge;
                }
            }
        }

    relocate:;
        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd__);
        if (err) {
            return err;
        }

        // try to compact
        err = lfsr_rbyd_compact(lfs, &rbyd__, &rbyd_, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // append any pending rattrs, it's up to upper
        // layers to make sure these always fit
        err = lfsr_rbyd_commit(lfs, &rbyd__, rid_,
                rattrs_, rattr_count_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        goto recurse;

    split:;
        // we should have something to split here
        LFS_ASSERT(split_rid > 0
                && split_rid < (lfsr_srid_t)rbyd_.weight);

    split_relocate_l:;
        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd__);
        if (err) {
            return err;
        }

        // copy over tags < split_rid
        err = lfsr_rbyd_compact(lfs, &rbyd__, &rbyd_, -1, split_rid);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

        // append pending rattrs < split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendrattrs(lfs, &rbyd__, rid_, -1, split_rid,
                rattrs_, rattr_count_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }

        // finalize commit
        err = lfsr_rbyd_appendcksum(lfs, &rbyd__);
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
        err = lfsr_rbyd_compact(lfs, &sibling, &rbyd_, split_rid, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }

        // append pending rattrs >= split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendrattrs(lfs, &sibling, rid_, split_rid, -1,
                rattrs_, rattr_count_);
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
        if (rbyd__.weight == 0 || sibling.weight == 0) {
            if (rbyd__.weight == 0) {
                rbyd__ = sibling;
            }
            goto recurse;
        }

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending rattrs in case
        // they introduce a new name!
        lfsr_tag_t split_tag;
        err = lfsr_rbyd_lookupnext(lfs, &sibling, 0, LFSR_TAG_NAME,
                NULL, &split_tag, NULL, &bctx->split_name);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // prepare commit to parent, tail recursing upwards
        LFS_ASSERT(rbyd__.weight > 0);
        LFS_ASSERT(sibling.weight > 0);
        rattr_count_ = 0;
        // new root?
        if (!lfsr_rbyd_trunk(&parent)) {
            lfsr_data_t branch_l = lfsr_data_frombranch(
                    &rbyd__, &bctx->buf[0*LFSR_BRANCH_DSIZE]);
            bctx->rattrs[rattr_count_++] = LFSR_RATTR_BUF(
                    LFSR_TAG_BRANCH, +rbyd__.weight,
                    branch_l.u.buffer, lfsr_data_size(branch_l));
            lfsr_data_t branch_r = lfsr_data_frombranch(
                    &sibling, &bctx->buf[1*LFSR_BRANCH_DSIZE]);
            bctx->rattrs[rattr_count_++] = LFSR_RATTR_BUF(
                    LFSR_TAG_BRANCH, +sibling.weight,
                    branch_r.u.buffer, lfsr_data_size(branch_r));
            if (lfsr_tag_suptype(split_tag) == LFSR_TAG_NAME) {
                bctx->rattrs[rattr_count_++] = LFSR_RATTR_DATA(
                        LFSR_TAG_NAME, 0,
                        &bctx->split_name);
            }
        // split root?
        } else {
            bid_ -= pid - (rbyd_.weight-1);
            lfsr_data_t branch_l = lfsr_data_frombranch(
                    &rbyd__, &bctx->buf[0*LFSR_BRANCH_DSIZE]);
            bctx->rattrs[rattr_count_++] = LFSR_RATTR_BUF(
                    LFSR_TAG_BRANCH, 0,
                    branch_l.u.buffer, lfsr_data_size(branch_l));
            if (rbyd__.weight != rbyd_.weight) {
                bctx->rattrs[rattr_count_++] = LFSR_RATTR(
                        LFSR_TAG_GROW, -rbyd_.weight + rbyd__.weight);
            }
            lfsr_data_t branch_r = lfsr_data_frombranch(
                    &sibling, &bctx->buf[1*LFSR_BRANCH_DSIZE]);
            bctx->rattrs[rattr_count_++] = LFSR_RATTR_BUF(
                    LFSR_TAG_BRANCH, +sibling.weight,
                    branch_r.u.buffer, lfsr_data_size(branch_r));
            if (lfsr_tag_suptype(split_tag) == LFSR_TAG_NAME) {
                bctx->rattrs[rattr_count_++] = LFSR_RATTR_DATA(
                        LFSR_TAG_NAME, 0,
                        &bctx->split_name);
            }
        }
        rattrs_ = bctx->rattrs;

        rbyd_ = parent;
        rid_ = pid;
        continue;

    merge:;
    merge_relocate:;
        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd__);
        if (err) {
            return err;
        }

        // merge the siblings together
        err = lfsr_rbyd_appendcompactrbyd(lfs, &rbyd__, &rbyd_, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        err = lfsr_rbyd_appendcompactrbyd(lfs, &rbyd__, &sibling, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        err = lfsr_rbyd_appendcompaction(lfs, &rbyd__, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }

        // append any pending rattrs, it's up to upper
        // layers to make sure these always fit
        err = lfsr_rbyd_commit(lfs, &rbyd__, rid_,
                rattrs_, rattr_count_);
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
        if (rbyd_.weight+sibling.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            *btree = rbyd__;
            *rattr_count = 0;
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        LFS_ASSERT(rbyd__.weight > 0);
        rattr_count_ = 0;
        // build attr list
        bid_ -= pid - (rbyd_.weight-1);
        bctx->rattrs[rattr_count_++] = LFSR_RATTR(
                LFSR_TAG_RM, -sibling.weight);
        lfsr_data_t branch = lfsr_data_frombranch(
                &rbyd__, &bctx->buf[0*LFSR_BRANCH_DSIZE]);
        bctx->rattrs[rattr_count_++] = LFSR_RATTR_BUF(
                LFSR_TAG_BRANCH, 0,
                branch.u.buffer, lfsr_data_size(branch));
        if (rbyd__.weight != rbyd_.weight) {
            bctx->rattrs[rattr_count_++] = LFSR_RATTR(
                    LFSR_TAG_GROW, -rbyd_.weight + rbyd__.weight);
        }
        rattrs_ = bctx->rattrs;

        rbyd_ = parent;
        rid_ = pid + sibling.weight;
        continue;

    recurse:;
        // done?
        if (!lfsr_rbyd_trunk(&parent)) {
            *btree = rbyd__;
            *rattr_count = 0;
            return 0;
        }

        // is our parent the root and is the root degenerate?
        if (rbyd_.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            *btree = rbyd__;
            *rattr_count = 0;
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        //
        // note that since we defer merges to compaction time, we can
        // end up removing an rbyd here
        rattr_count_ = 0;
        bid_ -= pid - (rbyd_.weight-1);
        if (rbyd__.weight == 0) {
            bctx->rattrs[rattr_count_++] = LFSR_RATTR(
                    LFSR_TAG_RM, -rbyd_.weight);
        } else {
            lfsr_data_t branch = lfsr_data_frombranch(
                    &rbyd__, &bctx->buf[0*LFSR_BRANCH_DSIZE]);
            bctx->rattrs[rattr_count_++] = LFSR_RATTR_BUF(
                    LFSR_TAG_BRANCH, 0,
                    branch.u.buffer, lfsr_data_size(branch));
            if (rbyd__.weight != rbyd_.weight) {
                bctx->rattrs[rattr_count_++] = LFSR_RATTR(
                        LFSR_TAG_GROW, -rbyd_.weight + rbyd__.weight);
            }
        }
        rattrs_ = bctx->rattrs;

        rbyd_ = parent;
        rid_ = pid;
        continue;
    }
}

// commit to a btree, this is atomic
static int lfsr_btree_commit(lfs_t *lfs, lfsr_btree_t *btree,
        lfsr_bid_t bid, const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    // try to commit to the btree
    lfsr_bctx_t bctx;
    int err = lfsr_btree_commit_(lfs, btree, &bctx,
            &bid, &rattrs, &rattr_count);
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }

    // needs a new root?
    if (err == LFS_ERR_RANGE) {
        LFS_ASSERT(rattr_count > 0);

    relocate:;
        lfsr_rbyd_t rbyd_;
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        err = lfsr_rbyd_commit(lfs, &rbyd_, bid, rattrs, rattr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        *btree = rbyd_;
    }

    LFS_ASSERT(lfsr_rbyd_trunk(btree));
    #ifdef LFS_DBGBTREECOMMITS
    LFS_DEBUG("Committed btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            btree->blocks[0], lfsr_rbyd_trunk(btree),
            btree->weight,
            btree->cksum);
    #endif
    return 0;
}

// lookup in a btree by name
static lfs_scmp_t lfsr_btree_namelookupleaf(lfs_t *lfs,
        const lfsr_btree_t *btree,
        lfsr_did_t did, const char *name, lfs_size_t name_len,
        lfsr_bid_t *bid_, lfsr_rbyd_t *rbyd_, lfsr_srid_t *rid_,
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
                did, name, name_len,
                &rid__, NULL, &weight__, NULL);
        if (cmp < 0) {
            LFS_ASSERT(cmp != LFS_ERR_NOENT);
            return cmp;
        }

        // the name may not match exactly, but indicates which branch to follow
        lfsr_tag_t tag__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookup(lfs, &branch, rid__,
                LFSR_TAG_MASK8 | LFSR_TAG_STRUCT,
                &tag__, &data__);
        if (err < 0) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // found another branch
        if (tag__ == LFSR_TAG_BRANCH) {
            // update our bid
            bid += rid__ - (weight__-1);

            // fetch the next branch
            err = lfsr_data_fetchbranch(lfs, &data__, weight__,
                    &branch);
            if (err < 0) {
                return err;
            }

        // found our rid
        } else {
            // TODO how many of these should be conditional?
            if (bid_) {
                *bid_ = bid + rid__;
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
            return cmp;
        }
    }
}

static lfs_scmp_t lfsr_btree_namelookup(lfs_t *lfs,
        const lfsr_btree_t *btree,
        lfsr_did_t did, const char *name, lfs_size_t name_len,
        lfsr_bid_t *bid_,
        lfsr_tag_t *tag_, lfsr_bid_t *weight_, lfsr_data_t *data_) {
    return lfsr_btree_namelookupleaf(lfs, btree,
            did, name, name_len,
            bid_, NULL, NULL, tag_, weight_, data_);
}

// incremental btree traversal
//
// note this is different from iteration, iteration should use
// lfsr_btree_lookupnext, traversal includes inner btree nodes

static void lfsr_btraversal_init(lfsr_btraversal_t *bt) {
    bt->bid = 0;
    bt->branch = NULL;
    bt->rid = 0;
}

static int lfsr_btree_traverse(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_btraversal_t *bt,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // explicitly traverse the root even if weight=0
    if (!bt->branch) {
        bt->branch = btree;
        bt->rid = bt->bid;

        // traverse the root
        if (bt->bid == 0
                // unless we don't even have a root yet
                && lfsr_rbyd_trunk(btree) != 0
                // or are a shrub
                && !lfsr_rbyd_isshrub(btree)) {
            if (bid_) {
                *bid_ = btree->weight-1;
            }
            if (tag_) {
                *tag_ = LFSR_TAG_BRANCH;
            }
            if (data_) {
                data_->u.buffer = (const uint8_t*)bt->branch;
            }
            return 0;
        }
    }

    // need to restart from the root?
    if (bt->rid >= (lfsr_srid_t)bt->branch->weight) {
        bt->branch = btree;
        bt->rid = bt->bid;
    }

    // descend down the tree
    while (true) {
        lfsr_srid_t rid__;
        lfsr_tag_t tag__;
        lfsr_rid_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, bt->branch, bt->rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_lookup(lfs, bt->branch, rid__,
                    LFSR_TAG_MASK8 | LFSR_TAG_STRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFSR_TAG_BRANCH) {
            // adjust rid with subtree's weight
            bt->rid -= (rid__ - (weight__-1));

            // fetch the next branch
            err = lfsr_data_fetchbranch(lfs, &data__, weight__,
                    &bt->rbyd);
            if (err) {
                return err;
            }
            bt->branch = &bt->rbyd;

            // return inner btree nodes if this is the first time we've
            // seen them
            if (bt->rid == 0) {
                if (bid_) {
                    *bid_ = bt->bid + (rid__ - bt->rid);
                }
                if (tag_) {
                    *tag_ = LFSR_TAG_BRANCH;
                }
                if (data_) {
                    data_->u.buffer = (const uint8_t*)bt->branch;
                }
                return 0;
            }

        // found our bid
        } else {
            // move on to the next rid
            //
            // note this effectively traverses a full leaf without redoing
            // the btree walk
            lfsr_bid_t bid__ = bt->bid + (rid__ - bt->rid);
            bt->bid = bid__ + 1;
            bt->rid = rid__ + 1;

            if (bid_) {
                *bid_ = bid__;
            }
            if (tag_) {
                *tag_ = tag__;
            }
            if (data_) {
                *data_ = data__;
            }
            return 0;
        }
    }
}



/// B-shrub operations ///

// shrub things

// helper functions
static inline bool lfsr_shrub_isshrub(const lfsr_shrub_t *shrub) {
    return lfsr_rbyd_isshrub(shrub);
}

static inline lfs_size_t lfsr_shrub_trunk(const lfsr_shrub_t *shrub) {
    return lfsr_rbyd_trunk(shrub);
}

static inline int lfsr_shrub_cmp(
        const lfsr_shrub_t *a,
        const lfsr_shrub_t *b) {
    return lfsr_rbyd_cmp(a, b);
}

// shrub on-disk encoding
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
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    d_ = lfs_toleb128(lfsr_shrub_trunk(shrub),
            &buffer[d], 4);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readshrub(lfs_t *lfs, lfsr_data_t *data,
        const lfsr_mdir_t *mdir,
        lfsr_shrub_t *shrub) {
    // copy the mdir block
    shrub->blocks[0] = mdir->rbyd.blocks[0];
    // force estimate recalculation if we write to this shrub
    shrub->eoff = -1;

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

// needed in lfsr_shrub_estimate
static inline bool lfsr_o_isbshrub(uint32_t flags);

// these are used in mdir commit/compaction
static lfs_ssize_t lfsr_shrub_estimate(lfs_t *lfs,
        const lfsr_shrub_t *shrub) {
    // only include the last reference
    const lfsr_shrub_t *last = NULL;
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_o_isbshrub(o->flags)
                && lfsr_shrub_cmp(
                    &((lfsr_bshrub_t*)o)->shrub,
                    shrub) == 0) {
            last = &((lfsr_bshrub_t*)o)->shrub;
        }
    }
    if (last && shrub != last) {
        return 0;
    }

    return lfsr_rbyd_estimate(lfs, shrub, -1, -1,
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
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_o_isbshrub(o->flags)
                && lfsr_shrub_cmp(
                    &((lfsr_bshrub_t*)o)->shrub,
                    shrub) == 0) {
            ((lfsr_bshrub_t*)o)->shrub_.blocks[0] = rbyd_->blocks[0];
            ((lfsr_bshrub_t*)o)->shrub_.trunk = rbyd_->trunk;
            ((lfsr_bshrub_t*)o)->shrub_.weight = rbyd_->weight;
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
typedef struct lfsr_shrubcommit {
    lfsr_bshrub_t *bshrub;
    lfsr_srid_t rid;
    const lfsr_rattr_t *rattrs;
    lfs_size_t rattr_count;
} lfsr_shrubcommit_t;

static int lfsr_shrub_commit(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        lfsr_shrub_t *shrub, lfsr_srid_t rid,
        const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
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
    int err = lfsr_rbyd_appendrattrs(lfs, rbyd_, rid, -1, -1,
            rattrs, rattr_count);
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


// ok, actual bshrub things

// create a non-existant bshrub
static void lfsr_bshrub_init(lfsr_bshrub_t *bshrub) {
    // set up a null bshrub
    bshrub->shrub.weight = 0;
    bshrub->shrub.blocks[0] = -1;
    bshrub->shrub.trunk = 0;
    // force estimate recalculation
    bshrub->shrub.eoff = -1;
}

static inline bool lfsr_bshrub_isbnull(const lfsr_bshrub_t *bshrub) {
    return !bshrub->shrub.trunk;
}

static inline bool lfsr_bshrub_isbshrub(const lfsr_bshrub_t *bshrub) {
    return lfsr_shrub_isshrub(&bshrub->shrub);
}

static inline bool lfsr_bshrub_isbtree(const lfsr_bshrub_t *bshrub) {
    return !lfsr_shrub_isshrub(&bshrub->shrub);
}

static inline int lfsr_bshrub_cmp(
        const lfsr_bshrub_t *a,
        const lfsr_bshrub_t *b) {
    return lfsr_rbyd_cmp(&a->shrub, &b->shrub);
}

// needed in lfsr_bshrub_estimate
static int lfsr_mdir_lookupnext(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_);

// find a tight upper bound on the _full_ bshrub size, this includes
// any on-disk bshrubs, and all pending bshrubs
static lfs_ssize_t lfsr_bshrub_estimate(lfs_t *lfs,
        const lfsr_bshrub_t *bshrub) {
    lfs_size_t estimate = 0;

    // include all unique shrubs related to our file, including the
    // on-disk shrub
    lfsr_tag_t tag;
    lfsr_data_t data;
    int err = lfsr_mdir_lookupnext(lfs, &bshrub->o.mdir, LFSR_TAG_DATA,
            &tag, &data);
    if (err < 0 && err != LFS_ERR_NOENT) {
        return err;
    }

    if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
        lfsr_shrub_t shrub;
        err = lfsr_data_readshrub(lfs, &data, &bshrub->o.mdir,
                &shrub);
        if (err < 0) {
            return err;
        }

        lfs_ssize_t dsize = lfsr_shrub_estimate(lfs, &shrub);
        if (dsize < 0) {
            return dsize;
        }
        estimate += dsize;
    }

    // this includes our current shrub
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_o_isbshrub(o->flags)
                && o->mdir.mid == bshrub->o.mdir.mid
                && lfsr_bshrub_isbshrub((lfsr_bshrub_t*)o)) {
            lfs_ssize_t dsize = lfsr_shrub_estimate(lfs,
                    &((lfsr_bshrub_t*)o)->shrub);
            if (dsize < 0) {
                return dsize;
            }
            estimate += dsize;
        }
    }

    return estimate;
}

// bshrub lookup functions
static int lfsr_bshrub_lookupleaf(lfs_t *lfs, const lfsr_bshrub_t *bshrub,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_rbyd_t *rbyd_, lfsr_srid_t *rid_,
        lfsr_tag_t *tag_, lfsr_bid_t *weight_, lfsr_data_t *data_) {
    return lfsr_btree_lookupleaf(lfs, &bshrub->shrub, bid,
            bid_, rbyd_, rid_, tag_, weight_, data_);
}

static int lfsr_bshrub_lookupnext(lfs_t *lfs, const lfsr_bshrub_t *bshrub,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bid_t *weight_,
        lfsr_data_t *data_) {
    return lfsr_btree_lookupnext(lfs, &bshrub->shrub, bid,
            bid_, tag_, weight_, data_);
}

static int lfsr_bshrub_lookup(lfs_t *lfs, const lfsr_bshrub_t *bshrub,
        lfsr_bid_t bid, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    return lfsr_btree_lookup(lfs, &bshrub->shrub, bid, tag,
            tag_, data_);
}

static int lfsr_bshrub_traverse(lfs_t *lfs, const lfsr_bshrub_t *bshrub,
        lfsr_btraversal_t *bt,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    return lfsr_btree_traverse(lfs, &bshrub->shrub, bt,
            bid_, tag_, data_);
}

// needed in lfsr_bshrub_commit
static int lfsr_mdir_commit(lfs_t *lfs, lfsr_mdir_t *mdir,
        const lfsr_rattr_t *rattrs, lfs_size_t rattr_count);

// commit to bshrub, this is atomic
static int lfsr_bshrub_commit(lfs_t *lfs, lfsr_bshrub_t *bshrub,
        lfsr_bid_t bid, const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    // before we touch anything, we need to mark all other btree references
    // as unerased
    if (lfsr_bshrub_isbtree(bshrub)) {
        for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
            if (lfsr_o_isbshrub(o->flags)
                    && (lfsr_bshrub_t*)o != bshrub
                    && lfsr_bshrub_cmp((lfsr_bshrub_t*)o, bshrub) == 0) {
                // mark as unerased
                ((lfsr_bshrub_t*)o)->shrub.eoff = -1;
            }
        }
    }

    // try to commit to the btree
    lfsr_bctx_t bctx;
    int err = lfsr_btree_commit_(lfs, &bshrub->shrub, &bctx,
            &bid, &rattrs, &rattr_count);
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }
    LFS_ASSERT(!err || rattr_count > 0);
    bool alloc = (err == LFS_ERR_RANGE);

    // when btree is shrubbed, lfsr_btree_commit_ stops at the root
    // and returns with pending rattrs
    if (rattr_count > 0) {
        // we need to prevent our shrub from overflowing our mdir somehow
        //
        // maintaining an accurate estimate is tricky and error-prone,
        // but recalculating an estimate every commit is expensive
        //
        // Instead, we keep track of an estimate of how many bytes have
        // been progged to the shrub since the last estimate, and recalculate
        // the estimate when this overflows our inline_size. This mirrors how
        // block_size and rbyds interact, and amortizes the estimate cost.

        // figure out how much data this commit progs
        lfs_size_t commit_estimate = 0;
        for (lfs_size_t i = 0; i < rattr_count; i++) {
            commit_estimate += lfs->rattr_estimate
                    + lfsr_rattr_dsize(rattrs[i]);
        }

        // does our estimate exceed our inline_size? need to recalculate an
        // accurate estimate
        lfs_ssize_t estimate = (alloc) ? (lfs_size_t)-1 : bshrub->shrub.eoff;
        // this double condition avoids overflow issues
        if ((lfs_size_t)estimate > lfs->cfg->inline_size
                || estimate + commit_estimate > lfs->cfg->inline_size) {
            estimate = lfsr_bshrub_estimate(lfs, bshrub);
            if (estimate < 0) {
                return estimate;
            }

            // two cases where we evict:
            // - overflow inline_size/2 - don't penalize for commits here
            // - overflow inline_size - must include commits or risk overflow
            //
            // the 1/2 here prevents runaway performance with the shrub is
            // near full, but it's a heuristic, so including the commit would
            // just be mean
            //
            if ((lfs_size_t)estimate > lfs->cfg->inline_size/2
                    || estimate + commit_estimate > lfs->cfg->inline_size) {
                goto relocate;
            }
        }

        // include our pending commit in the new estimate
        estimate += commit_estimate;

        // commit to shrub
        int err = lfsr_mdir_commit(lfs, &bshrub->o.mdir, LFSR_RATTRS(
                LFSR_RATTR_SHRUBCOMMIT(
                    (&(lfsr_shrubcommit_t){
                        .bshrub=bshrub,
                        .rid=bid,
                        .rattrs=rattrs,
                        .rattr_count=rattr_count}))));
        if (err) {
            return err;
        }
        LFS_ASSERT(bshrub->shrub.blocks[0] == bshrub->o.mdir.rbyd.blocks[0]);

        // update _all_ shrubs with the new estimate
        for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
            if (lfsr_o_isbshrub(o->flags)
                    && o->mdir.mid == bshrub->o.mdir.mid
                    && lfsr_bshrub_isbshrub((lfsr_bshrub_t*)o)) {
                ((lfsr_bshrub_t*)o)->shrub.eoff = estimate;
            }
        }
        LFS_ASSERT(bshrub->shrub.eoff == (lfs_size_t)estimate);
    }

    LFS_ASSERT(lfsr_shrub_trunk(&bshrub->shrub));
    #ifdef LFS_DBGBTREECOMMITS
    if (lfsr_bshrub_isbshrub(bshrub)) {
        LFS_DEBUG("Committed bshrub "
                    "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32,
                bshrub->o.mdir.rbyd.blocks[0], bshrub->o.mdir.rbyd.blocks[1],
                lfsr_shrub_trunk(&bshrub->shrub),
                bshrub->shrub.weight);
    } else {
        LFS_DEBUG("Committed btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                    "cksum %"PRIx32,
                bshrub->shrub.blocks[0], lfsr_shrub_trunk(&bshrub->shrub),
                bshrub->shrub.weight,
                bshrub->shrub.cksum);
    }
    #endif
    return 0;

relocate:;
    // convert to btree
    err = lfsr_rbyd_alloc(lfs, &bshrub->shrub_);
    if (err) {
        return err;
    }

    // note this may be a new root
    if (!alloc) {
        err = lfsr_rbyd_compact(lfs, &bshrub->shrub_, &bshrub->shrub, -1, -1);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }
    }

    err = lfsr_rbyd_commit(lfs, &bshrub->shrub_, bid, rattrs, rattr_count);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        // bad prog? try another block
        if (err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    bshrub->shrub = bshrub->shrub_;

    LFS_ASSERT(lfsr_rbyd_trunk(&bshrub->shrub));
    #ifdef LFS_DBGBTREECOMMITS
    LFS_DEBUG("Committed btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            bshrub->shrub.blocks[0], lfsr_shrub_trunk(&bshrub->shrub),
            bshrub->shrub.weight,
            bshrub->shrub.cksum);
    #endif
    return 0;
}




/// metadata-id things ///

#define LFSR_MID(_lfs, _bid, _rid) \
    (((_bid) & ~((1 << (_lfs)->mbits)-1)) + (_rid))

static inline lfsr_sbid_t lfsr_mbid(const lfs_t *lfs, lfsr_smid_t mid) {
    return mid | ((1 << lfs->mbits) - 1);
}

static inline lfsr_srid_t lfsr_mrid(const lfs_t *lfs, lfsr_smid_t mid) {
    // bit of a strange mapping, but we want to preserve mid=-1 => rid=-1
    return (mid >> (8*sizeof(lfsr_smid_t)-1))
            | (mid & ((1 << lfs->mbits) - 1));
}

// these should only be used for logging
static inline lfsr_sbid_t lfsr_dbgmbid(const lfs_t *lfs, lfsr_smid_t mid) {
    if (lfs->mtree.weight == 0) {
        return -1;
    } else {
        return mid >> lfs->mbits;
    }
}

static inline lfsr_srid_t lfsr_dbgmrid(const lfs_t *lfs, lfsr_smid_t mid) {
    return lfsr_mrid(lfs, mid);
}


/// metadata-pointer things ///

// the mroot anchor, mdir 0x{0,1} is the entry point into the filesystem
#define LFSR_MPTR_MROOTANCHOR() ((const lfs_block_t[2]){0, 1})

static inline int lfsr_mptr_cmp(
        const lfs_block_t a[static 2],
        const lfs_block_t b[static 2]) {
    // note these can be in either order
    if (lfs_max(a[0], a[1]) != lfs_max(b[0], b[1])) {
        return lfs_max(a[0], a[1]) - lfs_max(b[0], b[1]);
    } else {
        return lfs_min(a[0], a[1]) - lfs_min(b[0], b[1]);
    }
}

static inline bool lfsr_mptr_ismrootanchor(
        const lfs_block_t mptr[static 2]) {
    // mrootanchor is always at 0x{0,1}
    // just check that the first block is in mroot anchor range
    return mptr[0] <= 1;
}

// mptr on-disk encoding
static lfsr_data_t lfsr_data_frommptr(const lfs_block_t mptr[static 2],
        uint8_t buffer[static LFSR_MPTR_DSIZE]) {
    // blocks should not exceed 31-bits
    LFS_ASSERT(mptr[0] <= 0x7fffffff);
    LFS_ASSERT(mptr[1] <= 0x7fffffff);

    lfs_ssize_t d = 0;
    for (int i = 0; i < 2; i++) {
        lfs_ssize_t d_ = lfs_toleb128(mptr[i], &buffer[d], 5);
        if (d_ < 0) {
            LFS_UNREACHABLE();
        }
        d += d_;
    }

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readmptr(lfs_t *lfs, lfsr_data_t *data,
        lfs_block_t mptr[static 2]) {
    for (int i = 0; i < 2; i++) {
        int err = lfsr_data_readleb128(lfs, data, &mptr[i]);
        if (err) {
            return err;
        }
    }

    return 0;
}


/// various flag things ///

// open flags
static inline bool lfsr_o_isrdonly(uint32_t flags) {
    return (flags & LFS_O_MODE) == LFS_O_RDONLY;
}

static inline bool lfsr_o_iswronly(uint32_t flags) {
    return (flags & LFS_O_MODE) == LFS_O_WRONLY;
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

static inline bool lfsr_o_isflush(uint32_t flags) {
    return flags & LFS_O_FLUSH;
}

static inline bool lfsr_o_issync(uint32_t flags) {
    return flags & LFS_O_SYNC;
}

static inline bool lfsr_o_isdesync(uint32_t flags) {
    return flags & LFS_O_DESYNC;
}

// internal open flags
static inline uint8_t lfsr_o_type(uint32_t flags) {
    return flags >> 28;
}

static inline uint32_t lfsr_o_settype(uint32_t flags, uint8_t type) {
    return (flags & ~0xf0000000) | ((uint32_t)type << 28);
}

static inline bool lfsr_o_isbshrub(uint32_t flags) {
    // it turns out that bshrub types share a bit
    return flags & 0x10000000;
}

static inline bool lfsr_o_isunflush(uint32_t flags) {
    return flags & LFS_o_UNFLUSH;
}

static inline bool lfsr_o_isunsync(uint32_t flags) {
    return flags & LFS_o_UNSYNC;
}

static inline bool lfsr_o_isuncreat(uint32_t flags) {
    return flags & LFS_o_UNCREAT;
}

static inline bool lfsr_o_iszombie(uint32_t flags) {
    return flags & LFS_o_ZOMBIE;
}

// custom attr flags
static inline bool lfsr_a_islazy(uint32_t flags) {
    return flags & LFS_A_LAZY;
}

// traversal flags
static inline bool lfsr_t_ismtreeonly(uint32_t flags) {
    return flags & LFS_T_MTREEONLY;
}

static inline bool lfsr_t_ismkconsistent(uint32_t flags) {
    return flags & LFS_T_MKCONSISTENT;
}

static inline bool lfsr_t_islookahead(uint32_t flags) {
    return flags & LFS_T_LOOKAHEAD;
}

static inline bool lfsr_t_iscompact(uint32_t flags) {
    return flags & LFS_T_COMPACT;
}

static inline bool lfsr_t_isckmeta(uint32_t flags) {
    return flags & LFS_T_CKMETA;
}

static inline bool lfsr_t_isckdata(uint32_t flags) {
    return flags & LFS_T_CKDATA;
}

// internal traversal flags
static inline uint8_t lfsr_t_tstate(uint32_t flags) {
    return (flags >> 0) & 0xf;
}

static inline uint32_t lfsr_t_settstate(uint32_t flags, uint8_t tstate) {
    return (flags & ~0x0000000f) | (tstate << 0);
}

static inline uint8_t lfsr_t_btype(uint32_t flags) {
    return (flags >> 8) & 0x0f;
}

static inline uint32_t lfsr_t_setbtype(uint32_t flags, uint8_t btype) {
    return (flags & ~0x00000f00) | (btype << 8);
}

static inline bool lfsr_t_isdirty(uint32_t flags) {
    return flags & LFS_t_DIRTY;
}

static inline bool lfsr_t_ismutated(uint32_t flags) {
    return flags & LFS_t_MUTATED;
}

static inline uint32_t lfsr_t_swapdirty(uint32_t flags) {
    uint32_t x = ((flags >> 25) ^ (flags >> 24)) & 0x1;
    return flags ^ (x << 25) ^ (x << 24);
}

// mount flags
static inline bool lfsr_m_isrdonly(uint32_t flags) {
    return flags & LFS_M_RDONLY;
}

#ifdef LFS_NOISY
static inline bool lfsr_m_isnoisy(uint32_t flags) {
    return flags & LFS_M_NOISY;
}
#endif

#ifdef LFS_CKPROGS
static inline bool lfsr_m_isckprogs(uint32_t flags) {
    return flags & LFS_M_CKPROGS;
}
#endif

#ifdef LFS_CKFETCHES
static inline bool lfsr_m_isckfetches(uint32_t flags) {
    return flags & LFS_M_CKFETCHES;
}
#endif

#ifdef LFS_CKPARITY
static inline bool lfsr_m_isckparity(uint32_t flags) {
    return flags & LFS_M_CKPARITY;
}
#endif

#ifdef LFS_CKDATACKSUMS
static inline bool lfsr_m_isckdatacksums(uint32_t flags) {
    return flags & LFS_M_CKDATACKSUMS;
}
#endif



/// opened mdir things ///

// we maintain a linked-list of all opened mdirs, in order to keep
// metadata state in-sync, these may be casted to specific file types

static bool lfsr_omdir_isopen(lfs_t *lfs, const lfsr_omdir_t *o) {
    for (lfsr_omdir_t *o_ = lfs->omdirs; o_; o_ = o_->next) {
        if (o_ == o) {
            return true;
        }
    }

    return false;
}

static void lfsr_omdir_open(lfs_t *lfs, lfsr_omdir_t *o) {
    LFS_ASSERT(!lfsr_omdir_isopen(lfs, o));
    // add to opened list
    o->next = lfs->omdirs;
    lfs->omdirs = o;
}

// needed in lfsr_omdir_close
static void lfsr_omdir_clobber(lfs_t *lfs, const lfsr_omdir_t *o,
        bool dirty);

static void lfsr_omdir_close(lfs_t *lfs, lfsr_omdir_t *o) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, o));
    // make sure we're not entangled in any traversals, note we don't
    // set the dirty bit here
    lfsr_omdir_clobber(lfs, o, false);
    // remove from opened list
    for (lfsr_omdir_t **o_ = &lfs->omdirs; *o_; o_ = &(*o_)->next) {
        if (*o_ == o) {
            *o_ = (*o_)->next;
            break;
        }
    }
}

// check if a given mid is open
static bool lfsr_omdir_ismidopen(lfs_t *lfs, lfsr_smid_t mid, uint32_t mask) {
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        // we really only care about regular open files here, all
        // others are either transient (dirs) or fake (orphans)
        if (lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == mid
                // allow caller to ignore files with specific flags
                && !(o->flags & ~mask)) {
            return true;
        }
    }

    return false;
}

// traversal invalidation things

// needed in lfsr_omdir_clobber
static void lfsr_traversal_clobber(lfs_t *lfs, lfsr_traversal_t *t);

// clobber any traversals referencing our mdir
static void lfsr_omdir_clobber(lfs_t *lfs, const lfsr_omdir_t *o,
        bool dirty) {
    for (lfsr_omdir_t *o_ = lfs->omdirs; o_; o_ = o_->next) {
        if (lfsr_o_type(o_->flags) == LFS_TYPE_TRAVERSAL) {
            o_->flags |= (dirty) ? LFS_t_DIRTY : 0;

            if (o && ((lfsr_traversal_t*)o_)->ot == o) {
                lfsr_traversal_clobber(lfs, (lfsr_traversal_t*)o_);
            }
        }
    }
}

// clobber and mark traversals as dirty
static void lfsr_omdir_mkdirty(lfs_t *lfs, const lfsr_omdir_t *o) {
    lfsr_omdir_clobber(lfs, o, true);
}

// mark all traversals as dirty
static void lfsr_fs_mkdirty(lfs_t *lfs) {
    lfsr_omdir_clobber(lfs, NULL, true);
}



/// Global-state things ///

// grm (global remove) things
static inline uint8_t lfsr_grm_count_(const lfsr_grm_t *grm) {
    return (grm->mids[0] >= 0) + (grm->mids[1] >= 0);
}

static inline uint8_t lfsr_grm_count(const lfs_t *lfs) {
    return lfsr_grm_count_(&lfs->grm);
}

static inline void lfsr_grm_push(lfs_t *lfs, lfsr_smid_t mid) {
    LFS_ASSERT(lfs->grm.mids[1] == -1);
    lfs->grm.mids[1] = lfs->grm.mids[0];
    lfs->grm.mids[0] = mid;
}

static inline lfsr_smid_t lfsr_grm_pop(lfs_t *lfs) {
    lfsr_smid_t mid = lfs->grm.mids[0];
    lfs->grm.mids[0] = lfs->grm.mids[1];
    lfs->grm.mids[1] = -1;
    return mid;
}

static inline bool lfsr_grm_ismidrm(const lfs_t *lfs, lfsr_smid_t mid) {
    return lfs->grm.mids[0] == mid || lfs->grm.mids[1] == mid;
}

#define LFSR_DATA_GRM(_grm, _buffer) \
    ((struct {lfsr_data_t d;}){lfsr_data_fromgrm(_grm, _buffer)}.d)

static lfsr_data_t lfsr_data_fromgrm(const lfsr_grm_t *grm,
        uint8_t buffer[static LFSR_GRM_DSIZE]) {
    // make sure to zero so we don't leak any info
    lfs_memset(buffer, 0, LFSR_GRM_DSIZE);

    // first encode the number of grms, this can be 0, 1, or 2 and may
    // be extended to a general purpose leb128 type field in the future
    uint8_t mode = lfsr_grm_count_(grm);
    lfs_ssize_t d = 0;
    buffer[d] = mode;
    d += 1;

    for (uint8_t i = 0; i < mode; i++) {
        lfs_ssize_t d_ = lfs_toleb128(grm->mids[i], &buffer[d], 5);
        if (d_ < 0) {
            LFS_UNREACHABLE();
        }
        d += d_;
    }

    return LFSR_DATA_BUF(buffer, lfs_memlen(buffer, LFSR_GRM_DSIZE));
}

// required by lfsr_data_readgrm
static inline lfsr_mid_t lfsr_mtree_weight(lfs_t *lfs);

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
        return LFS_ERR_CORRUPT;
    }

    for (uint8_t i = 0; i < mode; i++) {
        int err = lfsr_data_readleb128(lfs, data, (lfsr_mid_t*)&grm->mids[i]);
        if (err) {
            return err;
        }
        LFS_ASSERT((lfsr_mid_t)grm->mids[i] < lfsr_mtree_weight(lfs));
    }

    return 0;
}


// some mdir-related gstate things we need
static void lfsr_fs_flushgdelta(lfs_t *lfs) {
    // zero any pending gdeltas
    lfs->gcksum_d = 0;

    lfs_memset(lfs->grm_d, 0, LFSR_GRM_DSIZE);
}

static void lfsr_fs_commitgdelta(lfs_t *lfs) {
    // commit any pending gdeltas
    lfs->gcksum_p = lfs->gcksum;

    lfsr_data_fromgrm(&lfs->grm, lfs->grm_p);
}

static void lfsr_fs_revertgdelta(lfs_t *lfs) {
    // revert gstate to on-disk state
    lfs->gcksum = lfs->gcksum_p;

    int err = lfsr_data_readgrm(lfs,
            &LFSR_DATA_BUF(lfs->grm_p, LFSR_GRM_DSIZE),
            &lfs->grm);
    if (err) {
        LFS_UNREACHABLE();
    }
}

// append and consume any pending gstate
static int lfsr_rbyd_appendgdelta(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    // note gcksums are a special case and handled directly in
    // lfsr_mdir_commit__/lfsr_rbyd_appendcksum_

    // pending grm state?
    uint8_t grmdelta_[LFSR_GRM_DSIZE];
    lfsr_data_fromgrm(&lfs->grm, grmdelta_);
    lfs_memxor(grmdelta_, lfs->grm_p, LFSR_GRM_DSIZE);
    lfs_memxor(grmdelta_, lfs->grm_d, LFSR_GRM_DSIZE);

    if (lfs_memlen(grmdelta_, LFSR_GRM_DSIZE) != 0) {
        // make sure to xor any existing delta
        lfsr_data_t data;
        int err = lfsr_rbyd_lookup(lfs, rbyd, -1, LFSR_TAG_GRMDELTA,
                NULL, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        uint8_t grmdelta[LFSR_GRM_DSIZE];
        lfs_memset(grmdelta, 0, LFSR_GRM_DSIZE);
        if (err != LFS_ERR_NOENT) {
            lfs_ssize_t d = lfsr_data_read(lfs, &data,
                    grmdelta, LFSR_GRM_DSIZE);
            if (d < 0) {
                return d;
            }
        }

        lfs_memxor(grmdelta_, grmdelta, LFSR_GRM_DSIZE);

        // append to our rbyd, replacing any existing delta
        lfs_size_t size = lfs_memlen(grmdelta_, LFSR_GRM_DSIZE);
        err = lfsr_rbyd_appendrattr(lfs, rbyd, -1, LFSR_RATTR_BUF(
                // opportunistically remove this tag if delta is all zero
                (size == 0)
                    ? LFSR_TAG_RM | LFSR_TAG_GRMDELTA
                    : LFSR_TAG_GRMDELTA, 0,
                grmdelta_, size));
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_fs_consumegdelta(lfs_t *lfs, const lfsr_mdir_t *mdir) {
    // consume any gcksum deltas
    lfs->gcksum_d ^= mdir->gcksumdelta;

    // consume any grm deltas
    lfsr_data_t data;
    int err = lfsr_rbyd_lookup(lfs, &mdir->rbyd, -1, LFSR_TAG_GRMDELTA,
            NULL, &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    if (err != LFS_ERR_NOENT) {
        uint8_t grmdelta[LFSR_GRM_DSIZE];
        lfs_ssize_t d = lfsr_data_read(lfs, &data, grmdelta, LFSR_GRM_DSIZE);
        if (d < 0) {
            return d;
        }

        lfs_memxor(lfs->grm_d, grmdelta, d);
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
//                            '---------- pseudorandom noise (optional)

static inline uint32_t lfsr_rev_init(lfs_t *lfs, uint32_t rev) {
    (void)lfs;
    // we really only care about the top revision bits here
    rev &= ~((1 << 28)-1);
    // increment revision
    rev += 1 << 28;
    // xor in pseudorandom noise
    #ifdef LFS_NOISY
    if (lfsr_m_isnoisy(lfs->flags)) {
        rev ^= ((1 << (28-lfs_smax(lfs->recycle_bits, 0)))-1) & lfs->gcksum;
    }
    #endif
    return rev;
}

static inline bool lfsr_rev_needsrelocation(lfs_t *lfs, uint32_t rev) {
    if (lfs->recycle_bits == -1) {
        return false;
    }

    // does out recycle counter overflow?
    uint32_t rev_ = rev + (1 << (28-lfs_smax(lfs->recycle_bits, 0)));
    return (rev_ >> 28) != (rev >> 28);
}

static inline uint32_t lfsr_rev_inc(lfs_t *lfs, uint32_t rev) {
    // increment recycle counter/revision
    rev += 1 << (28-lfs_smax(lfs->recycle_bits, 0));
    // xor in pseudorandom noise
    #ifdef LFS_NOISY
    if (lfsr_m_isnoisy(lfs->flags)) {
        rev ^= ((1 << (28-lfs_smax(lfs->recycle_bits, 0)))-1) & lfs->gcksum;
    }
    #endif
    return rev;
}



/// Metadata pair stuff ///

// mdir convenience functions
static inline int lfsr_mdir_cmp(const lfsr_mdir_t *a, const lfsr_mdir_t *b) {
    return lfsr_mptr_cmp(a->rbyd.blocks, b->rbyd.blocks);
}

static inline bool lfsr_mdir_ismrootanchor(const lfsr_mdir_t *mdir) {
    return lfsr_mptr_ismrootanchor(mdir->rbyd.blocks);
}

static inline void lfsr_mdir_sync(lfsr_mdir_t *a, const lfsr_mdir_t *b) {
    // copy over everything but the mid
    a->rbyd = b->rbyd;
    a->gcksumdelta = b->gcksumdelta;
}

// mdir operations
static int lfsr_mdir_fetch(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfsr_smid_t mid, const lfs_block_t mptr[static 2]) {
    // create a copy of the mptr, both so we can swap the blocks to keep
    // track of the current revision, and to prevents issues if mptr
    // references the blocks in the mdir
    lfs_block_t blocks[2] = {mptr[0], mptr[1]};
    // read both revision counts, try to figure out which block
    // has the most recent revision
    uint32_t revs[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        int err = lfsr_bd_read(lfs, blocks[0], 0, 0,
                &revs[0], sizeof(uint32_t));
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        revs[i] = lfs_fromle32_(&revs[i]);

        if (i == 0
                || err == LFS_ERR_CORRUPT
                || lfs_scmp(revs[1], revs[0]) > 0) {
            LFS_SWAP(lfs_block_t, &blocks[0], &blocks[1]);
            LFS_SWAP(uint32_t, &revs[0], &revs[1]);
        }
    }

    // try to fetch rbyds in the order of most recent to least recent
    for (int i = 0; i < 2; i++) {
        int err = lfsr_rbyd_fetch_(lfs,
                &mdir->rbyd, &mdir->gcksumdelta,
                blocks[0], 0);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        if (err != LFS_ERR_CORRUPT) {
            mdir->mid = mid;
            // keep track of other block for compactions
            mdir->rbyd.blocks[1] = blocks[1];
            #ifdef LFS_DBGMDIRFETCHES
            LFS_DEBUG("Fetched mdir %"PRId32" "
                        "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32", "
                        "cksum %"PRIx32,
                    lfsr_dbgmbid(lfs, mdir->mid),
                    mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
                    lfsr_rbyd_trunk(&mdir->rbyd),
                    mdir->rbyd.weight,
                    mdir->rbyd.cksum);
            #endif
            return 0;
        }

        LFS_SWAP(lfs_block_t, &blocks[0], &blocks[1]);
        LFS_SWAP(uint32_t, &revs[0], &revs[1]);
    }

    // could not find a non-corrupt rbyd
    return LFS_ERR_CORRUPT;
}

static int lfsr_data_fetchmdir(lfs_t *lfs,
        lfsr_data_t *data, lfsr_smid_t mid,
        lfsr_mdir_t *mdir) {
    // decode mptr and fetch
    int err = lfsr_data_readmptr(lfs, data,
            mdir->rbyd.blocks);
    if (err) {
        return err;
    }

    return lfsr_mdir_fetch(lfs, mdir, mid, mdir->rbyd.blocks);
}

static int lfsr_mdir_lookupnext(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_srid_t rid__;
    lfsr_tag_t tag__;
    int err = lfsr_rbyd_lookupnext(lfs, &mdir->rbyd,
            lfsr_mrid(lfs, mdir->mid), tag,
            &rid__, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // this is very similar to lfsr_rbyd_lookupnext, but we error if
    // lookupnext would change mids
    if (rid__ != lfsr_mrid(lfs, mdir->mid)) {
        return LFS_ERR_NOENT;
    }

    // intercept pending grms here and pretend they're orphaned
    // stickynotes
    //
    // fortunately pending grms/orphaned stickynotes have roughly the
    // same semantics, and it's easier to manage the implied mid gap in
    // higher-levels
    if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME
            && lfsr_grm_ismidrm(lfs, mdir->mid)) {
        tag__ = LFSR_TAG_STICKYNOTE;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}

static int lfsr_mdir_lookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_tag_t tag__;
    int err = lfsr_mdir_lookupnext(lfs, mdir, lfsr_tag_key(tag),
            &tag__, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if ((tag__ & lfsr_tag_mask(tag)) != (tag & lfsr_tag_mask(tag))) {
        return LFS_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}



/// Metadata-tree things ///

static inline lfsr_mid_t lfsr_mtree_weight(lfs_t *lfs) {
    return lfs_max(
            lfs->mtree.weight,
            1 << lfs->mbits);
}

// lookup mdir containing a given mid
static int lfsr_mtree_lookupleaf(lfs_t *lfs, lfsr_smid_t mid,
        lfsr_mdir_t *mdir_) {
    // looking up mid=-1 is probably a mistake
    LFS_ASSERT(mid >= 0);

    // out of bounds?
    if ((lfsr_mid_t)mid >= lfsr_mtree_weight(lfs)) {
        return LFS_ERR_NOENT;
    }

    // looking up mroot?
    lfsr_mdir_t mdir;
    if (lfs->mtree.weight == 0) {
        // treat inlined mdir as mid=0
        mdir.mid = mid;
        lfsr_mdir_sync(&mdir, &lfs->mroot);

    // look up mdir in actual mtree
    } else {
        lfsr_bid_t bid;
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_btree_lookupnext(lfs, &lfs->mtree, mid,
                &bid, &tag, NULL, &data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }
        LFS_ASSERT((lfsr_sbid_t)bid == lfsr_mbid(lfs, mid));
        LFS_ASSERT(tag == LFSR_TAG_MDIR);

        // fetch mdir
        err = lfsr_data_fetchmdir(lfs, &data, mid,
                &mdir);
        if (err) {
            return err;
        }
    }

    if (mdir_) {
        *mdir_ = mdir;
    }
    return 0;
}

// in-mdir lookups for convenience/possible code sharing
static int lfsr_mtree_lookupnext(lfs_t *lfs, lfsr_smid_t mid, lfsr_tag_t tag,
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_mdir_t mdir;
    int err = lfsr_mtree_lookupleaf(lfs, mid,
            &mdir);
    if (err) {
        return err;
    }

    err = lfsr_mdir_lookupnext(lfs, &mdir, tag,
            tag_, data_);
    if (err) {
        return err;
    }

    if (mdir_) {
        *mdir_ = mdir;
    }
    return 0;
}

static int lfsr_mtree_lookup(lfs_t *lfs, lfsr_smid_t mid, lfsr_tag_t tag,
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_mdir_t mdir;
    int err = lfsr_mtree_lookupleaf(lfs, mid,
            &mdir);
    if (err) {
        return err;
    }

    err = lfsr_mdir_lookup(lfs, &mdir, tag,
            tag_, data_);
    if (err) {
        return err;
    }

    if (mdir_) {
        *mdir_ = mdir;
    }
    return 0;
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
        lfsr_smid_t mid, bool partial) {
    // assign the mid
    mdir->mid = mid;
    // default to zero gcksumdelta
    mdir->gcksumdelta = 0;

    if (!partial) {
        // allocate one block without an erase
        lfs_sblock_t block = lfs_alloc(lfs, false);
        if (block < 0) {
            return block;
        }
        mdir->rbyd.blocks[1] = block;
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
    lfs_sblock_t block = lfs_alloc(lfs, true);
    if (block < 0) {
        return block;
    }
    mdir->rbyd.blocks[0] = block;
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
    // reset to zero gcksumdelta, upper layers should handle this
    mdir_->gcksumdelta = 0;

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
        lfsr_smid_t mid, const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    // since we only ever commit to one mid or split, we can ignore the
    // entire rattr-list if our mid is out of range
    lfsr_srid_t rid = lfsr_mrid(lfs, mid);
    if (rid >= start_rid
            // note the use of rid+1 and unsigned comparison here to
            // treat end_rid=-1 as "unbounded" in such a way that rid=-1
            // is still included
            && (lfs_size_t)(rid + 1) <= (lfs_size_t)end_rid) {

        for (lfs_size_t i = 0; i < rattr_count; i++) {
            // we just happen to never split in an mdir commit
            LFS_ASSERT(!(i > 0 && lfsr_rattr_isinsert(rattrs[i])));

            // rattr lists can be chained, but only tail-recursively
            if (rattrs[i].tag == LFSR_TAG_RATTRS) {
                // must be the last tag
                LFS_ASSERT(i == rattr_count-1);
                const lfsr_rattr_t *rattrs_ = rattrs[i].u.etc;
                lfs_size_t rattr_count_ = rattrs[i].count;

                // switch to chained rattr-list
                rattrs = rattrs_;
                rattr_count = rattr_count_;
                i = -1;
                continue;

            // shrub tags append a set of attributes to an unrelated trunk
            // in our rbyd
            } else if (rattrs[i].tag == LFSR_TAG_SHRUBCOMMIT) {
                const lfsr_shrubcommit_t *shrubcommit = rattrs[i].u.etc;
                lfsr_bshrub_t *bshrub_ = shrubcommit->bshrub;
                lfsr_srid_t rid_ = shrubcommit->rid;
                const lfsr_rattr_t *rattrs_ = shrubcommit->rattrs;
                lfs_size_t rattr_count_ = shrubcommit->rattr_count;

                // reset shrub if it doesn't live in our block, this happens
                // when converting from a btree
                if (!lfsr_bshrub_isbshrub(bshrub_)) {
                    bshrub_->shrub_.blocks[0] = mdir->rbyd.blocks[0];
                    bshrub_->shrub_.trunk = LFSR_RBYD_ISSHRUB | 0;
                    bshrub_->shrub_.weight = 0;
                }

                // commit to shrub
                int err = lfsr_shrub_commit(lfs,
                        &mdir->rbyd, &bshrub_->shrub_,
                        rid_, rattrs_, rattr_count_);
                if (err) {
                    return err;
                }

            // move tags copy over any tags associated with the source's rid
            // TODO can this be deduplicated with lfsr_mdir_compact__ more?
            // it _really_ wants to be deduplicated
            } else if (rattrs[i].tag == LFSR_TAG_MOVE) {
                const lfsr_mdir_t *mdir__ = rattrs[i].u.etc;

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

                    // found an inlined shrub? we need to compact the shrub
                    // as well to bring it along with us
                    if (tag == LFSR_TAG_BSHRUB) {
                        lfsr_shrub_t shrub;
                        err = lfsr_data_readshrub(lfs, &data, mdir__,
                                &shrub);
                        if (err) {
                            return err;
                        }

                        // compact our shrub
                        err = lfsr_shrub_compact(lfs, &mdir->rbyd, &shrub,
                                &shrub);
                        if (err) {
                            return err;
                        }

                        // write our new shrub tag
                        err = lfsr_rbyd_appendrattr(lfs, &mdir->rbyd,
                                rid - lfs_smax(start_rid, 0),
                                LFSR_RATTR_SHRUB(LFSR_TAG_BSHRUB, 0, &shrub));
                        if (err) {
                            return err;
                        }

                    // append the rattr
                    } else {
                        err = lfsr_rbyd_appendrattr(lfs, &mdir->rbyd,
                                rid - lfs_smax(start_rid, 0),
                                LFSR_RATTR_DATA(tag, 0, &data));
                        if (err) {
                            return err;
                        }
                    }
                }

                // we're not quite done! we also need to bring over any
                // unsynced files
                for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
                    if (lfsr_o_isbshrub(o->flags)
                            // belongs to our mid?
                            && o->mdir.mid == mdir__->mid
                            // is a bshrub?
                            && lfsr_bshrub_isbshrub((lfsr_bshrub_t*)o)
                            // only compact once, first compact should
                            // stage the new block
                            && ((lfsr_bshrub_t*)o)->shrub_.blocks[0]
                                != mdir->rbyd.blocks[0]) {
                        int err = lfsr_shrub_compact(lfs, &mdir->rbyd,
                                &((lfsr_bshrub_t*)o)->shrub_,
                                &((lfsr_bshrub_t*)o)->shrub);
                        if (err) {
                            return err;
                        }
                    }
                }

            // custom attributes need to be reencoded into our tag format
            } else if (lfsr_tag_key(rattrs[i].tag) == LFSR_TAG_ATTRS) {
                const struct lfs_attr *attrs_ = rattrs[i].u.etc;
                lfs_size_t attr_count_ = rattrs[i].count;

                for (lfs_size_t j = 0; j < attr_count_; j++) {
                    // skip readonly attrs and lazy attrs
                    if (lfsr_o_isrdonly(attrs_[j].flags)) {
                        continue;
                    }

                    // first lets check if the attr changed, we don't want
                    // to append attrs unless we have to
                    lfsr_data_t data;
                    int err = lfsr_mdir_lookup(lfs, mdir,
                            LFSR_TAG_ATTR(attrs_[j].type),
                            NULL, &data);
                    if (err && err != LFS_ERR_NOENT) {
                        return err;
                    }

                    // does disk match our attr?
                    lfs_scmp_t cmp = lfsr_attr_cmp(lfs, &attrs_[j],
                            (err != LFS_ERR_NOENT) ? &data : NULL);
                    if (cmp < 0) {
                        return cmp;
                    }

                    if (cmp == LFS_CMP_EQ) {
                        continue;
                    }

                    // append the custom attr
                    err = lfsr_rbyd_appendrattr(lfs, &mdir->rbyd,
                            rid - lfs_smax(start_rid, 0),
                            // removing or updating?
                            (lfsr_attr_isnoattr(&attrs_[j]))
                                ? LFSR_RATTR(
                                    LFSR_TAG_RM
                                        | LFSR_TAG_ATTR(attrs_[j].type), 0)
                                : LFSR_RATTR_BUF(
                                    LFSR_TAG_ATTR(attrs_[j].type), 0,
                                    attrs_[j].buffer,
                                    lfsr_attr_size(&attrs_[j])));
                    if (err) {
                        return err;
                    }
                }

            // write out normal tags normally
            } else {
                LFS_ASSERT(!lfsr_tag_isinternal(rattrs[i].tag));

                int err = lfsr_rbyd_appendrattr(lfs, &mdir->rbyd,
                        rid - lfs_smax(start_rid, 0),
                        rattrs[i]);
                if (err) {
                    return err;
                }
            }

            // adjust rid
            rid = lfsr_rattr_nextrid(rattrs[i], rid);
        }
    }

    // abort the commit if our weight dropped to zero!
    //
    // If we finish the commit it becomes immediately visible, but we really
    // need to atomically remove this mdir from the mtree. Leave the actual
    // remove up to upper layers.
    if (mdir->rbyd.weight == 0
            // unless we are an mroot
            && !(mdir->mid == -1 || lfsr_mdir_cmp(mdir, &lfs->mroot) == 0)) {
        // note! we can no longer read from this mdir as our pcache may
        // be clobbered
        return LFS_ERR_NOENT;
    }

    // append any gstate?
    if (start_rid <= -2) {
        int err = lfsr_rbyd_appendgdelta(lfs, &mdir->rbyd);
        if (err) {
            return err;
        }
    }

    // save our canonical cksum
    //
    // note this is before we calculate gcksumdelta, otherwise
    // everything would get all self-referential
    uint32_t cksum = mdir->rbyd.cksum;

    // append gkcsumdelta?
    if (start_rid <= -2) {
        // figure out changes to our gcksumdelta
        mdir->gcksumdelta ^= lfs_crc32c_cube(lfs->gcksum_p)
                ^ lfs_crc32c_cube(lfs->gcksum ^ cksum)
                ^ lfs->gcksum_d;

        int err = lfsr_rbyd_appendrattr_(lfs, &mdir->rbyd, LFSR_RATTR_LE32(
                LFSR_TAG_GCKSUMDELTA, 0, mdir->gcksumdelta));
        if (err) {
            return err;
        }
    }

    // finalize commit
    int err = lfsr_rbyd_appendcksum_(lfs, &mdir->rbyd, cksum);
    if (err) {
        return err;
    }

    // success?

    // xor our new cksum
    lfs->gcksum ^= mdir->rbyd.cksum;

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
    lfsr_srid_t a_rid = lfs_smax(start_rid, -1);
    lfsr_srid_t b_rid = lfs_min(mdir->rbyd.weight, end_rid);
    lfs_size_t a_dsize = 0;
    lfs_size_t b_dsize = 0;
    lfs_size_t mdir_dsize = 0;

    while (a_rid != b_rid) {
        if (a_dsize > b_dsize
                // bias so lower dsize >= upper dsize
                || (a_dsize == b_dsize && a_rid > b_rid)) {
            LFS_SWAP(lfsr_srid_t, &a_rid, &b_rid);
            LFS_SWAP(lfs_size_t, &a_dsize, &b_dsize);
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
            if (err < 0) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }
            if (rid_ != a_rid) {
                break;
            }

            // special handling for shrub trunks, we need to include the
            // compacted cost of the shrub in our estimate
            //
            // this is what would make lfsr_rbyd_estimate recursive, and
            // why we need a second function...
            //
            if (tag == LFSR_TAG_BSHRUB) {
                // include the cost of this trunk
                dsize_ += LFSR_SHRUB_DSIZE;

                lfsr_shrub_t shrub;
                err = lfsr_data_readshrub(lfs, &data, mdir, &shrub);
                if (err < 0) {
                    return err;
                }

                lfs_ssize_t dsize__ = lfsr_shrub_estimate(lfs, &shrub);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += lfs->rattr_estimate + dsize__;

            } else {
                // include the cost of this tag
                dsize_ += lfs->rattr_estimate + lfsr_data_size(data);
            }
        }

        // include any opened+unsynced inlined files
        //
        // this is O(n^2), but littlefs is unlikely to have many open
        // files, I suppose if this becomes a problem we could sort
        // opened files by mid
        for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
            if (lfsr_o_isbshrub(o->flags)
                    // belongs to our mdir + rid?
                    && lfsr_mdir_cmp(&o->mdir, mdir) == 0
                    && lfsr_mrid(lfs, o->mdir.mid) == a_rid
                    // is a bshrub?
                    && lfsr_bshrub_isbshrub((lfsr_bshrub_t*)o)) {
                lfs_ssize_t dsize__ = lfsr_shrub_estimate(lfs,
                        &((lfsr_bshrub_t*)o)->shrub);
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

    // assume we keep any gcksumdelta, this will get fixed the first time
    // we commit anything
    if (start_rid == -2) {
        mdir_->gcksumdelta = mdir->gcksumdelta;
    }

    // copy over tags in the rbyd in order
    lfsr_srid_t rid = lfs_smax(start_rid, -1);
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

        // found an inlined shrub? we need to compact the shrub as well to
        // bring it along with us
        if (tag == LFSR_TAG_BSHRUB) {
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
            err = lfsr_rbyd_appendcompactrattr(lfs, &mdir_->rbyd,
                    LFSR_RATTR_SHRUB(tag, weight, &shrub));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

        } else {
            // write the tag
            err = lfsr_rbyd_appendcompactrattr(lfs, &mdir_->rbyd,
                    LFSR_RATTR_DATA(tag, weight, &data));
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
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_o_isbshrub(o->flags)
                // belongs to our mdir?
                && lfsr_mdir_cmp(&o->mdir, mdir) == 0
                && lfsr_mrid(lfs, o->mdir.mid) >= start_rid
                && (lfsr_rid_t)lfsr_mrid(lfs, o->mdir.mid)
                    < (lfsr_rid_t)end_rid
                // is a bshrub?
                && lfsr_bshrub_isbshrub((lfsr_bshrub_t*)o)
                // only compact once, first compact should
                // stage the new block
                && ((lfsr_bshrub_t*)o)->shrub_.blocks[0]
                    != mdir_->rbyd.blocks[0]) {
            int err = lfsr_shrub_compact(lfs, &mdir_->rbyd,
                    &((lfsr_bshrub_t*)o)->shrub_,
                    &((lfsr_bshrub_t*)o)->shrub);
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
        lfsr_smid_t mid, const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    // make a copy
    lfsr_mdir_t mdir_ = *mdir;
    // mark as erased in case of failure
    mdir->rbyd.eoff = -1;

    // try to commit
    int err = lfsr_mdir_commit__(lfs, &mdir_, start_rid, end_rid,
            mid, rattrs, rattr_count);
    if (err) {
        if (err == LFS_ERR_RANGE || err == LFS_ERR_CORRUPT) {
            goto swap;
        }
        return err;
    }

    // update mdir
    *mdir = mdir_;
    return 0;

swap:;
    // can't commit, can we compact?
    bool relocated = false;
    bool overcompacted = false;

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
    err = lfsr_mdir_swap__(lfs, &mdir_, mdir, false);
    if (err) {
        if (err == LFS_ERR_NOSPC || err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    goto compact;

relocate:;
    // needs relocation? bad prog? ok, try allocating a new mdir
    err = lfsr_mdir_alloc__(lfs, &mdir_, mdir->mid, relocated);
    if (err && !(err == LFS_ERR_NOSPC && !overcompacted)) {
        return err;
    }
    relocated = true;

    // no more blocks? wear-leveling falls apart here, but we can try
    // without relocating
    if (err == LFS_ERR_NOSPC) {
        LFS_WARN("Overcompacting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                lfsr_dbgmbid(lfs, mdir->mid),
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1]);
        overcompacted = true;

        err = lfsr_mdir_swap__(lfs, &mdir_, mdir, true);
        if (err) {
            // bad prog? can't do much here, mdir stuck
            if (err == LFS_ERR_CORRUPT) {
                LFS_ERROR("Stuck mdir 0x{%"PRIx32",%"PRIx32"}",
                        mdir->rbyd.blocks[0],
                        mdir->rbyd.blocks[1]);
                return LFS_ERR_NOSPC;
            }
            return err;
        }
    }

compact:;
    #ifdef LFS_DBGMDIRCOMMITS
    LFS_DEBUG("Compacting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                "-> 0x{%"PRIx32",%"PRIx32"}",
            lfsr_dbgmbid(lfs, mdir->mid),
            mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
            mdir_.rbyd.blocks[0], mdir_.rbyd.blocks[1]);
    #endif

    // don't copy over gcksum if relocating
    lfsr_srid_t start_rid_ = start_rid;
    if (relocated && !overcompacted) {
        start_rid_ = lfs_smax(start_rid_, -1);
    }

    // compact our mdir
    err = lfsr_mdir_compact__(lfs, &mdir_, mdir, start_rid_, end_rid);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        // bad prog? try another block
        if (err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    // now try to commit again
    //
    // upper layers should make sure this can't fail by limiting the
    // maximum commit size
    err = lfsr_mdir_commit__(lfs, &mdir_, start_rid_, end_rid,
            mid, rattrs, rattr_count);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        // bad prog? try another block
        if (err == LFS_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }

    // consume gcksumdelta if relocated
    if (relocated && !overcompacted) {
        lfs->gcksum_d ^= mdir->gcksumdelta;
    }
    // update mdir
    *mdir = mdir_;
    return 0;
}

static int lfsr_mroot_parent(lfs_t *lfs, const lfs_block_t mptr[static 2],
        lfsr_mdir_t *mparent_) {
    // we only call this when we actually have parents
    LFS_ASSERT(!lfsr_mptr_ismrootanchor(mptr));

    // scan list of mroots for our requested pair
    lfs_block_t mptr_[2] = {
            LFSR_MPTR_MROOTANCHOR()[0],
            LFSR_MPTR_MROOTANCHOR()[1]};
    while (true) {
        // fetch next possible superblock
        lfsr_mdir_t mdir;
        int err = lfsr_mdir_fetch(lfs, &mdir, -1, mptr_);
        if (err) {
            return err;
        }

        // lookup next mroot
        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_MROOT,
                NULL, &data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // decode mdir
        err = lfsr_data_readmptr(lfs, &data, mptr_);
        if (err) {
            return err;
        }

        // found our child?
        if (lfsr_mptr_cmp(mptr_, mptr) == 0) {
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
        const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    // non-mroot mdirs must have weight
    LFS_ASSERT(mdir->mid == -1
            // note inlined mdirs are mroots with mid != -1
            || lfsr_mdir_cmp(mdir, &lfs->mroot) == 0
            || mdir->rbyd.weight > 0);
    // rid in-bounds?
    LFS_ASSERT(lfsr_mrid(lfs, mdir->mid)
            <= (lfsr_srid_t)mdir->rbyd.weight);
    // lfs->mroot must have mid=-1
    LFS_ASSERT(lfs->mroot.mid == -1);

    // play out any rattrs that affect our grm _before_ committing to disk,
    // keep in mind we revert to on-disk gstate if we run into an error
    lfsr_smid_t mid_ = mdir->mid;
    for (lfs_size_t i = 0; i < rattr_count; i++) {
        // automatically create grms for new bookmarks
        if (rattrs[i].tag == LFSR_TAG_BOOKMARK) {
            lfsr_grm_push(lfs, mid_);

        // adjust pending grms?
        } else {
            for (int j = 0; j < 2; j++) {
                if (lfsr_mbid(lfs, lfs->grm.mids[j]) == lfsr_mbid(lfs, mid_)
                        && lfs->grm.mids[j] >= mid_) {
                    // deleting a pending grm doesn't really make sense
                    LFS_ASSERT(lfs->grm.mids[j] >= mid_ - rattrs[i].weight);

                    // adjust the grm
                    lfs->grm.mids[j] += rattrs[i].weight;
                }
            }
        }

        // adjust mid
        mid_ = lfsr_rattr_nextrid(rattrs[i], mid_);
    }

    // flush gdeltas
    lfsr_fs_flushgdelta(lfs);

    // xor our old cksum
    lfs->gcksum ^= mdir->rbyd.cksum;

    // stage any bshrubs
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_o_isbshrub(o->flags)) {
            // a bshrub outside of its mdir means something has gone
            // horribly wrong
            LFS_ASSERT(!lfsr_bshrub_isbshrub((lfsr_bshrub_t*)o)
                    || ((lfsr_bshrub_t*)o)->shrub.blocks[0]
                        == o->mdir.rbyd.blocks[0]);
            ((lfsr_bshrub_t*)o)->shrub_ = ((lfsr_bshrub_t*)o)->shrub;
        }
    }

    // create a copy
    lfsr_mdir_t mdir_[2];
    mdir_[0] = *mdir;
    // mark our mdir as unerased in case we fail
    mdir->rbyd.eoff = -1;
    // mark any copies of our mdir as unerased in case we fail
    if (lfsr_mdir_cmp(mdir, &lfs->mroot) == 0) {
        lfs->mroot.rbyd.eoff = -1;
    }
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_mdir_cmp(&o->mdir, mdir) == 0) {
            o->mdir.rbyd.eoff = -1;
        }
    }

    // attempt to commit/compact the mdir normally
    lfsr_srid_t split_rid;
    int err = lfsr_mdir_commit_(lfs, &mdir_[0], -2, -1, &split_rid,
            mdir->mid, rattrs, rattr_count);
    if (err && err != LFS_ERR_RANGE
            && err != LFS_ERR_NOENT) {
        goto failed;
    }

    // keep track of any mroot changes
    lfsr_mdir_t mroot_ = lfs->mroot;
    if (!err && lfsr_mdir_cmp(mdir, &lfs->mroot) == 0) {
        lfsr_mdir_sync(&mroot_, &mdir_[0]);
    }

    // handle possible mtree updates, this gets a bit messy
    lfsr_btree_t mtree_ = lfs->mtree;
    lfsr_smid_t mdelta = 0;
    // need to split?
    if (err == LFS_ERR_RANGE) {
        // this should not happen unless we can't fit our mroot's metadata
        LFS_ASSERT(lfsr_mdir_cmp(mdir, &lfs->mroot) != 0
                || lfs->mtree.weight == 0);

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
            bool l = lfsr_mrid(lfs, mdir->mid) < split_rid;

            bool relocated = false;;
        split_relocate:;
            // alloc and compact into new mdirs
            err = lfsr_mdir_alloc__(lfs, &mdir_[i^l],
                    lfs_smax(mdir->mid, 0), relocated);
            if (err) {
                goto failed;
            }
            relocated = true;

            err = lfsr_mdir_compact__(lfs, &mdir_[i^l],
                    mdir,
                    ((i^l) == 0) ?         0 : split_rid,
                    ((i^l) == 0) ? split_rid :        -1);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto split_relocate;
                }
                goto failed;
            }

            err = lfsr_mdir_commit__(lfs, &mdir_[i^l],
                    ((i^l) == 0) ?         0 : split_rid,
                    ((i^l) == 0) ? split_rid :        -1,
                    mdir->mid, rattrs, rattr_count);
            if (err && err != LFS_ERR_NOENT) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto split_relocate;
                }
                goto failed;
            }
            // empty? set weight to zero
            if (err == LFS_ERR_NOENT) {
                mdir_[i^l].rbyd.weight = 0;
            }
        }

        // adjust our sibling's mid after committing rattrs
        mdir_[1].mid += (1 << lfs->mbits);

        LFS_INFO("Splitting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "-> 0x{%"PRIx32",%"PRIx32"}, 0x{%"PRIx32",%"PRIx32"}",
                lfsr_dbgmbid(lfs, mdir->mid),
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
                mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1],
                mdir_[1].rbyd.blocks[0], mdir_[1].rbyd.blocks[1]);

        // because of defered commits, children can be reduced to zero
        // when splitting, need to catch this here

        // both siblings reduced to zero
        if (mdir_[0].rbyd.weight == 0 && mdir_[1].rbyd.weight == 0) {
            LFS_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfsr_dbgmbid(lfs, mdir_[0].mid),
                    mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1]);
            LFS_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfsr_dbgmbid(lfs, mdir_[1].mid),
                    mdir_[1].rbyd.blocks[0], mdir_[1].rbyd.blocks[1]);
            goto dropped;

        // one sibling reduced to zero
        } else if (mdir_[0].rbyd.weight == 0) {
            LFS_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfsr_dbgmbid(lfs, mdir_[0].mid),
                    mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1]);
            lfsr_mdir_sync(&mdir_[0], &mdir_[1]);
            goto relocated;

        // other sibling reduced to zero
        } else if (mdir_[1].rbyd.weight == 0) {
            LFS_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                    lfsr_dbgmbid(lfs, mdir_[1].mid),
                    mdir_[1].rbyd.blocks[0], mdir_[1].rbyd.blocks[1]);
            goto relocated;
        }

        // no siblings reduced to zero, update our mtree
        mdelta = +(1 << lfs->mbits);

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending rattrs in
        // case they introduce a new name!
        lfsr_data_t split_name;
        err = lfsr_rbyd_lookup(lfs, &mdir_[1].rbyd, 0,
                LFSR_TAG_MASK8 | LFSR_TAG_NAME,
                NULL, &split_name);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            goto failed;
        }

        // new mtree?
        if (lfs->mtree.weight == 0) {
            lfsr_btree_init(&mtree_);

            err = lfsr_btree_commit(lfs, &mtree_,
                    0, LFSR_RATTRS(
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MDIR, +(1 << lfs->mbits),
                            mdir_[0].rbyd.blocks),
                        LFSR_RATTR_DATA(
                            LFSR_TAG_NAME, +(1 << lfs->mbits),
                            &split_name),
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MDIR, 0,
                            mdir_[1].rbyd.blocks)));
            if (err) {
                goto failed;
            }

        // update our mtree
        } else {
            // mark as unerased in case of failure
            lfs->mtree.eoff = -1;

            err = lfsr_btree_commit(lfs, &mtree_,
                    lfsr_mbid(lfs, mdir->mid), LFSR_RATTRS(
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MDIR, 0,
                            mdir_[0].rbyd.blocks),
                        LFSR_RATTR_DATA(
                            LFSR_TAG_NAME, +(1 << lfs->mbits),
                            &split_name),
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MDIR, 0,
                            mdir_[1].rbyd.blocks)));
            if (err) {
                goto failed;
            }
        }

    // need to drop?
    } else if (err == LFS_ERR_NOENT) {
        LFS_INFO("Dropping mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"}",
                lfsr_dbgmbid(lfs, mdir->mid),
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1]);
        // set weight to zero
        mdir_[0].rbyd.weight = 0;

        // consume gstate so we don't lose any info
        err = lfsr_fs_consumegdelta(lfs, mdir);
        if (err) {
            goto failed;
        }

    dropped:;
        mdelta = -(1 << lfs->mbits);

        // how can we drop if we have no mtree?
        LFS_ASSERT(lfs->mtree.weight != 0);

        // mark as unerased in case of failure
        lfs->mtree.eoff = -1;

        // update our mtree
        err = lfsr_btree_commit(lfs, &mtree_,
                lfsr_mbid(lfs, mdir->mid), LFSR_RATTRS(
                    LFSR_RATTR(
                        LFSR_TAG_RM, -(1 << lfs->mbits))));
        if (err) {
            goto failed;
        }

    // need to relocate?
    } else if (lfsr_mdir_cmp(&mdir_[0], mdir) != 0
            && lfsr_mdir_cmp(mdir, &lfs->mroot) != 0) {
        LFS_INFO("Relocating mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "-> 0x{%"PRIx32",%"PRIx32"}",
                lfsr_dbgmbid(lfs, mdir->mid),
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
                mdir_[0].rbyd.blocks[0], mdir_[0].rbyd.blocks[1]);

    relocated:;
        // new mtree?
        if (lfs->mtree.weight == 0) {
            lfsr_btree_init(&mtree_);

            err = lfsr_btree_commit(lfs, &mtree_,
                    0, LFSR_RATTRS(
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MDIR, +(1 << lfs->mbits),
                            mdir_[0].rbyd.blocks)));
            if (err) {
                goto failed;
            }

        // update our mtree
        } else {
            // mark as unerased in case of failure
            lfs->mtree.eoff = -1;

            err = lfsr_btree_commit(lfs, &mtree_,
                    lfsr_mbid(lfs, mdir->mid), LFSR_RATTRS(
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MDIR, 0,
                            mdir_[0].rbyd.blocks)));
            if (err) {
                goto failed;
            }
        }
    }

    // patch any pending grms
    for (int j = 0; j < 2; j++) {
        if (lfsr_mbid(lfs, lfs->grm.mids[j])
                == lfsr_mbid(lfs, lfs_smax(mdir->mid, 0))) {
            if (mdelta > 0
                    && lfsr_mrid(lfs, lfs->grm.mids[j])
                        >= (lfsr_srid_t)mdir_[0].rbyd.weight) {
                lfs->grm.mids[j]
                        += (1 << lfs->mbits) - mdir_[0].rbyd.weight;
            }
        } else if (lfs->grm.mids[j] > mdir->mid) {
            lfs->grm.mids[j] += mdelta;
        }
    }

    // need to update mtree?
    if (lfsr_btree_cmp(&mtree_, &lfs->mtree) != 0) {
        // mtree should never go to zero since we always have a root bookmark
        LFS_ASSERT(mtree_.weight > 0);

        // make sure mtree/mroot changes are on-disk before committing
        // metadata
        err = lfsr_bd_sync(lfs);
        if (err) {
            goto failed;
        }

        // xor mroot's cksum if we haven't already
        if (lfsr_mdir_cmp(mdir, &lfs->mroot) != 0) {
            lfs->gcksum ^= lfs->mroot.rbyd.cksum;
        }

        // mark any copies of our mroot as unerased
        lfs->mroot.rbyd.eoff = -1;
        for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
            if (lfsr_mdir_cmp(&o->mdir, &lfs->mroot) == 0) {
                o->mdir.rbyd.eoff = -1;
            }
        }

        // commit new mtree into our mroot
        //
        // note end_rid=0 here will delete any files leftover from a split
        // in our mroot
        err = lfsr_mdir_commit_(lfs, &mroot_, -2, 0, NULL,
                -1, LFSR_RATTRS(
                    LFSR_RATTR_BTREE(
                        LFSR_TAG_MASK8 | LFSR_TAG_MTREE, 0,
                        &mtree_),
                    // were we committing to the mroot? include any -1 rattrs
                    (mdir->mid == -1)
                        ? LFSR_RATTR_RATTRS(rattrs, rattr_count)
                        : LFSR_RATTR_NOOP()));
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
            err = lfsr_mroot_parent(lfs, mrootchild.rbyd.blocks,
                    &mrootparent_);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                goto failed;
            }

            LFS_INFO("Relocating mroot 0x{%"PRIx32",%"PRIx32"} "
                        "-> 0x{%"PRIx32",%"PRIx32"}",
                    mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                    mrootchild_.rbyd.blocks[0], mrootchild_.rbyd.blocks[1]);

            mrootchild = mrootparent_;

            // make sure mtree/mroot changes are on-disk before committing
            // metadata
            err = lfsr_bd_sync(lfs);
            if (err) {
                goto failed;
            }

            // xor mrootchild's cksum
            lfs->gcksum ^= mrootparent_.rbyd.cksum;

            // commit mrootchild
            err = lfsr_mdir_commit_(lfs, &mrootparent_, -2, -1, NULL,
                    -1, LFSR_RATTRS(
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MROOT, 0,
                            mrootchild_.rbyd.blocks)));
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
            LFS_INFO("Extending mroot 0x{%"PRIx32",%"PRIx32"}"
                        " -> 0x{%"PRIx32",%"PRIx32"}, 0x{%"PRIx32",%"PRIx32"}",
                    mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                    mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                    mrootchild_.rbyd.blocks[0], mrootchild_.rbyd.blocks[1]);

            // make sure mtree/mroot changes are on-disk before committing
            // metadata
            err = lfsr_bd_sync(lfs);
            if (err) {
                goto failed;
            }

            // commit the new mroot anchor
            lfsr_mdir_t mrootanchor_;
            err = lfsr_mdir_swap__(lfs, &mrootanchor_, &mrootchild, true);
            if (err) {
                // bad prog? can't do much here, mroot stuck
                if (err == LFS_ERR_CORRUPT) {
                    LFS_ERROR("Stuck mroot 0x{%"PRIx32",%"PRIx32"}",
                            mrootanchor_.rbyd.blocks[0],
                            mrootanchor_.rbyd.blocks[1]);
                    return LFS_ERR_NOSPC;
                }
                goto failed;
            }

            err = lfsr_mdir_commit__(lfs, &mrootanchor_, -2, -1,
                    -1, LFSR_RATTRS(
                        LFSR_RATTR_BUF(
                            LFSR_TAG_MAGIC, 0,
                            "littlefs", 8),
                        LFSR_RATTR_MPTR(
                            LFSR_TAG_MROOT, 0,
                            mrootchild_.rbyd.blocks)));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                LFS_ASSERT(err != LFS_ERR_NOENT);
                // bad prog? can't do much here, mroot stuck
                if (err == LFS_ERR_CORRUPT) {
                    LFS_ERROR("Stuck mroot 0x{%"PRIx32",%"PRIx32"}",
                            mrootanchor_.rbyd.blocks[0],
                            mrootanchor_.rbyd.blocks[1]);
                    return LFS_ERR_NOSPC;
                }
                goto failed;
            }
        }
    }

    // sync on-disk state
    err = lfsr_bd_sync(lfs);
    if (err) {
        return err;
    }

    ///////////////////////////////////////////////////////////////////////
    // success? update in-device state, we must not error at this point! //
    ///////////////////////////////////////////////////////////////////////

    // play out any rattrs that affect internal state
    mid_ = mdir->mid;
    for (lfs_size_t i = 0; i < rattr_count; i++) {
        // adjust any opened mdirs
        for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
            // adjust opened mdirs?
            if (lfsr_mdir_cmp(&o->mdir, mdir) == 0
                    && o->mdir.mid >= mid_) {
                // removed?
                if (o->mdir.mid < mid_ - rattrs[i].weight) {
                    // we should not be removing opened regular files
                    LFS_ASSERT(lfsr_o_type(o->flags) != LFS_TYPE_REG);
                    o->flags |= LFS_o_ZOMBIE;
                    o->mdir.mid = mid_;
                } else {
                    o->mdir.mid += rattrs[i].weight;
                }
            }
        }

        // adjust mid
        mid_ = lfsr_rattr_nextrid(rattrs[i], mid_);
    }

    // if mroot/mtree changed, clobber any mroot/mtree traversals
    if (lfsr_mdir_cmp(&mroot_, &lfs->mroot) != 0
            || lfsr_btree_cmp(&mtree_, &lfs->mtree) != 0) {
        for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
            if (lfsr_o_type(o->flags) == LFS_TYPE_TRAVERSAL
                    && o->mdir.mid == -1
                    // don't clobber the current mdir, assume upper layers
                    // know what they're doing
                    && &o->mdir != mdir) {
                lfsr_traversal_clobber(lfs, (lfsr_traversal_t*)o);
            }
        }
    }

    // update internal mdir state
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        // avoid double updating the current mdir
        if (&o->mdir == mdir) {
            continue;
        }

        // update any splits/drops
        if (lfsr_mdir_cmp(&o->mdir, mdir) == 0) {
            if (mdelta > 0
                    && lfsr_mrid(lfs, o->mdir.mid)
                        >= (lfsr_srid_t)mdir_[0].rbyd.weight) {
                o->mdir.mid += (1 << lfs->mbits) - mdir_[0].rbyd.weight;
                lfsr_mdir_sync(&o->mdir, &mdir_[1]);
            } else {
                lfsr_mdir_sync(&o->mdir, &mdir_[0]);
            }
        } else if (o->mdir.mid > mdir->mid) {
            o->mdir.mid += mdelta;
        }
    }

    // update mdir to follow requested rid
    if (mdelta > 0
            && mdir->mid == -1) {
        lfsr_mdir_sync(mdir, &mroot_);
    } else if (mdelta > 0
            && lfsr_mrid(lfs, mdir->mid)
                >= (lfsr_srid_t)mdir_[0].rbyd.weight) {
        mdir->mid += (1 << lfs->mbits) - mdir_[0].rbyd.weight;
        lfsr_mdir_sync(mdir, &mdir_[1]);
    } else {
        lfsr_mdir_sync(mdir, &mdir_[0]);
    }

    // update mroot and mtree
    lfsr_mdir_sync(&lfs->mroot, &mroot_);
    lfs->mtree = mtree_;

    // update any staged bshrubs
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_o_isbshrub(o->flags)) {
            ((lfsr_bshrub_t*)o)->shrub = ((lfsr_bshrub_t*)o)->shrub_;
        }
    }

    // update any gstate changes
    lfsr_fs_commitgdelta(lfs);

    // mark all traversals as dirty
    lfsr_fs_mkdirty(lfs);

    // we may have touched any number of mdirs, so assume uncompacted
    // until lfsr_fs_gc can prove otherwise
    lfs->flags |= LFS_I_COMPACT;

    #ifdef LFS_DBGMDIRCOMMITS
    LFS_DEBUG("Committed mdir %"PRId32" "
                "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            lfsr_dbgmbid(lfs, mdir->mid),
            mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
            lfsr_rbyd_trunk(&mdir->rbyd),
            mdir->rbyd.weight,
            mdir->rbyd.cksum);
    #endif
    return 0;

failed:;
    // revert gstate to on-disk state
    lfsr_fs_revertgdelta(lfs);
    return err;
}

static int lfsr_mdir_compact(lfs_t *lfs, lfsr_mdir_t *mdir) {
    // the easiest way to do this is to just mark mdir as unerased
    // and call lfsr_mdir_commit
    mdir->rbyd.eoff = -1;
    return lfsr_mdir_commit(lfs, mdir, NULL, 0);
}



/// Mtree path/name lookup ///

// lookup names in an mdir
//
// if not found, rid will be the best place to insert
static int lfsr_mdir_namelookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_did_t did, const char *name, lfs_size_t name_len,
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
            did, name, name_len,
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

    // intercept pending grms here and pretend they're orphaned
    // stickynotes
    //
    // fortunately pending grms/orphaned stickynotes have roughly the
    // same semantics, and it's easier to manage the implied mid gap in
    // higher-levels
    if (lfsr_grm_ismidrm(lfs, mid)) {
        tag = LFSR_TAG_STICKYNOTE;
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
static int lfsr_mtree_namelookupleaf(lfs_t *lfs,
        lfsr_did_t did, const char *name, lfs_size_t name_len,
        lfsr_mdir_t *mdir_) {
    // do we only have mroot?
    lfsr_mdir_t mdir;
    if (lfs->mtree.weight == 0) {
        // treat inlined mdir as mid=0
        mdir.mid = 0;
        lfsr_mdir_sync(&mdir, &lfs->mroot);

    // lookup name in actual mtree
    } else {
        lfsr_bid_t bid;
        lfsr_tag_t tag;
        lfsr_bid_t weight;
        lfsr_data_t data;
        lfs_scmp_t cmp = lfsr_btree_namelookup(lfs, &lfs->mtree,
                did, name, name_len,
                &bid, &tag, &weight, &data);
        if (cmp < 0) {
            LFS_ASSERT(cmp != LFS_ERR_NOENT);
            return cmp;
        }
        LFS_ASSERT(tag == LFSR_TAG_MDIR);
        LFS_ASSERT(weight == (1U << lfs->mbits));

        // fetch mdir
        int err = lfsr_data_fetchmdir(lfs, &data, bid-(weight-1),
                &mdir);
        if (err) {
            return err;
        }
    }

    if (mdir_) {
        *mdir_ = mdir;
    }
    return 0;
}

static int lfsr_mtree_namelookup(lfs_t *lfs,
        lfsr_did_t did, const char *name, lfs_size_t name_len,
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // lookup name in our mtree
    lfsr_mdir_t mdir;
    int err = lfsr_mtree_namelookupleaf(lfs,
            did, name, name_len,
            &mdir);
    if (err) {
        return err;
    }

    // and lookup name in our mdir
    lfsr_smid_t mid;
    err = lfsr_mdir_namelookup(lfs, &mdir,
            did, name, name_len,
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

// some operations on paths
static inline lfs_size_t lfsr_path_namelen(const char *path) {
    return lfs_strcspn(path, "/");
}

static inline bool lfsr_path_islast(const char *path) {
    lfs_size_t name_len = lfsr_path_namelen(path);
    return path[name_len + lfs_strspn(path + name_len, "/")] == '\0';
}

static inline bool lfsr_path_isdir(const char *path) {
    return path[lfsr_path_namelen(path)] != '\0';
}

// lookup a full path in our mtree, updating the path as we descend
//
// the errors get a bit subtle here, and rely on what ends up in the
// path/mdir:
// - 0                                      => file found
// - 0, lfsr_path_isdir(path)               => dir found
// - 0, mdir.mid=-1                         => root found
// - LFS_ERR_NOENT, lfsr_path_islast(path)  => file not found
// - LFS_ERR_NOENT, !lfsr_path_islast(path) => parent not found
// - LFS_ERR_NOTDIR                         => parent not a dir
// - LFS_ERR_NOTSUP                         => parent of unknown type
//
// if not found, mdir_/did_ will at least be set up with what should be
// the parent
//
static int lfsr_mtree_pathlookup(lfs_t *lfs, const char **path,
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_, lfsr_did_t *did_) {
    // setup root
    lfsr_mdir_t mdir = lfs->mroot;
    lfsr_tag_t tag = LFSR_TAG_DIR;
    lfsr_did_t did = LFSR_DID_ROOT;
    
    // we reduce path to a single name if we can find it
    const char *path_ = *path;

    // empty paths are not allowed
    if (path_[0] == '\0') {
        return LFS_ERR_INVAL;
    }

    while (true) {
        // skip slashes if we're a directory
        if (tag == LFSR_TAG_DIR) {
            path_ += lfs_strspn(path_, "/");
        }
        lfs_size_t name_len = lfs_strcspn(path_, "/");

        // skip '.'
        if (name_len == 1 && lfs_memcmp(path_, ".", 1) == 0) {
            path_ += name_len;
            goto next;
        }

        // error on unmatched '..', trying to go above root, eh?
        if (name_len == 2 && lfs_memcmp(path_, "..", 2) == 0) {
            return LFS_ERR_INVAL;
        }

        // skip if matched by '..' in name
        const char *suffix = path_ + name_len;
        lfs_size_t suffix_len;
        int depth = 1;
        while (true) {
            suffix += lfs_strspn(suffix, "/");
            suffix_len = lfs_strcspn(suffix, "/");
            if (suffix_len == 0) {
                break;
            }

            if (suffix_len == 1 && lfs_memcmp(suffix, ".", 1) == 0) {
                // noop
            } else if (suffix_len == 2 && lfs_memcmp(suffix, "..", 2) == 0) {
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
            if (mdir_) {
                *mdir_ = mdir;
            }
            if (tag_) {
                *tag_ = tag;
            }
            if (did_) {
                *did_ = did;
            }
            return 0;
        }

        // only continue if we hit a directory
        if (tag != LFSR_TAG_DIR) {
            return (tag == LFSR_TAG_STICKYNOTE)
                        ? LFS_ERR_NOENT
                    : (tag == LFSR_TAG_REG)
                        ? LFS_ERR_NOTDIR
                        : LFS_ERR_NOTSUP;
        }

        // read the next did from the mdir if this is not the root
        if (mdir.mid != -1) {
            lfsr_data_t data;
            int err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_DID,
                    NULL, &data);
            if (err) {
                return err;
            }

            err = lfsr_data_readleb128(lfs, &data, &did);
            if (err) {
                return err;
            }
        }

        // update path as we parse
        *path = path_;

        // lookup up this name in the mtree
        int err = lfsr_mtree_namelookup(lfs, did, path_, name_len,
                &mdir, &tag, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // keep track of where to insert if we can't find path
        if (err == LFS_ERR_NOENT) {
            if (mdir_) {
                *mdir_ = mdir;
            }
            if (tag_) {
                *tag_ = tag;
            }
            if (did_) {
                *did_ = did;
            }
            return LFS_ERR_NOENT;
        }

        // go on to next name
        path_ += name_len;
    next:;
    }
}



/// Mtree traversal ///

// traversing littlefs is a bit complex, so we use a state machine to keep
// track of where we are
enum {
    LFSR_TSTATE_MROOTANCHOR = 0,
    LFSR_TSTATE_MROOTCHAIN  = 1,
    LFSR_TSTATE_MTREE       = 2,
    LFSR_TSTATE_MDIRS       = 3,
    LFSR_TSTATE_MDIR        = 4,
    LFSR_TSTATE_BTREE       = 5,
    LFSR_TSTATE_OMDIRS      = 6,
    LFSR_TSTATE_OBTREE      = 7,
    LFSR_TSTATE_DONE        = 8,
};

static void lfsr_traversal_init(lfsr_traversal_t *t, uint32_t flags) {
    t->b.o.flags = lfsr_o_settype(0, LFS_TYPE_TRAVERSAL)
            | lfsr_t_settstate(0, LFSR_TSTATE_MROOTANCHOR)
            | flags;
    t->b.o.mdir.mid = -1;
    t->b.o.mdir.rbyd.weight = 0;
    t->b.o.mdir.rbyd.blocks[0] = -1;
    t->b.o.mdir.rbyd.blocks[1] = -1;
    lfsr_bshrub_init(&t->b);
    t->ot = NULL;
    t->u.mtortoise.blocks[0] = -1;
    t->u.mtortoise.blocks[1] = -1;
    t->u.mtortoise.step = 0;
    t->u.mtortoise.power = 0;
    t->gcksum = 0;
}

// needed in lfsr_mtree_traverse_
static int lfsr_file_traverse_(lfs_t *lfs, const lfsr_bshrub_t *bshrub,
        lfsr_btraversal_t *bt,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bptr_t *bptr_);

// low-level traversal _only_ finds blocks
static int lfsr_mtree_traverse_(lfs_t *lfs, lfsr_traversal_t *t,
        lfsr_tag_t *tag_, lfsr_bptr_t *bptr_) {
    while (true) {
        switch (lfsr_t_tstate(t->b.o.flags)) {
        // start with the mrootanchor 0x{0,1}
        //
        // note we make sure to include all mroots in our mroot chain!
        //
        case LFSR_TSTATE_MROOTANCHOR:;
            // fetch the first mroot 0x{0,1}
            int err = lfsr_mdir_fetch(lfs, &t->b.o.mdir,
                    -1, LFSR_MPTR_MROOTANCHOR());
            if (err) {
                return err;
            }

            // transition to traversing the mroot chain
            t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                    LFSR_TSTATE_MROOTCHAIN);

            if (tag_) {
                *tag_ = LFSR_TAG_MDIR;
            }
            if (bptr_) {
                bptr_->data.u.buffer = (const uint8_t*)&t->b.o.mdir;
            }
            return 0;

        // traverse the mroot chain, checking for mroots/mtrees
        case LFSR_TSTATE_MROOTCHAIN:;
            // lookup mroot, if we find one this is not the active mroot
            lfsr_tag_t tag;
            lfsr_data_t data;
            err = lfsr_mdir_lookup(lfs, &t->b.o.mdir,
                    LFSR_TAG_MASK8 | LFSR_TAG_STRUCT,
                    &tag, &data);
            if (err) {
                // if we have no mtree (inlined mdir), we need to
                // traverse any files in our mroot next
                if (err == LFS_ERR_NOENT) {
                    t->b.o.mdir.mid = 0;
                    t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                            LFSR_TSTATE_MDIR);
                    continue;
                }
                return err;
            }

            // found a new mroot
            if (tag == LFSR_TAG_MROOT) {
                // fetch this mroot
                err = lfsr_data_fetchmdir(lfs, &data, -1,
                        &t->b.o.mdir);
                if (err) {
                    return err;
                }

                // detect cycles with Brent's algorithm
                //
                // note we only check for cycles in the mroot chain, the
                // btree inner nodes require checksums of their pointers,
                // so creating a valid cycle is actually quite difficult
                //
                if (lfsr_mptr_cmp(
                        t->b.o.mdir.rbyd.blocks,
                        t->u.mtortoise.blocks) == 0) {
                    LFS_ERROR("Cycle detected during mtree traversal "
                                "0x{%"PRIx32",%"PRIx32"}",
                            t->b.o.mdir.rbyd.blocks[0],
                            t->b.o.mdir.rbyd.blocks[1]);
                    return LFS_ERR_CORRUPT;
                }
                if (t->u.mtortoise.step == (1U << t->u.mtortoise.power)) {
                    t->u.mtortoise.blocks[0] = t->b.o.mdir.rbyd.blocks[0];
                    t->u.mtortoise.blocks[1] = t->b.o.mdir.rbyd.blocks[1];
                    t->u.mtortoise.step = 0;
                    t->u.mtortoise.power += 1;
                }
                t->u.mtortoise.step += 1;

                if (tag_) {
                    *tag_ = LFSR_TAG_MDIR;
                }
                if (bptr_) {
                    bptr_->data.u.buffer = (const uint8_t*)&t->b.o.mdir;
                }
                return 0;

            // found an mtree?
            } else if (tag == LFSR_TAG_MTREE) {
                // fetch the root of the mtree
                err = lfsr_data_fetchbtree(lfs, &data,
                        &t->b.shrub);
                if (err) {
                    return err;
                }

                // transition to traversing the mtree
                lfsr_btraversal_init(&t->u.bt);
                t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                        LFSR_TSTATE_MTREE);
                continue;

            } else {
                LFS_ERROR("Weird mroot entry? 0x%"PRIx32, tag);
                return LFS_ERR_CORRUPT;
            }

        // iterate over mdirs in the mtree
        case LFSR_TSTATE_MDIRS:;
            // find the next mdir
            err = lfsr_mtree_lookupleaf(lfs, t->b.o.mdir.mid,
                    &t->b.o.mdir);
            if (err) {
                // end of mtree? guess we're done
                if (err == LFS_ERR_NOENT) {
                    t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                            LFSR_TSTATE_DONE);
                    continue;
                }
                return err;
            }

            // transition to traversing the mdir
            t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                    LFSR_TSTATE_MDIR);

            if (tag_) {
                *tag_ = LFSR_TAG_MDIR;
            }
            if (bptr_) {
                bptr_->data.u.buffer = (const uint8_t*)&t->b.o.mdir;
            }
            return 0;

        // scan for blocks/btrees in the current mdir
        case LFSR_TSTATE_MDIR:;
            // not traversing all blocks? have we exceeded our mdir's weight?
            // return to mtree iteration
            if (lfsr_t_ismtreeonly(t->b.o.flags)
                    || lfsr_mrid(lfs, t->b.o.mdir.mid)
                        >= (lfsr_srid_t)t->b.o.mdir.rbyd.weight) {
                t->b.o.mdir.mid = lfsr_mbid(lfs, t->b.o.mdir.mid) + 1;
                t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                        LFSR_TSTATE_MDIRS);
                continue;
            }

            // do we have a block/btree?
            err = lfsr_mdir_lookupnext(lfs, &t->b.o.mdir, LFSR_TAG_DATA,
                    &tag, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            // found a bshrub (inlined btree)?
            if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
                err = lfsr_data_readshrub(lfs, &data, &t->b.o.mdir,
                        &t->b.shrub);
                if (err) {
                    return err;
                }

            // found a btree?
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BTREE) {
                err = lfsr_data_fetchbtree(lfs, &data,
                        &t->b.shrub);
                if (err) {
                    return err;
                }

            // no? next we need to check any opened files
            } else {
                t->ot = lfs->omdirs;
                t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                        LFSR_TSTATE_OMDIRS);
                continue;
            }

            // start traversing
            lfsr_btraversal_init(&t->u.bt);
            t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                    LFSR_TSTATE_BTREE);
            continue;

        // scan for blocks/btrees in our opened file list
        case LFSR_TSTATE_OMDIRS:;
            // reached end of opened files? return to mdir traversal
            if (!t->ot) {
                t->b.o.mdir.mid += 1;
                t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                        LFSR_TSTATE_MDIR);
                continue;
            }

            // skip unrelated files, we only care about unsync reg files
            // associated with the current mid
            //
            // we traverse mids separately to make recovery from clobbered
            // traversals easier, which means this grows O(n^2) if you have
            // literally every file open, but other things grow O(n^2) with
            // this list anyways
            //
            if (t->ot->mdir.mid != t->b.o.mdir.mid
                    || lfsr_o_type(t->ot->flags) != LFS_TYPE_REG
                    || !lfsr_o_isunsync(t->ot->flags)) {
                t->ot = t->ot->next;
                continue;
            }

            // start traversing the file
            const lfsr_file_t *file = (const lfsr_file_t*)t->ot;
            t->b.shrub = file->b.shrub;
            lfsr_btraversal_init(&t->u.bt);
            t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                    LFSR_TSTATE_OBTREE);
            continue;

        // traverse any bshrubs/btrees we see, this includes the mtree
        // and any file btrees/bshrubs
        case LFSR_TSTATE_MTREE:;
        case LFSR_TSTATE_BTREE:;
        case LFSR_TSTATE_OBTREE:;
            // traverse through our bshrub/btree
            //
            // it probably looks a bit weird to go through
            // lfsr_file_traverse_, but this gets us bptr decoding
            // for free
            err = lfsr_file_traverse_(lfs, &t->b, &t->u.bt,
                    NULL, &tag, bptr_);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    // clear the bshrub state
                    lfsr_bshrub_init(&t->b);
                    // end of mtree? start iterating over mdirs
                    if (lfsr_t_tstate(t->b.o.flags)
                            == LFSR_TSTATE_MTREE) {
                        t->b.o.mdir.mid = 0;
                        t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                                LFSR_TSTATE_MDIRS);
                        continue;
                    // end of mdir btree? start iterating over opened files
                    } else if (lfsr_t_tstate(t->b.o.flags)
                            == LFSR_TSTATE_BTREE) {
                        t->ot = lfs->omdirs;
                        t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                                LFSR_TSTATE_OMDIRS);
                        continue;
                    // end of opened btree? go to next opened file
                    } else if (lfsr_t_tstate(t->b.o.flags)
                            == LFSR_TSTATE_OBTREE) {
                        t->ot = t->ot->next;
                        t->b.o.flags = lfsr_t_settstate(t->b.o.flags,
                                LFSR_TSTATE_OMDIRS);
                        continue;
                    } else {
                        LFS_UNREACHABLE();
                    }
                }
                return err;
            }

            // found an inner btree node?
            if (tag == LFSR_TAG_BRANCH) {
                if (tag_) {
                    *tag_ = tag;
                }
                return 0;

            // found an indirect block?
            } else if (tag == LFSR_TAG_BLOCK) {
                if (tag_) {
                    *tag_ = tag;
                }
                return 0;
            }

            continue;

        case LFSR_TSTATE_DONE:;
            return LFS_ERR_NOENT;

        default:;
            LFS_UNREACHABLE();
        }
    }
}

// needed in lfsr_mtree_traverse
static void lfs_alloc_markinuse(lfs_t *lfs,
        lfsr_tag_t tag, const lfsr_bptr_t *bptr);

// high-level immutable traversal, handle extra features here,
// but no mutation! (we're called in lfs_alloc, so things would end up
// recursive, which would be a bit bad!)
static int lfsr_mtree_traverse(lfs_t *lfs, lfsr_traversal_t *t,
        lfsr_tag_t *tag_, lfsr_bptr_t *bptr_) {
    lfsr_tag_t tag;
    lfsr_bptr_t bptr;
    int err = lfsr_mtree_traverse_(lfs, t,
            &tag, &bptr);
    if (err) {
        // end of traversal?
        if (err == LFS_ERR_NOENT) {
            goto eot;
        }
        return err;
    }

    // validate mdirs? mdir checksums are already validated in
    // lfsr_mdir_fetch, but this doesn't prevent rollback issues, where
    // the most recent commit is corrupted but a previous outdated
    // commit appears valid
    //
    // this is where the gcksum comes in, which we can recalculate to
    // check if the filesystem state on-disk is as expected
    //
    // we also compare mdir checksums with any open mdirs to try to
    // avoid traversing any outdated bshrubs/btrees
    if ((lfsr_t_isckmeta(t->b.o.flags)
                || lfsr_t_isckdata(t->b.o.flags))
            && tag == LFSR_TAG_MDIR) {
        lfsr_mdir_t *mdir = (lfsr_mdir_t*)bptr.data.u.buffer;

        // check cksum matches our mroot
        if (lfsr_mdir_cmp(mdir, &lfs->mroot) == 0
                && mdir->rbyd.cksum != lfs->mroot.rbyd.cksum) {
            LFS_ERROR("Found mroot cksum mismatch "
                        "0x{%"PRIx32",%"PRIx32"}, "
                        "cksum %08"PRIx32" (!= %08"PRIx32")",
                    mdir->rbyd.blocks[0],
                    mdir->rbyd.blocks[1],
                    mdir->rbyd.cksum,
                    lfs->mroot.rbyd.cksum);
            return LFS_ERR_CORRUPT;
        }

        // check cksum matches any open mdirs
        for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
            if (lfsr_mdir_cmp(&o->mdir, mdir) == 0
                    && o->mdir.rbyd.cksum != mdir->rbyd.cksum) {
                LFS_ERROR("Found mdir cksum mismatch %"PRId32" "
                            "0x{%"PRIx32",%"PRIx32"}, "
                            "cksum %08"PRIx32" (!= %08"PRIx32")",
                        lfsr_dbgmbid(lfs, mdir->mid),
                        mdir->rbyd.blocks[0],
                        mdir->rbyd.blocks[1],
                        mdir->rbyd.cksum,
                        o->mdir.rbyd.cksum);
                return LFS_ERR_CORRUPT;
            }
        }

        // recalculate gcksum
        t->gcksum ^= mdir->rbyd.cksum;
    }

    // validate btree nodes?
    //
    // this may end up revalidating some btree nodes when ckfetches
    // is enabled, but we need to revalidate cached btree nodes or
    // we risk missing errors in ckmeta scans
    if ((lfsr_t_isckmeta(t->b.o.flags)
                || lfsr_t_isckdata(t->b.o.flags))
            && tag == LFSR_TAG_BRANCH) {
        lfsr_rbyd_t *rbyd = (lfsr_rbyd_t*)bptr.data.u.buffer;
        err = lfsr_rbyd_fetchck(lfs, rbyd,
                rbyd->blocks[0], rbyd->trunk,
                rbyd->cksum);
        if (err) {
            return err;
        }
    }

    // validate data blocks?
    if (lfsr_t_isckdata(t->b.o.flags)
            && tag == LFSR_TAG_BLOCK) {
        err = lfsr_bptr_ck(lfs, &bptr);
        if (err) {
            return err;
        }
    }

    if (tag_) {
        *tag_ = tag;
    }
    if (bptr_) {
        *bptr_ = bptr;
    }
    return 0;

eot:;
    // compare gcksum with in-RAM gcksum
    if ((lfsr_t_isckmeta(t->b.o.flags)
                || lfsr_t_isckdata(t->b.o.flags))
            && !lfsr_t_isdirty(t->b.o.flags)
            && !lfsr_t_ismutated(t->b.o.flags)
            && t->gcksum != lfs->gcksum) {
        LFS_ERROR("Found gcksum mismatch, cksum %08"PRIx32" (!= %08"PRIx32")",
                t->gcksum,
                lfs->gcksum);
        return LFS_ERR_CORRUPT;
    }

    // was ckmeta/ckdata successful? we only consider our filesystem
    // checked if we weren't mutated
    if ((lfsr_t_isckmeta(t->b.o.flags)
                || lfsr_t_isckdata(t->b.o.flags))
            && !lfsr_t_ismtreeonly(t->b.o.flags)
            && !lfsr_t_isdirty(t->b.o.flags)
            && !lfsr_t_ismutated(t->b.o.flags)) {
        lfs->flags &= ~LFS_I_CKMETA;
    }
    if (lfsr_t_isckdata(t->b.o.flags)
            && !lfsr_t_ismtreeonly(t->b.o.flags)
            && !lfsr_t_isdirty(t->b.o.flags)
            && !lfsr_t_ismutated(t->b.o.flags)) {
        lfs->flags &= ~LFS_I_CKDATA;
    }

    return LFS_ERR_NOENT;
}

// needed in lfsr_mtree_gc
static int lfsr_mdir_mkconsistent(lfs_t *lfs, lfsr_mdir_t *mdir);
static void lfs_alloc_ckpoint(lfs_t *lfs);
static void lfs_alloc_markfree(lfs_t *lfs);

// high-level mutating traversal, handle extra features that require
// mutation here, upper layers should call lfs_alloc_ckpoint as needed
static int lfsr_mtree_gc(lfs_t *lfs, lfsr_traversal_t *t,
        lfsr_tag_t *tag_, lfsr_bptr_t *bptr_) {
dropped:;
    lfsr_tag_t tag;
    lfsr_bptr_t bptr;
    int err = lfsr_mtree_traverse(lfs, t,
            &tag, &bptr);
    if (err) {
        // end of traversal?
        if (err == LFS_ERR_NOENT) {
            goto eot;
        }
        goto failed;
    }

    // swap dirty/mutated flags while in lfsr_mtree_gc
    t->b.o.flags = lfsr_t_swapdirty(t->b.o.flags);

    // track in-use blocks?
    if (lfsr_t_islookahead(t->b.o.flags)) {
        lfs_alloc_markinuse(lfs, tag, &bptr);
    }

    // mkconsistencing mdirs?
    if (lfsr_t_ismkconsistent(t->b.o.flags)
            && lfsr_t_ismkconsistent(lfs->flags)
            && tag == LFSR_TAG_MDIR) {
        lfsr_mdir_t *mdir = (lfsr_mdir_t*)bptr.data.u.buffer;
        err = lfsr_mdir_mkconsistent(lfs, mdir);
        if (err) {
            goto failed;
        }

        // make sure we clear any zombie flags
        t->b.o.flags &= ~LFS_o_ZOMBIE;

        // did this drop our mdir?
        if (mdir->mid != -1 && mdir->rbyd.weight == 0) {
            // swap back dirty/mutated flags
            t->b.o.flags = lfsr_t_swapdirty(t->b.o.flags);
            // continue traversal
            t->b.o.flags = lfsr_t_settstate(t->b.o.flags, LFSR_TSTATE_MDIRS);
            goto dropped;
        }
    }

    // compacting mdirs?
    if (lfsr_t_iscompact(t->b.o.flags)
            && tag == LFSR_TAG_MDIR
            // exceed compaction threshold?
            && lfsr_rbyd_eoff(&((lfsr_mdir_t*)bptr.data.u.buffer)->rbyd)
                > ((lfs->cfg->gc_compact_thresh)
                    ? lfs->cfg->gc_compact_thresh
                    : lfs->cfg->block_size - lfs->cfg->block_size/8)) {
        lfsr_mdir_t *mdir = (lfsr_mdir_t*)bptr.data.u.buffer;
        LFS_INFO("Compacting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "(%"PRId32" > %"PRId32")",
                lfsr_dbgmbid(lfs, mdir->mid),
                mdir->rbyd.blocks[0],
                mdir->rbyd.blocks[1],
                lfsr_rbyd_eoff(&mdir->rbyd),
                (lfs->cfg->gc_compact_thresh)
                    ? lfs->cfg->gc_compact_thresh
                    : lfs->cfg->block_size - lfs->cfg->block_size/8);

        // checkpoint the allocator
        lfs_alloc_ckpoint(lfs);

        // compact the mdir
        err = lfsr_mdir_compact(lfs, mdir);
        if (err) {
            goto failed;
        }
    }

    // swap back dirty/mutated flags
    t->b.o.flags = lfsr_t_swapdirty(t->b.o.flags);
    if (tag_) {
        *tag_ = tag;
    }
    if (bptr_) {
        *bptr_ = bptr;
    }
    return 0;

eot:;
    // was lookahead scan successful?
    if (lfsr_t_islookahead(t->b.o.flags)
            && !lfsr_t_ismtreeonly(t->b.o.flags)
            && !lfsr_t_isdirty(t->b.o.flags)
            && !lfsr_t_ismutated(t->b.o.flags)) {
        lfs_alloc_markfree(lfs);
    }

    // was mkconsistent successful?
    if (lfsr_t_ismkconsistent(t->b.o.flags)
            && !lfsr_t_isdirty(t->b.o.flags)) {
        lfs->flags &= ~LFS_I_MKCONSISTENT;
    }

    // was compaction successful? note we may need multiple passes if
    // we want to be sure everything is compacted
    if (lfsr_t_iscompact(t->b.o.flags)
            && !lfsr_t_isdirty(t->b.o.flags)
            && !lfsr_t_ismutated(t->b.o.flags)) {
        lfs->flags &= ~LFS_I_COMPACT;
    }

    return LFS_ERR_NOENT;

failed:;
    // swap back dirty/mutated flags
    t->b.o.flags = lfsr_t_swapdirty(t->b.o.flags);
    return err;
}



/// Block allocator ///

// checkpoint the allocator
//
// operations that need to alloc should call this to indicate all in-use
// blocks are either committed into the filesystem or tracked by an opened
// mdir
static void lfs_alloc_ckpoint(lfs_t *lfs) {
    lfs->lookahead.ckpoint = lfs->block_count;
}

// discard any lookahead state, this is necessary if block_count changes
static void lfs_alloc_discard(lfs_t *lfs) {
    lfs->lookahead.size = 0;
    lfs_memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);
}

// mark a block as in-use
static void lfs_alloc_markinuse_(lfs_t *lfs, lfs_block_t block) {
    // translate to lookahead-relative
    lfs_block_t block_ = ((
                (lfs_sblock_t)(block
                        - (lfs->lookahead.window + lfs->lookahead.off))
            // we only need this mess because C's mod is actually rem, and
            // we want real mod in case block_ goes negative
                    % (lfs_sblock_t)lfs->block_count)
                + (lfs_sblock_t)lfs->block_count)
            % (lfs_sblock_t)lfs->block_count;

    if (block_ < 8*lfs->cfg->lookahead_size) {
        // mark as in-use
        lfs->lookahead.buffer[
                    ((lfs->lookahead.off + block_) / 8)
                        % lfs->cfg->lookahead_size]
                |= 1 << ((lfs->lookahead.off + block_) % 8);
    }
}

// mark some filesystem object as in-use
static void lfs_alloc_markinuse(lfs_t *lfs,
        lfsr_tag_t tag, const lfsr_bptr_t *bptr) {
    if (tag == LFSR_TAG_MDIR) {
        lfsr_mdir_t *mdir = (lfsr_mdir_t*)bptr->data.u.buffer;
        lfs_alloc_markinuse_(lfs, mdir->rbyd.blocks[0]);
        lfs_alloc_markinuse_(lfs, mdir->rbyd.blocks[1]);

    } else if (tag == LFSR_TAG_BRANCH) {
        lfsr_rbyd_t *rbyd = (lfsr_rbyd_t*)bptr->data.u.buffer;
        lfs_alloc_markinuse_(lfs, rbyd->blocks[0]);

    } else if (tag == LFSR_TAG_BLOCK) {
        lfs_alloc_markinuse_(lfs, bptr->data.u.disk.block);

    } else {
        LFS_UNREACHABLE();
    }
}

// needed in lfs_alloc_markfree
static lfs_sblock_t lfs_alloc_findfree(lfs_t *lfs);

// mark any not-in-use blocks as free
static void lfs_alloc_markfree(lfs_t *lfs) {
    // make lookahead buffer usable
    lfs->lookahead.size = lfs_min(
            8*lfs->cfg->lookahead_size,
            lfs->lookahead.ckpoint);

    // signal that lookahead is full, this may be cleared by
    // lfs_alloc_findfree
    lfs->flags &= ~LFS_I_LOOKAHEAD;

    // eagerly find the next free block so lookahead scans can make
    // the most progress
    lfs_alloc_findfree(lfs);
}

// increment lookahead buffer
static void lfs_alloc_inc(lfs_t *lfs) {
    LFS_ASSERT(lfs->lookahead.size > 0);

    // clear lookahead as we increment
    lfs->lookahead.buffer[lfs->lookahead.off / 8]
            &= ~(1 << (lfs->lookahead.off % 8));

    // signal that lookahead is no longer full
    lfs->flags |= LFS_I_LOOKAHEAD;

    // increment next/off
    lfs->lookahead.off += 1;
    if (lfs->lookahead.off == 8*lfs->cfg->lookahead_size) {
        lfs->lookahead.off = 0;
        lfs->lookahead.window = (lfs->lookahead.window
                + 8*lfs->cfg->lookahead_size)
                    % lfs->block_count;
    }

    // decrement size/ckpoint
    lfs->lookahead.size -= 1;
    lfs->lookahead.ckpoint -= 1;
}

// find next free block in lookahead buffer, if there is one
static lfs_sblock_t lfs_alloc_findfree(lfs_t *lfs) {
    while (lfs->lookahead.size > 0) {
        if (!(lfs->lookahead.buffer[lfs->lookahead.off / 8]
                & (1 << (lfs->lookahead.off % 8)))) {
            // found a free block
            return (lfs->lookahead.window + lfs->lookahead.off)
                    % lfs->block_count;
        }

        lfs_alloc_inc(lfs);
    }

    return LFS_ERR_NOSPC;
}

static lfs_sblock_t lfs_alloc(lfs_t *lfs, bool erase) {
    while (true) {
        // scan our lookahead buffer for free blocks
        lfs_sblock_t block = lfs_alloc_findfree(lfs);
        if (block < 0 && block != LFS_ERR_NOSPC) {
            return block;
        }

        if (block != LFS_ERR_NOSPC) {
            // we should never alloc blocks {0,1}
            LFS_ASSERT(block != 0 && block != 1);

            // erase requested?
            if (erase) {
                int err = lfsr_bd_erase(lfs, block);
                if (err) {
                    // bad erase? try another block
                    if (err == LFS_ERR_CORRUPT) {
                        lfs_alloc_inc(lfs);
                        continue;
                    }
                    return err;
                }
            }

            // eagerly find the next free block to maximize how many blocks
            // lfs_alloc_ckpoint makes available for scanning
            lfs_alloc_inc(lfs);
            lfs_alloc_findfree(lfs);

            #ifdef LFS_DBGALLOCS
            LFS_DEBUG("Allocated block 0x%"PRIx32", "
                        "lookahead %"PRId32"/%"PRId32"/%"PRId32,
                    block,
                    lfs->lookahead.size,
                    lfs->lookahead.ckpoint,
                    lfs->cfg->block_count);
            #endif
            return block;
        }

        // in order to keep our block allocator from spinning forever when our
        // filesystem is full, we mark points where there are no in-flight
        // allocations with a checkpoint before starting a set of allocations
        //
        // if we've looked at all blocks since the last checkpoint, we report
        // the filesystem as out of storage
        //
        if (lfs->lookahead.ckpoint <= 0) {
            LFS_ERROR("No more free space "
                        "(lookahead %"PRId32"/%"PRId32"/%"PRId32")",
                    lfs->lookahead.size,
                    lfs->lookahead.ckpoint,
                    lfs->cfg->block_count);
            return LFS_ERR_NOSPC;
        }

        // no blocks in our lookahead buffer?
        //
        // traverse the filesystem, building up knowledge of what blocks are
        // in-use in the next lookahead window
        //
        lfsr_traversal_t t;
        lfsr_traversal_init(&t, LFS_T_LOOKAHEAD);
        while (true) {
            lfsr_tag_t tag;
            lfsr_bptr_t bptr;
            int err = lfsr_mtree_traverse(lfs, &t,
                    &tag, &bptr);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }

            // track in-use blocks
            lfs_alloc_markinuse(lfs, tag, &bptr);
        }

        // mark anything not seen as free
        lfs_alloc_markfree(lfs);
    }
}




/// Directory operations ///

int lfsr_mkdir(lfs_t *lfs, const char *path) {
    // prepare our filesystem for writing
    int err = lfsr_fs_mkconsistent(lfs);
    if (err) {
        return err;
    }

    // lookup our parent
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    lfsr_did_t did;
    err = lfsr_mtree_pathlookup(lfs, &path,
            &mdir, &tag, &did);
    if (err && !(err == LFS_ERR_NOENT && lfsr_path_islast(path))) {
        return err;
    }
    // already exists? stickynotes don't really exist
    bool exists = (err != LFS_ERR_NOENT);
    if (exists && tag != LFSR_TAG_STICKYNOTE) {
        return LFS_ERR_EXIST;
    }

    // check that name fits
    lfs_size_t name_len = lfsr_path_namelen(path);
    if (name_len > lfs->name_limit) {
        return LFS_ERR_NAMETOOLONG;
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
    lfsr_did_t dmask
            = (1 << lfs_min(
                lfs_nlog2(lfsr_mtree_weight(lfs) >> lfs->mbits)
                    + lfs_nlog2(lfs->cfg->block_size/32),
                31)
            ) - 1;
    lfsr_did_t did_ = (did ^ lfs_crc32c(0, path, name_len)) & dmask;

    // check if we have a collision, if we do, search for the next
    // available did
    while (true) {
        err = lfsr_mtree_namelookup(lfs, did_, NULL, 0,
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
    lfs_alloc_ckpoint(lfs);
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_RATTRS(
            LFSR_RATTR_LEB128(
                LFSR_TAG_BOOKMARK, +1, did_)));
    if (err) {
        return err;
    }
    LFS_ASSERT(lfs->grm.mids[0] == mdir.mid);

    // committing our bookmark may have changed the mid of our metadata entry,
    // we need to look it up again, we can at least avoid the full path walk
    err = lfsr_mtree_namelookup(lfs, did, path, name_len,
            &mdir, NULL, NULL);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    LFS_ASSERT((exists) ? !err : err == LFS_ERR_NOENT);

    // commit our new directory into our parent, zeroing the grm in the
    // process
    lfsr_grm_pop(lfs);
    lfs_alloc_ckpoint(lfs);
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_RATTRS(
            LFSR_RATTR_NAME(
                LFSR_TAG_MASK12 | LFSR_TAG_DIR, (!exists) ? +1 : 0,
                did, path, name_len),
            LFSR_RATTR_LEB128(
                LFSR_TAG_DID, 0, did_)));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        // mark any clobbered uncreats as zombied
        if (exists
                && lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == mdir.mid) {
            o->flags = (o->flags & ~LFS_o_UNCREAT)
                    | LFS_o_ZOMBIE
                    | LFS_o_UNSYNC
                    | LFS_O_DESYNC;

        // update dir positions
        } else if (!exists
                && lfsr_o_type(o->flags) == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == did
                && o->mdir.mid >= mdir.mid) {
            ((lfsr_dir_t*)o)->pos += 1;
        }
    }

    return 0;
}

// push a did to grm, but only if the directory is empty
static int lfsr_grm_pushdid(lfs_t *lfs, lfsr_did_t did) {
    // first lookup the bookmark entry
    lfsr_mdir_t bookmark_mdir;
    int err = lfsr_mtree_namelookup(lfs, did, NULL, 0,
            &bookmark_mdir, NULL, NULL);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_NOENT);
        return err;
    }
    lfsr_mid_t bookmark_mid = bookmark_mdir.mid;

    // check that the directory is empty
    bookmark_mdir.mid += 1;
    if (lfsr_mrid(lfs, bookmark_mdir.mid)
            >= (lfsr_srid_t)bookmark_mdir.rbyd.weight) {
        err = lfsr_mtree_lookupleaf(lfs,
                lfsr_mbid(lfs, bookmark_mdir.mid-1) + 1,
                &bookmark_mdir);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                goto empty;
            }
            return err;
        }
    }

    lfsr_data_t data;
    err = lfsr_mdir_lookup(lfs, &bookmark_mdir,
            LFSR_TAG_MASK8 | LFSR_TAG_NAME,
            NULL, &data);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_NOENT);
        return err;
    }

    lfsr_did_t did_;
    err = lfsr_data_readleb128(lfs, &data, &did_);
    if (err) {
        return err;
    }

    if (did_ == did) {
        return LFS_ERR_NOTEMPTY;
    }

empty:;
    lfsr_grm_push(lfs, bookmark_mid);
    return 0;
}

// needed in lfsr_remove
static int lfsr_fs_fixgrm(lfs_t *lfs);

int lfsr_remove(lfs_t *lfs, const char *path) {
    // prepare our filesystem for writing
    int err = lfsr_fs_mkconsistent(lfs);
    if (err) {
        return err;
    }

    // lookup our entry
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    lfsr_did_t did;
    err = lfsr_mtree_pathlookup(lfs, &path,
            &mdir, &tag, &did);
    if (err) {
        return err;
    }
    // stickynotes don't really exist
    if (tag == LFSR_TAG_STICKYNOTE) {
        return LFS_ERR_NOENT;
    }
    // we can't remove unknown types or else we may leak resources
    if (tag != LFSR_TAG_REG && tag != LFSR_TAG_DIR) {
        return LFS_ERR_NOTSUP;
    }

    // trying to remove the root dir?
    if (mdir.mid == -1) {
        return LFS_ERR_INVAL;
    }

    // if we're removing a directory, we need to also remove the
    // bookmark entry
    lfsr_did_t did_ = 0;
    if (tag == LFSR_TAG_DIR) {
        // first lets figure out the did
        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_DID,
                NULL, &data);
        if (err) {
            return err;
        }

        err = lfsr_data_readleb128(lfs, &data, &did_);
        if (err) {
            return err;
        }

        // mark bookmark for removal with grm
        err = lfsr_grm_pushdid(lfs, did_);
        if (err) {
            return err;
        }
    }

    // are we removing an opened file?
    bool zombie = lfsr_omdir_ismidopen(lfs, mdir.mid, -1);

    // remove the metadata entry
    lfs_alloc_ckpoint(lfs);
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_RATTRS(
            // create a stickynote if zombied
            //
            // we use a create+delete here to also clear any rattrs
            // and trim the entry size
            (zombie)
                ? LFSR_RATTR_NAME(
                    LFSR_TAG_MASK12 | LFSR_TAG_STICKYNOTE, 0,
                    did, path, lfsr_path_namelen(path))
                : LFSR_RATTR(
                    LFSR_TAG_RM, -1)));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        // mark any clobbered uncreats as zombied
        if (zombie
                && lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == mdir.mid) {
            o->flags |= LFS_o_UNCREAT
                    | LFS_o_ZOMBIE
                    | LFS_o_UNSYNC
                    | LFS_O_DESYNC;

        // mark any removed dirs as zombied
        } else if (did_
                && lfsr_o_type(o->flags) == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == did_) {
            o->flags |= LFS_o_ZOMBIE;

        // update dir positions
        } else if (lfsr_o_type(o->flags) == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == did
                && o->mdir.mid >= mdir.mid) {
            if (lfsr_o_iszombie(o->flags)) {
                o->flags &= ~LFS_o_ZOMBIE;
            } else {
                ((lfsr_dir_t*)o)->pos -= 1;
            }

        // clobber entangled traversals
        } else if (lfsr_o_type(o->flags) == LFS_TYPE_TRAVERSAL) {
            if (lfsr_o_iszombie(o->flags)) {
                o->flags &= ~LFS_o_ZOMBIE;
                o->mdir.mid -= 1;
                lfsr_traversal_clobber(lfs, (lfsr_traversal_t*)o);
            }
        }
    }

    // if we were a directory, we need to clean up, fortunately we can leave
    // this up to lfsr_fs_fixgrm
    err = lfsr_fs_fixgrm(lfs);
    if (err) {
        // we did complete the remove, so we shouldn't error here, best
        // we can do is log this
        LFS_WARN("Failed to clean up grm (%d)", err);
    }

    return 0;
}

int lfsr_rename(lfs_t *lfs, const char *old_path, const char *new_path) {
    // prepare our filesystem for writing
    int err = lfsr_fs_mkconsistent(lfs);
    if (err) {
        return err;
    }

    // lookup old entry
    lfsr_mdir_t old_mdir;
    lfsr_tag_t old_tag;
    lfsr_did_t old_did;
    err = lfsr_mtree_pathlookup(lfs, &old_path,
            &old_mdir, &old_tag, &old_did);
    if (err) {
        return err;
    }
    // stickynotes don't really exist
    if (old_tag == LFSR_TAG_STICKYNOTE) {
        return LFS_ERR_NOENT;
    }
    // we can't rename unknown types or else we may leak resources
    if (old_tag != LFSR_TAG_REG && old_tag != LFSR_TAG_DIR) {
        return LFS_ERR_NOTSUP;
    }

    // trying to rename the root?
    if (old_mdir.mid == -1) {
        return LFS_ERR_INVAL;
    }

    // lookup new entry
    lfsr_mdir_t new_mdir;
    lfsr_tag_t new_tag;
    lfsr_did_t new_did;
    err = lfsr_mtree_pathlookup(lfs, &new_path,
            &new_mdir, &new_tag, &new_did);
    if (err && !(err == LFS_ERR_NOENT && lfsr_path_islast(new_path))) {
        return err;
    }
    bool exists = (err != LFS_ERR_NOENT);

    // there are a few cases we need to watch out for
    lfs_size_t new_name_len = lfsr_path_namelen(new_path);
    lfsr_did_t new_did_ = 0;
    if (!exists) {
        // if we're a file, don't allow trailing slashes
        if (old_tag != LFSR_TAG_DIR && lfsr_path_isdir(new_path)) {
              return LFS_ERR_NOTDIR;
        }

        // check that name fits
        if (new_name_len > lfs->name_limit) {
            return LFS_ERR_NAMETOOLONG;
        }

    } else {
        // trying to rename the root?
        if (new_mdir.mid == -1) {
            return LFS_ERR_INVAL;
        }

        // renaming different types is an error
        //
        // unless we found a stickynote, these don't really exist
        if (old_tag != new_tag && new_tag != LFSR_TAG_STICKYNOTE) {
            return (new_tag == LFSR_TAG_DIR)
                        ? LFS_ERR_ISDIR
                    : (new_tag == LFSR_TAG_REG)
                        ? LFS_ERR_NOTDIR
                        : LFS_ERR_NOTSUP;
        }

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
                    NULL, &data);
            if (err) {
                return err;
            }

            err = lfsr_data_readleb128(lfs, &data, &new_did_);
            if (err) {
                return err;
            }

            // mark bookmark for removal with grm
            err = lfsr_grm_pushdid(lfs, new_did_);
            if (err) {
                return err;
            }
        }
    }

    // mark old entry for removal with a grm
    lfsr_grm_push(lfs, old_mdir.mid);

    // rename our entry, copying all tags associated with the old rid to the
    // new rid, while also marking the old rid for removal
    lfs_alloc_ckpoint(lfs);
    err = lfsr_mdir_commit(lfs, &new_mdir, LFSR_RATTRS(
            LFSR_RATTR_NAME(
                LFSR_TAG_MASK12 | old_tag, (!exists) ? +1 : 0,
                new_did, new_path, new_name_len),
            LFSR_RATTR_MOVE(&old_mdir)));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        // mark any clobbered uncreats as zombied
        if (exists
                && lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == new_mdir.mid) {
            o->flags = (o->flags & ~LFS_o_UNCREAT)
                    | LFS_o_ZOMBIE
                    | LFS_o_UNSYNC
                    | LFS_O_DESYNC;

        // update moved files with the new mdir
        } else if (lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == lfs->grm.mids[0]) {
            o->mdir = new_mdir;

        // mark any removed dirs as zombied
        } else if (new_did_
                && lfsr_o_type(o->flags) == LFS_TYPE_DIR
                && ((lfsr_dir_t*)o)->did == new_did_) {
            o->flags |= LFS_o_ZOMBIE;

        // update dir positions
        } else if (lfsr_o_type(o->flags) == LFS_TYPE_DIR) {
            if (!exists
                    && ((lfsr_dir_t*)o)->did == new_did
                    && o->mdir.mid >= new_mdir.mid) {
                ((lfsr_dir_t*)o)->pos += 1;
            }

            if (((lfsr_dir_t*)o)->did == old_did
                    && o->mdir.mid >= lfs->grm.mids[0]) {
                if (o->mdir.mid == lfs->grm.mids[0]) {
                    o->mdir.mid += 1;
                } else {
                    ((lfsr_dir_t*)o)->pos -= 1;
                }
            }

        // clobber entangled traversals
        } else if (lfsr_o_type(o->flags) == LFS_TYPE_TRAVERSAL
                && ((exists && o->mdir.mid == new_mdir.mid)
                    || o->mdir.mid == lfs->grm.mids[0])) {
            lfsr_traversal_clobber(lfs, (lfsr_traversal_t*)o);
        }
    }

    // we need to clean up any pending grms, fortunately we can leave
    // this up to lfsr_fs_fixgrm
    err = lfsr_fs_fixgrm(lfs);
    if (err) {
        // we did complete the remove, so we shouldn't error here, best
        // we can do is log this
        LFS_WARN("Failed to clean up grm (%d)", err);
    }

    return 0;
}

// this just populates the info struct based on what we found
static int lfsr_stat_(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_tag_t tag, lfsr_data_t name,
        struct lfs_info *info) {
    // get file type from the tag
    info->type = lfsr_tag_subtype(tag);

    // read the file name
    LFS_ASSERT(lfsr_data_size(name) <= LFS_NAME_MAX);
    lfs_ssize_t name_len = lfsr_data_read(lfs, &name,
            info->name, LFS_NAME_MAX);
    if (name_len < 0) {
        return name_len;
    }
    info->name[name_len] = '\0';

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

        // may be a moss (simple inlined data)
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
    int err = lfsr_mtree_pathlookup(lfs, &path,
            &mdir, &tag, NULL);
    if (err) {
        return err;
    }
    // stickynotes don't really exist
    if (tag == LFSR_TAG_STICKYNOTE) {
        return LFS_ERR_NOENT;
    }

    // special case for root
    if (mdir.mid == -1) {
        lfs_strcpy(info->name, "/");
        info->type = LFS_TYPE_DIR;
        info->size = 0;
        return 0;
    }

    // fill out our info struct
    return lfsr_stat_(lfs, &mdir,
            tag, LFSR_DATA_BUF(path, lfsr_path_namelen(path)),
            info);
}

// needed in lfsr_dir_open
static int lfsr_dir_rewind_(lfs_t *lfs, lfsr_dir_t *dir);

int lfsr_dir_open(lfs_t *lfs, lfsr_dir_t *dir, const char *path) {
    // already open?
    LFS_ASSERT(!lfsr_omdir_isopen(lfs, &dir->o));

    // setup dir state
    dir->o.flags = lfsr_o_settype(0, LFS_TYPE_DIR);

    // lookup our directory
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    int err = lfsr_mtree_pathlookup(lfs, &path,
            &mdir, &tag, NULL);
    if (err) {
        return err;
    }
    // stickynotes don't really exist
    if (tag == LFSR_TAG_STICKYNOTE) {
        return LFS_ERR_NOENT;
    }

    // read our did from the mdir, unless we're root
    if (mdir.mid == -1) {
        dir->did = 0;

    } else {
        // not a directory?
        if (tag != LFSR_TAG_DIR) {
            return LFS_ERR_NOTDIR;
        }

        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, LFSR_TAG_DID,
                NULL, &data);
        if (err) {
            return err;
        }

        err = lfsr_data_readleb128(lfs, &data, &dir->did);
        if (err) {
            return err;
        }
    }

    // let rewind initialize the pos state
    err = lfsr_dir_rewind_(lfs, dir);
    if (err) {
        return err;
    }

    // add to tracked mdirs
    lfsr_omdir_open(lfs, &dir->o);
    return 0;
}

int lfsr_dir_close(lfs_t *lfs, lfsr_dir_t *dir) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &dir->o));

    // remove from tracked mdirs
    lfsr_omdir_close(lfs, &dir->o);
    return 0;
}

int lfsr_dir_read(lfs_t *lfs, lfsr_dir_t *dir, struct lfs_info *info) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &dir->o));

    // was our dir removed?
    if (lfsr_o_iszombie(dir->o.flags)) {
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

    while (true) {
        // next mdir?
        if (lfsr_mrid(lfs, dir->o.mdir.mid)
                >= (lfsr_srid_t)dir->o.mdir.rbyd.weight) {
            int err = lfsr_mtree_lookupleaf(lfs,
                    lfsr_mbid(lfs, dir->o.mdir.mid-1) + 1,
                    &dir->o.mdir);
            if (err) {
                return err;
            }
        }

        // lookup the next name tag
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_mdir_lookup(lfs, &dir->o.mdir,
                LFSR_TAG_MASK8 | LFSR_TAG_NAME,
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

        // skip stickynotes, we pretend these don't exist
        if (tag == LFSR_TAG_STICKYNOTE) {
            dir->o.mdir.mid += 1;
            dir->pos += 1;
            continue;
        }

        // fill out our info struct
        err = lfsr_stat_(lfs, &dir->o.mdir, tag, data,
                info);
        if (err) {
            return err;
        }

        // eagerly set to next entry
        dir->o.mdir.mid += 1;
        dir->pos += 1;
        return 0;
    }
}

int lfsr_dir_seek(lfs_t *lfs, lfsr_dir_t *dir, lfs_soff_t off) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &dir->o));

    // do nothing if removed
    if (lfsr_o_iszombie(dir->o.flags)) {
        return 0;
    }

    // first rewind
    int err = lfsr_dir_rewind_(lfs, dir);
    if (err) {
        return err;
    }

    // then seek to the requested offset
    //
    // note the -2 to adjust for dot entries
    lfs_off_t off_ = off - 2;
    while (off_ > 0) {
        // next mdir?
        if (lfsr_mrid(lfs, dir->o.mdir.mid)
                >= (lfsr_srid_t)dir->o.mdir.rbyd.weight) {
            int err = lfsr_mtree_lookupleaf(lfs,
                    lfsr_mbid(lfs, dir->o.mdir.mid-1) + 1,
                    &dir->o.mdir);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }
        }

        lfs_off_t d = lfs_min(
                off_,
                dir->o.mdir.rbyd.weight
                    - lfsr_mrid(lfs, dir->o.mdir.mid));
        dir->o.mdir.mid += d;
        off_ -= d;
    }

    dir->pos = off;
    return 0;
}

lfs_soff_t lfsr_dir_tell(lfs_t *lfs, lfsr_dir_t *dir) {
    (void)lfs;
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &dir->o));

    return dir->pos;
}

static int lfsr_dir_rewind_(lfs_t *lfs, lfsr_dir_t *dir) {
    // do nothing if removed
    if (lfsr_o_iszombie(dir->o.flags)) {
        return 0;
    }

    // lookup our bookmark in the mtree
    int err = lfsr_mtree_namelookup(lfs, dir->did, NULL, 0,
            &dir->o.mdir, NULL, NULL);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_NOENT);
        return err;
    }

    // eagerly set to next entry
    dir->o.mdir.mid += 1;
    // reset pos
    dir->pos = 0;
    return 0;
}

int lfsr_dir_rewind(lfs_t *lfs, lfsr_dir_t *dir) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &dir->o));

    return lfsr_dir_rewind_(lfs, dir);
}



/// Custom attribute stuff ///

static int lfsr_lookupattr(lfs_t *lfs, const char *path, uint8_t type,
        lfsr_mdir_t *mdir_, lfsr_data_t *data_) {
    // lookup our entry
    lfsr_tag_t tag;
    int err = lfsr_mtree_pathlookup(lfs, &path,
            mdir_, &tag, NULL);
    if (err) {
        return err;
    }
    // stickynotes don't really exist
    if (tag == LFSR_TAG_STICKYNOTE) {
        return LFS_ERR_NOENT;
    }

    // lookup our attr
    err = lfsr_mdir_lookup(lfs, mdir_, LFSR_TAG_ATTR(type),
            NULL, data_);
    if (err) {
        if (err == LFS_ERR_NOENT) {
            return LFS_ERR_NOATTR;
        }
        return err;
    }

    return 0;
}

lfs_ssize_t lfsr_getattr(lfs_t *lfs, const char *path, uint8_t type,
        void *buffer, lfs_size_t size) {
    // lookup our attr
    lfsr_mdir_t mdir;
    lfsr_data_t data;
    int err = lfsr_lookupattr(lfs, path, type,
            &mdir, &data);
    if (err) {
        return err;
    }

    // read the attr
    return lfsr_data_read(lfs, &data, buffer, size);
}

lfs_ssize_t lfsr_sizeattr(lfs_t *lfs, const char *path, uint8_t type) {
    // lookup our attr
    lfsr_mdir_t mdir;
    lfsr_data_t data;
    int err = lfsr_lookupattr(lfs, path, type,
            &mdir, &data);
    if (err) {
        return err;
    }

    // return the attr size
    return lfsr_data_size(data);
}

int lfsr_setattr(lfs_t *lfs, const char *path, uint8_t type,
        const void *buffer, lfs_size_t size) {
    // prepare our filesystem for writing
    int err = lfsr_fs_mkconsistent(lfs);
    if (err) {
        return err;
    }

    // lookup our attr
    lfsr_mdir_t mdir;
    lfsr_data_t data;
    err = lfsr_lookupattr(lfs, path, type,
            &mdir, &data);
    if (err && err != LFS_ERR_NOATTR) {
        return err;
    }

    // commit our attr
    lfs_alloc_ckpoint(lfs);
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_RATTRS(
            LFSR_RATTR_BUF(
                LFSR_TAG_ATTR(type), 0,
                buffer, size)));
    if (err) {
        return err;
    }

    // update any opened files tracking custom attrs
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (!(lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == mdir.mid
                && !lfsr_o_isdesync(o->flags))) {
            continue;
        }

        lfsr_file_t *file = (lfsr_file_t*)o;
        for (lfs_size_t i = 0; i < file->cfg->attr_count; i++) {
            if (!(file->cfg->attrs[i].type == type
                    && !lfsr_o_iswronly(file->cfg->attrs[i].flags))) {
                continue;
            }

            lfs_size_t d = lfs_min(size, file->cfg->attrs[i].buffer_size);
            memcpy(file->cfg->attrs[i].buffer, buffer, d);
            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = d;
            }
        }
    }


    return 0;
}

int lfsr_removeattr(lfs_t *lfs, const char *path, uint8_t type) {
    // prepare our filesystem for writing
    int err = lfsr_fs_mkconsistent(lfs);
    if (err) {
        return err;
    }

    // lookup our attr
    lfsr_mdir_t mdir;
    err = lfsr_lookupattr(lfs, path, type,
            &mdir, NULL);
    if (err) {
        return err;
    }

    // commit our removal
    lfs_alloc_ckpoint(lfs);
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_RATTRS(
            LFSR_RATTR(
                LFSR_TAG_RM | LFSR_TAG_ATTR(type), 0)));
    if (err) {
        return err;
    }

    // update any opened files tracking custom attrs
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (!(lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == mdir.mid
                && !lfsr_o_isdesync(o->flags))) {
            continue;
        }

        lfsr_file_t *file = (lfsr_file_t*)o;
        for (lfs_size_t i = 0; i < file->cfg->attr_count; i++) {
            if (!(file->cfg->attrs[i].type == type
                    && !lfsr_o_iswronly(file->cfg->attrs[i].flags))) {
                continue;
            }

            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = LFS_ERR_NOATTR;
            }
        }
    }

    return 0;
}




/// File operations ///

// file helpers
static inline lfs_size_t lfsr_file_cachesize(lfs_t *lfs,
        const lfsr_file_t *file) {
    return (file->cfg->cache_size)
            ? file->cfg->cache_size
            : lfs->cfg->file_cache_size;
}

static inline lfs_off_t lfsr_file_size_(const lfsr_file_t *file) {
    return lfs_max(
            file->cache.pos + file->cache.size,
            file->b.shrub.weight);
}

// file operations

static int lfsr_file_fetch(lfs_t *lfs, lfsr_file_t *file, bool trunc) {
    // default data state
    lfsr_bshrub_init(&file->b);
    // discard the current cache
    file->cache.pos = 0;
    file->cache.size = 0;
    // mark as flushed
    file->b.o.flags &= ~LFS_o_UNFLUSH;

    // don't bother reading disk if we're not created or truncating
    if (!lfsr_o_isuncreat(file->b.o.flags) && !trunc) {
        // lookup the file struct, if there is one
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_mdir_lookupnext(lfs, &file->b.o.mdir, LFSR_TAG_DATA,
                &tag, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // note many of these functions leave bshrub undefined if
        // there is an error, so we first read into the staging
        // bshrub
        file->b.shrub_ = file->b.shrub;

        // may be a bshrub (inlined btree)
        if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
            err = lfsr_data_readshrub(lfs, &data, &file->b.o.mdir,
                    &file->b.shrub_);
            if (err) {
                return err;
            }

        // or a btree
        } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BTREE) {
            err = lfsr_data_fetchbtree(lfs, &data,
                    &file->b.shrub_);
            if (err) {
                return err;
            }
        }

        // update the bshrub
        file->b.shrub = file->b.shrub_;

        // mark as synced
        file->b.o.flags &= ~LFS_o_UNSYNC;
    }

    // try to fetch any custom attributes
    for (lfs_size_t i = 0; i < file->cfg->attr_count; i++) {
        // skip writeonly attrs
        if (lfsr_o_iswronly(file->cfg->attrs[i].flags)) {
            continue;
        }

        // don't bother reading disk if we're not created yet
        if (lfsr_o_isuncreat(file->b.o.flags)) {
            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = LFS_ERR_NOATTR;
            }
            continue;
        }

        // lookup the attr
        lfsr_data_t data;
        int err = lfsr_mdir_lookup(lfs, &file->b.o.mdir,
                LFSR_TAG_ATTR(file->cfg->attrs[i].type),
                NULL, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // read the attr, if it exists
        if (err == LFS_ERR_NOENT
                // awkward case here if buffer_size is LFS_ERR_NOATTR
                || file->cfg->attrs[i].buffer_size == LFS_ERR_NOATTR) {
            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = LFS_ERR_NOATTR;
            }
        } else {
            lfs_ssize_t d = lfsr_data_read(lfs, &data,
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

// needed in lfsr_file_opencfg
static void lfsr_file_close_(lfs_t *lfs, const lfsr_file_t *file);
static int lfsr_file_ck(lfs_t *lfs, const lfsr_file_t *file,
        uint32_t flags);

int lfsr_file_opencfg(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs_file_config *cfg) {
    // already open?
    LFS_ASSERT(!lfsr_omdir_isopen(lfs, &file->b.o));
    // don't allow the forbidden mode!
    LFS_ASSERT((flags & 3) != 3);
    // unknown flags?
    LFS_ASSERT((flags & ~(
            LFS_O_RDONLY
                | LFS_O_WRONLY
                | LFS_O_RDWR
                | LFS_O_CREAT
                | LFS_O_EXCL
                | LFS_O_TRUNC
                | LFS_O_APPEND
                | LFS_O_FLUSH
                | LFS_O_SYNC
                | LFS_O_DESYNC
                | LFS_O_CKMETA
                | LFS_O_CKDATA)) == 0);
    // writeable files require a writeable filesystem
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags) || lfsr_o_isrdonly(flags));
    // these flags require a writable file
    LFS_ASSERT(!lfsr_o_isrdonly(flags) || !lfsr_o_iscreat(flags));
    LFS_ASSERT(!lfsr_o_isrdonly(flags) || !lfsr_o_isexcl(flags));
    LFS_ASSERT(!lfsr_o_isrdonly(flags) || !lfsr_o_istrunc(flags));
    for (lfs_size_t i = 0; i < cfg->attr_count; i++) {
        // these flags require a writable attr
        LFS_ASSERT(!lfsr_o_isrdonly(cfg->attrs[i].flags)
                || !lfsr_o_iscreat(cfg->attrs[i].flags));
        LFS_ASSERT(!lfsr_o_isrdonly(cfg->attrs[i].flags)
                || !lfsr_o_isexcl(cfg->attrs[i].flags));
    }

    if (!lfsr_o_isrdonly(flags)) {
        // prepare our filesystem for writing
        int err = lfsr_fs_mkconsistent(lfs);
        if (err) {
            return err;
        }
    }

    // setup file state
    file->cfg = cfg;
    file->b.o.flags = lfsr_o_settype(flags, LFS_TYPE_REG)
            // mounted with LFS_M_FLUSH/SYNC? implies LFS_O_FLUSH/SYNC
            | (lfs->flags & (LFS_M_FLUSH | LFS_M_SYNC))
            // default to unflushed for orphans/truncated files
            | LFS_o_UNFLUSH;
    file->pos = 0;
    file->eblock = 0;
    file->eoff = -1;

    // lookup our parent
    lfsr_tag_t tag;
    lfsr_did_t did;
    int err = lfsr_mtree_pathlookup(lfs, &path,
            &file->b.o.mdir, &tag, &did);
    if (err && !(err == LFS_ERR_NOENT && lfsr_path_islast(path))) {
        return err;
    }
    bool exists = err != LFS_ERR_NOENT;

    // creating a new entry?
    if (!exists || tag == LFSR_TAG_STICKYNOTE) {
        if (!lfsr_o_iscreat(flags)) {
            return LFS_ERR_NOENT;
        }
        LFS_ASSERT(!lfsr_o_isrdonly(flags));

        // we're a file, don't allow trailing slashes
        if (lfsr_path_isdir(path)) {
            return LFS_ERR_NOTDIR;
        }

        // if we're EXCL and we found a stickynote, check if the file
        // is open and not zombied/desynced
        //
        // we error here even though the file isn't created yet so
        // EXCL only lets one create through (ignoring desync+sync
        // shenanigans)
        if (exists
                && lfsr_o_isexcl(flags)
                && lfsr_omdir_ismidopen(lfs, file->b.o.mdir.mid,
                    ~(LFS_o_ZOMBIE | LFS_O_DESYNC))) {
            return LFS_ERR_EXIST;
        }

        // create a stickynote entry if we don't have one, this reserves the
        // mid until first sync
        if (!exists) {
            // check that name fits
            lfs_size_t name_len = lfsr_path_namelen(path);
            if (name_len > lfs->name_limit) {
                return LFS_ERR_NAMETOOLONG;
            }

            lfs_alloc_ckpoint(lfs);
            err = lfsr_mdir_commit(lfs, &file->b.o.mdir, LFSR_RATTRS(
                    LFSR_RATTR_NAME(
                        LFSR_TAG_STICKYNOTE, +1,
                        did, path, name_len)));
            if (err) {
                return err;
            }

            // update dir positions
            for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
                if (lfsr_o_type(o->flags) == LFS_TYPE_DIR
                        && ((lfsr_dir_t*)o)->did == did
                        && o->mdir.mid >= file->b.o.mdir.mid) {
                    ((lfsr_dir_t*)o)->pos += 1;
                }
            }
        }

        // mark as uncreated and unsynced, we need to convert to reg file
        // on first sync
        file->b.o.flags |= LFS_o_UNCREAT | LFS_o_UNSYNC;

    } else {
        // wanted to create a new entry?
        if (lfsr_o_isexcl(flags)) {
            return LFS_ERR_EXIST;
        }

        // wrong type?
        if (tag != LFSR_TAG_REG) {
            return (tag == LFSR_TAG_DIR)
                    ? LFS_ERR_ISDIR
                    : LFS_ERR_NOTSUP;
        }
    }

    // allocate cache if necessary
    if (file->cfg->cache_buffer) {
        file->cache.buffer = file->cfg->cache_buffer;
    } else {
        file->cache.buffer = lfs_malloc(lfsr_file_cachesize(lfs, file));
        if (!file->cache.buffer) {
            return LFS_ERR_NOMEM;
        }
    }
    file->cache.pos = 0;
    file->cache.size = 0;

    // fetch the file struct and custom attrs
    err = lfsr_file_fetch(lfs, file,
            lfsr_o_istrunc(file->b.o.flags));
    if (err) {
        goto failed;
    }

    // check metadata/data for errors?
    if (lfsr_t_isckmeta(flags) || lfsr_t_isckdata(flags)) {
        err = lfsr_file_ck(lfs, file, flags);
        if (err) {
            goto failed;
        }
    }

    // add to tracked mdirs
    lfsr_omdir_open(lfs, &file->b.o);
    return 0;

failed:;
    // clean up resources
    lfsr_file_close_(lfs, file);
    return err;
}

// default file config
static const struct lfs_file_config lfsr_file_defaults = {0};

int lfsr_file_open(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags) {
    return lfsr_file_opencfg(lfs, file, path, flags, &lfsr_file_defaults);
}

// clean up resources
static void lfsr_file_close_(lfs_t *lfs, const lfsr_file_t *file) {
    // clean up memory
    if (!file->cfg->cache_buffer) {
        lfs_free(file->cache.buffer);
    }

    // are we orphaning a file?
    //
    // make sure we check _after_ removing ourselves
    if (lfsr_o_isuncreat(file->b.o.flags)
            && !lfsr_omdir_ismidopen(lfs, file->b.o.mdir.mid, -1)) {
        // this gets a bit messy, since we're not able to write to the
        // filesystem if we're rdonly or desynced, fortunately we have
        // a few tricks

        // first try to push onto our grm queue
        if (lfsr_grm_count(lfs) < 2) {
            lfsr_grm_push(lfs, file->b.o.mdir.mid);

        // fallback to just marking the filesystem as inconsistent
        } else {
            lfs->flags |= LFS_I_MKCONSISTENT;
        }
    }
}

// needed in lfsr_file_close
int lfsr_file_sync(lfs_t *lfs, lfsr_file_t *file);

int lfsr_file_close(lfs_t *lfs, lfsr_file_t *file) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));

    // don't call lfsr_file_sync if we're readonly or desynced
    int err = 0;
    if (!lfsr_o_isrdonly(file->b.o.flags)
            && !lfsr_o_isdesync(file->b.o.flags)) {
        err = lfsr_file_sync(lfs, file);
    }

    // remove from tracked mdirs
    lfsr_omdir_close(lfs, &file->b.o);

    // clean up resources
    lfsr_file_close_(lfs, file);

    return err;
}

// low-level file reading

static int lfsr_file_lookupleaf(lfs_t *lfs, const lfsr_file_t *file,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_rbyd_t *rbyd_, lfsr_srid_t *rid_,
        lfsr_bid_t *weight_, lfsr_bptr_t *bptr_) {
    lfsr_tag_t tag;
    lfsr_data_t data;
    int err = lfsr_bshrub_lookupleaf(lfs, &file->b, bid,
            bid_, rbyd_, rid_, &tag, weight_, &data);
    if (err) {
        return err;
    }
    LFS_ASSERT(tag == LFSR_TAG_DATA
            || tag == LFSR_TAG_BLOCK);

    // decode bptrs
    if (bptr_) {
        if (tag == LFSR_TAG_DATA) {
            bptr_->data = data;
        } else {
            err = lfsr_data_readbptr(lfs, &data, bptr_);
            if (err) {
                return err;
            }
        }
    }
    return 0;
}

static int lfsr_file_lookupnext(lfs_t *lfs, const lfsr_file_t *file,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_bid_t *weight_, lfsr_bptr_t *bptr_) {
    return lfsr_file_lookupleaf(lfs, file, bid,
            bid_, NULL, NULL, weight_, bptr_);
}

static lfs_ssize_t lfsr_file_readnext(lfs_t *lfs, const lfsr_file_t *file,
        lfs_off_t pos, uint8_t *buffer, lfs_size_t size) {
    lfs_off_t pos_ = pos;
    // read one btree entry
    lfsr_bid_t bid;
    lfsr_bid_t weight;
    lfsr_bptr_t bptr;
    int err = lfsr_file_lookupnext(lfs, file, pos_,
            &bid, &weight, &bptr);
    if (err) {
        return err;
    }

    #ifdef LFS_CKFETCHES
    // checking fetches?
    if (lfsr_m_isckfetches(lfs->flags)
            && lfsr_bptr_isbptr(&bptr)) {
        err = lfsr_bptr_ck(lfs, &bptr);
        if (err) {
            return err;
        }
    }
    #endif

    // any data on disk?
    if (pos_ < bid-(weight-1) + lfsr_data_size(bptr.data)) {
        // note one important side-effect here is a strict
        // data hint
        lfs_ssize_t d = lfs_min(
                size,
                lfsr_data_size(bptr.data)
                    - (pos_ - (bid-(weight-1))));
        lfsr_data_t slice = LFSR_DATA_SLICE(bptr.data,
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
    lfs_ssize_t d = lfs_min(size, bid+1 - pos_);
    lfs_memset(buffer, 0, d);

    pos_ += d;
    buffer += d;
    size -= d;

    return pos_ - pos;
}

// high-level file reading

lfs_ssize_t lfsr_file_read(lfs_t *lfs, lfsr_file_t *file,
        void *buffer, lfs_size_t size) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't read from writeonly files
    LFS_ASSERT(!lfsr_o_iswronly(file->b.o.flags));
    LFS_ASSERT(file->pos + size <= 0x7fffffff);

    lfs_off_t pos_ = file->pos;
    uint8_t *buffer_ = buffer;
    while (size > 0 && pos_ < lfsr_file_size_(file)) {
        // keep track of the next highest priority data offset
        lfs_ssize_t d = lfs_min(size, lfsr_file_size_(file) - pos_);

        // any data in our cache?
        if (pos_ < file->cache.pos + file->cache.size
                && file->cache.size != 0) {
            if (pos_ >= file->cache.pos) {
                lfs_ssize_t d_ = lfs_min(
                        d,
                        file->cache.size - (pos_ - file->cache.pos));
                lfs_memcpy(buffer_,
                        &file->cache.buffer[pos_ - file->cache.pos],
                        d_);

                pos_ += d_;
                buffer_ += d_;
                size -= d_;
                d -= d_;
                continue;
            }

            // buffered data takes priority
            d = lfs_min(d, file->cache.pos - pos_);
        }

        // any data in our btree?
        if (pos_ < file->b.shrub.weight) {
            // bypass cache?
            if ((lfs_size_t)d >= lfsr_file_cachesize(lfs, file)) {
                lfs_ssize_t d_ = lfsr_file_readnext(lfs, file,
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

            // cache in use? we need to flush it
            //
            // note that flush does not change the actual file data, so if
            // a read fails it's ok to fall back to our flushed state
            //
            if (lfsr_o_isunflush(file->b.o.flags)) {
                int err = lfsr_file_flush(lfs, file);
                if (err) {
                    return err;
                }
                file->cache.pos = 0;
                file->cache.size = 0;
            }

            // try to fill our cache with some data
            lfs_ssize_t d_ = lfsr_file_readnext(lfs, file,
                    pos_, file->cache.buffer, d);
            if (d_ < 0) {
                LFS_ASSERT(d != LFS_ERR_NOENT);
                return d_;
            }
            file->cache.pos = pos_;
            file->cache.size = d_;
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

// low-level file writing

static int lfsr_file_commit(lfs_t *lfs, lfsr_file_t *file,
        lfsr_bid_t bid, const lfsr_rattr_t *rattrs, lfs_size_t rattr_count) {
    return lfsr_bshrub_commit(lfs, &file->b,
            bid, rattrs, rattr_count);
}

static int lfsr_file_carve(lfs_t *lfs, lfsr_file_t *file,
        lfs_off_t pos, lfs_off_t weight, lfsr_rattr_t rattr) {
    // note! we must never allow our btree size to overflow, even
    // temporarily

    // can't carve more than the carve weight
    LFS_ASSERT(rattr.weight >= -(lfs_soff_t)weight);

    // carving the entire tree? revert to no bshrub/btree
    if (pos == 0
            && weight >= file->b.shrub.weight
            && rattr.weight == -(lfs_soff_t)weight) {
        lfsr_bshrub_init(&file->b);
        return 0;
    }

    // try to merge commits where possible
    lfsr_bid_t bid = file->b.shrub.weight;
    lfsr_rattr_t rattrs[3];
    lfs_size_t rattr_count = 0;
    lfsr_bptr_t l;
    lfsr_bptr_t r;

    // need a hole?
    if (pos > file->b.shrub.weight) {
        // can we coalesce?
        if (file->b.shrub.weight > 0) {
            bid = lfs_min(bid, file->b.shrub.weight-1);
            rattrs[rattr_count++] = LFSR_RATTR(
                    LFSR_TAG_GROW, +(pos - file->b.shrub.weight));

        // new hole
        } else {
            bid = lfs_min(bid, file->b.shrub.weight);
            rattrs[rattr_count++] = LFSR_RATTR(
                    LFSR_TAG_DATA, +(pos - file->b.shrub.weight));
        }
    }

    // try to carve any existing data
    lfsr_rattr_t r_rattr_ = {.tag=0};
    while (pos < file->b.shrub.weight) {
        lfsr_bid_t weight_;
        lfsr_bptr_t bptr_;
        int err = lfsr_file_lookupnext(lfs, file, pos,
                &bid, &weight_, &bptr_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        #ifdef LFS_CKFETCHES
        // checking fetches?
        if (lfsr_m_isckfetches(lfs->flags)
                && lfsr_bptr_isbptr(&bptr_)) {
            err = lfsr_bptr_ck(lfs, &bptr_);
            if (err) {
                return err;
            }
        }
        #endif

        // note, an entry can be both a left and right sibling
        l = bptr_;
        l.data = LFSR_DATA_SLICE(bptr_.data,
                -1,
                pos - (bid-(weight_-1)));
        r = bptr_;
        r.data = LFSR_DATA_SLICE(bptr_.data,
                pos+weight - (bid-(weight_-1)),
                -1);

        // left sibling needs carving but falls underneath our
        // fragment threshold? break into fragments
        while (lfsr_bptr_isbptr(&bptr_)
                && lfsr_data_size(l.data) > lfs->cfg->fragment_size
                && lfsr_data_size(l.data) < lfs_min(
                    lfs->cfg->fragment_thresh,
                    lfs->cfg->crystal_thresh)) {
            bptr_.data = LFSR_DATA_SLICE(bptr_.data,
                    lfs->cfg->fragment_size,
                    -1);
            
            err = lfsr_file_commit(lfs, file, bid, LFSR_RATTRS(
                    LFSR_RATTR_DATA(
                        LFSR_TAG_GROW | LFSR_TAG_MASK8 | LFSR_TAG_DATA,
                            -(weight_ - lfs->cfg->fragment_size),
                        &LFSR_DATA_TRUNCATE(l.data,
                                lfs->cfg->fragment_size)),
                    LFSR_RATTR_BPTR(
                        LFSR_TAG_BLOCK,
                            +(weight_ - lfs->cfg->fragment_size),
                        &bptr_)));
            if (err) {
                return err;
            }

            weight_ -= lfs->cfg->fragment_size;
            l.data = LFSR_DATA_SLICE(bptr_.data,
                    -1,
                    pos - (bid-(weight_-1)));
        }

        // right sibling needs carving but falls underneath our
        // fragment threshold? break into fragments
        while (lfsr_bptr_isbptr(&bptr_)
                && lfsr_data_size(r.data) > lfs->cfg->fragment_size
                && lfsr_data_size(r.data) < lfs_min(
                    lfs->cfg->fragment_thresh,
                    lfs->cfg->crystal_thresh)) {
            bptr_.data = LFSR_DATA_SLICE(bptr_.data,
                    -1,
                    lfsr_data_size(bptr_.data) - lfs->cfg->fragment_size);

            err = lfsr_file_commit(lfs, file, bid, LFSR_RATTRS(
                    LFSR_RATTR_BPTR(
                        LFSR_TAG_GROW | LFSR_TAG_MASK8 | LFSR_TAG_BLOCK,
                            -(weight_ - lfsr_data_size(bptr_.data)),
                        &bptr_),
                    LFSR_RATTR_DATA(
                        LFSR_TAG_DATA,
                            +(weight_ - lfsr_data_size(bptr_.data)),
                        &LFSR_DATA_FRUNCATE(r.data,
                            lfs->cfg->fragment_size))));
            if (err) {
                return err;
            }

            bid -= (weight_-lfsr_data_size(bptr_.data));
            weight_ -= (weight_-lfsr_data_size(bptr_.data));
            r.data = LFSR_DATA_SLICE(bptr_.data,
                    pos+weight - (bid-(weight_-1)),
                    -1);
        }

        // found left sibling?
        if (bid-(weight_-1) < pos) {
            // can we get away with a grow attribute?
            if (lfsr_data_size(bptr_.data) == lfsr_data_size(l.data)) {
                rattrs[rattr_count++] = LFSR_RATTR(
                        LFSR_TAG_GROW, -(bid+1 - pos));

            // carve fragment?
            } else if (!lfsr_bptr_isbptr(&bptr_)
                    || lfsr_data_size(l.data) <= lfs->cfg->fragment_size) {
                rattrs[rattr_count++] = LFSR_RATTR_DATA(
                        LFSR_TAG_GROW | LFSR_TAG_MASK8 | LFSR_TAG_DATA,
                            -(bid+1 - pos),
                        &l.data);

            // carve bptr?
            //
            // make sure we're not creating a dag! we can't allow this
            // to happen or it would break our littlefs-is-a-tree
            // invariant
            } else if (!(pos+weight < bid+1
                    && lfsr_data_size(r.data) > lfs->cfg->fragment_size)) {
                rattrs[rattr_count++] = LFSR_RATTR_BPTR(
                        LFSR_TAG_GROW | LFSR_TAG_MASK8 | LFSR_TAG_BLOCK,
                            -(bid+1 - pos),
                        &l);

            // uh oh, keeping both siblings would create a dag? we have
            // no choice but to rewrite one into a new block
            //
            // our crystallization algorithm currently prevents this from
            // happening, but it may be a problem in the future (more
            // advanced hole APIs, alternative write strategies, etc)
            } else {
                // this is where we would split dags if an algorithm
                // needed it
                LFS_UNREACHABLE();
            }

        // completely overwriting this entry?
        } else {
            rattrs[rattr_count++] = LFSR_RATTR(
                    LFSR_TAG_RM, -weight_);
        }

        // spans more than one entry? we can't do everything in one
        // commit because it might span more than one btree leaf, so
        // commit what we have and move on to next entry
        if (pos+weight > bid+1) {
            LFS_ASSERT(lfsr_data_size(r.data) == 0);
            LFS_ASSERT(rattr_count <= sizeof(rattrs)/sizeof(lfsr_rattr_t));

            err = lfsr_file_commit(lfs, file, bid,
                    rattrs, rattr_count);
            if (err) {
                return err;
            }

            rattr.weight += lfs_min(weight, bid+1 - pos);
            weight -= lfs_min(weight, bid+1 - pos);
            rattr_count = 0;
            continue;
        }

        // found right sibling?
        if (pos+weight < bid+1) {
            // can we coalesce a hole?
            if (lfsr_data_size(r.data) == 0) {
                rattr.weight += bid+1 - (pos+weight);

            // carve fragment?
            } else if (!lfsr_bptr_isbptr(&bptr_)
                    || lfsr_data_size(r.data) <= lfs->cfg->fragment_size) {
                r_rattr_ = LFSR_RATTR_DATA(
                        LFSR_TAG_DATA, bid+1 - (pos+weight),
                        &r.data);

            // carve bptr?
            } else {
                r_rattr_ = LFSR_RATTR_BPTR(
                        LFSR_TAG_BLOCK, bid+1 - (pos+weight),
                        &r);
            }
        }

        rattr.weight += lfs_min(weight, bid+1 - pos);
        weight -= lfs_min(weight, bid+1 - pos);
        break;
    }

    // append our data
    if (weight + rattr.weight > 0) {
        // can we coalesce a hole?
        if (lfsr_rattr_dsize(rattr) == 0 && pos > 0) {
            bid = lfs_min(bid, file->b.shrub.weight-1);
            rattrs[rattr_count++] = LFSR_RATTR(
                    LFSR_TAG_GROW, +(weight + rattr.weight));

        // need a new hole?
        } else if (lfsr_rattr_dsize(rattr) == 0) {
            bid = lfs_min(bid, file->b.shrub.weight);
            rattrs[rattr_count++] = LFSR_RATTR(
                    LFSR_TAG_DATA, +(weight + rattr.weight));

        // append new fragment/bptr?
        } else {
            bid = lfs_min(bid, file->b.shrub.weight);
            rattrs[rattr_count++] = LFSR_RATTR_(
                    rattr.tag, +(weight + rattr.weight),
                    rattr.u, rattr.count);
        }
    }

    // and don't forget the right sibling
    if (r_rattr_.tag) {
        rattrs[rattr_count++] = r_rattr_;
    }

    // commit pending rattrs
    if (rattr_count > 0) {
        LFS_ASSERT(rattr_count <= sizeof(rattrs)/sizeof(lfsr_rattr_t));

        int err = lfsr_file_commit(lfs, file, bid,
                rattrs, rattr_count);
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
    lfs_off_t crystal_start;
    lfs_off_t crystal_end;
    while (size > 0) {
        // first we need to figure out our current crystal, we do this
        // heuristically.
        //
        // note that we may end up including holes in our crystal, but this
        // is fine. we don't want small holes breaking up blocks anyways

        // default to arbitrary alignment
        crystal_start = pos;
        crystal_end = pos + size;
        lfs_off_t block_start;
        lfs_off_t block_end;
        lfs_sblock_t block;
        lfs_size_t off;
        lfs_size_t eoff;
        uint32_t cksum;

        // within our tree? find left crystal neighbor
        if (pos > 0
                && lfs->cfg->crystal_thresh > 0
                && (lfs_soff_t)(pos - (lfs->cfg->crystal_thresh-1))
                    < (lfs_soff_t)file->b.shrub.weight
                && file->b.shrub.weight > 0
                // don't bother to lookup left after the first block
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_file_lookupnext(lfs, file,
                    lfs_smax(pos - (lfs->cfg->crystal_thresh-1), 0),
                    &bid, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // if left crystal neighbor is a fragment and there is no
            // obvious hole between our own crystal and our neighbor,
            // include as a part of our crystal
            if (!lfsr_bptr_isbptr(&bptr)
                    // hole? holes can be quite large and shouldn't trigger
                    // crystallization
                    && (lfs_soff_t)(bid-(weight-1)+lfsr_data_size(bptr.data))
                        >= (lfs_soff_t)(pos - (lfs->cfg->crystal_thresh-1))) {
                crystal_start = bid-(weight-1);

            // otherwise our neighbor determines our crystal boundary
            } else {
                crystal_start = lfs_min(bid+1, pos);

                // wait, found erased-state?
                if (lfsr_bptr_isbptr(&bptr)
                        && bptr.data.u.disk.block == file->eblock
                        && bptr.data.u.disk.off + lfsr_data_size(bptr.data)
                            == file->eoff
                        // not clobbering data?
                        && pos - (bid-(weight-1))
                            >= lfsr_data_size(bptr.data)
                        // enough for prog alignment?
                        && crystal_end - crystal_start
                            >= lfs->cfg->prog_size) {
                    // mark as unerased in case of failure
                    file->eblock = 0;
                    file->eoff = -1;

                    // try to use erased-state
                    block_start = bid-(weight-1);
                    block_end = block_start + lfsr_data_size(bptr.data);
                    block = bptr.data.u.disk.block;
                    off = bptr.data.u.disk.off;
                    eoff = lfsr_bptr_cksize(&bptr);
                    cksum = lfsr_bptr_cksum(&bptr);
                    goto compact;
                }
            }
        }

        // if we haven't already exceeded our crystallization threshold,
        // find right crystal neighbor
        if (crystal_end - crystal_start < lfs->cfg->crystal_thresh
                && file->b.shrub.weight > 0) {
            lfsr_bid_t bid;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_file_lookupnext(lfs, file,
                    lfs_min(
                        crystal_start + (lfs->cfg->crystal_thresh-1),
                        file->b.shrub.weight-1),
                    &bid, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // if right crystal neighbor is a fragment, include as a part
            // of our crystal
            if (!lfsr_bptr_isbptr(&bptr)) {
                crystal_end = lfs_max(
                        bid-(weight-1)+lfsr_data_size(bptr.data),
                        crystal_end);

            // otherwise treat as crystal boundary
            } else {
                crystal_end = lfs_max(
                        bid-(weight-1),
                        crystal_end);
            }
        }

        // below our crystallization threshold? fallback to writing fragments
        if (crystal_end - crystal_start < lfs->cfg->crystal_thresh
                // enough for prog alignment?
                || crystal_end - crystal_start < lfs->cfg->prog_size) {
            goto fragment;
        }

        // exceeded our crystallization threshold? crystallize into a
        // new block

        // before we can crystallize we need to figure out the best
        // block alignment, we use the entry immediately to the left of
        // our crystal for this
        if (crystal_start > 0
                && file->b.shrub.weight > 0
                // don't bother to lookup left after the first block
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_file_lookupnext(lfs, file,
                    lfs_min(
                        crystal_start-1,
                        file->b.shrub.weight-1),
                    &bid, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // is our left neighbor in the same block?
            if (crystal_start - (bid-(weight-1))
                        < lfs->cfg->block_size
                    && lfsr_data_size(bptr.data) > 0) {
                crystal_start = bid-(weight-1);

                // wait, found erased-state?
                if (lfsr_bptr_isbptr(&bptr)
                        && bptr.data.u.disk.block == file->eblock
                        && bptr.data.u.disk.off + lfsr_data_size(bptr.data)
                            == file->eoff
                        // not clobbering data?
                        && pos - (bid-(weight-1))
                            >= lfsr_data_size(bptr.data)) {
                    // mark as unerased in case of failure
                    file->eblock = 0;
                    file->eoff = -1;

                    // try to use erased-state
                    block_start = bid-(weight-1);
                    block_end = block_start + lfsr_data_size(bptr.data);
                    block = bptr.data.u.disk.block;
                    off = bptr.data.u.disk.off;
                    eoff = lfsr_bptr_cksize(&bptr);
                    cksum = lfsr_bptr_cksum(&bptr);
                    goto compact;
                }

            // no? is our left neighbor at least our left block neighbor?
            // align to block alignment
            } else if (crystal_start - (bid-(weight-1))
                        < 2*lfs->cfg->block_size
                    && lfsr_data_size(bptr.data) > 0) {
                crystal_start = bid-(weight-1) + lfs->cfg->block_size;
            }
        }

    crystallize:;
        block_start = crystal_start;

    relocate:;
        // allocate a new block
        //
        // note if we relocate, we rewrite the entire block from block_start
        // using what we can find in our tree
        block = lfs_alloc(lfs, true);
        if (block < 0) {
            return block;
        }

        block_end = block_start;
        off = 0;
        eoff = 0;
        cksum = 0;

    compact:;
        // crystallize data into our block
        //
        // eagerly merge any right neighbors we see unless that would
        // put us over our block size
        while (block_end < lfs_min(
                block_start
                    + (lfs->cfg->block_size - off),
                lfs_max(
                    pos + size,
                    file->b.shrub.weight))) {
            // keep track of the next highest priority data offset
            lfs_ssize_t d = lfs_min(
                    block_start
                        + (lfs->cfg->block_size - off),
                    lfs_max(
                        pos + size,
                        file->b.shrub.weight)) - block_end;

            // any data in our buffer?
            if (block_end < pos + size && size > 0) {
                if (block_end >= pos) {
                    lfs_ssize_t d_ = lfs_min(
                            d,
                            size - (block_end - pos));
                    int err = lfsr_bd_prog(lfs, block,
                            eoff,
                            &buffer[block_end - pos], d_,
                            &cksum, true);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_RANGE);
                        // bad prog? try another block
                        if (err == LFS_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }

                    block_end += d_;
                    eoff += d_;
                    d -= d_;
                }

                // buffered data takes priority
                d = lfs_min(d, pos - block_end);
            }

            // any data on disk?
            if (block_end < file->b.shrub.weight) {
                lfsr_bid_t bid_;
                lfsr_bid_t weight_;
                lfsr_bptr_t bptr_;
                int err = lfsr_file_lookupnext(lfs, file, block_end,
                        &bid_, &weight_, &bptr_);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                #ifdef LFS_CKFETCHES
                // checking fetches?
                if (lfsr_m_isckfetches(lfs->flags)
                        && lfsr_bptr_isbptr(&bptr_)) {
                    err = lfsr_bptr_ck(lfs, &bptr_);
                    if (err) {
                        return err;
                    }
                }
                #endif

                // make sure to include all of our crystal, or else this
                // loop may never terminate
                if (bid_-(weight_-1) >= crystal_end
                        // is this data a pure hole? stop early to better
                        // leverage erased-state in sparse files
                        && (block_end >= bid_-(weight_-1)
                                + lfsr_data_size(bptr_.data)
                            // does this data exceed our block_size?
                            // stop early to try to avoid messing up
                            // block alignment
                            || bid_-(weight_-1) + lfsr_data_size(bptr_.data)
                                    - block_start
                                > lfs->cfg->block_size)) {
                    break;
                }

                if (block_end
                        < bid_-(weight_-1) + lfsr_data_size(bptr_.data)) {
                    // note one important side-effect here is a strict
                    // data hint
                    lfs_ssize_t d_ = lfs_min(
                            d,
                            lfsr_data_size(bptr_.data)
                                - (block_end - (bid_-(weight_-1))));
                    err = lfsr_bd_progdata(lfs, block,
                            eoff,
                            LFSR_DATA_SLICE(bptr_.data,
                                block_end - (bid_-(weight_-1)),
                                d_),
                            &cksum, true);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_RANGE);
                        // bad prog? try another block
                        if (err == LFS_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }

                    block_end += d_;
                    eoff += d_;
                    d -= d_;
                }

                // found a hole? just make sure next leaf takes priority
                d = lfs_min(d, bid_+1 - block_end);
            }

            // found a hole? fill with zeros
            int err = lfsr_bd_set(lfs, block,
                    eoff,
                    0, d,
                    &cksum, true);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            block_end += d;
            eoff += d;
        }

        // a bit of a hack here, we need to truncate our block to
        // prog_size alignment to avoid padding issues
        //
        // doing this retroactively to the pcache greatly simplifies the
        // above loop, though we may end up reading more than is
        // strictly necessary
        lfs_ssize_t d = eoff % lfs->cfg->prog_size;
        lfs->pcache.size -= d;
        block_end -= d;
        eoff -= d;

        // finalize our write
        int err = lfsr_bd_flush(lfs,
                &cksum, true);
        if (err) {
            // bad prog? try another block
            if (err == LFS_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // prepare our block pointer
        LFS_ASSERT(eoff > 0);
        LFS_ASSERT(eoff <= lfs->cfg->block_size);
        lfsr_bptr_t bptr;
        lfsr_bptr_init(&bptr,
                LFSR_DATA_DISK(
                    block,
                    off,
                    eoff - off),
                eoff, cksum);

        // and write it into our tree
        err = lfsr_file_carve(lfs, file,
                block_start, block_end - block_start,
                LFSR_RATTR_BPTR(
                    LFSR_TAG_BLOCK, 0,
                    &bptr));
        if (err) {
            return err;
        }

        // keep track of any remaining erased-state
        if (eoff < lfs->cfg->block_size) {
            file->eblock = block;
            file->eoff = eoff;
        }

        // note crystallizing fragments -> blocks may not actually make
        // any progress on flushing the buffer on the first pass
        d = lfs_max(pos, block_end) - pos;
        pos += d;
        buffer += lfs_min(d, size);
        size -= lfs_min(d, size);
        aligned = true;
    }

fragment:;
    // iteratively write fragments (inlined leaves)
    while (size > 0) {
        // truncate to our fragment size
        lfs_off_t fragment_start = pos;
        lfs_off_t fragment_end = fragment_start + lfs_min(
                size,
                lfs->cfg->fragment_size);

        lfsr_data_t datas[3];
        lfs_size_t data_count = 0;

        // do we have a left sibling?
        if (fragment_start > 0
                && file->b.shrub.weight >= fragment_start
                // don't bother to lookup left after first fragment
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_file_lookupnext(lfs, file,
                    fragment_start-1,
                    &bid, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            #ifdef LFS_CKFETCHES
            // checking fetches?
            if (lfsr_m_isckfetches(lfs->flags)
                    && lfsr_bptr_isbptr(&bptr)) {
                err = lfsr_bptr_ck(lfs, &bptr);
                if (err) {
                    return err;
                }
            }
            #endif

            // can we coalesce?
            if (bid-(weight-1) + lfsr_data_size(bptr.data) >= fragment_start
                    && fragment_end - (bid-(weight-1))
                        <= lfs->cfg->fragment_size) {
                datas[data_count++] = LFSR_DATA_TRUNCATE(bptr.data,
                        fragment_start - (bid-(weight-1)));

                fragment_start = bid-(weight-1);
                fragment_end = fragment_start + lfs_min(
                        fragment_end - (bid-(weight-1)),
                        lfs->cfg->fragment_size);

            // uh oh, would we end up creating a dag?
            //
            // we would be forced to split the dag in lfsr_file_carve in
            // order to keep the littlefs-is-a-tree invariant, so we might
            // as well try to recrystallize the left sibling
            } else if (lfsr_bptr_isbptr(&bptr)
                        && fragment_end
                            < bid-(weight-1) + lfsr_data_size(bptr.data)) {
                crystal_start = bid-(weight-1);
                crystal_end = fragment_end;
                goto crystallize;
            }
        }

        // append our new data
        datas[data_count++] = LFSR_DATA_BUF(
                buffer,
                fragment_end - pos);

        // do we have a right sibling?
        //
        // note this may the same as our left sibling 
        if (fragment_end < file->b.shrub.weight
                // don't bother to lookup right if fragment is already full
                && fragment_end - fragment_start < lfs->cfg->fragment_size) {
            lfsr_bid_t bid;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_file_lookupnext(lfs, file,
                    fragment_end,
                    &bid, &weight, &bptr);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            #ifdef LFS_CKFETCHES
            // checking fetches?
            if (lfsr_m_isckfetches(lfs->flags)
                    && lfsr_bptr_isbptr(&bptr)) {
                err = lfsr_bptr_ck(lfs, &bptr);
                if (err) {
                    return err;
                }
            }
            #endif

            // can we coalesce?
            if (fragment_end < bid-(weight-1) + lfsr_data_size(bptr.data)
                    && bid-(weight-1) + lfsr_data_size(bptr.data)
                            - fragment_start
                        <= lfs->cfg->fragment_size) {
                datas[data_count++] = LFSR_DATA_FRUNCATE(bptr.data,
                        bid-(weight-1) + lfsr_data_size(bptr.data)
                            - fragment_end);

                fragment_end = fragment_start + lfs_min(
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
                LFSR_RATTR_CAT_(
                    LFSR_TAG_DATA, 0,
                    datas, data_count));
        if (err && err != LFS_ERR_RANGE) {
            return err;
        }

        // to next fragment
        lfs_ssize_t d = fragment_end - pos;
        pos += d;
        buffer += lfs_min(d, size);
        size -= lfs_min(d, size);
        aligned = true;
    }

    return 0;
}

// high-level file writing

lfs_ssize_t lfsr_file_write(lfs_t *lfs, lfsr_file_t *file,
        const void *buffer, lfs_size_t size) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't write to readonly files
    LFS_ASSERT(!lfsr_o_isrdonly(file->b.o.flags));

    // size=0 is a bit special and is guaranteed to have no effects on the
    // underlying file, this means no updating file pos or file size
    //
    // since we need to test for this, just return early
    if (size == 0) {
        return 0;
    }

    // would this write make our file larger than our file limit?
    int err;
    if (size > lfs->file_limit - file->pos) {
        err = LFS_ERR_FBIG;
        goto failed;
    }

    // clobber entangled traversals
    lfsr_omdir_mkdirty(lfs, &file->b.o);
    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);
    // mark as unsynced in case we fail
    file->b.o.flags |= LFS_o_UNSYNC;

    // update pos if we are appending
    lfs_off_t pos = file->pos;
    if (lfsr_o_isappend(file->b.o.flags)) {
        pos = lfsr_file_size_(file);
    }

    const uint8_t *buffer_ = buffer;
    lfs_size_t written = 0;
    while (size > 0) {
        // bypass cache?
        //
        // note we flush our cache before bypassing writes, this isn't
        // strictly necessary, but enforces a more intuitive write order
        // and avoids weird cases with low-level write heuristics
        //
        if ((!lfsr_o_isunflush(file->b.o.flags)
                    || file->cache.size == 0)
                && size >= lfsr_file_cachesize(lfs, file)) {
            err = lfsr_file_flush_(lfs, file,
                    pos, buffer_, size);
            if (err) {
                goto failed;
            }

            // after success, fill our cache with the tail of our write
            //
            // note we need to clear the cache anyways to avoid any
            // out-of-date data
            file->cache.pos = pos + size - lfsr_file_cachesize(lfs, file);
            lfs_memcpy(file->cache.buffer,
                    &buffer_[size - lfsr_file_cachesize(lfs, file)],
                    lfsr_file_cachesize(lfs, file));
            file->cache.size = lfsr_file_cachesize(lfs, file);

            file->b.o.flags &= ~LFS_o_UNFLUSH;
            written += size;
            pos += size;
            buffer_ += size;
            size -= size;
            continue;
        }

        // try to fill our cache
        //
        // This is a bit delicate, since our cache contains both old and
        // new data, but note:
        //
        // 1. We only write to yet unused cache memory.
        //
        // 2. Bypassing the cache above means we only write to the
        //    cache once, and flush at most twice.
        //
        if ((!lfsr_o_isunflush(file->b.o.flags)
                    || file->cache.size == 0)
                || (pos >= file->cache.pos
                    && pos <= file->cache.pos + file->cache.size
                    && pos
                        < file->cache.pos
                            + lfsr_file_cachesize(lfs, file))) {
            // unused cache? we can move it where we need it
            if ((!lfsr_o_isunflush(file->b.o.flags)
                    || file->cache.size == 0)) {
                file->cache.pos = pos;
                file->cache.size = 0;
            }

            lfs_size_t d = lfs_min(
                    size,
                    lfsr_file_cachesize(lfs, file)
                        - (pos - file->cache.pos));
            lfs_memcpy(&file->cache.buffer[pos - file->cache.pos],
                    buffer_,
                    d);
            file->cache.size = lfs_max(
                    file->cache.size,
                    pos+d - file->cache.pos);

            file->b.o.flags |= LFS_o_UNFLUSH;
            written += d;
            pos += d;
            buffer_ += d;
            size -= d;
            continue;
        }

        // flush our cache so the above can't fail
        err = lfsr_file_flush_(lfs, file,
                file->cache.pos, file->cache.buffer, file->cache.size);
        if (err) {
            goto failed;
        }
        file->b.o.flags &= ~LFS_o_UNFLUSH;
    }

    // update our pos
    file->pos = pos;

    // flush if requested
    if (lfsr_o_isflush(file->b.o.flags)) {
        err = lfsr_file_flush(lfs, file);
        if (err) {
            goto failed;
        }
    }

    // sync if requested
    if (lfsr_o_issync(file->b.o.flags)) {
        err = lfsr_file_sync(lfs, file);
        if (err) {
            goto failed;
        }
    }

    return written;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->b.o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_flush(lfs_t *lfs, lfsr_file_t *file) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't write to readonly files
    LFS_ASSERT(!lfsr_o_isrdonly(file->b.o.flags));

    // do nothing if our file is already flushed
    if (!lfsr_o_isunflush(file->b.o.flags)) {
        return 0;
    }

    // clobber entangled traversals
    lfsr_omdir_mkdirty(lfs, &file->b.o);
    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);

    // flush our cache
    int err = lfsr_file_flush_(lfs, file,
            file->cache.pos, file->cache.buffer, file->cache.size);
    if (err) {
        goto failed;
    }

    // mark as flushed
    file->b.o.flags &= ~LFS_o_UNFLUSH;
    return 0;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->b.o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_sync(lfs_t *lfs, lfsr_file_t *file) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't write to readonly files, if you want to resync call
    // lfsr_file_resync
    LFS_ASSERT(!lfsr_o_isrdonly(file->b.o.flags));

    // removed? we can't sync
    int err;
    if (lfsr_o_iszombie(file->b.o.flags)) {
        err = LFS_ERR_NOENT;
        goto failed;
    }

    // first flush any data in our cache, this is a noop if already
    // flushed
    //
    // note that flush does not change the actual file data, so if
    // flush succeeds but mdir commit fails it's ok to fall back to
    // our flushed state
    //
    err = lfsr_file_flush(lfs, file);
    if (err) {
        goto failed;
    }
    // build a commit of any pending file metadata
    lfsr_rattr_t rattrs[3];
    lfs_size_t rattr_count = 0;
    lfsr_data_t name_data;

    // not created yet? need to convert to normal file
    if (lfsr_o_isuncreat(file->b.o.flags)) {
        // uncreated files must be unsynced
        LFS_ASSERT(lfsr_o_isunsync(file->b.o.flags));

        err = lfsr_mdir_lookup(lfs, &file->b.o.mdir, LFSR_TAG_STICKYNOTE,
                NULL, &name_data);
        if (err) {
            // orphan flag but no stickynote tag?
            LFS_ASSERT(err != LFS_ERR_NOENT);
            goto failed;
        }

        rattrs[rattr_count++] = LFSR_RATTR_DATA(
                LFSR_TAG_MASK8 | LFSR_TAG_REG, 0,
                &name_data);
    }

    // pending file changes?
    if (lfsr_o_isunsync(file->b.o.flags)) {
        // make sure data is on-disk before committing metadata
        err = lfsr_bd_sync(lfs);
        if (err) {
            goto failed;
        }

        // zero size files should have no bshrub/btree
        LFS_ASSERT(file->b.shrub.weight > 0
                || lfsr_bshrub_isbnull(&file->b));

        // no bshrub/btree?
        if (lfsr_bshrub_isbnull(&file->b)) {
            rattrs[rattr_count++] = LFSR_RATTR(
                    LFSR_TAG_RM | LFSR_TAG_MASK8 | LFSR_TAG_STRUCT, 0);
        // bshrub?
        } else if (lfsr_bshrub_isbshrub(&file->b)) {
            rattrs[rattr_count++] = LFSR_RATTR_SHRUB(
                    LFSR_TAG_MASK8 | LFSR_TAG_BSHRUB, 0,
                    // note we use the staged trunk here
                    &file->b.shrub_);
        // btree?
        } else if (lfsr_bshrub_isbtree(&file->b)) {
            rattrs[rattr_count++] = LFSR_RATTR_BTREE(
                    LFSR_TAG_MASK8 | LFSR_TAG_BTREE, 0,
                    &file->b.shrub);
        } else {
            LFS_UNREACHABLE();
        }
    }

    // pending custom attributes?
    //
    // this gets real messy, since users can change custom attributes
    // whenever they want without informing littlefs, the best we can do
    // is read from disk to manually check if any attributes changed
    bool attrs = lfsr_o_isunsync(file->b.o.flags);
    if (!attrs) {
        for (lfs_size_t i = 0; i < file->cfg->attr_count; i++) {
            // skip readonly attrs and lazy attrs
            if (lfsr_o_isrdonly(file->cfg->attrs[i].flags)
                    || lfsr_a_islazy(file->cfg->attrs[i].flags)) {
                continue;
            }

            // lookup the attr
            lfsr_data_t data;
            err = lfsr_mdir_lookup(lfs, &file->b.o.mdir,
                    LFSR_TAG_ATTR(file->cfg->attrs[i].type),
                    NULL, &data);
            if (err && err != LFS_ERR_NOENT) {
                goto failed;
            }

            // does disk match our attr?
            lfs_scmp_t cmp = lfsr_attr_cmp(lfs, &file->cfg->attrs[i],
                    (err != LFS_ERR_NOENT) ? &data : NULL);
            if (cmp < 0) {
                err = cmp;
                goto failed;
            }

            if (cmp != LFS_CMP_EQ) {
                attrs = true;
                break;
            }
        }
    }
    if (attrs) {
        // need to append custom attributes
        rattrs[rattr_count++] = LFSR_RATTR_ATTRS(
                file->cfg->attrs, file->cfg->attr_count);
    }

    // pending metadata? looks like we need to write to disk
    if (rattr_count > 0) {
        // checkpoint the allocator
        lfs_alloc_ckpoint(lfs);

        // commit!
        LFS_ASSERT(rattr_count <= sizeof(rattrs)/sizeof(lfsr_rattr_t));
        err = lfsr_mdir_commit(lfs, &file->b.o.mdir,
                rattrs, rattr_count);
        if (err) {
            goto failed;
        }
    }

    // update in-device state
    for (lfsr_omdir_t *o = lfs->omdirs; o; o = o->next) {
        if (lfsr_o_type(o->flags) == LFS_TYPE_REG
                && o->mdir.mid == file->b.o.mdir.mid
                // don't double update
                && o != &file->b.o) {
            lfsr_file_t *file_ = (lfsr_file_t*)o;
            // notify all files of creation
            file_->b.o.flags &= ~LFS_o_UNCREAT;

            // mark desynced files an unsynced
            if (lfsr_o_isdesync(file_->b.o.flags)) {
                file_->b.o.flags |= LFS_o_UNSYNC;

            // update synced files
            } else {
                file_->b.o.flags &= ~(LFS_o_UNSYNC | LFS_o_UNFLUSH);
                file_->b.shrub = file->b.shrub;
                file_->cache.pos = file->cache.pos;
                LFS_ASSERT(file->cache.size
                        <= lfsr_file_cachesize(lfs, file));
                lfs_memcpy(file_->cache.buffer,
                        file->cache.buffer,
                        file->cache.size);
                file_->cache.size = file->cache.size;

                // update any custom attrs
                for (lfs_size_t i = 0; i < file->cfg->attr_count; i++) {
                    if (lfsr_o_isrdonly(file->cfg->attrs[i].flags)) {
                        continue;
                    }

                    for (lfs_size_t j = 0; j < file_->cfg->attr_count; j++) {
                        if (!(file_->cfg->attrs[j].type
                                    == file->cfg->attrs[i].type
                                && !lfsr_o_iswronly(
                                    file_->cfg->attrs[j].flags))) {
                            continue;
                        }

                        if (lfsr_attr_isnoattr(&file->cfg->attrs[i])) {
                            if (file_->cfg->attrs[j].size) {
                                *file_->cfg->attrs[j].size = LFS_ERR_NOATTR;
                            }
                        } else {
                            lfs_size_t d = lfs_min(
                                    lfsr_attr_size(&file->cfg->attrs[i]),
                                    file_->cfg->attrs[j].buffer_size);
                            memcpy(file_->cfg->attrs[j].buffer,
                                    file->cfg->attrs[i].buffer,
                                    d);
                            if (file_->cfg->attrs[j].size) {
                                *file_->cfg->attrs[j].size = d;
                            }
                        }
                    }
                }
            }

        // clobber entangled traversals
        } else if (lfsr_o_type(o->flags) == LFS_TYPE_TRAVERSAL
                && o->mdir.mid == file->b.o.mdir.mid) {
            lfsr_traversal_clobber(lfs, (lfsr_traversal_t*)o);
        }
    }

    // mark as synced
    file->b.o.flags &= ~(LFS_o_UNSYNC | LFS_o_UNCREAT | LFS_O_DESYNC);
    return 0;

failed:;
    file->b.o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_desync(lfs_t *lfs, lfsr_file_t *file) {
    (void)lfs;
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));

    // mark as desynced
    file->b.o.flags |= LFS_O_DESYNC;
    return 0;
}

int lfsr_file_resync(lfs_t *lfs, lfsr_file_t *file) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));

    // removed? we can't resync
    int err;
    if (lfsr_o_iszombie(file->b.o.flags)) {
        err = LFS_ERR_NOENT;
        goto failed;
    }

    // do nothing if already in-sync
    if (lfsr_o_isunsync(file->b.o.flags)) {
        // refetch the file struct from disk
        err = lfsr_file_fetch(lfs, file, false);
        if (err) {
            goto failed;
        }
    }

    // mark as resynced
    file->b.o.flags &= ~LFS_O_DESYNC;
    return 0;

failed:;
    file->b.o.flags |= LFS_O_DESYNC;
    return err;
}

// other file operations

lfs_soff_t lfsr_file_seek(lfs_t *lfs, lfsr_file_t *file,
        lfs_soff_t off, uint8_t whence) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));

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
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));

    return file->pos;
}

lfs_soff_t lfsr_file_rewind(lfs_t *lfs, lfsr_file_t *file) {
    (void)lfs;
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));

    file->pos = 0;
    return 0;
}

lfs_soff_t lfsr_file_size(lfs_t *lfs, lfsr_file_t *file) {
    (void)lfs;
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));

    return lfsr_file_size_(file);
}

int lfsr_file_truncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size_) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't write to readonly files
    LFS_ASSERT(!lfsr_o_isrdonly(file->b.o.flags));

    // do nothing if our size does not change
    lfs_off_t size = lfsr_file_size_(file);
    if (lfsr_file_size_(file) == size_) {
        return 0;
    }

    // exceeds our file limit?
    int err;
    if (size_ > lfs->file_limit) {
        err = LFS_ERR_FBIG;
        goto failed;
    }

    // clobber entangled traversals
    lfsr_omdir_mkdirty(lfs, &file->b.o);
    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);
    // mark as unsynced in case we fail
    file->b.o.flags |= LFS_o_UNSYNC;

    // truncate our btree
    err = lfsr_file_carve(lfs, file,
            lfs_min(size, size_), size - lfs_min(size, size_),
            LFSR_RATTR(
                LFSR_TAG_DATA, +size_ - size));
    if (err) {
        goto failed;
    }

    // truncate our cache
    file->cache.pos = lfs_min(file->cache.pos, size_);
    file->cache.size = lfs_min(
            file->cache.size,
            size_ - lfs_min(file->cache.pos, size_));

    // sync if requested
    if (lfsr_o_issync(file->b.o.flags)) {
        err = lfsr_file_sync(lfs, file);
        if (err) {
            goto failed;
        }
    }

    return 0;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->b.o.flags |= LFS_O_DESYNC;
    return err;
}

int lfsr_file_fruncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size_) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't write to readonly files
    LFS_ASSERT(!lfsr_o_isrdonly(file->b.o.flags));

    // do nothing if our size does not change
    lfs_off_t size = lfsr_file_size_(file);
    if (size == size_) {
        return 0;
    }

    // exceeds our file limit?
    int err;
    if (size_ > lfs->file_limit) {
        err = LFS_ERR_FBIG;
        goto failed;
    }

    // clobber entangled traversals
    lfsr_omdir_mkdirty(lfs, &file->b.o);
    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);
    // mark as unsynced in case we fail
    file->b.o.flags |= LFS_o_UNSYNC;

    // fruncate our btree
    err = lfsr_file_carve(lfs, file,
            0, lfs_smax(size - size_, 0),
            LFSR_RATTR(
                LFSR_TAG_DATA, +size_ - size));
    if (err) {
        goto failed;
    }

    // fruncate our cache
    lfs_memmove(file->cache.buffer,
            &file->cache.buffer[lfs_min(
                lfs_smax(
                    size - size_ - file->cache.pos,
                    0),
                file->cache.size)],
            file->cache.size - lfs_min(
                lfs_smax(
                    size - size_ - file->cache.pos,
                    0),
                file->cache.size));
    file->cache.size -= lfs_min(
            lfs_smax(
                size - size_ - file->cache.pos,
                0),
            file->cache.size);
    file->cache.pos -= lfs_smin(
            size - size_,
            file->cache.pos);

    // sync if requested
    if (lfsr_o_issync(file->b.o.flags)) {
        err = lfsr_file_sync(lfs, file);
        if (err) {
            goto failed;
        }
    }

    return 0;

failed:;
    // mark as desync so lfsr_file_close doesn't write to disk
    file->b.o.flags |= LFS_O_DESYNC;
    return err;
}

// file check functions

static int lfsr_file_traverse_(lfs_t *lfs, const lfsr_bshrub_t *bshrub,
        lfsr_btraversal_t *bt,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bptr_t *bptr_) {
    lfsr_tag_t tag;
    lfsr_data_t data;
    int err = lfsr_bshrub_traverse(lfs, bshrub, bt,
            bid_, &tag, &data);
    if (err) {
        return err;
    }

    // decode bptrs
    if (tag_) {
        *tag_ = tag;
    }
    if (bptr_) {
        if (tag == LFSR_TAG_BLOCK) {
            err = lfsr_data_readbptr(lfs, &data,
                    bptr_);
            if (err) {
                return err;
            }
        } else {
            bptr_->data = data;
        }
    }
    return 0;
}

static int lfsr_file_traverse(lfs_t *lfs, const lfsr_file_t *file,
        lfsr_btraversal_t *bt,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bptr_t *bptr_) {
    return lfsr_file_traverse_(lfs, &file->b, bt,
            bid_, tag_, bptr_);
}

static int lfsr_file_ck(lfs_t *lfs, const lfsr_file_t *file,
        uint32_t flags) {
    // traverse the file's bshrub/btree
    lfsr_btraversal_t bt;
    lfsr_btraversal_init(&bt);
    while (true) {
        lfsr_tag_t tag;
        lfsr_bptr_t bptr;
        int err = lfsr_file_traverse(lfs, file, &bt,
                NULL, &tag, &bptr);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }

        // validate btree nodes?
        //
        // this may end up revalidating some btree nodes when ckfetches
        // is enabled, but we need to revalidate cached btree nodes or
        // we risk missing errors in ckmeta scans
        if ((lfsr_t_isckmeta(flags)
                    || lfsr_t_isckdata(flags))
                && tag == LFSR_TAG_BRANCH) {
            lfsr_rbyd_t *rbyd = (lfsr_rbyd_t*)bptr.data.u.buffer;
            err = lfsr_rbyd_fetchck(lfs, rbyd,
                    rbyd->blocks[0], rbyd->trunk,
                    rbyd->cksum);
            if (err) {
                return err;
            }
        }

        // validate data blocks?
        if (lfsr_t_isckdata(flags)
                && tag == LFSR_TAG_BLOCK) {
            err = lfsr_bptr_ck(lfs, &bptr);
            if (err) {
                return err;
            }
        }
    }

    return 0;
}

int lfsr_file_ckmeta(lfs_t *lfs, lfsr_file_t *file) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't read from writeonly files
    LFS_ASSERT(!lfsr_o_iswronly(file->b.o.flags));

    return lfsr_file_ck(lfs, file, LFS_T_CKMETA);
}

int lfsr_file_ckdata(lfs_t *lfs, lfsr_file_t *file) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &file->b.o));
    // can't read from writeonly files
    LFS_ASSERT(!lfsr_o_iswronly(file->b.o.flags));

    return lfsr_file_ck(lfs, file, LFS_T_CKMETA | LFS_T_CKDATA);
}





/// High-level filesystem operations ///

// needed in lfs_init
static int lfs_deinit(lfs_t *lfs);

// initialize littlefs state, assert on bad configuration
static int lfs_init(lfs_t *lfs, uint32_t flags,
        const struct lfs_config *cfg) {
    // unknown flags?
    LFS_ASSERT((flags & ~(
            LFS_M_RDWR
                | LFS_M_RDONLY
                | LFS_M_FLUSH
                | LFS_M_SYNC
                | LFS_IFDEF_NOISY(LFS_M_NOISY, 0)
                | LFS_IFDEF_CKPROGS(LFS_M_CKPROGS, 0)
                | LFS_IFDEF_CKFETCHES(LFS_M_CKFETCHES, 0)
                | LFS_IFDEF_CKPARITY(LFS_M_CKPARITY, 0)
                | LFS_IFDEF_CKDATACKSUMS(LFS_M_CKDATACKSUMS, 0))) == 0);

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

    #ifdef LFS_GC
    // unknown gc flags?
    LFS_ASSERT((lfs->cfg->gc_flags & ~(
            LFS_GC_MKCONSISTENT
                | LFS_GC_LOOKAHEAD
                | LFS_GC_COMPACT
                | LFS_GC_CKMETA
                | LFS_GC_CKDATA)) == 0);
    #endif

    // check that gc_compact_thresh makes sense
    //
    // metadata can't be compacted below block_size/2, and metadata can't
    // exceed a block
    LFS_ASSERT(lfs->cfg->gc_compact_thresh == 0
            || lfs->cfg->gc_compact_thresh >= lfs->cfg->block_size/2);
    LFS_ASSERT(lfs->cfg->gc_compact_thresh == (lfs_size_t)-1
            || lfs->cfg->gc_compact_thresh <= lfs->cfg->block_size);

    // inline_size must be <= block_size/4
    LFS_ASSERT(lfs->cfg->inline_size <= lfs->cfg->block_size/4);
    // fragment_size must be <= block_size/4
    LFS_ASSERT(lfs->cfg->fragment_size <= lfs->cfg->block_size/4);
    // fragment_thresh > crystal_thresh is probably a mistake
    LFS_ASSERT(lfs->cfg->fragment_thresh == (lfs_size_t)-1
            || lfs->cfg->fragment_thresh <= lfs->cfg->crystal_thresh);

    // setup flags
    lfs->flags = flags
            // assume we contain orphans until proven otherwise
            | LFS_I_MKCONSISTENT
            // default to an empty lookahead
            | LFS_I_LOOKAHEAD
            // default to assuming we need compaction somewhere, worst case
            // this just makes lfsr_fs_gc read more than is strictly needed
            | LFS_I_COMPACT
            // default to needing a ckmeta/ckdata scan
            | LFS_I_CKMETA
            | LFS_I_CKDATA;

    // copy block_count so we can mutate it
    lfs->block_count = lfs->cfg->block_count;

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

    #ifdef LFS_CKPARITY
    // setup ptail, nothing should actually check off=0
    lfs->ptail.block = 0;
    lfs->ptail.off = 0;
    #endif

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
    lfs->lookahead.window = 0;
    lfs->lookahead.off = 0;
    lfs->lookahead.size = 0;
    lfs->lookahead.ckpoint = 0;
    lfs_alloc_discard(lfs);

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

//    lfs->root[0] = LFS_BLOCK_NULL;
//    lfs->root[1] = LFS_BLOCK_NULL;
//    lfs->mlist = NULL;
//    lfs->gdisk = (lfs_gstate_t){0};
//    lfs->gstate = (lfs_gstate_t){0};
//    lfs->gdelta = (lfs_gstate_t){0};
//#ifdef LFS_MIGRATE
//    lfs->lfs1 = NULL;
//#endif

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
    lfs->rattr_estimate = 3*tag_estimate + 4;

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
    lfs->mbits = lfs_nlog2(lfs->cfg->block_size) - 3;

    // zero linked-list of opened mdirs
    lfs->omdirs = NULL;

    // zero gstate
    lfs->gcksum = 0;
    lfs->gcksum_p = 0;
    lfs->gcksum_d = 0;

    lfs->grm.mids[0] = -1;
    lfs->grm.mids[1] = -1;
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


/// Mount/unmount ///

// compatibility flags
//
// - RCOMPAT => Must understand to read the filesystem
// - WCOMPAT => Must understand to write to the filesystem
// - OCOMPAT => Don't need to understand, we don't really use these
//
// note, "understanding" does not necessarily mean support
//
#define LFSR_RCOMPAT_NONSTANDARD 0x00000001 // Non-standard filesystem format
#define LFSR_RCOMPAT_WRONLY      0x00000002 // Reading is disallowed
#define LFSR_RCOMPAT_GRM         0x00000004 // Global-remove in use
#define LFSR_RCOMPAT_MMOSS       0x00000010 // May use an inlined mdir
#define LFSR_RCOMPAT_MSPROUT     0x00000020 // May use an mdir pointer
#define LFSR_RCOMPAT_MSHRUB      0x00000040 // May use an inlined mtree
#define LFSR_RCOMPAT_MTREE       0x00000080 // May use an mtree
#define LFSR_RCOMPAT_BMOSS       0x00000100 // Files may use inlined data
#define LFSR_RCOMPAT_BSPROUT     0x00000200 // Files may use block pointers
#define LFSR_RCOMPAT_BSHRUB      0x00000400 // Files may use inlined btrees
#define LFSR_RCOMPAT_BTREE       0x00000800 // Files may use btrees
// internal
#define LFSR_rcompat_OVERFLOW    0x80000000 // Can't represent all flags

#define LFSR_RCOMPAT_COMPAT \
    (LFSR_RCOMPAT_GRM \
        | LFSR_RCOMPAT_MMOSS \
        | LFSR_RCOMPAT_MTREE \
        | LFSR_RCOMPAT_BSHRUB \
        | LFSR_RCOMPAT_BTREE)

#define LFSR_WCOMPAT_NONSTANDARD 0x00000001 // Non-standard filesystem format
#define LFSR_WCOMPAT_RDONLY      0x00000002 // Writing is disallowed
#define LFSR_WCOMPAT_GCKSUM      0x00000004 // Global-checksum in use
// internal
#define LFSR_wcompat_OVERFLOW    0x80000000 // Can't represent all flags

#define LFSR_WCOMPAT_COMPAT \
    (LFSR_WCOMPAT_GCKSUM)

#define LFSR_OCOMPAT_NONSTANDARD 0x00000001 // Non-standard filesystem format
// internal
#define LFSR_ocompat_OVERFLOW    0x80000000 // Can't represent all flags

#define LFSR_OCOMPAT_COMPAT 0

typedef uint32_t lfsr_rcompat_t;
typedef uint32_t lfsr_wcompat_t;
typedef uint32_t lfsr_ocompat_t;

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

static int lfsr_data_readcompat(lfs_t *lfs, lfsr_data_t *data,
        uint32_t *compat) {
    // allow truncated compat flags
    uint8_t buf[4] = {0};
    lfs_ssize_t d = lfsr_data_read(lfs, data, buf, 4);
    if (d < 0) {
        return d;
    }
    *compat = lfs_fromle32_(buf);

    // if any out-of-range flags are set, set the internal overflow bit,
    // this is a compromise in correctness and and compat-flag complexity
    //
    // we don't really care about performance here
    while (lfsr_data_size(*data) > 0) {
        uint8_t b;
        lfs_ssize_t d = lfsr_data_read(lfs, data, &b, 1);
        if (d < 0) {
            return d;
        }

        if (b != 0x00) {
            *compat |= 0x80000000;
            break;
        }
    }

    return 0;
}

// all the compat parsing is basically the same, so try to reuse code

static inline int lfsr_data_readrcompat(lfs_t *lfs, lfsr_data_t *data,
        lfsr_rcompat_t *rcompat) {
    return lfsr_data_readcompat(lfs, data, rcompat);
}

static inline int lfsr_data_readwcompat(lfs_t *lfs, lfsr_data_t *data,
        lfsr_wcompat_t *wcompat) {
    return lfsr_data_readcompat(lfs, data, wcompat);
}

static inline int lfsr_data_readocompat(lfs_t *lfs, lfsr_data_t *data,
        lfsr_ocompat_t *ocompat) {
    return lfsr_data_readcompat(lfs, data, ocompat);
}


// disk geometry
//
// note these are stored minus 1 to avoid overflow issues
struct lfsr_geometry {
    lfs_off_t block_size;
    lfs_off_t block_count;
};

// geometry on-disk encoding
static lfsr_data_t lfsr_data_fromgeometry(const lfsr_geometry_t *geometry,
        uint8_t buffer[static LFSR_GEOMETRY_DSIZE]) {
    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfs_toleb128(geometry->block_size-1, &buffer[d], 4);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
    d += d_;

    d_ = lfs_toleb128(geometry->block_count-1, &buffer[d], 5);
    if (d_ < 0) {
        LFS_UNREACHABLE();
    }
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

static int lfsr_mountmroot(lfs_t *lfs, const lfsr_mdir_t *mroot) {
    // check the disk version
    uint8_t version[2] = {0, 0};
    lfsr_data_t data;
    int err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_VERSION,
            NULL, &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err != LFS_ERR_NOENT) {
        lfs_ssize_t d = lfsr_data_read(lfs, &data, version, 2);
        if (d < 0) {
            return err;
        }
    }

    if (version[0] != LFS_DISK_VERSION_MAJOR
            || version[1] > LFS_DISK_VERSION_MINOR) {
        LFS_ERROR("Incompatible version v%"PRId32".%"PRId32" "
                    "(!= v%"PRId32".%"PRId32")",
                version[0],
                version[1],
                LFS_DISK_VERSION_MAJOR,
                LFS_DISK_VERSION_MINOR);
        return LFS_ERR_NOTSUP;
    }

    // check for any rcompatflags, we must understand these to read
    // the filesystem
    lfsr_rcompat_t rcompat = 0;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_RCOMPAT,
            NULL, &data);
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
        LFS_ERROR("Incompatible rcompat flags 0x%0"PRIx32" (!= 0x%0"PRIx32")",
                rcompat,
                LFSR_RCOMPAT_COMPAT);
        return LFS_ERR_NOTSUP;
    }

    // check for any wcompatflags, we must understand these to write
    // the filesystem
    lfsr_wcompat_t wcompat = 0;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_WCOMPAT,
            NULL, &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err != LFS_ERR_NOENT) {
        err = lfsr_data_readwcompat(lfs, &data, &wcompat);
        if (err) {
            return err;
        }
    }

    if (lfsr_wcompat_isincompat(wcompat)) {
        LFS_WARN("Incompatible wcompat flags 0x%0"PRIx32" (!= 0x%0"PRIx32")",
                wcompat,
                LFSR_WCOMPAT_COMPAT);
        // we can continue if rdonly
        if (!lfsr_m_isrdonly(lfs->flags)) {
            return LFS_ERR_NOTSUP;
        }
    }

    // we don't bother to check for any ocompatflags, we would just
    // ignore these anyways

    // check the on-disk geometry
    lfsr_geometry_t geometry;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_GEOMETRY,
            NULL, &data);
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

    // either block_size matches or it doesn't, we don't support variable
    // block_sizes
    if (geometry.block_size != lfs->cfg->block_size) {
        LFS_ERROR("Incompatible block size %"PRId32" (!= %"PRId32")",
                geometry.block_size,
                lfs->cfg->block_size);
        return LFS_ERR_NOTSUP;
    }

    // on-disk block_count must be <= configured block_count
    if (geometry.block_count > lfs->cfg->block_count) {
        LFS_ERROR("Incompatible block count %"PRId32" (> %"PRId32")",
                geometry.block_count,
                lfs->cfg->block_count);
        return LFS_ERR_NOTSUP;
    }

    lfs->block_count = geometry.block_count;

    // read the name limit
    lfs_size_t name_limit = 0xff;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_NAMELIMIT,
            NULL, &data);
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
        LFS_ERROR("Incompatible name limit %"PRId32" (> %"PRId32")",
                name_limit,
                lfs->name_limit);
        return LFS_ERR_NOTSUP;
    }

    lfs->name_limit = name_limit;

    // read the file limit
    lfs_off_t file_limit = 0x7fffffff;
    err = lfsr_mdir_lookup(lfs, mroot, LFSR_TAG_FILELIMIT,
            NULL, &data);
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
        LFS_ERROR("Incompatible file limit %"PRId32" (> %"PRId32")",
                file_limit,
                lfs->file_limit);
        return LFS_ERR_NOTSUP;
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
        return LFS_ERR_NOTSUP;
    }

    return 0;
}

static int lfsr_mountinited(lfs_t *lfs) {
    // mark mroot as invalid to prevent lfsr_mtree_traverse from getting
    // confused
    lfs->mroot.mid = -1;
    lfs->mroot.rbyd.blocks[0] = -1;
    lfs->mroot.rbyd.blocks[1] = -1;

    // default to no mtree, this is allowed and implies all files are inlined
    // in the mroot
    lfsr_btree_init(&lfs->mtree);

    // zero gcksum/gdeltas, we'll read these from our mdirs
    lfs->gcksum = 0;
    lfsr_fs_flushgdelta(lfs);

    // traverse the mtree rooted at mroot 0x{1,0}
    //
    // we do validate btree inner nodes here, how can we trust our
    // mdirs are valid if we haven't checked the btree inner nodes at
    // least once?
    lfsr_traversal_t t;
    lfsr_traversal_init(&t, LFS_T_MTREEONLY | LFS_T_CKMETA);
    while (true) {
        lfsr_tag_t tag;
        lfsr_bptr_t bptr;
        int err = lfsr_mtree_traverse(lfs, &t,
                &tag, &bptr);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }

        // found an mdir?
        if (tag == LFSR_TAG_MDIR) {
            lfsr_mdir_t *mdir = (lfsr_mdir_t*)bptr.data.u.buffer;
            // found an mroot?
            if (mdir->mid == -1) {
                // check for the magic string, all mroot should have this
                lfsr_data_t data;
                err = lfsr_mdir_lookup(lfs, mdir, LFSR_TAG_MAGIC,
                        NULL, &data);
                if (err) {
                    if (err == LFS_ERR_NOENT) {
                        LFS_ERROR("No littlefs magic found");
                        return LFS_ERR_CORRUPT;
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
                    return LFS_ERR_CORRUPT;
                }

                // are we the last mroot?
                err = lfsr_mdir_lookup(lfs, mdir, LFSR_TAG_MROOT,
                        NULL, NULL);
                if (err && err != LFS_ERR_NOENT) {
                    return err;
                }
                if (err == LFS_ERR_NOENT) {
                    // track active mroot
                    lfs->mroot = *mdir;

                    // mount/validate config in active mroot
                    err = lfsr_mountmroot(lfs, &lfs->mroot);
                    if (err) {
                        return err;
                    }
                }
            }

            // build gcksum out of mdir cksums
            lfs->gcksum ^= mdir->rbyd.cksum;

            // collect any gdeltas from this mdir
            err = lfsr_fs_consumegdelta(lfs, mdir);
            if (err) {
                return err;
            }

        // found an mtree inner-node?
        } else if (tag == LFSR_TAG_BRANCH) {
            lfsr_rbyd_t *rbyd = (lfsr_rbyd_t*)bptr.data.u.buffer;
            // found the root of the mtree? keep track of this
            if (lfs->mtree.weight == 0) {
                lfs->mtree = *rbyd;
            }

        } else {
            LFS_UNREACHABLE();
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
    if (lfs_crc32c_cube(lfs->gcksum) != lfs->gcksum_d) {
        LFS_ERROR("Found gcksum mismatch, cksum^3 %08"PRIx32" "
                    "(!= %08"PRIx32")",
                lfs_crc32c_cube(lfs->gcksum),
                lfs->gcksum_d);
        return LFS_ERR_CORRUPT;
    }

    // keep track of the current gcksum
    lfs->gcksum_p = lfs->gcksum;

    // once we've mounted and derived a pseudo-random seed, initialize our
    // block allocator
    //
    // the purpose of this is to avoid bad wear patterns such as always 
    // allocating blocks near the beginning of disk after a power-loss
    //
    lfs->lookahead.window = lfs->gcksum % lfs->block_count;

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

    // found pending grms? this should only happen if we lost power
    if (lfsr_grm_count(lfs) == 2) {
        LFS_INFO("Found pending grm %"PRId32".%"PRId32" %"PRId32".%"PRId32,
                lfsr_dbgmbid(lfs, lfs->grm.mids[0]),
                lfsr_dbgmrid(lfs, lfs->grm.mids[0]),
                lfsr_dbgmbid(lfs, lfs->grm.mids[1]),
                lfsr_dbgmrid(lfs, lfs->grm.mids[1]));
    } else if (lfsr_grm_count(lfs) == 1) {
        LFS_INFO("Found pending grm %"PRId32".%"PRId32,
                lfsr_dbgmbid(lfs, lfs->grm.mids[0]),
                lfsr_dbgmrid(lfs, lfs->grm.mids[0]));
    }

    return 0;
}

// needed in lfsr_mount
static int lfsr_fs_gc_(lfs_t *lfs, lfsr_traversal_t *t,
        uint32_t flags, lfs_soff_t steps);

int lfsr_mount(lfs_t *lfs, uint32_t flags,
        const struct lfs_config *cfg) {
    // unknown flags?
    LFS_ASSERT((flags & ~(
            LFS_M_RDWR
                | LFS_M_RDONLY
                | LFS_M_FLUSH
                | LFS_M_SYNC
                | LFS_IFDEF_NOISY(LFS_M_NOISY, 0)
                | LFS_IFDEF_CKPROGS(LFS_M_CKPROGS, 0)
                | LFS_IFDEF_CKFETCHES(LFS_M_CKFETCHES, 0)
                | LFS_IFDEF_CKPARITY(LFS_M_CKPARITY, 0)
                | LFS_IFDEF_CKDATACKSUMS(LFS_M_CKDATACKSUMS, 0)
                | LFS_M_MKCONSISTENT
                | LFS_M_LOOKAHEAD
                | LFS_M_COMPACT
                | LFS_M_CKMETA
                | LFS_M_CKDATA)) == 0);
    // these flags require a writable filesystem
    LFS_ASSERT(!lfsr_m_isrdonly(flags) || !lfsr_t_ismkconsistent(flags));
    LFS_ASSERT(!lfsr_m_isrdonly(flags) || !lfsr_t_islookahead(flags));
    LFS_ASSERT(!lfsr_m_isrdonly(flags) || !lfsr_t_iscompact(flags));

    int err = lfs_init(lfs,
            flags & (
                LFS_M_RDWR
                    | LFS_M_RDONLY
                    | LFS_M_FLUSH
                    | LFS_M_SYNC
                    | LFS_IFDEF_NOISY(LFS_M_NOISY, 0)
                    | LFS_IFDEF_CKPROGS(LFS_M_CKPROGS, 0)
                    | LFS_IFDEF_CKFETCHES(LFS_M_CKFETCHES, 0)
                    | LFS_IFDEF_CKPARITY(LFS_M_CKPARITY, 0)
                    | LFS_IFDEF_CKDATACKSUMS(LFS_M_CKDATACKSUMS, 0)),
            cfg);
    if (err) {
        return err;
    }

    err = lfsr_mountinited(lfs);
    if (err) {
        goto failed;
    }

    // run gc if requested
    if (flags & (
            LFS_M_MKCONSISTENT
                | LFS_M_LOOKAHEAD
                | LFS_M_COMPACT
                | LFS_M_CKMETA
                | LFS_M_CKDATA)) {
        lfsr_traversal_t t;
        err = lfsr_fs_gc_(lfs, &t,
                flags & (
                    LFS_M_MKCONSISTENT
                        | LFS_M_LOOKAHEAD
                        | LFS_M_COMPACT
                        | LFS_M_CKMETA
                        | LFS_M_CKDATA),
                -1);
        if (err) {
            goto failed;
        }
    }

    // TODO this should use any configured values
    LFS_INFO("Mounted littlefs v%"PRId32".%"PRId32" %"PRId32"x%"PRId32" "
                "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32".%"PRId32", "
                "cksum %08"PRIx32,
            LFS_DISK_VERSION_MAJOR,
            LFS_DISK_VERSION_MINOR,
            lfs->cfg->block_size,
            lfs->block_count,
            lfs->mroot.rbyd.blocks[0],
            lfs->mroot.rbyd.blocks[1],
            lfsr_rbyd_trunk(&lfs->mroot.rbyd),
            lfs->mtree.weight >> lfs->mbits,
            1 << lfs->mbits,
            lfs->gcksum);

    return 0;

failed:;
    // make sure we clean up on error
    lfs_deinit(lfs);
    return err;
}

int lfsr_unmount(lfs_t *lfs) {
    // all files/dirs should be closed before lfsr_unmount
    LFS_ASSERT(lfs->omdirs == NULL
            // special case for our gc traversal handle
            || LFS_IFDEF_GC(
                (lfs->omdirs == &lfs->gc.t.b.o
                    && lfs->gc.t.b.o.next == NULL),
                false));

    return lfs_deinit(lfs);
}


/// Format ///

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
                | (((1 << (28-lfs_smax(lfs->recycle_bits, 0)))-1)
                    & 0x00216968);
        err = lfsr_rbyd_appendrev(lfs, &rbyd, rev);
        if (err) {
            return err;
        }

        // our initial superblock contains a couple things:
        // - our magic string, "littlefs"
        // - any format-time configuration
        // - the root's bookmark tag, which reserves did = 0 for the root
        err = lfsr_rbyd_appendrattrs(lfs, &rbyd, -1, -1, -1, LFSR_RATTRS(
                LFSR_RATTR_BUF(
                    LFSR_TAG_MAGIC, 0,
                    "littlefs", 8),
                LFSR_RATTR_BUF(
                    LFSR_TAG_VERSION, 0,
                    ((const uint8_t[2]){
                        LFS_DISK_VERSION_MAJOR,
                        LFS_DISK_VERSION_MINOR}), 2),
                LFSR_RATTR_LE32(
                    LFSR_TAG_RCOMPAT, 0,
                    LFSR_RCOMPAT_COMPAT),
                LFSR_RATTR_LE32(
                    LFSR_TAG_WCOMPAT, 0,
                    LFSR_WCOMPAT_COMPAT),
                LFSR_RATTR_GEOMETRY(
                    LFSR_TAG_GEOMETRY, 0,
                    (&(lfsr_geometry_t){
                        lfs->cfg->block_size,
                        lfs->cfg->block_count})),
                LFSR_RATTR_LLEB128(
                    LFSR_TAG_NAMELIMIT, 0,
                    lfs->name_limit),
                LFSR_RATTR_LEB128(
                    LFSR_TAG_FILELIMIT, 0,
                    lfs->file_limit),
                LFSR_RATTR_LEB128(
                    LFSR_TAG_BOOKMARK, +1,
                    0)));
        if (err) {
            return err;
        }

        // append initial gcksum
        uint32_t cksum = rbyd.cksum;
        err = lfsr_rbyd_appendrattr_(lfs, &rbyd, LFSR_RATTR_LE32(
                LFSR_TAG_GCKSUMDELTA, 0, lfs_crc32c_cube(cksum)));
        if (err) {
            return err;
        }

        // and commit
        err = lfsr_rbyd_appendcksum_(lfs, &rbyd, cksum);
        if (err) {
            return err;
        }
    }

    // sync on-disk state
    int err = lfsr_bd_sync(lfs);
    if (err) {
        return err;
    }

    return 0;
}

int lfsr_format(lfs_t *lfs, uint32_t flags,
        const struct lfs_config *cfg) {
    // unknown flags?
    LFS_ASSERT((flags & ~(
            LFS_F_RDWR
                | LFS_IFDEF_NOISY(LFS_F_NOISY, 0)
                | LFS_IFDEF_CKPROGS(LFS_F_CKPROGS, 0)
                | LFS_IFDEF_CKFETCHES(LFS_F_CKFETCHES, 0)
                | LFS_IFDEF_CKPARITY(LFS_F_CKPARITY, 0)
                | LFS_IFDEF_CKDATACKSUMS(LFS_F_CKDATACKSUMS, 0)
                | LFS_F_CKMETA
                | LFS_F_CKDATA)) == 0);

    int err = lfs_init(lfs,
            flags & (
                LFS_F_RDWR
                    | LFS_IFDEF_NOISY(LFS_F_NOISY, 0)
                    | LFS_IFDEF_CKPROGS(LFS_F_CKPROGS, 0)
                    | LFS_IFDEF_CKFETCHES(LFS_F_CKFETCHES, 0)
                    | LFS_IFDEF_CKPARITY(LFS_F_CKPARITY, 0)
                    | LFS_IFDEF_CKDATACKSUMS(LFS_F_CKDATACKSUMS, 0)),
            cfg);
    if (err) {
        return err;
    }

    LFS_INFO("Formatting littlefs v%"PRId32".%"PRId32" %"PRId32"x%"PRId32,
            LFS_DISK_VERSION_MAJOR,
            LFS_DISK_VERSION_MINOR,
            lfs->cfg->block_size,
            lfs->block_count);

    err = lfsr_formatinited(lfs);
    if (err) {
        goto failed;
    }

    // test that mount works with our formatted disk
    err = lfsr_mountinited(lfs);
    if (err) {
        goto failed;
    }

    // run gc if requested
    if (flags & (
            LFS_F_CKMETA
                | LFS_F_CKDATA)) {
        lfsr_traversal_t t;
        err = lfsr_fs_gc_(lfs, &t,
                flags & (
                    LFS_F_CKMETA
                        | LFS_F_CKDATA),
                -1);
        if (err) {
            goto failed;
        }
    }

    return lfs_deinit(lfs);

failed:;
    // make sure we clean up on error
    lfs_deinit(lfs);
    return err;
}



/// Other filesystem things  ///

int lfsr_fs_stat(lfs_t *lfs, struct lfs_fsinfo *fsinfo) {
    // return various filesystem flags
    fsinfo->flags = lfs->flags & (
            LFS_I_RDONLY
                | LFS_I_FLUSH
                | LFS_I_SYNC
                | LFS_IFDEF_NOISY(LFS_I_NOISY, 0)
                | LFS_IFDEF_CKPROGS(LFS_I_CKPROGS, 0)
                | LFS_IFDEF_CKFETCHES(LFS_I_CKFETCHES, 0)
                | LFS_IFDEF_CKPARITY(LFS_I_CKPARITY, 0)
                | LFS_IFDEF_CKDATACKSUMS(LFS_I_CKDATACKSUMS, 0)
                | LFS_I_MKCONSISTENT
                | LFS_I_LOOKAHEAD
                | LFS_I_COMPACT
                | LFS_I_CKMETA
                | LFS_I_CKDATA);
    // some flags we calculate on demand
    fsinfo->flags |= (lfsr_grm_count(lfs) > 0) ? LFS_I_MKCONSISTENT : 0;

    // return filesystem config, this may come from disk
    fsinfo->block_size = lfs->cfg->block_size;
    fsinfo->block_count = lfs->block_count;
    fsinfo->name_limit = lfs->name_limit;
    fsinfo->file_limit = lfs->file_limit;

    return 0;
}

lfs_ssize_t lfsr_fs_size(lfs_t *lfs) {
    lfs_size_t count = 0;
    lfsr_traversal_t t;
    lfsr_traversal_init(&t, 0);
    while (true) {
        lfsr_tag_t tag;
        int err = lfsr_mtree_traverse(lfs, &t,
                &tag, NULL);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }

        // count the number of blocks we see, yes this may result in duplicates
        if (tag == LFSR_TAG_MDIR) {
            count += 2;

        } else if (tag == LFSR_TAG_BRANCH) {
            count += 1;

        } else if (tag == LFSR_TAG_BLOCK) {
            count += 1;

        } else {
            LFS_UNREACHABLE();
        }
    }

    return count;
}


// consistency stuff

static int lfsr_fs_fixgrm(lfs_t *lfs) {
    if (lfsr_grm_count(lfs) == 2) {
        LFS_INFO("Fixing grm %"PRId32".%"PRId32" %"PRId32".%"PRId32,
                lfsr_dbgmbid(lfs, lfs->grm.mids[0]),
                lfsr_dbgmrid(lfs, lfs->grm.mids[0]),
                lfsr_dbgmbid(lfs, lfs->grm.mids[1]),
                lfsr_dbgmrid(lfs, lfs->grm.mids[1]));
    } else if (lfsr_grm_count(lfs) == 1) {
        LFS_INFO("Fixing grm %"PRId32".%"PRId32,
                lfsr_dbgmbid(lfs, lfs->grm.mids[0]),
                lfsr_dbgmrid(lfs, lfs->grm.mids[0]));
    }

    while (lfsr_grm_count(lfs) > 0) {
        LFS_ASSERT(lfs->grm.mids[0] != -1);

        // find our mdir
        lfsr_mdir_t mdir;
        int err = lfsr_mtree_lookupleaf(lfs, lfs->grm.mids[0],
                &mdir);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // we also use grm to track orphans that need to be cleaned up,
        // which means it may not match the on-disk state, which means
        // we need to revert manually on error
        lfsr_grm_t grm_p = lfs->grm;

        // mark grm as taken care of
        lfsr_grm_pop(lfs);
        // checkpoint the allocator
        lfs_alloc_ckpoint(lfs);
        // remove the rid while atomically updating our grm
        err = lfsr_mdir_commit(lfs, &mdir, LFSR_RATTRS(
                LFSR_RATTR(LFSR_TAG_RM, -1)));
        if (err) {
            // revert grm manually
            lfs->grm = grm_p;
            return err;
        }
    }

    return 0;
}

static int lfsr_mdir_mkconsistent(lfs_t *lfs, lfsr_mdir_t *mdir) {
    // save the current mid
    lfsr_mid_t mid = mdir->mid;

    // iterate through mids looking for orphans
    mdir->mid = LFSR_MID(lfs, mdir->mid, 0);
    int err;
    while (lfsr_mrid(lfs, mdir->mid) < (lfsr_srid_t)mdir->rbyd.weight) {
        // is this mid open? well we're not an orphan then, skip
        if (lfsr_omdir_ismidopen(lfs, mdir->mid, -1)) {
            mdir->mid += 1;
            continue;
        }

        // is this mid marked as a stickynote?
        err = lfsr_mdir_lookup(lfs, mdir, LFSR_TAG_STICKYNOTE,
                NULL, NULL);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                mdir->mid += 1;
                continue;
            }
            goto failed;
        }

        // we found an orphaned stickynote, remove
        LFS_INFO("Fixing orphaned stickynote %"PRId32".%"PRId32,
                lfsr_dbgmbid(lfs, mdir->mid),
                lfsr_dbgmrid(lfs, mdir->mid));

        lfs_alloc_ckpoint(lfs);
        err = lfsr_mdir_commit(lfs, mdir, LFSR_RATTRS(
                LFSR_RATTR(LFSR_TAG_RM, -1)));
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

static int lfsr_fs_fixorphans(lfs_t *lfs) {
    // LFS_T_MKCONSISTENT really just removes orphans
    lfsr_traversal_t t;
    lfsr_traversal_init(&t, LFS_T_MTREEONLY | LFS_T_MKCONSISTENT);
    while (true) {
        int err = lfsr_mtree_gc(lfs, &t,
                NULL, NULL);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }
    }

    return 0;
}

// prepare the filesystem for mutation
int lfsr_fs_mkconsistent(lfs_t *lfs) {
    // filesystem must be writeable
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags));

    // fix pending grms
    if (lfsr_grm_count(lfs) > 0) {
        int err = lfsr_fs_fixgrm(lfs);
        if (err) {
            return err;
        }
    }

    // fix orphaned stickynotes
    //
    // this must happen after fixgrm, since removing orphaned
    // stickynotes risks outdating the grm
    //
    if (lfsr_t_ismkconsistent(lfs->flags)) {
        int err = lfsr_fs_fixorphans(lfs);
        if (err) {
            return err;
        }
    }

    return 0;
}

// filesystem check functions
static int lfsr_fs_ck(lfs_t *lfs, uint32_t flags) {
    // we leave this up to lfsr_mtree_traverse
    lfsr_traversal_t t;
    lfsr_traversal_init(&t, flags);
    while (true) {
        int err = lfsr_mtree_traverse(lfs, &t,
                NULL, NULL);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }
    }

    return 0;
}

int lfsr_fs_ckmeta(lfs_t *lfs) {
    return lfsr_fs_ck(lfs, LFS_T_CKMETA);
}

int lfsr_fs_ckdata(lfs_t *lfs) {
    return lfsr_fs_ck(lfs, LFS_T_CKMETA | LFS_T_CKDATA);
}

// get the filesystem checksum
int lfsr_fs_cksum(lfs_t *lfs, uint32_t *cksum) {
    *cksum = lfs->gcksum;
    return 0;
}

// low-level filesystem gc
//
// runs the traversal until all work is completed, which may take
// multiple passes
static int lfsr_fs_gc_(lfs_t *lfs, lfsr_traversal_t *t,
        uint32_t flags, lfs_soff_t steps) {
    // unknown gc flags?
    //
    // we should have check these earlier, but it doesn't hurt to
    // double check
    LFS_ASSERT((flags & ~(
            LFS_GC_MKCONSISTENT
                | LFS_GC_LOOKAHEAD
                | LFS_GC_COMPACT
                | LFS_GC_CKMETA
                | LFS_GC_CKDATA)) == 0);
    // these flags require a writable filesystem
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags) || !lfsr_t_ismkconsistent(flags));
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags) || !lfsr_t_islookahead(flags));
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags) || !lfsr_t_iscompact(flags));
    // some flags don't make sense when only traversing the mtree
    LFS_ASSERT(!lfsr_t_ismtreeonly(flags) || !lfsr_t_islookahead(flags));
    LFS_ASSERT(!lfsr_t_ismtreeonly(flags) || !lfsr_t_isckdata(flags));

    // fix pending grms if requested
    if (lfsr_t_ismkconsistent(flags)
            && lfsr_grm_count(lfs) > 0) {
        int err = lfsr_fs_fixgrm(lfs);
        if (err) {
            return err;
        }
    }

    // do we have any pending work?
    uint32_t pending = flags & (
            (lfs->flags & (
                LFS_I_MKCONSISTENT
                    | LFS_I_LOOKAHEAD
                    | LFS_I_COMPACT
                    | LFS_I_CKMETA
                    | LFS_I_CKDATA)));

    while (pending && (lfs_off_t)steps > 0) {
        // checkpoint the allocator to maximize any lookahead scans
        lfs_alloc_ckpoint(lfs);

        // start a new traversal?
        if (!lfsr_omdir_isopen(lfs, &t->b.o)) {
            lfsr_traversal_init(t, pending);
            lfsr_omdir_open(lfs, &t->b.o);
        }

        // don't bother with lookahead if we've mutated
        if (lfsr_t_isdirty(t->b.o.flags)
                || lfsr_t_ismutated(t->b.o.flags)) {
            t->b.o.flags &= ~LFS_GC_LOOKAHEAD;
        }

        // will this traversal still make progress? no? start over
        if (!(t->b.o.flags & (
                LFS_GC_MKCONSISTENT
                    | LFS_GC_LOOKAHEAD
                    | LFS_GC_COMPACT
                    | LFS_GC_CKMETA
                    | LFS_GC_CKDATA))) {
            lfsr_omdir_close(lfs, &t->b.o);
            continue;
        }

        // do we really need a full traversal?
        if (!(t->b.o.flags & (
                LFS_GC_LOOKAHEAD
                    | LFS_GC_CKMETA
                    | LFS_GC_CKDATA))) {
            t->b.o.flags |= LFS_T_MTREEONLY;
        }

        // progress gc
        int err = lfsr_mtree_gc(lfs, t,
                NULL, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // end of traversal?
        if (err == LFS_ERR_NOENT) {
            lfsr_omdir_close(lfs, &t->b.o);

            // clear any pending flags we make progress on
            pending &= lfs->flags & (
                    LFS_I_MKCONSISTENT
                        | LFS_I_LOOKAHEAD
                        | LFS_I_COMPACT
                        | LFS_I_CKMETA
                        | LFS_I_CKDATA);
        }

        // decrement steps
        if (steps > 0) {
            steps -= 1;
        }
    }

    return 0;
}

#ifdef LFS_GC
// incremental filesystem gc
//
// perform any pending janitorial work
int lfsr_fs_gc(lfs_t *lfs) {
    return lfsr_fs_gc_(lfs, &lfs->gc.t,
            lfs->cfg->gc_flags,
            (lfs->cfg->gc_steps)
                ? lfs->cfg->gc_steps
                : 1);
}
#endif

// unperform janitorial work
int lfsr_fs_unck(lfs_t *lfs, uint32_t flags) {
    // unknown flags?
    LFS_ASSERT((flags & ~(
            LFS_I_MKCONSISTENT
                | LFS_I_LOOKAHEAD
                | LFS_I_COMPACT
                | LFS_I_CKMETA
                | LFS_I_CKDATA)) == 0);

    // reset the requested flags
    lfs->flags |= flags;

    #ifdef LFS_GC
    // and clear from any ongoing traversals
    //
    // lfsr_fs_gc will terminate early if it discovers it can no longer
    // make progress
    lfs->gc.t.b.o.flags &= ~flags;
    #endif

    return 0;
}


// attempt to grow the filesystem
int lfsr_fs_grow(lfs_t *lfs, lfs_size_t block_count_) {
    // filesystem must be writeable
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags));
    // shrinking the filesystem is not supported
    LFS_ASSERT(block_count_ >= lfs->block_count);

    // do nothing if block_count doesn't change
    if (block_count_ == lfs->block_count) {
        return 0;
    }

    // Note we do _not_ call lfsr_fs_mkconsistent here. This is a bit scary,
    // but we should be ok as long as we patch grms in lfsr_mdir_commit and
    // only commit to the mroot.
    //
    // Calling lfsr_fs_mkconsistent risks locking our filesystem up trying
    // to fix grms/orphans before we can commit the new filesystem size. If
    // we don't, we should always be able to recover a stuck filesystem with
    // lfsr_fs_grow.

    LFS_INFO("Growing littlefs %"PRId32"x%"PRId32" -> %"PRId32"x%"PRId32,
            lfs->cfg->block_size, lfs->block_count,
            lfs->cfg->block_size, block_count_);

    // keep track of our current block_count in case we fail
    lfs_size_t block_count = lfs->block_count;

    // we can use the new blocks immediately as long as the commit
    // with the new block_count is atomic
    lfs->block_count = block_count_;
    // discard stale lookahead buffer
    lfs_alloc_discard(lfs);

    // update our on-disk config
    lfs_alloc_ckpoint(lfs);
    int err = lfsr_mdir_commit(lfs, &lfs->mroot, LFSR_RATTRS(
            LFSR_RATTR_GEOMETRY(
                LFSR_TAG_GEOMETRY, 0,
                (&(lfsr_geometry_t){
                    lfs->cfg->block_size,
                    block_count_}))));
    if (err) {
        goto failed;
    }

    return 0;

failed:;
    // restore block_count
    lfs->block_count = block_count;
    // discard clobbered lookahead buffer
    lfs_alloc_discard(lfs);

    return err;
}


/// High-level filesystem traversal ///

// needed in lfsr_traversal_open
static int lfsr_traversal_rewind_(lfs_t *lfs, lfsr_traversal_t *t);

int lfsr_traversal_open(lfs_t *lfs, lfsr_traversal_t *t, uint32_t flags) {
    // already open?
    LFS_ASSERT(!lfsr_omdir_isopen(lfs, &t->b.o));
    // unknown flags?
    LFS_ASSERT((flags & ~(
            LFS_T_MTREEONLY
                | LFS_T_MKCONSISTENT
                | LFS_T_LOOKAHEAD
                | LFS_T_COMPACT
                | LFS_T_CKMETA
                | LFS_T_CKDATA)) == 0);
    // these flags require a writable filesystem
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags) || !lfsr_t_ismkconsistent(flags));
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags) || !lfsr_t_islookahead(flags));
    LFS_ASSERT(!lfsr_m_isrdonly(lfs->flags) || !lfsr_t_iscompact(flags));
    // some flags don't make sense when only traversing the mtree
    LFS_ASSERT(!lfsr_t_ismtreeonly(flags) || !lfsr_t_islookahead(flags));
    LFS_ASSERT(!lfsr_t_ismtreeonly(flags) || !lfsr_t_isckdata(flags));

    // setup traversal state
    t->b.o.flags = lfsr_o_settype(flags, LFS_TYPE_TRAVERSAL);

    // let rewind initialize/reset things
    int err = lfsr_traversal_rewind_(lfs, t);
    if (err) {
        return err;
    }

    // add to tracked mdirs
    lfsr_omdir_open(lfs, &t->b.o);
    return 0;
}

int lfsr_traversal_close(lfs_t *lfs, lfsr_traversal_t *t) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &t->b.o));

    // remove from tracked mdirs
    lfsr_omdir_close(lfs, &t->b.o);
    return 0;
}

int lfsr_traversal_read(lfs_t *lfs, lfsr_traversal_t *t,
        struct lfs_tinfo *tinfo) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &t->b.o));

    // check for pending grms every step, just in case some other
    // operation introduced new grms
    if (lfsr_t_ismkconsistent(t->b.o.flags)
            && lfsr_grm_count(lfs) > 0) {
        // swap dirty/mutated flags while mutating
        t->b.o.flags = lfsr_t_swapdirty(t->b.o.flags);

        int err = lfsr_fs_fixgrm(lfs);
        if (err) {
            t->b.o.flags = lfsr_t_swapdirty(t->b.o.flags);
            return err;
        }

        t->b.o.flags = lfsr_t_swapdirty(t->b.o.flags);
    }

    // checkpoint the allocator to maximize any lookahead scans
    lfs_alloc_ckpoint(lfs);

    while (true) {
        // some redund blocks left over?
        if (t->blocks[0] != -1) {
            // write our traversal info
            tinfo->btype = lfsr_t_btype(t->b.o.flags);
            tinfo->block = t->blocks[0];

            t->blocks[0] = t->blocks[1];
            t->blocks[1] = -1;
            return 0;
        }

        // find next block
        lfsr_tag_t tag;
        lfsr_bptr_t bptr;
        int err = lfsr_mtree_gc(lfs, t,
                &tag, &bptr);
        if (err) {
            return err;
        }

        // figure out type/blocks
        if (tag == LFSR_TAG_MDIR) {
            lfsr_mdir_t *mdir = (lfsr_mdir_t*)bptr.data.u.buffer;
            t->b.o.flags = lfsr_t_setbtype(t->b.o.flags, LFS_BTYPE_MDIR);
            t->blocks[0] = mdir->rbyd.blocks[0];
            t->blocks[1] = mdir->rbyd.blocks[1];

        } else if (tag == LFSR_TAG_BRANCH) {
            t->b.o.flags = lfsr_t_setbtype(t->b.o.flags, LFS_BTYPE_BTREE);
            lfsr_rbyd_t *rbyd = (lfsr_rbyd_t*)bptr.data.u.buffer;
            t->blocks[0] = rbyd->blocks[0];
            t->blocks[1] = -1;

        } else if (tag == LFSR_TAG_BLOCK) {
            t->b.o.flags = lfsr_t_setbtype(t->b.o.flags, LFS_BTYPE_DATA);
            t->blocks[0] = bptr.data.u.disk.block;
            t->blocks[1] = -1;

        } else {
            LFS_UNREACHABLE();
        }
    }
}

static void lfsr_traversal_clobber(lfs_t *lfs, lfsr_traversal_t *t) {
    (void)lfs;
    // mroot/mtree? transition to mdir iteration
    if (lfsr_t_tstate(t->b.o.flags) < LFSR_TSTATE_MDIRS) {
        t->b.o.flags = lfsr_t_settstate(t->b.o.flags, LFSR_TSTATE_MDIRS);
        t->b.o.mdir.mid = 0;
        lfsr_bshrub_init(&t->b);
        t->ot = NULL;
    // in-mtree mdir? increment the mid (to make progress) and reset to
    // mdir iteration
    } else if (lfsr_t_tstate(t->b.o.flags) < LFSR_TSTATE_OMDIRS) {
        t->b.o.flags = lfsr_t_settstate(t->b.o.flags, LFSR_TSTATE_MDIR);
        t->b.o.mdir.mid += 1;
        lfsr_bshrub_init(&t->b);
        t->ot = NULL;
    // opened mdir? skip to next omdir
    } else if (lfsr_t_tstate(t->b.o.flags) < LFSR_TSTATE_DONE) {
        t->b.o.flags = lfsr_t_settstate(t->b.o.flags, LFSR_TSTATE_OMDIRS);
        lfsr_bshrub_init(&t->b);
        t->ot = (t->ot) ? t->ot->next : NULL;
    // done traversals should never need clobbering
    } else {
        LFS_UNREACHABLE();
    }

    // and clear any pending blocks
    t->blocks[0] = -1;
    t->blocks[1] = -1;
}

static int lfsr_traversal_rewind_(lfs_t *lfs, lfsr_traversal_t *t) {
    (void)lfs;

    // reset traversal
    lfsr_traversal_init(t,
            t->b.o.flags & ~(LFS_t_DIRTY | LFS_t_MUTATED | LFS_t_TSTATE));

    // and clear any pending blocks
    t->blocks[0] = -1;
    t->blocks[1] = -1;

    return 0;
}

int lfsr_traversal_rewind(lfs_t *lfs, lfsr_traversal_t *t) {
    LFS_ASSERT(lfsr_omdir_isopen(lfs, &t->b.o));

    return lfsr_traversal_rewind_(lfs, t);
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

