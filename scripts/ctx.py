#!/usr/bin/env python3
#
# Script to find function context (params and relevant structs).
#
# Example:
# ./scripts/ctx.py lfs.o lfs_util.o -Ssize
#
# Copyright (c) 2024, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import csv
import functools as ft
import io
import itertools as it
import math as mt
import os
import re
import shlex
import subprocess as sp
import sys


OBJDUMP_PATH = ['objdump']


# integer fields
class CsvInt(co.namedtuple('CsvInt', 'a')):
    __slots__ = ()
    def __new__(cls, a=0):
        if isinstance(a, CsvInt):
            return a
        if isinstance(a, str):
            try:
                a = int(a, 0)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', a):
                    a = mt.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', a):
                    a = -mt.inf
                else:
                    raise
        if not (isinstance(a, int) or mt.isinf(a)):
            a = int(a)
        return super().__new__(cls, a)

    def __repr__(self):
        return '%s(%r)' % (self.__class__.__name__, self.a)

    def __str__(self):
        if self.a == mt.inf:
            return '∞'
        elif self.a == -mt.inf:
            return '-∞'
        else:
            return str(self.a)

    def __bool__(self):
        return bool(self.a)

    def __int__(self):
        assert not mt.isinf(self.a)
        return self.a

    def __float__(self):
        return float(self.a)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    def diff(self, other):
        new = self.a if self else 0
        old = other.a if other else 0
        diff = new - old
        if diff == +mt.inf:
            return '%7s' % '+∞'
        elif diff == -mt.inf:
            return '%7s' % '-∞'
        else:
            return '%+7d' % diff

    def ratio(self, other):
        new = self.a if self else 0
        old = other.a if other else 0
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
        return self.__class__(+self.a)

    def __neg__(self):
        return self.__class__(-self.a)

    def __abs__(self):
        return self.__class__(abs(self.a))

    def __add__(self, other):
        return self.__class__(self.a + other.a)

    def __sub__(self, other):
        return self.__class__(self.a - other.a)

    def __mul__(self, other):
        return self.__class__(self.a * other.a)

    def __truediv__(self, other):
        if not other:
            if self >= self.__class__(0):
                return self.__class__(+mt.inf)
            else:
                return self.__class__(-mt.inf)
        return self.__class__(self.a // other.a)

    def __mod__(self, other):
        return self.__class__(self.a % other.a)

# ctx size results
class CtxResult(co.namedtuple('CtxResult', [
        'z', 'i', 'file', 'function',
        'off', 'size',
        'children', 'notes'])):
    _prefix = 'ctx'
    _by = ['z', 'i', 'file', 'function']
    _fields = ['off', 'size']
    _sort = ['size']
    _types = {'off': CsvInt, 'size': CsvInt}
    _children = 'children'
    _notes = 'notes'

    __slots__ = ()
    def __new__(cls, z=0, i=0, file='', function='', off=0, size=0,
            children=None, notes=None):
        return super().__new__(cls, z, i, file, function,
                CsvInt(off), CsvInt(size),
                children if children is not None else [],
                notes if notes is not None else set())

    def __add__(self, other):
        return CtxResult(self.z, self.i, self.file, self.function,
                min(self.off, other.off),
                max(self.size, other.size),
                self.children + other.children,
                self.notes | other.notes)


# open with '-' for stdin/stdout
def openio(path, mode='r', buffering=-1):
    import os
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
                    key=lambda x: x.addr) - 1
            # check that we're actually in this sym's size
            if i > -1 and k < self._by_addr[i].addr+self._by_addr[i].size:
                return self._by_addr[i]
            else:
                return d

    def __getitem__(self, k):
        v = self.get(k)
        if v is None:
            raise KeyError(k)
        return v

    def __contains__(self, k):
        return self.get(k) is not None

    def __bool__(self):
        return bool(self.syms)

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
        # allow lookup by offset or dwarf name
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

    def __bool__(self):
        return bool(self.entries)

    def __len__(self):
        return len(self.entries)

    def __iter__(self):
        return iter(self.entries.values())

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

