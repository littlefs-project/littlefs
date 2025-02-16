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
import shutil


# we don't actually need that many chars/colors thanks to the
# 4-colorability of all 2d maps
CHARS = ['.']
COLORS = [34, 31, 32, 35, 33, 36]

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

def fold(results, by=None, fields=None, labels=None, defines=[]):
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

    # collect dataset
    datasets = co.OrderedDict()
    labels_ = co.OrderedDict()
    for key in (keys if by else [()]):
        for field in fields:
            # organize by 'by' and field
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

                # hide 'field' if there is only one field
                key_ = key
                if len(fields or []) > 1 or not key_:
                    key_ += (field,)
                # do _not_ sum v here, it's tempting but risks
                # incorrect and misleading results
                datasets[key_] = v

                # also find label?
                if labels is not None:
                    for label_ in labels:
                        if label_ not in r:
                            continue
                        labels_[key_] = r[label_]

    return datasets, labels_


# a little ascii renderer
class Canvas:
    def __init__(self, width, height, *,
            color=False,
            dots=False,
            braille=False):
        # scale if we're printing with dots or braille
        if braille:
            self.width = 2*width
            self.height = 4*height
        elif dots:
            self.width = width
            self.height = 2*height
        else:
            self.width = width
            self.height = height
        self.color_ = color
        self.dots = dots
        self.braille = braille

        # create initial canvas
        self.grid = [False] * (self.width*self.height)
        self.colors = [''] * (self.width*self.height)

    def __getitem__(self, xy):
        x, y = xy
        # ignore out of bounds
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return

        return self.grid[x + y*self.width]

    def __setitem__(self, xy, char):
        x, y = xy
        # ignore out of bounds
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return

        self.grid[x + y*self.width] = char

    def color(self, x, y, color=None):
        # ignore out of bounds
        if x < 0 or y < 0 or x >= self.width or y >= self.height:
            return

        if color is not None:
            self.colors[x + y*self.width] = color
        else:
            return self.colors[x + y*self.width]

    def point(self, x, y, *,
            char=True,
            color=''):
        # scale if needed
        if self.braille and char is not True and char is not False:
            xscale, yscale = 2, 4
        elif self.dots and char is not True and char is not False:
            xscale, yscale = 1, 2
        else:
            xscale, yscale = 1, 1

        for i in range(xscale*yscale):
            x_ = x-(x%xscale) + (xscale-1-(i%xscale))
            y_ = y-(y%yscale) + (i//xscale)

            self[x_, y_] = char
            self.color(x_, y_, color)

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
            self.point(x1, y1, color=color, char=char)
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

        self.point(x2, y2, color=color, char=char)

    def rect(self, x, y, w, h, *,
            char=True,
            color=''):
        for j in range(h):
            for i in range(w):
                self.point(x+i, y+j, char=char, color=color)

    def label(self, x, y, label, *,
            color=''):
        # scale if needed
        if self.braille:
            xscale, yscale = 2, 4
        elif self.dots:
            xscale, yscale = 1, 2
        else:
            xscale, yscale = 1, 1

        for i, char in enumerate(label):
            self.point(x+i*xscale, y, char=char, color=color)

    def draw(self, row):
        # scale if needed
        if self.braille:
            xscale, yscale = 2, 4
        elif self.dots:
            xscale, yscale = 1, 2
        else:
            xscale, yscale = 1, 1

        y = self.height//yscale-1 - row
        row_ = []
        for x in range(self.width//xscale):
            color = ''
            char = False
            byte = 0
            for i in range(xscale*yscale):
                x_ = x*xscale + (xscale-1-(i%xscale))
                y_ = y*yscale + (i//xscale)

                # calculate char
                char_ = self[x_, y_]
                if char_:
                    byte |= 1 << i
                    if char_ is not True and char_ is not False:
                        char = char_

                # keep track of best color
                color_ = self.color(x_, y_)
                if color_:
                    color = color_

            # figure out winning char
            if byte:
                if char is not True and char is not False:
                    pass
                elif self.braille:
                    char = CHARS_BRAILLE[byte]
                else:
                    char = CHARS_DOTS[byte]
            else:
                char = ' '

            # color?
            if byte and self.color_ and color:
                char = '\x1b[%sm%s\x1b[m' % (color, char)

            row_.append(char)

        return ''.join(row_)


# a type to represent tiles
class Tile:
    def __init__(self, key, children,
            x=None, y=None, width=None, height=None, *,
            label=None,
            char=None,
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
        self.label = label
        self.char = char
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

        return Tile(prefix, tiles_)

    def __lt__(self, other):
        return self.value < other.value

    # sort recursively
    def sort(self):
        self.children.sort(reverse=True)
        for t in self.children:
            t.sort()

    def tiles(self):
        yield self
        for child in self.children:
            yield from child.tiles()

    def leaves(self):
        for t in self.tiles():
            if not t.children:
                yield t


# our parititioning schemes

def partition_binary(tile, *,
        partition=None):
    partition = partition or partition_binary

    sums = [0]
    for t in tile.children:
        sums.append(sums[-1] + t.value)

    # recursively partition into a roughly weight-balanced binary tree
    def partition_(i, j, value, x, y, width, height):
        # no child? guess we're done
        if i == j:
            return
        # single child? assign the partition
        elif i == j-1:
            tile.children[i].x = x
            tile.children[i].y = y
            tile.children[i].width = width
            tile.children[i].height = height
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
            dx = ((sums[k] - sums[i]) / value) * width
            partition_(i, k, l, x, y, dx, height)
            partition_(k, j, r, x+dx, y, width-dx, height)

        # split vertically?
        else:
            dy = ((sums[k] - sums[i]) / value) * height
            partition_(i, k, l, x, y, width, dy)
            partition_(k, j, r, x, y+dy, width, height-dy)

    partition_(0, len(tile.children), tile.value,
            tile.x, tile.y, tile.width, tile.height)

    # recursively partition
    for t in tile.children:
        partition(t, partition=partition)

def partition_slice(tile, *,
        partition=None):
    partition = partition or partition_slice

    # give each child a slice
    x_ = tile.x
    for t in tile.children:
        t.x = x_
        t.y = tile.y
        t.width = (t.value / tile.value) * tile.width
        t.height = tile.height

        x_ += t.width

    # recursively partition
    for t in tile.children:
        partition(t, partition=partition)

def partition_dice(tile, *,
        partition=None):
    partition = partition or partition_dice

    # give each child a slice
    y_ = tile.y
    for t in tile.children:
        t.x = tile.x
        t.y = y_
        t.width = tile.width
        t.height = (t.value / tile.value) * tile.height

        y_ += t.height

    # recursively partition
    for t in tile.children:
        partition(t, partition=partition)

def partition_slice_and_dice(tile, *,
        partition=None):
    partition_slice(tile, partition=partition or partition_dice_and_slice)

def partition_dice_and_slice(tile, *,
        partition=None):
    partition_dice(tile, partition=partition or partition_slice_and_dice)

def partition_squarify(tile, *,
        partition=None, aspect_ratio=None):
    partition = partition or partition_squarify
    # derive target aspect ratio from top-level tile
    aspect_ratio = aspect_ratio or (tile.width, tile.height)

    # this algorithm is described here:
    # https://www.win.tue.nl/~vanwijk/stm.pdf
    i = 0
    x = tile.x
    y = tile.y
    value = tile.value
    width = tile.width
    height = tile.height
    # this is our target aspect ratio, note we don't really care
    # about width vs height until actually slicing
    ratio = aspect_ratio[0] / aspect_ratio[1]

    while i < len(tile.children):
        # calculate initial aspect ratio
        sum_ = tile.children[i].value
        min_ = tile.children[i].value
        max_ = tile.children[i].value
        w = value * (ratio / max(width/height, height/width))
        ratio_ = max((max_*w)/(sum_**2), (sum_**2)/(min_*w))

        # keep adding children to this row/col until it starts to hurt
        # our aspect ratio
        j = i + 1
        while j < len(tile.children):
            sum__ = sum_ + tile.children[j].value
            min__ = min(min_, tile.children[j].value)
            max__ = max(max_, tile.children[j].value)
            ratio__ = max((max__*w)/(sum__**2), (sum__**2)/(min__*w))
            if ratio__ > ratio_:
                break

            sum_ = sum__
            min_ = min__
            max_ = max__
            ratio_ = ratio__
            j += 1

        # vertical col? dice horizontally?
        if width > height:
            dx = (sum_ / value) * width
            partition_dice(Tile(
                        tile.key, tile.children[i:j],
                        x, y, dx, height),
                    partition=lambda *_, **__: ())
            x += dx
            width -= dx

        # horizontal row? slice vertically?
        else:
            dy = (sum_ / value) * height
            partition_slice(Tile(
                        tile.key, tile.children[i:j],
                        x, y, width, dy),
                    partition=lambda *_, **__: ())
            y += dy
            height -= dy

        # start partitioning the other direction
        value -= sum_
        i = j

    # recursively partition
    for t in tile.children:
        partition(t, partition=partition, aspect_ratio=aspect_ratio)


def main(csv_paths, *,
        by=None,
        fields=None,
        labels=None,
        defines=[],
        color=False,
        dots=False,
        braille=False,
        chars=None,
        colors=None,
        width=None,
        height=None,
        no_header=False,
        to_scale=None,
        aspect_ratio=(1,1),
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # figure out chars/colors
    if chars is not None:
        chars_ = chars
    else:
        chars_ = CHARS

    if colors is not None:
        colors_ = colors
    else:
        colors_ = COLORS

    # figure out width/height
    if width is None:
        width_ = min(80, shutil.get_terminal_size((80, 5))[0])
    elif width:
        width_ = width
    else:
        width_ = shutil.get_terminal_size((80, 5))[0]

    if height is None:
        height_ = 1
    elif height:
        height_ = height
    else:
        height_ = shutil.get_terminal_size((80, 5))[1]
        # make space for shell prompt and stuff
        height_ = max(height_-(1 if no_header else 2), 1)

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
    datasets, labels_ = fold(results, by, fields, labels, defines)

    # build tile heirarchy
    tile = Tile.merge([
            Tile(k, v, label=labels_.get(k))
            for k, v in datasets.items()
            # discard anything with the value 0 early, otherwise these
            # cause a lot of problems
            if v != 0])

    # sort
    tile.sort()

    # assign colors/chars after sorting to try to minimize touching
    # colors, while keeping things somewhat reproducible

    # use colors for top of tree
    for i, t in enumerate(tile.children):
        for t_ in t.tiles():
            t_.color = i % len(colors_)

    # and chars for bottom of tree
    for i, t in enumerate(tile.leaves()):
        t.char = i % len(chars_)

    # scale width/height if requested now that we have our data
    if to_scale and (width is None or height is None):
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
    canvas = Canvas(width_, height_,
            color=color,
            dots=dots or chars is None,
            braille=braille)

    # recursively partition tiles
    tile.x = 0
    tile.y = 0
    tile.width = canvas.width
    tile.height = canvas.height
    if tile.value:
        if args.get('binary'):
            partition_binary(tile)
        elif args.get('slice'):
            partition_slice(tile)
        elif args.get('dice'):
            partition_dice(tile)
        elif args.get('slice_and_dice'):
            partition_slice_and_dice(tile)
        elif args.get('dice_and_slice'):
            partition_dice_and_slice(tile)
        elif args.get('squarify'):
            partition_squarify(tile)
        else:
            # default to binary partitioning
            partition_binary(tile)

    # render to canvas
    labels_ = []
    for t in tile.leaves():
        # this extra +0.1 and using points instead of width/height is
        # to help minimize rounding errors
        x__ = int(t.x+0.1)
        y__ = int(t.y+0.1)
        width__ = int(t.x+t.width+0.1) - x__
        height__ = int(t.y+t.height+0.1) - y__
        # skip anything with zero weight/height after aligning things
        if width__ == 0 or height__ == 0:
            continue

        # flip y
        y__ = canvas.height - (y__+height__)

        canvas.rect(x__, y__, width__, height__,
                # default to first letter in each label/key
                char=(True if braille or dots
                    else t.label[0]
                        if chars is None
                            and t.label is not None
                    else t.key[-1][0]
                        if chars is None
                            and t.key
                            and t.key[-1]
                    else chars_[t.char or 0]),
                color=colors_[t.color or 0])

        if labels:
            if t.label is not None:
                label__ = t.label
            else:
                label__ = ','.join(t.key)

            # render these later so they get priority
            labels_.append((x__, y__+height__-1, label__[:width__]))

    for x__, y__, label__ in labels_:
        canvas.label(x__, y__, label__)

    # print some summary info
    if not no_header:
        leaves = list(tile.leaves())
        mean = tile.value / max(len(leaves), 1)
        stddev = mt.sqrt(sum((t.value - mean)**2 for v in leaves)
                / max(len(leaves), 1))
        min_ = min((t.value for t in leaves), default=0)
        max_ = max((t.value for t in leaves), default=0)
        print('total %d, avg %d +-%.1fσ, min %d, max %d' % (
                tile.value, mean, stddev, min_, max_))

    # draw canvas
    for row in range(height_):
        line = canvas.draw(row)
        print(line)


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
            '-l', '--label',
            nargs='?',
            dest='labels',
            action='append',
            help="Field to use as tile label.")
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
            help="Characters to use for tiles.")
    parser.add_argument(
            '--colors',
            type=lambda x: [x.strip() for x in x.split(',')],
            help="Colors to use for tiles.")
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
            '--to-scale',
            nargs='?',
            type=float,
            const=1,
            help="Scale the resulting treemap such that 1 pixel ~= 1/scale "
                "units. Defaults to scale=1. ")
    parser.add_argument(
            '--aspect-ratio',
            type=lambda x: tuple(float(v) for v in x.split(':', 1)),
            default=(1, 1),
            help="Aspect ratio to use with --to-scale. Defaults to 1:1.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
