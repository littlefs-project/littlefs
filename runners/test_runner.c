
#include "runners/test_runner.h"
#include <getopt.h>


// disk geometries
struct test_geometry {
    const char *name;
    lfs_size_t read_size;
    lfs_size_t prog_size;
    lfs_size_t erase_size;
    lfs_size_t erase_count;
};

const struct test_geometry test_geometries[] = {
    // Made up geometries that works well for testing
    {"small",    16,   16,     512, (1024*1024)/512},
    {"medium",   16,   16,    4096, (1024*1024)/4096},
    {"big",      16,   16, 32*1024, (1024*1024)/(32*1024)},
    // EEPROM/NVRAM
    {"eeprom",    1,    1,     512, (1024*1024)/512},
    // SD/eMMC
    {"emmc",    512,  512,     512, (1024*1024)/512},
    // NOR flash
    {"nor",       1,    1,    4096, (1024*1024)/4096},
    // NAND flash
    {"nand",   4096, 4096, 32*1024, (1024*1024)/(32*1024)},
};
const size_t test_geometry_count = (
        sizeof(test_geometries) / sizeof(test_geometries[0]));


// option handling
enum opt_flags {
    OPT_HELP            = 'h',
    OPT_LIST            = 'l',
    OPT_LIST_PATHS      = 1,
    OPT_LIST_DEFINES    = 2,
    OPT_LIST_GEOMETRIES = 3,
};

const struct option long_opts[] = {
    {"help",            no_argument, NULL, OPT_HELP},
    {"list",            no_argument, NULL, OPT_LIST},
    {"list-paths",      no_argument, NULL, OPT_LIST_PATHS},
    {"list-defines",    no_argument, NULL, OPT_LIST_DEFINES},
    {"list-geometries", no_argument, NULL, OPT_LIST_GEOMETRIES},
    {NULL, 0, NULL, 0},
};

const char *const help_text[] = {
    "Show this help message.",
    "List test cases.",
    "List the path for each test case.",
    "List the defines for each test permutation.",
    "List the disk geometries used for testing.",
};

int main(int argc, char **argv) {
    bool list = false;
    bool list_paths = false;
    bool list_defines = false;
    bool list_geometries = false;

    // parse options
    while (true) {
        int index = 0;
        int c = getopt_long(argc, argv, "hl", long_opts, &index);
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
            // list flags
            case OPT_LIST:
                list = true;
                break;
            case OPT_LIST_PATHS:
                list_paths = true;
                break;
            case OPT_LIST_DEFINES:
                list_defines = true;
                break;
            case OPT_LIST_GEOMETRIES:
                list_geometries = true;
                break;
            // done parsing
            case -1:
                goto getopt_done;
            // unknown arg, getopt prints a message for us
            default:
                exit(-1);
        }
    }
getopt_done:

    // what do we need to do?
    if (list) {
        printf("%-36s %-12s %-12s %7s %7s\n",
                "id", "suite", "case", "type", "perms");
        for (size_t i = 0; i < test_suite_count; i++) {
            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                printf("%-36s %-12s %-12s %7s %7d\n",
                        test_suites[i]->cases[j]->id,
                        test_suites[i]->name,
                        test_suites[i]->cases[j]->name,
                        "n", // TODO
                        test_suites[i]->cases[j]->permutations);
            }
        }

    } else if (list_paths) {
        printf("%-36s %-36s\n", "id", "path");
        for (size_t i = 0; i < test_suite_count; i++) {
            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                printf("%-36s %-36s\n",
                        test_suites[i]->cases[j]->id,
                        test_suites[i]->cases[j]->path);
            }
        }
    } else if (list_defines) {
        // TODO
    } else if (list_geometries) {
        printf("%-12s %7s %7s %7s %7s %7s\n",
                "name", "read", "prog", "erase", "count", "size");
        for (size_t i = 0; i < test_geometry_count; i++) {
            printf("%-12s %7d %7d %7d %7d %7d\n",
                    test_geometries[i].name,
                    test_geometries[i].read_size,
                    test_geometries[i].prog_size,
                    test_geometries[i].erase_size,
                    test_geometries[i].erase_count,
                    test_geometries[i].erase_size
                        * test_geometries[i].erase_count);
        }
    } else {
        printf("remaining: ");
        for (int i = optind; i < argc; i++) {
            printf("%s ", argv[i]);
        }
        printf("\n");
    }
}

