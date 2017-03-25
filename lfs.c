/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#include "lfs.h"
#include "lfs_util.h"

#include <string.h>
#include <stdbool.h>


/// Block device operations ///
static int lfs_bd_info(lfs_t *lfs, struct lfs_bd_info *info) {
    return lfs->bd_ops->info(lfs->bd, info);
}

static int lfs_bd_read(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, void *buffer) {
    return lfs->bd_ops->read(lfs->bd, block, off, size, buffer);
}

static int lfs_bd_prog(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer) {
    return lfs->bd_ops->prog(lfs->bd, block, off, size, buffer);
}

static int lfs_bd_erase(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size) {
    return lfs->bd_ops->erase(lfs->bd, block, off, size);
}

static int lfs_bd_sync(lfs_t *lfs) {
    return lfs->bd_ops->sync(lfs->bd);
}

static int lfs_bd_cmp(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, const void *buffer) {
    const uint8_t *data = buffer;

    while (off < size) {
        uint8_t c;
        int err = lfs_bd_read(lfs, block, off, 1, &c);
        if (err) {
            return err;
        }

        if (c != *data) {
            return false;
        }

        data += 1;
        off += 1;
    }

    return true;
}

static int lfs_bd_crc(lfs_t *lfs, lfs_block_t block,
        lfs_off_t off, lfs_size_t size, uint32_t *crc) {
    while (off < size) {
        uint8_t c;
        int err = lfs_bd_read(lfs, block, off, 1, &c);
        if (err) {
            return err;
        }

        *crc = lfs_crc(&c, 1, *crc);
        off += 1;
    }

    return 0;
}


/// Block allocator ///

static int lfs_alloc(lfs_t *lfs, lfs_block_t *block) {
    if (lfs->free.begin != lfs->free.end) {
        *block = lfs->free.begin;
        lfs->free.begin += 1;
        return 0;
    }

    // TODO find next stride of free blocks
    // TODO verify no strides exist where begin > current begin
    // note: begin = 0 is invalid (superblock)
    return LFS_ERROR_NO_SPACE;
}

static int lfs_alloc_erased(lfs_t *lfs, lfs_block_t *block) {
    int err = lfs_alloc(lfs, block);
    if (err) {
        return err;
    }

    return lfs_bd_erase(lfs, *block, 0, lfs->block_size);
}


/// Index list operations ///

// Next index offset
static lfs_off_t lfs_index_next(lfs_t *lfs, lfs_off_t ioff) {
    ioff += 1;
    while (ioff % lfs->words == 0) {
        ioff += lfs_min(lfs_ctz(ioff/lfs->words + 1), lfs->words-1) + 1;
    }

    return ioff;
}

static lfs_off_t lfs_toindex(lfs_t *lfs, lfs_off_t off) {
    lfs_off_t i = 0;
    while (off > lfs->block_size) {
        i = lfs_index_next(lfs, i);
        off -= lfs->block_size;
    }

    return i;
}

// Find index in index chain given its index offset
static int lfs_index_find(lfs_t *lfs, lfs_block_t head,
        lfs_size_t icount, lfs_off_t ioff, lfs_block_t *block) {
    lfs_off_t iitarget = ioff / lfs->words;
    lfs_off_t iicurrent = (icount-1) / lfs->words;

    while (iitarget != iicurrent) {
        lfs_size_t skip = lfs_min(
                lfs_min(lfs_ctz(iicurrent+1), lfs->words-1),
                lfs_npw2((iitarget ^ iicurrent)+1)-1);
        
        int err = lfs_bd_read(lfs, head, 4*skip, 4, &head);
        if (err) {
            return err;
        }

        iicurrent -= 1 << skip;
    }

    return lfs_bd_read(lfs, head, 4*(ioff % lfs->words), 4, block);
}

