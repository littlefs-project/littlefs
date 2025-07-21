#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co


FILTERS = [
    (['--o', '--open'],      'O',       "Filter by LFS3_O_* flags."),
    (['--a', '--attr'],      'A',       "Filter by LFS3_A_* flags."),
    (['--f', '--format'],    'F',       "Filter by LFS3_F_* flags."),
    (['--m', '--mount'],     'M',       "Filter by LFS3_M_* flags."),
    ('--gc',                 'GC',      "Filter by LFS3_GC_* flags."),
    (['--i', '--info'],      'I',       "Filter by LFS3_I_* flags."),
    (['--t', '--traversal'], 'T',       "Filter by LFS3_T_* flags."),
    ('--alloc',              'ALLOC',   "Filter by LFS3_ALLOC_* flags."),
    (['--rc', '--rcompat'],  'RCOMPAT', "Filter by LFS3_RCOMPAT_* flags."),
    (['--wc', '--wcompat'],  'WCOMPAT', "Filter by LFS3_WCOMPAT_* flags."),
    (['--oc', '--ocompat'],  'OCOMPAT', "Filter by LFS3_OCOMPAT_* flags."),
]

FLAGS = [
    # File open flags
    ('O_MODE',                  3, "The file's access mode"                   ),
    ('^_RDONLY',                0, "Open a file as read only"                 ),
    ('^_WRONLY',                1, "Open a file as write only"                ),
    ('^_RDWR',                  2, "Open a file as read and write"            ),
    ('O_CREAT',        0x00000004, "Create a file if it does not exist"       ),
    ('O_EXCL',         0x00000008, "Fail if a file already exists"            ),
    ('O_TRUNC',        0x00000010, "Truncate the existing file to zero size"  ),
    ('O_APPEND',       0x00000020, "Move to end of file on every write"       ),
    ('O_FLUSH',        0x00000040, "Flush data on every write"                ),
    ('O_SYNC',         0x00000080, "Sync metadata on every write"             ),
    ('O_DESYNC',       0x04000000, "Do not sync or recieve file updates"      ),
    ('O_CKMETA',       0x00001000, "Check metadata checksums"                 ),
    ('O_CKDATA',       0x00002000, "Check metadata + data checksums"          ),

    ('o_WRSET',                 3, "Open a file as an atomic write"           ),
    ('o_TYPE',         0xf0000000, "The file's type"                          ),
    ('^_REG',          0x10000000, "Type = regular-file"                      ),
    ('^_DIR',          0x20000000, "Type = directory"                         ),
    ('^_STICKYNOTE',   0x30000000, "Type = stickynote"                        ),
    ('^_BOOKMARK',     0x40000000, "Type = bookmark"                          ),
    ('^_ORPHAN',       0x50000000, "Type = orphan"                            ),
    ('^_TRAVERSAL',    0x60000000, "Type = traversal"                         ),
    ('^_UNKNOWN',      0x70000000, "Type = unknown"                           ),
    ('o_ZOMBIE',       0x08000000, "File has been removed"                    ),
    ('o_UNCREAT',      0x02000000, "File does not exist yet"                  ),
    ('o_UNSYNC',       0x01000000, "File's metadata does not match disk"      ),
    ('o_UNCRYST',      0x00800000, "File's leaf not fully crystallized"       ),
    ('o_UNFLUSH',      0x00400000, "File's cache does not match disk"         ),

    # Custom attribute flags
    ('A_MODE',                  3, "The attr's access mode"                   ),
    ('^_RDONLY',                0, "Open an attr as read only"                ),
    ('^_WRONLY',                1, "Open an attr as write only"               ),
    ('^_RDWR',                  2, "Open an attr as read and write"           ),
    ('A_LAZY',               0x04, "Only write attr if file changed"          ),

    # Filesystem format flags
    ('F_MODE',                  1, "Format's access mode"                     ),
    ('^_RDWR',                  0, "Format the filesystem as read and write"  ),
    ('F_REVDBG',       0x00000010, "Add debug info to revision counts"        ),
    ('F_REVNOISE',     0x00000020, "Add noise to revision counts"             ),
    ('F_CKPROGS',      0x00080000, "Check progs by reading back progged data" ),
    ('F_CKFETCHES',    0x00100000, "Check block checksums before first use"   ),
    ('F_CKMETAPARITY', 0x00200000, "Check metadata tag parity bits"           ),
    ('F_CKDATACKSUMS', 0x00800000, "Check data checksums on reads"            ),

    ('F_CKMETA',       0x00001000, "Check metadata checksums"                 ),
    ('F_CKDATA',       0x00002000, "Check metadata + data checksums"          ),

    # Filesystem mount flags
    ('M_MODE',                  1, "Mount's access mode"                      ),
    ('^_RDWR',                  0, "Mount the filesystem as read and write"   ),
    ('^_RDONLY',                1, "Mount the filesystem as read only"        ),
    ('M_FLUSH',        0x00000040, "Open all files with LFS3_O_FLUSH"         ),
    ('M_SYNC',         0x00000080, "Open all files with LFS3_O_SYNC"          ),
    ('M_REVDBG',       0x00000010, "Add debug info to revision counts"        ),
    ('M_REVNOISE',     0x00000020, "Add noise to revision counts"             ),
    ('M_CKPROGS',      0x00080000, "Check progs by reading back progged data" ),
    ('M_CKFETCHES',    0x00100000, "Check block checksums before first use"   ),
    ('M_CKMETAPARITY', 0x00200000, "Check metadata tag parity bits"           ),
    ('M_CKDATACKSUMS', 0x00800000, "Check data checksums on reads"            ),

    ('M_MKCONSISTENT', 0x00000100, "Make the filesystem consistent"           ),
    ('M_LOOKAHEAD',    0x00000200, "Populate lookahead buffer"                ),
    ('M_COMPACT',      0x00000800, "Compact metadata logs"                    ),
    ('M_CKMETA',       0x00001000, "Check metadata checksums"                 ),
    ('M_CKDATA',       0x00002000, "Check metadata + data checksums"          ),

    # GC flags
    ('GC_MKCONSISTENT',0x00000100, "Make the filesystem consistent"           ),
    ('GC_LOOKAHEAD',   0x00000200, "Populate lookahead buffer"                ),
    ('GC_COMPACT',     0x00000800, "Compact metadata logs"                    ),
    ('GC_CKMETA',      0x00001000, "Check metadata checksums"                 ),
    ('GC_CKDATA',      0x00002000, "Check metadata + data checksums"          ),

    # Filesystem info flags
    ('I_RDONLY',       0x00000001, "Mounted read only"                        ),
    ('I_FLUSH',        0x00000040, "Mounted with LFS3_M_FLUSH"                ),
    ('I_SYNC',         0x00000080, "Mounted with LFS3_M_SYNC"                 ),
    ('I_REVDBG',       0x00000010, "Mounted with LFS3_M_REVDBG"               ),
    ('I_REVNOISE',     0x00000020, "Mounted with LFS3_M_REVNOISE"             ),
    ('I_CKPROGS',      0x00080000, "Mounted with LFS3_M_CKPROGS"              ),
    ('I_CKFETCHES',    0x00100000, "Mounted with LFS3_M_CKFETCHES"            ),
    ('I_CKMETAPARITY', 0x00200000, "Mounted with LFS3_M_CKMETAPARITY"         ),
    ('I_CKDATACKSUMS', 0x00800000, "Mounted with LFS3_M_CKDATACKSUMS"         ),

    ('I_MKCONSISTENT', 0x00000100, "Filesystem needs mkconsistent to write"   ),
    ('I_LOOKAHEAD',    0x00000200, "Lookahead buffer is not full"             ),
    ('I_COMPACT',      0x00000800, "Filesystem may have uncompacted metadata" ),
    ('I_CKMETA',       0x00001000, "Metadata checksums not checked recently"  ),
    ('I_CKDATA',       0x00002000, "Data checksums not checked recently"      ),

    ('i_INMTREE',      0x08000000, "Committing to mtree"                      ),

    # Traversal flags
    ('T_MODE',                  1, "The traversal's access mode"              ),
    ('^_RDWR',                  0, "Open traversal as read and write"         ),
    ('^_RDONLY',                1, "Open traversal as read only"              ),
    ('T_MTREEONLY',    0x00000002, "Only traverse the mtree"                  ),
    ('T_MKCONSISTENT',
                       0x00000100, "Make the filesystem consistent"           ),
    ('T_LOOKAHEAD',    0x00000200, "Populate lookahead buffer"                ),
    ('T_COMPACT',      0x00000800, "Compact metadata logs"                    ),
    ('T_CKMETA',       0x00001000, "Check metadata checksums"                 ),
    ('T_CKDATA',       0x00002000, "Check metadata + data checksums"          ),

    ('t_TYPE',         0xf0000000, "The traversal's type"                     ),
    ('^_REG',          0x10000000, "Type = regular-file"                      ),
    ('^_DIR',          0x20000000, "Type = directory"                         ),
    ('^_STICKYNOTE',   0x30000000, "Type = stickynote"                        ),
    ('^_BOOKMARK',     0x40000000, "Type = bookmark"                          ),
    ('^_ORPHAN',       0x50000000, "Type = orphan"                            ),
    ('^_TRAVERSAL',    0x60000000, "Type = traversal"                         ),
    ('^_UNKNOWN',      0x70000000, "Type = unknown"                           ),
    ('t_TSTATE',       0x000f0000, "The current traversal state"              ),
    ('^_MROOTANCHOR',
                       0x00000000, "Tstate = mroot-anchor"                    ),
    ('^_MROOTCHAIN',   0x00010000, "Tstate = mroot-chain"                     ),
    ('^_MTREE',        0x00020000, "Tstate = mtree"                           ),
    ('^_MDIRS',        0x00030000, "Tstate = mtree-mdirs"                     ),
    ('^_MDIR',         0x00040000, "Tstate = mdir"                            ),
    ('^_BTREE',        0x00050000, "Tstate = btree"                           ),
    ('^_HANDLES',      0x00060000, "Tstate = open-mdirs"                      ),
    ('^_HBTREE',       0x00070000, "Tstate = open-btree"                      ),
    ('^_DONE',         0x00080000, "Tstate = done"                            ),
    ('t_BTYPE',        0x00f00000, "The current block type"                   ),
    ('^_MDIR',         0x00100000, "Btype = mdir"                             ),
    ('^_BTREE',        0x00200000, "Btype = btree"                            ),
    ('^_DATA',         0x00300000, "Btype = data"                             ),
    ('t_ZOMBIE',       0x08000000, "File has been removed"                    ),
    ('t_DIRTY',        0x02000000, "Filesystem modified during traversal"     ),
    ('t_MUTATED',      0x01000000, "Filesystem modified by traversal"         ),

    # Block allocator flags
    ('alloc_ERASE',    0x00000001, "Please erase the block"                   ),

    # Read-compat flags
    ('RCOMPAT_NONSTANDARD',
                       0x00000001, "Non-standard filesystem format"           ),
    ('RCOMPAT_WRONLY', 0x00000002, "Reading is disallowed"                    ),
    ('RCOMPAT_BMOSS',  0x00000010, "Files may use inlined data"               ),
    ('RCOMPAT_BSPROUT',0x00000020, "Files may use block pointers"             ),
    ('RCOMPAT_BSHRUB', 0x00000040, "Files may use inlined btrees"             ),
    ('RCOMPAT_BTREE',  0x00000080, "Files may use btrees"                     ),
    ('RCOMPAT_MMOSS',  0x00000100, "May use an inlined mdir"                  ),
    ('RCOMPAT_MSPROUT',0x00000200, "May use an mdir pointer"                  ),
    ('RCOMPAT_MSHRUB', 0x00000400, "May use an inlined mtree"                 ),
    ('RCOMPAT_MTREE',  0x00000800, "May use an mdir btree"                    ),
    ('RCOMPAT_GRM',    0x00001000, "Global-remove in use"                     ),
    ('rcompat_OVERFLOW',
                       0x80000000, "Can't represent all flags"                ),

    # Write-compat flags
    ('WCOMPAT_NONSTANDARD',
                       0x00000001, "Non-standard filesystem format"           ),
    ('WCOMPAT_RDONLY', 0x00000002, "Writing is disallowed"                    ),
    ('WCOMPAT_DIR',    0x00000010, "Directory file types in use"              ),
    ('WCOMPAT_GCKSUM', 0x00001000, "Global-checksum in use"                   ),
    ('wcompat_OVERFLOW',
                       0x80000000, "Can't represent all flags"                ),

    # Optional-compat flags
    ('OCOMPAT_NONSTANDARD',
                       0x00000001, "Non-standard filesystem format"           ),
    ('ocompat_OVERFLOW',
                       0x80000000, "Can't represent all flags"                ),
]


