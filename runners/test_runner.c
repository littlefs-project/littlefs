
#include "runners/test_runner.h"
#include "bd/lfs_testbd.h"

#include <getopt.h>
#include <sys/types.h>
#include <errno.h>
#include <setjmp.h>


// test suites in a custom ld section
extern struct test_suite __start__test_suites;
extern struct test_suite __stop__test_suites;

const struct test_suite *test_suites = &__start__test_suites;
#define TEST_SUITE_COUNT \
    ((size_t)(&__stop__test_suites - &__start__test_suites))

// test geometries
struct test_geometry {
    const char *name;
    intmax_t defines[TEST_GEOMETRY_DEFINE_COUNT];
};

const struct test_geometry test_geometries[TEST_GEOMETRY_COUNT]
        = TEST_GEOMETRIES;

// test define lookup and management
const intmax_t *test_override_defines;
intmax_t (*const *test_case_defines)(void);
const intmax_t *test_geometry_defines;
const intmax_t test_default_defines[TEST_PREDEFINE_COUNT]
        = TEST_DEFAULTS;

uint8_t test_override_predefine_map[TEST_PREDEFINE_COUNT];
uint8_t test_override_define_map[256];
uint8_t test_case_predefine_map[TEST_PREDEFINE_COUNT];

const char *const *test_override_names;
size_t test_override_count;

const char *const test_predefine_names[TEST_PREDEFINE_COUNT]
        = TEST_PREDEFINE_NAMES;

const char *const *test_define_names;
size_t test_define_count;


intmax_t test_predefine(size_t define) {
    if (test_override_defines
            && test_override_predefine_map[define] != 0xff) {
        return test_override_defines[test_override_predefine_map[define]];
    } else if (test_case_defines
            && test_case_predefine_map[define] != 0xff
            && test_case_defines[test_case_predefine_map[define]]) {
        return test_case_defines[test_case_predefine_map[define]]();
    } else if (define < TEST_GEOMETRY_DEFINE_COUNT) {
        return test_geometry_defines[define];
    } else {
        return test_default_defines[define-TEST_GEOMETRY_DEFINE_COUNT];
    }
}

intmax_t test_define(size_t define) {
    if (test_override_defines
            && test_override_define_map[define] != 0xff) {
        return test_override_defines[test_override_define_map[define]];
    } else if (test_case_defines
            && test_case_defines[define]) {
        return test_case_defines[define]();
    }

    fprintf(stderr, "error: undefined define %s\n",
            test_define_names[define]);
    assert(false);
    exit(-1);
}

static void define_geometry(const struct test_geometry *geometry) {
    test_geometry_defines = geometry->defines;
}

static void test_define_overrides(
        const char *const *override_names,
        const intmax_t *override_defines,
        size_t override_count) {
    test_override_defines = override_defines;
    test_override_names = override_names;
    test_override_count = override_count;

    // map any override predefines
    memset(test_override_predefine_map, 0xff, TEST_PREDEFINE_COUNT);
    for (size_t i = 0; i < test_override_count; i++) {
        for (size_t j = 0; j < TEST_PREDEFINE_COUNT; j++) {
            if (strcmp(test_override_names[i], test_predefine_names[j]) == 0) {
                test_override_predefine_map[j] = i;
            }
        }
    }
}

static void define_suite(const struct test_suite *suite) {
    test_define_names = suite->define_names;
    test_define_count = suite->define_count;

    // map any override defines
    memset(test_override_define_map, 0xff, suite->define_count);
    for (size_t i = 0; i < test_override_count; i++) {
        for (size_t j = 0; j < suite->define_count; j++) {
            if (strcmp(test_override_names[i], suite->define_names[j]) == 0) {
                test_override_define_map[j] = i;
            }
        }
    }

    // map any suite/case predefines
    memset(test_case_predefine_map, 0xff, TEST_PREDEFINE_COUNT);
    for (size_t i = 0; i < suite->define_count; i++) {
        for (size_t j = 0; j < TEST_PREDEFINE_COUNT; j++) {
            if (strcmp(suite->define_names[i], test_predefine_names[j]) == 0) {
                test_case_predefine_map[j] = i;
            }
        }
    }
}

static void define_perm(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm) {
    (void)suite;
    if (case_->defines) {
        test_case_defines = case_->defines[perm];
    } else {
        test_case_defines = NULL;
    }
}


// a quick encoding scheme for sequences of power-loss
static void leb16_print(
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    for (size_t i = 0; i < cycle_count; i++) {
        lfs_testbd_powercycles_t x = cycles[i];
        while (true) {
            lfs_testbd_powercycles_t nibble = (x & 0xf) | (x > 0xf ? 0x10 : 0);
            printf("%c", (nibble < 10) ? '0'+nibble : 'a'+nibble-10);
            if (x <= 0xf) {
                break;
            }
            x >>= 4;
        }
    }
}

