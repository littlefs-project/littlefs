#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import io
import os
import struct
import sys


TAG_NULL        = 0x0000    ## 0x0000  v--- ---- ---- ----
TAG_CONFIG      = 0x0000    ## 0x00tt  v--- ---- -ttt tttt
TAG_MAGIC       = 0x0003    #  0x0003  v--- ---- ---- --11
TAG_VERSION     = 0x0004    #  0x0004  v--- ---- ---- -1--
TAG_RCOMPAT     = 0x0005    #  0x0005  v--- ---- ---- -1-1
TAG_WCOMPAT     = 0x0006    #  0x0006  v--- ---- ---- -11-
TAG_OCOMPAT     = 0x0007    #  0x0007  v--- ---- ---- -111
TAG_GEOMETRY    = 0x0009    #  0x0008  v--- ---- ---- 1-rr
TAG_NAMELIMIT   = 0x000c    #  0x000c  v--- ---- ---- 11--
TAG_FILELIMIT   = 0x000d    #  0x000d  v--- ---- ---- 11-1
TAG_GDELTA      = 0x0100    ## 0x01tt  v--- ---1 -ttt tttt
TAG_GRMDELTA    = 0x0100    #  0x0100  v--- ---1 ---- ----
TAG_NAME        = 0x0200    ## 0x02tt  v--- --1- -ttt tttt
TAG_REG         = 0x0201    #  0x0201  v--- --1- ---- ---1
TAG_DIR         = 0x0202    #  0x0202  v--- --1- ---- --1-
TAG_BOOKMARK    = 0x0204    #  0x0204  v--- --1- ---- -1--
TAG_STICKYNOTE  = 0x0205    #  0x0205  v--- --1- ---- -1-1
TAG_STRUCT      = 0x0300    ## 0x03tt  v--- --11 -ttt tttt
TAG_DATA        = 0x0300    #  0x0300  v--- --11 ---- ----
TAG_BLOCK       = 0x0304    #  0x0304  v--- --11 ---- -1rr
TAG_BSHRUB      = 0x0308    #  0x0308  v--- --11 ---- 1---
TAG_BTREE       = 0x030c    #  0x030c  v--- --11 ---- 11rr
TAG_MROOT       = 0x0311    #  0x0310  v--- --11 ---1 --rr
TAG_MDIR        = 0x0315    #  0x0314  v--- --11 ---1 -1rr
TAG_MTREE       = 0x031c    #  0x031c  v--- --11 ---1 11rr
TAG_DID         = 0x0320    #  0x0320  v--- --11 --1- ----
TAG_BRANCH      = 0x032c    #  0x032c  v--- --11 --1- 11rr
TAG_ATTR        = 0x0400    ## 0x04aa  v--- -1-a -aaa aaaa
TAG_UATTR       = 0x0400    #  0x04aa  v--- -1-- -aaa aaaa
TAG_SATTR       = 0x0500    #  0x05aa  v--- -1-1 -aaa aaaa
TAG_SHRUB       = 0x1000    ## 0x1kkk  v--1 kkkk -kkk kkkk
TAG_ALT         = 0x4000    ## 0x4kkk  v1cd kkkk -kkk kkkk
TAG_B           = 0x0000
TAG_R           = 0x2000
TAG_LE          = 0x0000
TAG_GT          = 0x1000
TAG_CKSUM       = 0x3000    ## 0x300p  v-11 ---- ---- ---p
TAG_P           = 0x0001
TAG_NOTE        = 0x3100    ## 0x3100  v-11 ---1 ---- ----
TAG_ECKSUM      = 0x3200    ## 0x3200  v-11 --1- ---- ----
TAG_GCKSUMDELTA = 0x3300    ## 0x3300  v-11 --11 ---- ----


def fromleb128(data):
    word = 0
    for i, b in enumerate(data):
        word |= ((b & 0x7f) << 7*i)
        word &= 0xffffffff
        if not b & 0x80:
            return word, i+1
    return word, len(data)

