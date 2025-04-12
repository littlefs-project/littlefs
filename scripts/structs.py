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
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import csv
import itertools as it
import io
import functools as ft
import math as mt
import os
import re
import shlex
import subprocess as sp
import sys


OBJDUMP_PATH = ['objdump']


# integer fields
class CsvInt(co.namedtuple('CsvInt', 'x')):
    __slots__ = ()
    def __new__(cls, x=0):
        if isinstance(x, CsvInt):
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
        'z', 'i', 'file', 'struct',
        'off', 'size', 'align',
        'children'])):
    _prefix = 'struct'
    _by = ['z', 'i', 'file', 'struct']
    _fields = ['off', 'size', 'align']
    _sort = ['size', 'align']
    _types = {'off': CsvInt, 'size': CsvInt, 'align': CsvInt}
    _children = 'children'

    __slots__ = ()
    def __new__(cls, z=0, i=0, file='', struct='', off=0, size=0, align=0,
            children=None):
        return super().__new__(cls, z, i, file, struct,
                CsvInt(off), CsvInt(size), CsvInt(align),
                children if children is not None else [])

    def __add__(self, other):
        return StructResult(self.z, self.i, self.file, self.struct,
                min(self.off, other.off),
                max(self.size, other.size),
                max(self.align, other.align),
                self.children + other.children)


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    import os
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

def collect_structs(obj_paths, *,
        internal=False,
        everything=False,
        depth=1,
        **args):
    results = []
    for obj_path in obj_paths:
        # find dwarf info
        info = collect_dwarf_info(obj_path, **args)

        # find related file info
        files = collect_dwarf_files(obj_path, **args)

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
        def childrenof(entry, depth):
            # stop here?
            if depth < 1:
                return []
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
                # iterate over children in struct/union
                children = []
                for child in entry.children:
                    if child.tag != 'DW_TAG_member':
                        continue
                    # find name
                    name_ = child.name
                    # try to find offset for struct members, note this
                    # is _not_ the same as the dwarf entry offset
                    off_ = int(child.get('DW_AT_data_member_location', 0))
                    # find size, align, children, etc
                    size_ = sizeof(child)
                    align_ = alignof(child)
                    children_ = childrenof(child, depth-1)
                    children.append(StructResult(
                            0, len(children), file, name_, off_, size_, align_,
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
                children = childrenof(info[type], depth)
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

            # find source file
            if 'DW_AT_decl_file' in entry:
                src = files.get(int(entry['DW_AT_decl_file']), '?')
                # discard internal/stdlib types
                if not everything and src.startswith('/usr/include'):
                    continue
                # limit to .h files unless explicitly requested
                if not everything and not internal and not src.endswith('.h'):
                    continue

            # find name
            name = entry.name

            # find the size of a type, recursing if necessary
            size = sizeof(entry)

            # find alignment, recursing if necessary
            align = alignof(entry)

            # find children, recursing if necessary
            children = childrenof(entry, depth-1)

            # typdefs exist in a separate namespace, so we need to track
            # these separately
            if entry.tag == 'DW_TAG_typedef':
                typedefs[entry.off] = StructResult(
                        0, 0, file, name, 0, size, align,
                        children=children)
                typedefed.add(int(entry['DW_AT_type'].strip('<>'), 0))
            else:
                types[entry.off] = StructResult(
                        0, 0, file, name, 0, size, align,
                        children=children)

        # let typedefs take priority
        results.extend(typedefs.values())
        results.extend(type
                for off, type in types.items()
                if off not in typedefed)

    # assign z at the end to avoid issues with caching
    def zed(results, z):
        return [r._replace(z=z, children=zed(r.children, z+1))
                for r in results]
    results = zed(results, 0)

    return results


# common folding/tabling/read/write code

class Rev(co.namedtuple('Rev', 'x')):
    __slots__ = ()
    # yes we need all of these because we're a namedtuple
    def __lt__(self, other):
        return self.x > other.x
    def __gt__(self, other):
        return self.x < other.x
    def __le__(self, other):
        return self.x >= other.x
    def __ge__(self, other):
        return self.x <= other.x

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
    all_ = all; del all

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
            by = StructResult._by
        elif depth is not None or hot is not None:
            by = ['z', 'i', 'struct']
            labels = ['struct']
        else:
            by = ['struct']

    if fields is None:
        if args.get('output') or args.get('output_json'):
            fields = StructResult._fields
        else:
            fields = ['size', 'align']

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
        results = collect_structs(obj_paths,
                depth=depth,
                **args)

    else:
        results = read_csv(args['use'], StructResult,
                depth=depth,
                **args)

    # fold
    results = fold(StructResult, results,
            by=by,
            defines=defines,
            depth=depth)

    # hotify?
    if hot:
        results = hotify(StructResult, results,
                depth=depth,
                hot=hot)

    # find previous results?
    diff_results = None
    if args.get('diff'):
        try:
            diff_results = read_csv(
                    args.get('diff'),
                    StructResult,
                    depth=depth,
                    **args)
        except FileNotFoundError:
            diff_results = []

        # fold
        diff_results = fold(StructResult, diff_results,
                by=by,
                defines=defines,
                depth=depth)

        # hotify?
        if hot:
            diff_results = hotify(StructResult, diff_results,
                    depth=depth,
                    hot=hot)

    # write results to JSON
    if args.get('output_json'):
        write_csv(args['output_json'], StructResult, results, json=True,
                by=by,
                fields=fields,
                depth=depth,
                **args)
    # write results to CSV
    elif args.get('output'):
        write_csv(args['output'], StructResult, results,
                by=by,
                fields=fields,
                depth=depth,
                **args)
    # print table
    elif not args.get('quiet'):
        table(StructResult, results, diff_results,
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
            help="Don't parse anything, use this CSV file.")
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
                "to %r." % ("%s_" % StructResult._prefix))
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
            '--objdump-path',
            type=lambda x: x.split(),
            default=OBJDUMP_PATH,
            help="Path to the objdump executable, may include flags. "
                "Defaults to %r." % OBJDUMP_PATH)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