static size_t leb16_parse(const char *s, char **tail,
        lfs_testbd_powercycles_t **cycles) {
    // first lets count how many number we're dealing with
    size_t count = 0;
    size_t len = 0;
    for (size_t i = 0;; i++) {
        if ((s[i] >= '0' && s[i] <= '9')
                || (s[i] >= 'a' && s[i] <= 'f')) {
            len = i+1;
            count += 1;
        } else if ((s[i] >= 'g' && s[i] <= 'v')) {
            // do nothing
        } else {
            break;
        }
    }

    // then parse
    lfs_testbd_powercycles_t *cycles_ = malloc(
            count * sizeof(lfs_testbd_powercycles_t));
    size_t i = 0;
    lfs_testbd_powercycles_t x = 0;
    size_t k = 0;
    for (size_t j = 0; j < len; j++) {
        lfs_testbd_powercycles_t nibble = s[j];
        nibble = (nibble < 'a') ? nibble-'0' : nibble-'a'+10;
        x |= (nibble & 0xf) << (4*k);
        k += 1;
        if (!(nibble & 0x10)) {
            cycles_[i] = x;
            i += 1;
            x = 0;
            k = 0;
        }
    }

    if (tail) {
        *tail = (char*)s + len;
    }
    *cycles = cycles_;
    return count;
}


// test state
typedef struct test_powerloss {
    char short_name;
    const char *long_name;

    void (*run)(
            const struct test_suite *suite,
            const struct test_case *case_,
            size_t perm,
            const lfs_testbd_powercycles_t *cycles,
            size_t cycle_count);
    const lfs_testbd_powercycles_t *cycles;
    size_t cycle_count;
} test_powerloss_t;

static void run_powerloss_none(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count);
static const test_powerloss_t *test_powerlosses = (const test_powerloss_t[]){
    {'0', "none", run_powerloss_none, NULL, 0},
};
static size_t test_powerloss_count = 1;


typedef struct test_id {
    const char *suite;
    const char *case_;
    size_t perm;
    const lfs_testbd_powercycles_t *cycles;
    size_t cycle_count;
} test_id_t;

static const test_id_t *test_ids = (const test_id_t[]) {
    {NULL, NULL, -1, NULL, 0},
};
static size_t test_id_count = 1;


static const char *test_geometry = NULL;

static size_t test_start = 0;
static size_t test_stop = -1;
static size_t test_step = 1;

static const char *test_disk = NULL;
FILE *test_trace = NULL;


// how many permutations are there actually in a test case
static void count_perms(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count,
        size_t *perms,
        size_t *filtered) {
    (void)cycle_count;
    size_t perms_ = 0;
    size_t filtered_ = 0;

    for (size_t p = 0; p < (cycles ? 1 : test_powerloss_count); p++) {
        if (!cycles
                && test_powerlosses[p].short_name != '0'
                && !(case_->flags & TEST_REENTRANT)) {
            continue;
        }

        size_t perm_ = 0;
        for (size_t g = 0; g < TEST_GEOMETRY_COUNT; g++) {
            if (test_geometry && strcmp(
                    test_geometries[g].name, test_geometry) != 0) {
                continue;
            }

            for (size_t k = 0; k < case_->permutations; k++) {
                perm_ += 1;

                if (perm != (size_t)-1 && perm_ != perm) {
                    continue;
                }

                perms_ += 1;

                // setup defines
                define_perm(suite, case_, k);
                define_geometry(&test_geometries[g]);

                if (case_->filter && !case_->filter()) {
                    continue;
                }

                filtered_ += 1;
            }
        }
    }

    *perms += perms_;
    *filtered += filtered_;
}


// operations we can do
static void summary(void) {
    printf("%-36s %7s %7s %7s %11s\n",
            "", "flags", "suites", "cases", "perms");
    size_t cases = 0;
    test_flags_t flags = 0;
    size_t perms = 0;
    size_t filtered = 0;

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                count_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].cycles,
                        test_ids[t].cycle_count,
                        &perms, &filtered);
            }

            cases += test_suites[i].case_count;
            flags |= test_suites[i].flags;
        }
    }

    char perm_buf[64];
    sprintf(perm_buf, "%zu/%zu", filtered, perms);
    char flag_buf[64];
    sprintf(flag_buf, "%s%s",
            (flags & TEST_REENTRANT) ? "r" : "",
            (!flags) ? "-" : "");
    printf("%-36s %7s %7zu %7zu %11s\n",
            "TOTAL",
            flag_buf,
            TEST_SUITE_COUNT,
            cases,
            perm_buf);
}

