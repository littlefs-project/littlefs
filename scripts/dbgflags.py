#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co


ALIASES = [
    {'O', 'OPEN'},
    {'A', 'ATTR'},
    {'F', 'FORMAT'},
    {'M', 'MOUNT'},
    {'GC'},
    {'I', 'INFO'},
    {'T', 'TRAVERSAL'},
    {'RC', 'RCOMPAT'},
    {'WC', 'WCOMPAT'},
    {'OC', 'OCOMPAT'}
]

FLAGS = [
    # File open flags
    ('O', 'MODE',               3, "The file's access mode"                   ),
    ('^', 'RDONLY',             0, "Open a file as read only"                 ),
    ('^', 'WRONLY',             1, "Open a file as write only"                ),
    ('^', 'RDWR',               2, "Open a file as read and write"            ),
    ('O', 'CREAT',     0x00000004, "Create a file if it does not exist"       ),
    ('O', 'EXCL',      0x00000008, "Fail if a file already exists"            ),
    ('O', 'TRUNC',     0x00000010, "Truncate the existing file to zero size"  ),
    ('O', 'APPEND',    0x00000020, "Move to end of file on every write"       ),
    ('O', 'FLUSH',     0x00000040, "Flush data on every write"                ),
    ('O', 'SYNC',      0x00000080, "Sync metadata on every write"             ),
    ('O', 'DESYNC',    0x00000100, "Do not sync or recieve file updates"      ),
    ('O', 'CKMETA',    0x00100000, "Check metadata checksums"                 ),
    ('O', 'CKDATA',    0x00200000, "Check metadata + data checksums"          ),

    ('o', 'TYPE',      0xf0000000, "The file's type"                          ),
    ('^', 'REG',       0x10000000, "Type = regular-file"                      ),
    ('^', 'DIR',       0x20000000, "Type = directory"                         ),
    ('^', 'STICKYNOTE',0x30000000, "Type = stickynote"                        ),
    ('^', 'BOOKMARK',  0x40000000, "Type = bookmark"                          ),
    ('^', 'ORPHAN',    0x50000000, "Type = orphan"                            ),
    ('^', 'TRAVERSAL', 0x60000000, "Type = traversal"                         ),
    ('^', 'UNKNOWN',   0x70000000, "Type = unknown"                           ),
    ('o', 'UNFLUSH',   0x01000000, "File's data does not match disk"          ),
    ('o', 'UNSYNC',    0x02000000, "File's metadata does not match disk"      ),
    ('o', 'UNCREAT',   0x04000000, "File does not exist yet"                  ),
    ('o', 'ZOMBIE',    0x08000000, "File has been removed"                    ),

    # Custom attribute flags
    ('A', 'MODE',               3, "The attr's access mode"                   ),
    ('^', 'RDONLY',             0, "Open an attr as read only"                ),
    ('^', 'WRONLY',             1, "Open an attr as write only"               ),
    ('^', 'RDWR',               2, "Open an attr as read and write"           ),
    ('A', 'LAZY',            0x04, "Only write attr if file changed"          ),

    # Filesystem format flags
    ('F', 'MODE',               1, "Format's access mode"                     ),
    ('^', 'RDWR',               0, "Format the filesystem as read and write"  ),
    ('F', 'REVDBG',    0x00000010, "Add debug info to revision counts"        ),
    ('F', 'REVNOISE',  0x00000020, "Add noise to revision counts"             ),
    ('F', 'CKPROGS',   0x00000800, "Check progs by reading back progged data" ),
    ('F', 'CKFETCHES', 0x00001000, "Check block checksums before first use"   ),
    ('F', 'CKPARITY',  0x00002000, "Check metadata tag parity bits"           ),
    ('F', 'CKDATACKSUMS',
                       0x00008000, "Check data checksums on reads"            ),

    ('F', 'CKMETA',    0x00100000, "Check metadata checksums"                 ),
    ('F', 'CKDATA',    0x00200000, "Check metadata + data checksums"          ),

    # Filesystem mount flags
    ('M', 'MODE',               1, "Mount's access mode"                      ),
    ('^', 'RDWR',               0, "Mount the filesystem as read and write"   ),
    ('^', 'RDONLY',             1, "Mount the filesystem as read only"        ),
    ('M', 'FLUSH',     0x00000040, "Open all files with LFS_O_FLUSH"          ),
    ('M', 'SYNC',      0x00000080, "Open all files with LFS_O_SYNC"           ),
    ('M', 'REVDBG',    0x00000010, "Add debug info to revision counts"        ),
    ('M', 'REVNOISE',  0x00000020, "Add noise to revision counts"             ),
    ('M', 'CKPROGS',   0x00000800, "Check progs by reading back progged data" ),
    ('M', 'CKFETCHES', 0x00001000, "Check block checksums before first use"   ),
    ('M', 'CKPARITY',  0x00002000, "Check metadata tag parity bits"           ),
    ('M', 'CKDATACKSUMS',
                       0x00008000, "Check data checksums on reads"            ),

    ('M', 'MKCONSISTENT',
                       0x00010000, "Make the filesystem consistent"           ),
    ('M', 'LOOKAHEAD', 0x00020000, "Populate lookahead buffer"                ),
    ('M', 'COMPACT',   0x00080000, "Compact metadata logs"                    ),
    ('M', 'CKMETA',    0x00100000, "Check metadata checksums"                 ),
    ('M', 'CKDATA',    0x00200000, "Check metadata + data checksums"          ),

    # GC flags
    ('GC', 'MKCONSISTENT',
                       0x00010000, "Make the filesystem consistent"           ),
    ('GC', 'LOOKAHEAD',0x00020000, "Populate lookahead buffer"                ),
    ('GC', 'COMPACT',  0x00080000, "Compact metadata logs"                    ),
    ('GC', 'CKMETA',   0x00100000, "Check metadata checksums"                 ),
    ('GC', 'CKDATA',   0x00200000, "Check metadata + data checksums"          ),

    # Filesystem info flags
    ('I', 'RDONLY',    0x00000001, "Mounted read only"                        ),
    ('I', 'FLUSH',     0x00000040, "Mounted with LFS_M_FLUSH"                 ),
    ('I', 'SYNC',      0x00000080, "Mounted with LFS_M_SYNC"                  ),
    ('I', 'REVDBG',    0x00000010, "Mounted with LFS_M_REVDBG"                ),
    ('I', 'REVNOISE',  0x00000020, "Mounted with LFS_M_REVNOISE"              ),
    ('I', 'CKPROGS',   0x00000800, "Mounted with LFS_M_CKPROGS"               ),
    ('I', 'CKFETCHES', 0x00001000, "Mounted with LFS_M_CKFETCHES"             ),
    ('I', 'CKPARITY',  0x00002000, "Mounted with LFS_M_CKPARITY"              ),
    ('I', 'CKDATACKSUMS', 
                       0x00008000, "Mounted with LFS_M_CKDATACKSUMS"          ),
 
    ('I', 'MKCONSISTENT',
                       0x00010000, "Filesystem needs mkconsistent to write"   ),
    ('I', 'LOOKAHEAD', 0x00020000, "Lookahead buffer is not full"             ),
    ('I', 'COMPACT',   0x00080000, "Filesystem may have uncompacted metadata" ),
    ('I', 'CKMETA',    0x00100000, "Metadata checksums not checked recently"  ),
    ('I', 'CKDATA',    0x00200000, "Data checksums not checked recently"      ),

    ('i', 'INMTREE',   0x01000000, "Committing to mtree"                      ),

    # Traversal flags
    ('T', 'MTREEONLY', 0x00000010, "Only traverse the mtree"                  ),
    ('T', 'MKCONSISTENT',
                       0x00010000, "Make the filesystem consistent"           ),
    ('T', 'LOOKAHEAD', 0x00020000, "Populate lookahead buffer"                ),
    ('T', 'COMPACT',   0x00080000, "Compact metadata logs"                    ),
    ('T', 'CKMETA',    0x00100000, "Check metadata checksums"                 ),
    ('T', 'CKDATA',    0x00200000, "Check metadata + data checksums"          ),

    ('t', 'TYPE',      0xf0000000, "The file's type"                          ),
    ('^', 'REG',       0x10000000, "Type = regular-file"                      ),
    ('^', 'DIR',       0x20000000, "Type = directory"                         ),
    ('^', 'STICKYNOTE',0x30000000, "Type = stickynote"                        ),
    ('^', 'BOOKMARK',  0x40000000, "Type = bookmark"                          ),
    ('^', 'ORPHAN',    0x50000000, "Type = orphan"                            ),
    ('^', 'TRAVERSAL', 0x60000000, "Type = traversal"                         ),
    ('^', 'UNKNOWN',   0x70000000, "Type = unknown"                           ),
    ('t', 'TSTATE',    0x0000000f, "The traversal's current tstate"           ),
    ('^', 'MROOTANCHOR',
                       0x00000000, "Tstate = mroot-anchor"                    ),
    ('^', 'MROOTCHAIN',0x00000001, "Tstate = mroot-chain"                     ),
    ('^', 'MTREE',     0x00000002, "Tstate = mtree"                           ),
    ('^', 'MDIRS',     0x00000003, "Tstate = mtree-mdirs"                     ),
    ('^', 'MDIR',      0x00000004, "Tstate = mdir"                            ),
    ('^', 'BTREE',     0x00000005, "Tstate = btree"                           ),
    ('^', 'OMDIRS',    0x00000006, "Tstate = open-mdirs"                      ),
    ('^', 'OBTREE',    0x00000007, "Tstate = open-btree"                      ),
    ('^', 'DONE',      0x00000008, "Tstate = done"                            ),
    ('t', 'BTYPE',     0x00000f00, "The traversal's current btype"            ),
    ('^', 'MDIR',      0x00000100, "Btype = mdir"                             ),
    ('^', 'BTREE',     0x00000200, "Btype = btree"                            ),
    ('^', 'DATA',      0x00000300, "Btype = data"                             ),
    ('t', 'DIRTY',     0x01000000, "Filesystem modified during traversal"     ),
    ('t', 'MUTATED',   0x02000000, "Filesystem modified by traversal"         ),
    ('t', 'ZOMBIE',    0x08000000, "File has been removed"                    ),

    # Read-compat flags
    ('RCOMPAT', 'NONSTANDARD',
                       0x00000001, "Non-standard filesystem format"           ),
    ('RCOMPAT', 'WRONLY',
                       0x00000002, "Reading is disallowed"                    ),
    ('RCOMPAT', 'BMOSS',
                       0x00000010, "Files may use inlined data"               ),
    ('RCOMPAT', 'BSPROUT',
                       0x00000020, "Files may use block pointers"             ),
    ('RCOMPAT', 'BSHRUB',
                       0x00000040, "Files may use inlined btrees"             ),
    ('RCOMPAT', 'BTREE',
                       0x00000080, "Files may use btrees"                     ),
    ('RCOMPAT', 'MMOSS',
                       0x00000100, "May use an inlined mdir"                  ),
    ('RCOMPAT', 'MSPROUT',
                       0x00000200, "May use an mdir pointer"                  ),
    ('RCOMPAT', 'MSHRUB',
                       0x00000400, "May use an inlined mtree"                 ),
    ('RCOMPAT', 'MTREE',
                       0x00000800, "May use an mdir btree"                    ),
    ('RCOMPAT', 'GRM',
                       0x00001000, "Global-remove in use"                     ),
    ('rcompat', 'OVERFLOW',
                       0x80000000, "Can't represent all flags"                ),

    # Write-compat flags
    ('WCOMPAT', 'NONSTANDARD',
                       0x00000001, "Non-standard filesystem format"           ),
    ('WCOMPAT', 'RDONLY',
                       0x00000002, "Writing is disallowed"                    ),
    ('WCOMPAT', 'DIR',
                       0x00000010, "Directory file types in use"              ),
    ('WCOMPAT', 'GCKSUM',
                       0x00001000, "Global-checksum in use"                   ),
    ('wcompat', 'OVERFLOW',
                       0x80000000, "Can't represent all flags"                ),

    # Optional-compat flags
    ('OCOMPAT', 'NONSTANDARD',
                       0x00000001, "Non-standard filesystem format"           ),
    ('ocompat', 'OVERFLOW',
                       0x80000000, "Can't represent all flags"                ),
]


