#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import functools as ft


# Error codes
ERR_OK          = 0     # No error
ERR_UNKNOWN     = -1    # Unknown error
ERR_INVAL       = -22   # Invalid parameter
ERR_NOTSUP      = -95   # Operation not supported
ERR_IO          = -5    # Error during device operation
ERR_CORRUPT     = -84   # Corrupted
ERR_NOENT       = -2    # No directory entry
ERR_EXIST       = -17   # Entry already exists
ERR_NOTDIR      = -20   # Entry is not a dir
ERR_ISDIR       = -21   # Entry is a dir
ERR_NOTEMPTY    = -39   # Dir is not empty
ERR_FBIG        = -27   # File too large
ERR_NOSPC       = -28   # No space left on device
ERR_NOMEM       = -12   # No more memory available
ERR_NOATTR      = -61   # No data/attr available
ERR_NAMETOOLONG = -36   # File name too long
ERR_RANGE       = -34   # Result out of range


# self-parsing error codes
class Err:
    def __init__(self, name, code, help):
        self.name = name
        self.code = code
        self.help = help

    def __repr__(self):
        return 'Err(%r, %r, %r)' % (
                self.name,
                self.code,
                self.help)

    def __eq__(self, other):
        return self.name == other.name

    def __ne__(self, other):
        return self.name != other.name

    def __hash__(self):
        return hash(self.name)

    def line(self):
        return ('LFS3_%s' % self.name, '%d' % self.code, self.help)

@ft.cache
def errs():
    # parse our script's source to figure out errs
    import inspect
    import re
    errs = []
    err_pattern = re.compile(
            '^(?P<name>ERR_[^ ]*) *= *(?P<code>[^#]*?) *'
                '#+ *(?P<help>.*)$')
    for line in (inspect.getsource(inspect.getmodule(inspect.currentframe()))
            .replace('\\\n', '')
            .splitlines()):
        m = err_pattern.match(line)
        if m:
            errs.append(Err(
                    m.group('name'),
                    globals()[m.group('name')],
                    m.group('help')))
    return errs


def main(errs, *,
        list=False):
    import builtins
    list_, list = list, builtins.list

    # find errs
    errs__ = globals()['errs']()

    lines = []
    # list all known error codes
    if list_:
        for e in errs__:
            lines.append(e.line())

    # find errs by name or value
    else:
        for e_ in errs:
            found = False
            # find by LFS3_ERR_+name
            for e in errs__:
                if 'LFS3_%s' % e.name.upper() == e_.upper():
                    lines.append(e.line())
                    found = True
            if found:
                continue
            # find by ERR_+name
            for e in errs__:
                if e.name.upper() == e_.upper():
                    lines.append(e.line())
                    found = True
            if found:
                continue
            # find by name
            for e in errs__:
                if e.name.split('_', 1)[1] == e_.upper():
                    lines.append(e.line())
                    found = True
            if found:
                continue
            # find by E+name
            for e in errs__:
                if 'E%s' % e.name.split('_', 1)[1].upper() == e_.upper():
                    lines.append(e.line())
                    found = True
            if found:
                continue
            try:
                # find by err code
                for e in errs__:
                    if e.code == int(e_, 0):
                        lines.append(e.line())
                        found = True
                if found:
                    continue
                # find by negated err code
                for e in errs__:
                    if e.code == -int(e_, 0):
                        lines.append(e.line())
                        found = True
                if found:
                    continue
            except ValueError:
                lines.append(('?', e_, 'Unknown err code'))

    # first find widths
    w = [0, 0]
    for l in lines:
        w[0] = max(w[0], len(l[0]))
        w[1] = max(w[1], len(l[1]))

    # then print results
    for l in lines:
        print('%-*s  %-*s  %s' % (
                w[0], l[0],
                w[1], l[1],
                l[2]))


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Decode littlefs error codes.",
            allow_abbrev=False)
    parser.add_argument(
            'errs',
            nargs='*',
            help="Error codes or error names to decode.")
    parser.add_argument(
            '-l', '--list',
            action='store_true',
            help="List all known error codes.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
