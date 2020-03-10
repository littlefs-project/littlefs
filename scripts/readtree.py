#!/usr/bin/env python3

import struct
import sys
import json
import io
import itertools as it
import collections as c
from readmdir import Tag, MetadataPair

def popc(x):
    return bin(x).count('1')

def ctz(x):
    return len(bin(x)) - len(bin(x).rstrip('0'))

def dumpentries(args, mdir, mdirs, f):
    for k, id_ in enumerate(mdir.ids):
        name = mdir[Tag('name', id_, 0)]
        struct_ = mdir[Tag('struct', id_, 0)]

        desc = "id %d %s %s" % (
            id_, name.typerepr(),
            json.dumps(name.data.decode('utf8')))
        if struct_.is_('dirstruct'):
            pair = struct.unpack('<II', struct_.data[:8].ljust(8, b'\xff'))
            desc += " dir {%#x, %#x}%s" % (
                pair[0], pair[1],
                '?' if frozenset(pair) not in mdirs else '')
        if struct_.is_('ctzstruct'):
            desc += " ctz {%#x} size %d" % struct.unpack(
                '<II', struct_.data[:8].ljust(8, b'\xff'))
        if struct_.is_('inlinestruct'):
            desc += " inline size %d" % struct_.size

        data = None
        if struct_.is_('inlinestruct'):
            data = struct_.data
        elif struct_.is_('ctzstruct'):
            block, size = struct.unpack(
                '<II', struct_.data[:8].ljust(8, b'\xff'))
            data = []
            i = 0 if size == 0 else (size-1) // (args.block_size - 8)
            if i != 0:
                i = ((size-1) - 4*popc(i-1)+2) // (args.block_size - 8)
            with open(args.disk, 'rb') as f2:
                while i >= 0:
                    f2.seek(block * args.block_size)
                    dat = f2.read(args.block_size)
                    data.append(dat[4*(ctz(i)+1) if i != 0 else 0:])
                    block, = struct.unpack('<I', dat[:4].ljust(4, b'\xff'))
                    i -= 1
            data = bytes(it.islice(
                it.chain.from_iterable(reversed(data)), size))

        f.write("%-45s%s\n" % (desc,
            "%-23s  %-8s" % (
                ' '.join('%02x' % c for c in data[:8]),
                ''.join(c if c >= ' ' and c <= '~' else '.'
                    for c in map(chr, data[:8])))
            if not args.no_truncate and len(desc) < 45
                and data is not None else ""))

        if name.is_('superblock') and struct_.is_('inlinestruct'):
            f.write(
                "  block_size %d\n"
                "  block_count %d\n"
                "  name_max %d\n"
                "  file_max %d\n"
                "  attr_max %d\n" % struct.unpack(
                    '<IIIII', struct_.data[4:4+20].ljust(20, b'\xff')))

        for tag in mdir.tags:
            if tag.id==id_ and tag.is_('userattr'):
                desc = "%s size %d" % (tag.typerepr(), tag.size)
                f.write("  %-43s%s\n" % (desc,
                    "%-23s  %-8s" % (
                        ' '.join('%02x' % c for c in tag.data[:8]),
                        ''.join(c if c >= ' ' and c <= '~' else '.'
                            for c in map(chr, tag.data[:8])))
                    if not args.no_truncate and len(desc) < 43 else ""))

                if args.no_truncate:
                    for i in range(0, len(tag.data), 16):
                        f.write("    %08x: %-47s  %-16s\n" % (
                            i, ' '.join('%02x' % c for c in tag.data[i:i+16]),
                            ''.join(c if c >= ' ' and c <= '~' else '.'
                                for c in map(chr, tag.data[i:i+16]))))

        if args.no_truncate and data is not None:
            for i in range(0, len(data), 16):
                f.write("  %08x: %-47s  %-16s\n" % (
                    i, ' '.join('%02x' % c for c in data[i:i+16]),
                    ''.join(c if c >= ' ' and c <= '~' else '.'
                        for c in map(chr, data[i:i+16]))))

