#!/usr/bin/env python3
#
# Script to find stack usage at the function level. Will detect recursion and
# report as infinite stack usage.
#

import os
import glob
import itertools as it
import re
import csv
import collections as co
import math as m


CI_PATHS = ['*.ci']

def collect(paths, **args):
    # parse the vcg format
    k_pattern = re.compile('([a-z]+)\s*:', re.DOTALL)
    v_pattern = re.compile('(?:"(.*?)"|([a-z]+))', re.DOTALL)
    def parse_vcg(rest):
        def parse_vcg(rest):
            node = []
            while True:
                rest = rest.lstrip()
                m = k_pattern.match(rest)
                if not m:
                    return (node, rest)
                k, rest = m.group(1), rest[m.end(0):]

                rest = rest.lstrip()
                if rest.startswith('{'):
                    v, rest = parse_vcg(rest[1:])
                    assert rest[0] == '}', "unexpected %r" % rest[0:1]
                    rest = rest[1:]
                    node.append((k, v))
                else:
                    m = v_pattern.match(rest)
                    assert m, "unexpected %r" % rest[0:1]
                    v, rest = m.group(1) or m.group(2), rest[m.end(0):]
                    node.append((k, v))

        node, rest = parse_vcg(rest)
        assert rest == '', "unexpected %r" % rest[0:1]
        return node

    # collect into functions
    results = co.defaultdict(lambda: (None, None, 0, set()))
    f_pattern = re.compile(
        r'([^\\]*)\\n([^:]*)[^\\]*\\n([0-9]+) bytes \((.*)\)')
    for path in paths:
        with open(path) as f:
            vcg = parse_vcg(f.read())
        for k, graph in vcg:
            if k != 'graph':
                continue
            for k, info in graph:
                if k == 'node':
                    info = dict(info)
                    m = f_pattern.match(info['label'])
                    if m:
                        function, file, size, type = m.groups()
                        if not args.get('quiet') and type != 'static':
                            print('warning: found non-static stack for %s (%s)'
                                % (function, type))
                        _, _, _, targets = results[info['title']]
                        results[info['title']] = (
                            file, function, int(size), targets)
                elif k == 'edge':
                    info = dict(info)
                    _, _, _, targets = results[info['sourcename']]
                    targets.add(info['targetname'])
                else:
                    continue

    if not args.get('everything'):
        for source, (s_file, s_function, _, _) in list(results.items()):
            # discard internal functions
            if s_file.startswith('<') or s_file.startswith('/usr/include'):
                del results[source]

    # find maximum stack size recursively, this requires also detecting cycles
    # (in case of recursion)
    def stack_limit(source, seen=None):
        seen = seen or set()
        if source not in results:
            return 0
        _, _, frame, targets = results[source]

        limit = 0
        for target in targets:
            if target in seen:
                # found a cycle
                return float('inf')
            limit_ = stack_limit(target, seen | {target})
            limit = max(limit, limit_)

        return frame + limit

    # flatten into a list
    flat_results = []
    for source, (s_file, s_function, frame, targets) in results.items():
        limit = stack_limit(source)
        flat_results.append((s_file, s_function, frame, limit))

    return flat_results

