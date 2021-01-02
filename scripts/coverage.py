#!/usr/bin/env python3
#

import os
import glob
import csv
import re
import collections as co
import bisect as b

INFO_PATHS = 'tests/*.toml.info'


def collect(paths, **args):
    file = None
    funcs = []
    lines = co.defaultdict(lambda: 0)
    pattern = re.compile(
        '^(?P<file>SF:/?(?P<file_name>.*))$'
        '|^(?P<func>FN:(?P<func_lineno>[0-9]*),(?P<func_name>.*))$'
        '|^(?P<line>DA:(?P<line_lineno>[0-9]*),(?P<line_hits>[0-9]*))$')
    for path in paths:
        with open(path) as f:
            for line in f:
                m = pattern.match(line)
                if m and m.group('file'):
                    file = m.group('file_name')
                elif m and file and m.group('func'):
                    funcs.append((file, int(m.group('func_lineno')),
                        m.group('func_name')))
                elif m and file and m.group('line'):
                    lines[(file, int(m.group('line_lineno')))] += (
                        int(m.group('line_hits')))

    # map line numbers to functions
    funcs.sort()
    def func_from_lineno(file, lineno):
        i = b.bisect(funcs, (file, lineno))
        if i and funcs[i-1][0] == file:
            return funcs[i-1][2]
        else:
            return None

    # reduce to function info
    reduced_funcs = co.defaultdict(lambda: (0, 0))
    for (file, line_lineno), line_hits in lines.items():
        func = func_from_lineno(file, line_lineno)
        if not func:
            continue
        hits, count = reduced_funcs[(file, func)]
        reduced_funcs[(file, func)] = (hits + (line_hits > 0), count + 1)

    results = []
    for (file, func), (hits, count) in reduced_funcs.items():
        # discard internal/testing functions (test_* injected with
        # internal testing)
        if func.startswith('__') or func.startswith('test_'):
            continue
        # discard .8449 suffixes created by optimizer
        func = re.sub('\.[0-9]+', '', func)
        results.append((file, func, hits, count))

    return results


