#!/usr/bin/env python3

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

def main(path, lines=1, keep_open=False):
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
            with openio(path, 'r') as f:
                for line in f:
                    with lock:
                        ring[i] = line
                        i = (i + 1) % lines
                        count = min(lines, count + 1)
                    event.set()
            if not keep_open:
                break
        done = True

    th.Thread(target=read, daemon=True).start()

    try:
        last_count = 1
        while not done:
            time.sleep(0.01)
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
                sys.stdout.write('\r%s\x1b[K\x1b[?7l%s\x1b[?7h%s' % (
                    '\x1b[%dA' % (count_-1-j) if count_-1-j > 0 else '',
                    ring_[(i_-count+j) % lines][:-1],
                    '\x1b[%dB' % (count_-1-j) if count_-1-j > 0 else ''))

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
        default='-',
        help="Path to read from.")
    parser.add_argument(
        '-n',
        '--lines',
        type=lambda x: int(x, 0),
        default=1,
        help="Number of lines to show, defaults to 1.")
    parser.add_argument(
        '-k',
        '--keep-open',
        action='store_true',
        help="Reopen the pipe on EOF, useful when multiple "
            "processes are writing.")
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_args()).items()
        if v is not None}))
