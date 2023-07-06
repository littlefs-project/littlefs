/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "lfs.h"
#include "lfs_util.h"


// TODO do we still need these?
// some constants used throughout the code
#define LFS_BLOCK_NULL ((lfs_block_t)-1)
#define LFS_BLOCK_INLINE ((lfs_block_t)-2)

// TODO do we still need these?
enum {
    LFS_OK_RELOCATED = 1,
    LFS_OK_DROPPED   = 2,
    LFS_OK_ORPHANED  = 3,
};

// a normal compare enum, but shifted up by one to allow unioning with
// negative error codes
enum {
    LFS_CMP_LT = 0,
    LFS_CMP_EQ = 1,
    LFS_CMP_GT = 2,
};

typedef int lfs_scmp_t;

static inline int lfs_cmp(lfs_scmp_t cmp) {
    return cmp - 1;
}


/// Caching block device operations ///

static inline void lfs_cache_drop(lfs_t *lfs, lfs_cache_t *rcache) {
    // do not zero, cheaper if cache is readonly or only going to be
    // written with identical data (during relocates)
    (void)lfs;
    rcache->block = LFS_BLOCK_NULL;
}

static inline void lfs_cache_zero(lfs_t *lfs, lfs_cache_t *pcache) {
    // zero to avoid information leak
    memset(pcache->buffer, 0xff, lfs->cfg->cache_size);
    pcache->block = LFS_BLOCK_NULL;
}

static int lfs_bd_read(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    if (block >= lfs->cfg->block_count ||
            off+size > lfs->cfg->block_size) {
        return LFS_ERR_CORRUPT;
    }

    while (size > 0) {
        lfs_size_t diff = size;

        if (pcache && block == pcache->block &&
                off < pcache->off + pcache->size) {
            if (off >= pcache->off) {
                // is already in pcache?
                diff = lfs_min(diff, pcache->size - (off-pcache->off));
                memcpy(data, &pcache->buffer[off-pcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // pcache takes priority
            diff = lfs_min(diff, pcache->off-off);
        }

        if (block == rcache->block &&
                off < rcache->off + rcache->size) {
            if (off >= rcache->off) {
                // is already in rcache?
                diff = lfs_min(diff, rcache->size - (off-rcache->off));
                memcpy(data, &rcache->buffer[off-rcache->off], diff);

                data += diff;
                off += diff;
                size -= diff;
                continue;
            }

            // rcache takes priority
            diff = lfs_min(diff, rcache->off-off);
        }

        if (size >= hint && off % lfs->cfg->read_size == 0 &&
                size >= lfs->cfg->read_size) {
            // bypass cache?
            diff = lfs_aligndown(diff, lfs->cfg->read_size);
            int err = lfs->cfg->read(lfs->cfg, block, off, data, diff);
            if (err) {
                return err;
            }

            // TODO this was a quick hack, the entire cache system probably
            // requires a deeper look
            //
            // fix overlaps with our pcache
            if (pcache
                    && block == pcache->block
                    && off < pcache->off + pcache->size
                    && off + diff > pcache->off) {
                lfs_off_t off_ = lfs_max(off, pcache->off);
                lfs_size_t diff_ = lfs_min(
                        diff - (off_-off),
                        pcache->size - (off_-pcache->off));
                memcpy(&data[off_-off],
                        &pcache->buffer[off_-pcache->off],
                        diff_);
            }

            data += diff;
            off += diff;
            size -= diff;
            continue;
        }

        // load to cache, first condition can no longer fail
        LFS_ASSERT(block < lfs->cfg->block_count);
        rcache->block = block;
        rcache->off = lfs_aligndown(off, lfs->cfg->read_size);
        rcache->size = lfs_min(
                lfs_min(
                    lfs_alignup(off+lfs_max(size, hint), lfs->cfg->read_size),
                    lfs->cfg->block_size)
                - rcache->off,
                lfs->cfg->cache_size);
        int err = lfs->cfg->read(lfs->cfg, rcache->block,
                rcache->off, rcache->buffer, rcache->size);
        LFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }

        // TODO this was a quick hack, the entire cache system probably
        // requires a deeper look
        //
        // fix overlaps with our pcache
        if (pcache
                && rcache->block == pcache->block
                && rcache->off < pcache->off + pcache->size
                && rcache->off + rcache->size > pcache->off) {
            lfs_off_t off_ = lfs_max(rcache->off, pcache->off);
            lfs_size_t size_ = lfs_min(
                    rcache->size - (off_-rcache->off),
                    pcache->size - (off_-pcache->off));
            memcpy(&rcache->buffer[off_-rcache->off],
                    &pcache->buffer[off_-pcache->off],
                    size_);
        }
    }

    return 0;
}

static int lfs_bd_cmp(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    lfs_size_t diff = 0;
    // make sure our hint is at least as big as our buffer
    hint = lfs_max(hint, size);

    for (lfs_off_t i = 0; i < size; i += diff) {
        uint8_t dat[8];

        diff = lfs_min(size-i, sizeof(dat));
        int err = lfs_bd_read(lfs,
                pcache, rcache, hint-i,
                block, off+i, &dat, diff);
        if (err) {
            return err;
        }

        int res = memcmp(dat, data + i, diff);
        if (res) {
            return res < 0 ? LFS_CMP_LT : LFS_CMP_GT;
        }
    }

    return LFS_CMP_EQ;
}

//static int lfs_bd_crc(lfs_t *lfs,
//        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
//        lfs_block_t block, lfs_off_t off, lfs_size_t size, uint32_t *crc) {
//    lfs_size_t diff = 0;
//
//    for (lfs_off_t i = 0; i < size; i += diff) {
//        uint8_t dat[8];
//        diff = lfs_min(size-i, sizeof(dat));
//        int err = lfs_bd_read(lfs,
//                pcache, rcache, hint-i,
//                block, off+i, &dat, diff);
//        if (err) {
//            return err;
//        }
//
//        *crc = lfs_crc(*crc, &dat, diff);
//    }
//
//    return 0;
//}

static int lfs_bd_crc32c(lfs_t *lfs,
        const lfs_cache_t *pcache, lfs_cache_t *rcache, lfs_size_t hint,
        lfs_block_t block, lfs_off_t off, lfs_size_t size, uint32_t *crc) {
    lfs_size_t diff = 0;

    for (lfs_off_t i = 0; i < size; i += diff) {
        uint8_t dat[8];
        diff = lfs_min(size-i, sizeof(dat));
        int err = lfs_bd_read(lfs,
                pcache, rcache, lfs_max32(hint, size)-i,
                block, off+i, &dat, diff);
        if (err) {
            return err;
        }

        *crc = lfs_crc32c(*crc, &dat, diff);
    }

    return 0;
}

#ifndef LFS_READONLY
static int lfs_bd_flush(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate) {
    if (pcache->block != LFS_BLOCK_NULL && pcache->block != LFS_BLOCK_INLINE) {
        LFS_ASSERT(pcache->block < lfs->cfg->block_count);
        lfs_size_t diff = lfs_alignup(pcache->size, lfs->cfg->prog_size);
        int err = lfs->cfg->prog(lfs->cfg, pcache->block,
                pcache->off, pcache->buffer, diff);
        LFS_ASSERT(err <= 0);
        if (err) {
            return err;
        }

        if (validate) {
            // check data on disk
            lfs_cache_drop(lfs, rcache);
            lfs_scmp_t cmp = lfs_bd_cmp(lfs,
                    NULL, rcache, diff,
                    pcache->block, pcache->off, pcache->buffer, diff);
            if (cmp < 0) {
                return cmp;
            }

            if (lfs_cmp(cmp) != 0) {
                return LFS_ERR_CORRUPT;
            }
        }

        lfs_cache_zero(lfs, pcache);
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
static int lfs_bd_sync(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate) {
    lfs_cache_drop(lfs, rcache);

    int err = lfs_bd_flush(lfs, pcache, rcache, validate);
    if (err) {
        return err;
    }

    err = lfs->cfg->sync(lfs->cfg);
    LFS_ASSERT(err <= 0);
    return err;
}
#endif

#ifndef LFS_READONLY
static int lfs_bd_prog(lfs_t *lfs,
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate,
        lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    LFS_ASSERT(block == LFS_BLOCK_INLINE || block < lfs->cfg->block_count);
    LFS_ASSERT(off + size <= lfs->cfg->block_size);

    // update rcache if we overlap
    if (rcache
            && block == rcache->block
            && off < rcache->off + rcache->size
            && off + size > rcache->off) {
        lfs_off_t off_ = lfs_max(off, rcache->off);
        lfs_size_t size_ = lfs_min(
                size - (off_-off),
                rcache->size - (off_-rcache->off));
        memcpy(&rcache->buffer[off_-rcache->off], &data[off_-off], size_);
    }

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

            pcache->size = lfs_max(pcache->size, off - pcache->off);
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
        LFS_ASSERT(pcache->block == LFS_BLOCK_NULL);

        // prepare pcache, first condition can no longer fail
        pcache->block = block;
        pcache->off = lfs_aligndown(off, lfs->cfg->prog_size);
        pcache->size = 0;
    }

    return 0;
}
#endif

#ifndef LFS_READONLY
static int lfs_bd_erase(lfs_t *lfs, lfs_block_t block) {
    LFS_ASSERT(block < lfs->cfg->block_count);

    // make sure any caches are outdated appropriately here
    LFS_ASSERT(lfs->pcache.block != block);
    if (lfs->rcache.block == block) {
        lfs_cache_drop(lfs, &lfs->rcache);
    }

    int err = lfs->cfg->erase(lfs->cfg, block);
    LFS_ASSERT(err <= 0);
    return err;
}
#endif

// TODO should these be the only bd APIs?
// simpler APIs if assume file caches are irrelevant
//
// note hint has two convenience:
// 1. 0 = minimal caching
// 2. block_size = maximal caching
//
static int lfsr_bd_read(lfs_t *lfs,
        lfs_block_t block, lfs_off_t off, lfs_size_t hint,
        void *buffer, lfs_size_t size) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    return lfs_bd_read(lfs, &lfs->pcache, &lfs->rcache, hint,
            block, off, buffer, size);
}

// TODO merge lfsr_bd_readcsum/lfsr_bd_csum somehow?
static int lfsr_bd_readcsum(lfs_t *lfs,
        lfs_block_t block, lfs_off_t off, lfs_size_t hint,
        void *buffer, lfs_size_t size,
        uint32_t *csum_) {
    int err = lfsr_bd_read(lfs, block, off, hint, buffer, size);
    if (err) {
        return err;
    }

    *csum_ = lfs_crc32c(*csum_, buffer, size);
    return 0;
}

static int lfsr_bd_csum(lfs_t *lfs,
        lfs_block_t block, lfs_off_t off, lfs_size_t hint, lfs_size_t size,
        uint32_t *crc_) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    return lfs_bd_crc32c(lfs, &lfs->pcache, &lfs->rcache, hint,
            block, off, size, crc_);
}

static lfs_scmp_t lfsr_bd_cmp(lfs_t *lfs,
        lfs_block_t block, lfs_off_t off, lfs_size_t hint, 
        const void *buffer, lfs_size_t size) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    return lfs_bd_cmp(lfs, &lfs->pcache, &lfs->rcache, hint,
            block, off, buffer, size);
}

// program data with optional checksum
static int lfsr_bd_prog(lfs_t *lfs, lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *csum_) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        lfs_cache_zero(lfs, &lfs->pcache);
        return LFS_ERR_RANGE;
    }

    int err = lfs_bd_prog(lfs, &lfs->pcache, &lfs->rcache, false,
            block, off, buffer, size);
    if (err) {
        return err;
    }

    // optional checksum
    if (csum_) {
        *csum_ = lfs_crc32c(*csum_, buffer, size);
    }

    return 0;
}

static int lfsr_bd_sync(lfs_t *lfs) {
    return lfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
}

// TODO do we need this? should everything be checked by crc and validation
// be an optional ifdef?
static int lfsr_bd_progvalidate(lfs_t *lfs, lfs_block_t block, lfs_off_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *csum_) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        lfs_cache_zero(lfs, &lfs->pcache);
        return LFS_ERR_RANGE;
    }

    int err = lfs_bd_prog(lfs, &lfs->pcache, &lfs->rcache, true,
            block, off, buffer, size);
    if (err) {
        return err;
    }

    if (csum_) {
        *csum_ = lfs_crc32c(*csum_, buffer, size);
    }

    return 0;
}

static int lfsr_bd_syncvalidate(lfs_t *lfs) {
    return lfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, true);
}

static int lfsr_bd_erase(lfs_t *lfs, lfs_block_t block) {
    return lfs_bd_erase(lfs, block);
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
enum lfsr_tag_type {
    LFSR_TAG_NULL           = 0x0000,
    LFSR_TAG_UNR            = 0x1000, // in-device only
    LFSR_TAG_GROW           = 0x3000, // in-device only

    LFSR_TAG_SUPERMAGIC     = 0x0003,
    LFSR_TAG_SUPERCONFIG    = 0x0004,

    LFSR_TAG_NAME           = 0x0200,
    LFSR_TAG_WIDENAME       = 0x4200, // in-device only
    LFSR_TAG_BRANCH         = 0x0200,
    LFSR_TAG_DSTART         = 0x0201,
    LFSR_TAG_REG            = 0x0202,
    LFSR_TAG_GROWREG        = 0x2202, // test only? TODO
    LFSR_TAG_DIR            = 0x0203,

    LFSR_TAG_STRUCT         = 0x0300,
    LFSR_TAG_WIDESTRUCT     = 0x4300, // in-device only
    LFSR_TAG_RMWIDESTRUCT   = 0x5300, // in-device only
    LFSR_TAG_INLINED        = 0x0300,
    LFSR_TAG_BLOCK          = 0x0302,
    LFSR_TAG_BTREE          = 0x0303,
    LFSR_TAG_RMBTREE        = 0x1303, // in-device only
    LFSR_TAG_MROOT          = 0x0304,
    LFSR_TAG_MDIR           = 0x0305,
    LFSR_TAG_RMMDIR         = 0x1305, // in-device only
    LFSR_TAG_MTREE          = 0x0306,
    LFSR_TAG_RMMTREE        = 0x1306, // in-device only
    LFSR_TAG_DID            = 0x0307,

    LFSR_TAG_UATTR          = 0x0400,
    LFSR_TAG_WIDEUATTR      = 0x4400, // test only? TODO
    LFSR_TAG_GROWUATTR      = 0x2400, // test only? TODO
    LFSR_TAG_RMUATTR        = 0x1400, // in-device only
    LFSR_TAG_RMWIDEUATTR    = 0x5400, // test only? TODO
    LFSR_TAG_SATTR          = 0x0500, // test only? TODO
    LFSR_TAG_RMWIDESATTR    = 0x5500, // in-device only

    LFSR_TAG_ALT            = 0x4000,
    LFSR_TAG_ALTLE          = 0x4000,
    LFSR_TAG_ALTBLE         = 0x4000,
    LFSR_TAG_ALTRLE         = 0x5000,
    LFSR_TAG_ALTGT          = 0x6000,
    LFSR_TAG_ALTBGT         = 0x6000,
    LFSR_TAG_ALTRGT         = 0x7000,

    LFSR_TAG_CRC            = 0x2000,
    LFSR_TAG_FCRC           = 0x2100,
};

#define LFSR_TAG_ALTLE(red, key) \
    (LFSR_TAG_ALTLE \
        | ((0x1 & (lfsr_tag_t)(red)) << 12) \
        | (0x0fff & (lfsr_tag_t)(key)))

#define LFSR_TAG_ALTGT(red, key) \
    (LFSR_TAG_ALTGT \
        | ((0x1 & (lfsr_tag_t)(red)) << 12) \
        | (0x0fff & (lfsr_tag_t)(key)))

#define LFSR_TAG_UATTR(attr) \
    (LFSR_TAG_UATTR \
        | (0x7f & (lfsr_tag_t)(attr)))

// TODO test only?
#define LFSR_TAG_WIDEUATTR(attr) \
    (LFSR_TAG_WIDEUATTR \
        | (0x7f & (lfsr_tag_t)(attr)))

// TODO test only?
#define LFSR_TAG_GROWUATTR(attr) \
    (LFSR_TAG_GROWUATTR \
        | (0x7f & (lfsr_tag_t)(attr)))

#define LFSR_TAG_RMUATTR(attr) \
    (LFSR_TAG_RMUATTR \
        | (0x7f & (lfsr_tag_t)(attr)))

// TODO test only?
#define LFSR_TAG_SATTR(attr) \
    (LFSR_TAG_SATTR \
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

static inline bool lfsr_tag_isvalid(lfsr_tag_t tag) {
    return !(tag & 0x8000);
}

static inline lfsr_tag_t lfsr_tag_setvalid(lfsr_tag_t tag) {
    return tag & ~0x8000;
}

static inline lfsr_tag_t lfsr_tag_setinvalid(lfsr_tag_t tag) {
    return tag | 0x8000;
}

static inline bool lfsr_tag_iswide(lfsr_tag_t tag) {
    return tag & 0x4000;
}

static inline lfsr_tag_t lfsr_tag_setwide(lfsr_tag_t tag) {
    return tag | 0x4000;
}

static inline lfsr_tag_t lfsr_tag_clearwide(lfsr_tag_t tag) {
    return tag & ~0x4000;
}

static inline bool lfsr_tag_isgrow(lfsr_tag_t tag) {
    return tag & 0x2000;
}

static inline lfsr_tag_t lfsr_tag_setgrow(lfsr_tag_t tag) {
    return tag | 0x2000;
}

static inline lfsr_tag_t lfsr_tag_cleargrow(lfsr_tag_t tag) {
    return tag & ~0x2000;
}

static inline bool lfsr_tag_isrm(lfsr_tag_t tag) {
    return tag & 0x1000;
}

static inline lfsr_tag_t lfsr_tag_setrm(lfsr_tag_t tag) {
    return tag | 0x1000;
}

static inline bool lfsr_tag_isalt(lfsr_tag_t tag) {
    return tag & 0x4000;
}

static inline bool lfsr_tag_istrunk(lfsr_tag_t tag) {
    return (tag & 0x6000) != 0x2000;
}

static inline lfsr_tag_t lfsr_tag_next(lfsr_tag_t tag) {
    return tag + 0x1;
}

// lfsr_rbyd_append diverged specific flags
static inline bool lfsr_tag_hasdiverged(lfsr_tag_t tag) {
    return tag & 0x2000;
}

static inline bool lfsr_tag_isdivergedupper(lfsr_tag_t tag) {
    return tag & 0x1000;
}

static inline bool lfsr_tag_isdivergedlower(lfsr_tag_t tag) {
    return !lfsr_tag_isdivergedupper(tag);
}

static inline lfsr_tag_t lfsr_tag_setdivergedlower(lfsr_tag_t tag) {
    return tag | 0x2000;
}

static inline lfsr_tag_t lfsr_tag_setdivergedupper(lfsr_tag_t tag) {
    return tag | 0x3000;
}

// alt operations
static inline bool lfsr_tag_isblack(lfsr_tag_t tag) {
    return !(tag & 0x1000);
}

static inline bool lfsr_tag_isred(lfsr_tag_t tag) {
    return tag & 0x1000;
}

static inline lfsr_tag_t lfsr_tag_setblack(lfsr_tag_t tag) {
    return tag & ~0x1000;
}

static inline lfsr_tag_t lfsr_tag_setred(lfsr_tag_t tag) {
    return tag | 0x1000;
}

static inline bool lfsr_tag_isle(lfsr_tag_t tag) {
    return !(tag & 0x2000);
}

static inline bool lfsr_tag_isgt(lfsr_tag_t tag) {
    return tag & 0x2000;
}

static inline lfsr_tag_t lfsr_tag_isparallel(lfsr_tag_t a, lfsr_tag_t b) {
    return (a & 0x2000) == (b & 0x2000);
}

static inline lfsr_tag_t lfsr_tag_key(lfsr_tag_t tag) {
    return tag & 0x0fff;
}

static inline bool lfsr_tag_follow(lfsr_tag_t alt, lfs_size_t weight,
        lfs_ssize_t lower, lfs_ssize_t upper,
        lfs_ssize_t id, lfsr_tag_t tag) {
    if (lfsr_tag_isgt(alt)) {
        return id > upper - (lfs_ssize_t)weight - 1
                || (id == upper - (lfs_ssize_t)weight - 1
                    && lfsr_tag_key(tag) > lfsr_tag_key(alt));
    } else {
        return id < lower + (lfs_ssize_t)weight
                || (id == lower + (lfs_ssize_t)weight
                    && lfsr_tag_key(tag) <= lfsr_tag_key(alt));
    }
}

static inline bool lfsr_tag_follow2(
        lfsr_tag_t alt, lfs_size_t weight,
        lfsr_tag_t alt2, lfs_size_t weight2,
        lfs_ssize_t lower, lfs_ssize_t upper,
        lfs_ssize_t id, lfsr_tag_t tag) {
    if (lfsr_tag_isred(alt2) && lfsr_tag_isparallel(alt, alt2)) {
        weight += weight2;
    }

    return lfsr_tag_follow(alt, weight, lower, upper, id, tag);
}

static inline bool lfsr_tag_prune2(
        lfsr_tag_t alt, lfs_ssize_t weight,
        lfsr_tag_t alt2, lfs_ssize_t weight2,
        lfs_ssize_t lower_id, lfs_ssize_t upper_id,
        lfsr_tag_t lower_tag, lfsr_tag_t upper_tag) {
    if (lfsr_tag_isgt(alt)) {
        return lfsr_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_id, upper_id,
                lower_id, lower_tag);
    } else {
        return lfsr_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_id, upper_id,
                upper_id-1, upper_tag-0x1);
    }
}

static inline void lfsr_tag_flip(lfsr_tag_t *alt, lfs_size_t *weight,
        lfs_ssize_t lower, lfs_ssize_t upper) {
    *alt = *alt ^ 0x2000;
    *weight = (upper-lower) - *weight - 1;
}

static inline void lfsr_tag_flip2(lfsr_tag_t *alt, lfs_size_t *weight,
        lfsr_tag_t alt2, lfs_size_t weight2,
        lfs_ssize_t lower, lfs_ssize_t upper) {
    if (lfsr_tag_isred(alt2)) {
        *weight += weight2;
    }

    lfsr_tag_flip(alt, weight, lower, upper);
}

static inline void lfsr_tag_trim(
        lfsr_tag_t alt, lfs_size_t weight,
        lfs_ssize_t *lower_id, lfs_ssize_t *upper_id,
        lfsr_tag_t *lower_tag, lfsr_tag_t *upper_tag) {
    if (lfsr_tag_isgt(alt)) {
        *upper_id -= weight;
        if (upper_tag) {
            *upper_tag = alt + 0x1;
        }
    } else {
        *lower_id += weight;
        if (lower_tag) {
            *lower_tag = alt + 0x1;
        }
    }
}

static inline void lfsr_tag_trim2(
        lfsr_tag_t alt, lfs_size_t weight,
        lfsr_tag_t alt2, lfs_size_t weight2,
        lfs_ssize_t *lower_id, lfs_ssize_t *upper_id,
        lfsr_tag_t *lower_tag, lfsr_tag_t *upper_tag) {
    if (lfsr_tag_isred(alt2)) {
        lfsr_tag_trim(alt2, weight2, lower_id, upper_id, lower_tag, upper_tag);
    }

    lfsr_tag_trim(alt, weight, lower_id, upper_id, lower_tag, upper_tag);
}

// support for encoding/decoding tags on disk

// each piece of metadata in an rbyd tree is prefixed with a 4-piece tag:
//
// - 8-bit suptype    => 1 byte
// - 8-bit subtype    => 1 byte
// - 32-bit id/weight => 5 byte leb128 (worst case)
// - 32-bit size/jump => 5 byte leb128 (worst case)
//                    => 12 bytes total
//
#define LFSR_TAG_DSIZE (2+5+5)

static lfs_ssize_t lfsr_bd_readtag(lfs_t *lfs,
        lfs_block_t block, lfs_off_t off, lfs_size_t hint,
        lfsr_tag_t *tag_, lfs_size_t *weight_, lfs_size_t *size_,
        uint32_t *csum_) {
    // read the largest possible tag size
    lfs_size_t tsize = lfs_min32(LFSR_TAG_DSIZE, lfs->cfg->block_size-off);
    uint8_t buf[LFSR_TAG_DSIZE];
    int err = lfsr_bd_read(lfs, block, off, hint, &buf, tsize);
    if (err) {
        return err;
    }

    if (tsize < 2) {
        return LFS_ERR_CORRUPT;
    }
    uint16_t tag = ((lfsr_tag_t)buf[0] << 8) | ((lfsr_tag_t)buf[1] << 0);
    ssize_t d = 2;

    if (csum_) {
        // on-disk, the tags valid bit must reflect the parity of the
        // preceding data, fortunately for crc32c, this is the same as the
        // parity of the crc
        //
        // note we need to do this before leb128 decoding as we may not have
        // valid leb128 if we're erased, but we shouldn't treat a truncated
        // leb128 here as corruption
        if ((tag >> 15) != (lfs_popc(*csum_) & 1)) {
            return LFS_ERR_INVAL;
        }
    }

    lfs_size_t weight;
    lfs_ssize_t d_ = lfs_fromleb128(&weight, &buf[d], tsize-d);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    if (weight > 0x7fffffff) {
        return LFS_ERR_CORRUPT;
    }

    lfs_size_t size;
    d_ = lfs_fromleb128(&size, &buf[d], tsize-d);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    if (size > 0x7fffffff) {
        return LFS_ERR_CORRUPT;
    }

    // optionally crc
    if (csum_) {
        *csum_ = lfs_crc32c(*csum_, buf, d);
    }

    // save what we found, note we make a few tweaks on-disk => in-device
    // - clear the valid bit from tag, we checked this earlier
    // - adjust id so reserved id is -1, so we don't have mixed zero/one indexed
    //
    *tag_ = tag & 0x7fff;
    *weight_ = weight;
    *size_ = size;
    return d;
}

