#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Formatting tests ==="
rm -rf blocks

echo "--- Basic formatting ---"
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST

echo "--- Basic mounting ---"
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;

    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Invalid superblocks ---"
ln -f -s /dev/zero blocks/0
ln -f -s /dev/zero blocks/1
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => LFS2_ERR_NOSPC;
TEST
rm blocks/0 blocks/1

echo "--- Invalid mount ---"
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => LFS2_ERR_CORRUPT;
TEST

echo "--- Expanding superblock ---"
scripts/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int i = 0; i < 100; i++) {
        lfs2_mkdir(&lfs2, "dummy") => 0;
        lfs2_remove(&lfs2, "dummy") => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
scripts/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "dummy") => 0;
    lfs2_unmount(&lfs2) => 0;
TEST

scripts/results.py
