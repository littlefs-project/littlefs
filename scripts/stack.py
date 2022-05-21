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

def openio(path, mode='r'):
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

class StackResult(co.namedtuple('StackResult', 'stack_frame,stack_limit')):
    __slots__ = ()
    def __new__(cls, stack_frame=0, stack_limit=0):
        return super().__new__(cls,
            int(stack_frame),
            float(stack_limit))

    def __add__(self, other):
        return self.__class__(
            self.stack_frame + other.stack_frame,
            max(self.stack_limit, other.stack_limit))

    def __sub__(self, other):
        return StackDiff(other, self)

    def __rsub__(self, other):
        return self.__class__.__sub__(other, self)

    def key(self, **args):
        if args.get('limit_sort'):
            return -self.stack_limit
        elif args.get('reverse_limit_sort'):
            return +self.stack_limit
        elif args.get('frame_sort'):
            return -self.stack_frame
        elif args.get('reverse_frame_sort'):
            return +self.stack_frame
        else:
            return None

    _header = '%7s %7s' % ('frame', 'limit')
    def __str__(self):
        return '%7d %7s' % (
            self.stack_frame,
            '∞' if m.isinf(self.stack_limit) else int(self.stack_limit))

class StackDiff(co.namedtuple('StackDiff',  'old,new')):
    __slots__ = ()

    def ratio(self):
        old_limit = self.old.stack_limit if self.old is not None else 0
        new_limit = self.new.stack_limit if self.new is not None else 0
        return (0.0 if m.isinf(new_limit) and m.isinf(old_limit)
            else +float('inf') if m.isinf(new_limit)
            else -float('inf') if m.isinf(old_limit)
            else 0.0 if not old_limit and not new_limit
            else 1.0 if not old_limit
            else (new_limit-old_limit) / old_limit)

    def key(self, **args):
        return (
            self.new.key(**args) if self.new is not None else 0,
            -self.ratio())

    def __bool__(self):
        return bool(self.ratio())

    _header = '%15s %15s %15s' % ('old', 'new', 'diff')
    def __str__(self):
        old_frame = self.old.stack_frame if self.old is not None else 0
        old_limit = self.old.stack_limit if self.old is not None else 0
        new_frame = self.new.stack_frame if self.new is not None else 0
        new_limit = self.new.stack_limit if self.new is not None else 0
        diff_frame = new_frame - old_frame
        diff_limit = (0 if m.isinf(new_limit) and m.isinf(old_limit)
            else new_limit - old_limit)
        ratio = self.ratio()
        return '%7s %7s %7s %7s %+7d %7s%s' % (
            old_frame if self.old is not None else '-',
            ('∞' if m.isinf(old_limit) else int(old_limit))
                if self.old is not None else '-',
            new_frame if self.new is not None else '-',
            ('∞' if m.isinf(new_limit) else int(new_limit))
                if self.new is not None else '-',
            diff_frame,
            '+∞' if diff_limit > 0 and m.isinf(diff_limit)
                else '-∞' if diff_limit < 0 and m.isinf(diff_limit)
                else '%+d' % diff_limit,
            '' if not ratio
                else ' (+∞%)' if ratio > 0 and m.isinf(ratio)
                else ' (-∞%)' if ratio < 0 and m.isinf(ratio)
                else ' (%+.1f%%)' % (100*ratio))


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
                        if not args.get('quiet') and type != 'static':
                            print('warning: found non-static stack for %s (%s)'
                                % (function, type))
                        _, _, _, targets = callgraph[info['title']]
                        callgraph[info['title']] = (
                            file, function, int(size), targets)
                elif k == 'edge':
                    info = dict(info)
                    _, _, _, targets = callgraph[info['sourcename']]
                    targets.add(info['targetname'])
                else:
                    continue

    if not args.get('everything'):
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
    results = {}
    result_calls = {}
    for source, (s_file, s_function, frame, targets) in callgraph.items():
        limit = find_limit(source)
        calls = find_calls(targets)
        results[(s_file, s_function)] = StackResult(frame, limit)
        result_calls[(s_file, s_function)] = calls

    return results, result_calls

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

        results, result_calls = collect(paths, **args)
    else:
        with openio(args['use']) as f:
            r = csv.DictReader(f)
            results = {
                (result['file'], result['name']): StackResult(
                    *(result[f] for f in StackResult._fields))
                for result in r
                if all(result.get(f) not in {None, ''}
                    for f in StackResult._fields)}

        result_calls = {}

    # find previous results?
    if args.get('diff'):
        try:
            with openio(args['diff']) as f:
                r = csv.DictReader(f)
                prev_results = {
                    (result['file'], result['name']): StackResult(
                        *(result[f] for f in StackResult._fields))
                    for result in r
                    if all(result.get(f) not in {None, ''}
                        for f in StackResult._fields)}
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
                        for f in StackResult._fields:
                            result.pop(f, None)
                        merged_results[(file, func)] = result
                        other_fields = result.keys()
            except FileNotFoundError:
                pass

        for (file, func), result in results.items():
            merged_results[(file, func)] |= result._asdict()

        with openio(args['output'], 'w') as f:
            w = csv.DictWriter(f, ['file', 'name',
                *other_fields, *StackResult._fields])
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
            print('%-36s %s' % (by, StackResult._header))
        else:
            old = {entry(k) for k in results.keys()}
            new = {entry(k) for k in prev_results.keys()}
            print('%-36s %s' % (
                '%s (%d added, %d removed)' % (by,
                        sum(1 for k in new if k not in old),
                        sum(1 for k in old if k not in new))
                    if by else '',
                StackDiff._header))

    def print_entries(by):
        # print optional tree of dependencies
        def print_calls(entries, entry_calls, depth,
                filter=lambda _: True,
                prefixes=('', '', '', '')):
            filtered_entries = {
                name: result for name, result in entries.items()
                if filter(name)}
            for i, (name, result) in enumerate(sorted(filtered_entries.items(),
                    key=lambda p: (p[1].key(**args), p))):
                last = (i == len(filtered_entries)-1)
                print('%-36s %s' % (prefixes[0+last] + name, result))

                if depth > 0 and by != 'total':
                    calls = entry_calls.get(name, set())
                    print_calls(entries, entry_calls, depth-1,
                        lambda name: name in calls,
                        (   prefixes[2+last] + "|-> ",
                            prefixes[2+last] + "'-> ",
                            prefixes[2+last] + "|   ",
                            prefixes[2+last] + "    "))

        if by == 'total':
            entry = lambda k: 'TOTAL'
        elif by == 'file':
            entry = lambda k: k[0]
        else:
            entry = lambda k: k[1]

        entries = co.defaultdict(lambda: StackResult())
        for k, result in results.items():
            entries[entry(k)] += result

        entry_calls = co.defaultdict(lambda: set())
        for k, calls in result_calls.items():
            entry_calls[entry(k)] |= {entry(c) for c in calls}

        if not args.get('diff'):
            print_calls(
                entries,
                entry_calls,
                args.get('depth', 0))
        else:
            prev_entries = co.defaultdict(lambda: StackResult())
            for k, result in prev_results.items():
                prev_entries[entry(k)] += result

            diff_entries = {name: entries.get(name) - prev_entries.get(name)
                for name in (entries.keys() | prev_entries.keys())}

            print_calls(
                {name: diff for name, diff in diff_entries.items()
                    if diff or args.get('all')},
                entry_calls,
                args.get('depth', 0))

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

    # catch recursion
    if args.get('error_on_recursion') and any(
            m.isinf(limit) for _, _, _, limit, _ in results):
        sys.exit(2)


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
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-u', '--use',
        help="Don't parse callgraph files, instead use this CSV file.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff against.")
    parser.add_argument('-m', '--merge',
        help="Merge with an existing CSV file when writing to output.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all functions, not just the ones that changed.")
    parser.add_argument('-A', '--everything', action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument('--frame-sort', action='store_true',
        help="Sort by stack frame size.")
    parser.add_argument('--reverse-frame-sort', action='store_true',
        help="Sort by stack frame size, but backwards.")
    parser.add_argument('-s', '--limit-sort', action='store_true',
        help="Sort by stack limit.")
    parser.add_argument('-S', '--reverse-limit-sort', action='store_true',
        help="Sort by stack limit, but backwards.")
    parser.add_argument('-L', '--depth', default=0, type=lambda x: int(x, 0),
        nargs='?', const=float('inf'),
        help="Depth of dependencies to show.")
    parser.add_argument('-F', '--files', action='store_true',
        help="Show file-level calls.")
    parser.add_argument('-Y', '--summary', action='store_true',
        help="Only show the total stack size.")
    parser.add_argument('-e', '--error-on-recursion', action='store_true',
        help="Error if any functions are recursive.")
    parser.add_argument('--build-dir',
        help="Specify the relative build directory. Used to map object files \
            to the correct source files.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_args()).items()
        if v is not None}))
