#!/usr/bin/env python3
#
# Efficiently displays the last n lines of a file/pipe.
#

import os
import sys
import threading as th
import time


def openio(path, mode='r'):
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

def main(path='-', *, lines=1, sleep=0.01, keep_open=False):
    ring = [None] * lines
    i = 0
    count = 0
    lock = th.Lock()
    event = th.Event()
    done = False

    # do the actual reading in a background thread
    def read():
        nonlocal i
        nonlocal count
        nonlocal done
        while True:
            with openio(path) as f:
                for line in f:
                    with lock:
                        ring[i] = line
                        i = (i + 1) % lines
                        count = min(lines, count + 1)
                    event.set()
            if not keep_open:
                break
            # don't just flood open calls
            time.sleep(sleep)
        done = True

    th.Thread(target=read, daemon=True).start()

    try:
        last_count = 1
        while not done:
            time.sleep(sleep)
            event.wait()
            event.clear()

            # create a copy to avoid corrupt output
            with lock:
                ring_ = ring.copy()
                i_ = i
                count_ = count

            # first thing first, give ourself a canvas
            while last_count < count_:
                sys.stdout.write('\n')
                last_count += 1

            for j in range(count_):
                # move cursor, clear line, disable/reenable line wrapping
                sys.stdout.write('\r')
                if count_-1-j > 0:
                    sys.stdout.write('\x1b[%dA' % (count_-1-j))
                sys.stdout.write('\x1b[K')
                sys.stdout.write('\x1b[?7l')
                sys.stdout.write(ring_[(i_-count_+j) % lines][:-1])
                sys.stdout.write('\x1b[?7h')
                if count_-1-j > 0:
                    sys.stdout.write('\x1b[%dB' % (count_-1-j))

            sys.stdout.flush()

    except KeyboardInterrupt:
        pass

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
        help="Number of lines to show, defaults to 1.")
    parser.add_argument(
        '-s',
        '--sleep',
        type=float,
        help="Seconds to sleep between reads, defaults to 0.01.")
    parser.add_argument(
        '-k',
        '--keep-open',
        action='store_true',
        help="Reopen the pipe on EOF, useful when multiple "
            "processes are writing.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_args()).items()
        if v is not None}))
