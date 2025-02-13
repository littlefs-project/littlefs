#!/usr/bin/env python3
#
# Display operations on block devices based on trace output
#
# Example:
# ./scripts/tracebd.py trace
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import functools as ft
import io
import itertools as it
import math as mt
import os
import re
import shutil
import sys
import threading as th
import time


CHARS = 'rpe-'
COLORS = ['32', '35', '34', '90']

WEAR_CHARS = '-123456789'
WEAR_COLORS = ['90', '', '', '', '', '', '', '35', '35', '1;31']

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


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
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


class RingIO:
    def __init__(self, maxlen=None, head=False):
        self.maxlen = maxlen
        self.head = head
        self.lines = co.deque(maxlen=maxlen)
        self.tail = io.StringIO()

        # trigger automatic sizing
        if maxlen == 0:
            self.resize(0)

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

    def resize(self, maxlen):
        self.maxlen = maxlen
        if maxlen == 0:
            maxlen = shutil.get_terminal_size((80, 5))[1]
        if maxlen != self.lines.maxlen:
            self.lines = co.deque(self.lines, maxlen=maxlen)

    canvas_lines = 1
    def draw(self):
        # did terminal size change?
        if self.maxlen == 0:
            self.resize(0)

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

        # first thing first, give ourself a canvas
        while RingIO.canvas_lines < len(lines):
            sys.stdout.write('\n')
            RingIO.canvas_lines += 1

        # write lines from top to bottom so later lines overwrite earlier
        # lines, note [xA/[xB stop at terminal boundaries
        for i, line in enumerate(lines):
            # move cursor, clear line, disable/reenable line wrapping
            sys.stdout.write('\r')
            if len(lines)-1-i > 0:
                sys.stdout.write('\x1b[%dA' % (len(lines)-1-i))
            sys.stdout.write('\x1b[K')
            sys.stdout.write('\x1b[?7l')
            sys.stdout.write(line)
            sys.stdout.write('\x1b[?7h')
            if len(lines)-1-i > 0:
                sys.stdout.write('\x1b[%dB' % (len(lines)-1-i))
        sys.stdout.flush()


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
    for i in range(2**(2*mt.ceil(mt.log2(max(width, height))))):
        # we just operate on binary strings here because it's easier
        b = '{:0{}b}'.format(i, 2*mt.ceil(mt.log2(i+1)/2))
        x = int(b[1::2], 2) if b[1::2] else 0
        y = int(b[0::2], 2) if b[0::2] else 0
        if x < width and y < height:
            curve.append((x, y))

    return curve


