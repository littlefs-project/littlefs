#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import os


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
# 0xa       -> (0xa,)
# 0xa.c     -> ((0xa, 0xc),)
# 0x{a,b}   -> (0xa, 0xb)
# 0x{a,b}.c -> ((0xa, 0xc), (0xb, 0xc))
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

    return tuple(addr)

def xxd(data, width=16):
    for i in range(0, len(data), width):
        yield '%-*s %-*s' % (
                3*width,
                ' '.join('%02x' % b for b in data[i:i+width]),
                width,
                ''.join(
                    b if b >= ' ' and b <= '~' else '.'
                        for b in map(chr, data[i:i+width])))

def crc32c(data, crc=0):
    crc ^= 0xffffffff
    for b in data:
        crc ^= b
        for j in range(8):
            crc = (crc >> 1) ^ ((crc & 1) * 0x82f63b78)
    return 0xffffffff ^ crc

def main(disk, block=None, *,
        block_size=None,
        block_count=None,
        off=None,
        size=None,
        cksum=False):
    # is bd geometry specified?
    if isinstance(block_size, tuple):
        block_size, block_count_ = block_size
        if block_count is None:
            block_count = block_count_

    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # block may also encode an offset
        block, off, size = (
                block[0] if isinstance(block, tuple)
                    else block,
                off.start if isinstance(off, slice)
                    else off if off is not None
                    else size.start if isinstance(size, slice)
                    else block[1] if isinstance(block, tuple)
                    else None,
                (size.stop - (size.start or 0)
                        if size.stop is not None
                        else None) if isinstance(size, slice)
                    else size if size is not None
                    else ((off.stop - (off.start or 0))
                        if off.stop is not None
                        else None) if isinstance(off, slice)
                    else None)

        # bound to block_size
        block_ = block if block is not None else 0
        off_ = off if off is not None else 0
        size_ = size if size is not None else block_size - off_

        # read the block
        f.seek((block_ * block_size) + off_)
        data = f.read(size_)

        # calculate checksum
        cksum = crc32c(data)

        # print the header
        print('block %s, size %d, cksum %08x' % (
                '0x%x.%x' % (block_, off_)
                    if off is not None
                    else '0x%x' % block_,
                size_,
                cksum))

        # render the hex view
        for o, line in enumerate(xxd(data)):
            print('%08x: %s' % (off_ + 16*o, line))


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
            type=lambda x: (lambda x: x)(*rbydaddr(x)),
            help="Block address.")
    parser.add_argument(
            '-b', '--block-size',
            type=bdgeom,
            help="Block size/geometry in bytes.")
    parser.add_argument(
            '--block-count',
            type=lambda x: int(x, 0),
            help="Block count in blocks.")
    parser.add_argument(
            '--off',
            type=lambda x: (
                slice(*(int(x, 0) if x.strip() else None
                        for x in x.split(',', 1)))
                    if ',' in x
                    else int(x, 0)),
            help="Show a specific offset, may be a range.")
    parser.add_argument(
            '-n', '--size',
            type=lambda x: (
                slice(*(int(x, 0) if x.strip() else None
                        for x in x.split(',', 1)))
                    if ',' in x
                    else int(x, 0)),
            help="Show this many bytes, may be a range.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
