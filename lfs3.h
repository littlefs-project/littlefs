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
typedef int32_t  lfs3_sblock_t;

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

// Maximum name size in bytes, may be redefined to reduce the size of
// the info struct. Limited to <= block_size/8. Stored on-disk and must
// be respected by other littlefs drivers.
#ifndef LFS3_NAME_MAX
#define LFS3_NAME_MAX 255
#endif

// Maximum size of a file in bytes, may be redefined to limit to support
// other drivers. Limited on disk to <= (2^31)-1. Stored on-disk and
// must be respected by other littlefs drivers.
#ifndef LFS3_FILE_MAX
#define LFS3_FILE_MAX 2147483647
#endif


// Possible error codes, these are negative to allow
// valid positive return values
enum lfs3_err {
    // common errors
    LFS3_ERR_OK          = 0,    // No error
    LFS3_ERR_UNKNOWN     = -1,   // Unknown error
    LFS3_ERR_INVAL       = -22,  // Invalid parameter
    LFS3_ERR_NOTSUP      = -95,  // Operation not supported
    LFS3_ERR_BUSY        = -16,  // Device or resource busy
    LFS3_ERR_NOMEM       = -12,  // No more memory available

    // bd errors
    LFS3_ERR_IO          = -5,   // Error during device operation
    LFS3_ERR_DAMAGED     = -83,  // Data is ok, but needs repair
    LFS3_ERR_CORRUPT     = -84,  // Data is corrupt

    // filesystem errors
    LFS3_ERR_NOENT       = -2,   // No directory entry
    LFS3_ERR_EXIST       = -17,  // Entry already exists
    LFS3_ERR_NOTDIR      = -20,  // Entry is not a dir
    LFS3_ERR_ISDIR       = -21,  // Entry is a dir
    LFS3_ERR_NOTEMPTY    = -39,  // Dir is not empty
    LFS3_ERR_NOATTR      = -61,  // No data/attr available
    LFS3_ERR_NAMETOOLONG = -36,  // File name too long
    LFS3_ERR_FBIG        = -27,  // File too large
    LFS3_ERR_RANGE       = -34,  // Result out of range
    LFS3_ERR_NOSPC       = -28,  // No space left on device
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
    LFS3_TYPE_UNKNOWN    = 8,  // Unknown file type

    // internally used types, don't use these
    LFS3_type_BOOKMARK   = 4,  // Directory bookmark
    LFS3_type_ORPHAN     = 5,  // An orphaned stickynote
    LFS3_type_TRV        = 6,  // An open traversal object
    LFS3_type_GC         = 7,  // An open gc object
};

// File open flags
#define LFS3_O_MODE              3  // The file's access mode
#define LFS3_O_RDONLY            1  // Open a file as read only
#ifndef LFS3_RDONLY
#define LFS3_O_WRONLY            2  // Open a file as write only
#endif
#ifndef LFS3_RDONLY
#define LFS3_O_RDWR              3  // Open a file as read and write
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
#define LFS3_O_GRANULAR 0x00000100  // Only write grains
#define LFS3_O_DESYNC   0x02000000  // Do not sync or recieve file updates
#define LFS3_O_CKMETA   0x00100000  // Check metadata checksums
#define LFS3_O_CKDATA   0x00200000  // Check metadata + data checksums
#define LFS3_O_CK       0x00300000  // Alias for CKMETA + CKDATA
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_O_REPAIRMETA \
                        0x00400000  // Repair metadata damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_O_REPAIRDATA \
                        0x00800000  // Repair metadata + data damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_O_REPAIR   0x00800000  // Alias for REPAIRMETA + REPAIRDATA
#endif

// internally used flags, don't use these
#define LFS3_o_SET      0x00008000  // Atomically write file
#define LFS3_o_TYPE     0xf0000000  // The file's type
#define LFS3_o_ZOMBIE   0x08000000  // File has been removed
#define LFS3_o_UNCREAT  0x04000000  // File does not exist yet
#define LFS3_o_UNSYNC   0x01000000  // File's metadata does not match disk
#define LFS3_o_UNCRYST  0x00040000  // File's leaf not fully crystallized
#define LFS3_o_UNGRAFT  0x00020000  // File's leaf does not match disk
#define LFS3_o_UNFLUSH  0x00010000  // File's cache does not match disk

// Additional file config flags
#define LFS3_FILECFG_FLUSH \
                        0x00000040  // Flush data on every write
#define LFS3_FILECFG_SYNC \
                        0x00000080  // Sync metadata on every write

// File seek flags
#define LFS3_SEEK_SET 0  // Seek relative to an absolute position
#define LFS3_SEEK_CUR 1  // Seek relative to the current file position
#define LFS3_SEEK_END 2  // Seek relative to the end of the file

// Custom attribute flags
#define LFS3_A_MODE              3  // The attr's access mode
#define LFS3_A_RDONLY            1  // Open an attr as read only
#ifndef LFS3_RDONLY
#define LFS3_A_WRONLY            2  // Open an attr as write only
#endif
#ifndef LFS3_RDONLY
#define LFS3_A_RDWR              3  // Open an attr as read and write
#endif
#define LFS3_A_LAZY           0x04  // Only write attr if file changed

// File/filesystem check flags
#define LFS3_CK_MTREEONLY \
                        0x00000004  // Only traverse the mtree
#define LFS3_CK_CKMETA  0x00100000  // Check metadata checksums
#define LFS3_CK_CKDATA  0x00200000  // Check metadata + data checksums
#define LFS3_CK_CK      0x00300000  // Alias for CKMETA + CKDATA

// File/filesystem repair flags
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_MTREEONLY \
                        0x00000004  // Only traverse the mtree
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_CKMETA \
                        0x00100000  // Check metadata checksums
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_CKDATA \
                        0x00200000  // Check metadata + data checksums
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_CK  0x00300000  // Alias for CKMETA + CKDATA
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_REPAIRMETA \
                        0x00400000  // Repair metadata damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_REPAIRDATA \
                        0x00800000  // Repair metadata + data damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_REPAIR_REPAIR \
                        0x00c00000  // Alias for REPAIRMETA + REPAIRDATA
#endif

// Filesystem format flags
#ifndef LFS3_RDONLY
#define LFS3_F_MODE              1  // Format's access mode
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_RDWR              0  // Format the filesystem as read and write
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
#define LFS3_F_GBMAP    0x00000008  // Use the global on-disk block-map
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_MKCONSISTENT \
                        0x00010000  // Make the filesystem consistent
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_LOOKAHEAD \
                        0x00020000  // Repopulate lookahead/gbmap
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
#define LFS3_F_PREERASE 0x00040000  // Try to pre-erase free blocks
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_COMPACTMETA \
                        0x00080000  // Compact metadata logs
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_COMPACT  0x00080000  // Alias for COMPACTMETA
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_CKMETA   0x00100000  // Check metadata checksums
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_CKDATA   0x00200000  // Check metadata + data checksums
#endif
#ifndef LFS3_RDONLY
#define LFS3_F_CK       0x00300000  // Alias for CKMETA + CKDATA
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_F_REPAIRMETA \
                        0x00400000  // Repair metadata damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_F_REPAIRDATA \
                        0x00800000  // Repair metadata + data damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_F_REPAIR   0x00c00000  // Alias for REPAIRMETA + REPAIRDATA
#endif

// an alias for all gc work
#define LFS3_F_GC ( \
        LFS3_IFDEF_RDONLY(0, LFS3_F_MKCONSISTENT) \
            | LFS3_IFDEF_RDONLY(0, LFS3_F_LOOKAHEAD) \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_PREERASE(LFS3_F_PREERASE, 0)) \
            | LFS3_IFDEF_RDONLY(0, LFS3_F_COMPACTMETA) \
            | LFS3_F_CKMETA \
            | LFS3_F_CKDATA \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_REPAIR(LFS3_F_REPAIRMETA, 0)) \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_REPAIR(LFS3_F_REPAIRDATA, 0)))

// Filesystem mount flags
#define LFS3_M_MODE              1  // Mount's access mode
#ifndef LFS3_RDONLY
#define LFS3_M_RDWR              0  // Mount the filesystem as read and write
#endif
#define LFS3_M_RDONLY            1  // Mount the filesystem as read only
#define LFS3_M_FLUSH    0x00000040  // Open all files with LFS3_O_FLUSH
#define LFS3_M_SYNC     0x00000080  // Open all files with LFS3_O_SYNC
#define LFS3_M_GRANULAR 0x00000100  // Open all files with LFS3_O_GRANULAR
#ifndef LFS3_RDONLY
#define LFS3_M_MKCONSISTENT \
                        0x00010000  // Make the filesystem consistent
