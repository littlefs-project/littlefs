#!/usr/bin/env python3
#
# Script to find data size at the function level. Basically just a big wrapper
# around nm with some extra conveniences for comparing builds. Heavily inspired
# by Linux's Bloat-O-Meter.
#
# Example:
# ./scripts/data.py lfs.o lfs_util.o -Ssize
#
# Copyright (c) 2022, The littlefs authors.
# Copyright (c) 2020, Arm Limited. All rights reserved.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
__import__('sys').path.pop(0)

import collections as co
import csv
import itertools as it
import functools as ft
import math as mt
import os
import re
import shlex
import subprocess as sp


OBJDUMP_PATH = ['objdump']
SECTIONS = ['.data', '.bss']


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

# data size results
class DataResult(co.namedtuple('DataResult', [
        'file', 'function',
        'size'])):
    _by = ['file', 'function']
    _fields = ['size']
    _sort = ['size']
    _types = {'size': RInt}

    __slots__ = ()
    def __new__(cls, file='', function='', size=0):
        return super().__new__(cls, file, function,
                RInt(size))

    def __add__(self, other):
        return DataResult(self.file, self.function,
                self.size + other.size)


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

def collect_dwarf_files(obj_path, *,
        objdump_path=OBJDUMP_PATH,
        **args):
    line_pattern = re.compile(
            '^\s*(?P<no>[0-9]+)'
                '(?:\s+(?P<dir>[0-9]+))?'
                '.*\s+(?P<path>[^\s]+)\s*$')

    # find source paths
    dirs = co.OrderedDict()
    files = co.OrderedDict()
    # note objdump-path may contain extra args
    cmd = objdump_path + ['--dwarf=rawline', obj_path]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    for line in proc.stdout:
        # note that files contain references to dirs, which we
        # dereference as soon as we see them as each file table
        # follows a dir table
        m = line_pattern.match(line)
        if m:
            if not m.group('dir'):
                # found a directory entry
                dirs[int(m.group('no'))] = m.group('path')
            else:
                # found a file entry
                dir = int(m.group('dir'))
                if dir in dirs:
                    files[int(m.group('no'))] = os.path.join(
                            dirs[dir],
                            m.group('path'))
                else:
                    files[int(m.group('no'))] = m.group('path')
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    # simplify paths
    files_ = co.OrderedDict()
    for no, file in files.items():
        if os.path.commonpath([
                    os.getcwd(),
                    os.path.abspath(file)]) == os.getcwd():
            files_[no] = os.path.relpath(file)
        else:
            files_[no] = os.path.abspath(file)
    files = files_

    return files

# each dwarf entry can have attrs and children entries
class DwarfEntry:
    def __init__(self, level, off, tag, ats={}, children=[]):
        self.level = level
        self.off = off
        self.tag = tag
        self.ats = ats or {}
        self.children = children or []

    def get(self, k, d=None):
        return self.ats.get(k, d)

    def __getitem__(self, k):
        return self.ats[k]

    def __contains__(self, k):
        return k in self.ats

    def __repr__(self):
        return '%s(%d, 0x%x, %r, %r)' % (
                self.__class__.__name__,
                self.level,
                self.off,
                self.tag,
                self.ats)

    @ft.cached_property
    def name(self):
        if 'DW_AT_name' in self:
            name = self['DW_AT_name'].split(':')[-1].strip()
            # prefix with struct/union/enum
            if self.tag == 'DW_TAG_structure_type':
                name = 'struct ' + name
            elif self.tag == 'DW_TAG_union_type':
                name = 'union ' + name
            elif self.tag == 'DW_TAG_enumeration_type':
                name = 'enum ' + name
            return name
        else:
            return None

# a collection of dwarf entries
class DwarfInfo:
    def __init__(self, entries):
        self.entries = entries

    def get(self, k, d=None):
        # allow lookup by both offset and dwarf name
        if not isinstance(k, str):
            return self.entries.get(k, d)

        else:
            # organize entries by name
            if not hasattr(self, '_by_name'):
                self._by_name = {}
                for entry in self.entries.values():
                    if entry.name is not None:
                        self._by_name[entry.name] = entry

            # exact match? do a quick lookup
            if k in self._by_name:
                return self._by_name[k]
            # find the best matching dwarf entry with a simple
            # heuristic
            #
            # this can be different from the actual symbol because
            # of optimization passes
            else:
                def key(entry):
                    i = k.find(entry.name)
                    if i == -1:
                        return None
                    return (i, len(k)-(i+len(entry.name)), k)
                return min(
                        filter(key, self._by_name.values()),
                        key=key,
                        default=d)

    def __getitem__(self, k):
        v = self.get(k)
        if v is None:
            raise KeyError(k)
        return v

    def __contains__(self, k):
        return self.get(k) is not None

    def __len__(self):
        return len(self.entries)

    def __iter__(self):
        return (v for k, v in self.entries.items())

