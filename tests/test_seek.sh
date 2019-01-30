#!/bin/bash
set -eu

SMALLSIZE=4
MEDIUMSIZE=128
LARGESIZE=132

echo "=== Seek tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "hello") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf((char*)buffer, "hello/kitty%d", i);
        lfs1_file_open(&lfs1, &file[0], (char*)buffer,
                LFS1_O_WRONLY | LFS1_O_CREAT | LFS1_O_APPEND) => 0;

        size = strlen("kittycatcat");
        memcpy(buffer, "kittycatcat", size);
        for (int j = 0; j < $LARGESIZE; j++) {
            lfs1_file_write(&lfs1, &file[0], buffer, size);
        }

        lfs1_file_close(&lfs1, &file[0]) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Simple dir seek ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "hello") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;

    lfs1_soff_t pos;
    int i;
    for (i = 0; i < $SMALLSIZE; i++) {
        sprintf((char*)buffer, "kitty%d", i);
        lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        pos = lfs1_dir_tell(&lfs1, &dir[0]);
    }
    pos >= 0 => 1;

    lfs1_dir_seek(&lfs1, &dir[0], pos) => 0;
    sprintf((char*)buffer, "kitty%d", i);
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, (char*)buffer) => 0;

    lfs1_dir_rewind(&lfs1, &dir[0]) => 0;
    sprintf((char*)buffer, "kitty%d", 0);
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, (char*)buffer) => 0;

    lfs1_dir_seek(&lfs1, &dir[0], pos) => 0;
    sprintf((char*)buffer, "kitty%d", i);
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, (char*)buffer) => 0;

    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Large dir seek ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_dir_open(&lfs1, &dir[0], "hello") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;

    lfs1_soff_t pos;
    int i;
    for (i = 0; i < $MEDIUMSIZE; i++) {
        sprintf((char*)buffer, "kitty%d", i);
        lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
        strcmp(info.name, (char*)buffer) => 0;
        pos = lfs1_dir_tell(&lfs1, &dir[0]);
    }
    pos >= 0 => 1;

    lfs1_dir_seek(&lfs1, &dir[0], pos) => 0;
    sprintf((char*)buffer, "kitty%d", i);
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, (char*)buffer) => 0;

    lfs1_dir_rewind(&lfs1, &dir[0]) => 0;
    sprintf((char*)buffer, "kitty%d", 0);
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, (char*)buffer) => 0;

    lfs1_dir_seek(&lfs1, &dir[0], pos) => 0;
    sprintf((char*)buffer, "kitty%d", i);
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, (char*)buffer) => 0;

    lfs1_dir_close(&lfs1, &dir[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Simple file seek ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "hello/kitty42", LFS1_O_RDONLY) => 0;

    lfs1_soff_t pos;
    size = strlen("kittycatcat");
    for (int i = 0; i < $SMALLSIZE; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs1_file_tell(&lfs1, &file[0]);
    }
    pos >= 0 => 1;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_rewind(&lfs1, &file[0]) => 0;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], 0, LFS1_SEEK_CUR) => size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], size, LFS1_SEEK_CUR) => 3*size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], -size, LFS1_SEEK_CUR) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], -size, LFS1_SEEK_END) >= 0 => 1;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs1_file_size(&lfs1, &file[0]);
    lfs1_file_seek(&lfs1, &file[0], 0, LFS1_SEEK_CUR) => size;

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Large file seek ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "hello/kitty42", LFS1_O_RDONLY) => 0;

    lfs1_soff_t pos;
    size = strlen("kittycatcat");
    for (int i = 0; i < $MEDIUMSIZE; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs1_file_tell(&lfs1, &file[0]);
    }
    pos >= 0 => 1;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_rewind(&lfs1, &file[0]) => 0;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], 0, LFS1_SEEK_CUR) => size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], size, LFS1_SEEK_CUR) => 3*size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], -size, LFS1_SEEK_CUR) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], -size, LFS1_SEEK_END) >= 0 => 1;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs1_file_size(&lfs1, &file[0]);
    lfs1_file_seek(&lfs1, &file[0], 0, LFS1_SEEK_CUR) => size;

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Simple file seek and write ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "hello/kitty42", LFS1_O_RDWR) => 0;

    lfs1_soff_t pos;
    size = strlen("kittycatcat");
    for (int i = 0; i < $SMALLSIZE; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs1_file_tell(&lfs1, &file[0]);
    }
    pos >= 0 => 1;

    memcpy(buffer, "doggodogdog", size);
    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_write(&lfs1, &file[0], buffer, size) => size;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs1_file_rewind(&lfs1, &file[0]) => 0;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], -size, LFS1_SEEK_END) >= 0 => 1;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs1_file_size(&lfs1, &file[0]);
    lfs1_file_seek(&lfs1, &file[0], 0, LFS1_SEEK_CUR) => size;

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Large file seek and write ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "hello/kitty42", LFS1_O_RDWR) => 0;

    lfs1_soff_t pos;
    size = strlen("kittycatcat");
    for (int i = 0; i < $MEDIUMSIZE; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
        if (i != $SMALLSIZE) {
            memcmp(buffer, "kittycatcat", size) => 0;
        }
        pos = lfs1_file_tell(&lfs1, &file[0]);
    }
    pos >= 0 => 1;

    memcpy(buffer, "doggodogdog", size);
    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_write(&lfs1, &file[0], buffer, size) => size;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs1_file_rewind(&lfs1, &file[0]) => 0;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], pos, LFS1_SEEK_SET) => pos;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], -size, LFS1_SEEK_END) >= 0 => 1;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs1_file_size(&lfs1, &file[0]);
    lfs1_file_seek(&lfs1, &file[0], 0, LFS1_SEEK_CUR) => size;

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Boundary seek and write ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "hello/kitty42", LFS1_O_RDWR) => 0;

    size = strlen("hedgehoghog");
    const lfs1_soff_t offsets[] = {512, 1020, 513, 1021, 511, 1019};

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        lfs1_soff_t off = offsets[i];
        memcpy(buffer, "hedgehoghog", size);
        lfs1_file_seek(&lfs1, &file[0], off, LFS1_SEEK_SET) => off;
        lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
        lfs1_file_seek(&lfs1, &file[0], off, LFS1_SEEK_SET) => off;
        lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
        memcmp(buffer, "hedgehoghog", size) => 0;

        lfs1_file_seek(&lfs1, &file[0], 0, LFS1_SEEK_SET) => 0;
        lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;

        lfs1_file_sync(&lfs1, &file[0]) => 0;
    }

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Out-of-bounds seek ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "hello/kitty42", LFS1_O_RDWR) => 0;

    size = strlen("kittycatcat");
    lfs1_file_size(&lfs1, &file[0]) => $LARGESIZE*size;
    lfs1_file_seek(&lfs1, &file[0], ($LARGESIZE+$SMALLSIZE)*size,
            LFS1_SEEK_SET) => ($LARGESIZE+$SMALLSIZE)*size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => 0;

    memcpy(buffer, "porcupineee", size);
    lfs1_file_write(&lfs1, &file[0], buffer, size) => size;

    lfs1_file_seek(&lfs1, &file[0], ($LARGESIZE+$SMALLSIZE)*size,
            LFS1_SEEK_SET) => ($LARGESIZE+$SMALLSIZE)*size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "porcupineee", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], $LARGESIZE*size,
            LFS1_SEEK_SET) => $LARGESIZE*size;
    lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
    memcmp(buffer, "\0\0\0\0\0\0\0\0\0\0\0", size) => 0;

    lfs1_file_seek(&lfs1, &file[0], -(($LARGESIZE+$SMALLSIZE)*size),
            LFS1_SEEK_CUR) => LFS1_ERR_INVAL;
    lfs1_file_tell(&lfs1, &file[0]) => ($LARGESIZE+1)*size;

    lfs1_file_seek(&lfs1, &file[0], -(($LARGESIZE+2*$SMALLSIZE)*size),
            LFS1_SEEK_END) => LFS1_ERR_INVAL;
    lfs1_file_tell(&lfs1, &file[0]) => ($LARGESIZE+1)*size;

    lfs1_file_close(&lfs1, &file[0]) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Results ---"
tests/stats.py
