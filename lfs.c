/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#include "lfs.h"

#include <string.h>
#include <stdbool.h>


static int lfs_diff(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}

static uint32_t lfs_crc(const uint8_t *data, lfs_size_t size, uint32_t crc) {
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
        0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
        0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };

    for (lfs_size_t i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }

    return crc;
}

static lfs_error_t lfs_bd_cmp(lfs_t *lfs,
        lfs_ino_t ino, lfs_off_t off, lfs_size_t size, const void *d) {
    const uint8_t *data = d;

    for (int i = 0; i < size; i++) {
        uint8_t c;
        int err = lfs->ops->read(lfs->bd, (void*)&c, ino, off + i, 1);
        if (err) {
            return err;
        }

        if (c != data[i]) {
            return false;
        }
    }

    return true;
}



static lfs_error_t lfs_alloc(lfs_t *lfs, lfs_ino_t *ino);


// Next index offset
static lfs_off_t lfs_inext(lfs_t *lfs, lfs_off_t ioff) {
    ioff += 1;

    lfs_size_t wcount = lfs->info.erase_size/4;
    while (ioff % wcount == 0) {
        ioff += lfs_min(lfs_ctz(ioff/wcount + 1), wcount-1) + 1;
    }

    return ioff;
}

static lfs_off_t lfs_toindex(lfs_t *lfs, lfs_off_t off) {
    lfs_off_t i = 0;
    while (off > 512) {
        i = lfs_inext(lfs, i);
        off -= 512;
    }

    return i;
}

// Find index in index chain given its index offset
static lfs_error_t lfs_ifind_block(lfs_t *lfs, lfs_ino_t head,
        lfs_size_t icount, lfs_off_t ioff, lfs_ino_t *block) {
    lfs_size_t wcount = lfs->info.erase_size/4;
    lfs_off_t iitarget = ioff / wcount;
    lfs_off_t iicurrent = (icount-1) / wcount;

    while (iitarget != iicurrent) {
        lfs_size_t skip = lfs_min(
                lfs_min(lfs_ctz(iicurrent+1), wcount-1),
                lfs_npw2((iitarget ^ iicurrent)+1)-1);
        
        lfs_error_t err = lfs->ops->read(lfs->bd, (void*)&head,
                head, 4*skip, 4);
        if (err) {
            return err;
        }

        iicurrent -= 1 << skip;
    }

    *block = head;
    return 0;
}

static lfs_error_t lfs_ifind(lfs_t *lfs, lfs_ino_t head,
        lfs_size_t icount, lfs_off_t ioff, lfs_ino_t *ino) {
    lfs_size_t wcount = lfs->info.erase_size/4;
    int err = lfs_ifind_block(lfs, head, icount, ioff, &head);
    if (err) {
        return err;
    }

    return lfs->ops->read(lfs->bd, (void*)ino, head, 4*(ioff % wcount), 4);
}

// Append index to index chain, updates head and icount
static lfs_error_t lfs_iappend(lfs_t *lfs, lfs_ino_t *headp,
        lfs_size_t *icountp, lfs_ino_t ino) {
    lfs_ino_t head = *headp;
    lfs_size_t ioff = *icountp - 1;
    lfs_size_t wcount = lfs->info.erase_size/4;

    ioff += 1;

    while (ioff % wcount == 0) {
        lfs_ino_t nhead;
        lfs_error_t err = lfs_alloc(lfs, &nhead);
        if (err) {
            return err;
        }

        lfs_off_t skips = lfs_min(lfs_ctz(ioff/wcount + 1), wcount-2) + 1;
        for (lfs_off_t i = 0; i < skips; i++) {
            err = lfs->ops->write(lfs->bd, (void*)&head, nhead, 4*i, 4);
            if (err) {
                return err;
            }

            if (head && i != skips-1) {
                err = lfs->ops->read(lfs->bd, (void*)&head, head, 4*i, 4);
                if (err) {
                    return err;
                }
            }
        }

        ioff += skips;
        head = nhead;
    }

    lfs_error_t err = lfs->ops->write(lfs->bd, (void*)&ino,
            head, 4*(ioff % wcount), 4);
    if (err) {
        return err;
    }

    *headp = head;
    *icountp = ioff + 1;
    return 0;
}

// Memory managment
static lfs_error_t lfs_alloc(lfs_t *lfs, lfs_ino_t *ino) {
    if (lfs->free.d.begin != lfs->free.d.end) {
        *ino = lfs->free.d.begin;
        lfs->free.d.begin += 1;

        return lfs->ops->erase(lfs->bd, *ino, 0, lfs->info.erase_size);
    }

    // TODO find next stride of free blocks
    // TODO verify no strides exist where begin > current begin
    // note: begin = 0 is invalid (superblock)
    return LFS_ERROR_NO_SPACE;
}

