#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import bisect
import collections as co
import functools as ft
import itertools as it
import math as mt
import os
import struct
import sys

try:
    import crc32c as crc32c_lib
except ModuleNotFoundError:
    crc32c_lib = None


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
TAG_STICKYNOTE  = 0x0203    #  0x0203  v--- --1- ---- --11
TAG_BOOKMARK    = 0x0204    #  0x0204  v--- --1- ---- -1--
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
                    else 'stickynote' if (tag & 0xfff) == TAG_STICKYNOTE
                    else 'bookmark' if (tag & 0xfff) == TAG_BOOKMARK
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
        mask = 0x3

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



# tree renderer
class TreeArt:
    # tree branches are an abstract thing for tree rendering
    class Branch(co.namedtuple('Branch', ['a', 'b', 'z', 'color'])):
        __slots__ = ()
        def __new__(cls, a, b, z=0, color='b'):
            # a and b are context specific
            return super().__new__(cls, a, b, z, color)

        def __repr__(self):
            return '%s(%s, %s, %s, %s)' % (
                    self.__class__.__name__,
                    self.a,
                    self.b,
                    self.z,
                    self.color)

        # don't include color in branch comparisons, or else our tree
        # renderings can end up with inconsistent colors between runs
        def __eq__(self, other):
            return (self.a, self.b, self.z) == (other.a, other.b, other.z)

        def __ne__(self, other):
            return (self.a, self.b, self.z) != (other.a, other.b, other.z)

        def __hash__(self):
            return hash((self.a, self.b, self.z))

        # also order by z first, which can be useful for reproducibly
        # prioritizing branches when simplifying trees
        def __lt__(self, other):
            return (self.z, self.a, self.b) < (other.z, other.a, other.b)

        def __le__(self, other):
            return (self.z, self.a, self.b) <= (other.z, other.a, other.b)

        def __gt__(self, other):
            return (self.z, self.a, self.b) > (other.z, other.a, other.b)

        def __ge__(self, other):
            return (self.z, self.a, self.b) >= (other.z, other.a, other.b)

        # apply a function to a/b while trying to avoid copies
        def map(self, filter_, map_=None):
            if map_ is None:
                filter_, map_ = None, filter_

            a = self.a
            if filter_ is None or filter_(a):
                a = map_(a)

            b = self.b
            if filter_ is None or filter_(b):
                b = map_(b)

            if a != self.a or b != self.b:
                return self.__class__(
                        a if a != self.a else self.a,
                        b if b != self.b else self.b,
                        self.z,
                        self.color)
            else:
                return self

    def __init__(self, tree):
        self.tree = tree
        self.depth = max((t.z+1 for t in tree), default=0)
        if self.depth > 0:
            self.width = 2*self.depth + 2
        else:
            self.width = 0

    def __iter__(self):
        return iter(self.tree)

    def __bool__(self):
        return bool(self.tree)

    def __len__(self):
        return len(self.tree)

    # render an rbyd rbyd tree for debugging
    @classmethod
    def _fromrbydrtree(cls, rbyd, **args):
        trunks = co.defaultdict(lambda: (-1, 0))
        alts = co.defaultdict(lambda: {})

        for rid, rattr, path in rbyd.rattrs(path=True):
            # keep track of trunks/alts
            trunks[rattr.toff] = (rid, rattr.tag)

            for ralt in path:
                if ralt.followed:
                    alts[ralt.toff] |= {'f': ralt.joff, 'c': ralt.color}
                else:
                    alts[ralt.toff] |= {'nf': ralt.off, 'c': ralt.color}

        if args.get('tree_rbyd'):
            # treat unreachable alts as converging paths
            for j_, alt in alts.items():
                if 'f' not in alt:
                    alt['f'] = alt['nf']
                elif 'nf' not in alt:
                    alt['nf'] = alt['f']

        else:
            # prune any alts with unreachable edges
            pruned = {}
            for j, alt in alts.items():
                if 'f' not in alt:
                    pruned[j] = alt['nf']
                elif 'nf' not in alt:
                    pruned[j] = alt['f']
            for j in pruned.keys():
                del alts[j]

            for j, alt in alts.items():
                while alt['f'] in pruned:
                    alt['f'] = pruned[alt['f']]
                while alt['nf'] in pruned:
                    alt['nf'] = pruned[alt['nf']]

        # find the trunk and depth of each alt
        def rec_trunk(j):
            if j not in alts:
                return trunks[j]
            else:
                if 'nft' not in alts[j]:
                    alts[j]['nft'] = rec_trunk(alts[j]['nf'])
                return alts[j]['nft']

        for j in alts.keys():
            rec_trunk(j)
        for j, alt in alts.items():
            if alt['f'] in alts:
                alt['ft'] = alts[alt['f']]['nft']
            else:
                alt['ft'] = trunks[alt['f']]

        def rec_height(j):
            if j not in alts:
                return 0
            else:
                if 'h' not in alts[j]:
                    alts[j]['h'] = max(
                            rec_height(alts[j]['f']),
                            rec_height(alts[j]['nf'])) + 1
                return alts[j]['h']

        for j in alts.keys():
            rec_height(j)

        t_depth = max((alt['h']+1 for alt in alts.values()), default=0)

        # convert to more general tree representation
        tree = set()
        for j, alt in alts.items():
            # note all non-trunk edges should be colored black
            tree.add(cls.Branch(
                    alt['nft'],
                    alt['nft'],
                    t_depth-1 - alt['h'],
                    alt['c']))
            if alt['ft'] != alt['nft']:
                tree.add(cls.Branch(
                        alt['nft'],
                        alt['ft'],
                        t_depth-1 - alt['h'],
                        'b'))

        return cls(tree)

    # render an rbyd btree tree for debugging
    @classmethod
    def _fromrbydbtree(cls, rbyd, **args):
        # for rbyds this is just a pointer to every rid
        tree = set()
        root = None
        for rid, name in rbyd.rids():
            b = (rid, name.tag)
            if root is None:
                root = b
            tree.add(cls.Branch(root, b))
        return cls(tree)

    # render an rbyd tree for debugging
    @classmethod
    def fromrbyd(cls, rbyd, **args):
        if args.get('tree_btree'):
            return cls._fromrbydbtree(rbyd, **args)
        else:
            return cls._fromrbydrtree(rbyd, **args)

    # render some nice ascii trees
    def repr(self, x, color=False):
        if self.depth == 0:
            return ''

        def branchrepr(tree, x, d, was):
            for t in tree:
                if t.z == d and t.b == x:
                    if any(t.z == d and t.a == x
                            for t in tree):
                        return '+-', t.color, t.color
                    elif any(t.z == d
                                and x > min(t.a, t.b)
                                and x < max(t.a, t.b)
                            for t in tree):
                        return '|-', t.color, t.color
                    elif t.a < t.b:
                        return '\'-', t.color, t.color
                    else:
                        return '.-', t.color, t.color
            for t in tree:
                if t.z == d and t.a == x:
                    return '+ ', t.color, None
            for t in tree:
                if (t.z == d
                        and x > min(t.a, t.b)
                        and x < max(t.a, t.b)):
                    return '| ', t.color, was
            if was:
                return '--', was, was
            return '  ', None, None

        trunk = []
        was = None
        for d in range(self.depth):
            t, c, was = branchrepr(self.tree, x, d, was)

            trunk.append('%s%s%s%s' % (
                    '\x1b[33m' if color and c == 'y'
                        else '\x1b[31m' if color and c == 'r'
                        else '\x1b[1;30m' if color and c == 'b'
                        else '',
                    t,
                    ('>' if was else ' ') if d == self.depth-1 else '',
                    '\x1b[m' if color and c else ''))

        return '%s ' % ''.join(trunk)

