#!/usr/bin/env python3

import bisect
import itertools as it
import math as m
import os
import struct


def blocklim(s):
    if '.' in s:
        s = s.strip()
        b = 10
        if s.startswith('0x') or s.startswith('0X'):
            s = s[2:]
            b = 16
        elif s.startswith('0o') or s.startswith('0O'):
            s = s[2:]
            b = 8
        elif s.startswith('0b') or s.startswith('0B'):
            s = s[2:]
            b = 2

        s0, s1 = s.split('.', 1)
        return int(s0, b), int(s1, b)
    else:
        return int(s, 0)

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
    tag, delta = fromleb128(data)
    id, delta_ = fromleb128(data[delta:])
    size, delta__ = fromleb128(data[delta+delta_:])
    return tag&1, tag&~1, id if tag&0x8 else id-1, size, delta+delta_+delta__

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

def tagrepr(tag, id, size, off=None):
    if (tag & ~0x3f0) == 0x0400:
        return 'mk%s id%d %d' % (
            'branch' if ((tag & 0x3f0) >> 4) == 0x00
                else 'reg' if ((tag & 0x3f0) >> 4) == 0x01
                else 'dir' if ((tag & 0x3f0) >> 4) == 0x02
                else ' 0x%02x' % ((tag & 0x3f0) >> 4),
            id,
            size)
    elif tag == 0x0800:
        return 'inlined id%d %d' % (id, size)
    elif tag == 0x0810:
        return 'block id%d %d' % (id, size)
    elif tag == 0x0820:
        return 'btree id%d %d' % (id, size)
    elif tag == 0x0830:
        return 'branch id%d %d' % (id, size)
    elif (tag & ~0xff2) == 0x2000:
        return '%suattr 0x%02x%s%s' % (
            'rm' if tag & 0x2 else '',
            (tag & 0xff0) >> 4,
            ' id%d' % id if id != -1 else '',
            ' %d' % size if not tag & 0x2 or size else '')
    elif tag == 0x0006:
        return 'grow id%d w%d' % (
            id,
            size)
    elif tag == 0x0016:
        return 'shrink id%d w%d' % (
            id,
            size)
    elif (tag & ~0x10) == 0x0004:
        return 'crc%x%s %d' % (
            1 if tag & 0x10 else 0,
            ' 0x%02x' % id if id != -1 else '',
            size)
    elif tag == 0x0024:
        return 'fcrc%s %d' % (
            ' 0x%02x' % id if id != -1 else '',
            size)
    elif tag & 0x8:
        return 'alt%s%s 0x%x w%d %s' % (
            'r' if tag & 0x2 else 'b',
            'gt' if tag & 0x4 else 'le',
            tag & 0x3ff0,
            id,
            '0x%x' % (0xffffffff & (off-size))
                if off is not None
                else '-%d' % off)
    else:
        return '0x%04x id%d %d' % (tag, id, size)


