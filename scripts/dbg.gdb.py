#
# hooks for gdb:
# (gdb) source ./scripts/dbg.gdb.py
#
#


# dbgerr
class DbgErr(gdb.Command):
    """Decode littlefs error codes. See -h/--help for more info."""

    def __init__(self):
        super().__init__("dbgerr",
                gdb.COMMAND_DATA,
                gdb.COMPLETE_EXPRESSION)

    def invoke(self, args, *_):
        args = args.split()
        # find nonflags
        nonflags = []
        for i, a in enumerate(args):
            if not a.startswith('-'):
                nonflags.append(i)
        # parse and eval
        for i, n in enumerate(nonflags):
            try:
                args[n] = '%d' % gdb.parse_and_eval(args[n])
            except gdb.error as e:
                raise gdb.GdbError(e)

        # execute
        gdb.execute(' '.join(['!./scripts/dbgerr.py'] + args))

DbgErr()


# dbgflags
class DbgFlags(gdb.Command):
    """Decode littlefs flags. See -h/--help for more info."""

    def __init__(self):
        super().__init__("dbgflags",
                gdb.COMMAND_DATA,
                gdb.COMPLETE_EXPRESSION)

    def invoke(self, args, *_):
        args = args.split()
        # hack, but don't eval if -l or --list specified
        if '-l' not in args and '--list' not in args:
            # find nonflags
            nonflags = []
            for i, a in enumerate(args):
                if not a.startswith('-'):
                    nonflags.append(i)
            # parse and eval
            for i, n in enumerate(nonflags):
                # dbgflags is special in that first arg may be prefix
                if i > 0 or len(nonflags) <= 1:
                    try:
                        args[n] = '%d' % gdb.parse_and_eval(args[n])
                    except gdb.error as e:
                        raise gdb.GdbError(e)

        # execute
        gdb.execute(' '.join(['!./scripts/dbgflags.py'] + args))

DbgFlags()


# dbgtag
class DbgTag(gdb.Command):
    """Decode littlefs tags. See -h/--help for more info."""

    def __init__(self):
        super().__init__("dbgtag",
                gdb.COMMAND_DATA,
                gdb.COMPLETE_EXPRESSION)

    def invoke(self, args, *_):
        args = args.split()
        # find nonflags
        nonflags = []
        for i, a in enumerate(args):
            if not a.startswith('-'):
                nonflags.append(i)
        # parse and eval
        for i, n in enumerate(nonflags):
            try:
                args[n] = '%d' % gdb.parse_and_eval(args[n])
            except gdb.error as e:
                raise gdb.GdbError(e)

        # execute
        gdb.execute(' '.join(['!./scripts/dbgtag.py'] + args))

DbgTag()

