
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "runners/test_runner.h"
#include "bd/lfs_testbd.h"

#include <getopt.h>
#include <sys/types.h>
#include <errno.h>
#include <setjmp.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>


// some helpers

// append to an array with amortized doubling
void *mappend(void **p,
        size_t size,
        size_t *count,
        size_t *capacity) {
    uint8_t *p_ = *p;
    size_t count_ = *count;
    size_t capacity_ = *capacity;

    count_ += 1;
    if (count_ > capacity_) {
        capacity_ = (2*capacity_ < 4) ? 4 : 2*capacity_;

        p_ = realloc(p_, capacity_*size);
        if (!p_) {
            return NULL;
        }
    }

    *p = p_;
    *count = count_;
    *capacity = capacity_;
    return &p_[(count_-1)*size];
}

// a quick self-terminating text-safe varint scheme
static void leb16_print(uintmax_t x) {
    while (true) {
        lfs_testbd_powercycles_t nibble = (x & 0xf) | (x > 0xf ? 0x10 : 0);
        printf("%c", (nibble < 10) ? '0'+nibble : 'a'+nibble-10);
        if (x <= 0xf) {
            break;
        }
        x >>= 4;
    }
}

static uintmax_t leb16_parse(const char *s, char **tail) {
    uintmax_t x = 0;
    size_t i = 0;
    while (true) {
        uintmax_t nibble = s[i];
        if (nibble >= '0' && nibble <= '9') {
            nibble = nibble - '0';
        } else if (nibble >= 'a' && nibble <= 'v') {
            nibble = nibble - 'a' + 10;
        } else {
            // invalid?
            if (tail) {
                *tail = (char*)s;
            }
            return 0;
        }

        x |= (nibble & 0xf) << (4*i);
        i += 1;
        if (!(nibble & 0x10)) {
            break;
        }
    }

    if (tail) {
        *tail = (char*)s + i;
    }
    return x;
}



// test_runner types

typedef struct test_geometry {
    char short_name;
    const char *long_name;

    lfs_size_t read_size;
    lfs_size_t prog_size;
    lfs_size_t block_size;
    lfs_size_t block_count;
} test_geometry_t;

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

typedef struct test_id {
    const char *suite;
    const char *case_;
    size_t perm;
    const test_geometry_t *geometry;
    const lfs_testbd_powercycles_t *cycles;
    size_t cycle_count;
} test_id_t;

static void print_id(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    (void)suite;
    // suite[:case[:perm[:geometry[:powercycles]]]]
    printf("%s:%zu:", case_->id, perm);

    // reduce duplication in geometry, this is appended to every test
    if (READ_SIZE != BLOCK_SIZE || PROG_SIZE != BLOCK_SIZE) {
        if (READ_SIZE != PROG_SIZE) {
            leb16_print(READ_SIZE);
        }
        leb16_print(PROG_SIZE);
    }
    leb16_print(BLOCK_SIZE);
    if (BLOCK_COUNT*BLOCK_SIZE != 1024*1024) {
        leb16_print(BLOCK_COUNT);
    }

    // only print power-cycles if any occured
    if (cycles) {
        printf(":");
        for (size_t i = 0; i < cycle_count; i++) {
            leb16_print(cycles[i]);
        }
    }
}


// test suites are linked into a custom ld section
extern struct test_suite __start__test_suites;
extern struct test_suite __stop__test_suites;

const struct test_suite *test_suites = &__start__test_suites;
#define TEST_SUITE_COUNT \
    ((size_t)(&__stop__test_suites - &__start__test_suites))


// test define management
typedef struct test_define_map {
    intmax_t (*const *defines)(size_t);
    const char *const *names;
    size_t count;
} test_define_map_t;

extern const test_geometry_t *test_geometry;

#define TEST_DEFINE(k, v) \
    intmax_t test_define_##k(__attribute__((unused)) size_t define) { \
        return v; \
    }

    TEST_IMPLICIT_DEFINES
#undef TEST_DEFINE

