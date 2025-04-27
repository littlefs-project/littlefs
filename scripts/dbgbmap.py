#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import bisect
import collections as co
import fnmatch
import functools as ft
import io
import itertools as it
import math as mt
import os
import re
import shlex
import shutil
import struct
import time

try:
    import inotify_simple
except ModuleNotFoundError:
    inotify_simple = None

try:
    import crc32c as crc32c_lib
except ModuleNotFoundError:
    crc32c_lib = None


TAG_NULL        = 0x0000    ## 0x0000  v--- ---- ---- ----
TAG_CONFIG      = 0x0000    ## 0x00tt  v--- ---- -ttt tttt
TAG_MAGIC       = 0x0031    #  0x003r  v--- ---- --11 --rr
TAG_VERSION     = 0x0034    #  0x0034  v--- ---- --11 -1--
TAG_RCOMPAT     = 0x0035    #  0x0035  v--- ---- --11 -1-1
TAG_WCOMPAT     = 0x0036    #  0x0036  v--- ---- --11 -11-
TAG_OCOMPAT     = 0x0037    #  0x0037  v--- ---- --11 -111
TAG_GEOMETRY    = 0x0038    #  0x0038  v--- ---- --11 1---
TAG_FILELIMIT   = 0x0039    #  0x0039  v--- ---- --11 1--1
TAG_NAMELIMIT   = 0x003a    #  0x003a  v--- ---- --11 1-1-
TAG_GDELTA      = 0x0100    ## 0x01tt  v--- ---1 -ttt ttrr
TAG_GRMDELTA    = 0x0100    #  0x0100  v--- ---1 ---- ----
TAG_NAME        = 0x0200    ## 0x02tt  v--- --1- -ttt tttt
TAG_REG         = 0x0201    #  0x0201  v--- --1- ---- ---1
TAG_DIR         = 0x0202    #  0x0202  v--- --1- ---- --1-
TAG_STICKYNOTE  = 0x0203    #  0x0203  v--- --1- ---- --11
TAG_BOOKMARK    = 0x0204    #  0x0204  v--- --1- ---- -1--
TAG_STRUCT      = 0x0300    ## 0x03tt  v--- --11 -ttt ttrr
TAG_BRANCH      = 0x0300    #  0x030r  v--- --11 ---- --rr
TAG_DATA        = 0x0304    #  0x0304  v--- --11 ---- -1--
TAG_BLOCK       = 0x0308    #  0x0308  v--- --11 ---- 1err
TAG_DID         = 0x0314    #  0x0314  v--- --11 ---1 -1--
TAG_BSHRUB      = 0x0318    #  0x0318  v--- --11 ---1 1---
TAG_BTREE       = 0x031c    #  0x031c  v--- --11 ---1 11rr
TAG_MROOT       = 0x0321    #  0x032r  v--- --11 --1- --rr
TAG_MDIR        = 0x0325    #  0x0324  v--- --11 --1- -1rr
TAG_MTREE       = 0x032c    #  0x032c  v--- --11 --1- 11rr
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


# assign chars/colors to specific filesystem objects
CHARS = {
    'mdir':     'm',
    'btree':    'b',
    'data':     'd',
    'corrupt':  '!',
    'conflict': '!',
    'unused':   '-',
}
COLORS = {
    'mdir':     '33',
    'btree':    '34',
    'data':     '32',
    'corrupt':  '31',
    'conflict': '30;41',
    'unused':   '1;30',
}

# give more interesting objects a higher priority
Z_ORDER = ['corrupt', 'conflict', 'mdir', 'btree', 'data', 'unused']

CHARS_DOTS = " .':"
CHARS_BRAILLE = (
        '⠀⢀⡀⣀⠠⢠⡠⣠⠄⢄⡄⣄⠤⢤⡤⣤' '⠐⢐⡐⣐⠰⢰⡰⣰⠔⢔⡔⣔⠴⢴⡴⣴'
        '⠂⢂⡂⣂⠢⢢⡢⣢⠆⢆⡆⣆⠦⢦⡦⣦' '⠒⢒⡒⣒⠲⢲⡲⣲⠖⢖⡖⣖⠶⢶⡶⣶'
        '⠈⢈⡈⣈⠨⢨⡨⣨⠌⢌⡌⣌⠬⢬⡬⣬' '⠘⢘⡘⣘⠸⢸⡸⣸⠜⢜⡜⣜⠼⢼⡼⣼'
        '⠊⢊⡊⣊⠪⢪⡪⣪⠎⢎⡎⣎⠮⢮⡮⣮' '⠚⢚⡚⣚⠺⢺⡺⣺⠞⢞⡞⣞⠾⢾⡾⣾'
        '⠁⢁⡁⣁⠡⢡⡡⣡⠅⢅⡅⣅⠥⢥⡥⣥' '⠑⢑⡑⣑⠱⢱⡱⣱⠕⢕⡕⣕⠵⢵⡵⣵'
        '⠃⢃⡃⣃⠣⢣⡣⣣⠇⢇⡇⣇⠧⢧⡧⣧' '⠓⢓⡓⣓⠳⢳⡳⣳⠗⢗⡗⣗⠷⢷⡷⣷'
        '⠉⢉⡉⣉⠩⢩⡩⣩⠍⢍⡍⣍⠭⢭⡭⣭' '⠙⢙⡙⣙⠹⢹⡹⣹⠝⢝⡝⣝⠽⢽⡽⣽'
        '⠋⢋⡋⣋⠫⢫⡫⣫⠏⢏⡏⣏⠯⢯⡯⣯' '⠛⢛⡛⣛⠻⢻⡻⣻⠟⢟⡟⣟⠿⢿⡿⣿')


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
    if crc32c_lib is not None:
        return crc32c_lib.crc32c(data, crc)
    else:
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

