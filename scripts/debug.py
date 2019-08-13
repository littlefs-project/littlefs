#!/usr/bin/env python2

import struct
import binascii

TYPES = {
    (0x700, 0x400): 'splice',
    (0x7ff, 0x401): 'create',
    (0x7ff, 0x4ff): 'delete',
    (0x700, 0x000): 'name',
    (0x7ff, 0x001): 'name reg',
    (0x7ff, 0x002): 'name dir',
    (0x7ff, 0x0ff): 'name superblock',
    (0x700, 0x200): 'struct',
    (0x7ff, 0x200): 'struct dir',
    (0x7ff, 0x202): 'struct ctz',
    (0x7ff, 0x201): 'struct inline',
    (0x700, 0x300): 'userattr',
    (0x700, 0x600): 'tail',
    (0x7ff, 0x600): 'tail soft',
    (0x7ff, 0x601): 'tail hard',
    (0x700, 0x700): 'gstate',
    (0x7ff, 0x7ff): 'gstate move',
    (0x700, 0x500): 'crc',
}

def typeof(type):
    for prefix in range(12):
        mask = 0x7ff & ~((1 << prefix)-1)
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
    print "%-4s  %-8s  %-14s  %3s %4s  %s" % (
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

        type = (tag & 0x7ff00000) >> 20
        id   = (tag & 0x000ffc00) >> 10
        size = (tag & 0x000003ff) >> 0
        iscrc = (type & 0x700) == 0x500

        data = file.read(size if size != 0x3ff else 0)
        if iscrc:
            crc = binascii.crc32(data[:4], crc)
        else:
            crc = binascii.crc32(data, crc)

        print '%04x: %08x  %-15s %3s %4s  %-23s  %-8s' % (
            off, tag,
            typeof(type) + (' bad!' if iscrc and ~crc else ''),
            hex(id)[2:] if id != 0x3ff else '.',
            size if size != 0x3ff else 'x',
            ' '.join('%02x' % ord(c) for c in data[:8]),
            ''.join(c if c >= ' ' and c <= '~' else '.' for c in data[:8]))

        off += size if size != 0x3ff else 0
        if iscrc:
            crc = 0
            tag ^= (type & 1) << 31

    return 0

if __name__ == "__main__":
    import sys
    sys.exit(main(*sys.argv[1:]))