static lfs_ssize_t lfsr_bd_progtag(lfs_t *lfs,
        lfs_block_t block, lfs_off_t off,
        lfsr_tag_t tag, lfs_size_t weight, lfs_size_t size,
        uint32_t *csum_) {
    // check for underflow issues
    LFS_ASSERT(weight < 0x80000000);
    LFS_ASSERT(size < 0x80000000);

    // make sure to include the parity of the current crc
    tag |= (lfs_popc(*csum_) & 1) << 15;

    // encode into a be16 and pair of leb128s
    uint8_t buf[LFSR_TAG_DSIZE];
    buf[0] = (uint8_t)(tag >> 8);
    buf[1] = (uint8_t)(tag >> 0);

    lfs_size_t d = 2;
    ssize_t d_ = lfs_toleb128(weight, &buf[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfs_toleb128(size, &buf[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    int err = lfsr_bd_prog(lfs, block, off, &buf, d, csum_);
    if (err) {
        return err;
    }

    return d;
}



/// lfsr_data_t stuff ///

// either an on-disk or in-device data pointer
typedef union lfsr_data {
    // sign(size)=0 => in-device
    // sign(size)=1 => on-disk
    lfs_size_t size;
    struct {
        lfs_size_t size;
        const uint8_t *buffer;
        lfs_ssize_t did;
    } buf;
    struct {
        lfs_size_t size;
        lfs_block_t block;
        lfs_off_t off;
    } disk;
} lfsr_data_t;

#define LFSR_DATA_NULL LFSR_DATA_BUF(NULL, 0)

#define LFSR_DATA_BUF(_buffer, _size) \
    ((lfsr_data_t){.buf={ \
        .size=_size, \
        .buffer=(const void*)(_buffer), \
        .did=-1}})

#define LFSR_DATA_DNAME(_did, _buffer, _size) \
    ((lfsr_data_t){.buf={ \
        /* note this find the effective leb128 size */ \
        .size=_size + (lfs_nlog2((_did)+1)+7-1)/7, \
        .buffer=(const void*)(_buffer), \
        .did=_did}})

#define LFSR_DATA_LEB128(_did) \
    ((lfsr_data_t){.buf={ \
        /* note this find the effective leb128 size */ \
        .size=(lfs_nlog2((_did)+1)+7-1)/7, \
        .buffer=NULL, \
        .did=_did}})

#define LFSR_DATA_DISK(_block, _off, _size) \
    ((lfsr_data_t){.disk={ \
        .size=(0x80000000 | (_size)), \
        .block=_block, \
        .off=_off}})

static inline bool lfsr_data_ondisk(lfsr_data_t data) {
    return data.size & 0x80000000;
}

static inline lfs_size_t lfsr_data_size(lfsr_data_t data) {
    return data.size & 0x7fffffff;
}

static inline lfs_size_t lfsr_data_setondisk(lfs_size_t size) {
    return size | 0x80000000;
}

// data<->bd interactions
static lfs_ssize_t lfsr_data_read(lfs_t *lfs, lfsr_data_t data,
        lfs_off_t off, void *buffer, lfs_size_t size) {
    // limit our off/size to data range
    lfs_off_t off_ = lfs_min32(off, lfsr_data_size(data));
    lfs_size_t hint_ = lfsr_data_size(data)-off_;
    lfs_size_t size_ = lfs_min32(size, hint_);

    if (lfsr_data_ondisk(data)) {
        int err = lfsr_bd_read(lfs, data.disk.block, data.disk.off+off_,
                // note our hint includes the full data range
                hint_,
                buffer, size_);
        if (err) {
            return err;
        }
    } else {
        memcpy(buffer, data.buf.buffer+off_, size_);
    }

    return size_;
}

static lfs_ssize_t lfsr_data_readle32(lfs_t *lfs, lfsr_data_t data,
        lfs_off_t off, uint32_t *word) {
    lfs_ssize_t d = lfsr_data_read(lfs, data, off, word, sizeof(uint32_t));
    if (d < 0) {
        return d;
    }

    // truncated?
    if ((lfs_size_t)d < sizeof(uint32_t)) {
        return LFS_ERR_CORRUPT;
    }

    *word = lfs_fromle32_(word);
    return sizeof(uint32_t);
}

static lfs_ssize_t lfsr_data_readleb128(lfs_t *lfs, lfsr_data_t data,
        lfs_off_t off, uint32_t *word) {
    // for 32-bits we can assume worst-case leb128 size is 5-bytes
    uint8_t buf[5];
    lfs_ssize_t d = lfsr_data_read(lfs, data, off, buf, 5);
    if (d < 0) {
        return d;
    }

    return lfs_fromleb128(word, buf, d);
}

static lfs_scmp_t lfsr_data_cmp(lfs_t *lfs, lfsr_data_t data,
        lfs_off_t off, const void *buffer, lfs_size_t size) {
    // limit our off/size to data range
    lfs_off_t off_ = lfs_min32(off, lfsr_data_size(data));
    lfs_size_t hint_ = lfsr_data_size(data)-off_;

    // compare our data
    if (lfsr_data_ondisk(data)) {
        int cmp = lfsr_bd_cmp(lfs, data.disk.block, data.disk.off+off_, 0,
                buffer, lfs_min32(hint_, size));
        if (cmp != LFS_CMP_EQ) {
            return cmp;
        }
    } else {
        int cmp = memcmp(data.buf.buffer+off_, buffer, size);
        if (cmp < 0) {
            return LFS_CMP_LT;
        } else if (cmp > 0) {
            return LFS_CMP_GT;
        }
    }

    // if data is equal, check for size mismatch
    if (hint_ < size) {
        return LFS_CMP_LT;
    } else if (hint_ > size) {
        return LFS_CMP_GT;
    } else {
        return LFS_CMP_EQ;
    }
}

static lfs_scmp_t lfsr_data_dnamecmp(lfs_t *lfs, lfsr_data_t data,
        lfs_off_t off, lfs_size_t did, const char *name, lfs_size_t name_size) {
    // first compare the did
    lfs_size_t did_;
    lfs_ssize_t d = lfsr_data_readleb128(lfs, data, off, &did_);
    if (d < 0) {
        return d;
    }

    if (did_ < did) {
        return LFS_CMP_LT;
    } else if (did_ > did) {
        return LFS_CMP_GT;
    }

    // next compare the actual name
    return lfsr_data_cmp(lfs, data, off+d, name, name_size);
}

static int lfsr_bd_progdata(lfs_t *lfs,
        lfs_block_t block, lfs_off_t off,
        lfsr_data_t data,
        uint32_t *csum_) {
    if (lfsr_data_ondisk(data)) {
        // TODO byte-level copies have been a pain point, works for prototyping
        // but can this be better? configurable? leverage
        // rcache/pcache directly?
        uint8_t dat;
        for (lfs_size_t i = 0; i < lfsr_data_size(data); i++) {
            int err = lfsr_bd_read(lfs, data.disk.block, data.disk.off+i,
                    lfsr_data_size(data)-i,
                    &dat, 1);
            if (err) {
                return err;
            }

            err = lfsr_bd_prog(lfs, block, off+i,
                    &dat, 1,
                    csum_);
            if (err) {
                return err;
            }
        }

    } else {
        // this is kind of a hack, but when lfsr_data_t is in buffer mode, it
        // can also contain a leb128 encoded directory-id prefix
        if (data.buf.did != -1) {
            // TODO should progleb128 be its own function? rely on caching?
            uint8_t buf[5];
            lfs_ssize_t d = lfs_toleb128(data.buf.did, buf, 5);
            if (d < 0) {
                return d;
            }

            int err = lfsr_bd_prog(lfs, block, off, buf, d, csum_);
            if (err) {
                return err;
            }

            off += d;
            data.buf.size -= d;
        }

        int err = lfsr_bd_prog(lfs, block, off,
                data.buf.buffer, lfsr_data_size(data),
                csum_);
        if (err) {
            return err;
        }
    }

    return 0;
}


// operations on attribute lists

//struct lfs_mattr {
//    lfs_tag_t tag;
//    const void *buffer;
//};
//
//struct lfs_diskoff {
//    lfs_block_t block;
//    lfs_off_t off;
//};
//
//#define LFS_MKATTRS(...) 
//    (struct lfs_mattr[]){__VA_ARGS__}, 
//    sizeof((struct lfs_mattr[]){__VA_ARGS__}) / sizeof(struct lfs_mattr)

typedef struct lfsr_attr {
    lfs_ssize_t id;
    lfsr_tag_t tag;
    lfs_ssize_t delta;
    lfsr_data_t data;
} lfsr_attr_t;

#define LFSR_ATTR_DATA_(_id, _tag, _delta, _data) \
    ((const lfsr_attr_t){_id, _tag, _delta, _data})

#define LFSR_ATTR_DATA(_id, _type, _delta, _data) \
    LFSR_ATTR_DATA_(_id, LFSR_TAG_##_type, _delta, _data)

#define LFSR_ATTR_DNAME_(_id, _tag, _delta, _did, _buffer, _size) \
    LFSR_ATTR_DATA_(_id, _tag, _delta, LFSR_DATA_DNAME(_did, _buffer, _size))

#define LFSR_ATTR_DNAME(_id, _type, _delta, _did, _buffer, _size) \
    LFSR_ATTR_DNAME_(_id, LFSR_TAG_##_type, _delta, _did, _buffer, _size)

#define LFSR_ATTR_LEB128_(_id, _tag, _delta, _did) \
    LFSR_ATTR_DATA_(_id, _tag, _delta, LFSR_DATA_LEB128(_did))

#define LFSR_ATTR_LEB128(_id, _type, _delta, _did) \
    LFSR_ATTR_LEB128_(_id, LFSR_TAG_##_type, _delta, _did)

#define LFSR_ATTR_(_id, _tag, _delta, _buffer, _size) \
    LFSR_ATTR_DATA_(_id, _tag, _delta, LFSR_DATA_BUF(_buffer, _size))

#define LFSR_ATTR(_id, _type, _delta, _buffer, _size) \
    LFSR_ATTR_(_id, LFSR_TAG_##_type, _delta, _buffer, _size)

#define LFSR_ATTR_DISK_(_id, _tag, _delta, _block, _off, _size) \
    LFSR_ATTR_DATA_(_id, _tag, _delta, LFSR_DATA_DISK(_block, _off, _size))

#define LFSR_ATTR_DISK(_id, _type, _delta, _block, _off, _size) \
    LFSR_ATTR_DISK_(_id, LFSR_TAG_##_type, _delta, _block, _off, _size)

#define LFSR_ATTR_NOOP LFSR_ATTR(-1, UNR, 0, NULL, 0)

#define LFSR_ATTRS(...) \
    (const lfsr_attr_t[]){__VA_ARGS__}, \
    sizeof((const lfsr_attr_t[]){__VA_ARGS__}) / sizeof(lfsr_attr_t)

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

// fcrc on-disk encoding
typedef struct lfsr_fcrc {
    uint32_t crc;
    lfs_size_t size;
} lfsr_fcrc_t;

// 1 leb128 + 1 crc32c => 9 bytes (worst case)
#define LFSR_FCRC_DSIZE (5+4)

static lfs_ssize_t lfsr_fcrc_todisk(lfs_t *lfs, const lfsr_fcrc_t *fcrc,
        uint8_t buffer[static LFSR_FCRC_DSIZE]) {
    (void)lfs;
    lfs_ssize_t d = 0;

    lfs_tole32_(fcrc->crc, &buffer[d]);
    d += 4;

    lfs_ssize_t d_ = lfs_toleb128(fcrc->size, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    return d;
}

static lfs_ssize_t lfsr_fcrc_fromdisk(lfs_t *lfs, lfsr_fcrc_t *fcrc,
        lfsr_data_t data) {
    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfsr_data_readle32(lfs, data, d, &fcrc->crc);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfsr_data_readleb128(lfs, data, d, &fcrc->size);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    return d;
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
static int lfs_alloc(lfs_t *lfs, lfs_block_t *block);
static void lfs_alloc_ack(lfs_t *lfs);


/// Red-black-yellow Dhara tree operations ///

// helper functions
static bool lfsr_rbyd_isfetched(const lfsr_rbyd_t *rbyd) {
    return !(rbyd->off == 0 && rbyd->trunk > 0);
}


// allocate an rbyd block
static int lfsr_rbyd_alloc(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    *rbyd = (lfsr_rbyd_t){.weight=0, .trunk=0, .off=0, .crc=0};
    int err = lfs_alloc(lfs, &rbyd->block);
    if (err) {
        return err;
    }
            
    // TODO should erase be implicit in alloc eventually?
    err = lfsr_bd_erase(lfs, rbyd->block);
    if (err) {
        return err;
    }
    return 0;
}

static int lfsr_rbyd_fetch(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_block_t block, lfs_size_t trunk) {
    // checksum the revision count to get the crc started
    uint32_t crc = 0;
    int err = lfsr_bd_csum(lfs, block, 0, lfs->cfg->block_size,
            sizeof(uint32_t), &crc);
    if (err) {
        return err;
    }

    rbyd->block = block;
    rbyd->off = 0;
    rbyd->trunk = 0;

    // temporary state until we validate a crc
    lfs_off_t off = sizeof(uint32_t);
    lfs_off_t trunk_ = 0;
    bool wastrunk = false;
    lfs_size_t weight = 0;
    lfs_size_t weight_ = 0;

    // assume unerased until proven otherwise
    lfsr_fcrc_t fcrc;
    bool hasfcrc = false;
    bool maybeerased = false;

    // scan tags, checking valid bits, crcs, etc
    while (off < lfs->cfg->block_size && (!trunk || rbyd->off <= trunk)) {
        lfsr_tag_t tag;
        lfs_size_t w;
        lfs_size_t size;
        lfs_ssize_t d = lfsr_bd_readtag(lfs,
                block, off, lfs->cfg->block_size,
                &tag, &w, &size, &crc);
        if (d < 0) {
            if (d == LFS_ERR_INVAL || d == LFS_ERR_CORRUPT) {
                maybeerased = maybeerased && d == LFS_ERR_INVAL;
                break;
            }
            return d;
        }
        off += d;

        // tag goes out of range?
        if (!lfsr_tag_isalt(tag) && off + size > lfs->cfg->block_size) {
            break;
        }

        // not an end-of-commit crc
        if (!lfsr_tag_isalt(tag) && lfsr_tag_suptype(tag) != LFSR_TAG_CRC) {
            // crc the entry, hopefully leaving it in the cache
            err = lfsr_bd_csum(lfs, block, off, lfs->cfg->block_size, size,
                    &crc);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    break;
                }
                return err;
            }

            // found an fcrc? save for later
            if (tag == LFSR_TAG_FCRC) {
                uint8_t buf[LFSR_FCRC_DSIZE];
                err = lfsr_bd_read(lfs, block, off, lfs->cfg->block_size,
                        buf, lfs_min32(size, LFSR_FCRC_DSIZE));
                if (err) {
                    if (err == LFS_ERR_CORRUPT) {
                        break;
                    }
                    return err;
                }

                lfs_ssize_t d = lfsr_fcrc_fromdisk(lfs, &fcrc,
                        LFSR_DATA_BUF(buf, lfs_min32(size, LFSR_FCRC_DSIZE)));
                if (d < 0 && d != LFS_ERR_CORRUPT) {
                    return d;
                }

                // ignore malformed fcrcs
                hasfcrc = (d != LFS_ERR_CORRUPT);
            }

        // is an end-of-commit crc
        } else if (!lfsr_tag_isalt(tag)) {
            uint32_t crc_ = 0;
            err = lfsr_bd_read(lfs, block, off, lfs->cfg->block_size,
                    &crc_, sizeof(uint32_t));
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    break;
                }
                return err;
            }
            crc_ = lfs_fromle32_(&crc_);

            if (crc != crc_) {
                // uh oh, crcs don't match
                break;
            }

            // toss our crc into the filesystem seed for
            // pseudorandom numbers, note we use another crc here
            // as a collection function because it is sufficiently
            // random and convenient
            lfs->seed = lfs_crc32c(lfs->seed, &crc, sizeof(uint32_t));

            // fcrc appears valid so far
            maybeerased = hasfcrc;
            hasfcrc = false;

            // save what we've found so far
            rbyd->off = off + size;
            rbyd->crc = crc;
            rbyd->trunk = trunk_;
            rbyd->weight = weight;
        }

        // found a trunk of a tree?
        if (lfsr_tag_istrunk(tag)
                && (!trunk || trunk >= off-d || wastrunk)) {
            // start of trunk?
            if (!wastrunk) {
                wastrunk = true;
                // save trunk entry point
                trunk_ = off-d;
                // reset weight
                weight_ = 0;
            }

            // derive weight of the tree from alt pointers
            //
            // NOTE we can't check for overflow/underflow here because we
            // may be overeagerly parsing an invalid commit, it's ok for
            // this to overflow/underflow as long as we throw it out later
            // on a bad crc
            weight_ += w;

            // end of trunk?
            if (!lfsr_tag_isalt(tag)) {
                wastrunk = false;
                // update current weight
                weight = weight_;
            }
        }

        if (!lfsr_tag_isalt(tag)) {
            off += size;
        }
    }

    // no valid commits?
    if (!rbyd->trunk) {
        return LFS_ERR_CORRUPT;
    }

    // did we end on a valid commit? we may have an erased block
    bool erased = false;
    if (maybeerased && rbyd->off % lfs->cfg->prog_size == 0) {
        // check for an fcrc matching the next prog's erased state, if
        // this failed most likely a previous prog was interrupted, we
        // need a new erase
        uint32_t fcrc_ = 0;
        int err = lfsr_bd_csum(lfs, rbyd->block, rbyd->off, 0, fcrc.size,
                &fcrc_);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        // found beginning of erased part?
        erased = (fcrc_ == fcrc.crc);
    }

    if (!erased) {
        rbyd->off = lfs->cfg->block_size;
    }

    return 0;
}

static int lfsr_rbyd_lookupnext(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfs_ssize_t id, lfsr_tag_t tag,
        lfs_ssize_t *id_, lfsr_tag_t *tag_, lfs_size_t *weight_,
        lfsr_data_t *data_) {
    // tag must be valid at this point
    LFS_ASSERT(lfsr_tag_isvalid(tag));
    // these bits should be clear at this point
    LFS_ASSERT(lfsr_tag_mode(tag) == 0x0000);

    // make sure we never look up zero tags, the way we create
    // unreachable tags has a hole here
    tag = lfs_max16(tag, 0x1);

    // keep track of bounds as we descend down the tree
    lfs_off_t branch = rbyd->trunk;
    lfs_ssize_t lower = -1;
    lfs_ssize_t upper = rbyd->weight;

    // no trunk yet?
    if (!branch) {
        return LFS_ERR_NOENT;
    }

    // descend down tree
    while (true) {
        lfsr_tag_t alt;
        lfs_size_t weight;
        lfs_off_t jump;
        lfs_ssize_t d = lfsr_bd_readtag(lfs,
                rbyd->block, branch, 0,
                &alt, &weight, &jump, NULL);
        if (d < 0) {
            return d;
        }

        // found an alt?
        if (lfsr_tag_isalt(alt)) {
            if (lfsr_tag_follow(alt, weight, lower, upper, id, tag)) {
                lfsr_tag_flip(&alt, &weight, lower, upper);
                lfsr_tag_trim(alt, weight, &lower, &upper, NULL, NULL);
                branch = branch - jump;
            } else {
                lfsr_tag_trim(alt, weight, &lower, &upper, NULL, NULL);
                branch = branch + d;
            }

        // found end of tree?
        } else {
            // update the tag id
            lfs_ssize_t id__ = upper-1;
            lfsr_tag_t tag__ = alt;
            LFS_ASSERT(lfsr_tag_mode(tag__) == 0x0000);

            // not what we're looking for?
            if (!tag__
                    || id__ < id
                    || (id__ == id && tag__ < tag)) {
                return LFS_ERR_NOENT;
            }

            // save what we found
            // TODO how many of these need to be conditional?
            if (id_) {
                *id_ = id__;
            }
            if (tag_) {
                *tag_ = tag__;
            }
            if (weight_) {
                *weight_ = id__ - lower;
            }
            if (data_) {
                *data_ = LFSR_DATA_DISK(rbyd->block, branch + d, jump);
            }
            return 0;
        }
    }
}

static int lfsr_rbyd_lookup(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfs_ssize_t id, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfs_ssize_t id_;
    lfsr_tag_t tag__;
    int err = lfsr_rbyd_lookupnext(lfs, rbyd, id, lfsr_tag_clearwide(tag),
            &id_, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // lookup finds the next-smallest tag, all we need to do is fail if it
    // picks up the wrong tag
    //
    // we accept either exact matches or suptype matches depending on the
    // wide bit
    if (id_ != id
            || (lfsr_tag_iswide(tag)
                ? lfsr_tag_suptype(tag__) != lfsr_tag_clearwide(tag)
                : tag__ != tag)) {
        return LFS_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}

// TODO still need this?
// TODO move this into the tests?
static lfs_ssize_t lfsr_rbyd_get(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfs_ssize_t id, lfsr_tag_t tag, void *buffer, lfs_size_t size) {
    lfsr_data_t data;
    int err = lfsr_rbyd_lookup(lfs, rbyd, id, tag, NULL, &data);
    if (err) {
        return err;
    }

    return lfsr_data_read(lfs, data, 0, buffer, size);
}


// append a revision count
//
// this is optional, if not called revision count defaults to 0 (for btrees)
static int lfsr_rbyd_appendrev(lfs_t *lfs, lfsr_rbyd_t *rbyd, uint32_t rev) {
    // should only be called before any tags are written
    LFS_ASSERT(rbyd->off == 0);

    // revision count stored as le32, we don't use a leb128 encoding as we
    // intentionally allow the revision count to overflow
    uint8_t buf[sizeof(uint32_t)];
    lfs_tole32_(rev, &buf);
    int err = lfsr_bd_prog(lfs, rbyd->block, rbyd->off,
            &buf, sizeof(uint32_t), &rbyd->crc);
    if (err) {
        goto failed;
    }
    rbyd->off += sizeof(uint32_t);

    return 0;

failed:
    // if we fail mark the rbyd as unerased and release the pcache
    lfs_cache_zero(lfs, &lfs->pcache);
    rbyd->off = lfs->cfg->block_size;
    return err;
}

// helper functions for managing the 3-element fifo used in lfsr_rbyd_append
static int lfsr_rbyd_p_flush(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_tag_t p_alts[static 3],
        lfs_size_t p_weights[static 3],
        lfs_off_t p_jumps[static 3],
        unsigned count) {
    // write out some number of alt pointers in our queue
    for (unsigned i = 0; i < count; i++) {
        if (p_alts[3-1-i]) {
            // change to a relative jump at the last minute
            lfsr_tag_t alt = p_alts[3-1-i];
            lfs_size_t weight = p_weights[3-1-i];
            lfs_off_t jump = rbyd->off - p_jumps[3-1-i];

            lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->block, rbyd->off,
                    alt, weight, jump,
                    &rbyd->crc);
            if (d < 0) {
                return d;
            }
            rbyd->off += d;
        }
    }

    return 0;
}

static inline int lfsr_rbyd_p_push(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_tag_t p_alts[static 3],
        lfs_size_t p_weights[static 3],
        lfs_off_t p_jumps[static 3],
        lfsr_tag_t alt, lfs_ssize_t weight, lfs_off_t jump) {
    int err = lfsr_rbyd_p_flush(lfs, rbyd, p_alts, p_weights, p_jumps, 1);
    if (err) {
        return err;
    }

    memmove(p_alts+1, p_alts, 2*sizeof(lfsr_tag_t));
    memmove(p_weights+1, p_weights, 2*sizeof(lfs_size_t));
    memmove(p_jumps+1, p_jumps, 2*sizeof(lfs_off_t));
    p_alts[0] = alt;
    p_weights[0] = weight;
    p_jumps[0] = jump;

    return 0;
}

static inline void lfsr_rbyd_p_pop(
        lfsr_tag_t p_alts[static 3],
        lfs_size_t p_weights[static 3],
        lfs_off_t p_jumps[static 3]) {
    memmove(p_alts, p_alts+1, 2*sizeof(lfsr_tag_t));
    memmove(p_weights, p_weights+1, 2*sizeof(lfs_size_t));
    memmove(p_jumps, p_jumps+1, 2*sizeof(lfs_off_t));
    p_alts[2] = 0;
    p_weights[2] = 0;
    p_jumps[2] = 0;
}

static void lfsr_rbyd_p_red(
        lfsr_tag_t p_alts[static 3],
        lfs_size_t p_weights[static 3],
        lfs_off_t p_jumps[static 3]) {
    // propagate a red edge upwards
    p_alts[0] = lfsr_tag_setblack(p_alts[0]);

    if (p_alts[1]) {
        p_alts[1] = lfsr_tag_setred(p_alts[1]);

        // reorder so that top two edges always go in the same direction
        if (lfsr_tag_isred(p_alts[2])) {
            if (lfsr_tag_isparallel(p_alts[1], p_alts[2])) {
                // no reorder needed
            } else if (lfsr_tag_isparallel(p_alts[0], p_alts[2])) {
                lfsr_tag_t alt_ = p_alts[1];
                lfs_size_t weight_ = p_weights[1];
                lfs_off_t jump_ = p_jumps[1];
                p_alts[1] = lfsr_tag_setred(p_alts[0]);
                p_weights[1] = p_weights[0];
                p_jumps[1] = p_jumps[0];
                p_alts[0] = lfsr_tag_setblack(alt_);
                p_weights[0] = weight_;
                p_jumps[0] = jump_;
            } else if (lfsr_tag_isparallel(p_alts[0], p_alts[1])) {
                lfsr_tag_t alt_ = p_alts[2];
                lfs_size_t weight_ = p_weights[2];
                lfs_off_t jump_ = p_jumps[2];
                p_alts[2] = lfsr_tag_setred(p_alts[1]);
                p_weights[2] = p_weights[1];
                p_jumps[2] = p_jumps[1];
                p_alts[1] = lfsr_tag_setred(p_alts[0]);
                p_weights[1] = p_weights[0];
                p_jumps[1] = p_jumps[0];
                p_alts[0] = lfsr_tag_setblack(alt_);
                p_weights[0] = weight_;
                p_jumps[0] = jump_;
            } else {
                LFS_UNREACHABLE();
            }
        }
    }
}

// core rbyd algorithm
static int lfsr_rbyd_append(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_ssize_t id, lfsr_tag_t tag, lfs_ssize_t delta,
        lfsr_data_t data) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));
    // tag must be valid at this point
    LFS_ASSERT(lfsr_tag_isvalid(tag));
    // never write zero tags to disk, use unr if tag contains no data
    LFS_ASSERT(tag != 0);
    // reserve bit 7 to allow leb128 subtypes in the future
    LFS_ASSERT(!(tag & 0x80));

    // we can't do anything if we're not erased
    int err;
    if (rbyd->off >= lfs->cfg->block_size) {
        err = LFS_ERR_RANGE;
        goto failed;
    }

    // ignore noops
    // TODO is there a better way to represent noops?
    if (lfsr_tag_cleargrow(tag) == LFSR_TAG_UNR && delta == 0) {
        return 0;
    }

    // make sure every rbyd starts with a revision count
    if (rbyd->off == 0) {
        err = lfsr_rbyd_appendrev(lfs, rbyd, 0);
        if (err) {
            goto failed;
        }
    }

    // figure out the range of tags we're operating on
    //
    // several lower bits are reserved, so we repurpose these
    // to keep track of some append state
    lfs_ssize_t id_;
    lfs_ssize_t other_id_;
    lfsr_tag_t tag_;
    lfsr_tag_t other_tag_;
    if (delta != 0 && !lfsr_tag_isgrow(tag)) {
        LFS_ASSERT(!lfsr_tag_iswide(tag));

        if (delta > 0) {
            LFS_ASSERT(id <= (lfs_ssize_t)rbyd->weight);

            // it's a bit ugly, but adjusting the id here makes the following
            // logic work out more consistently
            id -= 1;
            id_ = id + 1;
            other_id_ = id + 1;
        } else {
            LFS_ASSERT(id < (lfs_ssize_t)rbyd->weight);

            // it's a bit ugly, but adjusting the id here makes the following
            // logic work out more consistently
            id += 1;
            id_ = id - lfs_smax32(-delta, 0);
            other_id_ = id;
        }

        // note these tags MUST NOT be zero, due to unreachable tag holes
        tag_ = 0x1;
        other_tag_ = tag_;

    } else {
        LFS_ASSERT(id < (lfs_ssize_t)rbyd->weight);

        id_ = id - lfs_smax32(-delta, 0);
        other_id_ = id;

        // note both normal and rm wide-tags have the same bounds, really it's
        // the normal non-wide-tags that are an outlier here
        if (lfsr_tag_iswide(tag)) {
            tag_ = lfsr_tag_suptype(lfsr_tag_key(tag));
            other_tag_ = tag_ + 0x100;
        } else if (lfsr_tag_isrm(tag)) {
            tag_ = lfsr_tag_key(tag);
            other_tag_ = tag_ + 0x1;
        } else {
            tag_ = lfsr_tag_key(tag);
            other_tag_ = tag_;
        }
    }
    // mark as invalid until found
    tag_ = lfsr_tag_setinvalid(tag_);
    other_tag_ = lfsr_tag_setinvalid(other_tag_);

    // keep track of bounds as we descend down the tree
    //
    // this gets a bit confusing as we also may need to keep
    // track of both the lower and upper bounds of diverging paths
    // in the case of range deletions
    lfs_off_t branch = rbyd->trunk;
    lfs_ssize_t lower_id = -1;
    lfs_ssize_t upper_id = rbyd->weight;
    lfsr_tag_t lower_tag = 0;
    lfsr_tag_t upper_tag = 0xffff;

    // diverged state in case we are removing a range from the tree
    //
    // this is a second copy of the search path state, used to keep track
    // of two search paths simulaneously when our range diverges.
    //
    // note we can't just perform two searches sequentially, or else our tree
    // will end up very unbalanced.
    lfs_off_t other_branch = 0;
    lfs_ssize_t other_lower_id = 0;
    lfs_ssize_t other_upper_id = 0;
    lfsr_tag_t other_lower_tag = 0;
    lfsr_tag_t other_upper_tag = 0;

    // go ahead and update the rbyd's weight, if an error occurs our
    // rbyd is no longer usable anyways
    LFS_ASSERT(delta >= -(lfs_ssize_t)rbyd->weight);
    rbyd->weight += delta;

    // assume we'll update our trunk
    rbyd->trunk = rbyd->off;

    // no trunk yet?
    if (!branch) {
        goto leaf;
    }

    // queue of pending alts we can emulate rotations with
    lfsr_tag_t p_alts[3] = {0, 0, 0};
    lfs_size_t p_weights[3] = {0, 0, 0};
    lfs_off_t p_jumps[3] = {0, 0, 0};
    lfs_off_t graft = 0;

    // descend down tree, building alt pointers
    while (true) {
        // read the alt pointer
        lfsr_tag_t alt;
        lfs_size_t weight;
        lfs_off_t jump;
        lfs_ssize_t d = lfsr_bd_readtag(lfs,
                rbyd->block, branch, 0,
                &alt, &weight, &jump, NULL);
        if (d < 0) {
            err = d;
            goto failed;
        }

        // found an alt?
        if (lfsr_tag_isalt(alt)) {
            // make jump absolute
            jump = branch - jump;
            lfs_off_t branch_ = branch + d;

            // do bounds want to take different paths? begin cutting
            if (!lfsr_tag_hasdiverged(tag_)
                    && lfsr_tag_follow2(alt, weight,
                            p_alts[0], p_weights[0],
                            lower_id, upper_id,
                            id_, tag_)
                        != lfsr_tag_follow2(alt, weight,
                            p_alts[0], p_weights[0],
                            lower_id, upper_id,
                            other_id_, other_tag_)) {
                // first take care of any lingering red alts
                if (lfsr_tag_isred(p_alts[0])) {
                    alt = lfsr_tag_setblack(p_alts[0]);
                    weight = p_weights[0];
                    jump = p_jumps[0];
                    branch_ = branch;
                    lfsr_rbyd_p_pop(p_alts, p_weights, p_jumps);
                } else {
                    tag_ = lfsr_tag_setdivergedlower(tag_);
                    other_tag_ = lfsr_tag_setdivergedupper(other_tag_);
                    other_branch = branch;
                    other_lower_id = lower_id;
                    other_upper_id = upper_id;
                    other_lower_tag = lower_tag;
                    other_upper_tag = upper_tag;
                }
            }

            // if we're diverging, go ahead and make alt black, this isn't
            // perfect but it's simpler and compact will take care of any
            // balance issues that may occur
            if (lfsr_tag_hasdiverged(tag_)) {
                alt = lfsr_tag_setblack(alt);
            }

            // prune?
            //            <b                    >b
            //          .-'|                  .-'|
            //         <y  |                  |  |
            // .-------'|  |                  |  |
            // |       <r  |  =>              | <b
            // |  .----'   |      .-----------|-'|
            // |  |       <b      |          <b  |
            // |  |  .----'|      |     .----'|  |
            // 1  2  3  4  4      1  2  3  4  4  2
            if (lfsr_tag_prune2(
                    alt, weight,
                    p_alts[0], p_weights[0],
                    lower_id, upper_id,
                    lower_tag, upper_tag)) {
                if (lfsr_tag_isred(p_alts[0])) {
                    alt = lfsr_tag_setblack(p_alts[0]);
                    weight = p_weights[0];
                    branch_ = jump;
                    jump = p_jumps[0];
                    lfsr_rbyd_p_pop(p_alts, p_weights, p_jumps);
                } else {
                    branch = jump;
                    continue;
                }
            }

            // two reds makes a yellow, split?
            if (lfsr_tag_isred(alt) && lfsr_tag_isred(p_alts[0])) {
                LFS_ASSERT(lfsr_tag_isparallel(alt, p_alts[0]));

                // if we take the red or yellow alt we can just point
                // to the black alt
                //         <y                 >b
                // .-------'|               .-'|
                // |       <r               | >b
                // |  .----'|  =>     .-----|-'|
                // |  |    <b         |    <b  |
                // |  |  .-'|         |  .-'|  |
                // 1  2  3  4      1  2  3  4  1
                if (lfsr_tag_follow2(
                        alt, weight,
                        p_alts[0], p_weights[0],
                        lower_id, upper_id,
                        id_, tag_)) {
                    lfsr_tag_flip2(&alt, &weight,
                            p_alts[0], p_weights[0],
                            lower_id, upper_id);
                    lfs_swap32(&jump, &branch_);

                    lfs_swap16(&p_alts[0], &alt);
                    lfs_swap32(&p_weights[0], &weight);
                    lfs_swap32(&p_jumps[0], &jump);
                    alt = lfsr_tag_setblack(alt);

                    lfsr_tag_trim(
                            p_alts[0], p_weights[0],
                            &lower_id, &upper_id,
                            &lower_tag, &upper_tag);
                    lfsr_rbyd_p_red(p_alts, p_weights, p_jumps);

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
                    LFS_ASSERT(graft != 0);
                    p_alts[0] = alt;
                    p_weights[0] += weight;
                    p_jumps[0] = graft;

                    lfsr_tag_trim(
                            p_alts[0], p_weights[0],
                            &lower_id, &upper_id,
                            &lower_tag, &upper_tag);
                    lfsr_rbyd_p_red(p_alts, p_weights, p_jumps);

                    branch = branch_;
                    continue;
                }
            }

            // take black alt? needs a flip
            //   <b           >b
            // .-'|  =>     .-'|
            // 1  2      1  2  1
            if (lfsr_tag_isblack(alt)
                    && lfsr_tag_follow2(
                        alt, weight,
                        p_alts[0], p_weights[0],
                        lower_id, upper_id,
                        id_, tag_)) {
                lfsr_tag_flip2(&alt, &weight,
                        p_alts[0], p_weights[0],
                        lower_id, upper_id);
                lfs_swap32(&jump, &branch_);
            }

            // should've taken red alt? needs a flip
            //      <r              >r
            // .----'|            .-'|
            // |    <b  =>        | >b
            // |  .-'|         .--|-'|
            // 1  2  3      1  2  3  1
            if (lfsr_tag_isred(p_alts[0])
                    && lfsr_tag_follow(p_alts[0], p_weights[0],
                        lower_id, upper_id,
                        id_, tag_)) {
                lfs_swap16(&p_alts[0], &alt);
                lfs_swap32(&p_weights[0], &weight);
                lfs_swap32(&p_jumps[0], &jump);
                p_alts[0] = lfsr_tag_setred(p_alts[0]);
                alt = lfsr_tag_setblack(alt);

                lfsr_tag_flip2(&alt, &weight,
                        p_alts[0], p_weights[0],
                        lower_id, upper_id);
                lfs_swap32(&jump, &branch_);
            }

            // trim alt from our current bounds
            if (lfsr_tag_isblack(alt)) {
                lfsr_tag_trim2(
                        alt, weight,
                        p_alts[0], p_weights[0],
                        &lower_id, &upper_id,
                        &lower_tag, &upper_tag);
            }
            // continue to next alt
            graft = branch;
            branch = branch_;

            // prune inner alts if our tags diverged
            if (lfsr_tag_hasdiverged(tag_)
                    && lfsr_tag_isdivergedupper(tag_) != lfsr_tag_isgt(alt)) {
                continue;
            }

            // push alt onto our queue
            err = lfsr_rbyd_p_push(lfs, rbyd,
                    p_alts, p_weights, p_jumps,
                    alt, weight, jump);
            if (err) {
                goto failed;
            }

        // found end of tree?
        } else {
            // update the found tag/id
            //
            // note we:
            // - clear valid bit, marking the tag as found
            // - preserve diverged state
            LFS_ASSERT(lfsr_tag_mode(alt) == 0x0000);
            tag_ = lfsr_tag_setvalid(lfsr_tag_mode(tag_) | alt);
            id_ = upper_id-1;

            // done?
            if (!lfsr_tag_hasdiverged(tag_) || lfsr_tag_isvalid(other_tag_)) {
                break;
            }
        }

        // switch to the other path if we have diverged
        if (lfsr_tag_hasdiverged(tag_) || !lfsr_tag_isalt(alt)) {
            lfs_swap16(&tag_, &other_tag_);
            lfs_sswap32(&id_, &other_id_);
            lfs_swap32(&branch, &other_branch);
            lfs_sswap32(&lower_id, &other_lower_id);
            lfs_sswap32(&upper_id, &other_upper_id);
            lfs_swap16(&lower_tag, &other_lower_tag);
            lfs_swap16(&upper_tag, &other_upper_tag);
        }
    }

    // the last alt should always end up black
    LFS_ASSERT(lfsr_tag_isblack(p_alts[0]));

    // if we diverged, merge the bounds
    LFS_ASSERT(lfsr_tag_isvalid(tag_));
    LFS_ASSERT(!lfsr_tag_hasdiverged(tag_) || lfsr_tag_isvalid(other_tag_));
    if (lfsr_tag_hasdiverged(tag_)) {
        if (lfsr_tag_isdivergedlower(tag_)) {
            // finished on lower path
            tag_ = other_tag_;
            id_ = other_id_;
            branch = other_branch;
            upper_id = other_upper_id;
        } else {
            // finished on upper path
            lower_id = other_lower_id;
        }
    }

    // split leaf nodes?
    //
    // note we bias the weights here so that lfsr_rbyd_lookupnext
    // always finds the next biggest tag
    //
    // note also if lfsr_tag_key(tag_) is null, we found a removed tag that
    // we should just prune
    //
    // this gets real messy because we have a lot of special behavior built in:
    // - default          => split if tags mismatch
    // - delta > 0, !grow => split if tags mismatch or we're inserting a new tag
    // - wide-bit set     => split if suptype of tags mismatch
    // - rm-bit set       => never split, but emit alt-always tags, making our
    //                       tag effectively unreachable
    //
    lfsr_tag_t alt = 0;
    lfs_size_t weight = 0;
    if (lfsr_tag_key(tag_)
            && (id_ < id-lfs_smax32(-delta, 0)
                || (id_ == id-lfs_smax32(-delta, 0)
                    && ((delta > 0 && !lfsr_tag_isgrow(tag))
                        || (lfsr_tag_iswide(tag)
                            ? lfsr_tag_suptype(lfsr_tag_key(tag_))
                                < lfsr_tag_suptype(lfsr_tag_key(tag))
                            : lfsr_tag_key(tag_)
                                < lfsr_tag_key(tag)))))) {
        if (lfsr_tag_isrm(tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALTGT(false, 0);
            weight = upper_id - lower_id - 1 + delta;
            upper_id -= weight;
        } else {
            // split less than
            alt = LFSR_TAG_ALTLE(
                    !lfsr_tag_hasdiverged(tag_),
                    lfsr_tag_key(tag_));
            weight = id_ - lower_id;
            lower_id += weight;
        }

    } else if (lfsr_tag_key(tag_)
            && (id_ > id
                || (id_ == id
                    && ((delta > 0 && !lfsr_tag_isgrow(tag))
                        || (lfsr_tag_iswide(tag)
                            ? lfsr_tag_suptype(lfsr_tag_key(tag_))
                                > lfsr_tag_suptype(lfsr_tag_key(tag))
                            : lfsr_tag_key(tag_)
                                > lfsr_tag_key(tag)))))) {
        if (lfsr_tag_isrm(tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALTGT(false, 0);
            weight = upper_id - lower_id - 1 + delta;
            upper_id -= weight;
        } else {
            // split greater than
            alt = LFSR_TAG_ALTGT(
                    !lfsr_tag_hasdiverged(tag_),
                    lfsr_tag_key(tag));
            weight = upper_id - id - 1;
            upper_id -= weight;
        }
    }

    if (alt) {
        err = lfsr_rbyd_p_push(lfs, rbyd,
                p_alts, p_weights, p_jumps,
                alt, weight, branch);
        if (err) {
            goto failed;
        }

        if (lfsr_tag_isred(p_alts[0])) {
            // introduce a red edge
            lfsr_rbyd_p_red(p_alts, p_weights, p_jumps);
        }
    }

    // flush any pending alts
    err = lfsr_rbyd_p_flush(lfs, rbyd,
            p_alts, p_weights, p_jumps, 3);
    if (err) {
        goto failed;
    }

leaf:;
    // write the actual tag
    //
    // note we always need a non-alt to terminate the trunk, otherwise we
    // can't find trunks during fetch
    lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->block, rbyd->off,
            // rm => null, otherwise strip off control bits
            (lfsr_tag_isrm(tag) ? LFSR_TAG_NULL : lfsr_tag_key(tag)),
            upper_id - lower_id - 1 + delta,
            lfsr_data_size(data),
            &rbyd->crc);
    if (d < 0) {
        err = d;
        goto failed;
    }
    rbyd->off += d;

    // don't forget the data!
    err = lfsr_bd_progdata(lfs, rbyd->block, rbyd->off, data,
            &rbyd->crc);
    if (err) {
        goto failed;
    }
    rbyd->off += lfsr_data_size(data);

    return 0;

failed:;
    // if we fail mark the rbyd as unerased and release the pcache
    lfs_cache_zero(lfs, &lfs->pcache);
    rbyd->off = lfs->cfg->block_size;
    return err;
}

static int lfsr_rbyd_appendall(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_ssize_t start_id, lfs_ssize_t end_id,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // append each tag to the tree
    for (lfs_size_t i = 0; i < attr_count; i++) {
        if (attrs[i].id >= start_id && (end_id < 0 || attrs[i].id < end_id)) {
            int err = lfsr_rbyd_append(lfs, rbyd,
                    attrs[i].id-lfs_smax32(start_id, 0),
                    attrs[i].tag, attrs[i].delta, attrs[i].data);
            if (err) {
                return err;
            }
        }

        // we need to make sure we keep start_id/end_id updated with
        // weight changes
        if (attrs[i].id < start_id) {
            start_id += attrs[i].delta;
        }
        if (attrs[i].id < end_id) {
            end_id += attrs[i].delta;
        }
    }

    return 0;
}

static int lfsr_rbyd_compact(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_ssize_t start_id, lfs_ssize_t end_id,
        const lfsr_rbyd_t *source) {
#ifndef LFSR_NO_REBALANCE
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));

    // we can't do anything if we're not erased
    int err;
    if (rbyd->off >= lfs->cfg->block_size) {
        err = LFS_ERR_RANGE;
        goto failed;
    }

    // make sure every rbyd starts with a revision count
    if (rbyd->off == 0) {
        err = lfsr_rbyd_appendrev(lfs, rbyd, 0);
        if (err) {
            goto failed;
        }
    }

    // keep track of the number of trunks and weight in each layer
    lfs_size_t layer_trunks = 0;
    lfs_size_t layer_w = 0;

    // first copy over raw tags, note this doesn't create a tree
    lfs_off_t layer_start = rbyd->off;
    lfs_ssize_t id = start_id;
    lfsr_tag_t tag = 0;
    while (true) {
        lfs_size_t w;
        lfsr_data_t data;
        int err = lfsr_rbyd_lookupnext(lfs, source, id, lfsr_tag_next(tag),
                &id, &tag, &w, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT || (end_id >= 0 && id >= end_id)) {
            break;
        }

        // write the tag
        lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->block, rbyd->off,
                tag, w, lfsr_data_size(data),
                &rbyd->crc);
        if (d < 0) {
            err = d;
            goto failed;
        }
        rbyd->off += d;

        // and the data
        err = lfsr_bd_progdata(lfs, rbyd->block, rbyd->off, data,
                &rbyd->crc);
        if (err) {
            goto failed;
        }
        rbyd->off += lfsr_data_size(data);

        // keep track of the layer weight/trunks
        layer_trunks += 1;
        layer_w += w;
    }
    lfs_off_t layer_end = rbyd->off;

    // connect every other trunk together, building layers of a perfectly
    // balanced binary tree upwards until we have a single trunk
    while (layer_trunks > 1) {
        // keep track of new layer trunks/weight
        layer_trunks = 0;
        layer_w = 0;

        lfs_off_t off = layer_start;
        layer_start = rbyd->off;
        while (off < layer_end) {
            // connect two trunks together with a new binary trunk
            for (int i = 0; i < 2 && off < layer_end; i++) {
                lfs_off_t trunk_off = off;
                lfsr_tag_t trunk_tag = 0;
                lfs_size_t trunk_w = 0;
                while (true) {
                    lfsr_tag_t tag;
                    lfs_size_t w;
                    lfs_size_t size;
                    lfs_ssize_t d = lfsr_bd_readtag(lfs, rbyd->block, off,
                            layer_end-off,
                            &tag, &w, &size, NULL);
                    if (d < 0) {
                        err = d;
                        goto failed;
                    }
                    off += d;

                    // skip any data
                    if (!lfsr_tag_isalt(tag)) {
                        off += size;
                    }

                    // keep track of trunk/layer weight, and the last non-null
                    // tag in our trunk. Because of how we construct each layer,
                    // the last non-null tag is the largest tag in that part
                    // of the tree
                    trunk_w += w;
                    layer_w += w;
                    if (tag) {
                        trunk_tag = tag;
                    }

                    // read all tags in the trunk
                    if (!lfsr_tag_isalt(tag)) {
                        break;
                    }
                }

                // connect with an altle
                lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->block, rbyd->off,
                        LFSR_TAG_ALTLE(false, lfsr_tag_key(trunk_tag)),
                        trunk_w,
                        rbyd->off - trunk_off,
                        &rbyd->crc);
                if (d < 0) {
                    err = d;
                    goto failed;
                }
                rbyd->off += d;
            }

            // terminate with a null tag
            lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->block, rbyd->off,
                    LFSR_TAG_NULL, 0, 0,
                    &rbyd->crc);
            if (d < 0) {
                err = d;
                goto failed;
            }
            rbyd->off += d;

            // keep track of the number of trunks
            layer_trunks += 1;
        }
        layer_end = rbyd->off;
    }

    // done! just need to update our trunk/weight, note we could have
    // no trunks after compaction. Leave this to upper layers to take
    // care of
    if (layer_trunks >= 1) {
        rbyd->trunk = layer_start;
    }
    rbyd->weight = layer_w;

    return 0;

failed:;
    // if we fail mark the rbyd as unerased and release the pcache
    lfs_cache_zero(lfs, &lfs->pcache);
    rbyd->off = lfs->cfg->block_size;
    return err;

#else
    // try to copy over tags
    lfs_ssize_t id = start_id;
    lfsr_tag_t tag = 0;
    while (true) {
        lfs_size_t w;
        lfsr_data_t data;
        int err = lfsr_rbyd_lookupnext(lfs, source, id, lfsr_tag_next(tag),
                &id, &tag, &w, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT || (end_id >= 0 && id >= end_id)) {
            return 0;
        }

        // append the attr
        err = lfsr_rbyd_append(lfs, rbyd,
                id-lfs_smax32(w-1, 0)-lfs_smax32(start_id, 0),
                tag, +w, data);
        if (err) {
            return err;
        }
    }
#endif
}

static int lfsr_rbyd_commit(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));

    // we can't do anything if we're not erased
    int err;
    if (rbyd->off >= lfs->cfg->block_size) {
        err = LFS_ERR_RANGE;
        goto failed;
    }

    // setup commit state, use a separate rbyd so we have a fallback in
    // case of error
    lfsr_rbyd_t rbyd_ = *rbyd;

    // make sure every rbyd starts with its revision count
    if (rbyd_.off == 0) {
        err = lfsr_rbyd_appendrev(lfs, &rbyd_, 0);
        if (err) {
            goto failed;
        }
    }

    // append each tag to the tree
    err = lfsr_rbyd_appendall(lfs, &rbyd_, -1, -1,
            attrs, attr_count);
    if (err) {
        goto failed;
    }

    // align to the next prog unit
    //
    // this gets a bit complicated as we have two types of crcs:
    //
    // - 9-word crc with fcrc to check following prog (middle of block)
    //   - fcrc tag type => 2 byte le16
    //   - fcrc tag id   => 1 byte leb128
    //   - fcrc tag size => 1 byte leb128 (worst case)
    //   - fcrc crc      => 4 byte le32
    //   - fcrc size     => 5 byte leb128 (worst case)
    //   - crc tag type  => 2 byte le16
    //   - crc tag id    => 1 byte leb128
    //   - crc tag size  => 5 byte leb128 (worst case)
    //   - crc crc       => 4 byte le32
    //                   => 25 bytes total
    //
    // - 4-word crc with no following prog (end of block)
    //   - crc tag type => 2 byte le16
    //   - crc tag id   => 1 byte leb128
    //   - crc tag size => 5 byte leb128 (worst case)
    //   - crc crc      => 4 byte le32
    //                  => 12 bytes total
    //
    lfs_off_t aligned = lfs_alignup(
            rbyd_.off + 2+1+1+4+5 + 2+1+5+4,
            lfs->cfg->prog_size);

    // space for fcrc?
    uint8_t perturb = 0;
    if (aligned < lfs->cfg->block_size) {
        // read the leading byte in case we need to change the expected
        // value of the next tag's valid bit
        int err = lfsr_bd_read(lfs, rbyd_.block, aligned, lfs->cfg->prog_size,
                &perturb, 1);
        if (err && err != LFS_ERR_CORRUPT) {
            rbyd->off = lfs->cfg->block_size;
            return err;
        }

        // find the expected fcrc, don't bother avoiding a reread of the
        // perturb byte, as it should still be in our cache
        lfsr_fcrc_t fcrc = {.crc=0, .size=lfs->cfg->prog_size};
        err = lfsr_bd_csum(lfs, rbyd_.block, aligned, lfs->cfg->prog_size,
                lfs->cfg->prog_size,
                &fcrc.crc);
        if (err && err != LFS_ERR_CORRUPT) {
            goto failed;
        }

        uint8_t buf[LFSR_FCRC_DSIZE];
        lfs_size_t d = lfsr_fcrc_todisk(lfs, &fcrc, buf);
        lfs_ssize_t d_ = lfsr_bd_progtag(lfs, rbyd_.block, rbyd_.off,
                LFSR_TAG_FCRC, 0, d,
                &rbyd_.crc);
        if (d_ < 0) {
            err = d_;
            goto failed;
        }
        rbyd_.off += d_;

        err = lfsr_bd_prog(lfs, rbyd_.block, rbyd_.off,
                buf, d, &rbyd_.crc);
        if (err) {
            goto failed;
        }
        rbyd_.off += d;

    // at least space for a crc?
    } else if (rbyd_.off + 2+1+5+4 <= lfs->cfg->block_size) {
        // note this implicitly marks the rbyd as unerased
        aligned = lfs->cfg->block_size;

    // not even space for a crc? we can't finish the commit
    } else {
        err = LFS_ERR_RANGE;
        goto failed;
    }

    // build end-of-commit crc
    //
    // note padding-size depends on leb-encoding depends on padding-size, to
    // get around this catch-22 we just always write a fully-expanded leb128
    // encoding
    uint8_t buf[2+1+5+4];
    buf[0] = (LFSR_TAG_CRC >> 8) | ((lfs_popc(rbyd_.crc) & 1) << 7);
    buf[1] = 0;
    buf[2] = 0;

    lfs_off_t padding = aligned - (rbyd_.off + 2+1+5);
    buf[3] = 0x80 | (0x7f & (padding >>  0));
    buf[4] = 0x80 | (0x7f & (padding >>  7));
    buf[5] = 0x80 | (0x7f & (padding >> 14));
    buf[6] = 0x80 | (0x7f & (padding >> 21));
    buf[7] = 0x00 | (0x7f & (padding >> 28));

    rbyd_.crc = lfs_crc32c(rbyd_.crc, buf, 2+1+5);
    // we can't let the next tag appear as valid, so intentionally perturb the
    // commit if this happens, note parity(crc(m)) == parity(m) with crc32c,
    // so we can really change any bit to make this happen, we've reserved a bit
    // in crc tags just for this purpose
    if ((lfs_popc(rbyd_.crc) & 1) == (perturb >> 7)) {
        buf[1] ^= 0x01;
        rbyd_.crc ^= 0x68032cc8; // note crc(a ^ b) == crc(a) ^ crc(b)
    }
    lfs_tole32_(rbyd_.crc, &buf[2+1+5]);

    err = lfsr_bd_prog(lfs, rbyd_.block, rbyd_.off, buf, 2+1+5+4, NULL);
    if (err) {
        goto failed;
    }
    rbyd_.off += 2+1+5+4;

    // flush our caches, finalizing the commit on-disk
    err = lfsr_bd_sync(lfs);
    if (err) {
        goto failed;
    }

    // succesful commit, check checksum to make sure
    uint32_t crc_ = rbyd->crc;
    err = lfsr_bd_csum(lfs, rbyd_.block, rbyd->off, 0,
            rbyd_.off-4 - rbyd->off,
            &crc_);
    if (err) {
        goto failed;
    }

    if (rbyd_.crc != crc_) {
        // oh no, something went wrong
        LFS_ERROR("Rbyd corrupted during commit "
                "(block=0x%"PRIx32", 0x%08"PRIx32" != 0x%08"PRIx32")",
                rbyd_.block, rbyd_.crc, crc_);
        err = LFS_ERR_CORRUPT;
        goto failed;
    }

    // ok, everything is good, save what we've committed
    rbyd_.off = aligned;
    *rbyd = rbyd_;
    return 0;

