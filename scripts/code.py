#!/usr/bin/env python3
#
# Script to find code size at the function level. Basically just a bit wrapper
# around nm with some extra conveniences for comparing builds. Heavily inspired
# by Linux's Bloat-O-Meter.
#

import collections as co
import csv
import glob
import itertools as it
import math as m
import os
import re
import shlex
import subprocess as sp


OBJ_PATHS = ['*.o']
NM_TOOL = ['nm']
TYPE = 'tTrRdD'


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

# code size results
class CodeResult(co.namedtuple('CodeResult', 'file,function,code_size')):
    __slots__ = ()
    def __new__(cls, file, function, code_size):
        return super().__new__(cls, file, function, IntField(code_size))

    def __add__(self, other):
        return CodeResult(self.file, self.function,
            self.code_size + other.code_size)


def openio(path, mode='r'):
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

def collect(paths, *,
        nm_tool=NM_TOOL,
        type=TYPE,
        build_dir=None,
        everything=False,
        **args):
    results = []
    pattern = re.compile(
        '^(?P<size>[0-9a-fA-F]+)' +
        ' (?P<type>[%s])' % re.escape(type) +
        ' (?P<func>.+?)$')
    for path in paths:
        # map to source file
        src_path = re.sub('\.o$', '.c', path)
        if build_dir:
            src_path = re.sub('%s/*' % re.escape(build_dir), '',
                src_path)
        # note nm-tool may contain extra args
        cmd = nm_tool + ['--size-sort', path]
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
                if not everything and func.startswith('__'):
                    continue
                # discard .8449 suffixes created by optimizer
                func = re.sub('\.[0-9]+', '', func)

                results.append(CodeResult(
                    src_path, func,
                    int(m.group('size'), 16)))

        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

    return results


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


def table(results, diff_results=None, *,
        by_file=False,
        size_sort=False,
        reverse_size_sort=False,
        summary=False,
        all=False,
        percent=False,
        **_):
    all_, all = all, __builtins__.all

    # fold
    results = fold(results, by=['file' if by_file else 'function'])
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
            table[n].code_size if n in table else None,
            diff_table[n].code_size if n in diff_table else None))
    if size_sort:
        names.sort(key=lambda n: (table[n].code_size,) if n in table else (),
            reverse=True)
    elif reverse_size_sort:
        names.sort(key=lambda n: (table[n].code_size,) if n in table else (),
            reverse=False)

    # print header
    print('%-36s' % ('%s%s' % (
        'file' if by_file else 'function',
        ' (%d added, %d removed)' % (
            sum(1 for n in table if n not in diff_table),
            sum(1 for n in diff_table if n not in table))
            if diff_results is not None and not percent else '')
        if not summary else ''),
        end='')
    if diff_results is None:
        print(' %s' % ('size'.rjust(len(IntField.none))))
    elif percent:
        print(' %s' % ('size'.rjust(len(IntField.diff_none))))
    else:
        print(' %s %s %s' % (
            'old'.rjust(len(IntField.diff_none)),
            'new'.rjust(len(IntField.diff_none)),
            'diff'.rjust(len(IntField.diff_none))))

    # print entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is not None:
                diff_r = diff_table.get(name)
                ratio = IntField.ratio(
                    r.code_size if r else None,
                    diff_r.code_size if diff_r else None)
                if not ratio and not all_:
                    continue

            print('%-36s' % name, end='')
            if diff_results is None:
                print(' %s' % (
                    r.code_size.table()
                        if r else IntField.none))
            elif percent:
                print(' %s%s' % (
                    r.code_size.diff_table()
                        if r else IntField.diff_none,
                    ' (%s)' % (
                        '+∞%' if ratio == float('+inf')
                        else '-∞%' if ratio == float('-inf')
                        else '%+.1f%%' % (100*ratio))))
            else:
                print(' %s %s %s%s' % (
                    diff_r.code_size.diff_table()
                        if diff_r else IntField.diff_none,
                    r.code_size.diff_table()
                        if r else IntField.diff_none,
                    IntField.diff_diff(
                        r.code_size if r else None,
                        diff_r.code_size if diff_r else None)
                        if r or diff_r else IntField.diff_none,
                    ' (%s)' % (
                        '+∞%' if ratio == float('+inf')
                        else '-∞%' if ratio == float('-inf')
                        else '%+.1f%%' % (100*ratio))
                        if ratio else ''))

    # print total
    total = fold(results, by=[])
    r = total[0] if total else None
    if diff_results is not None:
        diff_total = fold(diff_results, by=[])
        diff_r = diff_total[0] if diff_total else None
        ratio = IntField.ratio(
            r.code_size if r else None,
            diff_r.code_size if diff_r else None)

    print('%-36s' % 'TOTAL', end='')
    if diff_results is None:
        print(' %s' % (
            r.code_size.table()
                if r else IntField.none))
    elif percent:
        print(' %s%s' % (
            r.code_size.diff_table()
                if r else IntField.diff_none,
            ' (%s)' % (
                '+∞%' if ratio == float('+inf')
                else '-∞%' if ratio == float('-inf')
                else '%+.1f%%' % (100*ratio))))
    else:
        print(' %s %s %s%s' % (
            diff_r.code_size.diff_table()
                if diff_r else IntField.diff_none,
            r.code_size.diff_table()
                if r else IntField.diff_none,
            IntField.diff_diff(
                r.code_size if r else None,
                diff_r.code_size if diff_r else None)
                if r or diff_r else IntField.diff_none,
            ' (%s)' % (
                '+∞%' if ratio == float('+inf')
                else '-∞%' if ratio == float('-inf')
                else '%+.1f%%' % (100*ratio))
                if ratio else ''))


def main(obj_paths, **args):
    # find sizes
    if not args.get('use', None):
        # find .o files
        paths = []
        for path in obj_paths:
            if os.path.isdir(path):
                path = path + '/*.o'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print('no .obj files found in %r?' % obj_paths)
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f)
            for r in reader:
                try:
                    results.append(CodeResult(**{
                        k: v for k, v in r.items()
                        if k in CodeResult._fields}))
                except TypeError:
                    pass

    # fold to remove duplicates
    results = fold(results)

    # sort because why not
    results.sort()

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, CodeResult._fields)
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
                        diff_results.append(CodeResult(**{
                            k: v for k, v in r.items()
                            if k in CodeResult._fields}))
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
            diff_results if args.get('diff') else None,
            **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find code size at the function level.")
    parser.add_argument(
        'obj_paths',
        nargs='*',
        default=OBJ_PATHS,
        help="Description of where to find *.o files. May be a directory "
            "or a list of paths. Defaults to %(default)r.")
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
        help="Group by file. Note this does not include padding "
            "so sizes may differ from other tools.")
    parser.add_argument(
        '-s', '--size-sort',
        action='store_true',
        help="Sort by size.")
    parser.add_argument(
        '-S', '--reverse-size-sort',
        action='store_true',
        help="Sort by size, but backwards.")
    parser.add_argument(
        '-Y', '--summary',
        action='store_true',
        help="Only show the total size.")
    parser.add_argument(
        '-A', '--everything',
        action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument(
        '--type',
        default=TYPE,
        help="Type of symbols to report, this uses the same single-character "
            "type-names emitted by nm. Defaults to %(default)r.")
    parser.add_argument(
        '--nm-tool',
        type=lambda x: x.split(),
        default=NM_TOOL,
        help="Path to the nm tool to use. Defaults to %(default)r")
    parser.add_argument(
        '--build-dir',
        help="Specify the relative build directory. Used to map object files "
            "to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_args()).items()
        if v is not None}))
