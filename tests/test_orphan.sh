#!/bin/bash
set -eu

echo "=== Orphan tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs2_format(&lfs2, &cfg) => 0;
TEST

echo "--- Orphan test ---"
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    lfs2_mkdir(&lfs2, "parent") => 0;
    lfs2_mkdir(&lfs2, "parent/orphan") => 0;
    lfs2_mkdir(&lfs2, "parent/child") => 0;
    lfs2_remove(&lfs2, "parent/orphan") => 0;
TEST
# corrupt most recent commit, this should be the update to the previous
# linked-list entry and should orphan the child
tests/corrupt.py
tests/test.py << TEST
    lfs2_mount(&lfs2, &cfg) => 0;

    lfs2_stat(&lfs2, "parent/orphan", &info) => LFS2_ERR_NOENT;
    lfs2_ssize_t before = lfs2_fs_size(&lfs2);
    before => 8;

    lfs2_unmount(&lfs2) => 0;
    lfs2_mount(&lfs2, &cfg) => 0;

    lfs2_stat(&lfs2, "parent/orphan", &info) => LFS2_ERR_NOENT;
    lfs2_ssize_t orphaned = lfs2_fs_size(&lfs2);
    orphaned => 8;

    lfs2_mkdir(&lfs2, "parent/otherchild") => 0;

    lfs2_stat(&lfs2, "parent/orphan", &info) => LFS2_ERR_NOENT;
    lfs2_ssize_t deorphaned = lfs2_fs_size(&lfs2);
    deorphaned => 8;

    lfs2_unmount(&lfs2) => 0;
TEST

echo "--- Results ---"
tests/stats.py
