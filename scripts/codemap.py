#!/usr/bin/env python3
#
# Inspired by d3 and brendangregg's flamegraph svg:
# - https://d3js.org
# - https://github.com/brendangregg/FlameGraph
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
import json
import math as mt
import os
import re
import shlex
import shutil
import subprocess as sp
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

CODE_PATH = ['./scripts/code.py']
STACK_PATH = ['./scripts/stack.py']
CTX_PATH = ['./scripts/ctx.py']


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

def iself(path):
    # check for an elf file's magic string (\x7fELF)
    with open(path, 'rb') as f:
        return f.read(4) == b'\x7fELF'

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
            else:
                i, key = 0, key
        else:
            i, key = key, ()

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
            # cycle based on index
            return best[1][i % len(best[1])]

        # fallback to defaults?
        if self.defaults is not None:
            return self.defaults[i, key]

        return None

    def __contains__(self, key):
        return self.__getitem__(key) is not None

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


def collect_code(obj_paths, *,
        code_path=CODE_PATH,
        **args):
    # note code-path may contain extra args
    cmd = code_path + ['-O-'] + obj_paths
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    code = json.load(proc.stdout)
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    return code

def collect_stack(ci_paths, *,
        stack_path=STACK_PATH,
        **args):
    # note stack-path may contain extra args
    cmd = stack_path + ['-O-', '--depth=2'] + ci_paths
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    stack = json.load(proc.stdout)
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    return stack

def collect_ctx(obj_paths, *,
        ctx_path=CTX_PATH,
        **args):
    # note stack-path may contain extra args
    cmd = ctx_path + ['-O-', '--depth=2', '--internal'] + obj_paths
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    ctx = json.load(proc.stdout)
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    return ctx


