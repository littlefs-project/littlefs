#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import functools as ft
import math as mt


# Flag prefixes
PREFIX_O       = ['+o', '+open']        # Filter by LFS3_O_* flags
PREFIX_SEEK    = ['+seek']              # Filter by LFS3_SEEK_* flags
PREFIX_A       = ['+a', '+attr']        # Filter by LFS3_A_* flags
PREFIX_F       = ['+f', '+format']      # Filter by LFS3_F_* flags
PREFIX_M       = ['+m', '+mount']       # Filter by LFS3_M_* flags
PREFIX_CK      = ['+ck']                # Filter by LFS3_CK_* flags
PREFIX_GC      = ['+gc']                # Filter by LFS3_GC_* flags
PREFIX_I       = ['+i', '+info']        # Filter by LFS3_I_* flags
PREFIX_T       = ['+t', '+trv']         # Filter by LFS3_T_* flags
PREFIX_ALLOC   = ['+alloc']             # Filter by LFS3_ALLOC_* flags
PREFIX_RCOMPAT = ['+rc', '+rcompat']    # Filter by LFS3_RCOMPAT_* flags
PREFIX_WCOMPAT = ['+wc', '+wcompat']    # Filter by LFS3_WCOMPAT_* flags
PREFIX_OCOMPAT = ['+oc', '+ocompat']    # Filter by LFS3_OCOMPAT_* flags


# File open flags
O_MODE          =          3  # -m  The file's access mode
O_RDONLY        =          0  # -^  Open a file as read only
O_WRONLY        =          1  # -^  Open a file as write only
O_RDWR          =          2  # -^  Open a file as read and write
O_CREAT         = 0x00000004  # --  Create a file if it does not exist
O_EXCL          = 0x00000008  # --  Fail if a file already exists
O_TRUNC         = 0x00000010  # --  Truncate the existing file to zero size
O_APPEND        = 0x00000020  # --  Move to end of file on every write
O_FLUSH         = 0x00000040  # y-  Flush data on every write
O_SYNC          = 0x00000080  # y-  Sync metadata on every write
O_DESYNC        = 0x00100000  # --  Do not sync or recieve file updates

O_CKMETA        = 0x00001000  # --  Check metadata checksums
O_CKDATA        = 0x00002000  # --  Check metadata + data checksums
O_CK            = 0x00003000  # a-  Alias for all check work

o_WRSET         =          3  # i-  Open a file as an atomic write
o_TYPE          = 0xf0000000  # im  The file's type
o_REG           = 0x10000000  # i^  Type = regular-file
o_DIR           = 0x20000000  # i^  Type = directory
o_STICKYNOTE    = 0x30000000  # i^  Type = stickynote
o_BOOKMARK      = 0x40000000  # i^  Type = bookmark
o_ORPHAN        = 0x50000000  # i^  Type = orphan
o_TRAVERSAL     = 0x60000000  # i^  Type = traversal
o_UNKNOWN       = 0x70000000  # i^  Type = unknown
o_ZOMBIE        = 0x08000000  # i-  File has been removed
o_UNCREAT       = 0x04000000  # i-  File does not exist yet
o_UNSYNC        = 0x02000000  # i-  File's metadata does not match disk
o_UNCRYST       = 0x01000000  # i-  File's leaf not fully crystallized
o_UNGRAFT       = 0x00800000  # i-  File's leaf does not match disk
o_UNFLUSH       = 0x00400000  # i-  File's cache does not match disk

# File seek flags
SEEK_MODE       = 0xffffffff  # -m  Seek mode
SEEK_SET        =          0  # -^  Seek relative to an absolute position
SEEK_CUR        =          1  # -^  Seek relative to the current file position
SEEK_END        =          2  # -^  Seek relative to the end of the file

# Custom attribute flags
A_MODE          =          3  # -m  The attr's access mode
A_RDONLY        =          0  # -^  Open an attr as read only
A_WRONLY        =          1  # -^  Open an attr as write only
A_RDWR          =          2  # -^  Open an attr as read and write
A_LAZY          =       0x04  # --  Only write attr if file changed

# Filesystem format flags
F_MODE          =          1  # -m  Format's access mode
F_RDWR          =          0  # -^  Format the filesystem as read and write
F_GBMAP         = 0x02000000  # y-  Use the global on-disk block-map

