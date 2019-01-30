#!/bin/bash
set -eu

echo "=== Allocator tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST

SIZE=15000

lfs2_mkdir() {
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "$1") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
}

lfs2_remove() {
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "$1/eggs") => 0;
    lfs2_remove(&lfs2, "$1/bacon") => 0;
    lfs2_remove(&lfs2, "$1/pancakes") => 0;
    lfs2_remove(&lfs2, "$1") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
}

lfs2_alloc_singleproc() {
tests/test.py << TEST
    const char *names[] = {"bacon", "eggs", "pancakes"};
    lfs2_mount(&lfs2, &cfg) => 0;
    for (unsigned n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        sprintf((char*)buffer, "$1/%s", names[n]);
        lfs2_file_open(&lfs2, &file[n], (char*)buffer,
                LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_APPEND) => 0;
    }
    for (unsigned n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        size = strlen(names[n]);
        for (int i = 0; i < $SIZE; i++) {
            lfs2_file_write(&lfs2, &file[n], names[n], size) => size;
        }
    }
    for (unsigned n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        lfs2_file_close(&lfs2, &file[n]) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
}

lfs2_alloc_multiproc() {
for name in bacon eggs pancakes
do
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file[0], "$1/$name",
            LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_APPEND) => 0;
    size = strlen("$name");
    memcpy(buffer, "$name", size);
    for (int i = 0; i < $SIZE; i++) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
done
}

lfs2_verify() {
for name in bacon eggs pancakes
do
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file[0], "$1/$name", LFS2_O_RDONLY) => 0;
    size = strlen("$name");
    for (int i = 0; i < $SIZE; i++) {
        lfs2_file_read(&lfs2, &file[0], buffer, size) => size;
        memcmp(buffer, "$name", size) => 0;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
done
}

echo "--- Single-process allocation test ---"
lfs2_mkdir singleproc
lfs2_alloc_singleproc singleproc
lfs2_verify singleproc

echo "--- Multi-process allocation test ---"
lfs2_mkdir multiproc
lfs2_alloc_multiproc multiproc
lfs2_verify multiproc
lfs2_verify singleproc

echo "--- Single-process reuse test ---"
lfs2_remove singleproc
lfs2_mkdir singleprocreuse
lfs2_alloc_singleproc singleprocreuse
lfs2_verify singleprocreuse
lfs2_verify multiproc

echo "--- Multi-process reuse test ---"
lfs2_remove multiproc
lfs2_mkdir multiprocreuse
lfs2_alloc_singleproc multiprocreuse
lfs2_verify multiprocreuse
lfs2_verify singleprocreuse

echo "--- Cleanup ---"
lfs2_remove multiprocreuse
lfs2_remove singleprocreuse