#define TEST_DEFINE_MAP_COUNT 3
test_define_map_t test_define_maps[TEST_DEFINE_MAP_COUNT] = {
    {NULL, NULL, 0},
    {NULL, NULL, 0},
    {
        (intmax_t (*const[TEST_IMPLICIT_DEFINE_COUNT])(size_t)){
            #define TEST_DEFINE(k, v) \
                [k##_i] = test_define_##k,

                TEST_IMPLICIT_DEFINES
            #undef TEST_DEFINE
        },
        (const char *const[TEST_IMPLICIT_DEFINE_COUNT]){
            #define TEST_DEFINE(k, v) \
                [k##_i] = #k,

                TEST_IMPLICIT_DEFINES
            #undef TEST_DEFINE
        },
        TEST_IMPLICIT_DEFINE_COUNT,
    },
};

intmax_t *test_define_cache;
size_t test_define_cache_count;
unsigned *test_define_cache_mask;

const char *test_define_name(size_t define) {
    // lookup in our test defines
    for (size_t i = 0; i < TEST_DEFINE_MAP_COUNT; i++) {
        if (define < test_define_maps[i].count
                && test_define_maps[i].names
                && test_define_maps[i].names[define]) {
            return test_define_maps[i].names[define];
        }
    }

    return NULL;
}

intmax_t test_define(size_t define) {
    // is the define in our cache?
    if (define < test_define_cache_count
            && (test_define_cache_mask[define/(8*sizeof(unsigned))]
                & (1 << (define%(8*sizeof(unsigned)))))) {
        return test_define_cache[define];
    }

    // lookup in our test defines
    for (size_t i = 0; i < TEST_DEFINE_MAP_COUNT; i++) {
        if (define < test_define_maps[i].count
                && test_define_maps[i].defines[define]) {
            intmax_t v = test_define_maps[i].defines[define](define);

            // insert into cache!
            test_define_cache[define] = v;
            test_define_cache_mask[define / (8*sizeof(unsigned))]
                    |= 1 << (define%(8*sizeof(unsigned)));

            return v;
        }
    }

    // not found?
    const char *name = test_define_name(define);
    fprintf(stderr, "error: undefined define %s (%zd)\n",
            name ? name : "(unknown)",
            define);
    assert(false);
    exit(-1);
}

void test_define_flush(void) {
    // clear cache between permutations
    memset(test_define_cache_mask, 0,
            sizeof(unsigned)*(
                (test_define_cache_count+(8*sizeof(unsigned))-1)
                / (8*sizeof(unsigned))));
}

// geometry updates
const test_geometry_t *test_geometry = NULL;

void test_define_geometry(const test_geometry_t *geometry) {
    test_geometry = geometry;
}

// override updates
typedef struct test_override {
    const char *name;
    intmax_t define;
} test_override_t;

const test_override_t *test_overrides = NULL;
size_t test_override_count = 0;
intmax_t *test_override_map = NULL;

intmax_t test_define_override(size_t define) {
    return test_override_map[define];
}

void test_define_overrides(
        const test_override_t *overrides,
        size_t override_count) {
    test_overrides = overrides;
    test_override_count = override_count;
}

// suite/perm updates
void test_define_suite(const struct test_suite *suite) {
    test_define_maps[1].names = suite->define_names;
    test_define_maps[1].count = suite->define_count;

    // make sure our cache is large enough
    if (lfs_max(suite->define_count, TEST_IMPLICIT_DEFINE_COUNT)
            > test_define_cache_count) {
        // align to power of two to avoid any superlinear growth
        size_t ncount = 1 << lfs_npw2(
                lfs_max(suite->define_count, TEST_IMPLICIT_DEFINE_COUNT));
        test_define_cache = realloc(test_define_cache, ncount*sizeof(intmax_t));
        test_define_cache_mask = realloc(test_define_cache_mask,
                sizeof(unsigned)*(
                    (ncount+(8*sizeof(unsigned))-1)
                    / (8*sizeof(unsigned))));
        test_define_cache_count = ncount;
    }

    // map any overrides
    if (test_override_count > 0) {
        // make sure our override arrays are big enough
        if (suite->define_count > test_define_maps[0].count) {
            // align to power of two to avoid any superlinear growth
            size_t ncount = 1 << lfs_npw2(suite->define_count);
            test_define_maps[0].defines = realloc(
                    (intmax_t (**)(size_t))test_define_maps[0].defines,
                    ncount*sizeof(intmax_t (*)(size_t)));
            test_override_map = realloc(
                    test_override_map,
                    ncount*sizeof(intmax_t));
            test_define_maps[0].count = ncount;
        }

        for (size_t i = 0; i < test_define_maps[0].count; i++) {
            ((intmax_t (**)(size_t))test_define_maps[0].defines)[i] = NULL;

            const char *name = test_define_name(i);
            if (!name) {
                continue;
            }

            for (size_t j = 0; j < test_override_count; j++) {
                if (strcmp(name, test_overrides[j].name) == 0) {
                    test_override_map[i] = test_overrides[j].define;
                    ((intmax_t (**)(size_t))test_define_maps[0].defines)[i]
                            = test_define_override;
                    break;
                }
            }
        }
    }
}

void test_define_perm(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm) {
    if (case_->defines) {
        test_define_maps[1].defines = case_->defines[perm];
        test_define_maps[1].count = suite->define_count;
    } else {
        test_define_maps[1].defines = NULL;
        test_define_maps[1].count = 0;
    }
}

void test_define_cleanup(void) {
    // test define management can allocate a few things
    free(test_define_cache);
    free(test_define_cache_mask);
    free(test_override_map);
    free((intmax_t (**)(size_t))test_define_maps[0].defines);
}



// test state
extern const test_geometry_t *test_geometries;
extern size_t test_geometry_count;

extern const test_powerloss_t *test_powerlosses;
extern size_t test_powerloss_count;

const test_id_t *test_ids = (const test_id_t[]) {
    {NULL, NULL, -1, NULL, NULL, 0},
};
size_t test_id_count = 1;

size_t test_step_start = 0;
size_t test_step_stop = -1;
size_t test_step_step = 1;

const char *test_disk_path = NULL;
const char *test_trace_path = NULL;
FILE *test_trace_file = NULL;
uint32_t test_trace_cycles = 0;
lfs_testbd_sleep_t test_read_sleep = 0.0;
lfs_testbd_sleep_t test_prog_sleep = 0.0;
lfs_testbd_sleep_t test_erase_sleep = 0.0;


// trace printing
void test_trace(const char *fmt, ...) {
    if (test_trace_path) {
        if (!test_trace_file) {
            // Tracing output is heavy and trying to open every trace
            // call is slow, so we only try to open the trace file every
            // so often. Note this doesn't affect successfully opened files
            if (test_trace_cycles % 128 != 0) {
                test_trace_cycles += 1;
                return;
            }
            test_trace_cycles += 1;

            int fd;
            if (strcmp(test_trace_path, "-") == 0) {
                fd = dup(1);
                if (fd < 0) {
                    return;
                }
            } else {
                fd = open(
                        test_trace_path,
                        O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK,
                        0666);
                if (fd < 0) {
                    return;
                }
                int err = fcntl(fd, F_SETFL, O_WRONLY | O_CREAT | O_APPEND);
                assert(!err);
            }

            FILE *f = fdopen(fd, "a");
            assert(f);
            int err = setvbuf(f, NULL, _IOLBF, BUFSIZ);
            assert(!err);
            test_trace_file = f;
        }

        va_list va;
        va_start(va, fmt);
        int res = vfprintf(test_trace_file, fmt, va);
        if (res < 0) {
            fclose(test_trace_file);
            test_trace_file = NULL;
        }
        va_end(va);
    }
}


// how many permutations are there actually in a test case
static void count_perms(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const test_geometry_t *geometry,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count,
        size_t *perms,
        size_t *filtered) {
    (void)cycle_count;
    size_t perms_ = 0;
    size_t filtered_ = 0;

    for (size_t k = 0; k < case_->permutations; k++) {
        if (perm != (size_t)-1 && k != perm) {
            continue;
        }

        // define permutation
        test_define_perm(suite, case_, k);

        for (size_t g = 0; g < (geometry ? 1 : test_geometry_count); g++) {
            // define geometry
            test_define_geometry(geometry ? geometry : &test_geometries[g]);
            test_define_flush();

            for (size_t p = 0; p < (cycles ? 1 : test_powerloss_count); p++) {
                // skip non-reentrant tests when powerloss testing
                if (!cycles
                        && test_powerlosses[p].short_name != '0'
                        && !(case_->flags & TEST_REENTRANT)) {
                    continue;
                }

                perms_ += 1;

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
    size_t suites = 0;
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

            test_define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                cases += 1;
                count_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].geometry,
                        test_ids[t].cycles,
                        test_ids[t].cycle_count,
                        &perms, &filtered);
            }

            suites += 1;
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
            suites,
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

            test_define_suite(&test_suites[i]);

            size_t cases = 0;
            size_t perms = 0;
            size_t filtered = 0;

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                cases += 1;
                count_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].geometry,
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
                    cases,
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

            test_define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                size_t perms = 0;
                size_t filtered = 0;

                count_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].geometry,
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

static void list_suite_paths(void) {
    printf("%-36s %s\n", "suite", "path");

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            printf("%-36s %s\n",
                    test_suites[i].id,
                    test_suites[i].path);
        }
    }
}

