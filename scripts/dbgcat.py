#!/usr/bin/env python3

# prevent local imports
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

def main(disk, blocks=None, *,
        block_size=None,
        block_count=None,
        off=None,
        size=None):
    # is bd geometry specified?
    if isinstance(block_size, tuple):
        block_size, block_count_ = block_size
        if block_count is None:
            block_count = block_count_

    # flatten block, default to block 0
    if not blocks:
        blocks = [(0,)]
    blocks = [block for blocks_ in blocks for block in blocks_]

    with open(disk, 'rb') as f:
        # if block_size is omitted, assume the block device is one big block
        if block_size is None:
            f.seek(0, os.SEEK_END)
            block_size = f.tell()

        # blocks may also encode offsets
        blocks, offs, size = (
                [block[0] if isinstance(block, tuple) else block
                    for block in blocks],
                [off[0] if isinstance(off, tuple)
                        else off if off is not None
                        else size[0]
                        if isinstance(size, tuple) and len(size) > 1
                        else block[1] if isinstance(block, tuple)
                        else None
                    for block in blocks],
                size[1] - size[0] if isinstance(size, tuple) and len(size) > 1
                    else size[0] if isinstance(size, tuple)
                    else size if size is not None
                    else off[1] - off[0]
                    if isinstance(off, tuple) and len(off) > 1
                    else block_size)

        # cat the blocks
        for block, off in zip(blocks, offs):
            f.seek((block * block_size) + (off or 0))
            data = f.read(size)
            sys.stdout.buffer.write(data)
        sys.stdout.flush()


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Cat data from a block device.",
            allow_abbrev=False)
    parser.add_argument(
            'disk',
            help="File containing the block device.")
    parser.add_argument(
            'blocks',
            nargs='*',
            type=rbydaddr,
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
            type=lambda x: tuple(
                int(x, 0) if x.strip() else None
                    for x in x.split(',')),
            help="Show a specific offset, may be a range.")
    parser.add_argument(
            '-n', '--size',
            type=lambda x: tuple(
                int(x, 0) if x.strip() else None
                    for x in x.split(',')),
            help="Show this many bytes, may be a range.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
