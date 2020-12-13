#!/usr/bin/env python3
#
# This script finds the code size at the function level, with/without
# static functions, and has some conveniences for comparing different
# versions. It's basically one big wrapper around nm, and may or may
# not have been written out of jealousy of Linux's Bloat-O-Meter.
#
# Here's a useful bash script to use while developing:
# ./scripts/code_size.py -qo old.csv
# while true ; do ./code_scripts/size.py -d old.csv ; inotifywait -rqe modify * ; done
#
# Or even better, to automatically update results on commit:
# ./scripts/code_size.py -qo commit.csv
# while true ; do ./scripts/code_size.py -d commit.csv -o current.csv ; git diff --exit-code --quiet && cp current.csv commit.csv ; inotifywait -rqe modify * ; done
#
# Or my personal favorite:
# ./scripts/code_size.py -qo master.csv && cp master.csv commit.csv
# while true ; do ( ./scripts/code_size.py -i commit.csv -d master.csv -s ; ./scripts/code_size.py -i current.csv -d master.csv -s ; ./scripts/code_size.py -d master.csv -o current.csv -s ) | awk 'BEGIN {printf "%-16s %7s %7s %7s\n","","old","new","diff"} (NR==2 && $1="commit") || (NR==4 && $1="prev") || (NR==6 && $1="current") {printf "%-16s %7s %7s %7s %s\n",$1,$2,$3,$5,$6}' ; git diff --exit-code --quiet && cp current.csv commit.csv ; inotifywait -rqe modify * ; done
#

import os
import itertools as it
import subprocess as sp
import shlex
import re
import csv
import collections as co

SIZEDIR = 'sizes'
RULES = """
define FLATTEN
%(sizedir)s/%(build)s.$(subst /,.,$(target)): $(target)
    ( echo "#line 1 \\"$$<\\"" ; %(cat)s $$< ) > $$@
%(sizedir)s/%(build)s.$(subst /,.,$(target:.c=.size)): \\
        %(sizedir)s/%(build)s.$(subst /,.,$(target:.c=.o))
    $(NM) --size-sort $$^ | sed 's/^/$(subst /,\\/,$(target:.c=.o)):/' > $$@
endef
$(foreach target,$(SRC),$(eval $(FLATTEN)))

-include %(sizedir)s/*.d
.SECONDARY:

%%.size: $(foreach t,$(subst /,.,$(SRC:.c=.size)),%%.$t)
    cat $^ > $@
"""
CATS = {
    'code': 'cat',
    'code_inlined': 'sed \'s/^static\( inline\)\?//\'',
}

def build(**args):
    # mkdir -p sizedir
    os.makedirs(args['sizedir'], exist_ok=True)

    if args.get('inlined', False):
        builds = ['code', 'code_inlined']
    else:
        builds = ['code']

    # write makefiles for the different types of builds
    makefiles = []
    targets = []
    for build in builds:
        path = args['sizedir'] + '/' + build
        with open(path + '.mk', 'w') as mk:
            mk.write(RULES.replace(4*' ', '\t') % dict(
                sizedir=args['sizedir'],
                build=build,
                cat=CATS[build]))
            mk.write('\n')

            # pass on defines
            for d in args['D']:
                mk.write('%s: override CFLAGS += -D%s\n' % (
                    path+'.size', d))

        makefiles.append(path + '.mk')
        targets.append(path + '.size')

    # build in parallel
    cmd = (['make', '-f', 'Makefile'] +
        list(it.chain.from_iterable(['-f', m] for m in makefiles)) +
        [target for target in targets])
    if args.get('verbose', False):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
        stdout=sp.DEVNULL if not args.get('verbose', False) else None)
    proc.wait()
    if proc.returncode != 0:
        sys.exit(-1)

    # find results
    build_results = co.defaultdict(lambda: 0)
    # notes
    # - filters type
    # - discards internal/debug functions (leading __)
    pattern = re.compile(
        '^(?P<file>[^:]+)' +
        ':(?P<size>[0-9a-fA-F]+)' +
        ' (?P<type>[%s])' % re.escape(args['type']) +
        ' (?!__)(?P<name>.+?)$')
    for build in builds:
        path = args['sizedir'] + '/' + build
        with open(path + '.size') as size:
            for line in size:
                match = pattern.match(line)
                if match:
                    file = match.group('file')
                    # discard .8449 suffixes created by optimizer
                    name = re.sub('\.[0-9]+', '', match.group('name'))
                    size = int(match.group('size'), 16)
                    build_results[(build, file, name)] += size

    results = []
    for (build, file, name), size in build_results.items():
        if build == 'code':
            results.append((file, name, size, False))
        elif (build == 'code_inlined' and
                ('inlined', file, name) not in results):
            results.append((file, name, size, True))

    return results

