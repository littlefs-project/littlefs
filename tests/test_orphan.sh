#!/bin/bash
set -eu

echo "=== Orphan tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

echo "--- Orphan test ---"
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "parent") => 0;
    lfs_mkdir(&lfs, "parent/orphan") => 0;
    lfs_mkdir(&lfs, "parent/child") => 0;
    lfs_remove(&lfs, "parent/orphan") => 0;
TEST
# corrupt most recent commit, this should be the update to the previous
# linked-list entry and should orphan the child
tests/corrupt.py
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;

    lfs_stat(&lfs, "parent/orphan", &info) => LFS_ERR_NOENT;
    lfs_ssize_t before = lfs_fs_size(&lfs);
    before => 8;

    lfs_unmount(&lfs) => 0;
    lfs_mount(&lfs, &cfg) => 0;

    lfs_stat(&lfs, "parent/orphan", &info) => LFS_ERR_NOENT;
    lfs_ssize_t orphaned = lfs_fs_size(&lfs);
    orphaned => 8;

    lfs_mkdir(&lfs, "parent/otherchild") => 0;

    lfs_stat(&lfs, "parent/orphan", &info) => LFS_ERR_NOENT;
    lfs_ssize_t deorphaned = lfs_fs_size(&lfs);
    deorphaned => 8;

    lfs_unmount(&lfs) => 0;
TEST

echo "--- Results ---"
tests/stats.py
