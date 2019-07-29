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
    lfs2_format(&lfs2, &cfg) => 0;
TEST

echo "--- Simple file test ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "hello", LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    lfs2_size_t size = strlen("Hello World!\n");
    uint8_t wbuffer[1024];
    memcpy(wbuffer, "Hello World!\n", size);
    lfs2_file_write(&lfs2, &file, wbuffer, size) => size;
    lfs2_file_close(&lfs2, &file) => 0;

    lfs2_file_open(&lfs2, &file, "hello", LFS2_O_RDONLY) => 0;
    size = strlen("Hello World!\n");
    uint8_t rbuffer[1024];
    lfs2_file_read(&lfs2, &file, rbuffer, size) => size;
    memcmp(rbuffer, wbuffer, size) => 0;
    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

w_test() {
scripts/test.py ${4:-} << TEST
    lfs2_size_t size = $1;
    lfs2_size_t chunk = 31;
    srand(0);
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "$2",
        ${3:-LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_TRUNC}) => 0;
    for (lfs2_size_t i = 0; i < size; i += chunk) {
        chunk = (chunk < size - i) ? chunk : size - i;
        for (lfs2_size_t b = 0; b < chunk; b++) {
            buffer[b] = rand() & 0xff;
        }
        lfs2_file_write(&lfs2, &file, buffer, chunk) => chunk;
    }
    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
}

r_test() {
scripts/test.py << TEST
    lfs2_size_t size = $1;
    lfs2_size_t chunk = 29;
    srand(0);
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "$2", &info) => 0;
    info.type => LFS2_TYPE_REG;
    info.size => size;
    lfs2_file_open(&lfs2, &file, "$2", ${3:-LFS2_O_RDONLY}) => 0;
    for (lfs2_size_t i = 0; i < size; i += chunk) {
        chunk = (chunk < size - i) ? chunk : size - i;
        lfs2_file_read(&lfs2, &file, buffer, chunk) => chunk;
        for (lfs2_size_t b = 0; b < chunk && i+b < size; b++) {
            buffer[b] => rand() & 0xff;
        }
    }
    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
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
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_dir_open(&lfs2, &dir, "/") => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "hello") => 0;
    info.type => LFS2_TYPE_REG;
    info.size => strlen("Hello World!\n");
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "largeavacado") => 0;
    info.type => LFS2_TYPE_REG;
    info.size => $LARGESIZE;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "mediumavacado") => 0;
    info.type => LFS2_TYPE_REG;
    info.size => $MEDIUMSIZE;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "noavacado") => 0;
    info.type => LFS2_TYPE_REG;
    info.size => 0;
    lfs2_dir_read(&lfs2, &dir, &info) => 1;
    strcmp(info.name, "smallavacado") => 0;
    info.type => LFS2_TYPE_REG;
    info.size => $SMALLSIZE;
    lfs2_dir_read(&lfs2, &dir, &info) => 0;
    lfs2_dir_close(&lfs2, &dir) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Many files test ---"
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST
scripts/test.py << TEST
    // Create 300 files of 7 bytes
    lfs2_mount(&lfs2, &cfg) => 0;
    for (unsigned i = 0; i < 300; i++) {
        sprintf(path, "file_%03d", i);
        lfs2_file_open(&lfs2, &file, path,
                LFS2_O_RDWR | LFS2_O_CREAT | LFS2_O_EXCL) => 0;
        lfs2_size_t size = 7;
        uint8_t wbuffer[1024];
        uint8_t rbuffer[1024];
        snprintf((char*)wbuffer, size, "Hi %03d", i);
        lfs2_file_write(&lfs2, &file, wbuffer, size) => size;
        lfs2_file_rewind(&lfs2, &file) => 0;
        lfs2_file_read(&lfs2, &file, rbuffer, size) => size;
        memcmp(wbuffer, rbuffer, size) => 0;
        lfs2_file_close(&lfs2, &file) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Many files with flush test ---"
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST
scripts/test.py << TEST
    // Create 300 files of 7 bytes
    lfs2_mount(&lfs2, &cfg) => 0;
    for (unsigned i = 0; i < 300; i++) {
        sprintf(path, "file_%03d", i);
        lfs2_file_open(&lfs2, &file, path,
                LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_EXCL) => 0;
        lfs2_size_t size = 7;
        uint8_t wbuffer[1024];
        uint8_t rbuffer[1024];
        snprintf((char*)wbuffer, size, "Hi %03d", i);
        lfs2_file_write(&lfs2, &file, wbuffer, size) => size;
        lfs2_file_close(&lfs2, &file) => 0;

        lfs2_file_open(&lfs2, &file, path, LFS2_O_RDONLY) => 0;
        lfs2_file_read(&lfs2, &file, rbuffer, size) => size;
        memcmp(wbuffer, rbuffer, size) => 0;
        lfs2_file_close(&lfs2, &file) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Many files with power cycle test ---"
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST
scripts/test.py << TEST
    // Create 300 files of 7 bytes
    lfs2_mount(&lfs2, &cfg) => 0;
    for (unsigned i = 0; i < 300; i++) {
        sprintf(path, "file_%03d", i);
        lfs2_file_open(&lfs2, &file, path,
                LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_EXCL) => 0;
        lfs2_size_t size = 7;
        uint8_t wbuffer[1024];
        uint8_t rbuffer[1024];
        snprintf((char*)wbuffer, size, "Hi %03d", i);
        lfs2_file_write(&lfs2, &file, wbuffer, size) => size;
        lfs2_file_close(&lfs2, &file) => 0;
        lfs2_unmount(&lfs2) => 0;

        lfs2_mount(&lfs2, &cfg) => 0;
        lfs2_file_open(&lfs2, &file, path, LFS2_O_RDONLY) => 0;
        lfs2_file_read(&lfs2, &file, rbuffer, size) => size;
        memcmp(wbuffer, rbuffer, size) => 0;
        lfs2_file_close(&lfs2, &file) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST

scripts/results.py
