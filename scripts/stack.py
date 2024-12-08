#!/usr/bin/env python3
#
# Script to find stack usage at the function level. Will detect recursion and
# report as infinite stack usage.
#
# Example:
# ./scripts/stack.py lfs.ci lfs_util.ci -Slimit
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
import subprocess as sp
import bisect


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

# size results
class StackResult(co.namedtuple('StackResult', [
        'file', 'function',
        'frame', 'limit',
        'children', 'notes'])):
    _by = ['file', 'function']
    _fields = ['frame', 'limit']
    _sort = ['limit', 'frame']
    _types = {'frame': RInt, 'limit': RInt}
    _children = 'children'
    _notes = 'notes'

    __slots__ = ()
    def __new__(cls, file='', function='', frame=0, limit=0,
            children=None, notes=None):
        return super().__new__(cls, file, function,
                RInt(frame), RInt(limit),
                children if children is not None else [],
                notes if notes is not None else [])

    def __add__(self, other):
        return StackResult(self.file, self.function,
                self.frame + other.frame,
                max(self.limit, other.limit),
                self.children + other.children,
                list(co.OrderedDict.fromkeys(it.chain(
                        self.notes, other.notes)).keys()))


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

    @ft.cached_property
    def addr(self):
        if (self.tag == 'DW_TAG_subprogram'
                and 'DW_AT_low_pc' in self):
            return int(self['DW_AT_low_pc'].split(':')[-1], 16)
        else:
            return None

    @ft.cached_property
    def size(self):
        if (self.tag == 'DW_TAG_subprogram'
                and 'DW_AT_high_pc' in self):
            # this looks wrong, but high_pc does store the size,
            # for whatever reason
            return int(self['DW_AT_high_pc'].split(':')[-1], 16)
        else:
            return None

    def info(self, tags=None):
        # recursively flatten children
        def flatten(entry):
            for child in entry.children:
                # filter if requested
                if tags is None or child.tag in tags:
                    yield child

                yield from flatten(child)

        return DwarfInfo(co.OrderedDict(
                (child.off, child) for child in flatten(self)))

# a collection of dwarf entries
class DwarfInfo:
    def __init__(self, entries):
        self.entries = entries

    def get(self, k, d=None):
        # allow lookup by offset, symbol, or dwarf name
        if not isinstance(k, str) and not hasattr(k, 'addr'):
            return self.entries.get(k, d)

        elif hasattr(k, 'addr'):
            import bisect

            # organize by address
            if not hasattr(self, '_by_addr'):
                # sort and keep largest/first when duplicates
                entries = [entry
                        for entry in self.entries.values()
                        if entry.addr is not None
                            and entry.size is not None]
                entries.sort(key=lambda x: (x.addr, -x.size))

                by_addr = []
                for entry in entries:
                    if (len(by_addr) == 0
                            or by_addr[-1].addr != entry.addr):
                        by_addr.append(entry)
                self._by_addr = by_addr

            # find entry by range
            i = bisect.bisect(self._by_addr, k.addr,
                    key=lambda x: x.addr)
            # check that we're actually in this entry's size
            if (i > 0
                    and k.addr
                        < self._by_addr[i-1].addr
                            + self._by_addr[i-1].size):
                return self._by_addr[i-1]
            else:
                # fallback to lookup by name
                return self.get(k.name, d)

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
                    i = entry.name.find(k)
                    if i == -1:
                        return None
                    return (i, len(entry.name)-(i+len(k)), entry.name)
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

    # resolve abstract origins
    for entry in info.values():
        if 'DW_AT_abstract_origin' in entry:
            off = int(entry['DW_AT_abstract_origin'].strip('<>'), 0)
            origin = info[off]
            assert 'DW_AT_abstract_origin' not in origin, (
                    "Recursive abstract origin?")

            for k, v in origin.ats.items():
                if k not in entry.ats:
                    entry.ats[k] = v

    return DwarfInfo(info)

class Frame(co.namedtuple('Frame', ['addr', 'frame'])):
    __slots__ = ()
    def __new__(cls, addr, frame):
        return super().__new__(cls, addr, frame)

    def __repr__(self):
        return '%s(0x%x, %d)' % (
                self.__class__.__name__,
                self.addr,
                self.frame)

