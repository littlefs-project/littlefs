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

// Disk structures
//lfs_disk_struct lfs_disk_free {
//    lfs_ino_t head;
//    uint32_t ioff;
//    uint32_t icount;
//    uint32_t rev;
//};
//
//lfs_disk_struct lfs_disk_dir {
//    uint32_t rev;
//    uint32_t count;
//    lfs_ino_t tail[2];
//    struct lfs_disk_free free;
//};
//
//lfs_disk_struct lfs_disk_dirent {
//    uint16_t type;
//    uint16_t len;
//};
//
//lfs_disk_struct lfs_disk_superblock {
//    struct lfs_disk_dir dir;
//    struct lfs_disk_dirent header;
//    char magic[4];
//    uint32_t read_size;
//    uint32_t write_size;
//    uint32_t erase_size;
//    uint32_t erase_count;
//};
//
//lfs_disk_struct lfs_disk_dirent_file {
//    struct lfs_disk_dirent header;
//    lfs_ino_t head;
//    lfs_size_t size;
//    char name[LFS_NAME_MAX];
//};
//
//lfs_disk_struct lfs_disk_dirent_dir {
//    struct lfs_disk_dirent header;
//    lfs_ino_t ino[2];
//    char name[LFS_NAME_MAX];
//};


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

// create a dir
// create entry
// update entry


//static lfs_error_t lfs_dir_alloc(lfs_t *lfs, lfs_dir_t *dir);
//static lfs_error_t lfs_dir_update(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry);
//static lfs_error_t lfs_dir_destroy(lfs_t *lfs, lfs_dir_t *dir);
//static lfs_error_t lfs_entry_alloc(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry);
//static lfs_error_t lfs_entry_update(lfs_t *lfs, lfs_entry_t *entry);
//static lfs_error_t lfs_entry_destroy(lfs_t *lfs, lfs_dir_t *dir);

// Directory operations
static lfs_error_t lfs_dir_alloc(lfs_t *lfs, lfs_dir_t *dir) {
    // Allocate pair of dir blocks
    for (int i = 0; i < 2; i++) {
        int err = lfs_alloc(lfs, &dir->dno[i]);
        if (err) {
            return err;
        }
    }

    // Rather than clobbering one of the blocks we just pretend
    // the revision may be valid
    int err = lfs->ops->read(lfs->bd, (void*)&dir->d.rev, dir->dno[1], 0, 4);
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
    return 0;
}

lfs_error_t lfs_dir_update(lfs_t *lfs, lfs_dir_t *dir, lfs_entry_t *entry) {
    // TODO flush this
    dir->d.free = lfs->free.d;

    // Start by erasing target block
    int err = lfs->ops->erase(lfs->bd, dir->dno[0], 0, lfs->info.erase_size);

    // Write header and start calculating crc
    uint32_t crc = lfs_crc((void*)&dir->d, sizeof(dir->d), 0xffffffff);
    err = lfs->ops->write(lfs->bd, (void*)&dir->d,
        dir->dno[0], 0, sizeof(dir->d));
    if (err) {
        return err;
    }

    // Copy over entries and write optional entry update
    // TODO handle optional entry
    for (lfs_off_t i = sizeof(dir->d); i < lfs->info.erase_size-4; i += 4) {
        uint32_t data;
        err = lfs->ops->read(lfs->bd, (void*)&data, dir->dno[1], i, 4);
        if (err) {
            return err;
        }

        crc = lfs_crc((void*)&data, 4, crc);
        err = lfs->ops->write(lfs->bd, (void*)&data, dir->dno[0], i, 4);
        if (err) {
            return err;
        }
    }

    // Write resulting crc
    err = lfs->ops->write(lfs->bd, (void*)&crc,
            dir->dno[0], lfs->info.erase_size-4, 4);
    if (err) {
        return err;
    }

    // Flip dnos to indicate next write of the dir pair
    lfs_ino_t temp = dir->dno[0];
    dir->dno[0] = dir->dno[1];
    dir->dno[1] = temp;

    return 0;
}


//static lfs_error_t lfs_dir_alloc(lfs_t *lfs, lfs_dir_t *dir) {
//    memset(dir, 0, sizeof(lfs_dir_t));
//
//    for (int i = 0; i < 2; i++) {
//        int err = lfs_alloc(lfs, &dir->dno[i]);
//        if (err) {
//            return err;
//        }
//    }
//
//    // Rather than clobbering one of the blocks we just pretend
//    // the revision may be valid
//    int err = lfs->ops->read(lfs->bd, (void*)&dir->rev, dir->dno[1], 0, 4);
//    if (err) {
//        return err;
//    }
//    dir->rev += 1;
//
//    // TODO move this to a flush of some sort?
//    struct lfs_disk_dir disk_dir = {
//        .rev = dir->rev,
//        .count = dir->len,
//        .tail[0] = dir->tail[0],
//        .tail[1] = dir->tail[1],
//        .free.head = lfs->free.head,
//        .free.ioff = lfs->free.ioff,
//        .free.icount = lfs->free.icount,
//        .free.rev = lfs->free.rev,
//    };
//
//    err = lfs->ops->write(lfs->bd, (void*)&disk_dir,
//        dir->dno[0], 0, sizeof(struct lfs_disk_dir));
//    if (err) {
//        return err;
//    }
//
//    uint32_t crc = 0xffffffff;
//    for (lfs_off_t i = 0; i < lfs->info.erase_size-4; i += 4) {
//        uint32_t data;
//        err = lfs->ops->read(lfs->bd, (void*)&data, dir->dno[0], i, 4);
//        if (err) {
//            return err;
//        }
//
//        crc = lfs_crc((void*)&data, 4, crc);
//    }
//
//    err = lfs->ops->write(lfs->bd, (void*)&crc,
//            dir->dno[0], lfs->info.erase_size-4, 4);
//    if (err) {
//        return err;
//    }
//
//    lfs_ino_t temp = dir->dno[0];
//    dir->dno[0] = dir->dno[1];
//    dir->dno[1] = temp;
//
//    return 0;
//}

