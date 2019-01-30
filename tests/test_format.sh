#!/bin/bash
set -eu

echo "=== Formatting tests ==="
rm -rf blocks

echo "--- Basic formatting ---"
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST

echo "--- Invalid superblocks ---"
ln -f -s /dev/zero blocks/0
ln -f -s /dev/zero blocks/1
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => LFS1_ERR_CORRUPT;
TEST
rm blocks/0 blocks/1

echo "--- Basic mounting ---"
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Invalid mount ---"
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST
rm blocks/0 blocks/1
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => LFS1_ERR_CORRUPT;
TEST

echo "--- Valid corrupt mount ---"
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST
rm blocks/0
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Results ---"
tests/stats.py