def collect_dwarf_info(obj_path, tags=None, *,
        objdump_path=OBJDUMP_PATH,
        **args):
    info_pattern = re.compile(
            '^\s*<(?P<level>[^>]*)>'
                    '\s*<(?P<off>[^>]*)>'
                    '.*\(\s*(?P<tag>[^)]*?)\s*\)\s*$'
                '|' '^\s*<(?P<off_>[^>]*)>'
                    '\s*(?P<at>[^>:]*?)'
                    '\s*:(?P<v>.*)\s*$')

    # collect dwarf entries
    info = co.OrderedDict()
    entry = None
    levels = {}
    # note objdump-path may contain extra args
    cmd = objdump_path + ['--dwarf=info', obj_path]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    for line in proc.stdout:
        # state machine here to find dwarf entries
        m = info_pattern.match(line)
        if m:
            if m.group('tag'):
                entry = DwarfEntry(
                    level=int(m.group('level'), 0),
                    off=int(m.group('off'), 16),
                    tag=m.group('tag').strip(),
                )
                # keep track of unfiltered entries
                if tags is None or entry.tag in tags:
                    info[entry.off] = entry
                # store entry in parent
                levels[entry.level] = entry
                if entry.level-1 in levels:
                    levels[entry.level-1].children.append(entry)
            elif m.group('at'):
                if entry:
                    entry.ats[m.group('at').strip()] = (
                            m.group('v').strip())
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    return DwarfInfo(info)

def collect(obj_paths, *,
        sources=None,
        everything=False,
        **args):
    results = []
    for obj_path in obj_paths:
        # find relevant symbols and sizes
        syms = collect_syms(obj_path,
                sections=SECTIONS,
                **args)

        # find source paths
        files = collect_dwarf_files(obj_path, **args)

        # find dwarf info
        info = collect_dwarf_info(obj_path,
                tags={'DW_TAG_subprogram', 'DW_TAG_variable'},
                **args)

        # map function sizes to debug symbols
        for sym in syms:
            # discard internal functions
            if not everything and sym.name.startswith('__'):
                continue

            # find best matching dwarf entry, this may be slightly different
            # due to optimizations
            entry = info.get(sym.name)

            # if we have no file guess from obj path
            if entry is not None and 'DW_AT_decl_file' in entry:
                file = files.get(int(entry['DW_AT_decl_file']), '?')
            else:
                file = re.sub('(\.o)?$', '.c', obj_path, 1)

            # ignore filtered sources
            if sources is not None:
                if not any(os.path.abspath(file) == os.path.abspath(s)
                        for s in sources):
                    continue
            else:
                # default to only cwd
                if not everything and not os.path.commonpath([
                        os.getcwd(),
                        os.path.abspath(file)]) == os.getcwd():
                    continue

            results.append(DataResult(file, sym.name, sym.size))

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
        if hasattr(Result, '_notes'):
            entry[-1][1].extend(getattr(r, Result._notes))
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


def main(obj_paths, *,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        **args):
    # find sizes
    if not args.get('use', None):
        results = collect(obj_paths, **args)
    else:
        results = []
        with openio(args['use']) as f:
            reader = csv.DictReader(f, restval='')
            for r in reader:
                # filter by matching defines
                if not all(k in r and r[k] in vs for k, vs in defines):
                    continue

                try:
                    results.append(DataResult(
                            **{k: r[k] for k in DataResult._by
                                if k in r and r[k].strip()},
                            **{k: r[k] for k in DataResult._fields
                                if k in r and r[k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(DataResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(
                    key=lambda r: tuple(
                        (getattr(r, k),) if getattr(r, k) is not None else ()
                            for k in ([k] if k else DataResult._sort)),
                    reverse=reverse ^ (not k or k in DataResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f,
                    (by if by is not None else DataResult._by)
                        + [k for k in (
                            fields if fields is not None
                                else DataResult._fields)])
            writer.writeheader()
            for r in results:
                writer.writerow(
                        {k: getattr(r, k) for k in (
                                by if by is not None else DataResult._by)}
                            | {k: getattr(r, k) for k in (
                                fields if fields is not None
                                    else DataResult._fields)})

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
                            for k in DataResult._fields):
                        continue
                    try:
                        diff_results.append(DataResult(
                                **{k: r[k] for k in DataResult._by
                                    if k in r and r[k].strip()},
                                **{k: r[k] for k in DataResult._fields
                                    if k in r and r[k].strip()}))
                    except TypeError:
                        pass
        except FileNotFoundError:
            pass

        # fold
        diff_results = fold(DataResult, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        table(DataResult, results, diff_results,
                by=by if by is not None else ['function'],
                fields=fields,
                sort=sort,
                **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Find data size at the function level.",
            allow_abbrev=False)
    parser.add_argument(
            'obj_paths',
            nargs='*',
            help="Input *.o files.")
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
            choices=DataResult._by,
            help="Group by this field.")
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action='append',
            choices=DataResult._fields,
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
            '--objdump-path',
            type=lambda x: x.split(),
            default=OBJDUMP_PATH,
            help="Path to the objdump executable, may include flags. "
                "Defaults to %r." % OBJDUMP_PATH)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