class FrameInfo:
    def __init__(self, frames):
        self.frames = frames

    def get(self, k, d=None):
        import bisect

        # organize by address
        if not hasattr(self, '_by_addr'):
            # sort and keep largest when duplicates
            frames = self.frames.copy()
            frames.sort(key=lambda x: (x.addr, -x.frame))

            by_addr = []
            for frame in frames:
                if (len(by_addr) == 0
                        or by_addr[-1].addr != frame.addr):
                    by_addr.append(frame)
            self._by_addr = by_addr

        # allow lookup by addr or range of addrs
        if not isinstance(k, slice):
            # find frame by addr
            i = bisect.bisect(self._by_addr, k,
                    key=lambda x: x.addr)
            if i > 0:
                return self._by_addr[i-1]
            else:
                return d

        else:
            # find frame by range
            if k.start is None:
                start = 0
            else:
                start = max(
                        bisect.bisect(self._by_addr, k.start,
                            key=lambda x: x.addr) - 1,
                        0)
            if k.stop is None:
                stop = len(self._by_addr)
            else:
                stop = bisect.bisect(self._by_addr, k.stop,
                        key=lambda x: x.addr)

            return FrameInfo(self._by_addr[start:stop])

    def __getitem__(self, k):
        v = self.get(k)
        if v is None:
            raise KeyError(k)
        return v

    def __contains__(self, k):
        return self.get(k) is not None

    def __len__(self):
        return len(self.frames)

    def __iter__(self):
        return iter(self.frames)

def collect_dwarf_frames(obj_path, tags=None, *,
        objdump_path=OBJDUMP_PATH,
        **args):
    frame_pattern = re.compile(
            '^\s*(?P<cie_off>[0-9a-fA-F]+)'
                    '\s+(?P<cie_size>[0-9a-fA-F]+)'
                    '\s+(?P<cie_id>[0-9a-fA-F]+)'
                    '\s+CIE\s*$'
                '|' '^\s*(?P<fde_off>[0-9a-fA-F]+)'
                    '\s+(?P<fde_size>[0-9a-fA-F]+)'
                    '\s+(?P<fde_id>[0-9a-fA-F]+)'
                    '\s+FDE'
                    '\s+cie=(?P<fde_cie>[0-9a-fA-F]+)'
                    '\s+pc=(?P<fde_pc_lo>[0-9a-fA-F]+)'
                        '\.\.(?P<fde_pc_hi>[0-9a-fA-F]+)\s*$'
                '|' '^\s*(?P<op>DW_CFA_[^\s:]*)\s*:?'
                    '\s*(?P<change>.*?)\s*$')

    # collect frame info
    #
    # Frame info is encoded in a state machine stored in fde/cie
    # entries. fde entries can share cie entries, otherwise they are
    # mostly the same.
    #
    cies = co.OrderedDict()
    fdes = co.OrderedDict()
    entry = None
    # note objdump-path may contain extra args
    cmd = objdump_path + ['--dwarf=frames', obj_path]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    for line in proc.stdout:
        # state machine here to find fde/cie entries
        m = frame_pattern.match(line)
        if m:
            # start cie?
            if m.group('cie_off'):
                entry = {
                        'type': 'cie',
                        'off': int(m.group('cie_off'), 16),
                        'ops': []}
                cies[entry['off']] = entry

            # start fde?
            elif m.group('fde_off'):
                entry = {
                        'type': 'fde',
                        'off': int(m.group('fde_off'), 16),
                        'cie': int(m.group('fde_cie'), 16),
                        'pc': (
                            int(m.group('fde_pc_lo'), 16),
                            int(m.group('fde_pc_hi'), 16)),
                        'ops': []}
                fdes[entry['off']] = entry

            # found op?
            elif m.group('op'):
                entry['ops'].append((m.group('op'), m.group('change')))

            else:
                assert False
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    # execute the state machine
    frames = []
    for _, fde in fdes.items():
        cie = cies[fde['cie']]

        cfa_loc = fde['pc'][0]
        cfa_stack = []
        for op, change in it.chain(cie['ops'], fde['ops']):
            # advance location
            if op in {
                    'DW_CFA_advance_loc',
                    'DW_CFA_advance_loc1',
                    'DW_CFA_advance_loc2',
                    'DW_CFA_advance_loc4'}:
                cfa_loc = int(change.split('to')[-1], 16)
            # change cfa offset
            elif op in {
                    'DW_CFA_def_cfa',
                    'DW_CFA_def_cfa_offset'}:
                cfa_off = int(change.split('ofs')[-1], 0)
                frames.append(Frame(cfa_loc, cfa_off))
            # push state, because of course we need a stack
            elif op == 'DW_CFA_remember_state':
                cfa_stack.append(cfa_off)
            # pop state
            elif op == 'DW_CFA_restore_state':
                cfa_off = cfa_stack.pop()
            # ignore these
            elif op in {
                    'DW_CFA_nop',
                    'DW_CFA_offset',
                    'DW_CFA_restore',
                    'DW_CFA_def_cfa_register'}:
                pass
            else:
                assert False, "Unknown frame op? %r" % op

    return FrameInfo(frames)