#endif
#ifndef LFS3_RDONLY
#define LFS3_M_LOOKAHEAD \
                        0x00020000  // Repopulate lookahead/gbmap
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
#define LFS3_M_PREERASE 0x00040000  // Try to pre-erase free blocks
#endif
#ifndef LFS3_RDONLY
#define LFS3_M_COMPACTMETA \
                        0x00080000  // Compact metadata logs
#endif
#ifndef LFS3_RDONLY
#define LFS3_M_COMPACT  0x00080000  // Alias for COMPACTMETA
#endif
#define LFS3_M_CKMETA   0x00100000  // Check metadata checksums
#define LFS3_M_CKDATA   0x00200000  // Check metadata + data checksums
#define LFS3_M_CK       0x00300000  // Alias for CKMETA + CKDATA
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_M_REPAIRMETA \
                        0x00400000  // Repair metadata damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_M_REPAIRDATA \
                        0x00800000  // Repair metadata + data damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_M_REPAIR   0x00800000  // Alias for REPAIRMETA + REPAIRDATA
#endif

// an alias for all gc work
#define LFS3_M_GC ( \
        LFS3_IFDEF_RDONLY(0, LFS3_M_MKCONSISTENT) \
            | LFS3_IFDEF_RDONLY(0, LFS3_M_LOOKAHEAD) \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_PREERASE(LFS3_M_PREERASE, 0)) \
            | LFS3_IFDEF_RDONLY(0, LFS3_M_COMPACTMETA) \
            | LFS3_M_CKMETA \
            | LFS3_M_CKDATA \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_REPAIR(LFS3_M_REPAIRMETA, 0)) \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_REPAIR(LFS3_M_REPAIRDATA, 0)))

// Additional filesystem config flags
#define LFS3_CFG_MODE            1  // Filesystem's access mode
#ifndef LFS3_RDONLY
#define LFS3_CFG_RDWR            0  // Mount the filesystem as read and write
#endif
#define LFS3_CFG_RDONLY          1  // Mount the filesystem as read only
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
#define LFS3_CFG_GBMAP  0x00000008  // Use the global on-disk block-map
#endif
#define LFS3_CFG_FLUSH  0x00000040  // Open all files with LFS3_O_FLUSH
#define LFS3_CFG_SYNC   0x00000080  // Open all files with LFS3_O_SYNC
#define LFS3_CFG_GRANULAR \
                        0x00000100  // Open all files with LFS3_O_GRANULAR
#if !defined(LFS3_RDONLY) && defined(LFS3_REVPERTURB)
#define LFS3_CFG_REVPERTURB \
                        0x00010000  // Perturb first bit in revision counts
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REVNOISE)
#define LFS3_CFG_REVNOISE \
                        0x00020000  // Add noise to revision counts
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_CKPROGS)
#define LFS3_CFG_CKPROGS \
                        0x00100000  // Check progs by reading back progged data
#endif
#ifdef LFS3_CKFETCHES
#define LFS3_CFG_CKFETCHES \
                        0x00200000  // Check block checksums before first use
#endif
#ifdef LFS3_CKMETAPARITY
#define LFS3_CFG_CKMETAPARITY \
                        0x00400000  // Check metadata tag parity bits
#endif
#ifdef LFS3_CKDATACKSUMS
#define LFS3_CFG_CKDATACKSUMS \
                        0x01000000  // Check data checksums on reads
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_CFG_REPAIRMETADAMAGE \
                        0x10000000  // Repair metadata damage when found
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_CFG_REPAIRDATADAMAGE \
                        0x20000000  // Repair metadata + data damage when found
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_CFG_REPAIRDAMAGE \
                        0x30000000  // Alias for REPAIRMETADAMAGE + DATADAMAGE
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_CONDEMN)
#define LFS3_CFG_CONDEMNDAMAGE \
                        0x40000000  // Mark any damaged blocks as bad
#endif

// Filesystem info flags
#define LFS3_I_RDONLY   0x00000001  // Mounted read only
#ifdef LFS3_GBMAP
#define LFS3_I_GBMAP    0x00000008  // Global on-disk block-map in use
#endif
#define LFS3_I_FLUSH    0x00000040  // Mounted with LFS3_M_FLUSH
#define LFS3_I_SYNC     0x00000080  // Mounted with LFS3_M_SYNC
#define LFS3_I_GRANULAR 0x00000100  // Mounted with LFS3_M_GRANULAR
#ifndef LFS3_RDONLY
#define LFS3_I_MKCONSISTENT \
                        0x00010000  // Filesystem needs mkconsistent to write
#endif
#ifndef LFS3_RDONLY
#define LFS3_I_LOOKAHEAD \
                        0x00020000  // Lookahead/gbmap is not full
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
#define LFS3_I_PREERASE 0x00040000  // Blocks can be pre-erased
#endif
#ifndef LFS3_RDONLY
#define LFS3_I_COMPACTMETA \
                        0x00080000  // Filesystem may have uncompacted metadata
#endif
#define LFS3_I_CKMETA   0x00100000  // Metadata checksums not checked recently
#define LFS3_I_CKDATA   0x00200000  // Data checksums not checked recently
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_I_REPAIRMETA \
                        0x00400000  // Metadata blocks need repair
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_I_REPAIRDATA \
                        0x00800000  // Data blocks need repair
#endif
#ifndef LFS3_RDONLY
#define LFS3_I_GRMOVERFLOW \
                        0x08000000  // Global remove queue overflowed
#endif
#ifndef LFS3_RDONLY
#define LFS3_I_DAMAGEDPROG \
                        0x10000000  // Found damage during prog
#endif
#ifdef LFS3_REPAIR
#define LFS3_I_DAMAGEDREAD \
                        0x20000000  // Found damage during read
#endif
#ifdef LFS3_CONDEMN
#define LFS3_I_CONDEMNED \
                        0x40000000  // Found condemned blocks
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_I_EVICTOVERFLOW \
                        0x80000000  // Evict queue overflowed
#endif

// internally used flags, don't use these
#if !defined(LFS3_RDONLY) && defined(LFS3_SHRINK)
#define LFS3_i_SHRINKING \
                        0x04000000  // Filesystem is being shrunk
#endif

// Block types
enum lfs3_btype {
    LFS3_BTYPE_MDIR    = 1, // An mdir (metadata log)
    LFS3_BTYPE_BTREE   = 2, // A btree node
    LFS3_BTYPE_DATA    = 3, // A raw data block

    LFS3_BTYPE_FREE    = 4, // Known free, safe to alloc
    LFS3_BTYPE_INUSE   = 5, // Known in-use, type unknown
    #ifdef LFS3_GBMAP
    LFS3_BTYPE_ERASED  = 6, // Known erased, probably (requires ecksum proof)
    LFS3_BTYPE_BAD     = 7, // Known bad, do not alloc
    #endif
    LFS3_BTYPE_UNKNOWN = 8, // Unknown block status
};

// Traversal flags
#define LFS3_T_MTREEONLY \
                        0x00000004  // Only traverse the mtree
#define LFS3_T_EXCL     0x00000008  // Error if filesystem modified
#define LFS3_T_CKMETA   0x00100000  // Check metadata checksums
#define LFS3_T_CKDATA   0x00200000  // Check metadata + data checksums
#define LFS3_T_CK       0x00300000  // Alias for CKMETA + CKDATA

// internally used flags, don't use these
#define LFS3_t_TYPE     0xf0000000  // The traversal's type
#define LFS3_t_BTYPE    0x000000f0  // The current block type
#define LFS3_t_CKPOINTED \
                        0x08000000  // Filesystem ckpointed during traversal
#define LFS3_t_DIRTY    0x04000000  // Filesystem ckpointed outside traversal
#define LFS3_t_STALE    0x02000000  // Block queue probably out-of-date
#define LFS3_t_DAMAGED  0x01000000  // Filesystem damaged during traversal

// GC traversal flags - only used in lfs3_gc_open
#define LFS3_GC_EXCL    0x00000008  // Error if filesystem modified

// GC flags
#ifndef LFS3_RDONLY
#define LFS3_GC_MKCONSISTENT \
                        0x00010000  // Make the filesystem consistent
#endif
#ifndef LFS3_RDONLY
#define LFS3_GC_LOOKAHEAD \
                        0x00020000  // Repopulate lookahead/gbmap
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
#define LFS3_GC_PREERASE \
                        0x00040000  // Try to pre-erase free blocks
