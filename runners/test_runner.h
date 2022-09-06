#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H


// override LFS_TRACE
void test_trace(const char *fmt, ...);

#define LFS_TRACE_(fmt, ...) \
    test_trace("%s:%d:trace: " fmt "%s\n", \
        __FILE__, \
        __LINE__, \
        __VA_ARGS__)
#define LFS_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")
#define LFS_TESTBD_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")


// note these are indirectly included in any generated files
#include "bd/lfs_testbd.h"
#include <stdio.h>

// give source a chance to define feature macros
#undef _FEATURES_H
#undef _STDIO_H


// generated test configurations
enum test_flags {
    TEST_REENTRANT = 0x1,
};
typedef uint8_t test_flags_t;

struct lfs_config;

struct test_case {
    const char *id;
    const char *name;
    const char *path;
    test_flags_t flags;
    size_t permutations;

    intmax_t (*const *const *defines)(size_t);

    bool (*filter)(void);
    void (*run)(struct lfs_config *cfg);
};

struct test_suite {
    const char *id;
    const char *name;
    const char *path;
    test_flags_t flags;

    const char *const *define_names;
    size_t define_count;

    const struct test_case *cases;
    size_t case_count;
};


// access generated test defines
//intmax_t test_predefine(size_t define);
intmax_t test_define(size_t define);

// a few preconfigured defines that control how tests run
 
#define READ_SIZE_i          0
#define PROG_SIZE_i          1
#define BLOCK_SIZE_i         2
#define BLOCK_COUNT_i        3
#define CACHE_SIZE_i         4
#define LOOKAHEAD_SIZE_i     5
#define BLOCK_CYCLES_i       6
#define ERASE_VALUE_i        7
#define ERASE_CYCLES_i       8
#define BADBLOCK_BEHAVIOR_i  9
#define POWERLOSS_BEHAVIOR_i 10

#define READ_SIZE           test_define(READ_SIZE_i)
#define PROG_SIZE           test_define(PROG_SIZE_i)
#define BLOCK_SIZE          test_define(BLOCK_SIZE_i)
#define BLOCK_COUNT         test_define(BLOCK_COUNT_i)
#define CACHE_SIZE          test_define(CACHE_SIZE_i)
#define LOOKAHEAD_SIZE      test_define(LOOKAHEAD_SIZE_i)
#define BLOCK_CYCLES        test_define(BLOCK_CYCLES_i)
#define ERASE_VALUE         test_define(ERASE_VALUE_i)
#define ERASE_CYCLES        test_define(ERASE_CYCLES_i)
#define BADBLOCK_BEHAVIOR   test_define(BADBLOCK_BEHAVIOR_i)
#define POWERLOSS_BEHAVIOR  test_define(POWERLOSS_BEHAVIOR_i)

#define TEST_IMPLICIT_DEFINES \
    TEST_DEFINE(READ_SIZE,          test_geometry->read_size) \
    TEST_DEFINE(PROG_SIZE,          test_geometry->prog_size) \
    TEST_DEFINE(BLOCK_SIZE,         test_geometry->block_size) \
    TEST_DEFINE(BLOCK_COUNT,        test_geometry->block_count) \
    TEST_DEFINE(CACHE_SIZE,         lfs_max(64,lfs_max(READ_SIZE,PROG_SIZE))) \
    TEST_DEFINE(LOOKAHEAD_SIZE,     16) \
    TEST_DEFINE(BLOCK_CYCLES,       -1) \
    TEST_DEFINE(ERASE_VALUE,        0xff) \
    TEST_DEFINE(ERASE_CYCLES,       0) \
    TEST_DEFINE(BADBLOCK_BEHAVIOR,  LFS_TESTBD_BADBLOCK_PROGERROR) \
    TEST_DEFINE(POWERLOSS_BEHAVIOR, LFS_TESTBD_POWERLOSS_NOOP)

#define TEST_GEOMETRY_DEFINE_COUNT 4
#define TEST_IMPLICIT_DEFINE_COUNT 11


#endif
