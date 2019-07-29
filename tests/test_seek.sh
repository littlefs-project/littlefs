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
    lfs2_format(&lfs2, &cfg) => 0;
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "hello") => 0;
    for (int i = 0; i < $LARGESIZE; i++) {
        sprintf(path, "hello/kitty%03d", i);
        lfs2_file_open(&lfs2, &file, path,
                LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_APPEND) => 0;

        lfs2_size_t size = strlen("kittycatcat");
        memcpy(buffer, "kittycatcat", size);
        for (int j = 0; j < $LARGESIZE; j++) {
            lfs2_file_write(&lfs2, &file, buffer, size);
        }

        lfs2_file_close(&lfs2, &file) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Simple dir seek ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;

    lfs2_soff_t pos;
    int i;
    for (i = 0; i < $SMALLSIZE; i++) {
        sprintf(path, "kitty%03d", i);
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        pos = lfs2_dir_tell(&lfs2, &dir);
    }
    pos >= 0 => 1;

    lfs2_dir_seek(&lfs2, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs2_dir_rewind(&lfs2, &dir) => 0;
    sprintf(path, "kitty%03d", 0);
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs2_dir_seek(&lfs2, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Large dir seek ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "hello") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;

    lfs2_soff_t pos;
    int i;
    for (i = 0; i < $MEDIUMSIZE; i++) {
        sprintf(path, "kitty%03d", i);
        lfs2_dir_read(&lfs2, &dir, &info) => 1;
        strcmp(info.name, path) => 0;
        pos = lfs2_dir_tell(&lfs2, &dir);
    }
    pos >= 0 => 1;

    lfs2_dir_seek(&lfs2, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs2_dir_rewind(&lfs2, &dir) => 0;
    sprintf(path, "kitty%03d", 0);
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, ".") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "..") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs2_dir_seek(&lfs2, &dir, pos) => 0;
    sprintf(path, "kitty%03d", i);
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, path) => 0;

    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Simple file seek ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello/kitty042", LFS2_O_RDONLY) => 0;

    lfs2_soff_t pos;
    lfs2_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $SMALLSIZE; i++) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs2_file_tell(&lfs2, &file);
    }
    pos >= 0 => 1;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_rewind(&lfs2, &file) => 0;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_CUR) => size;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, size, LFS2_SEEK_CUR) => 3*size;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, -size, LFS2_SEEK_CUR) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, -size, LFS2_SEEK_END) >= 0 => 1;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs2_file_size(&lfs2, &file);
    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_CUR) => size;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Large file seek ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello/kitty042", LFS2_O_RDONLY) => 0;

    lfs2_soff_t pos;
    lfs2_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $MEDIUMSIZE; i++) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs2_file_tell(&lfs2, &file);
    }
    pos >= 0 => 1;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_rewind(&lfs2, &file) => 0;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_CUR) => size;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, size, LFS2_SEEK_CUR) => 3*size;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, -size, LFS2_SEEK_CUR) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, -size, LFS2_SEEK_END) >= 0 => 1;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs2_file_size(&lfs2, &file);
    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_CUR) => size;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Simple file seek and write ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello/kitty042", LFS2_O_RDWR) => 0;

    lfs2_soff_t pos;
    lfs2_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $SMALLSIZE; i++) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;
        pos = lfs2_file_tell(&lfs2, &file);
    }
    pos >= 0 => 1;

    memcpy(buffer, "doggodogdog", size);
    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_write(&lfs2, &file, buffer, size) => size;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs2_file_rewind(&lfs2, &file) => 0;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs2_file_seek(&lfs2, &file, -size, LFS2_SEEK_END) >= 0 => 1;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs2_file_size(&lfs2, &file);
    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_CUR) => size;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Large file seek and write ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello/kitty042", LFS2_O_RDWR) => 0;

    lfs2_soff_t pos;
    lfs2_size_t size = strlen("kittycatcat");
    for (int i = 0; i < $MEDIUMSIZE; i++) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        if (i != $SMALLSIZE) {
            memcmp(buffer, "kittycatcat", size) => 0;
        }
        pos = lfs2_file_tell(&lfs2, &file);
    }
    pos >= 0 => 1;

    memcpy(buffer, "doggodogdog", size);
    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_write(&lfs2, &file, buffer, size) => size;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs2_file_rewind(&lfs2, &file) => 0;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    lfs2_file_seek(&lfs2, &file, pos, LFS2_SEEK_SET) => pos;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "doggodogdog", size) => 0;

    lfs2_file_seek(&lfs2, &file, -size, LFS2_SEEK_END) >= 0 => 1;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "kittycatcat", size) => 0;

    size = lfs2_file_size(&lfs2, &file);
    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_CUR) => size;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Boundary seek and write ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello/kitty042", LFS2_O_RDWR) => 0;

    lfs2_size_t size = strlen("hedgehoghog");
    const lfs2_soff_t offsets[] = {512, 1020, 513, 1021, 511, 1019};

    for (unsigned i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        lfs2_soff_t off = offsets[i];
        memcpy(buffer, "hedgehoghog", size);
        lfs2_file_seek(&lfs2, &file, off, LFS2_SEEK_SET) => off;
        lfs2_file_write(&lfs2, &file, buffer, size) => size;
        lfs2_file_seek(&lfs2, &file, off, LFS2_SEEK_SET) => off;
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "hedgehoghog", size) => 0;

        lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_SET) => 0;
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "kittycatcat", size) => 0;

        lfs2_file_sync(&lfs2, &file) => 0;
    }

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Out-of-bounds seek ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello/kitty042", LFS2_O_RDWR) => 0;

    lfs2_size_t size = strlen("kittycatcat");
    lfs2_file_size(&lfs2, &file) => $LARGESIZE*size;
    lfs2_file_seek(&lfs2, &file, ($LARGESIZE+$SMALLSIZE)*size,
            LFS2_SEEK_SET) => ($LARGESIZE+$SMALLSIZE)*size;
    lfs2_file_read(&lfs2, &file, buffer, size) => 0;

    memcpy(buffer, "porcupineee", size);
    lfs2_file_write(&lfs2, &file, buffer, size) => size;

    lfs2_file_seek(&lfs2, &file, ($LARGESIZE+$SMALLSIZE)*size,
            LFS2_SEEK_SET) => ($LARGESIZE+$SMALLSIZE)*size;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "porcupineee", size) => 0;

    lfs2_file_seek(&lfs2, &file, $LARGESIZE*size,
            LFS2_SEEK_SET) => $LARGESIZE*size;
    lfs2_file_read(&lfs2, &file, buffer, size) => size;
    memcmp(buffer, "\0\0\0\0\0\0\0\0\0\0\0", size) => 0;

    lfs2_file_seek(&lfs2, &file, -(($LARGESIZE+$SMALLSIZE)*size),
            LFS2_SEEK_CUR) => LFS2_ERR_INVAL;
    lfs2_file_tell(&lfs2, &file) => ($LARGESIZE+1)*size;

    lfs2_file_seek(&lfs2, &file, -(($LARGESIZE+2*$SMALLSIZE)*size),
            LFS2_SEEK_END) => LFS2_ERR_INVAL;
    lfs2_file_tell(&lfs2, &file) => ($LARGESIZE+1)*size;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Inline write and seek ---"
