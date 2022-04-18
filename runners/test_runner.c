
#include "runners/test_runner.h"
#include "bd/lfs_testbd.h"

#include <getopt.h>
#include <sys/types.h>

// disk geometries
struct test_geometry {
    const char *name;
    const test_define_t *defines;
};

// Note this includes the default configuration for test pre-defines
#define TEST_GEOMETRY(name, read, prog, erase, count) \
    {name, (const test_define_t[]){ \
        /* READ_SIZE */         read, \
        /* PROG_SIZE */         prog, \
        /* BLOCK_SIZE */        erase, \
        /* BLOCK_COUNT */       count, \
        /* BLOCK_CYCLES */      -1, \
        /* CACHE_SIZE */        (64 % (prog) == 0) ? 64 : (prog), \
        /* LOOKAHEAD_SIZE */    16, \
        /* ERASE_VALUE */       0xff, \
        /* ERASE_CYCLES */      0, \
        /* BADBLOCK_BEHAVIOR */ LFS_TESTBD_BADBLOCK_PROGERROR, \
    }}

const struct test_geometry test_geometries[] = {
    // Made up geometry that works well for testing
    TEST_GEOMETRY("small",    16,   16,     512, (1024*1024)/512),
    TEST_GEOMETRY("medium",   16,   16,    4096, (1024*1024)/4096),
    TEST_GEOMETRY("big",      16,   16, 32*1024, (1024*1024)/(32*1024)),
    // EEPROM/NVRAM
    TEST_GEOMETRY("eeprom",    1,    1,     512, (1024*1024)/512),
    // SD/eMMC
    TEST_GEOMETRY("emmc",    512,  512,     512, (1024*1024)/512),
    // NOR flash                       
    TEST_GEOMETRY("nor",       1,    1,    4096, (1024*1024)/4096),
    // NAND flash                      
    TEST_GEOMETRY("nand",   4096, 4096, 32*1024, (1024*1024)/(32*1024)),
};

const size_t test_geometry_count = (
        sizeof(test_geometries) / sizeof(test_geometries[0]));


// test define lookup and management
const test_define_t *test_defines[3] = {NULL};
const uint8_t *test_define_maps[2] = {NULL};

test_define_t test_define(size_t define) {
    if (test_define_maps[0] && test_define_maps[0][define] != 0xff) {
        return test_defines[0][test_define_maps[0][define]];
    } else if (test_define_maps[1] && test_define_maps[1][define] != 0xff) {
        return test_defines[1][test_define_maps[1][define]];
    } else {
        return test_defines[2][define];
    }
}

static void test_define_geometry(const struct test_geometry *geometry) {
    if (geometry) {
        test_defines[2] = geometry->defines;
    } else {
        test_defines[2] = NULL;
    }
}

static void test_define_case(const struct test_case *case_, size_t perm) {
    if (case_ && case_->defines) {
        test_defines[1] = case_->defines[perm];
        test_define_maps[1] = case_->define_map;
    } else {
        test_defines[1] = NULL;
        test_define_maps[1] = NULL;
    }
}

static void test_define_overrides(
        const struct test_suite *suite,
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    if (override_names && override_defines && override_count > 0) {
        uint8_t *define_map = malloc(suite->define_count * sizeof(uint8_t));
        memset(define_map, 0xff, suite->define_count * sizeof(bool));

        // lookup each override in the suite defines, they may have a
        // different index in each suite
        for (size_t i = 0; i < override_count; i++) {
            size_t j = 0;
            for (; j < suite->define_count; j++) {
                if (strcmp(override_names[i], suite->define_names[j]) == 0) {
                    break;
                }
            }

            if (j < suite->define_count) {
                define_map[j] = i;
            }
        }

        test_defines[0] = override_defines;
        test_define_maps[0] = define_map;
    } else {
        test_defines[0] = NULL;
        free((uint8_t *)test_define_maps[0]);
        test_define_maps[0] = NULL;
    }
}


// other miscellany
static const char *test_suite = NULL;
static const char *test_case = NULL;
static size_t test_perm = -1;
static const char *test_geometry = NULL;
static test_types_t test_types = 0;
static size_t test_skip = 0;
static size_t test_count = -1;
static size_t test_every = 1;

static const char **override_names = NULL;
static test_define_t *override_defines = NULL;
static size_t override_count = 0;
static size_t override_cap = 0;

// note, these skips are different than filtered tests
static bool test_suite_skip(const struct test_suite *suite) {
    return (test_suite && strcmp(suite->name, test_suite) != 0)
            || (test_types && (suite->types & test_types) == 0);
}