def fromle32(data, j=0):
    return struct.unpack('<I', data[j:j+4].ljust(4, b'\0'))[0]

def fromleb128(data, j=0):
    word = 0
    d = 0
    while j+d < len(data):
        b = data[j+d]
        word |= (b & 0x7f) << 7*d
        word &= 0xffffffff
        if not b & 0x80:
            return word, d+1
        d += 1
    return word, d

def fromtag(data, j=0):
    d = 0
    tag = struct.unpack('>H', data[j:j+2].ljust(2, b'\0'))[0]; d += 2
    weight, d_ = fromleb128(data, j+d); d += d_
    size, d_ = fromleb128(data, j+d); d += d_
    return tag>>15, tag&0x7fff, weight, size, d

def frombranch(data, j=0):
    d = 0
    block, d_ = fromleb128(data, j+d); d += d_
    trunk, d_ = fromleb128(data, j+d); d += d_
    cksum = fromle32(data, j+d); d += 4
    return block, trunk, cksum, d

def frombtree(data, j=0):
    d = 0
    w, d_ = fromleb128(data, j+d); d += d_
    block, trunk, cksum, d_ = frombranch(data, j+d); d += d_
    return w, block, trunk, cksum, d

def frommdir(data, j=0):
    blocks = []
    d = 0
    while j+d < len(data):
        block, d_ = fromleb128(data, j+d)
        blocks.append(block)
        d += d_
    return tuple(blocks), d

def fromshrub(data, j=0):
    d = 0
    weight, d_ = fromleb128(data, j+d); d += d_
    trunk, d_ = fromleb128(data, j+d); d += d_
    return weight, trunk, d

