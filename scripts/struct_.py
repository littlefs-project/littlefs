#!/usr/bin/env python3
#
# Script to find struct sizes.
#
# Example:
# ./scripts/struct_.py lfs.o lfs_util.o -S
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
import shlex
import subprocess as sp


OBJ_PATHS = ['*.o']
OBJDUMP_TOOL = ['objdump']


# integer fields
class IntField(co.namedtuple('IntField', 'x')):
    __slots__ = ()
    def __new__(cls, x=0):
        if isinstance(x, IntField):
            return x
        if isinstance(x, str):
            try:
                x = int(x, 0)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', x):
                    x = m.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', x):
                    x = -m.inf
                else:
                    raise
        assert isinstance(x, int) or m.isinf(x), x
        return super().__new__(cls, x)

    def __str__(self):
        if self.x == m.inf:
            return '∞'
        elif self.x == -m.inf:
            return '-∞'
        else:
            return str(self.x)

    def __int__(self):
        assert not m.isinf(self.x)
        return self.x

    def __float__(self):
        return float(self.x)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    diff_none = '%7s' % '-'
    diff_table = table

    def diff_diff(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        diff = new - old
        if diff == +m.inf:
            return '%7s' % '+∞'
        elif diff == -m.inf:
            return '%7s' % '-∞'
        else:
            return '%+7d' % diff

    def ratio(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        if m.isinf(new) and m.isinf(old):
            return 0.0
        elif m.isinf(new):
            return +m.inf
        elif m.isinf(old):
            return -m.inf
        elif not old and not new:
            return 0.0
        elif not old:
            return 1.0
        else:
            return (new-old) / old

    def __add__(self, other):
        return IntField(self.x + other.x)

    def __sub__(self, other):
        return IntField(self.x - other.x)

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

# struct size results
class StructResult(co.namedtuple('StructResult', 'file,struct,struct_size')):
    __slots__ = ()
    def __new__(cls, file, struct, struct_size):
        return super().__new__(cls, file, struct, IntField(struct_size))

    def __add__(self, other):
        return StructResult(self.file, self.struct,
            self.struct_size + other.struct_size)


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

def collect(paths, *,
        objdump_tool=OBJDUMP_TOOL,
        build_dir=None,
        everything=False,
        **args):
    decl_pattern = re.compile(
        '^\s+(?P<no>[0-9]+)'
            '\s+(?P<dir>[0-9]+)'
            '\s+.*'
            '\s+(?P<file>[^\s]+)$')
    struct_pattern = re.compile(
        '^(?:.*DW_TAG_(?P<tag>[a-z_]+).*'
            '|^.*DW_AT_name.*:\s*(?P<name>[^:\s]+)\s*'
            '|^.*DW_AT_decl_file.*:\s*(?P<decl>[0-9]+)\s*'
            '|^.*DW_AT_byte_size.*:\s*(?P<size>[0-9]+)\s*)$')

    results = []
    for path in paths:
        # find decl, we want to filter by structs in .h files
        decls = {}
        # note objdump-tool may contain extra args
        cmd = objdump_tool + ['--dwarf=rawline', path]
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))
        proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            stderr=sp.PIPE if not args.get('verbose') else None,
            universal_newlines=True,
            errors='replace')
        for line in proc.stdout:
            # find file numbers
            m = decl_pattern.match(line)
            if m:
                decls[int(m.group('no'))] = m.group('file')
        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

        # collect structs as we parse dwarf info
        found = False
        name = None
        decl = None
        size = None

        # note objdump-tool may contain extra args
        cmd = objdump_tool + ['--dwarf=info', path]
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))
        proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            stderr=sp.PIPE if not args.get('verbose') else None,
            universal_newlines=True,
            errors='replace')
        for line in proc.stdout:
            # state machine here to find structs
            m = struct_pattern.match(line)
            if m:
                if m.group('tag'):
                    if (name is not None
                            and decl is not None
                            and size is not None):
                        file = decls.get(decl, '?')
                        # map to source file
                        file = re.sub('\.o$', '.c', file)
                        if build_dir:
                            file = re.sub(
                                '%s/*' % re.escape(build_dir), '',
                                file)
                        # only include structs declared in header files in the
                        # current directory, ignore internal-only structs (
                        # these are represented in other measurements)
                        if everything or file.endswith('.h'):
                            results.append(StructResult(file, name, size))

                    found = (m.group('tag') == 'structure_type')
                    name = None
                    decl = None
                    size = None
                elif found and m.group('name'):
                    name = m.group('name')
                elif found and name and m.group('decl'):
                    decl = int(m.group('decl'))
                elif found and name and m.group('size'):
                    size = int(m.group('size'))
        proc.wait()
        if proc.returncode != 0:
            if not args.get('verbose'):
                for line in proc.stderr:
                    sys.stdout.write(line)
            sys.exit(-1)

    return results