F_REVPERTURB    = 0x00000010  # y-  Perturb first bit in revision count
F_REVNOISE      = 0x00000020  # y-  Add noise to revision counts
F_CKPROGS       = 0x00100000  # y-  Check progs by reading back progged data
F_CKFETCHES     = 0x00200000  # y-  Check block checksums before first use
F_CKMETAPARITY  = 0x00400000  # y-  Check metadata tag parity bits
F_CKDATACKSUMS  = 0x01000000  # y-  Check data checksums on reads

F_MKCONSISTENT  = 0x00000100  # y-  Make the filesystem consistent
F_LOOKAHEAD     = 0x00000200  # y-  Repopulate lookahead buffer
F_PREERASE      = 0x00000400  # y-  Try to pre-erase free blocks
F_COMPACT       = 0x00000800  # y-  Compact metadata logs
F_CKMETA        = 0x00001000  # y-  Check metadata checksums
F_CKDATA        = 0x00002000  # y-  Check metadata + data checksums
F_CK            = 0x00003000  # a-  Alias for all check work
F_GC            = 0x00003f00  # a-  Alias for all gc work

# Filesystem mount flags
M_MODE          =          1  # -m  Mount's access mode
M_RDWR          =          0  # -^  Mount the filesystem as read and write
M_RDONLY        =          1  # -^  Mount the filesystem as read only
M_FLUSH         = 0x00000040  # y-  Open all files with LFS3_O_FLUSH
M_SYNC          = 0x00000080  # y-  Open all files with LFS3_O_SYNC
M_REVPERTURB    = 0x00000010  # y-  Perturb first bit in revision count
M_REVNOISE      = 0x00000020  # y-  Add noise to revision counts
M_CKPROGS       = 0x00100000  # y-  Check progs by reading back progged data
M_CKFETCHES     = 0x00200000  # y-  Check block checksums before first use
M_CKMETAPARITY  = 0x00400000  # y-  Check metadata tag parity bits
M_CKDATACKSUMS  = 0x01000000  # y-  Check data checksums on reads

M_MKCONSISTENT  = 0x00000100  # y-  Make the filesystem consistent
M_LOOKAHEAD     = 0x00000200  # y-  Repopulate lookahead buffer
M_PREERASE      = 0x00000400  # y-  Try to pre-erase free blocks
M_COMPACT       = 0x00000800  # y-  Compact metadata logs
M_CKMETA        = 0x00001000  # y-  Check metadata checksums
M_CKDATA        = 0x00002000  # y-  Check metadata + data checksums
M_CK            = 0x00003000  # a-  Alias for all check work
M_GC            = 0x00003f00  # a-  Alias for all gc work

# File/filesystem check flags
CK_MKCONSISTENT = 0x00000100  # --  Make the filesystem consistent
CK_LOOKAHEAD    = 0x00000200  # --  Repopulate lookahead buffer
CK_PREERASE     = 0x00000400  # --  Try to pre-erase free blocks
CK_COMPACT      = 0x00000800  # --  Compact metadata logs
CK_CKMETA       = 0x00001000  # --  Check metadata checksums
CK_CKDATA       = 0x00002000  # --  Check metadata + data checksums
CK_CK           = 0x00003000  # a-  Alias for all check work
CK_GC           = 0x00003f00  # a-  Alias for all gc work

# GC flags
GC_MKCONSISTENT = 0x00000100  # --  Make the filesystem consistent
GC_LOOKAHEAD    = 0x00000200  # --  Repopulate lookahead buffer
GC_PREERASE     = 0x00000400  # --  Try to pre-erase free blocks
GC_COMPACT      = 0x00000800  # --  Compact metadata logs
GC_CKMETA       = 0x00001000  # --  Check metadata checksums
GC_CKDATA       = 0x00002000  # --  Check metadata + data checksums
GC_CK           = 0x00003000  # a-  Alias for all check work
GC_GC           = 0x00003f00  # a-  Alias for all gc work

# Filesystem info flags
I_RDONLY        = 0x00000001  # --  Mounted read only
I_GBMAP         = 0x02000000  # --  Global on-disk block-map in use

I_FLUSH         = 0x00000040  # --  Mounted with LFS3_M_FLUSH
I_SYNC          = 0x00000080  # --  Mounted with LFS3_M_SYNC
I_REVPERTURB    = 0x00000010  # --  Mounted with LFS3_M_REVPERTURB
I_REVNOISE      = 0x00000020  # --  Mounted with LFS3_M_REVNOISE
I_CKPROGS       = 0x00100000  # --  Mounted with LFS3_M_CKPROGS
I_CKFETCHES     = 0x00200000  # --  Mounted with LFS3_M_CKFETCHES
I_CKMETAPARITY  = 0x00400000  # --  Mounted with LFS3_M_CKMETAPARITY
I_CKDATACKSUMS  = 0x01000000  # --  Mounted with LFS3_M_CKDATACKSUMS

