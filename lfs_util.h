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
#include <inttypes.h>
#ifndef LFS_NO_STRINGH
#include <string.h>
#endif
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
#define LFS_UNREACHABLE() LFS_ASSERT(false)
#elif !defined(LFS_NO_BUILTINS)
#define LFS_UNREACHABLE() __builtin_unreachable()
#else
#define LFS_UNREACHABLE()
#endif
#endif

// We need to know the endianness of the system for some struct packing
#if (defined(BYTE_ORDER) \
            && defined(ORDER_LITTLE_ENDIAN) \
            && BYTE_ORDER == ORDER_LITTLE_ENDIAN) \
        || (defined(__BYTE_ORDER) \
            && defined(__ORDER_LITTLE_ENDIAN) \
            && __BYTE_ORDER == __ORDER_LITTLE_ENDIAN) \
        || (defined(__BYTE_ORDER__) \
            && defined(__ORDER_LITTLE_ENDIAN__) \
            && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define LFS_LITTLE_ENDIAN
#elif (defined(BYTE_ORDER) \
            && defined(ORDER_BIG_ENDIAN) \
            && BYTE_ORDER == ORDER_BIG_ENDIAN) \
        || (defined(__BYTE_ORDER) \
            && defined(__ORDER_BIG_ENDIAN) \
            && __BYTE_ORDER == __ORDER_BIG_ENDIAN) \
        || (defined(__BYTE_ORDER__) \
            && defined(__ORDER_BIG_ENDIAN__) \
            && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#define LFS_BIG_ENDIAN
#else
#error "lfs: Unknown endianness?"
#endif


// Some ifdef conveniences
#ifdef LFS_CKPROGS
#define LFS_IFDEF_CKPROGS(a, b) (a)
#else
#define LFS_IFDEF_CKPROGS(a, b) (b)
#endif

#ifdef LFS_CKFETCHES
#define LFS_IFDEF_CKFETCHES(a, b) (a)
#else
#define LFS_IFDEF_CKFETCHES(a, b) (b)
#endif

#ifdef LFS_CKPARITY
#define LFS_IFDEF_CKPARITY(a, b) (a)
#else
#define LFS_IFDEF_CKPARITY(a, b) (b)
#endif

#ifdef LFS_CKDATACKSUMS
#define LFS_IFDEF_CKDATACKSUMS(a, b) (a)
#else
#define LFS_IFDEF_CKDATACKSUMS(a, b) (b)
#endif


// Builtin functions, these may be replaced by more efficient
// toolchain-specific implementations. LFS_NO_BUILTINS falls back to a more
// expensive basic C implementation for debugging purposes

// Compile time min/max
#define LFS_MIN(a, b) ((a < b) ? a : b)
#define LFS_MAX(a, b) ((a > b) ? a : b)

// Min/max functions for unsigned 32-bit numbers
static inline uint32_t lfs_min(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static inline uint32_t lfs_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static inline int32_t lfs_smin(int32_t a, int32_t b) {
    return (a < b) ? a : b;
}

static inline int32_t lfs_smax(int32_t a, int32_t b) {
    return (a > b) ? a : b;
}

// Absolute value of signed numbers
static inline int32_t lfs_abs(int32_t a) {
    return (a < 0) ? -a : a;
}

// Swap two variables
#define LFS_SWAP(_t, _a, _b) \
    do { \
        _t *a = _a; \
        _t *b = _b; \
        _t t = *a; \
        *a = *b; \
        *b = t; \
    } while (0)

// Align to nearest multiple of a size
static inline uint32_t lfs_aligndown(uint32_t a, uint32_t alignment) {
    return a - (a % alignment);
}

static inline uint32_t lfs_alignup(uint32_t a, uint32_t alignment) {
    return lfs_aligndown(a + alignment-1, alignment);
}

// Find the smallest power of 2 greater than or equal to a
static inline uint32_t lfs_npw2(uint32_t a) {
    // __builtin_clz of zero is undefined, so treat both 0 and 1 specially
    if (a <= 1) {
        return a;
    }

#if !defined(LFS_NO_BUILTINS) && (defined(__GNUC__) || defined(__CC_ARM))
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
#if !defined(LFS_NO_BUILTINS) && defined(__GNUC__)
    return __builtin_ctz(a);
#else
    return lfs_npw2((a & -a) + 1) - 1;
#endif
}

// Count the number of binary ones in a
static inline uint32_t lfs_popc(uint32_t a) {
#if !defined(LFS_NO_BUILTINS) && (defined(__GNUC__) || defined(__CC_ARM))
    return __builtin_popcount(a);
#else
    a = a - ((a >> 1) & 0x55555555);
    a = (a & 0x33333333) + ((a >> 2) & 0x33333333);
    return (((a + (a >> 4)) & 0xf0f0f0f) * 0x1010101) >> 24;
#endif
}

// Returns true if there is an odd number of binary ones in a
static inline bool lfs_parity(uint32_t a) {
#if !defined(LFS_NO_BUILTINS) && (defined(__GNUC__) || defined(__CC_ARM))
    return __builtin_parity(a);
#else
    return lfs_popc(a) & 1;
#endif
}

// Find the sequence comparison of a and b, this is the distance
// between a and b ignoring overflow
static inline int lfs_scmp(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}

// Convert between 32-bit little-endian and native order
static inline uint32_t lfs_fromle32(uint32_t a) {
#if !defined(LFS_NO_BUILTINS) && defined(LFS_LITTLE_ENDIAN)
    return a;
#elif !defined(LFS_NO_BUILTINS)
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
#if !defined(LFS_NO_BUILTINS) && defined(LFS_LITTLE_ENDIAN)
    return __builtin_bswap32(a);
#elif !defined(LFS_NO_BUILTINS)
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


// Compare n bytes of memory
#if !defined(LFS_NO_STRINGH)
#define lfs_memcmp memcmp
#elif !defined(LFS_NO_BUILTINS)
#define lfs_memcmp __builtin_memcmp
#else
static inline int lfs_memcmp(const void *a, const void *b, size_t size) {
    const uint8_t *a_ = a;
    const uint8_t *b_ = b;
    for (size_t i = 0; i < size; i++) {
        if (a_[i] != b_[i]) {
            return (int)a_[i] - (int)b_[i];
        }
    }

    return 0;
}
#endif

// Copy n bytes from src to dst, src and dst must not overlap
#if !defined(LFS_NO_STRINGH)
#define lfs_memcpy memcpy
#elif !defined(LFS_NO_BUILTINS)
#define lfs_memcpy __builtin_memcpy
#else
static inline void *lfs_memcpy(
        void *restrict dst, const void *restrict src, size_t size) {
    uint8_t *dst_ = dst;
    const uint8_t *src_ = src;
    for (size_t i = 0; i < size; i++) {
        dst_[i] = src_[i];
    }

    return dst_;
}
#endif

// Copy n bytes from src to dst, src and dst may overlap
#if !defined(LFS_NO_STRINGH)
#define lfs_memmove memmove
#elif !defined(LFS_NO_BUILTINS)
#define lfs_memmove __builtin_memmove
#else
static inline void *lfs_memmove(void *dst, const void *src, size_t size) {
    uint8_t *dst_ = dst;
    const uint8_t *src_ = src;
    if (dst_ < src_) {
        for (size_t i = 0; i < size; i++) {
            dst_[i] = src_[i];
        }
    } else if (dst_ > src_) {
        for (size_t i = 0; i < size; i++) {
            dst_[(size-1)-i] = src_[(size-1)-i];
        }
    }

    return dst_;
}
#endif

// Set n bytes to c
#if !defined(LFS_NO_STRINGH)
#define lfs_memset memset
#elif !defined(LFS_NO_BUILTINS)
#define lfs_memset __builtin_memset
#else
static inline void *lfs_memset(void *dst, int c, size_t size) {
    uint8_t *dst_ = dst;
    for (size_t i = 0; i < size; i++) {
        dst_[i] = c;
    }

    return dst_;
}
#endif

// Find the first occurrence of c or NULL
#if !defined(LFS_NO_STRINGH)
#define lfs_memchr memchr
#else
static inline void *lfs_memchr(const void *a, int c, size_t size) {
    const uint8_t *a_ = a;
    for (size_t i = 0; i < size; i++) {
        if (a_[i] == c) {
            return (void*)&a_[i];
        }
    }

    return NULL;
}
#endif

// Find the first occurrence of anything not c or NULL
static inline void *lfs_memcchr(const void *a, int c, size_t size) {
    const uint8_t *a_ = a;
    for (size_t i = 0; i < size; i++) {
        if (a_[i] != c) {
            return (void*)&a_[i];
        }
    }

    return NULL;
}

// Xor n bytes from b into a
static inline void *lfs_memxor(
        void *restrict a, const void *restrict b, size_t size) {
    uint8_t *a_ = a;
    const uint8_t *b_ = b;
    for (size_t i = 0; i < size; i++) {
        a_[i] ^= b_[i];
    }

    return a_;
}


// Find the length of a null-terminated string
#if !defined(LFS_NO_STRINGH)
#define lfs_strlen strlen
#else
static inline size_t lfs_strlen(const char *a) {
    const char *a_ = a;
    while (*a_) {
        a_++;
    }

    return a_ - a;
}
#endif

// Compare two null-terminated strings
#if !defined(LFS_NO_STRINGH)
#define lfs_strcmp strcmp
#else
static inline int lfs_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }

    return (int)*a - (int)*b;
}
#endif

// Copy a null-terminated string from src to dst
#if !defined(LFS_NO_STRINGH)
#define lfs_strcpy strcpy
#else
static inline char *lfs_strcpy(
        char *restrict dst, const char *restrict src) {
    char *dst_ = dst;
    while (*src) {
        *dst_ = *src;
        dst_++;
        src++;
    }

    *dst_ = '\0';
    return dst;
}
#endif

// Find first occurrence of c or NULL
#ifndef LFS_NO_STRINGH
#define lfs_strchr strchr
#else
static inline char *lfs_strchr(const char *a, int c) {
    while (*a) {
        if (*a == c) {
            return (char*)a;
        }

        a++;
    }

    return NULL;
}
#endif

// Find first occurrence of anything not c or NULL
static inline char *lfs_strcchr(const char *a, int c) {
    while (*a) {
        if (*a != c) {
            return (char*)a;
        }

        a++;
    }

    return NULL;
}

// Find length of a that does not contain any char in cs
#ifndef LFS_NO_STRINGH
#define lfs_strspn strspn
#else
static inline size_t lfs_strspn(const char *a, const char *cs) {
    const char *a_ = a;
    while (*a_) {
        const char *cs_ = cs;
        while (*cs_) {
            if (*a_ != *cs_) {
                return a_ - a;
            }
            cs_++;
        }

        a_++;
    }

    return a_ - a;
}
#endif

// Find length of a that only contains chars in cs
#ifndef LFS_NO_STRINGH
#define lfs_strcspn strcspn
#else
static inline size_t lfs_strcspn(const char *a, const char *cs) {
    const char *a_ = a;
    while (*a_) {
        const char *cs_ = cs;
        while (*cs_) {
            if (*a_ == *cs_) {
                return a_ - a;
            }
            cs_++;
        }

        a_++;
    }

    return a_ - a;
}
#endif


//// Calculate CRC-32 with polynomial = 0x04c11db7
//uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size);

// Odd-parity and even-parity zeros in our crc32c ring
#define LFS_CRC32C_ODDZERO  0xfca42daf
#define LFS_CRC32C_EVENZERO 0x00000000

// Calculate crc32c incrementally
//
// polynomial = 0x11edc6f41
// init = 0xffffffff
// fini = 0xffffffff
//
uint32_t lfs_crc32c(uint32_t crc, const void *buffer, size_t size);


// Allocate memory, only used if buffers are not provided to littlefs
// Note, memory must be 64-bit aligned
#ifndef LFS_NO_MALLOC
#define lfs_malloc malloc
#else
static inline void *lfs_malloc(size_t size) {
    (void)size;
    return NULL;
}
#endif

// Deallocate memory, only used if buffers are not provided to littlefs
#ifndef LFS_NO_MALLOC
#define lfs_free free
#else
static inline void lfs_free(void *p) {
    (void)p;
}
#endif


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif
#endif