class Loc(co.namedtuple('Loc', ['addr', 'size', 'ops'])):
    __slots__ = ()
    def __new__(cls, addr, size, ops):
        return super().__new__(cls, addr, size, ops)

    def __repr__(self):
        return '%s(0x%x, 0x%x, %r)' % (
                self.__class__.__name__,
                self.addr,
                self.size,
                self.ops)

class LocList:
    def __init__(self, off, locs):
        self.off = off
        self.locs = locs

    def get(self, k, d=None):
        import bisect

        # organize by address
        if not hasattr(self, '_by_addr'):
            # sort and keep largest/first when duplicates
            locs = self.locs.copy()
            locs.sort(key=lambda x: (x.addr, -x.size))

            by_addr = []
            for loc in locs:
                if (len(by_addr) == 0
                        or by_addr[-1].addr != loc.addr):
                    by_addr.append(loc)
            self._by_addr = by_addr

        # find loc by range
        i = bisect.bisect(self._by_addr, k,
                key=lambda x: x.addr)
        # check that we're actually in this loc's size
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
        return len(self.locs)

    def __iter__(self):
        return iter(self.locs)

def collect_dwarf_locs(obj_path, tags=None, *,
        objdump_path=OBJDUMP_PATH,
        **args):
    loc_pattern = re.compile(
            '^\s*(?P<begin_off>[0-9a-fA-F]+)'
                    '\s+(?P<begin_start>v?[0-9a-fA-F]+)'
                    '\s+(?P<begin_stop>v?[0-9a-fA-F]+)'
                    '\s+views.*$'
                '|' '^\s*(?P<end_off>[0-9a-fA-F]+)'
                    '\s+<End of list>\s*$'
                '|' '^\s*(?P<loc_start>[0-9a-fA-F]+)'
                    '\s+(?P<loc_stop>[0-9a-fA-F]+)'
                    '\s+\((?P<loc_ops>.+)\)\s*$',
            re.IGNORECASE)

    # collect location lists
    locs = co.OrderedDict()
    list_off = None
    list_locs = None
    # note objdump-path may contain extra args
    cmd = objdump_path + ['--dwarf=loc', obj_path]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    for line in proc.stdout:
        # find localtion lists
        m = loc_pattern.match(line)
        if m:
            # start of list?
            if m.group('begin_off'):
                # these occur between every entry, so ignore after
                # the first one
                if list_off is None:
                    list_off = int(m.group('begin_off'), 16)
                    list_locs = []
            # end of list?
            elif m.group('end_off'):
                assert list_off is not None
                locs[list_off] = LocList(list_off, list_locs)
                list_off = None
                list_locs = None
            # found a loc?
            elif m.group('loc_start'):
                assert list_off is not None
                start = int(m.group('loc_start'), 16)
                stop = int(m.group('loc_stop'), 16)
                ops = [op.strip() for op in m.group('loc_ops').split(';')]
                list_locs.append(Loc(start, stop-start, ops))
            else:
                assert False
    proc.wait()
    if proc.returncode != 0:
        raise sp.CalledProcessError(proc.returncode, proc.args)

    return locs

