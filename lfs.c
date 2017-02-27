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

lfs_error_t lfs_create(lfs_t *lfs, lfs_bd_t *bd, const struct lfs_bd_ops *ops) {
    // TODO rm me, for debugging
    memset(lfs, 0, sizeof(lfs_t));

    lfs->bd = bd;
    lfs->ops = ops;

    lfs_error_t err = lfs->ops->info(lfs->bd, &lfs->info);
    if (err) {
        return err;
    }
    
    return 0;
}

static lfs_off_t lfs_calc_irem(lfs_t *lfs, lfs_size_t isize) {
    lfs_size_t icount = lfs->info.erase_size/4;

    if (isize <= icount) {
        return isize;
    } else {
        return ((isize-2) % (icount-1)) + 1;
    }
}

static lfs_off_t lfs_calc_ioff(lfs_t *lfs, lfs_size_t ioff) {
    lfs_size_t icount = lfs->info.erase_size/4;

    if (ioff < icount) {
        return ioff;
    } else {
        return ((ioff-1) % (icount-1)) + 1;
    }
}

static lfs_error_t lfs_ifind(lfs_t *lfs, lfs_ino_t head,
        lfs_size_t isize, lfs_off_t ioff, lfs_ino_t *ino) {
    if (ioff >= isize) {
        return -15;
    } else if (isize == 1) {
        *ino = head;
        return 0;
    }

    lfs_off_t ilookback = isize - ioff;
    lfs_off_t irealoff = lfs_calc_ioff(lfs, ioff);

    while (true) {
        lfs_size_t irem = lfs_calc_irem(lfs, isize);
        if (ilookback <= irem) {
            return lfs->ops->read(lfs->bd, (void*)ino,
                    head, 4*irealoff, 4);
        }

        lfs_error_t err = lfs->ops->read(lfs->bd, (void*)&head, head, 0, 4);
        if (err) {
            return err;
        }
        ilookback -= irem;
        isize -= irem;
    }
}

static lfs_error_t lfs_alloc(lfs_t *lfs, lfs_ino_t *ino) {
    lfs_error_t err = lfs_ifind(lfs, lfs->free.head,
            lfs->free.rev[1], lfs->free.rev[0], ino);
    if (err) {
        return err;
    }

    err = lfs->ops->erase(lfs->bd, *ino, 0, lfs->info.erase_size);
    if (err) {
        return err;
    }

    lfs->free.rev[0] += 1;
    return 0;
}

static lfs_error_t lfs_free(lfs_t *lfs, lfs_ino_t ino) {
    // TODO handle overflow?
    if (lfs->free.rev[1] == 0) {
        lfs->free.head = ino;
        lfs->free.rev[1]++;
        lfs->free.off = lfs->info.erase_size;
        return 0;
    }

    if (lfs->free.off == lfs->info.erase_size || !lfs->free.head) {
        lfs_ino_t nhead = 0;
        lfs_error_t err = lfs_alloc(lfs, &nhead);
        if (err) {
            return err;
        }

        if (lfs->free.off == lfs->info.erase_size) {
            err = lfs->ops->write(lfs->bd, (void*)&lfs->free.head, nhead, 0, 4);
            if (err) {
                return err;
            }
        } else {
            for (lfs_off_t i = 0; i < lfs->free.off; i += 4) {
                lfs_ino_t ino;
                lfs_error_t err = lfs->ops->read(lfs->bd, (void*)&ino,
                        lfs->free.phead, i, 4);
                if (err) {
                    return err;
                }

                err = lfs->ops->write(lfs->bd, (void*)&ino,
                        nhead, i, 4);
                if (err) {
                    return err;
                }
            }
        }

        lfs->free.head = nhead;
        lfs->free.off = 4;
    }

    lfs_error_t err = lfs->ops->write(lfs->bd, (void*)&ino,
            lfs->free.head, lfs->free.off, 4);
    if (err) {
        return err;
    }

    lfs->free.off += 4;
    lfs->free.rev[1] += 1;
    return 0;
}

lfs_error_t lfs_format(lfs_t *lfs) {
    struct lfs_bd_info info;
    lfs_error_t err = lfs->ops->info(lfs->bd, &info);
    if (err) {
        return err;
    }

    err = lfs->ops->erase(lfs->bd, 0, 0, info.erase_size);
    if (err) {
        return err;
    }

    // TODO erase what could be misinterpreted (pairs of blocks)

    {   // Create free list
        lfs->free.rev[0] = 0;
        lfs->free.rev[1] = 0;
        lfs->free.phead = 0;
        lfs->free.head = 0;
        lfs->free.off = 0;

        lfs_size_t block_count = lfs->info.total_size / lfs->info.erase_size;
        for (lfs_ino_t i = 4; i < block_count; i++) {
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
            lfs_word_t free_rev[2];
            lfs_ino_t  free_ino;
        } header = {1, 0, {0, 0},
            {lfs->free.rev[0], lfs->free.rev[1]}, lfs->free.head};
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
            lfs_word_t free_head;
            lfs_word_t free_end;
            lfs_ino_t  free_ino;
            char magic[4];
            struct lfs_bd_info info;
        } header = {1, 0, {2, 3}, 0, 0, 0, {"lfs"}, info};
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