def fold(results, *,
        by=['file', 'struct'],
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
    results = fold(results, by=['file' if by_file else 'struct'])
    if diff_results is not None:
        diff_results = fold(diff_results,
            by=['file' if by_file else 'struct'])

    table = {
        r.file if by_file else r.struct: r
        for r in results}
    diff_table = {
        r.file if by_file else r.struct: r
        for r in diff_results or []}

    # sort, note that python's sort is stable
    names = list(table.keys() | diff_table.keys())
    names.sort()
    if diff_results is not None:
        names.sort(key=lambda n: -IntField.ratio(
            table[n].struct_size if n in table else None,
            diff_table[n].struct_size if n in diff_table else None))
    if size_sort:
        names.sort(key=lambda n: (table[n].struct_size,) if n in table else (),
            reverse=True)
    elif reverse_size_sort:
        names.sort(key=lambda n: (table[n].struct_size,) if n in table else (),
            reverse=False)

    # print header
    if not summary:
        title = '%s%s' % (
            'file' if by_file else 'struct',
            ' (%d added, %d removed)' % (
                sum(1 for n in table if n not in diff_table),
                sum(1 for n in diff_table if n not in table))
                if diff_results is not None and not percent else '')
        name_width = max(it.chain([23, len(title)], (len(n) for n in names)))
    else:
        title = ''
        name_width = 23
    name_width = 4*((name_width+1+4-1)//4)-1

    print('%-*s ' % (name_width, title), end='')
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
                    r.struct_size if r else None,
                    diff_r.struct_size if diff_r else None)
                if not ratio and not all_:
                    continue

            print('%-*s ' % (name_width, name), end='')
            if diff_results is None:
                print(' %s' % (
                    r.struct_size.table()
                        if r else IntField.none))
            elif percent:
                print(' %s%s' % (
                    r.struct_size.diff_table()
                        if r else IntField.diff_none,
                    ' (%s)' % (
                        '+∞%' if ratio == +m.inf
                        else '-∞%' if ratio == -m.inf
                        else '%+.1f%%' % (100*ratio))))
            else:
                print(' %s %s %s%s' % (
                    diff_r.struct_size.diff_table()
                        if diff_r else IntField.diff_none,
                    r.struct_size.diff_table()
                        if r else IntField.diff_none,
                    IntField.diff_diff(
                        r.struct_size if r else None,
                        diff_r.struct_size if diff_r else None)
                        if r or diff_r else IntField.diff_none,
                    ' (%s)' % (
                        '+∞%' if ratio == +m.inf
                        else '-∞%' if ratio == -m.inf
                        else '%+.1f%%' % (100*ratio))
                        if ratio else ''))

    # print total
    total = fold(results, by=[])
    r = total[0] if total else None
    if diff_results is not None:
        diff_total = fold(diff_results, by=[])
        diff_r = diff_total[0] if diff_total else None
        ratio = IntField.ratio(
            r.struct_size if r else None,
            diff_r.struct_size if diff_r else None)

    print('%-*s ' % (name_width, 'TOTAL'), end='')
    if diff_results is None:
        print(' %s' % (
            r.struct_size.table()
                if r else IntField.none))
    elif percent:
        print(' %s%s' % (
            r.struct_size.diff_table()
                if r else IntField.diff_none,
            ' (%s)' % (
                '+∞%' if ratio == +m.inf
                else '-∞%' if ratio == -m.inf
                else '%+.1f%%' % (100*ratio))))
    else:
        print(' %s %s %s%s' % (
            diff_r.struct_size.diff_table()
                if diff_r else IntField.diff_none,
            r.struct_size.diff_table()
                if r else IntField.diff_none,
            IntField.diff_diff(
                r.struct_size if r else None,
                diff_r.struct_size if diff_r else None)
                if r or diff_r else IntField.diff_none,
            ' (%s)' % (
                '+∞%' if ratio == +m.inf
                else '-∞%' if ratio == -m.inf
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
            reader = csv.DictReader(f, restval='')
            for r in reader:
                try:
                    results.append(StructResult(**{
                        k: v for k, v in r.items()
                        if k in StructResult._fields}))
                except TypeError:
                    pass

    # fold to remove duplicates
    results = fold(results)

    # sort because why not
    results.sort()

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, StructResult._fields)
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
                        diff_results.append(StructResult(**{
                            k: v for k, v in r.items()
                            if k in StructResult._fields}))
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
        description="Find struct sizes.")
    parser.add_argument(
        'obj_paths',
        nargs='*',
        default=OBJ_PATHS,
        help="Description of where to find *.o files. May be a directory "
            "or a list of paths. Defaults to %r." % OBJ_PATHS)
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
        '--objdump-tool',
        type=lambda x: x.split(),
        default=OBJDUMP_TOOL,
        help="Path to the objdump tool to use. Defaults to %r." % OBJDUMP_TOOL)
    parser.add_argument(
        '--build-dir',
        help="Specify the relative build directory. Used to map object files "
            "to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
