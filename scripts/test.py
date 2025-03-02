#!/usr/bin/env python3
#
# Script to compile and runs tests.
#
# Example:
# ./scripts/test.py runners/test_runner -b
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import csv
import errno
import fnmatch
import itertools as it
import functools as ft
import math as mt
import os
import pty
import re
import shlex
import shutil
import signal
import subprocess as sp
import sys
import threading as th
import time

try:
    import tomllib as toml
except ModuleNotFoundError:
    import tomli as toml


RUNNER_PATH = ['./runners/test_runner']
HEADER_PATH = 'runners/test_runner.h'

GDB_PATH = ['gdb']
VALGRIND_PATH = ['valgrind']
PERF_SCRIPT = ['./scripts/perf.py']


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

# a define range
class DRange:
    def __init__(self, start, stop=None, step=None):
        if stop is None:
            start, stop = None, start
        self.start = start if start is not None else 0
        self.stop = stop
        self.step = step if step is not None else 1

    def __len__(self):
        if self.step > 0:
            return (self.stop-1 - self.start) // self.step + 1
        else:
            return (self.start-1 - self.stop) // -self.step + 1

    def next(self, i):
        return '(%s)*%d + %d' % (i, self.step, self.start)


class TestCase:
    # create a TestCase object from a config
    def __init__(self, config, args={}):
        self.name = config.pop('name')
        self.path = config.pop('path')
        self.suite = config.pop('suite')
        self.lineno = config.pop('lineno', None)
        self.if_ = config.pop('if', [])
        if not isinstance(self.if_, list):
            self.if_ = [self.if_]
        self.ifdef = config.pop('ifdef', [])
        if not isinstance(self.ifdef, list):
            self.ifdef = [self.ifdef]
        self.code = config.pop('code')
        self.code_lineno = config.pop('code_lineno', None)
        self.in_ = config.pop('in',
                config.pop('suite_in', None))
        self.fuzz_ = config.pop('fuzz',
                config.pop('suite_fuzz', None))

        self.internal = bool(self.in_)
        self.reentrant = config.pop('reentrant',
                config.pop('suite_reentrant', False))
        self.fuzz = bool(self.fuzz_)

        # figure out defines and build possible permutations
        self.defines = set()
        self.permutations = []

        # defines can be a dict or a list or dicts
        suite_defines = config.pop('suite_defines', {})
        if not isinstance(suite_defines, list):
            suite_defines = [suite_defines]
        defines = config.pop('defines', {})
        if not isinstance(defines, list):
            defines = [defines]

        def csplit(v):
            # split commas but only outside of parens
            parens = 0
            i_ = 0
            for i in range(len(v)):
                if v[i] == ',' and parens == 0:
                    yield v[i_:i]
                    i_ = i+1
                elif v[i] in '([{':
                    parens += 1
                elif v[i] in '}])':
                    parens -= 1
            if v[i_:].strip():
                yield v[i_:]

        def parse_define(v):
            # a define entry can be a list
            if isinstance(v, list):
                return sum((parse_define(v_) for v_ in v), [])
            # or a string
            elif isinstance(v, str):
                # which can be comma-separated values, with optional
                # range statements. This matches the runtime define parser in
                # the runner itself.
                vs = []
                for v_ in csplit(v):
                    m = re.match(r'^\s*range\s*\((.*)\)\s*$', v_)
                    if m:
                        vs.append(DRange(*[
                                int(a, 0) for a in csplit(m.group(1))]))
                    else:
                        vs.append(v_)
                return vs
            # or a literal value
            elif isinstance(v, bool):
                return ['true' if v else 'false']
            else:
                return [v]

        # build possible permutations
        for suite_defines_ in suite_defines:
            self.defines |= suite_defines_.keys()
            for defines_ in defines:
                self.defines |= defines_.keys()
                self.permutations.append({
                        k: parse_define(v)
                            for k, v in (suite_defines_ | defines_).items()})

        for k in config.keys():
            print('%swarning:%s in %s, found unused key %r' % (
                    '\x1b[01;33m' if args['color'] else '',
                    '\x1b[m' if args['color'] else '',
                    self.name,
                    k),
                file=sys.stderr)

    def __repr__(self):
        return '<TestCase %s>' % self.name

    def __lt__(self, other):
        # sort by suite, lineno, and name
        return ((self.suite, self.lineno, self.name)
                < (other.suite, other.lineno, other.name))

    def isin(self, path):
        return (self.in_ is not None
                and os.path.normpath(self.in_)
                    == os.path.normpath(path))


class TestSuite:
    # create a TestSuite object from a toml file
    def __init__(self, path, args={}):
        self.path = path
        self.name = os.path.basename(path)
        if self.name.endswith('.toml'):
            self.name = self.name[:-len('.toml')]

        # load toml file and parse test cases
        with open(self.path) as f:
            # load tests
            config = toml.load(f.buffer)

            # find line numbers
            f.seek(0)
            case_linenos = []
            code_linenos = []
            for i, line in enumerate(f):
                match = re.match(
                        '(?P<case>\[\s*cases\s*\.\s*(?P<name>\w+)\s*\])'
                            '|' '(?P<code>code\s*=)',
                        line)
                if match and match.group('case'):
                    case_linenos.append((i+1, match.group('name')))
                elif match and match.group('code'):
                    code_linenos.append(i+2)

            # sort in case toml parsing did not retain order
            case_linenos.sort()

            cases = config.pop('cases', {})
            for (lineno, name), (nlineno, _) in it.zip_longest(
                    case_linenos, case_linenos[1:],
                    fillvalue=(float('inf'), None)):
                code_lineno = min(
                        (l for l in code_linenos
                            if l >= lineno and l < nlineno),
                        default=None)
                cases[name]['lineno'] = lineno
                cases[name]['code_lineno'] = code_lineno

            self.if_ = config.pop('if', [])
            if not isinstance(self.if_, list):
                self.if_ = [self.if_]

            self.ifdef = config.pop('ifdef', [])
            if not isinstance(self.ifdef, list):
                self.ifdef = [self.ifdef]

            self.code = config.pop('code', None)
            self.code_lineno = min(
                    (l for l in code_linenos
                        if not case_linenos or l < case_linenos[0][0]),
                    default=None)
            self.in_ = config.pop('in', None)
            self.fuzz_ = config.pop('fuzz', None)

            self.after = config.pop('after', [])
            if not isinstance(self.after, list):
                self.after = [self.after]

            # a couple of these we just forward to all cases
            defines = config.pop('defines', {})
            reentrant = config.pop('reentrant', False)

            self.cases = []
            for name, case in cases.items():
                self.cases.append(TestCase(
                        config={
                            'name': name,
                            'path': path + (':%d' % case['lineno']
                                if 'lineno' in case else ''),
                            'suite': self.name,
                            'suite_defines': defines,
                            'suite_in': self.in_,
                            'suite_reentrant': reentrant,
                            'suite_fuzz': self.fuzz_,
                            **case},
                        args=args))

            # sort for consistency
            self.cases.sort()

            # combine per-case defines
            self.defines = set.union(set(), *(
                    set(case.defines) for case in self.cases))

            # combine other per-case things
            self.internal = any(case.internal for case in self.cases)
            self.reentrant = any(case.reentrant for case in self.cases)
            self.fuzz = any(case.fuzz for case in self.cases)

        for k in config.keys():
            print('%swarning:%s in %s, found unused key %r' % (
                        '\x1b[01;33m' if args['color'] else '',
                        '\x1b[m' if args['color'] else '',
                        self.name,
                        k),
                    file=sys.stderr)

    def __repr__(self):
        return '<TestSuite %s>' % self.name

    def __lt__(self, other):
        # sort by name
        #
        # note we override this with a topological sort during compilation
        return self.name < other.name

    def isin(self, path):
        return (self.in_ is not None
                and os.path.normpath(self.in_)
                    == os.path.normpath(path))