def frombptr(data, j=0):
    d = 0
    size, d_ = fromleb128(data, j+d); d += d_
    block, d_ = fromleb128(data, j+d); d += d_
    off, d_ = fromleb128(data, j+d); d += d_
    cksize, d_ = fromleb128(data, j+d); d += d_
    cksum = fromle32(data, j+d); d += 4
    return size, block, off, cksize, cksum, d

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
                    else 'filelimit' if (tag & 0xfff) == TAG_FILELIMIT
                    else 'namelimit' if (tag & 0xfff) == TAG_NAMELIMIT
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
                    else 'stickynote' if (tag & 0xfff) == TAG_STICKYNOTE
                    else 'bookmark' if (tag & 0xfff) == TAG_BOOKMARK
                    else 'name 0x%02x' % (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    # structure tags
    elif (tag & 0x6f00) == TAG_STRUCT:
        return '%s%s%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                'branch' if (tag & 0xfff) == TAG_BRANCH
                    else 'data' if (tag & 0xfff) == TAG_DATA
                    else 'block' if (tag & 0xfff) == TAG_BLOCK
                    else 'did' if (tag & 0xfff) == TAG_DID
                    else 'bshrub' if (tag & 0xfff) == TAG_BSHRUB
                    else 'btree' if (tag & 0xfff) == TAG_BTREE
                    else 'mroot' if (tag & 0xfff) == TAG_MROOT
                    else 'mdir' if (tag & 0xfff) == TAG_MDIR
                    else 'mtree' if (tag & 0xfff) == TAG_MTREE
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
        rev = fromle32(data, 0)
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
            v, tag, w, size, d = fromtag(data, j_)
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
                    cksum___ = fromle32(data, j_)
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
            _, alt, w, jump, d = fromtag(self.data, j)

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
                            _, nalt, _, _, _ = fromtag(self.data, j+jump+d)
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
                            _, nalt, _, _, _ = fromtag(self.data, j)
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
                or (rattr_.tag & ~mask & 0xfff)
                    != (tag & ~mask & 0xfff)):
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
                        or (rattr_.tag & ~mask & 0xfff)
                            != (tag & ~mask & 0xfff)):
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
                block, trunk, cksum, _ = frombranch(branch_.data)
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
                        path_[:-1]
                            if path_ and path_[-1][1] == rbyd
                            else path_)
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
                block, trunk, cksum, _ = frombranch(branch_.data)
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

    def lookupnext(self, mid, tag=None, *,
            path=False):
        # this is similar to rbyd lookupnext, we just error if
        # lookupnext changes mids
        if not isinstance(mid, Mid):
            mid = Mid(mid, mbits=self.mbits)
        r = self.rbyd.lookupnext(mid.mrid, tag,
                path=path)
        if path:
            rid, rattr, path_ = r
        else:
            rid, rattr = r

        if rid != mid.mrid:
            if path:
                return None, path_
            else:
                return None

        if path:
            return rattr, path_
        else:
            return rattr

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
        return mt.ceil(mt.log2(block_size)) - 3

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
            blocks_, _ = frommdir(rattr_.data)
            mroot = Mdir.fetch(bd, -1, blocks_)
            mrootchain.append(mroot)

        # fetch the actual mtree, if there is one
        mtree = None
        if not depth or len(mrootchain) < depth:
            rattr_ = mroot.lookup(-1, TAG_MTREE, 0x3)
            if rattr_ is not None:
                w_, block_, trunk_, cksum_, _ = frombtree(rattr_.data)
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
                # stop here?
                if depth and len(path_) >= depth:
                    if path:
                        return mroot, path_
                    else:
                        return mroot

                name = mroot.lookup(-1, TAG_MAGIC)
                path_.append((mroot.mid, mroot, name))

        # no mtree? must be inlined in mroot
        if self.mtree is None:
            if mid.mbid != -1:
                if path:
                    return None, path_
                else:
                    return None

            if path:
                return self.mroot, path_
            else:
                return self.mroot

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
            blocks_, _ = frommdir(rattr_.data)
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

    def lookupnext(self, mid, *,
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

        # lookup mid in mdir
        rattr = mdir.lookupnext(mid)
        if path:
            return mdir, rattr, path_+[(mid, mdir, rattr)]
        else:
            return mdir, rattr

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

        # lookup mid in mdir
        rattr = mdir.lookup(mid)
        if path:
            return mdir, rattr, path_+[(mid, mdir, rattr)]
        else:
            return mdir, rattr

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
                # stop here?
                if depth and len(path_) >= depth:
                    return

                name = mroot.lookup(-1, TAG_MAGIC)
                path_.append((mroot.mid, mroot, name))

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
                                path_[:-1]
                                    if path_
                                        and isinstance(path_[-1][1], Rbyd)
                                        and path_[-1][1] == rbyd
                                    else path_)
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
                # stop here?
                if depth and len(path_) >= depth:
                    if path:
                        return mroot, path_
                    else:
                        return mroot

                name = mroot.lookup(-1, TAG_MAGIC)
                path_.append((mroot.mid, mroot, name))

        # no mtree? must be inlined in mroot
        if self.mtree is None:
            if path:
                return self.mroot, path_
            else:
                return self.mroot

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
            blocks_, _ = frommdir(rattr_.data)
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
        mask = 0x3

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
            self.major, d_ = fromleb128(self.data, d); d += d_
            self.minor, d_ = fromleb128(self.data, d); d += d_

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

        def __init__(self, mroot, tag, rattr):
            super().__init__(mroot, tag, rattr)
            d = 0
            block_size, d_ = fromleb128(self.data, d); d += d_
            block_count, d_ = fromleb128(self.data, d); d += d_
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
            count, d_ = fromleb128(self.data, d); d += d_
            rms = []
            if count <= 2:
                for _ in range(count):
                    mid, d_ = fromleb128(self.data, d); d += d_
                    mid = mtree.mid(mid)
                    # map mbids -> -1 if mroot-inlined
                    if mtree.mtree is None:
                        mid = mtree.mid(-1, mid.mrid)
                    rms.append(mid)
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
            depth=None,
            no_ck=False,
            no_ckmroot=False,
            no_ckmagic=False,
            no_ckgcksum=False):
        # Mtree does most of the work here
        mtree = Mtree.fetch(bd, blocks, trunk,
                depth=depth)

        # create lfs object
        lfs = cls(bd, mtree)

        # don't check anything?
        if no_ck:
            return lfs

        # check mroot
        if (not no_ckmroot
                and not lfs.corrupt
                and not lfs.ckmroot()):
            lfs.corrupt = True

        # check magic
        if (not no_ckmagic
                and not lfs.corrupt
                and not lfs.ckmagic()):
            lfs.corrupt = True

        # check gcksum
        if (not no_ckgcksum
                and not lfs.corrupt
                and not lfs.ckgcksum()):
            lfs.corrupt = True

        return lfs

    # check that the mroot is valid
    def ckmroot(self):
        return bool(self.mroot)

    # check that the magic string is littlefs
    def ckmagic(self):
        if self.config.magic is None:
            return False
        return self.config.magic.data == b'littlefs'

    # check that the gcksum checks out
    def ckgcksum(self):
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
                weight, trunk, _ = fromshrub(self.struct.data)
                self.bshrub = Btree.fetchshrub(lfs.bd, mdir.rbyd, trunk)
            elif (self.struct is not None
                    and (self.struct.tag & ~0x3) == TAG_BTREE):
                weight, block, trunk, cksum, _ = frombtree(self.struct.data)
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
                size, block, off, cksize, cksum, _ = frombptr(rattr.data)
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
                                path_[:-1]
                                    if path_ and path_[-1][1] == rbyd
                                    else path_)
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



# keep-open stuff
if inotify_simple is None:
    Inotify = None
else:
    class Inotify(inotify_simple.INotify):
        def __init__(self, paths):
            super().__init__()

            # wait for interesting events
            flags = (inotify_simple.flags.ATTRIB
                    | inotify_simple.flags.CREATE
                    | inotify_simple.flags.DELETE
                    | inotify_simple.flags.DELETE_SELF
                    | inotify_simple.flags.MODIFY
                    | inotify_simple.flags.MOVED_FROM
                    | inotify_simple.flags.MOVED_TO
                    | inotify_simple.flags.MOVE_SELF)

            # recurse into directories
            for path in paths:
                if os.path.isdir(path):
                    for dir, _, files in os.walk(path):
                        self.add_watch(dir, flags)
                        for f in files:
                            self.add_watch(os.path.join(dir, f), flags)
                else:
                    self.add_watch(path, flags)

