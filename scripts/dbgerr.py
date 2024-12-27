#!/usr/bin/env python3

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)


ERRS = [
    ('OK',              0,      "No error"                          ),
    ('UNKNOWN',         -1,     "Unknown error"                     ),
    ('INVAL',           -22,    "Invalid parameter"                 ),
    ('NOTSUP',          -95,    "Operation not supported"           ),
    ('IO',              -5,     "Error during device operation"     ),
    ('CORRUPT',         -84,    "Corrupted"                         ),
    ('NOENT',           -2,     "No directory entry"                ),
    ('EXIST',           -17,    "Entry already exists"              ),
    ('NOTDIR',          -20,    "Entry is not a dir"                ),
    ('ISDIR',           -21,    "Entry is a dir"                    ),
    ('NOTEMPTY',        -39,    "Dir is not empty"                  ),
    ('FBIG',            -27,    "File too large"                    ),
    ('NOSPC',           -28,    "No space left on device"           ),
    ('NOMEM',           -12,    "No more memory available"          ),
    ('NOATTR',          -61,    "No data/attr available"            ),
    ('NAMETOOLONG',     -36,    "File name too long"                ),
    ('RANGE',           -34,    "Result out of range"               ),
]


def main(errs, *,
        list=False):
    list_, list = list, __builtins__.list

    # list all known error codes
    if list_:
        # first find the widths
        w = [0, 0]
        for n, e, h in ERRS:
            w[0] = max(w[0], len('LFS_ERR_')+len(n))
            w[1] = max(w[1], len(str(e)))

        # print
        for n, e, h in ERRS:
            print('%-*s  %-*s  %s' % (
                    w[0], 'LFS_ERR_'+n,
                    w[1], e,
                    h))

    # find these errors
    else:
        def find_err(err):
            # find by LFS_ERR_+name
            for n, e, h in ERRS:
                if 'LFS_ERR_'+n == err:
                    return n, e, h
            # find by name
            for n, e, h in ERRS:
                if n == err:
                    return n, e, h
            # find by E+name
            for n, e, h in ERRS:
                if 'E'+n == err:
                    return n, e, h
            try:
                # find by err code
                for n, e, h in ERRS:
                    if e == int(err, 0):
                        return n, e, h
                # find by negated err code
                for n, e, h in ERRS:
                    if e == -int(err, 0):
                        return n, e, h
            except ValueError:
                pass
            # not found
            raise KeyError(err)

        for err in errs:
            try:
                n, e, h = find_err(err)
                print('%s %s %s' % ('LFS_ERR_'+n, e, h))
            except KeyError:
                print('%s ?' % err)


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
