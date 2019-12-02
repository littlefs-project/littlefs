#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

ITERATIONS=20
COUNT=10

echo "=== Relocation tests ==="
rm -rf blocks
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
    // fill up filesystem so only ~16 blocks are left
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "padding", LFS2_O_CREAT | LFS2_O_WRONLY) => 0;
    memset(buffer, 0, 512);
    while (LFS2_BLOCK_COUNT - lfs2_fs_size(&lfs2) > 16) {
        lfs2_file_write(&lfs2, &file, buffer, 512) => 512;
    }
    lfs2_file_close(&lfs2, &file) => 0;
    // make a child dir to use in bounded space
    lfs2_mkdir(&lfs2, "child") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Dangling split dir test ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int j = 0; j < $ITERATIONS; j++) {
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs2_file_open(&lfs2, &file, path, LFS2_O_CREAT | LFS2_O_WRONLY) => 0;
            lfs2_file_close(&lfs2, &file) => 0;
        }

        lfs2_dir_open(&lfs2, &dir, "child") => 0;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs2_dir_read(&lfs2, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
        }
        lfs2_dir_read(&lfs2, &dir, &info) => 0;
        lfs2_dir_close(&lfs2, &dir) => 0;

        if (j == $ITERATIONS-1) {
            break;
        }

        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs2_remove(&lfs2, path) => 0;
        }
    }
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "child") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    for (int i = 0; i < $COUNT; i++) {
        sprintf(path, "test%03d_loooooooooooooooooong_name", i);
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
    }
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    for (int i = 0; i < $COUNT; i++) {
        sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
        lfs2_remove(&lfs2, path) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Outdated head test ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int j = 0; j < $ITERATIONS; j++) {
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs2_file_open(&lfs2, &file, path, LFS2_O_CREAT | LFS2_O_WRONLY) => 0;
            lfs2_file_close(&lfs2, &file) => 0;
        }

        lfs2_dir_open(&lfs2, &dir, "child") => 0;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs2_dir_read(&lfs2, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
            info.size => 0;

            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs2_file_open(&lfs2, &file, path, LFS2_O_WRONLY) => 0;
            lfs2_file_write(&lfs2, &file, "hi", 2) => 2;
            lfs2_file_close(&lfs2, &file) => 0;
        }
        lfs2_dir_read(&lfs2, &dir, &info) => 0;

        lfs2_dir_rewind(&lfs2, &dir) => 0;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs2_dir_read(&lfs2, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
            info.size => 2;

            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs2_file_open(&lfs2, &file, path, LFS2_O_WRONLY) => 0;
            lfs2_file_write(&lfs2, &file, "hi", 2) => 2;
            lfs2_file_close(&lfs2, &file) => 0;
        }
        lfs2_dir_read(&lfs2, &dir, &info) => 0;

        lfs2_dir_rewind(&lfs2, &dir) => 0;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs2_dir_read(&lfs2, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
            info.size => 2;
        }
        lfs2_dir_read(&lfs2, &dir, &info) => 0;
        lfs2_dir_close(&lfs2, &dir) => 0;

        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs2_remove(&lfs2, path) => 0;
        }
    }
    lfs2_unmount(&lfs2) => 0;
TEST

scripts/results.py
