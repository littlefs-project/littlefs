#!/usr/bin/env python3
#
# Script to find coverage info after running tests.
#
# Example:
# ./scripts/coverage.py lfs.t.a.gcda lfs_util.t.a.gcda -s
#
# Copyright (c) 2022, The littlefs authors.
# Copyright (c) 2020, Arm Limited. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#

import collections as co
import csv
import glob
import itertools as it
import json
import math as m
import os
import re
import shlex
import subprocess as sp

# TODO use explode_asserts to avoid counting assert branches?
# TODO use dwarf=info to find functions for inline functions?


GCDA_PATHS = ['*.gcda']
GCOV_TOOL = ['gcov']


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

# fractional fields, a/b
class FracField(co.namedtuple('FracField', 'a,b')):
    __slots__ = ()
    def __new__(cls, a, b=None):
        if isinstance(a, FracField) and b is None:
            return a
        if isinstance(a, str) and b is None:
            a, b = a.split('/', 1)
        if b is None:
            b = a
        return super().__new__(cls, IntField(a), IntField(b))

    def __str__(self):
        return '%s/%s' % (self.a, self.b)

    none = '%11s %7s' % ('-', '-')
    def table(self):
        if not self.b.x:
            return self.none

        t = self.a.x/self.b.x
        return '%11s %7s' % (
            self,
            '∞%' if t == float('+inf')
            else '-∞%' if t == float('-inf')
            else '%.1f%%' % (100*t))

    diff_none = '%11s' % '-'
    def diff_table(self):
        if not self.b.x:
            return self.diff_none

        return '%11s' % (self,)

    def diff_diff(self, other):
        new_a, new_b = self if self else (IntField(0), IntField(0))
        old_a, old_b = other if other else (IntField(0), IntField(0))
        return '%11s' % ('%s/%s' % (
            new_a.diff_diff(old_a).strip(),
            new_b.diff_diff(old_b).strip()))

    def ratio(self, other):
        new_a, new_b = self if self else (IntField(0), IntField(0))
        old_a, old_b = other if other else (IntField(0), IntField(0))
        new = new_a.x/new_b.x if new_b.x else 1.0
        old = old_a.x/old_b.x if old_b.x else 1.0
        return new - old

    def __add__(self, other):
        return FracField(self.a + other.a, self.b + other.b)

    def __mul__(self, other):
        return FracField(self.a * other.a, self.b + other.b)

    def __lt__(self, other):
        self_r = self.a.x/self.b.x if self.b.x else float('-inf')
        other_r = other.a.x/other.b.x if other.b.x else float('-inf')
        return self_r < other_r

    def __gt__(self, other):
        return self.__class__.__lt__(other, self)

    def __le__(self, other):
        return not self.__gt__(other)

    def __ge__(self, other):
        return not self.__lt__(other)

    def __truediv__(self, n):
        return FracField(self.a / n, self.b / n)

# coverage results
class CoverageResult(co.namedtuple('CoverageResult',
        'file,function,line,'
        'coverage_hits,coverage_lines,coverage_branches')):
    __slots__ = ()
    def __new__(cls, file, function, line,
            coverage_hits, coverage_lines, coverage_branches):
        return super().__new__(cls, file, function, int(IntField(line)),
            IntField(coverage_hits),
            FracField(coverage_lines),
            FracField(coverage_branches))

    def __add__(self, other):
        return CoverageResult(self.file, self.function, self.line,
            max(self.coverage_hits, other.coverage_hits),
            self.coverage_lines + other.coverage_lines,
            self.coverage_branches + other.coverage_branches)


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

def collect(paths, *,
        gcov_tool=GCOV_TOOL,
        build_dir=None,
        everything=False,
        **args):
    results = []
    for path in paths:
        # map to source file
        src_path = re.sub('\.t\.a\.gcda$', '.c', path)
        if build_dir:
            src_path = re.sub('%s/*' % re.escape(build_dir), '',
                src_path)

        # get coverage info through gcov's json output
        # note, gcov-tool may contain extra args
        cmd = GCOV_TOOL + ['-b', '-t', '--json-format', path]
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))
        proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            stderr=sp.PIPE if not args.get('verbose') else None,
            universal_newlines=True,
            errors='replace')
        data = json.load(proc.stdout)
        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

        # collect line/branch coverage
        for file in data['files']:
            if file['file'] != src_path:
                continue

            for line in file['lines']:
                func = line.get('function_name', '(inlined)')
                # discard internal function (this includes injected test cases)
                if not everything:
                    if func.startswith('__'):
                        continue

                results.append(CoverageResult(
                    src_path, func, line['line_number'],
                    line['count'],
                    FracField(
                        1 if line['count'] > 0 else 0,
                        1),
                    FracField(
                        sum(1 if branch['count'] > 0 else 0
                            for branch in line['branches']),
                        len(line['branches']))))

    return results


