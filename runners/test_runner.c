
#include "runners/test_runner.h"
#include "bd/lfs_testbd.h"

#include <getopt.h>
#include <sys/types.h>
#include <errno.h>


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

static void test_define_geometry(const struct test_geometry *geometry) {
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

static void test_define_suite(const struct test_suite *suite) {
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

static void test_define_perm(
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


// other miscellany
static const char *test_suite = NULL;
static const char *test_case = NULL;
static size_t test_perm = -1;
static const char *test_geometry = NULL;
static test_types_t test_types = 0;
static size_t test_start = 0;
static size_t test_stop = -1;
static size_t test_step = 1;

static const char *test_disk = NULL;
FILE *test_trace = NULL;

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
    size_t geom_perm = perm % TEST_GEOMETRY_COUNT;
    return (test_perm != (size_t)-1 && perm != test_perm)
            || (test_geometry && (strcmp(
                test_geometries[geom_perm].name,
                test_geometry) != 0));
}

static bool test_step_skip(size_t step) {
    return !(step >= test_start
            && step < test_stop
            && (step-test_start) % test_step == 0);
}

static void test_case_permcount(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t *perms,
        size_t *filtered) {
    size_t perms_ = 0;
    size_t filtered_ = 0;

    for (size_t perm = 0;
            perm < TEST_GEOMETRY_COUNT
                * case_->permutations;
            perm++) {
        if (test_perm_skip(perm)) {
            continue;
        }

        perms_ += 1;

        // setup defines
        size_t case_perm = perm / TEST_GEOMETRY_COUNT;
        size_t geom_perm = perm % TEST_GEOMETRY_COUNT;
        test_define_perm(suite, case_, case_perm);
        test_define_geometry(&test_geometries[geom_perm]);

        if (case_->filter) {
            if (!case_->filter()) {
                continue;
            }
        }

        filtered_ += 1;
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

        test_define_suite(test_suites[i]);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            test_case_permcount(test_suites[i], test_suites[i]->cases[j],
                    &perms, &filtered);
        }

        cases += test_suites[i]->case_count;
        types |= test_suites[i]->types;
    }

    char perm_buf[64];
    sprintf(perm_buf, "%zu/%zu", filtered, perms);
    char type_buf[64];
    sprintf(type_buf, "%s%s",
            (types & TEST_NORMAL)    ? "n" : "",
            (types & TEST_REENTRANT) ? "r" : "");
    printf("%-36s %7s %7zu %7zu %11s\n",
            "TOTAL",
            type_buf,
            test_suite_count,
            cases,
            perm_buf);
}

static void list_suites(void) {
    printf("%-36s %7s %7s %11s\n", "suite", "types", "cases", "perms");
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_suite(test_suites[i]);

        size_t perms = 0;
        size_t filtered = 0;
        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            test_case_permcount(test_suites[i], test_suites[i]->cases[j],
                    &perms, &filtered);
        }

        char perm_buf[64];
        sprintf(perm_buf, "%zu/%zu", filtered, perms);
        char type_buf[64];
        sprintf(type_buf, "%s%s",
                (test_suites[i]->types & TEST_NORMAL)    ? "n" : "",
                (test_suites[i]->types & TEST_REENTRANT) ? "r" : "");
        printf("%-36s %7s %7zu %11s\n",
                test_suites[i]->id,
                type_buf,
                test_suites[i]->case_count,
                perm_buf);
    }
}

static void list_cases(void) {
    printf("%-36s %7s %11s\n", "case", "types", "perms");
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_suite(test_suites[i]);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            size_t perms = 0;
            size_t filtered = 0;
            test_case_permcount(test_suites[i], test_suites[i]->cases[j],
                    &perms, &filtered);
            test_types_t types = test_suites[i]->cases[j]->types;

            char perm_buf[64];
            sprintf(perm_buf, "%zu/%zu", filtered, perms);
            char type_buf[64];
            sprintf(type_buf, "%s%s",
                    (types & TEST_NORMAL)    ? "n" : "",
                    (types & TEST_REENTRANT) ? "r" : "");
            printf("%-36s %7s %11s\n",
                    test_suites[i]->cases[j]->id,
                    type_buf,
                    perm_buf);
        }
    }
}

