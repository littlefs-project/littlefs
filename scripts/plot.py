#!/usr/bin/env python3
#
# Plot CSV files in terminal.
#
# Example:
# ./scripts/plot.py bench.csv -xSIZE -ybench_read -W80 -H17
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import bisect
import collections as co
import csv
import fnmatch
import io
import itertools as it
import math as mt
import os
import re
import shlex
import shutil
import sys
import time

try:
    import inotify_simple
except ModuleNotFoundError:
    inotify_simple = None


COLORS = [
    '1;34', # bold blue
    '1;31', # bold red
    '1;32', # bold green
    '1;35', # bold purple
    '1;33', # bold yellow
    '1;36', # bold cyan
    '34',   # blue
    '31',   # red
    '32',   # green
    '35',   # purple
    '33',   # yellow
    '36',   # cyan
]

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
CHARS_POINTS_AND_LINES = 'o'

SI_PREFIXES = {
    18:  'E',
    15:  'P',
    12:  'T',
    9:   'G',
    6:   'M',
    3:   'K',
    0:   '',
    -3:  'm',
    -6:  'u',
    -9:  'n',
    -12: 'p',
    -15: 'f',
    -18: 'a',
}

SI2_PREFIXES = {
    60:  'Ei',
    50:  'Pi',
    40:  'Ti',
    30:  'Gi',
    20:  'Mi',
    10:  'Ki',
    0:   '',
    -10: 'mi',
    -20: 'ui',
    -30: 'ni',
    -40: 'pi',
    -50: 'fi',
    -60: 'ai',
}


# format a number to a strict character width using SI prefixes
def si(x, w=4):
    if x == 0:
        return '0'
    # figure out prefix and scale
    #
    # note we adjust this so that 100K = .1M, which has more info
    # per character
    p = 3*int(mt.log(abs(x)*10, 10**3))
    p = min(18, max(-18, p))
    # format with enough digits
    s = '%.*f' % (w, abs(x) / (10.0**p))
    s = s.lstrip('0')
    # truncate but only digits that follow the dot
    if '.' in s:
        s = s[:max(s.find('.'), w-(2 if x < 0 else 1))]
        s = s.rstrip('0')
        s = s.rstrip('.')
    return '%s%s%s' % ('-' if x < 0 else '', s, SI_PREFIXES[p])

def si2(x, w=5):
    if x == 0:
        return '0'
    # figure out prefix and scale
    #
    # note we adjust this so that 128Ki = .1Mi, which has more info
    # per character
    p = 10*int(mt.log(abs(x)*10, 2**10))
    p = min(30, max(-30, p))
    # format with enough digits
    s = '%.*f' % (w, abs(x) / (2.0**p))
    s = s.lstrip('0')
    # truncate but only digits that follow the dot
    if '.' in s:
        s = s[:max(s.find('.'), w-(3 if x < 0 else 2))]
        s = s.rstrip('0')
        s = s.rstrip('.')
    return '%s%s%s' % ('-' if x < 0 else '', s, SI2_PREFIXES[p])

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


# parse different data representations
def dat(x, *args):
    try:
        # allow the first part of an a/b fraction
        if '/' in x:
            x, _ = x.split('/', 1)

        # first try as int
        try:
            return int(x, 0)
        except ValueError:
            pass

        # then try as float
        try:
            return float(x)
        except ValueError:
            pass

        # else give up
        raise ValueError("invalid dat %r" % x)

    # default on error?
    except ValueError as e:
        if args:
            return args[0]
        else:
            raise

def collect(csv_paths, defines=[]):
    # collect results from CSV files
    fields = []
    results = []
    for path in csv_paths:
        try:
            with openio(path) as f:
                reader = csv.DictReader(f, restval='')
                fields.extend(
                        k for k in reader.fieldnames or []
                            if k not in fields)
                for r in reader:
                    # filter by matching defines
                    if not all(k in r and r[k] in vs for k, vs in defines):
                        continue

                    results.append(r)
        except FileNotFoundError:
            pass

    return fields, results

def fold(results, by=None, x=None, y=None, defines=[]):
    # filter by matching defines
    if defines:
        results_ = []
        for r in results:
            if all(k in r and r[k] in vs for k, vs in defines):
                results_.append(r)
        results = results_

    if by:
        # find all 'by' values
        keys = set()
        for r in results:
            keys.add(tuple(r.get(k, '') for k in by))
        keys = sorted(keys)

    # collect all datasets
    datasets = co.OrderedDict()
    dataattrs = co.OrderedDict()
    for key in (keys if by else [()]):
        for x_ in (x if x else [None]):
            for y_ in y:
                # organize by 'by', x, and y
                dataset = []
                dataattr = {}
                i = 0
                for r in results:
                    # filter by 'by'
                    if by and not all(
                            k in r and r[k] == v
                                for k, v in zip(by, key)):
                        continue

                    # find xs
                    if x_ is not None:
                        if x_ not in r:
                            continue
                        try:
                            x__ = dat(r[x_])
                        except ValueError:
                            continue
                    else:
                        # fallback to enumeration
                        x__ = i
                        i += 1

                    # find ys
                    if y_ is not None:
                        if y_ not in r:
                            continue
                        try:
                            y__ = dat(r[y_])
                        except ValueError:
                            continue
                    else:
                        y__ = None

                    # do _not_ sum ys here, it's tempting but risks
                    # incorrect and misleading results
                    dataset.append((x__, y__))

                    # include all fields in dataattrs in case we use
                    # them for % modifiers
                    dataattr.update(r)

                # hide x/y if there is only one field
                key_ = key
                if len(x or []) > 1:
                    key_ += (x_,)
                if len(y or []) > 1 or not key_:
                    key_ += (y_,)
                datasets[key_] = dataset
                dataattrs[key_] = dataattr

    return datasets, dataattrs

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