# human readable tag repr
def tagrepr(tag, weight=None, size=None, *,
        global_=False,
        toff=None):
    # null tags
    if (tag & 0x6fff) == TAG_NULL:
        return '%snull%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                ' w%d' % weight if weight else '',
                ' %d' % size if size else '')
    # config tags
    elif (tag & 0x6f00) == TAG_CONFIG:
        return '%s%s%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                'magic' if (tag & 0xfff) == TAG_MAGIC
                    else 'version' if (tag & 0xfff) == TAG_VERSION
                    else 'rcompat' if (tag & 0xfff) == TAG_RCOMPAT
                    else 'wcompat' if (tag & 0xfff) == TAG_WCOMPAT
                    else 'ocompat' if (tag & 0xfff) == TAG_OCOMPAT
                    else 'geometry' if (tag & 0xfff) == TAG_GEOMETRY
                    else 'namelimit' if (tag & 0xfff) == TAG_NAMELIMIT
                    else 'filelimit' if (tag & 0xfff) == TAG_FILELIMIT
                    else 'config 0x%02x' % (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # global-state delta tags
    elif (tag & 0x6f00) == TAG_GDELTA:
        if global_:
            return '%s%s%s%s' % (
                    'shrub' if tag & TAG_SHRUB else '',
                    'grm' if (tag & 0xfff) == TAG_GRMDELTA
                        else 'gstate 0x%02x' % (tag & 0xff),
                    ' w%d' % weight if weight else '',
                    ' %s' % size if size is not None else '')
        else:
            return '%s%s%s%s' % (
                    'shrub' if tag & TAG_SHRUB else '',
                    'grmdelta' if (tag & 0xfff) == TAG_GRMDELTA
                        else 'gdelta 0x%02x' % (tag & 0xff),
                    ' w%d' % weight if weight else '',
                    ' %s' % size if size is not None else '')
    # name tags, includes file types
    elif (tag & 0x6f00) == TAG_NAME:
        return '%s%s%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                'name' if (tag & 0xfff) == TAG_NAME
                    else 'reg' if (tag & 0xfff) == TAG_REG
                    else 'dir' if (tag & 0xfff) == TAG_DIR
                    else 'bookmark' if (tag & 0xfff) == TAG_BOOKMARK
                    else 'stickynote' if (tag & 0xfff) == TAG_STICKYNOTE
                    else 'name 0x%02x' % (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # structure tags
    elif (tag & 0x6f00) == TAG_STRUCT:
        return '%s%s%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                'data' if (tag & 0xfff) == TAG_DATA
                    else 'block' if (tag & 0xfff) == TAG_BLOCK
                    else 'bshrub' if (tag & 0xfff) == TAG_BSHRUB
                    else 'btree' if (tag & 0xfff) == TAG_BTREE
                    else 'mroot' if (tag & 0xfff) == TAG_MROOT
                    else 'mdir' if (tag & 0xfff) == TAG_MDIR
                    else 'mtree' if (tag & 0xfff) == TAG_MTREE
                    else 'did' if (tag & 0xfff) == TAG_DID
                    else 'branch' if (tag & 0xfff) == TAG_BRANCH
                    else 'struct 0x%02x' % (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # custom attributes
    elif (tag & 0x6e00) == TAG_ATTR:
        return '%s%sattr 0x%02x%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                's' if tag & 0x100 else 'u',
                ((tag & 0x100) >> 1) ^ (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # alt pointers
    elif tag & TAG_ALT:
        return 'alt%s%s 0x%03x%s%s' % (
                'r' if tag & TAG_R else 'b',
                'gt' if tag & TAG_GT else 'le',
                tag & 0x0fff,
                ' w%d' % weight if weight is not None else '',
                ' 0x%x' % (0xffffffff & (toff-size))
                    if size and toff is not None
                    else ' -%d' % size if size
                    else '')
    # checksum tags
    elif (tag & 0x7f00) == TAG_CKSUM:
        return 'cksum%s%s%s%s' % (
                'p' if not tag & 0xfe and tag & TAG_P else '',
                ' 0x%02x' % (tag & 0xff) if tag & 0xfe else '',
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # note tags
    elif (tag & 0x7f00) == TAG_NOTE:
        return 'note%s%s%s' % (
                ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # erased-state checksum tags
    elif (tag & 0x7f00) == TAG_ECKSUM:
        return 'ecksum%s%s%s' % (
                ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # global-checksum delta tags
    elif (tag & 0x7f00) == TAG_GCKSUMDELTA:
        if global_:
            return 'gcksum%s%s%s' % (
                    ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
                    ' w%d' % weight if weight else '',
                    ' %s' % size if size is not None else '')
        else:
            return 'gcksumdelta%s%s%s' % (
                    ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
                    ' w%d' % weight if weight else '',
                    ' %s' % size if size is not None else '')
    # unknown tags
    else:
        return '0x%04x%s%s' % (
                tag,
                ' w%d' % weight if weight is not None else '',
                ' %d' % size if size is not None else '')


def list_tags():
    # here be magic, parse our script's source to get to the tag comments
    import inspect
    import re
    tags = []
    tag_pattern = re.compile(
            '^(?P<name>TAG_[^ ]*) *= *(?P<tag>[^ #]+) *#+ *(?P<comment>.*)$')
    for line in inspect.getsourcelines(
            inspect.getmodule(inspect.currentframe()))[0]:
        m = tag_pattern.match(line)
        if m:
            tags.append(m.groups())

    # find widths for alignment
    w = [0]
    for n, t, c in tags:
        w[0] = max(w[0], len('LFSR_'+n))

    # print
    for n, t, c in tags:
        print('%-*s  %s' % (
                w[0], 'LFSR_'+n,
                c))

def dbg_tag(data):
    if isinstance(data, int):
        tag = data
        weight = None
        size = None
    else:
        data = data.ljust(2, b'\0')
        tag = (data[0] << 8) | data[1]
        weight, d = fromleb128(data[2:]) if 2 < len(data) else (None, 2)
        size, d_ = fromleb128(data[2+d:]) if d < len(data) else (None, d)

    print(tagrepr(tag, weight, size))

def main(tags, *,
        list=False,
        block_size=None,
        block_count=None,
        off=None,
        **args):
    list_ = list; del list

    # list all known tags
    if list_:
        list_tags()

    # try to decode these tags
    else:
        # interpret as a sequence of hex bytes
        if args.get('hex'):
            bytes_ = [b for tag in tags for b in tag.split()]
            dbg_tag(bytes(int(b, 16) for b in bytes_))

        # default to interpreting as ints
        else:
            for tag in tags:
                dbg_tag(int(tag, 0))


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Decode littlefs tags.",
            allow_abbrev=False)
    parser.add_argument(
            'tags',
            nargs='*',
            help="Tags to decode.")
    parser.add_argument(
            '-l', '--list',
            action='store_true',
            help="List all known tags.")
    parser.add_argument(
            '-x', '--hex',
            action='store_true',
            help="Interpret as a sequence of hex bytes.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
