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
TAG_BNAME       = 0x0200    #  0x0200  v--- --1- ---- ----
TAG_REG         = 0x0201    #  0x0201  v--- --1- ---- ---1
TAG_DIR         = 0x0202    #  0x0202  v--- --1- ---- --1-
TAG_STICKYNOTE  = 0x0203    #  0x0203  v--- --1- ---- --11
TAG_BOOKMARK    = 0x0204    #  0x0204  v--- --1- ---- -1--
TAG_MNAME       = 0x0220    #  0x0220  v--- --1- --1- ----
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
                'bname' if (tag & 0xfff) == TAG_BNAME
                    else 'reg' if (tag & 0xfff) == TAG_REG
                    else 'dir' if (tag & 0xfff) == TAG_DIR
                    else 'stickynote' if (tag & 0xfff) == TAG_STICKYNOTE
                    else 'bookmark' if (tag & 0xfff) == TAG_BOOKMARK
                    else 'mname' if (tag & 0xfff) == TAG_MNAME
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

# render an mtree tree for debugging
@classmethod
def _treeartfrommtreertree(cls, mtree, *,
        depth=None,
        inner=False,
        **args):
    # precompute rbyd trees so we know the max depth at each layer
    # to nicely align trees
    rtrees = {}
    rdepths = {}
    for mdir, path in mtree.traverse(path=True, depth=depth):
        if isinstance(mdir, Mdir):
            if not mdir:
                continue
            rbyd = mdir.rbyd
        else:
            bid, rbyd = mdir
            if not rbyd:
                continue

        rtree = cls.fromrbyd(rbyd, **args)
        rtrees[rbyd] = rtree
        rdepths[len(path)] = max(rdepths.get(len(path), 0), rtree.depth)

    # map rbyd branches into our mtree space
    tree = set()
    branches = {}
    for mdir, path in mtree.traverse(path=True, depth=depth):
        if isinstance(mdir, Mdir):
            if not mdir:
                continue
            rbyd = mdir.rbyd
        else:
            bid, rbyd = mdir
            if not rbyd:
                continue

        # yes we can find new rbyds if disk is being mutated, just
        # ignore these
        if rbyd not in rtrees:
            continue

        rtree = rtrees[rbyd]
        rz = max((t.z+1 for t in rtree), default=0)
        d = sum(rdepths[d]+1 for d, p in enumerate(path))

        # map into our mtree space
        for t in rtree:
            # note we adjust our mid/bid to be left-leaning, this allows
            # a global order and makes tree rendering quite a bit easier
            #
            # we also need to give btree nodes mrid=-1 so they come
            # before and mrid=-1 mdir attrs
            a_rid, a_tag = t.a
            b_rid, b_tag = t.b
            _, (_, a_w, _) = rbyd.lookupnext(a_rid)
            _, (_, b_w, _) = rbyd.lookupnext(b_rid)
            if isinstance(mdir, Mdir):
                a_mid = mtree.mid(mdir.mid, a_rid)
                b_mid = mtree.mid(mdir.mid, b_rid)
            else:
                a_mid = mtree.mid(bid-(rbyd.weight-1)+a_rid-(a_w-1), -1)
                b_mid = mtree.mid(bid-(rbyd.weight-1)+b_rid-(b_w-1), -1)

            tree.add(cls.Branch(
                    (a_mid, len(path), a_tag),
                    (b_mid, len(path), b_tag),
                    d + rdepths[len(path)]-rz + t.z,
                    t.color))

        # connect rbyd branches to rbyd roots
        if path:
            # figure out branch mid/attr
            if isinstance(path[-1][1], Mdir):
                l_mid, l_mdir, l_name = path[-1]
                l_branch = (l_mdir.lookup(l_mid, TAG_MROOT, 0x3)
                        or l_mdir.lookup(l_mid, TAG_MTREE, 0x3))
            else:
                l_bid, l_rbyd, l_rid, l_name = path[-1]
                l_mid = mtree.mid(l_bid-(l_name.weight-1), -1)
                l_branch = (l_rbyd.lookup(l_rid, TAG_BRANCH, 0x3)
                        or l_rbyd.lookup(l_rid, TAG_MDIR, 0x3))

            # figure out root mid/rattr
            if rtree:
                r_rid, r_tag = min(rtree, key=lambda t: t.z).a
                _, (_, r_w, _) = rbyd.lookupnext(r_rid)
            else:
                r_rid, (r_tag, r_w, _) = rbyd.lookupnext(-1)

            if isinstance(mdir, Mdir):
                r_mid = mtree.mid(mdir.mid, r_rid)
            else:
                r_mid = mtree.mid(bid-(rbyd.weight-1)+r_rid-(r_w-1), -1)

            tree.add(cls.Branch(
                    (l_mid, len(path)-1, l_branch.tag),
                    (r_mid, len(path), r_tag),
                    d-1))

    # remap branches to leaves if we aren't showing inner branches
    if not inner:
        # step through each btree layer backwards
        b_depth = max((t.a[1]+1 for t in tree), default=0)

        for d in reversed(range(len(mtree.mrootchain), b_depth-1)):
            # find mid ranges at this level
            mids = set()
            for t in tree:
                if t.b[1] == d:
                    mids.add(t.b[0])
            mids = sorted(mids)

            # find the best root for each mid range
            roots = {}
            for i in range(len(mids)):
                for t in tree:
                    if (t.a[1] > d
                            and t.a[0] >= mids[i]
                            and (i == len(mids)-1 or t.a[0] < mids[i+1])
                            and (mids[i] not in roots
                                or t < roots[mids[i]])):
                        roots[mids[i]] = t

            # remap branches to leaf-roots
            tree = {t.map(
                        lambda x: x[1] == d and x[0] in roots,
                        lambda x: roots[x[0]].a)
                    for t in tree}

    return cls(tree)

