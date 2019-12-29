#!/usr/bin/env python3

import parsy as p
import re
import io
import sys

ASSERT_PATTERN = p.string('LFS_ASSERT') | p.string('assert')
ASSERT_CHARS = 'La'
ASSERT_TARGET = '__LFS_ASSERT_{TYPE}_{COMP}'
ASSERT_TESTS = {
    'int': """
        __typeof__({lh}) _lh = {lh};
        __typeof__({lh}) _rh = (__typeof__({lh})){rh};
        if (!(_lh {op} _rh)) {{
            printf("%s:%d:assert: "
                "assert failed with %"PRIiMAX", expected {comp} %"PRIiMAX"\\n",
                {file}, {line}, (intmax_t)_lh, (intmax_t)_rh);
            exit(-2);
        }}
    """,
    'str': """
        const char *_lh = {lh};
        const char *_rh = {rh};
        if (!(strcmp(_lh, _rh) {op} 0)) {{
            printf("%s:%d:assert: "
                "assert failed with \\\"%s\\\", expected {comp} \\\"%s\\\"\\n",
                {file}, {line}, _lh, _rh);
            exit(-2);
        }}
    """,
    'bool': """
        bool _lh = !!({lh});
        bool _rh = !!({rh});
        if (!(_lh {op} _rh)) {{
            printf("%s:%d:assert: "
                "assert failed with %s, expected {comp} %s\\n",
                {file}, {line}, _lh ? "true" : "false", _rh ? "true" : "false");
            exit(-2);
        }}
    """,
}

def mkassert(lh, rh='true', type='bool', comp='eq'):
    return ((ASSERT_TARGET + "({lh}, {rh}, __FILE__, __LINE__, __func__)")
        .format(
            type=type, TYPE=type.upper(),
            comp=comp, COMP=comp.upper(),
            lh=lh.strip(' '),
            rh=rh.strip(' ')))

def mkdecl(type, comp, op):
    return ((
        "#define "+ASSERT_TARGET+"(lh, rh, file, line, func)"
        "   do {{"+re.sub('\s+', ' ', ASSERT_TESTS[type])+"}} while (0)\n")
        .format(
            type=type, TYPE=type.upper(),
            comp=comp, COMP=comp.upper(),
            lh='lh', rh='rh', op=op,
            file='file', line='line', func='func'))

# add custom until combinator
def until(self, end):
    return end.should_fail('should fail').then(self).many()
p.Parser.until = until

pcomp = (
    p.string('==').tag('eq') |
    p.string('!=').tag('ne') |
    p.string('<=').tag('le') |
    p.string('>=').tag('ge') |
    p.string('<').tag('lt') |
    p.string('>').tag('gt'));

plogic = p.string('&&') | p.string('||')

@p.generate
def pstrassert():
    yield ASSERT_PATTERN + p.regex('\s*') + p.string('(') + p.regex('\s*')
    yield p.string('strcmp') + p.regex('\s*') + p.string('(') + p.regex('\s*')
    lh = yield pexpr.until(p.string(',') | p.string(')') | plogic)
    yield p.string(',') + p.regex('\s*')
    rh = yield pexpr.until(p.string(')') | plogic)
    yield p.string(')') + p.regex('\s*')
    op = yield pcomp
    yield p.regex('\s*') + p.string('0') + p.regex('\s*') + p.string(')')
    return mkassert(''.join(lh), ''.join(rh), 'str', op[0])

@p.generate
def pintassert():
    yield ASSERT_PATTERN + p.regex('\s*') + p.string('(') + p.regex('\s*')
    lh = yield pexpr.until(pcomp | p.string(')') | plogic)
    op = yield pcomp
    rh = yield pexpr.until(p.string(')') | plogic)
    yield p.string(')')
    return mkassert(''.join(lh), ''.join(rh), 'int', op[0])

@p.generate
def pboolassert():
    yield ASSERT_PATTERN + p.regex('\s*') + p.string('(') + p.regex('\s*')
    expr = yield pexpr.until(p.string(')'))
    yield p.string(')')
    return mkassert(''.join(expr), 'true', 'bool', 'eq')

passert = p.peek(ASSERT_PATTERN) >> (pstrassert | pintassert | pboolassert)

@p.generate
def pcomment1():
    yield p.string('//')
    s = yield p.regex('[^\\n]*')
    yield p.string('\n')
    return '//' + s + '\n'

@p.generate
def pcomment2():
    yield p.string('/*')
    s = yield p.regex('((?!\*/).)*')
    yield p.string('*/')
    return '/*' + ''.join(s) + '*/'

@p.generate
def pcomment3():
    yield p.string('#')
    s = yield p.regex('[^\\n]*')
    yield p.string('\n')
    return '#' + s + '\n'

pws = p.regex('\s+') | pcomment1 | pcomment2 | pcomment3

@p.generate
def pstring():
    q = yield p.regex('["\']')
    s = yield (p.string('\\%s' % q) | p.regex('[^%s]' % q)).many()
    yield p.string(q)
    return q + ''.join(s) + q

@p.generate
def pnested():
    l = yield p.string('(')
    n = yield pexpr.until(p.string(')'))
    r = yield p.string(')')
    return l + ''.join(n) + r

pexpr = (
    # shortcut for a bit better performance
    p.regex('[^%s/#\'"();{}=><,&|-]+' % ASSERT_CHARS) |
    pws |
    passert |
    pstring |
    pnested |
    p.string('->') |
    p.regex('.', re.DOTALL))

@p.generate
def pstmt():
    ws = yield pws.many()
    lh = yield pexpr.until(p.string('=>') | p.regex('[;{}]'))
    op = yield p.string('=>').optional()
    if op == '=>':
        rh = yield pstmt
        return ''.join(ws) + mkassert(''.join(lh), rh, 'int', 'eq')
    else:
        return ''.join(ws) + ''.join(lh)

@p.generate
def pstmts():
    a = yield pstmt
    b = yield (p.regex('[;{}]') + pstmt).many()
    return [a] + b

def main(args):
    inf = open(args.input, 'r') if args.input else sys.stdin
    outf = open(args.output, 'w') if args.output else sys.stdout

    # parse C code
    input = inf.read()
    stmts = pstmts.parse(input)

    # write extra verbose asserts
    outf.write("#include <stdbool.h>\n")
    outf.write("#include <stdint.h>\n")
    outf.write("#include <inttypes.h>\n")
    outf.write(mkdecl('int',  'eq', '=='))
    outf.write(mkdecl('int',  'ne', '!='))
    outf.write(mkdecl('int',  'lt', '<'))
    outf.write(mkdecl('int',  'gt', '>'))
    outf.write(mkdecl('int',  'le', '<='))
    outf.write(mkdecl('int',  'ge', '>='))
    outf.write(mkdecl('str',  'eq', '=='))
    outf.write(mkdecl('str',  'ne', '!='))
    outf.write(mkdecl('str',  'lt', '<'))
    outf.write(mkdecl('str',  'gt', '>'))
    outf.write(mkdecl('str',  'le', '<='))
    outf.write(mkdecl('str',  'ge', '>='))
    outf.write(mkdecl('bool', 'eq', '=='))
    if args.input:
        outf.write("#line %d \"%s\"\n" % (1, args.input))

    # write parsed statements
    for stmt in stmts:
        outf.write(stmt)

if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(
        description="Cpp step that increases assert verbosity")
    parser.add_argument('input', nargs='?',
        help="Input C file after cpp.")
    parser.add_argument('-o', '--output',
        help="Output C file.")
    main(parser.parse_args())
