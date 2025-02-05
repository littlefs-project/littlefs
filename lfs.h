/*
 * The little filesystem
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_H
#define LFS_H

#include "lfs_util.h"

#ifdef __cplusplus
extern "C"
{
#endif


/// Version info ///

// Software library version
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS_VERSION 0x00020005
#define LFS_VERSION_MAJOR (0xffff & (LFS_VERSION >> 16))
#define LFS_VERSION_MINOR (0xffff & (LFS_VERSION >>  0))

// Version of On-disk data structures
// Major (top-nibble), incremented on backwards incompatible changes
// Minor (bottom-nibble), incremented on feature additions
#define LFS_DISK_VERSION 0x00000000
#define LFS_DISK_VERSION_MAJOR (0xffff & (LFS_DISK_VERSION >> 16))
#define LFS_DISK_VERSION_MINOR (0xffff & (LFS_DISK_VERSION >>  0))


/// Definitions ///

// Type definitions
typedef uint32_t lfs_size_t;
typedef int32_t  lfs_ssize_t;

typedef uint32_t lfs_off_t;
typedef int32_t  lfs_soff_t;

typedef uint32_t lfs_block_t;
typedef int32_t lfs_sblock_t;

typedef uint32_t lfsr_rid_t;
typedef int32_t  lfsr_srid_t;

typedef uint16_t lfsr_tag_t;
typedef int16_t  lfsr_stag_t;

typedef uint32_t lfsr_bid_t;
typedef int32_t  lfsr_sbid_t;

typedef uint32_t lfsr_mid_t;
typedef int32_t  lfsr_smid_t;

typedef uint32_t lfsr_did_t;
typedef int32_t  lfsr_sdid_t;

// Maximum name size in bytes, may be redefined to reduce the size of the
// info struct. Limited to <= 1022. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef LFS_NAME_MAX
#define LFS_NAME_MAX 255
#endif

// Maximum size of a file in bytes, may be redefined to limit to support other
// drivers. Limited on disk to <= 2147483647. Stored in superblock and must be
// respected by other littlefs drivers.
#ifndef LFS_FILE_MAX
#define LFS_FILE_MAX 2147483647
#endif

// TODO rm me
//// Maximum size of custom attributes in bytes, may be redefined, but there is
//// no real benefit to using a smaller LFS_ATTR_MAX. Limited to <= 1022.
//#ifndef LFS_ATTR_MAX
//#define LFS_ATTR_MAX 1022
//#endif
//
//// TODO document
//#ifndef LFS_UATTR_MAX
//#define LFS_UATTR_MAX 255
//#endif
//
//#ifndef LFS_SATTR_MAX
//#define LFS_SATTR_MAX 255
//#endif


// Possible error codes, these are negative to allow
// valid positive return values
enum lfs_error {
    LFS_ERR_OK          = 0,    // No error
    LFS_ERR_UNKNOWN     = -1,   // Unknown error
    LFS_ERR_INVAL       = -22,  // Invalid parameter
    LFS_ERR_NOTSUP      = -95,  // Operation not supported
    LFS_ERR_IO          = -5,   // Error during device operation
    LFS_ERR_CORRUPT     = -84,  // Corrupted
    LFS_ERR_NOENT       = -2,   // No directory entry
    LFS_ERR_EXIST       = -17,  // Entry already exists
    LFS_ERR_NOTDIR      = -20,  // Entry is not a dir
    LFS_ERR_ISDIR       = -21,  // Entry is a dir
    LFS_ERR_NOTEMPTY    = -39,  // Dir is not empty
    LFS_ERR_FBIG        = -27,  // File too large
    LFS_ERR_NOSPC       = -28,  // No space left on device
    LFS_ERR_NOMEM       = -12,  // No more memory available
    LFS_ERR_NOATTR      = -61,  // No data/attr available
    LFS_ERR_NAMETOOLONG = -36,  // File name too long
    LFS_ERR_RANGE       = -34,  // Result out of range
};

// File types
enum lfs_type {
    // file types
    LFS_TYPE_REG        = 1,
    LFS_TYPE_DIR        = 2,

    // internally used types, don't use these
    LFS_TYPE_BOOKMARK   = 4,
    LFS_TYPE_STICKYNOTE = 5,
    LFS_TYPE_TRAVERSAL  = 9,
};

// File open flags
#define LFS_O_MODE               3  // The file's access mode
#define LFS_O_RDONLY             0  // Open a file as read only
#define LFS_O_WRONLY             1  // Open a file as write only
#define LFS_O_RDWR               2  // Open a file as read and write
#define LFS_O_CREAT     0x00000004  // Create a file if it does not exist
#define LFS_O_EXCL      0x00000008  // Fail if a file already exists
#define LFS_O_TRUNC     0x00000010  // Truncate the existing file to zero size
#define LFS_O_APPEND    0x00000020  // Move to end of file on every write
#define LFS_O_FLUSH     0x00000040  // Flush data on every write
#define LFS_O_SYNC      0x00000080  // Sync metadata on every write
#define LFS_O_DESYNC    0x00000100  // Do not sync or recieve file updates
#define LFS_O_CKMETA    0x00100000  // Check metadata checksums
#define LFS_O_CKDATA    0x00200000  // Check metadata + data checksums

// internally used flags, don't use these
#define LFS_o_TYPE      0xf0000000  // The file's type
#define LFS_o_UNFLUSH   0x01000000  // File's data does not match disk
#define LFS_o_UNSYNC    0x02000000  // File's metadata does not match disk
#define LFS_o_UNCREAT   0x04000000  // File does not exist yet
#define LFS_o_ZOMBIE    0x08000000  // File has been removed

// File seek flags
#define LFS_SEEK_SET 0  // Seek relative to an absolute position
#define LFS_SEEK_CUR 1  // Seek relative to the current file position
#define LFS_SEEK_END 2  // Seek relative to the end of the file

// Custom attribute flags
#define LFS_A_MODE               3  // The attr's access mode
#define LFS_A_RDONLY             0  // Open an attr as read only
#define LFS_A_WRONLY             1  // Open an attr as write only
#define LFS_A_RDWR               2  // Open an attr as read and write
#define LFS_A_LAZY            0x04  // Only write attr if file changed

// Filesystem format flags
#define LFS_F_MODE               1  // Format's access mode
#define LFS_F_RDWR               0  // Format the filesystem as read and write
#ifdef LFS_NOISY
#define LFS_F_NOISY     0x00000010  // Add noise to revision counts
#endif
#ifdef LFS_CKPROGS
#define LFS_F_CKPROGS   0x00000800  // Check progs by reading back progged data
#endif
#ifdef LFS_CKFETCHES
#define LFS_F_CKFETCHES 0x00001000  // Check block checksums before first use
#endif
#ifdef LFS_CKPARITY
#define LFS_F_CKPARITY  0x00002000  // Check metadata tag parity bits
#endif
#ifdef LFS_CKDATACKSUMS
#define LFS_F_CKDATACKSUMS \
                        0x00008000  // Check data checksums on reads
#endif

#define LFS_F_CKMETA    0x00100000  // Check metadata checksums
#define LFS_F_CKDATA    0x00200000  // Check metadata + data checksums

// Filesystem mount flags
#define LFS_M_MODE               1  // Mount's access mode
#define LFS_M_RDWR               0  // Mount the filesystem as read and write
#define LFS_M_RDONLY             1  // Mount the filesystem as read only
#define LFS_M_FLUSH     0x00000040  // Open all files with LFS_O_FLUSH
#define LFS_M_SYNC      0x00000080  // Open all files with LFS_O_SYNC
#ifdef LFS_NOISY
#define LFS_M_NOISY     0x00000010  // Add noise to revision counts
#endif
#ifdef LFS_CKPROGS
#define LFS_M_CKPROGS   0x00000800  // Check progs by reading back progged data
#endif
#ifdef LFS_CKFETCHES
#define LFS_M_CKFETCHES 0x00001000  // Check block checksums before first use
#endif
#ifdef LFS_CKPARITY
#define LFS_M_CKPARITY  0x00002000  // Check metadata tag parity bits
#endif
#ifdef LFS_CKDATACKSUMS
#define LFS_M_CKDATACKSUMS \
                        0x00008000  // Check data checksums on reads
#endif

#define LFS_M_MKCONSISTENT \
                        0x00010000  // Make the filesystem consistent
#define LFS_M_LOOKAHEAD 0x00020000  // Populate lookahead buffer
#define LFS_M_COMPACT   0x00080000  // Compact metadata logs
#define LFS_M_CKMETA    0x00100000  // Check metadata checksums
#define LFS_M_CKDATA    0x00200000  // Check metadata + data checksums

// Filesystem info flags
#define LFS_I_RDONLY    0x00000001  // Mounted read only
#define LFS_I_FLUSH     0x00000040  // Mounted with LFS_M_FLUSH
#define LFS_I_SYNC      0x00000080  // Mounted with LFS_M_SYNC
#ifdef LFS_NOISY
#define LFS_I_NOISY     0x00000010  // Mounted with LFS_M_NOISY
#endif
#ifdef LFS_CKPROGS
#define LFS_I_CKPROGS   0x00000800  // Mounted with LFS_M_CKPROGS
#endif
#ifdef LFS_CKFETCHES
#define LFS_I_CKFETCHES 0x00001000  // Mounted with LFS_M_CKFETCHES
#endif
#ifdef LFS_CKPARITY
#define LFS_I_CKPARITY  0x00002000  // Mounted with LFS_M_CKPARITY
#endif
#ifdef LFS_CKDATACKSUMS
#define LFS_I_CKDATACKSUMS \
                        0x00008000  // Mounted with LFS_M_CKDATACKSUMS
#endif

#define LFS_I_MKCONSISTENT \
                        0x00010000  // Filesystem needs mkconsistent to write
#define LFS_I_LOOKAHEAD 0x00020000  // Lookahead buffer is not full
#define LFS_I_COMPACT   0x00080000  // Filesystem may have uncompacted metadata
#define LFS_I_CKMETA    0x00100000  // Metadata checksums not checked recently
#define LFS_I_CKDATA    0x00200000  // Data checksums not checked recently


// Block types
enum lfs_btype {
    LFS_BTYPE_MDIR      = 1,
    LFS_BTYPE_BTREE     = 2,
    LFS_BTYPE_DATA      = 3,
};

// Traversal flags
#define LFS_T_MTREEONLY 0x00000010  // Only traverse the mtree
#define LFS_T_MKCONSISTENT \
                        0x00010000  // Make the filesystem consistent
#define LFS_T_LOOKAHEAD 0x00020000  // Populate lookahead buffer
#define LFS_T_COMPACT   0x00080000  // Compact metadata logs
#define LFS_T_CKMETA    0x00100000  // Check metadata checksums
#define LFS_T_CKDATA    0x00200000  // Check metadata + data checksums

// internally used flags, don't use these
#define LFS_t_TSTATE    0x0000000f  // The traversal's current tstate
#define LFS_t_BTYPE     0x00000f00  // The traversal's current btype
#define LFS_t_DIRTY     0x01000000  // Filesystem modified during traversal
#define LFS_t_MUTATED   0x02000000  // Filesystem modified by traversal
#define LFS_t_ZOMBIE    0x08000000  // File has been removed

// GC flags
#define LFS_GC_MKCONSISTENT \
                        0x00010000  // Make the filesystem consistent
#define LFS_GC_LOOKAHEAD \
                        0x00020000  // Populate lookahead buffer
#define LFS_GC_COMPACT  0x00080000  // Compact metadata logs
#define LFS_GC_CKMETA   0x00100000  // Check metadata checksums
#define LFS_GC_CKDATA   0x00200000  // Check metadata + data checksums


// Configuration provided during initialization of the littlefs
struct lfs_config {
    // Opaque user provided context that can be used to pass
    // information to the block device operations
    void *context;

    // Read a region in a block. Negative error codes are propagated
    // to the user.
    int (*read)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, void *buffer, lfs_size_t size);

    // Program a region in a block. The block must have previously
    // been erased. Negative error codes are propagated to the user.
    // May return LFS_ERR_CORRUPT if the block should be considered bad.
    int (*prog)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, const void *buffer, lfs_size_t size);

    // Erase a block. A block must be erased before being programmed.
    // The state of an erased block is undefined. Negative error codes
    // are propagated to the user.
    // May return LFS_ERR_CORRUPT if the block should be considered bad.
    int (*erase)(const struct lfs_config *c, lfs_block_t block);

    // Sync the state of the underlying block device. Negative error codes
    // are propagated to the user.
    int (*sync)(const struct lfs_config *c);

#ifdef LFS_THREADSAFE
    // Lock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*lock)(const struct lfs_config *c);

    // Unlock the underlying block device. Negative error codes
    // are propagated to the user.
    int (*unlock)(const struct lfs_config *c);
#endif

    // Minimum size of a read in bytes. All read operations will be a
    // multiple of this value.
    lfs_size_t read_size;

    // Minimum size of a program in bytes. All program operations will be a
    // multiple of this value.
    lfs_size_t prog_size;

    // Size of an erasable block in bytes. This does not impact ram consumption
    // and may be larger than the physical erase size. Must be a multiple of
    // the read and program sizes.
    lfs_size_t block_size;

    // Number of erasable blocks on the device.
    lfs_size_t block_count;

    // Number of erase cycles before metadata blocks are relocated for
    // wear-leveling. Suggested values are in the range 16-1024. Larger values
    // relocate less frequently, improving average performance, at the cost
    // of worse wear distribution. Note this ends up rounded down to a
    // power-of-2.
    //
    // 0 results in pure copy-on-write, which may be counter-productive. Set
    // to -1 to disable block-level wear-leveling.
    int32_t block_recycles;

    // Size of the read cache in bytes. Larger buffers can improve
    // performance by storing more data and reducing the number of disk
    // accesses. Must be a multiple of the read size.
    lfs_size_t rcache_size;

    // Size of the program cache in bytes. Larger buffers can improve
    // performance by storing more data and reducing the number of disk
    // accesses. Must be a multiple of the program size.
    lfs_size_t pcache_size;

    // Size of file buffers in bytes. In addition to filesystem-wide
    // read/prog buffers, each file gets its own buffer to reduce disk
    // accesses.
    lfs_size_t file_buffer_size;

    // Size of the lookahead buffer in bytes. A larger lookahead buffer
    // increases the number of blocks found during an allocation pass. The
    // lookahead buffer is stored as a compact bitmap, so each byte of RAM
    // can track 8 blocks.
    lfs_size_t lookahead_size;

    #ifdef LFS_GC
    // Flags indicating what gc work to do during lfsr_gc calls.
    uint32_t gc_flags;
    #endif

    #ifdef LFS_GC
    // Number of gc steps to perform in each call to lfsr_gc, with each
    // step being ~1 block of work.
    //
    // More steps per call will make more progress if interleaved with
    // other filesystem operations, but may also introduce more latency.
    // steps=1 will do the minimum amount of work to make progress, and
    // steps=-1 will not return until all pending janitorial work has
    // been completed.
    //
    // Defaults to steps=1 when zero.
    lfs_soff_t gc_steps;
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
    lfs_size_t gc_compact_thresh;

    // Optional statically allocated read buffer. Must be rcache_size. By
    // default lfs_malloc is used to allocate this buffer.
    void *rcache_buffer;

    // Optional statically allocated program buffer. Must be pcache_size. By
    // default lfs_malloc is used to allocate this buffer.
    void *pcache_buffer;

    // Optional statically allocated lookahead buffer. Must be lookahead_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *lookahead_buffer;

    // Optional upper limit on length of file names in bytes. No downside for
    // larger names except the size of the info struct which is controlled by
    // the LFS_NAME_MAX define. Defaults to LFS_NAME_MAX when zero. Stored in
    // superblock and must be respected by other littlefs drivers.
    lfs_size_t name_limit;

    // Optional upper limit on files in bytes. No downside for larger files
    // but must be <= LFS_FILE_MAX. Defaults to LFS_FILE_MAX when zero. Stored
    // in superblock and must be respected by other littlefs drivers.
    lfs_size_t file_limit;

// TODO rm me
//    // Optional upper limit on custom attributes in bytes. No downside for
//    // larger attributes size but must be <= LFS_ATTR_MAX. Defaults to
//    // LFS_ATTR_MAX when zero.
//    lfs_size_t attr_max;
//
//    // Optional upper limit on total space given to metadata pairs in bytes. On
//    // devices with large blocks (e.g. 128kB) setting this to a low size (2-8kB)
//    // can help bound the metadata compaction time. Must be <= block_size.
//    // Defaults to block_size when zero.
//    lfs_size_t metadata_max;

    // TODO these are pretty low-level details, should we have reasonable
    // defaults? need to benchmark.

    // Maximum size on inlined files in bytes. Inlined files decrease storage
    // requirements, but may impact metadata-related performance. Must be <=
    // block_size/4.
    //
    // 0 disables inline files.
    lfs_size_t inline_size;

    // Maximum size of inlined trees (shrubs) in bytes. Shrubs reduce B-tree
    // root overhead, but may impact metadata-related performance. Must be <=
    // blocksize/4.
    //
    // 0 disables shrubs.
    lfs_size_t shrub_size;

    // Maximum size of a non-block B-tree leaf in bytes. Smaller values may
    // make small random-writes cheaper, but increase metadata overhead. Must
    // be <= block_size/4.
    lfs_size_t fragment_size;

    // Threshold for compacting multiple fragments into a block. Smaller
    // values will compact more frequently, reducing disk usage, but
    // increasing the cost of random-writes.
    //
    // 0 only writes blocks, minimizing disk usage, while -1 or any value >=
    // block_size only writes fragments, minimizing random-write cost.
    lfs_size_t crystal_thresh;
};

// File info structure
struct lfs_info {
    // Type of the file, either LFS_TYPE_REG or LFS_TYPE_DIR
    uint8_t type;

    // Size of the file, only valid for REG files. Limited to 32-bits.
    lfs_size_t size;

    // Name of the file stored as a null-terminated string. Limited to
    // LFS_NAME_MAX+1, which can be changed by redefining LFS_NAME_MAX to
    // reduce RAM. LFS_NAME_MAX is stored in superblock and must be
    // respected by other littlefs drivers.
    char name[LFS_NAME_MAX+1];
};

// Filesystem info structure
struct lfs_fsinfo {
    // Filesystem flags
    uint32_t flags;

    // Size of a logical block in bytes.
    lfs_size_t block_size;

    // Number of logical blocks in the filesystem.
    lfs_size_t block_count;

    // Upper limit on the length of file names in bytes.
    lfs_size_t name_limit;

    // Upper limit on the size of files in bytes.
    lfs_size_t file_limit;
};

// Traversal info structure
struct lfs_tinfo {
    // Type of the block
    uint8_t btype;

    // Block address
    lfs_block_t block;
};

//// Custom attribute structure, used to describe custom attributes
//// committed atomically during file writes.
//struct lfs_attr {
//    // 8-bit type of attribute, provided by user and used to
//    // identify the attribute
//    uint8_t type;
//
//    // Pointer to buffer containing the attribute
//    void *buffer;
//
//    // Size of attribute in bytes, limited to LFS_ATTR_MAX
//    lfs_size_t size;
//};

// Custom attribute structure, used to describe custom attributes
// committed atomically during file writes.
struct lfs_attr {
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
    // LFS_ERR_NOATTR to remove the attr
    lfs_ssize_t buffer_size;

    // Optional pointer to a mutable attr size, updated on read/write,
    // set to LFS_ERR_NOATTR if attr does not exist
    //
    // Defaults to buffer_size if NULL
    lfs_ssize_t *size;
};

// Optional configuration provided during lfs_file_opencfg
struct lfs_file_config {
    // Optional statically allocated file buffer. Must be buffer_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *buffer;

    // Size of the file buffer in bytes. In addition to filesystem-wide
    // read/prog buffers, each file gets its own buffer to reduce disk
    // accesses. Defaults to file_buffer_size.
    lfs_size_t buffer_size;

//    // Optional list of custom attributes related to the file. If the file
//    // is opened with read access, these attributes will be read from disk
//    // during the open call. If the file is opened with write access, the
//    // attributes will be written to disk every file sync or close. This
//    // write occurs atomically with update to the file's contents.
//    //
//    // Custom attributes are uniquely identified by an 8-bit type and limited
//    // to LFS_ATTR_MAX bytes. When read, if the stored attribute is smaller
//    // than the buffer, it will be padded with zeros. If the stored attribute
//    // is larger, then it will be silently truncated. If the attribute is not
//    // found, it will be created implicitly.
//    struct lfs_attr *attrs;
//
//    // Number of custom attributes in the list
//    lfs_size_t attr_count;

    // Optional list of custom attributes attached to the file. If readable,
    // these attributes will be kept up to date with the attributes on-disk.
    // If writeable, these attributes will be written to disk atomically on
    // every file sync or close.
    struct lfs_attr *attrs;

    // Number of custom attributes in the list
    lfs_size_t attr_count;
};


/// internal littlefs data structures ///

//typedef struct lfs_cache {
//    lfs_block_t block;
//    lfs_size_t off;
//    lfs_size_t size;
//    uint8_t *buffer;
//} lfs_cache_t;

//typedef struct lfs_mdir {
//    lfs_block_t pair[2];
//    uint32_t rev;
//    lfs_off_t off;
//    uint32_t etag;
//    uint16_t count;
//    bool erased;
//    bool split;
//    lfs_block_t tail[2];
//} lfs_mdir_t;

// either an on-disk or in-RAM data pointer
//
// note, it's tempting to make this fancier, but we benefit quite a lot
// from the compiler being able to aggresively optimize this struct
//
typedef struct lfsr_data {
    // sign2(size)=0b00 => in-RAM buffer
    // sign2(size)=0b10 => on-disk data
    // sign2(size)=0b11 => on-disk data + cksum
    lfs_size_t size;
    union {
        const uint8_t *buffer;
        struct {
            lfs_block_t block;
            lfs_size_t off;
            // optional context for validating data
            #ifdef LFS_CKDATACKSUMS
            lfs_size_t cksize;
            uint32_t cksum;
            #endif
        } disk;
    } u;
} lfsr_data_t;

// a possible block pointer
typedef struct lfsr_bptr {
    // sign2(size)=0b00 => in-RAM buffer
    // sign2(size)=0b10 => on-disk data
    // sign2(size)=0b11 => block pointer
    lfsr_data_t data;
    #ifndef LFS_CKDATACKSUMS
    lfs_size_t cksize;
    uint32_t cksum;
    #endif
} lfsr_bptr_t;

// littlefs's core metadata log type
typedef struct lfsr_rbyd {
    lfsr_rid_t weight;
    lfs_block_t blocks[2];
    // sign(trunk)=0 => normal rbyd
    // sign(trunk)=1 => shrub rbyd
    lfs_size_t trunk;
    // sign(eoff)       => perturb bit
    // eoff=0, trunk=0  => not yet committed
    // eoff=0, trunk>0  => not yet fetched
    // eoff>=block_size => rbyd not erased/needs compaction
    lfs_size_t eoff;
    uint32_t cksum;
} lfsr_rbyd_t;

// a btree is represented by the root rbyd
typedef lfsr_rbyd_t lfsr_btree_t;

// littlefs's atomic metadata log type
typedef struct lfsr_mdir {
    lfsr_smid_t mid;
    lfsr_rbyd_t rbyd;
    uint32_t gcksumdelta;
} lfsr_mdir_t;

typedef struct lfsr_omdir {
    struct lfsr_omdir *next;
    uint32_t flags;
    lfsr_mdir_t mdir;
} lfsr_omdir_t;

// a shrub is a secondary trunk in an mdir
typedef lfsr_rbyd_t lfsr_shrub_t;

// a bshrub is like a btree but with a shrub as a root
typedef struct lfsr_bshrub {
    // bshrubs need to be tracked for commits to work
    lfsr_omdir_t o;
    // files contain both an active bshrub and staging bshrub, to allow
    // staging during mdir compacts
    // trunk=0       => no bshrub/btree
    // sign(trunk)=1 => bshrub
    // sign(trunk)=0 => btree
    lfsr_shrub_t shrub;
    lfsr_shrub_t shrub_;
} lfsr_bshrub_t;

// littlefs file type

//typedef struct lfs_file {
//    struct lfs_file *next;
//    uint16_t id;
//    uint8_t type;
//    lfs_mdir_t m;
//
//    struct lfs_ctz {
//        lfs_block_t head;
//        lfs_size_t size;
//    } ctz;
//
//    uint32_t flags;
//    lfs_off_t pos;
//    lfs_block_t block;
//    lfs_off_t off;
//    lfs_cache_t cache;
//
//    const struct lfs_file_config *cfg;
//} lfs_file_t;

typedef struct lfsr_file {
    lfsr_bshrub_t b;
    const struct lfs_file_config *cfg;

    lfs_off_t pos;

    // note this lines up with lfsr_data_t's buffer representation
    struct {
        lfs_off_t size;
        uint8_t *buffer;
        lfs_off_t pos;
    } buffer;

    lfs_block_t eblock;
    lfs_size_t eoff;
} lfsr_file_t;

// littlefs directory type

//typedef struct lfs_dir {
//    struct lfs_dir *next;
//    uint16_t id;
//    uint8_t type;
//    lfs_mdir_t m;
//
//    lfs_off_t pos;
//    lfs_block_t head[2];
//} lfs_dir_t;

typedef struct lfsr_dir {
    lfsr_omdir_t o;
    lfsr_did_t did;
    lfs_off_t pos;
} lfsr_dir_t;

// littlefs traversal type

typedef struct lfsr_btraversal {
    lfsr_bid_t bid;
    const lfsr_rbyd_t *branch;
    lfsr_srid_t rid;
    lfsr_rbyd_t rbyd;
} lfsr_btraversal_t;

typedef struct lfsr_traversal {
    // mdir/bshrub/btree state, this also includes our traversal
    // state machine
    lfsr_bshrub_t b;
    // opened file state
    lfsr_omdir_t *ot;
    union {
        // cycle detection state, only valid when traversing the mroot chain
        struct {
            lfs_block_t blocks[2];
            lfs_block_t step;
            uint8_t power;
        } mtortoise;
        // btree traversal state
        lfsr_btraversal_t bt;
    } u;

    // recalculate gcksum when traversing with ckmeta
    uint32_t gcksum;
    // pending blocks, only used in lfsr_traversal_read
    lfs_sblock_t blocks[2];
} lfsr_traversal_t;


//typedef struct lfs_superblock {
//    uint32_t version;
//    lfs_size_t block_size;
//    lfs_size_t block_count;
//    lfs_size_t name_max;
//    lfs_size_t file_max;
//    lfs_size_t attr_max;
//} lfs_superblock_t;
//
//typedef struct lfs_gstate {
//    uint32_t tag;
//    lfs_block_t pair[2];
//} lfs_gstate_t;

// grm encoding:
// .---.                  mode:  1 leb128   1 byte
// |mod|                  mids:  2 leb128s  <=2x5 bytes
// +- -+- -+- -+- -+- -.  total:            <=11 bytes
// ' mid x mod         '
// +                   +
// '                   '
// '- -+- -+- -+- -+- -'
//
#define LFSR_GRM_DSIZE (1+5+5)

typedef struct lfsr_grm {
    lfsr_smid_t mids[2];
} lfsr_grm_t;

#ifdef LFS_CKPARITY
typedef struct lfsr_tailp {
    lfs_block_t block;
    // sign(off) => tail parity
    lfs_size_t off;
} lfsr_tailp_t;
#endif

// The littlefs filesystem type
typedef struct lfs {
    const struct lfs_config *cfg;
    uint32_t flags;
    lfs_size_t block_count;
    lfs_size_t name_limit;
    lfs_off_t file_limit;

    int8_t recycle_bits;
    uint8_t rat_estimate;
    uint8_t mdir_bits;

    // linked-list of opened mdirs
    lfsr_omdir_t *omdirs;

    lfsr_mdir_t mroot;
    lfsr_btree_t mtree;

    struct {
        lfs_block_t block;
        lfs_size_t off;
        lfs_size_t size;
        uint8_t *buffer;
    } rcache;
    struct {
        lfs_block_t block;
        lfs_size_t off;
        lfs_size_t size;
        uint8_t *buffer;
    } pcache;

    #ifdef LFS_CKPARITY
    lfsr_tailp_t tailp;
    #endif

    struct lfs_lookahead {
        lfs_block_t window;
        lfs_block_t off;
        lfs_block_t size;
        lfs_block_t ckpoint;
        uint8_t *buffer;
    } lookahead;

    uint32_t gcksum;
    uint32_t gcksum_p;
    uint32_t gcksum_d;

    lfsr_grm_t grm;
    uint8_t grm_p[LFSR_GRM_DSIZE];
    uint8_t grm_d[LFSR_GRM_DSIZE];

    #ifdef LFS_GC
    struct {
        lfsr_traversal_t t;
    } gc;
    #endif
} lfs_t;


/// Filesystem functions ///

#ifndef LFS_READONLY
// Format a block device with the littlefs
//
// Requires a littlefs object and config struct. This clobbers the littlefs
// object, and does not leave the filesystem mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
//int lfs_format(lfs_t *lfs, const struct lfs_config *config);
int lfsr_format(lfs_t *lfs, uint32_t flags,
        const struct lfs_config *cfg);
#endif

// Mounts a littlefs
//
// Requires a littlefs object and config struct. Multiple filesystems
// may be mounted simultaneously with multiple littlefs objects. Both
// lfs and config must be allocated while mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
//int lfs_mount(lfs_t *lfs, const struct lfs_config *config);
int lfsr_mount(lfs_t *lfs, uint32_t flags,
        const struct lfs_config *cfg);

// Unmounts a littlefs
//
// Does nothing besides releasing any allocated resources.
// Returns a negative error code on failure.
//int lfs_unmount(lfs_t *lfs);
int lfsr_unmount(lfs_t *lfs);

/// General operations ///

#ifndef LFS_READONLY
// Removes a file or directory
//
// If removing a directory, the directory must be empty.
// Returns a negative error code on failure.
//int lfs_remove(lfs_t *lfs, const char *path);
int lfsr_remove(lfs_t *lfs, const char *path);
#endif

#ifndef LFS_READONLY
// Rename or move a file or directory
//
// If the destination exists, it must match the source in type.
// If the destination is a directory, the directory must be empty.
//
// Returns a negative error code on failure.
//int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath);
int lfsr_rename(lfs_t *lfs, const char *old_path, const char *new_path);
#endif

// Find info about a file or directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a negative error code on failure.
//int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info);
int lfsr_stat(lfs_t *lfs, const char *path, struct lfs_info *info);

// Get a custom attribute
//
// Returns the number of bytes read, or a negative error code on failure.
// Note this may be less than the on-disk attr size if the buffer is not
// large enough.
//lfs_ssize_t lfs_getattr(lfs_t *lfs, const char *path,
//        uint8_t type, void *buffer, lfs_size_t size);
lfs_ssize_t lfsr_getattr(lfs_t *lfs, const char *path, uint8_t type,
        void *buffer, lfs_size_t size);

// Get a custom attribute's size
//
// Returns the size of the attribute, or a negative error code on failure.
lfs_ssize_t lfsr_sizeattr(lfs_t *lfs, const char *path, uint8_t type);

#ifndef LFS_READONLY
// Set a custom attributes
//
// Returns a negative error code on failure.
//int lfs_setattr(lfs_t *lfs, const char *path,
//        uint8_t type, const void *buffer, lfs_size_t size);
int lfsr_setattr(lfs_t *lfs, const char *path, uint8_t type,
        const void *buffer, lfs_size_t size);
#endif

#ifndef LFS_READONLY
// Removes a custom attribute
//
// Returns a negative error code on failure.
//int lfs_removeattr(lfs_t *lfs, const char *path, uint8_t type);
int lfsr_removeattr(lfs_t *lfs, const char *path, uint8_t type);
#endif


/// File operations ///

#ifndef LFS_NO_MALLOC
// Open a file
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum lfs_open_flags that are bitwise-ored together.
//
// Returns a negative error code on failure.
//int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
//        const char *path, int flags);
int lfsr_file_open(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags);

// if LFS_NO_MALLOC is defined, lfs_file_open() will fail with LFS_ERR_NOMEM
// thus use lfs_file_opencfg() with config.buffer set.
#endif

// Open a file with extra configuration
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum lfs_open_flags that are bitwise-ored together.
//
// The config struct provides additional config options per file as described
// above. The config struct must remain allocated while the file is open, and
// the config struct must be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
//int lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file,
//        const char *path, int flags,
//        const struct lfs_file_config *config);
int lfsr_file_opencfg(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs_file_config *cfg);

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
//int lfs_file_close(lfs_t *lfs, lfs_file_t *file);
int lfsr_file_close(lfs_t *lfs, lfsr_file_t *file);

// Synchronize a file on storage
//
// Any pending writes are written out to storage and other open files.
//
// If the file was desynchronized, it is now marked as synchronized. It will
// now recieve file updates and syncs on close.
//
// Returns a negative error code on failure.
//int lfs_file_sync(lfs_t *lfs, lfs_file_t *file);
int lfsr_file_sync(lfs_t *lfs, lfsr_file_t *file);

// Flush any buffered data
//
// This does not update metadata and is called implicitly by lfsr_file_sync.
// Calling this explicitly may be useful for preventing write errors in
// read operations.
//
// Returns a negative error code on failure.
int lfsr_file_flush(lfs_t *lfs, lfsr_file_t *file);

// Mark a file as desynchronized
//
// Desynchronized files do not recieve file updates and do not sync on close.
// They effectively act as snapshots of the underlying file at that point
// in time.
//
// If an error occurs during a write operation, the file is implicitly marked
// as desynchronized.
//
// An explicit and successful call to either lfsr_file_sync or
// lfsr_file_resync reverses this, marking the file as synchronized again.
//
// Returns a negative error code on failure.
int lfsr_file_desync(lfs_t *lfs, lfsr_file_t *file);

// Discard unsynchronized changes and mark a file as synchronized
//
// This is effectively the same as closing and reopening the file, and
// may read from disk to figure out file state.
//
// Returns a negative error code on failure.
int lfsr_file_resync(lfs_t *lfs, lfsr_file_t *file);

// Read data from file
//
// Takes a buffer and size indicating where to store the read data.
// Returns the number of bytes read, or a negative error code on failure.
//lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
//        void *buffer, lfs_size_t size);
lfs_ssize_t lfsr_file_read(lfs_t *lfs, lfsr_file_t *file,
        void *buffer, lfs_size_t size);

#ifndef LFS_READONLY
// Write data to file
//
// Takes a buffer and size indicating the data to write. The file will not
// actually be updated on the storage until either sync or close is called.
//
// Returns the number of bytes written, or a negative error code on failure.
//lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
//        const void *buffer, lfs_size_t size);
lfs_ssize_t lfsr_file_write(lfs_t *lfs, lfsr_file_t *file,
        const void *buffer, lfs_size_t size);
#endif

// Change the position of the file
//
// The change in position is determined by the offset and whence flag.
// Returns the new position of the file, or a negative error code on failure.
//lfs_soff_t lfs_file_seek(lfs_t *lfs, lfs_file_t *file,
//        lfs_soff_t off, int whence);
lfs_soff_t lfsr_file_seek(lfs_t *lfs, lfsr_file_t *file,
        lfs_soff_t off, uint8_t whence);

#ifndef LFS_READONLY
// Truncate/grow the size of the file to the specified size
//
// If size is larger than the current file size, a hole is created, appearing
// as if the file was filled with zeros.
//
// Returns a negative error code on failure.
//int lfs_file_truncate(lfs_t *lfs, lfs_file_t *file, lfs_off_t size);
int lfsr_file_truncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size);
#endif

#ifndef LFS_READONLY
// Truncate/grow the file, but from the front
//
// If size is larger than the current file size, a hole is created, appearing
// as if the file was filled with zeros.
//
// Returns a negative error code on failure.
int lfsr_file_fruncate(lfs_t *lfs, lfsr_file_t *file, lfs_off_t size);
#endif

// Return the position of the file
//
// Equivalent to lfs_file_seek(lfs, file, 0, LFS_SEEK_CUR)
// Returns the position of the file, or a negative error code on failure.
//lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file);
lfs_soff_t lfsr_file_tell(lfs_t *lfs, lfsr_file_t *file);

// Change the position of the file to the beginning of the file
//
// Equivalent to lfs_file_seek(lfs, file, 0, LFS_SEEK_SET)
// Returns a negative error code on failure.
//int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file);
int lfsr_file_rewind(lfs_t *lfs, lfsr_file_t *file);

// Return the size of the file
//
// Similar to lfs_file_seek(lfs, file, 0, LFS_SEEK_END)
// Returns the size of the file, or a negative error code on failure.
//lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file);
lfs_soff_t lfsr_file_size(lfs_t *lfs, lfsr_file_t *file);

// Check a file for metadata errors
//
// Returns LFS_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
int lfsr_file_ckmeta(lfs_t *lfs, lfsr_file_t *file);

// Check a file for metadata + data errors
//
// Returns LFS_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
int lfsr_file_ckdata(lfs_t *lfs, lfsr_file_t *file);


/// Directory operations ///

#ifndef LFS_READONLY
// Create a directory
//
// Returns a negative error code on failure.
//int lfs_mkdir(lfs_t *lfs, const char *path);
int lfsr_mkdir(lfs_t *lfs, const char *path);
#endif

// Open a directory
//
// Once open a directory can be used with read to iterate over files.
// Returns a negative error code on failure.
//int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path);
int lfsr_dir_open(lfs_t *lfs, lfsr_dir_t *dir, const char *path);

// Close a directory
//
// Releases any allocated resources.
// Returns a negative error code on failure.
//int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir);
int lfsr_dir_close(lfs_t *lfs, lfsr_dir_t *dir);

// Read an entry in the directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns 0 on success, LFS_ERR_NOENT at the end of directory, or a
// negative error code on failure.
//int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info);
int lfsr_dir_read(lfs_t *lfs, lfsr_dir_t *dir, struct lfs_info *info);

// Change the position of the directory
//
// The new off must be a value previous returned from tell and specifies
// an absolute offset in the directory seek.
//
// Returns a negative error code on failure.
//int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off);
int lfsr_dir_seek(lfs_t *lfs, lfsr_dir_t *dir, lfs_soff_t off);

// Return the position of the directory
//
// The returned offset is only meant to be consumed by seek and may not make
// sense, but does indicate the current position in the directory iteration.
//
// Returns the position of the directory, or a negative error code on failure.
//lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir);
lfs_soff_t lfsr_dir_tell(lfs_t *lfs, lfsr_dir_t *dir);

// Change the position of the directory to the beginning of the directory
//
// Returns a negative error code on failure.
//int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir);
int lfsr_dir_rewind(lfs_t *lfs, lfsr_dir_t *dir);


/// Traversal operations ///

// Open a traversal
//
// Once open, a traversal can be read from to iterate over all blocks in
// the filesystem.
//
// Returns a negative error code on failure.
int lfsr_traversal_open(lfs_t *lfs, lfsr_traversal_t *t, uint32_t flags);

// Close a traversal
//
// Releases any allocated resources.
// Returns a negative error code on failure.
int lfsr_traversal_close(lfs_t *lfs, lfsr_traversal_t *t);

// Progress the traversal and read an entry
//
// Fills out the tinfo structure.
//
// Returns 0 on success, LFS_ERR_NOENT at the end of traversal, or a
// negative error code on failure.
int lfsr_traversal_read(lfs_t *lfs, lfsr_traversal_t *t,
        struct lfs_tinfo *tinfo);

// Reset the traversal
//
// Returns a negative error code on failure.
int lfsr_traversal_rewind(lfs_t *lfs, lfsr_traversal_t *t);


/// Filesystem-level filesystem operations

// Find on-disk info about the filesystem
//
// Fills out the fsinfo structure based on the filesystem found on-disk.
// Returns a negative error code on failure.
int lfsr_fs_stat(lfs_t *lfs, struct lfs_fsinfo *fsinfo);

// Finds the current size of the filesystem
//
// Note: Result is best effort. If files share COW structures, the returned
// size may be larger than the filesystem actually is.
//
// Returns the number of allocated blocks, or a negative error code on failure.
//lfs_ssize_t lfs_fs_size(lfs_t *lfs);
lfs_ssize_t lfsr_fs_size(lfs_t *lfs);

// Traverse through all blocks in use by the filesystem
//
// The provided callback will be called with each block address that is
// currently in use by the filesystem. This can be used to determine which
// blocks are in use or how much of the storage is available.
//
// Returns a negative error code on failure.
//int lfs_fs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data);

#ifndef LFS_READONLY
// Attempt to make the filesystem consistent and ready for writing
//
// Calling this function is not required, consistency will be implicitly
// enforced on the first operation that writes to the filesystem, but this
// function allows the work to be performed earlier and without other
// filesystem changes.
//
// Returns a negative error code on failure.
int lfsr_fs_mkconsistent(lfs_t *lfs);
#endif

#ifndef LFS_READONLY
// Check the filesystem for metadata errors
//
// Returns LFS_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
int lfsr_fs_ckmeta(lfs_t *lfs);
#endif

#ifndef LFS_READONLY
// Check the filesystem for metadata + data errors
//
// Returns LFS_ERR_CORRUPT if a checksum mismatch is found, or a negative
// error code on failure.
int lfsr_fs_ckdata(lfs_t *lfs);
#endif

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
int lfsr_fs_cksum(lfs_t *lfs, uint32_t *cksum);

#ifdef LFS_GC
// Perform any janitorial work that may be pending
//
// The exact janitorial work depends on the configured flags and steps.
//
// Calling this function is not required, but may allow the offloading of
// expensive janitorial work to a less time-critical code path.
//
// Returns a negative error code on failure.
int lfsr_fs_gc(lfs_t *lfs);
#endif

// Mark janitorial work as incomplete
//
// Any info flags passed to lfsr_gc_unck will be reset internally,
// forcing the work to be redone.
//
// This is most useful for triggering new ckmeta/ckdata scans with
// LFS_I_CANCKMETA and LFS_I_CANCKDATA. Otherwise littlefs will perform
// only one scan after mount.
//
// Returns a negative error code on failure.
int lfsr_fs_unck(lfs_t *lfs, uint32_t flags);

#ifndef LFS_READONLY
// Change the number of blocks used by the filesystem
//
// This changes the number of blocks we are currently using and updates
// the superblock with the new block count.
//
// Note: This is irreversible.
//
// Returns a negative error code on failure.
int lfsr_fs_grow(lfs_t *lfs, lfs_size_t block_count);
#endif

#ifndef LFS_READONLY
#ifdef LFS_MIGRATE
// Attempts to migrate a previous version of littlefs
//
// Behaves similarly to the lfs_format function. Attempts to mount
// the previous version of littlefs and update the filesystem so it can be
// mounted with the current version of littlefs.
//
// Requires a littlefs object and config struct. This clobbers the littlefs
// object, and does not leave the filesystem mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
//int lfs_migrate(lfs_t *lfs, const struct lfs_config *cfg);
#endif
#endif


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
