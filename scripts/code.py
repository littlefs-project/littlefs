#!/usr/bin/env python3
#
# Script to find code size at the function level. Basically just a bit wrapper
# around nm with some extra conveniences for comparing builds. Heavily inspired
# by Linux's Bloat-O-Meter.
#

import os
import glob
import itertools as it
import subprocess as sp
import shlex
import re
import csv
import collections as co


OBJ_PATHS = ['*.o']

def collect(paths, **args):
    results = co.defaultdict(lambda: 0)
    pattern = re.compile(
        '^(?P<size>[0-9a-fA-F]+)' +
        ' (?P<type>[%s])' % re.escape(args['type']) +
        ' (?P<func>.+?)$')
    for path in paths:
        # note nm-tool may contain extra args
        cmd = args['nm_tool'] + ['--size-sort', path]
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
                results[(path, m.group('func'))] += int(m.group('size'), 16)
        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

    flat_results = []
    for (file, func), size in results.items():
        # map to source files
        if args.get('build_dir'):
            file = re.sub('%s/*' % re.escape(args['build_dir']), '', file)
        # replace .o with .c, different scripts report .o/.c, we need to
        # choose one if we want to deduplicate csv files
        file = re.sub('\.o$', '.c', file)
        # discard internal functions
        if not args.get('everything'):
            if func.startswith('__'):
                continue
        # discard .8449 suffixes created by optimizer
        func = re.sub('\.[0-9]+', '', func)

        flat_results.append((file, func, size))

    return flat_results

