#!/bin/bash
set -eu

echo "=== Path tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST

tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "tea") => 0;
    lfs1_mkdir(&lfs1, "coffee") => 0;
    lfs1_mkdir(&lfs1, "soda") => 0;
    lfs1_mkdir(&lfs1, "tea/hottea") => 0;
    lfs1_mkdir(&lfs1, "tea/warmtea") => 0;
    lfs1_mkdir(&lfs1, "tea/coldtea") => 0;
    lfs1_mkdir(&lfs1, "coffee/hotcoffee") => 0;
    lfs1_mkdir(&lfs1, "coffee/warmcoffee") => 0;
    lfs1_mkdir(&lfs1, "coffee/coldcoffee") => 0;
    lfs1_mkdir(&lfs1, "soda/hotsoda") => 0;
    lfs1_mkdir(&lfs1, "soda/warmsoda") => 0;
    lfs1_mkdir(&lfs1, "soda/coldsoda") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Root path tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "/tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs1_mkdir(&lfs1, "/milk1") => 0;
    lfs1_stat(&lfs1, "/milk1", &info) => 0;
    strcmp(info.name, "milk1") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Redundant slash path tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "/tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "//tea//hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "///tea///hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs1_mkdir(&lfs1, "///milk2") => 0;
    lfs1_stat(&lfs1, "///milk2", &info) => 0;
    strcmp(info.name, "milk2") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Dot path tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "./tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "/./tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "/././tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "/./tea/./hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs1_mkdir(&lfs1, "/./milk3") => 0;
    lfs1_stat(&lfs1, "/./milk3", &info) => 0;
    strcmp(info.name, "milk3") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Dot dot path tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "coffee/../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "tea/coldtea/../hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "coffee/coldcoffee/../../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "coffee/../soda/../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs1_mkdir(&lfs1, "coffee/../milk4") => 0;
    lfs1_stat(&lfs1, "coffee/../milk4", &info) => 0;
    strcmp(info.name, "milk4") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Trailing dot path tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "tea/hottea/", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "tea/hottea/.", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "tea/hottea/./.", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs1_stat(&lfs1, "tea/hottea/..", &info) => 0;
    strcmp(info.name, "tea") => 0;
    lfs1_stat(&lfs1, "tea/hottea/../.", &info) => 0;
    strcmp(info.name, "tea") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Root dot dot path tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "coffee/../../../../../../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;

    lfs1_mkdir(&lfs1, "coffee/../../../../../../milk5") => 0;
    lfs1_stat(&lfs1, "coffee/../../../../../../milk5", &info) => 0;
    strcmp(info.name, "milk5") => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Root tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "/", &info) => 0;
    info.type => LFS1_TYPE_DIR;
    strcmp(info.name, "/") => 0;

    lfs1_mkdir(&lfs1, "/") => LFS1_ERR_EXIST;
    lfs1_file_open(&lfs1, &file[0], "/", LFS1_O_WRONLY | LFS1_O_CREAT)
        => LFS1_ERR_ISDIR;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Sketchy path tests ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "dirt/ground") => LFS1_ERR_NOENT;
    lfs1_mkdir(&lfs1, "dirt/ground/earth") => LFS1_ERR_NOENT;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Results ---"
tests/stats.py