def compile(test_paths, **args):
    # load the suites
    suites = [TestSuite(path, args) for path in test_paths]

    # sort suites by:
    # 1. topologically by "after" dependencies
    # 2. lexicographically for consistency
    pending = co.OrderedDict((suite.name, suite)
            for suite in sorted(suites))
    suites = []
    while pending:
        pending_ = co.OrderedDict()
        for suite in pending.values():
            if not any(after in pending for after in suite.after):
                suites.append(suite)
            else:
                pending_[suite.name] = suite

        if len(pending_) == len(pending):
            print('%serror:%s cycle detected in suite ordering: {%s}' % (
                        '\x1b[01;31m' if args['color'] else '',
                        '\x1b[m' if args['color'] else '',
                        ', '.join(suite.name for suite in pending.values())),
                    file=sys.stderr)
            sys.exit(-1)

        pending = pending_

    # check for name conflicts, these will cause ambiguity problems later
    # when running tests
    seen = {}
    for suite in suites:
        if suite.name in seen:
            print('%swarning:%s conflicting suite %r, %s and %s' % (
                        '\x1b[01;33m' if args['color'] else '',
                        '\x1b[m' if args['color'] else '',
                        suite.name,
                        suite.path,
                        seen[suite.name].path),
                    file=sys.stderr)
        seen[suite.name] = suite

        for case in suite.cases:
            # only allow conflicts if a case and its suite share a name
            if case.name in seen and not (
                    isinstance(seen[case.name], TestSuite)
                        and seen[case.name].cases == [case]):
                print('%swarning:%s conflicting case %r, %s and %s' % (
                            '\x1b[01;33m' if args['color'] else '',
                            '\x1b[m' if args['color'] else '',
                            case.name,
                            case.path,
                            seen[case.name].path),
                        file=sys.stderr)
            seen[case.name] = case

    # we can only compile one test suite at a time
    if not args.get('source'):
        if len(suites) > 1:
            print('%serror:%s compiling more than one test suite? (%r)' % (
                        '\x1b[01;31m' if args['color'] else '',
                        '\x1b[m' if args['color'] else '',
                        test_paths),
                    file=sys.stderr)
            sys.exit(-1)

        suite = suites[0]

    # write generated test source
    if 'output' in args:
        with openio(args['output'], 'w') as f:
            _write = f.write
            def write(s):
                f.lineno += s.count('\n')
                _write(s)
            def writeln(s=''):
                f.lineno += s.count('\n') + 1
                _write(s)
                _write('\n')
            f.lineno = 1
            f.write = write
            f.writeln = writeln

            f.writeln("// Generated by %s:" % sys.argv[0])
            f.writeln("//")
            f.writeln("// %s" % ' '.join(sys.argv))
            f.writeln("//")
            f.writeln()

            # include test_runner.h in every generated file
            f.writeln("#include \"%s\"" % args['include'])
            f.writeln()

            # write out generated functions, this can end up in different
            # files depending on the "in" attribute
            #
            # note it's up to the specific generated file to declare
            # the test defines
            def write_case_functions(f, suite, case):
                    # write any ifdef prologues
                    if case.ifdef:
                        for ifdef in case.ifdef:
                            f.writeln('#ifdef %s' % ifdef)
                        f.writeln()

                    # create case define functions
                    for i, permutation in enumerate(case.permutations):
                        for k, vs in sorted(permutation.items()):
                            f.writeln('intmax_t __test__%s__%s__%d('
                                    '__attribute__((unused)) void *data, '
                                    'size_t i) {' % (
                                        case.name, k, i))
                            j = 0
                            for v in vs:
                                # generate range
                                if isinstance(v, DRange):
                                    f.writeln(4*' '+'if (i < %d) '
                                            'return %s;' % (
                                                j+len(v), v.next('i-%d' % j)))
                                    j += len(v)
                                # translate index to define
                                else:
                                    f.writeln(4*' '+'if (i == %d) '
                                            'return %s;' % (
                                                j, v))
                                    j += 1

                            f.writeln(4*' '+'__builtin_unreachable();')
                            f.writeln('}')
                            f.writeln()

                    # create case if function
                    if suite.if_ or case.if_:
                        f.writeln('bool __test__%s__if(void) {' % (
                                case.name))
                        for if_ in it.chain(suite.if_, case.if_):
                            f.writeln(4*' '+'if (!(%s)) return false;' % (
                                    'true' if if_ is True
                                        else 'false' if if_ is False
                                        else if_))
                        f.writeln(4*' '+'return true;')
                        f.writeln('}')
                        f.writeln()

                    # create case run function
                    f.writeln('void __test__%s__run('
                            '__attribute__((unused)) '
                            'struct lfs_config *CFG) {' % (
                                case.name))
                    f.writeln(4*' '+'// test case %s' % case.name)
                    if case.code_lineno is not None:
                        f.writeln(4*' '+'#line %d "%s"' % (
                                case.code_lineno, suite.path))
                    f.write(case.code)
                    if case.code_lineno is not None:
                        f.writeln(4*' '+'#line %d "%s"' % (
                                f.lineno+1, args['output']))
                    f.writeln('}')
                    f.writeln()

                    # write any ifdef epilogues
                    if case.ifdef:
                        for ifdef in case.ifdef:
                            f.writeln('#endif')
                        f.writeln()

            if not args.get('source'):
                # write any ifdef prologues
                if suite.ifdef:
                    for ifdef in suite.ifdef:
                        f.writeln('#ifdef %s' % ifdef)
                    f.writeln()

                # write any suite defines
                if suite.defines:
                    for define in sorted(suite.defines):
                        f.writeln('__attribute__((weak)) intmax_t %s;' % (
                                define))
                    f.writeln()

                # write any suite code
                if suite.code is not None and suite.in_ is None:
                    if suite.code_lineno is not None:
                        f.writeln('#line %d "%s"' % (
                                suite.code_lineno, suite.path))
                    f.write(suite.code)
                    if suite.code_lineno is not None:
                        f.writeln('#line %d "%s"' % (
                                f.lineno+1, args['output']))
                    f.writeln()

                # create case functions
                for case in suite.cases:
                    if case.in_ is None:
                        write_case_functions(f, suite, case)
                    else:
                        for i, permutation in enumerate(case.permutations):
                            for k, vs in sorted(permutation.items()):
                                f.writeln('extern intmax_t __test__%s__%s__%d('
                                        'void *data, size_t i);' % (
                                            case.name, k, i))
                        if suite.if_ or case.if_:
                            f.writeln('extern bool __test__%s__if('
                                    'void);' % (
                                        case.name))
                        f.writeln('extern void __test__%s__run('
                                'struct lfs_config *CFG);' % (
                                    case.name))
                        f.writeln()

                # write any ifdef epilogues
                if suite.ifdef:
                    for ifdef in suite.ifdef:
                        f.writeln('#endif')
                    f.writeln()

                # create suite struct
                f.writeln('const struct test_suite __test__%s__suite = {' % (
                        suite.name))
                f.writeln(4*' '+'.name = "%s",' % suite.name)
                f.writeln(4*' '+'.path = "%s",' % suite.path)
                f.writeln(4*' '+'.flags = %s,' % (
                        ' | '.join(filter(None, [
                                'TEST_INTERNAL' if suite.internal else None,
                                'TEST_REENTRANT' if suite.reentrant else None,
                                'TEST_FUZZ' if suite.fuzz else None]))
                            or 0))
                for ifdef in suite.ifdef:
                    f.writeln(4*' '+'#ifdef %s' % ifdef)
                # create suite defines
                if suite.defines:
                    f.writeln(4*' '+'.defines = (const test_define_t[]){')
                    for k in sorted(suite.defines):
                        f.writeln(8*' '+'{"%s", &%s, NULL, NULL, 0},' % (
                                k, k))
                    f.writeln(4*' '+'},')
                    f.writeln(4*' '+'.define_count = %d,' % len(suite.defines))
                for ifdef in suite.ifdef:
                    f.writeln(4*' '+'#endif')
                if suite.cases:
                    f.writeln(4*' '+'.cases = (const struct test_case[]){')
                    for case in suite.cases:
                        # create case structs
                        f.writeln(8*' '+'{')
                        f.writeln(12*' '+'.name = "%s",' % case.name)
                        f.writeln(12*' '+'.path = "%s",' % case.path)
                        f.writeln(12*' '+'.flags = %s,' % (
                                ' | '.join(filter(None, [
                                        'TEST_INTERNAL' if case.internal
                                            else None,
                                        'TEST_REENTRANT' if case.reentrant
                                            else None,
                                        'TEST_FUZZ' if case.fuzz
                                            else None]))
                                    or 0))
                        for ifdef in it.chain(suite.ifdef, case.ifdef):
                            f.writeln(12*' '+'#ifdef %s' % ifdef)
                        # create case defines
                        if case.defines:
                            f.writeln(12*' '+'.defines'
                                    ' = (const test_define_t*)'
                                    '(const test_define_t[][%d]){' % (
                                        len(suite.defines)))
                            for i, permutation in enumerate(case.permutations):
                                f.writeln(16*' '+'{')
                                for k, vs in sorted(permutation.items()):
                                    f.writeln(20*' '+'[%d] = {'
                                            '"%s", &%s, '
                                            '__test__%s__%s__%d, '
                                            'NULL, %d},' % (
                                                sorted(suite.defines).index(k),
                                                k, k, case.name, k, i,
                                                sum(len(v)
                                                        if isinstance(
                                                            v, DRange)
                                                        else 1
                                                    for v in vs)))
                                f.writeln(16*' '+'},')
                            f.writeln(12*' '+'},')
                            f.writeln(12*' '+'.permutations = %d,' % (
                                    len(case.permutations)))
                        if suite.if_ or case.if_:
                            f.writeln(12*' '+'.if_ = __test__%s__if,' % (
                                    case.name))
                        f.writeln(12*' '+'.run = __test__%s__run,' % (
                                case.name))
                        for ifdef in it.chain(suite.ifdef, case.ifdef):
                            f.writeln(12*' '+'#endif')
                        f.writeln(8*' '+'},')
                    f.writeln(4*' '+'},')
                f.writeln(4*' '+'.case_count = %d,' % len(suite.cases))
                f.writeln('};')
                f.writeln()

            else:
                # copy source
                f.writeln('#line 1 "%s"' % args['source'])
                with open(args['source']) as sf:
                    shutil.copyfileobj(sf, f)
                f.writeln()

                # merge all defines we need, otherwise we will run into
                # redefinition errors
                defines = ({define
                            for suite in suites
                            if suite.isin(args['source'])
                            for define in suite.defines}
                        | {define
                            for suite in suites
                            for case in suite.cases
                            if case.isin(args['source'])
                            for define in case.defines})
                if defines:
                    for define in sorted(defines):
                        f.writeln('__attribute__((weak)) intmax_t %s;' % (
                                define))
                    f.writeln()

                # write any internal tests
                for suite in suites:
                    # any ifdef prologues
                    if suite.ifdef:
                        for ifdef in suite.ifdef:
                            f.writeln('#ifdef %s' % ifdef)
                        f.writeln()

                    # any suite code
                    if suite.isin(args['source']):
                        if suite.code_lineno is not None:
                            f.writeln('#line %d "%s"' % (
                                    suite.code_lineno, suite.path))
                        f.write(suite.code)
                        if suite.code_lineno is not None:
                            f.writeln('#line %d "%s"' % (
                                    f.lineno+1, args['output']))
                        f.writeln()

                    # any case functions
                    for case in suite.cases:
                        if case.isin(args['source']):
                            write_case_functions(f, suite, case)

                    # any ifdef epilogues
                    if suite.ifdef:
                        for ifdef in suite.ifdef:
                            f.writeln('#endif')
                        f.writeln()

                # declare our test suites
                #
                # by declaring these as weak we can write these to every
                # source file without issue, eventually one of these copies
                # will be linked
                for suite in suites:
                    f.writeln('extern const struct test_suite '
                            '__test__%s__suite;' % (
                                suite.name))
                f.writeln()

                f.writeln('__attribute__((weak))')
                f.writeln('const struct test_suite *const test_suites[] = {')
                for suite in suites:
                    f.writeln(4*' '+'&__test__%s__suite,' % suite.name)
                if len(suites) == 0:
                    f.writeln(4*' '+'0,')
                f.writeln('};')
                f.writeln('__attribute__((weak))')
                f.writeln('const size_t test_suite_count = %d;' % len(suites))
                f.writeln()


