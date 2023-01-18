#!/usr/bin/env python3

import itertools as it
import math as m
import struct
import bisect

COLORS = [
    '34',   # blue
    '31',   # red
    '32',   # green
    '35',   # purple
    '33',   # yellow
    '36',   # cyan
]


def crc32c(data, crc=0):
    crc ^= 0xffffffff
    for b in data:
        crc ^= b
        for j in range(8):
            crc = (crc >> 1) ^ ((crc & 1) * 0x82f63b78)
    return 0xffffffff ^ crc

def fromleb128(data):
    word = 0
    for i, b in enumerate(data):
        word |= ((b & 0x7f) << 7*i)
        word &= 0xffffffff
        if not b & 0x80:
            return word, i+1
    return word, len(data)

def fromtag(data):
    tag, delta1 = fromleb128(data)
    size, delta2 = fromleb128(data[delta1:])
    return tag & 1, tag >> 1, size, delta1+delta2

def popc(x):
    return bin(x).count('1')

def xxd(data, width=16, crc=False):
    for i in range(0, len(data), width):
        yield '%-*s %-*s' % (
            3*width,
            ' '.join('%02x' % b for b in data[i:i+width]),
            width,
            ''.join(
                b if b >= ' ' and b <= '~' else '.'
                for b in map(chr, data[i:i+width])))

def tagrepr(tag, size, off=None):
    type = tag & 0x7fff
    suptype = tag & 0x7807
    subtype = (tag >> 3) & 0xff
    id = ((tag >> 15) & 0xffff) - 1

    if suptype == 0x0800:
        return 'mk%s id%d%s' % (
            'reg' if subtype == 0
                else ' 0x%02x' % subtype,
            id,
            ' %d' % size if not tag & 0x1 else '')
    elif suptype == 0x0801:
        return 'rm%s id%d%s' % (
            ' 0x%02x' % subtype if subtype else '',
            id,
            ' %d' % size if not tag & 0x1 else '')
    elif (suptype & ~0x1) == 0x1000:
        return '%suattr 0x%02x%s%s' % (
            'rm' if suptype & 0x1 else '',
            subtype,
            ' id%d' % id if id != -1 else '',
            ' %d' % size if not tag & 0x1 else '')
    elif (suptype & ~0x1) == 0x0002:
        return 'crc%x%s %d' % (
            suptype & 0x1,
            ' 0x%02x' % subtype if subtype else '',
            size)
    elif suptype == 0x0802:
        return 'fcrc%s %d' % (
            ' 0x%02x' % subtype if subtype else '',
            size)
    elif suptype & 0x4:
        return 'alt%s%s 0x%x %s' % (
            'r' if suptype & 0x1 else 'b',
            'gt' if suptype & 0x2 else 'lt',
            tag & ~0x7,
            '0x%x' % (0xffffffff & (off-size))
                if off is not None
                else '-%d' % off)
    else:
        return '0x%02x id%d %d' % (type, id, size)

