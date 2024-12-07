#!/usr/bin/env python3
#
# Aggregate and report call-stack propagated block-device operations
# from trace output.
#
# Example:
# ./scripts/bench.py -ttrace
# ./scripts/perfbd.py trace -j -Flfs.c -Flfs_util.c -Serased -Sproged -Sreaded
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
__import__('sys').path.pop(0)

import bisect
import collections as co
import csv
import functools as ft
import itertools as it
import math as mt
import multiprocessing as mp
import os
import re
import shlex
import subprocess as sp


OBJDUMP_PATH = ['objdump']
THRESHOLD = (0.5, 0.85)


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
        if not (isinstance(x, int) or mt.isinf(x)):
            x = int(x)
        return super().__new__(cls, x)

    def __repr__(self):
        return '%s(%r)' % (self.__class__.__name__, self.x)

    def __str__(self):
        if self.x == mt.inf:
            return '∞'
        elif self.x == -mt.inf:
            return '-∞'
        else:
            return str(self.x)

    def __bool__(self):
        return bool(self.x)

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

    def __pos__(self):
        return self.__class__(+self.x)

    def __neg__(self):
        return self.__class__(-self.x)

    def __abs__(self):
        return self.__class__(abs(self.x))

    def __add__(self, other):
        return self.__class__(self.x + other.x)

    def __sub__(self, other):
        return self.__class__(self.x - other.x)

    def __mul__(self, other):
        return self.__class__(self.x * other.x)

    def __truediv__(self, other):
        if not other:
            if self >= self.__class__(0):
                return self.__class__(+mt.inf)
            else:
                return self.__class__(-mt.inf)
        return self.__class__(self.x // other.x)

    def __mod__(self, other):
        return self.__class__(self.x % other.x)

# perf results
class PerfBdResult(co.namedtuple('PerfBdResult', [
        'file', 'function', 'line',
        'readed', 'proged', 'erased',
        'children'])):
    _by = ['file', 'function', 'line']
    _fields = ['readed', 'proged', 'erased']
    _sort = ['erased', 'proged', 'readed']
    _types = {'readed': RInt, 'proged': RInt, 'erased': RInt}
    _children = 'children'

    __slots__ = ()
    def __new__(cls, file='', function='', line=0,
            readed=0, proged=0, erased=0,
            children=None):
        return super().__new__(cls, file, function, int(RInt(line)),
                RInt(readed), RInt(proged), RInt(erased),
                children if children is not None else [])

    def __add__(self, other):
        return PerfBdResult(self.file, self.function, self.line,
                self.readed + other.readed,
                self.proged + other.proged,
                self.erased + other.erased,
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

class Sym(co.namedtuple('Sym', [
        'name', 'global_', 'section', 'addr', 'size'])):
    __slots__ = ()
    def __new__(cls, name, global_, section, addr, size):
        return super().__new__(cls, name, global_, section, addr, size)

    def __repr__(self):
        return '%s(%r, %r, %r, 0x%x, 0x%x)' % (
                self.__class__.__name__,
                self.name,
                self.global_,
                self.section,
                self.addr,
                self.size)

class SymInfo:
    def __init__(self, syms):
        self.syms = syms

    def get(self, k, d=None):
        # allow lookup by both symbol and address
        if isinstance(k, str):
            # organize by symbol, note multiple symbols can share a name
            if not hasattr(self, '_by_sym'):
                by_sym = {}
                for sym in self.syms:
                    if sym.name not in by_sym:
                        by_sym[sym.name] = []
                    if sym not in by_sym[sym.name]:
                        by_sym[sym.name].append(sym)
                self._by_sym = by_sym

            return self._by_sym.get(k, d)

        else:
            import bisect

            # organize by address
            if not hasattr(self, '_by_addr'):
                # sort and keep largest/first when duplicates
                syms = self.syms.copy()
                syms.sort(key=lambda x: (x.addr, -x.size))

                by_addr = []
                for sym in syms:
                    if (len(by_addr) == 0
                            or by_addr[-1].addr != sym.addr):
                        by_addr.append(sym)
                self._by_addr = by_addr

            # find sym by range
            i = bisect.bisect(self._by_addr, k,
                    key=lambda x: x.addr)
            # check that we're actually in this sym's size
            if i > 0 and k < self._by_addr[i-1].addr+self._by_addr[i-1].size:
                return self._by_addr[i-1]
            else:
                return d

    def __getitem__(self, k):
        v = self.get(k)
        if v is None:
            raise KeyError(k)
        return v

    def __contains__(self, k):
        return self.get(k) is not None

    def __len__(self):
        return len(self.syms)

    def __iter__(self):
        return iter(self.syms)

    def globals(self):
        return SymInfo([sym for sym in self.syms
                if sym.global_])

    def section(self, section):
        return SymInfo([sym for sym in self.syms
                # note we accept prefixes
                if s.startswith(section)])

def collect_syms(obj_path, global_=False, sections=None, *,
        objdump_path=OBJDUMP_PATH,
        **args):
    symbol_pattern = re.compile(
            '^(?P<addr>[0-9a-fA-F]+)'
                ' (?P<scope>.).*'
                '\s+(?P<section>[^\s]+)'
                '\s+(?P<size>[0-9a-fA-F]+)'
                '\s+(?P<name>[^\s]+)\s*$')

    # find symbol addresses and sizes
    syms = []
    cmd = objdump_path + ['--syms', obj_path]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    for line in proc.stdout:
        m = symbol_pattern.match(line)
        if m:
            name = m.group('name')
            scope = m.group('scope')
            section = m.group('section')
            addr = int(m.group('addr'), 16)
            size = int(m.group('size'), 16)
            # skip non-globals?
            # l => local
            # g => global
            # u => unique global
            #   => neither
            # ! => local + global
            global__ = scope not in 'l '
            if global_ and not global__:
                continue
            # filter by section? note we accept prefixes
            if (sections is not None
                    and not any(section.startswith(prefix)
                        for prefix in sections)):
                continue
            # skip zero sized symbols
            if not size:
                continue
            # note multiple symbols can share a name
            syms.append(Sym(name, global__, section, addr, size))
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    return SymInfo(syms)

class Line(co.namedtuple('Line', ['file', 'line', 'addr'])):
    __slots__ = ()
    def __new__(cls, file, line, addr):
        return super().__new__(cls, file, line, addr)

    def __repr__(self):
        return '%s(%r, %r, 0x%x)' % (
                self.__class__.__name__,
                self.file,
                self.line,
                self.addr)

class LineInfo:
    def __init__(self, lines):
        self.lines = lines

    def get(self, k, d=None):
        # allow lookup by both address and file+line tuple
        if not isinstance(k, tuple):
            import bisect

            # organize by address
            if not hasattr(self, '_by_addr'):
                # sort and keep first when duplicates
                lines = self.lines.copy()
                lines.sort(key=lambda x: (x.addr, x.file, x.line))

                by_addr = []
                for line in lines:
                    if (len(by_addr) == 0
                            or by_addr[-1].addr != line.addr):
                        by_addr.append(line)
                self._by_addr = by_addr

            # find file+line by addr
            i = bisect.bisect(self._by_addr, k,
                    key=lambda x: x.addr)
            if i > 0:
                return self._by_addr[i-1]
            else:
                return d

        else:
            import bisect

            # organize by file+line
            if not hasattr(self, '_by_line'):
                # sort and keep first when duplicates
                lines = self.lines.copy()
                lines.sort()

                by_line = []
                for line in lines:
                    if (len(by_line) == 0
                            or by_line[-1].file != line.file
                            or by_line[-1].line != line.line):
                        by_line.append(line)
                self._by_line = by_line

            # find addr by file+line tuple
            i = bisect.bisect(self._by_line, k,
                    key=lambda x: (x.file, x.line))
            # make sure file at least matches!
            if i > 0 and self._by_line[i-1].file == k[0]:
                return self._by_line[i-1]
            else:
                return d

    def __getitem__(self, k):
        v = self.get(k)
        if v is None:
            raise KeyError(k)
        return v

    def __contains__(self, k):
        return self.get(k) is not None

    def __len__(self):
        return len(self.lines)

    def __iter__(self):
        return iter(self.lines)

def collect_dwarf_lines(obj_path, *,
        objdump_path=OBJDUMP_PATH,
        **args):
    line_pattern = re.compile(
            # matches dir/file table
            '^\s*(?P<no>[0-9]+)'
                    '(?:\s+(?P<dir>[0-9]+))?'
                    '.*\s+(?P<path>[^\s]+)\s*$'
                # matches line opcodes
                '|' '^\s*\[[^\]]*\]' '(?:'
                    '\s+(?P<op_special>Special)'
                        '|' '\s+(?P<op_copy>Copy)'
                        '|' '\s+(?P<op_end>End of Sequence)'
                        '|' '\s+File.*?to.*?(?P<op_file>[0-9]+)'
                        '|' '\s+Line.*?to.*?(?P<op_line>[0-9]+)'
                        '|' '\s+(?:Address|PC)'
                            '\s+.*?to.*?(?P<op_addr>[0xX0-9a-fA-F]+)'
                        '|' '\s+[^\s]+' ')+\s*$',
            re.IGNORECASE)

    # state machine for dwarf line numbers, note that objdump's
    # decodedline seems to have issues with multiple dir/file
    # tables, which is why we need this
    lines = []
    dirs = co.OrderedDict()
    files = co.OrderedDict()
    op_file = 1
    op_line = 1
    op_addr = 0
    cmd = objdump_path + ['--dwarf=rawline', obj_path]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
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
                    file = os.path.abspath(files.get(op_file, '?'))
                    lines.append(Line(file, op_line, op_addr))

                if m.group('op_end'):
                    op_file = 1
                    op_line = 1
                    op_addr = 0
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    return LineInfo(lines)


def collect_job(path, start, stop, syms, lines, *,
        sources=None,
        everything=False,
        propagate=0,
        depth=1,
        **args):
    trace_pattern = re.compile(
            '^(?P<file>[^:]*):(?P<line>[0-9]+):trace:\s*'
                    '(?P<prefix>[^\s]*?bd_)(?:'
                '(?P<read>read)\('
                    '\s*(?P<read_ctx>\w+)' '\s*,'
                    '\s*(?P<read_block>\w+)' '\s*,'
                    '\s*(?P<read_off>\w+)' '\s*,'
                    '\s*(?P<read_buffer>\w+)' '\s*,'
                    '\s*(?P<read_size>\w+)' '\s*\)'
                '|' '(?P<prog>prog)\('
                    '\s*(?P<prog_ctx>\w+)' '\s*,'
                    '\s*(?P<prog_block>\w+)' '\s*,'
                    '\s*(?P<prog_off>\w+)' '\s*,'
                    '\s*(?P<prog_buffer>\w+)' '\s*,'
                    '\s*(?P<prog_size>\w+)' '\s*\)'
                '|' '(?P<erase>erase)\('
                    '\s*(?P<erase_ctx>\w+)' '\s*,'
                    '\s*(?P<erase_block>\w+)'
                    '\s*\(\s*(?P<erase_size>\w+)\s*\)' '\s*\)'
            ')\s*$')
    frame_pattern = re.compile(
            '^\s+at (?P<addr>\w+)\s*$')

    # parse all of the trace files for read/prog/erase operations
    last_filtered = False
    last_file = None
    last_line = None
    last_sym = None
    last_readed = 0
    last_proged = 0
    last_erased = 0
    last_stack = []
    last_delta = None
    at_cache = {}
    results = {}

    def commit():
        # fallback to just capturing top-level measurements
        if not last_stack:
            file = last_file
            sym = last_sym
            line = last_line

            # ignore filtered sources
            if sources is not None:
                if not any(os.path.abspath(file) == os.path.abspath(s)
                        for s in sources):
                    return
            else:
                # default to only cwd
                if not everything and not os.path.commonpath([
                        os.getcwd(),
                        os.path.abspath(file)]) == os.getcwd():
                    return

            # simplify path
            if os.path.commonpath([
                    os.getcwd(),
                    os.path.abspath(file)]) == os.getcwd():
                file = os.path.relpath(file)
            else:
                file = os.path.abspath(file)

            results[(file, sym, line)] = (
                    last_readed,
                    last_proged,
                    last_erased,
                    {})
        else:
            # tail-recursively propagate measurements
            for i in range(len(last_stack)):
                results_ = results
                for j in reversed(range(i+1)):
                    if i+1-j > depth:
                        break

                    # propagate
                    name = last_stack[j]
                    if name in results_:
                        r, p, e, children = results_[name]
                    else:
                        r, p, e, children = 0, 0, 0, {}
                    results_[name] = (
                            r+last_readed,
                            p+last_proged,
                            e+last_erased,
                            children)

                    # recurse
                    results_ = results_[name][-1]

    with openio(path) as f:
        # try to jump to middle of file? need step out of utf8-safe mode and
        # then resync up with the next newline to avoid parsing half a line
        if start is not None and start > 0:
            fd = f.fileno()
            os.lseek(fd, start, os.SEEK_SET)
            while os.read(fd, 1) not in {b'\n', b'\r', b''}:
                pass
            f = os.fdopen(fd)

        for line in f:
            # we have a lot of data, try to take a few shortcuts,
            # string search is much faster than regex so try to use
            # regex as late as possible.
            if not line.startswith('\t'):
                if last_filtered:
                    commit()
                last_filtered = False

                # done processing our slice?
                if stop is not None:
                    if os.lseek(f.fileno(), 0, os.SEEK_CUR) > stop:
                        break

                if 'trace' in line and 'bd' in line:
                    m = trace_pattern.match(line)
                    if m:
                        last_filtered = True
                        last_file = os.path.abspath(m.group('file'))
                        last_line = int(m.group('line'), 0)
                        last_sym = m.group('prefix')
                        last_readed = 0
                        last_proged = 0
                        last_erased = 0
                        last_stack = []
                        last_delta = None

                        if m.group('read'):
                            last_sym += m.group('read')
                            last_readed += int(m.group('read_size'))
                        elif m.group('prog'):
                            last_sym += m.group('prog')
                            last_proged += int(m.group('prog_size'))
                        elif m.group('erase'):
                            last_sym += m.group('erase')
                            last_erased += int(m.group('erase_size'))

            elif last_filtered:
                m = frame_pattern.match(line)
                if m:
                    addr_ = int(m.group('addr'), 0)

                    # before we can do anything with addr, we need to
                    # reverse ASLR, fortunately we know the file+line of
                    # the first stack frame, so we can use that as a point
                    # of reference
                    if last_delta is None:
                        line_ = lines.get((last_file, last_line))
                        if line_ is not None:
                            last_delta = line_.addr - addr_
                        else:
                            # can't reverse ASLR, give up on backtrace
                            commit()
                            last_filtered = False
                            continue

                    addr = addr_ + last_delta

                    # cached?
                    if addr in at_cache:
                        cached = at_cache[addr]
                        if cached is None:
                            # cache says to skip
                            continue
                        file, sym, line = cached
                    else:
                        # find sym
                        sym = syms.get(addr)
                        if sym is not None:
                            sym = sym.name
                        else:
                            sym = hex(addr)

                        # filter out internal/unknown functions
                        if not everything and (
                                sym.startswith('__')
                                    or sym.startswith('0')
                                    or sym.startswith('-')
                                    or sym == '_start'):
                            at_cache[addr] = None
                            continue

                        # find file+line
                        line_ = lines.get(addr)
                        if line_ is not None:
                            file, line = line_.file, line_.line
                        elif len(last_stack) == 0:
                            file, line = last_file, last_line
                        else:
                            file, line = re.sub('(\.o)?$', '.c', obj_path, 1), 0

                        # ignore filtered sources
                        if sources is not None:
                            if not any(
                                    os.path.abspath(file) == os.path.abspath(s)
                                        for s in sources):
                                at_cache[addr] = None
                                continue
                        else:
                            # default to only cwd
                            if not everything and not os.path.commonpath([
                                    os.getcwd(),
                                    os.path.abspath(file)]) == os.getcwd():
                                at_cache[addr] = None
                                continue

                        # simplify path
                        if os.path.commonpath([
                                os.getcwd(),
                                os.path.abspath(file)]) == os.getcwd():
                            file = os.path.relpath(file)
                        else:
                            file = os.path.abspath(file)

                        at_cache[addr] = file, sym, line

                    last_stack.append((file, sym, line))

                    # stop propagating?
                    if propagate and len(last_stack) >= propagate:
                        commit()
                        last_filtered = False
        if last_filtered:
            commit()

    # rearrange results into result type
    def to_results(results):
        results_ = []
        for name, (r, p, e, children) in results.items():
            results_.append(PerfBdResult(*name,
                    r, p, e,
                    children=to_results(children)))
        return results_

    return to_results(results)

def starapply(args):
    f, args, kwargs = args
    return f(*args, **kwargs)

def collect(obj_path, trace_paths, *,
        jobs=None,
        **args):
    # automatic job detection?
    if jobs == 0:
        jobs = len(os.sched_getaffinity(0))

    # find sym/line info to reverse ASLR
    syms = collect_syms(obj_path,
            sections=['.text'],
            **args)
    lines = collect_dwarf_lines(obj_path, **args)

    if jobs is not None:
        # try to split up files so that even single files can be processed
        # in parallel
        #
        # this looks naive, since we're splitting up text files by bytes, but
        # we do proper backtrace delimination in collect_job
        trace_ranges = []
        for path in trace_paths:
            if path == '-':
                trace_ranges.append([(None, None)])
                continue

            size = os.path.getsize(path)
            if size == 0:
                trace_ranges.append([(None, None)])
                continue

            perjob = mt.ceil(size // jobs)
            trace_ranges.append([(i, i+perjob) for i in range(0, size, perjob)])

        results = []
        with mp.Pool(jobs) as p:
            for results_ in p.imap_unordered(
                    starapply,
                    ((collect_job,
                            (path, start, stop, syms, lines),
                            args)
                        for path, ranges in zip(trace_paths, trace_ranges)
                        for start, stop in ranges)):
                results.extend(results_)

    else:
        results = []
        for path in trace_paths:
            results.extend(collect_job(
                    path, None, None, syms, lines,
                    **args))

    return results


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
        diff=None,
        percent=None,
        all=False,
        compare=None,
        summary=False,
        depth=1,
        hot=None,
        detect_cycles=True,
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

    # reduce children to hot paths? only used by some scripts
    if hot:
        # subclass to reintroduce __dict__
        Result_ = Result
        class HotResult(Result_):
            _i = '_hot_i'
            _children = '_hot_children'
            _notes = '_hot_notes'

            def __new__(cls, r, i=None, children=None, notes=None):
                self = HotResult._make(r)
                self._hot_i = i
                self._hot_children = children if children is not None else []
                self._hot_notes = notes if notes is not None else []
                if hasattr(Result_, '_notes'):
                    self._hot_notes.extend(getattr(r, r._notes))
                return self

            def __add__(self, other):
                return HotResult(
                        Result_.__add__(self, other),
                        self._hot_i if other._hot_i is None
                            else other._hot_i if self._hot_i is None
                            else min(self._hot_i, other._hot_i),
                        self._hot_children + other._hot_children,
                        self._hot_notes + other._hot_notes)

        results_ = []
        for r in results:
            hot_ = []
            def recurse(results_, depth_, seen=set()):
                nonlocal hot_
                if not results_:
                    return

                # find the hottest result
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
                hot_.append(HotResult(r, i=len(hot_)))

                # found a cycle?
                if (detect_cycles
                        and tuple(getattr(r, k) for k in Result._by) in seen):
                    hot_[-1]._hot_notes.append('cycle detected')
                    return

                # recurse?
                if depth_ > 1:
                    recurse(getattr(r, Result._children),
                            depth_-1,
                            seen | {tuple(getattr(r, k) for k in Result._by)})

            recurse(getattr(r, Result._children), depth-1)
            results_.append(HotResult(r, children=hot_))

        Result = HotResult
        results = results_

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

    # find compare entry if there is one
    if compare:
        compare_result = table.get(','.join(str(k) for k in compare))

    # sort again, now with diff info, note that python's sort is stable
    names.sort()
    if compare:
        names.sort(
                key=lambda n: (
                    table.get(n) == compare_result,
                    tuple(
                        types[k].ratio(
                                getattr(table.get(n), k, None),
                                getattr(compare_result, k, None))
                            for k in fields)),
                reverse=True)
    if diff or percent:
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
                    if diff else '')
            if not summary else '']
    if not diff:
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
        # normal entry?
        if ((compare is None or r == compare_result)
                and not percent
                and not diff):
            for k in fields:
                entry.append(
                        (getattr(r, k).table(),
                                getattr(getattr(r, k), 'notes', lambda: [])())
                            if getattr(r, k, None) is not None
                            else types[k].none)
        # compare entry?
        elif not percent and not diff:
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
                                    getattr(compare_result, k, None)))))
        # percent entry?
        elif not diff:
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
        # diff entry?
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
        # append any notes
        if hasattr(Result, '_notes') and r is not None:
            notes = getattr(r, Result._notes)
            if isinstance(entry[-1], tuple):
                entry[-1] = (entry[-1][0], entry[-1][1] + notes)
            else:
                entry[-1] = (entry[-1], notes)

        return entry

    # recursive entry helper, only used by some scripts
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
        if hasattr(Result, '_i'):
            names_.sort(key=lambda n: getattr(table_[n], Result._i))
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
            if detect_cycles and name in seen:
                line[-1] = (line[-1][0], line[-1][1] + ['cycle detected'])
            lines.append(line)

            # found a cycle?
            if detect_cycles and name in seen:
                continue

            # recurse?
            if depth_ > 1:
                recurse(getattr(r, Result._children),
                        depth_-1,
                        seen | {name},
                        (prefixes[2+is_last] + "|-> ",
                         prefixes[2+is_last] + "'-> ",
                         prefixes[2+is_last] + "|   ",
                         prefixes[2+is_last] + "    "))

    # entries
    if (not summary) or compare:
        for name in names:
            r = table.get(name)
            if diff_results is None:
                diff_r = None
            else:
                diff_r = diff_table.get(name)
            lines.append(table_entry(name, r, diff_r))

            # recursive entries
            if name in table and depth > 1:
                recurse(getattr(table[name], Result._children),
                        depth-1,
                        {name},
                        ("|-> ",
                         "'-> ",
                         "|   ",
                         "    "))

    # total, unless we're comparing
    if not (compare and not percent and not diff):
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
    widths = co.defaultdict(lambda: 7, {0: 7})
    nwidths = co.defaultdict(lambda: 0)
    for line in lines:
        for i, x in enumerate(line):
            widths[i] = max(widths[i], ((len(x[0])+1+4-1)//4)*4-1)
            if i != len(line)-1:
                nwidths[i] = max(nwidths[i], 1+sum(2+len(n) for n in x[1]))

    # print our table
    for line in lines:
        print('%-*s  %s' % (
                widths[0], line[0][0],
                ' '.join('%*s%-*s' % (
                        widths[i], x[0],
                        nwidths[i], ' (%s)' % ', '.join(x[1]) if x[1] else '')
                    for i, x in enumerate(line[1:], 1))))


def annotate(Result, results, *,
        annotate=None,
        threshold=None,
        read_threshold=None,
        prog_threshold=None,
        erase_threshold=None,
        **args):
    # figure out the thresholds
    if threshold is None:
        threshold = THRESHOLD
    elif len(threshold) == 1:
        threshold = threshold[0], threshold[0]

    if read_threshold is None:
        read_t0, read_t1 = threshold
    elif len(read_threshold) == 1:
        read_t0, read_t1 = read_threshold[0], read_threshold[0]
    else:
        read_t0, read_t1 = read_threshold
    read_t0, read_t1 = min(read_t0, read_t1), max(read_t0, read_t1)

    if prog_threshold is None:
        prog_t0, prog_t1 = threshold
    elif len(prog_threshold) == 1:
        prog_t0, prog_t1 = prog_threshold[0], prog_threshold[0]
    else:
        prog_t0, prog_t1 = prog_threshold
    prog_t0, prog_t1 = min(prog_t0, prog_t1), max(prog_t0, prog_t1)

    if erase_threshold is None:
        erase_t0, erase_t1 = threshold
    elif len(erase_threshold) == 1:
        erase_t0, erase_t1 = erase_threshold[0], erase_threshold[0]
    else:
        erase_t0, erase_t1 = erase_threshold
    erase_t0, erase_t1 = min(erase_t0, erase_t1), max(erase_t0, erase_t1)

    # find maxs
    max_readed = max(it.chain((float(r.readed) for r in results), [1]))
    max_proged = max(it.chain((float(r.proged) for r in results), [1]))
    max_erased = max(it.chain((float(r.erased) for r in results), [1]))

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
                if (float(r.readed) / max_readed >= read_t0
                        or float(r.proged) / max_proged >= prog_t0
                        or float(r.erased) / max_erased >= erase_t0):
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

                if i+1 in table:
                    r = table[i+1]
                    line = '%-*s // %s readed, %s proged, %s erased' % (
                            args['width'],
                            line,
                            r.readed,
                            r.proged,
                            r.erased)

                    if args['color']:
                        if (float(r.readed) / max_readed >= read_t1
                                or float(r.proged) / max_proged >= prog_t1
                                or float(r.erased) / max_erased >= erase_t1):
                            line = '\x1b[1;31m%s\x1b[m' % line
                        elif (float(r.readed) / max_readed >= read_t0
                                or float(r.proged) / max_proged >= prog_t0
                                or float(r.erased) / max_erased >= erase_t0):
                            line = '\x1b[35m%s\x1b[m' % line

                print(line)


def report(obj_path='', trace_paths=[], *,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        **args):
    # figure out what color should be
    if args.get('color') == 'auto':
        args['color'] = sys.stdout.isatty()
    elif args.get('color') == 'always':
        args['color'] = True
    else:
        args['color'] = False

    # figure out depth
    if args.get('depth') is None:
        args['depth'] = mt.inf if args.get('hot') else 1
    elif args.get('depth') == 0:
        args['depth'] = mt.inf

    # find sizes
    if not args.get('use', None):
        results = collect(obj_path, trace_paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f, restval='')
            for r in reader:
                # filter by matching defines
                if not all(k in r and r[k] in vs for k, vs in defines):
                    continue

                if not any(k in r and r[k].strip()
                        for k in PerfBdResult._fields):
                    continue
                try:
                    results.append(PerfBdResult(
                            **{k: r[k] for k in PerfBdResult._by
                                if k in r and r[k].strip()},
                            **{k: r[k] for k in PerfBdResult._fields
                                if k in r and r[k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(PerfBdResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(
                    key=lambda r: tuple(
                        (getattr(r, k),) if getattr(r, k) is not None else ()
                            for k in ([k] if k else PerfBdResult._sort)),
                    reverse=reverse ^ (not k or k in PerfBdResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f,
                    (by if by is not None else PerfBdResult._by)
                        + [k for k in (
                            fields if fields is not None
                                else PerfBdResult._fields)])
            writer.writeheader()
            for r in results:
                writer.writerow(
                        {k: getattr(r, k) for k in (
                                by if by is not None else PerfBdResult._by)}
                            | {k: getattr(r, k) for k in (
                                fields if fields is not None
                                    else PerfBdResult._fields)})

    # find previous results?
    diff_results = None
    if args.get('diff') or args.get('percent'):
        diff_results = []
        try:
            with openio(args.get('diff') or args.get('percent')) as f:
                reader = csv.DictReader(f, restval='')
                for r in reader:
                    # filter by matching defines
                    if not all(k in r and r[k] in vs for k, vs in defines):
                        continue

                    if not any(k in r and r[k].strip()
                            for k in PerfBdResult._fields):
                        continue
                    try:
                        diff_results.append(PerfBdResult(
                                **{k: r[k] for k in PerfBdResult._by
                                    if k in r and r[k].strip()},
                                **{k: r[k] for k in PerfBdResult._fields
                                    if k in r and r[k].strip()}))
                    except TypeError:
                        pass
        except FileNotFoundError:
            pass

        # fold
        diff_results = fold(PerfBdResult, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        if (args.get('annotate')
                or args.get('threshold')
                or args.get('read_threshold')
                or args.get('prog_threshold')
                or args.get('erase_threshold')):
            # annotate sources
            annotate(PerfBdResult, results, **args)
        else:
            # print table
            table(PerfBdResult, results, diff_results,
                    by=by if by is not None else ['function'],
                    fields=fields,
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
    parser = argparse.ArgumentParser(
            description="Aggregate and report call-stack propagated "
                "block-device operations from trace output.",
            allow_abbrev=False)
    parser.add_argument(
            'obj_path',
            nargs='?',
            help="Input executable for mapping addresses to symbols.")
    parser.add_argument(
            'trace_paths',
            nargs='*',
            help="Input *.trace files.")
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
            '-p', '--percent',
            help="Specify CSV file to diff against, but only show precentage "
                "change, not a full diff.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Show all, not just the ones that changed.")
    parser.add_argument(
            '-c', '--compare',
            type=lambda x: tuple(v.strip() for v in x.split(',')),
            help="Compare results to the row matching this by pattern.")
    parser.add_argument(
            '-Y', '--summary',
            action='store_true',
            help="Only show the total.")
    parser.add_argument(
            '-b', '--by',
            action='append',
            choices=PerfBdResult._by,
            help="Group by this field.")
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action='append',
            choices=PerfBdResult._fields,
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
            '-g', '--propagate',
            type=lambda x: int(x, 0),
            help="Depth to propagate samples up the call-stack. 0 propagates "
                "up to the entry point, 1 does no propagation. Defaults to 0.")
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
            '-A', '--annotate',
            action='store_true',
            help="Show source files annotated with coverage info.")
    parser.add_argument(
            '-T', '--threshold',
            nargs='?',
            type=lambda x: tuple(float(x) for x in x.split(',')),
            const=THRESHOLD,
            help="Show lines with any ops above this threshold as a percent "
                "of all lines. Defaults to "
                "%s." % ','.join(str(t) for t in THRESHOLD))
    parser.add_argument(
            '--read-threshold',
            nargs='?',
            type=lambda x: tuple(float(x) for x in x.split(',')),
            const=THRESHOLD,
            help="Show lines with reads above this threshold as a percent "
                "of all lines. Defaults to "
                "%s." % ','.join(str(t) for t in THRESHOLD))
    parser.add_argument(
            '--prog-threshold',
            nargs='?',
            type=lambda x: tuple(float(x) for x in x.split(',')),
            const=THRESHOLD,
            help="Show lines with progs above this threshold as a percent "
                "of all lines. Defaults to "
                "%s." % ','.join(str(t) for t in THRESHOLD))
    parser.add_argument(
            '--erase-threshold',
            nargs='?',
            type=lambda x: tuple(float(x) for x in x.split(',')),
            const=THRESHOLD,
            help="Show lines with erases above this threshold as a percent "
                "of all lines. Defaults to "
                "%s." % ','.join(str(t) for t in THRESHOLD))
    parser.add_argument(
            '-C', '--context',
            type=lambda x: int(x, 0),
            default=3,
            help="Show n additional lines of context. Defaults to 3.")
    parser.add_argument(
            '-W', '--width',
            type=lambda x: int(x, 0),
            default=80,
            help="Assume source is styled with this many columns. Defaults "
                "to 80.")
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
            '--objdump-path',
            type=lambda x: x.split(),
            default=OBJDUMP_PATH,
            help="Path to the objdump executable, may include flags. "
                "Defaults to %r." % OBJDUMP_PATH)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
