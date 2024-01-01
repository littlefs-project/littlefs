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
        lfs_block_t block, lfs_size_t off,
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
                lfs_size_t off_ = lfs_max(off, pcache->off);
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
            lfs_size_t off_ = lfs_max(rcache->off, pcache->off);
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
        lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    lfs_size_t diff = 0;
    // make sure our hint is at least as big as our buffer
    hint = lfs_max(hint, size);

    for (lfs_size_t i = 0; i < size; i += diff) {
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
        lfs_block_t block, lfs_size_t off, lfs_size_t size, uint32_t *crc) {
    lfs_size_t diff = 0;

    for (lfs_size_t i = 0; i < size; i += diff) {
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
        lfs_cache_t *pcache, lfs_cache_t *rcache, bool validate,
        uint32_t *flcksum_) {
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

        // this is when we update the the flushed checksum if requested
        if (flcksum_) {
            *flcksum_ = lfs_crc32c(*flcksum_,
                pcache->buffer, pcache->size);
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

    int err = lfs_bd_flush(lfs, pcache, rcache, validate, NULL);
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
        lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *flcksum_) {
    const uint8_t *data = buffer;
    LFS_ASSERT(block == LFS_BLOCK_INLINE || block < lfs->cfg->block_count);
    LFS_ASSERT(off + size <= lfs->cfg->block_size);

    // update rcache if we overlap
    if (rcache
            && block == rcache->block
            && off < rcache->off + rcache->size
            && off + size > rcache->off) {
        lfs_size_t off_ = lfs_max(off, rcache->off);
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
                int err = lfs_bd_flush(lfs, pcache, rcache, validate,
                        flcksum_);
                if (err) {
                    return err;
                }
            }

            continue;
        }

        // prepare pcache, first condition can no longer fail
        lfs_cache_zero(lfs, pcache);
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
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        void *buffer, lfs_size_t size) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    return lfs_bd_read(lfs, &lfs->pcache, &lfs->rcache, hint,
            block, off, buffer, size);
}

// TODO merge lfsr_bd_readcksum/lfsr_bd_cksum somehow?
static int lfsr_bd_readcksum(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        void *buffer, lfs_size_t size,
        uint32_t *cksum_) {
    int err = lfsr_bd_read(lfs, block, off, hint, buffer, size);
    if (err) {
        return err;
    }

    *cksum_ = lfs_crc32c(*cksum_, buffer, size);
    return 0;
}

static int lfsr_bd_cksum(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint, lfs_size_t size,
        uint32_t *cksum_) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    return lfs_bd_crc32c(lfs, &lfs->pcache, &lfs->rcache, hint,
            block, off, size, cksum_);
}

static lfs_scmp_t lfsr_bd_cmp(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint, 
        const void *buffer, lfs_size_t size) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    return lfs_bd_cmp(lfs, &lfs->pcache, &lfs->rcache, hint,
            block, off, buffer, size);
}

// program data with optional checksum
static int lfsr_bd_prog(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *cksum_, uint32_t *flcksum_) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    int err = lfs_bd_prog(lfs, &lfs->pcache, &lfs->rcache, false,
            block, off, buffer, size,
            flcksum_);
    if (err) {
        return err;
    }

    // optional checksum
    if (cksum_) {
        *cksum_ = lfs_crc32c(*cksum_, buffer, size);
    }

    return 0;
}

static int lfsr_bd_flush(lfs_t *lfs,
        uint32_t *flcksum_) {
    return lfs_bd_flush(lfs, &lfs->pcache, &lfs->rcache, false,
            flcksum_);
}

static int lfsr_bd_sync(lfs_t *lfs) {
    return lfs_bd_sync(lfs, &lfs->pcache, &lfs->rcache, false);
}