static void list_paths(void) {
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
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_suite(test_suites[i]);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            for (size_t perm = 0;
                    perm < TEST_GEOMETRY_COUNT
                        * test_suites[i]->cases[j]->permutations;
                    perm++) {
                if (test_perm_skip(perm)) {
                    continue;
                }

                // setup defines
                size_t case_perm = perm / TEST_GEOMETRY_COUNT;
                size_t geom_perm = perm % TEST_GEOMETRY_COUNT;
                test_define_perm(test_suites[i],
                        test_suites[i]->cases[j], case_perm);
                test_define_geometry(&test_geometries[geom_perm]);

                // print the case
                char id_buf[256];
                sprintf(id_buf, "%s#%zu", test_suites[i]->cases[j]->id, perm);
                printf("%-36s ", id_buf);

                // special case for the current geometry
                printf("GEOMETRY=%s ", test_geometries[geom_perm].name);

                // print each define
                for (size_t k = 0; k < test_suites[i]->define_count; k++) {
                    if (test_suites[i]->cases[j]->defines
                            && test_suites[i]->cases[j]
                                ->defines[case_perm][k]) {
                        printf("%s=%jd ",
                                test_suites[i]->define_names[k],
                                test_define(k));
                    }
                }
                printf("\n");
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

        test_define_geometry(&test_geometries[i]);

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

static void run(void) {
    size_t step = 0;
    for (size_t i = 0; i < test_suite_count; i++) {
        if (test_suite_skip(test_suites[i])) {
            continue;
        }

        test_define_suite(test_suites[i]);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            if (test_case_skip(test_suites[i]->cases[j])) {
                continue;
            }

            for (size_t perm = 0;
                    perm < TEST_GEOMETRY_COUNT
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
                size_t case_perm = perm / TEST_GEOMETRY_COUNT;
                size_t geom_perm = perm % TEST_GEOMETRY_COUNT;
                test_define_perm(test_suites[i],
                        test_suites[i]->cases[j], case_perm);
                test_define_geometry(&test_geometries[geom_perm]);

                // filter?
                if (test_suites[i]->cases[j]->filter) {
                    if (!test_suites[i]->cases[j]->filter()) {
                        printf("skipped %s#%zu\n",
                                test_suites[i]->cases[j]->id,
                                perm);
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

                int err = lfs_testbd_createcfg(&cfg, test_disk, &bdcfg);
                if (err) {
                    fprintf(stderr, "error: "
                            "could not create block device: %d\n", err);
                    exit(-1);
                }

                // run the test
                printf("running %s#%zu\n", test_suites[i]->cases[j]->id, perm);

                test_suites[i]->cases[j]->run(&cfg);

                printf("finished %s#%zu\n", test_suites[i]->cases[j]->id, perm);

                // cleanup
                err = lfs_testbd_destroy(&cfg);
                if (err) {
                    fprintf(stderr, "error: "
                            "could not destroy block device: %d\n", err);
                    exit(-1);
                }
            }
        }
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
    OPT_LIST_DEFAULTS   = 4,
    OPT_DEFINE          = 'D',
    OPT_GEOMETRY        = 'G',
    OPT_NORMAL          = 'n',
    OPT_REENTRANT       = 'r',
    OPT_START           = 5,
    OPT_STEP            = 6,
    OPT_STOP            = 7,
    OPT_DISK            = 'd',
    OPT_TRACE           = 't',
};

const char *short_opts = "hYlLD:G:nrVp:t:";

const struct option long_opts[] = {
    {"help",            no_argument,       NULL, OPT_HELP},
    {"summary",         no_argument,       NULL, OPT_SUMMARY},
    {"list-suites",     no_argument,       NULL, OPT_LIST_SUITES},
    {"list-cases",      no_argument,       NULL, OPT_LIST_CASES},
    {"list-paths",      no_argument,       NULL, OPT_LIST_PATHS},
    {"list-defines",    no_argument,       NULL, OPT_LIST_DEFINES},
    {"list-geometries", no_argument,       NULL, OPT_LIST_GEOMETRIES},
    {"list-defaults",   no_argument,       NULL, OPT_LIST_DEFAULTS},
    {"define",          required_argument, NULL, OPT_DEFINE},
    {"geometry",        required_argument, NULL, OPT_GEOMETRY},
    {"normal",          no_argument,       NULL, OPT_NORMAL},
    {"reentrant",       no_argument,       NULL, OPT_REENTRANT},
    {"start",           required_argument, NULL, OPT_START},
    {"stop",            required_argument, NULL, OPT_STOP},
    {"step",            required_argument, NULL, OPT_STEP},
    {"disk",            required_argument, NULL, OPT_DISK},
    {"trace",           required_argument, NULL, OPT_TRACE},
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
    "Override a test define.",
    "Filter by geometry.",
    "Filter for normal tests. Can be combined.",
    "Filter for reentrant tests. Can be combined.",
    "Start at the nth test.",
    "Stop before the nth test.",
    "Only run every n tests, calculated after --start and --stop.",
    "Use this file as the disk.",
    "Redirect trace output to this file.",
};

int main(int argc, char **argv) {
    void (*op)(void) = run;

    static const char **override_names = NULL;
    static intmax_t *override_defines = NULL;
    static size_t override_count = 0;
    static size_t override_cap = 0;

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
                if (override_count > override_cap) {
                    override_cap = (2*override_cap > 4) ? 2*override_cap : 4;
                    override_names = realloc(override_names, override_cap
                            * sizeof(const char *));
                    override_defines = realloc(override_defines, override_cap
                            * sizeof(intmax_t));
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
            case OPT_NORMAL:
                test_types |= TEST_NORMAL;
                break;
            case OPT_REENTRANT:
                test_types |= TEST_REENTRANT;
                break;
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

    // register overrides
    test_define_overrides(override_names, override_defines, override_count);

    // do the thing
    op();

    // cleanup (need to be done for valgrind testing)
    free(override_names);
    free(override_defines);
}