def main_(f, paths, *,
        namespace_depth=2,
        labels=[],
        chars=[],
        colors=[],
        color=False,
        dots=False,
        braille=False,
        width=None,
        height=None,
        no_header=False,
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
        height_ = 2 if not no_header else 1
    elif height > 0:
        height_ = height
    else:
        height_ = max(0, shutil.get_terminal_size((80, 5))[1] + height)

    # try to parse files as CSV/JSON
    results = []
    try:
        # if any file starts with elf magic (\x7fELF), assume input is
        # elf/callgraph files
        fs = []
        for path in paths:
            f_ = openio(path)
            if f_.buffer.peek(4)[:4] == b'\x7fELF':
                for f_ in fs:
                    f_.close()
                raise StopIteration()
            fs.append(f_)

        for f_ in fs:
            with f_:
                # csv or json? assume json starts with [
                is_json = (f_.buffer.peek(1)[:1] == b'[')

                # read csv?
                if not is_json:
                    results.extend(csv.DictReader(f_, restval=''))

                # read json?
                else:
                    results.extend(json.load(f_))

    # fall back to extracting code/stack/ctx info from elf/callgraph files
    except StopIteration:
        # figure out paths
        obj_paths = []
        ci_paths = []
        for path in paths:
            if iself(path):
                obj_paths.append(path)
            else:
                ci_paths.append(path)

        # find code/stack/ctx sizes
        if obj_paths:
            results.extend(collect_code(obj_paths, **args))
        if ci_paths:
            results.extend(collect_stack(ci_paths, **args))
        if obj_paths:
            results.extend(collect_ctx(obj_paths, **args))

    # don't render code/stack/ctx results if we don't have any
    nil_code = not any('code_size' in r for r in results)
    nil_frames = not any('stack_frame' in r for r in results)
    nil_ctx = not any('ctx_size' in r for r in results)

    # merge code/stack/ctx results
    functions = co.OrderedDict()
    for r in results:
        if r['function'] not in functions:
            functions[r['function']] = {'name': r['function']}
        # code things
        if 'code_size' in r:
            functions[r['function']]['code'] = dat(r['code_size'])
        # stack things, including callgraph
        if 'stack_frame' in r:
            functions[r['function']]['frame'] = dat(r['stack_frame'])
        if 'stack_limit' in r:
            functions[r['function']]['stack'] = dat(r['stack_limit'], mt.inf)
        if 'children' in r:
            if 'children' not in functions[r['function']]:
                functions[r['function']]['children'] = []
            functions[r['function']]['children'].extend(
                    r_['function']
                        for r_ in r['children']
                        if r_.get('stack_frame', '') != '')
        # ctx things, including any arguments
        if 'ctx_size' in r:
            functions[r['function']]['ctx'] = dat(r['ctx_size'])
        if 'children' in r:
            if 'args' not in functions[r['function']]:
                functions[r['function']]['args'] = []
            functions[r['function']]['args'].extend(
                    {'name': r_['function'],
                            'ctx': dat(r_['ctx_size']),
                            'attrs': r_}
                        for r_ in r['children']
                        if r_.get('ctx_size', '') != '')
        # keep track of other attrs for punescaping
        if 'attrs' not in functions[r['function']]:
            functions[r['function']]['attrs'] = {}
        functions[r['function']]['attrs'].update(r)

    # stack.py returns infinity for recursive functions, so we need to
    # recompute a bounded stack limit to show something useful
    def limitof(k, f, seen=set()):
        # found a cycle? stop here
        if k in seen:
            return 0

        limit = 0
        for child in f.get('children', []):
            if child not in functions:
                continue
            limit = max(limit, limitof(child, functions[child], seen | {k}))

        return f['frame'] + limit

    for k, f_ in functions.items():
        if 'stack' in f_:
            if mt.isinf(f_['stack']):
                f_['limit'] = limitof(k, f_)
            else:
                f_['limit'] = f_['stack']

    # organize into subsystems
    namespace_pattern = re.compile('_*[^_]+(?:_*$)?')
    namespace_slice = slice(namespace_depth if namespace_depth else None)
    subsystems = {}
    for k, f_ in functions.items():
        # ignore leading/trailing underscores
        f_['subsystem'] = ''.join(
                namespace_pattern.findall(k)[
                    namespace_slice])

        if f_['subsystem'] not in subsystems:
            subsystems[f_['subsystem']] = {'name': f_['subsystem']}

    # include ctx in subsystems to give them different colors
    for _, f_ in functions.items():
        for a in f_.get('args', []):
            a['subsystem'] = a['name']

            if a['subsystem'] not in subsystems:
                subsystems[a['subsystem']] = {'name': a['subsystem']}

    # sort to try to keep things reproducible
    functions = co.OrderedDict(sorted(functions.items()))
    subsystems = co.OrderedDict(sorted(subsystems.items()))

    # sum code/stack/ctx/attrs for punescaping
    for k, s in subsystems.items():
        s['code'] = sum(
                f.get('code', 0) for f in functions.values()
                    if f['subsystem'] == k)
        s['stack'] = max(
                (f.get('stack', 0) for f in functions.values()
                    if f['subsystem'] == k),
                default=0)
        s['ctx'] = max(
                (f.get('ctx', 0) for f in functions.values()
                    if f['subsystem'] == k),
                default=0)
        s['attrs'] = {k_: v_
                for f in functions.values()
                if f['subsystem'] == k
                for k_, v_ in f['attrs'].items()}

    # also build totals
    totals = {}
    totals['code'] = sum(
            f.get('code', 0) for f in functions.values())
    totals['stack'] = max(
            (f.get('stack', 0) for f in functions.values()),
            default=0)
    totals['ctx'] = max(
            (f.get('ctx', 0) for f in functions.values()),
            default=0)
    totals['attrs'] = {k: v
            for f in functions.values()
            for k, v in f['attrs'].items()}

    # assign colors to subsystems, note this is after sorting, but
    # before tile generation, we want code and stack tiles to have the
    # same color if they're in the same subsystem
    for i, (k, s) in enumerate(subsystems.items()):
        color__ = colors_[i, k]
        # don't punescape unless we have to
        if '%' in color__:
            color__ = punescape(color__, s['attrs'] | s)
        s['color'] = color__


    # build code heirarchy
    code = Tile.merge(
            Tile(   (f['subsystem'], f['name']),
                    # fallback to stack/ctx
                    f.get('code', 0) if not nil_code
                        else f.get('frame', 0) if not nil_frames
                        else f.get('ctx', 0),
                    attrs=f)
                for f in functions.values())

    # assign colors/chars/labels to code tiles
    for i, t in enumerate(code.leaves()):
        t.color = subsystems[t.attrs['subsystem']]['color']

        if (i, t.attrs['name']) in chars_:
            char__ = chars_[i, t.attrs['name']]
            if isinstance(char__, str):
                # don't punescape unless we have to
                if '%' in char__:
                    char__ = punescape(char__, t.attrs['attrs'] | t.attrs)
                char__ = char__[0] # limit to 1 char
            t.char = char__
        elif braille or dots:
            t.char = True
        elif len(t.attrs['subsystem']) < len(t.attrs['name']):
            t.char = (t.attrs['name'][len(t.attrs['subsystem']):].lstrip('_')
                    or '')[0]
        else:
            t.char = (t.attrs['subsystem'].rstrip('_').rsplit('_', 1)[-1]
                    or '')[0]

        if (i, t.attrs['name']) in labels_:
            label__ = labels_[i, t.attrs['name']]
            # don't punescape unless we have to
            if '%' in label__:
                label__ = punescape(label__, t.attrs['attrs'] | t.attrs)
            t.label = label__
        else:
            t.label = t.attrs['name']

    # scale width/height if requested now that we have our data
    if (to_scale is not None
            and (width is None or height is None)):
        total_value = (totals.get('code', 0) if not nil_code
                else totals.get('frame', 0) if not nil_frames
                else totals.get('ctx', 0))
        if total_value:
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
                        ((total_value * to_scale) / (height_*yscale))
                            / xscale)
            # scale height only
            elif width is not None:
                height_ = mt.ceil(
                        ((total_value * to_scale) / (width_*xscale))
                            / yscale)
            # scale based on aspect-ratio
            else:
                width_ = mt.ceil(
                        (mt.sqrt(total_value * to_scale)
                                * (aspect_ratio[0] / aspect_ratio[1]))
                            / xscale)
                height_ = mt.ceil(
                        ((total_value * to_scale) / (width_*xscale))
                            / yscale)

    # our general purpose partition function
    def partition(tile, **args):
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
            partition(t, **args)

    # create a canvas
    canvas = Canvas(
            width_,
            height_ - (1 if not no_header else 0),
            color=color,
            dots=dots,
            braille=braille)

    # sort and partition code
    code.sort()
    code.x = 0
    code.y = 0
    code.width = canvas.width
    code.height = canvas.height
    partition(code, **args)
    # align to pixel boundaries
    code.align()


    # render to canvas
    labels__ = []
    for t in code.leaves():
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
                    else chars_[0]),
                color=t.color if t.color is not None else colors_[0])

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
            f.writeln(punescape(title, totals['attrs'] | totals))
        else:
            f.writeln('code %d stack %s ctx %d' % (
                        totals.get('code', 0),
                        (lambda s: '∞' if mt.isinf(s) else s)(
                            totals.get('stack', 0)),
                        totals.get('ctx', 0)))

    # draw canvas
    for row in range(canvas.height//canvas.yscale):
        line = canvas.draw(row)
        f.writeln(line)

    if (args.get('error_on_recursion')
            and mt.isinf(totals.get('stack', 0))):
        sys.exit(2)


def main(paths, *,
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
                    inotify = Inotify(paths)

                if cat:
                    main_(sys.stdout, paths,
                            # make space for shell prompt
                            height=height if height is not False else -1,
                            **args)
                else:
                    ring = RingIO(head=head)
                    main_(ring, paths,
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
        main_(sys.stdout, paths,
                # make space for shell prompt
                height=height if height is not False else -1,
                **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Render code info as a treemap.",
            allow_abbrev=False)
    class AppendPath(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if getattr(namespace, 'paths', None) is None:
                namespace.paths = []
            if value is None:
                pass
            elif isinstance(value, str):
                namespace.paths.append(value)
            else:
                namespace.paths.extend(value)
    parser.add_argument(
            'obj_paths',
            nargs='*',
            action=AppendPath,
            help="Input *.o files.")
    parser.add_argument(
            'ci_paths',
            nargs='*',
            action=AppendPath,
            help="Input *.ci files.")
    parser.add_argument(
            'csv_paths',
            nargs='*',
            action=AppendPath,
            help="Input *.csv files.")
    parser.add_argument(
            'json_paths',
            nargs='*',
            action=AppendPath,
            help="Input *.json files.")
    parser.add_argument(
            '-_', '--namespace-depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Number of underscore-separated namespaces to partition by. "
                "0 treats every function as its own subsystem, while -1 uses "
                "the longest matching prefix. Defaults to 2, which is "
                "probably a good level of detail for most standalone "
                "libraries.")
    parser.add_argument(
            '-v', '--verbose',
            action='store_true',
            help="Output commands that run behind the scenes.")
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
            help="Add a label to use. Can be assigned to a specific "
                "function/subsystem. Accepts %% modifiers.")
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
                "function/subsystem. Accepts %% modifiers.")
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
                "function/subsystem. Accepts %% modifiers.")
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
            '-e', '--error-on-recursion',
            action='store_true',
            help="Error if any functions are recursive.")
    parser.add_argument(
            '--code-path',
            type=lambda x: x.split(),
            default=CODE_PATH,
            help="Path to the code.py script, may include flags. "
                "Defaults to %r." % CODE_PATH)
    parser.add_argument(
            '--stack-path',
            type=lambda x: x.split(),
            default=STACK_PATH,
            help="Path to the stack.py script, may include flags. "
                "Defaults to %r." % STACK_PATH)
    parser.add_argument(
            '--ctx-path',
            type=lambda x: x.split(),
            default=CTX_PATH,
            help="Path to the ctx.py script, may include flags. "
                "Defaults to %r." % CTX_PATH)
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