def find_runner(runner, id=None, main=True, **args):
    cmd = runner.copy()

    # run under some external command?
    if args.get('exec'):
        cmd[:0] = args['exec']

    # run under valgrind?
    if args.get('valgrind'):
        cmd[:0] = args['valgrind_path'] + [
                '--leak-check=full',
                '--track-origins=yes',
                '--error-exitcode=4',
                '-q']

    # run under perf?
    if args.get('perf'):
        cmd[:0] = args['perf_script'] + list(filter(None, [
                '--record',
                '--perf-freq=%s' % args['perf_freq']
                    if args.get('perf_freq') else None,
                '--perf-period=%s' % args['perf_period']
                    if args.get('perf_period') else None,
                '--perf-events=%s' % args['perf_events']
                    if args.get('perf_events') else None,
                '--perf-path=%s' % args['perf_path']
                    if args.get('perf_path') else None,
                '-o%s' % args['perf']]))

    # other context
    if args.get('define_depth'):
        cmd.append('--define-depth=%s' % args['define_depth'])
    if args.get('powerloss'):
        cmd.append('-P%s' % args['powerloss'])
    if args.get('all'):
        cmd.append('-a')

    # only one thread should write to disk/trace, otherwise the output
    # ends up clobbered and useless
    if main:
        if args.get('disk'):
            cmd.append('-d%s' % args['disk'])
        if args.get('trace'):
            cmd.append('-t%s' % args['trace'])
        if args.get('trace_backtrace'):
            cmd.append('--trace-backtrace')
        if args.get('trace_period'):
            cmd.append('--trace-period=%s' % args['trace_period'])
        if args.get('trace_freq'):
            cmd.append('--trace-freq=%s' % args['trace_freq'])
        if args.get('read_sleep'):
            cmd.append('--read-sleep=%s' % args['read_sleep'])
        if args.get('prog_sleep'):
            cmd.append('--prog-sleep=%s' % args['prog_sleep'])
        if args.get('erase_sleep'):
            cmd.append('--erase-sleep=%s' % args['erase_sleep'])

    # defines?
    if args.get('define') and id is None:
        for define in args.get('define'):
            cmd.append('-D%s' % define)

    # test id?
    #
    # note we disable defines above when id is explicit, defines override id
    # in the test runner, which is not what we want when querying an explicit
    # test id
    if id is not None:
        cmd.append(id)

    return cmd

