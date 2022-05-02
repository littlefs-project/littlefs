#!/usr/bin/env python3
#
# Script to compile and runs tests.
#

import collections as co
import errno
import glob
import itertools as it
import math as m
import os
import pty
import re
import shlex
import shutil
import subprocess as sp
import threading as th
import time
import toml


TEST_PATHS = ['tests_']
RUNNER_PATH = './runners/test_runner'

SUITE_PROLOGUE = """
#include "runners/test_runner.h"
#include "bd/lfs_testbd.h"
#include <stdio.h>
"""
CASE_PROLOGUE = """
"""
CASE_EPILOGUE = """
"""


def testpath(path):
    path, *_ = path.split('#', 1)
    return path

def testsuite(path):
    suite = testpath(path)
    suite = os.path.basename(suite)
    if suite.endswith('.toml'):
        suite = suite[:-len('.toml')]
    return suite

def testcase(path):
    _, case, *_ = path.split('#', 2)
    return '%s#%s' % (testsuite(path), case)

# TODO move this out in other files
def openio(path, mode='r'):
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), 'r')
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), 'w')
    else:
        return open(path, mode)

class TestCase:
    # create a TestCase object from a config
    def __init__(self, config, args={}):
        self.name = config.pop('name')
        self.path = config.pop('path')
        self.suite = config.pop('suite')
        self.lineno = config.pop('lineno', None)
        self.if_ = config.pop('if', None)
        if isinstance(self.if_, bool):
            self.if_ = 'true' if self.if_ else 'false'
        self.if_lineno = config.pop('if_lineno', None)
        self.code = config.pop('code')
        self.code_lineno = config.pop('code_lineno', None)
        self.in_ = config.pop('in',
            config.pop('suite_in', None))

        self.normal = config.pop('normal',
            config.pop('suite_normal', True))
        self.reentrant = config.pop('reentrant',
            config.pop('suite_reentrant', False))
        self.valgrind = config.pop('valgrind',
            config.pop('suite_valgrind', True))

        # figure out defines and build possible permutations
        self.defines = set()
        self.permutations = []

        suite_defines = config.pop('suite_defines', {})
        if not isinstance(suite_defines, list):
            suite_defines = [suite_defines]
        defines = config.pop('defines', {})
        if not isinstance(defines, list):
            defines = [defines]

        # build possible permutations
        for suite_defines_ in suite_defines:
            self.defines |= suite_defines_.keys()
            for defines_ in defines:
                self.defines |= defines_.keys()
                self.permutations.extend(map(dict, it.product(*(
                    [(k, v) for v in (vs if isinstance(vs, list) else [vs])]
                    for k, vs in sorted(
                        (suite_defines_ | defines_).items())))))

        for k in config.keys():
            print('\x1b[01;33mwarning:\x1b[m in %s, found unused key %r'
                % (self.id(), k),
                file=sys.stderr)

    def id(self):
        return '%s#%s' % (self.suite, self.name)


