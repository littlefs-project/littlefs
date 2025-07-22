/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS3_H
#define LFS3_H

#include "lfs3_util.h"


/// Version info ///

// Software library version
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS3_VERSION 0x00000000
#define LFS3_VERSION_MAJOR (0xffff & (LFS3_VERSION >> 16))
#define LFS3_VERSION_MINOR (0xffff & (LFS3_VERSION >>  0))

// Version of On-disk data structures
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS3_DISK_VERSION 0x00000000
#define LFS3_DISK_VERSION_MAJOR (0xffff & (LFS3_DISK_VERSION >> 16))
#define LFS3_DISK_VERSION_MINOR (0xffff & (LFS3_DISK_VERSION >>  0))


/// Definitions ///

// Type definitions
typedef uint32_t lfs3_size_t;
typedef int32_t  lfs3_ssize_t;

typedef uint32_t lfs3_off_t;
typedef int32_t  lfs3_soff_t;

typedef uint32_t lfs3_block_t;
typedef int32_t lfs3_sblock_t;

typedef uint32_t lfs3_rid_t;
typedef int32_t  lfs3_srid_t;

typedef uint16_t lfs3_tag_t;
typedef int16_t  lfs3_stag_t;

typedef uint32_t lfs3_bid_t;
typedef int32_t  lfs3_sbid_t;

typedef uint32_t lfs3_mid_t;
typedef int32_t  lfs3_smid_t;

typedef uint32_t lfs3_did_t;
typedef int32_t  lfs3_sdid_t;

// Maximum name size in bytes, may be redefined to reduce the size of the
// info struct. Limited to <= 1022. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef LFS3_NAME_MAX
#define LFS3_NAME_MAX 255
#endif

// Maximum size of a file in bytes, may be redefined to limit to support other
// drivers. Limited on disk to <= 2147483647. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef LFS3_FILE_MAX
#define LFS3_FILE_MAX 2147483647
#endif


// Possible error codes, these are negative to allow
// valid positive return values
enum lfs3_err {
    LFS3_ERR_OK          = 0,    // No error
    LFS3_ERR_UNKNOWN     = -1,   // Unknown error
    LFS3_ERR_INVAL       = -22,  // Invalid parameter
    LFS3_ERR_NOTSUP      = -95,  // Operation not supported
    LFS3_ERR_IO          = -5,   // Error during device operation
    LFS3_ERR_CORRUPT     = -84,  // Corrupted
    LFS3_ERR_NOENT       = -2,   // No directory entry
    LFS3_ERR_EXIST       = -17,  // Entry already exists
    LFS3_ERR_NOTDIR      = -20,  // Entry is not a dir
    LFS3_ERR_ISDIR       = -21,  // Entry is a dir
    LFS3_ERR_NOTEMPTY    = -39,  // Dir is not empty
    LFS3_ERR_FBIG        = -27,  // File too large
    LFS3_ERR_NOSPC       = -28,  // No space left on device
    LFS3_ERR_NOMEM       = -12,  // No more memory available
    LFS3_ERR_NOATTR      = -61,  // No data/attr available
    LFS3_ERR_NAMETOOLONG = -36,  // File name too long
    LFS3_ERR_RANGE       = -34,  // Result out of range
};

// File types
//
// LFS3_TYPE_UNKNOWN will always be the largest, including internal
// types, and can be used to deliminate user defined types at higher
// levels
//
enum lfs3_type {
    // file types
    LFS3_TYPE_REG        = 1,  // A regular file
    LFS3_TYPE_DIR        = 2,  // A directory file
    LFS3_TYPE_STICKYNOTE = 3,  // An uncommitted file
    LFS3_TYPE_UNKNOWN    = 7,  // Unknown file type

    // internally used types, don't use these
    LFS3_type_BOOKMARK   = 4,  // Directory bookmark
    LFS3_type_ORPHAN     = 5,  // An orphaned stickynote
    LFS3_type_TRV        = 6,  // An open traversal object
};

// File open flags
#define LFS3_O_MODE              3  // The file's access mode
#define LFS3_O_RDONLY            0  // Open a file as read only
#ifndef LFS3_RDONLY
#define LFS3_O_WRONLY            1  // Open a file as write only
#endif
#ifndef LFS3_RDONLY
#define LFS3_O_RDWR              2  // Open a file as read and write
#endif
#ifndef LFS3_RDONLY
#define LFS3_O_CREAT    0x00000004  // Create a file if it does not exist
#endif
#ifndef LFS3_RDONLY
#define LFS3_O_EXCL     0x00000008  // Fail if a file already exists
#endif
#ifndef LFS3_RDONLY
#define LFS3_O_TRUNC    0x00000010  // Truncate the existing file to zero size
#endif
#ifndef LFS3_RDONLY
#define LFS3_O_APPEND   0x00000020  // Move to end of file on every write
#endif
#define LFS3_O_FLUSH    0x00000040  // Flush data on every write
#define LFS3_O_SYNC     0x00000080  // Sync metadata on every write
#define LFS3_O_DESYNC   0x04000000  // Do not sync or recieve file updates
#define LFS3_O_CKMETA   0x00001000  // Check metadata checksums
#define LFS3_O_CKDATA   0x00002000  // Check metadata + data checksums

