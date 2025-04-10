#!/usr/bin/env python3
#
# Inspired by d3:
# https://d3js.org
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
import shutil
import time

try:
    import inotify_simple
except ModuleNotFoundError:
    inotify_simple = None


# we don't actually need that many chars/colors thanks to the
# 4-colorability of all 2d maps
COLORS = ['34', '31', '32', '35', '33', '36']

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

def fold(results, by=None, fields=None, defines=[]):
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

    # collect datasets
    datasets = co.OrderedDict()
    dataattrs = co.OrderedDict()
    for key in (keys if by else [()]):
        for field in fields:
            # organize by 'by' and field
            dataset = []
            dataattr = {}
            for r in results:
                # filter by 'by'
                if by and not all(
                        k in r and r[k] == v
                            for k, v in zip(by, key)):
                    continue

                # find field
                if field is not None:
                    if field not in r:
                        continue
                    try:
                        v = dat(r[field])
                    except ValueError:
                        continue
                else:
                    v = None

                # do _not_ sum v here, it's tempting but risks
                # incorrect and misleading results
                dataset.append(v)

                # include all fields in dataattrs in case we use
                # them for % modifiers
                dataattr.update(r)

            # hide 'field' if there is only one field
            key_ = key
            if len(fields or []) > 1 or not key_:
                key_ += (field,)
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


# a type to represent tiles
class Tile:
    def __init__(self, key, children, *,
            x=None, y=None, width=None, height=None,
            depth=None,
            attrs=None,
            label=None,
            color=None):
        self.key = key
        if isinstance(children, list):
            self.children = children
            self.value = sum(c.value for c in children)
        else:
            self.children = []
            self.value = children

        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self.depth = depth
        self.attrs = attrs
        self.label = label
        self.color = color

    def __repr__(self):
        return 'Tile(%r, %r, x=%r, y=%r, width=%r, height=%r)' % (
                ','.join(self.key), self.value,
                self.x, self.y, self.width, self.height)

    # recursively build heirarchy
    @staticmethod
    def merge(tiles, prefix=()):
        # organize by 'by' field
        tiles_ = co.OrderedDict()
        for t in tiles:
            if len(prefix)+1 >= len(t.key):
                tiles_[t.key] = t
            else:
                key = prefix + (t.key[len(prefix)],)
                if key not in tiles_:
                    tiles_[key] = []
                tiles_[key].append(t)

        tiles__ = []
        for key, t in tiles_.items():
            if isinstance(t, Tile):
                tiles__.append(t)
            else:
                tiles__.append(Tile.merge(t, key))
        tiles_ = tiles__

        return Tile(prefix, tiles_, depth=len(prefix))

    def __lt__(self, other):
        return self.value < other.value

    def __le__(self, other):
        return self.value <= other.value

    def __gt__(self, other):
        return self.value > other.value

    def __ge__(self, other):
        return self.value >= other.value

    # recursive traversals
    def tiles(self):
        yield self
        for child in self.children:
            yield from child.tiles()

    def leaves(self):
        for t in self.tiles():
            if not t.children:
                yield t

    # sort recursively
    def sort(self):
        self.children.sort(reverse=True)
        for t in self.children:
            t.sort()

    # recursive align to pixel boundaries
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

        # recurse
        for t in self.children:
            t.align()

    # return some interesting info about these tiles
    def stat(self):
        leaves = list(self.leaves())
        mean = self.value / max(len(leaves), 1)
        stddev = mt.sqrt(sum((t.value - mean)**2 for t in leaves)
                / max(len(leaves), 1))
        min_ = min((t.value for t in leaves), default=0)
        max_ = max((t.value for t in leaves), default=0)
        return {
            'total': self.value,
            'mean': mean,
            'stddev': stddev,
            'min': min_,
            'max': max_,
        }


# bounded division, limits result to dividend, useful for avoiding
# divide-by-zero issues
def bdiv(a, b):
    return a / max(b, 1)

