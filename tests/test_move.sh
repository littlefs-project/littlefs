#!/bin/bash
set -eu

echo "=== Move tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;

    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "a") => 0;
    lfs2_mkdir(&lfs2, "b") => 0;
    lfs2_mkdir(&lfs2, "c") => 0;
    lfs2_mkdir(&lfs2, "d") => 0;

    lfs2_mkdir(&lfs2, "a/hi") => 0;
    lfs2_mkdir(&lfs2, "a/hi/hola") => 0;
    lfs2_mkdir(&lfs2, "a/hi/bonjour") => 0;
    lfs2_mkdir(&lfs2, "a/hi/ohayo") => 0;

    lfs2_file_open(&lfs2, &file[0], "a/hello", LFS2_O_CREAT | LFS2_O_WRONLY) => 0;
    lfs2_file_write(&lfs2, &file[0], "hola\n", 5) => 5;
    lfs2_file_write(&lfs2, &file[0], "bonjour\n", 8) => 8;
    lfs2_file_write(&lfs2, &file[0], "ohayo\n", 6) => 6;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move file ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "a/hello", "b/hello") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "a") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "b") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move file corrupt source ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "b/hello", "c/hello") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/corrupt.py -n 1
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "b") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "c") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move file corrupt source and dest ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "c/hello", "d/hello") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/corrupt.py -n 2
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "c") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "d") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move file after corrupt ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "c/hello", "d/hello") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "c") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "d") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move dir ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "a/hi", "b/hi") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "a") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "b") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move dir corrupt source ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "b/hi", "c/hi") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/corrupt.py -n 1
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "b") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "c") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move dir corrupt source and dest ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "c/hi", "d/hi") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/corrupt.py -n 2
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "c") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "d") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move dir after corrupt ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "c/hi", "d/hi") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "c") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "d") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move check ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;

    lfs2_dir_open(&lfs2, &dir[0], "a/hi") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "b/hi") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "c/hi") => LFS2_ERR_NOENT;

    lfs2_dir_open(&lfs2, &dir[0], "d/hi") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "bonjour") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hola") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "ohayo") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;

    lfs2_dir_open(&lfs2, &dir[0], "a/hello") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "b/hello") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "c/hello") => LFS2_ERR_NOENT;

    lfs2_file_open(&lfs2, &file[0], "d/hello", LFS2_O_RDONLY) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, 5) => 5;
    memcmp(buffer, "hola\n", 5) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, 8) => 8;
    memcmp(buffer, "bonjour\n", 8) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, 6) => 6;
    memcmp(buffer, "ohayo\n", 6) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Move state stealing ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;

    lfs2_remove(&lfs2, "b") => 0;
    lfs2_remove(&lfs2, "c") => 0;

    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;

    lfs2_dir_open(&lfs2, &dir[0], "a/hi") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "b") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "c") => LFS2_ERR_NOENT;

    lfs2_dir_open(&lfs2, &dir[0], "d/hi") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "bonjour") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "hola") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "ohayo") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;

    lfs2_dir_open(&lfs2, &dir[0], "a/hello") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "b") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "c") => LFS2_ERR_NOENT;

    lfs2_file_open(&lfs2, &file[0], "d/hello", LFS2_O_RDONLY) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, 5) => 5;
    memcmp(buffer, "hola\n", 5) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, 8) => 8;
    memcmp(buffer, "bonjour\n", 8) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, 6) => 6;
    memcmp(buffer, "ohayo\n", 6) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_unmount(&lfs2) => 0;
TEST


echo "--- Results ---"
tests/stats.py