# some more renderers

# render a btree rbyd tree for debugging
@classmethod
def _treeartfrombtreertree(cls, btree, *,
        depth=None,
        inner=False,
        **args):
    # precompute rbyd trees so we know the max depth at each layer
    # to nicely align trees
    rtrees = {}
    rdepths = {}
    for bid, rbyd, path in btree.traverse(path=True, depth=depth):
        if not rbyd:
            continue

        rtree = cls.fromrbyd(rbyd, **args)
        rtrees[rbyd] = rtree
        rdepths[len(path)] = max(rdepths.get(len(path), 0), rtree.depth)

    # map rbyd branches into our btree space
    tree = set()
    for bid, rbyd, path in btree.traverse(path=True, depth=depth):
        if not rbyd:
            continue

        # yes we can find new rbyds if disk is being mutated, just
        # ignore these
        if rbyd not in rtrees:
            continue

        rtree = rtrees[rbyd]
        rz = max((t.z+1 for t in rtree), default=0)
        d = sum(rdepths[d]+1 for d in range(len(path)))

        # map into our btree space
        for t in rtree:
            # note we adjust our bid to be left-leaning, this allows
            # a global order and makes tree rendering quite a bit easier
            a_rid, a_tag = t.a
            b_rid, b_tag = t.b
            _, (_, a_w, _) = rbyd.lookupnext(a_rid)
            _, (_, b_w, _) = rbyd.lookupnext(b_rid)
            tree.add(cls.Branch(
                    (bid-(rbyd.weight-1)+a_rid-(a_w-1), len(path), a_tag),
                    (bid-(rbyd.weight-1)+b_rid-(b_w-1), len(path), b_tag),
                    d + rdepths[len(path)]-rz + t.z,
                    t.color))

        # connect rbyd branches to rbyd roots
        if path:
            l_bid, l_rbyd, l_rid, l_name = path[-1]
            l_branch = l_rbyd.lookup(l_rid, TAG_BRANCH, 0x3)

            if rtree:
                r_rid, r_tag = min(rtree, key=lambda t: t.z).a
                _, (_, r_w, _) = rbyd.lookupnext(r_rid)
            else:
                r_rid, (r_tag, r_w, _) = rbyd.lookupnext(-1)

            tree.add(cls.Branch(
                    (l_bid-(l_name.weight-1), len(path)-1, l_branch.tag),
                    (bid-(rbyd.weight-1)+r_rid-(r_w-1), len(path), r_tag),
                    d-1))

    # remap branches to leaves if we aren't showing inner branches
    if not inner:
        # step through each btree layer backwards
        b_depth = max((t.a[1]+1 for t in tree), default=0)

        for d in reversed(range(b_depth-1)):
            # find bid ranges at this level
            bids = set()
            for t in tree:
                if t.b[1] == d:
                    bids.add(t.b[0])
            bids = sorted(bids)

            # find the best root for each bid range
            roots = {}
            for i in range(len(bids)):
                for t in tree:
                    if (t.a[1] > d
                            and t.a[0] >= bids[i]
                            and (i == len(bids)-1 or t.a[0] < bids[i+1])
                            and (bids[i] not in roots
                                or t < roots[bids[i]])):
                        roots[bids[i]] = t

            # remap branches to leaf-roots
            tree = {t.map(
                        lambda x: x[1] == d and x[0] in roots,
                        lambda x: roots[x[0]].a)
                    for t in tree}

    return cls(tree)

