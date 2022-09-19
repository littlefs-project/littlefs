#!/usr/bin/env python3
#
# Display operations on block devices based on trace output
#
# Example:
# ./scripts/tracebd.py trace
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

import collections as co
import functools as ft
import itertools as it
import math as m
import os
import re
import shutil
import threading as th
import time


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

# space filling Hilbert-curve
#
# note we memoize the last curve since this is a bit expensive
#
@ft.lru_cache(1)
def hilbert_curve(width, height):
    # based on generalized Hilbert curves:
    # https://github.com/jakubcerveny/gilbert
    #
    def hilbert_(x, y, a_x, a_y, b_x, b_y):
        w = abs(a_x+a_y)
        h = abs(b_x+b_y)
        a_dx = -1 if a_x < 0 else +1 if a_x > 0 else 0
        a_dy = -1 if a_y < 0 else +1 if a_y > 0 else 0
        b_dx = -1 if b_x < 0 else +1 if b_x > 0 else 0
        b_dy = -1 if b_y < 0 else +1 if b_y > 0 else 0

        # trivial row
        if h == 1:
            for _ in range(w):
                yield (x,y)
                x, y = x+a_dx, y+a_dy
            return

        # trivial column
        if w == 1:
            for _ in range(h):
                yield (x,y)
                x, y = x+b_dx, y+b_dy
            return

        a_x_, a_y_ = a_x//2, a_y//2
        b_x_, b_y_ = b_x//2, b_y//2
        w_ = abs(a_x_+a_y_)
        h_ = abs(b_x_+b_y_)

        if 2*w > 3*h:
            # prefer even steps
            if w_ % 2 != 0 and w > 2:
                a_x_, a_y_ = a_x_+a_dx, a_y_+a_dy

            # split in two
            yield from hilbert_(x, y, a_x_, a_y_, b_x, b_y)
            yield from hilbert_(x+a_x_, y+a_y_, a_x-a_x_, a_y-a_y_, b_x, b_y)
        else:
            # prefer even steps
            if h_ % 2 != 0 and h > 2:
                b_x_, b_y_ = b_x_+b_dx, b_y_+b_dy

            # split in three
            yield from hilbert_(x, y, b_x_, b_y_, a_x_, a_y_)
            yield from hilbert_(x+b_x_, y+b_y_, a_x, a_y, b_x-b_x_, b_y-b_y_)
            yield from hilbert_(
                x+(a_x-a_dx)+(b_x_-b_dx), y+(a_y-a_dy)+(b_y_-b_dy),
                -b_x_, -b_y_, -(a_x-a_x_), -(a_y-a_y_))

    if width >= height:
        curve = hilbert_(0, 0, +width, 0, 0, +height)
    else:
        curve = hilbert_(0, 0, 0, +height, +width, 0)

    return list(curve)

# space filling Z-curve/Lebesgue-curve
#
# note we memoize the last curve since this is a bit expensive
#
@ft.lru_cache(1)
def lebesgue_curve(width, height):
    # we create a truncated Z-curve by simply filtering out the points
    # that are outside our region
    curve = []
    for i in range(2**(2*m.ceil(m.log2(max(width, height))))):
        # we just operate on binary strings here because it's easier
        b = '{:0{}b}'.format(i, 2*m.ceil(m.log2(i+1)/2))
        x = int(b[1::2], 2) if b[1::2] else 0
        y = int(b[0::2], 2) if b[0::2] else 0
        if x < width and y < height:
            curve.append((x, y))

    return curve