class Rbyd:
    def __init__(self, block, limit, data, rev, off, trunk, weight):
        self.block = block
        self.limit = limit
        self.data = data
        self.rev = rev
        self.off = off
        self.trunk = trunk
        self.weight = weight

    @classmethod
    def fetch(cls, f, block_size, block, limit):
        # seek to the block
        f.seek(block * block_size)
        data = f.read(limit)

        # fetch the rbyd
        rev, = struct.unpack('<I', data[0:4].ljust(4, b'\0'))
        crc = crc32c(data[0:4])
        off = 0
        j_ = 4
        trunk = None
        trunk_ = None
        weight = 0
        weight_ = 0
        wastrunk = False
        while j_ < limit:
            v, tag, id, size, delta = fromtag(data[j_:])
            if v != (popc(crc) & 1):
                break
            crc = crc32c(data[j_:j_+delta], crc)
            j_ += delta

            # find trunk
            if not wastrunk and (tag & 0xe) != 0x4:
                trunk_ = j_ - delta
            wastrunk = not not tag & 0x8

            # keep track of weight
            if tag == 0x0006:
                weight_ += size
            elif tag == 0x0016:
                weight_ = max(weight_ - size, 0)

            # take care of crcs
            if (tag & 0xe) <= 0x4:
                if (tag & ~0x10) != 0x04:
                    crc = crc32c(data[j_:j_+size], crc)
                # found a crc?
                else:
                    crc_, = struct.unpack('<I', data[j_:j_+4].ljust(4, b'\0'))
                    if crc != crc_:
                        break
                    # commit what we have
                    off = j_ + size
                    trunk = trunk_
                    weight = weight_
                j_ += size

        return Rbyd(block, limit, data, rev, off, trunk, weight)

    def lookup(self, tag, id):
        if not self:
            return True, 0, -1, 0, 0, 0, b''

        lower = -1
        upper = self.weight

        # descend down tree
        j = self.trunk
        while True:
            _, alt, weight_, jump, delta = fromtag(self.data[j:])

            # found an alt?
            if alt & 0x8:
                # follow?
                if ((id, tag & ~0xf) > (upper-weight_-1, alt & ~0xf)
                        if alt & 0x4
                        else ((id, tag & ~0xf) <= (lower+weight_, alt & ~0xf))):
                    lower += upper-lower-1-weight_ if alt & 0x4 else 0
                    upper -= upper-lower-1-weight_ if not alt & 0x4 else 0
                    j = j - jump
                # stay on path
                else:
                    lower += weight_ if not alt & 0x4 else 0
                    upper -= weight_ if alt & 0x4 else 0
                    j = j + delta
            # found tag
            else:
                tag_ = alt
                id_ = upper-1
                w_ = id_-lower

                done = (id_, tag_) < (id, tag) or tag_ & 2

                return (done, tag_, id_, w_,
                    j, delta, self.data[j+delta:j+delta+jump])

    def __bool__(self):
        return self.trunk is not None

    def __eq__(self, other):
        return self.block == other.block and self.limit == other.limit

    def __ne__(self, other):
        return not self.__eq__(other)

    def __iter__(self):
        tag = 0
        id = 0

        while True:
            done, tag, id, w, j, d, data = self.lookup(tag+0x10, id)
            if done:
                break

            yield tag, id, w, j, d, data


