/*
 * Simple config parser
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#ifndef LFS_CFG_H
#define LFS_CFG_H

#include <stddef.h>
#include <stdbool.h>
#include <unistd.h>

// This is a simple parser for config files
//
// The cfg file format is dumb simple. Attributes are
// key value pairs separated by a single colon. Delimited
// by comments (#) and newlines (\r\n) and trims
// whitespace ( \t\v\f)
//
// Here's an example file
// # Here is a dump example
// looky: it's_an_attribute
// hey_look: another_attribute
//
// huh: yeah_that's_basically_it # basically it

// Internal config structure
typedef struct lfs_cfg {
    size_t len;
    size_t size;

    size_t blen;
    size_t bsize;
    char *buf;

    struct lfs_cfg_attr {
        unsigned key;
        unsigned val;
    } *attrs;
} lfs_cfg_t;



// Creates a cfg object and reads in the cfg file from the filename
//
// If the lfs_cfg_read fails, returns a negative value from the underlying
// stdio functions
int lfs_cfg_create(lfs_cfg_t *cfg, const char *filename);

// Destroys the cfg object and frees any used memory
void lfs_cfg_destroy(lfs_cfg_t *cfg);

// Checks if a cfg attribute exists
bool lfs_cfg_has(lfs_cfg_t *cfg, const char *key);

// Retrieves a cfg attribute as a null-terminated string
//
// If the attribute does not exist, returns the string passed as def
const char *lfs_cfg_get(lfs_cfg_t *cfg, const char *key, const char *def);

// Retrieves a cfg attribute parsed as an int
//
// If the attribute does not exist or can't be parsed, returns the
// integer passed as def
ssize_t lfs_cfg_geti(lfs_cfg_t *cfg, const char *name, ssize_t def);

// Retrieves a cfg attribute parsed as an unsigned int
//
// If the attribute does not exist or can't be parsed, returns the
// integer passed as def
size_t lfs_cfg_getu(lfs_cfg_t *cfg, const char *name, size_t def);

#endif
