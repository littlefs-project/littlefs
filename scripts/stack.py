#!/usr/bin/env python3
#
# Script to find stack usage at the function level. Will detect recursion and
# report as infinite stack usage.
#
# Example:
# ./scripts/stack.py lfs.ci lfs_util.ci -Slimit
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

import collections as co
import csv
import itertools as it
import math as mt
import os
import re


# integer fields
class RInt(co.namedtuple('RInt', 'x')):
    __slots__ = ()
    def __new__(cls, x=0):
        if isinstance(x, RInt):
            return x
        if isinstance(x, str):
            try:
                x = int(x, 0)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', x):
                    x = mt.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', x):
                    x = -mt.inf
                else:
                    raise
        assert isinstance(x, int) or mt.isinf(x), x
        return super().__new__(cls, x)

    def __str__(self):
        if self.x == mt.inf:
            return '∞'
        elif self.x == -mt.inf:
            return '-∞'
        else:
            return str(self.x)

    def __int__(self):
        assert not mt.isinf(self.x)
        return self.x

    def __float__(self):
        return float(self.x)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    def diff(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        diff = new - old
        if diff == +mt.inf:
            return '%7s' % '+∞'
        elif diff == -mt.inf:
            return '%7s' % '-∞'
        else:
            return '%+7d' % diff

    def ratio(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        if mt.isinf(new) and mt.isinf(old):
            return 0.0
        elif mt.isinf(new):
            return +mt.inf
        elif mt.isinf(old):
            return -mt.inf
        elif not old and not new:
            return 0.0
        elif not old:
            return +mt.inf
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
        'file', 'function', 'frame', 'limit', 'children'])):
    _by = ['file', 'function']
    _fields = ['frame', 'limit']
    _sort = ['limit', 'frame']
    _types = {'frame': RInt, 'limit': RInt}

    __slots__ = ()
    def __new__(cls, file='', function='',
            frame=0, limit=0,
            children=[]):
        return super().__new__(cls, file, function,
                RInt(frame), RInt(limit),
                children)

    def __add__(self, other):
        return StackResult(self.file, self.function,
                self.frame + other.frame,
                max(self.limit, other.limit),
                self.children + other.children)


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

