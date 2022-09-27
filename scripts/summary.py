#!/usr/bin/env python3
#
# Script to summarize the outputs of other scripts. Operates on CSV files.
#
# Example:
# ./scripts/code.py lfs.o lfs_util.o -q -o lfs.code.csv
# ./scripts/data.py lfs.o lfs_util.o -q -o lfs.data.csv
# ./scripts/summary.py lfs.code.csv lfs.data.csv -q -o lfs.csv
# ./scripts/summary.py -Y lfs.csv -f code=code_size,data=data_size
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

import collections as co
import csv
import functools as ft
import glob
import itertools as it
import math as m
import os
import re


CSV_PATHS = ['*.csv']

# supported merge operations
#
# this is a terrible way to express these
#
OPS = {
    'sum':     lambda xs: sum(xs[1:], start=xs[0]),
    'prod':    lambda xs: m.prod(xs[1:], start=xs[0]),
    'min':     min,
    'max':     max,
    'mean':    lambda xs: FloatField(sum(float(x) for x in xs) / len(xs)),
    'stddev':  lambda xs: (
        lambda mean: FloatField(
            m.sqrt(sum((float(x) - mean)**2 for x in xs) / len(xs)))
        )(sum(float(x) for x in xs) / len(xs)),
    'gmean':   lambda xs: FloatField(m.prod(float(x) for x in xs)**(1/len(xs))),
    'gstddev': lambda xs: (
        lambda gmean: FloatField(
            m.exp(m.sqrt(sum(m.log(float(x)/gmean)**2 for x in xs) / len(xs)))
            if gmean else m.inf)
        )(m.prod(float(x) for x in xs)**(1/len(xs)))

}


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)


# integer fields
class IntField(co.namedtuple('IntField', 'x')):
    __slots__ = ()
    def __new__(cls, x=0):
        if isinstance(x, IntField):
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
        return IntField(self.x + other.x)

    def __sub__(self, other):
        return IntField(self.x - other.x)

    def __mul__(self, other):
        return IntField(self.x * other.x)

    def __lt__(self, other):
        return self.x < other.x

    def __gt__(self, other):
        return self.__class__.__lt__(other, self)

    def __le__(self, other):
        return not self.__gt__(other)

    def __ge__(self, other):
        return not self.__lt__(other)

# float fields
class FloatField(co.namedtuple('FloatField', 'x')):
    __slots__ = ()
    def __new__(cls, x=0.0):
        if isinstance(x, FloatField):
            return x
        if isinstance(x, str):
            try:
                x = float(x)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', x):
                    x = m.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', x):
                    x = -m.inf
                else:
                    raise
        assert isinstance(x, float), x
        return super().__new__(cls, x)

    def __str__(self):
        if self.x == m.inf:
            return '∞'
        elif self.x == -m.inf:
            return '-∞'
        else:
            return '%.1f' % self.x

    def __float__(self):
        return float(self.x)

    none = IntField.none
    table = IntField.table
    diff_none = IntField.diff_none
    diff_table = IntField.diff_table
    diff_diff = IntField.diff_diff
    ratio = IntField.ratio
    __add__ = IntField.__add__
    __sub__ = IntField.__sub__
    __mul__ = IntField.__mul__
    __lt__ = IntField.__lt__
    __gt__ = IntField.__gt__
    __le__ = IntField.__le__
    __ge__ = IntField.__ge__