def find_perms(runner, test_ids=[], **args):
    runner_ = find_runner(runner, main=False, **args)
    case_suites = {}
    expected_case_perms = co.OrderedDict()
    expected_perms = 0
    total_perms = 0

    # query cases from the runner
    cmd = runner_ + ['--list-cases'] + test_ids
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    pattern = re.compile(
            '^(?P<case>[^\s]+)'
                '\s+(?P<flags>[^\s]+)'
                '\s+(?P<filtered>\d+)/(?P<perms>\d+)')
    # skip the first line
    for line in it.islice(proc.stdout, 1, None):
        m = pattern.match(line)
        if m:
            filtered = int(m.group('filtered'))
            perms = int(m.group('perms'))
            expected_case_perms[m.group('case')] = (
                    expected_case_perms.get(m.group('case'), 0)
                        + filtered)
            expected_perms += filtered
            total_perms += perms
    proc.wait()
    if proc.returncode != 0:
        sys.exit(-1)

    # get which suite each case belongs to via paths
    cmd = runner_ + ['--list-case-paths'] + test_ids
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    pattern = re.compile(
            '^(?P<case>[^\s]+)'
                '\s+(?P<path>[^:]+):(?P<lineno>\d+)')
    # skip the first line
    for line in it.islice(proc.stdout, 1, None):
        m = pattern.match(line)
        if m:
            path = m.group('path')
            # strip path/suffix here
            suite = os.path.basename(path)
            if suite.endswith('.toml'):
                suite = suite[:-len('.toml')]
            case_suites[m.group('case')] = suite
    proc.wait()
    if proc.returncode != 0:
        sys.exit(-1)

    # figure out expected suite perms
    expected_suite_perms = co.OrderedDict()
    for case, suite in case_suites.items():
        expected_suite_perms[suite] = (
                expected_suite_perms.get(suite, 0)
                    + expected_case_perms.get(case, 0))

    return (case_suites,
            expected_suite_perms,
            expected_case_perms,
            expected_perms,
            total_perms)

def find_path(runner, id, **args):
    runner_ = find_runner(runner, id, main=False, **args)
    path = None
    # query from runner
    cmd = runner_ + ['--list-case-paths', id]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    pattern = re.compile(
            '^(?P<case>[^\s]+)'
                '\s+(?P<path>[^:]+):(?P<lineno>\d+)')
    # skip the first line
    for line in it.islice(proc.stdout, 1, None):
        m = pattern.match(line)
        if m and path is None:
            path_ = m.group('path')
            lineno = int(m.group('lineno'))
            path = (path_, lineno)
    proc.wait()
    if proc.returncode != 0:
        sys.exit(-1)

    return path

def find_defines(runner, id, **args):
    runner_ = find_runner(runner, id, main=False, **args)
    # query permutation defines from runner
    cmd = runner_ + ['--list-permutation-defines', id]
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
            stdout=sp.PIPE,
            universal_newlines=True,
            errors='replace',
            close_fds=False)
    defines = co.OrderedDict()
    pattern = re.compile('^(?P<define>\w+)=(?P<value>.+)')
    for line in proc.stdout:
        m = pattern.match(line)
        if m:
            define = m.group('define')
            value = m.group('value')
            defines[define] = value
    proc.wait()
    if proc.returncode != 0:
        sys.exit(-1)

    return defines

