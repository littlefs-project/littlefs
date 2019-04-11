#!/bin/bash
set -eu

echo "=== Corrupt tests ==="

NAMEMULT=64
FILEMULT=1

lfs2_mktree() {
tests/test.py ${1:-} << TEST
    lfs2_format(&lfs2, &cfg) => 0;

    lfs2_mount(&lfs2, &cfg) => 0;
    for (int i = 1; i < 10; i++) {
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j] = '0'+i;
        }
        buffer[$NAMEMULT] = '\0';
        lfs2_mkdir(&lfs2, (char*)buffer) => 0;

        buffer[$NAMEMULT] = '/';
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j+$NAMEMULT+1] = '0'+i;
        }
        buffer[2*$NAMEMULT+1] = '\0';
        lfs2_file_open(&lfs2, &file[0], (char*)buffer,
                LFS2_O_WRONLY | LFS2_O_CREAT) => 0;
        
        size = $NAMEMULT;
        for (int j = 0; j < i*$FILEMULT; j++) {
            lfs2_file_write(&lfs2, &file[0], buffer, size) => size;
        }

        lfs2_file_close(&lfs2, &file[0]) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
}

lfs2_chktree() {
tests/test.py ${1:-} << TEST
    lfs2_mount(&lfs2, &cfg) => 0;
    for (int i = 1; i < 10; i++) {
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j] = '0'+i;
        }
        buffer[$NAMEMULT] = '\0';
        lfs2_stat(&lfs2, (char*)buffer, &info) => 0;
        info.type => LFS2_TYPE_DIR;

        buffer[$NAMEMULT] = '/';
        for (int j = 0; j < $NAMEMULT; j++) {
            buffer[j+$NAMEMULT+1] = '0'+i;
        }
        buffer[2*$NAMEMULT+1] = '\0';
        lfs2_file_open(&lfs2, &file[0], (char*)buffer, LFS2_O_RDONLY) => 0;
        
        size = $NAMEMULT;
        for (int j = 0; j < i*$FILEMULT; j++) {
            lfs2_file_read(&lfs2, &file[0], rbuffer, size) => size;
            memcmp(buffer, rbuffer, size) => 0;
        }

        lfs2_file_close(&lfs2, &file[0]) => 0;
    }
    lfs2_unmount(&lfs2) => 0;
TEST
}

echo "--- Sanity check ---"
rm -rf blocks
lfs2_mktree
lfs2_chktree
BLOCKS="$(ls blocks | grep -vw '[01]')"

echo "--- Block corruption ---"
for b in $BLOCKS
do 
    rm -rf blocks
    mkdir blocks
    ln -s /dev/zero blocks/$b
    lfs2_mktree
    lfs2_chktree
done

echo "--- Block persistance ---"
for b in $BLOCKS
do 
    rm -rf blocks
    mkdir blocks
    lfs2_mktree
    chmod a-w blocks/$b || true
    lfs2_mktree
    lfs2_chktree
done

echo "--- Big region corruption ---"
rm -rf blocks
mkdir blocks
for i in {2..512}
do
    ln -s /dev/zero blocks/$(printf '%x' $i)
done
lfs2_mktree
lfs2_chktree

echo "--- Alternating corruption ---"
rm -rf blocks
mkdir blocks
for i in {2..1024..2}
do
    ln -s /dev/zero blocks/$(printf '%x' $i)
done
lfs2_mktree
lfs2_chktree

echo "--- Results ---"
tests/stats.py