def collect_ctx(obj_paths, *,
        internal=False,
        everything=False,
        no_strip=False,
        depth=1,
        **args):
    results = []
    for obj_path in obj_paths:
        # find global symbols
        syms = collect_syms(obj_path,
                sections=['.text'],
                # only include internal symbols if explicitly requested
                global_=not internal and not everything,
                **args)

        # find dwarf info
        info = collect_dwarf_info(obj_path, **args)

        # find source file from dwarf info
        for entry in info:
            if (entry.tag == 'DW_TAG_compile_unit'
                    and 'DW_AT_name' in entry
                    and 'DW_AT_comp_dir' in entry):
                file = os.path.join(
                        entry['DW_AT_comp_dir'].split(':')[-1].strip(),
                        entry['DW_AT_name'].split(':')[-1].strip())
                break
        else:
            # guess from obj path
            file = re.sub('(\.o)?$', '.c', obj_path, 1)

        # simplify path
        if os.path.commonpath([
                os.getcwd(),
                os.path.abspath(file)]) == os.getcwd():
            file = os.path.relpath(file)
        else:
            file = os.path.abspath(file)

        # recursive+cached size finder
        def sizeof(entry, seen=set()):
            # found a cycle? stop here
            if entry.off in seen:
                return 0
            # cached?
            if not hasattr(sizeof, 'cache'):
                sizeof.cache = {}
            if entry.off in sizeof.cache:
                return sizeof.cache[entry.off]

            # pointer? deref and include size
            if entry.tag == 'DW_TAG_pointer_type':
                size = int(entry['DW_AT_byte_size'])
                if 'DW_AT_type' in entry:
                    type = info[int(entry['DW_AT_type'].strip('<>'), 0)]
                    size += sizeof(type, seen | {entry.off})
            # struct? include any nested pointers
            elif entry.tag == 'DW_TAG_structure_type':
                size = int(entry['DW_AT_byte_size'])
                for child in entry.children:
                    if child.tag != 'DW_TAG_member':
                        continue
                    type_ = info[int(child['DW_AT_type'].strip('<>'), 0)]
                    if (type_.tag != 'DW_TAG_pointer_type'
                            or 'DW_AT_type' not in type_):
                        continue
                    type__ = info[int(type_['DW_AT_type'].strip('<>'), 0)]
                    size += sizeof(type__, seen | {entry.off})
            # union? include any nested pointers
            elif entry.tag == 'DW_TAG_union_type':
                size = int(entry['DW_AT_byte_size'])
                size_ = 0
                for child in entry.children:
                    if child.tag != 'DW_TAG_member':
                        continue
                    type_ = info[int(child['DW_AT_type'].strip('<>'), 0)]
                    if (type_.tag != 'DW_TAG_pointer_type'
                            or 'DW_AT_type' not in type_):
                        continue
                    type__ = info[int(type_['DW_AT_type'].strip('<>'), 0)]
                    size_ = max(size_, sizeof(type__, seen | {entry.off}))
                size += size_
            # array? multiply by size
            elif entry.tag == 'DW_TAG_array_type':
                type = info[int(entry['DW_AT_type'].strip('<>'), 0)]
                size = sizeof(type, seen | {entry.off})
                for child in entry.children:
                    if child.tag == 'DW_TAG_subrange_type':
                        size *= int(child['DW_AT_upper_bound']) + 1
            # base type?
            elif entry.tag == 'DW_TAG_base_type':
                size = int(entry['DW_AT_byte_size'])
            # function pointer?
            elif entry.tag == 'DW_TAG_subroutine_type':
                size = 0
            # a modifier?
            elif (entry.tag in {
                        'DW_TAG_typedef',
                        'DW_TAG_array_type',
                        'DW_TAG_enumeration_type',
                        'DW_TAG_formal_parameter',
                        'DW_TAG_member',
                        'DW_TAG_const_type',
                        'DW_TAG_volatile_type',
                        'DW_TAG_restrict_type'}
                    and 'DW_AT_type' in entry):
                type = info[int(entry['DW_AT_type'].strip('<>'), 0)]
                size = sizeof(type, seen | {entry.off})
            # void?
            elif ('DW_AT_type' not in entry
                    and 'DW_AT_byte_size' not in entry):
                size = 0
            else:
                assert False, "Unknown dwarf entry? %r" % entry.tag

            sizeof.cache[entry.off] = size
            return size

        # recursive+cached children finder
        def childrenof(entry, depth, seen=set()):
            # found a cycle? stop here
            if entry.off in seen:
                return [], {'cycle detected'}, True
            # stop here?
            if depth < 1:
                return [], set(), False
            # cached?
            if not hasattr(childrenof, 'cache'):
                childrenof.cache = {}
            if entry.off in childrenof.cache:
                return childrenof.cache[entry.off]

            # pointer? deref and include size
            if entry.tag == 'DW_TAG_pointer_type':
                children, notes, dirty = [], set(), False
                if 'DW_AT_type' in entry:
                    type = info[int(entry['DW_AT_type'].strip('<>'), 0)]
                    # skip modifiers to try to find name
                    while (type.name is None
                            and 'DW_AT_type' in type
                            and type.tag != 'DW_TAG_subroutine_type'):
                        type = info[int(type['DW_AT_type'].strip('<>'), 0)]
                    if (type.name is not None
                            and type.tag != 'DW_TAG_subroutine_type'):
                        # find size, etc
                        name_ = type.name
                        size_ = sizeof(type, seen | {entry.off})
                        children_, notes_, dirty_ = childrenof(
                                type, depth-1, seen | {entry.off})
                        children.append(CtxResult(
                                0, 0, file, name_, 0, size_,
                                children=children_,
                                notes=notes_))
                        dirty = dirty or dirty_
            # struct? union?
            elif entry.tag in {
                    'DW_TAG_structure_type',
                    'DW_TAG_union_type'}:
                # iterate over children in struct/union
                children, notes, dirty = [], set(), False
                for child in entry.children:
                    if child.tag != 'DW_TAG_member':
                        continue
                    # find name
                    name_ = child.name
                    # try to find offset for struct members, note this
                    # is _not_ the same as the dwarf entry offset
                    off_ = int(child.get('DW_AT_data_member_location', 0))
                    # find size, children, etc
                    size_ = sizeof(child, seen | {entry.off})
                    children_, notes_, dirty_ = childrenof(
                            child, depth-1, seen | {entry.off})
                    children.append(CtxResult(
                            0, len(children), file, name_, off_, size_,
                            children=children_,
                            notes=notes_))
                    dirty = dirty or dirty_
            # base type? function pointer?
            elif entry.tag in {
                    'DW_TAG_base_type',
                    'DW_TAG_subroutine_type'}:
                children, notes, dirty = [], set(), False
            # a modifier?
            elif (entry.tag in {
                        'DW_TAG_typedef',
                        'DW_TAG_array_type',
                        'DW_TAG_enumeration_type',
                        'DW_TAG_formal_parameter',
                        'DW_TAG_member',
                        'DW_TAG_const_type',
                        'DW_TAG_volatile_type',
                        'DW_TAG_restrict_type'}
                    and 'DW_AT_type' in entry):
                type = int(entry['DW_AT_type'].strip('<>'), 0)
                children, notes, dirty = childrenof(
                        info[type], depth, seen | {entry.off})
            # void?
            elif ('DW_AT_type' not in entry
                    and 'DW_AT_byte_size' not in entry):
                children, notes = [], set(), False
            else:
                assert False, "Unknown dwarf entry? %r" % entry.tag

            if not dirty:
                childrenof.cache[entry.off] = children, notes, dirty
            return children, notes, dirty

        # find each function's context
        for sym in syms:
            # discard internal functions
            if not everything and sym.name.startswith('__'):
                continue

            # find best matching dwarf entry
            entry = info.get(sym.name)

            # skip non-functions
            if entry is None or entry.tag != 'DW_TAG_subprogram':
                continue

            # find all parameters
            params = []
            for param in entry.children:
                if param.tag != 'DW_TAG_formal_parameter':
                    continue

                # find name, if there is one
                name_ = param.name if param.name is not None else '(unnamed)'

                # find size, recursing if necessary
                size_ = sizeof(param)

                # find children, recursing if necessary
                children_, notes_, _ = childrenof(param, depth-2)

                params.append(CtxResult(
                        0, len(params), file, name_, 0, size_,
                        children=children_,
                        notes=notes_))

            # strip compiler suffixes
            name = sym.name
            if not no_strip:
                name = name.split('.', 1)[0]

            # context = sum of params
            size = sum((param.size for param in params), start=CsvInt(0))

            results.append(CtxResult(
                    0, 0, file, name, 0, size,
                    children=params))

    # assign z at the end to avoid issues with caching
    def zed(results, z):
        return [r._replace(z=z, children=zed(r.children, z+1))
                for r in results]
    results = zed(results, 0)

    return results


