#!/usr/bin/env python3

import bisect
import collections as co
import functools as ft
import itertools as it
import math as m
import os
import shutil
import struct


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
TAG_ECKSUM          = 0x3100
TAG_ALT             = 0x4000
TAG_R               = 0x2000
TAG_GT              = 0x1000


CHARS = 'mbd-'
COLORS = ['33', '34', '32', '90']

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
    return size, block, off


# space filling Hilbert-curve
#
# note we memoize the last curve since this is a bit expensive
#
@ft.lru_cache(1)
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
                yield (x,y)
                x, y = x+a_dx, y+a_dy
            return

        # trivial column
        if w == 1:
            for _ in range(h):
                yield (x,y)
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
            yield from hilbert_(x, y, a_x_, a_y_, b_x, b_y)
            yield from hilbert_(x+a_x_, y+a_y_, a_x-a_x_, a_y-a_y_, b_x, b_y)
        else:
            # prefer even steps
            if h_ % 2 != 0 and h > 2:
                b_x_, b_y_ = b_x_+b_dx, b_y_+b_dy

            # split in three
            yield from hilbert_(x, y, b_x_, b_y_, a_x_, a_y_)
            yield from hilbert_(x+b_x_, y+b_y_, a_x, a_y, b_x-b_x_, b_y-b_y_)
            yield from hilbert_(
                x+(a_x-a_dx)+(b_x_-b_dx), y+(a_y-a_dy)+(b_y_-b_dy),
                -b_x_, -b_y_, -(a_x-a_x_), -(a_y-a_y_))

    if width >= height:
        curve = hilbert_(0, 0, +width, 0, 0, +height)
    else:
        curve = hilbert_(0, 0, 0, +height, +width, 0)

    return list(curve)

# space filling Z-curve/Lebesgue-curve
#
# note we memoize the last curve since this is a bit expensive
#
@ft.lru_cache(1)
def lebesgue_curve(width, height):
    # we create a truncated Z-curve by simply filtering out the points
    # that are outside our region
    curve = []
    for i in range(2**(2*m.ceil(m.log2(max(width, height))))):
        # we just operate on binary strings here because it's easier
        b = '{:0{}b}'.format(i, 2*m.ceil(m.log2(i+1)/2))
        x = int(b[1::2], 2) if b[1::2] else 0
        y = int(b[0::2], 2) if b[0::2] else 0
        if x < width and y < height:
            curve.append((x, y))

    return curve


# the rendering code is copied from tracebd.py, which is why it may look a
# little funny
#
# each block can be in one of 3 states: mdir, btree, or raw data, we keep track
# of these at the pixel-level via a bitmask
#
class Pixel(int):
    __slots__ = ()
    def __new__(cls, state=0, *,
            mdir=False,
            btree=False,
            data=False):
        return super().__new__(cls,
            state
            | (1 if mdir  else 0)
            | (2 if btree else 0)
            | (4 if data  else 0))

    @property
    def is_mdir(self):
        return (self & 1) != 0

    @property
    def is_btree(self):
        return (self & 2) != 0

    @property
    def is_data(self):
        return (self & 4) != 0

    def mdir(self):
        return Pixel(int(self) | 1)

    def btree(self):
        return Pixel(int(self) | 2)

    def data(self):
        return Pixel(int(self) | 4)

    def clear(self):
        return Pixel(0)

    def __or__(self, other):
        return Pixel(int(self) | int(other))

    def draw(self, char=None, *,
            mdirs=True,
            btrees=True,
            datas=True,
            color=True,
            dots=False,
            braille=False,
            chars=None,
            colors=None,
            **_):
        # fallback to default chars/colors
        if chars is None:
            chars = CHARS
        if len(chars) < len(CHARS):
            chars = chars + CHARS[len(chars):]

        if colors is None:
            colors = COLORS
        if len(colors) < len(COLORS):
            colors = colors + COLORS[len(colors):]

        # compute char/color
        c = chars[3]
        f = [colors[3]]

        if mdirs and self.is_mdir:
            c = chars[0]
            f.append(colors[0])
        elif btrees and self.is_btree:
            c = chars[1]
            f.append(colors[1])
        elif datas and self.is_data:
            c = chars[2]
            f.append(colors[2])

        # override char?
        if char:
            c = char

        # apply colors
        if f and color:
            c = '%s%s\x1b[m' % (
                ''.join('\x1b[%sm' % f_ for f_ in f),
                c)

        return c