I_MKCONSISTENT  = 0x00000100  # --  Filesystem needs mkconsistent to write
I_LOOKAHEAD     = 0x00000200  # --  Lookahead buffer is not full
I_PREERASE      = 0x00000400  # --  Blocks can be pre-erased
I_COMPACT       = 0x00000800  # --  Filesystem may have uncompacted metadata
I_CKMETA        = 0x00001000  # --  Metadata checksums not checked recently
I_CKDATA        = 0x00002000  # --  Data checksums not checked recently

# Traversal flags
T_MODE          =          1  # -m  The traversal's access mode
T_RDWR          =          0  # -^  Open traversal as read and write
T_RDONLY        =          1  # -^  Open traversal as read only
T_MTREEONLY     = 0x00000002  # --  Only traverse the mtree
T_EXCL          = 0x00000008  # --  Error if filesystem modified
T_MKCONSISTENT  = 0x00000100  # --  Make the filesystem consistent
T_LOOKAHEAD     = 0x00000200  # --  Repopulate lookahead buffer
T_PREERASE      = 0x00000400  # --  Try to pre-erase free blocks
T_COMPACT       = 0x00000800  # --  Compact metadata logs
T_CKMETA        = 0x00001000  # --  Check metadata checksums
T_CKDATA        = 0x00002000  # --  Check metadata + data checksums
T_CK            = 0x00003000  # a-  Alias for all check work
T_GC            = 0x00003f00  # a-  Alias for all gc work

t_TYPE          = 0xf0000000  # im  The traversal's type
t_REG           = 0x10000000  # i^  Type = regular-file
t_DIR           = 0x20000000  # i^  Type = directory
t_STICKYNOTE    = 0x30000000  # i^  Type = stickynote
t_BOOKMARK      = 0x40000000  # i^  Type = bookmark
t_ORPHAN        = 0x50000000  # i^  Type = orphan
t_TRAVERSAL     = 0x60000000  # i^  Type = traversal
t_UNKNOWN       = 0x70000000  # i^  Type = unknown
t_BTYPE         = 0x00ff0000  # im  The current block type
t_MDIR          = 0x00010000  # i^  Btype = mdir
t_BTREE         = 0x00020000  # i^  Btype = btree
t_DATA          = 0x00030000  # i^  Btype = data
t_ZOMBIE        = 0x08000000  # i-  File has been removed
t_CKPOINTED     = 0x04000000  # i-  Filesystem ckpointed during traversal
t_DIRTY         = 0x02000000  # i-  Filesystem ckpointed outside traversal
t_STALE         = 0x01000000  # i-  Block queue probably out-of-date

# Block allocator flags
alloc_ERASE     = 0x00000001  # i-  Please erase the block

# Read-compat flags
RCOMPAT_NONSTANDARD = 0x00000001  # --  Non-standard filesystem format
RCOMPAT_WRONLY      = 0x00000004  # --  Reading is disallowed
RCOMPAT_MMOSS       = 0x00000010  # --  May use an inlined mdir
RCOMPAT_MSPROUT     = 0x00000020  # --  May use an mdir pointer
RCOMPAT_MSHRUB      = 0x00000040  # --  May use an inlined mtree
RCOMPAT_MTREE       = 0x00000080  # --  May use an mdir btree
RCOMPAT_BMOSS       = 0x00000100  # --  Files may use inlined data
RCOMPAT_BSPROUT     = 0x00000200  # --  Files may use block pointers
RCOMPAT_BSHRUB      = 0x00000400  # --  Files may use inlined btrees
RCOMPAT_BTREE       = 0x00000800  # --  Files may use btrees
RCOMPAT_GRM         = 0x00010000  # --  Global-remove in use
rcompat_OVERFLOW    = 0x80000000  # i-  Can't represent all flags

# Write-compat flags
WCOMPAT_NONSTANDARD = 0x00000001  # --  Non-standard filesystem format
WCOMPAT_RDONLY      = 0x00000002  # --  Writing is disallowed
WCOMPAT_GCKSUM      = 0x00040000  # --  Global-checksum in use
WCOMPAT_GBMAP       = 0x00080000  # --  Global on-disk block-map in use
WCOMPAT_DIR         = 0x01000000  # --  Directory file types in use
wcompat_OVERFLOW    = 0x80000000  # i-  Can't represent all flags

