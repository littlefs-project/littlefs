#!/usr/bin/env python3

import bisect
import itertools as it
import math as m
import os
import struct


TAG_UNR         = 0x0002
TAG_MAGIC       = 0x0030
TAG_CONFIG      = 0x0040
TAG_MROOT       = 0x0110
TAG_NAME        = 0x1000
TAG_BRANCH      = 0x1000
TAG_REG         = 0x1010
TAG_DIR         = 0x1020
TAG_STRUCT      = 0x3000
TAG_INLINED     = 0x3000
TAG_BLOCK       = 0x3100
TAG_MDIR        = 0x3200
TAG_BTREE       = 0x3300
TAG_UATTR       = 0x4000
TAG_ALT         = 0x0008
TAG_CRC         = 0x0004
TAG_FCRC        = 0x1004

def rbydaddr(s):
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

def fromle16(data):
    return struct.unpack('<H', data[0:2].ljust(2, b'\0'))[0]

def fromle32(data):
    return struct.unpack('<I', data[0:4].ljust(4, b'\0'))[0]

def fromleb128(data):
    word = 0
    for i, b in enumerate(data):
        word |= ((b & 0x7f) << 7*i)
        word &= 0xffffffff
        if not b & 0x80:
            return word, i+1
    return word, len(data)

def fromtag(data):
    tag = fromle16(data)
    weight, d = fromleb128(data[2:])
    size, d_ = fromleb128(data[2+d:])
    return tag&1, tag&~1, weight, size, 2+d+d_

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

def tagrepr(tag, w, size, off=None):
    if (tag & 0xfffe) == TAG_UNR:
        return 'unr%s%s' % (
            ' w%d' % w if w else '',
            ' %d' % size if size else '')
    elif (tag & 0xfffc) == TAG_MAGIC:
        return '%smagic%s %d' % (
            'rm' if tag & 0x2 else '',
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xfffc) == TAG_CONFIG:
        return '%sconfig%s %d' % (
            'rm' if tag & 0x2 else '',
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xfffc) == TAG_MROOT:
        return '%smroot%s %d' % (
            'rm' if tag & 0x2 else '',
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xf00c) == TAG_NAME:
        return '%s%s%s %d' % (
            'rm' if tag & 0x2 else '',
            'bname' if (tag & 0xfffe) == TAG_BRANCH
                else 'reg' if (tag & 0xfffe) == TAG_REG
                else 'dir' if (tag & 0xfffe) == TAG_DIR
                else 'name 0x%02x' % ((tag & 0x0ff0) >> 4),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xf00c) == TAG_STRUCT:
        return '%s%s%s %d' % (
            'rm' if tag & 0x2 else '',
            'inlined' if (tag & 0xfffe) == TAG_INLINED
                else 'block' if (tag & 0xfffe) == TAG_BLOCK
                else 'mdir' if (tag & 0xfffe) == TAG_MDIR
                else 'btree' if (tag & 0xfffe) == TAG_BTREE
                else 'struct 0x%02x' % ((tag & 0x0ff0) >> 4),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xf00c) == TAG_UATTR:
        return '%suattr 0x%02x%s%s' % (
            'rm' if tag & 0x2 else '',
            (tag & 0x0ff0) >> 4,
            ' w%d' % w if w else '',
            ' %d' % size if not tag & 0x2 or size else '')
    elif (tag & 0xf00e) == TAG_CRC:
        return 'crc%x%s %d' % (
            1 if tag & 0x10 else 0,
            ' 0x%x' % w if w > 0 else '',
            size)
    elif (tag & 0xfffe) == TAG_FCRC:
        return 'fcrc%s %d' % (
            ' 0x%x' % w if w > 0 else '',
            size)
    elif tag & 0x8:
        return 'alt%s%s 0x%x w%d %s' % (
            'r' if tag & 0x2 else 'b',
            'gt' if tag & 0x4 else 'le',
            tag & 0xfff0,
            w,
            '0x%x' % (0xffffffff & (off-size))
                if off is not None
                else '-%d' % off)
    else:
        return '0x%04x w%d %d' % (tag, w, size)

