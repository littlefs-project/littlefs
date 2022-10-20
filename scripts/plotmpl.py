#!/usr/bin/env python3
#
# Plot CSV files with matplotlib.
#
# Example:
# ./scripts/plotmpl.py bench.csv -xSIZE -ybench_read -obench.svg
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
import numpy as np
import os
import shutil
import time

import matplotlib as mpl
import matplotlib.pyplot as plt

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
ALPHAS = [0.75]
FORMATS = ['-']
FORMATS_POINTS = ['.']
FORMATS_POINTS_AND_LINES = ['.-']

WIDTH = 735
HEIGHT = 350
FONT_SIZE = 11

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


# formatter for matplotlib
def si(x):
    if x == 0:
        return '0'
    # figure out prefix and scale
    p = 3*int(m.log(abs(x), 10**3))
    p = min(18, max(-18, p))
    # format with 3 digits of precision
    s = '%.3f' % (abs(x) / (10.0**p))
    s = s[:3+1]
    # truncate but only digits that follow the dot
    if '.' in s:
        s = s.rstrip('0')
        s = s.rstrip('.')
    return '%s%s%s' % ('-' if x < 0 else '', s, SI_PREFIXES[p])

# formatter for matplotlib
def si2(x):
    if x == 0:
        return '0'
    # figure out prefix and scale
    p = 10*int(m.log(abs(x), 2**10))
    p = min(30, max(-30, p))
    # format with 3 digits of precision
    s = '%.3f' % (abs(x) / (2.0**p))
    s = s[:3+1]
    # truncate but only digits that follow the dot
    if '.' in s:
        s = s.rstrip('0')
        s = s.rstrip('.')
    return '%s%s%s' % ('-' if x < 0 else '', s, SI2_PREFIXES[p])

# we want to use MaxNLocator, but since MaxNLocator forces multiples of 10
# to be an option, we can't really...
class AutoMultipleLocator(mpl.ticker.MultipleLocator):
    def __init__(self, base, nbins=None):
        # note base needs to be floats to avoid integer pow issues
        self.base = float(base)
        self.nbins = nbins
        super().__init__(self.base)

    def __call__(self):
        # find best tick count, conveniently matplotlib has a function for this
        vmin, vmax = self.axis.get_view_interval()
        vmin, vmax = mpl.transforms.nonsingular(vmin, vmax, 1e-12, 1e-13)
        if self.nbins is not None:
            nbins = self.nbins
        else:
            nbins = np.clip(self.axis.get_tick_space(), 1, 9)

        # find the best power, use this as our locator's actual base
        scale = self.base ** (m.ceil(m.log((vmax-vmin) / (nbins+1), self.base)))
        self.set_params(scale)

        return super().__call__()


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if mode == 'r':
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
        if m.isinf(x) or m.isnan(x):
            raise ValueError("invalid dat %r" % x)
    except ValueError:
        pass

    # else give up
    raise ValueError("invalid dat %r" % x)

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
                y_ = None
            else:
                try:
                    y_ = dat(r[y])
                except ValueError:
                    y_ = None
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
                    [(by_, k_) for by_, k_ in zip(by, ks_)]
                        if by is not None else [])

    return datasets