# fractional fields, a/b
class FracField(co.namedtuple('FracField', 'a,b')):
    __slots__ = ()
    def __new__(cls, a=0, b=None):
        if isinstance(a, FracField) and b is None:
            return a
        if isinstance(a, str) and b is None:
            a, b = a.split('/', 1)
        if b is None:
            b = a
        return super().__new__(cls, IntField(a), IntField(b))

    def __str__(self):
        return '%s/%s' % (self.a, self.b)

    def __float__(self):
        return float(self.a)

    none = '%11s %7s' % ('-', '-')
    def table(self):
        if not self.b.x:
            return self.none

        t = self.a.x/self.b.x
        return '%11s %7s' % (
            self,
            '∞%' if t == +m.inf
            else '-∞%' if t == -m.inf
            else '%.1f%%' % (100*t))

    diff_none = '%11s' % '-'
    def diff_table(self):
        if not self.b.x:
            return self.diff_none

        return '%11s' % (self,)

    def diff_diff(self, other):
        new_a, new_b = self if self else (IntField(0), IntField(0))
        old_a, old_b = other if other else (IntField(0), IntField(0))
        return '%11s' % ('%s/%s' % (
            new_a.diff_diff(old_a).strip(),
            new_b.diff_diff(old_b).strip()))

    def ratio(self, other):
        new_a, new_b = self if self else (IntField(0), IntField(0))
        old_a, old_b = other if other else (IntField(0), IntField(0))
        new = new_a.x/new_b.x if new_b.x else 1.0
        old = old_a.x/old_b.x if old_b.x else 1.0
        return new - old

    def __add__(self, other):
        return FracField(self.a + other.a, self.b + other.b)

    def __sub__(self, other):
        return FracField(self.a - other.a, self.b - other.b)

    def __mul__(self, other):
        return FracField(self.a * other.a, self.b + other.b)

    def __lt__(self, other):
        self_r = self.a.x/self.b.x if self.b.x else -m.inf
        other_r = other.a.x/other.b.x if other.b.x else -m.inf
        return self_r < other_r

    def __gt__(self, other):
        return self.__class__.__lt__(other, self)

    def __le__(self, other):
        return not self.__gt__(other)

    def __ge__(self, other):
        return not self.__lt__(other)

# available types
TYPES = [IntField, FloatField, FracField]


def homogenize(results, *,
        by=None,
        fields=None,
        renames=[],
        define={},
        types=None,
        **_):
    results = results.copy()

    # rename fields?
    if renames:
        for r in results:
            # make a copy so renames can overlap
            r_ = {}
            for new_k, old_k in renames:
                if old_k in r:
                    r_[new_k] = r[old_k]
            r.update(r_)

    # filter by matching defines
    if define:
        results_ = []
        for r in results:
            if all(k in r and r[k] in vs for k, vs in define):
                results_.append(r)
        results = results_

    # if fields not specified, try to guess from data
    if fields is None:
        fields = co.OrderedDict()
        for r in results:
            for k, v in r.items():
                if by is not None and k in by:
                    continue
                types_ = []
                for type in fields.get(k, TYPES):
                    try:
                        type(v)
                        types_.append(type)
                    except ValueError:
                        pass
                fields[k] = types_
        fields = list(k for k,v in fields.items() if v)

    # infer 'by' fields?
    if by is None:
        by = co.OrderedDict()
        for r in results:
            # also ignore None keys, these are introduced by csv.DictReader
            # when header + row mismatch
            by.update((k, True) for k in r.keys()
                if k is not None
                    and k not in fields
                    and not any(k == old_k for _, old_k in renames))
        by = list(by.keys())

    # go ahead and clean up none values, these can have a few forms
    results_ = []
    for r in results:
        results_.append({
            k: r[k] for k in it.chain(by, fields)
            if r.get(k) is not None and not (
                isinstance(r[k], str)
                and re.match('^\s*[+-]?\s*$', r[k]))})
    results = results_

    # find best type for all fields
    if types is None:
        def is_type(x, type):
            try:
                type(x)
                return True
            except ValueError:
                return False

        types = {}
        for k in fields:
            for type in TYPES:
                if all(k not in r or is_type(r[k], type) for r in results_):
                    types[k] = type
                    break
            else:
                print("no type matches field %r?" % k)
                sys.exit(-1)

    # homogenize types
    for r in results:
        for k in fields:
            if k in r:
                r[k] = types[k](r[k])

    return by, fields, types, results