#endif
#ifndef LFS3_RDONLY
#define LFS3_GC_COMPACTMETA \
                        0x00080000  // Compact metadata logs
#endif
#ifndef LFS3_RDONLY
#define LFS3_GC_COMPACT 0x00080000  // Alias for COMPACTMETA
#endif
#define LFS3_GC_CKMETA  0x00100000  // Check metadata checksums
#define LFS3_GC_CKDATA  0x00200000  // Check metadata + data checksums
#define LFS3_GC_CK      0x00300000  // Alias for CKMETA + CKDATA
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_GC_REPAIRMETA \
                        0x00400000  // Repair metadata damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_GC_REPAIRDATA \
                        0x00800000  // Repair metadata + data damage
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
#define LFS3_GC_REPAIR  0x00c00000  // Alias for REPAIRMETA + REPAIRDATA
#endif

// internally used flags, don't use these
#ifndef LFS3_RDONLY
#define LFS3_gc_MKCONSISTENTING \
                        0x00000100  // Working on LFS3_GC_MKCONSISTENT
#endif
#ifndef LFS3_RDONLY
#define LFS3_gc_LOOKAHEADING \
                        0x00000200  // Working on LFS3_GC_LOOKAHEAD
#endif
#ifndef LFS3_RDONLY
#define LFS3_gc_COMPACTMETAING \
                        0x00000800  // Working on LFS3_GC_COMPACTMETA
#endif
#define LFS3_gc_CKMETAING \
                        0x00001000  // Working on LFS3_GC_CKMETA
#define LFS3_gc_CKDATAING \
                        0x00002000  // Working on LFS3_GC_CKDATA
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_gc_EVICTMETAING \
                        0x00004000  // Working on LFS3_gc_EVICTMETA
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_gc_EVICTDATAING \
                        0x00008000  // Working on LFS3_gc_EVICTDATA
#endif

// an alias for all gc work
#define LFS3_GC_GC ( \
        LFS3_IFDEF_RDONLY(0, LFS3_GC_MKCONSISTENT) \
            | LFS3_IFDEF_RDONLY(0, LFS3_GC_LOOKAHEAD) \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_PREERASE(LFS3_GC_PREERASE, 0)) \
            | LFS3_IFDEF_RDONLY(0, LFS3_GC_COMPACTMETA) \
            | LFS3_GC_CKMETA \
            | LFS3_GC_CKDATA \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_REPAIR(LFS3_M_REPAIRMETA, 0)) \
            | LFS3_IFDEF_RDONLY(0, LFS3_IFDEF_REPAIR(LFS3_M_REPAIRDATA, 0)))

// Filesystem grow flags
#ifndef LFS3_RDONLY
#define LFS3_GROW_GROW  0x00000001  // Potentially grow the filesystem
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_SHRINK)
#define LFS3_GROW_SHRINK \
                        0x00000002  // Potentially shrink the filesystem
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_SHRINK)
#define LFS3_GROW_EVICT 0x00000004  // Evict blocks needed to shrink
#endif

// Block eviction flags
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_EVICT_EVICT \
                        0x00000004  // Delete all references to this block
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
#define LFS3_EVICT_BAD  0x80000000  // Mark this block as bad, do not alloc
#endif
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
#define LFS3_EVICT_GOOD 0x20000000  // Mark this block as good, do alloc
#endif

// internally used flags, don't use these
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
#define LFS3_evict_DATA 0x40000000  // Block is definitely data
#endif


// Configuration provided during initialization of the littlefs
struct lfs3_cfg {
    // Additional filesystem config flags. Bitwise-ored with
    // mount/format flags when relevant.
    uint32_t flags;

    // Opaque user provided context that can be used to pass information
    // to the block device operations
    void *context;

    // Read a region in a block. Negative error codes are propagated to
    // the user.
    //
    // May return LFS3_ERR_CORRUPT if the block is unreadable, or
    // LFS3_ERR_DAMAGED if the block is readable but needs repairs.
    int (*read)(const struct lfs3_cfg *c, lfs3_block_t block,
            lfs3_off_t off, void *buffer, lfs3_size_t size);

    // Program a region in a block. The block must have previously been
    // erased. Negative error codes are propagated to the user.
    //
    // May return LFS3_ERR_CORRUPT or LFS3_ERR_DAMAGED if the block will
    // be unreadable.
    #ifndef LFS3_RDONLY
    int (*prog)(const struct lfs3_cfg *c, lfs3_block_t block,
            lfs3_off_t off, const void *buffer, lfs3_size_t size);
    #endif

    // Erase a block. A block must be erased before being programmed.
    // The state of an erased block is undefined. Negative error codes
    // are propagated to the user.
    //
    // May return LFS3_ERR_CORRUPT or LFS3_ERR_DAMAGED if the block will
    // be unreadable.
    #ifndef LFS3_RDONLY
    int (*erase)(const struct lfs3_cfg *c, lfs3_block_t block);
    #endif

    // Sync the state of the underlying block device. Negative error
    // codes are propagated to the user.
    #ifndef LFS3_RDONLY
    int (*sync)(const struct lfs3_cfg *c);
    #endif

    // Lock the underlying block device. Negative error codes are
    // propagated to the user.
    #ifdef LFS3_THREADSAFE
    int (*lock)(const struct lfs3_cfg *c);
    #endif

    // Unlock the underlying block device. Negative error codes are
    // propagated to the user.
    #ifdef LFS3_THREADSAFE
    int (*unlock)(const struct lfs3_cfg *c);
    #endif

    // Minimum size of a read in bytes. All read operations will be a
    // multiple (>=) of this value.
    lfs3_size_t read_size;

    // Minimum size of a program in bytes. All program operations will
    // be a multiple (>=) of this value.
    #ifndef LFS3_RDONLY
    lfs3_size_t prog_size;
    #endif

    // Size of an erasable block in bytes. This does not impact RAM
    // consumption and may be larger than the physical erase size. Must
    // be a multiple (>=) of the read and program sizes.
    lfs3_size_t block_size;

    // Number of erasable blocks on the device.
    lfs3_block_t block_count;

    // Number of erase cycles before metadata blocks are relocated for
    // wear-leveling.
    //
    // Suggested values are in the range 16-1024. Larger values relocate
    // less frequently, improving average performance, at the cost of
    // worse wear distribution. Note this ends up rounded down to a
    // power-of-2.
    //
    // 0 results in pure copy-on-write, which may be counter-productive
    // due to write amplification. Set to -1 to disable block-level
    // wear-leveling.
    #ifndef LFS3_RDONLY
    int32_t block_recycles;
    #endif

    // Size of the read cache in bytes. Larger caches can improve
    // performance by storing more data and reducing the number of disk
    // accesses. Must be a multiple (>=) of the read size.
    lfs3_size_t rcache_size;

    // Size of the program cache in bytes. Larger caches can improve
    // performance by storing more data and reducing the number of disk
    // accesses. Must be a multiple (>=) of the program size.
    #ifndef LFS3_RDONLY
    lfs3_size_t pcache_size;
    #endif

    // Size of file caches in bytes. In addition to filesystem-wide
    // read/prog caches, each file gets its own cache to reduce disk
    // accesses.
    lfs3_size_t fcache_size;

    // Size of the lookahead buffer in bytes. A larger lookahead buffer
    // increases the number of blocks found during an allocation scan.
    // The lookahead buffer is stored as a compact bitmap, so each byte
    // of RAM can track 8 blocks.
    #ifndef LFS3_RDONLY
    lfs3_size_t lookahead_size;
    #endif

    // Threshold for repopulating the global on-disk block-map (gbmap).
    //
    // When <= this many blocks have a known state, littlefs will
    // traverse the filesystem and attempt to repopulate the gbmap.
    // Smaller values decrease repopulation frequency and improves
    // overall allocator throughput, at the risk of needing to fallback
    // to the slower lookahead allocator when empty.
    //
    // 0 only repopulates the gbmap when empty, minimizing gbmap
    // repopulations at the risk of large latency spikes.
    #ifdef LFS3_GBMAP
    lfs3_block_t lookgbmap_thresh;
    #endif

    // Size of the optional evict queue in lfs3_evict_t structs. A
    // larger evict queue can track more evicted/damaged blocks during
    // evictions/repairs/read-only operations. If the evict queue
    // overflows, damaged blocks are quietly forgotten until a
    // successful repair.
    //
    // A suggested value is 2. Finding damage is normally a rare event.
    #if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
    lfs3_size_t evictqueue_count;
    #endif

    // Flags indicating what gc work to do during lfs3_fs_gc calls.
    #ifdef LFS3_GC
    uint32_t gc_flags;
    #endif

