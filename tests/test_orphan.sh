#!/bin/bash
set -eu

echo "=== Orphan tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST

echo "--- Orphan test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_mkdir(&lfs1, "parent") => 0;
    lfs1_mkdir(&lfs1, "parent/orphan") => 0;
    lfs1_mkdir(&lfs1, "parent/child") => 0;
    lfs1_remove(&lfs1, "parent/orphan") => 0;
TEST
# remove most recent file, this should be the update to the previous
# linked-list entry and should orphan the child
rm -v blocks/8
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_stat(&lfs1, "parent/orphan", &info) => LFS1_ERR_NOENT;
    unsigned before = 0;
    lfs1_traverse(&lfs1, test_count, &before) => 0;
    test_log("before", before);

    lfs1_deorphan(&lfs1) => 0;

    lfs1_stat(&lfs1, "parent/orphan", &info) => LFS1_ERR_NOENT;
    unsigned after = 0;
    lfs1_traverse(&lfs1, test_count, &after) => 0;
    test_log("after", after);

    int diff = before - after;
    diff => 2;
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Results ---"
tests/stats.py
