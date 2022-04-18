
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

void test_define_geometry(const struct test_geometry *geometry) {
    if (geometry) {
        test_defines[2] = geometry->defines;
    } else {
        test_defines[2] = NULL;
    }
}

void test_define_case(const struct test_case *case_, size_t perm) {
    if (case_ && case_->defines) {
        test_defines[1] = case_->defines[perm];
        test_define_maps[1] = case_->define_map;
    } else {
        test_defines[1] = NULL;
        test_define_maps[1] = NULL;
    }
}

void test_define_overrides(
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


// operations we can do
void summary(
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    (void)override_names;
    (void)override_defines;
    (void)override_count;
    printf("%-36s %7s %7s %7s %7s\n",
            "", "geoms", "suites", "cases", "perms");
    size_t cases = 0;
    size_t perms = 0;
    for (size_t i = 0; i < test_suite_count; i++) {
        cases += test_suites[i]->case_count;

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            perms += test_suites[i]->cases[j]->permutations;
        }
    }

    printf("%-36s %7zu %7zu %7zu %7zu\n",
            "TOTAL",
            test_geometry_count,
            test_suite_count,
            cases,
            test_geometry_count*perms);
}

void list_suites(
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    (void)override_names;
    (void)override_defines;
    (void)override_count;
    printf("%-36s %-12s %7s %7s %7s\n",
            "id", "suite", "types", "cases", "perms");
    for (size_t i = 0; i < test_suite_count; i++) {
        size_t perms = 0;
        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            perms += test_suites[i]->cases[j]->permutations;
        }

        printf("%-36s %-12s %7s %7zu %7zu\n",
                test_suites[i]->id,
                test_suites[i]->name,
                "n", // TODO
                test_suites[i]->case_count,
                test_geometry_count*perms);
    }
}

void list_cases(
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    (void)override_names;
    (void)override_defines;
    (void)override_count;
    printf("%-36s %-12s %-12s %7s %7s\n",
            "id", "suite", "case", "types", "perms");
    for (size_t i = 0; i < test_suite_count; i++) {
        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            printf("%-36s %-12s %-12s %7s %7zu\n",
                    test_suites[i]->cases[j]->id,
                    test_suites[i]->name,
                    test_suites[i]->cases[j]->name,
                    "n", // TODO
                    test_geometry_count
                        * test_suites[i]->cases[j]->permutations);
        }
    }
}

void list_paths(
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    (void)override_names;
    (void)override_defines;
    (void)override_count;
    printf("%-36s %-36s\n", "id", "path");
    for (size_t i = 0; i < test_suite_count; i++) {
        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            printf("%-36s %-36s\n",
                    test_suites[i]->cases[j]->id,
                    test_suites[i]->cases[j]->path);
        }
    }
}

void list_defines(
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    (void)override_names;
    (void)override_defines;
    (void)override_count;
    // TODO
}

void list_geometries(
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    (void)override_names;
    (void)override_defines;
    (void)override_count;
    printf("%-36s %7s %7s %7s %7s %7s\n",
            "name", "read", "prog", "erase", "count", "size");
    for (size_t i = 0; i < test_geometry_count; i++) {
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

void run(
        const char *const *override_names,
        const test_define_t *override_defines,
        size_t override_count) {
    for (size_t i = 0; i < test_suite_count; i++) {
        test_define_overrides(
                test_suites[i],
                override_names, override_defines, override_count);

        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            for (size_t perm = 0;
                    perm < test_geometry_count
                        * test_suites[i]->cases[j]->permutations;
                    perm++) {
                size_t case_perm = perm / test_geometry_count;
                size_t geom_perm = perm % test_geometry_count;

                // setup defines
                test_define_geometry(&test_geometries[geom_perm]);
                test_define_case(test_suites[i]->cases[j], case_perm);

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

                // filter?
                if (test_suites[i]->cases[j]->filter) {
                    bool filter = test_suites[i]->cases[j]->filter(
                            &cfg, case_perm);
                    if (!filter) {
                        printf("skipped %s#%zu\n",
                                test_suites[i]->cases[j]->id,
                                perm);
                        continue;
                    }
                }

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
    OPT_LIST_SUITES     = 1,
    OPT_LIST_CASES      = 'l',
    OPT_LIST_PATHS      = 2,
    OPT_LIST_DEFINES    = 3,
    OPT_LIST_GEOMETRIES = 4,
    OPT_DEFINE          = 'D',
};

const char *short_opts = "hYlD:";

const struct option long_opts[] = {
    {"help",            no_argument,       NULL, OPT_HELP},
    {"summary",         no_argument,       NULL, OPT_SUMMARY},
    {"list-suites",     no_argument,       NULL, OPT_LIST_SUITES},
    {"list-cases",      no_argument,       NULL, OPT_LIST_CASES},
    {"list-paths",      no_argument,       NULL, OPT_LIST_PATHS},
    {"list-defines",    no_argument,       NULL, OPT_LIST_DEFINES},
    {"list-geometries", no_argument,       NULL, OPT_LIST_GEOMETRIES},
    {"define",          required_argument, NULL, OPT_DEFINE},
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
};

int main(int argc, char **argv) {
    void (*op)(
            const char *const *override_names,
            const test_define_t *override_defines,
            size_t override_count) = run;
    const char **override_names = NULL;
    test_define_t *override_defines = NULL;
    size_t override_count = 0;
    size_t override_cap = 0;

    // parse options
    while (true) {
        int c = getopt_long(argc, argv, short_opts, long_opts, NULL);
        switch (c) {
            // generate help message
            case OPT_HELP: {
                printf("usage: %s [options] [test_case]\n", argv[0]);
                printf("\n");

                printf("options:\n");
                size_t i = 0;
                while (long_opts[i].name) {
                    size_t indent;
                    if (long_opts[i].val >= '0' && long_opts[i].val < 'z') {
                        printf("  -%c, --%-16s",
                                long_opts[i].val,
                                long_opts[i].name);
                        indent = 8+strlen(long_opts[i].name);
                    } else {
                        printf("  --%-20s", long_opts[i].name);
                        indent = 4+strlen(long_opts[i].name);
                    }

                    // a quick, hacky, byte-level method for text wrapping
                    size_t len = strlen(help_text[i]);
                    size_t j = 0;
                    if (indent < 24) {
                        printf("%.80s\n", &help_text[i][j]);
                        j += 80;
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
            // done parsing
            case -1:
                goto getopt_done;
            // unknown arg, getopt prints a message for us
            default:
                exit(-1);
        }
    }
getopt_done:

    for (size_t i = 0; i < override_count; i++) {
        printf("define: %s %ju\n", override_names[i], override_defines[i]);
    }

    // do the thing
    op(
            override_names,
            override_defines,
            override_count);

    // cleanup (need to be done for valgrind testing)
    free(override_names);
    free(override_defines);}

