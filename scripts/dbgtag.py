#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import functools as ft
import io
import math as mt
import os
import struct
import sys


TAG_NULL        = 0x0000    ##  v--- ---- +--- ----
TAG_INTERNAL    = 0x0000    ##  v--- ---- +ttt tttt
TAG_CONFIG      = 0x0100    ##  v--- ---1 +ttt tttt
TAG_MAGIC       = 0x0131    #   v--- ---1 +-11 --rr
TAG_VERSION     = 0x0134    #   v--- ---1 +-11 -1--
TAG_RCOMPAT     = 0x0135    #   v--- ---1 +-11 -1-1
TAG_WCOMPAT     = 0x0136    #   v--- ---1 +-11 -11-
TAG_OCOMPAT     = 0x0137    #   v--- ---1 +-11 -111
TAG_GEOMETRY    = 0x0138    #   v--- ---1 +-11 1---
TAG_NAMELIMIT   = 0x0139    #   v--- ---1 +-11 1--1
TAG_FILELIMIT   = 0x013a    #   v--- ---1 +-11 1-1-
TAG_GDELTA      = 0x0200    ##  v--- --1- +ttt tttt
TAG_GRMDELTA    = 0x0230    #   v--- --1- +-11 --++
TAG_GBMAPDELTA  = 0x0234    #   v--- --1- +-11 -1rr
TAG_NAME        = 0x0300    ##  v--- --11 +ttt tttt
TAG_BNAME       = 0x0300    #   v--- --11 +--- ----
TAG_REG         = 0x0301    #   v--- --11 +--- ---1
TAG_DIR         = 0x0302    #   v--- --11 +--- --1-
TAG_STICKYNOTE  = 0x0303    #   v--- --11 +--- --11
TAG_BOOKMARK    = 0x0304    #   v--- --11 +--- -1--
TAG_MNAME       = 0x0330    #   v--- --11 +-11 ----
TAG_STRUCT      = 0x0400    ##  v--- -1-- +ttt tttt
TAG_BRANCH      = 0x0400    #   v--- -1-- +--- --rr
TAG_DATA        = 0x0404    #   v--- -1-- +--- -1rr
TAG_BLOCK       = 0x0408    #   v--- -1-- +--- 1err
TAG_DID         = 0x0420    #   v--- -1-- +-1- ----
TAG_BSHRUB      = 0x0428    #   v--- -1-- +-1- 1-rr
TAG_BTREE       = 0x042c    #   v--- -1-- +-1- 11rr
TAG_MROOT       = 0x0431    #   v--- -1-- +-11 --rr
TAG_MDIR        = 0x0435    #   v--- -1-- +-11 -1rr
TAG_MTREE       = 0x043c    #   v--- -1-- +-11 11rr
TAG_BMRANGE     = 0x0440    #   v--- -1-- +1-- ++uu
TAG_BMFREE      = 0x0440    #   v--- -1-- +1-- ----
TAG_BMINUSE     = 0x0441    #   v--- -1-- +1-- ---1
TAG_BMERASED    = 0x0442    #   v--- -1-- +1-- --1-
TAG_BMBAD       = 0x0443    #   v--- -1-- +1-- --11
TAG_ATTR        = 0x0600    ##  v--- -11a +aaa aaaa
TAG_UATTR       = 0x0600    #   v--- -11- +aaa aaaa
TAG_SATTR       = 0x0700    #   v--- -111 +aaa aaaa
TAG_SHRUB       = 0x1000    ##  v--1 kkkk +kkk kkkk
TAG_ALT         = 0x4000    ##  v1cd kkkk +kkk kkkk
TAG_B           = 0x0000
TAG_R           = 0x2000
TAG_LE          = 0x0000
TAG_GT          = 0x1000
TAG_CKSUM       = 0x3000    ##  v-11 ---- ++++ +pqq
TAG_PHASE       = 0x0003
TAG_PERTURB     = 0x0004
TAG_NOTE        = 0x3100    ##  v-11 ---1 ++++ ++++
TAG_ECKSUM      = 0x3200    ##  v-11 --1- ++++ ++++
TAG_GCKSUMDELTA = 0x3300    ##  v-11 --11 ++++ ++++


