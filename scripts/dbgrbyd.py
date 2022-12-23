#!/usr/bin/env python3

import itertools as it
import math as m
import struct


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
    type1 = tag & 0x7f
    type2 = (tag >> 7) & 0xff
    id = (tag >> 15) & 0xffff

    if type1 == 0x40:
        return 'create x%02x id%d %d' % (type2, id, size)
    elif type1 == 0x48:
        return 'delete x%02x id%d %d' % (type2, id, size)
    elif type1 == 0x50:
        return 'struct x%02x id%d %d' % (type2, id, size)
    elif type1 == 0x60:
        return 'uattr x%02x id%d %d' % (type2, id, size)
    elif type1 == 0x08:
        if type2 == 0:
            return 'tail %d' % size
        else:
            return 'tail x%02x %d' % (type2, size)
    elif type1 == 0x10:
        return 'gstate x%02x %d' % (type2, size)
    elif (type1 & 0x7e) == 0x02:
        if type2 == 0:
            return 'crc%x %d' % (type1 >> 3, size)
        else:
            return 'crc%x x%02x %d' % (type1 >> 3, type2, size)
    elif type1 == 0x0a:
        if type2 == 0:
            return 'fcrc %d' % (size)
        else:
            return 'fcrc x%02x %d' % (type2, size)
    elif type1 & 0x4:
        return 'alt%s%s x%x %s' % (
            'r' if type1 & 1 else 'b',
            'gt' if type1 & 2 else 'lt',
            tag >> 3,
            'x%x' % (0xffffffff & (off-size))
                if off is not None
                else '-%d' % off)
    else:
        return 'x%02x x%02x id%d %d' % (type1, type2, id, size)


def main(disk, block_size, block1, block2=None, **args):
    # read each block
    blocks = [block for block in [block1, block2] if block is not None]
    with open(disk, 'rb') as f:
        datas = []
        for block in blocks:
            f.seek(block * block_size)
            datas.append(f.read(block_size))

    # first figure out which block as the most recent revision
    def fetch(data):
        rev, = struct.unpack('<I', data[0:4])
        crc = crc32c(data[0:4])
        off = 0
        j = 4
        while j < block_size:
            v, tag, size, delta = fromtag(data[j:])
            if v != popc(crc) & 1:
                break
            crc = crc32c(data[j:j+delta], crc)
            j += delta

            if not tag & 0x4:
                if (tag & 0x7e) != 0x2:
                    crc = crc32c(data[j:j+size], crc)
                # found a crc?
                else:
                    crc_, = struct.unpack('<I', data[j:j+4])
                    if crc != crc_:
                        break
                    # commit what we have
                    off = j + size
                j += size

        return rev, off

    revs, offs = [], []
    i = 0
    for block, data in zip(blocks, datas):
        rev, off = fetch(data)
        revs.append(rev)
        offs.append(off)

        # compare with sequence arithmetic
        if off and ((rev - revs[i]) & 0x80000000):
            i = len(revs)-1

    # print contents of the winning metadata block
    block, data, rev, off = blocks[i], datas[i], revs[i], offs[i]
    print('mdir 0x%x, rev %d, size %d%s' % (
        block, rev, off,
        ' (was 0x%x, %d, %d)' % (blocks[~i], revs[~i], offs[~i])
            if len(blocks) > 1 else ''))
    print('%-8s  %-22s  %s' % (
        'off', 'tag',
        'data (truncated)'
            if not args.get('no_truncate') else ''))

    # print revision count
    crc = crc32c(data[0:4])
    if args.get('raw'):
        print('%08x: %s' % (0, next(xxd(data[0:4]))))

    # print tags
    j = 4
    while j < (block_size if args.get('all') else off):
        notes = []

        j_ = j
        v, tag, size, delta = fromtag(data[j:])
        if v != popc(crc) & 1:
            notes.append('v!=%x' % (popc(crc) & 1))
        crc = crc32c(data[j:j+delta], crc)
        j += delta

        if not tag & 0x4:
            if (tag & 0x7e) != 0x2:
                crc = crc32c(data[j:j+size], crc)
            # found a crc?
            else:
                crc_, = struct.unpack('<I', data[j:j+4])
                if crc != crc_:
                    notes.append('crc!=%08x' % crc)
            j += size

        print('%08x: %-57s%s' % (
            j_,
            '%-22s%s' % (
                tagrepr(tag, size, j_),
                '  %s' % next(xxd(data[j_+delta:j_+delta+min(size, 8)], 8), '')
                    if not tag & 0x4 and not args.get('no_truncate') else ''),
            '  (%s)' % ', '.join(notes)
                if notes else ''))

        if args.get('device'):
            print('%8s  %-47s  %08x %x' % (
                '',
                '%-22s%s' % (
                    '%08x %08x' % (tag, size),
                    '  %s' % ' '.join(
                            '%08x' % struct.unpack('<I',
                                data[j_+delta+i*4:j_+delta+i*4+4])
                            for i in range(min(size//4, 3)))[:23]
                        if not tag & 0x4 else ''),
                crc,
                popc(crc) & 1))

        if args.get('raw'):
            for o, line in enumerate(xxd(data[j_:j_+delta])):
                print('%8s: %s' % ('%04x' % (j_ + o*16), line))

        if not tag & 0x4:
            if args.get('raw') or args.get('no_truncate'):
                for o, line in enumerate(xxd(data[j_+delta:j_+delta+size])):
                    print('%8s: %s' % ('%04x' % (j_+delta + o*16), line))


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
        '-a', '--all',
        action='store_true',
        help="Don't stop parsing on bad commits.")
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
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
