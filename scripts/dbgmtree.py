#!/usr/bin/env python3

import bisect
import collections as co
import itertools as it
import math as m
import os
import struct


TAG_NULL        = 0x0000
TAG_CONFIG      = 0x0000
TAG_MAGIC       = 0x0003
TAG_VERSION     = 0x0004
TAG_RFLAGS      = 0x0005
TAG_WFLAGS      = 0x0006
TAG_OFLAGS      = 0x0007
TAG_BLOCKSIZE   = 0x0008
TAG_BLOCKCOUNT  = 0x0009
TAG_NAMELIMIT   = 0x000a
TAG_SIZELIMIT   = 0x000b
TAG_UTAGLIMIT   = 0x000c
TAG_UATTRLIMIT  = 0x000d
TAG_STAGLIMIT   = 0x000e
TAG_SATTRLIMIT  = 0x000f
TAG_MDIRLIMIT   = 0x0010
TAG_MTREELIMIT  = 0x0011
TAG_GSTATE      = 0x0100
TAG_GRM         = 0x0100
TAG_NAME        = 0x0200
TAG_BOOKMARK    = 0x0201
TAG_REG         = 0x0202
TAG_DIR         = 0x0203
TAG_STRUCT      = 0x0300
TAG_DATA        = 0x0300
TAG_TRUNK       = 0x0304
TAG_BLOCK       = 0x0308
TAG_BTREE       = 0x030c
TAG_BRANCH      = 0x031c
TAG_MDIR        = 0x0321
TAG_MTREE       = 0x0324
TAG_MROOT       = 0x0329
TAG_DID         = 0x032c
TAG_UATTR       = 0x0400
TAG_SATTR       = 0x0600
TAG_SHRUB       = 0x1000
TAG_CKSUM       = 0x3000
TAG_ECKSUM      = 0x3100
TAG_ALT         = 0x4000
TAG_GT          = 0x2000
TAG_R           = 0x1000


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

def frommdir(data):
    blocks = []
    d = 0
    while d < len(data):
        block, d_ = fromleb128(data[d:])
        blocks.append(block)
        d += d_
    return blocks

def frombranch(data):
    d = 0
    block, d_ = fromleb128(data[d:]); d += d_
    trunk, d_ = fromleb128(data[d:]); d += d_
    cksum = fromle32(data[d:]); d += 4
    return block, trunk, cksum

def frombtree(data):
    d = 0
    w, d_ = fromleb128(data[d:]); d += d_
    block, trunk, cksum = frombranch(data[d:])
    return w, block, trunk, cksum

def xxd(data, width=16):
    for i in range(0, len(data), width):
        yield '%-*s %-*s' % (
            3*width,
            ' '.join('%02x' % b for b in data[i:i+width]),
            width,
            ''.join(
                b if b >= ' ' and b <= '~' else '.'
                for b in map(chr, data[i:i+width])))