class TestSuite:
    # create a TestSuite object from a toml file
    def __init__(self, path, args={}):
        self.name = testsuite(path)
        self.path = testpath(path)

        # load toml file and parse test cases
        with open(self.path) as f:
            # load tests
            config = toml.load(f)

            # find line numbers
            f.seek(0)
            case_linenos = []
            if_linenos = []
            code_linenos = []
            for i, line in enumerate(f):
                match = re.match(
                    '(?P<case>\[\s*cases\s*\.\s*(?P<name>\w+)\s*\])'
                        '|' '(?P<if>if\s*=)'
                        '|' '(?P<code>code\s*=)',
                    line)
                if match and match.group('case'):
                    case_linenos.append((i+1, match.group('name')))
                elif match and match.group('if'):
                    if_linenos.append(i+1)
                elif match and match.group('code'):
                    code_linenos.append(i+2)

            # sort in case toml parsing did not retain order
            case_linenos.sort()

            cases = config.pop('cases')
            for (lineno, name), (nlineno, _) in it.zip_longest(
                    case_linenos, case_linenos[1:],
                    fillvalue=(float('inf'), None)):
                if_lineno = min(
                    (l for l in if_linenos if l >= lineno and l < nlineno),
                    default=None)
                code_lineno = min(
                    (l for l in code_linenos if l >= lineno and l < nlineno),
                    default=None)
                cases[name]['lineno'] = lineno
                cases[name]['if_lineno'] = if_lineno
                cases[name]['code_lineno'] = code_lineno

            self.if_ = config.pop('if', None)
            if isinstance(self.if_, bool):
                self.if_ = 'true' if self.if_ else 'false'
            self.if_lineno = min(
                (l for l in if_linenos
                    if not case_linenos or l < case_linenos[0][0]),
                default=None)

            self.code = config.pop('code', None)
            self.code_lineno = min(
                (l for l in code_linenos
                    if not case_linenos or l < case_linenos[0][0]),
                default=None)

            # a couple of these we just forward to all cases
            defines = config.pop('defines', {})
            in_ = config.pop('in', None)
            normal = config.pop('normal', True)
            reentrant = config.pop('reentrant', False)
            valgrind = config.pop('valgrind', True)

            self.cases = []
            for name, case in sorted(cases.items(),
                    key=lambda c: c[1].get('lineno')):
                self.cases.append(TestCase(config={
                    'name': name,
                    'path': path + (':%d' % case['lineno']
                        if 'lineno' in case else ''),
                    'suite': self.name,
                    'suite_defines': defines,
                    'suite_in': in_,
                    'suite_normal': normal,
                    'suite_reentrant': reentrant,
                    'suite_valgrind': valgrind,
                    **case}))

            # combine per-case defines
            self.defines = set.union(*(
                set(case.defines) for case in self.cases))

            # combine other per-case things
            self.normal = any(case.normal for case in self.cases)
            self.reentrant = any(case.reentrant for case in self.cases)
            self.valgrind = any(case.valgrind for case in self.cases)

        for k in config.keys():
            print('\x1b[01;33mwarning:\x1b[m in %s, found unused key %r'
                % (self.id(), k),
                file=sys.stderr)

    def id(self):
        return self.name



