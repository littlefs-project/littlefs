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
#define LFS_DISK_VERSION 0x00020000
#define LFS_DISK_VERSION_MAJOR (0xffff & (LFS_DISK_VERSION >> 16))
#define LFS_DISK_VERSION_MINOR (0xffff & (LFS_DISK_VERSION >>  0))


/// Definitions ///

// Type definitions
typedef uint32_t lfs_size_t;
typedef int32_t  lfs_ssize_t;

typedef uint32_t lfs_off_t;
typedef int32_t  lfs_soff_t;

typedef uint32_t lfs_block_t;

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
// drivers. Limited on disk to <= 4294967296. However, above 2147483647 the
// functions lfs_file_seek, lfs_file_size, and lfs_file_tell will return
// incorrect values due to using signed integers. Stored in superblock and
// must be respected by other littlefs drivers.
#ifndef LFS_FILE_MAX
#define LFS_FILE_MAX 2147483647
#endif

// TODO rm me
//// Maximum size of custom attributes in bytes, may be redefined, but there is
//// no real benefit to using a smaller LFS_ATTR_MAX. Limited to <= 1022.
//#ifndef LFS_ATTR_MAX
//#define LFS_ATTR_MAX 1022
//#endif

// TODO document
#ifndef LFS_UATTR_MAX
#define LFS_UATTR_MAX 255
#endif

#ifndef LFS_SATTR_MAX
#define LFS_SATTR_MAX 255
#endif


// Possible error codes, these are negative to allow
// valid positive return values
enum lfs_error {
    LFS_ERR_OK          = 0,    // No error
    LFS_ERR_IO          = -5,   // Error during device operation
    LFS_ERR_CORRUPT     = -84,  // Corrupted
    LFS_ERR_NOENT       = -2,   // No directory entry
    LFS_ERR_EXIST       = -17,  // Entry already exists
    LFS_ERR_NOTDIR      = -20,  // Entry is not a dir
    LFS_ERR_ISDIR       = -21,  // Entry is a dir
    LFS_ERR_NOTEMPTY    = -39,  // Dir is not empty
    LFS_ERR_BADF        = -9,   // Bad file number
    LFS_ERR_FBIG        = -27,  // File too large
    LFS_ERR_INVAL       = -22,  // Invalid parameter
    LFS_ERR_NOSPC       = -28,  // No space left on device
    LFS_ERR_NOMEM       = -12,  // No more memory available
    LFS_ERR_NOATTR      = -61,  // No data/attr available
    LFS_ERR_NAMETOOLONG = -36,  // File name too long
    LFS_ERR_RANGE       = -34,  // Result out of range
};

// File types
enum lfs_type {
    // file types
    LFS_TYPE_REG            = 1,
    LFS_TYPE_DIR            = 2,

    // used internally, don't use this
    LFS_TYPE_INTERNAL       = 3,

//    // internally used types
//    LFS_TYPE_SPLICE         = 0x400,
//    LFS_TYPE_NAME           = 0x000,
//    LFS_TYPE_STRUCT         = 0x200,
//    LFS_TYPE_USERATTR       = 0x300,
//    LFS_TYPE_FROM           = 0x100,
//    LFS_TYPE_TAIL           = 0x600,
//    LFS_TYPE_GLOBALS        = 0x700,
//    LFS_TYPE_CRC            = 0x500,
//
//    // internally used type specializations
//    LFS_TYPE_CREATE         = 0x401,
//    LFS_TYPE_DELETE         = 0x4ff,
//    LFS_TYPE_SUPERBLOCK     = 0x0ff,
//    LFS_TYPE_DIRSTRUCT      = 0x200,
//    LFS_TYPE_CTZSTRUCT      = 0x202,
//    LFS_TYPE_INLINESTRUCT   = 0x201,
//    LFS_TYPE_SOFTTAIL       = 0x600,
//    LFS_TYPE_HARDTAIL       = 0x601,
//    LFS_TYPE_MOVESTATE      = 0x7ff,
//    LFS_TYPE_CCRC           = 0x500,
//    LFS_TYPE_FCRC           = 0x5ff,
//
//    // internal chip sources
//    LFS_FROM_NOOP           = 0x000,
//    LFS_FROM_MOVE           = 0x101,
//    LFS_FROM_USERATTRS      = 0x102,
};

