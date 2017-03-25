#!/usr/bin/env python

import re
import sys
import subprocess
import os

def generate(test):
    with open("tests/template.fmt") as file:
        template = file.read()

    lines = []

    for line in test:
        if '=>' in line:
            test, expect = line.strip().strip(';').split('=>')
            lines.append('res = {test};'.format(test=test.strip()))
            lines.append('test_assert("{name}", res, {expect});'.format(
                    name = re.match('\w*', test.strip()).group(),
                    expect = expect.strip()))
        else:
            lines.append(line.strip())

    with open('test.c', 'w') as file:
        file.write(template.format(tests='\n'.join(4*' ' + l for l in lines)))

def compile():
    os.environ['DEBUG'] = '1'
    os.environ['CFLAGS'] = '-Werror'
    subprocess.check_call(['make', '--no-print-directory', '-s'], env=os.environ)

def execute():
    subprocess.check_call(["./lfs"])

def main(test=None):
    if test:
        with open(test) as file:
            generate(file)
    else:
        generate(sys.stdin)

    compile()
    execute()

if __name__ == "__main__":
    main(*sys.argv[1:])
