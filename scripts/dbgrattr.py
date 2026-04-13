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


TAG_NULL        = 0x0000    ##  v--- ---- ++++ ++++
TAG_INTERNAL    = 0x0100    ##  v--- ---1 +ttt tttt
TAG_CONFIG      = 0x0200    ##  v--- --1- +ttt tttt
TAG_MAGIC       = 0x0201    #   v--- --1- +--- --rr
TAG_VERSION     = 0x0204    #   v--- --1- +--- -1--
TAG_RCOMPAT     = 0x0205    #   v--- --1- +--- -1-1
TAG_WCOMPAT     = 0x0206    #   v--- --1- +--- -11-
TAG_OCOMPAT     = 0x0207    #   v--- --1- +--- -111
TAG_GEOMETRY    = 0x0208    #   v--- --1- +--- 1---
TAG_NAMELIMIT   = 0x0209    #   v--- --1- +--- 1--1
TAG_FILELIMIT   = 0x020a    #   v--- --1- +--- 1-1-
TAG_GDELTA      = 0x0300    ##  v--- --11 +ttt tttt
TAG_GRMDELTA    = 0x0300    #   v--- --11 +--- --++
TAG_GBMAPDELTA  = 0x0304    #   v--- --11 +--- -1rr
TAG_NAME        = 0x0400    ##  v--- -1-- +ttt tttt
TAG_BNAME       = 0x0400    #   v--- -1-- +--- ----
TAG_REG         = 0x0401    #   v--- -1-- +--- ---1
TAG_DIR         = 0x0402    #   v--- -1-- +--- --1-
TAG_STICKYNOTE  = 0x0403    #   v--- -1-- +--- --11
TAG_BOOKMARK    = 0x0404    #   v--- -1-- +--- -1--
TAG_MNAME       = 0x0430    #   v--- -1-- +-11 ----
TAG_STRUCT      = 0x0500    ##  v--- -1-1 +ttt tttt
TAG_BRANCH      = 0x0500    #   v--- -1-1 +--- --rr
TAG_DATA        = 0x0504    #   v--- -1-1 +--- -1rr
TAG_BLOCK       = 0x0508    #   v--- -1-1 +--- 1err
TAG_DID         = 0x0520    #   v--- -1-1 +-1- ----
TAG_BSHRUB      = 0x0528    #   v--- -1-1 +-1- 1-rr
TAG_BTREE       = 0x052c    #   v--- -1-1 +-1- 11rr
TAG_MROOT       = 0x0531    #   v--- -1-1 +-11 --rr
TAG_MDIR        = 0x0535    #   v--- -1-1 +-11 -1rr
TAG_MTREE       = 0x053c    #   v--- -1-1 +-11 11rr
TAG_BMRANGE     = 0x0540    #   v--- -1-1 +1-- ++uu
TAG_BMFREE      = 0x0540    #   v--- -1-1 +1-- ----
TAG_BMINUSE     = 0x0541    #   v--- -1-1 +1-- ---1
TAG_BMERASED    = 0x0542    #   v--- -1-1 +1-- --1-
TAG_BMBAD       = 0x0543    #   v--- -1-1 +1-- --11
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

# our core rbyd attribute type
#
#   wwll lfff ffcc cccc tttt tttt tttt tttt
#    ^'-.''--.-''--.--' :                 :
#    '--|----|-----|----:-----------------:-- compressed weight
#   ::  '----|-----|----:-----------------:-- total len
#   ::       '-----|----:-----------------:-- from encoder
#   ::             '----:-----------------:-- optional from count
#   ::                  rgmm kkkk +kkk kkkk
#   11 => w=-1          ^^ ^ '-.' '---.---'
#   00 => w=0           '|-|---|------|------ rm bit
#   01 => w=+1           '-|---|------|------ grow bit
#   10 => w=arg            '---|------|------ mask bits
#                              '------|------ tag suptype
#                                     '------ tag subtype
#
RATTR_WEIGHT    = 0xc0000000    # 11-- ---- ---- ---- ---- ---- ---- ----
RATTR_LEN       = 0x38000000    # --11 1--- ---- ---- ---- ---- ---- ----
RATTR_FROM      = 0x07c00000    # ---- -111 11-- ---- ---- ---- ---- ----
RATTR_FROMCOUNT = 0x003f0000    # ---- ---- --11 1111 ---- ---- ---- ----
RATTR_RM        = 0x00008000    # ---- ---- ---- ---- 1--- ---- ---- ----
RATTR_GROW      = 0x00004000    # ---- ---- ---- ---- -1-- ---- ---- ----
RATTR_MASK      = 0x00003000    # ---- ---- ---- ---- --11 ---- ---- ----
RATTR_TAG       = 0x00000fff    # ---- ---- ---- ---- ---- 1111 +111 1111