def compile(**args):
    # find .toml files
    paths = []
    for path in args.get('test_paths', TEST_PATHS):
        if os.path.isdir(path):
            path = path + '/*.toml'

        for path in glob.glob(path):
            paths.append(path)

    if not paths:
        print('no test suites found in %r?' % args['test_paths'])
        sys.exit(-1)

    if not args.get('source'):
        if len(paths) > 1:
            print('more than one test suite for compilation? (%r)'
                % args['test_paths'])
            sys.exit(-1)

        # load our suite
        suite = TestSuite(paths[0])
    else:
        # load all suites
        suites = [TestSuite(path) for path in paths]
        suites.sort(key=lambda s: s.name)

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

            # redirect littlefs tracing
            f.writeln('#define LFS_TRACE_(fmt, ...) do { \\')
            f.writeln(8*' '+'extern FILE *test_trace; \\')
            f.writeln(8*' '+'if (test_trace) { \\')
            f.writeln(12*' '+'fprintf(test_trace, '
                '"%s:%d:trace: " fmt "%s\\n", \\')
            f.writeln(20*' '+'__FILE__, __LINE__, __VA_ARGS__); \\')
            f.writeln(8*' '+'} \\')
            f.writeln(4*' '+'} while (0)')
            f.writeln('#define LFS_TRACE(...) LFS_TRACE_(__VA_ARGS__, "")')
            f.writeln('#define LFS_TESTBD_TRACE(...) '
                'LFS_TRACE_(__VA_ARGS__, "")')
            f.writeln()

            # write out generated functions, this can end up in different
            # files depending on the "in" attribute
            #
            # note it's up to the specific generated file to declare
            # the test defines
            def write_case_functions(f, suite, case):
                    # create case filter function
                    if suite.if_ is not None or case.if_ is not None:
                        f.writeln('bool __test__%s__%s__filter(void) {'
                            % (suite.name, case.name))
                        if suite.if_ is not None:
                            if suite.if_lineno is not None:
                                f.writeln(4*' '+'#line %d "%s"'
                                    % (suite.if_lineno, suite.path))
                            f.writeln(4*' '+'if (!(%s)) {' % suite.if_)
                            if suite.if_lineno is not None:
                                f.writeln(4*' '+'#line %d "%s"'
                                    % (f.lineno+1, args['output']))
                            f.writeln(8*' '+'return false;')
                            f.writeln(4*' '+'}')
                            f.writeln()
                        if case.if_ is not None:
                            if case.if_lineno is not None:
                                f.writeln(4*' '+'#line %d "%s"'
                                    % (case.if_lineno, suite.path))
                            f.writeln(4*' '+'if (!(%s)) {' % case.if_)
                            if case.if_lineno is not None:
                                f.writeln(4*' '+'#line %d "%s"'
                                    % (f.lineno+1, args['output']))
                            f.writeln(8*' '+'return false;')
                            f.writeln(4*' '+'}')
                            f.writeln()
                        f.writeln(4*' '+'return true;')
                        f.writeln('}')
                        f.writeln()

                    # create case run function
                    f.writeln('void __test__%s__%s__run('
                        '__attribute__((unused)) struct lfs_config *cfg) {'
                        % (suite.name, case.name))
                    if CASE_PROLOGUE.strip():
                        f.writeln(4*' '+'%s'
                            % CASE_PROLOGUE.strip().replace('\n', '\n'+4*' '))
                        f.writeln()
                    f.writeln(4*' '+'// test case %s' % case.id())
                    if case.code_lineno is not None:
                        f.writeln(4*' '+'#line %d "%s"'
                            % (case.code_lineno, suite.path))
                    f.write(case.code)
                    if case.code_lineno is not None:
                        f.writeln(4*' '+'#line %d "%s"'
                            % (f.lineno+1, args['output']))
                    if CASE_EPILOGUE.strip():
                        f.writeln()
                        f.writeln(4*' '+'%s'
                            % CASE_EPILOGUE.strip().replace('\n', '\n'+4*' '))
                    f.writeln('}')
                    f.writeln()

            if not args.get('source'):
                # write test suite prologue
                f.writeln('%s' % SUITE_PROLOGUE.strip())
                f.writeln()
                if suite.code is not None:
                    if suite.code_lineno is not None:
                        f.writeln('#line %d "%s"'
                            % (suite.code_lineno, suite.path))
                    f.write(suite.code)
                    if suite.code_lineno is not None:
                        f.writeln('#line %d "%s"'
                            % (f.lineno+1, args['output']))
                    f.writeln()

                if suite.defines:
                    for i, define in enumerate(sorted(suite.defines)):
                        f.writeln('#ifndef %s' % define)
                        f.writeln('#define %-24s test_define(%d)'
                            % (define, i))
                        f.writeln('#endif')
                    f.writeln()

                for case in suite.cases:
                    # create case defines
                    if case.defines:
                        f.writeln('const test_define_t *const '
                            '__test__%s__%s__defines[] = {'
                            % (suite.name, case.name))
                        for permutation in case.permutations:
                            f.writeln(4*' '+'(const test_define_t[]){%s},'
                                % ', '.join(str(v) for _, v in sorted(
                                    permutation.items())))
                        f.writeln('};')
                        f.writeln()

                        f.writeln('const uint8_t '
                            '__test__%s__%s__define_map[] = {'
                            % (suite.name, case.name))
                        f.writeln(4*' '+'%s,'
                            % ', '.join(
                                str(sorted(case.defines).index(k))
                                if k in case.defines else '0xff'
                                for k in sorted(suite.defines)))
                        f.writeln('};')
                        f.writeln()

                    # create case functions
                    if case.in_ is None:
                        write_case_functions(f, suite, case)
                    else:
                        if suite.if_ is not None or case.if_ is not None:
                            f.writeln('extern bool __test__%s__%s__filter('
                                'void);'
                                % (suite.name, case.name))
                        f.writeln('extern void __test__%s__%s__run('
                            'struct lfs_config *cfg);'
                            % (suite.name, case.name))
                        f.writeln()

                    # create case struct
                    f.writeln('const struct test_case __test__%s__%s__case = {'
                        % (suite.name, case.name))
                    f.writeln(4*' '+'.id = "%s",' % case.id())
                    f.writeln(4*' '+'.name = "%s",' % case.name)
                    f.writeln(4*' '+'.path = "%s",' % case.path)
                    f.writeln(4*' '+'.types = %s,'
                        % ' | '.join(filter(None, [
                            'TEST_NORMAL' if case.normal else None,
                            'TEST_REENTRANT' if case.reentrant else None,
                            'TEST_VALGRIND' if case.valgrind else None])))
                    f.writeln(4*' '+'.permutations = %d,'
                        % len(case.permutations))
                    if case.defines:
                        f.writeln(4*' '+'.defines = __test__%s__%s__defines,'
                            % (suite.name, case.name))
                        f.writeln(4*' '+'.define_map = '
                            '__test__%s__%s__define_map,'
                            % (suite.name, case.name))
                    if suite.if_ is not None or case.if_ is not None:
                        f.writeln(4*' '+'.filter = __test__%s__%s__filter,'
                            % (suite.name, case.name))
                    f.writeln(4*' '+'.run = __test__%s__%s__run,'
                        % (suite.name, case.name))
                    f.writeln('};')
                    f.writeln()

                # create suite define names
                if suite.defines:
                    f.writeln('const char *const __test__%s__define_names[] = {'
                        % suite.name)
                    for k in sorted(suite.defines):
                        f.writeln(4*' '+'"%s",' % k)
                    f.writeln('};')
                    f.writeln()

                # create suite struct
                f.writeln('const struct test_suite __test__%s__suite = {'
                    % suite.name)
                f.writeln(4*' '+'.id = "%s",' % suite.id())
                f.writeln(4*' '+'.name = "%s",' % suite.name)
                f.writeln(4*' '+'.path = "%s",' % suite.path)
                f.writeln(4*' '+'.types = %s,'
                    % ' | '.join(filter(None, [
                        'TEST_NORMAL' if suite.normal else None,
                        'TEST_REENTRANT' if suite.reentrant else None,
                        'TEST_VALGRIND' if suite.valgrind else None])))
                if suite.defines:
                    f.writeln(4*' '+'.define_names = __test__%s__define_names,'
                        % suite.name)
                f.writeln(4*' '+'.define_count = %d,' % len(suite.defines))
                f.writeln(4*' '+'.cases = (const struct test_case *const []){')
                for case in suite.cases:
                    f.writeln(8*' '+'&__test__%s__%s__case,'
                        % (suite.name, case.name))
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

                f.write(SUITE_PROLOGUE)
                f.writeln()

                # write any internal tests
                for suite in suites:
                    for case in suite.cases:
                        if case.in_ == args.get('source'):
                            # write defines, but note we need to undef any
                            # new defines since we're in someone else's file
                            if suite.defines:
                                for i, define in enumerate(
                                        sorted(suite.defines)):
                                    f.writeln('#ifndef %s' % define)
                                    f.writeln('#define %-24s test_define(%d)'
                                        % (define, i))
                                    f.writeln('#define __TEST__%s__NEEDS_UNDEF'
                                        % define)
                                    f.writeln('#endif')
                                f.writeln()

                            write_case_functions(f, suite, case)

                            if suite.defines:
                                for define in sorted(suite.defines):
                                    f.writeln('#ifdef __TEST__%s__NEEDS_UNDEF'
                                        % define)
                                    f.writeln('#undef __TEST__%s__NEEDS_UNDEF'
                                        % define)
                                    f.writeln('#undef %s' % define)
                                    f.writeln('#endif')
                                f.writeln()

                # add suite info to test_runner.c
                if args['source'] == 'runners/test_runner.c':
                    f.writeln()
                    for suite in suites:
                        f.writeln('extern const struct test_suite '
                            '__test__%s__suite;' % suite.name)
                    f.writeln('const struct test_suite *test_suites[] = {')
                    for suite in suites:
                        f.writeln(4*' '+'&__test__%s__suite,' % suite.name)
                    f.writeln('};')
                    f.writeln('const size_t test_suite_count = %d;'
                        % len(suites))

