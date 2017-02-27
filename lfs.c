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
    lfs_error_t err = lfs_ifind(lfs, lfs->free.head,
            lfs->free.icount, lfs->free.ioff, ino);
    if (err) {
        return err;
    }

    lfs->free.ioff = lfs_inext(lfs, lfs->free.ioff);

    return lfs->ops->erase(lfs->bd, *ino, 0, lfs->info.erase_size);
}

static lfs_error_t lfs_free(lfs_t *lfs, lfs_ino_t ino) {
    return lfs_iappend(lfs, &lfs->free.head, &lfs->free.icount, ino);
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

    err = lfs->ops->erase(lfs->bd, 0, 0, 5*info.erase_size);
    if (err) {
        return err;
    }

    // TODO make sure that erase clobbered blocks

    {   // Create free list
        lfs->free.head = 4;
        lfs->free.ioff = 1;
        lfs->free.icount = 1;
        lfs->free.rev = 1;

        lfs_size_t block_count = lfs->info.total_size / lfs->info.erase_size;
        for (lfs_ino_t i = 5; i < block_count; i++) {
            lfs_error_t err = lfs_free(lfs, i);
            if (err) {
                return err;
            }
        }
    }

    {
        // Write root directory
        struct __attribute__((packed)) {
            lfs_word_t rev;
            lfs_size_t len;
            lfs_ino_t  tail[2];
            struct lfs_free_list free;
        } header = {1, 0, {0, 0}, lfs->free};
        err = lfs->ops->write(lfs->bd, (void*)&header, 2, 0, sizeof(header));
        if (err) {
            return err;
        }

        uint32_t crc = lfs_crc((void*)&header, sizeof(header), 0xffffffff);

        for (lfs_size_t i = sizeof(header); i < info.erase_size-4; i += 4) {
            uint32_t data;
            err = lfs->ops->read(lfs->bd, (void*)&data, 2, i, 4);
            if (err) {
                return err;
            }

            crc = lfs_crc((void*)&data, 4, crc);
        }

        err = lfs->ops->write(lfs->bd, (void*)&crc, 2, info.erase_size-4, 4);
        if (err) {
            return err;
        }
    }

    {
        // Write superblock
        struct __attribute__((packed)) {
            lfs_word_t rev;
            lfs_word_t len;
            lfs_word_t tail[2];
            struct lfs_free_list free;
            char magic[4];
            struct lfs_bd_info info;
        } header = {1, 0, {2, 3}, {0}, {"lfs"}, info};
        err = lfs->ops->write(lfs->bd, (void*)&header, 0, 0, sizeof(header));
        if (err) {
            return err;
        }

        uint32_t crc = lfs_crc((void*)&header, sizeof(header), 0xffffffff);

        for (lfs_size_t i = sizeof(header); i < info.erase_size-4; i += 4) {
            uint32_t data;
            err = lfs->ops->read(lfs->bd, (void*)&data, 0, i, 4);
            if (err) {
                return err;
            }

            crc = lfs_crc((void*)&data, 4, crc);
        }

        err = lfs->ops->write(lfs->bd, (void*)&crc, 0, info.erase_size-4, 4);
        if (err) {
            return err;
        }
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

