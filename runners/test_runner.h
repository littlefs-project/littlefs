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

    intmax_t (*const *const *defines)(void);

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
intmax_t test_predefine(size_t define);
intmax_t test_define(size_t define);

// a few preconfigured defines that control how tests run
#define READ_SIZE           test_predefine(0)
#define PROG_SIZE           test_predefine(1)
#define BLOCK_SIZE          test_predefine(2)
#define BLOCK_COUNT         test_predefine(3)
#define CACHE_SIZE          test_predefine(4)
#define LOOKAHEAD_SIZE      test_predefine(5)
#define BLOCK_CYCLES        test_predefine(6)
#define ERASE_VALUE         test_predefine(7)
#define ERASE_CYCLES        test_predefine(8)
#define BADBLOCK_BEHAVIOR   test_predefine(9)
#define POWERLOSS_BEHAVIOR  test_predefine(10)

#define TEST_PREDEFINE_NAMES { \
    "READ_SIZE", \
    "PROG_SIZE", \
    "BLOCK_SIZE", \
    "BLOCK_COUNT", \
    "CACHE_SIZE", \
    "LOOKAHEAD_SIZE", \
    "BLOCK_CYCLES", \
    "ERASE_VALUE", \
    "ERASE_CYCLES", \
    "BADBLOCK_BEHAVIOR", \
    "POWERLOSS_BEHAVIOR", \
}
#define TEST_PREDEFINE_COUNT 11


// default predefines
#define TEST_DEFAULTS { \
    /* LOOKAHEAD_SIZE */     16, \
    /* BLOCK_CYCLES */       -1, \
    /* ERASE_VALUE */        0xff, \
    /* ERASE_CYCLES */       0, \
    /* BADBLOCK_BEHAVIOR */  LFS_TESTBD_BADBLOCK_PROGERROR, \
    /* POWERLOSS_BEHAVIOR */ LFS_TESTBD_POWERLOSS_NOOP, \
}
#define TEST_DEFAULT_DEFINE_COUNT 5

// test geometries
#define TEST_GEOMETRIES { \
    /*geometry, read, write,    erase,                 count, cache */ \
    {"test",   {  16,    16,      512,       (1024*1024)/512,    64}}, \
    {"eeprom", {   1,     1,      512,       (1024*1024)/512,    64}}, \
    {"emmc",   { 512,   512,      512,       (1024*1024)/512,   512}}, \
    {"nor",    {   1,     1,     4096,      (1024*1024)/4096,    64}}, \
    {"nand",   {4096,  4096,  32*1024, (1024*1024)/(32*1024),  4096}}, \
}
#define TEST_GEOMETRY_COUNT 5
#define TEST_GEOMETRY_DEFINE_COUNT 5


#endif