static void list_case_paths(void) {
    printf("%-36s %s\n", "case", "path");

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

                printf("%-36s %s\n",
                        test_suites[i].cases[j].id,
                        test_suites[i].cases[j].path);
            }
        }
    }
}

struct list_define {
    const char *name;
    intmax_t *values;
    size_t value_count;
    size_t value_capacity;
};

static void list_defines_perms(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const test_geometry_t *geometry,
        struct list_define **defines,
        size_t *define_count,
        size_t *define_capacity) {
    struct list_define *defines_ = *defines;
    size_t define_count_ = *define_count;
    size_t define_capacity_ = *define_capacity;

    for (size_t k = 0; k < case_->permutations; k++) {
        if (perm != (size_t)-1 && k != perm) {
            continue;
        }

        // define permutation
        test_define_perm(suite, case_, k);

        for (size_t g = 0; g < (geometry ? 1 : test_geometry_count); g++) {
            // define geometry
            test_define_geometry(geometry ? geometry : &test_geometries[g]);
            test_define_flush();

            // collect defines
            for (size_t d = 0;
                    d < lfs_max(suite->define_count,
                        TEST_IMPLICIT_DEFINE_COUNT);
                    d++) {
                if (!(d < TEST_IMPLICIT_DEFINE_COUNT || (
                        case_->defines
                        && case_->defines[k]
                        && case_->defines[k][d]))) {
                    continue;
                }
                const char *name = test_define_name(d);
                intmax_t value = test_define(d);

                // define already in defines?
                for (size_t i = 0; i < define_count_; i++) {
                    if (strcmp(defines_[i].name, name) == 0) {
                        // value already in values?
                        for (size_t j = 0; j < defines_[i].value_count; j++) {
                            if (defines_[i].values[j] == value) {
                                goto next_define;
                            }
                        }

                        *(intmax_t*)mappend(
                            (void**)&defines_[i].values,
                            sizeof(intmax_t),
                            &defines_[i].value_count,
                            &defines_[i].value_capacity) = value;

                        goto next_define;
                    }
                }

                {
                    // new define?
                    struct list_define *define = mappend(
                            (void**)&defines_,
                            sizeof(struct list_define),
                            &define_count_,
                            &define_capacity_);
                    define->name = name;
                    define->values = malloc(sizeof(intmax_t));
                    define->values[0] = value;
                    define->value_count = 1;
                    define->value_capacity = 1;
                }

                next_define:;
            }
        }
    }

    *defines = defines_;
    *define_count = define_count_;
    *define_capacity = define_capacity_;
}

