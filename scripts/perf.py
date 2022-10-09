#!/usr/bin/env python3
#
# Script to aggregate and report Linux perf results.
#
# Example:
# ./scripts/perf.py -R -obench.perf ./runners/bench_runner
# ./scripts/perf.py bench.perf -Flfs.c -Flfs_util.c -Scycles
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

import bisect
import collections as co
import csv
import errno
import fcntl
import functools as ft
import glob
import itertools as it
import math as m
import multiprocessing as mp
import os
import re
import shlex
import shutil
import subprocess as sp
import tempfile
import zipfile

# TODO support non-zip perf results?


PERF_PATHS = ['*.perf']
PERF_TOOL = ['perf']
PERF_EVENTS = 'cycles,branch-misses,branches,cache-misses,cache-references'
PERF_FREQ = 100
OBJDUMP_TOOL = ['objdump']
THRESHOLD = (0.5, 0.85)


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

# perf results
class PerfResult(co.namedtuple('PerfResult', [
        'file', 'function', 'line',
        'cycles', 'bmisses', 'branches', 'cmisses', 'caches',
        'children'])):
    _by = ['file', 'function', 'line']
    _fields = ['cycles', 'bmisses', 'branches', 'cmisses', 'caches']
    _types = {
        'cycles': Int,
        'bmisses': Int, 'branches': Int,
        'cmisses': Int, 'caches': Int}

    __slots__ = ()
    def __new__(cls, file='', function='', line=0,
            cycles=0, bmisses=0, branches=0, cmisses=0, caches=0,
            children=[]):
        return super().__new__(cls, file, function, int(Int(line)),
            Int(cycles), Int(bmisses), Int(branches), Int(cmisses), Int(caches),
            children)

    def __add__(self, other):
        return PerfResult(self.file, self.function, self.line,
            self.cycles + other.cycles,
            self.bmisses + other.bmisses,
            self.branches + other.branches,
            self.cmisses + other.cmisses,
            self.caches + other.caches,
            self.children + other.children)


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

# run perf as a subprocess, storing measurements into a zip file
def record(command, *,
        output=None,
        perf_freq=PERF_FREQ,
        perf_period=None,
        perf_events=PERF_EVENTS,
        perf_tool=PERF_TOOL,
        **args):
    if not command:
        print('error: no command specified?')
        sys.exit(-1)

    if not output:
        print('error: no output file specified?')
        sys.exit(-1)

    # create a temporary file for perf to write to, as far as I can tell
    # this is strictly needed because perf's pipe-mode only works with stdout
    with tempfile.NamedTemporaryFile('rb') as f:
        # figure out our perf invocation
        perf = perf_tool + list(filter(None, [
            'record',
            '-F%s' % perf_freq
                if perf_freq is not None
                and perf_period is None else None,
            '-c%s' % perf_period
                if perf_period is not None else None,
            '-B',
            '-g',
            '--all-user',
            '-e%s' % perf_events,
            '-o%s' % f.name]))

        # run our command
        try:
            if args.get('verbose'):
                print(' '.join(shlex.quote(c) for c in perf + command))
            err = sp.call(perf + command, close_fds=False)

        except KeyboardInterrupt:
            err = errno.EOWNERDEAD

        # synchronize access
        z = os.open(output, os.O_RDWR | os.O_CREAT)
        fcntl.flock(z, fcntl.LOCK_EX)

        # copy measurements into our zip file
        with os.fdopen(z, 'r+b') as z:
            with zipfile.ZipFile(z, 'a',
                    compression=zipfile.ZIP_DEFLATED,
                    compresslevel=1) as z:
                with z.open('perf.%d' % os.getpid(), 'w') as g:
                    shutil.copyfileobj(f, g)

    # forward the return code
    return err


