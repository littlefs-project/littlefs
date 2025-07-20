/*
 * Runner for littlefs tests
 *
 * Copyright (c) 2022, The littlefs authors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "runners/test_runner.h"
#include "bd/lfs3_emubd.h"

#include <getopt.h>
#include <sys/types.h>
#include <errno.h>
#include <setjmp.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <execinfo.h>
#include <signal.h>


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
    // allow 'w' to indicate negative numbers
    if ((intmax_t)x < 0) {
        printf("w");
        x = -x;
    }

    while (true) {
        char nibble = (x & 0xf) | ((x > 0xf) ? 0x10 : 0);
        printf("%c", (nibble < 10) ? '0'+nibble : 'a'+nibble-10);
        if (x <= 0xf) {
            break;
        }
        x >>= 4;
    }
}

static uintmax_t leb16_parse(const char *s, char **tail) {
    bool neg = false;
    uintmax_t x = 0;
    if (tail) {
        *tail = (char*)s;
    }

    if (s[0] == 'w') {
        neg = true;
        s = s+1;
    }

    size_t i = 0;
    while (true) {
        uintmax_t nibble = s[i];
        if (nibble >= '0' && nibble <= '9') {
            nibble = nibble - '0';
        } else if (nibble >= 'a' && nibble <= 'v') {
            nibble = nibble - 'a' + 10;
        } else {
            // invalid?
            return 0;
        }

        x |= (nibble & 0xf) << (4*i);
        i += 1;
        if (!(nibble & 0x10)) {
            s = s + i;
            break;
        }
    }

    if (tail) {
        *tail = (char*)s;
    }
    return (neg) ? -x : x;
}



// test_runner types

typedef struct test_powerloss {
    const char *name;
    void (*run)(
            const struct test_powerloss *powerloss,
            const struct test_suite *suite,
            const struct test_case *case_);
    const lfs3_emubd_powercycles_t *cycles;
    size_t cycle_count;
} test_powerloss_t;

typedef struct test_id {
    const char *name;
    test_define_t *defines;
    size_t define_count;
    test_powerloss_t powerloss;
} test_id_t;


// test define management

// implicit defines declared here
#define TEST_DEFINE(k, v) \
    intmax_t k;

    TEST_IMPLICIT_DEFINES
#undef TEST_DEFINE

#define TEST_DEFINE(k, v) \
    intmax_t test_define_##k(void *data, size_t i) { \
        (void)data; \
        (void)i; \
        return v; \
    }

    TEST_IMPLICIT_DEFINES
#undef TEST_DEFINE

const test_define_t test_implicit_defines[] = {
    #define TEST_DEFINE(k, v) \
        {#k, &k, test_define_##k, NULL, 1},

        TEST_IMPLICIT_DEFINES
    #undef TEST_DEFINE
};
const size_t test_implicit_define_count
        = sizeof(test_implicit_defines) / sizeof(test_define_t);

// some helpers
intmax_t test_define_lit(void *data, size_t i) {
    (void)i;
    return (intptr_t)data;
}

#define TEST_LIT(name, v) ((test_define_t){ \
    name, NULL, test_define_lit, (void*)(uintptr_t)(v), 1})


// define mapping
const test_define_t **test_defines = NULL;
size_t test_define_count = 0;
size_t test_define_capacity = 0;

const test_define_t **test_suite_defines = NULL;
size_t test_suite_define_count = 0;
ssize_t *test_suite_define_map = NULL;

test_define_t *test_override_defines = NULL;
size_t test_override_define_count = 0;

size_t test_define_depth = 1000;


static inline bool test_define_isdefined(const test_define_t *define) {
    return define->cb;
}

static inline bool test_define_ispermutation(const test_define_t *define) {
    // permutation defines are basically anything that's not implicit
    return test_define_isdefined(define)
            && !(define >= test_implicit_defines
                && define
                    < test_implicit_defines
                        + test_implicit_define_count);
}


void test_define_suite(
        const test_id_t *id,
        const struct test_suite *suite) {
    // reset our mapping
    test_define_count = 0;
    test_suite_define_count = 0;

    // make sure we have space for everything, just assume the worst case
    if (test_implicit_define_count + suite->define_count
            > test_define_capacity) {
        test_define_capacity
                = test_implicit_define_count + suite->define_count;
        test_defines = realloc(
                test_defines,
                test_define_capacity*sizeof(const test_define_t*));
        test_suite_defines = realloc(
                test_suite_defines,
                test_define_capacity*sizeof(const test_define_t*));
        test_suite_define_map = realloc(
                test_suite_define_map,
                test_define_capacity*sizeof(ssize_t));
    }

    // first map our implicit defines
    for (size_t i = 0; i < test_implicit_define_count; i++) {
        test_suite_defines[i] = &test_implicit_defines[i];
    }
    test_suite_define_count = test_implicit_define_count;

    // build a mapping from suite defines to test defines
    //
    // we will use this for both suite and case defines
    memset(test_suite_define_map, -1,
            test_suite_define_count*sizeof(size_t));

    for (size_t i = 0; i < suite->define_count; i++) {
        // assume suite defines are unique so we only need to compare
        // against implicit defines, this avoids a O(n^2)
        for (size_t j = 0; j < test_implicit_define_count; j++) {
            if (test_suite_defines[j]->define == suite->defines[i].define) {
                test_suite_define_map[j] = i;

                // don't override implicit defines if we're not defined
                if (test_define_isdefined(&suite->defines[i])) {
                    test_suite_defines[j] = &suite->defines[i];
                }
                goto next_suite_define;
            }
        }

        // map a new suite define
        test_suite_define_map[test_suite_define_count] = i;
        test_suite_defines[test_suite_define_count] = &suite->defines[i];
        test_suite_define_count += 1;
next_suite_define:;
    }

    // map any explicit defines
    //
    // we ignore any out-of-bounds defines here, even though it's likely
    // an error
    if (id && id->defines) {
        for (size_t i = 0;
                i < id->define_count && i < test_suite_define_count;
                i++) {
            if (test_define_isdefined(&id->defines[i])) {
                // update name/addr
                id->defines[i].name = test_suite_defines[i]->name;
                id->defines[i].define = test_suite_defines[i]->define;
                // map and override suite mapping
                test_suite_defines[i] = &id->defines[i];
                test_suite_define_map[i] = -1;
            }
        }
    }

    // map any override defines
    //
    // note it's not an error to override a define that doesn't exist
    for (size_t i = 0; i < test_override_define_count; i++) {
        for (size_t j = 0; j < test_suite_define_count; j++) {
            if (strcmp(
                    test_suite_defines[j]->name,
                    test_override_defines[i].name) == 0) {
                // update addr
                test_override_defines[i].define
                        = test_suite_defines[j]->define;
                // map and override suite mapping
                test_suite_defines[j] = &test_override_defines[i];
                test_suite_define_map[j] = -1;
                goto next_override_define;
            }
        }
next_override_define:;
    }
}

void test_define_case(
        const test_id_t *id,
        const struct test_suite *suite,
        const struct test_case *case_,
        size_t perm) {
    (void)id;

    // copy over suite defines
    for (size_t i = 0; i < test_suite_define_count; i++) {
        // map case define if case define is defined
        if (case_->defines
                && test_suite_define_map[i] != -1
                && test_define_isdefined(&case_->defines[
                    perm*suite->define_count
                        + test_suite_define_map[i]])) {
            test_defines[i] = &case_->defines[
                    perm*suite->define_count
                        + test_suite_define_map[i]];
        } else {
            test_defines[i] = test_suite_defines[i];
        }
    }
    test_define_count = test_suite_define_count;
}

void test_define_permutation(size_t perm) {
    // first zero everything, we really don't want reproducibility issues
    for (size_t i = 0; i < test_define_count; i++) {
        *test_defines[i]->define = 0;
    }

    // defines may be mutually recursive, which makes evaluation a bit tricky
    //
    // Rather than doing any clever, we just repeatedly evaluate the
    // permutation until values stabilize. If things don't stabilize after
    // some number of iterations, error, this likely means defines were
    // stuck in a cycle
    //
    size_t attempt = 0;
    while (true) {
        const test_define_t *changed = NULL;
        // define-specific permutations are encoded in the case permutation
        size_t perm_ = perm;
        for (size_t i = 0; i < test_define_count; i++) {
            if (test_defines[i]->cb) {
                intmax_t v = test_defines[i]->cb(
                        test_defines[i]->data,
                        perm_ % test_defines[i]->permutations);
                if (v != *test_defines[i]->define) {
                    *test_defines[i]->define = v;
                    changed = test_defines[i];
                }

                perm_ /= test_defines[i]->permutations;
            }
        }

        // stabilized?
        if (!changed) {
            break;
        }

        attempt += 1;
        if (test_define_depth && attempt >= test_define_depth+1) {
            fprintf(stderr, "error: could not resolve recursive defines: %s\n",
                    changed->name);
            exit(-1);
        }
    }
}

void test_define_cleanup(void) {
    // test define management can allocate a few things
    free(test_defines);
    free(test_suite_defines);
    free(test_suite_define_map);
}

size_t test_define_permutations(void) {
    size_t prod = 1;
    for (size_t i = 0; i < test_define_count; i++) {
        prod *= (test_defines[i]->permutations > 0)
                ? test_defines[i]->permutations
                : 1;
    }
    return prod;
}


// override define stuff

typedef struct test_override_value {
    intmax_t start;
    intmax_t stop;
    // step == 0 indicates a single value
    intmax_t step;
} test_override_value_t;

typedef struct test_override_data {
    test_override_value_t *values;
    size_t value_count;
} test_override_data_t;

intmax_t test_override_cb(void *data, size_t i) {
    const test_override_data_t *data_ = data;
    for (size_t j = 0; j < data_->value_count; j++) {
        const test_override_value_t *v = &data_->values[j];
        // range?
        if (v->step) {
            size_t range_count;
            if (v->step > 0) {
                range_count = (v->stop-1 - v->start) / v->step + 1;
            } else {
                range_count = (v->start-1 - v->stop) / -v->step + 1;
            }

            if (i < range_count) {
                return i*v->step + v->start;
            }
            i -= range_count;
        // value?
        } else {
            if (i == 0) {
                return v->start;
            }
            i -= 1;
        }
    }

    // should never get here
    assert(false);
    __builtin_unreachable();
}



// test state
const test_id_t *test_ids = (const test_id_t[]) {
    {NULL, NULL, 0, {NULL, NULL, NULL, 0}},
};
size_t test_id_count = 1;

size_t test_step_start = 0;
size_t test_step_stop = -1;
size_t test_step_step = 1;
bool test_all = false;

const char *test_disk_path = NULL;
const char *test_trace_path = NULL;
bool test_trace_backtrace = false;
uint32_t test_trace_period = 0;
uint32_t test_trace_freq = 0;
FILE *test_trace_file = NULL;
uint32_t test_trace_cycles = 0;
uint64_t test_trace_time = 0;
uint64_t test_trace_open_time = 0;
lfs3_emubd_sleep_t test_read_sleep = 0.0;
lfs3_emubd_sleep_t test_prog_sleep = 0.0;
lfs3_emubd_sleep_t test_erase_sleep = 0.0;

volatile size_t TEST_PLS = 0;

extern const test_powerloss_t *test_powerlosses;
extern size_t test_powerloss_count;


// this determines both the backtrace buffer and the trace printf buffer, if
// trace ends up interleaved or truncated this may need to be increased
#ifndef TEST_TRACE_BACKTRACE_BUFFER_SIZE
#define TEST_TRACE_BACKTRACE_BUFFER_SIZE 8192
#endif
void *test_trace_backtrace_buffer[
    TEST_TRACE_BACKTRACE_BUFFER_SIZE / sizeof(void*)];

// trace printing
void test_trace(const char *fmt, ...) {
    if (test_trace_path) {
        // sample at a specific period?
        if (test_trace_period) {
            if (test_trace_cycles % test_trace_period != 0) {
                test_trace_cycles += 1;
                return;
            }
            test_trace_cycles += 1;
        }

        // sample at a specific frequency?
        if (test_trace_freq) {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            uint64_t now = (uint64_t)t.tv_sec*1000*1000*1000
                    + (uint64_t)t.tv_nsec;
            if (now - test_trace_time < (1000*1000*1000) / test_trace_freq) {
                return;
            }
            test_trace_time = now;
        }

        if (!test_trace_file) {
            // Tracing output is heavy and trying to open every trace
            // call is slow, so we only try to open the trace file every
            // so often. Note this doesn't affect successfully opened files
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            uint64_t now = (uint64_t)t.tv_sec*1000*1000*1000
                    + (uint64_t)t.tv_nsec;
            if (now - test_trace_open_time < 100*1000*1000) {
                return;
            }
            test_trace_open_time = now;

            // try to open the trace file
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
            int err = setvbuf(f, NULL, _IOFBF,
                    TEST_TRACE_BACKTRACE_BUFFER_SIZE);
            assert(!err);
            test_trace_file = f;
        }

        // print trace
        va_list va;
        va_start(va, fmt);
        int res = vfprintf(test_trace_file, fmt, va);
        va_end(va);
        if (res < 0) {
            fclose(test_trace_file);
            test_trace_file = NULL;
            return;
        }

        if (test_trace_backtrace) {
            // print backtrace
            size_t count = backtrace(
                    test_trace_backtrace_buffer,
                    TEST_TRACE_BACKTRACE_BUFFER_SIZE);
            // note we skip our own stack frame
            for (size_t i = 1; i < count; i++) {
                res = fprintf(test_trace_file, "\tat %p\n",
                        test_trace_backtrace_buffer[i]);
                if (res < 0) {
                    fclose(test_trace_file);
                    test_trace_file = NULL;
                    return;
                }
            }
        }

        // flush immediately
        fflush(test_trace_file);
    }
}

// test prng
uint32_t test_prng(uint32_t *state) {
    // A simple xorshift32 generator, easily reproducible. Keep in mind
    // determinism is much more important than actual randomness here.
    uint32_t x = *state;
    // must be non-zero, use uintmax here so that seed=0 is different
    // from seed=1 and seed=range(0,n) makes a bit more sense
    if (x == 0) {
        x = -1;
    }
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// test factorial
size_t test_factorial(size_t x) {
    size_t y = 1;
    for (size_t i = 2; i <= x; i++) {
        y *= i;
    }
    return y;
}

// test array permutations
void test_permutation(size_t i, uint32_t *buffer, size_t size) {
    // https://stackoverflow.com/a/7919887 and
    // https://stackoverflow.com/a/24257996 helped a lot with this, but
    // changed to run in O(n) with no extra memory. This has a tradeoff
    // of generating the permutations in an unintuitive order.

    // initialize array
    for (size_t j = 0; j < size; j++) {
        buffer[j] = j;
    }

    for (size_t j = 0; j < size; j++) {
        // swap index with digit
        //
        //      .- i%rem --.
        //      v     .----+----.
        // [p0 p1 |-> r0 r1 r2 r3]
        //
        size_t t = buffer[j + (i % (size-j))];
        buffer[j + (i % (size-j))] = buffer[j];
        buffer[j] = t;
        // update i
        i /= (size-j);
    }
}


// encode our permutation into a reusable id
static void perm_printid(
        const struct test_suite *suite,
        const struct test_case *case_,
        const lfs3_emubd_powercycles_t *cycles,
        size_t cycle_count) {
    (void)suite;
    // case[:permutation[:powercycles]]
    printf("%s:", case_->name);
    for (size_t d = 0; d < test_define_count; d++) {
        if (test_define_ispermutation(test_defines[d])) {
            leb16_print(d);
            leb16_print(*test_defines[d]->define);
        }
    }

    // only print power-cycles if any occured
    if (cycle_count) {
        printf(":");
        for (size_t i = 0; i < cycle_count; i++) {
            leb16_print(cycles[i]);
        }
    }
}


// a quick trie for keeping track of permutations we've seen
typedef struct test_seen {
    struct test_seen_branch *branches;
    size_t branch_count;
    size_t branch_capacity;
} test_seen_t;

struct test_seen_branch {
    intmax_t define;
    struct test_seen branch;
};

bool test_seen_insert(test_seen_t *seen) {
    // use the currently set defines
    bool was_seen = true;
    for (size_t d = 0; d < test_define_count; d++) {
        // treat unpermuted defines the same as 0
        intmax_t v = test_define_ispermutation(test_defines[d])
                ? *test_defines[d]->define
                : 0;

        // already seen?
        struct test_seen_branch *branch = NULL;
        for (size_t i = 0; i < seen->branch_count; i++) {
            if (seen->branches[i].define == v) {
                branch = &seen->branches[i];
                break;
            }
        }

        // need to create a new node
        if (!branch) {
            was_seen = false;
            branch = mappend(
                    (void**)&seen->branches,
                    sizeof(struct test_seen_branch),
                    &seen->branch_count,
                    &seen->branch_capacity);
            branch->define = v;
            branch->branch = (test_seen_t){NULL, 0, 0};
        }

        seen = &branch->branch;
    }

    return was_seen;
}

void test_seen_cleanup(test_seen_t *seen) {
    for (size_t i = 0; i < seen->branch_count; i++) {
        test_seen_cleanup(&seen->branches[i].branch);
    }
    free(seen->branches);
}

static void run_powerloss_none(
        const test_powerloss_t *powerloss,
        const struct test_suite *suite,
        const struct test_case *case_);
static void run_powerloss_cycles(
        const test_powerloss_t *powerloss,
        const struct test_suite *suite,
        const struct test_case *case_);

// iterate through permutations in a test case
static void case_forperm(
        const test_id_t *id,
        const struct test_suite *suite,
        const struct test_case *case_,
        void (*cb)(
            void *data,
            const struct test_suite *suite,
            const struct test_case *case_,
            const test_powerloss_t *powerloss),
        void *data) {
    // explicit permutation?
    if (id && id->defines) {
        // define case permutation, the exact case perm doesn't matter here
        test_define_case(id, suite, case_, 0);

        size_t permutations = test_define_permutations();
        for (size_t p = 0; p < permutations; p++) {
            // define permutation permutation
            test_define_permutation(p);

            // explicit powerloss cycles?
            if (id && id->powerloss.run) {
                cb(data, suite, case_, &id->powerloss);
            } else {
                for (size_t p = 0; p < test_powerloss_count; p++) {
                    // skip non-reentrant tests when powerloss testing
                    if (test_powerlosses[p].run != run_powerloss_none
                            && !(case_->flags & TEST_REENTRANT)) {
                        continue;
                    }

                    cb(data, suite, case_, &test_powerlosses[p]);
                }
            }
        }

        return;
    }

    // deduplicate permutations with the same defines
    //
    // this can easily happen when overriding multiple case permutations,
    // we can't tell that multiple case permutations don't change defines,
    // duplicating results
    test_seen_t seen = {NULL, 0, 0};

    for (size_t k = 0;
            k < ((case_->permutations) ? case_->permutations : 1);
            k++) {
        // define case permutation
        test_define_case(id, suite, case_, k);

        size_t permutations = test_define_permutations();
        for (size_t p = 0; p < permutations; p++) {
            // define permutation permutation
            test_define_permutation(p);

            // have we seen this permutation before?
            bool was_seen = test_seen_insert(&seen);
            if (!(k == 0 && p == 0) && was_seen) {
                continue;
            }

            // explicit powerloss cycles?
            if (id && id->powerloss.run) {
                cb(data, suite, case_, &id->powerloss);
            } else {
                for (size_t p = 0; p < test_powerloss_count; p++) {
                    // skip non-reentrant tests when powerloss testing
                    if (test_powerlosses[p].run != run_powerloss_none
                            && !(case_->flags & TEST_REENTRANT)) {
                        continue;
                    }

                    cb(data, suite, case_, &test_powerlosses[p]);
                }
            }
        }
    }

    test_seen_cleanup(&seen);
}


// how many permutations are there actually in a test case
struct perm_count_state {
    size_t total;
    size_t filtered;
};

void perm_count(
        void *data,
        const struct test_suite *suite,
        const struct test_case *case_,
        const test_powerloss_t *powerloss) {
    struct perm_count_state *state = data;
    (void)suite;

    state->total += 1;

    // set pls to 1 if running under powerloss so it useful for if predicates
    TEST_PLS = (powerloss->run != run_powerloss_none);
    if (!case_->run || !(test_all || !case_->if_ || case_->if_())) {
        return;
    }

    state->filtered += 1;
}


// operations we can do
static void summary(void) {
    printf("%-23s  %7s %7s %7s %15s\n",
            "", "flags", "suites", "cases", "perms");
    size_t suites = 0;
    size_t cases = 0;
    test_flags_t flags = 0;
    struct perm_count_state perms = {0, 0};

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            test_define_suite(&test_ids[t], test_suites[i]);

            size_t cases_ = 0;

            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
                cases_ += 1;
                case_forperm(
                        &test_ids[t],
                        test_suites[i],
                        &test_suites[i]->cases[j],
                        perm_count,
                        &perms);
            }

            // no tests found?
            if (!cases_) {
                continue;
            }

            suites += 1;
            flags |= test_suites[i]->flags;
        }
    }

    char perm_buf[64];
    sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
    char flag_buf[64];
    sprintf(flag_buf, "%s%s%s%s",
            (flags & TEST_INTERNAL)  ? "i" : "",
            (flags & TEST_REENTRANT) ? "r" : "",
            (flags & TEST_FUZZ)      ? "f" : "",
            (!flags)                 ? "-" : "");
    printf("%-23s  %7s %7zu %7zu %15s\n",
            "TOTAL",
            flag_buf,
            suites,
            cases,
            perm_buf);
}

static void list_suites(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < test_suite_count; i++) {
        size_t len = strlen(test_suites[i]->name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %7s %7s %15s\n",
            name_width, "suite", "flags", "cases", "perms");
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            test_define_suite(&test_ids[t], test_suites[i]);

            size_t cases = 0;
            struct perm_count_state perms = {0, 0};

            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
                case_forperm(
                        &test_ids[t],
                        test_suites[i],
                        &test_suites[i]->cases[j],
                        perm_count,
                        &perms);
            }

            // no tests found?
            if (!cases) {
                continue;
            }

            char perm_buf[64];
            sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
            char flag_buf[64];
            sprintf(flag_buf, "%s%s%s%s",
                    (test_suites[i]->flags & TEST_INTERNAL)  ? "i" : "",
                    (test_suites[i]->flags & TEST_REENTRANT) ? "r" : "",
                    (test_suites[i]->flags & TEST_FUZZ)      ? "f" : "",
                    (!test_suites[i]->flags)                 ? "-" : "");
            printf("%-*s  %7s %7zu %15s\n",
                    name_width,
                    test_suites[i]->name,
                    flag_buf,
                    cases,
                    perm_buf);
        }
    }
}

static void list_cases(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < test_suite_count; i++) {
        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            size_t len = strlen(test_suites[i]->cases[j].name);
            if (len > name_width) {
                name_width = len;
            }
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %7s %15s\n", name_width, "case", "flags", "perms");
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            test_define_suite(&test_ids[t], test_suites[i]);

            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                struct perm_count_state perms = {0, 0};
                case_forperm(
                        &test_ids[t],
                        test_suites[i],
                        &test_suites[i]->cases[j],
                        perm_count,
                        &perms);

                char perm_buf[64];
                sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
                char flag_buf[64];
                sprintf(flag_buf, "%s%s%s%s",
                        (test_suites[i]->cases[j].flags & TEST_INTERNAL)
                            ? "i" : "",
                        (test_suites[i]->cases[j].flags & TEST_REENTRANT)
                            ? "r" : "",
                        (test_suites[i]->cases[j].flags & TEST_FUZZ)
                            ? "f" : "",
                        (!test_suites[i]->cases[j].flags)
                            ? "-" : "");
                printf("%-*s  %7s %15s\n",
                        name_width,
                        test_suites[i]->cases[j].name,
                        flag_buf,
                        perm_buf);
            }
        }
    }
}

static void list_suite_paths(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < test_suite_count; i++) {
        size_t len = strlen(test_suites[i]->name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "suite", "path");
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            size_t cases = 0;

            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
            }

            // no tests found?
            if (!cases) {
                continue;
            }

            printf("%-*s  %s\n",
                    name_width,
                    test_suites[i]->name,
                    test_suites[i]->path);
        }
    }
}

static void list_case_paths(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < test_suite_count; i++) {
        for (size_t j = 0; j < test_suites[i]->case_count; j++) {
            size_t len = strlen(test_suites[i]->cases[j].name);
            if (len > name_width) {
                name_width = len;
            }
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "case", "path");
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                printf("%-*s  %s\n",
                        name_width,
                        test_suites[i]->cases[j].name,
                        test_suites[i]->cases[j].path);
            }
        }
    }
}

struct list_defines_define {
    const char *name;
    intmax_t *values;
    size_t value_count;
    size_t value_capacity;
};

struct list_defines_defines {
    struct list_defines_define *defines;
    size_t define_count;
    size_t define_capacity;
};

static void list_defines_add(
        struct list_defines_defines *defines,
        const test_define_t *define) {
    const char *name = define->name;
    intmax_t v = *define->define;

    // define already in defines?
    for (size_t i = 0; i < defines->define_count; i++) {
        if (strcmp(defines->defines[i].name, name) == 0) {
            // value already in values?
            for (size_t j = 0; j < defines->defines[i].value_count; j++) {
                if (defines->defines[i].values[j] == v) {
                    return;
                }
            }

            *(intmax_t*)mappend(
                (void**)&defines->defines[i].values,
                sizeof(intmax_t),
                &defines->defines[i].value_count,
                &defines->defines[i].value_capacity) = v;

            return;
        }
    }

    // new define?
    struct list_defines_define *define_ = mappend(
            (void**)&defines->defines,
            sizeof(struct list_defines_define),
            &defines->define_count,
            &defines->define_capacity);
    define_->name = name;
    define_->values = malloc(sizeof(intmax_t));
    define_->values[0] = v;
    define_->value_count = 1;
    define_->value_capacity = 1;
}

void perm_list_defines(
        void *data,
        const struct test_suite *suite,
        const struct test_case *case_,
        const test_powerloss_t *powerloss) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;
    (void)powerloss;

    // collect defines
    for (size_t d = 0; d < test_define_count; d++) {
        if (test_define_isdefined(test_defines[d])) {
            list_defines_add(defines, test_defines[d]);
        }
    }
}

void perm_list_permutation_defines(
        void *data,
        const struct test_suite *suite,
        const struct test_case *case_,
        const test_powerloss_t *powerloss) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;
    (void)powerloss;

    // collect permutation_defines
    for (size_t d = 0; d < test_define_count; d++) {
        if (test_define_ispermutation(test_defines[d])) {
            list_defines_add(defines, test_defines[d]);
        }
    }
}

static void list_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add defines
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            test_define_suite(&test_ids[t], test_suites[i]);

            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &test_ids[t],
                        test_suites[i],
                        &test_suites[i]->cases[j],
                        perm_list_defines,
                        &defines);
            }
        }
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        printf("%s=", defines.defines[i].name);
        for (size_t j = 0; j < defines.defines[i].value_count; j++) {
            printf("%jd", defines.defines[i].values[j]);
            if (j != defines.defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        free(defines.defines[i].values);
    }
    free(defines.defines);
}

static void list_permutation_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add permutation defines
    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            test_define_suite(&test_ids[t], test_suites[i]);

            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &test_ids[t],
                        test_suites[i],
                        &test_suites[i]->cases[j],
                        perm_list_permutation_defines,
                        &defines);
            }
        }
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        printf("%s=", defines.defines[i].name);
        for (size_t j = 0; j < defines.defines[i].value_count; j++) {
            printf("%jd", defines.defines[i].values[j]);
            if (j != defines.defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        free(defines.defines[i].values);
    }
    free(defines.defines);
}

static void list_implicit_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // yes we do need to define a suite/case, these do a bit of bookeeping
    // around mapping defines
    test_define_suite(NULL,
            &(const struct test_suite){0});
    test_define_case(NULL,
            &(const struct test_suite){0},
            &(const struct test_case){0},
            0);

    size_t permutations = test_define_permutations();
    for (size_t p = 0; p < permutations; p++) {
        // define permutation permutation
        test_define_permutation(p);

        // add implicit defines
        for (size_t d = 0; d < test_define_count; d++) {
            list_defines_add(&defines, test_defines[d]);
        }
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        printf("%s=", defines.defines[i].name);
        for (size_t j = 0; j < defines.defines[i].value_count; j++) {
            printf("%jd", defines.defines[i].values[j]);
            if (j != defines.defines[i].value_count-1) {
                printf(",");
            }
        }
        printf("\n");
    }

    for (size_t i = 0; i < defines.define_count; i++) {
        free(defines.defines[i].values);
    }
    free(defines.defines);
}



// scenarios to run tests under power-loss

static void run_powerloss_none(
        const test_powerloss_t *powerloss,
        const struct test_suite *suite,
        const struct test_case *case_) {
    (void)powerloss;

    // create block device and configuration
    lfs3_emubd_t bd;

    struct lfs3_cfg cfg = {
        .context            = &bd,
        .read               = lfs3_emubd_read,
        .prog               = lfs3_emubd_prog,
        .erase              = lfs3_emubd_erase,
        .sync               = lfs3_emubd_sync,
        TEST_CFG
    };

    struct lfs3_emubd_cfg bdcfg = {
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        TEST_BDCFG
    };

    int err = lfs3_emubd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test
    printf("running ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    // zero pls
    TEST_PLS = 0;

    case_->run(&cfg);

    printf("finished ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    // cleanup
    err = lfs3_emubd_destroy(&cfg);
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
        const test_powerloss_t *powerloss,
        const struct test_suite *suite,
        const struct test_case *case_) {
    // zero pls
    TEST_PLS = 0;

    // create block device and configuration
    lfs3_emubd_t bd;
    jmp_buf powerloss_jmp;

    struct lfs3_cfg cfg = {
        .context            = &bd,
        .read               = lfs3_emubd_read,
        .prog               = lfs3_emubd_prog,
        .erase              = lfs3_emubd_erase,
        .sync               = lfs3_emubd_sync,
        TEST_CFG
    };

    struct lfs3_emubd_cfg bdcfg = {
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .power_cycles       = (TEST_PLS < powerloss->cycle_count)
                ? TEST_PLS+1
                : 0,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
        TEST_BDCFG
    };

    int err = lfs3_emubd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            // run the test
            case_->run(&cfg);
            break;
        }

        // power-loss!
        printf("powerloss ");
        perm_printid(suite, case_, NULL, 0);
        printf(":x");
        leb16_print(TEST_PLS+1);
        printf("\n");

        // increment pls
        TEST_PLS += 1;
        lfs3_emubd_setpowercycles(&cfg, (TEST_PLS < powerloss->cycle_count)
                ? TEST_PLS+1
                : 0);
    }

    printf("finished ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    // cleanup
    err = lfs3_emubd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

static void run_powerloss_log(
        const test_powerloss_t *powerloss,
        const struct test_suite *suite,
        const struct test_case *case_) {
    // zero pls
    TEST_PLS = 0;

    // create block device and configuration
    lfs3_emubd_t bd;
    jmp_buf powerloss_jmp;

    struct lfs3_cfg cfg = {
        .context            = &bd,
        .read               = lfs3_emubd_read,
        .prog               = lfs3_emubd_prog,
        .erase              = lfs3_emubd_erase,
        .sync               = lfs3_emubd_sync,
        TEST_CFG
    };

    struct lfs3_emubd_cfg bdcfg = {
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .power_cycles       = (TEST_PLS < powerloss->cycle_count)
                ? 1 << TEST_PLS
                : 0,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
        TEST_BDCFG
    };

    int err = lfs3_emubd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            // run the test
            case_->run(&cfg);
            break;
        }

        // power-loss!
        printf("powerloss ");
        perm_printid(suite, case_, NULL, 0);
        printf(":y");
        leb16_print(TEST_PLS+1);
        printf("\n");

        // increment pls
        TEST_PLS += 1;
        lfs3_emubd_setpowercycles(&cfg, (TEST_PLS < powerloss->cycle_count)
                ? 1 << TEST_PLS
                : 0);
    }

    printf("finished ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    // cleanup
    err = lfs3_emubd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

static void run_powerloss_cycles(
        const test_powerloss_t *powerloss,
        const struct test_suite *suite,
        const struct test_case *case_) {
    // zero pls
    TEST_PLS = 0;

    // create block device and configuration
    lfs3_emubd_t bd;
    jmp_buf powerloss_jmp;

    struct lfs3_cfg cfg = {
        .context            = &bd,
        .read               = lfs3_emubd_read,
        .prog               = lfs3_emubd_prog,
        .erase              = lfs3_emubd_erase,
        .sync               = lfs3_emubd_sync,
        TEST_CFG
    };

    struct lfs3_emubd_cfg bdcfg = {
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .power_cycles       = (TEST_PLS < powerloss->cycle_count)
                ? powerloss->cycles[TEST_PLS]
                : 0,
        .powerloss_cb       = powerloss_longjmp,
        .powerloss_data     = &powerloss_jmp,
        TEST_BDCFG
    };

    int err = lfs3_emubd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    while (true) {
        if (!setjmp(powerloss_jmp)) {
            // run the test
            case_->run(&cfg);
            break;
        }

        // power-loss!
        assert(TEST_PLS <= powerloss->cycle_count);
        printf("powerloss ");
        perm_printid(suite, case_, powerloss->cycles, TEST_PLS+1);
        printf("\n");

        // increment pls
        TEST_PLS += 1;
        lfs3_emubd_setpowercycles(&cfg, (TEST_PLS < powerloss->cycle_count)
                ? powerloss->cycles[TEST_PLS]
                : 0);
    }

    printf("finished ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    // cleanup
    err = lfs3_emubd_destroy(&cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }
}

struct powerloss_exhaustive_state {
    struct lfs3_cfg *cfg;

    lfs3_emubd_t *branches;
    size_t branch_count;
    size_t branch_capacity;
};

struct powerloss_exhaustive_cycles {
    lfs3_emubd_powercycles_t *cycles;
    size_t cycle_count;
    size_t cycle_capacity;
};

static void powerloss_exhaustive_branch(void *c) {
    struct powerloss_exhaustive_state *state = c;
    // append to branches
    lfs3_emubd_t *branch = mappend(
            (void**)&state->branches,
            sizeof(lfs3_emubd_t),
            &state->branch_count,
            &state->branch_capacity);
    if (!branch) {
        fprintf(stderr, "error: exhaustive: out of memory\n");
        exit(-1);
    }

    // create copy-on-write copy
    int err = lfs3_emubd_cpy(state->cfg, branch);
    if (err) {
        fprintf(stderr, "error: exhaustive: could not create bd copy\n");
        exit(-1);
    }

    // also trigger on next power cycle
    lfs3_emubd_setpowercycles(state->cfg, 1);
}

static void run_powerloss_exhaustive_layer(
        struct powerloss_exhaustive_cycles *cycles,
        const struct test_suite *suite,
        const struct test_case *case_,
        struct lfs3_cfg *cfg,
        struct lfs3_emubd_cfg *bdcfg,
        size_t depth,
        size_t pls) {
    struct powerloss_exhaustive_state state = {
        .cfg = cfg,
        .branches = NULL,
        .branch_count = 0,
        .branch_capacity = 0,
    };

    // make the number of pls currently seen available to tests/debugging
    TEST_PLS = pls;

    // run through the test without additional powerlosses, collecting possible
    // branches as we do so
    lfs3_emubd_setpowercycles(state.cfg, (depth > 0) ? 1 : 0);
    bdcfg->powerloss_data = &state;

    // run the tests
    case_->run(cfg);

    // aggressively clean up memory here to try to keep our memory usage low
    int err = lfs3_emubd_destroy(cfg);
    if (err) {
        fprintf(stderr, "error: could not destroy block device: %d\n", err);
        exit(-1);
    }

    // recurse into each branch
    for (size_t i = 0; i < state.branch_count; i++) {
        // first push and print the branch
        lfs3_emubd_powercycles_t *cycle = mappend(
                (void**)&cycles->cycles,
                sizeof(lfs3_emubd_powercycles_t),
                &cycles->cycle_count,
                &cycles->cycle_capacity);
        if (!cycle) {
            fprintf(stderr, "error: exhaustive: out of memory\n");
            exit(-1);
        }
        *cycle = i+1;

        printf("powerloss ");
        perm_printid(suite, case_, cycles->cycles, cycles->cycle_count);
        printf("\n");

        // now recurse
        cfg->context = &state.branches[i];
        run_powerloss_exhaustive_layer(cycles,
                suite, case_,
                cfg, bdcfg, depth-1, pls+1);

        // pop the cycle
        cycles->cycle_count -= 1;
    }

    // clean up memory
    free(state.branches);
}

static void run_powerloss_exhaustive(
        const test_powerloss_t *powerloss,
        const struct test_suite *suite,
        const struct test_case *case_) {
    // create block device and configuration
    lfs3_emubd_t bd;

    struct lfs3_cfg cfg = {
        .context            = &bd,
        .read               = lfs3_emubd_read,
        .prog               = lfs3_emubd_prog,
        .erase              = lfs3_emubd_erase,
        .sync               = lfs3_emubd_sync,
        TEST_CFG
    };

    struct lfs3_emubd_cfg bdcfg = {
        .disk_path          = test_disk_path,
        .read_sleep         = test_read_sleep,
        .prog_sleep         = test_prog_sleep,
        .erase_sleep        = test_erase_sleep,
        .powerloss_cb       = powerloss_exhaustive_branch,
        .powerloss_data     = NULL,
        TEST_BDCFG
    };

    int err = lfs3_emubd_createcfg(&cfg, test_disk_path, &bdcfg);
    if (err) {
        fprintf(stderr, "error: could not create block device: %d\n", err);
        exit(-1);
    }

    // run the test, increasing power-cycles as power-loss events occur
    printf("running ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");

    // recursively exhaust each layer of powerlosses
    run_powerloss_exhaustive_layer(
            &(struct powerloss_exhaustive_cycles){NULL, 0, 0},
            suite, case_,
            &cfg, &bdcfg, powerloss->cycle_count, 0);

    printf("finished ");
    perm_printid(suite, case_, NULL, 0);
    printf("\n");
}


const test_powerloss_t builtin_powerlosses[] = {
    {"none",       run_powerloss_none,       NULL, 0},
    {"log",        run_powerloss_log,        NULL, SIZE_MAX},
    {"linear",     run_powerloss_linear,     NULL, SIZE_MAX},
    {"exhaustive", run_powerloss_exhaustive, NULL, SIZE_MAX},
    {NULL, NULL, NULL, 0},
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

// default to -Pnone,linear, which provides a good heuristic while still
// running quickly
const test_powerloss_t *test_powerlosses = (const test_powerloss_t[]){
    {"none",   run_powerloss_none,   NULL, 0},
    {"linear", run_powerloss_linear, NULL, SIZE_MAX},
};
size_t test_powerloss_count = 2;

static void list_powerlosses(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; builtin_powerlosses[i].name; i++) {
        size_t len = strlen(builtin_powerlosses[i].name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s %s\n", name_width, "scenario", "description");
    size_t i = 0;
    for (; builtin_powerlosses[i].name; i++) {
        printf("%-*s %s\n",
                name_width,
                builtin_powerlosses[i].name,
                builtin_powerlosses_help[i]);
    }

    // a couple more options with special parsing
    printf("%-*s %s\n", name_width, "1,2,3",   builtin_powerlosses_help[i+0]);
    printf("%-*s %s\n", name_width, "{1,2,3}", builtin_powerlosses_help[i+1]);
    printf("%-*s %s\n", name_width, ":1248g1", builtin_powerlosses_help[i+2]);
}


// global test step count
size_t test_step = 0;

void perm_run(
        void *data,
        const struct test_suite *suite,
        const struct test_case *case_,
        const test_powerloss_t *powerloss) {
    (void)data;

    // skip this step?
    if (!(test_step >= test_step_start
            && test_step < test_step_stop
            && (test_step-test_step_start) % test_step_step == 0)) {
        test_step += 1;
        return;
    }
    test_step += 1;

    // set pls to 1 if running under powerloss so it useful for if predicates
    TEST_PLS = (powerloss->run != run_powerloss_none);
    // filter?
    if (!case_->run || !(test_all || !case_->if_ || case_->if_())) {
        printf("skipped ");
        perm_printid(suite, case_, NULL, 0);
        printf("\n");
        return;
    }

    // run the test, possibly under powerloss
    powerloss->run(powerloss, suite, case_);
}

static void run(void) {
    // ignore disconnected pipes
    signal(SIGPIPE, SIG_IGN);

    for (size_t t = 0; t < test_id_count; t++) {
        for (size_t i = 0; i < test_suite_count; i++) {
            test_define_suite(&test_ids[t], test_suites[i]);

            for (size_t j = 0; j < test_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (test_ids[t].name && !(
                        strcmp(test_ids[t].name,
                            test_suites[i]->name) == 0
                        || strcmp(test_ids[t].name,
                            test_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &test_ids[t],
                        test_suites[i],
                        &test_suites[i]->cases[j],
                        perm_run,
                        NULL);
            }
        }
    }
}



// option handling
enum opt_flags {
    OPT_HELP                     = 'h',
    OPT_SUMMARY                  = 'Y',
    OPT_LIST_SUITES              = 'l',
    OPT_LIST_CASES               = 'L',
    OPT_LIST_SUITE_PATHS         = 1,
    OPT_LIST_CASE_PATHS          = 2,
    OPT_LIST_DEFINES             = 3,
    OPT_LIST_PERMUTATION_DEFINES = 4,
    OPT_LIST_IMPLICIT_DEFINES    = 5,
    OPT_LIST_POWERLOSSES         = 6,
    OPT_DEFINE                   = 'D',
    OPT_DEFINE_DEPTH             = 7,
    OPT_POWERLOSS                = 'P',
    OPT_STEP                     = 's',
    OPT_ALL                      = 'a',
    OPT_DISK                     = 'd',
    OPT_TRACE                    = 't',
    OPT_TRACE_BACKTRACE          = 8,
    OPT_TRACE_PERIOD             = 9,
    OPT_TRACE_FREQ               = 10,
    OPT_READ_SLEEP               = 11,
    OPT_PROG_SLEEP               = 12,
    OPT_ERASE_SLEEP              = 13,
};

const char *short_opts = "hYlLD:P:s:ad:t:";

const struct option long_opts[] = {
    {"help",             no_argument,       NULL, OPT_HELP},
    {"summary",          no_argument,       NULL, OPT_SUMMARY},
    {"list-suites",      no_argument,       NULL, OPT_LIST_SUITES},
    {"list-cases",       no_argument,       NULL, OPT_LIST_CASES},
    {"list-suite-paths", no_argument,       NULL, OPT_LIST_SUITE_PATHS},
    {"list-case-paths",  no_argument,       NULL, OPT_LIST_CASE_PATHS},
    {"list-defines",     no_argument,       NULL, OPT_LIST_DEFINES},
    {"list-permutation-defines",
                         no_argument,       NULL, OPT_LIST_PERMUTATION_DEFINES},
    {"list-implicit-defines",
                         no_argument,       NULL, OPT_LIST_IMPLICIT_DEFINES},
    {"list-powerlosses", no_argument,       NULL, OPT_LIST_POWERLOSSES},
    {"define",           required_argument, NULL, OPT_DEFINE},
    {"define-depth",     required_argument, NULL, OPT_DEFINE_DEPTH},
    {"powerloss",        required_argument, NULL, OPT_POWERLOSS},
    {"step",             required_argument, NULL, OPT_STEP},
    {"all",              no_argument,       NULL, OPT_ALL},
    {"disk",             required_argument, NULL, OPT_DISK},
    {"trace",            required_argument, NULL, OPT_TRACE},
    {"trace-backtrace",  no_argument,       NULL, OPT_TRACE_BACKTRACE},
    {"trace-period",     required_argument, NULL, OPT_TRACE_PERIOD},
    {"trace-freq",       required_argument, NULL, OPT_TRACE_FREQ},
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
    "List explicit defines in this test-runner.",
    "List implicit defines in this test-runner.",
    "List the available power-loss scenarios.",
    "Override a test define.",
    "How deep to evaluate recursive defines before erroring.",
    "Comma-separated list of power-loss scenarios to test.",
    "Comma-separated range of permutations to run.",
    "Ignore test filters.",
    "Direct block device operations to this file.",
    "Direct trace output to this file.",
    "Include a backtrace with every trace statement.",
    "Sample trace output at this period in cycles.",
    "Sample trace output at this frequency in hz.",
    "Artificial read delay in seconds.",
    "Artificial prog delay in seconds.",
    "Artificial erase delay in seconds.",
};

int main(int argc, char **argv) {
    void (*op)(void) = run;

    size_t test_override_define_capacity = 0;
    size_t test_powerloss_capacity = 0;
    size_t test_id_capacity = 0;

    // parse options
    while (true) {
        int c = getopt_long(argc, argv, short_opts, long_opts, NULL);
        switch (c) {
        // generate help message
        case OPT_HELP:;
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

        // summary/list flags
        case OPT_SUMMARY:;
            op = summary;
            break;

        case OPT_LIST_SUITES:;
            op = list_suites;
            break;

        case OPT_LIST_CASES:;
            op = list_cases;
            break;

        case OPT_LIST_SUITE_PATHS:;
            op = list_suite_paths;
            break;

        case OPT_LIST_CASE_PATHS:;
            op = list_case_paths;
            break;

        case OPT_LIST_DEFINES:;
            op = list_defines;
            break;

        case OPT_LIST_PERMUTATION_DEFINES:;
            op = list_permutation_defines;
            break;

        case OPT_LIST_IMPLICIT_DEFINES:;
            op = list_implicit_defines;
            break;

        case OPT_LIST_POWERLOSSES:;
            op = list_powerlosses;
            break;

        // configuration
        case OPT_DEFINE:;
            // allocate space
            test_define_t *override = mappend(
                    (void**)&test_override_defines,
                    sizeof(test_define_t),
                    &test_override_define_count,
                    &test_override_define_capacity);

            // parse into string key/intmax_t value, cannibalizing the
            // arg in the process
            char *sep = strchr(optarg, '=');
            char *parsed = NULL;
            if (!sep) {
                goto invalid_define;
            }
            *sep = '\0';
            override->name = optarg;
            optarg = sep+1;

            // parse comma-separated permutations
            {
                test_override_value_t *override_values = NULL;
                size_t override_value_count = 0;
                size_t override_value_capacity = 0;
                size_t override_permutations = 0;
                while (true) {
                    optarg += strspn(optarg, " ");

                    if (strncmp(optarg, "range", strlen("range")) == 0) {
                        // range of values
                        optarg += strlen("range");
                        optarg += strspn(optarg, " ");
                        if (*optarg != '(') {
                            goto invalid_define;
                        }
                        optarg += 1;

                        intmax_t start = strtoumax(optarg, &parsed, 0);
                        intmax_t stop = -1;
                        intmax_t step = 1;
                        // allow empty string for start=0
                        if (parsed == optarg) {
                            start = 0;
                        }
                        optarg = parsed + strspn(parsed, " ");

                        if (*optarg != ',' && *optarg != ')') {
                            goto invalid_define;
                        }

                        if (*optarg == ',') {
                            optarg += 1;
                            stop = strtoumax(optarg, &parsed, 0);
                            // allow empty string for stop=end
                            if (parsed == optarg) {
                                stop = -1;
                            }
                            optarg = parsed + strspn(parsed, " ");

                            if (*optarg != ',' && *optarg != ')') {
                                goto invalid_define;
                            }

                            if (*optarg == ',') {
                                optarg += 1;
                                step = strtoumax(optarg, &parsed, 0);
                                // allow empty string for stop=1
                                if (parsed == optarg) {
                                    step = 1;
                                }
                                optarg = parsed + strspn(parsed, " ");

                                if (*optarg != ')') {
                                    goto invalid_define;
                                }
                            }
                        } else {
                            // single value = stop only
                            stop = start;
                            start = 0;
                        }

                        if (*optarg != ')') {
                            goto invalid_define;
                        }
                        optarg += 1;

                        // append range
                        *(test_override_value_t*)mappend(
                                (void**)&override_values,
                                sizeof(test_override_value_t),
                                &override_value_count,
                                &override_value_capacity)
                                = (test_override_value_t){
                            .start = start,
                            .stop = stop,
                            .step = step,
                        };
                        if (step > 0) {
                            override_permutations += (stop-1 - start)
                                    / step + 1;
                        } else {
                            override_permutations += (start-1 - stop)
                                    / -step + 1;
                        }
                    } else if (*optarg != '\0') {
                        // single value
                        intmax_t define = strtoumax(optarg, &parsed, 0);
                        if (parsed == optarg) {
                            goto invalid_define;
                        }
                        optarg = parsed + strspn(parsed, " ");

                        // append value
                        *(test_override_value_t*)mappend(
                                (void**)&override_values,
                                sizeof(test_override_value_t),
                                &override_value_count,
                                &override_value_capacity)
                                = (test_override_value_t){
                            .start = define,
                            .step = 0,
                        };
                        override_permutations += 1;
                    } else {
                        break;
                    }

                    if (*optarg == ',') {
                        optarg += 1;
                    }
                }

                // define should be patched in test_define_suite
                override->define = NULL;
                override->cb = test_override_cb;
                override->data = malloc(sizeof(test_override_data_t));
                *(test_override_data_t*)override->data
                        = (test_override_data_t){
                    .values = override_values,
                    .value_count = override_value_count,
                };
                override->permutations = override_permutations;
            }
            break;

        invalid_define:;
            fprintf(stderr, "error: invalid define: %s\n", optarg);
            exit(-1);

        case OPT_DEFINE_DEPTH:;
            parsed = NULL;
            test_define_depth = strtoumax(optarg, &parsed, 0);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid define-depth: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_POWERLOSS:;
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
                for (size_t i = 0; builtin_powerlosses[i].name; i++) {
                    if (len == strlen(builtin_powerlosses[i].name)
                            && memcmp(optarg,
                                builtin_powerlosses[i].name,
                                len) == 0) {
                        *powerloss = builtin_powerlosses[i];
                        optarg += len;
                        goto powerloss_next;
                    }
                }

                // comma-separated permutation
                if (*optarg == '{') {
                    lfs3_emubd_powercycles_t *cycles = NULL;
                    size_t cycle_count = 0;
                    size_t cycle_capacity = 0;

                    char *s = optarg + 1;
                    while (true) {
                        parsed = NULL;
                        *(lfs3_emubd_powercycles_t*)mappend(
                                (void**)&cycles,
                                sizeof(lfs3_emubd_powercycles_t),
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
                            "explicit",
                            run_powerloss_cycles,
                            cycles,
                            cycle_count};
                    optarg = s;
                    goto powerloss_next;
                }

                // leb16-encoded permutation
                if (*optarg == ':') {
                    // special case for linear power cycles
                    if (optarg[1] == 'x') {
                        size_t cycle_count = leb16_parse(optarg+2, &optarg);

                        *powerloss = (test_powerloss_t){
                                "linear",
                                run_powerloss_linear,
                                NULL,
                                cycle_count};
                        goto powerloss_next;

                    // special case for log power cycles
                    } else if (optarg[1] == 'y') {
                        size_t cycle_count = leb16_parse(optarg+2, &optarg);

                        *powerloss = (test_powerloss_t){
                                "log",
                                run_powerloss_log,
                                NULL,
                                cycle_count};
                        goto powerloss_next;

                    // otherwise explicit power cycles
                    } else {
                        lfs3_emubd_powercycles_t *cycles = NULL;
                        size_t cycle_count = 0;
                        size_t cycle_capacity = 0;

                        char *s = optarg + 1;
                        while (true) {
                            parsed = NULL;
                            uintmax_t x = leb16_parse(s, &parsed);
                            if (parsed == s) {
                                break;
                            }

                            *(lfs3_emubd_powercycles_t*)mappend(
                                    (void**)&cycles,
                                    sizeof(lfs3_emubd_powercycles_t),
                                    &cycle_count,
                                    &cycle_capacity) = x;
                            s = parsed;
                        }

                        *powerloss = (test_powerloss_t){
                                "explicit",
                                run_powerloss_cycles,
                                cycles,
                                cycle_count};
                        optarg = s;
                        goto powerloss_next;
                    }
                }

                // exhaustive permutations
                {
                    parsed = NULL;
                    size_t count = strtoumax(optarg, &parsed, 0);
                    if (parsed == optarg) {
                        goto powerloss_unknown;
                    }
                    *powerloss = (test_powerloss_t){
                            "exhaustive",
                            run_powerloss_exhaustive,
                            NULL,
                            count};
                    optarg = (char*)parsed;
                    goto powerloss_next;
                }

            powerloss_unknown:;
                // unknown scenario?
                fprintf(stderr, "error: unknown power-loss scenario: %s\n",
                        optarg);
                exit(-1);

            powerloss_next:;
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

        case OPT_STEP:;
            parsed = NULL;
            test_step_start = strtoumax(optarg, &parsed, 0);
            test_step_stop = -1;
            test_step_step = 1;
            // allow empty string for start=0
            if (parsed == optarg) {
                test_step_start = 0;
            }
            optarg = parsed + strspn(parsed, " ");

            if (*optarg != ',' && *optarg != '\0') {
                goto step_unknown;
            }

            if (*optarg == ',') {
                optarg += 1;
                test_step_stop = strtoumax(optarg, &parsed, 0);
                // allow empty string for stop=end
                if (parsed == optarg) {
                    test_step_stop = -1;
                }
                optarg = parsed + strspn(parsed, " ");

                if (*optarg != ',' && *optarg != '\0') {
                    goto step_unknown;
                }

                if (*optarg == ',') {
                    optarg += 1;
                    test_step_step = strtoumax(optarg, &parsed, 0);
                    // allow empty string for stop=1
                    if (parsed == optarg) {
                        test_step_step = 1;
                    }
                    optarg = parsed + strspn(parsed, " ");

                    if (*optarg != '\0') {
                        goto step_unknown;
                    }
                }
            } else {
                // single value = stop only
                test_step_stop = test_step_start;
                test_step_start = 0;
            }
            break;

        step_unknown:;
            fprintf(stderr, "error: invalid step: %s\n", optarg);
            exit(-1);

        case OPT_ALL:;
            test_all = true;
            break;

        case OPT_DISK:;
            test_disk_path = optarg;
            break;

        case OPT_TRACE:;
            test_trace_path = optarg;
            break;

        case OPT_TRACE_BACKTRACE:;
            test_trace_backtrace = true;
            break;

        case OPT_TRACE_PERIOD:;
            parsed = NULL;
            test_trace_period = strtoumax(optarg, &parsed, 0);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid trace-period: %s\n",
                        optarg);
                exit(-1);
            }
            break;

        case OPT_TRACE_FREQ:;
            parsed = NULL;
            test_trace_freq = strtoumax(optarg, &parsed, 0);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid trace-freq: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_READ_SLEEP:;
            parsed = NULL;
            double read_sleep = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid read-sleep: %s\n", optarg);
                exit(-1);
            }
            test_read_sleep = read_sleep*1.0e9;
            break;

        case OPT_PROG_SLEEP:;
            parsed = NULL;
            double prog_sleep = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid prog-sleep: %s\n", optarg);
                exit(-1);
            }
            test_prog_sleep = prog_sleep*1.0e9;
            break;

        case OPT_ERASE_SLEEP:;
            parsed = NULL;
            double erase_sleep = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid erase-sleep: %s\n", optarg);
                exit(-1);
            }
            test_erase_sleep = erase_sleep*1.0e9;
            break;

        // done parsing
        case -1:;
            goto getopt_done;

        // unknown arg, getopt prints a message for us
        default:;
            exit(-1);
        }
    }
getopt_done:;

    if (argc > optind) {
        // reset our test identifier list
        test_ids = NULL;
        test_id_count = 0;
        test_id_capacity = 0;
    }

    // parse test identifier, if any, cannibalizing the arg in the process
    for (; argc > optind; optind++) {
        test_define_t *defines = NULL;
        size_t define_count = 0;
        test_powerloss_t powerloss = {NULL, NULL, NULL, 0};

        // parse name, can be suite or case
        char *name = argv[optind];
        char *defines_ = strchr(name, ':');
        if (defines_) {
            *defines_ = '\0';
            defines_ += 1;
        }

        // remove optional path and .toml suffix
        char *slash = strrchr(name, '/');
        if (slash) {
            name = slash+1;
        }

        size_t name_len = strlen(name);
        if (name_len > 5 && strcmp(&name[name_len-5], ".toml") == 0) {
            name[name_len-5] = '\0';
        }

        if (defines_) {
            // parse defines
            char *cycles_ = strchr(defines_, ':');
            if (cycles_) {
                *cycles_ = '\0';
                cycles_ += 1;
            }

            while (true) {
                char *parsed;
                size_t d = leb16_parse(defines_, &parsed);
                intmax_t v = leb16_parse(parsed, &parsed);
                if (parsed == defines_) {
                    break;
                }
                defines_ = parsed;

                if (d >= define_count) {
                    // align to power of two to avoid any superlinear growth
                    size_t ncount = 1 << lfs3_nlog2(d+1);
                    defines = realloc(defines,
                            ncount*sizeof(test_define_t));
                    memset(defines+define_count, 0,
                            (ncount-define_count)*sizeof(test_define_t));
                    define_count = ncount;
                }
                // name/define should be patched in test_define_suite
                defines[d] = TEST_LIT(NULL, v);
            }

            // special case for linear power cycles
            if (cycles_ && *cycles_ == 'x') {
                char *parsed = NULL;
                size_t cycle_count = leb16_parse(cycles_+1, &parsed);
                if (parsed == cycles_+1) {
                    fprintf(stderr, "error: "
                            "could not parse test cycles: %s\n",
                            cycles_);
                    exit(-1);
                }
                cycles_ = parsed;

                powerloss = (test_powerloss_t){
                        "linear",
                        run_powerloss_linear,
                        NULL,
                        cycle_count};

            // special case for log power cycles
            } else if (cycles_ && *cycles_ == 'y') {
                char *parsed = NULL;
                size_t cycle_count = leb16_parse(cycles_+1, &parsed);
                if (parsed == cycles_+1) {
                    fprintf(stderr, "error: "
                            "could not parse test cycles: %s\n",
                            cycles_);
                    exit(-1);
                }
                cycles_ = parsed;

                powerloss = (test_powerloss_t){
                        "log",
                        run_powerloss_log,
                        NULL,
                        cycle_count};

            // otherwise explicit power cycles
            } else if (cycles_) {
                // parse power cycles
                lfs3_emubd_powercycles_t *cycles = NULL;
                size_t cycle_count = 0;
                size_t cycle_capacity = 0;
                while (*cycles_ != '\0') {
                    char *parsed = NULL;
                    *(lfs3_emubd_powercycles_t*)mappend(
                            (void**)&cycles,
                            sizeof(lfs3_emubd_powercycles_t),
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

                powerloss = (test_powerloss_t){
                        "explicit",
                        run_powerloss_cycles,
                        cycles,
                        cycle_count};
            }
        }

        // append to identifier list
        *(test_id_t*)mappend(
                (void**)&test_ids,
                sizeof(test_id_t),
                &test_id_count,
                &test_id_capacity) = (test_id_t){
            .name = name,
            .defines = defines,
            .define_count = define_count,
            .powerloss = powerloss,
        };
    }

    // do the thing
    op();

    // cleanup (need to be done for valgrind testing)
    test_define_cleanup();
    if (test_override_defines) {
        for (size_t i = 0; i < test_override_define_count; i++) {
            free((void*)test_override_defines[i].data);
        }
        free((void*)test_override_defines);
    }
    if (test_powerloss_capacity) {
        for (size_t i = 0; i < test_powerloss_count; i++) {
            free((void*)test_powerlosses[i].cycles);
        }
        free((void*)test_powerlosses);
    }
    if (test_id_capacity) {
        for (size_t i = 0; i < test_id_count; i++) {
            free((void*)test_ids[i].defines);
            free((void*)test_ids[i].powerloss.cycles);
        }
        free((void*)test_ids);
    }
}
