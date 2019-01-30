#!/bin/bash
set -eu

echo "=== Interspersed tests ==="
rm -rf blocks
tests/test.py << TEST
    lfs1_format(&lfs1, &cfg) => 0;
TEST

echo "--- Interspersed file test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "a", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    lfs1_file_open(&lfs1, &file[1], "b", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    lfs1_file_open(&lfs1, &file[2], "c", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    lfs1_file_open(&lfs1, &file[3], "d", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;

    for (int i = 0; i < 10; i++) {
        lfs1_file_write(&lfs1, &file[0], (const void*)"a", 1) => 1;
        lfs1_file_write(&lfs1, &file[1], (const void*)"b", 1) => 1;
        lfs1_file_write(&lfs1, &file[2], (const void*)"c", 1) => 1;
        lfs1_file_write(&lfs1, &file[3], (const void*)"d", 1) => 1;
    }

    lfs1_file_close(&lfs1, &file[0]);
    lfs1_file_close(&lfs1, &file[1]);
    lfs1_file_close(&lfs1, &file[2]);
    lfs1_file_close(&lfs1, &file[3]);

    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "a") => 0;
    info.type => LFS1_TYPE_REG;
    info.size => 10;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "b") => 0;
    info.type => LFS1_TYPE_REG;
    info.size => 10;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "c") => 0;
    info.type => LFS1_TYPE_REG;
    info.size => 10;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "d") => 0;
    info.type => LFS1_TYPE_REG;
    info.size => 10;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;

    lfs1_file_open(&lfs1, &file[0], "a", LFS1_O_RDONLY) => 0;
    lfs1_file_open(&lfs1, &file[1], "b", LFS1_O_RDONLY) => 0;
    lfs1_file_open(&lfs1, &file[2], "c", LFS1_O_RDONLY) => 0;
    lfs1_file_open(&lfs1, &file[3], "d", LFS1_O_RDONLY) => 0;

    for (int i = 0; i < 10; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, 1) => 1;
        buffer[0] => 'a';
        lfs1_file_read(&lfs1, &file[1], buffer, 1) => 1;
        buffer[0] => 'b';
        lfs1_file_read(&lfs1, &file[2], buffer, 1) => 1;
        buffer[0] => 'c';
        lfs1_file_read(&lfs1, &file[3], buffer, 1) => 1;
        buffer[0] => 'd';
    }

    lfs1_file_close(&lfs1, &file[0]);
    lfs1_file_close(&lfs1, &file[1]);
    lfs1_file_close(&lfs1, &file[2]);
    lfs1_file_close(&lfs1, &file[3]);
    
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Interspersed remove file test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "e", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;

    for (int i = 0; i < 5; i++) {
        lfs1_file_write(&lfs1, &file[0], (const void*)"e", 1) => 1;
    }

    lfs1_remove(&lfs1, "a") => 0;
    lfs1_remove(&lfs1, "b") => 0;
    lfs1_remove(&lfs1, "c") => 0;
    lfs1_remove(&lfs1, "d") => 0;

    for (int i = 0; i < 5; i++) {
        lfs1_file_write(&lfs1, &file[0], (const void*)"e", 1) => 1;
    }

    lfs1_file_close(&lfs1, &file[0]);

    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "e") => 0;
    info.type => LFS1_TYPE_REG;
    info.size => 10;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;

    lfs1_file_open(&lfs1, &file[0], "e", LFS1_O_RDONLY) => 0;

    for (int i = 0; i < 10; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, 1) => 1;
        buffer[0] => 'e';
    }

    lfs1_file_close(&lfs1, &file[0]);
    
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Remove inconveniently test ---"
tests/test.py << TEST
    lfs1_mount(&lfs1, &cfg) => 0;
    lfs1_file_open(&lfs1, &file[0], "e", LFS1_O_WRONLY | LFS1_O_TRUNC) => 0;
    lfs1_file_open(&lfs1, &file[1], "f", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;
    lfs1_file_open(&lfs1, &file[2], "g", LFS1_O_WRONLY | LFS1_O_CREAT) => 0;

    for (int i = 0; i < 5; i++) {
        lfs1_file_write(&lfs1, &file[0], (const void*)"e", 1) => 1;
        lfs1_file_write(&lfs1, &file[1], (const void*)"f", 1) => 1;
        lfs1_file_write(&lfs1, &file[2], (const void*)"g", 1) => 1;
    }

    lfs1_remove(&lfs1, "f") => 0;

    for (int i = 0; i < 5; i++) {
        lfs1_file_write(&lfs1, &file[0], (const void*)"e", 1) => 1;
        lfs1_file_write(&lfs1, &file[1], (const void*)"f", 1) => 1;
        lfs1_file_write(&lfs1, &file[2], (const void*)"g", 1) => 1;
    }

    lfs1_file_close(&lfs1, &file[0]);
    lfs1_file_close(&lfs1, &file[1]);
    lfs1_file_close(&lfs1, &file[2]);

    lfs1_dir_open(&lfs1, &dir[0], "/") => 0;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, ".") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "..") => 0;
    info.type => LFS1_TYPE_DIR;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "e") => 0;
    info.type => LFS1_TYPE_REG;
    info.size => 10;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 1;
    strcmp(info.name, "g") => 0;
    info.type => LFS1_TYPE_REG;
    info.size => 10;
    lfs1_dir_read(&lfs1, &dir[0], &info) => 0;
    lfs1_dir_close(&lfs1, &dir[0]) => 0;

    lfs1_file_open(&lfs1, &file[0], "e", LFS1_O_RDONLY) => 0;
    lfs1_file_open(&lfs1, &file[1], "g", LFS1_O_RDONLY) => 0;

    for (int i = 0; i < 10; i++) {
        lfs1_file_read(&lfs1, &file[0], buffer, 1) => 1;
        buffer[0] => 'e';
        lfs1_file_read(&lfs1, &file[1], buffer, 1) => 1;
        buffer[0] => 'g';
    }

    lfs1_file_close(&lfs1, &file[0]);
    lfs1_file_close(&lfs1, &file[1]);
    
    lfs1_unmount(&lfs1) => 0;
TEST

echo "--- Results ---"
tests/stats.py
