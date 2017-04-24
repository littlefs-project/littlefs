#!/bin/bash
set -eu

SMALLSIZE=32
MEDIUMSIZE=8192
LARGESIZE=262144

echo "=== File tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

echo "--- Simple file test ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "hello", LFS_O_WRONLY | LFS_O_CREAT) => 0;
    size = strlen("Hello World!\n");
    memcpy(wbuffer, "Hello World!\n", size);
    lfs_file_write(&lfs, &file[0], wbuffer, size) => size;
    lfs_file_close(&lfs, &file[0]) => 0;

    lfs_file_open(&lfs, &file[0], "hello", LFS_O_RDONLY) => 0;
    size = strlen("Hello World!\n");
    lfs_file_read(&lfs, &file[0], rbuffer, size) => size;
    memcmp(rbuffer, wbuffer, size) => 0;
    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST

w_test() {
tests/test.py << TEST
    lfs_size_t size = $1;
    lfs_size_t chunk = 31;
    srand(0);
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "$2", LFS_O_WRONLY | LFS_O_CREAT) => 0;
    for (lfs_size_t i = 0; i < size; i += chunk) {
        chunk = (chunk < size - i) ? chunk : size - i;
        for (lfs_size_t b = 0; b < chunk; b++) {
            buffer[b] = rand() & 0xff;
        }
        lfs_file_write(&lfs, &file[0], buffer, chunk) => chunk;
    }
    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST
}

r_test() {
tests/test.py << TEST
    lfs_size_t size = $1;
    lfs_size_t chunk = 29;
    srand(0);
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "$2", LFS_O_RDONLY) => 0;
    for (lfs_size_t i = 0; i < size; i += chunk) {
        chunk = (chunk < size - i) ? chunk : size - i;
        lfs_file_read(&lfs, &file[0], buffer, chunk) => chunk;
        for (lfs_size_t b = 0; b < chunk && i+b < size; b++) {
            buffer[b] => rand() & 0xff;
        }
    }
    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST
}

echo "--- Small file test ---"
w_test $SMALLSIZE smallavacado
r_test $SMALLSIZE smallavacado

echo "--- Medium file test ---"
w_test $MEDIUMSIZE mediumavacado
r_test $MEDIUMSIZE mediumavacado

echo "--- Large file test ---"
w_test $LARGESIZE largeavacado
r_test $LARGESIZE largeavacado

echo "--- Non-overlap check ---"
r_test $SMALLSIZE smallavacado
r_test $MEDIUMSIZE mediumavacado
r_test $LARGESIZE largeavacado

echo "--- Dir check ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir[0], "/") => 0;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, "hello") => 0;
    info.type => LFS_TYPE_REG;
    info.size => strlen("Hello World!\n");
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, "smallavacado") => 0;
    info.type => LFS_TYPE_REG;
    info.size => $SMALLSIZE;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, "mediumavacado") => 0;
    info.type => LFS_TYPE_REG;
    info.size => $MEDIUMSIZE;
    lfs_dir_read(&lfs, &dir[0], &info) => 1;
    strcmp(info.name, "largeavacado") => 0;
    info.type => LFS_TYPE_REG;
    info.size => $LARGESIZE;
    lfs_dir_read(&lfs, &dir[0], &info) => 0;
    lfs_dir_close(&lfs, &dir[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Results ---"
tests/stats.py
