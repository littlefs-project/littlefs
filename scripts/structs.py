#!/usr/bin/env python3
#
# Script to find struct sizes.
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
        '^(?:.*DW_TAG_(?P<tag>[a-z_]+).*'
            '|^.*DW_AT_name.*:\s*(?P<name>[^:\s]+)\s*'
            '|^.*DW_AT_byte_size.*:\s*(?P<size>[0-9]+)\s*)$')

    for path in paths:
        # collect structs as we parse dwarf info
        found = False
        name = None
        size = None

        # note objdump-tool may contain extra args
        cmd = args['objdump_tool'] + ['--dwarf=info', path]
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))
        proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            stderr=sp.PIPE if not args.get('verbose') else None,
            universal_newlines=True)
        for line in proc.stdout:
            # state machine here to find structs
            m = pattern.match(line)
            if m:
                if m.group('tag'):
                    if name is not None and size is not None:
                        results[(path, name)] = size
                    found = (m.group('tag') == 'structure_type')
                    name = None
                    size = None
                elif found and m.group('name'):
                    name = m.group('name')
                elif found and name and m.group('size'):
                    size = int(m.group('size'))
        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

    flat_results = []
    for (file, struct), size in results.items():
        # map to source files
        if args.get('build_dir'):
            file = re.sub('%s/*' % re.escape(args['build_dir']), '', file)
        flat_results.append((file, struct, size))

    return flat_results

def main(**args):
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
        with open(args['use']) as f:
            r = csv.DictReader(f)
            results = [
                (   result['file'],
                    result['struct'],
                    int(result['struct_size']))
                for result in r]

    total = 0
    for _, _, size in results:
        total += size

    # find previous results?
    if args.get('diff'):
        try:
            with open(args['diff']) as f:
                r = csv.DictReader(f)
                prev_results = [
                    (   result['file'],
                        result['struct'],
                        int(result['struct_size']))
                    for result in r]
        except FileNotFoundError:
            prev_results = []

        prev_total = 0
        for _, _, size in prev_results:
            prev_total += size

    # write results to CSV
    if args.get('output'):
        with open(args['output'], 'w') as f:
            w = csv.writer(f)
            w.writerow(['file', 'struct', 'struct_size'])
            for file, struct, size in sorted(results):
                w.writerow((file, struct, size))

    # print results
    def dedup_entries(results, by='struct'):
        entries = co.defaultdict(lambda: 0)
        for file, struct, size in results:
            entry = (file if by == 'file' else struct)
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

    def print_entries(by='struct'):
        entries = dedup_entries(results, by=by)

        if not args.get('diff'):
            print_header(by=by)
            for name, size in sorted_entries(entries.items()):
                print("%-36s %7d" % (name, size))
        else:
            prev_entries = dedup_entries(prev_results, by=by)
            diff = diff_entries(prev_entries, entries)
            print_header(by='%s (%d added, %d removed)' % (by,
                sum(1 for old, _, _, _ in diff.values() if not old),
                sum(1 for _, new, _, _ in diff.values() if not new)))
            for name, (old, new, diff, ratio) in sorted_diff_entries(
                    diff.items()):
                if ratio or args.get('all'):
                    print("%-36s %7s %7s %+7d%s" % (name,
                        old or "-",
                        new or "-",
                        diff,
                        ' (%+.1f%%)' % (100*ratio) if ratio else ''))

    def print_totals():
        if not args.get('diff'):
            print("%-36s %7d" % ('TOTAL', total))
        else:
            ratio = (total-prev_total)/prev_total if prev_total else 1.0
            print("%-36s %7s %7s %+7d%s" % (
                'TOTAL',
                prev_total if prev_total else '-',
                total if total else '-',
                total-prev_total,
                ' (%+.1f%%)' % (100*ratio) if ratio else ''))

    if args.get('quiet'):
        pass
    elif args.get('summary'):
        print_header()
        print_totals()
    elif args.get('files'):
        print_entries(by='file')
        print_totals()
    else:
        print_entries(by='struct')
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
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-u', '--use',
        help="Don't compile and find struct sizes, instead use this CSV file.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff struct size against.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all functions, not just the ones that changed.")
    parser.add_argument('-A', '--everything', action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument('-s', '--size-sort', action='store_true',
        help="Sort by size.")
    parser.add_argument('-S', '--reverse-size-sort', action='store_true',
        help="Sort by size, but backwards.")
    parser.add_argument('--files', action='store_true',
        help="Show file-level struct sizes.")
    parser.add_argument('--summary', action='store_true',
        help="Only show the total struct size.")
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument('--objdump-tool', default=['objdump'], type=lambda x: x.split(),
        help="Path to the objdump tool to use.")
    parser.add_argument('--build-dir',
        help="Specify the relative build directory. Used to map object files \
            to the correct source files.")
    sys.exit(main(**vars(parser.parse_args())))
