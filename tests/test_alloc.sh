#!/bin/bash
set -eu

echo "=== Allocator tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST

SIZE=15000

lfs1_mkdir() {
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "$1") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
}

lfs1_remove() {
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "$1/eggs") => 0;
    lfs1_remove(&lfs1, "$1/bacon") => 0;
    lfs1_remove(&lfs1, "$1/pancakes") => 0;
    lfs1_remove(&lfs1, "$1") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
}

lfs1_alloc_singleproc() {
tests/test.py << TEST
    const char *names[] = {"bacon", "eggs", "pancakes"};
    lfs1_mount(&lfs1, &cfg) => 0;
    for (unsigned n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        sprintf((char*)buffer, "$1/%s", names[n]);
        lfs1_file_open(&lfs1, &file[n], (char*)buffer,
                LFS1_O_WRONLY | LFS1_O_CREAT | LFS1_O_APPEND) => 0;
    }
    for (unsigned n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        size = strlen(names[n]);
        for (int i = 0; i < $SIZE; i++) {
            lfs1_file_write(&lfs1, &file[n], names[n], size) => size;
        }
    }
    for (unsigned n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        lfs1_file_close(&lfs1, &file[n]) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST
}

lfs1_alloc_multiproc() {
for name in bacon eggs pancakes
do
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "$1/$name",
            LFS1_O_WRONLY | LFS1_O_CREAT | LFS1_O_APPEND) => 0;
    size = strlen("$name");
    memcpy(buffer, "$name", size);
    for (int i = 0; i < $SIZE; i++) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
done
}

lfs1_verify() {
for name in bacon eggs pancakes
do
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "$1/$name", LFS1_O_RDONLY) => 0;
    size = strlen("$name");
    for (int i = 0; i < $SIZE; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
        memcmp(buffer, "$name", size) => 0;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
done
}

echo "--- Single-process allocation test ---"
lfs1_mkdir singleproc
lfs1_alloc_singleproc singleproc
lfs1_verify singleproc

echo "--- Multi-process allocation test ---"
lfs1_mkdir multiproc
lfs1_alloc_multiproc multiproc
lfs1_verify multiproc
lfs1_verify singleproc

echo "--- Single-process reuse test ---"
lfs1_remove singleproc
lfs1_mkdir singleprocreuse
lfs1_alloc_singleproc singleprocreuse
lfs1_verify singleprocreuse
lfs1_verify multiproc

echo "--- Multi-process reuse test ---"
lfs1_remove multiproc
lfs1_mkdir multiprocreuse
lfs1_alloc_singleproc multiprocreuse
lfs1_verify multiprocreuse
lfs1_verify singleprocreuse

echo "--- Cleanup ---"
lfs1_remove multiprocreuse
lfs1_remove singleprocreuse

echo "--- Exhaustion test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_WRONLY | LFS1_O_CREAT);
    size = strlen("exhaustion");
    memcpy(buffer, "exhaustion", size);
    lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    lfs1_file_sync(&lfs1, &file[0]) => 0;

    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    lfs1_ssize_t res;
    while (true) {
        res = lfs1_file_write(&lfs1, &file[0], buffer, size);
        if (res < 0) {
            break;
        }

        res => size;
    }
    res => LFS1_ERR_NOSPC;

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_RDONLY);
    size = strlen("exhaustion");
    lfs1_file_size(&lfs1, &file[0]) => size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "exhaustion", size) => 0;
    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Exhaustion wraparound test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "exhaustion") => 0;

    lfs1_file_open(&lfs1, &file[0], "padding", LFS1_O_WRONLY | LFS1_O_CREAT);
    size = strlen("buffering");
    memcpy(buffer, "buffering", size);
    for (int i = 0; i < $SIZE; i++) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_remove(&lfs1, "padding") => 0;

    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_WRONLY | LFS1_O_CREAT);
    size = strlen("exhaustion");
    memcpy(buffer, "exhaustion", size);
    lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    lfs1_file_sync(&lfs1, &file[0]) => 0;

    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    lfs1_ssize_t res;
    while (true) {
        res = lfs1_file_write(&lfs1, &file[0], buffer, size);
        if (res < 0) {
            break;
        }

        res => size;
    }
    res => LFS1_ERR_NOSPC;

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_RDONLY);
    size = strlen("exhaustion");
    lfs1_file_size(&lfs1, &file[0]) => size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "exhaustion", size) => 0;
    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Dir exhaustion test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "exhaustion") => 0;

    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_WRONLY | LFS1_O_CREAT);
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < (cfg.block_count-6)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_mkdir(&lfs1, "exhaustiondir") => 0;
    lfs1_remove(&lfs1, "exhaustiondir") => 0;

    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_WRONLY | LFS1_O_APPEND);
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < (cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_mkdir(&lfs1, "exhaustiondir") => LFS1_ERR_NOSPC;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Chained dir exhaustion test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "exhaustion") => 0;

    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_WRONLY | LFS1_O_CREAT);
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < (cfg.block_count-24)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    for (int i = 0; i < 9; i++) {
        sprintf((char*)buffer, "dirwithanexhaustivelylongnameforpadding%d", i);
        lfs1_mkdir(&lfs1, (char*)buffer) => 0;
    }

    lfs1_mkdir(&lfs1, "exhaustiondir") => LFS1_ERR_NOSPC;

    lfs1_remove(&lfs1, "exhaustion") => 0;
    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_WRONLY | LFS1_O_CREAT);
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < (cfg.block_count-26)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_mkdir(&lfs1, "exhaustiondir") => 0;
    lfs1_mkdir(&lfs1, "exhaustiondir2") => LFS1_ERR_NOSPC;