# render a btree btree tree for debugging
@classmethod
def _treeartfrombtreebtree(cls, btree, *,
        depth=None,
        inner=False,
        **args):
    # find all branches
    tree = set()
    root = None
    branches = {}
    for bid, name, path in btree.bids(
            path=True,
            depth=depth):
        # create branch for each jump in path
        #
        # note we adjust our bid to be left-leaning, this allows
        # a global order and makes tree rendering quite a bit easier
        a = root
        for d, (bid_, rbyd_, rid_, name_) in enumerate(path):
            # map into our btree space
            bid__ = bid_-(name_.weight-1)
            b = (bid__, d, name_.tag)

            # remap branches to leaves if we aren't showing inner
            # branches
            if not inner:
                if b not in branches:
                    bid_, rbyd_, rid_, name_ = path[-1]
                    bid__ = bid_-(name_.weight-1)
                    branches[b] = (bid__, len(path)-1, name_.tag)
                b = branches[b]

            # render the root path on first rid, this is arbitrary
            if root is None:
                root, a = b, b

            tree.add(cls.Branch(a, b, d))
            a = b

    return cls(tree)

# render a btree tree for debugging
@classmethod
def treeartfrombtree(cls, btree, **args):
    if args.get('tree_btree'):
        return cls._frombtreebtree(btree, **args)
    else:
        return cls._frombtreertree(btree, **args)

TreeArt._frombtreertree = _treeartfrombtreertree
TreeArt._frombtreebtree = _treeartfrombtreebtree
TreeArt.frombtree = treeartfrombtree

# render a file tree for debugging
@classmethod
def treeartfromfile(cls, file, **args):
    tree = cls.frombtree(file.bshrub, **args)
    t_depth = tree.depth

    # connect bptr tags to bptrs
    tree = set(tree)
    bptrs = {}
    for pos, data, path in file.datas(
            path=True,
            depth=args.get('depth')):
        if isinstance(data, Bptr):
            a = (pos, len(path)-1, data.tag)
            b = (pos, len(path), data.tag)
            bptrs[a] = b
            tree.add(cls.Branch(a, b, t_depth))

    # if we're not showing inner branches, nudge bptr tags to
    # their bptrs
    if not args.get('inner'):
        tree = {t.map(lambda x: bptrs.get(x, x)) for t in tree}

    return cls(tree)