def fold(results, *,
        by=[],
        fields=[],
        types=None,
        ops={},
        **_):
    folding = co.OrderedDict()
    for r in results:
        name = tuple(r.get(k, '') for k in by)
        if name not in folding:
            folding[name] = {k: [] for k in fields}
        for k in fields:
            if k in r:
                folding[name][k].append(r[k])

    # merge fields, we need the count at this point for averages
    folded = []
    for name, r in folding.items():
        r_ = {}
        for k, vs in r.items():
            if vs:
                # sum fields by default
                op = OPS[ops.get(k, 'sum')]
                r_[k] = op(vs)

        # drop any rows without fields and any empty keys
        if r_:
            folded.append(dict(
                {k: v for k, v in zip(by, name) if v},
                **r_))

    # what is the type of merged fields?
    if types is not None:
        types_ = {}
        for k in fields:
            op = OPS[ops.get(k, 'sum')]
            types_[k] = op([types[k]()]).__class__

    if types is None:
        return folded
    else:
        return types_, folded


def table(results, total, diff_results=None, diff_total=None, *,
        by=[],
        fields=[],
        types={},
        ops={},
        sort=None,
        reverse_sort=None,
        summary=False,
        all=False,
        percent=False,
        **_):
    all_, all = all, __builtins__.all

    table = {
        ','.join(r.get(k,'') for k in by): r
        for r in results}
    diff_table = {
        ','.join(r.get(k,'') for k in by): r
        for r in diff_results or []}

    # sort, note that python's sort is stable
    names = list(table.keys() | diff_table.keys())
    names.sort()
    if diff_results is not None:
        names.sort(key=lambda n: tuple(
            -types[k].ratio(
                table.get(n,{}).get(k),
                diff_table.get(n,{}).get(k))
            for k in fields))
    if sort:
        names.sort(key=lambda n: tuple(
            (table[n][k],) if k in table.get(n,{}) else ()
            for k in sort),
            reverse=True)
    elif reverse_sort:
        names.sort(key=lambda n: tuple(
            (table[n][k],) if k in table.get(n,{}) else ()
            for k in reverse_sort),
            reverse=False)

    # print header
    if not summary:
        title = '%s%s' % (
            ','.join(by),
            ' (%d added, %d removed)' % (
                sum(1 for n in table if n not in diff_table),
                sum(1 for n in diff_table if n not in table))
                if diff_results is not None and not percent else '')
        name_width = max(it.chain([23, len(title)], (len(n) for n in names)))
    else:
        title = ''
        name_width = 23
    name_width = 4*((name_width+1+4-1)//4)-1

    print('%-*s ' % (name_width, title), end='')
    if diff_results is None:
        widths = [
            4*((max(len(types[k].none), len(k))+1+4-1)//4)-1
            for k in fields]
        print(' %s' % (
            ' '.join(k.rjust(w) for w, k in zip(widths, fields))))
    elif percent:
        widths = [
            4*((max(len(types[k].diff_none), len(k))+1+4-1)//4)-1
            for k in fields]
        print(' %s' % (
            ' '.join(k.rjust(w) for w, k in zip(widths, fields))))
    else:
        widths = [
            4*((max(len(types[k].diff_none), 1+len(k))+1+4-1)//4)-1
            for k in fields]
        print(' %s %s %s' % (
            ' '.join(('o'+k).rjust(w) for w, k in zip(widths, fields)),
            ' '.join(('n'+k).rjust(w) for w, k in zip(widths, fields)),
            ' '.join(('d'+k).rjust(w) for w, k in zip(widths, fields))))

    # print entries
    if not summary:
        for name in names:
            r = table.get(name, {})
            if diff_results is not None:
                diff_r = diff_table.get(name, {})
                ratios = [types[k].ratio(r.get(k), diff_r.get(k))
                    for k in fields]
                if not any(ratios) and not all_:
                    continue

            print('%-*s ' % (name_width, name), end='')
            if diff_results is None:
                print(' %s' % (
                    ' '.join(
                        (r[k].table()
                            if k in r
                            else types[k].none).rjust(w)
                        for w, k in zip(widths, fields))))
            elif percent:
                print(' %s%s' % (
                    ' '.join(
                        (r[k].diff_table().rjust(w)
                            if k in r
                            else types[k].diff_none).rjust(w)
                        for w, k in zip(widths, fields)),
                    ' (%s)' % ', '.join(
                            '+∞%' if t == +m.inf
                            else '-∞%' if t == -m.inf
                            else '%+.1f%%' % (100*t)
                            for t in ratios)))
            else:
                print(' %s %s %s%s' % (
                    ' '.join(
                        (diff_r[k].diff_table()
                            if k in diff_r
                            else types[k].diff_none).rjust(w)
                        for w, k in zip(widths, fields)),
                    ' '.join(
                        (r[k].diff_table()
                            if k in r
                            else types[k].diff_none).rjust(w)
                        for w, k in zip(widths, fields)),
                    ' '.join(
                        (types[k].diff_diff(r.get(k), diff_r.get(k))
                            if k in r or k in diff_r
                            else types[k].diff_none).rjust(w)
                        for w, k in zip(widths, fields)),
                    ' (%s)' % ', '.join(
                            '+∞%' if t == +m.inf
                            else '-∞%' if t == -m.inf
                            else '%+.1f%%' % (100*t)
                            for t in ratios
                            if t)
                        if any(ratios) else ''))

    # print total
    r = total
    if diff_total is not None:
        diff_r = diff_total
        ratios = [types[k].ratio(r.get(k), diff_r.get(k))
            for k in fields]

    print('%-*s ' % (name_width, 'TOTAL'), end='')
    if diff_results is None:
        print(' %s' % (
            ' '.join(
                (r[k].table()
                    if k in r
                    else types[k].none).rjust(w)
                for w, k in zip(widths, fields))))
    elif percent:
        print(' %s%s' % (
            ' '.join(
                (r[k].diff_table().rjust(w)
                    if k in r
                    else types[k].diff_none).rjust(w)
                for w, k in zip(widths, fields)),
            ' (%s)' % ', '.join(
                    '+∞%' if t == +m.inf
                    else '-∞%' if t == -m.inf
                    else '%+.1f%%' % (100*t)
                    for t in ratios)))
    else:
        print(' %s %s %s%s' % (
            ' '.join(
                (diff_r[k].diff_table()
                    if k in diff_r
                    else types[k].diff_none).rjust(w)
                for w, k in zip(widths, fields)),
            ' '.join(
                (r[k].diff_table()
                    if k in r
                    else types[k].diff_none).rjust(w)
                for w, k in zip(widths, fields)),
            ' '.join(
                (types[k].diff_diff(r.get(k), diff_r.get(k))
                    if k in r or k in diff_r
                    else types[k].diff_none).rjust(w)
                for w, k in zip(widths, fields)),
            ' (%s)' % ', '.join(
                    '+∞%' if t == +m.inf
                    else '-∞%' if t == -m.inf
                    else '%+.1f%%' % (100*t)
                    for t in ratios
                    if t)
                if any(ratios) else ''))


def main(csv_paths, *,
        by=None,
        fields=None,
        define=[],
        **args):
    # separate out renames
    renames = [k.split('=', 1)
        for k in it.chain(by or [], fields or [])
        if '=' in k]
    if by is not None:
        by = [k.split('=', 1)[0] for k in by]
    if fields is not None:
        fields = [k.split('=', 1)[0] for k in fields]

    # figure out merge operations
    ops = {}
    for m in OPS.keys():
        for k in args.get(m, []):
            if k in ops:
                print("conflicting op for field %r?" % k)
                sys.exit(-1)
            ops[k] = m
    # rename ops?
    if renames:
        ops_ = {}
        for new_k, old_k in renames:
            if old_k in ops:
                ops_[new_k] = ops[old_k]
        ops.update(ops_)

    # find CSV files
    paths = []
    for path in csv_paths:
        if os.path.isdir(path):
            path = path + '/*.csv'

        for path in glob.glob(path):
            paths.append(path)

    if not paths:
        print('no .csv files found in %r?' % csv_paths)
        sys.exit(-1)

    results = []
    for path in paths:
        try:
            with openio(path) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    results.append(r)
        except FileNotFoundError:
            pass

    # homogenize
    by, fields, types, results = homogenize(results,
        by=by, fields=fields, renames=renames, define=define)

    # fold for total, note we do this with the raw data to avoid
    # issues with lossy operations
    total = fold(results, fields=fields, ops=ops)
    total = total[0] if total else {}

    # fold to remove duplicates
    types_, results = fold(results, by=by, fields=fields, types=types, ops=ops)

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, by + fields)
            writer.writeheader()
            for r in results:
                writer.writerow(r)

    # find previous results?
    if args.get('diff'):
        diff_results = []
        try:
            with openio(args['diff']) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    diff_results.append(r)
        except FileNotFoundError:
            pass

        # homogenize
        _, _, _, diff_results = homogenize(diff_results,
            by=by, fields=fields, renames=renames, define=define, types=types)

        # fold for total, note we do this with the raw data to avoid
        # issues with lossy operations
        diff_total = fold(diff_results, fields=fields, ops=ops)
        diff_total = diff_total[0] if diff_total else {}

        # fold to remove duplicates
        diff_results = fold(diff_results, by=by, fields=fields, ops=ops)

    # print table
    if not args.get('quiet'):
        table(
            results,
            total,
            diff_results if args.get('diff') else None,
            diff_total if args.get('diff') else None,
            by=by,
            fields=fields,
            types=types_,
            ops=ops,
            **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Summarize measurements in CSV files.")
    parser.add_argument(
        'csv_paths',
        nargs='*',
        default=CSV_PATHS,
        help="Description of where to find *.csv files. May be a directory "
            "or list of paths. Defaults to %r." % CSV_PATHS)
    parser.add_argument(
        '-q', '--quiet',
        action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument(
        '-o', '--output',
        help="Specify CSV file to store results.")
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
        help="Group by these fields. All other fields will be merged as "
            "needed. Can rename fields with new_name=old_name.")
    parser.add_argument(
        '-f', '--fields',
        action='append',
        help="Use these fields. Can rename fields with new_name=old_name.")
    parser.add_argument(
        '-D', '--define',
        action='append',
        type=lambda x: (lambda k,v: (k, set(v.split(','))))(*x.split('=', 1)),
        help="Only include rows where this field is this value. May include "
            "comma-separated options.")
    parser.add_argument(
        '--sum',
        action='append',
        help="Add these fields (the default).")
    parser.add_argument(
        '--prod',
        action='append',
        help="Multiply these fields.")
    parser.add_argument(
        '--min',
        action='append',
        help="Take the minimum of these fields.")
    parser.add_argument(
        '--max',
        action='append',
        help="Take the maximum of these fields.")
    parser.add_argument(
        '--mean',
        action='append',
        help="Average these fields.")
    parser.add_argument(
        '--stddev',
        action='append',
        help="Find the standard deviation of these fields.")
    parser.add_argument(
        '--gmean',
        action='append',
        help="Find the geometric mean of these fields.")
    parser.add_argument(
        '--gstddev',
        action='append',
        help="Find the geometric standard deviation of these fields.")
    parser.add_argument(
        '-s', '--sort',
        action='append',
        help="Sort by these fields.")
    parser.add_argument(
        '-S', '--reverse-sort',
        action='append',
        help="Sort by these fields, but backwards.")
    parser.add_argument(
        '-Y', '--summary',
        action='store_true',
        help="Only show the totals.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