# render an mtree tree for debugging
@classmethod
def _treeartfrommtreebtree(cls, mtree, *,
        depth=None,
        inner=False,
        **args):
    tree = set()
    root = None
    branches = {}
    for mid, mdir, name, path in mtree.mids(
            mdirs_only=False,
            path=True,
            depth=depth):
        # create branch for each jump in path
        #
        # note we adjust our mid/bid to be left-leaning, this allows
        # a global order and makes tree rendering quite a bit easier
        #
        # we also need to give btree nodes mrid=-1 so they come
        # before and mrid=-1 mdir attrs
        a = root
        for d, p in enumerate(path):
            # map into our mtree space
            if isinstance(p[1], Mdir):
                mid_, mdir_, name_ = p
            else:
                bid_, rbyd_, rid_, name_ = p
                mid_ = mtree.mid(bid_-(name_.weight-1), -1)
            b = (mid_, d, name_.tag)

            # remap branches to leaves if we aren't showing inner
            # branches
            if not inner:
                if b not in branches:
                    if isinstance(path[-1][1], Mdir):
                        mid_, mdir_, name_ = path[-1]
                    else:
                        bid_, rbyd_, rid_, name_ = path[-1]
                        mid_ = mtree.mid(bid_-(name_.weight-1), -1)
                    branches[b] = (mid_, len(path)-1, name_.tag)
                b = branches[b]

            # render the root path on first rid, this is arbitrary
            if root is None:
                root, a = b, b

            tree.add(cls.Branch(a, b, d))
            a = b

    return cls(tree)

# render an mtree tree for debugging
@classmethod
def treeartfrommtree(cls, mtree, **args):
    if args.get('tree_btree'):
        return cls._frommtreebtree(mtree, **args)
    else:
        return cls._frommtreertree(mtree, **args)

TreeArt._frommtreertree = _treeartfrommtreertree
TreeArt._frommtreebtree = _treeartfrommtreebtree
TreeArt.frommtree = treeartfrommtree



