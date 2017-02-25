/*
 * Simple config parser
 *
 * Copyright (c) 2017 Christopher Haster
 * Distributed under the MIT license
 */
#include "emubd/lfs_cfg.h"

#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>


static int lfs_cfg_buffer(lfs_cfg_t *cfg, char c) {
    // Amortize double
    if (cfg->blen == cfg->bsize) {
        size_t nsize = cfg->bsize * 2;
        char *nbuf = malloc(nsize);
        if (!nbuf) {
            return -ENOMEM;
        }

        memcpy(nbuf, cfg->buf, cfg->bsize);
        free(cfg->buf);
        cfg->buf = nbuf;
        cfg->bsize = nsize;
    }

    cfg->buf[cfg->blen] = c;
    cfg->blen += 1;
    return 0;
}

static int lfs_cfg_attr(lfs_cfg_t *cfg, unsigned key, unsigned val) {
    // Amortize double
    if (cfg->len == cfg->size) {
        size_t nsize = cfg->size * 2;
        struct lfs_cfg_attr *nattrs = malloc(nsize*sizeof(struct lfs_cfg_attr));
        if (!nattrs) {
            return -ENOMEM;
        }

        memcpy(nattrs, cfg->attrs, cfg->size*sizeof(struct lfs_cfg_attr));
        free(cfg->attrs);
        cfg->attrs = nattrs;
        cfg->size = nsize;
    }

    // Keep attrs sorted for binary search
    unsigned i = 0;
    while (i < cfg->len &&
            strcmp(&cfg->buf[key],
                &cfg->buf[cfg->attrs[i].key]) > 0) {
        i += 1;
    }

    memmove(&cfg->attrs[i+1], &cfg->attrs[i],
        (cfg->size - i)*sizeof(struct lfs_cfg_attr));
    cfg->attrs[i].key = key;
    cfg->attrs[i].val = val;
    cfg->len += 1;
    return 0;
}

static bool lfs_cfg_match(FILE *f, const char *matches) {
    char c = getc(f);
    ungetc(c, f);

    for (int i = 0; matches[i]; i++) {
        if (c == matches[i]) {
            return true;
        }
    }

    return false;
}

int lfs_cfg_create(lfs_cfg_t *cfg, const char *filename) {
    // start with some initial space
    cfg->len = 0;
    cfg->size = 4;
    cfg->attrs = malloc(cfg->size*sizeof(struct lfs_cfg_attr));

    cfg->blen = 0;
    cfg->bsize = 16;
    cfg->buf = malloc(cfg->size);

    FILE *f = fopen(filename, "r");
    if (!f) {
        return -errno;
    }

    while (!feof(f)) {
        int err;

        while (lfs_cfg_match(f, " \t\v\f")) {
            fgetc(f);
        }

        if (!lfs_cfg_match(f, "#\r\n")) {
            unsigned key = cfg->blen;
            while (!lfs_cfg_match(f, " \t\v\f:#") && !feof(f)) {
                if ((err = lfs_cfg_buffer(cfg, fgetc(f)))) {
                    return err;
                }
            }
            if ((err = lfs_cfg_buffer(cfg, 0))) {
                return err;
            }

            while (lfs_cfg_match(f, " \t\v\f")) {
                fgetc(f);
            }

            if (lfs_cfg_match(f, ":")) {
                fgetc(f);
                while (lfs_cfg_match(f, " \t\v\f")) {
                    fgetc(f);
                }

                unsigned val = cfg->blen;
                while (!lfs_cfg_match(f, " \t\v\f#\r\n") && !feof(f)) {
                    if ((err = lfs_cfg_buffer(cfg, fgetc(f)))) {
                        return err;
                    }
                }
                if ((err = lfs_cfg_buffer(cfg, 0))) {
                    return err;
                }

                if ((err = lfs_cfg_attr(cfg, key, val))) {
                    return err;
                }
            } else {
                cfg->blen = key;
            }
        }

        while (!lfs_cfg_match(f, "\r\n") && !feof(f)) {
            fgetc(f);
        }
        fgetc(f);
    }

    return 0;
}

void lfs_cfg_destroy(lfs_cfg_t *cfg) {
    free(cfg->attrs);
}

bool lfs_cfg_has(lfs_cfg_t *cfg, const char *key) {
    return lfs_cfg_get(cfg, key, 0);
}

const char *lfs_cfg_get(lfs_cfg_t *cfg, const char *key, const char *def) {
    // binary search for attribute
    int lo = 0;
    int hi = cfg->len-1;

    while (lo <= hi) {
        int i = (hi + lo) / 2;
        int cmp = strcmp(key, &cfg->buf[cfg->attrs[i].key]);
        if (cmp == 0) {
            return &cfg->buf[cfg->attrs[i].val];
        } else if (cmp < 0) {
            hi = i-1;
        } else {
            lo = i+1;
        }
    }

    return def;
}

ssize_t lfs_cfg_geti(lfs_cfg_t *cfg, const char *key, ssize_t def) {
    const char *val = lfs_cfg_get(cfg, key, 0);
    if (!val) {
        return def;
    }

    char *end;
    ssize_t res = strtoll(val, &end, 0);
    return (end == val) ? def : res;
}

size_t lfs_cfg_getu(lfs_cfg_t *cfg, const char *key, size_t def) {
    const char *val = lfs_cfg_get(cfg, key, 0);
    if (!val) {
        return def;
    }

    char *end;
    size_t res = strtoull(val, &end, 0);
    return (end == val) ? def : res;
}
