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
class Int(co.namedtuple('Int', 'x')):
    __slots__ = ()
    def __new__(cls, x=0):
        if isinstance(x, Int):
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
        return self.__class__(self.x + other.x)

    def __sub__(self, other):
        return self.__class__(self.x - other.x)

    def __mul__(self, other):
        return self.__class__(self.x * other.x)

# size results
class StackResult(co.namedtuple('StackResult', [
        'file', 'function', 'frame', 'limit', 'calls'])):
    _by = ['file', 'function']
    _fields = ['frame', 'limit']
    _types = {'frame': Int, 'limit': Int}

    __slots__ = ()
    def __new__(cls, file='', function='',
            frame=0, limit=0, calls=set()):
        return super().__new__(cls, file, function,
            Int(frame), Int(limit),
            calls)

    def __add__(self, other):
        return StackResult(self.file, self.function,
            self.frame + other.frame,
            max(self.limit, other.limit),
            self.calls | other.calls)


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
                m_ = k_pattern.match(rest)
                if not m_:
                    return (node, rest)
                k, rest = m_.group(1), rest[m_.end(0):]

                rest = rest.lstrip()
                if rest.startswith('{'):
                    v, rest = parse_vcg(rest[1:])
                    assert rest[0] == '}', "unexpected %r" % rest[0:1]
                    rest = rest[1:]
                    node.append((k, v))
                else:
                    m_ = v_pattern.match(rest)
                    assert m_, "unexpected %r" % rest[0:1]
                    v, rest = m_.group(1) or m_.group(2), rest[m_.end(0):]
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
                    m_ = f_pattern.match(info['label'])
                    if m_:
                        function, file, size, type = m_.groups()
                        if (not args.get('quiet')
                                and 'static' not in type
                                and 'bounded' not in type):
                            print("warning: found non-static stack for %s (%s)"
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
                return m.inf
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
    for source, (s_file, s_function, frame, targets) in callgraph.items():
        limit = find_limit(source)
        calls = find_calls(targets)
        results.append(StackResult(s_file, s_function, frame, limit, calls))

    return results


def fold(Result, results, *,
        by=None,
        defines=None,
        **_):
    if by is None:
        by = Result._by

    for k in it.chain(by or [], (k for k, _ in defines or [])):
        if k not in Result._by and k not in Result._fields:
            print("error: could not find field %r?" % k)
            sys.exit(-1)

    # filter by matching defines
    if defines is not None:
        results_ = []
        for r in results:
            if all(getattr(r, k) in vs for k, vs in defines):
                results_.append(r)
        results = results_

    # organize results into conflicts
    folding = co.OrderedDict()
    for r in results:
        name = tuple(getattr(r, k) for k in by)
        if name not in folding:
            folding[name] = []
        folding[name].append(r)

    # merge conflicts
    folded = []
    for name, rs in folding.items():
        folded.append(sum(rs[1:], start=rs[0]))

    return folded

def table(Result, results, diff_results=None, *,
        by=None,
        fields=None,
        sort=None,
        summary=False,
        all=False,
        percent=False,
        tree=False,
        depth=None,
        **_):
    all_, all = all, __builtins__.all

    if by is None:
        by = Result._by
    if fields is None:
        fields = Result._fields
    types = Result._types

    # fold again
    results = fold(Result, results, by=by)
    if diff_results is not None:
        diff_results = fold(Result, diff_results, by=by)

    # organize by name
    table = {
        ','.join(str(getattr(r, k) or '') for k in by): r
        for r in results}
    diff_table = {
        ','.join(str(getattr(r, k) or '') for k in by): r
        for r in diff_results or []}
    names = list(table.keys() | diff_table.keys())

    # sort again, now with diff info, note that python's sort is stable
    names.sort()
    if diff_results is not None:
        names.sort(key=lambda n: tuple(
            types[k].ratio(
                getattr(table.get(n), k, None),
                getattr(diff_table.get(n), k, None))
            for k in fields),
            reverse=True)
    if sort:
        for k, reverse in reversed(sort):
            names.sort(key=lambda n: (getattr(table[n], k),)
                if getattr(table.get(n), k, None) is not None else (),
                reverse=reverse ^ (not k or k in Result._fields))


    # build up our lines
    lines = []

    # header
    line = []
    line.append('%s%s' % (
        ','.join(by),
        ' (%d added, %d removed)' % (
            sum(1 for n in table if n not in diff_table),
            sum(1 for n in diff_table if n not in table))
            if diff_results is not None and not percent else '')
        if not summary else '')
    if diff_results is None:
        for k in fields:
            line.append(k)
    elif percent:
        for k in fields:
            line.append(k)
    else:
        for k in fields:
            line.append('o'+k)
        for k in fields:
            line.append('n'+k)
        for k in fields:
            line.append('d'+k)
    line.append('')
    lines.append(line)

    # entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is not None:
                diff_r = diff_table.get(name)
                ratios = [
                    types[k].ratio(
                        getattr(r, k, None),
                        getattr(diff_r, k, None))
                    for k in fields]
                if not any(ratios) and not all_:
                    continue

            line = []
            line.append(name)
            if diff_results is None:
                for k in fields:
                    line.append(getattr(r, k).table()
                        if getattr(r, k, None) is not None
                        else types[k].none)
            elif percent:
                for k in fields:
                    line.append(getattr(r, k).diff_table()
                        if getattr(r, k, None) is not None
                        else types[k].diff_none)
            else:
                for k in fields:
                    line.append(getattr(diff_r, k).diff_table()
                        if getattr(diff_r, k, None) is not None
                        else types[k].diff_none)
                for k in fields:
                    line.append(getattr(r, k).diff_table()
                        if getattr(r, k, None) is not None
                        else types[k].diff_none)
                for k in fields:
                    line.append(types[k].diff_diff(
                            getattr(r, k, None),
                            getattr(diff_r, k, None)))
            if diff_results is None:
                line.append('')
            elif percent:
                line.append(' (%s)' % ', '.join(
                    '+∞%' if t == +m.inf
                    else '-∞%' if t == -m.inf
                    else '%+.1f%%' % (100*t)
                    for t in ratios))
            else:
                line.append(' (%s)' % ', '.join(
                        '+∞%' if t == +m.inf
                        else '-∞%' if t == -m.inf
                        else '%+.1f%%' % (100*t)
                        for t in ratios
                        if t)
                    if any(ratios) else '')
            lines.append(line)

    # total
    r = next(iter(fold(Result, results, by=[])), None)
    if diff_results is not None:
        diff_r = next(iter(fold(Result, diff_results, by=[])), None)
        ratios = [
            types[k].ratio(
                getattr(r, k, None),
                getattr(diff_r, k, None))
            for k in fields]

    line = []
    line.append('TOTAL')
    if diff_results is None:
        for k in fields:
            line.append(getattr(r, k).table()
                if getattr(r, k, None) is not None
                else types[k].none)
    elif percent:
        for k in fields:
            line.append(getattr(r, k).diff_table()
                if getattr(r, k, None) is not None
                else types[k].diff_none)
    else:
        for k in fields:
            line.append(getattr(diff_r, k).diff_table()
                if getattr(diff_r, k, None) is not None
                else types[k].diff_none)
        for k in fields:
            line.append(getattr(r, k).diff_table()
                if getattr(r, k, None) is not None
                else types[k].diff_none)
        for k in fields:
            line.append(types[k].diff_diff(
                    getattr(r, k, None),
                    getattr(diff_r, k, None)))
    if diff_results is None:
        line.append('')
    elif percent:
        line.append(' (%s)' % ', '.join(
            '+∞%' if t == +m.inf
            else '-∞%' if t == -m.inf
            else '%+.1f%%' % (100*t)
            for t in ratios))
    else:
        line.append(' (%s)' % ', '.join(
                '+∞%' if t == +m.inf
                else '-∞%' if t == -m.inf
                else '%+.1f%%' % (100*t)
                for t in ratios
                if t)
            if any(ratios) else '')
    lines.append(line)

    # find the best widths, note that column 0 contains the names and column -1
    # the ratios, so those are handled a bit differently
    widths = [
        ((max(it.chain([w], (len(l[i]) for l in lines)))+1+4-1)//4)*4-1
        for w, i in zip(
            it.chain([23], it.repeat(7)),
            range(len(lines[0])-1))]

    # adjust the name width based on the expected call depth, though
    # note this doesn't really work with unbounded recursion
    if not summary:
        # it doesn't really make sense to not have a depth with tree,
        # so assume depth=inf if tree by default
        if depth is None:
            depth = m.inf if tree else 0
        elif depth == 0:
            depth = m.inf

        if not m.isinf(depth):
            widths[0] += 4*depth

    # print our table with optional call info
    #
    # note we try to adjust our name width based on expected call depth, but
    # this doesn't work if there's unbounded recursion
    if not tree:
        print('%-*s  %s%s' % (
            widths[0], lines[0][0],
            ' '.join('%*s' % (w, x)
                for w, x, in zip(widths[1:], lines[0][1:-1])),
            lines[0][-1]))

    # print the tree recursively
    if not summary:
        line_table = {n: l for n, l in zip(names, lines[1:-1])}

        def recurse(names_, depth_, prefixes=('', '', '', '')):
            for i, name in enumerate(names_):
                if name not in line_table:
                    continue
                line = line_table[name]
                is_last = (i == len(names_)-1)

                print('%s%-*s ' % (
                    prefixes[0+is_last],
                    widths[0] - (
                        len(prefixes[0+is_last])
                        if not m.isinf(depth) else 0),
                    line[0]),
                    end='')
                if not tree:
                    print(' %s%s' % (
                        ' '.join('%*s' % (w, x)
                            for w, x, in zip(widths[1:], line[1:-1])),
                        line[-1]),
                        end='')
                print() 

                # recurse?
                if name in table and depth_ > 0:
                    calls = {
                        ','.join(str(getattr(Result(*c), k) or '') for k in by)
                        for c in table[name].calls}

                    recurse(
                        # note we're maintaining sort order
                        [n for n in names if n in calls],
                        depth_-1,
                        (prefixes[2+is_last] + "|-> ",
                         prefixes[2+is_last] + "'-> ",
                         prefixes[2+is_last] + "|   ",
                         prefixes[2+is_last] + "    "))
        recurse(names, depth)

    if not tree:
        print('%-*s  %s%s' % (
            widths[0], lines[-1][0],
            ' '.join('%*s' % (w, x)
                for w, x, in zip(widths[1:], lines[-1][1:-1])),
            lines[-1][-1]))


def main(ci_paths,
        by=None,
        fields=None,
        defines=None,
        sort=None,
        **args):
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
            print("error: no .ci files found in %r?" % ci_paths)
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f, restval='')
            for r in reader:
                try:
                    results.append(StackResult(
                        **{k: r[k] for k in StackResult._by
                            if k in r and r[k].strip()},
                        **{k: r['stack_'+k] for k in StackResult._fields
                            if 'stack_'+k in r and r['stack_'+k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(StackResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(key=lambda r: (getattr(r, k),)
                if getattr(r, k) is not None else (),
                reverse=reverse ^ (not k or k in StackResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, StackResult._by
                + ['stack_'+k for k in StackResult._fields])
            writer.writeheader()
            for r in results:
                writer.writerow(
                    {k: getattr(r, k) for k in StackResult._by}
                    | {'stack_'+k: getattr(r, k) for k in StackResult._fields})

    # find previous results?
    if args.get('diff'):
        diff_results = []
        try:
            with openio(args['diff']) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    try:
                        diff_results.append(StackResult(
                            **{k: r[k] for k in StackResult._by
                                if k in r and r[k].strip()},
                            **{k: r['stack_'+k] for k in StackResult._fields
                                if 'stack_'+k in r and r['stack_'+k].strip()}))
                    except TypeError:
                        raise
        except FileNotFoundError:
            pass

        # fold
        diff_results = fold(StackResult, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        table(StackResult, results,
            diff_results if args.get('diff') else None,
            by=by if by is not None else ['function'],
            fields=fields,
            sort=sort,
            **args)

    # error on recursion
    if args.get('error_on_recursion') and any(
            m.isinf(float(r.limit)) for r in results):
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
        '-b', '--by',
        action='append',
        choices=StackResult._by,
        help="Group by this field.")
    parser.add_argument(
        '-f', '--field',
        dest='fields',
        action='append',
        choices=StackResult._fields,
        help="Show this field.")
    parser.add_argument(
        '-D', '--define',
        dest='defines',
        action='append',
        type=lambda x: (lambda k,v: (k, set(v.split(','))))(*x.split('=', 1)),
        help="Only include results where this field is this value.")
    class AppendSort(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.sort is None:
                namespace.sort = []
            namespace.sort.append((value, True if option == '-S' else False))
    parser.add_argument(
        '-s', '--sort',
        action=AppendSort,
        help="Sort by this fields.")
    parser.add_argument(
        '-S', '--reverse-sort',
        action=AppendSort,
        help="Sort by this fields, but backwards.")
    parser.add_argument(
        '-Y', '--summary',
        action='store_true',
        help="Only show the total.")
    parser.add_argument(
        '-A', '--everything',
        action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument(
        '--tree',
        action='store_true',
        help="Only show the function call tree.")
    parser.add_argument(
        '-L', '--depth',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Depth of function calls to show. 0 show all calls but may not "
            "terminate!")
    parser.add_argument(
        '-e', '--error-on-recursion',
        action='store_true',
        help="Error if any functions are recursive.")
    parser.add_argument(
        '--build-dir',
        help="Specify the relative build directory. Used to map object files "
            "to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
