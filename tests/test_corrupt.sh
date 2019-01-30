#!/bin/bash
set -eu

echo "=== Corrupt tests ==="

NAMEMULT=64
FILEMULT=1

lfs1_mktree() {
tests/test.py ${1:-} << TEST
    lfs1_format(&lfs1, &cfg) => 0;

    lfs1_mount(&lfs1, &cfg) => 0;
    for (int i = 1; i < 10; i++) {
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j] = '0'+i;
        }
        buffer[$NAMEMULT] = '\0';
        lfs1_mkdir(&lfs1, (char*)buffer) => 0;

        buffer[$NAMEMULT] = '/';
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j+$NAMEMULT+1] = '0'+i;
        }
        buffer[2*$NAMEMULT+1] = '\0';
        lfs1_file_open(&lfs1, &file[0], (char*)buffer,
                LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
        
        size = $NAMEMULT;
        for (int j = 0; j < i*$FILEMULT; j++) {
            lfs1_file_write(&lfs1, &file[0], buffer, size) => size;
        }

        lfs1_file_close(&lfs1, &file[0]) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST
}

lfs1_chktree() {
tests/test.py ${1:-} << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    for (int i = 1; i < 10; i++) {
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j] = '0'+i;
        }
        buffer[$NAMEMULT] = '\0';
        lfs1_stat(&lfs1, (char*)buffer, &info) => 0;
        info.type => LFS1_TYPE_DIR;

        buffer[$NAMEMULT] = '/';
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j+$NAMEMULT+1] = '0'+i;
        }
        buffer[2*$NAMEMULT+1] = '\0';
        lfs1_file_open(&lfs1, &file[0], (char*)buffer, LFS1_O_RDONLY) => 0;
        
        size = $NAMEMULT;
        for (int j = 0; j < i*$FILEMULT; j++) {
            lfs1_file_read(&lfs1, &file[0], rbuffer, size) => size;
            memcmp(buffer, rbuffer, size) => 0;
        }

        lfs1_file_close(&lfs1, &file[0]) => 0;
    }
    lfs1_unmount(&lfs1) => 0;
TEST
}

echo "--- Sanity check ---"
rm -rf blocks
lfs1_mktree
lfs1_chktree

echo "--- Block corruption ---"
for i in {0..33}
do 
    rm -rf blocks
    mkdir blocks
    ln -s /dev/zero blocks/$(printf '%x' $i)
    lfs1_mktree
    lfs1_chktree
done

echo "--- Block persistance ---"
for i in {0..33}
do 
    rm -rf blocks
    mkdir blocks
    lfs1_mktree
    chmod a-w blocks/$(printf '%x' $i)
    lfs1_mktree
    lfs1_chktree
done

echo "--- Big region corruption ---"
rm -rf blocks
mkdir blocks
for i in {2..255}
do
    ln -s /dev/zero blocks/$(printf '%x' $i)
done
lfs1_mktree
lfs1_chktree

echo "--- Alternating corruption ---"
rm -rf blocks
mkdir blocks
for i in {2..511..2}
do
    ln -s /dev/zero blocks/$(printf '%x' $i)
done
lfs1_mktree
lfs1_chktree

echo "--- Results ---"
tests/stats.py
