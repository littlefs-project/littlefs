#!/usr/bin/env python3
#
# Traditional watch command, but with higher resolution updates and a bit
# different options/output format
#
# Example:
# ./scripts/watch.py -s0.1 date
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import errno
import fcntl
import io
import os
import pty
import re
import shutil
import struct
import subprocess as sp
import sys
import termios
import time

try:
    import inotify_simple
except ModuleNotFoundError:
    inotify_simple = None


# open with '-' for stdin/stdout
def openio(path, mode='r', buffering=-1):
    import os
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

if inotify_simple is None:
    Inotify = None
else:
    class Inotify(inotify_simple.INotify):
        def __init__(self, paths):
            super().__init__()

            # wait for interesting events
            flags = (inotify_simple.flags.ATTRIB
                    | inotify_simple.flags.CREATE
                    | inotify_simple.flags.DELETE
                    | inotify_simple.flags.DELETE_SELF
                    | inotify_simple.flags.MODIFY
                    | inotify_simple.flags.MOVED_FROM
                    | inotify_simple.flags.MOVED_TO
                    | inotify_simple.flags.MOVE_SELF)

            # recurse into directories
            for path in paths:
                if os.path.isdir(path):
                    for dir, _, files in os.walk(path):
                        self.add_watch(dir, flags)
                        for f in files:
                            self.add_watch(os.path.join(dir, f), flags)
                else:
                    self.add_watch(path, flags)

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


def main(command, *,
        lines=0,
        head=False,
        cat=False,
        sleep=None,
        keep_open=False,
        keep_open_paths=None,
        buffer=False,
        ignore_errors=False,
        exit_on_error=False):
    if not command:
        print('usage: %s [options] command' % sys.argv[0],
                file=sys.stderr)
        sys.exit(-1)

    # if we have keep_open_paths, assume user wanted keep_open
    if keep_open_paths and not keep_open:
        keep_open = True

    # figure out the keep_open paths
    if keep_open and inotify_simple is not None:
        if keep_open_paths:
            keep_open_paths = set(keep_open_paths)
        else:
            # guess inotify paths from command
            keep_open_paths = set()
            for p in command:
                for p in {
                        p,
                        re.sub('^-.', '', p),
                        re.sub('^--[^=]+=', '', p)}:
                    if p and os.path.exists(p):
                        keep_open_paths.add(p)

    returncode = 0
    try:
        while True:
            # reset ring each run
            if cat:
                ring = sys.stdout
            else:
                ring = RingIO(lines, head)

            try:
                # register inotify before running the command, this avoids
                # modification race conditions
                if keep_open and Inotify:
                    inotify = Inotify(keep_open_paths)

                # run the command under a pseudoterminal
                mpty, spty = pty.openpty()

                # forward terminal size
                if cat:
                    w, h = shutil.get_terminal_size((80, 5))
                else:
                    w, h = ring.width, ring.height
                fcntl.ioctl(spty, termios.TIOCSWINSZ,
                        struct.pack('HHHH', h, w, 0, 0))

                proc = sp.Popen(command,
                        stdout=spty,
                        stderr=spty,
                        close_fds=False)
                os.close(spty)
                mpty = os.fdopen(mpty, 'r', 1)

                while True:
                    try:
                        line = mpty.readline()
                    except OSError as e:
                        if e.errno != errno.EIO:
                            raise
                        break
                    if not line:
                        break

                    if cat or not head or (head and len(ring) < h):
                        ring.write(line)
                        if not cat and not buffer and not ignore_errors:
                            ring.draw()

                mpty.close()
                proc.wait()

                if ((buffer or ignore_errors or (not cat and len(ring) == 0))
                        and not (ignore_errors and proc.returncode != 0)):
                    ring.draw()
                if exit_on_error and proc.returncode != 0:
                    returncode = proc.returncode
                    break

            except OSError as e:
                if e.errno != errno.ETXTBSY:
                    raise
                pass

            # try to inotifywait
            if keep_open and Inotify:
                ptime = time.time()
                inotify.read()
                inotify.close()
                # sleep a minimum amount of time to avoid flickering
                time.sleep(max(0, (sleep or 0.01) - (time.time()-ptime)))
            # or sleep
            else:
                time.sleep(sleep or 2)
    except KeyboardInterrupt:
        pass

    if not cat:
        sys.stdout.write('\n')
    sys.exit(returncode)


if __name__ == "__main__":
    import sys
    import argparse
    parser = argparse.ArgumentParser(
            description="Traditional watch command, but with higher "
                "resolution updates and a bit different options/output "
                "format.",
            allow_abbrev=False)
    parser.add_argument(
            'command',
            nargs=argparse.REMAINDER,
            help="Command to run.")
    parser.add_argument(
            '-n', '--lines',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Show this many lines of history. <=0 uses the terminal "
                "height. Defaults to 0.")
    parser.add_argument(
            '-^', '--head',
            action='store_true',
            help="Show the first n lines.")
    parser.add_argument(
            '-c', '--cat',
            action='store_true',
            help="Pipe directly to stdout.")
    parser.add_argument(
            '-s', '--sleep',
            type=float,
            help="Seconds to sleep between runs. Defaults to 2 seconds.")
    parser.add_argument(
            '-k', '--keep-open',
            action='store_true',
            help="Try to use inotify to wait for changes. Defaults to "
                "guessing, or explicit paths can be provided with "
                "-K/--keep-open-path.")
    parser.add_argument(
            '-K', '--keep-open-path',
            dest='keep_open_paths',
            action='append',
            help="Use this path for inotify. Implies --keep-open.")
    parser.add_argument(
            '-b', '--buffer',
            action='store_true',
            help="Wait until command finishes to show the output.")
    parser.add_argument(
            '-i', '--ignore-errors',
            action='store_true',
            help="Only show output after successful runs. Implies --buffer.")
    parser.add_argument(
            '-e', '--exit-on-error',
            action='store_true',
            help="Exit on error.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_args()).items()
            if v is not None}))
