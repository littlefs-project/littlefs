#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Truncate tests ==="

SMALLSIZE=32
MEDIUMSIZE=2048
LARGESIZE=8192

rm -rf blocks
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

echo "--- Simple truncate ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldynoop",
            LFS_O_WRONLY | LFS_O_CREAT) => 0;

    strcpy((char*)buffer, "hair");
    lfs_size_t size = strlen((char*)buffer);
    for (lfs_off_t j = 0; j < $LARGESIZE; j += size) {
        lfs_file_write(&lfs, &file, buffer, size) => size;
    }
    lfs_file_size(&lfs, &file) => $LARGESIZE;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldynoop", LFS_O_RDWR) => 0;
    lfs_file_size(&lfs, &file) => $LARGESIZE;

    lfs_file_truncate(&lfs, &file, $MEDIUMSIZE) => 0;
    lfs_file_size(&lfs, &file) => $MEDIUMSIZE;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldynoop", LFS_O_RDONLY) => 0;
    lfs_file_size(&lfs, &file) => $MEDIUMSIZE;

    lfs_size_t size = strlen("hair");
    for (lfs_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "hair", size) => 0;
    }
    lfs_file_read(&lfs, &file, buffer, size) => 0;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Truncate and read ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldyread",
            LFS_O_WRONLY | LFS_O_CREAT) => 0;

    strcpy((char*)buffer, "hair");
    lfs_size_t size = strlen((char*)buffer);
    for (lfs_off_t j = 0; j < $LARGESIZE; j += size) {
        lfs_file_write(&lfs, &file, buffer, size) => size;
    }
    lfs_file_size(&lfs, &file) => $LARGESIZE;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldyread", LFS_O_RDWR) => 0;
    lfs_file_size(&lfs, &file) => $LARGESIZE;

    lfs_file_truncate(&lfs, &file, $MEDIUMSIZE) => 0;
    lfs_file_size(&lfs, &file) => $MEDIUMSIZE;

    lfs_size_t size = strlen("hair");
    for (lfs_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "hair", size) => 0;
    }
    lfs_file_read(&lfs, &file, buffer, size) => 0;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldyread", LFS_O_RDONLY) => 0;
    lfs_file_size(&lfs, &file) => $MEDIUMSIZE;

    lfs_size_t size = strlen("hair");
    for (lfs_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "hair", size) => 0;
    }
    lfs_file_read(&lfs, &file, buffer, size) => 0;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Write, truncate, and read ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "sequence",
            LFS_O_RDWR | LFS_O_CREAT | LFS_O_TRUNC) => 0;

    lfs_size_t size = lfs.cfg->cache_size;
    lfs_size_t qsize = size / 4;
    uint8_t *wb = buffer;
    uint8_t *rb = buffer + size;
    for (lfs_off_t j = 0; j < size; ++j) {
        wb[j] = j;
    }

    /* Spread sequence over size */
    lfs_file_write(&lfs, &file, wb, size) => size;
    lfs_file_size(&lfs, &file) => size;
    lfs_file_tell(&lfs, &file) => size;

    lfs_file_seek(&lfs, &file, 0, LFS_SEEK_SET) => 0;
    lfs_file_tell(&lfs, &file) => 0;

    /* Chop off the last quarter */
    lfs_size_t trunc = size - qsize;
    lfs_file_truncate(&lfs, &file, trunc) => 0;
    lfs_file_tell(&lfs, &file) => 0;
    lfs_file_size(&lfs, &file) => trunc;

    /* Read should produce first 3/4 */
    lfs_file_read(&lfs, &file, rb, size) => trunc;
    memcmp(rb, wb, trunc) => 0;

    /* Move to 1/4 */
    lfs_file_size(&lfs, &file) => trunc;
    lfs_file_seek(&lfs, &file, qsize, LFS_SEEK_SET) => qsize;
    lfs_file_tell(&lfs, &file) => qsize;

    /* Chop to 1/2 */
    trunc -= qsize;
    lfs_file_truncate(&lfs, &file, trunc) => 0;
    lfs_file_tell(&lfs, &file) => qsize;
    lfs_file_size(&lfs, &file) => trunc;
    
    /* Read should produce second quarter */
    lfs_file_read(&lfs, &file, rb, size) => trunc - qsize;
    memcmp(rb, wb + qsize, trunc - qsize) => 0;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Truncate and write ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldywrite",
            LFS_O_WRONLY | LFS_O_CREAT) => 0;

    strcpy((char*)buffer, "hair");
    lfs_size_t size = strlen((char*)buffer);
    for (lfs_off_t j = 0; j < $LARGESIZE; j += size) {
        lfs_file_write(&lfs, &file, buffer, size) => size;
    }
    lfs_file_size(&lfs, &file) => $LARGESIZE;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldywrite", LFS_O_RDWR) => 0;
    lfs_file_size(&lfs, &file) => $LARGESIZE;

    lfs_file_truncate(&lfs, &file, $MEDIUMSIZE) => 0;
    lfs_file_size(&lfs, &file) => $MEDIUMSIZE;

    strcpy((char*)buffer, "bald");
    lfs_size_t size = strlen((char*)buffer);
    for (lfs_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs_file_write(&lfs, &file, buffer, size) => size;
    }
    lfs_file_size(&lfs, &file) => $MEDIUMSIZE;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file, "baldywrite", LFS_O_RDONLY) => 0;
    lfs_file_size(&lfs, &file) => $MEDIUMSIZE;

    lfs_size_t size = strlen("bald");
    for (lfs_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs_file_read(&lfs, &file, buffer, size) => size;
        memcmp(buffer, "bald", size) => 0;
    }
    lfs_file_read(&lfs, &file, buffer, size) => 0;

    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

