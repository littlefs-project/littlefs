/*
 * lfs3 utility functions
 *
 * Copyright (c) 2022, The littlefs authors.
 * Copyright (c) 2017, Arm Limited. All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef LFS3_UTIL_H
#define LFS3_UTIL_H

// Users can override lfs3_util.h with their own configuration by defining
// LFS3_CONFIG as a header file to include (-DLFS_CONFIG=lfs3_config.h).
//
// If LFS3_CONFIG is used, none of the default utils will be emitted and must be
// provided by the config file. To start, I would suggest copying lfs3_util.h
// and modifying as needed.
#ifdef LFS3_CONFIG
#define LFS3_STRINGIZE(x) LFS3_STRINGIZE2(x)
#define LFS3_STRINGIZE2(x) #x
#include LFS3_STRINGIZE(LFS3_CONFIG)
#else


// Some convenient macro aliases
// TODO move these to something like lfs3_cfg.h?

// LFS3_BIGGEST enables all opt-in features
#ifdef LFS3_BIGGEST
#ifndef LFS3_REVDBG
#define LFS3_REVDBG
#endif
#ifndef LFS3_REVNOISE
#define LFS3_REVNOISE
#endif
#ifndef LFS3_CKPROGS
#define LFS3_CKPROGS
#endif
#ifndef LFS3_CKFETCHES
#define LFS3_CKFETCHES
#endif
#ifndef LFS3_CKMETAPARITY
#define LFS3_CKMETAPARITY
#endif
#ifndef LFS3_CKDATACKSUMREADS
#define LFS3_CKDATACKSUMREADS
#endif
#ifndef LFS3_GC
#define LFS3_GC
#endif
#endif

// LFS3_YES_* variants imply the relevant LFS3_* macro
#ifdef LFS3_YES_RDONLY
#define LFS3_RDONLY
#endif
#ifdef LFS3_YES_KVONLY
#define LFS3_KVONLY
#endif
#ifdef LFS3_YES_2BONLY
#define LFS3_2BONLY
#endif
#ifdef LFS3_YES_REVDBG
#define LFS3_REVDBG
#endif
#ifdef LFS3_YES_REVNOISE
#define LFS3_REVNOISE
#endif
#ifdef LFS3_YES_CKPROGS
#define LFS3_CKPROGS
#endif
#ifdef LFS3_YES_CKFETCHES
#define LFS3_CKFETCHES
#endif
#ifdef LFS3_YES_CKMETAPARITY
#define LFS3_CKMETAPARITY
#endif
#ifdef LFS3_YES_CKDATACKSUMREADS
#define LFS3_CKDATACKSUMREADS
#endif
#ifdef LFS3_YES_GC
#define LFS3_GC
#endif

// LFS3_NO_LOG disables all logging macros
#ifdef LFS3_NO_LOG
#ifndef LFS3_NO_DEBUG
#define LFS3_NO_DEBUG
#endif
#ifndef LFS3_NO_INFO
#define LFS3_NO_INFO
#endif
#ifndef LFS3_NO_WARN
#define LFS3_NO_WARN
#endif
#ifndef LFS3_NO_ERROR
#define LFS3_NO_ERROR
#endif
#endif


// System includes
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <inttypes.h>
#ifndef LFS3_NO_STRINGH
#include <string.h>
#endif
#ifndef LFS3_NO_MALLOC
#include <stdlib.h>
#endif
#ifndef LFS3_NO_ASSERT
#include <assert.h>
#endif
#if !defined(LFS3_NO_DEBUG) || \
        !defined(LFS3_NO_INFO) || \
        !defined(LFS3_NO_WARN) || \
        !defined(LFS3_NO_ERROR) || \
        defined(LFS3_YES_TRACE)
#include <stdio.h>
#endif


// Macros, may be replaced by system specific wrappers. Arguments to these
// macros must not have side-effects as the macros can be removed for a smaller
// code footprint

// Logging functions
#ifndef LFS3_TRACE
#ifdef LFS3_YES_TRACE
#define LFS3_TRACE_(fmt, ...) \
    printf("%s:%d:trace: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS3_TRACE(...) LFS3_TRACE_(__VA_ARGS__, "")
#else
#define LFS3_TRACE(...)
#endif
#endif

#ifndef LFS3_DEBUG
#ifndef LFS3_NO_DEBUG
#define LFS3_DEBUG_(fmt, ...) \
    printf("%s:%d:debug: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS3_DEBUG(...) LFS3_DEBUG_(__VA_ARGS__, "")
#else
#define LFS3_DEBUG(...)
#endif
#endif

#ifndef LFS3_INFO
#ifndef LFS3_NO_INFO
#define LFS3_INFO_(fmt, ...) \
    printf("%s:%d:info: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS3_INFO(...) LFS3_INFO_(__VA_ARGS__, "")
#else
#define LFS3_INFO(...)
#endif
#endif

#ifndef LFS3_WARN
#ifndef LFS3_NO_WARN
#define LFS3_WARN_(fmt, ...) \
    printf("%s:%d:warn: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS3_WARN(...) LFS3_WARN_(__VA_ARGS__, "")
#else
#define LFS3_WARN(...)
#endif
#endif

#ifndef LFS3_ERROR
#ifndef LFS3_NO_ERROR
#define LFS3_ERROR_(fmt, ...) \
    printf("%s:%d:error: " fmt "%s\n", __FILE__, __LINE__, __VA_ARGS__)
#define LFS3_ERROR(...) LFS3_ERROR_(__VA_ARGS__, "")
#else
#define LFS3_ERROR(...)
#endif
#endif

// Runtime assertions
#ifndef LFS3_ASSERT
#ifndef LFS3_NO_ASSERT
#define LFS3_ASSERT(test) assert(test)
#else
#define LFS3_ASSERT(test)
#endif
#endif

#ifndef LFS3_UNREACHABLE
#ifndef LFS3_NO_ASSERT
#define LFS3_UNREACHABLE() LFS3_ASSERT(false)
#elif !defined(LFS3_NO_BUILTINS)
#define LFS3_UNREACHABLE() __builtin_unreachable()
#else
#define LFS3_UNREACHABLE()
#endif
#endif


// Some ifdef conveniences
#ifdef LFS3_RDONLY
#define LFS3_IFDEF_RDONLY(a, b) (a)
#else
#define LFS3_IFDEF_RDONLY(a, b) (b)
#endif

#ifdef LFS3_KVONLY
#define LFS3_IFDEF_KVONLY(a, b) (a)
#else
#define LFS3_IFDEF_KVONLY(a, b) (b)
#endif

#ifdef LFS3_2BONLY
#define LFS3_IFDEF_2BONLY(a, b) (a)
#else
#define LFS3_IFDEF_2BONLY(a, b) (b)
#endif

#ifdef LFS3_REVDBG
#define LFS3_IFDEF_REVDBG(a, b) (a)
#else
#define LFS3_IFDEF_REVDBG(a, b) (b)
#endif

#ifdef LFS3_REVNOISE
#define LFS3_IFDEF_REVNOISE(a, b) (a)
#else
#define LFS3_IFDEF_REVNOISE(a, b) (b)
#endif

#ifdef LFS3_CKPROGS
#define LFS3_IFDEF_CKPROGS(a, b) (a)
#else
#define LFS3_IFDEF_CKPROGS(a, b) (b)
#endif

#ifdef LFS3_CKFETCHES
#define LFS3_IFDEF_CKFETCHES(a, b) (a)
#else
#define LFS3_IFDEF_CKFETCHES(a, b) (b)
#endif

#ifdef LFS3_CKMETAPARITY
#define LFS3_IFDEF_CKMETAPARITY(a, b) (a)
#else
#define LFS3_IFDEF_CKMETAPARITY(a, b) (b)
#endif

#ifdef LFS3_CKDATACKSUMREADS
#define LFS3_IFDEF_CKDATACKSUMREADS(a, b) (a)
#else
#define LFS3_IFDEF_CKDATACKSUMREADS(a, b) (b)
#endif

#ifdef LFS3_GC
#define LFS3_IFDEF_GC(a, b) (a)
#else
#define LFS3_IFDEF_GC(a, b) (b)
#endif


// Some function attributes, no way around these

// Force a function to be inlined
#if !defined(LFS3_NO_BUILTINS) && defined(__GNUC__)
#define LFS3_FORCEINLINE __attribute__((always_inline))
#else
#define LFS3_FORCEINLINE
#endif

// Force a function to _not_ be inlined
#if !defined(LFS3_NO_BUILTINS) && defined(__GNUC__)
#define LFS3_NOINLINE __attribute__((noinline))
#else
#define LFS3_NOINLINE
#endif


// Builtin functions, these may be replaced by more efficient
// toolchain-specific implementations. LFS3_NO_BUILTINS falls back to a more
// expensive basic C implementation for debugging purposes
//
// Most of the backup implementations are based on the infamous Bit
// Twiddling Hacks compiled by Sean Eron Anderson:
// https://graphics.stanford.edu/~seander/bithacks.html
//

// Compile time min/max
#define LFS3_MIN(a, b) ((a < b) ? a : b)
#define LFS3_MAX(a, b) ((a > b) ? a : b)

// Min/max functions for unsigned 32-bit numbers
static inline uint32_t lfs3_min(uint32_t a, uint32_t b) {
    return (a < b) ? a : b;
}

static inline uint32_t lfs3_max(uint32_t a, uint32_t b) {
    return (a > b) ? a : b;
}

static inline int32_t lfs3_smin(int32_t a, int32_t b) {
    return (a < b) ? a : b;
}

static inline int32_t lfs3_smax(int32_t a, int32_t b) {
    return (a > b) ? a : b;
}

// Absolute value of signed numbers
static inline int32_t lfs3_abs(int32_t a) {
    return (a < 0) ? -a : a;
}

// Swap two variables
#define LFS3_SWAP(_t, _a, _b) \
    do { \
        _t *a = _a; \
        _t *b = _b; \
        _t t = *a; \
        *a = *b; \
        *b = t; \
    } while (0)

// Align to nearest multiple of a size
static inline uint32_t lfs3_aligndown(uint32_t a, uint32_t alignment) {
    return a - (a % alignment);
}

static inline uint32_t lfs3_alignup(uint32_t a, uint32_t alignment) {
    return lfs3_aligndown(a + alignment-1, alignment);
}

// Find the smallest power of 2 greater than or equal to a
static inline uint32_t lfs3_nlog2(uint32_t a) {
    // __builtin_clz of zero is undefined, so treat both 0 and 1 specially
    if (a <= 1) {
        return a;
    }

#if !defined(LFS3_NO_BUILTINS) && (defined(__GNUC__) || defined(__CC_ARM))
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

// Count the number of trailing binary zeros in a
// lfs3_ctz(0) may be undefined
static inline uint32_t lfs3_ctz(uint32_t a) {
#if !defined(LFS3_NO_BUILTINS) && defined(__GNUC__)
    return __builtin_ctz(a);
#else
    return lfs3_nlog2((a & -a) + 1) - 1;
#endif
}

// Count the number of binary ones in a
static inline uint32_t lfs3_popc(uint32_t a) {
#if !defined(LFS3_NO_BUILTINS) && (defined(__GNUC__) || defined(__CC_ARM))
    return __builtin_popcount(a);
#else
    a = a - ((a >> 1) & 0x55555555);
    a = (a & 0x33333333) + ((a >> 2) & 0x33333333);
    a = (a + (a >> 4)) & 0x0f0f0f0f;
    return (a * 0x1010101) >> 24;
#endif
}

// Returns true if there is an odd number of binary ones in a
static inline bool lfs3_parity(uint32_t a) {
#if !defined(LFS3_NO_BUILTINS) && (defined(__GNUC__) || defined(__CC_ARM))
    return __builtin_parity(a);
#else
    a ^= a >> 16;
    a ^= a >> 8;
    a ^= a >> 4;
    return (0x6996 >> (a & 0xf)) & 1;
#endif
}

// Find the sequence comparison of a and b, this is the distance
// between a and b ignoring overflow
static inline int lfs3_scmp(uint32_t a, uint32_t b) {
    return (int)(unsigned)(a - b);
}

// Perform polynomial/carry-less multiplication
//
// This is a multiply where all adds are replaced with xors. If we view
// a and b as binary polynomials, xor is polynomial addition and pmul is
// polynomial multiplication.
static inline uint64_t lfs3_pmul(uint32_t a, uint32_t b) {
    uint64_t r = 0;
    uint64_t a_ = a;
    while (b) {
        if (b & 1) {
            r ^= a_;
        }
        a_ <<= 1;
        b >>= 1;
    }
    return r;
}


// Convert to/from 32-bit little-endian
static inline void lfs3_tole32(uint32_t word, void *buffer) {
    ((uint8_t*)buffer)[0] = word >>  0;
    ((uint8_t*)buffer)[1] = word >>  8;
    ((uint8_t*)buffer)[2] = word >> 16;
    ((uint8_t*)buffer)[3] = word >> 24;
}

static inline uint32_t lfs3_fromle32(const void *buffer) {
    return (((uint8_t*)buffer)[0] <<  0)
         | (((uint8_t*)buffer)[1] <<  8)
         | (((uint8_t*)buffer)[2] << 16)
         | (((uint8_t*)buffer)[3] << 24);
}

// Convert to/from leb128 encoding
// TODO should we really be using ssize_t here and not lfs3_ssize_t?
ssize_t lfs3_toleb128(uint32_t word, void *buffer, size_t size);

ssize_t lfs3_fromleb128(uint32_t *word, const void *buffer, size_t size);


// Compare n bytes of memory
#if !defined(LFS3_NO_STRINGH)
#define lfs3_memcmp memcmp
#elif !defined(LFS3_NO_BUILTINS)
#define lfs3_memcmp __builtin_memcmp
#else
static inline int lfs3_memcmp(const void *a, const void *b, size_t size) {
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
#if !defined(LFS3_NO_STRINGH)
#define lfs3_memcpy memcpy
#elif !defined(LFS3_NO_BUILTINS)
#define lfs3_memcpy __builtin_memcpy
#else
static inline void *lfs3_memcpy(
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
#if !defined(LFS3_NO_STRINGH)
#define lfs3_memmove memmove
#elif !defined(LFS3_NO_BUILTINS)
#define lfs3_memmove __builtin_memmove
#else
static inline void *lfs3_memmove(void *dst, const void *src, size_t size) {
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
#if !defined(LFS3_NO_STRINGH)
#define lfs3_memset memset
#elif !defined(LFS3_NO_BUILTINS)
#define lfs3_memset __builtin_memset
#else
static inline void *lfs3_memset(void *dst, int c, size_t size) {
    uint8_t *dst_ = dst;
    for (size_t i = 0; i < size; i++) {
        dst_[i] = c;
    }

    return dst_;
}
#endif

// Find the first occurrence of c or NULL
#if !defined(LFS3_NO_STRINGH)
#define lfs3_memchr memchr
#else
static inline void *lfs3_memchr(const void *a, int c, size_t size) {
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
static inline void *lfs3_memcchr(const void *a, int c, size_t size) {
    const uint8_t *a_ = a;
    for (size_t i = 0; i < size; i++) {
        if (a_[i] != c) {
            return (void*)&a_[i];
        }
    }

    return NULL;
}

// Find the minimum length that includes all non-zero bytes
static inline size_t lfs3_memlen(const void *a, size_t size) {
    const uint8_t *a_ = a;
    while (size > 0 && a_[size-1] == 0) {
        size -= 1;
    }

    return size;
}

// Xor n bytes from b into a
static inline void *lfs3_memxor(
        void *restrict a, const void *restrict b, size_t size) {
    uint8_t *a_ = a;
    const uint8_t *b_ = b;
    for (size_t i = 0; i < size; i++) {
        a_[i] ^= b_[i];
    }

    return a_;
}


// Find the length of a null-terminated string
#if !defined(LFS3_NO_STRINGH)
#define lfs3_strlen strlen
#else
static inline size_t lfs3_strlen(const char *a) {
    const char *a_ = a;
    while (*a_) {
        a_++;
    }

    return a_ - a;
}
#endif

// Compare two null-terminated strings
#if !defined(LFS3_NO_STRINGH)
#define lfs3_strcmp strcmp
#else
static inline int lfs3_strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }

    return (int)*a - (int)*b;
}
#endif

// Copy a null-terminated string from src to dst
#if !defined(LFS3_NO_STRINGH)
#define lfs3_strcpy strcpy
#else
static inline char *lfs3_strcpy(
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
#ifndef LFS3_NO_STRINGH
#define lfs3_strchr strchr
#else
static inline char *lfs3_strchr(const char *a, int c) {
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
static inline char *lfs3_strcchr(const char *a, int c) {
    while (*a) {
        if (*a != c) {
            return (char*)a;
        }

        a++;
    }

    return NULL;
}

// Find length of a that does not contain any char in cs
#ifndef LFS3_NO_STRINGH
#define lfs3_strspn strspn
#else
static inline size_t lfs3_strspn(const char *a, const char *cs) {
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
#ifndef LFS3_NO_STRINGH
#define lfs3_strcspn strcspn
#else
static inline size_t lfs3_strcspn(const char *a, const char *cs) {
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


// Odd-parity and even-parity zeros in our crc32c ring
#define LFS3_CRC32C_ODDZERO  0xfca42daf
#define LFS3_CRC32C_EVENZERO 0x00000000

// Calculate crc32c incrementally
//
// polynomial = 0x11edc6f41
// init = 0xffffffff
// fini = 0xffffffff
//
uint32_t lfs3_crc32c(uint32_t crc, const void *buffer, size_t size);

// Multiply two crc32cs in the crc32c ring
uint32_t lfs3_crc32c_mul(uint32_t a, uint32_t b);

// Find the cube of a crc32c in the crc32c ring
static inline uint32_t lfs3_crc32c_cube(uint32_t a) {
    return lfs3_crc32c_mul(lfs3_crc32c_mul(a, a), a);
}


// Allocate memory, only used if buffers are not provided to littlefs
#ifndef LFS3_NO_MALLOC
#define lfs3_malloc malloc
#else
static inline void *lfs3_malloc(size_t size) {
    (void)size;
    return NULL;
}
#endif

// Deallocate memory, only used if buffers are not provided to littlefs
#ifndef LFS3_NO_MALLOC
#define lfs3_free free
#else
static inline void lfs3_free(void *p) {
    (void)p;
}
#endif


#endif
#endif