static void list_suites(void) {
    printf("%-36s %7s %7s %11s\n", "suite", "flags", "cases", "perms");

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            define_suite(&test_suites[i]);

            size_t perms = 0;
            size_t filtered = 0;

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                count_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].cycles,
                        test_ids[t].cycle_count,
                        &perms, &filtered);
            }

            char perm_buf[64];
            sprintf(perm_buf, "%zu/%zu", filtered, perms);
            char flag_buf[64];
            sprintf(flag_buf, "%s%s",
                    (test_suites[i].flags & TEST_REENTRANT) ? "r" : "",
                    (!test_suites[i].flags) ? "-" : "");
            printf("%-36s %7s %7zu %11s\n",
                    test_suites[i].id,
                    flag_buf,
                    test_suites[i].case_count,
                    perm_buf);
        }
    }
}

static void list_cases(void) {
    printf("%-36s %7s %11s\n", "case", "flags", "perms");

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                size_t perms = 0;
                size_t filtered = 0;

                count_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].cycles,
                        test_ids[t].cycle_count,
                        &perms, &filtered);

                char perm_buf[64];
                sprintf(perm_buf, "%zu/%zu", filtered, perms);
                char flag_buf[64];
                sprintf(flag_buf, "%s%s",
                        (test_suites[i].cases[j].flags & TEST_REENTRANT)
                            ? "r" : "",
                        (!test_suites[i].cases[j].flags)
                            ? "-" : "");
                printf("%-36s %7s %11s\n",
                        test_suites[i].cases[j].id,
                        flag_buf,
                        perm_buf);
            }
        }
    }
}

static void list_paths(void) {
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                printf("%-36s %-36s\n",
                        test_suites[i].cases[j].id,
                        test_suites[i].cases[j].path);
            }
        }
    }
}

static void list_defines(void) {
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                for (size_t p = 0;
                        p < (test_ids[t].cycles ? 1 : test_powerloss_count);
                        p++) {
                    if (!test_ids[t].cycles
                            && test_powerlosses[p].short_name != '0'
                            && !(test_suites[i].cases[j].flags
                                & TEST_REENTRANT)) {
                        continue;
                    }

                    size_t perm_ = 0;
                    for (size_t g = 0; g < TEST_GEOMETRY_COUNT; g++) {
                        if (test_geometry && strcmp(
                                test_geometries[g].name, test_geometry) != 0) {
                            continue;
                        }

                        for (size_t k = 0;
                                k < test_suites[i].cases[j].permutations;
                                k++) {
                            perm_ += 1;

                            if (test_ids[t].perm != (size_t)-1
                                    && perm_ != test_ids[t].perm) {
                                continue;
                            }

                            // setup defines
                            define_perm(&test_suites[i],
                                    &test_suites[i].cases[j],
                                    k);
                            define_geometry(&test_geometries[g]);

                            // print the case
                            char id_buf[256];
                            sprintf(id_buf, "%s#%zu",
                                    test_suites[i].cases[j].id, perm_);
                            printf("%-36s ", id_buf);

                            // special case for the current geometry
                            printf("GEOMETRY=%s ", test_geometries[g].name);

                            // print each define
                            for (size_t l = 0;
                                    l < test_suites[i].define_count;
                                    l++) {
                                if (test_suites[i].cases[j].defines
                                        && test_suites[i].cases[j]
                                            .defines[k][l]) {
                                    printf("%s=%jd ",
                                            test_suites[i].define_names[l],
                                            test_define(l));
                                }
                            }
                            printf("\n");
                        }
                    }
                }
            }
        }
    }
}

static void list_geometries(void) {
    for (size_t i = 0; i < TEST_GEOMETRY_COUNT; i++) {
        if (test_geometry && strcmp(
                test_geometries[i].name,
                test_geometry) != 0) {
            continue;
        }

        define_geometry(&test_geometries[i]);

        printf("%-36s ", test_geometries[i].name);
        // print each define
        for (size_t k = 0; k < TEST_GEOMETRY_DEFINE_COUNT; k++) {
            printf("%s=%jd ",
                    test_predefine_names[k],
                    test_predefine(k));
        }
        printf("\n");

    }
}

static void list_defaults(void) {
    printf("%-36s ", "defaults");
    // print each define
    for (size_t k = 0; k < TEST_DEFAULT_DEFINE_COUNT; k++) {
        printf("%s=%jd ",
                test_predefine_names[k+TEST_GEOMETRY_DEFINE_COUNT],
                test_predefine(k+TEST_GEOMETRY_DEFINE_COUNT));
    }
    printf("\n");
}



// scenarios to run tests under power-loss