static bool test_case_skip(const struct test_case *case_) {
    return (test_case && strcmp(case_->name, test_case) != 0)
            || (test_types && (case_->types & test_types) == 0);
}

static bool test_perm_skip(size_t perm) {
    size_t geom_perm = perm % test_geometry_count;
    return (test_perm != (size_t)-1 && perm != test_perm)
            || (test_geometry && (strcmp(
                test_geometries[geom_perm].name,
                test_geometry) != 0));
}

static bool test_step_skip(size_t step) {
    return !(step >= test_skip
            && (step-test_skip) < test_count
            && (step-test_skip) % test_every == 0);
}

static void test_case_sumpermutations(
        const struct test_case *case_,
        size_t *perms,
        size_t *filtered) {
    size_t perms_ = 0;
    size_t filtered_ = 0;

    for (size_t perm = 0;
            perm < test_geometry_count
                * case_->permutations;
            perm++) {
        if (test_perm_skip(perm)) {
            continue;
        }

        perms_ += 1;

        // setup defines
        size_t case_perm = perm / test_geometry_count;
        size_t geom_perm = perm % test_geometry_count;
        test_define_geometry(&test_geometries[geom_perm]);
        test_define_case(case_, case_perm);

        if (case_->filter) {
            if (!case_->filter(case_perm)) {
                test_define_geometry(NULL);
                test_define_case(NULL, 0);
                continue;
            }
        }

        filtered_ += 1;

        test_define_geometry(NULL);
        test_define_case(NULL, 0);
    }

    *perms += perms_;
    *filtered += filtered_;
}        


// operations we can do
static void summary(void) {
    printf("%-36s %7s %7s %7s %11s\n",
            "", "types", "suites", "cases", "perms");
    size_t cases = 0;
    test_types_t types = 0;
    size_t perms = 0;
    size_t filtered = 0;
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_overrides(
                test_suites[i],
                override_names, override_defines, override_count);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            test_case_sumpermutations(test_suites[i]->cases[j],
                    &perms, &filtered);
        }

        test_define_overrides(NULL, NULL, NULL, 0);

        cases += test_suites[i]->case_count;
        types |= test_suites[i]->types;
    }

    char perm_buf[64];
    sprintf(perm_buf, "%zu/%zu", filtered, perms);
    char type_buf[64];
    sprintf(type_buf, "%s%s%s",
            (types & TEST_NORMAL)    ? "n" : "",
            (types & TEST_REENTRANT) ? "r" : "",
            (types & TEST_VALGRIND)  ? "V" : "");
    printf("%-36s %7s %7zu %7zu %11s\n",
            "TOTAL",
            type_buf,
            test_suite_count,
            cases,
            perm_buf);
}

static void list_suites(void) {
    printf("%-36s %-12s %7s %7s %11s\n",
            "id", "suite", "types", "cases", "perms");
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_overrides(
                test_suites[i],
                override_names, override_defines, override_count);

        size_t perms = 0;
        size_t filtered = 0;
        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            test_case_sumpermutations(test_suites[i]->cases[j],
                    &perms, &filtered);
        }

        test_define_overrides(NULL, NULL, NULL, 0);

        char perm_buf[64];
        sprintf(perm_buf, "%zu/%zu", filtered, perms);
        char type_buf[64];
        sprintf(type_buf, "%s%s%s",
                (test_suites[i]->types & TEST_NORMAL)    ? "n" : "",
                (test_suites[i]->types & TEST_REENTRANT) ? "r" : "",
                (test_suites[i]->types & TEST_VALGRIND)  ? "V" : "");
        printf("%-36s %-12s %7s %7zu %11s\n",
                test_suites[i]->id,
                test_suites[i]->name,
                type_buf,
                test_suites[i]->case_count,
                perm_buf);
    }
}

static void list_cases(void) {
    printf("%-36s %-12s %-12s %7s %11s\n",
            "id", "suite", "case", "types", "perms");
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_overrides(
                test_suites[i],
                override_names, override_defines, override_count);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            size_t perms = 0;
            size_t filtered = 0;
            test_case_sumpermutations(test_suites[i]->cases[j],
                    &perms, &filtered);
            test_types_t types = test_suites[i]->cases[j]->types;

            char perm_buf[64];
            sprintf(perm_buf, "%zu/%zu", filtered, perms);
            char type_buf[64];
            sprintf(type_buf, "%s%s%s",
                    (types & TEST_NORMAL)    ? "n" : "",
                    (types & TEST_REENTRANT) ? "r" : "",
                    (types & TEST_VALGRIND)  ? "V" : "");
            printf("%-36s %-12s %-12s %7s %11s\n",
                    test_suites[i]->cases[j]->id,
                    test_suites[i]->name,
                    test_suites[i]->cases[j]->name,
                    type_buf,
                    perm_buf);
        }

        test_define_overrides(NULL, NULL, NULL, 0);
    }
}

