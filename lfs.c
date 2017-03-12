/*
 * The little filesystem
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#include "lfs.h"

#include <string.h>
#include <stdbool.h>


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

static lfs_error_t lfs_alloc(lfs_t *lfs, lfs_ino_t *ino);
static lfs_error_t lfs_free(lfs_t *lfs, lfs_ino_t ino);


// Next index offset
static lfs_off_t lfs_inext(lfs_t *lfs, lfs_off_t ioff) {
    ioff += 1;

    lfs_size_t wcount = lfs->info.erase_size/4;
    while (ioff % wcount == 0) {
        ioff += lfs_min(lfs_ctz(ioff/wcount + 1), wcount-1) + 1;
    }

    return ioff;
}

// Find index in index chain given its index offset
static lfs_error_t lfs_ifind(lfs_t *lfs, lfs_ino_t head,
        lfs_size_t icount, lfs_off_t ioff, lfs_ino_t *ino) {
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

        lfs_off_t skips = lfs_min(lfs_ctz(ioff/wcount + 1), wcount-1) + 1;
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
    // TODO save slot for freeing?
    lfs_error_t err = lfs_ifind(lfs, lfs->free.d.head,
            lfs->free.d.icount, lfs->free.d.ioff, ino);
    if (err) {
        return err;
    }

    lfs->free.d.ioff = lfs_inext(lfs, lfs->free.d.ioff);

    return lfs->ops->erase(lfs->bd, *ino, 0, lfs->info.erase_size);
}

static lfs_error_t lfs_free(lfs_t *lfs, lfs_ino_t ino) {
    return lfs_iappend(lfs, &lfs->free.d.head, &lfs->free.d.icount, ino);
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

lfs_error_t lfs_block_load(lfs_t *lfs,
        const lfs_ino_t pair[2], lfs_ino_t *ino) {
    lfs_word_t rev[2];
    for (int i = 0; i < 2; i++) {
        int err = lfs->ops->read(lfs->bd, (void*)&rev[i], pair[i], 0, 4);
        if (err) {
            return err;
        }
    }

    for (int i = 0; i < 2; i++) {
        lfs_ino_t check = pair[(rev[1] > rev[0]) ? 1-i : i];
        int err = lfs_check(lfs, check);
        if (err == LFS_ERROR_CORRUPT) {
            continue;
        } else if (err) {
            return err;
        }

        return check;
    }

    LFS_ERROR("Corrupted dir at %d %d", pair[0], pair[1]);
    return LFS_ERROR_CORRUPT;
}

struct lfs_read_region {
    lfs_off_t off;
    lfs_size_t size;
    void *data;
};

lfs_error_t lfs_pair_read(lfs_t *lfs, lfs_ino_t pair[2],
        int count, const struct lfs_read_region *regions) {
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
        if (checked > 0 && rev > nrev) {
            continue;
        }

        err = lfs_check(lfs, pair[i]);
        if (err == LFS_ERROR_CORRUPT) {
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

struct lfs_write_region {
    lfs_off_t off;
    lfs_size_t size;
    const void *data;
};

lfs_error_t lfs_pair_write(lfs_t *lfs, lfs_ino_t pair[2],
        int count, const struct lfs_write_region *regions) {
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

static lfs_error_t lfs_dir_make(lfs_t *lfs, lfs_dir_t *dir) {
    // Allocate pair of dir blocks
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->pair[i]);
        if (err) {
            return err;
        }
    }

    // Rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs->ops->read(lfs->bd, (void*)&dir->d.rev,
            dir->pair[1], 0, 4);
    if (err) {
        return err;
    }
    dir->d.rev += 1;

    // Other defaults
    dir->d.size = sizeof(struct lfs_disk_dir);
    dir->d.tail[0] = 0;
    dir->d.tail[1] = 0;
    dir->d.parent[0] = 0;
    dir->d.parent[1] = 0;

    // TODO sort this out
    dir->d.free = lfs->free.d;

    // Write out to memory
    return lfs_pair_write(lfs, dir->pair,
        1, (struct lfs_write_region[1]){
            {0, sizeof(dir->d), &dir->d}
        });
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

    {   // Create free list
        lfs->free = (lfs_free_t){
            .d.head = 2,
            .d.ioff = 1,
            .d.icount = 1,
            .d.rev = 1,
        };

        lfs_size_t block_count = lfs->info.total_size / lfs->info.erase_size;
        for (lfs_ino_t i = 3; i < block_count; i++) {
            lfs_error_t err = lfs_free(lfs, i);
            if (err) {
                return err;
            }
        }
    }

    lfs_dir_t root;
    {
        // Write root directory
        int err = lfs_dir_make(lfs, &root);
        if (err) {
            return err;
        }
    }

    {
        // Write superblocks
        lfs_superblock_t superblock = {
            .pair = {0, 1},
            .d.rev = 1,
            .d.size = sizeof(struct lfs_disk_superblock),
            .d.root = {root.pair[0], root.pair[1]},
            .d.magic = {"littlefs"},
            .d.block_size = info.erase_size,
            .d.block_count = info.total_size / info.erase_size,
        };

        for (int i = 0; i < 2; i++) {
            lfs_ino_t block = superblock.pair[0];
            int err = lfs_pair_write(lfs, superblock.pair,
                    1, (struct lfs_write_region[1]){
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