static void run_powerloss_none(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    (void)cycles;
    (void)cycle_count;
    (void)suite;

    // create block device and configuration
    lfs_testbd_t bd;

    struct lfs_config cfg = {
        .context            = &bd,
        .read               = lfs_testbd_read,
        .prog               = lfs_testbd_prog,
        .erase              = lfs_testbd_erase,
        .sync               = lfs_testbd_sync,
        .read_size          = READ_SIZE,
        .prog_size          = PROG_SIZE,
        .block_size         = BLOCK_SIZE,
        .block_count        = BLOCK_COUNT,
        .block_cycles       = BLOCK_CYCLES,
        .cache_size         = CACHE_SIZE,
        .lookahead_size     = LOOKAHEAD_SIZE,
    };

    struct lfs_testbd_config bdcfg = {
        .erase_value        = ERASE_VALUE,
        .erase_cycles       = ERASE_CYCLES,
        .badblock_behavior  = BADBLOCK_BEHAVIOR,
        .disk_path          = test_disk,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test
    printf("running %s#%zu\n", case_->id, perm);

    case_->run(&cfg);

    printf("finished %s#%zu\n", case_->id, perm);

    // cleanup
    err = lfs_testbd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

static void powerloss_longjmp(void *c) {
    jmp_buf *powerloss_jmp = c;
    longjmp(*powerloss_jmp, 1);
}

static void run_powerloss_linear(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    (void)cycles;
    (void)cycle_count;
    (void)suite;

    // create block device and configuration
    lfs_testbd_t bd;
    jmp_buf powerloss_jmp;
    volatile lfs_testbd_powercycles_t i = 1;

    struct lfs_config cfg = {
        .context            = &bd,
        .read               = lfs_testbd_read,
        .prog               = lfs_testbd_prog,
        .erase              = lfs_testbd_erase,
        .sync               = lfs_testbd_sync,
        .read_size          = READ_SIZE,
        .prog_size          = PROG_SIZE,
        .block_size         = BLOCK_SIZE,
        .block_count        = BLOCK_COUNT,
        .block_cycles       = BLOCK_CYCLES,
        .cache_size         = CACHE_SIZE,
        .lookahead_size     = LOOKAHEAD_SIZE,
    };

    struct lfs_testbd_config bdcfg = {
        .erase_value        = ERASE_VALUE,
        .erase_cycles       = ERASE_CYCLES,
        .badblock_behavior  = BADBLOCK_BEHAVIOR,
        .disk_path          = test_disk,
        .power_cycles       = i,
        .powerloss_behavior = POWERLOSS_BEHAVIOR,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running %s#%zu\n", case_->id, perm);

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            case_->run(&cfg);
            break;
        }

        // power-loss!
        printf("powerloss %s#%zu#", case_->id, perm);
        for (lfs_testbd_powercycles_t j = 1; j <= i; j++) {
            leb16_print(&j, 1);
        }
        printf("\n");

        i += 1;
        lfs_testbd_setpowercycles(&cfg, i);
    }

    printf("finished %s#%zu\n", case_->id, perm);

    // cleanup
    err = lfs_testbd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

static void run_powerloss_exponential(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    (void)cycles;
    (void)cycle_count;
    (void)suite;

    // create block device and configuration
    lfs_testbd_t bd;
    jmp_buf powerloss_jmp;
    volatile lfs_testbd_powercycles_t i = 1;

    struct lfs_config cfg = {
        .context            = &bd,
        .read               = lfs_testbd_read,
        .prog               = lfs_testbd_prog,
        .erase              = lfs_testbd_erase,
        .sync               = lfs_testbd_sync,
        .read_size          = READ_SIZE,
        .prog_size          = PROG_SIZE,
        .block_size         = BLOCK_SIZE,
        .block_count        = BLOCK_COUNT,
        .block_cycles       = BLOCK_CYCLES,
        .cache_size         = CACHE_SIZE,
        .lookahead_size     = LOOKAHEAD_SIZE,
    };

    struct lfs_testbd_config bdcfg = {
        .erase_value        = ERASE_VALUE,
        .erase_cycles       = ERASE_CYCLES,
        .badblock_behavior  = BADBLOCK_BEHAVIOR,
        .disk_path          = test_disk,
        .power_cycles       = i,
        .powerloss_behavior = POWERLOSS_BEHAVIOR,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running %s#%zu\n", case_->id, perm);

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            case_->run(&cfg);
            break;
        }

        // power-loss!
        printf("powerloss %s#%zu#", case_->id, perm);
        for (lfs_testbd_powercycles_t j = 1; j <= i; j *= 2) {
            leb16_print(&j, 1);
        }
        printf("\n");

        i *= 2;
        lfs_testbd_setpowercycles(&cfg, i);
    }

    printf("finished %s#%zu\n", case_->id, perm);

    // cleanup
    err = lfs_testbd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

static void run_powerloss_cycles(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    (void)suite;

    // create block device and configuration
    lfs_testbd_t bd;
    jmp_buf powerloss_jmp;
    volatile size_t i = 0;

    struct lfs_config cfg = {
        .context            = &bd,
        .read               = lfs_testbd_read,
        .prog               = lfs_testbd_prog,
        .erase              = lfs_testbd_erase,
        .sync               = lfs_testbd_sync,
        .read_size          = READ_SIZE,
        .prog_size          = PROG_SIZE,
        .block_size         = BLOCK_SIZE,
        .block_count        = BLOCK_COUNT,
        .block_cycles       = BLOCK_CYCLES,
        .cache_size         = CACHE_SIZE,
        .lookahead_size     = LOOKAHEAD_SIZE,
    };

    struct lfs_testbd_config bdcfg = {
        .erase_value        = ERASE_VALUE,
        .erase_cycles       = ERASE_CYCLES,
        .badblock_behavior  = BADBLOCK_BEHAVIOR,
        .disk_path          = test_disk,
        .power_cycles       = (i < cycle_count) ? cycles[i] : 0,
        .powerloss_behavior = POWERLOSS_BEHAVIOR,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running %s#%zu\n", case_->id, perm);

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            case_->run(&cfg);
            break;
        }

        // power-loss!
        assert(i <= cycle_count);
        printf("powerloss %s#%zu#", case_->id, perm);
        leb16_print(cycles, i+1);
        printf("\n");

        i += 1;
        lfs_testbd_setpowercycles(&cfg,
                (i < cycle_count) ? cycles[i] : 0);
    }

    printf("finished %s#%zu\n", case_->id, perm);

    // cleanup
    err = lfs_testbd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

//static void run_powerloss_n(void *data,
//
//static void run_powerloss_incremental(void *data,

const test_powerloss_t builtin_powerlosses[] = {
    {'0', "none",        run_powerloss_none,        NULL, 0},
    {'e', "exponential", run_powerloss_exponential, NULL, 0},
    {'l', "linear",      run_powerloss_linear,      NULL, 0},
    //{'x', "exhaustive", run_powerloss_exhaustive}
    {0, NULL, NULL, NULL, 0},
};

const char *const builtin_powerlosses_help[] = {
    "Run with no power-losses.",
    "Run with linearly-decreasing power-losses.",
    "Run with exponentially-decreasing power-losses.",
    //"Run a all permutations of power-losses, this may take a while.",
    "Run a all permutations of n power-losses.",
    "Run a custom comma-separated set of power-losses.",
    "Run a custom leb16-encoded set of power-losses.",
};

static void list_powerlosses(void) {
    printf("%-24s %s\n", "scenario", "description");
    size_t i = 0;
    for (; builtin_powerlosses[i].long_name; i++) {
        printf("%c,%-22s %s\n",
                builtin_powerlosses[i].short_name,
                builtin_powerlosses[i].long_name,
                builtin_powerlosses_help[i]);
    }

    // a couple more options with special parsing
    printf("%-24s %s\n", "1,2,3",   builtin_powerlosses_help[i+0]);
    printf("%-24s %s\n", "{1,2,3}", builtin_powerlosses_help[i+1]);
    printf("%-24s %s\n", "#1248g1", builtin_powerlosses_help[i+2]);
}


// global test step count
static size_t step = 0;

// run the tests
static void run_perms(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    for (size_t p = 0; p < (cycles ? 1 : test_powerloss_count); p++) {
        if (!cycles
                && test_powerlosses[p].short_name != '0'
                && !(case_->flags & TEST_REENTRANT)) {
            continue;
        }

        size_t perm_ = 0;
        for (size_t g = 0; g < TEST_GEOMETRY_COUNT; g++) {
            if (test_geometry && strcmp(
                    test_geometries[g].name, test_geometry) != 0) {
                continue;
            }

            for (size_t k = 0; k < case_->permutations; k++) {
                perm_ += 1;

                if (perm != (size_t)-1 && perm_ != perm) {
                    continue;
                }

                if (!(step >= test_start
                        && step < test_stop
                        && (step-test_start) % test_step == 0)) {
                    step += 1;
                    continue;
                }
                step += 1;

                // setup defines
                define_perm(suite, case_, k);
                define_geometry(&test_geometries[g]);

                // filter?
                if (case_->filter && !case_->filter()) {
                    printf("skipped %s#%zu\n", case_->id, perm_);
                    continue;
                }

                if (cycles) {
                    run_powerloss_cycles(
                            suite, case_, perm_,
                            cycles,
                            cycle_count);
                } else {
                    test_powerlosses[p].run(
                            suite, case_, perm_,
                            test_powerlosses[p].cycles,
                            test_powerlosses[p].cycle_count);
                }
            }
        }
    }
}

static void run(void) {
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                run_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].cycles,
                        test_ids[t].cycle_count);
            }
        }
    }
}