# a hack log that preserves sign, with a linear region between -1 and 1
def symlog(x):
    if x > 1:
        return mt.log(x)+1
    elif x < -1:
        return -mt.log(-x)-1
    else:
        return x

# our main plot class
class Plot:
    def __init__(self, width, height, *,
            color=False,
            dots=False,
            braille=False,
            xlim=None,
            ylim=None,
            xlog=False,
            ylog=False):
        # let Canvas handle braille/dots scaling
        self.canvas = Canvas(width, height,
                color=color,
                dots=dots,
                braille=braille)

        # we handle xlim/ylim scaling
        self.xlim = xlim or (0, width)
        self.ylim = ylim or (0, height)
        self.xlog = xlog
        self.ylog = ylog

        # go ahead and draw out axis first, we let data overwrite this
        # to make the best of the limited space
        for x in range(self.width):
            self.canvas.point(x, 0, char='-')
        for y in range(self.height):
            self.canvas.point(0, y, char='|')
        self.canvas.point(self.width-1, 0, char='>')
        self.canvas.point(0, self.height-1, char='^')
        self.canvas.point(0, 0, char='+')

    @property
    def width(self):
        return self.canvas.width

    @property
    def height(self):
        return self.canvas.height

    def _scale(self, x, y):
        # scale and clamp
        try:
            if self.xlog:
                x = int(self.width * (
                        (symlog(x)-symlog(self.xlim[0]))
                            / (symlog(self.xlim[1])-symlog(self.xlim[0]))))
            else:
                x = int(self.width * (
                        (x-self.xlim[0])
                            / (self.xlim[1]-self.xlim[0])))
            if self.ylog:
                y = int(self.height * (
                        (symlog(y)-symlog(self.ylim[0]))
                            / (symlog(self.ylim[1])-symlog(self.ylim[0]))))
            else:
                y = int(self.height * (
                        (y-self.ylim[0])
                            / (self.ylim[1]-self.ylim[0])))
        except ZeroDivisionError:
            x = 0
            y = 0
        return x, y

    def point(self, x, y, *,
            char=True,
            color=''):
        # scale
        x, y = self._scale(x, y)

        # render to canvas
        self.canvas.point(x, y,
                char=char,
                color=color)

    def line(self, x1, y1, x2, y2, *,
            char=True,
            color=''):
        # scale
        x1, y1 = self._scale(x1, y1)
        x2, y2 = self._scale(x2, y2)

        # render to canvas
        self.canvas.line(x1, y1, x2, y2,
                char=char,
                color=color)

    def plot(self, coords, *,
            char=True,
            line_char=True,
            color=''):
        # draw lines
        if line_char:
            for (x1, y1), (x2, y2) in zip(coords, coords[1:]):
                if y1 is not None and y2 is not None:
                    self.line(x1, y1, x2, y2,
                            char=line_char,
                            color=color)

        # draw points
        if char and (not line_char or char is not True):
            for x, y in coords:
                if y is not None:
                    self.point(x, y,
                            char=char,
                            color=color)

    def draw(self, row):
        return self.canvas.draw(row)


# some classes for organizing subplots into a grid
class Subplot:
    def __init__(self, **args):
        self.x = 0
        self.y = 0
        self.xspan = 1
        self.yspan = 1
        self.args = args

