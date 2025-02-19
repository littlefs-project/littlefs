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
import itertools as it
import math as mt
import re
import shutil


# some nicer colors borrowed from Seaborn
# note these include a non-opaque alpha
COLORS = [
    '#4c72b0bf', # blue
    '#dd8452bf', # orange
    '#55a868bf', # green
    '#c44e52bf', # red
    '#8172b3bf', # purple
    '#937860bf', # brown
    '#da8bc3bf', # pink
    '#8c8c8cbf', # gray
    '#ccb974bf', # yellow
    '#64b5cdbf', # cyan
]
COLORS_DARK = [
    '#a1c9f4bf', # blue
    '#ffb482bf', # orange
    '#8de5a1bf', # green
    '#ff9f9bbf', # red
    '#d0bbffbf', # purple
    '#debb9bbf', # brown
    '#fab0e4bf', # pink
    '#cfcfcfbf', # gray
    '#fffea3bf', # yellow
    '#b9f2f0bf', # cyan
]

WIDTH = 750
HEIGHT = 350
FONT = ['sans-serif']
FONT_SIZE = 10


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

# parse different data representations
def dat(x):
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
        # just don't allow infinity or nan
        if mt.isinf(x) or mt.isnan(x):
            raise ValueError("invalid dat %r" % x)
    except ValueError:
        pass

    # else give up
    raise ValueError("invalid dat %r" % x)

def try_dat(x):
    try:
        return dat(x)
    except ValueError:
        return None

def collect(csv_paths, defines=[]):
    # collect results from CSV files
    fields = []
    results = []
    for path in csv_paths:
        try:
            with openio(path) as f:
                reader = csv.DictReader(f, restval='')
                fields.extend(
                        k for k in reader.fieldnames
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
    def __init__(self, attrs, *,
            defaults=None):
        # include defaults?
        if (defaults is not None
                and not any(
                    not isinstance(attr, tuple)
                        or attr[0] in {None, (), ('',)}
                    for attr in (attrs or []))):
            attrs = defaults + (attrs or [])

        # normalize
        self.attrs = []
        self.keyed = co.OrderedDict()
        for attr in (attrs or []):
            if not isinstance(attr, tuple):
                attr = ((), attr)
            elif attr[0] in {None, (), ('',)}:
                attr = ((), attr[1])

            self.attrs.append(attr)
            if attr[0] not in self.keyed:
                self.keyed[attr[0]] = []
            self.keyed[attr[0]].append(attr[1])

    def __repr__(self):
        return 'Attr(%r)' % [
                (','.join(attr[0]), attr[1])
                for attr in self.attrs]

    def __iter__(self):
        return it.cycle(self.keyed[()])

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

        # try to lookup by key
        best = None
        for ks, vs in self.keyed.items():
            prefix = []
            for j, k in enumerate(ks):
                if j < len(key) and (not k or key[j] == k):
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

        return None

    def __contains__(self, key):
        return self.__getitem__(key) is not None

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
                '(?P<format>[+\- #0-9\.]*[scdboxXfFeEgG])')
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
            if m.group('format')[-1] in 'dboxXfFeEgG':
                if isinstance(v, str):
                    v = try_dat(v) or 0
            else:
                if not isinstance(v, str):
                    v = str(v)
            # note we need Python's new format syntax for binary
            f = '{:%s}' % m.group('format')
            return f.format(v)
        else: assert False
    return re.sub(pattern, unescape, s)