# common folding/tabling/read/write code

class Rev(co.namedtuple('Rev', 'a')):
    __slots__ = ()
    # yes we need all of these because we're a namedtuple
    def __lt__(self, other):
        return self.a > other.a
    def __gt__(self, other):
        return self.a < other.a
    def __le__(self, other):
        return self.a >= other.a
    def __ge__(self, other):
        return self.a <= other.a

def fold(Result, results, *,
        by=None,
        defines=[],
        sort=None,
        depth=1,
        **_):
    # stop when depth hits zero
    if depth == 0:
        return []

    # organize by by
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
            if all(str(getattr(r, k)) in vs for k, vs in defines):
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

    # sort, note that python's sort is stable
    folded.sort(key=lambda r: (
            # sort by explicit sort fields
            tuple((Rev
                        if reverse ^ (not k or k in Result._fields)
                        else lambda x: x)(
                    tuple((getattr(r, k_),)
                            if getattr(r, k_) is not None
                            else ()
                        for k_ in ([k] if k else Result._sort)))
                for k, reverse in (sort or [])),
            # sort by result
            r))

    # recurse if we have recursive results
    if hasattr(Result, '_children'):
        folded = [r._replace(**{
                Result._children: fold(
                        Result, getattr(r, Result._children),
                        by=by,
                        # only filter defines at the top level!
                        sort=sort,
                        depth=depth-1)})
                    for r in folded]

    return folded

