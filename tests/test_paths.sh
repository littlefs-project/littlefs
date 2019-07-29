#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Path tests ==="
rm -rf blocks
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST

scripts/test.py << TEST
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
scripts/test.py << TEST
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
scripts/test.py << TEST
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
scripts/test.py << TEST
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
scripts/test.py << TEST
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
scripts/test.py << TEST
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
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "coffee/../../../../../../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs2_mkdir(&lfs2, "coffee/../../../../../../milk5") => 0;
    lfs2_stat(&lfs2, "coffee/../../../../../../milk5", &info) => 0;
    strcmp(info.name, "milk5") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Root tests ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_stat(&lfs2, "/", &info) => 0;
    info.type => LFS2_TYPE_DIR;
    strcmp(info.name, "/") => 0;

    lfs2_mkdir(&lfs2, "/") => LFS2_ERR_EXIST;
    lfs2_file_open(&lfs2, &file, "/", LFS2_O_WRONLY | LFS2_O_CREAT)
        => LFS2_ERR_ISDIR;

    // more corner cases
    lfs2_remove(&lfs2, "") => LFS2_ERR_INVAL;
    lfs2_remove(&lfs2, ".") => LFS2_ERR_INVAL;
    lfs2_remove(&lfs2, "..") => LFS2_ERR_INVAL;
    lfs2_remove(&lfs2, "/") => LFS2_ERR_INVAL;
    lfs2_remove(&lfs2, "//") => LFS2_ERR_INVAL;
    lfs2_remove(&lfs2, "./") => LFS2_ERR_INVAL;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Sketchy path tests ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "dirt/ground") => LFS2_ERR_NOENT;
    lfs2_mkdir(&lfs2, "dirt/ground/earth") => LFS2_ERR_NOENT;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Superblock conflict test ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "littlefs") => 0;
    lfs2_remove(&lfs2, "littlefs") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Max path test ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    memset(path, 'w', LFS2_NAME_MAX+1);
    path[LFS2_NAME_MAX+2] = '\0';
    lfs2_mkdir(&lfs2, path) => LFS2_ERR_NAMETOOLONG;
    lfs2_file_open(&lfs2, &file, path,
            LFS2_O_WRONLY | LFS2_O_CREAT) => LFS2_ERR_NAMETOOLONG;

    memcpy(path, "coffee/", strlen("coffee/"));
    memset(path+strlen("coffee/"), 'w', LFS2_NAME_MAX+1);
    path[strlen("coffee/")+LFS2_NAME_MAX+2] = '\0';
    lfs2_mkdir(&lfs2, path) => LFS2_ERR_NAMETOOLONG;
    lfs2_file_open(&lfs2, &file, path,
            LFS2_O_WRONLY | LFS2_O_CREAT) => LFS2_ERR_NAMETOOLONG;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Really big path test ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    memset(path, 'w', LFS2_NAME_MAX);
    path[LFS2_NAME_MAX+1] = '\0';
    lfs2_mkdir(&lfs2, path) => 0;
    lfs2_remove(&lfs2, path) => 0;
    lfs2_file_open(&lfs2, &file, path,
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_remove(&lfs2, path) => 0;

    memcpy(path, "coffee/", strlen("coffee/"));
    memset(path+strlen("coffee/"), 'w', LFS2_NAME_MAX);
    path[strlen("coffee/")+LFS2_NAME_MAX+1] = '\0';
    lfs2_mkdir(&lfs2, path) => 0;
    lfs2_remove(&lfs2, path) => 0;
    lfs2_file_open(&lfs2, &file, path,
            LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
    lfs2_file_close(&lfs2, &file) => 0;
    lfs2_remove(&lfs2, path) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

scripts/results.py