# a pseudo-stdout ring buffer
class RingIO:
    def __init__(self, maxlen=None, head=False):
        self.maxlen = maxlen
        self.head = head
        self.lines = co.deque(
                maxlen=max(maxlen, 0) if maxlen is not None else None)
        self.tail = io.StringIO()

        # trigger automatic sizing
        self.resize(self.maxlen)

    @property
    def width(self):
        # just fetch this on demand, we don't actually use width
        return shutil.get_terminal_size((80, 5))[0]

    @property
    def height(self):
        # calculate based on terminal height?
        if self.maxlen is None or self.maxlen <= 0:
            return max(
                    shutil.get_terminal_size((80, 5))[1]
                        + (self.maxlen or 0),
                    0)
        # limit to maxlen
        else:
            return self.maxlen

    def resize(self, maxlen):
        self.maxlen = maxlen
        if maxlen is not None and maxlen <= 0:
            maxlen = self.height
        if maxlen != self.lines.maxlen:
            self.lines = co.deque(self.lines, maxlen=maxlen)

    def __len__(self):
        return len(self.lines)

    def write(self, s):
        # note using split here ensures the trailing string has no newline
        lines = s.split('\n')

        if len(lines) > 1 and self.tail.getvalue():
            self.tail.write(lines[0])
            lines[0] = self.tail.getvalue()
            self.tail = io.StringIO()

        self.lines.extend(lines[:-1])

        if lines[-1]:
            self.tail.write(lines[-1])

    # keep track of maximum drawn canvas
    canvas_lines = 1

    def draw(self):
        # did terminal size change?
        self.resize(self.maxlen)

        # copy lines
        lines = self.lines.copy()
        # pad to fill any existing canvas, but truncate to terminal size
        h = shutil.get_terminal_size((80, 5))[1]
        lines.extend('' for _ in range(
                len(lines),
                min(RingIO.canvas_lines, h)))
        while len(lines) > h:
            if self.head:
                lines.pop()
            else:
                lines.popleft()

        # build up the redraw in memory first and render in a single
        # write call, this minimizes flickering caused by the cursor
        # jumping around
        canvas = []

        # hide the cursor
        canvas.append('\x1b[?25l')

        # give ourself a canvas
        while RingIO.canvas_lines < len(lines):
            canvas.append('\n')
            RingIO.canvas_lines += 1

        # write lines from top to bottom so later lines overwrite earlier
        # lines, note xA/xB stop at terminal boundaries
        for i, line in enumerate(lines):
            # move to col 0
            canvas.append('\r')
            # move up to line
            if len(lines)-1-i > 0:
                canvas.append('\x1b[%dA' % (len(lines)-1-i))
            # clear line
            canvas.append('\x1b[K')
            # disable line wrap
            canvas.append('\x1b[?7l')
            # print the line
            canvas.append(line)
            # enable line wrap
            canvas.append('\x1b[?7h') # enable line wrap
            # move back down
            if len(lines)-1-i > 0:
                canvas.append('\x1b[%dB' % (len(lines)-1-i))

        # show the cursor again
        canvas.append('\x1b[?25h')

        # write to stdout and flush
        sys.stdout.write(''.join(canvas))
        sys.stdout.flush()

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
                if not isinstance(key, tuple):
                    key = (key,)
            else:
                i, key = 0, key
        elif isinstance(key, str):
            i, key = 0, (key,)
        else:
            i, key = key, ()

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

        raise KeyError(i, key)

    def get(self, key, default=None):
        try:
            return self.__getitem__(key)
        except KeyError:
            return default

    def __contains__(self, key):
        try:
            self.__getitem__(key)
            return True
        except KeyError:
            return False

    # get all results for a given key
    def getall(self, key, default=None):
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
            return best[1]

        # fallback to defaults?
        if self.defaults is not None:
            return self.defaults.getall(key, default)

        raise default

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
#
# attrs can override __getitem__ for lazy attr generation
def punescape(s, attrs=None):
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
            if attrs is not None:
                try:
                    v = attrs[m.group('field')]
                except KeyError:
                    return m.group()
            else:
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

# split %-escaped strings into chars
def psplit(s):
    pattern = re.compile(
        '%[%n]'
            '|' '%x..'
            '|' '%u....'
            '|' '%U........'
            '|' '%\((?P<field>[^)]*)\)'
                '(?P<format>[+\- #0-9\.]*[sdboxXfFeEgG])')
    return [m.group() for m in re.finditer(pattern.pattern + '|.', s)]


