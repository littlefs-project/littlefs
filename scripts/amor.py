#!/usr/bin/env python3
#
# Amortize benchmark measurements
#

import collections as co
import csv
import itertools as it
import math as mt
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
        if mt.isinf(x) or mt.isnan(x):
            raise ValueError("invalid dat %r" % x)
    except ValueError:
        pass

    # else give up
    raise ValueError("invalid dat %r" % x)

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

def main(csv_paths, output, *,
        amor=False,
        per=False,
        by=None,
        meas=None,
        iter=None,
        size=None,
        fields=None,
        defines=[]):
    # default to amortizing
    if not amor and not per:
        amor = True

    # separate out renames
    renames = list(it.chain.from_iterable(
            ((k, v) for v in vs)
            for k, vs in it.chain(by or [], fields or [])))
    if by is not None:
        by = [k for k, _ in by]
    if fields is not None:
        fields = [k for k, _ in fields]

    if by is None and fields is None:
        print("error: needs --by or --fields to figure out fields",
                file=sys.stderr)
        sys.exit(-1)

    # collect results from csv files
    fields_, results = collect(csv_paths, renames, defines)

    # if by not specified, guess it's anything not in
    # iter/size/fields/renames/defines
    if by is None:
        by = [k for k in fields_
                if k != iter
                    and k != size
                    and k not in (fields or [])
                    and not any(k == old_k for _, old_k in renames)
                    and not any(k == k_ for k_, _ in defines)]

    # if fields not specified, guess it's anything not in
    # by/iter/size/renames/defines
    if fields is None:
        fields = [k for k in fields_
                if k not in (by or [])
                    and k != iter
                    and k != size
                    and not any(k == old_k for _, old_k in renames)
                    and not any(k == k_ for k_, _ in defines)]

    # add meas to by if it isn't already present
    if meas is not None and meas not in by:
        by.append(meas)

    # convert iter/fields to ints/floats
    for r in results:
        for k in it.chain([iter], [size] if size is not None else [], fields):
            if k in r and isinstance(r[k], str):
                r[k] = dat(r[k]) if r[k].strip() else 0

    # organize by 'by' values
    results_ = co.defaultdict(lambda: [])
    for r in results:
        key = tuple(r.get(k, '') for k in by)
        results_[key].append(r)
    results = results_

    # for each key compute the amortized results
    amors = []
    for key, rs in results.items():
        # keep a running sum for each field
        sums = {f: 0 for f in fields}
        size_ = 0
        for j, (i, r) in enumerate(sorted(
                ((r.get(iter, 0), r) for r in rs),
                key=lambda p: p[0])):
            # update sums
            for f in fields:
                sums[f] += r.get(f, 0)
            size_ += r.get(size, 1)

            # find amortized results
            if amor:
                amors.append(r
                        | {f: sums[f] / size_ for f in fields}
                        | ({} if meas is None
                            else {meas: r[meas]+'+amor'} if meas in r
                            else {meas: 'amor'}))

            # also find per-byte results
            if per:
                amors.append(r
                        | {f: r.get(f, 0) / size_ for f in fields}
                        | ({} if meas is None
                            else {meas: r[meas]+'+per'} if meas in r
                            else {meas: 'per'}))

    # write results to CSV
    with openio(output, 'w') as f:
        writer = csv.DictWriter(f,
                by + [iter] + ([size] if size is not None else []) + fields)
        writer.writeheader()
        for r in amors:
            writer.writerow(r)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Amortize benchmark measurements.",
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
            '--amor',
            action='store_true',
            help="Compute amortized results.")
    parser.add_argument(
            '--per',
            action='store_true',
            help="Compute per-byte results.")
    parser.add_argument(
            '-b', '--by',
            action='append',
            type=lambda x: (
                lambda k, vs=None: (
                    k.strip(),
                    tuple(v.strip() for v in vs.split(','))
                        if vs is not None else ())
                )(*x.split('=', 1)),
            help="Group by this field. Can rename fields with "
                "new_name=old_name.")
    parser.add_argument(
            '-m', '--meas',
            help="Optional name of measurement name field. If provided, the "
                "name will be modified with +amor or +per.")
    parser.add_argument(
            '-i', '--iter',
            required=True,
            help="Name of iteration field.")
    parser.add_argument(
            '-n', '--size',
            help="Optional name of size field.")
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
            help="Field to amortize. Can rename fields with "
                "new_name=old_name.")
    parser.add_argument(
            '-D', '--define',
            dest='defines',
            action='append',
            type=lambda x: (
                lambda k, vs: (
                    k.strip(),
                    {v.strip() for v in vs.split(',')})
                )(*x.split('=', 1)),
            help="Only include results where this field is this value. May "
                "include comma-separated options.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))