def find_ids(runner, test_ids=[], **args):
    # no ids => all ids, we don't need an extra lookup if no special
    # behavior is requested
    if not (args.get('by_cases')
            or args.get('by_suites')
            or test_ids):
        return []

    # lookup suites/cases
    (suite_cases,
            expected_suite_perms,
            expected_case_perms,
            _,
            _) = find_perms(runner, **args)

    # no ids => all ids, before we evaluate globs
    if not test_ids and args.get('by_cases'):
        return [case_ for case_ in expected_case_perms.keys()]
    if not test_ids and args.get('by_suites'):
        return [suite for suite in expected_suite_perms.keys()]

    # find suite/case by id
    test_ids_ = []
    for id in test_ids:
        # strip permutation
        name, *_ = id.split(':', 1)
        test_ids__ = []
        # resolve globs
        if '*' in name:
            test_ids__.extend(suite
                    for suite in expected_suite_perms.keys()
                    if fnmatch.fnmatchcase(suite, name))
            if not test_ids__:
                test_ids__.extend(case_
                        for case_ in expected_case_perms.keys()
                        if fnmatch.fnmatchcase(case_, name))
        # literal suite
        elif name in expected_suite_perms:
            test_ids__.append(id)
        # literal case
        elif name in expected_case_perms:
            test_ids__.append(id)

        # no suite/case found? error
        if not test_ids__:
            print('%serror:%s no tests match id %r?' % (
                        '\x1b[01;31m' if args['color'] else '',
                        '\x1b[m' if args['color'] else '',
                        id),
                    file=sys.stderr)
            sys.exit(-1)

        test_ids_.extend(test_ids__)
    test_ids = test_ids_

    # expand suites to cases?
    if args.get('by_cases'):
        test_ids_ = []
        for id in test_ids:
            if id in expected_suite_perms:
                for case_, suite in suite_cases.items():
                    if suite == id:
                        test_ids_.append(case_)
            else:
                test_ids_.append(id)
        test_ids = test_ids_

    # no test ids found? return a garbage id for consistency
    return test_ids if test_ids else ['?']


def list_(runner, test_ids=[], **args):
    cmd = find_runner(runner, main=False, **args)
    cmd.extend(find_ids(runner, test_ids, **args))

    if args.get('summary'):          cmd.append('--summary')
    if args.get('list_suites'):      cmd.append('--list-suites')
    if args.get('list_cases'):       cmd.append('--list-cases')
    if args.get('list_suite_paths'): cmd.append('--list-suite-paths')
    if args.get('list_case_paths'):  cmd.append('--list-case-paths')
    if args.get('list_defines'):     cmd.append('--list-defines')
    if args.get('list_permutation_defines'):
                                     cmd.append('--list-permutation-defines')
    if args.get('list_implicit_defines'):
                                     cmd.append('--list-implicit-defines')
    if args.get('list_powerlosses'): cmd.append('--list-powerlosses')

    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    return sp.call(cmd)



# Thread-safe CSV writer
class TestOutput:
    def __init__(self, path, head=None, tail=None):
        self.f = openio(path, 'w+', 1)
        self.lock = th.Lock()
        self.head = head or []
        self.tail = tail or []
        self.writer = csv.DictWriter(self.f, self.head + self.tail)
        self.rows = []

    def close(self):
        self.f.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.f.close()

    def writerow(self, row):
        with self.lock:
            self.rows.append(row)
            if all(k in self.head or k in self.tail for k in row.keys()):
                # can simply append
                self.writer.writerow(row)
            else:
                # need to rewrite the file
                self.head.extend(row.keys() - (self.head + self.tail))
                self.f.seek(0)
                self.f.truncate()
                self.writer = csv.DictWriter(self.f, self.head + self.tail)
                self.writer.writeheader()
                for row in self.rows:
                    self.writer.writerow(row)

# A test failure
class TestFailure(Exception):
    def __init__(self, id, returncode, stdout, assert_=None):
        self.id = id
        self.returncode = returncode
        self.stdout = stdout
        self.assert_ = assert_

