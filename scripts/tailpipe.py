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

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import io
import os
import select
import shutil
import sys
import threading as th
import time


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

class RingIO:
    def __init__(self, maxlen=None, head=False):
        self.maxlen = maxlen
        self.head = head
        self.lines = co.deque(maxlen=maxlen)
        self.tail = io.StringIO()

        # trigger automatic sizing
        if maxlen == 0:
            self.resize(0)

    def __len__(self):
        return len(self.lines)

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

    canvas_lines = 1
    def draw(self):
        # did terminal size change?
        if self.maxlen == 0:
            self.resize(0)

        # copy lines
        lines = self.lines.copy()
        # pad to fill any existing canvas, but truncate to terminal size
        h = shutil.get_terminal_size((80, 5))[1]
        lines.extend('' for _ in range(
                len(lines),
                min(RingIO.canvas_lines, h)))
        while len(lines) > h:
            if self.head:
                lines.pop()
            else:
                lines.popleft()

        # first thing first, give ourself a canvas
        while RingIO.canvas_lines < len(lines):
            sys.stdout.write('\n')
            RingIO.canvas_lines += 1

        # write lines from top to bottom so later lines overwrite earlier
        # lines, note [xA/[xB stop at terminal boundaries
        for i, line in enumerate(lines):
            # move cursor, clear line, disable/reenable line wrapping
            sys.stdout.write('\r')
            if len(lines)-1-i > 0:
                sys.stdout.write('\x1b[%dA' % (len(lines)-1-i))
            sys.stdout.write('\x1b[K')
            sys.stdout.write('\x1b[?7l')
            sys.stdout.write(line)
            sys.stdout.write('\x1b[?7h')
            if len(lines)-1-i > 0:
                sys.stdout.write('\x1b[%dB' % (len(lines)-1-i))
        sys.stdout.flush()


def main(path='-', *,
        lines=5,
        cat=False,
        sleep=None,
        keep_open=False):
    if cat:
        ring = sys.stdout
    else:
        ring = RingIO(lines)

    # if sleep print in background thread to avoid getting stuck in a read call
    event = th.Event()
    lock = th.Lock()
    if not cat:
        done = False
        def background():
            while not done:
                event.wait()
                event.clear()
                with lock:
                    ring.draw()
                # sleep a minimum amount of time to avoid flickering
                time.sleep(sleep or 0.01)
        th.Thread(target=background, daemon=True).start()

    try:
        while True:
            with openio(path) as f:
                for line in f:
                    with lock:
                        ring.write(line)
                        event.set()

            if not keep_open:
                break
            # don't just flood open calls
            time.sleep(sleep or 2)
    except FileNotFoundError as e:
        print("error: file not found %r" % path,
                file=sys.stderr)
        sys.exit(-1)
    except KeyboardInterrupt:
        pass

    if not cat:
        done = True
        lock.acquire() # avoids https://bugs.python.org/issue42717
        sys.stdout.write('\n')


if __name__ == "__main__":
    import sys
    import argparse
    parser = argparse.ArgumentParser(
            description="Efficiently displays the last n lines of a "
                "file/pipe.",
            allow_abbrev=False)
    parser.add_argument(
            'path',
            nargs='?',
            help="Path to read from.")
    parser.add_argument(
            '-n', '--lines',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Show this many lines of history. 0 uses the terminal "
                "height. Defaults to 5.")
    parser.add_argument(
            '-z', '--cat',
            action='store_true',
            help="Pipe directly to stdout.")
    parser.add_argument(
            '-s', '--sleep',
            type=float,
            help="Seconds to sleep between reads.")
    parser.add_argument(
            '-k', '--keep-open',
            action='store_true',
            help="Reopen the pipe on EOF, useful when multiple "
                "processes are writing.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