def main(args):
    superblock = None
    gstate = b'\0\0\0\0\0\0\0\0\0\0\0\0'
    mdirs = c.OrderedDict()
    corrupted = []
    cycle = False
    with open(args.disk, 'rb') as f:
        tail = (args.block1, args.block2)
        while tail:
            if frozenset(tail) in mdirs:
                # cycle detected
                cycle = tail
                break

            # load mdir
            data = []
            blocks = {}
            for block in tail:
                f.seek(block * args.block_size)
                data.append(f.read(args.block_size)
                    .ljust(args.block_size, b'\xff'))
                blocks[id(data[-1])] = block

            mdir = MetadataPair(data)
            mdir.blocks = tuple(blocks[id(p.data)] for p in mdir.pair)

            # fetch some key metadata as a we scan
            try:
                mdir.tail = mdir[Tag('tail', 0, 0)]
                if mdir.tail.size != 8 or mdir.tail.data == 8*b'\xff':
                    mdir.tail = None
            except KeyError:
                mdir.tail = None

            try:
                mdir.branch = mdir[Tag('branch', 0, 0)]
                if mdir.branch.size != 8 or mdir.branch.data == 8*b'\xff':
                    mdir.branch = None
            except KeyError:
                mdir.branch = None

            # have superblock?
            try:
                nsuperblock = mdir[
                    Tag(0x7ff, 0x3ff, 0), Tag('superblock', 0, 0)]
                superblock = nsuperblock, mdir[Tag('inlinestruct', 0, 0)]
            except KeyError:
                pass

            # have gstate?
            try:
                ngstate = mdir[Tag('movestate', 0, 0)]
                gstate = bytes((a or 0) ^ (b or 0)
                    for a,b in it.zip_longest(gstate, ngstate.data))
            except KeyError:
                pass

            # corrupted?
            if not mdir:
                corrupted.append(mdir)

            # add to metadata-pairs
            mdirs[frozenset(mdir.blocks)] = mdir
            tail = (struct.unpack('<II', mdir.tail.data)
                if mdir.tail else None)

    # derive paths and build directories
    dirs = {}
    rogue = {}
    pending = [('/', (args.block1, args.block2))]
    while pending:
        path, branch = pending.pop(0)
        dir = []
        while branch and frozenset(branch) in mdirs:
            mdir = mdirs[frozenset(branch)]
            dir.append(mdir)

            for tag in mdir.tags:
                if tag.is_('dir'):
                    try:
                        npath = path + '/' + tag.data.decode('utf8')
                        npath = npath.replace('//', '/')
                        dirstruct = mdir[Tag('dirstruct', tag.id, 0)]
                        npair = struct.unpack('<II', dirstruct.data)
                        pending.append((npath, npair))
                    except KeyError:
                        pass

            branch = (struct.unpack('<II', mdir.branch.data)
                if mdir.branch else None)

        if not dir:
            rogue[path] = branch
        else:
            dirs[path] = dir

    # also find orphans
    not_orphans = {frozenset(mdir.blocks)
        for dir in dirs.values()
        for mdir in dir}
    orphans = []
    for pair, mdir in mdirs.items():
        if pair not in not_orphans:
            if len(orphans) > 0 and (pair == frozenset(
                    struct.unpack('<II', orphans[-1][-1].tail.data))):
                orphans[-1].append(mdir)
            else:
                orphans.append([mdir])

    # print littlefs + version info
    version = ('?', '?')
    if superblock:
        version = tuple(reversed(
            struct.unpack('<HH', superblock[1].data[0:4].ljust(4, b'\xff'))))
    print("%-47s%s" % ("littlefs v%s.%s" % version,
        "data (truncated, if it fits)"
        if not any([args.no_truncate, args.tags, args.log, args.all]) else ""))

    # print gstate
    badgstate = None
    print("gstate 0x%s" % ''.join('%02x' % c for c in gstate))
    tag = Tag(struct.unpack('<I', gstate[0:4].ljust(4, b'\xff'))[0])
    blocks = struct.unpack('<II', gstate[4:4+8].ljust(8, b'\xff'))
    if tag.size or not tag.isvalid:
        print("  orphans >=%d" % max(tag.size, 1))
    if tag.type:
        if frozenset(blocks) not in mdirs:
            badgstate = gstate
        print("  move dir {%#x, %#x}%s id %d" % (
            blocks[0], blocks[1],
            '?' if frozenset(blocks) not in mdirs else '',
            tag.id))

    # print dir info
    for path, dir in it.chain(
            sorted(dirs.items()),
            zip(it.repeat(None), orphans)):
        print("dir %s" % json.dumps(path) if path else "orphaned")

        for j, mdir in enumerate(dir):
            print("mdir {%#x, %#x} rev %d (was %d)%s%s" % (
                mdir.blocks[0], mdir.blocks[1], mdir.rev, mdir.pair[1].rev,
                ' (corrupted!)' if not mdir else '',
                ' -> {%#x, %#x}' % struct.unpack('<II', mdir.tail.data)
                if mdir.tail else ''))

            f = io.StringIO()
            if args.tags:
                mdir.dump_tags(f, truncate=not args.no_truncate)
            elif args.log:
                mdir.dump_log(f, truncate=not args.no_truncate)
            elif args.all:
                mdir.dump_all(f, truncate=not args.no_truncate)
            else:
                dumpentries(args, mdir, mdirs, f)

            lines = list(filter(None, f.getvalue().split('\n')))
            for k, line in enumerate(lines):
                print("%s %s" % (
                    ' ' if j == len(dir)-1 else
                    'v' if k == len(lines)-1 else
                    '|' if path else '.',
                    line))

    errcode = 0
    for mdir in corrupted:
        errcode = errcode or 1
        print("*** corrupted mdir {%#x, %#x}! ***" % (
            mdir.blocks[0], mdir.blocks[1]))

    for path, pair in rogue.items():
        errcode = errcode or 2
        print("*** couldn't find dir %s {%#x, %#x}! ***" % (
            json.dumps(path), pair[0], pair[1]))

    if badgstate:
        errcode = errcode or 3
        print("*** bad gstate 0x%s! ***" %
            ''.join('%02x' % c for c in gstate))

    if cycle:
        errcode = errcode or 4
        print("*** cycle detected {%#x, %#x}! ***" % (
            cycle[0], cycle[1]))

    return errcode

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Dump semantic info about the metadata tree in littlefs")
    parser.add_argument('disk',
        help="File representing the block device.")
    parser.add_argument('block_size', type=lambda x: int(x, 0),
        help="Size of a block in bytes.")
    parser.add_argument('block1', nargs='?', default=0,
        type=lambda x: int(x, 0),
        help="Optional first block address for finding the root.")
    parser.add_argument('block2', nargs='?', default=1,
        type=lambda x: int(x, 0),
        help="Optional second block address for finding the root.")
    parser.add_argument('-t', '--tags', action='store_true',
        help="Show metadata tags instead of reconstructing entries.")
    parser.add_argument('-l', '--log', action='store_true',
        help="Show tags in log.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all tags in log, included tags in corrupted commits.")
    parser.add_argument('-T', '--no-truncate', action='store_true',
        help="Show the full contents of files/attrs/tags.")
    sys.exit(main(parser.parse_args()))