def run_stage(name, runner, test_ids, stdout_, trace_, output_, **args):
    # get expected suite/case/perm counts
    (case_suites,
            expected_suite_perms,
            expected_case_perms,
            expected_perms,
            total_perms) = find_perms(runner, test_ids, **args)

    passed_suite_perms = co.defaultdict(lambda: 0)
    passed_case_perms = co.defaultdict(lambda: 0)
    passed_perms = 0
    failed_perms = 0
    powerlosses = 0
    failures = []
    killed = False

    pattern = re.compile('^(?:'
                '(?P<op>running|finished|skipped|powerloss) '
                    '(?P<id>(?P<case>[^:]+)[^\s]*)'
                '|' '(?P<path>[^:]+):(?P<lineno>\d+):(?P<op_>assert):'
                    ' *(?P<message>.*)'
            ')$')
    locals = th.local()
    children = set()

    def run_runner(runner_):
        nonlocal passed_suite_perms
        nonlocal passed_case_perms
        nonlocal passed_perms
        nonlocal powerlosses
        nonlocal locals

        # run the tests!
        cmd = runner_
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))

        mpty, spty = pty.openpty()
        proc = sp.Popen(cmd, stdout=spty, stderr=spty, close_fds=False)
        os.close(spty)
        children.add(proc)
        mpty = os.fdopen(mpty, 'r', 1)

        last_id = None
        last_stdout = co.deque(maxlen=args.get('context', 5) + 1)
        last_assert = None
        last_time = time.time()
        try:
            while True:
                # parse a line for state changes
                try:
                    line = mpty.readline()
                except OSError as e:
                    if e.errno != errno.EIO:
                        raise
                    break
                if not line:
                    break
                last_stdout.append(line)
                if stdout_:
                    try:
                        stdout_.write(line)
                        stdout_.flush()
                    except BrokenPipeError:
                        pass

                m = pattern.match(line)
                if m:
                    op = m.group('op') or m.group('op_')
                    if op == 'running':
                        locals.seen_perms += 1
                        last_id = m.group('id')
                        last_stdout.clear()
                        last_assert = None
                        last_time = time.time()
                    elif op == 'powerloss':
                        last_id = m.group('id')
                        powerlosses += 1
                    elif op == 'finished':
                        # force a failure?
                        if args.get('fail'):
                            proc.kill()
                            raise TestFailure(last_id, 0, list(last_stdout))
                        # passed
                        case = m.group('case')
                        suite = case_suites[case]
                        passed_suite_perms[suite] += 1
                        passed_case_perms[case] += 1
                        passed_perms += 1
                        if output_:
                            # get defines and write to csv
                            defines = find_defines(
                                    runner, m.group('id'), **args)
                            output_.writerow({
                                    'suite': suite,
                                    'case': case,
                                    **defines,
                                    'test_passed': '1/1',
                                    'test_time': '%.6f' % (
                                        time.time() - last_time)})
                    elif op == 'skipped':
                        locals.seen_perms += 1
                    elif op == 'assert':
                        last_assert = (
                                m.group('path'),
                                int(m.group('lineno')),
                                m.group('message'))
                        # go ahead and kill the process, aborting takes a while
                        if args.get('keep_going'):
                            proc.kill()
        except KeyboardInterrupt:
            proc.kill()
            raise TestFailure(last_id, 0, list(last_stdout))
        finally:
            children.remove(proc)
            mpty.close()

        proc.wait()
        if proc.returncode != 0:
            raise TestFailure(
                    last_id,
                    proc.returncode,
                    list(last_stdout),
                    last_assert)

    def run_job(main=True, start=None, step=None):
        nonlocal failed_perms
        nonlocal failures
        nonlocal killed
        nonlocal locals

        start = start or 0
        step = step or 1
        while start < total_perms:
            runner_ = find_runner(runner, main=main, **args)
            if args.get('isolate') or args.get('valgrind'):
                runner_.append('-s%s,%s,%s' % (start, start+step, step))
            elif start != 0 or step != 1:
                runner_.append('-s%s,,%s' % (start, step))

            runner_.extend(test_ids)

            try:
                # run the tests
                locals.seen_perms = 0
                run_runner(runner_)
                assert locals.seen_perms > 0
                start += locals.seen_perms*step

            except TestFailure as failure:
                # keep track of failures
                if output_ and failure.id is not None:
                    case, _ = failure.id.split(':', 1)
                    suite = case_suites[case]
                    # get defines and write to csv
                    defines = find_defines(runner, failure.id, **args)
                    output_.writerow({
                            'suite': suite,
                            'case': case,
                            'test_passed': '0/1',
                            **defines})

                # race condition for multiple failures?
                if not failures or args.get('keep_going'):
                    # keep track of how many failed
                    failed_perms += 1

                    # do not store more failures than we need to, otherwise
                    # we quickly explode RAM when a common bug fails a bunch
                    # of cases
                    if len(failures) < args.get('failures', 3):
                        failures.append(failure)

                if args.get('keep_going') and not killed:
                    # resume after failed test
                    assert locals.seen_perms > 0
                    start += locals.seen_perms*step
                    continue
                else:
                    # stop other tests
                    killed = True
                    for child in children.copy():
                        child.kill()
                    break


    # parallel jobs?
    runners = []
    if 'jobs' in args:
        for job in range(args['jobs']):
            runners.append(th.Thread(
                    target=run_job, args=(job == 0, job, args['jobs']),
                    daemon=True))
    else:
        runners.append(th.Thread(
                target=run_job, args=(True, None, None),
                daemon=True))

    def print_update(done):
        if (not args.get('quiet')
                and not args.get('verbose')
                and not args.get('stdout') == '-'
                and (args['color'] or done)):
            sys.stdout.write('%s%srunning %s%s:%s %s%s' % (
                    '\r\x1b[K' if args['color'] else '',
                    '\x1b[?7l' if not done else '',
                    ('\x1b[32m' if not failed_perms else '\x1b[31m')
                        if args['color'] else '',
                    name,
                    '\x1b[m' if args['color'] else '',
                    ', '.join(filter(None, [
                        '%d/%d suites' % (
                                sum(passed_suite_perms[k] == v
                                    for k, v in expected_suite_perms.items()),
                                len(expected_suite_perms))
                            if (not args.get('by_suites')
                                and not args.get('by_cases')) else None,
                        '%d/%d cases' % (
                                sum(passed_case_perms[k] == v
                                    for k, v in expected_case_perms.items()),
                                len(expected_case_perms))
                            if not args.get('by_cases') else None,
                        '%d/%d perms' % (passed_perms, expected_perms),
                        '%dpls!' % powerlosses
                            if powerlosses else None,
                        '%s%d/%d failures%s' % (
                                '\x1b[31m' if args['color'] else '',
                                failed_perms,
                                expected_perms,
                                '\x1b[m' if args['color'] else '')
                            if failed_perms else None])),
                    '\x1b[?7h' if not done else '\n'))
            sys.stdout.flush()

    for r in runners:
        r.start()

    try:
        while any(r.is_alive() for r in runners):
            time.sleep(0.01)
            print_update(False)
    except KeyboardInterrupt:
        # this is handled by the runner threads, we just
        # need to not abort here
        killed = True
    finally:
        print_update(True)

    for r in runners:
        r.join()

    return (expected_perms,
            passed_perms,
            failed_perms,
            powerlosses,
            failures,
            killed)