def show_log(block_size, data, rev, off, *,
        color=False,
        **args):
    crc = crc32c(data[0:4])

    # preprocess jumps
    if args.get('jumps'):
        jumps = []
        j_ = 4
        while j_ < (block_size if args.get('all') else off):
            j = j_
            v, tag, size, delta = fromtag(data[j_:])
            j_ += delta
            if not tag & 0x4:
                j_ += size

            if tag & 0x4:
                # figure out which alt color
                if tag & 0x1:
                    _, ntag, _, _ = fromtag(data[j_:])
                    if ntag & 0x1:
                        jumps.append((j, j-size, 0, 'y'))
                    else:
                        jumps.append((j, j-size, 0, 'r'))
                else:
                    jumps.append((j, j-size, 0, 'b'))

        # figure out x-offsets to avoid collisions between jumps
        for j in range(len(jumps)):
            a, b, _, c = jumps[j]
            x = 0
            while any(
                    max(a, b) >= min(a_, b_)
                        and max(a_, b_) >= min(a, b)
                        and x == x_
                    for a_, b_, x_, _ in jumps[:j]):
                x += 1
            jumps[j] = a, b, x, c

        def jumprepr(j):
            # render jumps
            chars = {}
            for a, b, x, c in jumps:
                c_start = (
                    '\x1b[33m' if color and c == 'y'
                    else '\x1b[31m' if color and c == 'r'
                    else '\x1b[90m' if color
                    else '')
                c_stop = '\x1b[m' if color else ''

                if j == a:
                    for x_ in range(2*x+1):
                        chars[x_] = '%s-%s' % (c_start, c_stop)
                    chars[2*x+1] = '%s\'%s' % (c_start, c_stop)
                elif j == b:
                    for x_ in range(2*x+1):
                        chars[x_] = '%s-%s' % (c_start, c_stop)
                    chars[2*x+1] = '%s.%s' % (c_start, c_stop)
                    chars[0] = '%s<%s' % (c_start, c_stop)
                elif j >= min(a, b) and j <= max(a, b):
                    chars[2*x+1] = '%s|%s' % (c_start, c_stop)

            return ''.join(chars.get(x, ' ')
                for x in range(max(chars.keys(), default=0)+1))

    # preprocess lifetimes
    if args.get('lifetimes'):
        count = 0
        max_count = 0
        lifetimes = {}
        ids = []
        ids_i = 0
        j_ = 4
        while j_ < (block_size if args.get('all') else off):
            j = j_
            v, tag, size, delta = fromtag(data[j_:])
            j_ += delta
            if not tag & 0x4:
                j_ += size

            if (tag & 0x7807) == 0x0800:
                count += 1
                max_count = max(max_count, count)
                ids.insert(((tag >> 15) & 0xffff)-1,
                    COLORS[ids_i % len(COLORS)])
                ids_i += 1
                lifetimes[j] = (
                    ''.join(
                        '%s%s%s' % (
                            '\x1b[%sm' % ids[id] if color else '',
                            '.' if id == ((tag >> 15) & 0xffff)-1
                                else '\ ' if id > ((tag >> 15) & 0xffff)-1
                                else '| ',
                            '\x1b[m' if color else '')
                            for id in range(count))
                        + ' ',
                    count)
            elif ((tag & 0x7807) == 0x0801
                    and ((tag >> 15) & 0xffff)-1 < len(ids)):
                lifetimes[j] = (
                    ''.join(
                        '%s%s%s' % (
                            '\x1b[%sm' % ids[id] if color else '',
                            '\'' if id == ((tag >> 15) & 0xffff)-1
                                else '/ ' if id > ((tag >> 15) & 0xffff)-1
                                else '| ',
                            '\x1b[m' if color else '')
                            for id in range(count))
                        + ' ',
                    count)
                count -= 1
                ids.pop(((tag >> 15) & 0xffff)-1)
            else:
                lifetimes[j] = (
                    ''.join(
                        '%s%s%s' % (
                            '\x1b[%sm' % ids[id] if color else '',
                            '* ' if not tag & 0x4
                                and id == ((tag >> 15) & 0xffff)-1
                                else '| ',
                            '\x1b[m' if color else '')
                            for id in range(count)),
                    count)

        def lifetimerepr(j):
            lifetime, count = lifetimes.get(j, ('', 0))
            return '%s%*s' % (lifetime, 2*(max_count-count), '')

    # prepare other things
    if args.get('rbyd'):
        alts = []

    # print header
    print('%-8s  %s%-22s  %s' % (
        'off',
        lifetimerepr(0) if args.get('lifetimes') else '',
        'tag',
        'data (truncated)'
            if not args.get('no_truncate') else ''))

    # print revision count
    if args.get('raw'):
        print('%8s: %s' % ('%04x' % 0, next(xxd(data[0:4]))))

    # print tags
    j_ = 4
    while j_ < (block_size if args.get('all') else off):
        notes = []

        j = j_
        v, tag, size, delta = fromtag(data[j_:])
        if v != popc(crc) & 1:
            notes.append('v!=%x' % (popc(crc) & 1))
        crc = crc32c(data[j_:j_+delta], crc)
        j_ += delta

        if not tag & 0x4:
            if (tag & 0x7806) != 0x0002:
                crc = crc32c(data[j_:j_+size], crc)
            # found a crc?
            else:
                crc_, = struct.unpack('<I', data[j_:j_+4].ljust(4, b'\0'))
                if crc != crc_:
                    notes.append('crc!=%08x' % crc)
            j_ += size

        # adjust count?
        if args.get('lifetimes'):
            if (tag & 0x7807) == 0x0800:
                count += 1
            elif ((tag & 0x7807) == 0x0801
                    and ((tag >> 15) & 0xffff)-1 < len(ids)):
                count -= 1

        if not args.get('in_tree') or (tag & 0x6) == 0:
            # show human-readable tag representation
            print('%s%08x:%s %s%s%-57s%s%s' % (
                '\x1b[90m' if color and j >= off else '',
                j,
                '\x1b[m' if color and j >= off else '',
                lifetimerepr(j) if args.get('lifetimes') else '',
                '\x1b[90m' if color and j >= off else '',
                '%-22s%s' % (
                    tagrepr(tag, size, j),
                    '  %s' % next(xxd(
                            data[j+delta:j+delta+min(size, 8)], 8), '')
                        if not args.get('no_truncate')
                            and not tag & 0x4 else ''),
                '\x1b[m' if color and j >= off else '',
                ' (%s)' % ', '.join(notes) if notes
                else ' %s' % ''.join(
                        ('\x1b[33my\x1b[m' if color else 'y')
                            if alts[i] & 0x1
                            and i+1 < len(alts)
                            and alts[i+1] & 0x1
                        else ('\x1b[31mr\x1b[m' if color else 'r')
                            if alts[i] & 0x1
                        else ('\x1b[90mb\x1b[m' if color else 'b')
                        for i in range(len(alts)-1, -1, -1))
                    if args.get('rbyd') and (tag & 0x7) == 0
                else ' %s' % jumprepr(j)
                    if args.get('jumps')
                else ''))

        if not args.get('in_tree') or (tag & 0x6) != 2:
            if args.get('raw'):
                # show on-disk encoding of tags
                for o, line in enumerate(xxd(data[j:j+delta])):
                    print('%s%8s: %s%s' % (
                        '\x1b[90m' if color and j >= off else '',
                        '%04x' % (j + o*16),
                        line,
                        '\x1b[m' if color and j >= off else ''))

            # show in-device representation, including some extra
            # crc/parity info
            if args.get('device'):
                print('%s%8s  %s%-47s  %08x %x%s' % (
                    '\x1b[90m' if color and j >= off else '',
                    '',
                    lifetimerepr(0) if args.get('lifetimes') else '',
                    '%-22s%s' % (
                        '%08x %08x' % (tag, size),
                        '  %s' % ' '.join(
                                '%08x' % struct.unpack('<I',
                                    data[j+delta+i*4:j+delta+i*4+4])
                                for i in range(min(size//4, 3)))[:23]
                            if not tag & 0x4 else ''),
                    crc,
                    popc(crc) & 1,
                    '\x1b[m' if color and j >= off else ''))

        if not tag & 0x4 and (not args.get('in_tree') or (tag & 0x6) != 2):
            # show on-disk encoding of data
            if args.get('raw') or args.get('no_truncate'):
                for o, line in enumerate(xxd(data[j+delta:j+delta+size])):
                    print('%s%8s: %s%s' % (
                        '\x1b[90m' if color and j >= off else '',
                        '%04x' % (j+delta + o*16),
                        line,
                        '\x1b[m' if color and j >= off else ''))

        if args.get('rbyd'):
            if tag & 0x4:
                alts.append(tag)
            else:
                alts = []


def show_tree(block_size, data, rev, trunk, count, *,
        color=False,
        **args):
    if trunk is None:
        return

    # lookup a tag, returning also the search path for decoration
    # purposes
    def lookup(tag):
        lower = 0
        upper = (count+1) << 15
        path = []

        # descend down tree
        j = trunk
        while True:
            _, alt, jump, delta = fromtag(data[j:])

            # founat alt?
            if alt & 0x4:
                # follow?
                if (tag >= upper - (alt & ~0x7)
                        if alt & 0x2
                        else tag < lower + (alt & ~0x7)):
                    lower += (upper-lower)-(alt & ~0x7) if alt & 0x2 else 0
                    upper -= (upper-lower)-(alt & ~0x7) if not alt & 0x2 else 0
                    j = j - jump

                    if args.get('rbyd') or args.get('tree'):
                        # figure out which color
                        if alt & 0x1:
                            _, nalt, _, _ = fromtag(data[j+jump+delta:])
                            if nalt & 0x1:
                                path.append((j+jump, j, 'y'))
                            else:
                                path.append((j+jump, j, 'r'))
                        else:
                            path.append((j+jump, j, 'b'))
                # stay on path
                else:
                    lower += (alt & ~0x7) if not alt & 0x2 else 0
                    upper -= (alt & ~0x7) if alt & 0x2 else 0
                    j = j + delta

                    if args.get('rbyd') or args.get('tree'):
                        # figure out which color
                        if alt & 0x1:
                            _, nalt, _, _ = fromtag(data[j:])
                            if nalt & 0x1:
                                path.append((j-delta, j, 'y'))
                            else:
                                path.append((j-delta, j, 'r'))
                        else:
                            path.append((j-delta, j, 'b'))
            # found tag
            else:
                tag_ = (((upper-0x8) & ~0x7fff)
                    | (alt & 0x7fff))
                return tag_, j, delta, jump, path

    # precompute tree
    if args.get('tree'):
        tags = []
        paths = {}

        tag_ = 0
        while True:
            tag, j, delta, size, path = lookup(tag_)
            # found end of tree?
            if tag < tag_ or tag & 1:
                break
            tag_ = tag + 0x8

            tags.append((j, tag))
            for x, (a, b, c) in enumerate(path):
                paths[a, b, x] = c

        # align paths to nearest tag
        tags.sort()
        paths = {(
            tags[bisect.bisect_left(tags, (a, 0), hi=len(tags)-1)],
            tags[bisect.bisect_left(tags, (b, 0), hi=len(tags)-1)],
            x): c for (a, b, x), c in paths.items()}

        # also find the maximum depth
        depth = max((x+1 for _, _, x in paths.keys()), default=0)

        def treerepr(j):
            if depth == 0:
                return ''

            _, tag = tags[bisect.bisect_left(tags, (j, 0), hi=len(tags)-1)]

            def c_start(c):
                return ('\x1b[33m' if color and c == 'y'
                    else '\x1b[31m' if color and c == 'r'
                    else '\x1b[90m' if color
                    else '')

            def c_stop(c):
                return '\x1b[m' if color else ''

            path = []
            seen = None
            for x in range(depth):
                if any(x == x_ and tag == a_tag
                        for (_, a_tag), _, x_ in paths.keys()):
                    c = next(c
                        for ((_, a_tag), _, x_), c in paths.items()
                        if x == x_ and tag == a_tag)
                    path.append('%s+%s' % (c_start(c), c_stop(c)))
                elif any(x == x_ and tag == b_tag
                        for _, (_, b_tag), x_ in paths.keys()):
                    a_tag, c = next((a_tag, c)
                        for ((_, a_tag), (_, b_tag), x_), c in paths.items()
                        if x == x_ and tag == b_tag)
                    if a_tag < tag:
                        path.append('%s\'%s' % (c_start(c), c_stop(c)))
                    else:
                        path.append('%s.%s' % (c_start(c), c_stop(c)))
                elif any(x == x_
                        and tag >= min(a_tag, b_tag)
                        and tag <= max(a_tag, b_tag)
                        for (_, a_tag), (_, b_tag), x_ in paths.keys()):
                    c = next(c
                        for ((_, a_tag), (_, b_tag), x_), c in paths.items()
                        if x == x_
                            and tag >= min(a_tag, b_tag)
                            and tag <= max(a_tag, b_tag))
                    path.append('%s|%s' % (c_start(c), c_stop(c)))
                elif seen:
                    path.append('%s-%s' % (c_start(seen), c_stop(seen)))
                else:
                    path.append(' ')

                if any(x == x_ and tag == b_tag
                        for _, (_, b_tag), x_ in paths.keys()):
                    c = next(c
                        for (_, (_, b_tag), x_), c in paths.items()
                        if x == x_ and tag == b_tag)
                    seen = c

                if seen and x == depth-1:
                    path.append('%s>%s' % (c_start(seen), c_stop(seen)))
                elif seen:
                    path.append('%s-%s' % (c_start(seen), c_stop(seen)))
                else:
                    path.append(' ')

            return ' %s' % ''.join(path)

    # print header
    print('%-8s  %*s%-22s  %s' % (
        'off',
        2*depth+1 if args.get('tree') and depth > 0 else 0, '',
        'tag',
        'data (truncated)'
            if not args.get('no_truncate') else ''))

    tag_ = 0
    while True:
        tag, j, delta, size, path = lookup(tag_)
        # found end of tree?
        if tag < tag_ or tag & 1:
            break
        tag_ = tag + 0x8

        # show human-readable tag representation
        print('%08x:%s %-57s%s' % (
            j,
            treerepr(j) if args.get('tree') else '',
            '%-22s%s' % (
                tagrepr(tag, size, j),
                '  %s' % next(xxd(
                        data[j+delta:j+delta+min(size, 8)], 8), '')
                    if not args.get('no_truncate')
                        and not tag & 0x4 else ''),
            ' %s' % ''.join(
                    ('\x1b[33my\x1b[m' if color else 'y')
                        if path[i][2] == 'y'
                    else ('\x1b[31mr\x1b[m' if color else 'r')
                        if path[i][2] == 'r'
                    else ('\x1b[90mb\x1b[m' if color else 'b')
                    for i in range(len(path)-1, -1, -1))
                if args.get('rbyd') else ''))

        if args.get('raw'):
            # show on-disk encoding of tags
            for o, line in enumerate(xxd(data[j:j+delta])):
                print('%8s: %s' % (
                    '%04x' % (j + o*16),
                    line))

        # show in-device representation
        if args.get('device'):
            print('%8s  %*s%-47s' % (
                '',
                2*depth+1 if args.get('tree') and depth > 0 else 0, '',
                '%-22s%s' % (
                    '%08x %08x' % (tag, size),
                    '  %s' % ' '.join(
                            '%08x' % struct.unpack('<I',
                                data[j+delta+i*4:j+delta+i*4+4])
                            for i in range(min(size//4, 3)))[:23]
                        if not tag & 0x4 else '')))

        # show on-disk encoding of data
        if args.get('raw') or args.get('no_truncate'):
            for o, line in enumerate(xxd(data[j+delta:j+delta+size])):
                print('%8s: %s' % (
                    '%04x' % (j+delta + o*16),
                    line))


def main(disk, block_size, block1, block2=None, *,
        trunk=None,
        color='auto',
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # read each block
    blocks = [block for block in [block1, block2] if block is not None]
    with open(disk, 'rb') as f:
        datas = []
        for block in blocks:
            f.seek(block * block_size)
            datas.append(f.read(block_size))

    # first figure out which block as the most recent revision
    def fetch(data):
        rev, = struct.unpack('<I', data[0:4].ljust(4, b'\0'))
        crc = crc32c(data[0:4])
        off = 0
        j_ = 4
        trunk = None
        trunk_ = None
        count = 0
        count_ = 0
        wastrunk = False
        while j_ < block_size:
            v, tag, size, delta = fromtag(data[j_:])
            if v != popc(crc) & 1:
                break
            crc = crc32c(data[j_:j_+delta], crc)
            j_ += delta

            if not wastrunk and (tag & 0x6) != 0x2:
                trunk_ = j_ - delta
                wastrunk = True

            if not tag & 0x4:
                if (tag & 0x7806) != 0x0002:
                    crc = crc32c(data[j_:j_+size], crc)
                    # keep track of id count
                    if (tag & 0x7807) == 0x0800:
                        count_ += 1
                    elif (tag & 0x7807) == 0x0801:
                        count_ = max(count_ - 1, 0)
                # found a crc?
                else:
                    crc_, = struct.unpack('<I', data[j_:j_+4].ljust(4, b'\0'))
                    if crc != crc_:
                        break
                    # commit what we have
                    off = j_ + size
                    trunk = trunk_
                    count = count_
                j_ += size
                wastrunk = False

        return rev, off, trunk, count

    revs, offs, trunks, counts = [], [], [], []
    i = 0
    for block, data in zip(blocks, datas):
        rev, off, trunk_, count = fetch(data)
        revs.append(rev)
        offs.append(off)
        trunks.append(trunk_)
        counts.append(count)

        # compare with sequence arithmetic
        if off and ((rev - revs[i]) & 0x80000000):
            i = len(revs)-1

    # print contents of the winning metadata block
    block, data, rev, off, trunk, count = (
        blocks[i], datas[i], revs[i], offs[i],
        trunk if trunk is not None else trunks[i],
        counts[i])

    print('mdir 0x%x, rev %d, size %d%s' % (
        block, rev, off,
        ' (was 0x%x, %d, %d)' % (blocks[~i], revs[~i], offs[~i])
            if len(blocks) > 1 else ''))

    if args.get('log'):
        show_log(block_size, data, rev, off,
            color=color,
            **args)
    else:
        show_tree(block_size, data, rev, trunk, count,
            color=color,
            **args)

    if args.get('error_on_corrupt') and off == 0:
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Debug rbyd metadata.",
        allow_abbrev=False)
    parser.add_argument(
        'disk',
        help="File containing the block device.")
    parser.add_argument(
        'block_size',
        type=lambda x: int(x, 0),
        help="Block size in bytes.")
    parser.add_argument(
        'block1',
        type=lambda x: int(x, 0),
        help="Block address of the first metadata block.")
    parser.add_argument(
        'block2',
        nargs='?',
        type=lambda x: int(x, 0),
        help="Block address of the second metadata block.")
    parser.add_argument(
        '--trunk',
        type=lambda x: int(x, 0),
        help="Use this offset as the trunk of the tree.")
    parser.add_argument(
        '--color',
        choices=['never', 'always', 'auto'],
        default='auto',
        help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
        '-a', '--all',
        action='store_true',
        help="Don't stop parsing on bad commits.")
    parser.add_argument(
        '-l', '--log',
        action='store_true',
        help="Show the raw tags as they appear in the log.")
    parser.add_argument(
        '-i', '--in-tree',
        action='store_true',
        help="Only show tags in the tree.")
    parser.add_argument(
        '-r', '--raw',
        action='store_true',
        help="Show the raw data including tag encodings.")
    parser.add_argument(
        '-x', '--device',
        action='store_true',
        help="Show the device-side representation of tags.")
    parser.add_argument(
        '-T', '--no-truncate',
        action='store_true',
        help="Don't truncate, show the full contents.")
    parser.add_argument(
        '-y', '--rbyd', 
        action='store_true',
        help="Show the rbyd tree in the margin.")
    parser.add_argument(
        '-t', '--tree',
        action='store_true',
        help="Show the rbyd tree.")
    parser.add_argument(
        '-j', '--jumps',
        action='store_true',
        help="Show alt pointer jumps in the margin.")
    parser.add_argument(
        '-g', '--lifetimes',
        action='store_true',
        help="Show inserts/deletes of ids in the margin.")
    parser.add_argument(
        '-e', '--error-on-corrupt',
        action='store_true',
        help="Error if no valid commit is found.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