TEST

echo "--- Split dir test ---"
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;

    // create one block whole for half a directory
    lfs1_file_open(&lfs1, &file[0], "bump", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    lfs1_file_write(&lfs1, &file[0], (void*)"hi", 2) => 2;
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_file_open(&lfs1, &file[0], "exhaustion", LFS1_O_WRONLY | LFS1_O_CREAT);
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < (cfg.block_count-6)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    // open hole
    lfs1_remove(&lfs1, "bump") => 0;

    lfs1_mkdir(&lfs1, "splitdir") => 0;
    lfs1_file_open(&lfs1, &file[0], "splitdir/bump",
            LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    lfs1_file_write(&lfs1, &file[0], buffer, size) => LFS1_ERR_NOSPC;
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Outdated lookahead test ---"
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;

    lfs1_mount(&lfs1, &cfg) => 0;

    // fill completely with two files
    lfs1_file_open(&lfs1, &file[0], "exhaustion1",
            LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < ((cfg.block_count-4)/2)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_file_open(&lfs1, &file[0], "exhaustion2",
            LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < ((cfg.block_count-4+1)/2)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    // remount to force reset of lookahead
    lfs1_unmount(&lfs1) => 0;

    lfs1_mount(&lfs1, &cfg) => 0;

    // rewrite one file
    lfs1_file_open(&lfs1, &file[0], "exhaustion1",
            LFS1_O_WRONLY | LFS1_O_TRUNC) => 0;
    lfs1_file_sync(&lfs1, &file[0]) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < ((cfg.block_count-4)/2)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    // rewrite second file, this requires lookahead does not
    // use old population
    lfs1_file_open(&lfs1, &file[0], "exhaustion2",
            LFS1_O_WRONLY | LFS1_O_TRUNC) => 0;
    lfs1_file_sync(&lfs1, &file[0]) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < ((cfg.block_count-4+1)/2)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;
TEST

echo "--- Outdated lookahead and split dir test ---"
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;

    lfs1_mount(&lfs1, &cfg) => 0;

    // fill completely with two files
    lfs1_file_open(&lfs1, &file[0], "exhaustion1",
            LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < ((cfg.block_count-4)/2)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_file_open(&lfs1, &file[0], "exhaustion2",
            LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < ((cfg.block_count-4+1)/2)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    // remount to force reset of lookahead
    lfs1_unmount(&lfs1) => 0;

    lfs1_mount(&lfs1, &cfg) => 0;

    // rewrite one file with a hole of one block
    lfs1_file_open(&lfs1, &file[0], "exhaustion1",
            LFS1_O_WRONLY | LFS1_O_TRUNC) => 0;
    lfs1_file_sync(&lfs1, &file[0]) => 0;
    size = strlen("blahblahblahblah");
    memcpy(buffer, "blahblahblahblah", size);
    for (lfs1_size_t i = 0;
            i < ((cfg.block_count-4)/2 - 1)*(cfg.block_size-8);
            i += size) {
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
    }
    lfs1_file_close(&lfs1, &file[0]) => 0;

    // try to allocate a directory, should fail!
    lfs1_mkdir(&lfs1, "split") => LFS1_ERR_NOSPC;

    // file should not fail
    lfs1_file_open(&lfs1, &file[0], "notasplit",
            LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    lfs1_file_write(&lfs1, &file[0], "hi", 2) => 2;
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Results ---"
tests/stats.py