# self-parsing tag repr
class Tag:
    def __init__(self, name, tag, encoding, help, *,
            lineno=0):
        self.name = name
        self.tag = tag
        self.encoding = encoding
        self.help = help
        self.lineno = lineno
        # derive mask from encoding
        self.mask = sum(
                (1 if x in 'v-01' else 0) << len(self.encoding)-1-i
                    for i, x in enumerate(self.encoding))

    def __repr__(self):
        return 'Tag(%r, %r, %r)' % (
                self.name,
                self.tag,
                self.encoding)

    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __hash__(self):
        return hash(self.name)

    def line(self):
        # substitute mask chars when zero
        tag = '0x%s' % ''.join(
                n if n != '0' else next(
                    (x for x in self.encoding[i*4:i*4+4]
                        if x not in 'v-01+'),
                    '0')
                for i, n in enumerate('%04x' % self.tag))
        # group into nibbles
        encoding = ' '.join(self.encoding[i*4:i*4+4]
                for i in range(len(self.encoding)//4))
        return ('LFS3_%s' % self.name, tag, encoding)

    def specificity(self):
        return sum(1 for x in self.encoding if x in 'v-01')

    def matches(self, tag):
        return (tag & self.mask) == (self.tag & self.mask)

    def get(self, chars, tag):
        return sum(
                tag & ((1 if x in chars else 0) << len(self.encoding)-1-i)
                    for i, x in enumerate(self.encoding))

    def max(self, chars):
        return max(len(self.encoding)-1-i
                for i, x in enumerate(self.encoding) if x in chars)

    def min(self, chars):
        return min(len(self.encoding)-1-i
                for i, x in enumerate(self.encoding) if x in chars)

    def width(self, chars):
        return self.max(chars) - self.min(chars)

    def __contains__(self, chars):
        return any(x in self.encoding for x in chars)

    @staticmethod
    @ft.cache
    def tags():
        # parse our script's source to figure out tags
        import inspect
        import re
        tags = []
        tag_pattern = re.compile(
            '^(?P<name>TAG_[^ ]*) *= *(?P<tag>[^#]*?) *'
                '#+ *(?P<encoding>(?:[^ ] *?){16}) *(?P<help>.*)$')
        for i, line in enumerate(
                inspect.getsource(inspect.getmodule(inspect.currentframe()))
                    .replace('\\\n', '')
                    .splitlines()):
            m = tag_pattern.match(line)
            if m:
                tags.append(Tag(
                        m.group('name'),
                        globals()[m.group('name')],
                        m.group('encoding').replace(' ', ''),
                        m.group('help'),
                        lineno=1+i))
        return tags

    # find best matching tag
    _sentinel = object()
    @staticmethod
    def find(tag, *, default=_sentinel):
        # find tags, note this is cached
        tags__ = Tag.tags()

        # find the most specific matching tag, ignoring valid bits
        t = max((t for t in tags__ if t.matches(tag & 0x7fff)),
                key=lambda t: t.specificity(),
                default=None)
        if t is not None:
            return t
        elif default is Tag._sentinel:
            raise KeyError(tag)
        else:
            return default

    # human readable tag repr
    @staticmethod
    def repr(tag, weight=None, size=None, *,
            global_=False,
            toff=None):
        # find the most specific matching tag, ignoring the shrub bit
        t = Tag.find(
                tag & ~(TAG_SHRUB if tag & 0x7000 == TAG_SHRUB else 0),
                default=None)

        # build repr
        r = []
        # normal tag?
        if not tag & TAG_ALT:
            if t is not None:
                # prefix shrub tags with shrub
                if tag & 0x7000 == TAG_SHRUB:
                    r.append('shrub')
                # lowercase name
                r.append(t.name.split('_', 1)[1].lower())
                # gstate tag?
                if global_:
                    if r[-1] == 'gdelta':
                        r[-1] = 'gstate'
                    elif r[-1].endswith('delta'):
                        r[-1] = r[-1][:-len('delta')]
                # include perturb/phase bits
                if 'q' in t:
                    r.append('q%d' % t.get('q', tag))
                if 'p' in t and tag & TAG_PERTURB:
                    r.append('p')

                # include unmatched fields, but not just redund, and
                # only reserved bits if non-zero
                if 'tua' in t or ('+' in t and t.get('+', tag) != 0):
                    r.append(' 0x%0*x' % (
                            (t.width('tuar+')+4-1)//4,
                            t.get('tuar+', tag)))
            # unknown tag?
            else:
                r.append('0x%04x' % tag)

            # weight?
            if weight:
                r.append(' w%d' % weight)
            # size? don't include if null
            if size is not None and (size or tag & 0x7fff):
                r.append(' %d' % size)

        # alt pointer?
        else:
            r.append('alt')
            r.append('r' if tag & TAG_R else 'b')
            r.append('gt' if tag & TAG_GT else 'le')
            r.append(' 0x%0*x' % (
                    (t.width('k')+4-1)//4,
                    t.get('k', tag)))

            # weight?
            if weight is not None:
                r.append(' w%d' % weight)
            # jump?
            if size and toff is not None:
                r.append(' 0x%x' % (0xffffffff & (toff-size)))
            elif size:
                r.append(' -%d' % size)

        return ''.join(r)


# open with '-' for stdin/stdout
def openio(path, mode='r', buffering=-1):
    import os
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

def fromleb128(data, j=0):
    word = 0
    d = 0
    while j+d < len(data):
        b = data[j+d]
        word |= (b & 0x7f) << 7*d
        word &= 0xffffffff
        if not b & 0x80:
            return word, d+1
        d += 1
    return word, d

def fromtag(data, j=0):
    d = 0
    tag = struct.unpack('>H', data[j:j+2].ljust(2, b'\0'))[0]; d += 2
    weight, d_ = fromleb128(data, j+d); d += d_
    size, d_ = fromleb128(data, j+d); d += d_
    return tag>>15, tag&0x7fff, weight, size, d


def list_tags():
    # find tags
    tags__ = Tag.tags()

    # list
    lines = []
    for t in tags__:
        lines.append(t.line())

    # figure out widths
    w = [0, 0]
    for l in lines:
        w[0] = max(w[0], len(l[0]))
        w[1] = max(w[1], len(l[1]))

    # then print results
    for l in lines:
        print('%-*s  %-*s  %s' % (
                w[0], l[0],
                w[1], l[1],
                l[2]))

def dbg_tags(data, *,
        word_bits=32):
    # figure out tag size in bytes
    if word_bits != 0:
        n = 2 + 2*mt.ceil(word_bits / 7)

    lines = []
    # interpret as ints?
    if not isinstance(data, bytes):
        for tag in data:
            lines.append((
                    ' '.join('%02x' % b for b in struct.pack('>H', tag)),
                    Tag.repr(tag)))

    # interpret as bytes?
    else:
        j = 0
        while j < len(data):
            # bounded tags?
            if word_bits != 0:
                v, tag, w, size, d = fromtag(data[j:j+n])
            # unbounded?
            else:
                v, tag, w, size, d = fromtag(data, j)

            lines.append((
                    ' '.join('%02x' % b for b in data[j:j+d]),
                    Tag.repr(tag, w, size)))
            j += d

            # skip attached data if there is any
            if not tag & TAG_ALT:
                j += size

    # figure out widths
    w = [0]
    for l in lines:
        w[0] = max(w[0], len(l[0]))

    # then print results
    for l in lines:
        print('%-*s    %s' % (
                w[0], l[0],
                l[1]))

def main(tags, *,
        list=False,
        hex=False,
        input=None,
        word_bits=32):
    import builtins
    list_, list = list, builtins.list
    hex_, hex = hex, builtins.hex

    # list all known tags
    if list_:
        list_tags()

    # interpret as a sequence of hex bytes
    elif hex_:
        bytes_ = [b for tag in tags for b in tag.split()]
        dbg_tags(bytes(int(b, 16) for b in bytes_),
                word_bits=word_bits)

    # parse tags in a file
    elif input:
        with openio(input, 'rb') as f:
            dbg_tags(f.read(),
                    word_bits=word_bits)

    # default to interpreting as ints
    else:
        dbg_tags((int(tag, 0) for tag in tags),
                word_bits=word_bits)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Decode littlefs tags.",
            allow_abbrev=False)
    parser.add_argument(
            'tags',
            nargs='*',
            help="Tags to decode.")
    parser.add_argument(
            '-l', '--list',
            action='store_true',
            help="List all known tags.")
    parser.add_argument(
            '-x', '--hex',
            action='store_true',
            help="Interpret as a sequence of hex bytes.")
    parser.add_argument(
            '-i', '--input',
            help="Read tags from this file. Can use - for stdin.")
    parser.add_argument(
            '-w', '--word', '--word-bits',
            dest='word_bits',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Word size in bits. 0 is unbounded. Defaults to 32.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