def fold(results, *,
        by=['file', 'function', 'line'],
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


def table(results, diff_results=None, *,
        by_file=False,
        by_line=False,
        line_sort=False,
        reverse_line_sort=False,
        branch_sort=False,
        reverse_branch_sort=False,
        summary=False,
        all=False,
        percent=False,
        **_):
    all_, all = all, __builtins__.all

    # fold
    results = fold(results,
        by=['file', 'line'] if by_line
            else ['file'] if by_file
            else ['function'])
    if diff_results is not None:
        diff_results = fold(diff_results,
            by=['file', 'line'] if by_line
                else ['file'] if by_file
                else ['function'])

    table = {
        (r.file, r.line) if by_line
            else r.file if by_file
            else r.function: r
        for r in results}
    diff_table = {
        (r.file, r.line) if by_line
            else r.file if by_file
            else r.function: r
        for r in diff_results or []}

    # sort, note that python's sort is stable
    names = list(table.keys() | diff_table.keys())
    names.sort()
    if diff_results is not None:
        names.sort(key=lambda n: (
            -FracField.ratio(
                table[n].coverage_lines if n in table else None,
                diff_table[n].coverage_lines if n in diff_table else None),
            -FracField.ratio(
                table[n].coverage_branches if n in table else None,
                diff_table[n].coverage_branches if n in diff_table else None)))
    if line_sort:
        names.sort(key=lambda n: (table[n].coverage_lines,)
            if n in table else (),
            reverse=True)
    elif reverse_line_sort:
        names.sort(key=lambda n: (table[n].coverage_lines,)
            if n in table else (),
            reverse=False)
    elif branch_sort:
        names.sort(key=lambda n: (table[n].coverage_branches,)
            if n in table else (),
            reverse=True)
    elif reverse_branch_sort:
        names.sort(key=lambda n: (table[n].coverage_branches,)
            if n in table else (),
            reverse=False)

    # print header
    print('%-36s' % ('%s%s' % (
        'line' if by_line
            else 'file' if by_file
            else 'function',
        ' (%d added, %d removed)' % (
            sum(1 for n in table if n not in diff_table),
            sum(1 for n in diff_table if n not in table))
            if diff_results is not None and not percent else '')
        if not summary else ''),
        end='')
    if diff_results is None:
        print(' %s %s' % (
            'hits/line'.rjust(len(FracField.none)),
            'hits/branch'.rjust(len(FracField.none))))
    elif percent:
        print(' %s %s' % (
            'hits/line'.rjust(len(FracField.diff_none)),
            'hits/branch'.rjust(len(FracField.diff_none))))
    else:
        print(' %s %s %s %s %s %s' % (
            'oh/line'.rjust(len(FracField.diff_none)),
            'oh/branch'.rjust(len(FracField.diff_none)),
            'nh/line'.rjust(len(FracField.diff_none)),
            'nh/branch'.rjust(len(FracField.diff_none)),
            'dh/line'.rjust(len(FracField.diff_none)),
            'dh/branch'.rjust(len(FracField.diff_none))))

    # print entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is not None:
                diff_r = diff_table.get(name)
                line_ratio = FracField.ratio(
                    r.coverage_lines if r else None,
                    diff_r.coverage_lines if diff_r else None)
                branch_ratio = FracField.ratio(
                    r.coverage_branches if r else None,
                    diff_r.coverage_branches if diff_r else None)
                if not line_ratio and not branch_ratio and not all_:
                    continue

            print('%-36s' % (
                ':'.join('%s' % n for n in name)
                if by_line else name), end='')
            if diff_results is None:
                print(' %s %s' % (
                    r.coverage_lines.table()
                        if r else FracField.none,
                    r.coverage_branches.table()
                        if r else FracField.none))
            elif percent:
                print(' %s %s%s' % (
                    r.coverage_lines.diff_table()
                        if r else FracField.diff_none,
                    r.coverage_branches.diff_table()
                        if r else FracField.diff_none,
                    ' (%s)' % ', '.join(
                            '+∞%' if t == float('+inf')
                            else '-∞%' if t == float('-inf')
                            else '%+.1f%%' % (100*t)
                            for t in [line_ratio, branch_ratio])))
            else:
                print(' %s %s %s %s %s %s%s' % (
                    diff_r.coverage_lines.diff_table()
                        if diff_r else FracField.diff_none,
                    diff_r.coverage_branches.diff_table()
                        if diff_r else FracField.diff_none,
                    r.coverage_lines.diff_table()
                        if r else FracField.diff_none,
                    r.coverage_branches.diff_table()
                        if r else FracField.diff_none,
                    FracField.diff_diff(
                        r.coverage_lines if r else None,
                        diff_r.coverage_lines if diff_r else None)
                        if r or diff_r else FracField.diff_none,
                    FracField.diff_diff(
                        r.coverage_branches if r else None,
                        diff_r.coverage_branches if diff_r else None)
                        if r or diff_r else FracField.diff_none,
                    ' (%s)' % ', '.join(
                            '+∞%' if t == float('+inf')
                            else '-∞%' if t == float('-inf')
                            else '%+.1f%%' % (100*t)
                            for t in [line_ratio, branch_ratio]
                            if t)
                        if line_ratio or branch_ratio else ''))

    # print total
    total = fold(results, by=[])
    r = total[0] if total else None
    if diff_results is not None:
        diff_total = fold(diff_results, by=[])
        diff_r = diff_total[0] if diff_total else None
        line_ratio = FracField.ratio(
            r.coverage_lines if r else None,
            diff_r.coverage_lines if diff_r else None)
        branch_ratio = FracField.ratio(
            r.coverage_branches if r else None,
            diff_r.coverage_branches if diff_r else None)

    print('%-36s' % 'TOTAL', end='')
    if diff_results is None:
        print(' %s %s' % (
            r.coverage_lines.table()
                if r else FracField.none,
            r.coverage_branches.table()
                if r else FracField.none))
    elif percent:
        print(' %s %s%s' % (
            r.coverage_lines.diff_table()
                if r else FracField.diff_none,
            r.coverage_branches.diff_table()
                if r else FracField.diff_none,
            ' (%s)' % ', '.join(
                    '+∞%' if t == float('+inf')
                    else '-∞%' if t == float('-inf')
                    else '%+.1f%%' % (100*t)
                    for t in [line_ratio, branch_ratio])))
    else:
        print(' %s %s %s %s %s %s%s' % (
            diff_r.coverage_lines.diff_table()
                if diff_r else FracField.diff_none,
            diff_r.coverage_branches.diff_table()
                if diff_r else FracField.diff_none,
            r.coverage_lines.diff_table()
                if r else FracField.diff_none,
            r.coverage_branches.diff_table()
                if r else FracField.diff_none,
            FracField.diff_diff(
                r.coverage_lines if r else None,
                diff_r.coverage_lines if diff_r else None)
                if r or diff_r else FracField.diff_none,
            FracField.diff_diff(
                r.coverage_branches if r else None,
                diff_r.coverage_branches if diff_r else None)
                if r or diff_r else FracField.diff_none,
            ' (%s)' % ', '.join(
                    '+∞%' if t == float('+inf')
                    else '-∞%' if t == float('-inf')
                    else '%+.1f%%' % (100*t)
                    for t in [line_ratio, branch_ratio]
                    if t)
                if line_ratio or branch_ratio else ''))


def annotate(paths, results, *,
        annotate=False,
        lines=False,
        branches=False,
        build_dir=None,
        **args):
    for path in paths:
        # map to source file
        src_path = re.sub('\.t\.a\.gcda$', '.c', path)
        if build_dir:
            src_path = re.sub('%s/*' % re.escape(build_dir), '',
                src_path)

        # flatten to line info
        results = fold(results, by=['file', 'line'])
        table = {r.line: r for r in results if r.file == src_path}

        # calculate spans to show
        if not annotate:
            spans = []
            last = None
            for line, r in sorted(table.items()):
                if ((lines and int(r.coverage_hits) == 0)
                        or (branches
                            and r.coverage_branches.a
                                < r.coverage_branches.b)):
                    if last is not None and line - last.stop <= args['context']:
                        last = range(
                            last.start,
                            line+1+args['context'])
                    else:
                        if last is not None:
                            spans.append(last)
                        last = range(
                            line-args['context'],
                            line+1+args['context'])
            if last is not None:
                spans.append(last)

        with open(src_path) as f:
            skipped = False
            for i, line in enumerate(f):
                # skip lines not in spans?
                if not annotate and not any(i+1 in s for s in spans):
                    skipped = True
                    continue

                if skipped:
                    skipped = False
                    print('%s@@ %s:%d @@%s' % (
                        '\x1b[36m' if args['color'] else '',
                        src_path,
                        i+1,
                        '\x1b[m' if args['color'] else ''))

                # build line
                if line.endswith('\n'):
                    line = line[:-1]

                if i+1 in table:
                    r = table[i+1]
                    line = '%-*s // %s hits, %s branches' % (
                        args['width'],
                        line,
                        r.coverage_hits,
                        r.coverage_branches)

                    if args['color']:
                        if lines and int(r.coverage_hits) == 0:
                            line = '\x1b[1;31m%s\x1b[m' % line
                        elif (branches
                                and r.coverage_branches.a
                                    < r.coverage_branches.b):
                            line = '\x1b[35m%s\x1b[m' % line

                print(line)


def main(gcda_paths, **args):
    # figure out what color should be
    if args.get('color') == 'auto':
        args['color'] = sys.stdout.isatty()
    elif args.get('color') == 'always':
        args['color'] = True
    else:
        args['color'] = False

    # find sizes
    if not args.get('use', None):
        # find .gcda files
        paths = []
        for path in gcda_paths:
            if os.path.isdir(path):
                path = path + '/*.gcda'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print('no .gcda files found in %r?' % gcda_paths)
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f)
            for r in reader:
                try:
                    results.append(CoverageResult(**{
                        k: v for k, v in r.items()
                        if k in CoverageResult._fields}))
                except TypeError:
                    pass

    # fold to remove duplicates
    results = fold(results)

    # sort because why not
    results.sort()

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, CoverageResult._fields)
            writer.writeheader()
            for r in results:
                writer.writerow(r._asdict())

    # find previous results?
    if args.get('diff'):
        diff_results = []
        try:
            with openio(args['diff']) as f:
                reader = csv.DictReader(f)
                for r in reader:
                    try:
                        diff_results.append(CoverageResult(**{
                            k: v for k, v in r.items()
                            if k in CoverageResult._fields}))
                    except TypeError:
                        pass
        except FileNotFoundError:
            pass

        # fold to remove duplicates
        diff_results = fold(diff_results)

    if not args.get('quiet'):
        if (args.get('annotate')
                or args.get('lines')
                or args.get('branches')):
            # annotate sources
            annotate(
                paths,
                results,
                **args)
        else:
            # print table
            table(
                results,
                diff_results if args.get('diff') else None,
                **args)

    # catch lack of coverage
    if args.get('error_on_lines') and any(
            r.coverage_lines.a < r.coverage_lines.b for r in results):
        sys.exit(2)
    elif args.get('error_on_branches') and any(
            r.coverage_branches.a < r.coverage_branches.b for r in results):
        sys.exit(3)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find coverage info after running tests.")
    parser.add_argument(
        'gcda_paths',
        nargs='*',
        default=GCDA_PATHS,
        help="Description of where to find *.gcda files. May be a directory "
            "or a list of paths. Defaults to %r." % GCDA_PATHS)
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
        '-b', '--by-file',
        action='store_true',
        help="Group by file.")
    parser.add_argument(
        '--by-line',
        action='store_true',
        help="Group by line.")
    parser.add_argument(
        '-s', '--line-sort',
        action='store_true',
        help="Sort by line coverage.")
    parser.add_argument(
        '-S', '--reverse-line-sort',
        action='store_true',
        help="Sort by line coverage, but backwards.")
    parser.add_argument(
        '--branch-sort',
        action='store_true',
        help="Sort by branch coverage.")
    parser.add_argument(
        '--reverse-branch-sort',
        action='store_true',
        help="Sort by branch coverage, but backwards.")
    parser.add_argument(
        '-Y', '--summary',
        action='store_true',
        help="Only show the total size.")
    parser.add_argument(
        '-l', '--annotate',
        action='store_true',
        help="Show source files annotated with coverage info.")
    parser.add_argument(
        '-L', '--lines',
        action='store_true',
        help="Show uncovered lines.")
    parser.add_argument(
        '-B', '--branches',
        action='store_true',
        help="Show uncovered branches.")
    parser.add_argument(
        '-c', '--context',
        type=lambda x: int(x, 0),
        default=3,
        help="Show a additional lines of context. Defaults to 3.")
    parser.add_argument(
        '-W', '--width',
        type=lambda x: int(x, 0),
        default=80,
        help="Assume source is styled with this many columns. Defaults to 80.")
    parser.add_argument(
        '--color',
        choices=['never', 'always', 'auto'],
        default='auto',
        help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
        '-e', '--error-on-lines',
        action='store_true',
        help="Error if any lines are not covered.")
    parser.add_argument(
        '-E', '--error-on-branches',
        action='store_true',
        help="Error if any branches are not covered.")
    parser.add_argument(
        '-A', '--everything',
        action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument(
        '--gcov-tool',
        default=GCOV_TOOL,
        type=lambda x: x.split(),
        help="Path to the gcov tool to use. Defaults to %r." % GCOV_TOOL)
    parser.add_argument(
        '--build-dir',
        help="Specify the relative build directory. Used to map object files "
            "to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
