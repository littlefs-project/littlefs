/*
 * Some extra bench helpers
 *
 */
#include "benches/bench_helpers.h"


// warm up the filesystem
//
// this writes a 1 block file 2*block_count times to get it into a good
// state for benchmarking
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


// find tight disk usage
uintmax_t bench_helpers_usage(lfs3_t *lfs3) {
    // measure disk usage
    //
    // littlefs can be a dag, so build a bitmap to find the exact
    // disk usage
    uint8_t *usage_bmap = malloc((BLOCK_COUNT+8-1)/8);
    memset(usage_bmap, 0, (BLOCK_COUNT+8-1)/8);

    lfs3_trv_t trv;
    lfs3_trv_open(lfs3, &trv, 0) => 0;
    while (true) {
        struct lfs3_tinfo tinfo;
        int err = lfs3_trv_read(lfs3, &trv, &tinfo);
        assert(!err || err == LFS3_ERR_NOENT);
        if (err == LFS3_ERR_NOENT) {
            break;
        }

        usage_bmap[tinfo.block/8] |= 1 << (tinfo.block % 8);
    }
    lfs3_trv_close(lfs3, &trv) => 0;

    lfs3_size_t usage = 0;
    for (lfs3_size_t j = 0; j < BLOCK_COUNT; j++) {
        if (usage_bmap[j / 8] & (1 << (j % 8))) {
            usage += 1;
        }
    }

    free(usage_bmap);
    return (uintmax_t)usage * (uintmax_t)BLOCK_SIZE;
}


