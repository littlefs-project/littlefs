#!/usr/bin/env python3
#
# Script to find struct sizes.
#
# Example:
# ./scripts/structs.py lfs.o lfs_util.o -Ssize
#
# Copyright (c) 2022, The littlefs authors.
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

# struct size results
class StructResult(co.namedtuple('StructResult', [
        'file', 'struct',
        'size', 'align',
        'i', 'children'])):
    _by = ['file', 'struct']
    _fields = ['size', 'align']
    _sort = ['size', 'align']
    _types = {'size': RInt, 'align': RInt}
    _i = 'i'
    _children = 'children'

    __slots__ = ()
    def __new__(cls, file='', struct='', size=0, align=0,
            i=None, children=None):
        return super().__new__(cls, file, struct,
                RInt(size), RInt(align),
                i,
                children if children is not None else [])

    def __add__(self, other):
        return StructResult(self.file, self.struct,
                self.size + other.size,
                max(self.align, other.align),
                self.i if other.i is None
                        else other.i if self.i is None
                        else min(self.i, other.i),
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
            # prefix with struct/union
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
        # find source paths
        files = collect_dwarf_files(obj_path, **args)

        # find dwarf info
        info = collect_dwarf_info(obj_path, **args)

        # recursive+cached size finder
        def sizeof(entry):
            # cached?
            if not hasattr(sizeof, 'cache'):
                sizeof.cache = {}
            if entry.off in sizeof.cache:
                return sizeof.cache[entry.off]

            # explicit size?
            if 'DW_AT_byte_size' in entry:
                size = int(entry['DW_AT_byte_size'])
            # array? multiply by size
            elif entry.tag == 'DW_TAG_array_type':
                type = info[int(entry['DW_AT_type'].strip('<>'), 0)]
                size = sizeof(type)
                for child in entry.children:
                    if child.tag == 'DW_TAG_subrange_type':
                        size *= int(child['DW_AT_upper_bound']) + 1
            # indirect type?
            elif entry.tag in {
                    'DW_TAG_typedef',
                    'DW_TAG_enumeration_type',
                    'DW_TAG_member',
                    'DW_TAG_const_type',
                    'DW_TAG_volatile_type',
                    'DW_TAG_restrict_type'}:
                type = info[int(entry['DW_AT_type'].strip('<>'), 0)]
                size = sizeof(type)
            else:
                assert False, "Unknown dwarf entry? %r" % entry.tag

            sizeof.cache[entry.off] = size
            return size

        # recursive+cached alignment finder
        #
        # Dwarf doesn't seem to give us this info, so we infer it from
        # the size of children pointer/base types. This is _usually_
        # correct.
        def alignof(entry):
            # cached?
            if not hasattr(alignof, 'cache'):
                alignof.cache = {}
            if entry.off in alignof.cache:
                return alignof.cache[entry.off]

            # pointer? base type? assume this size == alignment
            if entry.tag in {
                    'DW_TAG_pointer_type',
                    'DW_TAG_base_type'}:
                align = int(entry['DW_AT_byte_size'])
            # struct? union? take max alignment of children
            elif entry.tag in {
                    'DW_TAG_structure_type',
                    'DW_TAG_union_type'}:
                align = max(alignof(child) for child in entry.children)
            # indirect type?
            elif entry.tag in {
                    'DW_TAG_typedef',
                    'DW_TAG_array_type',
                    'DW_TAG_enumeration_type',
                    'DW_TAG_member',
                    'DW_TAG_const_type',
                    'DW_TAG_volatile_type',
                    'DW_TAG_restrict_type'}:
                type = int(entry['DW_AT_type'].strip('<>'), 0)
                align = alignof(info[type])
            else:
                assert False, "Unknown dwarf entry? %r" % entry.tag

            alignof.cache[entry.off] = align
            return align

        # recursive+cached children finder
        def childrenof(entry):
            # cached?
            if not hasattr(childrenof, 'cache'):
                childrenof.cache = {}
            if entry.off in childrenof.cache:
                return childrenof.cache[entry.off]

            # pointer? base type?
            if entry.tag in {
                    'DW_TAG_pointer_type',
                    'DW_TAG_base_type'}:
                children = []
            # struct? union?
            elif entry.tag in {
                    'DW_TAG_structure_type',
                    'DW_TAG_union_type'}:
                children = []
                for child in entry.children:
                    name_ = child.name
                    size_ = sizeof(child)
                    align_ = alignof(child)
                    children_ = childrenof(child)
                    children.append(StructResult(
                            file, name_, size_, align_,
                            i=child.off,
                            children=children_))
            # indirect type?
            elif entry.tag in {
                    'DW_TAG_typedef',
                    'DW_TAG_array_type',
                    'DW_TAG_enumeration_type',
                    'DW_TAG_member',
                    'DW_TAG_const_type',
                    'DW_TAG_volatile_type',
                    'DW_TAG_restrict_type'}:
                type = int(entry['DW_AT_type'].strip('<>'), 0)
                children = childrenof(info[type])
            else:
                assert False, "Unknown dwarf entry? %r" % entry.tag

            childrenof.cache[entry.off] = children
            return children

        # collect structs and other types
        typedefs = co.OrderedDict()
        typedefed = set()
        types = co.OrderedDict()
        for entry in info:
            # skip non-types and types with no name
            if (entry.tag not in {
                        'DW_TAG_typedef',
                        'DW_TAG_structure_type',
                        'DW_TAG_union_type',
                        'DW_TAG_enumeration_type'}
                    or entry.name is None):
                continue

            # discard internal types
            if not everything and entry.name.startswith('__'):
                continue

            # if we have no file guess from obj path
            if 'DW_AT_decl_file' in entry:
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
                if (not everything and not os.path.commonpath([
                        os.getcwd(),
                        os.path.abspath(file)]) == os.getcwd()):
                    continue

            # find name
            name = entry.name

            # find the size of a type, recursing if necessary
            size = sizeof(entry)

            # find alignment, recursing if necessary
            align = alignof(entry)

            # find children, recursing if necessary
            children = childrenof(entry)

            # typdefs exist in a separate namespace, so we need to track
            # these separately
            if entry.tag == 'DW_TAG_typedef':
                typedefs[entry.off] = StructResult(
                        file, name, size, align,
                        children=children)
                typedefed.add(int(entry['DW_AT_type'].strip('<>'), 0))
            else:
                types[entry.off] = StructResult(
                        file, name, size, align,
                        children=children)

        # let typedefs take priority
        results.extend(typedefs.values())
        results.extend(type
                for off, type in types.items()
                if off not in typedefed)

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
    # figure out depth
    if args.get('depth') is None:
        args['depth'] = mt.inf if args.get('hot') else 1
    elif args.get('depth') == 0:
        args['depth'] = mt.inf

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

                if not any(k in r and r[k].strip()
                        for k in StructResult._fields):
                    continue
                try:
                    results.append(StructResult(
                            **{k: r[k] for k in StructResult._by
                                if k in r and r[k].strip()},
                            **{k: r[k]
                                for k in StructResult._fields
                                if k in r and r[k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(StructResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(
                    key=lambda r: tuple(
                        (getattr(r, k),) if getattr(r, k) is not None else ()
                            for k in ([k] if k else StructResult._sort)),
                    reverse=reverse ^ (not k or k in StructResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f,
                    (by if by is not None else StructResult._by)
                        + [k for k in (
                            fields if fields is not None
                                else StructResult._fields)])
            writer.writeheader()
            for r in results:
                writer.writerow(
                        {k: getattr(r, k) for k in (
                                by if by is not None else StructResult._by)}
                            | {k: getattr(r, k) for k in (
                                fields if fields is not None
                                    else StructResult._fields)})

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
                            for k in StructResult._fields):
                        continue
                    try:
                        diff_results.append(StructResult(
                                **{k: r[k] for k in StructResult._by
                                    if k in r and r[k].strip()},
                                **{k: r[k]
                                    for k in StructResult._fields
                                    if k in r and r[k].strip()}))
                    except TypeError:
                        pass
        except FileNotFoundError:
            pass

        # fold
        diff_results = fold(StructResult, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        table(StructResult, results, diff_results,
                by=by if by is not None else ['struct'],
                fields=fields,
                sort=sort,
                detect_cycles=False,
                **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Find struct sizes.",
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
            choices=StructResult._by,
            help="Group by this field.")
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action='append',
            choices=StructResult._fields,
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
            '--objdump-path',
            type=lambda x: x.split(),
            default=OBJDUMP_PATH,
            help="Path to the objdump executable, may include flags. "
                "Defaults to %r." % OBJDUMP_PATH)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