static void list_paths(void) {
    printf("%-36s %-36s\n", "id", "path");
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            printf("%-36s %-36s\n",
                    test_suites[i]->cases[j]->id,
                    test_suites[i]->cases[j]->path);
        }
    }
}

static void list_defines(void) {
    printf("%-36s %s\n", "id", "defines");
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_overrides(
                test_suites[i],
                override_names, override_defines, override_count);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            for (size_t perm = 0;
                    perm < test_geometry_count
                        * test_suites[i]->cases[j]->permutations;
                    perm++) {
                if (test_perm_skip(perm)) {
                    continue;
                }

                // setup defines
                size_t case_perm = perm / test_geometry_count;
                size_t geom_perm = perm % test_geometry_count;
                test_define_geometry(&test_geometries[geom_perm]);
                test_define_case(test_suites[i]->cases[j], case_perm);

                // print each define
                char id_buf[256];
                sprintf(id_buf, "%s#%zu", test_suites[i]->cases[j]->id, perm);
                printf("%-36s ", id_buf);
                for (size_t k = 0; k < test_suites[i]->define_count; k++) {
                    if (k >= TEST_PREDEFINE_COUNT && (
                            !test_suites[i]->cases[j]->define_map
                            || test_suites[i]->cases[j]->define_map[k]
                                == 0xff)) {
                        continue;
                    }

                    printf("%s=%jd ",
                            test_suites[i]->define_names[k],
                            test_define(k));
                }
                printf("\n");
            }
        }

        test_define_overrides(NULL, NULL, NULL, 0);
    }
}

static void list_geometries(void) {
    printf("%-36s %7s %7s %7s %7s %7s\n",
            "name", "read", "prog", "erase", "count", "size");
    for (size_t i = 0; i < test_geometry_count; i++) {
        if (test_geometry && strcmp(
                test_geometries[i].name,
                test_geometry) != 0) {
            continue;
        }

        test_define_geometry(&test_geometries[i]);

        printf("%-36s %7ju %7ju %7ju %7ju %7ju\n",
                test_geometries[i].name,
                READ_SIZE,
                PROG_SIZE,
                BLOCK_SIZE,
                BLOCK_COUNT,
                BLOCK_SIZE*BLOCK_COUNT);
    }
}

static void run(void) {
    size_t step = 0;
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_overrides(
                test_suites[i],
                override_names, override_defines, override_count);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            for (size_t perm = 0;
                    perm < test_geometry_count
                        * test_suites[i]->cases[j]->permutations;
                    perm++) {
                if (test_perm_skip(perm)) {
                    continue;
                }

                if (test_step_skip(step)) {
                    step += 1;
                    continue;
                }
                step += 1;

                // setup defines
                size_t case_perm = perm / test_geometry_count;
                size_t geom_perm = perm % test_geometry_count;
                test_define_geometry(&test_geometries[geom_perm]);
                test_define_case(test_suites[i]->cases[j], case_perm);

                // filter?
                if (test_suites[i]->cases[j]->filter) {
                    if (!test_suites[i]->cases[j]->filter(case_perm)) {
                        printf("skipped %s#%zu\n",
                                test_suites[i]->cases[j]->id,
                                perm);
                        test_define_geometry(NULL);
                        test_define_case(NULL, 0);
                        continue;
                    }
                }

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
                    .power_cycles       = 0,
                };

                lfs_testbd_createcfg(&cfg, NULL, &bdcfg) => 0;

                // run the test
                printf("running %s#%zu\n", test_suites[i]->cases[j]->id, perm);

                test_suites[i]->cases[j]->run(&cfg, case_perm);

                printf("finished %s#%zu\n", test_suites[i]->cases[j]->id, perm);

                // cleanup
                lfs_testbd_destroy(&cfg) => 0;

                test_define_geometry(NULL);
                test_define_case(NULL, 0);
            }
        }

        test_define_overrides(NULL, NULL, NULL, 0);
    }
}




