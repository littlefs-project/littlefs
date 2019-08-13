#!/usr/bin/env python2

import struct
import sys
import time
import os
import re

def main():
    with open('blocks/.config') as file:
        read_size, prog_size, block_size, block_count = (
            struct.unpack('<LLLL', file.read()))

    real_size = sum(
        os.path.getsize(os.path.join('blocks', f))
        for f in os.listdir('blocks') if re.match('\d+', f))

    with open('blocks/.stats') as file:
        read_count, prog_count, erase_count = (
            struct.unpack('<QQQ', file.read()))

    runtime = time.time() - os.stat('blocks').st_ctime

    print 'results: %dB %dB %dB %.3fs' % (
        read_count, prog_count, erase_count, runtime)

if __name__ == "__main__":
    main(*sys.argv[1:])