// internally used flags, don't use these
#define LFS3_o_WRSET             3  // Open a file as an atomic write
#define LFS3_o_TYPE     0xf0000000  // The file's type
#define LFS3_o_ZOMBIE   0x08000000  // File has been removed
#define LFS3_o_UNCREAT  0x02000000  // File does not exist yet
#define LFS3_o_UNSYNC   0x01000000  // File's metadata does not match disk
#define LFS3_o_UNCRYST  0x00800000  // File's leaf not fully crystallized
#define LFS3_o_UNFLUSH  0x00400000  // File's cache does not match disk

// File seek flags
#define LFS3_SEEK_SET 0  // Seek relative to an absolute position
#define LFS3_SEEK_CUR 1  // Seek relative to the current file position
#define LFS3_SEEK_END 2  // Seek relative to the end of the file

// Custom attribute flags
#define LFS3_A_MODE              3  // The attr's access mode
#define LFS3_A_RDONLY            0  // Open an attr as read only
#ifndef LFS3_RDONLY
#define LFS3_A_WRONLY            1  // Open an attr as write only
#endif
#ifndef LFS3_RDONLY
#define LFS3_A_RDWR              2  // Open an attr as read and write
#endif
#define LFS3_A_LAZY           0x04  // Only write attr if file changed

// Filesystem format flags
#ifndef LFS3_RDONLY
#define LFS3_F_MODE              1  // Format's access mode
#define LFS3_F_RDWR              0  // Format the filesystem as read and write
#ifdef LFS3_REVDBG
#define LFS3_F_REVDBG   0x00000010  // Add debug info to revision counts
#endif
#ifdef LFS3_REVNOISE
#define LFS3_F_REVNOISE 0x00000020  // Add noise to revision counts
#endif
#ifdef LFS3_CKPROGS
#define LFS3_F_CKPROGS  0x00080000  // Check progs by reading back progged data
#endif
#ifdef LFS3_CKFETCHES
#define LFS3_F_CKFETCHES \
                        0x00100000  // Check block checksums before first use
#endif
#ifdef LFS3_CKMETAPARITY
#define LFS3_F_CKMETAPARITY \
                        0x00200000  // Check metadata tag parity bits
#endif
#ifdef LFS3_CKDATACKSUMS
#define LFS3_F_CKDATACKSUMS \
                        0x00800000  // Check data checksums on reads
#endif

#define LFS3_F_CKMETA   0x00001000  // Check metadata checksums
#define LFS3_F_CKDATA   0x00002000  // Check metadata + data checksums
#endif

// Filesystem mount flags
#define LFS3_M_MODE              1  // Mount's access mode
#ifndef LFS3_RDONLY
#define LFS3_M_RDWR              0  // Mount the filesystem as read and write
#endif
#define LFS3_M_RDONLY            1  // Mount the filesystem as read only
#define LFS3_M_FLUSH    0x00000040  // Open all files with LFS3_O_FLUSH
#define LFS3_M_SYNC     0x00000080  // Open all files with LFS3_O_SYNC
#if !defined(LFS3_RDONLY) && defined(LFS3_REVDBG)
#define LFS3_M_REVDBG   0x00000010  // Add debug info to revision counts
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REVNOISE)
#define LFS3_M_REVNOISE 0x00000020  // Add noise to revision counts
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_CKPROGS)
#define LFS3_M_CKPROGS  0x00080000  // Check progs by reading back progged data
#endif
#ifdef LFS3_CKFETCHES
#define LFS3_M_CKFETCHES \
                        0x00100000  // Check block checksums before first use
#endif
#ifdef LFS3_CKMETAPARITY
#define LFS3_M_CKMETAPARITY \
                        0x00200000  // Check metadata tag parity bits
#endif
#ifdef LFS3_CKDATACKSUMS
#define LFS3_M_CKDATACKSUMS \
                        0x00800000  // Check data checksums on reads
#endif

#ifndef LFS3_RDONLY
#define LFS3_M_MKCONSISTENT \
                        0x00000100  // Make the filesystem consistent
#endif
#ifndef LFS3_RDONLY
#define LFS3_M_LOOKAHEAD \
                        0x00000200  // Populate lookahead buffer
#endif
#ifndef LFS3_RDONLY
#define LFS3_M_COMPACT  0x00000800  // Compact metadata logs
#endif
#define LFS3_M_CKMETA   0x00001000  // Check metadata checksums
#define LFS3_M_CKDATA   0x00002000  // Check metadata + data checksums

// Filesystem info flags
#define LFS3_I_RDONLY   0x00000001  // Mounted read only
#define LFS3_I_FLUSH    0x00000040  // Mounted with LFS3_M_FLUSH
#define LFS3_I_SYNC     0x00000080  // Mounted with LFS3_M_SYNC
#if !defined(LFS3_RDONLY) && defined(LFS3_REVDBG)
#define LFS3_I_REVDBG   0x00000010  // Mounted with LFS3_M_REVDBG
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REVNOISE)
#define LFS3_I_REVNOISE 0x00000020  // Mounted with LFS3_M_REVNOISE
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_CKPROGS)
#define LFS3_I_CKPROGS  0x00080000  // Mounted with LFS3_M_CKPROGS
#endif
#ifdef LFS3_CKFETCHES
#define LFS3_I_CKFETCHES \
                        0x00100000  // Mounted with LFS3_M_CKFETCHES
#endif
#ifdef LFS3_CKMETAPARITY
#define LFS3_I_CKMETAPARITY \
                        0x00200000  // Mounted with LFS3_M_CKMETAPARITY
#endif
#ifdef LFS3_CKDATACKSUMS
#define LFS3_I_CKDATACKSUMS \
                        0x00800000  // Mounted with LFS3_M_CKDATACKSUMS
#endif