    // Number of gc steps to perform in each call to lfs3_fs_gc, with
    // each step representing ~1 block of work. More steps per call will
    // make more progress if interleaved with other filesystem
    // operations, but may also introduce more latency.
    //
    // steps=1 or 0 will do the minimum amount of work to make progress,
    // and steps=-1 will not return until all pending janitorial work
    // has been completed. Defaults to steps=1 when zero.
    #ifdef LFS3_GC
    lfs3_sblock_t gc_steps;
    #endif

    // Threshold for repopulating the lookahead buffer during gc. This
    // can be set lower than the lookahead size to delay gc work when
    // only a few blocks have been allocated.
    //
    // Note this only affects explicit gc operations. During normal
    // operations the lookahead buffer is only repopulated when empty.
    //
    // 0 only repopulates the lookahead buffer when empty, while -1 or
    // any value >= 8*lookahead_size-1 repopulates the lookahead buffer
    // after any block allocation.
    #ifndef LFS3_RDONLY
    lfs3_block_t gc_lookahead_thresh;
    #endif

    // Threshold for repopulating the gbmap during gc. This can be set
    // lower than the disk size to delay gc work when only a few blocks
    // have been allocated.
    //
    // Note this only affects explicit gc operations. During normal
    // operations gbmap repopulations are controlled by
    // lookgbmap_thresh.
    //
    // 0 or any value <= lookgbmap_thresh repopulates the gbmap when
    // below lookgbmap_thresh, while -1 or any value >= block_count-1
    // repopulates the lookahead buffer after any block allocation.
    #if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
    lfs3_block_t gc_lookgbmap_thresh;
    #endif

    // Number of blocks to try to pre-erase during gc. This can help
    // reduce the latency of block allocation when erasing is expensive.
    //
    // Requires the gbmap to track pre-erased blocks.
    //
    // 0 disables pre-erasing, while -1 or any value >= block_count
    // attempts to pre-erase all known free blocks during gc. When
    // disabled, littlefs erases blocks immediately before the first
    // prog operation.
    #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
    lfs3_block_t gc_preerase_count;
    #endif

    // Threshold for metadata compaction during gc in bytes. littlefs
    // will attempt to compact metadata logs that exceed this threshold
    // during gc operations.
    //
    // Note this only affects explicit gc operations. During normal
    // operations metadata is only compacted when full.
    //
    // Suggested values are around ~88% block_size (bs-bs/8).
    //
    // Set to -1 to disable metadata compaction during gc. Must be
    // >= block_size/2 to avoid balance issues. 0 is reserved.
    #ifndef LFS3_RDONLY
    lfs3_size_t gc_compactmeta_thresh;
    #endif

    // Threshold for btree node compaction during gc in bytes. littlefs
    // will attempt to compact btree nodes that exceed this threshold
    // during gc operations.
    //
    // This allows a separate compaction threshold for btree nodes,
    // which are usually less critical and more susceptible to write
    // amplification than mdirs.
    //
    // Note this only affects explicit gc operations. During normal
    // operations metadata is only compacted when full.
    //
    // Set to -1 to disable btree compaction during gc. Must be
    // >= block_size/2 to avoid balance issues. Defaults to
    // gc_compactmeta_thresh when zero.
    #ifndef LFS3_RDONLY
    lfs3_size_t gc_compactbtree_thresh;
    #endif

    // Optional statically allocated rcache buffer. Must be rcache_size.
    // By default lfs3_malloc is used to allocate this buffer.
    void *rcache_buffer;

    // Optional statically allocated pcache buffer. Must be pcache_size.
    // By default lfs3_malloc is used to allocate this buffer.
    #ifndef LFS3_RDONLY
    void *pcache_buffer;
    #endif

    // Optional statically allocated lookahead buffer. Must be
    // lookahead_size. By default lfs3_malloc is used to allocate this
    // buffer.
    #ifndef LFS3_RDONLY
    void *lookahead_buffer;
    #endif

    // Optional statically allocated evict queue array. Must be
    // evictqueue_size. By default lfs3_malloc is used to allocate this
    // array.
    #if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
    struct lfs3_evict *evictqueue_array;
    #endif

    // Optional upper limit on length of file names in bytes. No
    // downside for larger names except the size of the info struct
    // which is controlled by LFS3_NAME_MAX.
    //
    // Defaults to LFS3_NAME_MAX when zero. Stored on-disk and must be
    // respected by other littlefs drivers.
    #ifndef LFS3_RDONLY
    lfs3_size_t name_limit;
    #endif

    // Optional upper limit on files in bytes. No downside for larger
    // files but must be <= LFS3_FILE_MAX.
    //
    // Defaults to LFS3_FILE_MAX when zero. Stored on-disk and must be
    // respected by other littlefs drivers.
    #ifndef LFS3_RDONLY
    lfs3_off_t file_limit;
    #endif

    // Maximum size of inlined B-tree roots (shrubs) in bytes. Shrubs
    // reduce B-tree overhead and improve write performance, but may
    // add pressure to metadata-related operations.
    //
    // Suggested values are around ~block_size/8.
    //
    // Must be <= block_size/8. -1 disables shrubs. 0 is reserved.
    #ifndef LFS3_RDONLY
    lfs3_size_t shrub_size;
    #endif

    // Maximum size of inlined B-tree leaves (grains) in bytes. Smaller
    // values may speed up small random writes, but increases metadata
    // overhead.
    //
    // Suggested values are around ~min(block_size/16, 512).
    //
    // Must be <= block_size/4. -1 disables grains, but requires
    // crystal_thresh=1. 0 is reserved.
    #ifndef LFS3_RDONLY
    lfs3_size_t grain_size;
    #endif

    // Threshold for compacting multiple grains into a data block.
    // Smaller values will crystallize more eagerly, reducing random
    // write fragmentation at the cost of random write performance.
    //
    // Ideally >= prog_size to avoid prog padding, but <= prog_size is
    // supported for when prog_size ~= block_size.
    //
    // Suggested values are around ~block_size/16.
    //
    // 1 only writes blocks, while -1 or any value > block_size only
    // writes grains. Grain-only files may be useful for optimizing
    // random-write-heavy workloads, but increase disk usage by ~4x.
    // 0 is reserved.
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
    // LFS3_NAME_MAX+1, which can be changed by redefining LFS3_NAME_MAX
    // to reduce RAM. LFS3_NAME_MAX is stored in superblock and must be
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
    lfs3_block_t block_count;

    // Upper limit on the length of file names in bytes.
    lfs3_size_t name_limit;

    // Upper limit on the size of files in bytes.
    lfs3_off_t file_limit;
};

// Traversal info structure
struct lfs3_binfo {
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
    // Additional file config flags. Bitwise-ored with open flags when
    // relevant.
    uint32_t flags;

    // Optional statically allocated file cache buffer. Must be
    // fcache_size. By default lfs3_malloc is used to allocate this
    // buffer.
    void *fcache_buffer;

    // Size of the file cache in bytes. In addition to filesystem-wide
    // read/prog caches, each file gets its own cache to reduce disk
    // accesses. Defaults to fcache_size if fcache_buffer is NULL.
    lfs3_size_t fcache_size;

    // Optional list of custom attributes attached to the file. If
    // readable, these attributes will be kept up to date with the
    // attributes on-disk. If writeable, these attributes will be
    // written to disk atomically on every file sync or close.
    struct lfs3_attr *attrs;

    // Number of custom attributes in the list
    lfs3_size_t attr_count;

    // Maximum size of inlined B-tree leaves (grains) in bytes. Smaller
    // values may speed up small random writes, but increases metadata
    // overhead.
    //
    // Suggested values are around ~min(block_size/16, 512).
    //
    // Must be <= block_size/4. -1 disables grains, but requires
    // crystal_thresh=1. Defaults to cfg.grain_size when 0.
    #ifndef LFS3_RDONLY
    lfs3_size_t grain_size;
    #endif

    // Threshold for compacting multiple grains into a data block.
    // Smaller values will crystallize more eagerly, reducing random
    // write fragmentation at the cost of random write performance.
    //
    // Ideally >= prog_size to avoid prog padding, but <= prog_size is
    // supported for when prog_size ~= block_size.
    //
    // Suggested values are around ~block_size/16.
    //
    // 1 only writes blocks, while -1 or any value > block_size only
    // writes grains. Grain-only files may be useful for optimizing
    // random-write-heavy workloads, but increase disk usage by ~4x.
    // Defaults to cfg.crystal_thresh when zero.
    #ifndef LFS3_RDONLY
    lfs3_size_t crystal_thresh;
    #endif
};



