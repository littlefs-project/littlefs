#!/usr/bin/env python

import struct
import sys
import os

def main(*paths):
    # find most recent block
    file = None
    rev = None
    for path in paths:
        try:
            nfile = open(path, 'r+b')
            nrev, = struct.unpack('<I', nfile.read(4))

            assert rev != nrev
            if not file or ((rev - nrev) & 0x80000000):
                file = nfile
                rev = nrev
        except IOError:
            pass

    # go to last commit
    tag = 0
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

if __name__ == "__main__":
    main(*sys.argv[1:])
