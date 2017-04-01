#!/usr/bin/env python

import re
import sys
import subprocess
import os

def generate(test):
    with open("tests/template.fmt") as file:
        template = file.read()

    lines = []
    for line in re.split('(?<=[;{}])\n', test.read()):
        match = re.match('(?: *\n)*( *)(.*)=>(.*);', line, re.MULTILINE)
        if match:
            tab, test, expect = match.groups()
            lines.append(tab+'res = {test};'.format(test=test.strip()))
            lines.append(tab+'test_assert("{name}", res, {expect});'.format(
                    name = re.match('\w*', test.strip()).group(),
                    expect = expect.strip()))
        else:
            lines.append(line)

    with open('test.c', 'w') as file:
        file.write(template.format(tests='\n'.join(lines)))

def compile():
    os.environ['CFLAGS'] = os.environ.get('CFLAGS', '') + ' -Werror'
    subprocess.check_call(['make', '--no-print-directory', '-s'], env=os.environ)

def execute():
    subprocess.check_call(["./lfs"])

def main(test=None):
    if test and not test.startswith('-'):
        with open(test) as file:
            generate(file)
    else:
        generate(sys.stdin)

    compile()

    if test == '-s':
        sys.exit(1)

    execute()

if __name__ == "__main__":
    main(*sys.argv[1:])
