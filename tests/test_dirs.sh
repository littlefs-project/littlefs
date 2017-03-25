#!/bin/bash
set -eu

echo "=== Directory tests ==="
rm -rf blocks

echo "--- Root directory ---"
tests/test.py << TEST
    lfs_format(&lfs, &config) => 0;

    lfs_dir_open(&lfs, &dir[0], "/") => 0;
    lfs_dir_close(&lfs, &dir[0]) => 0;
TEST

echo "--- Directory creation ---"
tests/test.py << TEST
    lfs_mount(&lfs, &config) => 0;
    lfs_mkdir(&lfs, "potato") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- File creation ---"
tests/test.py << TEST
    lfs_mount(&lfs, &config) => 0;
    lfs_file_open(&lfs, &file[0], "burito", LFS_O_CREAT | LFS_O_WRONLY) => 0;
    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Directory iteration ---"
tests/test.py << TEST
    lfs_mount(&lfs, &config) => 0;
    lfs_dir_open(&lfs, &dir[0], "/") => 0;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, "potato") => 0;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    lfs_dir_read(&lfs, &dir[0], &info) => 0;
    lfs_dir_close(&lfs, &dir[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Directory failures ---"
tests/test.py << TEST
    lfs_mount(&lfs, &config) => 0;
    lfs_mkdir(&lfs, "potato") => LFS_ERROR_EXISTS;
    lfs_dir_open(&lfs, &dir[0], "tomato") => LFS_ERROR_NO_ENTRY;
    lfs_dir_open(&lfs, &dir[0], "burito") => LFS_ERROR_NOT_DIR;
    lfs_file_open(&lfs, &file[0], "tomato", LFS_O_RDONLY) => LFS_ERROR_NO_ENTRY;
    lfs_file_open(&lfs, &file[0], "potato", LFS_O_RDONLY) => LFS_ERROR_IS_DIR;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Results ---"
tests/stats.py