TreeArt.fromfile = treeartfromfile



# show the littlefs config
def dbg_config(lfs, *,
        color=False,
        w_width=2,
        **args):
    for i, config in enumerate(it.chain(
                lfs.config,
                lfs.attrs())):
        # some special situations worth reporting
        notes = []
        # magic corrupt?
        if (config.tag == TAG_MAGIC
                and config.data != b'littlefs'):
            notes.append('magic!=littlefs')

        print('%s%12s %*s %-*s  %s%s%s' % (
                '\x1b[31m' if color and notes else '',
                '{%s}:' % ','.join('%04x' % block
                        for block in config.blocks)
                    if i == 0 else '',
                2*w_width+1, '%d.%d' % (-1, -1)
                    if i == 0 else '',
                21+w_width, config.repr(),
                next(xxd(config.data, 8), '')
                    if not args.get('raw')
                        and not args.get('no_truncate')
                    else '',
                ' (%s)' % ', '.join(notes) if notes else '',
                '\x1b[m' if color and notes else ''))

        # show on-disk encoding
        if args.get('raw') or args.get('no_truncate'):
            for o, line in enumerate(xxd(config.data)):
                print('%11s: %*s %s' % (
                        '%04x' % (config.toff + o*16),
                        2*w_width+1, '',
                        line))

# show the littlefs gstate
def dbg_gstate(lfs, *,
        color=False,
        w_width=2,
        **args):
    for i, gstate in enumerate(lfs.gstate):
        # some special situations worth reporting
        notes = []
        # gcksum mismatch?
        if (gstate.tag == TAG_GCKSUMDELTA
                and int(gstate) != crc32ccube(lfs.cksum)):
            notes.append('gcksum!=%08x' % crc32ccube(lfs.cksum))

        print('%s%12s %*s %-*s  %s%s%s' % (
                '\x1b[31m' if color and notes else '',
                'gstate:'
                    if i == 0 or args.get('gdelta')
                    else '',
                2*w_width+1, 'g.-1'
                    if i == 0 or args.get('gdelta')
                    else '',
                21+w_width, gstate.repr(),
                next(xxd(gstate.data, 8), '')
                    if not args.get('raw')
                        and not args.get('no_truncate')
                    else '',
                ' (%s)' % ', '.join(notes) if notes else '',
                '\x1b[m' if color and notes else ''))

        # show on-disk encoding
        if args.get('raw') or args.get('no_truncate'):
            for o, line in enumerate(xxd(gstate.data)):
                print('%11s: %*s %s' % (
                        '%04x' % (o*16),
                        2*w_width+1, '',
                        line))

        # print gdeltas?
        if args.get('gdelta'):
            for mid, gdelta in gstate.gdeltas:
                print('%s%12s %*s %-*s  %s%s' % (
                        '\x1b[1;30m' if color else '',
                        '{%s}:' % ','.join('%04x' % block
                            for block in gdelta.blocks),
                        2*w_width+1, mid.repr(),
                        21+w_width, gdelta.repr(),
                        next(xxd(gdelta.data, 8), '')
                            if not args.get('raw')
                                and not args.get('no_truncate')
                            else '',
                        '\x1b[m' if color else ''))

                # show on-disk encoding
                if args.get('raw'):
                    for o, line in enumerate(xxd(gdelta.tdata)):
                        print('%11s: %*s %s' % (
                                '%04x' % (gdelta.toff + o*16),
                                2*w_width+1, '',
                                line))
                if args.get('raw') or args.get('no_truncate'):
                    for o, line in enumerate(xxd(gdelta.data)):
                        print('%11s: %*s %s' % (
                                '%04x' % (gdelta.off + o*16),
                                2*w_width+1, '',
                                line))