// Append index to index chain, updates head and icount
static int lfs_index_append(lfs_t *lfs, lfs_block_t *headp,
        lfs_size_t *icountp, lfs_block_t block) {
    lfs_block_t head = *headp;
    lfs_size_t ioff = *icountp - 1;

    ioff += 1;

    while (ioff % lfs->words == 0) {
        lfs_block_t nhead;
        int err = lfs_alloc_erased(lfs, &nhead);
        if (err) {
            return err;
        }

        lfs_off_t skips = lfs_min(
                lfs_ctz(ioff/lfs->words + 1), lfs->words-2) + 1;
        for (lfs_off_t i = 0; i < skips; i++) {
            err = lfs_bd_prog(lfs, nhead, 4*i, 4, &head);
            if (err) {
                return err;
            }

            if (head && i != skips-1) {
                err = lfs_bd_read(lfs, head, 4*i, 4, &head);
                if (err) {
                    return err;
                }
            }
        }

        ioff += skips;
        head = nhead;
    }

    int err = lfs_bd_prog(lfs, head, 4*(ioff % lfs->words), 4, &block);
    if (err) {
        return err;
    }

    *headp = head;
    *icountp = ioff + 1;
    return 0;
}


/// Metadata pair operations ///

static inline void lfs_swap(lfs_block_t pair[2]) {
    lfs_block_t t = pair[0];
    pair[0] = pair[1];
    pair[1] = t;
}

struct lfs_fetch_region {
    lfs_off_t off;
    lfs_size_t size;
    void *data;
};

static int lfs_pair_fetch(lfs_t *lfs, lfs_block_t pair[2],
        int count, const struct lfs_fetch_region *regions) {
    int checked = 0;
    int rev = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t nrev;
        int err = lfs_bd_read(lfs, pair[1], 0, 4, &nrev);
        if (err) {
            return err;
        }

        if (checked > 0 && lfs_scmp(nrev, rev) < 0) {
            continue;
        }

        uint32_t crc = 0xffffffff;
        err = lfs_bd_crc(lfs, pair[1], 0, lfs->block_size, &crc);
        if (err) {
            return err;
        }

        if (crc != 0) {
            lfs_swap(pair);
        }

        checked += 1;
        rev = nrev;
        lfs_swap(pair);
    }

    if (checked == 0) {
        LFS_ERROR("Corrupted metadata pair at %d %d", pair[0], pair[1]);
        return LFS_ERROR_CORRUPT;
    }

    for (int i = 0; i < count; i++) {
        int err = lfs_bd_read(lfs, pair[0],
                regions[i].off, regions[i].size, regions[i].data);
        if (err) {
            return err;
        }
    }

    return 0;
}

struct lfs_commit_region {
    lfs_off_t off;
    lfs_size_t size;
    const void *data;
};

static int lfs_pair_commit(lfs_t *lfs, lfs_block_t pair[2],
        int count, const struct lfs_commit_region *regions) {
    uint32_t crc = 0xffffffff;
    int err = lfs_bd_erase(lfs, pair[1], 0, lfs->block_size);
    if (err) {
        return err;
    }

    lfs_off_t off = 0;
    while (off < lfs->block_size - 4) {
        if (count > 0 && regions[0].off == off) {
            crc = lfs_crc(regions[0].data, regions[0].size, crc);
            int err = lfs_bd_prog(lfs, pair[1],
                    off, regions[0].size, regions[0].data);
            if (err) {
                return err;
            }

            off += regions[0].size;
            count -= 1;
            regions += 1;
        } else {
            // TODO faster strides?
            uint8_t data;
            int err = lfs_bd_read(lfs, pair[0], off, 1, &data);
            if (err) {
                return err;
            }

            crc = lfs_crc((void*)&data, 1, crc);
            err = lfs_bd_prog(lfs, pair[1], off, 1, &data);
            if (err) {
                return err;
            }

            off += 1;
        }
    }

    err = lfs_bd_prog(lfs, pair[1], lfs->block_size-4, 4, &crc);
    if (err) {
        return err;
    }

    err = lfs_bd_sync(lfs);
    if (err) {
        return err;
    }

    lfs_swap(pair);
    return 0;
}


/// Directory operations ///

