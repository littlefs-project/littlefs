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
import itertools as it
import math as m
import os
import re


# supported merge operations
#
# this is a terrible way to express these
#
OPS = {
    'sum':     lambda xs: sum(xs[1:], start=xs[0]),
    'prod':    lambda xs: m.prod(xs[1:], start=xs[0]),
    'min':     min,
    'max':     max,
    'avg':     lambda xs: Float(sum(float(x) for x in xs) / len(xs)),
    'stddev':  lambda xs: (
        lambda avg: Float(
            m.sqrt(sum((float(x) - avg)**2 for x in xs) / len(xs)))
        )(sum(float(x) for x in xs) / len(xs)),
    'gmean':   lambda xs: Float(m.prod(float(x) for x in xs)**(1/len(xs))),
    'gstddev': lambda xs: (
        lambda gmean: Float(
            m.exp(m.sqrt(sum(m.log(float(x)/gmean)**2 for x in xs) / len(xs)))
            if gmean else m.inf)
        )(m.prod(float(x) for x in xs)**(1/len(xs))),
}


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

# float fields
class Float(co.namedtuple('Float', 'x')):
    __slots__ = ()
    def __new__(cls, x=0.0):
        if isinstance(x, Float):
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

    none = Int.none
    table = Int.table
    diff_none = Int.diff_none
    diff_table = Int.diff_table

    def diff_diff(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        diff = new - old
        if diff == +m.inf:
            return '%7s' % '+∞'
        elif diff == -m.inf:
            return '%7s' % '-∞'
        else:
            return '%+7.1f' % diff

    ratio = Int.ratio
    __add__ = Int.__add__
    __sub__ = Int.__sub__
    __mul__ = Int.__mul__

# fractional fields, a/b
class Frac(co.namedtuple('Frac', 'a,b')):
    __slots__ = ()
    def __new__(cls, a=0, b=None):
        if isinstance(a, Frac) and b is None:
            return a
        if isinstance(a, str) and b is None:
            a, b = a.split('/', 1)
        if b is None:
            b = a
        return super().__new__(cls, Int(a), Int(b))

    def __str__(self):
        return '%s/%s' % (self.a, self.b)

    def __float__(self):
        return float(self.a)

    none = '%11s %7s' % ('-', '-')
    def table(self):
        t = self.a.x/self.b.x if self.b.x else 1.0
        return '%11s %7s' % (
            self,
            '∞%' if t == +m.inf
            else '-∞%' if t == -m.inf
            else '%.1f%%' % (100*t))

    diff_none = '%11s' % '-'
    def diff_table(self):
        return '%11s' % (self,)

    def diff_diff(self, other):
        new_a, new_b = self if self else (Int(0), Int(0))
        old_a, old_b = other if other else (Int(0), Int(0))
        return '%11s' % ('%s/%s' % (
            new_a.diff_diff(old_a).strip(),
            new_b.diff_diff(old_b).strip()))

    def ratio(self, other):
        new_a, new_b = self if self else (Int(0), Int(0))
        old_a, old_b = other if other else (Int(0), Int(0))
        new = new_a.x/new_b.x if new_b.x else 1.0
        old = old_a.x/old_b.x if old_b.x else 1.0
        return new - old

    def __add__(self, other):
        return self.__class__(self.a + other.a, self.b + other.b)

    def __sub__(self, other):
        return self.__class__(self.a - other.a, self.b - other.b)

    def __mul__(self, other):
        return self.__class__(self.a * other.a, self.b + other.b)

    def __lt__(self, other):
        self_t = self.a.x/self.b.x if self.b.x else 1.0
        other_t = other.a.x/other.b.x if other.b.x else 1.0
        return (self_t, self.a.x) < (other_t, other.a.x)

    def __gt__(self, other):
        return self.__class__.__lt__(other, self)

    def __le__(self, other):
        return not self.__gt__(other)

    def __ge__(self, other):
        return not self.__lt__(other)

# available types
TYPES = co.OrderedDict([
    ('int',   Int),
    ('float', Float),
    ('frac',  Frac)
])


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

def collect(csv_paths, renames=[], defines=[]):
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
                    # apply any renames
                    if renames:
                        # make a copy so renames can overlap
                        r_ = {}
                        for new_k, old_k in renames:
                            if old_k in r:
                                r_[new_k] = r[old_k]
                        r.update(r_)

                    # filter by matching defines
                    if not all(k in r and r[k] in vs for k, vs in defines):
                        continue

                    results.append(r)
        except FileNotFoundError:
            pass

    return fields, results

def infer(fields_, results,
        by=None,
        fields=None,
        types={},
        ops={},
        renames=[],
        defines=[]):
    # if by not specified, guess it's anything not in fields/renames/defines
    if by is None:
        by = [
            k for k in fields_
            if k not in (fields or [])
                and not any(k == old_k for _, old_k in renames)
                and not any(k == k_ for k_, _ in defines)]

    # if fields not specified, guess it's anything not in by/renames/defines
    if fields is None:
        fields = [
            k for k in fields_
            if k not in (by or [])
                and not any(k == old_k for _, old_k in renames)
                and not any(k == k_ for k_, _ in defines)]

    # deduplicate by/fields
    by = list(co.OrderedDict.fromkeys(by).keys())
    fields = list(co.OrderedDict.fromkeys(fields).keys())

    # find best type for all fields
    types_ = {}
    for k in fields:
        if k in types:
            types_[k] = types[k]
        else:
            for t in TYPES.values():
                for r in results:
                    if k in r and r[k].strip():
                        try:
                            t(r[k])
                        except ValueError:
                            break
                else:
                    types_[k] = t
                    break
            else:
                print("error: no type matches field %r?" % k,
                    file=sys.stderr)
                sys.exit(-1)
    types = types_

    # does folding change the type?
    types_ = {}
    for k, t in types.items():
        types_[k] = ops.get(k, OPS['sum'])([t()]).__class__


    # create result class
    def __new__(cls, **r):
        return cls.__mro__[1].__new__(cls,
            **{k: r.get(k, '') for k in by},
            **{k: r[k] if k in r and isinstance(r[k], tuple)
                else ([types[k](r[k])], 1) if k in r
                else ([], 0)
                for k in fields})

    def __add__(self, other):
        # reuse lists if possible
        def extend(a, b):
            if len(a[0]) == a[1]:
                a[0].extend(b[0][:b[1]])
                return (a[0], a[1] + b[1])
            else:
                return (a[0][:a[1]] + b[0][:b[1]], a[1] + b[1])

        return self.__class__(
            **{k: getattr(self, k) for k in by},
            **{k: extend(
                    object.__getattribute__(self, k),
                    object.__getattribute__(other, k))
                for k in fields})

    def __getattribute__(self, k):
        if k in fields:
            v = object.__getattribute__(self, k)
            if v[1]:
                return ops.get(k, OPS['sum'])(v[0][:v[1]])
            else:
                return None
        return object.__getattribute__(self, k)

    return type('Result', (co.namedtuple('Result', by + fields),), {
        '__slots__': (),
        '__new__': __new__,
        '__add__': __add__,
        '__getattribute__': __getattribute__,
        '_by': by,
        '_fields': fields,
        '_sort': fields,
        '_types': types_,
    })


def fold(Result, results, by=None, defines=[]):
    if by is None:
        by = Result._by

    for k in it.chain(by or [], (k for k, _ in defines)):
        if k not in Result._by and k not in Result._fields:
            print("error: could not find field %r?" % k,
                file=sys.stderr)
            sys.exit(-1)

    # filter by matching defines
    if defines:
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
            names.sort(
                key=lambda n: tuple(
                    (getattr(table[n], k),)
                    if getattr(table.get(n), k, None) is not None else ()
                    for k in ([k] if k else [
                        k for k in Result._sort if k in fields])),
                reverse=reverse ^ (not k or k in Result._fields))


    # build up our lines
    lines = []

    # header
    header = []
    header.append('%s%s' % (
        ','.join(by),
        ' (%d added, %d removed)' % (
            sum(1 for n in table if n not in diff_table),
            sum(1 for n in diff_table if n not in table))
            if diff_results is not None and not percent else '')
        if not summary else '')
    if diff_results is None:
        for k in fields:
            header.append(k)
    elif percent:
        for k in fields:
            header.append(k)
    else:
        for k in fields:
            header.append('o'+k)
        for k in fields:
            header.append('n'+k)
        for k in fields:
            header.append('d'+k)
    header.append('')
    lines.append(header)

    def table_entry(name, r, diff_r=None, ratios=[]):
        entry = []
        entry.append(name)
        if diff_results is None:
            for k in fields:
                entry.append(getattr(r, k).table()
                    if getattr(r, k, None) is not None
                    else types[k].none)
        elif percent:
            for k in fields:
                entry.append(getattr(r, k).diff_table()
                    if getattr(r, k, None) is not None
                    else types[k].diff_none)
        else:
            for k in fields:
                entry.append(getattr(diff_r, k).diff_table()
                    if getattr(diff_r, k, None) is not None
                    else types[k].diff_none)
            for k in fields:
                entry.append(getattr(r, k).diff_table()
                    if getattr(r, k, None) is not None
                    else types[k].diff_none)
            for k in fields:
                entry.append(types[k].diff_diff(
                        getattr(r, k, None),
                        getattr(diff_r, k, None)))
        if diff_results is None:
            entry.append('')
        elif percent:
            entry.append(' (%s)' % ', '.join(
                '+∞%' if t == +m.inf
                else '-∞%' if t == -m.inf
                else '%+.1f%%' % (100*t)
                for t in ratios))
        else:
            entry.append(' (%s)' % ', '.join(
                    '+∞%' if t == +m.inf
                    else '-∞%' if t == -m.inf
                    else '%+.1f%%' % (100*t)
                    for t in ratios
                    if t)
                if any(ratios) else '')
        return entry

    # entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is None:
                diff_r = None
                ratios = None
            else:
                diff_r = diff_table.get(name)
                ratios = [
                    types[k].ratio(
                        getattr(r, k, None),
                        getattr(diff_r, k, None))
                    for k in fields]
                if not all_ and not any(ratios):
                    continue
            lines.append(table_entry(name, r, diff_r, ratios))

    # total
    r = next(iter(fold(Result, results, by=[])), None)
    if diff_results is None:
        diff_r = None
        ratios = None
    else:
        diff_r = next(iter(fold(Result, diff_results, by=[])), None)
        ratios = [
            types[k].ratio(
                getattr(r, k, None),
                getattr(diff_r, k, None))
            for k in fields]
    lines.append(table_entry('TOTAL', r, diff_r, ratios))

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


def main(csv_paths, *,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        **args):
    # separate out renames
    renames = list(it.chain.from_iterable(
        ((k, v) for v in vs)
        for k, vs in it.chain(by or [], fields or [])))
    if by is not None:
        by = [k for k, _ in by]
    if fields is not None:
        fields = [k for k, _ in fields]

    # figure out types
    types = {}
    for t in TYPES.keys():
        for k in args.get(t, []):
            if k in types:
                print("error: conflicting type for field %r?" % k,
                    file=sys.stderr)
                sys.exit(-1)
            types[k] = TYPES[t]
    # rename types?
    if renames:
        types_ = {}
        for new_k, old_k in renames:
            if old_k in types:
                types_[new_k] = types[old_k]
        types.update(types_)

    # figure out merge operations
    ops = {}
    for o in OPS.keys():
        for k in args.get(o, []):
            if k in ops:
                print("error: conflicting op for field %r?" % k,
                    file=sys.stderr)
                sys.exit(-1)
            ops[k] = OPS[o]
    # rename ops?
    if renames:
        ops_ = {}
        for new_k, old_k in renames:
            if old_k in ops:
                ops_[new_k] = ops[old_k]
        ops.update(ops_)

    if by is None and fields is None:
        print("error: needs --by or --fields to figure out fields",
            file=sys.stderr)
        sys.exit(-1)

    # find CSV files
    fields_, results = collect(csv_paths, renames, defines)

    # homogenize
    Result = infer(fields_, results,
        by=by,
        fields=fields,
        types=types,
        ops=ops,
        renames=renames,
        defines=defines)
    results_ = []
    for r in results:
        if not any(k in r and r[k].strip()
                for k in Result._fields):
            continue
        try:
            results_.append(Result(**{
                k: r[k] for k in Result._by + Result._fields
                if k in r and r[k].strip()}))
        except TypeError:
            pass
    results = results_

    # fold
    results = fold(Result, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(
                key=lambda r: tuple(
                    (getattr(r, k),) if getattr(r, k) is not None else ()
                    for k in ([k] if k else Result._sort)),
                reverse=reverse ^ (not k or k in Result._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, Result._by + Result._fields)
            writer.writeheader()
            for r in results:
                # note we need to go through getattr to resolve lazy fields
                writer.writerow({
                    k: getattr(r, k) for k in Result._by + Result._fields})

    # find previous results?
    if args.get('diff'):
        _, diff_results = collect([args['diff']], renames, defines)
        diff_results_ = []
        for r in diff_results:
            if not any(k in r and r[k].strip()
                    for k in Result._fields):
                continue
            try:
                diff_results_.append(Result(**{
                    k: r[k] for k in Result._by + Result._fields
                    if k in r and r[k].strip()}))
            except TypeError:
                pass
        diff_results = diff_results_

        # fold
        diff_results = fold(Result, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        table(Result, results,
            diff_results if args.get('diff') else None,
            by=by,
            fields=fields,
            sort=sort,
            **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Summarize measurements in CSV files.",
        allow_abbrev=False)
    parser.add_argument(
        'csv_paths',
        nargs='*',
        help="Input *.csv files.")
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
        type=lambda x: (
            lambda k, vs=None: (
                k.strip(),
                tuple(v.strip() for v in vs.split(','))
                    if vs is not None else ())
            )(*x.split('=', 1)),
        help="Group by this field. Can rename fields with new_name=old_name.")
    parser.add_argument(
        '-f', '--field',
        dest='fields',
        action='append',
        type=lambda x: (
            lambda k, vs=None: (
                k.strip(),
                tuple(v.strip() for v in vs.split(','))
                    if vs is not None else ())
            )(*x.split('=', 1)),
        help="Show this field. Can rename fields with new_name=old_name.")
    parser.add_argument(
        '-D', '--define',
        dest='defines',
        action='append',
        type=lambda x: (
            lambda k, vs: (
                k.strip(),
                {v.strip() for v in vs.split(',')})
            )(*x.split('=', 1)),
        help="Only include results where this field is this value. May include "
            "comma-separated options.")
    class AppendSort(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.sort is None:
                namespace.sort = []
            namespace.sort.append((value, True if option == '-S' else False))
    parser.add_argument(
        '-s', '--sort',
        nargs='?',
        action=AppendSort,
        help="Sort by this field.")
    parser.add_argument(
        '-S', '--reverse-sort',
        nargs='?',
        action=AppendSort,
        help="Sort by this field, but backwards.")
    parser.add_argument(
        '-Y', '--summary',
        action='store_true',
        help="Only show the total.")
    parser.add_argument(
        '--int',
        action='append',
        help="Treat these fields as ints.")
    parser.add_argument(
        '--float',
        action='append',
        help="Treat these fields as floats.")
    parser.add_argument(
        '--frac',
        action='append',
        help="Treat these fields as fractions.")
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
        '--avg', '--mean',
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
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
