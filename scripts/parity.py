#!/usr/bin/env python3

import io
import os
import struct
import sys
import functools as ft
import operator as op


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

def popc(x):
    return bin(x).count('1')

def parity(x):
    return popc(x) & 1


def main(paths, **args):
    # interpret as sequence of hex bytes
    if args.get('hex'):
        print('%01x' % parity(ft.reduce(
            op.xor,
            bytes(int(path, 16) for path in paths),
            0)))

    # interpret as strings
    elif args.get('string'):
        for path in paths:
            print('%01x' % parity(ft.reduce(
                op.xor,
                path.encode('utf8'),
                0)))

    # default to interpreting as paths
    else:
        if not paths:
            paths = [None]

        for path in paths:
            with openio(path or '-', 'rb') as f:
                # calculate parity
                xor = 0
                while True:
                    block = f.read(io.DEFAULT_BUFFER_SIZE)
                    if not block:
                        break

                    xor = ft.reduce(op.xor, block, xor)

                # print what we found
                if path is not None:
                    print('%01x  %s' % (parity(xor), path))
                else:
                    print('%01x' % parity(xor))

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Calculates parity.",
        allow_abbrev=False)
    parser.add_argument(
        'paths',
        nargs='*',
        help="Paths to read. Reads stdin by default.")
    parser.add_argument(
        '-x', '--hex',
        action='store_true',
        help="Interpret as a sequence of hex bytes.")
    parser.add_argument(
        '-s', '--string',
        action='store_true',
        help="Interpret as strings.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
