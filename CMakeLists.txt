add_library(littlefs)

target_sources(littlefs PUBLIC
        lfs.c
        lfs_util.c)

target_include_directories(littlefs PUBLIC
        .)