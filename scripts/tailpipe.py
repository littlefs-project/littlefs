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
    import os
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

# a pseudo-stdout ring buffer
class RingIO:
    def __init__(self, maxlen=None, head=False):
        self.maxlen = maxlen
        self.head = head
        self.lines = co.deque(
                maxlen=max(maxlen, 0) if maxlen is not None else None)
        self.tail = io.StringIO()

        # trigger automatic sizing
        self.resize(self.maxlen)

    @property
    def width(self):
        # just fetch this on demand, we don't actually use width
        return shutil.get_terminal_size((80, 5))[0]

    @property
    def height(self):
        # calculate based on terminal height?
        if self.maxlen is None or self.maxlen <= 0:
            return max(
                    shutil.get_terminal_size((80, 5))[1]
                        + (self.maxlen or 0),
                    0)
        # limit to maxlen
        else:
            return self.maxlen

    def resize(self, maxlen):
        self.maxlen = maxlen
        if maxlen is not None and maxlen <= 0:
            maxlen = self.height
        if maxlen != self.lines.maxlen:
            self.lines = co.deque(self.lines, maxlen=maxlen)

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

    # keep track of maximum drawn canvas
    canvas_lines = 1

    def draw(self):
        # did terminal size change?
        self.resize(self.maxlen)

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

        # build up the redraw in memory first and render in a single
        # write call, this minimizes flickering caused by the cursor
        # jumping around
        canvas = []

        # hide the cursor
        canvas.append('\x1b[?25l')

        # give ourself a canvas
        while RingIO.canvas_lines < len(lines):
            canvas.append('\n')
            RingIO.canvas_lines += 1

        # write lines from top to bottom so later lines overwrite earlier
        # lines, note xA/xB stop at terminal boundaries
        for i, line in enumerate(lines):
            # move to col 0
            canvas.append('\r')
            # move up to line
            if len(lines)-1-i > 0:
                canvas.append('\x1b[%dA' % (len(lines)-1-i))
            # clear line
            canvas.append('\x1b[K')
            # disable line wrap
            canvas.append('\x1b[?7l')
            # print the line
            canvas.append(line)
            # enable line wrap
            canvas.append('\x1b[?7h') # enable line wrap
            # move back down
            if len(lines)-1-i > 0:
                canvas.append('\x1b[%dB' % (len(lines)-1-i))

        # show the cursor again
        canvas.append('\x1b[?25h')

        # write to stdout and flush
        sys.stdout.write(''.join(canvas))
        sys.stdout.flush()


def main(path='-', *,
        lines=5,
        cat=False,
        coalesce=None,
        sleep=None,
        keep_open=False):
    lock = th.Lock()
    event = th.Event()

    def main_(ring):
        try:
            while True:
                with openio(path) as f:
                    count = 0
                    for line in f:
                        with lock:
                            ring.write(line)
                            count += 1

                            # wait for coalesce number of lines
                            if count >= (coalesce or 1):
                                event.set()
                                count = 0

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

    # cat? let main_ write directly to stdout
    if cat:
        main_(sys.stdout)

    # not cat? print in a background thread
    else:
        ring = RingIO(lines)
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

        main_(ring)

        done = True
        lock.acquire() # avoids https://bugs.python.org/issue42717
        # give ourselves one last draw, helps if background is
        # never triggered
        ring.draw()
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
            help="Show this many lines of history. <=0 uses the terminal "
                "height. Defaults to 5.")
    parser.add_argument(
            '-c', '--cat',
            action='store_true',
            help="Pipe directly to stdout.")
    parser.add_argument(
            '-S', '--coalesce',
            type=lambda x: int(x, 0),
            help="Number of lines to coalesce together.")
    parser.add_argument(
            '-s', '--sleep',
            type=float,
            help="Seconds to sleep between draws, coalescing lines in "
                "between.")
    parser.add_argument(
            '-k', '--keep-open',
            action='store_true',
            help="Reopen the pipe on EOF, useful when multiple "
                "processes are writing.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