def runner(**args):
    cmd = args['runner'].copy()
    # TODO multiple paths?
    if 'test_paths' in args:
        cmd.extend(args.get('test_paths'))

    if args.get('normal'):    cmd.append('-n')
    if args.get('reentrant'): cmd.append('-r')
    if args.get('valgrind'):  cmd.append('-V')
    if args.get('geometry'):
        cmd.append('-G%s' % args.get('geometry'))
    if args.get('define'):
        for define in args.get('define'):
            cmd.append('-D%s' % define)

    return cmd

def list_(**args):
    cmd = runner(**args)
    if args.get('summary'):         cmd.append('--summary')
    if args.get('list_suites'):     cmd.append('--list-suites')
    if args.get('list_cases'):      cmd.append('--list-cases')
    if args.get('list_paths'):      cmd.append('--list-paths')
    if args.get('list_defines'):    cmd.append('--list-defines')
    if args.get('list_geometries'): cmd.append('--list-geometries')

    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    sys.exit(sp.call(cmd))


def find_cases(runner_, **args):
    # query from runner
    cmd = runner_ + ['--list-cases']
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
        stdout=sp.PIPE,
        stderr=sp.PIPE if not args.get('verbose') else None,
        universal_newlines=True,
        errors='replace')
    expected_suite_perms = co.defaultdict(lambda: 0)
    expected_case_perms = co.defaultdict(lambda: 0)
    expected_perms = 0
    total_perms = 0
    pattern = re.compile(
        '^(?P<id>(?P<case>(?P<suite>[^#]+)#[^\s#]+)[^\s]*)\s+'
            '[^\s]+\s+(?P<filtered>\d+)/(?P<perms>\d+)')
    # skip the first line
    next(proc.stdout)
    for line in proc.stdout:
        m = pattern.match(line)
        if m:
            filtered = int(m.group('filtered'))
            expected_suite_perms[m.group('suite')] += filtered
            expected_case_perms[m.group('id')] += filtered
            expected_perms += filtered
            total_perms += int(m.group('perms'))
    proc.wait()
    if proc.returncode != 0:
        if not args.get('verbose'):
            for line in proc.stderr:
                sys.stdout.write(line)
        sys.exit(-1)

    return (
        expected_suite_perms,
        expected_case_perms,
        expected_perms,
        total_perms)