// TODO do we need this? should everything be checked by crc and validation
// be an optional ifdef?
static int lfsr_bd_progvalidate(lfs_t *lfs, lfs_block_t block, lfs_size_t off,
        const void *buffer, lfs_size_t size,
        uint32_t *cksum_, uint32_t *flcksum_) {
    // check for in-bounds
    if (off+size > lfs->cfg->block_size) {
        lfs_cache_zero(lfs, &lfs->pcache);
        return LFS_ERR_RANGE;
    }

    int err = lfs_bd_prog(lfs, &lfs->pcache, &lfs->rcache, true,
            block, off, buffer, size,
            flcksum_);
    if (err) {
        return err;
    }

    // optional checksum
    if (cksum_) {
        *cksum_ = lfs_crc32c(*cksum_, buffer, size);
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
enum lfsr_tag {
    // the null tag is reserved
    LFSR_TAG_NULL           = 0x0000,

    // config tags
    LFSR_TAG_CONFIG         = 0x0000,
    LFSR_TAG_MAGIC          = 0x0003,
    LFSR_TAG_VERSION        = 0x0004,
    LFSR_TAG_OCOMPATFLAGS   = 0x0005,
    LFSR_TAG_RCOMPATFLAGS   = 0x0006,
    LFSR_TAG_WCOMPATFLAGS   = 0x0007,
    LFSR_TAG_BLOCKSIZE      = 0x0008,
    LFSR_TAG_BLOCKCOUNT     = 0x0009,
    LFSR_TAG_NAMELIMIT      = 0x000a,
    LFSR_TAG_SIZELIMIT      = 0x000b,

    // global-state tags
    LFSR_TAG_GDELTA         = 0x0100,
    LFSR_TAG_GRMDELTA       = 0x0100,

    // name tags
    LFSR_TAG_NAME           = 0x0200,
    LFSR_TAG_REG            = 0x0201,
    LFSR_TAG_DIR            = 0x0202,
    LFSR_TAG_BOOKMARK       = 0x0204,

    // struct tags
    LFSR_TAG_STRUCT         = 0x0300,
    LFSR_TAG_DATA           = 0x0300,
    LFSR_TAG_BLOCK          = 0x0304,
    LFSR_TAG_BSHRUB         = 0x0308,
    LFSR_TAG_BTREE          = 0x030c,
    LFSR_TAG_DID            = 0x0310,
    LFSR_TAG_BECKSUM        = 0x0314,
    LFSR_TAG_BRANCH         = 0x031c,
    LFSR_TAG_MROOT          = 0x0321,
    LFSR_TAG_MDIR           = 0x0325,
    LFSR_TAG_MTREE          = 0x032c,

    // user/sys attributes
    LFSR_TAG_UATTR          = 0x0400,
    LFSR_TAG_SATTR          = 0x0600,

    // shrub tags belong to secondary trees
    LFSR_TAG_SHRUB          = 0x1000,

    // alt pointers form the inner nodes of our rbyd trees
    LFSR_TAG_ALT            = 0x4000,
    LFSR_TAG_LE             = 0x0000,
    LFSR_TAG_GT             = 0x2000,
    LFSR_TAG_B              = 0x0000,
    LFSR_TAG_R              = 0x1000,

    // checksum tags
    LFSR_TAG_CKSUM          = 0x3000,
    LFSR_TAG_ECKSUM         = 0x3100,

    // in-device only tags, these should never get written to disk
    LFSR_TAG_INTERNAL       = 0x0800,
    LFSR_TAG_GRM            = 0x0800,
    LFSR_TAG_MOVE           = 0x0801,
    LFSR_TAG_SHRUBCOMMIT    = 0x0802,
    LFSR_TAG_SHRUBTRUNK     = 0x0803,

    // some in-device only tag modifiers
    LFSR_TAG_RM             = 0x8000,
    LFSR_TAG_GROW           = 0x4000,
    LFSR_TAG_WIDE           = 0x2000,

    // lfsr_rbyd_appendattr specific flags, also in-device only
    LFSR_TAG_DIVERGED       = 0x4000,
    LFSR_TAG_DIVERGEDUPPER  = 0x2000,
    LFSR_TAG_DIVERGEDLOWER  = 0x0000,
};

// LFSR_TAG_TAG just provides and escape hatch to pass raw tags
// through the LFSR_ATTR macro
#define LFSR_TAG_TAG(tag) (tag)

// some tag modifiers
#define LFSR_TAG_SHRUB(tag) (LFSR_TAG_SHRUB | LFSR_TAG_##tag)
#define LFSR_TAG_RM(tag)    (LFSR_TAG_RM    | LFSR_TAG_##tag)
#define LFSR_TAG_GROW(tag)  (LFSR_TAG_GROW  | LFSR_TAG_##tag)
#define LFSR_TAG_WIDE(tag)  (LFSR_TAG_WIDE  | LFSR_TAG_##tag)

// some other tag encodings with their own subfields
#define LFSR_TAG_ALT(d, c, key) \
    (LFSR_TAG_ALT \
        | LFSR_TAG_##d \
        | LFSR_TAG_##c \
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

static inline lfsr_tag_t lfsr_tag_shrubmode(lfsr_tag_t tag) {
    return tag & 0xe000;
}

static inline lfsr_tag_t lfsr_tag_shrubkey(lfsr_tag_t tag) {
    return tag & 0x1fff;
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

static inline bool lfsr_tag_iswide(lfsr_tag_t tag) {
    return tag & LFSR_TAG_WIDE;
}

// lfsr_rbyd_appendattr diverged specific flags
static inline bool lfsr_tag_hasdiverged(lfsr_tag_t tag) {
    return tag & LFSR_TAG_DIVERGED;
}

static inline bool lfsr_tag_isdivergedupper(lfsr_tag_t tag) {
    return tag & LFSR_TAG_DIVERGEDUPPER;
}

static inline bool lfsr_tag_isdivergedlower(lfsr_tag_t tag) {
    return !(tag & LFSR_TAG_DIVERGEDUPPER);
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
        lfsr_srid_t lower, lfsr_srid_t upper,
        lfsr_srid_t rid, lfsr_tag_t tag) {
    if (lfsr_tag_isgt(alt)) {
        return rid > upper - (lfsr_srid_t)weight - 1
                || (rid == upper - (lfsr_srid_t)weight - 1
                    && lfsr_tag_key(tag) > lfsr_tag_key(alt));
    } else {
        return rid < lower + (lfsr_srid_t)weight - 1
                || (rid == lower + (lfsr_srid_t)weight - 1
                    && lfsr_tag_key(tag) <= lfsr_tag_key(alt));
    }
}

static inline bool lfsr_tag_follow2(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t lower, lfsr_srid_t upper,
        lfsr_srid_t rid, lfsr_tag_t tag) {
    if (lfsr_tag_isred(alt2) && lfsr_tag_isparallel(alt, alt2)) {
        weight += weight2;
    }

    return lfsr_tag_follow(alt, weight, lower, upper, rid, tag);
}

static inline bool lfsr_tag_prune2(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t lower_rid, lfsr_srid_t upper_rid,
        lfsr_tag_t lower_tag, lfsr_tag_t upper_tag) {
    if (lfsr_tag_isgt(alt)) {
        return lfsr_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_rid, upper_rid,
                lower_rid-1, lower_tag);
    } else {
        return lfsr_tag_follow2(
                alt, weight,
                alt2, weight2,
                lower_rid, upper_rid,
                upper_rid-1, upper_tag-1);
    }
}

static inline void lfsr_tag_flip(
        lfsr_tag_t *alt, lfsr_rid_t *weight,
        lfsr_srid_t lower, lfsr_srid_t upper) {
    *alt = *alt ^ LFSR_TAG_GT;
    *weight = (upper - lower) - *weight;
}

static inline void lfsr_tag_flip2(
        lfsr_tag_t *alt, lfsr_rid_t *weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t lower, lfsr_srid_t upper) {
    if (lfsr_tag_isred(alt2)) {
        *weight += weight2;
    }

    lfsr_tag_flip(alt, weight, lower, upper);
}

static inline void lfsr_tag_trim(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_srid_t *lower_rid, lfsr_srid_t *upper_rid,
        lfsr_tag_t *lower_tag, lfsr_tag_t *upper_tag) {
    if (lfsr_tag_isgt(alt)) {
        *upper_rid -= weight;
        if (upper_tag) {
            *upper_tag = alt + 1;
        }
    } else {
        *lower_rid += weight;
        if (lower_tag) {
            *lower_tag = alt + 1;
        }
    }
}

static inline void lfsr_tag_trim2(
        lfsr_tag_t alt, lfsr_rid_t weight,
        lfsr_tag_t alt2, lfsr_rid_t weight2,
        lfsr_srid_t *lower_rid, lfsr_srid_t *upper_rid,
        lfsr_tag_t *lower_tag, lfsr_tag_t *upper_tag) {
    if (lfsr_tag_isred(alt2)) {
        lfsr_tag_trim(alt2, weight2,
                lower_rid, upper_rid,
                lower_tag, upper_tag);
    }

    lfsr_tag_trim(alt, weight,
            lower_rid, upper_rid,
            lower_tag, upper_tag);
}

// support for encoding/decoding tags on disk

// each piece of metadata in an rbyd tree is prefixed with a 4-piece tag:
//
// - 8-bit suptype     => 1 byte
// - 8-bit subtype     => 1 byte
// - 32-bit rid/weight => 5 byte leb128 (worst case)
// - 32-bit size/jump  => 5 byte leb128 (worst case)
//                     => 12 bytes total
//
#define LFSR_TAG_DSIZE (2+5+5)

static lfs_ssize_t lfsr_bd_readtag(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfs_size_t hint,
        lfsr_tag_t *tag_, lfsr_rid_t *weight_, lfs_size_t *size_,
        uint32_t *cksum_) {
    // read the largest possible tag size
    uint8_t tag_buf[LFSR_TAG_DSIZE];
    lfs_size_t tag_dsize = lfs_min32(LFSR_TAG_DSIZE, lfs->cfg->block_size-off);
    int err = lfsr_bd_read(lfs, block, off, hint, &tag_buf, tag_dsize);
    if (err) {
        return err;
    }

    if (tag_dsize < 2) {
        return LFS_ERR_CORRUPT;
    }
    lfsr_tag_t tag
            = ((lfsr_tag_t)tag_buf[0] << 8)
            | ((lfsr_tag_t)tag_buf[1] << 0);
    lfs_ssize_t d = 2;

    if (cksum_) {
        // on-disk, the tags valid bit must reflect the parity of the
        // preceding data, fortunately for crc32c, this is the same as the
        // parity of the crc
        //
        // note we need to do this before leb128 decoding as we may not have
        // valid leb128 if we're erased, but we shouldn't treat a truncated
        // leb128 here as corruption
        if ((tag >> 15) != (lfs_popc(*cksum_) & 1)) {
            return LFS_ERR_INVAL;
        }
    }

    lfsr_srid_t weight;
    lfs_ssize_t d_ = lfs_fromleb128(&weight, &tag_buf[d], tag_dsize-d);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    lfs_ssize_t size;
    d_ = lfs_fromleb128(&size, &tag_buf[d], tag_dsize-d);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    // optional checksum
    if (cksum_) {
        *cksum_ = lfs_crc32c(*cksum_, tag_buf, d);
    }

    // save what we found, clearing the valid bit from the tag, note we
    // checked this earlier
    *tag_ = tag & 0x7fff;
    *weight_ = weight;
    *size_ = size;
    return d;
}

static lfs_ssize_t lfsr_bd_progtag(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off,
        lfsr_tag_t tag, lfsr_rid_t weight, lfs_size_t size,
        uint32_t *cksum_) {
    // check for underflow issues
    LFS_ASSERT(weight < 0x80000000);
    LFS_ASSERT(size < 0x80000000);

    // make sure to include the parity of the current crc
    tag |= (lfs_popc(*cksum_) & 1) << 15;

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

    d_ = lfs_toleb128(size, &tag_buf[d], 5);
    if (d_ < 0) {
        return d_;
    }
    d += d_;

    int err = lfsr_bd_prog(lfs, block, off, &tag_buf, d,
            cksum_, NULL);
    if (err) {
        return err;
    }

    return d;
}


/// lfsr_data_t stuff ///

// data uses the size's sign bit to indicate on-disk vs in-device
#define LFSR_DATA_ONDISK 0x80000000

// in-device data modes
enum {
    LFSR_DATA_BUF   = 0,
    LFSR_DATA_IMM   = 1,
    LFSR_DATA_CAT   = 2,
};

// LFSR_DATA_DATA just provides and escape hatch to pass raw datas
// through the LFSR_ATTR macro
#define LFSR_DATA_DATA(_data) (_data)

#define LFSR_DATA_NULL() \
    ((lfsr_data_t){ \
        .u.buf.size=0, \
        .u.buf.mode=LFSR_DATA_BUF, \
        .u.buf.buffer=NULL})

#define LFSR_DATA_DISK(_block, _off, _size) \
    ((lfsr_data_t){ \
        .u.disk.size=LFSR_DATA_ONDISK | (_size), \
        .u.disk.block=_block, \
        .u.disk.off=_off})

#define LFSR_DATA_BUF(_buffer, _size) \
    ((lfsr_data_t){ \
        .u.buf.size=_size, \
        .u.buf.mode=LFSR_DATA_BUF, \
        .u.buf.buffer=(const void*)(_buffer)})

#define LFSR_DATA_IMM(_buffer, _size) \
    lfsr_data_fromimm(_buffer, _size)

#define LFSR_DATA_LEB128(_word) \
    lfsr_data_fromleb128(_word)

// this relies on temporary allocations which is a bit precarious...
#define LFSR_DATA_CAT(...) \
    lfsr_data_fromcat( \
        (const lfsr_data_t[]){__VA_ARGS__}, \
        sizeof((const lfsr_data_t[]){__VA_ARGS__}) / sizeof(lfsr_data_t))

// These aren't true runtime-typed datas, but allows some special cases to
// bypass data encoding. External context is required to access these
// correctly.

// a move of all attrs from an mdir entry 
#define LFSR_DATA_MOVE(_mdir) \
    ((lfsr_data_t){.u.buf.buffer=(const void*)(const lfsr_mdir_t*){_mdir}})

// a grm update, note this is mutable! we may update the grm during
// mdir commits
#define LFSR_DATA_GRM(_grm) \
    ((lfsr_data_t){.u.buf.buffer=(const void*)(lfsr_grm_t*){_grm}})

// writing to an unrelated trunk in the rbyd
#define LFSR_DATA_SHRUBCOMMIT(_bshrub, _attrs, _attr_count) \
    ((lfsr_data_t){.u.buf.buffer=(const void*)&(const lfsr_bshrubcommit_t){ \
        .bshrub=_bshrub, \
        .attrs=_attrs, \
        .attr_count=_attr_count}})

#define LFSR_DATA_SHRUBTRUNK(_bshrub) \
    ((lfsr_data_t){.u.buf.buffer=(const void*)(const lfsr_bshrub_t*){_bshrub}})

static inline bool lfsr_data_ondisk(const lfsr_data_t *data) {
    return data->u.size & LFSR_DATA_ONDISK;
}

static inline bool lfsr_data_isbuf(const lfsr_data_t *data) {
    return !lfsr_data_ondisk(data) && data->u.buf.mode == LFSR_DATA_BUF;
}

static inline bool lfsr_data_isimm(const lfsr_data_t *data) {
    return !lfsr_data_ondisk(data) && data->u.buf.mode == LFSR_DATA_IMM;
}

static inline bool lfsr_data_iscat(const lfsr_data_t *data) {
    return !lfsr_data_ondisk(data) && data->u.buf.mode == LFSR_DATA_CAT;
}

static inline lfs_size_t lfsr_data_size(const lfsr_data_t *data) {
    return data->u.size & ~LFSR_DATA_ONDISK;
}

// some data initializers just can't be macros, we at least make these inline
// so most of the internal logic is hopefully elided
static inline lfsr_data_t lfsr_data_fromimm(
        const void *buffer, lfs_size_t size) {
    LFS_ASSERT(size <= 5);

    lfsr_data_t data;
    memcpy(data.u.imm.buf, buffer, size);
    data.u.imm.size = size;
    data.u.imm.mode = LFSR_DATA_IMM;
    return data;
}

static inline lfsr_data_t lfsr_data_fromleb128(int32_t word) {
    lfsr_data_t data;
    lfs_ssize_t size = lfs_toleb128(word, data.u.imm.buf, 5);
    LFS_ASSERT(size >= 0);
    LFS_ASSERT(size <= 5);
    data.u.imm.size = size;
    data.u.imm.mode = LFSR_DATA_IMM;
    return data;
}

static inline lfsr_data_t lfsr_data_fromcat(
        const lfsr_data_t *datas, lfs_size_t count) {
    LFS_ASSERT(count <= 255);

    // find total size
    lfs_size_t size = 0;
    for (uint8_t i = 0; i < count; i++) {
        size += lfsr_data_size(&datas[i]);
    }

    return (lfsr_data_t){
            .u.cat.size=size,
            .u.cat.mode=LFSR_DATA_CAT,
            .u.cat.count=count,
            .u.cat.datas=datas};
}

// note these operations only work on "simple" (not concatenated) datas
static lfsr_data_t lfsr_data_slice(lfsr_data_t data,
        lfs_ssize_t off, lfs_ssize_t size) {
    // limit our off/size to data range, note the use of unsigned casts
    // here to treat -1 as unbounded
    lfs_size_t off_ = lfs_min32(
            lfs_smax32(off, 0),
            lfsr_data_size(&data));
    lfs_size_t size_ = lfs_min32(
            (lfs_size_t)size,
            lfsr_data_size(&data) - off_);

    // on-disk? increment
    if (lfsr_data_ondisk(&data)) {
        data.u.disk.off += off_;
        data.u.disk.size = LFSR_DATA_ONDISK | size_;

    // buffer? increment
    } else if (lfsr_data_isbuf(&data)) {
        data.u.buf.buffer += off_;
        data.u.buf.size = size_;

    // inlined? internal memmove
    } else if (lfsr_data_isimm(&data)) {
        memmove(data.u.imm.buf,
                data.u.imm.buf + off_,
                size_);
        data.u.imm.size = size_;

    // concatenated? not supported
    } else {
        LFS_UNREACHABLE();
    }

    return data;
}

static lfsr_data_t lfsr_data_truncate(lfsr_data_t data, lfs_size_t size) {
    return lfsr_data_slice(data, -1, size);
}

static lfsr_data_t lfsr_data_fruncate(lfsr_data_t data, lfs_size_t size) {
    return lfsr_data_slice(data,
            lfsr_data_size(&data) - lfs_min32(
                size,
                lfsr_data_size(&data)),
            -1);
}


// data <-> bd interactions

// lfsr_data_read* operations update the lfsr_data_t, effectively
// consuming the data

static lfs_ssize_t lfsr_data_read(lfs_t *lfs, lfsr_data_t *data,
        void *buffer, lfs_size_t size) {
    // limit our size to data range
    lfs_size_t d = lfs_min32(size, lfsr_data_size(data));

    // on-disk?
    if (lfsr_data_ondisk(data)) {
        int err = lfsr_bd_read(lfs, data->u.disk.block, data->u.disk.off,
                // note our hint includes the full data range
                lfsr_data_size(data),
                buffer, d);
        if (err) {
            return err;
        }

    // buffer?
    } else if (lfsr_data_isbuf(data)) {
        memcpy(buffer, data->u.buf.buffer, d);

    // inlined?
    } else if (lfsr_data_isimm(data)) {
        memcpy(buffer, data->u.imm.buf, d);

    // concatenated? not supported
    } else {
        LFS_UNREACHABLE();
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

static int lfsr_data_readleb128(lfs_t *lfs, lfsr_data_t *data,
        int32_t *word_) {
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

    *data = lfsr_data_slice(*data, d, -1);
    return 0;
}

static lfs_scmp_t lfsr_data_cmp(lfs_t *lfs, const lfsr_data_t *data,
        const void *buffer, lfs_size_t size) {
    // limit our size to data range
    lfs_size_t d = lfs_min32(size, lfsr_data_size(data));

    // on-disk?
    if (lfsr_data_ondisk(data)) {
        int cmp = lfsr_bd_cmp(lfs, data->u.disk.block, data->u.disk.off, 0,
                buffer, d);
        if (cmp != LFS_CMP_EQ) {
            return cmp;
        }

    // buffer?
    } else if (lfsr_data_isbuf(data)) {
        int cmp = memcmp(data->u.buf.buffer, buffer, d);
        if (cmp < 0) {
            return LFS_CMP_LT;
        } else if (cmp > 0) {
            return LFS_CMP_GT;
        }

    // inlined?
    } else if (lfsr_data_isimm(data)) {
        int cmp = memcmp(data->u.imm.buf, buffer, d);
        if (cmp < 0) {
            return LFS_CMP_LT;
        } else if (cmp > 0) {
            return LFS_CMP_GT;
        }

    // concatenated? not supported
    } else {
        LFS_UNREACHABLE();
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

static lfs_scmp_t lfsr_data_namecmp(lfs_t *lfs, const lfsr_data_t *data,
        lfsr_did_t did, const char *name, lfs_size_t name_size) {
    // first compare the did
    lfsr_data_t data_ = *data;
    lfsr_did_t did_;
    int err = lfsr_data_readleb128(lfs, &data_, (int32_t*)&did_);
    if (err) {
        return err;
    }

    if (did_ < did) {
        return LFS_CMP_LT;
    } else if (did_ > did) {
        return LFS_CMP_GT;
    }

    // then compare the actual name
    return lfsr_data_cmp(lfs, &data_, name, name_size);
}

static int lfsr_bd_progdata_(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfsr_data_t data,
        uint32_t *cksum_, uint32_t *flcksum_) {
    // on-disk?
    if (lfsr_data_ondisk(&data)) {
        // TODO byte-level copies have been a pain point, works for prototyping
        // but can this be better? configurable? leverage
        // rcache/pcache directly?
        uint8_t dat;
        for (lfs_size_t i = 0; i < lfsr_data_size(&data); i++) {
            int err = lfsr_bd_read(lfs,
                    data.u.disk.block, data.u.disk.off+i,
                    lfsr_data_size(&data)-i,
                    &dat, 1);
            if (err) {
                return err;
            }

            err = lfsr_bd_prog(lfs, block, off+i, &dat, 1,
                    cksum_, flcksum_);
            if (err) {
                return err;
            }
        }

    // buffer?
    } else if (lfsr_data_isbuf(&data)) {
        int err = lfsr_bd_prog(lfs, block, off,
                data.u.buf.buffer, data.u.buf.size,
                cksum_, flcksum_);
        if (err) {
            return err;
        }

    // inlined?
    } else if (lfsr_data_isimm(&data)) {
        int err = lfsr_bd_prog(lfs, block, off,
                data.u.imm.buf, data.u.imm.size,
                cksum_, flcksum_);
        if (err) {
            return err;
        }

    // concatenated? nesting would require recursion
    } else {
        LFS_UNREACHABLE();
    }

    return 0;
}

static int lfsr_bd_progdata(lfs_t *lfs,
        lfs_block_t block, lfs_size_t off, lfsr_data_t data,
        uint32_t *cksum_, uint32_t *flcksum_) {
    // simple data?
    if (!lfsr_data_iscat(&data)) {
        int err = lfsr_bd_progdata_(lfs, block, off, data,
                cksum_, flcksum_);
        if (err) {
            return err;
        }

    // concatenated data? handle specially to avoid recursion
    } else {
        for (uint8_t i = 0; i < data.u.cat.count; i++) {
            int err = lfsr_bd_progdata_(lfs, block, off, data.u.cat.datas[i],
                    cksum_, flcksum_);
            if (err) {
                return err;
            }

            off += lfsr_data_size(&data.u.cat.datas[i]);
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
    lfsr_srid_t rid;
    lfsr_tag_t tag;
    lfsr_srid_t delta;
    lfsr_data_t data;
} lfsr_attr_t;

#define LFSR_ATTR(_rid, _type, _delta, _data) \
    ((const lfsr_attr_t){ \
        _rid, \
        LFSR_TAG_##_type, \
        _delta, \
        LFSR_DATA_##_data})

// TODO do we really need two?
#define LFSR_ATTR_(_rid, _type, _delta, _data) \
    ((const lfsr_attr_t){ \
        _rid, \
        LFSR_TAG_##_type, \
        _delta, \
        LFSR_DATA_##_data})

#define LFSR_ATTR_NOOP() LFSR_ATTR(-1, GROW, 0, NULL())

// TODO make this const again eventually
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


// erased-state checksum stuff

static int lfsr_ecksum_validate(lfs_t *lfs, const lfsr_ecksum_t *ecksum,
        lfs_block_t block, lfs_size_t off) {
    LFS_ASSERT(ecksum->size != -1);
    LFS_ASSERT(off < lfs->cfg->block_size);

    // check that erased-state matches our checksum, if this fails
    // most likely a write was interrupted
    uint32_t cksum_ = 0;
    int err = lfsr_bd_cksum(lfs, block, off, 0, ecksum->size,
            &cksum_);
    if (err) {
        return err;
    }

    // ecksum mismatch?
    if (cksum_ != ecksum->cksum) {
        return LFS_ERR_CORRUPT;
    }

    return 0;
}

// erased-state checksum on-disk encoding

// 1 leb128 + 1 crc32c => 9 bytes (worst case)
#define LFSR_ECKSUM_DSIZE (5+4)

#define LFSR_DATA_FROMECKSUM(_ecksum, _buffer) \
    lfsr_data_fromecksum(_ecksum, _buffer)

static lfsr_data_t lfsr_data_fromecksum(const lfsr_ecksum_t *ecksum,
        uint8_t buffer[static LFSR_ECKSUM_DSIZE]) {
    LFS_ASSERT(ecksum->size != -1);
    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfs_toleb128(ecksum->size, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    lfs_tole32_(ecksum->cksum, &buffer[d]);
    d += 4;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readecksum(lfs_t *lfs, lfsr_data_t *data,
        lfsr_ecksum_t *ecksum) {
    int err = lfsr_data_readleb128(lfs, data, (int32_t*)&ecksum->size);
    if (err) {
        return err;
    }

    err = lfsr_data_readle32(lfs, data, &ecksum->cksum);
    if (err) {
        return err;
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


/// Global-state things ///

static inline bool lfsr_gdelta_iszero(
        const uint8_t *gdelta, lfs_size_t size) {
    // this condition is probably optimized out by constant propagation
    if (size == 0) {
        return true;
    }

    // check that gdelta is all zeros
    return gdelta[0] == 0 && memcmp(&gdelta[0], &gdelta[1], size-1) == 0;
}

static inline lfs_size_t lfsr_gdelta_size(
        const uint8_t *gdelta, lfs_size_t size) {
    // truncate based on number of trailing zeros
    while (size > 0 && gdelta[size-1] == 0) {
        size -= 1;
    }

    return size;
}

static int lfsr_gdelta_xor(lfs_t *lfs, 
        uint8_t *gdelta, lfs_size_t size,
        lfsr_data_t xor) {
    // check for overflow
    lfs_size_t xor_size = lfsr_data_size(&xor);
    LFS_ASSERT(xor_size <= size);
    if (xor_size > size) {
        return LFS_ERR_CORRUPT;
    }

    // TODO is there a way to avoid byte-level operations here?
    // xor with data, this should at least be cached if on-disk
    for (lfs_size_t i = 0; i < xor_size; i++) {
        uint8_t x;
        lfs_ssize_t d = lfsr_data_read(lfs, &xor, &x, 1);
        if (d < 0) {
            return d;
        }

        gdelta[i] ^= x;
    }

    return 0;
}


// grm (global remove) things
static inline bool lfsr_grm_hasrm(const lfsr_grm_t *grm) {
    return grm->rms[0] != -1;
}

static inline uint8_t lfsr_grm_count(const lfsr_grm_t *grm) {
    return (grm->rms[0] != -1) + (grm->rms[1] != -1);
}

static inline void lfsr_grm_pushrm(lfsr_grm_t *grm, lfsr_smid_t mid) {
    LFS_ASSERT(grm->rms[1] == -1);
    grm->rms[1] = grm->rms[0];
    grm->rms[0] = mid;
}

static inline void lfsr_grm_poprm(lfsr_grm_t *grm) {
    grm->rms[0] = grm->rms[1];
    grm->rms[1] = -1;
}

static inline bool lfsr_grm_iszero(
        const uint8_t gdelta[static LFSR_GRM_DSIZE]) {
    return lfsr_gdelta_iszero(gdelta, LFSR_GRM_DSIZE);
}

static inline lfs_size_t lfsr_grm_size(
        const uint8_t gdelta[static LFSR_GRM_DSIZE]) {
    return lfsr_gdelta_size(gdelta, LFSR_GRM_DSIZE);
}

static inline int lfsr_grm_xor(lfs_t *lfs,
        uint8_t gdelta[static LFSR_GRM_DSIZE],
        lfsr_data_t xor) {
    return lfsr_gdelta_xor(lfs, gdelta, LFSR_GRM_DSIZE, xor);
}

#define LFSR_DATA_FROMGRM(_grm, _buffer) \
    lfsr_data_fromgrm(_grm, _buffer)

static lfsr_data_t lfsr_data_fromgrm(const lfsr_grm_t *grm,
        uint8_t buffer[static LFSR_GRM_DSIZE]) {
    // make sure to zero so we don't leak any info
    memset(buffer, 0, LFSR_GRM_DSIZE);

    // first encode the number of grms, this can be 0, 1, or 2 and may
    // be extended to a general purpose leb128 type field in the future
    uint8_t mode = lfsr_grm_count(grm);
    lfs_ssize_t d = 0;
    buffer[d] = mode;
    d += 1;

    for (uint8_t i = 0; i < mode; i++) {
        lfs_ssize_t d_ = lfs_toleb128(grm->rms[i], &buffer[d], 5);
        LFS_ASSERT(d_ >= 0);
        d += d_;
    }

    return LFSR_DATA_BUF(buffer, lfsr_grm_size(buffer));
}

// required by lfsr_data_readgrm
static inline lfsr_mid_t lfsr_mweight(lfs_t *lfs);
static inline lfsr_mid_t lfsr_mtree_weight(lfs_t *lfs);

static int lfsr_data_readgrm(lfs_t *lfs, lfsr_data_t *data,
        lfsr_grm_t *grm) {
    // clear first
    grm->rms[0] = -1;
    grm->rms[1] = -1;

    // first read the mode field
    lfs_size_t mode;
    int err = lfsr_data_readleb128(lfs, data, (int32_t*)&mode);
    if (err) {
        return err;
    }

    // unknown mode? return an error, we may be able to mount read-only
    if (mode > 2) {
        return LFS_ERR_INVAL;
    }

    for (uint8_t i = 0; i < mode; i++) {
        err = lfsr_data_readleb128(lfs, data, &grm->rms[i]);
        if (err) {
            return err;
        }

        LFS_ASSERT(grm->rms[i] < lfs_smax32(
                lfsr_mtree_weight(lfs),
                lfsr_mweight(lfs)));
    }

    return 0;
}


// trunk on-disk encoding

// 2 leb128s => 10 bytes (worst case)
#define LFSR_TRUNK_DSIZE (5+5)

#define LFSR_DATA_FROMTRUNK(_rbyd, _buffer) \
    lfsr_data_fromtrunk(_rbyd, _buffer)

static lfsr_data_t lfsr_data_fromtrunk(lfs_size_t trunk, lfsr_rid_t weight,
        uint8_t buffer[static LFSR_TRUNK_DSIZE]) {
    // shrub trunks should never be null
    LFS_ASSERT(trunk != 0);
    lfs_ssize_t d = 0;

    // just write the trunk and weight, the rest of the rbyd is contextual
    lfs_ssize_t d_ = lfs_toleb128(weight, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(trunk, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readtrunk(lfs_t *lfs, lfsr_data_t *data,
        lfs_size_t *trunk, lfsr_rid_t *weight) {
    // note the rest of the rbyd may not actually be backed by memory, so
    // we need to be conservative here
    int err = lfsr_data_readleb128(lfs, data, (int32_t*)weight);
    if (err) {
        return err;
    }

    err = lfsr_data_readleb128(lfs, data, (int32_t*)trunk);
    if (err) {
        return err;
    }

    // shrub trunks should never be null
    LFS_ASSERT(*trunk != 0);
    return 0;
}

// other shrub things
typedef struct lfsr_bshrubcommit_t {
    lfsr_bshrub_t *bshrub;
    const lfsr_attr_t *attrs;
    lfs_size_t attr_count;
} lfsr_bshrubcommit_t;


// block pointer things

// 4 leb128s + 1 crc32c => 24 bytes (worst case)
#define LFSR_BPTR_DSIZE (5+5+5+5+4)

#define LFSR_DATA_FROMBPTR(_bptr, _buffer) \
    lfsr_data_frombptr(_bptr, _buffer)

static lfsr_data_t lfsr_data_frombptr(const lfsr_bptr_t *bptr,
        uint8_t buffer[static LFSR_BPTR_DSIZE]) {
    lfs_ssize_t d = 0;

    // write the block, offset, size
    lfs_ssize_t d_ = lfs_toleb128(lfsr_data_size(&bptr->data), &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(bptr->data.u.disk.block, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(bptr->data.u.disk.off, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    // write the cksize, cksum
    d_ = lfs_toleb128(bptr->cksize, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    lfs_tole32_(bptr->cksum, &buffer[d]);
    d += 4;

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readbptr(lfs_t *lfs, lfsr_data_t *data,
        lfsr_bptr_t *bptr) {
    // read the block, offset, size
    int err = lfsr_data_readleb128(lfs, data,
            (int32_t*)&bptr->data.u.disk.size);
    if (err) {
        return err;
    }

    err = lfsr_data_readleb128(lfs, data, (int32_t*)&bptr->data.u.disk.block);
    if (err) {
        return err;
    }

    err = lfsr_data_readleb128(lfs, data, (int32_t*)&bptr->data.u.disk.off);
    if (err) {
        return err;
    }

    // read the cksize, cksum
    err = lfsr_data_readleb128(lfs, data, (int32_t*)&bptr->cksize);
    if (err) {
        return err;
    }

    err = lfsr_data_readle32(lfs, data, &bptr->cksum);
    if (err) {
        return err;
    }

    // all bptrs have this flag set, this is used to differentiate
    // bptrs from btrees in files
    bptr->data.u.disk.size |= LFSR_DATA_ONDISK;
    return 0;
}




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
static void lfs_alloc_ckpoint(lfs_t *lfs);


/// Red-black-yellow Dhara tree operations ///

// helper functions
static inline bool lfsr_rbyd_isfetched(const lfsr_rbyd_t *rbyd) {
    return !(rbyd->eoff == 0 && rbyd->trunk > 0);
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
    int err = lfs_alloc(lfs, &rbyd->blocks[0]);
    if (err) {
        return err;
    }
            
    // TODO should erase be implicit in alloc eventually?
    err = lfsr_bd_erase(lfs, rbyd->blocks[0]);
    if (err) {
        return err;
    }

    return 0;
}

static int lfsr_rbyd_fetch(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfs_block_t block, lfs_size_t trunk) {
    // checksum the revision count to get the cksum started
    uint32_t cksum = 0;
    int err = lfsr_bd_cksum(lfs, block, 0, lfs->cfg->block_size,
            sizeof(uint32_t), &cksum);
    if (err) {
        return err;
    }

    rbyd->blocks[0] = block;
    rbyd->eoff = 0;
    rbyd->trunk = 0;

    // temporary state until we validate a cksum
    lfs_size_t off = sizeof(uint32_t);
    lfs_size_t trunk_ = 0;
    lfs_size_t trunk__ = 0;
    bool wastrunk = false;
    lfsr_rid_t weight = 0;
    lfsr_rid_t weight_ = 0;

    // assume unerased until proven otherwise
    lfsr_ecksum_t ecksum = {.size=-1};

    // scan tags, checking valid bits, cksums, etc
    while (off < lfs->cfg->block_size && (!trunk || rbyd->eoff <= trunk)) {
        lfsr_tag_t tag;
        lfsr_rid_t weight__;
        lfs_size_t size;
        lfs_ssize_t d = lfsr_bd_readtag(lfs,
                block, off, lfs->cfg->block_size,
                &tag, &weight__, &size, &cksum);
        if (d < 0) {
            if (d == LFS_ERR_INVAL || d == LFS_ERR_CORRUPT) {
                // if we are breaking for any reason other than the tag's
                // valid bit, our ecksum must be invalid
                if (d != LFS_ERR_INVAL) {
                    ecksum.size = -1;
                }
                break;
            }
            return d;
        }
        lfs_size_t off_ = off + d;

        // tag goes out of range?
        if (!lfsr_tag_isalt(tag) && off_ + size > lfs->cfg->block_size) {
            break;
        }

        // not an end-of-commit cksum
        if (!lfsr_tag_isalt(tag) && lfsr_tag_suptype(tag) != LFSR_TAG_CKSUM) {
            // cksum the entry, hopefully leaving it in the cache
            err = lfsr_bd_cksum(lfs, block, off_, lfs->cfg->block_size, size,
                    &cksum);
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
                            lfs->cfg->block_size - off_),
                        &ecksum);
                if (err && err != LFS_ERR_CORRUPT) {
                    return err;
                }

                // TODO ignore?? why not break?
                // ignore malformed ecksums
                if (err == LFS_ERR_CORRUPT) {
                    ecksum.size = -1;
                }
            }

        // is an end-of-commit cksum
        } else if (!lfsr_tag_isalt(tag)) {
            uint32_t cksum_ = 0;
            err = lfsr_bd_read(lfs, block, off_, lfs->cfg->block_size,
                    &cksum_, sizeof(uint32_t));
            if (err) {
                if (err == LFS_ERR_CORRUPT) {
                    break;
                }
                return err;
            }
            cksum_ = lfs_fromle32_(&cksum_);

            if (cksum != cksum_) {
                // uh oh, cksums don't match
                break;
            }

            // toss our cksum into the filesystem seed for
            // pseudorandom numbers, note we use another cksum here
            // as a collection function because it is sufficiently
            // random and convenient
            lfs->seed = lfs_crc32c(lfs->seed, &cksum, sizeof(uint32_t));

            // save what we've found so far
            rbyd->eoff = off_ + size;
            rbyd->cksum = cksum;
            rbyd->trunk = trunk_;
            rbyd->weight = weight;
        }

        // found a trunk of a tree?
        if (lfsr_tag_istrunk(tag) && (!trunk || trunk >= off || wastrunk)) {
            // start of trunk?
            if (!wastrunk) {
                wastrunk = true;
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
                wastrunk = false;
                // update most recent trunk and weight, unless we are a
                // shrub trunk
                if (!lfsr_tag_isshrub(tag)) {
                    trunk_ = trunk__;
                    weight = weight_;
                }
            }
        }

        // skip data
        if (!lfsr_tag_isalt(tag)) {
            off_ += size;
        }

        off = off_;
    }

    // no valid commits?
    if (!rbyd->trunk) {
        return LFS_ERR_CORRUPT;
    }

    // did we end on a valid commit? we may have an erased state
    bool erased = false;
    if (rbyd->eoff < lfs->cfg->block_size
            && rbyd->eoff % lfs->cfg->prog_size == 0
            && ecksum.size != -1) {
        err = lfsr_ecksum_validate(lfs, &ecksum, rbyd->blocks[0], rbyd->eoff);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        erased = (err != LFS_ERR_CORRUPT);
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
                    "cksum 0x%08"PRIx32,
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
                "cksum 0x%08"PRIx32" (!= 0x%08"PRIx32")",
                rbyd->blocks[0], rbyd->trunk, rbyd->cksum, cksum);
        return LFS_ERR_CORRUPT;
    }

    // if trunk/weight mismatch _after_ cksums match, that's not a storage
    // error, that's a programming error
    LFS_ASSERT(rbyd->trunk == trunk);
    LFS_ASSERT((lfsr_rid_t)rbyd->weight == weight);
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

    // keep track of bounds as we descend down the tree
    lfs_size_t branch = rbyd->trunk;
    lfsr_srid_t lower = 0;
    lfsr_srid_t upper = rbyd->weight;

    // no trunk yet?
    if (!branch) {
        return LFS_ERR_NOENT;
    }

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
            if (lfsr_tag_follow(alt, weight, lower, upper, rid, tag)) {
                lfsr_tag_flip(&alt, &weight, lower, upper);
                lfsr_tag_trim(alt, weight, &lower, &upper, NULL, NULL);
                branch = branch - jump;
            } else {
                lfsr_tag_trim(alt, weight, &lower, &upper, NULL, NULL);
                branch = branch + d;
            }

        // found end of tree?
        } else {
            // update the tag rid
            LFS_ASSERT(lfsr_tag_shrubmode(alt) == 0x0000);
            lfsr_srid_t rid__ = upper-1;
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
                *weight_ = upper - lower;
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

static int lfsr_rbyd_lookupwide(lfs_t *lfs, const lfsr_rbyd_t *rbyd,
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

    // the difference between lookup and lookupwide is we accept any
    // subtype of the requested tag
    if (rid_ != rid || lfsr_tag_suptype(tag__) != tag) {
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

    // revision count stored as le32, we don't use a leb128 encoding as we
    // intentionally allow the revision count to overflow
    uint8_t rev_buf[sizeof(uint32_t)];
    lfs_tole32_(rev, &rev_buf);
    int err = lfsr_bd_prog(lfs, rbyd->blocks[0], rbyd->eoff,
            &rev_buf, sizeof(uint32_t),
            &rbyd->cksum, NULL);
    if (err) {
        return err;
    }
    rbyd->eoff += sizeof(uint32_t);

    return 0;
}

// helper functions for managing the 3-element fifo used in
// lfsr_rbyd_appendattr
static int lfsr_rbyd_p_flush(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_tag_t p_alts[static 3],
        lfsr_rid_t p_weights[static 3],
        lfs_size_t p_jumps[static 3],
        int count) {
    // write out some number of alt pointers in our queue
    for (int i = 0; i < count; i++) {
        if (p_alts[3-1-i]) {
            // change to a relative jump at the last minute
            lfsr_tag_t alt = p_alts[3-1-i];
            lfsr_rid_t weight = p_weights[3-1-i];
            lfs_size_t jump = rbyd->eoff - p_jumps[3-1-i];

            lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->blocks[0], rbyd->eoff,
                    alt, weight, jump,
                    &rbyd->cksum);
            if (d < 0) {
                return d;
            }
            rbyd->eoff += d;
        }
    }

    return 0;
}

static inline int lfsr_rbyd_p_push(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_tag_t p_alts[static 3],
        lfsr_rid_t p_weights[static 3],
        lfs_size_t p_jumps[static 3],
        lfsr_tag_t alt, lfsr_srid_t weight, lfs_size_t jump) {
    int err = lfsr_rbyd_p_flush(lfs, rbyd, p_alts, p_weights, p_jumps, 1);
    if (err) {
        return err;
    }

    memmove(p_alts+1, p_alts, 2*sizeof(lfsr_tag_t));
    memmove(p_weights+1, p_weights, 2*sizeof(lfsr_rid_t));
    memmove(p_jumps+1, p_jumps, 2*sizeof(lfs_size_t));
    p_alts[0] = alt;
    p_weights[0] = weight;
    p_jumps[0] = jump;

    return 0;
}

static inline void lfsr_rbyd_p_pop(
        lfsr_tag_t p_alts[static 3],
        lfsr_rid_t p_weights[static 3],
        lfs_size_t p_jumps[static 3]) {
    memmove(p_alts, p_alts+1, 2*sizeof(lfsr_tag_t));
    memmove(p_weights, p_weights+1, 2*sizeof(lfsr_rid_t));
    memmove(p_jumps, p_jumps+1, 2*sizeof(lfs_size_t));
    p_alts[2] = 0;
    p_weights[2] = 0;
    p_jumps[2] = 0;
}

static void lfsr_rbyd_p_red(
        lfsr_tag_t p_alts[static 3],
        lfsr_rid_t p_weights[static 3],
        lfs_size_t p_jumps[static 3]) {
    // propagate a red edge upwards
    p_alts[0] &= ~LFSR_TAG_R;

    if (p_alts[1]) {
        p_alts[1] |= LFSR_TAG_R;

        // reorder so that top two edges always go in the same direction
        if (lfsr_tag_isred(p_alts[2])) {
            if (lfsr_tag_isparallel(p_alts[1], p_alts[2])) {
                // no reorder needed
            } else if (lfsr_tag_isparallel(p_alts[0], p_alts[2])) {
                lfsr_tag_t alt_ = p_alts[1];
                lfsr_rid_t weight_ = p_weights[1];
                lfs_size_t jump_ = p_jumps[1];
                p_alts[1] = p_alts[0] | LFSR_TAG_R;
                p_weights[1] = p_weights[0];
                p_jumps[1] = p_jumps[0];
                p_alts[0] = alt_ & ~LFSR_TAG_R;
                p_weights[0] = weight_;
                p_jumps[0] = jump_;
            } else if (lfsr_tag_isparallel(p_alts[0], p_alts[1])) {
                lfsr_tag_t alt_ = p_alts[2];
                lfsr_rid_t weight_ = p_weights[2];
                lfs_size_t jump_ = p_jumps[2];
                p_alts[2] = p_alts[1] | LFSR_TAG_R;
                p_weights[2] = p_weights[1];
                p_jumps[2] = p_jumps[1];
                p_alts[1] = p_alts[0] | LFSR_TAG_R;
                p_weights[1] = p_weights[0];
                p_jumps[1] = p_jumps[0];
                p_alts[0] = alt_ & ~LFSR_TAG_R;
                p_weights[0] = weight_;
                p_jumps[0] = jump_;
            } else {
                LFS_UNREACHABLE();
            }
        }
    }
}

// core rbyd algorithm
static int lfsr_rbyd_appendattr(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t rid, lfsr_tag_t tag, lfsr_srid_t delta, lfsr_data_t data) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));
    // tag must not be internal at this point
    LFS_ASSERT(!lfsr_tag_isinternal(tag));
    // there shouldn't be any null tags here
    LFS_ASSERT(tag != 0);
    // reserve bit 7 to allow leb128 subtypes in the future
    LFS_ASSERT(!(tag & 0x80));

    // we can't do anything if we're not erased
    if (rbyd->eoff >= lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    // ignore noops
    // TODO is there a better way to represent noops?
    if (!lfsr_tag_iswide(tag) && !lfsr_tag_key(tag) && delta == 0) {
        return 0;
    }

    // make sure every rbyd starts with a revision count
    if (rbyd->eoff == 0) {
        int err = lfsr_rbyd_appendrev(lfs, rbyd, 0);
        if (err) {
            return err;
        }
    }

    // figure out the range of tags we're operating on
    //
    // several lower bits are reserved, so we repurpose these
    // to keep track of some append state
    lfsr_srid_t rid_;
    lfsr_srid_t other_rid_;
    lfsr_tag_t tag_;
    lfsr_tag_t other_tag_;
    if (delta != 0 && !lfsr_tag_isgrow(tag)) {
        LFS_ASSERT(!lfsr_tag_iswide(tag));

        if (delta > 0) {
            LFS_ASSERT(rid <= rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid -= 1;
            rid_ = rid + 1;
            other_rid_ = rid + 1;
        } else {
            LFS_ASSERT(rid < rbyd->weight);

            // it's a bit ugly, but adjusting the rid here makes the following
            // logic work out more consistently
            rid += 1;
            rid_ = rid - lfs_smax32(-delta, 0);
            other_rid_ = rid;
        }

        // note these tags MUST NOT be zero, due to unreachable tag holes
        tag_ = 0x1;
        other_tag_ = tag_;

    } else {
        LFS_ASSERT(rid < rbyd->weight);

        rid_ = rid - lfs_smax32(-delta, 0);
        other_rid_ = rid;

        // note both normal and rm wide-tags have the same bounds, really it's
        // the normal non-wide-tags that are an outlier here
        if (lfsr_tag_iswide(tag)) {
            tag_ = lfsr_tag_supkey(tag);
            other_tag_ = tag_ + 0x100;
        } else if (lfsr_tag_isrm(tag) || !lfsr_tag_key(tag)) {
            tag_ = lfsr_tag_key(tag);
            other_tag_ = tag_ + 0x1;
        } else {
            tag_ = lfsr_tag_key(tag);
            other_tag_ = tag_;
        }
    }
    // mark as rmed until found
    tag_ |= LFSR_TAG_RM;
    other_tag_ |= LFSR_TAG_RM;

    // keep track of bounds as we descend down the tree
    //
    // this gets a bit confusing as we also may need to keep
    // track of both the lower and upper bounds of diverging paths
    // in the case of range deletions
    lfs_size_t branch = rbyd->trunk;
    lfsr_srid_t lower_rid = 0;
    lfsr_srid_t upper_rid = rbyd->weight;
    lfsr_tag_t lower_tag = 0;
    lfsr_tag_t upper_tag = 0xffff;

    // diverged state in case we are removing a range from the tree
    //
    // this is a second copy of the search path state, used to keep track
    // of two search paths simulaneously when our range diverges.
    //
    // note we can't just perform two searches sequentially, or else our tree
    // will end up very unbalanced.
    lfs_size_t other_branch = 0;
    lfsr_srid_t other_lower_rid = 0;
    lfsr_srid_t other_upper_rid = 0;
    lfsr_tag_t other_lower_tag = 0;
    lfsr_tag_t other_upper_tag = 0;

    // go ahead and update the rbyd's weight, if an error occurs our
    // rbyd is no longer usable anyways
    LFS_ASSERT(delta >= -rbyd->weight);
    rbyd->weight += delta;

    // assume we'll update our trunk
    rbyd->trunk = rbyd->eoff;

    // no trunk yet?
    if (!branch) {
        goto leaf;
    }

    // queue of pending alts we can emulate rotations with
    lfsr_tag_t p_alts[3] = {0, 0, 0};
    lfsr_rid_t p_weights[3] = {0, 0, 0};
    lfs_size_t p_jumps[3] = {0, 0, 0};
    lfs_size_t graft = 0;

    // descend down tree, building alt pointers
    while (true) {
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

            // do bounds want to take different paths? begin cutting
            if (!lfsr_tag_hasdiverged(tag_)
                    && lfsr_tag_follow2(alt, weight,
                            p_alts[0], p_weights[0],
                            lower_rid, upper_rid,
                            rid_, tag_)
                        != lfsr_tag_follow2(alt, weight,
                            p_alts[0], p_weights[0],
                            lower_rid, upper_rid,
                            other_rid_, other_tag_)) {
                // first take care of any lingering red alts
                if (lfsr_tag_isred(p_alts[0])) {
                    alt = p_alts[0] & ~LFSR_TAG_R;
                    weight = p_weights[0];
                    jump = p_jumps[0];
                    branch_ = branch;
                    lfsr_rbyd_p_pop(p_alts, p_weights, p_jumps);
                } else {
                    tag_ |= LFSR_TAG_DIVERGED | LFSR_TAG_DIVERGEDLOWER;
                    other_tag_ |= LFSR_TAG_DIVERGED | LFSR_TAG_DIVERGEDUPPER;
                    other_branch = branch;
                    other_lower_rid = lower_rid;
                    other_upper_rid = upper_rid;
                    other_lower_tag = lower_tag;
                    other_upper_tag = upper_tag;
                }
            }

            // if we're diverging, go ahead and make alt black, this isn't
            // perfect but it's simpler and compact will take care of any
            // balance issues that may occur
            if (lfsr_tag_hasdiverged(tag_)) {
                alt &= ~LFSR_TAG_R;
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
                    lower_rid, upper_rid,
                    lower_tag, upper_tag)) {
                if (lfsr_tag_isred(p_alts[0])) {
                    alt = p_alts[0] & ~LFSR_TAG_R;
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
                        lower_rid, upper_rid,
                        rid_, tag_)) {
                    lfsr_tag_flip2(&alt, &weight,
                            p_alts[0], p_weights[0],
                            lower_rid, upper_rid);
                    lfs_swap32(&jump, &branch_);

                    lfs_swap16(&p_alts[0], &alt);
                    lfs_swap32(&p_weights[0], &weight);
                    lfs_swap32(&p_jumps[0], &jump);
                    alt &= ~LFSR_TAG_R;

                    lfsr_tag_trim(
                            p_alts[0], p_weights[0],
                            &lower_rid, &upper_rid,
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
                            &lower_rid, &upper_rid,
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
                        lower_rid, upper_rid,
                        rid_, tag_)) {
                lfsr_tag_flip2(&alt, &weight,
                        p_alts[0], p_weights[0],
                        lower_rid, upper_rid);
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
                        lower_rid, upper_rid,
                        rid_, tag_)) {
                lfs_swap16(&p_alts[0], &alt);
                lfs_swap32(&p_weights[0], &weight);
                lfs_swap32(&p_jumps[0], &jump);
                p_alts[0] |= LFSR_TAG_R;
                alt &= ~LFSR_TAG_R;

                lfsr_tag_flip2(&alt, &weight,
                        p_alts[0], p_weights[0],
                        lower_rid, upper_rid);
                lfs_swap32(&jump, &branch_);
            }

            // trim alt from our current bounds
            if (lfsr_tag_isblack(alt)) {
                lfsr_tag_trim2(
                        alt, weight,
                        p_alts[0], p_weights[0],
                        &lower_rid, &upper_rid,
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
            int err = lfsr_rbyd_p_push(lfs, rbyd,
                    p_alts, p_weights, p_jumps,
                    alt, weight, jump);
            if (err) {
                return err;
            }

        // found end of tree?
        } else {
            // update the found tag/rid
            //
            // note we:
            // - clear valid bit, marking the tag as found
            // - preserve diverged state
            LFS_ASSERT(lfsr_tag_shrubmode(alt) == 0x0000);
            tag_ = lfsr_tag_mode(tag_ & ~LFSR_TAG_RM) | alt;

            // done?
            if (!lfsr_tag_hasdiverged(tag_) || !lfsr_tag_isrm(other_tag_)) {
                break;
            }
        }

        // switch to the other path if we have diverged
        if (lfsr_tag_hasdiverged(tag_) || !lfsr_tag_isalt(alt)) {
            lfs_swap16(&tag_, &other_tag_);
            lfs_sswap32(&rid_, &other_rid_);
            lfs_swap32(&branch, &other_branch);
            lfs_sswap32(&lower_rid, &other_lower_rid);
            lfs_sswap32(&upper_rid, &other_upper_rid);
            lfs_swap16(&lower_tag, &other_lower_tag);
            lfs_swap16(&upper_tag, &other_upper_tag);
        }
    }

    // the last alt should always end up black
    LFS_ASSERT(lfsr_tag_isblack(p_alts[0]));

    // if we diverged, merge the bounds
    LFS_ASSERT(!lfsr_tag_isrm(tag_));
    LFS_ASSERT(!lfsr_tag_hasdiverged(tag_) || !lfsr_tag_isrm(other_tag_));
    if (lfsr_tag_hasdiverged(tag_)) {
        if (lfsr_tag_isdivergedlower(tag_)) {
            // finished on lower path
            tag_ = other_tag_;
            branch = other_branch;
            upper_rid = other_upper_rid;
        } else {
            // finished on upper path
            lower_rid = other_lower_rid;
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
    lfsr_rid_t weight = 0;
    if (lfsr_tag_key(tag_)
            && (upper_rid-1 < rid-lfs_smax32(-delta, 0)
                || (upper_rid-1 == rid-lfs_smax32(-delta, 0)
                    && ((delta > 0 && !lfsr_tag_isgrow(tag))
                        || (lfsr_tag_iswide(tag)
                            ? lfsr_tag_supkey(tag_) < lfsr_tag_supkey(tag)
                            : lfsr_tag_key(tag_) < lfsr_tag_key(tag)))))) {
        if (lfsr_tag_isrm(tag) || !lfsr_tag_key(tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALT(GT, B, 0);
            weight = upper_rid - lower_rid + delta;
            upper_rid -= weight;
        } else {
            // split less than
            alt = LFSR_TAG_ALT(
                    LE,
                    TAG(!lfsr_tag_hasdiverged(tag_)
                        ? LFSR_TAG_R
                        : LFSR_TAG_B),
                    lfsr_tag_key(tag_));
            weight = upper_rid - lower_rid;
            lower_rid += weight;
        }

    } else if (lfsr_tag_key(tag_)
            && (upper_rid-1 > rid
                || (upper_rid-1 == rid
                    && ((delta > 0 && !lfsr_tag_isgrow(tag))
                        || (lfsr_tag_iswide(tag)
                            ? lfsr_tag_supkey(tag_) > lfsr_tag_supkey(tag)
                            : lfsr_tag_key(tag_) > lfsr_tag_key(tag)))))) {
        if (lfsr_tag_isrm(tag) || !lfsr_tag_key(tag)) {
            // if removed, make our tag unreachable
            alt = LFSR_TAG_ALT(GT, B, 0);
            weight = upper_rid - lower_rid + delta;
            upper_rid -= weight;
        } else {
            // split greater than
            alt = LFSR_TAG_ALT(
                    GT,
                    TAG(!lfsr_tag_hasdiverged(tag_)
                        ? LFSR_TAG_R
                        : LFSR_TAG_B),
                    lfsr_tag_key(tag));
            weight = upper_rid - (rid+1);
            upper_rid -= weight;
        }
    }

    if (alt) {
        int err = lfsr_rbyd_p_push(lfs, rbyd,
                p_alts, p_weights, p_jumps,
                alt, weight, branch);
        if (err) {
            return err;
        }

        if (lfsr_tag_isred(p_alts[0])) {
            // introduce a red edge
            lfsr_rbyd_p_red(p_alts, p_weights, p_jumps);
        }
    }

    // flush any pending alts
    int err = lfsr_rbyd_p_flush(lfs, rbyd,
            p_alts, p_weights, p_jumps, 3);
    if (err) {
        return err;
    }

leaf:;
    // write the actual tag
    //
    // note we always need a non-alt to terminate the trunk, otherwise we
    // can't find trunks during fetch
    lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->blocks[0], rbyd->eoff,
            // rm => null or shrubnull, otherwise strip off control bits
            (lfsr_tag_isrm(tag)
                ? lfsr_tag_mode(lfsr_tag_shrubkey(tag))
                : lfsr_tag_shrubkey(tag)),
            upper_rid - lower_rid + delta,
            lfsr_data_size(&data),
            &rbyd->cksum);
    if (d < 0) {
        return d;
    }
    rbyd->eoff += d;

    // don't forget the data!
    err = lfsr_bd_progdata(lfs, rbyd->blocks[0], rbyd->eoff, data,
            &rbyd->cksum, NULL);
    if (err) {
        return err;
    }
    rbyd->eoff += lfsr_data_size(&data);

    return 0;
}

static int lfsr_rbyd_appendcksum(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));

    // we can't do anything if we're not erased
    if (rbyd->eoff >= lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    // make sure every rbyd starts with its revision count
    if (rbyd->eoff == 0) {
        int err = lfsr_rbyd_appendrev(lfs, rbyd, 0);
        if (err) {
            return err;
        }
    }

    // align to the next prog unit
    //
    // this gets a bit complicated as we have two types of cksums:
    //
    // - 9-word cksum with ecksum to check following prog (middle of block)
    //   - ecksum tag type => 2 byte le16
    //   - ecksum tag rid  => 1 byte leb128
    //   - ecksum tag size => 1 byte leb128 (worst case)
    //   - ecksum crc32c   => 4 byte le32
    //   - ecksum size     => 5 byte leb128 (worst case)
    //   - cksum tag type  => 2 byte le16
    //   - cksum tag rid   => 1 byte leb128
    //   - cksum tag size  => 5 byte leb128 (worst case)
    //   - cksum crc32c    => 4 byte le32
    //                     => 25 bytes total
    //
    // - 4-word cksum with no following prog (end of block)
    //   - cksum tag type => 2 byte le16
    //   - cksum tag rid  => 1 byte leb128
    //   - cksum tag size => 5 byte leb128 (worst case)
    //   - cksum crc32c   => 4 byte le32
    //                    => 12 bytes total
    //
    lfs_size_t aligned_eoff = lfs_alignup(
            rbyd->eoff + 2+1+1+4+5 + 2+1+5+4,
            lfs->cfg->prog_size);

    // space for ecksum?
    uint8_t perturb = 0;
    if (aligned_eoff < lfs->cfg->block_size) {
        // read the leading byte in case we need to change the expected
        // value of the next tag's valid bit
        int err = lfsr_bd_read(lfs,
                rbyd->blocks[0], aligned_eoff, lfs->cfg->prog_size,
                &perturb, 1);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        // find the expected ecksum, don't bother avoiding a reread of the
        // perturb byte, as it should still be in our cache
        lfsr_ecksum_t ecksum = {.size=lfs->cfg->prog_size};
        err = lfsr_bd_cksum(lfs,
                rbyd->blocks[0], aligned_eoff, ecksum.size,
                ecksum.size,
                &ecksum.cksum);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }

        uint8_t ecksum_buf[LFSR_ECKSUM_DSIZE];
        lfsr_data_t ecksum_data = lfsr_data_fromecksum(&ecksum, ecksum_buf);
        lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->blocks[0], rbyd->eoff,
                LFSR_TAG_ECKSUM, 0, lfsr_data_size(&ecksum_data),
                &rbyd->cksum);
        if (d < 0) {
            return d;
        }
        rbyd->eoff += d;

        err = lfsr_bd_progdata(lfs, rbyd->blocks[0], rbyd->eoff, ecksum_data,
                &rbyd->cksum, NULL);
        if (err) {
            return err;
        }
        rbyd->eoff += lfsr_data_size(&ecksum_data);

    // at least space for a cksum?
    } else if (rbyd->eoff + 2+1+5+4 <= lfs->cfg->block_size) {
        // note this implicitly marks the rbyd as unerased
        aligned_eoff = lfs->cfg->block_size;

    // not even space for a cksum? we can't finish the commit
    } else {
        return LFS_ERR_RANGE;
    }

    // build end-of-commit cksum
    //
    // note padding-size depends on leb-encoding depends on padding-size, to
    // get around this catch-22 we just always write a fully-expanded leb128
    // encoding
    uint8_t cksum_buf[2+1+5+4];
    cksum_buf[0] = (LFSR_TAG_CKSUM >> 8) | ((lfs_popc(rbyd->cksum) & 1) << 7);
    cksum_buf[1] = 0;
    cksum_buf[2] = 0;

    lfs_size_t padding = aligned_eoff - (rbyd->eoff + 2+1+5);
    cksum_buf[3] = 0x80 | (0x7f & (padding >>  0));
    cksum_buf[4] = 0x80 | (0x7f & (padding >>  7));
    cksum_buf[5] = 0x80 | (0x7f & (padding >> 14));
    cksum_buf[6] = 0x80 | (0x7f & (padding >> 21));
    cksum_buf[7] = 0x00 | (0x7f & (padding >> 28));

    rbyd->cksum = lfs_crc32c(rbyd->cksum, cksum_buf, 2+1+5);
    // we can't let the next tag appear as valid, so intentionally perturb the
    // commit if this happens, note parity(crc(m)) == parity(m) with crc32c,
    // so we can really change any bit to make this happen, we've reserved a bit
    // in cksum tags just for this purpose
    if ((lfs_popc(rbyd->cksum) & 1) == (perturb >> 7)) {
        cksum_buf[1] ^= 0x01;
        rbyd->cksum ^= 0x68032cc8; // note crc(a ^ b) == crc(a) ^ crc(b)
    }
    lfs_tole32_(rbyd->cksum, &cksum_buf[2+1+5]);

    int err = lfsr_bd_prog(lfs, rbyd->blocks[0], rbyd->eoff,
            cksum_buf, 2+1+5+4,
            NULL, NULL);
    if (err) {
        return err;
    }
    rbyd->eoff += 2+1+5+4;

    // flush our caches, finalizing the commit on-disk
    err = lfsr_bd_sync(lfs);
    if (err) {
        return err;
    }

    rbyd->eoff = aligned_eoff;
    return 0;
}

static int lfsr_rbyd_appendattrs(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // append each tag to the tree
    for (lfs_size_t i = 0; i < attr_count; i++) {
        // don't write tags outside of the requested range
        if (attrs[i].rid >= start_rid
                // note the use of rid+1 and unsigned comparison here to
                // treat end_rid=-1 as "unbounded" in such a way that rid=-1
                // is still included
                && (lfs_size_t)(attrs[i].rid + 1) <= (lfs_size_t)end_rid) {
            int err = lfsr_rbyd_appendattr(lfs, rbyd,
                    attrs[i].rid - lfs_smax32(start_rid, 0),
                    attrs[i].tag, attrs[i].delta, attrs[i].data);
            if (err) {
                return err;
            }
        }

        // we need to make sure we keep start_rid/end_rid updated with
        // weight changes
        if (attrs[i].rid < start_rid) {
            start_rid += attrs[i].delta;
        }
        if (attrs[i].rid < end_rid) {
            end_rid += attrs[i].delta;
        }
    }

    return 0;
}

static int lfsr_rbyd_commit(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // append each tag to the tree
    int err = lfsr_rbyd_appendattrs(lfs, rbyd, -1, -1,
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


// determine the upper-bound cost of a single rbyd attr after compaction
//
// note that with rebalancing during compaction, we know the number
// of inner nodes is roughly the same as the number of tags. Each node
// has two alts and is terminated by a 4-byte null tag.
//
#define LFSR_ATTR_ESTIMATE (3*LFSR_TAG_DSIZE + 4)

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
    lfsr_srid_t rid = start_rid;
    lfsr_srid_t other_rid = lfs_min32(rbyd->weight, end_rid);
    lfs_size_t dsize = 0;
    lfs_size_t other_dsize = 0;
    lfs_size_t rbyd_dsize = 0;

    while (rid != other_rid) {
        if (dsize > other_dsize
                // bias so lower dsize >= upper dsize
                || (dsize == other_dsize && rid > other_rid)) {
            lfs_sswap32(&rid, &other_rid);
            lfs_swap32(&dsize, &other_dsize);
        }

        if (rid > other_rid) {
            rid -= 1;
        }

        lfsr_tag_t tag = 0;
        lfsr_rid_t weight = 0;
        lfs_size_t dsize_ = 0;
        while (true) {
            lfsr_srid_t rid_;
            lfsr_rid_t weight_;
            lfsr_data_t data;
            int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                    rid, tag+1,
                    &rid_, &tag, &weight_, &data);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }
            if (rid_ > rid+lfs_smax32(weight_-1, 0)) {
                break;
            }

            // keep track of rid and weight
            rid = rid_;
            weight += weight_;

            // include the cost of this tag
            dsize_ += LFSR_ATTR_ESTIMATE + lfsr_data_size(&data);
        }

        if (rid == -1) {
            rbyd_dsize += dsize_;
        } else {
            dsize += dsize_;
        }

        if (rid < other_rid) {
            rid += 1;
        } else {
            rid -= lfs_smax32(weight-1, 0);
        }
    }

    if (split_rid_) {
        *split_rid_ = rid;
    }

    return rbyd_dsize + dsize + other_dsize;
}

// appends a raw tag as a part of compaction, note these must
// be appended in order!
//
// also note the direct use of weight instead of delta here
static int lfsr_rbyd_appendcompactattr(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        lfsr_tag_t tag, lfsr_rid_t weight, lfsr_data_t data) {
    // TODO deduplicate this? rbyd_preparemutation or something?
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));

    // we can't do anything if we're not erased
    if (rbyd->eoff >= lfs->cfg->block_size) {
        return LFS_ERR_RANGE;
    }

    // make sure every rbyd starts with a revision count
    if (rbyd->eoff == 0) {
        int err = lfsr_rbyd_appendrev(lfs, rbyd, 0);
        if (err) {
            return err;
        }
    }

    // write the tag
    lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->blocks[0], rbyd->eoff,
            tag, weight, lfsr_data_size(&data),
            &rbyd->cksum);
    if (d < 0) {
        return d;
    }
    rbyd->eoff += d;

    // and the data
    int err = lfsr_bd_progdata(lfs, rbyd->blocks[0], rbyd->eoff, data,
            &rbyd->cksum, NULL);
    if (err) {
        return err;
    }
    rbyd->eoff += lfsr_data_size(&data);

    // keep track of the total weight, the rbyd is in an unusable
    // state until lfsr_rbyd_appendcompaction anyways
    rbyd->weight += weight;

    return 0;
}

static int lfsr_rbyd_appendcompactrbyd(lfs_t *lfs,
        lfsr_rbyd_t *rbyd_, bool isshrub,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        const lfsr_rbyd_t *rbyd) {
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
        err = lfsr_rbyd_appendcompactattr(lfs, rbyd_,
                ((isshrub) ? LFSR_TAG_SHRUB : 0) | tag, weight, data);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_rbyd_appendcompaction(lfs_t *lfs,
        lfsr_rbyd_t *rbyd, bool isshrub,
        lfs_size_t off) {
    // must fetch before mutating!
    LFS_ASSERT(lfsr_rbyd_isfetched(rbyd));

    // clamp offset to be after the revision count
    off = lfs_max32(off, sizeof(uint32_t));

    // make sure every rbyd starts with a revision count
    if (rbyd->eoff == 0) {
        int err = lfsr_rbyd_appendrev(lfs, rbyd, 0);
        if (err) {
            return err;
        }
    }

    // empty rbyd? write a null tag so our trunk can still point to something
    if (rbyd->eoff == off) {
        lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->blocks[0], rbyd->eoff,
                (isshrub) ? LFSR_TAG_SHRUB(NULL) : LFSR_TAG_NULL, 0, 0,
                &rbyd->cksum);
        if (d < 0) {
            return d;
        }
        rbyd->eoff += d;

        rbyd->trunk = off;
        rbyd->weight = 0;
        return 0;
    }

    // connect every other trunk together, building layers of a perfectly
    // balanced binary tree upwards until we have a single trunk
    lfs_size_t layer = off;
    lfsr_rid_t weight = 0;
    while (true) {
        lfs_size_t layer_ = rbyd->eoff;
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
                    if (!isshrub && lfsr_tag_isshrub(tag__)) {
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
                lfs_ssize_t d = lfsr_bd_progtag(lfs,
                        rbyd->blocks[0], rbyd->eoff,
                        LFSR_TAG_ALT(LE, B, lfsr_tag_key(tag)),
                        weight,
                        rbyd->eoff - trunk,
                        &rbyd->cksum);
                if (d < 0) {
                    return d;
                }
                rbyd->eoff += d;
            }

            // terminate with a null tag
            lfs_ssize_t d = lfsr_bd_progtag(lfs, rbyd->blocks[0], rbyd->eoff,
                    (isshrub) ? LFSR_TAG_SHRUB(NULL) : LFSR_TAG_NULL, 0, 0,
                    &rbyd->cksum);
            if (d < 0) {
                return d;
            }
            rbyd->eoff += d;
        }

        layer = layer_;
    }

done:;
    // done! just need to update our trunk. Note we could have no trunks
    // after compaction. Leave this to upper layers to take care of this.
    rbyd->trunk = layer;
    rbyd->weight = weight;

    return 0;
}

static int lfsr_rbyd_compact(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        const lfsr_rbyd_t *rbyd) {
    // append rbyd
    int err = lfsr_rbyd_appendcompactrbyd(lfs, rbyd_, false,
            start_rid, end_rid, rbyd);
    if (err) {
        return err;
    }

    // compact
    err = lfsr_rbyd_appendcompaction(lfs, rbyd_, false, 0);
    if (err) {
        return err;
    }

    return 0;
}

// append and consume any pending gstate
static int lfsr_rbyd_appendgdelta(lfs_t *lfs, lfsr_rbyd_t *rbyd) {
    // need grm delta?
    if (!lfsr_grm_iszero(lfs->grm_d)) {
        // calculate our delta
        uint8_t grm_buf[LFSR_GRM_DSIZE];
        memset(grm_buf, 0, LFSR_GRM_DSIZE);

        lfsr_data_t data;
        int err = lfsr_rbyd_lookup(lfs, rbyd, -1, LFSR_TAG_GRMDELTA,
                &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        if (err != LFS_ERR_NOENT) {
            lfs_ssize_t grm_dsize = lfsr_data_read(lfs, &data,
                    grm_buf, LFSR_GRM_DSIZE);
            if (grm_dsize < 0) {
                return grm_dsize;
            }
        }

        err = lfsr_grm_xor(lfs, grm_buf, LFSR_DATA_BUF(
                &lfs->grm_d, LFSR_GRM_DSIZE));
        if (err) {
            return err;
        }

        // append to our rbyd, note this replaces the original delta
        lfs_size_t size = lfsr_grm_size(grm_buf);
        err = lfsr_rbyd_appendattr(lfs, rbyd, -1,
                // opportunistically remove this tag if delta is all zero
                (size == 0 ? LFSR_TAG_RM(GRMDELTA) : LFSR_TAG_GRMDELTA), 0,
                LFSR_DATA_BUF(grm_buf, size));
        if (err) {
            return err;
        }
    }

    return 0;
}

// append a secondary "shrub" tree
static int lfsr_rbyd_appendshrub(lfs_t *lfs, lfsr_rbyd_t *rbyd,
        const lfsr_rbyd_t *shrub) {
    // keep track of the start of the new tree
    lfs_size_t off = rbyd->eoff;

    // compact our shrub
    int err = lfsr_rbyd_appendcompactrbyd(lfs, rbyd, true,
            -1, -1, shrub);
    if (err) {
        return err;
    }

    err = lfsr_rbyd_appendcompaction(lfs, rbyd, true,
            off);
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
    lfsr_srid_t lower = 0;
    lfsr_srid_t upper = rbyd->weight;
    lfs_scmp_t cmp;
    while (lower < upper) {
        lfsr_tag_t tag__;
        lfsr_srid_t rid__;
        lfsr_rid_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, rbyd,
                // lookup ~middle rid, note we may end up in the middle
                // of a weighted rid with this
                lower + (upper-1-lower)/2, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // if we have no name, treat this rid as always lt
        if (lfsr_tag_suptype(tag__) != LFSR_TAG_NAME) {
            cmp = LFS_CMP_LT;

        // compare names
        } else {
            cmp = lfsr_data_namecmp(lfs, &data__, did, name, name_size);
            if (cmp < 0) {
                return cmp;
            }
        }

        // bisect search space
        if (lfs_cmp(cmp) > 0) {
            upper = rid__ - (weight__-1);

            // only keep track of best-match rids > our target if we haven't
            // seen an rid < our target
            if (lower == 0) {
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

        } else if (lfs_cmp(cmp) < 0) {
            lower = rid__ + 1;

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
    return (lower == 0) ? LFS_CMP_GT : LFS_CMP_LT;
}



/// B-tree operations ///

// convenience operations

static inline int lfsr_btree_cmp(
        const lfsr_btree_t *a,
        const lfsr_btree_t *b) {
    return lfsr_rbyd_cmp(a, b);
}


// branch on-disk encoding
#define LFSR_BRANCH_DSIZE (5+5+4)

// 2 leb128 + 1 crc32c => 14 bytes (worst case)
#define LFSR_DATA_FROMBRANCH(_branch, _buffer) \
    lfsr_data_frombranch(_branch, _buffer)

static lfsr_data_t lfsr_data_frombranch(const lfsr_rbyd_t *branch,
        uint8_t buffer[static LFSR_BRANCH_DSIZE]) {
    lfs_ssize_t d = 0;

    lfs_ssize_t d_ = lfs_toleb128(branch->blocks[0], &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    d_ = lfs_toleb128(branch->trunk, &buffer[d], 5);
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

    int err = lfsr_data_readleb128(lfs, data, (int32_t*)&branch->blocks[0]);
    if (err) {
        return err;
    }

    err = lfsr_data_readleb128(lfs, data, (int32_t*)&branch->trunk);
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

// 3 leb128 + 1 crc32c => 19 bytes (worst case)
#define LFSR_BTREE_DSIZE (5+LFSR_BRANCH_DSIZE)

#define LFSR_DATA_FROMBTREE(_btree, _buffer) \
    lfsr_data_frombtree(_btree, _buffer)

static lfsr_data_t lfsr_data_frombtree(const lfsr_btree_t *btree,
        uint8_t buffer[static LFSR_BTREE_DSIZE]) {
    lfs_ssize_t d = 0;
    lfs_ssize_t d_ = lfs_toleb128(btree->weight, &buffer[d], 5);
    LFS_ASSERT(d_ >= 0);
    d += d_;

    lfsr_data_t data = lfsr_data_frombranch(btree, &buffer[d]);
    d += lfsr_data_size(&data);

    return LFSR_DATA_BUF(buffer, d);
}

static int lfsr_data_readbtree(lfs_t *lfs, lfsr_data_t *data,
        lfsr_btree_t *btree) {
    lfsr_bid_t weight;
    int err = lfsr_data_readleb128(lfs, data, (int32_t*)&weight);
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

static int lfsr_btree_alloc(lfs_t *lfs, lfsr_btree_t *btree) {
    return lfsr_rbyd_alloc(lfs, btree);
}

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
            err = lfsr_rbyd_lookupwide(lfs, &branch, rid__, LFSR_TAG_STRUCT,
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
            err = lfsr_rbyd_lookupwide(lfs, &branch, rid__, LFSR_TAG_STRUCT,
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
        if (branch_.blocks[0] == child->blocks[0]
                && branch_.trunk == child->trunk) {
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


// core btree algorithm
static lfs_ssize_t lfsr_btree_commit_(lfs_t *lfs,
        lfsr_btree_t *btree, bool shrub,
        lfsr_attr_t scratch_attrs[static 4],
        uint8_t scratch_buffer[static 2*LFSR_BRANCH_DSIZE],
        const lfsr_attr_t *attrs, lfs_size_t attr_count,
        const lfsr_attr_t **attrs_, lfs_size_t *attr_count_) {
    // TODO should we just use the first bid?
    // first find the effective bid
    lfsr_bid_t bid = -1;
    for (lfs_size_t i = 0; i < attr_count; i++) {
        // note unsigned min here chooses non-negative bids
        bid = lfs_min32(bid, attrs[i].rid);
    }
    LFS_ASSERT(bid <= (lfsr_bid_t)btree->weight);

    // lookup in which leaf our bids resides
    //
    // for lfsr_btree_commit operations to work out, we need to
    // limit our bid to an rid in the tree, which is what this min
    // is doing
    lfsr_rbyd_t rbyd = *btree;
    if (btree->weight > 0) {
        lfsr_srid_t rid;
        int err = lfsr_btree_lookupnext_(lfs, btree,
                lfs_min32(bid, btree->weight-1),
                &bid, &rbyd, &rid, NULL, NULL, NULL);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // adjust bid to indicate the zero-most rid
        bid -= rid;
    }

    // tail-recursively commit to btree
    while (true) {
        // we will always need our parent, so go ahead and find it
        lfsr_rbyd_t parent = {.trunk=0, .weight=0};
        lfsr_srid_t rid;
        // are we root?
        if (rbyd.blocks[0] == btree->blocks[0] || rbyd.trunk == 0) {
            // new root? shrub root? yield creation of new roots to
            // higher-level bshrub/btree logic
            if (shrub || rbyd.trunk == 0) {
                *btree = rbyd;
                if (attrs_) {
                    *attrs_ = attrs;
                }
                if (attr_count_) {
                    *attr_count_ = attr_count;
                }
                return 0;
            }

            // mark btree as unerased in case of failure, our btree rbyd and
            // root rbyd can diverge if there's a split, but we would have
            // marked the old root as unerased earlier anyways
            btree->eoff = -1;

        } else {
            int err = lfsr_btree_parent(lfs, btree, bid, &rbyd, &parent, &rid);
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
                    rbyd.blocks[0], rbyd.trunk, rbyd.weight, rbyd.cksum);
            if (err) {
                return err;
            }
        }

        // is rbyd erased? can we sneak our commit into any remaining
        // erased bytes? note that the btree trunk field prevents this from
        // interacting with other references to the rbyd
        lfsr_rbyd_t rbyd_ = rbyd;
        int err = lfsr_rbyd_appendattrs(lfs, &rbyd_, bid, -1,
                attrs, attr_count);
        if (err) {
            // TODO wait should we also move if there is corruption here?
            if (err == LFS_ERR_RANGE) {
                goto compact;
            }
            return err;
        }

        err = lfsr_rbyd_appendcksum(lfs, &rbyd_);
        if (err) {
            if (err == LFS_ERR_RANGE) {
                goto compact;
            }
            // TODO wait should we also move if there is corruption here?
            return err;
        }

        goto finalize;

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
                && parent.trunk != 0) {
            // try the right sibling
            if (rid+1 < parent.weight) {
                // try looking up the sibling
                lfsr_srid_t sibling_rid;
                lfsr_tag_t sibling_tag;
                lfsr_rid_t sibling_weight;
                lfsr_data_t sibling_data;
                err = lfsr_rbyd_lookupnext(lfs, &parent,
                        rid+1, LFSR_TAG_NAME,
                        &sibling_rid, &sibling_tag, &sibling_weight,
                        &sibling_data);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                if (sibling_tag == LFSR_TAG_NAME) {
                    err = lfsr_rbyd_lookupwide(lfs, &parent,
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
            if (rid-rbyd.weight >= 0) {
                // try looking up the sibling
                lfsr_srid_t sibling_rid;
                lfsr_tag_t sibling_tag;
                lfsr_rid_t sibling_weight;
                lfsr_data_t sibling_data;
                err = lfsr_rbyd_lookupnext(lfs, &parent,
                        rid-rbyd.weight, LFSR_TAG_NAME,
                        &sibling_rid, &sibling_tag, &sibling_weight,
                        &sibling_data);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                if (sibling_tag == LFSR_TAG_NAME) {
                    err = lfsr_rbyd_lookupwide(lfs, &parent,
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
                    rid -= rbyd.weight;

                    rbyd_ = sibling;
                    sibling = rbyd;
                    rbyd = rbyd_;

                    goto merge;
                }
            }
        }

        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // try to compact
        err = lfsr_rbyd_compact(lfs, &rbyd_, -1, -1,
                &rbyd);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // append any pending attrs, it's up to upper
        // layers to make sure these always fit
        err = lfsr_rbyd_appendattrs(lfs, &rbyd_, bid, -1,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // finalize commit
        err = lfsr_rbyd_appendcksum(lfs, &rbyd_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        goto finalize;

    split:;
        // we should have something to split here
        LFS_ASSERT(split_rid > 0 && split_rid < rbyd.weight);

        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // allocate a sibling
        err = lfsr_rbyd_alloc(lfs, &sibling);
        if (err) {
            return err;
        }

        // copy over tags < split_rid
        err = lfsr_rbyd_compact(lfs, &rbyd_, -1, split_rid,
                &rbyd);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // append pending attrs < split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendattrs(lfs, &rbyd_, bid, bid+split_rid,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // finalize commit
        err = lfsr_rbyd_appendcksum(lfs, &rbyd_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }
        
        // copy over tags >= split_rid
        err = lfsr_rbyd_compact(lfs, &sibling, split_rid, -1,
                &rbyd);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // append pending attrs >= split_rid
        //
        // upper layers should make sure this can't fail by limiting the
        // maximum commit size
        err = lfsr_rbyd_appendattrs(lfs, &sibling, bid+split_rid, -1,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }
        
        // finalize commit
        err = lfsr_rbyd_appendcksum(lfs, &sibling);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // did one of our siblings drop to zero? yes this can happen! revert
        // to a normal commit in that case
        if (rbyd_.weight == 0 || sibling.weight == 0) {
            if (rbyd_.weight == 0) {
                rbyd_ = sibling;
            }
            goto finalize;
        }

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending attrs in case
        // they introduce a new name!
        lfsr_tag_t split_tag;
        lfsr_data_t split_data;
        err = lfsr_rbyd_lookupnext(lfs, &sibling, 0, LFSR_TAG_NAME,
                NULL, &split_tag, NULL, &split_data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // prepare commit to parent, tail recursing upwards
        LFS_ASSERT(rbyd_.weight > 0);
        LFS_ASSERT(sibling.weight > 0);
        lfsr_attr_t *attrs_ = scratch_attrs;
        // new root?
        if (parent.trunk == 0) {
            *attrs_++ = LFSR_ATTR(0,
                    BRANCH, +rbyd_.weight, FROMBRANCH(&rbyd_, scratch_buffer));
            *attrs_++ = LFSR_ATTR(rbyd_.weight,
                    BRANCH, +sibling.weight,
                    FROMBRANCH(&sibling, scratch_buffer + LFSR_BRANCH_DSIZE));
            if (lfsr_tag_suptype(split_tag) == LFSR_TAG_NAME) {
                *attrs_++ = LFSR_ATTR(rbyd_.weight + sibling.weight - 1,
                        NAME, 0, DATA(split_data));
            }
        // split root?
        } else {
            bid -= rid - (rbyd.weight-1);
            *attrs_++ = LFSR_ATTR(bid+rid,
                    BRANCH, 0, FROMBRANCH(&rbyd_, scratch_buffer));
            *attrs_++ = LFSR_ATTR(bid+rid,
                    GROW, -rbyd.weight + rbyd_.weight, NULL());
            *attrs_++ = LFSR_ATTR(bid+rid - rbyd.weight + rbyd_.weight + 1,
                    BRANCH, +sibling.weight,
                    FROMBRANCH(&sibling, scratch_buffer + LFSR_BRANCH_DSIZE));
            if (lfsr_tag_suptype(split_tag) == LFSR_TAG_NAME) {
                *attrs_++ = LFSR_ATTR(
                        bid+rid - rbyd.weight + rbyd_.weight + sibling.weight,
                        NAME, 0, DATA(split_data));
            }
        }
        attrs = scratch_attrs;
        attr_count = attrs_ - scratch_attrs;

        rbyd = parent;
        continue;

    merge:;
        // allocate a new rbyd
        err = lfsr_rbyd_alloc(lfs, &rbyd_);
        if (err) {
            return err;
        }

        // merge the siblings together
        err = lfsr_rbyd_appendcompactrbyd(lfs, &rbyd_, false,
                -1, -1, &rbyd);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        err = lfsr_rbyd_appendcompactrbyd(lfs, &rbyd_, false,
                -1, -1, &sibling);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        err = lfsr_rbyd_appendcompaction(lfs, &rbyd_, false, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // append any pending attrs, it's up to upper
        // layers to make sure these always fit
        err = lfsr_rbyd_appendattrs(lfs, &rbyd_, bid, -1,
                attrs, attr_count);
        if (err) {
            return err;
        }

        // finalize the commit
        err = lfsr_rbyd_appendcksum(lfs, &rbyd_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // we must have a parent at this point, but is our parent the root
        // and is the root degenerate?
        LFS_ASSERT(parent.trunk != 0);
        if (rbyd.weight+sibling.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            *btree = rbyd_;
            if (attr_count_) {
                *attr_count_ = 0;
            }
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        LFS_ASSERT(rbyd_.weight > 0);
        attrs_ = scratch_attrs;
        bid -= rid - (rbyd.weight-1);
        *attrs_++ = LFSR_ATTR(bid+rid+sibling.weight,
                RM, -sibling.weight, NULL());
        *attrs_++ = LFSR_ATTR(bid+rid,
                BRANCH, 0, FROMBRANCH(&rbyd_, scratch_buffer));
        *attrs_++ = LFSR_ATTR(bid+rid,
                GROW, -rbyd.weight + rbyd_.weight, NULL());
        attrs = scratch_attrs;
        attr_count = attrs_ - scratch_attrs;

        rbyd = parent;
        continue;

    finalize:;
        // done?
        if (parent.trunk == 0) {
            LFS_ASSERT(bid == 0);
            *btree = rbyd_;
            if (attr_count_) {
                *attr_count_ = 0;
            }
            return 0;
        }

        // is our parent the root and is the root degenerate?
        if (rbyd.weight == btree->weight) {
            // collapse the root, decreasing the height of the tree
            *btree = rbyd_;
            if (attr_count_) {
                *attr_count_ = 0;
            }
            return 0;
        }

        // prepare commit to parent, tail recursing upwards
        //
        // note that since we defer merges to compaction time, we can
        // end up removing an rbyd here
        attrs_ = scratch_attrs;
        bid -= rid - (rbyd.weight-1);
        if (rbyd_.weight == 0) {
            *attrs_++ = LFSR_ATTR(bid+rid,
                    RM, -rbyd.weight, NULL());
        } else {
            *attrs_++ = LFSR_ATTR(bid+rid,
                    BRANCH, 0, FROMBRANCH(&rbyd_, scratch_buffer));
            *attrs_++ = LFSR_ATTR(bid+rid,
                    GROW, -rbyd.weight + rbyd_.weight, NULL());
        }
        attrs = scratch_attrs;
        attr_count = attrs_ - scratch_attrs;

        rbyd = parent;
    }
}

static int lfsr_btree_commit(lfs_t *lfs, lfsr_btree_t *btree,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // we need some scratch space for tail-recursive attrs
    lfsr_attr_t scratch_attrs[4];
    uint8_t scratch_buf[2*LFSR_BRANCH_DSIZE];

    // try to commit to the btree
    int err = lfsr_btree_commit_(lfs, btree, false,
            scratch_attrs, scratch_buf,
            attrs, attr_count,
            &attrs, &attr_count);
    if (err) {
        return err;
    }

    // needs a new root?
    if (attr_count > 0) {
        err = lfsr_rbyd_alloc(lfs, btree);
        if (err) {
            return err;
        }

        err = lfsr_rbyd_commit(lfs, btree, attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }
    }

    LFS_ASSERT(btree->trunk != 0);
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
        int err = lfsr_rbyd_lookupwide(lfs, &branch, rid__, LFSR_TAG_STRUCT,
                &tag__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // found another branch
        if (tag__ == LFSR_TAG_BRANCH) {
            // update our bid
            bid += rid__ - (weight__-1);

            // fetch the next branch
            err = lfsr_data_readbranch(lfs, &data__, weight__, &branch);
            if (err) {
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

// into returned from btree traversal
typedef struct lfsr_binfo {
    lfsr_bid_t bid;
    lfsr_tag_t tag;
    lfsr_bid_t weight;
    union {
        lfsr_rbyd_t rbyd;
        lfsr_data_t data;
    } u;
} lfsr_binfo_t;

static int lfsr_btree_traverse(lfs_t *lfs, const lfsr_btree_t *btree,
        lfsr_btraversal_t *btraversal,
        lfsr_binfo_t *binfo) {
    while (true) {
        // in range?
        if (btraversal->bid >= (lfsr_bid_t)btree->weight
                // make sure we traverse the root even if weight=0
                && (btraversal->branch.trunk != 0
                    // unless we don't even have a root yet
                    || btree->trunk == 0)) {
            return LFS_ERR_NOENT;
        }

        // restart from the root
        if (btraversal->rid >= btraversal->branch.weight) {
            btraversal->rid = btraversal->bid;
            btraversal->branch = *btree;

            // traverse the root
            if (btraversal->rid == 0) {
                binfo->bid = btree->weight-1;
                binfo->tag = LFSR_TAG_BRANCH;
                binfo->weight = btraversal->branch.weight;
                binfo->u.rbyd = btraversal->branch;
                return 0;
            }

            // continue, mostly for range check
            continue;
        }

        // descend down the tree
        lfsr_srid_t rid__;
        lfsr_tag_t tag__;
        lfsr_rid_t weight__;
        lfsr_data_t data__;
        int err = lfsr_rbyd_lookupnext(lfs, &btraversal->branch,
                btraversal->rid, 0,
                &rid__, &tag__, &weight__, &data__);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        if (lfsr_tag_suptype(tag__) == LFSR_TAG_NAME) {
            err = lfsr_rbyd_lookupwide(lfs, &btraversal->branch,
                    rid__, LFSR_TAG_STRUCT,
                    &tag__, &data__);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }
        }

        // found another branch
        if (tag__ == LFSR_TAG_BRANCH) {
            // adjust rid with subtree's weight
            btraversal->rid -= (rid__ - (weight__-1));

            // fetch the next branch
            err = lfsr_data_readbranch(lfs, &data__, weight__,
                    &btraversal->branch);
            if (err) {
                return err;
            }
            LFS_ASSERT((lfsr_bid_t)btraversal->branch.weight == weight__);

            // return inner btree nodes if this is the first time we've
            // seen them
            if (btraversal->rid == 0) {
                binfo->bid = btraversal->bid + (rid__ - btraversal->rid);
                binfo->tag = LFSR_TAG_BRANCH;
                binfo->weight = btraversal->branch.weight;
                binfo->u.rbyd = btraversal->branch;
                return 0;
            }

        // found our bid
        } else {
            // move on to the next rid
            //
            // note the effectively traverses a full leaf without redoing
            // the btree walk
            lfsr_bid_t bid__ = btraversal->bid + (rid__ - btraversal->rid);
            btraversal->bid = bid__ + 1;
            btraversal->rid = rid__ + 1;

            binfo->bid = bid__;
            binfo->tag = tag__;
            binfo->weight = weight__;
            binfo->u.data = data__;
            return 0;
        }
    }
}



/// Metadata pair operations things ///

// metadata-id things

static inline lfsr_mid_t lfsr_mweight(lfs_t *lfs) {
    return 1 << lfs->mbits;
}

static inline lfsr_sbid_t lfsr_mid_bid(lfs_t *lfs, lfsr_smid_t mid) {
    return mid | ((1 << lfs->mbits) - 1);
}

static inline lfsr_srid_t lfsr_mid_rid(lfs_t *lfs, lfsr_smid_t mid) {
    // note this maps mid=-1 => rid=-1 via sign extension
    return (mid >> (8*sizeof(lfsr_smid_t)-1))
            | (mid & ((1 << lfs->mbits) - 1));
}

// we use the root's bookmark at 0.0 to represent root
static inline bool lfsr_mid_isroot(lfsr_smid_t mid) {
    return mid == 0;
}


// metadata-pointer things

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

// 2 leb128 => 10 bytes (worst case)
#define LFSR_MPTR_DSIZE (5+5)

#define LFSR_DATA_FROMMPTR(_mptr, _buffer) \
    lfsr_data_frommptr(_mptr, _buffer)

static lfsr_data_t lfsr_data_frommptr(const lfsr_mptr_t *mptr,
        uint8_t buffer[static LFSR_MPTR_DSIZE]) {
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
        int err = lfsr_data_readleb128(lfs, data, (int32_t*)&mptr->blocks[i]);
        if (err) {
            return err;
        }
    }

    return 0;
}


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

static inline lfsr_sbid_t lfsr_mdir_bid(lfs_t *lfs, const lfsr_mdir_t *mdir) {
    return lfsr_mid_bid(lfs, mdir->mid);
}

static inline lfsr_srid_t lfsr_mdir_rid(lfs_t *lfs, const lfsr_mdir_t *mdir) {
    return lfsr_mid_rid(lfs, mdir->mid);
}

static inline bool lfsr_mdir_isroot(const lfsr_mdir_t *mdir) {
    return lfsr_mid_isroot(mdir->mid);
}

// track opened mdirs that may need to by updated
static void lfsr_mdir_addopened(lfs_t *lfs, int type,
        lfsr_openedmdir_t *opened) {
    opened->next = lfs->opened[type-LFS_TYPE_REG];
    lfs->opened[type-LFS_TYPE_REG] = opened;
}

static void lfsr_mdir_removeopened(lfs_t *lfs, int type,
        lfsr_openedmdir_t *opened) {
    for (lfsr_openedmdir_t **p = &lfs->opened[type-LFS_TYPE_REG];
            *p;
            p = &(*p)->next) {
        if (*p == opened) {
            *p = (*p)->next;
            break;
        }
    }
}

static bool lfsr_mdir_isopened(lfs_t *lfs, int type,
        const lfsr_openedmdir_t *opened) {
    for (lfsr_openedmdir_t *p = lfs->opened[type-LFS_TYPE_REG];
            p;
            p = p->next) {
        if (p == opened) {
            return true;
        }
    }

    return false;
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
        lfsr_smid_t mid, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    lfsr_smid_t mid_;
    lfsr_tag_t tag__;
    int err = lfsr_rbyd_lookupnext(lfs, &mdir->rbyd,
            lfsr_mid_rid(lfs, mid), tag,
            &mid_, &tag__, NULL, data_);
    if (err) {
        return err;
    }

    // this is very similar to lfsr_rbyd_lookupnext, but we error if
    // lookupnext would change mids
    if (mid_ != lfsr_mid_rid(lfs, mid)) {
        return LFS_ERR_NOENT;
    }

    if (tag_) {
        *tag_ = tag__;
    }
    return 0;
}

static int lfsr_mdir_lookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_smid_t mid, lfsr_tag_t tag,
        lfsr_data_t *data_) {
    return lfsr_rbyd_lookup(lfs, &mdir->rbyd,
            lfsr_mid_rid(lfs, mid), tag,
            data_);
}

static int lfsr_mdir_lookupwide(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_smid_t mid, lfsr_tag_t tag,
        lfsr_tag_t *tag_, lfsr_data_t *data_) {
    return lfsr_rbyd_lookupwide(lfs, &mdir->rbyd,
            lfsr_mid_rid(lfs, mid), tag,
            tag_, data_);
}


/// Metadata-tree things ///

// the mtree is the core tree of mdirs in littlefs

#define LFSR_MTREE_ISMPTR 0x80000000

#define LFSR_MTREE_NULL() ((lfsr_mtree_t){ \
    .u.weight=(LFSR_MTREE_ISMPTR | 0)})

#define LFSR_MTREE_MPTR(_mptr, _weight) ((lfsr_mtree_t){ \
    .u.mptr.weight=(LFSR_MTREE_ISMPTR | (_weight)), \
    .u.mptr.mptr=_mptr})

static inline bool lfsr_mtree_isnull(lfs_t *lfs) {
    return (lfsr_mid_t)lfs->mtree.u.weight == (LFSR_MTREE_ISMPTR | 0);
}

static inline bool lfsr_mtree_ismptr(lfs_t *lfs) {
    return lfs->mtree.u.weight & LFSR_MTREE_ISMPTR;
}

static inline bool lfsr_mtree_isbtree(lfs_t *lfs) {
    return !(lfs->mtree.u.weight & LFSR_MTREE_ISMPTR);
}

static inline lfsr_mid_t lfsr_mtree_weight(lfs_t *lfs) {
    return lfs->mtree.u.weight & ~LFSR_MTREE_ISMPTR;
}

static int lfsr_mtree_lookup(lfs_t *lfs, lfsr_smid_t mid,
        lfsr_mdir_t *mdir_) {
    // looking up mroot?
    if (lfsr_mtree_isnull(lfs)) {
        LFS_ASSERT(mid >= 0);
        LFS_ASSERT(mid < (lfsr_smid_t)lfsr_mweight(lfs));
        mdir_->mid = mid;
        mdir_->rbyd = lfs->mroot.rbyd;
        return 0;

    // looking up direct mdir?
    } else if (lfsr_mtree_ismptr(lfs)) {
        LFS_ASSERT(mid >= 0);
        LFS_ASSERT(mid < (lfsr_smid_t)lfsr_mweight(lfs));

        // fetch mdir
        return lfsr_mdir_fetch(lfs, mdir_, mid, &lfs->mtree.u.mptr.mptr);

    // look up mdir in actual mtree
    } else {
        LFS_ASSERT(mid >= 0);
        LFS_ASSERT(mid < (lfsr_smid_t)lfsr_mtree_weight(lfs));
        lfsr_bid_t bid;
        lfsr_tag_t tag;
        lfsr_data_t data;
        int err = lfsr_btree_lookupnext(lfs, &lfs->mtree.u.btree,
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

static int lfsr_mtree_seek(lfs_t *lfs, lfsr_mdir_t *mdir, lfs_off_t off) {
    // upper layers should handle removed mdirs
    LFS_ASSERT(mdir->mid >= 0);

    while (true) {
        // calculate new mid, be careful to avoid rid overflow
        lfsr_bid_t bid = lfsr_mdir_bid(lfs, mdir);
        lfsr_srid_t rid = lfsr_mdir_rid(lfs, mdir) + off;
        // lookup mdirs until we find our rid, we need to do this because
        // we don't know how many rids are in each mdir until we fetch
        while (rid >= mdir->rbyd.weight) {
            // end of mtree?
            if (bid+lfsr_mweight(lfs) >= lfsr_mtree_weight(lfs)) {
                // if we hit the end of the mtree, park the mdir so all future
                // seeks return noent
                mdir->mid = bid + lfsr_mweight(lfs);
                return LFS_ERR_NOENT;
            }

            bid += lfsr_mweight(lfs);
            rid -= mdir->rbyd.weight;
            int err = lfsr_mtree_lookup(lfs, bid, mdir);
            if (err) {
                return err;
            }
        }

        mdir->mid = bid-(lfsr_mweight(lfs)-1) + rid;

        // wait are we grmed? pretend this mid doesn't exist
        if (mdir->mid == lfs->grm.rms[0]
                || mdir->mid == lfs->grm.rms[1]) {
            continue;
        }

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

// needed in lfsr_mdir_commit/estimate/compact/etc
static inline bool lfsr_ftree_isnull(const lfsr_ftree_t *ftree);
static inline bool lfsr_ftree_isbsprout(const lfsr_ftree_t *ftree);
static inline bool lfsr_ftree_isbleaf(const lfsr_ftree_t *ftree);
static inline bool lfsr_ftree_isbshrub(const lfsr_ftree_t *ftree);
static inline bool lfsr_ftree_isbtree(const lfsr_ftree_t *ftree);
static inline bool lfsr_ftree_isbshruborbtree(const lfsr_ftree_t *ftree);
static int lfsr_bshrub_commit__(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_bshrub_t *bshrub,
        lfs_size_t *trunk_, lfsr_srid_t *weight_,
        const lfsr_attr_t *attrs, lfs_size_t attr_count);
static lfs_ssize_t lfsr_bsprout_estimate__(lfs_t *lfs,
        const lfsr_bsprout_t *bsprout);
static lfs_ssize_t lfsr_bshrub_estimate__(lfs_t *lfs,
        const lfsr_bshrub_t *bshrub);
static int lfsr_bsprout_compact__(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_bsprout_t *bsprout, bool shrub);
static int lfsr_bshrub_compact__(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_bshrub_t *bshrub, bool shrub,
        lfs_size_t *trunk_, lfsr_srid_t *weight_);

// some mdir-related gstate things we need
static void lfsr_fs_flushgdelta(lfs_t *lfs) {
    memset(lfs->grm_d, 0, LFSR_GRM_DSIZE);
}

static int lfsr_fs_consumegdelta(lfs_t *lfs, const lfsr_mdir_t *mdir) {
    lfsr_data_t data;
    int err = lfsr_mdir_lookup(lfs, mdir, -1, LFSR_TAG_GRMDELTA,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    if (err != LFS_ERR_NOENT) {
        err = lfsr_grm_xor(lfs, lfs->grm_d, data);
        if (err) {
            return err;
        }
    }

    return 0;
}

// low-level mdir operations needed by lfsr_mdir_commit
static int lfsr_mdir_alloc__(lfs_t *lfs, lfsr_mdir_t *mdir, lfsr_smid_t mid) {
    // assign the mid
    mdir->mid = mid;

    // allocate two blocks
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &mdir->rbyd.blocks[i]);
        if (err) {
            return err;
        }
    }

    mdir->rbyd.weight = 0;
    mdir->rbyd.trunk = 0;
    mdir->rbyd.eoff = 0;
    mdir->rbyd.cksum = 0;

    // read the new revision count
    //
    // we use whatever is on-disk to avoid needing to rewrite the
    // redund block
    uint32_t rev;
    int err = lfsr_bd_read(lfs, mdir->rbyd.blocks[1], 0, sizeof(uint32_t),
            &rev, sizeof(uint32_t));
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    rev = (err != LFS_ERR_CORRUPT ? lfs_fromle32_(&rev) : 0);

    // align revision count in new mdirs to our block_cycles, this makes
    // sure we don't immediately try to relocate the mdir
    if (lfs->cfg->block_cycles > 0) {
        rev = lfs_alignup(rev+1, lfs->cfg->block_cycles)-1;
    }

    // erase, preparing for compact
    err = lfsr_bd_erase(lfs, mdir->rbyd.blocks[0]);
    if (err) {
        return err;
    }

    // increment our revision count and write it to our rbyd
    // TODO rev things
    err = lfsr_rbyd_appendrev(lfs, &mdir->rbyd, rev + 1);
    if (err) {
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
    int err = lfsr_bd_read(lfs, mdir->rbyd.blocks[0], 0, sizeof(uint32_t),
            &rev, sizeof(uint32_t));
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    // note we allow corrupt errors here, as long as they are consistent
    rev = (err != LFS_ERR_CORRUPT ? lfs_fromle32_(&rev) : 0);

    // decide if we need to relocate
    if (!force
            && lfs->cfg->block_cycles > 0
            // TODO rev things
            && (rev + 1) % lfs->cfg->block_cycles == 0) {
        // alloc a new mdir
        return lfsr_mdir_alloc__(lfs, mdir_, mdir->mid);
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
    // TODO rev things
    err = lfsr_rbyd_appendrev(lfs, &mdir_->rbyd, rev + 1);
    if (err) {
        return err;
    }

    return 0;
}

// low-level mdir commit, does not handle mtree/mlist/compaction/etc
static int lfsr_mdir_commit__(lfs_t *lfs, lfsr_mdir_t *mdir,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // try to append a commit
    lfsr_rbyd_t rbyd_ = mdir->rbyd;
    // mark as erased in case of failure
    mdir->rbyd.eoff = -1;
    for (lfs_size_t i = 0; i < attr_count; i++) {
        // calculate adjusted rid
        lfsr_srid_t rid = lfsr_mid_rid(lfs, attrs[i].rid);

        // don't write tags outside of the requested range
        if (rid >= start_rid
                // note the use of rid+1 and unsigned comparison here to
                // treat end_rid=-1 as "unbounded" in such a way that rid=-1
                // is still included
                && (lfs_size_t)(rid + 1) <= (lfs_size_t)end_rid) {
            // ignore any gstate tags here, these need to be handled
            // specially by upper-layers
            if (attrs[i].tag == LFSR_TAG_GRM) {
                // do nothing

            // move tags copy over any tags associated with the source's rid
            } else if (attrs[i].tag == LFSR_TAG_MOVE) {
                // weighted moves are not supported
                LFS_ASSERT(attrs[i].delta == 0);
                const lfsr_mdir_t *mdir__
                        = (const lfsr_mdir_t*)attrs[i].data.u.buf.buffer;

                // skip the name tag, this is always replaced by upper layers
                lfsr_tag_t tag = LFSR_TAG_STRUCT-1;
                while (true) {
                    lfsr_data_t data;
                    int err = lfsr_mdir_lookupnext(lfs, mdir__,
                            mdir__->mid, tag+1,
                            &tag, &data);
                    if (err) {
                        if (err == LFS_ERR_NOENT) {
                            break;
                        }
                        return err;
                    }

                    // special case for bshrubs, we need to copy these over
                    if (tag == LFSR_TAG_BSHRUB) {
                        lfsr_rbyd_t shrub = mdir__->rbyd;
                        err = lfsr_data_readtrunk(lfs, &data,
                                &shrub.trunk, (lfsr_rid_t*)&shrub.weight);
                        if (err) {
                            return err;
                        }

                        // compact our bshrub
                        err = lfsr_bshrub_compact__(lfs, &rbyd_,
                                (const lfsr_bshrub_t*)&shrub, false,
                                &shrub.trunk, &shrub.weight);
                        if (err) {
                            return err;
                        }

                        // write our new shrub tag
                        uint8_t trunk_buf[LFSR_TRUNK_DSIZE];
                        err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                                rid - lfs_smax32(start_rid, 0),
                                LFSR_TAG_BSHRUB, 0, lfsr_data_fromtrunk(
                                    shrub.trunk, shrub.weight,
                                    trunk_buf));
                        if (err) {
                            return err;
                        }

                    // append the attr
                    } else {
                        err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                                rid - lfs_smax32(start_rid, 0),
                                tag, 0, data);
                        if (err) {
                            return err;
                        }
                    }
                }

            // shrub tags append a set of attributes to an unrelated trunk
            // in our rbyd
            } else if (attrs[i].tag == LFSR_TAG_SHRUBCOMMIT) {
                const lfsr_bshrubcommit_t *bshrubcommit
                        = (const lfsr_bshrubcommit_t*)
                            attrs[i].data.u.buf.buffer;
                int err = lfsr_bshrub_commit__(lfs, &rbyd_,
                        bshrubcommit->bshrub,
                        &bshrubcommit->bshrub->rbyd_.trunk,
                        &bshrubcommit->bshrub->rbyd_.weight,
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
                lfsr_bshrub_t *bshrub
                        = (lfsr_bshrub_t*)attrs[i].data.u.buf.buffer;

                uint8_t trunk_buf[LFSR_TRUNK_DSIZE];
                int err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                        rid - lfs_smax32(start_rid, 0),
                        lfsr_tag_mode(attrs[i].tag) | LFSR_TAG_BSHRUB,
                        attrs[i].delta,
                        lfsr_data_fromtrunk(
                            // note we use the staged trunk here
                            bshrub->rbyd_.trunk,
                            bshrub->rbyd_.weight,
                            trunk_buf));
                if (err) {
                    return err;
                }

            // write out normal tags normally
            } else {
                LFS_ASSERT(!lfsr_tag_isinternal(attrs[i].tag));

                int err = lfsr_rbyd_appendattr(lfs, &rbyd_,
                        rid - lfs_smax32(start_rid, 0),
                        attrs[i].tag, attrs[i].delta, attrs[i].data);
                if (err) {
                    return err;
                }
            }
        }

        // we need to make sure we keep start_rid/end_rid updated with
        // weight changes
        if (rid < start_rid) {
            start_rid += attrs[i].delta;
        }
        if (rid < end_rid) {
            end_rid += attrs[i].delta;
        }
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
    lfsr_srid_t rid = start_rid;
    lfsr_srid_t other_rid = lfs_min32(mdir->rbyd.weight, end_rid);
    lfs_size_t dsize = 0;
    lfs_size_t other_dsize = 0;
    lfs_size_t mdir_dsize = 0;

    while (rid != other_rid) {
        if (dsize > other_dsize
                // bias so lower dsize >= upper dsize
                || (dsize == other_dsize && rid > other_rid)) {
            lfs_sswap32(&rid, &other_rid);
            lfs_swap32(&dsize, &other_dsize);
        }

        if (rid > other_rid) {
            rid -= 1;
        }

        lfsr_tag_t tag = 0;
        lfs_size_t dsize_ = 0;
        while (true) {
            lfsr_srid_t rid_;
            lfsr_data_t data;
            int err = lfsr_rbyd_lookupnext(lfs, &mdir->rbyd,
                    rid, tag+1,
                    &rid_, &tag, NULL, &data);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }
            if (rid_ != rid) {
                break;
            }

            // include the cost of this tag
            dsize_ += LFSR_ATTR_ESTIMATE;

            // special handling for sprouts, just to avoid duplicate cost
            if (tag == LFSR_TAG_DATA) {
                // TODO don't include tag in attr estimate?
                // we already included the size of the tag in our attr
                // estimate, undo that for now
                dsize_ -= LFSR_TAG_DSIZE;

                lfs_ssize_t dsize__ = lfsr_bsprout_estimate__(lfs,
                        (const lfsr_bsprout_t*)&data);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += dsize__;

            // special handling for shrub trunks, we need to include the
            // compacted cost of the shrub in our estimate
            //
            // this is what would make lfsr_rbyd_estimate recursive, and
            // why we need a second function...
            //
            } else if (tag == LFSR_TAG_BSHRUB) {
                // include the cost of this trunk
                dsize_ += LFSR_TRUNK_DSIZE;

                lfsr_rbyd_t shrub = mdir->rbyd;
                err = lfsr_data_readtrunk(lfs, &data,
                        &shrub.trunk, (lfsr_rid_t*)&shrub.weight);
                if (err) {
                    return err;
                }

                lfs_ssize_t dsize__ = lfsr_bshrub_estimate__(lfs,
                        (const lfsr_bshrub_t*)&shrub);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += dsize__;

            } else {
                // include the cost of this data
                dsize_ += lfsr_data_size(&data);
            }
        }

        // include any opened+unsynced inlined files
        //
        // this is O(n^2), but littlefs is unlikely to have many open
        // files, I suppose if this becomes a problem we could sort
        // opened files by mid
        for (lfsr_openedmdir_t *opened = lfs->opened[
                    LFS_TYPE_REG-LFS_TYPE_REG];
                opened;
                opened = opened->next) {
            lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
            // belongs to our mdir + rid?
            if (lfsr_mdir_cmp(&ftree->mdir, mdir) != 0
                    || lfsr_mdir_rid(lfs, &ftree->mdir) != rid) {
                continue;
            }

            // inlined sprout?
            if (lfsr_ftree_isbsprout(ftree)) {
                lfs_ssize_t dsize__ = lfsr_bsprout_estimate__(lfs,
                        &ftree->u.bsprout);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += dsize__;

            // inlined shrub?
            } else if (lfsr_ftree_isbshrub(ftree)) {
                lfs_ssize_t dsize__ = lfsr_bshrub_estimate__(lfs,
                        &ftree->u.bshrub);
                if (dsize__ < 0) {
                    return dsize__;
                }
                dsize_ += dsize__;
            }
        }

        if (rid == -1) {
            mdir_dsize += dsize_;
        } else {
            dsize += dsize_;
        }

        if (rid < other_rid) {
            rid += 1;
        }
    }

    if (split_rid_) {
        *split_rid_ = rid;
    }

    return mdir_dsize + dsize + other_dsize;
}

static int lfsr_mdir_compact__(lfs_t *lfs, lfsr_mdir_t *mdir_,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        const lfsr_mdir_t *mdir) {
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
            LFS_ASSERT(weight == 0);
            err = lfsr_bsprout_compact__(lfs, &mdir_->rbyd,
                    (const lfsr_bsprout_t*)&data, false);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

        // found an inlined shrub? we need to compact the shrub as well to
        // bring it along with us
        } else if (tag == LFSR_TAG_BSHRUB) {
            lfsr_rbyd_t shrub = mdir->rbyd;
            err = lfsr_data_readtrunk(lfs, &data,
                    &shrub.trunk, (lfsr_rid_t*)&shrub.weight);
            if (err) {
                return err;
            }

            // compact our shrub
            err = lfsr_bshrub_compact__(lfs, &mdir_->rbyd,
                    (const lfsr_bshrub_t*)&shrub, false,
                    &shrub.trunk, &shrub.weight);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

            // write the new shrub tag
            uint8_t trunk_buf[LFSR_TRUNK_DSIZE];
            err = lfsr_rbyd_appendcompactattr(lfs, &mdir_->rbyd,
                    tag, weight, lfsr_data_fromtrunk(
                        shrub.trunk, shrub.weight,
                        trunk_buf));
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

        } else {
            // write the tag
            err = lfsr_rbyd_appendcompactattr(lfs, &mdir_->rbyd,
                    tag, weight, data);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }
        }
    }

    int err = lfsr_rbyd_appendcompaction(lfs, &mdir_->rbyd, false, 0);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // we're not quite done! we also need to bring over any unsynced files
    for (lfsr_openedmdir_t *opened = lfs->opened[
                LFS_TYPE_REG-LFS_TYPE_REG];
            opened;
            opened = opened->next) {
        lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
        // belongs to our mdir?
        if (lfsr_mdir_cmp(&ftree->mdir, mdir) != 0
                || lfsr_mdir_rid(lfs, &ftree->mdir) < start_rid
                || (lfsr_rid_t)lfsr_mdir_rid(lfs, &ftree->mdir)
                    >= (lfsr_rid_t)end_rid) {
            continue;
        }

        // inlined sprout?
        if (lfsr_ftree_isbsprout(ftree)) {
            err = lfsr_bsprout_compact__(lfs, &mdir_->rbyd,
                    &ftree->u.bsprout, true);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }

        // inlined shrub?
        } else if (lfsr_ftree_isbshrub(ftree)) {
            err = lfsr_bshrub_compact__(lfs, &mdir_->rbyd,
                    &ftree->u.bshrub, true,
                    NULL, NULL);
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
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // try to commit
    int err = lfsr_mdir_commit__(lfs, mdir, start_rid, end_rid,
            attrs, attr_count);
    if (err) {
        if (err == LFS_ERR_RANGE) {
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
    if (err) {
        return err;
    }

    // compact our mdir
    err = lfsr_mdir_compact__(lfs, &mdir_, start_rid, end_rid,
            mdir);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // now try to commit again
    //
    // upper layers should make sure this can't fail by limiting the
    // maximum commit size
    err = lfsr_mdir_commit__(lfs, &mdir_, start_rid, end_rid,
            attrs, attr_count);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    *mdir = mdir_;
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
        err = lfsr_mdir_lookup(lfs, &mdir, -1, LFSR_TAG_MROOT,
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

// mid-level mroot commit
//
// this is atomic and propagates any mroot changes up the mroot chain
//
// note! this does not handle uninlining/gstate/etc! actual commits
// to the mroot should go through lfsr_mdir_commit with lfs->mroot as
// a target
//
static int lfsr_mroot_commit(lfs_t *lfs,
        lfsr_srid_t start_rid, lfsr_srid_t end_rid,
        lfsr_srid_t *split_rid_,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // create a copy
    lfsr_mdir_t mroot_ = lfs->mroot;

    // mark any copies of our mroot as unerased
    lfs->mroot.rbyd.eoff = -1;

    for (int type = LFS_TYPE_REG; type < LFS_TYPE_REG+3; type++) {
        for (lfsr_openedmdir_t *opened = lfs->opened[type-LFS_TYPE_REG];
                opened;
                opened = opened->next) {
            if (lfsr_mdir_cmp(&opened->mdir, &lfs->mroot) == 0) {
                opened->mdir.rbyd.eoff = -1;
            }
        }
    }

    // try to commit to the mroot
    int err = lfsr_mdir_commit_(lfs, &mroot_,
            start_rid, end_rid, split_rid_,
            attrs, attr_count);
    if (err) {
        return err;
    }

    // need to update mroot chain? tail recurse, updating mroots until a
    // commit sticks
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
            return err;
        }

        LFS_DEBUG("Relocating mroot 0x{%"PRIx32",%"PRIx32"} "
                "-> 0x{%"PRIx32",%"PRIx32"}",
                mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                mrootchild_.rbyd.blocks[0], mrootchild_.rbyd.blocks[1]);

        mrootchild = mrootparent_;

        // commit mrootchild
        uint8_t mrootchild_buf[LFSR_MPTR_DSIZE];
        err = lfsr_mdir_commit_(lfs, &mrootparent_, -1, -1, NULL, LFSR_ATTRS(
                LFSR_ATTR(-1,
                    MROOT, 0,
                    FROMMPTR(lfsr_mdir_mptr(&mrootchild_), mrootchild_buf))));
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        mrootchild_ = mrootparent_;
    }

    // uh oh, we ran out of mroot parents, need to extend mroot chain
    if (lfsr_mdir_cmp(&mrootchild_, &mrootchild) != 0) {
        // mrootchild should be our previous mroot anchor at this point
        LFS_ASSERT(lfsr_mdir_ismrootanchor(&mrootchild));
        LFS_DEBUG("Extending mroot 0x{%"PRIx32",%"PRIx32"}"
                " -> 0x{%"PRIx32",%"PRIx32"}"
                ", 0x{%"PRIx32",%"PRIx32"}",
                mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                mrootchild.rbyd.blocks[0], mrootchild.rbyd.blocks[1],
                mrootchild_.rbyd.blocks[0], mrootchild_.rbyd.blocks[1]);

        // compact into the new mroot anchor
        lfsr_mdir_t mrootanchor_;
        err = lfsr_mdir_swap__(lfs, &mrootanchor_, &mrootchild, -1);
        if (err) {
            return err;
        }

        // copy only the config over
        lfsr_tag_t tag = 0;
        while (true) {
            lfsr_srid_t rid;
            lfsr_rid_t weight;
            lfsr_data_t data;
            err = lfsr_rbyd_lookupnext(lfs, &mrootchild.rbyd,
                    -1, tag+1,
                    &rid, &tag, &weight, &data);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    break;
                }
                return err;
            }
            if (rid != -1 || lfsr_tag_suptype(tag) != LFSR_TAG_CONFIG) {
                break;
            }

            // write the tag
            err = lfsr_rbyd_appendcompactattr(lfs, &mrootanchor_.rbyd,
                    tag, weight, data);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_RANGE);
                return err;
            }
        }

        err = lfsr_rbyd_appendcompaction(lfs, &mrootanchor_.rbyd, false, 0);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // and commit our new mroot
        uint8_t mrootchild_buf[LFSR_MPTR_DSIZE];
        err = lfsr_mdir_commit__(lfs, &mrootanchor_, -1, -1, LFSR_ATTRS(
                LFSR_ATTR(-1,
                    WIDE(MROOT), 0,
                    FROMMPTR(lfsr_mdir_mptr(&mrootchild_), mrootchild_buf))));
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }
    }

    // success? update in-device state, we must not error at this point
    for (int type = LFS_TYPE_REG; type < LFS_TYPE_REG+3; type++) {
        for (lfsr_openedmdir_t *opened = lfs->opened[type-LFS_TYPE_REG];
                opened;
                opened = opened->next) {
            if (lfsr_mdir_cmp(&opened->mdir, &lfs->mroot) == 0) {
                // update any bsprouts/bshrubs, note this must happen
                // before we update the mdir
                if (type == LFS_TYPE_REG) {
                    lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
                    if (lfsr_ftree_isbsprout(ftree)) {
                        ftree->u.bsprout.data = ftree->u.bsprout.data_;
                    } else if (lfsr_ftree_isbshrub(ftree)) {
                        ftree->u.bshrub.rbyd = ftree->u.bshrub.rbyd_;
                    }
                }

                // update any opened mdirs in our mroot
                opened->mdir.rbyd = mroot_.rbyd;
            }
        }
    }

    // and save our new mroot
    lfs->mroot = mroot_;
    return 0;
}

// high-level mtree commit
//
// this is atomic and propagates any mroot changes up the mroot chain
//
static int lfsr_mtree_commit(lfs_t *lfs,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    lfsr_btree_t mtree_;
    // new mtree?
    if (lfsr_mtree_ismptr(lfs)) {
        int err = lfsr_btree_alloc(lfs, &mtree_);
        if (err) {
            return err;
        }

    } else {
        // create a copy
        mtree_ = lfs->mtree.u.btree;

        // mark as unerased in case of failure
        lfs->mtree.u.btree.eoff = -1;
    }

    // commit to mtree
    int err = lfsr_btree_commit(lfs, &mtree_, attrs, attr_count);
    if (err) {
        return err;
    }

    // mtree should never go to zero since we will always have a
    // root bookmark
    LFS_ASSERT(mtree_.weight > 0);

    // commit new mtree into our mroot
    //
    // Note if we every have an mtree, mroot should have no files,
    // so we set end_rid=0 here which will delete any files. Above
    // layers rely on this.
    //
    uint8_t mtree_buf[LFSR_BTREE_DSIZE];
    err = lfsr_mroot_commit(lfs, -1, 0, NULL, LFSR_ATTRS(
            LFSR_ATTR(-1,
                WIDE(MTREE), 0, FROMBTREE(&mtree_, mtree_buf))));
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    // success? update in-device state, we must not error at this point

    // save our new mtree
    lfs->mtree.u.btree = mtree_;
    return 0;
}

// unlike merging btree nodes, mdirs must be explicitly dropped
//
// this is atomic updates any opened mdirs, lfs_t, gstate, etc
//
static int lfsr_mdir_drop(lfs_t *lfs, const lfsr_mdir_t *mdir) {
    // mdir should be empty at this point
    LFS_ASSERT(mdir->rbyd.weight == 0);
    // yeah, you really shouldn't try to drop the mroot
    LFS_ASSERT(mdir->mid != -1 && lfsr_mdir_cmp(mdir, &lfs->mroot) != 0);

    LFS_DEBUG("Dropping mdir %"PRId32" "
            "0x{%"PRIx32",%"PRIx32"}",
            mdir->mid >> lfs->mbits,
            mdir->rbyd.blocks[0], mdir->rbyd.blocks[1]);

    // reset gdelta for new commit
    lfsr_fs_flushgdelta(lfs);

    // TODO can we avoid this stack allocation somehow?
    // a bit hacky, but we need to update any pending grms here
    lfsr_grm_t grm_ = lfs->grm;
    if (lfsr_grm_hasrm(&grm_)) {
        // fix any pending grms
        for (int j = 0; j < lfsr_grm_count(&grm_); j++) {
            LFS_ASSERT(lfsr_mid_bid(lfs, grm_.rms[j])
                    != lfsr_mid_bid(lfs, lfs_smax32(mdir->mid, 0)));
            if (grm_.rms[j] > mdir->mid) {
                grm_.rms[j] -= lfsr_mweight(lfs);
            }
        }

        // xor our fix into our gdelta
        uint8_t grm_buf[LFSR_GRM_DSIZE];
        int err = lfsr_grm_xor(lfs, lfs->grm_d,
                lfsr_data_fromgrm(&grm_, grm_buf));
        if (err) {
            return err;
        }

        err = lfsr_grm_xor(lfs, lfs->grm_d,
                LFSR_DATA_BUF(lfs->grm_g, LFSR_GRM_DSIZE));
        if (err) {
            return err;
        }
    }

    // consume mdir's gstate so we don't lose any info
    int err = lfsr_fs_consumegdelta(lfs, mdir);
    if (err) {
        return err;
    }

    // we should never drop a direct mdir, because we always have our
    // root bookmark
    LFS_ASSERT(!lfsr_mtree_ismptr(lfs));

    // update our mtree
    err = lfsr_mtree_commit(lfs, LFSR_ATTRS(
            LFSR_ATTR(lfsr_mdir_bid(lfs, mdir),
                RM, -lfsr_mweight(lfs), NULL())));
    if (err) {
        return err;
    }

    // success? update in-device state, we must not error at this point

    // gstate must have been committed by a lower-level function at this point
    LFS_ASSERT(lfsr_grm_iszero(lfs->grm_d));

    // update gstate
    lfs->grm = grm_;
    // keep track of the exact encoding on-disk
    lfsr_data_fromgrm(&lfs->grm, lfs->grm_g);

    for (int type = LFS_TYPE_REG; type < LFS_TYPE_REG+3; type++) {
        for (lfsr_openedmdir_t *opened = lfs->opened[type-LFS_TYPE_REG];
                opened;
                opened = opened->next) {
            // update mids
            if (opened->mdir.mid > mdir->mid) {
                opened->mdir.mid -= lfsr_mweight(lfs);
            }

            // update directory bookmarks
            if (type == LFS_TYPE_DIR) {
                lfsr_dir_t *dir = (lfsr_dir_t*)opened;
                if (dir->bookmark > mdir->mid) {
                    dir->bookmark -= lfsr_mweight(lfs);
                }
            }
        }
    }

    return 0;
}

// high-level mdir commit
//
// this is atomic and updates any opened mdirs, lfs_t, gstate, etc
//
static int lfsr_mdir_commit(lfs_t *lfs, lfsr_mdir_t *mdir,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    LFS_ASSERT(lfsr_mdir_rid(lfs, mdir) <= mdir->rbyd.weight);

    // reset gdelta for new commit
    lfsr_fs_flushgdelta(lfs);

    // create a copy
    lfsr_mdir_t mdir_ = *mdir;

    for (lfs_size_t i = 0; i < attr_count; i++) {
        // parse out any pending gstate, these will get automatically
        // xored with on-disk gdeltas in lower-level functions
        if (attrs[i].tag == LFSR_TAG_GRM) {
            // encode to disk
            lfsr_grm_t *grm = (lfsr_grm_t*)attrs[i].data.u.buf.buffer;
            lfsr_data_fromgrm(grm, lfs->grm_d);

            // xor with our current gstate to find our initial gdelta
            int err = lfsr_grm_xor(lfs, lfs->grm_d,
                    LFSR_DATA_BUF(lfs->grm_g, LFSR_GRM_DSIZE));
            if (err) {
                return err;
            }
        }
    }

    for (int type = LFS_TYPE_REG; type < LFS_TYPE_REG+3; type++) {
        for (lfsr_openedmdir_t *opened = lfs->opened[type-LFS_TYPE_REG];
                opened;
                opened = opened->next) {
            // mark any copies of our mdir as unerased in case we fail
            //
            // note we need to not mark the mroot as unerased, because that
            // would force the mroot to always compact
            //
            if (lfsr_mdir_cmp(&opened->mdir, mdir) == 0) {
                opened->mdir.rbyd.eoff = -1;
            }

            // stage any bsprouts/bshrubs
            if (type == LFS_TYPE_REG) {
                lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
                if (lfsr_ftree_isbsprout(ftree)) {
                    ftree->u.bsprout.data_ = ftree->u.bsprout.data;
                } else if (lfsr_ftree_isbshrub(ftree)) {
                    ftree->u.bshrub.rbyd_ = ftree->u.bshrub.rbyd;
                }
            }
        }
    }

    // attempt to commit/compact the mdir normally
    lfsr_srid_t split_rid;
    int err;
    // mroot? we need special handling to propagate up the mroot chain
    if (lfsr_mdir_cmp(&mdir_, &lfs->mroot) == 0) {
        err = lfsr_mroot_commit(lfs, -1, -1, &split_rid,
                attrs, attr_count);
        if (err && err != LFS_ERR_RANGE) {
            return err;
        }

        // keep mdir_ in sync with mroot
        mdir_.rbyd = lfs->mroot.rbyd;

    // otherwise commit normally
    } else {
        // mark our mdir as unerased in case we fail
        mdir->rbyd.eoff = -1;

        err = lfsr_mdir_commit_(lfs, &mdir_, -1, -1, &split_rid,
                attrs, attr_count);
        if (err && err != LFS_ERR_RANGE) {
            return err;
        }
    }

    // handle possible mtree updates, this gets a bit messy
    lfsr_mdir_t msibling_;
    lfsr_smid_t mdelta = 0;
    // need to split?
    if (err == LFS_ERR_RANGE) {
        // this should not happen unless we can't fit our mroot's metadata
        LFS_ASSERT(mdir->mid != -1 || lfsr_mtree_isnull(lfs));

        // if we're not the mroot, we need to consume the gstate so
        // we don't lose any info during the split
        //
        // we do this here so we don't have to worry about corner cases
        // with dropping mdirs during a split
        if (!lfsr_mtree_isnull(lfs)) {
            err = lfsr_fs_consumegdelta(lfs, mdir);
            if (err) {
                return err;
            }
        }

        // compact into new mdir tags < split_rid
        err = lfsr_mdir_alloc__(lfs, &mdir_, lfs_smax32(mdir->mid, 0));
        if (err) {
            return err;
        }

        err = lfsr_mdir_compact__(lfs, &mdir_, 0, split_rid,
                mdir);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        err = lfsr_mdir_commit__(lfs, &mdir_, 0, split_rid,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // compact into new mdir tags >= split_rid
        err = lfsr_mdir_alloc__(lfs, &msibling_, lfs_smax32(mdir->mid, 0));
        if (err) {
            return err;
        }

        err = lfsr_mdir_compact__(lfs, &msibling_, split_rid, -1,
                mdir);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        err = lfsr_mdir_commit__(lfs, &msibling_, split_rid, -1,
                attrs, attr_count);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_RANGE);
            return err;
        }

        // adjust our sibling's mid after committing attrs
        msibling_.mid += lfsr_mweight(lfs);

        LFS_DEBUG("Splitting mdir %"PRId32" "
                "0x{%"PRIx32",%"PRIx32"} "
                "-> 0x{%"PRIx32",%"PRIx32"}, "
                "0x{%"PRIx32",%"PRIx32"}",
                mdir->mid >> lfs->mbits,
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
                mdir_.rbyd.blocks[0], mdir_.rbyd.blocks[1],
                msibling_.rbyd.blocks[0], msibling_.rbyd.blocks[1]);

        // because of defered commits, children can be reduced to zero
        // when splitting

        // one sibling reduced to zero
        if (msibling_.rbyd.weight == 0) {
            LFS_DEBUG("Dropping mdir %"PRId32" "
                    "0x{%"PRIx32",%"PRIx32"}",
                    msibling_.mid >> lfs->mbits,
                    msibling_.rbyd.blocks[0], msibling_.rbyd.blocks[1]);
            goto relocate;

        // other sibling reduced to zero
        } else if (mdir_.rbyd.weight == 0) {
            LFS_DEBUG("Dropping mdir %"PRId32" "
                    "0x{%"PRIx32",%"PRIx32"}",
                    mdir_.mid >> lfs->mbits,
                    mdir_.rbyd.blocks[0], mdir_.rbyd.blocks[1]);
            mdir_.rbyd = msibling_.rbyd;
            goto relocate;
        }

        // no siblings reduced to zero, update our mtree
        mdelta = +lfsr_mweight(lfs);

        // fix any pending grms
        for (lfs_size_t i = 0; i < attr_count; i++) {
            if (attrs[i].tag == LFSR_TAG_GRM) {
                // Assuming we already xored our gdelta with the grm, we first
                // need to xor the grm out of the gdelta. We can't just zero
                // the gdelta because we may have picked up extra gdelta from
                // split/dropped mdirs
                //
                // gd' = gd xor (grm' xor grm)
                //
                lfsr_grm_t *grm = (lfsr_grm_t*)attrs[i].data.u.buf.buffer;
                uint8_t grm_buf[LFSR_GRM_DSIZE];
                err = lfsr_grm_xor(lfs, lfs->grm_d,
                        lfsr_data_fromgrm(grm, grm_buf));
                if (err) {
                    return err;
                }

                // fix our grm
                for (int j = 0; j < 2; j++) {
                    if (lfsr_mid_bid(lfs, grm->rms[j])
                            == lfsr_mid_bid(lfs, lfs_smax32(mdir->mid, 0))) {
                        if (lfsr_mid_rid(lfs, grm->rms[j])
                                >= mdir_.rbyd.weight) {
                            grm->rms[j] += lfsr_mweight(lfs)
                                    - mdir_.rbyd.weight;
                        }
                    } else if (grm->rms[j] > mdir->mid) {
                        grm->rms[j] += mdelta;
                    }
                }

                // xor our fix into our gdelta
                err = lfsr_grm_xor(lfs, lfs->grm_d,
                        lfsr_data_fromgrm(grm, grm_buf));
                if (err) {
                    return err;
                }
            }
        }

        // lookup first name in sibling to use as the split name
        //
        // note we need to do this after playing out pending attrs in
        // case they introduce a new name!
        lfsr_data_t split_data;
        err = lfsr_rbyd_lookupwide(lfs, &msibling_.rbyd, 0, LFSR_TAG_NAME,
                NULL, &split_data);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // new mtree?
        if (lfsr_mtree_ismptr(lfs)) {
            uint8_t mdir_buf[LFSR_MPTR_DSIZE];
            uint8_t msibling_buf[LFSR_MPTR_DSIZE];
            err = lfsr_mtree_commit(lfs, LFSR_ATTRS(
                    LFSR_ATTR(0,
                        MDIR, +lfsr_mweight(lfs),
                        FROMMPTR(lfsr_mdir_mptr(&mdir_), mdir_buf)),
                    LFSR_ATTR(lfsr_mweight(lfs),
                        NAME, +lfsr_mweight(lfs), DATA(split_data)),
                    LFSR_ATTR(lfsr_mweight(lfs)+lfsr_mweight(lfs)-1,
                        MDIR, 0,
                        FROMMPTR(lfsr_mdir_mptr(&msibling_), msibling_buf))));
            if (err) {
                return err;
            }

        // update our mtree
        } else {
            uint8_t mdir_buf[LFSR_MPTR_DSIZE];
            uint8_t msibling_buf[LFSR_MPTR_DSIZE];
            err = lfsr_mtree_commit(lfs, LFSR_ATTRS(
                    LFSR_ATTR(lfsr_mdir_bid(lfs, &mdir_),
                        MDIR, 0, FROMMPTR(lfsr_mdir_mptr(&mdir_), mdir_buf)),
                    LFSR_ATTR(lfsr_mdir_bid(lfs, &mdir_)+1,
                        NAME, +lfsr_mweight(lfs), DATA(split_data)),
                    LFSR_ATTR(lfsr_mdir_bid(lfs, &msibling_),
                        MDIR, 0,
                        FROMMPTR(lfsr_mdir_mptr(&msibling_), msibling_buf))));
            if (err) {
                return err;
            }
        }

    // need to relocate?
    } else if (lfsr_mdir_cmp(mdir, &mdir_) != 0
            && lfsr_mdir_cmp(&mdir_, &lfs->mroot) != 0) {
        LFS_DEBUG("Relocating mdir %"PRId32" "
                "0x{%"PRIx32",%"PRIx32"} -> 0x{%"PRIx32",%"PRIx32"}",
                mdir->mid >> lfs->mbits,
                mdir->rbyd.blocks[0], mdir->rbyd.blocks[1],
                mdir_.rbyd.blocks[0], mdir_.rbyd.blocks[1]);

    relocate:;
        // new mtree?
        if (lfsr_mtree_ismptr(lfs)) {
            uint8_t mdir_buf[LFSR_MPTR_DSIZE];
            err = lfsr_mroot_commit(lfs, -1, 0, NULL, LFSR_ATTRS(
                    LFSR_ATTR(-1,
                        WIDE(MDIR), 0,
                        FROMMPTR(lfsr_mdir_mptr(&mdir_), mdir_buf))));
            if (err) {
                return err;
            }

            lfs->mtree = LFSR_MTREE_MPTR(
                    *lfsr_mdir_mptr(&mdir_),
                    lfsr_mweight(lfs));

        } else {
            // update our mtree
            uint8_t mdir_buf[LFSR_MPTR_DSIZE];
            err = lfsr_mtree_commit(lfs, LFSR_ATTRS(
                    LFSR_ATTR(lfsr_mdir_bid(lfs, &mdir_),
                        MDIR, 0, FROMMPTR(lfsr_mdir_mptr(&mdir_), mdir_buf))));
            if (err) {
                return err;
            }
        }
    }

    // success? update in-device state, we must not error at this point

    // gstate must have been committed by a lower-level function at this point
    LFS_ASSERT(lfsr_grm_iszero(lfs->grm_d));

    for (lfs_size_t i = 0; i < attr_count; i++) {
        // update any gstate
        if (attrs[i].tag == LFSR_TAG_GRM) {
            lfs->grm = *(lfsr_grm_t*)attrs[i].data.u.buf.buffer;

            // keep track of the exact encoding on-disk
            lfsr_data_fromgrm(&lfs->grm, lfs->grm_g);
        }
    }

    for (int type = LFS_TYPE_REG; type < LFS_TYPE_REG+3; type++) {
        for (lfsr_openedmdir_t *opened = lfs->opened[type-LFS_TYPE_REG];
                opened;
                opened = opened->next) {
            // update any bsprouts/bshrubs, note this must happen
            // before we update the mdir
            if (type == LFS_TYPE_REG) {
                lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
                if (lfsr_ftree_isbsprout(ftree)) {
                    ftree->u.bsprout.data = ftree->u.bsprout.data_;
                } else if (lfsr_ftree_isbshrub(ftree)) {
                    ftree->u.bshrub.rbyd = ftree->u.bshrub.rbyd_;
                }
            }

            // avoid double updating current mdir
            if (&opened->mdir == mdir) {
                continue;
            }

            // first play out any attrs that change our rid
            for (lfs_size_t i = 0; i < attr_count; i++) {
                // adjust opened mdirs?
                if (lfsr_mdir_cmp(&opened->mdir, mdir) == 0
                        && opened->mdir.mid >= attrs[i].rid) {
                    // removed?
                    if (opened->mdir.mid < attrs[i].rid - attrs[i].delta) {
                        // for dir's second mdir (the position mdir), move
                        // on to the next rid
                        if (type == LFS_TYPE_DIR) {
                            opened->mdir.mid = attrs[i].rid;
                        // for normal mdirs mark as dropped
                        } else {
                            opened->mdir.mid = -1;
                            goto next;
                        }
                    } else {
                        opened->mdir.mid += attrs[i].delta;
                        // adjust dir position?
                        if (type == LFS_TYPE_DIR) {
                            ((lfsr_dir_t*)opened)->pos += attrs[i].delta;
                        }
                    }
                } else if (opened->mdir.mid > mdir->mid) {
                    // adjust dir position?
                    if (type == LFS_TYPE_DIR) {
                        ((lfsr_dir_t*)opened)->pos += attrs[i].delta;
                    }
                }
            }

            // update any opened mdirs if we had a split or drop
            if (lfsr_mdir_cmp(&opened->mdir, mdir) == 0) {
                if (mdelta > 0
                        && lfsr_mdir_rid(lfs, &opened->mdir)
                            >= mdir_.rbyd.weight) {
                    opened->mdir.mid += lfsr_mweight(lfs)
                            - mdir_.rbyd.weight;
                    opened->mdir.rbyd = msibling_.rbyd;
                } else {
                    opened->mdir.rbyd = mdir_.rbyd;
                }
            } else if (opened->mdir.mid > mdir->mid) {
                opened->mdir.mid += mdelta;
            }

            if (type == LFS_TYPE_DIR) {
                // update any changes to directory bookmarks/positions, this
                // gets a bit tricky
                lfsr_dir_t *dir = (lfsr_dir_t*)opened;
                for (lfs_size_t i = 0; i < attr_count; i++) {
                    // TODO clean this up a bit?
                    // adjust opened mdirs?
                    if (lfsr_mid_bid(lfs, dir->bookmark)
                                == lfsr_mid_bid(lfs, lfs_smax32(mdir->mid, 0))
                            && dir->bookmark >= attrs[i].rid) {
                        // removed?
                        if (dir->bookmark < attrs[i].rid - attrs[i].delta) {
                            // mark dir as dropped
                            dir->mdir.mid = -1;
                            dir->bookmark = -1;
                            goto next;
                        } else {
                            dir->bookmark += attrs[i].delta;
                            // adjust dir position?
                            dir->pos -= attrs[i].delta;
                        }
                    } else if (dir->bookmark > mdir->mid) {
                        // adjust dir position?
                        dir->pos -= attrs[i].delta;
                    }
                }

                if (lfsr_mid_bid(lfs, dir->bookmark)
                        == lfsr_mid_bid(lfs, lfs_smax32(mdir->mid, 0))) {
                    if (mdelta > 0
                            && lfsr_mid_rid(lfs, dir->bookmark)
                                >= mdir_.rbyd.weight) {
                        dir->bookmark += lfsr_mweight(lfs)
                                - mdir_.rbyd.weight;
                    }
                } else if (dir->bookmark > mdir->mid) {
                    dir->bookmark += mdelta;
                }
            }
    next:;
        }
    }

    // update mdir to follow requested rid
    if (mdir->mid == -1) {
        mdir->rbyd = lfs->mroot.rbyd;
    } else if (mdelta > 0
            && lfsr_mdir_rid(lfs, mdir) >= mdir_.rbyd.weight) {
        mdir->mid += lfsr_mweight(lfs) - mdir_.rbyd.weight;
        mdir->rbyd = msibling_.rbyd;
    } else {
        mdir->rbyd = mdir_.rbyd;
    }

    // we're not quite done, we want to clean up any mdirs that have been
    // reduced to zero
    //
    // This can error, which probably sounds like it invalidates the previous
    // "don't error" comment, but this is technically a second commit. If we
    // error at this point, it should be modeled as though we lost power,
    // mainly, hasorphans should be set.
    //
    // We handle drops differently than splits/relocates, since these updates
    // become visible as soon as the commit completes.
    //
    if (lfsr_mdir_cmp(&mdir_, &lfs->mroot) != 0
            && mdir_.rbyd.weight == 0) {
        err = lfsr_mdir_drop(lfs, &mdir_);
        if (err) {
            lfs->hasorphans = true;
            return err;
        }
    }

    return 0;
}



/// Path/name lookup stuff ///

// lookup names in an mdir
//
// if not found, rid will be the best place to insert
static int lfsr_mdir_namelookup(lfs_t *lfs, const lfsr_mdir_t *mdir,
        lfsr_did_t did, const char *name, lfs_size_t name_size,
        lfsr_srid_t *rid_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // empty mdir? make sure rid_ = 0 at least
    if (mdir->rbyd.weight == 0) {
        if (rid_) {
            *rid_ = 0;
        }
        return LFS_ERR_NOENT;
    }

    lfsr_srid_t rid;
    lfs_scmp_t cmp = lfsr_rbyd_namelookup(lfs, &mdir->rbyd,
            did, name, name_size,
            &rid, tag_, NULL, data_);
    if (cmp < 0) {
        LFS_ASSERT(cmp != LFS_ERR_NOENT);
        return cmp;
    }

    // adjust rid if necessary
    if (lfs_cmp(cmp) < 0) {
        rid += 1;
    }

    if (rid_) {
        *rid_ = rid;
    }
    return (lfs_cmp(cmp) == 0) ? 0 : LFS_ERR_NOENT;
}

// lookup names in our mtree
//
// if not found, rid will be the best place to insert
static int lfsr_mtree_namelookup(lfs_t *lfs,
        lfsr_did_t did, const char *name, lfs_size_t name_size,
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_, lfsr_data_t *data_) {
    // do we only have mroot?
    lfsr_mdir_t mdir;
    if (lfsr_mtree_isnull(lfs)) {
        mdir = lfs->mroot;
        // treat inlined mdir as mid=0
        mdir.mid = 0;

    // direct mdir?
    } else if (lfsr_mtree_ismptr(lfs)) {
        int err = lfsr_mdir_fetch(lfs, &mdir, 0, &lfs->mtree.u.mptr.mptr);
        if (err) {
            return err;
        }

    // lookup name in actual mtree
    } else {
        lfsr_bid_t bid;
        lfsr_tag_t tag;
        lfsr_bid_t weight;
        lfsr_data_t data;
        lfs_scmp_t cmp = lfsr_btree_namelookup(lfs, &lfs->mtree.u.btree,
                did, name, name_size,
                &bid, &tag, &weight, &data);
        if (cmp < 0) {
            LFS_ASSERT(cmp != LFS_ERR_NOENT);
            return cmp;
        }
        LFS_ASSERT(tag == LFSR_TAG_MDIR);
        LFS_ASSERT(weight == lfsr_mweight(lfs));

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
    lfsr_srid_t rid;
    int err = lfsr_mdir_namelookup(lfs, &mdir,
            did, name, name_size,
            &rid, tag_, data_);
    // update mdir with best place to insert even if we fail
    mdir.mid += rid;
    if (mdir_) {
        *mdir_ = mdir;
    }
    if (err) {
        return err;
    }

    // wait are we grmed? pretend this mid doesn't exist
    if (mdir.mid == lfs->grm.rms[0]
            || mdir.mid == lfs->grm.rms[1]) {
        return LFS_ERR_NOENT;
    }

    return 0;
}


// special directory-ids
enum {
    LFSR_DID_ROOT = 0,
};

// TODO this function may need another look over
//
// lookup full paths in our mtree
//
// if not found, mdir_/did_/name_ will at least be set up
// with what should be the parent
static int lfsr_mtree_pathlookup(lfs_t *lfs, const char *path,
        // TODO originally path itself was a double pointer, is that a
        // better design?
        lfsr_mdir_t *mdir_, lfsr_tag_t *tag_,
        lfsr_did_t *did_, const char **name_, lfs_size_t *name_size_) {
    // setup root
    lfsr_mdir_t mdir = {.mid = 0};
    lfsr_tag_t tag = LFSR_TAG_DIR;
    lfsr_did_t did = LFSR_DID_ROOT;

    // use mid=-1 to indicate we can't even create the path
    if (mdir_) {
        mdir_->mid = -1;
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
            if (mdir_) {
                *mdir_ = mdir;
            }
            if (tag_) {
                *tag_ = tag;
            }
            return 0;
        }

        // only continue if we hit a directory
        if (tag != LFSR_TAG_DIR) {
            return LFS_ERR_NOTDIR;
        }

        // read the next did from the mdir if this is not the root
        if (!lfsr_mid_isroot(mdir.mid)) {
            lfsr_data_t data;
            int err = lfsr_mdir_lookup(lfs, &mdir, mdir.mid, LFSR_TAG_DID,
                    &data);
            if (err) {
                return err;
            }

            err = lfsr_data_readleb128(lfs, &data, (int32_t*)&did);
            if (err) {
                return err;
            }
        }

        // lookup up this name in the mtree
        int err = lfsr_mtree_namelookup(lfs, did, name, name_size,
                &mdir, &tag, NULL);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // keep track of where to insert if we are the last name in our path
        if (strchr(name, '/') == NULL) {
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
        }

        // error if not found
        if (err == LFS_ERR_NOENT) {
            return LFS_ERR_NOENT;
        }

        // go on to next name
        name += name_size;
next:;
    }
}



/// B-sprout operations ///

// this is really just some helper functions for inlined files

static inline bool lfsr_bsprout_isbsprout(
        const lfsr_mdir_t *mdir, const lfsr_bsprout_t *bsprout) {
    return mdir->rbyd.blocks[0] == bsprout->data.u.disk.block;
}

static inline bool lfsr_bsprout_isbleaf(
        const lfsr_mdir_t *mdir, const lfsr_bsprout_t *bsprout) {
    return mdir->rbyd.blocks[0] != bsprout->data.u.disk.block;
}

static inline lfs_size_t lfsr_bsprout_size(const lfsr_bsprout_t *bsprout) {
    return lfsr_data_size(&bsprout->data);
}

static inline int lfsr_bsprout_cmp(
        const lfsr_bsprout_t *a,
        const lfsr_bsprout_t *b) {
    // big assumption for sprouts, we convert straight to bshrubs,
    // and never leave sliced sprouts in our files, so we don't need
    // to compare the size
    LFS_ASSERT(a->data.u.disk.block != b->data.u.disk.block
            || a->data.u.disk.off != b->data.u.disk.off
            || lfsr_bsprout_size(a) == lfsr_bsprout_size(b));
    if (a->data.u.disk.block != b->data.u.disk.block) {
        return a->data.u.disk.block - b->data.u.disk.block;
    } else {
        return a->data.u.disk.off - b->data.u.disk.off;
    }
}

// these are used in mdir compaction
static lfs_ssize_t lfsr_bsprout_estimate__(lfs_t *lfs,
        const lfsr_bsprout_t *bsprout) {
    // only include the last reference
    const lfsr_bsprout_t *last = NULL;
    for (lfsr_openedmdir_t *opened = lfs->opened[
                LFS_TYPE_REG-LFS_TYPE_REG];
            opened;
            opened = opened->next) {
        lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
        if (lfsr_ftree_isbsprout(ftree)
                && lfsr_bsprout_cmp(&ftree->u.bsprout, bsprout) == 0) {
            last = &ftree->u.bsprout;
        }
    }
    if (last && bsprout != last) {
        return 0;
    }

    return LFSR_TAG_DSIZE + lfsr_bsprout_size(bsprout);
}

static int lfsr_bsprout_compact__(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_bsprout_t *bsprout, bool shrub) {
    // only compact once, first compact should stage the new block
    if (shrub && bsprout->data_.u.disk.block == rbyd_->blocks[0]) {
        return 0;
    }

    // write out bsprout
    int err = lfsr_rbyd_appendcompactattr(lfs, rbyd_,
            (shrub) ? LFSR_TAG_SHRUB(DATA) : LFSR_TAG_DATA, 0,
            bsprout->data);
    if (err) {
        return err;
    }

    // stage any opened inlined files with their new location so we
    // can update these later if our commit is a success
    for (lfsr_openedmdir_t *opened = lfs->opened[
                LFS_TYPE_REG-LFS_TYPE_REG];
            opened;
            opened = opened->next) {
        lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
        if (lfsr_ftree_isbsprout(ftree)
                && lfsr_bsprout_cmp(&ftree->u.bsprout, bsprout) == 0) {
            // this is a bit tricky since we don't know the tag size,
            // but we have just enough info
            ftree->u.bsprout.data_ = LFSR_DATA_DISK(
                    rbyd_->blocks[0],
                    rbyd_->eoff - lfsr_bsprout_size(bsprout),
                    lfsr_bsprout_size(bsprout));
        }
    }

    return 0;
}



/// B-shrub operations ///

// bshrubs are btrees with inlined (shrubbed) roots
//
// for the most part these are just aliases for btree functions

static inline bool lfsr_bshrub_isbshrub(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub) {
    return mdir->rbyd.blocks[0] == bshrub->rbyd.blocks[0];
}

static inline bool lfsr_bshrub_isbtree(
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub) {
    return mdir->rbyd.blocks[0] != bshrub->rbyd.blocks[0];
}

static inline int lfsr_bshrub_cmp(
        const lfsr_bshrub_t *a,
        const lfsr_bshrub_t *b) {
    return lfsr_rbyd_cmp(&a->rbyd, &b->rbyd);
}

// these are used in mdir commit/compaction
static int lfsr_bshrub_commit__(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_bshrub_t *bshrub,
        lfs_size_t *trunk_, lfsr_srid_t *weight_,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // swap out our trunk/weight temporarily, note we're
    // operating on a copy so if this fails not _too_ many
    // things will get messed up
    //
    // it is important that these rbyds share eoff/cksum/etc
    lfs_size_t trunk = rbyd_->trunk;
    lfsr_srid_t weight = rbyd_->weight;
    rbyd_->trunk = bshrub->rbyd_.trunk;
    rbyd_->weight = bshrub->rbyd_.weight;

    // append any bshrub attributes
    for (lfs_size_t j = 0; j < attr_count; j++) {
        int err = lfsr_rbyd_appendattr(lfs, rbyd_,
                attrs[j].rid,
                LFSR_TAG_SHRUB | attrs[j].tag,
                attrs[j].delta,
                attrs[j].data);
        if (err) {
            return err;
        }
    }

    // restore mdir to the main trunk/weight
    if (trunk_) {
        *trunk_ = rbyd_->trunk;
    }
    if (weight_) {
        *weight_ = rbyd_->weight;
    }
    rbyd_->trunk = trunk;
    rbyd_->weight = weight;
    return 0;
}

static lfs_ssize_t lfsr_bshrub_estimate__(lfs_t *lfs,
        const lfsr_bshrub_t *bshrub) {
    // only include the last reference
    const lfsr_bshrub_t *last = NULL;
    for (lfsr_openedmdir_t *opened = lfs->opened[
                LFS_TYPE_REG-LFS_TYPE_REG];
            opened;
            opened = opened->next) {
        lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
        if (lfsr_ftree_isbshrub(ftree)
                && lfsr_bshrub_cmp(&ftree->u.bshrub, bshrub) == 0) {
            last = &ftree->u.bshrub;
        }
    }
    if (last && bshrub != last) {
        return 0;
    }

    return lfsr_rbyd_estimate(lfs, &bshrub->rbyd, -1, -1,
            NULL);
}

static int lfsr_bshrub_compact__(lfs_t *lfs, lfsr_rbyd_t *rbyd_,
        const lfsr_bshrub_t *bshrub, bool shrub,
        lfs_size_t *trunk_, lfsr_srid_t *weight_) {
    // only compact once, first compact should stage the new block
    if (shrub && bshrub->rbyd_.blocks[0] == rbyd_->blocks[0]) {
        return 0;
    }

    // save our current trunk/weight
    lfs_size_t trunk = rbyd_->trunk;
    lfsr_srid_t weight = rbyd_->weight;

    // compact our bshrub
    int err = lfsr_rbyd_appendshrub(lfs, rbyd_,
            &bshrub->rbyd);
    if (err) {
        return err;
    }

    // stage any opened shrubs with their new location so we can
    // update these later if our commit is a success
    //
    // this should include our current bshrub
    for (lfsr_openedmdir_t *opened = lfs->opened[
                LFS_TYPE_REG-LFS_TYPE_REG];
            opened;
            opened = opened->next) {
        lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
        if (lfsr_ftree_isbshrub(ftree)
                && lfsr_bshrub_cmp(&ftree->u.bshrub, bshrub) == 0) {
            ftree->u.bshrub.rbyd_ = *rbyd_;
        }
    }

    // revert rbyd trunk/weight
    if (trunk_) {
        *trunk_ = rbyd_->trunk;
    }
    if (weight_) {
        *weight_ = rbyd_->weight;
    }
    rbyd_->trunk = trunk;
    rbyd_->weight = weight;
    return 0;
}

// lfsr_bshruballoc is a bit of a misnomer, this doesn't alloc, just
// prepares a new bshrub in the given mdir
static int lfsr_bshrub_alloc(lfs_t *lfs,
        const lfsr_mdir_t *mdir, lfsr_bshrub_t *bshrub,
        lfs_size_t estimate) {
    (void)lfs;
    bshrub->rbyd.blocks[0] = mdir->rbyd.blocks[0];
    bshrub->rbyd.trunk = 0;
    bshrub->rbyd.weight = 0;
    bshrub->progged = estimate;
    return 0;
}

// bshrubs don't really need to be fetched since the mdir must be
// fetched, but we do need to find the bshrubs estimate
static int lfsr_bshrub_fetch(lfs_t *lfs,
        const lfsr_mdir_t *mdir, lfsr_bshrub_t *bshrub,
        lfs_size_t trunk, lfsr_rid_t weight) {
    bshrub->rbyd.blocks[0] = mdir->rbyd.blocks[0];
    bshrub->rbyd.trunk = trunk;
    bshrub->rbyd.weight = weight;

    // find an estimate of the current shrub size, we need this
    // to prevent our shrub from overflowing the mdir
    lfs_ssize_t estimate = lfsr_rbyd_estimate(lfs,
            &bshrub->rbyd, -1, -1, NULL);
    if (estimate < 0) {
        return estimate;
    }
    bshrub->progged = estimate;

    return 0;
}

static int lfsr_bshrub_lookupnext_(lfs_t *lfs,
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_rbyd_t *rbyd_, lfsr_srid_t *rid_,
        lfsr_tag_t *tag_, lfsr_bid_t *weight_, lfsr_data_t *data_) {
    (void)mdir;
    return lfsr_btree_lookupnext_(lfs, &bshrub->rbyd, bid,
            bid_, rbyd_, rid_, tag_, weight_, data_);
}

static int lfsr_bshrub_lookupnext(lfs_t *lfs,
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub,
        lfsr_bid_t bid,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bid_t *weight_,
        lfsr_data_t *data_) {
    (void)mdir;
    return lfsr_btree_lookupnext(lfs, &bshrub->rbyd, bid,
            bid_, tag_, weight_, data_);
}

static int lfsr_bshrub_lookup(lfs_t *lfs,
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub,
        lfsr_bid_t bid,
        lfsr_tag_t *tag_, lfsr_bid_t *weight_, lfsr_data_t *data_) {
    (void)mdir;
    return lfsr_btree_lookup(lfs, &bshrub->rbyd, bid,
            tag_, weight_, data_);
}

static int lfsr_bshrub_commit(lfs_t *lfs,
        lfsr_mdir_t *mdir, lfsr_bshrub_t *bshrub,
        const lfsr_attr_t *attrs, lfs_size_t attr_count) {
    // we need some scratch space for tail-recursive attrs
    // TODO combined scratch pool?
    lfsr_attr_t scratch_attrs[4];
    uint8_t scratch_buf[2*LFSR_BRANCH_DSIZE];

    // try to commit to the btree
    int err = lfsr_btree_commit_(lfs, &bshrub->rbyd,
            lfsr_bshrub_isbshrub(mdir, bshrub),
            scratch_attrs, scratch_buf,
            attrs, attr_count,
            &attrs, &attr_count);
    if (err) {
        return err;
    }

    // when btree is shrubbed, lfsr_btree_commit_ stops at the root
    // and returns with pending attrs
    //
    // note! lfsr_bshrub_isbshrub may have changed state due to collapsed
    // parents, splits, etc
    //
    if (attr_count > 0) {
        // new bshrub?
        if (bshrub->rbyd.trunk == 0) {
            err = lfsr_bshrub_alloc(lfs, mdir, bshrub,
                    LFSR_ATTR_ESTIMATE + LFSR_BTREE_DSIZE);
            if (err) {
                return err;
            }
        }

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
        lfs_size_t progged = 0;
        for (lfs_size_t i = 0; i < attr_count; i++) {
            // only include tag overhead if tag is not a grow tag
            if (!lfsr_tag_isgrow(attrs[i].tag)) {
                progged += LFSR_ATTR_ESTIMATE;
            }
            progged += lfsr_data_size(&attrs[i].data);
        }

        // does progged exceed our shrub_size? need to recalculate an
        // accurate our estimate?
        bshrub->progged += progged;
        if (bshrub->progged > lfs->cfg->shrub_size) {
            // include all unique sprouts/shrubs related to our file,
            // including the on-disk sprout/shrub
            lfs_size_t estimate = 0;
            lfsr_tag_t tag;
            lfsr_data_t data;
            err = lfsr_mdir_lookupnext(lfs, mdir, mdir->mid, LFSR_TAG_DATA,
                    &tag, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT && tag == LFSR_TAG_DATA) {
                lfs_ssize_t dsize = lfsr_bsprout_estimate__(lfs,
                        (const lfsr_bsprout_t*)&data);
                if (dsize < 0) {
                    return dsize;
                }
                estimate += lfsr_data_size(&data);

            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
                lfsr_rbyd_t shrub = mdir->rbyd;
                err = lfsr_data_readtrunk(lfs, &data,
                        &shrub.trunk, (lfsr_rid_t*)&shrub.weight);
                if (err) {
                    return err;
                }

                lfs_ssize_t dsize = lfsr_bshrub_estimate__(lfs,
                        (const lfsr_bshrub_t*)&shrub);
                if (dsize < 0) {
                    return dsize;
                }
                estimate += dsize;
            }

            // this includes our current shrub
            for (lfsr_openedmdir_t *opened = lfs->opened[
                        LFS_TYPE_REG-LFS_TYPE_REG];
                    opened;
                    opened = opened->next) {
                lfsr_ftree_t *ftree = (lfsr_ftree_t*)opened;
                if (ftree->mdir.mid == mdir->mid) {
                    if (lfsr_ftree_isbsprout(ftree)) {
                        lfs_ssize_t dsize = lfsr_bsprout_estimate__(lfs,
                                &ftree->u.bsprout);
                        if (dsize < 0) {
                            return dsize;
                        }
                        estimate += dsize;

                    } else if (lfsr_ftree_isbshrub(ftree)) {
                        lfs_ssize_t dsize = lfsr_bshrub_estimate__(lfs,
                                &ftree->u.bshrub);
                        if (dsize < 0) {
                            return dsize;
                        }
                        estimate += dsize;
                    }
                }
            }

            bshrub->progged = estimate + progged;

            // do we overflow shrub_size/2? the 1/2 here prevents runaway
            // performance when the shrub is near full
            if (bshrub->progged > lfs->cfg->shrub_size/2) {
                goto evict;
            }
        }

        // commit to shrub
        err = lfsr_mdir_commit(lfs, mdir, LFSR_ATTRS(
                LFSR_ATTR(mdir->mid,
                    SHRUBCOMMIT, 0, SHRUBCOMMIT(
                        bshrub, attrs, attr_count))));
        if (err) {
            return err;
        }
    }

    LFS_ASSERT(bshrub->rbyd.trunk != 0);
    return 0;

evict:;
    // convert to btree
    err = lfsr_rbyd_alloc(lfs, &bshrub->rbyd_);
    if (err) {
        return err;
    }

    err = lfsr_rbyd_compact(lfs, &bshrub->rbyd_, -1, -1,
            &bshrub->rbyd);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_RANGE);
        return err;
    }

    err = lfsr_rbyd_appendattrs(lfs, &bshrub->rbyd_, -1, -1,
            attrs, attr_count);
    if (err) {
        return err;
    }

    err = lfsr_rbyd_appendcksum(lfs, &bshrub->rbyd_);
    if (err) {
        return err;
    }

    bshrub->rbyd = bshrub->rbyd_;
    LFS_ASSERT(bshrub->rbyd.trunk != 0);
    return 0;
}

static int lfsr_bshrub_traverse(lfs_t *lfs,
        const lfsr_mdir_t *mdir, const lfsr_bshrub_t *bshrub,
        lfsr_btraversal_t *btraversal,
        lfsr_binfo_t *binfo) {
    // prevent bshrub root from being traversed, since this is just our mdir
    if (lfsr_bshrub_isbshrub(mdir, bshrub)
            && btraversal->branch.trunk == 0) {
        btraversal->branch = bshrub->rbyd;
    }

    return lfsr_btree_traverse(lfs, &bshrub->rbyd, btraversal,
            binfo);
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
        lfsr_btraversal_t mtraversal;
        // opened file state, only valid when traversing opened files
        const lfsr_openedmdir_t *opened;
    } u;
    lfsr_mdir_t mdir;
    lfsr_bshrub_t bshrub;
    lfsr_btraversal_t btraversal;
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
    LFSR_TRAVERSAL_MDIRBLOCK    = 3,
    LFSR_TRAVERSAL_MDIRBTREE    = 4,
    LFSR_TRAVERSAL_OPENEDBLOCK  = 5,
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

static inline bool lfsr_traversal_isall(const lfsr_traversal_t *traversal) {
    return traversal->flags & LFSR_TRAVERSAL_ALL;
}

static inline bool lfsr_traversal_isvalidate(
        const lfsr_traversal_t *traversal) {
    return traversal->flags & LFSR_TRAVERSAL_VALIDATE;
}

// info returned by mtree traveral
typedef struct lfsr_tinfo {
    lfsr_tag_t tag;
    union {
        lfsr_mdir_t mdir;
        lfsr_rbyd_t rbyd;
        lfsr_bptr_t bptr;
    } u;
} lfsr_tinfo_t;

static int lfsr_traversal_read(lfs_t *lfs, lfsr_traversal_t *traversal,
        lfsr_tinfo_t *tinfo) {
    while (true) {
        switch (traversal->state) {
        // start with the mrootanchor 0x{0,1}
        //
        // note we make sure to include all mroots in our mroot chain!
        //
        case LFSR_TRAVERSAL_MROOTANCHOR:;
            // fetch the first mroot 0x{0,1}
            int err = lfsr_mdir_fetch(lfs, &traversal->mdir,
                    -1, &LFSR_MPTR_MROOTANCHOR());
            if (err) {
                return err;
            }

            // transition to traversing the mroot chain
            traversal->state = LFSR_TRAVERSAL_MROOTCHAIN;

            tinfo->tag = LFSR_TAG_MDIR;
            tinfo->u.mdir = traversal->mdir;
            return 0;

        // traverse the mroot chain, checking for mroot/mtree/mdir
        case LFSR_TRAVERSAL_MROOTCHAIN:;
            // lookup mroot, if we find one this is a fake mroot
            lfsr_tag_t tag;
            lfsr_data_t data;
            err = lfsr_mdir_lookupwide(lfs, &traversal->mdir,
                    -1, LFSR_TAG_STRUCT,
                    &tag, &data);
            if (err) {
                // if we have no mtree/mdir (inlined mdir), we need to traverse
                // any files in our mroot next
                if (err == LFS_ERR_NOENT) {
                    traversal->mdir.mid = 0;
                    traversal->state = LFSR_TRAVERSAL_MDIRBLOCK;
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
                if (lfsr_mptr_cmp(&mptr, &traversal->u.mtortoise.mptr) == 0) {
                    LFS_ERROR("Cycle detected during mtree traversal "
                            "0x{%"PRIx32",%"PRIx32"}",
                            mptr.blocks[0],
                            mptr.blocks[1]);
                    return LFS_ERR_CORRUPT;
                }
                if (traversal->u.mtortoise.step
                        // TODO why cast?
                        == ((lfs_block_t)1 << traversal->u.mtortoise.power)) {
                    traversal->u.mtortoise.mptr = mptr;
                    traversal->u.mtortoise.step = 0;
                    traversal->u.mtortoise.power += 1;
                }
                traversal->u.mtortoise.step += 1;

                // fetch this mroot
                err = lfsr_mdir_fetch(lfs, &traversal->mdir, -1, &mptr);
                if (err) {
                    return err;
                }

                tinfo->tag = LFSR_TAG_MDIR;
                tinfo->u.mdir = traversal->mdir;
                return 0;

            // found an mdir?
            } else if (tag == LFSR_TAG_MDIR) {
                // fetch this mdir
                lfsr_mptr_t mptr;
                err = lfsr_data_readmptr(lfs, &data, &mptr);
                if (err) {
                    return err;
                }

                err = lfsr_mdir_fetch(lfs, &traversal->mdir, 0, &mptr);
                if (err) {
                    return err;
                }

                // transition to mdir traversal next
                traversal->state = LFSR_TRAVERSAL_MDIRBLOCK;

                tinfo->tag = LFSR_TAG_MDIR;
                tinfo->u.mdir = traversal->mdir;
                return 0;

            // found an mtree?
            } else if (tag == LFSR_TAG_MTREE) {
                // read the root of the mtree and return it, lfs->mtree may not
                // be initialized yet
                err = lfsr_data_readbtree(lfs, &data, &tinfo->u.rbyd);
                if (err) {
                    return err;
                }

                // validate our btree nodes if requested, this just means we
                // need to do a full rbyd fetch and make sure the checksums
                // match
                if (lfsr_traversal_isvalidate(traversal)) {
                    err = lfsr_rbyd_fetchvalidate(lfs, &tinfo->u.rbyd,
                            tinfo->u.rbyd.blocks[0], tinfo->u.rbyd.trunk,
                            tinfo->u.rbyd.weight,
                            tinfo->u.rbyd.cksum);
                    if (err) {
                        return err;
                    }
                }

                // transition to traversing the mtree
                traversal->state = LFSR_TRAVERSAL_MTREE;
                traversal->u.mtraversal = LFSR_BTRAVERSAL();

                tinfo->tag = LFSR_TAG_BRANCH;
                return 0;

            } else {
                LFS_ERROR("Weird mtree entry? 0x%"PRIx32, tag);
                return LFS_ERR_CORRUPT;
            }

        // traverse the mtree, including both inner btree nodes and mdirs
        case LFSR_TRAVERSAL_MTREE:;
            // no mtree? transition to traversing any opened mdirs
            if (lfsr_mtree_ismptr(lfs)) {
                traversal->u.opened = lfs->opened[LFS_TYPE_REG-LFS_TYPE_REG];
                traversal->state = LFSR_TRAVERSAL_OPENEDBLOCK;
                continue;
            }

            // traverse through the mtree
            lfsr_binfo_t binfo;
            err = lfsr_btree_traverse(lfs, &lfs->mtree.u.btree,
                    &traversal->u.mtraversal,
                    &binfo);
            if (err) {
                // end of mtree? transition to traversing any opened mdirs
                if (err == LFS_ERR_NOENT) {
                    traversal->u.opened
                            = lfs->opened[LFS_TYPE_REG-LFS_TYPE_REG];
                    traversal->state = LFSR_TRAVERSAL_OPENEDBLOCK;
                    continue;
                }
                return err;
            }

            // wait is this the mtree's root? skip this, we assume we've already
            // seen it above (this gets a bit weird because 1. mtree may be
            // uninitialized in mountinited and 2. stack really matters since
            // we're at the bottom of lfs_alloc)
            if (binfo.tag == LFSR_TAG_BRANCH
                    && binfo.u.rbyd.blocks[0] == lfs->mtree.u.btree.blocks[0]) {
                continue;
            }

            // inner btree nodes already decoded
            if (binfo.tag == LFSR_TAG_BRANCH) {
                // validate our btree nodes if requested, this just means we
                // need to do a full rbyd fetch and make sure the checksums
                // match
                if (lfsr_traversal_isvalidate(traversal)) {
                    err = lfsr_rbyd_fetchvalidate(lfs, &binfo.u.rbyd,
                            binfo.u.rbyd.blocks[0], binfo.u.rbyd.trunk,
                            binfo.u.rbyd.weight,
                            binfo.u.rbyd.cksum);
                    if (err) {
                        return err;
                    }
                }

                tinfo->tag = LFSR_TAG_BRANCH;
                tinfo->u.rbyd = binfo.u.rbyd;
                return 0;

            // fetch mdir if we're on a leaf
            } else if (binfo.tag == LFSR_TAG_MDIR) {
                lfsr_mptr_t mptr;
                err = lfsr_data_readmptr(lfs, &binfo.u.data, &mptr);
                if (err) {
                    return err;
                }

                err = lfsr_mdir_fetch(lfs, &traversal->mdir,
                        binfo.bid-(lfsr_mweight(lfs)-1),
                        &mptr);
                if (err) {
                    return err;
                }

                // transition to mdir traversal next
                traversal->state = LFSR_TRAVERSAL_MDIRBLOCK;

                tinfo->tag = LFSR_TAG_MDIR;
                tinfo->u.mdir = traversal->mdir;
                return 0;

            } else {
                LFS_ERROR("Weird mtree entry? 0x%"PRIx32, binfo.tag);
                return LFS_ERR_CORRUPT;
            }

        // scan for blocks/btrees in the current mdir
        case LFSR_TRAVERSAL_MDIRBLOCK:;
            // not traversing all blocks? have we exceeded our mdir's weight?
            // return to mtree traversal
            if (!lfsr_traversal_isall(traversal)
                    || lfsr_mdir_rid(lfs, &traversal->mdir)
                        >= traversal->mdir.rbyd.weight) {
                traversal->state = LFSR_TRAVERSAL_MTREE;
                continue;
            }

            // do we have a block/btree?
            err = lfsr_mdir_lookupnext(lfs, &traversal->mdir,
                    traversal->mdir.mid, LFSR_TAG_DATA,
                    &tag, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            // found a direct block?
            if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BLOCK) {
                err = lfsr_data_readbptr(lfs, &data, &tinfo->u.bptr);
                if (err) {
                    return err;
                }

                // TODO validate?

                // transition to next file
                traversal->mdir.mid += 1;

                tinfo->tag = LFSR_TAG_BLOCK;
                return 0;

            // found a bshrub (inlined btree)?
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
                traversal->bshrub.rbyd = traversal->mdir.rbyd;
                err = lfsr_data_readtrunk(lfs, &data,
                        &traversal->bshrub.rbyd.trunk,
                        (lfsr_rid_t*)&traversal->bshrub.rbyd.weight);
                if (err) {
                    return err;
                }

                // start traversing
                traversal->btraversal = LFSR_BTRAVERSAL();
                traversal->state = LFSR_TRAVERSAL_MDIRBTREE;
                continue;

            // found a btree?
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BTREE) {
                err = lfsr_data_readbtree(lfs, &data,
                        &traversal->bshrub.rbyd);
                if (err) {
                    return err;
                }

                // start traversing
                traversal->btraversal = LFSR_BTRAVERSAL();
                traversal->state = LFSR_TRAVERSAL_MDIRBTREE;
                continue;

            // no? continue to next file
            } else {
                traversal->mdir.mid += 1;
                continue;
            }

        // scan for blocks/btrees in our opened file list
        case LFSR_TRAVERSAL_OPENEDBLOCK:;
            // not traversing all blocks? reached end of opened file list?
            if (!lfsr_traversal_isall(traversal) || !traversal->u.opened) {
                traversal->state = LFSR_TRAVERSAL_DONE;
                continue;
            }

            const lfsr_ftree_t *ftree
                    = (const lfsr_ftree_t*)traversal->u.opened;
            // found a direct block?
            if (lfsr_ftree_isbleaf(ftree)) {
                tinfo->u.bptr = ftree->u.bptr;

                // TODO validate?

                // transition to next ftree
                traversal->u.opened = ftree->next;

                tinfo->tag = LFSR_TAG_BLOCK;
                return 0;

            // found a bshrub/btree?
            } else if (lfsr_ftree_isbshruborbtree(ftree)) {
                // start traversing
                traversal->mdir = ftree->mdir;
                traversal->bshrub = ftree->u.bshrub;
                traversal->btraversal = LFSR_BTRAVERSAL();
                traversal->state = LFSR_TRAVERSAL_OPENEDBTREE;
                continue;

            // no? continue to next ftree
            } else {
                traversal->u.opened = ftree->next;
                continue;
            }

        // traverse any file btrees, including both inner btree nodes and
        // block pointers
        case LFSR_TRAVERSAL_MDIRBTREE:;
        case LFSR_TRAVERSAL_OPENEDBTREE:;
            // traverse through our btree
            err = lfsr_bshrub_traverse(lfs,
                    &traversal->mdir, &traversal->bshrub,
                    &traversal->btraversal,
                    &binfo);
            if (err) {
                if (err == LFS_ERR_NOENT) {
                    // end of btree? go to next file
                    if (traversal->state == LFSR_TRAVERSAL_MDIRBTREE) {
                        traversal->mdir.mid += 1;
                        traversal->state = LFSR_TRAVERSAL_MDIRBLOCK;
                        continue;
                    } else if (traversal->state == LFSR_TRAVERSAL_OPENEDBTREE) {
                        traversal->u.opened = traversal->u.opened->next;
                        traversal->state = LFSR_TRAVERSAL_OPENEDBLOCK;
                        continue;
                    } else {
                        LFS_UNREACHABLE();
                    }
                }
                return err;
            }

            // found an inner btree node?
            if (binfo.tag == LFSR_TAG_BRANCH) {
                // validate our btree nodes if requested, this just means we
                // need to do a full rbyd fetch and make sure the checksums
                // match
                if (lfsr_traversal_isvalidate(traversal)) {
                    err = lfsr_rbyd_fetchvalidate(lfs, &binfo.u.rbyd,
                            binfo.u.rbyd.blocks[0], binfo.u.rbyd.trunk,
                            binfo.u.rbyd.weight,
                            binfo.u.rbyd.cksum);
                    if (err) {
                        return err;
                    }
                }

                tinfo->tag = LFSR_TAG_BRANCH;
                tinfo->u.rbyd = binfo.u.rbyd;
                return 0;

            // found inlined data? ignore this
            } else if (binfo.tag == LFSR_TAG_DATA) {
                continue;

            // found an indirect block?
            } else if (binfo.tag == LFSR_TAG_BLOCK) {
                err = lfsr_data_readbptr(lfs, &binfo.u.data,
                        &tinfo->u.bptr);
                if (err) {
                    return err;
                }

                // TODO validate?

                tinfo->tag = LFSR_TAG_BLOCK;
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
//    d_ = lfs_toleb128(lfsr_mweight(lfs)-1, &buffer[d], 5);
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
// - WCOMPAT => Must understand to write to the filesystem
// - RCOMPAT => Must understand to read the filesystem
//
// note, "understanding" does not necessarily mean support
//
enum lfsr_rcompat {
    LFSR_RCOMPAT_GRM = 0x01,
};

typedef uint8_t lfsr_rcompat_t;
typedef uint8_t lfsr_wcompat_t;

static inline bool lfsr_rcompat_hasgrm(lfsr_rcompat_t rcompat) {
    return rcompat & LFSR_RCOMPAT_GRM;
}

static inline bool lfsr_rcompat_hasunknown(lfsr_rcompat_t rcompat) {
    return rcompat & ~LFSR_RCOMPAT_GRM;
}


/// Filesystem init functions ///

static int lfs_init(lfs_t *lfs, const struct lfs_config *cfg);
static int lfs_deinit(lfs_t *lfs);

static int lfsr_mountmroot(lfs_t *lfs, const lfsr_mdir_t *mroot) {
    // has magic string?
    lfsr_data_t data;
    int err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_MAGIC,
            &data);
    if (err) {
        if (err == LFS_ERR_NOENT) {
            LFS_ERROR("No littlefs magic found");
            return LFS_ERR_INVAL;
        }
        return err;
    }

    lfs_scmp_t cmp = lfsr_data_cmp(lfs, &data, "littlefs", 8);
    if (cmp < 0) {
        return cmp;
    }

    // treat corrupted magic as no magic
    if (lfs_cmp(cmp) != 0) {
        LFS_ERROR("No littlefs magic found");
        return LFS_ERR_INVAL;
    }

    // check the disk version
    err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_VERSION,
            &data);
    if (err) {
        if (err == LFS_ERR_NOENT) {
            LFS_ERROR("No littlefs version found");
            return LFS_ERR_INVAL;
        }
        return err;
    }

    uint32_t major_version;
    err = lfsr_data_readleb128(lfs, &data, (int32_t*)&major_version);
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    if (err == LFS_ERR_CORRUPT) {
        major_version = -1;
    }

    uint32_t minor_version;
    err = lfsr_data_readleb128(lfs, &data, (int32_t*)&minor_version);
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    if (err == LFS_ERR_CORRUPT) {
        minor_version = -1;
    }

    if (major_version != LFS_DISK_VERSION_MAJOR
            || minor_version > LFS_DISK_VERSION_MINOR) {
        LFS_ERROR("Incompatible version v%"PRId32".%"PRId32
                " (!= v%"PRId32".%"PRId32")",
                major_version,
                minor_version,
                LFS_DISK_VERSION_MAJOR,
                LFS_DISK_VERSION_MINOR);
        return LFS_ERR_INVAL;
    }

    // check for any rcompatflags, we must understand these to read
    // the filesystem
    err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_RCOMPATFLAGS,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err == LFS_ERR_NOENT) {
        data = LFSR_DATA_NULL();
    }

    lfsr_rcompat_t rcompat;
    lfs_ssize_t size = lfsr_data_read(lfs, &data, &rcompat, 1);
    if (size < 0) {
        return size;
    }
    if (size < 1) {
        rcompat = 0;
    }

    // unknown rcompat flags? flags must be tightly sized
    if (lfsr_rcompat_hasunknown(rcompat) || lfsr_data_size(&data) > 0) {
        LFS_ERROR("Incompatible rcompat flags 0x%s%"PRIx8,
                (lfsr_data_size(&data) > 0) ? "??" : "",
                rcompat);
        return LFS_ERR_INVAL;
    }

    // grm supported?
    if (!lfsr_rcompat_hasgrm(rcompat)) {
        LFS_ERROR("Incompatible rcompat flags, no grm");
        // TODO switch to read-only? upgrade?
        return LFS_ERR_INVAL;
    }

    // check for any wcompatflags, we must understand these to write
    // the filesystem
    err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_WCOMPATFLAGS,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    if (err == LFS_ERR_NOENT) {
        data = LFSR_DATA_NULL();
    }

    // unknown wcompat flags? flags must be tightly sized
    if (lfsr_data_size(&data) > 0) {
        LFS_ERROR("Incompatible wcompat flags 0x??");
        // TODO switch to read-only?
        return LFS_ERR_INVAL;
    }

    // check block size
    err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_BLOCKSIZE,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    uint32_t block_size = 0;
    if (err != LFS_ERR_NOENT) {
        err = lfsr_data_readleb128(lfs, &data, (int32_t*)&block_size);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        if (err == LFS_ERR_CORRUPT) {
            block_size = -1;
        }
    }

    if (block_size != lfs->cfg->block_size-1) {
        LFS_ERROR("Incompatible block size %"PRId32" (!= %"PRId32")",
                block_size+1,
                lfs->cfg->block_size);
        return LFS_ERR_INVAL;
    }

    // check block count
    err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_BLOCKCOUNT,
            &data);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    uint32_t block_count = 0;
    if (err != LFS_ERR_NOENT) {
        err = lfsr_data_readleb128(lfs, &data,
                (int32_t*)&block_count);
        if (err && err != LFS_ERR_CORRUPT) {
            return err;
        }
        if (err == LFS_ERR_CORRUPT) {
            block_count = -1;
        }
    }

    if (block_count != lfs->cfg->block_count-1) {
        LFS_ERROR("Incompatible block count %"PRId32" (!= %"PRId32")",
                block_count+1,
                lfs->cfg->block_count);
        return LFS_ERR_INVAL;
    }

    // read the name limit
    err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_NAMELIMIT,
            &data);
    if (err) {
        if (err == LFS_ERR_NOENT) {
            LFS_ERROR("No name limit found");
            return LFS_ERR_INVAL;
        }
        return err;
    }

    uint32_t name_limit;
    err = lfsr_data_readleb128(lfs, &data, (int32_t*)&name_limit);
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    if (err == LFS_ERR_CORRUPT) {
        name_limit = -1;
    }

    if (name_limit > lfs->name_limit) {
        LFS_ERROR("Incompatible name limit (%"PRId32" > %"PRId32")",
                name_limit,
                lfs->name_limit);
        return LFS_ERR_INVAL;
    }

    lfs->name_limit = name_limit;

    // read the size limit
    err = lfsr_mdir_lookup(lfs, mroot, -1, LFSR_TAG_SIZELIMIT,
            &data);
    if (err) {
        if (err == LFS_ERR_NOENT) {
            LFS_ERROR("No size limit found");
            return LFS_ERR_INVAL;
        }
        return err;
    }

    uint32_t size_limit;
    err = lfsr_data_readleb128(lfs, &data, (int32_t*)&size_limit);
    if (err && err != LFS_ERR_CORRUPT) {
        return err;
    }
    if (err == LFS_ERR_CORRUPT) {
        size_limit = -1;
    }

    if (size_limit > lfs->size_limit) {
        LFS_ERROR("Incompatible size limit (%"PRId32" > %"PRId32")",
                size_limit,
                lfs->size_limit);
        return LFS_ERR_INVAL;
    }

    lfs->size_limit = size_limit;

    // check for unknown configs
    lfsr_tag_t tag;
    err = lfsr_mdir_lookupnext(lfs, mroot, -1, LFSR_TAG_SIZELIMIT+1,
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
    // note that lfsr_traversal_next will update our mroot/mtree
    // based on what mroots it finds
    //
    // we do validate btree inner nodes here, how can we trust our
    // mdirs are valid if we haven't checked the btree inner nodes at
    // least once?
    lfsr_traversal_t traversal = LFSR_TRAVERSAL(LFSR_TRAVERSAL_VALIDATE);
    while (true) {
        lfsr_tinfo_t tinfo;
        int err = lfsr_traversal_read(lfs, &traversal, &tinfo);
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
                err = lfsr_mountmroot(lfs, &tinfo.u.mdir);
                if (err) {
                    return err;
                }

                // keep track of the last mroot we see, this is the
                // active mroot
                lfs->mroot = tinfo.u.mdir;

            } else {
                // found a direct mdir? keep track of this
                if (lfsr_mtree_isnull(lfs)) {
                    lfs->mtree = LFSR_MTREE_MPTR(
                            *lfsr_mdir_mptr(&tinfo.u.mdir),
                            lfsr_mweight(lfs));
                }

                // found an empty non-mroot mdir? this should only happen
                // if we lost power
                if (tinfo.u.mdir.rbyd.weight == 0) {
                    LFS_DEBUG("Found orphaned mdir %"PRId32" "
                            "0x{%"PRIx32",%"PRIx32"}",
                            tinfo.u.mdir.mid >> lfs->mbits,
                            tinfo.u.mdir.rbyd.blocks[0],
                            tinfo.u.mdir.rbyd.blocks[1]);
                    lfs->hasorphans = true;
                }
            }

            // collect any gdeltas from this mdir
            err = lfsr_fs_consumegdelta(lfs, &tinfo.u.mdir);
            if (err) {
                return err;
            }

        // found an mtree inner-node?
        } else if (tinfo.tag == LFSR_TAG_BRANCH) {
            // found the root of the mtree? keep track of this
            if (lfsr_mtree_isnull(lfs)) {
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
    memcpy(lfs->grm_g, lfs->grm_d, LFSR_GRM_DSIZE);

    // decode grm so we can report any removed files as missing
    int err = lfsr_data_readgrm(lfs, &LFSR_DATA_BUF(lfs->grm_g, LFSR_GRM_DSIZE),
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
                    lfsr_mid_bid(lfs, lfs->grm.rms[0]) >> lfs->mbits,
                    lfsr_mid_rid(lfs, lfs->grm.rms[0]),
                    lfsr_mid_bid(lfs, lfs->grm.rms[1]) >> lfs->mbits,
                    lfsr_mid_rid(lfs, lfs->grm.rms[1]));
        } else if (lfsr_grm_count(&lfs->grm) == 1) {
            LFS_DEBUG("Found pending grm %"PRId32".%"PRId32,
                    lfsr_mid_bid(lfs, lfs->grm.rms[0]) >> lfs->mbits,
                    lfsr_mid_rid(lfs, lfs->grm.rms[0]));
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

        // note the initial revision count is arbitrary, but we use
        // -1 and 0 here to help test that our sequence comparison
        // works correctly
        err = lfsr_rbyd_appendrev(lfs, &rbyd, (uint32_t)i - 1);
        if (err) {
            return err;
        }

        // our initial superblock contains a couple things:
        // - our magic string, "littlefs"
        // - any format-time configuration
        // - the root's bookmark tag, which reserves did = 0 for the root
        err = lfsr_rbyd_commit(lfs, &rbyd, LFSR_ATTRS(
                LFSR_ATTR(-1, MAGIC,        0, BUF("littlefs", 8)),
                LFSR_ATTR(-1, VERSION,      0, IMM(((const uint8_t[2]){
                    LFS_DISK_VERSION_MAJOR,
                    LFS_DISK_VERSION_MINOR}), 2)),
                LFSR_ATTR(-1, RCOMPATFLAGS, 0, IMM((&(uint8_t){
                    LFSR_RCOMPAT_GRM}), 1)),
                LFSR_ATTR(-1, BLOCKSIZE,    0, LEB128(lfs->cfg->block_size-1)),
                LFSR_ATTR(-1, BLOCKCOUNT,   0, LEB128(lfs->cfg->block_count-1)),
                LFSR_ATTR(-1, NAMELIMIT,    0, LEB128(lfs->name_limit)),
                LFSR_ATTR(-1, SIZELIMIT,    0, LEB128(lfs->size_limit)),
                LFSR_ATTR(0, BOOKMARK, +1, LEB128(0))));
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
            lfs->mroot.rbyd.trunk,
            lfsr_mtree_weight(lfs) / lfsr_mweight(lfs),
            lfsr_mweight(lfs));

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
                // lfs_alloc_ckpoint makes available for scanning
                while (true) {
                    lfs->lookahead.next += 1;
                    lfs->lookahead.ckpoint -= 1;

                    if (lfs->lookahead.next >= lfs->lookahead.size
                            || !(lfs->lookahead.buffer[lfs->lookahead.next / 8]
                                & (1 << (lfs->lookahead.next % 8)))) {
                        return 0;
                    }
                }
            }

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
        memset(lfs->lookahead.buffer, 0, lfs->cfg->lookahead_size);

        // traverse the filesystem, building up knowledge of what blocks are
        // in use in our lookahead window
        lfsr_traversal_t traversal = LFSR_TRAVERSAL(LFSR_TRAVERSAL_ALL);
        while (true) {
            lfsr_tinfo_t tinfo;
            int err = lfsr_traversal_read(lfs, &traversal, &tinfo);
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
    lfsr_traversal_t traversal = LFSR_TRAVERSAL(LFSR_TRAVERSAL_ALL);
    while (true) {
        lfsr_tinfo_t tinfo;
        int err = lfsr_traversal_read(lfs, &traversal, &tinfo);
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
        LFS_ASSERT(lfs->grm.rms[0] < lfs_smax32(
                lfsr_mtree_weight(lfs),
                lfsr_mweight(lfs)));
        int err = lfsr_mtree_lookup(lfs, lfs->grm.rms[0],
                &mdir);
        if (err) {
            return err;
        }

        // mark grm as taken care of
        lfsr_grm_t grm = lfs->grm;
        lfsr_grm_poprm(&grm);

        // make sure to adjust any remaining grms
        if (lfsr_mid_bid(lfs, grm.rms[0]) == lfsr_mdir_bid(lfs, &mdir)
                && grm.rms[0] >= mdir.mid) {
            LFS_ASSERT(grm.rms[0] != mdir.mid);
            grm.rms[0] -= 1;
        }

        // remove the rid while also updating our grm
        LFS_ASSERT(lfsr_mid_rid(lfs, lfs->grm.rms[0]) < mdir.rbyd.weight);
        err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
                LFSR_ATTR(mdir.mid, RM, -1, NULL()),
                LFSR_ATTR(-1, GRM, 0, GRM(&grm))));
    }

    return 0;
}

static int lfsr_fs_fixorphans(lfs_t *lfs) {
    // traverse the filesystem and drop any orphaned mdirs
    //
    // note this never takes longer than lfsr_mount
    //
    lfsr_traversal_t traversal = LFSR_TRAVERSAL(0);
    while (true) {
        lfsr_tinfo_t tinfo;
        int err = lfsr_traversal_read(lfs, &traversal, &tinfo);
        if (err) {
            if (err == LFS_ERR_NOENT) {
                break;
            }
            return err;
        }

        // found an orphaned mdir? drop
        if (tinfo.tag == LFSR_TAG_MDIR
                && tinfo.u.mdir.mid != -1
                && tinfo.u.mdir.rbyd.weight == 0) {
            err = lfsr_mdir_drop(lfs, &tinfo.u.mdir);
            if (err) {
                return err;
            }

            // TODO should we have a function for this?
            // TODO should traversals be "opened" and updated by
            // lfsr_mdir_commit/drop?
            //
            // dropping an orphan changes our mtree, we need to partially
            // invalidate out traversal
            LFS_ASSERT(traversal.state == LFSR_TRAVERSAL_MDIRBLOCK);
            traversal.state = LFSR_TRAVERSAL_MDIRBLOCK;
            traversal.u.mtraversal.bid -= lfsr_mweight(lfs);
            traversal.u.mtraversal.rid = traversal.u.mtraversal.bid;
            traversal.u.mtraversal.branch = lfs->mtree.u.btree;
        }
    }

    return 0;
}

static int lfsr_fs_preparemutation(lfs_t *lfs) {
    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);

    // fix pending grms
    bool pl = false;
    if (lfsr_grm_hasrm(&lfs->grm)) {
        LFS_DEBUG("Fixing pending grms...");
        pl = true;

        int err = lfsr_fs_fixgrm(lfs);
        if (err) {
            return err;
        }

        // checkpoint the allocator again since fixgrm completed
        // some work
        lfs_alloc_ckpoint(lfs);
    }

    // fix orphaned mdirs
    //
    // this must happen after fixgrm, since dropping mdirs risks outdating
    // the grm, fixgrm can also create temporary orphans, but it should
    // immediately clean them up
    //
    if (lfs->hasorphans) {
        LFS_DEBUG("Fixing orphaned mdirs...");
        pl = true;

        int err = lfsr_fs_fixorphans(lfs);
        if (err) {
            return err;
        }

        // checkpoint the allocator again since fixorphans completed
        // some work
        lfs_alloc_ckpoint(lfs);
    }

    if (pl) {
        LFS_DEBUG("littlefs is now consistent");
    }
    return 0;
}


/// Directory operations ///

int lfsr_mkdir(lfs_t *lfs, const char *path) {
    // prepare our filesystem for writing
    int err = lfsr_fs_preparemutation(lfs);
    if (err) {
        return err;
    }

    // lookup our parent
    lfsr_mdir_t mdir;
    lfsr_did_t did;
    const char *name;
    lfs_size_t name_size;
    err = lfsr_mtree_pathlookup(lfs, path,
            &mdir, NULL,
            &did, &name, &name_size);
    if (err && (err != LFS_ERR_NOENT || mdir.mid == -1)) {
        return err;
    }

    // already exists?
    if (err != LFS_ERR_NOENT) {
        return LFS_ERR_EXIST;
    }

    // check that name fits
    if (name_size > lfs->name_limit) {
        return LFS_ERR_NAMETOOLONG;
    }

    // Our directory needs an arbitrary directory-rid. To find one with
    // hopefully few collisions, we use a hash of the full path using our CRC,
    // since we have it handy.
    //
    // We also truncate to make better use of our leb128 encoding. This is
    // relatively arbitrary, but if we truncate too much we risk increasing
    // the number of collisions, so we want to aim for ~2x the number dids
    // in the system. We don't actually know the number of dids in the system,
    // but we can use a heuristic based on the maximum possible number of
    // directories in the current mtree assuming our block size.
    //
    // - Each directory needs 1 name tag, 1 did tag, and 1 bookmark
    // - Each tag needs ~2 alts+null with our current compaction strategy
    // - Each tag/alt encodes to a minimum of 4 bytes
    // - We can also assume ~1/2 block utilization due to our split threshold
    //
    // This gives us ~3*4*4*2 or ~96 bytes per directory at minimum.
    // Multiplying by 2 and rounding down to the nearest power of 2 for cheaper
    // division gives us a heuristic of ~block_size/32 directories per mdir.
    //
    // This is a nice number because for common NOR flash geometry,
    // 4096/32 = 128, so a filesystem with a single mdir encodes dids in a
    // single byte.
    //
    // Note we also need to be careful to catch integer overflow.
    //
    lfsr_did_t dmask = (1 << lfs_min32(
            lfs_nlog2(lfsr_mtree_weight(lfs))
                + lfs_nlog2(lfs->cfg->block_size/32),
            32)) - 1;
    lfsr_did_t did_ = lfs_crc32c(0, path, strlen(path)) & dmask;

    // Check if we have a collision. If we do, search for the next
    // available did
    lfsr_openedmdir_t bookmark;
    while (true) {
        err = lfsr_mtree_namelookup(lfs, did_, NULL, 0,
                &bookmark.mdir, NULL, NULL);
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

    // A problem: we need to create both 1. the metadata entry and 2. the
    // bookmark entry.
    //
    // To do this atomically, we first create the metadata entry with a grm
    // to delete-self in case of powerloss, then create the bookmark while
    // atomically cancelling the grm.
    //
    // These commits can change the relative mids of each other, so we track
    // the bookmark mdir as an "open file" temporarily.
    //
    // Note! The metadata/bookmark order is important! Attempting to create
    // the bookmark first risks inserting the bookmark before the metadata
    // entry, which breaks things.
    //
    lfsr_mdir_addopened(lfs, LFS_TYPE_INTERNAL, &bookmark);

    // commit our new directory into our parent, creating a grm to self-remove
    // in case of powerloss
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
            LFSR_ATTR(mdir.mid,
                DIR, +1, CAT(
                    LFSR_DATA_LEB128(did),
                    LFSR_DATA_BUF(name, name_size))),
            LFSR_ATTR(mdir.mid, DID, 0, LEB128(did_)),
            LFSR_ATTR(-1, GRM, 0, GRM(&((lfsr_grm_t){{mdir.mid, -1}})))));
    if (err) {
        goto failed_with_bookmark;
    }

    lfsr_mdir_removeopened(lfs, LFS_TYPE_INTERNAL, &bookmark);

    // commit our bookmark and zero the grm, the bookmark tag is an empty
    // entry that marks our did as allocated
    err = lfsr_mdir_commit(lfs, &bookmark.mdir, LFSR_ATTRS(
            LFSR_ATTR(bookmark.mdir.mid, BOOKMARK, +1, LEB128(did_)),
            LFSR_ATTR(-1, GRM, 0, GRM(&((lfsr_grm_t){{-1, -1}})))));
    if (err) {
        return err;
    }

    return 0;

failed_with_bookmark:
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, &bookmark);
    return err;
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
    err = lfsr_mtree_pathlookup(lfs, path,
            &mdir, &tag,
            NULL, NULL, NULL);
    if (err) {
        return err;
    }

    // as funny as it would be, you can't remove the root
    if (lfsr_mdir_isroot(&mdir)) {
        return LFS_ERR_INVAL;
    }

    // if we're removing a directory, we need to also remove the
    // bookmark entry
    lfsr_grm_t grm = lfs->grm;
    if (tag == LFSR_TAG_DIR) {
        // first lets figure out the did
        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, mdir.mid, LFSR_TAG_DID,
                &data);
        if (err) {
            return err;
        }

        lfsr_did_t did;
        err = lfsr_data_readleb128(lfs, &data, (int32_t*)&did);
        if (err) {
            return err;
        }

        // then lookup the bookmark entry
        lfsr_mdir_t bookmark_mdir;
        err = lfsr_mtree_namelookup(lfs, did, NULL, 0,
                &bookmark_mdir, NULL, NULL);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // create a grm to remove the bookmark entry
        lfsr_grm_pushrm(&grm, bookmark_mdir.mid);

        // check that the directory is empty
        err = lfsr_mtree_seek(lfs, &bookmark_mdir, 1);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        if (err != LFS_ERR_NOENT) {
            lfsr_tag_t bookmark_tag;
            err = lfsr_mdir_lookupwide(lfs, &bookmark_mdir,
                    bookmark_mdir.mid, LFSR_TAG_NAME,
                    &bookmark_tag, NULL);
            if (err) {
                return err;
            }

            if (bookmark_tag != LFSR_TAG_BOOKMARK) {
                return LFS_ERR_NOTEMPTY;
            }
        }

        // adjust rid if grm is on the same mdir as our dir
        if (lfsr_mid_bid(lfs, grm.rms[0]) == lfsr_mdir_bid(lfs, &mdir)
                && grm.rms[0] > mdir.mid) {
            grm.rms[0] -= 1;
        }
    }

    // remove the metadata entry
    err = lfsr_mdir_commit(lfs, &mdir, LFSR_ATTRS(
            LFSR_ATTR(mdir.mid, RM, -1, NULL()),
            LFSR_ATTR(-1, GRM, 0, GRM(&grm))));
    if (err) {
        return err;
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
    err = lfsr_mtree_pathlookup(lfs, old_path,
            &old_mdir, &old_tag,
            NULL, NULL, NULL);
    if (err) {
        return err;
    }

    // as funny as it would be, you can't rename the root
    if (lfsr_mdir_isroot(&old_mdir)) {
        return LFS_ERR_INVAL;
    }

    // mark old entry for removal with a grm
    lfsr_grm_t grm = lfs->grm;
    lfsr_grm_pushrm(&grm, old_mdir.mid);

    // lookup new entry
    lfsr_mdir_t new_mdir;
    lfsr_tag_t new_tag;
    lfsr_did_t new_did;
    const char *new_name;
    lfs_size_t new_name_size;
    err = lfsr_mtree_pathlookup(lfs, new_path,
            &new_mdir, &new_tag,
            &new_did, &new_name, &new_name_size);
    if (err && (err != LFS_ERR_NOENT || new_mdir.mid == -1)) {
        return err;
    }
    bool exists = (err != LFS_ERR_NOENT);

    // there are a few cases we need to watch out for
    if (!exists) {
        // check that name fits
        if (new_name_size > lfs->name_limit) {
            return LFS_ERR_NAMETOOLONG;
        }

        // adjust old rid if grm is on the same mdir as new rid
        if (lfsr_mid_bid(lfs, grm.rms[0]) == lfsr_mdir_bid(lfs, &new_mdir)
                && grm.rms[0] >= new_mdir.mid) {
            grm.rms[0] += 1;
        }

    } else {
        // renaming different types is an error
        if (old_tag != new_tag) {
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
            err = lfsr_mdir_lookup(lfs, &new_mdir,
                    new_mdir.mid, LFSR_TAG_DID,
                    &data);
            if (err) {
                return err;
            }

            lfsr_did_t did;
            err = lfsr_data_readleb128(lfs, &data, (int32_t*)&did);
            if (err) {
                return err;
            }

            // then lookup the bookmark entry
            lfsr_mdir_t bookmark_mdir;
            err = lfsr_mtree_namelookup(lfs, did, NULL, 0,
                    &bookmark_mdir, NULL, NULL);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // create a grm to remove the bookmark entry
            lfsr_grm_pushrm(&grm, bookmark_mdir.mid);

            // check that the directory is empty
            err = lfsr_mtree_seek(lfs, &bookmark_mdir, 1);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err != LFS_ERR_NOENT) {
                lfsr_tag_t bookmark_tag;
                err = lfsr_mdir_lookupwide(lfs, &bookmark_mdir,
                        bookmark_mdir.mid, LFSR_TAG_NAME,
                        &bookmark_tag, NULL);
                if (err) {
                    return err;
                }

                if (bookmark_tag != LFSR_TAG_BOOKMARK) {
                    return LFS_ERR_NOTEMPTY;
                }
            }
        }
    }

    // rename our entry, copying all tags associated with the old rid to the
    // new rid, while also marking the old rid for removal
    err = lfsr_mdir_commit(lfs, &new_mdir, LFSR_ATTRS(
            (exists
                ? LFSR_ATTR(new_mdir.mid, RM, -1, NULL())
                : LFSR_ATTR_NOOP()),
            LFSR_ATTR(new_mdir.mid,
                TAG(old_tag), +1, CAT(
                    LFSR_DATA_LEB128(new_did),
                    LFSR_DATA_BUF(new_name, new_name_size))),
            LFSR_ATTR(new_mdir.mid, MOVE, 0, MOVE(&old_mdir)),
            LFSR_ATTR(-1, GRM, 0, GRM(&grm))));
    if (err) {
        return err;
    }

    // we need to clean up any pending grms, fortunately we can leave
    // this up to lfsr_fs_fixgrm
    return lfsr_fs_fixgrm(lfs);
}

// common stat function once we have an mdir
static int lfsr_mdir_stat(lfs_t *lfs, lfsr_mdir_t *mdir, lfsr_mid_t mid,
        lfsr_sdid_t did, struct lfs_info *info) {
    // lookup our name tag
    lfsr_tag_t tag;
    lfsr_data_t data;
    int err = lfsr_mdir_lookupwide(lfs, mdir, mid, LFSR_TAG_NAME,
            &tag, &data);
    if (err) {
        return err;
    }

    // get our did
    lfsr_did_t did_;
    err = lfsr_data_readleb128(lfs, &data, (int32_t*)&did_);
    if (err) {
        return err;
    }

    // did mismatch? this terminates dir reads
    if (did != -1 && did_ != (lfsr_did_t)did) {
        return LFS_ERR_NOENT;
    }

    // get file type from the tag
    info->type = lfsr_tag_subtype(tag);

    // get file name from the name entry
    LFS_ASSERT(lfsr_data_size(&data) <= LFS_NAME_MAX);
    lfs_ssize_t name_size = lfsr_data_read(lfs, &data,
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
        err = lfsr_mdir_lookupnext(lfs, mdir, mid, LFSR_TAG_DATA,
                &tag, &data);
        if (err && err != LFS_ERR_NOENT) {
            return err;
        }

        // may be a sprout (simple inlined data)
        if (err != LFS_ERR_NOENT && tag == LFSR_TAG_DATA) {
            info->size = lfsr_data_size(&data);

        // or a block/bshrub/btree, size is always first field here
        } else if (err != LFS_ERR_NOENT
                && (tag == LFSR_TAG_BLOCK
                    || tag == LFSR_TAG_BSHRUB
                    || tag == LFSR_TAG_BTREE)) {
            err = lfsr_data_readleb128(lfs, &data, (int32_t*)&info->size);
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
    int err = lfsr_mtree_pathlookup(lfs, path,
            &mdir, &tag,
            NULL, &name, &name_size);
    if (err) {
        return err;
    }

    // special case for root
    if (lfsr_mdir_isroot(&mdir)) {
        strcpy(info->name, "/");
        info->type = LFS_TYPE_DIR;
        return 0;
    }

    // fill out our info struct
    return lfsr_mdir_stat(lfs, &mdir, mdir.mid, -1, info);
}

int lfsr_dir_open(lfs_t *lfs, lfsr_dir_t *dir, const char *path) {
    // lookup our directory
    lfsr_mdir_t mdir;
    lfsr_tag_t tag;
    int err = lfsr_mtree_pathlookup(lfs, path,
            &mdir, &tag,
            NULL, NULL, NULL);
    if (err) {
        return err;
    }

    // are we a directory?
    if (tag != LFSR_TAG_DIR) {
        return LFS_ERR_NOTDIR;
    }

    // read our did from the mdir, unless we're root
    if (lfsr_mdir_isroot(&mdir)) {
        dir->did = 0;
    } else {
        lfsr_data_t data;
        err = lfsr_mdir_lookup(lfs, &mdir, mdir.mid, LFSR_TAG_DID,
                &data);
        if (err) {
            return err;
        }

        err = lfsr_data_readleb128(lfs, &data, (int32_t*)&dir->did);
        if (err) {
            return err;
        }
    }

    // let rewind initialize the pos/bookmark state
    dir->bookmark = 0;
    err = lfsr_dir_rewind(lfs, dir);
    if (err) {
        return err;
    }

    // add to tracked mdirs
    lfsr_mdir_addopened(lfs, LFS_TYPE_DIR, (lfsr_openedmdir_t*)dir);
    return 0;
}

int lfsr_dir_close(lfs_t *lfs, lfsr_dir_t *dir) {
    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_DIR, (lfsr_openedmdir_t*)dir);
    return 0;
}

int lfsr_dir_read(lfs_t *lfs, lfsr_dir_t *dir, struct lfs_info *info) {
    // was our dir removed?
    if (dir->bookmark == -1) {
        return LFS_ERR_NOENT;
    }

    // handle dots specially
    if (dir->pos == 0) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, ".");
        dir->pos += 1;
        return 0;
    } else if (dir->pos == 1) {
        info->type = LFS_TYPE_DIR;
        strcpy(info->name, "..");
        dir->pos += 1;
        return 0;
    }

    // seek in case our mdir was dropped
    int err = lfsr_mtree_seek(lfs, &dir->mdir, 0);
    if (err) {
        return err;
    }

    // fill out our info struct
    //
    // this will return LFS_ERR_NOENT if our dids mismatch
    err = lfsr_mdir_stat(lfs, &dir->mdir, dir->mdir.mid, dir->did, info);
    if (err) {
        return err;
    }

    // eagerly look up the next entry
    err = lfsr_mtree_seek(lfs, &dir->mdir, 1);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }
    dir->pos += 1;

    return 0;
}

int lfsr_dir_seek(lfs_t *lfs, lfsr_dir_t *dir, lfs_soff_t off) {
    // do nothing if removed
    if (dir->bookmark == -1) {
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
        err = lfsr_mtree_seek(lfs, &dir->mdir, off - 2);
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
    if (dir->bookmark == -1) {
        return 0;
    }

    // lookup our bookmark in the mtree
    int err = lfsr_mtree_namelookup(lfs, dir->did, NULL, 0,
            &dir->mdir, NULL, NULL);
    if (err) {
        LFS_ASSERT(err != LFS_ERR_NOENT);
        return err;
    }

    // keep track of bookmark so we can adjust pos correctly
    dir->bookmark = dir->mdir.mid;
    dir->pos = 0;

    // eagerly lookup the next entry
    //
    // this makes handling of corner cases with mixed removes/dir reads easier
    err = lfsr_mtree_seek(lfs, &dir->mdir, 1);
    if (err && err != LFS_ERR_NOENT) {
        return err;
    }

    return 0;
}


/// File operations ///

// sign(weight)=1, data.size==0           => null
// sign(weight)=1, data.block==mdir.block => bsprout
// sign(weight)=1, data.block!=mdir.block => bleaf
// sign(weight)=0, data.block==mdir.block => bshrub
// sign(weight)=0, data.block!=mdir.block => btree

#define LFSR_FTREE_ISNULLORBSPROUTORBLEAF 0x80000000

#define LFSR_FTREE_NULL (LFSR_FTREE_ISNULLORBSPROUTORBLEAF | 0)

static inline bool lfsr_ftree_isnull(const lfsr_ftree_t *ftree) {
    return (lfs_size_t)ftree->u.bsprout.data.u.disk.size
            == (LFSR_FTREE_ISNULLORBSPROUTORBLEAF | 0);
}

static inline bool lfsr_ftree_isbsprout(const lfsr_ftree_t *ftree) {
    return (lfs_size_t)ftree->u.bsprout.data.u.disk.size
                > (LFSR_FTREE_ISNULLORBSPROUTORBLEAF | 0)
            && lfsr_bsprout_isbsprout(&ftree->mdir, &ftree->u.bsprout);
}

static inline bool lfsr_ftree_isbleaf(const lfsr_ftree_t *ftree) {
    return (lfs_size_t)ftree->u.bsprout.data.u.disk.size
                > (LFSR_FTREE_ISNULLORBSPROUTORBLEAF | 0)
            && lfsr_bsprout_isbleaf(&ftree->mdir, &ftree->u.bsprout);
}

static inline bool lfsr_ftree_isbshrub(const lfsr_ftree_t *ftree) {
    return !(ftree->u.bsprout.data.u.disk.size
                & LFSR_FTREE_ISNULLORBSPROUTORBLEAF)
            && lfsr_bshrub_isbshrub(&ftree->mdir, &ftree->u.bshrub);
}

static inline bool lfsr_ftree_isbtree(const lfsr_ftree_t *ftree) {
    return !(ftree->u.bsprout.data.u.disk.size
                & LFSR_FTREE_ISNULLORBSPROUTORBLEAF)
            && lfsr_bshrub_isbtree(&ftree->mdir, &ftree->u.bshrub);
}

static inline bool lfsr_ftree_isbshruborbtree(const lfsr_ftree_t *ftree) {
    return !(ftree->u.bsprout.data.u.disk.size
            & LFSR_FTREE_ISNULLORBSPROUTORBLEAF);
}

// the on-disk size/weight lines up to the same word across all unions
static inline lfs_off_t lfsr_ftree_size(const lfsr_ftree_t *ftree) {
    return ftree->u.bsprout.data.u.disk.size
            & ~LFSR_FTREE_ISNULLORBSPROUTORBLEAF;
}

// flag things
static inline bool lfsr_o_isreadable(uint32_t flags) {
    return (flags & LFS_O_RDONLY) == LFS_O_RDONLY;
}

static inline bool lfsr_o_iswriteable(uint32_t flags) {
    return (flags & LFS_O_WRONLY) == LFS_O_WRONLY;
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

static inline bool lfsr_f_isunflushed(uint32_t flags) {
    return flags & LFS_F_UNFLUSHED;
}

static inline bool lfsr_f_isunsynced(uint32_t flags) {
    return flags & LFS_F_UNSYNCED;
}

static inline bool lfsr_f_iserrored(uint32_t flags) {
    return flags & LFS_F_ERRORED;
}

// file operations

// needed in lfsr_file_opencfg
static lfs_ssize_t lfsr_ftree_read(lfs_t *lfs, const lfsr_ftree_t *ftree,
        lfs_off_t pos, uint8_t *buffer, lfs_size_t size);

int lfsr_file_opencfg(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs_file_config *cfg) {
    if (lfsr_o_iswriteable(flags)) {
        // prepare our filesystem for writing
        int err = lfsr_fs_preparemutation(lfs);
        if (err) {
            return err;
        }
    }

    // setup file state
    file->flags = flags;
    file->cfg = cfg;
    file->pos = 0;
    file->size = 0;
    // default data state
    file->ftree.u.size = LFSR_FTREE_NULL;

    // lookup our parent
    lfsr_tag_t tag;
    lfsr_did_t did;
    const char *name;
    lfs_size_t name_size;
    int err = lfsr_mtree_pathlookup(lfs, path,
            &file->ftree.mdir, &tag,
            &did, &name, &name_size);
    if (err && (err != LFS_ERR_NOENT || file->ftree.mdir.mid == -1)) {
        return err;
    }

    // creating a new entry?
    if (err == LFS_ERR_NOENT) {
        if (!lfsr_o_iscreat(flags)) {
            return LFS_ERR_NOENT;
        }
        LFS_ASSERT(lfsr_o_iswriteable(flags));

        // check that name fits
        if (name_size > lfs->name_limit) {
            return LFS_ERR_NAMETOOLONG;
        }

        // create our entry
        //
        // note this risks creating a zero-length file if we lose power here,
        // but it's the only way for us to save the file name.
        //
        // TODO or is it? ;)
        err = lfsr_mdir_commit(lfs, &file->ftree.mdir, LFSR_ATTRS(
                LFSR_ATTR(file->ftree.mdir.mid,
                    REG, +1, CAT(
                        LFSR_DATA_LEB128(did),
                        LFSR_DATA_BUF(name, name_size)))));
        if (err) {
            return err;
        }
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
            err = lfsr_mdir_lookupnext(lfs, &file->ftree.mdir,
                    file->ftree.mdir.mid, LFSR_TAG_DATA,
                    &tag, &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            // TODO the above clobbers data on failure, which is why we can't
            // lookup into the inlined data directly. Should this be avoided?
            // Should we at least be consistent in this codebase?

            // may be a sprout (simple inlined data)
            if (err != LFS_ERR_NOENT && tag == LFSR_TAG_DATA) {
                file->ftree.u.bsprout.data = data;

            // or a direct block
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BLOCK) {
                err = lfsr_data_readbptr(lfs, &data, &file->ftree.u.bptr);
                if (err) {
                    return err;
                }

                // also fetch the becksum here if we are writable
                file->ftree.u.bleaf.becksum.size = -1;
                if (lfsr_o_iswriteable(flags)) {
                    lfsr_data_t data;
                    err = lfsr_mdir_lookupnext(lfs, &file->ftree.mdir,
                            file->ftree.mdir.mid, LFSR_TAG_BECKSUM,
                            NULL, &data);
                    if (err && err != LFS_ERR_NOENT) {
                        return err;
                    }

                    err = lfsr_data_readecksum(lfs, &data,
                            &file->ftree.u.bleaf.becksum);
                    if (err) {
                        return err;
                    }
                }

            // or a bshrub (inlined btree)
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BSHRUB) {
                lfs_size_t trunk;
                lfsr_rid_t weight;
                err = lfsr_data_readtrunk(lfs, &data, &trunk, &weight);
                if (err) {
                    return err;
                }

                int err = lfsr_bshrub_fetch(lfs,
                        &file->ftree.mdir, &file->ftree.u.bshrub,
                        trunk, weight);
                if (err) {
                    return err;
                }

            // or a btree
            } else if (err != LFS_ERR_NOENT && tag == LFSR_TAG_BTREE) {
                err = lfsr_data_readbtree(lfs, &data, &file->ftree.u.btree);
                if (err) {
                    return err;
                }
            }

            file->size = lfsr_ftree_size(&file->ftree);
        }
    }

    // allocate buffer if necessary
    if (file->cfg->buffer) {
        file->buffer = file->cfg->buffer;
    } else {
        file->buffer = lfs_malloc(lfs->cfg->cache_size);
        if (!file->buffer) {
            return LFS_ERR_NOMEM;
        }
    }
    file->buffer_pos = 0;
    file->buffer_size = 0;

    // if our file is small, try to keep the whole thing in our buffer
    if (file->size <= lfs->cfg->cache_size
            && file->size <= lfs->cfg->inline_size
            && file->size <= lfs->cfg->fragment_size) {
        lfs_ssize_t d = lfsr_ftree_read(lfs, &file->ftree,
                0, file->buffer, file->size);
        if (d < 0) {
            err = d;
            goto failed_with_buffer;
        }

        // small files remain perpetually unflushed
        file->flags |= LFS_F_UNFLUSHED;
        file->buffer_pos = 0;
        file->buffer_size = file->size;
        file->ftree.u.size = LFSR_FTREE_NULL;
    }

    // add to tracked mdirs
    lfsr_mdir_addopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)file);

    return 0;

failed_with_buffer:;
    // clean up memory
    if (!file->cfg->buffer) {
        lfs_free(file->buffer);
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
    int err = lfsr_file_sync(lfs, file);

    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)file);

    // clean up memory
    if (!file->cfg->buffer) {
        lfs_free(file->buffer);
    }

    return err;
}

// low-level ftree operations

static int lfsr_ftree_lookupnext(lfs_t *lfs, const lfsr_ftree_t *ftree,
        lfs_off_t pos,
        lfsr_bid_t *bid_, lfsr_tag_t *tag_, lfsr_bid_t *weight_,
        lfsr_bptr_t *bptr_, lfsr_ecksum_t *becksum_) {
    if (pos >= lfsr_ftree_size(ftree)) {
        return LFS_ERR_NOENT;
    }
    // the above size check should make this impossible
    LFS_ASSERT(!lfsr_ftree_isnull(ftree));

    // inlined sprout?
    if (lfsr_ftree_isbsprout(ftree)) {
        if (bid_) {
            *bid_ = lfsr_data_size(&ftree->u.bsprout.data)-1;
        }
        if (tag_) {
            *tag_ = LFSR_TAG_DATA;
        }
        if (weight_) {
            *weight_ = lfsr_data_size(&ftree->u.bsprout.data);
        }
        if (bptr_) {
            bptr_->data = ftree->u.bsprout.data;
        }
        if (becksum_) {
            becksum_->size = -1;
        }
        return 0;

    // block pointer?
    } else if (lfsr_ftree_isbleaf(ftree)) {
        if (bid_) {
            *bid_ = lfsr_data_size(&ftree->u.bptr.data)-1;
        }
        if (tag_) {
            *tag_ = LFSR_TAG_BLOCK;
        }
        if (weight_) {
            *weight_ = lfsr_data_size(&ftree->u.bptr.data);
        }
        if (bptr_) {
            *bptr_ = ftree->u.bptr;
        }
        if (becksum_) {
            *becksum_ = ftree->u.bleaf.becksum;
        }
        return 0;

    // bshrub/btree?
    } else {
        lfsr_bid_t bid;
        lfsr_rbyd_t rbyd;
        lfsr_srid_t rid;
        lfsr_tag_t tag;
        lfsr_bid_t weight;
        lfsr_data_t data;
        int err = lfsr_bshrub_lookupnext_(lfs,
                &ftree->mdir, &ftree->u.bshrub,
                pos,
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
            LFS_ASSERT(lfsr_data_size(&bptr_->data) <= weight);
        }
        if (becksum_) {
            // need an extra lookup to find becksums
            err = lfsr_rbyd_lookup(lfs, &rbyd, rid, LFSR_TAG_BECKSUM,
                    &data);
            if (err && err != LFS_ERR_NOENT) {
                return err;
            }

            if (err == LFS_ERR_NOENT) {
                becksum_->size = -1;
            } else {
                err = lfsr_data_readecksum(lfs, &data, becksum_);
                if (err) {
                    return err;
                }
            }
        }
        return 0;
    }
}

static lfs_ssize_t lfsr_ftree_readnext(lfs_t *lfs, const lfsr_ftree_t *ftree,
        lfs_off_t pos, uint8_t *buffer, lfs_size_t size) {
    lfs_off_t pos_ = pos;
    // read one btree entry
    lfsr_bid_t bid;
    lfsr_tag_t tag;
    lfsr_bid_t weight;
    lfsr_bptr_t bptr;
    int err = lfsr_ftree_lookupnext(lfs, ftree, pos_,
            &bid, &tag, &weight, &bptr, NULL);
    if (err) {
        return err;
    }

    // any data on disk?
    if (pos_ < bid-(weight-1) + lfsr_data_size(&bptr.data)) {
        // note one important side-effect here is a strict
        // data hint
        lfs_ssize_t d = lfs_min32(
                size,
                lfsr_data_size(&bptr.data)
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
    memset(buffer, 0, d);

    pos_ += d;
    buffer += d;
    size -= d;

    return pos_ - pos;
}

static lfs_ssize_t lfsr_ftree_read(lfs_t *lfs, const lfsr_ftree_t *ftree,
        lfs_off_t pos, uint8_t *buffer, lfs_size_t size) {
    lfs_off_t pos_ = pos;
    while (size > 0 && pos_ < lfsr_ftree_size(ftree)) {
        lfs_ssize_t d = lfsr_ftree_readnext(lfs, ftree,
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

static int lfsr_ftree_carve(lfs_t *lfs, lfsr_ftree_t *ftree,
        lfs_off_t pos, lfs_off_t weight, lfs_soff_t delta,
        lfsr_tag_t tag, const lfsr_bptr_t *bptr, const lfsr_ecksum_t *becksum) {
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

    // always convert to bshrub/btree when this function is called
    if (!lfsr_ftree_isbshruborbtree(ftree)) {
        lfsr_attr_t attrs_[2];
        lfs_size_t attr_count_ = 0;
        uint8_t buf[LFSR_BPTR_DSIZE+LFSR_ECKSUM_DSIZE];
        lfs_size_t buf_size = 0;
        lfs_size_t estimate = 0;

        // these also check if ftree is non-zero
        if (lfsr_ftree_isbsprout(ftree)) {
            attrs_[attr_count_++] = LFSR_ATTR(0,
                    DATA, +lfsr_ftree_size(ftree),
                    DATA(ftree->u.bsprout.data));
            estimate += LFSR_ATTR_ESTIMATE + lfsr_ftree_size(ftree);

        } else if (lfsr_ftree_isbleaf(ftree)) {
            attrs_[attr_count_++] = LFSR_ATTR(0,
                    BLOCK, +lfsr_ftree_size(ftree),
                    FROMBPTR(&ftree->u.bptr, &buf[buf_size]));
            buf_size += LFSR_BPTR_DSIZE;
            estimate += LFSR_ATTR_ESTIMATE + LFSR_BPTR_DSIZE;

            // append becksum?
            if (ftree->u.bleaf.becksum.size != -1) {
                attrs_[attr_count_++] = LFSR_ATTR(lfsr_ftree_size(ftree)-1,
                        BECKSUM, 0,
                        FROMECKSUM(&ftree->u.bleaf.becksum, &buf[buf_size]));
                buf_size += LFSR_ECKSUM_DSIZE;
                estimate += LFSR_ATTR_ESTIMATE + LFSR_ECKSUM_DSIZE;
            }
        }

        int err = lfsr_bshrub_alloc(lfs, &ftree->mdir, &ftree->u.bshrub,
                estimate);
        if (err) {
            return err;
        }

        if (attr_count_ > 0) {
            LFS_ASSERT(attr_count_ <= sizeof(attrs_)/sizeof(lfsr_attr_t));
            LFS_ASSERT(buf_size <= sizeof(buf));
            err = lfsr_bshrub_commit(lfs, &ftree->mdir, &ftree->u.bshrub,
                    attrs_, attr_count_);
            if (err) {
                return err;
            }
        }
    }

    // TODO adopt this pattern for other scratch attrs
    //
    // try to merge commits where possible
    lfsr_attr_t attrs_[5];
    lfs_size_t attr_count_ = 0;
    uint8_t buf[3*LFSR_BPTR_DSIZE+2*LFSR_ECKSUM_DSIZE];
    lfs_size_t buf_size = 0;

    // try to carve any existing data
    while (pos < lfsr_ftree_size(ftree)) {
        lfsr_bid_t bid_;
        lfsr_tag_t tag_;
        lfsr_bid_t weight_;
        lfsr_bptr_t bptr_;
        lfsr_ecksum_t becksum_;
        int err = lfsr_ftree_lookupnext(lfs, ftree,
                pos,
                &bid_, &tag_, &weight_, &bptr_, &becksum_);
        if (err) {
            LFS_ASSERT(err != LFS_ERR_NOENT);
            return err;
        }

        // note, an entry can be both a left and right sibling
        lfsr_data_t left_slice_ = lfsr_data_slice(bptr_.data,
                -1,
                pos - (bid_-(weight_-1)));
        lfsr_data_t right_slice_ = lfsr_data_slice(bptr_.data,
                pos+weight - (bid_-(weight_-1)),
                -1);

        // left sibling needs carving but falls underneath our
        // crystallization threshold? break into fragments
        while (tag_ == LFSR_TAG_BLOCK
                && lfsr_data_size(&left_slice_) > lfs->cfg->fragment_size
                && lfsr_data_size(&left_slice_) < lfs->cfg->crystal_thresh) {
            bptr_.data = lfsr_data_slice(bptr_.data,
                    lfs->cfg->fragment_size,
                    -1);

            err = lfsr_bshrub_commit(lfs, &ftree->mdir, &ftree->u.bshrub,
                    LFSR_ATTRS(
                        LFSR_ATTR(bid_,
                            GROW(WIDE(DATA)),
                                -(weight_ - lfs->cfg->fragment_size),
                            DATA(lfsr_data_truncate(left_slice_,
                                lfs->cfg->fragment_size))),
                        LFSR_ATTR(bid_-(weight_ - lfs->cfg->fragment_size)+1,
                            BLOCK, +(weight_ - lfs->cfg->fragment_size),
                            FROMBPTR(&bptr_, buf))));
            if (err) {
                return err;
            }

            weight_ -= lfs->cfg->fragment_size;
            left_slice_ = lfsr_data_slice(bptr_.data,
                    -1,
                    pos - (bid_-(weight_-1)));
        }

        // right sibling needs carving but falls underneath our
        // crystallization threshold? break into fragments
        while (tag_ == LFSR_TAG_BLOCK
                && lfsr_data_size(&right_slice_) > lfs->cfg->fragment_size
                && lfsr_data_size(&right_slice_) < lfs->cfg->crystal_thresh) {
            bptr_.data = lfsr_data_truncate(bptr_.data,
                    lfsr_data_size(&bptr_.data) - lfs->cfg->fragment_size);

            err = lfsr_bshrub_commit(lfs, &ftree->mdir, &ftree->u.bshrub,
                    LFSR_ATTRS(
                        LFSR_ATTR(bid_,
                            GROW(WIDE(BLOCK)),
                                -(weight_ - lfsr_data_size(&bptr_.data)),
                            FROMBPTR(&bptr_, buf)),
                        LFSR_ATTR(bid_
                                -(weight_ - lfsr_data_size(&bptr_.data))+1,
                            DATA, +(weight_ - lfsr_data_size(&bptr_.data)),
                            DATA(lfsr_data_fruncate(right_slice_,
                                lfs->cfg->fragment_size)))));
            if (err) {
                return err;
            }

            bid_ -= (weight_-lfsr_data_size(&bptr_.data));
            weight_ -= (weight_-lfsr_data_size(&bptr_.data));
            right_slice_ = lfsr_data_slice(bptr_.data,
                    pos+weight - (bid_-(weight_-1)),
                    -1);
        }

        // found left sibling?
        if (bid_-(weight_-1) < pos) {
            // can we get away with a grow attribute?
            if (lfsr_data_size(&bptr_.data) == lfsr_data_size(&left_slice_)) {
                attrs_[attr_count_++] = LFSR_ATTR(bid_,
                        GROW, -(bid_+1 - pos), NULL());

            // carve bptr?
            } else if (tag_ == LFSR_TAG_BLOCK) {
                lfsr_bptr_t bptr__ = {
                    .data = left_slice_,
                    .cksize = bptr_.cksize,
                    .cksum = bptr_.cksum,
                };
                attrs_[attr_count_++] = LFSR_ATTR(bid_,
                        GROW(WIDE(BLOCK)), -(bid_+1 - pos),
                        FROMBPTR(&bptr__, &buf[buf_size]));
                buf_size += LFSR_BPTR_DSIZE;

            // carve fragment?
            } else {
                attrs_[attr_count_++] = LFSR_ATTR(bid_,
                        GROW(WIDE(DATA)), -(bid_+1 - pos),
                        DATA(left_slice_));
            }

        // completely overwriting this entry?
        } else {
            attrs_[attr_count_++] = LFSR_ATTR(bid_,
                    RM, -weight_, NULL());
        }

        // spans more than one entry? we can't do everything in one commit,
        // so commit what we have and move on to next entry
        if (pos+weight > bid_+1) {
            LFS_ASSERT(lfsr_data_size(&right_slice_) == 0);
            LFS_ASSERT(attr_count_ <= sizeof(attrs_)/sizeof(lfsr_attr_t));
            LFS_ASSERT(buf_size <= sizeof(buf));

            err = lfsr_bshrub_commit(lfs, &ftree->mdir, &ftree->u.bshrub,
                    attrs_, attr_count_);
            if (err) {
                return err;
            }

            delta += lfs_min32(weight, bid_+1 - pos);
            weight -= lfs_min32(weight, bid_+1 - pos);
            attr_count_ = 0;
            buf_size = 0;
            continue;
        }

        // found right sibling?
        if (pos+weight < bid_+1) {
            // can we coalesce a hole?
            if (lfsr_data_size(&right_slice_) == 0) {
                delta += bid_+1 - (pos+weight);

            // carve bptr?
            } else if (tag_ == LFSR_TAG_BLOCK) {
                lfsr_bptr_t bptr__ = {
                    .data = right_slice_,
                    .cksize = bptr_.cksize,
                    .cksum = bptr_.cksum,
                };
                attrs_[attr_count_++] = LFSR_ATTR(pos,
                        BLOCK, +(bid_+1 - (pos+weight)),
                        FROMBPTR(&bptr__, &buf[buf_size]));
                buf_size += LFSR_BPTR_DSIZE;

                // copy over becksum since erase-state is still valid
                if (becksum_.size != -1) {
                    attrs_[attr_count_++] = LFSR_ATTR(
                            pos + (bid_+1 - (pos+weight)) - 1,
                            BECKSUM, 0,
                            FROMECKSUM(&becksum_, &buf[buf_size]));
                    buf_size += LFSR_ECKSUM_DSIZE;
                }

            // carve fragment?
            } else {
                attrs_[attr_count_++] = LFSR_ATTR(pos,
                        DATA, +(bid_+1 - (pos+weight)),
                        DATA(right_slice_));
            }
        }

        delta += lfs_min32(weight, bid_+1 - pos);
        weight -= lfs_min32(weight, bid_+1 - pos);
        break;
    }

    // need a hole?
    if (pos > lfsr_ftree_size(ftree)) {
        // can we coalesce?
        if (lfsr_ftree_size(ftree) > 0) {
            attrs_[attr_count_++] = LFSR_ATTR(lfsr_ftree_size(ftree)-1,
                    GROW, +(pos - lfsr_ftree_size(ftree)), NULL());

        // new hole
        } else {
            attrs_[attr_count_++] = LFSR_ATTR(lfsr_ftree_size(ftree),
                    DATA, +(pos - lfsr_ftree_size(ftree)), NULL());
        }
    }

    // finally append our data
    if (weight + delta > 0) {
        // can we coalesce a hole?
        if ((!bptr || lfsr_data_size(&bptr->data) == 0) && pos > 0) {
            attrs_[attr_count_++] = LFSR_ATTR(pos-1,
                    GROW, +(weight + delta), NULL());

        // need a new hole?
        } else if (!bptr || lfsr_data_size(&bptr->data) == 0) {
            attrs_[attr_count_++] = LFSR_ATTR(pos,
                    DATA, +(weight + delta), NULL());

        // append new fragment?
        } else if (tag == LFSR_TAG_DATA) {
            attrs_[attr_count_++] = LFSR_ATTR(pos,
                    DATA, +(weight + delta), DATA(bptr->data));

        // append a new block?
        } else if (tag == LFSR_TAG_BLOCK) {
            attrs_[attr_count_++] = LFSR_ATTR(pos,
                    BLOCK, +(weight + delta),
                    FROMBPTR(bptr, &buf[buf_size]));
            buf_size += LFSR_BPTR_DSIZE;

            // append becksum?
            if (becksum && becksum->size != -1) {
                attrs_[attr_count_++] = LFSR_ATTR(pos+weight+delta-1,
                        BECKSUM, 0,
                        FROMECKSUM(becksum, &buf[buf_size]));
                buf_size += LFSR_ECKSUM_DSIZE;
            }
        }
    }

    // commit pending attrs
    if (attr_count_ > 0) {
        LFS_ASSERT(attr_count_ <= sizeof(attrs_)/sizeof(lfsr_attr_t));
        LFS_ASSERT(buf_size <= sizeof(buf));

        int err = lfsr_bshrub_commit(lfs, &ftree->mdir, &ftree->u.bshrub,
                attrs_, attr_count_);
        if (err) {
            return err;
        }
    }

    return 0;
}

static int lfsr_ftree_flush(lfs_t *lfs, lfsr_ftree_t *ftree,
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
                    < (lfs_soff_t)lfsr_ftree_size(ftree)
                && lfsr_ftree_size(ftree) > 0
                // don't bother to lookup left after the first block
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            lfsr_ecksum_t becksum;
            int err = lfsr_ftree_lookupnext(lfs, ftree,
                    lfs_smax32(pos - (lfs->cfg->crystal_thresh-1), 0),
                    &bid, &tag, &weight, &bptr, &becksum);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // if left crystal neighbor is a fragment and there is no hole
            // between our own crystal and our neighbor, include as a part
            // of our crystal
            if (tag == LFSR_TAG_DATA
                    && bid-(weight-1)+lfsr_data_size(&bptr.data)
                        >= pos - (lfs->cfg->crystal_thresh-1)) {
                crystal_start = bid-(weight-1);

            // otherwise our neighbor determines our crystal boundary
            } else {
                crystal_start = lfs_min32(bid+1, pos);

                // wait, found block-level erased-state?
                if (tag == LFSR_TAG_BLOCK
                        && becksum.size != -1
                        // data not truncated?
                        && bptr.data.u.disk.off + lfsr_data_size(&bptr.data)
                            == bptr.cksize
                        // not clobbering data?
                        && crystal_start - (bid-(weight-1))
                            >= lfsr_data_size(&bptr.data)
                        // enough for prog alignment?
                        && crystal_end - crystal_start
                            >= lfs->cfg->prog_size) {
                    LFS_ASSERT(bptr.cksize + becksum.size
                            <= lfs->cfg->block_size);

                    err = lfsr_ecksum_validate(lfs, &becksum,
                            bptr.data.u.disk.block, bptr.cksize);
                    if (err && err != LFS_ERR_CORRUPT) {
                        return err;
                    }

                    // found _valid_ block-level erased-state? eagerly
                    // append
                    if (err != LFS_ERR_CORRUPT) {
                        block_start = bid-(weight-1);
                        goto compact;
                    }
                }
            }
        }

        // if we haven't already exceeded our crystallization threshold,
        // find right crystal neighbor
        if (crystal_end - crystal_start < lfs->cfg->crystal_thresh
                && lfsr_ftree_size(ftree) > 0) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            int err = lfsr_ftree_lookupnext(lfs, ftree,
                    lfs_min32(
                        crystal_start + (lfs->cfg->crystal_thresh-1),
                        lfsr_ftree_size(ftree)-1),
                    &bid, &tag, &weight, &bptr, NULL);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // if right crystal neighbor is a fragment, include as a part
            // of our crystal
            if (tag == LFSR_TAG_DATA) {
                crystal_end = lfs_max32(
                        bid-(weight-1)+lfsr_data_size(&bptr.data),
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
            break;
        }

        // exceeded our crystallization threshold? compact into a new block

        // before we can compact we need to figure out the best block
        // alignment, we use the entry immediately to the left of our
        // crystal for this
        block_start = crystal_start;
        if (crystal_start > 0
                && lfsr_ftree_size(ftree) > 0
                // don't bother to lookup left after the first block
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            lfsr_ecksum_t becksum;
            int err = lfsr_ftree_lookupnext(lfs, ftree,
                    lfs_min32(
                        crystal_start-1,
                        lfsr_ftree_size(ftree)-1),
                    &bid, &tag, &weight, &bptr, &becksum);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // is our left neighbor in the same block?
            if (crystal_start - (bid-(weight-1))
                        < lfs->cfg->block_size
                    && lfsr_data_size(&bptr.data) > 0) {
                block_start = bid-(weight-1);

                // wait, found block-level erased-state?
                if (tag == LFSR_TAG_BLOCK
                        && becksum.size != -1
                        // data not truncated?
                        && bptr.data.u.disk.off + lfsr_data_size(&bptr.data)
                            == bptr.cksize
                        // not clobbering data?
                        && crystal_start - (bid-(weight-1))
                            >= lfsr_data_size(&bptr.data)
                        // enough for prog alignment?
                        && crystal_end - crystal_start
                            >= lfs->cfg->prog_size) {
                    LFS_ASSERT(bptr.cksize + becksum.size
                            <= lfs->cfg->block_size);

                    err = lfsr_ecksum_validate(lfs, &becksum,
                            bptr.data.u.disk.block, bptr.cksize);
                    if (err && err != LFS_ERR_CORRUPT) {
                        return err;
                    }

                    // found _valid_ block-level erased-state? eagerly
                    // append
                    if (err != LFS_ERR_CORRUPT) {
                        goto compact;
                    }
                }

            // no? is our left neighbor at least our left block neighbor?
            // align to block alignment
            } else if (crystal_start - (bid-(weight-1))
                        < 2*lfs->cfg->block_size
                    && lfsr_data_size(&bptr.data) > 0) {
                block_start = bid-(weight-1) + lfs->cfg->block_size;
            }
        }

        // allocate a new block
        int err = lfs_alloc(lfs, &bptr.data.u.disk.block);
        if (err) {
            return err;
        }

        // TODO should lfs_alloc handle erase?
        err = lfsr_bd_erase(lfs, bptr.data.u.disk.block);
        if (err) {
            return err;
        }

        bptr.data = LFSR_DATA_DISK(bptr.data.u.disk.block, 0, 0);
        bptr.cksize = 0;
        bptr.cksum = 0;

    compact:;
        // compact data into our new block
        //
        // eagerly merge any right neighbors we see unless that would
        // put us over our block size
        lfs_off_t pos_ = block_start + lfsr_data_size(&bptr.data);
        while (pos_ < lfs_min32(
                block_start
                    + (lfs->cfg->block_size - bptr.data.u.disk.off),
                lfs_max32(
                    pos + size,
                    lfsr_ftree_size(ftree)))) {
            // keep track of the next highest priority data offset
            lfs_ssize_t d = lfs_min32(
                    block_start
                        + (lfs->cfg->block_size - bptr.data.u.disk.off),
                    lfs_max32(
                        pos + size,
                        lfsr_ftree_size(ftree))) - pos_;

            // any data in our buffer?
            if (pos_ < pos + size && size > 0) {
                if (pos_ >= pos) {
                    lfs_ssize_t d_ = lfs_min32(
                            d,
                            size - (pos_ - pos));
                    err = lfsr_bd_prog(lfs, bptr.data.u.disk.block,
                            bptr.cksize,
                            &buffer[pos_ - pos], d_,
                            NULL, &bptr.cksum);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_RANGE);
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
            if (pos_ < lfsr_ftree_size(ftree)) {
                lfsr_bid_t bid_;
                lfsr_tag_t tag_;
                lfsr_bid_t weight_;
                lfsr_bptr_t bptr_;
                err = lfsr_ftree_lookupnext(lfs, ftree, pos_,
                        &bid_, &tag_, &weight_, &bptr_, NULL);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_NOENT);
                    return err;
                }

                // make sure to include all of our crystal, or else this
                // loop may never terminate
                if (bid_-(weight_-1) >= crystal_end
                        // is this data a pure hole? stop early to better
                        // leverage becksums in sparse files
                        && (pos_ >= bid_-(weight_-1)
                                + lfsr_data_size(&bptr_.data)
                            // does this data exceed our block_size?
                            // stop early to try to avoid messing up
                            // block alignment
                            || bid_-(weight_-1) + lfsr_data_size(&bptr_.data)
                                    - block_start
                                > lfs->cfg->block_size)) {
                    break;
                }

                if (pos_ < bid_-(weight_-1) + lfsr_data_size(&bptr_.data)) {
                    // note one important side-effect here is a strict
                    // data hint
                    lfs_ssize_t d_ = lfs_min32(
                            d,
                            lfsr_data_size(&bptr_.data)
                                - (pos_ - (bid_-(weight_-1))));
                    err = lfsr_bd_progdata(lfs, bptr.data.u.disk.block,
                            bptr.cksize,
                            lfsr_data_slice(bptr_.data,
                                pos_ - (bid_-(weight_-1)),
                                d_),
                            NULL, &bptr.cksum);
                    if (err) {
                        LFS_ASSERT(err != LFS_ERR_RANGE);
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
            // TODO do something better than byte-level progs here
            for (lfs_size_t i = 0; i < (lfs_size_t)d; i++) {
                err = lfsr_bd_prog(lfs, bptr.data.u.disk.block,
                        bptr.cksize + i,
                        &(uint8_t){0}, 1,
                        NULL, &bptr.cksum);
                if (err) {
                    LFS_ASSERT(err != LFS_ERR_RANGE);
                    return err;
                }
            }

            pos_ += d;
            bptr.cksize += d;
        }

        // A bit of a hack here, we need to truncate our block to prog_size
        // alignment to avoid padding issues. Doing this retroactively to
        // the pcache greatly simplifies the above loop, though we may end
        // up reading more than is strictly necessary.
        lfs_ssize_t d = bptr.cksize % lfs->cfg->prog_size;
        LFS_ASSERT(d <= lfs->pcache.size);
        lfs->pcache.size -= d;
        bptr.cksize -= d;

        // TODO validate?
        // finalize our write
        err = lfsr_bd_flush(lfs,
                &bptr.cksum);
        if (err) {
            return err;
        }

        // TODO this is a cludge, but right now our bd layer is a mess,
        // we need caches to be clean so becksum calculation does not pick
        // up out-of-date pcaches/rcaches
        lfs_cache_drop(lfs, &lfs->pcache);
        lfs_cache_drop(lfs, &lfs->rcache);

        // prepare our block pointer
        LFS_ASSERT(bptr.cksize > 0);
        LFS_ASSERT(bptr.cksize <= lfs->cfg->block_size);
        bptr.data = LFSR_DATA_DISK(
                bptr.data.u.disk.block,
                bptr.data.u.disk.off,
                bptr.cksize - bptr.data.u.disk.off);
        lfs_off_t block_end = block_start + lfsr_data_size(&bptr.data);

        // do we have space for a block ecksum?
        lfsr_ecksum_t becksum = {.size=-1};
        if (bptr.cksize < lfs->cfg->block_size) {
            becksum.size = lfs->cfg->prog_size;
            becksum.cksum = 0;
            err = lfsr_bd_cksum(lfs,
                    bptr.data.u.disk.block, bptr.cksize, becksum.size,
                    becksum.size,
                    &becksum.cksum);
            if (err && err != LFS_ERR_CORRUPT) {
                return err;
            }
        }

        // and write it into our tree
        err = lfsr_ftree_carve(lfs, ftree,
                block_start, block_end - block_start, 0,
                LFSR_TAG_BLOCK, &bptr, &becksum);
        if (err) {
            return err;
        }

        // note compacting fragments -> blocks may not actually make any
        // progress on flushing the buffer on the first pass
        d = lfs_max32(pos, block_end) - pos;
        pos += d;
        buffer += lfs_min32(d, size);
        size -= lfs_min32(d, size);
        aligned = true;
    }

    // iteratively write fragments (inlined leaves)
    while (size > 0) {
        // truncate to our fragment size
        lfs_off_t fragment_start = pos;
        lfs_off_t fragment_end = fragment_start
                + lfs_min32(size, lfs->cfg->fragment_size);
        lfsr_data_t data = LFSR_DATA_BUF(
                buffer,
                fragment_end - fragment_start);

        lfsr_data_t datas[3];
        lfs_size_t data_count = 0;
        datas[data_count++] = data;

        // do we have a left sibling?
        if (fragment_start > 0
                && lfsr_ftree_size(ftree) >= fragment_start
                // don't bother to lookup left after first fragment
                && !aligned) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_ftree_lookupnext(lfs, ftree,
                    fragment_start-1,
                    &bid, &tag, &weight, &bptr, NULL);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // can we coalesce?
            if (bid-(weight-1) + lfsr_data_size(&bptr.data)
                        >= fragment_start
                    && lfsr_data_size(&bptr.data)
                        < lfs->cfg->fragment_size) {
                // coalesce, but truncate to our fragment size
                // TODO this is a bit of a hacky way to prepend data...
                LFS_ASSERT(data_count == 1);
                datas[0] = lfsr_data_truncate(bptr.data,
                        fragment_start - (bid-(weight-1)));
                datas[1] = lfsr_data_truncate(data,
                        lfs->cfg->fragment_size
                            - (fragment_start - (bid-(weight-1))));
                data_count = 2;
                data = lfsr_data_fromcat(datas, data_count);

                fragment_start = bid-(weight-1);
                fragment_end = fragment_start + lfsr_data_size(&data);
            }
        }

        // do we have a right sibling?
        //
        // note this may the same as our left sibling 
        if (fragment_end < lfsr_ftree_size(ftree)
                // don't bother to lookup right if fragment is already full
                && fragment_end - fragment_start
                    < lfs->cfg->fragment_size) {
            lfsr_bid_t bid;
            lfsr_tag_t tag;
            lfsr_bid_t weight;
            lfsr_bptr_t bptr;
            int err = lfsr_ftree_lookupnext(lfs, ftree,
                    fragment_end,
                    &bid, &tag, &weight, &bptr, NULL);
            if (err) {
                LFS_ASSERT(err != LFS_ERR_NOENT);
                return err;
            }

            // can we coalesce?
            if (fragment_end < bid-(weight-1)
                        + lfsr_data_size(&bptr.data)
                    && bid-(weight-1) + lfsr_data_size(&bptr.data)
                            - fragment_start
                        <= lfs->cfg->fragment_size) {
                datas[data_count++] = lfsr_data_fruncate(bptr.data,
                        bid-(weight-1) + lfsr_data_size(&bptr.data)
                            - fragment_end);
                data = lfsr_data_fromcat(datas, data_count);

                fragment_end = fragment_start + lfsr_data_size(&data);
            }
        }

        // make sure we didn't overflow our data buffer
        LFS_ASSERT(data_count <= 3);

        // once we've figured out what fragment to write, carve it into
        // our tree
        int err = lfsr_ftree_carve(lfs, ftree,
                fragment_start, fragment_end - fragment_start, 0,
                LFSR_TAG_DATA, &(const lfsr_bptr_t){.data=data}, NULL);
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

// needed in lfsr_file_read
static int lfsr_file_flush(lfs_t *lfs, lfsr_file_t *file);

lfs_ssize_t lfsr_file_read(lfs_t *lfs, lfsr_file_t *file,
        void *buffer, lfs_size_t size) {
    LFS_ASSERT(lfsr_o_isreadable(file->flags));
    LFS_ASSERT(file->pos + size <= 0x7fffffff);

    lfs_off_t pos_ = file->pos;
    uint8_t *buffer_ = buffer;
    while (size > 0 && pos_ < file->size) {
        // keep track of the next highest priority data offset
        lfs_ssize_t d = lfs_min32(size, file->size - pos_);

        // any data in our buffer?
        if (pos_ < file->buffer_pos + file->buffer_size
                && file->buffer_size != 0) {
            if (pos_ >= file->buffer_pos) {
                lfs_ssize_t d_ = lfs_min32(
                        d,
                        file->buffer_size - (pos_ - file->buffer_pos));
                memcpy(buffer_,
                        &file->buffer[pos_ - file->buffer_pos],
                        d_);

                pos_ += d_;
                buffer_ += d_;
                size -= d_;
                d -= d_;
                continue;
            }

            // buffered data takes priority
            d = lfs_min32(d, file->buffer_pos - pos_);
        }

        // any data in our ftree?
        if (pos_ < lfsr_ftree_size(&file->ftree)) {
            // bypass buffer?
            if ((lfs_size_t)d >= lfs->cfg->cache_size) {
                lfs_ssize_t d_ = lfsr_ftree_readnext(lfs, &file->ftree,
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
            if (lfsr_f_isunflushed(file->flags)) {
                int err = lfsr_file_flush(lfs, file);
                if (err) {
                    return err;
                }
                file->buffer_pos = 0;
                file->buffer_size = 0;
            }

            // try to fill our buffer with some data
            lfs_ssize_t d_ = lfsr_ftree_readnext(lfs, &file->ftree,
                    pos_, file->buffer, d);
            if (d_ < 0) {
                LFS_ASSERT(d != LFS_ERR_NOENT);
                return d_;
            }
            file->buffer_pos = pos_;
            file->buffer_size = d_;
            continue;
        }

        // found a hole? fill with zeros
        memset(buffer_, 0, d);
        
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
    LFS_ASSERT(lfsr_o_iswriteable(file->flags));

    // would this write make our file larger than our size limit?
    if (size > lfs->size_limit - file->pos) {
        return LFS_ERR_FBIG;
    }

    // size=0 is a bit special and is guaranteed to have no effects on the
    // underlying file, this means no updating file pos or file size
    //
    // since we need to test for this, just return early
    if (size == 0) {
        return 0;
    }

    // copy state so we can recover from errors
    lfs_off_t pos_ = file->pos;
    bool unflushed_ = lfsr_f_isunflushed(file->flags);
    lfs_off_t buffer_pos_ = file->buffer_pos;
    lfs_size_t buffer_size_ = file->buffer_size;
    lfsr_ftree_t ftree_ = file->ftree;
    // add to tracked mdirs
    lfsr_mdir_addopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    int err;

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);

    // update pos if we are appending
    if (lfsr_o_isappend(file->flags)) {
        pos_ = file->size;
    }

    // if we're a small file, we may need to append zeros
    if (pos_ > file->size
            && pos_ <= lfs->cfg->cache_size
            && pos_ <= lfs->cfg->inline_size
            && pos_ <= lfs->cfg->fragment_size) {
        LFS_ASSERT(unflushed_);
        LFS_ASSERT(file->size == buffer_size_);
        memset(&file->buffer[buffer_size_],
                0,
                pos_ - buffer_size_);
        buffer_size_ = pos_;
    }

    const uint8_t *buffer_ = buffer;
    while (size > 0) {
        // bypass buffer?
        //
        // note we flush our buffer before bypassing writes, this isn't
        // strictly necessary, but enforces a more intuitive write order
        // and avoids weird cases with low-level write heuristics
        //
        if (!unflushed_ && size >= lfs->cfg->cache_size) {
            err = lfsr_ftree_flush(lfs, &ftree_,
                    pos_, buffer_, size);
            if (err) {
                goto failed;
            }

            // update our buffer if we overlap
            //
            // but do this after writing so we can't fail
            if (pos_ < buffer_pos_ + buffer_size_
                    && pos_ + size > buffer_pos_) {
                memcpy(&file->buffer[pos_ - lfs_min32(buffer_pos_, pos_)],
                        &buffer_[buffer_pos_ - lfs_min32(pos_, buffer_pos_)],
                        lfs_min32(
                            buffer_size_ - (
                                pos_ - lfs_min32(buffer_pos_, pos_)),
                            size - (
                                buffer_pos_ - lfs_min32(pos_, buffer_pos_))));
            }

            pos_ += size;
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
        if (!unflushed_
                || (pos_ >= buffer_pos_
                    && pos_ <= buffer_pos_ + buffer_size_
                    && pos_ < buffer_pos_ + lfs->cfg->cache_size)) {
            // unused buffer? we can move it where we need it
            if (!unflushed_) {
                buffer_pos_ = pos_;
                buffer_size_ = 0;
            }

            lfs_size_t d = lfs_min32(
                    size,
                    lfs->cfg->cache_size - (pos_ - buffer_pos_));
            memcpy(&file->buffer[pos_ - buffer_pos_], buffer_, d);
            buffer_size_ = lfs_max32(
                    buffer_size_,
                    pos_+d - buffer_pos_);

            unflushed_ = true;
            pos_ += d;
            buffer_ += d;
            size -= d;
            continue;
        }

        // flush our buffer so the above can't fail
        err = lfsr_ftree_flush(lfs, &ftree_,
                buffer_pos_, file->buffer, buffer_size_);
        if (err) {
            goto failed;
        }
        unflushed_ = false;
    }

    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as unflushed and unsynced, update file, and return amount written
    lfs_size_t written;
    if (lfsr_o_isappend(file->flags)) {
        written = pos_ - file->size;
    } else {
        written = pos_ - file->pos;
    }
    file->flags |= LFS_F_UNSYNCED;
    if (unflushed_) {
        file->flags |= LFS_F_UNFLUSHED;
    }
    file->pos = pos_;
    file->size = lfs_max32(file->size, pos_);
    file->buffer_pos = buffer_pos_;
    file->buffer_size = buffer_size_;
    file->ftree.u = ftree_.u;
    return written;

failed:;
    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as errored so lfsr_file_close doesn't write to disk
    file->flags |= LFS_F_ERRORED;
    return err;
}

static int lfsr_file_flush(lfs_t *lfs, lfsr_file_t *file) {
    // do nothing if our file is readonly
    if (!lfsr_o_iswriteable(file->flags)) {
        LFS_ASSERT(!lfsr_f_isunflushed(file->flags)
                || (file->size <= lfs->cfg->cache_size
                    && file->size <= lfs->cfg->inline_size
                    && file->size <= lfs->cfg->fragment_size));
        return 0;
    }

    // do nothing if our file is already flushed
    if (!lfsr_f_isunflushed(file->flags)) {
        return 0;
    }

    // do nothing if our file is small
    //
    // note this means small files remain perpetually unflushed
    if (file->size <= lfs->cfg->cache_size
            && file->size <= lfs->cfg->inline_size
            && file->size <= lfs->cfg->fragment_size) {
        // our file must reside entirely in our buffer
        LFS_ASSERT(file->buffer_pos == 0);
        LFS_ASSERT(file->buffer_size == file->size);
        return 0;
    }

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);

    // copy state so we can recover from errors
    lfsr_ftree_t ftree_ = file->ftree;
    // add to tracked mdirs
    lfsr_mdir_addopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    int err;

    // flush our buffer if it contains any unwritten data
    if (lfsr_f_isunflushed(file->flags) && file->buffer_size != 0) {
        // flush
        err = lfsr_ftree_flush(lfs, &ftree_,
                file->buffer_pos, file->buffer, file->buffer_size);
        if (err) {
            goto failed;
        }
    }

    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as flushed and update our file
    file->ftree.u = ftree_.u;
    file->flags &= ~LFS_F_UNFLUSHED;
    return 0;

failed:;
    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as errored so lfsr_file_close doesn't write to disk
    file->flags |= LFS_F_ERRORED;
    return err;
}

int lfsr_file_sync(lfs_t *lfs, lfsr_file_t *file) {
    // it's not safe to do anything if our file errored
    if (lfsr_f_iserrored(file->flags)) {
        return 0;
    }

    // do nothing if our file has been removed
    if (file->ftree.mdir.mid == -1) {
        return 0;
    }

    // do nothing if our file is readonly
    if (!lfsr_o_iswriteable(file->flags)) {
        LFS_ASSERT(!lfsr_f_isunsynced(file->flags));
        return 0;
    }

    // do nothing if we're already in sync
    if (!lfsr_f_isunsynced(file->flags)) {
        return 0;
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

    // checkpoint the allocator again
    lfs_alloc_ckpoint(lfs);

    // note because of small-file caching and our current write
    // strategy, we never actually end up with only a direct data
    // or bptr
    //
    // this is convenient because bpts are a bit annoying to commit
    LFS_ASSERT(!lfsr_ftree_isbsprout(&file->ftree));
    LFS_ASSERT(!lfsr_ftree_isbleaf(&file->ftree));
    // if this invariant breaks something has gone horribly wrong
    LFS_ASSERT(file->size == lfs_max32(
            file->buffer_pos + file->buffer_size,
            lfsr_ftree_size(&file->ftree)));
    // small files must be inlined entirely in our buffer
    LFS_ASSERT(!(file->size <= lfs->cfg->cache_size
                && file->size <= lfs->cfg->inline_size
                && file->size <= lfs->cfg->fragment_size)
            || file->size == file->buffer_size);

    // commit our file's metadata
    uint8_t buf[LFSR_BTREE_DSIZE];
    err = lfsr_mdir_commit(lfs, &file->ftree.mdir, LFSR_ATTRS(
            (file->size == 0)
                ? LFSR_ATTR(file->ftree.mdir.mid,
                    WIDE(RM(STRUCT)), 0,
                    NULL())
            : (file->size <= lfs->cfg->cache_size
                    && file->size <= lfs->cfg->inline_size
                    && file->size <= lfs->cfg->fragment_size)
                ? LFSR_ATTR(file->ftree.mdir.mid,
                    WIDE(DATA), 0,
                    BUF(file->buffer, file->size))
            : (lfsr_ftree_isbshrub(&file->ftree))
                ? LFSR_ATTR(file->ftree.mdir.mid,
                    WIDE(SHRUBTRUNK), 0,
                    SHRUBTRUNK(&file->ftree.u.bshrub))
                : LFSR_ATTR(file->ftree.mdir.mid,
                    WIDE(BTREE), 0,
                    FROMBTREE(&file->ftree.u.btree, buf))));
    if (err) {
        goto failed;
    }

    // mark as synced
    file->flags &= ~LFS_F_UNSYNCED;
    // update other file handles
    for (lfsr_openedmdir_t *opened = lfs->opened[
                LFS_TYPE_REG-LFS_TYPE_REG];
            opened;
            opened = opened->next) {
        lfsr_file_t *file_ = (lfsr_file_t*)opened;
        if (file_->ftree.mdir.mid == file->ftree.mdir.mid) {
            file_->size = file->size;
            file_->ftree.u = file->ftree.u;
            file_->buffer_pos = file->buffer_pos;
            memcpy(file_->buffer, file->buffer, file->buffer_size);
            file_->buffer_size = file->buffer_size;
        }
    }

    return 0;

failed:;
    file->flags |= LFS_F_ERRORED;
    return err;
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
        pos_ = file->size + off;
    } else {
        LFS_UNREACHABLE();
    }

    // out of range?
    if (pos_ > lfs->size_limit) {
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
    return file->size;
}

int lfsr_file_truncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size) {
    // exceeds our size limit?
    if (size > lfs->size_limit) {
        return LFS_ERR_FBIG;
    }

    // do nothing if our size does not change
    if (file->size == size) {
        return 0;
    }

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);

    // copy state so we can recover from errors
    lfsr_ftree_t ftree_ = file->ftree;
    // add to tracked mdirs
    lfsr_mdir_addopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    int err;

    // does our file become small?
    if (size <= lfs->cfg->cache_size
            && size <= lfs->cfg->inline_size
            && size <= lfs->cfg->fragment_size) {
        // if our data is not already in our buffer we unfortunately
        // need to flush so our buffer is available to hold everything
        if (file->buffer_pos > 0
                || file->buffer_size < lfs_min32(size, file->size)) {
            // note that flush does not change the actual file data, so if
            // a read fails it's ok to fall back to our flushed state
            err = lfsr_file_flush(lfs, file);
            if (err) {
                goto failed;
            }
            file->buffer_pos = 0;
            file->buffer_size = 0;

            lfs_ssize_t d = lfsr_ftree_read(lfs, &file->ftree,
                    0, file->buffer, size);
            if (d < 0) {
                err = d;
                goto failed;
            }
            file->buffer_pos = 0;
            file->buffer_size = size;
        }

        // we may need to zero some of our buffer
        if (size > file->buffer_size) {
            memset(&file->buffer[file->buffer_size],
                    0,
                    size - file->buffer_size);
        }

        // small files remain perpetually unflushed
        file->flags |= LFS_F_UNFLUSHED;
        file->buffer_pos = 0;
        file->buffer_size = size;
        ftree_.u.size = LFSR_FTREE_NULL;

    // truncate our file normally
    } else {
        // truncate our ftree
        err = lfsr_ftree_carve(lfs, &ftree_,
                lfs_min32(file->size, size),
                file->size - lfs_min32(file->size, size),
                +size - file->size,
                LFSR_TAG_DATA, NULL, NULL);
        if (err) {
            goto failed;
        }

        // truncate our buffer
        file->buffer_pos = lfs_min32(file->buffer_pos, size);
        file->buffer_size = lfs_min32(
                file->buffer_size,
                size - lfs_min32(file->buffer_pos, size));
    }

    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as unsynced and update our size
    file->flags |= LFS_F_UNSYNCED;
    file->ftree.u = ftree_.u;
    file->size = size;
    LFS_ASSERT(file->size == lfs_max32(
            file->buffer_pos + file->buffer_size,
            lfsr_ftree_size(&file->ftree)));
    return 0;

failed:;
    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as errored so lfsr_file_close doesn't write to disk
    file->flags |= LFS_F_ERRORED;
    return err;
}

int lfsr_file_fruncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size) {
    // exceeds our size limit?
    if (size > lfs->size_limit) {
        return LFS_ERR_FBIG;
    }

    // do nothing if our size does not change
    if (file->size == size) {
        return 0;
    }

    // checkpoint the allocator
    lfs_alloc_ckpoint(lfs);

    // copy state so we can recover from errors
    lfsr_ftree_t ftree_ = file->ftree;
    // add to tracked mdirs
    lfsr_mdir_addopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    int err;

    // does our file become small?
    if (size <= lfs->cfg->cache_size
            && size <= lfs->cfg->inline_size
            && size <= lfs->cfg->fragment_size) {
        // if our data is not already in our buffer we unfortunately
        // need to flush so our buffer is available to hold everything
        if (file->buffer_pos + file->buffer_size < file->size
                || file->buffer_size < lfs_min32(size, file->size)) {
            // note that flush does not change the actual file data, so if
            // a read fails it's ok to fall back to our flushed state
            err = lfsr_file_flush(lfs, file);
            if (err) {
                goto failed;
            }
            file->buffer_pos = 0;
            file->buffer_size = 0;

            lfs_ssize_t d = lfsr_ftree_read(lfs, &file->ftree,
                    file->size - lfs_min32(size, file->size),
                    file->buffer, size);
            if (d < 0) {
                err = d;
                goto failed;
            }
            file->buffer_pos = 0;
            file->buffer_size = size;
        }

        // we may need to move the data in our buffer
        if (file->buffer_size > size) {
            memmove(file->buffer,
                    &file->buffer[file->buffer_size - size],
                    file->buffer_size);
        }
        // we may need to zero some of our buffer
        if (size > file->buffer_size) {
            memmove(&file->buffer[size - file->buffer_size],
                    file->buffer,
                    file->buffer_size);
            memset(file->buffer,
                    0,
                    size - file->buffer_size);
        }

        // small files remain perpetually unflushed
        file->flags |= LFS_F_UNFLUSHED;
        file->buffer_pos = 0;
        file->buffer_size = size;
        ftree_.u.size = LFSR_FTREE_NULL;

    // fruncate our file normally
    } else {
        // fruncate our ftree
        err = lfsr_ftree_carve(lfs, &ftree_,
                0,
                lfs_smax32(file->size - size, 0),
                +size - file->size,
                LFSR_TAG_DATA, NULL, NULL);
        if (err) {
            goto failed;
        }

        // fruncate our buffer
        memmove(file->buffer,
                &file->buffer[lfs_min32(
                    lfs_smax32(file->size - size - file->buffer_pos, 0),
                    file->buffer_size)],
                file->buffer_size - lfs_min32(
                    lfs_smax32(file->size - size - file->buffer_pos, 0),
                    file->buffer_size));
        file->buffer_size -= lfs_min32(
                lfs_smax32(file->size - size - file->buffer_pos, 0),
                file->buffer_size);
        file->buffer_pos -= lfs_smin32(file->size - size, file->buffer_pos);
    }

    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as unsynced and update our size
    file->flags |= LFS_F_UNSYNCED;
    file->ftree.u = ftree_.u;
    file->size = size;
    LFS_ASSERT(file->size == lfs_max32(
            file->buffer_pos + file->buffer_size,
            lfsr_ftree_size(&file->ftree)));
    return 0;

failed:;
    // remove from tracked mdirs
    lfsr_mdir_removeopened(lfs, LFS_TYPE_REG, (lfsr_openedmdir_t*)&ftree_);
    // mark as errored so lfsr_file_close doesn't write to disk
    file->flags |= LFS_F_ERRORED;
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
    lfs->lookahead.ckpoint = 0;

    // check that the size limits are sane
    LFS_ASSERT(lfs->cfg->name_limit <= LFS_NAME_MAX);
    lfs->name_limit = lfs->cfg->name_limit;
    if (!lfs->name_limit) {
        lfs->name_limit = LFS_NAME_MAX;
    }

    LFS_ASSERT(lfs->cfg->size_limit <= LFS_FILE_MAX);
    lfs->size_limit = lfs->cfg->size_limit;
    if (!lfs->size_limit) {
        lfs->size_limit = LFS_FILE_MAX;
    }

    LFS_ASSERT(lfs->cfg->uattr_limit <= LFS_UATTR_MAX);
    lfs->uattr_limit = lfs->cfg->uattr_limit;
    if (!lfs->uattr_limit) {
        lfs->uattr_limit = LFS_UATTR_MAX;
    }

    LFS_ASSERT(lfs->cfg->sattr_limit <= LFS_SATTR_MAX);
    lfs->sattr_limit = lfs->cfg->sattr_limit;
    if (!lfs->sattr_limit) {
        lfs->sattr_limit = LFS_SATTR_MAX;
    }

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

    // TODO maybe reorganize this function?

    lfs->hasorphans = false;

    // compute the number of bits we need to reserve for metadata rids
    //
    // This is equivalent to the nlog2 of the maximum number of rids we can
    // ever have in a single mdir. With some knowledge of our system we can
    // find a conservative, but useful, limit to this upper bound:
    //
    // - Each tag needs <=2 alts+null with our current compaction strategy
    // - Each tag/alt encodes to a minimum of 4 bytes
    //
    // This gives us ~4*4 or ~16 bytes per mid at minimum. If we cram an mdir
    // with the smallest possible mids, this gives us at most ~block_size/16
    // mids in a single mdir before the mdir runs out of space.
    //
    // Note we can't assume ~1/2 block utilization here, as an mdir may
    // temporarily fill with more mids before compaction occurs.
    //
    lfs->mbits = lfs_nlog2(lfs->cfg->block_size/16);

    // zero linked-lists of opened mdirs
    lfs->opened[LFS_TYPE_REG      - LFS_TYPE_REG] = NULL;
    lfs->opened[LFS_TYPE_DIR      - LFS_TYPE_REG] = NULL;
    lfs->opened[LFS_TYPE_INTERNAL - LFS_TYPE_REG] = NULL;

    // zero gstate
    memset(lfs->grm_g, 0, LFSR_GRM_DSIZE);
    memset(lfs->grm_d, 0, LFSR_GRM_DSIZE);

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

