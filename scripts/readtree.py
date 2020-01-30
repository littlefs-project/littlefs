#!/usr/bin/env python3

import struct
import sys
import json
import io
import itertools as it
from readmdir import Tag, MetadataPair

def popc(x):
    return bin(x).count('1')

def ctz(x):
    return len(bin(x)) - len(bin(x).rstrip('0'))

def dumpentries(args, mdir, f):
    for k, id_ in enumerate(mdir.ids):
        name = mdir[Tag('name', id_, 0)]
        struct_ = mdir[Tag('struct', id_, 0)]

        f.write("id %d %s %s" % (
            id_, name.typerepr(),
            json.dumps(name.data.decode('utf8'))))
        if struct_.is_('dirstruct'):
            f.write(" dir {%#x, %#x}" % struct.unpack(
                '<II', struct_.data[:8].ljust(8, b'\xff')))
        if struct_.is_('ctzstruct'):
            f.write(" ctz {%#x} size %d" % struct.unpack(
                '<II', struct_.data[:8].ljust(8, b'\xff')))
        if struct_.is_('inlinestruct'):
            f.write(" inline size %d" % struct_.size)
        f.write("\n")

        if args.data and struct_.is_('inlinestruct'):
            for i in range(0, len(struct_.data), 16):
                f.write("  %08x: %-47s  %-16s\n" % (
                    i, ' '.join('%02x' % c for c in struct_.data[i:i+16]),
                    ''.join(c if c >= ' ' and c <= '~' else '.'
                        for c in map(chr, struct_.data[i:i+16]))))
        elif args.data and struct_.is_('ctzstruct'):
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
            for i in range(0, min(len(data), 256)
                    if not args.no_truncate else len(data), 16):
                f.write("  %08x: %-47s  %-16s\n" % (
                    i, ' '.join('%02x' % c for c in data[i:i+16]),
                    ''.join(c if c >= ' ' and c <= '~' else '.'
                        for c in map(chr, data[i:i+16]))))

        for tag in mdir.tags:
            if tag.id==id_ and tag.is_('userattr'):
                f.write("id %d %s size %d\n" % (
                    id_, tag.typerepr(), tag.size))

                if args.data:
                    for i in range(0, len(tag.data), 16):
                        f.write("  %-47s  %-16s\n" % (
                            ' '.join('%02x' % c for c in tag.data[i:i+16]),
                            ''.join(c if c >= ' ' and c <= '~' else '.'
                                for c in map(chr, tag.data[i:i+16]))))

def main(args):
    with open(args.disk, 'rb') as f:
        dirs = []
        superblock = None
        gstate = b''
        mdirs = []
        cycle = False
        tail = (args.block1, args.block2)
        hard = False
        while True:
            for m in it.chain((m for d in dirs for m in d), mdirs):
                if set(m.blocks) == set(tail):
                    # cycle detected
                    cycle = m.blocks
            if cycle:
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

            # add to directories
            mdirs.append(mdir)
            if mdir.tail is None or not mdir.tail.is_('hardtail'):
                dirs.append(mdirs)
                mdirs = []

            if mdir.tail is None:
                break

            tail = struct.unpack('<II', mdir.tail.data)
            hard = mdir.tail.is_('hardtail')

    # find paths
    dirtable = {}
    for dir in dirs:
        dirtable[frozenset(dir[0].blocks)] = dir

    pending = [("/", dirs[0])]
    while pending:
        path, dir = pending.pop(0)
        for mdir in dir:
            for tag in mdir.tags:
                if tag.is_('dir'):
                    try:
                        npath = tag.data.decode('utf8')
                        dirstruct = mdir[Tag('dirstruct', tag.id, 0)]
                        nblocks = struct.unpack('<II', dirstruct.data)
                        nmdir = dirtable[frozenset(nblocks)]
                        pending.append(((path + '/' + npath), nmdir))
                    except KeyError:
                        pass

        dir[0].path = path.replace('//', '/')

    # dump tree
    if not args.superblock and not args.gstate and not args.mdirs:
        args.superblock = True
        args.gstate = True
        args.mdirs = True

    if args.superblock and superblock:
        print("superblock %s v%d.%d" % (
            json.dumps(superblock[0].data.decode('utf8')),
            struct.unpack('<H', superblock[1].data[2:2+2])[0],
            struct.unpack('<H', superblock[1].data[0:0+2])[0]))
        print(
            "  block_size %d\n"
            "  block_count %d\n"
            "  name_max %d\n"
            "  file_max %d\n"
            "  attr_max %d" % struct.unpack(
                '<IIIII', superblock[1].data[4:4+20].ljust(20, b'\xff')))

    if args.gstate and gstate:
        print("gstate 0x%s" % ''.join('%02x' % c for c in gstate))
        tag = Tag(struct.unpack('<I', gstate[0:4].ljust(4, b'\xff'))[0])
        blocks = struct.unpack('<II', gstate[4:4+8].ljust(8, b'\xff'))
        if tag.size:
            print("  orphans %d" % tag.size)
        if tag.type:
            print("  move dir {%#x, %#x} id %d" % (
                blocks[0], blocks[1], tag.id))

    if args.mdirs:
        for i, dir in enumerate(dirs):
            print("dir %s" % (json.dumps(dir[0].path)
                if hasattr(dir[0], 'path') else '(orphan)'))

            for j, mdir in enumerate(dir):
                print("mdir {%#x, %#x} rev %d%s" % (
                    mdir.blocks[0], mdir.blocks[1], mdir.rev,
                    ' (corrupted)' if not mdir else ''))

                f = io.StringIO()
                if args.tags:
                    mdir.dump_tags(f, truncate=not args.no_truncate)
                elif args.log:
                    mdir.dump_log(f, truncate=not args.no_truncate)
                elif args.all:
                    mdir.dump_all(f, truncate=not args.no_truncate)
                else:
                    dumpentries(args, mdir, f)

                lines = list(filter(None, f.getvalue().split('\n')))
                for k, line in enumerate(lines):
                    print("%s %s" % (
                        ' ' if j == len(dir)-1 else
                        'v' if k == len(lines)-1 else
                        '|',
                        line))

    if cycle:
        print("*** cycle detected! -> {%#x, %#x} ***" % (cycle[0], cycle[1]))

    if cycle:
        return 2
    elif not all(mdir for dir in dirs for mdir in dir):
        return 1
    else:
        return 0;

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
    parser.add_argument('-s', '--superblock', action='store_true',
        help="Show contents of the superblock.")
    parser.add_argument('-g', '--gstate', action='store_true',
        help="Show contents of global-state.")
    parser.add_argument('-m', '--mdirs', action='store_true',
        help="Show contents of metadata-pairs/directories.")
    parser.add_argument('-t', '--tags', action='store_true',
        help="Show metadata tags instead of reconstructing entries.")
    parser.add_argument('-l', '--log', action='store_true',
        help="Show tags in log.")
    parser.add_argument('-a', '--all', action='store_true',
        help="Show all tags in log, included tags in corrupted commits.")
    parser.add_argument('-d', '--data', action='store_true',
        help="Also show the raw contents of files/attrs/tags.")
    parser.add_argument('-T', '--no-truncate', action='store_true',
        help="Don't truncate large amounts of data.")
    sys.exit(main(parser.parse_args()))
