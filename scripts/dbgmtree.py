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

# parse some rbyd addr encodings
# 0xa     -> [0xa]
# 0xa.b   -> ([0xa], b)
# 0x{a,b} -> [0xa, 0xb]
def rbydaddr(s):
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

    trunk = None
    if '.' in s:
        s, s_ = s.split('.', 1)
        trunk = int(s_, b)

    if s.startswith('{') and '}' in s:
        ss = s[1:s.find('}')].split(',')
    else:
        ss = [s]

    addr = []
    for s in ss:
        if trunk is not None:
            addr.append((int(s, b), trunk))
        else:
            addr.append(int(s, b))

    return addr

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

def frommdir(data):
    blocks = []
    d = 0
    while d < len(data):
        block, d_ = fromleb128(data[d:])
        blocks.append(block)
        d += d_
    return blocks

def frombtree(data):
    w, d1 = fromleb128(data)
    trunk, d2 = fromleb128(data[d1:])
    block, d3 = fromleb128(data[d1+d2:])
    crc = fromle32(data[d1+d2+d3:])
    return w, trunk, block, crc

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
        self.other_blocks = []

    def addr(self):
        if not self.other_blocks:
            return '0x%x.%x' % (self.block, self.trunk)
        else:
            return '0x{%x,%s}.%x' % (
                self.block,
                ','.join('%x' % block for block in self.other_blocks),
                self.trunk)

    @classmethod
    def fetch(cls, f, block_size, blocks, trunk=None):
        if isinstance(blocks, int):
            blocks = [blocks]

        if len(blocks) > 1:
            # fetch all blocks
            rbyds = [cls.fetch(f, block_size, block, trunk) for block in blocks]
            # determine most recent revision
            i = 0
            for i_, rbyd in enumerate(rbyds):
                # compare with sequence arithmetic
                if rbyd and (
                        not ((rbyd.rev - rbyds[i].rev) & 0x80000000)
                        or (rbyd.rev == rbyds[i].rev
                            and rbyd.trunk > rbyds[i].trunk)):
                    i = i_
            # keep track of the other blocks
            rbyd = rbyds[i]
            rbyd.other_blocks = [rbyds[(i+1+j) % len(rbyds)].block
                for j in range(len(rbyds)-1)]
            return rbyd
        else:
            # block may encode a trunk
            block = blocks[0]
            if isinstance(block, tuple):
                if trunk is None:
                    trunk = block[1]
                block = block[0]

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

        return cls(block, data, rev, off, trunk_, weight)

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
        id = -1

        while True:
            done, id, tag, w, j, d, data = self.lookup(id, tag+0x10)
            if done:
                break

            yield id, tag, w, j, d, data