# a type to represent tiles
class Tile:
    def __init__(self, key, children,
            x=None, y=None, width=None, height=None, *,
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
        return 'Tile(%r, %r, %r, %r, %r, %r)' % (
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

    # recursive align to int boundaries
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
    ratio = max(bdiv(aspect_ratio[0], aspect_ratio[1]),
            bdiv(aspect_ratio[1], aspect_ratio[0]))

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


def main(csv_paths, output, *,
        quiet=False,
        by=None,
        fields=None,
        defines=[],
        labels=None,
        colors=None,
        width=None,
        height=None,
        no_header=False,
        to_scale=None,
        aspect_ratio=(1,1),
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
        to_scale = True
        no_header = True
        no_label = True

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

    # first collect results from CSV files
    fields_, results = collect(csv_paths, defines)

    if not by and not fields:
        print("error: needs --by or --fields to figure out fields",
                file=sys.stderr)
        sys.exit(-1)

    # if by not specified, guess it's anything not in fields/labels/defines
    if not by:
        by = [k for k in fields_
                if k not in (fields or [])
                    and k not in (labels or [])
                    and not any(k == k_ for k_, _ in defines)]

    # if fields not specified, guess it's anything not in by/labels/defines
    if not fields:
        fields = [k for k in fields_
                if k not in (by or [])
                    and k not in (labels or [])
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
            t_.color = colors_[i, t_.key]

    # and labels everywhere
    for i, t in enumerate(tile.tiles()):
        if (i, t.key) in labels_:
            t.label = punescape(labels_[i, t.key], t.attrs)

    # scale width/height if requested now that we have our data
    if to_scale and (width is None or height is None) and tile.value != 0:
        # scale width only
        if height is not None:
            width_ = mt.ceil((tile.value * to_scale) / height_)
        # scale height only
        elif width is not None:
            height_ = mt.ceil((tile.value * to_scale) / width_)
        # scale based on aspect-ratio
        else:
            width_ = mt.ceil(mt.sqrt(tile.value * to_scale)
                    * (aspect_ratio[0] / aspect_ratio[1]))
            height_ = mt.ceil((tile.value * to_scale) / width_)

    # sort
    tile.sort()

    # recursively partition tiles
    tile.x = 0
    tile.y = 0
    tile.width = width_
    tile.height = height_
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

            # create space for header
            if title is not None or not no_header:
                y__ += mt.ceil(FONT_SIZE * 1.3)
                height__ -= min(mt.ceil(FONT_SIZE * 1.3), height__)

        else:
            # apply top padding
            if nested and tile.depth != 1:
                tile.x += padding
                tile.y += padding
                tile.width  -= min(padding, tile.width)
                tile.height -= min(padding, tile.height)
            # apply bottom padding
            if nested or not tile.children:
                tile.width  -= min(padding, tile.width)
                tile.height -= min(padding, tile.height)

            x__ = tile.x
            y__ = tile.y
            width__ = tile.width
            height__ = tile.height

            # create space for names and junk
            if nested:
                y__ += mt.ceil(FONT_SIZE * 1.3)
                height__ -= min(mt.ceil(FONT_SIZE * 1.3), height__)

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
            elif args.get('squarify'):
                partition_squarify(tile.children, tile.value,
                        x__, y__, width__, height__)
            elif args.get('rectify'):
                partition_squarify(tile.children, tile.value,
                        x__, y__, width__, height__,
                        aspect_ratio=(width_, height_))
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

    # create svg file
    with openio(output, 'w') as f:
        def writeln(s=''):
            f.write(s)
            f.write('\n')
        f.writeln = writeln

        # yes this is svg
        f.write('<svg '
                'viewBox="0,0,%(width)d,%(height)d" '
                'width="%(width)d" '
                'height="%(height)d" '
                'style="max-width: 100%%; '
                    'height: auto; '
                    'font: %(font_size)dpx %(font)s; '
                    'background-color: %(background)s;" '
                'xmlns="http://www.w3.org/2000/svg">' % dict(
                    width=width_,
                    height=height_,
                    font=','.join(font),
                    font_size=font_size,
                    background=background_))

        # create header
        if title is not None or not no_header:
            f.write('<text fill="%(color)s">' % dict(
                    color='#ffffff' if dark else '#000000'))
            if not no_header:
                stat = tile.stat()
            if title:
                f.write('<tspan x="3" y="1.1em">')
                f.write(punescape(title, tile.attrs))
                f.write('</tspan>')
                if not no_header:
                    f.write('<tspan x="%(x)d" y="1.1em" '
                            'text-anchor="end">' % dict(
                                x=tile.width-3))
                    f.write('total %d, avg %d +-%dσ, min %d, max %d' % (
                            stat['total'],
                            stat['mean'], stat['stddev'],
                            stat['min'], stat['max']))
                    f.write('</tspan>')
            else:
                f.write('<tspan x="3" y="1.1em">')
                f.write('total %d, avg %d +-%dσ, min %d, max %d' % (
                        stat['total'],
                        stat['mean'], stat['stddev'],
                        stat['min'], stat['max']))
                f.write('</tspan>')
            f.write('</text>')

        # create tiles
        for i, t in enumerate(tile.tiles() if nested else tile.leaves()):
            # skip the top tile
            if t.depth == 0:
                continue
            # skip anything with zero weight/height after aligning things
            if t.width == 0 or t.height == 0:
                continue

            if t.label is not None:
                label__ = t.label
            else:
                label__ = '%s\n%d' % (','.join(t.key), t.value)

            f.write('<g transform="translate(%d,%d)">' % (t.x, t.y))
            f.write('<title>')
            f.write(label__)
            f.write('</title>')
            f.write('<rect '
                    'id="tile-%(id)s" '
                    'fill="%(color)s" '
                    'width="%(width)d" '
                    'height="%(height)d">' % dict(
                        id=i,
                        color=t.color,
                        width=t.width,
                        height=t.height))
            f.write('</rect>')
            if not no_label:
                f.write('<clipPath id="clip-%s">' % i)
                f.write('<use href="#tile-%s">' % i)
                f.write('</use>')
                f.write('</clipPath>')
                f.write('<text clip-path="url(#clip-%s)">' % i)
                for j, l in enumerate(label__.split('\n')):
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

        f.write('</svg>')


    # print some summary info
    if not quiet:
        stat = tile.stat()
        print('updated %s, total %d, avg %d +-%dσ, min %d, max %d' % (
                output, stat['total'],
                stat['mean'], stat['stddev'],
                stat['min'], stat['max']))


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Render CSV files as a treemap to a d3-esque svg.",
            allow_abbrev=False)
    parser.add_argument(
            'csv_paths',
            nargs='*',
            help="Input *.csv files.")
    parser.add_argument(
            '-o', '--output',
            required=True,
            help="Output *.svg file.")
    parser.add_argument(
            '-q', '--quiet',
            action='store_true',
            help="Don't print info.")
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
                "where a group is the comma-separated 'by' fields.")
    parser.add_argument(
            '-W', '--width',
            type=lambda x: int(x, 0),
            help="Width in pixels. Defaults to %r." % WIDTH)
    parser.add_argument(
            '-H', '--height',
            type=lambda x: int(x, 0),
            help="Height in pixels. Defaults to %r." % HEIGHT)
    parser.add_argument(
            '-N', '--no-header',
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
            '--to-scale',
            nargs='?',
            type=float,
            const=1,
            help="Scale the resulting treemap such that 1 pixel ~= 1/scale "
                "units. Defaults to scale=1. ")
    parser.add_argument(
            '-R', '--aspect-ratio',
            type=lambda x: tuple(float(v) for v in x.split(':', 1)),
            default=(1, 1),
            help="Aspect ratio to use with --to-scale. Defaults to 1:1.")
    parser.add_argument(
            '-t', '--tiny',
            action='store_true',
            help="Tiny mode, alias for --to-scale=1, --no-header, and "
                "--no-label.")
    parser.add_argument(
            '-r', '--nested',
            action='store_true',
            help="Show nested tiles.")
    parser.add_argument(
            '--title',
            help="Add a title.")
    parser.add_argument(
            '--padding',
            type=float,
            default=1,
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
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