def find_paths(runner_, **args):
    # query from runner
    cmd = runner_ + ['--list-paths']
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
        stdout=sp.PIPE,
        stderr=sp.PIPE if not args.get('verbose') else None,
        universal_newlines=True,
        errors='replace')
    paths = co.OrderedDict()
    pattern = re.compile(
        '^(?P<id>(?P<case>(?P<suite>[^#]+)#[^\s#]+)[^\s]*)\s+'
            '(?P<path>[^:]+):(?P<lineno>\d+)')
    # skip the first line
    for line in proc.stdout:
        m = pattern.match(line)
        if m:
            paths[m.group('id')] = (m.group('path'), int(m.group('lineno')))
    proc.wait()
    if proc.returncode != 0:
        if not args.get('verbose'):
            for line in proc.stderr:
                sys.stdout.write(line)
        sys.exit(-1)

    return paths

def find_defines(runner_, **args):
    # query from runner
    cmd = runner_ + ['--list-defines']
    if args.get('verbose'):
        print(' '.join(shlex.quote(c) for c in cmd))
    proc = sp.Popen(cmd,
        stdout=sp.PIPE,
        stderr=sp.PIPE if not args.get('verbose') else None,
        universal_newlines=True,
        errors='replace')
    defines = co.OrderedDict()
    pattern = re.compile(
        '^(?P<id>(?P<case>(?P<suite>[^#]+)#[^\s#]+)[^\s]*)\s+'
            '(?P<defines>(?:\w+=\w+\s*)+)')
    # skip the first line
    for line in proc.stdout:
        m = pattern.match(line)
        if m:
            defines[m.group('id')] = {k: v
                for k, v in re.findall('(\w+)=(\w+)', m.group('defines'))}
    proc.wait()
    if proc.returncode != 0:
        if not args.get('verbose'):
            for line in proc.stderr:
                sys.stdout.write(line)
        sys.exit(-1)

    return defines