class Grid:
    def __init__(self, subplot, width=1.0, height=1.0):
        self.xweights = [width]
        self.yweights = [height]
        self.map = {(0,0): subplot}
        self.subplots = [subplot]

    def __repr__(self):
        return 'Grid(%r, %r)' % (self.xweights, self.yweights)

    @property
    def width(self):
        return len(self.xweights)

    @property
    def height(self):
        return len(self.yweights)

    def __iter__(self):
        return iter(self.subplots)

    def __getitem__(self, i):
        x, y = i
        if x < 0:
            x += len(self.xweights)
        if y < 0:
            y += len(self.yweights)

        return self.map[(x,y)]

    def merge(self, other, dir):
        if dir in ['above', 'below']:
            # first scale the two grids so they line up
            self_xweights = self.xweights
            other_xweights = other.xweights
            self_w = sum(self_xweights)
            other_w = sum(other_xweights)
            ratio = self_w / other_w
            other_xweights = [s*ratio for s in other_xweights]

            # now interleave xweights as needed
            new_xweights = []
            self_map = {}
            other_map = {}
            self_i = 0
            other_i = 0
            self_xweight = (self_xweights[self_i]
                    if self_i < len(self_xweights) else mt.inf)
            other_xweight = (other_xweights[other_i]
                    if other_i < len(other_xweights) else mt.inf)
            while self_i < len(self_xweights) and other_i < len(other_xweights):
                if other_xweight - self_xweight > 0.0000001:
                    new_xweights.append(self_xweight)
                    other_xweight -= self_xweight

                    new_i = len(new_xweights)-1
                    for j in range(len(self.yweights)):
                        self_map[(new_i, j)] = self.map[(self_i, j)]
                    for j in range(len(other.yweights)):
                        other_map[(new_i, j)] = other.map[(other_i, j)]
                    for s in other.subplots:
                        if s.x+s.xspan-1 == new_i:
                            s.xspan += 1
                        elif s.x > new_i:
                            s.x += 1

                    self_i += 1
                    self_xweight = (self_xweights[self_i]
                            if self_i < len(self_xweights) else mt.inf)
                elif self_xweight - other_xweight > 0.0000001:
                    new_xweights.append(other_xweight)
                    self_xweight -= other_xweight

                    new_i = len(new_xweights)-1
                    for j in range(len(other.yweights)):
                        other_map[(new_i, j)] = other.map[(other_i, j)]
                    for j in range(len(self.yweights)):
                        self_map[(new_i, j)] = self.map[(self_i, j)]
                    for s in self.subplots:
                        if s.x+s.xspan-1 == new_i:
                            s.xspan += 1
                        elif s.x > new_i:
                            s.x += 1

                    other_i += 1
                    other_xweight = (other_xweights[other_i]
                            if other_i < len(other_xweights) else mt.inf)
                else:
                    new_xweights.append(self_xweight)

                    new_i = len(new_xweights)-1
                    for j in range(len(self.yweights)):
                        self_map[(new_i, j)] = self.map[(self_i, j)]
                    for j in range(len(other.yweights)):
                        other_map[(new_i, j)] = other.map[(other_i, j)]

                    self_i += 1
                    self_xweight = (self_xweights[self_i]
                            if self_i < len(self_xweights) else mt.inf)
                    other_i += 1
                    other_xweight = (other_xweights[other_i]
                            if other_i < len(other_xweights) else mt.inf)

            # squish so ratios are preserved
            self_h = sum(self.yweights)
            other_h = sum(other.yweights)
            ratio = (self_h-other_h) / self_h
            self_yweights = [s*ratio for s in self.yweights]

            # finally concatenate the two grids
            if dir == 'above':
                for s in other.subplots:
                    s.y += len(self_yweights)
                self.subplots.extend(other.subplots)

                self.xweights = new_xweights
                self.yweights = self_yweights + other.yweights
                self.map = self_map | {
                        (x, y+len(self_yweights)): s
                            for (x, y), s in other_map.items()}
            else:
                for s in self.subplots:
                    s.y += len(other.yweights)
                self.subplots.extend(other.subplots)

                self.xweights = new_xweights
                self.yweights = other.yweights + self_yweights
                self.map = other_map | {
                        (x, y+len(other.yweights)): s
                            for (x, y), s in self_map.items()}

        if dir in ['right', 'left']:
            # first scale the two grids so they line up
            self_yweights = self.yweights
            other_yweights = other.yweights
            self_h = sum(self_yweights)
            other_h = sum(other_yweights)
            ratio = self_h / other_h
            other_yweights = [s*ratio for s in other_yweights]

            # now interleave yweights as needed
            new_yweights = []
            self_map = {}
            other_map = {}
            self_i = 0
            other_i = 0
            self_yweight = (self_yweights[self_i]
                    if self_i < len(self_yweights) else mt.inf)
            other_yweight = (other_yweights[other_i]
                    if other_i < len(other_yweights) else mt.inf)
            while self_i < len(self_yweights) and other_i < len(other_yweights):
                if other_yweight - self_yweight > 0.0000001:
                    new_yweights.append(self_yweight)
                    other_yweight -= self_yweight

                    new_i = len(new_yweights)-1
                    for j in range(len(self.xweights)):
                        self_map[(j, new_i)] = self.map[(j, self_i)]
                    for j in range(len(other.xweights)):
                        other_map[(j, new_i)] = other.map[(j, other_i)]
                    for s in other.subplots:
                        if s.y+s.yspan-1 == new_i:
                            s.yspan += 1
                        elif s.y > new_i:
                            s.y += 1

                    self_i += 1
                    self_yweight = (self_yweights[self_i]
                            if self_i < len(self_yweights) else mt.inf)
                elif self_yweight - other_yweight > 0.0000001:
                    new_yweights.append(other_yweight)
                    self_yweight -= other_yweight

                    new_i = len(new_yweights)-1
                    for j in range(len(other.xweights)):
                        other_map[(j, new_i)] = other.map[(j, other_i)]
                    for j in range(len(self.xweights)):
                        self_map[(j, new_i)] = self.map[(j, self_i)]
                    for s in self.subplots:
                        if s.y+s.yspan-1 == new_i:
                            s.yspan += 1
                        elif s.y > new_i:
                            s.y += 1

                    other_i += 1
                    other_yweight = (other_yweights[other_i]
                            if other_i < len(other_yweights) else mt.inf)
                else:
                    new_yweights.append(self_yweight)

                    new_i = len(new_yweights)-1
                    for j in range(len(self.xweights)):
                        self_map[(j, new_i)] = self.map[(j, self_i)]
                    for j in range(len(other.xweights)):
                        other_map[(j, new_i)] = other.map[(j, other_i)]

                    self_i += 1
                    self_yweight = (self_yweights[self_i]
                            if self_i < len(self_yweights) else mt.inf)
                    other_i += 1
                    other_yweight = (other_yweights[other_i]
                            if other_i < len(other_yweights) else mt.inf)

            # squish so ratios are preserved
            self_w = sum(self.xweights)
            other_w = sum(other.xweights)
            ratio = (self_w-other_w) / self_w
            self_xweights = [s*ratio for s in self.xweights]

            # finally concatenate the two grids
            if dir == 'right':
                for s in other.subplots:
                    s.x += len(self_xweights)
                self.subplots.extend(other.subplots)

                self.xweights = self_xweights + other.xweights
                self.yweights = new_yweights
                self.map = self_map | {
                        (x+len(self_xweights), y): s
                            for (x, y), s in other_map.items()}
            else:
                for s in self.subplots:
                    s.x += len(other.xweights)
                self.subplots.extend(other.subplots)

                self.xweights = other.xweights + self_xweights
                self.yweights = new_yweights
                self.map = other_map | {
                        (x+len(other.xweights), y): s
                            for (x, y), s in self_map.items()}


    def scale(self, width, height):
        self.xweights = [s*width for s in self.xweights]
        self.yweights = [s*height for s in self.yweights]

    @classmethod
    def fromargs(cls, width=1.0, height=1.0, *,
            subplots=[],
            **args):
        grid = cls(Subplot(**args))

        for dir, subargs in subplots:
            subgrid = cls.fromargs(
                    width=subargs.pop('width',
                        0.5 if dir in ['right', 'left'] else width),
                    height=subargs.pop('height',
                        0.5 if dir in ['above', 'below'] else height),
                    **subargs)
            grid.merge(subgrid, dir)

        grid.scale(width, height)
        return grid