#ifndef LFS3_RDONLY
#define LFS3_I_MKCONSISTENT \
                        0x00000100  // Filesystem needs mkconsistent to write
#endif
#ifndef LFS3_RDONLY
#define LFS3_I_LOOKAHEAD \
                        0x00000200  // Lookahead buffer is not full
#endif
#ifndef LFS3_RDONLY
#define LFS3_I_COMPACT  0x00000800  // Filesystem may have uncompacted metadata
#endif
#define LFS3_I_CKMETA   0x00001000  // Metadata checksums not checked recently
#define LFS3_I_CKDATA   0x00002000  // Data checksums not checked recently

// internally used flags, don't use these
#ifdef LFS3_REVDBG
#define LFS3_i_INMTREE  0x08000000  // Committing to mtree
#endif


// Block types
enum lfs3_btype {
    LFS3_BTYPE_MDIR  = 1,
    LFS3_BTYPE_BTREE = 2,
    LFS3_BTYPE_DATA  = 3,
};

// Traversal flags
#define LFS3_T_MODE              1  // The traversal's access mode
#ifndef LFS3_RDONLY
#define LFS3_T_RDWR              0  // Open traversal as read and write
#endif
#define LFS3_T_RDONLY            1  // Open traversal as read only
#define LFS3_T_MTREEONLY \
                        0x00000002  // Only traverse the mtree
#ifndef LFS3_RDONLY
#define LFS3_T_MKCONSISTENT \
                        0x00000100  // Make the filesystem consistent
#endif
#ifndef LFS3_RDONLY
#define LFS3_T_LOOKAHEAD \
                        0x00000200  // Populate lookahead buffer
#endif
#ifndef LFS3_RDONLY
#define LFS3_T_COMPACT  0x00000800  // Compact metadata logs
#endif
#define LFS3_T_CKMETA   0x00001000  // Check metadata checksums
#define LFS3_T_CKDATA   0x00002000  // Check metadata + data checksums

// internally used flags, don't use these
#define LFS3_t_TYPE     0xf0000000  // The traversal's type
#define LFS3_t_TSTATE   0x000f0000  // The current traversal state
#define LFS3_t_BTYPE    0x00f00000  // The current block type
#define LFS3_t_ZOMBIE   0x08000000  // File has been removed
#define LFS3_t_DIRTY    0x02000000  // Filesystem modified during traversal
#define LFS3_t_MUTATED  0x01000000  // Filesystem modified by traversal

// GC flags
#ifndef LFS3_RDONLY
#define LFS3_GC_MKCONSISTENT \
                        0x00000100  // Make the filesystem consistent
#endif
#ifndef LFS3_RDONLY
#define LFS3_GC_LOOKAHEAD \
                        0x00000200  // Populate lookahead buffer
#endif
#ifndef LFS3_RDONLY
#define LFS3_GC_COMPACT 0x00000800  // Compact metadata logs
#endif
#define LFS3_GC_CKMETA  0x00001000  // Check metadata checksums
#define LFS3_GC_CKDATA  0x00002000  // Check metadata + data checksums


// Configuration provided during initialization of the littlefs
struct lfs3_cfg {
    // Opaque user provided context that can be used to pass
    // information to the block device operations
    void *context;

    // Read a region in a block. Negative error codes are propagated
    // to the user.
    int (*read)(const struct lfs3_cfg *c, lfs3_block_t block,
            lfs3_off_t off, void *buffer, lfs3_size_t size);

    // Program a region in a block. The block must have previously
    // been erased. Negative error codes are propagated to the user.
    // May return LFS3_ERR_CORRUPT if the block should be considered bad.
    #ifndef LFS3_RDONLY
    int (*prog)(const struct lfs3_cfg *c, lfs3_block_t block,
            lfs3_off_t off, const void *buffer, lfs3_size_t size);
    #endif

    // Erase a block. A block must be erased before being programmed.
    // The state of an erased block is undefined. Negative error codes
    // are propagated to the user.
    // May return LFS3_ERR_CORRUPT if the block should be considered bad.
    #ifndef LFS3_RDONLY
    int (*erase)(const struct lfs3_cfg *c, lfs3_block_t block);
    #endif

    // Sync the state of the underlying block device. Negative error codes
    // are propagated to the user.
    #ifndef LFS3_RDONLY
    int (*sync)(const struct lfs3_cfg *c);
    #endif

#ifdef LFS3_THREADSAFE
    // Lock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*lock)(const struct lfs3_cfg *c);

    // Unlock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*unlock)(const struct lfs3_cfg *c);
#endif

    // Minimum size of a read in bytes. All read operations will be a
    // multiple of this value.
    lfs3_size_t read_size;

    // Minimum size of a program in bytes. All program operations will be a
    // multiple of this value.
    #ifndef LFS3_RDONLY
    lfs3_size_t prog_size;
    #endif

    // Size of an erasable block in bytes. This does not impact ram consumption
    // and may be larger than the physical erase size. Must be a multiple of
    // the read and program sizes.
    lfs3_size_t block_size;

    // Number of erasable blocks on the device.
    lfs3_size_t block_count;

    // Number of erase cycles before metadata blocks are relocated for
    // wear-leveling. Suggested values are in the range 16-1024. Larger values
    // relocate less frequently, improving average performance, at the cost
    // of worse wear distribution. Note this ends up rounded down to a
    // power-of-2.
    //
    // 0 results in pure copy-on-write, which may be counter-productive. Set
    // to -1 to disable block-level wear-leveling.
    #ifndef LFS3_RDONLY
    int32_t block_recycles;
    #endif

