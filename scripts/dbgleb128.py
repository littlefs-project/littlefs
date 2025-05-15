#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import io
import math as mt
import os
import struct
import sys


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

def dbg_leb128s(data, *,
        word_bits=32):
    # figure out leb128 size in bytes
    if word_bits != 0:
        n = mt.ceil(word_bits / 7)

    # parse leb128s
    lines = []
    j = 0
    while j < len(data):
        # bounded leb128s?
        if word_bits != 0:
            word, d = fromleb128(data[j:j+n])
        # unbounded?
        else:
            word, d = fromleb128(data, j)

        lines.append((
                ' '.join('%02x' % b for b in data[j:j+d]),
                word))
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

def main(leb128s, *,
        hex=False,
        input=None,
        word_bits=32):
    import builtins
    hex_, hex = hex, builtins.hex

    # interpret as a sequence of hex bytes
    if hex_:
        bytes_ = [b for leb128 in leb128s for b in leb128.split()]
        dbg_leb128s(bytes(int(b, 16) for b in bytes_),
                word_bits=word_bits)

    # parse leb128s in a file
    elif input:
        with openio(input, 'rb') as f:
            dbg_leb128s(f.read(),
                    word_bits=word_bits)

    # we don't currently have a default interpretation
    else:
        print("error: no -x/--hex or -i/--input?",
                file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Decode leb128s.",
            allow_abbrev=False)
    parser.add_argument(
            'leb128s',
            nargs='*',
            help="Leb128s to decode.")
    parser.add_argument(
            '-x', '--hex',
            action='store_true',
            help="Interpret as a sequence of hex bytes.")
    parser.add_argument(
            '-i', '--input',
            help="Read leb128s from this file. Can use - for stdin.")
    parser.add_argument(
            '-w', '--word-bits',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Word size in bits. 0 is unbounded. Defaults to 32.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
