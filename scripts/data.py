#!/usr/bin/env python3
#
# Script to find data size at the function level. Basically just a bit wrapper
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

class DataResult(co.namedtuple('DataResult', 'data_size')):
    __slots__ = ()
    def __new__(cls, data_size=0):
        return super().__new__(cls, int(data_size))

    def __add__(self, other):
        return self.__class__(self.data_size + other.data_size)

    def __sub__(self, other):
        return DataDiff(other, self)

    def __rsub__(self, other):
        return self.__class__.__sub__(other, self)

    def key(self, **args):
        if args.get('size_sort'):
            return -self.data_size
        elif args.get('reverse_size_sort'):
            return +self.data_size
        else:
            return None

    _header = '%7s' % 'size'
    def __str__(self):
        return '%7d' % self.data_size

class DataDiff(co.namedtuple('DataDiff',  'old,new')):
    __slots__ = ()

    def ratio(self):
        old = self.old.data_size if self.old is not None else 0
        new = self.new.data_size if self.new is not None else 0
        return (new-old) / old if old else 1.0

    def key(self, **args):
        return (
            self.new.key(**args) if self.new is not None else 0,
            -self.ratio())

    def __bool__(self):
        return bool(self.ratio())

    _header = '%7s %7s %7s' % ('old', 'new', 'diff')
    def __str__(self):
        old = self.old.data_size if self.old is not None else 0
        new = self.new.data_size if self.new is not None else 0
        diff = new - old
        ratio = self.ratio()
        return '%7s %7s %+7d%s' % (
            old or "-",
            new or "-",
            diff,
            ' (%+.1f%%)' % (100*ratio) if ratio else '')


def openio(path, mode='r'):
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

def collect(paths, **args):
    results = co.defaultdict(lambda: DataResult())
    pattern = re.compile(
        '^(?P<size>[0-9a-fA-F]+)' +
        ' (?P<type>[%s])' % re.escape(args['type']) +
        ' (?P<func>.+?)$')
    for path in paths:
        # map to source file
        src_path = re.sub('\.o$', '.c', path)
        if args.get('build_dir'):
            src_path = re.sub('%s/*' % re.escape(args['build_dir']), '',
                src_path)
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
                func = m.group('func')
                # discard internal functions
                if not args.get('everything') and func.startswith('__'):
                    continue
                # discard .8449 suffixes created by optimizer
                func = re.sub('\.[0-9]+', '', func)
                results[(src_path, func)] += DataResult(
                    int(m.group('size'), 16))
        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

    return results

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
        with openio(args['use']) as f:
            r = csv.DictReader(f)
            results = {
                (result['file'], result['name']): DataResult(
                    *(result[f] for f in DataResult._fields))
                for result in r
                if all(result.get(f) not in {None, ''}
                    for f in DataResult._fields)}

    # find previous results?
    if args.get('diff'):
        try:
            with openio(args['diff']) as f:
                r = csv.DictReader(f)
                prev_results = {
                    (result['file'], result['name']): DataResult(
                        *(result[f] for f in DataResult._fields))
                    for result in r
                    if all(result.get(f) not in {None, ''}
                        for f in DataResult._fields)}
        except FileNotFoundError:
            prev_results = []

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
                        for f in DataResult._fields:
                            result.pop(f, None)
                        merged_results[(file, func)] = result
                        other_fields = result.keys()
            except FileNotFoundError:
                pass

        for (file, func), result in results.items():
            merged_results[(file, func)] |= result._asdict()

        with openio(args['output'], 'w') as f:
            w = csv.DictWriter(f, ['file', 'name',
                *other_fields, *DataResult._fields])
            w.writeheader()
            for (file, func), result in sorted(merged_results.items()):
                w.writerow({'file': file, 'name': func, **result})

    # print results
    def print_header(by):
        if by == 'total':
            entry = lambda k: 'TOTAL'
        elif by == 'file':
            entry = lambda k: k[0]
        else:
            entry = lambda k: k[1]

        if not args.get('diff'):
            print('%-36s %s' % (by, DataResult._header))
        else:
            old = {entry(k) for k in results.keys()}
            new = {entry(k) for k in prev_results.keys()}
            print('%-36s %s' % (
                '%s (%d added, %d removed)' % (by,
                        sum(1 for k in new if k not in old),
                        sum(1 for k in old if k not in new))
                    if by else '',
                DataDiff._header))

    def print_entries(by):
        if by == 'total':
            entry = lambda k: 'TOTAL'
        elif by == 'file':
            entry = lambda k: k[0]
        else:
            entry = lambda k: k[1]

        entries = co.defaultdict(lambda: DataResult())
        for k, result in results.items():
            entries[entry(k)] += result

        if not args.get('diff'):
            for name, result in sorted(entries.items(),
                    key=lambda p: (p[1].key(**args), p)):
                print('%-36s %s' % (name, result))
        else:
            prev_entries = co.defaultdict(lambda: DataResult())
            for k, result in prev_results.items():
                prev_entries[entry(k)] += result

            diff_entries = {name: entries.get(name) - prev_entries.get(name)
                for name in (entries.keys() | prev_entries.keys())}

            for name, diff in sorted(diff_entries.items(),
                    key=lambda p: (p[1].key(**args), p)):
                if diff or args.get('all'):
                    print('%-36s %s' % (name, diff))

    if args.get('quiet'):
        pass
    elif args.get('summary'):
        print_header('')
        print_entries('total')
    elif args.get('files'):
        print_header('file')
        print_entries('file')
        print_entries('total')
    else:
        print_header('function')
        print_entries('function')
        print_entries('total')


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find data size at the function level.")
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
        help="Don't compile and find data sizes, instead use this CSV file.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff data size against.")
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
        help="Show file-level data sizes. Note this does not include padding! "
            "So sizes may differ from other tools.")
    parser.add_argument('-Y', '--summary', action='store_true',
        help="Only show the total data size.")
    parser.add_argument('--type', default='dDbB',
        help="Type of symbols to report, this uses the same single-character "
            "type-names emitted by nm. Defaults to %(default)r.")
    parser.add_argument('--nm-tool', default=['nm'], type=lambda x: x.split(),
        help="Path to the nm tool to use.")
    parser.add_argument('--build-dir',
        help="Specify the relative build directory. Used to map object files \
            to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_args()).items()
        if v is not None}))