    // Size of the read cache in bytes. Larger caches can improve
    // performance by storing more data and reducing the number of disk
    // accesses. Must be a multiple of the read size.
    lfs3_size_t rcache_size;

    // Size of the program cache in bytes. Larger caches can improve
    // performance by storing more data and reducing the number of disk
    // accesses. Must be a multiple of the program size.
    #ifndef LFS3_RDONLY
    lfs3_size_t pcache_size;
    #endif

    // Size of file caches in bytes. In addition to filesystem-wide
    // read/prog caches, each file gets its own cache to reduce disk
    // accesses.
    lfs3_size_t file_cache_size;

    // Size of the lookahead buffer in bytes. A larger lookahead buffer
    // increases the number of blocks found during an allocation pass. The
    // lookahead buffer is stored as a compact bitmap, so each byte of RAM
    // can track 8 blocks.
    #ifndef LFS3_RDONLY
    lfs3_size_t lookahead_size;
    #endif

    // Flags indicating what gc work to do during lfs3_gc calls.
    #ifdef LFS3_GC
    uint32_t gc_flags;
    #endif

    // Number of gc steps to perform in each call to lfs3_gc, with each
    // step being ~1 block of work.
    //
    // More steps per call will make more progress if interleaved with
    // other filesystem operations, but may also introduce more latency.
    // steps=1 will do the minimum amount of work to make progress, and
    // steps=-1 will not return until all pending janitorial work has
    // been completed.
    //
    // Defaults to steps=1 when zero.
    #ifdef LFS3_GC
    lfs3_soff_t gc_steps;
    #endif

    // Threshold for metadata compaction during gc in bytes. Metadata logs
    // that exceed this threshold will be compacted during gc operations.
    // Defaults to ~88% block_size when zero, though this default may change
    // in the future.
    //
    // Note this only affects explicit gc operations. Otherwise metadata is
    // only compacted when full.
    //
    // Set to -1 to disable metadata compaction during gc.
    #ifndef LFS3_RDONLY
    lfs3_size_t gc_compact_thresh;
    #endif

    // Optional statically allocated rcache buffer. Must be rcache_size. By
    // default lfs3_malloc is used to allocate this buffer.
    void *rcache_buffer;

    // Optional statically allocated pcache buffer. Must be pcache_size. By
    // default lfs3_malloc is used to allocate this buffer.
    #ifndef LFS3_RDONLY
    void *pcache_buffer;
    #endif

    // Optional statically allocated lookahead buffer. Must be lookahead_size.
    // By default lfs3_malloc is used to allocate this buffer.
    #ifndef LFS3_RDONLY
    void *lookahead_buffer;
    #endif

    // Optional upper limit on length of file names in bytes. No downside for
    // larger names except the size of the info struct which is controlled by
    // the LFS3_NAME_MAX define. Defaults to LFS3_NAME_MAX when zero. Stored in
    // superblock and must be respected by other littlefs drivers.
    #ifndef LFS3_RDONLY
    lfs3_size_t name_limit;
    #endif

    // Optional upper limit on files in bytes. No downside for larger files
    // but must be <= LFS3_FILE_MAX. Defaults to LFS3_FILE_MAX when zero. Stored
    // in superblock and must be respected by other littlefs drivers.
    #ifndef LFS3_RDONLY
    lfs3_size_t file_limit;
    #endif

    // TODO these are pretty low-level details, should we have reasonable
    // defaults? need to benchmark.

    // Maximum size of inlined trees (shrubs) in bytes. Shrubs reduce B-tree
    // root overhead, but may impact metadata-related performance. Must be <=
    // blocksize/4.
    //
    // 0 disables shrubs.
    #ifndef LFS3_RDONLY
    lfs3_size_t inline_size;
    #endif

    // Maximum size of a non-block B-tree leaf in bytes. Smaller values may
    // make small random-writes cheaper, but increase metadata overhead. Must
    // be <= block_size/4.
    #ifndef LFS3_RDONLY
    lfs3_size_t fragment_size;
    #endif

    // Threshold for compacting multiple fragments into a block. Smaller
    // values will crystallize more eagerly, reducing disk usage, but
    // increasing the cost of random-writes.
    //
    // 0 only writes blocks, minimizing disk usage, while -1 or any value >
    // block_size only writes fragments, minimizing random-write cost.
    #ifndef LFS3_RDONLY
    lfs3_size_t crystal_thresh;
    #endif
};

// File info structure
struct lfs3_info {
    // Type of the file, either LFS3_TYPE_REG or LFS3_TYPE_DIR
    uint8_t type;

    // Size of the file, only valid for REG files. Limited to 32-bits.
    lfs3_size_t size;

    // Name of the file stored as a null-terminated string. Limited to
    // LFS3_NAME_MAX+1, which can be changed by redefining LFS3_NAME_MAX to
    // reduce RAM. LFS3_NAME_MAX is stored in superblock and must be
    // respected by other littlefs drivers.
    char name[LFS3_NAME_MAX+1];
};

// Filesystem info structure
struct lfs3_fsinfo {
    // Filesystem flags
    uint32_t flags;

    // Size of a logical block in bytes.
    lfs3_size_t block_size;

    // Number of logical blocks in the filesystem.
    lfs3_size_t block_count;

    // Upper limit on the length of file names in bytes.
    lfs3_size_t name_limit;

    // Upper limit on the size of files in bytes.
    lfs3_size_t file_limit;
};

// Traversal info structure
struct lfs3_tinfo {
    // Type of the block
    uint8_t btype;

