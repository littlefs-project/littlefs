#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

ITERATIONS=20
COUNT=10

echo "=== Relocation tests ==="
rm -rf blocks
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
    // fill up filesystem so only ~16 blocks are left
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "padding", LFS_O_CREAT | LFS_O_WRONLY) => 0;
    memset(buffer, 0, 512);
    while (LFS_BLOCK_COUNT - lfs_fs_size(&lfs) > 16) {
        lfs_file_write(&lfs, &file, buffer, 512) => 512;
    }
    lfs_file_close(&lfs, &file) => 0;
    // make a child dir to use in bounded space
    lfs_mkdir(&lfs, "child") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Dangling split dir test ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    for (int j = 0; j < $ITERATIONS; j++) {
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs_file_open(&lfs, &file, path, LFS_O_CREAT | LFS_O_WRONLY) => 0;
            lfs_file_close(&lfs, &file) => 0;
        }

        lfs_dir_open(&lfs, &dir, "child") => 0;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs_dir_read(&lfs, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
        }
        lfs_dir_read(&lfs, &dir, &info) => 0;
        lfs_dir_close(&lfs, &dir) => 0;

        if (j == $ITERATIONS-1) {
            break;
        }

        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs_remove(&lfs, path) => 0;
        }
    }
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "child") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    for (int i = 0; i < $COUNT; i++) {
        sprintf(path, "test%03d_loooooooooooooooooong_name", i);
        lfs_dir_read(&lfs, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
    }
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    for (int i = 0; i < $COUNT; i++) {
        sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
        lfs_remove(&lfs, path) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Outdated head test ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    for (int j = 0; j < $ITERATIONS; j++) {
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs_file_open(&lfs, &file, path, LFS_O_CREAT | LFS_O_WRONLY) => 0;
            lfs_file_close(&lfs, &file) => 0;
        }

        lfs_dir_open(&lfs, &dir, "child") => 0;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs_dir_read(&lfs, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
            info.size => 0;

            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs_file_open(&lfs, &file, path, LFS_O_WRONLY) => 0;
            lfs_file_write(&lfs, &file, "hi", 2) => 2;
            lfs_file_close(&lfs, &file) => 0;
        }
        lfs_dir_read(&lfs, &dir, &info) => 0;

        lfs_dir_rewind(&lfs, &dir) => 0;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs_dir_read(&lfs, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
            info.size => 2;

            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs_file_open(&lfs, &file, path, LFS_O_WRONLY) => 0;
            lfs_file_write(&lfs, &file, "hi", 2) => 2;
            lfs_file_close(&lfs, &file) => 0;
        }
        lfs_dir_read(&lfs, &dir, &info) => 0;

        lfs_dir_rewind(&lfs, &dir) => 0;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        lfs_dir_read(&lfs, &dir, &info) => 1;
        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "test%03d_loooooooooooooooooong_name", i);
            lfs_dir_read(&lfs, &dir, &info) => 1;
            strcmp(info.name, path) => 0;
            info.size => 2;
        }
        lfs_dir_read(&lfs, &dir, &info) => 0;
        lfs_dir_close(&lfs, &dir) => 0;

        for (int i = 0; i < $COUNT; i++) {
            sprintf(path, "child/test%03d_loooooooooooooooooong_name", i);
            lfs_remove(&lfs, path) => 0;
        }
    }
    lfs_unmount(&lfs) => 0;
TEST

scripts/results.py