class Rbyd:
    def __init__(self, block, data, rev, off, trunk, weight):
        self.block = block
        self.data = data
        self.rev = rev
        self.off = off
        self.trunk = trunk
        self.weight = weight

    @classmethod
    def fetch(cls, f, block_size, block, trunk):
        # seek to the block
        f.seek(block * block_size)
        data = f.read(block_size)

        # fetch the rbyd
        rev = fromle32(data[0:4])
        crc = 0
        crc_ = crc32c(data[0:4])
        off = 0
        j_ = 4
        trunk_ = 0
        trunk__ = 0
        weight = 0
        lower_, upper_ = 0, 0
        weight_ = 0
        wastrunk = False
        trunkoff = None
        while j_ < len(data) and (not trunk or off <= trunk):
            v, tag, w, size, d = fromtag(data[j_:])
            if v != (popc(crc_) & 1):
                break
            crc_ = crc32c(data[j_:j_+d], crc_)
            j_ += d
            if not tag & 0x8 and j_ + size > len(data):
                break

            # take care of crcs
            if not tag & 0x8:
                if (tag & 0xf00f) != TAG_CRC:
                    crc_ = crc32c(data[j_:j_+size], crc_)
                # found a crc?
                else:
                    crc__ = fromle32(data[j_:j_+4])
                    if crc_ != crc__:
                        break
                    # commit what we have
                    off = trunkoff if trunkoff else j_ + size
                    crc = crc_
                    trunk_ = trunk__
                    weight = weight_

            # evaluate trunks
            if (tag & 0xc) != 0x4 and (
                    not trunk or trunk >= j_-d or wastrunk):
                # new trunk?
                if not wastrunk:
                    trunk__ = j_-d
                    lower_, upper_ = 0, 0
                    wastrunk = True

                # keep track of weight
                if tag & 0x8:
                    if tag & 0x4:
                        upper_ += w
                    else:
                        lower_ += w
                else:
                    weight_ = lower_+upper_+w
                    wastrunk = False
                    # keep track of off for best matching trunk
                    if trunk and j_ + size > trunk:
                        trunkoff = j_ + size

            if not tag & 0x8:
                j_ += size

        return Rbyd(block, data, rev, off, trunk_, weight)

    def lookup(self, id, tag):
        if not self:
            return True, 0, -1, 0, 0, 0, b''

        lower = -1
        upper = self.weight

        # descend down tree
        j = self.trunk
        while True:
            _, alt, weight_, jump, d = fromtag(self.data[j:])

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
                    j = j + d
            # found tag
            else:
                id_ = upper-1
                tag_ = alt
                w_ = id_-lower

                done = (id_, tag_) < (id, tag) or tag_ & 2

                return (done, id_, tag_, w_,
                    j, d, self.data[j+d:j+d+jump])

    def __bool__(self):
        return bool(self.trunk)

    def __eq__(self, other):
        return self.block == other.block and self.trunk == other.trunk

    def __ne__(self, other):
        return not self.__eq__(other)

    def __iter__(self):
        tag = 0
        id = 0

        while True:
            done, id, tag, w, j, d, data = self.lookup(id, tag+0x10)
            if done:
                break

            yield id, tag, w, j, d, data


