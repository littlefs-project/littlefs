#!/usr/bin/env python3
#
# Efficiently displays the last n lines of a file/pipe.
#
# Example:
# ./scripts/tailpipe.py trace -n5
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

import collections as co
import io
import os
import shutil
import sys
import time


def openio(path, mode='r'):
    if path == '-':
        if mode == 'r':
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

class LinesIO:
    def __init__(self, maxlen=None):
        self.maxlen = maxlen
        self.lines = co.deque(maxlen=maxlen)
        self.tail = io.StringIO()

        # trigger automatic sizing
        if maxlen == 0:
            self.resize(0)

    def write(self, s):
        # note using split here ensures the trailing string has no newline
        lines = s.split('\n')

        if len(lines) > 1 and self.tail.getvalue():
            self.tail.write(lines[0])
            lines[0] = self.tail.getvalue()
            self.tail = io.StringIO()

        self.lines.extend(lines[:-1])

        if lines[-1]:
            self.tail.write(lines[-1])

    def resize(self, maxlen):
        self.maxlen = maxlen
        if maxlen == 0:
            maxlen = shutil.get_terminal_size((80, 5))[1]
        if maxlen != self.lines.maxlen:
            self.lines = co.deque(self.lines, maxlen=maxlen)

    last_lines = 1
    def draw(self):
        # did terminal size change?
        if self.maxlen == 0:
            self.resize(0)

        # first thing first, give ourself a canvas
        while LinesIO.last_lines < len(self.lines):
            sys.stdout.write('\n')
            LinesIO.last_lines += 1

        for j, line in enumerate(self.lines):
            # move cursor, clear line, disable/reenable line wrapping
            sys.stdout.write('\r')
            if len(self.lines)-1-j > 0:
                sys.stdout.write('\x1b[%dA' % (len(self.lines)-1-j))
            sys.stdout.write('\x1b[K')
            sys.stdout.write('\x1b[?7l')
            sys.stdout.write(line)
            sys.stdout.write('\x1b[?7h')
            if len(self.lines)-1-j > 0:
                sys.stdout.write('\x1b[%dB' % (len(self.lines)-1-j))
        sys.stdout.flush()


def main(path='-', *, lines=5, cat=False, sleep=0.01, keep_open=False):
    if cat:
        ring = sys.stdout
    else:
        ring = LinesIO(lines)

    ptime = time.time()
    try:
        while True:
            with openio(path) as f:
                for line in f:
                    ring.write(line)

                    # need to redraw?
                    if not cat and time.time()-ptime >= sleep:
                        ring.draw()
                        ptime = time.time()

            if not keep_open:
                break
            # don't just flood open calls
            time.sleep(sleep or 0.1)
    except KeyboardInterrupt:
        pass

    if not cat:
        sys.stdout.write('\n')


if __name__ == "__main__":
    import sys
    import argparse
    parser = argparse.ArgumentParser(
        description="Efficiently displays the last n lines of a file/pipe.")
    parser.add_argument(
        'path',
        nargs='?',
        help="Path to read from.")
    parser.add_argument(
        '-n',
        '--lines',
        type=lambda x: int(x, 0),
        help="Show this many lines of history. 0 uses the terminal height. "
            "Defaults to 5.")
    parser.add_argument(
        '-z', '--cat',
        action='store_true',
        help="Pipe directly to stdout.")
    parser.add_argument(
        '-s', '--sleep',
        type=float,
        help="Seconds to sleep between reads. Defaults to 0.01.")
    parser.add_argument(
        '-k', '--keep-open',
        action='store_true',
        help="Reopen the pipe on EOF, useful when multiple "
            "processes are writing.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_intermixed_args()).items()
        if v is not None}))
