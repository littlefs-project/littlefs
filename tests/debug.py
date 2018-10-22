#!/usr/bin/env python2

import struct
import binascii

TYPES = {
    (0x1ff, 0x011): 'create reg',
    (0x1ff, 0x010): 'create dir',
    (0x1ff, 0x001): 'superblock',
    (0x1ff, 0x020): 'delete',
    (0x1f0, 0x0e0): 'globals',
    (0x1ff, 0x080): 'tail soft',
    (0x1ff, 0x081): 'tail hard',
    (0x1f0, 0x0a0): 'crc',
    (0x1ff, 0x040): 'struct dir',
    (0x1ff, 0x041): 'struct inline',
    (0x1ff, 0x042): 'struct ctz',
    (0x100, 0x100): 'attr',
}

def typeof(type):
    for prefix in range(9):
        mask = 0x1ff & ~((1 << prefix)-1)
        if (mask, type & mask) in TYPES:
            return TYPES[mask, type & mask] + (
                ' %0*x' % (prefix/4, type & ((1 << prefix)-1))
                if prefix else '')
    else:
        return '%02x' % type

def main(*blocks):
    # find most recent block
    file = None
    rev = None
    crc = None
    versions = []

    for block in blocks:
        try:
            nfile = open(block, 'rb')
            ndata = nfile.read(4)
            ncrc = binascii.crc32(ndata)
            nrev, = struct.unpack('<I', ndata)

            assert rev != nrev
            if not file or ((rev - nrev) & 0x80000000):
                file = nfile
                rev = nrev
                crc = ncrc

            versions.append((nrev, '%s (rev %d)' % (block, nrev)))
        except (IOError, struct.error):
            pass

    if not file:
        print 'Bad metadata pair {%s}' % ', '.join(blocks)
        return 1

    print "--- %s ---" % ', '.join(v for _,v in sorted(versions, reverse=True))

    # go through each tag, print useful information
    print "%-4s  %-8s  %-14s  %3s  %3s  %s" % (
        'off', 'tag', 'type', 'id', 'len', 'dump')

    tag = 0xffffffff
    off = 4
    while True:
        try:
            data = file.read(4)
            crc = binascii.crc32(data, crc)
            ntag, = struct.unpack('>I', data)
        except struct.error:
            break

        tag ^= ntag
        off += 4

        type = (tag & 0x7fc00000) >> 22
        id   = (tag & 0x003fe000) >> 13
        size = (tag & 0x00001fff) >> 0
        iscrc = (type & 0x1f0) == 0x0f0

        data = file.read(size if size != 0x1fff else 0)
        if iscrc:
            crc = binascii.crc32(data[:4], crc)
        else:
            crc = binascii.crc32(data, crc)

        print '%04x: %08x  %-14s  %3s  %3s  %-23s  %-8s' % (
            off, tag,
            typeof(type) + (' bad!' if iscrc and ~crc else ''),
            id if id != 0x1ff else '.',
            size if size != 0x1fff else 'x',
            ' '.join('%02x' % ord(c) for c in data[:8]),
            ''.join(c if c >= ' ' and c <= '~' else '.' for c in data[:8]))

        off += size if size != 0x1fff else 0
        if iscrc:
            crc = 0
            tag ^= (type & 1) << 31

    return 0

if __name__ == "__main__":
    import sys
    sys.exit(main(*sys.argv[1:]))