# we basically need a small linker here
class Func(co.namedtuple('Func', ['file', 'sym', 'entry',
        'frames', 'calls'])):
    __slots__ = ()
    def __new__(cls, file, sym, entry, frames=None, calls=None):
        return super().__new__(cls, file, sym, entry,
                frames if frames is not None else FuncFrameInfo(),
                calls if calls is not None else co.OrderedDict())

    def __repr__(self):
        return '<%s %s>' % (
                self.__class__.__name__,
                self.sym.name)

class FuncFrame(co.namedtuple('FuncFrame', ['addr', 'size', 'frame'])):
    __slots__ = ()
    def __new__(cls, addr, size, frame):
        return super().__new__(cls, addr, size, frame)

    def __repr__(self):
        return '%s(0x%x, 0x%x, %d)' % (
                self.__class__.__name__,
                self.addr,
                self.size,
                self.frame)

class FuncFrameInfo:
    def __init__(self, frames=None):
        self.frames = frames if frames is not None else []

    def get(self, k, d=None):
        # find frame by address
        i = bisect.bisect(self.frames, k,
                key=lambda x: x.addr)
        # check that we're actually in this frame's size
        if i > 0 and k < self.frames[i-1].addr+self.frames[i-1].size:
            return self.frames[i-1]
        else:
            return d

    def set(self, k, v):
        # always operate on ranges
        if not isinstance(k, slice):
            k = slice(k, 1)

        # insert frame, merging frames to find max frames
        frames_ = []
        for f in it.chain(
                (f for f in self.frames if f.addr < k.start),
                [FuncFrame(k.start, k.stop-k.start, v)],
                (f for f in self.frames if f.addr >= k.start)):
            g = frames_[-1] if frames_ else None

            # new frame?
            if g is None or f.addr > g.addr+g.size:
                frames_.append(f)
            # merge with previous frame?
            elif f.frame == g.frame:
                frames_[-1] = FuncFrame(
                        g.addr,
                        max(g.size, (f.addr+f.size) - g.addr),
                        g.frame)
            # previous frame wins?
            elif f.frame < g.frame:
                # slice new frame?
                if (f.addr+f.size > g.addr+g.size):
                    frames_.append(FuncFrame(
                            g.addr+g.size,
                            f.addr+f.size - (g.addr+g.size),
                            f.frame))
            # new frame wins?
            elif f.frame > g.frame:
                # slice previous frame
                frames_[-1] = FuncFrame(
                        g.addr,
                        f.addr - g.addr,
                        g.frame)
                # append new frame
                frames_.append(f)
                # slice previous frame tail?
                if (f.addr+f.size < g.addr+g.size):
                    frames_.append(FuncFrame(
                            f.addr+f.size,
                            (g.addr+g.size) - (f.addr+f.size),
                            g.frame))

        self.frames = frames_

    def __getitem__(self, k):
        v = self.get(k)
        if v is None:
            raise KeyError(k)
        return v

    def __contains__(self, k):
        return self.get(k) is not None

    def __setitem__(self, k, v):
        return self.set(k, v)

    def __len__(self):
        return len(self.frames)

    def __iter__(self):
        return iter(self.frames)

