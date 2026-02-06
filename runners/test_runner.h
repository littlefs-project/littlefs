/*
 * Runner for littlefs tests
 *
 * Copyright (c) 2022, The littlefs authors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#define TEST_STRINGIFY_(x) #x
#define TEST_STRINGIFY(x) TEST_STRINGIFY_(x)

// the default TEST_DEFINES path can be overridden to add shims for
// other filesystems out-of-tree
//
// note this is an unusual header file! instead of being included once,
// TEST_DEFINES is included several times with various "query macros"
// defined before inclusion:
//
// - TEST_INCLUDE - common includes (optional)
// - TEST_DEFINE(name, value) - name and default values for test defines
// - TEST_CFG[+_CFG] - struct lfs3_cfg definition
// - TEST_BDCFG[+_CFG] - struct lfs3_*bd_cfg definition
//
#ifndef TEST_DEFINES
#define TEST_DEFINES runners/test_defines.h
#endif

// default to using emubd for tests
#if !defined(TEST_EMUBD) && !defined(TEST_KIWIBD)
#define TEST_EMUBD
#endif

// ifdef macros for emubd vs kiwibd
#ifdef TEST_EMUBD
#define TEST_IFDEF_EMUBD(a, b) (a)
#else
#define TEST_IFDEF_EMUBD(a, b) (b)
#endif
#ifdef TEST_KIWIBD
#define TEST_IFDEF_KIWIBD(a, b) (a)
#else
#define TEST_IFDEF_KIWIBD(a, b) (b)
#endif

// override LFS3_TRACE
#ifndef LFS3_NO_TRACE
void test_trace(const char *fmt, ...);
#define LFS3_TRACE_(fmt, ...) \
    test_trace("%s:%d:trace: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS3_TRACE(...) LFS3_TRACE_(__VA_ARGS__, "")
#define LFS3_EMUBD_TRACE(...) LFS3_TRACE_(__VA_ARGS__, "")
#define LFS3_KIWIBD_TRACE(...) LFS3_TRACE_(__VA_ARGS__, "")
#else
#define LFS3_TRACE(...)
#define LFS3_EMUBD_TRACE(...)
#define LFS3_KIWIBD_TRACE(...)
#endif


// note these are indirectly included in any generated files
#define TEST_INCLUDE
    #include TEST_STRINGIFY(TEST_DEFINES)
#undef TEST_INCLUDE

#ifndef TEST_KIWIBD
#include "bd/lfs3_emubd.h"
#else
#include "bd/lfs3_kiwibd.h"
#endif
#include "lfs3_util.h"

#include <stdio.h>
#include <stdint.h>

// give source a chance to define feature macros
#undef _FEATURES_H
#undef _STDIO_H


// generated test configurations
struct lfs3_cfg;

enum test_flags {
    TEST_INTERNAL  = 0x1,
    TEST_REENTRANT = 0x2,
    TEST_FUZZ      = 0x4,
};
typedef uint8_t test_flags_t;

typedef struct test_define {
    const char *name;
    intmax_t *define;
    intmax_t (*cb)(void *data, size_t i);
    void *data;
    size_t permutations;
} test_define_t;

struct test_case {
    const char *name;
    const char *path;
    test_flags_t flags;

    const test_define_t *defines;
    size_t permutations;

    bool (*if_)(void);
    void (*run)(const struct lfs3_cfg *cfg);
};

struct test_suite {
    const char *name;
    const char *path;
    test_flags_t flags;

    const test_define_t *defines;
    size_t define_count;

    const struct test_case *cases;
    size_t case_count;
};

extern const struct test_suite *const test_suites[];
extern const size_t test_suite_count;


// this variable tracks the number of powerlosses triggered during the
// current test permutation, this is useful for both tests and debugging
extern volatile size_t TEST_PLS;

// deterministic prng for pseudo-randomness in tests
uint32_t test_prng(uint32_t *state);

#define TEST_PRNG(state) test_prng(state)

// generation of specific permutations of an array for exhaustive testing
size_t test_factorial(size_t x);
void test_permutation(size_t i, uint32_t *buffer, size_t size);

#define TEST_FACTORIAL(x) test_factorial(x)
#define TEST_PERMUTATION(i, buffer, size) test_permutation(i, buffer, size)

#ifdef TEST_STACK
// get the maximum/current stack usage for this run
extern size_t test_stack_watermark;
__attribute__((noinline)) size_t test_stack_current(void);
__attribute__((noinline)) void test_stack_pause(void);
void test_stack_resume(void);

#define TEST_STACK_WATERMARK() test_stack_watermark
#define TEST_STACK_CURRENT() test_stack_current()
#define TEST_STACK_PAUSE() test_stack_pause()
#define TEST_STACK_RESUME() test_stack_resume()
#else
// stubs if not measuring stack
#define TEST_STACK_PAUSE()
#define TEST_STACK_RESUME()
#endif

#ifdef TEST_HEAP
// get the maximum/current heap usage for this run
extern size_t test_heap_watermark;
extern size_t test_heap_current;
void test_heap_pause(void);
void test_heap_resume(void);
void test_heap_inc(size_t size);
void test_heap_dec(size_t size);

#define TEST_HEAP_WATERMARK() test_heap_watermark
#define TEST_HEAP_CURRENT() test_heap_current
#define TEST_HEAP_PAUSE() test_heap_pause()
#define TEST_HEAP_RESUME() test_heap_resume()
#define TEST_HEAP_INC(size) test_heap_inc(size)
#define TEST_HEAP_DEC(size) test_heap_dec(size)
#else
// stubs if not measuring heap
#define TEST_HEAP_PAUSE()
#define TEST_HEAP_RESUME()
#define TEST_HEAP_INC(size)
#define TEST_HEAP_DEC(size)
#endif


// declare implicit defines as global intmax_ts
#define TEST_DEFINE(k, v) \
        extern intmax_t k;
    #include TEST_STRINGIFY(TEST_DEFINES)
#undef TEST_DEFINE


#endif