static void list_defines(void) {
    struct list_define *defines = NULL;
    size_t define_count = 0;
    size_t define_capacity = 0;

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            test_define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                list_defines_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].geometry,
                        &defines,
                        &define_count,
                        &define_capacity);
            }
        }
    }

    for (size_t i = 0; i < define_count; i++) {
        printf("%s=", defines[i].name);
        for (size_t j = 0; j < defines[i].value_count; j++) {
            printf("%jd", defines[i].values[j]);
            if (j != defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < define_count; i++) {
        free(defines[i].values);
    }
    free(defines);
}

static void list_implicit(void) {
    struct list_define *defines = NULL;
    size_t define_count = 0;
    size_t define_capacity = 0;

    for (size_t t = 0; t < test_id_count; t++) {
        // yes we do need to define a suite, this does a bit of bookeeping
        // such as setting up the define cache
        test_define_suite(&(const struct test_suite){0});
        list_defines_perms(
                &(const struct test_suite){0},
                &(const struct test_case){.permutations=1},
                -1,
                test_ids[t].geometry,
                &defines,
                &define_count,
                &define_capacity);
    }

    for (size_t i = 0; i < define_count; i++) {
        printf("%s=", defines[i].name);
        for (size_t j = 0; j < defines[i].value_count; j++) {
            printf("%jd", defines[i].values[j]);
            if (j != defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < define_count; i++) {
        free(defines[i].values);
    }
    free(defines);
}



// geometries to test

const test_geometry_t builtin_geometries[] = {
    {'d', "default",   16,   16,   512,       (1024*1024)/512},
    {'e', "eeprom",     1,    1,   512,       (1024*1024)/512},
    {'E', "emmc",     512,  512,   512,       (1024*1024)/512},
    {'n', "nor",        1,    1,  4096,      (1024*1024)/4096},
    {'N', "nand",    4096, 4096, 32768, (1024*1024)/(32*1024)},
    {0, NULL, 0, 0, 0, 0},
};

const test_geometry_t *test_geometries = (const test_geometry_t[]){
    {'d', "default",   16,   16,   512,       (1024*1024)/512},
    {'e', "eeprom",     1,    1,   512,       (1024*1024)/512},
    {'E', "emmc",     512,  512,   512,       (1024*1024)/512},
    {'n', "nor",        1,    1,  4096,      (1024*1024)/4096},
    {'N', "nand",    4096, 4096, 32768, (1024*1024)/(32*1024)},
};
size_t test_geometry_count = 5;

static void list_geometries(void) {
    printf("%-24s %7s %7s %7s %7s %11s  %s\n",
            "geometry", "read", "prog", "erase", "count", "size", "leb16");
    size_t i = 0;
    for (; builtin_geometries[i].long_name; i++) {
        uintmax_t read_size   = builtin_geometries[i].read_size;
        uintmax_t prog_size   = builtin_geometries[i].prog_size;
        uintmax_t block_size  = builtin_geometries[i].block_size;
        uintmax_t block_count = builtin_geometries[i].block_count;
        printf("%c,%-22s %7ju %7ju %7ju %7ju %11ju  ",
                builtin_geometries[i].short_name,
                builtin_geometries[i].long_name,
                read_size,
                prog_size,
                block_size,
                block_count,
                block_size*block_count);
        if (read_size != block_size || prog_size != block_size) {
            if (read_size != prog_size) {
                leb16_print(read_size);
            }
            leb16_print(prog_size);
        }
        leb16_print(block_size);
        if (block_count*block_size != 1024*1024) {
            leb16_print(block_count);
        }
        printf("\n");
    }
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
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test
    printf("running ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

    case_->run(&cfg);

    printf("finished ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

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
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .power_cycles       = i,
        .powerloss_behavior = POWERLOSS_BEHAVIOR,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            // run the test
            case_->run(&cfg);
            break;
        }

        // power-loss!
        printf("powerloss ");
        print_id(suite, case_, perm, NULL, 0);
        printf(":");
        for (lfs_testbd_powercycles_t j = 1; j <= i; j++) {
            leb16_print(j);
        }
        printf("\n");

        i += 1;
        lfs_testbd_setpowercycles(&cfg, i);
    }

    printf("finished ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

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
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .power_cycles       = i,
        .powerloss_behavior = POWERLOSS_BEHAVIOR,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            // run the test
            case_->run(&cfg);
            break;
        }

        // power-loss!
        printf("powerloss ");
        print_id(suite, case_, perm, NULL, 0);
        printf(":");
        for (lfs_testbd_powercycles_t j = 1; j <= i; j *= 2) {
            leb16_print(j);
        }
        printf("\n");

        i *= 2;
        lfs_testbd_setpowercycles(&cfg, i);
    }

    printf("finished ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

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
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .power_cycles       = (i < cycle_count) ? cycles[i] : 0,
        .powerloss_behavior = POWERLOSS_BEHAVIOR,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            // run the test
            case_->run(&cfg);
            break;
        }

        // power-loss!
        assert(i <= cycle_count);
        printf("powerloss ");
        print_id(suite, case_, perm, cycles, i+1);
        printf("\n");

        i += 1;
        lfs_testbd_setpowercycles(&cfg,
                (i < cycle_count) ? cycles[i] : 0);
    }

    printf("finished ");
    print_id(suite, case_, perm, NULL, 0);
    printf("\n");

    // cleanup
    err = lfs_testbd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

struct powerloss_exhaustive_state {
    struct lfs_config *cfg;

    lfs_testbd_t *branches;
    size_t branch_count;
    size_t branch_capacity;
};

struct powerloss_exhaustive_cycles {
    lfs_testbd_powercycles_t *cycles;
    size_t cycle_count;
    size_t cycle_capacity;
};

static void powerloss_exhaustive_branch(void *c) {
    struct powerloss_exhaustive_state *state = c;
    // append to branches
    lfs_testbd_t *branch = mappend(
            (void**)&state->branches,
            sizeof(lfs_testbd_t),
            &state->branch_count,
            &state->branch_capacity);
    if (!branch) {
        fprintf(stderr, "error: exhaustive: out of memory\n");
        exit(-1);
    }

    // create copy-on-write copy
    int err = lfs_testbd_copy(state->cfg, branch);
    if (err) {
        fprintf(stderr, "error: exhaustive: could not create bd copy\n");
        exit(-1);
    }

    // also trigger on next power cycle
    lfs_testbd_setpowercycles(state->cfg, 1);
}

static void run_powerloss_exhaustive_layer(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        struct lfs_config *cfg,
        struct lfs_testbd_config *bdcfg,
        size_t depth,
        struct powerloss_exhaustive_cycles *cycles) {
    (void)suite;

    struct powerloss_exhaustive_state state = {
        .cfg = cfg,
        .branches = NULL,
        .branch_count = 0,
        .branch_capacity = 0,
    };

    // run through the test without additional powerlosses, collecting possible
    // branches as we do so
    lfs_testbd_setpowercycles(state.cfg, depth > 0 ? 1 : 0);
    bdcfg->powerloss_data = &state;

    // run the tests
    case_->run(cfg);

    // aggressively clean up memory here to try to keep our memory usage low
    int err = lfs_testbd_destroy(cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }

    // recurse into each branch
    for (size_t i = 0; i < state.branch_count; i++) {
        // first push and print the branch
        lfs_testbd_powercycles_t *cycle = mappend(
                (void**)&cycles->cycles,
                sizeof(lfs_testbd_powercycles_t),
                &cycles->cycle_count,
                &cycles->cycle_capacity);
        if (!cycle) {
            fprintf(stderr, "error: exhaustive: out of memory\n");
            exit(-1);
        }
        *cycle = i;

        printf("powerloss ");
        print_id(suite, case_, perm, cycles->cycles, cycles->cycle_count);
        printf("\n");

        // now recurse
        cfg->context = &state.branches[i];
        run_powerloss_exhaustive_layer(suite, case_, perm,
                cfg, bdcfg, depth-1, cycles);

        // pop the cycle
        cycles->cycle_count -= 1;
    }

    // clean up memory
    free(state.branches);
}

static void run_powerloss_exhaustive(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    (void)cycles;
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
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .powerloss_behavior = POWERLOSS_BEHAVIOR,
        .powerloss_cb       = powerloss_exhaustive_branch,
        .powerloss_data     = NULL,
    };

    int err = lfs_testbd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running %s:%zu\n", case_->id, perm);

    // recursively exhaust each layer of powerlosses
    run_powerloss_exhaustive_layer(suite, case_, perm,
            &cfg, &bdcfg, cycle_count,
            &(struct powerloss_exhaustive_cycles){NULL, 0, 0});

    printf("finished %s:%zu\n", case_->id, perm);
}


const test_powerloss_t builtin_powerlosses[] = {
    {'0', "none",        run_powerloss_none,        NULL, 0},
    {'e', "exponential", run_powerloss_exponential, NULL, 0},
    {'l', "linear",      run_powerloss_linear,      NULL, 0},
    {'x', "exhaustive",  run_powerloss_exhaustive,  NULL, SIZE_MAX},
    {0, NULL, NULL, NULL, 0},
};

const char *const builtin_powerlosses_help[] = {
    "Run with no power-losses.",
    "Run with exponentially-decreasing power-losses.",
    "Run with linearly-decreasing power-losses.",
    "Run a all permutations of power-losses, this may take a while.",
    "Run a all permutations of n power-losses.",
    "Run a custom comma-separated set of power-losses.",
    "Run a custom leb16-encoded set of power-losses.",
};

const test_powerloss_t *test_powerlosses = (const test_powerloss_t[]){
    {'0', "none", run_powerloss_none, NULL, 0},
};
size_t test_powerloss_count = 1;

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
    printf("%-24s %s\n", ":1248g1", builtin_powerlosses_help[i+2]);
}