class Block:
    def __init__(self, wear=0, readed=False, proged=False, erased=False):
        self._ = ((wear << 3)
            | (1 if readed else 0)
            | (2 if proged else 0)
            | (4 if erased else False))

    @property
    def wear(self):
        return self._ >> 3

    @property
    def readed(self):
        return (self._ & 1) != 0

    @property
    def proged(self):
        return (self._ & 2) != 0

    @property
    def erased(self):
        return (self._ & 4) != 0

    def read(self):
        self._ |= 1

    def prog(self):
        self._ |= 2

    def erase(self):
        self._ = (self._ | 4) + 8

    def clear(self):
        self._ &= ~7

    def reset(self):
        self._ = 0

    def copy(self):
        return Block(self.wear, self.readed, self.proged, self.erased)

    def __add__(self, other):
        return Block(
            max(self.wear, other.wear),
            self.readed | other.readed,
            self.proged | other.proged,
            self.erased | other.erased)

    def draw(self, *,
            subscripts=False,
            chars=None,
            wear_chars=None,
            color=True,
            read=True,
            prog=True,
            erase=True,
            wear=False,
            max_wear=None,
            block_cycles=None,
            **_):
        if not chars: chars = '.rpe'
        c = chars[0]
        f = []

        if wear:
            if not wear_chars and subscripts: wear_chars = '.₁₂₃₄₅₆789'
            elif not wear_chars:              wear_chars = '0123456789'

            if block_cycles:
                w = self.wear / block_cycles
            else:
                w = self.wear / max(max_wear, len(wear_chars)-1)

            c = wear_chars[min(
                int(w*(len(wear_chars)-1)),
                len(wear_chars)-1)]
            if color:
                if w*9 >= 9:   f.append('\x1b[1;31m')
                elif w*9 >= 7: f.append('\x1b[35m')

        if erase and self.erased:  c = chars[3]
        elif prog and self.proged: c = chars[2]
        elif read and self.readed: c = chars[1]

        if color:
            if erase and self.erased:  f.append('\x1b[44m')
            elif prog and self.proged: f.append('\x1b[45m')
            elif read and self.readed: f.append('\x1b[42m')

        if color:
            return '%s%c\x1b[m' % (''.join(f), c)
        else:
            return c