class TestFailure(Exception):
    def __init__(self, id, returncode, output, assert_=None):
        self.id = id
        self.returncode = returncode
        self.output = output
        self.assert_ = assert_

def run_stage(name, runner_, **args):
    # get expected suite/case/perm counts
    expected_suite_perms, expected_case_perms, expected_perms, total_perms = (
        find_cases(runner_, **args))

    # TODO valgrind/gdb/exec
    passed_suite_perms = co.defaultdict(lambda: 0)
    passed_case_perms = co.defaultdict(lambda: 0)
    passed_perms = 0
    failures = []
    killed = False

    pattern = re.compile('^(?:'
            '(?P<op>running|finished|skipped) '
                '(?P<id>(?P<case>(?P<suite>[^#]+)#[^\s#]+)[^\s]*)'
            '|' '(?P<path>[^:]+):(?P<lineno>\d+):(?P<op_>assert):'
                ' *(?P<message>.*)' ')$')
    locals = th.local()
    # TODO use process group instead of this set?
    children = set()

    def run_runner(runner_):
        nonlocal passed_suite_perms
        nonlocal passed_case_perms
        nonlocal passed_perms
        nonlocal locals

        # run the tests!
        cmd = runner_.copy()
        if args.get('disk'):
            cmd.append('--disk=%s' % args['disk'])
        if args.get('trace'):
            cmd.append('--trace=%s' % args['trace'])
        if args.get('verbose'):
            print(' '.join(shlex.quote(c) for c in cmd))
        mpty, spty = pty.openpty()
        proc = sp.Popen(cmd, stdout=spty, stderr=spty)
        os.close(spty)
        children.add(proc)
        mpty = os.fdopen(mpty, 'r', 1)
        if args.get('output'):
            output = openio(args['output'], 'w')

        last_id = None
        last_output = []
        last_assert = None
        try:
            while True:
                # parse a line for state changes
                try:
                    line = mpty.readline()
                except OSError as e:
                    if e.errno == errno.EIO:
                        break
                    raise
                if not line:
                    break
                last_output.append(line)
                if args.get('output'):
                    output.write(line)
                elif args.get('verbose'):
                    sys.stdout.write(line)

                m = pattern.match(line)
                if m:
                    op = m.group('op') or m.group('op_')
                    if op == 'running':
                        locals.seen_perms += 1
                        last_id = m.group('id')
                        last_output = []
                        last_assert = None
                    elif op == 'finished':
                        passed_suite_perms[m.group('suite')] += 1
                        passed_case_perms[m.group('case')] += 1
                        passed_perms += 1
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
            raise TestFailure(last_id, 1, last_output)
        finally:
            children.remove(proc)
            mpty.close()
            if args.get('output'):
                output.close()

        proc.wait()
        if proc.returncode != 0:
            raise TestFailure(
                last_id,
                proc.returncode,
                last_output,
                last_assert)

    def run_job(runner, start=None, step=None):
        nonlocal failures
        nonlocal locals

        while (start or 0) < total_perms:
            runner_ = runner.copy()
            if start is not None:
                runner_.append('--start=%d' % start)
            if step is not None:
                runner_.append('--step=%d' % step)

            try:
                # run the tests
                locals.seen_perms = 0
                run_runner(runner_)

            except TestFailure as failure:
                # race condition for multiple failures?
                if failures and not args.get('keep_going'):
                    break

                failures.append(failure)

                if args.get('keep_going') and not killed:
                    # resume after failed test
                    start = (start or 0) + locals.seen_perms*(step or 1)
                    continue
                else:
                    # stop other tests
                    for child in children.copy():
                        child.kill()

            break
    

    # parallel jobs?
    runners = []
    if 'jobs' in args:
        for job in range(args['jobs']):
            runners.append(th.Thread(
                target=run_job, args=(runner_, job, args['jobs'])))
    else:
        runners.append(th.Thread(
            target=run_job, args=(runner_, None, None)))

    for r in runners:
        r.start()

    needs_newline = False
    try:
        while any(r.is_alive() for r in runners):
            time.sleep(0.01)

            if not args.get('verbose'):
                sys.stdout.write('\r\x1b[K'
                    'running \x1b[%dm%s:\x1b[m %s '
                    % (32 if not failures else 31,
                        name,
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
                            '\x1b[31m%d/%d failures\x1b[m'
                                % (len(failures), expected_perms)
                                if failures else None]))))
                sys.stdout.flush()
                needs_newline = True
    except KeyboardInterrupt:
        # this is handled by the runner threads, we just
        # need to not abort here
        killed = True
    finally:
        if needs_newline:
            print()

    for r in runners:
        r.join()

    return (
        expected_perms,
        passed_perms,
        failures,
        killed)
    

