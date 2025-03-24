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
TAG_CKSUM       = 0x3000    ## 0x300p  v-11 cccc ---- ---p
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

def frombranch(data):
    d = 0
    block, d_ = fromleb128(data[d:]); d += d_
    trunk, d_ = fromleb128(data[d:]); d += d_
    cksum = fromle32(data[d:]); d += 4
    return block, trunk, cksum

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

    # don't include color in branch comparisons, or else our tree
    # renderings can end up with inconsistent colors between runs
    def __eq__(self, other):
        return (self.a, self.b, self.depth) == (other.a, other.b, other.depth)

    def __ne__(self, other):
        return (self.a, self.b, self.depth) != (other.a, other.b, other.depth)

    def __hash__(self):
        return hash((self.a, self.b, self.depth))

    # also order by depth first, which can be useful for reproducibly
    # prioritizing branches when simplifying trees
    def __lt__(self, other):
        return (self.depth, self.a, self.b) < (other.depth, other.a, other.b)

    def __le__(self, other):
        return (self.depth, self.a, self.b) <= (other.depth, other.a, other.b)

    def __gt__(self, other):
        return (self.depth, self.a, self.b) > (other.depth, other.a, other.b)

    def __ge__(self, other):
        return (self.depth, self.a, self.b) >= (other.depth, other.a, other.b)

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

