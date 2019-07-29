#!/bin/bash
set -eu
export TEST_FILE=$0
trap 'export TEST_LINE=$LINENO' DEBUG

echo "=== Corrupt tests ==="

NAMEMULT=64
FILEMULT=1

lfs_mktree() {
scripts/test.py ${1:-} << TEST
    lfs_format(&lfs, &cfg) => 0;

    lfs_mount(&lfs, &cfg) => 0;
    for (int i = 1; i < 10; i++) {
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j] = '0'+i;
        }
        buffer[$NAMEMULT] = '\0';
        lfs_mkdir(&lfs, (char*)buffer) => 0;

        buffer[$NAMEMULT] = '/';
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j+$NAMEMULT+1] = '0'+i;
        }
        buffer[2*$NAMEMULT+1] = '\0';
        lfs_file_open(&lfs, &file, (char*)buffer,
                LFS_O_WRONLY | LFS_O_CREAT) => 0;
        
        lfs_size_t size = $NAMEMULT;
        for (int j = 0; j < i*$FILEMULT; j++) {
            lfs_file_write(&lfs, &file, buffer, size) => size;
        }

        lfs_file_close(&lfs, &file) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
}

lfs_chktree() {
scripts/test.py ${1:-} << TEST
    lfs_mount(&lfs, &cfg) => 0;
    for (int i = 1; i < 10; i++) {
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j] = '0'+i;
        }
        buffer[$NAMEMULT] = '\0';
        lfs_stat(&lfs, (char*)buffer, &info) => 0;
        info.type => LFS_TYPE_DIR;

        buffer[$NAMEMULT] = '/';
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j+$NAMEMULT+1] = '0'+i;
        }
        buffer[2*$NAMEMULT+1] = '\0';
        lfs_file_open(&lfs, &file, (char*)buffer, LFS_O_RDONLY) => 0;
        
        lfs_size_t size = $NAMEMULT;
        for (int j = 0; j < i*$FILEMULT; j++) {
            uint8_t rbuffer[1024];
            lfs_file_read(&lfs, &file, rbuffer, size) => size;
            memcmp(buffer, rbuffer, size) => 0;
        }

        lfs_file_close(&lfs, &file) => 0;
    }
    lfs_unmount(&lfs) => 0;
TEST
}

echo "--- Sanity check ---"
rm -rf blocks
lfs_mktree
lfs_chktree
BLOCKS="$(ls blocks | grep -vw '[01]')"

echo "--- Block corruption ---"
for b in $BLOCKS
do 
    rm -rf blocks
    mkdir blocks
    ln -s /dev/zero blocks/$b
    lfs_mktree
    lfs_chktree
done

echo "--- Block persistance ---"
for b in $BLOCKS
do 
    rm -rf blocks
    mkdir blocks
    lfs_mktree
    chmod a-w blocks/$b || true
    lfs_mktree
    lfs_chktree
done

echo "--- Big region corruption ---"
rm -rf blocks
mkdir blocks
for i in {2..512}
do
    ln -s /dev/zero blocks/$(printf '%x' $i)
done
lfs_mktree
lfs_chktree

echo "--- Alternating corruption ---"
rm -rf blocks
mkdir blocks
for i in {2..1024..2}
do
    ln -s /dev/zero blocks/$(printf '%x' $i)
done
lfs_mktree
lfs_chktree

scripts/results.py