def run(runner, test_ids=[], **args):
    # query runner for tests
    if not args.get('quiet'):
        print('using runner: %s' % ' '.join(
                shlex.quote(c) for c in find_runner(runner, **args)))

    # query ids, perms, etc
    test_ids = find_ids(runner, test_ids, **args)
    (_,
            expected_suite_perms,
            expected_case_perms,
            expected_perms,
            total_perms) = find_perms(runner, test_ids, **args)
    if not args.get('quiet'):
        print('found %d suites, %d cases, %d/%d permutations' % (
                len(expected_suite_perms),
                len(expected_case_perms),
                expected_perms,
                total_perms))
        print()

    # automatic job detection?
    if args.get('jobs') == 0:
        args['jobs'] = len(os.sched_getaffinity(0))

    # truncate and open logs here so they aren't disconnected between tests
    stdout = None
    if args.get('stdout'):
        stdout = openio(args['stdout'], 'w', 1)
    trace = None
    if args.get('trace'):
        trace = openio(args['trace'], 'w', 1)
    output = None
    if args.get('output'):
        output = TestOutput(args['output'],
                ['suite', 'case'],
                # defines go here
                ['test_passed', 'test_time'])

    # measure runtime
    start = time.time()

    # spawn runners
    expected = 0
    passed = 0
    failed = 0
    powerlosses = 0
    failures = []
    for by in (test_ids if test_ids else [None]):
        # spawn jobs for stage
        (expected_,
                passed_,
                failed_,
                powerlosses_,
                failures_,
                killed) = run_stage(
                    by or 'tests',
                    runner,
                    [by] if by is not None else [],
                    stdout,
                    trace,
                    output,
                    **args)
        # collect passes/failures
        expected += expected_
        passed += passed_
        failed += failed_
        powerlosses += powerlosses_
        # do not store more failures than we need to, otherwise we
        # quickly explode RAM when a common bug fails a bunch of cases
        failures.extend(failures_[:max(
                args.get('failures', 3) - len(failures),
                0)])
        if (failed and not args.get('keep_going')) or killed:
            break

    stop = time.time()

    if stdout:
        try:
            stdout.close()
        except BrokenPipeError:
            pass
    if trace:
        try:
            trace.close()
        except BrokenPipeError:
            pass
    if output:
        output.close()

    # show summary
    if not args.get('quiet'):
        print()
        print('%sdone:%s %s' % (
                ('\x1b[32m' if not failed else '\x1b[31m')
                    if args['color'] else '',
                '\x1b[m' if args['color'] else '',
                ', '.join(filter(None, [
                    '%d/%d passed' % (passed, expected),
                    '%d/%d failed' % (failed, expected),
                    '%dpls!' % powerlosses if powerlosses else None,
                    'in %.2fs' % (stop-start)]))))
        print()

    # print each failure
    for failure in failures[:args.get('failures', 3)]:
        assert failure.id is not None, '%s broken? %r' % (
                ' '.join(shlex.quote(c) for c in find_runner(runner, **args)),
                failure)

        # get some extra info from runner
        path, lineno = find_path(runner, failure.id, **args)
        defines = find_defines(runner, failure.id, **args)

        # show summary of failure
        print('%s%s:%d:%sfailure:%s %s%s failed' % (
                '\x1b[01m' if args['color'] else '',
                path, lineno,
                '\x1b[01;31m' if args['color'] else '',
                '\x1b[m' if args['color'] else '',
                failure.id,
                ' (%s)' % ', '.join('%s=%s' % (k,v)
                    for k,v in defines.items())
                    if defines else ''))

        if failure.stdout:
            stdout = failure.stdout
            if failure.assert_ is not None:
                stdout = stdout[:-1]
            for line in stdout[max(len(stdout)-args.get('context', 5), 0):]:
                sys.stdout.write(line)

        if failure.assert_ is not None:
            path, lineno, message = failure.assert_
            print('%s%s:%d:%sassert:%s %s' % (
                    '\x1b[01m' if args['color'] else '',
                    path, lineno,
                    '\x1b[01;31m' if args['color'] else '',
                    '\x1b[m' if args['color'] else '',
                    message))
            with open(path) as f:
                line = next(it.islice(f, lineno-1, None)).strip('\n')
                print(line)
        print()

    # drop into gdb?
    if failures and (args.get('gdb')
            or args.get('gdb_perm')
            or args.get('gdb_main')
            or args.get('gdb_pl') is not None
            or args.get('gdb_pl_before')
            or args.get('gdb_pl_after')):
        failure = failures[0]
        cmd = find_runner(runner, failure.id, **args)

        if args.get('gdb_main'):
            # we don't really need the case breakpoint here, but it
            # can be helpful
            path, lineno = find_path(runner, failure.id, **args)
            cmd[:0] = args['gdb_path'] + [
                    '-q',
                    '-ex', 'break main',
                    '-ex', 'break %s:%d' % (path, lineno),
                    '-ex', 'run',
                    '--args']
        elif args.get('gdb_perm'):
            path, lineno = find_path(runner, failure.id, **args)
            cmd[:0] = args['gdb_path'] + [
                    '-q',
                    '-ex', 'break %s:%d' % (path, lineno),
                    '-ex', 'run',
                    '--args']
        elif args.get('gdb_pl') is not None:
            path, lineno = find_path(runner, failure.id, **args)
            cmd[:0] = args['gdb_path'] + [
                    '-q',
                    '-ex', 'break %s:%d' % (path, lineno),
                    '-ex', 'ignore 1 %d' % args['gdb_pl'],
                    '-ex', 'run',
                    '--args']
        elif args.get('gdb_pl_before'):
            # figure out how many powerlosses there were
            powerlosses = (
                    sum(1 for _ in re.finditer('[0-9a-f]',
                            failure.id.split(':', 2)[-1]))
                        if failure.id.count(':') >= 2 else 0)
            path, lineno = find_path(runner, failure.id, **args)
            cmd[:0] = args['gdb_path'] + [
                    '-q',
                    '-ex', 'break %s:%d' % (path, lineno),
                    '-ex', 'ignore 1 %d' % max(powerlosses-1, 0),
                    '-ex', 'run',
                    '--args']
        elif args.get('gdb_pl_after'):
            # figure out how many powerlosses there were
            powerlosses = (
                    sum(1 for _ in re.finditer('[0-9a-f]',
                            failure.id.split(':', 2)[-1]))
                        if failure.id.count(':') >= 2 else 0)
            path, lineno = find_path(runner, failure.id, **args)
            cmd[:0] = args['gdb_path'] + [
                    '-q',
                    '-ex', 'break %s:%d' % (path, lineno),
                    '-ex', 'ignore 1 %d' % powerlosses,
                    '-ex', 'run',
                    '--args']
        else:
            cmd[:0] = args['gdb_path'] + [
                    '-q',
                    '-ex', 'run',
                    '--args']

        # exec gdb interactively
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))
        os.execvp(cmd[0], cmd)

    return 1 if failed else 0


def main(**args):
    # figure out what color should be
    if args.get('color') == 'auto':
        args['color'] = sys.stdout.isatty()
    elif args.get('color') == 'always':
        args['color'] = True
    else:
        args['color'] = False

    if args.get('compile'):
        return compile(**args)
    elif (args.get('summary')
            or args.get('list_suites')
            or args.get('list_cases')
            or args.get('list_suite_paths')
            or args.get('list_case_paths')
            or args.get('list_defines')
            or args.get('list_permutation_defines')
            or args.get('list_implicit_defines')
            or args.get('list_powerlosses')):
        return list_(**args)
    else:
        return run(**args)


