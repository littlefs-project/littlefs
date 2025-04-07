#!/usr/bin/env python3
#
# Inspired by d3 and brendangregg's flamegraph svg:
# - https://d3js.org
# - https://github.com/brendangregg/FlameGraph
#

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import bisect
import collections as co
import fnmatch
import functools as ft
import itertools as it
import json
import math as mt
import re
import shlex
import struct


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



# assign colors to specific filesystem objects
#
# some nicer colors borrowed from Seaborn
# note these include a non-opaque alpha
#
# COLORS = [
#     '#7995c4', # was '#4c72b0bf', # blue
#     '#e6a37d', # was '#dd8452bf', # orange
#     '#80be8e', # was '#55a868bf', # green
#     '#d37a7d', # was '#c44e52bf', # red
#     '#a195c6', # was '#8172b3bf', # purple
#     '#ae9a88', # was '#937860bf', # brown
#     '#e3a8d2', # was '#da8bc3bf', # pink
#     '#a9a9a9', # was '#8c8c8cbf', # gray
#     '#d9cb97', # was '#ccb974bf', # yellow
#     '#8bc8da', # was '#64b5cdbf', # cyan
# ]
# COLORS_DARK = [
#     '#7997b7', # was '#a1c9f4bf', # blue
#     '#bf8761', # was '#ffb482bf', # orange
#     '#6aac79', # was '#8de5a1bf', # green
#     '#bf7774', # was '#ff9f9bbf', # red
#     '#9c8cbf', # was '#d0bbffbf', # purple
#     '#a68c74', # was '#debb9bbf', # brown
#     '#bb84ab', # was '#fab0e4bf', # pink
#     '#9b9b9b', # was '#cfcfcfbf', # gray
#     '#bfbe7a', # was '#fffea3bf', # yellow
#     '#8bb5b4', # was '#b9f2f0bf', # cyan
# ]
#
COLORS = {
    'mdir':     '#d9cb97', # was '#ccb974bf', # yellow
    'btree':    '#7995c4', # was '#4c72b0bf', # blue
    'data':     '#80be8e', # was '#55a868bf', # green
    'corrupt':  '#d37a7d', # was '#c44e52bf', # red
    'conflict': '#d37a7d', # was '#c44e52bf', # red
    'unused':   '#e5e5e5', # light gray
}
COLORS_DARK = {
    'mdir':     '#bfbe7a', # was '#fffea3bf', # yellow
    'btree':    '#7997b7', # was '#a1c9f4bf', # blue
    'data':     '#6aac79', # was '#8de5a1bf', # green
    'corrupt':  '#bf7774', # was '#ff9f9bbf', # red
    'conflict': '#bf7774', # was '#ff9f9bbf', # red
    'unused':   '#333333', # dark gray
}

WIDTH = 750
HEIGHT = 350
FONT = ['sans-serif']
FONT_SIZE = 10


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    import os
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

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
# 0xa       -> (0xa,)
# 0xa.c     -> ((0xa, 0xc),)
# 0x{a,b}   -> (0xa, 0xb)
# 0x{a,b}.c -> ((0xa, 0xc), (0xb, 0xc))
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

    return tuple(addr)

def crc32c(data, crc=0):
    crc ^= 0xffffffff
    for b in data:
        crc ^= b
        for j in range(8):
            crc = (crc >> 1) ^ ((crc & 1) * 0x82f63b78)
    return 0xffffffff ^ crc

def pmul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a <<= 1
        b >>= 1
    return r

def crc32cmul(a, b):
    r = pmul(a, b)
    for _ in range(31):
        r = (r >> 1) ^ ((r & 1) * 0x82f63b78)
    return r

def crc32ccube(a):
    return crc32cmul(crc32cmul(a, a), a)

def popc(x):
    return bin(x).count('1')

def parity(x):
    return popc(x) & 1

def fromle32(data):
    return struct.unpack('<I', data[0:4].ljust(4, b'\0'))[0]

def fromleb128(data):
    word = 0
    for i, b in enumerate(data):
        word |= ((b & 0x7f) << 7*i)
        word &= 0xffffffff
        if not b & 0x80:
            return word, i+1
    return word, len(data)

def fromtag(data):
    data = data.ljust(4, b'\0')
    tag = (data[0] << 8) | data[1]
    weight, d = fromleb128(data[2:])
    size, d_ = fromleb128(data[2+d:])
    return tag>>15, tag&0x7fff, weight, size, 2+d+d_

def frommdir(data):
    blocks = []
    d = 0
    while d < len(data):
        block, d_ = fromleb128(data[d:])
        blocks.append(block)
        d += d_
    return blocks

def fromshrub(data):
    d = 0
    weight, d_ = fromleb128(data[d:]); d += d_
    trunk, d_ = fromleb128(data[d:]); d += d_
    return weight, trunk

def frombranch(data):
    d = 0
    block, d_ = fromleb128(data[d:]); d += d_
    trunk, d_ = fromleb128(data[d:]); d += d_
    cksum = fromle32(data[d:]); d += 4
    return block, trunk, cksum

def frombtree(data):
    d = 0
    w, d_ = fromleb128(data[d:]); d += d_
    block, trunk, cksum = frombranch(data[d:])
    return w, block, trunk, cksum

def frombptr(data):
    d = 0
    size, d_ = fromleb128(data[d:]); d += d_
    block, d_ = fromleb128(data[d:]); d += d_
    off, d_ = fromleb128(data[d:]); d += d_
    cksize, d_ = fromleb128(data[d:]); d += d_
    cksum = fromle32(data[d:]); d += 4
    return size, block, off, cksize, cksum

def xxd(data, width=16):
    for i in range(0, len(data), width):
        yield '%-*s %-*s' % (
                3*width,
                ' '.join('%02x' % b for b in data[i:i+width]),
                width,
                ''.join(
                    b if b >= ' ' and b <= '~' else '.'
                        for b in map(chr, data[i:i+width])))

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

# compute the difference between two paths, returning everything
# in a after the paths diverge, as well as the relevant index
def pathdelta(a, b):
    if not isinstance(a, list):
        a = list(a)
    i = 0
    for a_, b_ in zip(a, b):
        try:
            if type(a_) == type(b_) and a_ == b_:
                i += 1
            else:
                break
        # treat exceptions here as failure to match, most likely
        # the compared types are incompatible, it's the caller's
        # problem
        except Exception:
            break

    return [(i+j, a_) for j, a_ in enumerate(a[i:])]


# a simple wrapper over an open file with bd geometry
class Bd:
    def __init__(self, f, block_size=None, block_count=None):
        self.f = f
        self.block_size = block_size
        self.block_count = block_count

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return 'bd %sx%s' % (self.block_size, self.block_count)

    def read(self, block, off, size):
        self.f.seek(block*self.block_size + off)
        return self.f.read(size)

    def readblock(self, block):
        self.f.seek(block*self.block_size)
        return self.f.read(self.block_size)

# tagged data in an rbyd
class Rattr:
    def __init__(self, tag, weight, blocks, toff, tdata, data):
        self.tag = tag
        self.weight = weight
        if isinstance(blocks, int):
            self.blocks = (blocks,)
        else:
            self.blocks = blocks
        self.toff = toff
        self.tdata = tdata
        self.data = data

    @property
    def block(self):
        return self.blocks[0]

    @property
    def tsize(self):
        return len(self.tdata)

    @property
    def off(self):
        return self.toff + len(self.tdata)

    @property
    def size(self):
        return len(self.data)

    def __bytes__(self):
        return self.data

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return tagrepr(self.tag, self.weight, self.size)

    def __iter__(self):
        return iter((self.tag, self.weight, self.data))

    def __eq__(self, other):
        return ((self.tag, self.weight, self.data)
                == (other.tag, other.weight, other.data))

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((self.tag, self.weight, self.data))

    # convenience for did/name access
    def _parse_name(self):
        # note we return a null name for non-name tags, this is so
        # vestigial names in btree nodes act as a catch-all
        if (self.tag & 0xff00) != TAG_NAME:
            did = 0
            name = b''
        else:
            did, d = fromleb128(self.data)
            name = self.data[d:]

        # cache both
        self.did = did
        self.name = name

    @ft.cached_property
    def did(self):
        self._parse_name()
        return self.did

    @ft.cached_property
    def name(self):
        self._parse_name()
        return self.name

class Ralt:
    def __init__(self, tag, weight, blocks, toff, tdata, jump,
            color=None, followed=None):
        self.tag = tag
        self.weight = weight
        if isinstance(blocks, int):
            self.blocks = (blocks,)
        else:
            self.blocks = blocks
        self.toff = toff
        self.tdata = tdata
        self.jump = jump

        if color is not None:
            self.color = color
        else:
            self.color = 'r' if tag & TAG_R else 'b'
        self.followed = followed

    @property
    def block(self):
        return self.blocks[0]

    @property
    def tsize(self):
        return len(self.tdata)

    @property
    def off(self):
        return self.toff + len(self.tdata)

    @property
    def joff(self):
        return self.toff - self.jump

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return tagrepr(self.tag, self.weight, self.jump, toff=self.toff)

    def __iter__(self):
        return iter((self.tag, self.weight, self.jump))

    def __eq__(self, other):
        return ((self.tag, self.weight, self.jump)
                == (other.tag, other.weight, other.jump))

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((self.tag, self.weight, self.jump))