# show the littlefs file tree
def dbg_files(lfs, paths, *,
        color=False,
        w_width=2,
        recurse=None,
        all=False,
        no_orphans=False,
        **args):
    all_ = all; del all

    # parse all paths first, error if anything is malformed
    dirs = []
    # default paths to the root dir
    for path in (paths or ['/']):
        try:
            dir = lfs.pathlookup(path,
                    all=args.get('all'))
        except Lfs.PathError as e:
            print("error: %s" % e,
                    file=sys.stderr)
            sys.exit(-1)

        if dir is not None:
            dirs.append(dir)

    # it's kinda tricky to iterate over everything we want to show,
    # so create a reusable iterator
    def iter_dir(dir, **args_):
        if dir.recursable:
            yield from dir.files(**args_)
        else:
            if args_.get('path'):
                yield dir, []
            else:
                yield dir

        # include any orphaned entries in the root directory to help
        # debugging (these don't actually live in the root directory)
        if not no_orphans and isinstance(dir, Lfs.Root):
            # finding orphans is expensive, so cache this
            if not hasattr(iter_dir, 'orphans'):
                iter_dir.orphans = dir.lfs.orphans()
            for orphan in iter_dir.orphans:
                if args_.get('path'):
                    yield orphan, []
                else:
                    yield orphan

    # do a pass to figure out the width+depth of the file tree
    # and file names so we can format things nicely
    f_depth, f_width = 0, 0
    for dir in dirs:
        for file, path in iter_dir(dir,
                all=all_,
                depth=recurse,
                path=True):
            f_depth = max(f_depth, len(path)+1)
            f_width = max(f_width, 4*len(path) + len(file.name.name))

    # only show the mdir/rbyd/block address on mdir change
    pmdir = None
    # recursively print directories
    def dbg_dir(dir,
            depth,
            prefixes=('', '', '', '')):
        nonlocal pmdir

        # first figure out the dir length so we know when the dir ends
        if prefixes != ('', '', '', ''):
            len_ = sum(1 for _ in iter_dir(dir, all=all_))
        else:
            len_ = 1

        # print files
        for i, file in enumerate(iter_dir(dir, all=all_)):
            # some special situations worth reporting
            notes = []
            # grmed?
            if file.grmed:
                notes.append('grmed')
            # orphaned?
            if file.orphaned:
                notes.append('orphaned')
            # missing bookmark/did?
            if isinstance(file, Lfs.Dir):
                if file.did is None:
                    notes.append('missing did')
                elif lfs.namelookup(file.did, b'') is None:
                    notes.append('missing bookmark')

            # print human readable file entry
            print('%s%12s %*s %-*s  %s%s%s' % (
                    '\x1b[31m'
                        if color and not file.grmed and notes
                        else '\x1b[1;30m'
                        if color and (file.grmed or file.internal)
                        else '',
                    '{%s}:' % ','.join('%04x' % block
                            for block in file.mdir.blocks)
                        if not isinstance(pmdir, Mdir) or file.mdir != pmdir
                        else '',
                    2*w_width+1, file.mid.repr(),
                    f_width, '%s%s' % (
                        prefixes[0+(i==len_-1)],
                        file.name.name.decode('utf8',
                            errors='backslashreplace')),
                    file.repr(),
                    ' (%s)' % ', '.join(notes) if notes else '',
                    '\x1b[m'
                        if color and (notes or file.grmed or file.internal)
                        else ''))
            pmdir = file.mdir

            # print attrs associated with each file?
            if args.get('attrs'):
                for rattr in file.rattrs():
                    print('%12s %*s %-*s  %s' % (
                            '',
                            2*w_width+1, '',
                            21+w_width, rattr.repr(),
                            next(xxd(rattr.data, 8), '')
                                if not args.get('raw')
                                    and not args.get('no_truncate')
                                else ''))

                    # show on-disk encoding
                    if args.get('raw'):
                        for o, line in enumerate(xxd(rattr.tdata)):
                            print('%11s: %*s %s' % (
                                    '%04x' % (rattr.toff + o*16),
                                    2*w_width+1, '',
                                    line))
                    if args.get('raw') or args.get('no_truncate'):
                        for o, line in enumerate(xxd(rattr.data)):
                            print('%11s: %*s %s' % (
                                    '%04x' % (rattr.off + o*16),
                                    2*w_width+1, '',
                                    line))

            # print file structures?
            if args.get('structs'):
                dbg_struct(file)

            # recurse?
            if (file.recursable
                    and depth is not None
                    and (depth == 0 or depth > 1)):
                dbg_dir(file,
                        depth-1 if depth else 0,
                        (prefixes[2+(i==len_-1)] + "|-> ",
                         prefixes[2+(i==len_-1)] + "'-> ",
                         prefixes[2+(i==len_-1)] + "|   ",
                         prefixes[2+(i==len_-1)] + "    "))

    # print file structures
    def dbg_struct(file):
        nonlocal pmdir

        # no tree?
        if file.bshrub is None:
            return

        # precompute tree renderings
        bt_width = 0
        if (args.get('tree')
                or args.get('tree_rbyd')
                or args.get('tree_btree')):
            treeart = TreeArt.fromfile(file, **args)
            bt_width = treeart.width

        # dynamically size the id field
        bw_width = mt.ceil(mt.log10(max(1, file.size)+1))

        # recursively print bshrub branches
        def dbg_branch(d, bid, rbyd, rid, name):
            nonlocal pmdir

            for rattr in rbyd.rattrs(rid):
                print('%12s %*s %s%*s %-*s  %s' % (
                        '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                            if not isinstance(pmdir, Rbyd) or rbyd != pmdir
                            else '',
                        2*w_width+1, '',
                        treeart.repr(
                                (bid-(name.weight-1), d, rattr.tag),
                                color)
                            if args.get('tree')
                                or args.get('tree_rbyd')
                                or args.get('tree_btree')
                            else '',
                        2*bw_width+1, '%d-%d' % (bid-(rattr.weight-1), bid)
                            if rattr.weight > 1
                            else bid if rattr.weight > 0
                            else '',
                        21+2*bw_width+1, rattr.repr(),
                        next(xxd(rattr.data, 8), '')
                            if not args.get('raw')
                                and not args.get('no_truncate')
                            else ''))
                pmdir = rbyd

                # show on-disk encoding of tags/data
                if args.get('raw'):
                    for o, line in enumerate(xxd(rattr.tdata)):
                        print('%11s: %*s %*s%*s %s' % (
                                '%04x' % (rattr.toff + o*16),
                                2*w_width+1, '',
                                bt_width, '',
                                2*bw_width+1, '',
                                line))
                if args.get('raw') or args.get('no_truncate'):
                    for o, line in enumerate(xxd(rattr.data)):
                        print('%11s: %*s %*s%*s %s' % (
                                '%04x' % (rattr.off + o*16),
                                2*w_width+1, '',
                                bt_width, '',
                                2*bw_width+1, '',
                                line))

        # print inlined data, block pointers, etc
        def dbg_bptr(d, pos, bptr):
            nonlocal pmdir
            # some special situations worth reporting
            notes = []
            # cksum mismatch?
            cksum = crc32c(bptr.ckdata)
            if cksum != bptr.cksum:
                notes.append('cksum!=%08x' % bptr.cksum)

            print('%s%12s%s %*s %s%s%s%-*s%s%s' % (
                    '\x1b[31m' if color and notes else '',
                    '%04x.%04x:' % (bptr.block, bptr.off)
                        if not isinstance(pmdir, Bptr) or bptr != pmdir
                        else '',
                    '\x1b[0m' if color and notes else '',
                    2*w_width+1, '',
                    treeart.repr((pos, d, bptr.tag), color)
                        if args.get('tree')
                            or args.get('tree_rbyd')
                            or args.get('tree_btree')
                        else '',
                    '\x1b[31m' if color and notes else '',
                    '%*s ' % (
                        2*bw_width+1, '%d-%d' % (pos, pos+(bptr.weight-1))
                            if bptr.weight > 1
                            else pos if bptr.weight > 0
                            else ''),
                    56+2*bw_width+1, '%-*s  %s' % (
                        21+2*bw_width+1, bptr.repr(),
                        next(xxd(bptr.data, 8), '')
                            if not args.get('raw')
                                and not args.get('no_truncate')
                            else ''),
                    ' (%s)' % ', '.join(notes) if notes else '',
                    '\x1b[m' if color and notes else ''))
            pmdir = bptr

            # show on-disk encoding of tag/bptr/data
            if args.get('raw'):
                for o, line in enumerate(xxd(bptr.rattr.tdata)):
                    print('%11s: %*s %*s%s%s' % (
                            '%04x' % (bptr.rattr.toff + o*16),
                            2*w_width+1, '',
                            bt_width, '',
                            '%*s ' % (2*bw_width+1, ''),
                            line))
            if args.get('raw'):
                for o, line in enumerate(xxd(bptr.rattr.data)):
                    print('%11s: %*s %*s%s%s' % (
                            '%04x' % (bptr.rattr.off + o*16),
                            2*w_width+1, '',
                            bt_width, '',
                            '%*s ' % (2*bw_width+1, ''),
                            line))
            if args.get('raw') or args.get('no_truncate'):
                for o, line in enumerate(xxd(bptr.data)):
                    print('%11s: %*s %*s%s%s' % (
                            '%04x' % (bptr.off + o*16),
                            2*w_width+1, '',
                            bt_width, '',
                            '%*s ' % (2*bw_width+1, ''),
                            line))

        # traverse and print entries
        ppath = []
        for pos, data, path in file.leaves(
                path=True,
                depth=args.get('depth')):
            # print inner branches if requested
            if args.get('inner'):
                for d, (bid_, rbyd_, rid_, name_) in pathdelta(
                        path, ppath):
                    dbg_branch(d, bid_, rbyd_, rid_, name_)
            ppath = path

            # inlined data?
            if isinstance(data, Rattr):
                # a bit of a hack
                if not args.get('inner'):
                    bid_, rbyd_, rid_, name_ = path[-1]
                    dbg_branch(len(path)-1, bid_, rbyd_, rid_, data)

            # block pointer?
            elif isinstance(data, Bptr):
                # show the data
                dbg_bptr(len(path), pos, data)

            # btree node?
            else:
                rbyd = data
                bid = pos + (rbyd.weight-1)

                # corrupted? try to keep printing the tree
                if not rbyd:
                    print('%s%11s: %*s %*s%s%s' % (
                            '\x1b[31m' if color else '',
                            '%04x.%04x' % (rbyd.block, rbyd.trunk),
                            2*w_width+1, '',
                            bt_width, '',
                            '(corrupted rbyd %s)' % rbyd.addr(),
                            '\x1b[m' if color else ''))
                    pmdir = None
                    continue

                for rid, name in rbyd.rids():
                    bid_ = bid-(rbyd.weight-1) + rid
                    # show the leaf entry/branch
                    dbg_branch(len(path), bid_, rbyd, rid, name)

    # print stuff
    for dir in dirs:
        dbg_dir(dir, recurse)

