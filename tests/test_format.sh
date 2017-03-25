#!/bin/bash
set -eu

echo "=== Formatting tests ==="
rm -rf blocks

echo "--- Basic formatting ---"
tests/test.py << TEST
    lfs_format(&lfs, &config) => 0;
TEST

echo "--- Invalid superblocks ---"
ln -f -s /dev/null blocks/0
tests/test.py << TEST
    lfs_format(&lfs, &config) => LFS_ERROR_CORRUPT;
TEST
rm blocks/0

echo "--- Basic mounting ---"
tests/test.py << TEST
    lfs_mount(&lfs, &config) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Invalid mount ---"
tests/test.py << TEST
    lfs_format(&lfs, &config) => 0;
TEST
rm blocks/0 blocks/1
tests/test.py << TEST
    lfs_mount(&lfs, &config) => LFS_ERROR_CORRUPT;
TEST

echo "--- Valid corrupt mount ---"
tests/test.py << TEST
    lfs_format(&lfs, &config) => 0;
TEST
rm blocks/0
tests/test.py << TEST
    lfs_mount(&lfs, &config) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Results ---"
tests/stats.py