def main(**args):
    def openio(path, mode='r'):
        if path == '-':
            if 'r' in mode:
                return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
            else:
                return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
        else:
            return open(path, mode)

    # find sizes
    if not args.get('use', None):
        # find .o files
        paths = []
        for path in args['obj_paths']:
            if os.path.isdir(path):
                path = path + '/*.o'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print('no .obj files found in %r?' % args['obj_paths'])
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        with openio(args['use']) as f:
            r = csv.DictReader(f)
            results = [
                (   result['file'],
                    result['name'],
                    int(result['code_size']))
                for result in r
                if result.get('code_size') not in {None, ''}]

    total = 0
    for _, _, size in results:
        total += size

    # find previous results?
    if args.get('diff'):
        try:
            with openio(args['diff']) as f:
                r = csv.DictReader(f)
                prev_results = [
                    (   result['file'],
                        result['name'],
                        int(result['code_size']))
                    for result in r
                    if result.get('code_size') not in {None, ''}]
        except FileNotFoundError:
            prev_results = []

        prev_total = 0
        for _, _, size in prev_results:
            prev_total += size

    # write results to CSV
    if args.get('output'):
        merged_results = co.defaultdict(lambda: {})
        other_fields = []

        # merge?
        if args.get('merge'):
            try:
                with openio(args['merge']) as f:
                    r = csv.DictReader(f)
                    for result in r:
                        file = result.pop('file', '')
                        func = result.pop('name', '')
                        result.pop('code_size', None)
                        merged_results[(file, func)] = result
                        other_fields = result.keys()
            except FileNotFoundError:
                pass

        for file, func, size in results:
            merged_results[(file, func)]['code_size'] = size

        with openio(args['output'], 'w') as f:
            w = csv.DictWriter(f, ['file', 'name', *other_fields, 'code_size'])
            w.writeheader()
            for (file, func), result in sorted(merged_results.items()):
                w.writerow({'file': file, 'name': func, **result})

    # print results
    def dedup_entries(results, by='name'):
        entries = co.defaultdict(lambda: 0)
        for file, func, size in results:
            entry = (file if by == 'file' else func)
            entries[entry] += size
        return entries

    def diff_entries(olds, news):
        diff = co.defaultdict(lambda: (0, 0, 0, 0))
        for name, new in news.items():
            diff[name] = (0, new, new, 1.0)
        for name, old in olds.items():
            _, new, _, _ = diff[name]
            diff[name] = (old, new, new-old, (new-old)/old if old else 1.0)
        return diff

    def sorted_entries(entries):
        if args.get('size_sort'):
            return sorted(entries, key=lambda x: (-x[1], x))
        elif args.get('reverse_size_sort'):
            return sorted(entries, key=lambda x: (+x[1], x))
        else:
            return sorted(entries)

    def sorted_diff_entries(entries):
        if args.get('size_sort'):
            return sorted(entries, key=lambda x: (-x[1][1], x))
        elif args.get('reverse_size_sort'):
            return sorted(entries, key=lambda x: (+x[1][1], x))
        else:
            return sorted(entries, key=lambda x: (-x[1][3], x))

    def print_header(by=''):
        if not args.get('diff'):
            print('%-36s %7s' % (by, 'size'))
        else:
            print('%-36s %7s %7s %7s' % (by, 'old', 'new', 'diff'))

    def print_entry(name, size):
        print("%-36s %7d" % (name, size))

    def print_diff_entry(name, old, new, diff, ratio):
        print("%-36s %7s %7s %+7d%s" % (name,
            old or "-",
            new or "-",
            diff,
            ' (%+.1f%%)' % (100*ratio) if ratio else ''))

    def print_entries(by='name'):
        entries = dedup_entries(results, by=by)

        if not args.get('diff'):
            print_header(by=by)
            for name, size in sorted_entries(entries.items()):
                print_entry(name, size)
        else:
            prev_entries = dedup_entries(prev_results, by=by)
            diff = diff_entries(prev_entries, entries)
            print_header(by='%s (%d added, %d removed)' % (by,
                sum(1 for old, _, _, _ in diff.values() if not old),
                sum(1 for _, new, _, _ in diff.values() if not new)))
            for name, (old, new, diff, ratio) in sorted_diff_entries(
                    diff.items()):
                if ratio or args.get('all'):
                    print_diff_entry(name, old, new, diff, ratio)

    def print_totals():
        if not args.get('diff'):
            print_entry('TOTAL', total)
        else:
            ratio = (0.0 if not prev_total and not total
                else 1.0 if not prev_total
                else (total-prev_total)/prev_total)
            print_diff_entry('TOTAL',
                prev_total, total,
                total-prev_total,
                ratio)

    if args.get('quiet'):
        pass
    elif args.get('summary'):
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
        description="Find code size at the function level.")
    parser.add_argument('obj_paths', nargs='*', default=OBJ_PATHS,
        help="Description of where to find *.o files. May be a directory \
            or a list of paths. Defaults to %r." % OBJ_PATHS)
    parser.add_argument('-v', '--verbose', action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-u', '--use',
        help="Don't compile and find code sizes, instead use this CSV file.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff code size against.")
    parser.add_argument('-m', '--merge',
        help="Merge with an existing CSV file when writing to output.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all functions, not just the ones that changed.")
    parser.add_argument('-A', '--everything', action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument('-s', '--size-sort', action='store_true',
        help="Sort by size.")
    parser.add_argument('-S', '--reverse-size-sort', action='store_true',
        help="Sort by size, but backwards.")
    parser.add_argument('-F', '--files', action='store_true',
        help="Show file-level code sizes. Note this does not include padding! "
            "So sizes may differ from other tools.")
    parser.add_argument('-Y', '--summary', action='store_true',
        help="Only show the total code size.")
    parser.add_argument('--type', default='tTrRdD',
        help="Type of symbols to report, this uses the same single-character "
            "type-names emitted by nm. Defaults to %(default)r.")
    parser.add_argument('--nm-tool', default=['nm'], type=lambda x: x.split(),
        help="Path to the nm tool to use.")
    parser.add_argument('--build-dir',
        help="Specify the relative build directory. Used to map object files \
            to the correct source files.")
    sys.exit(main(**vars(parser.parse_args())))