# common ck function
def dbg_ck(lfs, *,
        meta=True,
        data=True,
        mtree_only=False,
        quiet=False,
        color=False,
        **args):
    # lfs traverse does most of the work here
    corrupted = False
    for child in lfs.traverse(
            mtree_only=mtree_only):
        # limit to metadata blocks?
        if (((meta and isinstance(child, (Mdir, Rbyd)))
                    or (data and isinstance(child, Bptr)))
                and not child):
            if not quiet:
                print('%s%11s: %s%s' % (
                        '\x1b[31m' if color else '',
                        '{%s}' % ','.join('%04x' % block
                                for block in child.blocks)
                            if isinstance(child, Mdir)
                            else '%04x.%04x' % (child.block, child.trunk)
                            if isinstance(child, Rbyd)
                            else '%04x.%04x' % (child.block, child.off),
                        '(corrupted %s %s)' % (
                            'mroot' if isinstance(child, Mdir)
                                    and child.mid == -1
                                else 'mdir' if isinstance(child, Mdir)
                                else 'rbyd' if isinstance(child, Rbyd)
                                else 'bptr',
                            child.addr()),
                        '\x1b[m' if color else ''))
            corrupted = True

    return not corrupted

# check metadata blocks for errors
def dbg_ckmeta(lfs, **args):
    return dbg_ck(lfs, meta=True, **args)

