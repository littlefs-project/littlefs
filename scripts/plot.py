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

import codecs
import collections as co
import csv
import io
import itertools as it
import math as m
import os
import shutil
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
    p = 3*int(m.log(abs(x)*10, 10**3))
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
    p = 10*int(m.log(abs(x)*10, 2**10))
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
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

def inotifywait(paths):
    # wait for interesting events
    inotify = inotify_simple.INotify()
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
                inotify.add_watch(dir, flags)
                for f in files:
                    inotify.add_watch(os.path.join(dir, f), flags)
        else:
            inotify.add_watch(path, flags)

    # wait for event
    inotify.read()

class LinesIO:
    def __init__(self, maxlen=None):
        self.maxlen = maxlen
        self.lines = co.deque(maxlen=maxlen)
        self.tail = io.StringIO()

        # trigger automatic sizing
        if maxlen == 0:
            self.resize(0)

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

        # first thing first, give ourself a canvas
        while LinesIO.canvas_lines < len(self.lines):
            sys.stdout.write('\n')
            LinesIO.canvas_lines += 1

        # clear the bottom of the canvas if we shrink
        shrink = LinesIO.canvas_lines - len(self.lines)
        if shrink > 0:
            for i in range(shrink):
                sys.stdout.write('\r')
                if shrink-1-i > 0:
                    sys.stdout.write('\x1b[%dA' % (shrink-1-i))
                sys.stdout.write('\x1b[K')
                if shrink-1-i > 0:
                    sys.stdout.write('\x1b[%dB' % (shrink-1-i))
            sys.stdout.write('\x1b[%dA' % shrink)
            LinesIO.canvas_lines = len(self.lines)

        for i, line in enumerate(self.lines):
            # move cursor, clear line, disable/reenable line wrapping
            sys.stdout.write('\r')
            if len(self.lines)-1-i > 0:
                sys.stdout.write('\x1b[%dA' % (len(self.lines)-1-i))
            sys.stdout.write('\x1b[K')
            sys.stdout.write('\x1b[?7l')
            sys.stdout.write(line)
            sys.stdout.write('\x1b[?7h')
            if len(self.lines)-1-i > 0:
                sys.stdout.write('\x1b[%dB' % (len(self.lines)-1-i))
        sys.stdout.flush()


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
        if m.isinf(x) or m.isnan(x):
            raise ValueError("invalid dat %r" % x)
    except ValueError:
        pass

    # else give up
    raise ValueError("invalid dat %r" % x)


# a hack log that preserves sign, with a linear region between -1 and 1
def symlog(x):
    if x > 1:
        return m.log(x)+1
    elif x < -1:
        return -m.log(-x)-1
    else:
        return x