def run(**args):
    start = time.time()

    runner_ = runner(**args)
    print('using runner `%s`'
        % ' '.join(shlex.quote(c) for c in runner_))
    expected_suite_perms, expected_case_perms, expected_perms, total_perms = (
        find_cases(runner_, **args))
    print('found %d suites, %d cases, %d/%d permutations'
        % (len(expected_suite_perms),
            len(expected_case_perms),
            expected_perms,
            total_perms))
    print()

    expected = 0
    passed = 0
    failures = []
    for type, by in it.product(
            ['normal', 'reentrant', 'valgrind'],
            expected_case_perms.keys() if args.get('by_cases')
                else expected_suite_perms.keys() if args.get('by_suites')
                else [None]):

        expected_, passed_, failures_, killed = run_stage(
            '%s %s' % (type, by or 'tests'),
            runner_ + ['--%s' % type] + ([by] if by is not None else []),
            **args)
        expected += expected_
        passed += passed_
        failures.extend(failures_)
        if (failures and not args.get('keep_going')) or killed:
            break

    # show summary
    print()
    print('\x1b[%dmdone:\x1b[m %d/%d passed, %d/%d failed, in %.2fs'
        % (32 if not failures else 31,
            passed, expected, len(failures), expected,
            time.time()-start))
    print()

    # print each failure
    if failures:
        # get some extra info from runner
        runner_paths = find_paths(runner_, **args)
        runner_defines = find_defines(runner_, **args)

    for failure in failures:
        # show summary of failure
        path, lineno = runner_paths[testcase(failure.id)]
        defines = runner_defines[failure.id]

        print('\x1b[01m%s:%d:\x1b[01;31mfailure:\x1b[m %s%s failed'
            % (path, lineno, failure.id,
                ' (%s)' % ', '.join(
                    '%s=%s' % (k, v) for k, v in defines.items())
                if defines else ''))

        if failure.output:
            output = failure.output
            if failure.assert_ is not None:
                output = output[:-1]
            for line in output[-5:]:
                sys.stdout.write(line)

        if failure.assert_ is not None:
            path, lineno, message = failure.assert_
            print('\x1b[01m%s:%d:\x1b[01;31massert:\x1b[m %s'
                % (path, lineno, message))
            with open(path) as f:
                line = next(it.islice(f, lineno-1, None)).strip('\n')
                print(line)
        print()

    return 1 if failures else 0