def collect(obj_paths, *,
        sources=None,
        everything=False,
        **args):
    funcs = []
    globals = co.OrderedDict()
    incomplete = False
    for obj_path in obj_paths:
        # find relevant symbols
        syms = collect_syms(obj_path,
                sections=['.text'],
                **args)

        # find source paths
        files = collect_dwarf_files(obj_path, **args)

        # find dwarf info, we only care about functions
        info = collect_dwarf_info(obj_path,
                tags={'DW_TAG_subprogram'},
                **args)

        # find frame info
        frames = collect_dwarf_frames(obj_path, **args)

        # find location info
        locs = collect_dwarf_locs(obj_path, **args)

        # find the max stack frame for each function
        locals = co.OrderedDict()
        for sym in syms:
            # discard internal functions
            if not everything and sym.name.startswith('__'):
                continue

            # find best matching dwarf entry, this may have a slightly
            # different name due to optimizations
            entry = info.get(sym)

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

            # build our func
            func = Func(file, sym, entry)

            # find the relevant stack frames
            if entry is not None:
                # base frame
                func.frames[sym.addr:sym.addr+sym.size] = max(
                        (frame.frame
                            for frame in frames[sym.addr:sym.addr+sym.size]),
                        default=0)

                for var in entry.info():
                    # find stack usage of relevant variables
                    if var.tag in {
                            'DW_TAG_variable',
                            'DW_TAG_formal_parameter',
                            'DW_TAG_call_site_parameter'}:
                        # ignore vars with no location, these are usually
                        # globals or synthetic variables
                        if 'DW_AT_location' not in var:
                            continue

                        m = re.match(
                                '^\s*(?P<list>[0xX0-9a-fA-F]+)'
                                        '\s*\(.*\)\s*$'
                                    '|' '^.*?\((?P<ops>.*)\)\s*$',
                                var['DW_AT_location'])
                        if m.group('ops'):
                            # TODO use range of lexical_block
                            locs_ = [Loc(sym.addr, sym.size,
                                    [m.group('ops').strip()])]
                        elif m.group('list'):
                            locs_ = locs[int(m.group('list'), 0)]
                        else:
                            assert False, "Unknown loc? %r" % (
                                    var['DW_AT_location'])

                        for loc in locs_:
                            frame = frames[loc.addr]
                            for op in loc.ops:
                                if op.startswith('DW_OP_fbreg'):
                                    off = int(op.split(':')[-1].strip(), 0)
                                    func.frames[loc.addr:loc.addr+loc.size] = (
                                            frame.frame - off)
                    # ignore these
                    elif var.tag in {
                            'DW_TAG_lexical_block',
                            'DW_TAG_inlined_subroutine',
                            'DW_TAG_call_site',
                            'DW_TAG_label',
                            'DW_TAG_structure_type',
                            'DW_TAG_union_type',
                            'DW_TAG_member'}:
                        pass
                    else:
                        assert False, "Unknown frame tag? %r" % var.tag

            # keep track of funcs
            funcs.append(func)

            # keep track of locals/globals
            if sym.global_:
                globals[sym.name] = func
            if entry is not None:
                locals[entry.off] = func

        # link local function calls via dwarf entries
        for func in locals.values():
            if not func.entry:
                continue

            if ((args.get('warn_on_incomplete')
                        or args.get('error_on_incomplete'))
                    and 'DW_AT_call_all_calls' not in func.entry):
                print('%s: incomplete call info in %s '
                        '(DW_AT_call_all_calls missing)' % (
                            'error' if args.get('error_on_incomplete')
                                else 'warning',
                            func.sym.name),
                        file=sys.stderr)
                incomplete = True

            for call in func.entry.info(
                    tags={'DW_TAG_call_site'}):
                if ('DW_AT_call_return_pc' not in call
                        or 'DW_AT_call_origin' not in call):
                    continue

                # note DW_AT_call_return_pc refers to the address
                # _after_ the call
                #
                # we change this to the last byte in the call
                # instruction, which is a bit weird, but should at least
                # map to the right stack frame
                addr = (int(call['DW_AT_call_return_pc'].split(':')[-1], 16)
                        - 1)
                off = int(call['DW_AT_call_origin'].strip('<>'), 0)

                # callee in locals?
                if off in locals:
                    func.calls[addr] = locals[off]
                else:
                    # if not, just keep track of the symbol and try to link
                    # during the global pass
                    func.calls[addr] = info[off].name

    # error on incomplete calls after printing all relevant functions
    if args.get('error_on_incomplete') and incomplete:
        sys.exit(3)

    # link global function calls via symbol
    for func in funcs:
        for addr, callee in func.calls.copy().items():
            if isinstance(callee, str):
                if callee in globals:
                    func.calls[addr] = globals[callee]
                else:
                    del func.calls[addr]

    # recursive+cached limit finder
    def limitof(func, seen=set()):
        # found a cycle? stop here
        if id(func) in seen:
            return 0, mt.inf
        # cached?
        if not hasattr(limitof, 'cache'):
            limitof.cache = {}
        if id(func) in limitof.cache:
            return limitof.cache[id(func)]

        # find max stack frame
        frame = max((frame.frame for frame in func.frames), default=0)

        # find stack limit recursively
        limit = frame
        for addr, callee in func.calls.items():
            if args.get('no_shrinkwrap'):
                frame_ = frame
            else:
                # use stack frame at call site
                frame_ = func.frames.get(addr)
                if frame_ is not None:
                    frame_ = frame_.frame
                else:
                    frame_ = 0

            _, limit_ = limitof(callee, seen | {id(func)})

            limit = max(limit, frame_ + limit_)

        limitof.cache[id(func)] = frame, limit
        return frame, limit

    # recursive+cached children finder
    def childrenof(func, seen=set()):
        # found a cycle? stop here
        if id(func) in seen:
            return [], ['cycle detected'], True
        # cached?
        if not hasattr(childrenof, 'cache'):
            childrenof.cache = {}
        if id(func) in childrenof.cache:
            return childrenof.cache[id(func)]

        # find children recursively
        children = []
        dirty = False
        for addr, callee in func.calls.items():
            file_ = callee.file
            name_ = callee.sym.name
            frame_, limit_ = limitof(callee, seen | {id(func)})
            children_, notes_, dirty_ = childrenof(callee, seen | {id(func)})
            dirty = dirty or dirty_
            children.append(StackResult(file_, name_, frame_, limit_,
                    children=children_,
                    notes=notes_))

        if not dirty:
            childrenof.cache[id(func)] = children, [], dirty
        return children, [], dirty

    # build results
    results = []
    for func in funcs:
        file = func.file
        name = func.sym.name
        frame, limit = limitof(func)
        children, notes, _ = childrenof(func)

        results.append(StackResult(file, name, frame, limit,
                children=children,
                notes=notes))

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