def main(disk, root=0, *,
        block_size=None,
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

    # root may encode a trunk
    if isinstance(root, tuple):
        if trunk is None:
            trunk = root[1]
        root = root[0]

    # we seek around a bunch, so just keep the disk open
    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # fetch the root
        btree = Rbyd.fetch(f, block_size, root, trunk)
        print('btree 0x%x.%x, rev %d, weight %d' % (
            btree.block, btree.trunk, btree.rev, btree.weight))

        # look up an id, while keeping track of the search path
        def lookup(id, depth=None):
            rbyd = btree
            rid = id
            depth_ = 1
            path = []

            # corrupted? return a corrupted block once
            if not rbyd:
                return (id > 0, id, 0, rbyd, -1,
                    (0, 0, 0, b''),
                    (0, 0, 0, b''),
                    path)

            while True:
                # first lookup id/name
                (done, rid_, name_tag, w,
                    name_j, name_d, name) = rbyd.lookup(rid, 0)
                if done:
                    return (True, id, 0, rbyd, -1,
                        (0, 0, 0, b''),
                        (0, 0, 0, b''),
                        path)

                if name_tag & 0xf00f == TAG_NAME:
                    # then lookup struct
                    (done, _, struct_tag, _,
                        struct_j, struct_d, struct_) = rbyd.lookup(
                            rid_, TAG_STRUCT)
                    if done:
                        return (True, id, 0, rbyd, -1,
                            (0, 0, 0, b''),
                            (0, 0, 0, b''),
                            path)
                else:
                    tag = name_tag
                    struct_tag, struct_j, struct_d, struct_ = (
                        name_tag, name_j, name_d, name)
                    name_tag, name_j, name_d, name = 0, 0, 0, b''

                path.append((id + (rid_-rid), w, rbyd, rid_,
                    (name_tag, name_j, name_d, name),
                    (struct_tag, struct_j, struct_d, struct_)))

                # is it another branch? continue down tree
                if struct_tag == TAG_BTREE and (
                        depth is None or depth_ < depth):
                    w, d1 = fromleb128(struct_)
                    trunk, d2 = fromleb128(struct_[d1:])
                    block, d3 = fromleb128(struct_[d1+d2:])
                    crc = fromle32(struct_[d1+d2+d3:])
                    rbyd = Rbyd.fetch(f, block_size, block, trunk)

                    # corrupted? bail here so we can keep traversing the tree
                    if not rbyd:
                        return (False, id + (rid_-rid), w, rbyd, -1,
                            (0, 0, 0, b''),
                            (0, 0, 0, b''),
                            path)

                    rid -= (rid_-(w-1))
                    depth_ += 1
                else:
                    return (False, id + (rid_-rid), w, rbyd, rid_,
                        (name_tag, name_j, name_d, name),
                        (struct_tag, struct_j, struct_d, struct_),
                        path)

        # if we're printing the tree, first find the max depth so we know how
        # much space to reserve
        t_width = 0
        if args.get('tree'):
            t_depth = 0
            id = -1
            while True:
                (done, id, w, rbyd, rid, _, _,
                    path) = (lookup(id+1, depth=args.get('depth')))
                if done:
                    break

                t_depth = max(t_depth, len(path))

            t_width = 2*t_depth+2 if t_depth > 0 else 0
            t_branches = [(0, btree.weight)]

            def treerepr(id, w, leaf=True, depth=None):
                branches_ = []
                for i in range(len(t_branches)):
                    if depth is not None and depth == i-1:
                        branches_.append('+' if leaf else '|')
                        break

                    if i+1 < len(t_branches):
                        if (leaf
                                and id-(w-1) == t_branches[i+1][0]
                                and t_branches[i][0] == t_branches[i+1][0]
                                and (not args.get('inner')
                                    or (i == 0 and depth == 0))):
                            branches_.append('+-')
                        elif (leaf
                                and id-(w-1) == t_branches[i+1][0]
                                and t_branches[i][1] == t_branches[i+1][1]
                                and (not args.get('inner') or depth == i)):
                            branches_.append('\'-')
                        elif (leaf
                                and id-(w-1) == t_branches[i+1][0]
                                and (not args.get('inner') or depth == i)):
                            branches_.append('|-')
                        elif (id-(w-1) >= t_branches[i][0]
                                and id-(w-1) < t_branches[i][1]
                                and t_branches[i][1] != t_branches[i+1][1]):
                            branches_.append('| ')
                        else:
                            branches_.append('  ')
                    else:
                        if (leaf
                                and id-(w-1) == t_branches[i][0]
                                and (not args.get('inner') or i == 0)):
                            branches_.append('+-%s> ' % ('-'*2*(t_depth-i-1)))
                        elif (leaf
                                and id == t_branches[i][1]-1):
                            branches_.append('\'-%s> ' % ('-'*2*(t_depth-i-1)))
                        elif (leaf
                                and id >= t_branches[i][0]
                                and id-(w-1) < t_branches[i][1]):
                            branches_.append('|-%s> ' % ('-'*2*(t_depth-i-1)))
                        elif (id >= t_branches[i][0]
                                and id < t_branches[i][1]-1):
                            branches_.append('|')
                        else:
                            branches_.append(' ')

                return '%s%-*s%s' % (
                    '\x1b[90m' if color else '',
                    t_width, ''.join(branches_),
                    '\x1b[m' if color else '')


        # print header
        w_width = 2*m.ceil(m.log10(max(1, btree.weight)+1))+1
        print('%-9s  %*s%-*s %-22s  %s' % (
            'block',
            t_width, '',
            w_width, 'ids',
            'tag',
            'data (truncated)'
                if not args.get('no_truncate') else ''))

        # prbyd here means the last rendered rbyd, we update
        # in show_entry to always print interleaved addresses
        prbyd = None
        def show_entry(id, w, rbyd, rid,
                name_tag, name_j, name_d, name,
                struct_tag, struct_j, struct_d, struct_,
                depth=None):
            nonlocal prbyd

            # show human-readable representation
            if name_tag:
                print('%10s %s%*s %-22s  %s' % (
                    '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                        if prbyd is None or rbyd != prbyd
                        else '',
                    treerepr(id, w, True, depth) if args.get('tree') else '',
                    w_width, '%d-%d' % (id-(w-1), id) if w > 1
                        else id if w > 0
                        else '',
                    tagrepr(name_tag, w, len(name), None),
                    ''.join(
                        b if b >= ' ' and b <= '~' else '.'
                        for b in map(chr, name))))
                prbyd = rbyd
            print('%10s %s%*s %-22s  %s' % (
                '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                    if prbyd is None or rbyd != prbyd
                    else '',
                treerepr(id, w, not name_tag, depth) if args.get('tree') else '',
                w_width, '' if name_tag
                    else '%d-%d' % (id-(w-1), id) if w > 1
                    else id if w > 0
                    else '',
                tagrepr(struct_tag,
                    w if not name_tag else 0,
                    len(struct_), None),
                next(xxd(struct_, 8), '')
                    if not args.get('no_truncate') else ''))
            prbyd = rbyd

            # show in-device representation
            if args.get('device'):
                if name_tag:
                    print('%9s  %*s%*s %-22s%s' % (
                        '',
                        t_width, '',
                        w_width, '',
                        '%04x %08x %07x' % (name_tag, w, len(name)),
                        '  %s' % ' '.join(
                            '%08x' % fromle32(
                                rbyd.data[name_j+name_d+i*4
                                    : name_j+name_d + min(i*4+4,len(name))])
                            for i in range(min(m.ceil(len(name)/4), 3)))[:23]))
                print('%9s  %*s%*s %-22s%s' % (
                    '',
                    t_width, '',
                    w_width, '',
                    '%04x %08x %07x' % (
                        struct_tag, w if not name_tag else 0, len(struct_)),
                    '  %s' % ' '.join(
                        '%08x' % fromle32(
                            rbyd.data[struct_j+struct_d+i*4
                                : struct_j+struct_d + min(i*4+4,len(struct_))])
                        for i in range(min(m.ceil(len(struct_)/4), 3)))[:23]))

            # show on-disk encoding of tags/data
            if args.get('raw'):
                for o, line in enumerate(xxd(
                        rbyd.data[name_j:name_j+name_d])):
                    print('%9s: %*s%*s %s' % (
                        '%04x' % (name_j + o*16),
                        t_width, '',
                        w_width, '',
                        line))
            if args.get('raw'):
                for o, line in enumerate(xxd(name)):
                    print('%9s: %*s%*s %s' % (
                        '%04x' % (name_j+name_d + o*16),
                        t_width, '',
                        w_width, '',
                        line))
            if args.get('raw'):
                for o, line in enumerate(xxd(
                        rbyd.data[struct_j:struct_j+struct_d])):
                    print('%9s: %*s%*s %s' % (
                        '%04x' % (struct_j + o*16),
                        t_width, '',
                        w_width, '',
                        line))
            if args.get('raw') or args.get('no_truncate'):
                for o, line in enumerate(xxd(struct_)):
                    print('%9s: %*s%*s %s' % (
                        '%04x' % (struct_j+struct_d + o*16),
                        t_width, '',
                        w_width, '',
                        line))


        # traverse and print entries
        id = -1
        prbyd = None
        ppath = []
        corrupted = False
        while True:
            (done, id, w, rbyd, rid,
                (name_tag, name_j, name_d, name),
                (struct_tag, struct_j, struct_d, struct_),
                path) = (lookup(id+1, depth=args.get('depth')))
            if done:
                break

            if args.get('inner') or args.get('tree'):
                t_branches = [(0, btree.weight)]
                changed = False
                for i, (x, px) in enumerate(
                        it.zip_longest(path[:-1], ppath[:-1])):
                    if x is None:
                        break

                    (id_, w_, rbyd_, rid_,
                        (name_tag_, name_j_, name_d_, name_),
                        (struct_tag_, struct_j_, struct_d_, struct__)) = x
                    t_branches.append((id_-(w_-1), id_+1))

                    if args.get('inner'):
                        if not (changed or px is None or x != px):
                            continue
                        changed = True

                        # show the inner entry
                        show_entry(id_, w_, rbyd_, rid_,
                            name_tag_, name_j_, name_d_, name_,
                            struct_tag_, struct_j_, struct_d_, struct__,
                            i)

            # corrupted? try to keep printing the tree
            if not rbyd:
                print('%04x.%04x: %s%s%s%s' % (
                    rbyd.block, rbyd.trunk,
                    treerepr(id, w) if args.get('tree') else '',
                    '\x1b[31m' if color else '',
                    '(corrupted rbyd 0x%x.%x)' % (rbyd.block, rbyd.trunk),
                    '\x1b[m' if color else ''))

                prbyd = rbyd
                ppath = path
                corrupted = True
                continue

            # if we're not showing inner nodes, prefer names higher in the tree
            # since this avoids showing vestigial names
            if not args.get('inner'):
                for id_, w_, rbyd_, rid_, name_, struct__ in reversed(path):
                    name_tag_, name_j, name_d, name = name_
                    if rid_-(w_-1) != 0:
                        break

            # show the entry
            show_entry(id, w, rbyd, rid,
                name_tag, name_j, name_d, name,
                struct_tag, struct_j, struct_d, struct_)

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
        'root',
        nargs='?',
        type=rbydaddr,
        help="Block address of the root of the tree.")
    parser.add_argument(
        '-B', '--block-size',
        type=lambda x: int(x, 0),
        help="Block size in bytes.")
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
