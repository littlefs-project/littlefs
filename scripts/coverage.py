#!/usr/bin/env python3
#
# Script to find test coverage. Basically just a big wrapper around gcov with
# some extra conveniences for comparing builds. Heavily inspired by Linux's
# Bloat-O-Meter.
#

import collections as co
import csv
import glob
import itertools as it
import json
import os
import re
import shlex
import subprocess as sp

# TODO use explode_asserts to avoid counting assert branches?
# TODO use dwarf=info to find functions for inline functions?


GCDA_PATHS = ['*.gcda']

class CoverageResult(co.namedtuple('CoverageResult',
        'coverage_line_hits,coverage_line_count,'
        'coverage_branch_hits,coverage_branch_count')):
    __slots__ = ()
    def __new__(cls,
            coverage_line_hits=0, coverage_line_count=0,
            coverage_branch_hits=0, coverage_branch_count=0):
        return super().__new__(cls,
            int(coverage_line_hits),
            int(coverage_line_count),
            int(coverage_branch_hits),
            int(coverage_branch_count))

    def __add__(self, other):
        return self.__class__(
            self.coverage_line_hits + other.coverage_line_hits,
            self.coverage_line_count + other.coverage_line_count,
            self.coverage_branch_hits + other.coverage_branch_hits,
            self.coverage_branch_count + other.coverage_branch_count)

    def __sub__(self, other):
        return CoverageDiff(other, self)

    def __rsub__(self, other):
        return self.__class__.__sub__(other, self)

    def key(self, **args):
        ratio_line = (self.coverage_line_hits/self.coverage_line_count
            if self.coverage_line_count else -1)
        ratio_branch = (self.coverage_branch_hits/self.coverage_branch_count
            if self.coverage_branch_count else -1)

        if args.get('line_sort'):
            return (-ratio_line, -ratio_branch)
        elif args.get('reverse_line_sort'):
            return (+ratio_line, +ratio_branch)
        elif args.get('branch_sort'):
            return (-ratio_branch, -ratio_line)
        elif args.get('reverse_branch_sort'):
            return (+ratio_branch, +ratio_line)
        else:
            return None

    _header = '%19s %19s' % ('hits/line', 'hits/branch')
    def __str__(self):
        line_hits = self.coverage_line_hits
        line_count = self.coverage_line_count
        branch_hits = self.coverage_branch_hits
        branch_count = self.coverage_branch_count
        return '%11s %7s %11s %7s' % (
            '%d/%d' % (line_hits, line_count)
                if line_count else '-',
            '%.1f%%' % (100*line_hits/line_count)
                if line_count else '-',
            '%d/%d' % (branch_hits, branch_count)
                if branch_count else '-',
            '%.1f%%' % (100*branch_hits/branch_count)
                if branch_count else '-')

class CoverageDiff(co.namedtuple('CoverageDiff', 'old,new')):
    __slots__ = ()

    def ratio_line(self):
        old_line_hits = (self.old.coverage_line_hits
            if self.old is not None else 0)
        old_line_count = (self.old.coverage_line_count
            if self.old is not None else 0)
        new_line_hits = (self.new.coverage_line_hits
            if self.new is not None else 0)
        new_line_count = (self.new.coverage_line_count
            if self.new is not None else 0)
        return ((new_line_hits/new_line_count if new_line_count else 1.0)
            - (old_line_hits/old_line_count if old_line_count else 1.0))

    def ratio_branch(self):
        old_branch_hits = (self.old.coverage_branch_hits
            if self.old is not None else 0)
        old_branch_count = (self.old.coverage_branch_count
            if self.old is not None else 0)
        new_branch_hits = (self.new.coverage_branch_hits
            if self.new is not None else 0)
        new_branch_count = (self.new.coverage_branch_count
            if self.new is not None else 0)
        return ((new_branch_hits/new_branch_count if new_branch_count else 1.0)
            - (old_branch_hits/old_branch_count if old_branch_count else 1.0))

    def key(self, **args):
        return (
            self.new.key(**args) if self.new is not None else 0,
            -self.ratio_line(),
            -self.ratio_branch())

    def __bool__(self):
        return bool(self.ratio_line() or self.ratio_branch())

    _header = '%23s %23s %23s' % ('old', 'new', 'diff')
    def __str__(self):
        old_line_hits = (self.old.coverage_line_hits
            if self.old is not None else 0)
        old_line_count = (self.old.coverage_line_count
            if self.old is not None else 0)
        old_branch_hits = (self.old.coverage_branch_hits
            if self.old is not None else 0)
        old_branch_count = (self.old.coverage_branch_count
            if self.old is not None else 0)
        new_line_hits = (self.new.coverage_line_hits
            if self.new is not None else 0)
        new_line_count = (self.new.coverage_line_count
            if self.new is not None else 0)
        new_branch_hits = (self.new.coverage_branch_hits
            if self.new is not None else 0)
        new_branch_count = (self.new.coverage_branch_count
            if self.new is not None else 0)
        diff_line_hits = new_line_hits - old_line_hits
        diff_line_count = new_line_count - old_line_count
        diff_branch_hits = new_branch_hits - old_branch_hits
        diff_branch_count = new_branch_count - old_branch_count
        ratio_line = self.ratio_line()
        ratio_branch = self.ratio_branch()
        return '%11s %11s %11s %11s %11s %11s%-10s%s' % (
            '%d/%d' % (old_line_hits, old_line_count)
                if old_line_count else '-',
            '%d/%d' % (old_branch_hits, old_branch_count)
                if old_branch_count else '-',
            '%d/%d' % (new_line_hits, new_line_count)
                if new_line_count else '-',
            '%d/%d' % (new_branch_hits, new_branch_count)
                if new_branch_count else '-',
            '%+d/%+d' % (diff_line_hits, diff_line_count),
            '%+d/%+d' % (diff_branch_hits, diff_branch_count),
            ' (%+.1f%%)' % (100*ratio_line) if ratio_line else '',
            ' (%+.1f%%)' % (100*ratio_branch) if ratio_branch else '')


