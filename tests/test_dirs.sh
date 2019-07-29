#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Directory tests ==="

LARGESIZE=128

rm -rf blocks
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST

echo "--- Root directory ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory creation ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "potato") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- File creation ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "burito", LFS2_O_CREAT | LFS2_O_WRONLY) => 0;
    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory iteration ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "potato") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory failures ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "potato") => LFS2_ERR_EXIST;
    lfs2_dir_open(&lfs2, &dir, "tomato") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir, "burito") => LFS2_ERR_NOTDIR;
    lfs2_file_open(&lfs2, &file, "tomato", LFS2_O_RDONLY) => LFS2_ERR_NOENT;
    lfs2_file_open(&lfs2, &file, "potato", LFS2_O_RDONLY) => LFS2_ERR_ISDIR;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Nested directories ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "potato/baked") => 0;
    lfs2_mkdir(&lfs2, "potato/sweet") => 0;
    lfs2_mkdir(&lfs2, "potato/fried") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "potato") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block directory ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "cactus") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "cactus/test%03d", i);
        lfs2_mkdir(&lfs2, path) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "cactus") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "test%03d", i);
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS2_TYPE_DIR;
    }
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory remove ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "potato") => LFS2_ERR_NOTEMPTY;
    lfs2_remove(&lfs2, "potato/sweet") => 0;
    lfs2_remove(&lfs2, "potato/baked") => 0;
    lfs2_remove(&lfs2, "potato/fried") => 0;

    lfs2_dir_open(&lfs2, &dir, "potato") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;

    lfs2_remove(&lfs2, "potato") => 0;

    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory rename ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "coldpotato") => 0;
    lfs2_mkdir(&lfs2, "coldpotato/baked") => 0;
    lfs2_mkdir(&lfs2, "coldpotato/sweet") => 0;
    lfs2_mkdir(&lfs2, "coldpotato/fried") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "coldpotato", "hotpotato") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "hotpotato") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "warmpotato") => 0;
    lfs2_mkdir(&lfs2, "warmpotato/mushy") => 0;
    lfs2_rename(&lfs2, "hotpotato", "warmpotato") => LFS2_ERR_NOTEMPTY;

    lfs2_remove(&lfs2, "warmpotato/mushy") => 0;
    lfs2_rename(&lfs2, "hotpotato", "warmpotato") => 0;

    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "warmpotato") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "coldpotato") => 0;
    lfs2_rename(&lfs2, "warmpotato/baked", "coldpotato/baked") => 0;
    lfs2_rename(&lfs2, "warmpotato/sweet", "coldpotato/sweet") => 0;
    lfs2_rename(&lfs2, "warmpotato/fried", "coldpotato/fried") => 0;
    lfs2_remove(&lfs2, "coldpotato") => LFS2_ERR_NOTEMPTY;
    lfs2_remove(&lfs2, "warmpotato") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "coldpotato") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Recursive remove ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "coldpotato") => LFS2_ERR_NOTEMPTY;

    lfs2_dir_open(&lfs2, &dir, "coldpotato") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;

    while (true) {
        int err = lfs2_dir_read(&lfs2, &dir, &info);
        err >= 0 => 1;
        if (err == 0) {
            break;
        }

        strcpy(path, "coldpotato/");
        strcat(path, info.name);
        lfs2_remove(&lfs2, path) => 0;
    }

    lfs2_remove(&lfs2, "coldpotato") => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block rename ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        char oldpath[1024];
        char newpath[1024];
        sprintf(oldpath, "cactus/test%03d", i);
        sprintf(newpath, "cactus/tedd%03d", i);
        lfs2_rename(&lfs2, oldpath, newpath) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "cactus") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "tedd%03d", i);
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS2_TYPE_DIR;
    }
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block remove ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "cactus") => LFS2_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "cactus/tedd%03d", i);
        lfs2_remove(&lfs2, path) => 0;
    }

    lfs2_remove(&lfs2, "cactus") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block directory with files ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "prickly-pear") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "prickly-pear/test%03d", i);
        lfs2_file_open(&lfs2, &file, path, LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
        lfs2_size_t size = 6;
        memcpy(buffer, "Hello", size);
        lfs2_file_write(&lfs2, &file, buffer, size) => size;
        lfs2_file_close(&lfs2, &file) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "prickly-pear") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "test%03d", i);
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS2_TYPE_REG;
        info.size => 6;
    }
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block rename with files ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        char oldpath[1024];
        char newpath[1024];
        sprintf(oldpath, "prickly-pear/test%03d", i);
        sprintf(newpath, "prickly-pear/tedd%03d", i);
        lfs2_rename(&lfs2, oldpath, newpath) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "prickly-pear") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "tedd%03d", i);
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        info.type => LFS2_TYPE_REG;
        info.size => 6;
    }
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block remove with files ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "prickly-pear") => LFS2_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "prickly-pear/tedd%03d", i);
        lfs2_remove(&lfs2, path) => 0;
    }

    lfs2_remove(&lfs2, "prickly-pear") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

scripts/results.py
