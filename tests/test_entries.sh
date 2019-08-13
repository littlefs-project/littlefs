#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Entry tests ==="

# Note: These tests are intended for 512 byte inline size at different
# inline sizes they should still pass, but won't be testing anything

rm -rf blocks
function read_file {
cat << TEST

    size = $2;
    lfs_file_open(&lfs, &file, "$1", LFS_O_RDONLY) => 0;
    lfs_file_read(&lfs, &file, rbuffer, size) => size;
    memcmp(rbuffer, wbuffer, size) => 0;
    lfs_file_close(&lfs, &file) => 0;
TEST
}

function write_file {
cat << TEST

    size = $2;
    lfs_file_open(&lfs, &file, "$1",
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) => 0;
    memset(wbuffer, 'c', size);
    lfs_file_write(&lfs, &file, wbuffer, size) => size;
    lfs_file_close(&lfs, &file) => 0;
TEST
}

echo "--- Entry grow test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    uint8_t wbuffer[1024];
    uint8_t rbuffer[1024];
    lfs_size_t size;

    lfs_mount(&lfs, &cfg) => 0;
    $(write_file "hi0" 20)
    $(write_file "hi1" 20)
    $(write_file "hi2" 20)
    $(write_file "hi3" 20)

    $(read_file "hi1" 20)
    $(write_file "hi1" 200)

    $(read_file "hi0" 20)
    $(read_file "hi1" 200)
    $(read_file "hi2" 20)
    $(read_file "hi3" 20)
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Entry shrink test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    uint8_t wbuffer[1024];
    uint8_t rbuffer[1024];
    lfs_size_t size;

    lfs_mount(&lfs, &cfg) => 0;
    $(write_file "hi0" 20)
    $(write_file "hi1" 200)
    $(write_file "hi2" 20)
    $(write_file "hi3" 20)

    $(read_file "hi1" 200)
    $(write_file "hi1" 20)

    $(read_file "hi0" 20)
    $(read_file "hi1" 20)
    $(read_file "hi2" 20)
    $(read_file "hi3" 20)
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Entry spill test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    uint8_t wbuffer[1024];
    uint8_t rbuffer[1024];
    lfs_size_t size;

    lfs_mount(&lfs, &cfg) => 0;
    $(write_file "hi0" 200)
    $(write_file "hi1" 200)
    $(write_file "hi2" 200)
    $(write_file "hi3" 200)

    $(read_file "hi0" 200)
    $(read_file "hi1" 200)
    $(read_file "hi2" 200)
    $(read_file "hi3" 200)
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Entry push spill test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    uint8_t wbuffer[1024];
    uint8_t rbuffer[1024];
    lfs_size_t size;

    lfs_mount(&lfs, &cfg) => 0;
    $(write_file "hi0" 200)
    $(write_file "hi1" 20)
    $(write_file "hi2" 200)
    $(write_file "hi3" 200)

    $(read_file "hi1" 20)
    $(write_file "hi1" 200)

    $(read_file "hi0" 200)
    $(read_file "hi1" 200)
    $(read_file "hi2" 200)
    $(read_file "hi3" 200)
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Entry push spill two test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    uint8_t wbuffer[1024];
    uint8_t rbuffer[1024];
    lfs_size_t size;

    lfs_mount(&lfs, &cfg) => 0;
    $(write_file "hi0" 200)
    $(write_file "hi1" 20)
    $(write_file "hi2" 200)
    $(write_file "hi3" 200)
    $(write_file "hi4" 200)

    $(read_file "hi1" 20)
    $(write_file "hi1" 200)

    $(read_file "hi0" 200)
    $(read_file "hi1" 200)
    $(read_file "hi2" 200)
    $(read_file "hi3" 200)
    $(read_file "hi4" 200)
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Entry drop test ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    uint8_t wbuffer[1024];
    uint8_t rbuffer[1024];
    lfs_size_t size;

    lfs_mount(&lfs, &cfg) => 0;
    $(write_file "hi0" 200)
    $(write_file "hi1" 200)
    $(write_file "hi2" 200)
    $(write_file "hi3" 200)

    lfs_remove(&lfs, "hi1") => 0;
    lfs_stat(&lfs, "hi1", &info) => LFS_ERR_NOENT;
    $(read_file "hi0" 200)
    $(read_file "hi2" 200)
    $(read_file "hi3" 200)

    lfs_remove(&lfs, "hi2") => 0;
    lfs_stat(&lfs, "hi2", &info) => LFS_ERR_NOENT;
    $(read_file "hi0" 200)
    $(read_file "hi3" 200)

    lfs_remove(&lfs, "hi3") => 0;
    lfs_stat(&lfs, "hi3", &info) => LFS_ERR_NOENT;
    $(read_file "hi0" 200)

    lfs_remove(&lfs, "hi0") => 0;
    lfs_stat(&lfs, "hi0", &info) => LFS_ERR_NOENT;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Create too big ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    lfs_mount(&lfs, &cfg) => 0;
    memset(path, 'm', 200);
    path[200] = '\0';

    lfs_size_t size = 400;
    lfs_file_open(&lfs, &file, path,
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) => 0;
    uint8_t wbuffer[1024];
    memset(wbuffer, 'c', size);
    lfs_file_write(&lfs, &file, wbuffer, size) => size;
    lfs_file_close(&lfs, &file) => 0;

    size = 400;
    lfs_file_open(&lfs, &file, path, LFS_O_RDONLY) => 0;
    uint8_t rbuffer[1024];
    lfs_file_read(&lfs, &file, rbuffer, size) => size;
    memcmp(rbuffer, wbuffer, size) => 0;
    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

echo "--- Resize too big ---"
scripts/test.py << TEST
    lfs_format(&lfs, &cfg) => 0;

    lfs_mount(&lfs, &cfg) => 0;
    memset(path, 'm', 200);
    path[200] = '\0';

    lfs_size_t size = 40;
    lfs_file_open(&lfs, &file, path,
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) => 0;
    uint8_t wbuffer[1024];
    memset(wbuffer, 'c', size);
    lfs_file_write(&lfs, &file, wbuffer, size) => size;
    lfs_file_close(&lfs, &file) => 0;

    size = 40;
    lfs_file_open(&lfs, &file, path, LFS_O_RDONLY) => 0;
    uint8_t rbuffer[1024];
    lfs_file_read(&lfs, &file, rbuffer, size) => size;
    memcmp(rbuffer, wbuffer, size) => 0;
    lfs_file_close(&lfs, &file) => 0;

    size = 400;
    lfs_file_open(&lfs, &file, path,
            LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) => 0;
    memset(wbuffer, 'c', size);
    lfs_file_write(&lfs, &file, wbuffer, size) => size;
    lfs_file_close(&lfs, &file) => 0;

    size = 400;
    lfs_file_open(&lfs, &file, path, LFS_O_RDONLY) => 0;
    lfs_file_read(&lfs, &file, rbuffer, size) => size;
    memcmp(rbuffer, wbuffer, size) => 0;
    lfs_file_close(&lfs, &file) => 0;
    lfs_unmount(&lfs) => 0;
TEST

scripts/results.py