// File open flags
enum lfs_open_flags {
    // open flags
    LFS_O_RDONLY = 1,         // Open a file as read only
#ifndef LFS_READONLY
    LFS_O_WRONLY = 2,         // Open a file as write only
    LFS_O_RDWR   = 3,         // Open a file as read and write
    LFS_O_CREAT  = 0x0100,    // Create a file if it does not exist
    LFS_O_EXCL   = 0x0200,    // Fail if a file already exists
    LFS_O_TRUNC  = 0x0400,    // Truncate the existing file to zero size
    LFS_O_APPEND = 0x0800,    // Move to end of file on every write
    LFS_O_DESYNC = 0x1000,    // Do not sync or recieve file updates
#endif

    // internally used flags
    LFS_F_UNFLUSHED = 0x010000, // File's data does not match storage
    LFS_F_UNSYNCED  = 0x020000, // File's metadata does not match storage
};

// File seek flags
enum lfs_whence_flags {
    LFS_SEEK_SET = 0,   // Seek relative to an absolute position
    LFS_SEEK_CUR = 1,   // Seek relative to the current file position
    LFS_SEEK_END = 2,   // Seek relative to the end of the file
};


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

    // Minimum size of a block read in bytes. All read operations will be a
    // multiple of this value.
    lfs_size_t read_size;

    // Minimum size of a block program in bytes. All program operations will be
    // a multiple of this value.
    lfs_size_t prog_size;

    // Size of an erasable block in bytes. This does not impact ram consumption
    // and may be larger than the physical erase size. However, non-inlined
    // files take up at minimum one block. Must be a multiple of the read and
    // program sizes.
    lfs_size_t block_size;

    // Number of erasable blocks on the device.
    lfs_size_t block_count;

    // Number of erase cycles before littlefs evicts metadata logs and moves
    // the metadata to another block. Suggested values are in the
    // range 100-1000, with large values having better performance at the cost
    // of less consistent wear distribution.
    //
    // Set to -1 to disable block-level wear-leveling.
    int32_t block_cycles;

    // Size of block caches in bytes. Each cache buffers a portion of a block in
    // RAM. The littlefs needs a read cache, a program cache, and one additional
    // cache per file. Larger caches can improve performance by storing more
    // data and reducing the number of disk accesses. Must be a multiple of the
    // read and program sizes, and a factor of the block size.
    lfs_size_t cache_size;

    // Size of the lookahead buffer in bytes. A larger lookahead buffer
    // increases the number of blocks found during an allocation pass. The
    // lookahead buffer is stored as a compact bitmap, so each byte of RAM
    // can track 8 blocks. Must be a multiple of 8.
    lfs_size_t lookahead_size;

    // Optional statically allocated read buffer. Must be cache_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *read_buffer;

    // Optional statically allocated program buffer. Must be cache_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *prog_buffer;

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
    lfs_size_t size_limit;

    // TODO document
    lfs_size_t uattr_limit;
    lfs_size_t sattr_limit;

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

    // TODO document
    lfs_size_t inline_size;
    lfs_size_t shrub_size;
    lfs_size_t fragment_size;
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

// Custom attribute structure, used to describe custom attributes
// committed atomically during file writes.
struct lfs_attr {
    // 8-bit type of attribute, provided by user and used to
    // identify the attribute
    uint8_t type;

    // Pointer to buffer containing the attribute
    void *buffer;

    // Size of attribute in bytes, limited to LFS_ATTR_MAX
    lfs_size_t size;
};

// Optional configuration provided during lfs_file_opencfg
struct lfs_file_config {
    // Optional statically allocated file buffer. Must be cache_size.
    // By default lfs_malloc is used to allocate this buffer.
    void *buffer;

    // Optional list of custom attributes related to the file. If the file
    // is opened with read access, these attributes will be read from disk
    // during the open call. If the file is opened with write access, the
    // attributes will be written to disk every file sync or close. This
    // write occurs atomically with update to the file's contents.
    //
    // Custom attributes are uniquely identified by an 8-bit type and limited
    // to LFS_ATTR_MAX bytes. When read, if the stored attribute is smaller
    // than the buffer, it will be padded with zeros. If the stored attribute
    // is larger, then it will be silently truncated. If the attribute is not
    // found, it will be created implicitly.
    struct lfs_attr *attrs;

    // Number of custom attributes in the list
    lfs_size_t attr_count;
};