failed:;
    // if we fail mark the rbyd as unerased and release the pcache
    lfs_cache_zero(lfs, &lfs->pcache);
    rbyd->off = lfs->cfg->block_size;
    return err;
}


// the following are mostly btree helpers, but since they operate on rbyds,
// exist in the rbyd namespace

// determine if a given rbyd will be within the compaction threshold (1/2)
// after compaction, note this uses a conservative estimate so the actual
// on-disk cost may be smaller
//
// if rbyd does not fit, a good split_id is returned
static int lfsr_rbyd_estimate(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfs_ssize_t start_id, lfs_size_t threshold,
        lfs_size_t *split_id_) {
#ifndef LFSR_NO_REBALANCE
    lfs_size_t dsize = 0;

    lfs_ssize_t id = start_id;
    lfsr_tag_t tag = 0;
    while (true) {
        lfs_size_t w;
        lfsr_data_t data;
        int err = lfsr_rbyd_lookupnext(lfs, rbyd, id, lfsr_tag_next(tag),
                &id, &tag, &w, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT) {
            return true;
        }

        // determine the upper-bound of alt pointers, tags, and data
        // after compaction
        //
        // note that with rebalancing during compaction, we know the number
        // of inner nodes is the same as the number of tags. Each node has
        // two alts and is terminated by a 4-byte null tag.
        dsize += LFSR_TAG_DSIZE + lfsr_data_size(data)
                + 2*LFSR_TAG_DSIZE + 4;

        // exceeded our compaction threshold?
        if (dsize > threshold) {
            // requested a split id?
            if (split_id_) {
                // TODO is this really worth it vs a simpler algorithm?
                //
                // here we ignore the cost of alt-pointers, and only use the
                // tag+data cost as a heuristic
                //
                // we assume we already found an over-estimate of the split id
                // so we only need to work backwards through the rbyd to
                // correct the over-estimate. This is a very minor optimization.
                //
                lfs_size_t lower_id = id+1;
                lfs_ssize_t upper_id = rbyd->weight-1;
                lfs_size_t lower_dsize = dsize;
                lfs_size_t upper_dsize = 0;
                while (true) {
                    lfsr_tag_t tag = 0;
                    lfs_size_t w = 0;
                    lfs_size_t dsize = 0;
                    while (true) {
                        lfs_ssize_t id_;
                        lfs_size_t w_;
                        lfsr_data_t data;
                        int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                                upper_id, lfsr_tag_next(tag),
                                &id_, &tag, &w_, &data);
                        if (err && err != LFS_ERR_NOENT) {
                            return err;
                        }
                        if (err == LFS_ERR_NOENT || id_ != upper_id) {
                            break;
                        }

                        // keep track of weight to iterate backwards
                        w += w_;

                        // determine the upper-bound of alt pointers, tags,
                        // and data after compaction (same as above)
                        dsize += LFSR_TAG_DSIZE + lfsr_data_size(data)
                                + 2*LFSR_TAG_DSIZE + 4;
                    }

                    // steal dsize from lower_dsize if we start overlapping
                    if ((lfs_size_t)upper_id-(w-1) < lower_id) {
                        lower_id = upper_id-(w-1);
                        lower_dsize -= dsize;
                    }
                    upper_dsize += dsize;

                    // done when upper/lower dsizes are close to balanced
                    if (upper_dsize >= lower_dsize) {
                        break;
                    }

                    // iterate backwards
                    upper_id -= w;
                }

                LFS_ASSERT(lower_id < rbyd->weight);
                *split_id_ = lower_id;
            }

            return false;
        }
    }

