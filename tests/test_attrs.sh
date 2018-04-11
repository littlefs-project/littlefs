#!/bin/bash
set -eu

echo "=== Attr tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "hello") => 0;
    lfs_file_open(&lfs, &file[0], "hello/hello",
            LFS_O_WRONLY | LFS_O_CREAT) => 0;
    lfs_file_write(&lfs, &file[0], "hello", strlen("hello"))
            => strlen("hello");
    lfs_file_close(&lfs, &file[0]);
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Set/get attribute ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_setattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', "aaaa",   4},
            {'B', "bbbbbb", 6},
            {'C', "ccccc",  5}}, 3) => 0;
    lfs_getattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "bbbbbb", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs_setattrs(&lfs, "hello", (struct lfs_attr[]){
            {'B', "", 0}}, 1) => 0;
    lfs_getattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;

    lfs_setattrs(&lfs, "hello", (struct lfs_attr[]){
            {'B', "dddddd", 6}}, 1) => 0;
    lfs_getattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "dddddd", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs_setattrs(&lfs, "hello", (struct lfs_attr[]){
            {'B', "eee", 3}}, 1) => 0;
    lfs_getattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "eee\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",     5) => 0;

    lfs_setattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', buffer, LFS_ATTRS_MAX+1}}, 1) => LFS_ERR_NOSPC;
    lfs_setattrs(&lfs, "hello", (struct lfs_attr[]){
            {'B', "fffffffff", 9}}, 1) => 0;
    lfs_getattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => LFS_ERR_RANGE;

    lfs_unmount(&lfs) => 0;