# internal tags
tag_RATTRS      = 0x0100    #i  ---- ---1 ---- ----
tag_SHRUBCOMMIT = 0x0101    #i  ---- ---1 ---- ---1
tag_GRMPUSH     = 0x0102    #i  ---- ---1 ---- --1-
tag_MOVE        = 0x0103    #i  ---- ---1 ---- --11
tag_ATTRS       = 0x0104    #i  ---- ---1 ---- -1--

tag_RM          = 0x8000    #i  1--- ---- ---- ----
tag_GROW        = 0x4000    #i  -1-- ---- ---- ----
tag_MASK        = 0x3000    #i  --11 ---- ---- ----
tag_MASK0       = 0x0000    #i  ---- ---- ---- ----  (---- ---- ----)
tag_MASK2       = 0x1000    #i  ---1 ---- ---- ----  (---- ---- --11)
tag_MASK8       = 0x2000    #i  --1- ---- ---- ----  (---- 1111 1111)
tag_MASK12      = 0x3000    #i  --11 ---- ---- ----  (1111 1111 1111)

# from encoders
FROM_NIL      = 0   #
FROM_LBUF     = 1   #
FROM_BUF      = 2   #
FROM_DISK     = 3   #
FROM_CAT      = 4   #
FROM_LE32     = 5   #
FROM_LEB128   = 6   #
FROM_LLEB128  = 7   #
FROM_NAME     = 8   #
FROM_BRANCH   = 9   #
FROM_ECKSUM   = 10  #
FROM_BPTR     = 11  #
FROM_BTREE    = 12  #
FROM_SHRUB    = 13  #
FROM_MPTR     = 14  #
FROM_GEOMETRY = 15  #


