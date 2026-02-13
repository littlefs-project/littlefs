/*
 * Runner for littlefs benchmarks
 *
 * Copyright (c) 2022, The littlefs authors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 199309L
#endif

#include "runners/bench_runner.h"

#include <getopt.h>
#include <sys/types.h>
#include <errno.h>
#include <setjmp.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <execinfo.h>
#include <signal.h>
#include <time.h>
#include <stddef.h>


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
static void leb16_print(intmax_t x) {
    // allow 'w' to indicate negative numbers
    if (x < 0) {
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

static intmax_t leb16_parse(const char *s, char **tail) {
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



// bench_runner types

typedef struct bench_id {
    const char *name;
    bench_define_t *defines;
    size_t define_count;
} bench_id_t;


// bench define management

// implicit defines declared here
#define BENCH_DEFINE(k, v) \
        intmax_t k;
    #include BENCH_STRINGIFY(BENCH_DEFINES)
#undef BENCH_DEFINE

#define BENCH_DEFINE(k, v) \
        intmax_t bench_define_##k(void *data, size_t i) { \
            (void)data; \
            (void)i; \
            return v; \
        }
    #include BENCH_STRINGIFY(BENCH_DEFINES)
#undef BENCH_DEFINE

const bench_define_t bench_implicit_defines[] = {
    #define BENCH_DEFINE(k, v) \
            {#k, &k, bench_define_##k, NULL, 1},
        #include BENCH_STRINGIFY(BENCH_DEFINES)
    #undef BENCH_DEFINE
};
const size_t bench_implicit_define_count
        = sizeof(bench_implicit_defines) / sizeof(bench_define_t);

// some helpers
intmax_t bench_define_lit(void *data, size_t i) {
    (void)i;
    return (intptr_t)data;
}

#define BENCH_LIT(name, v) ((bench_define_t){ \
    name, NULL, bench_define_lit, (void*)(uintptr_t)(v), 1})


// define mapping
const bench_define_t **bench_defines = NULL;
size_t bench_define_count = 0;
size_t bench_define_capacity = 0;

const bench_define_t **bench_suite_defines = NULL;
size_t bench_suite_define_count = 0;
ssize_t *bench_suite_define_map = NULL;

bench_define_t *bench_override_defines = NULL;
size_t bench_override_define_count = 0;
size_t bench_override_define_capacity = 0;

size_t bench_define_depth = 1000;


static inline bool bench_define_isdefined(const bench_define_t *define) {
    return define->cb;
}

static inline bool bench_define_ispermutation(const bench_define_t *define) {
    // permutation defines are basically anything that's not implicit
    return bench_define_isdefined(define)
            && !(define >= bench_implicit_defines
                && define
                    < bench_implicit_defines
                        + bench_implicit_define_count);
}


void bench_define_suite(
        const bench_id_t *id,
        const struct bench_suite *suite) {
    // reset our mapping
    bench_define_count = 0;
    bench_suite_define_count = 0;

    // make sure we have space for everything, just assume the worst case
    if (bench_implicit_define_count + suite->define_count
            > bench_define_capacity) {
        bench_define_capacity
                = bench_implicit_define_count + suite->define_count;
        bench_defines = realloc(
                bench_defines,
                bench_define_capacity*sizeof(const bench_define_t*));
        bench_suite_defines = realloc(
                bench_suite_defines,
                bench_define_capacity*sizeof(const bench_define_t*));
        bench_suite_define_map = realloc(
                bench_suite_define_map,
                bench_define_capacity*sizeof(ssize_t));
    }

    // first map our implicit defines
    for (size_t i = 0; i < bench_implicit_define_count; i++) {
        bench_suite_defines[i] = &bench_implicit_defines[i];
    }
    bench_suite_define_count = bench_implicit_define_count;

    // build a mapping from suite defines to bench defines
    //
    // we will use this for both suite and case defines
    memset(bench_suite_define_map, -1,
            bench_suite_define_count*sizeof(size_t));

    for (size_t i = 0; i < suite->define_count; i++) {
        // assume suite defines are unique so we only need to compare
        // against implicit defines, this avoids a O(n^2)
        for (size_t j = 0; j < bench_implicit_define_count; j++) {
            if (bench_suite_defines[j]->define == suite->defines[i].define) {
                bench_suite_define_map[j] = i;

                // don't override implicit defines if we're not defined
                if (bench_define_isdefined(&suite->defines[i])) {
                    bench_suite_defines[j] = &suite->defines[i];
                }
                goto next_suite_define;
            }
        }

        // map a new suite define
        bench_suite_define_map[bench_suite_define_count] = i;
        bench_suite_defines[bench_suite_define_count] = &suite->defines[i];
        bench_suite_define_count += 1;
next_suite_define:;
    }

    // map any explicit defines
    //
    // we ignore any out-of-bounds defines here, even though it's likely
    // an error
    if (id && id->defines) {
        for (size_t i = 0;
                i < id->define_count && i < bench_suite_define_count;
                i++) {
            if (bench_define_isdefined(&id->defines[i])) {
                // update name/addr
                id->defines[i].name = bench_suite_defines[i]->name;
                id->defines[i].define = bench_suite_defines[i]->define;
                // map and override suite mapping
                bench_suite_defines[i] = &id->defines[i];
                bench_suite_define_map[i] = -1;
            }
        }
    }

    // map any override defines
    //
    // note it's not an error to override a define that doesn't exist
    for (size_t i = 0; i < bench_override_define_count; i++) {
        for (size_t j = 0; j < bench_suite_define_count; j++) {
            if (strcmp(
                    bench_suite_defines[j]->name,
                    bench_override_defines[i].name) == 0) {
                // update addr
                bench_override_defines[i].define
                        = bench_suite_defines[j]->define;
                // map and override suite mapping
                bench_suite_defines[j] = &bench_override_defines[i];
                bench_suite_define_map[j] = -1;
                goto next_override_define;
            }
        }
next_override_define:;
    }
}

void bench_define_case(
        const bench_id_t *id,
        const struct bench_suite *suite,
        const struct bench_case *case_,
        size_t perm) {
    (void)id;

    // copy over suite defines
    for (size_t i = 0; i < bench_suite_define_count; i++) {
        // map case define if case define is defined
        if (case_->defines
                && bench_suite_define_map[i] != -1
                && bench_define_isdefined(&case_->defines[
                    perm*suite->define_count
                        + bench_suite_define_map[i]])) {
            bench_defines[i] = &case_->defines[
                    perm*suite->define_count
                        + bench_suite_define_map[i]];
        } else {
            bench_defines[i] = bench_suite_defines[i];
        }
    }
    bench_define_count = bench_suite_define_count;
}

void bench_define_permutation(size_t perm) {
    // first zero everything, we really don't want reproducibility issues
    for (size_t i = 0; i < bench_define_count; i++) {
        *bench_defines[i]->define = 0;
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
        const bench_define_t *changed = NULL;
        // define-specific permutations are encoded in the case permutation
        size_t perm_ = perm;
        for (size_t i = 0; i < bench_define_count; i++) {
            if (bench_defines[i]->cb) {
                intmax_t v = bench_defines[i]->cb(
                        bench_defines[i]->data,
                        perm_ % bench_defines[i]->permutations);
                if (v != *bench_defines[i]->define) {
                    *bench_defines[i]->define = v;
                    changed = bench_defines[i];
                }

                perm_ /= bench_defines[i]->permutations;
            }
        }

        // stabilized?
        if (!changed) {
            break;
        }

        attempt += 1;
        if (bench_define_depth && attempt >= bench_define_depth+1) {
            fprintf(stderr, "error: could not resolve recursive defines: %s\n",
                    changed->name);
            exit(-1);
        }
    }
}

void bench_define_cleanup(void) {
    // bench define management can allocate a few things
    free(bench_defines);
    free(bench_suite_defines);
    free(bench_suite_define_map);
}

size_t bench_define_permutations(void) {
    size_t prod = 1;
    for (size_t i = 0; i < bench_define_count; i++) {
        prod *= (bench_defines[i]->permutations > 0)
                ? bench_defines[i]->permutations
                : 1;
    }
    return prod;
}


// override define stuff

typedef struct bench_override_value {
    intmax_t start;
    intmax_t stop;
    // step == 0 indicates a single value
    intmax_t step;
} bench_override_value_t;

typedef struct bench_override_data {
    bench_override_value_t *values;
    size_t value_count;
} bench_override_data_t;

intmax_t bench_override_cb(void *data, size_t i) {
    const bench_override_data_t *data_ = data;
    for (size_t j = 0; j < data_->value_count; j++) {
        const bench_override_value_t *v = &data_->values[j];
        // range?
        if (v->step) {
            size_t range_count;
            if (v->step > 0) {
                range_count = (v->stop-1 - v->start) / +v->step + 1;
            } else {
                range_count = (v->start-1 - v->stop) / -v->step + 1;
            }

            if (i < range_count) {
                return v->start + i*v->step;
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



// bench state
const bench_id_t *bench_ids = (const bench_id_t[]) {
    {NULL, NULL, 0},
};
size_t bench_id_count = 1;
size_t bench_id_capacity = 0;

size_t bench_step_start = 0;
size_t bench_step_stop = -1;
size_t bench_step_step = 1;
size_t bench_steps = 0; // incremented every permutation
bool bench_force = false;
bench_flags_t bench_mask = 0;

const char *bench_disk_path = NULL;
const char *bench_trace_path = NULL;
bool bench_trace_backtrace = false;
size_t bench_trace_step = 0;
double bench_trace_runfreq = 0.0;
double bench_trace_simfreq = 0.0;
uint32_t bench_trace_paused = false;
FILE *bench_trace_file = NULL;
size_t bench_trace_steps = 0;
bench_ns_t bench_trace_runtime = 0;
bench_ns_t bench_trace_simtime = 0;
bench_ns_t bench_trace_open_runtime = 0;
bench_ns_t bench_read_sleep = 0.0;
bench_ns_t bench_prog_sleep = 0.0;
bench_ns_t bench_erase_sleep = 0.0;

// this determines both the backtrace buffer and the trace printf buffer, if
// trace ends up interleaved or truncated this may need to be increased
#ifndef BENCH_TRACE_BACKTRACE_BUFFER_SIZE
#define BENCH_TRACE_BACKTRACE_BUFFER_SIZE 8192
#endif
void *bench_trace_backtrace_buffer[
    BENCH_TRACE_BACKTRACE_BUFFER_SIZE / sizeof(void*)];

// trace printing
void bench_trace(const char *fmt, ...) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    if (!bench_trace_path || bench_trace_paused) {
        goto done;
    }

    // prevent accidental recursion
    BENCH_TRACE_PAUSE();

    // sample at a specific step?
    if (bench_trace_step) {
        if (bench_trace_steps % bench_trace_step != 0) {
            bench_trace_steps += 1;
            goto done_;
        }
        bench_trace_steps += 1;
    }

    // sample at a specific frequency?
    if (bench_trace_runfreq) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        bench_ns_t now = (bench_ns_t)t.tv_sec*1000*1000*1000
                + (bench_ns_t)t.tv_nsec;
        if (now - bench_trace_runtime
                < (bench_ns_t)((1000.0*1000.0*1000.0)
                    / bench_trace_runfreq)) {
            goto done_;
        }
        bench_trace_runtime = now;
    }

    // sample at a specific simulated frequency?
    if (bench_trace_simfreq) {
        bench_sns_t now = BENCH_SIMTIME();
        if (now < 0) {
            // I guess we shouldn't print anything until bench has
            // started
            goto done_;
        }
        if (now - bench_trace_simtime
                < (bench_ns_t)((1000.0*1000.0*1000.0)
                    / bench_trace_simfreq)) {
            goto done_;
        }
        bench_trace_simtime = now;
    }

    if (!bench_trace_file) {
        // Tracing output is heavy and trying to open every trace
        // call is slow, so we only try to open the trace file every
        // so often. Note this doesn't affect successfully opened files
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        bench_ns_t now = (bench_ns_t)t.tv_sec*1000*1000*1000
                + (bench_ns_t)t.tv_nsec;
        if (now - bench_trace_open_runtime < 100*1000*1000) {
            goto done_;
        }
        bench_trace_open_runtime = now;

        // try to open the trace file
        int fd;
        if (strcmp(bench_trace_path, "-") == 0) {
            fd = dup(1);
            if (fd < 0) {
                goto done_;
            }
        } else {
            fd = open(
                    bench_trace_path,
                    O_WRONLY | O_CREAT | O_APPEND | O_NONBLOCK,
                    0666);
            if (fd < 0) {
                goto done_;
            }
            int err = fcntl(fd, F_SETFL, O_WRONLY | O_CREAT | O_APPEND);
            assert(!err);
        }

        FILE *f = fdopen(fd, "a");
        assert(f);
        int err = setvbuf(f, NULL, _IOFBF,
                BENCH_TRACE_BACKTRACE_BUFFER_SIZE);
        assert(!err);
        bench_trace_file = f;
    }

    // print trace
    va_list va;
    va_start(va, fmt);
    int res = vfprintf(bench_trace_file, fmt, va);
    va_end(va);
    if (res < 0) {
        fclose(bench_trace_file);
        bench_trace_file = NULL;
        goto done_;
    }

    if (bench_trace_backtrace) {
        // print backtrace
        size_t count = backtrace(
                bench_trace_backtrace_buffer,
                BENCH_TRACE_BACKTRACE_BUFFER_SIZE);
        // note we skip our own stack frame
        for (size_t i = 1; i < count; i++) {
            res = fprintf(bench_trace_file, "\tat %p\n",
                    bench_trace_backtrace_buffer[i]);
            if (res < 0) {
                fclose(bench_trace_file);
                bench_trace_file = NULL;
                goto done_;
            }
        }
    }

    // flush immediately
    fflush(bench_trace_file);

done_:;
    BENCH_TRACE_RESUME();

done:;
    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
}

void bench_trace_pause(void) {
    bench_trace_paused += 1;
}

void bench_trace_resume(void) {
    assert(bench_trace_paused);
    bench_trace_paused -= 1;
}


// bench prng
uint32_t bench_prng(uint32_t *state) {
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

// bench factorial
size_t bench_factorial(size_t x) {
    size_t y = 1;
    for (size_t i = 2; i <= x; i++) {
        y *= i;
    }
    return y;
}

// bench array permutations
void bench_permutation(size_t i, uint32_t *buffer, size_t size) {
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


// stack hooks
#ifdef BENCH_STACK
uint32_t bench_stack_entered = false;
uint32_t bench_stack_paused = false;
uint8_t *bench_stack_entrance = NULL;
size_t bench_stack_watermark = 0;
#endif

// call me when entering/exiting a bench!
#ifdef BENCH_STACK
__attribute__((noinline))
void bench_stack_enter(void) {
    bench_stack_entered = true;
    bench_stack_paused = false;
    bench_stack_entrance = __builtin_frame_address(0);
    bench_stack_watermark = 0;
}
#endif

#ifdef BENCH_STACK
void bench_stack_exit(void) {
    assert(bench_stack_entered);
    bench_stack_entered = false;
}
#endif

#ifdef BENCH_STACK
void bench_stack_reset(void) {
    bench_stack_watermark = 0;
}
#endif

// call me when entering/exiting a bd op!
#ifdef BENCH_STACK
__attribute__((noinline))
void bench_stack_pause(void) {
    if (bench_stack_entered && !bench_stack_paused) {
        uint8_t *current = __builtin_frame_address(0);

        // keep track of the deepest stack
        ssize_t depth = current - bench_stack_entrance;
        if (depth < 0) {
            depth = -depth;
        }

        if ((size_t)depth > bench_stack_watermark) {
            bench_stack_watermark = depth;
        }
    }

    bench_stack_paused += 1;
}
#endif

#ifdef BENCH_STACK
void bench_stack_resume(void) {
    assert(bench_stack_paused);
    bench_stack_paused -= 1;
}
#endif

// get the current stack usage
//
// note the noinline here is important for forcing a new stack frame
#ifdef BENCH_STACK
__attribute__((noinline))
size_t bench_stack_current(void) {
    uint8_t *current = __builtin_frame_address(0);

    ssize_t depth = current - bench_stack_entrance;
    if (depth < 0) {
        depth = -depth;
    }

    return depth;
}
#endif


// heap hooks
#ifdef BENCH_HEAP
uint32_t bench_heap_entered = false;
uint32_t bench_heap_paused = false;
size_t bench_heap_current = 0;
size_t bench_heap_watermark = 0;
#endif

// call me when entering/exiting a bench!
#ifdef BENCH_HEAP
void bench_heap_enter(void) {
    bench_heap_entered = true;
    bench_heap_paused = false;
    bench_heap_current = 0;
    bench_heap_watermark = 0;
}
#endif

#ifdef BENCH_HEAP
void bench_heap_exit(void) {
    assert(bench_heap_entered);
    bench_heap_entered = false;
}
#endif

#ifdef BENCH_HEAP
void bench_heap_reset(void) {
    bench_heap_watermark = 0;
}
#endif

// call me when entering/exiting a bd op!
#ifdef BENCH_HEAP
void bench_heap_pause(void) {
    bench_heap_paused += 1;
}
#endif

#ifdef BENCH_HEAP
void bench_heap_resume(void) {
    assert(bench_heap_paused);
    bench_heap_paused -= 1;
}
#endif

#ifdef BENCH_HEAP
void bench_heap_inc(size_t size) {
    if (bench_heap_entered && !bench_heap_paused) {
        bench_heap_current += size;
        // keep track of the deepest heap
        if (bench_heap_current > bench_heap_watermark) {
            bench_heap_watermark = bench_heap_current;
        }
    }
}
#endif

#ifdef BENCH_HEAP
void bench_heap_dec(size_t size) {
    if (bench_heap_entered && !bench_heap_paused) {
        bench_heap_current -= lfs3_min(size, bench_heap_current);
    }
}
#endif

// __real_malloc stubs, gcc's --wrap wraps these over the original symbols
#ifdef BENCH_HEAP
extern void *__real_malloc(size_t size);
extern void __real_free(void *p);
extern void *__real_realloc(void *p, size_t size);
#endif

// the actual malloc hooks
//
// these only work if wrapped via gcc's --wrap
#ifdef BENCH_HEAP
void *__wrap_malloc(size_t size) {
    // prefix with allocation size, note we use uintptr_t to hopefully
    // keep things aligned
    uintptr_t *p_ = __real_malloc(sizeof(uintptr_t) + size);
    if (!p_) {
        return NULL;
    }

    BENCH_HEAP_INC(size);
    *p_ = size;
    return p_ + 1;
}
#endif

#ifdef BENCH_HEAP
void __wrap_free(void *p) {
    if (!p) {
        return;
    }

    uintptr_t *p_ = ((uintptr_t*)p) - 1;
    size_t size = *p_;
    BENCH_HEAP_DEC(size);

    __real_free(p_);
}
#endif

#ifdef BENCH_HEAP
void *__wrap_realloc(void *p, size_t size) {
    uintptr_t *p_;
    size_t old;
    if (p) {
        p_ = ((uintptr_t*)p) - 1;
        old = *p_;
    } else {
        p_ = NULL;
        old = 0;
    }

    assert(size != 0);
    p_ = __real_realloc(p_, sizeof(uintptr_t) + size);
    if (!p_) {
        return NULL;
    }

    BENCH_HEAP_DEC(old);
    BENCH_HEAP_INC(size);
    *p_ = size;
    return p_ + 1;
}
#endif


// rather than intercepting all of littlefs's log functions, just
// intercept all calls to printf at link-time
//
// note this is not a perfect solution as the call itself needs stack,
// which may already be allocated in the parent frame, and some of
// littlefs's debug statements get loooooong
//
// disabling logging at compile time may give you more accurate results
#if defined(BENCH_STACK) || defined(BENCH_HEAP)
extern int __real_vprintf(const char *fmt, va_list args);

int __wrap_printf(const char *fmt, ...) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    va_list args;
    va_start(args, fmt);
    int n = __real_vprintf(fmt, args);
    va_end(args);

    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
    return n;
}
#endif

#if defined(BENCH_STACK) || defined(BENCH_HEAP)
extern int __real_vprintf(const char *fmt, va_list args);

int __wrap_vprintf(const char *fmt, va_list args) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    int n = __real_vprintf(fmt, args);

    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
    return n;
}
#endif



// bench probe/recording state
typedef struct bench_probe {
    const char *name;
    size_t step;
    double runfreq;
    double simfreq;
} bench_probe_t;

#define BENCH_RECORD_IGNORED 0x01
#define BENCH_RECORD_STARTED 0x02
#define BENCH_RECORD_DIRTY   0x04
#define BENCH_RECORD_RESULT  0x10
#define BENCH_RECORD_FRESULT 0x20
#define BENCH_RECORD_SIMTIME 0x40

typedef struct bench_record {
    const char *probe;
    uint32_t flags;
    size_t step;
    double runfreq;
    double simfreq;
    size_t steps;
    bench_ns_t runtime; // time of last print
    bench_ns_t simtime;

    uintmax_t n;
    uintmax_t result;
    double fresult;
    bench_io_t cumul_reads; // cumulative results
    bench_io_t cumul_progs;
    bench_io_t cumul_erases;
    bench_io_t cumul_readed;
    bench_io_t cumul_progged;
    bench_io_t cumul_erased;
    bench_ns_t cumul_simtime;
    bench_io_t start_reads; // start of probe
    bench_io_t start_progs;
    bench_io_t start_erases;
    bench_io_t start_readed;
    bench_io_t start_progged;
    bench_io_t start_erased;
    bench_ns_t start_simtime;
} bench_record_t;

typedef struct bench_cache {
    const char *probe;
    size_t i;
} bench_cache_t;

bench_probe_t *bench_probes = NULL;
size_t bench_probe_count = 0;
size_t bench_probe_capacity = 0;
size_t bench_probe_step = 0;
double bench_probe_runfreq = 0.0;
double bench_probe_simfreq = 0.0;

const struct lfs3_cfg *bench_cfg = NULL;
bench_record_t *bench_records = NULL;
size_t bench_record_count = 0;
size_t bench_record_capacity = 0;

#define BENCH_CACHE_COUNT 64
bench_cache_t bench_cache[BENCH_CACHE_COUNT];

void bench_init(const struct lfs3_cfg *cfg) {
    bench_cfg = cfg;
    bench_record_count = 0;
    memset(bench_cache, 0, sizeof(bench_cache));
}

// needed in bench_deinit
void bench_print(bench_record_t *record);

void bench_deinit(const struct lfs3_cfg *cfg) {
    (void)cfg;
    bench_cfg = NULL;

    // print any dirty probes at least once at the end of the bench
    for (size_t i = 0; i < bench_record_count; i++) {
        if (bench_records[i].flags & BENCH_RECORD_DIRTY) {
            bench_print(&bench_records[i]);
        }
    }
}

bench_record_t *bench_find(const char *probe) {
    // cached?
    bench_cache_t *cache = &bench_cache[(size_t)probe % BENCH_CACHE_COUNT];
    if (cache->probe == probe) {
        return &bench_records[cache->i];
    }

    // find our record
    bench_record_t *record = NULL;
    for (size_t i = 0; i < bench_record_count; i++) {
        if (strcmp(bench_records[i].probe, probe) == 0) {
            record = &bench_records[i];
            break;
        }
    }

    // allocate a new record?
    if (!record) {
        record = mappend(
                (void**)&bench_records,
                sizeof(bench_record_t),
                &bench_record_count,
                &bench_record_capacity);
        record->probe = probe;
        record->flags = 0;
        record->step = 0;
        record->runfreq = 0.0;
        record->simfreq = 0.0;
        record->steps = 0;
        record->runtime = 0;
        record->simtime = 0;
        record->n = 0;
        record->result = 0;
        record->fresult = 0.0;
        record->cumul_reads   = 0;
        record->cumul_progs   = 0;
        record->cumul_erases  = 0;
        record->cumul_readed  = 0;
        record->cumul_progged = 0;
        record->cumul_erased  = 0;
        record->cumul_simtime = 0;

        if (bench_probe_count) {
            // find probe descriptor, if there is one
            bench_probe_t *probe_ = NULL;
            for (size_t i = 0; i < bench_probe_count; i++) {
                if (strcmp(bench_probes[i].name, probe) == 0) {
                    probe_ = &bench_probes[i];
                    break;
                }
            }

            // no matching probe descriptor?
            if (!probe_) {
                record->flags |= BENCH_RECORD_IGNORED;
            } else {
                record->step = probe_->step;
                record->runfreq = probe_->runfreq;
                record->simfreq = probe_->simfreq;
            }
        }

        // fallback to default step/runfreq/simfreq
        if (!record->step && !record->runfreq && !record->simfreq) {
            record->step = bench_probe_step;
            record->runfreq = bench_probe_runfreq;
            record->simfreq = bench_probe_simfreq;
        }
    }

    // add to cache
    cache->probe = probe;
    cache->i = record - bench_records;
    return record;
}

void bench_print(bench_record_t *record) {
    if (record->flags & BENCH_RECORD_RESULT) {
        printf("benched %s %jd %"PRIu64"\n",
                record->probe,
                record->n,
                record->result);
    } else if (record->flags & BENCH_RECORD_FRESULT) {
        printf("benched %s %jd %.6f\n",
                record->probe,
                record->n,
                record->fresult);
    } else if (record->flags & BENCH_RECORD_SIMTIME) {
        printf("benched %s %jd "
                    "%"PRIu64" %"PRIu64" %"PRIu64" "
                    "%"PRIu64" %"PRIu64" %"PRIu64" "
                    "%"PRIu64"\n",
                record->probe,
                record->n,
                record->cumul_reads,
                record->cumul_progs,
                record->cumul_erases,
                record->cumul_readed,
                record->cumul_progged,
                record->cumul_erased,
                record->cumul_simtime);
    } else {
        printf("benched %s %jd "
                    "%"PRIu64" %"PRIu64" %"PRIu64" "
                    "%"PRIu64" %"PRIu64" %"PRIu64"\n",
                record->probe,
                record->n,
                record->cumul_reads,
                record->cumul_progs,
                record->cumul_erases,
                record->cumul_readed,
                record->cumul_progged,
                record->cumul_erased);
    }

    record->flags &= ~BENCH_RECORD_DIRTY;
}

void bench_sample(bench_record_t *record) {
    // if no sample method is set, default to only printing at the end
    // of the bench
    if (!record->step && !record->runfreq && !record->simfreq) {
        return;
    }

    // sample at a specific step?
    if (record->step) {
        if (record->steps % record->step != 0) {
            record->steps += 1;
            return;
        }
        record->steps += 1;
    }

    // sample at a specific frequency?
    if (record->runfreq) {
        struct timespec t;
        clock_gettime(CLOCK_MONOTONIC, &t);
        bench_ns_t now = (bench_ns_t)t.tv_sec*1000*1000*1000
                + (bench_ns_t)t.tv_nsec;
        if (now - record->runtime
                < (bench_ns_t)((1000.0*1000.0*1000.0)
                    / record->runfreq)) {
            return;
        }
        record->runtime = now;
    }

    // sample at a specific simulated frequency?
    if (record->simfreq) {
        bench_sns_t now = BENCH_SIMTIME();
        if (now - record->simtime
                < (bench_ns_t)((1000.0*1000.0*1000.0)
                    / record->simfreq)) {
            return;
        }
        record->simtime = now;
    }

    bench_print(record);
}

void bench_start(const char *probe) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    // find our record
    bench_record_t *record = bench_find(probe);
    if (record->flags & BENCH_RECORD_IGNORED) {
        goto done;
    }

    if (record->flags & BENCH_RECORD_STARTED) {
        fprintf(stderr, "error: probe double started before it was "
                    "stopped (%s)\n",
                probe);
        assert(false);
        exit(-1);
    }

    // find current read/prog/erase
    #ifndef BENCH_KIWIBD
    bench_sio_t reads = lfs3_emubd_reads(bench_cfg);
    assert(reads >= 0);
    bench_sio_t progs = lfs3_emubd_progs(bench_cfg);
    assert(progs >= 0);
    bench_sio_t erases = lfs3_emubd_erases(bench_cfg);
    assert(erases >= 0);
    bench_sio_t readed = lfs3_emubd_readed(bench_cfg);
    assert(readed >= 0);
    bench_sio_t progged = lfs3_emubd_progged(bench_cfg);
    assert(progged >= 0);
    bench_sio_t erased = lfs3_emubd_erased(bench_cfg);
    assert(erased >= 0);
    // note this can error if no timings provided
    bench_sns_t simtime = lfs3_emubd_simtime(bench_cfg);
    #else
    bench_sio_t reads = lfs3_kiwibd_reads(bench_cfg);
    assert(reads >= 0);
    bench_sio_t progs = lfs3_kiwibd_progs(bench_cfg);
    assert(progs >= 0);
    bench_sio_t erases = lfs3_kiwibd_erases(bench_cfg);
    assert(erases >= 0);
    bench_sio_t readed = lfs3_kiwibd_readed(bench_cfg);
    assert(readed >= 0);
    bench_sio_t progged = lfs3_kiwibd_progged(bench_cfg);
    assert(progged >= 0);
    bench_sio_t erased = lfs3_kiwibd_erased(bench_cfg);
    assert(erased >= 0);
    // note this can error if no timings provided
    bench_sns_t simtime = lfs3_kiwibd_simtime(bench_cfg);
    #endif

    record->flags |= BENCH_RECORD_STARTED;

    record->start_reads   = reads;
    record->start_progs   = progs;
    record->start_erases  = erases;
    record->start_readed  = readed;
    record->start_progged = progged;
    record->start_erased  = erased;
    record->start_simtime = simtime;

done:;
    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
}

void bench_stop(const char *probe, uintmax_t n) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    // find our record
    bench_record_t *record = bench_find(probe);
    if (record->flags & BENCH_RECORD_IGNORED) {
        goto done;
    }

    if (!(record->flags & BENCH_RECORD_STARTED)) {
        fprintf(stderr, "error: probe stopped before it was started (%s)\n",
                probe);
        assert(false);
        exit(-1);
    }

    // find current read/prog/erase
    #ifndef BENCH_KIWIBD
    bench_sio_t reads = lfs3_emubd_reads(bench_cfg);
    assert(reads >= 0);
    bench_sio_t progs = lfs3_emubd_progs(bench_cfg);
    assert(progs >= 0);
    bench_sio_t erases = lfs3_emubd_erases(bench_cfg);
    assert(erases >= 0);
    bench_sio_t readed = lfs3_emubd_readed(bench_cfg);
    assert(readed >= 0);
    bench_sio_t progged = lfs3_emubd_progged(bench_cfg);
    assert(progged >= 0);
    bench_sio_t erased = lfs3_emubd_erased(bench_cfg);
    assert(erased >= 0);
    // note this can error if no timings provided
    bench_sns_t simtime = lfs3_emubd_simtime(bench_cfg);
    #else
    bench_sio_t reads = lfs3_kiwibd_reads(bench_cfg);
    assert(reads >= 0);
    bench_sio_t progs = lfs3_kiwibd_progs(bench_cfg);
    assert(progs >= 0);
    bench_sio_t erases = lfs3_kiwibd_erases(bench_cfg);
    assert(erases >= 0);
    bench_sio_t readed = lfs3_kiwibd_readed(bench_cfg);
    assert(readed >= 0);
    bench_sio_t progged = lfs3_kiwibd_progged(bench_cfg);
    assert(progged >= 0);
    bench_sio_t erased = lfs3_kiwibd_erased(bench_cfg);
    assert(erased >= 0);
    // note this can error if no timings provided
    bench_sns_t simtime = lfs3_kiwibd_simtime(bench_cfg);
    #endif

    // mark as dirty
    record->flags |= BENCH_RECORD_DIRTY;
    record->flags &= ~BENCH_RECORD_RESULT;
    record->flags &= ~BENCH_RECORD_FRESULT;
    if (simtime >= 0) {
        record->flags |= BENCH_RECORD_SIMTIME;
    }

    // update n
    record->n = n;
    // add to cumulative measurements
    record->cumul_reads   += reads   - record->start_reads;
    record->cumul_progs   += progs   - record->start_progs;
    record->cumul_erases  += erases  - record->start_erases;
    record->cumul_readed  += readed  - record->start_readed;
    record->cumul_progged += progged - record->start_progged;
    record->cumul_erased  += erased  - record->start_erased;
    record->cumul_simtime += simtime - record->start_simtime;

    // report probe sample
    bench_sample(record);

    record->flags &= ~BENCH_RECORD_STARTED;

done:;
    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
}

void bench_result(const char *probe, uintmax_t n, uintmax_t result) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    // find our record
    bench_record_t *record = bench_find(probe);
    if (record->flags & BENCH_RECORD_IGNORED) {
        goto done;
    }

    // mark as dirty
    record->flags |= BENCH_RECORD_DIRTY;
    record->flags |= BENCH_RECORD_RESULT;
    record->flags &= ~BENCH_RECORD_FRESULT;

    // update n
    record->n = n;
    // update result
    record->result = result;

    // report probe sample
    bench_sample(record);

done:;
    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
}

void bench_fresult(const char *probe, uintmax_t n, double result) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    // find our record
    bench_record_t *record = bench_find(probe);
    if (record->flags & BENCH_RECORD_IGNORED) {
        goto done;
    }

    // mark as dirty
    record->flags |= BENCH_RECORD_DIRTY;
    record->flags &= ~BENCH_RECORD_RESULT;
    record->flags |= BENCH_RECORD_FRESULT;

    // update n
    record->n = n;
    // update result
    record->fresult = result;

    // report probe sample
    bench_sample(record);

done:;
    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
}


bench_sns_t bench_simtime(void) {
    // bench not started?
    if (!bench_cfg) {
        return LFS3_ERR_INVAL;
    }

    // get the current simtime
    #ifndef BENCH_KIWIBD
    // note this can error if no timings provided
    bench_sns_t simtime = lfs3_emubd_simtime(bench_cfg);
    #else
    // note this can error if no timings provided
    bench_sns_t simtime = lfs3_kiwibd_simtime(bench_cfg);
    #endif
    return simtime;
}

void bench_simreset(void) {
    // reset bd
    #ifndef BENCH_KIWIBD
    int err = lfs3_emubd_simreset(bench_cfg);
    assert(!err);
    #else
    int err = lfs3_kiwibd_simreset(bench_cfg);
    assert(!err);
    #endif
}

void bench_simpause(void) {
    // pause bd simulation
    #ifndef BENCH_KIWIBD
    int err = lfs3_emubd_simpause(bench_cfg);
    assert(!err);
    #else
    int err = lfs3_kiwibd_simpause(bench_cfg);
    assert(!err);
    #endif
}

void bench_simresume(void) {
    // resume bd simulation
    #ifndef BENCH_KIWIBD
    int err = lfs3_emubd_simresume(bench_cfg);
    assert(!err);
    #else
    int err = lfs3_kiwibd_simresume(bench_cfg);
    assert(!err);
    #endif
}

void bench_reset(void) {
    // reset bd
    bench_simreset();

    // reset stack/heap measurements
    #ifdef BENCH_HEAP
    bench_heap_reset();
    #endif
    #ifdef BENCH_STACK
    bench_stack_reset();
    #endif
}

void bench_pause(void) {
    // pause stack/heap measurements
    #ifdef BENCH_STACK
    bench_stack_pause();
    #endif
    #ifdef BENCH_HEAP
    bench_heap_pause();
    #endif

    // pause bd simulation
    bench_simpause();
}

void bench_resume(void) {
    // resume bd simulation
    bench_simresume();

    // resume stack/heap measurements
    #ifdef BENCH_HEAP
    bench_heap_resume();
    #endif
    #ifdef BENCH_STACK
    bench_stack_resume();
    #endif
}




// encode our permutation into a reusable id
static void perm_printid(
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    (void)suite;
    // case[:permutation]
    printf("%s:", case_->name);
    for (size_t d = 0; d < bench_define_count; d++) {
        if (bench_define_ispermutation(bench_defines[d])) {
            leb16_print(d);
            leb16_print(*bench_defines[d]->define);
        }
    }
}

// a quick trie for keeping track of permutations we've seen
typedef struct bench_seen {
    struct bench_seen_branch *branches;
    size_t branch_count;
    size_t branch_capacity;
} bench_seen_t;

struct bench_seen_branch {
    intmax_t define;
    struct bench_seen branch;
};

bool bench_seen_insert(bench_seen_t *seen) {
    // use the currently set defines
    bool was_seen = true;
    for (size_t d = 0; d < bench_define_count; d++) {
        // treat unpermuted defines the same as 0
        intmax_t v = bench_define_ispermutation(bench_defines[d])
                ? *bench_defines[d]->define
                : 0;

        // already seen?
        struct bench_seen_branch *branch = NULL;
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
                    sizeof(struct bench_seen_branch),
                    &seen->branch_count,
                    &seen->branch_capacity);
            branch->define = v;
            branch->branch = (bench_seen_t){NULL, 0, 0};
        }

        seen = &branch->branch;
    }

    return was_seen;
}

void bench_seen_cleanup(bench_seen_t *seen) {
    for (size_t i = 0; i < seen->branch_count; i++) {
        bench_seen_cleanup(&seen->branches[i].branch);
    }
    free(seen->branches);
}

// iterate through permutations in a bench case
static void case_forperm(
        const bench_id_t *id,
        const struct bench_suite *suite,
        const struct bench_case *case_,
        void (*cb)(
            void *data,
            const struct bench_suite *suite,
            const struct bench_case *case_),
        void *data) {
    // explicit permutation?
    if (id && id->defines) {
        // define case permutation, the exact case perm doesn't matter here
        bench_define_case(id, suite, case_, 0);

        size_t permutations = bench_define_permutations();
        for (size_t p = 0; p < permutations; p++) {
            // define permutation permutation
            bench_define_permutation(p);

            cb(data, suite, case_);
        }

        return;
    }

    // deduplicate permutations with the same defines
    //
    // this can easily happen when overriding multiple case permutations,
    // we can't tell that multiple case permutations don't change defines,
    // duplicating results
    bench_seen_t seen = {NULL, 0, 0};

    for (size_t k = 0;
            k < ((case_->permutations) ? case_->permutations : 1);
            k++) {
        // define case permutation
        bench_define_case(id, suite, case_, k);

        size_t permutations = bench_define_permutations();
        for (size_t p = 0; p < permutations; p++) {
            // define permutation permutation
            bench_define_permutation(p);

            // have we seen this permutation before?
            bool was_seen = bench_seen_insert(&seen);
            if (!(k == 0 && p == 0) && was_seen) {
                continue;
            }

            cb(data, suite, case_);
        }
    }

    bench_seen_cleanup(&seen);
}


// how many permutations are there actually in a bench case
struct perm_count_state {
    size_t total;
    size_t filtered;
};

void perm_count(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct perm_count_state *state = data;
    (void)suite;

    // masked? consider this lower-level than filtering
    if (case_->flags & bench_mask) {
        return;
    }

    // skip this step?
    if (!(bench_steps >= bench_step_start
            && bench_steps < bench_step_stop
            && (bench_steps-bench_step_start) % bench_step_step == 0)) {
        bench_steps += 1;
        return;
    }
    bench_steps += 1;

    state->total += 1;

    // filter? this includes ifdef (run=NULL) and if checks
    if (!case_->run || !(bench_force || !case_->if_ || case_->if_())) {
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
    bench_flags_t flags = 0;
    struct perm_count_state perms = {0, 0};

    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            size_t cases_ = 0;

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
                cases_ += 1;
                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        perm_count,
                        &perms);
            }

            // no benches found?
            if (!cases_) {
                continue;
            }

            suites += 1;
            flags |= bench_suites[i]->flags;
        }
    }

    char perm_buf[64];
    sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
    char flag_buf[64];
    sprintf(flag_buf, "%s%s%s",
            (flags & BENCH_INTERNAL) ? "i" : "",
            (flags & BENCH_LITMUS)   ? "l" : "",
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
    for (size_t i = 0; i < bench_suite_count; i++) {
        size_t len = strlen(bench_suites[i]->name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %7s %7s %15s\n",
            name_width, "suite", "flags", "cases", "perms");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            size_t cases = 0;
            struct perm_count_state perms = {0, 0};

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        perm_count,
                        &perms);
            }

            // no benches found?
            if (!cases) {
                continue;
            }

            char perm_buf[64];
            sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
            bench_flags_t flags = bench_suites[i]->flags;
            char flag_buf[64];
            sprintf(flag_buf, "%s%s%s",
                    (flags & BENCH_INTERNAL) ? "i" : "",
                    (flags & BENCH_LITMUS)   ? "l" : "",
                    (!flags)                 ? "-" : "");
            printf("%-*s  %7s %7zu %15s\n",
                    name_width,
                    bench_suites[i]->name,
                    flag_buf,
                    cases,
                    perm_buf);
        }
    }
}

static void list_cases(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
            size_t len = strlen(bench_suites[i]->cases[j].name);
            if (len > name_width) {
                name_width = len;
            }
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %7s %15s\n", name_width, "case", "flags", "perms");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                struct perm_count_state perms = {0, 0};
                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        perm_count,
                        &perms);

                char perm_buf[64];
                sprintf(perm_buf, "%zu/%zu", perms.filtered, perms.total);
                bench_flags_t flags = bench_suites[i]->cases[j].flags;
                char flag_buf[64];
                sprintf(flag_buf, "%s%s%s",
                        (flags & BENCH_INTERNAL) ? "i" : "",
                        (flags & BENCH_LITMUS)   ? "l" : "",
                        (!flags)                 ? "-" : "");
                printf("%-*s  %7s %15s\n",
                        name_width,
                        bench_suites[i]->cases[j].name,
                        flag_buf,
                        perm_buf);
            }
        }
    }
}

static void list_suite_paths(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        size_t len = strlen(bench_suites[i]->name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "suite", "path");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            size_t cases = 0;

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                cases += 1;
            }

            // no benches found?
            if (!cases) {
                continue;
            }

            printf("%-*s  %s\n",
                    name_width,
                    bench_suites[i]->name,
                    bench_suites[i]->path);
        }
    }
}

static void list_case_paths(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
            size_t len = strlen(bench_suites[i]->cases[j].name);
            if (len > name_width) {
                name_width = len;
            }
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "case", "path");
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                printf("%-*s  %s\n",
                        name_width,
                        bench_suites[i]->cases[j].name,
                        bench_suites[i]->cases[j].path);
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
        const bench_define_t *define) {
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

static void list_defines_cleanup(
        struct list_defines_defines *defines) {
    for (size_t i = 0; i < defines->define_count; i++) {
        free(defines->defines[i].values);
    }
    free(defines->defines);
}

void perm_list_defines(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;

    // collect defines
    for (size_t d = 0; d < bench_define_count; d++) {
        if (bench_define_isdefined(bench_defines[d])) {
            list_defines_add(defines, bench_defines[d]);
        }
    }
}

void perm_list_permutation_defines(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;

    // collect permutation_defines
    for (size_t d = 0; d < bench_define_count; d++) {
        if (bench_define_ispermutation(bench_defines[d])) {
            list_defines_add(defines, bench_defines[d]);
        }
    }
}

static void list_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add defines
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
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

    list_defines_cleanup(&defines);
}

static void list_permutation_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add permutation defines
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
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

    list_defines_cleanup(&defines);
}

static void list_implicit_defines(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // yes we do need to define a suite/case, these do a bit of bookeeping
    // around mapping defines
    bench_define_suite(NULL,
            &(const struct bench_suite){0});
    bench_define_case(NULL,
            &(const struct bench_suite){0},
            &(const struct bench_case){0},
            0);

    size_t permutations = bench_define_permutations();
    for (size_t p = 0; p < permutations; p++) {
        // define permutation permutation
        bench_define_permutation(p);

        // add implicit defines
        for (size_t d = 0; d < bench_define_count; d++) {
            list_defines_add(&defines, bench_defines[d]);
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

    list_defines_cleanup(&defines);
}

static void list_probes(void) {
    // find relevant probes
    const char **probes = NULL;
    size_t probe_count = 0;
    size_t probe_capacity = 0;
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                // add unseen probes
                for (size_t p = 0;
                        p < bench_suites[i]->cases[j].probe_count;
                        p++) {
                    for (size_t q = 0; q < probe_count; q++) {
                        if (strcmp(probes[q],
                                bench_suites[i]->cases[j].probes[p]) == 0) {
                            goto next;
                        }
                    }

                    const char **probe = mappend(
                            (void**)&probes,
                            sizeof(const char*),
                            &probe_count,
                            &probe_capacity);
                    *probe = bench_suites[i]->cases[j].probes[p];
                }

            next:;
            }
        }
    }

    for (size_t p = 0; p < probe_count; p++) {
        printf("%s\n", probes[p]);
    }

    free(probes);
}

static void list_suite_probes(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        size_t len = strlen(bench_suites[i]->name);
        if (len > name_width) {
            name_width = len;
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "suite", "probes");
    // find relevant probes
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            const char **probes = NULL;
            size_t probe_count = 0;
            size_t probe_capacity = 0;

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                // add unseen probes
                for (size_t p = 0;
                        p < bench_suites[i]->cases[j].probe_count;
                        p++) {
                    for (size_t q = 0; q < probe_count; q++) {
                        if (strcmp(probes[q],
                                bench_suites[i]->cases[j].probes[p]) == 0) {
                            goto next;
                        }
                    }

                    const char **probe = mappend(
                            (void**)&probes,
                            sizeof(const char*),
                            &probe_count,
                            &probe_capacity);
                    *probe = bench_suites[i]->cases[j].probes[p];
                }

            next:;
            }

            printf("%-*s  ",
                    name_width,
                    bench_suites[i]->name);
            for (size_t p = 0; p < probe_count; p++) {
                printf("%s", probes[p]);
                if (p != probe_count-1) {
                    printf(",");
                }
            }
            printf("\n");

            free(probes);
        }
    }
}

static void list_case_probes(void) {
    // at least size so that names fit
    unsigned name_width = 23;
    for (size_t i = 0; i < bench_suite_count; i++) {
        for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
            size_t len = strlen(bench_suites[i]->cases[j].name);
            if (len > name_width) {
                name_width = len;
            }
        }
    }
    name_width = 4*((name_width+1+4-1)/4)-1;

    printf("%-*s  %s\n", name_width, "case", "probes");
    // find relevant probes
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                printf("%-*s  ",
                        name_width,
                        bench_suites[i]->cases[j].name);
                for (size_t p = 0;
                        p < bench_suites[i]->cases[j].probe_count;
                        p++) {
                    printf("%s", bench_suites[i]->cases[j].probes[p]);
                    if (p != bench_suites[i]->cases[j].probe_count-1) {
                        printf(",");
                    }
                }
                printf("\n");
            }
        }
    }
}

static const char *query_define_query = NULL;

void perm_query_define(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;

    // collect defines
    for (size_t d = 0; d < bench_define_count; d++) {
        if (bench_define_isdefined(bench_defines[d])
                && strcmp(bench_defines[d]->name, query_define_query) == 0) {
            list_defines_add(defines, bench_defines[d]);
        }
    }
}

void perm_query_permutation_define(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    struct list_defines_defines *defines = data;
    (void)suite;
    (void)case_;

    // collect permutation_defines
    for (size_t d = 0; d < bench_define_count; d++) {
        if (bench_define_ispermutation(bench_defines[d])
                && strcmp(bench_defines[d]->name, query_define_query) == 0) {
            list_defines_add(defines, bench_defines[d]);
        }
    }
}

static void query_define(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add defines
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        perm_query_define,
                        &defines);
            }
        }
    }

    // none found?
    if (defines.define_count == 0) {
        exit(1);
    }

    // print what was found
    assert(defines.define_count == 1);
    for (size_t j = 0; j < defines.defines[0].value_count; j++) {
        printf("%jd\n", defines.defines[0].values[j]);
    }

    list_defines_cleanup(&defines);
}

static void query_permutation_define(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // add permutation defines
    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
                        perm_query_permutation_define,
                        &defines);
            }
        }
    }

    // none found?
    if (defines.define_count == 0) {
        exit(1);
    }

    // print what was found
    assert(defines.define_count == 1);
    for (size_t j = 0; j < defines.defines[0].value_count; j++) {
        printf("%jd\n", defines.defines[0].values[j]);
    }

    list_defines_cleanup(&defines);
}

static void query_implicit_define(void) {
    struct list_defines_defines defines = {NULL, 0, 0};

    // yes we do need to define a suite/case, these do a bit of bookeeping
    // around mapping defines
    bench_define_suite(NULL,
            &(const struct bench_suite){0});
    bench_define_case(NULL,
            &(const struct bench_suite){0},
            &(const struct bench_case){0},
            0);

    size_t permutations = bench_define_permutations();
    for (size_t p = 0; p < permutations; p++) {
        // define permutation permutation
        bench_define_permutation(p);

        // add implicit defines
        for (size_t d = 0; d < bench_define_count; d++) {
            if (strcmp(bench_defines[d]->name, query_define_query) == 0) {
                list_defines_add(&defines, bench_defines[d]);
            }
        }
    }

    // none found?
    if (defines.define_count == 0) {
        exit(1);
    }

    // print what was found
    assert(defines.define_count == 1);
    for (size_t j = 0; j < defines.defines[0].value_count; j++) {
        printf("%jd\n", defines.defines[0].values[j]);
    }

    list_defines_cleanup(&defines);
}



// bench bd wrappers for heap/stack tracking
int bench_bd_read(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, void *buffer, lfs3_size_t size) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    #ifdef BENCH_KIWIBD
    int err = lfs3_kiwibd_read(cfg, block, off, buffer, size);
    #else
    int err = lfs3_emubd_read(cfg, block, off, buffer, size);
    #endif

    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
    return err;
}

int bench_bd_prog(const struct lfs3_cfg *cfg, lfs3_block_t block,
        lfs3_off_t off, const void *buffer, lfs3_size_t size) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    #ifdef BENCH_KIWIBD
    int err = lfs3_kiwibd_prog(cfg, block, off, buffer, size);
    #else
    int err = lfs3_emubd_prog(cfg, block, off, buffer, size);
    #endif

    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
    return err;
}

int bench_bd_erase(const struct lfs3_cfg *cfg, lfs3_block_t block) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    #ifdef BENCH_KIWIBD
    int err = lfs3_kiwibd_erase(cfg, block);
    #else
    int err = lfs3_emubd_erase(cfg, block);
    #endif

    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
    return err;
}

int bench_bd_sync(const struct lfs3_cfg *cfg) {
    BENCH_STACK_PAUSE();
    BENCH_HEAP_PAUSE();

    #ifdef BENCH_KIWIBD
    int err = lfs3_kiwibd_sync(cfg);
    #else
    int err = lfs3_emubd_sync(cfg);
    #endif

    BENCH_HEAP_RESUME();
    BENCH_STACK_RESUME();
    return err;
}



// main permutation runner
void perm_run(
        void *data,
        const struct bench_suite *suite,
        const struct bench_case *case_) {
    (void)data;

    // masked? consider this lower-level than filtering
    if (case_->flags & bench_mask) {
        return;
    }

    // skip this step?
    if (!(bench_steps >= bench_step_start
            && bench_steps < bench_step_stop
            && (bench_steps-bench_step_start) % bench_step_step == 0)) {
        bench_steps += 1;
        return;
    }
    bench_steps += 1;

    // filter? this includes ifdef (run=NULL) and if checks
    if (!case_->run || !(bench_force || !case_->if_ || case_->if_())) {
        printf("skipped ");
        perm_printid(suite, case_);
        printf("\n");
        return;
    }

    // create block device and configuration
    #ifndef BENCH_KIWIBD
    lfs3_emubd_t bd;
    #else
    lfs3_kiwibd_t bd;
    #endif

    #define BENCH_CFG CFG
    #define BENCH_CFG_CFG \
            .context        = &bd, \
            .read           = bench_bd_read, \
            .prog           = bench_bd_prog, \
            .erase          = bench_bd_erase, \
            .sync           = bench_bd_sync,
        #include BENCH_STRINGIFY(BENCH_DEFINES)
    #undef BENCH_CFG_CFG
    #undef BENCH_CFG

    #define BENCH_BDCFG BDCFG
    #define BENCH_BDCFG_CFG \
            .read_sleep     = bench_read_sleep, \
            .prog_sleep     = bench_prog_sleep, \
            .erase_sleep    = bench_erase_sleep,
        #include BENCH_STRINGIFY(BENCH_DEFINES)
    #undef BENCH_BDCFG_CFG
    #undef BENCH_BDCFG

    // init emubd?
    #ifndef BENCH_KIWIBD
    int err = lfs3_emubd_createcfg(CFG, bench_disk_path, BDCFG);
    if (err) {
        fprintf(stderr, "error: could not create emubd: %d\n", err);
        exit(-1);
    }
    // init kiwibd?
    #else
    int err = lfs3_kiwibd_createcfg(CFG, bench_disk_path, BDCFG);
    if (err) {
        fprintf(stderr, "error: could not create kiwibd: %d\n", err);
        exit(-1);
    }
    #endif

    // run the bench
    printf("running ");
    perm_printid(suite, case_);
    printf("\n");
    bench_init(CFG);
    #ifdef BENCH_HEAP
    bench_heap_enter();
    #endif
    #ifdef BENCH_STACK
    bench_stack_enter();
    #endif

    case_->run(CFG);

    #ifdef BENCH_STACK
    bench_stack_exit();
    #endif
    #ifdef BENCH_HEAP
    bench_heap_exit();
    #endif
    bench_deinit(CFG);
    printf("finished ");
    perm_printid(suite, case_);
    printf("\n");

    // cleanup
    #ifndef BENCH_KIWIBD
    err = lfs3_emubd_destroy(CFG);
    if (err) {
        fprintf(stderr, "error: could not destroy emubd: %d\n", err);
        exit(-1);
    }
    #else
    err = lfs3_kiwibd_destroy(CFG);
    if (err) {
        fprintf(stderr, "error: could not destroy kiwibd: %d\n", err);
        exit(-1);
    }
    #endif
}

static void run(void) {
    // ignore disconnected pipes
    signal(SIGPIPE, SIG_IGN);

    for (size_t t = 0; t < bench_id_count; t++) {
        for (size_t i = 0; i < bench_suite_count; i++) {
            bench_define_suite(&bench_ids[t], bench_suites[i]);

            for (size_t j = 0; j < bench_suites[i]->case_count; j++) {
                // does neither suite nor case name match?
                if (bench_ids[t].name && !(
                        strcmp(bench_ids[t].name,
                            bench_suites[i]->name) == 0
                        || strcmp(bench_ids[t].name,
                            bench_suites[i]->cases[j].name) == 0)) {
                    continue;
                }

                case_forperm(
                        &bench_ids[t],
                        bench_suites[i],
                        &bench_suites[i]->cases[j],
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
    OPT_LIST_PROBES              = 6,
    OPT_LIST_SUITE_PROBES        = 7,
    OPT_LIST_CASE_PROBES         = 8,
    OPT_QUERY_DEFINE             = 'Q',
    OPT_QUERY_PERMUTATION_DEFINE = 9,
    OPT_QUERY_IMPLICIT_DEFINE    = 10,
    OPT_DEFINE                   = 'D',
    OPT_DEFINE_DEPTH             = 11,
    OPT_PROBE                    = 'S',
    OPT_PROBE_STEP               = 'x',
    OPT_PROBE_RUNFREQ            = 12,
    OPT_PROBE_SIMFREQ            = 'X',
    OPT_STEP                     = 13,
    OPT_FORCE                    = 14,
    OPT_NO_INTERNAL              = 15,
    OPT_NO_LITMUS                = 16,
    OPT_DISK                     = 'd',
    OPT_TRACE                    = 't',
    OPT_TRACE_BACKTRACE          = 17,
    OPT_TRACE_STEP               = 18,
    OPT_TRACE_RUNFREQ            = 19,
    OPT_TRACE_SIMFREQ            = 20,
    OPT_READ_SLEEP               = 21,
    OPT_PROG_SLEEP               = 22,
    OPT_ERASE_SLEEP              = 23,
};

const char *short_opts = "hYlLQ:D:S:x:X:d:t:";

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
    {"list-probes",      no_argument,       NULL, OPT_LIST_PROBES},
    {"list-suite-probes",
                         no_argument,       NULL, OPT_LIST_SUITE_PROBES},
    {"list-case-probes", no_argument,       NULL, OPT_LIST_CASE_PROBES},
    {"query-define",     required_argument, NULL, OPT_QUERY_DEFINE},
    {"query-permutation-define",
                         required_argument, NULL, OPT_QUERY_PERMUTATION_DEFINE},
    {"query-implicit-define",
                         required_argument, NULL, OPT_QUERY_IMPLICIT_DEFINE},
    {"define",           required_argument, NULL, OPT_DEFINE},
    {"define-depth",     required_argument, NULL, OPT_DEFINE_DEPTH},
    {"probe",            required_argument, NULL, OPT_PROBE},
    {"probe-step",       required_argument, NULL, OPT_PROBE_STEP},
    {"probe-runfreq",    required_argument, NULL, OPT_PROBE_RUNFREQ},
    {"probe-simfreq",    required_argument, NULL, OPT_PROBE_SIMFREQ},
    {"step",             required_argument, NULL, OPT_STEP},
    {"force",            no_argument,       NULL, OPT_FORCE},
    {"no-internal",      no_argument,       NULL, OPT_NO_INTERNAL},
    {"no-litmus",        no_argument,       NULL, OPT_NO_LITMUS},
    {"disk",             required_argument, NULL, OPT_DISK},
    {"trace",            required_argument, NULL, OPT_TRACE},
    {"trace-backtrace",  no_argument,       NULL, OPT_TRACE_BACKTRACE},
    {"trace-step",       required_argument, NULL, OPT_TRACE_STEP},
    {"trace-runfreq",    required_argument, NULL, OPT_TRACE_RUNFREQ},
    {"trace-simfreq",    required_argument, NULL, OPT_TRACE_SIMFREQ},
    {"read-sleep",       required_argument, NULL, OPT_READ_SLEEP},
    {"prog-sleep",       required_argument, NULL, OPT_PROG_SLEEP},
    {"erase-sleep",      required_argument, NULL, OPT_ERASE_SLEEP},
    {NULL, 0, NULL, 0},
};

const char *const help_text[] = {
    "Show this help message.",
    "Show quick summary.",
    "List bench suites.",
    "List bench cases.",
    "List the path for each bench suite.",
    "List the path and line number for each bench case.",
    "List all defines in this bench-runner.",
    "List explicit defines in this bench-runner.",
    "List implicit defines in this bench-runner.",
    "List estimated probes.",
    "List estimated probes for each bench suite.",
    "List estimated probes for each bench case.",
    "Query a bench define.",
    "Query a permutation bench define.",
    "Query an implicit bench define.",
    "Override a bench define.",
    "How deep to evaluate recursive defines before erroring.",
    "Specify a probe to sample.",
    "Sample probes every n steps.",
    "Sample probes at this frequency in hz.",
    "Sample probes at this frequency in simulated hz.",
    "Comma-separated range of permutations to run.",
    "Ignore bench filters.",
    "Don't run internal benches.",
    "Don't run litmus benches.",
    "Direct block device operations to this file.",
    "Direct trace output to this file.",
    "Include a backtrace with every trace statement.",
    "Sample trace output every n steps.",
    "Sample trace output at this frequency in hz.",
    "Sample trace output at this frequency in simulated hz.",
    "Artificial read delay in seconds.",
    "Artificial prog delay in seconds.",
    "Artificial erase delay in seconds.",
};

int main(int argc, char **argv) {
    void (*op)(void) = run;

    // parse options
    while (true) {
        int c = getopt_long(argc, argv, short_opts, long_opts, NULL);
        switch (c) {
        // generate help message
        case OPT_HELP:;
            printf("usage: %s [options] [bench_id]\n", argv[0]);
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

        case OPT_LIST_PROBES:;
            op = list_probes;
            break;

        case OPT_LIST_SUITE_PROBES:;
            op = list_suite_probes;
            break;

        case OPT_LIST_CASE_PROBES:;
            op = list_case_probes;
            break;

        case OPT_QUERY_DEFINE:;
            op = query_define;
            query_define_query = optarg;
            break;

        case OPT_QUERY_PERMUTATION_DEFINE:;
            op = query_permutation_define;
            query_define_query = optarg;
            break;

        case OPT_QUERY_IMPLICIT_DEFINE:;
            op = query_implicit_define;
            query_define_query = optarg;
            break;

        // configuration
        case OPT_DEFINE:;
            // allocate space
            bench_define_t *override = mappend(
                    (void**)&bench_override_defines,
                    sizeof(bench_define_t),
                    &bench_override_define_count,
                    &bench_override_define_capacity);

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
                bench_override_value_t *override_values = NULL;
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
                                // allow empty string for step=1
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
                        *(bench_override_value_t*)mappend(
                                (void**)&override_values,
                                sizeof(bench_override_value_t),
                                &override_value_count,
                                &override_value_capacity)
                                = (bench_override_value_t){
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
                        *(bench_override_value_t*)mappend(
                                (void**)&override_values,
                                sizeof(bench_override_value_t),
                                &override_value_count,
                                &override_value_capacity)
                                = (bench_override_value_t){
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

                // define should be patched in bench_define_suite
                override->define = NULL;
                override->cb = bench_override_cb;
                override->data = malloc(sizeof(bench_override_data_t));
                *(bench_override_data_t*)override->data
                        = (bench_override_data_t){
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
            bench_define_depth = strtoumax(optarg, &parsed, 0);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid define-depth: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_PROBE:;
            // allocate space
            bench_probe_t *probe = mappend(
                    (void**)&bench_probes,
                    sizeof(bench_probe_t),
                    &bench_probe_count,
                    &bench_probe_capacity);

            // parse into string key/intmax_t value, cannibalizing the
            // arg in the process
            probe->name = optarg;
            sep = strchr(optarg, '=');
            if (sep) {
                *sep = '\0';
            }
            probe->step = 0;
            probe->runfreq = 0.0;
            probe->simfreq = 0.0;

            if (sep) {
                optarg = sep+1;

                // parse sample rate
                if (strstr(optarg, "rhz")) {
                    parsed = NULL;
                    probe->runfreq = strtod(optarg, &parsed);
                    if (parsed == optarg) {
                        goto invalid_probe;
                    }
                } else if (strstr(optarg, "shz")) {
                    parsed = NULL;
                    probe->simfreq = strtod(optarg, &parsed);
                    if (parsed == optarg) {
                        goto invalid_probe;
                    }
                } else {
                    parsed = NULL;
                    probe->step = strtoumax(optarg, &parsed, 0);
                    if (parsed == optarg) {
                        goto invalid_probe;
                    }
                }
            }
            break;

        invalid_probe:;
            fprintf(stderr, "error: invalid probe: %s\n", optarg);
            exit(-1);

        case OPT_PROBE_STEP:;
            parsed = NULL;
            bench_probe_step = strtoumax(optarg, &parsed, 0);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid probe-step: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_PROBE_RUNFREQ:;
            parsed = NULL;
            bench_probe_runfreq = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid probe-runfreq: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_PROBE_SIMFREQ:;
            parsed = NULL;
            bench_probe_simfreq = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid probe-simfreq: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_STEP:;
            parsed = NULL;
            bench_step_start = strtoumax(optarg, &parsed, 0);
            bench_step_stop = -1;
            bench_step_step = 1;
            // allow empty string for start=0
            if (parsed == optarg) {
                bench_step_start = 0;
            }
            optarg = parsed + strspn(parsed, " ");

            if (*optarg != ',' && *optarg != '\0') {
                goto step_unknown;
            }

            if (*optarg == ',') {
                optarg += 1;
                bench_step_stop = strtoumax(optarg, &parsed, 0);
                // allow empty string for stop=end
                if (parsed == optarg) {
                    bench_step_stop = -1;
                }
                optarg = parsed + strspn(parsed, " ");

                if (*optarg != ',' && *optarg != '\0') {
                    goto step_unknown;
                }

                if (*optarg == ',') {
                    optarg += 1;
                    bench_step_step = strtoumax(optarg, &parsed, 0);
                    // allow empty string for stop=1
                    if (parsed == optarg) {
                        bench_step_step = 1;
                    }
                    optarg = parsed + strspn(parsed, " ");

                    if (*optarg != '\0') {
                        goto step_unknown;
                    }
                }
            } else {
                // single value = stop only
                bench_step_stop = bench_step_start;
                bench_step_start = 0;
            }

            break;

        step_unknown:;
            fprintf(stderr, "error: invalid step: %s\n", optarg);
            exit(-1);

        case OPT_FORCE:;
            bench_force = true;
            break;

        case OPT_NO_INTERNAL:;
            bench_mask |= BENCH_INTERNAL;
            break;

        case OPT_NO_LITMUS:;
            bench_mask |= BENCH_LITMUS;
            break;

        case OPT_DISK:;
            bench_disk_path = optarg;
            break;

        case OPT_TRACE:;
            bench_trace_path = optarg;
            break;

        case OPT_TRACE_BACKTRACE:;
            bench_trace_backtrace = true;
            break;

        case OPT_TRACE_STEP:;
            parsed = NULL;
            bench_trace_step = strtoumax(optarg, &parsed, 0);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid trace-step: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_TRACE_RUNFREQ:;
            parsed = NULL;
            bench_trace_runfreq = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid trace-runfreq: %s\n", optarg);
                exit(-1);
            }
            break;

        case OPT_TRACE_SIMFREQ:;
            parsed = NULL;
            bench_trace_simfreq = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid trace-simfreq: %s\n", optarg);
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
            bench_read_sleep = read_sleep*1.0e9;
            break;

        case OPT_PROG_SLEEP:;
            parsed = NULL;
            double prog_sleep = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid prog-sleep: %s\n", optarg);
                exit(-1);
            }
            bench_prog_sleep = prog_sleep*1.0e9;
            break;

        case OPT_ERASE_SLEEP:;
            printf("hmm [%s]\n", optarg);
            parsed = NULL;
            double erase_sleep = strtod(optarg, &parsed);
            if (parsed == optarg) {
                fprintf(stderr, "error: invalid erase-sleep: %s\n", optarg);
                exit(-1);
            }
            printf("huh [%s]\n", parsed);
            bench_erase_sleep = erase_sleep*1.0e9;
            break;

        // done parsing
        case -1:;
            goto getopt_done;

        // unknown arg, getopt prints a message for us
        default:;
            exit(-1);
        }
    }
getopt_done: ;

    if (argc > optind) {
        // reset our bench identifier list
        bench_ids = NULL;
        bench_id_count = 0;
        bench_id_capacity = 0;
    }

    // parse bench identifier, if any, cannibalizing the arg in the process
    for (; argc > optind; optind++) {
        bench_define_t *defines = NULL;
        size_t define_count = 0;

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
                            ncount*sizeof(bench_define_t));
                    memset(defines+define_count, 0,
                            (ncount-define_count)*sizeof(bench_define_t));
                    define_count = ncount;
                }
                // name/define should be patched in bench_define_suite
                defines[d] = BENCH_LIT(NULL, v);
            }
        }

        // append to identifier list
        *(bench_id_t*)mappend(
                (void**)&bench_ids,
                sizeof(bench_id_t),
                &bench_id_count,
                &bench_id_capacity) = (bench_id_t){
            .name = name,
            .defines = defines,
            .define_count = define_count,
        };
    }

    // do the thing
    op();

    // cleanup (need to be done for valgrind benching)
    bench_define_cleanup();
    if (bench_override_defines) {
        for (size_t i = 0; i < bench_override_define_count; i++) {
            free((void*)(
                    (const bench_override_data_t*)
                        bench_override_defines[i].data)->values);
            free((void*)bench_override_defines[i].data);
        }
        free((void*)bench_override_defines);
    }
    if (bench_id_capacity) {
        for (size_t i = 0; i < bench_id_count; i++) {
            free((void*)bench_ids[i].defines);
        }
        free((void*)bench_ids);
    }
}