/// internal littlefs data structures ///
typedef struct lfs_cache {
    lfs_block_t block;
    lfs_size_t off;
    lfs_size_t size;
    uint8_t *buffer;
} lfs_cache_t;

// TODO do we get ram savings with a lfsr_rorbyd_t substruct? need to measure
typedef struct lfsr_rbyd {
    // note this lines up with weight in lfsr_btree_t
    lfsr_srid_t weight;
    lfs_block_t blocks[2];
    // eoff=0, trunk=0  => not yet committed
    // eoff=0, trunk>0  => not yet fetched
    // eoff>=block_size => rbyd not erased/needs compaction
    lfs_size_t trunk;
    lfs_size_t eoff;
    uint32_t cksum;
} lfsr_rbyd_t;

typedef lfsr_rbyd_t lfsr_btree_t;

typedef struct lfsr_mptr {
    lfs_block_t blocks[2];
} lfsr_mptr_t;

typedef struct lfsr_mdir {
    lfsr_smid_t mid;
    lfsr_rbyd_t rbyd;
} lfsr_mdir_t;

typedef struct lfsr_openedmdir {
    struct lfsr_openedmdir *next;
    lfsr_mdir_t mdir;
} lfsr_openedmdir_t;

// space for:
// - type   - 1 leb128 - 1 byte (worst case)
// - 2 mids - 2 leb128 - 10 bytes (worst case)
#define LFSR_GRM_DSIZE (1+5+5)

typedef struct lfsr_grm {
    lfsr_smid_t rms[2];
} lfsr_grm_t;


typedef struct lfs_mdir {
    lfs_block_t pair[2];
    uint32_t rev;
    lfs_off_t off;
    uint32_t etag;
    uint16_t count;
    bool erased;
    bool split;
    lfs_block_t tail[2];
} lfs_mdir_t;

// Either an on-disk or in-device data pointer
//
// The sign-bit of the size field indicates if the data is
// in-device or on-disk.
//
// After removing the sign bit, the size always encodes the
// resulting size on-disk.
//
// The exact representation of in-device data also depends on the
// mode field:
// - pointer to a RAM-backed buffer
// - inlined data able to fit at least 1 leb128
// - an array of concatenated datas
//
// Note concatenated datas can only be 1 level deep. Concatenating
// concatenated datas would require recursion to resolve.
//
typedef struct lfsr_data {
    union {
        lfs_ssize_t size;
        struct {
            lfs_ssize_t size;
            lfs_block_t block;
            lfs_size_t off;
        } disk;
        struct {
            lfs_ssize_t size;
            uint8_t mode;
            const uint8_t *buffer;
        } buf;
        struct {
            lfs_ssize_t size;
            uint8_t mode;
            uint8_t buf[5];
        } imm;
        struct {
            lfs_ssize_t size;
            uint8_t mode;
            uint8_t count;
            const struct lfsr_data *datas;
        } cat;
    } u;
} lfsr_data_t;

typedef struct lfsr_bptr {
    // note data.size lines up with weight in lfsr_btree_t
    lfsr_data_t data;
    lfs_size_t cksize;
    uint32_t cksum;
} lfsr_bptr_t;

// erased-state checksum
typedef struct lfsr_ecksum {
    // size=-1 indicates no ecksum
    lfs_ssize_t size;
    uint32_t cksum;
} lfsr_ecksum_t;

// littlefs directory type
typedef struct lfs_dir {
    struct lfs_dir *next;
    uint16_t id;
    uint8_t type;
    lfs_mdir_t m;

    lfs_off_t pos;
    lfs_block_t head[2];
} lfs_dir_t;

typedef struct lfsr_dir {
    lfsr_openedmdir_t *next;
    lfsr_mdir_t mdir;
    lfsr_did_t did;
    lfsr_smid_t bookmark;
    lfs_soff_t pos;
} lfsr_dir_t;

// littlefs file type
typedef struct lfs_file {
    struct lfs_file *next;
    uint16_t id;
    uint8_t type;
    lfs_mdir_t m;

    struct lfs_ctz {
        lfs_block_t head;
        lfs_size_t size;
    } ctz;

    uint32_t flags;
    lfs_off_t pos;
    lfs_block_t block;
    lfs_off_t off;
    lfs_cache_t cache;

    const struct lfs_file_config *cfg;
} lfs_file_t;

