#!/usr/bin/env python3
#
# Render operations on block devices based on trace output
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
import sys
import threading as th
import time


# assign chars/colors to specific bd operations
CHARS = {
    'read':  'r',
    'prog':  'p',
    'erase': 'e',
    'noop':  '-',
}
COLORS = {
    'read':  '32',
    'prog':  '35',
    'erase': '34',
    'noop':  '1;30',
}

# assign chars/colors to varying levels of wear
WEAR_CHARS = '0123456789'
WEAR_COLORS = ['1;30', '1;30', '1;30', '', '', '', '', '31', '31', '1;31']

# give more interesting operations a higher priority
#
# note that while erase is more destructive than prog, all progs subset
# erases, so progs are more interesting
Z_ORDER = ['prog', 'erase', 'read', 'wear', 'noop']

# TODO rm
#CHARS = 'rpe-'
#COLORS = ['32', '35', '34', '90']
#
#WEAR_CHARS = '-123456789'
#WEAR_COLORS = ['90', '', '', '', '', '', '', '35', '35', '1;31']

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
def punescape(s, attrs=None):
    # TODO punescape should just take something that provides __getitem__
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


# an abstract block representation
class TraceBlock:
    def __init__(self, block, *,
            readed=None, proged=None, erased=None, wear=0,
            x=None, y=None, width=None, height=None):
        self.block = block
        self.readed = readed if readed is not None else RangeSet()
        self.proged = proged if proged is not None else RangeSet()
        self.erased = erased if erased is not None else RangeSet()
        self.wear = wear
        self.x = x
        self.y = y
        self.width = width
        self.height = height

    def __repr__(self):
        return 'TraceBlock(0x%x, x=%s, y=%s, width=%s, height=%s)' % (
                self.block,
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

    # generate attrs for punescaping
    @ft.cached_property
    def attrs(self):
        # really the only reasonable attrs are block and wear
        return {
            'block': self.block,
            'wear': self.wear,
        }

    # some simulated bd operations
    def read(self, off, size):
        self.readed.add(range(off, off+size))

    def prog(self, off, size):
        self.proged.add(range(off, off+size))

    def erase(self, off, size, *,
            wear=0):
        self.erased.add(range(off, off+size))
        self.wear += wear

    def clear(self):
        self.readed = RangeSet()
        self.proged = RangeSet()
        self.erased = RangeSet()







## space filling Hilbert-curve
##
## note we memoize the last curve since this is a bit expensive
##
#@ft.lru_cache(1)
#def hilbert_curve(width, height):
#    # based on generalized Hilbert curves:
#    # https://github.com/jakubcerveny/gilbert
#    #
#    def hilbert_(x, y, a_x, a_y, b_x, b_y):
#        w = abs(a_x+a_y)
#        h = abs(b_x+b_y)
#        a_dx = -1 if a_x < 0 else +1 if a_x > 0 else 0
#        a_dy = -1 if a_y < 0 else +1 if a_y > 0 else 0
#        b_dx = -1 if b_x < 0 else +1 if b_x > 0 else 0
#        b_dy = -1 if b_y < 0 else +1 if b_y > 0 else 0
#
#        # trivial row
#        if h == 1:
#            for _ in range(w):
#                yield (x,y)
#                x, y = x+a_dx, y+a_dy
#            return
#
#        # trivial column
#        if w == 1:
#            for _ in range(h):
#                yield (x,y)
#                x, y = x+b_dx, y+b_dy
#            return
#
#        a_x_, a_y_ = a_x//2, a_y//2
#        b_x_, b_y_ = b_x//2, b_y//2
#        w_ = abs(a_x_+a_y_)
#        h_ = abs(b_x_+b_y_)
#
#        if 2*w > 3*h:
#            # prefer even steps
#            if w_ % 2 != 0 and w > 2:
#                a_x_, a_y_ = a_x_+a_dx, a_y_+a_dy
#
#            # split in two
#            yield from hilbert_(x, y, a_x_, a_y_, b_x, b_y)
#            yield from hilbert_(x+a_x_, y+a_y_, a_x-a_x_, a_y-a_y_, b_x, b_y)
#        else:
#            # prefer even steps
#            if h_ % 2 != 0 and h > 2:
#                b_x_, b_y_ = b_x_+b_dx, b_y_+b_dy
#
#            # split in three
#            yield from hilbert_(x, y, b_x_, b_y_, a_x_, a_y_)
#            yield from hilbert_(x+b_x_, y+b_y_, a_x, a_y, b_x-b_x_, b_y-b_y_)
#            yield from hilbert_(
#                    x+(a_x-a_dx)+(b_x_-b_dx), y+(a_y-a_dy)+(b_y_-b_dy),
#                    -b_x_, -b_y_, -(a_x-a_x_), -(a_y-a_y_))
#
#    if width >= height:
#        curve = hilbert_(0, 0, +width, 0, 0, +height)
#    else:
#        curve = hilbert_(0, 0, 0, +height, +width, 0)
#
#    return list(curve)
#
## space filling Z-curve/Lebesgue-curve
##
## note we memoize the last curve since this is a bit expensive
##
#@ft.lru_cache(1)
#def lebesgue_curve(width, height):
#    # we create a truncated Z-curve by simply filtering out the points
#    # that are outside our region
#    curve = []
#    for i in range(2**(2*mt.ceil(mt.log2(max(width, height))))):
#        # we just operate on binary strings here because it's easier
#        b = '{:0{}b}'.format(i, 2*mt.ceil(mt.log2(i+1)/2))
#        x = int(b[1::2], 2) if b[1::2] else 0
#        y = int(b[0::2], 2) if b[0::2] else 0
#        if x < width and y < height:
#            curve.append((x, y))
#
#    return curve
#
#
#class Pixel(int):
#    __slots__ = ()
#    def __new__(cls, state=0, *,
#            wear=0,
#            readed=False,
#            proged=False,
#            erased=False):
#        return super().__new__(cls,
#                state
#                    | (wear << 3)
#                    | (1 if readed else 0)
#                    | (2 if proged else 0)
#                    | (4 if erased else 0))
#
#    @property
#    def wear(self):
#        return self >> 3
#
#    @property
#    def readed(self):
#        return (self & 1) != 0
#
#    @property
#    def proged(self):
#        return (self & 2) != 0
#
#    @property
#    def erased(self):
#        return (self & 4) != 0
#
#    def read(self):
#        return Pixel(int(self) | 1)
#
#    def prog(self):
#        return Pixel(int(self) | 2)
#
#    def erase(self):
#        return Pixel((int(self) | 4) + 8)
#
#    def clear(self):
#        return Pixel(int(self) & ~7)
#
#    def __or__(self, other):
#        return Pixel(
#                (int(self) | int(other)) & 7,
#                wear=max(self.wear, other.wear))
#
#    def worn(self, max_wear, *,
#            block_cycles=None,
#            wear_chars=None,
#            **_):
#        if wear_chars is None:
#            wear_chars = WEAR_CHARS
#
#        if block_cycles:
#            return self.wear / block_cycles
#        else:
#            return self.wear / max(max_wear, len(wear_chars))
#
#    def draw(self, max_wear, char=None, *,
#            reads=True,
#            progs=True,
#            erases=True,
#            wear=False,
#            block_cycles=None,
#            color=True,
#            dots=False,
#            braille=False,
#            chars=None,
#            wear_chars=None,
#            colors=None,
#            wear_colors=None,
#            **_):
#        # fallback to default chars/colors
#        if chars is None:
#            chars = CHARS
#        if len(chars) < len(CHARS):
#            chars = chars + CHARS[len(chars):]
#
#        if colors is None:
#            colors = COLORS
#        if len(colors) < len(COLORS):
#            colors = colors + COLORS[len(colors):]
#
#        if wear_chars is None:
#            wear_chars = WEAR_CHARS
#
#        if wear_colors is None:
#            wear_colors = WEAR_COLORS
#
#        # compute char/color
#        c = chars[3]
#        f = [colors[3]]
#
#        if wear:
#            w = min(self.worn(
#                        max_wear,
#                        block_cycles=block_cycles,
#                        wear_chars=wear_chars),
#                    1)
#
#            c = wear_chars[int(w * (len(wear_chars)-1))]
#            f.append(wear_colors[int(w * (len(wear_colors)-1))])
#
#        if progs and self.proged:
#            c = chars[1]
#            f.append(colors[1])
#        elif erases and self.erased:
#            c = chars[2]
#            f.append(colors[2])
#        elif reads and self.readed:
#            c = chars[0]
#            f.append(colors[0])
#
#        # override char?
#        if char:
#            c = char
#
#        # apply colors
#        if f and color:
#            c = '%s%s\x1b[m' % (
#                    ''.join('\x1b[%sm' % f_ for f_ in f),
#                    c)
#
#        return c
#
#
#class Bmap:
#    def __init__(self, *,
#            block_size=1,
#            block_count=1,
#            block_window=None,
#            off_window=None,
#            width=1,
#            height=1,
#            pixels=None):
#        # default width to block_window or block_size
#        if width is None:
#            if block_window is not None:
#                width = len(block_window)
#            else:
#                width = block_count
#
#        # allocate pixels if not provided
#        if pixels is None:
#            pixels = [Pixel() for _ in range(width*height)]
#
#        self.pixels = pixels
#        self.block_size = block_size
#        self.block_count = block_count
#        self.block_window = block_window
#        self.off_window = off_window
#        self.width = width
#        self.height = height
#
#    @property
#    def _block_window(self):
#        if self.block_window is None:
#            return range(0, self.block_count)
#        else:
#            return self.block_window
#
#    @property
#    def _off_window(self):
#        if self.off_window is None:
#            return range(0, self.block_size)
#        else:
#            return self.off_window
#
#    @property
#    def _window(self):
#        return len(self._off_window)*len(self._block_window)
#
#    def _op(self, f, block=None, off=None, size=None):
#        if block is None:
#            range_ = range(len(self.pixels))
#        else:
#            if off is None:
#                off, size = 0, self.block_size
#            elif size is None:
#                off, size = 0, off
#
#            # map into our window
#            if block not in self._block_window:
#                return
#            block -= self._block_window.start
#
#            size = (max(self._off_window.start,
#                        min(self._off_window.stop, off+size))
#                    - max(self._off_window.start,
#                        min(self._off_window.stop, off)))
#            off = (max(self._off_window.start,
#                        min(self._off_window.stop, off))
#                    - self._off_window.start)
#            if size == 0:
#                return
#
#            # map to our block space
#            range_ = range(
#                    block*len(self._off_window) + off,
#                    block*len(self._off_window) + off+size)
#            range_ = range(
#                    (range_.start*len(self.pixels)) // self._window,
#                    (range_.stop*len(self.pixels)) // self._window)
#            range_ = range(
#                    range_.start,
#                    max(range_.stop, range_.start+1))
#
#        # apply the op
#        for i in range_:
#            self.pixels[i] = f(self.pixels[i])
#
#    def read(self, block=None, off=None, size=None):
#        self._op(Pixel.read, block, off, size)
#
#    def prog(self, block=None, off=None, size=None):
#        self._op(Pixel.prog, block, off, size)
#
#    def erase(self, block=None, off=None, size=None):
#        self._op(Pixel.erase, block, off, size)
#
#    def clear(self, block=None, off=None, size=None):
#        self._op(Pixel.clear, block, off, size)
#
#    def resize(self, *,
#            block_size=None,
#            block_count=None,
#            width=None,
#            height=None):
#        block_size = (block_size if block_size is not None
#                else self.block_size)
#        block_count = (block_count if block_count is not None
#                else self.block_count)
#        width = width if width is not None else self.width
#        height = height if height is not None else self.height
#
#        if (block_size == self.block_size
#                and block_count == self.block_count
#                and width == self.width
#                and height == self.height):
#            return
#
#        # transform our pixels
#        self.block_size = block_size
#        self.block_count = block_count
#
#        pixels = []
#        for x in range(width*height):
#            # map into our old bd space
#            range_ = range(
#                    (x*self._window) // (width*height),
#                    ((x+1)*self._window) // (width*height))
#            range_ = range(
#                    range_.start,
#                    max(range_.stop, range_.start+1))
#
#            # aggregate state
#            pixels.append(ft.reduce(
#                    Pixel.__or__,
#                    self.pixels[range_.start:range_.stop],
#                    Pixel()))
#            
#        self.width = width
#        self.height = height
#        self.pixels = pixels
#
#    def draw(self, row, *,
#            reads=False,
#            progs=False,
#            erases=False,
#            wear=False,
#            hilbert=False,
#            lebesgue=False,
#            dots=False,
#            braille=False,
#            **args):
#        # find max wear?
#        max_wear = None
#        if wear:
#            max_wear = max(p.wear for p in self.pixels)
#
#        # fold via a curve?
#        if hilbert:
#            grid = [None]*(self.width*self.height)
#            for (x,y), p in zip(
#                    hilbert_curve(self.width, self.height),
#                    self.pixels):
#                grid[x + y*self.width] = p
#        elif lebesgue:
#            grid = [None]*(self.width*self.height)
#            for (x,y), p in zip(
#                    lebesgue_curve(self.width, self.height),
#                    self.pixels):
#                grid[x + y*self.width] = p
#        else:
#            grid = self.pixels
#
#        # need to wait for more trace output before rendering
#        #
#        # this is sort of a hack that knows the output is going to a terminal
#        if (braille and self.height < 4) or (dots and self.height < 2):
#            needed_height = 4 if braille else 2
#
#            self.history = getattr(self, 'history', [])
#            self.history.append(grid.copy())
#
#            if len(self.history)*self.height < needed_height:
#                # skip for now
#                return None
#
#            grid = list(it.chain.from_iterable(
#                    # did we resize?
#                    it.islice(it.chain(h, it.repeat(Pixel())),
#                            self.width*self.height)
#                        for h in self.history))
#            self.history = []
#
#        line = []
#        if braille:
#            # encode into a byte
#            for x in range(0, self.width, 2):
#                byte_p = 0
#                best_p = Pixel()
#                for i in range(2*4):
#                    p = grid[x+(2-1-(i%2)) + ((row*4)+(4-1-(i//2)))*self.width]
#                    best_p |= p
#                    if ((reads and p.readed)
#                            or (progs and p.proged)
#                            or (erases and p.erased)
#                            or (not reads and not progs and not erases
#                                and wear and p.worn(max_wear, **args) >= 0.7)):
#                        byte_p |= 1 << i
#
#                line.append(best_p.draw(
#                        max_wear,
#                        CHARS_BRAILLE[byte_p],
#                        braille=True,
#                        reads=reads,
#                        progs=progs,
#                        erases=erases,
#                        wear=wear,
#                        **args))
#        elif dots:
#            # encode into a byte
#            for x in range(self.width):
#                byte_p = 0
#                best_p = Pixel()
#                for i in range(2):
#                    p = grid[x + ((row*2)+(2-1-i))*self.width]
#                    best_p |= p
#                    if ((reads and p.readed)
#                            or (progs and p.proged)
#                            or (erases and p.erased)
#                            or (not reads and not progs and not erases
#                                and wear and p.worn(max_wear, **args) >= 0.7)):
#                        byte_p |= 1 << i
#
#                line.append(best_p.draw(
#                        max_wear,
#                        CHARS_DOTS[byte_p],
#                        dots=True,
#                        reads=reads,
#                        progs=progs,
#                        erases=erases,
#                        wear=wear,
#                        **args))
#        else:
#            for x in range(self.width):
#                line.append(grid[x + row*self.width].draw(
#                        max_wear,
#                        reads=reads,
#                        progs=progs,
#                        erases=erases,
#                        wear=wear,
#                        **args))
#
#        return ''.join(line)



def main(path='-', *,
        block_size=None,
        block_count=None,
        blocks=None,
        reads=False,
        progs=False,
        erases=False,
        wear=False,
        wear_only=False,
        block_cycles=None,
        volatile=False,
        chars=[],
        wear_chars=[],
        colors=[],
        wear_colors=[],
        # TODO this should be color='auto' in all scripts
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
        aspect_ratio=(1,1),
        tiny=False,
        title=None,
        padding=0,
        lines=None,
        head=False,
        cat=False,
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

    # if not specified default to all ops
    if not reads and not progs and not erases and not wear_only:
        progs = True
        reads = True
        erases = True

    # wear_only implies only wear
    if wear_only:
        wear = True

    # block_cycles implies wear
    if block_cycles is not None:
        wear = True

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
    chars_ = Attr(chars_,
            defaults=[True] if braille or dots else CHARS)

    wear_chars_ = []
    for char in wear_chars:
        if isinstance(char, tuple):
            wear_chars_.extend((char[0], c) for c in psplit(char[1]))
        else:
            wear_chars_.extend(psplit(char))
    wear_chars_ = Attr(wear_chars_,
            defaults=[True] if braille or dots else WEAR_CHARS)

    colors_ = Attr(colors, defaults=COLORS)

    wear_colors_ = Attr(wear_colors, defaults=WEAR_COLORS)

    # is bd geometry specified?
    if isinstance(block_size, tuple):
        block_size, block_count_ = block_size
        if block_count is None:
            block_count = block_count_

    # keep track of block_size/block_count and block map state
    block_size_ = block_size
    block_count_ = block_count
    bmap = None
    # keep track of some extra info
    readed = 0
    proged = 0
    erased = 0

    def bmap_init(block_size__, block_count__):
        nonlocal block_size_
        nonlocal block_count_
        nonlocal bmap
        nonlocal readed
        nonlocal proged 
        nonlocal erased

        # keep track of block_size/block_count
        if block_size is None:
            block_size_ = block_size__
        if block_count is None:
            block_count_ = block_count__

        # flatten blocks, default to all blocks
        blocks_ = list(
                range(blocks.start or 0, blocks.stop or block_count_)
                        if isinstance(blocks, slice)
                        else range(blocks, blocks+1)
                    if blocks
                    else range(block_count_))

        # create a new block map?
        if bmap is None or volatile:
            bmap = {b: TraceBlock(b) for b in blocks_}
            readed = 0
            proged = 0
            erased = 0

        # just resize block map
        else:
            bmap = {b: bmap[b] if b in bmap else TraceBlock(b)
                    for b in blocks_}

    # if we know block_count, go ahead and flatten blocks + create
    # a block map, otherwise we need to wait for first bd init
    if block_size is not None and block_count is not None:
        bmap_init(block_size, block_count)


    ## trace parser

    # precompute trace regexes

    # TODO rm me
#    trace_pattern = re.compile(
#            '^(?P<file>[^:]*):(?P<line>[0-9]+):trace:.*?bd_(?:'
#                '(?P<create>create\w*)\('
#                    '(?:'
#                        'block_size=(?P<block_size>\w+)'
#                        '|' 'block_count=(?P<block_count>\w+)'
#                        '|' '.*?' ')*'
#                    '\)'
#                '|' '(?P<read>read)\('
#                    '\s*(?P<read_ctx>\w+)' '\s*,'
#                    '\s*(?P<read_block>\w+)' '\s*,'
#                    '\s*(?P<read_off>\w+)' '\s*,'
#                    '\s*(?P<read_buffer>\w+)' '\s*,'
#                    '\s*(?P<read_size>\w+)' '\s*\)'
#                '|' '(?P<prog>prog)\('
#                    '\s*(?P<prog_ctx>\w+)' '\s*,'
#                    '\s*(?P<prog_block>\w+)' '\s*,'
#                    '\s*(?P<prog_off>\w+)' '\s*,'
#                    '\s*(?P<prog_buffer>\w+)' '\s*,'
#                    '\s*(?P<prog_size>\w+)' '\s*\)'
#                '|' '(?P<erase>erase)\('
#                    '\s*(?P<erase_ctx>\w+)' '\s*,'
#                    '\s*(?P<erase_block>\w+)'
#                    '\s*\(\s*(?P<erase_size>\w+)\s*\)' '\s*\)'
#                '|' '(?P<sync>sync)\('
#                    '\s*(?P<sync_ctx>\w+)' '\s*\)'
#            ')\s*$')

    init_pattern = re.compile(
            '^(?P<file>[^ :]*):(?P<line>[0-9]+):trace:'
                '.*?bd_createcfg\('
                    '\s*(?P<ctx>\w+)'
                    '(?:'
                        'block_size=(?P<block_size>\w+)'
                        '|' 'block_count=(?P<block_count>\w+)'
                        '|' '.*?' ')*' '\)')
    read_pattern = re.compile(
            '^(?P<file>[^ :]*):(?P<line>[0-9]+):trace:'
                '.*?bd_read\('
                    '\s*(?P<ctx>\w+)' '\s*,'
                    '\s*(?P<block>\w+)' '\s*,'
                    '\s*(?P<off>\w+)' '\s*,'
                    '\s*(?P<buffer>\w+)' '\s*,'
                    '\s*(?P<size>\w+)' '\s*\)')
    prog_pattern = re.compile(
            '^(?P<file>[^ :]*):(?P<line>[0-9]+):trace:'
                '.*?bd_prog\('
                    '\s*(?P<ctx>\w+)' '\s*,'
                    '\s*(?P<block>\w+)' '\s*,'
                    '\s*(?P<off>\w+)' '\s*,'
                    '\s*(?P<buffer>\w+)' '\s*,'
                    '\s*(?P<size>\w+)' '\s*\)')
    erase_pattern = re.compile(
            '^(?P<file>[^ :]*):(?P<line>[0-9]+):trace:'
                '.*?bd_erase\('
                    '\s*(?P<ctx>\w+)' '\s*,'
                    '\s*(?P<block>\w+)'
                    '(?:\s*\(\s*(?P<size>\w+)\s*\))?' '\s*\)')
    sync_pattern = re.compile(
            '^(?P<file>[^ :]*):(?P<line>[0-9]+):trace:'
                '.*?bd_sync\('
                    '\s*(?P<ctx>\w+)' '\s*\)')

    def trace__(line):
        nonlocal readed
        nonlocal proged
        nonlocal erased

        # string searching is much faster than the regex here, this
        # actually has a big impact given the sheer quantity of how much
        # trace output we have to deal with
        if ('trace' not in line
                # ignore return trace statements
                or '->' in line):
            return False

        # note we can't do most ops until we know block_count/block_size

        # bd init?
        if 'bd_createcfg(' in line:
            m = init_pattern.match(line)
            if not m:
                return False
            # block_size/block_count missing?
            if not m.group('block_size') or not m.group('block_count'):
                return False
            block_size__ = int(m.group('block_size'), 0)
            block_count__ = int(m.group('block_count'), 0)

            bmap_init(block_size__, block_count__)
            return True

        # bd read?
        elif reads and bmap is not None and 'bd_read(' in line:
            m = read_pattern.match(line)
            if not m:
                return False
            block = int(m.group('block'), 0)
            off = int(m.group('off'), 0)
            size = int(m.group('size'), 0)

            if block not in bmap:
                return False
            else:
                bmap[block].read(off, size)
                readed += size
                return True

        # bd prog?
        elif progs and bmap is not None and 'bd_prog(' in line:
            m = prog_pattern.match(line)
            if not m:
                return False
            block = int(m.group('block'), 0)
            off = int(m.group('off'), 0)
            size = int(m.group('size'), 0)

            if block not in bmap:
                return False
            else:
                bmap[block].prog(off, size)
                proged += size
                return True

        # bd erase?
        elif (erases or wear) and bmap is not None and 'bd_erase(' in line:
            m = erase_pattern.match(line)
            if not m:
                return False

            block = int(m.group('block'), 0)
            if block not in bmap:
                return False
            else:
                bmap[block].erase(0, block_size_,
                        wear=+1 if wear else 0)
                erased += block_count_
                return True

        else:
            return False


    ## bmap renderer

    # these curves are expensive to calculate, so memoize these
    if hilbert:
        curve = ft.lru_cache(16)(lambda w, h: list(hilbert_curve(w, h)))
    elif lebesgue:
        curve = ft.lru_cache(16)(lambda w, h: list(lebesgue_curve(w, h)))
    else:
        curve = ft.lru_cache(16)(lambda w, h: list(naive_curve(w, h)))

    # TODO adopt f -> ring name in all scripts?
    def draw__(ring, width, height):
        nonlocal bmap
        # still waiting on bd init
        if bmap is None:
            return

        # compute total ops
        total = readed + proged + erased

        # if we're showing wear, find min/max/avg/etc
        if wear:
            wear_min = min(b.wear for b in bmap.values())
            wear_max = max(b.wear for b in bmap.values())
            wear_avg = (sum(b.wear for b in bmap.values())
                    / max(len(bmap), 1))
            wear_stddev = mt.sqrt(
                    sum((b.wear - wear_avg)**2 for b in bmap.values())
                        / max(len(bmap), 1))

            # if block_cycles isn't provide, scale based on max wear
            if block_cycles is not None:
                block_cycles_ = block_cycles
            else:
                block_cycles_ = wear_max

        # give ring a writeln function
        def writeln(s=''):
            ring.write(s)
            ring.write('\n')
        ring.writeln = writeln

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

        # create a canvas
        canvas = Canvas(
                width_,
                height_ - (1 if not no_header else 0),
                color=color,
                dots=dots,
                braille=braille)

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
                while (abs(((canvas.width/(block_cols_ * 2))
                                / (canvas.height/mt.ceil(block_rows_ / 2)))
                            - block_ratio)
                        < abs(((canvas.width/block_cols_)
                                / (canvas.height/block_rows_)))
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

                # bump up to at least one pixel for every block, dont't
                # worry about out-of-bounds, Canvas handles this for us
                b.width = max(b.width, 1)
                b.height = max(b.height, 1)

        # TODO chars should probably take priority over braille/dots
        # TODO limit these based on requested ops?
        # assign chars based on op + block
        for b in bmap.values():
            b.chars = {}
            for op in ((['read'] if reads else [])
                    + (['prog'] if progs else [])
                    + (['erase'] if erases else [])
                    + ['noop']):
                char__ = chars_.get((b.block, (op, '0x%x' % b.block)))
                if char__ is not None:
                    if isinstance(char__, str):
                        # don't punescape unless we have to
                        if '%' in char__:
                            char__ = punescape(char__, b.attrs)
                        char__ = char__[0] # limit to 1 char
                    b.chars[op] = char__

        # assign colors based on op + block
        for b in bmap.values():
            b.colors = {}
            for op in ((['read'] if reads else [])
                    + (['prog'] if progs else [])
                    + (['erase'] if erases else [])
                    + ['noop']):
                color__ = colors_.get((b.block, (op, '0x%x' % b.block)))
                if color__ is not None:
                    # don't punescape unless we have to
                    if '%' in color__:
                        color__ = punescape(color__, b.attrs)
                    b.colors[op] = color__

        # assign wear chars based on block
        if wear:
            for b in bmap.values():
                b.wear_chars = []
                for char__ in wear_chars_.getall((b.block, '0x%x' % b.block)):
                    if isinstance(char__, str):
                        # don't punescape unless we have to
                        if '%' in char__:
                            char__ = punescape(char__, b.attrs)
                        char__ = char__[0] # limit to 1 char
                    b.wear_chars.append(char__)

        # assign wear colors based on block
        if wear:
            for b in bmap.values():
                b.wear_colors = []
                for color__ in wear_colors_.getall((b.block, '0x%x' % b.block)):
                    # don't punescape unless we have to
                    if '%' in color__:
                        color__ = punescape(color__, b.attrs)
                    b.wear_colors.append(color__)

        # render to canvas in a specific z-order that prioritizes
        # interesting ops
        for op in reversed(Z_ORDER):
            # TODO if we're rendering noops as dashes here, should we render
            # unused as dashes even in usage mode in dbgbmap.py?
            # don't render noops in braille/dots mode
            if (braille or dots) and op == 'noop':
                continue
            # skip ops we're not interested in
            if ((not reads and op == 'read')
                    or (not progs and op == 'prog')
                    or (not erases and op == 'erase')
                    or (not wear and op == 'wear')):
                continue

            for b in bmap.values():
                if op == 'read':
                    ranges__ = b.readed
                    char__ = b.chars['read']
                    color__ = b.colors['read']
                elif op == 'prog':
                    ranges__ = b.proged
                    char__ = b.chars['prog']
                    color__ = b.colors['prog']
                elif op == 'erase':
                    ranges__ = b.erased
                    char__ = b.chars['erase']
                    color__ = b.colors['erase']
                elif op == 'wear':
                    # _no_ wear?
                    if b.wear == 0:
                        continue
                    ranges__ = RangeSet([range(block_size_)])
                    # scale char/color based on either block_cycles
                    # or wear_avg
                    if block_cycles is not None:
                        wear__ = min(b.wear / max(block_cycles, 1), 1.0)
                    else:
                        wear__ = min(b.wear / max(2*wear_avg, 1), 1.0)
                    char__ = b.wear_chars[int(wear__*(len(b.wear_chars)-1))]
                    color__ = b.wear_colors[int(wear__*(len(b.wear_colors)-1))]
                else:
                    ranges__ = RangeSet([range(block_size_)])
                    char__ = b.chars['noop']
                    color__ = b.colors['noop']

                if not ranges__:
                    continue

                # contiguous?
                if contiguous:
                    for range__ in ranges__.ranges():
                        # where are we in the curve?
                        block__ = b.block - global_block
                        range__ = range(
                                mt.floor(((block__*block_size_ + range__.start)
                                        / (block_size_ * len(bmap)))
                                    * len(global_curve)),
                                mt.ceil(((block__*block_size_ + range__.stop)
                                        / (block_size_ * len(bmap)))
                                    * len(global_curve)))

                        # map to global curve
                        for i in range__:
                            if i >= len(global_curve):
                                continue
                            x__, y__ = global_curve[i]

                            # flip y
                            y__ = canvas.height - (y__+1)

                            canvas.point(x__, y__,
                                    char=char__,
                                    color=color__)

                # blocky?
                else:
                    x__ = b.x
                    y__ = b.y
                    width__ = b.width
                    height__ = b.height

                    # flip y
                    y__ = canvas.height - (y__+height__)

                    for range__ in ranges__.ranges():
                        # scale from bytes -> pixels
                        range__ = range(
                                mt.floor((range__.start/block_size_)
                                    * (width__*height__)),
                                mt.ceil((range__.stop/block_size_)
                                    * (width__*height__)))
                        # map to in-block curve
                        for i, (dx, dy) in enumerate(curve(width__, height__)):
                            if i in range__:
                                # flip y
                                canvas.point(x__+dx, y__+(height__-(dy+1)),
                                        char=char__,
                                        color=color__)

        # print some summary info
        if not no_header:
# TODO
#            # compute stddev of wear using our bmap, this is a bit different
#            # from reads/progs/erases which ignores any bmap window, but it's
#            # what we have
#            if wear:
#                mean = (sum(p.wear for p in bmap.pixels)
#                        / max(len(bmap.pixels), 1))
#                stddev = mt.sqrt(sum((p.wear - mean)**2 for p in bmap.pixels)
#                        / max(len(bmap.pixels), 1))
#                worst = max((p.wear for p in bmap.pixels), default=0)

            if title:
                ring.writeln(punescape(title, {
                    # TODO simplify this in other scripts
                    # geometry -> just block_size/block_count, don't eagerly
                    # format percents, etc
                    'block_size': block_size_,
                    'block_count': block_count_,
                    'total': total,
                    'read': readed,
                    # TODO if we're showing percentage of total ops here,
                    # should we should percentage of in-use blocks in
                    # dbgbmap.py?
                    'read_percent': 100*readed / max(total, 1),
                    'prog': proged,
                    'prog_percent': 100*proged / max(total, 1),
                    'erase': erased,
                    'erase_percent': 100*erased / max(total, 1),
                    'wear_min': wear_min if wear else '?',
                    'wear_min_percent': 100*wear_min / max(block_cycles_, 1)
                            if wear else '?',
                    'wear_max': wear_max if wear else '?',
                    'wear_max_percent': 100*wear_max / max(block_cycles_, 1)
                            if wear else '?',
                    'wear_avg': wear_avg if wear else '?',
                    'wear_avg_percent': 100*wear_avg / max(block_cycles_, 1)
                            if wear else '?',
                    'wear_stddev': wear_stddev if wear else '?',
                    'wear_stddev_percent':
                            100*wear_stddev / max(block_cycles_, 1)
                                if wear else '?',
                }))
            else:
#                ring.writeln('curve: %s' % (curve.cache_info(),))
                ring.writeln('bd %dx%d%s%s%s%s' % (
                        block_size_, block_count_,
                        ', %6s read' % (
                                '%.1f%%' % (100*readed / max(total, 1)))
                            if reads else '',
                        ', %6s prog' % (
                                '%.1f%%' % (100*proged / max(total, 1)))
                            if progs else '',
                        ', %6s erase' % (
                                '%.1f%%' % (100*erased / max(total, 1)))
                            if erases else '',
                        ', %15s wear' % (
                                '%.1f%% +-%.1fσ' % (
                                    100*wear_avg / max(block_cycles_, 1),
                                    100*wear_stddev / max(block_cycles_, 1)))
                            if wear else ''))

        # draw canvas
        for row in range(canvas.height//canvas.yscale):
            line = canvas.draw(row)
            ring.writeln(line)

        # clear bmap
        for b in bmap.values():
            b.clear()



# TODO rm me
#
#
#
#
#
#
#
#
#
#    # exclusive wear or reads/progs/erases by default
#    if not reads and not progs and not erases and not wear:
#        reads = True
#        progs = True
#        erases = True
#
#    # assume a reasonable lines/height if not specified
#    #
#    # note that we let height = None if neither hilbert or lebesgue
#    # are specified, this is a bit special as the default may be less
#    # than one character in height.
#    if height is None and (hilbert or lebesgue):
#        if lines is not None:
#            height = lines
#        else:
#            height = 5
#
#    if lines is None:
#        if height is not None:
#            lines = height
#        else:
#            lines = 5
#
#    # is bd geometry specified?
#    if isinstance(block_size, tuple):
#        block_size, block_count_ = block_size
#        if block_count is None:
#            block_count = block_count_
#
#    # try to simplify the block/off/size arguments a bit
#    if not isinstance(block, dict):
#        block = {'block': block}
#    block, off, size = (
#            block.get('block'),
#            block.get('off'),
#            block.get('size'))
#
#    if not isinstance(block, tuple):
#        block = block,
#    if isinstance(off, tuple) and len(off) == 1:
#        off, = off
#    if isinstance(size, tuple) and len(size) == 1:
#        if off is None:
#            off, = size
#        size = None
#
#    if any(isinstance(b, tuple) and len(b) > 1 for b in block):
#        print("error: more than one block address?",
#                file=sys.stderr)
#        sys.exit(-1)
#    if isinstance(block[0], tuple):
#        block = (block[0][0], *block[1:])
#    if len(block) > 1 and isinstance(block[1], tuple):
#        block = (block[0], block[1][0])
#    if isinstance(block[0], tuple):
#        block, off_ = (block[0][0], *block[1:]), block[0][1]
#        if off is None:
#            off = off_
#    if len(block) > 1 and isinstance(block[1], tuple):
#        block = (block[0], block[1][0])
#    if len(block) == 1:
#        block, = block
#
#    if isinstance(off, tuple):
#        off, size_ = off[0], off[1] - off[0]
#        if size is None:
#            size = size_
#    if isinstance(size, tuple):
#        off_, size = off[0], off[1] - off[0]
#        if off is None:
#            off = off_
#
#    # is a block window specified?
#    block_window = None
#    if block is not None:
#        if isinstance(block, tuple):
#            block_window = range(*block)
#        else:
#            block_window = range(block, block+1)
#
#    off_window = None
#    if off is not None or size is not None:
#        off_ = off if off is not None else 0
#        size_ = size if size is not None else 1
#        off_window = range(off_, off_+size_)
#
#    # create our block device representation
#    bmap = Bmap(
#            block_size=block_size if block_size is not None else 1,
#            block_count=block_count if block_count is not None else 1,
#            block_window=block_window,
#            off_window=off_window)
#
#    def resize():
#        nonlocal bmap
#
#        # figure out best width/height
#        if width is None:
#            width_ = min(80, shutil.get_terminal_size((80, 5))[0])
#        elif width:
#            width_ = width
#        else:
#            width_ = shutil.get_terminal_size((80, 5))[0]
#
#        if height is None:
#            height_ = 0
#        elif height:
#            height_ = height
#        else:
#            height_ = shutil.get_terminal_size((80, 5))[1]
#
#        # terminal size changed?
#        if width_ != bmap.width or height_ != bmap.height:
#            bmap.resize(
#                    # scale if we're printing with dots or braille
#                    width=2*width_ if braille else width_,
#                    height=max(
#                        1,
#                        4*height_ if braille
#                            else 2*height_ if dots
#                            else height_))
#    resize()
#
#    # keep track of some extra info
#    readed = 0
#    proged = 0
#    erased = 0
#
#    # parse a line of trace output
#    pattern = re.compile(
#            '^(?P<file>[^:]*):(?P<line>[0-9]+):trace:.*?bd_(?:'
#                '(?P<create>create\w*)\('
#                    '(?:'
#                        'block_size=(?P<block_size>\w+)'
#                        '|' 'block_count=(?P<block_count>\w+)'
#                        '|' '.*?' ')*'
#                    '\)'
#                '|' '(?P<read>read)\('
#                    '\s*(?P<read_ctx>\w+)' '\s*,'
#                    '\s*(?P<read_block>\w+)' '\s*,'
#                    '\s*(?P<read_off>\w+)' '\s*,'
#                    '\s*(?P<read_buffer>\w+)' '\s*,'
#                    '\s*(?P<read_size>\w+)' '\s*\)'
#                '|' '(?P<prog>prog)\('
#                    '\s*(?P<prog_ctx>\w+)' '\s*,'
#                    '\s*(?P<prog_block>\w+)' '\s*,'
#                    '\s*(?P<prog_off>\w+)' '\s*,'
#                    '\s*(?P<prog_buffer>\w+)' '\s*,'
#                    '\s*(?P<prog_size>\w+)' '\s*\)'
#                '|' '(?P<erase>erase)\('
#                    '\s*(?P<erase_ctx>\w+)' '\s*,'
#                    '\s*(?P<erase_block>\w+)'
#                    '\s*\(\s*(?P<erase_size>\w+)\s*\)' '\s*\)'
#                '|' '(?P<sync>sync)\('
#                    '\s*(?P<sync_ctx>\w+)' '\s*\)'
#            ')\s*$')
#    def parse(line):
#        nonlocal bmap
#        nonlocal readed
#        nonlocal proged
#        nonlocal erased
#
#        # string searching is much faster than the regex here, and this
#        # actually has a big impact given the sheer quantity of how much
#        # trace output we have to deal with
#        if 'trace' not in line or 'bd' not in line:
#            return False
#        m = pattern.match(line)
#        if not m:
#            return False
#
#        if m.group('create'):
#            # update our block size/count
#            block_size_ = int(m.group('block_size'), 0)
#            block_count_ = int(m.group('block_count'), 0)
#
#            if reset:
#                bmap = Bmap(
#                        block_size=block_size_,
#                        block_count=block_count_,
#                        block_window=bmap.block_window,
#                        off_window=bmap.off_window,
#                        width=bmap.width,
#                        height=bmap.height)
#            elif ((block_size is None
#                        and block_size_ != bmap.block_size)
#                    or (block_count is None
#                        and block_count_ != bmap.block_count)):
#                bmap.resize(
#                        block_size=block_size if block_size is not None
#                            else block_size_,
#                        block_count=block_count if block_count is not None
#                            else block_count_)
#            return True
#
#        elif m.group('read') and reads:
#            block = int(m.group('read_block'), 0)
#            off = int(m.group('read_off'), 0)
#            size = int(m.group('read_size'), 0)
#
#            if ((block_size is None and off+size > bmap.block_size)
#                    or (block_count is None and block >= bmap.block_count)):
#                bmap.resize(
#                        block_size=block_size if block_size is not None
#                            else max(off+size, bmap.block_size),
#                        block_count=block_count if block_count is not None
#                            else max(block+1, bmap.block_count))
#
#            bmap.read(block, off, size)
#            readed += size
#            return True
#
#        elif m.group('prog') and progs:
#            block = int(m.group('prog_block'), 0)
#            off = int(m.group('prog_off'), 0)
#            size = int(m.group('prog_size'), 0)
#
#            if ((block_size is None and off+size > bmap.block_size)
#                    or (block_count is None and block >= bmap.block_count)):
#                bmap.resize(
#                        block_size=block_size if block_size is not None
#                            else max(off+size, bmap.block_size),
#                        block_count=block_count if block_count is not None
#                            else max(block+1, bmap.block_count))
#
#            bmap.prog(block, off, size)
#            proged += size
#            return True
#
#        elif m.group('erase') and (erases or wear):
#            block = int(m.group('erase_block'), 0)
#            size = int(m.group('erase_size'), 0)
#
#            if ((block_size is None and size > bmap.block_size)
#                    or (block_count is None and block >= bmap.block_count)):
#                bmap.resize(
#                        block_size=block_size if block_size is not None
#                            else max(size, bmap.block_size),
#                        block_count=block_count if block_count is not None
#                            else max(block+1, bmap.block_count))
#
#            bmap.erase(block, size)
#            erased += size
#            return True
#
#        else:
#            return False
#
#    # print trace output
#    def draw(f):
#        nonlocal readed
#        nonlocal proged
#        nonlocal erased
#
#        def writeln(s=''):
#            f.write(s)
#            f.write('\n')
#        f.writeln = writeln
#
#        # don't forget we've scaled this for braille/dots!
#        for row in range(
#                mt.ceil(bmap.height/4) if braille
#                    else mt.ceil(bmap.height/2) if dots
#                    else bmap.height):
#            line = bmap.draw(row,
#                    reads=reads,
#                    progs=progs,
#                    erases=erases,
#                    wear=wear,
#                    block_cycles=block_cycles,
#                    color=color,
#                    dots=dots,
#                    braille=braille,
#                    hilbert=hilbert,
#                    lebesgue=lebesgue,
#                    **args)
#            if line:
#                f.writeln(line)
#
#        # print some information about reads/progs/erases
#        #
#        # cat implies no-header, because a header wouldn't really make sense
#        if not no_header and not cat:
#            # compute total ops
#            total = readed+proged+erased
#
#            # compute stddev of wear using our bmap, this is a bit different
#            # from reads/progs/erases which ignores any bmap window, but it's
#            # what we have
#            if wear:
#                mean = (sum(p.wear for p in bmap.pixels)
#                        / max(len(bmap.pixels), 1))
#                stddev = mt.sqrt(sum((p.wear - mean)**2 for p in bmap.pixels)
#                        / max(len(bmap.pixels), 1))
#                worst = max((p.wear for p in bmap.pixels), default=0)
#
#            # a bit of a hack here, but this forces our header to always be
#            # at row zero
#            if len(f.lines) == 0:
#                f.lines.append('')
#            f.lines[0] = 'bd %dx%d%s%s%s%s' % (
#                    bmap.block_size, bmap.block_count,
#                    ', %6s read' % ('%.1f%%' % (100*readed  / max(total, 1)))
#                        if reads else '',
#                    ', %6s prog' % ('%.1f%%' % (100*proged / max(total, 1)))
#                        if progs else '',
#                    ', %6s erase' % ('%.1f%%' % (100*erased / max(total, 1)))
#                        if erases else '',
#                    ', %13s wear' % ('%.1fσ (%.1f%%)' % (
#                            worst / max(stddev, 1),
#                            100*stddev / max(worst, 1)))
#                        if wear else '')
#
#        bmap.clear()
#        readed = 0
#        proged = 0
#        erased = 0
#        resize()
#
#
#    # read/parse/coalesce operations
#    if cat:
#        ring = sys.stdout
#    else:
#        ring = RingIO(lines + (1 if not no_header else 0), head)
#
#    # if sleep print in background thread to avoid getting stuck in a read call
#    event = th.Event()
#    lock = th.Lock()
#    if sleep:
#        done = False
#        def background():
#            while not done:
#                event.wait()
#                event.clear()
#                with lock:
#                    draw(ring)
#                    if not cat:
#                        ring.draw()
#                # sleep a minimum amount of time to avoid flickering
#                time.sleep(sleep or 0.01)
#        th.Thread(target=background, daemon=True).start()
#
#    try:
#        while True:
#            with openio(path) as f:
#                changed = 0
#                for line in f:
#                    with lock:
#                        changed += parse(line)
#
#                        # need to redraw?
#                        if changed and (not coalesce or changed >= coalesce):
#                            if sleep:
#                                event.set()
#                            else:
#                                draw(ring)
#                                if not cat:
#                                    ring.draw()
#                            changed = 0
#
#            if not keep_open:
#                break
#            # don't just flood open calls
#            time.sleep(sleep or 2)
#    except FileNotFoundError as e:
#        print("error: file not found %r" % path,
#                file=sys.stderr)
#        sys.exit(-1)
#    except KeyboardInterrupt:
#        pass
#
#    if sleep:
#        done = True
#        lock.acquire() # avoids https://bugs.python.org/issue42717
#    if not cat:
#        sys.stdout.write('\n')
#
#
#
#
#
#
#
#    def trace_(line):
#        # TODO
#
#    # TODO adopt f -> ring name in all scripts?
#    def draw_(ring, *,
#            width=None,
#            height=None):
#        # TODO





    ## main loop
    lock = th.Lock()
    event = th.Event()
    def main_():
        try:
            while True:
                with openio(path) as f:
                    count = 0
                    for line in f:
                        with lock:
                            count += trace__(line)

                            # always redraw if we're sleeping, otherwise
                            # wait for coalesce number of operations
                            if sleep is not None or count >= (coalesce or 1):
                                event.set()
                                count = 0

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

    # keep track of history if lines specified
    if lines is not None:
        ring = RingIO(lines+1
                # TODO expose no_header in other scripts for consistency
                if not no_header and lines > 0
                else lines)
    def draw_():
        # cat? write directly to stdout
        if cat:
            draw__(sys.stdout,
                    # TODO copy this pattern (width+height) for
                    # other scripts?
                    width=width,
                    # make space for shell prompt
                    height=height if height is not False else -1)
        # not cat? write to a bounded ring
        else:
            ring_ = RingIO(head=head)
            draw__(ring_,
                    width=width,
                    height=height if height is not False else 0)
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

    # print in a background thread
    done = False
    def background():
        while not done:
            event.wait()
            event.clear()
            with lock:
                draw_()
            # sleep a minimum amount of time to avoid flickering
            time.sleep(sleep or 0.01)
    th.Thread(target=background, daemon=True).start()

    main_()

    done = True
    lock.acquire() # avoids https://bugs.python.org/issue42717
    if not cat:
        # give ourselves one last draw, helps if background is
        # never triggered
        draw_()
        sys.stdout.write('\n')






#    # TODO WIP
#
#    # cat? let main_ write directly to stdout
#    else:
#        main_(sys.stdout)
#
#    # not cat? write to a bounded ring
#    if not cat:
#        ring = RingIO(lines, head=head)
#        main_(ring)
#        sys.stdout.write('\n')




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
            '--reads',
            action='store_true',
            help="Render reads.")
    parser.add_argument(
            '--progs',
            action='store_true',
            help="Render progs.")
    parser.add_argument(
            '--erases',
            action='store_true',
            help="Render erases.")
    parser.add_argument(
            '--wear',
            action='store_true',
            help="Render wear.")
    parser.add_argument(
            '--wear-only',
            action='store_true',
            help="Only render wear, don't render bd ops. Implies --wear.")
    parser.add_argument(
            '-w', '--block-cycles',
            type=lambda x: int(x, 0),
            help="Assumed maximum number of erase cycles when measuring "
                "wear. Implies --wear.")
    parser.add_argument(
            '--volatile',
            action='store_true',
            help="Reset wear on block device initialization.")
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
                "operation/block. Accepts %% modifiers.")
    parser.add_argument(
            '-,', '--add-wear-char', '--wear-chars',
            dest='wear_chars',
            action='append',
            type=lambda x: (
                lambda ks, v: (
                    tuple(k.strip() for k in ks.split(',')),
                    v.strip())
                )(*x.split('=', 1))
                    if '=' in x else x.strip(),
            help="Add wear characters to use. Can be assigned to a specific "
                "operation/block. Accepts %% modifiers.")
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
            '-G', '--add-wear-color',
            dest='wear_colors',
            action='append',
            type=lambda x: (
                lambda ks, v: (
                    tuple(k.strip() for k in ks.split(',')),
                    v.strip())
                )(*x.split('=', 1))
                    if '=' in x else x.strip(),
            help="Add a wear color to use. Can be assigned to a specific "
                "operation/block. Accepts %% modifiers.")
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
            const=False,
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
            help="Scale the resulting map such that 1 pixel ~= 1/scale "
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
                "and --no-header.")
    parser.add_argument(
            '--title',
            help="Add a title. Accepts %% modifiers.")
    # TODO drop padding in ascii scripts, no one is ever going to use this
    parser.add_argument(
            '--padding',
            type=float,
            help="Padding to add to each block. Defaults to 0.")
    # TODO lines in dbgbmap.py?
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
            '-S', '--coalesce',
            type=lambda x: int(x, 0),
            help="Number of operations to coalesce together.")
    parser.add_argument(
            '-s', '--sleep',
            type=float,
            help="Seconds to sleep between draws, coalescing operations "
                "in between.")
    parser.add_argument(
            '-k', '--keep-open',
            action='store_true',
            help="Reopen the pipe on EOF, useful when multiple "
                "processes are writing.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
