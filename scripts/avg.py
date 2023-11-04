#!/usr/bin/env python3
#
# Compute averages/etc of benchmark measurements
#

import collections as co
import csv
import itertools as it
import math as m
import os


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
        if m.isinf(x) or m.isnan(x):
            raise ValueError("invalid dat %r" % x)
    except ValueError:
        pass

    # else give up
    raise ValueError("invalid dat %r" % x)

def collect(csv_paths, renames=[], defines=[]):
    # collect results from CSV files
    results = []
    for path in csv_paths:
        try:
            with openio(path) as f:
                reader = csv.DictReader(f, restval='')
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

    return results

def main(csv_paths, output, *,
        sum=False,
        prod=False,
        min=False,
        max=False,
        bnd=False,
        avg=False,
        stddev=False,
        gmean=False,
        gstddev=False,
        meas=None,
        by=None,
        seeds=None,
        fields=None,
        defines=[]):
    sum_, sum = sum, __builtins__.sum
    min_, min = min, __builtins__.min
    max_, max = max, __builtins__.max

    # default to averaging
    if (not sum_
            and not prod
            and not min_
            and not max_
            and not bnd
            and not avg
            and not stddev
            and not gmean
            and not gstddev):
        avg = True

    # separate out renames
    renames = list(it.chain.from_iterable(
        ((k, v) for v in vs)
        for k, vs in it.chain(by or [], seeds or [], fields or [])))
    if by is not None:
        by = [k for k, _ in by]
    if seeds is not None:
        seeds = [k for k, _ in seeds]
    if fields is not None:
        fields = [k for k, _ in fields]

    # collect results from csv files
    results = collect(csv_paths, renames, defines)

    # if fields not specified, try to guess from data
    if fields is None:
        fields = co.OrderedDict()
        for r in results:
            for k, v in r.items():
                if k not in (by or []) and k not in (seeds or []) and v.strip():
                    try:
                        dat(v)
                        fields[k] = True
                    except ValueError:
                        fields[k] = False
        fields = list(k for k,v in fields.items() if v)

    # if by not specified, guess it's anything not in seeds/fields and not a
    # source of a rename
    if by is None:
        by = co.OrderedDict()
        for r in results:
            # also ignore None keys, these are introduced by csv.DictReader
            # when header + row mismatch
            by.update((k, True) for k in r.keys()
                if k is not None
                    and k not in (seeds or [])
                    and k not in fields
                    and not any(k == old_k for _, old_k in renames))
        by = list(by.keys())

    # convert fields to ints/floats
    for r in results:
        for k in fields:
            if k in r:
                r[k] = dat(r[k]) if r[k].strip() else 0

    # organize by 'by' values
    results_ = co.defaultdict(lambda: [])
    for r in results:
        key = tuple(r.get(k, '') for k in by)
        results_[key].append(r)
    results = results_

    # for each key calculate the avgs/etc
    avgs = []
    for key, rs in results.items():
        vs = {f: [] for f in fields}
        meas__ = None
        for r in rs:
            if all(k in r and r[k] == v for k, v in zip(by, key)):
                for f in fields:
                    vs[f].append(r.get(f, 0))
                if meas is not None and meas in r:
                    meas__ = r[meas]

        def append(meas_, f_):
            avgs.append(
                {k: v for k, v in zip(by, key)}
                | {f: f_(vs_) for f, vs_ in vs.items()}
                | ({} if meas is None
                    else {meas: meas_} if meas__ is None
                    else {meas: meas__+'+'+meas_}))

        if sum_:    append('sum',     lambda vs: sum(vs))
        if prod:    append('prod',    lambda vs: m.prod(vs))
        if min_:    append('min',     lambda vs: min(vs, default=0))
        if max_:    append('max',     lambda vs: max(vs, default=0))
        if bnd:     append('bnd',     lambda vs: min(vs, default=0))
        if bnd:     append('bnd',     lambda vs: max(vs, default=0))
        if avg:     append('avg',     lambda vs: sum(vs) / max(len(vs), 1))
        if stddev:  append('stddev',  lambda vs: (
                lambda avg: m.sqrt(
                    sum((v - avg)**2 for v in vs) / max(len(vs), 1))
            )(sum(vs) / max(len(vs), 1)))
        if gmean:   append('gmean',   lambda vs:
            m.prod(float(v) for v in vs)**(1 / max(len(vs), 1)))
        if gstddev: append('gstddev', lambda vs: (
            lambda gmean: m.exp(m.sqrt(
                    sum(m.log(v/gmean)**2 for v in vs) / max(len(vs), 1)))
                if gmean else m.inf
            )(m.prod(float(v) for v in vs)**(1 / max(len(vs), 1))))

    # write results to CSVS
    with openio(output, 'w') as f:
        writer = csv.DictWriter(f,
            by + ([meas] if meas not in by else []) + fields)
        writer.writeheader()
        for r in avgs:
            writer.writerow(r)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Compute averages/etc of benchmark measurements.",
        allow_abbrev=False)
    parser.add_argument(
        'csv_paths',
        nargs='*',
        help="Input *.csv files.")
    parser.add_argument(
        '-o', '--output',
        required=True,
        help="*.csv file to write amortized measurements to.")
    parser.add_argument(
        '--sum',
        action='store_true',
        help="Compute the sum.")
    parser.add_argument(
        '--prod',
        action='store_true',
        help="Compute the product.")
    parser.add_argument(
        '--min',
        action='store_true',
        help="Compute the min.")
    parser.add_argument(
        '--max',
        action='store_true',
        help="Compute the max.")
    parser.add_argument(
        '--bnd',
        action='store_true',
        help="Compute the bounds (min+max concatenated).")
    parser.add_argument(
        '--avg', '--mean',
        action='store_true',
        help="Compute the average (the default).")
    parser.add_argument(
        '--stddev',
        action='store_true',
        help="Compute the standard deviation.")
    parser.add_argument(
        '--gmean',
        action='store_true',
        help="Compute the geometric mean.")
    parser.add_argument(
        '--gstddev',
        action='store_true',
        help="Compute the geometric standard deviation.")
    parser.add_argument(
        '-m', '--meas',
        help="Optional name of measurement name field. If provided, the name "
            "will be modified with +amor or +per.")
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
        '-s', '--seed',
        dest='seeds',
        action='append',
        type=lambda x: (
            lambda k, vs=None: (
                k.strip(),
                tuple(v.strip() for v in vs.split(','))
                    if vs is not None else ())
            )(*x.split('=', 1)),
        help="Field to ignore when averaging. Can rename fields with "
            "new_name=old_name.")
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
        help="Field to amortize. Can rename fields with new_name=old_name.")
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
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))

