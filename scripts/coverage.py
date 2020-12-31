#!/usr/bin/env python3
#

import os
import glob
import csv
import re
import collections as co
import bisect as b

RESULTDIR = 'results'
#RULES = """
#define FLATTEN
#%(sizedir)s/%(build)s.$(subst /,.,$(target)): $(target)
#    ( echo "#line 1 \\"$$<\\"" ; %(cat)s $$< ) > $$@
#%(sizedir)s/%(build)s.$(subst /,.,$(target:.c=.size)): \\
#        %(sizedir)s/%(build)s.$(subst /,.,$(target:.c=.o))
#    $(NM) --size-sort $$^ | sed 's/^/$(subst /,\\/,$(target:.c=.o)):/' > $$@
#endef
#$(foreach target,$(SRC),$(eval $(FLATTEN)))
#
#-include %(sizedir)s/*.d
#.SECONDARY:
#
#%%.size: $(foreach t,$(subst /,.,$(OBJ:.o=.size)),%%.$t)
#    cat $^ > $@
#"""
#CATS = {
#    'code': 'cat',
#    'code_inlined': 'sed \'s/^static\( inline\)\?//\'',
#}
#
#def build(**args):
#    # mkdir -p sizedir
#    os.makedirs(args['sizedir'], exist_ok=True)
#
#    if args.get('inlined', False):
#        builds = ['code', 'code_inlined']
#    else:
#        builds = ['code']
#
#    # write makefiles for the different types of builds
#    makefiles = []
#    targets = []
#    for build in builds:
#        path = args['sizedir'] + '/' + build
#        with open(path + '.mk', 'w') as mk:
#            mk.write(RULES.replace(4*' ', '\t') % dict(
#                sizedir=args['sizedir'],
#                build=build,
#                cat=CATS[build]))
#            mk.write('\n')
#
#            # pass on defines
#            for d in args['D']:
#                mk.write('%s: override CFLAGS += -D%s\n' % (
#                    path+'.size', d))
#
#        makefiles.append(path + '.mk')
#        targets.append(path + '.size')
#
#    # build in parallel
#    cmd = (['make', '-f', 'Makefile'] +
#        list(it.chain.from_iterable(['-f', m] for m in makefiles)) +
#        [target for target in targets])
#    if args.get('verbose', False):
#        print(' '.join(shlex.quote(c) for c in cmd))
#    proc = sp.Popen(cmd,
#        stdout=sp.DEVNULL if not args.get('verbose', False) else None)
#    proc.wait()
#    if proc.returncode != 0:
#        sys.exit(-1)
#
#    # find results
#    build_results = co.defaultdict(lambda: 0)
#    # notes
#    # - filters type
#    # - discards internal/debug functions (leading __)
#    pattern = re.compile(
#        '^(?P<file>[^:]+)' +
#        ':(?P<size>[0-9a-fA-F]+)' +
#        ' (?P<type>[%s])' % re.escape(args['type']) +
#        ' (?!__)(?P<name>.+?)$')
#    for build in builds:
#        path = args['sizedir'] + '/' + build
#        with open(path + '.size') as size:
#            for line in size:
#                match = pattern.match(line)
#                if match:
#                    file = match.group('file')
#                    # discard .8449 suffixes created by optimizer
#                    name = re.sub('\.[0-9]+', '', match.group('name'))
#                    size = int(match.group('size'), 16)
#                    build_results[(build, file, name)] += size
#
#    results = []
#    for (build, file, name), size in build_results.items():
#        if build == 'code':
#            results.append((file, name, size, False))
#        elif (build == 'code_inlined' and
#                ('inlined', file, name) not in results):
#            results.append((file, name, size, True))
#
#    return results

def collect(covfuncs, covlines, path, **args):
    with open(path) as f:
        file = None
        filter = args['filter'].split() if args.get('filter') else None
        pattern = re.compile(
            '^(?P<file>file'
                ':(?P<file_name>.*))' +
            '|(?P<func>function' +
                ':(?P<func_lineno>[0-9]+)' +
                ',(?P<func_hits>[0-9]+)' +
                ',(?P<func_name>.*))' +
            '|(?P<line>lcount' +
                ':(?P<line_lineno>[0-9]+)' +
                ',(?P<line_hits>[0-9]+))$')
        for line in f:
            match = pattern.match(line)
            if match:
                if match.group('file'):
                    file = match.group('file_name')
                    # filter?
                    if filter and file not in filter:
                        file = None
                elif file is not None and match.group('func'):
                    lineno = int(match.group('func_lineno'))
                    name, hits = covfuncs[(file, lineno)]
                    covfuncs[(file, lineno)] = (
                        name or match.group('func_name'),
                        hits + int(match.group('func_hits')))
                elif file is not None and match.group('line'):
                    lineno = int(match.group('line_lineno'))
                    covlines[(file, lineno)] += int(match.group('line_hits'))

