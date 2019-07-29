#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Move tests ==="
rm -rf blocks
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "a") => 0;
    lfs_mkdir(&lfs, "b") => 0;
    lfs_mkdir(&lfs, "c") => 0;
    lfs_mkdir(&lfs, "d") => 0;

    lfs_mkdir(&lfs, "a/hi") => 0;
    lfs_mkdir(&lfs, "a/hi/hola") => 0;
    lfs_mkdir(&lfs, "a/hi/bonjour") => 0;
    lfs_mkdir(&lfs, "a/hi/ohayo") => 0;

    lfs_file_open(&lfs, &file, "a/hello", LFS_O_CREAT | LFS_O_WRONLY) => 0;
    lfs_file_write(&lfs, &file, "hola\n", 5) => 5;
    lfs_file_write(&lfs, &file, "bonjour\n", 8) => 8;
    lfs_file_write(&lfs, &file, "ohayo\n", 6) => 6;
    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move file ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "a/hello", "b/hello") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "a") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "b") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move file corrupt source ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "b/hello", "c/hello") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/corrupt.py -n 1
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "b") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "c") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move file corrupt source and dest ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "c/hello", "d/hello") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/corrupt.py -n 2
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "c") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "d") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move file after corrupt ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "c/hello", "d/hello") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "c") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "d") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move dir ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "a/hi", "b/hi") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "a") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "b") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move dir corrupt source ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "b/hi", "c/hi") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/corrupt.py -n 1
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "b") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "c") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move dir corrupt source and dest ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "c/hi", "d/hi") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/corrupt.py -n 2
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "c") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "d") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move dir after corrupt ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "c/hi", "d/hi") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "c") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_dir_open(&lfs, &dir, "d") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move check ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;

    lfs_dir_open(&lfs, &dir, "a/hi") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "b/hi") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "c/hi") => LFS_ERR_NOENT;

    lfs_dir_open(&lfs, &dir, "d/hi") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "bonjour") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hola") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "ohayo") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;

    lfs_dir_open(&lfs, &dir, "a/hello") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "b/hello") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "c/hello") => LFS_ERR_NOENT;

    lfs_file_open(&lfs, &file, "d/hello", LFS_O_RDONLY) => 0;
    lfs_file_read(&lfs, &file, buffer, 5) => 5;
    memcmp(buffer, "hola\n", 5) => 0;
    lfs_file_read(&lfs, &file, buffer, 8) => 8;
    memcmp(buffer, "bonjour\n", 8) => 0;
    lfs_file_read(&lfs, &file, buffer, 6) => 6;
    memcmp(buffer, "ohayo\n", 6) => 0;
    lfs_file_close(&lfs, &file) => 0;

    lfs_unmount(&lfs) => 0;
TEST

echo "--- Move state stealing ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;

    lfs_remove(&lfs, "b") => 0;
    lfs_remove(&lfs, "c") => 0;

    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;

    lfs_dir_open(&lfs, &dir, "a/hi") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "b") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "c") => LFS_ERR_NOENT;

    lfs_dir_open(&lfs, &dir, "d/hi") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "bonjour") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hola") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "ohayo") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;

    lfs_dir_open(&lfs, &dir, "a/hello") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "b") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "c") => LFS_ERR_NOENT;

    lfs_file_open(&lfs, &file, "d/hello", LFS_O_RDONLY) => 0;
    lfs_file_read(&lfs, &file, buffer, 5) => 5;
    memcmp(buffer, "hola\n", 5) => 0;
    lfs_file_read(&lfs, &file, buffer, 8) => 8;
    memcmp(buffer, "bonjour\n", 8) => 0;
    lfs_file_read(&lfs, &file, buffer, 6) => 6;
    memcmp(buffer, "ohayo\n", 6) => 0;
    lfs_file_close(&lfs, &file) => 0;

    lfs_unmount(&lfs) => 0;
TEST


scripts/results.py