def main_(f, csv_paths, *,
        by=None,
        x=None,
        y=None,
        define=[],
        labels=[],
        chars=[],
        line_chars=[],
        colors=[],
        color=False,
        dots=False,
        braille=False,
        points=False,
        points_and_lines=False,
        width=None,
        height=None,
        xlim=(None,None),
        ylim=(None,None),
        xlog=False,
        ylog=False,
        x2=False,
        y2=False,
        xunits='',
        yunits='',
        xlabel=None,
        ylabel=None,
        xticklabels=None,
        yticklabels=None,
        title=None,
        legend_right=False,
        legend_above=False,
        legend_below=False,
        subplot={},
        subplots=[],
        **args):
    # give f an writeln function
    def writeln(s=''):
        f.write(s)
        f.write('\n')
    f.writeln = writeln

    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # what chars/colors to use?
    chars_ = []
    for char in chars:
        if isinstance(char, tuple):
            chars_.extend((char[0], c) for c in psplit(char[1]))
        else:
            chars_.extend(psplit(char))
    chars_ = Attr(chars_, defaults=(
            CHARS_POINTS_AND_LINES if points_and_lines
                else [True]))

    line_chars_ = []
    for line_char in line_chars:
        if isinstance(line_char, tuple):
            line_chars_.extend((line_char[0], c) for c in psplit(line_char[1]))
        else:
            line_chars_.extend(psplit(line_char))
    line_chars_ = Attr(line_chars_, defaults=(
            [True] if points_and_lines or not points
                else [False]))

    colors_ = Attr(colors, defaults=COLORS)

    labels_ = Attr(labels)

    # split %n newlines early
    title = (title.replace('%n', '\n').split('\n')
            if title is not None else [])
    xlabel = (xlabel.replace('%n', '\n').split('\n')
            if xlabel is not None else [])
    ylabel = (ylabel.replace('%n', '\n').split('\n')
            if ylabel is not None else [])

    # subplot can also contribute to subplots, resolve this here or things
    # become a mess...
    subplots += subplot.pop('subplots', [])

    # allow any subplots to contribute to by/x/y
    def subplots_get(k, *, subplots=[], **args):
        v = args.get(k, []).copy()
        for _, subargs in subplots:
            v.extend(subplots_get(k, **subargs))
        return v

    all_by = (by or []) + subplots_get('by', **subplot, subplots=subplots)
    all_x = (x or []) + subplots_get('x', **subplot, subplots=subplots)
    all_y = (y or []) + subplots_get('y', **subplot, subplots=subplots)
    all_defines = co.defaultdict(lambda: set())
    for k, vs in it.chain(define or [],
            subplots_get('define', **subplot, subplots=subplots)):
        all_defines[k] |= vs
    all_defines = sorted(all_defines.items())

    if not all_by and not all_y:
        print("error: needs --by or -y to figure out fields",
                file=sys.stderr)
        sys.exit(-1)

    # create a grid of subplots
    grid = Grid.fromargs(**subplot, subplots=subplots)

    for s in grid:
        # allow subplot params to override global params
        x2_ = s.args.get('x2', False) or x2
        y2_ = s.args.get('y2', False) or y2
        xunits_ = s.args.get('xunits', xunits)
        yunits_ = s.args.get('yunits', yunits)
        xticklabels_ = s.args.get('xticklabels', xticklabels)
        yticklabels_ = s.args.get('yticklabels', yticklabels)

        # label/titles are handled a bit differently in subplots
        subtitle = s.args.get('title')
        xsublabel = s.args.get('xlabel')
        ysublabel = s.args.get('ylabel')

        # split %n newlines early
        subtitle = (subtitle.replace('%n', '\n').split('\n')
                if subtitle is not None else [])
        xsublabel = (xsublabel.replace('%n', '\n').split('\n')
                if xsublabel is not None else [])
        ysublabel = (ysublabel.replace('%n', '\n').split('\n')
                if ysublabel is not None else [])

        # don't allow >2 ticklabels and render single ticklabels only once
        if xticklabels_ is not None:
            if len(xticklabels_) == 1:
                xticklabels_ = ["", xticklabels_[0]]
            elif len(xticklabels_) > 2:
                xticklabels_ = [xticklabels_[0], xticklabels_[-1]]
        if yticklabels_ is not None:
            if len(yticklabels_) == 1:
                yticklabels_ = ["", yticklabels_[0]]
            elif len(yticklabels_) > 2:
                yticklabels_ = [yticklabels_[0], yticklabels_[-1]]

        s.x2 = x2_
        s.y2 = y2_
        s.xunits = xunits_
        s.yunits = yunits_
        s.xticklabels = xticklabels_
        s.yticklabels = yticklabels_
        s.title = subtitle
        s.xlabel = xsublabel
        s.ylabel = ysublabel

    # preprocess margins so they can be shared
    for s in grid:
        s.xmargin = (
            len(s.ylabel) + (1 if s.ylabel else 0) # fit ysublabel
                + (1 if s.x > 0 else 0),           # space between
            ((5 if s.y2 else 4) + len(s.yunits)    # fit yticklabels
                    if s.yticklabels is None
                    else max(
                        # bit of a hack, we just guess the yticklabel size
                        # since we don't have the data yet
                        (len(punescape(l, {'y': 0})) for l in s.yticklabels),
                        default=0))
                + (1 if s.yticklabels != [] else 0),
        )
        s.ymargin = (
            len(s.xlabel),                   # fit xsublabel
            1 if s.xticklabels != [] else 0, # fit xticklabels
            len(s.title),                    # fit subtitle
        )

    for s in grid:
        # share margins so everything aligns nicely
        s.xmargin = (
            max(s_.xmargin[0] for s_ in grid if s_.x == s.x),
            max(s_.xmargin[1] for s_ in grid if s_.x == s.x),
        )
        s.ymargin = (
            max(s_.ymargin[0] for s_ in grid if s_.y == s.y),
            max(s_.ymargin[1] for s_ in grid if s_.y == s.y),
            max(s_.ymargin[-1] for s_ in grid if s_.y+s_.yspan == s.y+s.yspan),
        )


    ## our main drawing logic

    # first collect results from CSV files
    fields_, results = collect(csv_paths)

    # if y not specified, guess it's anything not in by/defines/x
    all_y_ = all_y
    if not all_y:
        all_y_ = [k for k in fields_
                if k not in all_by
                    and not any(k == k_ for k_, _ in all_defines)]

    # then extract the requested datasets
    #
    # note we don't need to filter by defines again
    datasets_, dataattrs_ = fold(results, all_by, all_x, all_y)

    # order by labels
    datasets_ = co.OrderedDict(sorted(
            datasets_.items(),
            key=lambda kv: labels_.key(kv[0])))

    # and merge dataattrs
    mergedattrs_ = {k: v
            for dataattr in dataattrs_.values()
            for k, v in dataattr.items()}

    # figure out labels/titles now that we have our data
    title_ = [punescape(l, mergedattrs_) for l in title]
    xlabel_ = [punescape(l, mergedattrs_) for l in xlabel]
    ylabel_ = [punescape(l, mergedattrs_) for l in ylabel]

    # figure out colors/chars here so that subplot defines
    # don't change them later, that'd be bad
    datachars_ = {name: (lambda c:
                c if isinstance(c, bool)
                        # limit to 1 char
                        else punescape(c, dataattrs_[name])[0])(
                    chars_[i, name])
            for i, name in enumerate(datasets_.keys())}
    dataline_chars_ = {name: (lambda c:
                c if isinstance(c, bool)
                        # limit to 1 char
                        else punescape(c, dataattrs_[name])[0])(
                    line_chars_[i, name])
            for i, name in enumerate(datasets_.keys())}
    datacolors_ = {name: punescape(colors_[i, name], dataattrs_[name])
            for i, name in enumerate(datasets_.keys())}
    datalabels_ = {name: punescape(labels_[i, name], dataattrs_[name])
            for i, name in enumerate(datasets_.keys())
            if (i, name) in labels_}

    # build legend?
    legend_width = 0
    if legend_right or legend_above or legend_below:
        legend_ = []
        for i, name in enumerate(datasets_.keys()):
            if name in datalabels_ and not datalabels_[name]:
                continue
            label = '%s%s' % (
                    '. ' if chars
                            and isinstance(datachars_[name], bool)
                        else '%s ' % datachars_[name]
                        if chars
                        else '. '
                        if line_chars
                            and isinstance(dataline_chars_[name], bool)
                        else '%s ' % dataline_chars_[name]
                        if line_chars
                        else '',
                    datalabels_[name]
                        if name in datalabels_
                        else ','.join(name))

            if label:
                legend_.append((label, datacolors_[name]))
                legend_width = max(legend_width, len(label)+1)

    # figure out our canvas size
    if width is None:
        width_ = min(80, shutil.get_terminal_size((80, 5))[0])
    elif width > 0:
        width_ = width
    else:
        width_ = max(0, shutil.get_terminal_size((80, 5))[0] + width)

    if height is None:
        height_ = 17 + len(title_) + len(xlabel_)
    elif height > 0:
        height_ = height
    else:
        height_ = max(0, shutil.get_terminal_size((80, 5))[1] + height)

    # carve out space for the xlabel
    height_ -= len(xlabel_)
    # carve out space for the ylabel
    width_ -= len(ylabel_) + (1 if ylabel_ else 0)
    # carve out space for title
    height_ -= len(title_)

    # carve out space for the legend
    if legend_right and legend_:
        width_ -= legend_width
    if legend_above and legend_:
        legend_cols = len(legend_)
        while True:
            legend_widths = [
                    max(len(l) for l, _ in legend_[i::legend_cols])
                        for i in range(legend_cols)]
            if (legend_cols <= 1
                    or sum(legend_widths)+2*(legend_cols-1)
                            + max(sum(s.xmargin[:2])
                                for s in grid
                                if s.x == 0)
                        <= width_):
                break
            legend_cols -= 1
        height_ -= (len(legend_)+legend_cols-1) // legend_cols
    if legend_below and legend_:
        legend_cols = len(legend_)
        while True:
            legend_widths = [
                    max(len(l) for l, _ in legend_[i::legend_cols])
                        for i in range(legend_cols)]
            if (legend_cols <= 1
                    or sum(legend_widths)+2*(legend_cols-1)
                            + max(sum(s.xmargin[:2])
                                for s in grid
                                if s.x == 0)
                        <= width_):
                break
            legend_cols -= 1
        height_ -= (len(legend_)+legend_cols-1) // legend_cols

    # figure out the grid dimensions
    #
    # note we floor to give the dimension tweaks the best chance of not
    # exceeding the requested dimensions, this means we usually are less
    # than the requested dimensions by quite a bit when we have many
    # subplots, but it's a tradeoff for a relatively simple implementation
    widths = [mt.floor(w*width_) for w in grid.xweights]
    heights = [mt.floor(w*height_) for w in grid.yweights]

    # tweak dimensions to allow all plots to have a minimum width,
    # this may force the plot to be larger than the requested dimensions,
    # but that's the best we can do
    for s in grid:
        # fit xunits
        minwidth = sum(s.xmargin) + max(
                2,
                2*((5 if s.x2 else 4)+len(s.xunits))
                    if s.xticklabels is None
                    # bit of a hack, we just guess the xticklabel size
                    # since we don't have the data yet
                    else sum(len(punescape(l, {'x': 0}))
                        for l in s.xticklabels))
        # fit yunits
        minheight = sum(s.ymargin) + 2

        i = 0
        while minwidth > sum(widths[s.x:s.x+s.xspan]):
            widths[s.x+i] += 1
            i = (i + 1) % s.xspan

        i = 0
        while minheight > sum(heights[s.y:s.y+s.yspan]):
            heights[s.y+i] += 1
            i = (i + 1) % s.yspan

    width_ = sum(widths)
    height_ = sum(heights)

    # create a plot for each subplot
    for s in grid:
        # allow subplot params to override global params
        x_ = set((x or []) + s.args.get('x', []))
        y_ = set((y or []) + s.args.get('y', []))
        define_ = define + s.args.get('define', [])
        xlim_ = s.args.get('xlim', xlim)
        ylim_ = s.args.get('ylim', ylim)
        xlog_ = s.args.get('xlog', False) or xlog
        ylog_ = s.args.get('ylog', False) or ylog

        # allow shortened ranges
        if len(xlim_) == 1:
            xlim_ = (0, xlim_[0])
        if len(ylim_) == 1:
            ylim_ = (0, ylim_[0])

        # data can be constrained by subplot-specific defines,
        # so re-extract for each plot
        subdatasets, subdataattrs = fold(
                results, all_by, all_x, all_y_, define_)

        # order by labels
        subdatasets = co.OrderedDict(sorted(
                subdatasets.items(),
                key=lambda kv: labels_.key(kv[0])))

        # filter by subplot x/y
        subdatasets = co.OrderedDict([(name, dataset)
                for name, dataset in subdatasets.items()
                if len(all_x) <= 1
                    or name[-(1 if len(all_y_) <= 1 else 2)] in x_
                if len(all_y_) <= 1
                    or name[-1] in y_])
        subdataattrs = co.OrderedDict([(name, dataattr)
                for name, dataattr in subdataattrs.items()
                if len(all_x) <= 1
                    or name[-(1 if len(all_y) <= 1 else 2)] in x_
                if len(all_y) <= 1
                    or name[-1] in y_])
        # and merge dataattrs
        submergedattrs = {k: v
                for dataattr in subdataattrs.values()
                for k, v in dataattr.items()}

        # find actual xlim/ylim
        xlim_ = (
                xlim_[0] if xlim_[0] is not None
                    else min(it.chain([0], (x
                        for dataset in subdatasets.values()
                        for x, y in dataset
                        if y is not None))),
                xlim_[1] if xlim_[1] is not None
                    else max(it.chain([0], (x
                        for dataset in subdatasets.values()
                        for x, y in dataset
                        if y is not None))))

        ylim_ = (
                ylim_[0] if ylim_[0] is not None
                    else min(it.chain([0], (y
                        for dataset in subdatasets.values()
                        for _, y in dataset
                        if y is not None))),
                ylim_[1] if ylim_[1] is not None
                    else max(it.chain([0], (y
                        for dataset in subdatasets.values()
                        for _, y in dataset
                        if y is not None))))

        # figure out labels/titles now that we have our data
        subtitle = [punescape(l, submergedattrs) for l in s.title]
        subxlabel = [punescape(l, submergedattrs) for l in s.xlabel]
        subylabel = [punescape(l, submergedattrs) for l in s.ylabel]
        subxticklabels = (
                [punescape(l, submergedattrs | {'x': x})
                        for l, x in zip(s.xticklabels, xlim_)]
                    if s.xticklabels is not None else None)
        subyticklabels = (
                [punescape(l, submergedattrs | {'y': y})
                        for l, y in zip(s.yticklabels, ylim_)]
                    if s.yticklabels is not None else None)

        # find actual width/height
        subwidth = sum(widths[s.x:s.x+s.xspan]) - sum(s.xmargin)
        subheight = sum(heights[s.y:s.y+s.yspan]) - sum(s.ymargin)

        # plot!
        plot = Plot(
                subwidth,
                subheight,
                color=color,
                dots=dots or not line_chars,
                braille=braille,
                xlim=xlim_,
                ylim=ylim_,
                xlog=xlog_,
                ylog=ylog_)

        for name, dataset in subdatasets.items():
            plot.plot(
                    sorted((x,y) for x,y in dataset),
                    char=datachars_[name],
                    line_char=dataline_chars_[name],
                    color=datacolors_[name])

        s.plot_ = plot
        s.width_ = subwidth
        s.height_ = subheight
        s.xlim_ = xlim_
        s.ylim_ = ylim_
        s.title_ = subtitle
        s.xlabel_ = subxlabel
        s.ylabel_ = subylabel
        s.xticklabels_ = subxticklabels
        s.yticklabels_ = subyticklabels


    ## now that everything's plotted, let's render things to the terminal

    # figure out margin
    xmargin = (
        len(ylabel_) + (1 if ylabel_ else 0),
        sum(grid[0,0].xmargin[:2]),
    )
    ymargin = (
        sum(grid[0,0].ymargin[:2]),
        grid[-1,-1].ymargin[-1],
    )

    # draw title?
    for line in title_:
        f.writeln('%*s%s' % (
                sum(xmargin[:2]), '',
                line.center(width_-xmargin[1])))

    # draw legend_above?
    if legend_above and legend_:
        for i in range(0, len(legend_), legend_cols):
            f.writeln('%*s%s' % (
                    max(
                        sum(xmargin[:2])
                            + (width_-xmargin[1]
                                - (sum(legend_widths)+2*(legend_cols-1)))
                            // 2,
                        0), '',
                    '  '.join('%s%s%s' % (
                            '\x1b[%sm' % legend_[i+j][1] if color else '',
                            '%-*s' % (legend_widths[j], legend_[i+j][0]),
                            '\x1b[m' if color else '')
                        for j in range(min(legend_cols, len(legend_)-i)))))

    for row in range(height_):
        # draw ylabel?
        f.write('%s ' % ''.join(
                    ('%*s%s%*s' % (
                            ymargin[-1], '',
                            line.center(height_-sum(ymargin)),
                            ymargin[0], ''))[row]
                        for line in ylabel_)
                if ylabel_ else '')

        for x_ in range(grid.width):
            # figure out the grid x/y position
            subrow = row
            y_ = len(heights)-1
            while subrow >= heights[y_]:
                subrow -= heights[y_]
                y_ -= 1

            s = grid[x_, y_]
            subrow = row - sum(heights[s.y+s.yspan:])

            # header
            if subrow < s.ymargin[-1]:
                # draw subtitle?
                if subrow < len(s.title_):
                    f.write('%*s%s' % (
                            sum(s.xmargin[:2]), '',
                            s.title_[subrow].center(s.width_)))
                else:
                    f.write('%*s%*s' % (
                            sum(s.xmargin[:2]), '',
                            s.width_, ''))
            # draw plot?
            elif subrow-s.ymargin[-1] < s.height_:
                subrow = subrow-s.ymargin[-1]

                # draw ysublabel?
                f.write('%-*s' % (
                        s.xmargin[0],
                        '%s ' % ''.join(
                            line.center(s.height_)[subrow]
                                for line in s.ylabel_)
                                if s.ylabel_ else ''))

                # draw yunits?
                if subrow == 0 and s.yticklabels_ != []:
                    f.write('%*s' % (
                            s.xmargin[1],
                            ((si2 if s.y2 else si)(s.ylim_[1]) + s.yunits
                                    if s.yticklabels_ is None
                                    else s.yticklabels_[1])
                                + ' '))
                elif subrow == s.height_-1 and s.yticklabels_ != []:
                    f.write('%*s' % (
                            s.xmargin[1],
                            ((si2 if s.y2 else si)(s.ylim_[0]) + s.yunits
                                    if s.yticklabels_ is None
                                    else s.yticklabels_[0])
                                + ' '))
                else:
                    f.write('%*s' % (
                            s.xmargin[1], ''))

                # draw plot!
                f.write(s.plot_.draw(subrow))

            # footer
            else:
                subrow = subrow-s.ymargin[-1]-s.height_

                # draw xunits?
                if subrow < (1 if s.xticklabels_ != [] else 0):
                    f.write('%*s%-*s%*s%*s' % (
                            sum(s.xmargin[:2]), '',
                            (5 if s.x2 else 4) + len(s.xunits)
                                if s.xticklabels_ is None
                                else len(s.xticklabels_[0]),
                            (si2 if s.x2 else si)(s.xlim_[0]) + s.xunits
                                if s.xticklabels_ is None
                                else s.xticklabels_[0],
                            s.width_ - (2*((5 if s.x2 else 4)+len(s.xunits))
                                if s.xticklabels_ is None
                                else sum(len(t)
                                    for t in s.xticklabels_)), '',
                            (5 if s.x2 else 4) + len(s.xunits)
                                if s.xticklabels_ is None
                                else len(s.xticklabels_[1]),
                            (si2 if s.x2 else si)(s.xlim_[1]) + s.xunits
                                if s.xticklabels_ is None
                                else s.xticklabels_[1]))
                # draw xsublabel?
                elif (subrow < s.ymargin[1]
                        or subrow-s.ymargin[1] >= len(s.xlabel_)):
                    f.write('%*s%*s' % (
                            sum(s.xmargin[:2]), '',
                            s.width_, ''))
                else:
                    f.write('%*s%s' % (
                            sum(s.xmargin[:2]), '',
                            s.xlabel_[subrow-s.ymargin[1]]
                                .center(s.width_)))

        # draw legend_right?
        if (legend_right and legend_
                and row >= ymargin[-1]
                and row-ymargin[-1] < len(legend_)):
            j = row-ymargin[-1]
            f.write(' %s%s%s' % (
                    '\x1b[%sm' % legend_[j][1] if color else '',
                    legend_[j][0],
                    '\x1b[m' if color else ''))

        f.writeln()

    # draw xlabel?
    for line in xlabel_:
        f.writeln('%*s%s' % (
                sum(xmargin[:2]), '',
                line.center(width_-xmargin[1])))

    # draw legend below?
    if legend_below and legend_:
        for i in range(0, len(legend_), legend_cols):
            f.writeln('%*s%s' % (
                    max(
                        sum(xmargin[:2])
                            + (width_-xmargin[1]
                                - (sum(legend_widths)+2*(legend_cols-1)))
                            // 2,
                        0), '',
                    '  '.join('%s%s%s' % (
                            '\x1b[%sm' % legend_[i+j][1] if color else '',
                            '%-*s' % (legend_widths[j], legend_[i+j][0]),
                            '\x1b[m' if color else '')
                        for j in range(min(legend_cols, len(legend_)-i)))))


