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
    lfs2_format(&lfs2, &cfg) => 0;
TEST

echo "--- Simple truncate ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldynoop",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;

    strcpy((char*)buffer, "hair");
    lfs2_size_t size = strlen((char*)buffer);
    for (lfs2_off_t j = 0; j < $LARGESIZE; j += size) {
        lfs2_file_write(&lfs2, &file, buffer, size) => size;
    }
    lfs2_file_size(&lfs2, &file) => $LARGESIZE;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldynoop", LFS2_O_RDWR) => 0;
    lfs2_file_size(&lfs2, &file) => $LARGESIZE;

    lfs2_file_truncate(&lfs2, &file, $MEDIUMSIZE) => 0;
    lfs2_file_size(&lfs2, &file) => $MEDIUMSIZE;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldynoop", LFS2_O_RDONLY) => 0;
    lfs2_file_size(&lfs2, &file) => $MEDIUMSIZE;

    lfs2_size_t size = strlen("hair");
    for (lfs2_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "hair", size) => 0;
    }
    lfs2_file_read(&lfs2, &file, buffer, size) => 0;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Truncate and read ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldyread",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;

    strcpy((char*)buffer, "hair");
    lfs2_size_t size = strlen((char*)buffer);
    for (lfs2_off_t j = 0; j < $LARGESIZE; j += size) {
        lfs2_file_write(&lfs2, &file, buffer, size) => size;
    }
    lfs2_file_size(&lfs2, &file) => $LARGESIZE;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldyread", LFS2_O_RDWR) => 0;
    lfs2_file_size(&lfs2, &file) => $LARGESIZE;

    lfs2_file_truncate(&lfs2, &file, $MEDIUMSIZE) => 0;
    lfs2_file_size(&lfs2, &file) => $MEDIUMSIZE;

    lfs2_size_t size = strlen("hair");
    for (lfs2_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "hair", size) => 0;
    }
    lfs2_file_read(&lfs2, &file, buffer, size) => 0;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldyread", LFS2_O_RDONLY) => 0;
    lfs2_file_size(&lfs2, &file) => $MEDIUMSIZE;

    lfs2_size_t size = strlen("hair");
    for (lfs2_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "hair", size) => 0;
    }
    lfs2_file_read(&lfs2, &file, buffer, size) => 0;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Write, truncate, and read ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "sequence",
            LFS2_O_RDWR | LFS2_O_CREAT | LFS2_O_TRUNC) => 0;

    lfs2_size_t size = lfs2.cfg->cache_size;
    lfs2_size_t qsize = size / 4;
    uint8_t *wb = buffer;
    uint8_t *rb = buffer + size;
    for (lfs2_off_t j = 0; j < size; ++j) {
        wb[j] = j;
    }

    /* Spread sequence over size */
    lfs2_file_write(&lfs2, &file, wb, size) => size;
    lfs2_file_size(&lfs2, &file) => size;
    lfs2_file_tell(&lfs2, &file) => size;

    lfs2_file_seek(&lfs2, &file, 0, LFS2_SEEK_SET) => 0;
    lfs2_file_tell(&lfs2, &file) => 0;

    /* Chop off the last quarter */
    lfs2_size_t trunc = size - qsize;
    lfs2_file_truncate(&lfs2, &file, trunc) => 0;
    lfs2_file_tell(&lfs2, &file) => 0;
    lfs2_file_size(&lfs2, &file) => trunc;

    /* Read should produce first 3/4 */
    lfs2_file_read(&lfs2, &file, rb, size) => trunc;
    memcmp(rb, wb, trunc) => 0;

    /* Move to 1/4 */
    lfs2_file_size(&lfs2, &file) => trunc;
    lfs2_file_seek(&lfs2, &file, qsize, LFS2_SEEK_SET) => qsize;
    lfs2_file_tell(&lfs2, &file) => qsize;

    /* Chop to 1/2 */
    trunc -= qsize;
    lfs2_file_truncate(&lfs2, &file, trunc) => 0;
    lfs2_file_tell(&lfs2, &file) => qsize;
    lfs2_file_size(&lfs2, &file) => trunc;
    
    /* Read should produce second quarter */
    lfs2_file_read(&lfs2, &file, rb, size) => trunc - qsize;
    memcmp(rb, wb + qsize, trunc - qsize) => 0;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Truncate and write ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldywrite",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;

    strcpy((char*)buffer, "hair");
    lfs2_size_t size = strlen((char*)buffer);
    for (lfs2_off_t j = 0; j < $LARGESIZE; j += size) {
        lfs2_file_write(&lfs2, &file, buffer, size) => size;
    }
    lfs2_file_size(&lfs2, &file) => $LARGESIZE;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldywrite", LFS2_O_RDWR) => 0;
    lfs2_file_size(&lfs2, &file) => $LARGESIZE;

    lfs2_file_truncate(&lfs2, &file, $MEDIUMSIZE) => 0;
    lfs2_file_size(&lfs2, &file) => $MEDIUMSIZE;

    strcpy((char*)buffer, "bald");
    lfs2_size_t size = strlen((char*)buffer);
    for (lfs2_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs2_file_write(&lfs2, &file, buffer, size) => size;
    }
    lfs2_file_size(&lfs2, &file) => $MEDIUMSIZE;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_file_open(&lfs2, &file, "baldywrite", LFS2_O_RDONLY) => 0;
    lfs2_file_size(&lfs2, &file) => $MEDIUMSIZE;

    lfs2_size_t size = strlen("bald");
    for (lfs2_off_t j = 0; j < $MEDIUMSIZE; j += size) {
        lfs2_file_read(&lfs2, &file, buffer, size) => size;
        memcmp(buffer, "bald", size) => 0;
    }
    lfs2_file_read(&lfs2, &file, buffer, size) => 0;

    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