def main(flags, *,
        list=False,
        all=False,
        filter=[]):
    import builtins
    list_, list = list, builtins.list
    all_, all = all, builtins.all
    filter_, filter = filter, builtins.filter

    # filter by prefix if there are any filters
    filter__ = set(filter_)
    flags__ = []
    types__ = co.defaultdict(lambda: set())
    for n, f, h in FLAGS:
        p, n = n.split('_', 1)
        if p == '^':
            p = last_p
            t = last_t
            types__[p].add(t)
        else:
            t = None
            last_p = p
            last_t = f

        if not filter__ or p.upper() in filter__:
            flags__.append((p, t, n, f, h))

    lines = []
    # list all known flags
    if list_:
        for p, t, n, f, h in flags__:
            if not all_ and (t is not None or p[0].islower()):
                continue
            lines.append(('LFS3_%s_%s' % (p, n), '0x%08x' % f, h))

    # find flags by name or value
    else:
        for f_ in flags:
            found = False
            # find by LFS3_+prefix+_+name
            for p, t, n, f, h in flags__:
                if 'LFS3_%s_%s' % (p, n) == f_.upper():
                    lines.append(('LFS3_%s_%s' % (p, n), '0x%08x' % f, h))
                    found = True
            if found:
                continue
            # find by prefix+_+name
            for p, t, n, f, h in flags__:
                if '%s_%s' % (p, n) == f_.upper():
                    lines.append(('LFS3_%s_%s' % (p, n), '0x%08x' % f, h))
                    found = True
            if found:
                continue
            # find by name
            for p, t, n, f, h in flags__:
                if n == f_.upper():
                    lines.append(('LFS3_%s_%s' % (p, n), '0x%08x' % f, h))
                    found = True
            if found:
                continue
            # find by value
            try:
                f__ = int(f_, 0)
                f___ = f__
                for p, t, n, f, h in flags__:
                    # ignore type masks here
                    if t is None and f in types__[p]:
                        continue
                    # matches flag?
                    if t is None and (f__ & f) == f:
                        lines.append(('LFS3_%s_%s' % (p, n), '0x%08x' % f, h))
                        f___ &= ~f
                    # matches type?
                    elif t is not None and (f__ & t) == f:
                        lines.append(('LFS3_%s_%s' % (p, n), '0x%08x' % f, h))
                        f___ &= ~t
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
    class AppendFilter(argparse.Action):
        def __init__(self, nargs=None, **kwargs):
            super().__init__(nargs=0, **kwargs)
        def __call__(self, parser, namespace, value, option):
            if getattr(namespace, 'filter', None) is None:
                namespace.filter = []
            namespace.filter.append(self.const)
    for flag, prefix, help in FILTERS:
        parser.add_argument(
                *([flag] if isinstance(flag, str) else flag),
                action=AppendFilter,
                const=prefix,
                help=help)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
