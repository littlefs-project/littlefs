#!/bin/bash
set -eu

echo "=== Move tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;

    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "a") => 0;
    lfs1_mkdir(&lfs1, "b") => 0;
    lfs1_mkdir(&lfs1, "c") => 0;
    lfs1_mkdir(&lfs1, "d") => 0;

    lfs1_mkdir(&lfs1, "a/hi") => 0;
    lfs1_mkdir(&lfs1, "a/hi/hola") => 0;
    lfs1_mkdir(&lfs1, "a/hi/bonjour") => 0;
    lfs1_mkdir(&lfs1, "a/hi/ohayo") => 0;

    lfs1_file_open(&lfs1, &file[0], "a/hello", LFS1_O_CREAT | LFS1_O_WRONLY) => 0;
    lfs1_file_write(&lfs1, &file[0], "hola\n", 5) => 5;
    lfs1_file_write(&lfs1, &file[0], "bonjour\n", 8) => 8;
    lfs1_file_write(&lfs1, &file[0], "ohayo\n", 6) => 6;
    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Move file ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_rename(&lfs1, "a/hello", "b/hello") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "a") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "b") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Move file corrupt source ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_rename(&lfs1, "b/hello", "c/hello") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
rm -v blocks/7
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "b") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "c") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Move file corrupt source and dest ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_rename(&lfs1, "c/hello", "d/hello") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
rm -v blocks/8
rm -v blocks/a
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "c") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "d") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Move dir ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_rename(&lfs1, "a/hi", "b/hi") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "a") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "b") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Move dir corrupt source ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_rename(&lfs1, "b/hi", "c/hi") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
rm -v blocks/7
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "b") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "c") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Move dir corrupt source and dest ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_rename(&lfs1, "c/hi", "d/hi") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
rm -v blocks/9
rm -v blocks/a
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "c") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hi") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "d") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Move check ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;

    lfs1_dir_open(&lfs1, &dir[0], "a/hi") => LFS1_ERR_NOENT;
    lfs1_dir_open(&lfs1, &dir[0], "b/hi") => LFS1_ERR_NOENT;
    lfs1_dir_open(&lfs1, &dir[0], "d/hi") => LFS1_ERR_NOENT;

    lfs1_dir_open(&lfs1, &dir[0], "c/hi") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "hola") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "bonjour") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "ohayo") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;

    lfs1_dir_open(&lfs1, &dir[0], "a/hello") => LFS1_ERR_NOENT;
    lfs1_dir_open(&lfs1, &dir[0], "b/hello") => LFS1_ERR_NOENT;
    lfs1_dir_open(&lfs1, &dir[0], "d/hello") => LFS1_ERR_NOENT;

    lfs1_file_open(&lfs1, &file[0], "c/hello", LFS1_O_RDONLY) => 0;
    lfs1_file_read(&lfs1, &file[0], buffer, 5) => 5;
    memcmp(buffer, "hola\n", 5) => 0;
    lfs1_file_read(&lfs1, &file[0], buffer, 8) => 8;
    memcmp(buffer, "bonjour\n", 8) => 0;
    lfs1_file_read(&lfs1, &file[0], buffer, 6) => 6;
    memcmp(buffer, "ohayo\n", 6) => 0;
    lfs1_file_close(&lfs1, &file[0]) => 0;

    lfs1_unmount(&lfs1) => 0;
TEST


echo "--- Results ---"
tests/stats.py