// option handling
enum opt_flags {
    OPT_HELP            = 'h',
    OPT_SUMMARY         = 'Y',
    OPT_LIST_SUITES     = 'l',
    OPT_LIST_CASES      = 'L',
    OPT_LIST_PATHS      = 1,
    OPT_LIST_DEFINES    = 2,
    OPT_LIST_GEOMETRIES = 3,
    OPT_DEFINE          = 'D',
    OPT_GEOMETRY        = 'G',
    OPT_NORMAL          = 'n',
    OPT_REENTRANT       = 'r',
    OPT_VALGRIND        = 'V',
    OPT_SKIP            = 4,
    OPT_COUNT           = 5,
    OPT_EVERY           = 6,
};

const char *short_opts = "hYlLD:G:nrV";

const struct option long_opts[] = {
    {"help",            no_argument,       NULL, OPT_HELP},
    {"summary",         no_argument,       NULL, OPT_SUMMARY},
    {"list-suites",     no_argument,       NULL, OPT_LIST_SUITES},
    {"list-cases",      no_argument,       NULL, OPT_LIST_CASES},
    {"list-paths",      no_argument,       NULL, OPT_LIST_PATHS},
    {"list-defines",    no_argument,       NULL, OPT_LIST_DEFINES},
    {"list-geometries", no_argument,       NULL, OPT_LIST_GEOMETRIES},
    {"define",          required_argument, NULL, OPT_DEFINE},
    {"geometry",        required_argument, NULL, OPT_GEOMETRY},
    {"normal",          no_argument,       NULL, OPT_NORMAL},
    {"reentrant",       no_argument,       NULL, OPT_REENTRANT},
    {"valgrind",        no_argument,       NULL, OPT_VALGRIND},
    {"skip",            required_argument, NULL, OPT_SKIP},
    {"count",           required_argument, NULL, OPT_COUNT},
    {"every",           required_argument, NULL, OPT_EVERY},
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
    "Override a test define.",
    "Filter by geometry.",
    "Filter for normal tests. Can be combined.",
    "Filter for reentrant tests. Can be combined.",
    "Filter for valgrind tests. Can be combined.",
    "Skip the first n tests.",
    "Stop after n tests.",
    "Only run every n tests, calculated after --skip and --stop.",
};

int main(int argc, char **argv) {
    void (*op)(void) = run;

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
            // configuration
            case OPT_DEFINE: {
                // realloc if necessary
                override_count += 1;
                if (override_count > override_cap) {
                    override_cap = (2*override_cap > 4) ? 2*override_cap : 4;
                    override_names = realloc(override_names, override_cap
                            * sizeof(const char *));
                    override_defines = realloc(override_defines, override_cap
                            * sizeof(test_define_t));
                }

                // parse into string key/test_define_t value, cannibalizing the
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
            case OPT_NORMAL:
                test_types |= TEST_NORMAL;
                break;
            case OPT_REENTRANT:
                test_types |= TEST_REENTRANT;
                break;
            case OPT_VALGRIND:
                test_types |= TEST_VALGRIND;
                break;
            case OPT_SKIP: {
                char *parsed = NULL;
                test_skip = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid skip: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            case OPT_COUNT: {
                char *parsed = NULL;
                test_count = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid count: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            case OPT_EVERY: {
                char *parsed = NULL;
                test_every = strtoumax(optarg, &parsed, 0);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid every: %s\n", optarg);
                    exit(-1);
                }
                break;
            }
            // done parsing
            case -1:
                goto getopt_done;
            // unknown arg, getopt prints a message for us
            default:
                exit(-1);
        }
    }
getopt_done: ;

    // parse test identifier, if any, cannibalizing the arg in the process
    if (argc > optind) {
        if (argc - optind > 1) {
            fprintf(stderr, "error: more than one test identifier\n");
            exit(-1);
        }

        // parse suite
        char *suite = argv[optind];
        char *case_ = strchr(suite, '#');

        if (case_) {
            *case_ = '\0';
            case_ += 1;

            // parse case
            char *perm = strchr(case_, '#');
            if (perm) {
                *perm = '\0';
                perm += 1;

                char *parsed = NULL;
                test_perm = strtoumax(perm, &parsed, 10);
                if (parsed == perm) {
                    fprintf(stderr, "error: could not parse test identifier\n");
                    exit(-1);
                }
            }

            test_case = case_;
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

        test_suite = suite;
    }

    // do the thing
    op();

    // cleanup (need to be done for valgrind testing)
    free(override_names);
    free(override_defines);
}
