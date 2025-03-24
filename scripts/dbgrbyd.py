#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import bisect
import collections as co
import itertools as it
import math as mt
import os
import struct


COLORS = [
    '34',   # blue
    '31',   # red
    '32',   # green
    '35',   # purple
    '33',   # yellow
    '36',   # cyan
]


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

def crc32c(data, crc=0):
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

def xxd(data, width=16):
    for i in range(0, len(data), width):
        yield '%-*s %-*s' % (
                3*width,
                ' '.join('%02x' % b for b in data[i:i+width]),
                width,
                ''.join(
                    b if b >= ' ' and b <= '~' else '.'
                        for b in map(chr, data[i:i+width])))

def tagrepr(tag, weight=None, size=None, off=None):
    if (tag & 0x6fff) == TAG_NULL:
        return '%snull%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                ' w%d' % weight if weight else '',
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
                    else 'namelimit' if (tag & 0xfff) == TAG_NAMELIMIT
                    else 'filelimit' if (tag & 0xfff) == TAG_FILELIMIT
                    else 'config 0x%02x' % (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    elif (tag & 0x6f00) == TAG_GDELTA:
        return '%s%s%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                'grmdelta' if (tag & 0xfff) == TAG_GRMDELTA
                    else 'gdelta 0x%02x' % (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
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
    elif (tag & 0x6e00) == TAG_ATTR:
        return '%s%sattr 0x%02x%s%s' % (
                'shrub' if tag & TAG_SHRUB else '',
                's' if tag & 0x100 else 'u',
                ((tag & 0x100) >> 1) ^ (tag & 0xff),
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    elif tag & TAG_ALT:
        return 'alt%s%s 0x%03x%s%s' % (
                'r' if tag & TAG_R else 'b',
                'gt' if tag & TAG_GT else 'le',
                tag & 0x0fff,
                ' w%d' % weight if weight is not None else '',
                ' 0x%x' % (0xffffffff & (off-size))
                    if size and off is not None
                    else ' -%d' % size if size
                    else '')
    elif (tag & 0x7f00) == TAG_CKSUM:
        return 'cksum%s%s%s%s' % (
                'p' if not tag & 0xfe and tag & TAG_P else '',
                ' 0x%02x' % (tag & 0xff) if tag & 0xfe else '',
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_NOTE:
        return 'note%s%s%s' % (
                ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_ECKSUM:
        return 'ecksum%s%s%s' % (
                ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_GCKSUMDELTA:
        return 'gcksumdelta%s%s%s' % (
                ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
                ' w%d' % weight if weight else '',
                ' %s' % size if size is not None else '')
    else:
        return '0x%04x%s%s' % (
                tag,
                ' w%d' % weight if weight is not None else '',
                ' %d' % size if size is not None else '')

# tree branches are an abstract thing for tree rendering
class TreeBranch(co.namedtuple('TreeBranch', ['a', 'b', 'depth', 'color'])):
    __slots__ = ()
    def __new__(cls, a, b, depth=0, color='b'):
        # a and b are context specific
        return super().__new__(cls, a, b, depth, color)

    def __repr__(self):
        return '%s(%s, %s, %s, %s)' % (
                self.__class__.__name__,
                self.a,
                self.b,
                self.depth,
                self.color)

def treerepr(tree, x, depth=None, color=False):
    # find the max depth from the tree
    if depth is None:
        depth = max((t.depth+1 for t in tree), default=0)
    if depth == 0:
        return ''

    def branchrepr(tree, x, d, was):
        for t in tree:
            if t.depth == d and t.b == x:
                if any(t.depth == d and t.a == x
                        for t in tree):
                    return '+-', t.color, t.color
                elif any(t.depth == d
                            and x > min(t.a, t.b)
                            and x < max(t.a, t.b)
                        for t in tree):
                    return '|-', t.color, t.color
                elif t.a < t.b:
                    return '\'-', t.color, t.color
                else:
                    return '.-', t.color, t.color
        for t in tree:
            if t.depth == d and t.a == x:
                return '+ ', t.color, None
        for t in tree:
            if (t.depth == d
                    and x > min(t.a, t.b)
                    and x < max(t.a, t.b)):
                return '| ', t.color, was
        if was:
            return '--', was, was
        return '  ', None, None

    trunk = []
    was = None
    for d in range(depth):
        t, c, was = branchrepr(tree, x, d, was)

        trunk.append('%s%s%s%s' % (
                '\x1b[33m' if color and c == 'y'
                    else '\x1b[31m' if color and c == 'r'
                    else '\x1b[90m' if color and c == 'b'
                    else '',
                t,
                ('>' if was else ' ') if d == depth-1 else '',
                '\x1b[m' if color and c else ''))

    return '%s ' % ''.join(trunk)


# a simple wrapper over an open file with bd geometry
class Bd:
    def __init__(self, f, block_size=None, block_count=None):
        self.f = f
        self.block_size = block_size
        self.block_count = block_count

    def __repr__(self):
        return '<%s %sx%s>' % (
                self.__class__.__name__,
                self.block_size,
                self.block_count)

    def read(self, size=-1):
        return self.f.read(size)

    def seek(self, block, off, whence=0):
        pos = self.f.seek(block*self.block_size + off, whence)
        return pos // block_size, pos % block_size

    def readblock(self, block):
        self.f.seek(block*self.block_size)
        return self.f.read(self.block_size)

# tagged data in an rbyd
class Rattr:
    def __init__(self, tag, weight, block, toff, off, data):
        self.tag = tag
        self.weight = weight
        self.block = block
        self.toff = toff
        self.off = off
        self.data = data

    @property
    def size(self):
        return len(self.data)

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self)

    def __str__(self):
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

class Ralt:
    def __init__(self, tag, weight, block, toff, off, jump,
            color=None, followed=None):
        self.tag = tag
        self.weight = weight
        self.block = block
        self.toff = toff
        self.off = off
        self.jump = jump

        if color is not None:
            self.color = color
        else:
            self.color = 'r' if tag & TAG_R else 'b'
        self.followed = followed

    @property
    def joff(self):
        return self.toff - self.jump

    def __repr__(self):
        return '<%s %s>' % (self.__class__.__name__, self)

    def __str__(self):
        return tagrepr(self.tag, self.weight, self.jump, self.toff)

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
    def __init__(self, data, blocks, trunk, weight, rev, eoff, cksum, *,
            gcksumdelta=None,
            corrupt=False):
        if isinstance(blocks, int):
            blocks = [blocks]

        self.data = data
        self.blocks = list(blocks)
        self.trunk = trunk
        self.weight = weight
        self.rev = rev
        self.eoff = eoff
        self.cksum = cksum
        self.gcksumdelta = gcksumdelta
        self.corrupt = corrupt

    @property
    def block(self):
        return self.blocks[0]

    def addr(self):
        if len(self.blocks) == 1:
            return '0x%x.%x' % (self.block, self.trunk)
        else:
            return '0x{%s}.%x' % (
                    ','.join('%x' % block for block in self.blocks),
                    self.trunk)

    def __repr__(self):
        return '<%s %s w%s>' % (
                self.__class__.__name__,
                self.addr(),
                self.weight)

    def __bool__(self):
        return not self.corrupt

    def __eq__(self, other):
        return ((frozenset(self.blocks), self.trunk)
                == (frozenset(other.blocks), other.trunk))

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((frozenset(self.blocks), self.trunk))

    @classmethod
    def fetch(cls, bd, blocks, trunk=None, cksum=None):
        # multiple blocks? unfortunately this must be a list
        if isinstance(blocks, list):
            # fetch all blocks
            rbyds = [cls.fetch(bd, block, trunk, cksum) for block in blocks]
            # determine most recent revision
            i = 0
            for i_, rbyd in enumerate(rbyds):
                # compare with sequence arithmetic
                if rbyd and (
                        not rbyds[i]
                            or not ((rbyd.rev - rbyds[i].rev) & 0x80000000)
                            or (rbyd.rev == rbyds[i].rev
                                and rbyd.trunk > rbyds[i].trunk)):
                    i = i_
            # keep track of the other blocks
            rbyd = rbyds[i]
            rbyd.blocks += tuple(
                    rbyds[(i+1+j) % len(rbyds)].block
                        for j in range(len(rbyds)-1))
            return rbyd

        block = blocks

        # blocks may also encode trunks
        block, trunk = (
                block[0] if isinstance(block, tuple)
                    else block,
                trunk if trunk is not None
                    else block[1] if isinstance(block, tuple)
                    else None)

        # bd can be either a bd reference or preread data
        #
        # preread data can be useful for avoiding race conditions
        # with cksums and shrubs
        if isinstance(bd, Bd):
            # seek/read the block
            data = bd.readblock(block)
        else:
            data = bd

        # fetch the rbyd
        rev = fromle32(data[0:4])
        cksum_ = 0
        cksum__ = crc32c(data[0:4])
        cksum___ = cksum__
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
            if v != parity(cksum___):
                break
            cksum___ ^= 0x00000080 if v else 0
            cksum___ = crc32c(data[j_:j_+d], cksum___)
            j_ += d
            if not tag & TAG_ALT and j_ + size > len(data):
                break

            # take care of cksums
            if not tag & TAG_ALT:
                if (tag & 0xff00) != TAG_CKSUM:
                    cksum___ = crc32c(data[j_:j_+size], cksum___)

                    # found a gcksumdelta?
                    if (tag & 0xff00) == TAG_GCKSUMDELTA:
                        gcksumdelta_ = Rattr(tag, w,
                                block, j_-d, d, data[j_:j_+size])

                # found a cksum?
                else:
                    # check cksum
                    cksum____ = fromle32(data[j_:j_+4])
                    if cksum___ != cksum____:
                        break
                    # commit what we have
                    eoff = eoff_ if eoff_ else j_ + size
                    cksum_ = cksum__
                    trunk_ = trunk__
                    weight = weight_
                    gcksumdelta = gcksumdelta_
                    gcksumdelta_ = None
                    # update perturb bit
                    perturb = tag & TAG_P
                    # revert to data cksum and perturb
                    cksum___ = cksum__ ^ (0xfca42daf if perturb else 0)

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
                                cksum_ = cksum___ ^ (
                                        0xfca42daf if perturb else 0)
                                trunk_ = trunk__
                                weight = weight_
                                gcksumdelta = gcksumdelta_
                        trunk___ = 0

                # update canonical checksum, xoring out any perturb state
                cksum__ = cksum___ ^ (0xfca42daf if perturb else 0)

            if not tag & TAG_ALT:
                j_ += size

        # cksum mismatch?
        if cksum is not None and cksum_ != cksum:
            return cls(data, block, 0, 0, rev, 0, cksum_,
                    corrupt=True)

        return cls(data, block, trunk_, weight, rev, eoff, cksum_,
                gcksumdelta=gcksumdelta,
                corrupt=not trunk_)

    def lookupnext(self, rid, tag=None, *,
            path=False):
        if not self:
            return None, None, *(([],) if path else ())

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
                                alt, w, self.block, j+jump, j+jump+d, jump,
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
                                alt, w, self.block, j-d, j, jump,
                                color=color,
                                followed=False))

            # found tag
            else:
                rid_ = upper-1
                tag_ = alt
                w_ = upper-lower

                if not tag_ or (rid_, tag_) < (rid, tag):
                    return None, None, *(([],) if path else ())

                return (rid_,
                        Rattr(tag_, w_, self.block, j, j+d,
                            self.data[j+d:j+d+jump]),
                        *((path_,) if path else ()))

    def lookup_(self, rid, tag=None, mask=None, *,
            path=False):
        if tag is None:
            tag, mask = 0, 0xffff
        if mask is None:
            mask = 0

        rid_, rattr_, *path_ = self.lookupnext(rid, tag & ~mask,
                path=path)
        if (rid_ is None
                or rid_ != rid
                or (rattr_.tag & ~mask) != (tag & ~mask)):
            return None, *path_

        return rattr_, *path_

    def lookup(self, rid, tag=None, mask=None, *,
            path=False):
        rattr_, *path_ = self.lookup_(rid, tag, mask,
                path=path)
        if path:
            return rattr_, *path_
        else:
            return rattr_

    def __getitem__(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        return self.lookup(*key)

    def __contains__(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        return self.lookup_(*key)[0] is not None

    def rids(self, *,
            path=False):
        rid = -1
        while True:
            rid, name, *path_ = self.lookupnext(rid,
                    path=path)
            # found end of tree?
            if rid is None:
                break

            yield rid, name, *path_
            rid += 1

    def rattrs_(self, rid=None, *,
            path=False):
        if rid is None:
            rid, tag = -1, 0
            while True:
                rid, rattr, *path_ = self.lookupnext(rid, tag+0x1,
                        path=path)
                # found end of tree?
                if rid is None:
                    break

                yield rid, rattr, *path_
                tag = rattr.tag
        else:
            tag = 0
            while True:
                rid_, rattr, *path_ = self.lookupnext(rid, tag+0x1,
                        path=path)
                # found end of tree?
                if rid_ is None or rid_ != rid:
                    break

                yield rattr, *path_
                tag = rattr.tag

    def rattrs(self, rid=None, *,
            path=False):
        if rid is None:
            yield from self.rattrs_(rid,
                    path=path)
        else:
            for rattr, *path_ in self.rattrs_(rid,
                    path=path):
                if path:
                    yield rattr, *path_
                else:
                    yield rattr

    def __iter__(self):
        return self.rattrs()

    # lookup by name
    def namelookup(self, did, name):
        # binary search
        best = (False, None, None, None, None)
        lower = 0
        upper = self.weight
        while lower < upper:
            rid, rattr = self.lookupnext(lower + (upper-1-lower)//2)
            if rid is None:
                break

            # treat vestigial names as a catch-all
            if ((rattr.tag == TAG_NAME and rid-(rattr.weight-1) == 0)
                    or (rattr.tag & 0xff00) != TAG_NAME):
                did_ = 0
                name_ = b''
            else:
                did_, d = fromleb128(rattr.data)
                name_ = rattr.data[d:]

            # bisect search space
            if (did_, name_) > (did, name):
                upper = rid-(w-1)
            elif (did_, name_) < (did, name):
                lower = rid + 1
                # keep track of best match
                best = (False, rid, rattr)
            else:
                # found a match
                return True, rid, rattr

        return best

    # create an rbyd tree for debugging
    def _tree_rtree(self, **args):
        trunks = co.defaultdict(lambda: (-1, 0))
        alts = co.defaultdict(lambda: {})

        for rid, rattr, path in self.rattrs(path=True):
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
            tree.add(TreeBranch(
                    alt['nft'],
                    alt['nft'],
                    t_depth-1 - alt['h'],
                    alt['c']))
            if alt['ft'] != alt['nft']:
                tree.add(TreeBranch(
                        alt['nft'],
                        alt['ft'],
                        t_depth-1 - alt['h'],
                        'b'))

        return tree

    # create a btree tree for debugging
    def _tree_btree(self, **args):
        # for rbyds this is just a pointer to ever rid
        tree = set()
        root = None
        for rid, name in self.rids():
            b = (rid, name.tag)
            if root is None:
                root = b
            tree.add(TreeBranch(root, b))
        return tree

    # create tree representation for debugging
    def tree(self, **args):
        if args.get('tree_btree'):
            return self._tree_btree(**args)
        else:
            return self._tree_rtree(**args)


def dbg_log(rbyd, *,
        block_size,
        color=False,
        **args):
    data = rbyd.data

    # preprocess jumps
    if args.get('jumps'):
        jumps = []
        j_ = 4
        while j_ < (block_size if args.get('all') else rbyd.eoff):
            j = j_
            v, tag, w, size, d = fromtag(data[j_:])
            j_ += d
            if not tag & TAG_ALT:
                j_ += size

            if tag & TAG_ALT and size:
                # figure out which alt color
                if tag & TAG_R:
                    _, ntag, _, _, _ = fromtag(data[j_:])
                    if ntag & TAG_R:
                        jumps.append((j, j-size, 0, 'y'))
                    else:
                        jumps.append((j, j-size, 0, 'r'))
                else:
                    jumps.append((j, j-size, 0, 'b'))

        # figure out x-offsets to avoid collisions between jumps
        for j in range(len(jumps)):
            a, b, _, c = jumps[j]
            x = 0
            while any(
                    max(a, b) >= min(a_, b_)
                            and max(a_, b_) >= min(a, b)
                            and x == x_
                        for a_, b_, x_, _ in jumps[:j]):
                x += 1
            jumps[j] = a, b, x, c

        def jumprepr(j):
            # render jumps
            chars = {}
            for a, b, x, c in jumps:
                c_start = (
                    '\x1b[33m' if color and c == 'y'
                        else '\x1b[31m' if color and c == 'r'
                        else '\x1b[90m' if color
                        else '')
                c_stop = '\x1b[m' if color else ''

                if j == a:
                    for x_ in range(2*x+1):
                        chars[x_] = '%s-%s' % (c_start, c_stop)
                    chars[2*x+1] = '%s\'%s' % (c_start, c_stop)
                elif j == b:
                    for x_ in range(2*x+1):
                        chars[x_] = '%s-%s' % (c_start, c_stop)
                    chars[2*x+1] = '%s.%s' % (c_start, c_stop)
                    chars[0] = '%s<%s' % (c_start, c_stop)
                elif j >= min(a, b) and j <= max(a, b):
                    chars[2*x+1] = '%s|%s' % (c_start, c_stop)

            return ''.join(chars.get(x, ' ')
                    for x in range(max(chars.keys(), default=0)+1))

    # preprocess lifetimes
    lifetime_width = 0
    if args.get('lifetimes'):
        class Lifetime:
            color_i = 0
            def __init__(self, j):
                self.origin = j
                self.tags = set()
                self.color = COLORS[self.__class__.color_i]
                self.__class__.color_i = (
                        self.__class__.color_i + 1) % len(COLORS)

            def add(self, j):
                self.tags.add(j)

            def __bool__(self):
                return bool(self.tags)


        # first figure out where each rid comes from
        weights = []
        lifetimes = []
        def index(weights, rid):
            for i, w in enumerate(weights):
                if rid < w:
                    return i, rid
                rid -= w
            return len(weights), 0

        checkpoint_js = [0]
        checkpoints = [([], [], set(), set(), set())]
        def checkpoint(j, weights, lifetimes, grows, shrinks, tags):
            checkpoint_js.append(j)
            checkpoints.append((
                    weights.copy(), lifetimes.copy(),
                    grows, shrinks, tags))

        lower_, upper_ = 0, 0
        weight_ = 0
        trunk_ = 0
        j_ = 4
        while j_ < (block_size if args.get('all') else rbyd.eoff):
            j = j_
            v, tag, w, size, d = fromtag(data[j_:])
            j_ += d
            if not tag & TAG_ALT:
                j_ += size

            # evaluate trunks
            if (tag & 0xf000) != TAG_CKSUM:
                if not trunk_:
                    trunk_ = j_-d
                    lower_, upper_ = 0, 0

                if tag & TAG_ALT and not tag & TAG_GT:
                    lower_ += w
                else:
                    upper_ += w

                if not tag & TAG_ALT:
                    # derive the current tag's rid from alt weights
                    delta = (lower_+upper_) - weight_
                    weight_ = lower_+upper_
                    rid = lower_ + w-1
                    trunk_ = 0

            if (tag & 0xf000) != TAG_CKSUM and not tag & TAG_ALT:
                # note we ignore out-of-bounds here for debugging
                if delta > 0:
                    # grow lifetimes
                    i, rid_ = index(weights, lower_)
                    if rid_ > 0:
                        weights[i:i+1] = [rid_, delta, weights[i]-rid_]
                        lifetimes[i:i+1] = [
                                lifetimes[i], Lifetime(j), lifetimes[i]]
                    else:
                        weights[i:i] = [delta]
                        lifetimes[i:i] = [Lifetime(j)]

                    checkpoint(j, weights, lifetimes, {i}, set(), {i})

                elif delta < 0:
                    # shrink lifetimes
                    i, rid_ = index(weights, lower_)
                    delta_ = -delta
                    weights_ = weights.copy()
                    lifetimes_ = lifetimes.copy()
                    shrinks = set()
                    while delta_ > 0 and i < len(weights_):
                        if weights_[i] > delta_:
                            delta__ = min(delta_, weights_[i]-rid_)
                            delta_ -= delta__
                            weights_[i] -= delta__
                            i += 1
                            rid_ = 0
                        else:
                            delta_ -= weights_[i]
                            weights_[i:i+1] = []
                            lifetimes_[i:i+1] = []
                            shrinks.add(i + len(shrinks))

                    checkpoint(j, weights, lifetimes, set(), shrinks, {i})
                    weights = weights_
                    lifetimes = lifetimes_

                if rid >= 0:
                    # attach tag to lifetime
                    i, rid_ = index(weights, rid)
                    if i < len(weights):
                        lifetimes[i].add(j)

                    if delta == 0:
                        checkpoint(j, weights, lifetimes, set(), set(), {i})

        lifetime_width = 2*max((
                sum(1 for lifetime in lifetimes if lifetime)
                    for _, lifetimes, _, _, _ in checkpoints),
                default=0)

        def lifetimerepr(j):
            x = bisect.bisect(checkpoint_js, j)-1
            j_ = checkpoint_js[x]
            weights, lifetimes, grows, shrinks, tags = checkpoints[x]

            reprs = []
            colors = []
            was = None
            for i, (w, lifetime) in enumerate(zip(weights, lifetimes)):
                # skip lifetimes with no tags and shrinks
                if not lifetime or (j != j_ and i in shrinks):
                    if i in grows or i in shrinks or i in tags:
                        tags = tags.copy()
                        tags.add(i+1)
                    continue

                if j == j_ and i in grows:
                    reprs.append('.')
                    was = 'grow'
                elif j == j_ and i in shrinks:
                    reprs.append('\'')
                    was = 'shrink'
                elif j == j_ and i in tags:
                    reprs.append('* ')
                elif was == 'grow':
                    reprs.append('\\ ')
                elif was == 'shrink':
                    reprs.append('/ ')
                else:
                    reprs.append('| ')

                colors.append(lifetime.color)

            return '%s%*s' % (
                    ''.join('%s%s%s' % (
                            '\x1b[%sm' % c if color else '',
                            r,
                            '\x1b[m' if color else '')
                        for r, c in zip(reprs, colors)),
                    lifetime_width - sum(len(r) for r in reprs), '')


    # dynamically size the id field
    #
    # we need to do an additional pass to find this since our rbyd weight
    # does not include any shrub trees
    weight_ = 0
    weight__ = 0
    trunk_ = 0
    j_ = 4
    while j_ < (block_size if args.get('all') else rbyd.eoff):
        j = j_
        v, tag, w, size, d = fromtag(data[j_:])
        j_ += d

        if not tag & TAG_ALT:
            j_ += size

        # evaluate trunks
        if (tag & 0xf000) != TAG_CKSUM:
            if not trunk_:
                trunk_ = j_-d
                weight__ = 0

            weight__ += w

            if not tag & TAG_ALT:
                # found new weight?
                weight_ = max(weight_, weight__)
                trunk_ = 0

    w_width = mt.ceil(mt.log10(max(1, weight_)+1))

    # print revision count
    if args.get('raw'):
        print('%8s: %*s%*s %s' % (
                '%04x' % 0,
                lifetime_width, '',
                2*w_width+1, '',
                next(xxd(data[0:4]))))

    # print tags
    cksum = crc32c(data[0:4])
    cksum_ = cksum
    perturb = False
    lower_, upper_ = 0, 0
    trunk_ = 0
    j_ = 4
    while j_ < (block_size if args.get('all') else rbyd.eoff):
        notes = []

        # read next tag
        j = j_
        v, tag, w, size, d = fromtag(data[j_:])
        if v != parity(cksum_):
            notes.append('v!=%x' % parity(cksum_))
        cksum_ ^= 0x00000080 if v else 0
        cksum_ = crc32c(data[j_:j_+d], cksum_)
        j_ += d

        # take care of cksums
        if not tag & TAG_ALT:
            if (tag & 0xff00) != TAG_CKSUM:
                cksum_ = crc32c(data[j_:j_+size], cksum_)
            # found a cksum?
            else:
                # check cksum
                cksum__ = fromle32(data[j_:j_+4])
                if cksum_ != cksum__:
                    notes.append('cksum!=%08x' % cksum__)
                # update perturb bit
                perturb = tag & TAG_P
                # revert to data cksum and perturb
                cksum_ = cksum ^ (0xfca42daf if perturb else 0)
            j_ += size

        # evaluate trunks
        if (tag & 0xf000) != TAG_CKSUM:
            if not trunk_:
                trunk_ = j_-d
                lower_, upper_ = 0, 0

            if tag & TAG_ALT and not tag & TAG_GT:
                lower_ += w
            else:
                upper_ += w

            # end of trunk?
            if not tag & TAG_ALT:
                # derive the current tag's rid from alt weights
                rid = lower_ + w-1
                trunk_ = 0

            # update canonical checksum, xoring out any perturb state
            cksum = cksum_ ^ (0xfca42daf if perturb else 0)

        # show human-readable tag representation
        print('%s%08x:%s %*s%s%*s %-*s%s%s%s' % (
                '\x1b[90m' if color and j >= rbyd.eoff else '',
                j,
                '\x1b[m' if color and j >= rbyd.eoff else '',
                lifetime_width, lifetimerepr(j)
                    if args.get('lifetimes')
                    else '',
                '\x1b[90m' if color and j >= rbyd.eoff else '',
                2*w_width+1, '' if (tag & 0xe000) != 0x0000
                    else '%d-%d' % (rid-(w-1), rid) if w > 1
                    else rid,
                56+w_width, '%-*s  %s' % (
                    21+w_width, tagrepr(tag, w, size, j),
                    next(xxd(data[j+d:j+d+min(size, 8)], 8), '')
                        if not args.get('raw')
                            and not args.get('no_truncate')
                            and not tag & TAG_ALT
                        else ''),
                ' (%s)' % ', '.join(notes) if notes else '',
                '\x1b[m' if color and j >= rbyd.eoff else '',
                ' %s' % jumprepr(j)
                    if args.get('jumps') and not notes
                    else ''))

        # show on-disk encoding of tags
        if args.get('raw'):
            for o, line in enumerate(xxd(data[j:j+d])):
                print('%s%8s: %*s%*s %s%s' % (
                        '\x1b[90m' if color and j >= rbyd.eoff else '',
                        '%04x' % (j + o*16),
                        lifetime_width, '',
                        2*w_width+1, '',
                        line,
                        '\x1b[m' if color and j >= rbyd.eoff else ''))
        if args.get('raw') or args.get('no_truncate'):
            if not tag & TAG_ALT:
                for o, line in enumerate(xxd(data[j+d:j+d+size])):
                    print('%s%8s: %*s%*s %s%s' % (
                            '\x1b[90m' if color and j >= rbyd.eoff else '',
                            '%04x' % (j+d + o*16),
                            lifetime_width, '',
                            2*w_width+1, '',
                            line,
                            '\x1b[m' if color and j >= rbyd.eoff else ''))


def dbg_tree(rbyd, *,
        block_size,
        color=False,
        **args):
    if not rbyd:
        return

    data = rbyd.data

    # precompute tree renderings
    t_width = 0
    if (args.get('tree')
            or args.get('tree_rbyd')
            or args.get('tree_btree')):
        tree = rbyd.tree(**args)

        # find the max depth from the tree
        t_depth = max((t.depth+1 for t in tree), default=0)
        if t_depth > 0:
            t_width = 2*t_depth + 2

    # dynamically size the id field
    w_width = mt.ceil(mt.log10(max(1, rbyd.weight)+1))

    for i, (rid, rattr) in enumerate(rbyd):
        # show human-readable tag representation
        print('%08x: %s%*s %-*s  %s' % (
                rattr.toff,
                treerepr(tree, (rid, rattr.tag), t_depth, color)
                    if (args.get('tree')
                        or args.get('tree_rbyd')
                        or args.get('tree_btree'))
                    else '',
                2*w_width+1, '%d-%d' % (rid-(rattr.weight-1), rid)
                    if rattr.weight > 1
                    else rid if rattr.weight > 0 or i == 0
                    else '',
                21+w_width, rattr,
                next(xxd(rattr.data[:8], 8), '')
                    if not args.get('raw')
                        and not args.get('no_truncate')
                        and not rattr.tag & TAG_ALT
                    else ''))

        # show on-disk encoding of tags
        if args.get('raw'):
            for o, line in enumerate(xxd(data[rattr.toff:rattr.off])):
                print('%8s: %*s%*s %s' % (
                        '%04x' % (rattr.toff + o*16),
                        t_width, '',
                        2*w_width+1, '',
                        line))
        if args.get('raw') or args.get('no_truncate'):
            if not rattr.tag & TAG_ALT:
                for o, line in enumerate(xxd(rattr.data)):
                    print('%8s: %*s%*s %s' % (
                            '%04x' % (rattr.off + o*16),
                            t_width, '',
                            2*w_width+1, '',
                            line))


def main(disk, blocks=None, *,
        block_size=None,
        block_count=None,
        trunk=None,
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

    # flatten blocks, default to block 0
    if not blocks:
        blocks = [(0,)]
    blocks = [block for blocks_ in blocks for block in blocks_]

    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # fetch the rbyd
        bd = Bd(f, block_size, block_count)
        rbyd = Rbyd.fetch(bd, blocks)

    # print some information about the rbyd
    print('rbyd %s w%d, rev %08x, size %d, cksum %08x' % (
            rbyd.addr(),
            rbyd.weight,
            rbyd.rev,
            rbyd.eoff,
            rbyd.cksum))

    if args.get('log'):
        dbg_log(rbyd,
                block_size=block_size,
                color=color,
                **args)
    else:
        dbg_tree(rbyd,
                block_size=block_size,
                color=color,
                **args)

    if args.get('error_on_corrupt') and not rbyd:
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Debug rbyd metadata.",
            allow_abbrev=False)
    parser.add_argument(
            'disk',
            help="File containing the block device.")
    parser.add_argument(
            'blocks',
            nargs='*',
            type=rbydaddr,
            help="Block address of metadata blocks.")
    parser.add_argument(
            '-b', '--block-size',
            type=bdgeom,
            help="Block size/geometry in bytes.")
    parser.add_argument(
            '--block-count',
            type=lambda x: int(x, 0),
            help="Block count in blocks.")
    parser.add_argument(
            '--trunk',
            type=lambda x: int(x, 0),
            help="Use this offset as the trunk of the tree.")
    parser.add_argument(
            '--color',
            choices=['never', 'always', 'auto'],
            default='auto',
            help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Don't stop parsing on bad commits.")
    parser.add_argument(
            '-l', '--log',
            action='store_true',
            help="Show the raw tags as they appear in the log.")
    parser.add_argument(
            '-r', '--raw',
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
            '-j', '--jumps',
            action='store_true',
            help="Show alt pointer jumps in the margin.")
    parser.add_argument(
            '-g', '--lifetimes',
            action='store_true',
            help="Show inserts/deletes of ids in the margin.")
    parser.add_argument(
            '-e', '--error-on-corrupt',
            action='store_true',
            help="Error if no valid commit is found.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
