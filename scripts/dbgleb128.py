#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import io
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

def fromleb128(data):
    word = 0
    for i, b in enumerate(data):
        word |= ((b & 0x7f) << 7*i)
        word &= 0xffffffff
        if not b & 0x80:
            return word, i+1
    return word, len(data)

def dbg_leb128s(data):
    lines = []
    j = 0
    while j < len(data):
        word, d = fromleb128(data[j:])
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
        input=None):
    hex_ = hex; del hex

    # interpret as a sequence of hex bytes
    if hex_:
        bytes_ = [b for leb128 in leb128s for b in leb128.split()]
        dbg_leb128s(bytes(int(b, 16) for b in bytes_))

    # parse leb128s in a file
    elif input:
        with openio(input, 'rb') as f:
            dbg_leb128s(f.read())

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
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