lfs_error_t lfs_check(lfs_t *lfs, lfs_ino_t block) {
    uint32_t crc = 0xffffffff;

    for (lfs_size_t i = 0; i < lfs->info.erase_size; i += 4) {
        uint32_t data;
        int err = lfs->ops->read(lfs->bd, (void*)&data, block, i, 4);
        if (err) {
            return err;
        }

        crc = lfs_crc((void*)&data, 4, crc);
    }

    return (crc != 0) ? LFS_ERROR_CORRUPT : LFS_ERROR_OK;
}

struct lfs_fetch_region {
    lfs_off_t off;
    lfs_size_t size;
    void *data;
};

lfs_error_t lfs_pair_fetch(lfs_t *lfs, lfs_ino_t pair[2],
        int count, const struct lfs_fetch_region *regions) {
    int checked = 0;
    int rev = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t nrev;
        int err = lfs->ops->read(lfs->bd, (void*)&nrev,
                pair[0], 0, 4);
        if (err) {
            return err;
        }

        // TODO diff these
        if (checked > 0 && lfs_diff(nrev, rev) < 0) {
            continue;
        }

        err = lfs_check(lfs, pair[0]);
        if (err == LFS_ERROR_CORRUPT) {
            lfs_swap(&pair[0], &pair[1]);
            continue;
        } else if (err) {
            return err;
        }

        checked += 1;
        rev = nrev;
        lfs_swap(&pair[0], &pair[1]);
    }

    if (checked == 0) {
        return LFS_ERROR_CORRUPT;
    }

    for (int i = 0; i < count; i++) {
        int err = lfs->ops->read(lfs->bd, regions[i].data,
                pair[1], regions[i].off, regions[i].size);
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

lfs_error_t lfs_pair_commit(lfs_t *lfs, lfs_ino_t pair[2],
        int count, const struct lfs_commit_region *regions) {
    uint32_t crc = 0xffffffff;
    int err = lfs->ops->erase(lfs->bd,
            pair[0], 0, lfs->info.erase_size);
    if (err) {
        return err;
    }

    lfs_off_t off = 0;
    while (off < lfs->info.erase_size - 4) {
        if (count > 0 && regions[0].off == off) {
            crc = lfs_crc(regions[0].data, regions[0].size, crc);
            int err = lfs->ops->write(lfs->bd, regions[0].data,
                    pair[0], off, regions[0].size);
            if (err) {
                return err;
            }

            off += regions[0].size;
            count -= 1;
            regions += 1;
        } else {
            // TODO faster strides?
            uint8_t data;
            int err = lfs->ops->read(lfs->bd, (void*)&data,
                    pair[1], off, sizeof(data));
            if (err) {
                return err;
            }

            crc = lfs_crc((void*)&data, sizeof(data), crc);
            err = lfs->ops->write(lfs->bd, (void*)&data,
                    pair[0], off, sizeof(data));
            if (err) {
                return err;
            }

            off += sizeof(data);
        }
    }

    err = lfs->ops->write(lfs->bd, (void*)&crc,
            pair[0], lfs->info.erase_size-4, 4);
    if (err) {
        return err;
    }

    lfs_swap(&pair[0], &pair[1]);
    return 0;
}

lfs_error_t lfs_dir_make(lfs_t *lfs, lfs_dir_t *dir, lfs_ino_t parent[2]) {
    // Allocate pair of dir blocks
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[i]);
        if (err) {
            return err;
        }
    }

    // Rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs->ops->read(lfs->bd, (void*)&dir->d.rev, dir->pair[1], 0, 4);
    if (err) {
        return err;
    }
    dir->d.rev += 1;

    // Other defaults
    dir->i = sizeof(struct lfs_disk_dir);
    dir->d.size = sizeof(struct lfs_disk_dir);
    dir->d.tail[0] = 0;
    dir->d.tail[1] = 0;
    dir->d.free = lfs->free.d;

    if (parent) {
        // Create '..' entry
        lfs_entry_t entry = {
            .d.type = LFS_TYPE_DIR,
            .d.len = sizeof(entry.d) + 2,
            .d.u.dir[0] = parent[0],
            .d.u.dir[1] = parent[1],
        };

        dir->d.size += entry.d.len;

        // Write out to memory
        return lfs_pair_commit(lfs, dir->pair,
            3, (struct lfs_commit_region[3]){
                {0, sizeof(dir->d), &dir->d},
                {sizeof(dir->d), sizeof(entry.d), &entry.d},
                {sizeof(dir->d)+sizeof(entry.d), 2, ".."},
            });
    } else {
        return lfs_pair_commit(lfs, dir->pair,
            1, (struct lfs_commit_region[1]){
                {0, sizeof(dir->d), &dir->d},
            });
    }
}

