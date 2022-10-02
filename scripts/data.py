#!/usr/bin/env python3
#
# Script to find data size at the function level. Basically just a big wrapper
# around nm with some extra conveniences for comparing builds. Heavily inspired
# by Linux's Bloat-O-Meter.
#
# Example:
# ./scripts/data.py lfs.o lfs_util.o -S
#
# Copyright (c) 2022, The littlefs authors.
# Copyright (c) 2020, Arm Limited. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#

import collections as co
import csv
import glob
import itertools as it
import math as m
import os
import re
import shlex
import subprocess as sp


OBJ_PATHS = ['*.o']
NM_TOOL = ['nm']
TYPE = 'dDbB'


# integer fields
class Int(co.namedtuple('Int', 'x')):
    __slots__ = ()
    def __new__(cls, x=0):
        if isinstance(x, Int):
            return x
        if isinstance(x, str):
            try:
                x = int(x, 0)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', x):
                    x = m.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', x):
                    x = -m.inf
                else:
                    raise
        assert isinstance(x, int) or m.isinf(x), x
        return super().__new__(cls, x)

    def __str__(self):
        if self.x == m.inf:
            return '∞'
        elif self.x == -m.inf:
            return '-∞'
        else:
            return str(self.x)

    def __int__(self):
        assert not m.isinf(self.x)
        return self.x

    def __float__(self):
        return float(self.x)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    diff_none = '%7s' % '-'
    diff_table = table

    def diff_diff(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        diff = new - old
        if diff == +m.inf:
            return '%7s' % '+∞'
        elif diff == -m.inf:
            return '%7s' % '-∞'
        else:
            return '%+7d' % diff

    def ratio(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        if m.isinf(new) and m.isinf(old):
            return 0.0
        elif m.isinf(new):
            return +m.inf
        elif m.isinf(old):
            return -m.inf
        elif not old and not new:
            return 0.0
        elif not old:
            return 1.0
        else:
            return (new-old) / old

    def __add__(self, other):
        return self.__class__(self.x + other.x)

    def __sub__(self, other):
        return self.__class__(self.x - other.x)

    def __mul__(self, other):
        return self.__class__(self.x * other.x)

# data size results
class DataResult(co.namedtuple('DataResult', [
        'file', 'function',
        'size'])):
    _by = ['file', 'function']
    _fields = ['size']
    _types = {'size': Int}

    __slots__ = ()
    def __new__(cls, file='', function='', size=0):
        return super().__new__(cls, file, function,
            Int(size))

    def __add__(self, other):
        return DataResult(self.file, self.function,
            self.size + other.size)


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

def collect(paths, *,
        nm_tool=NM_TOOL,
        type=TYPE,
        build_dir=None,
        everything=False,
        **args):
    results = []
    pattern = re.compile(
        '^(?P<size>[0-9a-fA-F]+)' +
        ' (?P<type>[%s])' % re.escape(type) +
        ' (?P<func>.+?)$')
    for path in paths:
        # map to source file
        src_path = re.sub('\.o$', '.c', path)
        if build_dir:
            src_path = re.sub('%s/*' % re.escape(build_dir), '',
                src_path)
        # note nm-tool may contain extra args
        cmd = nm_tool + ['--size-sort', path]
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))
        proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            stderr=sp.PIPE if not args.get('verbose') else None,
            universal_newlines=True,
            errors='replace')
        for line in proc.stdout:
            m = pattern.match(line)
            if m:
                func = m.group('func')
                # discard internal functions
                if not everything and func.startswith('__'):
                    continue
                # discard .8449 suffixes created by optimizer
                func = re.sub('\.[0-9]+', '', func)

                results.append(DataResult(
                    src_path, func,
                    int(m.group('size'), 16)))

        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

    return results


def fold(Result, results, *,
        by=None,
        defines=None,
        **_):
    if by is None:
        by = Result._by

    for k in it.chain(by or [], (k for k, _ in defines or [])):
        if k not in Result._by and k not in Result._fields:
            print("error: could not find field %r?" % k)
            sys.exit(-1)

    # filter by matching defines
    if defines is not None:
        results_ = []
        for r in results:
            if all(getattr(r, k) in vs for k, vs in defines):
                results_.append(r)
        results = results_

    # organize results into conflicts
    folding = co.OrderedDict()
    for r in results:
        name = tuple(getattr(r, k) for k in by)
        if name not in folding:
            folding[name] = []
        folding[name].append(r)

    # merge conflicts
    folded = []
    for name, rs in folding.items():
        folded.append(sum(rs[1:], start=rs[0]))

    return folded