def main(csv_paths, *,
        height=None,
        keep_open=False,
        head=False,
        cat=False,
        sleep=False,
        **args):
    # keep-open?
    if keep_open:
        try:
            while True:
                # register inotify before running the command, this avoids
                # modification race conditions
                if Inotify:
                    inotify = Inotify(csv_paths)

                if cat:
                    main_(sys.stdout, csv_paths,
                            # make space for shell prompt
                            height=height if height is not False else -1,
                            **args)
                else:
                    ring = RingIO(head=head)
                    main_(ring, csv_paths,
                            height=height if height is not False else 0,
                            **args)
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
        main_(sys.stdout, csv_paths,
                # make space for shell prompt
                height=height if height is not False else -1,
                **args)


if __name__ == "__main__":
    import sys
    import argparse
    import re
    parser = argparse.ArgumentParser(
            description="Plot CSV files in terminal.",
            allow_abbrev=False)
    parser.add_argument(
            'csv_paths',
            nargs='*',
            help="Input *.csv files.")
    parser.add_argument(
            '-b', '--by',
            action='append',
            help="Group by this field.")
    parser.add_argument(
            '-x',
            action='append',
            help="Field to use for the x-axis.")
    parser.add_argument(
            '-y',
            action='append',
            help="Field to use for the y-axis.")
    parser.add_argument(
            '-D', '--define',
            type=lambda x: (
                lambda k, vs: (
                    k.strip(),
                    {v.strip() for v in vs.split(',')})
                )(*x.split('=', 1)),
            action='append',
            help="Only include results where this field is this value. May "
                "include comma-separated options.")
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
            help="Add a label to use. Can be assigned to a specific group "
                "where a group is the comma-separated 'by' fields. Accepts %% "
                "modifiers. Also provides an ordering.")
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
            help="Add characters to use for points. Can be assigned to a "
                "specific group where a group is the comma-separated "
                "'by' fields. Accepts %% modifiers.")
    parser.add_argument(
            '-,', '--add-line-char', '--line-chars',
            dest='line_chars',
            action='append',
            type=lambda x: (
                lambda ks, v: (
                    tuple(k.strip() for k in ks.split(',')),
                    v.strip())
                )(*x.split('=', 1))
                    if '=' in x else x.strip(),
            help="Add characters to use for lines. Can be assigned to a "
                "specific group where a group is the comma-separated "
                "'by' fields. Accepts %% modifiers.")
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
            help="Add a color to use. Can be assigned to a specific group "
                "where a group is the comma-separated 'by' fields. Accepts %% "
                "modifiers.")
    parser.add_argument(
            '--color',
            choices=['never', 'always', 'auto'],
            default='auto',
            help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
            '-:', '--dots',
            action='store_true',
            help="Use 1x2 ascii dot characters. This is the default.")
    parser.add_argument(
            '-⣿', '--braille',
            action='store_true',
            help="Use 2x4 unicode braille characters. Note that braille "
                "characters sometimes suffer from inconsistent widths.")
    parser.add_argument(
            '-p', '--points',
            action='store_true',
            help="Only draw data points.")
    parser.add_argument(
            '-P', '--points-and-lines',
            action='store_true',
            help="Draw data points and lines.")
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
                "to 17.")
    parser.add_argument(
            '-X', '--xlim',
            type=lambda x: tuple(
                dat(x) if x.strip() else None
                    for x in x.split(',')),
            help="Range for the x-axis.")
    parser.add_argument(
            '-Y', '--ylim',
            type=lambda x: tuple(
                dat(x) if x.strip() else None
                    for x in x.split(',')),
            help="Range for the y-axis.")
    parser.add_argument(
            '--xlog',
            action='store_true',
            help="Use a logarithmic x-axis.")
    parser.add_argument(
            '--ylog',
            action='store_true',
            help="Use a logarithmic y-axis.")
    parser.add_argument(
            '--x2',
            action='store_true',
            help="Use base-2 prefixes for the x-axis.")
    parser.add_argument(
            '--y2',
            action='store_true',
            help="Use base-2 prefixes for the y-axis.")
    parser.add_argument(
            '--xunits',
            help="Units for the x-axis.")
    parser.add_argument(
            '--yunits',
            help="Units for the y-axis.")
    parser.add_argument(
            '--xlabel',
            help="Add a label to the x-axis. Accepts %% modifiers.")
    parser.add_argument(
            '--ylabel',
            help="Add a label to the y-axis. Accepts %% modifiers.")
    parser.add_argument(
            '--add-xticklabel',
            dest='xticklabels',
            action='append',
            help="Add an xticklabel. Accepts %% modifiers.")
    parser.add_argument(
            '--add-yticklabel',
            dest='yticklabels',
            action='append',
            help="Add an yticklabel.  Accepts %% modifiers.")
    parser.add_argument(
            '--title',
            help="Add a title. Accepts %% modifiers.")
    parser.add_argument(
            '-l', '--legend', '--legend-right',
            dest='legend_right',
            action='store_true',
            help="Place a legend to the right.")
    parser.add_argument(
            '--legend-above',
            action='store_true',
            help="Place a legend above.")
    parser.add_argument(
            '--legend-below',
            action='store_true',
            help="Place a legend below.")
    class AppendSubplot(argparse.Action):
        @staticmethod
        def parse(value):
            import copy
            subparser = copy.deepcopy(parser)
            subparser.prog = "%s --subplot" % parser.prog
            next(a for a in subparser._actions
                    if '--width' in a.option_strings).type = float
            next(a for a in subparser._actions
                    if '--height' in a.option_strings).type = float
            return subparser.parse_intermixed_args(shlex.split(value or ""))
        def __call__(self, parser, namespace, value, option):
            if not hasattr(namespace, 'subplots'):
                namespace.subplots = []
            namespace.subplots.append((
                    option.split('-')[-1],
                    self.__class__.parse(value)))
    parser.add_argument(
            '--subplot-above',
            action=AppendSubplot,
            help="Add subplot above with the same dataset. Takes an arg "
                "string to control the subplot which supports most (but "
                "not all) of the parameters listed here. The relative "
                "dimensions of the subplot can be controlled with -W/-H "
                "which now take a percentage.")
    parser.add_argument(
            '--subplot-below',
            action=AppendSubplot,
            help="Add subplot below with the same dataset.")
    parser.add_argument(
            '--subplot-left',
            action=AppendSubplot,
            help="Add subplot left with the same dataset.")
    parser.add_argument(
            '--subplot-right',
            action=AppendSubplot,
            help="Add subplot right with the same dataset.")
    parser.add_argument(
            '--subplot',
            type=AppendSubplot.parse,
            help="Add subplot-specific arguments to the main plot.")
    parser.add_argument(
            '-k', '--keep-open',
            action='store_true',
            help="Continue to open and redraw the CSV files in a loop.")
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

    def dictify(ns):
        if hasattr(ns, 'subplots'):
            ns.subplots = [(dir, dictify(subplot_ns))
                    for dir, subplot_ns in ns.subplots]
        if ns.subplot is not None:
            ns.subplot = dictify(ns.subplot)
        return {k: v
                for k, v in vars(ns).items()
                if v is not None}

    sys.exit(main(**dictify(parser.parse_intermixed_args())))
