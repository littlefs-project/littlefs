/*
 * Runner for littlefs benchmarks
 *
 * Copyright (c) 2022, The littlefs authors.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef BENCH_RUNNER_H
#define BENCH_RUNNER_H

#define BENCH_STRINGIFY_(x) #x
#define BENCH_STRINGIFY(x) BENCH_STRINGIFY_(x)

// the default BENCH_DEFINES path can be overridden to add shims for
// other filesystems out-of-tree
//
// note this is an unusual header file! instead of being included once,
// BENCH_DEFINES is included several times with various "query macros"
// defined before inclusion:
//
// - BENCH_INCLUDE - common includes (optional)
// - BENCH_DEFINE(name, value) - name and default values for bench defines
// - BENCH_CFG[+_CFG] - struct lfs3_cfg definition
// - BENCH_BDCFG[+_CFG] - struct lfs3_*bd_cfg definition
//
#ifndef BENCH_DEFINES
#define BENCH_DEFINES runners/bench_defines.h
#endif

// default to using kiwibd for benches
#if !defined(BENCH_EMUBD) && !defined(BENCH_KIWIBD)
#define BENCH_KIWIBD
#endif

// ifdef macros for emubd vs kiwibd
#ifdef BENCH_EMUBD
#define BENCH_IFDEF_EMUBD(a, b) (a)
#else
#define BENCH_IFDEF_EMUBD(a, b) (b)
#endif
#ifdef BENCH_KIWIBD
#define BENCH_IFDEF_KIWIBD(a, b) (a)
#else
#define BENCH_IFDEF_KIWIBD(a, b) (b)
#endif

// override LFS3_TRACE
#ifndef LFS3_NO_TRACE
void bench_trace(const char *fmt, ...);
#define LFS3_TRACE_(fmt, ...) \
    bench_trace("%s:%d:trace: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS3_TRACE(...) LFS3_TRACE_(__VA_ARGS__, "")
#define LFS3_EMUBD_TRACE(...) LFS3_TRACE_(__VA_ARGS__, "")
#define LFS3_KIWIBD_TRACE(...) LFS3_TRACE_(__VA_ARGS__, "")
#else
#define LFS3_TRACE(...)
#define LFS3_EMUBD_TRACE(...)
#define LFS3_KIWIBD_TRACE(...)
#endif


// note these are indirectly included in any generated files
#define BENCH_INCLUDE
    #include BENCH_STRINGIFY(BENCH_DEFINES)
#undef BENCH_INCLUDE

#ifndef BENCH_KIWIBD
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


// some common types
#ifndef BENCH_KIWIBD
typedef lfs3_emubd_io_t   bench_io_t;
typedef lfs3_emubd_sio_t  bench_sio_t;
typedef lfs3_emubd_ns_t   bench_ns_t;
typedef lfs3_emubd_sns_t  bench_sns_t;
#else
typedef lfs3_kiwibd_io_t  bench_io_t;
typedef lfs3_kiwibd_sio_t bench_sio_t;
typedef lfs3_kiwibd_ns_t  bench_ns_t;
typedef lfs3_kiwibd_sns_t bench_sns_t;
#endif

// generated bench configurations
struct lfs3_cfg;

enum bench_flags {
    BENCH_INTERNAL  = 0x1,
    BENCH_LITMUS    = 0x2,
};
typedef uint8_t bench_flags_t;

typedef struct bench_define {
    const char *name;
    intmax_t *define;
    intmax_t (*cb)(void *data, size_t i);
    void *data;
    size_t permutations;
} bench_define_t;

struct bench_case {
    const char *name;
    const char *path;
    bench_flags_t flags;

    const bench_define_t *defines;
    size_t permutations;

    const char **probes;
    size_t probe_count;

    bool (*if_)(void);
    void (*run)(const struct lfs3_cfg *cfg);
};

struct bench_suite {
    const char *name;
    const char *path;
    bench_flags_t flags;

    const bench_define_t *defines;
    size_t define_count;

    const struct bench_case *cases;
    size_t case_count;
};

extern const struct bench_suite *const bench_suites[];
extern const size_t bench_suite_count;


// BENCH_START/BENCH_STOP macros measure readed/progged/erased bytes
// through emubd
void bench_start(const char *probe);
void bench_stop(const char *probe, uintmax_t n);

#define BENCH_START(probe) bench_start(probe)
#define BENCH_STOP(probe, n) bench_stop(probe, n)

// BENCH_RESULT/BENCH_FRESULT allow for explicit non-io measurements
void bench_result(const char *probe, uintmax_t n, uintmax_t result);
void bench_fresult(const char *probe, uintmax_t n, double result);

#define BENCH_RESULT(probe, n, result) bench_result(probe, n, result)
#define BENCH_FRESULT(probe, n, result) bench_fresult(probe, n, result)

// extra hooks to get the current simtime, pause readed/progged/erased
// counters, etc
bench_sns_t bench_simtime(void);
void bench_simreset(void);
void bench_simpause(void);
void bench_simresume(void);
void bench_reset(void);
void bench_pause(void);
void bench_resume(void);

#define BENCH_SIMTIME() bench_simtime()
#define BENCH_SIMRESET() bench_simreset()
#define BENCH_SIMPAUSE() bench_simpause()
#define BENCH_SIMRESUME() bench_simresume()
#define BENCH_RESET() bench_reset()
#define BENCH_PAUSE() bench_pause()
#define BENCH_RESUME() bench_resume()


// deterministic prng for pseudo-randomness in benches
uint32_t bench_prng(uint32_t *state);

#define BENCH_PRNG(state) bench_prng(state)

// generation of specific permutations of an array for exhaustive benching
size_t bench_factorial(size_t x);
void bench_permutation(size_t i, uint32_t *buffer, size_t size);

#define BENCH_FACTORIAL(x) bench_factorial(x)
#define BENCH_PERMUTATION(i, buffer, size) bench_permutation(i, buffer, size)

// option to pause trace output
void bench_trace_pause(void);
void bench_trace_resume(void);

#define BENCH_TRACE_PAUSE() bench_trace_pause()
#define BENCH_TRACE_RESUME() bench_trace_resume()

#ifdef BENCH_STACK
// get the maximum/current stack usage for this run
extern size_t bench_stack_watermark;
__attribute__((noinline)) size_t bench_stack_current(void);
void bench_stack_reset(void);
__attribute__((noinline)) void bench_stack_pause(void);
void bench_stack_resume(void);

#define BENCH_STACK_WATERMARK() bench_stack_watermark
#define BENCH_STACK_CURRENT() bench_stack_current()
#define BENCH_STACK_RESET() bench_stack_reset()
#define BENCH_STACK_PAUSE() bench_stack_pause()
#define BENCH_STACK_RESUME() bench_stack_resume()
#else
// stubs if not measuring stack
#define BENCH_STACK_RESET()
#define BENCH_STACK_PAUSE()
#define BENCH_STACK_RESUME()
#endif

#ifdef BENCH_HEAP
// get the maximum/current heap usage for this run
extern size_t bench_heap_watermark;
extern size_t bench_heap_current;
void bench_heap_pause(void);
void bench_heap_resume(void);
void bench_heap_inc(size_t size);
void bench_heap_dec(size_t size);

#define BENCH_HEAP_WATERMARK() bench_heap_watermark
#define BENCH_HEAP_CURRENT() bench_heap_current
#define BENCH_HEAP_RESET() bench_heap_reset()
#define BENCH_HEAP_PAUSE() bench_heap_pause()
#define BENCH_HEAP_RESUME() bench_heap_resume()
#define BENCH_HEAP_INC(size) bench_heap_inc(size)
#define BENCH_HEAP_DEC(size) bench_heap_dec(size)
#else
// stubs if not measuring heap
#define BENCH_HEAP_RESET()
#define BENCH_HEAP_PAUSE()
#define BENCH_HEAP_RESUME()
#define BENCH_HEAP_INC(size)
#define BENCH_HEAP_DEC(size)
#endif


// declare implicit defines as global intmax_ts
#define BENCH_DEFINE(k, v) \
        extern intmax_t k;
    #include BENCH_STRINGIFY(BENCH_DEFINES)
#undef BENCH_DEFINE


#endif
