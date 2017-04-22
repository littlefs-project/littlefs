#!/bin/bash
set -eu

echo "=== Path tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "tea") => 0;
    lfs_mkdir(&lfs, "coffee") => 0;
    lfs_mkdir(&lfs, "soda") => 0;
    lfs_mkdir(&lfs, "tea/hottea") => 0;
    lfs_mkdir(&lfs, "tea/warmtea") => 0;
    lfs_mkdir(&lfs, "tea/coldtea") => 0;
    lfs_mkdir(&lfs, "coffee/hotcoffee") => 0;
    lfs_mkdir(&lfs, "coffee/warmcoffee") => 0;
    lfs_mkdir(&lfs, "coffee/coldcoffee") => 0;
    lfs_mkdir(&lfs, "soda/hotsoda") => 0;
    lfs_mkdir(&lfs, "soda/warmsoda") => 0;
    lfs_mkdir(&lfs, "soda/coldsoda") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Root path tests ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_stat(&lfs, "tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "/tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Redundant slash path tests ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_stat(&lfs, "/tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "//tea//hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "///tea///hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Dot path tests ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_stat(&lfs, "./tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "/./tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "/././tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "/./tea/./hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Dot dot path tests ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_stat(&lfs, "coffee/../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "tea/coldtea/../hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "coffee/coldcoffee/../../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_stat(&lfs, "coffee/../soda/../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Root dot dot path tests ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_stat(&lfs, "coffee/../../../../../../tea/hottea", &info) => 0;
    strcmp(info.name, "hottea") => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Results ---"
tests/stats.py
