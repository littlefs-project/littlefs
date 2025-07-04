#
# Hooks for gdb:
# (gdb) source ./scripts/dbg.gdb.py
#
#

import shlex


# split spaces but only outside of parens and quotes
def gdbsplit(v):
    parens = 0
    quote = None
    escape = False
    i_ = 0
    for i in range(len(v)):
        if v[i].isspace() and not parens and not quote:
            v_ = v[i_:i].strip()
            if v_:
                yield v_
            i_ = i+1
        elif quote:
            if escape:
                escape = False
            elif v[i] == quote:
                quote = None
            elif v[i] == '\\':
                escape = True
        elif v[i] in '\'"':
            quote = v[i]
        elif v[i] in '([{':
            parens += 1
        elif v[i] in '}])':
            parens -= 1
    v_ = v[i_:].strip()
    if v_:
        yield v_

# common wrapper for dbg scripts
#
# Note some tricks to help interact with bash and gdb:
#
# - Flags are passed as is (-b4096, -t, --trunk)
# - All non-flags are parsed as expressions (file->b.shrub.blocks[0])
# - String expressions may be useful for paths and stuff ("./disk")
#
class DbgCommand(gdb.Command):
    """A littlefs debug script. See -h/--help for more info."""
    name = None
    path = None

    def __init__(self):
        super().__init__(self.name,
                gdb.COMMAND_DATA,
                gdb.COMPLETE_EXPRESSION)

    def invoke(self, args, *_):
        # parse args
        args = list(gdbsplit(args))
        args_ = []
        for a in args:
            # pass flags as is
            if a.startswith('-'):
                args_.append(a)

            # parse and eval
            else:
                try:
                    v = gdb.parse_and_eval(a)
                    t = v.type.strip_typedefs()
                    if t.code in {
                            gdb.TYPE_CODE_ENUM,
                            gdb.TYPE_CODE_FLAGS,
                            gdb.TYPE_CODE_INT,
                            gdb.TYPE_CODE_RANGE,
                            gdb.TYPE_CODE_CHAR,
                            gdb.TYPE_CODE_BOOL}:
                        v = str(int(v))
                    elif t.code in {
                            gdb.TYPE_CODE_FLT}:
                        v = str(float(v))
                    else:
                        try:
                            v = v.string('utf8')
                        except gdb.error:
                            raise gdb.GdbError('Unexpected type: %s' % v.type)
                except gdb.error as e:
                    raise gdb.GdbError(e)

                args_.append(shlex.quote(v))
        args = args_

        # execute
        gdb.execute(' '.join(['!'+self.path, *args]))


# at some point this was manual, then I realized I could just glob all
# scripts with this prefix
#
# # dbgerr
# class DbgErr(DbgCommand):
#     name = 'dbgerr'
#     path = './scripts/dbgerr.py'
#
# # dbgflags
# class DbgFlags(DbgCommand):
#     name = 'dbgflags'
#     path = './scripts/dbgflags.py'
#
# # dbgtag
# class DbgTag(DbgCommand):
#     name = 'dbgtag'
#     path = './scripts/dbgtag.py'

import os
import glob

for path in glob.glob(os.path.join(
        os.path.dirname(__file__),
        'dbg*.py')):
    if path == __file__:
        continue

    # create dbg class
    name = os.path.splitext(os.path.basename(path))[0]
    type(name, (DbgCommand,), {
        'name': name,
        'path': path
    })


# initialize gdb hooks
for Dbg in DbgCommand.__subclasses__():
    if Dbg.__doc__ is None:
        Dbg.__doc__ = DbgCommand.__doc__
    Dbg()