// bsprouts must always be associated with an mdir
typedef struct lfsr_bsprout {
    lfsr_data_t data;
    // copy for staging
    lfsr_data_t data_;
} lfsr_bsprout_t;

// a bleaf is just a bptr with all optional attrs
typedef struct lfsr_bleaf {
    lfsr_bptr_t bptr;
    lfsr_ecksum_t becksum;
} lfsr_bleaf_t;

// bshrubs must always be associated with an mdir
//
// rbyd.block == mdir.blocks[0] => bshrub
// rbyd.block != mdir.blocks[0] => btree
typedef struct lfsr_bshrub {
    lfsr_rbyd_t rbyd;
    // copy for staging
    lfsr_rbyd_t rbyd_;
    // an upper-bound estimate on the on-disk shrub size
    lfs_size_t estimate;
} lfsr_bshrub_t;

// the lfsr_ftree_t struct is a sort of proto-file
typedef struct lfsr_ftree {
    lfsr_openedmdir_t *next;
    lfsr_mdir_t mdir;
    union {
        lfs_soff_t size;
        lfsr_data_t data;
        lfsr_bsprout_t bsprout;
        lfsr_bptr_t bptr;
        lfsr_bleaf_t bleaf;
        lfsr_bshrub_t bshrub;
        lfsr_btree_t btree;
    } u;
} lfsr_ftree_t;

typedef struct lfsr_file {
    lfsr_ftree_t ftree;
    uint32_t flags;
    lfs_off_t pos;
    lfs_off_t size;

    lfs_off_t buffer_pos;
    uint8_t *buffer;
    lfs_size_t buffer_size;

    const struct lfs_file_config *cfg;
} lfsr_file_t;

typedef struct lfs_superblock {
    uint32_t version;
    lfs_size_t block_size;
    lfs_size_t block_count;
    lfs_size_t name_max;
    lfs_size_t file_max;
    lfs_size_t attr_max;
} lfs_superblock_t;

typedef struct lfs_gstate {
    uint32_t tag;
    lfs_block_t pair[2];
} lfs_gstate_t;

typedef struct lfsr_mtree {
    union {
        // the sign bit indicates if this is an inlined mdir/direct mdir
        // pointer or a full mtree
        lfsr_smid_t weight;
        struct {
            lfsr_smid_t weight;
            lfsr_mptr_t mptr;
        } mptr;
        lfsr_btree_t btree;
    } u;
} lfsr_mtree_t;

// The littlefs filesystem type
typedef struct lfs {
    lfs_cache_t rcache;
    lfs_cache_t pcache;

    lfs_block_t root[2];
    struct lfs_mlist {
        struct lfs_mlist *next;
        uint16_t id;
        uint8_t type;
        lfs_mdir_t m;
    } *mlist;
    uint32_t seed;

    lfs_gstate_t gstate;
    lfs_gstate_t gdisk;
    lfs_gstate_t gdelta;

    struct lfs_lookahead {
        lfs_block_t start;
        lfs_block_t size;
        lfs_block_t next;
        lfs_block_t ckpoint;
        uint8_t *buffer;
    } lookahead;

    const struct lfs_config *cfg;
    lfs_size_t name_limit;
    lfs_off_t size_limit;
    lfs_size_t uattr_limit;
    lfs_size_t sattr_limit;

    // begin lfsr things
    lfsr_grm_t grm;
    uint8_t grm_g[LFSR_GRM_DSIZE];
    uint8_t grm_d[LFSR_GRM_DSIZE];

    // TODO we should put this flag somewhere, should lfs_t have a general
    // purpose flags field? this has been useful for lfsr_file_t
    bool hasorphans;

    uint8_t mbits;
    lfsr_mdir_t mroot;
    lfsr_mtree_t mtree;

    // linked-lists of opened mdirs, we keep a separate linked-list
    // for each type since these need to be handled a bit differently
    lfsr_openedmdir_t *opened[3];

#ifdef LFS_MIGRATE
    struct lfs1 *lfs1;
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
int lfs_format(lfs_t *lfs, const struct lfs_config *config);
int lfsr_format(lfs_t *lfs, const struct lfs_config *config);
#endif

// Mounts a littlefs
//
// Requires a littlefs object and config struct. Multiple filesystems
// may be mounted simultaneously with multiple littlefs objects. Both
// lfs and config must be allocated while mounted. The config struct must
// be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int lfs_mount(lfs_t *lfs, const struct lfs_config *config);
int lfsr_mount(lfs_t *lfs, const struct lfs_config *config);

// Unmounts a littlefs
//
// Does nothing besides releasing any allocated resources.
// Returns a negative error code on failure.
int lfs_unmount(lfs_t *lfs);
int lfsr_unmount(lfs_t *lfs);

/// General operations ///

#ifndef LFS_READONLY
// Removes a file or directory
//
// If removing a directory, the directory must be empty.
// Returns a negative error code on failure.
int lfs_remove(lfs_t *lfs, const char *path);
int lfsr_remove(lfs_t *lfs, const char *path);
#endif

#ifndef LFS_READONLY
// Rename or move a file or directory
//
// If the destination exists, it must match the source in type.
// If the destination is a directory, the directory must be empty.
//
// Returns a negative error code on failure.
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath);
int lfsr_rename(lfs_t *lfs, const char *old_path, const char *new_path);
#endif

