#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include "lfs.h"


// generated test configurations
enum test_types {
    TEST_NORMAL    = 0x1,
    TEST_REENTRANT = 0x2,
    TEST_VALGRIND  = 0x4,
};

typedef uint8_t test_types_t;
typedef uintmax_t test_define_t;

struct test_case {
    const char *id;
    const char *name;
    const char *path;
    test_types_t types;
    size_t permutations;

    const test_define_t *const *defines;
    const uint8_t *define_map;

    bool (*filter)(struct lfs_config *cfg, uint32_t perm);
    void (*run)(struct lfs_config *cfg, uint32_t perm);
};

struct test_suite {
    const char *id;
    const char *name;
    const char *path;

    const char *const *define_names;
    size_t define_count;

    const struct test_case *const *cases;
    size_t case_count;
};

// TODO remove this indirection
extern const struct test_suite *test_suites[];
extern const size_t test_suite_count;


// access generated test defines
test_define_t test_define(size_t define);

// a few preconfigured defines that control how tests run
#define READ_SIZE           test_define(0)
#define PROG_SIZE           test_define(1)
#define BLOCK_SIZE          test_define(2)
#define BLOCK_COUNT         test_define(3)
#define BLOCK_CYCLES        test_define(4)
#define CACHE_SIZE          test_define(5)
#define LOOKAHEAD_SIZE      test_define(6)
#define ERASE_VALUE         test_define(7)
#define ERASE_CYCLES        test_define(8)
#define BADBLOCK_BEHAVIOR   test_define(9)


#endif