    // Block address
    lfs3_block_t block;
};

// Custom attribute structure, used to describe custom attributes
// committed atomically during file writes.
struct lfs3_attr {
    // Type of attribute
    //
    // Note some of this range is reserved:
    // 0x00-0x7f - Free for custom attributes
    // 0x80-0xff - May be assigned a standard attribute
    uint8_t type;

    // Flags that control how attr is read/written/removed
    uint8_t flags;

    // Pointer the buffer where the attr will be read/written
    void *buffer;

    // Size of the attr buffer in bytes, this can be set to
    // LFS3_ERR_NOATTR to remove the attr
    lfs3_ssize_t buffer_size;

    // Optional pointer to a mutable attr size, updated on read/write,
    // set to LFS3_ERR_NOATTR if attr does not exist
    //
    // Defaults to buffer_size if NULL
    lfs3_ssize_t *size;
};

// Optional configuration provided during lfs3_file_opencfg
struct lfs3_file_cfg {
    // Optional statically allocated file cache buffer. Must be cache_size.
    // By default lfs3_malloc is used to allocate this buffer.
    void *cache_buffer;

    // Size of the file cache in bytes. In addition to filesystem-wide
    // read/prog caches, each file gets its own cache to reduce disk
    // accesses. Defaults to file_cache_size if cache_buffer is NULL.
    lfs3_size_t cache_size;

    // Optional list of custom attributes attached to the file. If readable,
    // these attributes will be kept up to date with the attributes on-disk.
    // If writeable, these attributes will be written to disk atomically on
    // every file sync or close.
    #ifndef LFS3_KVONLY
    struct lfs3_attr *attrs;
    #endif

    // Number of custom attributes in the list
    #ifndef LFS3_KVONLY
    lfs3_size_t attr_count;
    #endif
};


/// internal littlefs data structures ///

// either an on-disk or in-RAM data pointer
//
// note, it's tempting to make this fancier, but we benefit quite a lot
// from the compiler being able to aggresively optimize this struct
//
typedef struct lfs3_data {
    // sign2(size)=0b00 => in-RAM buffer
    // sign2(size)=0b10 => on-disk data
    // sign2(size)=0b11 => on-disk data + cksum
    lfs3_size_t size;
    union {
        const uint8_t *buffer;
        uint8_t *buffer_;
        struct {
            lfs3_block_t block;
            lfs3_size_t off;
            // optional context for validating data
            #ifdef LFS3_CKDATACKSUMS
            // sign(cksize)=0 => block not erased
            // sign(cksize)=1 => block erased
            lfs3_size_t cksize;
            uint32_t cksum;
            #endif
        } disk;
    } u;
} lfs3_data_t;

// a possible block pointer
typedef struct lfs3_bptr {
    // sign2(size)=0b00 => in-RAM buffer
    // sign2(size)=0b10 => on-disk data
    // sign2(size)=0b11 => block pointer
    lfs3_data_t d;
    #if !defined(LFS3_2BONLY) && !defined(LFS3_CKDATACKSUMS)
    // sign(cksize)=0 => block not erased
    // sign(cksize)=1 => block erased
    lfs3_size_t cksize;
    uint32_t cksum;
    #endif
} lfs3_bptr_t;

// littlefs's core metadata log type
typedef struct lfs3_rbyd {
    lfs3_rid_t weight;
    lfs3_block_t blocks[2];
    // sign(trunk)=0 => normal rbyd
    // sign(trunk)=1 => shrub rbyd
    lfs3_size_t trunk;
    #ifndef LFS3_RDONLY
    // sign(eoff)       => perturb bit
    // eoff=0, trunk=0  => not yet committed
    // eoff=0, trunk>0  => not yet fetched
    // eoff>=block_size => rbyd not erased/needs compaction
    lfs3_size_t eoff;
    #endif
    uint32_t cksum;
} lfs3_rbyd_t;

// littlefs's btree representation
//
// technically all we need for btrees is the root rbyd, but tracking the
// most recent leaf helps speed up iteration/subattrs/etc without
// local rbyd allocations -- less code and stack for the same
// performance
typedef struct lfs3_btree {
    lfs3_rbyd_t r;
    struct {
        lfs3_bid_t bid;
        lfs3_rbyd_t r;
    } leaf;
} lfs3_btree_t;

// littlefs's atomic metadata log type
typedef struct lfs3_mdir {
    lfs3_smid_t mid;
    lfs3_rbyd_t r;
    uint32_t gcksumdelta;
} lfs3_mdir_t;

// a handle to an opened mdir for tracking purposes
typedef struct lfs3_handle {
    // an invasive linked-list is used to keep things in-sync
    struct lfs3_handle *next;
    // flags includes the type and type-specific flags
    uint32_t flags;
    lfs3_mdir_t mdir;
} lfs3_handle_t;

// a shrub is a secondary trunk in an mdir
typedef lfs3_rbyd_t lfs3_shrub_t;

// a bshrub is like a btree but with a shrub as a root
typedef struct lfs3_bshrub {
    // bshrubs need to be tracked for commits to work
    lfs3_handle_t h;
    // files contain both an active bshrub and staging bshrub, to allow
    // staging during mdir compacts
    // trunk=0       => no bshrub/btree
    // sign(trunk)=1 => bshrub
    // sign(trunk)=0 => btree
    lfs3_btree_t shrub;
    #ifndef LFS3_RDONLY
    lfs3_shrub_t shrub_;
    #endif
} lfs3_bshrub_t;

