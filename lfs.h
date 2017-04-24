/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_H
#define LFS_H

#include <stdint.h>
#include <stdbool.h>


// Type definitions
typedef uint32_t lfs_size_t;
typedef uint32_t lfs_off_t;

typedef int32_t  lfs_ssize_t;
typedef int32_t  lfs_soff_t;

typedef uint32_t lfs_block_t;


// Configurable littlefs constants
#ifndef LFS_NAME_MAX
#define LFS_NAME_MAX 255
#endif

// The littefs constants
enum lfs_error {
    LFS_ERROR_OK       = 0,
    LFS_ERROR_CORRUPT  = -3,
    LFS_ERROR_NO_ENTRY = -4,
    LFS_ERROR_EXISTS   = -5,
    LFS_ERROR_NOT_DIR  = -6,
    LFS_ERROR_IS_DIR   = -7,
    LFS_ERROR_INVALID  = -8,
    LFS_ERROR_NO_SPACE = -9,
    LFS_ERROR_NO_MEM   = -10,
};

enum lfs_type {
    LFS_TYPE_REG        = 0x01,
    LFS_TYPE_DIR        = 0x02,
    LFS_TYPE_SUPERBLOCK = 0x10,
};

enum lfs_open_flags {
    LFS_O_RDONLY = 0,
    LFS_O_WRONLY = 1,
    LFS_O_RDWR   = 2,
    LFS_O_CREAT  = 0x020,
    LFS_O_EXCL   = 0x040,
    LFS_O_TRUNC  = 0x080,
    LFS_O_APPEND = 0x100,
    LFS_O_SYNC   = 0x200,
};

enum lfs_whence_flags {
    LFS_SEEK_SET = 0,
    LFS_SEEK_CUR = 1,
    LFS_SEEK_END = 2,
};


// Configuration provided during initialization of the littlefs
struct lfs_config {
    // Opaque user provided context
    void *context;

    // Read a region in a block
    int (*read)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, lfs_size_t size, void *buffer);

    // Program a region in a block. The block must have previously
    // been erased.
    int (*prog)(const struct lfs_config *c, lfs_block_t block,
            lfs_off_t off, lfs_size_t size, const void *buffer);

    // Erase a block. A block must be erased before being programmed.
    // The state of an erased block is undefined.
    int (*erase)(const struct lfs_config *c, lfs_block_t block);

    // Sync the state of the underlying block device
    int (*sync)(const struct lfs_config *c);

    // Minimum size of a read. This may be larger than the physical
    // read size to cache reads from the block device.
    lfs_size_t read_size;

    // Minimum size of a program. This may be larger than the physical
    // program size to cache programs to the block device.
    lfs_size_t prog_size;

    // Size of an erasable block.
    lfs_size_t block_size;

    // Number of erasable blocks on the device.
    lfs_size_t block_count;

    // Number of blocks to lookahead during block allocation.
    lfs_size_t lookahead;

    // Optional, statically allocated read buffer. Must be read sized.
    void *read_buffer;

    // Optional, statically allocated program buffer. Must be program sized.
    void *prog_buffer;

    // Optional, statically allocated lookahead buffer.
    // Must be 1 bit per lookahead block.
    void *lookahead_buffer;
};

// File info structure
struct lfs_info {
    // Type of the file, either REG or DIR
    uint8_t type;

    // Size of the file, only valid for REG files
    lfs_size_t size;

    // Name of the file stored as a null-terminated string
    char name[LFS_NAME_MAX+1];
};


// littlefs data structures
typedef struct lfs_entry {
    lfs_block_t pair[2];
    lfs_off_t off;

    struct lfs_disk_entry {
        uint16_t type;
        uint16_t len;
        union {
            struct {
                lfs_block_t head;
                lfs_size_t size;
            } file;
            lfs_block_t dir[2];
        } u;
    } d;
} lfs_entry_t;

typedef struct lfs_file {
    struct lfs_entry entry;
    int flags;

    lfs_off_t wpos;
    lfs_block_t wblock;
    lfs_off_t woff;

    lfs_off_t rpos;
    lfs_block_t rblock;
    lfs_off_t roff;
} lfs_file_t;

typedef struct lfs_dir {
    lfs_block_t pair[2];
    lfs_off_t off;

    lfs_block_t head[2];
    lfs_off_t pos;

    struct lfs_disk_dir {
        uint32_t rev;
        lfs_size_t size;
        lfs_block_t tail[2];
    } d;
} lfs_dir_t;

typedef struct lfs_superblock {
    lfs_block_t pair[2];
    lfs_off_t off;

    struct lfs_disk_superblock {
        uint16_t type;
        uint16_t len;
        uint32_t version;
        char magic[8];
        uint32_t block_size;
        uint32_t block_count;
        lfs_block_t root[2];
    } d;
} lfs_superblock_t;

// littlefs type
typedef struct lfs {
    const struct lfs_config *cfg;
    lfs_size_t words;       // number of 32-bit words that can fit in a block

    lfs_block_t root[2];

    struct {
        lfs_block_t block;
        lfs_off_t off;
        uint8_t *buffer;
    } rcache, pcache;

    struct {
        lfs_block_t start;
        lfs_block_t off;
        uint32_t *lookahead;
    } free;
} lfs_t;


// filesystem functions
int lfs_format(lfs_t *lfs, const struct lfs_config *config);
int lfs_mount(lfs_t *lfs, const struct lfs_config *config);
int lfs_unmount(lfs_t *lfs);

// general operations
int lfs_remove(lfs_t *lfs, const char *path);
int lfs_rename(lfs_t *lfs, const char *oldpath, const char *newpath);
int lfs_stat(lfs_t *lfs, const char *path, struct lfs_info *info);

// directory operations
int lfs_mkdir(lfs_t *lfs, const char *path);
int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path);
int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir);
int lfs_dir_read(lfs_t *lfs, lfs_dir_t *dir, struct lfs_info *info);
int lfs_dir_seek(lfs_t *lfs, lfs_dir_t *dir, lfs_off_t off);
lfs_soff_t lfs_dir_tell(lfs_t *lfs, lfs_dir_t *dir);
int lfs_dir_rewind(lfs_t *lfs, lfs_dir_t *dir);

// file operations
int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags);
int lfs_file_close(lfs_t *lfs, lfs_file_t *file);
int lfs_file_sync(lfs_t *lfs, lfs_file_t *file);
lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size);
lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size);
lfs_soff_t lfs_file_seek(lfs_t *lfs, lfs_file_t *file,
        lfs_soff_t off, int whence);
lfs_soff_t lfs_file_tell(lfs_t *lfs, lfs_file_t *file);
int lfs_file_rewind(lfs_t *lfs, lfs_file_t *file);
lfs_soff_t lfs_file_size(lfs_t *lfs, lfs_file_t *file);

// miscellaneous lfs specific operations
int lfs_deorphan(lfs_t *lfs);
int lfs_traverse(lfs_t *lfs, int (*cb)(void*, lfs_block_t), void *data);


#endif