def main(disk, mroots=None, *,
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

        # fetch the mtree
        bd = Bd(f, block_size, block_count)
        mtree = Mtree.fetch(bd, mroots, trunk,
                depth=args.get('depth'))

        # print some information about the mtree
        if not quiet:
            print('mtree %s w%s.%s, rev %08x, cksum %08x' % (
                    mtree.addr(),
                    mtree.mbweightrepr(), mtree.mrweightrepr(),
                    mtree.rev,
                    mtree.cksum))

        # precompute tree renderings
        t_width = 0
        if (args.get('tree')
                or args.get('tree_rbyd')
                or args.get('tree_btree')):
            treeart = TreeArt.frommtree(mtree, **args)
            t_width = treeart.width

        # dynamically size the id field
        w_width = max(
                mt.ceil(mt.log10(max(1, mtree.mbweight >> mtree.mbits)+1)),
                mt.ceil(mt.log10(max(1, max(
                    mdir.weight for mdir in mtree.mdirs(
                        depth=args.get('depth')))))+1),
                # in case of -1.-1
                2)

        # pmdir keeps track of the last rendered mdir/rbyd, we update
        # this in dbg_mdir/dbg_branch to always print interleaved
        # addresses
        pmdir = None
        def dbg_mdir(d, mdir):
            nonlocal pmdir

            # show human-readable tag representation
            for i, (mid, rattr) in enumerate(mdir.rattrs()):
                print('%12s %s%s' % (
                        '{%s}:' % ','.join('%04x' % block
                                for block in mdir.blocks)
                            if not isinstance(pmdir, Mdir) or mdir != pmdir
                            else '',
                        treeart.repr((mid, d, rattr.tag), color)
                            if args.get('tree')
                                or args.get('tree_rbyd')
                                or args.get('tree_btree')
                            else '',
                        '%*s %-*s%s' % (
                            2*w_width+1, '%d.%d-%d' % (
                                    mid.mbid >> mtree.mbits,
                                    mid.mrid-(rattr.weight-1),
                                    mid.mrid)
                                if rattr.weight > 1
                                else '%d.%d' % (
                                    mid.mbid >> mtree.mbits,
                                    mid.mrid)
                                if rattr.weight > 0 or i == 0
                                else '',
                            21+w_width, rattr.repr(),
                            '  %s' % next(xxd(rattr.data, 8), '')
                                if not args.get('raw')
                                    and not args.get('no_truncate')
                                else '')))
                pmdir = mdir

                # show on-disk encoding of tags
                if args.get('raw'):
                    for o, line in enumerate(xxd(rattr.tdata)):
                        print('%11s: %*s%*s %s' % (
                                '%04x' % (rattr.toff + o*16),
                                t_width, '',
                                2*w_width+1, '',
                                line))
                if args.get('raw') or args.get('no_truncate'):
                    for o, line in enumerate(xxd(rattr.data)):
                        print('%11s: %*s%*s %s' % (
                                '%04x' % (rattr.off + o*16),
                                t_width, '',
                                2*w_width+1, '',
                                line))

        def dbg_branch(d, bid, rbyd, rid, name):
            nonlocal pmdir

            # show human-readable representation
            for rattr in rbyd.rattrs(rid):
                print('%12s %s%*s %-*s  %s' % (
                        '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                            if not isinstance(pmdir, Rbyd) or rbyd != pmdir
                            else '',
                        treeart.repr(
                                (mtree.mid(bid-(name.weight-1), -1),
                                    d, rattr.tag),
                                color)
                            if args.get('tree')
                                or args.get('tree_rbyd')
                                or args.get('tree_btree')
                            else '',
                        2*w_width+1, '%d-%d' % (
                                (bid-(rattr.weight-1)) >> mtree.mbits,
                                bid >> mtree.mbits)
                            if (rattr.weight >> mtree.mbits) > 1
                            else bid >> mtree.mbits if rattr.weight > 0
                            else '',
                        21+w_width, rattr.repr(),
                        next(xxd(rattr.data, 8), '')
                            if not args.get('raw')
                                and not args.get('no_truncate')
                            else ''))
                pmdir = rbyd

                # show on-disk encoding of tags/data
                if args.get('raw'):
                    for o, line in enumerate(xxd(
                            rbyd.data[rattr.toff:rattr.off])):
                        print('%11s: %*s%*s %s' % (
                                '%04x' % (rattr.toff + o*16),
                                t_width, '',
                                2*w_width+1, '',
                                line))
                if args.get('raw') or args.get('no_truncate'):
                    for o, line in enumerate(xxd(rattr.data)):
                        print('%11s: %*s%*s %s' % (
                                '%04x' % (rattr.off + o*16),
                                t_width, '',
                                2*w_width+1, '',
                                line))

        # traverse and print entries
        prbyd = None
        ppath = []
        mrootseen = set()
        corrupted = False
        for mdir, path in mtree.leaves(
                path=True,
                depth=args.get('depth')):
            # print inner branches if requested
            if args.get('inner') and not quiet:
                for d, (bid_, rbyd_, rid_, name_) in pathdelta(
                        # skip the mrootchain
                        path[len(mtree.mrootchain):],
                        ppath[len(mtree.mrootchain):]):
                    dbg_branch(len(mtree.mrootchain)+d,
                            bid_, rbyd_, rid_, name_)
            ppath = path

            # mdir?
            if isinstance(mdir, Mdir):
                # corrupted?
                if not mdir:
                    if not quiet:
                        print('%s{%s}: %s%s' % (
                                '\x1b[31m' if color else '',
                                ','.join('%04x' % block
                                    for block in mdir.blocks),
                                '(corrupted %s %s)' % (
                                    'mroot' if mdir.mid == -1 else 'mdir',
                                    mdir.addr()),
                                '\x1b[m' if color else ''))
                    pmdir = None
                    corrupted = True
                    continue

                # cycle detected?
                if mdir.mid == -1:
                    if mdir in mrootseen:
                        if not quiet:
                            print('%s{%s}: %s%s' % (
                                    '\x1b[31m' if color else '',
                                    ','.join('%04x' % block
                                        for block in mdir.blocks),
                                    '(mroot cycle detected %s)' % mdir.addr(),
                                    '\x1b[m' if color else ''))
                        pmdir = None
                        corrupted = True
                        continue
                    mrootseen.add(mdir)

                # show the mdir
                if not quiet:
                    dbg_mdir(len(path), mdir)

            # btree node?
            else:
                bid, rbyd = mdir
                # corrupted? try to keep printing the tree
                if not rbyd:
                    if not quiet:
                        print('%s%11s: %*s%s%s' % (
                                '\x1b[31m' if color else '',
                                '%04x.%04x' % (rbyd.block, rbyd.trunk),
                                t_width, '',
                                '(corrupted rbyd %s)' % rbyd.addr(),
                                '\x1b[m' if color else ''))
                    pmdir = None
                    corrupted = True
                    continue

                if not quiet:
                    for rid, name in rbyd.rids():
                        bid_ = bid-(rbyd.weight-1) + rid
                        # show the leaf entry/branch
                        dbg_branch(len(path), bid_, rbyd, rid, name)

    if args.get('error_on_corrupt') and corrupted:
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Debug littlefs's metadata tree.",
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
            '-q', '--quiet',
            action='store_true',
            help="Don't show anything, useful when checking for errors.")
    parser.add_argument(
            '--color',
            choices=['never', 'always', 'auto'],
            default='auto',
            help="When to use terminal colors. Defaults to 'auto'.")
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
            '-z', '--depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of mtree to show.")
    parser.add_argument(
            '-e', '--error-on-corrupt',
            action='store_true',
            help="Error if the filesystem is corrupt.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