/// On-disk things ///

// On-disk metadata tags
enum lfs3_tag {
    // null tag reserved for null tag things
    LFS3_TAG_NULL           = 0x0000,   /// v--- ---- ++++ ++++
    // internal tag reserved for in-device use
    LFS3_TAG_INTERNAL       = 0x0100,   /// v--- ---1 +ttt tttt

    // config tags
    LFS3_TAG_CONFIG         = 0x0200,   /// v--- --1- +ttt tttt
    LFS3_TAG_MAGIC          = 0x0201,   //  v--- --1- +--- --rr
    LFS3_TAG_VERSION        = 0x0204,   //  v--- --1- +--- -1++
    LFS3_TAG_COMPAT         = 0x0208,   //  v--- --1- +--- 1-++
    LFS3_TAG_GEOMETRY       = 0x020c,   //  v--- --1- +--- 11++
    LFS3_TAG_NAMELIMIT      = 0x0210,   //  v--- --1- +--1 --++
    LFS3_TAG_FILELIMIT      = 0x0214,   //  v--- --1- +--1 -1++

    // global-state tags
    LFS3_TAG_GDELTA         = 0x0300,   /// v--- --11 +ttt tttt
    LFS3_TAG_GRMDELTA       = 0x0300,   //  v--- --11 +--- --++
    LFS3_TAG_GBMAPDELTA     = 0x0304,   //  v--- --11 +--- -1rr

    // name tags
    LFS3_TAG_NAME           = 0x0400,   /// v--- -1-- +ttt tttt
    LFS3_TAG_BNAME          = 0x0400,   //  v--- -1-- +--- ----
    LFS3_TAG_REG            = 0x0401,   //  v--- -1-- +--- ---1
    LFS3_TAG_DIR            = 0x0402,   //  v--- -1-- +--- --1-
    LFS3_TAG_STICKYNOTE     = 0x0403,   //  v--- -1-- +--- --11
    LFS3_TAG_BOOKMARK       = 0x0404,   //  v--- -1-- +--- -1--
    // in-device only name tags, these should never get written to disk
    LFS3_tag_ORPHAN         = 0x0405,
    LFS3_tag_TRV            = 0x0406,
    LFS3_tag_GC             = 0x0407,
    LFS3_tag_UNKNOWN        = 0x0408,
    // non-file name tags
    LFS3_TAG_MNAME          = 0x0440,   //  v--- -1-- +1-- ----

    // struct tags
    LFS3_TAG_STRUCT         = 0x0500,   /// v--- -1-1 +ttt tttt
    LFS3_TAG_BRANCH         = 0x0500,   //  v--- -1-1 +--- --rr
    LFS3_TAG_BSHRUB         = 0x0508,   //  v--- -1-1 +--- 1-rr
    LFS3_TAG_BTREE          = 0x050c,   //  v--- -1-1 +--- 11rr
    LFS3_TAG_HOLE           = 0x0510,   //  v--- -1-1 +--1 --++
    LFS3_TAG_DATA           = 0x0514,   //  v--- -1-1 +--1 -1++
    LFS3_TAG_BLOCK          = 0x0518,   //  v--- -1-1 +--1 1err
    LFS3_TAG_DID            = 0x0520,   //  v--- -1-1 +-1- --++
    LFS3_TAG_MROOT          = 0x0541,   //  v--- -1-1 +1-- --rr
    LFS3_TAG_MDIR           = 0x0545,   //  v--- -1-1 +1-- -1rr
    LFS3_TAG_MTREE          = 0x054c,   //  v--- -1-1 +1-- 11rr
    LFS3_TAG_BMRANGE        = 0x0550,   //  v--- -1-1 +1-1 ++uu
    LFS3_TAG_BMFREE         = 0x0550,   //  v--- -1-1 +1-1 ----
    LFS3_TAG_BMINUSE        = 0x0551,   //  v--- -1-1 +1-1 ---1
    LFS3_TAG_BMERASED       = 0x0552,   //  v--- -1-1 +1-1 --1-
    LFS3_TAG_BMBAD          = 0x0553,   //  v--- -1-1 +1-1 --11

    // user/sys attributes
    LFS3_TAG_ATTR           = 0x0600,   /// v--- -11a +aaa aaaa
    LFS3_TAG_UATTR          = 0x0600,   //  v--- -11- +aaa aaaa
    LFS3_TAG_SATTR          = 0x0700,   //  v--- -111 +aaa aaaa

    // shrub tags belong to secondary trees
    LFS3_TAG_SHRUB          = 0x1000,   /// v--1 kkkk +kkk kkkk

    // alt pointers form the inner nodes of our rbyd trees
    LFS3_TAG_ALT            = 0x4000,   /// v1cd kkkk +kkk kkkk
    LFS3_TAG_B              = 0x0000,
    LFS3_TAG_R              = 0x2000,
    LFS3_TAG_LE             = 0x0000,
    LFS3_TAG_GT             = 0x1000,

    // checksum tags
    LFS3_TAG_CKSUM          = 0x3000,   /// v-11 ---- ++++ +pqq
    LFS3_TAG_PHASE          = 0x0003,
    LFS3_TAG_PERTURB        = 0x0004,
    LFS3_TAG_NOTE           = 0x3100,   /// v-11 ---1 ++++ ++++
    LFS3_TAG_ECKSUM         = 0x3200,   /// v-11 --1- ++++ ++++
    LFS3_TAG_GCKSUMDELTA    = 0x3300,   /// v-11 --11 ++++ ++++

    // in-device only tags, these should never get written to disk
    LFS3_tag_NOOP           = 0x0100,
    LFS3_tag_RATTRS         = 0x0101,
    LFS3_tag_SHRUBCOMMIT    = 0x0102,
    LFS3_tag_GRMPUSH        = 0x0103,
    LFS3_tag_GRMPOP         = 0x0104,
    LFS3_tag_MOVE           = 0x0105,
    LFS3_tag_ATTRS          = 0x0106,

