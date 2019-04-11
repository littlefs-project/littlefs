#!/bin/bash
set -eu

echo "=== Attr tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;

    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "hello") => 0;
    lfs2_file_open(&lfs2, &file[0], "hello/hello",
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    lfs2_file_write(&lfs2, &file[0], "hello", strlen("hello"))
            => strlen("hello");
    lfs2_file_close(&lfs2, &file[0]);
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Set/get attribute ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_setattr(&lfs2, "hello", 'A', "aaaa",   4) => 0;
    lfs2_setattr(&lfs2, "hello", 'B', "bbbbbb", 6) => 0;
    lfs2_setattr(&lfs2, "hello", 'C', "ccccc",  5) => 0;
    lfs2_getattr(&lfs2, "hello", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "hello", 'B', buffer+4,  6) => 6;
    lfs2_getattr(&lfs2, "hello", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "bbbbbb", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs2_setattr(&lfs2, "hello", 'B', "", 0) => 0;
    lfs2_getattr(&lfs2, "hello", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "hello", 'B', buffer+4,  6) => 0;
    lfs2_getattr(&lfs2, "hello", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;

    lfs2_removeattr(&lfs2, "hello", 'B') => 0;
    lfs2_getattr(&lfs2, "hello", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "hello", 'B', buffer+4,  6) => LFS2_ERR_NOATTR;
    lfs2_getattr(&lfs2, "hello", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;

    lfs2_setattr(&lfs2, "hello", 'B', "dddddd", 6) => 0;
    lfs2_getattr(&lfs2, "hello", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "hello", 'B', buffer+4,  6) => 6;
    lfs2_getattr(&lfs2, "hello", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "dddddd", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs2_setattr(&lfs2, "hello", 'B', "eee", 3) => 0;
    lfs2_getattr(&lfs2, "hello", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "hello", 'B', buffer+4,  6) => 3;
    lfs2_getattr(&lfs2, "hello", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "eee\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",     5) => 0;

    lfs2_setattr(&lfs2, "hello", 'A', buffer, LFS2_ATTR_MAX+1) => LFS2_ERR_NOSPC;
    lfs2_setattr(&lfs2, "hello", 'B', "fffffffff", 9) => 0;
    lfs2_getattr(&lfs2, "hello", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "hello", 'B', buffer+4,  6) => 9;
    lfs2_getattr(&lfs2, "hello", 'C', buffer+10, 5) => 5;

    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_getattr(&lfs2, "hello", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "hello", 'B', buffer+4,  9) => 9;
    lfs2_getattr(&lfs2, "hello", 'C', buffer+13, 5) => 5;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "fffffffff", 9) => 0;
    memcmp(buffer+13, "ccccc",     5) => 0;

    lfs2_file_open(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, sizeof(buffer)) => strlen("hello");
    memcmp(buffer, "hello", strlen("hello")) => 0;
    lfs2_file_close(&lfs2, &file[0]);
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Set/get root attribute ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_setattr(&lfs2, "/", 'A', "aaaa",   4) => 0;
    lfs2_setattr(&lfs2, "/", 'B', "bbbbbb", 6) => 0;
    lfs2_setattr(&lfs2, "/", 'C', "ccccc",  5) => 0;
    lfs2_getattr(&lfs2, "/", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "/", 'B', buffer+4,  6) => 6;
    lfs2_getattr(&lfs2, "/", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "bbbbbb", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs2_setattr(&lfs2, "/", 'B', "", 0) => 0;
    lfs2_getattr(&lfs2, "/", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "/", 'B', buffer+4,  6) => 0;
    lfs2_getattr(&lfs2, "/", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;

    lfs2_removeattr(&lfs2, "/", 'B') => 0;
    lfs2_getattr(&lfs2, "/", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "/", 'B', buffer+4,  6) => LFS2_ERR_NOATTR;
    lfs2_getattr(&lfs2, "/", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;

    lfs2_setattr(&lfs2, "/", 'B', "dddddd", 6) => 0;
    lfs2_getattr(&lfs2, "/", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "/", 'B', buffer+4,  6) => 6;
    lfs2_getattr(&lfs2, "/", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "dddddd", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    lfs2_setattr(&lfs2, "/", 'B', "eee", 3) => 0;
    lfs2_getattr(&lfs2, "/", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "/", 'B', buffer+4,  6) => 3;
    lfs2_getattr(&lfs2, "/", 'C', buffer+10, 5) => 5;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "eee\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",     5) => 0;

    lfs2_setattr(&lfs2, "/", 'A', buffer, LFS2_ATTR_MAX+1) => LFS2_ERR_NOSPC;
    lfs2_setattr(&lfs2, "/", 'B', "fffffffff", 9) => 0;
    lfs2_getattr(&lfs2, "/", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "/", 'B', buffer+4,  6) => 9;
    lfs2_getattr(&lfs2, "/", 'C', buffer+10, 5) => 5;
    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_getattr(&lfs2, "/", 'A', buffer,    4) => 4;
    lfs2_getattr(&lfs2, "/", 'B', buffer+4,  9) => 9;
    lfs2_getattr(&lfs2, "/", 'C', buffer+13, 5) => 5;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "fffffffff", 9) => 0;
    memcmp(buffer+13, "ccccc",     5) => 0;

    lfs2_file_open(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, sizeof(buffer)) => strlen("hello");
    memcmp(buffer, "hello", strlen("hello")) => 0;
    lfs2_file_close(&lfs2, &file[0]);
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Set/get file attribute ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    struct lfs2_attr attrs1[] = {
        {'A', buffer,    4},
        {'B', buffer+4,  6},
        {'C', buffer+10, 5},
    };
    struct lfs2_file_config cfg1 = {.attrs=attrs1, .attr_count=3};

    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_WRONLY, &cfg1) => 0;
    memcpy(buffer,    "aaaa",   4);
    memcpy(buffer+4,  "bbbbbb", 6);
    memcpy(buffer+10, "ccccc",  5);
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memset(buffer, 0, 15);
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY, &cfg1) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "bbbbbb", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    attrs1[1].size = 0;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_WRONLY, &cfg1) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memset(buffer, 0, 15);
    attrs1[1].size = 6;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY, &cfg1) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memcmp(buffer,    "aaaa",         4) => 0;
    memcmp(buffer+4,  "\0\0\0\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",        5) => 0;

    attrs1[1].size = 6;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_WRONLY, &cfg1) => 0;
    memcpy(buffer+4,  "dddddd", 6);
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memset(buffer, 0, 15);
    attrs1[1].size = 6;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY, &cfg1) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memcmp(buffer,    "aaaa",   4) => 0;
    memcmp(buffer+4,  "dddddd", 6) => 0;
    memcmp(buffer+10, "ccccc",  5) => 0;

    attrs1[1].size = 3;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_WRONLY, &cfg1) => 0;
    memcpy(buffer+4,  "eee", 3);
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memset(buffer, 0, 15);
    attrs1[1].size = 6;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY, &cfg1) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "eee\0\0\0", 6) => 0;
    memcmp(buffer+10, "ccccc",     5) => 0;

    attrs1[0].size = LFS2_ATTR_MAX+1;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_WRONLY, &cfg1)
        => LFS2_ERR_NOSPC;

    struct lfs2_attr attrs2[] = {
        {'A', buffer,    4},
        {'B', buffer+4,  9},
        {'C', buffer+13, 5},
    };
    struct lfs2_file_config cfg2 = {.attrs=attrs2, .attr_count=3};
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_RDWR, &cfg2) => 0;
    memcpy(buffer+4,  "fffffffff", 9);
    lfs2_file_close(&lfs2, &file[0]) => 0;
    attrs1[0].size = 4;
    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY, &cfg1) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;

    lfs2_unmount(&lfs2) => 0;
