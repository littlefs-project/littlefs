#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import functools as ft


# Flag prefixes
PREFIX_O       = ['--o', '--open']      # Filter by LFS3_O_* flags
PREFIX_SEEK    = ['--seek']             # Filter by LFS3_SEEK_* flags
PREFIX_A       = ['--a', '--attr']      # Filter by LFS3_A_* flags
PREFIX_F       = ['--f', '--format']    # Filter by LFS3_F_* flags
PREFIX_M       = ['--m', '--mount']     # Filter by LFS3_M_* flags
PREFIX_GC      = ['--gc']               # Filter by LFS3_GC_* flags
PREFIX_I       = ['--i', '--info']      # Filter by LFS3_I_* flags
PREFIX_T       = ['--t', '--trv']       # Filter by LFS3_T_* flags
PREFIX_ALLOC   = ['--alloc']            # Filter by LFS3_ALLOC_* flags
PREFIX_RCOMPAT = ['--rc', '--rcompat']  # Filter by LFS3_RCOMPAT_* flags
PREFIX_WCOMPAT = ['--wc', '--wcompat']  # Filter by LFS3_WCOMPAT_* flags
PREFIX_OCOMPAT = ['--oc', '--ocompat']  # Filter by LFS3_OCOMPAT_* flags


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
O_DESYNC        = 0x04000000  # --  Do not sync or recieve file updates
O_CKMETA        = 0x00001000  # --  Check metadata checksums
O_CKDATA        = 0x00002000  # --  Check metadata + data checksums

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
o_UNCREAT       = 0x02000000  # i-  File does not exist yet
o_UNSYNC        = 0x01000000  # i-  File's metadata does not match disk
o_UNCRYST       = 0x00800000  # i-  File's leaf not fully crystallized
o_UNGRAFT       = 0x00400000  # i-  File's leaf does not match disk
o_UNFLUSH       = 0x00200000  # i-  File's cache does not match disk

# File seek flags
seek_MODE       = 0xffffffff  # im  Seek mode
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
F_REVDBG        = 0x00000010  # y-  Add debug info to revision counts
F_REVNOISE      = 0x00000020  # y-  Add noise to revision counts
F_CKPROGS       = 0x00080000  # y-  Check progs by reading back progged data
F_CKFETCHES     = 0x00100000  # y-  Check block checksums before first use
F_CKMETAPARITY  = 0x00200000  # y-  Check metadata tag parity bits
F_CKDATACKSUMS  = 0x00800000  # y-  Check data checksums on reads

F_CKMETA        = 0x00001000  # y-  Check metadata checksums
F_CKDATA        = 0x00002000  # y-  Check metadata + data checksums

F_GBMAP         = 0x01000000  # y-  Use the global on-disk block-map

# Filesystem mount flags
M_MODE          =          1  # -m  Mount's access mode
M_RDWR          =          0  # -^  Mount the filesystem as read and write
M_RDONLY        =          1  # -^  Mount the filesystem as read only
M_FLUSH         = 0x00000040  # y-  Open all files with LFS3_O_FLUSH
M_SYNC          = 0x00000080  # y-  Open all files with LFS3_O_SYNC
M_REVDBG        = 0x00000010  # y-  Add debug info to revision counts
M_REVNOISE      = 0x00000020  # y-  Add noise to revision counts
M_CKPROGS       = 0x00080000  # y-  Check progs by reading back progged data
M_CKFETCHES     = 0x00100000  # y-  Check block checksums before first use
M_CKMETAPARITY  = 0x00200000  # y-  Check metadata tag parity bits
M_CKDATACKSUMS  = 0x00800000  # y-  Check data checksums on reads

M_MKCONSISTENT  = 0x00000100  # y-  Make the filesystem consistent
M_RELOOKAHEAD   = 0x00000200  # y-  Repopulate lookahead buffer
M_REGBMAP       = 0x00000400  # y-  Repopulate the gbmap
M_COMPACTMETA   = 0x00000800  # y-  Compact metadata logs
M_CKMETA        = 0x00001000  # y-  Check metadata checksums
M_CKDATA        = 0x00002000  # y-  Check metadata + data checksums

# GC flags
GC_MKCONSISTENT = 0x00000100  # --  Make the filesystem consistent
GC_RELOOKAHEAD  = 0x00000200  # --  Repopulate lookahead buffer
GC_REGBMAP      = 0x00000400  # --  Repopulate the gbmap
GC_COMPACTMETA  = 0x00000800  # --  Compact metadata logs
GC_CKMETA       = 0x00001000  # --  Check metadata checksums
GC_CKDATA       = 0x00002000  # --  Check metadata + data checksums