// option handling
enum opt_flags {
    OPT_HELP             = 'h',
    OPT_SUMMARY          = 'Y',
    OPT_LIST_SUITES      = 'l',
    OPT_LIST_CASES       = 'L',
    OPT_LIST_PATHS       = 1,
    OPT_LIST_DEFINES     = 2,
    OPT_LIST_GEOMETRIES  = 3,
    OPT_LIST_DEFAULTS    = 4,
    OPT_LIST_POWERLOSSES = 5,
    OPT_DEFINE           = 'D',
    OPT_GEOMETRY         = 'G',
    OPT_POWERLOSS        = 'p',
    OPT_START            = 6,
    OPT_STEP             = 7,
    OPT_STOP             = 8,
    OPT_DISK             = 'd',
    OPT_TRACE            = 't',
};

const char *short_opts = "hYlLD:G:p:nrVd:t:";

const struct option long_opts[] = {
    {"help",             no_argument,       NULL, OPT_HELP},
    {"summary",          no_argument,       NULL, OPT_SUMMARY},
    {"list-suites",      no_argument,       NULL, OPT_LIST_SUITES},
    {"list-cases",       no_argument,       NULL, OPT_LIST_CASES},
    {"list-paths",       no_argument,       NULL, OPT_LIST_PATHS},
    {"list-defines",     no_argument,       NULL, OPT_LIST_DEFINES},
    {"list-geometries",  no_argument,       NULL, OPT_LIST_GEOMETRIES},
    {"list-defaults",    no_argument,       NULL, OPT_LIST_DEFAULTS},
    {"list-powerlosses", no_argument,       NULL, OPT_LIST_POWERLOSSES},
    {"define",           required_argument, NULL, OPT_DEFINE},
    {"geometry",         required_argument, NULL, OPT_GEOMETRY},
    {"powerloss",        required_argument, NULL, OPT_POWERLOSS},
    {"start",            required_argument, NULL, OPT_START},
    {"stop",             required_argument, NULL, OPT_STOP},
    {"step",             required_argument, NULL, OPT_STEP},
    {"disk",             required_argument, NULL, OPT_DISK},
    {"trace",            required_argument, NULL, OPT_TRACE},
    {NULL, 0, NULL, 0},
};