def coverage(**args):
    # find *.gcov files
    gcovpaths = []
    for gcovpath in args.get('gcovpaths') or [args['results']]:
        if os.path.isdir(gcovpath):
            gcovpath = gcovpath + '/*.gcov'

        for path in glob.glob(gcovpath):
            gcovpaths.append(path)

    if not gcovpaths:
        print('no gcov files found in %r?'
            % (args.get('gcovpaths') or [args['results']]))
        sys.exit(-1)

    # collect coverage info
    covfuncs = co.defaultdict(lambda: (None, 0))
    covlines = co.defaultdict(lambda: 0)
    for path in gcovpaths:
        collect(covfuncs, covlines, path, **args)

    # merge? go ahead and handle that here, but
    # with a copy so we only report on the current coverage
    if args.get('merge', None):
        if os.path.isfile(args['merge']):
            accfuncs = covfuncs.copy()
            acclines = covlines.copy()
            collect(accfuncs, acclines, args['merge']) # don't filter!
        else:
            accfuncs = covfuncs
            acclines = covlines

        accfiles = sorted({file for file, _ in acclines.keys()})
        accfuncs, i = sorted(accfuncs.items()), 0 
        acclines, j = sorted(acclines.items()), 0
        with open(args['merge'], 'w') as f:
            for file in accfiles:
                f.write('file:%s\n' % file)
                while i < len(accfuncs) and accfuncs[i][0][0] == file:
                    ((_, lineno), (name, hits)) = accfuncs[i]
                    f.write('function:%d,%d,%s\n' % (lineno, hits, name))
                    i += 1
                while j < len(acclines) and acclines[j][0][0] == file:
                    ((_, lineno), hits) = acclines[j]
                    f.write('lcount:%d,%d\n' % (lineno, hits))
                    j += 1

    # annotate?
    if args.get('annotate', False):
        # annotate(covlines, **args)
        pass

    # condense down to file/function results
    funcs = sorted(covfuncs.items())
    func_lines = [(file, lineno) for (file, lineno), _ in funcs]
    func_names = [name for _, (name, _) in funcs]
    def line_func(file, lineno):
        i = b.bisect(func_lines, (file, lineno))
        if i and func_lines[i-1][0] == file:
            return func_names[i-1]
        else:
            return '???'

    func_results = co.defaultdict(lambda: (0, 0))
    for ((file, lineno), hits) in covlines.items():
        func = line_func(file, lineno)
        branch_hits, branches = func_results[(file, func)]
        func_results[(file, func)] = (branch_hits + (hits > 0), branches + 1)

    results = []
    for (file, func), (hits, branches) in func_results.items():
        # discard internal/testing functions (test_* injected with
        # internal testing)
        if func == '???' or func.startswith('__') or func.startswith('test_'):
            continue
        # discard .8449 suffixes created by optimizer
        func = re.sub('\.[0-9]+', '', func)
        results.append((file, func, hits, branches))

    return results