# Filesystem info flags
I_RDONLY        = 0x00000001  # --  Mounted read only
I_FLUSH         = 0x00000040  # --  Mounted with LFS3_M_FLUSH
I_SYNC          = 0x00000080  # --  Mounted with LFS3_M_SYNC
I_REVDBG        = 0x00000010  # --  Mounted with LFS3_M_REVDBG
I_REVNOISE      = 0x00000020  # --  Mounted with LFS3_M_REVNOISE
I_CKPROGS       = 0x00080000  # --  Mounted with LFS3_M_CKPROGS
I_CKFETCHES     = 0x00100000  # --  Mounted with LFS3_M_CKFETCHES
I_CKMETAPARITY  = 0x00200000  # --  Mounted with LFS3_M_CKMETAPARITY
I_CKDATACKSUMS  = 0x00800000  # --  Mounted with LFS3_M_CKDATACKSUMS

I_MKCONSISTENT  = 0x00000100  # --  Filesystem needs mkconsistent to write
I_RELOOKAHEAD   = 0x00000200  # --  Lookahead buffer is not full
I_REGBMAP       = 0x00000400  # --  The gbmap is not full
I_COMPACTMETA   = 0x00000800  # --  Filesystem may have uncompacted metadata
I_CKMETA        = 0x00001000  # --  Metadata checksums not checked recently
I_CKDATA        = 0x00002000  # --  Data checksums not checked recently

I_GBMAP         = 0x01000000  # --  Global on-disk block-map in use

i_INMODE        = 0x00030000  # im  Btree commit mode
i_INMTREE       = 0x00010000  # i^  Committing to mtree
i_INGBMAP       = 0x00020000  # i^  Committing to gbmap

# Traversal flags
T_MODE          =          1  # -m  The traversal's access mode
T_RDWR          =          0  # -^  Open traversal as read and write
T_RDONLY        =          1  # -^  Open traversal as read only
T_MTREEONLY     = 0x00000002  # --  Only traverse the mtree
T_MKCONSISTENT  = 0x00000100  # --  Make the filesystem consistent
T_RELOOKAHEAD   = 0x00000200  # --  Repopulate lookahead buffer
T_REGBMAP       = 0x00000400  # --  Repopulate the gbmap
T_COMPACTMETA   = 0x00000800  # --  Compact metadata logs
T_CKMETA        = 0x00001000  # --  Check metadata checksums
T_CKDATA        = 0x00002000  # --  Check metadata + data checksums

t_TYPE          = 0xf0000000  # im  The traversal's type
t_REG           = 0x10000000  # i^  Type = regular-file
t_DIR           = 0x20000000  # i^  Type = directory
t_STICKYNOTE    = 0x30000000  # i^  Type = stickynote
t_BOOKMARK      = 0x40000000  # i^  Type = bookmark
t_ORPHAN        = 0x50000000  # i^  Type = orphan
t_TRAVERSAL     = 0x60000000  # i^  Type = traversal
t_UNKNOWN       = 0x70000000  # i^  Type = unknown
t_TSTATE        = 0x000f0000  # im  The current traversal state
t_MROOTANCHOR   = 0x00000000  # i^  Tstate = mroot-anchor
t_MROOTCHAIN    = 0x00010000  # i^  Tstate = mroot-chain
t_MTREE         = 0x00020000  # i^  Tstate = mtree
t_MDIRS         = 0x00030000  # i^  Tstate = mtree-mdirs
t_MDIR          = 0x00040000  # i^  Tstate = mdir
t_BTREE         = 0x00050000  # i^  Tstate = btree
t_HANDLES       = 0x00060000  # i^  Tstate = open-mdirs
t_HBTREE        = 0x00070000  # i^  Tstate = open-btree
t_GBMAP         = 0x00080000  # i^  Tstate = gbmap
t_GBMAP_P       = 0x00090000  # i^  Tstate = gbmap_p
t_DONE          = 0x000a0000  # i^  Tstate = done
t_BTYPE         = 0x00f00000  # im  The current block type
t_MDIR          = 0x00100000  # i^  Btype = mdir
t_BTREE         = 0x00200000  # i^  Btype = btree
t_DATA          = 0x00300000  # i^  Btype = data
t_ZOMBIE        = 0x08000000  # i-  File has been removed
t_DIRTY         = 0x04000000  # i-  Filesystem ckpointed outside traversal
t_CKPOINTED     = 0x02000000  # i-  Filesystem ckpointed during traversal

# Block allocator flags
alloc_ERASE     = 0x00000001  # i-  Please erase the block

# Read-compat flags
RCOMPAT_NONSTANDARD = 0x00000001  # --  Non-standard filesystem format
RCOMPAT_WRONLY      = 0x00000002  # --  Reading is disallowed
RCOMPAT_BMOSS       = 0x00000010  # --  Files may use inlined data
RCOMPAT_BSPROUT     = 0x00000020  # --  Files may use block pointers
RCOMPAT_BSHRUB      = 0x00000040  # --  Files may use inlined btrees
RCOMPAT_BTREE       = 0x00000080  # --  Files may use btrees
RCOMPAT_MMOSS       = 0x00000100  # --  May use an inlined mdir
RCOMPAT_MSPROUT     = 0x00000200  # --  May use an mdir pointer
RCOMPAT_MSHRUB      = 0x00000400  # --  May use an inlined mtree
RCOMPAT_MTREE       = 0x00000800  # --  May use an mdir btree
RCOMPAT_GRM         = 0x00001000  # --  Global-remove in use
rcompat_OVERFLOW    = 0x80000000  # i-  Can't represent all flags

