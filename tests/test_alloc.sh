#!/bin/bash
set -eu

echo "=== Allocator tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;
TEST

SIZE=15000

lfs_mkdir() {
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_mkdir(&lfs, "$1") => 0;
    lfs_unmount(&lfs) => 0;
TEST
}

lfs_remove() {
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_remove(&lfs, "$1/eggs") => 0;
    lfs_remove(&lfs, "$1/bacon") => 0;
    lfs_remove(&lfs, "$1/pancakes") => 0;
    lfs_remove(&lfs, "$1") => 0;
    lfs_unmount(&lfs) => 0;
TEST
}

lfs_alloc_singleproc() {
tests/test.py << TEST
    const char *names[] = {"bacon", "eggs", "pancakes"};
    lfs_mount(&lfs, &cfg) => 0;
    for (int n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        sprintf((char*)buffer, "$1/%s", names[n]);
        lfs_file_open(&lfs, &file[n], (char*)buffer,
                LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND) => 0;
    }
    for (int n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        size = strlen(names[n]);
        for (int i = 0; i < $SIZE; i++) {
            lfs_file_write(&lfs, &file[n], names[n], size) => size;
        }
    }
    for (int n = 0; n < sizeof(names)/sizeof(names[0]); n++) {
        lfs_file_close(&lfs, &file[n]) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
}

lfs_alloc_multiproc() {
for name in bacon eggs pancakes
do
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "$1/$name",
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_APPEND) => 0;
    size = strlen("$name");
    memcpy(buffer, "$name", size);
    for (int i = 0; i < $SIZE; i++) {
        lfs_file_write(&lfs, &file[0], buffer, size) => size;
    }
    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST
done
}

lfs_verify() {
for name in bacon eggs pancakes
do
tests/test.py << TEST
    lfs_mount(&lfs, &cfg) => 0;
    lfs_file_open(&lfs, &file[0], "$1/$name", LFS_O_RDONLY) => 0;
    size = strlen("$name");
    for (int i = 0; i < $SIZE; i++) {
        lfs_file_read(&lfs, &file[0], buffer, size) => size;
        memcmp(buffer, "$name", size) => 0;
    }
    lfs_file_close(&lfs, &file[0]) => 0;
    lfs_unmount(&lfs) => 0;
TEST
done
}

echo "--- Single-process allocation test ---"
lfs_mkdir singleproc
lfs_alloc_singleproc singleproc
lfs_verify singleproc

echo "--- Multi-process allocation test ---"
lfs_mkdir multiproc
lfs_alloc_multiproc multiproc
lfs_verify multiproc
lfs_verify singleproc

echo "--- Single-process reuse test ---"
lfs_remove singleproc
lfs_mkdir singleprocreuse
lfs_alloc_singleproc singleprocreuse
lfs_verify singleprocreuse
lfs_verify multiproc

echo "--- Multi-process reuse test ---"
lfs_remove multiproc
lfs_mkdir multiprocreuse
lfs_alloc_singleproc multiprocreuse
lfs_verify multiprocreuse
lfs_verify singleprocreuse

echo "--- Results ---"
tests/stats.py
