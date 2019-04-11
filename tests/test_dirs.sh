#!/bin/bash
set -eu

LARGESIZE=128

echo "=== Directory tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST

echo "--- Root directory ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "/") => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory creation ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "potato") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- File creation ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file[0], "burito", LFS2_O_CREAT | LFS2_O_WRONLY) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory iteration ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "/") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "potato") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory failures ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "potato") => LFS2_ERR_EXIST;
    lfs2_dir_open(&lfs2, &dir[0], "tomato") => LFS2_ERR_NOENT;
    lfs2_dir_open(&lfs2, &dir[0], "burito") => LFS2_ERR_NOTDIR;
    lfs2_file_open(&lfs2, &file[0], "tomato", LFS2_O_RDONLY) => LFS2_ERR_NOENT;
    lfs2_file_open(&lfs2, &file[0], "potato", LFS2_O_RDONLY) => LFS2_ERR_ISDIR;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Nested directories ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "potato/baked") => 0;
    lfs2_mkdir(&lfs2, "potato/sweet") => 0;
    lfs2_mkdir(&lfs2, "potato/fried") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "potato") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block directory ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "cactus") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "cactus/test%03d", i);
        lfs2_mkdir(&lfs2, (char*)buffer) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "cactus") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "test%03d", i);
        lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS2_TYPE_DIR;
    }
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory remove ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "potato") => LFS2_ERR_NOTEMPTY;
    lfs2_remove(&lfs2, "potato/sweet") => 0;
    lfs2_remove(&lfs2, "potato/baked") => 0;
    lfs2_remove(&lfs2, "potato/fried") => 0;

    lfs2_dir_open(&lfs2, &dir[0], "potato") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;

    lfs2_remove(&lfs2, "potato") => 0;

    lfs2_dir_open(&lfs2, &dir[0], "/") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "/") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Directory rename ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "coldpotato") => 0;
    lfs2_mkdir(&lfs2, "coldpotato/baked") => 0;
    lfs2_mkdir(&lfs2, "coldpotato/sweet") => 0;
    lfs2_mkdir(&lfs2, "coldpotato/fried") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_rename(&lfs2, "coldpotato", "hotpotato") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "hotpotato") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "warmpotato") => 0;
    lfs2_mkdir(&lfs2, "warmpotato/mushy") => 0;
    lfs2_rename(&lfs2, "hotpotato", "warmpotato") => LFS2_ERR_NOTEMPTY;

    lfs2_remove(&lfs2, "warmpotato/mushy") => 0;
    lfs2_rename(&lfs2, "hotpotato", "warmpotato") => 0;

    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "warmpotato") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "coldpotato") => 0;
    lfs2_rename(&lfs2, "warmpotato/baked", "coldpotato/baked") => 0;
    lfs2_rename(&lfs2, "warmpotato/sweet", "coldpotato/sweet") => 0;
    lfs2_rename(&lfs2, "warmpotato/fried", "coldpotato/fried") => 0;
    lfs2_remove(&lfs2, "coldpotato") => LFS2_ERR_NOTEMPTY;
    lfs2_remove(&lfs2, "warmpotato") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "coldpotato") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "baked") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "fried") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "sweet") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Recursive remove ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "coldpotato") => LFS2_ERR_NOTEMPTY;

    lfs2_dir_open(&lfs2, &dir[0], "coldpotato") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;

    while (true) {
        int err = lfs2_dir_read(&lfs2, &dir[0], &info);
        err >= 0 => 1;
        if (err == 0) {
            break;
        }

        strcpy((char*)buffer, "coldpotato/");
        strcat((char*)buffer, info.name);
        lfs2_remove(&lfs2, (char*)buffer) => 0;
    }

    lfs2_remove(&lfs2, "coldpotato") => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "/") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "cactus") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block rename ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "cactus/test%03d", i);
        sprintf((char*)wbuffer, "cactus/tedd%03d", i);
        lfs2_rename(&lfs2, (char*)buffer, (char*)wbuffer) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "cactus") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "tedd%03d", i);
        lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS2_TYPE_DIR;
    }
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block remove ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "cactus") => LFS2_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "cactus/tedd%03d", i);
        lfs2_remove(&lfs2, (char*)buffer) => 0;
    }

    lfs2_remove(&lfs2, "cactus") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "/") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block directory with files ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "prickly-pear") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "prickly-pear/test%03d", i);
        lfs2_file_open(&lfs2, &file[0], (char*)buffer,
                LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
        size = 6;
        memcpy(wbuffer, "Hello", size);
        lfs2_file_write(&lfs2, &file[0], wbuffer, size) => size;
        lfs2_file_close(&lfs2, &file[0]) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "prickly-pear") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "test%03d", i);
        lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS2_TYPE_REG;
        info.size => 6;
    }
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block rename with files ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "prickly-pear/test%03d", i);
        sprintf((char*)wbuffer, "prickly-pear/tedd%03d", i);
        lfs2_rename(&lfs2, (char*)buffer, (char*)wbuffer) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "prickly-pear") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "tedd%03d", i);
        lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        info.type => LFS2_TYPE_REG;
        info.size => 6;
    }
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Multi-block remove with files ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_remove(&lfs2, "prickly-pear") => LFS2_ERR_NOTEMPTY;

    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "prickly-pear/tedd%03d", i);
        lfs2_remove(&lfs2, (char*)buffer) => 0;
    }

    lfs2_remove(&lfs2, "prickly-pear") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir[0], "/") => 0;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS2_TYPE_DIR;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 1;
    strcmp(info.name, "burito") => 0;
    info.type => LFS2_TYPE_REG;
    lfs2_dir_read(&lfs2, &dir[0], &info) => 0;
    lfs2_dir_close(&lfs2, &dir[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Results ---"
tests/stats.py
