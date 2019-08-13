#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Formatting tests ==="
rm -rf blocks

echo "--- Basic formatting ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

echo "--- Basic mounting ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    lfs_mount(&lfs, &cfg) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Invalid superblocks ---"
ln -f -s /dev/zero blocks/0
ln -f -s /dev/zero blocks/1
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => LFS_ERR_NOSPC;
TEST
rm blocks/0 blocks/1

echo "--- Invalid mount ---"
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => LFS_ERR_CORRUPT;
TEST

echo "--- Expanding superblock ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
    lfs_mount(&lfs, &cfg) => 0;
    for (int i = 0; i < 100; i++) {
        lfs_mkdir(&lfs, "dummy") => 0;
        lfs_remove(&lfs, "dummy") => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
scripts/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "dummy") => 0;
    lfs_unmount(&lfs) => 0;
TEST

scripts/results.py