# Optional-compat flags
OCOMPAT_NONSTANDARD = 0x00000001  # --  Non-standard filesystem format
ocompat_OVERFLOW    = 0x80000000  # i-  Can't represent all flags


# self-parsing prefixes
class Prefix:
    def __init__(self, name, aliases, help):
        self.name = name
        self.aliases = aliases
        self.help = help

    def __repr__(self):
        return 'Prefix(%r, %r, %r)' % (
                self.name,
                self.aliases,
                self.help)

    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __hash__(self):
        return hash(self.name)

    @staticmethod
    @ft.cache
    def prefixes():
        # parse our script's source to figure out prefixes
        import inspect
        import re
        prefixes = []
        prefix_pattern = re.compile(
                '^(?P<name>PREFIX_[^ ]*) *= *(?P<aliases>[^#]*?) *'
                    '#+ *(?P<help>.*)$')
        for line in (inspect.getsource(
                    inspect.getmodule(inspect.currentframe()))
                .replace('\\\n', '')
                .splitlines()):
            m = prefix_pattern.match(line)
            if m:
                prefixes.append(Prefix(
                        m.group('name'),
                        globals()[m.group('name')],
                        m.group('help')))
        return prefixes

# self-parsing flags
class Flag:
    def __init__(self, name, flag, help, *,
            lineno=0,
            prefix=None,
            yes=False,
            alias=False,
            internal=False,
            mask=False,
            type=False):
        self.name = name
        self.flag = flag
        self.help = help
        self.lineno = lineno
        self.prefix = prefix
        self.yes = yes
        self.alias = alias
        self.internal = internal
        self.mask = mask
        self.type = type

    def __repr__(self):
        return 'Flag(%r, %r, %r)' % (
                self.name,
                self.flag,
                self.help)

    def __eq__(self, other):
        return self.name == getattr(other, 'name', None)

    def __ne__(self, other):
        return self.name != getattr(other, 'name', None)

    def __hash__(self):
        return hash(self.name)

    def line(self):
        if isinstance(self, Flag):
            return ('LFS3_%s' % self.name, '0x%08x' % self.flag, self.help)
        elif isinstance(self, int):
            return ('?', '0x%08x' % self, 'Unknown flags')
        else:
            return ('?', str(self), 'Unknown flag')

    @staticmethod
    @ft.cache
    def _flags(*, filter=None):
        # filter by prefixes
        if filter:
            assert isinstance(filter, frozenset)
            # make sure to cache all flags
            flags = Flag._flags()
            return [f for f in flags if f.prefix in filter]

        # parse our script's source to figure out flags
        import inspect
        import re

        # limit to known prefixes
        prefixes_ = {p.name.split('_', 1)[1].upper(): p
                for p in Prefix.prefixes()}
        # keep track of last mask
        mask_ = None

        flags = []
        flag_pattern = re.compile(
                '^(?P<name>(?i:%s)_[^ ]*) '
                        '*= *(?P<flag>[^#]*?) *'
                        '#+ (?P<mode>[^ ]+) *(?P<help>.*)$'
                    % '|'.join(prefixes_.keys()))
        for i, line in enumerate(
                inspect.getsource(inspect.getmodule(inspect.currentframe()))
                    .replace('\\\n', '')
                    .splitlines()):
            m = flag_pattern.match(line)
            if m:
                flags.append(Flag(
                        m.group('name'),
                        globals()[m.group('name')],
                        m.group('help'),
                        lineno=1+i,
                        # associate flags -> prefix
                        prefix=prefixes_[
                            m.group('name').split('_', 1)[0].upper()],
                        yes='y' in m.group('mode'),
                        alias='a' in m.group('mode'),
                        internal='i' in m.group('mode'),
                        mask='m' in m.group('mode'),
                        # associate types -> mask
                        type=mask_ if '^' in m.group('mode') else False))

                # keep track of last mask
                if flags[-1].mask:
                    mask_ = flags[-1]

        return flags

    @staticmethod
    def flags(*, filter=None):
        if isinstance(filter, str):
            filter = frozenset((filter,))
        if filter is not None and not isinstance(filter, frozenset):
            filter = frozenset(filter)
        return Flag._flags(filter=filter)

    _sentinel = object()
    @staticmethod
    def find(f_, *, filter=None, default=_sentinel):
        # find flags, note this is cached
        flags__ = Flag.flags(filter=filter)

        flags_ = []
        # find by LFS3_+prefix+_+name
        for f in flags__:
            if 'LFS3_%s' % f.name.upper() == f_.upper():
                flags_.append(f)
        if flags_:
            return flags_
        # find by prefix+_+name
        for f in flags__:
            if '%s' % f.name.upper() == f_.upper():
                flags_.append(f)
        if flags_:
            return flags_
        # find by name
        for f in flags__:
            if f.name.split('_', 1)[1].upper() == f_.upper():
                flags_.append(f)
        if flags_:
            return flags_
        # find by value
        try:
            f__ = int(f_, 0)
            f___ = f__
            for f in flags__:
                # ignore aliases and type masks here
                if f.alias or f.mask:
                    continue
                # matches flag?
                if not f.type and (f__ & f.flag) == f.flag:
                    flags_.append(f)
                    f___ &= ~f.flag
                # matches type?
                elif f.type and (f__ & f.type.flag) == f.flag:
                    flags_.append(f)
                    f___ &= ~f.type.flag
            if f___:
                flags_.append(f___)
            return flags_
        except ValueError:
            pass
        # not found
        if default is Flag._sentinel:
            raise KeyError(f_)
        else:
            return default