def main(disk, mroots=None, *,
        block_size=None,
        color='auto',
        **args):
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    # flatten mroots, default to 0x{0,1}
    if not mroots:
        mroots = [[0,1]]
    mroots = [block for mroots_ in mroots for block in mroots_]

    # we seek around a bunch, so just keep the disk open
    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # look up an id, while keeping track of the search path
        def btree_lookup(btree, id, depth=None):
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
                        not depth or depth_ < depth):
                    w, trunk, block, crc = frombtree(struct_)
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

        # before we print, we need to do a pass for a few things:
        # - find the actual mroot
        # - find the total weight
        mweight = 0
        rweight = 0

        mroot = Rbyd.fetch(f, block_size, mroots)
        while True:
            rweight = max(rweight, mroot.weight)

            # fetch the next mroot
            done, rid, tag, w, j, d, data = mroot.lookup(-1, TAG_MROOT)
            if not (not done and rid == -1 and tag == TAG_MROOT):
                break

            blocks = frommdir(data)
            mroot = Rbyd.fetch(f, block_size, blocks)

        # fetch the actual mtree, if there is one
        done, rid, tag, w, j, d, data = mroot.lookup(-1, TAG_BTREE)
        if not done and rid == -1 or tag == TAG_BTREE:
            w, trunk, block, crc = frombtree(data)
            mtree = Rbyd.fetch(f, block_size, block, trunk)

            mweight = w

            # traverse entries
            mid = -1
            while True:
                (done, mid, w, rbyd, rid,
                    (name_tag, name_j, name_d, name),
                    (struct_tag, struct_j, struct_d, struct_),
                    path) = (btree_lookup(mtree, mid+1,
                        depth=args.get('depth')))
                if done:
                    break

                # corrupted?
                if not rbyd:
                    continue

                if struct_tag != TAG_MDIR:
                    continue

                # fetch the mdir
                blocks = frommdir(struct_)
                mdir = Rbyd.fetch(f, block_size, blocks)

                rweight = max(rweight, mroot.weight)


        def dbg_mdir(mdir, mid):
            for i, (id, tag, w, j, d, data) in enumerate(mdir):
                # show human-readable tag representation
                print('%12s %-57s' % (
                    '{%s}:' % ','.join('%04x' % block
                        for block in it.chain([mdir.block],
                            mdir.other_blocks))
                        if i == 0 else '',
                    '%*s %-22s%s' % (
                        w_width, '%d.%d-%d' % (mid, id-(w-1), id)
                            if w > 1 else '%d.%d' % (mid, id)
                            if w > 0 else '%d' % mid
                            if i == 0 else '',
                        tagrepr(tag, w, len(data), j),
                        '  %s' % next(xxd(data, 8), '')
                            if not args.get('no_truncate') else '')))

                # show in-device representation
                if args.get('device'):
                    print('%11s  %*s %s' % (
                        '',
                        w_width, '',
                        '%-22s%s' % (
                            '%04x %08x %07x' % (tag, w, len(data)),
                            '  %s' % ' '.join(
                                    '%08x' % fromle32(
                                        mdir.data[j+d+i*4
                                            : j+d+min(i*4+4,len(data))])
                                    for i in range(
                                        min(m.ceil(len(data)/4),
                                        3)))[:23]
                                if not args.get('no_truncate')
                                    and not tag & 0x8 else '')))

                # show on-disk encoding of tags
                if args.get('raw'):
                    for o, line in enumerate(xxd(mdir.data[j:j+d])):
                        print('%11s: %*s %s' % (
                            '%04x' % (j + o*16),
                            w_width, '',
                            line))
                if args.get('raw') or args.get('no_truncate'):
                    if not tag & 0x8:
                        for o, line in enumerate(xxd(data)):
                            print('%11s: %*s %s' % (
                                '%04x' % (j+d + o*16),
                                w_width, '',
                                line))

        # prbyd here means the last rendered rbyd, we update
        # in dbg_entry to always print interleaved addresses
        prbyd = None
        def dbg_entry(id, w, rbyd, rid,
                name_tag, name_j, name_d, name,
                struct_tag, struct_j, struct_d, struct_,
                depth=None):
            nonlocal prbyd

            # show human-readable representation
            if name_tag:
                print('%12s %*s %-22s  %s' % (
                    '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                        if prbyd is None or rbyd != prbyd
                        else '',
                    w_width, '%d-%d' % (id-(w-1), id) if w > 1
                        else id if w > 0
                        else '',
                    tagrepr(name_tag, w, len(name), None),
                    ''.join(
                        b if b >= ' ' and b <= '~' else '.'
                        for b in map(chr, name))))
                prbyd = rbyd
            print('%12s %*s %-22s  %s' % (
                '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                    if prbyd is None or rbyd != prbyd
                    else '',
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
                    print('%11s  %*s %-22s%s' % (
                        '',
                        w_width, '',
                        '%04x %08x %07x' % (name_tag, w, len(name)),
                        '  %s' % ' '.join(
                            '%08x' % fromle32(
                                rbyd.data[name_j+name_d+i*4
                                    : name_j+name_d + min(i*4+4,len(name))])
                            for i in range(min(m.ceil(len(name)/4), 3)))[:23]))
                print('%11s  %*s %-22s%s' % (
                    '',
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
                    print('%11s: %*s %s' % (
                        '%04x' % (name_j + o*16),
                        w_width, '',
                        line))
            if args.get('raw'):
                for o, line in enumerate(xxd(name)):
                    print('%11s: %*s %s' % (
                        '%04x' % (name_j+name_d + o*16),
                        w_width, '',
                        line))
            if args.get('raw'):
                for o, line in enumerate(xxd(
                        rbyd.data[struct_j:struct_j+struct_d])):
                    print('%11s: %*s %s' % (
                        '%04x' % (struct_j + o*16),
                        w_width, '',
                        line))
            if args.get('raw') or args.get('no_truncate'):
                for o, line in enumerate(xxd(struct_)):
                    print('%11s: %*s %s' % (
                        '%04x' % (struct_j+struct_d + o*16),
                        w_width, '',
                        line))


        # print the actual mroot
        print('mroot %s, rev %d, weight %d' % (
            mroot.addr(), mroot.rev, mroot.weight))

        # print header
        w_width = (m.ceil(m.log10(max(1, mweight)+1))
            + 2*m.ceil(m.log10(max(1, rweight)+1))
            + 2)
        print('%-11s  %-*s %-22s  %s' % (
            'mdir',
            w_width, 'ids',
            'tag',
            'data (truncated)'
                if not args.get('no_truncate') else ''))

        # show each mroot
        prbyd = None
        ppath = []
        corrupted = False
        mroot = Rbyd.fetch(f, block_size, mroots)
        while True:
            # corrupted?
            if not mroot:
                print('{%s}: %s%s%s' % (
                    ','.join('%04x' % block
                        for block in it.chain([mroot.block],
                            mroot.other_blocks)),
                    '\x1b[31m' if color else '',
                    '(corrupted mroot %s)' % mroot.addr(),
                    '\x1b[m' if color else ''))
                corrupted = True
            else:
                # show the mdir
                dbg_mdir(mroot, -1)

            # fetch the next mroot
            done, rid, tag, w, j, d, data = mroot.lookup(-1, TAG_MROOT)
            if not (not done and rid == -1 and tag == TAG_MROOT):
                break

            blocks = frommdir(data)
            mroot = Rbyd.fetch(f, block_size, blocks)

        # fetch the actual mtree, if there is one
        done, rid, tag, w, j, d, data = mroot.lookup(-1, TAG_BTREE)
        if not done and rid == -1 or tag == TAG_BTREE:
            w, trunk, block, crc = frombtree(data)
            mtree = Rbyd.fetch(f, block_size, block, trunk)

            # traverse entries
            mid = -1
            while True:
                (done, mid, w, rbyd, rid,
                    (name_tag, name_j, name_d, name),
                    (struct_tag, struct_j, struct_d, struct_),
                    path) = (btree_lookup(mtree, mid+1,
                        depth=args.get('depth')))
                if done:
                    break

                # print inner btree entries if requested
                if args.get('inner'):
                    changed = False
                    for i, (x, px) in enumerate(
                            it.zip_longest(path[:-1], ppath[:-1])):
                        if x is None:
                            break

                        (id_, w_, rbyd_, rid_,
                            (name_tag_, name_j_, name_d_, name_),
                            (struct_tag_, struct_j_, struct_d_, struct__)) = x

                        if args.get('inner'):
                            if not (changed or px is None or x != px):
                                continue
                            changed = True

                            # show the inner entry
                            dbg_entry(id_, w_, rbyd_, rid_,
                                name_tag_, name_j_, name_d_, name_,
                                struct_tag_, struct_j_, struct_d_, struct__,
                                i)
                ppath = path

                # corrupted? try to keep printing the tree
                if not rbyd:
                    print('%11s: %s%s%s' % (
                        '%04x.%04x' % (rbyd.block, rbyd.trunk),
                        '\x1b[31m' if color else '',
                        '(corrupted rbyd %s)' % rbyd.addr(),
                        '\x1b[m' if color else ''))
                    prbyd = rbyd
                    ppath = path
                    corrupted = True
                    continue

                # print btree entries in certain cases
                if args.get('inner') or struct_tag != TAG_MDIR:
                    dbg_entry(mid, w, rbyd, rid,
                        name_tag, name_j, name_d, name,
                        struct_tag, struct_j, struct_d, struct_)

                if struct_tag != TAG_MDIR:
                    continue

                # fetch the mdir
                blocks = frommdir(struct_)
                mdir = Rbyd.fetch(f, block_size, blocks)

                # corrupted?
                if not mdir:
                    print('{%s}: %s%s%s' % (
                        ','.join('%04x' % block
                            for block in it.chain([mdir.block],
                                mdir.other_blocks)),
                        '\x1b[31m' if color else '',
                        '(corrupted mdir %s)' % mdir.addr(),
                        '\x1b[m' if color else ''))
                    corrupted = True
                else:
                    # show the mdir
                    dbg_mdir(mdir, mid)

                    # force next btree entry to be shown
                    prbyd = None

        if args.get('error_on_corrupt') and corrupted:
            sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Debug littlefs's metadata tree.",
        allow_abbrev=False)
    parser.add_argument(
        'disk',
        help="File containing the block device.")
    parser.add_argument(
        'mroots',
        nargs='*',
        type=rbydaddr,
        help="Block address of the mroots. Defaults to 0x{0,1}.")
    parser.add_argument(
        '-B', '--block-size',
        type=lambda x: int(x, 0),
        help="Block size in bytes.")
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