// littlefs file type
typedef struct lfs3_file {
    // btree/bshrub stuff is in here
    lfs3_bshrub_t b;
    const struct lfs3_file_cfg *cfg;

    // current file position
    #ifndef LFS3_KVONLY
    lfs3_off_t pos;
    #endif

    // in-RAM cache
    struct {
        #ifndef LFS3_KVONLY
        lfs3_off_t pos;
        #endif
        lfs3_data_t d;
    } cache;

    // on-disk leaf bptr
    #ifndef LFS3_KVONLY
    struct {
        lfs3_off_t pos;
        lfs3_off_t weight;
        lfs3_bptr_t bptr;
    } leaf;
    #endif
} lfs3_file_t;

// littlefs directory type
typedef struct lfs3_dir {
    lfs3_handle_t h;
    lfs3_did_t did;
    lfs3_off_t pos;
} lfs3_dir_t;

// littlefs traversal type
typedef struct lfs3_trv {
    // mdir/bshrub/btree state, this also includes our traversal
    // state machine and cycle detection state
    lfs3_bshrub_t b;
    // opened file state
    lfs3_handle_t *h;
    // bshrub/btree traversal state
    lfs3_sbid_t bid;

    // recalculate gcksum when traversing with ckmeta
    uint32_t gcksum;
    // pending blocks, only used in lfs3_trv_read
    lfs3_sblock_t blocks[2];
} lfs3_trv_t;

// grm encoding:
// .- -+- -+- -+- -+- -.  mids:  2 leb128s  <=2x5 bytes
// ' mids              '  total:            <=10 bytes
// +                   +
// '                   '
// '- -+- -+- -+- -+- -'
//
#define LFS3_GRM_DSIZE (5+5)

typedef struct lfs3_grm {
    lfs3_smid_t queue[2];
} lfs3_grm_t;


// The littlefs filesystem type
typedef struct lfs3 {
    const struct lfs3_cfg *cfg;
    uint32_t flags;
    lfs3_size_t block_count;
    lfs3_size_t name_limit;
    lfs3_off_t file_limit;

    uint8_t mbits;
    #ifndef LFS3_RDONLY
    int8_t recycle_bits;
    uint8_t rattr_estimate;
    uint8_t mattr_estimate;
    #endif

    // linked-list of opened mdirs
    lfs3_handle_t *handles;

    lfs3_mdir_t mroot;
    #ifndef LFS3_2BONLY
    lfs3_btree_t mtree;
    #endif

    struct lfs3_rcache {
        lfs3_block_t block;
        lfs3_size_t off;
        lfs3_size_t size;
        uint8_t *buffer;
    } rcache;

    #ifndef LFS3_RDONLY
    struct lfs3_pcache {
        lfs3_block_t block;
        lfs3_size_t off;
        lfs3_size_t size;
        uint8_t *buffer;
    } pcache;
    // optional prog-aligned cksum
    uint32_t pcksum;
    #endif

    #if !defined(LFS3_RDONLY) && defined(LFS3_CKMETAPARITY)
    struct {
        lfs3_block_t block;
        // sign(off) => tail parity
        lfs3_size_t off;
    } ptail;
    #endif

    #if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
    struct lfs3_lookahead {
        lfs3_block_t window;
        lfs3_block_t off;
        lfs3_block_t size;
        lfs3_block_t ckpoint;
        uint8_t *buffer;
    } lookahead;
    #endif

    #if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
    const lfs3_data_t *graft;
    lfs3_ssize_t graft_count;
    #endif

    // global state
    uint32_t gcksum;
    #ifndef LFS3_RDONLY
    uint32_t gcksum_p;
    #endif
    // TODO can we actually get rid of grm_d when LFS3_RDONLY?
    uint32_t gcksum_d;

    lfs3_grm_t grm;
    #ifndef LFS3_RDONLY
    uint8_t grm_p[LFS3_GRM_DSIZE];
    #endif
    // TODO can we actually get rid of grm_d when LFS3_RDONLY?
    uint8_t grm_d[LFS3_GRM_DSIZE];

    // optional incremental gc state
    #ifdef LFS3_GC
    struct {
        lfs3_trv_t trv;
    } gc;
    #endif
} lfs3_t;


/// Filesystem functions ///

// Format a block device with the littlefs
//
// Requires a littlefs object and config struct. This clobbers the littlefs
// object, and does not leave the filesystem mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_format(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_cfg *cfg);
#endif

// Mounts a littlefs
//
// Requires a littlefs object and config struct. Multiple filesystems
// may be mounted simultaneously with multiple littlefs objects. Both
// lfs3 and config must be allocated while mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int lfs3_mount(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_cfg *cfg);

// Unmounts a littlefs
//
// Does nothing besides releasing any allocated resources.
// Returns a negative error code on failure.
int lfs3_unmount(lfs3_t *lfs3);

/// General operations ///

// Get the value of a file
//
// Returns the number of bytes read, or a negative error code on failure.
// Note this may be less than the on-disk file size if the buffer is not
// large enough.
lfs3_ssize_t lfs3_get(lfs3_t *lfs3, const char *path,
        void *buffer, lfs3_size_t size);

// Get a file's size
//
// Returns the size of the file, or a negative error code on failure.
lfs3_ssize_t lfs3_size(lfs3_t *lfs3, const char *path);

// Set the value of a file
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_set(lfs3_t *lfs3, const char *path,
        const void *buffer, lfs3_size_t size);
#endif

// Removes a file or directory
//
// If removing a directory, the directory must be empty.
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_remove(lfs3_t *lfs3, const char *path);
#endif