def openio(path, mode='r'):
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

def color(**args):
    if args.get('color') == 'auto':
        return sys.stdout.isatty()
    elif args.get('color') == 'always':
        return True
    else:
        return False

def collect(paths, **args):
    results = {}
    for path in paths:
        # map to source file
        src_path = re.sub('\.t\.a\.gcda$', '.c', path)
        # TODO test this
        if args.get('build_dir'):
            src_path = re.sub('%s/*' % re.escape(args['build_dir']), '',
                src_path)

        # get coverage info through gcov's json output
        # note, gcov-tool may contain extra args
        cmd = args['gcov_tool'] + ['-b', '-t', '--json-format', path]
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
                if not args.get('everything'):
                    if func.startswith('__'):
                        continue

                results[(src_path, func, line['line_number'])] = (
                    line['count'],
                    CoverageResult(
                        coverage_line_hits=1 if line['count'] > 0 else 0,
                        coverage_line_count=1,
                        coverage_branch_hits=sum(
                            1 if branch['count'] > 0 else 0
                            for branch in line['branches']),
                        coverage_branch_count=len(line['branches'])))

    # merge into functions, since this is what other scripts use
    func_results = co.defaultdict(lambda: CoverageResult())
    for (file, func, _), (_, result) in results.items():
        func_results[(file, func)] += result

    return func_results, results

def annotate(paths, results, **args):
    for path in paths:
        # map to source file
        src_path = re.sub('\.t\.a\.gcda$', '.c', path)
        # TODO test this
        if args.get('build_dir'):
            src_path = re.sub('%s/*' % re.escape(args['build_dir']), '',
                src_path)

        # flatten to line info
        line_results = {line: (hits, result)
            for (_, _, line), (hits, result) in results.items()}

        # calculate spans to show
        if not args.get('annotate'):
            spans = []
            last = None
            for line, (hits, result) in sorted(line_results.items()):
                if ((args.get('lines') and hits == 0)
                        or (args.get('branches')
                            and result.coverage_branch_hits
                                < result.coverage_branch_count)):
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
                if (not args.get('annotate')
                        and not any(i+1 in s for s in spans)):
                    skipped = True
                    continue

                if skipped:
                    skipped = False
                    print('%s@@ %s:%d @@%s' % (
                        '\x1b[36m' if color(**args) else '',
                        src_path,
                        i+1,
                        '\x1b[m' if color(**args) else ''))

                # build line
                if line.endswith('\n'):
                    line = line[:-1]

                if i+1 in line_results:
                    hits, result = line_results[i+1]
                    line = '%-*s // %d hits, %d/%d branches' % (
                        args['width'],
                        line,
                        hits,
                        result.coverage_branch_hits,
                        result.coverage_branch_count)

                    if color(**args):
                        if args.get('lines') and hits == 0:
                            line = '\x1b[1;31m%s\x1b[m' % line
                        elif (args.get('branches') and
                                result.coverage_branch_hits
                                < result.coverage_branch_count):
                            line = '\x1b[35m%s\x1b[m' % line

                print(line)

