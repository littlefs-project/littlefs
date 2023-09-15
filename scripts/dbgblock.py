#!/usr/bin/env python3

import os


# parse some rbyd addr encodings
# 0xa     -> [0xa]
# 0xa.b   -> ([0xa], b)
# 0x{a,b} -> [0xa, 0xb]
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

def xxd(data, width=16):
    for i in range(0, len(data), width):
        yield '%-*s %-*s' % (
            3*width,
            ' '.join('%02x' % b for b in data[i:i+width]),
            width,
            ''.join(
                b if b >= ' ' and b <= '~' else '.'
                for b in map(chr, data[i:i+width])))

def main(disk, block=None, *,
        block_size=None,
        seek=None,
        size=None):
    # flatten block, default to block 0
    if not block:
        block = [0]

    if len(block) > 1:
        print("error: More than one block address?")
        sys.exit(-1)

    block = block[0]

    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # block may also encode an offset
        block, off, size = (
            block[0] if isinstance(block, tuple) else block,
            seek if seek is not None
                else block[1] if isinstance(block, tuple)
                else None,
            size if size is not None else block_size)

        print('block %s, size %d' % (
            '0x%x.%x' % (block, off)
                if off is not None
                else '0x%x' % block,
            size))

        # read the block
        f.seek((block * block_size) + (off or 0))
        data = f.read(size)

        # print header
        print('%-8s  %s' % ('off', 'data'))

        # render the hex view
        for o, line in enumerate(xxd(data)):
            print('%08x: %s' % ((off or 0) + 16*o, line))

if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Debug block devices.",
        allow_abbrev=False)
    parser.add_argument(
        'disk',
        help="File containing the block device.")
    parser.add_argument(
        'block',
        nargs='?',
        type=rbydaddr,
        help="Block address.")
    parser.add_argument(
        '-B', '--block-size',
        type=lambda x: int(x, 0),
        help="Block size in bytes.")
    parser.add_argument(
        '-s', '--seek',
        type=lambda x: int(x, 0),
        help="Start at this offset.")
    parser.add_argument(
        '-n', '--size',
        type=lambda x: int(x, 0),
        help="Show this many bytes.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