def main(csv_paths, output, *,
        svg=False,
        png=False,
        quiet=False,
        by=None,
        x=None,
        y=None,
        define=[],
        points=False,
        points_and_lines=False,
        colors=None,
        formats=None,
        width=WIDTH,
        height=HEIGHT,
        xlim=(None,None),
        ylim=(None,None),
        xlog=False,
        ylog=False,
        x2=False,
        y2=False,
        xticks=None,
        yticks=None,
        xunits=None,
        yunits=None,
        xlabel=None,
        ylabel=None,
        xticklabels=None,
        yticklabels=None,
        title=None,
        legend=None,
        dark=False,
        ggplot=False,
        xkcd=False,
        font=None,
        font_size=FONT_SIZE,
        background=None):
    # guess the output format
    if not png and not svg:
        if output.endswith('.png'):
            png = True
        else:
            svg = True

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

    # what colors/alphas/formats to use?
    if colors is not None:
        colors_ = colors
    elif dark:
        colors_ = COLORS_DARK
    else:
        colors_ = COLORS

    if formats is not None:
        formats_ = formats
    elif points_and_lines:
        formats_ = FORMATS_POINTS_AND_LINES
    elif points:
        formats_ = FORMATS_POINTS
    else:
        formats_ = FORMATS

    if background is not None:
        background_ = background
    elif dark:
        background_ = mpl.style.library['dark_background']['figure.facecolor']
    else:
        background_ = plt.rcParams['figure.facecolor']

    # allow escape codes in labels/titles
    if title is not None:
        title = codecs.escape_decode(title.encode('utf8'))[0].decode('utf8')
    if xlabel is not None:
        xlabel = codecs.escape_decode(xlabel.encode('utf8'))[0].decode('utf8')
    if ylabel is not None:
        ylabel = codecs.escape_decode(ylabel.encode('utf8'))[0].decode('utf8')

    # first collect results from CSV files
    results = collect(csv_paths, renames)

    # then extract the requested datasets
    datasets_ = datasets(results, by, x, y, define)

    # configure some matplotlib settings
    if xkcd:
        plt.xkcd()
        # turn off the white outline, this breaks some things
        plt.rc('path', effects=[])
    if ggplot:
        plt.style.use('ggplot')
        plt.rc('patch', linewidth=0)
        plt.rc('axes', edgecolor=background_)
        plt.rc('grid', color=background_)
        # fix the the gridlines when ggplot+xkcd
        if xkcd:
            plt.rc('grid', linewidth=1)
            plt.rc('axes.spines', bottom=False, left=False)
    if dark:
        plt.style.use('dark_background')
        plt.rc('savefig', facecolor='auto')
        # fix ggplot when dark
        if ggplot:
            plt.rc('axes',
                facecolor='#333333',
                edgecolor=background_,
                labelcolor='#aaaaaa')
            plt.rc('xtick', color='#aaaaaa')
            plt.rc('ytick', color='#aaaaaa')
            plt.rc('grid', color=background_)

    if font is not None:
        plt.rc('font', family=font)
    plt.rc('font', size=font_size)
    plt.rc('figure', titlesize='medium')
    plt.rc('axes', titlesize='medium', labelsize='small')
    plt.rc('xtick', labelsize='small')
    plt.rc('ytick', labelsize='small')
    plt.rc('legend',
        fontsize='small',
        fancybox=False,
        framealpha=None,
        borderaxespad=0)
    plt.rc('axes.spines', top=False, right=False)

    plt.rc('figure', facecolor=background_, edgecolor=background_)
    if not ggplot:
        plt.rc('axes', facecolor='#00000000')

    # create a matplotlib plot
    fig = plt.figure(figsize=(
        width/plt.rcParams['figure.dpi'],
        height/plt.rcParams['figure.dpi']),
        # note we need a linewidth to keep xkcd mode happy
        linewidth=8)
    ax = fig.subplots()

    for i, (name, dataset) in enumerate(datasets_.items()):
        dats = sorted((x,y) for x,y in dataset.items())
        ax.plot([x for x,_ in dats], [y for _,y in dats],
            formats_[i % len(formats_)],
            color=colors_[i % len(colors_)],
            label=','.join(k for k in name if k))

    # axes scaling
    if xlog:
        ax.set_xscale('symlog')
        ax.xaxis.set_minor_locator(mpl.ticker.NullLocator())
    if ylog:
        ax.set_yscale('symlog')
        ax.yaxis.set_minor_locator(mpl.ticker.NullLocator())
    # axes limits
    ax.set_xlim(
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
    ax.set_ylim(
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
    # axes ticks
    if x2:
        ax.xaxis.set_major_formatter(lambda x, pos:
            si2(x)+(xunits if xunits else ''))
        if xticklabels is not None:
            ax.xaxis.set_ticklabels(xticklabels)
        if xticks is None:
            ax.xaxis.set_major_locator(AutoMultipleLocator(2))
        elif isinstance(xticks, list):
            ax.xaxis.set_major_locator(mpl.ticker.FixedLocator(xticks))
        elif xticks != 0:
            ax.xaxis.set_major_locator(AutoMultipleLocator(2, xticks-1))
        else:
            ax.xaxis.set_major_locator(mpl.ticker.NullLocator())
    else:
        ax.xaxis.set_major_formatter(lambda x, pos:
            si(x)+(xunits if xunits else ''))
        if xticklabels is not None:
            ax.xaxis.set_ticklabels(xticklabels)
        if xticks is None:
            ax.xaxis.set_major_locator(mpl.ticker.AutoLocator())
        elif isinstance(xticks, list):
            ax.xaxis.set_major_locator(mpl.ticker.FixedLocator(xticks))
        elif xticks != 0:
            ax.xaxis.set_major_locator(mpl.ticker.MaxNLocator(xticks-1))
        else:
            ax.xaxis.set_major_locator(mpl.ticker.NullLocator())
    if y2:
        ax.yaxis.set_major_formatter(lambda x, pos:
            si2(x)+(yunits if yunits else ''))
        if yticklabels is not None:
            ax.yaxis.set_ticklabels(yticklabels)
        if yticks is None:
            ax.yaxis.set_major_locator(AutoMultipleLocator(2))
        elif isinstance(yticks, list):
            ax.yaxis.set_major_locator(mpl.ticker.FixedLocator(yticks))
        elif yticks != 0:
            ax.yaxis.set_major_locator(AutoMultipleLocator(2, yticks-1))
        else:
            ax.yaxis.set_major_locator(mpl.ticker.NullLocator())
    else:
        ax.yaxis.set_major_formatter(lambda x, pos:
            si(x)+(yunits if yunits else ''))
        if yticklabels is not None:
            ax.yaxis.set_ticklabels(yticklabels)
        if yticks is None:
            ax.yaxis.set_major_locator(mpl.ticker.AutoLocator())
        elif isinstance(yticks, list):
            ax.yaxis.set_major_locator(mpl.ticker.FixedLocator(yticks))
        elif yticks != 0:
            ax.yaxis.set_major_locator(mpl.ticker.MaxNLocator(yticks-1))
        else:
            ax.yaxis.set_major_locator(mpl.ticker.NullLocator())
    # axes labels
    if xlabel is not None:
        ax.set_xlabel(xlabel)
    if ylabel is not None:
        ax.set_ylabel(ylabel)
    if ggplot:
        ax.grid(sketch_params=None)

    if title is not None:
        ax.set_title(title)

    # pre-render so we can derive some bboxes
    fig.tight_layout()
    # it's not clear how you're actually supposed to get the renderer if
    # get_renderer isn't supported
    try:
        renderer = fig.canvas.get_renderer()
    except AttributeError:
        renderer = fig._cachedRenderer

    # add a legend? this actually ends up being _really_ complicated
    if legend == 'right':
        l_pad = fig.transFigure.inverted().transform((
            mpl.font_manager.FontProperties('small')
                .get_size_in_points()/2,
            0))[0]

        legend_ = ax.legend(
            bbox_to_anchor=(1+l_pad, 1),
            loc='upper left',
            fancybox=False,
            borderaxespad=0)
        if ggplot:
            legend_.get_frame().set_linewidth(0)
        fig.tight_layout()

    elif legend == 'left':
        l_pad = fig.transFigure.inverted().transform((
            mpl.font_manager.FontProperties('small')
                .get_size_in_points()/2,
            0))[0]

        # place legend somewhere to get its bbox
        legend_ = ax.legend(
            bbox_to_anchor=(0, 1),
            loc='upper right',
            fancybox=False,
            borderaxespad=0)

        # first make space for legend without the legend in the figure
        l_bbox = (legend_.get_tightbbox(renderer)
            .transformed(fig.transFigure.inverted()))
        legend_.remove()
        fig.tight_layout(rect=(0, 0, 1-l_bbox.width-l_pad, 1))

        # place legend after tight_layout computation
        bbox = (ax.get_tightbbox(renderer)
            .transformed(ax.transAxes.inverted()))
        legend_ = ax.legend(
            bbox_to_anchor=(bbox.x0-l_pad, 1),
            loc='upper right',
            fancybox=False,
            borderaxespad=0)
        if ggplot:
            legend_.get_frame().set_linewidth(0)

    elif legend == 'above':
        l_pad = fig.transFigure.inverted().transform((
            0,
            mpl.font_manager.FontProperties('small')
                .get_size_in_points()/2))[1]

        # try different column counts until we fit in the axes
        for ncol in reversed(range(1, len(datasets_)+1)):
            legend_ = ax.legend(
                bbox_to_anchor=(0.5, 1+l_pad),
                loc='lower center',
                ncol=ncol,
                fancybox=False,
                borderaxespad=0)
            if ggplot:
                legend_.get_frame().set_linewidth(0)

            l_bbox = (legend_.get_tightbbox(renderer)
                .transformed(ax.transAxes.inverted()))
            if l_bbox.x0 >= 0:
                break

        # fix the title
        if title is not None:
            t_bbox = (ax.title.get_tightbbox(renderer)
                .transformed(ax.transAxes.inverted()))
            ax.set_title(None)
            fig.tight_layout(rect=(0, 0, 1, 1-t_bbox.height))

            l_bbox = (legend_.get_tightbbox(renderer)
                .transformed(ax.transAxes.inverted()))
            ax.set_title(title, y=1+l_bbox.height+l_pad)

    elif legend == 'below':
        l_pad = fig.transFigure.inverted().transform((
            0,
            mpl.font_manager.FontProperties('small')
                .get_size_in_points()/2))[1]

        # try different column counts until we fit in the axes
        for ncol in reversed(range(1, len(datasets_)+1)):
            legend_ = ax.legend(
                bbox_to_anchor=(0.5, 0),
                loc='upper center',
                ncol=ncol,
                fancybox=False,
                borderaxespad=0)

            l_bbox = (legend_.get_tightbbox(renderer)
                .transformed(ax.transAxes.inverted()))
            if l_bbox.x0 >= 0:
                break

        # first make space for legend without the legend in the figure
        l_bbox = (legend_.get_tightbbox(renderer)
            .transformed(fig.transFigure.inverted()))
        legend_.remove()
        fig.tight_layout(rect=(0, 0, 1, 1-l_bbox.height-l_pad))

        bbox = (ax.get_tightbbox(renderer)
            .transformed(ax.transAxes.inverted()))
        legend_ = ax.legend(
            bbox_to_anchor=(0.5, bbox.y0-l_pad),
            loc='upper center',
            ncol=ncol,
            fancybox=False,
            borderaxespad=0)
        if ggplot:
            legend_.get_frame().set_linewidth(0)

    # compute another tight_layout for good measure, because this _does_
    # fix some things... I don't really know why though
    fig.tight_layout()

    plt.savefig(output, format='png' if png else 'svg', bbox_inches='tight')

    # some stats
    if not quiet:
        print('updated %s, %s datasets, %s points' % (
            output,
            len(datasets_),
            sum(len(dataset) for dataset in datasets_.values())))


if __name__ == "__main__":
    import sys
    import argparse
    parser = argparse.ArgumentParser(
        description="Plot CSV files with matplotlib.",
        allow_abbrev=False)
    parser.add_argument(
        'csv_paths',
        nargs='*',
        help="Input *.csv files.")
    parser.add_argument(
        '-o', '--output',
        required=True,
        help="Output *.svg/*.png file.")
    parser.add_argument(
        '--svg',
        action='store_true',
        help="Output an svg file. By default this is infered.")
    parser.add_argument(
        '--png',
        action='store_true',
        help="Output a png file. By default this is infered.")
    parser.add_argument(
        '-q', '--quiet',
        action='store_true',
        help="Don't print info.")
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
        help="Comma-separated hex colors to use.")
    parser.add_argument(
        '--formats',
        type=lambda x: [x.strip().replace('0',',') for x in x.split(',')],
        help="Comma-separated matplotlib formats to use. Allows '0' as an "
            "alternative for ','.")
    parser.add_argument(
        '-W', '--width',
        type=lambda x: int(x, 0),
        help="Width in pixels. Defaults to %r." % WIDTH)
    parser.add_argument(
        '-H', '--height',
        type=lambda x: int(x, 0),
        help="Height in pixels. Defaults to %r." % HEIGHT)
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
        '--xticks',
        type=lambda x: int(x, 0) if ',' not in x
            else [dat(x) for x in x.split(',')],
        help="Ticks for the x-axis. This can be explicit comma-separated "
            "ticks, the number of ticks, or 0 to disable.")
    parser.add_argument(
        '--yticks',
        type=lambda x: int(x, 0) if ',' not in x
            else [dat(x) for x in x.split(',')],
        help="Ticks for the y-axis. This can be explicit comma-separated "
            "ticks, the number of ticks, or 0 to disable.")
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
        '--xticklabels',
        type=lambda x: [x.strip() for x in x.split(',')],
        help="Comma separated xticklabels.")
    parser.add_argument(
        '--yticklabels',
        type=lambda x: [x.strip() for x in x.split(',')],
        help="Comma separated yticklabels.")
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
        '--dark',
        action='store_true',
        help="Use the dark style.")
    parser.add_argument(
        '--ggplot',
        action='store_true',
        help="Use the ggplot style.")
    parser.add_argument(
        '--xkcd',
        action='store_true',
        help="Use the xkcd style.")
    parser.add_argument(
        '--font',
        type=lambda x: [x.strip() for x in x.split(',')],
        help="Font family for matplotlib.")
    parser.add_argument(
        '--font-size',
        help="Font size for matplotlib. Defaults to %r." % FONT_SIZE)
    parser.add_argument(
        '--background',
        help="Background color to use.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