// global test step count
size_t test_step = 0;

// run the tests
static void run_perms(
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm,
        const test_geometry_t *geometry,
        const lfs_testbd_powercycles_t *cycles,
        size_t cycle_count) {
    for (size_t k = 0; k < case_->permutations; k++) {
        if (perm != (size_t)-1 && k != perm) {
            continue;
        }

        // define permutation
        test_define_perm(suite, case_, k);

        for (size_t g = 0; g < (geometry ? 1 : test_geometry_count); g++) {
            // define geometry
            test_define_geometry(geometry ? geometry : &test_geometries[g]);
            test_define_flush();

            for (size_t p = 0; p < (cycles ? 1 : test_powerloss_count); p++) {
                // skip non-reentrant tests when powerloss testing
                if (!cycles
                        && test_powerlosses[p].short_name != '0'
                        && !(case_->flags & TEST_REENTRANT)) {
                    continue;
                }

                if (!(test_step >= test_step_start
                        && test_step < test_step_stop
                        && (test_step-test_step_start) % test_step_step == 0)) {
                    test_step += 1;
                    continue;
                }
                test_step += 1;

                // filter?
                if (case_->filter && !case_->filter()) {
                    printf("skipped %s:%zu\n", case_->id, k);
                    continue;
                }

                if (cycles) {
                    run_powerloss_cycles(
                            suite, case_, k,
                            cycles,
                            cycle_count);
                } else {
                    test_powerlosses[p].run(
                            suite, case_, k,
                            test_powerlosses[p].cycles,
                            test_powerlosses[p].cycle_count);
                }
            }
        }
    }
}

