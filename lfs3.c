/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs3.h"
#include "lfs3_util.h"


// internally used disk-comparison enum
//
// note LT < EQ < GT
enum lfs3_scmp {
    LFS3_CMP_LT = 0, // disk < query
    LFS3_CMP_EQ = 1, // disk = query
    LFS3_CMP_GT = 2, // disk > query
};

typedef int lfs3_scmp_t;

// this is just a hint that the function returns a bool + err union
typedef int lfs3_sbool_t;


/// Simple bd wrappers (asserts go here) ///

static int lfs3_bd_read__(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        void *buffer, lfs3_size_t size) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);
    // must be aligned
    LFS3_ASSERT(off % lfs3->cfg->read_size == 0);
    LFS3_ASSERT(size % lfs3->cfg->read_size == 0);

    // bd read
    int err = lfs3->cfg->read(lfs3->cfg, block, off, buffer, size);
    LFS3_ASSERT(err <= 0);
    if (err) {
        LFS3_INFO("Bad read 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                block, off, size, err);
        return err;
    }

    return 0;
}

#ifndef LFS3_RDONLY
static int lfs3_bd_prog__(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        const void *buffer, lfs3_size_t size) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);
    // must be aligned
    LFS3_ASSERT(off % lfs3->cfg->prog_size == 0);
    LFS3_ASSERT(size % lfs3->cfg->prog_size == 0);

    // bd prog
    int err = lfs3->cfg->prog(lfs3->cfg, block, off, buffer, size);
    LFS3_ASSERT(err <= 0);
    if (err) {
        LFS3_INFO("Bad prog 0x%"PRIx32".%"PRIx32" %"PRIu32" (%d)",
                block, off, size, err);
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_erase__(lfs3_t *lfs3, lfs3_block_t block) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);

    // bd erase
    int err = lfs3->cfg->erase(lfs3->cfg, block);
    LFS3_ASSERT(err <= 0);
    if (err) {
        LFS3_INFO("Bad erase 0x%"PRIx32" (%d)",
                block, err);
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_sync__(lfs3_t *lfs3) {
    // bd sync
    int err = lfs3->cfg->sync(lfs3->cfg);
    LFS3_ASSERT(err <= 0);
    if (err) {
        LFS3_INFO("Bad sync (%d)", err);
        return err;
    }

    return 0;
}
#endif


/// Caching block device operations ///

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
        lfs3_size_t size,
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
                lfs3->rcache.buffer, size__);
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
        void *buffer, lfs3_size_t size) {
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
            int err = lfs3_bd_read__(lfs3, block, off_, buffer_, d);
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
                lfs3->rcache.buffer, size__);
        if (err) {
            return err;
        }

        lfs3->rcache.block = block;
        lfs3->rcache.off = off__;
        lfs3->rcache.size = size__;
    }

    return 0;
}

// needed in lfs3_bd_prog_ for prog validation
#ifdef LFS3_CKPROGS
static inline bool lfs3_m_isckprogs(uint32_t flags);
#endif
static lfs3_scmp_t lfs3_bd_cmp(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        const void *buffer, lfs3_size_t size);

// low-level prog stuff
#ifndef LFS3_RDONLY
static int lfs3_bd_prog_(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        const void *buffer, lfs3_size_t size,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    // prog to disk
    int err = lfs3_bd_prog__(lfs3, block, off, buffer, size);
    if (err) {
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

    // optional aligned checksum
    if (cksum && align) {
        *cksum = lfs3_crc32c(*cksum, buffer, size);
    }

    return 0;
}
#endif

// flush the pcache
#ifndef LFS3_RDONLY
static int lfs3_bd_flush(lfs3_t *lfs3, uint32_t *cksum, bool align) {
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
                lfs3->pcache.off, lfs3->pcache.buffer, size,
                cksum, align);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

// caching prog that lends you a buffer
//
// with optional checksum
#ifndef LFS3_RDONLY
static int lfs3_bd_prognext(lfs3_t *lfs3, lfs3_block_t block, lfs3_size_t off,
        lfs3_size_t size,
        uint8_t **buffer_, lfs3_size_t *size_,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    while (true) {
        // active pcache?
        if (lfs3->pcache.block == block
                && lfs3->pcache.size != 0) {
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
            int err = lfs3_bd_flush(lfs3, cksum, align);
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
        const void *buffer, lfs3_size_t size,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t off_ = off;
    const uint8_t *buffer_ = buffer;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        // active pcache?
        if (lfs3->pcache.block == block
                && lfs3->pcache.size != 0) {
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
            int err = lfs3_bd_flush(lfs3, cksum, align);
            if (err) {
                return err;
            }
        }

        // bypass pcache?
        if (off_ % lfs3->cfg->prog_size == 0
                && size_ >= lfs3->cfg->pcache_size) {
            lfs3_size_t d = lfs3_aligndown(size_, lfs3->cfg->prog_size);
            int err = lfs3_bd_prog_(lfs3, block, off_, buffer_, d,
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
        lfs3->pcache.block = block;
        lfs3->pcache.off = lfs3_aligndown(off_, lfs3->cfg->prog_size);
        lfs3->pcache.size = lfs3_min(
                (off_-lfs3->pcache.off) + size_,
                lfs3->cfg->pcache_size);

        // zero to avoid any information leaks
        lfs3_memset(lfs3->pcache.buffer, 0xff, lfs3->cfg->pcache_size);
    }

    // optional checksum
    if (cksum && !align) {
        *cksum = lfs3_crc32c(*cksum, buffer, size);
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_sync(lfs3_t *lfs3) {
    // make sure we flush any caches
    int err = lfs3_bd_flush(lfs3, NULL, false);
    if (err) {
        return err;
    }

    return lfs3_bd_sync__(lfs3);
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_bd_erase(lfs3_t *lfs3, lfs3_block_t block) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);

    // invalidate any relevant caches
    if (lfs3->pcache.block == block) {
        lfs3_bd_droppcache(lfs3);
    }
    if (lfs3->rcache.block == block) {
        lfs3_bd_droprcache(lfs3);
    }

    return lfs3_bd_erase__(lfs3, block);
}
#endif


// other block device utils

static int lfs3_bd_cksum(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t size,
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
        int err = lfs3_bd_readnext(lfs3, block, off_, hint_, size_,
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
        const void *buffer, lfs3_size_t size) {
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
        int err = lfs3_bd_readnext(lfs3, block, off_, hint_, size_,
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
        lfs3_size_t size,
        uint32_t *cksum, bool align) {
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
        int err = lfs3_bd_prognext(lfs3, dst_block, dst_off_, size_,
                &buffer__, &size__,
                cksum, align);
        if (err) {
            return err;
        }

        err = lfs3_bd_read(lfs3, src_block, src_off_, hint_,
                buffer__, size__);
        if (err) {
            return err;
        }

        // optional checksum
        if (cksum && !align) {
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
        uint8_t c, lfs3_size_t size,
        uint32_t *cksum, bool align) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(off+size <= lfs3->cfg->block_size);

    lfs3_size_t off_ = off;
    lfs3_size_t size_ = size;
    while (size_ > 0) {
        uint8_t *buffer__;
        lfs3_size_t size__;
        int err = lfs3_bd_prognext(lfs3, block, off_, size_,
                &buffer__, &size__,
                cksum, align);
        if (err) {
            return err;
        }

        lfs3_memset(buffer__, c, size__);

        // optional checksum
        if (cksum && !align) {
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

#ifdef LFS3_CKDATACKSUMREADS
static int lfs3_bd_ckprefix(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t cksize, uint32_t cksum,
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

#ifdef LFS3_CKDATACKSUMREADS
static int lfs3_bd_cksuffix(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_size_t cksize, uint32_t cksum,
        uint32_t cksum__) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(cksize <= lfs3->cfg->block_size);

    // checksum any suffixed data
    int err = lfs3_bd_cksum(lfs3,
            block, off, hint,
            cksize - off,
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
#ifdef LFS3_CKDATACKSUMREADS
static int lfs3_bd_readck(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        void *buffer, lfs3_size_t size,
        lfs3_size_t cksize, uint32_t cksum) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(cksize <= lfs3->cfg->block_size);
    // read should fit in ck info
    LFS3_ASSERT(off+size <= cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs3_size_t hint_;
    int err = lfs3_bd_ckprefix(lfs3, block, off, hint,
            cksize, cksum,
            &hint_,
            &cksum__);
    if (err) {
        return err;
    }

    // read and checksum the data we're interested in
    err = lfs3_bd_read(lfs3,
            block, off, hint_,
            buffer, size);
    if (err) {
        return err;
    }

    cksum__ = lfs3_crc32c(cksum__, buffer, size);

    // checksum any suffixed data and validate
    err = lfs3_bd_cksuffix(lfs3, block, off+size, hint_-size,
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
// we'd also need to worry about early termination in lfs3_bd_cmp/cmpck

#ifdef LFS3_CKDATACKSUMREADS
static lfs3_scmp_t lfs3_bd_cmpck(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        const void *buffer, lfs3_size_t size,
        lfs3_size_t cksize, uint32_t cksum) {
    // must be in-bounds
    LFS3_ASSERT(block < lfs3->block_count);
    LFS3_ASSERT(cksize <= lfs3->cfg->block_size);
    // read should fit in ck info
    LFS3_ASSERT(off+size <= cksize);

    // checksum any prefixed data
    uint32_t cksum__ = 0;
    lfs3_size_t hint_;
    int err = lfs3_bd_ckprefix(lfs3, block, off, hint,
            cksize, cksum,
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
        err = lfs3_bd_readnext(lfs3, block, off_, hint__, size_,
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
            cksize, cksum,
            cksum__);
    if (err) {
        return err;
    }

    return cmp;
}
#endif

#if !defined(LFS3_RDONLY) && defined(LFS3_CKDATACKSUMREADS)
static int lfs3_bd_cpyck(lfs3_t *lfs3,
        lfs3_block_t dst_block, lfs3_size_t dst_off,
        lfs3_block_t src_block, lfs3_size_t src_off, lfs3_size_t hint,
        lfs3_size_t size,
        lfs3_size_t src_cksize, uint32_t src_cksum,
        uint32_t *cksum, bool align) {
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
            src_cksize, src_cksum,
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
        err = lfs3_bd_prognext(lfs3, dst_block, dst_off_, size_,
                &buffer__, &size__,
                cksum, align);
        if (err) {
            return err;
        }

        err = lfs3_bd_read(lfs3, src_block, src_off_, hint__,
                buffer__, size__);
        if (err) {
            return err;
        }

        // validating checksum
        cksum__ = lfs3_crc32c(cksum__, buffer__, size__);

        // optional prog checksum
        if (cksum && !align) {
            *cksum = lfs3_crc32c(*cksum, buffer__, size__);
        }

        dst_off_ += size__;
        src_off_ += size__;
        hint__ -= size__;
        size_ -= size__;
    }

    // checksum any suffixed data and validate
    err = lfs3_bd_cksuffix(lfs3, src_block, src_off+size, hint_-size,
            src_cksize, src_cksum,
            cksum__);
    if (err) {
        return err;
    }

    return 0;
}
#endif




/// lfs3_tag_t stuff ///

// 16-bit metadata tags
enum lfs3_tag {
    // the null tag is reserved
    LFS3_TAG_NULL           = 0x0000,

    // config tags
    LFS3_TAG_CONFIG         = 0x0000,
    LFS3_TAG_MAGIC          = 0x0031,
    LFS3_TAG_VERSION        = 0x0034,
    LFS3_TAG_RCOMPAT        = 0x0035,
    LFS3_TAG_WCOMPAT        = 0x0036,
    LFS3_TAG_OCOMPAT        = 0x0037,
    LFS3_TAG_GEOMETRY       = 0x0038,
    LFS3_TAG_NAMELIMIT      = 0x0039,
    LFS3_TAG_FILELIMIT      = 0x003a,
    // in-device only, to help find unknown config tags
    LFS3_TAG_UNKNOWNCONFIG  = 0x003b,

    // global-state tags
    LFS3_TAG_GDELTA         = 0x0100,
    LFS3_TAG_GRMDELTA       = 0x0100,

    // name tags
    LFS3_TAG_NAME           = 0x0200,
    LFS3_TAG_BNAME          = 0x0200,
    LFS3_TAG_REG            = 0x0201,
    LFS3_TAG_DIR            = 0x0202,
    LFS3_TAG_STICKYNOTE     = 0x0203,
    LFS3_TAG_BOOKMARK       = 0x0204,
    // in-device only name tags, these should never get written to disk
    LFS3_TAG_ORPHAN         = 0x0205,
    LFS3_TAG_TRAVERSAL      = 0x0206,
    LFS3_TAG_UNKNOWN        = 0x0207,
    // non-file name tags
    LFS3_TAG_MNAME          = 0x0220,

    // struct tags
    LFS3_TAG_STRUCT         = 0x0300,
    LFS3_TAG_BRANCH         = 0x0300,
    LFS3_TAG_DATA           = 0x0304,
    LFS3_TAG_BLOCK          = 0x0308,
    LFS3_TAG_DID            = 0x0314,
    LFS3_TAG_BSHRUB         = 0x0318,
    LFS3_TAG_BTREE          = 0x031c,
    LFS3_TAG_MROOT          = 0x0321,
    LFS3_TAG_MDIR           = 0x0325,
    LFS3_TAG_MTREE          = 0x032c,

    // user/sys attributes
    LFS3_TAG_ATTR           = 0x0400,
    LFS3_TAG_UATTR          = 0x0400,
    LFS3_TAG_SATTR          = 0x0500,

    // shrub tags belong to secondary trees
    LFS3_TAG_SHRUB          = 0x1000,

    // alt pointers form the inner nodes of our rbyd trees
    LFS3_TAG_ALT            = 0x4000,
    LFS3_TAG_B              = 0x0000,
    LFS3_TAG_R              = 0x2000,
    LFS3_TAG_LE             = 0x0000,
    LFS3_TAG_GT             = 0x1000,

    // checksum tags
    LFS3_TAG_CKSUM          = 0x3000,
    LFS3_TAG_PHASE          = 0x0003,
    LFS3_TAG_PERTURB        = 0x0004,
    LFS3_TAG_NOTE           = 0x3100,
    LFS3_TAG_ECKSUM         = 0x3200,
    LFS3_TAG_GCKSUMDELTA    = 0x3300,

    // in-device only tags, these should never get written to disk
    LFS3_TAG_INTERNAL       = 0x0800,
    LFS3_TAG_RATTRS         = 0x0800,
    LFS3_TAG_SHRUBCOMMIT    = 0x0801,
    LFS3_TAG_GRMPUSH        = 0x0802,
    LFS3_TAG_MOVE           = 0x0803,
    LFS3_TAG_ATTRS          = 0x0804,

    // some in-device only tag modifiers
    LFS3_TAG_RM             = 0x8000,
    LFS3_TAG_GROW           = 0x4000,
    LFS3_TAG_MASK0          = 0x0000,
    LFS3_TAG_MASK2          = 0x1000,
    LFS3_TAG_MASK8          = 0x2000,
    LFS3_TAG_MASK12         = 0x3000,
};

// some other tag encodings with their own subfields
#define LFS3_TAG_ALT(c, d, key) \
    (LFS3_TAG_ALT \
        | (0x2000 & (c)) \
        | (0x1000 & (d)) \
        | (0x0fff & (lfs3_tag_t)(key)))

#define LFS3_TAG_ATTR(attr) \
    (LFS3_TAG_ATTR \
        | ((0x80 & (lfs3_tag_t)(attr)) << 1) \
        | (0x7f & (lfs3_tag_t)(attr)))

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

static inline bool lfs3_tag_isinternal(lfs3_tag_t tag) {
    return tag & LFS3_TAG_INTERNAL;
}

static inline bool lfs3_tag_isrm(lfs3_tag_t tag) {
    return tag & LFS3_TAG_RM;
}

static inline bool lfs3_tag_isgrow(lfs3_tag_t tag) {
    return tag & LFS3_TAG_GROW;
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
    return 0x0fff & (-1U << ((0xc820 >> (4*((tag >> 12) & 0x3))) & 0xf));
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
    LFS3_ASSERT((lfs3_srid_t)weight >= 0);
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

// tag encoding:
// .---+---+---+- -+- -+- -+- -+---+- -+- -+- -.  tag:    1 be16    2 bytes
// |  tag  | weight            | size          |  weight: 1 leb128  <=5 bytes
// '---+---+---+- -+- -+- -+- -+---+- -+- -+- -'  size:   1 leb128  <=4 bytes
//                                                total:            <=11 bytes
#define LFS3_TAG_DSIZE (2+5+4)

// needed in lfs3_bd_readtag
#ifdef LFS3_CKMETAPARITY
static inline bool lfs3_m_isckparity(uint32_t flags);
#endif

static lfs3_ssize_t lfs3_bd_readtag(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_size_t hint,
        lfs3_tag_t *tag_, lfs3_rid_t *weight_, lfs3_size_t *size_,
        uint32_t *cksum) {
    // read the largest possible tag size
    uint8_t tag_buf[LFS3_TAG_DSIZE];
    lfs3_size_t tag_dsize = lfs3_min(LFS3_TAG_DSIZE, lfs3->cfg->block_size-off);
    if (tag_dsize < 4) {
        return LFS3_ERR_CORRUPT;
    }

    int err = lfs3_bd_read(lfs3, block, off, hint,
            tag_buf, tag_dsize);
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
                    size,
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
                    &p, 1);
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
        lfs3_block_t block, lfs3_size_t off, bool perturb,
        lfs3_tag_t tag, lfs3_rid_t weight, lfs3_size_t size,
        uint32_t *cksum, bool align) {
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
    bool v = lfs3_parity(*cksum) ^ perturb;
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

    int err = lfs3_bd_prog(lfs3, block, off, tag_buf, d,
            cksum, align);
    if (err) {
        return err;
    }

    return d;
}
#endif


/// lfs3_data_t stuff ///

#define LFS3_DATA_ONDISK 0x80000000
#define LFS3_DATA_ISBPTR 0x40000000

#ifdef LFS3_CKDATACKSUMREADS
#define LFS3_DATA_ISERASED 0x80000000
#endif

#define LFS3_DATA_NULL() \
    ((lfs3_data_t){ \
        .size=0, \
        .u.buffer=NULL})

#define LFS3_DATA_BUF(_buffer, _size) \
    ((lfs3_data_t){ \
        .size=_size, \
        .u.buffer=(const void*)(_buffer)})

#define LFS3_DATA_DISK(_block, _off, _size) \
    ((lfs3_data_t){ \
        .size=LFS3_DATA_ONDISK | (_size), \
        .u.disk.block=_block, \
        .u.disk.off=_off})

// data helpers
static inline bool lfs3_data_ondisk(lfs3_data_t data) {
    return data.size & LFS3_DATA_ONDISK;
}

static inline bool lfs3_data_isbuf(lfs3_data_t data) {
    return !(data.size & LFS3_DATA_ONDISK);
}

static inline bool lfs3_data_isbptr(lfs3_data_t data) {
    return data.size & LFS3_DATA_ISBPTR;
}

static inline lfs3_size_t lfs3_data_size(lfs3_data_t data) {
    return data.size & ~LFS3_DATA_ONDISK & ~LFS3_DATA_ISBPTR;
}

#ifdef LFS3_CKDATACKSUMREADS
static inline lfs3_size_t lfs3_data_cksize(lfs3_data_t data) {
    return data.u.disk.cksize & ~LFS3_DATA_ISERASED;
}
#endif

#ifdef LFS3_CKDATACKSUMREADS
static inline uint32_t lfs3_data_cksum(lfs3_data_t data) {
    return data.u.disk.cksum;
}
#endif

// data slicing
#define LFS3_DATA_SLICE(_data, _off, _size) \
    ((struct {lfs3_data_t d;}){lfs3_data_fromslice(_data, _off, _size)}.d)

LFS3_FORCEINLINE
static inline lfs3_data_t lfs3_data_fromslice(lfs3_data_t data,
        lfs3_ssize_t off, lfs3_ssize_t size) {
    // limit our off/size to data range, note the use of unsigned casts
    // here to treat -1 as unbounded
    lfs3_size_t off_ = lfs3_min(
            lfs3_smax(off, 0),
            lfs3_data_size(data));
    lfs3_size_t size_ = lfs3_min(
            (lfs3_size_t)size,
            lfs3_data_size(data) - off_);

    // on-disk?
    if (lfs3_data_ondisk(data)) {
        data.u.disk.off += off_;
        data.size -= lfs3_data_size(data) - size_;

    // buffer?
    } else {
        data.u.buffer += off_;
        data.size -= lfs3_data_size(data) - size_;
    }

    return data;
}

#define LFS3_DATA_TRUNCATE(_data, _size) \
    ((struct {lfs3_data_t d;}){lfs3_data_fromtruncate(_data, _size)}.d)

LFS3_FORCEINLINE
static inline lfs3_data_t lfs3_data_fromtruncate(lfs3_data_t data,
        lfs3_size_t size) {
    return LFS3_DATA_SLICE(data, -1, size);
}

#define LFS3_DATA_FRUNCATE(_data, _size) \
    ((struct {lfs3_data_t d;}){lfs3_data_fromfruncate(_data, _size)}.d)

LFS3_FORCEINLINE
static inline lfs3_data_t lfs3_data_fromfruncate(lfs3_data_t data,
        lfs3_size_t size) {
    return LFS3_DATA_SLICE(data,
            lfs3_data_size(data) - lfs3_min(
                size,
                lfs3_data_size(data)),
            -1);
}


// data <-> bd interactions

// lfs3_data_read* operations update the lfs3_data_t, effectively
// consuming the data

// needed in lfs3_data_read and friends
#ifdef LFS3_CKDATACKSUMREADS
static inline bool lfs3_m_isckdatacksums(uint32_t flags);
#endif

static lfs3_ssize_t lfs3_data_read(lfs3_t *lfs3, lfs3_data_t *data,
        void *buffer, lfs3_size_t size) {
    // limit our size to data range
    lfs3_size_t d = lfs3_min(size, lfs3_data_size(*data));

    // on-disk?
    if (lfs3_data_ondisk(*data)) {
        // validating data cksums?
        if (LFS3_IFDEF_CKDATACKSUMREADS(
                lfs3_m_isckdatacksums(lfs3->flags)
                    && lfs3_data_isbptr(*data),
                false)) {
            #ifdef LFS3_CKDATACKSUMREADS
            int err = lfs3_bd_readck(lfs3,
                    data->u.disk.block, data->u.disk.off,
                    // note our hint includes the full data range
                    lfs3_data_size(*data),
                    buffer, d,
                    lfs3_data_cksize(*data), lfs3_data_cksum(*data));
            if (err) {
                return err;
            }
            #endif

        } else {
            int err = lfs3_bd_read(lfs3,
                    data->u.disk.block, data->u.disk.off,
                    // note our hint includes the full data range
                    lfs3_data_size(*data),
                    buffer, d);
            if (err) {
                return err;
            }
        }

    // buffer?
    } else {
        lfs3_memcpy(buffer, data->u.buffer, d);
    }

    *data = LFS3_DATA_SLICE(*data, d, -1);
    return d;
}

static int lfs3_data_readle32(lfs3_t *lfs3, lfs3_data_t *data,
        uint32_t *word) {
    uint8_t buf[4];
    lfs3_ssize_t d = lfs3_data_read(lfs3, data, buf, 4);
    if (d < 0) {
        return d;
    }

    // truncated?
    if (d < 4) {
        return LFS3_ERR_CORRUPT;
    }

    *word = lfs3_fromle32(buf);
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

    *data = LFS3_DATA_SLICE(*data, d, -1);
    return 0;
}

// a little-leb128 in our system is truncated to align nicely
//
// for 32-bit words, little-leb128s are truncated to 28-bits, so the
// resulting leb128 encoding fits nicely in 4-bytes
static inline int lfs3_data_readlleb128(lfs3_t *lfs3, lfs3_data_t *data,
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

static lfs3_scmp_t lfs3_data_cmp(lfs3_t *lfs3, lfs3_data_t data,
        const void *buffer, lfs3_size_t size) {
    // compare common prefix
    lfs3_size_t d = lfs3_min(size, lfs3_data_size(data));

    // on-disk?
    if (lfs3_data_ondisk(data)) {
        // validating data cksums?
        if (LFS3_IFDEF_CKDATACKSUMREADS(
                lfs3_m_isckdatacksums(lfs3->flags)
                    && lfs3_data_isbptr(data),
                false)) {
            #ifdef LFS3_CKDATACKSUMREADS
            int cmp = lfs3_bd_cmpck(lfs3,
                    // note the 0 hint, we don't usually use any
                    // following data
                    data.u.disk.block, data.u.disk.off, 0,
                    buffer, d,
                    lfs3_data_cksize(data), lfs3_data_cksum(data));
            if (cmp != LFS3_CMP_EQ) {
                return cmp;
            }
            #endif

        } else {
            int cmp = lfs3_bd_cmp(lfs3,
                    // note the 0 hint, we don't usually use any
                    // following data
                    data.u.disk.block, data.u.disk.off, 0,
                    buffer, d);
            if (cmp != LFS3_CMP_EQ) {
                return cmp;
            }
        }

    // buffer?
    } else {
        int cmp = lfs3_memcmp(data.u.buffer, buffer, d);
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

static lfs3_scmp_t lfs3_data_namecmp(lfs3_t *lfs3, lfs3_data_t data,
        lfs3_did_t did, const char *name, lfs3_size_t name_len) {
    // first compare the did
    lfs3_did_t did_;
    int err = lfs3_data_readleb128(lfs3, &data, &did_);
    if (err) {
        return err;
    }

    if (did_ < did) {
        return LFS3_CMP_LT;
    } else if (did_ > did) {
        return LFS3_CMP_GT;
    }

    // then compare the actual name
    return lfs3_data_cmp(lfs3, data, name, name_len);
}

#ifndef LFS3_RDONLY
static int lfs3_bd_progdata(lfs3_t *lfs3,
        lfs3_block_t block, lfs3_size_t off, lfs3_data_t data,
        uint32_t *cksum, bool align) {
    // on-disk?
    if (lfs3_data_ondisk(data)) {
        // validating data cksums?
        if (LFS3_IFDEF_CKDATACKSUMREADS(
                lfs3_m_isckdatacksums(lfs3->flags)
                    && lfs3_data_isbptr(data),
                false)) {
            #ifdef LFS3_CKDATACKSUMREADS
            int err = lfs3_bd_cpyck(lfs3, block, off,
                    data.u.disk.block, data.u.disk.off, lfs3_data_size(data),
                    lfs3_data_size(data),
                    lfs3_data_cksize(data), lfs3_data_cksum(data),
                    cksum, align);
            if (err) {
                return err;
            }
            #endif

        } else {
            int err = lfs3_bd_cpy(lfs3, block, off,
                    data.u.disk.block, data.u.disk.off, lfs3_data_size(data),
                    lfs3_data_size(data),
                    cksum, align);
            if (err) {
                return err;
            }
        }

    // buffer?
    } else {
        int err = lfs3_bd_prog(lfs3, block, off,
                data.u.buffer, data.size,
                cksum, align);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif


// macros for le32/leb128/lleb128 encoding, these are useful for
// building rattrs

// le32 encoding:
// .---+---+---+---.  total: 1 le32  4 bytes
// |     le32      |
// '---+---+---+---'
//
#define LFS3_LE32_DSIZE 4

#ifndef LFS3_RDONLY
static inline lfs3_data_t lfs3_data_fromle32(uint32_t word,
        uint8_t buffer[static LFS3_LE32_DSIZE]) {
    lfs3_tole32(word, buffer);
    return LFS3_DATA_BUF(buffer, LFS3_LE32_DSIZE);
}
#endif

// leb128 encoding:
// .---+- -+- -+- -+- -.  total: 1 leb128  <=5 bytes
// |      leb128       |
// '---+- -+- -+- -+- -'
//
#define LFS3_LEB128_DSIZE 5

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

// lleb128 encoding:
// .---+- -+- -+- -.  total: 1 leb128  <=4 bytes
// |    lleb128    |
// '---+- -+- -+- -'
//
#define LFS3_LLEB128_DSIZE 4

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


// rattr layouts/lazy encoders
enum lfs3_from {
    LFS3_FROM_BUF       = 0,
    LFS3_FROM_DATA      = 1,

    LFS3_FROM_LE32      = 2,
    LFS3_FROM_LEB128    = 3,
    LFS3_FROM_NAME      = 4,

    LFS3_FROM_ECKSUM    = 5,
    LFS3_FROM_BPTR      = 6,
    LFS3_FROM_BTREE     = 7,
    LFS3_FROM_SHRUB     = 8,
    LFS3_FROM_MPTR      = 9,
    LFS3_FROM_GEOMETRY  = 10,
};

// we need to at least define DSIZE/DATA macros here

// ecksum encoding:
// .---+- -+- -+- -.  cksize: 1 leb128  <=4 bytes
// | cksize        |  cksum:  1 le32    4 bytes
// +---+- -+- -+- -+  total:            <=8 bytes
// |     cksum     |
// '---+---+---+---'
//
#define LFS3_ECKSUM_DSIZE (4+4)

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
#define LFS3_BPTR_DSIZE (4+5+4+4+4)

// branch encoding:
// .---+- -+- -+- -+- -.  block: 1 leb128  <=5 bytes
// | block             |  trunk: 1 leb128  <=4 bytes
// +---+- -+- -+- -+- -'  cksum: 1 le32    4 bytes
// | trunk         |      total:           <=13 bytes
// +---+- -+- -+- -+
// |     cksum     |
// '---+---+---+---'
//
#define LFS3_BRANCH_DSIZE (5+4+4)

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
#define LFS3_BTREE_DSIZE (5+LFS3_BRANCH_DSIZE)

// shrub encoding:
// .---+- -+- -+- -+- -.  weight: 1 leb128  <=5 bytes
// | weight            |  trunk:  1 leb128  <=4 bytes
// +---+- -+- -+- -+- -'  total:            <=9 bytes
// | trunk         |
// '---+- -+- -+- -'
//
#define LFS3_SHRUB_DSIZE (5+4)

// mptr encoding:
// .---+- -+- -+- -+- -.  blocks: 2 leb128s  <=2x5 bytes
// | block x 2         |  total:             <=10 bytes
// +                   +
// |                   |
// '---+- -+- -+- -+- -'
//
#define LFS3_MPTR_DSIZE (5+5)

// geometry encoding
// .---+- -+- -+- -.      block_size:  1 leb128  <=4 bytes
// | block_size    |      block_count: 1 leb128  <=5 bytes
// +---+- -+- -+- -+- -.  total:                 <=9 bytes
// | block_count       |
// '---+- -+- -+- -+- -'
#define LFS3_GEOMETRY_DSIZE (4+5)


// operations on attribute lists

// our core attribute type
#ifndef LFS3_RDONLY
typedef struct lfs3_rattr {
    lfs3_tag_t tag;
    uint8_t from;
    uint8_t count;
    lfs3_srid_t weight;
    union {
        const uint8_t *buffer;
        const lfs3_data_t *datas;
        uint32_t le32;
        uint32_t leb128;
        uint32_t lleb128;
        const void *etc;
    } u;
} lfs3_rattr_t;
#endif

// low-level attr macro
#define LFS3_RATTR_(_tag, _weight, _rattr) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=(_rattr).from, \
        .count=(_rattr).count, \
        .weight=_weight, \
        .u=(_rattr).u})

// high-level attr macros
#define LFS3_RATTR(_tag, _weight) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_BUF, \
        .count=0, \
        .weight=_weight, \
        .u.datas=NULL})

#define LFS3_RATTR_BUF(_tag, _weight, _buffer, _size) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_BUF, \
        .count=_size, \
        .weight=_weight, \
        .u.buffer=(const void*)(_buffer)})

#define LFS3_RATTR_DATA(_tag, _weight, _data) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_DATA, \
        .count=1, \
        .weight=_weight, \
        .u.datas=_data})

#define LFS3_RATTR_CAT_(_tag, _weight, _datas, _data_count) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_DATA, \
        .count=_data_count, \
        .weight=_weight, \
        .u.datas=_datas})

#define LFS3_RATTR_CAT(_tag, _weight, ...) \
    LFS3_RATTR_CAT_( \
        _tag, \
        _weight, \
        ((const lfs3_data_t[]){__VA_ARGS__}), \
        sizeof((const lfs3_data_t[]){__VA_ARGS__}) / sizeof(lfs3_data_t))

#define LFS3_RATTR_NOOP() \
    ((lfs3_rattr_t){ \
        .tag=LFS3_TAG_NULL, \
        .from=LFS3_FROM_BUF, \
        .count=0, \
        .weight=0, \
        .u.buffer=NULL})

// as convenience we lazily encode single le32/leb128/lleb128 attrs
//
// this also avoids needing a stack allocation for these attrs
#define LFS3_RATTR_LE32(_tag, _weight, _le32) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_LE32, \
        .count=0, \
        .weight=_weight, \
        .u.le32=_le32})

#define LFS3_RATTR_LEB128(_tag, _weight, _leb128) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_LEB128, \
        .count=0, \
        .weight=_weight, \
        .u.leb128=_leb128})

#define LFS3_RATTR_LLEB128(_tag, _weight, _lleb128) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_LEB128, \
        .count=0, \
        .weight=_weight, \
        .u.lleb128=_lleb128})

// helper macro for did + name pairs
#ifndef LFS3_RDONLY
typedef struct lfs3_name {
    uint32_t did;
    const char *name;
    lfs3_size_t name_len;
} lfs3_name_t;
#endif

#define LFS3_RATTR_NAME_(_tag, _weight, _name) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_NAME, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfs3_name_t*){_name}})

#define LFS3_RATTR_NAME(_tag, _weight, _did, _name, _name_len) \
    LFS3_RATTR_NAME_( \
        _tag, \
        _weight, \
        (&(const lfs3_name_t){ \
            .did=_did, \
            .name=_name, \
            .name_len=_name_len}))

// macros for other lazily encoded attrs
#define LFS3_RATTR_ECKSUM(_tag, _weight, _ecksum) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_ECKSUM, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfs3_ecksum_t*){_ecksum}})

// note the LFS3_BPTR_DSIZE hint so shrub estimates work
#define LFS3_RATTR_BPTR(_tag, _weight, _bptr) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_BPTR, \
        .count=LFS3_BPTR_DSIZE, \
        .weight=_weight, \
        .u.etc=(const lfs3_bptr_t*){_bptr}})

#define LFS3_RATTR_BTREE(_tag, _weight, _btree) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_BTREE, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfs3_btree_t*){_btree}})

#define LFS3_RATTR_SHRUB(_tag, _weight, _shrub) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_SHRUB, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfs3_shrub_t*){_shrub}})

#define LFS3_RATTR_MPTR(_tag, _weight, _mptr) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_MPTR, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfs3_block_t*){_mptr}})

#define LFS3_RATTR_GEOMETRY(_tag, _weight, _geometry) \
    ((lfs3_rattr_t){ \
        .tag=_tag, \
        .from=LFS3_FROM_GEOMETRY, \
        .count=0, \
        .weight=_weight, \
        .u.etc=(const lfs3_geometry_t*){_geometry}})

// these are special attrs that trigger unique behavior in
// lfs3_mdir_commit__
#define LFS3_RATTR_RATTRS(_rattrs, _rattr_count) \
    ((lfs3_rattr_t){ \
        .tag=LFS3_TAG_RATTRS, \
        .from=LFS3_FROM_BUF, \
        .count=_rattr_count, \
        .weight=0, \
        .u.etc=(const lfs3_rattr_t*){_rattrs}})

#define LFS3_RATTR_SHRUBCOMMIT(_shrubcommit) \
    ((lfs3_rattr_t){ \
        .tag=LFS3_TAG_SHRUBCOMMIT, \
        .from=LFS3_FROM_BUF, \
        .count=0, \
        .weight=0, \
        .u.etc=(const lfs3_shrubcommit_t*){_shrubcommit}})

#define LFS3_RATTR_MOVE(_move) \
    ((lfs3_rattr_t){ \
        .tag=LFS3_TAG_MOVE, \
        .from=LFS3_FROM_BUF, \
        .count=0, \
        .weight=0, \
        .u.etc=(const lfs3_mdir_t*){_move}})

#define LFS3_RATTR_ATTRS(_attrs, _attr_count) \
    ((lfs3_rattr_t){ \
        .tag=LFS3_TAG_ATTRS, \
        .from=LFS3_FROM_BUF, \
        .count=_attr_count, \
        .weight=0, \
        .u.etc=(const struct lfs3_attr*){_attrs}})

// create an attribute list
#define LFS3_RATTRS(...) \
    (const lfs3_rattr_t[]){__VA_ARGS__}, \
    sizeof((const lfs3_rattr_t[]){__VA_ARGS__}) / sizeof(lfs3_rattr_t)

// rattr helpers
#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isnoop(lfs3_rattr_t rattr) {
    // noop rattrs must have zero weight
    LFS3_ASSERT(rattr.tag || rattr.weight == 0);
    return !rattr.tag;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rattr_isinsert(lfs3_rattr_t rattr) {
    return !lfs3_tag_isgrow(rattr.tag) && rattr.weight > 0;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_srid_t lfs3_rattr_nextrid(lfs3_rattr_t rattr,
        lfs3_srid_t rid) {
    if (lfs3_rattr_isinsert(rattr)) {
        return rid + rattr.weight-1;
    } else {
        return rid + rattr.weight;
    }
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
            return lfs3_data_cmp(lfs3, *data,
                    attr->buffer,
                    lfs3_attr_size(attr));
        }
    }
}


// operations on erased-state checksums

// erased-state checksum
#ifndef LFS3_RDONLY
typedef struct lfs3_ecksum {
    // cksize=-1 indicates no ecksum
    lfs3_ssize_t cksize;
    uint32_t cksum;
} lfs3_ecksum_t;
#endif

// erased-state checksum on-disk encoding
#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_fromecksum(const lfs3_ecksum_t *ecksum,
        uint8_t buffer[static LFS3_ECKSUM_DSIZE]) {
    // you shouldn't try to encode a not-ecksum, that doesn't make sense
    LFS3_ASSERT(ecksum->cksize != -1);
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



/// block pointer things ///

#define LFS3_BPTR_ONDISK LFS3_DATA_ONDISK
#define LFS3_BPTR_ISBPTR LFS3_DATA_ISBPTR

#ifndef LFS3_RDONLY
#define LFS3_BPTR_ISERASED 0x80000000
#endif

#ifndef LFS3_2BONLY
static void lfs3_bptr_init(lfs3_bptr_t *bptr,
        lfs3_data_t data, lfs3_size_t cksize, uint32_t cksum) {
    // make sure the bptr flag is set
    LFS3_ASSERT(lfs3_data_ondisk(data));
    bptr->d.size = data.size | LFS3_DATA_ONDISK | LFS3_BPTR_ISBPTR;
    bptr->d.u.disk.block = data.u.disk.block;
    bptr->d.u.disk.off = data.u.disk.off;
    #ifdef LFS3_CKDATACKSUMREADS
    bptr->d.u.disk.cksize = cksize;
    bptr->d.u.disk.cksum = cksum;
    #else
    bptr->cksize = cksize;
    bptr->cksum = cksum;
    #endif
}
#endif

static inline void lfs3_bptr_discard(lfs3_bptr_t *bptr) {
    bptr->d = LFS3_DATA_NULL();
    #if !defined(LFS3_2BONLY) && !defined(LFS3_CKDATACKSUMREADS)
    bptr->cksize = 0;
    bptr->cksum = 0;
    #endif
}

#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static inline void lfs3_bptr_claim(lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMREADS
    bptr->d.u.disk.cksize &= ~LFS3_BPTR_ISERASED;
    #else
    bptr->cksize &= ~LFS3_BPTR_ISERASED;
    #endif
}
#endif

static inline bool lfs3_bptr_isbptr(const lfs3_bptr_t *bptr) {
    return bptr->d.size & LFS3_BPTR_ISBPTR;
}

static inline lfs3_block_t lfs3_bptr_block(const lfs3_bptr_t *bptr) {
    return bptr->d.u.disk.block;
}

static inline lfs3_size_t lfs3_bptr_off(const lfs3_bptr_t *bptr) {
    return bptr->d.u.disk.off;
}

static inline lfs3_size_t lfs3_bptr_size(const lfs3_bptr_t *bptr) {
    return bptr->d.size & ~LFS3_BPTR_ONDISK & ~LFS3_BPTR_ISBPTR;
}

// checked reads adds ck info to lfs3_data_t that we don't want to
// unnecessarily duplicate, this makes accessing ck info annoyingly
// messy...
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static inline bool lfs3_bptr_iserased(const lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMREADS
    return bptr->d.u.disk.cksize & LFS3_BPTR_ISERASED;
    #else
    return bptr->cksize & LFS3_BPTR_ISERASED;
    #endif
}
#endif

#ifndef LFS3_2BONLY
static inline lfs3_size_t lfs3_bptr_cksize(const lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMREADS
    return LFS3_IFDEF_RDONLY(
            bptr->d.u.disk.cksize,
            bptr->d.u.disk.cksize & ~LFS3_BPTR_ISERASED);
    #else
    return LFS3_IFDEF_RDONLY(
            bptr->cksize,
            bptr->cksize & ~LFS3_BPTR_ISERASED);
    #endif
}
#endif

#ifndef LFS3_2BONLY
static inline uint32_t lfs3_bptr_cksum(const lfs3_bptr_t *bptr) {
    #ifdef LFS3_CKDATACKSUMREADS
    return bptr->d.u.disk.cksum;
    #else
    return bptr->cksum;
    #endif
}
#endif

// bptr on-disk encoding
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static lfs3_data_t lfs3_data_frombptr(const lfs3_bptr_t *bptr,
        uint8_t buffer[static LFS3_BPTR_DSIZE]) {
    // size should not exceed 28-bits
    LFS3_ASSERT(lfs3_data_size(bptr->d) <= 0x0fffffff);
    // block should not exceed 31-bits
    LFS3_ASSERT(lfs3_bptr_block(bptr) <= 0x7fffffff);
    // off should not exceed 28-bits
    LFS3_ASSERT(lfs3_bptr_off(bptr) <= 0x0fffffff);
    // cksize should not exceed 28-bits
    LFS3_ASSERT(lfs3_bptr_cksize(bptr) <= 0x0fffffff);
    lfs3_ssize_t d = 0;

    // write the block, offset, size
    lfs3_ssize_t d_ = lfs3_toleb128(lfs3_data_size(bptr->d), &buffer[d], 4);
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

#ifndef LFS3_2BONLY
static int lfs3_data_readbptr(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_bptr_t *bptr) {
    // read the block, offset, size
    int err = lfs3_data_readlleb128(lfs3, data, &bptr->d.size);
    if (err) {
        return err;
    }

    err = lfs3_data_readleb128(lfs3, data, &bptr->d.u.disk.block);
    if (err) {
        return err;
    }

    err = lfs3_data_readlleb128(lfs3, data, &bptr->d.u.disk.off);
    if (err) {
        return err;
    }

    // read the cksize, cksum
    err = lfs3_data_readlleb128(lfs3, data,
            LFS3_IFDEF_CKDATACKSUMREADS(
                &bptr->d.u.disk.cksize,
                &bptr->cksize));
    if (err) {
        return err;
    }

    err = lfs3_data_readle32(lfs3, data,
            LFS3_IFDEF_CKDATACKSUMREADS(
                &bptr->d.u.disk.cksum,
                &bptr->cksum));
    if (err) {
        return err;
    }

    // mark as on-disk + cksum
    bptr->d.size |= LFS3_DATA_ONDISK | LFS3_DATA_ISBPTR;
    return 0;
}
#endif

// needed in lfs3_bptr_fetch
#ifdef LFS3_CKFETCHES
static inline bool lfs3_m_isckfetches(uint32_t flags);
#endif
static int lfs3_bptr_ck(lfs3_t *lfs3, const lfs3_bptr_t *bptr);

// fetch a bptr or data fragment
static int lfs3_bptr_fetch(lfs3_t *lfs3, lfs3_bptr_t *bptr,
        lfs3_tag_t tag, lfs3_bid_t weight, lfs3_data_t data) {
    // fragment? (inlined data)
    if (tag == LFS3_TAG_DATA) {
        bptr->d = data;

    // bptr?
    } else if (LFS3_IFDEF_2BONLY(false, tag == LFS3_TAG_BLOCK)) {
        #ifndef LFS3_2BONLY
        int err = lfs3_data_readbptr(lfs3, &data,
                bptr);
        if (err) {
            return err;
        }
        #endif

    } else {
        LFS3_UNREACHABLE();
    }

    // limit bptrs to btree weights, this may be useful for
    // compression in the future
    bptr->d = LFS3_DATA_TRUNCATE(bptr->d, weight);

    // checking fetches?
    #ifdef LFS3_CKFETCHES
    if (lfs3_m_isckfetches(lfs3->flags)
            && lfs3_bptr_isbptr(bptr)) {
        int err = lfs3_bptr_ck(lfs3, bptr);
        if (err) {
            return err;
        }
    }
    #endif

    return 0;
}

// check the contents of a bptr
#ifndef LFS3_2BONLY
static int lfs3_bptr_ck(lfs3_t *lfs3, const lfs3_bptr_t *bptr) {
    uint32_t cksum = 0;
    int err = lfs3_bd_cksum(lfs3,
            lfs3_bptr_block(bptr), 0, 0,
            lfs3_bptr_cksize(bptr),
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
#endif




/// Red-black-yellow Dhara tree operations ///

#define LFS3_RBYD_ISSHRUB 0x80000000
#define LFS3_RBYD_ISPERTURB 0x80000000

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
    rbyd->eoff = -1;
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
    return rbyd->eoff & LFS3_RBYD_ISPERTURB;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_size_t lfs3_rbyd_eoff(const lfs3_rbyd_t *rbyd) {
    return rbyd->eoff & ~LFS3_RBYD_ISPERTURB;
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


// needed in lfs3_rbyd_alloc
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static lfs3_sblock_t lfs3_alloc(lfs3_t *lfs3, bool erase);
#endif

// allocate an rbyd block
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static int lfs3_rbyd_alloc(lfs3_t *lfs3, lfs3_rbyd_t *rbyd) {
    lfs3_sblock_t block = lfs3_alloc(lfs3, true);
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
            &e, 1);
    if (err) {
        return err;
    }

    if (((e >> 7)^lfs3_rbyd_isperturb(rbyd)) == lfs3_parity(rbyd->cksum)) {
        return LFS3_ERR_CORRUPT;
    }

    // check that erased-state matches our checksum, if this fails
    // most likely a write was interrupted
    uint32_t ecksum_ = 0;
    err = lfs3_bd_cksum(lfs3,
            rbyd->blocks[0], lfs3_rbyd_eoff(rbyd), 0,
            ecksum->cksize,
            &ecksum_);
    if (err) {
        return err;
    }

    // found erased-state?
    return (ecksum_ == ecksum->cksum) ? 0 : LFS3_ERR_CORRUPT;
}
#endif

// needed in lfs3_rbyd_fetch_ if debugging rbyd balance
static int lfs3_rbyd_lookupnext_(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_srid_t *rid_, lfs3_tag_t *tag_, lfs3_rid_t *weight_,
        lfs3_data_t *data_,
        lfs3_size_t *height_, lfs3_size_t *bheight_);

// fetch an rbyd
static int lfs3_rbyd_fetch_(lfs3_t *lfs3,
        lfs3_rbyd_t *rbyd, uint32_t *gcksumdelta,
        lfs3_block_t block, lfs3_size_t trunk) {
    // set up some initial state
    rbyd->blocks[0] = block;
    rbyd->trunk = (trunk & LFS3_RBYD_ISSHRUB) | 0;
    rbyd->weight = 0;
    #ifndef LFS3_RDONLY
    rbyd->eoff = 0;
    #endif

    // ignore the shrub bit here
    trunk &= ~LFS3_RBYD_ISSHRUB;

    // keep track of last commit off and perturb bit
    lfs3_size_t eoff = 0;
    bool perturb = false;

    // checksum the revision count to get the cksum started
    uint32_t cksum_ = 0;
    int err = lfs3_bd_cksum(lfs3, block, 0, -1, sizeof(uint32_t),
            &cksum_);
    if (err) {
        return err;
    }

    // temporary state until we validate a cksum
    lfs3_size_t off_ = sizeof(uint32_t);
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
    uint32_t gcksumdelta_ = 0;

    // scan tags, checking valid bits, cksums, etc
    while (off_ < lfs3->cfg->block_size
            && (!trunk || eoff <= trunk)) {
        // read next tag
        lfs3_tag_t tag;
        lfs3_rid_t weight;
        lfs3_size_t size;
        lfs3_ssize_t d = lfs3_bd_readtag(lfs3, block, off_, -1,
                &tag, &weight, &size,
                &cksum__);
        if (d < 0) {
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
                // cksum the entry, hopefully leaving it in the cache
                err = lfs3_bd_cksum(lfs3, block, off__, -1, size,
                        &cksum__);
                if (err) {
                    if (err == LFS3_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }

                // found an ecksum? save for later
                if (LFS3_IFDEF_RDONLY(
                        false,
                        tag == LFS3_TAG_ECKSUM)) {
                    #ifndef LFS3_RDONLY
                    err = lfs3_data_readecksum(lfs3,
                            &LFS3_DATA_DISK(block, off__,
                                // note this size is to make the hint do
                                // what we want
                                lfs3->cfg->block_size - off__),
                            &ecksum_);
                    if (err) {
                        if (err == LFS3_ERR_CORRUPT) {
                            break;
                        }
                        return err;
                    }
                    #endif

                // found gcksumdelta? save for later
                } else if (tag == LFS3_TAG_GCKSUMDELTA) {
                    err = lfs3_data_readle32(lfs3,
                            &LFS3_DATA_DISK(block, off__,
                                // note this size is to make the hint do
                                // what we want
                                lfs3->cfg->block_size - off__),
                            &gcksumdelta_);
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

                // check checksum
                uint32_t cksum___ = 0;
                err = lfs3_bd_read(lfs3, block, off__, -1,
                        &cksum___, sizeof(uint32_t));
                if (err) {
                    if (err == LFS3_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }
                cksum___ = lfs3_fromle32(&cksum___);

                if (cksum__ != cksum___) {
                    // uh oh, checksums don't match
                    break;
                }

                // save what we've found so far
                eoff = off__ + size;
                rbyd->trunk = (LFS3_RBYD_ISSHRUB & rbyd->trunk) | trunk_;
                rbyd->weight = weight_;
                rbyd->cksum = cksum_;
                if (gcksumdelta) {
                    *gcksumdelta = gcksumdelta_;
                }
                gcksumdelta_ = 0;

                // update perturb bit
                perturb = lfs3_tag_perturb(tag);

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
            if (!(trunk && off_ > trunk && !trunk__)) {
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
                    // update trunk and weight, unless we are a shrub trunk
                    if (!lfs3_tag_isshrub(tag) || trunk__ == trunk) {
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
    if (ecksum.cksize != -1) {
        // check the erased-state checksum
        err = lfs3_rbyd_ckecksum(lfs3, rbyd, &ecksum);
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

    #ifdef LFS3_DBGRBYDFETCHES
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
    #endif

    // debugging rbyd balance? check that all branches in the rbyd have
    // the same height
    #ifdef LFS3_DBGRBYDBALANCE
    lfs3_srid_t rid = -1;
    lfs3_tag_t tag = 0;
    lfs3_size_t min_height = 0;
    lfs3_size_t max_height = 0;
    lfs3_size_t min_bheight = 0;
    lfs3_size_t max_bheight = 0;
    while (true) {
        lfs3_size_t height;
        lfs3_size_t bheight;
        int err = lfs3_rbyd_lookupnext_(lfs3, rbyd,
                rid, tag+1,
                &rid, &tag, NULL, NULL,
                &height, &bheight);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
        }

        // find the min/max height and bheight
        min_height = (min_height) ? lfs3_min(min_height, height) : height;
        max_height = (max_height) ? lfs3_max(max_height, height) : height;
        min_bheight = (min_bheight) ? lfs3_min(min_bheight, bheight) : bheight;
        max_bheight = (max_bheight) ? lfs3_max(max_bheight, bheight) : bheight;
    }
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
    return lfs3_rbyd_fetch_(lfs3, rbyd, NULL, block, trunk);
}

// a more aggressive fetch when checksum is known
static int lfs3_rbyd_fetchck(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_block_t block, lfs3_size_t trunk,
        uint32_t cksum) {
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


// our core rbyd lookup algorithm
//
// finds the next rid+tag such that rid_+tag_ >= rid+tag
static int lfs3_rbyd_lookupnext_(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_srid_t *rid_, lfs3_tag_t *tag_, lfs3_rid_t *weight_,
        lfs3_data_t *data_,
        lfs3_size_t *height_, lfs3_size_t *bheight_) {
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
    if (height_) {
        *height_ = 0;
    }
    if (bheight_) {
        *bheight_ = 0;
    }

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
                rbyd->blocks[0], branch, 0,
                &alt, &weight, &jump,
                NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfs3_tag_isalt(alt)) {
            lfs3_size_t branch_ = branch + d;

            // keep track of height for debugging
            if (height_) {
                *height_ += 1;
            }
            if (bheight_
                    // only count black+followed alts towards bheight
                    && (lfs3_tag_isblack(alt)
                        || lfs3_tag_follow(
                            alt, weight,
                            lower_rid, upper_rid,
                            rid, tag))) {
                *bheight_ += 1;
            }

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
            if (tag_) {
                *tag_ = tag__;
            }
            if (weight_) {
                *weight_ = upper_rid - lower_rid;
            }
            if (data_) {
                *data_ = LFS3_DATA_DISK(rbyd->blocks[0], branch + d, jump);
            }
            return 0;
        }
    }
}


// finds the next rid_+tag_ such that rid_+tag_ >= rid+tag
static int lfs3_rbyd_lookupnext(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_srid_t *rid_, lfs3_tag_t *tag_, lfs3_rid_t *weight_,
        lfs3_data_t *data_) {
    return lfs3_rbyd_lookupnext_(lfs3, rbyd, rid, tag,
            rid_, tag_, weight_, data_,
            NULL, NULL);
}

// lookup assumes a known rid
static int lfs3_rbyd_lookup(lfs3_t *lfs3, const lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, lfs3_tag_t tag,
        lfs3_tag_t *tag_, lfs3_data_t *data_) {
    lfs3_srid_t rid__;
    lfs3_tag_t tag__;
    int err = lfs3_rbyd_lookupnext(lfs3, rbyd, rid, lfs3_tag_key(tag),
            &rid__, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if (rid__ != rid
            || (tag__ & lfs3_tag_mask(tag)) != (tag & lfs3_tag_mask(tag))) {
        return LFS3_ERR_NOENT;
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
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendrev(lfs3_t *lfs3, lfs3_rbyd_t *rbyd, uint32_t rev) {
    // should only be called before any tags are written
    LFS3_ASSERT(rbyd->eoff == 0);
    LFS3_ASSERT(rbyd->cksum == 0);

    // revision count stored as le32, we don't use a leb128 encoding as we
    // intentionally allow the revision count to overflow
    uint8_t rev_buf[sizeof(uint32_t)];
    lfs3_tole32(rev, &rev_buf);

    int err = lfs3_bd_prog(lfs3,
            rbyd->blocks[0], lfs3_rbyd_eoff(rbyd),
            &rev_buf, sizeof(uint32_t),
            &rbyd->cksum, false);
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
    LFS3_ASSERT(!lfs3_tag_isinternal(tag));
    // bit 7 is reserved for future subtype extensions
    LFS3_ASSERT(!(tag & 0x80));

    // do we fit?
    if (lfs3_rbyd_eoff(rbyd) + LFS3_TAG_DSIZE
            > lfs3->cfg->block_size) {
        return LFS3_ERR_RANGE;
    }

    lfs3_ssize_t d = lfs3_bd_progtag(lfs3,
            rbyd->blocks[0], lfs3_rbyd_eoff(rbyd), lfs3_rbyd_isperturb(rbyd),
            tag, weight, size,
            &rbyd->cksum, false);
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
static lfs3_data_t lfs3_data_frombptr(const lfs3_bptr_t *bptr,
        uint8_t buffer[static LFS3_BPTR_DSIZE]);
static lfs3_data_t lfs3_data_frombtree(const lfs3_btree_t *btree,
        uint8_t buffer[static LFS3_BTREE_DSIZE]);
static lfs3_data_t lfs3_data_fromshrub(const lfs3_shrub_t *shrub,
        uint8_t buffer[static LFS3_SHRUB_DSIZE]);
static lfs3_data_t lfs3_data_frommptr(const lfs3_block_t mptr[static 2],
        uint8_t buffer[static LFS3_MPTR_DSIZE]);
typedef struct lfs3_geometry lfs3_geometry_t;
static lfs3_data_t lfs3_data_fromgeometry(const lfs3_geometry_t *geometry,
        uint8_t buffer[static LFS3_GEOMETRY_DSIZE]);

// encode rattrs
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendrattr_(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_rattr_t rattr) {
    // tag must not be internal at this point
    LFS3_ASSERT(!lfs3_tag_isinternal(rattr.tag));
    // bit 7 is reserved for future subtype extensions
    LFS3_ASSERT(!(rattr.tag & 0x80));

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
                lfs3_data_t data;
                uint8_t buf[LFS3_LE32_DSIZE];
            } le32;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_LEB128_DSIZE];
            } leb128;
            struct {
                lfs3_data_t datas[2];
                uint8_t buf[LFS3_LEB128_DSIZE];
            } name;

            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_ECKSUM_DSIZE];
            } ecksum;
            struct {
                lfs3_data_t data;
                uint8_t buf[LFS3_BPTR_DSIZE];
            } bptr;
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
                uint8_t buf[LFS3_GEOMETRY_DSIZE];
            } geometry;
        } u;
    } ctx;

    switch (rattr.from) {
    // direct buffer?
    case LFS3_FROM_BUF:;
        ctx.u.data = LFS3_DATA_BUF(rattr.u.buffer, rattr.count);
        datas = &ctx.u.data;
        data_count = 1;
        break;

    // indirect concatenated data?
    case LFS3_FROM_DATA:;
        datas = rattr.u.datas;
        data_count = rattr.count;
        break;

    // le32?
    case LFS3_FROM_LE32:;
        ctx.u.le32.data = lfs3_data_fromle32(rattr.u.le32,
                ctx.u.le32.buf);
        datas = &ctx.u.le32.data;
        data_count = 1;
        break;

    // leb128?
    case LFS3_FROM_LEB128:;
        // leb128s should not exceed 31-bits
        LFS3_ASSERT(rattr.u.leb128 <= 0x7fffffff);
        // little-leb128s should not exceed 28-bits
        LFS3_ASSERT(rattr.tag != LFS3_TAG_NAMELIMIT
                || rattr.u.leb128 <= 0x0fffffff);
        ctx.u.leb128.data = lfs3_data_fromleb128(rattr.u.leb128,
                ctx.u.leb128.buf);
        datas = &ctx.u.leb128.data;
        data_count = 1;
        break;

    // name?
    case LFS3_FROM_NAME:;
        const lfs3_name_t *name = rattr.u.etc;
        ctx.u.name.datas[0] = lfs3_data_fromleb128(name->did, ctx.u.name.buf);
        ctx.u.name.datas[1] = LFS3_DATA_BUF(name->name, name->name_len);
        datas = ctx.u.name.datas;
        data_count = 2;
        break;

    // ecksum?
    case LFS3_FROM_ECKSUM:;
        ctx.u.ecksum.data = lfs3_data_fromecksum(rattr.u.etc,
                ctx.u.ecksum.buf);
        datas = &ctx.u.ecksum.data;
        data_count = 1;
        break;

    // bptr?
    #ifndef LFS3_2BONLY
    case LFS3_FROM_BPTR:;
        ctx.u.bptr.data = lfs3_data_frombptr(rattr.u.etc,
                ctx.u.bptr.buf);
        datas = &ctx.u.bptr.data;
        data_count = 1;
        break;
    #endif

    // btree?
    #ifndef LFS3_2BONLY
    case LFS3_FROM_BTREE:;
        ctx.u.btree.data = lfs3_data_frombtree(rattr.u.etc,
                ctx.u.btree.buf);
        datas = &ctx.u.btree.data;
        data_count = 1;
        break;
    #endif

    // shrub trunk?
    case LFS3_FROM_SHRUB:;
        // note unlike the other lazy tags, we _need_ to lazily encode
        // shrub trunks, since they change underneath us during mdir
        // compactions, relocations, etc
        ctx.u.shrub.data = lfs3_data_fromshrub(rattr.u.etc,
                ctx.u.shrub.buf);
        datas = &ctx.u.shrub.data;
        data_count = 1;
        break;

    // mptr?
    case LFS3_FROM_MPTR:;
        ctx.u.mptr.data = lfs3_data_frommptr(rattr.u.etc,
                ctx.u.mptr.buf);
        datas = &ctx.u.mptr.data;
        data_count = 1;
        break;

    // geometry?
    case LFS3_FROM_GEOMETRY:;
        ctx.u.geometry.data = lfs3_data_fromgeometry(rattr.u.etc,
                ctx.u.geometry.buf);
        datas = &ctx.u.geometry.data;
        data_count = 1;
        break;

    default:;
        LFS3_UNREACHABLE();
    }

    // now everything should be raw data, either in-ram or on-disk

    // find the concatenated size
    lfs3_size_t size = 0;
    for (lfs3_size_t i = 0; i < data_count; i++) {
        size += lfs3_data_size(datas[i]);
    }

    // do we fit?
    if (lfs3_rbyd_eoff(rbyd) + LFS3_TAG_DSIZE + size
            > lfs3->cfg->block_size) {
        return LFS3_ERR_RANGE;
    }

    // append tag
    int err = lfs3_rbyd_appendtag(lfs3, rbyd,
            rattr.tag, rattr.weight, size);
    if (err) {
        return err;
    }

    // append data
    for (lfs3_size_t i = 0; i < data_count; i++) {
        err = lfs3_bd_progdata(lfs3,
                rbyd->blocks[0], lfs3_rbyd_eoff(rbyd), datas[i],
                &rbyd->cksum, false);
        if (err) {
            return err;
        }

        rbyd->eoff += lfs3_data_size(datas[i]);
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
        int err = lfs3_rbyd_appendrev(lfs3, rbyd, 0);
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
        lfs3_srid_t rid, lfs3_rattr_t rattr) {
    // must fetch before mutating!
    LFS3_ASSERT(lfs3_rbyd_isfetched(rbyd));
    // tag must not be internal at this point
    LFS3_ASSERT(!lfs3_tag_isinternal(rattr.tag));
    // bit 7 is reserved for future subtype extensions
    LFS3_ASSERT(!(rattr.tag & 0x80));
    // you can't delete more than what's in the rbyd
    LFS3_ASSERT(rattr.weight >= -(lfs3_srid_t)rbyd->weight);

    // ignore noops
    if (lfs3_rattr_isnoop(rattr)) {
        return 0;
    }

    // begin appending
    int err = lfs3_rbyd_appendinit(lfs3, rbyd);
    if (err) {
        return err;
    }

    // figure out what range of tags we're operating on
    lfs3_srid_t a_rid;
    lfs3_srid_t b_rid;
    lfs3_tag_t a_tag;
    lfs3_tag_t b_tag;
    if (!lfs3_tag_isgrow(rattr.tag) && rattr.weight != 0) {
        if (rattr.weight > 0) {
            LFS3_ASSERT(rid <= (lfs3_srid_t)rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid -= 1;
            a_rid = rid + 1;
            b_rid = rid + 1;
        } else {
            LFS3_ASSERT(rid < (lfs3_srid_t)rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid += 1;
            a_rid = rid - lfs3_smax(-rattr.weight, 0);
            b_rid = rid;
        }

        a_tag = 0;
        b_tag = 0;

    } else {
        LFS3_ASSERT(rid < (lfs3_srid_t)rbyd->weight);

        a_rid = rid - lfs3_smax(-rattr.weight, 0);
        b_rid = rid;

        // note both normal and rm wide-tags have the same bounds, really it's
        // the normal non-wide-tags that are an outlier here
        if (lfs3_tag_ismask12(rattr.tag)) {
            a_tag = 0x000;
            b_tag = 0xfff;
        } else if (lfs3_tag_ismask8(rattr.tag)) {
            a_tag = (rattr.tag & 0xf00);
            b_tag = (rattr.tag & 0xf00) + 0x100;
        } else if (lfs3_tag_ismask2(rattr.tag)) {
            a_tag = (rattr.tag & 0xffc);
            b_tag = (rattr.tag & 0xffc) + 0x004;
        } else if (lfs3_tag_isrm(rattr.tag)) {
            a_tag = lfs3_tag_key(rattr.tag);
            b_tag = lfs3_tag_key(rattr.tag) + 1;
        } else {
            a_tag = lfs3_tag_key(rattr.tag);
            b_tag = lfs3_tag_key(rattr.tag);
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
        lfs3_tag_t alt;
        lfs3_rid_t weight;
        lfs3_size_t jump;
        lfs3_ssize_t d = lfs3_bd_readtag(lfs3,
                rbyd->blocks[0], branch, 0,
                &alt, &weight, &jump,
                NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfs3_tag_isalt(alt)) {
            // make jump absolute
            jump = branch - jump;
            lfs3_size_t branch_ = branch + d;

            // yellow alts should be parallel
            LFS3_ASSERT(!(lfs3_tag_isred(alt) && lfs3_tag_isred(p[0].alt))
                    || lfs3_tag_isparallel(alt, p[0].alt));

            // take black alt? needs a flip
            //   <b           >b
            // .-'|  =>     .-'|
            // 1  2      1  2  1
            if (lfs3_tag_follow2(
                    alt, weight,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    a_rid, a_tag)) {
                lfs3_tag_flip2(
                        &alt, &weight,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid);
                LFS3_SWAP(lfs3_size_t, &jump, &branch_);
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
                LFS3_SWAP(lfs3_tag_t, &p[0].alt, &alt);
                LFS3_SWAP(lfs3_rid_t, &p[0].weight, &weight);
                LFS3_SWAP(lfs3_size_t, &p[0].jump, &jump);
                alt = (alt & ~LFS3_TAG_R) | (p[0].alt & LFS3_TAG_R);
                p[0].alt |= LFS3_TAG_R;

                lfs3_tag_flip2(
                        &alt, &weight,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid);
                LFS3_SWAP(lfs3_size_t, &jump, &branch_);
            }

            // do bounds want to take different paths? begin diverging
            //                            >b                    <b
            //                          .-'|                  .-'|
            //         <b  =>           | nb  =>             nb  |
            //    .----'|      .--------|--'      .-----------'  |
            //   <b    <b      |       <b         |             nb
            // .-'|  .-'|      |     .-'|         |        .-----'
            // 1  2  3  4      1  2  3  4  x      1  2  3  4  x  x
            bool diverging_b = lfs3_tag_diverging2(
                    alt, weight,
                    p[0].alt, p[0].weight,
                    lower_rid, upper_rid,
                    a_rid, a_tag,
                    b_rid, b_tag);
            bool diverging_r = lfs3_tag_isred(p[0].alt)
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
                if (diverging_b && diverging_r) {
                    LFS3_ASSERT(a_rid < b_rid || a_tag < b_tag);
                    LFS3_ASSERT(lfs3_tag_isparallel(alt, p[0].alt));

                    weight += p[0].weight;
                    jump = p[0].jump;
                    lfs3_rbyd_p_pop(p);

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
                        && (lfs3_tag_isblack(alt)
                            // give up if we find a yellow alt
                            || (lfs3_tag_isred(p[0].alt)))) {
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
                        LFS3_ASSERT(!diverging_r);

                        alt = LFS3_TAG_ALT(
                            alt & LFS3_TAG_R,
                            LFS3_TAG_LE,
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
                    lfs3_tag_trim(
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
            if (lfs3_tag_isred(p[0].alt)
                    && lfs3_tag_unreachable(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)
                    && p[0].jump > branch) {
                alt &= ~LFS3_TAG_R;
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
                        alt, weight,
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)
                    && jump > branch) {
                alt = p[0].alt & ~LFS3_TAG_R;
                weight = p[0].weight;
                jump = p[0].jump;
                lfs3_rbyd_p_pop(p);
            }

            // prune red alts
            if (lfs3_tag_isred(p[0].alt)
                    && lfs3_tag_unreachable(
                        p[0].alt, p[0].weight,
                        lower_rid, upper_rid,
                        lower_tag, upper_tag)) {
                // prune unreachable recolorable alts
                //      <r  =>          <b
                // .----'|         .----'|
                // |    <b         |     |
                // |  .-'|         |  .--'
                // 1  2  3      1  2  3  x
                LFS3_ASSERT(p[0].jump < branch);
                lfs3_rbyd_p_pop(p);
            }

            // prune black alts
            if (lfs3_tag_unreachable2(
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
                } else if (lfs3_tag_isred(p[0].alt)) {
                    LFS3_ASSERT(jump < branch);
                    alt = (p[0].alt & ~LFS3_TAG_R) | (alt & LFS3_TAG_R);
                    weight = p[0].weight;
                    jump = p[0].jump;
                    lfs3_rbyd_p_pop(p);

                // we can't prune non-root black alts or we risk
                // breaking the color balance of our tree, so instead
                // we just mark these alts as unreachable (jump=0), and
                // collapse them if we propagate a red edge later
                //   <b  =>       nb
                // .-'|         .--'
                // 3  4      3  4  x
                } else if (lfs3_tag_isblack(alt)) {
                    alt = LFS3_TAG_ALT(
                            LFS3_TAG_B,
                            LFS3_TAG_LE,
                            (diverged && (a_rid > b_rid || a_tag > b_tag))
                                ? d_tag
                                : lower_tag);
                    LFS3_ASSERT(weight == 0);
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
            if (lfs3_tag_isred(alt) && lfs3_tag_isred(p[0].alt)) {
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
                        LFS3_SWAP(lfs3_tag_t, &p[0].alt, &alt);
                        LFS3_SWAP(lfs3_rid_t, &p[0].weight, &weight);
                        LFS3_SWAP(lfs3_size_t, &p[0].jump, &jump);
                    }
                    alt &= ~LFS3_TAG_R;

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
                    p[0].alt = alt;
                    p[0].weight += weight;
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
            if (lfs3_tag_isred(alt)) {
                // undo flip temporarily
                if (branch_ < branch) {
                    lfs3_tag_flip2(
                            &alt, &weight,
                            p[0].alt, p[0].weight,
                            lower_rid, upper_rid);
                    LFS3_SWAP(lfs3_size_t, &jump, &branch_);
                }

            // black alt? terminate 2-3-4 nodes
            } else {
                // trim alts from our current bounds
                lfs3_tag_trim2(
                        alt, weight,
                        p[0].alt, p[0].weight,
                        &lower_rid, &upper_rid,
                        &lower_tag, &upper_tag);
            }

            // push alt onto our queue
            err = lfs3_rbyd_p_push(lfs3, rbyd, p,
                    alt, weight, jump);
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
            tag_ = lfs3_tag_key(alt);

            // the last alt should always end up black
            LFS3_ASSERT(lfs3_tag_isblack(p[0].alt));

            if (diverged) {
                // diverged lower trunk? move on to upper trunk
                if (a_rid < b_rid || a_tag < b_tag) {
                    // keep track of the lower diverged bound
                    d_tag = lower_tag;
                    d_weight = upper_rid - lower_rid;

                    // flush any pending alts
                    err = lfs3_rbyd_p_flush(lfs3, rbyd, p, 3);
                    if (err) {
                        return err;
                    }

                    // terminate diverged trunk with an unreachable tag
                    err = lfs3_rbyd_appendrattr_(lfs3, rbyd, LFS3_RATTR(
                            (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0)
                                | LFS3_TAG_NULL,
                            0));
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
    lfs3_tag_t alt = 0;
    lfs3_rid_t weight = 0;
    if (tag_
            && (upper_rid-1 < rid-lfs3_smax(-rattr.weight, 0)
                || (upper_rid-1 == rid-lfs3_smax(-rattr.weight, 0)
                    && ((!lfs3_tag_isgrow(rattr.tag) && rattr.weight > 0)
                        || ((tag_ & lfs3_tag_mask(rattr.tag))
                            < (rattr.tag & lfs3_tag_mask(rattr.tag))))))) {
        if (lfs3_tag_isrm(rattr.tag) || !lfs3_tag_key(rattr.tag)) {
            // if removed, make our tag unreachable
            alt = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_GT, lower_tag);
            weight = upper_rid - lower_rid + rattr.weight;
            upper_rid -= weight;
        } else {
            // split less than
            alt = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_LE, tag_);
            weight = upper_rid - lower_rid;
            lower_rid += weight;
        }

    } else if (tag_
            && (upper_rid-1 > rid
                || (upper_rid-1 == rid
                    && ((!lfs3_tag_isgrow(rattr.tag) && rattr.weight > 0)
                        || ((tag_ & lfs3_tag_mask(rattr.tag))
                            > (rattr.tag & lfs3_tag_mask(rattr.tag))))))) {
        if (lfs3_tag_isrm(rattr.tag) || !lfs3_tag_key(rattr.tag)) {
            // if removed, make our tag unreachable
            alt = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_GT, lower_tag);
            weight = upper_rid - lower_rid + rattr.weight;
            upper_rid -= weight;
        } else {
            // split greater than
            alt = LFS3_TAG_ALT(LFS3_TAG_B, LFS3_TAG_GT, rattr.tag);
            weight = upper_rid - (rid+1);
            upper_rid -= weight;
        }
    }

    if (alt) {
        err = lfs3_rbyd_p_push(lfs3, rbyd, p,
                alt, weight, branch);
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
    err = lfs3_rbyd_appendrattr_(lfs3, rbyd, LFS3_RATTR_(
            // mark as shrub if we are a shrub
            (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0)
                // rm => null, otherwise strip off control bits
                | ((lfs3_tag_isrm(rattr.tag))
                    ? LFS3_TAG_NULL
                    : lfs3_tag_key(rattr.tag)),
            upper_rid - lower_rid + rattr.weight,
            rattr));
    if (err) {
        return err;
    }

    // update the trunk and weight
    rbyd->trunk = (rbyd->trunk & LFS3_RBYD_ISSHRUB) | trunk_;
    rbyd->weight += rattr.weight;
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
                &e, 1);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }

        // we don't want the next commit to appear as valid, so we
        // intentionally perturb the commit if this happens, this is
        // roughly equivalent to inverting all tags' valid bits
        perturb = ((e >> 7) == lfs3_parity(cksum));

        // calculate the erased-state checksum
        uint32_t ecksum = 0;
        err = lfs3_bd_cksum(lfs3,
                rbyd->blocks[0], off_, lfs3->cfg->prog_size,
                lfs3->cfg->prog_size,
                &ecksum);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }

        err = lfs3_rbyd_appendrattr_(lfs3, rbyd, LFS3_RATTR_ECKSUM(
                LFS3_TAG_ECKSUM, 0,
                (&(lfs3_ecksum_t){
                    .cksize=lfs3->cfg->prog_size,
                    .cksum=ecksum})));
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
            cksum_buf, 2+1+4+4,
            NULL, false);
    if (err) {
        return err;
    }

    // flush any pending progs
    err = lfs3_bd_flush(lfs3, NULL, false);
    if (err) {
        return err;
    }

    // update the eoff and perturb
    rbyd->eoff
            = ((lfs3_size_t)perturb << (8*sizeof(lfs3_size_t)-1))
            | off_;
    // revert to canonical checksum
    rbyd->cksum = cksum;

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
        const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    // append each tag to the tree
    for (lfs3_size_t i = 0; i < rattr_count; i++) {
        // treat inserts after the first tag as though they are splits,
        // sequential inserts don't really make sense otherwise
        if (i > 0 && lfs3_rattr_isinsert(rattrs[i])) {
            rid += 1;
        }

        // don't write tags outside of the requested range
        if (rid >= start_rid
                // note the use of rid+1 and unsigned comparison here to
                // treat end_rid=-1 as "unbounded" in such a way that rid=-1
                // is still included
                && (lfs3_size_t)(rid + 1) <= (lfs3_size_t)end_rid) {
            int err = lfs3_rbyd_appendrattr(lfs3, rbyd,
                    rid - lfs3_smax(start_rid, 0),
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
        rid = lfs3_rattr_nextrid(rattrs[i], rid);
    }

    return 0;
}

static int lfs3_rbyd_commit(lfs3_t *lfs3, lfs3_rbyd_t *rbyd,
        lfs3_srid_t rid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    // append each tag to the tree
    int err = lfs3_rbyd_appendrattrs(lfs3, rbyd, rid, -1, -1,
            rattrs, rattr_count);
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
// compaction. This uses a conservative estimate so the actual on-disk cost
// should be smaller.
//
// This also returns a good split_rid in case the rbyd needs to be split.
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

        lfs3_tag_t tag = 0;
        lfs3_rid_t weight = 0;
        lfs3_size_t dsize_ = 0;
        while (true) {
            lfs3_srid_t rid_;
            lfs3_rid_t weight_;
            lfs3_data_t data;
            int err = lfs3_rbyd_lookupnext(lfs3, rbyd,
                    a_rid, tag+1,
                    &rid_, &tag, &weight_, &data);
            if (err) {
                if (err == LFS3_ERR_NOENT) {
                    break;
                }
                return err;
            }
            if (rid_ > a_rid+lfs3_smax(weight_-1, 0)) {
                break;
            }

            // keep track of rid and weight
            a_rid = rid_;
            weight += weight_;

            // include the cost of this tag
            dsize_ += lfs3->rattr_estimate + lfs3_data_size(data);
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
        lfs3_rattr_t rattr) {
    // begin appending
    int err = lfs3_rbyd_appendinit(lfs3, rbyd);
    if (err) {
        return err;
    }

    // write the tag
    err = lfs3_rbyd_appendrattr_(lfs3, rbyd, LFS3_RATTR_(
            (lfs3_rbyd_isshrub(rbyd) ? LFS3_TAG_SHRUB : 0) | rattr.tag,
            rattr.weight,
            rattr));
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
    lfs3_tag_t tag = 0;
    while (true) {
        lfs3_rid_t weight;
        lfs3_data_t data;
        int err = lfs3_rbyd_lookupnext(lfs3, rbyd,
                rid, tag+1,
                &rid, &tag, &weight, &data);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
        }
        // end of range? note the use of rid+1 and unsigned comparison here to
        // treat end_rid=-1 as "unbounded" in such a way that rid=-1 is still
        // included
        if ((lfs3_size_t)(rid + 1) > (lfs3_size_t)end_rid) {
            break;
        }

        // write the tag
        err = lfs3_rbyd_appendcompactrattr(lfs3, rbyd_, LFS3_RATTR_DATA(
                tag, weight, &data));
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
                            rbyd->blocks[0], off, layer_ - off,
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
        lfs3_srid_t *rid_,
        lfs3_tag_t *tag_, lfs3_rid_t *weight_, lfs3_data_t *data_) {
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
        lfs3_tag_t tag__;
        lfs3_srid_t rid__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        int err = lfs3_rbyd_lookupnext(lfs3, rbyd,
                // lookup ~middle rid, note we may end up in the middle
                // of a weighted rid with this
                lower_rid + (upper_rid-1-lower_rid)/2, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // if we have no name, treat this rid as always lt
        if (lfs3_tag_suptype(tag__) != LFS3_TAG_NAME) {
            cmp = LFS3_CMP_LT;

        // compare names
        } else {
            cmp = lfs3_data_namecmp(lfs3, data__, did, name, name_len);
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
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static inline void lfs3_btree_claim(lfs3_btree_t *btree) {
    lfs3_rbyd_claim(btree);
}
#endif

#ifndef LFS3_2BONLY
static inline int lfs3_btree_cmp(
        const lfs3_btree_t *a,
        const lfs3_btree_t *b) {
    return lfs3_rbyd_cmp(a, b);
}
#endif


// branch on-disk encoding
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
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

#ifndef LFS3_2BONLY
static int lfs3_data_readbranch(lfs3_t *lfs3,
        lfs3_bid_t weight, lfs3_data_t *data,
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
#endif

#ifndef LFS3_2BONLY
static int lfs3_branch_fetch(lfs3_t *lfs3, lfs3_rbyd_t *branch,
        lfs3_block_t block, lfs3_size_t trunk, lfs3_bid_t weight,
        uint32_t cksum) {
    (void)lfs3;
    branch->blocks[0] = block;
    branch->trunk = trunk;
    branch->weight = weight;
    #ifndef LFS3_RDONLY
    branch->eoff = 0;
    #endif
    branch->cksum = cksum;

    // checking fetches?
    #ifdef LFS3_CKFETCHES
    if (lfs3_m_isckfetches(lfs3->flags)) {
        int err = lfs3_rbyd_fetchck(lfs3, branch,
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
#endif

#ifndef LFS3_2BONLY
static int lfs3_data_fetchbranch(lfs3_t *lfs3,
        lfs3_data_t *data, lfs3_bid_t weight,
        lfs3_rbyd_t *branch) {
    // decode branch and fetch
    int err = lfs3_data_readbranch(lfs3, weight, data,
            branch);
    if (err) {
        return err;
    }

    return lfs3_branch_fetch(lfs3, branch,
            branch->blocks[0], branch->trunk, branch->weight,
            branch->cksum);
}
#endif


// btree on-disk encoding
//
// this is the same as the branch on-disk econding, but prefixed with the
// btree's weight
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
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
    d += lfs3_data_size(data);

    return LFS3_DATA_BUF(buffer, d);
}
#endif

#ifndef LFS3_2BONLY
static int lfs3_data_readbtree(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_btree_t *btree) {
    lfs3_bid_t weight;
    int err = lfs3_data_readleb128(lfs3, data, &weight);
    if (err) {
        return err;
    }

    err = lfs3_data_readbranch(lfs3, weight, data, btree);
    if (err) {
        return err;
    }

    return 0;
}
#endif


// core btree operations

#ifndef LFS3_2BONLY
static int lfs3_btree_fetch(lfs3_t *lfs3, lfs3_btree_t *btree,
        lfs3_block_t block, lfs3_size_t trunk, lfs3_bid_t weight,
        uint32_t cksum) {
    // btree/branch fetch really are the same once we know the weight
    int err = lfs3_branch_fetch(lfs3, btree,
            block, trunk, weight,
            cksum);
    if (err) {
        return err;
    }

    #ifdef LFS3_DBGBTREEFETCHES
    LFS3_DEBUG("Fetched btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                "cksum %"PRIx32,
            btree->blocks[0], lfs3_rbyd_trunk(btree),
            btree->weight,
            btree->cksum);
    #endif
    return 0;
}
#endif

#ifndef LFS3_2BONLY
static int lfs3_data_fetchbtree(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_btree_t *btree) {
    // decode btree and fetch
    int err = lfs3_data_readbtree(lfs3, data,
            btree);
    if (err) {
        return err;
    }

    return lfs3_btree_fetch(lfs3, btree,
            btree->blocks[0], btree->trunk, btree->weight,
            btree->cksum);
}
#endif

// lookup rbyd/rid containing a given bid
#ifndef LFS3_2BONLY
static int lfs3_btree_lookupleaf(lfs3_t *lfs3, const lfs3_btree_t *btree,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_rbyd_t *rbyd_, lfs3_srid_t *rid_,
        lfs3_tag_t *tag_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    // descend down the btree looking for our bid
    *rbyd_ = *btree;
    lfs3_srid_t rid = bid;
    while (true) {
        // each branch is a pair of optional name + on-disk structure

        // lookup our bid in the rbyd
        lfs3_srid_t rid__;
        lfs3_tag_t tag__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        int err = lfs3_rbyd_lookupnext(lfs3, rbyd_, rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            return err;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            err = lfs3_rbyd_lookup(lfs3, rbyd_, rid__, LFS3_TAG_BRANCH,
                    &tag__, &data__);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFS3_TAG_BRANCH) {
            // adjust rid with subtree's weight
            rid -= (rid__ - (weight__-1));

            // fetch the next branch
            err = lfs3_data_fetchbranch(lfs3, &data__, weight__,
                    rbyd_);
            if (err) {
                return err;
            }

        // found our bid
        } else {
            // TODO how many of these should be conditional?
            if (bid_) {
                *bid_ = bid + (rid__ - rid);
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
#endif

// non-leaf lookups discard the rbyd info, which can be a bit more
// convenient, but may make commits more costly
#ifndef LFS3_2BONLY
static int lfs3_btree_lookupnext(lfs3_t *lfs3, const lfs3_btree_t *btree,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_tag_t *tag_, lfs3_bid_t *weight_,
        lfs3_data_t *data_) {
    lfs3_rbyd_t rbyd;
    return lfs3_btree_lookupleaf(lfs3, btree, bid,
            bid_, &rbyd, NULL, tag_, weight_, data_);
}
#endif

// lfs3_btree_lookup assumes a known bid, matching lfs3_rbyd_lookup's
// behavior, if you don't care about the exact bid either first call
// lfs3_btree_lookupnext, or lfs3_btree_lookupleaf + lfs3_rbyd_lookup
#ifndef LFS3_2BONLY
static int lfs3_btree_lookup(lfs3_t *lfs3, const lfs3_btree_t *btree,
        lfs3_bid_t bid, lfs3_tag_t tag,
        lfs3_tag_t *tag_, lfs3_data_t *data_) {
    // lookup rbyd in btree
    lfs3_bid_t bid__;
    lfs3_rbyd_t rbyd__;
    lfs3_srid_t rid__;
    int err = lfs3_btree_lookupleaf(lfs3, btree, bid,
            &bid__, &rbyd__, &rid__, NULL, NULL, NULL);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest bid, all we need to do is fail if it
    // picks up the wrong bid
    if (bid__ != bid) {
        return LFS3_ERR_NOENT;
    }

    // lookup tag in rbyd
    return lfs3_rbyd_lookup(lfs3, &rbyd__, rid__, tag,
            tag_, data_);
}
#endif

// TODO should lfs3_btree_lookupnext/lfs3_btree_parent be deduplicated?
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static int lfs3_btree_parent(lfs3_t *lfs3, const lfs3_btree_t *btree,
        lfs3_bid_t bid, const lfs3_rbyd_t *child,
        lfs3_rbyd_t *rbyd_, lfs3_srid_t *rid_) {
    // we should only call this when we actually have parents
    LFS3_ASSERT(bid < (lfs3_bid_t)btree->weight);
    LFS3_ASSERT(lfs3_rbyd_cmp(btree, child) != 0);

    // descend down the btree looking for our rid
    *rbyd_ = *btree;
    lfs3_srid_t rid = bid;
    while (true) {
        // each branch is a pair of optional name + on-disk structure
        lfs3_srid_t rid__;
        lfs3_tag_t tag__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        int err = lfs3_rbyd_lookupnext(lfs3, rbyd_, rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            err = lfs3_rbyd_lookup(lfs3, rbyd_, rid__, LFS3_TAG_BRANCH,
                    &tag__, &data__);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }
        }

        // didn't find our child?
        if (tag__ != LFS3_TAG_BRANCH) {
            return LFS3_ERR_NOENT;
        }

        // adjust rid with subtree's weight
        rid -= (rid__ - (weight__-1));

        // fetch the next branch
        lfs3_rbyd_t child_;
        err = lfs3_data_readbranch(lfs3, weight__, &data__, &child_);
        if (err) {
            return err;
        }

        // found our child?
        if (lfs3_rbyd_cmp(&child_, child) == 0) {
            // TODO how many of these should be conditional?
            if (rid_) {
                *rid_ = rid__;
            }
            return 0;
        }

        err = lfs3_branch_fetch(lfs3, rbyd_,
                child_.blocks[0], child_.trunk, child_.weight,
                child_.cksum);
        if (err) {
            return err;
        }
    }
}
#endif


// extra state needed for non-terminating lfs3_btree_commit_ calls
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
typedef struct lfs3_bcommit {
    // pending commit, this is updates as lfs3_btree_commit_ recurses
    lfs3_bid_t bid;
    const lfs3_rattr_t *rattrs;
    lfs3_size_t rattr_count;

    // internal lfs3_btree_commit_ state that needs to persist until
    // the root is committed
    struct {
        lfs3_rattr_t rattrs[4];
        lfs3_data_t split_name;
        uint8_t branch_l_buf[LFS3_BRANCH_DSIZE];
        uint8_t branch_r_buf[LFS3_BRANCH_DSIZE];
    } ctx;
} lfs3_bcommit_t;
#endif

// needed in lfs3_btree_commit_
static inline uint32_t lfs3_rev_btree(lfs3_t *lfs3);

// core btree algorithm
//
// this commits up to the root, but stops if:
// 1. we need a new root    => LFS3_ERR_RANGE
// 2. we have a shrub root  => LFS3_ERR_EXIST
//
// ---
//
// note! all non-bid-0 name updates must be via splits!
//
// This is because our btrees contain vestigial names, i.e. our inner
// nodes may contain names no longer in the tree. This simplifies
// lfs3_btree_commit_, but means insert-before-bid+1 is _not_ the same
// as insert-after-bid when named btrees are involved. If you try this
// it _will not_ work and if try to make it work you _will_ cry:
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
// The problem is that lfs3_btree_commit_ needs to find the same leaf
// rbyd as lfs3_btree_namelookup, and potentially insert-before the
// first rid or insert-after the last rid.
//
// Instead of separate insert-before/after flags, we make the first tag
// in a commit insert-before, and all following non-grow tags
// insert-after (splits).
//
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static int lfs3_btree_commit_(lfs3_t *lfs3,
        lfs3_btree_t *btree_, lfs3_btree_t *btree,
        lfs3_bcommit_t *bcommit) {
    LFS3_ASSERT(bcommit->bid <= (lfs3_bid_t)btree->weight);

    // lookup which leaf our bid resides
    //
    // for lfs3_btree_commit_ operations to work out, we need to
    // limit our bid to an rid in the tree, which is what this min
    // is doing
    lfs3_rbyd_t child = *btree;
    lfs3_srid_t rid = bcommit->bid;
    if (btree->weight > 0) {
        lfs3_srid_t rid_;
        int err = lfs3_btree_lookupleaf(lfs3, btree,
                lfs3_min(bcommit->bid, btree->weight-1),
                &bcommit->bid, &child, &rid_, NULL, NULL, NULL);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // adjust rid
        rid -= (bcommit->bid - rid_);
    }

    // tail-recursively commit to btree
    lfs3_rbyd_t *const child_ = btree_;
    while (true) {
        // we will always need our parent, so go ahead and find it
        lfs3_rbyd_t parent = {.trunk=0, .weight=0};
        lfs3_srid_t pid = 0;
        // are we root?
        if (!lfs3_rbyd_trunk(&child)
                || child.blocks[0] == btree->blocks[0]) {
            // new root? shrub root? yield the final root commit to
            // higher-level btree/bshrub logic
            if (!lfs3_rbyd_trunk(&child)
                    || lfs3_rbyd_isshrub(btree)) {
                bcommit->bid = rid;
                return (!lfs3_rbyd_trunk(&child))
                        ? LFS3_ERR_RANGE
                        : LFS3_ERR_EXIST;
            }

            // mark btree as unerased in case of failure, our btree rbyd and
            // root rbyd can diverge if there's a split, but we would have
            // marked the old root as unerased earlier anyways
            lfs3_btree_claim(btree);

        } else {
            int err = lfs3_btree_parent(lfs3, btree, bcommit->bid, &child,
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
        if (!lfs3_rbyd_isfetched(&child)) {
            int err = lfs3_rbyd_fetchck(lfs3, &child,
                    child.blocks[0], lfs3_rbyd_trunk(&child),
                    child.cksum);
            if (err) {
                return err;
            }
        }

        // is rbyd erased? can we sneak our commit into any remaining
        // erased bytes? note that the btree trunk field prevents this from
        // interacting with other references to the rbyd
        *child_ = child;
        int err = lfs3_rbyd_commit(lfs3, child_, rid,
                bcommit->rattrs, bcommit->rattr_count);
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
        if (child.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            // (note btree_ == child_)
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        //
        // note that since we defer merges to compaction time, we can
        // end up removing an rbyd here
        bcommit->bid -= pid - (child.weight-1);
        lfs3_size_t rattr_count = 0;
        if (child_->weight == 0) {
            bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_RM, -child.weight);
        } else {
            lfs3_data_t branch = lfs3_data_frombranch(
                    child_, bcommit->ctx.branch_l_buf);
            bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_BUF(
                    LFS3_TAG_BRANCH, 0,
                    branch.u.buffer, lfs3_data_size(branch));
            if (child_->weight != child.weight) {
                bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR(
                        LFS3_TAG_GROW, -child.weight + child_->weight);
            }
        }
        LFS3_ASSERT(rattr_count
                <= sizeof(bcommit->ctx.rattrs)
                    / sizeof(lfs3_rattr_t));
        bcommit->rattrs = bcommit->ctx.rattrs;
        bcommit->rattr_count = rattr_count;

        // recurse!
        child = parent;
        rid = pid;
        continue;

    compact:;
        // estimate our compacted size
        lfs3_srid_t split_rid;
        lfs3_ssize_t estimate = lfs3_rbyd_estimate(lfs3, &child, -1, -1,
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
                lfs3_tag_t sibling_tag;
                lfs3_rid_t sibling_weight;
                lfs3_data_t sibling_data;
                err = lfs3_rbyd_lookupnext(lfs3, &parent, pid+1, 0,
                        &sibling_rid, &sibling_tag, &sibling_weight,
                        &sibling_data);
                if (err) {
                    LFS3_ASSERT(err != LFS3_ERR_NOENT);
                    return err;
                }

                // if we found a bname, lookup the branch
                if (sibling_tag == LFS3_TAG_BNAME) {
                    err = lfs3_rbyd_lookup(lfs3, &parent,
                            sibling_rid, LFS3_TAG_BRANCH,
                            &sibling_tag, &sibling_data);
                    if (err) {
                        LFS3_ASSERT(err != LFS3_ERR_NOENT);
                        return err;
                    }
                }

                LFS3_ASSERT(sibling_tag == LFS3_TAG_BRANCH);
                err = lfs3_data_fetchbranch(lfs3, &sibling_data, sibling_weight,
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
            if (pid-(lfs3_srid_t)child.weight >= 0) {
                // try looking up the sibling
                lfs3_srid_t sibling_rid;
                lfs3_tag_t sibling_tag;
                lfs3_rid_t sibling_weight;
                lfs3_data_t sibling_data;
                err = lfs3_rbyd_lookupnext(lfs3, &parent, pid-child.weight, 0,
                        &sibling_rid, &sibling_tag, &sibling_weight,
                        &sibling_data);
                if (err) {
                    LFS3_ASSERT(err != LFS3_ERR_NOENT);
                    return err;
                }

                // if we found a bname, lookup the branch
                if (sibling_tag == LFS3_TAG_BNAME) {
                    err = lfs3_rbyd_lookup(lfs3, &parent,
                            sibling_rid, LFS3_TAG_BRANCH,
                            &sibling_tag, &sibling_data);
                    if (err) {
                        LFS3_ASSERT(err != LFS3_ERR_NOENT);
                        return err;
                    }
                }

                LFS3_ASSERT(sibling_tag == LFS3_TAG_BRANCH);
                err = lfs3_data_fetchbranch(lfs3, &sibling_data, sibling_weight,
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
                    pid -= child.weight;

                    *child_ = sibling;
                    sibling = child;
                    child = *child_;

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

        #if defined(LFS3_REVDBG) || defined(LFS3_REVNOISE)
        // append a revision count?
        err = lfs3_rbyd_appendrev(lfs3, child_, lfs3_rev_btree(lfs3));
        if (err) {
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }
        #endif

        // try to compact
        err = lfs3_rbyd_compact(lfs3, child_, &child, -1, -1);
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
                bcommit->rattrs, bcommit->rattr_count);
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
                && split_rid < (lfs3_srid_t)child.weight);

    split_relocate_l:;
        // allocate a new rbyd
        err = lfs3_rbyd_alloc(lfs3, child_);
        if (err) {
            return err;
        }

        #if defined(LFS3_REVDBG) || defined(LFS3_REVNOISE)
        // append a revision count?
        err = lfs3_rbyd_appendrev(lfs3, child_, lfs3_rev_btree(lfs3));
        if (err) {
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_l;
            }
            return err;
        }
        #endif

        // copy over tags < split_rid
        err = lfs3_rbyd_compact(lfs3, child_, &child, -1, split_rid);
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
                bcommit->rattrs, bcommit->rattr_count);
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

        #if defined(LFS3_REVDBG) || defined(LFS3_REVNOISE)
        // append a revision count?
        err = lfs3_rbyd_appendrev(lfs3, &sibling, lfs3_rev_btree(lfs3));
        if (err) {
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto split_relocate_r;
            }
            return err;
        }
        #endif

        // copy over tags >= split_rid
        err = lfs3_rbyd_compact(lfs3, &sibling, &child, split_rid, -1);
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
                bcommit->rattrs, bcommit->rattr_count);
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
        lfs3_tag_t split_tag;
        err = lfs3_rbyd_lookupnext(lfs3, &sibling, 0, 0,
                NULL, &split_tag, NULL, &bcommit->ctx.split_name);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // prepare commit to parent, tail recursing upwards
        LFS3_ASSERT(child_->weight > 0);
        LFS3_ASSERT(sibling.weight > 0);
        // new root?
        rattr_count = 0;
        if (!lfs3_rbyd_trunk(&parent)) {
            lfs3_data_t branch_l = lfs3_data_frombranch(
                    child_, bcommit->ctx.branch_l_buf);
            bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_BUF(
                    LFS3_TAG_BRANCH, +child_->weight,
                    branch_l.u.buffer, lfs3_data_size(branch_l));
            lfs3_data_t branch_r = lfs3_data_frombranch(
                    &sibling, bcommit->ctx.branch_r_buf);
            bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_BUF(
                    LFS3_TAG_BRANCH, +sibling.weight,
                    branch_r.u.buffer, lfs3_data_size(branch_r));
            if (lfs3_tag_suptype(split_tag) == LFS3_TAG_NAME) {
                bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_DATA(
                        LFS3_TAG_BNAME, 0,
                        &bcommit->ctx.split_name);
            }
        // split root?
        } else {
            bcommit->bid -= pid - (child.weight-1);
            lfs3_data_t branch_l = lfs3_data_frombranch(
                    child_, bcommit->ctx.branch_l_buf);
            bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_BUF(
                    LFS3_TAG_BRANCH, 0,
                    branch_l.u.buffer, lfs3_data_size(branch_l));
            if (child_->weight != child.weight) {
                bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR(
                        LFS3_TAG_GROW, -child.weight + child_->weight);
            }
            lfs3_data_t branch_r = lfs3_data_frombranch(
                    &sibling, bcommit->ctx.branch_r_buf);
            bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_BUF(
                    LFS3_TAG_BRANCH, +sibling.weight,
                    branch_r.u.buffer, lfs3_data_size(branch_r));
            if (lfs3_tag_suptype(split_tag) == LFS3_TAG_NAME) {
                bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_DATA(
                        LFS3_TAG_BNAME, 0,
                        &bcommit->ctx.split_name);
            }
        }
        LFS3_ASSERT(rattr_count
                <= sizeof(bcommit->ctx.rattrs)
                    / sizeof(lfs3_rattr_t));
        bcommit->rattrs = bcommit->ctx.rattrs;
        bcommit->rattr_count = rattr_count;

        // recurse!
        child = parent;
        rid = pid;
        continue;

    merge:;
    merge_relocate:;
        // allocate a new rbyd
        err = lfs3_rbyd_alloc(lfs3, child_);
        if (err) {
            return err;
        }

        #if defined(LFS3_REVDBG) || defined(LFS3_REVNOISE)
        // append a revision count?
        err = lfs3_rbyd_appendrev(lfs3, child_, lfs3_rev_btree(lfs3));
        if (err) {
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto merge_relocate;
            }
            return err;
        }
        #endif

        // merge the siblings together
        err = lfs3_rbyd_appendcompactrbyd(lfs3, child_, &child, -1, -1);
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
                bcommit->rattrs, bcommit->rattr_count);
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
        if (child.weight+sibling.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            // (note btree_ == child_)
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        LFS3_ASSERT(child_->weight > 0);
        // build attr list
        bcommit->bid -= pid - (child.weight-1);
        rattr_count = 0;
        bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR(
                LFS3_TAG_RM, -sibling.weight);
        lfs3_data_t branch = lfs3_data_frombranch(
                child_, bcommit->ctx.branch_l_buf);
        bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR_BUF(
                LFS3_TAG_BRANCH, 0,
                branch.u.buffer, lfs3_data_size(branch));
        if (child_->weight != child.weight) {
            bcommit->ctx.rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_GROW, -child.weight + child_->weight);
        }
        LFS3_ASSERT(rattr_count
                <= sizeof(bcommit->ctx.rattrs)
                    / sizeof(lfs3_rattr_t));
        bcommit->rattrs = bcommit->ctx.rattrs;
        bcommit->rattr_count = rattr_count;

        // recurse!
        child = parent;
        rid = pid + sibling.weight;
        continue;
    }
}
#endif

// commit/alloc a new btree root
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static int lfs3_btree_commitroot_(lfs3_t *lfs3,
        lfs3_btree_t *btree_, lfs3_btree_t *btree,
        bool split,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
relocate:;
    int err = lfs3_rbyd_alloc(lfs3, btree_);
    if (err) {
        return err;
    }

    #if defined(LFS3_REVDBG) || defined(LFS3_REVNOISE)
    // append a revision count?
    err = lfs3_rbyd_appendrev(lfs3, btree_, lfs3_rev_btree(lfs3));
    if (err) {
        // bad prog? try another block
        if (err == LFS3_ERR_CORRUPT) {
            goto relocate;
        }
        return err;
    }
    #endif

    // bshrubs may call this just to migrate rattrs to a btree
    if (!split) {
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

    err = lfs3_rbyd_commit(lfs3, btree_, bid, rattrs, rattr_count);
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

// commit to a btree, this is atomic
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static int lfs3_btree_commit(lfs3_t *lfs3, lfs3_btree_t *btree,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    // try to commit to the btree
    lfs3_btree_t btree_;
    lfs3_bcommit_t bcommit; // do _not_ fully init this
    bcommit.bid = bid;
    bcommit.rattrs = rattrs;
    bcommit.rattr_count = rattr_count;
    int err = lfs3_btree_commit_(lfs3, &btree_, btree,
            &bcommit);
    if (err && err != LFS3_ERR_RANGE) {
        LFS3_ASSERT(err != LFS3_ERR_EXIST);
        return err;
    }

    // needs a new root?
    if (err == LFS3_ERR_RANGE) {
        err = lfs3_btree_commitroot_(lfs3, &btree_, btree, true,
                bcommit.bid, bcommit.rattrs, bcommit.rattr_count);
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

// lookup in a btree by name
#ifndef LFS3_2BONLY
static lfs3_scmp_t lfs3_btree_namelookupleaf(lfs3_t *lfs3,
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
    *rbyd_ = *btree;
    lfs3_bid_t bid = 0;
    while (true) {
        // each branch is a pair of optional name + on-disk structure

        // lookup our name in the rbyd via binary search
        lfs3_srid_t rid__;
        lfs3_tag_t tag__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        lfs3_scmp_t cmp = lfs3_rbyd_namelookup(lfs3, rbyd_,
                did, name, name_len,
                &rid__, &tag__, &weight__, &data__);
        if (cmp < 0) {
            LFS3_ASSERT(cmp != LFS3_ERR_NOENT);
            return cmp;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            int err = lfs3_rbyd_lookup(lfs3, rbyd_, rid__,
                    LFS3_TAG_MASK8 | LFS3_TAG_STRUCT,
                    &tag__, &data__);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFS3_TAG_BRANCH) {
            // update our bid
            bid += rid__ - (weight__-1);

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
                *bid_ = bid + rid__;
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
#endif

#ifndef LFS3_2BONLY
static lfs3_scmp_t lfs3_btree_namelookup(lfs3_t *lfs3,
        const lfs3_btree_t *btree,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_bid_t *bid_,
        lfs3_tag_t *tag_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    lfs3_rbyd_t rbyd;
    return lfs3_btree_namelookupleaf(lfs3, btree,
            did, name, name_len,
            bid_, &rbyd, NULL, tag_, weight_, data_);
}
#endif

// incremental btree traversal
//
// note this is different from iteration, iteration should use
// lfs3_btree_lookupnext, traversal includes inner btree nodes

#ifndef LFS3_2BONLY
static void lfs3_btraversal_init(lfs3_btraversal_t *bt) {
    bt->bid = 0;
    bt->branch = NULL;
    bt->rid = 0;
}
#endif

#ifndef LFS3_2BONLY
static int lfs3_btree_traverse(lfs3_t *lfs3, const lfs3_btree_t *btree,
        lfs3_btraversal_t *bt,
        lfs3_bid_t *bid_, lfs3_tag_t *tag_, lfs3_bid_t *weight_,
        lfs3_data_t *data_) {
    // explicitly traverse the root even if weight=0
    if (!bt->branch) {
        bt->branch = btree;
        bt->rid = bt->bid;

        // traverse the root
        if (bt->bid == 0
                // unless we don't even have a root yet
                && lfs3_rbyd_trunk(btree) != 0
                // or are a shrub
                && !lfs3_rbyd_isshrub(btree)) {
            if (bid_) {
                *bid_ = btree->weight-1;
            }
            if (tag_) {
                *tag_ = LFS3_TAG_BRANCH;
            }
            if (weight_) {
                *weight_ = btree->weight;
            }
            if (data_) {
                data_->u.buffer = (const uint8_t*)bt->branch;
            }
            return 0;
        }
    }

    // need to restart from the root?
    if (bt->rid >= (lfs3_srid_t)bt->branch->weight) {
        bt->branch = btree;
        bt->rid = bt->bid;
    }

    // descend down the tree
    while (true) {
        lfs3_srid_t rid__;
        lfs3_tag_t tag__;
        lfs3_rid_t weight__;
        lfs3_data_t data__;
        int err = lfs3_rbyd_lookupnext(lfs3, bt->branch, bt->rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            return err;
        }

        // if we found a bname, lookup the branch
        if (tag__ == LFS3_TAG_BNAME) {
            err = lfs3_rbyd_lookup(lfs3, bt->branch, rid__, LFS3_TAG_BRANCH,
                    &tag__, &data__);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFS3_TAG_BRANCH) {
            // adjust rid with subtree's weight
            bt->rid -= (rid__ - (weight__-1));

            // fetch the next branch
            err = lfs3_data_fetchbranch(lfs3, &data__, weight__,
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
                    *tag_ = LFS3_TAG_BRANCH;
                }
                if (weight_) {
                    *weight_ = weight__;
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
            lfs3_bid_t bid__ = bt->bid + (rid__ - bt->rid);
            bt->bid = bid__ + 1;
            bt->rid = rid__ + 1;

            if (bid_) {
                *bid_ = bid__;
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
#endif




/// B-shrub operations ///

// shrub things

// helper functions
static inline bool lfs3_shrub_isshrub(const lfs3_shrub_t *shrub) {
    return lfs3_rbyd_isshrub(shrub);
}

static inline lfs3_size_t lfs3_shrub_trunk(const lfs3_shrub_t *shrub) {
    return lfs3_rbyd_trunk(shrub);
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

    // just write the trunk and weight, the rest of the rbyd is contextual
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
    // force estimate recalculation if we write to this shrub
    #ifndef LFS3_RDONLY
    shrub->eoff = -1;
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

// needed in lfs3_shrub_estimate
static inline bool lfs3_o_isbshrub(uint32_t flags);

// these are used in mdir commit/compaction
#ifndef LFS3_RDONLY
static lfs3_ssize_t lfs3_shrub_estimate(lfs3_t *lfs3,
        const lfs3_shrub_t *shrub) {
    // only include the last reference
    const lfs3_shrub_t *last = NULL;
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_o_isbshrub(o->flags)
                && lfs3_shrub_cmp(
                    &((lfs3_bshrub_t*)o)->shrub,
                    shrub) == 0) {
            last = &((lfs3_bshrub_t*)o)->shrub;
        }
    }
    if (last && shrub != last) {
        return 0;
    }

    return lfs3_rbyd_estimate(lfs3, shrub, -1, -1,
            NULL);
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_shrub_compact(lfs3_t *lfs3, lfs3_rbyd_t *rbyd_,
        lfs3_shrub_t *shrub_, const lfs3_shrub_t *shrub) {
    // save our current trunk/weight
    lfs3_size_t trunk = rbyd_->trunk;
    lfs3_srid_t weight = rbyd_->weight;

    // compact our bshrub
    int err = lfs3_rbyd_appendshrub(lfs3, rbyd_, shrub);
    if (err) {
        return err;
    }

    // stage any opened shrubs with their new location so we can
    // update these later if our commit is a success
    //
    // this should include our current bshrub
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_o_isbshrub(o->flags)
                && lfs3_shrub_cmp(
                    &((lfs3_bshrub_t*)o)->shrub,
                    shrub) == 0) {
            ((lfs3_bshrub_t*)o)->shrub_.blocks[0] = rbyd_->blocks[0];
            ((lfs3_bshrub_t*)o)->shrub_.trunk = rbyd_->trunk;
            ((lfs3_bshrub_t*)o)->shrub_.weight = rbyd_->weight;
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

// this is needed to sneak shrub commits into mdir commits
#ifndef LFS3_RDONLY
typedef struct lfs3_shrubcommit {
    lfs3_bshrub_t *bshrub;
    lfs3_srid_t rid;
    const lfs3_rattr_t *rattrs;
    lfs3_size_t rattr_count;
} lfs3_shrubcommit_t;
#endif

#ifndef LFS3_RDONLY
static int lfs3_shrub_commit(lfs3_t *lfs3, lfs3_rbyd_t *rbyd_,
        lfs3_shrub_t *shrub, lfs3_srid_t rid,
        const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    // swap out our trunk/weight temporarily, note we're
    // operating on a copy so if this fails we shouldn't mess
    // things up too much
    //
    // it is important that these rbyds share eoff/cksum/etc
    lfs3_size_t trunk = rbyd_->trunk;
    lfs3_srid_t weight = rbyd_->weight;
    rbyd_->trunk = shrub->trunk;
    rbyd_->weight = shrub->weight;

    // append any bshrub attributes
    int err = lfs3_rbyd_appendrattrs(lfs3, rbyd_, rid, -1, -1,
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
#endif


// ok, actual bshrub things

// create a non-existant bshrub
static void lfs3_bshrub_init(lfs3_bshrub_t *bshrub) {
    // set up a null bshrub
    bshrub->shrub.weight = 0;
    bshrub->shrub.blocks[0] = -1;
    bshrub->shrub.trunk = 0;
    // force estimate recalculation
    #ifndef LFS3_RDONLY
    bshrub->shrub.eoff = -1;
    #endif
}

static inline bool lfs3_bshrub_isbnull(const lfs3_bshrub_t *bshrub) {
    return !bshrub->shrub.trunk;
}

static inline bool lfs3_bshrub_isbshrub(const lfs3_bshrub_t *bshrub) {
    return lfs3_shrub_isshrub(&bshrub->shrub);
}

static inline bool lfs3_bshrub_isbtree(const lfs3_bshrub_t *bshrub) {
    return !lfs3_shrub_isshrub(&bshrub->shrub);
}

static inline int lfs3_bshrub_cmp(
        const lfs3_bshrub_t *a,
        const lfs3_bshrub_t *b) {
    return lfs3_rbyd_cmp(&a->shrub, &b->shrub);
}

// needed in lfs3_bshrub_fetch
static int lfs3_mdir_lookup(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag,
        lfs3_tag_t *tag_, lfs3_data_t *data_);

// fetch the bshrub/btree attatched to the current mdir+mid, if there
// is one
//
// note we don't mess with bshrub on error!
static int lfs3_bshrub_fetch(lfs3_t *lfs3, lfs3_bshrub_t *bshrub) {
    // lookup the file struct, if there is one
    lfs3_tag_t tag;
    lfs3_data_t data;
    int err = lfs3_mdir_lookup(lfs3, &bshrub->o.mdir,
            LFS3_TAG_MASK8 | LFS3_TAG_STRUCT,
            &tag, &data);
    if (err) {
        return err;
    }

    // these functions leave bshrub undefined if there is an error, so
    // first read into the staging shrub

    // found a bshrub? (inlined btree)
    if (tag == LFS3_TAG_BSHRUB) {
        err = lfs3_data_readshrub(lfs3, &bshrub->o.mdir, &data,
                &bshrub->shrub_);
        if (err) {
            return err;
        }

    // found a btree?
    } else if (LFS3_IFDEF_2BONLY(false, tag == LFS3_TAG_BTREE)) {
        #ifndef LFS3_2BONLY
        err = lfs3_data_fetchbtree(lfs3, &data,
                &bshrub->shrub_);
        if (err) {
            return err;
        }
        #endif

    // we can run into other structs, dids in lfs3_mtree_traverse for
    // example, just ignore these for now
    } else {
        return LFS3_ERR_NOENT;
    }

    // update the bshrub/btree
    bshrub->shrub = bshrub->shrub_;
    return 0;
}

// find a tight upper bound on the _full_ bshrub size, this includes
// any on-disk bshrubs, and all pending bshrubs
#ifndef LFS3_RDONLY
static lfs3_ssize_t lfs3_bshrub_estimate(lfs3_t *lfs3,
        const lfs3_bshrub_t *bshrub) {
    lfs3_size_t estimate = 0;

    // include all unique shrubs related to our file, including the
    // on-disk shrub
    lfs3_tag_t tag;
    lfs3_data_t data;
    int err = lfs3_mdir_lookup(lfs3, &bshrub->o.mdir, LFS3_TAG_BSHRUB,
            &tag, &data);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }

    if (err != LFS3_ERR_NOENT) {
        lfs3_shrub_t shrub;
        err = lfs3_data_readshrub(lfs3, &bshrub->o.mdir, &data,
                &shrub);
        if (err) {
            return err;
        }

        lfs3_ssize_t dsize = lfs3_shrub_estimate(lfs3, &shrub);
        if (dsize < 0) {
            return dsize;
        }
        estimate += dsize;
    }

    // this includes our current shrub
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_o_isbshrub(o->flags)
                && o->mdir.mid == bshrub->o.mdir.mid
                && lfs3_bshrub_isbshrub((lfs3_bshrub_t*)o)) {
            lfs3_ssize_t dsize = lfs3_shrub_estimate(lfs3,
                    &((lfs3_bshrub_t*)o)->shrub);
            if (dsize < 0) {
                return dsize;
            }
            estimate += dsize;
        }
    }

    return estimate;
}
#endif

// bshrub lookup functions
#ifndef LFS3_2BONLY
static int lfs3_bshrub_lookupleaf(lfs3_t *lfs3, const lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_rbyd_t *rbyd_, lfs3_srid_t *rid_,
        lfs3_tag_t *tag_, lfs3_bid_t *weight_, lfs3_data_t *data_) {
    return lfs3_btree_lookupleaf(lfs3, &bshrub->shrub, bid,
            bid_, rbyd_, rid_, tag_, weight_, data_);
}
#endif

static int lfs3_bshrub_lookupnext(lfs3_t *lfs3, const lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_tag_t *tag_, lfs3_bid_t *weight_,
        lfs3_data_t *data_) {
    #ifndef LFS3_2BONLY
    return lfs3_btree_lookupnext(lfs3, &bshrub->shrub, bid,
            bid_, tag_, weight_, data_);
    #else
    return lfs3_rbyd_lookupnext(lfs3, &bshrub->shrub, bid, 0,
            (lfs3_srid_t*)bid_, tag_, weight_, data_);
    #endif
}

#ifndef LFS3_2BONLY
static int lfs3_bshrub_lookup(lfs3_t *lfs3, const lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid, lfs3_tag_t tag,
        lfs3_tag_t *tag_, lfs3_data_t *data_) {
    return lfs3_btree_lookup(lfs3, &bshrub->shrub, bid, tag,
            tag_, data_);
}
#endif

#ifndef LFS3_2BONLY
static int lfs3_bshrub_traverse(lfs3_t *lfs3, const lfs3_bshrub_t *bshrub,
        lfs3_btraversal_t *bt,
        lfs3_bid_t *bid_, lfs3_tag_t *tag_, lfs3_bid_t *weight_,
        lfs3_data_t *data_) {
    return lfs3_btree_traverse(lfs3, &bshrub->shrub, bt,
            bid_, tag_, weight_, data_);
}
#endif

// needed in lfs3_bshrub_commitroot_
#ifndef LFS3_RDONLY
static int lfs3_mdir_commit(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count);
#endif

// commit to the bshrub root, i.e. the bshrub's shrub
#ifndef LFS3_RDONLY
static int lfs3_bshrub_commitroot_(lfs3_t *lfs3, lfs3_bshrub_t *bshrub,
        bool split,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
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
    lfs3_size_t commit_estimate = 0;
    for (lfs3_size_t i = 0; i < rattr_count; i++) {
        commit_estimate += lfs3->rattr_estimate;
        // fortunately the tags we commit to shrubs are actually quite
        // limited, if lazily encoded the rattr should set rattr.count
        // to the expected dsize
        if (rattrs[i].from == LFS3_FROM_DATA) {
            for (lfs3_size_t j = 0; j < rattrs[i].count; j++) {
                commit_estimate += lfs3_data_size(rattrs[i].u.datas[j]);
            }
        } else {
            commit_estimate += rattrs[i].count;
        }
    }

    // does our estimate exceed our inline_size? need to recalculate an
    // accurate estimate
    lfs3_ssize_t estimate = (split) ? (lfs3_size_t)-1 : bshrub->shrub.eoff;
    // this double condition avoids overflow issues
    if ((lfs3_size_t)estimate > lfs3->cfg->inline_size
            || estimate + commit_estimate > lfs3->cfg->inline_size) {
        estimate = lfs3_bshrub_estimate(lfs3, bshrub);
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
        if ((lfs3_size_t)estimate > lfs3->cfg->inline_size/2
                || estimate + commit_estimate > lfs3->cfg->inline_size) {
            return LFS3_ERR_RANGE;
        }
    }

    // include our pending commit in the new estimate
    estimate += commit_estimate;

    // commit to shrub
    //
    // note we do _not_ checkpoint the allocator here, blocks may be
    // in-flight!
    int err = lfs3_mdir_commit(lfs3, &bshrub->o.mdir, LFS3_RATTRS(
            LFS3_RATTR_SHRUBCOMMIT(
                (&(lfs3_shrubcommit_t){
                    .bshrub=bshrub,
                    .rid=bid,
                    .rattrs=rattrs,
                    .rattr_count=rattr_count}))));
    if (err) {
        return err;
    }
    LFS3_ASSERT(bshrub->shrub.blocks[0] == bshrub->o.mdir.r.blocks[0]);

    // update _all_ shrubs with the new estimate
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_o_isbshrub(o->flags)
                && o->mdir.mid == bshrub->o.mdir.mid
                && lfs3_bshrub_isbshrub((lfs3_bshrub_t*)o)) {
            ((lfs3_bshrub_t*)o)->shrub.eoff = estimate;
        }
    }
    LFS3_ASSERT(bshrub->shrub.eoff == (lfs3_size_t)estimate);

    return 0;
}
#endif

// commit to bshrub, this is atomic
#ifndef LFS3_RDONLY
static int lfs3_bshrub_commit(lfs3_t *lfs3, lfs3_bshrub_t *bshrub,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    #ifndef LFS3_2BONLY
    // before we touch anything, we need to mark all other btree references
    // as unerased
    if (lfs3_bshrub_isbtree(bshrub)) {
        for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
            if (lfs3_o_isbshrub(o->flags)
                    && o != &bshrub->o
                    && ((lfs3_bshrub_t*)o)->shrub.blocks[0]
                        == bshrub->shrub.blocks[0]) {
                // mark as unerased
                lfs3_btree_claim(&((lfs3_bshrub_t*)o)->shrub);
            }
        }
    }

    // try to commit to the btree
    lfs3_bcommit_t bcommit; // do _not_ fully init this
    bcommit.bid = bid;
    bcommit.rattrs = rattrs;
    bcommit.rattr_count = rattr_count;
    int err = lfs3_btree_commit_(lfs3, &bshrub->shrub_, &bshrub->shrub,
            &bcommit);
    if (err && err != LFS3_ERR_EXIST
            && err != LFS3_ERR_RANGE) {
        return err;
    }
    bool split = (err == LFS3_ERR_RANGE);

    // when btree is shrubbed, lfs3_btree_commit_ stops at the root
    // and returns with pending rattrs
    if (err == LFS3_ERR_EXIST
            || err == LFS3_ERR_RANGE) {
        // try to commit to shrub root
        err = lfs3_bshrub_commitroot_(lfs3, bshrub, split,
                bcommit.bid, bcommit.rattrs, bcommit.rattr_count);
        if (err && err != LFS3_ERR_RANGE) {
            return err;
        }

        // if we don't fit, convert to btree
        if (err == LFS3_ERR_RANGE) {
            err = lfs3_btree_commitroot_(lfs3,
                    &bshrub->shrub_, &bshrub->shrub, split,
                    bcommit.bid, bcommit.rattrs, bcommit.rattr_count);
            if (err) {
                return err;
            }
        }
    }
    #else
    // in 2-block mode, just commit to the shrub root
    int err = lfs3_bshrub_commitroot_(lfs3, bshrub, false,
            bcommit.bid, bcommit.rattrs, bcommit.rattr_count);
    if (err) {
        if (err == LFS3_ERR_RANGE) {
            return LFS3_ERR_NOSPC;
        }
        return err;
    }
    #endif

    // update the bshrub/btree
    bshrub->shrub = bshrub->shrub_;

    LFS3_ASSERT(lfs3_shrub_trunk(&bshrub->shrub));
    #ifdef LFS3_DBGBTREECOMMITS
    if (lfs3_bshrub_isbshrub(bshrub)) {
        LFS3_DEBUG("Committed bshrub "
                    "0x{%"PRIx32",%"PRIx32"}.%"PRIx32" w%"PRId32,
                bshrub->o.mdir.r.blocks[0], bshrub->o.mdir.r.blocks[1],
                lfs3_shrub_trunk(&bshrub->shrub),
                bshrub->shrub.weight);
    } else {
        LFS3_DEBUG("Committed btree 0x%"PRIx32".%"PRIx32" w%"PRId32", "
                    "cksum %"PRIx32,
                bshrub->shrub.blocks[0], lfs3_shrub_trunk(&bshrub->shrub),
                bshrub->shrub.weight,
                bshrub->shrub.cksum);
    }
    #endif
    return 0;
}
#endif




/// metadata-id things ///

#define LFS3_MID(_lfs, _bid, _rid) \
    (((_bid) & ~((1 << (_lfs)->mbits)-1)) + (_rid))

static inline lfs3_sbid_t lfs3_mbid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    return mid | ((1 << lfs3->mbits) - 1);
}

static inline lfs3_srid_t lfs3_mrid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    // bit of a strange mapping, but we want to preserve mid=-1 => rid=-1
    return (mid >> (8*sizeof(lfs3_smid_t)-1))
            | (mid & ((1 << lfs3->mbits) - 1));
}

// these should only be used for logging
static inline lfs3_sbid_t lfs3_dbgmbid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    if (LFS3_IFDEF_2BONLY(0, lfs3->mtree.weight) == 0) {
        return -1;
    } else {
        return mid >> lfs3->mbits;
    }
}

static inline lfs3_srid_t lfs3_dbgmrid(const lfs3_t *lfs3, lfs3_smid_t mid) {
    return lfs3_mrid(lfs3, mid);
}


/// metadata-pointer things ///

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



/// various flag things ///

// open flags
static inline bool lfs3_o_isrdonly(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return (flags & LFS3_O_MODE) == LFS3_O_RDONLY;
    #else
    return true;
    #endif
}

static inline bool lfs3_o_iswronly(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return (flags & LFS3_O_MODE) == LFS3_O_WRONLY;
    #else
    return false;
    #endif
}

static inline bool lfs3_o_iswrset(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return (flags & LFS3_O_MODE) == LFS3_o_WRSET;
    #else
    return false;
    #endif
}

static inline bool lfs3_o_iscreat(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_O_CREAT;
    #else
    return false;
    #endif
}

static inline bool lfs3_o_isexcl(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_O_EXCL;
    #else
    return false;
    #endif
}

static inline bool lfs3_o_istrunc(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_O_TRUNC;
    #else
    return false;
    #endif
}

static inline bool lfs3_o_isappend(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_O_APPEND;
    #else
    return false;
    #endif
}

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
static inline uint8_t lfs3_o_type(uint32_t flags) {
    return flags >> 28;
}

static inline uint32_t lfs3_o_typeflags(uint8_t type) {
    return (uint32_t)type << 28;
}

static inline void lfs3_o_settype(uint32_t *flags, uint8_t type) {
    *flags = (*flags & ~LFS3_o_TYPE) | lfs3_o_typeflags(type);
}

static inline bool lfs3_o_isbshrub(uint32_t flags) {
    return lfs3_o_type(flags) == LFS3_TYPE_REG
            || lfs3_o_type(flags) == LFS3_type_TRAVERSAL;
}

static inline bool lfs3_o_iszombie(uint32_t flags) {
    return flags & LFS3_o_ZOMBIE;
}

static inline bool lfs3_o_isuncreat(uint32_t flags) {
    return flags & LFS3_o_UNCREAT;
}

static inline bool lfs3_o_isunsync(uint32_t flags) {
    return flags & LFS3_o_UNSYNC;
}

static inline bool lfs3_o_isuncryst(uint32_t flags) {
    (void)flags;
    #if !defined(LFS3_KVONLY) && !defined(LFS3_2BONLY)
    return flags & LFS3_o_UNCRYST;
    #else
    return false;
    #endif
}

static inline bool lfs3_o_isunflush(uint32_t flags) {
    return flags & LFS3_o_UNFLUSH;
}

// custom attr flags
static inline bool lfs3_a_islazy(uint32_t flags) {
    return flags & LFS3_A_LAZY;
}

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

static inline bool lfs3_t_ismkconsistent(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_T_MKCONSISTENT;
    #else
    return false;
    #endif
}

static inline bool lfs3_t_islookahead(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_T_LOOKAHEAD;
    #else
    return false;
    #endif
}

static inline bool lfs3_t_iscompact(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_T_COMPACT;
    #else
    return false;
    #endif
}

static inline bool lfs3_t_isckmeta(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_T_CKMETA;
    #else
    return false;
    #endif
}

static inline bool lfs3_t_isckdata(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_T_CKDATA;
    #else
    return false;
    #endif
}

// internal traversal flags
static inline uint8_t lfs3_t_tstate(uint32_t flags) {
    return (flags >> 16) & 0xf;
}

static inline uint32_t lfs3_t_tstateflags(uint8_t tstate) {
    return (uint32_t)tstate << 16;
}

static inline void lfs3_t_settstate(uint32_t *flags, uint8_t tstate) {
    *flags = (*flags & ~LFS3_t_TSTATE) | lfs3_t_tstateflags(tstate);
}

static inline uint8_t lfs3_t_btype(uint32_t flags) {
    return (flags >> 20) & 0x0f;
}

static inline uint32_t lfs3_t_btypeflags(uint8_t btype) {
    return (uint32_t)btype << 20;
}

static inline void lfs3_t_setbtype(uint32_t *flags, uint8_t btype) {
    *flags = (*flags & ~LFS3_t_BTYPE) | lfs3_t_btypeflags(btype);
}

static inline bool lfs3_t_isdirty(uint32_t flags) {
    return flags & LFS3_t_DIRTY;
}

static inline bool lfs3_t_ismutated(uint32_t flags) {
    return flags & LFS3_t_MUTATED;
}

static inline uint32_t lfs3_t_swapdirty(uint32_t flags) {
    uint32_t x = ((flags >> 24) ^ (flags >> 25)) & 0x1;
    return flags ^ (x << 24) ^ (x << 25);
}

// mount flags
static inline bool lfs3_m_isrdonly(uint32_t flags) {
    (void)flags;
    #ifndef LFS3_RDONLY
    return flags & LFS3_M_RDONLY;
    #else
    return true;
    #endif
}

#ifdef LFS3_REVDBG
static inline bool lfs3_m_isrevdbg(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_REVDBG
    return true;
    #else
    return flags & LFS3_M_REVDBG;
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

#ifdef LFS3_CKDATACKSUMREADS
static inline bool lfs3_m_isckdatacksums(uint32_t flags) {
    (void)flags;
    #ifdef LFS3_YES_CKDATACKSUMREADS
    return true;
    #else
    return flags & LFS3_M_CKDATACKSUMREADS;
    #endif
}
#endif

// other internal flags
#ifdef LFS3_REVDBG
static inline bool lfs3_i_isinmtree(uint32_t flags) {
    return flags & LFS3_i_INMTREE;
}
#endif



/// opened mdir things ///

// we maintain a linked-list of all opened mdirs, in order to keep
// metadata state in-sync, these may be casted to specific file types

static bool lfs3_omdir_isopen(const lfs3_t *lfs3, const lfs3_omdir_t *o) {
    for (lfs3_omdir_t *o_ = lfs3->omdirs; o_; o_ = o_->next) {
        if (o_ == o) {
            return true;
        }
    }

    return false;
}

static void lfs3_omdir_open(lfs3_t *lfs3, lfs3_omdir_t *o) {
    LFS3_ASSERT(!lfs3_omdir_isopen(lfs3, o));
    // add to opened list
    o->next = lfs3->omdirs;
    lfs3->omdirs = o;
}

// needed in lfs3_omdir_close
static void lfs3_omdir_clobber(lfs3_t *lfs3, const lfs3_omdir_t *o,
        uint32_t flags);

static void lfs3_omdir_close(lfs3_t *lfs3, lfs3_omdir_t *o) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, o));
    // make sure we're not entangled in any traversals, note we don't
    // set the dirty bit here
    #ifndef LFS3_RDONLY
    lfs3_omdir_clobber(lfs3, o, 0);
    #endif
    // remove from opened list
    for (lfs3_omdir_t **o_ = &lfs3->omdirs; *o_; o_ = &(*o_)->next) {
        if (*o_ == o) {
            *o_ = (*o_)->next;
            break;
        }
    }
}

// check if a given mid is open
static bool lfs3_omdir_ismidopen(const lfs3_t *lfs3,
        lfs3_smid_t mid, uint32_t mask) {
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        // we really only care about regular open files here, all
        // others are either transient (dirs) or fake (orphans)
        if (lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == mid
                // allow caller to ignore files with specific flags
                && !(o->flags & ~mask)) {
            return true;
        }
    }

    return false;
}

// traversal invalidation things

// needed in lfs3_omdir_clobber
static void lfs3_traversal_clobber(lfs3_t *lfs3, lfs3_traversal_t *t);

// clobber any traversals referencing our mdir
#ifndef LFS3_RDONLY
static void lfs3_omdir_clobber(lfs3_t *lfs3, const lfs3_omdir_t *o,
        uint32_t flags) {
    for (lfs3_omdir_t *o_ = lfs3->omdirs; o_; o_ = o_->next) {
        if (lfs3_o_type(o_->flags) == LFS3_type_TRAVERSAL) {
            o_->flags |= flags;

            if (o && ((lfs3_traversal_t*)o_)->ot == o) {
                lfs3_traversal_clobber(lfs3, (lfs3_traversal_t*)o_);
            }
        }
    }
}
#endif

// clobber all traversals
#ifndef LFS3_RDONLY
static void lfs3_fs_clobber(lfs3_t *lfs3, uint32_t flags) {
    lfs3_omdir_clobber(lfs3, NULL, flags);
}
#endif



/// Global-state things ///

// grm (global remove) things
static inline lfs3_size_t lfs3_grm_count(const lfs3_t *lfs3) {
    return (lfs3->grm.queue[0] != 0) + (lfs3->grm.queue[1] != 0);
}

#ifndef LFS3_RDONLY
static inline void lfs3_grm_push(lfs3_t *lfs3, lfs3_smid_t mid) {
    // note mid=0.0 always maps to the root bookmark and should never
    // be grmed
    LFS3_ASSERT(mid != 0);
    LFS3_ASSERT(lfs3->grm.queue[1] == 0);
    lfs3->grm.queue[1] = lfs3->grm.queue[0];
    lfs3->grm.queue[0] = mid;
}
#endif

#ifndef LFS3_RDONLY
static inline lfs3_smid_t lfs3_grm_pop(lfs3_t *lfs3) {
    lfs3_smid_t mid = lfs3->grm.queue[0];
    lfs3->grm.queue[0] = lfs3->grm.queue[1];
    lfs3->grm.queue[1] = 0;
    return mid;
}
#endif

static inline bool lfs3_grm_ismidrm(const lfs3_t *lfs3, lfs3_smid_t mid) {
    return mid != 0
            && (lfs3->grm.queue[0] == mid
                || lfs3->grm.queue[1] == mid);
}

#ifndef LFS3_RDONLY
static lfs3_data_t lfs3_data_fromgrm(const lfs3_t *lfs3,
        uint8_t buffer[static LFS3_GRM_DSIZE]) {
    // make sure to zero so we don't leak any info
    lfs3_memset(buffer, 0, LFS3_GRM_DSIZE);

    // encode grms
    lfs3_size_t count = lfs3_grm_count(lfs3);
    lfs3_ssize_t d = 0;
    for (lfs3_size_t i = 0; i < count; i++) {
        lfs3_ssize_t d_ = lfs3_toleb128(lfs3->grm.queue[i], &buffer[d], 5);
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

static int lfs3_data_readgrm(lfs3_t *lfs3, lfs3_data_t *data) {
    // clear first
    lfs3->grm.queue[0] = 0;
    lfs3->grm.queue[1] = 0;

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
        lfs3->grm.queue[i] = mid;
    }

    return 0;
}


// some mdir-related gstate things we need

// zero any pending gdeltas
static void lfs3_fs_flushgdelta(lfs3_t *lfs3) {
    // zero the gcksumdelta
    lfs3->gcksum_d = 0;

    // zero the grmdelta
    lfs3_memset(lfs3->grm_d, 0, LFS3_GRM_DSIZE);
}

// commit any pending gdeltas
#ifndef LFS3_RDONLY
static void lfs3_fs_commitgdelta(lfs3_t *lfs3) {
    // keep track of the on-disk gcksum
    lfs3->gcksum_p = lfs3->gcksum;

    // keep track of the on-disk grm
    lfs3_data_fromgrm(lfs3, lfs3->grm_p);
}
#endif

// revert gstate to on-disk state
#ifndef LFS3_RDONLY
static void lfs3_fs_revertgdelta(lfs3_t *lfs3) {
    // revert to the on-disk gcksum
    lfs3->gcksum = lfs3->gcksum_p;

    // revert to the on-disk grm
    int err = lfs3_data_readgrm(lfs3,
            &LFS3_DATA_BUF(lfs3->grm_p, LFS3_GRM_DSIZE));
    if (err) {
        LFS3_UNREACHABLE();
    }
}
#endif

// append and consume any pending gstate
#ifndef LFS3_RDONLY
static int lfs3_rbyd_appendgdelta(lfs3_t *lfs3, lfs3_rbyd_t *rbyd) {
    // note gcksums are a special case and handled directly in
    // lfs3_mdir_commit__/lfs3_rbyd_appendcksum_

    // pending grm state?
    uint8_t grmdelta_[LFS3_GRM_DSIZE];
    lfs3_data_fromgrm(lfs3, grmdelta_);
    lfs3_memxor(grmdelta_, lfs3->grm_p, LFS3_GRM_DSIZE);
    lfs3_memxor(grmdelta_, lfs3->grm_d, LFS3_GRM_DSIZE);

    if (lfs3_memlen(grmdelta_, LFS3_GRM_DSIZE) != 0) {
        // make sure to xor any existing delta
        lfs3_data_t data;
        int err = lfs3_rbyd_lookup(lfs3, rbyd, -1, LFS3_TAG_GRMDELTA,
                NULL, &data);
        if (err && err != LFS3_ERR_NOENT) {
            return err;
        }

        uint8_t grmdelta[LFS3_GRM_DSIZE];
        lfs3_memset(grmdelta, 0, LFS3_GRM_DSIZE);
        if (err != LFS3_ERR_NOENT) {
            lfs3_ssize_t d = lfs3_data_read(lfs3, &data,
                    grmdelta, LFS3_GRM_DSIZE);
            if (d < 0) {
                return d;
            }
        }

        lfs3_memxor(grmdelta_, grmdelta, LFS3_GRM_DSIZE);

        // append to our rbyd, replacing any existing delta
        lfs3_size_t size = lfs3_memlen(grmdelta_, LFS3_GRM_DSIZE);
        err = lfs3_rbyd_appendrattr(lfs3, rbyd, -1, LFS3_RATTR_BUF(
                // opportunistically remove this tag if delta is all zero
                (size == 0)
                    ? LFS3_TAG_RM | LFS3_TAG_GRMDELTA
                    : LFS3_TAG_GRMDELTA, 0,
                grmdelta_, size));
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

static int lfs3_fs_consumegdelta(lfs3_t *lfs3, const lfs3_mdir_t *mdir) {
    // consume any gcksum deltas
    lfs3->gcksum_d ^= mdir->gcksumdelta;

    // consume any grm deltas
    lfs3_data_t data;
    int err = lfs3_rbyd_lookup(lfs3, &mdir->r, -1, LFS3_TAG_GRMDELTA,
            NULL, &data);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }

    if (err != LFS3_ERR_NOENT) {
        uint8_t grmdelta[LFS3_GRM_DSIZE];
        lfs3_ssize_t d = lfs3_data_read(lfs3, &data, grmdelta, LFS3_GRM_DSIZE);
        if (d < 0) {
            return d;
        }

        lfs3_memxor(lfs3->grm_d, grmdelta, d);
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
//                            '---------- pseudorandom noise (if revnoise)
//
// in revdbg mode, the bottom 24 bits are initialized with a hint based
// on rbyd type, though it may be overwritten by the recycle counter if
// it overlaps:
//
//   vvvv---- --1----1 -11-1--1 -11-1---  (68 69 21 v0  hi!.)  mroot anchor
//   vvvv---- -111111- -111--1- -11-11-1  (6d 72 7e v0  mr~.)  mroot
//   vvvv---- -111111- -11--1-- -11-11-1  (6d 64 7e v0  md~.)  mdir
//   vvvv---- -111111- -111-1-- -11---1-  (62 74 7e v0  bt~.)  file btree node
//   vvvv---- -111111- -11-11-1 -11---1-  (62 6d 7e v0  bm~.)  mtree node
//

// needed in lfs3_rev_init
static inline bool lfs3_mdir_ismrootanchor(const lfs3_mdir_t *mdir);
static inline int lfs3_mdir_cmp(const lfs3_mdir_t *a, const lfs3_mdir_t *b);

#ifndef LFS3_RDONLY
static inline uint32_t lfs3_rev_init(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        uint32_t rev) {
    (void)lfs3;
    (void)mdir;
    // we really only care about the top revision bits here
    rev &= ~((1 << 28)-1);
    // increment revision
    rev += 1 << 28;
    // include debug bits?
    #ifdef LFS3_REVDBG
    if (lfs3_m_isrevdbg(lfs3->flags)) {
        // mroot?
        if (mdir->mid == -1 || lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0) {
            rev |= 0x007e726d;
        // mdir?
        } else {
            rev |= 0x007e646d;
        }
    }
    #endif
    // xor in pseudorandom noise
    #ifdef LFS3_REVNOISE
    if (lfs3_m_isrevnoise(lfs3->flags)) {
        rev ^= ((1 << (28-lfs3_smax(lfs3->recycle_bits, 0)))-1) & lfs3->gcksum;
    }
    #endif
    return rev;
}
#endif

// btrees don't normally need revision counts, but we make use of them
// if revdbg or revnoise is enabled
#ifndef LFS3_RDONLY
static inline uint32_t lfs3_rev_btree(lfs3_t *lfs3) {
    (void)lfs3;
    uint32_t rev = 0;
    // include debug bits?
    #ifdef LFS3_REVDBG
    if (lfs3_m_isrevdbg(lfs3->flags)) {
        // mtree?
        if (lfs3_i_isinmtree(lfs3->flags)) {
            rev |= 0x007e6d62;
        // file btree?
        } else {
            rev |= 0x007e7462;
        }
    }
    #endif
    // xor in pseudorandom noise
    #ifdef LFS3_REVNOISE
    if (lfs3_m_isrevnoise(lfs3->flags)) {
        // keep the top nibble zero
        rev ^= 0x0fffffff & lfs3->gcksum;
    }
    #endif
    return rev;
}
#endif

#ifndef LFS3_RDONLY
static inline bool lfs3_rev_needsrelocation(lfs3_t *lfs3, uint32_t rev) {
    if (lfs3->recycle_bits == -1) {
        return false;
    }

    // does out recycle counter overflow?
    uint32_t rev_ = rev + (1 << (28-lfs3_smax(lfs3->recycle_bits, 0)));
    return (rev_ >> 28) != (rev >> 28);
}
#endif

#ifndef LFS3_RDONLY
static inline uint32_t lfs3_rev_inc(lfs3_t *lfs3, uint32_t rev) {
    // increment recycle counter/revision
    rev += 1 << (28-lfs3_smax(lfs3->recycle_bits, 0));
    // xor in pseudorandom noise
    #ifdef LFS3_REVNOISE
    if (lfs3_m_isrevnoise(lfs3->flags)) {
        rev ^= ((1 << (28-lfs3_smax(lfs3->recycle_bits, 0)))-1) & lfs3->gcksum;
    }
    #endif
    return rev;
}
#endif



/// Metadata pair stuff ///

// mdir convenience functions
#ifndef LFS3_RDONLY
static inline void lfs3_mdir_claim(lfs3_mdir_t *mdir) {
    lfs3_rbyd_claim(&mdir->r);
}
#endif

static inline int lfs3_mdir_cmp(const lfs3_mdir_t *a, const lfs3_mdir_t *b) {
    return lfs3_mptr_cmp(a->r.blocks, b->r.blocks);
}

static inline bool lfs3_mdir_ismrootanchor(const lfs3_mdir_t *mdir) {
    return lfs3_mptr_ismrootanchor(mdir->r.blocks);
}

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
        int err = lfs3_bd_read(lfs3, blocks[0], 0, 0,
                &revs[0], sizeof(uint32_t));
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }
        revs[i] = lfs3_fromle32(&revs[i]);

        if (i == 0
                || err == LFS3_ERR_CORRUPT
                || lfs3_scmp(revs[1], revs[0]) > 0) {
            LFS3_SWAP(lfs3_block_t, &blocks[0], &blocks[1]);
            LFS3_SWAP(uint32_t, &revs[0], &revs[1]);
        }
    }

    // try to fetch rbyds in the order of most recent to least recent
    for (int i = 0; i < 2; i++) {
        int err = lfs3_rbyd_fetch_(lfs3,
                &mdir->r, &mdir->gcksumdelta,
                blocks[0], 0);
        if (err && err != LFS3_ERR_CORRUPT) {
            return err;
        }

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
    if (lfs3_grm_ismidrm(lfs3, mid)) {
        return LFS3_TAG_ORPHAN;

    // if we find a stickynote, check to see if there are any open
    // in-sync file handles to decide if it really exists
    } else if (tag == LFS3_TAG_STICKYNOTE
            && !lfs3_omdir_ismidopen(lfs3, mid,
                ~LFS3_o_ZOMBIE & ~LFS3_O_DESYNC)) {
        return LFS3_TAG_ORPHAN;

    // map unknown types -> LFS3_TAG_UNKNOWN, this simplifies higher
    // levels and prevents collisions with internal types
    //
    // Note future types should probably come with WCOMPAT flags, and be
    // at least reported on non-supporting filesystems
    } else if (tag < LFS3_TAG_REG || tag > LFS3_TAG_BOOKMARK) {
        return LFS3_TAG_UNKNOWN;
    }

    return tag;
}

static int lfs3_mdir_lookupnext(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag,
        lfs3_tag_t *tag_, lfs3_data_t *data_) {
    lfs3_srid_t rid__;
    lfs3_tag_t tag__;
    int err = lfs3_rbyd_lookupnext(lfs3, &mdir->r,
            lfs3_mrid(lfs3, mdir->mid), tag,
            &rid__, &tag__, NULL, data_);
    if (err) {
        return err;
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

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}

static int lfs3_mdir_lookup(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag,
        lfs3_tag_t *tag_, lfs3_data_t *data_) {
    lfs3_tag_t tag__;
    int err = lfs3_mdir_lookupnext(lfs3, mdir, lfs3_tag_key(tag),
            &tag__, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    if ((tag__ & lfs3_tag_mask(tag)) != (tag & lfs3_tag_mask(tag))) {
        return LFS3_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}



/// Metadata-tree things ///

static inline lfs3_mid_t lfs3_mtree_weight(lfs3_t *lfs3) {
    return lfs3_max(
            LFS3_IFDEF_2BONLY(0, lfs3->mtree.weight),
            1 << lfs3->mbits);
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
    if (LFS3_IFDEF_2BONLY(0, lfs3->mtree.weight) == 0) {
        // treat inlined mdir as mid=0
        mdir_->mid = mid;
        lfs3_mdir_sync(mdir_, &lfs3->mroot);
        return 0;

    // look up mdir in actual mtree
    } else {
        #ifndef LFS3_2BONLY
        lfs3_bid_t bid;
        lfs3_srid_t rid;
        lfs3_tag_t tag;
        lfs3_bid_t weight;
        lfs3_data_t data;
        int err = lfs3_btree_lookupleaf(lfs3, &lfs3->mtree, mid,
                &bid, &mdir_->r, &rid, &tag, &weight, &data);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }
        LFS3_ASSERT((lfs3_sbid_t)bid == lfs3_mbid(lfs3, mid));
        LFS3_ASSERT(weight == (lfs3_bid_t)(1 << lfs3->mbits));
        LFS3_ASSERT(tag == LFS3_TAG_MNAME
                || tag == LFS3_TAG_MDIR);

        // if we found an mname, lookup the mdir
        if (tag == LFS3_TAG_MNAME) {
            err = lfs3_rbyd_lookup(lfs3, &mdir_->r, rid, LFS3_TAG_MDIR,
                    NULL, &data);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }
        }

        // fetch mdir
        return lfs3_data_fetchmdir(lfs3, &data, mid,
                mdir_);
        #endif
    }
}

// this is the same as lfs3_btree_commit, but we set the inmtree flag
// for debugging reasons
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static int lfs3_mtree_commit(lfs3_t *lfs3, lfs3_btree_t *mtree,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    #ifdef LFS3_REVDBG
    lfs3->flags |= LFS3_i_INMTREE;
    #endif
    int err = lfs3_btree_commit(lfs3, mtree, bid, rattrs, rattr_count);
    #ifdef LFS3_REVDBG
    lfs3->flags &= ~LFS3_i_INMTREE;
    #endif
    return err;
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
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static int lfs3_mdir_alloc__(lfs3_t *lfs3, lfs3_mdir_t *mdir,
        lfs3_smid_t mid, bool partial) {
    // assign the mid
    mdir->mid = mid;
    // default to zero gcksumdelta
    mdir->gcksumdelta = 0;

    if (!partial) {
        // allocate one block without an erase
        lfs3_sblock_t block = lfs3_alloc(lfs3, false);
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
    int err = lfs3_bd_read(lfs3, mdir->r.blocks[1], 0, 0,
            &rev, sizeof(uint32_t));
    if (err && err != LFS3_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    rev = (err != LFS3_ERR_CORRUPT) ? lfs3_fromle32(&rev) : 0;
    // reset recycle bits in revision count and increment
    rev = lfs3_rev_init(lfs3, mdir, rev);

relocate:;
    // allocate another block with an erase
    lfs3_sblock_t block = lfs3_alloc(lfs3, true);
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
    int err = lfs3_bd_read(lfs3, mdir->r.blocks[0], 0, 0,
            &rev, sizeof(uint32_t));
    if (err && err != LFS3_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    rev = (err != LFS3_ERR_CORRUPT) ? lfs3_fromle32(&rev) : 0;
    // increment our revision count
    rev = lfs3_rev_inc(lfs3, rev);

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
    err = lfs3_bd_erase(lfs3, mdir_->r.blocks[0]);
    if (err) {
        return err;
    }

    // increment our revision count and write it to our rbyd
    err = lfs3_rbyd_appendrev(lfs3, &mdir_->r, rev);
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
        lfs3_smid_t mid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    // since we only ever commit to one mid or split, we can ignore the
    // entire rattr-list if our mid is out of range
    lfs3_srid_t rid = lfs3_mrid(lfs3, mid);
    if (rid >= start_rid
            // note the use of rid+1 and unsigned comparison here to
            // treat end_rid=-1 as "unbounded" in such a way that rid=-1
            // is still included
            && (lfs3_size_t)(rid + 1) <= (lfs3_size_t)end_rid) {

        for (lfs3_size_t i = 0; i < rattr_count; i++) {
            // we just happen to never split in an mdir commit
            LFS3_ASSERT(!(i > 0 && lfs3_rattr_isinsert(rattrs[i])));

            // rattr lists can be chained, but only tail-recursively
            if (rattrs[i].tag == LFS3_TAG_RATTRS) {
                // must be the last tag
                LFS3_ASSERT(i == rattr_count-1);
                const lfs3_rattr_t *rattrs_ = rattrs[i].u.etc;
                lfs3_size_t rattr_count_ = rattrs[i].count;

                // switch to chained rattr-list
                rattrs = rattrs_;
                rattr_count = rattr_count_;
                i = -1;
                continue;

            // shrub tags append a set of attributes to an unrelated trunk
            // in our rbyd
            } else if (rattrs[i].tag == LFS3_TAG_SHRUBCOMMIT) {
                const lfs3_shrubcommit_t *shrubcommit = rattrs[i].u.etc;
                lfs3_bshrub_t *bshrub_ = shrubcommit->bshrub;
                lfs3_srid_t rid_ = shrubcommit->rid;
                const lfs3_rattr_t *rattrs_ = shrubcommit->rattrs;
                lfs3_size_t rattr_count_ = shrubcommit->rattr_count;

                // reset shrub if it doesn't live in our block, this happens
                // when converting from a btree
                if (!lfs3_bshrub_isbshrub(bshrub_)) {
                    bshrub_->shrub_.blocks[0] = mdir_->r.blocks[0];
                    bshrub_->shrub_.trunk = LFS3_RBYD_ISSHRUB | 0;
                    bshrub_->shrub_.weight = 0;
                }

                // commit to shrub
                int err = lfs3_shrub_commit(lfs3,
                        &mdir_->r, &bshrub_->shrub_,
                        rid_, rattrs_, rattr_count_);
                if (err) {
                    return err;
                }

            // push a new grm, this tag lets us push grms atomically when
            // creating new mids
            } else if (rattrs[i].tag == LFS3_TAG_GRMPUSH) {
                // do nothing here, this is handled up in lfs3_mdir_commit

            // move tags copy over any tags associated with the source's rid
            // TODO can this be deduplicated with lfs3_mdir_compact__ more?
            // it _really_ wants to be deduplicated
            } else if (rattrs[i].tag == LFS3_TAG_MOVE) {
                const lfs3_mdir_t *mdir__ = rattrs[i].u.etc;

                // skip the name tag, this is always replaced by upper layers
                lfs3_tag_t tag = LFS3_TAG_STRUCT-1;
                while (true) {
                    lfs3_data_t data;
                    int err = lfs3_mdir_lookupnext(lfs3, mdir__, tag+1,
                            &tag, &data);
                    if (err) {
                        if (err == LFS3_ERR_NOENT) {
                            break;
                        }
                        return err;
                    }

                    // found an inlined shrub? we need to compact the shrub
                    // as well to bring it along with us
                    if (tag == LFS3_TAG_BSHRUB) {
                        lfs3_shrub_t shrub;
                        err = lfs3_data_readshrub(lfs3, mdir__, &data,
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
                                LFS3_RATTR_SHRUB(LFS3_TAG_BSHRUB, 0, &shrub));
                        if (err) {
                            return err;
                        }

                    // append the rattr
                    } else {
                        err = lfs3_rbyd_appendrattr(lfs3, &mdir_->r,
                                rid - lfs3_smax(start_rid, 0),
                                LFS3_RATTR_DATA(tag, 0, &data));
                        if (err) {
                            return err;
                        }
                    }
                }

                // we're not quite done! we also need to bring over any
                // unsynced files
                for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
                    if (lfs3_o_isbshrub(o->flags)
                            // belongs to our mid?
                            && o->mdir.mid == mdir__->mid
                            // is a bshrub?
                            && lfs3_bshrub_isbshrub((lfs3_bshrub_t*)o)
                            // only compact once, first compact should
                            // stage the new block
                            && ((lfs3_bshrub_t*)o)->shrub_.blocks[0]
                                != mdir_->r.blocks[0]) {
                        int err = lfs3_shrub_compact(lfs3, &mdir_->r,
                                &((lfs3_bshrub_t*)o)->shrub_,
                                &((lfs3_bshrub_t*)o)->shrub);
                        if (err) {
                            return err;
                        }
                    }
                }

            // custom attributes need to be reencoded into our tag format
            } else if (rattrs[i].tag == LFS3_TAG_ATTRS) {
                const struct lfs3_attr *attrs_ = rattrs[i].u.etc;
                lfs3_size_t attr_count_ = rattrs[i].count;

                for (lfs3_size_t j = 0; j < attr_count_; j++) {
                    // skip readonly attrs and lazy attrs
                    if (lfs3_o_isrdonly(attrs_[j].flags)) {
                        continue;
                    }

                    // first lets check if the attr changed, we don't want
                    // to append attrs unless we have to
                    lfs3_data_t data;
                    int err = lfs3_mdir_lookup(lfs3, mdir_,
                            LFS3_TAG_ATTR(attrs_[j].type),
                            NULL, &data);
                    if (err && err != LFS3_ERR_NOENT) {
                        return err;
                    }

                    // does disk match our attr?
                    lfs3_scmp_t cmp = lfs3_attr_cmp(lfs3, &attrs_[j],
                            (err != LFS3_ERR_NOENT) ? &data : NULL);
                    if (cmp < 0) {
                        return cmp;
                    }

                    if (cmp == LFS3_CMP_EQ) {
                        continue;
                    }

                    // append the custom attr
                    err = lfs3_rbyd_appendrattr(lfs3, &mdir_->r,
                            rid - lfs3_smax(start_rid, 0),
                            // removing or updating?
                            (lfs3_attr_isnoattr(&attrs_[j]))
                                ? LFS3_RATTR(
                                    LFS3_TAG_RM
                                        | LFS3_TAG_ATTR(attrs_[j].type), 0)
                                : LFS3_RATTR_DATA(
                                    LFS3_TAG_ATTR(attrs_[j].type), 0,
                                    &LFS3_DATA_BUF(
                                        attrs_[j].buffer,
                                        lfs3_attr_size(&attrs_[j]))));
                    if (err) {
                        return err;
                    }
                }

            // write out normal tags normally
            } else {
                LFS3_ASSERT(!lfs3_tag_isinternal(rattrs[i].tag));

                int err = lfs3_rbyd_appendrattr(lfs3, &mdir_->r,
                        rid - lfs3_smax(start_rid, 0),
                        rattrs[i]);
                if (err) {
                    return err;
                }
            }

            // adjust rid
            rid = lfs3_rattr_nextrid(rattrs[i], rid);
        }
    }

    // abort the commit if our weight dropped to zero!
    //
    // If we finish the commit it becomes immediately visible, but we really
    // need to atomically remove this mdir from the mtree. Leave the actual
    // remove up to upper layers.
    if (mdir_->r.weight == 0
            // unless we are an mroot
            && !(mdir_->mid == -1
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

        int err = lfs3_rbyd_appendrattr_(lfs3, &mdir_->r, LFS3_RATTR_LE32(
                LFS3_TAG_GCKSUMDELTA, 0, mdir_->gcksumdelta));
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

        lfs3_tag_t tag = 0;
        lfs3_size_t dsize_ = 0;
        while (true) {
            lfs3_srid_t rid_;
            lfs3_data_t data;
            int err = lfs3_rbyd_lookupnext(lfs3, &mdir->r,
                    a_rid, tag+1,
                    &rid_, &tag, NULL, &data);
            if (err) {
                if (err == LFS3_ERR_NOENT) {
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
            // this is what would make lfs3_rbyd_estimate recursive, and
            // why we need a second function...
            //
            if (tag == LFS3_TAG_BSHRUB) {
                // include the cost of this trunk
                dsize_ += LFS3_SHRUB_DSIZE;

                lfs3_shrub_t shrub;
                err = lfs3_data_readshrub(lfs3, mdir, &data,
                        &shrub);
                if (err) {
                    return err;
                }

                lfs3_ssize_t dsize__ = lfs3_shrub_estimate(lfs3, &shrub);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += lfs3->rattr_estimate + dsize__;

            } else {
                // include the cost of this tag
                dsize_ += lfs3->mattr_estimate + lfs3_data_size(data);
            }
        }

        // include any opened+unsynced inlined files
        //
        // this is O(n^2), but littlefs is unlikely to have many open
        // files, I suppose if this becomes a problem we could sort
        // opened files by mid
        for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
            if (lfs3_o_isbshrub(o->flags)
                    // belongs to our mdir + rid?
                    && lfs3_mdir_cmp(&o->mdir, mdir) == 0
                    && lfs3_mrid(lfs3, o->mdir.mid) == a_rid
                    // is a bshrub?
                    && lfs3_bshrub_isbshrub((lfs3_bshrub_t*)o)) {
                lfs3_ssize_t dsize__ = lfs3_shrub_estimate(lfs3,
                        &((lfs3_bshrub_t*)o)->shrub);
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
    lfs3_srid_t rid = lfs3_smax(start_rid, -1);
    lfs3_tag_t tag = 0;
    while (true) {
        lfs3_rid_t weight;
        lfs3_data_t data;
        int err = lfs3_rbyd_lookupnext(lfs3, &mdir->r,
                rid, tag+1,
                &rid, &tag, &weight, &data);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
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
            err = lfs3_data_readshrub(lfs3, mdir, &data,
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
            err = lfs3_rbyd_appendcompactrattr(lfs3, &mdir_->r,
                    LFS3_RATTR_SHRUB(tag, weight, &shrub));
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                return err;
            }

        } else {
            // write the tag
            err = lfs3_rbyd_appendcompactrattr(lfs3, &mdir_->r,
                    LFS3_RATTR_DATA(tag, weight, &data));
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
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_o_isbshrub(o->flags)
                // belongs to our mdir?
                && lfs3_mdir_cmp(&o->mdir, mdir) == 0
                && lfs3_mrid(lfs3, o->mdir.mid) >= start_rid
                && (lfs3_rid_t)lfs3_mrid(lfs3, o->mdir.mid)
                    < (lfs3_rid_t)end_rid
                // is a bshrub?
                && lfs3_bshrub_isbshrub((lfs3_bshrub_t*)o)
                // only compact once, first compact should
                // stage the new block
                && ((lfs3_bshrub_t*)o)->shrub_.blocks[0]
                    != mdir_->r.blocks[0]) {
            int err = lfs3_shrub_compact(lfs3, &mdir_->r,
                    &((lfs3_bshrub_t*)o)->shrub_,
                    &((lfs3_bshrub_t*)o)->shrub);
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
        lfs3_smid_t mid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    // make a copy
    *mdir_ = *mdir;
    // mark our mdir as unerased in case we fail
    lfs3_mdir_claim(mdir);
    // mark any copies of our mdir as unerased in case we fail
    if (lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0) {
        lfs3_mdir_claim(&lfs3->mroot);
    }
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_mdir_cmp(&o->mdir, mdir) == 0) {
            lfs3_mdir_claim(&o->mdir);
        }
    }

    // try to commit
    int err = lfs3_mdir_commit__(lfs3, mdir_, start_rid, end_rid,
            mid, rattrs, rattr_count);
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

        // don't copy over gcksum if relocating
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
                mid, rattrs, rattr_count);
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
        #ifndef LFS3_2BONLY
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
        #else
        return LFS3_ERR_NOSPC;
        #endif
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
        err = lfs3_mdir_lookup(lfs3, &mdir, LFS3_TAG_MROOT,
                NULL, &data);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
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
        const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    // non-mroot mdirs must have weight
    LFS3_ASSERT(mdir->mid == -1
            // note inlined mdirs are mroots with mid != -1
            || lfs3_mdir_cmp(mdir, &lfs3->mroot) == 0
            || mdir->r.weight > 0);
    // rid in-bounds?
    LFS3_ASSERT(lfs3_mrid(lfs3, mdir->mid)
            <= (lfs3_srid_t)mdir->r.weight);
    // lfs3->mroot must have mid=-1
    LFS3_ASSERT(lfs3->mroot.mid == -1);

    // play out any rattrs that affect our grm _before_ committing to disk,
    // keep in mind we revert to on-disk gstate if we run into an error
    lfs3_smid_t mid_ = mdir->mid;
    for (lfs3_size_t i = 0; i < rattr_count; i++) {
        // push a new grm, this tag lets us push grms atomically when
        // creating new mids
        if (rattrs[i].tag == LFS3_TAG_GRMPUSH) {
            lfs3_grm_push(lfs3, mid_);

        // adjust pending grms?
        } else {
            for (int j = 0; j < 2; j++) {
                if (lfs3_mbid(lfs3, lfs3->grm.queue[j]) == lfs3_mbid(lfs3, mid_)
                        && lfs3->grm.queue[j] >= mid_) {
                    // deleting a pending grm doesn't really make sense
                    LFS3_ASSERT(lfs3->grm.queue[j] >= mid_ - rattrs[i].weight);

                    // adjust the grm
                    lfs3->grm.queue[j] += rattrs[i].weight;
                }
            }
        }

        // adjust mid
        mid_ = lfs3_rattr_nextrid(rattrs[i], mid_);
    }

    // flush gdeltas
    lfs3_fs_flushgdelta(lfs3);

    // xor our old cksum
    lfs3->gcksum ^= mdir->r.cksum;

    // stage any bshrubs
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_o_isbshrub(o->flags)) {
            // a bshrub outside of its mdir means something has gone
            // horribly wrong
            LFS3_ASSERT(!lfs3_bshrub_isbshrub((lfs3_bshrub_t*)o)
                    || ((lfs3_bshrub_t*)o)->shrub.blocks[0]
                        == o->mdir.r.blocks[0]);
            ((lfs3_bshrub_t*)o)->shrub_ = ((lfs3_bshrub_t*)o)->shrub;
        }
    }

    // attempt to commit/compact the mdir normally
    lfs3_mdir_t mdir_[2];
    lfs3_srid_t split_rid;
    int err = lfs3_mdir_commit_(lfs3, &mdir_[0], mdir, -2, -1,
            &split_rid,
            mdir->mid, rattrs, rattr_count);
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
    #ifndef LFS3_2BONLY
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
                    mdir->mid, rattrs, rattr_count);
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
        err = lfs3_rbyd_lookup(lfs3, &mdir_[1].r, 0,
                LFS3_TAG_MASK8 | LFS3_TAG_NAME,
                NULL, &split_name);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            goto failed;
        }

        // new mtree?
        if (lfs3->mtree.weight == 0) {
            lfs3_btree_init(&mtree_);

            err = lfs3_mtree_commit(lfs3, &mtree_,
                    0, LFS3_RATTRS(
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MDIR, +(1 << lfs3->mbits),
                            mdir_[0].r.blocks),
                        LFS3_RATTR_DATA(
                            LFS3_TAG_MNAME, +(1 << lfs3->mbits),
                            &split_name),
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MDIR, 0,
                            mdir_[1].r.blocks)));
            if (err) {
                goto failed;
            }

        // update our mtree
        } else {
            // mark as unerased in case of failure
            lfs3_btree_claim(&lfs3->mtree);

            err = lfs3_mtree_commit(lfs3, &mtree_,
                    lfs3_mbid(lfs3, mdir->mid), LFS3_RATTRS(
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MDIR, 0,
                            mdir_[0].r.blocks),
                        LFS3_RATTR_DATA(
                            LFS3_TAG_MNAME, +(1 << lfs3->mbits),
                            &split_name),
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MDIR, 0,
                            mdir_[1].r.blocks)));
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

        // mark as unerased in case of failure
        lfs3_btree_claim(&lfs3->mtree);

        // update our mtree
        err = lfs3_mtree_commit(lfs3, &mtree_,
                lfs3_mbid(lfs3, mdir->mid), LFS3_RATTRS(
                    LFS3_RATTR(
                        LFS3_TAG_RM, -(1 << lfs3->mbits))));
        if (err) {
            goto failed;
        }

    // need to relocate?
    } else if (lfs3_mdir_cmp(&mdir_[0], mdir) != 0
            && lfs3_mdir_cmp(mdir, &lfs3->mroot) != 0) {
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
                    0, LFS3_RATTRS(
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MDIR, +(1 << lfs3->mbits),
                            mdir_[0].r.blocks)));
            if (err) {
                goto failed;
            }

        // update our mtree
        } else {
            // mark as unerased in case of failure
            lfs3_btree_claim(&lfs3->mtree);

            err = lfs3_mtree_commit(lfs3, &mtree_,
                    lfs3_mbid(lfs3, mdir->mid), LFS3_RATTRS(
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MDIR, 0,
                            mdir_[0].r.blocks)));
            if (err) {
                goto failed;
            }
        }
    }
    #endif

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
        } else if (lfs3->grm.queue[j] > mdir->mid) {
            lfs3->grm.queue[j] += mdelta;
        }
    }

    // need to update mtree?
    #ifndef LFS3_2BONLY
    if (lfs3_btree_cmp(&mtree_, &lfs3->mtree) != 0) {
        // mtree should never go to zero since we always have a root bookmark
        LFS3_ASSERT(mtree_.weight > 0);

        // make sure mtree/mroot changes are on-disk before committing
        // metadata
        err = lfs3_bd_sync(lfs3);
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
                -1, LFS3_RATTRS(
                    LFS3_RATTR_BTREE(
                        LFS3_TAG_MASK8 | LFS3_TAG_MTREE, 0,
                        &mtree_),
                    // were we committing to the mroot? include any -1 rattrs
                    (mdir->mid == -1)
                        ? LFS3_RATTR_RATTRS(rattrs, rattr_count)
                        : LFS3_RATTR_NOOP()));
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_RANGE);
            goto failed;
        }
    }
    #endif

    // need to update mroot chain?
    if (lfs3_mdir_cmp(&mroot_, &lfs3->mroot) != 0) {
        // tail recurse, updating mroots until a commit sticks
        lfs3_mdir_t mrootchild = lfs3->mroot;
        lfs3_mdir_t mrootchild_ = mroot_;
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
            err = lfs3_bd_sync(lfs3);
            if (err) {
                goto failed;
            }

            // xor mrootparent's cksum
            lfs3->gcksum ^= mrootparent.r.cksum;

            // commit mrootchild
            lfs3_mdir_t mrootparent_;
            err = lfs3_mdir_commit_(lfs3, &mrootparent_, &mrootparent, -2, -1,
                    NULL,
                    -1, LFS3_RATTRS(
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MROOT, 0,
                            mrootchild_.r.blocks)));
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
            err = lfs3_bd_sync(lfs3);
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

            err = lfs3_mdir_commit__(lfs3, &mrootanchor_, -2, -1,
                    -1, LFS3_RATTRS(
                        LFS3_RATTR_BUF(
                            LFS3_TAG_MAGIC, 0,
                            "littlefs", 8),
                        LFS3_RATTR_MPTR(
                            LFS3_TAG_MROOT, 0,
                            mrootchild_.r.blocks)));
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
    err = lfs3_bd_sync(lfs3);
    if (err) {
        return err;
    }

    ///////////////////////////////////////////////////////////////////////
    // success? update in-device state, we must not error at this point! //
    ///////////////////////////////////////////////////////////////////////

    // play out any rattrs that affect internal state
    mid_ = mdir->mid;
    for (lfs3_size_t i = 0; i < rattr_count; i++) {
        // adjust any opened mdirs
        for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
            // adjust opened mdirs?
            if (lfs3_mdir_cmp(&o->mdir, mdir) == 0
                    && o->mdir.mid >= mid_) {
                // removed?
                if (o->mdir.mid < mid_ - rattrs[i].weight) {
                    // opened files should turn into stickynote, not
                    // be removed
                    LFS3_ASSERT(lfs3_o_type(o->flags) != LFS3_TYPE_REG);
                    o->flags |= LFS3_o_ZOMBIE;
                    o->mdir.mid = mid_;
                } else {
                    o->mdir.mid += rattrs[i].weight;
                }
            }
        }

        // adjust mid
        mid_ = lfs3_rattr_nextrid(rattrs[i], mid_);
    }

    // if mroot/mtree changed, clobber any mroot/mtree traversals
    #ifndef LFS3_2BONLY
    if (lfs3_mdir_cmp(&mroot_, &lfs3->mroot) != 0
            || lfs3_btree_cmp(&mtree_, &lfs3->mtree) != 0) {
        for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
            if (lfs3_o_type(o->flags) == LFS3_type_TRAVERSAL
                    && o->mdir.mid == -1
                    // don't clobber the current mdir, assume upper layers
                    // know what they're doing
                    && &o->mdir != mdir) {
                lfs3_traversal_clobber(lfs3, (lfs3_traversal_t*)o);
            }
        }
    }
    #endif

    // update internal mdir state
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        // avoid double updating the current mdir
        if (&o->mdir == mdir) {
            continue;
        }

        // update any splits/drops
        if (lfs3_mdir_cmp(&o->mdir, mdir) == 0) {
            if (mdelta > 0
                    && lfs3_mrid(lfs3, o->mdir.mid)
                        >= (lfs3_srid_t)mdir_[0].r.weight) {
                o->mdir.mid += (1 << lfs3->mbits) - mdir_[0].r.weight;
                lfs3_mdir_sync(&o->mdir, &mdir_[1]);
            } else {
                lfs3_mdir_sync(&o->mdir, &mdir_[0]);
            }
        } else if (o->mdir.mid > mdir->mid) {
            o->mdir.mid += mdelta;
        }
    }

    // update mdir to follow requested rid
    if (mdelta > 0
            && mdir->mid == -1) {
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
    #ifndef LFS3_2BONLY
    lfs3->mtree = mtree_;
    #endif

    // update any staged bshrubs
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        // if we moved a shrub, we also need to discard any related
        // leaves that moved
        #ifndef LFS3_KVONLY
        if (lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && lfs3_bptr_block(&((lfs3_file_t*)o)->leaf.bptr)
                    == ((lfs3_bshrub_t*)o)->shrub.blocks[0]
                && ((lfs3_bshrub_t*)o)->shrub_.blocks[0]
                    != ((lfs3_bshrub_t*)o)->shrub.blocks[0]) {
            lfs3_file_discardleaf((lfs3_file_t*)o);
        }
        #endif

        // update the shrub
        if (lfs3_o_isbshrub(o->flags)) {
            ((lfs3_bshrub_t*)o)->shrub = ((lfs3_bshrub_t*)o)->shrub_;
        }
    }

    // update any gstate changes
    lfs3_fs_commitgdelta(lfs3);

    // mark all traversals as dirty
    lfs3_fs_clobber(lfs3, LFS3_t_DIRTY);

    // we may have touched any number of mdirs, so assume uncompacted
    // until lfs3_fs_gc can prove otherwise
    lfs3->flags |= LFS3_I_COMPACT;

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
    // revert gstate to on-disk state
    lfs3_fs_revertgdelta(lfs3);
    return err;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mdir_compact(lfs3_t *lfs3, lfs3_mdir_t *mdir) {
    // the easiest way to do this is to just mark mdir as unerased
    // and call lfs3_mdir_commit
    lfs3_mdir_claim(mdir);
    return lfs3_mdir_commit(lfs3, mdir, NULL, 0);
}
#endif



/// Mtree path/name lookup ///

// lookup names in an mdir
//
// if not found, rid will be the best place to insert
static int lfs3_mdir_namelookup(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_smid_t *mid_, lfs3_tag_t *tag_, lfs3_data_t *data_) {
    // default to mid_ = 0, this blanket assignment is the only way to
    // keep GCC happy
    if (mid_) {
        *mid_ = 0;
    }

    // empty mdir?
    if (mdir->r.weight == 0) {
        return LFS3_ERR_NOENT;
    }

    lfs3_srid_t rid;
    lfs3_tag_t tag;
    lfs3_scmp_t cmp = lfs3_rbyd_namelookup(lfs3, &mdir->r,
            did, name, name_len,
            &rid, &tag, NULL, data_);
    if (cmp < 0) {
        LFS3_ASSERT(cmp != LFS3_ERR_NOENT);
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
    if (tag_) {
        *tag_ = tag;
    }
    return (cmp == LFS3_CMP_EQ) ? 0 : LFS3_ERR_NOENT;
}

// lookup names in our mtree
//
// if not found, rid will be the best place to insert
static int lfs3_mtree_namelookup(lfs3_t *lfs3,
        lfs3_did_t did, const char *name, lfs3_size_t name_len,
        lfs3_mdir_t *mdir_, lfs3_tag_t *tag_, lfs3_data_t *data_) {
    // do we only have mroot?
    if (LFS3_IFDEF_2BONLY(0, lfs3->mtree.weight) == 0) {
        // treat inlined mdir as mid=0
        mdir_->mid = 0;
        lfs3_mdir_sync(mdir_, &lfs3->mroot);

    // lookup name in actual mtree
    } else {
        #ifndef LFS3_2BONLY
        lfs3_bid_t bid;
        lfs3_srid_t rid;
        lfs3_tag_t tag;
        lfs3_bid_t weight;
        lfs3_data_t data;
        lfs3_scmp_t cmp = lfs3_btree_namelookupleaf(lfs3, &lfs3->mtree,
                did, name, name_len,
                &bid, &mdir_->r, &rid, &tag, &weight, &data);
        if (cmp < 0) {
            LFS3_ASSERT(cmp != LFS3_ERR_NOENT);
            return cmp;
        }
        LFS3_ASSERT(weight == (lfs3_bid_t)(1 << lfs3->mbits));
        LFS3_ASSERT(tag == LFS3_TAG_MNAME
                || tag == LFS3_TAG_MDIR);

        // if we found an mname, lookup the mdir
        if (tag == LFS3_TAG_MNAME) {
            int err = lfs3_rbyd_lookup(lfs3, &mdir_->r, rid, LFS3_TAG_MDIR,
                    NULL, &data);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }
        }

        // fetch mdir
        int err = lfs3_data_fetchmdir(lfs3, &data, bid-((1 << lfs3->mbits)-1),
                mdir_);
        if (err) {
            return err;
        }
        #endif
    }

    // and lookup name in our mdir
    lfs3_smid_t mid;
    int err = lfs3_mdir_namelookup(lfs3, mdir_, did, name, name_len,
            &mid, tag_, data_);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }

    // update mdir with best place to insert even if we fail
    mdir_->mid = mid;
    return err;
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
// - 0                                       => file found
// - 0, lfs3_path_isdir(path)                => dir found
// - 0, mdir.mid=-1                          => root found
// - LFS3_ERR_NOENT, lfs3_path_islast(path)  => file not found
// - LFS3_ERR_NOENT, !lfs3_path_islast(path) => parent not found
// - LFS3_ERR_NOTDIR                         => parent not a dir
//
// if not found, mdir/did_ will at least be set up with what should be
// the parent
//
static int lfs3_mtree_pathlookup(lfs3_t *lfs3, const char **path,
        lfs3_mdir_t *mdir_, lfs3_tag_t *tag_, lfs3_did_t *did_) {
    // setup root
    *mdir_ = lfs3->mroot;
    lfs3_tag_t tag = LFS3_TAG_DIR;
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
            if (tag_) {
                *tag_ = tag;
            }
            if (did_) {
                *did_ = did;
            }
            return 0;
        }

        // only continue if we hit a directory
        if (tag != LFS3_TAG_DIR) {
            return (tag == LFS3_TAG_ORPHAN)
                    ? LFS3_ERR_NOENT
                    : LFS3_ERR_NOTDIR;
        }

        // read the next did from the mdir if this is not the root
        if (mdir_->mid != -1) {
            lfs3_data_t data;
            int err = lfs3_mdir_lookup(lfs3, mdir_, LFS3_TAG_DID,
                    NULL, &data);
            if (err) {
                return err;
            }

            err = lfs3_data_readleb128(lfs3, &data, &did);
            if (err) {
                return err;
            }
        }

        // update path as we parse
        *path = path_;

        // lookup up this name in the mtree
        int err = lfs3_mtree_namelookup(lfs3, did, path_, name_len,
                mdir_, &tag, NULL);
        if (err && err != LFS3_ERR_NOENT) {
            return err;
        }

        // keep track of where to insert if we can't find path
        if (err == LFS3_ERR_NOENT) {
            if (tag_) {
                *tag_ = tag;
            }
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

// traversing littlefs is a bit complex, so we use a state machine to keep
// track of where we are
enum lfs3_tstate {
    LFS3_TSTATE_MROOTANCHOR = 0,
    #ifndef LFS3_2BONLY
    LFS3_TSTATE_MROOTCHAIN  = 1,
    LFS3_TSTATE_MTREE       = 2,
    LFS3_TSTATE_MDIRS       = 3,
    LFS3_TSTATE_MDIR        = 4,
    LFS3_TSTATE_BTREE       = 5,
    LFS3_TSTATE_OMDIRS      = 6,
    LFS3_TSTATE_OBTREE      = 7,
    #endif
    LFS3_TSTATE_DONE        = 8,
};

static void lfs3_traversal_init(lfs3_traversal_t *t, uint32_t flags) {
    t->b.o.flags = lfs3_o_typeflags(LFS3_type_TRAVERSAL)
            | lfs3_t_tstateflags(LFS3_TSTATE_MROOTANCHOR)
            | flags;
    t->b.o.mdir.mid = -1;
    t->b.o.mdir.r.weight = 0;
    t->b.o.mdir.r.blocks[0] = -1;
    t->b.o.mdir.r.blocks[1] = -1;
    lfs3_bshrub_init(&t->b);
    t->ot = NULL;
    t->u.mtortoise.blocks[0] = -1;
    t->u.mtortoise.blocks[1] = -1;
    t->u.mtortoise.step = 0;
    t->u.mtortoise.power = 0;
    t->gcksum = 0;
}

// low-level traversal _only_ finds blocks
static int lfs3_mtree_traverse_(lfs3_t *lfs3, lfs3_traversal_t *t,
        lfs3_tag_t *tag_, lfs3_bptr_t *bptr) {
    while (true) {
        switch (lfs3_t_tstate(t->b.o.flags)) {
        // start with the mrootanchor 0x{0,1}
        //
        // note we make sure to include all mroots in our mroot chain!
        //
        case LFS3_TSTATE_MROOTANCHOR:;
            // fetch the first mroot 0x{0,1}
            int err = lfs3_mdir_fetch(lfs3, &t->b.o.mdir,
                    -1, LFS3_MPTR_MROOTANCHOR());
            if (err) {
                return err;
            }

            // transition to traversing the mroot chain
            lfs3_t_settstate(&t->b.o.flags, LFS3_IFDEF_2BONLY(
                    LFS3_TSTATE_DONE,
                    LFS3_TSTATE_MROOTCHAIN));

            if (tag_) {
                *tag_ = LFS3_TAG_MDIR;
            }
            bptr->d.u.buffer = (const uint8_t*)&t->b.o.mdir;
            return 0;

        // traverse the mroot chain, checking for mroots/mtrees
        #ifndef LFS3_2BONLY
        case LFS3_TSTATE_MROOTCHAIN:;
            // lookup mroot, if we find one this is not the active mroot
            lfs3_tag_t tag;
            lfs3_data_t data;
            err = lfs3_mdir_lookup(lfs3, &t->b.o.mdir,
                    LFS3_TAG_MASK8 | LFS3_TAG_STRUCT,
                    &tag, &data);
            if (err) {
                // if we have no mtree (inlined mdir), we need to
                // traverse any files in our mroot next
                if (err == LFS3_ERR_NOENT) {
                    t->b.o.mdir.mid = 0;
                    lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIR);
                    continue;
                }
                return err;
            }

            // found a new mroot
            if (tag == LFS3_TAG_MROOT) {
                // fetch this mroot
                err = lfs3_data_fetchmdir(lfs3, &data, -1,
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
                if (lfs3_mptr_cmp(
                        t->b.o.mdir.r.blocks,
                        t->u.mtortoise.blocks) == 0) {
                    LFS3_ERROR("Cycle detected during mtree traversal "
                                "0x{%"PRIx32",%"PRIx32"}",
                            t->b.o.mdir.r.blocks[0],
                            t->b.o.mdir.r.blocks[1]);
                    return LFS3_ERR_CORRUPT;
                }
                if (t->u.mtortoise.step == (1U << t->u.mtortoise.power)) {
                    t->u.mtortoise.blocks[0] = t->b.o.mdir.r.blocks[0];
                    t->u.mtortoise.blocks[1] = t->b.o.mdir.r.blocks[1];
                    t->u.mtortoise.step = 0;
                    t->u.mtortoise.power += 1;
                }
                t->u.mtortoise.step += 1;

                if (tag_) {
                    *tag_ = LFS3_TAG_MDIR;
                }
                bptr->d.u.buffer = (const uint8_t*)&t->b.o.mdir;
                return 0;

            // found an mtree?
            } else if (tag == LFS3_TAG_MTREE) {
                // fetch the root of the mtree
                err = lfs3_data_fetchbtree(lfs3, &data,
                        &t->b.shrub);
                if (err) {
                    return err;
                }

                // transition to traversing the mtree
                lfs3_btraversal_init(&t->u.bt);
                lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MTREE);
                continue;

            } else {
                LFS3_ERROR("Weird mroot entry? 0x%"PRIx32, tag);
                return LFS3_ERR_CORRUPT;
            }
            LFS3_UNREACHABLE();
        #endif

        // iterate over mdirs in the mtree
        #ifndef LFS3_2BONLY
        case LFS3_TSTATE_MDIRS:;
            // find the next mdir
            err = lfs3_mtree_lookup(lfs3, t->b.o.mdir.mid,
                    &t->b.o.mdir);
            if (err) {
                // end of mtree? guess we're done
                if (err == LFS3_ERR_NOENT) {
                    lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_DONE);
                    continue;
                }
                return err;
            }

            // transition to traversing the mdir
            lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIR);

            if (tag_) {
                *tag_ = LFS3_TAG_MDIR;
            }
            bptr->d.u.buffer = (const uint8_t*)&t->b.o.mdir;
            return 0;
        #endif

        // scan for blocks/btrees in the current mdir
        #ifndef LFS3_2BONLY
        case LFS3_TSTATE_MDIR:;
            // not traversing all blocks? have we exceeded our mdir's weight?
            // return to mtree iteration
            if (lfs3_t_ismtreeonly(t->b.o.flags)
                    || lfs3_mrid(lfs3, t->b.o.mdir.mid)
                        >= (lfs3_srid_t)t->b.o.mdir.r.weight) {
                t->b.o.mdir.mid = lfs3_mbid(lfs3, t->b.o.mdir.mid) + 1;
                lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIRS);
                continue;
            }

            // do we have a bshrub/btree?
            err = lfs3_bshrub_fetch(lfs3, &t->b);
            if (err && err != LFS3_ERR_NOENT) {
                return err;
            }

            // found a bshrub/btree? note we may also run into dirs/dids
            // here, lfs3_bshrub_fetch ignores these for us
            if (err != LFS3_ERR_NOENT) {
                // start traversing
                lfs3_btraversal_init(&t->u.bt);
                lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_BTREE);
                continue;

            // no? next we need to check any opened files
            } else {
                t->ot = lfs3->omdirs;
                lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_OMDIRS);
                continue;
            }
            LFS3_UNREACHABLE();
        #endif

        // scan for blocks/btrees in our opened file list
        #ifndef LFS3_2BONLY
        case LFS3_TSTATE_OMDIRS:;
            // reached end of opened files? return to mdir traversal
            //
            // note we can skip checking opened files if mounted rdonly,
            // this saves a bit of code when compiled rdonly
            if (lfs3_m_isrdonly(lfs3->flags) || !t->ot) {
                t->ot = NULL;
                t->b.o.mdir.mid += 1;
                lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIR);
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
                    || lfs3_o_type(t->ot->flags) != LFS3_TYPE_REG
                    || !lfs3_o_isunsync(t->ot->flags)) {
                t->ot = t->ot->next;
                continue;
            }

            // transition to traversing the file
            const lfs3_file_t *file = (const lfs3_file_t*)t->ot;
            t->b.shrub = file->b.shrub;
            lfs3_btraversal_init(&t->u.bt);
            lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_OBTREE);
            continue;
        #endif

        // traverse any bshrubs/btrees we see, this includes the mtree
        // and any file btrees/bshrubs
        #ifndef LFS3_2BONLY
        case LFS3_TSTATE_MTREE:;
        case LFS3_TSTATE_BTREE:;
        case LFS3_TSTATE_OBTREE:;
            // traverse through our bshrub/btree
            err = lfs3_bshrub_traverse(lfs3, &t->b, &t->u.bt,
                    NULL, &tag, NULL, &data);
            if (err) {
                if (err == LFS3_ERR_NOENT) {
                    // clear the bshrub state
                    lfs3_bshrub_init(&t->b);
                    // end of mtree? start iterating over mdirs
                    if (lfs3_t_tstate(t->b.o.flags)
                            == LFS3_TSTATE_MTREE) {
                        t->b.o.mdir.mid = 0;
                        lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIRS);
                        continue;
                    // end of mdir btree? start iterating over opened files
                    } else if (lfs3_t_tstate(t->b.o.flags)
                            == LFS3_TSTATE_BTREE) {
                        t->ot = lfs3->omdirs;
                        lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_OMDIRS);
                        continue;
                    // end of opened btree? go to next opened file
                    } else if (lfs3_m_isrdonly(lfs3->flags)
                            || lfs3_t_tstate(t->b.o.flags)
                                == LFS3_TSTATE_OBTREE) {
                        t->ot = t->ot->next;
                        lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_OMDIRS);
                        continue;
                    } else {
                        LFS3_UNREACHABLE();
                    }
                }
                return err;
            }

            // found an inner btree node?
            if (tag == LFS3_TAG_BRANCH) {
                if (tag_) {
                    *tag_ = tag;
                }
                bptr->d = data;
                return 0;

            // found an indirect block?
            } else if (LFS3_IFDEF_2BONLY(false, tag == LFS3_TAG_BLOCK)) {
                #ifndef LFS3_2BONLY
                if (tag_) {
                    *tag_ = tag;
                }
                err = lfs3_data_readbptr(lfs3, &data,
                        bptr);
                if (err) {
                    return err;
                }
                return 0;
                #endif
            }

            continue;
        #endif

        case LFS3_TSTATE_DONE:;
            return LFS3_ERR_NOENT;

        default:;
            LFS3_UNREACHABLE();
        }
    }
}

// needed in lfs3_mtree_traverse
static void lfs3_alloc_markinuse(lfs3_t *lfs3,
        lfs3_tag_t tag, const lfs3_bptr_t *bptr);

// high-level immutable traversal, handle extra features here,
// but no mutation! (we're called in lfs3_alloc, so things would end up
// recursive, which would be a bit bad!)
static int lfs3_mtree_traverse(lfs3_t *lfs3, lfs3_traversal_t *t,
        lfs3_tag_t *tag_, lfs3_bptr_t *bptr) {
    lfs3_tag_t tag;
    int err = lfs3_mtree_traverse_(lfs3, t,
            &tag, bptr);
    if (err) {
        // end of traversal?
        if (err == LFS3_ERR_NOENT) {
            goto eot;
        }
        return err;
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
    if ((lfs3_t_isckmeta(t->b.o.flags)
                || lfs3_t_isckdata(t->b.o.flags))
            && tag == LFS3_TAG_MDIR) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr->d.u.buffer;

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
        for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
            if (lfs3_mdir_cmp(&o->mdir, mdir) == 0
                    && o->mdir.r.cksum != mdir->r.cksum) {
                LFS3_ERROR("Found mdir cksum mismatch %"PRId32" "
                            "0x{%"PRIx32",%"PRIx32"}, "
                            "cksum %08"PRIx32" (!= %08"PRIx32")",
                        lfs3_dbgmbid(lfs3, mdir->mid),
                        mdir->r.blocks[0],
                        mdir->r.blocks[1],
                        mdir->r.cksum,
                        o->mdir.r.cksum);
                return LFS3_ERR_CORRUPT;
            }
        }

        // recalculate gcksum
        t->gcksum ^= mdir->r.cksum;
    }

    // validate btree nodes?
    //
    // this may end up revalidating some btree nodes when ckfetches
    // is enabled, but we need to revalidate cached btree nodes or
    // we risk missing errors in ckmeta scans
    if ((lfs3_t_isckmeta(t->b.o.flags)
                || lfs3_t_isckdata(t->b.o.flags))
            && tag == LFS3_TAG_BRANCH) {
        lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr->d.u.buffer;
        err = lfs3_rbyd_fetchck(lfs3, rbyd,
                rbyd->blocks[0], rbyd->trunk,
                rbyd->cksum);
        if (err) {
            return err;
        }
    }

    // validate data blocks?
    #ifndef LFS3_2BONLY
    if (lfs3_t_isckdata(t->b.o.flags)
            && tag == LFS3_TAG_BLOCK) {
        err = lfs3_bptr_ck(lfs3, bptr);
        if (err) {
            return err;
        }
    }
    #endif

    if (tag_) {
        *tag_ = tag;
    }
    return 0;

eot:;
    // compare gcksum with in-RAM gcksum
    if ((lfs3_t_isckmeta(t->b.o.flags)
                || lfs3_t_isckdata(t->b.o.flags))
            && !lfs3_t_isdirty(t->b.o.flags)
            && !lfs3_t_ismutated(t->b.o.flags)
            && t->gcksum != lfs3->gcksum) {
        LFS3_ERROR("Found gcksum mismatch, cksum %08"PRIx32" (!= %08"PRIx32")",
                t->gcksum,
                lfs3->gcksum);
        return LFS3_ERR_CORRUPT;
    }

    // was ckmeta/ckdata successful? we only consider our filesystem
    // checked if we weren't mutated
    if ((lfs3_t_isckmeta(t->b.o.flags)
                || lfs3_t_isckdata(t->b.o.flags))
            && !lfs3_t_ismtreeonly(t->b.o.flags)
            && !lfs3_t_isdirty(t->b.o.flags)
            && !lfs3_t_ismutated(t->b.o.flags)) {
        lfs3->flags &= ~LFS3_I_CKMETA;
    }
    if (lfs3_t_isckdata(t->b.o.flags)
            && !lfs3_t_ismtreeonly(t->b.o.flags)
            && !lfs3_t_isdirty(t->b.o.flags)
            && !lfs3_t_ismutated(t->b.o.flags)) {
        lfs3->flags &= ~LFS3_I_CKDATA;
    }

    return LFS3_ERR_NOENT;
}

// needed in lfs3_mtree_gc
static int lfs3_mdir_mkconsistent(lfs3_t *lfs3, lfs3_mdir_t *mdir);
static void lfs3_alloc_ckpoint(lfs3_t *lfs3);
static void lfs3_alloc_markfree(lfs3_t *lfs3);

// high-level mutating traversal, handle extra features that require
// mutation here, upper layers should call lfs3_alloc_ckpoint as needed
static int lfs3_mtree_gc(lfs3_t *lfs3, lfs3_traversal_t *t,
        lfs3_tag_t *tag_, lfs3_bptr_t *bptr) {
dropped:;
    lfs3_tag_t tag;
    int err = lfs3_mtree_traverse(lfs3, t,
            &tag, bptr);
    if (err) {
        // end of traversal?
        if (err == LFS3_ERR_NOENT) {
            goto eot;
        }
        // don't goto failed here, we haven't swapped dirty/mutated
        // flags yet
        return err;
    }

    // swap dirty/mutated flags while in lfs3_mtree_gc
    #ifndef LFS3_RDONLY
    t->b.o.flags = lfs3_t_swapdirty(t->b.o.flags);

    // track in-use blocks?
    #ifndef LFS3_2BONLY
    if (lfs3_t_islookahead(t->b.o.flags)) {
        lfs3_alloc_markinuse(lfs3, tag, bptr);
    }
    #endif

    // mkconsistencing mdirs?
    if (lfs3_t_ismkconsistent(t->b.o.flags)
            && lfs3_t_ismkconsistent(lfs3->flags)
            && tag == LFS3_TAG_MDIR) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr->d.u.buffer;
        err = lfs3_mdir_mkconsistent(lfs3, mdir);
        if (err) {
            goto failed;
        }

        // make sure we clear any zombie flags
        t->b.o.flags &= ~LFS3_o_ZOMBIE;

        // did this drop our mdir?
        #ifndef LFS3_2BONLY
        if (mdir->mid != -1 && mdir->r.weight == 0) {
            // swap back dirty/mutated flags
            t->b.o.flags = lfs3_t_swapdirty(t->b.o.flags);
            // continue traversal
            lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIRS);
            goto dropped;
        }
        #endif
    }

    // compacting mdirs?
    if (lfs3_t_iscompact(t->b.o.flags)
            && tag == LFS3_TAG_MDIR
            // exceed compaction threshold?
            && lfs3_rbyd_eoff(&((lfs3_mdir_t*)bptr->d.u.buffer)->r)
                > ((lfs3->cfg->gc_compact_thresh)
                    ? lfs3->cfg->gc_compact_thresh
                    : lfs3->cfg->block_size - lfs3->cfg->block_size/8)) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr->d.u.buffer;
        LFS3_INFO("Compacting mdir %"PRId32" 0x{%"PRIx32",%"PRIx32"} "
                    "(%"PRId32" > %"PRId32")",
                lfs3_dbgmbid(lfs3, mdir->mid),
                mdir->r.blocks[0],
                mdir->r.blocks[1],
                lfs3_rbyd_eoff(&mdir->r),
                (lfs3->cfg->gc_compact_thresh)
                    ? lfs3->cfg->gc_compact_thresh
                    : lfs3->cfg->block_size - lfs3->cfg->block_size/8);

        // checkpoint the allocator
        lfs3_alloc_ckpoint(lfs3);
        // compact the mdir
        err = lfs3_mdir_compact(lfs3, mdir);
        if (err) {
            goto failed;
        }
    }

    // swap back dirty/mutated flags
    t->b.o.flags = lfs3_t_swapdirty(t->b.o.flags);
    #endif
    if (tag_) {
        *tag_ = tag;
    }
    return 0;

    #ifndef LFS3_RDONLY
failed:;
    // swap back dirty/mutated flags
    t->b.o.flags = lfs3_t_swapdirty(t->b.o.flags);
    return err;
    #endif

eot:;
    #ifndef LFS3_RDONLY
    // was lookahead scan successful?
    #ifndef LFS3_2BONLY
    if (lfs3_t_islookahead(t->b.o.flags)
            && !lfs3_t_ismtreeonly(t->b.o.flags)
            && !lfs3_t_isdirty(t->b.o.flags)
            && !lfs3_t_ismutated(t->b.o.flags)) {
        lfs3_alloc_markfree(lfs3);
    }
    #endif

    // was mkconsistent successful?
    if (lfs3_t_ismkconsistent(t->b.o.flags)
            && !lfs3_t_isdirty(t->b.o.flags)) {
        lfs3->flags &= ~LFS3_I_MKCONSISTENT;
    }

    // was compaction successful? note we may need multiple passes if
    // we want to be sure everything is compacted
    if (lfs3_t_iscompact(t->b.o.flags)
            && !lfs3_t_isdirty(t->b.o.flags)
            && !lfs3_t_ismutated(t->b.o.flags)) {
        lfs3->flags &= ~LFS3_I_COMPACT;
    }
    #endif

    return LFS3_ERR_NOENT;
}



/// Block allocator ///

// checkpoint the allocator
//
// operations that need to alloc should call this to indicate all in-use
// blocks are either committed into the filesystem or tracked by an opened
// mdir
#if !defined(LFS3_RDONLY)
static void lfs3_alloc_ckpoint(lfs3_t *lfs3) {
    #ifndef LFS3_2BONLY
    lfs3->lookahead.ckpoint = lfs3->block_count;
    #else
    (void)lfs3;
    #endif
}
#endif

// discard any lookahead state, this is necessary if block_count changes
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static void lfs3_alloc_discard(lfs3_t *lfs3) {
    lfs3->lookahead.size = 0;
    lfs3_memset(lfs3->lookahead.buffer, 0, lfs3->cfg->lookahead_size);
}
#endif

// mark a block as in-use
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static void lfs3_alloc_markinuse_(lfs3_t *lfs3, lfs3_block_t block) {
    // translate to lookahead-relative
    lfs3_block_t block_ = ((
                (lfs3_sblock_t)(block
                        - (lfs3->lookahead.window + lfs3->lookahead.off))
            // we only need this mess because C's mod is actually rem, and
            // we want real mod in case block_ goes negative
                    % (lfs3_sblock_t)lfs3->block_count)
                + (lfs3_sblock_t)lfs3->block_count)
            % (lfs3_sblock_t)lfs3->block_count;

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
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static void lfs3_alloc_markinuse(lfs3_t *lfs3,
        lfs3_tag_t tag, const lfs3_bptr_t *bptr) {
    if (tag == LFS3_TAG_MDIR) {
        lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr->d.u.buffer;
        lfs3_alloc_markinuse_(lfs3, mdir->r.blocks[0]);
        lfs3_alloc_markinuse_(lfs3, mdir->r.blocks[1]);

    } else if (tag == LFS3_TAG_BRANCH) {
        lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr->d.u.buffer;
        lfs3_alloc_markinuse_(lfs3, rbyd->blocks[0]);

    } else if (tag == LFS3_TAG_BLOCK) {
        lfs3_alloc_markinuse_(lfs3, lfs3_bptr_block(bptr));

    } else {
        LFS3_UNREACHABLE();
    }
}
#endif

// needed in lfs3_alloc_markfree
static lfs3_sblock_t lfs3_alloc_findfree(lfs3_t *lfs3);

// mark any not-in-use blocks as free
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static void lfs3_alloc_markfree(lfs3_t *lfs3) {
    // make lookahead buffer usable
    lfs3->lookahead.size = lfs3_min(
            8*lfs3->cfg->lookahead_size,
            lfs3->lookahead.ckpoint);

    // signal that lookahead is full, this may be cleared by
    // lfs3_alloc_findfree
    lfs3->flags &= ~LFS3_I_LOOKAHEAD;

    // eagerly find the next free block so lookahead scans can make
    // the most progress
    lfs3_alloc_findfree(lfs3);
}
#endif

// increment lookahead buffer
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static void lfs3_alloc_inc(lfs3_t *lfs3) {
    LFS3_ASSERT(lfs3->lookahead.size > 0);

    // clear lookahead as we increment
    lfs3->lookahead.buffer[lfs3->lookahead.off / 8]
            &= ~(1 << (lfs3->lookahead.off % 8));

    // signal that lookahead is no longer full
    lfs3->flags |= LFS3_I_LOOKAHEAD;

    // increment next/off
    lfs3->lookahead.off += 1;
    if (lfs3->lookahead.off == 8*lfs3->cfg->lookahead_size) {
        lfs3->lookahead.off = 0;
        lfs3->lookahead.window = (lfs3->lookahead.window
                + 8*lfs3->cfg->lookahead_size)
                    % lfs3->block_count;
    }

    // decrement size/ckpoint
    lfs3->lookahead.size -= 1;
    lfs3->lookahead.ckpoint -= 1;
}
#endif

// find next free block in lookahead buffer, if there is one
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static lfs3_sblock_t lfs3_alloc_findfree(lfs3_t *lfs3) {
    while (lfs3->lookahead.size > 0) {
        if (!(lfs3->lookahead.buffer[lfs3->lookahead.off / 8]
                & (1 << (lfs3->lookahead.off % 8)))) {
            // found a free block
            return (lfs3->lookahead.window + lfs3->lookahead.off)
                    % lfs3->block_count;
        }

        lfs3_alloc_inc(lfs3);
    }

    return LFS3_ERR_NOSPC;
}
#endif

// needed in lfs3_mtree_traverse_
static inline lfs3_size_t lfs3_graft_count(lfs3_size_t graft_count);

#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
static lfs3_sblock_t lfs3_alloc(lfs3_t *lfs3, bool erase) {
    while (true) {
        // scan our lookahead buffer for free blocks
        lfs3_sblock_t block = lfs3_alloc_findfree(lfs3);
        if (block < 0 && block != LFS3_ERR_NOSPC) {
            return block;
        }

        if (block != LFS3_ERR_NOSPC) {
            // we should never alloc blocks {0,1}
            LFS3_ASSERT(block != 0 && block != 1);

            // erase requested?
            if (erase) {
                int err = lfs3_bd_erase(lfs3, block);
                if (err) {
                    // bad erase? try another block
                    if (err == LFS3_ERR_CORRUPT) {
                        lfs3_alloc_inc(lfs3);
                        continue;
                    }
                    return err;
                }
            }

            // eagerly find the next free block to maximize how many blocks
            // lfs3_alloc_ckpoint makes available for scanning
            lfs3_alloc_inc(lfs3);
            lfs3_alloc_findfree(lfs3);

            #ifdef LFS3_DBGALLOCS
            LFS3_DEBUG("Allocated block 0x%"PRIx32", "
                        "lookahead %"PRId32"/%"PRId32"/%"PRId32,
                    block,
                    lfs3->lookahead.size,
                    lfs3->lookahead.ckpoint,
                    lfs3->cfg->block_count);
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
        if (lfs3->lookahead.ckpoint <= 0) {
            LFS3_ERROR("No more free space "
                        "(lookahead %"PRId32"/%"PRId32"/%"PRId32")",
                    lfs3->lookahead.size,
                    lfs3->lookahead.ckpoint,
                    lfs3->cfg->block_count);
            return LFS3_ERR_NOSPC;
        }

        // no blocks in our lookahead buffer?
        //
        // traverse the filesystem, building up knowledge of what blocks are
        // in-use in the next lookahead window
        //
        lfs3_traversal_t t;
        lfs3_traversal_init(&t, LFS3_T_RDONLY | LFS3_T_LOOKAHEAD);
        while (true) {
            lfs3_tag_t tag;
            lfs3_bptr_t bptr;
            int err = lfs3_mtree_traverse(lfs3, &t,
                    &tag, &bptr);
            if (err) {
                if (err == LFS3_ERR_NOENT) {
                    break;
                }
                return err;
            }

            // track in-use blocks
            lfs3_alloc_markinuse(lfs3, tag, &bptr);
        }

        // mask out any in-flight graft state
        for (lfs3_size_t i = 0;
                i < lfs3_graft_count(lfs3->graft_count);
                i++) {
            lfs3_alloc_markinuse_(lfs3, lfs3->graft[i].u.disk.block);
        }

        // mark anything not seen as free
        lfs3_alloc_markfree(lfs3);
    }
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
    lfs3_tag_t tag;
    lfs3_did_t did;
    err = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, &tag, &did);
    if (err && !(err == LFS3_ERR_NOENT && lfs3_path_islast(path))) {
        return err;
    }
    // already exists? pretend orphans don't exist
    bool exists = (err != LFS3_ERR_NOENT);
    if (exists && tag != LFS3_TAG_ORPHAN) {
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
        err = lfs3_mtree_namelookup(lfs3, did_, NULL, 0,
                &mdir, NULL, NULL);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
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
    // This is done automatically by lfs3_mdir_commit to avoid issues with
    // mid updates, since the mid technically doesn't exist yet...

    // commit our bookmark and a grm to self-remove in case of powerloss
    lfs3_alloc_ckpoint(lfs3);
    err = lfs3_mdir_commit(lfs3, &mdir, LFS3_RATTRS(
            LFS3_RATTR_NAME(
                LFS3_TAG_BOOKMARK, +1, did_, NULL, 0),
            LFS3_RATTR(
                LFS3_TAG_GRMPUSH, 0)));
    if (err) {
        return err;
    }
    LFS3_ASSERT(lfs3->grm.queue[0] == mdir.mid);

    // committing our bookmark may have changed the mid of our metadata entry,
    // we need to look it up again, we can at least avoid the full path walk
    err = lfs3_mtree_namelookup(lfs3, did, name, name_len,
            &mdir, NULL, NULL);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }
    LFS3_ASSERT((exists) ? !err : err == LFS3_ERR_NOENT);

    // commit our new directory into our parent, zeroing the grm in the
    // process
    lfs3_grm_pop(lfs3);
    lfs3_alloc_ckpoint(lfs3);
    err = lfs3_mdir_commit(lfs3, &mdir, LFS3_RATTRS(
            LFS3_RATTR_NAME(
                LFS3_TAG_MASK12 | LFS3_TAG_DIR, (!exists) ? +1 : 0,
                did, name, name_len),
            LFS3_RATTR_LEB128(
                LFS3_TAG_DID, 0, did_)));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        // mark any clobbered uncreats as zombied
        if (exists
                && lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == mdir.mid) {
            o->flags = (o->flags & ~LFS3_o_UNCREAT)
                    | LFS3_o_ZOMBIE
                    | LFS3_o_UNSYNC
                    | LFS3_O_DESYNC;

        // update dir positions
        } else if (!exists
                && lfs3_o_type(o->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)o)->did == did
                && o->mdir.mid >= mdir.mid) {
            ((lfs3_dir_t*)o)->pos += 1;
        }
    }

    return 0;
}
#endif

// push a did to grm, but only if the directory is empty
#ifndef LFS3_RDONLY
static int lfs3_grm_pushdid(lfs3_t *lfs3, lfs3_did_t did) {
    // first lookup the bookmark entry
    lfs3_mdir_t bookmark_mdir;
    int err = lfs3_mtree_namelookup(lfs3, did, NULL, 0,
            &bookmark_mdir, NULL, NULL);
    if (err) {
        LFS3_ASSERT(err != LFS3_ERR_NOENT);
        return err;
    }
    lfs3_mid_t bookmark_mid = bookmark_mdir.mid;

    // check that the directory is empty
    bookmark_mdir.mid += 1;
    if (lfs3_mrid(lfs3, bookmark_mdir.mid)
            >= (lfs3_srid_t)bookmark_mdir.r.weight) {
        err = lfs3_mtree_lookup(lfs3,
                lfs3_mbid(lfs3, bookmark_mdir.mid-1) + 1,
                &bookmark_mdir);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                goto empty;
            }
            return err;
        }
    }

    lfs3_data_t data;
    err = lfs3_mdir_lookup(lfs3, &bookmark_mdir,
            LFS3_TAG_MASK8 | LFS3_TAG_NAME,
            NULL, &data);
    if (err) {
        LFS3_ASSERT(err != LFS3_ERR_NOENT);
        return err;
    }

    lfs3_did_t did_;
    err = lfs3_data_readleb128(lfs3, &data, &did_);
    if (err) {
        return err;
    }

    if (did_ == did) {
        return LFS3_ERR_NOTEMPTY;
    }

empty:;
    lfs3_grm_push(lfs3, bookmark_mid);
    return 0;
}
#endif

// needed in lfs3_remove
static int lfs3_fs_fixgrm(lfs3_t *lfs3);

#ifndef LFS3_RDONLY
int lfs3_remove(lfs3_t *lfs3, const char *path) {
    // prepare our filesystem for writing
    int err = lfs3_fs_mkconsistent(lfs3);
    if (err) {
        return err;
    }

    // lookup our entry
    lfs3_mdir_t mdir;
    lfs3_tag_t tag;
    lfs3_did_t did;
    err = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, &tag, &did);
    if (err) {
        return err;
    }
    // pretend orphans don't exist
    if (tag == LFS3_TAG_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // trying to remove the root dir?
    if (mdir.mid == -1) {
        return LFS3_ERR_INVAL;
    }

    // if we're removing a directory, we need to also remove the
    // bookmark entry
    lfs3_did_t did_ = 0;
    if (tag == LFS3_TAG_DIR) {
        // first lets figure out the did
        lfs3_data_t data;
        err = lfs3_mdir_lookup(lfs3, &mdir, LFS3_TAG_DID,
                NULL, &data);
        if (err) {
            return err;
        }

        err = lfs3_data_readleb128(lfs3, &data, &did_);
        if (err) {
            return err;
        }

        // mark bookmark for removal with grm
        err = lfs3_grm_pushdid(lfs3, did_);
        if (err) {
            return err;
        }
    }

    // are we removing an opened file?
    bool zombie = lfs3_omdir_ismidopen(lfs3, mdir.mid, -1);

    // remove the metadata entry
    lfs3_alloc_ckpoint(lfs3);
    err = lfs3_mdir_commit(lfs3, &mdir, LFS3_RATTRS(
            // create a stickynote if zombied
            //
            // we use a create+delete here to also clear any rattrs
            // and trim the entry size
            (zombie)
                ? LFS3_RATTR_NAME(
                    LFS3_TAG_MASK12 | LFS3_TAG_STICKYNOTE, 0,
                    did, path, lfs3_path_namelen(path))
                : LFS3_RATTR(
                    LFS3_TAG_RM, -1)));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        // mark any clobbered uncreats as zombied
        if (zombie
                && lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == mdir.mid) {
            o->flags |= LFS3_o_UNCREAT
                    | LFS3_o_ZOMBIE
                    | LFS3_o_UNSYNC
                    | LFS3_O_DESYNC;

        // mark any removed dirs as zombied
        } else if (did_
                && lfs3_o_type(o->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)o)->did == did_) {
            o->flags |= LFS3_o_ZOMBIE;

        // update dir positions
        } else if (lfs3_o_type(o->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)o)->did == did
                && o->mdir.mid >= mdir.mid) {
            if (lfs3_o_iszombie(o->flags)) {
                o->flags &= ~LFS3_o_ZOMBIE;
            } else {
                ((lfs3_dir_t*)o)->pos -= 1;
            }

        // clobber entangled traversals
        } else if (lfs3_o_type(o->flags) == LFS3_type_TRAVERSAL) {
            if (lfs3_o_iszombie(o->flags)) {
                o->flags &= ~LFS3_o_ZOMBIE;
                o->mdir.mid -= 1;
                lfs3_traversal_clobber(lfs3, (lfs3_traversal_t*)o);
            }
        }
    }

    // if we were a directory, we need to clean up, fortunately we can leave
    // this up to lfs3_fs_fixgrm
    err = lfs3_fs_fixgrm(lfs3);
    if (err) {
        // we did complete the remove, so we shouldn't error here, best
        // we can do is log this
        LFS3_WARN("Failed to clean up grm (%d)", err);
    }

    return 0;
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
    lfs3_tag_t old_tag;
    lfs3_did_t old_did;
    err = lfs3_mtree_pathlookup(lfs3, &old_path,
            &old_mdir, &old_tag, &old_did);
    if (err) {
        return err;
    }
    // pretend orphans don't exist
    if (old_tag == LFS3_TAG_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // trying to rename the root?
    if (old_mdir.mid == -1) {
        return LFS3_ERR_INVAL;
    }

    // lookup new entry
    lfs3_mdir_t new_mdir;
    lfs3_tag_t new_tag;
    lfs3_did_t new_did;
    err = lfs3_mtree_pathlookup(lfs3, &new_path,
            &new_mdir, &new_tag, &new_did);
    if (err && !(err == LFS3_ERR_NOENT && lfs3_path_islast(new_path))) {
        return err;
    }
    bool exists = (err != LFS3_ERR_NOENT);

    // there are a few cases we need to watch out for
    lfs3_did_t new_did_ = 0;
    if (!exists) {
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
            return LFS3_ERR_INVAL;
        }

        // we allow reg <-> stickynote renaming, but renaming a non-dir
        // to a dir and a dir to a non-dir is an error
        if (old_tag != LFS3_TAG_DIR && new_tag == LFS3_TAG_DIR) {
            return LFS3_ERR_ISDIR;
        }
        if (old_tag == LFS3_TAG_DIR
                && new_tag != LFS3_TAG_DIR
                // pretend orphans don't exist
                && new_tag != LFS3_TAG_ORPHAN) {
            return LFS3_ERR_NOTDIR;
        }

        // renaming to ourself is a noop
        if (old_mdir.mid == new_mdir.mid) {
            return 0;
        }

        // if our destination is a directory, we will be implicitly removing
        // the directory, we need to create a grm for this
        if (new_tag == LFS3_TAG_DIR) {
            // TODO deduplicate the isempty check with lfs3_remove?
            // first lets figure out the did
            lfs3_data_t data;
            err = lfs3_mdir_lookup(lfs3, &new_mdir, LFS3_TAG_DID,
                    NULL, &data);
            if (err) {
                return err;
            }

            err = lfs3_data_readleb128(lfs3, &data, &new_did_);
            if (err) {
                return err;
            }

            // mark bookmark for removal with grm
            err = lfs3_grm_pushdid(lfs3, new_did_);
            if (err) {
                return err;
            }
        }
    }

    if (old_tag == LFS3_TAG_UNKNOWN) {
        // lookup the actual tag
        err = lfs3_rbyd_lookup(lfs3, &old_mdir.r,
                lfs3_mrid(lfs3, old_mdir.mid), LFS3_TAG_MASK8 | LFS3_TAG_NAME,
                &old_tag, NULL);
        if (err) {
            return err;
        }
    }

    // mark old entry for removal with a grm
    lfs3_grm_push(lfs3, old_mdir.mid);

    // rename our entry, copying all tags associated with the old rid to the
    // new rid, while also marking the old rid for removal
    lfs3_alloc_ckpoint(lfs3);
    err = lfs3_mdir_commit(lfs3, &new_mdir, LFS3_RATTRS(
            LFS3_RATTR_NAME(
                LFS3_TAG_MASK12 | old_tag, (!exists) ? +1 : 0,
                new_did, new_path, lfs3_path_namelen(new_path)),
            LFS3_RATTR_MOVE(&old_mdir)));
    if (err) {
        return err;
    }

    // update in-device state
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        // mark any clobbered uncreats as zombied
        if (exists
                && lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == new_mdir.mid) {
            o->flags = (o->flags & ~LFS3_o_UNCREAT)
                    | LFS3_o_ZOMBIE
                    | LFS3_o_UNSYNC
                    | LFS3_O_DESYNC;

        // update moved files with the new mdir
        } else if (lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == lfs3->grm.queue[0]) {
            o->mdir = new_mdir;

        // mark any removed dirs as zombied
        } else if (new_did_
                && lfs3_o_type(o->flags) == LFS3_TYPE_DIR
                && ((lfs3_dir_t*)o)->did == new_did_) {
            o->flags |= LFS3_o_ZOMBIE;

        // update dir positions
        } else if (lfs3_o_type(o->flags) == LFS3_TYPE_DIR) {
            if (!exists
                    && ((lfs3_dir_t*)o)->did == new_did
                    && o->mdir.mid >= new_mdir.mid) {
                ((lfs3_dir_t*)o)->pos += 1;
            }

            if (((lfs3_dir_t*)o)->did == old_did
                    && o->mdir.mid >= lfs3->grm.queue[0]) {
                if (o->mdir.mid == lfs3->grm.queue[0]) {
                    o->mdir.mid += 1;
                } else {
                    ((lfs3_dir_t*)o)->pos -= 1;
                }
            }

        // clobber entangled traversals
        } else if (lfs3_o_type(o->flags) == LFS3_type_TRAVERSAL
                && ((exists && o->mdir.mid == new_mdir.mid)
                    || o->mdir.mid == lfs3->grm.queue[0])) {
            lfs3_traversal_clobber(lfs3, (lfs3_traversal_t*)o);
        }
    }

    // we need to clean up any pending grms, fortunately we can leave
    // this up to lfs3_fs_fixgrm
    err = lfs3_fs_fixgrm(lfs3);
    if (err) {
        // we did complete the remove, so we shouldn't error here, best
        // we can do is log this
        LFS3_WARN("Failed to clean up grm (%d)", err);
    }

    return 0;
}
#endif

// this just populates the info struct based on what we found
static int lfs3_stat_(lfs3_t *lfs3, const lfs3_mdir_t *mdir,
        lfs3_tag_t tag, lfs3_data_t name,
        struct lfs3_info *info) {
    // get file type from the tag
    info->type = lfs3_tag_subtype(tag);

    // read the file name
    LFS3_ASSERT(lfs3_data_size(name) <= LFS3_NAME_MAX);
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
        lfs3_tag_t tag;
        lfs3_data_t data;
        int err = lfs3_mdir_lookup(lfs3, mdir,
                LFS3_TAG_MASK8 | LFS3_TAG_STRUCT,
                &tag, &data);
        if (err && err != LFS3_ERR_NOENT) {
            return err;
        }

        if (err != LFS3_ERR_NOENT) {
            // in bshrubs/btrees, size is always the first field
            err = lfs3_data_readleb128(lfs3, &data, &info->size);
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
    lfs3_tag_t tag;
    int err = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, &tag, NULL);
    if (err) {
        return err;
    }
    // pretend orphans don't exist
    if (tag == LFS3_TAG_ORPHAN) {
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
    LFS3_ASSERT(!lfs3_omdir_isopen(lfs3, &dir->o));

    // setup dir state
    dir->o.flags = lfs3_o_typeflags(LFS3_TYPE_DIR);

    // lookup our directory
    lfs3_mdir_t mdir;
    lfs3_tag_t tag;
    int err = lfs3_mtree_pathlookup(lfs3, &path,
            &mdir, &tag, NULL);
    if (err) {
        return err;
    }
    // pretend orphans don't exist
    if (tag == LFS3_TAG_ORPHAN) {
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

        lfs3_data_t data;
        err = lfs3_mdir_lookup(lfs3, &mdir, LFS3_TAG_DID,
                NULL, &data);
        if (err) {
            return err;
        }

        err = lfs3_data_readleb128(lfs3, &data, &dir->did);
        if (err) {
            return err;
        }
    }

    // let rewind initialize the pos state
    err = lfs3_dir_rewind_(lfs3, dir);
    if (err) {
        return err;
    }

    // add to tracked mdirs
    lfs3_omdir_open(lfs3, &dir->o);
    return 0;
}

int lfs3_dir_close(lfs3_t *lfs3, lfs3_dir_t *dir) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &dir->o));

    // remove from tracked mdirs
    lfs3_omdir_close(lfs3, &dir->o);
    return 0;
}

int lfs3_dir_read(lfs3_t *lfs3, lfs3_dir_t *dir, struct lfs3_info *info) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &dir->o));

    // was our dir removed?
    if (lfs3_o_iszombie(dir->o.flags)) {
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
        if (lfs3_mrid(lfs3, dir->o.mdir.mid)
                >= (lfs3_srid_t)dir->o.mdir.r.weight) {
            int err = lfs3_mtree_lookup(lfs3,
                    lfs3_mbid(lfs3, dir->o.mdir.mid-1) + 1,
                    &dir->o.mdir);
            if (err) {
                return err;
            }
        }

        // lookup the next name tag
        lfs3_tag_t tag;
        lfs3_data_t data;
        int err = lfs3_mdir_lookup(lfs3, &dir->o.mdir,
                LFS3_TAG_MASK8 | LFS3_TAG_NAME,
                &tag, &data);
        if (err) {
            return err;
        }

        // get the did
        lfs3_did_t did;
        err = lfs3_data_readleb128(lfs3, &data, &did);
        if (err) {
            return err;
        }

        // did mismatch? this terminates the dir read
        if (did != dir->did) {
            return LFS3_ERR_NOENT;
        }

        // skip orphans, we pretend these don't exist
        if (tag == LFS3_TAG_ORPHAN) {
            dir->o.mdir.mid += 1;
            dir->pos += 1;
            continue;
        }

        // fill out our info struct
        err = lfs3_stat_(lfs3, &dir->o.mdir, tag, data,
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

int lfs3_dir_seek(lfs3_t *lfs3, lfs3_dir_t *dir, lfs3_soff_t off) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &dir->o));

    // do nothing if removed
    if (lfs3_o_iszombie(dir->o.flags)) {
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
        if (lfs3_mrid(lfs3, dir->o.mdir.mid)
                >= (lfs3_srid_t)dir->o.mdir.r.weight) {
            int err = lfs3_mtree_lookup(lfs3,
                    lfs3_mbid(lfs3, dir->o.mdir.mid-1) + 1,
                    &dir->o.mdir);
            if (err) {
                if (err == LFS3_ERR_NOENT) {
                    break;
                }
                return err;
            }
        }

        lfs3_off_t d = lfs3_min(
                off_,
                dir->o.mdir.r.weight
                    - lfs3_mrid(lfs3, dir->o.mdir.mid));
        dir->o.mdir.mid += d;
        off_ -= d;
    }

    dir->pos = off;
    return 0;
}

lfs3_soff_t lfs3_dir_tell(lfs3_t *lfs3, lfs3_dir_t *dir) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &dir->o));

    return dir->pos;
}

static int lfs3_dir_rewind_(lfs3_t *lfs3, lfs3_dir_t *dir) {
    // do nothing if removed
    if (lfs3_o_iszombie(dir->o.flags)) {
        return 0;
    }

    // lookup our bookmark in the mtree
    int err = lfs3_mtree_namelookup(lfs3, dir->did, NULL, 0,
            &dir->o.mdir, NULL, NULL);
    if (err) {
        LFS3_ASSERT(err != LFS3_ERR_NOENT);
        return err;
    }

    // eagerly set to next entry
    dir->o.mdir.mid += 1;
    // reset pos
    dir->pos = 0;
    return 0;
}

int lfs3_dir_rewind(lfs3_t *lfs3, lfs3_dir_t *dir) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &dir->o));

    return lfs3_dir_rewind_(lfs3, dir);
}




/// Custom attribute stuff ///

static int lfs3_lookupattr(lfs3_t *lfs3, const char *path, uint8_t type,
        lfs3_mdir_t *mdir_, lfs3_data_t *data_) {
    // lookup our entry
    lfs3_tag_t tag;
    int err = lfs3_mtree_pathlookup(lfs3, &path,
            mdir_, &tag, NULL);
    if (err) {
        return err;
    }
    // pretend orphans don't exist
    if (tag == LFS3_TAG_ORPHAN) {
        return LFS3_ERR_NOENT;
    }

    // lookup our attr
    err = lfs3_mdir_lookup(lfs3, mdir_, LFS3_TAG_ATTR(type),
            NULL, data_);
    if (err) {
        if (err == LFS3_ERR_NOENT) {
            return LFS3_ERR_NOATTR;
        }
        return err;
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
    return lfs3_data_size(data);
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

    // commit our attr
    lfs3_alloc_ckpoint(lfs3);
    err = lfs3_mdir_commit(lfs3, &mdir, LFS3_RATTRS(
            LFS3_RATTR_DATA(
                LFS3_TAG_ATTR(type), 0,
                &LFS3_DATA_BUF(
                    buffer, size))));
    if (err) {
        return err;
    }

    // update any opened files tracking custom attrs
    #ifndef LFS3_KVONLY
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (!(lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == mdir.mid
                && !lfs3_o_isdesync(o->flags))) {
            continue;
        }

        lfs3_file_t *file = (lfs3_file_t*)o;
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
    #endif

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

    // commit our removal
    lfs3_alloc_ckpoint(lfs3);
    err = lfs3_mdir_commit(lfs3, &mdir, LFS3_RATTRS(
            LFS3_RATTR(
                LFS3_TAG_RM | LFS3_TAG_ATTR(type), 0)));
    if (err) {
        return err;
    }

    // update any opened files tracking custom attrs
    #ifndef LFS3_KVONLY
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (!(lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == mdir.mid
                && !lfs3_o_isdesync(o->flags))) {
            continue;
        }

        lfs3_file_t *file = (lfs3_file_t*)o;
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
    #endif

    return 0;
}
#endif




/// File operations ///

// file helpers

static inline void lfs3_file_discardcache(lfs3_file_t *file) {
    file->b.o.flags &= ~LFS3_o_UNFLUSH;
    #ifndef LFS3_KVONLY
    file->cache.pos = 0;
    #endif
    file->cache.size = 0;
}

#ifndef LFS3_KVONLY
static inline void lfs3_file_discardleaf(lfs3_file_t *file) {
    file->b.o.flags &= ~LFS3_o_UNCRYST;
    file->leaf.pos = 0;
    file->leaf.weight = 0;
    lfs3_bptr_discard(&file->leaf.bptr);
}
#endif

static inline void lfs3_file_discardbshrub(lfs3_file_t *file) {
    lfs3_bshrub_init(&file->b);
}

static inline lfs3_size_t lfs3_file_cachesize(lfs3_t *lfs3,
        const lfs3_file_t *file) {
    return (file->cfg->cache_buffer || file->cfg->cache_size)
            ? file->cfg->cache_size
            : lfs3->cfg->file_cache_size;
}

static inline lfs3_off_t lfs3_file_size_(const lfs3_file_t *file) {
    return lfs3_max(
            LFS3_IFDEF_KVONLY(0, file->cache.pos) + file->cache.size,
            file->b.shrub.weight);
}



// file operations

static void lfs3_file_init(lfs3_file_t *file, uint32_t flags,
        const struct lfs3_file_config *cfg) {
    file->cfg = cfg;
    file->b.o.flags = lfs3_o_typeflags(LFS3_TYPE_REG) | flags;
    #ifndef LFS3_KVONLY
    file->pos = 0;
    #endif
    lfs3_file_discardcache(file);
    #ifndef LFS3_KVONLY
    lfs3_file_discardleaf(file);
    #endif
    lfs3_file_discardbshrub(file);
}

static int lfs3_file_fetch(lfs3_t *lfs3, lfs3_file_t *file, uint32_t flags) {
    // don't bother reading disk if we're not created or truncating
    if (!lfs3_o_isuncreat(flags) && !lfs3_o_istrunc(flags)) {
        // fetch the file's bshrub/btree, if there is one
        int err = lfs3_bshrub_fetch(lfs3, &file->b);
        if (err && err != LFS3_ERR_NOENT) {
            return err;
        }

        // mark as in-sync
        file->b.o.flags &= ~LFS3_o_UNSYNC;
    }

    // try to fetch any custom attributes
    #ifndef LFS3_KVONLY
    for (lfs3_size_t i = 0; i < file->cfg->attr_count; i++) {
        // skip writeonly attrs
        if (lfs3_o_iswronly(file->cfg->attrs[i].flags)) {
            continue;
        }

        // don't bother reading disk if we're not created yet
        if (lfs3_o_isuncreat(flags)) {
            if (file->cfg->attrs[i].size) {
                *file->cfg->attrs[i].size = LFS3_ERR_NOATTR;
            }
            continue;
        }

        // lookup the attr
        lfs3_data_t data;
        int err = lfs3_mdir_lookup(lfs3, &file->b.o.mdir,
                LFS3_TAG_ATTR(file->cfg->attrs[i].type),
                NULL, &data);
        if (err && err != LFS3_ERR_NOENT) {
            return err;
        }

        // read the attr, if it exists
        if (err == LFS3_ERR_NOENT
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
    #endif

    return 0;
}

// needed in lfs3_file_opencfg
static void lfs3_file_close_(lfs3_t *lfs3, const lfs3_file_t *file);
#ifndef LFS3_RDONLY
static int lfs3_file_sync_(lfs3_t *lfs3, lfs3_file_t *file,
        const lfs3_name_t *name);
#endif
static int lfs3_file_ck(lfs3_t *lfs3, const lfs3_file_t *file,
        uint32_t flags);

int lfs3_file_opencfg_(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs3_file_config *cfg) {
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
    // wrset is a special lfs3_set specific mode that passes data via
    // the file cache, so make sure not to clobber it
    if (lfs3_o_iswrset(file->b.o.flags)) {
        file->b.o.flags |= LFS3_o_UNFLUSH;
        file->cache.buffer = file->cfg->cache_buffer;
        #ifndef LFS3_KVONLY
        file->cache.pos = 0;
        #endif
        file->cache.size = file->cfg->cache_size;
    } else if (file->cfg->cache_buffer) {
        file->cache.buffer = file->cfg->cache_buffer;
    } else {
        #ifndef LFS3_KVONLY
        file->cache.buffer = lfs3_malloc(lfs3_file_cachesize(lfs3, file));
        if (!file->cache.buffer) {
            return LFS3_ERR_NOMEM;
        }
        #else
        LFS3_UNREACHABLE();
        #endif
    }

    // lookup our parent
    lfs3_tag_t tag;
    lfs3_did_t did;
    int err = lfs3_mtree_pathlookup(lfs3, &path,
            &file->b.o.mdir, &tag, &did);
    if (err && !(err == LFS3_ERR_NOENT && lfs3_path_islast(path))) {
        goto failed;
    }
    bool exists = (err != LFS3_ERR_NOENT);

    // creating a new entry?
    if (!exists || tag == LFS3_TAG_ORPHAN) {
        if (!lfs3_o_iscreat(file->b.o.flags)) {
            err = LFS3_ERR_NOENT;
            goto failed;
        }
        LFS3_ASSERT(!lfs3_o_isrdonly(file->b.o.flags));

        #ifndef LFS3_RDONLY
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
        if (exists) {
            file->b.o.flags |= LFS3_o_UNCREAT | LFS3_o_UNSYNC;
        }
        #endif
    } else {
        // wanted to create a new entry?
        if (lfs3_o_isexcl(file->b.o.flags)) {
            err = LFS3_ERR_EXIST;
            goto failed;
        }

        // wrong type?
        if (tag == LFS3_TAG_DIR) {
            err = LFS3_ERR_ISDIR;
            goto failed;
        }
        if (tag == LFS3_TAG_UNKNOWN) {
            err = LFS3_ERR_NOTSUP;
            goto failed;
        }

        #ifndef LFS3_RDONLY
        // if stickynote, mark as uncreated + unsync
        if (tag == LFS3_TAG_STICKYNOTE) {
            file->b.o.flags |= LFS3_o_UNCREAT | LFS3_o_UNSYNC;
        }

        // if truncating, mark as unsync
        if (lfs3_o_istrunc(file->b.o.flags)) {
            file->b.o.flags |= LFS3_o_UNSYNC;
        }
        #endif
    }

    // need to create an entry?
    #ifndef LFS3_RDONLY
    if (!exists) {
        // small file wrset? can we atomically commit everything in one
        // commit? currently this is only possible via lfs3_set
        if (lfs3_o_iswrset(file->b.o.flags)
                && file->cache.size <= lfs3->cfg->inline_size
                && file->cache.size <= lfs3->cfg->fragment_size
                && file->cache.size < lfs3->cfg->crystal_thresh) {
            // we need to mark as unsync for sync to do anything
            file->b.o.flags |= LFS3_o_UNSYNC;

            err = lfs3_file_sync_(lfs3, file, &(lfs3_name_t){
                    .did=did,
                    .name=path,
                    .name_len=lfs3_path_namelen(path)});
            if (err) {
                goto failed;
            }

        } else {
            // create a stickynote entry if we don't have one, this
            // reserves the mid until first sync
            lfs3_alloc_ckpoint(lfs3);
            err = lfs3_mdir_commit(lfs3, &file->b.o.mdir, LFS3_RATTRS(
                    LFS3_RATTR_NAME(
                        LFS3_TAG_STICKYNOTE, +1,
                        did, path, lfs3_path_namelen(path))));
            if (err) {
                goto failed;
            }

            // mark as uncreated + unsync
            file->b.o.flags |= LFS3_o_UNCREAT | LFS3_o_UNSYNC;
        }

        // update dir positions
        for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
            if (lfs3_o_type(o->flags) == LFS3_TYPE_DIR
                    && ((lfs3_dir_t*)o)->did == did
                    && o->mdir.mid >= file->b.o.mdir.mid) {
                ((lfs3_dir_t*)o)->pos += 1;
            }
        }
    }
    #endif

    // fetch the file struct and custom attrs
    err = lfs3_file_fetch(lfs3, file, file->b.o.flags);
    if (err) {
        goto failed;
    }

    // check metadata/data for errors?
    #if !defined(LFS3_KVONLY) && !defined(LFS3_2BONLY)
    if (lfs3_t_isckmeta(file->b.o.flags)
            || lfs3_t_isckdata(file->b.o.flags)) {
        err = lfs3_file_ck(lfs3, file, file->b.o.flags);
        if (err) {
            goto failed;
        }
    }
    #endif

    // add to tracked mdirs
    lfs3_omdir_open(lfs3, &file->b.o);
    return 0;

failed:;
    // clean up resources
    lfs3_file_close_(lfs3, file);
    return err;
}

int lfs3_file_opencfg(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs3_file_config *cfg) {
    // already open?
    LFS3_ASSERT(!lfs3_omdir_isopen(lfs3, &file->b.o));
    // don't allow the forbidden mode!
    LFS3_ASSERT((flags & 3) != 3);
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
    // these flags require a writable file
    LFS3_ASSERT(!lfs3_o_isrdonly(flags) || !lfs3_o_iscreat(flags));
    LFS3_ASSERT(!lfs3_o_isrdonly(flags) || !lfs3_o_isexcl(flags));
    LFS3_ASSERT(!lfs3_o_isrdonly(flags) || !lfs3_o_istrunc(flags));
    #ifndef LFS3_KVONLY
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
static const struct lfs3_file_config lfs3_file_defaultcfg = {0};

int lfs3_file_open(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags) {
    return lfs3_file_opencfg(lfs3, file, path, flags,
            &lfs3_file_defaultcfg);
}

// clean up resources
static void lfs3_file_close_(lfs3_t *lfs3, const lfs3_file_t *file) {
    (void)lfs3;
    // clean up memory
    if (!file->cfg->cache_buffer) {
        lfs3_free(file->cache.buffer);
    }

    // are we orphaning a file?
    //
    // make sure we check _after_ removing ourselves
    #ifndef LFS3_RDONLY
    if (lfs3_o_isuncreat(file->b.o.flags)
            && !lfs3_omdir_ismidopen(lfs3, file->b.o.mdir.mid, -1)) {
        // this can only happen in a rdwr filesystem
        LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));

        // this gets a bit messy, since we're not able to write to the
        // filesystem if we're rdonly or desynced, fortunately we have
        // a few tricks

        // first try to push onto our grm queue
        if (lfs3_grm_count(lfs3) < 2) {
            lfs3_grm_push(lfs3, file->b.o.mdir.mid);

        // fallback to just marking the filesystem as inconsistent
        } else {
            lfs3->flags |= LFS3_I_MKCONSISTENT;
        }
    }
    #endif
}

// needed in lfs3_file_close
#ifdef LFS3_KVONLY
static
#endif
int lfs3_file_sync(lfs3_t *lfs3, lfs3_file_t *file);

int lfs3_file_close(lfs3_t *lfs3, lfs3_file_t *file) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    // don't call lfs3_file_sync if we're readonly or desynced
    int err = 0;
    if (!lfs3_o_isrdonly(file->b.o.flags)
            && !lfs3_o_isdesync(file->b.o.flags)) {
        err = lfs3_file_sync(lfs3, file);
    }

    // remove from tracked mdirs
    lfs3_omdir_close(lfs3, &file->b.o);

    // clean up resources
    lfs3_file_close_(lfs3, file);

    return err;
}

// low-level file reading

static int lfs3_file_lookupnext(lfs3_t *lfs3, const lfs3_file_t *file,
        lfs3_bid_t bid,
        lfs3_bid_t *bid_, lfs3_bid_t *weight_, lfs3_bptr_t *bptr_) {
    lfs3_tag_t tag;
    lfs3_bid_t weight;
    lfs3_data_t data;
    int err = lfs3_bshrub_lookupnext(lfs3, &file->b, bid,
            bid_, &tag, &weight, &data);
    if (err) {
        return err;
    }
    LFS3_ASSERT(tag == LFS3_TAG_DATA
            || tag == LFS3_TAG_BLOCK);

    // fetch the bptr/data fragment
    err = lfs3_bptr_fetch(lfs3, bptr_, tag, weight, data);
    if (err) {
        return err;
    }

    if (weight_) {
        *weight_ = weight;
    }
    return 0;
}

#ifndef LFS3_KVONLY
static lfs3_ssize_t lfs3_file_readnext(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t pos, uint8_t *buffer, lfs3_size_t size) {
    // the leaf must not be pinned down here
    LFS3_ASSERT(!lfs3_o_isuncryst(file->b.o.flags));

    while (true) {
        // any data in our leaf?
        if (pos >= file->leaf.pos
                && pos < file->leaf.pos + file->leaf.weight) {
            // any data on disk?
            lfs3_off_t pos_ = pos;
            if (pos_ < file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr)) {
                // note one important side-effect here is a strict
                // data hint
                lfs3_ssize_t d = lfs3_min(
                        size,
                        lfs3_bptr_size(&file->leaf.bptr)
                            - (pos_ - file->leaf.pos));
                lfs3_data_t slice = LFS3_DATA_SLICE(file->leaf.bptr.d,
                        pos_ - file->leaf.pos,
                        d);
                d = lfs3_data_read(lfs3, &slice,
                        buffer, d);
                if (d < 0) {
                    return d;
                }

                pos_ += d;
                buffer += d;
                size -= d;
            }

            // found a hole? fill with zeros
            lfs3_ssize_t d = lfs3_min(
                    size,
                    file->leaf.pos+file->leaf.weight - pos_);
            lfs3_memset(buffer, 0, d);

            pos_ += d;
            buffer += d;
            size -= d;

            return pos_ - pos;
        }

        // fetch a new leaf
        lfs3_bid_t bid;
        lfs3_bid_t weight;
        lfs3_bptr_t bptr;
        int err = lfs3_file_lookupnext(lfs3, file, pos,
                &bid, &weight, &bptr);
        if (err) {
            return err;
        }

        file->leaf.pos = bid - (weight-1);
        file->leaf.weight = weight;
        file->leaf.bptr = bptr;
    }
}
#endif

// high-level file reading

#ifdef LFS3_KVONLY
// a simpler read if we only read files once
static lfs3_ssize_t lfs3_file_readget_(lfs3_t *lfs3, lfs3_file_t *file,
        void *buffer, lfs3_size_t size) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));
    // can't read from writeonly files
    LFS3_ASSERT(!lfs3_o_iswronly(file->b.o.flags));
    LFS3_ASSERT(size <= 0x7fffffff);

    lfs3_off_t pos_ = 0;
    uint8_t *buffer_ = buffer;
    while (size > 0 && pos_ < lfs3_file_size_(file)) {
        // read from the bshrub/btree
        lfs3_bid_t bid;
        lfs3_bid_t weight;
        lfs3_bptr_t bptr;
        int err = lfs3_file_lookupnext_(lfs3, file, pos_,
                &bid, &weight, &bptr);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // any data on disk?
        if (pos_ < bid-(weight-1) + lfs3_bptr_size(&bptr)) {
            // note one important side-effect here is a strict
            // data hint
            lfs3_ssize_t d = lfs3_min(
                    size,
                    lfs3_bptr_size(&bptr)
                        - (pos_ - (bid-(weight-1))));
            lfs3_data_t slice = LFS3_DATA_SLICE(bptr.d,
                    pos_ - (bid-(weight-1)),
                    d);
            d = lfs3_data_read(lfs3, &slice,
                    buffer_, d);
            if (d < 0) {
                return d;
            }

            pos_ += d;
            buffer_ += d;
            size -= d;
        }

        // found a hole? fill with zeros
        lfs3_ssize_t d = lfs3_min(
                size,
                bid+1 - pos_);
        lfs3_memset(buffer_, 0, d);

        pos_ += d;
        buffer_ += d;
        size -= d;
    }

    // return amount read
    return pos_;
}
#endif

#ifndef LFS3_KVONLY
lfs3_ssize_t lfs3_file_read(lfs3_t *lfs3, lfs3_file_t *file,
        void *buffer, lfs3_size_t size) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));
    // can't read from writeonly files
    LFS3_ASSERT(!lfs3_o_iswronly(file->b.o.flags));
    LFS3_ASSERT(file->pos + size <= 0x7fffffff);

    lfs3_off_t pos_ = file->pos;
    uint8_t *buffer_ = buffer;
    while (size > 0 && pos_ < lfs3_file_size_(file)) {
        // keep track of the next highest priority data offset
        lfs3_ssize_t d = lfs3_min(size, lfs3_file_size_(file) - pos_);

        // any data in our cache?
        if (pos_ < file->cache.pos + file->cache.size
                && file->cache.size != 0) {
            if (pos_ >= file->cache.pos) {
                lfs3_ssize_t d_ = lfs3_min(
                        d,
                        file->cache.size - (pos_ - file->cache.pos));
                lfs3_memcpy(buffer_,
                        &file->cache.buffer[pos_ - file->cache.pos],
                        d_);

                pos_ += d_;
                buffer_ += d_;
                size -= d_;
                d -= d_;
                continue;
            }

            // cached data takes priority
            d = lfs3_min(d, file->cache.pos - pos_);
        }

        // any data in our btree?
        if (pos_ < file->b.shrub.weight) {
            if (!lfs3_o_isuncryst(file->b.o.flags)) {
                // bypass cache?
                if ((lfs3_size_t)d >= lfs3_file_cachesize(lfs3, file)) {
                    lfs3_ssize_t d_ = lfs3_file_readnext(lfs3, file,
                            pos_, buffer_, d);
                    if (d_ < 0) {
                        LFS3_ASSERT(d_ != LFS3_ERR_NOENT);
                        return d_;
                    }

                    pos_ += d_;
                    buffer_ += d_;
                    size -= d_;
                    continue;
                }

                // try to fill our cache with some data
                if (!lfs3_o_isunflush(file->b.o.flags)) {
                    lfs3_ssize_t d_ = lfs3_file_readnext(lfs3, file,
                            pos_, file->cache.buffer, d);
                    if (d_ < 0) {
                        LFS3_ASSERT(d != LFS3_ERR_NOENT);
                        return d_;
                    }
                    file->cache.pos = pos_;
                    file->cache.size = d_;
                    continue;
                }
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
            continue;
        }

        // found a hole? fill with zeros
        lfs3_memset(buffer_, 0, d);
        
        pos_ += d;
        buffer_ += d;
        size -= d;
    }

    // update file and return amount read
    lfs3_size_t read = pos_ - file->pos;
    file->pos = pos_;
    return read;
}
#endif

// low-level file writing

#ifndef LFS3_RDONLY
static int lfs3_file_commit(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_bid_t bid, const lfs3_rattr_t *rattrs, lfs3_size_t rattr_count) {
    return lfs3_bshrub_commit(lfs3, &file->b,
            bid, rattrs, rattr_count);
}
#endif

// use this flag to indicate bptr vs concatenated data fragments
#define LFS3_GRAFT_ISBPTR 0x80000000

static inline bool lfs3_graft_isbptr(lfs3_size_t graft_count) {
    return graft_count & LFS3_GRAFT_ISBPTR;
}

static inline lfs3_size_t lfs3_graft_count(lfs3_size_t graft_count) {
    return graft_count & ~LFS3_GRAFT_ISBPTR;
}

// graft bptr/fragments into our bshrub/btree
#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
static int lfs3_file_graft_(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t pos, lfs3_off_t weight, lfs3_soff_t delta,
        const lfs3_data_t *graft, lfs3_ssize_t graft_count) {
    // note! we must never allow our btree size to overflow, even
    // temporarily

    // can't carve more than the graft weight
    LFS3_ASSERT(delta >= -(lfs3_soff_t)weight);

    // carving the entire tree? revert to no bshrub/btree
    if (pos == 0
            && weight >= file->b.shrub.weight
            && delta == -(lfs3_soff_t)weight) {
        lfs3_file_discardbshrub(file);
        return 0;
    }

    // keep track of in-flight graft state
    //
    // normally, in-flight state would be protected by the block
    // allocator's checkpoint mechanism, where checkpoints prevent double
    // allocation of new blocks while the old copies remain tracked
    //
    // but we don't track the original bshrub copy during grafting!
    //
    // in theory, we could track 3 copies of the bshrub/btree: before
    // after, and mid-graft (we need the mid-graft copy to survive mdir
    // compactions), but that would add a lot of complexity/state to a
    // critical function on the stack hot-path
    //
    // instead, we can just explicitly track any in-flight graft state to
    // make sure we don't allocate these blocks in-between commits
    //
    lfs3->graft = graft;
    lfs3->graft_count = graft_count;

    // try to merge commits where possible
    lfs3_bid_t bid = file->b.shrub.weight;
    lfs3_rattr_t rattrs[3];
    lfs3_size_t rattr_count = 0;
    lfs3_bptr_t l;
    lfs3_bptr_t r;
    int err;

    // need a hole?
    if (pos > file->b.shrub.weight) {
        // can we coalesce?
        if (file->b.shrub.weight > 0) {
            bid = lfs3_min(bid, file->b.shrub.weight-1);
            rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_GROW, +(pos - file->b.shrub.weight));

        // new hole
        } else {
            bid = lfs3_min(bid, file->b.shrub.weight);
            rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_DATA, +(pos - file->b.shrub.weight));
        }
    }

    // try to carve any existing data
    lfs3_rattr_t r_rattr_ = {.tag=0};
    while (pos < file->b.shrub.weight) {
        lfs3_bid_t weight_;
        lfs3_bptr_t bptr_;
        err = lfs3_file_lookupnext(lfs3, file, pos,
                &bid, &weight_, &bptr_);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            goto failed;
        }

        // note, an entry can be both a left and right sibling
        l = bptr_;
        l.d = LFS3_DATA_SLICE(bptr_.d,
                -1,
                pos - (bid-(weight_-1)));
        r = bptr_;
        r.d = LFS3_DATA_SLICE(bptr_.d,
                pos+weight - (bid-(weight_-1)),
                -1);

        // found left sibling?
        if (bid-(weight_-1) < pos) {
            // can we get away with a grow attribute?
            if (lfs3_bptr_size(&bptr_) == lfs3_bptr_size(&l)) {
                rattrs[rattr_count++] = LFS3_RATTR(
                        LFS3_TAG_GROW, -(bid+1 - pos));

            // carve fragment?
            } else if (!lfs3_bptr_isbptr(&bptr_)
                    // carve bptr into fragment?
                    || lfs3_bptr_size(&l) <= lfs3->cfg->fragment_size) {
                rattrs[rattr_count++] = LFS3_RATTR_DATA(
                        LFS3_TAG_GROW | LFS3_TAG_MASK8 | LFS3_TAG_DATA,
                            -(bid+1 - pos),
                        &l.d);

            // carve bptr?
            } else {
                rattrs[rattr_count++] = LFS3_RATTR_BPTR(
                        LFS3_TAG_GROW | LFS3_TAG_MASK8 | LFS3_TAG_BLOCK,
                            -(bid+1 - pos),
                        &l);
            }

        // completely overwriting this entry?
        } else {
            rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_RM, -weight_);
        }

        // spans more than one entry? we can't do everything in one
        // commit because it might span more than one btree leaf, so
        // commit what we have and move on to next entry
        if (pos+weight > bid+1) {
            LFS3_ASSERT(lfs3_bptr_size(&r) == 0);
            LFS3_ASSERT(rattr_count <= sizeof(rattrs)/sizeof(lfs3_rattr_t));

            err = lfs3_file_commit(lfs3, file, bid,
                    rattrs, rattr_count);
            if (err) {
                goto failed;
            }

            delta += lfs3_min(weight, bid+1 - pos);
            weight -= lfs3_min(weight, bid+1 - pos);
            rattr_count = 0;
            continue;
        }

        // found right sibling?
        if (pos+weight < bid+1) {
            // can we coalesce a hole?
            if (lfs3_bptr_size(&r) == 0) {
                delta += bid+1 - (pos+weight);

            // carve fragment?
            } else if (!lfs3_bptr_isbptr(&bptr_)
                    // carve bptr into fragment?
                    || lfs3_bptr_size(&r) <= lfs3->cfg->fragment_size) {
                r_rattr_ = LFS3_RATTR_DATA(
                        LFS3_TAG_DATA, bid+1 - (pos+weight),
                        &r.d);

            // carve bptr?
            } else {
                r_rattr_ = LFS3_RATTR_BPTR(
                        LFS3_TAG_BLOCK, bid+1 - (pos+weight),
                        &r);
            }
        }

        delta += lfs3_min(weight, bid+1 - pos);
        weight -= lfs3_min(weight, bid+1 - pos);
        break;
    }

    // append our data
    if (weight + delta > 0) {
        lfs3_size_t dsize = 0;
        for (lfs3_size_t i = 0; i < lfs3_graft_count(graft_count); i++) {
            dsize += lfs3_data_size(graft[i]);
        }

        // can we coalesce a hole?
        if (dsize == 0 && pos > 0) {
            bid = lfs3_min(bid, file->b.shrub.weight-1);
            rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_GROW, +(weight + delta));

        // need a new hole?
        } else if (dsize == 0) {
            bid = lfs3_min(bid, file->b.shrub.weight);
            rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_DATA, +(weight + delta));

        // append a new fragment?
        } else if (!lfs3_graft_isbptr(graft_count)) {
            bid = lfs3_min(bid, file->b.shrub.weight);
            rattrs[rattr_count++] = LFS3_RATTR_CAT_(
                    LFS3_TAG_DATA, +(weight + delta),
                    graft, graft_count);

        // append a new bptr?
        } else {
            bid = lfs3_min(bid, file->b.shrub.weight);
            rattrs[rattr_count++] = LFS3_RATTR_BPTR(
                    LFS3_TAG_BLOCK, +(weight + delta),
                    (const lfs3_bptr_t*)graft);
        }
    }

    // and don't forget the right sibling
    if (r_rattr_.tag) {
        rattrs[rattr_count++] = r_rattr_;
    }

    // commit pending rattrs
    if (rattr_count > 0) {
        LFS3_ASSERT(rattr_count <= sizeof(rattrs)/sizeof(lfs3_rattr_t));

        err = lfs3_file_commit(lfs3, file, bid,
                rattrs, rattr_count);
        if (err) {
            goto failed;
        }
    }

    lfs3->graft = NULL;
    lfs3->graft_count = 0;
    return 0;

failed:;
    lfs3->graft = NULL;
    lfs3->graft_count = 0;
    return err;
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
#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY) && !defined(LFS3_2BONLY)
// this LFS3_NOINLINE is to force lfs3_file_crystallize__ off the stack
// hot-path
LFS3_NOINLINE
static int lfs3_file_crystallize__(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t block_pos,
        lfs3_ssize_t crystal_min, lfs3_ssize_t crystal_max,
        lfs3_off_t pos, const uint8_t *buffer, lfs3_size_t size) {
    // align to prog_size, limit to block_size and theoretical file size
    lfs3_off_t crystal_limit = lfs3_min(
            block_pos + lfs3_min(
                lfs3_aligndown(
                    (lfs3_off_t)crystal_max,
                    lfs3->cfg->prog_size),
                lfs3->cfg->block_size),
            lfs3_max(
                pos + size,
                file->b.shrub.weight));

    // resuming crystallization? or do we need to allocate a new block?
    if (!lfs3_o_isuncryst(file->b.o.flags)) {
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
    LFS3_ASSERT(lfs3_bptr_size(&file->leaf.bptr)
            == file->leaf.weight);

    // before we write, claim the erased state!
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        if (lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o != &file->b.o
                && lfs3_bptr_block(&((lfs3_file_t*)o)->leaf.bptr)
                    == lfs3_bptr_block(&file->leaf.bptr)) {
            lfs3_bptr_claim(&((lfs3_file_t*)o)->leaf.bptr);
        }
    }

    while (true) {
        // crystallize data into our block
        //
        // i.e. eagerly merge any right neighbors unless that would put
        // us over our crystal_size/block_size
        lfs3_off_t pos_ = block_pos
                + lfs3_bptr_off(&file->leaf.bptr)
                + lfs3_bptr_size(&file->leaf.bptr);
        uint32_t cksum_ = lfs3_bptr_cksum(&file->leaf.bptr);
        while (pos_ < crystal_limit) {
            // keep track of the next highest priority data offset
            lfs3_ssize_t d = crystal_limit - pos_;

            // any data in our buffer?
            if (pos_ < pos + size && size > 0) {
                if (pos_ >= pos) {
                    lfs3_ssize_t d_ = lfs3_min(
                            d,
                            size - (pos_ - pos));
                    int err = lfs3_bd_prog(lfs3,
                            lfs3_bptr_block(&file->leaf.bptr),
                            pos_ - block_pos,
                            &buffer[pos_ - pos], d_,
                            &cksum_, true);
                    if (err) {
                        LFS3_ASSERT(err != LFS3_ERR_RANGE);
                        // bad prog? try another block
                        if (err == LFS3_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }

                    pos_ += d_;
                    d -= d_;
                }

                // buffered data takes priority
                d = lfs3_min(d, pos - pos_);
            }

            // any data on disk?
            if (pos_ < file->b.shrub.weight) {
                lfs3_bid_t bid__;
                lfs3_bid_t weight__;
                lfs3_bptr_t bptr__;
                int err = lfs3_file_lookupnext(lfs3, file, pos_,
                        &bid__, &weight__, &bptr__);
                if (err) {
                    LFS3_ASSERT(err != LFS3_ERR_NOENT);
                    return err;
                }

                // is this data a pure hole? stop early to (FUTURE)
                // better leverage erased-state in sparse files, and to
                // try to avoid writing a bunch of unnecessary zeros
                if ((pos_ >= bid__-(weight__-1) + lfs3_bptr_size(&bptr__)
                            // does this data exceed our block_size? also
                            // stop early to try to avoid messing up
                            // block alignment
                            || (bid__-(weight__-1) + lfs3_bptr_size(&bptr__))
                                    - block_pos
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
                    file->b.o.flags &= ~LFS3_o_UNCRYST;
                    break;
                }

                if (pos_ < bid__-(weight__-1) + lfs3_bptr_size(&bptr__)) {
                    // note one important side-effect here is a strict
                    // data hint
                    lfs3_ssize_t d_ = lfs3_min(
                            d,
                            (bid__-(weight__-1) + lfs3_bptr_size(&bptr__))
                                - pos_);
                    err = lfs3_bd_progdata(lfs3,
                            lfs3_bptr_block(&file->leaf.bptr),
                            pos_ - block_pos,
                            LFS3_DATA_SLICE(bptr__.d,
                                pos_ - (bid__-(weight__-1)),
                                d_),
                            &cksum_, true);
                    if (err) {
                        LFS3_ASSERT(err != LFS3_ERR_RANGE);
                        // bad prog? try another block
                        if (err == LFS3_ERR_CORRUPT) {
                            goto relocate;
                        }
                        return err;
                    }

                    pos_ += d_;
                    d -= d_;
                }

                // found a hole? just make sure next leaf takes priority
                d = lfs3_min(d, bid__+1 - pos_);
            }

            // found a hole? fill with zeros
            int err = lfs3_bd_set(lfs3,
                    lfs3_bptr_block(&file->leaf.bptr),
                    pos_ - block_pos,
                    0, d,
                    &cksum_, true);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_RANGE);
                // bad prog? try another block
                if (err == LFS3_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            pos_ += d;
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
                    pos + size,
                    file->b.shrub.weight)) {
            file->b.o.flags &= ~LFS3_o_UNCRYST;
        }

        // a bit of a hack here, we need to truncate our block to
        // prog_size alignment to avoid padding issues
        //
        // doing this retroactively to the pcache greatly simplifies the
        // above loop, though we may end up reading more than is
        // strictly necessary
        lfs3_ssize_t d = (pos_ - block_pos) % lfs3->cfg->prog_size;
        lfs3->pcache.size -= d;
        pos_ -= d;

        // finalize our write
        int err = lfs3_bd_flush(lfs3,
                &cksum_, true);
        if (err) {
            // bad prog? try another block
            if (err == LFS3_ERR_CORRUPT) {
                goto relocate;
            }
            return err;
        }

        // and update the leaf bptr
        LFS3_ASSERT(pos_ - block_pos >= lfs3_bptr_off(&file->leaf.bptr));
        LFS3_ASSERT(pos_ - block_pos <= lfs3->cfg->block_size);
        file->leaf.pos = block_pos + lfs3_bptr_off(&file->leaf.bptr);
        file->leaf.weight = pos_ - file->leaf.pos;
        lfs3_bptr_init(&file->leaf.bptr,
                LFS3_DATA_DISK(
                    lfs3_bptr_block(&file->leaf.bptr),
                    lfs3_bptr_off(&file->leaf.bptr),
                    pos_ - file->leaf.pos),
                // mark as erased
                (pos_ - block_pos) | LFS3_BPTR_ISERASED,
                cksum_);
        return 0;

    relocate:;
        // allocate a new block
        //
        // note if we relocate, we rewrite the entire block from
        // block_pos using what we can find in our tree
        lfs3_sblock_t block = lfs3_alloc(lfs3, true);
        if (block < 0) {
            return block;
        }

        lfs3_bptr_init(&file->leaf.bptr,
                LFS3_DATA_DISK(block, 0, 0),
                // mark as erased
                LFS3_BPTR_ISERASED | 0,
                0);

        // mark as uncrystallized
        file->b.o.flags |= LFS3_o_UNCRYST;
    }
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
#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY) && !defined(LFS3_2BONLY)
static int lfs3_file_crystallize_(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t block_pos,
        lfs3_ssize_t crystal_min, lfs3_ssize_t crystal_max,
        lfs3_off_t pos, const uint8_t *buffer, lfs3_size_t size) {
    // this is split into two functions to try to minimize stack usage

    // crystallize
    int err = lfs3_file_crystallize__(lfs3, file,
            block_pos, crystal_min, crystal_max,
            pos, buffer, size);
    if (err) {
        goto failed;
    }

    // and graft into tree
    err = lfs3_file_graft_(lfs3, file,
            file->leaf.pos, file->leaf.weight, 0,
            &file->leaf.bptr.d, LFS3_GRAFT_ISBPTR | 1);
    if (err) {
        goto failed;
    }

    return 0;

failed:;
    // if we failed to crystallize we need to discard the leaf as it no
    // longer matches the btree/bshrub state, this also clears the
    // LFS3_o_UNCRYST flag
    lfs3_file_discardleaf(file);
    return err;
}
#endif

#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY) && !defined(LFS3_2BONLY)
static int lfs3_file_crystallize(lfs3_t *lfs3, lfs3_file_t *file) {
    // do nothing if our file is already crystallized
    if (!lfs3_o_isuncryst(file->b.o.flags)) {
        return 0;
    }

    // uncrystallized files must be unsynced
    LFS3_ASSERT(lfs3_o_isunsync(file->b.o.flags));

    // checkpoint the allocator
    lfs3_alloc_ckpoint(lfs3);
    // finish crystallizing
    int err = lfs3_file_crystallize_(lfs3, file,
            file->leaf.pos - lfs3_bptr_off(&file->leaf.bptr), -1, -1,
            0, NULL, 0);
    if (err) {
        return err;
    }

    // we should have crystallized
    LFS3_ASSERT(!lfs3_o_isuncryst(file->b.o.flags));
    return 0;
}
#endif

#if defined(LFS3_KVONLY) && !defined(LFS3_RDONLY)
// a simpler flush if we only flush files once
static int lfs3_file_flushset_(lfs3_t *lfs3, lfs3_file_t *file,
        const uint8_t *buffer, lfs3_size_t size) {
    lfs3_off_t pos = 0;
    while (size > 0) {
        // checkpoint the allocator
        lfs3_alloc_ckpoint(lfs3);

        // enough data for a block?
        #ifndef LFS3_2BONLY
        if (size > lfs3->cfg->crystal_thresh) {
            // align down for prog alignment
            lfs3_ssize_t d = lfs3_aligndown(
                    lfs3_min(size, lfs3->cfg->block_size),
                    lfs3->cfg->prog_size);

        relocate:;
            // allocate a new block
            lfs3_sblock_t block = lfs3_alloc(lfs3, true);
            if (block < 0) {
                return block;
            }

            // write our data
            uint32_t cksum = 0;
            int err = lfs3_bd_prog(lfs3, block, 0, buffer, d,
                    &cksum, true);
            if (err) {
                // bad prog? try another block
                if (err == LFS3_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // finalize our write
            err = lfs3_bd_flush(lfs3,
                    &cksum, true);
            if (err) {
                // bad prog? try another block
                if (err == LFS3_ERR_CORRUPT) {
                    goto relocate;
                }
                return err;
            }

            // create a block pointer
            lfs3_bptr_t bptr;
            lfs3_bptr_init(&bptr,
                    LFS3_DATA_DISK(block, 0, d),
                    d,
                    cksum);

            // and commit to bshrub/btree
            err = lfs3_file_commit(lfs3, file, pos, LFS3_RATTRS(
                    LFS3_RATTR_BPTR(LFS3_TAG_BLOCK, +d, &bptr)));
            if (err) {
                return err;
            }

            pos += d;
            buffer += d;
            size -= d;
            continue;
        }
        #endif

        // fallback to writing fragments
        lfs3_ssize_t d = lfs3_min(size, lfs3->cfg->fragment_size);

        // commit to bshrub/btree
        int err = lfs3_file_commit(lfs3, file, pos, LFS3_RATTRS(
                LFS3_RATTR_DATA(
                    LFS3_TAG_DATA, +d,
                    &LFS3_DATA_BUF(buffer, d))));
        if (err) {
            return err;
        }

        pos += d;
        buffer += d;
        size -= d;
    }

    return 0;
}
#endif

#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
static int lfs3_file_flush_(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_off_t pos, const uint8_t *buffer, lfs3_size_t size) {
    // we can skip some btree lookups if we know we are aligned from a
    // previous iteration, we already do way too many btree lookups
    bool aligned = false;

    // if crystallization is disabled, just skip to writing fragments
    if (LFS3_IFDEF_2BONLY(
            true,
            lfs3->cfg->crystal_thresh > lfs3->cfg->block_size)) {
        goto fragment;
    }

    // iteratively write blocks
    #ifndef LFS3_2BONLY
    while (size > 0) {
        // checkpoint the allocator
        lfs3_alloc_ckpoint(lfs3);

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
                && pos >= block_end
                && pos < block_start + lfs3->cfg->block_size
                && pos - block_end < lfs3->cfg->crystal_thresh
                // need to bail if we can't meet prog alignment
                && (pos + size) - block_end >= lfs3->cfg->prog_size) {
            // mark as uncrystallized
            file->b.o.flags |= LFS3_o_UNCRYST;
            // crystallize
            int err = lfs3_file_crystallize_(lfs3, file,
                    block_start, -1, (pos + size) - block_start,
                    pos, buffer, size);
            if (err) {
                return err;
            }

            // update buffer state
            lfs3_ssize_t d = lfs3_max(
                    file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr),
                    pos) - pos;
            pos += d;
            buffer += lfs3_min(d, size);
            size -= lfs3_min(d, size);

            // we should be aligned now
            aligned = true;
            continue;
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
        lfs3_off_t crystal_start = pos;
        lfs3_off_t crystal_end = pos + size;

        // if we haven't already exceeded our crystallization threshold,
        // find left crystal neighbor
        lfs3_off_t poke = lfs3_smax(
                crystal_start - (lfs3->cfg->crystal_thresh-1),
                0);
        if (crystal_end - crystal_start < lfs3->cfg->crystal_thresh
                && crystal_start > 0
                && poke < file->b.shrub.weight
                // don't bother looking up left after the first block
                && !aligned) {
            lfs3_bid_t bid;
            lfs3_bid_t weight;
            lfs3_bptr_t bptr;
            int err = lfs3_file_lookupnext(lfs3, file, poke,
                    &bid, &weight, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // if left crystal neighbor is a fragment and there is no
            // obvious hole between our own crystal and our neighbor,
            // include as a part of our crystal
            if (!lfs3_bptr_isbptr(&bptr)
                    && lfs3_bptr_size(&bptr) > 0
                    // hole? holes can be quite large and shouldn't
                    // trigger crystallization
                    && bid-(weight-1) + lfs3_bptr_size(&bptr) >= poke) {
                crystal_start = bid-(weight-1);

            // otherwise our neighbor determines our crystal boundary
            } else {
                crystal_start = lfs3_min(bid+1, crystal_start);
            }
        }

        // if we haven't already exceeded our crystallization threshold,
        // find right crystal neighbor
        poke = lfs3_min(
                crystal_start + (lfs3->cfg->crystal_thresh-1),
                file->b.shrub.weight-1);
        if (crystal_end - crystal_start < lfs3->cfg->crystal_thresh
                && crystal_end < file->b.shrub.weight) {
            lfs3_bid_t bid;
            lfs3_bid_t weight;
            lfs3_bptr_t bptr;
            int err = lfs3_file_lookupnext(lfs3, file, poke,
                    &bid, &weight, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // if right crystal neighbor is a fragment, include as a part
            // of our crystal
            if (!lfs3_bptr_isbptr(&bptr)
                    && lfs3_bptr_size(&bptr) > 0) {
                crystal_end = lfs3_max(
                        bid-(weight-1) + lfs3_bptr_size(&bptr),
                        crystal_end);

            // otherwise treat as crystal boundary
            } else {
                crystal_end = lfs3_max(
                        bid-(weight-1),
                        crystal_end);
            }
        }

        // now that we have our crystal guess, we need to decide how to
        // write to the file

        // below our crystallization threshold? fallback to writing fragments
        if (crystal_end - crystal_start < lfs3->cfg->crystal_thresh
                // enough for prog alignment?
                || crystal_end - crystal_start < lfs3->cfg->prog_size) {
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
            file->b.o.flags |= LFS3_o_UNCRYST;
            // crystallize
            int err = lfs3_file_crystallize_(lfs3, file,
                    block_start, -1, crystal_end - block_start,
                    pos, buffer, size);
            if (err) {
                return err;
            }

            // update buffer state, this may or may not make progress
            lfs3_ssize_t d = lfs3_max(
                    file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr),
                    pos) - pos;
            pos += d;
            buffer += lfs3_min(d, size);
            size -= lfs3_min(d, size);

            // we should be aligned now
            aligned = true;
            continue;
        }

        // if we're mid-crystallization, finish crystallizing the block
        // and graft it into our bshrub/btree
        if (lfs3_o_isuncryst(file->b.o.flags)) {
            // finish crystallizing
            int err = lfs3_file_crystallize_(lfs3, file,
                    file->leaf.pos - lfs3_bptr_off(&file->leaf.bptr), -1, -1,
                    0, NULL, 0);
            if (err) {
                return err;
            }

            // we should have crystallized
            LFS3_ASSERT(!lfs3_o_isuncryst(file->b.o.flags));
        }

        // before we can crystallize we need to figure out the best
        // block alignment, we use the entry immediately to the left of
        // our crystal for this
        if (crystal_start > 0
                && file->b.shrub.weight > 0
                // don't bother to lookup left after the first block
                && !aligned) {
            lfs3_bid_t bid;
            lfs3_bid_t weight;
            lfs3_bptr_t bptr;
            int err = lfs3_file_lookupnext(lfs3, file,
                    lfs3_min(
                        crystal_start-1,
                        file->b.shrub.weight-1),
                    &bid, &weight, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // is our left neighbor in the same block?
            if (crystal_start - (bid-(weight-1))
                        < lfs3->cfg->block_size
                    && lfs3_bptr_size(&bptr) > 0) {
                crystal_start = bid-(weight-1);

            // no? is our left neighbor at least our left block neighbor?
            // align to block alignment
            } else if (crystal_start - (bid-(weight-1))
                        < 2*lfs3->cfg->block_size
                    && lfs3_bptr_size(&bptr) > 0) {
                crystal_start = bid-(weight-1) + lfs3->cfg->block_size;
            }
        }

        // start crystallizing!
        //
        // lfs3_file_crystallize_ handles block allocation/relocation
        int err = lfs3_file_crystallize_(lfs3, file,
                crystal_start, -1, crystal_end - crystal_start,
                pos, buffer, size);
        if (err) {
            return err;
        }

        // update buffer state, this may or may not make progress
        lfs3_ssize_t d = lfs3_max(
                file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr),
                pos) - pos;
        pos += d;
        buffer += lfs3_min(d, size);
        size -= lfs3_min(d, size);

        // we should be aligned now
        aligned = true;
    }
    #endif

    return 0;

fragment:;
    // iteratively write fragments (inlined leaves)
    while (size > 0) {
        // checkpoint the allocator
        lfs3_alloc_ckpoint(lfs3);

        // do we need to discard our leaf? we need to discard fragments
        // in case the underlying rbyd compacts, and we need to discard
        // overwritten blocks
        //
        // note we need to discard before attempting to graft since a
        // single graft may be split up into multiple commits
        //
        // unfortunately we don't know where our fragment will end up
        // until after the commit, so we can't track it in our leaf
        // quite yet
        if (!lfs3_bptr_isbptr(&file->leaf.bptr)
                || (pos < file->leaf.pos + lfs3_bptr_size(&file->leaf.bptr)
                    && pos + size > file->leaf.pos)) {
            lfs3_file_discardleaf(file);
        }

        // truncate to our fragment size
        lfs3_off_t fragment_start = pos;
        lfs3_off_t fragment_end = fragment_start + lfs3_min(
                size,
                lfs3->cfg->fragment_size);

        lfs3_data_t datas[3];
        lfs3_size_t data_count = 0;

        // do we have a left sibling? don't bother to lookup if fragment
        // is already full
        if (fragment_end - fragment_start < lfs3->cfg->fragment_size
                && fragment_start > 0
                && fragment_start <= file->b.shrub.weight
                // don't bother to lookup left after first fragment
                && !aligned) {
            lfs3_bid_t bid;
            lfs3_bid_t weight;
            lfs3_bptr_t bptr;
            int err = lfs3_file_lookupnext(lfs3, file,
                    fragment_start-1,
                    &bid, &weight, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // can we coalesce?
            if (bid-(weight-1) + lfs3_bptr_size(&bptr) >= fragment_start
                    && fragment_end - (bid-(weight-1))
                        <= lfs3->cfg->fragment_size) {
                datas[data_count++] = LFS3_DATA_TRUNCATE(bptr.d,
                        fragment_start - (bid-(weight-1)));

                fragment_start = bid-(weight-1);
                fragment_end = fragment_start + lfs3_min(
                        fragment_end - (bid-(weight-1)),
                        lfs3->cfg->fragment_size);
            }
        }

        // append our new data
        datas[data_count++] = LFS3_DATA_BUF(
                buffer,
                fragment_end - pos);

        // do we have a right sibling? don't bother to lookup if fragment
        // is already full
        //
        // note this may the same as our left sibling
        if (fragment_end - fragment_start < lfs3->cfg->fragment_size
                && fragment_end < file->b.shrub.weight) {
            lfs3_bid_t bid;
            lfs3_bid_t weight;
            lfs3_bptr_t bptr;
            int err = lfs3_file_lookupnext(lfs3, file,
                    fragment_end,
                    &bid, &weight, &bptr);
            if (err) {
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            // can we coalesce?
            if (fragment_end < bid-(weight-1) + lfs3_bptr_size(&bptr)
                    && bid-(weight-1) + lfs3_bptr_size(&bptr)
                            - fragment_start
                        <= lfs3->cfg->fragment_size) {
                datas[data_count++] = LFS3_DATA_FRUNCATE(bptr.d,
                        bid-(weight-1) + lfs3_bptr_size(&bptr)
                            - fragment_end);

                fragment_end = fragment_start + lfs3_min(
                        bid-(weight-1) + lfs3_bptr_size(&bptr)
                            - fragment_start,
                        lfs3->cfg->fragment_size);
            }
        }

        // make sure we didn't overflow our data buffer
        LFS3_ASSERT(data_count <= 3);

        // once we've figured out what fragment to write, graft it into
        // our tree
        int err = lfs3_file_graft_(lfs3, file,
                fragment_start, fragment_end - fragment_start, 0,
                datas, data_count);
        if (err) {
            return err;
        }

        // update buffer state
        lfs3_ssize_t d = fragment_end - pos;
        pos += d;
        buffer += lfs3_min(d, size);
        size -= lfs3_min(d, size);

        // we should be aligned now
        aligned = true;
    }

    return 0;
}
#endif


// high-level file writing

#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
lfs3_ssize_t lfs3_file_write(lfs3_t *lfs3, lfs3_file_t *file,
        const void *buffer, lfs3_size_t size) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));
    // can't write to readonly files
    LFS3_ASSERT(!lfs3_o_isrdonly(file->b.o.flags));

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

    // clobber entangled traversals
    lfs3_omdir_clobber(lfs3, &file->b.o, LFS3_t_DIRTY);
    // mark as unsynced in case we fail
    file->b.o.flags |= LFS3_o_UNSYNC;

    // update pos if we are appending
    lfs3_off_t pos = file->pos;
    if (lfs3_o_isappend(file->b.o.flags)) {
        pos = lfs3_file_size_(file);
    }

    const uint8_t *buffer_ = buffer;
    lfs3_size_t written = 0;
    while (size > 0) {
        // bypass cache?
        //
        // note we flush our cache before bypassing writes, this isn't
        // strictly necessary, but enforces a more intuitive write order
        // and avoids weird cases with low-level write heuristics
        //
        if (!lfs3_o_isunflush(file->b.o.flags)
                && size >= lfs3_file_cachesize(lfs3, file)) {
            err = lfs3_file_flush_(lfs3, file,
                    pos, buffer_, size);
            if (err) {
                goto failed;
            }

            // after success, fill our cache with the tail of our write
            //
            // note we need to clear the cache anyways to avoid any
            // out-of-date data
            file->cache.pos = pos + size - lfs3_file_cachesize(lfs3, file);
            lfs3_memcpy(file->cache.buffer,
                    &buffer_[size - lfs3_file_cachesize(lfs3, file)],
                    lfs3_file_cachesize(lfs3, file));
            file->cache.size = lfs3_file_cachesize(lfs3, file);

            file->b.o.flags &= ~LFS3_o_UNFLUSH;
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
        if (!lfs3_o_isunflush(file->b.o.flags)
                || (pos >= file->cache.pos
                    && pos <= file->cache.pos + file->cache.size
                    && pos
                        < file->cache.pos
                            + lfs3_file_cachesize(lfs3, file))) {
            // unused cache? we can move it where we need it
            if (!lfs3_o_isunflush(file->b.o.flags)) {
                file->cache.pos = pos;
                file->cache.size = 0;
            }

            lfs3_size_t d = lfs3_min(
                    size,
                    lfs3_file_cachesize(lfs3, file)
                        - (pos - file->cache.pos));
            lfs3_memcpy(&file->cache.buffer[pos - file->cache.pos],
                    buffer_,
                    d);
            file->cache.size = lfs3_max(
                    file->cache.size,
                    pos+d - file->cache.pos);

            file->b.o.flags |= LFS3_o_UNFLUSH;
            written += d;
            pos += d;
            buffer_ += d;
            size -= d;
            continue;
        }

        // flush our cache so the above can't fail
        err = lfs3_file_flush_(lfs3, file,
                file->cache.pos, file->cache.buffer, file->cache.size);
        if (err) {
            goto failed;
        }
        file->b.o.flags &= ~LFS3_o_UNFLUSH;
    }

    // update our pos
    file->pos = pos;

    // flush if requested
    if (lfs3_o_isflush(file->b.o.flags)) {
        err = lfs3_file_flush(lfs3, file);
        if (err) {
            goto failed;
        }
    }

    // sync if requested
    if (lfs3_o_issync(file->b.o.flags)) {
        err = lfs3_file_sync(lfs3, file);
        if (err) {
            goto failed;
        }
    }

    return written;

failed:;
    // mark as desync so lfs3_file_close doesn't write to disk
    file->b.o.flags |= LFS3_O_DESYNC;
    return err;
}
#endif

#ifdef LFS3_KVONLY
static
#endif
int lfs3_file_flush(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    // do nothing if our file is already flushed, crystallized,
    // and grafted
    if (!lfs3_o_isunflush(file->b.o.flags)
            && !lfs3_o_isuncryst(file->b.o.flags)) {
        return 0;
    }
    // unflushed files must be unsynced
    LFS3_ASSERT(lfs3_o_isunsync(file->b.o.flags));
    // uncrystallized files must be unsynced
    LFS3_ASSERT(lfs3_o_isunsync(file->b.o.flags));
    // unflushed files can't be readonly
    LFS3_ASSERT(!lfs3_o_isrdonly(file->b.o.flags));

    #ifndef LFS3_RDONLY
    // clobber entangled traversals
    lfs3_omdir_clobber(lfs3, &file->b.o, LFS3_t_DIRTY);
    int err;

    // flush our cache
    if (lfs3_o_isunflush(file->b.o.flags)) {
        #ifdef LFS3_KVONLY
        err = lfs3_file_flushset_(lfs3, file,
                file->cache.buffer, file->cache.size);
        if (err) {
            goto failed;
        }
        #else
        err = lfs3_file_flush_(lfs3, file,
                file->cache.pos, file->cache.buffer, file->cache.size);
        if (err) {
            goto failed;
        }
        #endif

        // mark as flushed
        file->b.o.flags &= ~LFS3_o_UNFLUSH;
    }

    #if !defined(LFS3_KVONLY) && !defined(LFS3_2BONLY)
    // and crystallize/graft our leaf
    err = lfs3_file_crystallize(lfs3, file);
    if (err) {
        goto failed;
    }
    #endif
    #endif

    return 0;

    #ifndef LFS3_RDONLY
failed:;
    // mark as desync so lfs3_file_close doesn't write to disk
    file->b.o.flags |= LFS3_O_DESYNC;
    return err;
    #endif
}

#ifndef LFS3_RDONLY
// this LFS3_NOINLINE is to force lfs3_file_sync_ off the stack hot-path
LFS3_NOINLINE
static int lfs3_file_sync_(lfs3_t *lfs3, lfs3_file_t *file,
        const lfs3_name_t *name) {
    // build a commit of any pending file metadata
    lfs3_rattr_t rattrs[LFS3_IFDEF_KVONLY(3, 4)];
    lfs3_size_t rattr_count = 0;
    lfs3_data_t name_data;
    lfs3_rattr_t shrub_rattrs[1];
    lfs3_size_t shrub_rattr_count = 0;
    lfs3_shrubcommit_t shrub_commit;

    // uncreated files must be unsync
    LFS3_ASSERT(!lfs3_o_isuncreat(file->b.o.flags)
            || lfs3_o_isunsync(file->b.o.flags));
    // small unflushed files must be unsync
    LFS3_ASSERT(!lfs3_o_isunflush(file->b.o.flags)
            || lfs3_o_isunsync(file->b.o.flags));
    LFS3_ASSERT(!lfs3_o_isuncryst(file->b.o.flags)
            || lfs3_o_isunsync(file->b.o.flags));

    // pending metadata changes?
    if (lfs3_o_isunsync(file->b.o.flags)) {
        // explicit name?
        if (name) {
            rattrs[rattr_count++] = LFS3_RATTR_NAME_(
                    LFS3_TAG_REG, +1,
                    name);

        // not created yet? need to convert to normal file
        } else if (lfs3_o_isuncreat(file->b.o.flags)) {
            // convert stickynote -> reg file
            int err = lfs3_rbyd_lookup(lfs3, &file->b.o.mdir.r,
                    lfs3_mrid(lfs3, file->b.o.mdir.mid), LFS3_TAG_STICKYNOTE,
                    NULL, &name_data);
            if (err) {
                // orphan flag but no stickynote tag?
                LFS3_ASSERT(err != LFS3_ERR_NOENT);
                return err;
            }

            rattrs[rattr_count++] = LFS3_RATTR_DATA(
                    LFS3_TAG_MASK8 | LFS3_TAG_REG, 0,
                    &name_data);
        }

        // pending small file flush?
        if (lfs3_o_isunflush(file->b.o.flags)
                || lfs3_o_isuncryst(file->b.o.flags)) {
            // this only works if the file is entirely in our cache
            #ifndef LFS3_KVONLY
            LFS3_ASSERT(file->cache.pos == 0);
            #endif
            LFS3_ASSERT(file->cache.size == lfs3_file_size_(file));

            // discard any lingering bshrub state
            #ifndef LFS3_KVONLY
            lfs3_file_discardleaf(file);
            #endif
            lfs3_file_discardbshrub(file);

            // build a small shrub commit
            if (file->cache.size > 0) {
                shrub_rattrs[shrub_rattr_count++] = LFS3_RATTR_DATA(
                        LFS3_TAG_DATA, +file->cache.size,
                        (const lfs3_data_t*)&file->cache);

                LFS3_ASSERT(shrub_rattr_count
                        <= sizeof(shrub_rattrs)/sizeof(lfs3_rattr_t));
                shrub_commit.bshrub = &file->b;
                shrub_commit.rid = 0;
                shrub_commit.rattrs = shrub_rattrs;
                shrub_commit.rattr_count = shrub_rattr_count;
                rattrs[rattr_count++] = LFS3_RATTR_SHRUBCOMMIT(&shrub_commit);
            }
        }

        // make sure data is on-disk before committing metadata
        if (lfs3_file_size_(file) > 0
                && !lfs3_o_isunflush(file->b.o.flags)
                && !lfs3_o_isuncryst(file->b.o.flags)) {
            int err = lfs3_bd_sync(lfs3);
            if (err) {
                return err;
            }
        }

        // zero size files should have no bshrub/btree
        LFS3_ASSERT(lfs3_file_size_(file) > 0
                || lfs3_bshrub_isbnull(&file->b));

        // no bshrub/btree?
        if (lfs3_file_size_(file) == 0) {
            rattrs[rattr_count++] = LFS3_RATTR(
                    LFS3_TAG_RM | LFS3_TAG_MASK8 | LFS3_TAG_STRUCT, 0);
        // bshrub?
        } else if (lfs3_bshrub_isbshrub(&file->b)
                || lfs3_o_isunflush(file->b.o.flags)
                || lfs3_o_isuncryst(file->b.o.flags)) {
            rattrs[rattr_count++] = LFS3_RATTR_SHRUB(
                    LFS3_TAG_MASK8 | LFS3_TAG_BSHRUB, 0,
                    // note we use the staged trunk here
                    &file->b.shrub_);
        // btree?
        } else if (lfs3_bshrub_isbtree(&file->b)) {
            rattrs[rattr_count++] = LFS3_RATTR_BTREE(
                    LFS3_TAG_MASK8 | LFS3_TAG_BTREE, 0,
                    &file->b.shrub);
        } else {
            LFS3_UNREACHABLE();
        }
    }

    // pending custom attributes?
    //
    // this gets real messy, since users can change custom attributes
    // whenever they want without informing littlefs, the best we can do
    // is read from disk to manually check if any attributes changed
    #ifndef LFS3_KVONLY
    bool attrs = lfs3_o_isunsync(file->b.o.flags);
    if (!attrs) {
        for (lfs3_size_t i = 0; i < file->cfg->attr_count; i++) {
            // skip readonly attrs and lazy attrs
            if (lfs3_o_isrdonly(file->cfg->attrs[i].flags)
                    || lfs3_a_islazy(file->cfg->attrs[i].flags)) {
                continue;
            }

            // lookup the attr
            lfs3_data_t data;
            int err = lfs3_mdir_lookup(lfs3, &file->b.o.mdir,
                    LFS3_TAG_ATTR(file->cfg->attrs[i].type),
                    NULL, &data);
            if (err && err != LFS3_ERR_NOENT) {
                return err;
            }

            // does disk match our attr?
            lfs3_scmp_t cmp = lfs3_attr_cmp(lfs3, &file->cfg->attrs[i],
                    (err != LFS3_ERR_NOENT) ? &data : NULL);
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
        rattrs[rattr_count++] = LFS3_RATTR_ATTRS(
                file->cfg->attrs, file->cfg->attr_count);
    }
    #endif

    // pending metadata? looks like we need to write to disk
    if (rattr_count > 0) {
        // make sure we don't overflow our rattr buffer
        LFS3_ASSERT(rattr_count <= sizeof(rattrs)/sizeof(lfs3_rattr_t));
        // checkpoint the allocator
        lfs3_alloc_ckpoint(lfs3);
        // and commit!
        int err = lfs3_mdir_commit(lfs3, &file->b.o.mdir,
                rattrs, rattr_count);
        if (err) {
            return err;
        }
    }

    // update in-device state
    for (lfs3_omdir_t *o = lfs3->omdirs; o; o = o->next) {
        #ifndef LFS3_KVONLY
        if (lfs3_o_type(o->flags) == LFS3_TYPE_REG
                && o->mdir.mid == file->b.o.mdir.mid
                // don't double update
                && o != &file->b.o) {
            lfs3_file_t *file_ = (lfs3_file_t*)o;
            // notify all files of creation
            file_->b.o.flags &= ~LFS3_o_UNCREAT;

            // mark desynced files an unsynced
            if (lfs3_o_isdesync(file_->b.o.flags)) {
                file_->b.o.flags |= LFS3_o_UNSYNC;

            // update synced files
            } else {
                // update flags
                file_->b.o.flags &= ~LFS3_o_UNSYNC
                        & ~LFS3_o_UNFLUSH
                        & ~LFS3_o_UNCRYST;
                // update shrubs
                file_->b.shrub = file->b.shrub;
                // update leaves
                file_->leaf = file->leaf;

                // update caches
                //
                // note we need to be careful if caches have different
                // sizes, prefer the most recent data in this case
                lfs3_size_t d = file->cache.size - lfs3_min(
                        lfs3_file_cachesize(lfs3, file_),
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
        #endif

        // clobber entangled traversals
        if (lfs3_o_type(o->flags) == LFS3_type_TRAVERSAL
                && o->mdir.mid == file->b.o.mdir.mid) {
            lfs3_traversal_clobber(lfs3, (lfs3_traversal_t*)o);
        }
    }

    // mark as synced
    file->b.o.flags &= ~LFS3_o_UNSYNC
            & ~LFS3_o_UNFLUSH
            & ~LFS3_o_UNCRYST
            & ~LFS3_o_UNCREAT;
    return 0;
}
#endif

#ifdef LFS3_KVONLY
static
#endif
int lfs3_file_sync(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    // removed? ignore sync requests
    if (lfs3_o_iszombie(file->b.o.flags)) {
        return 0;
    }

    #ifndef LFS3_RDONLY
    // first flush any data in our cache, this is a noop if already
    // flushed
    //
    // note that flush does not change the actual file data, so if
    // flush succeeds but mdir commit fails it's ok to fall back to
    // our flushed state
    //
    // though don't flush quite yet if our file is small and can be
    // combined with sync in a single commit
    int err;
    if (!(file->cache.size == lfs3_file_size_(file)
            && file->cache.size <= lfs3->cfg->inline_size
            && file->cache.size <= lfs3->cfg->fragment_size
            && file->cache.size < lfs3->cfg->crystal_thresh)) {
        err = lfs3_file_flush(lfs3, file);
        if (err) {
            goto failed;
        }
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
    file->b.o.flags &= ~LFS3_O_DESYNC;
    return 0;

    #ifndef LFS3_RDONLY
failed:;
    file->b.o.flags |= LFS3_O_DESYNC;
    return err;
    #endif
}

#ifndef LFS3_KVONLY
int lfs3_file_desync(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    (void)file;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    #ifndef LFS3_RDONLY
    // mark as desynced
    file->b.o.flags |= LFS3_O_DESYNC;
    #endif
    return 0;
}
#endif

#ifndef LFS3_KVONLY
int lfs3_file_resync(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    (void)file;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    #ifndef LFS3_RDONLY
    // removed? we can't resync
    int err;
    if (lfs3_o_iszombie(file->b.o.flags)) {
        err = LFS3_ERR_NOENT;
        goto failed;
    }

    // do nothing if already in-sync
    if (lfs3_o_isunsync(file->b.o.flags)) {
        // discard cached state
        lfs3_file_discardbshrub(file);
        lfs3_file_discardcache(file);
        lfs3_file_discardleaf(file);

        // refetch the file struct from disk
        err = lfs3_file_fetch(lfs3, file,
                // don't truncate again!
                file->b.o.flags & ~LFS3_O_TRUNC);
        if (err) {
            goto failed;
        }
    }
    #endif

    // clear desync flag
    file->b.o.flags &= ~LFS3_O_DESYNC;
    return 0;

    #ifndef LFS3_RDONLY
failed:;
    file->b.o.flags |= LFS3_O_DESYNC;
    return err;
    #endif
}
#endif

// other file operations

#ifndef LFS3_KVONLY
lfs3_soff_t lfs3_file_seek(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_soff_t off, uint8_t whence) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

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
#endif

#ifndef LFS3_KVONLY
lfs3_soff_t lfs3_file_tell(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    return file->pos;
}
#endif

#ifndef LFS3_KVONLY
lfs3_soff_t lfs3_file_rewind(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    file->pos = 0;
    return 0;
}
#endif

#ifndef LFS3_KVONLY
lfs3_soff_t lfs3_file_size(lfs3_t *lfs3, lfs3_file_t *file) {
    (void)lfs3;
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));

    return lfs3_file_size_(file);
}
#endif

#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
int lfs3_file_truncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size_) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));
    // can't write to readonly files
    LFS3_ASSERT(!lfs3_o_isrdonly(file->b.o.flags));

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

    // clobber entangled traversals
    lfs3_omdir_clobber(lfs3, &file->b.o, LFS3_t_DIRTY);
    // mark as unsynced in case we fail
    file->b.o.flags |= LFS3_o_UNSYNC;

    // checkpoint the allocator
    lfs3_alloc_ckpoint(lfs3);
    // truncate our btree
    err = lfs3_file_graft_(lfs3, file,
            lfs3_min(size, size_), size - lfs3_min(size, size_),
                +size_ - size,
            NULL, 0);
    if (err) {
        goto failed;
    }

    // truncate our leaf
    //
    // note we don't unconditionally discard to match fruncate, where we
    // _really_ don't want to discard erased-state
    file->leaf.bptr.d = LFS3_DATA_TRUNCATE(
            file->leaf.bptr.d,
            size_ - lfs3_min(file->leaf.pos, size_));
    file->leaf.weight = lfs3_min(
            file->leaf.weight,
            size_ - lfs3_min(file->leaf.pos, size_));
    file->leaf.pos = lfs3_min(file->leaf.pos, size_);
    #ifndef LFS3_2BONLY
    // mark as crystallized if this truncates our erased-state
    if (lfs3_bptr_off(&file->leaf.bptr)
                + lfs3_bptr_size(&file->leaf.bptr)
            < lfs3_bptr_cksize(&file->leaf.bptr)) {
        lfs3_bptr_claim(&file->leaf.bptr);
        file->b.o.flags &= ~LFS3_o_UNCRYST;
    }
    #endif
    // discard if our leaf is a fragment, is fragmented, or is completed
    // truncated, we can't rely on any in-bshrub/btree state
    if (!lfs3_bptr_isbptr(&file->leaf.bptr)
            || lfs3_bptr_size(&file->leaf.bptr)
                <= lfs3->cfg->fragment_size) {
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
    file->b.o.flags |= LFS3_O_DESYNC;
    return err;
}
#endif

#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
int lfs3_file_fruncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size_) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));
    // can't write to readonly files
    LFS3_ASSERT(!lfs3_o_isrdonly(file->b.o.flags));

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

    // clobber entangled traversals
    lfs3_omdir_clobber(lfs3, &file->b.o, LFS3_t_DIRTY);
    // mark as unsynced in case we fail
    file->b.o.flags |= LFS3_o_UNSYNC;

    // checkpoint the allocator
    lfs3_alloc_ckpoint(lfs3);
    // fruncate our btree
    err = lfs3_file_graft_(lfs3, file,
            0, lfs3_smax(size - size_, 0),
                +size_ - size,
            NULL, 0);
    if (err) {
        goto failed;
    }

    // fruncate our leaf
    //
    // note we _really_ don't want to discard erased-state if possible,
    // as fruncate is intended for logging operations, otherwise we'd
    // just unconditionally discard the leaf and avoid this hassle
    file->leaf.bptr.d = LFS3_DATA_FRUNCATE(
            file->leaf.bptr.d,
            lfs3_bptr_size(&file->leaf.bptr) - lfs3_min(
                lfs3_smax(
                    size - size_ - file->leaf.pos,
                    0),
                lfs3_bptr_size(&file->leaf.bptr)));
    file->leaf.weight -= lfs3_min(
            lfs3_smax(
                size - size_ - file->leaf.pos,
                0),
            file->leaf.weight);
    file->leaf.pos -= lfs3_smin(
            size - size_,
            file->leaf.pos);
    // discard if our leaf is a fragment, is fragmented, or is completed
    // truncated, we can't rely on any in-bshrub/btree state
    if (!lfs3_bptr_isbptr(&file->leaf.bptr)
            || lfs3_bptr_size(&file->leaf.bptr)
                <= lfs3->cfg->fragment_size) {
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
    // of file, though we can't let pos go negative
    file->pos -= lfs3_smin(
            size - size_,
            file->pos);

    return 0;

failed:;
    // mark as desync so lfs3_file_close doesn't write to disk
    file->b.o.flags |= LFS3_O_DESYNC;
    return err;
}
#endif

// file check functions

#if !defined(LFS3_KVONLY) && !defined(LFS3_2BONLY)
static int lfs3_file_ck(lfs3_t *lfs3, const lfs3_file_t *file,
        uint32_t flags) {
    // traverse the file's bshrub/btree
    lfs3_btraversal_t bt;
    lfs3_btraversal_init(&bt);
    while (true) {
        lfs3_tag_t tag;
        lfs3_data_t data;
        int err = lfs3_bshrub_traverse(lfs3, &file->b, &bt,
                NULL, &tag, NULL, &data);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
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
            err = lfs3_rbyd_fetchck(lfs3, rbyd,
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
            err = lfs3_data_readbptr(lfs3, &data,
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
#endif

#ifndef LFS3_KVONLY
int lfs3_file_ckmeta(lfs3_t *lfs3, lfs3_file_t *file) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));
    // can't read from writeonly files
    LFS3_ASSERT(!lfs3_o_iswronly(file->b.o.flags));

    #ifndef LFS3_2BONLY
    return lfs3_file_ck(lfs3, file,
            LFS3_T_RDONLY | LFS3_T_CKMETA);
    #else
    // in 2-block mode this is a noop
    (void)lfs3;
    (void)file;
    return 0;
    #endif
}
#endif

#ifndef LFS3_KVONLY
int lfs3_file_ckdata(lfs3_t *lfs3, lfs3_file_t *file) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &file->b.o));
    // can't read from writeonly files
    LFS3_ASSERT(!lfs3_o_iswronly(file->b.o.flags));

    // in 2-block mode this is a noop
    #ifndef LFS3_2BONLY
    return lfs3_file_ck(lfs3, file,
            LFS3_T_RDONLY | LFS3_T_CKMETA | LFS3_T_CKDATA);
    #else
    (void)lfs3;
    (void)file;
    return 0;
    #endif
}
#endif



/// Simple key-value API ///

// a simple key-value API is easier to use if your file fits in RAM, and
// if that's all you need you can potentially compile-out the more
// advanced file operations

// kv file config, we need to explicitly disable the file cache
static const struct lfs3_file_config lfs3_file_kvconfig = {
    // TODO is this the best way to do this?
    .cache_buffer = (uint8_t*)1,
    .cache_size = 0,
};

lfs3_ssize_t lfs3_get(lfs3_t *lfs3, const char *path,
        void *buffer, lfs3_size_t size) {
    // we just use the file API here, but with no cache so all reads
    // bypass the cache
    lfs3_file_t file;
    int err = lfs3_file_opencfg(lfs3, &file, path, LFS3_O_RDONLY,
            &lfs3_file_kvconfig);
    if (err) {
        return err;
    }

    #ifdef LFS3_KVONLY
    lfs3_ssize_t size_ = lfs3_file_readget_(lfs3, &file, buffer, size);
    #else
    lfs3_ssize_t size_ = lfs3_file_read(lfs3, &file, buffer, size);
    #endif

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
    int err = lfs3_file_opencfg(lfs3, &file, path, LFS3_O_RDONLY,
            &lfs3_file_kvconfig);
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
    // LFS3_o_WRSET is a special mode specifically to make lfs3_set work
    // atomically when possible
    //
    // - if we need to reserve the mid _and_ we're small, everything is
    //   committed/broadcasted in lfs3_file_opencfg
    //
    // - otherwise (exists? stickynote?), we flush/sync/broadcast
    //   normally in lfs3_file_close, lfs3_file_sync has its own logic
    //   to try to commit small files atomically
    //
    struct lfs3_file_config cfg = {
        .cache_buffer = (uint8_t*)buffer,
        .cache_size = size,
    };
    lfs3_file_t file;
    int err = lfs3_file_opencfg_(lfs3, &file, path,
            LFS3_o_WRSET | LFS3_O_CREAT | LFS3_O_TRUNC,
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
        const struct lfs3_config *cfg) {
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_M_RDWR)
                | LFS3_M_RDONLY
                | LFS3_M_FLUSH
                | LFS3_M_SYNC
                | LFS3_IFDEF_REVDBG(LFS3_M_REVDBG, 0)
                | LFS3_IFDEF_REVNOISE(LFS3_M_REVNOISE, 0)
                | LFS3_IFDEF_CKPROGS(LFS3_M_CKPROGS, 0)
                | LFS3_IFDEF_CKFETCHES(LFS3_M_CKFETCHES, 0)
                | LFS3_IFDEF_CKMETAPARITY(LFS3_M_CKMETAPARITY, 0)
                | LFS3_IFDEF_CKDATACKSUMREADS(
                    LFS3_M_CKDATACKSUMREADS,
                    0))) == 0);
    // LFS3_M_REVDBG and LFS3_M_REVNOISE are incompatible
    #if defined(LFS3_REVNOISE) && defined(LFS3_REVDBG)
    LFS3_ASSERT(!lfs3_m_isrevdbg(flags) || !lfs3_m_isrevnoise(flags));
    #endif

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
    // 2-block mode only supports... 2 blocks
    #ifdef LFS3_2BLOCK
    LFS3_ASSERT(lfs3->cfg->block_count == 2);
    #endif

    #ifdef LFS3_GC
    // unknown gc flags?
    LFS3_ASSERT((lfs3->cfg->gc_flags & ~(
            LFS3_GC_MKCONSISTENT
                | LFS3_GC_LOOKAHEAD
                | LFS3_GC_COMPACT
                | LFS3_GC_CKMETA
                | LFS3_GC_CKDATA)) == 0);

    // check that gc_compact_thresh makes sense
    //
    // metadata can't be compacted below block_size/2, and metadata can't
    // exceed a block
    LFS3_ASSERT(lfs3->cfg->gc_compact_thresh == 0
            || lfs3->cfg->gc_compact_thresh >= lfs3->cfg->block_size/2);
    LFS3_ASSERT(lfs3->cfg->gc_compact_thresh == (lfs3_size_t)-1
            || lfs3->cfg->gc_compact_thresh <= lfs3->cfg->block_size);
    #endif

    #ifndef LFS3_RDONLY
    // inline_size must be <= block_size/4
    LFS3_ASSERT(lfs3->cfg->inline_size <= lfs3->cfg->block_size/4);
    // fragment_size must be <= block_size/4
    LFS3_ASSERT(lfs3->cfg->fragment_size <= lfs3->cfg->block_size/4);
    #endif

    // setup flags
    lfs3->flags = flags
            // assume we contain orphans until proven otherwise
            | LFS3_IFDEF_RDONLY(0, LFS3_I_MKCONSISTENT)
            // default to an empty lookahead
            | LFS3_IFDEF_RDONLY(0, LFS3_I_LOOKAHEAD)
            // default to assuming we need compaction somewhere, worst case
            // this just makes lfs3_fs_gc read more than is strictly needed
            | LFS3_IFDEF_RDONLY(0, LFS3_I_COMPACT)
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
    #if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
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
    lfs3->lookahead.size = 0;
    lfs3->lookahead.ckpoint = 0;
    lfs3_alloc_discard(lfs3);
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
    // Add 1, to include the initial erase, multiply by 2, since we
    // alternate which metadata block we erase each compaction, and limit
    // to 28-bits so we always have some bits to determine the most recent
    // revision.
    #ifndef LFS3_RDONLY
    if (lfs3->cfg->block_recycles != -1) {
        lfs3->recycle_bits = lfs3_min(
                lfs3_nlog2(2*(lfs3->cfg->block_recycles+1)+1)-1,
                28);
    } else {
        lfs3->recycle_bits = -1;
    }
    #endif

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
    // Note this is different from LFS3_TAG_DSIZE, which is the worst case
    // tag encoding at compile-time.
    //
    #ifndef LFS3_RDONLY
    uint8_t tag_estimate
            = 2
            + (lfs3_nlog2(lfs3->file_limit+1)+7-1)/7
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
    lfs3->omdirs = NULL;

    // zero in-flight graft state
    lfs3->graft = NULL;
    lfs3->graft_count = 0;

    // zero gstate
    lfs3->gcksum = 0;
    #ifndef LFS3_RDONLY
    lfs3->gcksum_p = 0;
    lfs3->gcksum_d = 0;
    #endif

    lfs3->grm.queue[0] = -1;
    lfs3->grm.queue[1] = -1;
    #ifndef LFS3_RDONLY
    lfs3_memset(lfs3->grm_p, 0, LFS3_GRM_DSIZE);
    lfs3_memset(lfs3->grm_d, 0, LFS3_GRM_DSIZE);
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

    #if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
    if (!lfs3->cfg->lookahead_buffer) {
        lfs3_free(lfs3->lookahead.buffer);
    }
    #endif

    return 0;
}



/// Mount/unmount ///

// compatibility flags
//
// - RCOMPAT => Must understand to read the filesystem
// - WCOMPAT => Must understand to write to the filesystem
// - OCOMPAT => No understanding necessary, we don't really use these
//
// note, "understanding" does not necessarily mean support
//
#define LFS3_RCOMPAT_NONSTANDARD 0x00000001 // Non-standard filesystem format
#define LFS3_RCOMPAT_WRONLY      0x00000002 // Reading is disallowed
#define LFS3_RCOMPAT_BMOSS       0x00000010 // Files may use inlined data
#define LFS3_RCOMPAT_BSPROUT     0x00000020 // Files may use block pointers
#define LFS3_RCOMPAT_BSHRUB      0x00000040 // Files may use inlined btrees
#define LFS3_RCOMPAT_BTREE       0x00000080 // Files may use btrees
#define LFS3_RCOMPAT_MMOSS       0x00000100 // May use an inlined mdir
#define LFS3_RCOMPAT_MSPROUT     0x00000200 // May use an mdir pointer
#define LFS3_RCOMPAT_MSHRUB      0x00000400 // May use an inlined mtree
#define LFS3_RCOMPAT_MTREE       0x00000800 // May use an mtree
#define LFS3_RCOMPAT_GRM         0x00001000 // Global-remove in use
// internal
#define LFS3_rcompat_OVERFLOW    0x80000000 // Can't represent all flags

#define LFS3_RCOMPAT_COMPAT \
    (LFS3_RCOMPAT_BSHRUB \
        | LFS3_RCOMPAT_BTREE \
        | LFS3_RCOMPAT_MMOSS \
        | LFS3_RCOMPAT_MTREE \
        | LFS3_RCOMPAT_GRM)

#define LFS3_WCOMPAT_NONSTANDARD 0x00000001 // Non-standard filesystem format
#define LFS3_WCOMPAT_RDONLY      0x00000002 // Writing is disallowed
#define LFS3_WCOMPAT_DIR         0x00000010 // Directory files in use
#define LFS3_WCOMPAT_GCKSUM      0x00001000 // Global-checksum in use
// internal
#define LFS3_wcompat_OVERFLOW    0x80000000 // Can't represent all flags

#define LFS3_WCOMPAT_COMPAT \
    (LFS3_WCOMPAT_DIR \
        | LFS3_WCOMPAT_GCKSUM)

#define LFS3_OCOMPAT_NONSTANDARD 0x00000001 // Non-standard filesystem format
// internal
#define LFS3_ocompat_OVERFLOW    0x80000000 // Can't represent all flags

#define LFS3_OCOMPAT_COMPAT 0

typedef uint32_t lfs3_rcompat_t;
typedef uint32_t lfs3_wcompat_t;
typedef uint32_t lfs3_ocompat_t;

static inline bool lfs3_rcompat_isincompat(lfs3_rcompat_t rcompat) {
    return rcompat != LFS3_RCOMPAT_COMPAT;
}

static inline bool lfs3_wcompat_isincompat(lfs3_wcompat_t wcompat) {
    return wcompat != LFS3_WCOMPAT_COMPAT;
}

static inline bool lfs3_ocompat_isincompat(lfs3_ocompat_t ocompat) {
    return ocompat != LFS3_OCOMPAT_COMPAT;
}

// compat flags on-disk encoding
//
// little-endian, truncated bits must be assumed zero

static int lfs3_data_readcompat(lfs3_t *lfs3, lfs3_data_t *data,
        uint32_t *compat) {
    // allow truncated compat flags
    uint8_t buf[4] = {0};
    lfs3_ssize_t d = lfs3_data_read(lfs3, data, buf, 4);
    if (d < 0) {
        return d;
    }
    *compat = lfs3_fromle32(buf);

    // if any out-of-range flags are set, set the internal overflow bit,
    // this is a compromise in correctness and and compat-flag complexity
    //
    // we don't really care about performance here
    while (lfs3_data_size(*data) > 0) {
        uint8_t b;
        lfs3_ssize_t d = lfs3_data_read(lfs3, data, &b, 1);
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

static inline int lfs3_data_readrcompat(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_rcompat_t *rcompat) {
    return lfs3_data_readcompat(lfs3, data, rcompat);
}

static inline int lfs3_data_readwcompat(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_wcompat_t *wcompat) {
    return lfs3_data_readcompat(lfs3, data, wcompat);
}

static inline int lfs3_data_readocompat(lfs3_t *lfs3, lfs3_data_t *data,
        lfs3_ocompat_t *ocompat) {
    return lfs3_data_readcompat(lfs3, data, ocompat);
}


// disk geometry
//
// note these are stored minus 1 to avoid overflow issues
struct lfs3_geometry {
    lfs3_off_t block_size;
    lfs3_off_t block_count;
};

// geometry on-disk encoding
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
    int err = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_VERSION,
            NULL, &data);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }
    if (err != LFS3_ERR_NOENT) {
        lfs3_ssize_t d = lfs3_data_read(lfs3, &data, version, 2);
        if (d < 0) {
            return err;
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

    // check for any rcompatflags, we must understand these to read
    // the filesystem
    lfs3_rcompat_t rcompat = 0;
    err = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_RCOMPAT,
            NULL, &data);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }
    if (err != LFS3_ERR_NOENT) {
        err = lfs3_data_readrcompat(lfs3, &data, &rcompat);
        if (err) {
            return err;
        }
    }

    if (lfs3_rcompat_isincompat(rcompat)) {
        LFS3_ERROR("Incompatible rcompat flags 0x%0"PRIx32" (!= 0x%0"PRIx32")",
                rcompat,
                LFS3_RCOMPAT_COMPAT);
        return LFS3_ERR_NOTSUP;
    }

    // check for any wcompatflags, we must understand these to write
    // the filesystem
    #ifndef LFS3_RDONLY
    lfs3_wcompat_t wcompat = 0;
    err = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_WCOMPAT,
            NULL, &data);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }
    if (err != LFS3_ERR_NOENT) {
        err = lfs3_data_readwcompat(lfs3, &data, &wcompat);
        if (err) {
            return err;
        }
    }

    if (lfs3_wcompat_isincompat(wcompat)) {
        LFS3_WARN("Incompatible wcompat flags 0x%0"PRIx32" (!= 0x%0"PRIx32")",
                wcompat,
                LFS3_WCOMPAT_COMPAT);
        // we can continue if rdonly
        if (!lfs3_m_isrdonly(lfs3->flags)) {
            return LFS3_ERR_NOTSUP;
        }
    }
    #endif

    // we don't bother to check for any ocompatflags, we would just
    // ignore these anyways

    // check the on-disk geometry
    lfs3_geometry_t geometry;
    err = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_GEOMETRY,
            NULL, &data);
    if (err) {
        if (err == LFS3_ERR_NOENT) {
            LFS3_ERROR("No geometry found");
            return LFS3_ERR_INVAL;
        }
        return err;
    }
    err = lfs3_data_readgeometry(lfs3, &data, &geometry);
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
    err = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_NAMELIMIT,
            NULL, &data);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }
    if (err != LFS3_ERR_NOENT) {
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
    err = lfs3_mdir_lookup(lfs3, mroot, LFS3_TAG_FILELIMIT,
            NULL, &data);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }
    if (err != LFS3_ERR_NOENT) {
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

    // check for unknown configs
    lfs3_tag_t tag;
    err = lfs3_mdir_lookupnext(lfs3, mroot, LFS3_TAG_UNKNOWNCONFIG,
            &tag, NULL);
    if (err && err != LFS3_ERR_NOENT) {
        return err;
    }

    if (err != LFS3_ERR_NOENT
            && lfs3_tag_suptype(tag) == LFS3_TAG_CONFIG) {
        LFS3_ERROR("Unknown config 0x%04"PRIx16,
                tag);
        return LFS3_ERR_NOTSUP;
    }

    return 0;
}

static int lfs3_mountinited(lfs3_t *lfs3) {
    // mark mroot as invalid to prevent lfs3_mtree_traverse from getting
    // confused
    lfs3->mroot.mid = -1;
    lfs3->mroot.r.blocks[0] = -1;
    lfs3->mroot.r.blocks[1] = -1;

    // default to no mtree, this is allowed and implies all files are inlined
    // in the mroot
    #ifndef LFS3_2BONLY
    lfs3_btree_init(&lfs3->mtree);
    #endif

    // zero gcksum/gdeltas, we'll read these from our mdirs
    lfs3->gcksum = 0;
    lfs3_fs_flushgdelta(lfs3);

    // traverse the mtree rooted at mroot 0x{1,0}
    //
    // we do validate btree inner nodes here, how can we trust our
    // mdirs are valid if we haven't checked the btree inner nodes at
    // least once?
    lfs3_traversal_t t;
    lfs3_traversal_init(&t, LFS3_T_RDONLY | LFS3_T_MTREEONLY | LFS3_T_CKMETA);
    while (true) {
        lfs3_tag_t tag;
        lfs3_bptr_t bptr;
        int err = lfs3_mtree_traverse(lfs3, &t,
                &tag, &bptr);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
        }

        // found an mdir?
        if (tag == LFS3_TAG_MDIR) {
            lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr.d.u.buffer;
            // found an mroot?
            if (mdir->mid == -1) {
                // check for the magic string, all mroot should have this
                lfs3_data_t data;
                err = lfs3_mdir_lookup(lfs3, mdir, LFS3_TAG_MAGIC,
                        NULL, &data);
                if (err) {
                    if (err == LFS3_ERR_NOENT) {
                        LFS3_ERROR("No littlefs magic found");
                        return LFS3_ERR_CORRUPT;
                    }
                    return err;
                }

                // treat corrupted magic as no magic
                lfs3_scmp_t cmp = lfs3_data_cmp(lfs3, data, "littlefs", 8);
                if (cmp < 0) {
                    return cmp;
                }
                if (cmp != LFS3_CMP_EQ) {
                    LFS3_ERROR("No littlefs magic found");
                    return LFS3_ERR_CORRUPT;
                }

                // are we the last mroot?
                err = lfs3_mdir_lookup(lfs3, mdir, LFS3_TAG_MROOT,
                        NULL, NULL);
                if (err && err != LFS3_ERR_NOENT) {
                    return err;
                }
                if (err == LFS3_ERR_NOENT) {
                    // track active mroot
                    lfs3->mroot = *mdir;

                    // mount/validate config in active mroot
                    err = lfs3_mountmroot(lfs3, &lfs3->mroot);
                    if (err) {
                        return err;
                    }
                }
            }

            // build gcksum out of mdir cksums
            lfs3->gcksum ^= mdir->r.cksum;

            // collect any gdeltas from this mdir
            err = lfs3_fs_consumegdelta(lfs3, mdir);
            if (err) {
                return err;
            }

        // found an mtree inner-node?
        } else if (LFS3_IFDEF_2BONLY(false, tag == LFS3_TAG_BRANCH)) {
            #ifndef LFS3_2BONLY
            lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr.d.u.buffer;
            // found the root of the mtree? keep track of this
            if (lfs3->mtree.weight == 0) {
                lfs3->mtree = *rbyd;
            }
            #endif

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

    // once we've mounted and derived a pseudo-random seed, initialize our
    // block allocator
    //
    // the purpose of this is to avoid bad wear patterns such as always 
    // allocating blocks near the beginning of disk after a power-loss
    //
    #if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
    lfs3->lookahead.window = lfs3->gcksum % lfs3->block_count;
    #endif

    // TODO should the consumegdelta above take gstate/gdelta as a parameter?
    // keep track of the current gstate on disk
    #ifndef LFS3_RDONLY
    lfs3_memcpy(lfs3->grm_p, lfs3->grm_d, LFS3_GRM_DSIZE);
    #endif

    // decode grm so we can report any removed files as missing
    int err = lfs3_data_readgrm(lfs3,
            &LFS3_DATA_BUF(lfs3->grm_d, LFS3_GRM_DSIZE));
    if (err) {
        // TODO switch to read-only?
        return err;
    }

    // found pending grms? this should only happen if we lost power
    if (lfs3_grm_count(lfs3) == 2) {
        LFS3_INFO("Found pending grm %"PRId32".%"PRId32" %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[1]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[1]));
    } else if (lfs3_grm_count(lfs3) == 1) {
        LFS3_INFO("Found pending grm %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]));
    }

    return 0;
}

// needed in lfs3_mount
static int lfs3_fs_gc_(lfs3_t *lfs3, lfs3_traversal_t *t,
        uint32_t flags, lfs3_soff_t steps);

int lfs3_mount(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_config *cfg) {
    #ifdef LFS3_YES_RDONLY
    flags |= LFS3_M_RDONLY;
    #endif
    #ifdef LFS3_YES_FLUSH
    flags |= LFS3_M_FLUSH;
    #endif
    #ifdef LFS3_YES_SYNC
    flags |= LFS3_M_SYNC;
    #endif
    #ifdef LFS3_YES_REVDBG
    flags |= LFS3_M_REVDBG;
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
    #ifdef LFS3_YES_CKDATACKSUMREADS
    flags |= LFS3_M_CKDATACKSUMREADS;
    #endif
    #ifdef LFS3_YES_MKCONSISTENT
    flags |= LFS3_M_MKCONSISTENT;
    #endif
    #ifdef LFS3_YES_LOOKAHEAD
    flags |= LFS3_M_LOOKAHEAD;
    #endif
    #ifdef LFS3_YES_COMPACT
    flags |= LFS3_M_COMPACT;
    #endif
    #ifdef LFS3_YES_CKMETA
    flags |= LFS3_M_CKMETA;
    #endif
    #ifdef LFS3_YES_CKDATA
    flags |= LFS3_M_CKDATA
    #endif

    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_M_RDWR)
                | LFS3_M_RDONLY
                | LFS3_M_FLUSH
                | LFS3_M_SYNC
                | LFS3_IFDEF_REVDBG(LFS3_M_REVDBG, 0)
                | LFS3_IFDEF_REVNOISE(LFS3_M_REVNOISE, 0)
                | LFS3_IFDEF_CKPROGS(LFS3_M_CKPROGS, 0)
                | LFS3_IFDEF_CKFETCHES(LFS3_M_CKFETCHES, 0)
                | LFS3_IFDEF_CKMETAPARITY(LFS3_M_CKMETAPARITY, 0)
                | LFS3_IFDEF_CKDATACKSUMREADS(LFS3_M_CKDATACKSUMREADS, 0)
                | LFS3_IFDEF_RDONLY(0, LFS3_M_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_M_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0, LFS3_M_COMPACT)
                | LFS3_M_CKMETA
                | LFS3_M_CKDATA)) == 0);
    // these flags require a writable filesystem
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_t_ismkconsistent(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_t_islookahead(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(flags) || !lfs3_t_iscompact(flags));

    int err = lfs3_init(lfs3,
            flags & (
                LFS3_IFDEF_RDONLY(0, LFS3_M_RDWR)
                    | LFS3_M_RDONLY
                    | LFS3_M_FLUSH
                    | LFS3_M_SYNC
                    | LFS3_IFDEF_REVDBG(LFS3_M_REVDBG, 0)
                    | LFS3_IFDEF_REVNOISE(LFS3_M_REVNOISE, 0)
                    | LFS3_IFDEF_CKPROGS(LFS3_M_CKPROGS, 0)
                    | LFS3_IFDEF_CKFETCHES(LFS3_M_CKFETCHES, 0)
                    | LFS3_IFDEF_CKMETAPARITY(LFS3_M_CKMETAPARITY, 0)
                    | LFS3_IFDEF_CKDATACKSUMREADS(LFS3_M_CKDATACKSUMREADS, 0)),
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
            LFS3_IFDEF_RDONLY(0, LFS3_M_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_M_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0, LFS3_M_COMPACT)
                | LFS3_M_CKMETA
                | LFS3_M_CKDATA)) {
        lfs3_traversal_t t;
        err = lfs3_fs_gc_(lfs3, &t,
                flags & (
                    LFS3_IFDEF_RDONLY(0, LFS3_M_MKCONSISTENT)
                        | LFS3_IFDEF_RDONLY(0, LFS3_M_LOOKAHEAD)
                        | LFS3_IFDEF_RDONLY(0, LFS3_M_COMPACT)
                        | LFS3_M_CKMETA
                        | LFS3_M_CKDATA),
                -1);
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
            LFS3_IFDEF_2BONLY(0, lfs3->mtree.weight) >> lfs3->mbits,
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
    LFS3_ASSERT(lfs3->omdirs == NULL
            // special case for our gc traversal handle
            || LFS3_IFDEF_GC(
                (lfs3->omdirs == &lfs3->gc.t.b.o
                    && lfs3->gc.t.b.o.next == NULL),
                false));

    return lfs3_deinit(lfs3);
}



/// Format ///

#ifndef LFS3_RDONLY
static int lfs3_formatinited(lfs3_t *lfs3) {
    for (int i = 0; i < 2; i++) {
        // write superblock to both rbyds in the root mroot to hopefully
        // avoid mounting an older filesystem on disk
        lfs3_rbyd_t rbyd = {.blocks[0]=i, .eoff=0, .trunk=0};

        int err = lfs3_bd_erase(lfs3, rbyd.blocks[0]);
        if (err) {
            return err;
        }

        // the initial revision count is arbitrary, but it's nice to have
        // something here to tell the initial mroot apart from btree nodes
        // (rev=0), it's also useful for start with -1 and 0 in the upper
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
        // - the root's bookmark tag, which reserves did = 0 for the root
        err = lfs3_rbyd_appendrattrs(lfs3, &rbyd, -1, -1, -1, LFS3_RATTRS(
                LFS3_RATTR_BUF(
                    LFS3_TAG_MAGIC, 0,
                    "littlefs", 8),
                LFS3_RATTR_BUF(
                    LFS3_TAG_VERSION, 0,
                    ((const uint8_t[2]){
                        LFS3_DISK_VERSION_MAJOR,
                        LFS3_DISK_VERSION_MINOR}), 2),
                LFS3_RATTR_LE32(
                    LFS3_TAG_RCOMPAT, 0,
                    LFS3_RCOMPAT_COMPAT),
                LFS3_RATTR_LE32(
                    LFS3_TAG_WCOMPAT, 0,
                    LFS3_WCOMPAT_COMPAT),
                LFS3_RATTR_GEOMETRY(
                    LFS3_TAG_GEOMETRY, 0,
                    (&(lfs3_geometry_t){
                        lfs3->cfg->block_size,
                        lfs3->cfg->block_count})),
                LFS3_RATTR_LLEB128(
                    LFS3_TAG_NAMELIMIT, 0,
                    lfs3->name_limit),
                LFS3_RATTR_LEB128(
                    LFS3_TAG_FILELIMIT, 0,
                    lfs3->file_limit),
                LFS3_RATTR_NAME(
                    LFS3_TAG_BOOKMARK, +1,
                    0, NULL, 0)));
        if (err) {
            return err;
        }

        // append initial gcksum
        uint32_t cksum = rbyd.cksum;
        err = lfs3_rbyd_appendrattr_(lfs3, &rbyd, LFS3_RATTR_LE32(
                LFS3_TAG_GCKSUMDELTA, 0, lfs3_crc32c_cube(cksum)));
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
    int err = lfs3_bd_sync(lfs3);
    if (err) {
        return err;
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
int lfs3_format(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_config *cfg) {
    #ifdef LFS3_YES_REVDBG
    flags |= LFS3_F_REVDBG;
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
    #ifdef LFS3_YES_CKDATACKSUMREADS
    flags |= LFS3_F_CKDATACKSUMREADS;
    #endif
    #ifdef LFS3_YES_CKMETA
    flags |= LFS3_F_CKMETA;
    #endif
    #ifdef LFS3_YES_CKDATA
    flags |= LFS3_F_CKDATA
    #endif

    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_F_RDWR
                | LFS3_IFDEF_REVDBG(LFS3_F_REVDBG, 0)
                | LFS3_IFDEF_REVNOISE(LFS3_F_REVNOISE, 0)
                | LFS3_IFDEF_CKPROGS(LFS3_F_CKPROGS, 0)
                | LFS3_IFDEF_CKFETCHES(LFS3_F_CKFETCHES, 0)
                | LFS3_IFDEF_CKMETAPARITY(LFS3_F_CKMETAPARITY, 0)
                | LFS3_IFDEF_CKDATACKSUMREADS(LFS3_F_CKDATACKSUMREADS, 0)
                | LFS3_F_CKMETA
                | LFS3_F_CKDATA)) == 0);

    int err = lfs3_init(lfs3,
            flags & (
                LFS3_F_RDWR
                    | LFS3_IFDEF_REVDBG(LFS3_F_REVDBG, 0)
                    | LFS3_IFDEF_REVNOISE(LFS3_F_REVNOISE, 0)
                    | LFS3_IFDEF_CKPROGS(LFS3_F_CKPROGS, 0)
                    | LFS3_IFDEF_CKFETCHES(LFS3_F_CKFETCHES, 0)
                    | LFS3_IFDEF_CKMETAPARITY(LFS3_F_CKMETAPARITY, 0)
                    | LFS3_IFDEF_CKDATACKSUMREADS(LFS3_F_CKDATACKSUMREADS, 0)),
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
            LFS3_F_CKMETA
                | LFS3_F_CKDATA)) {
        lfs3_traversal_t t;
        err = lfs3_fs_gc_(lfs3, &t,
                flags & (
                    LFS3_F_CKMETA
                        | LFS3_F_CKDATA),
                -1);
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

int lfs3_fs_stat(lfs3_t *lfs3, struct lfs3_fsinfo *fsinfo) {
    // return various filesystem flags
    fsinfo->flags = lfs3->flags & (
            LFS3_I_RDONLY
                | LFS3_I_FLUSH
                | LFS3_I_SYNC
                | LFS3_IFDEF_REVDBG(LFS3_I_REVDBG, 0)
                | LFS3_IFDEF_REVNOISE(LFS3_I_REVNOISE, 0)
                | LFS3_IFDEF_CKPROGS(LFS3_I_CKPROGS, 0)
                | LFS3_IFDEF_CKFETCHES(LFS3_I_CKFETCHES, 0)
                | LFS3_IFDEF_CKMETAPARITY(LFS3_I_CKMETAPARITY, 0)
                | LFS3_IFDEF_CKDATACKSUMREADS(LFS3_I_CKDATACKSUMREADS, 0)
                | LFS3_IFDEF_RDONLY(0, LFS3_I_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_I_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0, LFS3_I_COMPACT)
                | LFS3_I_CKMETA
                | LFS3_I_CKDATA);
    // some flags we calculate on demand
    #ifndef LFS3_RDONLY
    fsinfo->flags |= (lfs3_grm_count(lfs3) > 0) ? LFS3_I_MKCONSISTENT : 0;
    #endif

    // return filesystem config, this may come from disk
    fsinfo->block_size = lfs3->cfg->block_size;
    fsinfo->block_count = lfs3->block_count;
    fsinfo->name_limit = lfs3->name_limit;
    fsinfo->file_limit = lfs3->file_limit;

    return 0;
}

lfs3_ssize_t lfs3_fs_usage(lfs3_t *lfs3) {
    lfs3_size_t count = 0;
    lfs3_traversal_t t;
    lfs3_traversal_init(&t, LFS3_T_RDONLY);
    while (true) {
        lfs3_tag_t tag;
        lfs3_bptr_t bptr;
        int err = lfs3_mtree_traverse(lfs3, &t,
                &tag, &bptr);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
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


// consistency stuff

#ifndef LFS3_RDONLY
static int lfs3_fs_fixgrm(lfs3_t *lfs3) {
    if (lfs3_grm_count(lfs3) == 2) {
        LFS3_INFO("Fixing grm %"PRId32".%"PRId32" %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[1]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[1]));
    } else if (lfs3_grm_count(lfs3) == 1) {
        LFS3_INFO("Fixing grm %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, lfs3->grm.queue[0]),
                lfs3_dbgmrid(lfs3, lfs3->grm.queue[0]));
    }

    while (lfs3_grm_count(lfs3) > 0) {
        LFS3_ASSERT(lfs3->grm.queue[0] != -1);

        // find our mdir
        lfs3_mdir_t mdir;
        int err = lfs3_mtree_lookup(lfs3, lfs3->grm.queue[0],
                &mdir);
        if (err) {
            LFS3_ASSERT(err != LFS3_ERR_NOENT);
            return err;
        }

        // we also use grm to track orphans that need to be cleaned up,
        // which means it may not match the on-disk state, which means
        // we need to revert manually on error
        lfs3_grm_t grm_p = lfs3->grm;

        // mark grm as taken care of
        lfs3_grm_pop(lfs3);
        // checkpoint the allocator
        lfs3_alloc_ckpoint(lfs3);
        // remove the rid while atomically updating our grm
        err = lfs3_mdir_commit(lfs3, &mdir, LFS3_RATTRS(
                LFS3_RATTR(LFS3_TAG_RM, -1)));
        if (err) {
            // revert grm manually
            lfs3->grm = grm_p;
            return err;
        }
    }

    return 0;
}
#endif

#ifndef LFS3_RDONLY
static int lfs3_mdir_mkconsistent(lfs3_t *lfs3, lfs3_mdir_t *mdir) {
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
        if (lfs3_omdir_ismidopen(lfs3, mdir->mid, -1)) {
            mdir->mid += 1;
            continue;
        }

        // is this mid marked as a stickynote?
        err = lfs3_rbyd_lookup(lfs3, &mdir->r,
                lfs3_mrid(lfs3, mdir->mid), LFS3_TAG_STICKYNOTE,
                NULL, NULL);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                mdir->mid += 1;
                continue;
            }
            goto failed;
        }

        // we found an orphaned stickynote, remove
        LFS3_INFO("Fixing orphaned stickynote %"PRId32".%"PRId32,
                lfs3_dbgmbid(lfs3, mdir->mid),
                lfs3_dbgmrid(lfs3, mdir->mid));

        // checkpoint the allocator
        lfs3_alloc_ckpoint(lfs3);
        // remove the orphaned stickynote
        err = lfs3_mdir_commit(lfs3, mdir, LFS3_RATTRS(
                LFS3_RATTR(LFS3_TAG_RM, -1)));
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
static int lfs3_fs_fixorphans(lfs3_t *lfs3) {
    // LFS3_T_MKCONSISTENT really just removes orphans
    lfs3_traversal_t t;
    lfs3_traversal_init(&t,
            LFS3_T_RDWR | LFS3_T_MTREEONLY | LFS3_T_MKCONSISTENT);
    while (true) {
        lfs3_bptr_t bptr;
        int err = lfs3_mtree_gc(lfs3, &t,
                NULL, &bptr);
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

    // fix pending grms
    if (lfs3_grm_count(lfs3) > 0) {
        int err = lfs3_fs_fixgrm(lfs3);
        if (err) {
            return err;
        }
    }

    // fix orphaned stickynotes
    //
    // this must happen after fixgrm, since removing orphaned
    // stickynotes risks outdating the grm
    //
    if (lfs3_t_ismkconsistent(lfs3->flags)) {
        int err = lfs3_fs_fixorphans(lfs3);
        if (err) {
            return err;
        }
    }

    return 0;
}
#endif

// filesystem check functions
static int lfs3_fs_ck(lfs3_t *lfs3, uint32_t flags) {
    // we leave this up to lfs3_mtree_traverse
    lfs3_traversal_t t;
    lfs3_traversal_init(&t, flags);
    while (true) {
        lfs3_bptr_t bptr;
        int err = lfs3_mtree_traverse(lfs3, &t,
                NULL, &bptr);
        if (err) {
            if (err == LFS3_ERR_NOENT) {
                break;
            }
            return err;
        }
    }

    return 0;
}

int lfs3_fs_ckmeta(lfs3_t *lfs3) {
    return lfs3_fs_ck(lfs3, LFS3_T_RDONLY | LFS3_T_CKMETA);
}

int lfs3_fs_ckdata(lfs3_t *lfs3) {
    return lfs3_fs_ck(lfs3, LFS3_T_RDONLY | LFS3_T_CKMETA | LFS3_T_CKDATA);
}

// get the filesystem checksum
int lfs3_fs_cksum(lfs3_t *lfs3, uint32_t *cksum) {
    *cksum = lfs3->gcksum;
    return 0;
}

// low-level filesystem gc
//
// runs the traversal until all work is completed, which may take
// multiple passes
static int lfs3_fs_gc_(lfs3_t *lfs3, lfs3_traversal_t *t,
        uint32_t flags, lfs3_soff_t steps) {
    // unknown gc flags?
    //
    // we should have check these earlier, but it doesn't hurt to
    // double check
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_T_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_T_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0, LFS3_T_COMPACT)
                | LFS3_T_CKMETA
                | LFS3_T_CKDATA)) == 0);
    // these flags require a writable filesystem
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags) || !lfs3_t_ismkconsistent(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags) || !lfs3_t_islookahead(flags));
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags) || !lfs3_t_iscompact(flags));
    // some flags don't make sense when only traversing the mtree
    LFS3_ASSERT(!lfs3_t_ismtreeonly(flags) || !lfs3_t_islookahead(flags));
    LFS3_ASSERT(!lfs3_t_ismtreeonly(flags) || !lfs3_t_isckdata(flags));

    // fix pending grms if requested
    #ifndef LFS3_RDONLY
    if (lfs3_t_ismkconsistent(flags)
            && lfs3_grm_count(lfs3) > 0) {
        int err = lfs3_fs_fixgrm(lfs3);
        if (err) {
            return err;
        }
    }
    #endif

    // do we have any pending work?
    uint32_t pending = flags & (
            (lfs3->flags & (
                LFS3_IFDEF_RDONLY(0, LFS3_I_MKCONSISTENT)
                    | LFS3_IFDEF_RDONLY(0, LFS3_I_LOOKAHEAD)
                    | LFS3_IFDEF_RDONLY(0, LFS3_I_COMPACT)
                    | LFS3_I_CKMETA
                    | LFS3_I_CKDATA)));

    while (pending && (lfs3_off_t)steps > 0) {
        // checkpoint the allocator to maximize any lookahead scans
        #ifndef LFS3_RDONLY
        lfs3_alloc_ckpoint(lfs3);
        #endif

        // start a new traversal?
        if (!lfs3_omdir_isopen(lfs3, &t->b.o)) {
            lfs3_traversal_init(t, pending);
            lfs3_omdir_open(lfs3, &t->b.o);
        }

        // don't bother with lookahead if we've mutated
        #ifndef LFS3_RDONLY
        if (lfs3_t_isdirty(t->b.o.flags)
                || lfs3_t_ismutated(t->b.o.flags)) {
            t->b.o.flags &= ~LFS3_T_LOOKAHEAD;
        }
        #endif

        // will this traversal still make progress? no? start over
        if (!(t->b.o.flags & (
                LFS3_IFDEF_RDONLY(0, LFS3_T_MKCONSISTENT)
                    | LFS3_IFDEF_RDONLY(0, LFS3_T_LOOKAHEAD)
                    | LFS3_IFDEF_RDONLY(0, LFS3_T_COMPACT)
                    | LFS3_T_CKMETA
                    | LFS3_T_CKDATA))) {
            lfs3_omdir_close(lfs3, &t->b.o);
            continue;
        }

        // do we really need a full traversal?
        if (!(t->b.o.flags & (
                LFS3_IFDEF_RDONLY(0, LFS3_T_LOOKAHEAD)
                    | LFS3_T_CKMETA
                    | LFS3_T_CKDATA))) {
            t->b.o.flags |= LFS3_T_MTREEONLY;
        }

        // progress gc
        lfs3_bptr_t bptr;
        int err = lfs3_mtree_gc(lfs3, t,
                NULL, &bptr);
        if (err && err != LFS3_ERR_NOENT) {
            return err;
        }

        // end of traversal?
        if (err == LFS3_ERR_NOENT) {
            lfs3_omdir_close(lfs3, &t->b.o);

            // clear any pending flags we make progress on
            pending &= lfs3->flags & (
                    LFS3_IFDEF_RDONLY(0, LFS3_I_MKCONSISTENT)
                        | LFS3_IFDEF_RDONLY(0, LFS3_I_LOOKAHEAD)
                        | LFS3_IFDEF_RDONLY(0, LFS3_I_COMPACT)
                        | LFS3_I_CKMETA
                        | LFS3_I_CKDATA);
        }

        // decrement steps
        if (steps > 0) {
            steps -= 1;
        }
    }

    return 0;
}

// incremental filesystem gc
//
// perform any pending janitorial work
#ifdef LFS3_GC
int lfs3_fs_gc(lfs3_t *lfs3) {
    return lfs3_fs_gc_(lfs3, &lfs3->gc.t,
            lfs3->cfg->gc_flags,
            (lfs3->cfg->gc_steps)
                ? lfs3->cfg->gc_steps
                : 1);
}
#endif

// unperform janitorial work
int lfs3_fs_unck(lfs3_t *lfs3, uint32_t flags) {
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_I_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_I_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0, LFS3_I_COMPACT)
                | LFS3_I_CKMETA
                | LFS3_I_CKDATA)) == 0);

    // reset the requested flags
    lfs3->flags |= flags;

    // and clear from any ongoing traversals
    //
    // lfs3_fs_gc will terminate early if it discovers it can no longer
    // make progress
    #ifdef LFS3_GC
    lfs3->gc.t.b.o.flags &= ~flags;
    #endif

    return 0;
}


// attempt to grow the filesystem
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
int lfs3_fs_grow(lfs3_t *lfs3, lfs3_size_t block_count_) {
    // filesystem must be writeable
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags));
    // shrinking the filesystem is not supported
    LFS3_ASSERT(block_count_ >= lfs3->block_count);

    // do nothing if block_count doesn't change
    if (block_count_ == lfs3->block_count) {
        return 0;
    }

    // Note we do _not_ call lfs3_fs_mkconsistent here. This is a bit scary,
    // but we should be ok as long as we patch grms in lfs3_mdir_commit and
    // only commit to the mroot.
    //
    // Calling lfs3_fs_mkconsistent risks locking our filesystem up trying
    // to fix grms/orphans before we can commit the new filesystem size. If
    // we don't, we should always be able to recover a stuck filesystem with
    // lfs3_fs_grow.

    LFS3_INFO("Growing littlefs %"PRId32"x%"PRId32" -> %"PRId32"x%"PRId32,
            lfs3->cfg->block_size, lfs3->block_count,
            lfs3->cfg->block_size, block_count_);

    // keep track of our current block_count in case we fail
    lfs3_size_t block_count = lfs3->block_count;

    // we can use the new blocks immediately as long as the commit
    // with the new block_count is atomic
    lfs3->block_count = block_count_;
    // discard stale lookahead buffer
    lfs3_alloc_discard(lfs3);

    // update our on-disk config
    lfs3_alloc_ckpoint(lfs3);
    int err = lfs3_mdir_commit(lfs3, &lfs3->mroot, LFS3_RATTRS(
            LFS3_RATTR_GEOMETRY(
                LFS3_TAG_GEOMETRY, 0,
                (&(lfs3_geometry_t){
                    lfs3->cfg->block_size,
                    block_count_}))));
    if (err) {
        goto failed;
    }

    return 0;

failed:;
    // restore block_count
    lfs3->block_count = block_count;
    // discard clobbered lookahead buffer
    lfs3_alloc_discard(lfs3);

    return err;
}
#endif



/// High-level filesystem traversal ///

// needed in lfs3_traversal_open
static int lfs3_traversal_rewind_(lfs3_t *lfs3, lfs3_traversal_t *t);

int lfs3_traversal_open(lfs3_t *lfs3, lfs3_traversal_t *t, uint32_t flags) {
    // already open?
    LFS3_ASSERT(!lfs3_omdir_isopen(lfs3, &t->b.o));
    // unknown flags?
    LFS3_ASSERT((flags & ~(
            LFS3_IFDEF_RDONLY(0, LFS3_T_RDWR)
                | LFS3_T_RDONLY
                | LFS3_T_MTREEONLY
                | LFS3_IFDEF_RDONLY(0, LFS3_T_MKCONSISTENT)
                | LFS3_IFDEF_RDONLY(0, LFS3_T_LOOKAHEAD)
                | LFS3_IFDEF_RDONLY(0, LFS3_T_COMPACT)
                | LFS3_T_CKMETA
                | LFS3_T_CKDATA)) == 0);
    // writeable traversals require a writeable filesystem
    LFS3_ASSERT(!lfs3_m_isrdonly(lfs3->flags) || lfs3_t_isrdonly(flags));
    // these flags require a writable traversal
    LFS3_ASSERT(!lfs3_t_isrdonly(flags) || !lfs3_t_ismkconsistent(flags));
    LFS3_ASSERT(!lfs3_t_isrdonly(flags) || !lfs3_t_islookahead(flags));
    LFS3_ASSERT(!lfs3_t_isrdonly(flags) || !lfs3_t_iscompact(flags));
    // some flags don't make sense when only traversing the mtree
    LFS3_ASSERT(!lfs3_t_ismtreeonly(flags) || !lfs3_t_islookahead(flags));
    LFS3_ASSERT(!lfs3_t_ismtreeonly(flags) || !lfs3_t_isckdata(flags));

    // setup traversal state
    t->b.o.flags = flags | lfs3_o_typeflags(LFS3_type_TRAVERSAL);

    // let rewind initialize/reset things
    int err = lfs3_traversal_rewind_(lfs3, t);
    if (err) {
        return err;
    }

    // add to tracked mdirs
    lfs3_omdir_open(lfs3, &t->b.o);
    return 0;
}

int lfs3_traversal_close(lfs3_t *lfs3, lfs3_traversal_t *t) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &t->b.o));

    // remove from tracked mdirs
    lfs3_omdir_close(lfs3, &t->b.o);
    return 0;
}

int lfs3_traversal_read(lfs3_t *lfs3, lfs3_traversal_t *t,
        struct lfs3_tinfo *tinfo) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &t->b.o));

    // check for pending grms every step, just in case some other
    // operation introduced new grms
    #ifndef LFS3_RDONLY
    if (lfs3_t_ismkconsistent(t->b.o.flags)
            && lfs3_grm_count(lfs3) > 0) {
        // swap dirty/mutated flags while mutating
        t->b.o.flags = lfs3_t_swapdirty(t->b.o.flags);

        int err = lfs3_fs_fixgrm(lfs3);
        if (err) {
            t->b.o.flags = lfs3_t_swapdirty(t->b.o.flags);
            return err;
        }

        t->b.o.flags = lfs3_t_swapdirty(t->b.o.flags);
    }
    #endif

    // checkpoint the allocator to maximize any lookahead scans
    #ifndef LFS3_RDONLY
    lfs3_alloc_ckpoint(lfs3);
    #endif

    while (true) {
        // some redund blocks left over?
        if (t->blocks[0] != -1) {
            // write our traversal info
            tinfo->btype = lfs3_t_btype(t->b.o.flags);
            tinfo->block = t->blocks[0];

            t->blocks[0] = t->blocks[1];
            t->blocks[1] = -1;
            return 0;
        }

        // find next block
        lfs3_tag_t tag;
        lfs3_bptr_t bptr;
        int err = lfs3_mtree_gc(lfs3, t,
                &tag, &bptr);
        if (err) {
            return err;
        }

        // figure out type/blocks
        if (tag == LFS3_TAG_MDIR) {
            lfs3_mdir_t *mdir = (lfs3_mdir_t*)bptr.d.u.buffer;
            lfs3_t_setbtype(&t->b.o.flags, LFS3_BTYPE_MDIR);
            t->blocks[0] = mdir->r.blocks[0];
            t->blocks[1] = mdir->r.blocks[1];

        } else if (tag == LFS3_TAG_BRANCH) {
            lfs3_t_setbtype(&t->b.o.flags, LFS3_BTYPE_BTREE);
            lfs3_rbyd_t *rbyd = (lfs3_rbyd_t*)bptr.d.u.buffer;
            t->blocks[0] = rbyd->blocks[0];
            t->blocks[1] = -1;

        } else if (tag == LFS3_TAG_BLOCK) {
            lfs3_t_setbtype(&t->b.o.flags, LFS3_BTYPE_DATA);
            t->blocks[0] = lfs3_bptr_block(&bptr);
            t->blocks[1] = -1;

        } else {
            LFS3_UNREACHABLE();
        }
    }
}

#ifndef LFS3_RDONLY
static void lfs3_traversal_clobber(lfs3_t *lfs3, lfs3_traversal_t *t) {
    (void)lfs3;
    // mroot/mtree? transition to mdir iteration
    if (LFS3_IFDEF_2BONLY(
            false,
            lfs3_t_tstate(t->b.o.flags) < LFS3_TSTATE_MDIRS)) {
        #ifndef LFS3_2BONLY
        lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIRS);
        t->b.o.mdir.mid = 0;
        lfs3_bshrub_init(&t->b);
        t->ot = NULL;
        #endif
    // in-mtree mdir? increment the mid (to make progress) and reset to
    // mdir iteration
    } else if (LFS3_IFDEF_2BONLY(
            false,
            lfs3_t_tstate(t->b.o.flags) < LFS3_TSTATE_OMDIRS)) {
        #ifndef LFS3_2BONLY
        lfs3_t_settstate(&t->b.o.flags, LFS3_TSTATE_MDIR);
        t->b.o.mdir.mid += 1;
        lfs3_bshrub_init(&t->b);
        t->ot = NULL;
        #endif
    // opened mdir? skip to next omdir
    } else if (lfs3_t_tstate(t->b.o.flags) < LFS3_TSTATE_DONE) {
        lfs3_t_settstate(&t->b.o.flags, LFS3_IFDEF_2BONLY(
                LFS3_TSTATE_DONE,
                LFS3_TSTATE_OMDIRS));
        lfs3_bshrub_init(&t->b);
        t->ot = (t->ot) ? t->ot->next : NULL;
    // done traversals should never need clobbering
    } else {
        LFS3_UNREACHABLE();
    }

    // and clear any pending blocks
    t->blocks[0] = -1;
    t->blocks[1] = -1;
}
#endif

static int lfs3_traversal_rewind_(lfs3_t *lfs3, lfs3_traversal_t *t) {
    (void)lfs3;

    // reset traversal
    lfs3_traversal_init(t,
            t->b.o.flags
                & ~LFS3_t_DIRTY
                & ~LFS3_t_MUTATED
                & ~LFS3_t_TSTATE);

    // and clear any pending blocks
    t->blocks[0] = -1;
    t->blocks[1] = -1;

    return 0;
}

int lfs3_traversal_rewind(lfs3_t *lfs3, lfs3_traversal_t *t) {
    LFS3_ASSERT(lfs3_omdir_isopen(lfs3, &t->b.o));

    return lfs3_traversal_rewind_(lfs3, t);
}



// that's it! you've reached the end! go home!