# our core rbyd type
class Rbyd:
    def __init__(self, blocks, trunk, weight, rev, eoff, cksum, data, *,
            shrub=False,
            gcksumdelta=None,
            redund=0):
        if isinstance(blocks, int):
            self.blocks = (blocks,)
        else:
            self.blocks = blocks
        self.trunk = trunk
        self.weight = weight
        self.rev = rev
        self.eoff = eoff
        self.cksum = cksum
        self.data = data

        self.shrub = shrub
        self.gcksumdelta = gcksumdelta
        self.redund = redund

    @property
    def block(self):
        return self.blocks[0]

    @property
    def corrupt(self):
        # use redund=-1 to indicate corrupt rbyds
        return self.redund >= 0

    def addr(self):
        if len(self.blocks) == 1:
            return '0x%x.%x' % (self.block, self.trunk)
        else:
            return '0x{%s}.%x' % (
                    ','.join('%x' % block for block in self.blocks),
                    self.trunk)

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return 'rbyd %s w%s' % (self.addr(), self.weight)

    def __bool__(self):
        # use redund=-1 to indicate corrupt rbyds
        return self.redund >= 0

    def __eq__(self, other):
        return ((frozenset(self.blocks), self.trunk)
                == (frozenset(other.blocks), other.trunk))

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((frozenset(self.blocks), self.trunk))

    @classmethod
    def _fetch(cls, data, block, trunk=None):
        # fetch the rbyd
        rev = fromle32(data[0:4])
        cksum = 0
        cksum_ = crc32c(data[0:4])
        cksum__ = cksum_
        perturb = False
        eoff = 0
        eoff_ = None
        j_ = 4
        trunk_ = 0
        trunk__ = 0
        trunk___ = 0
        weight = 0
        weight_ = 0
        weight__ = 0
        gcksumdelta = None
        gcksumdelta_ = None
        while j_ < len(data) and (not trunk or eoff <= trunk):
            # read next tag
            v, tag, w, size, d = fromtag(data[j_:])
            if v != parity(cksum__):
                break
            cksum__ ^= 0x00000080 if v else 0
            cksum__ = crc32c(data[j_:j_+d], cksum__)
            j_ += d
            if not tag & TAG_ALT and j_ + size > len(data):
                break

            # take care of cksums
            if not tag & TAG_ALT:
                if (tag & 0xff00) != TAG_CKSUM:
                    cksum__ = crc32c(data[j_:j_+size], cksum__)

                    # found a gcksumdelta?
                    if (tag & 0xff00) == TAG_GCKSUMDELTA:
                        gcksumdelta_ = Rattr(tag, w, block, j_-d,
                                data[j_-d:j_],
                                data[j_:j_+size])

                # found a cksum?
                else:
                    # check cksum
                    cksum___ = fromle32(data[j_:j_+4])
                    if cksum__ != cksum___:
                        break
                    # commit what we have
                    eoff = eoff_ if eoff_ else j_ + size
                    cksum = cksum_
                    trunk_ = trunk__
                    weight = weight_
                    gcksumdelta = gcksumdelta_
                    gcksumdelta_ = None
                    # update perturb bit
                    perturb = tag & TAG_P
                    # revert to data cksum and perturb
                    cksum__ = cksum_ ^ (0xfca42daf if perturb else 0)

            # evaluate trunks
            if (tag & 0xf000) != TAG_CKSUM:
                if not (trunk and j_-d > trunk and not trunk___):
                    # new trunk?
                    if not trunk___:
                        trunk___ = j_-d
                        weight__ = 0

                    # keep track of weight
                    weight__ += w

                    # end of trunk?
                    if not tag & TAG_ALT:
                        # update trunk/weight unless we found a shrub or an
                        # explicit trunk (which may be a shrub) is requested
                        if not tag & TAG_SHRUB or trunk___ == trunk:
                            trunk__ = trunk___
                            weight_ = weight__
                            # keep track of eoff for best matching trunk
                            if trunk and j_ + size > trunk:
                                eoff_ = j_ + size
                                eoff = eoff_
                                cksum = cksum__ ^ (
                                        0xfca42daf if perturb else 0)
                                trunk_ = trunk__
                                weight = weight_
                                gcksumdelta = gcksumdelta_
                        trunk___ = 0

                # update canonical checksum, xoring out any perturb state
                cksum_ = cksum__ ^ (0xfca42daf if perturb else 0)

            if not tag & TAG_ALT:
                j_ += size

        return cls(block, trunk_, weight, rev, eoff, cksum, data,
                gcksumdelta=gcksumdelta,
                redund=0 if trunk_ else -1)

    @classmethod
    def fetch(cls, bd, blocks, trunk=None):
        # multiple blocks?
        if not isinstance(blocks, int):
            # fetch all blocks
            rbyds = [cls.fetch(bd, block, trunk) for block in blocks]

            # determine most recent revision/trunk
            rev, trunk = None, None
            for rbyd in rbyds:
                # compare with sequence arithmetic
                if rbyd and (
                        rev is None
                            or not ((rbyd.rev - rev) & 0x80000000)
                            or (rbyd.rev == rev and rbyd.trunk > trunk)):
                    rev, trunk = rbyd.rev, rbyd.trunk
            # sort for reproducibility
            rbyds.sort(key=lambda rbyd: (
                    # prioritize valid redund blocks
                    0 if rbyd and rbyd.rev == rev and rbyd.trunk == trunk
                        else 1,
                    # default to sorting by block
                    rbyd.block))

            # choose an active rbyd
            rbyd = rbyds[0]
            # keep track of the other blocks
            rbyd.blocks = tuple(rbyd.block for rbyd in rbyds)
            # keep track of how many redund blocks are valid
            rbyd.redund = -1 + sum(1 for rbyd in rbyds
                    if rbyd and rbyd.rev == rev and rbyd.trunk == trunk)
            # and patch the gcksumdelta if we have one
            if rbyd.gcksumdelta is not None:
                rbyd.gcksumdelta.blocks = rbyd.blocks
            return rbyd

        # seek/read the block
        block = blocks
        data = bd.readblock(block)

        # fetch the rbyd
        return cls._fetch(data, block, trunk)

    @classmethod
    def fetchck(cls, bd, blocks, trunk, weight, cksum):
        # try to fetch the rbyd normally
        rbyd = cls.fetch(bd, blocks, trunk)

        # cksum mismatch? trunk/weight mismatch?
        if (rbyd.cksum != cksum
                or rbyd.trunk != trunk
                or rbyd.weight != weight):
            # mark as corrupt and keep track of expected trunk/weight
            rbyd.redund = -1
            rbyd.trunk = trunk
            rbyd.weight = weight

        return rbyd

    @classmethod
    def fetchshrub(cls, rbyd, trunk):
        # steal the original rbyd's data
        #
        # this helps avoid race conditions with cksums and stuff
        shrub = cls._fetch(rbyd.data, rbyd.block, trunk)
        shrub.blocks = rbyd.blocks
        shrub.shrub = True
        return shrub

    def lookupnext(self, rid, tag=None, *,
            path=False):
        if not self or rid >= self.weight:
            if path:
                return None, None, []
            else:
                return None, None

        tag = max(tag or 0, 0x1)
        lower = 0
        upper = self.weight
        path_ = []

        # descend down tree
        j = self.trunk
        while True:
            _, alt, w, jump, d = fromtag(self.data[j:])

            # found an alt?
            if alt & TAG_ALT:
                # follow?
                if ((rid, tag & 0xfff) > (upper-w-1, alt & 0xfff)
                        if alt & TAG_GT
                        else ((rid, tag & 0xfff)
                            <= (lower+w-1, alt & 0xfff))):
                    lower += upper-lower-w if alt & TAG_GT else 0
                    upper -= upper-lower-w if not alt & TAG_GT else 0
                    j = j - jump

                    if path:
                        # figure out which color
                        if alt & TAG_R:
                            _, nalt, _, _, _ = fromtag(self.data[j+jump+d:])
                            if nalt & TAG_R:
                                color = 'y'
                            else:
                                color = 'r'
                        else:
                            color = 'b'

                        path_.append(Ralt(
                                alt, w, self.blocks, j+jump,
                                self.data[j+jump:j+jump+d], jump,
                                color=color,
                                followed=True))

                # stay on path
                else:
                    lower += w if not alt & TAG_GT else 0
                    upper -= w if alt & TAG_GT else 0
                    j = j + d

                    if path:
                        # figure out which color
                        if alt & TAG_R:
                            _, nalt, _, _, _ = fromtag(self.data[j:])
                            if nalt & TAG_R:
                                color = 'y'
                            else:
                                color = 'r'
                        else:
                            color = 'b'

                        path_.append(Ralt(
                                alt, w, self.blocks, j-d,
                                self.data[j-d:j], jump,
                                color=color,
                                followed=False))

            # found tag
            else:
                rid_ = upper-1
                tag_ = alt
                w_ = upper-lower

                if not tag_ or (rid_, tag_) < (rid, tag):
                    if path:
                        return None, None, path_
                    else:
                        return None, None

                rattr_ = Rattr(tag_, w_, self.blocks, j,
                        self.data[j:j+d],
                        self.data[j+d:j+d+jump])
                if path:
                    return rid_, rattr_, path_
                else:
                    return rid_, rattr_

    def lookup(self, rid, tag=None, mask=None, *,
            path=False):
        if tag is None:
            tag, mask = 0, 0xffff
        if mask is None:
            mask = 0

        r = self.lookupnext(rid, tag & ~mask,
                path=path)
        if path:
            rid_, rattr_, path_ = r
        else:
            rid_, rattr_ = r
        if (rid_ is None
                or rid_ != rid
                or (rattr_.tag & ~mask) != (tag & ~mask)):
            if path:
                return None, path_
            else:
                return None

        if path:
            return rattr_, path_
        else:
            return rattr_

    def rids(self, *,
            path=False):
        rid = -1
        while True:
            r = self.lookupnext(rid,
                    path=path)
            if path:
                rid, name, path_ = r
            else:
                rid, name = r
            # found end of tree?
            if rid is None:
                break

            if path:
                yield rid, name, path_
            else:
                yield rid, name
            rid += 1

    def rattrs(self, rid=None, tag=None, mask=None, *,
            path=False):
        if rid is None:
            rid, tag = -1, 0
            while True:
                r = self.lookupnext(rid, tag+0x1,
                        path=path)
                if path:
                    rid, rattr, path_ = r
                else:
                    rid, rattr = r
                # found end of tree?
                if rid is None:
                    break

                if path:
                    yield rid, rattr, path_
                else:
                    yield rid, rattr
                tag = rattr.tag
        else:
            if tag is None:
                tag, mask = 0, 0xffff
            if mask is None:
                mask = 0

            tag_ = max((tag & ~mask) - 1, 0)
            while True:
                r = self.lookupnext(rid, tag_+0x1,
                        path=path)
                if path:
                    rid_, rattr_, path_ = r
                else:
                    rid_, rattr_ = r
                # found end of tree?
                if (rid_ is None
                        or rid_ != rid
                        or (rattr_.tag & ~mask) != (tag & ~mask)):
                    break

                if path:
                    yield rattr_, path_
                else:
                    yield rattr_
                tag_ = rattr_.tag

    # lookup by name
    def namelookup(self, did, name):
        # binary search
        best = None, None
        lower = 0
        upper = self.weight
        while lower < upper:
            rid, name_ = self.lookupnext(
                lower + (upper-1-lower)//2)
            if rid is None:
                break

            # bisect search space
            if (name_.did, name_.name) > (did, name):
                upper = rid-(name_.weight-1)
            elif (name_.did, name_.name) < (did, name):
                lower = rid + 1
                # keep track of best match
                best = rid, name_
            else:
                # found a match
                return rid, name_

        return best


# our rbyd btree type
class Btree:
    def __init__(self, bd, rbyd):
        self.bd = bd
        self.rbyd = rbyd

    @property
    def block(self):
        return self.rbyd.block

    @property
    def blocks(self):
        return self.rbyd.blocks

    @property
    def trunk(self):
        return self.rbyd.trunk

    @property
    def weight(self):
        return self.rbyd.weight

    @property
    def rev(self):
        return self.rbyd.rev

    @property
    def cksum(self):
        return self.rbyd.cksum

    @property
    def shrub(self):
        return self.rbyd.shrub

    def addr(self):
        return self.rbyd.addr()

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return 'btree %s w%s' % (self.addr(), self.weight)

    def __eq__(self, other):
        return self.rbyd == other.rbyd

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(self.rbyd)

    @classmethod
    def fetch(cls, bd, blocks, trunk=None):
        # rbyd fetch does most of the work here
        rbyd = Rbyd.fetch(bd, blocks, trunk)
        return cls(bd, rbyd)

    @classmethod
    def fetchck(cls, bd, blocks, trunk, weight, cksum):
        # rbyd fetchck does most of the work here
        rbyd = Rbyd.fetchck(bd, blocks, trunk, weight, cksum)
        return cls(bd, rbyd)

    @classmethod
    def fetchshrub(cls, bd, rbyd, trunk):
        shrub = Rbyd.fetchshrub(rbyd, trunk)
        return cls(bd, shrub)

    def lookupleaf(self, bid, *,
            path=False,
            depth=None):
        if not self or bid >= self.weight:
            if path:
                return None, None, None, None, []
            else:
                return None, None, None, None

        rbyd = self.rbyd
        rid = bid
        depth_ = 1
        path_ = []

        while True:
            # corrupt branch?
            if not rbyd:
                if path:
                    return bid, rbyd, rid, None, path_
                else:
                    return bid, rbyd, rid, None

            # first tag indicates the branch's weight
            rid_, name_ = rbyd.lookupnext(rid)
            if rid_ is None:
                if path:
                    return None, None, None, None, path_
                else:
                    return None, None, None, None

            # keep track of path
            if path:
                path_.append((bid + (rid_-rid), rbyd, rid_, name_))

            # find branch tag if there is one
            branch_ = rbyd.lookup(rid_, TAG_BRANCH, 0x3)

            # descend down branch?
            if branch_ is not None and (
                    not depth or depth_ < depth):
                block, trunk, cksum = frombranch(branch_.data)
                rbyd = Rbyd.fetchck(self.bd, block, trunk, name_.weight,
                        cksum)

                rid -= (rid_-(name_.weight-1))
                depth_ += 1

            else:
                if path:
                    return bid + (rid_-rid), rbyd, rid_, name_, path_
                else:
                    return bid + (rid_-rid), rbyd, rid_, name_

    # the non-leaf variants discard the rbyd info, these can be a bit
    # more convenient, but at a performance cost
    def lookupnext(self, bid, *,
            path=False,
            depth=None):
        # just discard the rbyd info
        r = self.lookupleaf(bid,
                path=path,
                depth=depth)
        if path:
            bid, rbyd, rid, name, path_ = r
        else:
            bid, rbyd, rid, name = r

        if path:
            return bid, name, path_
        else:
            return bid, name

    def lookup(self, bid, tag=None, mask=None, *,
            path=False,
            depth=None):
        # lookup rbyd in btree
        #
        # note this function expects bid to be known, use lookupnext
        # first if you don't care about the exact bid (or better yet,
        # lookupleaf and call lookup on the returned rbyd)
        #
        # this matches rbyd's lookup behavior, which needs a known rid
        # to avoid a double lookup
        r = self.lookupleaf(bid,
                path=path,
                depth=depth)
        if path:
            bid_, rbyd_, rid_, name_, path_ = r
        else:
            bid_, rbyd_, rid_, name_ = r
        if bid_ is None or bid_ != bid:
            if path:
                return None, path_
            else:
                return None

        # lookup tag in rbyd
        rattr_ = rbyd_.lookup(rid_, tag, mask)
        if rattr_ is None:
            if path:
                return None, path_
            else:
                return None

        if path:
            return rattr_, path_
        else:
            return rattr_

    # note leaves only iterates over leaf rbyds, whereas traverse
    # traverses all rbyds
    def leaves(self, *,
            path=False,
            depth=None):
        # include our root rbyd even if the weight is zero
        if self.weight == 0:
            if path:
                yield -1, self.rbyd, []
            else:
                yield -1, self.rbyd
            return

        bid = 0
        while True:
            r = self.lookupleaf(bid,
                    path=path,
                    depth=depth)
            if r:
                bid, rbyd, rid, name, path_ = r
            else:
                bid, rbyd, rid, name = r 
            if bid is None:
                break

            if path:
                yield (bid-rid + (rbyd.weight-1), rbyd,
                        # path tail is usually redundant unless corrupt
                        path_[:-1] if rbyd else path_)
            else:
                yield bid-rid + (rbyd.weight-1), rbyd
            bid += rbyd.weight - rid + 1

    def traverse(self, *,
            path=False,
            depth=None):
        ptrunk_ = []
        for bid, rbyd, path_ in self.leaves(
                path=True,
                depth=depth):
            # we only care about the rbyds here
            trunk_ = ([(bid_-rid_ + (rbyd_.weight-1), rbyd_)
                        for bid_, rbyd_, rid_, name_ in path_]
                    + [(bid, rbyd)])
            for d, (bid_, rbyd_) in pathdelta(
                    trunk_, ptrunk_):
                # but include branch rids in the path if requested
                if path:
                    yield bid_, rbyd_, path_[:d]
                else:
                    yield bid_, rbyd_
            ptrunk_ = trunk_

    # note bids/rattrs do _not_ include corrupt btree nodes!
    def bids(self, *,
            leaves=False,
            path=False,
            depth=None):
        for r in self.leaves(
                path=path,
                depth=depth):
            if path:
                bid, rbyd, path_ = r
            else:
                bid, rbyd = r
            for rid, name in rbyd.rids():
                bid_ = bid-(rbyd.weight-1) + rid
                if leaves:
                    if path:
                        yield (bid_, rbyd, rid, name,
                                path_+[(bid_, rbyd, rid, name)])
                    else:
                        yield bid_, rbyd, rid, name
                else:
                    if path:
                        yield (bid_, name,
                                path_+[(bid_, rbyd, rid, name)])
                    else:
                        yield bid_, name

    def rattrs(self, bid=None, tag=None, mask=None, *,
            leaves=False,
            path=False,
            depth=None):
        if bid is None:
            for r in self.leaves(
                    path=path,
                    depth=depth):
                if path:
                    bid, rbyd, path_ = r
                else:
                    bid, rbyd = r
                for rid, name in rbyd.rids():
                    bid_ = bid-(rbyd.weight-1) + rid
                    for rattr in rbyd.rattrs(rid):
                        if leaves:
                            if path:
                                yield (bid_, rbyd, rid, rattr,
                                        path_+[(bid_, rbyd, rid, name)])
                            else:
                                yield bid_, rbyd, rid, rattr
                        else:
                            if path:
                                yield (bid_, rattr,
                                        path_+[(bid_, rbyd, rid, name)])
                            else:
                                yield bid_, rattr
        else:
            r = self.lookupleaf(bid,
                    path=path,
                    depth=depth)
            if path:
                bid, rbyd, rid, name, path_ = r
            else:
                bid, rbyd, rid, name = r
            if bid is None:
                return

            for rattr in rbyd.rattrs(rid, tag, mask):
                if leaves:
                    if path:
                        yield rbyd, rid, rattr, path_
                    else:
                        yield rbyd, rid, rattr
                else:
                    if path:
                        yield rattr, path_
                    else:
                        yield rattr

    # lookup by name
    def namelookupleaf(self, did, name, *,
            path=False,
            depth=None):
        rbyd = self.rbyd
        bid = 0
        depth_ = 1
        path_ = []

        while True:
            # corrupt branch?
            if not rbyd:
                bid_ = bid+(rbyd.weight-1)
                if path:
                    return bid_, rbyd, rbyd.weight-1, None, path_
                else:
                    return bid_, rbyd, rbyd.weight-1, None

            rid_, name_ = rbyd.namelookup(did, name)

            # keep track of path
            if path:
                path_.append((bid + rid_, rbyd, rid_, name_))

            # find branch tag if there is one
            branch_ = rbyd.lookup(rid_, TAG_BRANCH, 0x3)

            # found another branch
            if branch_ is not None and (
                    not depth or depth_ < depth):
                block, trunk, cksum = frombranch(branch_.data)
                rbyd = Rbyd.fetchck(self.bd, block, trunk, name_.weight,
                        cksum)

                # update our bid
                bid += rid_ - (name_.weight-1)
                depth_ += 1

            # found best match
            else:
                if path:
                    return bid + rid_, rbyd, rid_, name_, path_
                else:
                    return bid + rid_, rbyd, rid_, name_

    def namelookup(self, bid, *,
            path=False,
            depth=None):
        # just discard the rbyd info
        r = self.namelookupleaf(did, name,
                path=path,
                depth=depth)
        if path:
            bid, rbyd, rid, name, path_ = r
        else:
            bid, rbyd, rid, name = r

        if path:
            return bid, name, path_
        else:
            return bid, name


# a metadata id, this includes mbits for convenience
class Mid:
    def __init__(self, mbid, mrid=None, *,
            mbits=None):
        # we need one of these to figure out mbits
        if mbits is not None:
            self.mbits = mbits
        elif isinstance(mbid, Mid):
            self.mbits = mbid.mbits
        else:
            assert mbits is not None, "mbits?"

        # accept other mids which can be useful for changing mrids
        if isinstance(mbid, Mid):
            mbid = mbid.mbid

        # accept either merged mid or separate mbid+mrid
        if mrid is None:
            mid = mbid
            mbid = mid | ((1 << self.mbits) - 1)
            mrid = mid & ((1 << self.mbits) - 1)

        # map mrid=-1
        if mrid == ((1 << self.mbits) - 1):
            mrid = -1

        self.mbid = mbid
        self.mrid = mrid

    @property
    def mid(self):
        return ((self.mbid & ~((1 << self.mbits) - 1))
                | (self.mrid & ((1 << self.mbits) - 1)))

    def mbidrepr(self):
        return str(self.mbid >> self.mbits)

    def mridrepr(self):
        return str(self.mrid)

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return '%s.%s' % (self.mbidrepr(), self.mridrepr())

    def __iter__(self):
        return iter((self.mbid, self.mrid))

    # note this is slightly different from mid order when mrid=-1
    def __eq__(self, other):
        if isinstance(other, Mid):
            return (self.mbid, self.mrid) == (other.mbid, other.mrid)
        else:
            return self.mid == other

    def __ne__(self, other):
        if isinstance(other, Mid):
            return (self.mbid, self.mrid) != (other.mbid, other.mrid)
        else:
            return self.mid != other

    def __hash__(self):
        return hash((self.mbid, self.mrid))

    def __lt__(self, other):
        return (self.mbid, self.mrid) < (other.mbid, other.mrid)

    def __le__(self, other):
        return (self.mbid, self.mrid) <= (other.mbid, other.mrid)

    def __gt__(self, other):
        return (self.mbid, self.mrid) > (other.mbid, other.mrid)

    def __ge__(self, other):
        return (self.mbid, self.mrid) >= (other.mbid, other.mrid)

# mdirs, the gooey atomic center of littlefs
#
# really the only difference between this and our rbyd class is the
# implicit mbid associated with the mdir
class Mdir:
    def __init__(self, mid, rbyd, *,
            mbits=None):
        # we need one of these to figure out mbits
        if mbits is not None:
            self.mbits = mbits
        elif isinstance(mid, Mid):
            self.mbits = mid.mbits
        elif isinstance(rbyd, Mdir):
            self.mbits = rbyd.mbits
        else:
            assert mbits is not None, "mbits?"

        # strip mrid, bugs will happen if caller relies on mrid here
        self.mid = Mid(mid, -1, mbits=self.mbits)

        # accept either another mdir or rbyd
        if isinstance(rbyd, Mdir):
            self.rbyd = rbyd.rbyd
        else:
            self.rbyd = rbyd

    @property
    def data(self):
        return self.rbyd.data

    @property
    def block(self):
        return self.rbyd.block

    @property
    def blocks(self):
        return self.rbyd.blocks

    @property
    def trunk(self):
        return self.rbyd.trunk

    @property
    def weight(self):
        return self.rbyd.weight

    @property
    def rev(self):
        return self.rbyd.rev

    @property
    def eoff(self):
        return self.rbyd.eoff

    @property
    def cksum(self):
        return self.rbyd.cksum

    @property
    def gcksumdelta(self):
        return self.rbyd.gcksumdelta

    @property
    def corrupt(self):
        return self.rbyd.corrupt

    @property
    def redund(self):
        return self.rbyd.redund

    def addr(self):
        if len(self.blocks) == 1:
            return '0x%x' % self.block
        else:
            return '0x{%s}' % (
                    ','.join('%x' % block for block in self.blocks))

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return 'mdir %s %s w%s' % (
                self.mid.mbidrepr(),
                self.addr(),
                self.weight)

    def __bool__(self):
        return bool(self.rbyd)

    # we _don't_ care about mid for equality, or trunk even
    def __eq__(self, other):
        return frozenset(self.blocks) == frozenset(other.blocks)

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(frozenset(self.blocks))

    @classmethod
    def fetch(cls, bd, mid, blocks, trunk=None):
        rbyd = Rbyd.fetch(bd, blocks, trunk)
        return cls(mid, rbyd, mbits=Mtree.mbits_(bd))

    def lookup(self, mid, tag=None, mask=None, *,
            path=False):
        if not isinstance(mid, Mid):
            mid = Mid(mid, mbits=self.mbits)
        return self.rbyd.lookup(mid.mrid, tag, mask,
                path=path)

    def mids(self, *,
            path=False):
        for r in self.rbyd.rids(
                path=path):
            if path:
                rid, name, path_ = r
            else:
                rid, name = r

            mid = Mid(self.mid, rid)
            if path:
                yield mid, name, path_
            else:
                yield mid, name

    def rattrs(self, mid=None, tag=None, mask=None, *,
            path=False):
        if mid is None:
            for r in self.rbyd.rattrs(
                    path=path):
                if path:
                    rid, rattr, path_ = r
                else:
                    rid, rattr = r

                mid = Mid(self.mid, rid)
                if path:
                    yield mid, rattr, path_
                else:
                    yield mid, rattr
        else:
            if not isinstance(mid, Mid):
                mid = Mid(mid, mbits=self.mbits)
            yield from self.rbyd.rattrs(mid.mrid, tag, mask,
                    path=path)

    # lookup by name
    def namelookup(self, did, name):
        # unlike rbyd namelookup, we need an exact match here
        rid, name_ = self.rbyd.namelookup(did, name)
        if rid is None or (name_.did, name_.name) != (did, name):
            return None, None

        return Mid(self.mid, rid), name_

# the mtree, the skeletal structure of littlefs
class Mtree:
    def __init__(self, bd, mrootchain, mtree, *,
            mrootpath=False,
            mtreepath=False,
            mbits=None):
        if isinstance(mrootchain, Mdir):
            mrootchain = [Mdir]
        # we at least need the mrootanchor, even if it is corrupt
        assert len(mrootchain) >= 1

        self.bd = bd
        if mbits is not None:
            self.mbits = mbits
        else:
            self.mbits = Mtree.mbits_(self.bd)

        self.mrootchain = mrootchain
        self.mrootanchor = mrootchain[0]
        self.mroot = mrootchain[-1]
        self.mtree = mtree

    # mbits is a static value derived from the block_size
    @staticmethod
    def mbits_(block_size):
        if isinstance(block_size, Bd):
            block_size = block_size.block_size
        return mt.ceil(mt.log2(block_size // 8))

    # convenience function for creating mbits-dependent mids
    def mid(self, mbid, mrid=None):
        return Mid(mbid, mrid, mbits=self.mbits)

    @property
    def block(self):
        return self.mroot.block

    @property
    def blocks(self):
        return self.mroot.blocks

    @property
    def trunk(self):
        return self.mroot.trunk

    @property
    def weight(self):
        if self.mtree is None:
            return 0
        else:
            return self.mtree.weight

    @property
    def mbweight(self):
        return self.weight

    @property
    def mrweight(self):
        return 1 << self.mbits

    def mbweightrepr(self):
        return str(self.mbweight >> self.mbits)

    def mrweightrepr(self):
        return str(self.mrweight)

    @property
    def rev(self):
        return self.mroot.rev

    @property
    def cksum(self):
        return self.mroot.cksum

    def addr(self):
        return self.mroot.addr()

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return 'mtree %s w%s.%s' % (
                self.addr(),
                self.mbweightrepr(), self.mrweightrepr())

    def __eq__(self, other):
        return self.mrootanchor == other.mrootanchor

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(self.mrootanchor)

    @classmethod
    def fetch(cls, bd, blocks=None, trunk=None, *,
            depth=None):
        # default to blocks 0x{0,1}
        if blocks is None:
            blocks = [0, 1]

        # figure out mbits
        mbits = Mtree.mbits_(bd)

        # fetch the mrootanchor
        mrootanchor = Mdir.fetch(bd, -1, blocks, trunk)

        # follow the mroot chain to try to find the active mroot
        mroot = mrootanchor
        mrootchain = [mrootanchor]
        mrootseen = set()
        while True:
            # corrupted?
            if not mroot:
                break
            # cycle detected?
            if mroot in mrootseen:
                break
            mrootseen.add(mroot)

            # stop here?
            if depth and len(mrootchain) >= depth:
                break

            # fetch the next mroot
            rattr_ = mroot.lookup(-1, TAG_MROOT, 0x3)
            if rattr_ is None:
                break
            blocks_ = frommdir(rattr_.data)
            mroot = Mdir.fetch(bd, -1, blocks_)
            mrootchain.append(mroot)

        # fetch the actual mtree, if there is one
        mtree = None
        if not depth or len(mrootchain) < depth:
            rattr_ = mroot.lookup(-1, TAG_MTREE, 0x3)
            if rattr_ is not None:
                w_, block_, trunk_, cksum_ = frombtree(rattr_.data)
                mtree = Btree.fetchck(bd, block_, trunk_, w_, cksum_)

        return cls(bd, mrootchain, mtree,
                mbits=mbits)

    def _lookupleaf(self, mid, *,
            path=False,
            depth=None):
        if not isinstance(mid, Mid):
            mid = self.mid(mid)

        if path or depth:
            # iterate over mrootchain
            path_ = []
            for mroot in self.mrootchain:
                name = mroot.lookup(-1, TAG_MAGIC)
                path_.append((mroot.mid, mroot, name))
                # stop here?
                if depth and len(path_) >= depth:
                    if path:
                        return mroot, path_
                    else:
                        return mroot

        # no mtree? must be inlined in mroot
        if self.mtree is None:
            if mid.mbid >= (1 << self.mbits):
                if path:
                    return None, path_
                else:
                    return None

            mdir = Mdir(0, self.mroot)
            if path:
                return mdir, path_
            else:
                return mdir

        # mtree? lookup in mtree
        else:
            # need to do two steps here in case lookupleaf stops early
            r = self.mtree.lookupleaf(mid.mid,
                    path=path or depth,
                    depth=depth-len(path_) if depth else None)
            if path or depth:
                bid_, rbyd_, rid_, name_, path__ = r
                path_.extend(path__)
            else:
                bid_, rbyd_, rid_, name_ = r
            if bid_ is None:
                if path:
                    return None, path_
                else:
                    return None

            # corrupt btree node?
            if not rbyd_:
                if path:
                    return (bid_, rbyd_, rid_), path_
                else:
                    return (bid_, rbyd_, rid_)

            # stop here? it's not an mdir, but we only return btree nodes
            # if explicitly requested
            if depth and len(path_) >= depth:
                if path:
                    return (bid_, rbyd_, rid_), path_
                else:
                    return (bid_, rbyd_, rid_)

            # fetch the mdir
            rattr_ = rbyd_.lookup(rid_, TAG_MDIR, 0x3)
            # mdir tag missing? weird
            if rattr_ is None:
                if path:
                    return (bid_, rbyd_, rid_), path_
                else:
                    return (bid_, rbyd_, rid_)
            blocks_ = frommdir(rattr_.data)
            mdir = Mdir.fetch(self.bd, mid, blocks_)
            if path:
                return mdir, path_
            else:
                return mdir

    def lookupleaf(self, mid, *,
            mdirs_only=True,
            path=False,
            depth=None):
        # most of the logic is in _lookupleaf, this just helps
        # deduplicate the mdirs_only logic
        r = self._lookupleaf(mid,
                path=path,
                depth=depth)
        if path:
            mdir, path_ = r
        else:
            mdir = r
        if mdir is None or (
                mdirs_only and not isinstance(mdir, Mdir)):
            if path:
                return None, path_
            else:
                return None

        if path:
            return mdir, path_
        else:
            return mdir

    def lookup(self, mid, *,
            path=False,
            depth=None):
        if not isinstance(mid, Mid):
            mid = self.mid(mid)

        # lookup the relevant mdir
        r = self.lookupleaf(mid,
                path=path,
                depth=depth)
        if path:
            mdir, path_ = r
        else:
            mdir = r
        if mdir is None:
            if path:
                return None, None, path_
            else:
                return None, None

        # not in mdir?
        if mid.mrid >= mdir.weight:
            if path:
                return None, None, path_
            else:
                return None, None

        # lookup name in mdir
        name = mdir.lookup(mid)
        # name tag missing? weird
        if name is None:
            if path:
                return None, None, path_
            else:
                return None, None
        if path:
            return mdir, name, path_+[(mid, mdir, name)]
        else:
            return mdir, name

    # iterate over all mdirs, this includes the mrootchain
    def _leaves(self, *,
            path=False,
            depth=None):
        # iterate over mrootchain
        if path or depth:
            path_ = []
        for mroot in self.mrootchain:
            if path:
                yield mroot, path_
            else:
                yield mroot

            if path or depth:
                name = mroot.lookup(-1, TAG_MAGIC)
                path_.append((mroot.mid, mroot, name))
                # stop here?
                if depth and len(path_) >= depth:
                    return

        # do we even have an mtree?
        if self.mtree is not None:
            # include the mtree root even if the weight is zero
            if self.mtree.weight == 0:
                if path:
                    yield -1, self.mtree.rbyd, path_
                else:
                    yield -1, self.mtree.rbyd
                return

            mid = self.mid(0)
            while True:
                r = self.lookupleaf(mid,
                        mdirs_only=False,
                        path=path,
                        depth=depth)
                if path:
                    mdir, path_ = r
                else:
                    mdir = r
                if mdir is None:
                    break

                # mdir?
                if isinstance(mdir, Mdir):
                    if path:
                        yield mdir, path_
                    else:
                        yield mdir
                    mid = self.mid(mid.mbid+1)
                # btree node?
                else:
                    bid, rbyd, rid = mdir
                    if path:
                        yield ((bid-rid + (rbyd.weight-1), rbyd),
                                # path tail is usually redundant unless corrupt
                                path_[:-1] if rbyd else path_)
                    else:
                        yield (bid-rid + (rbyd.weight-1), rbyd)
                    mid = self.mid(bid-rid + (rbyd.weight-1) + 1)

    def leaves(self, *,
            mdirs_only=False,
            path=False,
            depth=None):
        for r in self._leaves(
                path=path,
                depth=depth):
            if path:
                mdir, path_ = r
            else:
                mdir = r
            if mdirs_only and not isinstance(mdir, Mdir):
                continue

            if path:
                yield mdir, path_
            else:
                yield mdir

    # traverse over all mdirs and btree nodes
    # - mdir       => Mdir
    # - btree node => (bid, rbyd)
    def _traverse(self, *,
            path=False,
            depth=None):
        ptrunk_ = []
        for mdir, path_ in self.leaves(
                path=True,
                depth=depth):
            # we only care about the mdirs/rbyds here
            trunk_ = ([(lambda mid_, mdir_, name_: mdir_)(*p)
                        if isinstance(p[1], Mdir)
                        else (lambda bid_, rbyd_, rid_, name_:
                            (bid_-rid_ + (rbyd_.weight-1), rbyd_))(*p)
                        for p in path_]
                    + [mdir])
            for d, mdir in pathdelta(
                    trunk_, ptrunk_):
                # but include branch mids/rids in the path if requested
                if path:
                    yield mdir, path_[:d]
                else:
                    yield mdir
            ptrunk_ = trunk_

    def traverse(self, *,
            mdirs_only=False,
            path=False,
            depth=None):
        for r in self._traverse(
                path=path,
                depth=depth):
            if path:
                mdir, path_ = r
            else:
                mdir = r
            if mdirs_only and not isinstance(mdir, Mdir):
                continue

            if path:
                yield mdir, path_
            else:
                yield mdir

    # these are just aliases

    # the difference between mdirs and leaves is mdirs defaults to only
    # mdirs, leaves can include btree nodes if corrupt
    def mdirs(self, *,
            mdirs_only=True,
            path=False,
            depth=None):
        return self.leaves(
                mdirs_only=mdirs_only,
                path=path,
                depth=depth)

    # note mids/rattrs do _not_ include corrupt btree nodes!
    def mids(self, *,
            mdirs_only=True,
            path=False,
            depth=None):
        for r in self.mdirs(
                mdirs_only=mdirs_only,
                path=path,
                depth=depth):
            if path:
                mdir, path_ = r
            else:
                mdir = r
            if isinstance(mdir, Mdir):
                for mid, name in mdir.mids():
                    if path:
                        yield (mid, mdir, name,
                                path_+[(mid, mdir, name)])
                    else:
                        yield mid, mdir, name
            else:
                bid, rbyd = mdir
                for rid, name in rbyd.rids():
                    bid_ = bid-(rbyd.weight-1) + rid
                    mid_ = self.mid(bid_)
                    mdir_ = (bid_, rbyd, rid)
                    if path:
                        yield (mid_, mdir_, name,
                                path_+[(bid_, rbyd, rid, name)])
                    else:
                        yield mid_, mdir_, name

    def rattrs(self, mid=None, tag=None, mask=None, *,
            mdirs_only=True,
            path=False,
            depth=None):
        if mid is None:
            for r in self.mdirs(
                    mdirs_only=mdirs_only,
                    path=path,
                    depth=depth):
                if path:
                    mdir, path_ = r
                else:
                    mdir = r
                if isinstance(mdir, Mdir):
                    for mid, rattr in mdir.rattrs():
                        if path:
                            yield (mid, mdir, rattr,
                                    path_+[(mid, mdir, mdir.lookup(mid))])
                        else:
                            yield mid, mdir, rattr
                else:
                    bid, rbyd = mdir
                    for rid, name in rbyd.rids():
                        bid_ = bid-(rbyd.weight-1) + rid
                        mid_ = self.mid(bid_)
                        mdir_ = (bid_, rbyd, rid)
                        for rattr in rbyd.rattrs(rid):
                            if path:
                                yield (mid_, mdir_, rattr,
                                        path_+[(bid_, rbyd, rid, name)])
                            else:
                                yield mid_, mdir_, rattr
        else:
            if not isinstance(mid, Mid):
                mid = self.mid(mid)

            r = self.lookupleaf(mid,
                    path=path,
                    depth=depth)
            if path:
                mdir, path_ = r
            else:
                mdir = r
            if mdir is None or (
                    mdirs_only and not isinstance(mdir, Mdir)):
                return

            if isinstance(mdir, Mdir):
                for rattr in mdir.rattrs(mid, tag, mask):
                    if path:
                        yield rattr, path_
                    else:
                        yield rattr
            else:
                bid, rbyd, rid = mdir
                for rattr in rbyd.rattrs(rid, tag, mask):
                    if path:
                        yield rattr, path_
                    else:
                        yield rattr

    # lookup by name
    def _namelookupleaf(self, did, name, *,
            path=False,
            depth=None):
        if path or depth:
            # iterate over mrootchain
            path_ = []
            for mroot in self.mrootchain:
                name = mroot.lookup(-1, TAG_MAGIC)
                path_.append((mroot.mid, mroot, name))
                # stop here?
                if depth and len(path_) >= depth:
                    if path:
                        return mroot, path_
                    else:
                        return mroot

        # no mtree? must be inlined in mroot
        if self.mtree is None:
            mdir = Mdir(0, self.mroot)
            if path:
                return mdir, path_
            else:
                return mdir

        # mtree? find name in mtree
        else:
            # need to do two steps here in case namelookupleaf stops early
            r = self.mtree.namelookupleaf(did, name,
                    path=path or depth,
                    depth=depth-len(path_) if depth else None)
            if path or depth:
                bid_, rbyd_, rid_, name_, path__ = r
                path_.extend(path__)
            else:
                bid_, rbyd_, rid_, name_ = r
            if bid_ is None:
                if path:
                    return None, path_
                else:
                    return None

            # corrupt btree node?
            if not rbyd_:
                if path:
                    return (bid_, rbyd_, rid_), path_
                else:
                    return (bid_, rbyd_, rid_)

            # stop here? it's not an mdir, but we only return btree nodes
            # if explicitly requested
            if depth and len(path_) >= depth:
                if path:
                    return (bid_, rbyd_, rid_), path_
                else:
                    return (bid_, rbyd_, rid_)

            # fetch the mdir
            rattr_ = rbyd_.lookup(rid_, TAG_MDIR, 0x3)
            # mdir tag missing? weird
            if rattr_ is None:
                if path:
                    return (bid_, rbyd_, rid_), path_
                else:
                    return (bid_, rbyd_, rid_)
            blocks_ = frommdir(rattr_.data)
            mdir = Mdir.fetch(self.bd, self.mid(bid_), blocks_)
            if path:
                return mdir, path_
            else:
                return mdir

    def namelookupleaf(self, did, name, *,
            mdirs_only=True,
            path=False,
            depth=None):
        # most of the logic is in _namelookupleaf, this just helps
        # deduplicate the mdirs_only logic
        r = self._namelookupleaf(did, name,
                path=path,
                depth=depth)
        if path:
            mdir, path_ = r
        else:
            mdir = r
        if mdir is None or (
                mdirs_only and not isinstance(mdir, Mdir)):
            if path:
                return None, path_
            else:
                return None

        if path:
            return mdir, path_
        else:
            return mdir

    def namelookup(self, did, name, *,
            path=False,
            depth=None):
        # lookup the relevant mdir
        r = self.namelookupleaf(did, name,
                path=path,
                depth=depth)
        if path:
            mdir, path_ = r
        else:
            mdir = r
        if mdir is None:
            if path:
                return None, None, None, path_
            else:
                return None, None, None

        # find name in mdir
        mid_, name_ = mdir.namelookup(did, name)
        if mid_ is None:
            if path:
                return None, None, None, path_
            else:
                return None, None, None

        if path:
            return mid_, mdir, name_, path_+[(mid_, mdir, name_)]
        else:
            return mid_, mdir, name_


# in-btree block pointers
class Bptr:
    def __init__(self, rattr, block, off, size, cksize, cksum, ckdata, *,
            corrupt=False):
        self.rattr = rattr
        self.block = block
        self.off = off
        self.size = size
        self.cksize = cksize
        self.cksum = cksum
        self.ckdata = ckdata

        self.corrupt = corrupt

    @property
    def tag(self):
        return self.rattr.tag

    @property
    def weight(self):
        return self.rattr.weight

    # this is just for consistency with btrees, rbyds, etc
    @property
    def blocks(self):
        return [self.block]

    # try to avoid unnecessary allocations
    @ft.cached_property
    def data(self):
        return self.ckdata[self.off:self.off+self.size]

    def addr(self):
        return '0x%x.%x' % (self.block, self.off)

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return '%sblock %s w%s %s' % (
                'shrub' if self.tag & TAG_SHRUB else '',
                self.addr(),
                self.weight,
                self.size)

    # lazily check the cksum
    @ft.cached_property
    def corrupt(self):
        cksum_ = crc32c(self.ckdata)
        return (cksum_ != self.cksum)

    @property
    def redund(self):
        return -1 if self.corrupt else 0

    def __bool__(self):
        return not self.corrupt

    @classmethod
    def fetch(cls, bd, rattr, block, off, size, cksize, cksum):
        # seek/read cksize bytes from the block, the actual data should
        # always be a subset of cksize
        ckdata = bd.read(block, 0, cksize)

        return cls(rattr, block, off, size, cksize, cksum, ckdata)

    @classmethod
    def fetchck(cls, bd, rattr, blocks, off, size, cksize, cksum):
        # fetch the bptr normally
        bptr = cls.fetch(bd, rattr, blocks, off, size, cksize, cksum)

        # bit of a hack, but this exposes the lazy cksum checker
        del bptr.corrupt

        return bptr

    # yeah, so, this doesn't catch mismatched cksizes, but at least the
    # underlying data should be identical assuming no mutation
    def __eq__(self, other):
        return ((self.block, self.off, self.size)
                == (other.block, other.off, other.size))

    def __ne__(self, other):
        return ((self.block, self.off, self.size)
                != (other.block, other.off, other.size))

    def __hash__(self):
        return hash((self.block, self.off, self.size))


# lazy config object
class Config:
    def __init__(self, mroot):
        self.mroot = mroot

    # lookup a specific tag
    def lookup(self, tag=None, mask=None):
        rattr = self.mroot.rbyd.lookup(-1, tag, mask)
        if rattr is None:
            return None

        return self._parse(rattr.tag, rattr)

    def __getitem__(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        return self.lookup(*key)

    def __contains__(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        return self.lookup(*key) is not None

    def __iter__(self):
        for rattr in self.mroot.rbyd.rattrs(-1, TAG_CONFIG, 0xff):
            yield self._parse(rattr.tag, rattr)

    # common config operations
    class Config:
        tag = None
        mask = None

        def __init__(self, mroot, tag, rattr):
            # replace tag with what we find
            self.tag = tag
            # and keep track of rattr
            self.rattr = rattr

        @property
        def block(self):
            return self.rattr.block

        @property
        def blocks(self):
            return self.rattr.blocks

        @property
        def toff(self):
            return self.rattr.toff

        @property
        def tdata(self):
            return self.rattr.data

        @property
        def off(self):
            return self.rattr.off

        @property
        def data(self):
            return self.rattr.data

        @property
        def size(self):
            return self.rattr.size

        def __bytes__(self):
            return self.data

        def __repr__(self):
            return '<%s %s>' % (self.__class__.__name__, self.repr())

        def repr(self):
            return self.rattr.repr()

        def __iter__(self):
            return iter((self.tag, self.data))

        def __eq__(self, other):
            return (self.tag, self.data) == (other.tag, other.data)

        def __ne__(self, other):
            return (self.tag, self.data) != (other.tag, other.data)

        def __hash__(self):
            return hash((self.tag, self.data))

    # marker class for unknown config
    class Unknown(Config):
        pass

    # special handling for known configs

    # the filesystem magic string
    class Magic(Config):
        tag = TAG_MAGIC

        def repr(self):
            return 'magic \"%s\"' % (
                    ''.join(b if b >= ' ' and b <= '~' else '.'
                            for b in map(chr, self.data)))

    # version tuple
    class Version(Config):
        tag = TAG_VERSION

        def __init__(self, mroot, tag, rattr):
            super().__init__(mroot, tag, rattr)
            d = 0
            self.major, d_ = fromleb128(self.data[d:]); d += d_
            self.minor, d_ = fromleb128(self.data[d:]); d += d_

        @property
        def tuple(self):
            return (self.major, self.minor)

        def repr(self):
            return 'version v%s.%s' % (self.major, self.minor)

    # compat flags
    class Rcompat(Config):
        tag = TAG_RCOMPAT

        def repr(self):
            return 'rcompat 0x%s' % (
                    ''.join('%02x' % f for f in reversed(self.data)))

    class Wcompat(Config):
        tag = TAG_WCOMPAT

        def repr(self):
            return 'wcompat 0x%s' % (
                    ''.join('%02x' % f for f in reversed(self.data)))

    class Ocompat(Config):
        tag = TAG_OCOMPAT

        def repr(self):
            return 'ocompat 0x%s' % (
                    ''.join('%02x' % f for f in reversed(self.data)))

    # block device geometry
    class Geometry(Config):
        tag = TAG_GEOMETRY
        mask = 0x3

        def __init__(self, mroot, tag, rattr):
            super().__init__(mroot, tag, rattr)
            d = 0
            block_size, d_ = fromleb128(self.data[d:]); d += d_
            block_count, d_ = fromleb128(self.data[d:]); d += d_
            # these are offset by 1 to avoid overflow issues
            self.block_size = block_size + 1
            self.block_count = block_count + 1

        def repr(self):
            return 'geometry %sx%s' % (self.block_size, self.block_count)

    # file name limit
    class NameLimit(Config):
        tag = TAG_NAMELIMIT

        def __init__(self, mroot, tag, rattr):
            super().__init__(mroot, tag, rattr)
            self.limit, _ = fromleb128(self.data)

        def __int__(self):
            return self.limit

        def repr(self):
            return 'namelimit %s' % self.limit

    # file size limit
    class FileLimit(Config):
        tag = TAG_FILELIMIT

        def __init__(self, mroot, tag, rattr):
            super().__init__(mroot, tag, rattr)
            self.limit, _ = fromleb128(self.data)

        def __int__(self):
            return self.limit

        def repr(self):
            return 'filelimit %s' % self.limit

    # keep track of known configs
    _known = [c for c in Config.__subclasses__() if c.tag is not None]

    # parse if known
    def _parse(self, tag, rattr):
        # known config?
        for c in self._known:
            if (c.tag & ~(c.mask or 0)) == (tag & ~(c.mask or 0)):
                return c(self.mroot, tag, rattr)
        # otherwise return a marker class
        else:
            return self.Unknown(self.mroot, tag, rattr)

    # create cached accessors for known config
    def _parser(c):
        def _parser(self):
            return self.lookup(c.tag, c.mask)
        return _parser

    for c in _known:
        locals()[c.__name__.lower()] = ft.cached_property(_parser(c))

# lazy gstate object
class Gstate:
    def __init__(self, mtree):
        self.mtree = mtree

    # lookup a specific tag
    def lookup(self, tag=None, mask=None):
        # collect relevant gdeltas in the mtree
        gdeltas = []
        for mdir in self.mtree.mdirs():
            # gcksumdelta is a bit special since it's outside the
            # rbyd tree
            if tag == TAG_GCKSUMDELTA:
                gdelta = mdir.gcksumdelta
            else:
                gdelta = mdir.rbyd.lookup(-1, tag, mask)
            if gdelta is not None:
                gdeltas.append((mdir.mid, gdelta))

        # xor to find gstate
        return self._parse(tag, gdeltas)

    def __getitem__(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        return self.lookup(*key)

    def __contains__(self, key):
        # note gstate doesn't really "not exist" like normal attrs,
        # missing gstate is equivalent to zero gstate, but we can
        # still test if there are any gdeltas that match the given
        # tag here
        if not isinstance(key, tuple):
            key = (key,)

        return any(
                (mdir.gcksumdelta if tag == TAG_GCKSUMDELTA
                        else mdir.rbyd.lookup(-1, *key))
                    is not None
                for mdir in self.mtree.mdirs())

    def __iter__(self):
        # first figure out what gstate tags actually exist in the
        # filesystem
        gtags = set()
        for mdir in self.mtree.mdirs():
            if mdir.gcksumdelta is not None:
                gtags.add(TAG_GCKSUMDELTA)

            for rattr in mdir.rbyd.rattrs(-1):
                if (rattr.tag & 0xff00) == TAG_GDELTA:
                    gtags.add(rattr.tag)

        # sort to keep things stable, moving gcksum to the front
        gtags = sorted(gtags, key=lambda t: (-(t & 0xf000), t))

        # compute all gstate in one pass (well, two technically)
        gdeltas = {tag: [] for tag in gtags}
        for mdir in self.mtree.mdirs():
            for tag in gtags:
                # gcksumdelta is a bit special since it's outside the
                # rbyd tree
                if tag == TAG_GCKSUMDELTA:
                    gdelta = mdir.gcksumdelta
                else:
                    gdelta = mdir.rbyd.lookup(-1, tag)
                if gdelta is not None:
                    gdeltas[tag].append((mdir.mid, gdelta))

        for tag in gtags:
            # xor to find gstate
            yield self._parse(tag, gdeltas[tag])

    # common gstate operations
    class Gstate:
        tag = None
        mask = None

        def __init__(self, mtree, tag, gdeltas):
            # replace tag with what we find
            self.tag = tag
            # keep track of gdeltas for debugging
            self.gdeltas = gdeltas

            # xor together to build our gstate
            data = bytes()
            for mid, gdelta in gdeltas:
                data = bytes(
                        a^b for a, b in it.zip_longest(
                            data, gdelta.data,
                            fillvalue=0))
            self.data = data

        @property
        def size(self):
            return len(self.data)

        def __bytes__(self):
            return self.data

        def __repr__(self):
            return '<%s %s>' % (self.__class__.__name__, self.repr())

        def repr(self):
            return tagrepr(self.tag, 0, self.size, global_=True)

        def __iter__(self):
            return iter((self.tag, self.data))

        def __eq__(self, other):
            return (self.tag, self.data) == (other.tag, other.data)

        def __ne__(self, other):
            return (self.tag, self.data) != (other.tag, other.data)

        def __hash__(self):
            return hash((self.tag, self.data))

    # marker class for unknown gstate
    class Unknown(Gstate):
        pass

    # special handling for known gstate

    # the global-checksum, cubed
    class Gcksum(Gstate):
        tag = TAG_GCKSUMDELTA

        def __init__(self, mtree, tag, gdeltas):
            super().__init__(mtree, tag, gdeltas)
            self.gcksum = fromle32(self.data)

        def __int__(self):
            return self.gcksum

        def repr(self):
            return 'gcksum %08x' % self.gcksum

    # any global-removes
    class Grm(Gstate):
        tag = TAG_GRMDELTA

        def __init__(self, mtree, tag, gdeltas):
            super().__init__(mtree, tag, gdeltas)
            d = 0
            count, d_ = fromleb128(self.data[d:]); d += d_
            rms = []
            if count <= 2:
                for _ in range(count):
                    mid, d_ = fromleb128(self.data[d:]); d += d_
                    rms.append(mtree.mid(mid))
            self.count = count
            self.rms = rms

        def repr(self):
            return 'grm %s' % (
                    'none' if self.count == 0
                        else ' '.join(mid.repr() for mid in self.rms)
                        if self.count <= 2
                        else '0x%x %d' % (self.count, len(self.data)))

    # keep track of known gstate
    _known = [g for g in Gstate.__subclasses__() if g.tag is not None]

    # parse if known
    def _parse(self, tag, gdeltas):
        # known config?
        for g in self._known:
            if (g.tag & ~(g.mask or 0)) == (tag & ~(g.mask or 0)):
                return g(self.mtree, tag, gdeltas)
        # otherwise return a marker class
        else:
            return Unknown(self.mtree, tag, gdeltas)

    # create cached accessors for known gstate
    def _parser(g):
        def _parser(self):
            return self.lookup(g.tag, g.mask)
        return _parser

    for g in _known:
        locals()[g.__name__.lower()] = ft.cached_property(_parser(g))


# high-level littlefs representation
class Lfs:
    def __init__(self, bd, mtree, config=None, gstate=None, cksum=None, *,
            corrupt=False):
        self.bd = bd
        self.mtree = mtree

        # create lazy config/gstate objects
        self.config = config or Config(self.mroot)
        self.gstate = gstate or Gstate(self.mtree)

        # go ahead and fetch some expected fields
        self.version = self.config.version
        self.rcompat = self.config.rcompat
        self.wcompat = self.config.wcompat
        self.ocompat = self.config.ocompat
        if self.config.geometry is not None:
            self.block_count = self.config.geometry.block_count
            self.block_size = self.config.geometry.block_size
        else:
            self.block_count = self.bd.block_count
            self.block_size = self.bd.block_size

        # calculate on-disk gcksum
        if cksum is None:
            cksum = 0
            for mdir in self.mtree.mdirs():
                cksum ^= mdir.cksum
        self.cksum = cksum

        # is the filesystem corrupt?
        self.corrupt = corrupt

        # check mroot
        if not self.corrupt and not self.ckmroot():
            self.corrupt = True

        # check magic
        if not self.corrupt and not self.ckmagic():
            self.corrupt = True

        # check gcksum
        if not self.corrupt and not self.ckcksum():
            self.corrupt = True

        # create the root directory, this is a bit of a special case
        self.root = self.Root(self)

    # mbits is a static value derived from the block_size
    @staticmethod
    def mbits_(block_size):
        return Mtree.mbits_(block_size)

    @property
    def mbits(self):
        return self.mtree.mbits

    # convenience function for creating mbits-dependent mids
    def mid(self, mbid, mrid=None):
        return self.mtree.mid(mbid, mrid)

    # most of our fields map to the mtree
    @property
    def block(self):
        return self.mroot.block

    @property
    def blocks(self):
        return self.mroot.blocks

    @property
    def trunk(self):
        return self.mroot.trunk

    @property
    def rev(self):
        return self.mroot.rev

    @property
    def weight(self):
        return self.mtree.weight

    @property
    def mbweight(self):
        return self.mtree.mbweight

    @property
    def mrweight(self):
        return self.mtree.mrweight

    def mbweightrepr(self):
        return self.mtree.mbweightrepr()

    def mrweightrepr(self):
        return self.mtree.mrweightrepr()

    @property
    def mrootchain(self):
        return self.mtree.mrootchain

    @property
    def mrootanchor(self):
        return self.mtree.mrootanchor

    @property
    def mroot(self):
        return self.mtree.mroot

    def addr(self):
        return self.mroot.addr()

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self.repr())

    def repr(self):
        return 'littlefs v%s.%s %sx%s %s w%s.%s' % (
                self.version.major if self.version is not None else '?',
                self.version.minor if self.version is not None else '?',
                self.block_size if self.block_size is not None else '?',
                self.block_count if self.block_count is not None else '?',
                self.addr(),
                self.mbweightrepr(), self.mrweightrepr())

    def __bool__(self):
        return not self.corrupt

    def __eq__(self, other):
        return self.mrootanchor == other.mrootanchor

    def __ne__(self, other):
        return self.mrootanchor != other.mrootanchor

    def __hash__(self):
        return hash(self.mrootanchor)

    @classmethod
    def fetch(cls, bd, blocks=None, trunk=None, *,
            depth=None):
        # Mtree does most of the work here
        mtree = Mtree.fetch(bd, blocks, trunk,
                depth=depth)
        return cls(bd, mtree)

    # check that the mroot is valid
    def ckmroot(self):
        return bool(self.mroot)

    # check that the magic string is littlefs
    def ckmagic(self):
        if self.config.magic is None:
            return False
        return self.config.magic.data == b'littlefs'

    # check that the gcksum checks out
    def ckcksum(self):
        return crc32ccube(self.cksum) == int(self.gstate.gcksum)

    # read custom attrs
    def uattrs(self):
        return self.mroot.rattrs(-1, TAG_UATTR, 0xff)

    def sattrs(self):
        return self.mroot.rattrs(-1, TAG_SATTR, 0xff)

    def attrs(self):
        yield from self.uattrs()
        yield from self.sattrs()

    # is file in grm queue?
    def grmed(self, mid):
        if not isinstance(mid, Mid):
            mid = self.mid(mid)

        return mid in self.gstate.grm.rms

    # lookup operations 
    def lookup(self, mid, mdir=None, *,
            all=False):
        all_ = all; del all

        # is this mid grmed?
        if not all_ and self.grmed(mid):
            return None

        if mdir is None:
            mdir, name = self.mtree.lookup(mid)
            if mdir is None:
                return None
        else:
            name = mdir.lookup(mid)

        # stickynote?
        if not all_ and name.tag == TAG_STICKYNOTE:
            return None

        return self._open(mid, mdir, name.tag, name)

    def namelookup(self, did, name, *,
            all=False):
        all_ = all; del all

        mid_, mdir_, name_ = self.mtree.namelookup(did, name)
        if mid_ is None:
            return None

        # is this mid grmed?
        if not all_ and self.grmed(mid_):
            return None

        # stickynote?
        if not all_ and name_.tag == TAG_STICKYNOTE:
            return None

        return self._open(mid_, mdir_, name_.tag, name_)

    class PathError(Exception):
        pass

    # split a path into its components
    #
    # note this follows littlefs's internal logic, so dots and dotdot
    # entries get resolved _before_ walking the path
    @staticmethod
    def pathsplit(path):
        path_ = path
        if isinstance(path_, str):
            path_ = path_.encode('utf8')

        # empty path?
        if path_ == b'':
            raise Lfs.PathError("invalid path: %r" % path)

        path__ = []
        for p in path_.split(b'/'):
            # skip multiple slashes and dots
            if p == b'' or p == b'.':
                continue
            path__.append(p)
        path_ = path__

        # resolve dotdots
        path__ = []
        dotdots = 0
        for p in reversed(path_):
            if p == b'..':
                dotdots += 1
            elif dotdots:
                dotdots -= 1
            else:
                path__.append(p)
        if dotdots:
            raise Lfs.PathError("invalid path: %r" % path)
        path__.reverse()
        path_ = path__

        return path_

    def pathlookup(self, did, path_=None, *,
            all=False,
            path=False,
            depth=None):
        all_ = all; del all

        # default to the root directory
        if path_ is None:
            did, path_ = 0, did
        # parse/split the path
        if isinstance(path_, (bytes, str)):
            path_ = self.pathsplit(path_)

        # start at the root dir
        dir = self.root
        did = did
        if path or depth:
            path__ = []

        for p in path_:
            # lookup the next file
            file = self.namelookup(did, p,
                    all=all_)
            if file is None:
                if path:
                    return None, path__
                else:
                    return None

            # file? done?
            if not file.recursable:
                if path:
                    return file, path__
                else:
                    return file

            # recurse down the file tree
            dir = file
            did = dir.did
            if path or depth:
                path__.append(dir)
                # stop here?
                if depth and len(path__) >= depth:
                    if path:
                        return None, path__
                    else:
                        return None

        if path:
            return dir, path__
        else:
            return dir

    def files(self, did=None, *,
            all=False,
            path=False,
            depth=None):
        all_ = all; del all

        # default to the root directory
        did = did or self.root.did

        # start with the bookmark entry
        mid, mdir, name = self.mtree.namelookup(did, b'')
        # no bookmark? weird
        if mid is None:
            return

        # iterate over files until we find a different did
        while name.did == did:
            # yield file, hiding grms, stickynotes, etc, by default
            if all_ or (not self.grmed(mid)
                    and not name.tag == TAG_BOOKMARK
                    and not name.tag == TAG_STICKYNOTE):
                file = self._open(mid, mdir, name.tag, name)
                if path:
                    yield file, []
                else:
                    yield file

                # recurse?
                if (file.recursable
                        and depth is not None
                        and (depth == 0 or depth > 1)):
                    for r in self.files(file.did,
                            all=all_,
                            path=path,
                            depth=depth-1 if depth else 0):
                        if path:
                            file_, path_ = r
                            yield file_, [file]+path_
                        else:
                            file_ = r
                            yield file_

            # increment mid and find the next mdir if needed
            mbid, mrid = mid.mbid, mid.mrid + 1
            if mrid == mdir.weight:
                mbid, mrid = mbid + (1 << self.mbits), 0
                mdir = self.mtree.lookupleaf(mbid)
                if mdir is None:
                    break
            # lookup name and adjust rid if necessary, you don't
            # normally need to do this, but we don't want the iteration
            # to terminate early on a corrupt filesystem
            mrid, name = mdir.rbyd.lookupnext(mrid)
            if mrid is None:
                break
            mid = self.mid(mbid, mrid)

    def orphans(self,
            all=False):
        all_ = all; del all

        # first find all reachable dids
        dids = {self.root.did}
        for file in self.files(depth=mt.inf):
            if file.recursable:
                dids.add(file.did)

        # then iterate over all dids and yield any that aren't reachable
        for mid, mdir, name in self.mtree.mids():
            # is this mid grmed?
            if not all_ and self.grmed(mid):
                continue

            # stickynote?
            if not all_ and name.tag == TAG_STICKYNOTE:
                continue

            # unreachable? note this lazily parses the did
            if name.did not in dids:
                file = self._open(mid, mdir, name.tag, name)
                # mark as orphaned
                file.orphaned = True
                yield file

    # traverse the filesystem
    def traverse(self, *,
            mtree_only=False,
            shrubs=False,
            fragments=False,
            path=False):
        # traverse the mtree
        for r in self.mtree.traverse(
                path=path):
            if path:
                mdir, path_ = r
            else:
                mdir = r

            # mdir?
            if isinstance(mdir, Mdir):
                if path:
                    yield mdir, path_
                else:
                    yield mdir

            # btree node? we only care about the rbyd for simplicity
            else:
                bid, rbyd = mdir
                if path:
                    yield rbyd, path_
                else:
                    yield rbyd

            # traverse file bshrubs/btrees
            if not mtree_only and isinstance(mdir, Mdir):
                for mid, name in mdir.mids():
                    file = self._open(mid, mdir, name.tag, name)
                    for r in file.traverse(
                            path=path):
                        if path:
                            pos, data, path__ = r
                            path__ = [(mid, mdir, name)]+path__
                        else:
                            pos, data = r

                        # inlined data? we usually ignore these
                        if isinstance(data, Rattr):
                            if fragments:
                                if path:
                                    yield data, path_+path__
                                else:
                                    yield data
                        # block pointer?
                        elif isinstance(data, Bptr):
                            if path:
                                yield data, path_+path__
                            else:
                                yield data
                        # bshrub/btree node? we only care about the rbyd
                        # for simplicity, we also usually ignore shrubs
                        # since these live the the parent mdir
                        else:
                            if shrubs or not data.shrub:
                                if path:
                                    yield data, path_+path__
                                else:
                                    yield data

    # common file operations, note Reg extends this for regular files
    class File:
        tag = None
        mask = None
        internal = False
        recursable = False
        grmed = False
        orphaned = False

        def __init__(self, lfs, mid, mdir, tag, name):
            self.lfs = lfs
            self.mid = mid
            self.mdir = mdir
            # replace tag with what we find
            self.tag = tag
            self.name = name

            # fetch the file structure if there is one
            self.struct = mdir.lookup(mid, TAG_STRUCT, 0xff)

            # bshrub/btree?
            self.bshrub = None
            if (self.struct is not None
                    and (self.struct.tag & ~0x3) == TAG_BSHRUB):
                weight, trunk = fromshrub(self.struct.data)
                self.bshrub = Btree.fetchshrub(lfs.bd, mdir.rbyd, trunk)
            elif (self.struct is not None
                    and (self.struct.tag & ~0x3) == TAG_BTREE):
                weight, block, trunk, cksum = frombtree(self.struct.data)
                self.bshrub = Btree.fetchck(
                        lfs.bd, block, trunk, weight, cksum)

            # did?
            self.did = None
            if (self.struct is not None
                    and self.struct.tag == TAG_DID):
                self.did, _ = fromleb128(self.struct.data)

            # some other info that is useful for scripts

            # mark as grmed if grmed
            if lfs.grmed(mid):
                self.grmed = True

        @property
        def size(self):
            if self.bshrub is not None:
                return self.bshrub.weight
            else:
                return 0

        def structrepr(self):
            if self.struct is not None:
                # inlined bshrub?
                if (self.struct.tag & ~0x3) == TAG_BSHRUB:
                    return 'bshrub %s' % self.bshrub.addr()
                # btree?
                elif (self.struct.tag & ~0x3) == TAG_BTREE:
                    return 'btree %s' % self.bshrub.addr()
                # btree?
                else:
                    return str(self.struct)
            else:
                return ''

        def __repr__(self):
            return '<%s %s.%s %s>' % (
                    self.__class__.__name__,
                    self.mid.mbidrepr(), self.mid.mridrepr(),
                    self.repr())

        def repr(self):
            return 'type 0x%02x%s' % (
                    self.tag & 0xff,
                    ', %s' % self.structrepr()
                        if self.struct is not None else '')

        def __eq__(self, other):
            return self.mid == other.mid

        def __ne__(self, other):
            return self.mid != other.mid

        def __hash__(self):
            return hash(self.mid)

        # read attrs, note this includes _all_ attrs
        def rattrs(self):
            return self.mdir.rattrs(self.mid)

        # read custom attrs
        def uattrs(self):
            return self.mdir.rattrs(self.mid, TAG_UATTR, 0xff)

        def sattrs(self):
            return self.mdir.rattrs(self.mid, TAG_SATTR, 0xff)

        def attrs(self):
            yield from self.uattrs()
            yield from self.sattrs()

        # lookup data in the underlying bshrub
        def _lookupleaf(self, pos, *,
                path=False,
                depth=None):
            # no bshrub?
            if self.bshrub is None:
                if path:
                    return None, None, []
                else:
                    return None, None

            # lookup data in our bshrub
            r = self.bshrub.lookupleaf(pos,
                    path=path or depth,
                    depth=depth)
            if path or depth:
                bid, rbyd, rid, rattr, path_ = r
            else:
                bid, rbyd, rid, rattr = r
            if bid is None:
                if path:
                    return None, None, path_
                else:
                    return None, None

            # corrupt btree node?
            if not rbyd:
                if path:
                    return bid-(rbyd.weight-1), rbyd, path_
                else:
                    return bid-(rbyd.weight-1), rbyd

            # stop here?
            if depth and len(path_) >= depth:
                if path:
                    return bid-(rattr.weight-1), rbyd, path_
                else:
                    return bid-(rattr.weight-1), rbyd

            # inlined data?
            if (rattr.tag & ~0x1003) == TAG_DATA:
                if path:
                    return bid-(rattr.weight-1), rattr, path_
                else:
                    return bid-(rattr.weight-1), rattr
            # block pointer?
            elif (rattr.tag & ~0x1003) == TAG_BLOCK:
                size, block, off, cksize, cksum = frombptr(rattr.data)
                bptr = Bptr.fetchck(self.lfs.bd, rattr,
                        block, off, size, cksize, cksum)
                if path:
                    return bid-(rattr.weight-1), bptr, path_
                else:
                    return bid-(rattr.weight-1), bptr
            # uh oh, something is broken
            else:
                if path:
                    return bid-(rattr.weight-1), rattr, path_
                else:
                    return bid-(rattr.weight-1), rattr

        def lookupleaf(self, pos, *,
                data_only=True,
                path=False,
                depth=None):
            r = self._lookupleaf(pos,
                    path=path,
                    depth=depth)
            if path:
                pos, data, path_ = r
            else:
                pos, data = r
            if pos is None or (
                    data_only and not isinstance(data, (Rattr, Bptr))):
                if path:
                    return None, None, path_
                else:
                    return None, None

            if path:
                return pos, data, path_
            else:
                return pos, data

        def _leaves(self, *,
                path=False,
                depth=None):
            pos = 0
            while True:
                r = self.lookupleaf(pos,
                        data_only=False,
                        path=path,
                        depth=depth)
                if path:
                    pos, data, path_ = r
                else:
                    pos, data = r
                if pos is None:
                    break

                # data?
                if isinstance(data, (Rattr, Bptr)):
                    if path:
                        yield pos, data, path_
                    else:
                        yield pos, data
                    pos += data.weight
                # btree node?
                else:
                    rbyd = data
                    if path:
                        yield (pos, rbyd,
                                # path tail is usually redundant unless corrupt
                                path_[:-1] if rbyd else path_)
                    else:
                        yield pos, rbyd
                    pos += rbyd.weight

        def leaves(self, *,
                data_only=False,
                path=False,
                depth=None):
            for r in self._leaves(
                    path=path,
                    depth=depth):
                if path:
                    pos, data, path_ = r
                else:
                    pos, data = r
                if data_only and not isinstance(data, (Rattr, Bptr)):
                    continue

                if path:
                    yield pos, data, path_
                else:
                    yield pos, data

        def _traverse(self, *,
                path=False,
                depth=None):
            ptrunk_ = []
            for pos, data, path_ in self.leaves(
                    path=True,
                    depth=depth):
                # we only care about the data/rbyds here
                trunk_ = ([(bid_-rid_, rbyd_)
                            for bid_, rbyd_, rid_, name_ in path_]
                        + [(pos, data)])
                for d, (pos, data) in pathdelta(
                        trunk_, ptrunk_):
                    # but include branch rids in path if requested
                    if path:
                        yield pos, data, path_[:d]
                    else:
                        yield pos, data
                ptrunk_ = trunk_

        def traverse(self, *,
                data_only=False,
                path=False,
                depth=None):
            for r in self._traverse(
                    path=path,
                    depth=depth):
                if path:
                    pos, data, path_ = r
                else:
                    pos, data = r
                if data_only and not isinstance(data, (Rattr, Bptr)):
                    continue

                if path:
                    yield pos, data, path_
                else:
                    yield pos, data

        def datas(self, *,
                data_only=True,
                path=False,
                depth=None):
            return self.leaves(
                    data_only=data_only,
                    path=path,
                    depth=depth)

        # some convience operations for reading data
        def bytes(self, *,
                depth=None):
            for pos, data in self.datas(depth=depth):
                if data.size > 0:
                    yield data.data
                if data.weight > data.size:
                    yield b'\0' * (data.weight-data.size)

        def read(self, *,
                depth=None):
            return b''.join(self.bytes())

    # bleh, with that out of the way, here are our known file types

    # regular files
    class Reg(File):
        tag = TAG_REG

        def repr(self):
            return 'reg %s%s' % (
                    self.size,
                    ', %s' % self.structrepr()
                        if self.struct is not None else '')

    # directories
    class Dir(File):
        tag = TAG_DIR

        def __init__(self, lfs, mid, mdir, tag, name):
            super().__init__(lfs, mid, mdir, tag, name)

            # we're recursable if we're a non-grmed directory with a did
            if (isinstance(self, Lfs.Dir)
                    and not self.grmed
                    and self.did is not None):
                self.recursable = True

        def repr(self):
            return 'dir %s%s' % (
                    '0x%x' % self.did
                        if self.did is not None else '?',
                    ', %s' % self.structrepr()
                        if self.struct is not None
                            and self.struct.tag != TAG_DID else '')

        # provide some convenient filesystem access relative to our did
        def namelookup(self, name, **args):
            if self.did is None:
                return None
            return self.lfs.namelookup(self.did, name, **args)

        def pathlookup(self, path_, **args):
            if self.did is None:
                if args.get('path'):
                    return None, []
                else:
                    return None
            return self.lfs.pathlookup(self.did, path_, **args)

        def files(self, **args):
            if self.did is None:
                return iter(())
            return self.lfs.files(self.did, **args)

    # root is a bit special
    class Root(Dir):
        tag = None

        def __init__(self, lfs):
            # root always has mid=-1 and did=0
            super().__init__(lfs, lfs.mid(-1), lfs.mroot, TAG_DIR, None)
            self.did = 0
            self.recursable = True

        def repr(self):
            return 'root'

    # bookmarks keep track of where directories start
    class Bookmark(File):
        tag = TAG_BOOKMARK
        internal = True

        def repr(self):
            return 'bookmark %s%s' % (
                    '0x%x' % self.name.did
                        if self.name.did is not None else '?',
                    ', %s' % self.structrepr()
                        if self.struct is not None else '')

    # stickynotes, i.e. uncommitted files, behave the same as files
    # for the most part
    class Stickynote(File):
        tag = TAG_STICKYNOTE
        internal = True

        def repr(self):
            return 'stickynote%s' % (
                    ' %s, %s' % (self.size, self.structrepr())
                        if self.struct is not None else '')

    # marker class for unknown file types
    class Unknown(File):
        pass

    # keep track of known file types
    _known = [f for f in File.__subclasses__() if f.tag is not None]

    # fetch/parse state if known
    def _open(self, mid, mdir, tag, name):
        # known file type?
        tag = name.tag
        for f in self._known:
            if (f.tag & ~(f.mask or 0)) == (tag & ~(f.mask or 0)):
                return f(self, mid, mdir, tag, name)
        # otherwise return a marker class
        else:
            return self.Unknown(self, mid, mdir, tag, name)


# a representation of optionally key-mapped attrs
class Attr:
    def __init__(self, attrs, defaults=None):
        if attrs is None:
            attrs = []
        if isinstance(attrs, dict):
            attrs = attrs.items()

        # normalize
        self.attrs = []
        self.keyed = co.OrderedDict()
        for attr in attrs:
            if (not isinstance(attr, tuple)
                    or attr[0] in {None, (), (None,), ('*',)}):
                attr = ((), attr)
            if not isinstance(attr[0], tuple):
                attr = ((attr[0],), attr[1])

            self.attrs.append(attr)
            if attr[0] not in self.keyed:
                self.keyed[attr[0]] = []
            self.keyed[attr[0]].append(attr[1])

        # create attrs object for defaults
        if isinstance(defaults, Attr):
            self.defaults = defaults
        elif defaults is not None:
            self.defaults = Attr(defaults)
        else:
            self.defaults = None

    def __repr__(self):
        if self.defaults is None:
            return 'Attr(%r)' % (
                    [(','.join(attr[0]), attr[1])
                        for attr in self.attrs])
        else:
            return 'Attr(%r, %r)' % (
                    [(','.join(attr[0]), attr[1])
                        for attr in self.attrs],
                    [(','.join(attr[0]), attr[1])
                        for attr in self.defaults.attrs])

    def __iter__(self):
        if () in self.keyed:
            return it.cycle(self.keyed[()])
        elif self.defaults is not None:
            return iter(self.defaults)
        else:
            return iter(())

    def __bool__(self):
        return bool(self.attrs)

    def __getitem__(self, key):
        if isinstance(key, tuple):
            if len(key) > 0 and not isinstance(key[0], str):
                i, key = key
            else:
                i, key = 0, key
        else:
            i, key = key, ()

        if not isinstance(key, tuple):
            key = (key,)

        # try to lookup by key
        best = None
        for ks, vs in self.keyed.items():
            prefix = []
            for j, k in enumerate(ks):
                if j < len(key) and fnmatch.fnmatchcase(key[j], k):
                    prefix.append(k)
                else:
                    prefix = None
                    break

            if prefix is not None and (
                    best is None or len(prefix) >= len(best[0])):
                best = (prefix, vs)

        if best is not None:
            # cycle based on index
            return best[1][i % len(best[1])]

        # fallback to defaults?
        if self.defaults is not None:
            return self.defaults[i, key]

        return None

    def __contains__(self, key):
        return self.__getitem__(key) is not None

    # a key function for sorting by key order
    def key(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        best = None
        for i, ks in enumerate(self.keyed.keys()):
            prefix = []
            for j, k in enumerate(ks):
                if j < len(key) and (not k or key[j] == k):
                    prefix.append(k)
                else:
                    prefix = None
                    break

            if prefix is not None and (
                    best is None or len(prefix) >= len(best[0])):
                best = (prefix, i)

        if best is not None:
            return best[1]

        # fallback to defaults?
        if self.defaults is not None:
            return len(self.keyed) + self.defaults.key(key)

        return len(self.keyed)

# parse %-escaped strings
def punescape(s, attrs=None):
    if attrs is None:
        attrs = {}
    if isinstance(attrs, dict):
        attrs_ = attrs
        attrs = lambda k: attrs_[k]

    pattern = re.compile(
        '%[%n]'
            '|' '%x..'
            '|' '%u....'
            '|' '%U........'
            '|' '%\((?P<field>[^)]*)\)'
                '(?P<format>[+\- #0-9\.]*[sdboxXfFeEgG])')
    def unescape(m):
        if m.group()[1] == '%': return '%'
        elif m.group()[1] == 'n': return '\n'
        elif m.group()[1] == 'x': return chr(int(m.group()[2:], 16))
        elif m.group()[1] == 'u': return chr(int(m.group()[2:], 16))
        elif m.group()[1] == 'U': return chr(int(m.group()[2:], 16))
        elif m.group()[1] == '(':
            try:
                v = attrs(m.group('field'))
            except KeyError:
                return m.group()
            f = m.group('format')
            if f[-1] in 'dboxX':
                if isinstance(v, str):
                    v = dat(v, 0)
                v = int(v)
            elif f[-1] in 'fFeEgG':
                if isinstance(v, str):
                    v = dat(v, 0)
                v = float(v)
            else:
                f = ('<' if '-' in f else '>') + f.replace('-', '')
                v = str(v)
            # note we need Python's new format syntax for binary
            return ('{:%s}' % f).format(v)
        else: assert False
    return re.sub(pattern, unescape, s)


# TODO sync these

# naive space filling curve (the default)
def naive_curve(width, height):
    for y in range(height):
        for x in range(width):
            yield x, y

# space filling Hilbert-curve
def hilbert_curve(width, height):
    # based on generalized Hilbert curves:
    # https://github.com/jakubcerveny/gilbert
    #
    def hilbert_(x, y, a_x, a_y, b_x, b_y):
        w = abs(a_x+a_y)
        h = abs(b_x+b_y)
        a_dx = -1 if a_x < 0 else +1 if a_x > 0 else 0
        a_dy = -1 if a_y < 0 else +1 if a_y > 0 else 0
        b_dx = -1 if b_x < 0 else +1 if b_x > 0 else 0
        b_dy = -1 if b_y < 0 else +1 if b_y > 0 else 0

        # trivial row
        if h == 1:
            for _ in range(w):
                yield x, y
                x, y = x+a_dx, y+a_dy
            return

        # trivial column
        if w == 1:
            for _ in range(h):
                yield x, y
                x, y = x+b_dx, y+b_dy
            return

        a_x_, a_y_ = a_x//2, a_y//2
        b_x_, b_y_ = b_x//2, b_y//2
        w_ = abs(a_x_+a_y_)
        h_ = abs(b_x_+b_y_)

        if 2*w > 3*h:
            # prefer even steps
            if w_ % 2 != 0 and w > 2:
                a_x_, a_y_ = a_x_+a_dx, a_y_+a_dy

            # split in two
            yield from hilbert_(
                    x, y,
                    a_x_, a_y_, b_x, b_y)
            yield from hilbert_(
                    x+a_x_, y+a_y_,
                    a_x-a_x_, a_y-a_y_, b_x, b_y)
        else:
            # prefer even steps
            if h_ % 2 != 0 and h > 2:
                b_x_, b_y_ = b_x_+b_dx, b_y_+b_dy

            # split in three
            yield from hilbert_(
                    x, y,
                    b_x_, b_y_, a_x_, a_y_)
            yield from hilbert_(
                    x+b_x_, y+b_y_,
                    a_x, a_y, b_x-b_x_, b_y-b_y_)
            yield from hilbert_(
                    x+(a_x-a_dx)+(b_x_-b_dx), y+(a_y-a_dy)+(b_y_-b_dy),
                    -b_x_, -b_y_, -(a_x-a_x_), -(a_y-a_y_))

    if width >= height:
        yield from hilbert_(0, 0, +width, 0, 0, +height)
    else:
        yield from hilbert_(0, 0, 0, +height, +width, 0)

# space filling Z-curve/Lebesgue-curve
def lebesgue_curve(width, height):
    # we create a truncated Z-curve by simply filtering out the
    # points that are outside our region
    for i in range(2**(2*mt.ceil(mt.log2(max(width, height))))):
        # we just operate on binary strings here because it's easier
        b = '{:0{}b}'.format(i, 2*mt.ceil(mt.log2(i+1)/2))
        x = int(b[1::2], 2) if b[1::2] else 0
        y = int(b[0::2], 2) if b[0::2] else 0
        if x < width and y < height:
            yield x, y


# an abstract block representation
class BmapBlock:
    def __init__(self, block, type='unused', value=None, usage=range(0), *,
            siblings=None, children=None,
            x=None, y=None, width=None, height=None):
        self.block = block
        self.type = type
        self.value = value
        self.usage = usage
        self.siblings = siblings if siblings is not None else set()
        self.children = children if children is not None else set()
        self.x = x
        self.y = y
        self.width = width
        self.height = height

    def __repr__(self):
        return 'BmapBlock(0x%x, %r, x=%s, y=%s, width=%s, height=%s)' % (
                self.block,
                self.type,
                self.x, self.y, self.width, self.height)

    def __eq__(self, other):
        return self.block == other.block

    def __ne__(self, other):
        return self.block != other.block

    def __hash__(self):
        return hash(self.block)

    def __lt__(self, other):
        return self.block < other.block

    def __le__(self, other):
        return self.block <= other.block

    def __gt__(self, other):
        return self.block > other.block

    def __ge__(self, other):
        return self.block >= other.block

    # align to pixel boundaries
    def align(self):
        # this extra +0.1 and using points instead of width/height is
        # to help minimize rounding errors
        x0 = int(self.x+0.1)
        y0 = int(self.y+0.1)
        x1 = int(self.x+self.width+0.1)
        y1 = int(self.y+self.height+0.1)
        self.x = x0
        self.y = y0
        self.width = x1 - x0
        self.height = y1 - y0

    # generate a label
    @ft.cached_property
    def label(self):
        if self.type == 'mdir':
            return '%s %s %s w%s\ncksum %08x' % (
                    self.type,
                    self.value.mid.mbidrepr(),
                    self.value.addr(),
                    self.value.weight,
                    self.value.cksum)
        elif self.type == 'btree':
            return '%s %s w%s\ncksum %08x' % (
                    self.type,
                    self.value.addr(),
                    self.value.weight,
                    self.value.cksum)
        elif self.type == 'data':
            return '%s %s %s\ncksize %s\ncksum %08x' % (
                    self.type,
                    '0x%x.%x' % (self.block, self.value.off),
                    self.value.size,
                    self.value.cksize,
                    self.value.cksum)
        elif self.type != 'unused':
            return '%s\n%s' % (
                    self.type,
                    '0x%x' % self.block)
        else:
            return ''

    # generate attrs for punescaping
    @ft.cached_property
    def attrs(self):
        if self.type == 'mdir':
            return {
                'block': self.block,
                'type': self.type,
                'addr': self.value.addr(),
                'trunk': self.value.trunk,
                'weight': self.value.weight,
                'cksum': self.value.cksum,
                'usage': len(self.usage),
            }
        elif self.type == 'btree':
            return {
                'block': self.block,
                'type': self.type,
                'addr': self.value.addr(),
                'trunk': self.value.trunk,
                'weight': self.value.weight,
                'cksum': self.value.cksum,
                'usage': len(self.usage),
            }
        elif self.type == 'data':
            return {
                'block': self.block,
                'type': self.type,
                'addr': self.value.addr(),
                'off': self.value.off,
                'size': self.value.size,
                'cksize': self.value.cksize,
                'cksum': self.value.cksum,
                'usage': len(self.usage),
            }
        else:
            return {
                'block': self.block,
                'type': self.type,
                'usage': len(self.usage),
            }


# a mergable range set type
class RangeSet:
    def __init__(self, ranges=None):
        self._ranges = []

        if ranges is not None:
            # using add here makes sure all ranges are merged/sorted
            # correctly
            for r in ranges:
                self.add(r)

    def __repr__(self):
        return 'RangeSet(%r)' % self._ranges

    def __contains__(self, k):
        i = bisect.bisect(self._ranges, k,
                key=lambda r: r.start) - 1
        if i > -1:
            return k in self._ranges[i]
        else:
            return False

    def __bool__(self):
        return bool(self._ranges)

    def ranges(self):
        yield from self._ranges

    def __iter__(self):
        for r in self._ranges:
            yield from r

    def add(self, r):
        assert isinstance(r, range)
        # trivial range?
        if not r:
            return

        # find earliest possible merge point
        ranges = self._ranges
        i = bisect.bisect_left(ranges, r.start,
                key=lambda r: r.stop)

        # copy ranges < merge
        merged = ranges[:i]

        # merge ranges and append
        while i < len(ranges) and ranges[i].start <= r.stop:
            r = range(
                    min(ranges[i].start, r.start),
                    max(ranges[i].stop, r.stop))
            i += 1
        merged.append(r)

        # copy ranges > merge
        merged.extend(ranges[i:])

        self._ranges = merged

    def remove(self, r):
        assert isinstance(r, range)
        # trivial range?
        if not r:
            return

        # find earliest possible carve point
        ranges = self._ranges
        i = bisect.bisect_left(ranges, r.start,
                key=lambda r: r.stop)

        # copy ranges < carve
        carved = ranges[:i]

        # carve overlapping ranges, note this can split ranges
        while i < len(ranges) and ranges[i].start <= r.stop:
            if ranges[i].start < r.start:
                carved.append(range(ranges[i].start, r.start))
            if ranges[i].stop > r.stop:
                carved.append(range(r.stop, ranges[i].stop))
            i += 1

        # copy ranges > carve 
        carved.extend(ranges[i:])

        self._ranges = carved

    @property
    def start(self):
        if not self._ranges:
            return 0
        else:
            return self._ranges[0].start

    @property
    def stop(self):
        if not self._ranges:
            return 0
        else:
            return self._ranges[-1].stop

    def __len__(self):
        return self.stop

    def copy(self):
        # create a shallow copy
        ranges = RangeSet()
        ranges._ranges = self._ranges.copy()
        return ranges

    def __getitem__(self, slice_):
        assert isinstance(slice_, slice)

        # create a copy
        ranges = self.copy()

        # just use remove to do the carving, it's good enough probably
        if slice_.stop is not None:
            ranges.remove(range(slice_.stop, len(self)))
        if slice_.start is not None:
            ranges.remove(range(0, slice_.start))
            ranges._ranges = [range(
                        r.start - slice_.start,
                        r.stop - slice_.start)
                    for r in ranges._ranges]

        return ranges

    def __ior__(self, other):
        for r in other.ranges():
            self.add(r)
        return self

    def __or__(self, other):
        ranges = self.copy()
        ranges |= other
        return ranges

# a mergable range dict type
class RangeDict:
    def __init__(self, ranges=None):
        self._ranges = []

        if ranges is not None:
            # using __setitem__ here makes sure all ranges are
            # merged/sorted correctly
            for r, v in ranges:
                self[r] = v

    def __repr__(self):
        return 'RangeDict(%r)' % self._ranges

    def _get(self, k):
        i = bisect.bisect(self._ranges, k,
                key=lambda rv: rv[0].start) - 1
        if i > -1:
            return self._ranges[i][1]
        else:
            raise KeyError(k)

    def get(self, k, d=None):
        try:
            return self._get(k)
        except KeyError:
            return d

    def __getitem__(self, k):
        # special case for slicing
        if isinstance(k, slice):
            return self._slice(k)
        else:
            return self._get(k)

    def __contains__(self, k):
        try:
            self._get(k)
            return True
        except KeyError:
            return False

    def __bool__(self):
        return bool(self._ranges)

    def ranges(self):
        yield from self._ranges

    def __iter__(self):
        for r, v in self._ranges:
            for k in r:
                yield k, v

    # apply a function to a range
    def map(self, r, f):
        assert isinstance(r, range)
        # trivial range?
        if not r:
            return

        # find earliest possible merge point
        ranges = self._ranges
        i = bisect.bisect_left(ranges, r.start,
                key=lambda rv: rv[0].stop)

        # copy ranges < merge
        merged = ranges[:i]

        # map, merge/carve ranges
        while i < len(ranges) and ranges[i][0].start <= r.stop:
            # carve prefix
            if ranges[i][0].start < r.start:
                merged.append((
                        range(ranges[i][0].start, r.start),
                        ranges[i][1]))

            # need fill?
            if ranges[i][0].start > r.start:
                v = f(None)
                # merge?
                if (merged
                        and merged[-1][0].stop >= r.start
                        and merged[-1][1] == v):
                    merged[-1] = (
                            range(
                                merged[-1][0].start,
                                ranges[i][0].start),
                            merged[-1][1])
                else:
                    merged.append((
                            range(r.start, ranges[i][0].start),
                            f(None)))

            # apply f
            v = f(ranges[i][1])

            # merge?
            if (merged
                    and merged[-1][0].stop >= r.start
                    and merged[-1][1] == v):
                merged[-1] = (
                        range(
                            merged[-1][0].start,
                            min(ranges[i][0].stop, r.stop)),
                        merged[-1][1])
            else:
                merged.append((
                        range(
                            r.start,
                            min(ranges[i][0].stop, r.stop)),
                        v))
            r = range(
                    min(ranges[i][0].stop, r.stop),
                    r.stop)

            # carve suffix
            if ranges[i][0].stop > r.stop:
                # merge?
                if ranges[i][1] == v:
                    merged[-1] = (
                        range(
                            merged[-1][0].start,
                            ranges[i][0].stop),
                        merged[-1][1])
                else:
                    merged.append((
                            range(r.stop, ranges[i][0].stop),
                            ranges[i][1]))

            i += 1

        # need fill?
        if not merged or merged[-1][0].stop < r.stop:
            v = f(None)
            # merge?
            if (merged
                    and merged[-1][0].stop >= r.start
                    and merged[-1][1] == v):
                merged[-1] = (
                        range(merged[-1][0].start, r.stop),
                        merged[-1][1])
            else:
                merged.append((r, v))

        # copy ranges > merge
        merged.extend(ranges[i:])

        self._ranges = merged

    def __setitem__(self, r, v):
        # we can define __setitem__ using map
        assert isinstance(r, range)
        self.map(r, lambda _: v)

    def __delitem__(self, r):
        # __delitem__ is a bit more complicated
        assert isinstance(r, range)
        # trivial range?
        if not r:
            return

        # find earliest possible carve point
        ranges = self._ranges
        i = bisect.bisect_left(ranges, r.start,
                key=lambda rv: rv[0].stop)

        # copy ranges < carve
        carved = ranges[:i]

        # carve overlapping ranges, note this can split ranges
        while i < len(ranges) and ranges[i][0].start <= r.stop:
            if ranges[i][0].start < r.start:
                carved.append((
                        range(ranges[i][0].start, r.start),
                        ranges[i][1]))
            if ranges[i][0].stop > r.stop:
                carved.append((
                        range(r.stop, ranges[i][0].stop),
                        ranges[i][1]))
            i += 1

        # copy ranges > carve 
        carved.extend(ranges[i:])

        self._ranges = carved

    @property
    def start(self):
        if not self._ranges:
            return 0
        else:
            return self._ranges[0][0].start

    @property
    def stop(self):
        if not self._ranges:
            return 0
        else:
            return self._ranges[-1][0].stop

    def __len__(self):
        return self.stop

    def copy(self):
        # create a shallow copy
        ranges = RangeDict()
        ranges._ranges = self._ranges.copy()
        return ranges

    def _slice(self, slice_):
        assert isinstance(slice_, slice)

        # create a copy
        ranges = RangeDict()
        ranges._ranges = self._ranges

        # just use __delitem__ to do the carving, it's good enough probably
        if slice_.stop is not None:
            del ranges[range(slice_.stop, len(self))]
        if slice_.start is not None:
            del ranges[range(0, slice_.start)]
            ranges._ranges = [(
                        range(
                            r.start - slice_.start,
                            r.stop - slice_.start),
                        v)
                    for r, v in ranges._ranges]

        return ranges

    def __ior__(self, other):
        for r, v in other.ranges():
            self[r] = v
        return self

    def __or__(self, other):
        ranges = self.copy()
        ranges |= other
        return ranges


## a representation of a range of blocks/offs to show
#class BmapSlice:
#    def __init__(self, blocks=None, off=None, size=None):
#        self.orig_blocks = blocks
#        self.orig_off = off
#        self.orig_size = size
#
#        # flatten blocks, default to all blocks
#        blocks = ([blocks] if isinstance(blocks, slice)
#                else list(it.chain.from_iterable(
#                    [blocks_] if isinstance(blocks_, slice) else blocks_
#                    for blocks_ in blocks))
#                if blocks is not None
#                else [slice(None)])
#
#        # blocks may also encode offsets
#        blocks, offs, size = (
#                [block[0] if isinstance(block, tuple)
#                        else block
#                    for block in blocks],
#                [off.start if isinstance(off, slice)
#                        else off if off is not None
#                        else size.start if isinstance(size, slice)
#                        else block[1] if isinstance(block, tuple)
#                        else None
#                    for block in blocks],
#                (size.stop - (size.start or 0)
#                        if size.stop is not None
#                        else None) if isinstance(size, slice)
#                    else size if size is not None
#                    else ((off.stop - (off.start or 0))
#                        if off.stop is not None
#                        else None) if isinstance(off, slice)
#                    else None)
#
#        self.blocks = blocks
#        self.offs = offs
#        self.size = size
#
#    @property
#    def off(self):
#        if not self.offs:
#            return None
#        else:
#            return self.offs[-1]
#
#    def __repr__(self):
#        return 'BmapSlice(%r, %r, %r)' % (
#                self.orig_blocks,
#                self.orig_off,
#                self.orig_size)
#
#    def slices(self, block_size, block_count):
#        for block, off in zip(self.blocks, self.offs):
#            # figure out off range, bound to block_size
#            off = off if off is not None else 0
#            size = self.size if self.size is not None else block_size - off
#            if off >= block_size:
#                continue
#            size = min(off + size, block_size) - off
#
#            # flatten/filter blocks
#            if isinstance(block, slice):
#                start = block.start if block.start is not None else 0
#                stop = block.stop if block.stop is not None else block_count
#                for block_ in range(start, min(stop, block_count)):
#                    yield block_, off, size
#            else:
#                block = block if block is not None else 0
#                if block < block_count:
#                    yield block, off, size
#
#    def blocks(self, block_count):
#        for block in self.blocks:
#            # flatten/filter blocks
#            if isinstance(block, slice):
#                start = block.start if block.start is not None else 0
#                stop = block.stop if block.stop is not None else block_count
#                for block_ in range(start, min(stop, block_count)):
#                    yield block_
#            else:
#                block = block if block is not None else 0
#                if block < block_count:
#                    yield block
#
#    def sorted_slices(self, block_size, block_count):
#        # merge block ranges
#        blocks = RangeDict()
#        for block in self.blocks:
#            # figure out off range, bound to block_size
#            off = off if off is not None else 0
#            size = self.size if self.size is not None else block_size - off
#            if off >= block_size:
#                continue
#            size = min(off + size, block_size) - off
#
#            # flatten/filter blocks
#            if isinstance(block, slice):
#                start = block.start if block.start is not None else 0
#                stop = block.stop if block.stop is not None else block_count
#                blocks.map(range(start, min(stop, block_count)),
#                        lambda r: (r if r is not None else RangeSet())
#                            | RangeSet([range(off, off+size)]))
#            else:
#                block = block if block is not None else 0
#                if block < block_count:
#                    blocks.map(range(block, block+1),
#                            lambda r: (r if r is not None else RangeSet())
#                                | RangeSet([range(off, off+size)]))
#
#        for block, r in blocks:
#            for off, stop in r:
#                yield off, stop-off
#
#    def sorted_blocks(self, block_count):
#        # merge block ranges
#        blocks = RangeSet()
#        for block in self.blocks:
#            # flatten/filter blocks
#            if isinstance(block, slice):
#                start = block.start if block.start is not None else 0
#                stop = block.stop if block.stop is not None else block_count
#                blocks.add(range(start, min(stop, block_count)))
#            else:
#                block = block if block is not None else 0
#                if block < block_count:
#                    blocks.add(range(block, block+1))
#
#        yield from blocks
#
#
#
##        # merge block ranges
##        merged = []
##        for blocks, offs in zip(self.blocks, self.offs):
##            # simplify blocks first
##            if not isinstance(blocks, slice):
##                blocks = slice(blocks, blocks+1)
##            blocks = slice(
##                    blocks.start if blocks.start is not None else 0,
##                    blocks.start if blocks.start is not None else block_count)
##
##            # copy ranges before merge
##            i = 0
##            merged_ = []
##            blocks_ = blocks
##            while i < len(merged) and merged[i].stop < blocks_.start:
##                merged_.append(merged[i])
##
##            # merge ranges and append
##            while i < len(merged) and merged[i].start <= blocks_.stop:
##                blocks_ = slice(
##                        min(merged[i].start, blocks_.start),
##                        max(merged[i].stop, blocks_.stop))
##            merged_.append(blocks_)
##
##            # and copy the rest
##            merged_.extend(merged[i:])
##
##                
##
##
##
##            for blocks_ in merged:
##                # merge ranges?
##                if blocks_
##                else
#
#
#    def __contains__(self, block):
#        if isinstance(block, tuple):
#            block, off = block
#        else:
#            block, off = block, None
#
#        for block_, off_ in zip(self.blocks, self.offs):
#            # in off range?
#            if off is not None:
#                off_ = off_ if off_ is not None else 0
#                size_ = self.size
#                if not (off >= off_
#                        and (size_ is None or off < off_ + size_)):
#                    continue
#
#            # in block range?
#            if isinstance(block_, slice):
#                if ((block_.start is None or block >= block_.start)
#                        and (block_.stop is None or block < block_.stop)):
#                    return True
#            else:
#                if block_ is None or block == block_:
#                    return True
#
#        return False
        


def main(disk, output, mroots=None, *,
        trunk=None,
        block_size=None,
        block_count=None,
        blocks=None,
        no_ckmeta=False,
        no_ckdata=False,
        mtree_only=False,
        quiet=False,
        labels=[],
        colors=[],
        width=None,
        height=None,
        block_cols=None,
        block_rows=None,
        block_ratio=None,
        no_header=False,
        no_mode=False,
        hilbert=False,
        lebesgue=False,
        no_javascript=False,
        mode_tree=False,
        mode_branches=False,
        mode_references=False,
        mode_redund=False,
        to_scale=None,
        aspect_ratio=(1,1),
        tiny=False,
        title=None,
        padding=None,
        no_label=False,
        dark=False,
        font=FONT,
        font_size=FONT_SIZE,
        background=None,
        **args):
    # tiny mode?
    if tiny:
        if block_ratio is None:
            block_ratio = 1
        if to_scale is None:
            to_scale = 1
        if padding is None:
            padding = 0
        no_header = True
        no_label = True
        no_javascript = True

    if block_ratio is None:
        # golden ratio
        block_ratio = 1 / ((1 + mt.sqrt(5))/2)

    if padding is None:
        padding = 1

    # default to all modes
    if (not mode_tree
            and not mode_branches
            and not mode_references
            and not mode_redund):
        mode_tree = True
        mode_branches = True
        mode_references = True
        mode_redund = True

    # what colors/labels to use?
    colors_ = Attr(colors, defaults=COLORS_DARK if dark else COLORS)

    labels_ = Attr(labels)

    if background is not None:
        background_ = background
    elif dark:
        background_ = '#000000'
    else:
        background_ = '#ffffff'

    # figure out width/height
    if width is not None:
        width_ = width
    else:
        width_ = WIDTH

    if height is not None:
        height_ = height
    else:
        height_ = HEIGHT

    # is bd geometry specified?
    if isinstance(block_size, tuple):
        block_size, block_count_ = block_size
        if block_count is None:
            block_count = block_count_

    # flatten mroots, default to 0x{0,1}
    mroots = list(it.chain.from_iterable(mroots)) if mroots else [0, 1]

    # mroots may also encode trunks
    mroots, trunk = (
            [block[0] if isinstance(block, tuple)
                    else block
                for block in mroots],
            trunk if trunk is not None
                else ft.reduce(
                    lambda x, y: y,
                    (block[1] for block in mroots
                        if isinstance(block, tuple)),
                    None))

    # we seek around a bunch, so just keep the disk open
    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # fetch the filesystem
        bd = Bd(f, block_size, block_count)
        lfs = Lfs.fetch(bd, mroots, trunk)
        corrupted = not bool(lfs)

        # if we can't figure out the block_count, guess
        if block_count is None:
            if lfs.config.geometry is not None:
                block_count = lfs.config.geometry.block_count
            else:
                f.seek(0, os.SEEK_END)
                block_count = mt.ceil(f.tell() / block_size)

        # flatten blocks, default to all blocks
        blocks = list(
                range(blocks.start or 0, blocks.stop or block_count)
                        if isinstance(blocks, slice)
                        else range(blocks, blocks+1)
                    if blocks
                    else range(block_count))

        # traverse the filesystem and create a block map
        bmap = {b: BmapBlock(b, 'unused') for b in blocks}
        for child, path in lfs.traverse(
                mtree_only=mtree_only,
                path=True):
            # track each block in our window
            for b in child.blocks:
                if b not in bmap:
                    continue

                # mdir?
                if isinstance(child, Mdir):
                    type = 'mdir'
                    if b in child.blocks[:1+child.redund]:
                        usage = range(child.eoff)
                    else:
                        usage = range(0)

                # btree node?
                elif isinstance(child, Rbyd):
                    type = 'btree'
                    if b in child.blocks[:1+child.redund]:
                        usage = range(child.eoff)
                    else:
                        usage = range(0)

                # bptr?
                elif isinstance(child, Bptr):
                    type = 'data'
                    usage = range(child.off, child.off+child.size)

                else:
                    assert False, "%r?" % b

                # check for some common issues

                # block conflict?
                if b in bmap and bmap[b].type != 'unused':
                    if bmap[b].type == 'conflict':
                        bmap[b].value.append(child)
                    else:
                        bmap[b] = BmapBlock(b, 'conflict',
                                [bmap[b].value, child],
                                range(block_size))
                    corrupted = True

                # corrupt metadata?
                elif (not no_ckmeta
                        and isinstance(child, (Mdir, Rbyd))
                        and not child):
                    bmap[b] = BmapBlock(b, 'corrupt', child, range(block_size))
                    corrupted = True

                # corrupt data?
                elif (not no_ckdata
                        and isinstance(child, Bptr)
                        and not child):
                    bmap[b] = BmapBlock(b, 'corrupt', child, range(block_size))
                    corrupted = True

                # normal block
                else:
                    bmap[b] = BmapBlock(b, type, child, usage)

                # keep track of siblings
                bmap[b].siblings.update(
                        b_ for b_ in child.blocks
                            if b_ != b and b_ in bmap)

            # update parents with children
            if path:
                parent = path[-1][1]
                for b in parent.blocks:
                    if b in bmap:
                        bmap[b].children.update(
                                b_ for b_ in child.blocks
                                    if b_ in bmap)

    # scale width/height if requested
    if (to_scale is not None
            and (width is None or height is None)):
        # scale width only
        if height is not None:
            width_ = mt.ceil((len(bmap) * to_scale) / height_)
        # scale height only
        elif width is not None:
            height_ = mt.ceil((len(bmap) * to_scale) / width_)
        # scale based on aspect-ratio
        else:
            width_ = mt.ceil(mt.sqrt(len(bmap) * to_scale)
                    * (aspect_ratio[0] / aspect_ratio[1]))
            height_ = mt.ceil((len(bmap) * to_scale) / width_)

    # create space for header
    x__ = 0
    y__ = 0
    width__ = width_
    height__ = height_
    if not no_header:
        y__ += mt.ceil(FONT_SIZE * 1.3)
        height__ -= min(mt.ceil(FONT_SIZE * 1.3), height__)

    # figure out block_cols/block_rows
    if block_cols is not None and block_rows is not None:
        pass
    elif block_rows is not None:
        block_cols = mt.ceil(len(bmap) / block_rows)
    elif block_cols is not None:
        block_rows = mt.ceil(len(bmap) / block_cols)
    else:
        # divide by 2 until we hit our target ratio, this works
        # well for things that are often powers-of-two
        block_cols = 1
        block_rows = len(bmap)
        while (abs(((width__/(block_cols * 2))
                        / (height__/mt.ceil(block_rows / 2)))
                    - block_ratio)
                < abs(((width__/block_cols)
                        / (height__/block_rows)))
                    - block_ratio):
            block_cols *= 2
            block_rows = mt.ceil(block_rows / 2)

    block_width = width__ / block_cols
    block_height = height__ / block_rows

    # assign block locations based on block_rows/block_cols and the
    # requested space filling curve
    for (x, y), b in zip(
            (hilbert_curve if hilbert
                else lebesgue_curve if lebesgue
                else naive_curve)(block_cols, block_rows),
            sorted(bmap.values())):
        b.x = x__ + (x * block_width)
        b.y = y__ + (y * block_height)
        b.width = block_width
        b.height = block_height

        # apply top padding
        if x == 0:
            b.x += padding
            b.width -= min(padding, b.width)
        if y == 0:
            b.y += padding
            b.height -= min(padding, b.height)
        # apply bottom padding
        b.width  -= min(padding, b.width)
        b.height -= min(padding, b.height)

        # align to pixel boundaries
        b.align()

        # bump up to at least one pixel for every block
        b.width = max(b.width, 1)
        b.height = max(b.height, 1)

    # assign colors based on block type
    for b in bmap.values():
        color__ = colors_[b.block, (b.type, '0x%x' % b.block)]
        if color__ is not None:
            if '%' in color__:
                color__ = punescape(color__, b.attrs)
            b.color = color__

    # assign labels
    for b in bmap.values():
        label__ = labels_[b.block, (b.type, '0x%x' % b.block)]
        if label__ is not None:
            if '%' in label__:
                label__ = punescape(label__, b.attrs)
            b.label = label__


    # create svg file
    with openio(output, 'w') as f:
        def writeln(s=''):
            f.write(s)
            f.write('\n')
        f.writeln = writeln

        # yes this is svg
        f.write('<svg '
                'xmlns="http://www.w3.org/2000/svg" '
                'viewBox="0,0,%(width)d,%(height)d" '
                'width="%(width)d" '
                'height="%(height)d" '
                'style="max-width: 100%%; '
                        'height: auto; '
                        'font: %(font_size)dpx %(font)s; '
                        'background-color: %(background)s; '
                        'user-select: %(user_select)s;">' % dict(
                    width=width_,
                    height=height_,
                    font=','.join(font),
                    font_size=font_size,
                    background=background_,
                    user_select='none' if not no_javascript else 'auto'))

        # create header
        if not no_header:
            f.write('<g '
                    'id="header" '
                    '%(js)s>' % dict(
                        js= 'cursor="pointer" '
                            'onclick="click_header(this,event)">'
                                if not no_javascript else ''))
            # add an invisible rect to make things more clickable
            f.write('<rect '
                    'x="%(x)d" '
                    'y="%(y)d" '
                    'width="%(width)d" '
                    'height="%(height)d" '
                    'opacity="0">' % dict(
                        x=0,
                        y=0,
                        width=width_,
                        height=y__))
            f.write('</rect>')
            f.write('<text fill="%(color)s">' % dict(
                    color='#ffffff' if dark else '#000000'))
            f.write('<tspan x="3" y="1.1em">')
            if title:
                f.write(punescape(title, {
                    'magic': 'littlefs%s' % (
                        '' if lfs.ckmagic() else '?'),
                    'version': 'v%s.%s' % (
                        lfs.version.major
                            if lfs.version is not None else '?',
                        lfs.version.minor
                            if lfs.version is not None else '?'),
                    'version_major': lfs.version.major
                            if lfs.version is not None else '?',
                    'version_minor': lfs.version.minor
                            if lfs.version is not None else '?',
                    'geometry': '%sx%s' % (
                        lfs.block_size
                            if lfs.block_size is not None else '?',
                        lfs.block_count
                            if lfs.block_count is not None else '?'),
                    'block_size': lfs.block_size
                            if lfs.block_size is not None else '?',
                    'block_count': lfs.block_count
                            if lfs.block_count is not None else '?',
                    'addr': lfs.addr(),
                    'weight': 'w%s.%s' % (
                        lfs.mbweightrepr(),
                        lfs.mrweightrepr()),
                    'mbweight': lfs.mbweightrepr(),
                    'mrweight': lfs.mrweightrepr(),
                    'cksum': '%08x%s' % (
                        lfs.cksum,
                        '' if lfs.ckcksum() else '?'),
                }))
            else:
                f.write('littlefs%s v%s.%s %sx%s %s w%s.%s, cksum %08x%s' % (
                        '' if lfs.ckmagic() else '?',
                        lfs.version.major if lfs.version is not None else '?',
                        lfs.version.minor if lfs.version is not None else '?',
                        lfs.block_size if lfs.block_size is not None else '?',
                        lfs.block_count if lfs.block_count is not None else '?',
                        lfs.addr(),
                        lfs.mbweightrepr(), lfs.mrweightrepr(),
                        lfs.cksum,
                        '' if lfs.ckcksum() else '?'))
            f.write('</tspan>')
            if not no_mode and not no_javascript:
                f.write('<tspan id="mode" x="%(x)d" y="1.1em" '
                        'text-anchor="end">' % dict(
                            x=width_-3))
                f.write('mode: %s' % (
                        'tree' if mode_tree
                            else 'branches' if mode_branches
                            else 'references' if mode_references
                            else 'redund'))
                f.write('</tspan>')
            f.write('</text>')
            f.write('</g>')

        # create block tiles
        for b in bmap.values():
            # skip anything with zero weight/height after aligning things
            if b.width == 0 or b.height == 0:
                continue

            f.write('<g '
                    'id="b-%(block)d" '
                    'class="block %(type)s" '
                    'transform="translate(%(x)d,%(y)d)" '
                    '%(js)s>' % dict(
                        block=b.block,
                        type=b.type,
                        x=b.x,
                        y=b.y,
                        js= 'data-block="%(block)d" '
                            # precompute x/y for javascript, svg makes this
                            # weirdly difficult to figure out post-transform
                            'data-x="%(x)d" '
                            'data-y="%(y)d" '
                            'data-width="%(width)d" '
                            'data-height="%(height)d" '
                            'onmouseenter="enter_block(this,event)" '
                            'onmouseleave="leave_block(this,event)" '
                            'onclick="click_block(this,event)">' % dict(
                                    block=b.block,
                                    x=b.x,
                                    y=b.y,
                                    width=b.width,
                                    height=b.height)
                                if not no_javascript else ''))
            # add an invisible rect to make things more clickable
            f.write('<rect '
                    'width="%(width)d" '
                    'height="%(height)d" '
                    'opacity="0">' % dict(
                        width=b.width + padding,
                        height=b.height + padding))
            f.write('</rect>')
            f.write('<title>')
            f.write(b.label)
            f.write('</title>')
            f.write('<rect '
                    'id="b-tile-%(block)d" '
                    'fill="%(color)s" '
                    'width="%(width)d" '
                    'height="%(height)d">' % dict(
                        block=b.block,
                        color=b.color,
                        width=b.width,
                        height=b.height))
            f.write('</rect>')
            if not no_label:
                f.write('<clipPath id="b-clip-%d">' % b.block)
                f.write('<use href="#b-tile-%d">' % b.block)
                f.write('</use>')
                f.write('</clipPath>')
                f.write('<text clip-path="url(#b-clip-%d)">' % b.block)
                for j, l in enumerate(b.label.split('\n')):
                    if j == 0:
                        f.write('<tspan x="3" y="1.1em">')
                        f.write(l)
                        f.write('</tspan>')
                    else:
                        f.write('<tspan x="3" dy="1.1em" '
                                'fill-opacity="0.7">')
                        f.write(l)
                        f.write('</tspan>')
                f.write('</text>')
            f.write('</g>')

        if not no_javascript:
            # arrowhead for arrows
            f.write('<defs>')
            f.write('<marker '
                    'id="arrowhead" '
                    'viewBox="0 0 10 10" '
                    'refX="10" '
                    'refY="5" '
                    'markerWidth="6" '
                    'markerHeight="6" '
                    'orient="auto-start-reverse" '
                    'fill="%(color)s">' % dict(
                        color='#000000' if dark else '#555555'))
            f.write('<path d="M 0 0 L 10 5 L 0 10 z"/>')
            f.write('</marker>')
            f.write('</defs>')

            # javascript for arrows
            #
            # why tf does svg support javascript?
            f.write('<script><![CDATA[')

            # embed our siblings
            f.write('const siblings = %s;' % json.dumps(
                    {b.block: list(b.siblings)
                        for b in bmap.values()
                        if b.siblings},
                    separators=(',', ':')))

            # embed our children
            f.write('const children = %s;' % json.dumps(
                    {b.block: list(b.children)
                        for b in bmap.values()
                        if b.children},
                    separators=(',', ':')))

            # function for rect <-> line interesection
            f.write('function rect_intersect(x, y, width, height, l_x, l_y) {')
            f.write(    'let r_x = (x + width/2);')
            f.write(    'let r_y = (y + height/2);')
            f.write(    'let dx = l_x - r_x;')
            f.write(    'let dy = l_y - r_y;')
            f.write(    'let θ = Math.abs(dy / dx);')
            f.write(    'let φ = height / width;')
            f.write(    'if (θ > φ) {')
            f.write(        'return [')
            f.write(            'r_x + ((height/2)/θ)*Math.sign(dx),')
            f.write(            'r_y + (height/2)*Math.sign(dy),')
            f.write(        '];')
            f.write(    '} else {')
            f.write(        'return [')
            f.write(            'r_x + (width/2)*Math.sign(dx),')
            f.write(            'r_y + ((width/2)*θ)*Math.sign(dy),')
            f.write(        '];')
            f.write(    '}')
            f.write('}')

            # our main drawing functions
            f.write('function draw_unfocus() {')
                        # lower opacity of unfocused tiles
            f.write(    'for (let b of document.querySelectorAll('
                                '".block:not(.unused)")) {')
            f.write(        'b.setAttribute("fill-opacity", 0.5);')
            f.write(    '}')
            f.write('}')

            f.write('function draw_focus(a) {')
                        # revert opacity and move to top
            f.write(    'a.setAttribute("fill-opacity", 1);')
            f.write(    'a.parentElement.appendChild(a);')
            f.write('}')

            # draw an arrow
            f.write('function draw_arrow(a, b) {')
                        # no self-referential arrows
            f.write(    'if (b == a) {')
            f.write(        'return;')
            f.write(    '}')
                        # figure out rect intersections
            f.write(    'let svg = document.documentElement;')
            f.write(    'let ns = svg.getAttribute("xmlns");')
            f.write(    'let a_x = parseInt(a.dataset.x);')
            f.write(    'let a_y = parseInt(a.dataset.y);')
            f.write(    'let a_width = parseInt(a.dataset.width);')
            f.write(    'let a_height = parseInt(a.dataset.height);')
            f.write(    'let b_x = parseInt(b.dataset.x);')
            f.write(    'let b_y = parseInt(b.dataset.y);')
            f.write(    'let b_width = parseInt(b.dataset.width);')
            f.write(    'let b_height = parseInt(b.dataset.height);')
            f.write(    'let [a_ix, a_iy] = rect_intersect(')
            f.write(            'a_x, a_y, a_width, a_height,')
            f.write(            'b_x + b_width/2, b_y + b_height/2);')
            f.write(    'let [b_ix, b_iy] = rect_intersect(')
            f.write(            'b_x, b_y, b_width, b_height,')
            f.write(            'a_x + a_width/2, a_y + a_height/2);')
                        # create the actual arrow
            f.write(    'let arrow = document.createElementNS(ns, "line");')
            f.write(    'arrow.classList.add("arrow");')
            f.write(    'arrow.setAttribute("x1", a_ix);')
            f.write(    'arrow.setAttribute("y1", a_iy);')
            f.write(    'arrow.setAttribute("x2", b_ix);')
            f.write(    'arrow.setAttribute("y2", b_iy);')
            f.write(    'arrow.setAttribute("stroke", "%(color)s");' % dict(
                                color='#000000' if dark else '#555555'))
            f.write(    'arrow.setAttribute("marker-end", "url(#arrowhead)");')
            f.write(    'arrow.setAttribute("pointer-events", "none");')
            f.write(    'a.parentElement.appendChild(arrow);')
            f.write('}')

            # draw a dashed line
            f.write('function draw_dashed(a, b) {')
                        # no self-referential arrows
            f.write(    'if (b == a) {')
            f.write(        'return;')
            f.write(    '}')
                        # figure out rect intersections
            f.write(    'let svg = document.documentElement;')
            f.write(    'let ns = svg.getAttribute("xmlns");')
            f.write(    'let a_x = parseInt(a.dataset.x);')
            f.write(    'let a_y = parseInt(a.dataset.y);')
            f.write(    'let a_width = parseInt(a.dataset.width);')
            f.write(    'let a_height = parseInt(a.dataset.height);')
            f.write(    'let b_x = parseInt(b.dataset.x);')
            f.write(    'let b_y = parseInt(b.dataset.y);')
            f.write(    'let b_width = parseInt(b.dataset.width);')
            f.write(    'let b_height = parseInt(b.dataset.height);')
            f.write(    'let [a_ix, a_iy] = rect_intersect(')
            f.write(            'a_x, a_y, a_width, a_height,')
            f.write(            'b_x + b_width/2, b_y + b_height/2);')
            f.write(    'let [b_ix, b_iy] = rect_intersect(')
            f.write(            'b_x, b_y, b_width, b_height,')
            f.write(            'a_x + a_width/2, a_y + a_height/2);')
                        # create the actual arrow
            f.write(    'let arrow = document.createElementNS(ns, "line");')
            f.write(    'arrow.classList.add("arrow");')
            f.write(    'arrow.setAttribute("x1", a_ix);')
            f.write(    'arrow.setAttribute("y1", a_iy);')
            f.write(    'arrow.setAttribute("x2", b_ix);')
            f.write(    'arrow.setAttribute("y2", b_iy);')
            f.write(    'arrow.setAttribute("stroke", "%(color)s");' % dict(
                                color='#000000' if dark else '#555555'))
            f.write(    'arrow.setAttribute("stroke-dasharray", "5,5");')
            f.write(    'arrow.setAttribute("pointer-events", "none");')
            f.write(    'a.parentElement.appendChild(arrow);')
            f.write('}')

            # some other draw helpers

            # connect siblings with dashed lines
            f.write('function draw_sibling_arrows(a, seen) {')
            f.write(    'if (a.dataset.block in seen) {')
            f.write(        'return;')
            f.write(    '}')
            f.write(    'seen[a.dataset.block] = true;')
            f.write(    'for (let sibling of '
                                'siblings[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById('
                                    '"b-"+sibling);')
            f.write(        'if (b) {')
            f.write(            'draw_dashed(a, b);')
            f.write(            'seen[b.dataset.block] = true;')
            f.write(        '}')
            f.write(    '}')
            f.write('}')

            # focus siblings
            f.write('function draw_sibling_focus(a) {')
            f.write(    'for (let sibling of '
                                'siblings[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById('
                                    '"b-"+sibling);')
            f.write(        'if (b) {')
            f.write(            'draw_focus(b);')
            f.write(        '}')
            f.write(    '}')
            f.write('}')

            # here are some drawing modes to choose from

            # draw full tree
            f.write('function draw_tree(a) {')
                        # track visited children to avoid cycles
            f.write(    'let seen = {};')
                        # avoid duplicate sibling arrows
            f.write(    'let seen_siblings = {};')
                        # create new arrows
            f.write(    'let recurse = function(a) {')
            f.write(        'if (a.dataset.block in seen) {')
            f.write(            'return;')
            f.write(        '}')
            f.write(        'seen[a.dataset.block] = true;')
            f.write(        'for (let child of ')
            f.write(                'children[a.dataset.block] || []) {')
            f.write(            'let b = document.getElementById("b-"+child);')
            f.write(            'if (b) {')
            f.write(                'draw_arrow(a, b);')
            f.write(                'recurse(b);')
            f.write(            '}')
            f.write(        '}')
                            # also connect siblings
            f.write(        'draw_sibling_arrows(a, seen_siblings);')
                            # and siblings to their children
            f.write(        'for (let sibling of '
                                    'siblings[a.dataset.block] || []) {')
            f.write(            'let b = document.getElementById('
                                        '"b-"+sibling);')
            f.write(            'if (b) {')
            f.write(                'recurse(b);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '};')
            f.write(    'recurse(a);')
                        # track visited children to avoid cycles
            f.write(    'seen = {};')
                        # move in-focus tiles to the top
            f.write(    'recurse = function(a) {')
            f.write(        'if (a.dataset.block in seen) {')
            f.write(            'return;')
            f.write(        '}')
            f.write(        'seen[a.dataset.block] = true;')
            f.write(        'for (let child of ')
            f.write(                'children[a.dataset.block] || []) {')
            f.write(            'let b = document.getElementById("b-"+child);')
            f.write(            'if (b) {')
            f.write(                'draw_focus(b);')
            f.write(                'recurse(b);')
            f.write(            '}')
            f.write(        '}')
                            # also connect siblings
            f.write(        'draw_sibling_focus(a);')
                            # and siblings to their children
            f.write(        'for (let sibling of '
                                    'siblings[a.dataset.block] || []) {')
            f.write(            'let b = document.getElementById('
                                        '"b-"+sibling);')
            f.write(            'if (b) {')
            f.write(                'recurse(b);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '};')
            f.write(    'recurse(a);')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # draw one level of branches
            f.write('function draw_branches(a) {')
                        # avoid duplicate sibling arrows
            f.write(    'let seen_siblings = {};')
                        # create new arrows
            f.write(    'for (let child of children[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById("b-"+child);')
            f.write(        'if (b) {')
            f.write(            'draw_arrow(a, b);')
                                # also connect siblings
            f.write(            'draw_sibling_arrows(b, seen_siblings);')
            f.write(        '}')
            f.write(    '}')
                        # also connect siblings
            f.write(    'draw_sibling_arrows(a, seen_siblings);')
                        # and siblings to their children
            f.write(    'for (let sibling of '
                                'siblings[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById("b-"+sibling);')
            f.write(        'if (b) {')
            f.write(            'for (let child of '
                                        'children[b.dataset.block] || []) {')
            f.write(                'let c = document.getElementById('
                                            '"b-"+child);')
            f.write(                'if (c) {')
            f.write(                    'draw_arrow(b, c);')
            f.write(                '}')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # move in-focus tiles to the top
            f.write(    'for (let child of children[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById("b-"+child);')
            f.write(        'if (b) {')
            f.write(            'draw_focus(b);')
                                # also connect siblings
            f.write(            'draw_sibling_focus(b);')
            f.write(        '}')
            f.write(    '}')
                        # also connect siblings
            f.write(    'draw_sibling_focus(a);')
                        # and siblings to their children
            f.write(    'for (let sibling of '
                                'siblings[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById("b-"+sibling);')
            f.write(        'if (b) {')
            f.write(            'for (let child of '
                                        'children[b.dataset.block] || []) {')
            f.write(                'let c = document.getElementById('
                                            '"b-"+child);')
            f.write(                'if (c) {')
            f.write(                    'draw_focus(c);')
            f.write(                '}')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # draw one level of references
            f.write('function draw_references(a) {')
                        # avoid duplicate sibling arrows
            f.write(    'let seen_siblings = {};')
                        # create new arrows
            f.write(    'for (let parent in children) {')
            f.write(        'if ((children[parent] || []).includes(')
            f.write(                'parseInt(a.dataset.block))) {')
            f.write(            'let b = document.getElementById('
                                        '"b-"+parent);')
            f.write(            'if (b) {')
            f.write(                'draw_arrow(b, a);')
                                    # also connect parent siblings
            f.write(                'draw_sibling_arrows(b, seen_siblings);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # connect siblings
            f.write(    'draw_sibling_arrows(a, seen_siblings);')
                        # and siblings to their parents
            f.write(    'for (let sibling of '
                                'siblings[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById("b-"+sibling);')
            f.write(        'if (b) {')
            f.write(            'for (let parent in children) {')
            f.write(                'if ((children[parent] || []).includes(')
            f.write(                        'parseInt(b.dataset.block))) {')
            f.write(                    'let c = document.getElementById('
                                                '"b-"+parent);')
            f.write(                    'if (c) {')
            f.write(                        'draw_arrow(c, b);')
            f.write(                    '}')
            f.write(                '}')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # move in-focus tiles to the top
            f.write(    'for (let parent in children) {')
            f.write(        'if ((children[parent] || []).includes(')
            f.write(                'parseInt(a.dataset.block))) {')
            f.write(            'let b = document.getElementById('
                                        '"b-"+parent);')
            f.write(            'if (b) {')
            f.write(                'draw_focus(b);')
                                    # also connect parent siblings
            f.write(                'draw_sibling_focus(b);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # connect siblings
            f.write(    'draw_sibling_focus(a);')
                        # and siblings to their parents
            f.write(    'for (let sibling of '
                                'siblings[a.dataset.block] || []) {')
            f.write(        'let b = document.getElementById("b-"+sibling);')
            f.write(        'if (b) {')
            f.write(            'for (let parent in children) {')
            f.write(                'if ((children[parent] || []).includes(')
            f.write(                        'parseInt(b.dataset.block))) {')
            f.write(                    'let c = document.getElementById('
                                                '"b-"+parent);')
            f.write(                    'if (c) {')
            f.write(                        'draw_focus(c, b);')
            f.write(                    '}')
            f.write(                '}')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # draw related redund blocks
            f.write('function draw_redund(a) {')
                        # avoid duplicate sibling arrows
            f.write(    'let seen_siblings = {};')
                        # connect siblings
            f.write(    'draw_sibling_arrows(a, seen_siblings);')
                        # move in-focus tiles to the top
            f.write(    'draw_sibling_focus(a);')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # clear old arrows/tiles if we leave
            f.write('function undraw() {')
                        # clear arrows
            f.write(    'for (let arrow of document.querySelectorAll('
                                '".arrow")) {')
            f.write(        'arrow.remove();')
            f.write(    '}')
                        # revert opacity
            f.write(    'for (let b of document.querySelectorAll('
                                '".block:not(.unused)")) {')
            f.write(        'b.setAttribute("fill-opacity", 1);')
            f.write(    '}')
            f.write('}')

            # state machine for mouseover/clicks
            f.write('const modes = [')
            if mode_tree:
                f.write('{name: "tree",         draw: draw_tree         },')
            if mode_branches:
                f.write('{name: "branches",     draw: draw_branches     },')
            if mode_references:
                f.write('{name: "references",   draw: draw_references   },')
            if mode_redund:
                f.write('{name: "redund",       draw: draw_redund       },')
            f.write('];')
            f.write('let state = 0;')
            f.write('let hovered = null;')
            f.write('let active = null;')
            f.write('let paused = false;')

            f.write('function enter_block(a, event) {')
            f.write(    'hovered = a;')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')

            f.write(    'if (!active) {')
                            # reset
            f.write(        'undraw();')
            f.write(        'draw_unfocus();')
                            # draw selected mode
            f.write(        'modes[state].draw(a);')
            f.write(    '}')
            f.write('}')

            f.write('function leave_block(a, event) {')
            f.write(    'hovered = null;')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')

                        # do nothing if ctrl is held
            f.write(    'if (!active) {')
                            # reset
            f.write(        'undraw();')
            f.write(    '}')
            f.write('}')

            # update the mode string
            f.write('function draw_mode() {')
            f.write(    'let mode = document.getElementById("mode");')
            f.write(    'if (mode) {')
            f.write(        'mode.textContent = "mode: "'
                                    '+ modes[state].name'
                                    '+ ((paused) ? " (paused)"'
                                        ': active ? " (frozen)"'
                                        ': "");')
            f.write(    '}')
            f.write('}')

            # redraw things
            f.write('function redraw() {')
                        # reset
            f.write(    'undraw();')
                        # redraw block if active
            f.write(    'if (active) {')
            f.write(        'draw_unfocus();')
            f.write(        'modes[state].draw(active);')
                        # otherwise try to enter hovered block if there is one
            f.write(    '} else if (hovered) {')
            f.write(        'enter_block(hovered);')
            f.write(    '}')
            f.write('}')

            # clicking the mode element changes the mode
            f.write('function click_header(a, event) {')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')
                        # update state
            f.write(    'state = (state + 1) % modes.length;')
                        # update the mode string
            f.write(    'draw_mode();')
                        # redraw with new mode
            f.write(    'redraw();')
            f.write('}')

            # click handler is kinda complicated, we handle both single
            # and double clicks here
            f.write('let prev = null;')
            f.write('function click_block(a, event) {')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')

                        # double clicking changes the mode
            f.write(    'if (event && event.detail == 2 '
                                # limit this to double-clicking the active tile
                                '&& (!prev || a == prev)) {')
                            # undo single-click
            f.write(        'active = prev;')
                            # trigger a mode change
            f.write(        'click_header();')
            f.write(        'return;')
            f.write(    '}')
                        # save state in case we are trying to double click,
                        # double clicks always send a single click first
            f.write(    'prev = active;')

                        # clicking blocks toggles frozen mode
            f.write(    'if (a == active) {')
            f.write(        'active = null;')
            f.write(    '} else {')
            f.write(        'active = null;')
            f.write(        'enter_block(a);')
            f.write(        'active = a;')
            f.write(    '}')
                        # update mode string
            f.write(    'draw_mode();')
            f.write('}')

            # include some minor keybindings
            f.write('function keydown(event) {')
                        # m => change mode
            f.write(    'if (event.key == "m") {')
            f.write(        'click_header();')
                        # escape/e => clear frozen/paused state
            f.write(    '} else if (event.key == "Escape"'
                                '|| event.key == "e") {')
                            # reset frozen state
            f.write(        'active = null;')
                            # reset paused state
            f.write(        'if (paused) {')
            f.write(            'keydown({key: "Pause"});')
            f.write(        '}')
                            # redraw things
            f.write(        'draw_mode();')
            f.write(        'redraw();')
                        # pause/p  => pause all interactivity and allow
                        # copy-paste
            f.write(    '} else if (event.key == "Pause"'
                                '|| event.key == "p") {')
            f.write(        'paused = !paused;')
                            # update mode string
            f.write(        'draw_mode();')
            f.write(        'if (paused) {')
                                # enabled copy-pasting when paused
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[style*=\\"user-select\\"]")) {')
            f.write(                'e.style["user-select"] = "auto";')
            f.write(            '}')
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[cursor]")) {')
            f.write(                'e.setAttribute("cursor", "auto");')
            f.write(            '}')
            f.write(        '} else {')
                                # reset copy-pasting
            f.write(            'document.getSelection().empty();')
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[style*=\\"user-select\\"]")) {')
            f.write(                'e.style["user-select"] = "none";')
            f.write(            '}')
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[cursor]")) {')
            f.write(                'e.setAttribute("cursor", "pointer");')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
            f.write('}')
            f.write('window.addEventListener("keydown", keydown);')

            f.write(']]></script>')

        f.write('</svg>')


    # print some summary info
    if not quiet:
        print('updated %s, '
                'littlefs%s v%s.%s %sx%s %s w%s.%s, '
                'cksum %08x%s' % (
                    output,
                    '' if lfs.ckmagic() else '?',
                    lfs.version.major if lfs.version is not None else '?',
                    lfs.version.minor if lfs.version is not None else '?',
                    lfs.block_size if lfs.block_size is not None else '?',
                    lfs.block_count if lfs.block_count is not None else '?',
                    lfs.addr(),
                    lfs.mbweightrepr(), lfs.mrweightrepr(),
                    lfs.cksum,
                    '' if lfs.ckcksum() else '?'))

    if args.get('error_on_corrupt') and corrupted:
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Render currently used blocks in a littlefs image "
                "as an interactive d3-esque map.",
            allow_abbrev=False)
    parser.add_argument(
            'disk',
            help="File containing the block device.")
    parser.add_argument(
            'mroots',
            nargs='*',
            type=rbydaddr,
            help="Block address of the mroots. Defaults to 0x{0,1}.")
    parser.add_argument(
            '-o', '--output',
            required=True,
            help="Output *.svg file.")
    parser.add_argument(
            '--trunk',
            type=lambda x: int(x, 0),
            help="Use this offset as the trunk of the mroots.")
    parser.add_argument(
            '-b', '--block-size',
            type=bdgeom,
            help="Block size/geometry in bytes. Accepts <size>x<count>.")
    parser.add_argument(
            '--block-count',
            type=lambda x: int(x, 0),
            help="Block count in blocks.")

#    # subparser for block arguments
#    blocks_parser = argparse.ArgumentParser(
#            prog="%s -@/--blocks" % parser.prog,
#            allow_abbrev=False)
#    blocks_parser.add_argument(
#            'blocks',
#            nargs='*',
#            type=lambda x: (
#                slice(*(int(x, 0) if x.strip() else None
#                        for x in x.split(',', 1)))
#                    if ',' in x and '{' not in x
#                    else rbydaddr(x)),
#            help="Block addresses, may be a range.")
#    blocks_parser.add_argument(
#            '--off',
#            type=lambda x: (
#                slice(*(int(x, 0) if x.strip() else None
#                        for x in x.split(',', 1)))
#                    if ',' in x
#                    else int(x, 0)),
#            help="Show a specific offset, may be a range.")
#    blocks_parser.add_argument(
#            '-n', '--size',
#            type=lambda x: (
#                slice(*(int(x, 0) if x.strip() else None
#                        for x in x.split(',', 1)))
#                    if ',' in x
#                    else int(x, 0)),
#            help="Show this many bytes, may be a range.")
#
#    parser.add_argument(
#            '-@', '--blocks',
#            type=lambda blocks: BmapSlice(**{k: v
#                for k, v in vars(blocks_parser.parse_intermixed_args(
#                    shlex.split(blocks))).items()
#                if v is not None}),
#            help="Optional blocks to show, may be a range. Can also include "
#                "--off and -n/--size flags to indicate a range inside the "
#                "block, both which may also be ranges.")

    parser.add_argument(
            '-@', '--blocks',
            type=lambda x: (
                slice(*(int(x, 0) if x.strip() else None
                        for x in x.split(',', 1)))
                    if ',' in x
                    else int(x, 0)),
            help="Show a specific block, may be a range.")
    parser.add_argument(
            '--no-ckmeta',
            action='store_true',
            help="Don't check metadata blocks for errors.")
    parser.add_argument(
            '--no-ckdata',
            action='store_true',
            help="Don't check metadata + data blocks for errors.")
    parser.add_argument(
            '--mtree-only',
            action='store_true',
            help="Only traverse the mtree.")
    parser.add_argument(
            '-q', '--quiet',
            action='store_true',
            help="Don't print info.")

    parser.add_argument(
            '-L', '--add-label',
            dest='labels',
            action='append',
            type=lambda x: (
                lambda ks, v: (
                    tuple(k.strip() for k in ks.split(',')),
                    v.strip())
                )(*x.split('=', 1))
                    if '=' in x else x.strip(),
            help="Add a label to use. Can be assigned to a specific "
                "block type/block. Accepts %% modifiers.")
    parser.add_argument(
            '-C', '--add-color',
            dest='colors',
            action='append',
            type=lambda x: (
                lambda ks, v: (
                    tuple(k.strip() for k in ks.split(',')),
                    v.strip())
                )(*x.split('=', 1))
                    if '=' in x else x.strip(),
            help="Add a color to use. Can be assigned to a specific "
                "block type/block. Accepts %% modifiers.")
    parser.add_argument(
            '-W', '--width',
            type=lambda x: int(x, 0),
            help="Width in pixels. Defaults to %r." % WIDTH)
    parser.add_argument(
            '-H', '--height',
            type=lambda x: int(x, 0),
            help="Height in pixels. Defaults to %r." % HEIGHT)
    parser.add_argument(
            '-X', '--block-cols',
            type=lambda x: int(x, 0),
            help="Number of blocks on the x-axis. Guesses from --block-count "
                "and --block-ratio by default.")
    parser.add_argument(
            '-Y', '--block-rows',
            type=lambda x: int(x, 0),
            help="Number of blocks on the y-axis. Guesses from --block-count "
                "and --block-ratio by default.")
    parser.add_argument(
            '--block-ratio',
            dest='block_ratio',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            help="Target ratio for block sizes. Defaults to the golden ratio.")
    parser.add_argument(
            '--no-header',
            action='store_true',
            help="Don't show the header.")
    parser.add_argument(
            '--no-mode',
            action='store_true',
            help="Don't show the mode state.")
    parser.add_argument(
            '-U', '--hilbert',
            action='store_true',
            help="Render as a space-filling Hilbert curve.")
    parser.add_argument(
            '-Z', '--lebesgue',
            action='store_true',
            help="Render as a space-filling Z-curve.")
    parser.add_argument(
            '-J', '--no-javascript',
            action='store_true',
            help="Don't add javascript for interactability.")
    parser.add_argument(
            '--mode-tree',
            action='store_true',
            help="Include the tree rendering mode.")
    parser.add_argument(
            '--mode-branches',
            action='store_true',
            help="Include the branches rendering mode.")
    parser.add_argument(
            '--mode-references',
            action='store_true',
            help="Include the references rendering mode.")
    parser.add_argument(
            '--mode-redund',
            action='store_true',
            help="Include the redund rendering mode.")
    parser.add_argument(
            '--to-scale',
            nargs='?',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            const=1,
            help="Scale the resulting treemap such that 1 pixel ~= 1/scale "
                "blocks. Defaults to scale=1. ")
    parser.add_argument(
            '-R', '--aspect-ratio',
            type=lambda x: (
                tuple(float(v) for v in x.split(':', 1))
                    if ':' in x else (float(x), 1)),
            help="Aspect ratio to use with --to-scale. Defaults to 1:1.")
    parser.add_argument(
            '-t', '--tiny',
            action='store_true',
            help="Tiny mode, alias for --block-ratio=1, --to-scale=1, "
                "--padding=0, --no-header, --no-label, and --no-javascript.")
    parser.add_argument(
            '--title',
            help="Add a title. Accepts %% modifiers.")
    parser.add_argument(
            '--padding',
            type=float,
            help="Padding to add to each block. Defaults to 1.")
    parser.add_argument(
            '--no-label',
            action='store_true',
            help="Don't render any labels.")
    parser.add_argument(
            '--dark',
            action='store_true',
            help="Use the dark style.")
    parser.add_argument(
            '--font',
            type=lambda x: [x.strip() for x in x.split(',')],
            help="Font family to use.")
    parser.add_argument(
            '--font-size',
            help="Font size to use. Defaults to %r." % FONT_SIZE)
    parser.add_argument(
            '--background',
            help="Background color to use. Note #00000000 can make the "
                "background transparent.")
    parser.add_argument(
            '-e', '--error-on-corrupt',
            action='store_true',
            help="Error if the filesystem is corrupt.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