def main(**args):
    # find coverage
    if not args.get('input', None):
        # find *.info files
        paths = []
        for path in args['info_paths']:
            if os.path.isdir(path):
                path = path + '/*.gcov'

            for path in glob.glob(path, recursive=True):
                paths.append(path)

        if not paths:
            print('no .info files found in %r?' % args['info_paths'])
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        with open(args['input']) as f:
            r = csv.DictReader(f)
            results = [
                (   result['file'],
                    result['function'],
                    int(result['hits']),
                    int(result['count']))
                for result in r]

    total_hits, total_count = 0, 0
    for _, _, hits, count in results:
        total_hits += hits
        total_count += count

    # find previous results?
    if args.get('diff', None):
        with open(args['diff']) as f:
            r = csv.DictReader(f)
            prev_results = [
                (   result['file'],
                    result['function'],
                    int(result['hits']),
                    int(result['count']))
                for result in r]

        prev_total_hits, prev_total_count = 0, 0
        for _, _, hits, count in prev_results:
            prev_total_hits += hits
            prev_total_count += count

    # write results to CSV
    if args.get('output', None):
        results.sort(key=lambda x: (-(x[3]-x[2]), -x[3], x))
        with open(args['output'], 'w') as f:
            w = csv.writer(f)
            w.writerow(['file', 'function', 'hits', 'count'])
            for file, func, hits, count in results:
                w.writerow((file, func, hits, count))

    # print results
    def dedup_entries(results, by='function'):
        entries = co.defaultdict(lambda: (0, 0))
        for file, func, hits, count in results:
            entry = (file if by == 'file' else func)
            entry_hits, entry_count = entries[entry]
            entries[entry] = (entry_hits + hits, entry_count + count)
        return entries

    def diff_entries(olds, news):
        diff = co.defaultdict(lambda: (None, None, None, None, None, None))
        for name, (new_hits, new_count) in news.items():
            diff[name] = (
                0, 0,
                new_hits, new_count,
                new_hits, new_count)
        for name, (old_hits, old_count) in olds.items():
            new_hits = diff[name][2] or 0
            new_count = diff[name][3] or 0
            diff[name] = (
                old_hits, old_count,
                new_hits, new_count,
                new_hits-old_hits, new_count-old_count)
        return diff

    def print_header(by=''):
        if not args.get('diff', False):
            print('%-36s %11s' % (by, 'hits/count'))
        else:
            print('%-36s %11s %11s %11s' % (by, 'old', 'new', 'diff'))

    def print_entries(by='function'):
        entries = dedup_entries(results, by=by)

        if not args.get('diff', None):
            print_header(by=by)
            for name, (hits, count) in sorted(entries.items(),
                    key=lambda x: (-(x[1][1]-x[1][0]), -x[1][1], x)):
                print("%-36s %11s (%.2f%%)" % (name,
                    '%d/%d' % (hits, count),
                    100*(hits/count if count else 1.0)))
        else:
            prev_entries = dedup_entries(prev_results, by=by)
            diff = diff_entries(prev_entries, entries)
            print_header(by='%s (%d added, %d removed)' % (by,
                sum(1 for _, old, _, _, _, _ in diff.values() if not old),
                sum(1 for _, _, _, new, _, _ in diff.values() if not new)))
            for name, (
                    old_hits, old_count,
                    new_hits, new_count,
                    diff_hits, diff_count) in sorted(diff.items(),
                        key=lambda x: (
                            -(x[1][5]-x[1][4]), -x[1][5], -x[1][3], x)):
                ratio = ((new_hits/new_count if new_count else 1.0)
                    - (old_hits/old_count if old_count else 1.0))
                if diff_hits or diff_count or args.get('all', False):
                    print("%-36s %11s %11s %11s%s" % (name,
                        '%d/%d' % (old_hits, old_count)
                            if old_count else '-',
                        '%d/%d' % (new_hits, new_count)
                            if new_count else '-',
                        '%+d/%+d' % (diff_hits, diff_count),
                        ' (%+.2f%%)' % (100*ratio) if ratio else ''))

    def print_totals():
        if not args.get('diff', None):
            print("%-36s %11s (%.2f%%)" % ('TOTALS',
                '%d/%d' % (total_hits, total_count),
                100*(total_hits/total_count if total_count else 1.0)))
        else:
            ratio = ((total_hits/total_count
                    if total_count else 1.0)
                - (prev_total_hits/prev_total_count
                    if prev_total_count else 1.0))
            print("%-36s %11s %11s %11s%s" % ('TOTALS',
                '%d/%d' % (prev_total_hits, prev_total_count),
                '%d/%d' % (total_hits, total_count),
                '%+d/%+d' % (total_hits-prev_total_hits,
                    total_count-prev_total_count),
                ' (%+.2f%%)' % (100*ratio) if ratio else ''))

    def print_status():
        if not args.get('diff', None):
            print("%d/%d (%.2f%%)" % (total_hits, total_count,
                100*(total_hits/total_count if total_count else 1.0)))
        else:
            ratio = ((total_hits/total_count
                    if total_count else 1.0)
                - (prev_total_hits/prev_total_count
                    if prev_total_count else 1.0))
            print("%d/%d (%+.2f%%)" % (total_hits, total_count,
                (100*ratio) if ratio else ''))

    if args.get('quiet', False):
        pass
    elif args.get('status', False):
        print_status()
    elif args.get('summary', False):
        print_header()
        print_totals()
    elif args.get('files', False):
        print_entries(by='file')
        print_totals()
    else:
        print_entries(by='function')
        print_totals()

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Show/manipulate coverage info")
    parser.add_argument('info_paths', nargs='*', default=[INFO_PATHS],
        help="Description of where to find *.info files. May be a directory \
            or list of paths. *.info files will be merged to show the total \
            coverage. Defaults to \"%s\"." % INFO_PATHS)
    parser.add_argument('-v', '--verbose', action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument('-i', '--input',
        help="Don't do any work, instead use this CSV file.")
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff code size against.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all functions, not just the ones that changed.")
    parser.add_argument('--files', action='store_true',
        help="Show file-level coverage.")
    parser.add_argument('-s', '--summary', action='store_true',
        help="Only show the total coverage.")
    parser.add_argument('-S', '--status', action='store_true',
        help="Show minimum info useful for a single-line status.")
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    sys.exit(main(**vars(parser.parse_args())))
