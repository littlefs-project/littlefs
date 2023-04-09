/*
 * lfs utility functions
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS_UTIL_H
#define LFS_UTIL_H

// Users can override lfs_util.h with their own configuration by defining
// LFS_CONFIG as a header file to include (-DLFS_CONFIG=lfs_config.h).
//
// If LFS_CONFIG is used, none of the default utils will be emitted and must be
// provided by the config file. To start, I would suggest copying lfs_util.h
// and modifying as needed.
#ifdef LFS_CONFIG
#define LFS_STRINGIZE(x) LFS_STRINGIZE2(x)
#define LFS_STRINGIZE2(x) #x
#include LFS_STRINGIZE(LFS_CONFIG)
#else

// System includes
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <string.h>
#include <inttypes.h>

#ifndef LFS_NO_MALLOC
#include <stdlib.h>
#endif
#ifndef LFS_NO_ASSERT
#include <assert.h>
#endif
#if !defined(LFS_NO_DEBUG) || \
        !defined(LFS_NO_WARN) || \
        !defined(LFS_NO_ERROR) || \
        defined(LFS_YES_TRACE)
#include <stdio.h>
#endif

#ifdef __cplusplus
extern "C"
{
#endif


// Macros, may be replaced by system specific wrappers. Arguments to these
// macros must not have side-effects as the macros can be removed for a smaller
// code footprint

// Logging functions
#ifndef LFS_TRACE
#ifdef LFS_YES_TRACE
#define LFS_TRACE_(fmt, ...) \
    printf("%s:%d:trace: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")
#else
#define LFS_TRACE(...)
#endif
#endif

#ifndef LFS_DEBUG
#ifndef LFS_NO_DEBUG
#define LFS_DEBUG_(fmt, ...) \
    printf("%s:%d:debug: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_DEBUG(...) LFS_DEBUG_(__VA_ARGS__, "")
#else
#define LFS_DEBUG(...)
#endif
#endif

#ifndef LFS_WARN
#ifndef LFS_NO_WARN
#define LFS_WARN_(fmt, ...) \
    printf("%s:%d:warn: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_WARN(...) LFS_WARN_(__VA_ARGS__, "")
#else
#define LFS_WARN(...)
#endif
#endif

#ifndef LFS_ERROR
#ifndef LFS_NO_ERROR
#define LFS_ERROR_(fmt, ...) \
    printf("%s:%d:error: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS_ERROR(...) LFS_ERROR_(__VA_ARGS__, "")
#else
#define LFS_ERROR(...)
#endif
#endif

// Runtime assertions
#ifndef LFS_ASSERT
#ifndef LFS_NO_ASSERT
#define LFS_ASSERT(test) assert(test)
#else
#define LFS_ASSERT(test)
#endif
#endif

#ifndef LFS_UNREACHABLE
#ifndef LFS_NO_ASSERT
#define LFS_UNREACHABLE() __builtin_unreachable()
#else
#define LFS_UNREACHABLE() LFS_ASSERT(false)
#endif
#endif


// Builtin functions, these may be replaced by more efficient
// toolchain-specific implementations. LFS_NO_INTRINSICS falls back to a more
// expensive basic C implementation for debugging purposes

// Min/max functions for unsigned 32-bit numbers
static inline uint32_t lfs_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static inline uint32_t lfs_min(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static inline uint32_t lfs_max32(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static inline uint32_t lfs_min32(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static inline int32_t lfs_smax32(int32_t a, int32_t b) {
    return (a > b) ? a : b;
}

static inline int32_t lfs_smin32(int32_t a, int32_t b) {
    return (a < b) ? a : b;
}

static inline uint16_t lfs_max16(uint16_t a, uint16_t b) {
    return (a > b) ? a : b;
}

static inline uint16_t lfs_min16(uint16_t a, uint16_t b) {
    return (a < b) ? a : b;
}

// Absolute value of signed numbers
static inline int32_t lfs_abs32(int32_t a) {
    return a < 0 ? -a : a;
}

// TODO how many of these do we actually need
// Swap two 16-bit numbers
static inline void lfs_swap16(uint16_t *a, uint16_t *b) {
    uint16_t t = *a;
    *a = *b;
    *b = t;
}

static inline void lfs_sswap16(int16_t *a, int16_t *b) {
    int16_t t = *a;
    *a = *b;
    *b = t;
}

// Swap two 32-bit numbers
static inline void lfs_swap32(uint32_t *a, uint32_t *b) {
    uint32_t t = *a;
    *a = *b;
    *b = t;
}

static inline void lfs_sswap32(int32_t *a, int32_t *b) {
    int32_t t = *a;
    *a = *b;
    *b = t;
}

// Align to nearest multiple of a size
static inline uint32_t lfs_aligndown(uint32_t a, uint32_t alignment) {
    return a - (a % alignment);
}

static inline uint32_t lfs_alignup(uint32_t a, uint32_t alignment) {
    return lfs_aligndown(a + alignment-1, alignment);
}

// Find the smallest power of 2 greater than or equal to a
static inline uint32_t lfs_npw2(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && (defined(__GNUC__) || defined(__CC_ARM))
    return 32 - __builtin_clz(a-1);
#else
    uint32_t r = 0;
    uint32_t s;
    a -= 1;
    s = (a > 0xffff) << 4; a >>= s; r |= s;
    s = (a > 0xff  ) << 3; a >>= s; r |= s;
    s = (a > 0xf   ) << 2; a >>= s; r |= s;
    s = (a > 0x3   ) << 1; a >>= s; r |= s;
    return (r | (a >> 1)) + 1;
#endif
}

// TODO we should eventually adopt this as the new name for npw2
// Find the ceiling of log base 2 of the given number
static inline uint32_t lfs_nlog2(uint32_t a) {
    return lfs_npw2(a);
}

// Count the number of trailing binary zeros in a
// lfs_ctz(0) may be undefined
static inline uint32_t lfs_ctz(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && defined(__GNUC__)
    return __builtin_ctz(a);
#else
    return lfs_npw2((a & -a) + 1) - 1;
#endif
}

// Count the number of binary ones in a
static inline uint32_t lfs_popc(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && (defined(__GNUC__) || defined(__CC_ARM))
    return __builtin_popcount(a);
#else
    a = a - ((a >> 1) & 0x55555555);
    a = (a & 0x33333333) + ((a >> 2) & 0x33333333);
    return (((a + (a >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
#endif
}

// Find the sequence comparison of a and b, this is the distance
// between a and b ignoring overflow
static inline int lfs_scmp(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}

// Convert between 32-bit little-endian and native order
static inline uint32_t lfs_fromle32(uint32_t a) {
#if (defined(  BYTE_ORDER  ) && defined(  ORDER_LITTLE_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_LITTLE_ENDIAN  ) && __BYTE_ORDER   == __ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    return a;
#elif !defined(LFS_NO_INTRINSICS) && ( \
    (defined(  BYTE_ORDER  ) && defined(  ORDER_BIG_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_BIG_ENDIAN  ) && __BYTE_ORDER   == __ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__))
    return __builtin_bswap32(a);
#else
    return (((uint8_t*)&a)[0] <<  0) |
           (((uint8_t*)&a)[1] <<  8) |
           (((uint8_t*)&a)[2] << 16) |
           (((uint8_t*)&a)[3] << 24);
#endif
}

static inline uint32_t lfs_tole32(uint32_t a) {
    return lfs_fromle32(a);
}

// Convert between 32-bit big-endian and native order
static inline uint32_t lfs_frombe32(uint32_t a) {
#if !defined(LFS_NO_INTRINSICS) && ( \
    (defined(  BYTE_ORDER  ) && defined(  ORDER_LITTLE_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_LITTLE_ENDIAN  ) && __BYTE_ORDER   == __ORDER_LITTLE_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_LITTLE_ENDIAN__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
    return __builtin_bswap32(a);
#elif (defined(  BYTE_ORDER  ) && defined(  ORDER_BIG_ENDIAN  ) &&   BYTE_ORDER   ==   ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER  ) && defined(__ORDER_BIG_ENDIAN  ) && __BYTE_ORDER   == __ORDER_BIG_ENDIAN  ) || \
    (defined(__BYTE_ORDER__) && defined(__ORDER_BIG_ENDIAN__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    return a;
#else
    return (((uint8_t*)&a)[0] << 24) |
           (((uint8_t*)&a)[1] << 16) |
           (((uint8_t*)&a)[2] <<  8) |
           (((uint8_t*)&a)[3] <<  0);
#endif
}

static inline uint32_t lfs_tobe32(uint32_t a) {
    return lfs_frombe32(a);
}

// Convert to/from 16-bit little-endian
static inline void lfs_tole16_(uint16_t word, void *buffer) {
    ((uint8_t*)buffer)[0] = word >>  0;
    ((uint8_t*)buffer)[1] = word >>  8;
}

static inline uint16_t lfs_fromle16_(const void *buffer) {
    return (((uint8_t*)buffer)[0] <<  0)
         | (((uint8_t*)buffer)[1] <<  8);
}

// Convert to/from 32-bit little-endian
static inline void lfs_tole32_(uint32_t word, void *buffer) {
    ((uint8_t*)buffer)[0] = word >>  0;
    ((uint8_t*)buffer)[1] = word >>  8;
    ((uint8_t*)buffer)[2] = word >> 16;
    ((uint8_t*)buffer)[3] = word >> 24;
}

static inline uint32_t lfs_fromle32_(const void *buffer) {
    return (((uint8_t*)buffer)[0] <<  0)
         | (((uint8_t*)buffer)[1] <<  8)
         | (((uint8_t*)buffer)[2] << 16)
         | (((uint8_t*)buffer)[3] << 24);
}

// Convert to/from leb128 encoding
// TODO should we really be using ssize_t here and not lfs_ssize_t?
ssize_t lfs_toleb128(uint32_t word, void *buffer, size_t size);

ssize_t lfs_fromleb128(uint32_t *word, const void *buffer, size_t size);



// Calculate CRC-32 with polynomial = 0x04c11db7
uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size);

// Calculate crc32c incrementally
//
// polynomial = 0x11edc6f41
// init = 0xffffffff
// fini = 0xffffffff
//
uint32_t lfs_crc32c(uint32_t crc, const void *buffer, size_t size);


// Allocate memory, only used if buffers are not provided to littlefs
// Note, memory must be 64-bit aligned
static inline void *lfs_malloc(size_t size) {
#ifndef LFS_NO_MALLOC
    return malloc(size);
#else
    (void)size;
    return NULL;
#endif
}

// Deallocate memory, only used if buffers are not provided to littlefs
static inline void lfs_free(void *p) {
#ifndef LFS_NO_MALLOC
    free(p);
#else
    (void)p;
#endif
}


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
#endif