# our partitioning schemes

def partition_binary(children, total, x, y, width, height):
    sums = [0]
    for t in children:
        sums.append(sums[-1] + t.value)

    # recursively partition into a roughly weight-balanced binary tree
    def partition_(i, j, value, x, y, width, height):
        # no child? guess we're done
        if i == j:
            return
        # single child? assign the partition
        elif i == j-1:
            children[i].x = x
            children[i].y = y
            children[i].width = width
            children[i].height = height
            return

        # binary search to find best split index
        target = sums[i] + (value / 2)
        k = bisect.bisect(sums, target, i+1, j-1)

        # nudge split index if it results in less error
        if k > i+1 and (sums[k] - target) > (target - sums[k-1]):
            k -= 1

        l = sums[k] - sums[i]
        r = value - l

        # split horizontally?
        if width > height:
            dx = bdiv(sums[k] - sums[i], value) * width
            partition_(i, k, l, x, y, dx, height)
            partition_(k, j, r, x+dx, y, width-dx, height)

        # split vertically?
        else:
            dy = bdiv(sums[k] - sums[i], value) * height
            partition_(i, k, l, x, y, width, dy)
            partition_(k, j, r, x, y+dy, width, height-dy)

    partition_(0, len(children), total, x, y, width, height)

def partition_slice(children, total, x, y, width, height):
    # give each child a slice
    x_ = x
    for t in children:
        t.x = x_
        t.y = y
        t.width = bdiv(t.value, total) * width
        t.height = height

        x_ += t.width

def partition_dice(children, total, x, y, width, height):
    # give each child a slice
    y_ = y
    for t in children:
        t.x = x
        t.y = y_
        t.width = width
        t.height = bdiv(t.value, total) * height

        y_ += t.height

def partition_squarify(children, total, x, y, width, height, *,
        aspect_ratio=(1,1)):
    # this algorithm is described here:
    # https://www.win.tue.nl/~vanwijk/stm.pdf
    i = 0
    x_ = x
    y_ = y
    total_ = total
    width_ = width
    height_ = height
    # note we don't really care about width vs height until
    # actually slicing
    ratio = max(aspect_ratio[0] / aspect_ratio[1],
            aspect_ratio[1] / aspect_ratio[0])

    while i < len(children):
        # calculate initial aspect ratio
        sum_ = children[i].value
        min_ = children[i].value
        max_ = children[i].value
        w = total_ * bdiv(ratio,
                max(bdiv(width_, height_), bdiv(height_, width_)))
        ratio_ = max(bdiv(max_*w, sum_**2), bdiv(sum_**2, min_*w))

        # keep adding children to this row/col until it starts to hurt
        # our aspect ratio
        j = i + 1
        while j < len(children):
            sum__ = sum_ + children[j].value
            min__ = min(min_, children[j].value)
            max__ = max(max_, children[j].value)
            ratio__ = max(bdiv(max__*w, sum__**2), bdiv(sum__**2, min__*w))
            if ratio__ > ratio_:
                break

            sum_ = sum__
            min_ = min__
            max_ = max__
            ratio_ = ratio__
            j += 1

        # vertical col? dice horizontally?
        if width_ > height_:
            dx = bdiv(sum_, total_) * width_
            partition_dice(children[i:j], sum_, x_, y_, dx, height_)
            x_ += dx
            width_ -= dx

        # horizontal row? slice vertically?
        else:
            dy = bdiv(sum_, total_) * height_
            partition_slice(children[i:j], sum_, x_, y_, width_, dy)
            y_ += dy
            height_ -= dy

        # start partitioning the other direction
        total_ -= sum_
        i = j