static int lfs_dir_create(lfs_t *lfs, lfs_dir_t *dir, lfs_block_t parent[2]) {
    // Allocate pair of dir blocks
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[i]);
        if (err) {
            return err;
        }
    }

    // Rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs_bd_read(lfs, dir->pair[0], 0, 4, &dir->d.rev);
    if (err) {
        return err;
    }
    dir->d.rev += 1;

    // Calculate total size
    dir->d.size = sizeof(dir->d);
    if (parent) {
        dir->d.size += sizeof(struct lfs_disk_entry);
    }

    // Other defaults
    dir->off = dir->d.size;
    dir->d.tail[0] = 0;
    dir->d.tail[1] = 0;
    dir->d.free = lfs->free;

    // Write out to memory
    return lfs_pair_commit(lfs, dir->pair,
        1 + (parent ? 2 : 0), (struct lfs_commit_region[]){
            {0, sizeof(dir->d), &dir->d},
            {sizeof(dir->d), sizeof(struct lfs_disk_entry),
             &(struct lfs_disk_entry){
                .type     = LFS_TYPE_DIR,
                .len      = 12+2,
                .u.dir[0] = parent ? parent[0] : 0,
                .u.dir[1] = parent ? parent[1] : 0,
            }},
            {sizeof(dir->d)+sizeof(struct lfs_disk_entry), 2, ".."},
        });
}

static int lfs_dir_fetch(lfs_t *lfs, lfs_dir_t *dir, lfs_block_t pair[2]) {
    dir->pair[0] = pair[0];
    dir->pair[1] = pair[1];
    dir->off = sizeof(dir->d);

    return lfs_pair_fetch(lfs, dir->pair,
        1, (struct lfs_fetch_region[1]) {
            {0, sizeof(dir->d), &dir->d}
        });
}

static int lfs_dir_next(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry) {
    while (true) {
        // TODO iterate down list
        entry->dir[0] = dir->pair[0];
        entry->dir[1] = dir->pair[1];
        entry->off = dir->off;

        if (dir->d.size - dir->off < sizeof(entry->d)) {
            return LFS_ERROR_NO_ENTRY;
        }

        int err = lfs_bd_read(lfs, dir->pair[0], dir->off,
                sizeof(entry->d), &entry->d);
        if (err) {
            return err;
        }

        dir->off += entry->d.len;

        // Skip any unknown entries
        if ((entry->d.type & 0xf) == 1 || (entry->d.type & 0xf) == 2) {
            return 0;
        }
    }
}

static int lfs_dir_find(lfs_t *lfs, lfs_dir_t *dir,
        const char *path, lfs_entry_t *entry) {
    // TODO follow directories
    lfs_size_t pathlen = strcspn(path, "/");
    while (true) {
        int err = lfs_dir_next(lfs, dir, entry);
        if (err) {
            return err;
        }

        if (entry->d.len - sizeof(entry->d) != pathlen) {
            continue;
        }

        int ret = lfs_bd_cmp(lfs, entry->dir[0],
                entry->off + sizeof(entry->d), pathlen, path);
        if (ret < 0) {
            return ret;
        }

        // Found match
        if (ret == true) {
            return 0;
        }
    }
}

static int lfs_dir_append(lfs_t *lfs, lfs_dir_t *dir,
        const char *path, lfs_entry_t *entry) {
    int err = lfs_dir_find(lfs, dir, path, entry);
    if (err != LFS_ERROR_NO_ENTRY) {
        return err ? err : LFS_ERROR_EXISTS;
    }

    // Check if we fit
    if (dir->d.size + strlen(path) > lfs->block_size - 4) {
        return -1; // TODO make fit
    }

    return 0;
}

int lfs_mkdir(lfs_t *lfs, const char *path) {
    // Allocate entry for directory
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->cwd);
    if (err) {
        return err;
    }

    lfs_entry_t entry;
    err = lfs_dir_append(lfs, &cwd, path, &entry);
    if (err) {
        return err;
    }

    // Build up new directory
    lfs_dir_t dir;
    err = lfs_dir_create(lfs, &dir, cwd.pair);
    if (err) {
        return err;
    }

    entry.d.type = LFS_TYPE_DIR;
    entry.d.len = sizeof(entry.d) + strlen(path);
    entry.d.u.dir[0] = dir.pair[0];
    entry.d.u.dir[1] = dir.pair[1];

    cwd.d.rev += 1;
    cwd.d.size += entry.d.len;
    cwd.d.free = lfs->free;

    return lfs_pair_commit(lfs, entry.dir,
        3, (struct lfs_commit_region[3]) {
            {0, sizeof(cwd.d), &cwd.d},
            {entry.off, sizeof(entry.d), &entry.d},
            {entry.off+sizeof(entry.d), entry.d.len - sizeof(entry.d), path}
        });
}

int lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
    int err = lfs_dir_fetch(lfs, dir, lfs->cwd);
    if (err) {
        return err;
    }
    
    lfs_entry_t entry;
    err = lfs_dir_find(lfs, dir, path, &entry);
    if (err) {
        return err;
    } else if (entry.d.type != LFS_TYPE_DIR) {
        return LFS_ERROR_NOT_DIR;
    }

    return lfs_dir_fetch(lfs, dir, entry.d.u.dir);
}

int lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
    // Do nothing, dir is always synchronized
    return 0;
}


/// File operations ///

int lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    // Allocate entry for file if it doesn't exist
    // TODO check open files
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->cwd);
    if (err) {
        return err;
    }

    if (flags & LFS_O_CREAT) {
        err = lfs_dir_append(lfs, &cwd, path, &file->entry);
        if (err && err != LFS_ERROR_EXISTS) {
            return err;
        }
    } else {
        err = lfs_dir_find(lfs, &cwd, path, &file->entry);
        if (err) {
            return err;
        }
    }

    if ((flags & LFS_O_CREAT) && err != LFS_ERROR_EXISTS) {
        // Store file
        file->head = 0;
        file->size = 0;
        file->wblock = 0;
        file->windex = 0;
        file->rblock = 0;
        file->rindex = 0;
        file->roff = 0;

        file->entry.d.type = 1;
        file->entry.d.len = sizeof(file->entry.d) + strlen(path);
        file->entry.d.u.file.head = file->head;
        file->entry.d.u.file.size = file->size;

        cwd.d.rev += 1;
        cwd.d.size += file->entry.d.len;
        cwd.d.free = lfs->free;

        return lfs_pair_commit(lfs, file->entry.dir,
            3, (struct lfs_commit_region[3]) {
                {0, sizeof(cwd.d), &cwd.d},
                {file->entry.off,
                 sizeof(file->entry.d),
                 &file->entry.d},
                {file->entry.off+sizeof(file->entry.d),
                 file->entry.d.len-sizeof(file->entry.d),
                 path}
            });
    } else {
        file->head = file->entry.d.u.file.head;
        file->size = file->entry.d.u.file.size;
        file->windex = lfs_toindex(lfs, file->size);
        file->rblock = 0;
        file->rindex = 0;
        file->roff = 0;

        // TODO do this lazily in write?
        // TODO cow the head i/d block
        if (file->size < lfs->block_size) {
            file->wblock = file->head;
        } else {
            int err = lfs_index_find(lfs, file->head, file->windex,
                    file->windex, &file->wblock);
            if (err) {
                return err;
            }
        }

        return 0;
    }
}

int lfs_file_close(lfs_t *lfs, lfs_file_t *file) {
    // Store file
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, file->entry.dir);
    if (err) {
        return err;
    }

    file->entry.d.u.file.head = file->head;
    file->entry.d.u.file.size = file->size;

    cwd.d.rev += 1;
    cwd.d.free = lfs->free;

    return lfs_pair_commit(lfs, file->entry.dir,
        3, (struct lfs_commit_region[3]) {
            {0, sizeof(cwd.d), &cwd.d},
            {file->entry.off, sizeof(file->entry.d), &file->entry.d},
        });
}

lfs_ssize_t lfs_file_write(lfs_t *lfs, lfs_file_t *file,
        const void *buffer, lfs_size_t size) {
    const uint8_t *data = buffer;
    lfs_size_t nsize = size;

    while (nsize > 0) {
        lfs_off_t woff = file->size % lfs->block_size;

        if (file->size == 0) {
            int err = lfs_alloc_erased(lfs, &file->wblock);
            if (err) {
                return err;
            }

            file->head = file->wblock;
            file->windex = 0;
        } else if (woff == 0) {
            int err = lfs_alloc_erased(lfs, &file->wblock);
            if (err) {
                return err;
            }

            err = lfs_index_append(lfs, &file->head,
                    &file->windex, file->wblock);
            if (err) {
                return err;
            }
        }

        lfs_size_t diff = lfs_min(nsize, lfs->block_size - woff);
        int err = lfs_bd_prog(lfs, file->wblock, woff, diff, data);
        if (err) {
            return err;
        }

        file->size += diff;
        data += diff;
        nsize -= diff;
    }

    return size;
}

