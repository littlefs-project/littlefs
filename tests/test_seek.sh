#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Seek tests ==="

SMALLSIZE=4
MEDIUMSIZE=128
LARGESIZE=132

rm -rf blocks
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "hello") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "hello/kitty%03d", i);
        lfs_file_open(&lfs, &file, path,
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND) => 0;

        lfs_size_t size = strlen("kittycatcat");
        memcpy(buffer, "kittycatcat", size);
        for (int j = 0; j < $LARGESIZE; j++) {
            lfs_file_write(&lfs, &file, buffer, size);
        }

        lfs_file_close(&lfs, &file) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Simple dir seek ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;

    lfs_soff_t pos;
    int i;
    for (i = 0; i < $SMALLSIZE; i++) {
        sprintf(path, "kitty%03d", i);
        lfs_dir_read(&lfs, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        pos = lfs_dir_tell(&lfs, &dir);
    }
    pos >= 0 => 1;

    lfs_dir_seek(&lfs, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs_dir_rewind(&lfs, &dir) => 0;
    sprintf(path, "kitty%03d", 0);
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs_dir_seek(&lfs, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Large dir seek ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "hello") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;

    lfs_soff_t pos;
    int i;
    for (i = 0; i < $MEDIUMSIZE; i++) {
        sprintf(path, "kitty%03d", i);
        lfs_dir_read(&lfs, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        pos = lfs_dir_tell(&lfs, &dir);
    }
    pos >= 0 => 1;

    lfs_dir_seek(&lfs, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs_dir_rewind(&lfs, &dir) => 0;
    sprintf(path, "kitty%03d", 0);
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs_dir_seek(&lfs, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Simple file seek ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello/kitty042", LFS_O_RDONLY) => 0;

    lfs_soff_t pos;
    lfs_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $SMALLSIZE; i++) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs_file_tell(&lfs, &file);
    }
    pos >= 0 => 1;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_rewind(&lfs, &file) => 0;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_CUR) => size;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, size, LFS_SEEK_CUR) => 3*size;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, -size, LFS_SEEK_CUR) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, -size, LFS_SEEK_END) >= 0 => 1;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs_file_size(&lfs, &file);
    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_CUR) => size;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Large file seek ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello/kitty042", LFS_O_RDONLY) => 0;

    lfs_soff_t pos;
    lfs_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $MEDIUMSIZE; i++) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs_file_tell(&lfs, &file);
    }
    pos >= 0 => 1;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_rewind(&lfs, &file) => 0;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_CUR) => size;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, size, LFS_SEEK_CUR) => 3*size;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, -size, LFS_SEEK_CUR) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, -size, LFS_SEEK_END) >= 0 => 1;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs_file_size(&lfs, &file);
    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_CUR) => size;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Simple file seek and write ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello/kitty042", LFS_O_RDWR) => 0;

    lfs_soff_t pos;
    lfs_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $SMALLSIZE; i++) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs_file_tell(&lfs, &file);
    }
    pos >= 0 => 1;

    memcpy(buffer, "doggodogdog", size);
    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_write(&lfs, &file, buffer, size) => size;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs_file_rewind(&lfs, &file) => 0;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs_file_seek(&lfs, &file, -size, LFS_SEEK_END) >= 0 => 1;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs_file_size(&lfs, &file);
    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_CUR) => size;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Large file seek and write ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello/kitty042", LFS_O_RDWR) => 0;

    lfs_soff_t pos;
    lfs_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $MEDIUMSIZE; i++) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        if (i != $SMALLSIZE) {
            memcmp(buffer, "kittycatcat", size) => 0;
        }
        pos = lfs_file_tell(&lfs, &file);
    }
    pos >= 0 => 1;

    memcpy(buffer, "doggodogdog", size);
    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_write(&lfs, &file, buffer, size) => size;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs_file_rewind(&lfs, &file) => 0;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs_file_seek(&lfs, &file, pos, LFS_SEEK_SET) => pos;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs_file_seek(&lfs, &file, -size, LFS_SEEK_END) >= 0 => 1;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs_file_size(&lfs, &file);
    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_CUR) => size;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Boundary seek and write ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello/kitty042", LFS_O_RDWR) => 0;

    lfs_size_t size = strlen("hedgehoghog");
    const lfs_soff_t offsets[] = {512, 1020, 513, 1021, 511, 1019};

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        lfs_soff_t off = offsets[i];
        memcpy(buffer, "hedgehoghog", size);
        lfs_file_seek(&lfs, &file, off, LFS_SEEK_SET) => off;
        lfs_file_write(&lfs, &file, buffer, size) => size;
        lfs_file_seek(&lfs, &file, off, LFS_SEEK_SET) => off;
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "hedgehoghog", size) => 0;

        lfs_file_seek(&lfs, &file, 0, LFS_SEEK_SET) => 0;
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;

        lfs_file_sync(&lfs, &file) => 0;
    }

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Out-of-bounds seek ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello/kitty042", LFS_O_RDWR) => 0;

    lfs_size_t size = strlen("kittycatcat");
    lfs_file_size(&lfs, &file) => $LARGESIZE*size;
    lfs_file_seek(&lfs, &file, ($LARGESIZE+$SMALLSIZE)*size,
            LFS_SEEK_SET) => ($LARGESIZE+$SMALLSIZE)*size;
    lfs_file_read(&lfs, &file, buffer, size) => 0;

    memcpy(buffer, "porcupineee", size);
    lfs_file_write(&lfs, &file, buffer, size) => size;

    lfs_file_seek(&lfs, &file, ($LARGESIZE+$SMALLSIZE)*size,
            LFS_SEEK_SET) => ($LARGESIZE+$SMALLSIZE)*size;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "porcupineee", size) => 0;

    lfs_file_seek(&lfs, &file, $LARGESIZE*size,
            LFS_SEEK_SET) => $LARGESIZE*size;
    lfs_file_read(&lfs, &file, buffer, size) => size;
    memcmp(buffer, "\0\0\0\0\0\0\0\0\0\0\0", size) => 0;

    lfs_file_seek(&lfs, &file, -(($LARGESIZE+$SMALLSIZE)*size),
            LFS_SEEK_CUR) => LFS_ERR_INVAL;
    lfs_file_tell(&lfs, &file) => ($LARGESIZE+1)*size;

    lfs_file_seek(&lfs, &file, -(($LARGESIZE+2*$SMALLSIZE)*size),
            LFS_SEEK_END) => LFS_ERR_INVAL;
    lfs_file_tell(&lfs, &file) => ($LARGESIZE+1)*size;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Inline write and seek ---"