def main(flags, *,
        list=False,
        all=False):
    list_ = list; del list
    all_ = all; del all

    # first compile prefixes
    prefixes = {}
    for a in ALIASES:
        for k in a:
            prefixes[k] = a
    for p, n, f, h in FLAGS:
        if p not in prefixes:
            prefixes[p] = {p}

    # only look at specific prefix?
    flags_ = []
    prefix = set()
    for f in flags:
        # accept prefix prefix
        if f.upper() in prefixes:
            prefix.update(prefixes[f.upper()])
        # accept LFS_+prefix prefix
        elif (f.upper().startswith('LFS_')
                and f.upper()[len('LFS_'):] in prefixes):
            prefix.update(prefixes[f.upper()[len('LFS_'):]])
        else:
            flags_.append(f)

    # filter by prefix
    flags__ = []
    types__ = co.defaultdict(lambda: set())
    for p, n, f, h in FLAGS:
        if p == '^':
            p = last_p
            t = last_t
            types__[p].add(t)
        else:
            t = None
            last_p = p
            last_t = f
        if not prefix or p.upper() in prefix:
            flags__.append((p, t, n, f, h))

    lines = []
    # list all known flags
    if list_:
        for p, t, n, f, h in flags__:
            if not all_ and (t is not None or p[0].islower()):
                continue
            lines.append(('LFS_%s_%s' % (p, n), '0x%08x' % f, h))

    # find flags by name or value
    else:
        for f_ in flags_:
            found = False
            # find by LFS_+prefix+_+name
            for p, t, n, f, h in flags__:
                if 'LFS_%s_%s' % (p, n) == f_.upper():
                    lines.append(('LFS_%s_%s' % (p, n), '0x%08x' % f, h))
                    found = True
            if found:
                continue
            # find by prefix+_+name
            for p, t, n, f, h in flags__:
                if '%s_%s' % (p, n) == f_.upper():
                    lines.append(('LFS_%s_%s' % (p, n), '0x%08x' % f, h))
                    found = True
            if found:
                continue
            # find by name
            for p, t, n, f, h in flags__:
                if n == f_.upper():
                    lines.append(('LFS_%s_%s' % (p, n), '0x%08x' % f, h))
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
                        lines.append(('LFS_%s_%s' % (p, n), '0x%08x' % f, h))
                        f___ &= ~f
                    # matches type?
                    elif t is not None and (f__ & t) == f:
                        lines.append(('LFS_%s_%s' % (p, n), '0x%08x' % f, h))
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
    class AppendFlags(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if getattr(namespace, 'flags', None) is None:
                namespace.flags = []
            if value is None:
                pass
            elif isinstance(value, str):
                namespace.flags.append(value)
            else:
                namespace.flags.extend(value)
    parser.add_argument(
            'prefix',
            nargs='?',
            action=AppendFlags,
            help="Flag prefix to consider, defaults to all flags.")
    parser.add_argument(
            'flags',
            nargs='*',
            action=AppendFlags,
            help="Flags or names of flags to decode.")
    parser.add_argument(
            '-l', '--list',
            action='store_true',
            help="List all known flags.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Also show internal flags and types.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