    // some in-device only tag modifiers
    LFS3_tag_RM             = 0x8000,
    LFS3_tag_GROW           = 0x4000,
    LFS3_tag_MASK0          = 0x0000,
    LFS3_tag_MASK2          = 0x1000,
    LFS3_tag_MASK8          = 0x2000,
    LFS3_tag_MASK12         = 0x3000,
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


// On-disk compat flags
//
// - RCOMPAT - Must understand to read the filesystem
// - WCOMPAT - Must understand to write to the filesystem
//
// At some point we may also add OCOMPAT flags (no understanding
// necessary), but we currently have no need for these.
//
// Note, "understanding" does not necessarily mean support

// On-disk read-compat flags - Must understand to read the filesystem
#define LFS3_RCOMPAT_WRONLY          0x0001 // Reading is disallowed
#define LFS3_RCOMPAT_EXPERIMENTAL    0x0002 // Experimental
#define LFS3_RCOMPAT_GRM             0x0004 // Global remove queue in use
#define LFS3_RCOMPAT_STICKYNOTE      0x0008 // Stickynote file type in use
// internally used flags
#define LFS3_rcompat_OVERFLOW        0x8000 // Can't represent all flags
// mask of features we care about when mounting
#define LFS3_rcompat_MASK \
    (0xffff)

// On-disk write-compat flags - Must understand to write to the filesystem
#define LFS3_WCOMPAT_RDONLY          0x0001 // Writing is disallowed
#define LFS3_WCOMPAT_EXPERIMENTAL    0x0002 // Experimental
#define LFS3_WCOMPAT_GCKSUM          0x0004 // Global checksum in use
#define LFS3_WCOMPAT_DIR             0x0008 // Directory files in use
#define LFS3_WCOMPAT_GBMAP           0x0010 // Global on-disk block-map in use
// internally used flags
#define LFS3_wcompat_OVERFLOW        0x8000 // Can't represent all flags
// mask of features we care about when mounting
#define LFS3_wcompat_MASK \
    (0xffff & ~( \
        LFS3_IFDEF_GBMAP(LFS3_WCOMPAT_GBMAP, 0)))


// On-disk encodings/decodings

// tag encoding:                                  tag:    1 be16      2 bytes
// .---+---+---+- -+- -+- -+- -+---+- -+- -+- -.  weight: 1 leb128  <=5 bytes
// |  tag  | weight            | size          |  size:   1 leb128  <=4 bytes
// '---+---+---+- -+- -+- -+- -+---+- -+- -+- -'                      .
#define LFS3_TAG_DSIZE                                               11

// le32 encoding:     word: 1 le32  4 bytes
// .---+---+---+---.                .
// |     le32      |                .
// '---+---+---+---'                .
#define LFS3_LE32_DSIZE             4

// leb128 encoding:       word: 1 leb128  <=5 bytes
// .---+- -+- -+- -+- -.                    .
// | leb128            |                    .
// '---+- -+- -+- -+- -'                    .
#define LFS3_LEB128_DSIZE                   5

// compat encoding:  rcompat: 1 leb128  <=1 bytes
// .- -+- -.         wcompat: 1 leb128  <=1 bytes
// | r | w |                              .
// '- -+- -'                              .
#define LFS3_COMPAT_DSIZE                 2

// geometry encoding:     block_size:  1 leb128  <=4 bytes
// .---+- -+- -+- -.      block_count: 1 leb128  <=5 bytes
// | block_size    |                               .
// +---+- -+- -+- -+- -.                           .
// | block_count       |                           .
// '---+- -+- -+- -+- -'                           .
#define LFS3_GEOMETRY_DSIZE                        9

// grm encoding:          mids:  2 leb128s  <=2x5 bytes
// .- -+- -+- -+- -+- -.                        .
// ' mids              '                        .
// +                   +                        .
// '                   '                        .
// '- -+- -+- -+- -+- -'                        .
#define LFS3_GRM_DSIZE                         10

// gbmap encoding:        window: 1 leb128  <=5 bytes
// .---+- -+- -+- -+- -.  known:  1 leb128  <=5 bytes
// | window            |  block:  1 leb128  <=5 bytes
// +---+- -+- -+- -+- -+  trunk:  1 leb128  <=4 bytes
// | known             |  cksum:  1 le32      4 bytes
// +---+- -+- -+- -+- -+                      .
// | block             |                      .
// +---+- -+- -+- -+- -'                      .
// | trunk         |                          .
// +---+- -+- -+- -+                          .
// |     cksum     |                          .
// '---+---+---+---'                          .
#define LFS3_GBMAP_DSIZE                     23

// branch encoding:       block: 1 leb128  <=5 bytes
// .---+- -+- -+- -+- -.  trunk: 1 leb128  <=4 bytes
// | block             |  cksum: 1 le32      4 bytes
// +---+- -+- -+- -+- -'                     .
// | trunk         |                         .
// +---+- -+- -+- -+                         .
// |     cksum     |                         .
// '---+---+---+---'                         .
#define LFS3_BRANCH_DSIZE                   13

// bptr encoding:         size:   1 leb128  <=4 bytes
// .---+- -+- -+- -.      block:  1 leb128  <=5 bytes
// | size          |      off:    1 leb128  <=4 bytes
// +---+- -+- -+- -+- -.  cksize: 1 leb128  <=4 bytes
// | block             |  cksum:  1 le32      4 bytes
// +---+- -+- -+- -+- -'                      .
// | off           |                          .
// +---+- -+- -+- -+                          .
// | cksize        |                          .
// +---+- -+- -+- -+                          .
// |     cksum     |                          .
// '---+---+---+---'                          .
#define LFS3_BPTR_DSIZE                      21

// btree encoding:        weight: 1 leb128  <=5 bytes
// .---+- -+- -+- -+- -.  block:  1 leb128  <=5 bytes
// | weight            |  trunk:  1 leb128  <=4 bytes
// +---+- -+- -+- -+- -+  cksum:  1 le32      4 bytes
// | block             |                      .
// +---+- -+- -+- -+- -'                      .
// | trunk         |                          .
// +---+- -+- -+- -+                          .
// |     cksum     |                          .
// '---+---+---+---'                          .
#define LFS3_BTREE_DSIZE                     18

// shrub encoding:        weight: 1 leb128  <=5 bytes
// .---+- -+- -+- -+- -.  trunk:  1 leb128  <=4 bytes
// | weight            |                      .
// +---+- -+- -+- -+- -'                      .
// | trunk         |                          .
// '---+- -+- -+- -'                          .
#define LFS3_SHRUB_DSIZE                      9

// mptr encoding:         blocks: 2 leb128s  <=2x5 bytes
// .---+- -+- -+- -+- -.                         .
// | block x 2         |                         .
// +                   +                         .
// |                   |                         .
// '---+- -+- -+- -+- -'                         .
#define LFS3_MPTR_DSIZE                         10

// ecksum encoding:   cksize: 1 leb128  <=4 bytes
// .---+- -+- -+- -.  cksum:  1 le32      4 bytes
// | cksize        |                      .
// +---+- -+- -+- -+                      .
// |     cksum     |                      .
// '---+---+---+---'                      .
#define LFS3_ECKSUM_DSIZE                 8



/// Internal littlefs structs ///

// either an on-disk or in-RAM data pointer
//
// note, it's tempting to make this fancier, but we benefit quite a lot
// from the compiler being able to aggresively optimize this struct
//
typedef struct lfs3_data {
    // this is only lfs3_off_t because we use it to store holes in
    // lfs3_bptr_t, when used as lfs3_data_t, this should only store
    // lfs3_size_ts
    lfs3_off_t weight;
    // sign2(off)=0b00 => in-RAM buffer
    // sign2(off)=0b01 => hole (unreadable)
    // sign2(off)=0b10 => on-disk data
    // sign2(off)=0b11 => on-disk data + cksum
    lfs3_size_t off;
    union {
        const uint8_t *buffer;
        struct {
            lfs3_block_t block;
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
    // sign2(off)=0b00 => in-RAM buffer
    // sign2(off)=0b01 => hole (unreadable)
    // sign2(off)=0b10 => on-disk grain 
    // sign2(off)=0b11 => on-disk bptr
    lfs3_data_t d;
    #ifndef LFS3_CKDATACKSUMS
    // sign(cksize)=0 => block not erased
    // sign(cksize)=1 => block erased
    lfs3_size_t cksize;
    uint32_t cksum;
    #endif
} lfs3_bptr_t;

// erased-state checksum
#ifndef LFS3_RDONLY
typedef struct lfs3_ecksum {
    // cksize=-1 indicates no ecksum
    lfs3_ssize_t cksize;
    uint32_t cksum;
} lfs3_ecksum_t;
#endif

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

// a btree is represented by the root rbyd
typedef lfs3_rbyd_t lfs3_btree_t;

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
typedef lfs3_rbyd_t lfs3_bshrub_t;

// littlefs file type
typedef struct lfs3_file {
    lfs3_handle_t h;
    const struct lfs3_file_cfg *cfg;
    lfs3_size_t grain_size;
    lfs3_size_t crystal_thresh;

    // current file position
    lfs3_off_t pos;

    // in-RAM cache
    struct {
        lfs3_off_t pos;
        lfs3_off_t size;
        uint8_t *buffer;
    } cache;

    // on-disk bshrub/btree, our core file data-structure
    //
    // files contain both an active bshrub and staging bshrub, to allow
    // staging during mdir compacts
    //
    // weight=0      => no bshrub/btree
    // sign(trunk)=1 => bshrub
    // sign(trunk)=0 => btree
    lfs3_bshrub_t bshrub;
    #ifndef LFS3_RDONLY
    lfs3_bshrub_t bshrub_;
    #endif

    // on-disk leaf bptr
    struct {
        lfs3_off_t pos;
        lfs3_bptr_t bptr;
    } leaf;
} lfs3_file_t;

// littlefs directory type
typedef struct lfs3_dir {
    lfs3_handle_t h;
    lfs3_did_t did;
    lfs3_off_t pos;
} lfs3_dir_t;

// littlefs low-level traversal types
typedef struct lfs3_btrv {
    lfs3_sbid_t bid;
    lfs3_rbyd_t rbyd;
    lfs3_srid_t rid;
} lfs3_btrv_t;

typedef struct lfs3_mtortoise {
    // this aligns with btrv.bid
    lfs3_sbid_t bid;
    lfs3_block_t blocks[2];
    lfs3_block_t dist;
    uint8_t nlog2;
} lfs3_mtortoise_t;

typedef struct lfs3_mtrv {
    // mtree traversal state, our position in then handle linked-list
    // is also used to keep track of what handles we've seen
    lfs3_handle_t h;
    // current bshrub/btree
    lfs3_btree_t btree;
    union {
        // bshrub/btree traversal state
        lfs3_btrv_t btrv;
        // mtortoise for cycle detection
        lfs3_mtortoise_t mtortoise;
    } u;

    // recalculate gcksum when traversing with ckmeta
    uint32_t gcksum;
} lfs3_mtrv_t;

typedef struct lfs3_mgc {
    // core traversal state
    lfs3_mtrv_t t;

    #ifdef LFS3_GBMAP
    // repopulate gbmap when traversing with lookgbmap
    lfs3_btree_t gbmap_;
    #endif
} lfs3_mgc_t;

// littlefs traversal type
typedef struct lfs3_trv {
    // core traversal state
    lfs3_mtrv_t t;

    // pending blocks, only used in lfs3_trv_read
    lfs3_sblock_t blocks[2];
} lfs3_trv_t;

// littlefs gc type
typedef struct lfs3_gc {
    // core gc state
    lfs3_mgc_t gc;
} lfs3_gc_t;

// a single eviction entry
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
typedef struct lfs3_evict {
    // sign(block)=0  => damaged
    // sign(block)=1  => bad
    lfs3_block_t block;
    // sign(block_)=1 => is definitely data
    // block_!=0      => dest block for dags
    lfs3_sblock_t block_;
} lfs3_evict_t;
#endif

// optional evict queue
#if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
typedef struct lfs3_evictqueue lfs3_evictqueue_t;
#endif

// littlefs on-disk compat flags
typedef uint32_t lfs3_compat_t;

// littlefs on-disk geometry
typedef struct lfs3_geometry {
    lfs3_size_t block_size;
    lfs3_block_t block_count;
} lfs3_geometry_t;

// littlefs global state
typedef struct lfs3_grm lfs3_grm_t;
#ifdef LFS3_GBMAP
typedef struct lfs3_gbmap lfs3_gbmap_t;
#endif

// The littlefs filesystem type
typedef struct lfs3 {
    uint32_t flags;
    const struct lfs3_cfg *cfg;
    lfs3_block_t block_count;
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
    lfs3_btree_t mtree;

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
    #ifdef LFS3_CKMETAPARITY
    struct {
        lfs3_block_t block;
        // sign(off) => tail parity
        lfs3_size_t off;
    } ptail;
    #endif
    #endif

    #ifndef LFS3_RDONLY
    struct lfs3_lookahead {
        lfs3_block_t window;
        lfs3_block_t off;
        lfs3_block_t known;
        lfs3_block_t ckpoint;
        uint8_t *buffer;
    } lookahead;
    #endif

    // global state
    uint32_t gcksum;
    #ifndef LFS3_RDONLY
    uint32_t gcksum_p;
    #endif
    // TODO can we actually get rid of grm_d when LFS3_RDONLY?
    uint32_t gcksum_d;

    struct lfs3_grm {
        lfs3_mid_t queue[2];
    } grm;
    #ifndef LFS3_RDONLY
    uint8_t grm_p[LFS3_GRM_DSIZE];
    #endif
    // TODO can we actually get rid of grm_d when LFS3_RDONLY?
    uint8_t grm_d[LFS3_GRM_DSIZE];

    #ifdef LFS3_GBMAP
    struct lfs3_gbmap {
        lfs3_block_t window;
        lfs3_block_t known;
        #if !defined(LFS3_RDONLY)
        lfs3_sblock_t next;
        #endif
        #if !defined(LFS3_RDONLY) && defined(LFS3_PREERASE)
        lfs3_ecksum_t ecksum;
        struct lfs3_preeraser {
            lfs3_block_t known;
            lfs3_block_t count;
        } preeraser;
        #endif
        lfs3_btree_t b;
        lfs3_btree_t b_p;
    } gbmap;
    uint8_t gbmap_p[LFS3_GBMAP_DSIZE];
    uint8_t gbmap_d[LFS3_GBMAP_DSIZE];
    #endif

    // optional evict queue
    #if !defined(LFS3_RDONLY) && defined(LFS3_EVICT)
    struct lfs3_evictqueue {
        lfs3_evict_t *queue;
        lfs3_size_t count;
    } evictqueue;
    #endif

    // optional incremental gc state
    #ifdef LFS3_GC
    lfs3_mgc_t gc;
    #endif
} lfs3_t;



/// Filesystem functions ///

// Format a block device with the littlefs
//
// Requires a littlefs object and config struct. This clobbers the
// littlefs object, and does not leave the filesystem mounted. The
// config struct must be zeroed for defaults and backwards
// compatibility.
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
// lfs3 and config must be allocated while mounted. The config struct
// must be zeroed for defaults and backwards compatibility.
//
// Returns a negative error code on failure.
int lfs3_mount(lfs3_t *lfs3, uint32_t flags,
        const struct lfs3_cfg *cfg);

// Unmounts a littlefs
//
// Does nothing besides releasing any allocated resources.
//
// Returns a negative error code on failure.
int lfs3_unmount(lfs3_t *lfs3);

/// General operations ///

// Get the value of a file
//
// Returns the number of bytes read, or a negative error code on
// failure. Note this may be less than the on-disk file size if the
// buffer is not large enough.
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
//
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
// Fills out the info structure, based on the specified file or
// directory.
//
// Returns a negative error code on failure.
int lfs3_stat(lfs3_t *lfs3, const char *path, struct lfs3_info *info);

// Get a custom attribute
//
// Returns the number of bytes read, or a negative error code on
// failure. Note this may be less than the on-disk attr size if the
// buffer is not large enough.
lfs3_ssize_t lfs3_getattr(lfs3_t *lfs3, const char *path, uint8_t type,
        void *buffer, lfs3_size_t size);

// Get a custom attribute's size
//
// Returns the size of the attribute, or a negative error code on
// failure.
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
// Returns a negative error code on failure.
#ifndef LFS3_NO_MALLOC
int lfs3_file_open(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags);
#endif

// Open a file with extra configuration
//
// The config struct provides additional config options per file as
// described above. The config struct must remain allocated while the
// file is open, and the config struct must be zeroed for defaults and
// backwards compatibility.
//
// Returns a negative error code on failure.
int lfs3_file_opencfg(lfs3_t *lfs3, lfs3_file_t *file,
        const char *path, uint32_t flags,
        const struct lfs3_file_cfg *cfg);

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
int lfs3_file_close(lfs3_t *lfs3, lfs3_file_t *file);

// Synchronize a file on storage
//
// Any pending writes are written out to storage and other open files.
//
// If the file was desynchronized, it is now marked as synchronized. It
// will now recieve file updates and syncs on close.
//
// Returns a negative error code on failure.
int lfs3_file_sync(lfs3_t *lfs3, lfs3_file_t *file);

// Flush any buffered data
//
// This does not update metadata and is called implicitly by
// lfs3_file_sync. Calling this explicitly may be useful for preventing
// write errors in read operations.
//
// Returns a negative error code on failure.
int lfs3_file_flush(lfs3_t *lfs3, lfs3_file_t *file);

// Mark a file as desynchronized
//
// Desynchronized files do not recieve file updates and do not sync on
// close. They effectively act as snapshots of the underlying file at
// that point in time.
//
// If an error occurs during a write operation, the file is implicitly
// marked as desynchronized.
//
// An explicit and successful call to either lfs3_file_sync or
// lfs3_file_resync reverses this, marking the file as synchronized
// again.
//
// Returns a negative error code on failure.
int lfs3_file_desync(lfs3_t *lfs3, lfs3_file_t *file);

// Discard unsynchronized changes and mark a file as synchronized
//
// This is effectively the same as closing and reopening the file, and
// may read from disk to figure out file state.
//
// Returns a negative error code on failure.
int lfs3_file_resync(lfs3_t *lfs3, lfs3_file_t *file);

// Read data from file
//
// Takes a buffer and size indicating where to store the read data.
//
// Returns the number of bytes read, or a negative error code on
// failure.
lfs3_ssize_t lfs3_file_read(lfs3_t *lfs3, lfs3_file_t *file,
        void *buffer, lfs3_size_t size);

// Write data to file
//
// Takes a buffer and size indicating the data to write. The file will
// not actually be updated on the storage until either sync or close is
// called.
//
// Returns the number of bytes written, or a negative error code on
// failure.
#ifndef LFS3_RDONLY
lfs3_ssize_t lfs3_file_write(lfs3_t *lfs3, lfs3_file_t *file,
        const void *buffer, lfs3_size_t size);
#endif

// Change the position of the file
//
// The change in position is determined by the offset and whence flag.
//
// Returns the new position of the file, or a negative error code on
// failure.
lfs3_soff_t lfs3_file_seek(lfs3_t *lfs3, lfs3_file_t *file,
        lfs3_soff_t off, uint32_t whence);

// Truncate/grow the size of the file to the specified size
//
// If size is larger than the current file size, a hole is created,
// appearing as if the file was filled with zeros.
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_file_truncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size);
#endif

// Truncate/grow the file, but from the front
//
// If size is larger than the current file size, a hole is created,
// appearing as if the file was filled with zeros.
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_file_fruncate(lfs3_t *lfs3, lfs3_file_t *file, lfs3_off_t size);
#endif

// Return the position of the file
//
// Equivalent to lfs3_file_seek(lfs3, file, 0, LFS3_SEEK_CUR)
//
// Returns the position of the file, or a negative error code on
// failure.
lfs3_soff_t lfs3_file_tell(lfs3_t *lfs3, lfs3_file_t *file);

// Change the position of the file to the beginning of the file
//
// Equivalent to lfs3_file_seek(lfs3, file, 0, LFS3_SEEK_SET)
//
// Returns a negative error code on failure.
int lfs3_file_rewind(lfs3_t *lfs3, lfs3_file_t *file);

// Return the size of the file
//
// Similar to lfs3_file_seek(lfs3, file, 0, LFS3_SEEK_END)
//
// Returns the size of the file, or a negative error code on failure.
lfs3_soff_t lfs3_file_size(lfs3_t *lfs3, lfs3_file_t *file);

// Check a file for damage
//
// Returns LFS3_ERR_CORRUPT if a checksum mismatch is found, or a
// negative error code on failure.
int lfs3_file_ck(lfs3_t *lfs3, lfs3_file_t *file, uint32_t flags);

// Check and repair damage in a file
//
// Returns LFS3_ERR_CORRUPT if unrecoverable damage is found, or a
// negative error code on failure.
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
int lfs3_file_repair(lfs3_t *lfs3, lfs3_file_t *file, uint32_t flags);
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
//
// Returns a negative error code on failure.
int lfs3_dir_open(lfs3_t *lfs3, lfs3_dir_t *dir, const char *path);

// Close a directory
//
// Releases any allocated resources.
//
// Returns a negative error code on failure.
int lfs3_dir_close(lfs3_t *lfs3, lfs3_dir_t *dir);

// Read an entry in the directory
//
// Fills out the info structure, based on the specified file or
// directory.
//
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
// The returned offset is only meant to be consumed by seek and may not
// make sense, but does indicate the current position in the directory
// iteration.
//
// Returns the position of the directory, or a negative error code on
// failure.
lfs3_soff_t lfs3_dir_tell(lfs3_t *lfs3, lfs3_dir_t *dir);

// Change the position of the directory to the beginning of the
// directory
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
//
// Returns a negative error code on failure.
int lfs3_trv_close(lfs3_t *lfs3, lfs3_trv_t *trv);

// Progress the traversal and read an entry
//
// Fills out the binfo structure.
//
// Returns 0 on success, LFS3_ERR_NOENT at the end of traversal, or a
// negative error code on failure.
int lfs3_trv_read(lfs3_t *lfs3, lfs3_trv_t *trv,
        struct lfs3_binfo *binfo);

// Reset the traversal
//
// Returns a negative error code on failure.
int lfs3_trv_rewind(lfs3_t *lfs3, lfs3_trv_t *trv);


/// GC operations ///

// Open a gc
//
// Once open, a gc and be written to progress any pending janitorial
// work.
//
// See also lfs3_fs_gc when LFS3_GC is defined for an easier API. This
// provides low-level access to progressing janitorial work, but is
// discouraged for library use.
//
// Returns a negative error code on failure.
int lfs3_gc_open(lfs3_t *lfs3, lfs3_gc_t *gc, uint32_t flags);

// Close a gc
//
// Releases any allocated resources.
//
// Returns a negative error code on failure.
int lfs3_gc_close(lfs3_t *lfs3, lfs3_gc_t *gc);

// Progress the gc and perform any janitorial work that may be pending
//
// The exact janitorial work depends on the flags in cfg.gc_flags.
//
// The steps field controls how much work is done during each call, with
// each step being ~1 block of work. More steps per call will make more
// progress if interleaved with other filesystem operations, but may
// also introduce more latency.
//
// steps=1 or 0 will do the minimum amount of work to make progress, and
// steps=-1 will not return until all pending janitorial work has been
// completed.
//
// Returns the number of steps progressed on success, 0 if no work is
// available, or a negative error code on failure.
lfs3_sblock_t lfs3_gc_write(lfs3_t *lfs3, lfs3_gc_t *gc, lfs3_sblock_t steps);


/// Filesystem-level filesystem operations

// Find on-disk info about the filesystem
//
// Fills out the fsinfo structure based on the filesystem found on-disk.
//
// Returns a negative error code on failure.
int lfs3_fs_stat(lfs3_t *lfs3, struct lfs3_fsinfo *fsinfo);

// Find the number of blocks in use by the filesystem
//
// Note: Result is best effort. If files share CoW structures, the
// returned size may be larger than the filesystem actually is.
//
// Returns the number of allocated blocks, or a negative error code on
// failure.
lfs3_sblock_t lfs3_fs_size(lfs3_t *lfs3);

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

// Attempt to make the filesystem consistent and ready for writing
//
// Calling this function is not required, consistency will be implicitly
// enforced on the first operation that writes to the filesystem, but
// this function allows the work to be performed earlier and without
// other filesystem changes.
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_fs_mkconsistent(lfs3_t *lfs3);
#endif

// Check the filesystem for damage
//
// Returns LFS3_ERR_CORRUPT if a checksum mismatch is found, or a
// negative error code on failure.
int lfs3_fs_ck(lfs3_t *lfs3, uint32_t flags);

// Check and repair damage in the filesystem
//
// Returns LFS3_ERR_CORRUPT if unrecoverable damage is found, or a
// negative error code on failure.
#if !defined(LFS3_RDONLY) && defined(LFS3_REPAIR)
int lfs3_fs_repair(lfs3_t *lfs3, uint32_t flags);
#endif

// Perform any janitorial work that may be pending
//
// The exact janitorial work depends on the configured flags and steps.
//
// Calling this function is not required, but may allow the offloading
// of expensive janitorial work to a less time-critical code path.
//
// Returns the number of steps progressed on success, 0 if no work is
// available, or a negative error code on failure.
#ifdef LFS3_GC
lfs3_sblock_t lfs3_fs_gc(lfs3_t *lfs3);
#endif

// Request janitorial work
//
// This is mainly for triggering new ckmeta/ckdata scans with
// LFS3_I_CKMETA and LFS3_I_CKDATA. Otherwise littlefs will perform
// only one scan after mount.
//
// Returns a negative error code on failure.
int lfs3_fs_requestck(lfs3_t *lfs3, uint32_t flags);

// Clear flags/optional janitorial work
//
// This can be used to clear janitorial work that is not strictly
// necessary, such as LFS3_I_CKMETA, LFS3_I_REPAIRMETA, etc, as well as
// informative sticky flags such as LFS3_I_GRMOVERFLOW, etc.
//
// Returns a negative error code on failure.
int lfs3_fs_clearck(lfs3_t *lfs3, uint32_t flags);

// Change the number of blocks used by the filesystem
//
// This changes the number of blocks we are currently using and updates
// the superblock with the new block count.
//
// Note: This is irreversible.
//
// Returns a negative error code on failure.
#ifndef LFS3_RDONLY
int lfs3_fs_grow(lfs3_t *lfs3, lfs3_block_t block_count, uint32_t flags);
#endif

// Enable the global on-disk block-map
//
// Returns 0 on success, LFS3_ERR_EXIST a gbmap already exists, or a
// negative error code on failure.
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
int lfs3_fs_mkgbmap(lfs3_t *lfs3);
#endif

// Disable the global on-disk block-map
//
// Returns 0 on success, LFS3_ERR_NOENT if no gbmap is found, or a
// negative error code on failure.
#if !defined(LFS3_RDONLY) && defined(LFS3_GBMAP)
int lfs3_fs_rmgbmap(lfs3_t *lfs3);
#endif

// Evict a block from the filesystem, and/or mark it as good/bad
//
// Returns 0 if all flags are satisfied, or a negative error code on
// failure.
#if !defined(LFS3_RDONLY) && (defined(LFS3_EVICT) || defined(LFS3_GBMAP))
int lfs3_fs_evictblock(lfs3_t *lfs3, lfs3_block_t block, uint32_t flags);
#endif

// Find info about littlefs's knowledge of a specific block
//
// Note littlefs generally knowns very little, and will return
// LFS3_BTYPE_UNKNOWN for most blocks. lfs3_trv_t can be used figure out
// more info, but at a runtime cost.
//
// Fills out the binfo structure using lookahead and gbmap information.
//
// Returns a negative error code on failure.
int lfs3_fs_statblock(lfs3_t *lfs3, lfs3_block_t block,
        struct lfs3_binfo *binfo);


#endif