def collect_decompressed(path, *,
        perf_tool=PERF_TOOL,
        everything=False,
        propagate=0,
        depth=1,
        **args):
    sample_pattern = re.compile(
        '(?P<comm>\w+)'
        '\s+(?P<pid>\w+)'
        '\s+(?P<time>[\w.]+):'
        '\s*(?P<period>\w+)'
        '\s+(?P<event>[^:]+):')
    frame_pattern = re.compile(
        '\s+(?P<addr>\w+)'
        '\s+(?P<sym>[^\s]+)'
        '\s+\((?P<dso>[^\)]+)\)')
    events = {
        'cycles':           'cycles',
        'branch-misses':    'bmisses',
        'branches':         'branches',
        'cache-misses':     'cmisses',
        'cache-references': 'caches'}

    # note perf_tool may contain extra args
    cmd = perf_tool + [
        'script',
        '-i%s' % path]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
        stdout=sp.PIPE,
        stderr=sp.PIPE if not args.get('verbose') else None,
        universal_newlines=True,
        errors='replace',
        close_fds=False)

    last_filtered = False
    last_event = ''
    last_period = 0
    last_stack = []
    results = {}

    def commit():
        # tail-recursively propagate measurements
        for i in range(len(last_stack)):
            results_ = results
            for j in reversed(range(i+1)):
                if i+1-j > depth:
                    break

                # propagate
                name = last_stack[j]
                if name not in results_:
                    results_[name] = (co.defaultdict(lambda: 0), {})
                results_[name][0][last_event] += last_period

                # recurse
                results_ = results_[name][1]

    for line in proc.stdout:
        # we need to process a lot of data, so wait to use regex as late
        # as possible
        if not line:
            continue
        if not line.startswith('\t'):
            m = sample_pattern.match(line)
            if m:
                if last_stack:
                    commit()
                last_event = m.group('event')
                last_filtered = last_event in events
                last_period = int(m.group('period'), 0)
                last_stack = []
        elif last_filtered:
            m = frame_pattern.match(line)
            if m:
                # filter out internal/kernel functions
                if not everything and (
                        m.group('sym').startswith('__')
                        or m.group('dso').startswith('/usr/lib')
                        or not m.group('sym')[:1].isalpha()):
                    continue

                last_stack.append((
                    m.group('dso'),
                    m.group('sym'),
                    int(m.group('addr'), 16)))

                # stop propogating?
                if propagate and len(last_stack) >= propagate:
                    last_filtered = False
    if last_stack:
        commit()

    proc.wait()
    if proc.returncode != 0:
        if not args.get('verbose'):
            for line in proc.stderr:
                sys.stdout.write(line)
        sys.exit(-1)

    # rearrange results into result type
    def to_results(results):
        results_ = []
        for name, (r, children) in results.items():
            results_.append(PerfResult(*name,
                **{events[k]: v for k, v in r.items()},
                children=to_results(children)))
        return results_

    return to_results(results)

def collect_job(path, i, **args):
    # decompress into a temporary file, this is to work around
    # some limitations of perf
    with zipfile.ZipFile(path) as z:
        with z.open(i) as f:
            with tempfile.NamedTemporaryFile('wb') as g:
                shutil.copyfileobj(f, g)
                g.flush()

                return collect_decompressed(g.name, **args)

def starapply(args):
    f, args, kwargs = args
    return f(*args, **kwargs)