// Rename or move a file or directory
//
// If the destination exists, it must match the source in type.
// If the destination is a directory, the directory must be empty.
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_rename(lfs3_t *lfs3, const char *old_path, const char *new_path);
#endif

// Find info about a file or directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a negative error code on failure.
int lfs3_stat(lfs3_t *lfs3, const char *path, struct lfs3_info *info);

// Get a custom attribute
//
// Returns the number of bytes read, or a negative error code on failure.
// Note this may be less than the on-disk attr size if the buffer is not
// large enough.
lfs3_ssize_t lfs3_getattr(lfs3_t *lfs3, const char *path, uint8_t type,
        void *buffer, lfs3_size_t size);

// Get a custom attribute's size
//
// Returns the size of the attribute, or a negative error code on failure.
lfs3_ssize_t lfs3_sizeattr(lfs3_t *lfs3, const char *path, uint8_t type);

// Set a custom attributes
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_setattr(lfs3_t *lfs3, const char *path, uint8_t type,
        const void *buffer, lfs3_size_t size);
#endif

// Removes a custom attribute
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_removeattr(lfs3_t *lfs3, const char *path, uint8_t type);
#endif


/// File operations ///

// Open a file
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum lfs3_open_flags that are bitwise-ored together.
//
// Returns a negative error code on failure.
#if !defined(LFS3_KVONLY) && !defined(LFS3_NO_MALLOC)
int lfs3_file_open(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags);
#endif

// Open a file with extra configuration
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum lfs3_open_flags that are bitwise-ored together.
//
// The config struct provides additional config options per file as described
// above. The config struct must remain allocated while the file is open, and
// the config struct must be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_opencfg(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs3_file_cfg *cfg);
#endif

// Close a file
//
// If the file is not desynchronized, any pending writes are written out
// to storage as though sync had been called.
//
// Releases any allocated resources, even if there is an error.
//
// Readonly and desynchronized files do not touch disk and will always
// return 0.
//
// Returns a negative error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_close(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Synchronize a file on storage
//
// Any pending writes are written out to storage and other open files.
//
// If the file was desynchronized, it is now marked as synchronized. It will
// now recieve file updates and syncs on close.
//
// Returns a negative error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_sync(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Flush any buffered data
//
// This does not update metadata and is called implicitly by lfs3_file_sync.
// Calling this explicitly may be useful for preventing write errors in
// read operations.
//
// Returns a negative error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_flush(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Mark a file as desynchronized
//
// Desynchronized files do not recieve file updates and do not sync on close.
// They effectively act as snapshots of the underlying file at that point
// in time.
//
// If an error occurs during a write operation, the file is implicitly marked
// as desynchronized.
//
// An explicit and successful call to either lfs3_file_sync or
// lfs3_file_resync reverses this, marking the file as synchronized again.
//
// Returns a negative error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_desync(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Discard unsynchronized changes and mark a file as synchronized
//
// This is effectively the same as closing and reopening the file, and
// may read from disk to figure out file state.
//
// Returns a negative error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_resync(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Read data from file
//
// Takes a buffer and size indicating where to store the read data.
// Returns the number of bytes read, or a negative error code on failure.
#ifndef LFS3_KVONLY
lfs3_ssize_t lfs3_file_read(lfs3_t *lfs3, lfs3_file_t *file,
        void *buffer, lfs3_size_t size);
#endif

// Write data to file
//
// Takes a buffer and size indicating the data to write. The file will not
// actually be updated on the storage until either sync or close is called.
//
// Returns the number of bytes written, or a negative error code on failure.
#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
lfs3_ssize_t lfs3_file_write(lfs3_t *lfs3, lfs3_file_t *file,
        const void *buffer, lfs3_size_t size);
#endif

// Change the position of the file
//
// The change in position is determined by the offset and whence flag.
// Returns the new position of the file, or a negative error code on failure.
#ifndef LFS3_KVONLY
lfs3_soff_t lfs3_file_seek(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_soff_t off, uint8_t whence);
#endif

// Truncate/grow the size of the file to the specified size
//
// If size is larger than the current file size, a hole is created, appearing
// as if the file was filled with zeros.
//
// Returns a negative error code on failure.
#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
int lfs3_file_truncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size);
#endif

// Truncate/grow the file, but from the front
//
// If size is larger than the current file size, a hole is created, appearing
// as if the file was filled with zeros.
//
// Returns a negative error code on failure.
#if !defined(LFS3_RDONLY) && !defined(LFS3_KVONLY)
int lfs3_file_fruncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size);
#endif