def main(disk, block_size=None, trunk=0, limit=None, *,
        color='auto',
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # trunk may include a limit
    if isinstance(trunk, tuple):
        if limit is None:
            limit = trunk[1]
        trunk = trunk[0]

    # we seek around a bunch, so just keep the disk open
    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # default limit to the block_size
        if limit is None:
            limit = block_size

        # fetch the trunk
        trunk = Rbyd.fetch(f, block_size, trunk, limit)
        print('btree 0x%x.%x, rev %d, weight %d' % (
            trunk.block, trunk.limit, trunk.rev, trunk.weight))

        # look up an id, while keeping track of the search path
        def lookup(id, depth=None):
            rbyd = trunk
            rid = id
            depth_ = 1
            path = []

            # corrupted? return a corrupted block once
            if not rbyd:
                return (id > 0, id, 0, rbyd, -1, 0,
                    0, 0, b'',
                    0, 0, b'',
                    path)

            while True:
                # first lookup id/name
                (done, _, rid_, w,
                    name_j, name_d, name) = rbyd.lookup(0x400, rid)
                if done:
                    return True, id, 0, rbyd, -1, 0, 0, 0, b'', 0, 0, b'', path

                # then lookup struct
                (done, tag, _, _,
                    struct_j, struct_d, struct_) = rbyd.lookup(0x800, rid_)
                if done:
                    return True, id, 0, rbyd, -1, 0, 0, 0, b'', 0, 0, b'', path

                path.append((id + (rid_-rid), w, rbyd, rid_, tag,
                    name_j, name_d, name,
                    struct_j, struct_d, struct_))

                # is it another branch? continue down tree
                if tag == 0x830 and (depth is None or depth_ < depth):
                    block, delta = fromleb128(struct_)
                    limit, _ = fromleb128(struct_[delta:])
                    rbyd = Rbyd.fetch(f, block_size, block, limit)

                    # corrupted? bail here so we can keep traversing the tree
                    if not rbyd:
                        return (False, id + (rid_-rid), w, rbyd, -1, 0,
                            0, 0, b'',
                            0, 0, b'',
                            path)

                    rid -= (rid_-(w-1))
                    depth_ += 1
                else:
                    return (False, id + (rid_-rid), w, rbyd, rid_, tag,
                        name_j, name_d, name,
                        struct_j, struct_d, struct_,
                        path)

        # if we're printing the tree, first find the max depth so we know how
        # much space to reserve
        t_width = 0
        if args.get('tree'):
            t_depth = 0
            id = -1
            while True:
                (done, id, w, rbyd, rid, tag,
                    name_j, name_d, name,
                    struct_j, struct_d, struct_,
                    path) = (lookup(id+1, depth=args.get('depth')))
                if done:
                    break

                t_depth = max(t_depth, len(path))

            t_width = 2*t_depth+2 if t_depth > 0 else 0
            t_branches = [(0, trunk.weight)]

            def t_repr(id, w, d=None):
                branches_ = []
                for i in range(len(t_branches)):
                    if d is not None and d == i-1:
                        branches_.append('+')
                    elif i+1 < len(t_branches):
                        if (id-(w-1) == t_branches[i+1][0]
                                and t_branches[i+1][0] == t_branches[i][0]
                                and (not args.get('inner')
                                    or (i == 0 and d == 0))):
                            branches_.append('+-')
                        elif (id-(w-1) == t_branches[i+1][0]
                                and t_branches[i+1][1]-1 == t_branches[i][1]-1
                                and (not args.get('inner') or d == i)):
                            branches_.append('\'-')
                        elif (id-(w-1) == t_branches[i+1][0]
                                and (not args.get('inner') or d == i)):
                            branches_.append('|-')
                        elif (id-(w-1) >= t_branches[i][0]
                                and id-(w-1) < t_branches[i][1]
                                and t_branches[i+1][1]-1 != t_branches[i][1]-1):
                            branches_.append('| ')
                        else:
                            branches_.append('  ')
                    else:
                        if (id-(w-1) == t_branches[i][0]
                                and (not args.get('inner') or i == 0)):
                            branches_.append('+-%s> ' % ('-'*2*(t_depth-i-1)))
                        elif id-(w-1) == t_branches[i][1]-1:
                            branches_.append('\'-%s> ' % ('-'*2*(t_depth-i-1)))
                        elif (id-(w-1) >= t_branches[i][0]
                                and id-(w-1) < t_branches[i][1]):
                            branches_.append('|-%s> ' % ('-'*2*(t_depth-i-1)))

                return '%s%-*s%s' % (
                    '\x1b[90m' if color else '',
                    t_width, ''.join(branches_),
                    '\x1b[m' if color else '')

        # print header
        w_width = 2*m.ceil(m.log10(max(1, trunk.weight)+1))+1
        print('%-9s  %*s%-*s %-8s %-22s  %s' % (
            'block',
            t_width, '',
            w_width, 'ids',
            'name',
            'tag',
            'data (truncated)'
                if not args.get('no_truncate') else ''))

        # traverse and print entries
        id = -1
        prbyd = None
        ppath = []
        corrupted = False
        while True:
            (done, id, w, rbyd, rid, tag,
                name_j, name_d, name,
                struct_j, struct_d, struct_,
                path) = (lookup(id+1, depth=args.get('depth')))
            if done:
                break

            if args.get('inner') or args.get('tree'):
                t_branches = [(0, trunk.weight)]
                changed = False
                for i, (x, px) in enumerate(
                        it.zip_longest(path[:-1], ppath[:-1])):
                    if x is None:
                        break

                    (id_, w_, rbyd_, rid_, tag_,
                        name_j_, name_d_, name_,
                        struct_j_, struct_d_, struct__) = x
                    t_branches.append((id_-(w_-1), id_+1))

                    if args.get('inner'):
                        if not (changed or px is None or x != px):
                            continue
                        changed = True

                        # show human-readable representation
                        print('%10s %s%*s %-8s %-22s  %s' % (
                            '%04x.%04x:' % (rbyd_.block, rbyd_.limit)
                                if prbyd is None or rbyd_ != prbyd
                                else '',
                            t_repr(id_, w_, i) if args.get('tree') else '',
                            w_width, '%d-%d' % (id_-(w_-1), id_)
                                if w_ > 1 else id_
                                if w_ > 0 else '',
                            ''.join(
                                b if b >= ' ' and b <= '~' else '.'
                                for b in map(chr, name_)),
                            tagrepr(tag_, rid_, len(struct__), None),
                            next(xxd(struct__, 8), '')
                                if not args.get('no_truncate') else ''))

                        # show in-device representation
                        if args.get('device'):
                            print('%9s  %*s%*s %8s %-22s%s' % (
                                '',
                                t_width, '',
                                w_width, '',
                                '',
                                '%04x %08x %07x' % (
                                    tag_, 0xffffffff & rid_, len(struct__)),
                                '  %s' % ' '.join(
                                    '%08x' % struct.unpack('<I',
                                        rbyd_.data[struct_j_+struct_d_+i*4
                                            : struct_j_+struct_d_
                                                + min(i*4+4,len(struct__))]
                                            .ljust(4, b'\0'))
                                    for i in range(
                                        min(m.ceil(len(struct__)/4), 3)))[:23]))

                        # show on-disk encoding of tags/data
                        for j, d, data in [
                                (name_j_, name_d_, name_),
                                (struct_j_, struct_d_, struct__)]:
                            if args.get('raw'):
                                for o, line in enumerate(
                                        xxd(rbyd_.data[j:j+d])):
                                    print('%9s: %s' % (
                                        '%04x' % (j + o*16),
                                        line))

                            # show on-disk encoding of tags
                            if args.get('raw') or args.get('no_truncate'):
                                for o, line in enumerate(xxd(data)):
                                    print('%9s: %s' % (
                                        '%04x' % (j+d + o*16),
                                        line))

                        # prbyd here means the last rendered rbyd, we update
                        # here to always print interleaved addresses
                        prbyd = rbyd_

            # corrupted? try to keep printing the tree
            if not rbyd:
                print('%04x.%04x: %s%s%s%s' % (
                    rbyd.block, rbyd.limit,
                    t_repr(id, w) if args.get('tree') else '',
                    '\x1b[31m' if color else '',
                    '(corrupted rbyd 0x%x.%x)' % (rbyd.block, rbyd.limit),
                    '\x1b[m' if color else ''))

                prbyd = rbyd
                ppath = path
                corrupted = True
                continue

            # show human-readable representation
            print('%10s %s%*s %-8s %-22s  %s' % (
                '%04x.%04x:' % (rbyd.block, rbyd.limit)
                    if prbyd is None or rbyd != prbyd
                    else '',
                t_repr(id, w) if args.get('tree') else '',
                w_width, '%d-%d' % (id-(w-1), id)
                    if w > 1 else id
                    if w > 0 else '',
                ''.join(
                    b if b >= ' ' and b <= '~' else '.'
                    for b in map(chr, name)),
                tagrepr(tag, rid, len(struct_), None),
                next(xxd(struct_, 8), '')
                    if not args.get('no_truncate') else ''))

            # show in-device representation
            if args.get('device'):
                print('%9s  %*s%*s %8s %-22s%s' % (
                    '',
                    t_width, '',
                    w_width, '',
                    '',
                    '%04x %08x %07x' % (
                        tag, 0xffffffff & rid, len(struct_)),
                    '  %s' % ' '.join(
                        '%08x' % struct.unpack('<I',
                            rbyd.data[struct_j+struct_d+i*4
                                : struct_j+struct_d
                                    + min(i*4+4,len(struct_))]
                                .ljust(4, b'\0'))
                        for i in range(
                            min(m.ceil(len(struct_)/4), 3)))[:23]))

            # show on-disk encoding of tags/data
            for j, d, data in [
                    (name_j, name_d, name),
                    (struct_j, struct_d, struct_)]:
                if args.get('raw'):
                    for o, line in enumerate(xxd(rbyd.data[j:j+d])):
                        print('%9s: %s' % (
                            '%04x' % (j + o*16),
                            line))

                # show on-disk encoding of tags
                if args.get('raw') or args.get('no_truncate'):
                    for o, line in enumerate(xxd(data)):
                        print('%9s: %s' % (
                            '%04x' % (j+d + o*16),
                            line))

            prbyd = rbyd
            ppath = path

        if args.get('error_on_corrupt') and corrupted:
            sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Debug rbyd B-trees.",
        allow_abbrev=False)
    parser.add_argument(
        'disk',
        help="File containing the block device.")
    parser.add_argument(
        'trunk',
        nargs='?',
        type=blocklim,
        help="Block address of the trunk of the tree.")
    parser.add_argument(
        '-B', '--block-size',
        type=lambda x: int(x, 0),
        help="Block size in bytes.")
    parser.add_argument(
        '-L', '--limit',
        type=lambda x: int(x, 0),
        help="Rbyd limit of the trunk of the tree (alias).")
    parser.add_argument(
        '--color',
        choices=['never', 'always', 'auto'],
        default='auto',
        help="When to use terminal colors. Defaults to 'auto'.")
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
        '-i', '--inner',
        action='store_true',
        help="Show inner branches.")
    parser.add_argument(
        '-t', '--tree',
        action='store_true',
        help="Show the underlying B-tree.")
    parser.add_argument(
        '-Z', '--depth',
        type=lambda x: int(x, 0),
        help="Depth of tree to show.")
    parser.add_argument(
        '-e', '--error-on-corrupt',
        action='store_true',
        help="Error if B-tree is corrupt.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
