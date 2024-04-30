#!/usr/bin/env python3

import io
import os
import struct
import sys


TAG_NULL            = 0x0000
TAG_CONFIG          = 0x0000
TAG_MAGIC           = 0x0003
TAG_VERSION         = 0x0004
TAG_RCOMPAT         = 0x0005
TAG_WCOMPAT         = 0x0006
TAG_OCOMPAT         = 0x0007
TAG_GEOMETRY        = 0x0009
TAG_NAMELIMIT       = 0x000c
TAG_SIZELIMIT       = 0x000d
TAG_GDELTA          = 0x0100
TAG_GRMDELTA        = 0x0100
TAG_NAME            = 0x0200
TAG_REG             = 0x0201
TAG_DIR             = 0x0202
TAG_BOOKMARK        = 0x0204
TAG_ORPHAN          = 0x0205
TAG_STRUCT          = 0x0300
TAG_DATA            = 0x0300
TAG_BLOCK           = 0x0304
TAG_BSHRUB          = 0x0308
TAG_BTREE           = 0x030c
TAG_MROOT           = 0x0311
TAG_MDIR            = 0x0315
TAG_MTREE           = 0x031c
TAG_DID             = 0x0320
TAG_BRANCH          = 0x032c
TAG_UATTR           = 0x0400
TAG_SATTR           = 0x0600
TAG_SHRUB           = 0x1000
TAG_CKSUM           = 0x3000
TAG_PERTURB         = 0x3100
TAG_ECKSUM          = 0x3200
TAG_ALT             = 0x4000
TAG_R               = 0x2000
TAG_GT              = 0x1000


# some ways of block geometry representations
# 512      -> 512
# 512x16   -> (512, 16)
# 0x200x10 -> (512, 16)
def bdgeom(s):
    s = s.strip()
    b = 10
    if s.startswith('0x') or s.startswith('0X'):
        s = s[2:]
        b = 16
    elif s.startswith('0o') or s.startswith('0O'):
        s = s[2:]
        b = 8
    elif s.startswith('0b') or s.startswith('0B'):
        s = s[2:]
        b = 2

    if 'x' in s:
        s, s_ = s.split('x', 1)
        return (int(s, b), int(s_, b))
    else:
        return int(s, b)

# parse some rbyd addr encodings
# 0xa       -> [0xa]
# 0xa.c     -> [(0xa, 0xc)]
# 0x{a,b}   -> [0xa, 0xb]
# 0x{a,b}.c -> [(0xa, 0xc), (0xb, 0xc)]
def rbydaddr(s):
    s = s.strip()
    b = 10
    if s.startswith('0x') or s.startswith('0X'):
        s = s[2:]
        b = 16
    elif s.startswith('0o') or s.startswith('0O'):
        s = s[2:]
        b = 8
    elif s.startswith('0b') or s.startswith('0B'):
        s = s[2:]
        b = 2

    trunk = None
    if '.' in s:
        s, s_ = s.split('.', 1)
        trunk = int(s_, b)

    if s.startswith('{') and '}' in s:
        ss = s[1:s.find('}')].split(',')
    else:
        ss = [s]

    addr = []
    for s in ss:
        if trunk is not None:
            addr.append((int(s, b), trunk))
        else:
            addr.append(int(s, b))

    return addr

def fromleb128(data):
    word = 0
    for i, b in enumerate(data):
        word |= ((b & 0x7f) << 7*i)
        word &= 0xffffffff
        if not b & 0x80:
            return word, i+1
    return word, len(data)