def main(**args):
    if args.get('compile'):
        compile(**args)
    elif (args.get('summary')
            or args.get('list_suites')
            or args.get('list_cases')
            or args.get('list_paths')
            or args.get('list_defines')
            or args.get('list_geometries')
            or args.get('list_defaults')):
        list_(**args)
    else:
        run(**args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
        description="Build and run tests.",
        conflict_handler='resolve')
    # TODO document test case/perm specifier
    parser.add_argument('test_paths', nargs='*',
        help="Description of testis to run. May be a directory, path, or \
            test identifier. Defaults to %r." % TEST_PATHS)
    parser.add_argument('-v', '--verbose', action='store_true',
        help="Output commands that run behind the scenes.")
    # test flags
    test_parser = parser.add_argument_group('test options')
    test_parser.add_argument('-Y', '--summary', action='store_true',
        help="Show quick summary.")
    test_parser.add_argument('-l', '--list-suites', action='store_true',
        help="List test suites.")
    test_parser.add_argument('-L', '--list-cases', action='store_true',
        help="List test cases.")
    test_parser.add_argument('--list-paths', action='store_true',
        help="List the path for each test case.")
    test_parser.add_argument('--list-defines', action='store_true',
        help="List the defines for each test permutation.")
    test_parser.add_argument('--list-geometries', action='store_true',
        help="List the disk geometries used for testing.")
    test_parser.add_argument('--list-defaults', action='store_true',
        help="List the default defines in this test-runner.")
    test_parser.add_argument('-D', '--define', action='append',
        help="Override a test define.")
    test_parser.add_argument('-G', '--geometry',
        help="Filter by geometry.")
    test_parser.add_argument('-n', '--normal', action='store_true',
        help="Filter for normal tests. Can be combined.")
    test_parser.add_argument('-r', '--reentrant', action='store_true',
        help="Filter for reentrant tests. Can be combined.")
    test_parser.add_argument('-V', '--valgrind', action='store_true',
        help="Filter for Valgrind tests. Can be combined.")
    test_parser.add_argument('-d', '--disk',
        help="Use this file as the disk.")
    test_parser.add_argument('-t', '--trace',
        help="Redirect trace output to this file.")
    test_parser.add_argument('-o', '--output',
        help="Redirect stdout and stderr to this file.")
    test_parser.add_argument('--runner', default=[RUNNER_PATH],
        type=lambda x: x.split(),
        help="Path to runner, defaults to %r" % RUNNER_PATH)
    test_parser.add_argument('-j', '--jobs', nargs='?', type=int,
        const=len(os.sched_getaffinity(0)),
        help="Number of parallel runners to run.")
    test_parser.add_argument('-k', '--keep-going', action='store_true',
        help="Don't stop on first error.")
    test_parser.add_argument('-b', '--by-suites', action='store_true',
        help="Step through tests by suite.")
    test_parser.add_argument('-B', '--by-cases', action='store_true',
        help="Step through tests by case.")
    # compilation flags
    comp_parser = parser.add_argument_group('compilation options')
    comp_parser.add_argument('-c', '--compile', action='store_true',
        help="Compile a test suite or source file.")
    comp_parser.add_argument('-s', '--source',
        help="Source file to compile, possibly injecting internal tests.")
    comp_parser.add_argument('-o', '--output',
        help="Output file.")
    # TODO apply this to other scripts?
    sys.exit(main(**{k: v
        for k, v in vars(parser.parse_args()).items()
        if v is not None}))
