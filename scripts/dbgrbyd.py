#!/usr/bin/env python3

import bisect
import collections as co
import itertools as it
import math as m
import os
import struct

COLORS = [
    '34',   # blue
    '31',   # red
    '32',   # green
    '35',   # purple
    '33',   # yellow
    '36',   # cyan
]


TAG_NULL            = 0x0000
TAG_CONFIG          = 0x0000
TAG_MAGIC           = 0x0003
TAG_VERSION         = 0x0004
TAG_RCOMPAT         = 0x0005
TAG_WCOMPAT         = 0x0006
TAG_OCOMPAT         = 0x0007
TAG_GEOMETRY        = 0x0009
TAG_NAMELIMIT       = 0x000c
TAG_SIZELIMIT       = 0x000d
TAG_GDELTA          = 0x0100
TAG_GRMDELTA        = 0x0100
TAG_NAME            = 0x0200
TAG_REG             = 0x0201
TAG_DIR             = 0x0202
TAG_ORPHAN          = 0x0203
TAG_BOOKMARK        = 0x0204
TAG_STRUCT          = 0x0300
TAG_DATA            = 0x0300
TAG_BLOCK           = 0x0304
TAG_BSHRUB          = 0x0308
TAG_BTREE           = 0x030c
TAG_DID             = 0x0310
TAG_BECKSUM         = 0x0314
TAG_BRANCH          = 0x031c
TAG_MROOT           = 0x0321
TAG_MDIR            = 0x0325
TAG_MTREE           = 0x032c
TAG_UATTR           = 0x0400
TAG_SATTR           = 0x0600
TAG_SHRUB           = 0x1000
TAG_CKSUM           = 0x3000
TAG_ECKSUM          = 0x3100
TAG_ALT             = 0x4000
TAG_GT              = 0x2000
TAG_R               = 0x1000


# some ways of block geometry representations
# 512      -> 512
# 512x16   -> (512, 16)
# 0x200x10 -> (512, 16)
def bdgeom(s):
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

    if 'x' in s:
        s, s_ = s.split('x', 1)
        return (int(s, b), int(s_, b))
    else:
        return int(s, b)

# parse some rbyd addr encodings
# 0xa       -> [0xa]
# 0xa.c     -> [(0xa, 0xc)]
# 0x{a,b}   -> [0xa, 0xb]
# 0x{a,b}.c -> [(0xa, 0xc), (0xb, 0xc)]
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

def popc(x):
    return bin(x).count('1')

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
    data = data.ljust(4, b'\0')
    tag = (data[0] << 8) | data[1]
    weight, d = fromleb128(data[2:])
    size, d_ = fromleb128(data[2+d:])
    return tag>>15, tag&0x7fff, weight, size, 2+d+d_

def xxd(data, width=16):
    for i in range(0, len(data), width):
        yield '%-*s %-*s' % (
            3*width,
            ' '.join('%02x' % b for b in data[i:i+width]),
            width,
            ''.join(
                b if b >= ' ' and b <= '~' else '.'
                for b in map(chr, data[i:i+width])))