def collect(paths, *,
        jobs=None,
        objdump_tool=None,
        sources=None,
        everything=False,
        **args):
    symbol_pattern = re.compile(
        '^(?P<addr>[0-9a-fA-F]+)\s.*\s(?P<name>[^\s]+)\s*$')
    line_pattern = re.compile(
        '^\s+(?:'
            # matches dir/file table
            '(?P<no>[0-9]+)\s+'
                '(?:(?P<dir>[0-9]+)\s+)?'
                '.*\s+'
                '(?P<path>[^\s]+)'
            # matches line opcodes
            '|' '\[[^\]]*\]\s+'
                '(?:'
                    '(?P<op_special>Special)'
                    '|' '(?P<op_copy>Copy)'
                    '|' '(?P<op_end>End of Sequence)'
                    '|' 'File .*?to (?:entry )?(?P<op_file>\d+)'
                    '|' 'Line .*?to (?P<op_line>[0-9]+)'
                    '|' '(?:Address|PC) .*?to (?P<op_addr>[0x0-9a-fA-F]+)'
                    '|' '.' ')*'
            ')$', re.IGNORECASE)

    records = []
    for path in paths:
        # each .perf file is actually a zip file containing perf files from
        # multiple runs
        with zipfile.ZipFile(path) as z:
            records.extend((path, i) for i in z.infolist())

    # we're dealing with a lot of data but also surprisingly
    # parallelizable
    dsos = {}
    results = []
    with mp.Pool(jobs or len(os.sched_getaffinity(0))) as p:
        for results_ in p.imap_unordered(
                starapply,
                ((collect_job, (path, i), dict(
                    everything=everything,
                    **args))
                    for path, i in records)):

            # organize by dso
            results__ = {}
            for r in results_:
                if r.file not in results__:
                    results__[r.file] = []
                results__[r.file].append(r)
            results_ = results__

            for dso, results_ in results_.items():
                if dso not in dsos:
                    # find file+line ranges for dsos
                    #
                    # do this here so we only process each dso once
                    syms = {}
                    sym_at = []
                    cmd = objdump_tool + ['-t', dso]
                    if args.get('verbose'):
                        print(' '.join(shlex.quote(c) for c in cmd))
                    proc = sp.Popen(cmd,
                        stdout=sp.PIPE,
                        stderr=sp.PIPE if not args.get('verbose') else None,
                        universal_newlines=True,
                        errors='replace',
                        close_fds=False)
                    for line in proc.stdout:
                        m = symbol_pattern.match(line)
                        if m:
                            name = m.group('name')
                            addr = int(m.group('addr'), 16)
                            # note multiple symbols can share a name
                            if name not in syms:
                                syms[name] = set()
                            syms[name].add(addr)
                            sym_at.append((addr, name))
                    proc.wait()
                    if proc.returncode != 0:
                        if not args.get('verbose'):
                            for line in proc.stderr:
                                sys.stdout.write(line)
                        # assume no debug-info on failure
                        pass

                    # sort and keep first when duplicates
                    sym_at.sort()
                    sym_at_ = []
                    for addr, name in sym_at:
                        if len(sym_at_) == 0 or sym_at_[-1][0] != addr:
                            sym_at_.append((addr, name))
                    sym_at = sym_at_

                    # state machine for dwarf line numbers, note that objdump's
                    # decodedline seems to have issues with multiple dir/file
                    # tables, which is why we need this
                    line_at = []
                    dirs = {}
                    files = {}
                    op_file = 1
                    op_line = 1
                    op_addr = 0
                    cmd = objdump_tool + ['--dwarf=rawline', dso]
                    if args.get('verbose'):
                        print(' '.join(shlex.quote(c) for c in cmd))
                    proc = sp.Popen(cmd,
                        stdout=sp.PIPE,
                        stderr=sp.PIPE if not args.get('verbose') else None,
                        universal_newlines=True,
                        errors='replace',
                        close_fds=False)
                    for line in proc.stdout:
                        m = line_pattern.match(line)
                        if m:
                            if m.group('no') and not m.group('dir'):
                                # found a directory entry
                                dirs[int(m.group('no'))] = m.group('path')
                            elif m.group('no'):
                                # found a file entry
                                dir = int(m.group('dir'))
                                if dir in dirs:
                                    files[int(m.group('no'))] = os.path.join(
                                        dirs[dir],
                                        m.group('path'))
                                else:
                                    files[int(m.group('no'))] = m.group('path')
                            else:
                                # found a state machine update
                                if m.group('op_file'):
                                    op_file = int(m.group('op_file'), 0)
                                if m.group('op_line'):
                                    op_line = int(m.group('op_line'), 0)
                                if m.group('op_addr'):
                                    op_addr = int(m.group('op_addr'), 0)

                                if (m.group('op_special')
                                        or m.group('op_copy')
                                        or m.group('op_end')):
                                    line_at.append((
                                        op_addr,
                                        files.get(op_file, '?'),
                                        op_line))

                                if m.group('op_end'):
                                    op_file = 1
                                    op_line = 1
                                    op_addr = 0
                    proc.wait()
                    if proc.returncode != 0:
                        if not args.get('verbose'):
                            for line in proc.stderr:
                                sys.stdout.write(line)
                        # assume no debug-info on failure
                        pass

                    # sort and keep first when duplicates
                    #
                    # I think dwarf requires this to be sorted but just in case
                    line_at.sort()
                    line_at_ = []
                    for addr, file, line in line_at:
                        if len(line_at_) == 0 or line_at_[-1][0] != addr:
                            line_at_.append((addr, file, line))
                    line_at = line_at_

                    # discard lines outside of the range of the containing
                    # function, these are introduced by dwarf for inlined
                    # functions but don't map to elf-level symbols
                    sym_at_ = []
                    for addr, sym in sym_at:
                        i = bisect.bisect(line_at, addr, key=lambda x: x[0])
                        if i > 0:
                            _, file, line = line_at[i-1]
                            sym_at_.append((file, line, sym))
                    sym_at_.sort()

                    line_at_ = []
                    for addr, file, line in line_at:
                        # only keep if sym-at-addr and sym-at-line match
                        i = bisect.bisect(
                            sym_at, addr, key=lambda x: x[0])
                        j = bisect.bisect(
                            sym_at_, (file, line), key=lambda x: (x[0], x[1]))
                        if i > 0 and j > 0 and (
                                sym_at[i-1][1] == sym_at_[j-1][2]):
                            line_at_.append((addr, file, line))
                    line_at = line_at_

                    dsos[dso] = (syms, sym_at, line_at)

                syms, _, line_at = dsos[dso]

                # first try to reverse ASLR
                def deltas(r, d):
                    if '+' in r.function:
                        sym, off = r.function.split('+', 1)
                        off = int(off, 0)
                    else:
                        sym, off = r.function, 0
                    addr = r.line - off + d

                    for addr_ in syms.get(sym, []):
                        yield addr_ - addr

                delta = min(
                    it.chain.from_iterable(
                        deltas(r, 0) for r in results_),
                    key=lambda d: sum(it.chain.from_iterable(
                        deltas(r, d) for r in results_)),
                    default=0)

                # then try to map addrs -> file+line
                #
                # note we need to do this recursively
                def remap(results):
                    results_ = []
                    for r in results:
                        addr = r.line + delta
                        i = bisect.bisect(line_at, addr, key=lambda x: x[0])
                        if i > 0:
                            _, file, line = line_at[i-1]
                        else:
                            file, line = re.sub('(\.o)?$', '.c', r.file, 1), 0

                        # ignore filtered sources
                        if sources is not None:
                            if not any(
                                    os.path.abspath(file) == os.path.abspath(s)
                                    for s in sources):
                                continue
                        else:
                            # default to only cwd
                            if not everything and not os.path.commonpath([
                                    os.getcwd(),
                                    os.path.abspath(file)]) == os.getcwd():
                                continue

                        # simplify path
                        if os.path.commonpath([
                                os.getcwd(),
                                os.path.abspath(file)]) == os.getcwd():
                            file = os.path.relpath(file)
                        else:
                            file = os.path.abspath(file)

                        function, *_ = r.function.split('+', 1)
                        results_.append(r._replace(
                            file=file, function=function, line=line,
                            children=remap(r.children)))
                    return results_

                results.extend(remap(results_))

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

    # fold recursively
    folded_ = []
    for r in folded:
        folded_.append(r._replace(children=fold(
            Result, r.children,
            by=by,
            defines=defines)))
    folded = folded_ 

    return folded