def main(**args):
    # find sizes
    if not args.get('use', None):
        # find .ci files
        paths = []
        for path in args['ci_paths']:
            if os.path.isdir(path):
                path = path + '/*.ci'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print('no .ci files found in %r?' % args['ci_paths'])
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        with open(args['use']) as f:
            r = csv.DictReader(f)
            results = [
                (   result['file'],
                    result['function'],
                    int(result['frame']),
                    float(result['limit'])) # note limit can be inf
                for result in r]

    total_frame = 0
    total_limit = 0
    for _, _, frame, limit in results:
        total_frame += frame
        total_limit = max(total_limit, limit)

    # find previous results?
    if args.get('diff'):
        with open(args['diff']) as f:
            r = csv.DictReader(f)
            prev_results = [
                (   result['file'],
                    result['function'],
                    int(result['frame']),
                    float(result['limit']))
                for result in r]

        prev_total_frame = 0
        prev_total_limit = 0
        for _, _, frame, limit in prev_results:
            prev_total_frame += frame
            prev_total_limit = max(prev_total_limit, limit)

    # write results to CSV
    if args.get('output'):
        with open(args['output'], 'w') as f:
            w = csv.writer(f)
            w.writerow(['file', 'function', 'frame', 'limit'])
            for file, func, frame, limit in sorted(results):
                w.writerow((file, func, frame, limit))

    # print results
    def dedup_entries(results, by='function'):
        entries = co.defaultdict(lambda: (0, 0))
        for file, func, frame, limit in results:
            entry = (file if by == 'file' else func)
            entry_frame, entry_limit = entries[entry]
            entries[entry] = (entry_frame + frame, max(entry_limit, limit))
        return entries

    def diff_entries(olds, news):
        diff = co.defaultdict(lambda: (None, None, None, None, 0, 0, 0))
        for name, (new_frame, new_limit) in news.items():
            diff[name] = (
                None, None,
                new_frame, new_limit,
                new_frame, new_limit,
                1.0)
        for name, (old_frame, old_limit) in olds.items():
            _, _, new_frame, new_limit, _, _, _ = diff[name]
            diff[name] = (
                old_frame, old_limit,
                new_frame, new_limit,
                (new_frame or 0) - (old_frame or 0),
                0 if m.isinf(new_limit or 0) and m.isinf(old_limit or 0)
                    else (new_limit or 0) - (old_limit or 0),
                0.0 if m.isinf(new_limit or 0) and m.isinf(old_limit or 0)
                    else +float('inf') if m.isinf(new_limit or 0)
                    else -float('inf') if m.isinf(old_limit or 0)
                    else +0.0 if not old_limit and not new_limit
                    else +1.0 if not old_limit
                    else ((new_limit or 0) - (old_limit or 0))/(old_limit or 0))
        return diff

    def sorted_entries(entries):
        if args.get('limit_sort'):
            return sorted(entries, key=lambda x: (-x[1][1], x))
        elif args.get('reverse_limit_sort'):
            return sorted(entries, key=lambda x: (+x[1][1], x))
        elif args.get('frame_sort'):
            return sorted(entries, key=lambda x: (-x[1][0], x))
        elif args.get('reverse_frame_sort'):
            return sorted(entries, key=lambda x: (+x[1][0], x))
        else:
            return sorted(entries)

    def sorted_diff_entries(entries):
        if args.get('limit_sort'):
            return sorted(entries, key=lambda x: (-(x[1][3] or 0), x))
        elif args.get('reverse_limit_sort'):
            return sorted(entries, key=lambda x: (+(x[1][3] or 0), x))
        elif args.get('frame_sort'):
            return sorted(entries, key=lambda x: (-(x[1][2] or 0), x))
        elif args.get('reverse_frame_sort'):
            return sorted(entries, key=lambda x: (+(x[1][2] or 0), x))
        else:
            return sorted(entries, key=lambda x: (-x[1][6], x))

    def print_header(by=''):
        if not args.get('diff'):
            print('%-36s %7s %7s' % (by, 'frame', 'limit'))
        else:
            print('%-36s %15s %15s %15s' % (by, 'old', 'new', 'diff'))

    def print_entries(by='function'):
        entries = dedup_entries(results, by=by)

        if not args.get('diff'):
            print_header(by=by)
            for name, (frame, limit) in sorted_entries(entries.items()):
                print("%-36s %7d %7s" % (name,
                    frame, '∞' if m.isinf(limit) else int(limit)))
        else:
            prev_entries = dedup_entries(prev_results, by=by)
            diff = diff_entries(prev_entries, entries)
            print_header(by='%s (%d added, %d removed)' % (by,
                sum(1 for _, old, _, _, _, _, _ in diff.values() if old is None),
                sum(1 for _, _, _, new, _, _, _ in diff.values() if new is None)))
            for name, (
                    old_frame, old_limit,
                    new_frame, new_limit,
                    diff_frame, diff_limit, ratio) in sorted_diff_entries(
                        diff.items()):
                if ratio or args.get('all'):
                    print("%-36s %7s %7s %7s %7s %+7d %7s%s" % (name,
                        old_frame if old_frame is not None else "-",
                        ('∞' if m.isinf(old_limit) else int(old_limit))
                            if old_limit is not None else "-",
                        new_frame if new_frame is not None else "-",
                        ('∞' if m.isinf(new_limit) else int(new_limit))
                            if new_limit is not None else "-",
                        diff_frame,
                        ('+∞' if diff_limit > 0 and m.isinf(diff_limit)
                            else '-∞' if diff_limit < 0 and m.isinf(diff_limit)
                            else '%+d' % diff_limit),
                        '' if not ratio
                            else ' (+∞%)' if ratio > 0 and m.isinf(ratio)
                            else ' (-∞%)' if ratio < 0 and m.isinf(ratio)
                            else ' (%+.1f%%)' % (100*ratio)))

    def print_totals():
        if not args.get('diff'):
            print("%-36s %7d %7s" % ('TOTAL',
                total_frame, '∞' if m.isinf(total_limit) else int(total_limit)))
        else:
            diff_frame = total_frame - prev_total_frame
            diff_limit = (
                0 if m.isinf(total_limit or 0) and m.isinf(prev_total_limit or 0)
                    else (total_limit or 0) - (prev_total_limit or 0))
            ratio = (
                0.0 if m.isinf(total_limit or 0) and m.isinf(prev_total_limit or 0)
                    else +float('inf') if m.isinf(total_limit or 0)
                    else -float('inf') if m.isinf(prev_total_limit or 0)
                    else +0.0 if not prev_total_limit and not total_limit
                    else +1.0 if not prev_total_limit
                    else ((total_limit or 0) - (prev_total_limit or 0))/(prev_total_limit or 0))
            print("%-36s %7s %7s %7s %7s %+7d %7s%s" % ('TOTAL',
                prev_total_frame if prev_total_frame is not None else '-',
                ('∞' if m.isinf(prev_total_limit) else int(prev_total_limit))
                    if prev_total_limit is not None else '-',
                total_frame if total_frame is not None else '-',
                ('∞' if m.isinf(total_limit) else int(total_limit))
                    if total_limit is not None else '-',
                diff_frame,
                ('+∞' if diff_limit > 0 and m.isinf(diff_limit)
                    else '-∞' if diff_limit < 0 and m.isinf(diff_limit)
                    else '%+d' % diff_limit),
                '' if not ratio
                    else ' (+∞%)' if ratio > 0 and m.isinf(ratio)
                    else ' (-∞%)' if ratio < 0 and m.isinf(ratio)
                    else ' (%+.1f%%)' % (100*ratio)))


    if args.get('quiet'):
        pass
    elif args.get('summary'):
        print_header()
        print_totals()
    elif args.get('files'):
        print_entries(by='file')
        print_totals()
    else:
        print_entries(by='function')
        print_totals()

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find stack usage at the function level.")
    parser.add_argument('ci_paths', nargs='*', default=CI_PATHS,
        help="Description of where to find *.ci files. May be a directory \
            or a list of paths. Defaults to %r." % CI_PATHS)
    parser.add_argument('-v', '--verbose', action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-u', '--use',
        help="Don't parse callgraph files, instead use this CSV file.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff against.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all functions, not just the ones that changed.")
    parser.add_argument('-A', '--everything', action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument('-s', '--limit-sort', action='store_true',
        help="Sort by stack limit.")
    parser.add_argument('-S', '--reverse-limit-sort', action='store_true',
        help="Sort by stack limit, but backwards.")
    parser.add_argument('-f', '--frame-sort', action='store_true',
        help="Sort by stack frame size.")
    parser.add_argument('-F', '--reverse-frame-sort', action='store_true',
        help="Sort by stack frame size, but backwards.")
    parser.add_argument('--files', action='store_true',
        help="Show file-level calls.")
    parser.add_argument('--summary', action='store_true',
        help="Only show the total stack size.")
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument('--build-dir',
        help="Specify the relative build directory. Used to map object files \
            to the correct source files.")
    sys.exit(main(**vars(parser.parse_args())))
