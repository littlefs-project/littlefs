#!/usr/bin/env python2

import re
import sys
import subprocess
import os


def generate(test):
    with open("scripts/template.fmt") as file:
        template = file.read()

    haslines = 'TEST_LINE' in os.environ and 'TEST_FILE' in os.environ

    lines = []
    for offset, line in enumerate(
            re.split('(?<=(?:.;| [{}]))\n', test.read())):
        match = re.match('((?: *\n)*)( *)(.*)=>(.*);',
                line, re.DOTALL | re.MULTILINE)
        if match:
            preface, tab, test, expect = match.groups()
            lines.extend(['']*preface.count('\n'))
            lines.append(tab+'test_assert({test}, {expect});'.format(
                test=test.strip(), expect=expect.strip()))
        else:
            lines.append(line)

    # Create test file
    with open('test.c', 'w') as file:
        if 'TEST_LINE' in os.environ and 'TEST_FILE' in os.environ:
            lines.insert(0, '#line %d "%s"' % (
                    int(os.environ['TEST_LINE']) + 1,
                    os.environ['TEST_FILE']))
            lines.append('#line %d "test.c"' % (
                    template[:template.find('{tests}')].count('\n')
                    + len(lines) + 2))

        file.write(template.format(tests='\n'.join(lines)))

    # Remove build artifacts to force rebuild
    try:
        os.remove('test.o')
        os.remove('lfs')
    except OSError:
        pass

def compile():
    subprocess.check_call([
            os.environ.get('MAKE', 'make'),
            '--no-print-directory', '-s'])

def execute():
    if 'EXEC' in os.environ:
        subprocess.check_call([os.environ['EXEC'], "./lfs"])
    else:
        subprocess.check_call(["./lfs"])

def main(test=None):
    try:
        if test and not test.startswith('-'):
            with open(test) as file:
                generate(file)
        else:
            generate(sys.stdin)

        compile()

        if test == '-s':
            sys.exit(1)

        execute()

    except subprocess.CalledProcessError:
        # Python stack trace is counterproductive, just exit
        sys.exit(2)
    except KeyboardInterrupt:
        # Python stack trace is counterproductive, just exit
        sys.exit(3)

if __name__ == "__main__":
    main(*sys.argv[1:])