#else
    lfs_size_t count = 0;
    lfs_size_t dsize = 0;
    lfs_size_t real_dsize = sizeof(uint32_t);

    lfs_ssize_t id = start_id;
    lfsr_tag_t tag = 0;
    while (true) {
        lfs_size_t w;
        lfsr_data_t data;
        int err = lfsr_rbyd_lookupnext(lfs, rbyd, id, lfsr_tag_next(tag),
                &id, &tag, &w, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT) {
            return true;
        }

        // Exhibit A. Why I really didn't want to estimate the rbyd threshold:

        // keep track of alt-less tag count and dsize
        //
        // this is used as a heuristic for split, so the exactness matters less,
        // but we need to be able to subtract tags from the result so we can't
        // use the estimate with alt pointers
        count += 1;
        dsize += LFSR_TAG_DSIZE + lfsr_data_size(data);
        // determine the upper-bound of our alt pointers, tag, and data
        //
        // fortunately the self-balancing nature of rybds give us a tight
        // bound on the number of alt pointers
        real_dsize
                += (2*lfs_nlog2(count+1)+1) * LFSR_TAG_DSIZE
                + LFSR_TAG_DSIZE
                + lfsr_data_size(data);

        // exceeded our compaction threshold?
        if (real_dsize > threshold) {
            // requested a split id?
            if (split_id_) {
                // TODO is this really worth it vs a simpler algorithm?
                //
                // here we ignore the cost of alt-pointers, and only use the
                // tag+data cost as a heuristic
                //
                // we assume we already found an over-estimate of the split id
                // so we only need to work backwards through the rbyd to
                // correct the over-estimate. This is a very minor optimization.
                //
                lfs_size_t lower_id = id+1;
                lfs_ssize_t upper_id = rbyd->weight-1;
                lfs_size_t lower_dsize = dsize;
                lfs_size_t upper_dsize = 0;
                while (true) {
                    lfsr_tag_t tag = 0;
                    lfs_size_t w = 0;
                    lfs_size_t dsize = 0;
                    while (true) {
                        lfs_ssize_t id_;
                        lfs_size_t w_;
                        lfsr_data_t data;
                        int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                                upper_id, lfsr_tag_next(tag),
                                &id_, &tag, &w_, &data);
                        if (err && err != LFS_ERR_NOENT) {
                            return err;
                        }
                        if (err == LFS_ERR_NOENT || id_ != upper_id) {
                            break;
                        }

                        // keep track of weight to iterate backwards
                        w += w_;

                        // assume worst-case encoding size
                        dsize += LFSR_TAG_DSIZE + lfsr_data_size(data);
                    }

                    // steal dsize from lower_dsize if we start overlapping
                    if ((lfs_size_t)upper_id-(w-1) < lower_id) {
                        lower_id = upper_id-(w-1);
                        lower_dsize -= dsize;
                    }
                    upper_dsize += dsize;

                    // done when upper/lower dsizes are close to balanced
                    if (upper_dsize >= lower_dsize) {
                        break;
                    }

                    // iterate backwards
                    upper_id -= w;
                }

                LFS_ASSERT(lower_id < rbyd->weight);
                *split_id_ = lower_id;
            }

            return false;
        }
    }
#endif
}

// determine if there are fewer than "cutoff" unique ids in the rbyd,
// this is used to determine if the underlying rbyd is degenerate and can
// be reverted to an inlined btree
//
// note cutoff is expected to be quite small, <= 2, so we should make sure
// to exit our traverse early
static int lfsr_rbyd_isdegenerate(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfs_ssize_t cutoff) {
    // cutoff=-1 => no cutoff
    if (cutoff < 0) {
        return false;
    }

    // count ids until we exceed our cutoff
    lfs_ssize_t id = -1;
    lfs_size_t count = 0;
    while (true) {
        int err = lfsr_rbyd_lookupnext(lfs, rbyd, id+1, 0,
                &id, NULL, NULL, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT) {
            return true;
        }

        count += 1;
        if (count > (lfs_size_t)cutoff) {
            return false;
        }
    }
}


// some low-level dname things
//
// dnames in littlefs are tuples of directory-ids + ascii/utf8 strings


