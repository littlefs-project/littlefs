#!/usr/bin/env python

import struct
import sys
import os
import argparse

def corrupt(block):
    with open(block, 'r+b') as file:
        # skip rev
        file.read(4)

        # go to last commit
        tag = 0xffffffff
        while True:
            try:
                ntag, = struct.unpack('<I', file.read(4))
            except struct.error:
                break

            tag ^= ntag
            file.seek(tag & 0xfff, os.SEEK_CUR)

        # lob off last 3 bytes
        file.seek(-((tag & 0xfff) + 3), os.SEEK_CUR)
        file.truncate()

def main(args):
    if args.n or not args.blocks:
        with open('blocks/.history', 'rb') as file:
            for i in range(int(args.n or 1)):
                last, = struct.unpack('<I', file.read(4))
                args.blocks.append('blocks/%x' % last)

    for block in args.blocks:
        print 'corrupting %s' % block
        corrupt(block)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument('-n')
    parser.add_argument('blocks', nargs='*')
    main(parser.parse_args())
