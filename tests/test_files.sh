#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== File tests ==="

SMALLSIZE=32
MEDIUMSIZE=8192
LARGESIZE=262144

rm -rf blocks
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

echo "--- Simple file test ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "hello", LFS_O_WRONLY | LFS_O_CREAT) => 0;
    lfs_size_t size = strlen("Hello World!\n");
    uint8_t wbuffer[1024];
    memcpy(wbuffer, "Hello World!\n", size);
    lfs_file_write(&lfs, &file, wbuffer, size) => size;
    lfs_file_close(&lfs, &file) => 0;

    lfs_file_open(&lfs, &file, "hello", LFS_O_RDONLY) => 0;
    size = strlen("Hello World!\n");
    uint8_t rbuffer[1024];
    lfs_file_read(&lfs, &file, rbuffer, size) => size;
    memcmp(rbuffer, wbuffer, size) => 0;
    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

w_test() {
scripts/test.py ${4:-} << TEST
    lfs_size_t size = $1;
    lfs_size_t chunk = 31;
    srand(0);
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "$2",
        ${3:-LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC}) => 0;
    for (lfs_size_t i = 0; i < size; i += chunk) {
        chunk = (chunk < size - i) ? chunk : size - i;
        for (lfs_size_t b = 0; b < chunk; b++) {
            buffer[b] = rand() & 0xff;
        }
        lfs_file_write(&lfs, &file, buffer, chunk) => chunk;
    }
    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
}

r_test() {
scripts/test.py << TEST
    lfs_size_t size = $1;
    lfs_size_t chunk = 29;
    srand(0);
    lfs_mount(&lfs, &cfg) => 0;
    lfs_stat(&lfs, "$2", &info) => 0;
    info.type => LFS_TYPE_REG;
    info.size => size;
    lfs_file_open(&lfs, &file, "$2", ${3:-LFS_O_RDONLY}) => 0;
    for (lfs_size_t i = 0; i < size; i += chunk) {
        chunk = (chunk < size - i) ? chunk : size - i;
        lfs_file_read(&lfs, &file, buffer, chunk) => chunk;
        for (lfs_size_t b = 0; b < chunk && i+b < size; b++) {
            buffer[b] => rand() & 0xff;
        }
    }
    lfs_file_close(&lfs, &file) => 0;
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

echo "--- Zero file test ---"
w_test 0 noavacado
r_test 0 noavacado

echo "--- Truncate small test ---"
w_test $SMALLSIZE mediumavacado
r_test $SMALLSIZE mediumavacado
w_test $MEDIUMSIZE mediumavacado
r_test $MEDIUMSIZE mediumavacado

echo "--- Truncate zero test ---"
w_test $SMALLSIZE noavacado
r_test $SMALLSIZE noavacado
w_test 0 noavacado
r_test 0 noavacado

echo "--- Non-overlap check ---"
r_test $SMALLSIZE smallavacado
r_test $MEDIUMSIZE mediumavacado
r_test $LARGESIZE largeavacado
r_test 0 noavacado

echo "--- Dir check ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_dir_open(&lfs, &dir, "/") => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    info.type => LFS_TYPE_REG;
    info.size => strlen("Hello World!\n");
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "largeavacado") => 0;
    info.type => LFS_TYPE_REG;
    info.size => $LARGESIZE;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "mediumavacado") => 0;
    info.type => LFS_TYPE_REG;
    info.size => $MEDIUMSIZE;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "noavacado") => 0;
    info.type => LFS_TYPE_REG;
    info.size => 0;
    lfs_dir_read(&lfs, &dir, &info) => 1;
    strcmp(info.name, "smallavacado") => 0;
    info.type => LFS_TYPE_REG;
    info.size => $SMALLSIZE;
    lfs_dir_read(&lfs, &dir, &info) => 0;
    lfs_dir_close(&lfs, &dir) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Many files test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST
scripts/test.py << TEST
    // Create 300 files of 7 bytes
    lfs_mount(&lfs, &cfg) => 0;
    for (unsigned i = 0; i < 300; i++) {
        sprintf(path, "file_%03d", i);
        lfs_file_open(&lfs, &file, path,
                LFS_O_RDWR | LFS_O_CREAT | LFS_O_EXCL) => 0;
        lfs_size_t size = 7;
        uint8_t wbuffer[1024];
        uint8_t rbuffer[1024];
        snprintf((char*)wbuffer, size, "Hi %03d", i);
        lfs_file_write(&lfs, &file, wbuffer, size) => size;
        lfs_file_rewind(&lfs, &file) => 0;
        lfs_file_read(&lfs, &file, rbuffer, size) => size;
        memcmp(wbuffer, rbuffer, size) => 0;
        lfs_file_close(&lfs, &file) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Many files with flush test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST
scripts/test.py << TEST
    // Create 300 files of 7 bytes
    lfs_mount(&lfs, &cfg) => 0;
    for (unsigned i = 0; i < 300; i++) {
        sprintf(path, "file_%03d", i);
        lfs_file_open(&lfs, &file, path,
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL) => 0;
        lfs_size_t size = 7;
        uint8_t wbuffer[1024];
        uint8_t rbuffer[1024];
        snprintf((char*)wbuffer, size, "Hi %03d", i);
        lfs_file_write(&lfs, &file, wbuffer, size) => size;
        lfs_file_close(&lfs, &file) => 0;

        lfs_file_open(&lfs, &file, path, LFS_O_RDONLY) => 0;
        lfs_file_read(&lfs, &file, rbuffer, size) => size;
        memcmp(wbuffer, rbuffer, size) => 0;
        lfs_file_close(&lfs, &file) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Many files with power cycle test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST
scripts/test.py << TEST
    // Create 300 files of 7 bytes
    lfs_mount(&lfs, &cfg) => 0;
    for (unsigned i = 0; i < 300; i++) {
        sprintf(path, "file_%03d", i);
        lfs_file_open(&lfs, &file, path,
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_EXCL) => 0;
        lfs_size_t size = 7;
        uint8_t wbuffer[1024];
        uint8_t rbuffer[1024];
        snprintf((char*)wbuffer, size, "Hi %03d", i);
        lfs_file_write(&lfs, &file, wbuffer, size) => size;
        lfs_file_close(&lfs, &file) => 0;
        lfs_unmount(&lfs) => 0;

        lfs_mount(&lfs, &cfg) => 0;
        lfs_file_open(&lfs, &file, path, LFS_O_RDONLY) => 0;
        lfs_file_read(&lfs, &file, rbuffer, size) => size;
        memcmp(wbuffer, rbuffer, size) => 0;
        lfs_file_close(&lfs, &file) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST

scripts/results.py