// binary search an rbyd for a name, leaving the id_/weight_ with the best
// matching name if not found
static int lfsr_rbyd_dnamelookup(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
        lfs_size_t did, const char *name, lfs_size_t name_size,
        lfs_ssize_t *id_, lfsr_tag_t *tag_, lfs_size_t *weight_,
        lfsr_data_t *data_) {
    // if we have an empty mdir, default to id = -1
    if (id_) {
        *id_ = -1;
    }
    if (tag_) {
        *tag_ = 0;
    }
    if (weight_) {
        *weight_ = 0;
    }
    if (data_) {
        *data_ = LFSR_DATA_NULL;
    }

    // binary search for our name
    lfs_ssize_t lower = 0;
    lfs_ssize_t upper = rbyd->weight;
    while (lower < upper) {
        lfsr_tag_t tag__;
        lfs_ssize_t id__;
        lfs_size_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                // lookup ~middle id, note we may end up in the middle
                // of a weighted id with this
                lower + (upper-1-lower)/2, 0,
                &id__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // if we have no name or a vestigial name, treat this id as always lt
        lfs_scmp_t cmp;
        if ((tag__ == LFSR_TAG_BRANCH && id__-(weight__-1) == 0)
                || lfsr_tag_suptype(tag__) != LFSR_TAG_NAME) {
            cmp = LFS_CMP_LT;

        // compare names
        } else {
            cmp = lfsr_data_dnamecmp(lfs, data__, 0, did, name, name_size);
            if (cmp < 0) {
                return cmp;
            }
        }

        // bisect search space
        if (lfs_cmp(cmp) > 0) {
            upper = id__-weight__ + 1;

        } else if (lfs_cmp(cmp) < 0) {
            lower = id__ + 1;

            // keep track of best-matching id >= our target
            if (id_) {
                *id_ = id__;
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
            if (id_) {
                *id_ = id__;
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

    // no match, at least update id_/tag_/weight_/data_ with the best
    // match so far
    return LFS_ERR_NOENT;
}



/// Rbyd b-tree operations ///

// convenience operations

// TODO need null btrees?
#define LFSR_BTREE_NULL ((lfsr_btree_t){.weight=0x80000000})

static inline bool lfsr_btree_isinlined(const lfsr_btree_t *btree) {
    return btree->weight & 0x80000000;
}

static inline bool lfsr_btree_isnull(const lfsr_btree_t *btree) {
    return btree->weight == 0x80000000;
}

static inline lfs_size_t lfsr_btree_weight(const lfsr_btree_t *btree) {
    return btree->weight & 0x7fffffff;
}

static inline lfs_size_t lfsr_btree_setinlined(lfs_size_t weight) {
    return weight | 0x80000000;
}


// bptr on-disk encoding

// 1 crc32c + 3 leb128 => 19 bytes (worst case)
#define LFSR_BPTR_DSIZE (4+5+5+5)

static lfs_ssize_t lfsr_bptr_todisk(lfs_t *lfs, const lfsr_rbyd_t *bptr,
        uint8_t buffer[static LFSR_BPTR_DSIZE]) {
    (void)lfs;
    lfs_ssize_t d = 0;

    lfs_tole32_(bptr->crc, &buffer[d]);
    d += 4;

    lfs_ssize_t d_ = lfs_toleb128(bptr->weight, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfs_toleb128(bptr->trunk, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfs_toleb128(bptr->block, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    return d;
}

static lfs_ssize_t lfsr_bptr_fromdisk(lfs_t *lfs, lfsr_rbyd_t *bptr,
        lfsr_data_t data) {
    // setting off to 0 here will trigger asserts if we try to append
    // without fetching first
    bptr->off = 0;

    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfsr_data_readle32(lfs, data, d, &bptr->crc);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfsr_data_readleb128(lfs, data, d, &bptr->weight);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfsr_data_readleb128(lfs, data, d, &bptr->trunk);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    d_ = lfsr_data_readleb128(lfs, data, d, &bptr->block);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    return d;
}

// btree on-disk encoding
//
// note we leave disambiguating inlined/non-inlined btrees up to the caller
#define LFSR_BTREE_DSIZE ( \
        LFSR_BPTR_DSIZE > LFSR_BTREE_INLINESIZE \
            ? LFSR_BPTR_DSIZE \
            : LFSR_BTREE_INLINESIZE)

static lfs_ssize_t lfsr_btree_todisk(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_tag_t btree_tag,
        lfsr_tag_t *tag_, uint8_t buffer[static LFSR_BTREE_DSIZE]) {
    // emit null btrees as an rm to remove anything on-disk
    if (lfsr_btree_isnull(btree)) {
        *tag_ = LFSR_TAG_RMWIDESTRUCT;
        return 0;
    // inlined?
    } else if (lfsr_btree_isinlined(btree)) {
        *tag_ = lfsr_tag_setwide(btree->inlined.tag);
        memcpy(buffer, btree->inlined.buffer, btree->inlined.size);
        return btree->inlined.size;
    // not inlined
    } else {
        *tag_ = lfsr_tag_setwide(btree_tag);
        return lfsr_bptr_todisk(lfs, &btree->root, buffer);
    }
}

// TODO wait we actually need to store the weight on-disk for btrees
static lfs_ssize_t lfsr_btree_fromdisk(lfs_t *lfs, lfsr_btree_t *btree,
        lfsr_tag_t btree_tag,
        lfsr_tag_t tag, lfs_size_t weight, lfsr_data_t data) {
    // inlined?
    if (tag != btree_tag) {
        btree->weight = lfsr_btree_setinlined(weight);
        btree->inlined.tag = tag;
        lfs_ssize_t size = lfsr_data_read(lfs, data, 0,
                btree->inlined.buffer, LFSR_BTREE_INLINESIZE);
        if (size < 0) {
            return size;
        }
        btree->inlined.size = size;
        return size;

    // not inlined
    } else {
        return lfsr_bptr_fromdisk(lfs, &btree->root, data);
    }
}


// B-tree operations

static int lfsr_btree_lookupnext_(lfs_t *lfs,
        const lfsr_btree_t *btree, lfs_size_t bid,
        lfs_size_t *bid_, lfsr_rbyd_t *rbyd_, lfs_ssize_t *rid_,
        lfsr_tag_t *tag_, lfs_size_t *weight_, lfsr_data_t *data_) {
    // in range?
    if (bid >= lfsr_btree_weight(btree)) {
        return LFS_ERR_NOENT;
    }

    // inlined?
    if (lfsr_btree_isinlined(btree)) {
        // TODO how many of these should be conditional?
        if (bid_) {
            *bid_ = lfsr_btree_weight(btree)-1;
        }
        if (tag_) {
            *tag_ = btree->inlined.tag;
        }
        if (weight_) {
            *weight_ = lfsr_btree_weight(btree);
        }
        if (data_) {
            *data_ = LFSR_DATA_BUF(btree->inlined.buffer, btree->inlined.size);
        }
        return 0;
    }

    // descend down the btree looking for our bid
    lfsr_rbyd_t branch = btree->root;
    lfs_ssize_t rid = bid;
    while (true) {
        // each branch is a pair of optional name + on-disk structure
        lfs_ssize_t rid__;
        lfsr_tag_t tag__;
        // TODO do we really need to fetch weight__ if we get it in our
        // btree struct?
        // TODO maybe only when validating?
        lfs_size_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, &branch, rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_lookup(lfs, &branch, rid__, LFSR_TAG_WIDESTRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFSR_TAG_BTREE) {
            // adjust rid with subtree's weight
            rid -= (rid__ - (weight__-1));

            // fetch the next branch
            lfs_ssize_t d = lfsr_bptr_fromdisk(lfs, &branch, data__);
            if (d < 0) {
                return d;
            }
            LFS_ASSERT(branch.weight == weight__);

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

static int lfsr_btree_lookupnext(lfs_t *lfs,
        const lfsr_btree_t *btree, lfs_size_t bid,
        lfs_size_t *bid_, lfsr_tag_t *tag_, lfs_size_t *weight_,
        lfsr_data_t *data_) {
    return lfsr_btree_lookupnext_(lfs, btree, bid,
            bid_, NULL, NULL, tag_, weight_, data_);
}

static int lfsr_btree_lookup(lfs_t *lfs,
        const lfsr_btree_t *btree, lfs_size_t bid,
        lfsr_tag_t *tag_, lfs_size_t *weight_, lfsr_data_t *data_) {
    lfs_size_t bid_;
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

// TODO still need this?
// TODO move this into the tests?
static int lfsr_btree_get(lfs_t *lfs,
        const lfsr_btree_t *btree, lfs_size_t bid,
        lfsr_tag_t *tag_, lfs_size_t *weight_,
        void *buffer, lfs_size_t size) {
    lfsr_data_t data;
    int err = lfsr_btree_lookup(lfs, btree, bid,
            tag_, weight_, &data);
    if (err) {
        return err;
    }

    return lfsr_data_read(lfs, data, 0, buffer, size);
}

// TODO should lfsr_btree_lookupnext/lfsr_btree_parent be deduplicated?
static int lfsr_btree_parent(lfs_t *lfs,
        const lfsr_btree_t *btree, lfs_size_t bid, const lfsr_rbyd_t *child,
        lfsr_rbyd_t *rbyd_, lfs_ssize_t *rid_) {
    // inlined? root?
    if (bid >= lfsr_btree_weight(btree)
            || lfsr_btree_isinlined(btree)
            || (btree->root.block == child->block
                && btree->root.trunk == child->trunk)) {
        return LFS_ERR_NOENT;
    }

    // descend down the btree looking for our id
    lfsr_rbyd_t branch = btree->root;
    lfs_ssize_t rid = bid;
    while (true) {
        // each branch is a pair of optional name + on-disk structure
        lfs_ssize_t rid__;
        lfsr_tag_t tag__;
        // TODO do we really need to fetch weight__ if we get it in our
        // btree struct?
        // TODO maybe only when validating?
        lfs_size_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, &branch, rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_lookup(lfs, &branch, rid__, LFSR_TAG_WIDESTRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // didn't find our child?
        if (tag__ != LFSR_TAG_BTREE) {
            return LFS_ERR_NOENT;
        }

        // adjust rid with subtree's weight
        rid -= (rid__ - (weight__-1));

        // fetch the next branch
        lfsr_rbyd_t branch_;
        lfs_ssize_t d = lfsr_bptr_fromdisk(lfs, &branch_, data__);
        if (d < 0) {
            return d;
        }
        LFS_ASSERT(branch_.weight == weight__);

        // found our child?
        if (branch_.block == child->block && branch_.trunk == child->trunk) {
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


// we need some scratch space for tail-recursive attr in lfsr_btree_commit
//
// note this is a mix of attributes and their payloads
// note also we need this to be a const expression since it's used in
// array allocations
#define LFSR_BTREE_SCRATCHATTRS ( \
    4 \
    + ((2*LFSR_BPTR_DSIZE) + sizeof(lfsr_attr_t)-1) \
        / sizeof(lfsr_attr_t))

// this macro creates an attr list with enough reserved space for
// btree commit operations, it's ugly but likely any implementation
// of this will look ugly since we can't use things like lfs_min32 in
// an array declaration
#define LFSR_BTREE_ATTRS(...) \
    (lfsr_attr_t[ \
        sizeof((lfsr_attr_t[]){__VA_ARGS__}) / sizeof(lfsr_attr_t) \
            > LFSR_BTREE_SCRATCHATTRS \
            ? sizeof((lfsr_attr_t[]){__VA_ARGS__}) / sizeof(lfsr_attr_t) \
            : LFSR_BTREE_SCRATCHATTRS \
    ]){__VA_ARGS__}, \
    sizeof((lfsr_attr_t[]){__VA_ARGS__}) / sizeof(lfsr_attr_t)

// core btree algorithm
static int lfsr_btree_commit(lfs_t *lfs,
        lfsr_btree_t *btree, lfs_size_t bid, lfs_ssize_t cutoff,
        lfsr_rbyd_t *rbyd,
        lfsr_attr_t attrs[static LFSR_BTREE_SCRATCHATTRS],
        lfs_size_t attr_count) {
    // other layers should check for inlined btrees before this
    LFS_ASSERT(!lfsr_btree_isinlined(btree));

    while (true) {
        // we will always need our parent, so go ahead and find it
        lfsr_rbyd_t parent;
        lfs_ssize_t pid;
        int err = lfsr_btree_parent(lfs, btree, bid, rbyd, &parent, &pid);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT) {
            // mark pid as -1 if we have no parent
            pid = -1;
        }
        lfs_size_t pweight = rbyd->weight;

        // fetch our rbyd so we can mutate it
        //
        // note that some paths lead this to being a newly allocated rbyd, these
        // will fail to fetch so we need to check that this rbyd is unfetched
        //
        // a strange benefit is we cache the root of our btree this way
        if (!lfsr_rbyd_isfetched(rbyd)) {
            err = lfsr_rbyd_fetch(lfs, rbyd, rbyd->block, rbyd->trunk);
            if (err) {
                return err;
            }
        }

        // is rbyd erased? can we sneak our commit into any remaining
        // erased bytes? note that the btree limit prevents this from mutating
        // other references to the rbyd
        err = lfsr_rbyd_commit(lfs, rbyd, attrs, attr_count);
        if (err && err != LFS_ERR_RANGE) {
            // TODO wait should we also move if there is corruption here?
            return err;
        }
        if (err) {
            goto compact;
        }

        // done?
        if (pid == -1) {
            break;
        }

        // cannibalize some attributes in our attr list to store
        // our branch
        uint8_t *scratch_buf = (uint8_t*)&attrs[2];
        lfs_ssize_t d = lfsr_bptr_todisk(lfs, rbyd, scratch_buf);
        if (d < 0) {
            return d;
        }

        // prepare commit to parent, tail recursing upwards
        //
        // note that since we defer merges to compaction time, we can
        // end up removing an rbyd here
        if (rbyd->weight == 0) {
            attrs[0] = LFSR_ATTR(pid, UNR, +rbyd->weight-pweight,
                    scratch_buf, d);
            attr_count = 1;
        } else {
            attrs[0] = LFSR_ATTR(pid, GROW, +rbyd->weight-pweight, NULL, 0);
            attrs[1] = LFSR_ATTR(pid+rbyd->weight-pweight, BTREE, 0,
                    scratch_buf, d);
            attr_count = 2;
        }

        *rbyd = parent;
        cutoff = -1;
        continue;

    compact:;
        // can't commit, try to compact
        lfsr_rbyd_t rbyd_;
        lfs_size_t split_id;

        // first check if we are a degenerate root and can be reverted to
        // an inlined btree
        //
        // This gets a bit weird since we're defering our pending
        // attributes to after the compaction. When we can/can't be inlined
        // depends on those attributes, but trying to evaluate attributes
        // is complicated and expensive.
        //
        // Instead we just let the upper layers indicate a cutoff for when
        // an rbyd can be inlined, and leave the inlining work up to the
        // upper layers.
        if (pid == -1) {
            int degenerate = lfsr_rbyd_isdegenerate(lfs, rbyd, cutoff);
            if (degenerate) {
                return degenerate;
            }
        }

        // check if we're within our compaction threshold, otherwise we
        // need to split
        int fits = lfsr_rbyd_estimate(lfs, rbyd, -1,
                lfs->cfg->block_size/2,
                &split_id);
        if (fits < 0) {
            return fits;
        }

        if (!fits) {
            // need to split
            goto split;
        }

        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // try to compact
        err = lfsr_rbyd_compact(lfs, &rbyd_, 0, -1, rbyd);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // append any pending attrs, it's up to upper
        // layers to make sure these always fit
        err = lfsr_rbyd_appendall(lfs, &rbyd_, 0, -1,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // TODO do we really need a threshold for this? should we just
        // always try since this only happens on compaction and our merges
        // are defered?
        // TODO should we allow merging both siblings?
        // TODO we should have a benchmark for how removes affect tree size
        // is our compacted size too small? try to merge with one of
        // our siblings
        if (rbyd_.off < lfs->cfg->block_size/4) {
            goto merge;
        merge_abort:;
        }

        // finalize commit
        err = lfsr_rbyd_commit(lfs, &rbyd_, NULL, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        *rbyd = rbyd_;

        // done?
        if (pid == -1) {
            break;
        }

        // cannibalize some attributes in our attr list to store
        // our branch
        scratch_buf = (uint8_t*)&attrs[2];
        d = lfsr_bptr_todisk(lfs, rbyd, scratch_buf);
        if (d < 0) {
            return d;
        }

        // prepare commit to parent, tail recursing upwards
        //
        // note that since we defer merges to compaction time, we can
        // end up removing an rbyd here
        if (rbyd->weight == 0) {
            attrs[0] = LFSR_ATTR(pid, UNR, +rbyd->weight-pweight,
                    scratch_buf, d);
            attr_count = 1;
        } else {
            attrs[0] = LFSR_ATTR(pid, GROW, +rbyd->weight-pweight, NULL, 0);
            attrs[1] = LFSR_ATTR(pid+rbyd->weight-pweight, BTREE, 0,
                    scratch_buf, d);
            attr_count = 2;
        }

        *rbyd = parent;
        cutoff = -1;
        continue;

    split:;
        // we should have something to split here
        LFS_ASSERT(split_id > 0 && split_id < rbyd->weight);

        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // allocate a sibling
        lfsr_rbyd_t sibling;
        err = lfsr_rbyd_alloc(lfs, &sibling);
        if (err) {
            return err;
        }

        // copy over tags < split_id
        err = lfsr_rbyd_compact(lfs, &rbyd_, 0, split_id, rbyd);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // append pending attrs < split_id
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendall(lfs, &rbyd_, 0, split_id,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // finalize commit
        err = lfsr_rbyd_commit(lfs, &rbyd_, NULL, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }
        
        // copy over tags >= split_id
        err = lfsr_rbyd_compact(lfs, &sibling, split_id, -1, rbyd);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // append pending attrs >= split_id
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendall(lfs, &sibling, split_id, -1,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }
        

        // finalize commit
        err = lfsr_rbyd_commit(lfs, &sibling, NULL, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending attrs in case
        // they introduce a new name!
        lfsr_tag_t stag;
        lfsr_data_t sdata;
        err = lfsr_rbyd_lookupnext(lfs, &sibling, 0, LFSR_TAG_NAME,
                NULL, &stag, NULL, &sdata);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // cannibalize some attributes in our attr list to store
        // our branches
        uint8_t *scratch_buf1 = (uint8_t*)&attrs[4];
        uint8_t *scratch_buf2 = (uint8_t*)&attrs[4] + LFSR_BPTR_DSIZE;
        lfs_ssize_t d1 = lfsr_bptr_todisk(lfs, &rbyd_, scratch_buf1);
        if (d1 < 0) {
            return d1;
        }
        lfs_ssize_t d2 = lfsr_bptr_todisk(lfs, &sibling, scratch_buf2);
        if (d2 < 0) {
            return d2;
        }

        // no parent? introduce a new trunk
        if (pid == -1) {
            int err = lfsr_rbyd_alloc(lfs, &parent);
            if (err) {
                return err;
            }
        
            // prepare commit to parent, tail recursing upwards
            attrs[0] = LFSR_ATTR(0, BTREE, +rbyd_.weight,
                    scratch_buf1, d1);
            attrs[1] = (lfsr_tag_suptype(stag) == LFSR_TAG_NAME
                    ? LFSR_ATTR_DATA(rbyd_.weight, BRANCH, +sibling.weight,
                        sdata)
                    : LFSR_ATTR_NOOP);
            attrs[2] = (lfsr_tag_suptype(stag) == LFSR_TAG_NAME
                    ? LFSR_ATTR(0+rbyd_.weight+sibling.weight-1, BTREE, 0,
                        scratch_buf2, d2)
                    : LFSR_ATTR(0+rbyd_.weight, BTREE, +sibling.weight,
                        scratch_buf2, d2));
            attr_count = 3;

        // yes parent? push up split
        } else {
            // prepare commit to parent, tail recursing upwards
            attrs[0] = LFSR_ATTR(pid, GROW, +rbyd_.weight-pweight, NULL, 0);
            attrs[1] = LFSR_ATTR(pid-(pweight-1)+rbyd_.weight-1, BTREE, 0,
                    scratch_buf1, d1);
            attrs[2] = (lfsr_tag_suptype(stag) == LFSR_TAG_NAME
                    ? LFSR_ATTR_DATA(pid-(pweight-1)+rbyd_.weight,
                        BRANCH, +sibling.weight,
                        sdata)
                    : LFSR_ATTR_NOOP);
            attrs[3] = (lfsr_tag_suptype(stag) == LFSR_TAG_NAME
                    ? LFSR_ATTR(pid-(pweight-1)+rbyd_.weight+sibling.weight-1,
                        BTREE, 0,
                        scratch_buf2, d2)
                    : LFSR_ATTR(pid-(pweight-1)+rbyd_.weight,
                        BTREE, +sibling.weight,
                        scratch_buf2, d2));
            attr_count = 4;
        }

        *rbyd = parent;
        cutoff = -1;
        continue;

    merge:;
        // no parent? can't merge
        if (pid == -1) {
            goto merge_abort;
        }

        // only child? can't merge
        if (pweight == parent.weight) {
            goto merge_abort;
        }

        lfs_ssize_t sid;
        lfs_ssize_t sdelta;
        lfs_size_t sweight;
        for (int i = 0;; i++) {
            if (i >= 2) {
                // no siblings can be merged
                goto merge_abort;
            }

            // try the right sibling
            if (i == 0) {
                // right-most child? can't merge
                if ((lfs_size_t)pid == parent.weight-1) {
                    continue;
                }

                sid = pid+1;
                sdelta = rbyd_.weight;

            // try the left sibling
            } else {
                // left-most child? can't merge
                if ((lfs_size_t)pid-(pweight-1) == 0) {
                    continue;
                }

                sid = pid-pweight;
                sdelta = 0;
            } 

            // try looking up the sibling
            // TODO do we really need to fetch sweight if we get it in our
            // btree struct?
            err = lfsr_rbyd_lookupnext(lfs, &parent, sid, LFSR_TAG_NAME,
                    &sid, &stag, &sweight, &sdata);
            if (err) {
                // no sibling? can't merge
                if (err == LFS_ERR_NOENT) {
                    continue;
                }
                return err;
            }

            if (stag == LFSR_TAG_NAME) {
                err = lfsr_rbyd_lookup(lfs, &parent,
                        sid, LFSR_TAG_WIDESTRUCT,
                        &stag, &sdata);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }
            }

            // no sibling? can't merge
            if (stag != LFSR_TAG_BTREE) {
                continue;
            }

            d = lfsr_bptr_fromdisk(lfs, &sibling, sdata);
            if (d < 0) {
                return d;
            }
            LFS_ASSERT(sibling.weight == sweight);

            // estimate if our sibling will fit
            //
            // this is imprecise when not compacting, so we may still fail to
            // merge, but this at least lets us avoid wasting programming cycles
            // when merge failure is obvious
            int fits = lfsr_rbyd_estimate(lfs, &sibling, -1,
                    lfs->cfg->block_size/4,
                    NULL);
            if (fits < 0) {
                return fits;
            }

            // don't fit? can't merge
            if (!fits) {
                continue;
            }

            // found a sibling
            break;
        }

        // try to add our sibling's tags to our rbyd
        lfs_size_t rweight_ = rbyd_.weight;
        lfs_ssize_t id = 0;
        lfsr_tag_t tag = 0;
        while (true) {
            lfs_size_t w;
            lfsr_data_t data;
            err = lfsr_rbyd_lookupnext(lfs, &sibling, id, lfsr_tag_next(tag),
                    &id, &tag, &w, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }
            if (err == LFS_ERR_NOENT) {
                break;
            }

            // append the attr
            err = lfsr_rbyd_append(lfs, &rbyd_,
                    sdelta+id-lfs_smax32(w-1, 0), tag, +w,
                    data);
            if (err) {
                return err;
            }

            // if we exceed our compaction threshold our merge has failed,
            // clean up ids and return to merge_abort
            if (rbyd_.off > lfs->cfg->block_size/2) {
                err = lfsr_rbyd_append(lfs, &rbyd_,
                        sdelta+(rbyd_.weight-rweight_)-1,
                        LFSR_TAG_UNR, -(rbyd_.weight-rweight_),
                        LFSR_DATA_NULL);
                if (err) {
                    return err;
                }

                goto merge_abort;
            }
        }

        if (sweight > 0 && rweight_ > 0) {
            // bring in name that previously split the siblings
            lfsr_tag_t split_tag;
            lfsr_data_t split_data;
            err = lfsr_rbyd_lookupnext(lfs, &parent,
                    (sdelta == 0 ? pid : sid), LFSR_TAG_NAME,
                    NULL, &split_tag, NULL, &split_data);
            if (err) {
                return err;
            }

            if (lfsr_tag_suptype(split_tag) == LFSR_TAG_NAME) {
                // lookup the id (weight really) of the previously-split entry
                lfs_ssize_t split_id;
                err = lfsr_rbyd_lookupnext(lfs, &rbyd_,
                        (sdelta == 0 ? sweight : rweight_), LFSR_TAG_NAME,
                        &split_id, NULL, NULL, NULL);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                err = lfsr_rbyd_append(lfs, &rbyd_,
                        split_id, LFSR_TAG_BRANCH, 0, split_data);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_RANGE);
                    return err;
                }
            }
        }

        // finalize the commit
        err = lfsr_rbyd_commit(lfs, &rbyd_, NULL, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // we must have a parent at this point, but is our parent degenerate?
        LFS_ASSERT(pid != -1);
        if (pweight+sweight == lfsr_btree_weight(btree)) {
            // collapse our parent, decreasing the height of the tree
            *rbyd = rbyd_;
            break;

        } else {
            // make pid the lower child so the following math is easier
            if (pid > sid) {
                lfs_sswap32(&pid, &sid);
                lfs_swap32(&pweight, &sweight);
            }

            // cannibalize some attributes in our attr list to store
            // our branch
            uint8_t *scratch_buf = (uint8_t*)&attrs[3];
            lfs_ssize_t d = lfsr_bptr_todisk(lfs, &rbyd_, scratch_buf);
            if (d < 0) {
                return d;
            }

            // prepare commit to parent, tail recursing upwards
            attrs[0] = LFSR_ATTR(sid, UNR, -sweight, NULL, 0);
            attrs[1] = LFSR_ATTR(pid, GROW, +rbyd_.weight-pweight, NULL, 0);
            attrs[2] = LFSR_ATTR(pid+rbyd_.weight-pweight, BTREE, 0,
                    scratch_buf, d);
            attr_count = 3;
        }

        *rbyd = parent;
        cutoff = -1;
        continue;
    }

    // at this point rbyd should be the trunk of our tree
    btree->root = *rbyd;
    return false;
}

static int lfsr_btree_push(lfs_t *lfs, lfsr_btree_t *btree,
        lfs_size_t bid, lfsr_tag_t tag, lfs_size_t weight, lfsr_data_t data) {
    LFS_ASSERT(bid <= lfsr_btree_weight(btree));

    // null btree?
    if (lfsr_btree_isinlined(btree) && lfsr_btree_weight(btree) == 0) {
        LFS_ASSERT(bid == 0);

        btree->weight = lfsr_btree_setinlined(weight);
        btree->inlined.tag = tag;

        lfs_ssize_t d = lfsr_data_read(lfs, data, 0,
                btree->inlined.buffer, LFSR_BTREE_INLINESIZE);
        if (d < 0) {
            return d;
        }
        LFS_ASSERT(d <= LFSR_BTREE_INLINESIZE);
        btree->inlined.size = d;
        return 0;

    // inlined btree, need to expand into an rbyd
    } else if (lfsr_btree_isinlined(btree)) {
        lfsr_rbyd_t rbyd;
        int err = lfsr_rbyd_alloc(lfs, &rbyd);
        if (err) {
            return err;
        }

        // commit our entries
        err = lfsr_rbyd_commit(lfs, &rbyd, LFSR_ATTRS(
                LFSR_ATTR_(0, btree->inlined.tag, +lfsr_btree_weight(btree),
                    btree->inlined.buffer, btree->inlined.size),
                LFSR_ATTR_DATA_(bid, tag, +weight, data)));
        if (err) {
            return err;
        }

        btree->root = rbyd;
        return 0;

    // a normal btree
    } else {
        // lookup in which leaf our id resides
        //
        // for lfsr_btree_commit operations to work out, we need to
        // limit our bid to an id in the tree, which is what this min
        // is doing
        //
        // note it is possible for our btree to have a weight of zero here,
        // since we defer inlining until compaction time
        lfs_size_t bid_ = lfs_min32(bid,
                lfs_smax32(lfsr_btree_weight(btree)-1, 0));
        lfsr_rbyd_t rbyd = btree->root;
        lfs_ssize_t rid = -1;
        lfs_size_t rweight = 0;
        int err = lfsr_btree_lookupnext_(lfs, btree, bid_,
                NULL, &rbyd, &rid, NULL, &rweight, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // adjust rid for push
        if (bid >= lfsr_btree_weight(btree)) {
            rid += 1;
        } else {
            rid -= rweight-1;
        }

        // commit our id into the tree, letting lfsr_btree_commit take care
        // of the rest
        int degenerate = lfsr_btree_commit(lfs, btree, bid_, 0, &rbyd,
                LFSR_BTREE_ATTRS(
                    LFSR_ATTR_DATA_(rid, tag, +weight, data)));
        if (degenerate < 0) {
            return degenerate;
        }

        // revert to an inlined btree
        if (degenerate) {
            btree->weight = lfsr_btree_setinlined(weight);
            btree->inlined.tag = tag;

            lfs_ssize_t d = lfsr_data_read(lfs, data, 0,
                    btree->inlined.buffer, LFSR_BTREE_INLINESIZE);
            if (d < 0) {
                return d;
            }
            LFS_ASSERT(d <= LFSR_BTREE_INLINESIZE);
            btree->inlined.size = d;
        }

        return 0;
    }
}

static int lfsr_btree_set(lfs_t *lfs, lfsr_btree_t *btree,
        lfs_size_t bid, lfsr_tag_t tag, lfs_size_t weight, lfsr_data_t data) {
    LFS_ASSERT(bid < lfsr_btree_weight(btree));
    LFS_ASSERT(lfsr_btree_weight(btree) > 0);

    // inlined btree?
    if (lfsr_btree_isinlined(btree)) {
        LFS_ASSERT(bid == lfsr_btree_weight(btree)-1);

        btree->weight = lfsr_btree_setinlined(weight);
        btree->inlined.tag = tag;

        lfs_ssize_t d = lfsr_data_read(lfs, data, 0,
                btree->inlined.buffer, LFSR_BTREE_INLINESIZE);
        if (d < 0) {
            return d;
        }
        LFS_ASSERT(d <= LFSR_BTREE_INLINESIZE);
        btree->inlined.size = d;
        return 0;

    // a normal btree
    } else {
        // lookup in which leaf our id resides
        lfsr_rbyd_t rbyd;
        lfsr_tag_t rtag;
        lfs_ssize_t rid;
        lfs_size_t rweight;
        int err = lfsr_btree_lookupnext_(lfs, btree, bid,
                NULL, &rbyd, &rid, &rtag, &rweight, NULL);
        if (err) {
            return err;
        }

        // commit our id into the tree, letting lfsr_btree_commit take care
        // of the rest
        int degenerate = lfsr_btree_commit(lfs, btree, bid, 1, &rbyd,
                LFSR_BTREE_ATTRS(
                    (tag != rtag
                        ? LFSR_ATTR_(rid, lfsr_tag_setrm(rtag), 0, NULL, 0)
                        : LFSR_ATTR_NOOP),
                    LFSR_ATTR_DATA_(rid, tag, 0, data),
                    LFSR_ATTR(rid, GROW, +weight-rweight, NULL, 0)));
        if (degenerate < 0) {
            return degenerate;
        }

        // revert to an inlined btree
        if (degenerate) {
            btree->weight = lfsr_btree_setinlined(weight);
            btree->inlined.tag = tag;

            lfs_ssize_t d = lfsr_data_read(lfs, data, 0,
                    btree->inlined.buffer, LFSR_BTREE_INLINESIZE);
            if (d < 0) {
                return d;
            }
            LFS_ASSERT(d <= LFSR_BTREE_INLINESIZE);
            btree->inlined.size = d;
        }

        return 0;
    }
}

static int lfsr_btree_pop(lfs_t *lfs, lfsr_btree_t *btree, lfs_size_t bid) {
    LFS_ASSERT(bid < lfsr_btree_weight(btree));
    LFS_ASSERT(lfsr_btree_weight(btree) > 0);

    // inlined btree?
    if (lfsr_btree_isinlined(btree)) {
        LFS_ASSERT(bid == lfsr_btree_weight(btree)-1);
        btree->weight = lfsr_btree_setinlined(0);
        return 0;

    // a normal btree
    } else {
        // lookup in which leaf our id resides
        lfsr_rbyd_t rbyd;
        lfsr_tag_t rtag;
        lfs_ssize_t rid;
        lfs_size_t rweight;
        int err = lfsr_btree_lookupnext_(lfs, btree, bid,
                NULL, &rbyd, &rid, &rtag, &rweight, NULL);
        if (err) {
            return err;
        }

        // remove our id, letting lfsr_btree_commit take care
        // of the rest
        //
        // note we use a cutoff of 2 here, if we have 2 entries before
        // the commit, we should have 1 entry after the commit and can
        // revert to an inlined btree
        int degenerate = lfsr_btree_commit(lfs, btree, bid, 2, &rbyd,
                LFSR_BTREE_ATTRS(
                    LFSR_ATTR(rid, UNR, -rweight, NULL, 0)));
        if (degenerate < 0) {
            return degenerate;
        }

        // revert to a null btree
        if (degenerate && rweight >= rbyd.weight) {
            btree->weight = lfsr_btree_setinlined(0);

        // revert to an inlined btree
        } else if (degenerate) {
            lfs_ssize_t sid;
            // left sibling
            if ((lfs_size_t)rid == rbyd.weight-1) {
                sid = rid-rweight;
            // right sibling
            } else {
                sid = rid+1;
            }

            lfsr_tag_t stag;
            lfs_size_t sweight;
            lfsr_data_t sdata;
            int err = lfsr_rbyd_lookupnext(lfs, &rbyd, sid, LFSR_TAG_NAME,
                    &sid, &stag, &sweight, &sdata);
            if (err) {
                LFS_ASSERT(err == LFS_ERR_NOENT);
                return err;
            }

            if (lfsr_tag_suptype(stag) == LFSR_TAG_NAME) {
                err = lfsr_rbyd_lookup(lfs, &rbyd, sid, LFSR_TAG_WIDESTRUCT,
                        &stag, &sdata);
                if (err) {
                    LFS_ASSERT(err == LFS_ERR_NOENT);
                    return err;
                }
            }

            LFS_ASSERT(sweight+rweight == rbyd.weight);
            btree->weight = lfsr_btree_setinlined(sweight);
            btree->inlined.tag = stag;

            LFS_ASSERT(lfsr_data_size(sdata) <= LFSR_BTREE_INLINESIZE);
            err = lfsr_bd_read(lfs, sdata.disk.block, sdata.disk.off, 0,
                    btree->inlined.buffer, lfsr_data_size(sdata));
            if (err) {
                return err;
            }
            btree->inlined.size = lfsr_data_size(sdata);
        }

        return 0;
    }
}

// lfsr_btree_split can be done with a update+push, but this function
// does all this in one commit, which is much more efficient
//
// this is also the only btree function that creates name entries, in theory
// push could as well, we just don't need the functionality for littlefs
//
static int lfsr_btree_split(lfs_t *lfs, lfsr_btree_t *btree,
        lfs_size_t bid, lfsr_data_t name,
        lfsr_tag_t tag1, lfs_size_t weight1, lfsr_data_t data1,
        lfsr_tag_t tag2, lfs_size_t weight2, lfsr_data_t data2) {
    LFS_ASSERT(bid < lfsr_btree_weight(btree));
    LFS_ASSERT(lfsr_btree_weight(btree) > 0);

    // inlined btree, need to expand into an rbyd
    if (lfsr_btree_isinlined(btree)) {
        lfsr_rbyd_t rbyd;
        int err = lfsr_rbyd_alloc(lfs, &rbyd);
        if (err) {
            return err;
        }

        // commit our entries
        err = lfsr_rbyd_commit(lfs, &rbyd, LFSR_ATTRS(
                LFSR_ATTR_DATA_(0, tag1, +weight1, data1),
                (lfsr_data_size(name) > 0
                    ? LFSR_ATTR_DATA(weight1, BRANCH, +weight2, name)
                    : LFSR_ATTR_NOOP),
                (lfsr_data_size(name) > 0
                    ? LFSR_ATTR_DATA_(weight1+weight2-1, tag2, 0, data2)
                    : LFSR_ATTR_DATA_(weight1, tag2, +weight2, data2))));
        if (err) {
            return err;
        }

        btree->root = rbyd;
        return 0;

    // a normal btree
    } else {
        // lookup in which leaf our bid resides
        lfsr_rbyd_t rbyd;
        lfs_ssize_t rid;
        lfs_size_t rweight;
        int err = lfsr_btree_lookupnext_(lfs, btree, bid,
                NULL, &rbyd, &rid, NULL, &rweight, NULL);
        if (err) {
            return err;
        }

        // commit our bid into the tree, letting lfsr_btree_commit take care
        // of the rest
        int degenerate = lfsr_btree_commit(lfs, btree, bid, -1, &rbyd,
                LFSR_BTREE_ATTRS(
                    LFSR_ATTR(rid, GROW, +weight1-rweight, NULL, 0),
                    LFSR_ATTR_DATA_(rid-(rweight-1)+weight1-1, tag1, 0, data1),
                    (lfsr_data_size(name) > 0
                        ? LFSR_ATTR_DATA(
                            rid-(rweight-1)+weight1, BRANCH, +weight2,
                            name)
                        : LFSR_ATTR_NOOP),
                    (lfsr_data_size(name) > 0
                        ? LFSR_ATTR_DATA_(rid-(rweight-1)+weight1+weight2-1,
                            tag2, 0, data2)
                        : LFSR_ATTR_DATA_(rid-(rweight-1)+weight1,
                            tag2, +weight2, data2))));
        if (degenerate < 0) {
            return degenerate;
        }

        // this should never happen
        LFS_ASSERT(!degenerate);
        return 0;
    }
}

// lookup in a btree by dname
static int lfsr_btree_dnamelookup(lfs_t *lfs, const lfsr_btree_t *btree,
        lfs_size_t did, const char *name, lfs_size_t name_size,
        lfs_size_t *bid_, lfsr_tag_t *tag_, lfs_size_t *weight_,
        lfsr_data_t *data_) {
    // an empty tree?
    if (lfsr_btree_weight(btree) == 0) {
        return LFS_ERR_NOENT;
    }

    // inlined?
    if (lfsr_btree_isinlined(btree)) {
        // TODO how many of these should be conditional?
        if (bid_) {
            *bid_ = lfsr_btree_weight(btree)-1;
        }
        if (tag_) {
            *tag_ = btree->inlined.tag;
        }
        if (weight_) {
            *weight_ = lfsr_btree_weight(btree);
        }
        if (data_) {
            *data_ = LFSR_DATA_BUF(btree->inlined.buffer, btree->inlined.size);
        }
        return 0;
    }

    // descend down the btree looking for our name
    lfsr_rbyd_t branch = btree->root;
    lfs_ssize_t bid = 0;
    while (true) {
        // lookup our name in the rbyd via binary search
        lfs_ssize_t rid__;
        lfs_size_t weight__;
        int err = lfsr_rbyd_dnamelookup(lfs, &branch, did, name, name_size,
                &rid__, NULL, &weight__, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // the name may not match exactly, but indicates which branch to follow
        lfsr_tag_t tag__;
        lfsr_data_t data__;
        err = lfsr_rbyd_lookup(lfs, &branch, rid__, LFSR_TAG_WIDESTRUCT,
                &tag__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // found another branch
        if (tag__ == LFSR_TAG_BTREE) {
            // update our bid
            bid += rid__ - (weight__-1);

            // fetch the next branch
            lfs_ssize_t d = lfsr_bptr_fromdisk(lfs, &branch, data__);
            if (d < 0) {
                return d;
            }
            LFS_ASSERT(branch.weight == weight__);

        // found our id
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
            return 0;
        }
    }
}

// incremental btree traversal
//
// note this is different from iteration, iteration should use
// lfsr_btree_lookupnext, traversal includes inner entries
typedef struct lfsr_btree_traversal {
    lfs_size_t bid;
    lfs_ssize_t rid;
    lfsr_rbyd_t branch;
} lfsr_btree_traversal_t;

#define LFSR_BTREE_TRAVERSAL_INIT ((lfsr_btree_traversal_t){ \
        .bid = 0, \
        .rid = 0, \
        .branch.trunk = 0, \
        .branch.weight = 0, \
    })

static int lfsr_btree_traversal_next(lfs_t *lfs,
        const lfsr_btree_t *btree,
        lfsr_btree_traversal_t *traversal,
        lfs_size_t *bid_, lfsr_tag_t *tag_, lfs_size_t *weight_,
        lfsr_data_t *data_) {
    while (true) {
        // in range?
        if (traversal->bid >= lfsr_btree_weight(btree)) {
            return LFS_ERR_NOENT;
        }

        // inlined?
        if (lfsr_btree_isinlined(btree)) {
            // setup traversal to terminate next call
            traversal->bid = lfsr_btree_weight(btree);

            // TODO how many of these should be conditional?
            if (bid_) {
                *bid_ = lfsr_btree_weight(btree)-1;
            }
            if (tag_) {
                *tag_ = btree->inlined.tag;
            }
            if (weight_) {
                *weight_ = lfsr_btree_weight(btree);
            }
            if (data_) {
                *data_ = LFSR_DATA_BUF(btree->inlined.buffer,
                        btree->inlined.size);
            }
            return 0;
        }

        // restart from the root
        if ((lfs_size_t)traversal->rid >= traversal->branch.weight) {
            traversal->bid += traversal->branch.weight;
            traversal->rid = traversal->bid;
            traversal->branch = btree->root;

            if (traversal->rid == 0) {
                // TODO how many of these should be conditional?
                if (bid_) {
                    *bid_ = lfsr_btree_weight(btree)-1;
                }
                if (tag_) {
                    *tag_ = LFSR_TAG_BTREE;
                }
                if (weight_) {
                    *weight_ = lfsr_btree_weight(btree);
                }
                if (data_) {
                    // note btrees are returned decoded
                    *data_ = LFSR_DATA_BUF(&traversal->branch,
                            sizeof(lfsr_rbyd_t));
                }
                return 0;
            }

            // continue, mostly for range check
            continue;
        }

        // descend down the tree
        lfs_ssize_t rid__;
        lfsr_tag_t tag__;
        lfs_size_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, &traversal->branch,
                traversal->rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_lookup(lfs, &traversal->branch,
                    rid__, LFSR_TAG_WIDESTRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFSR_TAG_BTREE) {
            // adjust rid with subtree's weight
            traversal->rid -= (rid__ - (weight__-1));

            // fetch the next branch
            lfs_ssize_t d = lfsr_bptr_fromdisk(lfs,
                    &traversal->branch, data__);
            if (d < 0) {
                return d;
            }
            LFS_ASSERT(traversal->branch.weight == weight__);

            // return inner btree nodes if this is the first time we've
            // seen them
            if (traversal->rid == 0) {
                // TODO how many of these should be conditional?
                if (bid_) {
                    *bid_ = traversal->bid + (rid__ - traversal->rid);
                }
                if (tag_) {
                    *tag_ = LFSR_TAG_BTREE;
                }
                if (weight_) {
                    *weight_ = traversal->branch.weight;
                }
                if (data_) {
                    // note btrees are returned decoded
                    *data_ = LFSR_DATA_BUF(
                            &traversal->branch, sizeof(lfsr_rbyd_t));
                }
                return 0;
            }

        // found our bid
        } else {
            // move on to the next rid
            //
            // note the effectively traverses a full leaf without redoing
            // the btree walk
            lfs_ssize_t bid__ = traversal->bid + (rid__ - traversal->rid);
            traversal->rid = rid__ + 1;

            // TODO how many of these should be conditional?
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



/// Metadata pair operations ///

// special mid values
enum {
    LFSR_MID_MROOTANCHOR = -4,
    LFSR_MID_WL          = -3,
    LFSR_MID_RM          = -2,
    LFSR_MID_MROOT       = -1,
};

// mptr things
typedef struct lfsr_mptr {
    lfs_block_t blocks[2];
} lfsr_mptr_t;

#define LFSR_MPTR(block0, block1) ((lfsr_mptr_t){.blocks={block0, block1}})

// the mroot anchor, mdir 0x{0,1} is the entry point into the filesystem
#define LFSR_MPTR_MROOTANCHOR LFSR_MPTR(0, 1)

static inline int lfsr_mptr_cmp(lfsr_mptr_t a, lfsr_mptr_t b) {
    // allow either order
    if ((a.blocks[0] == b.blocks[0] && a.blocks[1] == b.blocks[1])
            || (a.blocks[0] == b.blocks[1] && a.blocks[1] == b.blocks[0])) {
        return 0;
    } else {
        return 1;
    }
}

static inline bool lfsr_mptr_ismrootanchor(lfsr_mptr_t a) {
    return lfsr_mptr_cmp(a, LFSR_MPTR_MROOTANCHOR) == 0;
}

// 2 leb128 => 10 bytes (worst case)
#define LFSR_MPTR_DSIZE (5+5)

static lfs_ssize_t lfsr_mptr_todisk(lfs_t *lfs, lfsr_mptr_t mptr,
        uint8_t buffer[static LFSR_MPTR_DSIZE]) {
    (void)lfs;
    lfs_ssize_t d = 0;
    for (int i = 0; i < 2; i++) {
        lfs_ssize_t d_ = lfs_toleb128(mptr.blocks[i], &buffer[d], 5);
        if (d_ < 0) {
            return d_;
        }
        d += d_;
    }

    return d;
}

static lfs_ssize_t lfsr_mptr_fromdisk(lfs_t *lfs, lfsr_mptr_t *mptr,
        lfsr_data_t data) {
    lfs_ssize_t d = 0;
    for (int i = 0; i < 2; i++) {
        lfs_ssize_t d_ = lfsr_data_readleb128(lfs, data, d, &mptr->blocks[i]);
        if (d_ < 0) {
            return d_;
        }
        d += d_;
    }

    return d;
}

// mdir things
static inline lfsr_mptr_t lfsr_mdir_mptr(const lfsr_mdir_t *mdir) {
    return LFSR_MPTR(mdir->rbyd.block, mdir->redund_block);
}

static inline int lfsr_mdir_cmp(const lfsr_mdir_t *a, const lfsr_mdir_t *b) {
    return lfsr_mptr_cmp(lfsr_mdir_mptr(a), lfsr_mdir_mptr(b));
}

static inline bool lfsr_mdir_ismrootanchor(const lfsr_mdir_t *a) {
    // mrootanchor is always at 0x{0,1}
    // just check that at least one block is at 0x0
    return a->rbyd.block == 0 || a->redund_block == 0;
}

static lfs_ssize_t lfsr_mdir_todisk(lfs_t *lfs, const lfsr_mdir_t *mdir,
        uint8_t buffer[static LFSR_MPTR_DSIZE]) {
    return lfsr_mptr_todisk(lfs, lfsr_mdir_mptr(mdir), buffer);
}

static inline lfs_size_t lfsr_mdir_weight(const lfsr_mdir_t *mdir) {
    return mdir->rbyd.weight;
}

// track "opened" mdirs that may need to by updated
static void lfsr_mdir_addopened(lfs_t *lfs, lfsr_openedmdir_t *opened) {
    opened->next = lfs->opened;
    lfs->opened = opened;
}

static void lfsr_mdir_removeopened(lfs_t *lfs, lfsr_openedmdir_t *opened) {
    for (lfsr_openedmdir_t **p = &lfs->opened; *p; p = &(*p)->next) {
        if (*p == opened) {
            *p = (*p)->next;
            break;
        }
    }
}

static bool lfsr_mdir_isopened(lfs_t *lfs, const lfsr_openedmdir_t *opened) {
    for (lfsr_openedmdir_t *p = lfs->opened; p; p = p->next) {
        if (p == opened) {
            return true;
        }
    }

    return false;
}

static int lfsr_mdir_alloc(lfs_t *lfs, lfsr_mdir_t *mdir, lfs_ssize_t mid) {
    // allocate two blocks
    lfs_block_t blocks[2];
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &blocks[i]);
        if (err) {
            return err;
        }
    }

    // rather than performing an unecessary erase, treat the current contents
    // of the other block as a valid revision count
    uint32_t rev;
    int err = lfsr_bd_read(lfs, blocks[1], 0, 0, &rev, sizeof(uint32_t));
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    // note we can allow blocks that report corrupt here, with the assumption
    // that all future reads will also return corrupt, this can happen if
    // the underlying block device uses ECC that may be invalid when
    // uninitialized
    if (err == LFS_ERR_CORRUPT) {
        rev = 0;
    }

    // align revision count in new mdirs to our block_cycles, this makes sure
    // we don't immediately try to relocate the mdir
    if (lfs->cfg->block_cycles > 0) {
        rev = lfs_alignup(rev+1, lfs->cfg->block_cycles)-1;
    }

    // setup mdir struct
    mdir->mid = mid;
    mdir->redund_block = blocks[0];
    mdir->rbyd.weight = 0;
    mdir->rbyd.block = blocks[1];
    // mark mdir as needing compaction
    mdir->rbyd.off = lfs->cfg->block_size;
    mdir->rbyd.trunk = 0;
    return 0;
}

static int lfsr_mdir_fetch(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfs_ssize_t mid, lfsr_mptr_t mptr) {
    // read both revision counts, try to figure out which block
    // has the most recent revision
    uint32_t revs[2] = {0, 0};
    for (int i = 0; i < 2; i++) {
        int err = lfsr_bd_read(lfs, mptr.blocks[0], 0, 0,
                &revs[0], sizeof(uint32_t));
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        revs[i] = lfs_fromle32_(&revs[i]);

        if (i == 0
                || err == LFS_ERR_CORRUPT
                || lfs_scmp(revs[1], revs[0]) > 0) {
            lfs_swap32(&mptr.blocks[0], &mptr.blocks[1]);
            lfs_swap32(&revs[0], &revs[1]);
        }
    }

    // try to fetch rbyds in the order of most recent to least recent
    for (int i = 0; i < 2; i++) {
        int err = lfsr_rbyd_fetch(lfs, &mdir->rbyd, mptr.blocks[0], 0);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        if (!err) {
            mdir->mid = mid;
            // keep track of other block for compactions
            mdir->redund_block = mptr.blocks[1];
            return 0;
        }

        lfs_swap32(&mptr.blocks[0], &mptr.blocks[1]);
        lfs_swap32(&revs[0], &revs[1]);
    }

    // could not find a non-corrupt rbyd
    return LFS_ERR_CORRUPT;
}

static int lfsr_mdir_lookupnext(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfs_ssize_t id, lfsr_tag_t tag,
        lfs_ssize_t *id_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    return lfsr_rbyd_lookupnext(lfs, &mdir->rbyd, id, tag,
            id_, tag_, NULL, data_);
}

static int lfsr_mdir_lookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfs_ssize_t id, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    return lfsr_rbyd_lookup(lfs, &mdir->rbyd, id, tag, tag_, data_);
}

// TODO do we need this?
// TODO move this into the tests?
static lfs_ssize_t lfsr_mdir_get(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfs_ssize_t id, lfsr_tag_t tag, void *buffer, lfs_size_t size) {
    return lfsr_rbyd_get(lfs, &mdir->rbyd, id, tag, buffer, size);
}


// mtree is the core tree of mdirs in littlefs

static inline int lfsr_mtree_isinlined(lfs_t *lfs) {
    return lfsr_btree_weight(&lfs->mtree) == 0;
}

static inline lfs_ssize_t lfsr_mtree_weight(lfs_t *lfs) {
    return lfsr_btree_weight(&lfs->mtree);
}

static int lfsr_mtree_lookup(lfs_t *lfs, lfs_ssize_t mid, lfsr_mdir_t *mdir_) {
    // TODO should we really allow -1=>mroot lookup?
    LFS_ASSERT(mid >= -1);
    LFS_ASSERT(mid < lfsr_mtree_weight(lfs));

    // looking up mroot?
    if (mid < 0) {
        *mdir_ = lfs->mroot;
        return 0;

    // look up mdir in actual mtree
    } else {
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_btree_lookup(lfs, &lfs->mtree, mid,
                &tag, NULL, &data);
        if (err) {
            return err;
        }
        LFS_ASSERT(tag == LFSR_TAG_MDIR);

        // decode mptr
        lfsr_mptr_t mptr;
        lfs_ssize_t d = lfsr_mptr_fromdisk(lfs, &mptr, data);
        if (d < 0) {
            return d;
        }

        // fetch mdir
        return lfsr_mdir_fetch(lfs, mdir_, mid, mptr);
    }
}

static int lfsr_mtree_parent(lfs_t *lfs, lfsr_mptr_t mchild,
        lfsr_mdir_t *mparent_) {
    // if mptr is our initial 0x{0,1} blocks, we have no parent
    if (lfsr_mptr_ismrootanchor(mchild)) {
        return LFS_ERR_NOENT;
    }

    // scan list of mroots for our requested pair
    lfsr_mptr_t mptr = LFSR_MPTR_MROOTANCHOR;
    lfsr_mdir_t mdir;
    while (true) {
        // fetch next possible superblock
        int err = lfsr_mdir_fetch(lfs, &mdir, LFSR_MID_MROOT, mptr);
        if (err) {
            return err;
        }

        // lookup next mroot
        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, -1, LFSR_TAG_MROOT, NULL, &data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // decode mptr
        lfsr_mptr_t mptr;
        lfs_ssize_t d = lfsr_mptr_fromdisk(lfs, &mptr, data);
        if (d < 0) {
            return d;
        }

        // found our child?
        if (lfsr_mptr_cmp(mptr, mchild) == 0) {
            *mparent_ = mdir;
            return 0;
        }
    }
}

// low-level mdir compaction
static int lfsr_mdir_compact_(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfs_ssize_t mid,
        lfs_ssize_t start_id, lfs_ssize_t end_id,
        const lfsr_mdir_t *msource,
        const lfsr_attr_t *attr1s, lfs_size_t attr1_count,
        const lfsr_attr_t *attr2s, lfs_size_t attr2_count) {
    // note mid indicates some special cases:
    // - mid = mrootanchor => never alloc (mroot anchor)
    // - mid = wl          => only alloc if mdir is tired (wear-leveling)
    // - otherwise         => always alloc, use this mid (new mdir)

    // first thing we need to do is read our current revision count
    uint32_t rev;
    int err = lfsr_bd_read(lfs, msource->rbyd.block, 0, sizeof(uint32_t),
            &rev, sizeof(uint32_t));
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    rev = (err != LFS_ERR_CORRUPT ? lfs_fromle32_(&rev) : 0);

    // decide if we need to relocate
    if (mid != LFSR_MID_MROOTANCHOR && (mid != LFSR_MID_WL || (
            lfs->cfg->block_cycles > 0
                // TODO rev things
                && (rev + 1) % lfs->cfg->block_cycles == 0))) {
        // allocate a new mdir for relocation
        err = lfsr_mdir_alloc(lfs, mdir,
                (mid != LFSR_MID_WL ? mid : mdir->mid));
        if (err) {
            return err;
        }

        // read the new revision count
        //
        // we use whatever is on-disk to avoid needing to rewrite the
        // redund block
        err = lfsr_bd_read(lfs, mdir->rbyd.block, 0, sizeof(uint32_t),
                &rev, sizeof(uint32_t));
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        // note we allow corrupt errors here, as long as they are consistent
        rev = (err != LFS_ERR_CORRUPT ? lfs_fromle32_(&rev) : 0);
    }

    // swap our rbyds 
    lfs_swap32(&mdir->rbyd.block, &mdir->redund_block);
    // update our revision count
    mdir->rbyd.off = 0;
    mdir->rbyd.trunk = 0;
    mdir->rbyd.weight = 0;
    mdir->rbyd.crc = 0;

    // erase, preparing for compact
    err = lfsr_bd_erase(lfs, mdir->rbyd.block);
    if (err) {
        return err;
    }

    // increment our revision count and write it to our rbyd
    // TODO rev things
    err = lfsr_rbyd_appendrev(lfs, &mdir->rbyd, rev + 1);
    if (err) {
        return err;
    }

    // copy over attrs
    err = lfsr_rbyd_compact(lfs, &mdir->rbyd, start_id, end_id, &msource->rbyd);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // append any pending attrs
    //
    // upper layers should make sure this can't fail by limiting the
    // maximum commit size
    err = lfsr_rbyd_appendall(lfs, &mdir->rbyd, start_id, end_id,
            attr1s, attr1_count);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // note we don't filter attrs from our second pending list, this
    // is used for some auxiliary attrs in lfsr_mdir_commit
    err = lfsr_rbyd_appendall(lfs, &mdir->rbyd, -1, -1,
            attr2s, attr2_count);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // drop commit if weight goes to zero
    if (mdir->mid >= 0 && mdir->rbyd.weight == 0) {
        lfs_cache_zero(lfs, &lfs->pcache);

    // finalize commit
    } else {
        err = lfsr_rbyd_commit(lfs, &mdir->rbyd, NULL, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }
    }

    return 0;
}

// low-level mdir commit, does not handle mtree/mlist updates
static int lfsr_mdir_commit_(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfs_size_t *split_id_,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // try to append a commit
    lfsr_mdir_t mdir_ = *mdir;
    int err = lfsr_rbyd_appendall(lfs, &mdir_.rbyd, -1, -1, attrs, attr_count);
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }
    if (err == LFS_ERR_RANGE) {
        goto compact;
    }

    // drop commit if weight goes to zero
    if (mdir_.mid >= 0 && mdir_.rbyd.weight == 0) {
        lfs_cache_zero(lfs, &lfs->pcache);

    // finalize commit
    } else {
        err = lfsr_rbyd_commit(lfs, &mdir_.rbyd, NULL, 0);
        if (err && err != LFS_ERR_RANGE) {
            return err;
        }
        if (err == LFS_ERR_RANGE) {
            goto compact;
        }
    }

    // update our mdir
    *mdir = mdir_;
    return 0;

compact:;
    // can't commit, try to compact

    // check if we're within our compaction threshold
    int fits = lfsr_rbyd_estimate(lfs, &mdir->rbyd, -1,
            lfs->cfg->block_size/2,
            split_id_);
    if (fits < 0) {
        return fits;
    }

    // TODO change lfsr_rbyd_estimate so !fits => err=LFS_ERR_RANGE?
    if (!fits) {
        return LFS_ERR_RANGE;
    }

    // try to compact
    err = lfsr_mdir_compact_(lfs, &mdir_, LFSR_MID_WL, -1, -1, mdir,
            attrs, attr_count,
            NULL, 0);
    if (err) {
        return err;
    }

    // update our mdir
    *mdir = mdir_;
    return 0;
}

// low-level mdir split, note this is really an operation on the mtree
static int lfsr_mtree_split_(lfs_t *lfs, lfsr_btree_t *mtree,
        lfsr_mdir_t *mdir, lfsr_mdir_t *msibling,
        lfs_ssize_t start_id, lfs_ssize_t end_id,
        const lfsr_mdir_t *msource, lfs_size_t split_id,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // if we're the mroot, create a new mtree, assume the upper layers
    // will take care of grafting our mtree into the mroot as needed
    lfs_ssize_t mid = msource->mid;
    if (mid == LFSR_MID_MROOT) {
        mid = 0;

        // create a null entry in our btree first. don't worry! thanks
        // to inlining this doesn't allocate anything yet
        //
        // the reason for this is twofold:
        //
        // 1. it makes it so the split logic is the same whether or not
        //    we're uninlining
        // 2. it makes it so we can actually split, lfsr_btree_split
        //    currently doesn't support an empty tree
        //
        int err = lfsr_btree_push(lfs, mtree, 0, LFSR_TAG_MDIR, 1,
                LFSR_DATA_NULL);
        if (err) {
            return err;
        }
    }

    // we should have something to split here
    LFS_ASSERT(split_id > 0 && split_id < msource->rbyd.weight);

    // compact into new mdir tags < split_id
    int err = lfsr_mdir_compact_(lfs, mdir, mid, start_id, split_id,
            msource, attrs, attr_count, NULL, 0);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // compact into new mdir tags >= split_id
    err = lfsr_mdir_compact_(lfs, msibling, mid+1, split_id, end_id,
            msource, attrs, attr_count, NULL, 0);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    LFS_DEBUG("Splitting mdir 0x{%"PRIx32",%"PRIx32"} "
            "-> 0x{%"PRIx32",%"PRIx32"}"
            ", 0x{%"PRIx32",%"PRIx32"}",
            msource->rbyd.block, msource->redund_block,
            mdir->rbyd.block, mdir->redund_block,
            msibling->rbyd.block, msibling->redund_block);

    // because of defered commits, both children can still be reduced
    // to zero, need to catch this here
    if (mdir->rbyd.weight > 0 && msibling->rbyd.weight > 0) {
        // update out mtree

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending attrs in
        // case they introduce a new name!
        lfsr_tag_t stag;
        lfsr_data_t sdata;
        err = lfsr_mdir_lookupnext(lfs, msibling, 0, LFSR_TAG_NAME,
                NULL, &stag, &sdata);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        uint8_t buf1[LFSR_MPTR_DSIZE];
        lfs_ssize_t d1 = lfsr_mdir_todisk(lfs, mdir, buf1);
        if (d1 < 0) {
            return d1;
        }
        uint8_t buf2[LFSR_MPTR_DSIZE];
        lfs_ssize_t d2 = lfsr_mdir_todisk(lfs, msibling, buf2);
        if (d2 < 0) {
            return d2;
        }

        err = lfsr_btree_split(lfs, mtree, mid,
                (lfsr_tag_suptype(stag) == LFSR_TAG_NAME
                    ? sdata
                    : LFSR_DATA_NULL),
                LFSR_TAG_MDIR, 1, LFSR_DATA_BUF(buf1, d1),
                LFSR_TAG_MDIR, 1, LFSR_DATA_BUF(buf2, d2));
        if (err) {
            return err;
        }

    // one sibling reduced to zero
    } else if (mdir->rbyd.weight > 0) {
        LFS_DEBUG("Dropping mdir 0x{%"PRIx32",%"PRIx32"}",
                mdir->rbyd.block, mdir->redund_block);

        // update our mtree
        uint8_t buf[LFSR_MPTR_DSIZE];
        lfs_ssize_t d = lfsr_mdir_todisk(lfs, mdir, buf);
        if (d < 0) {
            return d;
        }

        err = lfsr_btree_set(lfs, mtree, mid, LFSR_TAG_MDIR, 1,
                LFSR_DATA_BUF(buf, d));
        if (err) {
            return err;
        }

    // other sibling reduced to zero
    } else if (msibling->rbyd.weight > 0) {
        LFS_DEBUG("Dropping mdir 0x{%"PRIx32",%"PRIx32"}",
                msibling->rbyd.block, msibling->redund_block);

        // update our mtree
        uint8_t buf[LFSR_MPTR_DSIZE];
        lfs_ssize_t d = lfsr_mdir_todisk(lfs, msibling, buf);
        if (d < 0) {
            return d;
        }

        err = lfsr_btree_set(lfs, mtree, mid, LFSR_TAG_MDIR, 1,
                LFSR_DATA_BUF(buf, d));
        if (err) {
            return err;
        }

    // both siblings reduced to zero
    } else {
        LFS_DEBUG("Dropping mdir 0x{%"PRIx32",%"PRIx32"}",
                mdir->rbyd.block, mdir->redund_block);
        LFS_DEBUG("Dropping mdir 0x{%"PRIx32",%"PRIx32"}",
                msibling->rbyd.block, msibling->redund_block);

        // update our mtree
        err = lfsr_btree_pop(lfs, mtree, mid);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_mdir_commit(lfs_t *lfs, lfsr_mdir_t *mdir, lfs_ssize_t *rid,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    LFS_ASSERT(mdir->mid != LFSR_MID_RM);

    // attempt to commit/compact the mdir normally
    lfsr_mdir_t mdir_ = *mdir;
    lfs_size_t split_id;
    int err = lfsr_mdir_commit_(lfs, &mdir_, &split_id, attrs, attr_count);
    if (err && err != LFS_ERR_RANGE) {
        return err;
    }

    // handle possible mtree updates, this gets a bit messy
    lfsr_mdir_t mroot_ = lfs->mroot;
    lfsr_btree_t mtree_ = lfs->mtree;
    lfsr_mdir_t msibling_;
    bool dirtymroot = false;
    bool dirtymtree = false;

    // need to split?
    if (err == LFS_ERR_RANGE) {
        // inlined in mroot?
        //
        // we need to uninline before we split, and it's possible uninlining
        // makes the mdir small enough that we don't even need to split
        if (mdir->mid == LFSR_MID_MROOT) {
            // wait, not inlined? this shouldn't happen, most likely too many
            // attributes in mroot
            LFS_ASSERT(lfsr_btree_weight(&mtree_) == 0);

            // do we still need to split?
            //
            // note init_id is changed to 0 here, ignoring -1 attrs
            int fits = lfsr_rbyd_estimate(lfs, &mdir->rbyd, 0,
                    lfs->cfg->block_size/2,
                    &split_id);
            if (fits < 0) {
                return fits;
            }

            // uninlining, but not splitting
            if (fits) {
                // compact into new mdir tags >= 0
                err = lfsr_mdir_compact_(lfs, &mdir_, 0, 0, -1,
                        mdir, attrs, attr_count, NULL, 0);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_RANGE);
                    return err;
                }

                LFS_DEBUG("Uninlining mdir 0x{%"PRIx32",%"PRIx32"} "
                        "-> 0x{%"PRIx32",%"PRIx32"}"
                        ", 0x{%"PRIx32",%"PRIx32"}",
                        mdir->rbyd.block, mdir->redund_block,
                        mdir->rbyd.block, mdir->redund_block,
                        mdir_.rbyd.block, mdir_.redund_block);

                // because of defered commits, our child can still be
                // reduced to zero, need to catch this here
                if (mdir_.rbyd.weight > 0) {
                    // update our mtree
                    uint8_t buf[LFSR_MPTR_DSIZE];
                    lfs_ssize_t d = lfsr_mdir_todisk(lfs, &mdir_, buf);
                    if (d < 0) {
                        return d;
                    }

                    err = lfsr_btree_push(lfs, &mtree_, 0, LFSR_TAG_MDIR, 1,
                            LFSR_DATA_BUF(buf, d));
                    if (err) {
                        return err;
                    }

                } else {
                    LFS_DEBUG("Dropping mdir 0x{%"PRIx32",%"PRIx32"}",
                            mdir_.rbyd.block, mdir_.redund_block);

                    // don't really need to update our mtree here
                }

            // uninlining and splitting
            } else {
                LFS_DEBUG("Uninlining mdir 0x{%"PRIx32",%"PRIx32"}",
                        mdir->rbyd.block, mdir->redund_block);

                // let lfsr_mtree_split_ do most of the work
                int err = lfsr_mtree_split_(lfs, &mtree_,
                        &mdir_, &msibling_, 0, -1,
                        mdir, split_id,
                        attrs, attr_count);
                if (err) {
                    return err;
                }
            }

            // commit mtree, also copying over any -1 tags
            lfsr_tag_t tag;
            uint8_t buf[LFSR_BTREE_DSIZE];
            lfs_ssize_t d = lfsr_btree_todisk(lfs, &mtree_, LFSR_TAG_MTREE,
                    &tag, buf);
            if (d < 0) {
                return d;
            }

            err = lfsr_mdir_compact_(lfs, &mroot_, LFSR_MID_WL, -1, 0,
                    mdir, attrs, attr_count, LFSR_ATTRS(
                        LFSR_ATTR_(-1, tag, 0, buf, d)));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

            dirtymroot = (lfsr_mdir_cmp(&lfs->mroot, &mroot_) != 0);

        // splitting a normal mdir
        } else {
            // let lfsr_mtree_split_ do most of the work
            int err = lfsr_mtree_split_(lfs, &mtree_,
                    &mdir_, &msibling_, -1, -1,
                    mdir, split_id,
                    attrs, attr_count);
            if (err) {
                return err;
            }

            dirtymtree = true;
        }

    // mdir reduced to zero? need to drop?
    } else if (mdir->mid != LFSR_MID_MROOT && mdir_.rbyd.weight == 0) {
        LFS_DEBUG("Dropping mdir 0x{%"PRIx32",%"PRIx32"}",
                mdir->rbyd.block, mdir->redund_block);

        // update our mtree
        err = lfsr_btree_pop(lfs, &mtree_, mdir->mid);
        if (err) {
            return err;
        }

        dirtymtree = true;

    // need to relocate?
    } else if (lfsr_mdir_cmp(mdir, &mdir_) != 0) {
        // relocate mroot
        if (mdir->mid == LFSR_MID_MROOT) {
            mroot_ = mdir_;
            dirtymroot = true;

        // relocate a normal mdir
        } else {
            LFS_DEBUG("Relocating mdir 0x{%"PRIx32",%"PRIx32"} "
                    "-> 0x{%"PRIx32",%"PRIx32"}",
                    mdir->rbyd.block, mdir->redund_block,
                    mdir_.rbyd.block, mdir_.redund_block);

            // update our mtree
            uint8_t buf[LFSR_MPTR_DSIZE];
            lfs_ssize_t d = lfsr_mdir_todisk(lfs, &mdir_, buf);
            if (d < 0) {
                return d;
            }

            err = lfsr_btree_set(lfs, &mtree_, mdir->mid, LFSR_TAG_MDIR, 1,
                    LFSR_DATA_BUF(buf, d));
            if (err) {
                return err;
            }

            dirtymtree = true;
        }

    // update the root
    } else if (mdir->mid == LFSR_MID_MROOT) {
        mroot_ = mdir_;
    }

    // need to update mtree?
    if (dirtymtree) {
        LFS_ASSERT(mdir_.mid != LFSR_MID_MROOT);

        // commit mtree
        lfsr_tag_t tag;
        uint8_t buf[LFSR_BTREE_DSIZE];
        lfs_ssize_t d = lfsr_btree_todisk(lfs, &mtree_, LFSR_TAG_MTREE,
                &tag, buf);
        if (d < 0) {
            return d;
        }

        err = lfsr_mdir_commit_(lfs, &mroot_, NULL, LFSR_ATTRS(
                LFSR_ATTR_(-1, tag, 0, buf, d)));
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        dirtymroot = (lfsr_mdir_cmp(&lfs->mroot, &mroot_) != 0);
    }

    // need to update mroot? tail recurse, updating mroots until a commit sticks
    lfsr_mdir_t mchildroot = lfs->mroot;
    lfsr_mdir_t mchildroot_ = mroot_;
    while (dirtymroot) {
        lfsr_mdir_t mparentroot;
        int err = lfsr_mtree_parent(lfs, lfsr_mdir_mptr(&mchildroot),
                &mparentroot);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT) {
            break;
        }

        LFS_DEBUG("Relocating mroot 0x{%"PRIx32",%"PRIx32"} "
                "-> 0x{%"PRIx32",%"PRIx32"}",
                mchildroot.rbyd.block, mchildroot.redund_block,
                mchildroot_.rbyd.block, mchildroot_.redund_block);

        // commit mrootchild 
        uint8_t buf[LFSR_MPTR_DSIZE];
        lfs_ssize_t d = lfsr_mdir_todisk(lfs, &mchildroot_, buf);
        if (d < 0) {
            return d;
        }

        lfsr_mdir_t mparentroot_ = mparentroot;
        err = lfsr_mdir_commit_(lfs, &mparentroot_, NULL, LFSR_ATTRS(
                LFSR_ATTR(-1, MROOT, 0, buf, d)));
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        mchildroot = mparentroot;
        mchildroot_ = mparentroot_;
        dirtymroot = (lfsr_mdir_cmp(&mchildroot, &mchildroot_) != 0);
    }

    // uh oh, we ran out of mrootparents, need to extend mroot chain
    if (dirtymroot) {
        // mchildroot should be our initial mroot at this point
        LFS_ASSERT(lfsr_mdir_ismrootanchor(&mchildroot));

        LFS_DEBUG("Extending mroot 0x{%"PRIx32",%"PRIx32"}"
                " -> 0x{%"PRIx32",%"PRIx32"}"
                ", 0x{%"PRIx32",%"PRIx32"}",
                mchildroot.rbyd.block, mchildroot.redund_block,
                mchildroot.redund_block, mchildroot.rbyd.block,
                mchildroot_.rbyd.block, mchildroot_.redund_block);

        // copy magic/config from current mroot
        lfsr_data_t magic;
        err = lfsr_mdir_lookup(lfs, &mchildroot,
                -1, LFSR_TAG_SUPERMAGIC, NULL, &magic);
        if (err) {
            return err;
        }

        lfsr_data_t config;
        err = lfsr_mdir_lookup(lfs, &mchildroot,
                -1, LFSR_TAG_SUPERCONFIG, NULL, &config);
        if (err) {
            return err;
        }

        // commit mrootchild
        uint8_t buf[LFSR_MPTR_DSIZE];
        lfs_ssize_t d = lfsr_mdir_todisk(lfs, &mchildroot_, buf);
        if (d < 0) {
            return d;
        }

        // compact into mparentroot, this should stay our mroot anchor
        lfsr_mdir_t mparentroot_ = mchildroot;
        err = lfsr_mdir_compact_(lfs, &mparentroot_, LFSR_MID_MROOTANCHOR, 0, 0,
                &mchildroot, NULL, 0, LFSR_ATTRS(
                    LFSR_ATTR_DATA(-1, SUPERMAGIC, 0, magic),
                    LFSR_ATTR_DATA(-1, SUPERCONFIG, 0, config),
                    // commit our new mchildroot
                    LFSR_ATTR(-1, MROOT, 0, buf, d)));
        if (err) {
            return err;
        }
    }

    // success?? update in-device state
    lfs->mroot = mroot_;
    lfs->mtree = mtree_;

    // update any opened mdirs
    for (lfsr_openedmdir_t *opened = lfs->opened;
            opened;
            opened = opened->next) {
        if (opened->mdir.mid == mdir->mid
                // avoid double-updating our current mdir
                && &opened->mdir != mdir) {
            LFS_ASSERT(opened->rid <= (lfs_ssize_t)opened->mdir.rbyd.weight);
            LFS_ASSERT(opened->rid != -1);

            // first play out any attrs that change our rid
            for (lfs_size_t i = 0; i < attr_count; i++) {
                if (opened->rid >= attrs[i].id) {
                    // removed?
                    if (opened->rid + attrs[i].delta < attrs[i].id) {
                        opened->rid = -2;
                        opened->mdir.mid = LFSR_MID_RM;
                    } else {
                        opened->rid += attrs[i].delta;
                    }
                }
            }

            // update mdir to follow rid
            if (opened->rid == -2) {
                // skip removed mdirs
            } else if ((lfs_size_t)opened->rid < mdir_.rbyd.weight) {
                opened->mdir = mdir_;
            } else {
                opened->rid = opened->rid - mdir_.rbyd.weight;
                opened->mdir = msibling_;
            }
        }
    }

    // update mdir to follow requested rid
    lfs_ssize_t rid_ = *rid;
    LFS_ASSERT(rid_ <= (lfs_ssize_t)mdir->rbyd.weight);
    LFS_ASSERT(rid_ != -2);
    if (rid_ == -1) {
        *mdir = mroot_;
    } else if ((lfs_size_t)rid_ < mdir_.rbyd.weight) {
        *mdir = mdir_;
    } else {
        *rid = rid_ - mdir_.rbyd.weight;
        *mdir = msibling_;
    }

    return 0;
}


// lookup dnames in our mtree 
static int lfsr_mdir_dnamelookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfs_size_t did, const char *name, lfs_size_t name_size,
        lfs_ssize_t *id_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    int err = lfsr_rbyd_dnamelookup(lfs, &mdir->rbyd,
            did, name, name_size,
            id_, tag_, NULL, data_);

    // When not found, lfsr_rbyd_dnamelookup returns the id smaller than our
    // expected name. This is correct for btree lookups, but not correct for
    // mdir insertions. For mdirs we need to adjust this by 1 so we insert
    // _after_ the smaller id.
    if (id_ && err == LFS_ERR_NOENT) {
        *id_ += 1;
    }

    return err;
}

// note if we fail, we at least leave mdir_/rid_ with the best place to insert
static int lfsr_mtree_dnamelookup(lfs_t *lfs,
        lfs_size_t did, const char *name, lfs_size_t name_size,
        lfsr_mdir_t *mdir_, lfs_ssize_t *rid_, lfsr_tag_t *tag_,
        lfsr_data_t *data_) {
    // do we only have mroot?
    lfsr_mdir_t mdir;
    if (lfsr_mtree_isinlined(lfs)) {
        mdir = lfs->mroot;

    // lookup dname in actual mtree
    } else {
        lfs_size_t mid;
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_btree_dnamelookup(lfs, &lfs->mtree,
                did, name, name_size,
                &mid, &tag, NULL, &data);
        if (err) {
            return err;
        }
        LFS_ASSERT(tag == LFSR_TAG_MDIR);

        // decode mptr
        lfsr_mptr_t mptr;
        lfs_ssize_t d = lfsr_mptr_fromdisk(lfs, &mptr, data);
        if (d < 0) {
            return d;
        }

        // fetch mdir
        err = lfsr_mdir_fetch(lfs, &mdir, mid, mptr);
        if (err) {
            return err;
        }
    }

    if (mdir_) {
        *mdir_ = mdir;
    }

    // and finally lookup dname in our mdir
    return lfsr_mdir_dnamelookup(lfs, &mdir,
            did, name, name_size,
            rid_, tag_, data_);
}


// special directory-ids
enum {
    LFSR_DID_ROOT = 0,
};

// lookup full paths in our mtree
//
// if not found, mdir_/rid_/did_/name_ will at least be set up
// with what should be the parent
static int lfsr_mtree_pathlookup(lfs_t *lfs, const char *path,
        // TODO originally path itself was a double pointer, is that a
        // better design?
        lfsr_mdir_t *mdir_, lfs_ssize_t *rid_, lfsr_tag_t *tag_,
        lfs_size_t *did_, const char **name_, lfs_size_t *name_size_) {
    // setup root
    lfsr_mdir_t mdir = {.mid = LFSR_MID_RM};
    lfs_ssize_t rid = -1;
    lfsr_tag_t tag = LFSR_TAG_DIR;
    lfs_size_t did = LFSR_DID_ROOT;

    if (mdir_) {
        *mdir_ = mdir;
    }
    if (rid_) {
        *rid_ = rid;
    }
    if (tag_) {
        *tag_ = tag;
    }
    
    // we reduce path to a single name if we can find it
    const char *name = path;

    while (true) {
        // skip slashes
        name += strspn(name, "/");
        lfs_size_t name_size = strcspn(name, "/");

        // skip '.' and root '..'
        if ((name_size == 1 && memcmp(name, ".", 1) == 0)
                || (name_size == 2 && memcmp(name, "..", 2) == 0)) {
            name += name_size;
            goto next;
        }

        // skip if matched by '..' in name
        const char *suffix = name + name_size;
        lfs_size_t suffix_size;
        int depth = 1;
        while (true) {
            suffix += strspn(suffix, "/");
            suffix_size = strcspn(suffix, "/");
            if (suffix_size == 0) {
                break;
            }

            if (suffix_size == 2 && memcmp(suffix, "..", 2) == 0) {
                depth -= 1;
                if (depth == 0) {
                    name = suffix + suffix_size;
                    goto next;
                }
            } else {
                depth += 1;
            }

            suffix += suffix_size;
        }

        // found end of path, we must be done parsing our path now
        if (name[0] == '\0') {
            return 0;
        }

        // only continue if we hit a directory
        if (tag != LFSR_TAG_DIR) {
            return LFS_ERR_NOTDIR;
        }

        // read the next did from the mdir if this is not the root
        if (rid != -1) {
            lfsr_data_t data;
            int err = lfsr_mdir_lookup(lfs, &mdir, rid, LFSR_TAG_DID,
                    NULL, &data);
            if (err) {
                return err;
            }

            lfs_ssize_t d = lfsr_data_readleb128(lfs, data, 0, &did);
            if (d < 0) {
                return d;
            }

            // TODO should we put this in lfsr_data_readleb128?
            // TODO should readtag then call lfsr_data_readleb128?
            if (did > 0x7fffffff) {
                return LFS_ERR_CORRUPT;
            }
        }

        // lookup up this dname in the mtree
        int err = lfsr_mtree_dnamelookup(lfs, did, name, name_size,
                &mdir, &rid, &tag, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // keep track of what we've seen so far
        if (mdir_) {
            *mdir_ = mdir;
        }
        if (rid_) {
            *rid_ = rid;
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

        // error if not found, note we update things first so mdir/rid
        // get updated with where to insert correctly
        if (err == LFS_ERR_NOENT) {
            return LFS_ERR_NOENT;
        }

        // go on to next name
        name += name_size;
next:;
    }
}


// incremental mtree traversal
typedef struct lfsr_mtree_traversal {
    // core traversal state
    lfsr_mdir_t mdir;
    lfsr_btree_traversal_t mtraversal;
    uint8_t flags;

    // cycle detection state
    uint8_t tortoise_power;
    lfs_size_t tortoise_step;
    lfsr_mptr_t tortoise_mptr;
} lfsr_mtree_traversal_t;

enum {
    LFSR_MTREE_TRAVERSAL_VALIDATE = 0x1,
};

#define LFSR_MTREE_TRAVERSAL_INIT(_flags) ((lfsr_mtree_traversal_t){ \
        .flags = _flags, \
        .mdir.rbyd.trunk = 0, \
        .mtraversal = LFSR_BTREE_TRAVERSAL_INIT, \
        .tortoise_power = 0, \
        .tortoise_step = 0, \
    })

static int lfsr_mtree_traversal_next(lfs_t *lfs,
        lfsr_mtree_traversal_t *traversal,
        lfs_size_t *mid_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // new traversal? start with 0x{0,1}
    //
    // note we make sure to include fake mroots!
    //
    if (traversal->mdir.rbyd.trunk == 0) {
        // fetch the first mroot 0x{0,1}
        int err = lfsr_mdir_fetch(lfs, &traversal->mdir,
                LFSR_MID_MROOT, LFSR_MPTR_MROOTANCHOR);
        if (err) {
            return err;
        }

        if (mid_) {
            *mid_ = -1;
        }
        if (tag_) {
            *tag_ = LFSR_TAG_MDIR;
        }
        if (data_) {
            *data_ = LFSR_DATA_BUF(&traversal->mdir, sizeof(lfsr_mdir_t));
        }
        goto cycle_detect;

    // check for mroot/mtree/mdir
    } else if (traversal->mdir.mid == LFSR_MID_MROOT) {
        // lookup mroot, if we find one this is a fake mroot
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_mdir_lookup(lfs, &traversal->mdir,
                -1, LFSR_TAG_WIDESTRUCT,
                &tag, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // found a new mroot
        if (err != LFS_ERR_NOENT && tag == LFSR_TAG_MROOT) {
            lfsr_mptr_t mptr;
            lfs_ssize_t d = lfsr_mptr_fromdisk(lfs, &mptr, data);
            if (d < 0) {
                return d;
            }

            int err = lfsr_mdir_fetch(lfs, &traversal->mdir,
                    LFSR_MID_MROOT, mptr);
            if (err) {
                return err;
            }

            if (mid_) {
                *mid_ = -1;
            }
            if (tag_) {
                *tag_ = LFSR_TAG_MDIR;
            }
            if (data_) {
                *data_ = LFSR_DATA_BUF(&traversal->mdir, sizeof(lfsr_mdir_t));
            }
            goto cycle_detect;

        // no more mroots, which makes this our real mroot
        } else {
            // update our mroot
            lfs->mroot = traversal->mdir;

            // do we have an mtree? mdir?
            if (err != LFS_ERR_NOENT) {
                if (tag != LFSR_TAG_MDIR && tag != LFSR_TAG_MTREE) {
                    LFS_ERROR("Weird mstruct? (0x%"PRIx32")", tag);
                    return LFS_ERR_CORRUPT;
                }

                lfs_ssize_t d = lfsr_btree_fromdisk(lfs, &lfs->mtree,
                        LFSR_TAG_MTREE, tag, 1, data);
                if (d < 0) {
                    return d;
                }

            // no mtree
            } else {
                lfs->mtree = LFSR_BTREE_NULL;
            }

            // initialize our mtree traversal
            traversal->mtraversal = LFSR_BTREE_TRAVERSAL_INIT;
        }
    }

    // traverse through the mtree
    lfs_size_t mid;
    lfsr_tag_t tag;
    lfsr_data_t data;
    int err = lfsr_btree_traversal_next(
            lfs, &lfs->mtree, &traversal->mtraversal,
            &mid, &tag, NULL, &data);
    if (err) {
        return err;
    }

    // inner btree nodes already decoded
    if (tag == LFSR_TAG_BTREE) {
        // validate our btree nodes if requested, this just means we need
        // to do a full rbyd fetch and make sure the checksums match
        if (traversal->flags & LFSR_MTREE_TRAVERSAL_VALIDATE) {
            lfsr_rbyd_t *branch = (lfsr_rbyd_t*)data.buf.buffer;
            lfsr_rbyd_t branch_;
            int err = lfsr_rbyd_fetch(lfs, &branch_,
                    branch->block, branch->trunk);
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    LFS_ERROR("Corrupted rbyd during mtree traversal "
                            "(0x%"PRIx32".%"PRIx32", 0x%08"PRIx32")",
                            branch->block, branch->trunk, branch->crc);
                }
                return err;
            }

            // test that our branch's crc matches what's expected
            //
            // it should be noted it's very unlikely for this to be hit without
            // the above fetch failing since it includes both an internal
            // crc check and trunk check
            if (branch_.crc != branch->crc) {
                LFS_ERROR("Checksum mismatch during mtree traversal "
                        "(0x%"PRIx32".%"PRIx32", "
                        "0x%08"PRIx32" != 0x%08"PRIx32")",
                        branch->block, branch->trunk,
                        branch_.crc, branch->crc);
                return LFS_ERR_CORRUPT;
            }

            LFS_ASSERT(branch_.trunk == branch->trunk);
            LFS_ASSERT(branch_.weight == branch->weight);

            // TODO is this useful at all?
            // change our branch to the fetched version
            *branch = branch_;
        }

        // still update our mdir mid so we don't get stuck in a loop
        // traversing mroots
        traversal->mdir.mid = mid;

        if (mid_) {
            *mid_ = mid;
        }
        if (tag_) {
            *tag_ = tag;
        }
        if (data_) {
            *data_ = data;
        }
        return 0;

    // fetch mdir if we're on a leaf
    } else if (tag == LFSR_TAG_MDIR) {
        lfsr_mptr_t mptr;
        lfs_ssize_t d = lfsr_mptr_fromdisk(lfs, &mptr, data);
        if (d < 0) {
            return d;
        }

        int err = lfsr_mdir_fetch(lfs, &traversal->mdir, mid, mptr);
        if (err) {
            return err;
        }

        if (mid_) {
            *mid_ = mid;
        }
        if (tag_) {
            *tag_ = tag;
        }
        if (data_) {
            *data_ = LFSR_DATA_BUF(&traversal->mdir, sizeof(lfsr_mdir_t));
        }
        goto cycle_detect;

    } else {
        LFS_ERROR("Weird mtree entry? (0x%"PRIx32")", tag);
        return LFS_ERR_CORRUPT;
    }

cycle_detect:;
    // detect cycles with Brent's algorithm
    //
    // note we only consider mdirs here, the btree inner nodes
    // require checksums of their pointers, so creating a valid
    // cycle is actually quite difficult
    //
    if (lfsr_mptr_cmp(
            lfsr_mdir_mptr(&traversal->mdir),
            traversal->tortoise_mptr) == 0) {
        LFS_ERROR("Cycle detected during mtree traversal "
                "(0x{%"PRIx32",%"PRIx32"})",
                traversal->mdir.rbyd.block, traversal->mdir.redund_block);
        return LFS_ERR_CORRUPT;
    }
    if (traversal->tortoise_step
            == ((lfs_size_t)1 << traversal->tortoise_power)) {
        traversal->tortoise_mptr = lfsr_mdir_mptr(&traversal->mdir);
        traversal->tortoise_step = 0;
        traversal->tortoise_power += 1;
    }
    traversal->tortoise_step += 1;

    return 0;
}


/// Superblock things ///

// These are all leb128s, but we can expect smaller encodings
// if we assume the version.
//
// - 7-bit major_version => 1 byte leb128 (worst case)
// - 7-bit minor_version => 1 byte leb128 (worst case)
// - 7-bit csum_type     => 1 byte leb128 (worst case)
// - 7-bit flags         => 1 byte leb128 (worst case)
// - 32-bit block_size   => 5 byte leb128 (worst case)
// - 32-bit block_count  => 5 byte leb128 (worst case)
// - 7-bit utag_limit    => 1 byte leb128 (worst case)
// - 32-bit attr_limit   => 5 byte leb128 (worst case)
// - 32-bit name_limit   => 5 byte leb128 (worst case)
// - 32-bit file_limit   => 5 byte leb128 (worst case)
//                       => 30 bytes total
// 
#define LFSR_SUPERCONFIG_DSIZE (1+1+1+1+5+5+1+5+5+5)

static lfs_ssize_t lfsr_superconfig_todisk(lfs_t *lfs,
        uint8_t buffer[static LFSR_SUPERCONFIG_DSIZE]) {
    // TODO most of these should also be in the lfs_config/lfs_t structs

    // note we take a shortcut for for single-byte leb128s, but these
    // are still leb128s! the top bit must be zero!

    // on-disk major version
    buffer[0] = LFS_DISK_VERSION_MAJOR;
    // on-disk minor version
    buffer[1] = LFS_DISK_VERSION_MINOR;
    // on-disk csum type
    buffer[2] = 2;
    // on-disk flags
    buffer[3] = 0;

    // on-disk block size
    lfs_ssize_t d = 4;
    lfs_ssize_t d_ = lfs_toleb128(lfs->cfg->block_size, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    // on-disk block count
    d_ = lfs_toleb128(lfs->cfg->block_count, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    // on-disk utag limit
    buffer[d] = 0x7f;
    d += 1;

    // on-disk attr limit
    d_ = lfs_toleb128(0x7fffffff, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    // on-disk name limit
    d_ = lfs_toleb128(0xff, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    // on-disk file limit
    d_ = lfs_toleb128(0x7fffffff, &buffer[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    return d;
}


/// Filesystem init functions ///

static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg);
static int lfs_deinit(lfs_t *lfs);

static int lfsr_mountinited(lfs_t *lfs) {
    // traverse the mtree rooted at mroot 0x{1,0}
    //
    // note that lfsr_mtree_traversal_next will update our mroot/mtree
    // based on what mroots it finds
    //
    // we do validate btree inner nodes here, how can we trust our
    // mdirs are valid if we haven't checked the btree inner nodes at
    // least once?
    lfsr_mtree_traversal_t traversal = LFSR_MTREE_TRAVERSAL_INIT(
            LFSR_MTREE_TRAVERSAL_VALIDATE);
    while (true) {
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_mtree_traversal_next(lfs, &traversal,
                NULL, &tag, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT) {
            break;
        }

        // we only care about mdirs here
        if (tag != LFSR_TAG_MDIR) {
            continue;
        }

        lfsr_mdir_t *mdir = (lfsr_mdir_t*)data.buf.buffer;
        // found an mroot?
        if (mdir->mid == LFSR_MID_MROOT) {
            // has magic string?
            lfsr_data_t data;
            err = lfsr_mdir_lookup(lfs, mdir, -1, LFSR_TAG_SUPERMAGIC,
                    NULL, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT) {
                lfs_scmp_t cmp = lfsr_data_cmp(lfs, data, 0, "littlefs", 8);
                if (cmp < 0) {
                    return cmp;
                }

                // treat corrupted magic as no magic
                if (lfs_cmp(cmp) != 0) {
                    err = LFS_ERR_NOENT;
                }
            }

            if (err == LFS_ERR_NOENT) {
                LFS_ERROR("No littlefs magic found");
                return LFS_ERR_INVAL;
            }

            // lookup the superconfig
            err = lfsr_mdir_lookup(lfs, mdir, -1, LFSR_TAG_SUPERCONFIG,
                    NULL, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT) {
                // check the major/minor version
                uint32_t major_version;
                uint32_t minor_version;

                lfs_size_t d = 0;
                lfs_ssize_t d_ = lfsr_data_readleb128(lfs, data, d,
                        &major_version);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;

                    d_ = lfsr_data_readleb128(lfs, data, d, &minor_version);
                    // treat any leb128 overflows as out-of-range values
                    if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                        return d_;
                    }
                    if (d_ != LFS_ERR_CORRUPT) {
                        d += d_;
                    }
                }
                
                if (d_ == LFS_ERR_CORRUPT
                        || major_version != LFS_DISK_VERSION_MAJOR
                        || minor_version > LFS_DISK_VERSION_MINOR) {
                    LFS_ERROR("Incompatible version v%"PRIu32".%"PRIu32
                            " (!= v%"PRIu32".%"PRIu32")",
                            (d_ == LFS_ERR_CORRUPT
                                ? (uint32_t)-1
                                : major_version),
                            (d_ == LFS_ERR_CORRUPT
                                ? (uint32_t)-1
                                : minor_version),
                            LFS_DISK_VERSION_MAJOR,
                            LFS_DISK_VERSION_MINOR);
                    return LFS_ERR_INVAL;
                }

                // check the on-disk csum type
                uint32_t csum_type;
                d_ = lfsr_data_readleb128(lfs, data, d, &csum_type);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT || csum_type != 2) {
                    LFS_ERROR("Incompatible csum type 0x%"PRIx32
                            " (!= 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT ? (uint32_t)-1 : csum_type),
                            2);
                    return LFS_ERR_INVAL;
                }

                // check for any on-disk flags
                uint32_t flags;
                d_ = lfsr_data_readleb128(lfs, data, d, &flags);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT || flags != 0) {
                    LFS_ERROR("Incompatible flags 0x%"PRIx32
                            " (!= 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT ? (uint32_t)-1 : flags),
                            0);
                    return LFS_ERR_INVAL;
                }

                // check the on-disk block size
                // TODO actually use this
                uint32_t block_size;
                d_ = lfsr_data_readleb128(lfs, data, d, &block_size);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT
                        || block_size != lfs->cfg->block_size) {
                    LFS_ERROR("Incompatible block size 0x%"PRIx32
                            " (!= 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT ? (uint32_t)-1 : block_size),
                            lfs->cfg->block_size);
                    return LFS_ERR_INVAL;
                }

                // check the on-disk block count
                // TODO actually use this
                uint32_t block_count;
                d_ = lfsr_data_readleb128(lfs, data, d, &block_count);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT
                        || block_count != lfs->cfg->block_count) {
                    LFS_ERROR("Incompatible block count 0x%"PRIx32
                            " (!= 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT
                                ? (uint32_t)-1
                                : block_count),
                            lfs->cfg->block_count);
                    return LFS_ERR_INVAL;
                }

                // check the on-disk utag limit
                // TODO actually use this
                uint32_t utag_limit;
                d_ = lfsr_data_readleb128(lfs, data, d, &utag_limit);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT || utag_limit != 0x7f) {
                    LFS_ERROR("Incompatible utag limit 0x%"PRIx32
                            " (> 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT ? (uint32_t)-1 : utag_limit),
                            0x7f);
                    return LFS_ERR_INVAL;
                }

                // check the on-disk attr limit
                // TODO actually use this
                uint32_t attr_limit;
                d_ = lfsr_data_readleb128(lfs, data, d, &attr_limit);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT || attr_limit != 0x7fffffff) {
                    LFS_ERROR("Incompatible attr limit 0x%"PRIx32
                            " (> 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT ? (uint32_t)-1 : attr_limit),
                            0x7fffffff);
                    return LFS_ERR_INVAL;
                }

                // check the on-disk name limit
                // TODO actually use this
                uint32_t name_limit;
                d_ = lfsr_data_readleb128(lfs, data, d, &name_limit);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT || name_limit != 0xff) {
                    LFS_ERROR("Incompatible name limit 0x%"PRIx32
                            " (> 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT ? (uint32_t)-1 : name_limit),
                            0xff);
                    return LFS_ERR_INVAL;
                }

                // check the on-disk file limit
                // TODO actually use this
                uint32_t file_limit;
                d_ = lfsr_data_readleb128(lfs, data, d, &file_limit);
                // treat any leb128 overflows as out-of-range values
                if (d_ < 0 && d_ != LFS_ERR_CORRUPT) {
                    return d_;
                }
                if (d_ != LFS_ERR_CORRUPT) {
                    d += d_;
                }

                if (d_ == LFS_ERR_CORRUPT || file_limit != 0x7fffffff) {
                    LFS_ERROR("Incompatible file limit 0x%"PRIx32
                            " (> 0x%"PRIx32")",
                            (d_ == LFS_ERR_CORRUPT ? (uint32_t)-1 : file_limit),
                            0x7fffffff);
                    return LFS_ERR_INVAL;
                }
            }
        }
    }

    // once we've mounted and derived a pseudo-random seed, initialize our
    // block allocator
    //
    // the purpose of this is to avoid bad wear patterns such as always 
    // allocating blocks near the beginning of disk after a power-loss
    //
    lfs->lookahead.start = lfs->seed % lfs->cfg->block_count;

    return 0;
}

static int lfsr_formatinited(lfs_t *lfs) {
    uint8_t buf[LFSR_SUPERCONFIG_DSIZE];
    lfs_ssize_t d = lfsr_superconfig_todisk(lfs, buf);
    if (d < 0) {
        return d;
    }

    for (uint32_t i = 0; i < 2; i++) {
        // write superblock to both rbyds in the root mroot to hopefully
        // avoid mounting an older filesystem on disk
        lfsr_rbyd_t rbyd = {.block=i, .off=0, .trunk=0};

        int err = lfsr_bd_erase(lfs, rbyd.block);
        if (err) {
            return err;
        }

        err = lfsr_rbyd_appendrev(lfs, &rbyd, (uint32_t)i - 1);
        if (err) {
            return err;
        }

        // our initial superblock contains a couple things:
        // - our magic string, "littlefs"
        // - the superconfig, format-time configuration
        // - the root's dstart tag, which reserves did = 0 for the root
        err = lfsr_rbyd_commit(lfs, &rbyd, LFSR_ATTRS(
                LFSR_ATTR(-1, SUPERMAGIC, 0, "littlefs", 8),
                LFSR_ATTR(-1, SUPERCONFIG, 0, buf, d),
                LFSR_ATTR_DNAME(0, DSTART, +1, 0, NULL, 0)));
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

    err = lfsr_formatinited(lfs);
    if (err) {
        // make sure we clean up on error
        lfs_deinit(lfs);
        return err;
    }

    return lfs_deinit(lfs);
}



/// Block allocator ///

// Allocations should call this when all allocated blocks are committed to the
// filesystem, either in the mtree or in tracked mdirs. After an ack, the block
// allocator may realloc any untracked blocks.
static void lfs_alloc_ack(lfs_t *lfs) {
    lfs->lookahead.acked = lfs->cfg->block_count;
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

static int lfs_alloc(lfs_t *lfs, lfs_block_t *block) {
    while (true) {
        // scan our lookahead buffer for free blocks
        while (lfs->lookahead.next < lfs->lookahead.size) {
            if (!(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                    & (1 << (lfs->lookahead.next % 8)))) {
                // found a free block
                *block = (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->cfg->block_count;

                // eagerly find next free block to maximize how many blocks
                // lfs_alloc_ack makes available for scanning
                while (true) {
                    lfs->lookahead.next += 1;
                    lfs->lookahead.acked -= 1;

                    if (lfs->lookahead.next >= lfs->lookahead.size
                            || !(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                                & (1 << (lfs->lookahead.next % 8)))) {
                        return 0;
                    }
                }
            }

            lfs->lookahead.next += 1;
            lfs->lookahead.acked -= 1;
        }

        // In order to keep our block allocator from spinning forever when our
        // filesystem is full, we mark points where there are no in-flight
        // allocations with an "ack" before starting a set of allocaitons.
        //
        // If we've looked at all blocks since the last ack, we report the
        // filesystem as out of storage.
        //
        if (lfs->lookahead.acked <= 0) {
            LFS_ERROR("No more free space 0x%"PRIx32,
                    (lfs->lookahead.start + lfs->lookahead.next)
                        % lfs->cfg->block_count);
            return LFS_ERR_NOSPC;
        }

        // No blocks in our lookahead buffer, we need to scan the filesystem for
        // unused blocks in the next lookahead window.
        //
        // note we limit the lookahead window to at most the amount of blocks
        // acked, this prevents the above math from underflowing
        //
        lfs->lookahead.start += lfs->lookahead.size;
        lfs->lookahead.next = 0;
        lfs->lookahead.size = lfs_min32(
                8*lfs->cfg->lookahead_size,
                lfs->lookahead.acked);
        memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);

        // traverse the filesystem, building up knowledge of what blocks are
        // in use in our lookahead window
        lfsr_mtree_traversal_t traversal = LFSR_MTREE_TRAVERSAL_INIT(0);
        while (true) {
            lfsr_tag_t tag;
            lfsr_data_t data;
            int err = lfsr_mtree_traversal_next(lfs, &traversal,
                    NULL, &tag, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }
            if (err == LFS_ERR_NOENT) {
                break;
            }

            // TODO add block pointers here?

            // mark any blocks we see at in-use, including any btree/mdir blocks
            if (tag == LFSR_TAG_MDIR) {
                lfsr_mdir_t *mdir = (lfsr_mdir_t*)data.buf.buffer;
                lfs_alloc_setinuse(lfs, mdir->rbyd.block);
                lfs_alloc_setinuse(lfs, mdir->redund_block);

            } else if (tag == LFSR_TAG_BTREE) {
                lfsr_rbyd_t *branch = (lfsr_rbyd_t*)data.buf.buffer;
                lfs_alloc_setinuse(lfs, branch->block);
            }
        }
    }
}


/// Directory operations ///

int lfsr_mkdir(lfs_t *lfs, const char *path) {
    // checkpoint block allocations
    // TODO we should just name this lfsr_alloc_checkpoint
    lfs_alloc_ack(lfs);

    // lookup our parent
    lfsr_openedmdir_t parent;
    lfs_size_t parent_did;
    const char *name;
    lfs_size_t name_size;
    int err = lfsr_mtree_pathlookup(lfs, path,
            &parent.mdir, &parent.rid, NULL,
            &parent_did, &name, &name_size);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    // woah, already exists?
    if (err != LFS_ERR_NOENT) {
        return LFS_ERR_EXIST;
    }

    // check that name fits
    if (name_size > lfs->name_max) {
        return LFS_ERR_NAMETOOLONG;
    }

    // Our directory needs an arbitrary directory-id. To find one with
    // hopefully few collisions, we use a hash of the full path. Since
    // we have a CRC handy, we can use that.
    //
    // We truncate to 28-bits to more optimally use our leb128 encoding.
    // TODO should we have a configurable limit for this? dir_limit? or
    // just loose ~3 bits from the configured file limit?
    //
    lfs_size_t did = lfs_crc32c(0, path, strlen(path)) & 0xfffffff;

    // Check if we have a collision. If we do, search for the next
    // available did
    lfsr_mdir_t mdir;
    lfs_ssize_t rid;
    while (true) {
        int err = lfsr_mtree_dnamelookup(lfs, did, NULL, 0,
                &mdir, &rid, NULL, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }
        if (err == LFS_ERR_NOENT) {
            break;
        }

        // try the next did
        did = (did + 1) & 0xfffffff;
    }

    // Note when we write to the mtree, it's possible it changes our
    // parent's mdir/rid. We can catch this by tracking our parent
    // as "opened" temporarily
    // TODO is this the best workaround for rid update issues?
    parent.rid -= 1;
    lfsr_mdir_addopened(lfs, &parent);

    // TODO GRM, make this power-safe
    // Conveniently, we just found where our dstart should go. The dstart
    // tag is an empty entry that marks our directory as being allocated.
    err = lfsr_mdir_commit(lfs, &mdir, &rid, LFSR_ATTRS(
            LFSR_ATTR_DNAME(rid, DSTART, +1, did, NULL, 0)));
    if (err) {
        goto failed_with_parent;
    }

    lfsr_mdir_removeopened(lfs, &parent);
    parent.rid += 1;

    // commit our new directory into our parent
    err = lfsr_mdir_commit(lfs, &parent.mdir, &parent.rid, LFSR_ATTRS(
            LFSR_ATTR_DNAME(parent.rid, DIR, +1, parent_did, name, name_size),
            LFSR_ATTR_LEB128(parent.rid, DID, 0, did)));
    if (err) {
        return err;
    }

    return 0;

failed_with_parent:
    lfsr_mdir_removeopened(lfs, &parent);
    return err;
}

int lfsr_dir_open(lfs_t *lfs, lfsr_dir_t *dir, const char *path) {
    // lookup our directory
    lfsr_mdir_t mdir;
    lfs_ssize_t rid;
    lfsr_tag_t tag;
    int err = lfsr_mtree_pathlookup(lfs, path,
            &mdir, &rid, &tag,
            NULL, NULL, NULL);
    if (err) {
        return err;
    }

    // are we a directory?
    if (tag != LFSR_TAG_DIR) {
        return LFS_ERR_NOENT;
    }

    // read our did from the mdir, unless we're root
    lfs_size_t did = 0;
    if (rid != -1) {
        lfsr_data_t data;
        int err = lfsr_mdir_lookup(lfs, &mdir, rid, LFSR_TAG_DID,
                NULL, &data);
        if (err) {
            return err;
        }

        lfs_ssize_t d = lfsr_data_readleb128(lfs, data, 0, &did);
        if (d < 0) {
            return d;
        }

        // TODO should we put this in lfsr_data_readleb128?
        // TODO should readtag then call lfsr_data_readleb128?
        if (did > 0x7fffffff) {
            return LFS_ERR_CORRUPT;
        }
    }

    // now lookup our dstart in the mtree
    err = lfsr_mtree_dnamelookup(lfs, did, NULL, 0,
            &dir->mdir.mdir, &dir->mdir.rid, NULL, NULL);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_NOENT);
        return err;
    }

    // add to tracked mdirs
    lfsr_mdir_addopened(lfs, &dir->mdir);
    dir->off = 0;
    return 0;
}

int lfsr_dir_close(lfs_t *lfs, lfsr_dir_t *dir) {
    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, &dir->mdir);
    return 0;
}

int lfsr_dir_read(lfs_t *lfs, lfsr_dir_t *dir, struct lfs_info *info) {
    memset(info, 0, sizeof(struct lfs_info));

    // handle "." and ".." specially
    if (dir->off == 0) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, ".");
        dir->off += 1;
        return 0;
    } else if (dir->off == 1) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, "..");
        dir->off += 1;
        return 0;
    }

    // lookup the next entry in our dir
    lfs_ssize_t rid = dir->mdir.rid + 1;
    // need to lookup the next mdir?
    if (rid >= (lfs_ssize_t)dir->mdir.mdir.rbyd.weight) {
        // out of mdirs?
        lfs_ssize_t mid = dir->mdir.mdir.mid + 1;
        if (mid >= lfsr_mtree_weight(lfs)) {
            return LFS_ERR_NOENT;
        }

        int err = lfsr_mtree_lookup(lfs, mid, &dir->mdir.mdir);
        if (err) {
            return err;
        }
        rid = 0;
    }
    dir->mdir.rid = rid;

    // lookup our name tag
    lfsr_tag_t tag;
    lfsr_data_t data;
    int err = lfsr_mdir_lookup(lfs, &dir->mdir.mdir, rid, LFSR_TAG_WIDENAME,
            &tag, &data);
    if (err) {
        return err;
    }

    // found another directory's dstart? we must be done
    if (tag == LFSR_TAG_DSTART) {
        return LFS_ERR_NOENT;
    }

    // fill in our info struct
    info->type = tag - LFSR_TAG_REG;

    LFS_ASSERT(lfsr_data_size(data) <= lfs->name_max);
    lfs_ssize_t d = lfsr_data_readleb128(lfs, data, 0, &(uint32_t){0});
    if (d < 0) {
        return d;
    }
    d = lfsr_data_read(lfs, data, d, info->name, LFS_NAME_MAX);
    if (d < 0) {
        return d;
    }
    info->name[d] = '\0';

    // TODO size once we actually have regular files

    return 0;
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
//    file->type = LFS_TYPE_REG;
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
    lfs->cfg = cfg;
    int err = 0;

    // validate that the lfs-cfg sizes were initiated properly before
    // performing any arithmetic logics with them
    LFS_ASSERT(lfs->cfg->read_size != 0);
    LFS_ASSERT(lfs->cfg->prog_size != 0);
    LFS_ASSERT(lfs->cfg->cache_size != 0);

    // check that block size is a multiple of cache size is a multiple
    // of prog and read sizes
    LFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->read_size == 0);
    LFS_ASSERT(lfs->cfg->cache_size % lfs->cfg->prog_size == 0);
    LFS_ASSERT(lfs->cfg->block_size % lfs->cfg->cache_size == 0);

    // check that the block size is large enough to fit ctz pointers
    LFS_ASSERT(4*lfs_npw2(0xffffffff / (lfs->cfg->block_size-2*4))
            <= lfs->cfg->block_size);

    // block_cycles = 0 is no longer supported.
    //
    // block_cycles is the number of erase cycles before littlefs evicts
    // metadata logs as a part of wear leveling. Suggested values are in the
    // range of 100-1000, or set block_cycles to -1 to disable block-level
    // wear-leveling.
    LFS_ASSERT(lfs->cfg->block_cycles != 0);


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

    // setup lookahead buffer, note mount finishes initializing this after
    // we establish a decent pseudo-random seed
    LFS_ASSERT(lfs->cfg->lookahead_size > 0);
    if (lfs->cfg->lookahead_buffer) {
        lfs->lookahead.buffer = lfs->cfg->lookahead_buffer;
    } else {
        lfs->lookahead.buffer = lfs_malloc(lfs->cfg->lookahead_size);
        if (!lfs->lookahead.buffer) {
            err = LFS_ERR_NOMEM;
            goto cleanup;
        }
    }
    lfs->lookahead.start = 0;
    lfs->lookahead.size = 0;
    lfs->lookahead.next = 0;
    lfs->lookahead.acked = 0;

    // check that the size limits are sane
    LFS_ASSERT(lfs->cfg->name_max <= LFS_NAME_MAX);
    lfs->name_max = lfs->cfg->name_max;
    if (!lfs->name_max) {
        lfs->name_max = LFS_NAME_MAX;
    }

    LFS_ASSERT(lfs->cfg->file_max <= LFS_FILE_MAX);
    lfs->file_max = lfs->cfg->file_max;
    if (!lfs->file_max) {
        lfs->file_max = LFS_FILE_MAX;
    }

    LFS_ASSERT(lfs->cfg->attr_max <= LFS_ATTR_MAX);
    lfs->attr_max = lfs->cfg->attr_max;
    if (!lfs->attr_max) {
        lfs->attr_max = LFS_ATTR_MAX;
    }

    LFS_ASSERT(lfs->cfg->metadata_max <= lfs->cfg->block_size);

    // setup default state
    lfs->root[0] = LFS_BLOCK_NULL;
    lfs->root[1] = LFS_BLOCK_NULL;
    lfs->mlist = NULL;
    lfs->seed = 0;
    lfs->gdisk = (lfs_gstate_t){0};
    lfs->gstate = (lfs_gstate_t){0};
    lfs->gdelta = (lfs_gstate_t){0};
#ifdef LFS_MIGRATE
    lfs->lfs1 = NULL;
#endif

    lfs->opened = NULL;

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