class Pixel(int):
    __slots__ = ()
    def __new__(cls, state=0, *,
            wear=0,
            readed=False,
            proged=False,
            erased=False):
        return super().__new__(cls,
                state
                    | (wear << 3)
                    | (1 if readed else 0)
                    | (2 if proged else 0)
                    | (4 if erased else 0))

    @property
    def wear(self):
        return self >> 3

    @property
    def readed(self):
        return (self & 1) != 0

    @property
    def proged(self):
        return (self & 2) != 0

    @property
    def erased(self):
        return (self & 4) != 0

    def read(self):
        return Pixel(int(self) | 1)

    def prog(self):
        return Pixel(int(self) | 2)

    def erase(self):
        return Pixel((int(self) | 4) + 8)

    def clear(self):
        return Pixel(int(self) & ~7)

    def __or__(self, other):
        return Pixel(
                (int(self) | int(other)) & 7,
                wear=max(self.wear, other.wear))

    def worn(self, max_wear, *,
            block_cycles=None,
            wear_chars=None,
            **_):
        if wear_chars is None:
            wear_chars = WEAR_CHARS

        if block_cycles:
            return self.wear / block_cycles
        else:
            return self.wear / max(max_wear, len(wear_chars))

    def draw(self, max_wear, char=None, *,
            read=True,
            prog=True,
            erase=True,
            wear=False,
            block_cycles=None,
            color=True,
            dots=False,
            braille=False,
            chars=None,
            wear_chars=None,
            colors=None,
            wear_colors=None,
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

        if wear_chars is None:
            wear_chars = WEAR_CHARS

        if wear_colors is None:
            wear_colors = WEAR_COLORS

        # compute char/color
        c = chars[3]
        f = [colors[3]]

        if wear:
            w = min(self.worn(
                        max_wear,
                        block_cycles=block_cycles,
                        wear_chars=wear_chars),
                    1)

            c = wear_chars[int(w * (len(wear_chars)-1))]
            f.append(wear_colors[int(w * (len(wear_colors)-1))])

        if prog and self.proged:
            c = chars[1]
            f.append(colors[1])
        elif erase and self.erased:
            c = chars[2]
            f.append(colors[2])
        elif read and self.readed:
            c = chars[0]
            f.append(colors[0])

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
            width=1,
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

    def read(self, block=None, off=None, size=None):
        self._op(Pixel.read, block, off, size)

    def prog(self, block=None, off=None, size=None):
        self._op(Pixel.prog, block, off, size)

    def erase(self, block=None, off=None, size=None):
        self._op(Pixel.erase, block, off, size)

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
            read=False,
            prog=False,
            erase=False,
            wear=False,
            hilbert=False,
            lebesgue=False,
            dots=False,
            braille=False,
            **args):
        # find max wear?
        max_wear = None
        if wear:
            max_wear = max(p.wear for p in self.pixels)

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

        # need to wait for more trace output before rendering
        #
        # this is sort of a hack that knows the output is going to a terminal
        if (braille and self.height < 4) or (dots and self.height < 2):
            needed_height = 4 if braille else 2

            self.history = getattr(self, 'history', [])
            self.history.append(grid.copy())

            if len(self.history)*self.height < needed_height:
                # skip for now
                return None

            grid = list(it.chain.from_iterable(
                    # did we resize?
                    it.islice(it.chain(h, it.repeat(Pixel())),
                            self.width*self.height)
                        for h in self.history))
            self.history = []

        line = []
        if braille:
            # encode into a byte
            for x in range(0, self.width, 2):
                byte_p = 0
                best_p = Pixel()
                for i in range(2*4):
                    p = grid[x+(2-1-(i%2)) + ((row*4)+(4-1-(i//2)))*self.width]
                    best_p |= p
                    if ((read and p.readed)
                            or (prog and p.proged)
                            or (erase and p.erased)
                            or (not read and not prog and not erase
                                and wear and p.worn(max_wear, **args) >= 0.7)):
                        byte_p |= 1 << i

                line.append(best_p.draw(
                        max_wear,
                        CHARS_BRAILLE[byte_p],
                        braille=True,
                        read=read,
                        prog=prog,
                        erase=erase,
                        wear=wear,
                        **args))
        elif dots:
            # encode into a byte
            for x in range(self.width):
                byte_p = 0
                best_p = Pixel()
                for i in range(2):
                    p = grid[x + ((row*2)+(2-1-i))*self.width]
                    best_p |= p
                    if ((read and p.readed)
                            or (prog and p.proged)
                            or (erase and p.erased)
                            or (not read and not prog and not erase
                                and wear and p.worn(max_wear, **args) >= 0.7)):
                        byte_p |= 1 << i

                line.append(best_p.draw(
                        max_wear,
                        CHARS_DOTS[byte_p],
                        dots=True,
                        read=read,
                        prog=prog,
                        erase=erase,
                        wear=wear,
                        **args))
        else:
            for x in range(self.width):
                line.append(grid[x + row*self.width].draw(
                        max_wear,
                        read=read,
                        prog=prog,
                        erase=erase,
                        wear=wear,
                        **args))

        return ''.join(line)



def main(path='-', *,
        block_size=None,
        block_count=None,
        block_cycles=None,
        block=None,
        off=None,
        size=None,
        read=False,
        prog=False,
        erase=False,
        wear=False,
        reset=False,
        no_header=False,
        color='auto',
        dots=False,
        braille=False,
        width=None,
        height=None,
        lines=None,
        head=False,
        cat=False,
        hilbert=False,
        lebesgue=False,
        coalesce=None,
        sleep=None,
        keep_open=False,
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # exclusive wear or read/prog/erase by default
    if not read and not prog and not erase and not wear:
        read = True
        prog = True
        erase = True

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

    if any(isinstance(b, tuple) and len(b) > 1 for b in block):
        print("error: more than one block address?",
                file=sys.stderr)
        sys.exit(-1)
    if isinstance(block[0], tuple):
        block = (block[0][0], *block[1:])
    if len(block) > 1 and isinstance(block[1], tuple):
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

    # create our block device representation
    bmap = Bmap(
            block_size=block_size if block_size is not None else 1,
            block_count=block_count if block_count is not None else 1,
            block_window=block_window,
            off_window=off_window)

    def resize():
        nonlocal bmap

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

        # terminal size changed?
        if width_ != bmap.width or height_ != bmap.height:
            bmap.resize(
                    # scale if we're printing with dots or braille
                    width=2*width_ if braille else width_,
                    height=max(
                        1,
                        4*height_ if braille
                            else 2*height_ if dots
                            else height_))
    resize()

    # keep track of some extra info
    readed = 0
    proged = 0
    erased = 0

    # parse a line of trace output
    pattern = re.compile(
            '^(?P<file>[^:]*):(?P<line>[0-9]+):trace:.*?bd_(?:'
                '(?P<create>create\w*)\('
                    '(?:'
                        'block_size=(?P<block_size>\w+)'
                        '|' 'block_count=(?P<block_count>\w+)'
                        '|' '.*?' ')*'
                    '\)'
                '|' '(?P<read>read)\('
                    '\s*(?P<read_ctx>\w+)' '\s*,'
                    '\s*(?P<read_block>\w+)' '\s*,'
                    '\s*(?P<read_off>\w+)' '\s*,'
                    '\s*(?P<read_buffer>\w+)' '\s*,'
                    '\s*(?P<read_size>\w+)' '\s*\)'
                '|' '(?P<prog>prog)\('
                    '\s*(?P<prog_ctx>\w+)' '\s*,'
                    '\s*(?P<prog_block>\w+)' '\s*,'
                    '\s*(?P<prog_off>\w+)' '\s*,'
                    '\s*(?P<prog_buffer>\w+)' '\s*,'
                    '\s*(?P<prog_size>\w+)' '\s*\)'
                '|' '(?P<erase>erase)\('
                    '\s*(?P<erase_ctx>\w+)' '\s*,'
                    '\s*(?P<erase_block>\w+)'
                    '\s*\(\s*(?P<erase_size>\w+)\s*\)' '\s*\)'
                '|' '(?P<sync>sync)\('
                    '\s*(?P<sync_ctx>\w+)' '\s*\)'
            ')\s*$')
    def parse(line):
        nonlocal bmap
        nonlocal readed
        nonlocal proged
        nonlocal erased

        # string searching is much faster than the regex here, and this
        # actually has a big impact given how much trace output comes
        # through here
        if 'trace' not in line or 'bd' not in line:
            return False
        m = pattern.match(line)
        if not m:
            return False

        if m.group('create'):
            # update our block size/count
            block_size_ = int(m.group('block_size'), 0)
            block_count_ = int(m.group('block_count'), 0)

            if reset:
                bmap = Bmap(
                        block_size=block_size_,
                        block_count=block_count_,
                        block_window=bmap.block_window,
                        off_window=bmap.off_window,
                        width=bmap.width,
                        height=bmap.height)
            elif ((block_size is None
                        and block_size_ != bmap.block_size)
                    or (block_count is None
                        and block_count_ != bmap.block_count)):
                bmap.resize(
                        block_size=block_size if block_size is not None
                            else block_size_,
                        block_count=block_count if block_count is not None
                            else block_count_)
            return True

        elif m.group('read') and read:
            block = int(m.group('read_block'), 0)
            off = int(m.group('read_off'), 0)
            size = int(m.group('read_size'), 0)

            if ((block_size is None and off+size > bmap.block_size)
                    or (block_count is None and block >= bmap.block_count)):
                bmap.resize(
                        block_size=block_size if block_size is not None
                            else max(off+size, bmap.block_size),
                        block_count=block_count if block_count is not None
                            else max(block+1, bmap.block_count))

            bmap.read(block, off, size)
            readed += size
            return True

        elif m.group('prog') and prog:
            block = int(m.group('prog_block'), 0)
            off = int(m.group('prog_off'), 0)
            size = int(m.group('prog_size'), 0)

            if ((block_size is None and off+size > bmap.block_size)
                    or (block_count is None and block >= bmap.block_count)):
                bmap.resize(
                        block_size=block_size if block_size is not None
                            else max(off+size, bmap.block_size),
                        block_count=block_count if block_count is not None
                            else max(block+1, bmap.block_count))

            bmap.prog(block, off, size)
            proged += size
            return True

        elif m.group('erase') and (erase or wear):
            block = int(m.group('erase_block'), 0)
            size = int(m.group('erase_size'), 0)

            if ((block_size is None and size > bmap.block_size)
                    or (block_count is None and block >= bmap.block_count)):
                bmap.resize(
                        block_size=block_size if block_size is not None
                            else max(size, bmap.block_size),
                        block_count=block_count if block_count is not None
                            else max(block+1, bmap.block_count))

            bmap.erase(block, size)
            erased += size
            return True

        else:
            return False

    # print trace output
    def draw(f):
        nonlocal readed
        nonlocal proged
        nonlocal erased

        def writeln(s=''):
            f.write(s)
            f.write('\n')
        f.writeln = writeln

        # don't forget we've scaled this for braille/dots!
        for row in range(
                mt.ceil(bmap.height/4) if braille
                    else mt.ceil(bmap.height/2) if dots
                    else bmap.height):
            line = bmap.draw(row,
                    read=read,
                    prog=prog,
                    erase=erase,
                    wear=wear,
                    block_cycles=block_cycles,
                    color=color,
                    dots=dots,
                    braille=braille,
                    hilbert=hilbert,
                    lebesgue=lebesgue,
                    **args)
            if line:
                f.writeln(line)

        # print some information about read/prog/erases
        #
        # cat implies no-header, because a header wouldn't really make sense
        if not no_header and not cat:
            # compute total ops
            total = readed+proged+erased

            # compute stddev of wear using our bmap, this is a bit different
            # from read/prog/erase which ignores any bmap window, but it's
            # what we have
            if wear:
                mean = (sum(p.wear for p in bmap.pixels)
                        / max(len(bmap.pixels), 1))
                stddev = mt.sqrt(sum((p.wear - mean)**2 for p in bmap.pixels)
                        / max(len(bmap.pixels), 1))
                worst = max((p.wear for p in bmap.pixels), default=0)

            # a bit of a hack here, but this forces our header to always be
            # at row zero
            if len(f.lines) == 0:
                f.lines.append('')
            f.lines[0] = 'bd %dx%d%s%s%s%s' % (
                    bmap.block_size, bmap.block_count,
                    ', %6s read' % ('%.1f%%' % (100*readed  / max(total, 1)))
                        if read else '',
                    ', %6s prog' % ('%.1f%%' % (100*proged / max(total, 1)))
                        if prog else '',
                    ', %6s erase' % ('%.1f%%' % (100*erased / max(total, 1)))
                        if erase else '',
                    ', %13s wear' % ('%.1fσ (%.1f%%)' % (
                            worst / max(stddev, 1),
                            100*stddev / max(worst, 1)))
                        if wear else '')

        bmap.clear()
        readed = 0
        proged = 0
        erased = 0
        resize()


    # read/parse/coalesce operations
    if cat:
        ring = sys.stdout
    else:
        ring = RingIO(lines + (1 if not no_header else 0), head)

    # if sleep print in background thread to avoid getting stuck in a read call
    event = th.Event()
    lock = th.Lock()
    if sleep:
        done = False
        def background():
            while not done:
                event.wait()
                event.clear()
                with lock:
                    draw(ring)
                    if not cat:
                        ring.draw()
                # sleep a minimum amount of time to avoid flickering
                time.sleep(sleep or 0.01)
        th.Thread(target=background, daemon=True).start()

    try:
        while True:
            with openio(path) as f:
                changed = 0
                for line in f:
                    with lock:
                        changed += parse(line)

                        # need to redraw?
                        if changed and (not coalesce or changed >= coalesce):
                            if sleep:
                                event.set()
                            else:
                                draw(ring)
                                if not cat:
                                    ring.draw()
                            changed = 0

            if not keep_open:
                break
            # don't just flood open calls
            time.sleep(sleep or 2)
    except FileNotFoundError as e:
        print("error: file not found %r" % path,
                file=sys.stderr)
        sys.exit(-1)
    except KeyboardInterrupt:
        pass

    if sleep:
        done = True
        lock.acquire() # avoids https://bugs.python.org/issue42717
    if not cat:
        sys.stdout.write('\n')


if __name__ == "__main__":
    import sys
    import argparse
    parser = argparse.ArgumentParser(
            description="Render operations on block devices based on "
                "trace output.",
            allow_abbrev=False)
    parser.add_argument(
            'path',
            nargs='?',
            help="Path to read from.")
    parser.add_argument(
            '-b', '--block-size',
            type=bdgeom,
            help="Block size/geometry in bytes.")
    parser.add_argument(
            '--block-count',
            type=lambda x: int(x, 0),
            help="Block count in blocks.")
    parser.add_argument(
            '-c', '--block-cycles',
            type=lambda x: int(x, 0),
            help="Assumed maximum number of erase cycles when measuring "
                "wear.")
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
            '-r', '--read',
            action='store_true',
            help="Render reads.")
    parser.add_argument(
            '-p', '--prog',
            action='store_true',
            help="Render progs.")
    parser.add_argument(
            '-e', '--erase',
            action='store_true',
            help="Render erases.")
    parser.add_argument(
            '-w', '--wear',
            action='store_true',
            help="Render wear.")
    parser.add_argument(
            '-R', '--reset',
            action='store_true',
            help="Reset wear on block device initialization.")
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
            help="Use 2x4 unicode braille characters. Note that braille "
                "characters sometimes suffer from inconsistent widths.")
    parser.add_argument(
            '--chars',
            help="Characters to use for read, prog, erase, noop operations.")
    parser.add_argument(
            '--wear-chars',
            help="Characters to use for showing wear.")
    parser.add_argument(
            '--colors',
            type=lambda x: [x.strip() for x in x.split(',')],
            help="Colors to use for read, prog, erase, noop operations.")
    parser.add_argument(
            '--wear-colors',
            type=lambda x: [x.strip() for x in x.split(',')],
            help="Colors to use for showing wear.")
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
            help="Show this many lines of history. 0 uses the terminal "
                "height. Defaults to 5.")
    parser.add_argument(
            '-^', '--head',
            action='store_true',
            help="Show the first n lines.")
    parser.add_argument(
            '-z', '--cat',
            action='store_true',
            help="Pipe directly to stdout.")
    parser.add_argument(
            '-U', '--hilbert',
            action='store_true',
            help="Render as a space-filling Hilbert curve.")
    parser.add_argument(
            '-Z', '--lebesgue',
            action='store_true',
            help="Render as a space-filling Z-curve.")
    parser.add_argument(
            '-S', '--coalesce',
            type=lambda x: int(x, 0),
            help="Number of operations to coalesce together.")
    parser.add_argument(
            '-s', '--sleep',
            type=float,
            help="Time in seconds to sleep between reads, coalescing "
                "operations.")
    parser.add_argument(
            '-k', '--keep-open',
            action='store_true',
            help="Reopen the pipe on EOF, useful when multiple "
                "processes are writing.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