// Find info about a file or directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a negative error code on failure.
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info);
int lfsr_stat(lfs_t *lfs, const char *path, struct lfs_info *info);

// Get a custom attribute
//
// Custom attributes are uniquely identified by an 8-bit type and limited
// to LFS_ATTR_MAX bytes. When read, if the stored attribute is smaller than
// the buffer, it will be padded with zeros. If the stored attribute is larger,
// then it will be silently truncated. If no attribute is found, the error
// LFS_ERR_NOATTR is returned and the buffer is filled with zeros.
//
// Returns the size of the attribute, or a negative error code on failure.
// Note, the returned size is the size of the attribute on disk, irrespective
// of the size of the buffer. This can be used to dynamically allocate a buffer
// or check for existence.
lfs_ssize_t lfs_getattr(lfs_t *lfs, const char *path,
        uint8_t type, void *buffer, lfs_size_t size);

#ifndef LFS_READONLY
// Set custom attributes
//
// Custom attributes are uniquely identified by an 8-bit type and limited
// to LFS_ATTR_MAX bytes. If an attribute is not found, it will be
// implicitly created.
//
// Returns a negative error code on failure.
int lfs_setattr(lfs_t *lfs, const char *path,
        uint8_t type, const void *buffer, lfs_size_t size);
#endif

#ifndef LFS_READONLY
// Removes a custom attribute
//
// If an attribute is not found, nothing happens.
//
// Returns a negative error code on failure.
int lfs_removeattr(lfs_t *lfs, const char *path, uint8_t type);
#endif


/// File operations ///

#ifndef LFS_NO_MALLOC
// Open a file
//
// The mode that the file is opened in is determined by the flags, which
// are values from the enum lfs_open_flags that are bitwise-ored together.
//
// Returns a negative error code on failure.
int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags);
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
int lfs_file_opencfg(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags,
        const struct lfs_file_config *config);
int lfsr_file_opencfg(lfs_t *lfs, lfsr_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs_file_config *config);

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
int lfs_file_close(lfs_t *lfs, lfs_file_t *file);
int lfsr_file_close(lfs_t *lfs, lfsr_file_t *file);

// Synchronize a file on storage
//
// Any pending writes are written out to storage and other open files.
//
// If the file was desynchronized, it is now marked as synchronized. It will
// now recieve file updates and syncs on close.
//
// Returns a negative error code on failure.
int lfs_file_sync(lfs_t *lfs, lfs_file_t *file);
int lfsr_file_sync(lfs_t *lfs, lfsr_file_t *file);

// Mark a file as desynchronized
//
// Desynchronized files do not recieve file updates and do not sync on close.
// They effectively act as snapshots of the underlying file at that point
// in time.
//
// If an error occurs during a write operation, the file is implicitly marked
// as desynchronized.
//
// An explicit and successful call to lfsr_file_sync reverses this, marking
// the file as synchronized again.
//
// Returns a negative error code on failure.
int lfsr_file_desync(lfs_t *lfs, lfsr_file_t *file);

// Read data from file
//
// Takes a buffer and size indicating where to store the read data.
// Returns the number of bytes read, or a negative error code on failure.
lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size);
lfs_ssize_t lfsr_file_read(lfs_t *lfs, lfsr_file_t *file,
        void *buffer, lfs_size_t size);