const char *const help_text[] = {
    "Show this help message.",
    "Show quick summary.",
    "List test suites.",
    "List test cases.",
    "List the path for each test case.",
    "List the defines for each test permutation.",
    "List the disk geometries used for testing.",
    "List the default defines in this test-runner.",
    "List the available power-loss scenarios.",
    "Override a test define.",
    "Filter by geometry.",
    "Comma-separated list of power-loss scenarios to test. Defaults to 0,l.",
    "Start at the nth test.",
    "Stop before the nth test.",
    "Only run every n tests, calculated after --start and --stop.",
    "Redirect block device operations to this file.",
    "Redirect trace output to this file.",
};

int main(int argc, char **argv) {
    void (*op)(void) = run;

    const char **override_names = NULL;
    intmax_t *override_defines = NULL;
    size_t override_count = 0;
    size_t override_capacity = 0;

    size_t test_powerloss_capacity = 0;
    size_t test_id_capacity = 0;

    // parse options
    while (true) {
        int c = getopt_long(argc, argv, short_opts, long_opts, NULL);
        switch (c) {
            // generate help message
            case OPT_HELP: {
                printf("usage: %s [options] [test_id]\n", argv[0]);
                printf("\n");

                printf("options:\n");
                size_t i = 0;
                while (long_opts[i].name) {
                    size_t indent;
                    if (long_opts[i].has_arg == no_argument) {
                        if (long_opts[i].val >= '0' && long_opts[i].val < 'z') {
                            indent = printf("  -%c, --%s ",
                                    long_opts[i].val,
                                    long_opts[i].name);
                        } else {
                            indent = printf("  --%s ",
                                    long_opts[i].name);
                        }
                    } else {
                        if (long_opts[i].val >= '0' && long_opts[i].val < 'z') {
                            indent = printf("  -%c %s, --%s %s ",
                                    long_opts[i].val,
                                    long_opts[i].name,
                                    long_opts[i].name,
                                    long_opts[i].name);
                        } else {
                            indent = printf("  --%s %s ",
                                    long_opts[i].name,
                                    long_opts[i].name);
                        }
                    }

                    // a quick, hacky, byte-level method for text wrapping
                    size_t len = strlen(help_text[i]);
                    size_t j = 0;
                    if (indent < 24) {
                        printf("%*s %.80s\n",
                                (int)(24-1-indent),
                                "",
                                &help_text[i][j]);
                        j += 80;
                    } else {
                        printf("\n");
                    }

                    while (j < len) {
                        printf("%24s%.80s\n", "", &help_text[i][j]);
                        j += 80;
                    }

                    i += 1;
                }

                printf("\n");
                exit(0);
            }
            // summary/list flags
            case OPT_SUMMARY:
                op = summary;
                break;
            case OPT_LIST_SUITES:
                op = list_suites;
                break;
            case OPT_LIST_CASES:
                op = list_cases;
                break;
            case OPT_LIST_PATHS:
                op = list_paths;
                break;
            case OPT_LIST_DEFINES:
                op = list_defines;
                break;
            case OPT_LIST_GEOMETRIES:
                op = list_geometries;
                break;
            case OPT_LIST_DEFAULTS:
                op = list_defaults;
                break;
            case OPT_LIST_POWERLOSSES:
                op = list_powerlosses;
                break;
            // configuration
            case OPT_DEFINE: {
                // special case for -DGEOMETRY=<name>, we treat this the same
                // as --geometry=<name>
                if (strncmp(optarg, "GEOMETRY=", strlen("GEOMETRY=")) == 0) {
                    test_geometry = &optarg[strlen("GEOMETRY=")];
                    break;
                }

                // realloc if necessary
                override_count += 1;
                if (override_count > override_capacity) {
                    override_capacity = (2*override_capacity > 4)
                            ? 2*override_capacity
                            : 4;
                    override_names = realloc(override_names,
                            override_capacity * sizeof(const char *));
                    override_defines = realloc(override_defines,
                            override_capacity * sizeof(intmax_t));
                }

                // parse into string key/intmax_t value, cannibalizing the
                // arg in the process
                char *sep = strchr(optarg, '=');
                char *parsed = NULL;
                if (!sep) {
                    goto invalid_define;
                }
                override_defines[override_count-1]
                        = strtoumax(sep+1, &parsed, 0);
                if (parsed == sep+1) {
                    goto invalid_define;
                }

                override_names[override_count-1] = optarg;
                *sep = '\0';
                break;

invalid_define:
                fprintf(stderr, "error: invalid define: %s\n", optarg);
                exit(-1);
            }
            case OPT_GEOMETRY:
                test_geometry = optarg;
                break;
            case OPT_POWERLOSS: {
                // reset our powerloss scenarios
                if (test_powerloss_capacity > 0) {
                    free((test_powerloss_t*)test_powerlosses);
                }
                test_powerlosses = NULL;
                test_powerloss_count = 0;
                test_powerloss_capacity = 0;

                // parse the comma separated list of power-loss scenarios
                while (*optarg) {
                    // allocate space
                    test_powerloss_count += 1;
                    if (test_powerloss_count > test_powerloss_capacity) {
                        test_powerloss_capacity
                                = (2*test_powerloss_capacity > 4)
                                ? 2*test_powerloss_capacity
                                : 4;
                        test_powerlosses = realloc(
                                (test_powerloss_t*)test_powerlosses,
                                test_powerloss_capacity
                                * sizeof(test_powerloss_t));
                    }

                    // parse the power-loss scenario
                    optarg += strspn(optarg, " ");

                    // named power-loss scenario
                    size_t len = strcspn(optarg, " ,");
                    for (size_t i = 0; builtin_powerlosses[i].long_name; i++) {
                        if ((len == 1
                                && *optarg == builtin_powerlosses[i].short_name)
                                || (len == strlen(
                                        builtin_powerlosses[i].long_name)
                                    && memcmp(optarg,
                                        builtin_powerlosses[i].long_name,
                                        len) == 0))  {
                            ((test_powerloss_t*)test_powerlosses)[
                                    test_powerloss_count-1]
                                    = builtin_powerlosses[i];
                            optarg += len;
                            goto powerloss_next;
                        }
                    }

                    // exhaustive permutations
                    // TODO

                    // comma-separated permutation
                    if (*optarg == '{') {
                        // how many cycles?
                        size_t count = 1;
                        for (size_t i = 0; optarg[i]; i++) {
                            if (optarg[i] == ',') {
                                count += 1;
                            }
                        }

                        // parse cycles
                        lfs_testbd_powercycles_t *cycles = malloc(
                                count * sizeof(lfs_testbd_powercycles_t));
                        size_t i = 0;
                        char *s = optarg + 1;
                        while (true) {
                            char *parsed = NULL;
                            cycles[i] = strtoumax(s, &parsed, 0);
                            if (parsed == s) {
                                count -= 1;
                                i -= 1;
                            }
                            i += 1;

                            s = parsed + strspn(parsed, " ");
                            if (*s == ',') {
                                s += 1;
                                continue;
                            } else if (*s == '}') {
                                s += 1;
                                break;
                            } else {
                                goto powerloss_unknown;
                            }
                        }

                        ((test_powerloss_t*)test_powerlosses)[
                                test_powerloss_count-1] = (test_powerloss_t){
                            .run = run_powerloss_cycles,
                            .cycles = cycles,
                            .cycle_count = count,
                        };
                        optarg = s;
                        goto powerloss_next;
                    }

                    // leb16-encoded permutation
                    if (*optarg == '#') {
                        lfs_testbd_powercycles_t *cycles;
                        char *parsed = NULL;
                        size_t count = leb16_parse(optarg+1, &parsed, &cycles);
                        if (parsed == optarg+1) {
                            goto powerloss_unknown;
                        }

                        ((test_powerloss_t*)test_powerlosses)[
                                test_powerloss_count-1] = (test_powerloss_t){
                            .run = run_powerloss_cycles,
                            .cycles = cycles,
                            .cycle_count = count,
                        };
                        optarg = (char*)parsed;
                        goto powerloss_next;
                    }

powerloss_unknown:
                    // unknown scenario?
                    fprintf(stderr, "error: "
                            "unknown power-loss scenario: %s\n",
                            optarg);
                    exit(-1);

powerloss_next:
                    optarg += strcspn(optarg, ",");
                    if (*optarg == ',') {
                        optarg += 1;
                    }
                }
                break;
            }
            case OPT_START: {
                char *parsed = NULL;
                test_start = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid skip: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            case OPT_STOP: {
                char *parsed = NULL;
                test_stop = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid count: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            case OPT_STEP: {
                char *parsed = NULL;
                test_step = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid every: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            case OPT_DISK:
                test_disk = optarg;
                break;
            case OPT_TRACE:
                if (strcmp(optarg, "-") == 0) {
                    test_trace = stdout;
                } else {
                    test_trace = fopen(optarg, "w");
                    if (!test_trace) {
                        fprintf(stderr, "error: could not open for trace: %d\n",
                                -errno);
                        exit(-1);
                    }
                }
                break;
            // done parsing
            case -1:
                goto getopt_done;
            // unknown arg, getopt prints a message for us
            default:
                exit(-1);
        }
    }
getopt_done: ;

    if (argc > optind) {
        // reset our test identifier list
        test_ids = NULL;
        test_id_count = 0;
        test_id_capacity = 0;
    }

    // parse test identifier, if any, cannibalizing the arg in the process
    for (; argc > optind; optind++) {
        // parse suite
        char *suite = argv[optind];
        char *case_ = strchr(suite, '#');
        size_t perm = -1;
        lfs_testbd_powercycles_t *cycles = NULL;
        size_t cycle_count = 0;

        if (case_) {
            *case_ = '\0';
            case_ += 1;

            // parse case
            char *perm_ = strchr(case_, '#');
            if (perm_) {
                *perm_ = '\0';
                perm_ += 1;

                // parse power cycles
                char *cycles_ = strchr(perm_, '#');
                if (cycles_) {
                    *cycles_ = '\0';
                    cycles_ += 1;

                    char *parsed = NULL;
                    cycle_count = leb16_parse(cycles_, &parsed, &cycles);
                    if (parsed == cycles_) {
                        fprintf(stderr, "error: "
                                "could not parse test cycles: %s\n", cycles_);
                        exit(-1);
                    }
                }

                char *parsed = NULL;
                perm = strtoumax(perm_, &parsed, 10);
                if (parsed == perm_) {
                    fprintf(stderr, "error: "
                            "could not parse test permutation: %s\n", perm_);
                    exit(-1);
                }
            }
        }

        // remove optional path and .toml suffix
        char *slash = strrchr(suite, '/');
        if (slash) {
            suite = slash+1;
        }

        size_t suite_len = strlen(suite);
        if (suite_len > 5 && strcmp(&suite[suite_len-5], ".toml") == 0) {
            suite[suite_len-5] = '\0';
        }

        // append to identifier list
        test_id_count += 1;
        if (test_id_count > test_id_capacity) {
            test_id_capacity = (2*test_id_capacity > 4)
                    ? 2*test_id_capacity
                    : 4;
            test_ids = realloc((test_id_t*)test_ids,
                    test_id_capacity * sizeof(test_id_t));
        }
        ((test_id_t*)test_ids)[test_id_count-1] = (test_id_t){
            .suite = suite,
            .case_ = case_,
            .perm = perm,
            .cycles = cycles,
            .cycle_count = cycle_count,
        };
    }

    // register overrides
    test_define_overrides(override_names, override_defines, override_count);

    // do the thing
    op();

    // cleanup (need to be done for valgrind testing)
    free(override_names);
    free(override_defines);
    if (test_powerloss_capacity) {
        for (size_t i = 0; i < test_powerloss_count; i++) {
            free((lfs_testbd_powercycles_t*)test_powerlosses[i].cycles);
        }
        free((test_powerloss_t*)test_powerlosses);
    }
    if (test_id_capacity) {
        for (size_t i = 0; i < test_id_count; i++) {
            free((lfs_testbd_powercycles_t*)test_ids[i].cycles);
        }
        free((test_id_t*)test_ids);
    }
}