# a little ascii renderer
class Canvas:
    def __init__(self, width, height, *,
            color=False,
            dots=False,
            braille=False):
        # scale if we're printing with dots or braille
        if braille:
            xscale, yscale = 2, 4
        elif dots:
            xscale, yscale = 1, 2
        else:
            xscale, yscale = 1, 1

        self.width_ = width
        self.height_ = height
        self.width = xscale*width
        self.height = yscale*height
        self.xscale = xscale
        self.yscale = yscale
        self.color_ = color
        self.dots = dots
        self.braille = braille

        # create initial canvas
        self.chars = [0] * (width*height)
        self.colors = [''] * (width*height)

    def char(self, x, y, char=None):
        # ignore out of bounds
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return False

        x_ = x // self.xscale
        y_ = y // self.yscale
        if char is not None:
            c = self.chars[x_ + y_*self.width_]
            # mask in sub-char pixel?
            if isinstance(char, bool):
                if not isinstance(c, int):
                    c = 0
                self.chars[x_ + y_*self.width_] = (c
                        | (1
                            << ((y%self.yscale)*self.xscale
                                + (self.xscale-1)-(x%self.xscale))))
            else:
                self.chars[x_ + y_*self.width_] = char
        else:
            c = self.chars[x_ + y_*self.width_]
            if isinstance(c, int):
                return ((c
                            >> ((y%self.yscale)*self.xscale
                                + (self.xscale-1)-(x%self.xscale)))
                        & 1) == 1
            else:
                return c

    def color(self, x, y, color=None):
        # ignore out of bounds
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return ''

        x_ = x // self.xscale
        y_ = y // self.yscale
        if color is not None:
            self.colors[x_ + y_*self.width_] = color
        else:
            return self.colors[x_ + y_*self.width_]

    def __getitem__(self, xy):
        x, y = xy
        return self.char(x, y)

    def __setitem__(self, xy, char):
        x, y = xy
        self.char(x, y, char)

    def point(self, x, y, *,
            char=True,
            color=''):
        self.char(x, y, char)
        self.color(x, y, color)

    def line(self, x1, y1, x2, y2, *,
            char=True,
            color=''):
        # incremental error line algorithm
        ex = abs(x2 - x1)
        ey = -abs(y2 - y1)
        dx = +1 if x1 < x2 else -1
        dy = +1 if y1 < y2 else -1
        e = ex + ey

        while True:
            self.point(x1, y1, char=char, color=color)
            e2 = 2*e

            if x1 == x2 and y1 == y2:
                break

            if e2 > ey:
                e += ey
                x1 += dx

            if x1 == x2 and y1 == y2:
                break

            if e2 < ex:
                e += ex
                y1 += dy

        self.point(x2, y2, char=char, color=color)

    def rect(self, x, y, w, h, *,
            char=True,
            color=''):
        for j in range(h):
            for i in range(w):
                self.point(x+i, y+j, char=char, color=color)

    def label(self, x, y, label, width=None, height=None, *,
            color=''):
        x_ = x
        y_ = y
        for char in label:
            if char == '\n':
                x_ = x
                y_ -= self.yscale
            else:
                if ((width is None or x_ < x+width)
                        and (height is None or y_ > y-height)):
                    self.point(x_, y_, char=char, color=color)
                x_ += self.xscale

    def draw(self, row):
        y_ = self.height_-1 - row
        row_ = []
        for x_ in range(self.width_):
            # char?
            c = self.chars[x_ + y_*self.width_]
            if isinstance(c, int):
                if self.braille:
                    assert c < 256
                    c = CHARS_BRAILLE[c]
                elif self.dots:
                    assert c < 4
                    c = CHARS_DOTS[c]
                else:
                    assert c < 2
                    c = '.' if c else ' '

            # color?
            if self.color_:
                color = self.colors[x_ + y_*self.width_]
                if color:
                    c = '\x1b[%sm%s\x1b[m' % (color, c)

            row_.append(c)

        return ''.join(row_)


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