class Bd:
    def __init__(self, *, blocks=None, size=1, count=1, width=80):
        if blocks is not None:
            self.blocks = blocks
            self.size = size
            self.count = count
            self.width = width
        else:
            self.blocks = []
            self.size = None
            self.count = None
            self.width = None
            self.smoosh(size=size, count=count, width=width)

    def get(self, block=slice(None), off=slice(None)):
        if not isinstance(block, slice):
            block = slice(block, block+1)
        if not isinstance(off, slice):
            off = slice(off, off+1)

        if (not self.blocks
                or not self.width
                or not self.size
                or not self.count):
            return

        if self.count >= self.width:
            scale = (self.count+self.width-1) // self.width
            for i in range(
                    (block.start if block.start is not None else 0)//scale,
                    (min(block.stop if block.stop is not None else self.count,
                        self.count)+scale-1)//scale):
                yield self.blocks[i]
        else:
            scale = self.width // self.count
            for i in range(
                    block.start if block.start is not None else 0,
                    min(block.stop if block.stop is not None else self.count,
                        self.count)):
                for j in range(
                        ((off.start if off.start is not None else 0)
                            *scale)//self.size,
                        (min(off.stop if off.stop is not None else self.size,
                            self.size)*scale+self.size-1)//self.size):
                    yield self.blocks[i*scale+j]

    def __getitem__(self, block=slice(None), off=slice(None)):
        if isinstance(block, tuple):
            block, off = block
        if not isinstance(block, slice):
            block = slice(block, block+1)
        if not isinstance(off, slice):
            off = slice(off, off+1)

        # needs resize?
        if ((block.stop is not None and block.stop > self.count)
                or (off.stop is not None and off.stop > self.size)):
            self.smoosh(
                count=max(block.stop or self.count, self.count),
                size=max(off.stop or self.size, self.size))

        return self.get(block, off)

    def smoosh(self, *, size=None, count=None, width=None):
        size = size or self.size
        count = count or self.count
        width = width or self.width

        if count >= width:
            scale = (count+width-1) // width
            self.blocks = [
                sum(self.get(slice(i,i+scale)), start=Block())
                for i in range(0, count, scale)]
        else:
            scale = width // count
            self.blocks = [
                sum(self.get(i, slice(j*(size//width),(j+1)*(size//width))),
                    start=Block())
                for i in range(0, count)
                for j in range(scale)]

        self.size = size
        self.count = count
        self.width = width

    def read(self, block=slice(None), off=slice(None)):
        for c in self[block, off]:
            c.read()

    def prog(self, block=slice(None), off=slice(None)):
        for c in self[block, off]:
            c.prog()

    def erase(self, block=slice(None), off=slice(None)):
        for c in self[block, off]:
            c.erase()

    def clear(self, block=slice(None), off=slice(None)):
        for c in self[block, off]:
            c.clear()

    def reset(self, block=slice(None), off=slice(None)):
        for c in self[block, off]:
            c.reset()

    def copy(self):
        return Bd(
            blocks=[b.copy() for b in self.blocks],
            size=self.size, count=self.count, width=self.width)


def main(path='-', *,
        read=False,
        prog=False,
        erase=False,
        wear=False,
        color='auto',
        block=(None,None),
        off=(None,None),
        block_size=None,
        block_count=None,
        block_cycles=None,
        reset=False,
        width=None,
        height=1,
        scale=None,
        lines=None,
        coalesce=None,
        sleep=None,
        hilbert=False,
        lebesgue=False,
        keep_open=False,
        **args):
    # exclusive wear or read/prog/erase by default
    if not read and not prog and not erase and not wear:
        read = True
        prog = True
        erase = True
    # figure out what color should be
    if color == 'auto':
        color = sys.stdout.isatty()
    elif color == 'always':
        color = True
    else:
        color = False

    block_start = block[0]
    block_stop = block[1] if len(block) > 1 else block[0]+1
    off_start = off[0]
    off_stop = off[1] if len(off) > 1 else off[0]+1

    if block_start is None:
        block_start = 0
    if block_stop is None and block_count is not None:
        block_stop = block_count
    if off_start is None:
        off_start = 0
    if off_stop is None and block_size is not None:
        off_stop = block_size

    bd = Bd(
        size=(block_size if block_size is not None
            else off_stop-off_start if off_stop is not None
            else 1),
        count=(block_count if block_count is not None
            else block_stop-block_start if block_stop is not None
            else 1),
        width=(width or 80)*height)
    lock = th.Lock()
    event = th.Event()
    done = False

    # adjust width?
    def resmoosh():
        if width is None:
            w = shutil.get_terminal_size((80, 0))[0] * height
        elif width == 0:
            w = max(int(bd.count*(scale or 1)), 1)
        else:
            w = width * height

        if scale and int(bd.count*scale) > w:
            c = int(w/scale)
        elif scale and int(bd.count*scale) < w:
            w = max(int(bd.count*(scale or 1)), 1)
            c = bd.count
        else:
            c = bd.count

        if w != bd.width or c != bd.count:
            bd.smoosh(width=w, count=c)
    resmoosh()

    # parse a line of trace output
    pattern = re.compile(
        'trace.*?bd_(?:'
            '(?P<create>create\w*)\('
                '(?:'
                    'block_size=(?P<block_size>\w+)'
                    '|' 'block_count=(?P<block_count>\w+)'
                    '|' '.*?' ')*' '\)'
            '|' '(?P<read>read)\('
                '\s*(?P<read_ctx>\w+)\s*' ','
                '\s*(?P<read_block>\w+)\s*' ','
                '\s*(?P<read_off>\w+)\s*' ','
                '\s*(?P<read_buffer>\w+)\s*' ','
                '\s*(?P<read_size>\w+)\s*' '\)'
            '|' '(?P<prog>prog)\('
                '\s*(?P<prog_ctx>\w+)\s*' ','
                '\s*(?P<prog_block>\w+)\s*' ','
                '\s*(?P<prog_off>\w+)\s*' ','
                '\s*(?P<prog_buffer>\w+)\s*' ','
                '\s*(?P<prog_size>\w+)\s*' '\)'
            '|' '(?P<erase>erase)\('
                '\s*(?P<erase_ctx>\w+)\s*' ','
                '\s*(?P<erase_block>\w+)\s*' '\)'
            '|' '(?P<sync>sync)\('
                '\s*(?P<sync_ctx>\w+)\s*' '\)' ')')
    def parse_line(line):
        # string searching is actually much faster than
        # the regex here
        if 'trace' not in line or 'bd' not in line:
            return False
        m = pattern.search(line)
        if not m:
            return False

        if m.group('create'):
            # update our block size/count
            size = int(m.group('block_size'), 0)
            count = int(m.group('block_count'), 0)

            if off_stop is not None:
                size = off_stop-off_start
            if block_stop is not None:
                count = block_stop-block_start

            with lock:
                if reset:
                    bd.reset()

                # ignore the new values if block_stop/off_stop is explicit
                bd.smoosh(
                    size=(size if off_stop is None
                        else off_stop-off_start),
                    count=(count if block_stop is None
                        else block_stop-block_start))
            return True

        elif m.group('read') and read:
            block = int(m.group('read_block'), 0)
            off = int(m.group('read_off'), 0)
            size = int(m.group('read_size'), 0)

            if block_stop is not None and block >= block_stop:
                return False
            block -= block_start
            if off_stop is not None:
                if off >= off_stop:
                    return False
                size = min(size, off_stop-off)
            off -= off_start

            with lock:
                bd.read(block, slice(off,off+size))
            return True

        elif m.group('prog') and prog:
            block = int(m.group('prog_block'), 0)
            off = int(m.group('prog_off'), 0)
            size = int(m.group('prog_size'), 0)

            if block_stop is not None and block >= block_stop:
                return False
            block -= block_start
            if off_stop is not None:
                if off >= off_stop:
                    return False
                size = min(size, off_stop-off)
            off -= off_start

            with lock:
                bd.prog(block, slice(off,off+size))
            return True

        elif m.group('erase') and (erase or wear):
            block = int(m.group('erase_block'), 0)

            if block_stop is not None and block >= block_stop:
                return False
            block -= block_start

            with lock:
                bd.erase(block)
            return True

        else:
            return False


    # print a pretty line of trace output
    history = []
    def push_line():
        # create copy to avoid corrupt output
        with lock:
            resmoosh()
            bd_ = bd.copy()
            bd.clear()

        max_wear = None
        if wear:
            max_wear = max(b.wear for b in bd_.blocks)

        def draw(b):
            return b.draw(
                read=read,
                prog=prog,
                erase=erase,
                wear=wear,
                color=color,
                max_wear=max_wear,
                block_cycles=block_cycles,
                **args)

        # fold via a curve?
        if height > 1:
            w = (len(bd.blocks)+height-1) // height
            if hilbert:
                grid = {}
                for (x,y),b in zip(hilbert_curve(w, height), bd_.blocks):
                    grid[(x,y)] = draw(b)
                line = [
                    ''.join(grid.get((x,y), ' ') for x in range(w))
                    for y in range(height)]
            elif lebesgue:
                grid = {}
                for (x,y),b in zip(lebesgue_curve(w, height), bd_.blocks):
                    grid[(x,y)] = draw(b)
                line = [
                    ''.join(grid.get((x,y), ' ') for x in range(w))
                    for y in range(height)]
            else:
                line = [
                    ''.join(draw(b) for b in bd_.blocks[y*w:y*w+w])
                    for y in range(height)]
        else:
            line = [''.join(draw(b) for b in bd_.blocks)]

        if not lines:
            # just go ahead and print here
            for row in line:
                sys.stdout.write(row)
                sys.stdout.write('\n')
            sys.stdout.flush()
        else:
            history.append(line)
            del history[:-lines]

    last_rows = 1
    def print_line():
        nonlocal last_rows
        if not lines:
            return

        # give ourself a canvas
        while last_rows < len(history)*height:
            sys.stdout.write('\n')
            last_rows += 1

        for i, row in enumerate(it.chain.from_iterable(history)):
            jump = len(history)*height-1-i
            # move cursor, clear line, disable/reenable line wrapping
            sys.stdout.write('\r')
            if jump > 0:
                sys.stdout.write('\x1b[%dA' % jump)
            sys.stdout.write('\x1b[K')
            sys.stdout.write('\x1b[?7l')
            sys.stdout.write(row)
            sys.stdout.write('\x1b[?7h')
            if jump > 0:
                sys.stdout.write('\x1b[%dB' % jump)


    if sleep is None or (coalesce and not lines):
        # read/parse coalesce number of operations
        try:
            while True:
                with openio(path) as f:
                    changes = 0
                    for line in f:
                        change = parse_line(line)
                        changes += change
                        if change and changes % (coalesce or 1) == 0:
                            push_line()
                            print_line()
                            # sleep between coalesced lines?
                            if sleep is not None:
                                time.sleep(sleep)
                if not keep_open:
                    break
                # don't just flood open calls
                time.sleep(sleep)
        except KeyboardInterrupt:
            pass
    else:
        # read/parse in a background thread
        def parse():
            nonlocal done
            while True:
                with openio(path) as f:
                    changes = 0
                    for line in f:
                        change = parse_line(line)
                        changes += change
                        if change and changes % (coalesce or 1) == 0:
                            if coalesce:
                                push_line()
                            event.set()
                if not keep_open:
                    break
                # don't just flood open calls
                time.sleep(sleep)
            done = True

        th.Thread(target=parse, daemon=True).start()

        try:
            while not done:
                time.sleep(sleep)
                event.wait()
                event.clear()
                if not coalesce:
                    push_line()
                print_line()
        except KeyboardInterrupt:
            pass

    if lines:
        sys.stdout.write('\n')


if __name__ == "__main__":
    import sys
    import argparse
    parser = argparse.ArgumentParser(
        description="Display operations on block devices based on "
            "trace output.")
    parser.add_argument(
        'path',
        nargs='?',
        help="Path to read from.")
    parser.add_argument(
        '-r',
        '--read',
        action='store_true',
        help="Render reads.")
    parser.add_argument(
        '-p',
        '--prog',
        action='store_true',
        help="Render progs.")
    parser.add_argument(
        '-e',
        '--erase',
        action='store_true',
        help="Render erases.")
    parser.add_argument(
        '-w',
        '--wear',
        action='store_true',
        help="Render wear.")
    parser.add_argument(
        '--subscripts',
        help="Use unicode subscripts for showing wear.")
    parser.add_argument(
        '--chars',
        help="Characters to use for noop, read, prog, erase operations.")
    parser.add_argument(
        '--wear-chars',
        help="Characters to use to show wear.")
    parser.add_argument(
        '--color',
        choices=['never', 'always', 'auto'],
        default='auto',
        help="When to use terminal colors. Defaults to 'auto'.")
    parser.add_argument(
        '-b',
        '--block',
        type=lambda x: tuple(int(x,0) if x else None for x in x.split(',',1)),
        help="Show a specific block or range of blocks.")
    parser.add_argument(
        '-i',
        '--off',
        type=lambda x: tuple(int(x,0) if x else None for x in x.split(',',1)),
        help="Show a specific offset or range of offsets.")
    parser.add_argument(
        '-B',
        '--block-size',
        type=lambda x: int(x, 0),
        help="Assume a specific block size.")
    parser.add_argument(
        '--block-count',
        type=lambda x: int(x, 0),
        help="Assume a specific block count.")
    parser.add_argument(
        '-C',
        '--block-cycles',
        type=lambda x: int(x, 0),
        help="Assumed maximum number of erase cycles when measuring wear.")
    parser.add_argument(
        '-R',
        '--reset',
        action='store_true',
        help="Reset wear on block device initialization.")
    parser.add_argument(
        '-W',
        '--width',
        type=lambda x: int(x, 0),
        help="Width in columns. A width of 0 indicates no limit. Defaults "
            "to terminal width or 80.")
    parser.add_argument(
        '-H',
        '--height',
        type=lambda x: int(x, 0),
        help="Height in rows. Defaults to 1.")
    parser.add_argument(
        '-x',
        '--scale',
        type=float,
        help="Number of characters per block, ignores --width if set.")
    parser.add_argument(
        '-n',
        '--lines',
        type=lambda x: int(x, 0),
        help="Number of lines to show.")
    parser.add_argument(
        '-c',
        '--coalesce',
        type=lambda x: int(x, 0),
        help="Number of operations to coalesce together.")
    parser.add_argument(
        '-s',
        '--sleep',
        type=float,
        help="Time in seconds to sleep between reads, while coalescing "
            "operations.")
    parser.add_argument(
        '-I',
        '--hilbert',
        action='store_true',
        help="Render as a space-filling Hilbert curve.")
    parser.add_argument(
        '-Z',
        '--lebesgue',
        action='store_true',
        help="Render as a space-filling Z-curve.")
    parser.add_argument(
        '-k',
        '--keep-open',
        action='store_true',
        help="Reopen the pipe on EOF, useful when multiple "
            "processes are writing.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