class Bmap:
    def __init__(self, *,
            block_size=1,
            block_count=1,
            block_window=None,
            off_window=None,
            width=None,
            height=1,
            pixels=None):
        # default width to block_window or block_size
        if width is None:
            if block_window is not None:
                width = len(block_window)
            else:
                width = block_count

        # allocate pixels if not provided
        if pixels is None:
            pixels = [Pixel() for _ in range(width*height)]

        self.pixels = pixels
        self.block_size = block_size
        self.block_count = block_count
        self.block_window = block_window
        self.off_window = off_window
        self.width = width
        self.height = height

    @property
    def _block_window(self):
        if self.block_window is None:
            return range(0, self.block_count)
        else:
            return self.block_window

    @property
    def _off_window(self):
        if self.off_window is None:
            return range(0, self.block_size)
        else:
            return self.off_window

    @property
    def _window(self):
        return len(self._off_window)*len(self._block_window)

    def _op(self, f, block=None, off=None, size=None):
        if block is None:
            range_ = range(len(self.pixels))
        else:
            if off is None:
                off, size = 0, self.block_size
            elif size is None:
                off, size = 0, off

            # map into our window
            if block not in self._block_window:
                return
            block -= self._block_window.start

            size = (max(self._off_window.start,
                    min(self._off_window.stop, off+size))
                - max(self._off_window.start,
                    min(self._off_window.stop, off)))
            off = (max(self._off_window.start,
                min(self._off_window.stop, off))
                - self._off_window.start)
            if size == 0:
                return

            # map to our block space
            range_ = range(
                block*len(self._off_window) + off,
                block*len(self._off_window) + off+size)
            range_ = range(
                (range_.start*len(self.pixels)) // self._window,
                (range_.stop*len(self.pixels)) // self._window)
            range_ = range(
                range_.start,
                max(range_.stop, range_.start+1))

        # apply the op
        for i in range_:
            self.pixels[i] = f(self.pixels[i])

    def mdir(self, block=None, off=None, size=None):
        self._op(Pixel.mdir, block, off, size)

    def btree(self, block=None, off=None, size=None):
        self._op(Pixel.btree, block, off, size)

    def data(self, block=None, off=None, size=None):
        self._op(Pixel.data, block, off, size)

    def clear(self, block=None, off=None, size=None):
        self._op(Pixel.clear, block, off, size)

    def resize(self, *,
            block_size=None,
            block_count=None,
            width=None,
            height=None):
        block_size = (block_size if block_size is not None
            else self.block_size)
        block_count = (block_count if block_count is not None
            else self.block_count)
        width = width if width is not None else self.width
        height = height if height is not None else self.height

        if (block_size == self.block_size
                and block_count == self.block_count
                and width == self.width
                and height == self.height):
            return

        # transform our pixels
        self.block_size = block_size
        self.block_count = block_count

        pixels = []
        for x in range(width*height):
            # map into our old bd space
            range_ = range(
                (x*self._window) // (width*height),
                ((x+1)*self._window) // (width*height))
            range_ = range(
                range_.start,
                max(range_.stop, range_.start+1))

            # aggregate state
            pixels.append(ft.reduce(
                Pixel.__or__,
                self.pixels[range_.start:range_.stop],
                Pixel()))
            
        self.width = width
        self.height = height
        self.pixels = pixels

    def draw(self, row, *,
            mdirs=False,
            btrees=False,
            datas=False,
            hilbert=False,
            lebesgue=False,
            dots=False,
            braille=False,
            **args):
        # fold via a curve?
        if hilbert:
            grid = [None]*(self.width*self.height)
            for (x,y), p in zip(
                    hilbert_curve(self.width, self.height),
                    self.pixels):
                grid[x + y*self.width] = p
        elif lebesgue:
            grid = [None]*(self.width*self.height)
            for (x,y), p in zip(
                    lebesgue_curve(self.width, self.height),
                    self.pixels):
                grid[x + y*self.width] = p
        else:
            grid = self.pixels

        line = []
        if braille:
            # encode into a byte
            for x in range(0, self.width, 2):
                byte_p = 0
                best_p = Pixel()
                for i in range(2*4):
                    p = grid[x+(2-1-(i%2)) + ((row*4)+(4-1-(i//2)))*self.width]
                    best_p |= p
                    if ((mdirs and p.is_mdir)
                            or (btrees and p.is_btree)
                            or (datas and p.is_data)):
                        byte_p |= 1 << i

                line.append(best_p.draw(
                    CHARS_BRAILLE[byte_p],
                    braille=True,
                    mdirs=mdirs,
                    btrees=btrees,
                    datas=datas,
                    **args))
        elif dots:
            # encode into a byte
            for x in range(self.width):
                byte_p = 0
                best_p = Pixel()
                for i in range(2):
                    p = grid[x + ((row*2)+(2-1-i))*self.width]
                    best_p |= p
                    if ((mdirs and p.is_mdir)
                            or (btrees and p.is_btree)
                            or (datas and p.is_data)):
                        byte_p |= 1 << i

                line.append(best_p.draw(
                    CHARS_DOTS[byte_p],
                    dots=True,
                    mdirs=mdirs,
                    btrees=btrees,
                    datas=datas,
                    **args))
        else:
            for x in range(self.width):
                line.append(grid[x + row*self.width].draw(
                    mdirs=mdirs,
                    btrees=btrees,
                    datas=datas,
                    **args))

        return ''.join(line)


# our core rbyd type
class Rbyd:
    def __init__(self, block, data, rev, eoff, trunk, weight):
        self.block = block
        self.data = data
        self.rev = rev
        self.eoff = eoff
        self.trunk = trunk
        self.weight = weight
        self.redund_blocks = []

    @property
    def blocks(self):
        return (self.block, *self.redund_blocks)

    def addr(self):
        if not self.redund_blocks:
            return '0x%x.%x' % (self.block, self.trunk)
        else:
            return '0x{%x,%s}.%x' % (
                self.block,
                ','.join('%x' % block for block in self.redund_blocks),
                self.trunk)

    @classmethod
    def fetch(cls, f, block_size, blocks, trunk=None):
        if isinstance(blocks, int):
            blocks = [blocks]

        if len(blocks) > 1:
            # fetch all blocks
            rbyds = [cls.fetch(f, block_size, block, trunk) for block in blocks]
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
            rbyd.redund_blocks = [rbyds[(i+1+j) % len(rbyds)].block
                for j in range(len(rbyds)-1)]
            return rbyd
        else:
            # block may encode a trunk
            block = blocks[0]
            if isinstance(block, tuple):
                if trunk is None:
                    trunk = block[1]
                block = block[0]

        # seek to the block
        f.seek(block * block_size)
        data = f.read(block_size)

        # fetch the rbyd
        rev = fromle32(data[0:4])
        cksum = 0
        cksum_ = crc32c(data[0:4])
        eoff = 0
        j_ = 4
        trunk_ = 0
        trunk__ = 0
        trunk___ = 0
        weight = 0
        weight_ = 0
        weight__ = 0
        wastrunk = False
        trunkeoff = None
        while j_ < len(data) and (not trunk or eoff <= trunk):
            v, tag, w, size, d = fromtag(data[j_:])
            if v != (popc(cksum_) & 1):
                break
            cksum_ = crc32c(data[j_:j_+d], cksum_)
            j_ += d
            if not tag & TAG_ALT and j_ + size > len(data):
                break

            # take care of cksums
            if not tag & TAG_ALT:
                if (tag & 0xff00) != TAG_CKSUM:
                    cksum_ = crc32c(data[j_:j_+size], cksum_)
                # found a cksum?
                else:
                    cksum__ = fromle32(data[j_:j_+4])
                    if cksum_ != cksum__:
                        break
                    # commit what we have
                    eoff = trunkeoff if trunkeoff else j_ + size
                    cksum = cksum_
                    trunk_ = trunk__
                    weight = weight_

            # evaluate trunks
            if (tag & 0xf000) != TAG_CKSUM and (
                    not trunk or trunk >= j_-d or wastrunk):
                # new trunk?
                if not wastrunk:
                    wastrunk = True
                    trunk___ = j_-d
                    weight__ = 0

                # keep track of weight
                weight__ += w

                # end of trunk?
                if not tag & TAG_ALT:
                    wastrunk = False
                    # update trunk/weight unless we found a shrub or an
                    # explicit trunk (which may be a shrub) is requested
                    if not tag & TAG_SHRUB or trunk:
                        trunk__ = trunk___
                        weight_ = weight__
                        # keep track of eoff for best matching trunk
                        if trunk and j_ + size > trunk:
                            trunkeoff = j_ + size
                            eoff = trunkeoff
                            cksum = cksum_
                            trunk_ = trunk__
                            weight = weight_

            if not tag & TAG_ALT:
                j_ += size

        return cls(block, data, rev, eoff, trunk_, weight)

    def lookup(self, rid, tag):
        if not self:
            return True, 0, -1, 0, 0, 0, b'', []

        tag = max(tag, 0x1)
        lower = 0
        upper = self.weight
        path = []

        # descend down tree
        j = self.trunk
        while True:
            _, alt, weight_, jump, d = fromtag(self.data[j:])

            # found an alt?
            if alt & TAG_ALT:
                # follow?
                if ((rid, tag & 0xfff) > (upper-weight_-1, alt & 0xfff)
                        if alt & TAG_GT
                        else ((rid, tag & 0xfff)
                            <= (lower+weight_-1, alt & 0xfff))):
                    lower += upper-lower-weight_ if alt & TAG_GT else 0
                    upper -= upper-lower-weight_ if not alt & TAG_GT else 0
                    j = j - jump

                    # figure out which color
                    if alt & TAG_R:
                        _, nalt, _, _, _ = fromtag(self.data[j+jump+d:])
                        if nalt & TAG_R:
                            path.append((j+jump, j, True, 'y'))
                        else:
                            path.append((j+jump, j, True, 'r'))
                    else:
                        path.append((j+jump, j, True, 'b'))

                # stay on path
                else:
                    lower += weight_ if not alt & TAG_GT else 0
                    upper -= weight_ if alt & TAG_GT else 0
                    j = j + d

                    # figure out which color
                    if alt & TAG_R:
                        _, nalt, _, _, _ = fromtag(self.data[j:])
                        if nalt & TAG_R:
                            path.append((j-d, j, False, 'y'))
                        else:
                            path.append((j-d, j, False, 'r'))
                    else:
                        path.append((j-d, j, False, 'b'))

            # found tag
            else:
                rid_ = upper-1
                tag_ = alt
                w_ = upper-lower

                done = not tag_ or (rid_, tag_) < (rid, tag)

                return done, rid_, tag_, w_, j, d, self.data[j+d:j+d+jump], path

    def __bool__(self):
        return bool(self.trunk)

    def __eq__(self, other):
        return self.block == other.block and self.trunk == other.trunk

    def __ne__(self, other):
        return not self.__eq__(other)

    def __iter__(self):
        tag = 0
        rid = -1

        while True:
            done, rid, tag, w, j, d, data, _ = self.lookup(rid, tag+0x1)
            if done:
                break

            yield rid, tag, w, j, d, data

    # btree lookup with this rbyd as the root
    def btree_lookup(self, f, block_size, bid, *,
            depth=None):
        rbyd = self
        rid = bid
        depth_ = 1
        path = []

        # corrupted? return a corrupted block once
        if not rbyd:
            return bid > 0, bid, 0, rbyd, -1, [], path

        while True:
            # collect all tags, normally you don't need to do this
            # but we are debugging here
            name = None
            tags = []
            branch = None
            rid_ = rid
            tag = 0
            w = 0
            for i in it.count():
                done, rid__, tag, w_, j, d, data, _ = rbyd.lookup(
                    rid_, tag+0x1)
                if done or (i != 0 and rid__ != rid_):
                    break

                # first tag indicates the branch's weight
                if i == 0:
                    rid_, w = rid__, w_

                # catch any branches
                if tag & 0xfff == TAG_BRANCH:
                    branch = (tag, j, d, data)

                tags.append((tag, j, d, data))

            # keep track of path
            path.append((bid + (rid_-rid), w, rbyd, rid_, tags))

            # descend down branch?
            if branch is not None and (
                    not depth or depth_ < depth):
                tag, j, d, data = branch
                block, trunk, cksum = frombranch(data)
                rbyd = Rbyd.fetch(f, block_size, block, trunk)

                # corrupted? bail here so we can keep traversing the tree
                if not rbyd:
                    return False, bid + (rid_-rid), w, rbyd, -1, [], path

                rid -= (rid_-(w-1))
                depth_ += 1
            else:
                return not tags, bid + (rid_-rid), w, rbyd, rid_, tags, path

    # mtree lookup with this rbyd as the mroot
    def mtree_lookup(self, f, block_size, mbid):
        # have mtree?
        done, rid, tag, w, j, d, data, _ = self.lookup(-1, TAG_MTREE)
        if not done and rid == -1 and tag == TAG_MTREE:
            w, block, trunk, cksum = frombtree(data)
            mtree = Rbyd.fetch(f, block_size, block, trunk)
            # corrupted?
            if not mtree:
                return True, -1, 0, None

            # lookup our mbid
            done, mbid, mw, rbyd, rid, tags, path = mtree.btree_lookup(
                f, block_size, mbid)
            if done:
                return True, -1, 0, None

            mdir = next(((tag, j, d, data)
                for tag, j, d, data in tags
                if tag == TAG_MDIR),
                None)
            if not mdir:
                return True, -1, 0, None

            # fetch the mdir
            _, _, _, data = mdir
            blocks = frommdir(data)
            return False, mbid, mw, Rbyd.fetch(f, block_size, blocks)

        else:
            # have mdir?
            done, rid, tag, w, j, _, data, _ = self.lookup(-1, TAG_MDIR)
            if not done and rid == -1 and tag == TAG_MDIR:
                blocks = frommdir(data)
                return False, 0, 0, Rbyd.fetch(f, block_size, blocks)

            else:
                # I guess we're inlined?
                if mbid == -1:
                    return False, -1, 0, self
                else:
                    return True, -1, 0, None


def main(disk, mroots=None, *,
        block_size=None,
        block_count=None,
        block=None,
        off=None,
        size=None,
        mdirs=False,
        btrees=False,
        datas=False,
        no_header=False,
        color='auto',
        dots=False,
        braille=False,
        width=None,
        height=None,
        lines=None,
        hilbert=False,
        lebesgue=False,
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # show all block types by default
    if not mdirs and not btrees and not datas:
        mdirs = True
        btrees = True
        datas = True

    # assume a reasonable lines/height if not specified
    #
    # note that we let height = None if neither hilbert or lebesgue
    # are specified, this is a bit special as the default may be less
    # than one character in height.
    if height is None and (hilbert or lebesgue):
        if lines is not None:
            height = lines
        else:
            height = 5

    if lines is None:
        if height is not None:
            lines = height
        else:
            lines = 5

    # is bd geometry specified?
    if isinstance(block_size, tuple):
        block_size, block_count_ = block_size
        if block_count is None:
            block_count = block_count_

    # try to simplify the block/off/size arguments a bit
    if not isinstance(block, tuple):
        block = block,
    if isinstance(off, tuple) and len(off) == 1:
        off, = off
    if isinstance(size, tuple) and len(size) == 1:
        if off is None:
            off, = size
        size = None

    if any(isinstance(b, list) and len(b) > 1 for b in block):
        print("error: more than one block address?",
            file=sys.stderr)
        sys.exit(-1)
    if isinstance(block[0], list):
        block = (block[0][0], *block[1:])
    if len(block) > 1 and isinstance(block[1], list):
        block = (block[0], block[1][0])
    if isinstance(block[0], tuple):
        block, off_ = (block[0][0], *block[1:]), block[0][1]
        if off is None:
            off = off_
    if len(block) > 1 and isinstance(block[1], tuple):
        block = (block[0], block[1][0])
    if len(block) == 1:
        block, = block

    if isinstance(off, tuple):
        off, size_ = off[0], off[1] - off[0]
        if size is None:
            size = size_
    if isinstance(size, tuple):
        off_, size = off[0], off[1] - off[0]
        if off is None:
            off = off_

    # is a block window specified?
    block_window = None
    if block is not None:
        if isinstance(block, tuple):
            block_window = range(*block)
        else:
            block_window = range(block, block+1)

    off_window = None
    if off is not None or size is not None:
        off_ = off if off is not None else 0
        size_ = size if size is not None else 1
        off_window = range(off_, off_+size_)

    # figure out best width/height
    if width is None:
        width_ = min(80, shutil.get_terminal_size((80, 5))[0])
    elif width:
        width_ = width
    else:
        width_ = shutil.get_terminal_size((80, 5))[0]

    if height is None:
        height_ = 0
    elif height:
        height_ = height
    else:
        height_ = shutil.get_terminal_size((80, 5))[1]

    # create our block device representation
    bmap = Bmap(
        block_size=block_size,
        block_count=block_count,
        block_window=block_window,
        off_window=off_window,
        # scale if we're printing with dots or braille
        width=2*width_ if braille else width_,
        height=max(1,
            4*height_ if braille
            else 2*height_ if dots
            else height_))

    # keep track of how many blocks are in use
    mdirs_ = 0
    btrees_ = 0
    datas_ = 0

    # flatten mroots, default to 0x{0,1}
    if not mroots:
        mroots = [[0,1]]
    mroots = [block for mroots_ in mroots for block in mroots_]

    # we seek around a bunch, so just keep the disk open
    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()
            block_count = 1
            bmap.resize(
                block_size=block_size,
                block_count=block_count)

        # if block_count is omitted, derive the block_count from our file size
        if block_count is None:
            f.seek(0, os.SEEK_END)
            block_count = f.tell() // block_size
            bmap.resize(
                block_size=block_size,
                block_count=block_count)

        #### traverse the filesystem

        # fetch the mroot chain
        corrupted = False
        btrees__ = []
        mroot = Rbyd.fetch(f, block_size, mroots)
        mdepth = 1
        while True:
            # corrupted?
            if not mroot:
                corrupted = True
                break

            # mark mroots in our bmap
            for block in mroot.blocks:
                bmap.mdir(block,
                    mroot.eoff if args.get('in_use') else block_size)
                mdirs_ += 1;

            # find any file btrees in our mroot
            for rid, tag, w, j, d, data in mroot:
                if (tag == TAG_DATA
                        or tag == TAG_BLOCK
                        or tag == TAG_BSHRUB
                        or tag == TAG_BTREE):
                    btrees__.append((mroot, tag, data))

            # stop here?
            if args.get('depth') and mdepth >= args.get('depth'):
                break

            # fetch the next mroot
            done, rid, tag, w, j, d, data, _ = mroot.lookup(-1, TAG_MROOT)
            if not (not done and rid == -1 and tag == TAG_MROOT):
                break

            blocks = frommdir(data)
            mroot = Rbyd.fetch(f, block_size, blocks)
            mdepth += 1

        # fetch the mdir, if there is one
        mdir = None
        if not args.get('depth') or mdepth < args.get('depth'):
            done, rid, tag, w, j, _, data, _ = mroot.lookup(-1, TAG_MDIR)
            if not done and rid == -1 and tag == TAG_MDIR:
                blocks = frommdir(data)
                mdir = Rbyd.fetch(f, block_size, blocks)

                # corrupted?
                if not mdir:
                    corrupted = True
                else:
                    # mark mdir in our bmap
                    for block in mdir.blocks:
                        bmap.mdir(block,
                            mdir.eoff if args.get('in_use') else block_size)
                        mdirs_ += 1

                    # find any file btrees in our mdir
                    for rid, tag, w, j, d, data in mdir:
                        if (tag == TAG_DATA
                                or tag == TAG_BLOCK
                                or tag == TAG_BSHRUB
                                or tag == TAG_BTREE):
                            btrees__.append((mdir, tag, data))

        # fetch the actual mtree, if there is one
        mtree = None
        if not args.get('depth') or mdepth < args.get('depth'):
            done, rid, tag, w, j, d, data, _ = mroot.lookup(-1, TAG_MTREE)
            if not done and rid == -1 and tag == TAG_MTREE:
                w, block, trunk, cksum = frombtree(data)
                mtree = Rbyd.fetch(f, block_size, block, trunk)

                # traverse entries
                mbid = -1
                ppath = []
                while True:
                    done, mbid, mw, rbyd, rid, tags, path = mtree.btree_lookup(
                        f, block_size, mbid+1,
                        depth=args.get('depth', mdepth)-mdepth)
                    if done:
                        break

                    # traverse the inner btree nodes
                    changed = False
                    for (x, px) in it.zip_longest(
                            enumerate(path),
                            enumerate(ppath)):
                        if x is None:
                            break
                        if not (changed or px is None or x[0] != px[0]):
                            continue
                        changed = True

                        # mark btree inner nodes in our bmap
                        d, (mid_, w_, rbyd_, rid_, tags_) = x
                        for block in rbyd_.blocks:
                            bmap.btree(block,
                                rbyd_.eoff if args.get('in_use')
                                else block_size)
                            btrees_ += 1
                    ppath = path

                    # corrupted?
                    if not rbyd:
                        corrupted = True
                        continue

                    # found an mdir in the tags?
                    mdir__ = None
                    if (not args.get('depth')
                            or mdepth+len(path) < args.get('depth')):
                        mdir__ = next(((tag, j, d, data)
                            for tag, j, d, data in tags
                            if tag == TAG_MDIR),
                            None)

                    if mdir__:
                        # fetch the mdir
                        _, _, _, data = mdir__
                        blocks = frommdir(data)
                        mdir_ = Rbyd.fetch(f, block_size, blocks)

                        # corrupted?
                        if not mdir_:
                            corrupted = True
                        else:
                            # mark mdir in our bmap
                            for block in mdir_.blocks:
                                bmap.mdir(block, 0,
                                    mdir_.eoff if args.get('in_use')
                                    else block_size)
                                mdirs_ += 1

                            # find any file btrees in our mdir
                            for rid, tag, w, j, d, data in mdir_:
                                if (tag == TAG_DATA
                                        or tag == TAG_BLOCK
                                        or tag == TAG_BSHRUB
                                        or tag == TAG_BTREE):
                                    btrees__.append((mdir_, tag, data))

        # fetch any file btrees we found
        if not args.get('depth') or mdepth < args.get('depth'):
            for mdir, tag, data in btrees__:
                # inlined data?
                if tag == TAG_DATA:
                    # ignore here
                    continue

                # direct block?
                elif tag == TAG_BLOCK:
                    size, block, off = frombptr(data)
                    # mark block in our bmap
                    bmap.data(block,
                        off if args.get('in_use') else 0,
                        size if args.get('in_use') else block_size)
                    datas_ += 1
                    continue

                # inlined bshrub?
                elif tag == TAG_BSHRUB:
                    weight, trunk = fromshrub(data)
                    btree = Rbyd.fetch(f, block_size, mdir.block, trunk)
                    shrub = True

                # indirect btree?
                elif tag == TAG_BTREE:
                    w, block, trunk, cksum = frombtree(data)
                    btree = Rbyd.fetch(f, block_size, block, trunk)
                    shrub = False

                else:
                    assert False

                # traverse entries
                bid = -1
                ppath = []
                while True:
                    (done, bid, w, rbyd, rid, tags, path
                        ) = btree.btree_lookup(
                            f, block_size, bid+1,
                            depth=args.get('depth', mdepth)-mdepth)
                    if done:
                        break

                    # traverse the inner btree nodes
                    changed = False
                    for (x, px) in it.zip_longest(
                            enumerate(path),
                            enumerate(ppath)):
                        if x is None:
                            break
                        if not (changed or px is None or x[0] != px[0]):
                            continue
                        changed = True

                        # mark btree inner nodes in our bmap
                        d, (mid_, w_, rbyd_, rid_, tags_) = x
                        # ignore bshrub roots
                        if shrub and d == 0:
                            continue
                        for block in rbyd_.blocks:
                            bmap.btree(block,
                                rbyd_.eoff if args.get('in_use')
                                else block_size)
                            btrees_ += 1
                    ppath = path

                    # corrupted?
                    if not rbyd:
                        corrupted = True
                        continue

                    # found a block in the tags?
                    bptr__ = None
                    if (not args.get('depth')
                            or mdepth+len(path) < args.get('depth')):
                        bptr__ = next(((tag, j, d, data)
                            for tag, j, d, data in tags
                            if tag & 0xfff == TAG_BLOCK),
                            None)

                    if bptr__:
                        # fetch the block
                        _, _, _, data = bptr__
                        size, block, off = frombptr(data)

                        # mark blocks in our bmap
                        bmap.data(block,
                            off if args.get('in_use') else 0,
                            size if args.get('in_use') else block_size)
                        datas_ += 1

    #### actual rendering begins here

    # print some information about the bmap
    if not no_header:
        print('bd %dx%d%s%s%s' % (
            block_size, block_count,
            ', %6s mdir' % ('%.1f%%' % (100*mdirs_  / block_count))
                if mdirs else '',
            ', %6s btree' % ('%.1f%%' % (100*btrees_ / block_count))
                if btrees else '',
            ', %6s data' % ('%.1f%%' % (100*datas_  / block_count))
                if datas else ''))

    # and then print the bmap
    for row in range(
            m.ceil(bmap.height/4) if braille
            else m.ceil(bmap.height/2) if dots
            else bmap.height):
        line = bmap.draw(row,
            mdirs=mdirs,
            btrees=btrees,
            datas=datas,
            color=color,
            dots=dots,
            braille=braille,
            hilbert=hilbert,
            lebesgue=lebesgue,
            **args)
        print(line)

    if args.get('error_on_corrupt') and corrupted:
        sys.exit(2)


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
        '-b', '--block-size',
        type=bdgeom,
        help="Block size/geometry in bytes.")
    parser.add_argument(
        '--block-count',
        type=lambda x: int(x, 0),
        help="Block count in blocks.")
    parser.add_argument(
        '-@', '--block',
        nargs='?',
        type=lambda x: tuple(
            rbydaddr(x) if x.strip() else None
            for x in x.split(',')),
        help="Optional block to show, may be a range.")
    parser.add_argument(
        '--off',
        type=lambda x: tuple(
            int(x, 0) if x.strip() else None
            for x in x.split(',')),
        help="Show a specific offset, may be a range.")
    parser.add_argument(
        '--size',
        type=lambda x: tuple(
            int(x, 0) if x.strip() else None
            for x in x.split(',')),
        help="Show this many bytes, may be a range.")
    parser.add_argument(
        '-M', '--mdirs',
        action='store_true',
        help="Render mdir blocks.")
    parser.add_argument(
        '-B', '--btrees',
        action='store_true',
        help="Render btree blocks.")
    parser.add_argument(
        '-D', '--datas',
        action='store_true',
        help="Render data blocks.")
    parser.add_argument(
        '-N', '--no-header',
        action='store_true',
        help="Don't show the header.")
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
        help="Use 2x4 unicode braille characters. Note that braille characters "
            "sometimes suffer from inconsistent widths.")
    parser.add_argument(
        '--chars',
        help="Characters to use for mdir, btree, data, unused blocks.")
    parser.add_argument(
        '--colors',
        type=lambda x: [x.strip() for x in x.split(',')],
        help="Colors to use for mdir, btree, data, unused blocks.")
    parser.add_argument(
        '-W', '--width',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Width in columns. 0 uses the terminal width. Defaults to "
            "min(terminal, 80).")
    parser.add_argument(
        '-H', '--height',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Height in rows. 0 uses the terminal height. Defaults to 1.")
    parser.add_argument(
        '-n', '--lines',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Show this many lines of history. 0 uses the terminal height. "
            "Defaults to 5.")
    parser.add_argument(
        '-U', '--hilbert',
        action='store_true',
        help="Render as a space-filling Hilbert curve.")
    parser.add_argument(
        '-Z', '--lebesgue',
        action='store_true',
        help="Render as a space-filling Z-curve.")
    parser.add_argument(
        '-i', '--in-use',
        action='store_true',
        help="Show how much of each block is in use.")
    parser.add_argument(
        '-z', '--depth',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Depth of the filesystem tree to parse.")
    parser.add_argument(
        '-e', '--error-on-corrupt',
        action='store_true',
        help="Error if the filesystem is corrupt.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