def main_(f, csv_paths, *,
        by=None,
        fields=None,
        defines=[],
        labels=[],
        chars=[],
        colors=[],
        color=False,
        dots=False,
        braille=False,
        width=None,
        height=None,
        no_header=False,
        no_stats=False,
        to_scale=None,
        aspect_ratio=(1,1),
        tiny=False,
        title=None,
        padding=0,
        label=False,
        no_label=False,
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

    # tiny mode?
    if tiny:
        if to_scale is None:
            to_scale = 1
        no_header = True

    # what chars/colors/labels to use?
    chars_ = []
    for char in chars:
        if isinstance(char, tuple):
            chars_.extend((char[0], c) for c in psplit(char[1]))
        else:
            chars_.extend(psplit(char))
    chars_ = Attr(chars_)

    colors_ = Attr(colors, defaults=COLORS)

    labels_ = Attr(labels)

    # figure out width/height
    if width is None:
        width_ = min(80, shutil.get_terminal_size((80, 5))[0])
    elif width > 0:
        width_ = width
    else:
        width_ = max(0, shutil.get_terminal_size((80, 5))[0] + width)

    if height is None:
        height_ = (2
                if not no_header
                    and (title is not None or not no_stats)
                else 1)
    elif height > 0:
        height_ = height
    else:
        height_ = max(0, shutil.get_terminal_size((80, 5))[1] + height)

    # first collect results from CSV files
    fields_, results = collect(csv_paths, defines)

    if not by and not fields:
        print("error: needs --by or --fields to figure out fields",
                file=sys.stderr)
        sys.exit(-1)

    # if by not specified, guess it's anything not in fields/defines
    if not by:
        by = [k for k in fields_
                if k not in (fields or [])
                    and not any(k == k_ for k_, _ in defines)]

    # if fields not specified, guess it's anything not in by/defines
    if not fields:
        fields = [k for k in fields_
                if k not in (by or [])
                    and not any(k == k_ for k_, _ in defines)]

    # then extract the requested dataset
    datasets, dataattrs = fold(results, by, fields, defines)

    # build tile heirarchy
    children = []
    for key, dataset in datasets.items():
        for i, v in enumerate(dataset):
            children.append(Tile(
                key + ((str(i),) if len(dataset) > 1 else ()),
                v,
                attrs=dataattrs[key]))

    tile = Tile.merge(children)

    # merge attrs
    for t in tile.tiles():
        if t.children:
            t.attrs = {k: v
                    for t_ in t.leaves()
                    for k, v in t_.attrs.items()}
            # also sum fields here in case they're used by % modifiers,
            # note other fields are _not_ summed
            for k in fields:
                t.attrs[k] = sum(t_.value
                        for t_ in t.leaves()
                        if len(fields) == 1 or t_.key[len(by)] == k)

    # assign colors/labels before sorting to keep things reproducible

    # use colors for top of tree
    for i, t in enumerate(tile.children):
        for t_ in t.tiles():
            color__ = colors_[i, t_.key]
            # don't punescape unless we have to
            if '%' in color__:
                color__ = punescape(color__, t_.attrs)
            t_.color = color__

    # and chars/labels for bottom of tree
    for i, t in enumerate(tile.leaves()):
        if (i, t.key) in chars_:
            char__ = chars_[i, t.key]
            if isinstance(char__, str):
                # don't punescape unless we have to
                if '%' in char__:
                    char__ = punescape(char__, t.attrs)
                char__ = char__[0] # limit to 1 char
            t.char = char__

        if (i, t.key) in labels_:
            label__ = labels_[i, t.key]
            # don't punescape unless we have to
            if '%' in label__:
                label__ = punescape(label__, t.attrs)
            t.label = label__

    # scale width/height if requested now that we have our data
    if (to_scale
            and (width is None or height is None)
            and tile.value != 0):
        # scale if needed
        if braille:
            xscale, yscale = 2, 4
        elif dots:
            xscale, yscale = 1, 2
        else:
            xscale, yscale = 1, 1

        # scale width only
        if height is not None:
            width_ = mt.ceil(
                    ((tile.value * to_scale) / (height_*yscale))
                        / xscale)
        # scale height only
        elif width is not None:
            height_ = mt.ceil(
                    ((tile.value * to_scale) / (width_*xscale))
                        / yscale)
        # scale based on aspect-ratio
        else:
            width_ = mt.ceil(
                    (mt.sqrt(tile.value * to_scale)
                            * (aspect_ratio[0] / aspect_ratio[1]))
                        / xscale)
            height_ = mt.ceil(
                    ((tile.value * to_scale) / (width_*xscale))
                        / yscale)

    # create a canvas
    canvas = Canvas(
            width_,
            height_ - (1
                if not no_header
                    and (title is not None or not no_stats)
                else 0),
            color=color,
            dots=dots,
            braille=braille)

    # sort
    tile.sort()

    # recursively partition tiles
    tile.x = 0
    tile.y = 0
    tile.width = canvas.width
    tile.height = canvas.height
    def partition(tile):
        if tile.depth == 0:
            # apply top padding
            tile.x += padding
            tile.y += padding
            tile.width  -= min(padding, tile.width)
            tile.height -= min(padding, tile.height)
            # apply bottom padding
            if not tile.children:
                tile.width  -= min(padding, tile.width)
                tile.height -= min(padding, tile.height)

            x__ = tile.x
            y__ = tile.y
            width__ = tile.width
            height__ = tile.height

        else:
            # apply bottom padding
            if not tile.children:
                tile.width  -= min(padding, tile.width)
                tile.height -= min(padding, tile.height)

            x__ = tile.x
            y__ = tile.y
            width__ = tile.width
            height__ = tile.height

        # partition via requested scheme
        if tile.children:
            if args.get('binary'):
                partition_binary(tile.children, tile.value,
                        x__, y__, width__, height__)
            elif (args.get('slice')
                    or (args.get('slice_and_dice') and (tile.depth & 1) == 0)
                    or (args.get('dice_and_slice') and (tile.depth & 1) == 1)):
                partition_slice(tile.children, tile.value,
                        x__, y__, width__, height__)
            elif (args.get('dice')
                    or (args.get('slice_and_dice') and (tile.depth & 1) == 1)
                    or (args.get('dice_and_slice') and (tile.depth & 1) == 0)):
                partition_dice(tile.children, tile.value,
                        x__, y__, width__, height__)
            elif (args.get('squarify')
                    or args.get('squarify_ratio')
                    or args.get('rectify')):
                partition_squarify(tile.children, tile.value,
                        x__, y__, width__, height__,
                        aspect_ratio=(args['squarify_ratio'], 1)
                                if args.get('squarify_ratio')
                            else (width_, height_)
                                if args.get('rectify')
                            else (1, 1))
            else:
                # default to binary partitioning
                partition_binary(tile.children, tile.value,
                        x__, y__, width__, height__)

        # recursively partition
        for t in tile.children:
            partition(t)

    partition(tile)

    # align to pixel boundaries
    tile.align()

    # render to canvas
    labels__ = []
    for t in tile.leaves():
        x__ = t.x
        y__ = t.y
        width__ = t.width
        height__ = t.height
        # skip anything with zero weight/height after aligning things
        if width__ == 0 or height__ == 0:
            continue

        # flip y
        y__ = canvas.height - (y__+height__)

        canvas.rect(x__, y__, width__, height__,
                # default to first letter of the last part of the key
                char=(t.char if getattr(t, 'char', None) is not None
                    else True if braille or dots
                    else t.key[len(by)-1][0] if t.key and t.key[len(by)-1]
                    else chars_.get(0)),
                color=t.color if t.color is not None else colors_.get(0))

        if label or (labels and not no_label):
            if t.label is not None:
                label__ = t.label
            else:
                label__ = ','.join(t.key)

            # render these later so they get priority
            labels__.append((x__, y__+height__-1, label__,
                    width__, height__))

    for label__ in labels__:
        canvas.label(*label__)

    # print some summary info
    if not no_header:
        if title:
            title_ = punescape(title, tile.attrs)
        if not no_stats:
            stat = tile.stat()
            stat_ = 'total %d, avg %d +-%dσ, min %d, max %d' % (
                    stat['total'],
                    stat['mean'], stat['stddev'],
                    stat['min'], stat['max'])
        if title and not no_stats:
            f.writeln('%s%*s%s' % (
                    title_,
                    max(width_-len(stat_)-len(title_), 0), ' ',
                    stat_))
        elif title:
            f.writeln(title_)
        elif not no_stats:
            f.writeln(stat_)

    # draw canvas
    for row in range(canvas.height//canvas.yscale):
        line = canvas.draw(row)
        f.writeln(line)


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
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Render CSV files as a treemap.",
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
            '-f', '--field',
            dest='fields',
            action='append',
            help="Field to use for tile sizes.")
    parser.add_argument(
            '-D', '--define',
            dest='defines',
            action='append',
            type=lambda x: (
                lambda k, vs: (
                    k.strip(),
                    {v.strip() for v in vs.split(',')})
                )(*x.split('=', 1)),
            help="Only include results where this field is this value.")
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
                "modifiers.")
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
            help="Add characters to use. Can be assigned to a specific group "
                "where a group is the comma-separated 'by' fields. Accepts %% "
                "modifiers.")
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
            '--no-header',
            action='store_true',
            help="Don't show the header.")
    parser.add_argument(
            '--no-stats',
            action='store_true',
            help="Don't show data stats in the header.")
    parser.add_argument(
            '--binary',
            action='store_true',
            help="Use the binary partitioning scheme. This attempts to "
                "recursively subdivide the tiles into a roughly "
                "weight-balanced binary tree. This is the default.")
    parser.add_argument(
            '--slice',
            action='store_true',
            help="Use the slice partitioning scheme. This simply slices "
                "tiles vertically.")
    parser.add_argument(
            '--dice',
            action='store_true',
            help="Use the dice partitioning scheme. This simply slices "
                "tiles horizontally.")
    parser.add_argument(
            '--slice-and-dice',
            action='store_true',
            help="Use the slice-and-dice partitioning scheme. This "
                "alternates between slicing and dicing each layer.")
    parser.add_argument(
            '--dice-and-slice',
            action='store_true',
            help="Use the dice-and-slice partitioning scheme. This is like "
                "slice-and-dice, but flipped.")
    parser.add_argument(
            '--squarify',
            action='store_true',
            help="Use the squarify partitioning scheme. This is a greedy "
                "algorithm created by Mark Bruls et al that tries to "
                "minimize tile aspect ratios.")
    parser.add_argument(
            '--rectify',
            action='store_true',
            help="Use the rectify partitioning scheme. This is like "
                "squarify, but tries to match the aspect ratio of the "
                "window.")
    parser.add_argument(
            '--squarify-ratio',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            help="Specify an explicit ratio for the squarify algorithm. "
                "Implies --squarify.")
    parser.add_argument(
            '--to-scale',
            nargs='?',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            const=1,
            help="Scale the resulting treemap such that 1 pixel ~= 1/scale "
                "units. Defaults to scale=1. ")
    parser.add_argument(
            '-R', '--aspect-ratio',
            type=lambda x: (
                tuple(float(v) for v in x.split(':', 1))
                    if ':' in x else (float(x), 1)),
            help="Aspect ratio to use with --to-scale. Defaults to 1:1.")
    parser.add_argument(
            '-t', '--tiny',
            action='store_true',
            help="Tiny mode, alias for --to-scale=1 and --no-header.")
    parser.add_argument(
            '--title',
            help="Add a title. Accepts %% modifiers.")
    parser.add_argument(
            '--padding',
            type=float,
            help="Padding to add to each level of the treemap. Defaults to 0.")
    parser.add_argument(
            '-l', '--label',
            action='store_true',
            help="Render labels.")
    parser.add_argument(
            '--no-label',
            action='store_true',
            help="Don't render any labels.")
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
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