//lfs_error_t lfs_dir_update(lfs_t *lfs, lfs_dir_t *dir,
//        lfs_dirent_t *ent, const char *name) {
//
//
//
//    struct lfs_disk_dir disk_dir = {
//        .rev = dir->rev,
//        .count = dir->len,
//        .tail[0] = dir->tail[0],
//        .tail[1] = dir->tail[1],
//        // TODO flush this?
//        .free.head = lfs->free.head,
//        .free.ioff = lfs->free.ioff,
//        .free.icount = lfs->free.icount,
//        .free.rev = lfs->free.rev,
//    };
//
//    err = lfs->ops->write(lfs->bd, (void*)&disk_dir,
//        dir->dno[0], 0, sizeof(struct lfs_disk_dir));
//    if (err) {
//        return err;
//    }
//
//    if (ent) {
//        // TODO update entry
//    }
//
//    uint32_t crc = 0xffffffff;
//    for (lfs_off_t i = 0; i < lfs->info.erase_size-4; i += 4) {
//        uint32_t data;
//        err = lfs->ops->read(lfs->bd, (void*)&data, dir->dno[0], i, 4);
//        if (err) {
//            return err;
//        }
//
//        crc = lfs_crc((void*)&data, 4, crc);
//    }
//
//    err = lfs->ops->write(lfs->bd, (void*)&crc,
//            dir->dno[0], lfs->info.erase_size-4, 4);
//    if (err) {
//        return err;
//    }
//
//    lfs_ino_t temp = dir->dno[0];
//    dir->dno[0] = dir->dno[1];
//    dir->dno[1] = temp;
//
//    return 0;
//}


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

    err = lfs->ops->erase(lfs->bd, 0, 0, 5*info.erase_size);
    if (err) {
        return err;
    }

    // TODO make sure that erase clobbered blocks

    {   // Create free list
        lfs->free.d.head = 4;
        lfs->free.d.ioff = 1;
        lfs->free.d.icount = 1;
        lfs->free.d.rev = 1;

        lfs_size_t block_count = lfs->info.total_size / lfs->info.erase_size;
        for (lfs_ino_t i = 5; i < block_count; i++) {
            lfs_error_t err = lfs_free(lfs, i);
            if (err) {
                return err;
            }
        }
    }

    lfs_dir_t root;
    {
        // Write root directory
        int err = lfs_dir_alloc(lfs, &root);
        if (err) {
            return err;
        }

        err = lfs_dir_update(lfs, &root, 0);
        if (err) {
            return err;
        }
    }

    {
        // Write superblocks
        lfs_ino_t sno[2] = {0, 1};
        lfs_superblock_t superblock = {
            .d.rev = 1,
            .d.count = 0,
            .d.root = {root.dno[0], root.dno[1]},
            .d.magic = {"littlefs"},
            .d.block_size = info.erase_size,
            .d.block_count = info.total_size / info.erase_size,
        };

        for (int i = 0; i < 2; i++) {
            err = lfs->ops->erase(lfs->bd, sno[i], 0, info.erase_size);
            if (err) {
                return err;
            }

            err = lfs->ops->write(lfs->bd, (void*)&superblock.d,
                    sno[i], 0, sizeof(superblock.d));
            if (err) {
                return err;
            }

            uint32_t crc = lfs_crc((void*)&superblock.d,
                    sizeof(superblock.d), 0xffffffff);

            for (lfs_size_t i = sizeof(superblock);
                    i < info.erase_size-4; i += 4) {
                uint32_t data;
                err = lfs->ops->read(lfs->bd, (void*)&data, 0, i, 4);
                if (err) {
                    return err;
                }

                crc = lfs_crc((void*)&data, 4, crc);
            }

            err = lfs->ops->write(lfs->bd, (void*)&crc,
                    sno[i], info.erase_size-4, 4);
            if (err) {
                return err;
            }
        }

        // TODO verify superblocks written correctly
    }


    // Sanity check
    uint32_t crc = 0xffffffff;
    for (lfs_size_t i = 0; i < info.erase_size; i += 4) {
        uint32_t data;
        err = lfs->ops->read(lfs->bd, (void*)&data, 0, i, 4);
        if (err) {
            return err;
        }

        crc = lfs_crc((void*)&data, 4, crc);
    }

    uint32_t data;
    err = lfs->ops->read(lfs->bd, (void*)&data, 0, info.erase_size-4, 4);
    if (err) {
        return err;
    }

    return crc;
}