TEST
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_getattrs(&lfs, "hello", (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  9},
            {'C', buffer+13, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "fffffffff", 9) => 0;
    memcmp(buffer+13, "ccccc",     5) => 0;

    lfs_file_open(&lfs, &file[0], "hello/hello", LFS_O_RDONLY) => 0;
    lfs_file_read(&lfs, &file[0], buffer, sizeof(buffer)) => strlen("hello");
    memcmp(buffer, "hello", strlen("hello")) => 0;
    lfs_file_close(&lfs, &file[0]);
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Set/get fs attribute ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_fs_setattrs(&lfs, (struct lfs_attr[]){
            {'A', "aaaa",   4},
            {'B', "bbbbbb", 6},
            {'C', "ccccc",  5}}, 3) => 0;
    lfs_fs_getattrs(&lfs, (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "bbbbbb", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs_fs_setattrs(&lfs, (struct lfs_attr[]){
            {'B', "", 0}}, 1) => 0;
    lfs_fs_getattrs(&lfs, (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;

    lfs_fs_setattrs(&lfs, (struct lfs_attr[]){
            {'B', "dddddd", 6}}, 1) => 0;
    lfs_fs_getattrs(&lfs, (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "dddddd", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs_fs_setattrs(&lfs, (struct lfs_attr[]){
            {'B', "eee", 3}}, 1) => 0;
    lfs_fs_getattrs(&lfs, (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "eee\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",     5) => 0;

    lfs_fs_setattrs(&lfs, (struct lfs_attr[]){
            {'A', buffer, LFS_ATTRS_MAX+1}}, 1) => LFS_ERR_NOSPC;
    lfs_fs_setattrs(&lfs, (struct lfs_attr[]){
            {'B', "fffffffff", 9}}, 1) => 0;
    lfs_fs_getattrs(&lfs, (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => LFS_ERR_RANGE;
    lfs_unmount(&lfs) => 0;
TEST
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_fs_getattrs(&lfs, (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  9},
            {'C', buffer+13, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "fffffffff", 9) => 0;
    memcmp(buffer+13, "ccccc",     5) => 0;

    lfs_file_open(&lfs, &file[0], "hello/hello", LFS_O_RDONLY) => 0;
    lfs_file_read(&lfs, &file[0], buffer, sizeof(buffer)) => strlen("hello");
    memcmp(buffer, "hello", strlen("hello")) => 0;
    lfs_file_close(&lfs, &file[0]);
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Set/get file attribute ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "hello/hello", LFS_O_WRONLY) => 0;

    struct lfs_attr attr[3];
    attr[0] = (struct lfs_attr){'A', "aaaa",   4};
    attr[1] = (struct lfs_attr){'B', "bbbbbb", 6};
    attr[2] = (struct lfs_attr){'C', "ccccc",  5};
    lfs_file_setattrs(&lfs, &file[0], attr, 3) => 0;
    lfs_file_getattrs(&lfs, &file[0], (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "bbbbbb", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;
    lfs_file_sync(&lfs, &file[0]) => 0;

    attr[0] = (struct lfs_attr){'B', "", 0};
    lfs_file_setattrs(&lfs, &file[0], attr, 1) => 0;
    lfs_file_getattrs(&lfs, &file[0], (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;
    lfs_file_sync(&lfs, &file[0]) => 0;

    attr[0] = (struct lfs_attr){'B', "dddddd", 6};
    lfs_file_setattrs(&lfs, &file[0], attr, 1) => 0;
    lfs_file_getattrs(&lfs, &file[0], (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "dddddd", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;
    lfs_file_sync(&lfs, &file[0]) => 0;

    attr[0] = (struct lfs_attr){'B', "eee", 3};
    lfs_file_setattrs(&lfs, &file[0], attr, 1);
    lfs_file_getattrs(&lfs, &file[0], (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "eee\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",     5) => 0;
    lfs_file_sync(&lfs, &file[0]) => 0;

    attr[0] = (struct lfs_attr){'A', buffer, LFS_ATTRS_MAX+1};
    lfs_file_setattrs(&lfs, &file[0], attr, 1) => LFS_ERR_NOSPC;
    attr[0] = (struct lfs_attr){'B', "fffffffff", 9};
    lfs_file_open(&lfs, &file[1], "hello/hello", LFS_O_RDONLY) => 0;
    lfs_file_setattrs(&lfs, &file[1], attr, 1) => LFS_ERR_BADF;
    lfs_file_close(&lfs, &file[1]) => 0;
    lfs_file_setattrs(&lfs, &file[0], attr, 1) => 0;
    lfs_file_getattrs(&lfs, &file[0], (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  6},
            {'C', buffer+10, 5}}, 3) => LFS_ERR_RANGE;

    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "hello/hello", LFS_O_RDONLY) => 0;

    lfs_file_getattrs(&lfs, &file[0], (struct lfs_attr[]){
            {'A', buffer,    4},
            {'B', buffer+4,  9},
            {'C', buffer+13, 5}}, 3) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "fffffffff", 9) => 0;
    memcmp(buffer+13, "ccccc",     5) => 0;

    lfs_file_open(&lfs, &file[0], "hello/hello", LFS_O_RDONLY) => 0;
    lfs_file_read(&lfs, &file[0], buffer, sizeof(buffer)) => strlen("hello");
    memcmp(buffer, "hello", strlen("hello")) => 0;
    lfs_file_close(&lfs, &file[0]);
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Deferred file attributes ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "hello/hello", LFS_O_RDWR) => 0;
    
    struct lfs_attr attr[] = {
        {'B', "gggg", 4},
        {'C', "",     0},
        {'D', "hhhh", 4},
    };

    lfs_file_setattrs(&lfs, &file[0], attr, 3) => 0;
    lfs_file_getattrs(&lfs, &file[0], (struct lfs_attr[]){
            {'B', buffer,    9},
            {'C', buffer+9,  9},
            {'D', buffer+18, 9}}, 3) => 0;
    memcmp(buffer,    "gggg\0\0\0\0\0",     9) => 0;
    memcmp(buffer+9,  "\0\0\0\0\0\0\0\0\0", 9) => 0;
    memcmp(buffer+18, "hhhh\0\0\0\0\0",     9) => 0;

    lfs_getattrs(&lfs, "hello/hello", (struct lfs_attr[]){
            {'B', buffer,    9},
            {'C', buffer+9,  9},
            {'D', buffer+18, 9}}, 3) => 0;
    memcmp(buffer,    "fffffffff",          9) => 0;
    memcmp(buffer+9,  "ccccc\0\0\0\0",      9) => 0;
    memcmp(buffer+18, "\0\0\0\0\0\0\0\0\0", 9) => 0;

    lfs_file_sync(&lfs, &file[0]) => 0;
    lfs_getattrs(&lfs, "hello/hello", (struct lfs_attr[]){
            {'B', buffer,    9},
            {'C', buffer+9,  9},
            {'D', buffer+18, 9}}, 3) => 0;
    memcmp(buffer,    "gggg\0\0\0\0\0",     9) => 0;
    memcmp(buffer+9,  "\0\0\0\0\0\0\0\0\0", 9) => 0;
    memcmp(buffer+18, "hhhh\0\0\0\0\0",     9) => 0;

    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Results ---"
tests/stats.py