def main(obj_paths,
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
                        for k in StackResult._fields):
                    continue
                try:
                    results.append(StackResult(
                            **{k: r[k] for k in StackResult._by
                                if k in r and r[k].strip()},
                            **{k: r[k] for k in StackResult._fields
                                if k in r and r[k].strip()}))
                except TypeError:
                    pass

    # fold
    results = fold(StackResult, results, by=by, defines=defines)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(
                    key=lambda r: tuple(
                        (getattr(r, k),) if getattr(r, k) is not None else ()
                            for k in ([k] if k else StackResult._sort)),
                    reverse=reverse ^ (not k or k in StackResult._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f,
                    (by if by is not None else StackResult._by)
                        + [k for k in (
                            fields if fields is not None
                                else StackResult._fields)])
            writer.writeheader()
            for r in results:
                writer.writerow(
                        {k: getattr(r, k) for k in (
                                by if by is not None else StackResult._by)}
                            | {k: getattr(r, k) for k in (
                                fields if fields is not None
                                    else StackResult._fields)})

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
                            for k in StackResult._fields):
                        continue
                    try:
                        diff_results.append(StackResult(
                                **{k: r[k] for k in StackResult._by
                                    if k in r and r[k].strip()},
                                **{k: r[k] for k in StackResult._fields
                                    if k in r and r[k].strip()}))
                    except TypeError:
                        raise
        except FileNotFoundError:
            pass

        # fold
        diff_results = fold(StackResult, diff_results, by=by, defines=defines)

    # print table
    if not args.get('quiet'):
        table(StackResult, results, diff_results,
                by=by if by is not None else ['function'],
                fields=fields,
                sort=sort,
                detect_cycles=False,
                **args)

    # error on recursion
    if args.get('error_on_recursion') and any(
            mt.isinf(float(r.limit)) for r in results):
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Find stack usage at the function level.",
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
            choices=StackResult._by,
            help="Group by this field.")
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action='append',
            choices=StackResult._fields,
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
            '--no-shrinkwrap',
            action='store_true',
            help="Ignore the effects of shrinkwrap optimizations (assume one "
                "big frame per function).")
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
            '-e', '--error-on-recursion',
            action='store_true',
            help="Error if any functions are recursive.")
    parser.add_argument(
            '--warn-on-incomplete',
            action='store_true',
            help="Warn if callgraph may be incomplete.")
    parser.add_argument(
            '--error-on-incomplete',
            action='store_true',
            help="Error if callgraph may be incomplete.")
    parser.add_argument(
            '--objdump-path',
            type=lambda x: x.split(),
            default=OBJDUMP_PATH,
            help="Path to the objdump executable, may include flags. "
                "Defaults to %r." % OBJDUMP_PATH)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
