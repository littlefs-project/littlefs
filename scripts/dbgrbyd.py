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


TAG_UNR         = 0x0002
TAG_SUPERMAGIC  = 0x0030
TAG_SUPERCONFIG = 0x0040
TAG_SUPERMDIR   = 0x0110
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
    elif (tag & 0xfffc) == TAG_SUPERMAGIC:
        return '%ssupermagic%s %d' % (
            'rm' if tag & 0x2 else '',
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xfffc) == TAG_SUPERCONFIG:
        return '%ssuperconfig%s %d' % (
            'rm' if tag & 0x2 else '',
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xfffc) == TAG_SUPERMDIR:
        return '%ssupermdir%s %d' % (
            'rm' if tag & 0x2 else '',
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xf00c) == TAG_NAME:
        return '%s%s%s %d' % (
            'rm' if tag & 0x2 else '',
            'branch' if (tag & 0xfffe) == TAG_BRANCH
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

def show_log(data, block_size, rev, off, weight, *,
        color=False,
        **args):
    crc = crc32c(data[0:4])

    # preprocess jumps
    if args.get('jumps'):
        jumps = []
        j_ = 4
        while j_ < (block_size if args.get('all') else off):
            j = j_
            v, tag, w, size, d = fromtag(data[j_:])
            j_ += d
            if not tag & 0x8:
                j_ += size

            if tag & 0x8:
                # figure out which alt color
                if tag & 0x2:
                    _, ntag, _, _, _ = fromtag(data[j_:])
                    if ntag & 0x2:
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


        # first figure out where each id comes from
        weights = []
        lifetimes = []
        def index(weights, id):
            for i, w in enumerate(weights):
                if id < w:
                    return i, id
                id -= w
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
        while j_ < (block_size if args.get('all') else off):
            j = j_
            v, tag, w, size, d = fromtag(data[j_:])
            j_ += d
            if not tag & 0x8:
                j_ += size

            # find trunk
            if not wastrunk and (tag & 0xc) != 0x4:
                lower_, upper_ = 0, 0
            wastrunk = not not tag & 0x8

            # keep track of weight
            if tag & 0x8:
                if tag & 0x4:
                    upper_ += w
                else:
                    lower_ += w
            elif (tag & 0xc) == 0x0:
                delta = (lower_+upper_+w) - weight_
                weight_ = lower_+upper_+w
                id = lower_+w-1

                # note we ignore out-of-bounds here for debugging
                if delta > 0:
                    # grow lifetimes
                    i, id_ = index(weights, lower_)
                    if id_ > 0:
                        weights[i:i+1] = [id_, delta, weights[i]-id_]
                        lifetimes[i:i+1] = [
                            lifetimes[i], Lifetime(j), lifetimes[i]]
                    else:
                        weights[i:i] = [delta]
                        lifetimes[i:i] = [Lifetime(j)]

                    checkpoint(j, weights, lifetimes, {i}, set(), {i})

                elif delta < 0:
                    # shrink lifetimes
                    i, id_ = index(weights, lower_)
                    delta_ = -delta
                    weights_ = weights.copy()
                    lifetimes_ = lifetimes.copy()
                    shrinks = set()
                    while delta_ > 0 and i < len(weights_):
                        if weights_[i] > delta_:
                            delta__ = min(delta_, weights_[i]-id_)
                            delta_ -= delta__
                            weights_[i] -= delta__
                            i += 1
                            id_ = 0
                        else:
                            delta_ -= weights_[i]
                            weights_[i:i+1] = []
                            lifetimes_[i:i+1] = []
                            shrinks.add(i + len(shrinks))

                    checkpoint(j, weights, lifetimes, set(), shrinks, {i})
                    weights = weights_
                    lifetimes = lifetimes_

                if not tag & 0x2 and id >= 0:
                    # attach tag to lifetime
                    i, id_ = index(weights, id)
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


    # print header
    w_width = 2*m.ceil(m.log10(max(1, weight)+1))+1
    print('%-8s  %*s%-*s %-22s  %s' % (
        'off',
        lifetime_width, '',
        w_width, 'ids',
        'tag',
        'data (truncated)'
            if not args.get('no_truncate') else ''))

    # print revision count
    if args.get('raw'):
        print('%8s: %*s%*s %s' % (
            '%04x' % 0,
            lifetime_width, '',
            w_width, '',
            next(xxd(data[0:4]))))

    # print tags
    lower_, upper_ = 0, 0
    wastrunk = False
    j_ = 4
    while j_ < (block_size if args.get('all') else off):
        notes = []

        j = j_
        v, tag, w, size, d = fromtag(data[j_:])
        if v != (popc(crc) & 1):
            notes.append('v!=%x' % (popc(crc) & 1))
        tag &= ~1
        crc = crc32c(data[j_:j_+d], crc)
        j_ += d

        # find trunk
        if not wastrunk and (tag & 0xc) != 0x4:
            lower_, upper_ = 0, 0
        wastrunk = not not tag & 0x8

        # calculate id from alt weights
        if tag & 0x8:
            if tag & 0x4:
                upper_ += w
            else:
                lower_ += w
        elif (tag & 0xc) == 0x0:
            weight_ = lower_+upper_+w
            id = lower_+w-1

        if not tag & 0x8:
            if (tag & 0xf00f) != TAG_CRC:
                crc = crc32c(data[j_:j_+size], crc)
            # found a crc?
            else:
                crc_ = fromle32(data[j_:j_+4])
                if crc != crc_:
                    notes.append('crc!=%08x' % crc)
            j_ += size

        # show human-readable tag representation
        print('%s%08x:%s %*s%s%*s %-57s%s%s' % (
            '\x1b[90m' if color and j >= off else '',
            j,
            '\x1b[m' if color and j >= off else '',
            lifetime_width, lifetimerepr(j) if args.get('lifetimes') else '',
            '\x1b[90m' if color and j >= off else '',
            w_width, '' if (tag & 0xc) != 0x0
                else '%d-%d' % (id-(w-1), id) if w > 1
                else id,
            '%-22s%s' % (
                tagrepr(tag, w, size, j),
                '  %s' % next(xxd(
                        data[j+d:j+d+min(size, 8)], 8), '')
                    if not args.get('no_truncate')
                        and not tag & 0x8 else ''),
            '\x1b[m' if color and j >= off else '',
            ' (%s)' % ', '.join(notes) if notes
            else ' %s' % jumprepr(j)
                if args.get('jumps')
            else ''))

        # show in-device representation, including some extra
        # crc/parity info
        if args.get('device'):
            print('%s%8s  %*s%*s %-47s  %08x %x%s' % (
                '\x1b[90m' if color and j >= off else '',
                '',
                lifetime_width, '',
                w_width, '',
                '%-22s%s' % (
                    '%04x %08x %07x' % (tag, w, size),
                    '  %s' % ' '.join(
                            '%08x' % fromle32(
                                data[j+d+i*4:j+d+min(i*4+4,size)])
                            for i in range(min(m.ceil(size/4), 3)))[:23]
                        if not args.get('no_truncate')
                            and not tag & 0x8 else ''),
                crc,
                popc(crc) & 1,
                '\x1b[m' if color and j >= off else ''))

        # show on-disk encoding of tags
        if args.get('raw'):
            for o, line in enumerate(xxd(data[j:j+d])):
                print('%s%8s: %*s%*s %s%s' % (
                    '\x1b[90m' if color and j >= off else '',
                    '%04x' % (j + o*16),
                    lifetime_width, '',
                    w_width, '',
                    line,
                    '\x1b[m' if color and j >= off else ''))
        if args.get('raw') or args.get('no_truncate'):
            if not tag & 0x8:
                for o, line in enumerate(xxd(data[j+d:j+d+size])):
                    print('%s%8s: %*s%*s %s%s' % (
                        '\x1b[90m' if color and j >= off else '',
                        '%04x' % (j+d + o*16),
                        lifetime_width, '',
                        w_width, '',
                        line,
                        '\x1b[m' if color and j >= off else ''))


def show_tree(data, block_size, rev, trunk, weight, *,
        color=False,
        **args):
    if not trunk:
        return

    # lookup a tag, returning also the search path for decoration
    # purposes
    def lookup(id, tag):
        lower = -1
        upper = weight
        path = []

        # descend down tree
        j = trunk
        while True:
            _, alt, w, jump, d = fromtag(data[j:])

            # found an alt?
            if alt & 0x8:
                # follow?
                if ((id, tag & ~0xf) > (upper-w-1, alt & ~0xf)
                        if alt & 0x4
                        else ((id, tag & ~0xf) <= (lower+w, alt & ~0xf))):
                    lower += upper-lower-1-w if alt & 0x4 else 0
                    upper -= upper-lower-1-w if not alt & 0x4 else 0
                    j = j - jump

                    if args.get('tree'):
                        # figure out which color
                        if alt & 0x2:
                            _, nalt, _, _, _ = fromtag(data[j+jump+d:])
                            if nalt & 0x2:
                                path.append((j+jump, j, True, 'y'))
                            else:
                                path.append((j+jump, j, True, 'r'))
                        else:
                            path.append((j+jump, j, True, 'b'))
                # stay on path
                else:
                    lower += w if not alt & 0x4 else 0
                    upper -= w if alt & 0x4 else 0
                    j = j + d

                    if args.get('tree'):
                        # figure out which color
                        if alt & 0x2:
                            _, nalt, _, _, _ = fromtag(data[j:])
                            if nalt & 0x2:
                                path.append((j-d, j, False, 'y'))
                            else:
                                path.append((j-d, j, False, 'r'))
                        else:
                            path.append((j-d, j, False, 'b'))
            # found tag
            else:
                id_ = upper-1
                tag_ = alt
                w_ = id_-lower

                done = (id_, tag_) < (id, tag) or tag_ & 2

                return done, id_, tag_, w_, j, d, jump, path

    # precompute tree
    tree_width = 0
    if args.get('tree'):
        trunks = co.defaultdict(lambda: (-1, 0))
        alts = co.defaultdict(lambda: {})

        id, tag = -1, 0
        while True:
            done, id, tag, w, j, d, size, path = lookup(id, tag+0x10)
            # found end of tree?
            if done:
                break

            # keep track of trunks/alts
            trunks[j] = (id, tag)

            for j_, j__, followed, c in path:
                if followed:
                    alts[j_] |= {'f': j__, 'c': c}
                else:
                    alts[j_] |= {'nf': j__, 'c': c}

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

        # find the trunk and depth of each alt, assuming pruned alts
        # didn't exist
        def rec_trunk(j_):
            if j_ not in alts:
                return j_
            else:
                if 't' not in alts[j_]:
                    alts[j_]['t'] = rec_trunk(alts[j_]['nf'])
                return alts[j_]['t']

        for j_ in alts.keys():
            rec_trunk(j_)
        for j_, alt in alts.items():
            if alt['f'] in alts:
                alt['ft'] = alts[alt['f']]['t']
            else:
                alt['ft'] = alt['f']

        def rec_depth(j_):
            if j_ not in alts:
                return 0
            else:
                if 'd' not in alts[j_]:
                    alts[j_]['d'] = max(
                        rec_depth(alts[j_]['f']),
                        rec_depth(alts[j_]['nf'])) + 1
                return alts[j_]['d']

        for j_ in alts.keys():
            rec_depth(j_)

        # oh hey this also gives us the max depth
        tree_depth = max((alt['d'] for alt in alts.values()), default=0)
        if tree_depth > 0:
            tree_width = 2*tree_depth + 2

        def treerepr(j):
            if tree_depth == 0:
                return ''

            def c(s, c):
                return '%s%s%s' % (
                    '\x1b[33m' if color and c == 'y'
                        else '\x1b[31m' if color and c == 'r'
                        else '\x1b[90m' if color
                        else '',
                    s,
                    '\x1b[m' if color else '')

            trunk = []
            def altrepr(j, x, was=None):
                # note all non-trunk edges should be black
                for alt in alts.values():
                    if alt['d'] == x and alt['t'] == j:
                        return '+-', alt['c'], alt['c']
                for alt in alts.values():
                    if (alt['d'] == x
                            and alt['ft'] == j
                            and trunks[j] <= trunks[alt['t']]):
                        return '.-', 'b', 'b'
                for alt in alts.values():
                    if (alt['d'] == x
                            and alt['ft'] == j
                            and trunks[j] >= trunks[alt['t']]):
                        return '\'-', 'b', 'b'
                for alt in alts.values():
                    if (alt['d'] == x
                            and trunks[j] >= min(
                                trunks[alt['t']], trunks[alt['ft']])
                            and trunks[j] <= max(
                                trunks[alt['t']], trunks[alt['ft']])):
                        return '| ', 'b', was
                if was:
                    return '--', was, was
                return '  ', None, None

            was = None
            for x in reversed(range(1, tree_depth+1)):
                t, c, was = altrepr(j, x, was)

                trunk.append('%s%s%s%s' % (
                    '\x1b[33m' if color and c == 'y'
                        else '\x1b[31m' if color and c == 'r'
                        else '\x1b[90m' if color
                        else '',
                    t,
                    ('>' if was else ' ')
                        if x == 1 else '',
                    '\x1b[m' if color else ''))

            return ' %s' % ''.join(trunk)


    # print header
    w_width = 2*m.ceil(m.log10(max(1, weight)+1))+1
    print('%-8s  %*s%-*s %-22s  %s' % (
        'off',
        tree_width, '',
        w_width, 'ids',
        'tag',
        'data (truncated)'
            if not args.get('no_truncate') else ''))

    id, tag = -1, 0
    while True:
        done, id, tag, w, j, d, size, path = lookup(id, tag+0x10)
        # found end of tree?
        if done:
            break

        # show human-readable tag representation
        print('%08x:%s %-57s' % (
            j,
            treerepr(j) if args.get('tree') else '',
            '%*s %-22s%s' % (
                w_width, '%d-%d' % (id-(w-1), id)
                    if w > 1 else id
                    if w > 0 else '',
                tagrepr(tag, w, size, j),
                '  %s' % next(xxd(
                        data[j+d:j+d+min(size, 8)], 8), '')
                    if not args.get('no_truncate')
                        and not tag & 0x8 else '')))

        # show in-device representation
        if args.get('device'):
            print('%8s  %*s%*s %s' % (
                '',
                tree_width, '',
                w_width, '',
                '%-22s%s' % (
                    '%04x %08x %07x' % (tag, 0xffffffff & id, size),
                    '  %s' % ' '.join(
                            '%08x' % fromle32(
                                data[j+d+i*4:j+d+min(i*4+4,size)])
                            for i in range(min(m.ceil(size/4), 3)))[:23]
                        if not args.get('no_truncate')
                            and not tag & 0x8 else '')))

        # show on-disk encoding of tags
        if args.get('raw'):
            for o, line in enumerate(xxd(data[j:j+d])):
                print('%8s: %*s%*s %s' % (
                    '%04x' % (j + o*16),
                    tree_width, '',
                    w_width, '',
                    line))
        if args.get('raw') or args.get('no_truncate'):
            if not tag & 0x8:
                for o, line in enumerate(xxd(data[j+d:j+d+size])):
                    print('%8s: %*s%*s %s' % (
                        '%04x' % (j+d + o*16),
                        tree_width, '',
                        w_width, '',
                        line))


def main(disk, block1=0, block2=None, *,
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

    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # blocks may also encode trunks 
        blocks = [
            block[0] if isinstance(block, tuple) else block
            for block in [block1, block2]
            if block is not None]
        trunks = [
            trunk if trunk is not None
                else block[1] if isinstance(block, tuple)
                else None
            for block in [block1, block2]
            if block is not None]

        # read each block
        datas = []
        for block in blocks:
            f.seek(block * block_size)
            datas.append(f.read(block_size))

    # first figure out which block as the most recent revision
    def fetch(data, trunk):
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

        return rev, off, trunk_, weight

    revs, offs, trunks_, weights = [], [], [], []
    i = 0
    for data, trunk in zip(datas, trunks):
        rev, off, trunk_, weight = fetch(data, trunk)
        revs.append(rev)
        offs.append(off)
        trunks_.append(trunk_)
        weights.append(weight)

        # compare with sequence arithmetic
        if off and not ((rev - revs[i]) & 0x80000000):
            i = len(revs)-1

    # print contents of the winning metadata block
    block, data, rev, off, trunk, weight = (
        blocks[i], datas[i], revs[i], offs[i], trunks_[i], weights[i])

    print('rbyd 0x%x.%x, rev %d, size %d, weight %d%s' % (
        block, trunk, rev, off, weight,
        ' (was 0x%x.%x, %d, %d, %d)' % (
            blocks[~i], trunks_[~i], revs[~i], offs[~i], weights[~i])
            if len(blocks) > 1 else ''))

    if args.get('log'):
        show_log(data, block_size, rev, off, weight,
            color=color,
            **args)
    else:
        show_tree(data, block_size, rev, trunk, weight,
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
        'block1',
        nargs='?',
        type=rbydaddr,
        help="Block address of the first metadata block.")
    parser.add_argument(
        'block2',
        nargs='?',
        type=rbydaddr,
        help="Block address of the second metadata block.")
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
