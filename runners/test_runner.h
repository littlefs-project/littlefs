#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include "lfs.h"


struct test_case {
    const char *id;
    const char *name;
    const char *path;
    uint32_t permutations;
    void (*run)(struct lfs_config *cfg, uint32_t perm);
};

struct test_suite {
    const char *id;
    const char *name;
    const char *path;
    const struct test_case *const *cases;
    size_t case_count;
};

extern const struct test_suite *test_suites[];
extern const size_t test_suite_count;

#endif
