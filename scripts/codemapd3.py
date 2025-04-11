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
import itertools as it
import json
import math as mt
import re
import shlex
import shutil
import subprocess as sp


# some nicer colors borrowed from Seaborn
# note these include a non-opaque alpha
COLORS = [
    '#7995c4', # was '#4c72b0bf', # blue
    '#e6a37d', # was '#dd8452bf', # orange
    '#80be8e', # was '#55a868bf', # green
    '#d37a7d', # was '#c44e52bf', # red
    '#a195c6', # was '#8172b3bf', # purple
    '#ae9a88', # was '#937860bf', # brown
    '#e3a8d2', # was '#da8bc3bf', # pink
    '#a9a9a9', # was '#8c8c8cbf', # gray
    '#d9cb97', # was '#ccb974bf', # yellow
    '#8bc8da', # was '#64b5cdbf', # cyan
]
COLORS_DARK = [
    '#7997b7', # was '#a1c9f4bf', # blue
    '#bf8761', # was '#ffb482bf', # orange
    '#6aac79', # was '#8de5a1bf', # green
    '#bf7774', # was '#ff9f9bbf', # red
    '#9c8cbf', # was '#d0bbffbf', # purple
    '#a68c74', # was '#debb9bbf', # brown
    '#bb84ab', # was '#fab0e4bf', # pink
    '#9b9b9b', # was '#cfcfcfbf', # gray
    '#bfbe7a', # was '#fffea3bf', # yellow
    '#8bb5b4', # was '#b9f2f0bf', # cyan
]

WIDTH = 750
HEIGHT = 350
FONT = ['sans-serif']
FONT_SIZE = 10

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
#
# attrs can override __getitem__ for lazy attr generation
def punescape(s, attrs=None):
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
            if attrs is not None:
                try:
                    v = attrs[m.group('field')]
                except KeyError:
                    return m.group()
            else:
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
        aspect_ratio=1/1):
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
    ratio = max(aspect_ratio, 1/aspect_ratio)

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