# More aggressive general truncation tests
truncate_test() {
STARTSIZES="$1"
STARTSEEKS="$2"
HOTSIZES="$3"
COLDSIZES="$4"
scripts/test.py << TEST
    static const lfs2_off_t startsizes[] = {$STARTSIZES};
    static const lfs2_off_t startseeks[] = {$STARTSEEKS};
    static const lfs2_off_t hotsizes[]   = {$HOTSIZES};

    lfs2_mount(&lfs2, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf(path, "hairyhead%d", i);
        lfs2_file_open(&lfs2, &file, path,
                LFS2_O_WRONLY | LFS2_O_CREAT | LFS2_O_TRUNC) => 0;

        strcpy((char*)buffer, "hair");
        lfs2_size_t size = strlen((char*)buffer);
        for (lfs2_off_t j = 0; j < startsizes[i]; j += size) {
            lfs2_file_write(&lfs2, &file, buffer, size) => size;
        }
        lfs2_file_size(&lfs2, &file) => startsizes[i];

        if (startseeks[i] != startsizes[i]) {
            lfs2_file_seek(&lfs2, &file,
                    startseeks[i], LFS2_SEEK_SET) => startseeks[i];
        }

        lfs2_file_truncate(&lfs2, &file, hotsizes[i]) => 0;
        lfs2_file_size(&lfs2, &file) => hotsizes[i];

        lfs2_file_close(&lfs2, &file) => 0;
    }

    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    static const lfs2_off_t startsizes[] = {$STARTSIZES};
    static const lfs2_off_t hotsizes[]   = {$HOTSIZES};
    static const lfs2_off_t coldsizes[]  = {$COLDSIZES};

    lfs2_mount(&lfs2, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf(path, "hairyhead%d", i);
        lfs2_file_open(&lfs2, &file, path, LFS2_O_RDWR) => 0;
        lfs2_file_size(&lfs2, &file) => hotsizes[i];

        lfs2_size_t size = strlen("hair");
        lfs2_off_t j = 0;
        for (; j < startsizes[i] && j < hotsizes[i]; j += size) {
            lfs2_file_read(&lfs2, &file, buffer, size) => size;
            memcmp(buffer, "hair", size) => 0;
        }

        for (; j < hotsizes[i]; j += size) {
            lfs2_file_read(&lfs2, &file, buffer, size) => size;
            memcmp(buffer, "\0\0\0\0", size) => 0;
        }

        lfs2_file_truncate(&lfs2, &file, coldsizes[i]) => 0;
        lfs2_file_size(&lfs2, &file) => coldsizes[i];

        lfs2_file_close(&lfs2, &file) => 0;
    }

    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    static const lfs2_off_t startsizes[] = {$STARTSIZES};
    static const lfs2_off_t hotsizes[]   = {$HOTSIZES};
    static const lfs2_off_t coldsizes[]  = {$COLDSIZES};

    lfs2_mount(&lfs2, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf(path, "hairyhead%d", i);
        lfs2_file_open(&lfs2, &file, path, LFS2_O_RDONLY) => 0;
        lfs2_file_size(&lfs2, &file) => coldsizes[i];

        lfs2_size_t size = strlen("hair");
        lfs2_off_t j = 0;
        for (; j < startsizes[i] && j < hotsizes[i] && j < coldsizes[i];
                j += size) {
            lfs2_file_read(&lfs2, &file, buffer, size) => size;
            memcmp(buffer, "hair", size) => 0;
        }

        for (; j < coldsizes[i]; j += size) {
            lfs2_file_read(&lfs2, &file, buffer, size) => size;
            memcmp(buffer, "\0\0\0\0", size) => 0;
        }

        lfs2_file_close(&lfs2, &file) => 0;
    }

    lfs2_unmount(&lfs2) => 0;
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
