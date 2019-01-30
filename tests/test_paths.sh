#!/bin/bash
set -eu

echo "=== Path tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST

tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "tea") => 0;
    lfs2_mkdir(&lfs2, "coffee") => 0;
    lfs2_mkdir(&lfs2, "soda") => 0;
    lfs2_mkdir(&lfs2, "tea/hottea") => 0;
    lfs2_mkdir(&lfs2, "tea/warmtea") => 0;
    lfs2_mkdir(&lfs2, "tea/coldtea") => 0;
    lfs2_mkdir(&lfs2, "coffee/hotcoffee") => 0;
    lfs2_mkdir(&lfs2, "coffee/warmcoffee") => 0;
    lfs2_mkdir(&lfs2, "coffee/coldcoffee") => 0;
    lfs2_mkdir(&lfs2, "soda/hotsoda") => 0;
    lfs2_mkdir(&lfs2, "soda/warmsoda") => 0;
    lfs2_mkdir(&lfs2, "soda/coldsoda") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Root path tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "/tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs2_mkdir(&lfs2, "/milk1") => 0;
    lfs2_stat(&lfs2, "/milk1", &info) => 0;
    strcmp(info.name, "milk1") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Redundant slash path tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "/tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "//tea//hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "///tea///hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs2_mkdir(&lfs2, "///milk2") => 0;
    lfs2_stat(&lfs2, "///milk2", &info) => 0;
    strcmp(info.name, "milk2") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Dot path tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "./tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "/./tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "/././tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "/./tea/./hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs2_mkdir(&lfs2, "/./milk3") => 0;
    lfs2_stat(&lfs2, "/./milk3", &info) => 0;
    strcmp(info.name, "milk3") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Dot dot path tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "coffee/../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "tea/coldtea/../hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "coffee/coldcoffee/../../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "coffee/../soda/../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs2_mkdir(&lfs2, "coffee/../milk4") => 0;
    lfs2_stat(&lfs2, "coffee/../milk4", &info) => 0;
    strcmp(info.name, "milk4") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Trailing dot path tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "tea/hottea/", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "tea/hottea/.", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "tea/hottea/./.", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs2_stat(&lfs2, "tea/hottea/..", &info) => 0;
    strcmp(info.name, "tea") => 0;
    lfs2_stat(&lfs2, "tea/hottea/../.", &info) => 0;
    strcmp(info.name, "tea") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Root dot dot path tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "coffee/../../../../../../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs2_mkdir(&lfs2, "coffee/../../../../../../milk5") => 0;
    lfs2_stat(&lfs2, "coffee/../../../../../../milk5", &info) => 0;
    strcmp(info.name, "milk5") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Root tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "/", &info) => 0;
    info.type => LFS2_TYPE_DIR;
    strcmp(info.name, "/") => 0;

    lfs2_mkdir(&lfs2, "/") => LFS2_ERR_EXIST;
    lfs2_file_open(&lfs2, &file[0], "/", LFS2_O_WRONLY | LFS2_O_CREAT)
        => LFS2_ERR_ISDIR;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Sketchy path tests ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "dirt/ground") => LFS2_ERR_NOENT;
    lfs2_mkdir(&lfs2, "dirt/ground/earth") => LFS2_ERR_NOENT;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Superblock conflict test ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "littlefs") => 0;
    lfs2_remove(&lfs2, "littlefs") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Max path test ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    memset(buffer, 'w', LFS2_NAME_MAX+1);
    buffer[LFS2_NAME_MAX+2] = '\0';
    lfs2_mkdir(&lfs2, (char*)buffer) => LFS2_ERR_NAMETOOLONG;
    lfs2_file_open(&lfs2, &file[0], (char*)buffer,
            LFS2_O_WRONLY | LFS2_O_CREAT) => LFS2_ERR_NAMETOOLONG;

    memcpy(buffer, "coffee/", strlen("coffee/"));
    memset(buffer+strlen("coffee/"), 'w', LFS2_NAME_MAX+1);
    buffer[strlen("coffee/")+LFS2_NAME_MAX+2] = '\0';
    lfs2_mkdir(&lfs2, (char*)buffer) => LFS2_ERR_NAMETOOLONG;
    lfs2_file_open(&lfs2, &file[0], (char*)buffer,
            LFS2_O_WRONLY | LFS2_O_CREAT) => LFS2_ERR_NAMETOOLONG;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Results ---"
tests/stats.py