for SIZE in $SMALLSIZE $MEDIUMSIZE $LARGESIZE
do
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello/tinykitty$SIZE",
            LFS_O_RDWR | LFS_O_CREAT) => 0;
    int j = 0;
    int k = 0;

    memcpy(buffer, "abcdefghijklmnopqrstuvwxyz", 26);
    for (unsigned i = 0; i < $SIZE; i++) {
        lfs_file_write(&lfs, &file, &buffer[j++ % 26], 1) => 1;
        lfs_file_tell(&lfs, &file) => i+1;
        lfs_file_size(&lfs, &file) => i+1;
    }

    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_SET) => 0;
    lfs_file_tell(&lfs, &file) => 0;
    lfs_file_size(&lfs, &file) => $SIZE;
    for (unsigned i = 0; i < $SIZE; i++) {
        uint8_t c;
        lfs_file_read(&lfs, &file, &c, 1) => 1;
        c => buffer[k++ % 26];
    }

    lfs_file_sync(&lfs, &file) => 0;
    lfs_file_tell(&lfs, &file) => $SIZE;
    lfs_file_size(&lfs, &file) => $SIZE;

    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_SET) => 0;
    for (unsigned i = 0; i < $SIZE; i++) {
        lfs_file_write(&lfs, &file, &buffer[j++ % 26], 1) => 1;
        lfs_file_tell(&lfs, &file) => i+1;
        lfs_file_size(&lfs, &file) => $SIZE;
        lfs_file_sync(&lfs, &file) => 0;
        lfs_file_tell(&lfs, &file) => i+1;
        lfs_file_size(&lfs, &file) => $SIZE;
        if (i < $SIZE-2) {
            uint8_t c[3];
            lfs_file_seek(&lfs, &file, -1, LFS_SEEK_CUR) => i;
            lfs_file_read(&lfs, &file, &c, 3) => 3;
            lfs_file_tell(&lfs, &file) => i+3;
            lfs_file_size(&lfs, &file) => $SIZE;
            lfs_file_seek(&lfs, &file, i+1, LFS_SEEK_SET) => i+1;
            lfs_file_tell(&lfs, &file) => i+1;
            lfs_file_size(&lfs, &file) => $SIZE;
        }
    }

    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_SET) => 0;
    lfs_file_tell(&lfs, &file) => 0;
    lfs_file_size(&lfs, &file) => $SIZE;
    for (unsigned i = 0; i < $SIZE; i++) {
        uint8_t c;
        lfs_file_read(&lfs, &file, &c, 1) => 1;
        c => buffer[k++ % 26];
    }

    lfs_file_sync(&lfs, &file) => 0;
    lfs_file_tell(&lfs, &file) => $SIZE;
    lfs_file_size(&lfs, &file) => $SIZE;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
done

scripts/results.py