TEST
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    struct lfs2_attr attrs2[] = {
        {'A', buffer,    4},
        {'B', buffer+4,  9},
        {'C', buffer+13, 5},
    };
    struct lfs2_file_config cfg2 = {.attrs=attrs2, .attr_count=3};

    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY, &cfg2) => 0;
    lfs2_file_close(&lfs2, &file[0]) => 0;
    memcmp(buffer,    "aaaa",      4) => 0;
    memcmp(buffer+4,  "fffffffff", 9) => 0;
    memcmp(buffer+13, "ccccc",     5) => 0;

    lfs2_file_open(&lfs2, &file[0], "hello/hello", LFS2_O_RDONLY) => 0;
    lfs2_file_read(&lfs2, &file[0], buffer, sizeof(buffer)) => strlen("hello");
    memcmp(buffer, "hello", strlen("hello")) => 0;
    lfs2_file_close(&lfs2, &file[0]);
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Deferred file attributes ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    struct lfs2_attr attrs1[] = {
        {'B', "gggg", 4},
        {'C', "",     0},
        {'D', "hhhh", 4},
    };
    struct lfs2_file_config cfg1 = {.attrs=attrs1, .attr_count=3};

    lfs2_file_opencfg(&lfs2, &file[0], "hello/hello", LFS2_O_WRONLY, &cfg1) => 0;

    lfs2_getattr(&lfs2, "hello/hello", 'B', buffer,    9) => 9;
    lfs2_getattr(&lfs2, "hello/hello", 'C', buffer+9,  9) => 5;
    lfs2_getattr(&lfs2, "hello/hello", 'D', buffer+18, 9) => LFS2_ERR_NOATTR;
    memcmp(buffer,    "fffffffff",          9) => 0;
    memcmp(buffer+9,  "ccccc\0\0\0\0",      9) => 0;
    memcmp(buffer+18, "\0\0\0\0\0\0\0\0\0", 9) => 0;

    lfs2_file_sync(&lfs2, &file[0]) => 0;
    lfs2_getattr(&lfs2, "hello/hello", 'B', buffer,    9) => 4;
    lfs2_getattr(&lfs2, "hello/hello", 'C', buffer+9,  9) => 0;
    lfs2_getattr(&lfs2, "hello/hello", 'D', buffer+18, 9) => 4;
    memcmp(buffer,    "gggg\0\0\0\0\0",     9) => 0;
    memcmp(buffer+9,  "\0\0\0\0\0\0\0\0\0", 9) => 0;
    memcmp(buffer+18, "hhhh\0\0\0\0\0",     9) => 0;

    lfs2_file_close(&lfs2, &file[0]) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Results ---"
tests/stats.py
