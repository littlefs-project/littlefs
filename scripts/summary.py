#!/usr/bin/env python3
#
# Script to summarize the outputs of other scripts. Operates on CSV files.
#

import functools as ft
import collections as co
import os
import csv
import re
import math as m

# displayable fields
Field = co.namedtuple('Field', 'name,parse,acc,key,fmt,repr,null,ratio')
FIELDS = [
    # name, parse, accumulate, fmt, print, null
    Field('code',
        lambda r: int(r['code_size']),
        sum,
        lambda r: r,
        '%7s',
        lambda r: r,
        '-',
        lambda old, new: (new-old)/old),
    Field('data',
        lambda r: int(r['data_size']),
        sum,
        lambda r: r,
        '%7s',
        lambda r: r,
        '-',
        lambda old, new: (new-old)/old),
    Field('stack',
        lambda r: float(r['stack_limit']),
        max,
        lambda r: r,
        '%7s',
        lambda r: '∞' if m.isinf(r) else int(r),
        '-',
        lambda old, new: (new-old)/old),
    Field('structs',
        lambda r: int(r['struct_size']),
        sum,
        lambda r: r,
        '%8s',
        lambda r: r,
        '-',
        lambda old, new: (new-old)/old),
    Field('coverage',
        lambda r: (int(r['coverage_hits']), int(r['coverage_count'])),
        lambda rs: ft.reduce(lambda a, b: (a[0]+b[0], a[1]+b[1]), rs),
        lambda r: r[0]/r[1],
        '%19s',
        lambda r: '%11s %7s' % ('%d/%d' % (r[0], r[1]), '%.1f%%' % (100*r[0]/r[1])),
        '%11s %7s' % ('-', '-'),
        lambda old, new: ((new[0]/new[1]) - (old[0]/old[1])))
]