# Write-compat flags
WCOMPAT_NONSTANDARD = 0x00000001  # --  Non-standard filesystem format
WCOMPAT_RDONLY      = 0x00000002  # --  Writing is disallowed
WCOMPAT_DIR         = 0x00000010  # --  Directory file types in use
WCOMPAT_GCKSUM      = 0x00001000  # --  Global-checksum in use
WCOMPAT_GBMAP       = 0x00002000  # --  Global on-disk block-map in use
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
            prefix=None,
            yes=False,
            internal=False,
            mask=False,
            type=False):
        self.name = name
        self.flag = flag
        self.help = help
        self.prefix = prefix
        self.yes = yes
        self.internal = internal
        self.mask = mask
        self.type = type

    def __repr__(self):
        return 'Flag(%r, %r, %r)' % (
                self.name,
                self.flag,
                self.help)

    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __hash__(self):
        return hash(self.name)

    def line(self):
        return ('LFS3_%s' % self.name, '0x%08x' % self.flag, self.help)

    @staticmethod
    @ft.cache
    def flags():
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
        for line in (inspect.getsource(
                    inspect.getmodule(inspect.currentframe()))
                .replace('\\\n', '')
                .splitlines()):
            m = flag_pattern.match(line)
            if m:
                flags.append(Flag(
                        m.group('name'),
                        globals()[m.group('name')],
                        m.group('help'),
                        # associate flags -> prefix
                        prefix=prefixes_[
                            m.group('name').split('_', 1)[0].upper()],
                        yes='y' in m.group('mode'),
                        internal='i' in m.group('mode'),
                        mask='m' in m.group('mode'),
                        # associate types -> mask
                        type=mask_ if '^' in m.group('mode') else False))

                # keep track of last mask
                if flags[-1].mask:
                    mask_ = flags[-1]

        return flags


def main(flags, *,
        list=False,
        all=False,
        prefixes=[]):
    import builtins
    list_, list = list, builtins.list
    all_, all = all, builtins.all

    # find flags
    flags__ = Flag.flags()

    # filter by prefixes if there are any prefixes
    if prefixes:
        prefixes = set(prefixes)
        flags__ = [f for f in flags__ if f.prefix in prefixes]

    lines = []
    # list all known flags
    if list_:
        for f in flags__:
            if not all_ and (f.internal or f.type):
                continue
            lines.append(f.line())

    # find flags by name or value
    else:
        for f_ in flags:
            found = False
            # find by LFS3_+prefix+_+name
            for f in flags__:
                if 'LFS3_%s' % f.name.upper() == f_.upper():
                    lines.append(f.line())
                    found = True
            if found:
                continue
            # find by prefix+_+name
            for f in flags__:
                if '%s' % f.name.upper() == f_.upper():
                    lines.append(f.line())
                    found = True
            if found:
                continue
            # find by name
            for f in flags__:
                if f.name.split('_', 1)[1].upper() == f_.upper():
                    lines.append(f.line())
                    found = True
            if found:
                continue
            # find by value
            try:
                f__ = int(f_, 0)
                f___ = f__
                for f in flags__:
                    # ignore type masks here
                    if f.mask:
                        continue
                    # matches flag?
                    if not f.type and (f__ & f.flag) == f.flag:
                        lines.append(f.line())
                        f___ &= ~f.flag
                    # matches type?
                    elif f.type and (f__ & f.type.flag) == f.flag:
                        lines.append(f.line())
                        f___ &= ~f.type.flag
                if f___:
                    lines.append(('?', '0x%08x' % f___, 'Unknown flags'))
            except ValueError:
                lines.append(('?', f_, 'Unknown flag'))

    # first find widths
    w = [0, 0]
    for l in lines:
        w[0] = max(w[0], len(l[0]))
        w[1] = max(w[1], len(l[1]))

    # then print results
    for l in lines:
        print('%-*s  %-*s  %s' % (
                w[0], l[0],
                w[1], l[1],
                l[2]))


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Decode littlefs flags.",
            allow_abbrev=False)
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
    class AppendPrefix(argparse.Action):
        def __init__(self, nargs=None, **kwargs):
            super().__init__(nargs=0, **kwargs)
        def __call__(self, parser, namespace, value, option):
            if getattr(namespace, 'prefixes', None) is None:
                namespace.prefixes = []
            namespace.prefixes.append(self.const)
    for p in Prefix.prefixes():
        parser.add_argument(
                *p.aliases,
                action=AppendPrefix,
                const=p,
                help=p.help+'.')
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