// Return the position of the file
//
// Equivalent to lfs3_file_seek(lfs3, file, 0, LFS3_SEEK_CUR)
// Returns the position of the file, or a negative error code on failure.
#ifndef LFS3_KVONLY
lfs3_soff_t lfs3_file_tell(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Change the position of the file to the beginning of the file
//
// Equivalent to lfs3_file_seek(lfs3, file, 0, LFS3_SEEK_SET)
// Returns a negative error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_rewind(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Return the size of the file
//
// Similar to lfs3_file_seek(lfs3, file, 0, LFS3_SEEK_END)
// Returns the size of the file, or a negative error code on failure.
#ifndef LFS3_KVONLY
lfs3_soff_t lfs3_file_size(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Check a file for metadata errors
//
// Returns LFS3_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_ckmeta(lfs3_t *lfs3, lfs3_file_t *file);
#endif

// Check a file for metadata + data errors
//
// Returns LFS3_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
#ifndef LFS3_KVONLY
int lfs3_file_ckdata(lfs3_t *lfs3, lfs3_file_t *file);
#endif


/// Directory operations ///

// Create a directory
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_mkdir(lfs3_t *lfs3, const char *path);
#endif

// Open a directory
//
// Once open a directory can be used with read to iterate over files.
// Returns a negative error code on failure.
int lfs3_dir_open(lfs3_t *lfs3, lfs3_dir_t *dir, const char *path);

// Close a directory
//
// Releases any allocated resources.
// Returns a negative error code on failure.
int lfs3_dir_close(lfs3_t *lfs3, lfs3_dir_t *dir);

// Read an entry in the directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns 0 on success, LFS3_ERR_NOENT at the end of directory, or a
// negative error code on failure.
int lfs3_dir_read(lfs3_t *lfs3, lfs3_dir_t *dir, struct lfs3_info *info);

// Change the position of the directory
//
// The new off must be a value previous returned from tell and specifies
// an absolute offset in the directory seek.
//
// Returns a negative error code on failure.
int lfs3_dir_seek(lfs3_t *lfs3, lfs3_dir_t *dir, lfs3_soff_t off);

// Return the position of the directory
//
// The returned offset is only meant to be consumed by seek and may not make
// sense, but does indicate the current position in the directory iteration.
//
// Returns the position of the directory, or a negative error code on failure.
lfs3_soff_t lfs3_dir_tell(lfs3_t *lfs3, lfs3_dir_t *dir);

// Change the position of the directory to the beginning of the directory
//
// Returns a negative error code on failure.
int lfs3_dir_rewind(lfs3_t *lfs3, lfs3_dir_t *dir);


/// Traversal operations ///

// Open a traversal
//
// Once open, a traversal can be read from to iterate over all blocks in
// the filesystem.
//
// Returns a negative error code on failure.
int lfs3_trv_open(lfs3_t *lfs3, lfs3_trv_t *trv, uint32_t flags);

// Close a traversal
//
// Releases any allocated resources.
// Returns a negative error code on failure.
int lfs3_trv_close(lfs3_t *lfs3, lfs3_trv_t *trv);

// Progress the traversal and read an entry
//
// Fills out the tinfo structure.
//
// Returns 0 on success, LFS3_ERR_NOENT at the end of traversal, or a
// negative error code on failure.
int lfs3_trv_read(lfs3_t *lfs3, lfs3_trv_t *trv,
        struct lfs3_tinfo *tinfo);

// Reset the traversal
//
// Returns a negative error code on failure.
int lfs3_trv_rewind(lfs3_t *lfs3, lfs3_trv_t *trv);


/// Filesystem-level filesystem operations

// Find on-disk info about the filesystem
//
// Fills out the fsinfo structure based on the filesystem found on-disk.
// Returns a negative error code on failure.
int lfs3_fs_stat(lfs3_t *lfs3, struct lfs3_fsinfo *fsinfo);

// Finds the number of blocks in use by the filesystem
//
// Note: Result is best effort. If files share COW structures, the returned
// usage may be larger than the filesystem actually is.
//
// Returns the number of allocated blocks, or a negative error code on failure.
lfs3_ssize_t lfs3_fs_usage(lfs3_t *lfs3);

// Attempt to make the filesystem consistent and ready for writing
//
// Calling this function is not required, consistency will be implicitly
// enforced on the first operation that writes to the filesystem, but this
// function allows the work to be performed earlier and without other
// filesystem changes.
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_fs_mkconsistent(lfs3_t *lfs3);
#endif

// Check the filesystem for metadata errors
//
// Returns LFS3_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
int lfs3_fs_ckmeta(lfs3_t *lfs3);

// Check the filesystem for metadata + data errors
//
// Returns LFS3_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
int lfs3_fs_ckdata(lfs3_t *lfs3);

// Get the current filesystem checksum
//
// This is a checksum of all metadata + data in the filesystem, which
// can be stored externally to provide increased protection against
// filesystem corruption.
//
// Note this checksum is order-sensitive. So while it's unlikely two
// filesystems with different contents will have the same checksum, two
// filesystems with the same contents may not have the same checksum.
//
// Also note this is only a 32-bit checksum. Collisions should be
// expected.
//
// Returns a negative error code on failure.
int lfs3_fs_cksum(lfs3_t *lfs3, uint32_t *cksum);

// Perform any janitorial work that may be pending
//
// The exact janitorial work depends on the configured flags and steps.
//
// Calling this function is not required, but may allow the offloading of
// expensive janitorial work to a less time-critical code path.
//
// Returns a negative error code on failure.
#ifdef LFS3_GC
int lfs3_fs_gc(lfs3_t *lfs3);
#endif

// Mark janitorial work as incomplete
//
// Any info flags passed to lfs3_gc_unck will be reset internally,
// forcing the work to be redone.
//
// This is most useful for triggering new ckmeta/ckdata scans with
// LFS3_I_CANCKMETA and LFS3_I_CANCKDATA. Otherwise littlefs will perform
// only one scan after mount.
//
// Returns a negative error code on failure.
int lfs3_fs_unck(lfs3_t *lfs3, uint32_t flags);

// Change the number of blocks used by the filesystem
//
// This changes the number of blocks we are currently using and updates
// the superblock with the new block count.
//
// Note: This is irreversible.
//
// Returns a negative error code on failure.
#if !defined(LFS3_RDONLY) && !defined(LFS3_2BONLY)
int lfs3_fs_grow(lfs3_t *lfs3, lfs3_size_t block_count);
#endif


#endif