def collect(ci_paths, *,
        sources=None,
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
    for path in ci_paths:
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
                            print("warning: found non-static stack "
                                    "for %s (%s, %s)" % (
                                        function, type, size))
                        _, _, _, targets = callgraph[info['title']]
                        callgraph[info['title']] = (
                                file, function, int(size), targets)
                elif k == 'edge':
                    info = dict(info)
                    _, _, _, targets = callgraph[info['sourcename']]
                    targets.add(info['targetname'])
                else:
                    continue

    callgraph_ = co.defaultdict(lambda: (None, None, 0, set()))
    for source, (s_file, s_function, frame, targets) in callgraph.items():
        # discard internal functions
        if not everything and s_function.startswith('__'):
            continue
        # ignore filtered sources
        if sources is not None:
            if not any(os.path.abspath(s_file) == os.path.abspath(s)
                    for s in sources):
                continue
        else:
            # default to only cwd
            if not everything and not os.path.commonpath([
                    os.getcwd(),
                    os.path.abspath(s_file)]) == os.getcwd():
                continue

        # smiplify path
        if os.path.commonpath([
                os.getcwd(),
                os.path.abspath(s_file)]) == os.getcwd():
            s_file = os.path.relpath(s_file)
        else:
            s_file = os.path.abspath(s_file)

        callgraph_[source] = (s_file, s_function, frame, targets)
    callgraph = callgraph_

    if not everything:
        callgraph_ = co.defaultdict(lambda: (None, None, 0, set()))
        for source, (s_file, s_function, frame, targets) in callgraph.items():
            # discard filtered sources
            if sources is not None and not any(
                    os.path.abspath(s_file) == os.path.abspath(s)
                    for s in sources):
                continue
            # discard internal functions
            if s_function.startswith('__'):
                continue
            callgraph_[source] = (s_file, s_function, frame, targets)
        callgraph = callgraph_

    # find maximum stack size recursively, this requires also detecting cycles
    # (in case of recursion)
    def find_limit(source, seen=set()):
        if not hasattr(find_limit, 'cache'):
            find_limit.cache = {}
        if source in find_limit.cache:
            return find_limit.cache[source]

        if source not in callgraph:
            return 0
        _, _, frame, targets = callgraph[source]

        limit = 0
        for target in targets:
            # found a cycle?
            if target in seen:
                return mt.inf
            limit_ = find_limit(target, seen | {target})
            limit = max(limit, limit_)

        find_limit.cache[source] = frame + limit
        return frame + limit

    # build results
    results = {}
    for source, (s_file, s_function, frame, _) in callgraph.items():
        limit = find_limit(source)
        results[source] = StackResult(s_file, s_function, frame, limit, [])

    # connect parents to their children, this may create a fully cyclic graph
    # in the case of recursion
    for source, (_, _, _, targets) in callgraph.items():
        results[source].children.extend(
                results[target]
                    for target in targets
                    if target in results)

    return list(results.values())


def fold(Result, results, by=None, defines=[]):
    if by is None:
        by = Result._by

    for k in it.chain(by or [], (k for k, _ in defines)):
        if k not in Result._by and k not in Result._fields:
            print("error: could not find field %r?" % k,
                    file=sys.stderr)
            sys.exit(-1)

    # filter by matching defines
    if defines:
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
        depth=None,
        hot=None,
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

    # reduce children to hot paths?
    if hot:
        def rec_hot(results_, seen=set()):
            if not results_:
                return []

            r = max(results_,
                    key=lambda r: tuple(
                        tuple((getattr(r, k),)
                                    if getattr(r, k, None) is not None
                                    else ()
                                for k in (
                                    [k] if k else [
                                        k for k in Result._sort
                                            if k in fields])
                                if k in fields)
                            for k in it.chain(hot, [None])))

            # found a cycle?
            if tuple(getattr(r, k) for k in Result._by) in seen:
                return []

            return [r._replace(children=[])] + rec_hot(
                    r.children,
                    seen | {tuple(getattr(r, k) for k in Result._by)})

        results = [r._replace(children=rec_hot(r.children)) for r in results]

    # organize by name
    table = {
            ','.join(str(getattr(r, k) or '') for k in by): r
                for r in results}
    diff_table = {
            ','.join(str(getattr(r, k) or '') for k in by): r
                for r in diff_results or []}
    names = [name
            for name in table.keys() | diff_table.keys()
            if diff_results is None
                or all_
                or any(
                    types[k].ratio(
                        getattr(table.get(name), k, None),
                        getattr(diff_table.get(name), k, None))
                    for k in fields)]

    # sort again, now with diff info, note that python's sort is stable
    names.sort()
    if diff_results is not None:
        names.sort(
                key=lambda n: tuple(
                    types[k].ratio(
                            getattr(table.get(n), k, None),
                            getattr(diff_table.get(n), k, None))
                        for k in fields),
                reverse=True)
    if sort:
        for k, reverse in reversed(sort):
            names.sort(
                    key=lambda n: tuple(
                        (getattr(table[n], k),)
                                if getattr(table.get(n), k, None) is not None
                                else ()
                            for k in (
                                [k] if k else [
                                    k for k in Result._sort
                                        if k in fields])),
                    reverse=reverse ^ (not k or k in Result._fields))


    # build up our lines
    lines = []

    # header
    header = ['%s%s' % (
                ','.join(by),
                ' (%d added, %d removed)' % (
                        sum(1 for n in table if n not in diff_table),
                        sum(1 for n in diff_table if n not in table))
                    if diff_results is not None and not percent else '')
            if not summary else '']
    if diff_results is None:
        for k in fields:
            header.append(k)
    elif percent:
        for k in fields:
            header.append(k)
    else:
        for k in fields:
            header.append('o'+k)
        for k in fields:
            header.append('n'+k)
        for k in fields:
            header.append('d'+k)
    lines.append(header)

    # entry helper
    def table_entry(name, r, diff_r=None):
        entry = [name]
        if diff_results is None:
            for k in fields:
                entry.append(
                        (getattr(r, k).table(),
                                getattr(getattr(r, k), 'notes', lambda: [])())
                            if getattr(r, k, None) is not None
                            else types[k].none)
        elif percent:
            for k in fields:
                entry.append(
                        (getattr(r, k).table()
                                if getattr(r, k, None) is not None
                                else types[k].none,
                            (lambda t: ['+∞%'] if t == +mt.inf
                                    else ['-∞%'] if t == -mt.inf
                                    else ['%+.1f%%' % (100*t)])(
                                types[k].ratio(
                                    getattr(r, k, None),
                                    getattr(diff_r, k, None)))))
        else:
            for k in fields:
                entry.append(getattr(diff_r, k).table()
                        if getattr(diff_r, k, None) is not None
                        else types[k].none)
            for k in fields:
                entry.append(getattr(r, k).table()
                        if getattr(r, k, None) is not None
                        else types[k].none)
            for k in fields:
                entry.append(
                        (types[k].diff(
                                getattr(r, k, None),
                                getattr(diff_r, k, None)),
                            (lambda t: ['+∞%'] if t == +mt.inf
                                    else ['-∞%'] if t == -mt.inf
                                    else ['%+.1f%%' % (100*t)] if t
                                    else [])(
                                types[k].ratio(
                                    getattr(r, k, None),
                                    getattr(diff_r, k, None)))))
        return entry

    # recursive entry helper
    def recurse(results_, depth_, seen=set(),
            prefixes=('', '', '', '')):
        # build the children table at each layer
        results_ = fold(Result, results_, by=by)
        table_ = {
                ','.join(str(getattr(r, k) or '') for k in by): r
                    for r in results_}
        names_ = list(table_.keys())

        # sort the children layer
        names_.sort()
        if sort:
            for k, reverse in reversed(sort):
                names_.sort(
                        key=lambda n: tuple(
                            (getattr(table_[n], k),)
                                    if getattr(table_.get(n), k, None)
                                        is not None
                                    else ()
                                for k in (
                                    [k] if k else [
                                        k for k in Result._sort
                                            if k in fields])),
                        reverse=reverse ^ (not k or k in Result._fields))

        for i, name in enumerate(names_):
            r = table_[name]
            is_last = (i == len(names_)-1)

            line = table_entry(name, r)
            line = [x if isinstance(x, tuple) else (x, []) for x in line]
            # add prefixes
            line[0] = (prefixes[0+is_last] + line[0][0], line[0][1])
            # add cycle detection
            if name in seen:
                line[-1] = (line[-1][0], line[-1][1] + ['cycle detected'])
            lines.append(line)

            # found a cycle?
            if name in seen:
                continue

            # recurse?
            if depth_ > 1:
                recurse(r.children,
                        depth_-1,
                        seen | {name},
                        (prefixes[2+is_last] + "|-> ",
                         prefixes[2+is_last] + "'-> ",
                         prefixes[2+is_last] + "|   ",
                         prefixes[2+is_last] + "    "))

    # entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is None:
                diff_r = None
            else:
                diff_r = diff_table.get(name)
            lines.append(table_entry(name, r, diff_r))

            # recursive entries
            if name in table and depth > 1:
                recurse(table[name].children,
                        depth-1,
                        {name},
                        ("|-> ",
                         "'-> ",
                         "|   ",
                         "    "))

    # total
    r = next(iter(fold(Result, results, by=[])), None)
    if diff_results is None:
        diff_r = None
    else:
        diff_r = next(iter(fold(Result, diff_results, by=[])), None)
    lines.append(table_entry('TOTAL', r, diff_r))

    # homogenize
    lines = [
            [x if isinstance(x, tuple) else (x, []) for x in line]
                for line in lines]

    # find the best widths, note that column 0 contains the names and is
    # handled a bit differently
    widths = co.defaultdict(lambda: 7, {0: 23})
    notes = co.defaultdict(lambda: 0)
    for line in lines:
        for i, x in enumerate(line):
            widths[i] = max(widths[i], ((len(x[0])+1+4-1)//4)*4-1)
            notes[i] = max(notes[i], 1+2*len(x[1])+sum(len(n) for n in x[1]))

    # print our table
    for line in lines:
        print('%-*s  %s' % (
                widths[0], line[0][0],
                ' '.join('%*s%-*s' % (
                        widths[i], x[0],
                        notes[i], ' (%s)' % ', '.join(x[1]) if x[1] else '')
                    for i, x in enumerate(line[1:], 1))))


def main(ci_paths,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        **args):
    # figure out depth
    if args.get('depth') is None:
        args['depth'] = mt.inf if args.get('hot') else 1
    elif args.get('depth') == 0:
        args['depth'] = mt.inf

    # find sizes
    if not args.get('use', None):
        results = collect(ci_paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f, restval='')
            for r in reader:
                # filter by matching defines
                if not all(k in r and r[k] in vs for k, vs in defines):
                    continue

                if not any(k in r and r[k].strip()
                        for k in StackResult._fields):
                    continue
                try:
                    results.append(StackResult(
                            **{k: r[k] for k in StackResult._by
                                if k in r and r[k].strip()},
                            **{k: r[k] for k in StackResult._fields
                                if k in r and r[k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(StackResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(
                    key=lambda r: tuple(
                        (getattr(r, k),) if getattr(r, k) is not None else ()
                            for k in ([k] if k else StackResult._sort)),
                    reverse=reverse ^ (not k or k in StackResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f,
                    (by if by is not None else StackResult._by)
                        + [k for k in (
                            fields if fields is not None
                                else StackResult._fields)])
            writer.writeheader()
            for r in results:
                writer.writerow(
                        {k: getattr(r, k) for k in (
                                by if by is not None else StackResult._by)}
                            | {k: getattr(r, k) for k in (
                                fields if fields is not None
                                    else StackResult._fields)})

    # find previous results?
    if args.get('diff'):
        diff_results = []
        try:
            with openio(args['diff']) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    # filter by matching defines
                    if not all(k in r and r[k] in vs for k, vs in defines):
                        continue

                    if not any(k in r and r[k].strip()
                            for k in StackResult._fields):
                        continue
                    try:
                        diff_results.append(StackResult(
                                **{k: r[k] for k in StackResult._by
                                    if k in r and r[k].strip()},
                                **{k: r[k] for k in StackResult._fields
                                    if k in r and r[k].strip()}))
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
            mt.isinf(float(r.limit)) for r in results):
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Find stack usage at the function level.",
            allow_abbrev=False)
    parser.add_argument(
            'ci_paths',
            nargs='*',
            help="Input *.ci files.")
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
            type=lambda x: (
                lambda k, vs: (
                    k.strip(),
                    {v.strip() for v in vs.split(',')})
                )(*x.split('=', 1)),
            help="Only include results where this field is this value.")
    class AppendSort(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.sort is None:
                namespace.sort = []
            namespace.sort.append((value, True if option == '-S' else False))
    parser.add_argument(
            '-s', '--sort',
            nargs='?',
            action=AppendSort,
            help="Sort by this field.")
    parser.add_argument(
            '-S', '--reverse-sort',
            nargs='?',
            action=AppendSort,
            help="Sort by this field, but backwards.")
    parser.add_argument(
            '-Y', '--summary',
            action='store_true',
            help="Only show the total.")
    parser.add_argument(
            '-F', '--source',
            dest='sources',
            action='append',
            help="Only consider definitions in this file. Defaults to "
                "anything in the current directory.")
    parser.add_argument(
            '--everything',
            action='store_true',
            help="Include builtin and libc specific symbols.")
    parser.add_argument(
            '-z', '--depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of function calls to show. 0 shows all calls unless "
                "we find a cycle. Defaults to 0.")
    parser.add_argument(
            '-t', '--hot',
            nargs='?',
            action='append',
            help="Show only the hot path for each function call.")
    parser.add_argument(
            '-e', '--error-on-recursion',
            action='store_true',
            help="Error if any functions are recursive.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
