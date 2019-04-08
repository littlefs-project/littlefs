#!/bin/bash
set -eu

SMALLSIZE=32
MEDIUMSIZE=2048
LARGESIZE=8192

echo "=== Truncate tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST

truncate_test() {
STARTSIZES="$1"
STARTSEEKS="$2"
HOTSIZES="$3"
COLDSIZES="$4"
tests/test.py << TEST
    static const lfs1_off_t startsizes[] = {$STARTSIZES};
    static const lfs1_off_t startseeks[] = {$STARTSEEKS};
    static const lfs1_off_t hotsizes[]   = {$HOTSIZES};

    lfs1_mount(&lfs1, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf((char*)buffer, "hairyhead%d", i);
        lfs1_file_open(&lfs1, &file[0], (const char*)buffer,
                LFS1_O_WRONLY | LFS1_O_CREAT | LFS1_O_TRUNC) => 0;

        strcpy((char*)buffer, "hair");
        size = strlen((char*)buffer);
        for (lfs1_off_t j = 0; j < startsizes[i]; j += size) {
            lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
        }
        lfs1_file_size(&lfs1, &file[0]) => startsizes[i];

        if (startseeks[i] != startsizes[i]) {
            lfs1_file_seek(&lfs1, &file[0],
                    startseeks[i], LFS1_SEEK_SET) => startseeks[i];
        }

        lfs1_file_truncate(&lfs1, &file[0], hotsizes[i]) => 0;
        lfs1_file_size(&lfs1, &file[0]) => hotsizes[i];

        lfs1_file_close(&lfs1, &file[0]) => 0;
    }

    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    static const lfs1_off_t startsizes[] = {$STARTSIZES};
    static const lfs1_off_t hotsizes[]   = {$HOTSIZES};
    static const lfs1_off_t coldsizes[]  = {$COLDSIZES};

    lfs1_mount(&lfs1, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf((char*)buffer, "hairyhead%d", i);
        lfs1_file_open(&lfs1, &file[0], (const char*)buffer, LFS1_O_RDWR) => 0;
        lfs1_file_size(&lfs1, &file[0]) => hotsizes[i];

        size = strlen("hair");
        lfs1_off_t j = 0;
        for (; j < startsizes[i] && j < hotsizes[i]; j += size) {
            lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
            memcmp(buffer, "hair", size) => 0;
        }

        for (; j < hotsizes[i]; j += size) {
            lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
            memcmp(buffer, "\0\0\0\0", size) => 0;
        }

        lfs1_file_truncate(&lfs1, &file[0], coldsizes[i]) => 0;
        lfs1_file_size(&lfs1, &file[0]) => coldsizes[i];

        lfs1_file_close(&lfs1, &file[0]) => 0;
    }

    lfs1_unmount(&lfs1) => 0;
TEST
tests/test.py << TEST
    static const lfs1_off_t startsizes[] = {$STARTSIZES};
    static const lfs1_off_t hotsizes[]   = {$HOTSIZES};
    static const lfs1_off_t coldsizes[]  = {$COLDSIZES};

    lfs1_mount(&lfs1, &cfg) => 0;

    for (unsigned i = 0; i < sizeof(startsizes)/sizeof(startsizes[0]); i++) {
        sprintf((char*)buffer, "hairyhead%d", i);
        lfs1_file_open(&lfs1, &file[0], (const char*)buffer, LFS1_O_RDONLY) => 0;
        lfs1_file_size(&lfs1, &file[0]) => coldsizes[i];

        size = strlen("hair");
        lfs1_off_t j = 0;
        for (; j < startsizes[i] && j < hotsizes[i] && j < coldsizes[i];
                j += size) {
            lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
            memcmp(buffer, "hair", size) => 0;
        }

        for (; j < coldsizes[i]; j += size) {
            lfs1_file_read(&lfs1, &file[0], buffer, size) => size;
            memcmp(buffer, "\0\0\0\0", size) => 0;
        }

        lfs1_file_close(&lfs1, &file[0]) => 0;
    }

    lfs1_unmount(&lfs1) => 0;
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

echo "--- Results ---"
tests/stats.py