def main_(ring, disk, mroots=None, *,
        trunk=None,
        block_size=None,
        block_count=None,
        blocks=None,
        no_ckmeta=False,
        no_ckdata=False,
        mtree_only=False,
        chars=[],
        colors=[],
        color='auto',
        dots=False,
        braille=False,
        width=None,
        height=None,
        block_cols=None,
        block_rows=None,
        block_ratio=None,
        no_header=False,
        hilbert=False,
        lebesgue=False,
        contiguous=False,
        to_scale=None,
        to_ratio=1/1,
        tiny=False,
        title=None,
        title_littlefs=False,
        title_usage=False,
        **args):
    # give ring an writeln function
    def writeln(s=''):
        ring.write(s)
        ring.write('\n')
    ring.writeln = writeln

    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # tiny mode?
    if tiny:
        if block_ratio is None:
            block_ratio = 1
        if to_scale is None:
            to_scale = 1
        no_header = True

    if block_ratio is None:
        # try to align block_ratio to chars, even in braille/dots
        # mode (we can't color sub-chars)
        if braille or dots:
            block_ratio = 1/2
        else:
            block_ratio = 1

    # what chars/colors/labels to use?
    chars_ = []
    for char in chars:
        if isinstance(char, tuple):
            chars_.extend((char[0], c) for c in psplit(char[1]))
        else:
            chars_.extend(psplit(char))
    chars_ = Attr(chars_, defaults=[True] if braille or dots else CHARS)

    colors_ = Attr(colors, defaults=COLORS)

    # figure out width/height
    if width is None:
        width_ = min(80, shutil.get_terminal_size((80, 5))[0])
    elif width > 0:
        width_ = width
    else:
        width_ = max(0, shutil.get_terminal_size((80, 5))[0] + width)

    if height is None:
        height_ = 2 if not no_header else 1
    elif height > 0:
        height_ = height
    else:
        height_ = max(0, shutil.get_terminal_size((80, 5))[1] + height)

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
        lfs = Lfs.fetch(bd, mroots, trunk,
                # don't bother to check things if we're not reporting errors
                no_ck=not args.get('error_on_corrupt'))
        corrupted = not bool(lfs)

        # if we can't figure out the block_count, guess
        block_size_ = block_size
        block_count_ = block_count
        if block_count is None:
            if lfs.config.geometry is not None:
                block_count_ = lfs.config.geometry.block_count
            else:
                f.seek(0, os.SEEK_END)
                block_count_ = mt.ceil(f.tell() / block_size)

        # flatten blocks, default to all blocks
        blocks_ = list(
                range(blocks.start or 0, blocks.stop or block_count_)
                        if isinstance(blocks, slice)
                        else range(blocks, blocks+1)
                    if blocks
                    else range(block_count_))

        # traverse the filesystem and create a block map
        bmap = {b: BmapBlock(b, 'unused') for b in blocks_}
        mdir_count = 0
        btree_count = 0
        data_count = 0
        total_count = 0
        for child in lfs.traverse(
                mtree_only=mtree_only):
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
                    mdir_count += 1
                    total_count += 1

                # btree node?
                elif isinstance(child, Rbyd):
                    type = 'btree'
                    if b in child.blocks[:1+child.redund]:
                        usage = range(child.eoff)
                    else:
                        usage = range(0)
                    btree_count += 1
                    total_count += 1

                # bptr?
                elif isinstance(child, Bptr):
                    type = 'data'
                    usage = range(child.off, child.off+child.size)
                    data_count += 1
                    total_count += 1

                else:
                    assert False, "%r?" % b

                # check for some common issues

                # block conflict?
                #
                # note we can't compare more than types due to different
                # trunks, slicing, etc
                if (b in bmap
                        and bmap[b].type != 'unused'
                        and bmap[b].type != type):
                    if bmap[b].type == 'conflict':
                        bmap[b].value.append(child)
                    else:
                        bmap[b] = BmapBlock(b, 'conflict',
                                [bmap[b].value, child],
                                range(block_size_))
                    corrupted = True

                # corrupt metadata?
                elif (not no_ckmeta
                        and isinstance(child, (Mdir, Rbyd))
                        and not child):
                    bmap[b] = BmapBlock(b, 'corrupt', child, range(block_size_))
                    corrupted = True

                # corrupt data?
                elif (not no_ckdata
                        and isinstance(child, Bptr)
                        and not child):
                    bmap[b] = BmapBlock(b, 'corrupt', child, range(block_size_))
                    corrupted = True

                # normal block
                else:
                    bmap[b] = BmapBlock(b, type, child, usage)

        # one last thing, build a title
        if title:
            title_ = punescape(title, {
                'magic': 'littlefs%s' % (
                    '' if lfs.ckmagic() else '?'),
                'version': 'v%s.%s' % (
                    lfs.version.major if lfs.version is not None else '?',
                    lfs.version.minor if lfs.version is not None else '?'),
                'version_major':
                    lfs.version.major if lfs.version is not None else '?',
                'version_minor':
                    lfs.version.minor if lfs.version is not None else '?',
                'geometry': '%sx%s' % (
                    lfs.block_size if lfs.block_size is not None else '?',
                    lfs.block_count if lfs.block_count is not None else '?'),
                'block_size':
                    lfs.block_size if lfs.block_size is not None else '?',
                'block_count':
                    lfs.block_count if lfs.block_count is not None else '?',
                'addr': lfs.addr(),
                'weight': 'w%s.%s' % (lfs.mbweightrepr(), lfs.mrweightrepr()),
                'mbweight': lfs.mbweightrepr(),
                'mrweight': lfs.mrweightrepr(),
                'rev': '%08x' % lfs.rev,
                'cksum': '%08x%s' % (
                    lfs.cksum,
                    '' if lfs.ckgcksum() else '?'),
                'total': total_count,
                'total_percent': 100*total_count / max(len(bmap), 1),
                'mdir': mdir_count,
                'mdir_percent': 100*mdir_count / max(len(bmap), 1),
                'btree': btree_count,
                'btree_percent': 100*btree_count / max(len(bmap), 1),
                'data': data_count,
                'data_percent': 100*data_count / max(len(bmap), 1),
            })
        elif title_littlefs:
            title_ = ('littlefs%s v%s.%s %sx%s %s w%s.%s, '
                    'rev %08x, '
                    'cksum %08x%s' % (
                        '' if lfs.ckmagic() else '?',
                        lfs.version.major if lfs.version is not None else '?',
                        lfs.version.minor if lfs.version is not None else '?',
                        lfs.block_size if lfs.block_size is not None else '?',
                        lfs.block_count if lfs.block_count is not None else '?',
                        lfs.addr(),
                        lfs.mbweightrepr(), lfs.mrweightrepr(),
                        lfs.rev,
                        lfs.cksum,
                        '' if lfs.ckgcksum() else '?'))
        else:
            title_ = ('bd %sx%s, %s mdir, %s btree, %s data' % (
                    lfs.block_size if lfs.block_size is not None else '?',
                    lfs.block_count if lfs.block_count is not None else '?',
                    '%.1f%%' % (100*mdir_count / max(len(bmap), 1)),
                    '%.1f%%' % (100*btree_count / max(len(bmap), 1)),
                    '%.1f%%' % (100*data_count / max(len(bmap), 1))))

    # scale width/height if requested
    if (to_scale is not None
            and (width is None or height is None)):
        # don't include header in scale
        width__ = width_
        height__ = height_ - (1 if not no_header else 0)

        # scale width only
        if height is not None:
            width__ = mt.ceil((len(bmap) * to_scale) / max(height__, 1))
        # scale height only
        elif width is not None:
            height__ = mt.ceil((len(bmap) * to_scale) / max(width__, 1))
        # scale based on aspect-ratio
        else:
            width__ = mt.ceil(mt.sqrt(len(bmap) * to_scale * to_ratio))
            height__ = mt.ceil((len(bmap) * to_scale) / max(width__, 1))

        width_ = width__
        height_ = height__ + (1 if not no_header else 0)

    # create a canvas
    canvas = Canvas(
            width_,
            height_ - (1 if not no_header else 0),
            color=color,
            dots=dots,
            braille=braille)

    # these curves are expensive to calculate, so memoize these
    if hilbert:
        curve = ft.cache(lambda w, h: list(hilbert_curve(w, h)))
    elif lebesgue:
        curve = ft.cache(lambda w, h: list(lebesgue_curve(w, h)))
    else:
        curve = ft.cache(lambda w, h: list(naive_curve(w, h)))

    # if contiguous, compute the global curve
    if contiguous:
        global_block = min(bmap.keys(), default=0)
        global_curve = list(curve(canvas.width, canvas.height))

    # if blocky, figure out block sizes/locations
    else:
        # figure out block_cols_/block_rows_
        if block_cols is not None and block_rows is not None:
            block_cols_ = block_cols
            block_rows_ = block_rows
        elif block_rows is not None:
            block_cols_ = mt.ceil(len(bmap) / block_rows)
            block_rows_ = block_rows
        elif block_cols is not None:
            block_cols_ = block_cols
            block_rows_ = mt.ceil(len(bmap) / block_cols)
        else:
            # divide by 2 until we hit our target ratio, this works
            # well for things that are often powers-of-two
            block_cols_ = 1
            block_rows_ = len(bmap)
            while (abs(((canvas.width/(block_cols_*2))
                            / max(canvas.height/mt.ceil(block_rows_/2), 1))
                        - block_ratio)
                    < abs(((canvas.width/block_cols_)
                            / max(canvas.height/block_rows_, 1)))
                        - block_ratio):
                block_cols_ *= 2
                block_rows_ = mt.ceil(block_rows_ / 2)

        block_width_ = canvas.width / block_cols_
        block_height_ = canvas.height / block_rows_

        # assign block locations based on block_rows_/block_cols_ and
        # the requested space filling curve
        for (x, y), b in zip(
                curve(block_cols_, block_rows_),
                sorted(bmap.values())):
            b.x = x * block_width_
            b.y = y * block_height_
            b.width = block_width_
            b.height = block_height_

            # align to pixel boundaries
            b.align()

            # bump up to at least one pixel for every block, dont't
            # worry about out-of-bounds, Canvas handles this for us
            b.width = max(b.width, 1)
            b.height = max(b.height, 1)

    # assign chars based on block type
    for b in bmap.values():
        b.chars = {}
        for type in [b.type] + (['unused'] if args.get('usage') else []):
            char__ = chars_.get((b.block, (type, '0x%x' % b.block)))
            if char__ is not None:
                if isinstance(char__, str):
                    # don't punescape unless we have to
                    if '%' in char__:
                        char__ = punescape(char__, b.attrs)
                    char__ = char__[0] # limit to 1 char
                b.chars[type] = char__

    # assign colors based on block type
    for b in bmap.values():
        b.colors = {}
        for type in [b.type] + (['unused'] if args.get('usage') else []):
            color__ = colors_.get((b.block, (type, '0x%x' % b.block)))
            if color__ is not None:
                # don't punescape unless we have to
                if '%' in color__:
                    color__ = punescape(color__, b.attrs)
                b.colors[type] = color__

    # render to canvas in a specific z-order that prioritizes
    # interesting blocks
    for type in reversed(Z_ORDER):
        # don't render unused blocks in braille/dots mode
        if (braille or dots) and type == 'unused':
            continue

        for b in bmap.values():
            # a bit of a hack, but render all blocks as unused
            # in the first pass in usage mode
            if args.get('usage') and type == 'unused':
                type__ = 'unused'
                usage__ = range(block_size_)
            else:
                type__ = b.type
                usage__ = b.usage

            if type__ != type:
                continue

            # contiguous?
            if contiguous:
                # where are we in the curve?
                if args.get('usage'):
                    # skip blocks with no usage
                    if not usage__:
                        continue
                    block__ = b.block - global_block
                    usage__ = range(
                            mt.floor(((block__*block_size_ + usage__.start)
                                    / (block_size_ * len(bmap)))
                                * len(global_curve)),
                            mt.ceil(((block__*block_size_ + usage__.stop)
                                    / (block_size_ * len(bmap)))
                                * len(global_curve)))
                else:
                    block__ = b.block - global_block
                    usage__ = range(
                            mt.floor((block__/len(bmap)) * len(global_curve)),
                            mt.ceil((block__/len(bmap)) * len(global_curve)))

                # map to global curve
                for i in usage__:
                    if i >= len(global_curve):
                        continue
                    x__, y__ = global_curve[i]

                    # flip y
                    y__ = canvas.height - (y__+1)

                    canvas.point(x__, y__,
                            char=b.chars[type],
                            color=b.colors[type])

            # blocky?
            else:
                x__ = b.x
                y__ = b.y
                width__ = b.width
                height__ = b.height

                # flip y
                y__ = canvas.height - (y__+height__)

                # render byte-level usage?
                if args.get('usage'):
                    # skip blocks with no usage
                    if not usage__:
                        continue
                    # scale from bytes -> pixels
                    usage__ = range(
                            mt.floor((usage__.start/block_size_)
                                * (width__*height__)),
                            mt.ceil((usage__.stop/block_size_)
                                * (width__*height__)))
                    # map to in-block curve
                    for i, (dx, dy) in enumerate(curve(width__, height__)):
                        if i in usage__:
                            # flip y
                            canvas.point(x__+dx, y__+(height__-(dy+1)),
                                    char=b.chars[type],
                                    color=b.colors[type])

                # render simple blocks
                else:
                    canvas.rect(x__, y__, width__, height__,
                            char=b.chars[type],
                            color=b.colors[type])

    # print some summary info
    if not no_header:
        ring.writeln(title_)

    # draw canvas
    for row in range(canvas.height//canvas.yscale):
        line = canvas.draw(row)
        ring.writeln(line)

    if args.get('error_on_corrupt') and corrupted:
        sys.exit(2)


def main(disk, mroots=None, *,
        width=None,
        height=None,
        no_header=None,
        keep_open=False,
        lines=None,
        head=False,
        cat=False,
        sleep=False,
        **args):
    # keep-open?
    if keep_open:
        try:
            # keep track of history if lines specified
            if lines is not None:
                ring = RingIO(lines+1
                        if not no_header and lines > 0
                        else lines)
            while True:
                # register inotify before running the command, this avoids
                # modification race conditions
                if Inotify:
                    inotify = Inotify([disk])

                # cat? write directly to stdout
                if cat:
                    main_(sys.stdout, disk, mroots,
                            width=width,
                            # make space for shell prompt
                            height=-1 if height is ... else height,
                            no_header=no_header,
                            **args)
                # not cat? write to a bounded ring
                else:
                    ring_ = RingIO(head=head)
                    main_(ring_, disk, mroots,
                            width=width,
                            height=0 if height is ... else height,
                            no_header=no_header,
                            **args)
                    # no history? draw immediately
                    if lines is None:
                        ring_.draw()
                    # history? merge with previous lines
                    else:
                        # write header separately?
                        if not no_header:
                            if not ring.lines:
                                ring.lines.append('')
                            ring.lines.extend(it.islice(ring_.lines, 1, None))
                            ring.lines[0] = ring_.lines[0]
                        else:
                            ring.lines.extend(ring_.lines)
                        ring.draw()

                # try to inotifywait
                if Inotify:
                    ptime = time.time()
                    inotify.read()
                    inotify.close()
                    # sleep a minimum amount of time to avoid flickering
                    time.sleep(max(0, (sleep or 0.01) - (time.time()-ptime)))
                else:
                    time.sleep(sleep or 2)
        except KeyboardInterrupt:
            pass

        if not cat:
            sys.stdout.write('\n')

    # single-pass?
    else:
        main_(sys.stdout, disk, mroots,
                width=width,
                # make space for shell prompt
                height=-1 if height is ... else height,
                no_header=no_header,
                **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Render currently used blocks in a littlefs image.",
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
    # need a special Action here because this % causes problems
    class StoreTrueUsage(argparse._StoreTrueAction):
        def format_usage(self):
            return '-%%'
    parser.add_argument(
            '-%', '--usage',
            action=StoreTrueUsage,
            help="Show how much of each block is in use.")
    parser.add_argument(
            '-.', '--add-char', '--chars',
            dest='chars',
            action='append',
            type=lambda x: (
                lambda ks, v: (
                    tuple(k.strip() for k in ks.split(',')),
                    v.strip())
                )(*x.split('=', 1))
                    if '=' in x else x.strip(),
            help="Add characters to use. Can be assigned to a specific "
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
            '--color',
            choices=['never', 'always', 'auto'],
            default='auto',
            help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
            '-:', '--dots',
            action='store_true',
            help="Use 1x2 ascii dot characters.")
    parser.add_argument(
            '-⣿', '--braille',
            action='store_true',
            help="Use 2x4 unicode braille characters. Note that braille "
                "characters sometimes suffer from inconsistent widths.")
    parser.add_argument(
            '-W', '--width',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Width in columns. <=0 uses the terminal width. Defaults "
                "to min(terminal, 80).")
    parser.add_argument(
            '-H', '--height',
            nargs='?',
            type=lambda x: int(x, 0),
            const=..., # handles shell prompt spacing, which is a bit subtle
            help="Height in rows. <=0 uses the terminal height. Defaults "
                "to 1.")
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
            help="Target ratio for block sizes. Defaults to 1:1 or 1:2 "
                "for -:/--dots and -⣿/--braille.")
    parser.add_argument(
            '--no-header',
            action='store_true',
            help="Don't show the header.")
    parser.add_argument(
            '-U', '--hilbert',
            action='store_true',
            help="Render as a space-filling Hilbert curve.")
    parser.add_argument(
            '-Z', '--lebesgue',
            action='store_true',
            help="Render as a space-filling Z-curve.")
    parser.add_argument(
            '-u', '--contiguous',
            action='store_true',
            help="Render as one contiguous curve instead of organizing by "
                "blocks first.")
    parser.add_argument(
            '--to-scale',
            nargs='?',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            const=1,
            help="Scale the resulting map such that 1 char ~= 1/scale "
                "blocks. Defaults to scale=1. ")
    parser.add_argument(
            '--to-ratio',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            help="Aspect ratio to use with --to-scale. Defaults to 1:1.")
    parser.add_argument(
            '-t', '--tiny',
            action='store_true',
            help="Tiny mode, alias for --block-ratio=1, --to-scale=1, "
                "and --no-header.")
    parser.add_argument(
            '--title',
            help="Add a title. Accepts %% modifiers.")
    parser.add_argument(
            '--title-littlefs',
            action='store_true',
            help="Use the littlefs mount string as the title.")
    parser.add_argument(
            '--title-usage',
            action='store_true',
            help="Use the mdir/btree/data usage as the title. This is the "
                "default.")
    parser.add_argument(
            '-e', '--error-on-corrupt',
            action='store_true',
            help="Error if the filesystem is corrupt.")
    parser.add_argument(
            '-k', '--keep-open',
            action='store_true',
            help="Continue to open and redraw the CSV files in a loop.")
    parser.add_argument(
            '-n', '--lines',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Show this many lines of history. <=0 uses the terminal "
                "height. Defaults to 1.")
    parser.add_argument(
            '-^', '--head',
            action='store_true',
            help="Show the first n lines.")
    parser.add_argument(
            '-c', '--cat',
            action='store_true',
            help="Pipe directly to stdout.")
    parser.add_argument(
            '-s', '--sleep',
            type=float,
            help="Time in seconds to sleep between redraws when running "
                "with -k. Defaults to 2 seconds.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