# self-parsing tag repr
class Tag:
    def __init__(self, name, tag, encoding, help='', *,
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
        return 'Tag(%r, %r, %r%s)' % (
                self.name,
                self.tag,
                self.encoding,
                ', %r' % self.help if self.help else '')

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
            '^(?P<name>(?i:TAG)_[^ ]*) *= *(?P<tag>[^#]*?) *'
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
                if 'p' in t and tag & TAG_PERTURB:
                    r.append('p')
                if 'q' in t:
                    r.append('q%d' % t.get('q', tag))

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

# self-parsing from repr
class From:
    def __init__(self, name, from_, help='', *,
            lineno=0):
        self.name = name
        self.from_ = from_
        self.help = help
        self.lineno = lineno

    def __repr__(self):
        return 'From(%r, %r%s)' % (
                self.name,
                self.from_,
                ', ' % self.help if self.help else '')

    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __hash__(self):
        return hash(self.name)

    @staticmethod
    @ft.cache
    def froms():
        # parse our script's source to figure out froms
        import inspect
        import re
        froms = []
        from_pattern = re.compile(
            '^(?P<name>FROM_[^ ]*) *= *(?P<from>[^#]*?) *'
                '#+ *(?P<help>.*)$')
        for i, line in enumerate(
                inspect.getsource(inspect.getmodule(inspect.currentframe()))
                    .replace('\\\n', '')
                    .splitlines()):
            m = from_pattern.match(line)
            if m:
                froms.append(From(
                        m.group('name'),
                        globals()[m.group('name')],
                        m.group('help'),
                        lineno=1+i))
        return froms

    # find best matching from
    _sentinel = object()
    @staticmethod
    def find(from_, *, default=_sentinel):
        # find froms, note this is cached
        froms__ = From.froms()

        # find the most specific from encoder
        for f in froms__:
            if f.from_ == from_:
                return f
        # not found
        if default is From._sentinel:
            raise KeyError(from_)
        else:
            return default

# self-parsing rattr repr
class Rattr:
    def __init__(self, name, rattr, encoding, help='', *,
            lineno=0):
        self.name = name
        self.rattr = rattr
        self.encoding = encoding
        self.help = help
        self.lineno = lineno
        # derive mask from encoding
        self.mask = sum(
                (1 if x in 'v-01' else 0) << len(self.encoding)-1-i
                    for i, x in enumerate(self.encoding))

    def __repr__(self):
        return 'Rattr(%r, %r, %r%s)' % (
                self.name,
                self.rattr,
                self.encoding,
                ', %r' % self.help if self.help else '')

    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __hash__(self):
        return hash(self.name)

    def line(self):
        # substitute mask chars when zero
        rattr = '0x%s' % ''.join(
                n if n != '0' else next(
                    (x for x in self.encoding[i*4:i*4+4]
                        if x not in 'v-01+'),
                    '0')
                for i, n in enumerate('%08x' % self.rattr))
        # group into nibbles
        encoding = ' '.join(self.encoding[i*4:i*4+4]
                for i in range(len(self.encoding)//4))
        return ('LFS3_%s' % self.name, rattr, encoding)

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
    def rattrs():
        # parse our script's source to figure out rattrs
        import inspect
        import re
        rattrs = []
        rattr_pattern = re.compile(
            '^(?P<name>RATTR_[^ ]*) *= *(?P<rattr>[^#]*?) *'
                '#+ *(?P<encoding>(?:[^ ] *?){32}) *(?P<help>.*)$')
        for i, line in enumerate(
                inspect.getsource(inspect.getmodule(inspect.currentframe()))
                    .replace('\\\n', '')
                    .splitlines()):
            m = rattr_pattern.match(line)
            if m:
                rattrs.append(Rattr(
                        m.group('name'),
                        globals()[m.group('name')],
                        m.group('encoding').replace(' ', ''),
                        m.group('help'),
                        lineno=1+i))
        return rattrs

    # some field accessors
    @staticmethod
    def weight(rattr):
        return (0x3 & (rattr >> 30)) | -((0x3 & (rattr >> 30)) & 0x2)

    @staticmethod
    def weightcount(rattr):
        return 1 if Rattr.weight(rattr) == -2 else 0

    @staticmethod
    def len(rattr):
        return 1 + (0x7 & (rattr >> 27))

    @staticmethod
    def argcount(rattr):
        return Rattr.len(rattr) - Rattr.weightcount(rattr) - 1

    @staticmethod
    def from_(rattr):
        return 0x1f & (rattr >> 22)

    @staticmethod
    def fromcount(rattr):
        return 0x3f & (rattr >> 16)

    @staticmethod
    def tag(rattr):
        return 0xffff & rattr

    # human readable rattr repr
    @staticmethod
    def repr(rattr):
        # build repr
        r = []

        # rm bit?
        if rattr & tag_RM:
            r.append('rm')
        # grow bit?
        if rattr & tag_GROW:
            # noop?
            if Rattr.tag(rattr) == tag_GROW and Rattr.weight(rattr) == 0:
                r.append('noop')
            else:
                r.append('grow')
        # mask bits?
        if (rattr & tag_MASK) == tag_MASK12:
            r.append('mask12')
        elif (rattr & tag_MASK) == tag_MASK8:
            r.append('mask8')
        elif (rattr & tag_MASK) == tag_MASK2:
            r.append('mask2')

        # include tag if non-null, ignoring internal bits
        if rattr & 0xfff:
            r.append(Tag.repr(rattr & 0xfff))

        # truly null?
        if not r:
            r.append('null')

        # include weight
        weight = Rattr.weight(rattr)
        if weight in {+1, -1}:
            r.append('%sw%d' % (
                    '+' if weight > 0 else '-' if weight < 0 else '',
                    abs(weight)))
        elif weight == -2:
            r.append('w%')

        # include argcount
        argcount = Rattr.argcount(rattr)
        if argcount:
            r.append('%d' % argcount)

        # include from encoder, if there is one
        from_ = Rattr.from_(rattr)
        fromcount = Rattr.fromcount(rattr)
        if from_:
            try:
                f = From.find(from_)
                r.append('from%s' % f.name.split('_', 1)[1].lower())
            except KeyError:
                r.append('from 0x%x' % from_)

            if fromcount:
                r.append('%d' % fromcount)

        # include a % for each arg
        for _ in range(max(argcount, 0)):
            r.append('%')

        if not r:
            return '?'
        else:
            return ' '.join(r)



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


def list_rattrs():
    # find rattrs
    rattrs__ = Rattr.rattrs()

    # list
    lines = []
    for r in rattrs__:
        lines.append(r.line())

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

def dbg_rattrs(data, *,
        word_bits=32):
    # figure out le32 size in bytes
    if word_bits != 0:
        n = mt.ceil(word_bits / 8)

    lines = []
    # interpret as ints?
    if not isinstance(data, bytes):
        for rattr in data:
            lines.append((
                    '%0*x' % (2*n, rattr & ((1 << (8*n))-1)),
                    Rattr.repr(rattr)))

    # interpret as bytes?
    else:
        j = 0
        weightcount = 0
        argcount = 0
        argi = 0
        while j < len(data):
            # decode the next word
            word = 0
            d = 0
            while j+d < len(data) and d < n:
                word |= data[j+d] << (8*d)
                d += 1

            # rattr?
            if not weightcount and not argcount:
                weightcount = Rattr.weightcount(word)
                argcount = Rattr.argcount(word)
                argi = 0
                lines.append((
                        '%0*x' % (2*n, word),
                        Rattr.repr(word)))

                # found null terminator?
                if Rattr.len(word) == 0:
                    break

            # weight arg?
            elif weightcount:
                sword = word | -(word & (1 << (word_bits-1)))
                lines.append((
                        '%0*x' % (2*n, word),
                        'weight %sw%d' % (
                            '+' if sword > 0 else '-' if sword < 0 else '',
                            abs(sword))))
                weightcount -= 1

            # arg arg?
            elif argcount:
                sword = word | -(word & (1 << (word_bits-1)))
                lines.append((
                        '%0*x' % (2*n, word),
                        'arg%d %d' % (argi, sword)))
                argcount -= 1
                argi += 1

            j += d

    # figure out widths
    w = [0]
    for l in lines:
        w[0] = max(w[0], len(l[0]))

    # then print results
    for l in lines:
        print('%-*s    %s' % (
                w[0], l[0],
                l[1]))

def main(rattrs, *,
        list=False,
        hex=False,
        input=None,
        word_bits=32):
    import builtins
    list_, list = list, builtins.list
    hex_, hex = hex, builtins.hex

    # list all known rattrs
    if list_:
        list_rattrs()

    # interpret as a sequence of hex bytes
    elif hex_:
        bytes_ = [b for rattr in rattrs for b in rattr.split()]
        dbg_rattrs(bytes(int(b, 16) for b in bytes_),
                word_bits=word_bits)

    # parse rattrs in a file
    elif input:
        with openio(input, 'rb') as f:
            dbg_rattrs(f.read(),
                    word_bits=word_bits)

    # default to interpreting as ints
    else:
        dbg_rattrs((int(rattr, 0) for rattr in rattrs),
                word_bits=word_bits)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Decode littlefs rattrs.",
            allow_abbrev=False)
    parser.add_argument(
            'rattrs',
            nargs='*',
            help="Rattrs to decode.")
    parser.add_argument(
            '-l', '--list',
            action='store_true',
            help="List rattr encoding.")
    parser.add_argument(
            '-x', '--hex',
            action='store_true',
            help="Interpret as a sequence of hex bytes.")
    parser.add_argument(
            '-i', '--input',
            help="Read rattrs from this file. Can use - for stdin.")
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