def main(paths, output, *,
        namespace_depth=2,
        quiet=False,
        labels=[],
        colors=[],
        width=None,
        height=None,
        no_header=False,
        no_mode=False,
        no_stack=False,
        stack_ratio=1/5,
        no_ctx=False,
        no_frames=False,
        no_javascript=False,
        mode_callgraph=False,
        mode_deepest=False,
        mode_callees=False,
        mode_callers=False,
        to_scale=None,
        to_ratio=1/1,
        title=None,
        padding=1,
        no_label=False,
        tiny=False,
        nested=False,
        dark=False,
        font=FONT,
        font_size=FONT_SIZE,
        background=None,
        **args):
    # tiny mode?
    if tiny:
        if to_scale is None:
            to_scale = 1
        no_header = True
        no_label = True
        no_stack = True
        no_javascript = True

    # default to all modes
    if (not mode_callgraph
            and not mode_deepest
            and not mode_callees
            and not mode_callers):
        mode_callgraph = True
        mode_deepest = True
        mode_callees = True
        mode_callers = True

    # what colors/labels to use?
    colors_ = Attr(colors, defaults=COLORS_DARK if dark else COLORS)

    labels_ = Attr(labels)

    if background is not None:
        background_ = background
    elif dark:
        background_ = '#000000'
    else:
        background_ = '#ffffff'

    # figure out width/height
    if width is not None:
        width_ = width
    else:
        width_ = WIDTH

    if height is not None:
        height_ = height
    else:
        height_ = HEIGHT

    # try to parse files as CSV/JSON
    results = []
    try:
        # if any file starts with elf magic (\x7fELF), assume input is
        # elf/callgraph files
        fs = []
        for path in paths:
            f = openio(path)
            if f.buffer.peek(4)[:4] == b'\x7fELF':
                for f_ in fs:
                    f_.close()
                raise StopIteration()
            fs.append(f)

        for f in fs:
            with f:
                # csv or json? assume json starts with [
                is_json = (f.buffer.peek(1)[:1] == b'[')

                # read csv?
                if not is_json:
                    results.extend(csv.DictReader(f, restval=''))

                # read json?
                else:
                    results.extend(json.load(f))

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
    if nil_frames:
        no_frames = True
    nil_ctx = not any('ctx_size' in r for r in results)
    if nil_ctx:
        no_ctx = True
    if no_frames and no_ctx:
        no_stack = True

    # merge code/stack/ctx results
    functions = co.OrderedDict()
    for r in results:
        if 'function' not in r:
            continue
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

    for k, f in functions.items():
        if 'stack' in f:
            if mt.isinf(f['stack']):
                f['limit'] = limitof(k, f)
            else:
                f['limit'] = f['stack']

    # organize into subsystems
    namespace_pattern = re.compile('_*[^_]+(?:_*$)?')
    namespace_slice = slice(namespace_depth if namespace_depth else None)
    subsystems = {}
    for k, f in functions.items():
        # ignore leading/trailing underscores
        f['subsystem'] = ''.join(
                namespace_pattern.findall(k)[
                    namespace_slice])

        if f['subsystem'] not in subsystems:
            subsystems[f['subsystem']] = {'name': f['subsystem']}

    # include ctx in subsystems to give them different colors
    for _, f in functions.items():
        for a in f.get('args', []):
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

    # assign colors/labels to code tiles
    for i, t in enumerate(code.leaves()):
        # skip the top tile, yes this can happen if we have no code
        if t.depth == 0:
            continue

        t.color = subsystems[t.attrs['subsystem']]['color']

        if (i, t.attrs['name']) in labels_:
            label__ = labels_[i, t.attrs['name']]
            # don't punescape unless we have to
            if '%' in label__:
                label__ = punescape(label__, t.attrs['attrs'] | t.attrs)
            t.label = label__
        else:
            t.label = '%s%s%s%s' % (
                    t.attrs['name'],
                    '\ncode %d' % t.attrs.get('code', 0)
                        if not nil_code else '',
                    '\nstack %s' % (lambda s: '∞' if mt.isinf(s) else s)(
                            t.attrs.get('stack', 0))
                        if not nil_frames else '',
                    '\nctx %d' % t.attrs.get('ctx', 0)
                        if not nil_ctx else '')

    # build stack heirarchies
    if not no_stack and not no_frames:
        stacks = co.OrderedDict()
        for k, f in functions.items():
            stack = []
            def rec(f, seen=set()):
                if f['name'] in seen:
                    stack.append(f)
                    return
                seen.add(f['name'])

                stack.append(f)

                if f.get('children'):
                    hot = max(f['children'], key=lambda k:
                            functions[k].get('limit', 0)
                                if k not in seen else -1)
                    rec(functions[hot], seen)
            rec(f)

            stacks[k] = Tile.merge(
                    Tile(   (f['name'],),
                            f.get('frame', 0),
                            attrs=f)
                        for f in stack)

            # assign colors/labels to stack tiles
            for i, t in enumerate(stacks[k].leaves()):
                t.color = subsystems[t.attrs['subsystem']]['color']
                if (i, t.attrs['name']) in labels_:
                    label__ = labels_[i, t.attrs['name']]
                    # don't punescape unless we have to
                    if '%' in label__:
                        label__ = punescape(label__,
                                t.attrs['attrs'] | t.attrs)
                    t.label = label__
                else:
                    t.label = '%s\nframe %d' % (
                            t.attrs['name'],
                            t.attrs.get('frame', 0))

    # build ctx heirarchies
    if not no_stack and not no_ctx:
        ctxs = co.OrderedDict()
        for k, f in functions.items():
            if f.get('args'):
                args_ = f['args']
            else:
                args_ = [{
                    'name': k,
                    'subsystem': f['subsystem'],
                    'ctx': f.get('ctx', 0),
                    'attrs': f}]

            ctxs[k] = Tile.merge(
                    Tile(   (a['name'],),
                            a.get('ctx', 0),
                            attrs=a)
                        for a in args_)

            # assign colors/labels to ctx tiles
            for i, t in enumerate(ctxs[k].leaves()):
                t.color = subsystems[t.attrs['subsystem']]['color']
                if (i, t.attrs['name']) in labels_:
                    label__ = labels_[i, t.attrs['name']]
                    # don't punescape unless we have to
                    if '%' in label__:
                        label__ = punescape(label__,
                                t.attrs['attrs'] | t.attrs)
                    t.label = label__
                else:
                    t.label = '%s\nctx %d' % (
                            t.attrs['name'],
                            t.attrs.get('ctx', 0))

    # scale width/height if requested now that we have our data
    if (to_scale is not None
            and (width is None or height is None)):
        total_value = (totals.get('code', 0) if not nil_code
                else totals.get('frame', 0) if not nil_frames
                else totals.get('ctx', 0))
        if total_value:
            # don't include header/stack in scale
            width__ = width_
            height__ = height_
            if not no_header:
                height__ -= mt.ceil(FONT_SIZE * 1.3)
            if not no_stack:
                width__ *= (1 - stack_ratio)

            # scale width only
            if height is not None:
                width__ = mt.ceil((total_value * to_scale) / max(height__, 1))
            # scale height only
            elif width is not None:
                height__ = mt.ceil((total_value * to_scale) / max(width__, 1))
            # scale based on aspect-ratio
            else:
                width__ = mt.ceil(mt.sqrt(total_value * to_scale * to_ratio))
                height__ = mt.ceil((total_value * to_scale) / max(width__, 1))

            if not no_stack:
                width__ /= (1 - stack_ratio)
            if not no_header:
                height__ += mt.ceil(FONT_SIZE * 1.3)
            width_ = width__
            height_ = height__

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
                        aspect_ratio=(
                            args['squarify_ratio']
                            if args.get('squarify_ratio')
                            else width_/height_
                            if args.get('rectify')
                            else 1/1))
            else:
                # default to binary partitioning
                partition_binary(tile.children, tile.value,
                        x__, y__, width__, height__)

        # recursively partition
        for t in tile.children:
            partition(t, **args)

    # create space for header
    x__ = 0
    y__ = 0
    width__ = width_
    height__ = height_
    if not no_header:
        y__ += mt.ceil(FONT_SIZE * 1.3)
        height__ -= min(mt.ceil(FONT_SIZE * 1.3), height__)

    # split code/stack
    if not no_stack:
        code_split = width__ * (1 - stack_ratio)
    else:
        code_split = width__

    # sort and partition code
    code.sort()
    code.x = x__
    code.y = y__
    code.width = code_split
    code.height = height__
    partition(code, **args)
    # align to pixel boundaries
    code.align()

    # partition stacks/ctxs
    if not no_stack:
        deepest = max(functions.values(),
                key=lambda f:
                    (f.get('limit', 0) if not no_frames else 0)
                        + (f.get('ctx', 0) if not no_ctx else 0))

        for k, f in functions.items():
            # scale to deepest stack/ctx
            height___ = height__ * bdiv(
                    (f.get('limit', 0) if not no_frames else 0)
                        + (f.get('ctx', 0) if not no_ctx else 0),
                    (deepest.get('limit', 0) if not no_frames else 0)
                        + (deepest.get('ctx', 0) if not no_ctx else 0))

            # split stack/ctx
            ctx_split = height___ * bdiv(
                    (f.get('ctx', 0) if not no_ctx else 0),
                    (f.get('limit', 0) if not no_frames else 0)
                        + (f.get('ctx', 0) if not no_ctx else 0))

            # partition ctx
            if not no_ctx:
                ctx = ctxs[k]
                ctx.x = code.x + code.width + 1
                ctx.y = y__
                ctx.width = width__ - ctx.x
                ctx.height = ctx_split
                partition(ctx, slice=True)
                # align to pixel boundaries
                ctx.align()

            # partition stack
            if not no_frames:
                stack = stacks[k]
                stack.x = code.x + code.width + 1
                stack.y = ctx.y + ctx.height + 1 if ctx_split > 0 else y__
                stack.width = width__ - stack.x
                stack.height = height___ - (stack.y - y__)
                partition(stack, dice=True)
                # align to pixel boundaries
                stack.align()


    # create svg file
    with openio(output, 'w') as f:
        def writeln(s=''):
            f.write(s)
            f.write('\n')
        f.writeln = writeln

        # yes this is svg
        f.write('<svg '
                'xmlns="http://www.w3.org/2000/svg" '
                'viewBox="0,0,%(width)d,%(height)d" '
                'width="%(width)d" '
                'height="%(height)d" '
                'style="max-width: 100%%; '
                        'height: auto; '
                        'font: %(font_size)dpx %(font)s; '
                        'background-color: %(background)s; '
                        'user-select: %(user_select)s;">' % dict(
                    width=width_,
                    height=height_,
                    font=','.join(font),
                    font_size=font_size,
                    background=background_,
                    user_select='none' if not no_javascript else 'auto'))

        # create header
        if not no_header:
            f.write('<g '
                    'id="header" '
                    '%(js)s>' % dict(
                        js= 'cursor="pointer" '
                            'onclick="click_header(this,event)">'
                                if not no_javascript else ''))
            # add an invisible rect to make things more clickable
            f.write('<rect '
                    'x="%(x)d" '
                    'y="%(y)d" '
                    'width="%(width)d" '
                    'height="%(height)d" '
                    'opacity="0">' % dict(
                        x=0,
                        y=0,
                        width=width_,
                        height=y__))
            f.write('</rect>')
            f.write('<text fill="%(color)s">' % dict(
                    color='#ffffff' if dark else '#000000'))
            f.write('<tspan x="3" y="1.1em">')
            if title:
                f.write(punescape(title, totals['attrs'] | totals))
            else:
                f.write('code %d stack %s ctx %d' % (
                        totals.get('code', 0),
                        (lambda s: '∞' if mt.isinf(s) else s)(
                            totals.get('stack', 0)),
                        totals.get('ctx', 0)))
            f.write('</tspan>')
            if not no_mode and not no_javascript:
                f.write('<tspan id="mode" x="%(x)d" y="1.1em" '
                        'text-anchor="end">' % dict(
                            x=width_-3))
                f.write('mode: %s' % (
                        'callgraph' if mode_callgraph
                            else 'deepest' if mode_deepest
                            else 'callees' if mode_callees
                            else 'callers'))
                f.write('</tspan>')
            f.write('</text>')
            f.write('</g>')

        # create code tiles
        for i, t in enumerate(code.leaves()):
            # skip the top tile, yes this can happen if we have no code
            if t.depth == 0:
                continue
            # skip anything with zero weight/height after aligning things
            if t.width == 0 or t.height == 0:
                continue

            f.write('<g '
                    'id="c-%(name)s" '
                    'class="tile code" '
                    'transform="translate(%(x)d,%(y)d)" '
                    '%(js)s>' % dict(
                        name=t.attrs['name'],
                        x=t.x,
                        y=t.y,
                        js= 'data-name="%(name)s" '
                            # precompute x/y for javascript, svg makes this
                            # weirdly difficult to figure out post-transform
                            'data-x="%(x)d" '
                            'data-y="%(y)d" '
                            'data-width="%(width)d" '
                            'data-height="%(height)d" '
                            'onmouseenter="enter_tile(this,event)" '
                            'onmouseleave="leave_tile(this,event)" '
                            'onclick="click_tile(this,event)">' % dict(
                                    name=t.attrs['name'],
                                    x=t.x,
                                    y=t.y,
                                    width=t.width,
                                    height=t.height)
                                if not no_javascript else ''))
            # add an invisible rect to make things more clickable
            f.write('<rect '
                    'width="%(width)d" '
                    'height="%(height)d" '
                    'opacity="0">' % dict(
                        width=t.width + padding,
                        height=t.height + padding))
            f.write('</rect>')
            f.write('<title>')
            f.write(t.label)
            f.write('</title>')
            f.write('<rect '
                    'id="c-tile-%(id)s" '
                    'fill="%(color)s" '
                    'width="%(width)d" '
                    'height="%(height)d">' % dict(
                        id=i,
                        color=t.color,
                        width=t.width,
                        height=t.height))
            f.write('</rect>')
            if not no_label:
                f.write('<clipPath id="c-clip-%s">' % i)
                f.write('<use href="#c-tile-%s">' % i)
                f.write('</use>')
                f.write('</clipPath>')
                f.write('<text clip-path="url(#c-clip-%s)">' % i)
                for j, l in enumerate(t.label.split('\n')):
                    if j == 0:
                        f.write('<tspan x="3" y="1.1em">')
                        f.write(l)
                        f.write('</tspan>')
                    else:
                        if t.children:
                            f.write('<tspan dx="3" y="1.1em" '
                                    'fill-opacity="0.7">')
                            f.write(l)
                            f.write('</tspan>')
                        else:
                            f.write('<tspan x="3" dy="1.1em" '
                                    'fill-opacity="0.7">')
                            f.write(l)
                            f.write('</tspan>')
                f.write('</text>')
            f.write('</g>')

        # create stack/ctx tiles
        if not no_stack and (not no_ctx or not no_frames):
          for i, k in enumerate(functions.keys()):
            # only include the deepest stack if no_javascript, no reason to
            # include a bunch of tiles we will never render
            if no_javascript and functions[k]['name'] != deepest['name']:
                continue

            # create stack group
            #
            # note we conveniently don't need unique ids for each ctx/frame
            # tile, just for the entire stack group
            f.write('<g '
                    'id="s-%(name)s" '
                    'class="stack" '
                    '%(js)s>' % dict(
                        name=k,
                        js= 'visibility="%(visibility)s">' % dict(
                                    visibility="visible"
                                        if functions[k]['name']
                                            == deepest['name']
                                        else "hidden")
                                if not no_javascript else ''))

            # add a separator between code/stack
            f.write('<rect '
                    'x="%(x)d" '
                    'y="%(y)d" '
                    'width="%(width)d" '
                    'height="%(height)d" '
                    'fill="%(color)s">' % dict(
                        x=code.x + code.width,
                        y=code.y,
                        width=1,
                        height=max(
                                stacks[k].y + stacks[k].height
                                    if not no_frames else 0,
                                ctxs[k].y + ctxs[k].height
                                    if not no_ctx else 0)
                            - code.y - padding,
                        color='#7f7f7f' if dark else '#555555'))
            f.write('</rect>')

            # create ctx tiles
            if not no_ctx:
              for j, t in enumerate(ctxs[k].leaves()):
                # skip anything with zero weight/height after aligning things
                if t.width == 0 or t.height == 0:
                    continue

                f.write('<g '
                        'id="x-%(id)s" '
                        'class="tile ctx" '
                        'transform="translate(%(x)d,%(y)d)" '
                        '%(js)s>' % dict(
                            id='%s-%s' % (i, j),
                            x=t.x,
                            y=t.y,
                            js= 'data-name="%(name)s" '
                                'data-func="%(func)s" '
                                # precompute x/y for javascript, svg makes
                                # this weirdly difficult to figure out
                                # post-transform
                                'data-x="%(x)d" '
                                'data-y="%(y)d" '
                                'data-width="%(width)d" '
                                'data-height="%(height)d" '
                                'onmouseenter="enter_tile(this,event)" '
                                'onmouseleave="leave_tile(this,event)" '
                                'onclick="click_tile(this,event)">' % dict(
                                        name=t.attrs['name'],
                                        func=k,
                                        x=t.x,
                                        y=t.y,
                                        width=t.width,
                                        height=t.height)
                                    if not no_javascript else ''))
                # add an invisible rect to make things more clickable
                f.write('<rect '
                        'width="%(width)d" '
                        'height="%(height)d" '
                        'opacity="0">' % dict(
                            width=t.width + padding,
                            height=t.height + padding))
                f.write('</rect>')
                f.write('<title>')
                f.write(t.label)
                f.write('</title>')
                f.write('<rect '
                        'id="x-tile-%(id)s" '
                        'fill="%(color)s" '
                        'width="%(width)d" '
                        'height="%(height)d">' % dict(
                            id='%s-%s' % (i, j),
                            color=t.color,
                            width=t.width,
                            height=t.height))
                f.write('</rect>')
                if not no_label:
                    f.write('<clipPath id="x-clip-%s">' % ('%s-%s' % (i, j)))
                    f.write('<use href="#x-tile-%s">' % ('%s-%s' % (i, j)))
                    f.write('</use>')
                    f.write('</clipPath>')
                    f.write('<text clip-path="url(#x-clip-%s)">' % (
                            '%s-%s' % (i, j)))
                    for j, l in enumerate(t.label.split('\n')):
                        if j == 0:
                            f.write('<tspan x="3" y="1.1em">')
                            f.write(l)
                            f.write('</tspan>')
                        else:
                            if t.children:
                                f.write('<tspan dx="3" y="1.1em" '
                                        'fill-opacity="0.7">')
                                f.write(l)
                                f.write('</tspan>')
                            else:
                                f.write('<tspan x="3" dy="1.1em" '
                                        'fill-opacity="0.7">')
                                f.write(l)
                                f.write('</tspan>')
                    f.write('</text>')
                f.write('</g>')

            # add a separator between ctx/stack
            if not no_ctx and not no_frames:
              f.write('<rect '
                      'x="%(x)d" '
                      'y="%(y)d" '
                      'width="%(width)d" '
                      'height="%(height)d" '
                      'fill="%(color)s">' % dict(
                          x=ctxs[k].x,
                          y=ctxs[k].y + ctxs[k].height,
                          width=ctxs[k].width - padding,
                          height=1,
                          color='#7f7f7f' if dark else '#555555'))
              f.write('</rect>')

            # create stack tiles
            if not no_frames:
              for j, t in enumerate(stacks[k].leaves()):
                # skip anything with zero weight/height after aligning things
                if t.width == 0 or t.height == 0:
                    continue

                f.write('<g '
                        'id="f-%(id)s" '
                        'class="tile frame" '
                        'transform="translate(%(x)d,%(y)d)" '
                        '%(js)s>' % dict(
                            id='%s-%s' % (i, j),
                            x=t.x,
                            y=t.y,
                            js= 'data-name="%(name)s" '
                                'data-func="%(func)s" '
                                # precompute x/y for javascript, svg makes
                                # this weirdly difficult to figure out
                                # post-transform
                                'data-x="%(x)d" '
                                'data-y="%(y)d" '
                                'data-width="%(width)d" '
                                'data-height="%(height)d" '
                                'onmouseenter="enter_tile(this,event)" '
                                'onmouseleave="leave_tile(this,event)" '
                                'onclick="click_tile(this,event)"' % dict(
                                        name=t.attrs['name'],
                                        func=k,
                                        x=t.x,
                                        y=t.y,
                                        width=t.width,
                                        height=t.height)
                                    if not no_javascript else ''))
                # add an invisible rect to make things more clickable
                f.write('<rect '
                        'width="%(width)d" '
                        'height="%(height)d" '
                        'opacity="0">' % dict(
                            width=t.width + padding,
                            height=t.height + padding))
                f.write('</rect>')
                f.write('<title>')
                f.write(t.label)
                f.write('</title>')
                f.write('<rect '
                        'id="f-tile-%(id)s" '
                        'fill="%(color)s" '
                        'width="%(width)d" '
                        'height="%(height)d">' % dict(
                            id='%s-%s' % (i, j),
                            color=t.color,
                            width=t.width,
                            height=t.height))
                f.write('</rect>')
                if not no_label:
                    f.write('<clipPath id="f-clip-%s">' % ('%s-%s' % (i, j)))
                    f.write('<use href="#f-tile-%s">' % ('%s-%s' % (i, j)))
                    f.write('</use>')
                    f.write('</clipPath>')
                    f.write('<text clip-path="url(#f-clip-%s)">' % (
                            '%s-%s' % (i, j)))
                    for j, l in enumerate(t.label.split('\n')):
                        if j == 0:
                            f.write('<tspan x="3" y="1.1em">')
                            f.write(l)
                            f.write('</tspan>')
                        else:
                            if t.children:
                                f.write('<tspan dx="3" y="1.1em" '
                                        'fill-opacity="0.7">')
                                f.write(l)
                                f.write('</tspan>')
                            else:
                                f.write('<tspan x="3" dy="1.1em" '
                                        'fill-opacity="0.7">')
                                f.write(l)
                                f.write('</tspan>')
                    f.write('</text>')
                f.write('</g>')

            f.write('</g>')

        if not no_javascript:
            # arrowhead for arrows
            f.write('<defs>')
            f.write('<marker '
                    'id="arrowhead" '
                    'viewBox="0 0 10 10" '
                    'refX="10" '
                    'refY="5" '
                    'markerWidth="6" '
                    'markerHeight="6" '
                    'orient="auto-start-reverse" '
                    'fill="%(color)s">' % dict(
                        color='#000000' if dark else '#555555'))
            f.write('<path d="M 0 0 L 10 5 L 0 10 z"/>')
            f.write('</marker>')
            f.write('</defs>')

            # javascript for arrows
            #
            # why tf does svg support javascript?
            f.write('<script><![CDATA[')

            # embed our callgraph
            f.write('const children = %s;' % json.dumps(
                    {f['name']: sorted(f.get('children', []),
                            key=lambda c: functions[c]['limit'],
                            reverse=True)
                        for f in functions.values()},
                    separators=(',', ':')))

            # function for rect <-> line interesection
            f.write('function rect_intersect(x, y, width, height, l_x, l_y) {')
            f.write(    'let r_x = (x + width/2);')
            f.write(    'let r_y = (y + height/2);')
            f.write(    'let dx = l_x - r_x;')
            f.write(    'let dy = l_y - r_y;')
            f.write(    'let θ = Math.abs(dy / dx);')
            f.write(    'let φ = height / width;')
            f.write(    'if (θ > φ) {')
            f.write(        'return [')
            f.write(            'r_x + ((height/2)/θ)*Math.sign(dx),')
            f.write(            'r_y + (height/2)*Math.sign(dy),')
            f.write(        '];')
            f.write(    '} else {')
            f.write(        'return [')
            f.write(            'r_x + (width/2)*Math.sign(dx),')
            f.write(            'r_y + ((width/2)*θ)*Math.sign(dy),')
            f.write(        '];')
            f.write(    '}')
            f.write('}')

            # our main drawing functions
            f.write('function draw_unfocus() {')
                        # lower opacity of unfocused tiles
            f.write(    'for (let b of document.querySelectorAll(".tile")) {')
            f.write(        'b.setAttribute("fill-opacity", 0.5);')
            f.write(    '}')
            f.write('}')

            f.write('function draw_focus(a) {')
                        # revert opacity and move to top
            f.write(    'a.setAttribute("fill-opacity", 1);')
            f.write(    'a.parentElement.appendChild(a);')
            f.write('}')

            # draw an arrow
            f.write('function draw_arrow(a, b) {')
                        # no self-referential arrows
            f.write(    'if (b == a) {')
            f.write(        'return;')
            f.write(    '}')
                        # figure out rect intersections
            f.write(    'let svg = document.documentElement;')
            f.write(    'let ns = svg.getAttribute("xmlns");')
            f.write(    'let a_x = parseInt(a.dataset.x);')
            f.write(    'let a_y = parseInt(a.dataset.y);')
            f.write(    'let a_width = parseInt(a.dataset.width);')
            f.write(    'let a_height = parseInt(a.dataset.height);')
            f.write(    'let b_x = parseInt(b.dataset.x);')
            f.write(    'let b_y = parseInt(b.dataset.y);')
            f.write(    'let b_width = parseInt(b.dataset.width);')
            f.write(    'let b_height = parseInt(b.dataset.height);')
            f.write(    'let [a_ix, a_iy] = rect_intersect(')
            f.write(            'a_x, a_y, a_width, a_height,')
            f.write(            'b_x + b_width/2, b_y + b_height/2);')
            f.write(    'let [b_ix, b_iy] = rect_intersect(')
            f.write(            'b_x, b_y, b_width, b_height,')
            f.write(            'a_x + a_width/2, a_y + a_height/2);')
                        # create the actual arrow
            f.write(    'let arrow = document.createElementNS(ns, "line");')
            f.write(    'arrow.classList.add("arrow");')
            f.write(    'arrow.setAttribute("x1", a_ix);')
            f.write(    'arrow.setAttribute("y1", a_iy);')
            f.write(    'arrow.setAttribute("x2", b_ix);')
            f.write(    'arrow.setAttribute("y2", b_iy);')
            f.write(    'arrow.setAttribute("stroke", "%(color)s");' % dict(
                                color='#000000' if dark else '#555555'))
            f.write(    'arrow.setAttribute("marker-end", "url(#arrowhead)");')
            f.write(    'arrow.setAttribute("pointer-events", "none");')
            f.write(    'a.parentElement.appendChild(arrow);')
            f.write('}')

            # here are some drawing modes to choose from

            # draw full callgraph
            f.write('function draw_callgraph(a) {')
                        # track visited children to avoid cycles
            f.write(    'let seen = {};')
                        # create new arrows
            f.write(    'let recurse = function(a) {')
            f.write(        'if (a.dataset.name in seen) {')
            f.write(            'return;')
            f.write(        '}')
            f.write(        'seen[a.dataset.name] = true;')
            f.write(        'for (let child of ')
            f.write(                'children[a.dataset.name] || []) {')
            f.write(            'let b = document.getElementById("c-"+child);')
            f.write(            'if (b) {')
            f.write(                'draw_arrow(a, b);')
            f.write(                'recurse(b);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '};')
            f.write(    'recurse(a);')
                        # track visited children to avoid cycles
            f.write(    'seen = {};')
                        # move in-focus tiles to the top
            f.write(    'recurse = function(a) {')
            f.write(        'if (a.dataset.name in seen) {')
            f.write(            'return;')
            f.write(        '}')
            f.write(        'seen[a.dataset.name] = true;')
            f.write(        'for (let child of ')
            f.write(                'children[a.dataset.name] || []) {')
            f.write(            'let b = document.getElementById("c-"+child);')
            f.write(            'if (b) {')
            f.write(                'draw_focus(b);')
            f.write(                'recurse(b);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '};')
            f.write(    'recurse(a);')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # draw deepest set of calls
            f.write('function draw_deepest(a) {')
                        # track visited children to avoid cycles
            f.write(    'let seen = {};')
                        # create/ new arrows
            f.write(    'let recurse = function(a) {')
            f.write(        'if (a.dataset.name in seen) {')
            f.write(            'return;')
            f.write(        '}')
            f.write(        'seen[a.dataset.name] = true;')
            f.write(        'if (children[a.dataset.name]) {')
                                # draw recursive arrows to show cycles
            f.write(            'if (children[a.dataset.name][0] in seen) {')
            f.write(                'let child = children[a.dataset.name][0];')
            f.write(                'let b = document.getElementById('
                                            '"c-"+child);')
            f.write(                'if (b) {')
            f.write(                    'draw_arrow(a, b);')
            f.write(                '}')
            f.write(            '}')
                                # but descend down the deepest non-recursive
                                # child to show useful stack info
            f.write(            'let child = children[a.dataset.name]'
                                        '.find((child) => !(child in seen));')
            f.write(            'if (child) {')
            f.write(                'let b = document.getElementById('
                                            '"c-"+child);')
            f.write(                'if (b) {')
            f.write(                    'draw_arrow(a, b);')
            f.write(                    'recurse(b);')
            f.write(                '}')
            f.write(            '}')
            f.write(        '}')
            f.write(    '};')
            f.write(    'recurse(a);')
                        # track visited children to avoid cycles
            f.write(    'seen = {};')
                        # move in-focus tiles to the top
            f.write(    'recurse = function(a) {')
            f.write(        'if (a.dataset.name in seen) {')
            f.write(            'return;')
            f.write(        '}')
            f.write(        'seen[a.dataset.name] = true;')
            f.write(        'if (children[a.dataset.name]) {')
            f.write(            'let child = children[a.dataset.name]'
                                        '.find((child) => !(child in seen));')
            f.write(            'if (child) {')
            f.write(                'let b = document.getElementById('
                                            '"c-"+child);')
            f.write(                'if (b) {')
            f.write(                    'draw_focus(b);')
            f.write(                    'recurse(b);')
            f.write(                '}')
            f.write(            '}')
            f.write(        '}')
            f.write(    '};')
            f.write(    'recurse(a);')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # draw one level of calls
            f.write('function draw_callees(a) {')
                        # create new arrows
            f.write(    'for (let child of children[a.dataset.name] || []) {')
            f.write(        'let b = document.getElementById("c-"+child);')
            f.write(        'if (b) {')
            f.write(            'draw_arrow(a, b);')
            f.write(        '}')
            f.write(    '}')
                        # move in-focus tiles to the top
            f.write(    'for (let child of children[a.dataset.name] || []) {')
            f.write(        'let b = document.getElementById("c-"+child);')
            f.write(        'if (b) {')
            f.write(            'draw_focus(b);')
            f.write(        '}')
            f.write(    '}')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # draw one level of callers
            f.write('function draw_callers(a) {')
                        # create new arrows
            f.write(    'for (let parent in children) {')
            f.write(        'if ((children[parent] || []).includes(')
            f.write(                'a.dataset.name)) {')
            f.write(            'let b = document.getElementById('
                                        '"c-"+parent);')
            f.write(            'if (b) {')
            f.write(                'draw_arrow(b, a);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # move in-focus tiles to the top
            f.write(    'for (let parent in children) {')
            f.write(        'if ((children[parent] || []).includes(')
            f.write(                'a.dataset.name)) {')
            f.write(            'let b = document.getElementById('
                                        '"c-"+parent);')
            f.write(            'if (b) {')
            f.write(                'draw_focus(b);')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # clear old arrows/tiles if we leave
            f.write('function undraw() {')
                        # clear arrows
            f.write(    'for (let arrow of document.querySelectorAll('
                                '".arrow")) {')
            f.write(        'arrow.remove();')
            f.write(    '}')
                        # revert opacity
            f.write(    'for (let b of document.querySelectorAll(".tile")) {')
            f.write(        'b.setAttribute("fill-opacity", 1);')
            f.write(    '}')
            f.write('}')

            # render stack+ctx tiles
            f.write('function switch_stack(name) {')
                        # update stack tiles
            f.write(    'for (let b of document.querySelectorAll(".stack")) {')
            f.write(        'b.setAttribute("visibility", "hidden");')
            f.write(    '}')
            f.write(    'let s = document.getElementById("s-"+name);')
            f.write(    'if (s) {')
                            # make visible
            f.write(        's.setAttribute("visibility", "visible");')
                            # and refocus stack tiles in case they were
                            # unfocused
            f.write(        'for (let b of s.querySelectorAll('
                                    '".frame,.ctx")) {')
            f.write(            'draw_focus(b);')
            f.write(        '}')
            f.write(    '}')
            f.write('}')

            # draw stack frame/ctx tile
            f.write('function draw_stack(a) {')
                        # if a is null we just refocus the stack
            f.write(    'if (!a) {')
            f.write(        'let s = document.querySelector('
                                    '".stack[visibility=\\"visible\\"]");')
            f.write(        'for (let b of s.querySelectorAll('
                                    '".frame,.ctx")) {')
            f.write(            'draw_focus(b);')
            f.write(        '}')
            f.write(        'return;')
            f.write(    '}')

                        # render the deepest call path of the relevant code
                        # tile
            f.write(    'let c = document.getElementById("c-"+('
                                'a.classList.contains("ctx")'
                                    '? a.dataset.func'
                                    ': a.dataset.name));')
            f.write(    'if (c) {')
            f.write(        'draw_deepest(c);')
            f.write(    '}')
                        # focus all tiles beneath this one, bit of a hack to
                        # avoid another recursive function, yes this includes
                        # all ctxs if any ctx is in focus
            f.write(    'let y = parseInt(a.dataset.y);')
            f.write(    'for (b of a.parentElement'
                                '.querySelectorAll(".frame,.ctx")) {')
            f.write(        'if (parseInt(b.dataset.y) >= y) {')
            f.write(            'draw_focus(b);')
            f.write(        '}')
            f.write(    '}')
                        # move our tile to the top
            f.write(    'draw_focus(a);')
            f.write('}')

            # state machine for mouseover/clicks
            f.write('const modes = [')
            if mode_callgraph:
                f.write('{name: "callgraph",    draw: draw_callgraph},')
            if mode_deepest:
                f.write('{name: "deepest",      draw: draw_deepest  },')
            if mode_callees:
                f.write('{name: "callees",      draw: draw_callees  },')
            if mode_callers:
                f.write('{name: "callers",      draw: draw_callers  },')
            f.write('];')
            f.write('let state = 0;')
            f.write('let hovered = null;')
            f.write('let active_code = null;')
            f.write('let active_stack = null;')
            f.write('let paused = false;')

            f.write('function enter_tile(a, event) {')
            f.write(    'hovered = a;')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')

                        # code tile or stack tile?
            f.write(    'if (a.classList.contains("code")) {')
            f.write(        'if (!active_code && !active_stack) {')
                                # reset
            f.write(            'undraw();')
            f.write(            'draw_unfocus();')
                                # draw selected mode
            f.write(            'modes[state].draw(a);')
            if not no_stack:
                                # render relevant stack tiles
                f.write(        'switch_stack(a.dataset.name);')
            f.write(        '}')
            f.write(    '} else if (a.classList.contains("frame") '
                                '|| a.classList.contains("ctx")) {')
            f.write(        'if (!active_stack) {')
                                # reset
            f.write(            'undraw();')
            f.write(            'draw_unfocus();')
            if not no_stack:
                                # draw stack mode
                f.write(        'draw_stack(a);')
            f.write(        '}')
            f.write(    '}')
            f.write('}')

            f.write('function leave_tile(a, event) {')
            f.write(    'hovered = null;')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')

                        # do nothing if ctrl is held
            f.write(    'if (!active_stack) {')
                            # reset
            f.write(        'undraw();')
            f.write(        'if (!active_code) {')
            if not no_stack:
                                # reset to deepest stack
                f.write(        'switch_stack("%s");' % deepest['name'])
            f.write(        '} else {')
                                # reset to active code
            f.write(            'draw_unfocus();')
            f.write(            'modes[state].draw(active_code);')
            if not no_stack:
                f.write(        'draw_stack();')
            f.write(        '}')
            f.write(    '}')
            f.write('}')

            # update the mode string
            f.write('function draw_mode() {')
            f.write(    'let mode = document.getElementById("mode");')
            f.write(    'if (mode) {')
            f.write(        'mode.textContent = "mode: "'
                                    '+ modes[state].name'
                                    '+ ((paused) ? " (paused)"'
                                        ': (active_code || active_stack)'
                                            '? " (frozen)"'
                                        ': "");')
            f.write(    '}')
            f.write('}')

            # redraw things
            f.write('function redraw() {')
                        # reset
            f.write(    'undraw();')
                        # redraw stack if active
            f.write(    'if (active_stack) {')
            f.write(        'draw_unfocus();')
            if not no_stack:
                f.write(    'draw_stack(active_stack);')
                        # redraw code if active
            f.write(    '} else if (active_code) {')
            f.write(        'draw_unfocus();')
            f.write(        'modes[state].draw(active_code);')
            if not no_stack:
                f.write(    'draw_stack();')
                        # otherwise try to enter hovered tile if there is one
            f.write(    '} else if (hovered) {')
            f.write(        'enter_tile(hovered);')
            f.write(    '}')
            f.write('}')

            # clicking the mode element changes the mode
            f.write('function click_header(a, event) {')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')
                        # update state
            f.write(    'state = (state + 1) % modes.length;')
                        # update the mode string
            f.write(    'draw_mode();')
                        # redraw with new mode
            f.write(    'redraw();')
            f.write('}')

            # click handler is kinda complicated, we handle both single
            # and double clicks here
            f.write('let prev_code = null;')
            f.write('let prev_stack = null;')
            f.write('function click_tile(a, event) {')
                        # do nothing if paused
            f.write(    'if (paused) {')
            f.write(        'return;')
            f.write(    '}')

                        # double clicking changes the mode
            f.write(    'if (event && event.detail == 2 '
                                # limit this to double-clicking the active tile
                                '&& ((!prev_code && !prev_stack)'
                                    '|| (a == prev_code && !prev_stack)'
                                    '|| a == prev_stack)) {')
                            # undo single-click
            f.write(        'active_code = prev_code;')
            f.write(        'active_stack = prev_stack;')
                            # trigger a mode change if double-clicking code,
                            # note we could also change stack modes here if we
                            # had more than one
            f.write(        'if (a.classList.contains("code")) {')
            f.write(            'click_header();')
            f.write(        '}')
            f.write(        'return;')
            f.write(    '}')
                        # save state in case we are trying to double click,
                        # double clicks always send a single click first
            f.write(    'prev_code = active_code;')
            f.write(    'prev_stack = active_stack;')

                        # clicking tiles toggles frozen mode
            f.write(    'if (a.classList.contains("code")) {')
            f.write(        'if (a == active_code && !active_stack) {')
            f.write(            'active_code = null;')
            f.write(        '} else {')
            f.write(            'active_code = null;')
            f.write(            'active_stack = null;')
            f.write(            'enter_tile(a);')
            f.write(            'active_code = a;')
            f.write(        '}')
            f.write(    '} else if (a.classList.contains("frame")'
                                '|| a.classList.contains("ctx")) {')
            f.write(        'if (a == active_stack) {')
            f.write(            'active_stack = null;')
            f.write(        '} else {')
            f.write(            'active_stack = null;')
            f.write(            'enter_tile(a);')
            f.write(            'active_stack = a;')
            f.write(        '}')
            f.write(    '}')
                        # update mode string
            f.write(    'draw_mode();')
            f.write('}')

            # include some minor keybindings
            f.write('function keydown(event) {')
                        # m => change mode
            f.write(    'if (event.key == "m") {')
            f.write(        'click_header();')
                        # escape/e => clear frozen/paused state
            f.write(    '} else if (event.key == "Escape"'
                                '|| event.key == "e") {')
                            # reset frozen state
            f.write(        'active_code = null;')
            f.write(        'active_stack = null;')
                            # reset paused state
            f.write(        'if (paused) {')
            f.write(            'keydown({key: "Pause"});')
            f.write(        '}')
                            # redraw things
            f.write(        'draw_mode();')
            f.write(        'redraw();')
                        # pause/p  => pause all interactivity and allow
                        # copy-paste
            f.write(    '} else if (event.key == "Pause"'
                                '|| event.key == "p") {')
            f.write(        'paused = !paused;')
                            # update mode string
            f.write(        'draw_mode();')
            f.write(        'if (paused) {')
                                # enabled copy-pasting when paused
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[style*=\\"user-select\\"]")) {')
            f.write(                'e.style["user-select"] = "auto";')
            f.write(            '}')
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[cursor]")) {')
            f.write(                'e.setAttribute("cursor", "auto");')
            f.write(            '}')
            f.write(        '} else {')
                                # reset copy-pasting
            f.write(            'document.getSelection().empty();')
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[style*=\\"user-select\\"]")) {')
            f.write(                'e.style["user-select"] = "none";')
            f.write(            '}')
            f.write(            'for (let e of document.querySelectorAll('
                                        '"[cursor]")) {')
            f.write(                'e.setAttribute("cursor", "pointer");')
            f.write(            '}')
            f.write(        '}')
            f.write(    '}')
            f.write('}')
            f.write('window.addEventListener("keydown", keydown);')

            f.write(']]></script>')

        f.write('</svg>')


    # print some summary info
    if not quiet:
        stat = code.stat()
        print('updated %s, code %d stack %s ctx %d' % (
                    output,
                    totals.get('code', 0),
                    (lambda s: '∞' if mt.isinf(s) else s)(
                        totals.get('stack', 0)),
                    totals.get('ctx', 0)))

    if (args.get('error_on_recursion')
            and mt.isinf(totals.get('stack', 0))):
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Render code info as an interactive d3-esque treemap.",
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
            '-o', '--output',
            required=True,
            help="Output *.svg file.")
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
            '-q', '--quiet',
            action='store_true',
            help="Don't print info.")
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
            '-W', '--width',
            type=lambda x: int(x, 0),
            help="Width in pixels. Defaults to %r." % WIDTH)
    parser.add_argument(
            '-H', '--height',
            type=lambda x: int(x, 0),
            help="Height in pixels. Defaults to %r." % HEIGHT)
    parser.add_argument(
            '--no-header',
            action='store_true',
            help="Don't show the header.")
    parser.add_argument(
            '--no-mode',
            action='store_true',
            help="Don't show the mode state.")
    parser.add_argument(
            '--no-stack',
            action='store_true',
            help="Don't render any stack info.")
    parser.add_argument(
            '-S', '--stack-ratio',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            help="Ratio of width to use for stack info. Defaults to 1:5.")
    parser.add_argument(
            '--no-ctx',
            action='store_true',
            help="Don't render function context.")
    parser.add_argument(
            '--no-frames',
            action='store_true',
            help="Don't render function stack frame info.")
    parser.add_argument(
            '-J', '--no-javascript',
            action='store_true',
            help="Don't add javascript for interactability.")
    parser.add_argument(
            '--mode-callgraph',
            action='store_true',
            help="Include the callgraph rendering mode.")
    parser.add_argument(
            '--mode-deepest',
            action='store_true',
            help="Include the deepest rendering mode.")
    parser.add_argument(
            '--mode-callees',
            action='store_true',
            help="Include the callees rendering mode.")
    parser.add_argument(
            '--mode-callers',
            action='store_true',
            help="Include the callers rendering mode.")
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
            help="Specify an explicit aspect ratio for the squarify "
                "algorithm. Implies --squarify.")
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
            '--to-ratio',
            type=lambda x: (
                (lambda a, b: a / b)(*(float(v) for v in x.split(':', 1)))
                    if ':' in x else float(x)),
            help="Aspect ratio to use with --to-scale. Defaults to 1:1.")
    parser.add_argument(
            '-t', '--tiny',
            action='store_true',
            help="Tiny mode, alias for --to-scale=1, --no-header, "
                "--no-label, --no-stack, and --no-javascript.")
    parser.add_argument(
            '--title',
            help="Add a title. Accepts %% modifiers.")
    parser.add_argument(
            '--padding',
            type=float,
            help="Padding to add to each level of the treemap. Defaults to 1.")
    parser.add_argument(
            '--no-label',
            action='store_true',
            help="Don't render any labels.")
    parser.add_argument(
            '--dark',
            action='store_true',
            help="Use the dark style.")
    parser.add_argument(
            '--font',
            type=lambda x: [x.strip() for x in x.split(',')],
            help="Font family to use.")
    parser.add_argument(
            '--font-size',
            help="Font size to use. Defaults to %r." % FONT_SIZE)
    parser.add_argument(
            '--background',
            help="Background color to use. Note #00000000 can make the "
                "background transparent.")
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
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
