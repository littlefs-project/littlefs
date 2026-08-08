/*
 * Some extra bench helpers
 *
 */
#include "benches/bench_helpers.h"


// warm up the filesystem
//
// this writes a 1 block file 2*block_count times to get things into a
// good state for benchmarking
int bench_helpers_warmup(lfs3_t *lfs3) {
    uint8_t *wbuf = malloc(BLOCK_SIZE);
    memset(wbuf, '1', BLOCK_SIZE);

    lfs3_file_t file;
    lfs3_file_open(lfs3, &file, "warmup",
            LFS3_O_WRONLY | LFS3_O_CREAT | LFS3_O_EXCL) => 0;
    for (lfs3_block_t i = 0; i < 2*BLOCK_COUNT; i++) {
        lfs3_file_rewind(lfs3, &file) => 0;
        lfs3_file_write(lfs3, &file, wbuf, BLOCK_SIZE) => BLOCK_SIZE;
        lfs3_file_sync(lfs3, &file) => 0;
    }
    lfs3_file_close(lfs3, &file) => 0;

    lfs3_remove(lfs3, "warmup") => 0;

    free(wbuf);
    return 0;
}


// populate the filesystem with static files
//
// useful for static vs dynamic wear-leveling, metadata pressure, etc
int bench_helpers_populate(lfs3_t *lfs3,
        lfs3_off_t static_count, lfs3_off_t static_size,
        bool static_compact) {
    char nbuf[256];
    uint8_t *wbuf = malloc(static_size);
    memset(wbuf, 's', static_size);

    for (lfs3_off_t i = 0; i < static_count; i++) {
        lfs3_file_t file;
        sprintf(nbuf, "static_%08x", i);
        lfs3_file_open(lfs3, &file, nbuf,
                LFS3_O_WRONLY | LFS3_O_CREAT | LFS3_O_EXCL) => 0;
        lfs3_file_write(lfs3, &file, wbuf, static_size) => static_size;
        lfs3_file_close(lfs3, &file) => 0;
    }

    if (static_compact) {
        lfs3_gc_t gc;
        lfs3_gc_open(lfs3, &gc, LFS3_GC_COMPACTMETA) => 0;
        lfs3_sblock_t steps = lfs3_gc_write(lfs3, &gc, -1);
        assert(steps >= 0);
        lfs3_gc_close(lfs3, &gc) => 0;
    }

    free(wbuf);
    return 0;
}


// find tight disk usage
int bench_helpers_usage(lfs3_t *lfs3, struct bench_helpers_usage *usage) {
    // measure disk usage
    //
    // littlefs can be a dag, so build a bitmap to find the exact
    // disk usage
    //
    // but also use 2 bits so we can still find mdir/btree/data specific
    // usage
    uint8_t *usage_bmap = malloc(((2*BLOCK_COUNT)+8-1)/8);
    memset(usage_bmap, 0, ((2*BLOCK_COUNT)+8-1)/8);

    lfs3_trv_t trv;
    lfs3_trv_open(lfs3, &trv, 0) => 0;
    while (true) {
        struct lfs3_binfo binfo;
        int err = lfs3_trv_read(lfs3, &trv, &binfo);
        assert(!err || err == LFS3_ERR_NOENT);
        if (err == LFS3_ERR_NOENT) {
            break;
        }

        if (binfo.btype == LFS3_BTYPE_MDIR) {
            usage_bmap[(2*binfo.block/8)] |= 1 << ((2*binfo.block) % 8);
        } else if (binfo.btype == LFS3_BTYPE_BTREE) {
            usage_bmap[(2*binfo.block/8)] |= 2 << ((2*binfo.block) % 8);
        } else {
            usage_bmap[(2*binfo.block/8)] |= 3 << ((2*binfo.block) % 8);
        }
    }
    lfs3_trv_close(lfs3, &trv) => 0;

    memset(usage, 0, sizeof(struct bench_helpers_usage));
    for (lfs3_size_t j = 0; j < BLOCK_COUNT; j++) {
        uint8_t btype = (usage_bmap[(2*j) / 8] >> ((2*j) % 8)) & 0x3;
        if (btype != 0) {
            usage->usage += 1;
            if (btype == 1) {
                usage->mdir += 1;
            } else if (btype == 2) {
                usage->btree += 1;
            } else {
                usage->data += 1;
            }
        }
    }

    free(usage_bmap);
    return 0;
}