def table(Result, results, diff_results=None, *,
        by=None,
        fields=None,
        sort=None,
        summary=False,
        all=False,
        percent=False,
        depth=1,
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
    header = []
    header.append('%s%s' % (
        ','.join(by),
        ' (%d added, %d removed)' % (
            sum(1 for n in table if n not in diff_table),
            sum(1 for n in diff_table if n not in table))
            if diff_results is not None and not percent else '')
        if not summary else '')
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
    header.append('')
    lines.append(header)

    def table_entry(name, r, diff_r=None, ratios=[]):
        entry = []
        entry.append(name)
        if diff_results is None:
            for k in fields:
                entry.append(getattr(r, k).table()
                    if getattr(r, k, None) is not None
                    else types[k].none)
        elif percent:
            for k in fields:
                entry.append(getattr(r, k).diff_table()
                    if getattr(r, k, None) is not None
                    else types[k].diff_none)
        else:
            for k in fields:
                entry.append(getattr(diff_r, k).diff_table()
                    if getattr(diff_r, k, None) is not None
                    else types[k].diff_none)
            for k in fields:
                entry.append(getattr(r, k).diff_table()
                    if getattr(r, k, None) is not None
                    else types[k].diff_none)
            for k in fields:
                entry.append(types[k].diff_diff(
                        getattr(r, k, None),
                        getattr(diff_r, k, None)))
        if diff_results is None:
            entry.append('')
        elif percent:
            entry.append(' (%s)' % ', '.join(
                '+∞%' if t == +m.inf
                else '-∞%' if t == -m.inf
                else '%+.1f%%' % (100*t)
                for t in ratios))
        else:
            entry.append(' (%s)' % ', '.join(
                    '+∞%' if t == +m.inf
                    else '-∞%' if t == -m.inf
                    else '%+.1f%%' % (100*t)
                    for t in ratios
                    if t)
                if any(ratios) else '')
        return entry

    # entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is None:
                diff_r = None
                ratios = None
            else:
                diff_r = diff_table.get(name)
                ratios = [
                    types[k].ratio(
                        getattr(r, k, None),
                        getattr(diff_r, k, None))
                    for k in fields]
                if not all_ and not any(ratios):
                    continue
            lines.append(table_entry(name, r, diff_r, ratios))

    # total
    r = next(iter(fold(Result, results, by=[])), None)
    if diff_results is None:
        diff_r = None
        ratios = None
    else:
        diff_r = next(iter(fold(Result, diff_results, by=[])), None)
        ratios = [
            types[k].ratio(
                getattr(r, k, None),
                getattr(diff_r, k, None))
            for k in fields]
    lines.append(table_entry('TOTAL', r, diff_r, ratios))

    # find the best widths, note that column 0 contains the names and column -1
    # the ratios, so those are handled a bit differently
    widths = [
        ((max(it.chain([w], (len(l[i]) for l in lines)))+1+4-1)//4)*4-1
        for w, i in zip(
            it.chain([23], it.repeat(7)),
            range(len(lines[0])-1))]

    # adjust the name width based on the expected call depth, though
    # note this doesn't really work with unbounded recursion
    if not summary and not m.isinf(depth):
        widths[0] += 4*(depth-1)

    # print the tree recursively
    print('%-*s  %s%s' % (
        widths[0], lines[0][0],
        ' '.join('%*s' % (w, x)
            for w, x in zip(widths[1:], lines[0][1:-1])),
        lines[0][-1]))

    if not summary:
        def recurse(results_, depth_, prefixes=('', '', '', '')):
            # rebuild our tables at each layer
            table_ = {
                ','.join(str(getattr(r, k) or '') for k in by): r
                for r in results_}
            names_ = list(table_.keys())

            # sort again at each layer, keep in mind the numbers are
            # changing as we descend
            names_.sort()
            if sort:
                for k, reverse in reversed(sort):
                    names_.sort(key=lambda n: (getattr(table_[n], k),)
                        if getattr(table_.get(n), k, None) is not None else (),
                        reverse=reverse ^ (not k or k in Result._fields))

            for i, name in enumerate(names_):
                r = table_[name]
                is_last = (i == len(names_)-1)

                print('%s%-*s  %s' % (
                    prefixes[0+is_last],
                    widths[0] - (
                        len(prefixes[0+is_last])
                        if not m.isinf(depth) else 0),
                    name,
                    ' '.join('%*s' % (w, x)
                        for w, x in zip(
                            widths[1:],
                            table_entry(name, r)[1:]))))

                # recurse?
                if depth_ > 1:
                    recurse(
                        r.children,
                        depth_-1,
                        (prefixes[2+is_last] + "|-> ",
                         prefixes[2+is_last] + "'-> ",
                         prefixes[2+is_last] + "|   ",
                         prefixes[2+is_last] + "    "))

        # we have enough going on with diffing to make the top layer
        # a special case
        for name, line in zip(names, lines[1:-1]):
            print('%-*s  %s%s' % (
                widths[0], line[0],
                ' '.join('%*s' % (w, x)
                    for w, x in zip(widths[1:], line[1:-1])),
                line[-1]))

            if name in table and depth > 1:
                recurse(
                    table[name].children,
                    depth-1,
                    ("|-> ",
                     "'-> ",
                     "|   ",
                     "    "))

    print('%-*s  %s%s' % (
        widths[0], lines[-1][0],
        ' '.join('%*s' % (w, x)
            for w, x in zip(widths[1:], lines[-1][1:-1])),
        lines[-1][-1]))


def annotate(Result, results, *,
        annotate=None,
        threshold=None,
        branches=False,
        caches=False,
        **args):
    # figure out the threshold
    if threshold is None:
        t0, t1 = THRESHOLD
    elif len(threshold) == 1:
        t0, t1 = threshold[0], threshold[0]
    else:
        t0, t1 = threshold
    t0, t1 = min(t0, t1), max(t0, t1)

    if not branches and not caches:
        tk = 'cycles'
    elif branches:
        tk = 'bmisses'
    else:
        tk = 'cmisses'

    # find max cycles
    max_ = max(it.chain((float(getattr(r, tk)) for r in results), [1]))

    for path in co.OrderedDict.fromkeys(r.file for r in results).keys():
        # flatten to line info
        results = fold(Result, results, by=['file', 'line'])
        table = {r.line: r for r in results if r.file == path}

        # calculate spans to show
        if not annotate:
            spans = []
            last = None
            func = None
            for line, r in sorted(table.items()):
                if float(getattr(r, tk)) / max_ >= t0:
                    if last is not None and line - last.stop <= args['context']:
                        last = range(
                            last.start,
                            line+1+args['context'])
                    else:
                        if last is not None:
                            spans.append((last, func))
                        last = range(
                            line-args['context'],
                            line+1+args['context'])
                        func = r.function
            if last is not None:
                spans.append((last, func))

        with open(path) as f:
            skipped = False
            for i, line in enumerate(f):
                # skip lines not in spans?
                if not annotate and not any(i+1 in s for s, _ in spans):
                    skipped = True
                    continue

                if skipped:
                    skipped = False
                    print('%s@@ %s:%d: %s @@%s' % (
                        '\x1b[36m' if args['color'] else '',
                        path,
                        i+1,
                        next(iter(f for _, f in spans)),
                        '\x1b[m' if args['color'] else ''))

                # build line
                if line.endswith('\n'):
                    line = line[:-1]

                r = table.get(i+1)
                if r is not None and (
                        float(r.cycles) > 0
                        if not branches and not caches
                        else float(r.bmisses) > 0 or float(r.branches) > 0
                        if branches
                        else float(r.cmisses) > 0 or float(r.caches) > 0):
                    line = '%-*s // %s' % (
                        args['width'],
                        line,
                        '%s cycles' % r.cycles
                        if not branches and not caches
                        else '%s bmisses, %s branches' % (r.bmisses, r.branches)
                        if branches
                        else '%s cmisses, %s caches' % (r.cmisses, r.caches))

                    if args['color']:
                        if float(getattr(r, tk)) / max_ >= t1:
                            line = '\x1b[1;31m%s\x1b[m' % line
                        elif float(getattr(r, tk)) / max_ >= t0:
                            line = '\x1b[35m%s\x1b[m' % line

                print(line)


def report(perf_paths, *,
        by=None,
        fields=None,
        defines=None,
        sort=None,
        self=False,
        branches=False,
        caches=False,
        **args):
    # figure out what color should be
    if args.get('color') == 'auto':
        args['color'] = sys.stdout.isatty()
    elif args.get('color') == 'always':
        args['color'] = True
    else:
        args['color'] = False

    # depth of 0 == m.inf
    if args.get('depth') == 0:
        args['depth'] = m.inf

    # find sizes
    if not args.get('use', None):
        # find .o files
        paths = []
        for path in perf_paths:
            if os.path.isdir(path):
                path = path + '/*.perf'

            for path in glob.glob(path):
                paths.append(path)

        if not paths:
            print("error: no .perf files found in %r?" % perf_paths)
            sys.exit(-1)

        results = collect(paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f, restval='')
            for r in reader:
                try:
                    results.append(PerfResult(
                        **{k: r[k] for k in PerfResult._by
                            if k in r and r[k].strip()},
                        **{k: r['perf_'+k] for k in PerfResult._fields
                            if 'perf_'+k in r and r['perf_'+k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(PerfResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(key=lambda r: (getattr(r, k),)
                if getattr(r, k) is not None else (),
                reverse=reverse ^ (not k or k in PerfResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f,
                (by if by is not None else PerfResult._by)
                + ['perf_'+k for k in PerfResult._fields])
            writer.writeheader()
            for r in results:
                writer.writerow(
                    {k: getattr(r, k)
                        for k in (by if by is not None else PerfResult._by)}
                    | {'perf_'+k: getattr(r, k)
                        for k in PerfResult._fields})

    # find previous results?
    if args.get('diff'):
        diff_results = []
        try:
            with openio(args['diff']) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    try:
                        diff_results.append(PerfResult(
                            **{k: r[k] for k in PerfResult._by
                                if k in r and r[k].strip()},
                            **{k: r['perf_'+k] for k in PerfResult._fields
                                if 'perf_'+k in r and r['perf_'+k].strip()}))
                    except TypeError:
                        pass
        except FileNotFoundError:
            pass

        # fold
        diff_results = fold(PerfResult, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        if args.get('annotate') or args.get('threshold'):
            # annotate sources
            annotate(PerfResult, results,
                branches=branches,
                caches=caches,
                **args)
        else:
            # print table
            table(PerfResult, results,
                diff_results if args.get('diff') else None,
                by=by if by is not None else ['function'],
                fields=fields if fields is not None
                    else ['cycles'] if not branches and not caches
                    else ['bmisses', 'branches'] if branches
                    else ['cmisses', 'caches'],
                sort=sort,
                **args)


def main(**args):
    if args.get('record'):
        return record(**args)
    else:
        return report(**args)


if __name__ == "__main__":
    import argparse
    import sys

    # bit of a hack, but parse_intermixed_args and REMAINDER are
    # incompatible, so we need to figure out what we want before running
    # argparse
    if '-R' in sys.argv or '--record' in sys.argv:
        nargs = argparse.REMAINDER
    else:
        nargs = '*'

    argparse.ArgumentParser._handle_conflict_ignore = lambda *_: None
    argparse._ArgumentGroup._handle_conflict_ignore = lambda *_: None
    parser = argparse.ArgumentParser(
        description="Aggregate and report Linux perf results.",
        allow_abbrev=False,
        conflict_handler='ignore')
    parser.add_argument(
        'perf_paths',
        nargs=nargs,
        help="Description of where to find *.perf files. May be a directory "
            "or a list of paths. Defaults to %r." % PERF_PATHS)
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
        choices=PerfResult._by,
        help="Group by this field.")
    parser.add_argument(
        '-f', '--field',
        dest='fields',
        action='append',
        choices=PerfResult._fields,
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
        '-F', '--source',
        dest='sources',
        action='append',
        help="Only consider definitions in this file. Defaults to anything "
            "in the current directory.")
    parser.add_argument(
        '--everything',
        action='store_true',
        help="Include builtin and libc specific symbols.")
    parser.add_argument(
        '--branches',
        action='store_true',
        help="Show branches and branch misses.")
    parser.add_argument(
        '--caches',
        action='store_true',
        help="Show cache accesses and cache misses.")
    parser.add_argument(
        '-P', '--propagate',
        type=lambda x: int(x, 0),
        help="Depth to propagate samples up the call-stack. 0 propagates up "
            "to the entry point, 1 does no propagation. Defaults to 0.")
    parser.add_argument(
        '-Z', '--depth',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Depth of function calls to show. 0 shows all calls but may not "
            "terminate!")
    parser.add_argument(
        '-A', '--annotate',
        action='store_true',
        help="Show source files annotated with coverage info.")
    parser.add_argument(
        '-T', '--threshold',
        nargs='?',
        type=lambda x: tuple(float(x) for x in x.split(',')),
        const=THRESHOLD,
        help="Show lines wth samples above this threshold as a percent of "
            "all lines. Defaults to %s." % ','.join(str(t) for t in THRESHOLD))
    parser.add_argument(
        '-c', '--context',
        type=lambda x: int(x, 0),
        default=3,
        help="Show n additional lines of context. Defaults to 3.")
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
        '-j', '--jobs',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Number of processes to use. 0 spawns one process per core.")
    parser.add_argument(
        '--perf-tool',
        type=lambda x: x.split(),
        help="Path to the perf tool to use. Defaults to %r." % PERF_TOOL)
    parser.add_argument(
        '--objdump-tool',
        type=lambda x: x.split(),
        default=OBJDUMP_TOOL,
        help="Path to the objdump tool to use. Defaults to %r." % OBJDUMP_TOOL)

    # record flags
    record_parser = parser.add_argument_group('record options')
    record_parser.add_argument(
        'command',
        nargs=nargs,
        help="Command to run.")
    record_parser.add_argument(
        '-R', '--record',
        action='store_true',
        help="Run a command and aggregate perf measurements.")
    record_parser.add_argument(
        '-o', '--output',
        help="Output file. Uses flock to synchronize. This is stored as a "
            "zip-file of multiple perf results.")
    record_parser.add_argument(
        '--perf-freq',
        help="perf sampling frequency. This is passed directly to perf. "
            "Defaults to %r." % PERF_FREQ)
    record_parser.add_argument(
        '--perf-period',
        help="perf sampling period. This is passed directly to perf.")
    record_parser.add_argument(
        '--perf-events',
        help="perf events to record. This is passed directly to perf. "
            "Defaults to %r." % PERF_EVENTS)
    record_parser.add_argument(
        '--perf-tool',
        type=lambda x: x.split(),
        help="Path to the perf tool to use. Defaults to %r." % PERF_TOOL)

    # avoid intermixed/REMAINDER conflict, see above
    if nargs == argparse.REMAINDER:
        args = parser.parse_args()
    else:
        args = parser.parse_intermixed_args()

    # perf_paths/command overlap, so need to do some munging here
    args.command = args.perf_paths
    args.perf_paths = args.perf_paths or PERF_PATHS

    sys.exit(main(**{k: v
        for k, v in vars(args).items()
        if v is not None}))