lfs_error_t lfs_dir_fetch(lfs_t *lfs, lfs_dir_t *dir, lfs_ino_t pair[2]) {
    dir->pair[0] = pair[0];
    dir->pair[1] = pair[1];
    dir->i = sizeof(dir->d);

    int err = lfs_pair_fetch(lfs, dir->pair,
        1, (struct lfs_fetch_region[1]) {
            {0, sizeof(dir->d), &dir->d}
        });

    if (err == LFS_ERROR_CORRUPT) {
        LFS_ERROR("Corrupted dir at %d %d", pair[0], pair[1]);
    }

    return err;
}

lfs_error_t lfs_dir_next(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry) {
    while (true) {
        // TODO iterate down list
        entry->dir[0] = dir->pair[0];
        entry->dir[1] = dir->pair[1];
        entry->off = dir->i;

        if (dir->d.size - dir->i < sizeof(entry->d)) {
            return LFS_ERROR_NO_ENTRY;
        }

        int err = lfs->ops->read(lfs->bd, (void*)&entry->d,
                dir->pair[1], dir->i, sizeof(entry->d));
        if (err) {
            return err;
        }

        dir->i += entry->d.len;

        // Skip any unknown entries
        if (entry->d.type == 1 || entry->d.type == 2) {
            return 0;
        }
    }
}