# More aggressive general truncation tests
truncate_test() {
STARTSIZES="$1"
STARTSEEKS="$2"
HOTSIZES="$3"
COLDSIZES="$4"
scripts/test.py << TEST
    static const lfs_off_t startsizes[] = {$STARTSIZES};
    static const lfs_off_t startseeks[] = {$STARTSEEKS};
    static const lfs_off_t hotsizes[]   = {$HOTSIZES};

    lfs_mount(&lfs, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf(path, "hairyhead%d", i);
        lfs_file_open(&lfs, &file, path,
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) => 0;

        strcpy((char*)buffer, "hair");
        lfs_size_t size = strlen((char*)buffer);
        for (lfs_off_t j = 0; j < startsizes[i]; j += size) {
            lfs_file_write(&lfs, &file, buffer, size) => size;
        }
        lfs_file_size(&lfs, &file) => startsizes[i];

        if (startseeks[i] != startsizes[i]) {
            lfs_file_seek(&lfs, &file,
                    startseeks[i], LFS_SEEK_SET) => startseeks[i];
        }

        lfs_file_truncate(&lfs, &file, hotsizes[i]) => 0;
        lfs_file_size(&lfs, &file) => hotsizes[i];

        lfs_file_close(&lfs, &file) => 0;
    }

    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    static const lfs_off_t startsizes[] = {$STARTSIZES};
    static const lfs_off_t hotsizes[]   = {$HOTSIZES};
    static const lfs_off_t coldsizes[]  = {$COLDSIZES};

    lfs_mount(&lfs, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf(path, "hairyhead%d", i);
        lfs_file_open(&lfs, &file, path, LFS_O_RDWR) => 0;
        lfs_file_size(&lfs, &file) => hotsizes[i];

        lfs_size_t size = strlen("hair");
        lfs_off_t j = 0;
        for (; j < startsizes[i] && j < hotsizes[i]; j += size) {
            lfs_file_read(&lfs, &file, buffer, size) => size;
            memcmp(buffer, "hair", size) => 0;
        }

        for (; j < hotsizes[i]; j += size) {
            lfs_file_read(&lfs, &file, buffer, size) => size;
            memcmp(buffer, "\0\0\0\0", size) => 0;
        }

        lfs_file_truncate(&lfs, &file, coldsizes[i]) => 0;
        lfs_file_size(&lfs, &file) => coldsizes[i];

        lfs_file_close(&lfs, &file) => 0;
    }

    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    static const lfs_off_t startsizes[] = {$STARTSIZES};
    static const lfs_off_t hotsizes[]   = {$HOTSIZES};
    static const lfs_off_t coldsizes[]  = {$COLDSIZES};

    lfs_mount(&lfs, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf(path, "hairyhead%d", i);
        lfs_file_open(&lfs, &file, path, LFS_O_RDONLY) => 0;
        lfs_file_size(&lfs, &file) => coldsizes[i];

        lfs_size_t size = strlen("hair");
        lfs_off_t j = 0;
        for (; j < startsizes[i] && j < hotsizes[i] && j < coldsizes[i];
                j += size) {
            lfs_file_read(&lfs, &file, buffer, size) => size;
            memcmp(buffer, "hair", size) => 0;
        }

        for (; j < coldsizes[i]; j += size) {
            lfs_file_read(&lfs, &file, buffer, size) => size;
            memcmp(buffer, "\0\0\0\0", size) => 0;
        }

        lfs_file_close(&lfs, &file) => 0;
    }

    lfs_unmount(&lfs) => 0;
TEST
}

echo "--- Cold shrinking truncate ---"
truncate_test \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE"

echo "--- Cold expanding truncate ---"
truncate_test \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE"

echo "--- Warm shrinking truncate ---"
truncate_test \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "           0,            0,            0,            0,            0"

echo "--- Warm expanding truncate ---"
truncate_test \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE"

echo "--- Mid-file shrinking truncate ---"
truncate_test \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "  $LARGESIZE,   $LARGESIZE,   $LARGESIZE,   $LARGESIZE,   $LARGESIZE" \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "           0,            0,            0,            0,            0"

echo "--- Mid-file expanding truncate ---"
truncate_test \
    "           0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE, 2*$LARGESIZE" \
    "           0,            0,   $SMALLSIZE,  $MEDIUMSIZE,   $LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE" \
    "2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE, 2*$LARGESIZE"

scripts/results.py