def hotify(Result, results, *,
        enumerates=None,
        depth=1,
        hot=None,
        **_):
    # note! hotifying risks confusion if you don't enumerate/have a
    # z field, since it will allow folding across recursive boundaries

    # hotify only makes sense for recursive results
    assert hasattr(Result, '_children')

    results_ = []
    for r in results:
        hot_ = []
        def recurse(results_, depth_):
            nonlocal hot_
            if not results_:
                return

            # find the hottest result
            r = min(results_, key=lambda r:
                    tuple((Rev
                                if reverse ^ (not k or k in Result._fields)
                                else lambda x: x)(
                            tuple((getattr(r, k_),)
                                    if getattr(r, k_) is not None
                                    else ()
                                for k_ in ([k] if k else Result._sort)))
                        for k, reverse in it.chain(hot, [(None, False)])))

            hot_.append(r._replace(**(
                    # enumerate?
                    ({e: len(hot_) for e in enumerates}
                            if enumerates is not None
                            else {})
                        | {Result._children: []})))

            # recurse?
            if depth_ > 1:
                recurse(getattr(r, Result._children),
                        depth_-1)

        recurse(getattr(r, Result._children), depth-1)
        results_.append(r._replace(**{Result._children: hot_}))

    return results_

def table(Result, results, diff_results=None, *,
        by=None,
        fields=None,
        sort=None,
        labels=None,
        depth=1,
        hot=None,
        percent=False,
        all=False,
        compare=None,
        no_header=False,
        small_header=False,
        no_total=False,
        small_table=False,
        summary=False,
        **_):
    import builtins
    all_, all = all, builtins.all

    if by is None:
        by = Result._by
    if fields is None:
        fields = Result._fields
    types = Result._types

    # organize by name
    table = {
            ','.join(str(getattr(r, k)
                        if getattr(r, k) is not None
                        else '')
                    for k in by): r
                for r in results}
    diff_table = {
            ','.join(str(getattr(r, k)
                        if getattr(r, k) is not None
                        else '')
                    for k in by): r
                for r in diff_results or []}

    # lost results? this only happens if we didn't fold by the same
    # by field, which is an error and risks confusing results
    assert len(table) == len(results)
    if diff_results is not None:
        assert len(diff_table) == len(diff_results)

    # find compare entry if there is one
    if compare:
        compare_r = table.get(','.join(str(k) for k in compare))

    # build up our lines
    lines = []

    # header
    if not no_header:
        header = ['%s%s' % (
                    ','.join(labels if labels is not None else by),
                    ' (%d added, %d removed)' % (
                            sum(1 for n in table if n not in diff_table),
                            sum(1 for n in diff_table if n not in table))
                        if diff_results is not None and not percent else '')
                if not small_header and not small_table and not summary
                    else '']
        if diff_results is None or percent:
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

    # delete these to try to catch typos below, we need to rebuild
    # these tables at each recursive layer
    del table
    del diff_table

    # entry helper
    def table_entry(name, r, diff_r=None):
        # prepend name
        entry = [name]

        # normal entry?
        if ((compare is None or r == compare_r)
                and diff_results is None):
            for k in fields:
                entry.append(
                        (getattr(r, k).table(),
                                getattr(getattr(r, k), 'notes', lambda: [])())
                            if getattr(r, k, None) is not None
                            else types[k].none)
        # compare entry?
        elif diff_results is None:
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
                                    getattr(compare_r, k, None)))))
        # percent entry?
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
            notes = sorted(getattr(r, Result._notes))
            if isinstance(entry[-1], tuple):
                entry[-1] = (entry[-1][0], entry[-1][1] + notes)
            else:
                entry[-1] = (entry[-1], notes)

        return entry

    # recursive entry helper
    def table_recurse(results_, diff_results_,
            depth_,
            prefixes=('', '', '', '')):
        # build the children table at each layer
        table_ = {
                ','.join(str(getattr(r, k)
                            if getattr(r, k) is not None
                            else '')
                        for k in by): r
                    for r in results_}
        diff_table_ = {
                ','.join(str(getattr(r, k)
                            if getattr(r, k) is not None
                            else '')
                        for k in by): r
                    for r in diff_results_ or []}
        names_ = [n
                for n in table_.keys() | diff_table_.keys()
                if diff_results is None
                    or all_
                    or any(
                        types[k].ratio(
                                getattr(table_.get(n), k, None),
                                getattr(diff_table_.get(n), k, None))
                            for k in fields)]

        # sort again, now with diff info, note that python's sort is stable
        names_.sort(key=lambda n: (
                # sort by explicit sort fields
                next(
                    tuple((Rev
                                    if reverse ^ (not k or k in Result._fields)
                                    else lambda x: x)(
                                tuple((getattr(r_, k_),)
                                        if getattr(r_, k_) is not None
                                        else ()
                                    for k_ in ([k] if k else Result._sort)))
                            for k, reverse in (sort or []))
                        for r_ in [table_.get(n), diff_table_.get(n)]
                        if r_ is not None),
                # sort by ratio if diffing
                Rev(tuple(types[k].ratio(
                            getattr(table_.get(n), k, None),
                            getattr(diff_table_.get(n), k, None))
                        for k in fields))
                    if diff_results is not None
                    else (),
                # move compare entry to the top, note this can be
                # overridden by explicitly sorting by fields
                (table_.get(n) != compare_r,
                        # sort by ratio if comparing
                        Rev(tuple(
                            types[k].ratio(
                                    getattr(table_.get(n), k, None),
                                    getattr(compare_r, k, None))
                                for k in fields)))
                    if compare
                    else (),
                # sort by result
                (table_[n],) if n in table_ else (),
                # and finally by name (diffs may be missing results)
                n))

        for i, name in enumerate(names_):
            # find comparable results
            r = table_.get(name)
            diff_r = diff_table_.get(name)

            # figure out a good label
            if labels is not None:
                label = next(
                        ','.join(str(getattr(r_, k)
                                    if getattr(r_, k) is not None
                                    else '')
                                for k in labels)
                            for r_ in [r, diff_r]
                            if r_ is not None)
            else:
                label = name

            # build line
            line = table_entry(label, r, diff_r)

            # add prefixes
            line = [x if isinstance(x, tuple) else (x, []) for x in line]
            line[0] = (prefixes[0+(i==len(names_)-1)] + line[0][0], line[0][1])
            lines.append(line)

            # recurse?
            if name in table_ and depth_ > 1:
                table_recurse(
                        getattr(r, Result._children),
                        getattr(diff_r, Result._children, None),
                        depth_-1,
                        (prefixes[2+(i==len(names_)-1)] + "|-> ",
                         prefixes[2+(i==len(names_)-1)] + "'-> ",
                         prefixes[2+(i==len(names_)-1)] + "|   ",
                         prefixes[2+(i==len(names_)-1)] + "    "))

    # build entries
    if not summary:
        table_recurse(results, diff_results, depth)

    # total
    if not no_total and not (small_table and not summary):
        r = next(iter(fold(Result, results, by=[])), None)
        if diff_results is None:
            diff_r = None
        else:
            diff_r = next(iter(fold(Result, diff_results, by=[])), None)
        lines.append(table_entry('TOTAL', r, diff_r))

    # homogenize
    lines = [[x if isinstance(x, tuple) else (x, []) for x in line]
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

def read_csv(path, Result, *,
        depth=1,
        prefix=None,
        **_):
    # prefix? this only applies to field fields
    if prefix is None:
        if hasattr(Result, '_prefix'):
            prefix = '%s_' % Result._prefix
        else:
            prefix = ''

    by = Result._by
    fields = Result._fields

    with openio(path, 'r') as f:
        # csv or json? assume json starts with [
        is_json = (f.buffer.peek(1)[:1] == b'[')

        # read csv?
        if not is_json:
            results = []
            reader = csv.DictReader(f, restval='')
            for r in reader:
                if not any(prefix+k in r and r[prefix+k].strip()
                        for k in fields):
                    continue
                try:
                    # note this allows by/fields to overlap
                    results.append(Result(**(
                            {k: r[k] for k in by
                                    if k in r
                                        and r[k].strip()}
                                | {k: r[prefix+k] for k in fields
                                    if prefix+k in r
                                        and r[prefix+k].strip()})))
                except TypeError:
                    pass
            return results

        # read json?
        else:
            import json
            def unjsonify(results, depth_):
                results_ = []
                for r in results:
                    if not any(prefix+k in r and r[prefix+k].strip()
                            for k in fields):
                        continue
                    try:
                        # note this allows by/fields to overlap
                        results_.append(Result(**(
                                {k: r[k] for k in by
                                        if k in r
                                            and r[k] is not None}
                                    | {k: r[prefix+k] for k in fields
                                        if prefix+k in r
                                            and r[prefix+k] is not None}
                                    | ({Result._children: unjsonify(
                                            r[Result._children],
                                            depth_-1)}
                                        if hasattr(Result, '_children')
                                            and Result._children in r
                                            and r[Result._children] is not None
                                            and depth_ > 1
                                        else {})
                                    | ({Result._notes: set(r[Result._notes])}
                                        if hasattr(Result, '_notes')
                                            and Result._notes in r
                                            and r[Result._notes] is not None
                                        else {}))))
                    except TypeError:
                        pass
                return results_
            return unjsonify(json.load(f), depth)

def write_csv(path, Result, results, *,
        json=False,
        by=None,
        fields=None,
        depth=1,
        prefix=None,
        **_):
    # prefix? this only applies to field fields
    if prefix is None:
        if hasattr(Result, '_prefix'):
            prefix = '%s_' % Result._prefix
        else:
            prefix = ''

    if by is None:
        by = Result._by
    if fields is None:
        fields = Result._fields

    with openio(path, 'w') as f:
        # write csv?
        if not json:
            writer = csv.DictWriter(f, list(
                    co.OrderedDict.fromkeys(it.chain(
                        by,
                        (prefix+k for k in fields))).keys()))
            writer.writeheader()
            for r in results:
                # note this allows by/fields to overlap
                writer.writerow(
                        {k: getattr(r, k)
                                for k in by
                                if getattr(r, k) is not None}
                            | {prefix+k: str(getattr(r, k))
                                for k in fields
                                if getattr(r, k) is not None})

        # write json?
        else:
            import json
            # the neat thing about json is we can include recursive results
            def jsonify(results, depth_):
                results_ = []
                for r in results:
                    # note this allows by/fields to overlap
                    results_.append(
                            {k: getattr(r, k)
                                    for k in by
                                    if getattr(r, k) is not None}
                                | {prefix+k: str(getattr(r, k))
                                    for k in fields
                                    if getattr(r, k) is not None}
                                | ({Result._children: jsonify(
                                        getattr(r, Result._children),
                                        depth_-1)}
                                    if hasattr(Result, '_children')
                                        and getattr(r, Result._children)
                                        and depth_ > 1
                                    else {})
                                | ({Result._notes: list(
                                        getattr(r, Result._notes))}
                                    if hasattr(Result, '_notes')
                                        and getattr(r, Result._notes)
                                    else {}))
                return results_
            json.dump(jsonify(results, depth), f,
                    separators=(',', ':'))


def main(obj_paths, *,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        depth=None,
        hot=None,
        **args):
    # figure out what fields we're interested in
    labels = None
    if by is None:
        if args.get('output') or args.get('output_json'):
            by = CtxResult._by
        elif depth is not None or hot is not None:
            by = ['z', 'i', 'function']
            labels = ['function']
        else:
            by = ['function']

    if fields is None:
        if args.get('output') or args.get('output_json'):
            fields = CtxResult._fields
        else:
            fields = ['size']

    # figure out depth
    if depth is None:
        depth = mt.inf if hot else 1
    elif depth == 0:
        depth = mt.inf

    # find sizes
    if not args.get('use', None):
        # not enough info?
        if not obj_paths:
            print("error: no *.o files?",
                    file=sys.stderr)
            sys.exit(1)

        # collect info
        results = collect_ctx(obj_paths,
                depth=depth,
                **args)

    else:
        results = read_csv(args['use'], CtxResult,
                depth=depth,
                **args)

    # fold
    results = fold(CtxResult, results,
            by=by,
            defines=defines,
            depth=depth)

    # hotify?
    if hot:
        results = hotify(CtxResult, results,
                depth=depth,
                hot=hot)

    # find previous results?
    diff_results = None
    if args.get('diff'):
        try:
            diff_results = read_csv(
                    args.get('diff'),
                    CtxResult,
                    depth=depth,
                    **args)
        except FileNotFoundError:
            diff_results = []

        # fold
        diff_results = fold(CtxResult, diff_results,
                by=by,
                defines=defines,
                depth=depth)

        # hotify?
        if hot:
            diff_results = hotify(CtxResult, diff_results,
                    depth=depth,
                    hot=hot)

    # write results to JSON
    if args.get('output_json'):
        write_csv(args['output_json'], CtxResult, results, json=True,
                by=by,
                fields=fields,
                depth=depth,
                **args)
    # write results to CSV
    elif args.get('output'):
        write_csv(args['output'], CtxResult, results,
                by=by,
                fields=fields,
                depth=depth,
                **args)
    # print table
    elif not args.get('quiet'):
        table(CtxResult, results, diff_results,
                by=by,
                fields=fields,
                sort=sort,
                labels=labels,
                depth=depth,
                **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Find the overhead of function contexts.",
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
            help="Don't show anything, useful when checking for errors.")
    parser.add_argument(
            '-o', '--output',
            help="Specify CSV file to store results.")
    parser.add_argument(
            '-O', '--output-json',
            help="Specify JSON file to store results. This may contain "
                "recursive info.")
    parser.add_argument(
            '-u', '--use',
            help="Don't parse anything, use this CSV/JSON file.")
    parser.add_argument(
            '-d', '--diff',
            help="Specify CSV/JSON file to diff against.")
    parser.add_argument(
            '-p', '--percent',
            action='store_true',
            help="Only show percentage change, not a full diff.")
    parser.add_argument(
            '-c', '--compare',
            type=lambda x: tuple(v.strip() for v in x.split(',')),
            help="Compare results to the row matching this by pattern.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Show all, not just the ones that changed.")
    parser.add_argument(
            '-b', '--by',
            action='append',
            choices=CtxResult._by,
            help="Group by this field.")
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action='append',
            choices=CtxResult._fields,
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
            namespace.sort.append((value, option in {'-S', '--reverse-sort'}))
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
            '-z', '--depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of function calls to show. 0 shows all calls unless "
                "we find a cycle. Defaults to 0.")
    class AppendHot(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.hot is None:
                namespace.hot = []
            namespace.hot.append((value, option in {'-R', '--reverse-hot'}))
    parser.add_argument(
            '-r', '--hot',
            nargs='?',
            action=AppendHot,
            help="Show only the hot path for each function call. Can "
                "optionally provide fields like sort.")
    parser.add_argument(
            '-R', '--reverse-hot',
            nargs='?',
            action=AppendHot,
            help="Like -r/--hot, but backwards.")
    parser.add_argument(
            '--no-header',
            action='store_true',
            help="Don't show the header.")
    parser.add_argument(
            '--small-header',
            action='store_true',
            help="Don't show by field names.")
    parser.add_argument(
            '--no-total',
            action='store_true',
            help="Don't show the total.")
    parser.add_argument(
            '-Q', '--small-table',
            action='store_true',
            help="Equivalent to --small-header + --no-total.")
    parser.add_argument(
            '-Y', '--summary',
            action='store_true',
            help="Only show the total.")
    parser.add_argument(
            '--prefix',
            help="Prefix to use for fields in CSV/JSON output. Defaults "
                "to %r." % ("%s_" % CtxResult._prefix))
    parser.add_argument(
            '-i', '--internal',
            action='store_true',
            help="Include internal symbols. Useful for introspection, but "
                "usually you don't care about these.")
    parser.add_argument(
            '-!', '--everything',
            action='store_true',
            help="Include builtin and libc specific symbols.")
    parser.add_argument(
            '-x', '--no-strip',
            action='store_true',
            help="Don't strip compiler optimization suffixes from symbols.")
    parser.add_argument(
            '--objdump-path',
            type=lambda x: x.split(),
            default=OBJDUMP_PATH,
            help="Path to the objdump executable, may include flags. "
                "Defaults to %r." % OBJDUMP_PATH)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