if __name__ == "__main__":
    import argparse
    import sys
    argparse.ArgumentParser._handle_conflict_ignore = lambda *_: None
    argparse._ArgumentGroup._handle_conflict_ignore = lambda *_: None
    parser = argparse.ArgumentParser(
            description="Build and run tests.",
            allow_abbrev=False,
            conflict_handler='ignore')
    parser.add_argument(
            '-v', '--verbose',
            action='store_true',
            help="Output commands that run behind the scenes.")
    parser.add_argument(
            '-q', '--quiet',
            action='store_true',
            help="Show nothing except for test failures.")
    parser.add_argument(
            '--color',
            choices=['never', 'always', 'auto'],
            default='auto',
            help="When to use terminal colors. Defaults to 'auto'.")

    # test flags
    test_parser = parser.add_argument_group('test options')
    test_parser.add_argument(
            'test_ids',
            nargs='*',
            help="Description of tests to run.")
    test_parser.add_argument(
            '-R', '--runner',
            type=lambda x: x.split(),
            default=RUNNER_PATH,
            help="Test runner to use for testing. Defaults to "
                "%r." % RUNNER_PATH)
    test_parser.add_argument(
            '-Y', '--summary',
            action='store_true',
            help="Show quick summary.")
    test_parser.add_argument(
            '-l', '--list-suites',
            action='store_true',
            help="List test suites.")
    test_parser.add_argument(
            '-L', '--list-cases',
            action='store_true',
            help="List test cases.")
    test_parser.add_argument(
            '--list-suite-paths',
            action='store_true',
            help="List the path for each test suite.")
    test_parser.add_argument(
            '--list-case-paths',
            action='store_true',
            help="List the path and line number for each test case.")
    test_parser.add_argument(
            '--list-defines',
            action='store_true',
            help="List all defines in this test-runner.")
    test_parser.add_argument(
            '--list-permutation-defines',
            action='store_true',
            help="List explicit defines in this test-runner.")
    test_parser.add_argument(
            '--list-implicit-defines',
            action='store_true',
            help="List implicit defines in this test-runner.")
    test_parser.add_argument(
            '--list-powerlosses',
            action='store_true',
            help="List the available power-loss scenarios.")
    test_parser.add_argument(
            '-D', '--define',
            action='append',
            help="Override a test define.")
    test_parser.add_argument(
            '--define-depth',
            help="How deep to evaluate recursive defines before erroring.")
    test_parser.add_argument(
            '-P', '--powerloss',
            help="Comma-separated list of power-loss scenarios to test.")
    test_parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Ignore test filters.")
    test_parser.add_argument(
            '-d', '--disk',
            help="Direct block device operations to this file.")
    test_parser.add_argument(
            '-t', '--trace',
            help="Direct trace output to this file.")
    test_parser.add_argument(
            '--trace-backtrace',
            action='store_true',
            help="Include a backtrace with every trace statement.")
    test_parser.add_argument(
            '--trace-period',
            help="Sample trace output at this period in cycles.")
    test_parser.add_argument(
            '--trace-freq',
            help="Sample trace output at this frequency in hz.")
    test_parser.add_argument(
            '-O', '--stdout',
            help="Direct stdout to this file. Note stderr is already merged "
                "here.")
    test_parser.add_argument(
            '-o', '--output',
            help="CSV file to store results.")
    test_parser.add_argument(
            '--read-sleep',
            help="Artificial read delay in seconds.")
    test_parser.add_argument(
            '--prog-sleep',
            help="Artificial prog delay in seconds.")
    test_parser.add_argument(
            '--erase-sleep',
            help="Artificial erase delay in seconds.")
    test_parser.add_argument(
            '-j', '--jobs',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Number of parallel runners to run. 0 runs one runner per "
                "core.")
    test_parser.add_argument(
            '-k', '--keep-going',
            action='store_true',
            help="Don't stop on first failure.")
    test_parser.add_argument(
            '-f', '--fail',
            action='store_true',
            help="Force a failure.")
    test_parser.add_argument(
            '-i', '--isolate',
            action='store_true',
            help="Run each test permutation in a separate process.")
    test_parser.add_argument(
            '-b', '--by-suites',
            action='store_true',
            help="Step through tests by suite.")
    test_parser.add_argument(
            '-B', '--by-cases',
            action='store_true',
            help="Step through tests by case.")
    test_parser.add_argument(
            '-F', '--failures',
            type=lambda x: int(x, 0),
            default=3,
            help="Show this many test failures. Defaults to 3.")
    test_parser.add_argument(
            '-C', '--context',
            type=lambda x: int(x, 0),
            default=5,
            help="Show this many lines of stdout on test failure. "
                "Defaults to 5.")
    test_parser.add_argument(
            '--gdb',
            action='store_true',
            help="Drop into gdb on test failure.")
    test_parser.add_argument(
            '--gdb-perm', '--gdb-permutation',
            action='store_true',
            help="Drop into gdb on test failure but stop at the beginning "
                "of the failing test permutation.")
    test_parser.add_argument(
            '--gdb-main',
            action='store_true',
            help="Drop into gdb on test failure but stop at the beginning "
                "of main.")
    test_parser.add_argument(
            '--gdb-pl',
            type=lambda x: int(x, 0),
            help="Drop into gdb on this specific powerloss.")
    test_parser.add_argument(
            '--gdb-pl-before',
            action='store_true',
            help="Drop into gdb before the powerloss that caused the failure.")
    test_parser.add_argument(
            '--gdb-pl-after',
            action='store_true',
            help="Drop into gdb after the powerloss that caused the failure.")
    test_parser.add_argument(
            '--gdb-path',
            type=lambda x: x.split(),
            default=GDB_PATH,
            help="Path to the gdb executable, may include flags. "
                "Defaults to %r." % GDB_PATH)
    test_parser.add_argument(
            '--exec',
            type=lambda e: e.split(),
            help="Run under another executable.")
    test_parser.add_argument(
            '--valgrind',
            action='store_true',
            help="Run under Valgrind to find memory errors. Implicitly sets "
                "--isolate.")
    test_parser.add_argument(
            '--valgrind-path',
            type=lambda x: x.split(),
            default=VALGRIND_PATH,
            help="Path to the Valgrind executable, may include flags. "
                "Defaults to %r." % VALGRIND_PATH)
    test_parser.add_argument(
            '-p', '--perf',
            help="Run under Linux's perf to sample performance counters, "
                "writing samples to this file.")
    test_parser.add_argument(
            '--perf-freq',
            help="perf sampling frequency. This is passed directly to the "
                "perf script.")
    test_parser.add_argument(
            '--perf-period',
            help="perf sampling period. This is passed directly to the perf "
                "script.")
    test_parser.add_argument(
            '--perf-events',
            help="perf events to record. This is passed directly to the perf "
                "script.")
    test_parser.add_argument(
            '--perf-script',
            type=lambda x: x.split(),
            default=PERF_SCRIPT,
            help="Path to the perf script to use. Defaults to "
                "%r." % PERF_SCRIPT)
    test_parser.add_argument(
            '--perf-path',
            type=lambda x: x.split(),
            help="Path to the perf executable, may include flags. This is "
                "passed directly to the perf script")

    # compilation flags
    comp_parser = parser.add_argument_group('compilation options')
    comp_parser.add_argument(
            'test_paths',
            nargs='*',
            help="Set of *.toml files to compile.")
    comp_parser.add_argument(
            '-c', '--compile',
            action='store_true',
            help="Compile a test suite or source file.")
    comp_parser.add_argument(
            '-s', '--source',
            help="Source file to compile, possibly injecting internal tests.")
    comp_parser.add_argument(
            '--include',
            default=HEADER_PATH,
            help="Inject this header file into every compiled test file. "
                "Defaults to %r." % HEADER_PATH)
    comp_parser.add_argument(
            '-o', '--output',
            help="Output file.")

    # do the thing
    args = parser.parse_intermixed_args()
    args.test_paths = args.test_ids
    sys.exit(main(**{k: v
            for k, v in vars(args).items()
            if v is not None}))
