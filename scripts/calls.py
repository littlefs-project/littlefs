#!/usr/bin/env python3
#
# Script to show the callgraph in a human readable manner. Basically just a
# wrapper aroung GCC's -fcallgraph-info flag.
#

import os
import glob
import itertools as it
import re
import csv
import collections as co


CI_PATHS = ['*.ci']

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
    results = co.defaultdict(lambda: (None, None, set()))
    f_pattern = re.compile(r'([^\\]*)\\n([^:]*)')
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
                        function, file = m.groups()
                        _, _, targets = results[info['title']]
                        results[info['title']] = (file, function, targets)
                elif k == 'edge':
                    info = dict(info)
                    _, _, targets = results[info['sourcename']]
                    targets.add(info['targetname'])
                else:
                    continue

    if not args.get('everything'):
        for source, (s_file, s_function, _) in list(results.items()):
            # discard internal functions
            if s_file.startswith('<') or s_file.startswith('/usr/include'):
                del results[source]

    # flatten into a list
    flat_results = []
    for _, (s_file, s_function, targets) in results.items():
        for target in targets:
            if target not in results:
                continue

            t_file, t_function, _ = results[target]
            flat_results.append((s_file, s_function, t_file, t_function))

    return flat_results

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

        results = collect(paths, **args)
    else:
        with open(args['use']) as f:
            r = csv.DictReader(f)
            results = [
                (   result['file'],
                    result['function'],
                    result['callee_file'],
                    result['callee_function'])
                for result in r]

    # write results to CSV
    if args.get('output'):
        with open(args['output'], 'w') as f:
            w = csv.writer(f)
            w.writerow(['file', 'function', 'callee_file', 'callee_function'])
            for file, func, c_file, c_func in sorted(results):
                w.writerow((file, func, c_file, c_func))

    # print results
    def dedup_entries(results, by='function'):
        entries = co.defaultdict(lambda: set())
        for file, func, c_file, c_func in results:
            entry = (file if by == 'file' else func)
            entries[entry].add(c_file if by == 'file' else c_func)
        return entries

    def print_entries(by='function'):
        entries = dedup_entries(results, by=by)

        for name, callees in sorted(entries.items()):
            print(name)
            for i, c_name in enumerate(sorted(callees)):
                print(" -> %s" % c_name)

    if args.get('quiet'):
        pass
    elif args.get('files'):
        print_entries(by='file')
    else:
        print_entries(by='function')

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find and show callgraph.")
    parser.add_argument('ci_paths', nargs='*', default=CI_PATHS,
        help="Description of where to find *.ci files. May be a directory \
            or a list of paths. Defaults to %r." % CI_PATHS)
    parser.add_argument('-v', '--verbose', action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-u', '--use',
        help="Don't parse callgraph files, instead use this CSV file.")
    parser.add_argument('-A', '--everything', action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument('--files', action='store_true',
        help="Show file-level calls.")
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument('--build-dir',
        help="Specify the relative build directory. Used to map object files \
            to the correct source files.")
    sys.exit(main(**vars(parser.parse_args())))
