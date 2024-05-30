/*
 * Runner for littlefs tests
 *
 * Copyright (c) 2022, The littlefs authors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
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
#define LFS_EMUBD_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")


// note these are indirectly included in any generated files
#include "bd/lfs_emubd.h"
#include <stdio.h>

// give source a chance to define feature macros
#undef _FEATURES_H
#undef _STDIO_H


// generated test configurations
struct lfs_config;

enum test_flags {
    TEST_INTERNAL  = 0x1,
    TEST_REENTRANT = 0x2,
    TEST_FUZZ      = 0x4,
};
typedef uint8_t test_flags_t;

typedef struct test_define {
    const char *name;
    intmax_t *define;
    intmax_t (*cb)(void *data, size_t i);
    void *data;
    size_t permutations;
} test_define_t;

struct test_case {
    const char *name;
    const char *path;
    test_flags_t flags;

    const test_define_t *defines;
    size_t permutations;

    bool (*if_)(void);
    void (*run)(struct lfs_config *cfg);
};

struct test_suite {
    const char *name;
    const char *path;
    test_flags_t flags;

    const test_define_t *defines;
    size_t define_count;

    const struct test_case *cases;
    size_t case_count;
};

extern const struct test_suite *const test_suites[];
extern const size_t test_suite_count;


// this variable tracks the number of powerlosses triggered during the
// current test permutation, this is useful for both tests and debugging
extern volatile size_t TEST_PLS;

// deterministic prng for pseudo-randomness in tests
uint32_t test_prng(uint32_t *state);

#define TEST_PRNG(state) test_prng(state)

// generation of specific permutations of an array for exhaustive testing
size_t test_factorial(size_t x);
void test_permutation(size_t i, uint32_t *buffer, size_t size);

#define TEST_FACTORIAL(x) test_factorial(x)
#define TEST_PERMUTATION(i, buffer, size) test_permutation(i, buffer, size)


// a few preconfigured defines that control how tests run
#define TEST_IMPLICIT_DEFINES \
    /*          name                value (overridable)                    */ \
    TEST_DEFINE(READ_SIZE,          1                                       ) \
    TEST_DEFINE(PROG_SIZE,          1                                       ) \
    TEST_DEFINE(BLOCK_SIZE,         4096                                    ) \
    TEST_DEFINE(BLOCK_COUNT,        DISK_SIZE/BLOCK_SIZE                    ) \
    TEST_DEFINE(DISK_SIZE,          1024*1024                               ) \
    TEST_DEFINE(BLOCK_RECYCLES,     -1                                      ) \
    TEST_DEFINE(RCACHE_SIZE,        LFS_MAX(16, READ_SIZE)                  ) \
    TEST_DEFINE(PCACHE_SIZE,        LFS_MAX(16, PROG_SIZE)                  ) \
    TEST_DEFINE(FILE_BUFFER_SIZE,   16                                      ) \
    TEST_DEFINE(LOOKAHEAD_SIZE,     16                                      ) \
    TEST_DEFINE(INLINE_SIZE,        BLOCK_SIZE/4                            ) \
    TEST_DEFINE(SHRUB_SIZE,         INLINE_SIZE                             ) \
    TEST_DEFINE(FRAGMENT_SIZE,      BLOCK_SIZE/8                            ) \
    TEST_DEFINE(CRYSTAL_THRESH,     BLOCK_SIZE/8                            ) \
    TEST_DEFINE(CHECK_PROGS,        false                                   ) \
    TEST_DEFINE(ERASE_VALUE,        0xff                                    ) \
    TEST_DEFINE(ERASE_CYCLES,       0                                       ) \
    TEST_DEFINE(BADBLOCK_BEHAVIOR,  LFS_EMUBD_BADBLOCK_PROGERROR            ) \
    TEST_DEFINE(POWERLOSS_BEHAVIOR, LFS_EMUBD_POWERLOSS_NOOP                )

// declare defines as global intmax_ts
#define TEST_DEFINE(k, v) \
    extern intmax_t k;

    TEST_IMPLICIT_DEFINES
#undef TEST_DEFINE

// map defines to cfg struct fields
#define TEST_CFG \
    .read_size          = READ_SIZE,            \
    .prog_size          = PROG_SIZE,            \
    .block_size         = BLOCK_SIZE,           \
    .block_count        = BLOCK_COUNT,          \
    .block_recycles     = BLOCK_RECYCLES,       \
    .rcache_size        = RCACHE_SIZE,          \
    .pcache_size        = PCACHE_SIZE,          \
    .file_buffer_size   = FILE_BUFFER_SIZE,     \
    .lookahead_size     = LOOKAHEAD_SIZE,       \
    .inline_size        = INLINE_SIZE,          \
    .shrub_size         = SHRUB_SIZE,           \
    .fragment_size      = FRAGMENT_SIZE,        \
    .crystal_thresh     = CRYSTAL_THRESH,       \
    .check_progs        = CHECK_PROGS,

#define TEST_BDCFG \
    .erase_value        = ERASE_VALUE,          \
    .erase_cycles       = ERASE_CYCLES,         \
    .badblock_behavior  = BADBLOCK_BEHAVIOR,


#endif