# check metadata + data blocks for errors
def dbg_ckdata(lfs, **args):
    return dbg_ck(lfs, meta=True, data=True, **args)


def main(disk, mroots=None, paths=None, *,
        trunk=None,
        block_size=None,
        block_count=None,
        quiet=False,
        color='auto',
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # show files be default, but there's quite a few other things we
    # can show if requested
    show_config = args.get('config')
    show_gstate = (args.get('gstate')
            or args.get('gdelta'))
    show_files = (args.get('files')
            or args.get('structs')
            or args.get('attrs'))
    show_ckmeta = args.get('ckmeta')
    show_ckdata = args.get('ckdata')

    if (not show_config
            and not show_gstate
            and not show_files
            and not show_ckmeta
            and not show_ckdata):
        show_files = True

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

        # print some information about the filesystem
        if not quiet:
            print('littlefs%s v%s.%s %sx%s %s w%s.%s, '
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

        # dynamically size the id field
        w_width = max(
                mt.ceil(mt.log10(max(1, lfs.mbweight >> lfs.mbits)+1)),
                mt.ceil(mt.log10(max(1, max(
                    mdir.weight for mdir in lfs.mtree.mdirs())))+1),
                # in case of -1.-1
                2)

        # show the on-disk config?
        if show_config and not quiet:
            dbg_config(lfs,
                    color=color,
                    w_width=w_width,
                    **args)

        # show the on-disk gstate?
        if show_gstate and not quiet:
            dbg_gstate(lfs,
                    color=color,
                    w_width=w_width,
                    **args)

        # show the on-disk file tree?
        if show_files and not quiet:
            dbg_files(lfs, paths,
                    color=color,
                    w_width=w_width,
                    **args)

        # always check magic/gcksum
        corrupted = not bool(lfs)

        # check metadata blocks for errors
        if show_ckmeta and not show_ckdata:
            if not dbg_ckmeta(lfs,
                    quiet=quiet,
                    color=color,
                    w_width=w_width,
                    **args):
                corrupted = True

        # check metadata + data blocks for errors
        if show_ckdata:
            if not dbg_ckdata(lfs,
                    quiet=quiet,
                    color=color,
                    w_width=w_width,
                    **args):
                corrupted = True

    # ckmeta/ckdata implies error_on_corrupt
    if ((show_ckmeta or show_ckdata or args.get('error_on_corrupt'))
            and corrupted):
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Debug littlefs stuff.",
            allow_abbrev=False)
    parser.add_argument(
            'disk',
            help="File containing the block device.")
    class AppendMrootOrPath(argparse.Action):
        def __call__(self, parser, namespace, values, option):
            for value in values:
                # mroot?
                if not isinstance(value, str):
                    if getattr(namespace, 'mroots', None) is None:
                        namespace.mroots = []
                    namespace.mroots.append(value)
                # or path?
                else:
                    if getattr(namespace, 'paths', None) is None:
                        namespace.paths = []
                    namespace.paths.append(value)
    parser.add_argument(
            'mroots',
            nargs='*',
            type=lambda x: rbydaddr(x) if not x.startswith('/') else x,
            action=AppendMrootOrPath,
            help="Block address of the mroots. Defaults to 0x{0,1}.")
    parser.add_argument(
            'paths',
            nargs='*',
            type=lambda x: rbydaddr(x) if not x.startswith('/') else x,
            action=AppendMrootOrPath,
            help="Paths to show, must start with a leading slash. Defaults "
                "to the root directory.")
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
            '-q', '--quiet',
            action='store_true',
            help="Don't show anything, useful when checking for errors.")
    parser.add_argument(
            '--color',
            choices=['never', 'always', 'auto'],
            default='auto',
            help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
            '--config',
            action='store_true',
            help="Show the on-disk config.")
    parser.add_argument(
            '--gstate',
            action='store_true',
            help="Show the on-disk global-state.")
    parser.add_argument(
            '--gdelta',
            action='store_true',
            help="Show relevant gdeltas used to build the gstate. "
                "Implies --gstate.")
    parser.add_argument(
            '--files',
            action='store_true',
            help="Show the file tree (the default).")
    parser.add_argument(
            '--structs',
            action='store_true',
            help="Show the internal structure of files. Implies --files.")
    parser.add_argument(
            '--attrs',
            action='store_true',
            help="Show custom attributes attached to files. Implies --files.")
    parser.add_argument(
            '--ckmeta',
            action='store_true',
            help="Check metadata blocks for errors.")
    parser.add_argument(
            '--ckdata',
            action='store_true',
            help="Check metadata + data blocks for errors.")
    parser.add_argument(
            '--mtree-only',
            action='store_true',
            help="Only traverse the mtree.")
    parser.add_argument(
            '-r', '--recurse', '--file-depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of the file tree to show. 0 shows all files in the "
                "filesystem. Defaults to 1, which only shows the root "
                "directory.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Show all files including bookmarks, stickynotes, grms, "
                "etc.")
    parser.add_argument(
            '--no-orphans',
            action='store_true',
            help="Don't scan for orphaned files.")
    parser.add_argument(
            '-x', '--raw',
            action='store_true',
            help="Show the raw data including tag encodings.")
    parser.add_argument(
            '-T', '--no-truncate',
            action='store_true',
            help="Don't truncate, show the full contents.")
    parser.add_argument(
            '-t', '--tree',
            action='store_true',
            help="Show the rbyd tree.")
    parser.add_argument(
            '-R', '--tree-rbyd',
            action='store_true',
            help="Show the full rbyd tree.")
    parser.add_argument(
            '-B', '--tree-btree',
            action='store_true',
            help="Show a simplified btree tree.")
    parser.add_argument(
            '-i', '--inner',
            action='store_true',
            help="Show inner branches.")
    parser.add_argument(
            '-z', '--depth', '--tree-depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of trees to show. Defaults to 0, which shows full "
                "trees.")
    parser.add_argument(
            '-e', '--error-on-corrupt',
            action='store_true',
            help="Error if the filesystem is corrupt.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