def main(**args):
    # find sizes
    if not args.get('use', None):
        # find .gcda files
        paths = []
        for path in args['gcda_paths']:
            if os.path.isdir(path):
                path = path + '/*.gcda'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print('no .gcda files found in %r?' % args['gcda_paths'])
            sys.exit(-1)

        results, line_results = collect(paths, **args)
    else:
        with openio(args['use']) as f:
            r = csv.DictReader(f)
            results = {
                (result['file'], result['name']): CoverageResult(
                    *(result[f] for f in CoverageResult._fields))
                for result in r
                if all(result.get(f) not in {None, ''}

                    for f in CoverageResult._fields)}
        paths = []
        line_results = {}

    # find previous results?
    if args.get('diff'):
        try:
            with openio(args['diff']) as f:
                r = csv.DictReader(f)
                prev_results = {
                    (result['file'], result['name']): CoverageResult(
                        *(result[f] for f in CoverageResult._fields))
                    for result in r
                    if all(result.get(f) not in {None, ''}
                        for f in CoverageResult._fields)}
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
                        for f in CoverageResult._fields:
                            result.pop(f, None)
                        merged_results[(file, func)] = result
                        other_fields = result.keys()
            except FileNotFoundError:
                pass

        for (file, func), result in results.items():
            merged_results[(file, func)] |= result._asdict()

        with openio(args['output'], 'w') as f:
            w = csv.DictWriter(f, ['file', 'name',
                *other_fields, *CoverageResult._fields])
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
            print('%-36s %s' % (by, CoverageResult._header))
        else:
            old = {entry(k) for k in results.keys()}
            new = {entry(k) for k in prev_results.keys()}
            print('%-36s %s' % (
                '%s (%d added, %d removed)' % (by,
                        sum(1 for k in new if k not in old),
                        sum(1 for k in old if k not in new))
                    if by else '',
                CoverageDiff._header))

    def print_entries(by):
        if by == 'total':
            entry = lambda k: 'TOTAL'
        elif by == 'file':
            entry = lambda k: k[0]
        else:
            entry = lambda k: k[1]

        entries = co.defaultdict(lambda: CoverageResult())
        for k, result in results.items():
            entries[entry(k)] += result

        if not args.get('diff'):
            for name, result in sorted(entries.items(),
                    key=lambda p: (p[1].key(**args), p)):
                print('%-36s %s' % (name, result))
        else:
            prev_entries = co.defaultdict(lambda: CoverageResult())
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
    elif (args.get('annotate')
            or args.get('lines')
            or args.get('branches')):
        annotate(paths, line_results, **args)
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

    # catch lack of coverage
    if args.get('error_on_lines') and any(
            r.coverage_line_hits < r.coverage_line_count
            for r in results.values()):
        sys.exit(2)
    elif args.get('error_on_branches') and any(
            r.coverage_branch_hits < r.coverage_branch_count
            for r in results.values()):
        sys.exit(3)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find coverage info after running tests.")
    parser.add_argument('gcda_paths', nargs='*', default=GCDA_PATHS,
        help="Description of where to find *.gcda files. May be a directory \
            or a list of paths. Defaults to %r." % GCDA_PATHS)
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
    parser.add_argument('-s', '--line-sort', action='store_true',
        help="Sort by line coverage.")
    parser.add_argument('-S', '--reverse-line-sort', action='store_true',
        help="Sort by line coverage, but backwards.")
    parser.add_argument('--branch-sort', action='store_true',
        help="Sort by branch coverage.")
    parser.add_argument('--reverse-branch-sort', action='store_true',
        help="Sort by branch coverage, but backwards.")
    parser.add_argument('-F', '--files', action='store_true',
        help="Show file-level coverage.")
    parser.add_argument('-Y', '--summary', action='store_true',
        help="Only show the total coverage.")
    parser.add_argument('-p', '--annotate', action='store_true',
        help="Show source files annotated with coverage info.")
    parser.add_argument('-l', '--lines', action='store_true',
        help="Show uncovered lines.")
    parser.add_argument('-b', '--branches', action='store_true',
        help="Show uncovered branches.")
    parser.add_argument('-c', '--context', type=lambda x: int(x, 0), default=3,
        help="Show a additional lines of context. Defaults to 3.")
    parser.add_argument('-w', '--width', type=lambda x: int(x, 0), default=80,
        help="Assume source is styled with this many columns. Defaults to 80.")
    parser.add_argument('--color',
        choices=['never', 'always', 'auto'], default='auto',
        help="When to use terminal colors.")
    parser.add_argument('-e', '--error-on-lines', action='store_true',
        help="Error if any lines are not covered.")
    parser.add_argument('-E', '--error-on-branches', action='store_true',
        help="Error if any branches are not covered.")
    parser.add_argument('--gcov-tool', default=['gcov'],
        type=lambda x: x.split(),
        help="Path to the gcov tool to use.")
    parser.add_argument('--build-dir',
        help="Specify the relative build directory. Used to map object files \
            to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_args()).items()
        if v is not None}))