def main(**args):
    # find results
    if not args.get('input', None):
        results = build(**args)
    else:
        with open(args['input']) as f:
            r = csv.DictReader(f)
            results = [
                (   result['file'],
                    result['name'],
                    int(result['size']),
                    bool(int(result.get('inlined', 0))))
                for result in r
                if (not bool(int(result.get('inlined', 0))) or
                    args.get('inlined', False))]

    total = 0
    for _, _, size, inlined in results:
        if not inlined:
            total += size

    # find previous results?
    if args.get('diff', None):
        with open(args['diff']) as f:
            r = csv.DictReader(f)
            prev_results = [
                (   result['file'],
                    result['name'],
                    int(result['size']),
                    bool(int(result.get('inlined', 0))))
                for result in r
                if (not bool(int(result.get('inlined', 0))) or
                    args.get('inlined', False))]

        prev_total = 0
        for _, _, size, inlined in prev_results:
            if not inlined:
                prev_total += size

    # write results to CSV
    if args.get('output', None):
        results.sort(key=lambda x: (-x[2], x))
        with open(args['output'], 'w') as f:
            w = csv.writer(f)
            if args.get('inlined', False):
                w.writerow(['file', 'name', 'size', 'inlined'])
                for file, name, size, inlined in results:
                    w.writerow((file, name, size, int(inlined)))
            else:
                w.writerow(['file', 'name', 'size'])
                for file, name, size, inlined in results:
                    w.writerow((file, name, size))

    # print results
    def dedup_functions(results):
        functions = co.defaultdict(lambda: (0, True))
        for _, name, size, inlined in results:
            if not inlined:
                functions[name] = (functions[name][0] + size, False)
        for _, name, size, inlined in results:
            if inlined and functions[name][1]:
                functions[name] = (functions[name][0] + size, True)
        return functions

    def dedup_files(results):
        files = co.defaultdict(lambda: 0)
        for file, _, size, inlined in results:
            if not inlined:
                files[file] += size
        return files

    def diff_sizes(olds, news):
        diff = co.defaultdict(lambda: (None, None, None))
        for name, new in news.items():
            diff[name] = (None, new, new)
        for name, old in olds.items():
            new = diff[name][1] or 0
            diff[name] = (old, new, new-old)
        return diff

    def print_header(name=''):
        if not args.get('diff', False):
            print('%-40s %7s' % (name, 'size'))
        else:
            print('%-40s %7s %7s %7s' % (name, 'old', 'new', 'diff'))

    def print_functions():
        functions = dedup_functions(results)
        functions = {
            name+' (inlined)' if inlined else name: size
            for name, (size, inlined) in functions.items()}

        if not args.get('diff', None):
            print_header('function')
            for name, size in sorted(functions.items(),
                    key=lambda x: (-x[1], x)):
                print("%-40s %7d" % (name, size))
        else:
            prev_functions = dedup_functions(prev_results)
            prev_functions = {
                name+' (inlined)' if inlined else name: size
                for name, (size, inlined) in prev_functions.items()}
            diff = diff_sizes(functions, prev_functions)
            print_header('function (%d added, %d removed)' % (
                sum(1 for old, _, _ in diff.values() if not old),
                sum(1 for _, new, _ in diff.values() if not new)))
            for name, (old, new, diff) in sorted(diff.items(),
                    key=lambda x: (-(x[1][2] or 0), x)):
                if diff or args.get('all', False):
                    print("%-40s %7s %7s %+7d%s" % (
                        name, old or "-", new or "-", diff,
                        ' (%+.2f%%)' % (100*((new-old)/old))
                        if old and new else
                        ''))

    def print_files():
        files = dedup_files(results)

        if not args.get('diff', None):
            print_header('file')
            for file, size in sorted(files.items(),
                    key=lambda x: (-x[1], x)):
                print("%-40s %7d" % (file, size))
        else:
            prev_files = dedup_files(prev_results)
            diff = diff_sizes(files, prev_files)
            print_header('file (%d added, %d removed)' % (
                sum(1 for old, _, _ in diff.values() if not old),
                sum(1 for _, new, _ in diff.values() if not new)))
            for name, (old, new, diff) in sorted(diff.items(),
                    key=lambda x: (-(x[1][2] or 0), x)):
                if diff or args.get('all', False):
                    print("%-40s %7s %7s %+7d%s" % (
                        name, old or "-", new or "-", diff,
                        ' (%+.2f%%)' % (100*((new-old)/old))
                        if old and new else
                        ''))

    def print_totals():
        if not args.get('diff', None):
            print("%-40s %7d" % ('TOTALS', total))
        else:
            print("%-40s %7s %7s %+7d%s" % (
                'TOTALS', prev_total, total, total-prev_total,
                ' (%+.2f%%)' % (100*((total-prev_total)/total))
                if prev_total and total else
                ''))

    def print_status():
        if not args.get('diff', None):
            print(total)
        else:
            print("%d (%+.2f%%)" % (total, 100*((total-prev_total)/total)))

    if args.get('quiet', False):
        pass
    elif args.get('status', False):
        print_status()
    elif args.get('summary', False):
        print_header()
        print_totals()
    elif args.get('files', False):
        print_files()
        print_totals()
    else:
        print_functions()
        print_totals()

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Find code size at the function level.")
    parser.add_argument('sizedir', nargs='?', default=SIZEDIR,
        help="Directory to store intermediary results. Defaults "
            "to \"%s\"." % SIZEDIR)
    parser.add_argument('-D', action='append', default=[],
        help="Specify compile-time define.")
    parser.add_argument('-v', '--verbose', action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument('-i', '--input',
        help="Don't compile and find code sizes, instead use this CSV file.")
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff code size against.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all functions, not just the ones that changed.")
    parser.add_argument('--inlined', action='store_true',
        help="Run a second compilation to find the sizes of functions normally "
            "removed by optimizations. These will be shown as \"*.inlined\" "
            "functions, and will not be included in the total.")
    parser.add_argument('--files', action='store_true',
        help="Show file-level code sizes. Note this does not include padding! "
            "So sizes may differ from other tools.")
    parser.add_argument('-s', '--summary', action='store_true',
        help="Only show the total code size.")
    parser.add_argument('-S', '--status', action='store_true',
        help="Show minimum info useful for a single-line status.")
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    parser.add_argument('--type', default='tTrRdDbB',
        help="Type of symbols to report, this uses the same single-character "
            "type-names emitted by nm. Defaults to %(default)r.")
    sys.exit(main(**vars(parser.parse_args())))