lfs_error_t lfs_dir_find(lfs_t *lfs, lfs_dir_t *dir,
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

        int ret = lfs_bd_cmp(lfs, entry->dir[1],
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

lfs_error_t lfs_dir_alloc(lfs_t *lfs, lfs_dir_t *dir,
        const char *path, lfs_entry_t *entry, uint16_t len) {
    int err = lfs_dir_find(lfs, dir, path, entry);
    if (err != LFS_ERROR_NO_ENTRY) {
        return err ? err : LFS_ERROR_EXISTS;
    }

    // Check if we fit
    if (dir->d.size + len > lfs->info.erase_size - 4) {
        return -1; // TODO make fit
    }

    return 0;
}

lfs_error_t lfs_dir_open(lfs_t *lfs, lfs_dir_t *dir, const char *path) {
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

lfs_error_t lfs_dir_close(lfs_t *lfs, lfs_dir_t *dir) {
    // Do nothing, dir is always synchronized
    return 0;
}

lfs_error_t lfs_mkdir(lfs_t *lfs, const char *path) {
    // Allocate entry for directory
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->cwd);
    if (err) {
        return err;
    }

    lfs_entry_t entry;
    err = lfs_dir_alloc(lfs, &cwd, path,
            &entry, sizeof(entry.d)+strlen(path));
    if (err) {
        return err;
    }

    // Build up new directory
    lfs_dir_t dir;
    err = lfs_dir_make(lfs, &dir, cwd.pair); // TODO correct parent?
    if (err) {
        return err;
    }

    entry.d.type = 2;
    entry.d.len = sizeof(entry.d) + strlen(path);
    entry.d.u.dir[0] = dir.pair[0];
    entry.d.u.dir[1] = dir.pair[1];

    cwd.d.rev += 1;
    cwd.d.size += entry.d.len;
    cwd.d.free = lfs->free.d;

    return lfs_pair_commit(lfs, entry.dir,
        3, (struct lfs_commit_region[3]) {
            {0, sizeof(cwd.d), &cwd.d},
            {entry.off, sizeof(entry.d), &entry.d},
            {entry.off+sizeof(entry.d), entry.d.len - sizeof(entry.d), path}
        });
}

lfs_error_t lfs_file_open(lfs_t *lfs, lfs_file_t *file,
        const char *path, int flags) {
    // Allocate entry for file if it doesn't exist
    // TODO check open files
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, lfs->cwd);
    if (err) {
        return err;
    }

    if (flags & LFS_O_CREAT) {
        err = lfs_dir_alloc(lfs, &cwd, path,
                &file->entry, sizeof(file->entry.d)+strlen(path));
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
        cwd.d.free = lfs->free.d;

        return lfs_pair_commit(lfs, file->entry.dir,
            3, (struct lfs_commit_region[3]) {
                {0, sizeof(cwd.d), &cwd.d},
                {file->entry.off, sizeof(file->entry.d),
                        &file->entry.d},
                {file->entry.off+sizeof(file->entry.d),
                        file->entry.d.len-sizeof(file->entry.d), path}
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
        if (file->size < lfs->info.erase_size) {
            file->wblock = file->head;
        } else {
            int err = lfs_ifind(lfs, file->head, file->windex,
                    file->windex, &file->wblock);
            if (err) {
                return err;
            }
        }

        return 0;
    }
}

lfs_error_t lfs_file_close(lfs_t *lfs, lfs_file_t *file) {
    // Store file
    lfs_dir_t cwd;
    int err = lfs_dir_fetch(lfs, &cwd, file->entry.dir);
    if (err) {
        return err;
    }

    file->entry.d.u.file.head = file->head;
    file->entry.d.u.file.size = file->size;

    cwd.d.rev += 1;
    cwd.d.free = lfs->free.d;

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
        lfs_off_t woff = file->size % lfs->info.erase_size;

        if (file->size == 0) {
            int err = lfs_alloc(lfs, &file->wblock);
            if (err) {
                return err;
            }

            file->head = file->wblock;
            file->windex = 0;
        } else if (woff == 0) {
            // TODO check that 2 blocks are available
            // TODO check for available blocks for backing up scratch files?
            int err = lfs_alloc(lfs, &file->wblock);
            if (err) {
                return err;
            }

            err = lfs_iappend(lfs, &file->head, &file->windex, file->wblock);
            if (err) {
                return err;
            }
        }

        lfs_size_t diff = lfs_min(nsize, lfs->info.erase_size - woff);
        int err = lfs->ops->write(lfs->bd, data, file->wblock, woff, diff);
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
        lfs_off_t roff = file->roff % lfs->info.erase_size;

        // TODO cache index blocks
        if (file->size < lfs->info.erase_size) {
            file->rblock = file->head;
        } else if (roff == 0) {
            int err = lfs_ifind(lfs, file->head, file->windex,
                    file->rindex, &file->rblock);
            if (err) {
                return err;
            }

            file->rindex = lfs_inext(lfs, file->rindex);
        }

        lfs_size_t diff = lfs_min(
                lfs_min(nsize, file->size-file->roff),
                lfs->info.erase_size - roff);
        int err = lfs->ops->read(lfs->bd, data, file->rblock, roff, diff);
        if (err) {
            return err;
        }

        file->roff += diff;
        data += diff;
        nsize -= diff;
    }

    return size - nsize;
}



// Little filesystem operations
lfs_error_t lfs_create(lfs_t *lfs, lfs_bd_t *bd, const struct lfs_bd_ops *ops) {
    lfs->bd = bd;
    lfs->ops = ops;

    lfs_error_t err = lfs->ops->info(lfs->bd, &lfs->info);
    if (err) {
        return err;
    }
    
    return 0;
}

lfs_error_t lfs_format(lfs_t *lfs) {
    struct lfs_bd_info info;
    lfs_error_t err = lfs->ops->info(lfs->bd, &info);
    if (err) {
        return err;
    }

    err = lfs->ops->erase(lfs->bd, 0, 0, 3*info.erase_size);
    if (err) {
        return err;
    }

    // TODO make sure that erase clobbered blocks

    {
        lfs_size_t block_count = lfs->info.total_size / lfs->info.erase_size;

        // Create free list
        lfs->free = (lfs_free_t){
            .d.begin = 2,
            .d.end = block_count,
        };
    }

    {
        // Write root directory
        lfs_dir_t root;
        int err = lfs_dir_make(lfs, &root, 0);
        if (err) {
            return err;
        }

        lfs->cwd[0] = root.pair[0];
        lfs->cwd[1] = root.pair[1];
    }

    {
        // Write superblocks
        lfs_superblock_t superblock = {
            .pair = {0, 1},
            .d.rev = 1,
            .d.size = sizeof(struct lfs_disk_superblock),
            .d.root = {lfs->cwd[0], lfs->cwd[1]},
            .d.magic = {"littlefs"},
            .d.block_size = info.erase_size,
            .d.block_count = info.total_size / info.erase_size,
        };

        for (int i = 0; i < 2; i++) {
            lfs_ino_t block = superblock.pair[0];
            int err = lfs_pair_commit(lfs, superblock.pair,
                    1, (struct lfs_commit_region[1]){
                        {0, sizeof(superblock.d), &superblock.d}
                    });

            err = lfs_check(lfs, block);
            if (err) {
                LFS_ERROR("Failed to write superblock at %d", block);
                return err;
            }
        }
    }

    return 0;
}

lfs_error_t lfs_mount(lfs_t *lfs) {
    struct lfs_bd_info info;
    lfs_error_t err = lfs->ops->info(lfs->bd, &info);
    if (err) {
        return err;
    }

    lfs_superblock_t superblock;
    err = lfs_pair_fetch(lfs,
            (lfs_ino_t[2]){0, 1},
            1, (struct lfs_fetch_region[1]){
                {0, sizeof(superblock.d), &superblock.d}
            });

    if ((err == LFS_ERROR_CORRUPT ||
            memcmp(superblock.d.magic, "littlefs", 8) != 0)) {
        LFS_ERROR("Invalid superblock at %d %d\n", 0, 1);
        return LFS_ERROR_CORRUPT;
    }

    printf("superblock %d %d\n",
            superblock.d.block_size,
            superblock.d.block_count);

    lfs->cwd[0] = superblock.d.root[0];
    lfs->cwd[1] = superblock.d.root[1];

    return err;
}