for SIZE in $SMALLSIZE $MEDIUMSIZE $LARGESIZE
do
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello/tinykitty$SIZE",
            LFS2_O_RDWR | LFS2_O_CREAT) => 0;
    int j = 0;
    int k = 0;

    memcpy(buffer, "abcdefghijklmnopqrstuvwxyz", 26);
    for (unsigned i = 0; i < $SIZE; i++) {
        lfs2_file_write(&lfs2, &file, &buffer[j++ % 26], 1) => 1;
        lfs2_file_tell(&lfs2, &file) => i+1;
        lfs2_file_size(&lfs2, &file) => i+1;
    }

    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_SET) => 0;
    lfs2_file_tell(&lfs2, &file) => 0;
    lfs2_file_size(&lfs2, &file) => $SIZE;
    for (unsigned i = 0; i < $SIZE; i++) {
        uint8_t c;
        lfs2_file_read(&lfs2, &file, &c, 1) => 1;
        c => buffer[k++ % 26];
    }

    lfs2_file_sync(&lfs2, &file) => 0;
    lfs2_file_tell(&lfs2, &file) => $SIZE;
    lfs2_file_size(&lfs2, &file) => $SIZE;

    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_SET) => 0;
    for (unsigned i = 0; i < $SIZE; i++) {
        lfs2_file_write(&lfs2, &file, &buffer[j++ % 26], 1) => 1;
        lfs2_file_tell(&lfs2, &file) => i+1;
        lfs2_file_size(&lfs2, &file) => $SIZE;
        lfs2_file_sync(&lfs2, &file) => 0;
        lfs2_file_tell(&lfs2, &file) => i+1;
        lfs2_file_size(&lfs2, &file) => $SIZE;
        if (i < $SIZE-2) {
            uint8_t c[3];
            lfs2_file_seek(&lfs2, &file, -1, LFS2_SEEK_CUR) => i;
            lfs2_file_read(&lfs2, &file, &c, 3) => 3;
            lfs2_file_tell(&lfs2, &file) => i+3;
            lfs2_file_size(&lfs2, &file) => $SIZE;
            lfs2_file_seek(&lfs2, &file, i+1, LFS2_SEEK_SET) => i+1;
            lfs2_file_tell(&lfs2, &file) => i+1;
            lfs2_file_size(&lfs2, &file) => $SIZE;
        }
    }

    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_SET) => 0;
    lfs2_file_tell(&lfs2, &file) => 0;
    lfs2_file_size(&lfs2, &file) => $SIZE;
    for (unsigned i = 0; i < $SIZE; i++) {
        uint8_t c;
        lfs2_file_read(&lfs2, &file, &c, 1) => 1;
        c => buffer[k++ % 26];
    }

    lfs2_file_sync(&lfs2, &file) => 0;
    lfs2_file_tell(&lfs2, &file) => $SIZE;
    lfs2_file_size(&lfs2, &file) => $SIZE;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
done

scripts/results.py