static void run(void) {
    // ignore disconnected pipes
    signal(SIGPIPE, SIG_IGN);

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < TEST_SUITE_COUNT; i++) {
            if (test_ids[t].suite && strcmp(
                    test_suites[i].name, test_ids[t].suite) != 0) {
                continue;
            }

            test_define_suite(&test_suites[i]);

            for (size_t j = 0; j < test_suites[i].case_count; j++) {
                if (test_ids[t].case_ && strcmp(
                        test_suites[i].cases[j].name, test_ids[t].case_) != 0) {
                    continue;
                }

                run_perms(&test_suites[i], &test_suites[i].cases[j],
                        test_ids[t].perm,
                        test_ids[t].geometry,
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
    OPT_LIST_SUITE_PATHS = 1,
    OPT_LIST_CASE_PATHS  = 2,
    OPT_LIST_DEFINES     = 3,
    OPT_LIST_IMPLICIT    = 4,
    OPT_LIST_GEOMETRIES  = 5,
    OPT_LIST_POWERLOSSES = 6,
    OPT_DEFINE           = 'D',
    OPT_GEOMETRY         = 'g',
    OPT_POWERLOSS        = 'p',
    OPT_STEP             = 's',
    OPT_DISK             = 'd',
    OPT_TRACE            = 't',
    OPT_READ_SLEEP       = 7,
    OPT_PROG_SLEEP       = 8,
    OPT_ERASE_SLEEP      = 9,
};

const char *short_opts = "hYlLD:g:p:s:d:t:";

const struct option long_opts[] = {
    {"help",             no_argument,       NULL, OPT_HELP},
    {"summary",          no_argument,       NULL, OPT_SUMMARY},
    {"list-suites",      no_argument,       NULL, OPT_LIST_SUITES},
    {"list-cases",       no_argument,       NULL, OPT_LIST_CASES},
    {"list-suite-paths", no_argument,       NULL, OPT_LIST_SUITE_PATHS},
    {"list-case-paths",  no_argument,       NULL, OPT_LIST_CASE_PATHS},
    {"list-defines",     no_argument,       NULL, OPT_LIST_DEFINES},
    {"list-implicit",    no_argument,       NULL, OPT_LIST_IMPLICIT},
    {"list-geometries",  no_argument,       NULL, OPT_LIST_GEOMETRIES},
    {"list-powerlosses", no_argument,       NULL, OPT_LIST_POWERLOSSES},
    {"define",           required_argument, NULL, OPT_DEFINE},
    {"geometry",         required_argument, NULL, OPT_GEOMETRY},
    {"powerloss",        required_argument, NULL, OPT_POWERLOSS},
    {"step",             required_argument, NULL, OPT_STEP},
    {"disk",             required_argument, NULL, OPT_DISK},
    {"trace",            required_argument, NULL, OPT_TRACE},
    {"read-sleep",       required_argument, NULL, OPT_READ_SLEEP},
    {"prog-sleep",       required_argument, NULL, OPT_PROG_SLEEP},
    {"erase-sleep",      required_argument, NULL, OPT_ERASE_SLEEP},
    {NULL, 0, NULL, 0},
};

const char *const help_text[] = {
    "Show this help message.",
    "Show quick summary.",
    "List test suites.",
    "List test cases.",
    "List the path for each test suite.",
    "List the path and line number for each test case.",
    "List all defines in this test-runner.",
    "List implicit defines in this test-runner.",
    "List the available disk geometries.",
    "List the available power-loss scenarios.",
    "Override a test define.",
    "Comma-separated list of disk geometries to test. Defaults to d,e,E,n,N.",
    "Comma-separated list of power-loss scenarios to test. Defaults to 0,l.",
    "Comma-separated range of test permutations to run (start,stop,step).",
    "Redirect block device operations to this file.",
    "Redirect trace output to this file.",
    "Artificial read delay in seconds.",
    "Artificial prog delay in seconds.",
    "Artificial erase delay in seconds.",
};

int main(int argc, char **argv) {
    void (*op)(void) = run;

    test_override_t *overrides = NULL;
    size_t override_count = 0;
    size_t override_capacity = 0;

    size_t test_geometry_capacity = 0;
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
            case OPT_LIST_SUITE_PATHS:
                op = list_suite_paths;
                break;
            case OPT_LIST_CASE_PATHS:
                op = list_case_paths;
                break;
            case OPT_LIST_DEFINES:
                op = list_defines;
                break;
            case OPT_LIST_IMPLICIT:
                op = list_implicit;
                break;
            case OPT_LIST_GEOMETRIES:
                op = list_geometries;
                break;
            case OPT_LIST_POWERLOSSES:
                op = list_powerlosses;
                break;
            // configuration
            case OPT_DEFINE: {
                // allocate space
                test_override_t *override = mappend(
                        (void**)&overrides,
                        sizeof(test_override_t),
                        &override_count,
                        &override_capacity);

                // parse into string key/intmax_t value, cannibalizing the
                // arg in the process
                char *sep = strchr(optarg, '=');
                char *parsed = NULL;
                if (!sep) {
                    goto invalid_define;
                }
                override->define = strtoumax(sep+1, &parsed, 0);
                if (parsed == sep+1) {
                    goto invalid_define;
                }

                override->name = optarg;
                *sep = '\0';
                break;

invalid_define:
                fprintf(stderr, "error: invalid define: %s\n", optarg);
                exit(-1);
            }
            case OPT_GEOMETRY: {
                // reset our geometry scenarios
                if (test_geometry_capacity > 0) {
                    free((test_geometry_t*)test_geometries);
                }
                test_geometries = NULL;
                test_geometry_count = 0;
                test_geometry_capacity = 0;

                // parse the comma separated list of disk geometries
                while (*optarg) {
                    // allocate space
                    test_geometry_t *geometry = mappend(
                            (void**)&test_geometries,
                            sizeof(test_geometry_t),
                            &test_geometry_count,
                            &test_geometry_capacity);

                    // parse the disk geometry
                    optarg += strspn(optarg, " ");

                    // named disk geometry
                    size_t len = strcspn(optarg, " ,");
                    for (size_t i = 0; builtin_geometries[i].long_name; i++) {
                        if ((len == 1
                                && *optarg == builtin_geometries[i].short_name)
                                || (len == strlen(
                                        builtin_geometries[i].long_name)
                                    && memcmp(optarg,
                                        builtin_geometries[i].long_name,
                                        len) == 0))  {
                            *geometry = builtin_geometries[i];
                            optarg += len;
                            goto geometry_next;
                        }
                    }

                    // comma-separated read/prog/erase/count
                    if (*optarg == '{') {
                        lfs_size_t sizes[4];
                        size_t count = 0;

                        char *s = optarg + 1;
                        while (count < 4) {
                            char *parsed = NULL;
                            sizes[count] = strtoumax(s, &parsed, 0);
                            count += 1;

                            s = parsed + strspn(parsed, " ");
                            if (*s == ',') {
                                s += 1;
                                continue;
                            } else if (*s == '}') {
                                s += 1;
                                break;
                            } else {
                                goto geometry_unknown;
                            }
                        }

                        // allow implicit r=p and p=e for common geometries
                        geometry->read_size = sizes[0];
                        geometry->prog_size
                                = count >= 3 ? sizes[1]
                                : sizes[0];
                        geometry->block_size
                                = count >= 3 ? sizes[2]
                                : count >= 2 ? sizes[1]
                                : sizes[0];
                        // if no block_count, figure out 1 MiB total size
                        geometry->block_count
                                = count >= 4 ? sizes[3]
                                : (1024*1024) / geometry->block_size;
                        optarg = s;
                        goto geometry_next;
                    }

                    // leb16-encoded read/prog/erase/count
                    if (*optarg == ':') {
                        lfs_size_t sizes[4];
                        size_t count = 0;

                        char *s = optarg + 1;
                        while (true) {
                            char *parsed = NULL;
                            uintmax_t x = leb16_parse(s, &parsed);
                            if (parsed == s || count >= 4) {
                                break;
                            }

                            sizes[count] = x;
                            count += 1;
                            s = parsed;
                        }

                        // allow implicit r=p and p=e for common geometries
                        geometry->read_size = sizes[0];
                        geometry->prog_size
                                = count >= 3 ? sizes[1]
                                : sizes[0];
                        geometry->block_size
                                = count >= 3 ? sizes[2]
                                : count >= 2 ? sizes[1]
                                : sizes[0];
                        // if no block_count, figure out 1 MiB total size
                        geometry->block_count
                                = count >= 4 ? sizes[3]
                                : (1024*1024) / geometry->block_size;
                        optarg = s;
                        goto geometry_next;
                    }

geometry_unknown:
                    // unknown scenario?
                    fprintf(stderr, "error: unknown disk geometry: %s\n",
                            optarg);
                    exit(-1);

geometry_next:
                    optarg += strspn(optarg, " ");
                    if (*optarg == ',') {
                        optarg += 1;
                    } else if (*optarg == '\0') {
                        break;
                    } else {
                        goto geometry_unknown;
                    }
                }
                break;
            }
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
                    test_powerloss_t *powerloss = mappend(
                            (void**)&test_powerlosses,
                            sizeof(test_powerloss_t),
                            &test_powerloss_count,
                            &test_powerloss_capacity);

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
                            *powerloss = builtin_powerlosses[i];
                            optarg += len;
                            goto powerloss_next;
                        }
                    }

                    // comma-separated permutation
                    if (*optarg == '{') {
                        lfs_testbd_powercycles_t *cycles = NULL;
                        size_t cycle_count = 0;
                        size_t cycle_capacity = 0;

                        char *s = optarg + 1;
                        while (true) {
                            char *parsed = NULL;
                            *(lfs_testbd_powercycles_t*)mappend(
                                    (void**)&cycles,
                                    sizeof(lfs_testbd_powercycles_t),
                                    &cycle_count,
                                    &cycle_capacity)
                                    = strtoumax(s, &parsed, 0);

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

                        *powerloss = (test_powerloss_t){
                            .run = run_powerloss_cycles,
                            .cycles = cycles,
                            .cycle_count = cycle_count,
                        };
                        optarg = s;
                        goto powerloss_next;
                    }

                    // leb16-encoded permutation
                    if (*optarg == ':') {
                        lfs_testbd_powercycles_t *cycles = NULL;
                        size_t cycle_count = 0;
                        size_t cycle_capacity = 0;

                        char *s = optarg + 1;
                        while (true) {
                            char *parsed = NULL;
                            uintmax_t x = leb16_parse(s, &parsed);
                            if (parsed == s) {
                                break;
                            }

                            *(lfs_testbd_powercycles_t*)mappend(
                                    (void**)&cycles,
                                    sizeof(lfs_testbd_powercycles_t),
                                    &cycle_count,
                                    &cycle_capacity) = x;
                            s = parsed;
                        }

                        *powerloss = (test_powerloss_t){
                            .run = run_powerloss_cycles,
                            .cycles = cycles,
                            .cycle_count = cycle_count,
                        };
                        optarg = s;
                        goto powerloss_next;
                    }

                    // exhaustive permutations
                    {
                        char *parsed = NULL;
                        size_t count = strtoumax(optarg, &parsed, 0);
                        if (parsed == optarg) {
                            goto powerloss_unknown;
                        }
                        *powerloss = (test_powerloss_t){
                            .run = run_powerloss_exhaustive,
                            .cycles = NULL,
                            .cycle_count = count,
                        };
                        optarg = (char*)parsed;
                        goto powerloss_next;
                    }

powerloss_unknown:
                    // unknown scenario?
                    fprintf(stderr, "error: unknown power-loss scenario: %s\n",
                            optarg);
                    exit(-1);

powerloss_next:
                    optarg += strspn(optarg, " ");
                    if (*optarg == ',') {
                        optarg += 1;
                    } else if (*optarg == '\0') {
                        break;
                    } else {
                        goto powerloss_unknown;
                    }
                }
                break;
            }
            case OPT_STEP: {
                char *parsed = NULL;
                size_t start = strtoumax(optarg, &parsed, 0);
                // allow empty string for start=0
                if (parsed != optarg) {
                    test_step_start = start;
                }
                optarg = parsed + strspn(parsed, " ");

                if (*optarg != ',' && *optarg != '\0') {
                    goto step_unknown;
                }

                if (*optarg == ',') {
                    optarg += 1;
                    size_t stop = strtoumax(optarg, &parsed, 0);
                    // allow empty string for stop=end
                    if (parsed != optarg) {
                        test_step_stop = stop;
                    }
                    optarg = parsed + strspn(parsed, " ");

                    if (*optarg != ',' && *optarg != '\0') {
                        goto step_unknown;
                    }

                    if (*optarg == ',') {
                        optarg += 1;
                        size_t step = strtoumax(optarg, &parsed, 0);
                        // allow empty string for stop=1
                        if (parsed != optarg) {
                            test_step_step = step;
                        }
                        optarg = parsed + strspn(parsed, " ");

                        if (*optarg != '\0') {
                            goto step_unknown;
                        }
                    }
                }

                break;
step_unknown:
                fprintf(stderr, "error: invalid step: %s\n", optarg);
                exit(-1);
            }
            case OPT_DISK:
                test_disk_path = optarg;
                break;
            case OPT_TRACE:
                test_trace_path = optarg;
                break;
            case OPT_READ_SLEEP: {
                char *parsed = NULL;
                double read_sleep = strtod(optarg, &parsed);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid read-sleep: %s\n", optarg);
                    exit(-1);
                }
                test_read_sleep = read_sleep*1.0e9;
                break;
            }
            case OPT_PROG_SLEEP: {
                char *parsed = NULL;
                double prog_sleep = strtod(optarg, &parsed);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid prog-sleep: %s\n", optarg);
                    exit(-1);
                }
                test_prog_sleep = prog_sleep*1.0e9;
                break;
            }
            case OPT_ERASE_SLEEP: {
                char *parsed = NULL;
                double erase_sleep = strtod(optarg, &parsed);
                if (parsed == optarg) {
                    fprintf(stderr, "error: invalid erase-sleep: %s\n", optarg);
                    exit(-1);
                }
                test_erase_sleep = erase_sleep*1.0e9;
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

    if (argc > optind) {
        // reset our test identifier list
        test_ids = NULL;
        test_id_count = 0;
        test_id_capacity = 0;
    }

    // parse test identifier, if any, cannibalizing the arg in the process
    for (; argc > optind; optind++) {
        size_t perm = -1;
        test_geometry_t *geometry = NULL;
        lfs_testbd_powercycles_t *cycles = NULL;
        size_t cycle_count = 0;

        // parse suite
        char *suite = argv[optind];
        char *case_ = strchr(suite, ':');
        if (case_) {
            *case_ = '\0';
            case_ += 1;
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

        if (case_) {
            // parse case
            char *perm_ = strchr(case_, ':');
            if (perm_) {
                *perm_ = '\0';
                perm_ += 1;
            }

            // nothing really to do for case

            if (perm_) {
                // parse permutation
                char *geometry_ = strchr(perm_, ':');
                if (geometry_) {
                    *geometry_ = '\0';
                    geometry_ += 1;
                }

                char *parsed = NULL;
                perm = strtoumax(perm_, &parsed, 10);
                if (parsed == perm_) {
                    fprintf(stderr, "error: "
                            "could not parse test permutation: %s\n", perm_);
                    exit(-1);
                }

                if (geometry_) {
                    // parse geometry
                    char *cycles_ = strchr(geometry_, ':');
                    if (cycles_) {
                        *cycles_ = '\0';
                        cycles_ += 1;
                    }

                    geometry = malloc(sizeof(test_geometry_t));
                    lfs_size_t sizes[4];
                    size_t count = 0;

                    while (*geometry_ != '\0') {
                        uintmax_t x = leb16_parse(geometry_, &parsed);
                        if (parsed == geometry_ || count >= 4) {
                            fprintf(stderr, "error: "
                                    "count not parse test geometry: %s\n",
                                    geometry_);
                            exit(-1);
                        }

                        sizes[count] = x;
                        count += 1;
                        geometry_ = parsed;
                    }

                    // allow implicit r=p and p=e for common geometries
                    geometry->read_size = sizes[0];
                    geometry->prog_size
                            = count >= 3 ? sizes[1]
                            : sizes[0];
                    geometry->block_size
                            = count >= 3 ? sizes[2]
                            : count >= 2 ? sizes[1]
                            : sizes[0];
                    // if no block_count, figure out 1 MiB total size
                    geometry->block_count
                            = count >= 4 ? sizes[3]
                            : (1024*1024) / geometry->block_size;

                    if (cycles_) {
                        // parse power cycles
                        size_t cycle_capacity = 0;
                        while (*cycles_ != '\0') {
                            *(lfs_testbd_powercycles_t*)mappend(
                                    (void**)&cycles,
                                    sizeof(lfs_testbd_powercycles_t),
                                    &cycle_count,
                                    &cycle_capacity)
                                    = leb16_parse(cycles_, &parsed);
                            if (parsed == cycles_) {
                                fprintf(stderr, "error: "
                                        "could not parse test cycles: %s\n",
                                        cycles_);
                                exit(-1);
                            }
                            cycles_ = parsed;
                        }
                    }
                }
            }
        }

        // append to identifier list
        *(test_id_t*)mappend(
                (void**)&test_ids,
                sizeof(test_id_t),
                &test_id_count,
                &test_id_capacity) = (test_id_t){
            .suite = suite,
            .case_ = case_,
            .perm = perm,
            .geometry = geometry,
            .cycles = cycles,
            .cycle_count = cycle_count,
        };
    }

    // register overrides
    test_define_overrides(overrides, override_count);

    // do the thing
    op();

    // cleanup (need to be done for valgrind testing)
    test_define_cleanup();
    free(overrides);

    if (test_geometry_capacity) {
        free((test_geometry_t*)test_geometries);
    }
    if (test_powerloss_capacity) {
        for (size_t i = 0; i < test_powerloss_count; i++) {
            free((lfs_testbd_powercycles_t*)test_powerlosses[i].cycles);
        }
        free((test_powerloss_t*)test_powerlosses);
    }
    if (test_id_capacity) {
        for (size_t i = 0; i < test_id_count; i++) {
            free((test_geometry_t*)test_ids[i].geometry);
            free((lfs_testbd_powercycles_t*)test_ids[i].cycles);
        }
        free((test_id_t*)test_ids);
    }
}