def main(**args):
    def openio(path, mode='r'):
        if path == '-':
            if 'r' in mode:
                return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
            else:
                return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
        else:
            return open(path, mode)

    # find results
    results = co.defaultdict(lambda: {})
    for path in args.get('csv_paths', '-'):
        try:
            with openio(path) as f:
                r = csv.DictReader(f)
                for result in r:
                    file = result.pop('file', '')
                    name = result.pop('name', '')
                    prev = results[(file, name)]
                    for field in FIELDS:
                        try:
                            r = field.parse(result)
                            if field.name in prev:
                                results[(file, name)][field.name] = field.acc(
                                    [prev[field.name], r])
                            else:
                                results[(file, name)][field.name] = r
                        except (KeyError, ValueError):
                            pass
        except FileNotFoundError:
            pass

    # find fields
    if args.get('all_fields'):
        fields = FIELDS
    elif args.get('fields') is not None:
        fields_dict = {field.name: field for field in FIELDS}
        fields = [fields_dict[f] for f in args['fields']]
    else:
        fields = []
        for field in FIELDS:
            if any(field.name in result for result in results.values()):
                fields.append(field)

    # find total for every field
    total = {}
    for result in results.values():
        for field in fields:
            if field.name in result and field.name in total:
                total[field.name] = field.acc(
                    [total[field.name], result[field.name]])
            elif field.name in result:
                total[field.name] = result[field.name]

    # find previous results?
    if args.get('diff'):
        prev_results = co.defaultdict(lambda: {})
        try:
            with openio(args['diff']) as f:
                r = csv.DictReader(f)
                for result in r:
                    file = result.pop('file', '')
                    name = result.pop('name', '')
                    prev = prev_results[(file, name)]
                    for field in FIELDS:
                        try:
                            r = field.parse(result)
                            if field.name in prev:
                                prev_results[(file, name)][field.name] = field.acc(
                                    [prev[field.name], r])
                            else:
                                prev_results[(file, name)][field.name] = r
                        except (KeyError, ValueError):
                            pass
        except FileNotFoundError:
            pass

        prev_total = {}
        for result in prev_results.values():
            for field in fields:
                if field.name in result and field.name in prev_total:
                    prev_total[field.name] = field.acc(
                        [prev_total[field.name], result[field.name]])
                elif field.name in result:
                    prev_total[field.name] = result[field.name]

    # print results
    def dedup_entries(results, by='name'):
        entries = co.defaultdict(lambda: {})
        for (file, func), result in results.items():
            entry = (file if by == 'file' else func)
            prev = entries[entry]
            for field in fields:
                if field.name in result and field.name in prev:
                    entries[entry][field.name] = field.acc(
                        [prev[field.name], result[field.name]])
                elif field.name in result:
                    entries[entry][field.name] = result[field.name]
        return entries

    def sorted_entries(entries):
        if args.get('sort') is not None:
            field = {field.name: field for field in FIELDS}[args['sort']]
            return sorted(entries, key=lambda x: (
                -(field.key(x[1][field.name])) if field.name in x[1] else -1, x))
        elif args.get('reverse_sort') is not None:
            field = {field.name: field for field in FIELDS}[args['reverse_sort']]
            return sorted(entries, key=lambda x: (
                +(field.key(x[1][field.name])) if field.name in x[1] else -1, x))
        else:
            return sorted(entries)

    def print_header(by=''):
        if not args.get('diff'):
            print('%-36s' % by, end='')
            for field in fields:
                print((' '+field.fmt) % field.name, end='')
            print()
        else:
            print('%-36s' % by, end='')
            for field in fields:
                print((' '+field.fmt) % field.name, end='')
                print(' %-9s' % '', end='')
            print()

    def print_entry(name, result):
        print('%-36s' % name, end='')
        for field in fields:
            r = result.get(field.name)
            if r is not None:
                print((' '+field.fmt) % field.repr(r), end='')
            else:
                print((' '+field.fmt) % '-', end='')
        print()

    def print_diff_entry(name, old, new):
        print('%-36s' % name, end='')
        for field in fields:
            n = new.get(field.name)
            if n is not None:
                print((' '+field.fmt) % field.repr(n), end='')
            else:
                print((' '+field.fmt) % '-', end='')
            o = old.get(field.name)
            ratio = (
                0.0 if m.isinf(o or 0) and m.isinf(n or 0)
                    else +float('inf') if m.isinf(n or 0)
                    else -float('inf') if m.isinf(o or 0)
                    else 0.0 if not o and not n
                    else +1.0 if not o
                    else -1.0 if not n
                    else field.ratio(o, n))
            print(' %-9s' % (
                '' if not ratio
                    else '(+∞%)' if ratio > 0 and m.isinf(ratio)
                    else '(-∞%)' if ratio < 0 and m.isinf(ratio)
                    else '(%+.1f%%)' % (100*ratio)), end='')
        print()

    def print_entries(by='name'):
        entries = dedup_entries(results, by=by)

        if not args.get('diff'):
            print_header(by=by)
            for name, result in sorted_entries(entries.items()):
                print_entry(name, result)
        else:
            prev_entries = dedup_entries(prev_results, by=by)
            print_header(by='%s (%d added, %d removed)' % (by,
                sum(1 for name in entries if name not in prev_entries),
                sum(1 for name in prev_entries if name not in entries)))
            for name, result in sorted_entries(entries.items()):
                if args.get('all') or result != prev_entries.get(name, {}):
                    print_diff_entry(name, prev_entries.get(name, {}), result)

    def print_totals():
        if not args.get('diff'):
            print_entry('TOTAL', total)
        else:
            print_diff_entry('TOTAL', prev_total, total)

    if args.get('summary'):
        print_header()
        print_totals()
    elif args.get('files'):
        print_entries(by='file')
        print_totals()
    else:
        print_entries(by='name')
        print_totals()


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Summarize measurements")
    parser.add_argument('csv_paths', nargs='*', default='-',
        help="Description of where to find *.csv files. May be a directory \
            or list of paths. *.csv files will be merged to show the total \
            coverage.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff against.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all objects, not just the ones that changed.")
    parser.add_argument('-e', '--all-fields', action='store_true',
        help="Show all fields, even those with no results.")
    parser.add_argument('-f', '--fields', type=lambda x: re.split('\s*,\s*', x),
        help="Comma separated list of fields to print, by default all fields \
            that are found in the CSV files are printed.")
    parser.add_argument('-s', '--sort',
        help="Sort by this field.")
    parser.add_argument('-S', '--reverse-sort',
        help="Sort by this field, but backwards.")
    parser.add_argument('-F', '--files', action='store_true',
        help="Show file-level calls.")
    parser.add_argument('-Y', '--summary', action='store_true',
        help="Only show the totals.")
    sys.exit(main(**vars(parser.parse_args())))