def main(flags, *,
        list=False,
        all=False,
        diff=None,
        color='auto',
        prefixes=[]):
    import builtins
    list_, list = list, builtins.list
    all_, all = all, builtins.all

    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    lines = []
    # list all known flags
    if list_:
        for f in Flag.flags(filter=prefixes or None):
            if not all_ and (f.internal or f.type):
                continue
            lines.append(f.line())

    # diff flags by name or value
    elif diff:
        # first find flags
        a = []
        for f_ in flags:
            a.extend(Flag.find(f_, filter=prefixes or None, default=[f_]))

        b = Flag.find(diff, filter=prefixes or None, default=[diff])

        # compute line-by-line diff
        a_set = set(a)
        b_set = set(b)
        i, j = 0, 0
        while i < len(a) or j < len(b):
            if i < len(a) and (
                    j >= len(b)
                        or getattr(a[i], 'lineno', mt.inf)
                            <= getattr(b[j], 'lineno', mt.inf)):
                if a[i] not in b_set:
                    l = Flag.line(a[i])
                    lines.append(('+'+l[0], *l[1:]))
                else:
                    l = Flag.line(a[i])
                    lines.append((' '+l[0], *l[1:]))
                i += 1
            else:
                if b[j] not in a_set:
                    l = Flag.line(b[j])
                    lines.append(('-'+l[0], *l[1:]))
                j += 1

    # find flags by name or value
    else:
        for f_ in flags:
            for f in Flag.find(f_, filter=prefixes or None, default=[f_]):
                lines.append(Flag.line(f))

    # first find widths
    w = [0, 0]
    for l in lines:
        w[0] = max(w[0], len(l[0]))
        w[1] = max(w[1], len(l[1]))

    # then print results
    for l in lines:
        print('%s%-*s  %-*s  %s%s' % (
                '\x1b[32m' if color and diff and l[0].startswith('+')
                    else '\x1b[31m' if color and diff and l[0].startswith('-')
                    else '',
                w[0], l[0],
                w[1], l[1],
                l[2],
                '\x1b[m' if color and diff and l[0].startswith('+')
                    else '\x1b[m' if color and diff and l[0].startswith('-')
                    else ''))


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Decode littlefs flags.",
            allow_abbrev=False,
            # allow + for prefix filters
            prefix_chars='-+')
    parser.add_argument(
            'flags',
            nargs='*',
            help="Flags or names of flags to decode.")
    parser.add_argument(
            '-l', '--list',
            action='store_true',
            help="List all known flags.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Also show internal flags and types.")
    parser.add_argument(
            '-d', '--diff',
            help="Diff against these flags.")
    parser.add_argument(
            '--color',
            choices=['never', 'always', 'auto'],
            default='auto',
            help="When to use terminal colors. Defaults to 'auto'.")
    prefixes = parser.add_argument_group('prefixes')
    class AppendPrefix(argparse.Action):
        def __init__(self, nargs=None, **kwargs):
            super().__init__(nargs=0, **kwargs)
        def __call__(self, parser, namespace, value, option):
            if getattr(namespace, 'prefixes', None) is None:
                namespace.prefixes = []
            namespace.prefixes.append(self.const)
    for p in Prefix.prefixes():
        prefixes.add_argument(
                *p.aliases,
                action=AppendPrefix,
                const=p,
                help=p.help+'.')
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