def tagrepr(tag, w, size, off=None):
    if (tag & 0xefff) == TAG_NULL:
        return '%snull%s%s' % (
            'shrub' if tag & TAG_SHRUB else '',
            ' w%d' % w if w else '',
            ' %d' % size if size else '')
    elif (tag & 0xef00) == TAG_CONFIG:
        return '%s%s%s %d' % (
            'shrub' if tag & TAG_SHRUB else '',
            'magic' if (tag & 0xfff) == TAG_MAGIC
                else 'version' if (tag & 0xfff) == TAG_VERSION
                else 'rflags' if (tag & 0xfff) == TAG_RFLAGS
                else 'wflags' if (tag & 0xfff) == TAG_WFLAGS
                else 'oflags' if (tag & 0xfff) == TAG_OFLAGS
                else 'blocksize' if (tag & 0xfff) == TAG_BLOCKSIZE
                else 'blockcount' if (tag & 0xfff) == TAG_BLOCKCOUNT
                else 'sizelimit' if (tag & 0xfff) == TAG_SIZELIMIT
                else 'namelimit' if (tag & 0xfff) == TAG_NAMELIMIT
                else 'utaglimit' if (tag & 0xfff) == TAG_UTAGLIMIT
                else 'uattrlimit' if (tag & 0xfff) == TAG_UATTRLIMIT
                else 'staglimit' if (tag & 0xfff) == TAG_STAGLIMIT
                else 'sattrlimit' if (tag & 0xfff) == TAG_SATTRLIMIT
                else 'mdirlimit' if (tag & 0xfff) == TAG_MDIRLIMIT
                else 'mtreelimit' if (tag & 0xfff) == TAG_MTREELIMIT
                else 'config 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xef00) == TAG_GSTATE:
        return '%s%s%s %d' % (
            'shrub' if tag & TAG_SHRUB else '',
            'grm' if (tag & 0xfff) == TAG_GRM
                else 'gstate 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xef00) == TAG_NAME:
        return '%s%s%s %d' % (
            'shrub' if tag & TAG_SHRUB else '',
            'name' if (tag & 0xfff) == TAG_NAME
                else 'bookmark' if (tag & 0xfff) == TAG_BOOKMARK
                else 'reg' if (tag & 0xfff) == TAG_REG
                else 'dir' if (tag & 0xfff) == TAG_DIR
                else 'name 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xef00) == TAG_STRUCT:
        return '%s%s%s %d' % (
            'shrub' if tag & TAG_SHRUB else '',
            'data' if (tag & 0xfff) == TAG_DATA
                else 'trunk' if (tag & 0xfff) == TAG_TRUNK
                else 'block' if (tag & 0xfff) == TAG_BLOCK
                else 'btree' if (tag & 0xfff) == TAG_BTREE
                else 'branch' if (tag & 0xfff) == TAG_BRANCH
                else 'mdir' if (tag & 0xfff) == TAG_MDIR
                else 'mtree' if (tag & 0xfff) == TAG_MTREE
                else 'mroot' if (tag & 0xfff) == TAG_MROOT
                else 'did' if (tag & 0xfff) == TAG_DID
                else 'struct 0x%02x' % (tag & 0xff),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xef00) == TAG_UATTR:
        return '%suattr 0x%02x%s %d' % (
            'shrub' if tag & TAG_SHRUB else '',
            ((tag & 0x100) >> 1) | (tag & 0xff),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xef00) == TAG_SATTR:
        return '%ssattr 0x%02x%s %d' % (
            'shrub' if tag & TAG_SHRUB else '',
            ((tag & 0x100) >> 1) | (tag & 0xff),
            ' w%d' % w if w else '',
            size)
    elif (tag & 0xff00) == TAG_CKSUM:
        return 'cksum 0x%02x%s %d' % (
            tag & 0xff,
            ' w%d' % w if w > 0 else '',
            size)
    elif (tag & 0xff00) == TAG_ECKSUM:
        return 'ecksum%s%s %d' % (
            ' 0x%02x' % (tag & 0xff) if tag & 0xff else '',
            ' w%d' % w if w > 0 else '',
            size)
    elif tag & TAG_ALT:
        return 'alt%s%s 0x%x w%d %s' % (
            'r' if tag & TAG_R else 'b',
            'gt' if tag & TAG_GT else 'le',
            tag & 0x0fff,
            w,
            '0x%x' % (0xffffffff & (off-size))
                if off is not None
                else '-%d' % off)
    else:
        return '0x%04x w%d %d' % (tag, w, size)


# this type is used for tree representations
TBranch = co.namedtuple('TBranch', 'a, b, d, c')

# our core rbyd type
class Rbyd:
    def __init__(self, block, data, rev, eoff, trunk, weight):
        self.block = block
        self.data = data
        self.rev = rev
        self.eoff = eoff
        self.trunk = trunk
        self.weight = weight
        self.redund_blocks = []

    def addr(self):
        if not self.redund_blocks:
            return '0x%x.%x' % (self.block, self.trunk)
        else:
            return '0x{%x,%s}.%x' % (
                self.block,
                ','.join('%x' % block for block in self.redund_blocks),
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
                        not rbyds[i]
                        or not ((rbyd.rev - rbyds[i].rev) & 0x80000000)
                        or (rbyd.rev == rbyds[i].rev
                            and rbyd.trunk > rbyds[i].trunk)):
                    i = i_
            # keep track of the other blocks
            rbyd = rbyds[i]
            rbyd.redund_blocks = [rbyds[(i+1+j) % len(rbyds)].block
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
                            cksum = cksum_
                            trunk_ = trunk__
                            weight = weight_

            if not tag & TAG_ALT:
                j_ += size

        return cls(block, data, rev, eoff, trunk_, weight)

    def lookup(self, rid, tag):
        if not self:
            return True, 0, -1, 0, 0, 0, b'', []

        lower = 0
        upper = self.weight
        path = []

        # descend down tree
        j = self.trunk
        while True:
            _, alt, weight_, jump, d = fromtag(self.data[j:])

            # found an alt?
            if alt & TAG_ALT:
                # follow?
                if ((rid, tag & 0xfff) > (upper-weight_-1, alt & 0xfff)
                        if alt & TAG_GT
                        else ((rid, tag & 0xfff)
                            <= (lower+weight_-1, alt & 0xfff))):
                    lower += upper-lower-weight_ if alt & TAG_GT else 0
                    upper -= upper-lower-weight_ if not alt & TAG_GT else 0
                    j = j - jump

                    # figure out which color
                    if alt & TAG_R:
                        _, nalt, _, _, _ = fromtag(self.data[j+jump+d:])
                        if nalt & TAG_R:
                            path.append((j+jump, j, True, 'y'))
                        else:
                            path.append((j+jump, j, True, 'r'))
                    else:
                        path.append((j+jump, j, True, 'b'))

                # stay on path
                else:
                    lower += weight_ if not alt & TAG_GT else 0
                    upper -= weight_ if alt & TAG_GT else 0
                    j = j + d

                    # figure out which color
                    if alt & TAG_R:
                        _, nalt, _, _, _ = fromtag(self.data[j:])
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

                return done, rid_, tag_, w_, j, d, self.data[j+d:j+d+jump], path

    def __bool__(self):
        return bool(self.trunk)

    def __eq__(self, other):
        return self.block == other.block and self.trunk == other.trunk

    def __ne__(self, other):
        return not self.__eq__(other)

    def __iter__(self):
        tag = 0
        rid = -1

        while True:
            done, rid, tag, w, j, d, data, _ = self.lookup(rid, tag+0x1)
            if done:
                break

            yield rid, tag, w, j, d, data

    # create tree representation for debugging
    def tree(self):
        trunks = co.defaultdict(lambda: (-1, 0))
        alts = co.defaultdict(lambda: {})

        rid, tag = -1, 0
        while True:
            done, rid, tag, w, j, d, data, path = self.lookup(rid, tag+0x1)
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

        return tree, t_depth

    # btree lookup with this rbyd as the root
    def btree_lookup(self, f, block_size, bid, *,
            depth=None):
        rbyd = self
        rid = bid
        depth_ = 1
        path = []

        # corrupted? return a corrupted block once
        if not rbyd:
            return bid > 0, bid, 0, rbyd, -1, [], path

        while True:
            # collect all tags, normally you don't need to do this
            # but we are debugging here
            name = None
            tags = []
            branch = None
            rid_ = rid
            tag = 0
            w = 0
            for i in it.count():
                done, rid__, tag, w_, j, d, data, _ = rbyd.lookup(
                    rid_, tag+0x1)
                if done or (i != 0 and rid__ != rid_):
                    break

                # first tag indicates the branch's weight
                if i == 0:
                    rid_, w = rid__, w_

                # catch any branches
                if tag == TAG_BRANCH:
                    branch = (tag, j, d, data)

                tags.append((tag, j, d, data))

            # keep track of path
            path.append((bid + (rid_-rid), w, rbyd, rid_, tags))

            # descend down branch?
            if branch is not None and (
                    not depth or depth_ < depth):
                tag, j, d, data = branch
                block, trunk, cksum = frombranch(data)
                rbyd = Rbyd.fetch(f, block_size, block, trunk)

                # corrupted? bail here so we can keep traversing the tree
                if not rbyd:
                    return False, bid + (rid_-rid), w, rbyd, -1, [], path

                rid -= (rid_-(w-1))
                depth_ += 1
            else:
                return not tags, bid + (rid_-rid), w, rbyd, rid_, tags, path

    # btree rbyd-tree generation for debugging
    def btree_tree(self, f, block_size, *,
            depth=None,
            inner=False):
        # find the max depth of each layer to nicely align trees
        bdepths = {}
        bid = -1
        while True:
            done, bid, w, rbyd, rid, tags, path = self.btree_lookup(
                f, block_size, bid+1, depth=depth)
            if done:
                break

            for d, (bid, w, rbyd, rid, tags) in enumerate(path):
                _, rdepth = rbyd.tree()
                bdepths[d] = max(bdepths.get(d, 0), rdepth)

        # find all branches
        tree = set()
        root = None
        branches = {}
        bid = -1
        while True:
            done, bid, w, rbyd, rid, tags, path = self.btree_lookup(
                f, block_size, bid+1, depth=depth)
            if done:
                break

            d_ = 0
            leaf = None
            for d, (bid, w, rbyd, rid, tags) in enumerate(path):
                if not tags:
                    continue

                # map rbyd tree into B-tree space
                rtree, rdepth = rbyd.tree()

                # note we adjust our bid/rids to be left-leaning,
                # this allows a global order and make tree rendering quite
                # a bit easier
                rtree_ = set()
                for branch in rtree:
                    a_rid, a_tag = branch.a
                    b_rid, b_tag = branch.b
                    _, _, _, a_w, _, _, _, _ = rbyd.lookup(a_rid, 0)
                    _, _, _, b_w, _, _, _, _ = rbyd.lookup(b_rid, 0)
                    rtree_.add(TBranch(
                        a=(a_rid-(a_w-1), a_tag),
                        b=(b_rid-(b_w-1), b_tag),
                        d=branch.d,
                        c=branch.c,
                    ))
                rtree = rtree_

                # connect our branch to the rbyd's root
                if leaf is not None:
                    root = min(rtree,
                        key=lambda branch: branch.d,
                        default=None)

                    if root is not None:
                        r_rid, r_tag = root.a
                    else:
                        r_rid, r_tag = rid-(w-1), tags[0][0]
                    tree.add(TBranch(
                        a=leaf,
                        b=(bid-rid+r_rid, d, r_rid, r_tag),
                        d=d_-1,
                        c='b',
                    ))

                for branch in rtree:
                    # map rbyd branches into our btree space
                    a_rid, a_tag = branch.a
                    b_rid, b_tag = branch.b
                    tree.add(TBranch(
                        a=(bid-rid+a_rid, d, a_rid, a_tag),
                        b=(bid-rid+b_rid, d, b_rid, b_tag),
                        d=branch.d + d_ + bdepths.get(d, 0)-rdepth,
                        c=branch.c,
                    ))

                d_ += max(bdepths.get(d, 0), 1)
                leaf = (bid-(w-1), d, rid-(w-1), TAG_BRANCH)

        # remap branches to leaves if we aren't showing inner branches
        if not inner:
            # step through each layer backwards
            b_depth = max((branch.a[1]+1 for branch in tree), default=0)

            # keep track of the original bids, unfortunately because we
            # store the bids in the branches we overwrite these
            tree = {(branch.b[0] - branch.b[2], branch) for branch in tree}

            for bd in reversed(range(b_depth-1)):
                # find leaf-roots at this level
                roots = {}
                for bid, branch in tree:
                    # choose the highest node as the root
                    if (branch.b[1] == b_depth-1
                            and (bid not in roots
                                or branch.d < roots[bid].d)):
                        roots[bid] = branch

                # remap branches to leaf-roots
                tree_ = set()
                for bid, branch in tree:
                    if branch.a[1] == bd and branch.a[0] in roots:
                        branch = TBranch(
                            a=roots[branch.a[0]].b,
                            b=branch.b,
                            d=branch.d,
                            c=branch.c,
                        )
                    if branch.b[1] == bd and branch.b[0] in roots:
                        branch = TBranch(
                            a=branch.a,
                            b=roots[branch.b[0]].b,
                            d=branch.d,
                            c=branch.c,
                        )
                    tree_.add((bid, branch))
                tree = tree_

            # strip out bids
            tree = {branch for _, branch in tree}

        return tree, max((branch.d+1 for branch in tree), default=0)

    # btree B-tree generation for debugging
    def btree_btree(self, f, block_size, *,
            depth=None,
            inner=False):
        # find all branches
        tree = set()
        root = None
        branches = {}
        bid = -1
        while True:
            done, bid, w, rbyd, rid, tags, path = self.btree_lookup(
                f, block_size, bid+1, depth=depth)
            if done:
                break

            # if we're not showing inner nodes, prefer names higher in
            # the tree since this avoids showing vestigial names
            name = None
            if not inner:
                name = None
                for bid_, w_, rbyd_, rid_, tags_ in reversed(path):
                    for tag_, j_, d_, data_ in tags_:
                        if tag_ & 0x7f00 == TAG_NAME:
                            name = (tag_, j_, d_, data_)

                    if rid_-(w_-1) != 0:
                        break

            a = root
            for d, (bid, w, rbyd, rid, tags) in enumerate(path):
                if not tags:
                    continue

                b = (bid-(w-1), d, rid-(w-1),
                    (name if name else tags[0])[0])

                # remap branches to leaves if we aren't showing
                # inner branches
                if not inner:
                    if b not in branches:
                        bid, w, rbyd, rid, tags = path[-1]
                        if not tags:
                            continue
                        branches[b] = (
                            bid-(w-1), len(path)-1, rid-(w-1),
                            (name if name else tags[0])[0])
                    b = branches[b]

                # found entry point?
                if root is None:
                    root = b
                    a = root

                tree.add(TBranch(
                    a=a,
                    b=b,
                    d=d,
                    c='b',
                ))
                a = b

        return tree, max((branch.d+1 for branch in tree), default=0)


def main(disk, mroots=None, *,
        block_size=None,
        block_count=None,
        mleaf_weight=None,
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

        # determine the mleaf_weight from the block_size, this is just for
        # printing purposes
        if mleaf_weight is None:
            mleaf_weight = 1 << m.ceil(m.log2(block_size // 16))

        # before we print, we need to do a pass for a few things:
        # - find the actual mroot
        # - find the total weight
        bweight = 0
        rweight = 0

        mroot = Rbyd.fetch(f, block_size, mroots)
        mdepth = 1
        while True:
            # corrupted?
            if not mroot:
                break

            rweight = max(rweight, mroot.weight)

            # stop here?
            if args.get('depth') and mdepth >= args.get('depth'):
                break

            # fetch the next mroot
            done, rid, tag, w, j, d, data, _ = mroot.lookup(-1, TAG_MROOT)
            if not (not done and rid == -1 and tag == TAG_MROOT):
                break

            blocks = frommdir(data)
            mroot = Rbyd.fetch(f, block_size, blocks)
            mdepth += 1

        # fetch the mdir, if there is one
        mdir = None
        if not args.get('depth') or mdepth < args.get('depth'):
            done, rid, tag, w, j, _, data, _ = mroot.lookup(-1, TAG_MDIR)
            if not done and rid == -1 and tag == TAG_MDIR:
                blocks = frommdir(data)
                mdir = Rbyd.fetch(f, block_size, blocks)

                # corrupted?
                if mdir:
                    rweight = max(rweight, mdir.weight)

        # fetch the actual mtree, if there is one
        mtree = None
        if not args.get('depth') or mdepth < args.get('depth'):
            done, rid, tag, w, j, d, data, _ = mroot.lookup(-1, TAG_MTREE)
            if not done and rid == -1 and tag == TAG_MTREE:
                w, block, trunk, cksum = frombtree(data)
                mtree = Rbyd.fetch(f, block_size, block, trunk)

                bweight = w

                # traverse entries
                mbid = -1
                while True:
                    done, mbid, mw, rbyd, rid, tags, path = mtree.btree_lookup(
                        f, block_size, mbid+1,
                        depth=args.get('depth', mdepth)-mdepth)
                    if done:
                        break

                    # corrupted?
                    if not rbyd:
                        continue

                    mdir__ = None
                    if (not args.get('depth')
                            or mdepth+len(path) < args.get('depth')):
                        mdir__ = next(((tag, j, d, data)
                            for tag, j, d, data in tags
                            if tag == TAG_MDIR),
                            None)

                    if mdir__:
                        # fetch the mdir
                        _, _, _, data = mdir__
                        blocks = frommdir(data)
                        mdir_ = Rbyd.fetch(f, block_size, blocks)

                        # corrupted?
                        if mdir_:
                            rweight = max(rweight, mdir_.weight)

        # precompute rbyd-tree if requested
        t_width = 0
        if args.get('tree'):
            # compute mroot chain "tree", prefix our actual mtree with this
            tree = set()
            d_ = 0
            mroot_ = Rbyd.fetch(f, block_size, mroots)
            mdepth_ = 1
            for d in it.count():
                # corrupted?
                if not mroot_:
                    break

                # compute the mroots rbyd-tree
                rtree, rdepth = mroot_.tree()

                # connect branch to our root
                if d > 0:
                    root = min(rtree,
                        key=lambda branch: branch.d,
                        default=None)

                    if root:
                        r_rid, r_tag = root.a
                    else:
                        _, r_rid, r_tag, _, _, _, _, _ = mroot_.lookup(-1, 0x1)
                    tree.add(TBranch(
                        a=(-1, d-1, 0, -1, TAG_MROOT),
                        b=(-1, d, 0, r_rid, r_tag),
                        d=d_-1,
                        c='b',
                    ))

                # map the tree into our metadata space
                for branch in rtree:
                    a_rid, a_tag = branch.a
                    b_rid, b_tag = branch.b
                    tree.add(TBranch(
                        a=(-1, d, 0, a_rid, a_tag),
                        b=(-1, d, 0, b_rid, b_tag),
                        d=d_ + branch.d,
                        c=branch.c,
                    ))
                d_ += rdepth

                # stop here?
                if args.get('depth') and mdepth_ >= args.get('depth'):
                    break

                # fetch the next mroot
                done, rid, tag, w, j, _, data, _ = mroot_.lookup(-1, TAG_MROOT)
                if not (not done and rid == -1 and tag == TAG_MROOT):
                    break

                blocks = frommdir(data)
                mroot_ = Rbyd.fetch(f, block_size, blocks)
                mdepth_ += 1

            # compute mdir's rbyd-tree if there is one
            if mdir:
                rtree, rdepth = mdir.tree()

                # connect branch to our root
                root = min(rtree,
                    key=lambda branch: branch.d,
                    default=None)

                if root:
                    r_rid, r_tag = root.a
                else:
                    _, r_rid, r_tag, _, _, _, _, _ = mdir.lookup(-1, 0x1)
                tree.add(TBranch(
                    a=(-1, d, 0, -1, TAG_MDIR),
                    b=(0, 0, 0, r_rid, r_tag),
                    d=d_-1,
                    c='b',
                ))

                # map the tree into our metadata space
                for branch in rtree:
                    a_rid, a_tag = branch.a
                    b_rid, b_tag = branch.b
                    tree.add(TBranch(
                        a=(0, 0, 0, a_rid, a_tag),
                        b=(0, 0, 0, b_rid, b_tag),
                        d=d_ + branch.d,
                        c=branch.c,
                    ))

            # compute the mtree's rbyd-tree if there is one
            if mtree:
                tree_, tdepth = mtree.btree_tree(
                    f, block_size,
                    depth=args.get('depth', mdepth)-mdepth,
                    inner=args.get('inner'))

                # connect a branch to the root of the tree
                root = min(tree_, key=lambda branch: branch.d, default=None)
                if root:
                    r_bid, r_bd, r_rid, r_tag = root.a
                    tree.add(TBranch(
                        a=(-1, d, 0, -1, TAG_MTREE),
                        b=(r_bid, r_bd, r_rid, 0, r_tag),
                        d=d_-1,
                        c='b',
                    ))

                # map the tree into our metadata space
                for branch in tree_:
                    a_bid, a_bd, a_rid, a_tag = branch.a
                    b_bid, b_bd, b_rid, b_tag = branch.b
                    tree.add(TBranch(
                        a=(a_bid, a_bd, a_rid, 0, a_tag),
                        b=(b_bid, b_bd, b_rid, 0, b_tag),
                        d=d_ + branch.d,
                        c=branch.c,
                    ))

                # find the max depth of each mdir to nicely align trees
                mdepth_ = 0
                mbid = -1
                while True:
                    done, mbid, mw, rbyd, rid, tags, path = mtree.btree_lookup(
                        f, block_size, mbid+1,
                        depth=args.get('depth', mdepth)-mdepth)
                    if done:
                        break

                    # corrupted?
                    if not rbyd:
                        continue

                    mdir__ = None
                    if (not args.get('depth')
                            or mdepth+len(path) < args.get('depth')):
                        mdir__ = next(((tag, j, d, data)
                            for tag, j, d, data in tags
                            if tag == TAG_MDIR),
                            None)

                    if mdir__:
                        # fetch the mdir
                        _, _, _, data = mdir__
                        blocks = frommdir(data)
                        mdir_ = Rbyd.fetch(f, block_size, blocks)

                        rtree, rdepth = mdir_.tree()
                        mdepth_ = max(mdepth_, rdepth)

                # compute the rbyd-tree for each mdir
                mbid = -1
                while True:
                    done, mbid, mw, rbyd, rid, tags, path = mtree.btree_lookup(
                        f, block_size, mbid+1,
                        depth=args.get('depth', mdepth)-mdepth)
                    if done:
                        break

                    # corrupted?
                    if not rbyd:
                        continue

                    mdir__ = None
                    if (not args.get('depth')
                            or mdepth+len(path) < args.get('depth')):
                        mdir__ = next(((tag, j, d, data)
                            for tag, j, d, data in tags
                            if tag == TAG_MDIR),
                            None)

                    if mdir__:
                        # fetch the mdir
                        _, _, _, data = mdir__
                        blocks = frommdir(data)
                        mdir_ = Rbyd.fetch(f, block_size, blocks)

                        rtree, rdepth = mdir_.tree()

                        # connect the root to the mtree
                        branch = max(
                            (branch for branch in tree
                                if branch.b[0] == mbid-(mw-1)),
                            key=lambda branch: branch.d,
                            default=None)
                        if branch:
                            root = min(rtree,
                                key=lambda branch: branch.d,
                                default=None)
                            if root:
                                r_rid, r_tag = root.a
                            else:
                                _, r_rid, r_tag, _, _, _, _, _ = (
                                    mdir_.lookup(-1, 0x1))
                            tree.add(TBranch(
                                a=branch.b,
                                b=(mbid-(mw-1), len(path), 0, r_rid, r_tag),
                                d=d_ + tdepth,
                                c='b',
                            ))

                        # map the tree into our metadata space
                        for branch in rtree:
                            a_rid, a_tag = branch.a
                            b_rid, b_tag = branch.b
                            tree.add(TBranch(
                                a=(mbid-(mw-1), len(path), 0, a_rid, a_tag),
                                b=(mbid-(mw-1), len(path), 0, b_rid, b_tag),
                                d=(d_ + tdepth + 1
                                    + branch.d + mdepth_-rdepth),
                                c=branch.c,
                            ))

                # remap branches to leaves if we aren't showing inner branches
                if not args.get('inner'):
                    # step through each layer backwards
                    b_depth = max((branch.b[1]+1 for branch in tree), default=0)

                    # keep track of the original bids, unfortunately because we
                    # store the bids in the branches we overwrite these
                    tree = {(branch.b[0] - branch.b[2], branch)
                        for branch in tree}

                    for bd in reversed(range(b_depth-1)):
                        # find leaf-roots at this level
                        roots = {}
                        for bid, branch in tree:
                            # choose the highest node as the root
                            if (branch.b[1] == b_depth-1
                                    and (bid not in roots
                                        or branch.d < roots[bid].d)):
                                roots[bid] = branch

                        # remap branches to leaf-roots
                        tree_ = set()
                        for bid, branch in tree:
                            # note we ignore mroot branches, we don't collapse
                            # normally these
                            if (branch.a[0] != -1
                                    and branch.a[1] == bd
                                    and branch.a[0] in roots):
                                branch = TBranch(
                                    a=roots[branch.a[0]].b,
                                    b=branch.b,
                                    d=branch.d,
                                    c=branch.c,
                                )
                            if (branch.b[0] != -1
                                    and branch.b[1] == bd
                                    and branch.b[0] in roots):
                                branch = TBranch(
                                    a=branch.a,
                                    b=roots[branch.b[0]].b,
                                    d=branch.d,
                                    c=branch.c,
                                )
                            tree_.add((bid, branch))
                        tree = tree_

                    # strip out bids
                    tree = {branch for _, branch in tree}

        # precompute B-tree if requested
        elif args.get('btree'):
            # compute mroot chain "tree", prefix our actual mtree with this
            tree = set()
            mroot_ = Rbyd.fetch(f, block_size, mroots)
            mdepth_ = 1
            for d in it.count():
                # corrupted?
                if not mroot_:
                    break

                # connect branch to our first tag
                if d > 0:
                    done, rid, tag, w, j, _, data, _ = mroot_.lookup(-1, 0x1)
                    if not done:
                        tree.add(TBranch(
                            a=(-1, d-1, 0, -1, TAG_MROOT),
                            b=(-1, d, 0, rid, tag),
                            d=0,
                            c='b',
                        ))

                # stop here?
                if args.get('depth') and mdepth_ >= args.get('depth'):
                    break

                # fetch the next mroot
                done, rid, tag, w, j, _, data, _ = mroot_.lookup(-1, TAG_MROOT)
                if not (not done and rid == -1 and tag == TAG_MROOT):
                    break

                blocks = frommdir(data)
                mroot_ = Rbyd.fetch(f, block_size, blocks)
                mdepth_ += 1

            # create a branch to our mdir if there is one
            if mdir:
                # connect branch to our first tag
                done, rid, tag, w, j, _, data, _ = mdir.lookup(-1, 0x1)
                if not done:
                    tree.add(TBranch(
                        a=(-1, d, 0, -1, TAG_MDIR),
                        b=(0, 0, 0, rid, tag),
                        d=0,
                        c='b',
                    ))

            # compute the mtree's B-tree if there is one
            if mtree:
                tree_, tdepth = mtree.btree_btree(
                    f, block_size,
                    depth=args.get('depth', mdepth)-mdepth,
                    inner=args.get('inner'))

                # connect a branch to the root of the tree
                root = min(tree_, key=lambda branch: branch.d, default=None)
                if root:
                    r_bid, r_bd, r_rid, r_tag = root.a
                    tree.add(TBranch(
                        a=(-1, d, 0, -1, TAG_MTREE),
                        b=(r_bid, r_bd, r_rid, 0, r_tag),
                        d=0,
                        c='b',
                    ))

                # map the tree into our metadata space
                for branch in tree_:
                    a_bid, a_bd, a_rid, a_tag = branch.a
                    b_bid, b_bd, b_rid, b_tag = branch.b
                    tree.add(TBranch(
                        a=(a_bid, a_bd, a_rid, 0, a_tag),
                        b=(b_bid, b_bd, b_rid, 0, b_tag),
                        d=1 + branch.d,
                        c=branch.c,
                    ))

                # remap branches to leaves if we aren't showing inner branches
                if not args.get('inner'):
                    mbid = -1
                    while True:
                        done, mbid, mw, rbyd, rid, tags, path = (
                            mtree.btree_lookup(
                                f, block_size, mbid+1,
                                depth=args.get('depth', mdepth)-mdepth))
                        if done:
                            break

                        # corrupted?
                        if not rbyd:
                            continue

                        mdir__ = None
                        if (not args.get('depth')
                                or mdepth+len(path) < args.get('depth')):
                            mdir__ = next(((tag, j, d, data)
                                for tag, j, d, data in tags
                                if tag == TAG_MDIR),
                                None)

                        if mdir__:
                            # fetch the mdir
                            _, _, _, data = mdir__
                            blocks = frommdir(data)
                            mdir_ = Rbyd.fetch(f, block_size, blocks)

                            # find the first entry in the mdir, map branches
                            # to this entry
                            done, rid, tag, _, j, d, data, _ = (
                                mdir_.lookup(-1, 0x1))

                            tree_ = set()
                            for branch in tree:
                                if branch.a[0] == mbid-(mw-1):
                                    a_bid, a_bd, _, _, _ = branch.a
                                    branch = TBranch(
                                        a=(a_bid, a_bd+1, 0, rid, tag),
                                        b=branch.b,
                                        d=branch.d,
                                        c=branch.c,
                                    )
                                if branch.b[0] == mbid-(mw-1):
                                    b_bid, b_bd, _, _, _ = branch.b
                                    branch = TBranch(
                                        a=branch.a,
                                        b=(b_bid, b_bd+1, 0, rid, tag),
                                        d=branch.d,
                                        c=branch.c,
                                    )
                                tree_.add(branch)
                            tree = tree_

        # common tree renderer
        if args.get('tree') or args.get('btree'):
            # find the max depth from the tree
            t_depth = max((branch.d+1 for branch in tree), default=0)
            if t_depth > 0:
                t_width = 2*t_depth + 2

            def treerepr(mbid, mw, md, mrid, rid, tag):
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
                    t, c, was = branchrepr(
                        (mbid-max(mw-1, 0), md, mrid-max(mw-1, 0), rid, tag),
                        d, was)

                    trunk.append('%s%s%s%s' % (
                        '\x1b[33m' if color and c == 'y'
                            else '\x1b[31m' if color and c == 'r'
                            else '\x1b[90m' if color and c == 'b'
                            else '',
                        t,
                        ('>' if was else ' ') if d == t_depth-1 else '',
                        '\x1b[m' if color and c else ''))

                return '%s ' % ''.join(trunk)


        def dbg_mdir(mdir, mbid, mw, md):
            for i, (rid, tag, w, j, d, data) in enumerate(mdir):
                # show human-readable tag representation
                print('%12s %s%s' % (
                    '{%s}:' % ','.join('%04x' % block
                        for block in it.chain([mdir.block],
                            mdir.redund_blocks))
                        if i == 0 else '',
                    treerepr(mbid-max(mw-1, 0), 0, md, 0, rid, tag)
                        if args.get('tree') or args.get('btree') else '',
                    '%*s %-*s%s' % (
                        2*w_width+1, '%d.%d-%d' % (
                                mbid//mleaf_weight, rid-(w-1), rid)
                            if w > 1 else '%d.%d' % (mbid//mleaf_weight, rid)
                            if w > 0 or i == 0 else '',
                        21+w_width, tagrepr(tag, w, len(data), j),
                        '  %s' % next(xxd(data, 8), '')
                            if not args.get('raw')
                                and not args.get('no_truncate')
                            else '')))

                # show in-device representation
                if args.get('device'):
                    print('%11s  %*s%*s %04x %08x %07x' % (
                        '',
                        t_width, '',
                        2*w_width+1, '',
                        tag, w, len(data)));

                # show on-disk encoding of tags
                if args.get('raw'):
                    for o, line in enumerate(xxd(mdir.data[j:j+d])):
                        print('%11s: %*s%*s %s' % (
                            '%04x' % (j + o*16),
                            t_width, '',
                            2*w_width+1, '',
                            line))
                if args.get('raw') or args.get('no_truncate'):
                    if not tag & TAG_ALT:
                        for o, line in enumerate(xxd(data)):
                            print('%11s: %*s%*s %s' % (
                                '%04x' % (j+d + o*16),
                                t_width, '',
                                2*w_width+1, '',
                                line))

        # prbyd here means the last rendered rbyd, we update
        # in dbg_branch to always print interleaved addresses
        prbyd = None
        def dbg_branch(bid, w, rbyd, rid, tags, bd):
            nonlocal prbyd

            # show human-readable representation
            for i, (tag, j, d, data) in enumerate(tags):
                print('%12s %s%*s %-*s  %s' % (
                    '%04x.%04x:' % (rbyd.block, rbyd.trunk)
                        if prbyd is None or rbyd != prbyd
                        else '',
                    treerepr(bid, w, bd, rid, 0, tag)
                        if args.get('tree') or args.get('btree') else '',
                    2*w_width+1, '' if i != 0
                        else '%d-%d' % (
                                (bid-(w-1))//mleaf_weight,
                                bid//mleaf_weight)
                            if (w//mleaf_weight) > 1
                        else bid//mleaf_weight if w > 0
                        else '',
                    21+w_width, tagrepr(
                        tag, w if i == 0 else 0, len(data), None),
                    next(xxd(data, 8), '')
                        if not args.get('raw') and not args.get('no_truncate')
                        else ''))
                prbyd = rbyd

                # show in-device representation
                if args.get('device'):
                    print('%11s  %*s%*s %04x %08x %07x' % (
                        '',
                        t_width, '',
                        2*w_width+1, '',
                        tag, w if i == 0 else 0, len(data)));

                # show on-disk encoding of tags/data
                if args.get('raw'):
                    for o, line in enumerate(xxd(rbyd.data[j:j+d])):
                        print('%11s: %*s%*s %s' % (
                            '%04x' % (j + o*16),
                            t_width, '',
                            2*w_width+1, '',
                            line))
                if args.get('raw') or args.get('no_truncate'):
                    for o, line in enumerate(xxd(data)):
                        print('%11s: %*s%*s %s' % (
                            '%04x' % (j+d + o*16),
                            t_width, '',
                            2*w_width+1, '',
                            line))


        #### actual debugging begins here

        # print some information about the mtree
        print('mtree %s, rev %d, weight %d.%d' % (
            mroot.addr(), mroot.rev, bweight//mleaf_weight, 1*mleaf_weight))

        # dynamically size the id field
        w_width = max(
            m.ceil(m.log10(max(1, bweight//mleaf_weight)+1)),
            m.ceil(m.log10(max(1, rweight)+1)),
            # in case of -1.-1
            2)

        # show each mroot
        prbyd = None
        ppath = []
        corrupted = False
        mroot = Rbyd.fetch(f, block_size, mroots)
        mdepth = 1
        for d in it.count():
            # corrupted?
            if not mroot:
                print('{%s}: %s%s%s' % (
                    ','.join('%04x' % block
                        for block in it.chain([mroot.block],
                            mroot.redund_blocks)),
                    '\x1b[31m' if color else '',
                    '(corrupted mroot %s)' % mroot.addr(),
                    '\x1b[m' if color else ''))
                corrupted = True
                break
            else:
                # show the mdir
                dbg_mdir(mroot, -1, 0, d)

            # stop here?
            if args.get('depth') and mdepth >= args.get('depth'):
                break

            # fetch the next mroot
            done, rid, tag, w, j, _, data, _ = mroot.lookup(-1, TAG_MROOT)
            if not (not done and rid == -1 and tag == TAG_MROOT):
                break

            blocks = frommdir(data)
            mroot = Rbyd.fetch(f, block_size, blocks)
            mdepth += 1

        # show the mdir, if there is one
        if not args.get('depth') or mdepth < args.get('depth'):
            done, rid, tag, w, j, _, data, _ = mroot.lookup(-1, TAG_MDIR)
            if not done and rid == -1 and tag == TAG_MDIR:
                blocks = frommdir(data)
                mdir = Rbyd.fetch(f, block_size, blocks)

                # corrupted?
                if not mdir:
                    print('{%s}: %s%s%s' % (
                        ','.join('%04x' % block
                            for block in it.chain([mdir.block],
                                mdir.redund_blocks)),
                        '\x1b[31m' if color else '',
                        '(corrupted mdir %s)' % mdir.addr(),
                        '\x1b[m' if color else ''))
                    corrupted = True
                else:
                    # show the mdir
                    dbg_mdir(mdir, 0, 0, 0)

        # fetch the actual mtree, if there is one
        if not args.get('depth') or mdepth < args.get('depth'):
            done, rid, tag, w, j, d, data, _ = mroot.lookup(-1, TAG_MTREE)
            if not done and rid == -1 and tag == TAG_MTREE:
                w, block, trunk, cksum = frombtree(data)
                mtree = Rbyd.fetch(f, block_size, block, trunk)

                # traverse entries
                mbid = -1
                while True:
                    done, mbid, mw, rbyd, rid, tags, path = mtree.btree_lookup(
                        f, block_size, mbid+1,
                        depth=args.get('depth', mdepth)-mdepth)
                    if done:
                        break

                    # print inner btree entries if requested
                    if args.get('inner'):
                        changed = False
                        for (x, px) in it.zip_longest(
                                enumerate(path[:-1]),
                                enumerate(ppath[:-1])):
                            if x is None:
                                break
                            if not (changed or px is None or x != px):
                                continue
                            changed = True

                            # show the inner entry
                            d, (mid_, w_, rbyd_, rid_, tags_) = x
                            dbg_branch(mid_, w_, rbyd_, rid_, tags_, d)
                    ppath = path

                    # corrupted? try to keep printing the tree
                    if not rbyd:
                        print('%11s: %*s%s%s%s' % (
                            '%04x.%04x' % (rbyd.block, rbyd.trunk),
                            t_width, '',
                            '\x1b[31m' if color else '',
                            '(corrupted rbyd %s)' % rbyd.addr(),
                            '\x1b[m' if color else ''))
                        prbyd = rbyd
                        corrupted = True
                        continue

                    # if we're not showing inner nodes, prefer names higher in
                    # the tree since this avoids showing vestigial names
                    if not args.get('inner'):
                        name = None
                        for mid_, w_, rbyd_, rid_, tags_ in reversed(path):
                            for tag_, j_, d_, data_ in tags_:
                                if tag_ & 0x7f00 == TAG_NAME:
                                    name = (tag_, j_, d_, data_)

                            if rid_-(w_-1) != 0:
                                break

                        if name is not None:
                            tags = [name] + [(tag, j, d, data)
                                for tag, j, d, data in tags
                                if tag & 0x7f00 != TAG_NAME]

                    # found an mdir in the tags?
                    mdir__ = None
                    if (not args.get('depth')
                            or mdepth+len(path) < args.get('depth')):
                        mdir__ = next(((tag, j, d, data)
                            for tag, j, d, data in tags
                            if tag == TAG_MDIR),
                            None)

                    # show other btree entries in certain cases
                    if args.get('inner') or not mdir__:
                        dbg_branch(mbid, mw, rbyd, rid, tags, len(path)-1)

                    if not mdir__:
                        continue

                    # fetch the mdir
                    _, _, _, data = mdir__
                    blocks = frommdir(data)
                    mdir_ = Rbyd.fetch(f, block_size, blocks)

                    # corrupted?
                    if not mdir_:
                        print('{%s}: %*s%s%s%s' % (
                            ','.join('%04x' % block
                                for block in it.chain([mdir_.block],
                                    mdir_.redund_blocks)),
                            t_width, '',
                            '\x1b[31m' if color else '',
                            '(corrupted mdir %s)' % mdir_.addr(),
                            '\x1b[m' if color else ''))
                        corrupted = True
                    else:
                        # show the mdir
                        dbg_mdir(mdir_, mbid, mw, len(path))

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
        type=bdgeom,
        help="Block size/geometry in bytes.")
    parser.add_argument(
        '--block-count',
        type=lambda x: int(x, 0),
        help="Block count in blocks.")
    parser.add_argument(
        '-M', '--mleaf-weight',
        type=lambda x: int(x, 0),
        help="Maximum weight of mdirs for mid decoding. Defaults to a "
            "block_size derived value.")
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
        '-t', '--tree',
        action='store_true',
        help="Show the underlying rbyd trees.")
    parser.add_argument(
        '-b', '--btree',
        action='store_true',
        help="Show the underlying B-tree.")
    parser.add_argument(
        '-i', '--inner',
        action='store_true',
        help="Show inner branches.")
    parser.add_argument(
        '-Z', '--depth',
        nargs='?',
        type=lambda x: int(x, 0),
        const=0,
        help="Depth of tree to show.")
    parser.add_argument(
        '-e', '--error-on-corrupt',
        action='store_true',
        help="Error if the filesystem is corrupt.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