# compute the difference between two paths, returning everything
# in a after the paths diverge, as well as the relevant index
def pathdelta(a, b):
    if not isinstance(a, list):
        a = list(a)
    i = 0
    for a_, b_ in zip(a, b):
        if type(a_) == type(b_) and a_ == b_:
            i += 1
        else:
            break

    return [(i+j, a_) for j, a_ in enumerate(a[i:])]


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
    def eoff(self):
        return self.rbyd.eoff

    @property
    def cksum(self):
        return self.rbyd.cksum

    def addr(self):
        return self.rbyd.addr()

    def __repr__(self):
        return '<%s %s w%s>' % (
                self.__class__.__name__,
                self.addr(),
                self.weight)

    def __eq__(self, other):
        return self.rbyd == other.rbyd

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash(self.rbyd)

    @classmethod
    def fetch(cls, bd, blocks, trunk=None, cksum=None):
        # we need a real bd reference here
        assert isinstance(bd, Bd)

        rbyd = Rbyd.fetch(bd, blocks, trunk, cksum)
        return cls(bd, rbyd)

    def lookupleaf(self, bid, *,
            path=None,
            depth=None):
        rbyd = self.rbyd
        rid = bid
        depth_ = 1
        path_ = []

        while True:
            # corrupt branch?
            if not rbyd:
                return (bid, rbyd, rid, None,
                        *((path_,) if path else ()))

            # first tag indicates the branch's weight
            rid_, name_ = rbyd.lookupnext(rid)
            if rid_ is None:
                return (None, None, None, None,
                        *((path_,) if path else ()))

            # keep track of path
            if path:
                path_.append((bid + (rid_-rid), rbyd, rid_, name_))

            # find branch tag if there is one
            branch_ = rbyd.lookup(rid_, TAG_BRANCH, 0x3)

            # descend down branch?
            if branch_ is not None and (
                    not depth or depth_ < depth):
                block, trunk, cksum = frombranch(branch_.data)
                rbyd = Rbyd.fetch(self.bd, block, trunk, cksum)
                # keep track of expected trunk/weight if corrupted
                if not rbyd:
                    rbyd.trunk = trunk
                    rbyd.weight = name_.weight

                rid -= (rid_-(name_.weight-1))
                depth_ += 1

            else:
                return (bid + (rid_-rid), rbyd, rid_, name_,
                        *((path_,) if path else ()))

    def lookup(self, bid, tag=None, mask=None, *,
            path=False,
            depth=None):
        # lookup rbyd in btree
        bid_, rbyd_, rid_, name_, *path_ = self.lookupleaf(bid,
                path=path,
                depth=depth)
        if bid_ is None:
            return None, None, None, None, None, *path_

        # lookup tag in rbyd
        rattr_ = rbyd_.lookup(rid_, tag, mask)
        if rattr_ is None:
            return None, None, None, None, None, *path_

        return bid_, rbyd_, rid_, name_, rattr_, *path_

    def __getitem__(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        return self.lookup(*key)

    def __contains__(self, key):
        if not isinstance(key, tuple):
            key = (key,)

        return self.lookup(*key)[0] is not None

    # note leaves only iterates over leaf rbyds, whereas traverse
    # traverses all rbyds
    def leaves(self, *,
            path=False,
            depth=None):
        # include our root rbyd even if the weight is zero
        if self.weight == 0:
            yield -1, self.rbyd, *(([],) if path else())

        bid = 0
        while True:
            bid, rbyd, rid, name, *path_ = self.lookupleaf(bid,
                    path=path,
                    depth=depth)
            if bid is None:
                break

            yield (bid-rid + (rbyd.weight-1), rbyd,
                    *((path_[0][:-1],) if path else ()))
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
                yield bid_, rbyd_, *((path_[:d],) if path else ())
            ptrunk_ = trunk_

    def bids(self, *,
            path=False,
            depth=None):
        for bid, rbyd, *path_ in self.leaves(
                path=path,
                depth=depth):
            for rid, name in rbyd.rids():
                yield (bid-(rbyd.weight-1) + rid,
                        rbyd, rid, name,
                        *((path_[0]+[
                                (bid-(rbyd.weight-1) + rid,
                                    rbyd, rid, name)],)
                            if path else ()))

    def rattrs_(self, bid=None, *,
            path=False,
            depth=None):
        if bid is None:
            for bid, rbyd, *path_ in self.leaves(
                    path=path,
                    depth=depth):
                for rid, name in rbyd.rids():
                    for rattr in rbyd.rattrs(rid):
                        yield (bid-(rbyd.weight-1) + rid,
                                rbyd, rid, name, rattr,
                                *((path_[0]+[
                                        (bid-(rbyd.weight-1) + rid,
                                            rbyd, rid, name)],)
                                    if path else ()))
        else:
            bid, rbyd, rid, name, *path_ = self.lookupleaf(bid,
                    path=path,
                    depth=depth)
            for rattr in rbyd.rattrs(rid):
                yield rattr, *path_

    def rattrs(self, bid=None, *,
            path=False,
            depth=None):
        if bid is None:
            yield from self.rattrs_(bid,
                    path=path,
                    depth=depth)
        else:
            for rattr, *path_ in self.rattrs_(bid,
                    path=path,
                    depth=depth):
                if path:
                    yield rattr, *path_
                else:
                    yield rattr

    def __iter__(self):
        return self.rattrs()

    # lookup by name
    def namelookup(self, did, name, *,
            depth=None):
        rbyd = self.rbyd
        bid = 0
        depth_ = 1

        while True:
            found_, rid_, name_ = rbyd.namelookup(did, name)

            # find branch tag if there is one
            branch_ = rbyd.lookup(rid_, TAG_BRANCH, 0x3)

            # found another branch
            if branch_ is not None and (
                    not depth or depth_ < depth):
                # update our bid
                bid += rid_ - (name_.weight-1)

                block, trunk, cksum = frombranch(branch_.data)
                rbyd = Rbyd.fetch(self.bd, block, trunk, cksum)
                # keep track of expected trunk/weight if corrupted
                if not rbyd:
                    rbyd.trunk = trunk
                    rbyd.weight = name_.weight

                depth_ += 1

            # found best match
            else:
                return found_, bid + rid_, rbyd, rid_, name_

    # create an rbyd tree for debugging
    def _tree_rtree(self, *,
            depth=None,
            inner=False,
            **args):
        # precompute rbyd trees so we know the max depth at each layer
        # to nicely align trees
        rtrees = {}
        rdepths = {}
        for bid, rbyd, path in self.traverse(path=True, depth=depth):
            if not rbyd:
                continue

            rtrees[rbyd] = rbyd.tree(**args)
            rdepths[len(path)] = max(
                    rdepths.get(len(path), 0),
                    max((t.depth+1 for t in rtrees[rbyd]), default=0))

        # map rbyd branches into our btree space
        tree = set()
        for bid, rbyd, path in self.traverse(path=True, depth=depth):
            if not rbyd:
                continue

            # yes we can find new rbyds if disk is being mutated, just
            # ignore these
            if rbyd not in rtrees:
                continue

            rtree = rtrees[rbyd]
            rdepth = max((t.depth+1 for t in rtree), default=0)
            d = sum(rdepths[d]+(1 if inner or args.get('tree_rbyd') else 0)
                for d in range(len(path)))

            # map into our btree space
            for t in rtree:
                # note we adjust our bid to be left-leaning, this allows
                # a global order and makes tree rendering quite a bit easier
                a_rid, a_tag = t.a
                b_rid, b_tag = t.b
                _, (_, a_w, _) = rbyd.lookupnext(a_rid)
                _, (_, b_w, _) = rbyd.lookupnext(b_rid)
                tree.add(TreeBranch(
                        (bid-(rbyd.weight-1)+a_rid-(a_w-1), len(path), a_tag),
                        (bid-(rbyd.weight-1)+b_rid-(b_w-1), len(path), b_tag),
                        d + rdepths[len(path)]-rdepth + t.depth,
                        t.color))

            # connect rbyd branches to rbyd roots
            if path and (inner or args.get('tree_rbyd')):
                l_bid, l_rbyd, l_rid, l_name = path[-1]
                l_branch = l_rbyd.lookup(l_rid, TAG_BRANCH, 0x3)

                if rtree:
                    r_rid, r_tag = min(rtree, key=lambda t: t.depth).a
                    _, (_, r_w, _) = rbyd.lookupnext(r_rid)
                else:
                    r_rid, (r_tag, r_w, _) = rbyd.lookupnext(-1)

                tree.add(TreeBranch(
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
                tree_ = set()
                for t in tree:
                    if t.a[1] == d and t.a[0] in roots:
                        t = TreeBranch(
                                roots[t.a[0]].a,
                                t.b,
                                t.depth,
                                t.color)
                    if t.b[1] == d and t.b[0] in roots:
                        t = TreeBranch(
                                t.a,
                                roots[t.b[0]].a,
                                t.depth,
                                t.color)
                    tree_.add(t)
                tree = tree_

        return tree

    # create a btree tree for debugging
    def _tree_btree(self, *,
            depth=None,
            inner=False,
            **args):
        # find all branches
        tree = set()
        root = None
        branches = {}
        for bid, rbyd, rid, name, path in self.bids(
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

                tree.add(TreeBranch(a, b, d))
                a = b

        return tree

    # create tree representation for debugging
    def tree(self, **args):
        if args.get('tree_btree'):
            return self._tree_btree(**args)
        else:
            return self._tree_rtree(**args)



def main(disk, roots=None, *,
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

    # flatten roots, default to block 0
    if not roots:
        roots = [(0,)]
    roots = [block for roots_ in roots for block in roots_]

    # we seek around a bunch, so just keep the disk open
    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # fetch the btree
        bd = Bd(f, block_size, block_count)
        btree = Btree.fetch(bd, roots, trunk)

        # print some information about the btree
        print('btree %s w%d, rev %08x, cksum %08x' % (
                btree.addr(),
                btree.weight,
                btree.rev,
                btree.cksum))

        # precompute tree renderings
        t_width = 0
        if (args.get('tree')
                or args.get('tree_rbyd')
                or args.get('tree_btree')):
            tree = btree.tree(**args)

            # find the max depth from the tree
            t_depth = max((t.depth+1 for t in tree), default=0)
            if t_depth > 0:
                t_width = 2*t_depth + 2

        # dynamically size the id field
        w_width = mt.ceil(mt.log10(max(1, btree.weight)+1))

        # prbyd here means the last rendered rbyd, we update
        # in dbg_branch to always print interleaved addresses
        prbyd = None
        def dbg_branch(d, bid, rbyd, rid, name):
            nonlocal prbyd

            # show human-readable representation
            for rattr in rbyd.rattrs(rid):
                print('%10s %s%*s %-*s  %s' % (
                        '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                            if prbyd is None or rbyd != prbyd
                            else '',
                        treerepr(tree, (bid-(name.weight-1), d, rattr.tag),
                                t_depth, color)
                            if args.get('tree')
                                or args.get('tree_rbyd')
                                or args.get('tree_btree')
                            else '',
                        2*w_width+1, '%d-%d' % (bid-(rattr.weight-1), bid)
                            if rattr.weight > 1
                            else bid if rattr.weight > 0
                            else '',
                        21+w_width, rattr,
                        next(xxd(rattr.data, 8), '')
                            if not args.get('raw')
                                and not args.get('no_truncate')
                            else ''))
                prbyd = rbyd

                # show on-disk encoding of tags/data
                if args.get('raw'):
                    for o, line in enumerate(xxd(
                            rbyd.data[rattr.toff:rattr.off])):
                        print('%9s: %*s%*s %s' % (
                                '%04x' % (rattr.toff + o*16),
                                t_width, '',
                                2*w_width+1, '',
                                line))
                if args.get('raw') or args.get('no_truncate'):
                    for o, line in enumerate(xxd(rattr.data)):
                        print('%9s: %*s%*s %s' % (
                                '%04x' % (rattr.off + o*16),
                                t_width, '',
                                2*w_width+1, '',
                                line))

        # traverse and print entries
        ppath = []
        corrupted = False
        for bid, rbyd, path in btree.leaves(
                path=True,
                depth=args.get('depth')):
            # print inner branches if requested
            if args.get('inner'):
                for d, (bid_, rbyd_, rid_, name_) in pathdelta(
                        path, ppath):
                    dbg_branch(d, bid_, rbyd_, rid_, name_)
            ppath = path

            # corrupted? try to keep printing the tree
            if not rbyd:
                print('%04x.%04x: %*s%s%s%s' % (
                        rbyd.block, rbyd.trunk,
                        t_width, '',
                        '\x1b[31m' if color else '',
                        '(corrupted rbyd %s)' % rbyd.addr(),
                        '\x1b[m' if color else ''))
                prbyd = rbyd
                corrupted = True
                continue

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
            description="Debug rbyd B-trees.",
            allow_abbrev=False)
    parser.add_argument(
            'disk',
            help="File containing the block device.")
    parser.add_argument(
            'roots',
            nargs='*',
            type=rbydaddr,
            help="Block address of the roots of the tree.")
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
            '-i', '--inner',
            action='store_true',
            help="Show inner branches.")
    parser.add_argument(
            '-z', '--depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of tree to show.")
    parser.add_argument(
            '-e', '--error-on-corrupt',
            action='store_true',
            help="Error if B-tree is corrupt.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