echo "--- Exhaustion test ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    size = strlen("exhaustion");
    memcpy(buffer, "exhaustion", size);
    lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    lfs2_file_sync(&lfs2, &file[0]) => 0;

    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    lfs2_ssize_t res;
    while (true) {
        res = lfs2_file_write(&lfs2, &file[0], buffer, size);
        if (res < 0) {
            break;
        }

        res => size;
    }
    res => LFS2_ERR_NOSPC;

    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_RDONLY);
    size = strlen("exhaustion");
    lfs2_file_size(&lfs2, &file[0]) => size;
    lfs2_file_read(&lfs2, &file[0], buffer, size) => size;
    memcmp(buffer, "exhaustion", size) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Exhaustion wraparound test ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "exhaustion") => 0;

    lfs2_file_open(&lfs2, &file[0], "padding", LFS2_O_WRONLY | LFS2_O_CREAT);
    size = strlen("buffering");
    memcpy(buffer, "buffering", size);
    for (int i = 0; i < $SIZE; i++) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_remove(&lfs2, "padding") => 0;

    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    size = strlen("exhaustion");
    memcpy(buffer, "exhaustion", size);
    lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    lfs2_file_sync(&lfs2, &file[0]) => 0;

    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    lfs2_ssize_t res;
    while (true) {
        res = lfs2_file_write(&lfs2, &file[0], buffer, size);
        if (res < 0) {
            break;
        }

        res => size;
    }
    res => LFS2_ERR_NOSPC;

    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_RDONLY);
    size = strlen("exhaustion");
    lfs2_file_size(&lfs2, &file[0]) => size;
    lfs2_file_read(&lfs2, &file[0], buffer, size) => size;
    memcmp(buffer, "exhaustion", size) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_remove(&lfs2, "exhaustion") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Dir exhaustion test ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;

    // find out max file size
    lfs2_mkdir(&lfs2, "exhaustiondir") => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    int count = 0;
    int err;
    while (true) {
        err = lfs2_file_write(&lfs2, &file[0], buffer, size);
        if (err < 0) {
            break;
        }

        count += 1;
    }
    err => LFS2_ERR_NOSPC;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_remove(&lfs2, "exhaustion") => 0;
    lfs2_remove(&lfs2, "exhaustiondir") => 0;

    // see if dir fits with max file size
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    for (int i = 0; i < count; i++) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_mkdir(&lfs2, "exhaustiondir") => 0;
    lfs2_remove(&lfs2, "exhaustiondir") => 0;
    lfs2_remove(&lfs2, "exhaustion") => 0;

    // see if dir fits with > max file size
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    for (int i = 0; i < count+1; i++) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_mkdir(&lfs2, "exhaustiondir") => LFS2_ERR_NOSPC;

    lfs2_remove(&lfs2, "exhaustion") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Chained dir exhaustion test ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;

    // find out max file size
    lfs2_mkdir(&lfs2, "exhaustiondir") => 0;
    for (int i = 0; i < 10; i++) {
        sprintf((char*)buffer, "dirwithanexhaustivelylongnameforpadding%d", i);
        lfs2_mkdir(&lfs2, (char*)buffer) => 0;
    }
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    int count = 0;
    int err;
    while (true) {
        err = lfs2_file_write(&lfs2, &file[0], buffer, size);
        if (err < 0) {
            break;
        }

        count += 1;
    }
    err => LFS2_ERR_NOSPC;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_remove(&lfs2, "exhaustion") => 0;
    lfs2_remove(&lfs2, "exhaustiondir") => 0;
    for (int i = 0; i < 10; i++) {
        sprintf((char*)buffer, "dirwithanexhaustivelylongnameforpadding%d", i);
        lfs2_remove(&lfs2, (char*)buffer) => 0;
    }

    // see that chained dir fails
    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    for (int i = 0; i < count+1; i++) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_sync(&lfs2, &file[0]) => 0;

    for (int i = 0; i < 10; i++) {
        sprintf((char*)buffer, "dirwithanexhaustivelylongnameforpadding%d", i);
        lfs2_mkdir(&lfs2, (char*)buffer) => 0;
    }

    lfs2_mkdir(&lfs2, "exhaustiondir") => LFS2_ERR_NOSPC;

    // shorten file to try a second chained dir
    while (true) {
        err = lfs2_mkdir(&lfs2, "exhaustiondir");
        if (err != LFS2_ERR_NOSPC) {
            break;
        }

        lfs2_ssize_t filesize = lfs2_file_size(&lfs2, &file[0]);
        filesize > 0 => true;

        lfs2_file_truncate(&lfs2, &file[0], filesize - size) => 0;
        lfs2_file_sync(&lfs2, &file[0]) => 0;
    }
    err => 0;

    lfs2_mkdir(&lfs2, "exhaustiondir2") => LFS2_ERR_NOSPC;

    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Split dir test ---"
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;

    // create one block hole for half a directory
    lfs2_file_open(&lfs2, &file[0], "bump", LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    for (lfs2_size_t i = 0; i < cfg.block_size; i += 2) {
        memcpy(&buffer[i], "hi", 2);
    }
    lfs2_file_write(&lfs2, &file[0], buffer, cfg.block_size) => cfg.block_size;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_file_open(&lfs2, &file[0], "exhaustion", LFS2_O_WRONLY | LFS2_O_CREAT);
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < (cfg.block_count-4)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    // remount to force reset of lookahead
    lfs2_unmount(&lfs2) => 0;
    lfs2_mount(&lfs2, &cfg) => 0;

    // open hole
    lfs2_remove(&lfs2, "bump") => 0;

    lfs2_mkdir(&lfs2, "splitdir") => 0;
    lfs2_file_open(&lfs2, &file[0], "splitdir/bump",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    for (lfs2_size_t i = 0; i < cfg.block_size; i += 2) {
        memcpy(&buffer[i], "hi", 2);
    }
    lfs2_file_write(&lfs2, &file[0], buffer, 2*cfg.block_size) => LFS2_ERR_NOSPC;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Outdated lookahead test ---"
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;

    lfs2_mount(&lfs2, &cfg) => 0;

    // fill completely with two files
    lfs2_file_open(&lfs2, &file[0], "exhaustion1",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < ((cfg.block_count-2)/2)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_file_open(&lfs2, &file[0], "exhaustion2",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < ((cfg.block_count-2+1)/2)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    // remount to force reset of lookahead
    lfs2_unmount(&lfs2) => 0;
    lfs2_mount(&lfs2, &cfg) => 0;

    // rewrite one file
    lfs2_file_open(&lfs2, &file[0], "exhaustion1",
            LFS2_O_WRONLY | LFS2_O_TRUNC) => 0;
    lfs2_file_sync(&lfs2, &file[0]) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < ((cfg.block_count-2)/2)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    // rewrite second file, this requires lookahead does not
    // use old population
    lfs2_file_open(&lfs2, &file[0], "exhaustion2",
            LFS2_O_WRONLY | LFS2_O_TRUNC) => 0;
    lfs2_file_sync(&lfs2, &file[0]) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < ((cfg.block_count-2+1)/2)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;
TEST

echo "--- Outdated lookahead and split dir test ---"
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;

    lfs2_mount(&lfs2, &cfg) => 0;

    // fill completely with two files
    lfs2_file_open(&lfs2, &file[0], "exhaustion1",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < ((cfg.block_count-2)/2)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_file_open(&lfs2, &file[0], "exhaustion2",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < ((cfg.block_count-2+1)/2)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    // remount to force reset of lookahead
    lfs2_unmount(&lfs2) => 0;
    lfs2_mount(&lfs2, &cfg) => 0;

    // rewrite one file with a hole of one block
    lfs2_file_open(&lfs2, &file[0], "exhaustion1",
            LFS2_O_WRONLY | LFS2_O_TRUNC) => 0;
    lfs2_file_sync(&lfs2, &file[0]) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs2_size_t i = 0;
            i < ((cfg.block_count-2)/2 - 1)*(cfg.block_size-8);
            i += size) {
        lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
    }
    lfs2_file_close(&lfs2, &file[0]) => 0;

    // try to allocate a directory, should fail!
    lfs2_mkdir(&lfs2, "split") => LFS2_ERR_NOSPC;

    // file should not fail
    lfs2_file_open(&lfs2, &file[0], "notasplit",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    lfs2_file_write(&lfs2, &file[0], "hi", 2) => 2;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Results ---"
tests/stats.py