def tagrepr(tag, w=None, size=None, off=None):
    if (tag & 0x6fff) == TAG_NULL:
        return '%snull%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            ' w%d' % w if w else '',
            ' %d' % size if size else '')
    elif (tag & 0x6f00) == TAG_CONFIG:
        return '%s%s%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            'magic' if (tag & 0xfff) == TAG_MAGIC
                else 'version' if (tag & 0xfff) == TAG_VERSION
                else 'rcompat' if (tag & 0xfff) == TAG_RCOMPAT
                else 'wcompat' if (tag & 0xfff) == TAG_WCOMPAT
                else 'ocompat' if (tag & 0xfff) == TAG_OCOMPAT
                else 'geometry' if (tag & 0xfff) == TAG_GEOMETRY
                else 'sizelimit' if (tag & 0xfff) == TAG_SIZELIMIT
                else 'namelimit' if (tag & 0xfff) == TAG_NAMELIMIT
                else 'config 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6f00) == TAG_GDELTA:
        return '%s%s%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            'grmdelta' if (tag & 0xfff) == TAG_GRMDELTA
                else 'gdelta 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6f00) == TAG_NAME:
        return '%s%s%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            'name' if (tag & 0xfff) == TAG_NAME
                else 'reg' if (tag & 0xfff) == TAG_REG
                else 'dir' if (tag & 0xfff) == TAG_DIR
                else 'orphan' if (tag & 0xfff) == TAG_ORPHAN
                else 'bookmark' if (tag & 0xfff) == TAG_BOOKMARK
                else 'name 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6f00) == TAG_STRUCT:
        return '%s%s%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            'data' if (tag & 0xfff) == TAG_DATA
                else 'block' if (tag & 0xfff) == TAG_BLOCK
                else 'bshrub' if (tag & 0xfff) == TAG_BSHRUB
                else 'btree' if (tag & 0xfff) == TAG_BTREE
                else 'did' if (tag & 0xfff) == TAG_DID
                else 'becksum' if (tag & 0xfff) == TAG_BECKSUM
                else 'branch' if (tag & 0xfff) == TAG_BRANCH
                else 'mroot' if (tag & 0xfff) == TAG_MROOT
                else 'mdir' if (tag & 0xfff) == TAG_MDIR
                else 'mtree' if (tag & 0xfff) == TAG_MTREE
                else 'struct 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6e00) == TAG_UATTR:
        return '%suattr 0x%02x%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            ((tag & 0x100) >> 1) | (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x6e00) == TAG_SATTR:
        return '%ssattr 0x%02x%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            ((tag & 0x100) >> 1) | (tag & 0xff),
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_CKSUM:
        return 'cksum 0x%02x%s%s' % (
            tag & 0xff,
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif (tag & 0x7f00) == TAG_ECKSUM:
        return 'ecksum%s%s%s' % (
            ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
            ' w%d' % w if w else '',
            ' %s' % size if size is not None else '')
    elif tag & TAG_ALT:
        return 'alt%s%s%s%s%s' % (
            'r' if tag & TAG_R else 'b',
            'a' if tag & 0x0fff == 0 and tag & TAG_GT
                else 'n' if tag & 0x0fff == 0
                else 'gt' if tag & TAG_GT
                else 'le',
            ' 0x%x' % (tag & 0x0fff) if tag & 0x0fff != 0 else '',
            ' w%d' % w if w is not None else '',
            ' 0x%x' % (0xffffffff & (off-size))
                if size is not None and off is not None
                else ' -%d' % size if size is not None
                else '')
    else:
        return '0x%04x%s%s' % (
            tag,
            ' w%d' % w if w is not None else '',
            ' %d' % size if size is not None else '')


def dbg_log(data, block_size, rev, eoff, weight, *,
        color=False,
        **args):
    cksum = crc32c(data[0:4])

    # preprocess jumps
    if args.get('jumps'):
        jumps = []
        j_ = 4
        while j_ < (block_size if args.get('all') else eoff):
            j = j_
            v, tag, w, size, d = fromtag(data[j_:])
            j_ += d
            if not tag & TAG_ALT:
                j_ += size

            # skip alt-nevers
            if tag & TAG_ALT and tag & ~TAG_R != TAG_ALT:
                # figure out which alt color
                if tag & TAG_R:
                    _, ntag, _, _, _ = fromtag(data[j_:])
                    if ntag & TAG_R:
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
    lifetime_width = 0
    if args.get('lifetimes'):
        class Lifetime:
            color_i = 0
            def __init__(self, j):
                self.origin = j
                self.tags = set()
                self.color = COLORS[self.__class__.color_i]
                self.__class__.color_i = (
                    self.__class__.color_i + 1) % len(COLORS)

            def add(self, j):
                self.tags.add(j)

            def __bool__(self):
                return bool(self.tags)


        # first figure out where each rid comes from
        weights = []
        lifetimes = []
        def index(weights, rid):
            for i, w in enumerate(weights):
                if rid < w:
                    return i, rid
                rid -= w
            return len(weights), 0

        checkpoint_js = [0]
        checkpoints = [([], [], set(), set(), set())]
        def checkpoint(j, weights, lifetimes, grows, shrinks, tags):
            checkpoint_js.append(j)
            checkpoints.append((
                weights.copy(), lifetimes.copy(),
                grows, shrinks, tags))

        lower_, upper_ = 0, 0
        weight_ = 0
        wastrunk = False
        j_ = 4
        while j_ < (block_size if args.get('all') else eoff):
            j = j_
            v, tag, w, size, d = fromtag(data[j_:])
            j_ += d
            if not tag & TAG_ALT:
                j_ += size

            # evaluate trunks
            if (tag & 0xf000) != TAG_CKSUM:
                if not wastrunk:
                    wastrunk = True
                    lower_, upper_ = 0, 0

                if (tag & 0xf000) == TAG_ALT:
                    lower_ += w
                else:
                    upper_ += w

                if not tag & TAG_ALT:
                    wastrunk = False
                    # derive the current tag's rid from alt weights
                    delta = (lower_+upper_) - weight_
                    weight_ = lower_+upper_
                    rid = lower_ + w-1

            if (tag & 0xf000) != TAG_CKSUM and not tag & TAG_ALT:
                # note we ignore out-of-bounds here for debugging
                if delta > 0:
                    # grow lifetimes
                    i, rid_ = index(weights, lower_)
                    if rid_ > 0:
                        weights[i:i+1] = [rid_, delta, weights[i]-rid_]
                        lifetimes[i:i+1] = [
                            lifetimes[i], Lifetime(j), lifetimes[i]]
                    else:
                        weights[i:i] = [delta]
                        lifetimes[i:i] = [Lifetime(j)]

                    checkpoint(j, weights, lifetimes, {i}, set(), {i})

                elif delta < 0:
                    # shrink lifetimes
                    i, rid_ = index(weights, lower_)
                    delta_ = -delta
                    weights_ = weights.copy()
                    lifetimes_ = lifetimes.copy()
                    shrinks = set()
                    while delta_ > 0 and i < len(weights_):
                        if weights_[i] > delta_:
                            delta__ = min(delta_, weights_[i]-rid_)
                            delta_ -= delta__
                            weights_[i] -= delta__
                            i += 1
                            rid_ = 0
                        else:
                            delta_ -= weights_[i]
                            weights_[i:i+1] = []
                            lifetimes_[i:i+1] = []
                            shrinks.add(i + len(shrinks))

                    checkpoint(j, weights, lifetimes, set(), shrinks, {i})
                    weights = weights_
                    lifetimes = lifetimes_

                if rid >= 0:
                    # attach tag to lifetime
                    i, rid_ = index(weights, rid)
                    if i < len(weights):
                        lifetimes[i].add(j)

                    if delta == 0:
                        checkpoint(j, weights, lifetimes, set(), set(), {i})

        lifetime_width = 2*max((
            sum(1 for lifetime in lifetimes if lifetime)
            for _, lifetimes, _, _, _ in checkpoints),
            default=0)

        def lifetimerepr(j):
            x = bisect.bisect(checkpoint_js, j)-1
            j_ = checkpoint_js[x]
            weights, lifetimes, grows, shrinks, tags = checkpoints[x]

            reprs = []
            colors = []
            was = None
            for i, (w, lifetime) in enumerate(zip(weights, lifetimes)):
                # skip lifetimes with no tags and shrinks
                if not lifetime or (j != j_ and i in shrinks):
                    if i in grows or i in shrinks or i in tags:
                        tags = tags.copy()
                        tags.add(i+1)
                    continue

                if j == j_ and i in grows:
                    reprs.append('.')
                    was = 'grow'
                elif j == j_ and i in shrinks:
                    reprs.append('\'')
                    was = 'shrink'
                elif j == j_ and i in tags:
                    reprs.append('* ')
                elif was == 'grow':
                    reprs.append('\\ ')
                elif was == 'shrink':
                    reprs.append('/ ')
                else:
                    reprs.append('| ')

                colors.append(lifetime.color)

            return '%s%*s' % (
                ''.join('%s%s%s' % (
                    '\x1b[%sm' % c if color else '',
                    r,
                    '\x1b[m' if color else '')
                    for r, c in zip(reprs, colors)),
                lifetime_width - sum(len(r) for r in reprs), '')


    # dynamically size the id field
    #
    # we need to do an additional pass to find this since our rbyd weight
    # does not include any shrub trees
    weight_ = 0
    weight__ = 0
    wastrunk = False
    j_ = 4
    while j_ < (block_size if args.get('all') else eoff):
        j = j_
        v, tag, w, size, d = fromtag(data[j_:])
        j_ += d

        if not tag & TAG_ALT:
            j_ += size

        # evaluate trunks
        if (tag & 0xf000) != TAG_CKSUM:
            if not wastrunk:
                wastrunk = True
                weight__ = 0

            weight__ += w

            if not tag & TAG_ALT:
                wastrunk = False
                # found new weight?
                weight_ = max(weight_, weight__)

    w_width = m.ceil(m.log10(max(1, weight_)+1))

    # print revision count
    if args.get('raw'):
        print('%8s: %*s%*s %s' % (
            '%04x' % 0,
            lifetime_width, '',
            2*w_width+1, '',
            next(xxd(data[0:4]))))

    # print tags
    lower_, upper_ = 0, 0
    wastrunk = False
    j_ = 4
    while j_ < (block_size if args.get('all') else eoff):
        notes = []

        j = j_
        v, tag, w, size, d = fromtag(data[j_:])
        if v != (popc(cksum) & 1):
            notes.append('v!=%x' % (popc(cksum) & 1))
        cksum = crc32c(data[j_:j_+d], cksum)
        j_ += d

        # take care of cksums
        if not tag & TAG_ALT:
            if (tag & 0xff00) != TAG_CKSUM:
                cksum = crc32c(data[j_:j_+size], cksum)
            # found a cksum?
            else:
                cksum_ = fromle32(data[j_:j_+4])
                if cksum != cksum_:
                    notes.append('cksum!=%08x' % cksum)
            j_ += size

        # evaluate trunks
        if (tag & 0xf000) != TAG_CKSUM:
            if not wastrunk:
                wastrunk = True
                lower_, upper_ = 0, 0

            if (tag & 0xe000) == TAG_ALT:
                lower_ += w
            else:
                upper_ += w

            if not tag & TAG_ALT:
                wastrunk = False
                # derive the current tag's rid from alt weights
                rid = lower_ + w-1

        # show human-readable tag representation
        print('%s%08x:%s %*s%s%*s %-*s%s%s%s' % (
            '\x1b[90m' if color and j >= eoff else '',
            j,
            '\x1b[m' if color and j >= eoff else '',
            lifetime_width, lifetimerepr(j) if args.get('lifetimes') else '',
            '\x1b[90m' if color and j >= eoff else '',
            2*w_width+1, '' if (tag & 0xe000) != 0x0000
                else '%d-%d' % (rid-(w-1), rid) if w > 1
                else rid,
            56+w_width, '%-*s  %s' % (
                21+w_width, tagrepr(tag, w, size, j),
                next(xxd(data[j+d:j+d+min(size, 8)], 8), '')
                    if not args.get('raw') and not args.get('no_truncate')
                        and not tag & TAG_ALT else ''),
            ' (%s)' % ', '.join(notes) if notes else '',
            '\x1b[m' if color and j >= eoff else '',
            ' %s' % jumprepr(j)
                if args.get('jumps') and not notes else ''))

        # show in-device representation, including some extra
        # cksum/parity info
        if args.get('device'):
            print('%s%8s  %*s%*s %-*s  %08x %x%s' % (
                '\x1b[90m' if color and j >= eoff else '',
                '',
                lifetime_width, '',
                2*w_width+1, '',
                21+w_width, '%04x %08x %07x' % (tag, w, size),
                cksum,
                popc(cksum) & 1,
                '\x1b[m' if color and j >= eoff else ''))

        # show on-disk encoding of tags
        if args.get('raw'):
            for o, line in enumerate(xxd(data[j:j+d])):
                print('%s%8s: %*s%*s %s%s' % (
                    '\x1b[90m' if color and j >= eoff else '',
                    '%04x' % (j + o*16),
                    lifetime_width, '',
                    2*w_width+1, '',
                    line,
                    '\x1b[m' if color and j >= eoff else ''))
        if args.get('raw') or args.get('no_truncate'):
            if not tag & TAG_ALT:
                for o, line in enumerate(xxd(data[j+d:j+d+size])):
                    print('%s%8s: %*s%*s %s%s' % (
                        '\x1b[90m' if color and j >= eoff else '',
                        '%04x' % (j+d + o*16),
                        lifetime_width, '',
                        2*w_width+1, '',
                        line,
                        '\x1b[m' if color and j >= eoff else ''))


def dbg_tree(data, block_size, rev, trunk, weight, *,
        color=False,
        **args):
    if not trunk:
        return

    # lookup a tag, returning also the search path for decoration
    # purposes
    def lookup(rid, tag):
        tag = max(tag, 0x1)
        lower = 0
        upper = weight
        path = []

        # descend down tree
        j = trunk
        while True:
            _, alt, w, jump, d = fromtag(data[j:])

            # found an alt?
            if alt & TAG_ALT:
                # follow?
                if ((rid, tag & 0xfff) > (upper-w-1, alt & 0xfff)
                        if alt & TAG_GT
                        else ((rid, tag & 0xfff) <= (lower+w-1, alt & 0xfff))):
                    lower += upper-lower-w if alt & TAG_GT else 0
                    upper -= upper-lower-w if not alt & TAG_GT else 0
                    j = j - jump

                    # figure out which color
                    if alt & TAG_R:
                        _, nalt, _, _, _ = fromtag(data[j+jump+d:])
                        if nalt & TAG_R:
                            path.append((j+jump, j, True, 'y'))
                        else:
                            path.append((j+jump, j, True, 'r'))
                    else:
                        path.append((j+jump, j, True, 'b'))

                # stay on path
                else:
                    lower += w if not alt & TAG_GT else 0
                    upper -= w if alt & TAG_GT else 0
                    j = j + d

                    # figure out which color
                    if alt & TAG_R:
                        _, nalt, _, _, _ = fromtag(data[j:])
                        if nalt & TAG_R:
                            path.append((j-d, j, False, 'y'))
                        else:
                            path.append((j-d, j, False, 'r'))
                    else:
                        path.append((j-d, j, False, 'b'))

            # found tag
            else:
                rid_ = upper-1
                tag_ = alt
                w_ = upper-lower

                done = not tag_ or (rid_, tag_) < (rid, tag)

                return done, rid_, tag_, w_, j, d, jump, path

    # precompute tree
    t_width = 0
    if args.get('tree') or args.get('rbyd'):
        trunks = co.defaultdict(lambda: (-1, 0))
        alts = co.defaultdict(lambda: {})

        rid, tag = -1, 0
        while True:
            done, rid, tag, w, j, d, size, path = lookup(rid, tag+0x1)
            # found end of tree?
            if done:
                break

            # keep track of trunks/alts
            trunks[j] = (rid, tag)

            for j_, j__, followed, c in path:
                if followed:
                    alts[j_] |= {'f': j__, 'c': c}
                else:
                    alts[j_] |= {'nf': j__, 'c': c}

        if args.get('rbyd'):
            # treat unreachable alts as converging paths
            for j_, alt in alts.items():
                if 'f' not in alt:
                    alt['f'] = alt['nf']
                elif 'nf' not in alt:
                    alt['nf'] = alt['f']

        else:
            # prune any alts with unreachable edges
            pruned = {}
            for j_, alt in alts.items():
                if 'f' not in alt:
                    pruned[j_] = alt['nf']
                elif 'nf' not in alt:
                    pruned[j_] = alt['f']
            for j_ in pruned.keys():
                del alts[j_]

            for j_, alt in alts.items():
                while alt['f'] in pruned:
                    alt['f'] = pruned[alt['f']]
                while alt['nf'] in pruned:
                    alt['nf'] = pruned[alt['nf']]

        # find the trunk and depth of each alt
        def rec_trunk(j_):
            if j_ not in alts:
                return trunks[j_]
            else:
                if 'nft' not in alts[j_]:
                    alts[j_]['nft'] = rec_trunk(alts[j_]['nf'])
                return alts[j_]['nft']

        for j_ in alts.keys():
            rec_trunk(j_)
        for j_, alt in alts.items():
            if alt['f'] in alts:
                alt['ft'] = alts[alt['f']]['nft']
            else:
                alt['ft'] = trunks[alt['f']]

        def rec_height(j_):
            if j_ not in alts:
                return 0
            else:
                if 'h' not in alts[j_]:
                    alts[j_]['h'] = max(
                        rec_height(alts[j_]['f']),
                        rec_height(alts[j_]['nf'])) + 1
                return alts[j_]['h']

        for j_ in alts.keys():
            rec_height(j_)

        t_depth = max((alt['h']+1 for alt in alts.values()), default=0)

        # convert to more general tree representation
        TBranch = co.namedtuple('TBranch', 'a, b, d, c')
        tree = set()
        for j, alt in alts.items():
            # note all non-trunk edges should be black
            tree.add(TBranch(
                a=alt['nft'],
                b=alt['nft'],
                d=t_depth-1 - alt['h'],
                c=alt['c'],
            ))
            tree.add(TBranch(
                a=alt['nft'],
                b=alt['ft'],
                d=t_depth-1 - alt['h'],
                c='b',
            ))

        # find the max depth from the tree
        t_depth = max((branch.d+1 for branch in tree), default=0)
        if t_depth > 0:
            t_width = 2*t_depth + 2

        def treerepr(rid, tag):
            if t_depth == 0:
                return ''

            def branchrepr(x, d, was):
                for branch in tree:
                    if branch.d == d and branch.b == x:
                        if any(branch.d == d and branch.a == x
                                for branch in tree):
                            return '+-', branch.c, branch.c
                        elif any(branch.d == d
                                and x > min(branch.a, branch.b)
                                and x < max(branch.a, branch.b)
                                for branch in tree):
                            return '|-', branch.c, branch.c
                        elif branch.a < branch.b:
                            return '\'-', branch.c, branch.c
                        else:
                            return '.-', branch.c, branch.c
                for branch in tree:
                    if branch.d == d and branch.a == x:
                        return '+ ', branch.c, None
                for branch in tree:
                    if (branch.d == d
                            and x > min(branch.a, branch.b)
                            and x < max(branch.a, branch.b)):
                        return '| ', branch.c, was
                if was:
                    return '--', was, was
                return '  ', None, None

            trunk = []
            was = None
            for d in range(t_depth):
                t, c, was = branchrepr((rid, tag), d, was)

                trunk.append('%s%s%s%s' % (
                    '\x1b[33m' if color and c == 'y'
                        else '\x1b[31m' if color and c == 'r'
                        else '\x1b[90m' if color and c == 'b'
                        else '',
                    t,
                    ('>' if was else ' ') if d == t_depth-1 else '',
                    '\x1b[m' if color and c else ''))

            return '%s ' % ''.join(trunk)


    # dynamically size the id field
    w_width = m.ceil(m.log10(max(1, weight)+1))

    rid, tag = -1, 0
    for i in it.count():
        done, rid, tag, w, j, d, size, path = lookup(rid, tag+0x1)
        # found end of tree?
        if done:
            break

        # show human-readable tag representation
        print('%08x: %s%*s %-*s  %s' % (
            j,
            treerepr(rid, tag)
                if args.get('tree') or args.get('rbyd') else '',
            2*w_width+1, '%d-%d' % (rid-(w-1), rid)
                if w > 1 else rid
                if w > 0 or i == 0 else '',
            21+w_width, tagrepr(tag, w, size, j),
            next(xxd(data[j+d:j+d+min(size, 8)], 8), '')
                if not args.get('raw') and not args.get('no_truncate')
                    and not tag & TAG_ALT else ''))

        # show in-device representation
        if args.get('device'):
            print('%8s  %*s%*s %04x %08x %07x' % (
                '',
                t_width, '',
                2*w_width+1, '',
                tag, w, size))

        # show on-disk encoding of tags
        if args.get('raw'):
            for o, line in enumerate(xxd(data[j:j+d])):
                print('%8s: %*s%*s %s' % (
                    '%04x' % (j + o*16),
                    t_width, '',
                    2*w_width+1, '',
                    line))
        if args.get('raw') or args.get('no_truncate'):
            if not tag & TAG_ALT:
                for o, line in enumerate(xxd(data[j+d:j+d+size])):
                    print('%8s: %*s%*s %s' % (
                        '%04x' % (j+d + o*16),
                        t_width, '',
                        2*w_width+1, '',
                        line))


def main(disk, blocks=None, *,
        block_size=None,
        block_count=None,
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

    # is bd geometry specified?
    if isinstance(block_size, tuple):
        block_size, block_count_ = block_size
        if block_count is None:
            block_count = block_count_

    # flatten blocks, default to block 0
    if not blocks:
        blocks = [[0]]
    blocks = [block for blocks_ in blocks for block in blocks_]

    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # blocks may also encode trunks 
        blocks, trunks = (
            [block[0] if isinstance(block, tuple) else block
                for block in blocks],
            [trunk if trunk is not None
                    else block[1] if isinstance(block, tuple)
                    else None
                for block in blocks])

        # read each block
        datas = []
        for block in blocks:
            f.seek(block * block_size)
            datas.append(f.read(block_size))

    # first figure out which block as the most recent revision
    def fetch(data, trunk):
        rev = fromle32(data[0:4])
        cksum = 0
        cksum_ = crc32c(data[0:4])
        eoff = 0
        j_ = 4
        trunk_ = 0
        trunk__ = 0
        trunk___ = 0
        weight = 0
        weight_ = 0
        weight__ = 0
        wastrunk = False
        trunkeoff = None 
        while j_ < len(data) and (not trunk or eoff <= trunk):
            v, tag, w, size, d = fromtag(data[j_:])
            if v != (popc(cksum_) & 1):
                break
            cksum_ = crc32c(data[j_:j_+d], cksum_)
            j_ += d
            if not tag & TAG_ALT and j_ + size > len(data):
                break

            # take care of cksums
            if not tag & TAG_ALT:
                if (tag & 0xff00) != TAG_CKSUM:
                    cksum_ = crc32c(data[j_:j_+size], cksum_)
                # found a cksum?
                else:
                    cksum__ = fromle32(data[j_:j_+4])
                    if cksum_ != cksum__:
                        break
                    # commit what we have
                    eoff = trunkeoff if trunkeoff else j_ + size
                    cksum = cksum_
                    trunk_ = trunk__
                    weight = weight_

            # evaluate trunks
            if (tag & 0xf000) != TAG_CKSUM and (
                    not trunk or trunk >= j_-d or wastrunk):
                # new trunk?
                if not wastrunk:
                    wastrunk = True
                    trunk___ = j_-d
                    weight__ = 0

                # keep track of weight
                weight__ += w

                # end of trunk?
                if not tag & TAG_ALT:
                    wastrunk = False
                    # update trunk/weight unless we found a shrub or an
                    # explicit trunk (which may be a shrub) is requested
                    if not tag & TAG_SHRUB or trunk:
                        trunk__ = trunk___
                        weight_ = weight__
                        # keep track of eoff for best matching trunk
                        if trunk and j_ + size > trunk:
                            trunkeoff = j_ + size
                            eoff = trunkeoff
                            cksum = cksum_
                            trunk_ = trunk__
                            weight = weight_

            if not tag & TAG_ALT:
                j_ += size

        return rev, eoff, trunk_, weight

    revs, eoffs, trunks_, weights = [], [], [], []
    i = 0
    for i_, (data, trunk_) in enumerate(zip(datas, trunks)):
        rev, eoff, trunk_, weight = fetch(data, trunk_)
        revs.append(rev)
        eoffs.append(eoff)
        trunks_.append(trunk_)
        weights.append(weight)

        # compare with sequence arithmetic
        if trunk_ and (
                not trunks_[i]
                or not ((rev - revs[i]) & 0x80000000)
                or (rev == revs[i] and trunk_ > trunks_[i])):
            i = i_

    # print contents of the winning metadata block
    block, data, rev, eoff, trunk_, weight = (
        blocks[i], datas[i], revs[i], eoffs[i], trunks_[i], weights[i])

    print('rbyd %s, rev %d, size %d, weight %d' % (
        '0x%x.%x' % (block, trunk_)
            if len(blocks) == 1
            else '0x{%x,%s}.%x' % (
                block,
                ','.join('%x' % blocks[(i+1+j) % len(blocks)]
                    for j in range(len(blocks)-1)),
                trunk_),
        rev, eoff, weight))

    if args.get('log'):
        dbg_log(data, block_size, rev, eoff, weight,
            color=color,
            **args)
    else:
        dbg_tree(data, block_size, rev, trunk_, weight,
            color=color,
            **args)

    if args.get('error_on_corrupt') and eoff == 0:
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
        'blocks',
        nargs='*',
        type=rbydaddr,
        help="Block address of metadata blocks.")
    parser.add_argument(
        '-b', '--block-size',
        type=bdgeom,
        help="Block size/geometry in bytes.")
    parser.add_argument(
        '--block-count',
        type=lambda x: int(x, 0),
        help="Block count in blocks.")
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
        '-t', '--tree',
        action='store_true',
        help="Show the rbyd tree.")
    parser.add_argument(
        '-R', '--rbyd',
        action='store_true',
        help="Show the full rbyd tree.")
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