class Plot:
    def __init__(self, width, height, *,
            xlim=None,
            ylim=None,
            xlog=False,
            ylog=False,
            **_):
        self.width = width
        self.height = height
        self.xlim = xlim or (0, width)
        self.ylim = ylim or (0, height)
        self.xlog = xlog
        self.ylog = ylog
        self.grid = [('',False)]*(self.width*self.height)

    def scale(self, x, y):
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
            color=COLORS[0],
            char=True):
        # scale
        x, y = self.scale(x, y)

        # ignore out of bounds points
        if x >= 0 and x < self.width and y >= 0 and y < self.height:
            self.grid[x + y*self.width] = (color, char)

    def line(self, x1, y1, x2, y2, *,
            color=COLORS[0],
            char=True):
        # scale
        x1, y1 = self.scale(x1, y1)
        x2, y2 = self.scale(x2, y2)

        # incremental error line algorithm
        ex = abs(x2 - x1)
        ey = -abs(y2 - y1)
        dx = +1 if x1 < x2 else -1
        dy = +1 if y1 < y2 else -1
        e = ex + ey

        while True:
            if x1 >= 0 and x1 < self.width and y1 >= 0 and y1 < self.height:
                self.grid[x1 + y1*self.width] = (color, char)
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

        if x2 >= 0 and x2 < self.width and y2 >= 0 and y2 < self.height:
            self.grid[x2 + y2*self.width] = (color, char)

    def plot(self, coords, *,
            color=COLORS[0],
            char=True,
            line_char=True):
        # draw lines
        if line_char:
            for (x1, y1), (x2, y2) in zip(coords, coords[1:]):
                if y1 is not None and y2 is not None:
                    self.line(x1, y1, x2, y2,
                        color=color,
                        char=line_char)

        # draw points
        if char and (not line_char or char is not True):
            for x, y in coords:
                if y is not None:
                    self.point(x, y,
                        color=color,
                        char=char)

    def draw(self, row, *,
            dots=False,
            braille=False,
            color=False,
            **_):
        # scale if needed
        if braille:
            xscale, yscale = 2, 4
        elif dots:
            xscale, yscale = 1, 2
        else:
            xscale, yscale = 1, 1

        y = self.height//yscale-1 - row
        row_ = []
        for x in range(self.width//xscale):
            best_f = ''
            best_c = False

            # encode into a byte
            b = 0
            for i in range(xscale*yscale):
                f, c = self.grid[x*xscale+(xscale-1-(i%xscale))
                        + (y*yscale+(i//xscale))*self.width]
                if c:
                    b |= 1 << i

                if f:
                    best_f = f
                if c and c is not True:
                    best_c = c

            # use byte to lookup character
            if b:
                if best_c:
                    c = best_c
                elif braille:
                    c = CHARS_BRAILLE[b]
                else:
                    c = CHARS_DOTS[b]
            else:
                c = ' '

            # color?
            if b and color and best_f:
                c = '\x1b[%sm%s\x1b[m' % (best_f, c)

            # draw axis in blank spaces
            if not b:
                if x == 0 and y == 0:
                    c = '+'
                elif x == 0 and y == self.height//yscale-1:
                    c = '^'
                elif x == self.width//xscale-1 and y == 0:
                    c = '>'
                elif x == 0:
                    c = '|'
                elif y == 0:
                    c = '-'

            row_.append(c)

        return ''.join(row_)


def collect(csv_paths, renames=[]):
    # collect results from CSV files
    results = []
    for path in csv_paths:
        try:
            with openio(path) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    results.append(r)
        except FileNotFoundError:
            pass

    if renames:
        for r in results:
            # make a copy so renames can overlap
            r_ = {}
            for new_k, old_k in renames:
                if old_k in r:
                    r_[new_k] = r[old_k]
            r.update(r_)

    return results

def dataset(results, x=None, y=None, define=[]):
    # organize by 'by', x, and y
    dataset = {}
    i = 0
    for r in results:
        # filter results by matching defines
        if not all(k in r and r[k] in vs for k, vs in define):
            continue

        # find xs
        if x is not None:
            if x not in r:
                continue
            try:
                x_ = dat(r[x])
            except ValueError:
                continue
        else:
            x_ = i
            i += 1

        # find ys
        if y is not None:
            if y not in r:
                continue
            try:
                y_ = dat(r[y])
            except ValueError:
                continue
        else:
            y_ = None

        if y_ is not None:
            dataset[x_] = y_ + dataset.get(x_, 0)
        else:
            dataset[x_] = y_ or dataset.get(x_, None)

    return dataset

def datasets(results, by=None, x=None, y=None, define=[]):
    # filter results by matching defines
    results_ = []
    for r in results:
        if all(k in r and r[k] in vs for k, vs in define):
            results_.append(r)
    results = results_

    # if y not specified, try to guess from data
    if y is None:
        y = co.OrderedDict()
        for r in results:
            for k, v in r.items():
                if (by is None or k not in by) and v.strip():
                    try:
                        dat(v)
                        y[k] = True
                    except ValueError:
                        y[k] = False
        y = list(k for k,v in y.items() if v)

    if by is not None:
        # find all 'by' values
        ks = set()
        for r in results:
            ks.add(tuple(r.get(k, '') for k in by))
        ks = sorted(ks)

    # collect all datasets
    datasets = co.OrderedDict()
    for ks_ in (ks if by is not None else [()]):
        for x_ in (x if x is not None else [None]):
            for y_ in y:
                # hide x/y if there is only one field
                k_x = x_ if len(x or []) > 1 else ''
                k_y = y_ if len(y or []) > 1 or (not ks_ and not k_x) else ''

                datasets[ks_ + (k_x, k_y)] = dataset(
                    results,
                    x_,
                    y_,
                    [(by_, {k_}) for by_, k_ in zip(by, ks_)]
                        if by is not None else [])

    return datasets
    

def main(csv_paths, *,
        by=None,
        x=None,
        y=None,
        define=[],
        width=None,
        height=None,
        xlim=(None,None),
        ylim=(None,None),
        x2=False,
        y2=False,
        xunits='',
        yunits='',
        xlabel=None,
        ylabel=None,
        cat=False,
        color=False,
        braille=False,
        colors=None,
        chars=None,
        line_chars=None,
        points=False,
        points_and_lines=False,
        title=None,
        legend=None,
        keep_open=False,
        sleep=None,
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # allow shortened ranges
    if len(xlim) == 1:
        xlim = (0, xlim[0])
    if len(ylim) == 1:
        ylim = (0, ylim[0])

    # separate out renames
    renames = list(it.chain.from_iterable(
        ((k, v) for v in vs)
        for k, vs in it.chain(by or [], x or [], y or [])))
    if by is not None:
        by = [k for k, _ in by]
    if x is not None:
        x = [k for k, _ in x]
    if y is not None:
        y = [k for k, _ in y]

    # what colors to use?
    if colors is not None:
        colors_ = colors
    else:
        colors_ = COLORS

    if chars is not None:
        chars_ = chars
    elif points_and_lines:
        chars_ = CHARS_POINTS_AND_LINES
    else:
        chars_ = [True]

    if line_chars is not None:
        line_chars_ = line_chars
    elif points_and_lines or not points:
        line_chars_ = [True]
    else:
        line_chars_ = [False]

    # allow escape codes in labels/titles
    if title is not None:
        title = codecs.escape_decode(title.encode('utf8'))[0].decode('utf8')
    if xlabel is not None:
        xlabel = codecs.escape_decode(xlabel.encode('utf8'))[0].decode('utf8')
    if ylabel is not None:
        ylabel = codecs.escape_decode(ylabel.encode('utf8'))[0].decode('utf8')

    title = title.splitlines() if title is not None else []
    xlabel = xlabel.splitlines() if xlabel is not None else []
    ylabel = ylabel.splitlines() if ylabel is not None else []

    def draw(f):
        def writeln(s=''):
            f.write(s)
            f.write('\n')
        f.writeln = writeln

        # first collect results from CSV files
        results = collect(csv_paths, renames)

        # then extract the requested datasets
        datasets_ = datasets(results, by, x, y, define)

        # build legend?
        legend_width = 0
        if legend:
            legend_ = []
            for i, k in enumerate(datasets_.keys()):
                label = '%s%s' % (
                    '%s ' % chars_[i % len(chars_)]
                        if chars is not None
                        else '%s ' % line_chars_[i % len(line_chars_)]
                        if line_chars is not None
                        else '',
                    ','.join(k_ for k_ in k if k_))

                if label:
                    legend_.append(label)
                    legend_width = max(legend_width, len(label)+1)

        # find xlim/ylim
        xlim_ = (
            xlim[0] if xlim[0] is not None
                else min(it.chain([0], (k
                    for r in datasets_.values()
                    for k, v in r.items()
                    if v is not None))),
            xlim[1] if xlim[1] is not None
                else max(it.chain([0], (k
                    for r in datasets_.values()
                    for k, v in r.items()
                    if v is not None))))

        ylim_ = (
            ylim[0] if ylim[0] is not None
                else min(it.chain([0], (v
                    for r in datasets_.values()
                    for _, v in r.items()
                    if v is not None))),
            ylim[1] if ylim[1] is not None
                else max(it.chain([0], (v
                    for r in datasets_.values()
                    for _, v in r.items()
                    if v is not None))))

        # figure out our plot size
        if width is None:
            width_ = min(80, shutil.get_terminal_size((80, None))[0])
        elif width:
            width_ = width
        else:
            width_ = shutil.get_terminal_size((80, None))[0]
        # make space for units
        width_ -= (5 if y2 else 4)+1+len(yunits)
        # make space for label
        width_ -= len(ylabel)
        # make space for legend
        if legend in {'left', 'right'} and legend_:
            width_ -= legend_width
        # limit a bit
        width_ = max(2*((5 if x2 else 4)+len(xunits)), width_)

        if height is None:
            height_ = 17 + len(title) + len(xlabel)
        elif height:
            height_ = height
        else:
            height_ = shutil.get_terminal_size((None,
                17 + len(title) + len(xlabel)))[1]
            # make space for shell prompt
            if not keep_open:
                height_ -= 1
        # make space for units
        height_ -= 1
        # make space for label
        height_ -= len(xlabel)
        # make space for title
        height_ -= len(title)
        # make space for legend
        if legend in {'above', 'below'} and legend_:
            legend_cols = min(len(legend_), max(1, width_//legend_width))
            height_ -= (len(legend_)+legend_cols-1) // legend_cols
        # limit a bit
        height_ = max(2, height_)

        # figure out margin for label/units/legend
        margin = (5 if y2 else 4) + len(yunits) + len(ylabel)
        if legend == 'left' and legend_:
            margin += legend_width

        # make it easier to transpose ylabel
        ylabel_ = [l.center(height_) for l in ylabel]

        # create a plot and draw our coordinates
        plot = Plot(
            # scale if we're printing with dots or braille
            2*width_ if line_chars is None and braille else width_,
            4*height_ if line_chars is None and braille
                else 2*height_ if line_chars is None
                else height_,
            xlim=xlim_,
            ylim=ylim_,
            **args)

        for i, (k, dataset) in enumerate(datasets_.items()):
            plot.plot(
                sorted((x,y) for x,y in dataset.items()),
                color=colors_[i % len(colors_)],
                char=chars_[i % len(chars_)],
                line_char=line_chars_[i % len(line_chars_)])
        

        # draw title?
        for line in title:
            f.writeln('%*s %s' % (margin, '', line.center(width_)))
        # draw legend=above?
        if legend == 'above' and legend_:
            for i in range(0, len(legend_), legend_cols):
                f.writeln('%*s %*s%s' % (
                    margin,
                    '',
                    max(width_ - sum(len(label)+1
                        for label in legend_[i:i+legend_cols]),
                        0) // 2,
                    '',
                    ' '.join('%s%s%s' % (
                        '\x1b[%sm' % colors_[j % len(colors_)] if color else '',
                        legend_[j],
                        '\x1b[m' if color else '')
                        for j in range(i, min(i+legend_cols, len(legend_))))))
        for row in range(height_):
            f.writeln('%s%s%*s %s%s' % (
                # draw legend=left?
                ('%s%-*s %s' % (
                    '\x1b[%sm' % colors_[row % len(colors_)] if color else '',
                    legend_width-1,
                    legend_[row] if row < len(legend_) else '',
                    '\x1b[m' if color else ''))
                    if legend == 'left' and legend_ else '',
                # draw ylabel?
                ('%*s' % (
                    len(ylabel),
                    ''.join(l[row] for l in ylabel_))),
                # draw plot
                (5 if y2 else 4)+len(yunits),
                (si2 if y2 else si)(ylim_[0])+yunits if row == height_-1
                    else (si2 if y2 else si)(ylim_[1])+yunits if row == 0
                    else '',
                plot.draw(row,
                    braille=line_chars is None and braille,
                    dots=line_chars is None and not braille,
                    color=color,
                    **args),
                # draw legend=right?
                (' %s%s%s' % (
                    '\x1b[%sm' % colors_[row % len(colors_)] if color else '',
                    legend_[row] if row < len(legend_) else '',
                    '\x1b[m' if color else ''))
                    if legend == 'right' and legend_ else ''))
        f.writeln('%*s %-*s%*s%*s' % (
            margin,
            '',
            (5 if x2 else 4)+len(xunits),
            (si2 if x2 else si)(xlim_[0])+xunits,
            width_ - 2*((5 if x2 else 4)+len(xunits)),
            '',
            (5 if x2 else 4)+len(xunits),
            (si2 if x2 else si)(xlim_[1])+xunits))
        # draw xlabel?
        for line in xlabel:
            f.writeln('%*s %s' % (margin, '', line.center(width_)))
        # draw legend=below?
        if legend == 'below' and legend_:
            for i in range(0, len(legend_), legend_cols):
                f.writeln('%*s %*s%s' % (
                    margin,
                    '',
                    max(width_ - sum(len(label)+1
                        for label in legend_[i:i+legend_cols]),
                        0) // 2,
                    '',
                    ' '.join('%s%s%s' % (
                        '\x1b[%sm' % colors_[j % len(colors_)] if color else '',
                        legend_[j],
                        '\x1b[m' if color else '')
                        for j in range(i, min(i+legend_cols, len(legend_))))))

    if keep_open:
        try:
            while True:
                if cat:
                    draw(sys.stdout)
                else:
                    ring = LinesIO()
                    draw(ring)
                    ring.draw()

                # try to inotifywait
                if inotify_simple is not None:
                    ptime = time.time()
                    inotifywait(csv_paths)
                    # sleep for a minimum amount of time, this helps issues
                    # around rapidly updating files
                    time.sleep(max(0, (sleep or 0.01) - (time.time()-ptime)))
                else:
                    time.sleep(sleep or 0.1)
        except KeyboardInterrupt:
            pass

        if cat:
            draw(sys.stdout)
        else:
            ring = LinesIO()
            draw(ring)
            ring.draw()
        sys.stdout.write('\n')
    else:
        draw(sys.stdout)


if __name__ == "__main__":
    import sys
    import argparse
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
        type=lambda x: (
            lambda k,v=None: (k, v.split(',') if v is not None else ())
            )(*x.split('=', 1)),
        help="Group by this field. Can rename fields with new_name=old_name.")
    parser.add_argument(
        '-x',
        action='append',
        type=lambda x: (
            lambda k,v=None: (k, v.split(',') if v is not None else ())
            )(*x.split('=', 1)),
        help="Field to use for the x-axis. Can rename fields with "
            "new_name=old_name.")
    parser.add_argument(
        '-y',
        action='append',
        type=lambda x: (
            lambda k,v=None: (k, v.split(',') if v is not None else ())
            )(*x.split('=', 1)),
        help="Field to use for the y-axis. Can rename fields with "
            "new_name=old_name.")
    parser.add_argument(
        '-D', '--define',
        type=lambda x: (lambda k,v: (k, set(v.split(','))))(*x.split('=', 1)),
        action='append',
        help="Only include results where this field is this value. May include "
            "comma-separated options.")
    parser.add_argument(
        '--color',
        choices=['never', 'always', 'auto'],
        default='auto',
        help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
        '-⣿', '--braille',
        action='store_true',
        help="Use 2x4 unicode braille characters. Note that braille characters "
            "sometimes suffer from inconsistent widths.")
    parser.add_argument(
        '-.', '--points',
        action='store_true',
        help="Only draw data points.")
    parser.add_argument(
        '-!', '--points-and-lines',
        action='store_true',
        help="Draw data points and lines.")
    parser.add_argument(
        '--colors',
        type=lambda x: [x.strip() for x in x.split(',')],
        help="Comma-separated colors to use.")
    parser.add_argument(
        '--chars',
        help="Characters to use for points.")
    parser.add_argument(
        '--line-chars',
        help="Characters to use for lines.")
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
        help="Height in rows. 0 uses the terminal height. Defaults to 17.")
    parser.add_argument(
        '-z', '--cat',
        action='store_true',
        help="Pipe directly to stdout.")
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
        help="Add a label to the x-axis.")
    parser.add_argument(
        '--ylabel',
        help="Add a label to the y-axis.")
    parser.add_argument(
        '-t', '--title',
        help="Add a title.")
    parser.add_argument(
        '-l', '--legend',
        nargs='?',
        choices=['above', 'below', 'left', 'right'],
        const='right',
        help="Place a legend here.")
    parser.add_argument(
        '-k', '--keep-open',
        action='store_true',
        help="Continue to open and redraw the CSV files in a loop.")
    parser.add_argument(
        '-s', '--sleep',
        type=float,
        help="Time in seconds to sleep between redraws when running with -k. "
            "Defaults to 0.01.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