def tagrepr(tag, w=None, size=None, off=None):
    if (tag & 0x6fff) == TAG_NULL:
        return '%snull%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            ' w%d' % w if w else '',
            ' %d' % size if size else '')
    elif (tag & 0x6f00) == TAG_CONFIG:
        return '%s%s%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            'magic' if (tag & 0xfff) == TAG_MAGIC
                else 'version' if (tag & 0xfff) == TAG_VERSION
                else 'rcompat' if (tag & 0xfff) == TAG_RCOMPAT
                else 'wcompat' if (tag & 0xfff) == TAG_WCOMPAT
                else 'ocompat' if (tag & 0xfff) == TAG_OCOMPAT
                else 'geometry' if (tag & 0xfff) == TAG_GEOMETRY
                else 'sizelimit' if (tag & 0xfff) == TAG_SIZELIMIT
                else 'namelimit' if (tag & 0xfff) == TAG_NAMELIMIT
                else 'config 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6f00) == TAG_GDELTA:
        return '%s%s%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            'grmdelta' if (tag & 0xfff) == TAG_GRMDELTA
                else 'gdelta 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6f00) == TAG_NAME:
        return '%s%s%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            'name' if (tag & 0xfff) == TAG_NAME
                else 'reg' if (tag & 0xfff) == TAG_REG
                else 'dir' if (tag & 0xfff) == TAG_DIR
                else 'orphan' if (tag & 0xfff) == TAG_ORPHAN
                else 'bookmark' if (tag & 0xfff) == TAG_BOOKMARK
                else 'name 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
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
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6e00) == TAG_UATTR:
        return '%suattr 0x%02x%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            ((tag & 0x100) >> 1) | (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6e00) == TAG_SATTR:
        return '%ssattr 0x%02x%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            ((tag & 0x100) >> 1) | (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_CKSUM:
        return 'cksum 0x%02x%s%s' % (
            tag & 0xff,
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_PERTURB:
        return 'perturb%s%s%s' % (
            ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_ECKSUM:
        return 'ecksum%s%s%s' % (
            ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif tag & TAG_ALT:
        return 'alt%s%s%s%s%s' % (
            'r' if tag & TAG_R else 'b',
            'a' if tag & 0x0fff == 0 and tag & TAG_GT
                else 'n' if tag & 0x0fff == 0
                else 'gt' if tag & TAG_GT
                else 'le',
            ' 0x%x' % (tag & 0x0fff) if tag & 0x0fff != 0 else '',
            ' w%d' % w if w is not None else '',
            ' 0x%x' % (0xffffffff & (off-size))
                if size and off is not None
                else ' -%d' % size if size
                else '')
    else:
        return '0x%04x%s%s' % (
            tag,
            ' w%d' % w if w is not None else '',
            ' %d' % size if size is not None else '')


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
        block_size=None,
        block_count=None,
        off=None,
        **args):
    # interpret as a sequence of hex bytes
    if args.get('hex'):
        dbg_tag(bytes(int(tag, 16) for tag in tags))

    # interpret as strings
    elif args.get('string'):
        for tag in tags:
            dbg_tag(tag.encode('utf8'))

    # default to interpreting as ints
    elif block_size is None and off is None:
        for tag in tags:
            dbg_tag(int(tag, 0))

    # if either block_size or off provided interpret as a block device
    else:
        disk, *blocks = tags
        blocks = [rbydaddr(block) for block in blocks]

        # is bd geometry specified?
        if isinstance(block_size, tuple):
            block_size, block_count_ = block_size
            if block_count is None:
                block_count = block_count_

        # flatten block, default to block 0
        if not blocks:
            blocks = [[0]]
        blocks = [block for blocks_ in blocks for block in blocks_]

        with open(disk, 'rb') as f:
            # if block_size is omitted, assume the block device is one big block
            if block_size is None:
                f.seek(0, os.SEEK_END)
                block_size = f.tell()

            # blocks may also encode offsets
            blocks, offs = (
                [block[0] if isinstance(block, tuple) else block
                    for block in blocks],
                [off if off is not None
                        else block[1] if isinstance(block, tuple)
                        else None
                    for block in blocks])

            # read each tag
            for block, off in zip(blocks, offs):
                f.seek((block * block_size) + (off or 0))
                # read maximum tag size
                data = f.read(2+5+5)
                dbg_tag(data)

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
        '-x', '--hex',
        action='store_true',
        help="Interpret as a sequence of hex bytes.")
    parser.add_argument(
        '-s', '--string',
        action='store_true',
        help="Interpret as strings.")
    parser.add_argument(
        '-b', '--block-size',
        type=bdgeom,
        help="Block size/geometry in bytes.")
    parser.add_argument(
        '--block-count',
        type=lambda x: int(x, 0),
        help="Block count in blocks.")
    parser.add_argument(
        '--off',
        type=lambda x: int(x, 0),
        help="Use this offset.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