def table(Result, results, diff_results=None, *,
        by=None,
        fields=None,
        sort=None,
        summary=False,
        all=False,
        percent=False,
        **_):
    all_, all = all, __builtins__.all

    if by is None:
        by = Result._by
    if fields is None:
        fields = Result._fields
    types = Result._types

    # fold again
    results = fold(Result, results, by=by)
    if diff_results is not None:
        diff_results = fold(Result, diff_results, by=by)

    # organize by name
    table = {
        ','.join(str(getattr(r, k) or '') for k in by): r
        for r in results}
    diff_table = {
        ','.join(str(getattr(r, k) or '') for k in by): r
        for r in diff_results or []}
    names = list(table.keys() | diff_table.keys())

    # sort again, now with diff info, note that python's sort is stable
    names.sort()
    if diff_results is not None:
        names.sort(key=lambda n: tuple(
            types[k].ratio(
                getattr(table.get(n), k, None),
                getattr(diff_table.get(n), k, None))
            for k in fields),
            reverse=True)
    if sort:
        for k, reverse in reversed(sort):
            names.sort(key=lambda n: (getattr(table[n], k),)
                if getattr(table.get(n), k, None) is not None else (),
                reverse=reverse ^ (not k or k in Result._fields))


    # build up our lines
    lines = []

    # header
    line = []
    line.append('%s%s' % (
        ','.join(by),
        ' (%d added, %d removed)' % (
            sum(1 for n in table if n not in diff_table),
            sum(1 for n in diff_table if n not in table))
            if diff_results is not None and not percent else '')
        if not summary else '')
    if diff_results is None:
        for k in fields:
            line.append(k)
    elif percent:
        for k in fields:
            line.append(k)
    else:
        for k in fields:
            line.append('o'+k)
        for k in fields:
            line.append('n'+k)
        for k in fields:
            line.append('d'+k)
    line.append('')
    lines.append(line)

    # entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is not None:
                diff_r = diff_table.get(name)
                ratios = [
                    types[k].ratio(
                        getattr(r, k, None),
                        getattr(diff_r, k, None))
                    for k in fields]
                if not any(ratios) and not all_:
                    continue

            line = []
            line.append(name)
            if diff_results is None:
                for k in fields:
                    line.append(getattr(r, k).table()
                        if getattr(r, k, None) is not None
                        else types[k].none)
            elif percent:
                for k in fields:
                    line.append(getattr(r, k).diff_table()
                        if getattr(r, k, None) is not None
                        else types[k].diff_none)
            else:
                for k in fields:
                    line.append(getattr(diff_r, k).diff_table()
                        if getattr(diff_r, k, None) is not None
                        else types[k].diff_none)
                for k in fields:
                    line.append(getattr(r, k).diff_table()
                        if getattr(r, k, None) is not None
                        else types[k].diff_none)
                for k in fields:
                    line.append(types[k].diff_diff(
                            getattr(r, k, None),
                            getattr(diff_r, k, None)))
            if diff_results is None:
                line.append('')
            elif percent:
                line.append(' (%s)' % ', '.join(
                    '+∞%' if t == +m.inf
                    else '-∞%' if t == -m.inf
                    else '%+.1f%%' % (100*t)
                    for t in ratios))
            else:
                line.append(' (%s)' % ', '.join(
                        '+∞%' if t == +m.inf
                        else '-∞%' if t == -m.inf
                        else '%+.1f%%' % (100*t)
                        for t in ratios
                        if t)
                    if any(ratios) else '')
            lines.append(line)

    # total
    r = next(iter(fold(Result, results, by=[])), None)
    if diff_results is not None:
        diff_r = next(iter(fold(Result, diff_results, by=[])), None)
        ratios = [
            types[k].ratio(
                getattr(r, k, None),
                getattr(diff_r, k, None))
            for k in fields]

    line = []
    line.append('TOTAL')
    if diff_results is None:
        for k in fields:
            line.append(getattr(r, k).table()
                if getattr(r, k, None) is not None
                else types[k].none)
    elif percent:
        for k in fields:
            line.append(getattr(r, k).diff_table()
                if getattr(r, k, None) is not None
                else types[k].diff_none)
    else:
        for k in fields:
            line.append(getattr(diff_r, k).diff_table()
                if getattr(diff_r, k, None) is not None
                else types[k].diff_none)
        for k in fields:
            line.append(getattr(r, k).diff_table()
                if getattr(r, k, None) is not None
                else types[k].diff_none)
        for k in fields:
            line.append(types[k].diff_diff(
                    getattr(r, k, None),
                    getattr(diff_r, k, None)))
    if diff_results is None:
        line.append('')
    elif percent:
        line.append(' (%s)' % ', '.join(
            '+∞%' if t == +m.inf
            else '-∞%' if t == -m.inf
            else '%+.1f%%' % (100*t)
            for t in ratios))
    else:
        line.append(' (%s)' % ', '.join(
                '+∞%' if t == +m.inf
                else '-∞%' if t == -m.inf
                else '%+.1f%%' % (100*t)
                for t in ratios
                if t)
            if any(ratios) else '')
    lines.append(line)

    # find the best widths, note that column 0 contains the names and column -1
    # the ratios, so those are handled a bit differently
    widths = [
        ((max(it.chain([w], (len(l[i]) for l in lines)))+1+4-1)//4)*4-1
        for w, i in zip(
            it.chain([23], it.repeat(7)),
            range(len(lines[0])-1))]

    # print our table
    for line in lines:
        print('%-*s  %s%s' % (
            widths[0], line[0],
            ' '.join('%*s' % (w, x)
                for w, x in zip(widths[1:], line[1:-1])),
            line[-1]))


def main(obj_paths, *,
        by=None,
        fields=None,
        defines=None,
        sort=None,
        **args):
    # find sizes
    if not args.get('use', None):
        # find .o files
        paths = []
        for path in obj_paths:
            if os.path.isdir(path):
                path = path + '/*.o'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print("error: no .obj files found in %r?" % obj_paths)
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f, restval='')
            for r in reader:
                try:
                    results.append(DataResult(
                        **{k: r[k] for k in DataResult._by
                            if k in r and r[k].strip()},
                        **{k: r['data_'+k] for k in DataResult._fields
                            if 'data_'+k in r and r['data_'+k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(DataResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(key=lambda r: (getattr(r, k),)
                if getattr(r, k) is not None else (),
                reverse=reverse ^ (not k or k in DataResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, DataResult._by
                + ['data_'+k for k in DataResult._fields])
            writer.writeheader()
            for r in results:
                writer.writerow(
                    {k: getattr(r, k) for k in DataResult._by}
                    | {'data_'+k: getattr(r, k) for k in DataResult._fields})

    # find previous results?
    if args.get('diff'):
        diff_results = []
        try:
            with openio(args['diff']) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    try:
                        diff_results.append(DataResult(
                            **{k: r[k] for k in DataResult._by
                                if k in r and r[k].strip()},
                            **{k: r['data_'+k] for k in DataResult._fields
                                if 'data_'+k in r and r['data_'+k].strip()}))
                    except TypeError:
                        pass
        except FileNotFoundError:
            pass

        # fold
        diff_results = fold(DataResult, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        table(DataResult, results,
            diff_results if args.get('diff') else None,
            by=by if by is not None else ['function'],
            fields=fields,
            sort=sort,
            **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find data size at the function level.")
    parser.add_argument(
        'obj_paths',
        nargs='*',
        default=OBJ_PATHS,
        help="Description of where to find *.o files. May be a directory "
            "or a list of paths. Defaults to %r." % OBJ_PATHS)
    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument(
        '-q', '--quiet',
        action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument(
        '-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument(
        '-u', '--use',
        help="Don't parse anything, use this CSV file.")
    parser.add_argument(
        '-d', '--diff',
        help="Specify CSV file to diff against.")
    parser.add_argument(
        '-a', '--all',
        action='store_true',
        help="Show all, not just the ones that changed.")
    parser.add_argument(
        '-p', '--percent',
        action='store_true',
        help="Only show percentage change, not a full diff.")
    parser.add_argument(
        '-b', '--by',
        action='append',
        choices=DataResult._by,
        help="Group by this field.")
    parser.add_argument(
        '-f', '--field',
        dest='fields',
        action='append',
        choices=DataResult._fields,
        help="Show this field.")
    parser.add_argument(
        '-D', '--define',
        dest='defines',
        action='append',
        type=lambda x: (lambda k,v: (k, set(v.split(','))))(*x.split('=', 1)),
        help="Only include results where this field is this value.")
    class AppendSort(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.sort is None:
                namespace.sort = []
            namespace.sort.append((value, True if option == '-S' else False))
    parser.add_argument(
        '-s', '--sort',
        action=AppendSort,
        help="Sort by this fields.")
    parser.add_argument(
        '-S', '--reverse-sort',
        action=AppendSort,
        help="Sort by this fields, but backwards.")
    parser.add_argument(
        '-Y', '--summary',
        action='store_true',
        help="Only show the total.")
    parser.add_argument(
        '-A', '--everything',
        action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument(
        '--type',
        default=TYPE,
        help="Type of symbols to report, this uses the same single-character "
            "type-names emitted by nm. Defaults to %r." % TYPE)
    parser.add_argument(
        '--nm-tool',
        type=lambda x: x.split(),
        default=NM_TOOL,
        help="Path to the nm tool to use. Defaults to %r." % NM_TOOL)
    parser.add_argument(
        '--build-dir',
        help="Specify the relative build directory. Used to map object files "
            "to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
