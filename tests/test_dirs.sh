#!/bin/bash
set -eu

LARGESIZE=128

echo "=== Directory tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST

echo "--- Root directory ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Directory creation ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "potato") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- File creation ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "burito", LFS1_O_CREAT | LFS1_O_WRONLY) => 0;
    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Directory iteration ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "potato") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS1_TYPE_REG;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Directory failures ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "potato") => LFS1_ERR_EXIST;
    lfs1_dir_open(&lfs1, &dir[0], "tomato") => LFS1_ERR_NOENT;
    lfs1_dir_open(&lfs1, &dir[0], "burito") => LFS1_ERR_NOTDIR;
    lfs1_file_open(&lfs1, &file[0], "tomato", LFS1_O_RDONLY) => LFS1_ERR_NOENT;
    lfs1_file_open(&lfs1, &file[0], "potato", LFS1_O_RDONLY) => LFS1_ERR_ISDIR;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Nested directories ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "potato/baked") => 0;
    lfs1_mkdir(&lfs1, "potato/sweet") => 0;
    lfs1_mkdir(&lfs1, "potato/fried") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "potato") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Multi-block directory ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "cactus") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "cactus/test%d", i);
        lfs1_mkdir(&lfs1, (char*)buffer) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "cactus") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "test%d", i);
        lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS1_TYPE_DIR;
    }
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Directory remove ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "potato") => LFS1_ERR_NOTEMPTY;
    lfs1_remove(&lfs1, "potato/sweet") => 0;
    lfs1_remove(&lfs1, "potato/baked") => 0;
    lfs1_remove(&lfs1, "potato/fried") => 0;

    lfs1_dir_open(&lfs1, &dir[0], "potato") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;

    lfs1_remove(&lfs1, "potato") => 0;

    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS1_TYPE_REG;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS1_TYPE_REG;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Directory rename ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "coldpotato") => 0;
    lfs1_mkdir(&lfs1, "coldpotato/baked") => 0;
    lfs1_mkdir(&lfs1, "coldpotato/sweet") => 0;
    lfs1_mkdir(&lfs1, "coldpotato/fried") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_rename(&lfs1, "coldpotato", "hotpotato") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "hotpotato") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "warmpotato") => 0;
    lfs1_mkdir(&lfs1, "warmpotato/mushy") => 0;
    lfs1_rename(&lfs1, "hotpotato", "warmpotato") => LFS1_ERR_NOTEMPTY;

    lfs1_remove(&lfs1, "warmpotato/mushy") => 0;
    lfs1_rename(&lfs1, "hotpotato", "warmpotato") => 0;

    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "warmpotato") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "coldpotato") => 0;
    lfs1_rename(&lfs1, "warmpotato/baked", "coldpotato/baked") => 0;
    lfs1_rename(&lfs1, "warmpotato/sweet", "coldpotato/sweet") => 0;
    lfs1_rename(&lfs1, "warmpotato/fried", "coldpotato/fried") => 0;
    lfs1_remove(&lfs1, "coldpotato") => LFS1_ERR_NOTEMPTY;
    lfs1_remove(&lfs1, "warmpotato") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "coldpotato") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Recursive remove ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "coldpotato") => LFS1_ERR_NOTEMPTY;

    lfs1_dir_open(&lfs1, &dir[0], "coldpotato") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;

    while (true) {
        int err = lfs1_dir_read(&lfs1, &dir[0], &info);
        err >= 0 => 1;
        if (err == 0) {
            break;
        }

        strcpy((char*)buffer, "coldpotato/");
        strcat((char*)buffer, info.name);
        lfs1_remove(&lfs1, (char*)buffer) => 0;
    }

    lfs1_remove(&lfs1, "coldpotato") => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS1_TYPE_REG;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Multi-block rename ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "cactus/test%d", i);
        sprintf((char*)wbuffer, "cactus/tedd%d", i);
        lfs1_rename(&lfs1, (char*)buffer, (char*)wbuffer) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "cactus") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "tedd%d", i);
        lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS1_TYPE_DIR;
    }
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Multi-block remove ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "cactus") => LFS1_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "cactus/tedd%d", i);
        lfs1_remove(&lfs1, (char*)buffer) => 0;
    }

    lfs1_remove(&lfs1, "cactus") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS1_TYPE_REG;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Multi-block directory with files ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "prickly-pear") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "prickly-pear/test%d", i);
        lfs1_file_open(&lfs1, &file[0], (char*)buffer,
                LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
        size = 6;
        memcpy(wbuffer, "Hello", size);
        lfs1_file_write(&lfs1, &file[0], wbuffer, size) => size;
        lfs1_file_close(&lfs1, &file[0]) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "prickly-pear") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "test%d", i);
        lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS1_TYPE_REG;
        info.size => 6;
    }
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Multi-block rename with files ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "prickly-pear/test%d", i);
        sprintf((char*)wbuffer, "prickly-pear/tedd%d", i);
        lfs1_rename(&lfs1, (char*)buffer, (char*)wbuffer) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "prickly-pear") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "tedd%d", i);
        lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS1_TYPE_REG;
        info.size => 6;
    }
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Multi-block remove with files ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_remove(&lfs1, "prickly-pear") => LFS1_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "prickly-pear/tedd%d", i);
        lfs1_remove(&lfs1, (char*)buffer) => 0;
    }

    lfs1_remove(&lfs1, "prickly-pear") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS1_TYPE_REG;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Results ---"
tests/stats.py
