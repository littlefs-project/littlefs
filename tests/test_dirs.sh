#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Directory tests ==="

LARGESIZE=128

rm -rf blocks
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

echo "--- Root directory ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Directory creation ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "potato") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- File creation ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "burito", LFS_O_CREAT | LFS_O_WRONLY) => 0;
    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Directory iteration ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS_TYPE_REG;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "potato") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Directory failures ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "potato") => LFS_ERR_EXIST;
    lfs_dir_open(&lfs, &dir, "tomato") => LFS_ERR_NOENT;
    lfs_dir_open(&lfs, &dir, "burito") => LFS_ERR_NOTDIR;
    lfs_file_open(&lfs, &file, "tomato", LFS_O_RDONLY) => LFS_ERR_NOENT;
    lfs_file_open(&lfs, &file, "potato", LFS_O_RDONLY) => LFS_ERR_ISDIR;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Nested directories ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "potato/baked") => 0;
    lfs_mkdir(&lfs, "potato/sweet") => 0;
    lfs_mkdir(&lfs, "potato/fried") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "potato") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Multi-block directory ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "cactus") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "cactus/test%03d", i);
        lfs_mkdir(&lfs, path) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "cactus") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "test%03d", i);
        lfs_dir_read(&lfs, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS_TYPE_DIR;
    }
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Directory remove ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_remove(&lfs, "potato") => LFS_ERR_NOTEMPTY;
    lfs_remove(&lfs, "potato/sweet") => 0;
    lfs_remove(&lfs, "potato/baked") => 0;
    lfs_remove(&lfs, "potato/fried") => 0;

    lfs_dir_open(&lfs, &dir, "potato") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;

    lfs_remove(&lfs, "potato") => 0;

    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS_TYPE_REG;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS_TYPE_REG;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Directory rename ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "coldpotato") => 0;
    lfs_mkdir(&lfs, "coldpotato/baked") => 0;
    lfs_mkdir(&lfs, "coldpotato/sweet") => 0;
    lfs_mkdir(&lfs, "coldpotato/fried") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_rename(&lfs, "coldpotato", "hotpotato") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "hotpotato") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "warmpotato") => 0;
    lfs_mkdir(&lfs, "warmpotato/mushy") => 0;
    lfs_rename(&lfs, "hotpotato", "warmpotato") => LFS_ERR_NOTEMPTY;

    lfs_remove(&lfs, "warmpotato/mushy") => 0;
    lfs_rename(&lfs, "hotpotato", "warmpotato") => 0;

    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "warmpotato") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "coldpotato") => 0;
    lfs_rename(&lfs, "warmpotato/baked", "coldpotato/baked") => 0;
    lfs_rename(&lfs, "warmpotato/sweet", "coldpotato/sweet") => 0;
    lfs_rename(&lfs, "warmpotato/fried", "coldpotato/fried") => 0;
    lfs_remove(&lfs, "coldpotato") => LFS_ERR_NOTEMPTY;
    lfs_remove(&lfs, "warmpotato") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "coldpotato") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Recursive remove ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_remove(&lfs, "coldpotato") => LFS_ERR_NOTEMPTY;

    lfs_dir_open(&lfs, &dir, "coldpotato") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    lfs_dir_read(&lfs, &dir, &info) => 1;

    while (true) {
        int err = lfs_dir_read(&lfs, &dir, &info);
        err >= 0 => 1;
        if (err == 0) {
            break;
        }

        strcpy(path, "coldpotato/");
        strcat(path, info.name);
        lfs_remove(&lfs, path) => 0;
    }

    lfs_remove(&lfs, "coldpotato") => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS_TYPE_REG;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Multi-block rename ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        char oldpath[1024];
        char newpath[1024];
        sprintf(oldpath, "cactus/test%03d", i);
        sprintf(newpath, "cactus/tedd%03d", i);
        lfs_rename(&lfs, oldpath, newpath) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "cactus") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "tedd%03d", i);
        lfs_dir_read(&lfs, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS_TYPE_DIR;
    }
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Multi-block remove ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_remove(&lfs, "cactus") => LFS_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "cactus/tedd%03d", i);
        lfs_remove(&lfs, path) => 0;
    }

    lfs_remove(&lfs, "cactus") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS_TYPE_REG;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Multi-block directory with files ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "prickly-pear") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "prickly-pear/test%03d", i);
        lfs_file_open(&lfs, &file, path, LFS_O_WRONLY | LFS_O_CREAT) => 0;
        lfs_size_t size = 6;
        memcpy(buffer, "Hello", size);
        lfs_file_write(&lfs, &file, buffer, size) => size;
        lfs_file_close(&lfs, &file) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "prickly-pear") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "test%03d", i);
        lfs_dir_read(&lfs, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS_TYPE_REG;
        info.size => 6;
    }
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Multi-block rename with files ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        char oldpath[1024];
        char newpath[1024];
        sprintf(oldpath, "prickly-pear/test%03d", i);
        sprintf(newpath, "prickly-pear/tedd%03d", i);
        lfs_rename(&lfs, oldpath, newpath) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "prickly-pear") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "tedd%03d", i);
        lfs_dir_read(&lfs, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS_TYPE_REG;
        info.size => 6;
    }
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Multi-block remove with files ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_remove(&lfs, "prickly-pear") => LFS_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "prickly-pear/tedd%03d", i);
        lfs_remove(&lfs, path) => 0;
    }

    lfs_remove(&lfs, "prickly-pear") => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS_TYPE_DIR;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS_TYPE_REG;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

scripts/results.py