def main(**args):
    # find coverage
    if not args.get('input', None):
        results = coverage(**args)
    else:
        with open(args['input']) as f:
            r = csv.DictReader(f)
            results = [
                (   result['file'],
                    result['function'],
                    int(result['hits']),
                    int(result['branches']))
                for result in r]

    total_hits, total_branches = 0, 0
    for _, _, hits, branches in results:
        total_hits += hits
        total_branches += branches

    # find previous results?
    if args.get('diff', None):
        with open(args['diff']) as f:
            r = csv.DictReader(f)
            prev_results = [
                (   result['file'],
                    result['function'],
                    int(result['hits']),
                    int(result['branches']))
                for result in r]

        prev_total_hits, prev_total_branches = 0, 0
        for _, _, hits, branches in prev_results:
            prev_total_hits += hits
            prev_total_branches += branches

    # write results to CSV
    if args.get('output', None):
        results.sort(key=lambda x: (-(x[2]/x[3]), -x[3], x))
        with open(args['output'], 'w') as f:
            w = csv.writer(f)
            w.writerow(['file', 'function', 'hits', 'branches'])
            for file, func, hits, branches in results:
                w.writerow((file, func, hits, branches))

    # print results
    def dedup_entries(results, by='function'):
        entries = co.defaultdict(lambda: (0, 0))
        for file, func, hits, branches in results:
            entry = (file if by == 'file' else func)
            entry_hits, entry_branches = entries[entry]
            entries[entry] = (entry_hits + hits, entry_branches + branches)
        return entries

    def diff_entries(olds, news):
        diff = co.defaultdict(lambda: (None, None, None, None, None, None))
        for name, (new_hits, new_branches) in news.items():
            diff[name] = (
                0, 0,
                new_hits, new_branches,
                new_hits, new_branches)
        for name, (old_hits, old_branches) in olds.items():
            new_hits = diff[name][2] or 0
            new_branches = diff[name][3] or 0
            diff[name] = (
                old_hits, old_branches,
                new_hits, new_branches,
                new_hits-old_hits, new_branches-old_branches)
        return diff

    def print_header(by=''):
        if not args.get('diff', False):
            print('%-36s %11s' % (by, 'branches'))
        else:
            print('%-36s %11s %11s %11s' % (by, 'old', 'new', 'diff'))

    def print_entries(by='function'):
        entries = dedup_entries(results, by=by)

        if not args.get('diff', None):
            print_header(by=by)
            for name, (hits, branches) in sorted(entries.items(),
                    key=lambda x: (-(x[1][0]-x[1][1]), -x[1][1], x)):
                print("%-36s %11s (%.2f%%)" % (name,
                    '%d/%d' % (hits, branches),
                    100*(hits/branches if branches else 1.0)))
        else:
            prev_entries = dedup_entries(prev_results, by=by)
            diff = diff_entries(prev_entries, entries)
            print_header(by='%s (%d added, %d removed)' % (by,
                sum(1 for _, old, _, _, _, _ in diff.values() if not old),
                sum(1 for _, _, _, new, _, _ in diff.values() if not new)))
            for name, (
                    old_hits, old_branches,
                    new_hits, new_branches,
                    diff_hits, diff_branches) in sorted(diff.items(),
                        key=lambda x: (
                            -(x[1][4]-x[1][5]), -x[1][5], -x[1][3], x)):
                ratio = ((new_hits/new_branches if new_branches else 1.0)
                    - (old_hits/old_branches if old_branches else 1.0))
                if diff_hits or diff_branches or args.get('all', False):
                    print("%-36s %11s %11s %11s%s" % (name,
                        '%d/%d' % (old_hits, old_branches)
                            if old_branches else '-',
                        '%d/%d' % (new_hits, new_branches)
                            if new_branches else '-',
                        '%+d/%+d' % (diff_hits, diff_branches),
                        ' (%+.2f%%)' % (100*ratio) if ratio else ''))

    def print_totals():
        if not args.get('diff', None):
            print("%-36s %11s (%.2f%%)" % ('TOTALS',
                '%d/%d' % (total_hits, total_branches),
                100*(total_hits/total_branches if total_branches else 1.0)))
        else:
            ratio = ((total_hits/total_branches
                    if total_branches else 1.0)
                - (prev_total_hits/prev_total_branches
                    if prev_total_branches else 1.0))
            print("%-36s %11s %11s %11s%s" % ('TOTALS',
                '%d/%d' % (prev_total_hits, prev_total_branches),
                '%d/%d' % (total_hits, total_branches),
                '%+d/%+d' % (total_hits-prev_total_hits,
                    total_branches-prev_total_branches),
                ' (%+.2f%%)' % (100*ratio) if ratio else ''))

    def print_status():
        if not args.get('diff', None):
            print("%d/%d (%.2f%%)" % (total_hits, total_branches,
                100*(total_hits/total_branches if total_branches else 1.0)))
        else:
            ratio = ((total_hits/total_branches
                    if total_branches else 1.0)
                - (prev_total_hits/prev_total_branches
                    if prev_total_branches else 1.0))
            print("%d/%d (%+.2f%%)" % (total_hits, total_branches,
                (100*ratio) if ratio else ''))

    if args.get('quiet', False):
        pass
    elif args.get('status', False):
        print_status()
    elif args.get('summary', False):
        print_header()
        print_totals()
    elif args.get('files', False):
        print_entries(by='file')
        print_totals()
    else:
        print_entries(by='function')
        print_totals()

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Show/manipulate coverage info")
    parser.add_argument('gcovpaths', nargs='*',
        help="Description of *.gcov files to use for coverage info. May be \
            a directory or list of files. Coverage files will be merged to \
            show the total coverage. Defaults to \"%s\"." % RESULTDIR)
    parser.add_argument('--results', default=RESULTDIR,
        help="Directory to store results. Created implicitly. Used if \
            annotated files are requested. Defaults to \"%s\"." % RESULTDIR)
    parser.add_argument('--merge',
        help="Merge coverage info into the specified file, writing the \
            cumulative coverage info to the file. The output from this script \
            does not include the coverage from the merge file.")
    parser.add_argument('--filter',
        help="Specify files with care about, all other coverage info (system \
            headers, test framework, etc) will be discarded.")
    parser.add_argument('--annotate', action='store_true',
        help="Output annotated source files into the result directory. Each \
            line will be annotated with the number of hits during testing. \
            This is useful for finding out which lines do not have test \
            coverage.")
    parser.add_argument('-v', '--verbose', action='store_true',
        help="Output commands that run behind the scenes.")
    parser.add_argument('-i', '--input',
        help="Don't do any work, instead use this CSV file.")
    parser.add_argument('-o', '--output',
        help="Specify CSV file to store results.")
    parser.add_argument('-d', '--diff',
        help="Specify CSV file to diff code size against.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all functions, not just the ones that changed.")
    parser.add_argument('--files', action='store_true',
        help="Show file-level coverage.")
    parser.add_argument('-s', '--summary', action='store_true',
        help="Only show the total coverage.")
    parser.add_argument('-S', '--status', action='store_true',
        help="Show minimum info useful for a single-line status.")
    parser.add_argument('-q', '--quiet', action='store_true',
        help="Don't show anything, useful with -o.")
    sys.exit(main(**vars(parser.parse_args())))