lfs_ssize_t lfs_file_read(lfs_t *lfs, lfs_file_t *file,
        void *buffer, lfs_size_t size) {
    uint8_t *data = buffer;
    lfs_size_t nsize = size;

    while (nsize > 0 && file->roff < file->size) {
        lfs_off_t roff = file->roff % lfs->block_size;

        // TODO cache index blocks
        if (file->size < lfs->block_size) {
            file->rblock = file->head;
        } else if (roff == 0) {
            int err = lfs_index_find(lfs, file->head, file->windex,
                    file->rindex, &file->rblock);
            if (err) {
                return err;
            }

            file->rindex = lfs_index_next(lfs, file->rindex);
        }

        lfs_size_t diff = lfs_min(
                lfs_min(nsize, file->size-file->roff),
                lfs->block_size - roff);
        int err = lfs_bd_read(lfs, file->rblock, roff, diff, data);
        if (err) {
            return err;
        }

        file->roff += diff;
        data += diff;
        nsize -= diff;
    }

    return size - nsize;
}


/// Generic filesystem operations ///
int lfs_format(lfs_t *lfs, lfs_bd_t *bd, const struct lfs_bd_ops *bd_ops) {
    lfs->bd = bd;
    lfs->bd_ops = bd_ops;

    struct lfs_bd_info info;
    int err = lfs_bd_info(lfs, &info);
    if (err) {
        return err;
    }

    lfs->read_size  = info.read_size;
    lfs->prog_size  = info.prog_size;
    lfs->block_size = info.erase_size;
    lfs->block_count = info.total_size / info.erase_size;
    lfs->words = info.erase_size / sizeof(uint32_t);

    // Create free list
    lfs->free.begin = 2;
    lfs->free.end = lfs->block_count;

    // Write root directory
    lfs_dir_t root;
    err = lfs_dir_create(lfs, &root, 0);
    if (err) {
        return err;
    }
    lfs->cwd[0] = root.pair[0];
    lfs->cwd[1] = root.pair[1];

    // Write superblocks
    lfs_superblock_t superblock = {
        .pair = {0, 1},
        .d.rev = 1,
        .d.size = sizeof(superblock),
        .d.root = {lfs->cwd[0], lfs->cwd[1]},
        .d.magic = {"littlefs"},
        .d.block_size  = lfs->block_size,
        .d.block_count = lfs->block_count,
    };

    for (int i = 0; i < 2; i++) {
        int err = lfs_pair_commit(lfs, superblock.pair,
                1, (struct lfs_commit_region[]){
                    {0, sizeof(superblock.d), &superblock.d}
                });
        if (err) {
            LFS_ERROR("Failed to write superblock at %d", superblock.pair[1]);
            return err;
        }

        uint32_t crc = 0xffffffff;
        err = lfs_bd_crc(lfs, superblock.pair[0], 0, lfs->block_size, &crc);
        if (err || crc != 0) {
            LFS_ERROR("Failed to write superblock at %d", superblock.pair[0]);
            return err ? err : LFS_ERROR_CORRUPT;
        }
    }

    return 0;
}

int lfs_mount(lfs_t *lfs, lfs_bd_t *bd, const struct lfs_bd_ops *bd_ops) {
    lfs->bd = bd;
    lfs->bd_ops = bd_ops;

    struct lfs_bd_info info;
    int err = lfs_bd_info(lfs, &info);
    if (err) {
        return err;
    }

    lfs->read_size  = info.read_size;
    lfs->prog_size  = info.prog_size;
    lfs->block_size = info.erase_size;
    lfs->block_count = info.total_size / info.erase_size;
    lfs->words = info.erase_size / sizeof(uint32_t);

    lfs_superblock_t superblock = {
        .pair = {0, 1},
    };
    err = lfs_pair_fetch(lfs, superblock.pair,
            1, (struct lfs_fetch_region[]){
                {0, sizeof(superblock.d), &superblock.d}
            });

    if ((err == LFS_ERROR_CORRUPT ||
            memcmp(superblock.d.magic, "littlefs", 8) != 0)) {
        LFS_ERROR("Invalid superblock at %d %d",
                superblock.pair[0], superblock.pair[1]);
        return LFS_ERROR_CORRUPT;
    }

    lfs->cwd[0] = superblock.d.root[0];
    lfs->cwd[1] = superblock.d.root[1];

    return err;
}

int lfs_unmount(lfs_t *lfs) {
    // No nothing for now
    return 0;
}