#ifndef LFS_READONLY
// Write data to file
//
// Takes a buffer and size indicating the data to write. The file will not
// actually be updated on the storage until either sync or close is called.
//
// If an error occurs during a write (including LFS_ERR_NOSPC), the file
// will be marked as desynchronized, the position will be left unchanged,
// and some, all, or none of the data may be written.
//
// Returns the number of bytes written, or a negative error code on failure.
lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size);
lfs_ssize_t lfsr_file_write(lfs_t *lfs, lfsr_file_t *file,
        const void *buffer, lfs_size_t size);
#endif

// Change the position of the file
//
// The change in position is determined by the offset and whence flag.
// Returns the new position of the file, or a negative error code on failure.
lfs_soff_t lfs_file_seek(lfs_t *lfs, lfs_file_t *file,
        lfs_soff_t off, int whence);
lfs_soff_t lfsr_file_seek(lfs_t *lfs, lfsr_file_t *file,
        lfs_soff_t off, uint8_t whence);

#ifndef LFS_READONLY
// Truncate/grow the size of the file to the specified size
//
// If size is larger than the current file size, a hole is created, appearing
// as if the file was filled with zeros.
//
// Returns a negative error code on failure.
int lfs_file_truncate(lfs_t *lfs, lfs_file_t *file, lfs_off_t size);
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
lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file);
lfs_soff_t lfsr_file_tell(lfs_t *lfs, lfsr_file_t *file);

// Change the position of the file to the beginning of the file
//
// Equivalent to lfs_file_seek(lfs, file, 0, LFS_SEEK_SET)
// Returns a negative error code on failure.
int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file);
int lfsr_file_rewind(lfs_t *lfs, lfsr_file_t *file);

// Return the size of the file
//
// Similar to lfs_file_seek(lfs, file, 0, LFS_SEEK_END)
// Returns the size of the file, or a negative error code on failure.
lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file);
lfs_soff_t lfsr_file_size(lfs_t *lfs, lfsr_file_t *file);


/// Directory operations ///

#ifndef LFS_READONLY
// Create a directory
//
// Returns a negative error code on failure.
int lfs_mkdir(lfs_t *lfs, const char *path);
int lfsr_mkdir(lfs_t *lfs, const char *path);
#endif

// Open a directory
//
// Once open a directory can be used with read to iterate over files.
// Returns a negative error code on failure.
int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path);
int lfsr_dir_open(lfs_t *lfs, lfsr_dir_t *dir, const char *path);

// Close a directory
//
// Releases any allocated resources.
// Returns a negative error code on failure.
int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir);
int lfsr_dir_close(lfs_t *lfs, lfsr_dir_t *dir);

// Read an entry in the directory
//
// Fills out the info structure, based on the specified file or directory.
// Returns a positive value on success, 0 at the end of directory,
// or a negative error code on failure.
int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info);
int lfsr_dir_read(lfs_t *lfs, lfsr_dir_t *dir, struct lfs_info *info);

// Change the position of the directory
//
// The new off must be a value previous returned from tell and specifies
// an absolute offset in the directory seek.
//
// Returns a negative error code on failure.
int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off);
int lfsr_dir_seek(lfs_t *lfs, lfsr_dir_t *dir, lfs_soff_t off);

// Return the position of the directory
//
// The returned offset is only meant to be consumed by seek and may not make
// sense, but does indicate the current position in the directory iteration.
//
// Returns the position of the directory, or a negative error code on failure.
lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir);
lfs_soff_t lfsr_dir_tell(lfs_t *lfs, lfsr_dir_t *dir);

// Change the position of the directory to the beginning of the directory
//
// Returns a negative error code on failure.
int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir);
int lfsr_dir_rewind(lfs_t *lfs, lfsr_dir_t *dir);


/// Filesystem-level filesystem operations

// Finds the current size of the filesystem
//
// Note: Result is best effort. If files share COW structures, the returned
// size may be larger than the filesystem actually is.
//
// Returns the number of allocated blocks, or a negative error code on failure.
lfs_ssize_t lfs_fs_size(lfs_t *lfs);
lfs_ssize_t lfsr_fs_size(lfs_t *lfs);

// Traverse through all blocks in use by the filesystem
//
// The provided callback will be called with each block address that is
// currently in use by the filesystem. This can be used to determine which
// blocks are in use or how much of the storage is available.
//
// Returns a negative error code on failure.
int lfs_fs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data);

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
int lfs_migrate(lfs_t *lfs, const struct lfs_config *cfg);
#endif
#endif


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
