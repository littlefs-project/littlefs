#!/usr/bin/env python3
#
# Script to find stack usage at the function level. Will detect recursion and
# report as infinite stack usage.
#
# Example:
# ./scripts/stack.py lfs.ci lfs_util.ci -S
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

import collections as co
import csv
import glob
import itertools as it
import math as m
import os
import re


CI_PATHS = ['*.ci']


# integer fields
class IntField(co.namedtuple('IntField', 'x')):
    __slots__ = ()
    def __new__(cls, x):
        if isinstance(x, IntField):
            return x
        if isinstance(x, str):
            try:
                x = int(x, 0)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', x):
                    x = float('inf')
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', x):
                    x = float('-inf')
                else:
                    raise
        return super().__new__(cls, x)

    def __int__(self):
        assert not m.isinf(self.x)
        return self.x

    def __float__(self):
        return float(self.x)

    def __str__(self):
        if self.x == float('inf'):
            return '∞'
        elif self.x == float('-inf'):
            return '-∞'
        else:
            return str(self.x)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    diff_none = '%7s' % '-'
    diff_table = table

    def diff_diff(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        diff = new - old
        if diff == float('+inf'):
            return '%7s' % '+∞'
        elif diff == float('-inf'):
            return '%7s' % '-∞'
        else:
            return '%+7d' % diff

    def ratio(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        if m.isinf(new) and m.isinf(old):
            return 0.0
        elif m.isinf(new):
            return float('+inf')
        elif m.isinf(old):
            return float('-inf')
        elif not old and not new:
            return 0.0
        elif not old:
            return 1.0
        else:
            return (new-old) / old

    def __add__(self, other):
        return IntField(self.x + other.x)

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

    def __truediv__(self, n):
        if m.isinf(self.x):
            return self
        else:
            return IntField(round(self.x / n))

# size results
class StackResult(co.namedtuple('StackResult',
        'file,function,stack_frame,stack_limit')):
    __slots__ = ()
    def __new__(cls, file, function, stack_frame, stack_limit):
        return super().__new__(cls, file, function,
            IntField(stack_frame), IntField(stack_limit))

    def __add__(self, other):
        return StackResult(self.file, self.function,
            self.stack_frame + other.stack_frame,
            max(self.stack_limit, other.stack_limit))


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)


def collect(paths, *,
        everything=False,
        **args):
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
    callgraph = co.defaultdict(lambda: (None, None, 0, set()))
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
                        if (not args.get('quiet')
                                and 'static' not in type
                                and 'bounded' not in type):
                            print('warning: found non-static stack for %s (%s)'
                                % (function, type, size))
                        _, _, _, targets = callgraph[info['title']]
                        callgraph[info['title']] = (
                            file, function, int(size), targets)
                elif k == 'edge':
                    info = dict(info)
                    _, _, _, targets = callgraph[info['sourcename']]
                    targets.add(info['targetname'])
                else:
                    continue

    if not everything:
        for source, (s_file, s_function, _, _) in list(callgraph.items()):
            # discard internal functions
            if s_file.startswith('<') or s_file.startswith('/usr/include'):
                del callgraph[source]

    # find maximum stack size recursively, this requires also detecting cycles
    # (in case of recursion)
    def find_limit(source, seen=None):
        seen = seen or set()
        if source not in callgraph:
            return 0
        _, _, frame, targets = callgraph[source]

        limit = 0
        for target in targets:
            if target in seen:
                # found a cycle
                return float('inf')
            limit_ = find_limit(target, seen | {target})
            limit = max(limit, limit_)

        return frame + limit

    def find_calls(targets):
        calls = set()
        for target in targets:
            if target in callgraph:
                t_file, t_function, _, _ = callgraph[target]
                calls.add((t_file, t_function))
        return calls

    # build results
    results = []
    calls = {}
    for source, (s_file, s_function, frame, targets) in callgraph.items():
        limit = find_limit(source)
        cs = find_calls(targets)
        results.append(StackResult(s_file, s_function, frame, limit))
        calls[(s_file, s_function)] = cs

    return results, calls


def fold(results, *,
        by=['file', 'function'],
        **_):
    folding = co.OrderedDict()
    for r in results:
        name = tuple(getattr(r, k) for k in by)
        if name not in folding:
            folding[name] = []
        folding[name].append(r)

    folded = []
    for rs in folding.values():
        folded.append(sum(rs[1:], start=rs[0]))

    return folded

def fold_calls(calls, *,
        by=['file', 'function'],
        **_):
    def by_(name):
        file, function = name
        return (((file,) if 'file' in by else ())
            + ((function,) if 'function' in by else ()))

    folded = {}
    for name, cs in calls.items():
        name = by_(name)
        if name not in folded:
            folded[name] = set()
        folded[name] |= {by_(c) for c in cs}

    return folded


def table(results, calls, diff_results=None, *,
        by_file=False,
        limit_sort=False,
        reverse_limit_sort=False,
        frame_sort=False,
        reverse_frame_sort=False,
        summary=False,
        all=False,
        percent=False,
        tree=False,
        depth=None,
        **_):
    all_, all = all, __builtins__.all

    # tree doesn't really make sense with depth=0, assume depth=inf
    if depth is None:
        depth = float('inf') if tree else 0

    # fold
    results = fold(results, by=['file' if by_file else 'function'])
    calls = fold_calls(calls, by=['file' if by_file else 'function'])
    if diff_results is not None:
        diff_results = fold(diff_results,
            by=['file' if by_file else 'function'])

    table = {
        r.file if by_file else r.function: r
        for r in results}
    diff_table = {
        r.file if by_file else r.function: r
        for r in diff_results or []}

    # sort, note that python's sort is stable
    names = list(table.keys() | diff_table.keys())
    names.sort()
    if diff_results is not None:
        names.sort(key=lambda n: -IntField.ratio(
            table[n].stack_frame if n in table else None,
            diff_table[n].stack_frame if n in diff_table else None))
    if limit_sort:
        names.sort(key=lambda n: (table[n].stack_limit,) if n in table else (),
            reverse=True)
    elif reverse_limit_sort:
        names.sort(key=lambda n: (table[n].stack_limit,) if n in table else (),
            reverse=False)
    elif frame_sort:
        names.sort(key=lambda n: (table[n].stack_frame,) if n in table else (),
            reverse=True)
    elif reverse_frame_sort:
        names.sort(key=lambda n: (table[n].stack_frame,) if n in table else (),
            reverse=False)

    # adjust the name width based on the expected call depth, note that we
    # can't always find the depth due to recursion
    width = 36 + (4*depth if not m.isinf(depth) else 0)

    # print header
    if not tree:
        print('%-*s' % (width, '%s%s' % (
            'file' if by_file else 'function',
            ' (%d added, %d removed)' % (
                sum(1 for n in table if n not in diff_table),
                sum(1 for n in diff_table if n not in table))
                if diff_results is not None and not percent else '')
            if not summary else ''),
            end='')
        if diff_results is None:
            print(' %s %s' % (
                'frame'.rjust(len(IntField.none)),
                'limit'.rjust(len(IntField.none))))
        elif percent:
            print(' %s %s' % (
                'frame'.rjust(len(IntField.diff_none)),
                'limit'.rjust(len(IntField.diff_none))))
        else:
            print(' %s %s %s %s %s %s' % (
                'oframe'.rjust(len(IntField.diff_none)),
                'olimit'.rjust(len(IntField.diff_none)),
                'nframe'.rjust(len(IntField.diff_none)),
                'nlimit'.rjust(len(IntField.diff_none)),
                'dframe'.rjust(len(IntField.diff_none)),
                'dlimit'.rjust(len(IntField.diff_none))))

    # print entries
    if not summary:
        # print the tree recursively
        def table_calls(names_, depth,
                prefixes=('', '', '', '')):
            for i, name in enumerate(names_):
                r = table.get(name)
                if diff_results is not None:
                    diff_r = diff_table.get(name)
                    ratio = IntField.ratio(
                        r.stack_limit if r else None,
                        diff_r.stack_limit if diff_r else None)
                    if not ratio and not all_:
                        continue

                is_last = (i == len(names_)-1)
                print('%-*s' % (width, prefixes[0+is_last] + name), end='')
                if tree:
                    print()
                elif diff_results is None:
                    print(' %s %s' % (
                        r.stack_frame.table()
                            if r else IntField.none,
                        r.stack_limit.table()
                            if r else IntField.none))
                elif percent:
                    print(' %s %s%s' % (
                        r.stack_frame.diff_table()
                            if r else IntField.diff_none,
                        r.stack_limit.diff_table()
                            if r else IntField.diff_none,
                        ' (%s)' % (
                            '+∞%' if ratio == float('+inf')
                            else '-∞%' if ratio == float('-inf')
                            else '%+.1f%%' % (100*ratio))))
                else:
                    print(' %s %s %s %s %s %s%s' % (
                        diff_r.stack_frame.diff_table()
                            if diff_r else IntField.diff_none,
                        diff_r.stack_limit.diff_table()
                            if diff_r else IntField.diff_none,
                        r.stack_frame.diff_table()
                            if r else IntField.diff_none,
                        r.stack_limit.diff_table()
                            if r else IntField.diff_none,
                        IntField.diff_diff(
                            r.stack_frame if r else None,
                            diff_r.stack_frame if diff_r else None)
                            if r or diff_r else IntField.diff_none,
                        IntField.diff_diff(
                            r.stack_limit if r else None,
                            diff_r.stack_limit if diff_r else None)
                            if r or diff_r else IntField.diff_none,
                        ' (%s)' % (
                            '+∞%' if ratio == float('+inf')
                            else '-∞%' if ratio == float('-inf')
                            else '%+.1f%%' % (100*ratio))
                            if ratio else ''))

                # recurse?
                if depth > 0:
                    cs = calls.get((name,), set())
                    table_calls(
                        [n for n in names if (n,) in cs],
                        depth-1,
                        (   prefixes[2+is_last] + "|-> ",
                            prefixes[2+is_last] + "'-> ",
                            prefixes[2+is_last] + "|   ",
                            prefixes[2+is_last] + "    "))


        table_calls(names, depth)

    # print total
    if not tree:
        total = fold(results, by=[])
        r = total[0] if total else None
        if diff_results is not None:
            diff_total = fold(diff_results, by=[])
            diff_r = diff_total[0] if diff_total else None
            ratio = IntField.ratio(
                r.stack_limit if r else None,
                diff_r.stack_limit if diff_r else None)

        print('%-*s' % (width, 'TOTAL'), end='')
        if diff_results is None:
            print(' %s %s' % (
                r.stack_frame.table()
                    if r else IntField.none,
                r.stack_limit.table()
                    if r else IntField.none))
        elif percent:
            print(' %s %s%s' % (
                r.stack_frame.diff_table()
                    if r else IntField.diff_none,
                r.stack_limit.diff_table()
                    if r else IntField.diff_none,
                ' (%s)' % (
                    '+∞%' if ratio == float('+inf')
                    else '-∞%' if ratio == float('-inf')
                    else '%+.1f%%' % (100*ratio))))
        else:
            print(' %s %s %s %s %s %s%s' % (
                diff_r.stack_frame.diff_table()
                    if diff_r else IntField.diff_none,
                diff_r.stack_limit.diff_table()
                    if diff_r else IntField.diff_none,
                r.stack_frame.diff_table()
                    if r else IntField.diff_none,
                r.stack_limit.diff_table()
                    if r else IntField.diff_none,
                IntField.diff_diff(
                    r.stack_frame if r else None,
                    diff_r.stack_frame if diff_r else None)
                    if r or diff_r else IntField.diff_none,
                IntField.diff_diff(
                    r.stack_limit if r else None,
                    diff_r.stack_limit if diff_r else None)
                    if r or diff_r else IntField.diff_none,
                ' (%s)' % (
                    '+∞%' if ratio == float('+inf')
                    else '-∞%' if ratio == float('-inf')
                    else '%+.1f%%' % (100*ratio))
                    if ratio else ''))


def main(ci_paths, **args):
    # find sizes
    if not args.get('use', None):
        # find .ci files
        paths = []
        for path in ci_paths:
            if os.path.isdir(path):
                path = path + '/*.ci'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print('no .ci files found in %r?' % ci_paths)
            sys.exit(-1)

        results, calls = collect(paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f, restval='')
            for r in reader:
                try:
                    results.append(StackResult(**{
                        k: v for k, v in r.items()
                        if k in StackResult._fields}))
                except TypeError:
                    pass

        calls = {}

    # fold to remove duplicates
    results = fold(results)

    # sort because why not
    results.sort()

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, StackResult._fields)
            writer.writeheader()
            for r in results:
                writer.writerow(r._asdict())

    # find previous results?
    if args.get('diff'):
        diff_results = []
        try:
            with openio(args['diff']) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    try:
                        diff_results.append(StackResult(**{
                            k: v for k, v in r.items()
                            if k in StackResult._fields}))
                    except TypeError:
                        pass
        except FileNotFoundError:
            pass

        # fold to remove duplicates
        diff_results = fold(diff_results)

    # print table
    if not args.get('quiet'):
        table(
            results,
            calls,
            diff_results if args.get('diff') else None,
            **args)

    # error on recursion
    if args.get('error_on_recursion') and any(
            m.isinf(float(r.stack_limit)) for r in results):
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find stack usage at the function level.")
    parser.add_argument(
        'ci_paths',
        nargs='*',
        default=CI_PATHS,
        help="Description of where to find *.ci files. May be a directory "
            "or a list of paths. Defaults to %r." % CI_PATHS)
    parser.add_argument(
        '-v', '--verbose',
        action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument(
        '-q', '--quiet',
        action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument(
        '-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument(
        '-u', '--use',
        help="Don't parse anything, use this CSV file.")
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
        '-t', '--tree',
        action='store_true',
        help="Only show the function call tree.")
    parser.add_argument(
        '-b', '--by-file',
        action='store_true',
        help="Group by file.")
    parser.add_argument(
        '-s', '--limit-sort',
        action='store_true',
        help="Sort by stack limit.")
    parser.add_argument(
        '-S', '--reverse-limit-sort',
        action='store_true',
        help="Sort by stack limit, but backwards.")
    parser.add_argument(
        '--frame-sort',
        action='store_true',
        help="Sort by stack frame.")
    parser.add_argument(
        '--reverse-frame-sort',
        action='store_true',
        help="Sort by stack frame, but backwards.")
    parser.add_argument(
        '-Y', '--summary',
        action='store_true',
        help="Only show the total size.")
    parser.add_argument(
        '-L', '--depth',
        nargs='?',
        type=lambda x: int(x, 0),
        const=float('inf'),
        help="Depth of function calls to show.")
    parser.add_argument(
        '-e', '--error-on-recursion',
        action='store_true',
        help="Error if any functions are recursive.")
    parser.add_argument(
        '-A', '--everything',
        action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument(
        '--build-dir',
        help="Specify the relative build directory. Used to map object files "
            "to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
