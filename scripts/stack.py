#!/usr/bin/env python3
#
# Script to find stack usage at the function level. Will detect recursion and
# report as infinite stack usage.
#
# Example:
# ./scripts/stack.py lfs.ci lfs_util.ci -Slimit
#
# Copyright (c) 2022, The littlefs authors.
# SPDX-License-Identifier: BSD-3-Clause
#

# prevent local imports
if __name__ == "__main__":
    __import__('sys').path.pop(0)

import collections as co
import csv
import functools as ft
import io
import itertools as it
import math as mt
import os
import re
import subprocess as sp
import sys


OBJDUMP_PATH = ['objdump']


# integer fields
class RInt(co.namedtuple('RInt', 'x')):
    __slots__ = ()
    def __new__(cls, x=0):
        if isinstance(x, RInt):
            return x
        if isinstance(x, str):
            try:
                x = int(x, 0)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', x):
                    x = mt.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', x):
                    x = -mt.inf
                else:
                    raise
        if not (isinstance(x, int) or mt.isinf(x)):
            x = int(x)
        return super().__new__(cls, x)

    def __repr__(self):
        return '%s(%r)' % (self.__class__.__name__, self.x)

    def __str__(self):
        if self.x == mt.inf:
            return '∞'
        elif self.x == -mt.inf:
            return '-∞'
        else:
            return str(self.x)

    def __bool__(self):
        return bool(self.x)

    def __int__(self):
        assert not mt.isinf(self.x)
        return self.x

    def __float__(self):
        return float(self.x)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    def diff(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        diff = new - old
        if diff == +mt.inf:
            return '%7s' % '+∞'
        elif diff == -mt.inf:
            return '%7s' % '-∞'
        else:
            return '%+7d' % diff

    def ratio(self, other):
        new = self.x if self else 0
        old = other.x if other else 0
        if mt.isinf(new) and mt.isinf(old):
            return 0.0
        elif mt.isinf(new):
            return +mt.inf
        elif mt.isinf(old):
            return -mt.inf
        elif not old and not new:
            return 0.0
        elif not old:
            return +mt.inf
        else:
            return (new-old) / old

    def __pos__(self):
        return self.__class__(+self.x)

    def __neg__(self):
        return self.__class__(-self.x)

    def __abs__(self):
        return self.__class__(abs(self.x))

    def __add__(self, other):
        return self.__class__(self.x + other.x)

    def __sub__(self, other):
        return self.__class__(self.x - other.x)

    def __mul__(self, other):
        return self.__class__(self.x * other.x)

    def __truediv__(self, other):
        if not other:
            if self >= self.__class__(0):
                return self.__class__(+mt.inf)
            else:
                return self.__class__(-mt.inf)
        return self.__class__(self.x // other.x)

    def __mod__(self, other):
        return self.__class__(self.x % other.x)

# stack size results
class StackResult(co.namedtuple('StackResult', [
        'z', 'file', 'function',
        'frame', 'limit',
        'children', 'notes'])):
    _by = ['z', 'file', 'function']
    _fields = ['frame', 'limit']
    _sort = ['limit', 'frame']
    _types = {'frame': RInt, 'limit': RInt}
    _children = 'children'
    _notes = 'notes'

    __slots__ = ()
    def __new__(cls, z=0, file='', function='', frame=0, limit=0,
            children=None, notes=None):
        return super().__new__(cls, z, file, function,
                RInt(frame), RInt(limit),
                children if children is not None else [],
                notes if notes is not None else set())

    def __add__(self, other):
        return StackResult(self.z, self.file, self.function,
                self.frame + other.frame,
                max(self.limit, other.limit),
                self.children + other.children,
                self.notes | other.notes)


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

# a simple general-purpose parser class
#
# basically just because memoryview doesn't support strs
class Parser:
    def __init__(self, data, ws='\s*', ws_flags=0):
        self.data = data
        self.i = 0
        self.m = None
        # also consume whitespace
        self.ws = re.compile(ws, ws_flags)
        self.i = self.ws.match(self.data, self.i).end()

    def __repr__(self):
        if len(self.data) - self.i <= 32:
            return repr(self.data[self.i:])
        else:
            return "%s..." % repr(self.data[self.i:self.i+32])[:32]

    def __str__(self):
        return self.data[self.i:]

    def __len__(self):
        return len(self.data) - self.i

    def __bool__(self):
        return self.i != len(self.data)

    def match(self, pattern, flags=0):
        # compile so we can use the pos arg, this is still cached
        self.m = re.compile(pattern, flags).match(self.data, self.i)
        return self.m

    def group(self, *groups):
        return self.m.group(*groups)

    def chomp(self, *groups):
        g = self.group(*groups)
        self.i = self.m.end()
        # also consume whitespace
        self.i = self.ws.match(self.data, self.i).end()
        return g

    class Error(Exception):
        pass

    def chompmatch(self, pattern, flags=0, *groups):
        if not self.match(pattern, flags):
            raise Parser.Error("expected %r, found %r" % (pattern, self))
        return self.chomp(*groups)

    def unexpected(self):
        raise Parser.Error("unexpected %r" % self)

    def lookahead(self):
        # push state on the stack
        if not hasattr(self, 'stack'):
            self.stack = []
        self.stack.append((self.i, self.m))
        return self

    def consume(self):
        # pop and use new state
        self.stack.pop()

    def discard(self):
        # pop and discard new state
        self.i, self.m = self.stack.pop()

    def __enter__(self):
        return self

    def __exit__(self, et, ev, tb):
        # keep new state if no exception occured
        if et is None:
            self.consume()
        else:
            self.discard()

class CGNode(co.namedtuple('CGNode', [
        'name', 'file', 'size', 'qualifiers', 'calls'])):
    __slots__ = ()
    def __new__(cls, name, file, size, qualifiers, calls=None):
        return super().__new__(cls, name, file, size, qualifiers,
                calls if calls is not None else set())

    def __repr__(self):
        return '%s(%r, %r, %r, %r, %r)' % (
                self.__class__.__name__,
                self.name,
                self.file,
                self.size,
                self.qualifiers,
                self.calls)

def collect_callgraph(ci_path,
        **args):
    with open(ci_path) as f:
        # parse callgraph
        p = Parser(f.read())

        def p_cg(p):
            node = {}
            while True:
                # key?
                if not p.match('[^\s:{}]+'):
                    break
                k = p.chomp()
                p.chompmatch(':')

                # string?
                if p.match('"((?:\\.|[^"])*)"'):
                    v = p.chomp(1)
                # keyword?
                elif p.match('[^\s:{}]+'):
                    v = p.chomp()
                # child node?
                elif p.match('{'):
                    p.chomp()
                    v = p_cg(p)
                    p.chompmatch('}')
                else:
                    p.unexpected()

                if k not in node:
                    node[k] = []
                node[k].append(v)
            return node

        cg = p_cg(p)
        # trailing junk?
        if p:
            p.unexpected()

        # convert to something a bit more useful
        cg_ = {}
        for node in cg['graph'][0].get('node') or []:
            name = node['title'][0]
            label = node['label'][0].split('\\n')
            if len(label) < 3:
                continue
            file = label[1].split(':', 1)[0]
            m = re.match('([0-9]+) bytes \((.*)\)', label[2])
            size = int(m.group(1))
            qualifiers = [q.strip() for q in m.group(2).split(',')]
            cg_[name] = CGNode(name, file, size, qualifiers)

        for edge in cg['graph'][0].get('edge') or []:
            cg_[edge['sourcename'][0]].calls.add(edge['targetname'][0])

        return cg_

def collect_stack(ci_paths, *,
        everything=False,
        depth=1,
        **args):
    # parse the callgraphs
    cg = {}
    for ci_path in ci_paths:
        cg.update(collect_callgraph(ci_path))

    # cached function/file finder
    def nameof(node):
        # cached?
        if not hasattr(nameof, 'cache'):
            nameof.cache = {}
        if node.name in nameof.cache:
            return nameof.cache[node.name]

        # find name and file, note static functions are prefixed
        # with the file path
        name = node.name.split(':', 1)[-1]
        file = node.file

        # simplify path
        if os.path.commonpath([
                os.getcwd(),
                os.path.abspath(file)]) == os.getcwd():
            file = os.path.relpath(file)
        else:
            file = os.path.abspath(file)

        nameof.cache[node.name] = name, file
        return name, file

    # cached frame finder
    def frameof(node):
        # cached?
        if not hasattr(frameof, 'cache'):
            frameof.cache = {}
        if node.name in frameof.cache:
            return frameof.cache[node.name]

        # is our stack frame bounded?
        if ('static' not in node.qualifiers
                and 'bounded' not in node.qualifiers):
            print('warning: found unbounded stack for %s' % node.name,
                    file=sys.stderr)

        frameof.cache[node.name] = node.size
        return node.size

    # recursive+cached limit finder
    def limitof(node, seen=set()):
        # found a cycle? stop here
        if node.name in seen:
            return mt.inf
        # cached?
        if not hasattr(limitof, 'cache'):
            limitof.cache = {}
        if node.name in limitof.cache:
            return limitof.cache[node.name]

        # find maximum stack limit
        frame = frameof(node)
        limit = 0
        for call in node.calls:
            if call not in cg:
                continue
            limit = max(limit, limitof(cg[call], seen | {node.name}))

        limitof.cache[node.name] = frame + limit
        return frame + limit

    # recursive+cached children finder
    def childrenof(node, depth, seen=set()):
        # found a cycle? stop here
        if node.name in seen:
            return [], {'cycle detected'}, True
        # stop here?
        if depth < 1:
            return [], set(), False
        # cached?
        if not hasattr(childrenof, 'cache'):
            childrenof.cache = {}
        if node.name in childrenof.cache:
            return childrenof.cache[node.name]

        children, dirty = [], False
        for call in node.calls:
            if call not in cg:
                continue
            node_ = cg[call]
            name_, file_ = nameof(node_)
            frame_ = frameof(node_)
            limit_ = limitof(node_, seen | {node.name})
            children_, notes_, dirty_ = childrenof(
                    node_, depth-1, seen | {node.name})
            children.append(StackResult(
                    file=file_, function=name_, frame=frame_, limit=limit_,
                    children=children_,
                    notes=notes_))
            dirty = dirty or dirty_

        if not dirty:
            childrenof.cache[node.name] = children, set(), dirty
        return children, set(), dirty

    # find function sizes
    results = []
    for node in cg.values():
        # find name and file
        name, file = nameof(node)

        # discard internal functions
        if not everything and name.startswith('__'):
            continue

        # build result
        frame = frameof(node)
        limit = limitof(node)
        children, notes, _ = childrenof(node, depth-1)
        results.append(StackResult(
                file=file, function=name, frame=frame, limit=limit,
                children=children,
                notes=notes))

    # assign z at the end to avoid issues with caching
    def zed(results, z):
        return [r._replace(z=z, children=zed(r.children, z+1))
                for r in results]
    results = zed(results, 0)

    return results


# common folding/tabling/read/write code

class Rev(co.namedtuple('Rev', 'x')):
    __slots__ = ()
    # yes we need all of these because we're a namedtuple
    def __lt__(self, other):
        return self.x > other.x
    def __gt__(self, other):
        return self.x < other.x
    def __le__(self, other):
        return self.x >= other.x
    def __ge__(self, other):
        return self.x <= other.x

def fold(Result, results, *,
        by=None,
        defines=[],
        sort=None,
        depth=1,
        **_):
    # stop when depth hits zero
    if depth == 0:
        return []

    # organize by by
    if by is None:
        by = Result._by

    for k in it.chain(by or [], (k for k, _ in defines)):
        if k not in Result._by and k not in Result._fields:
            print("error: could not find field %r?" % k,
                    file=sys.stderr)
            sys.exit(-1)

    # filter by matching defines
    if defines:
        results_ = []
        for r in results:
            if all(str(getattr(r, k)) in vs for k, vs in defines):
                results_.append(r)
        results = results_

    # organize results into conflicts
    folding = co.OrderedDict()
    for r in results:
        name = tuple(getattr(r, k) for k in by)
        if name not in folding:
            folding[name] = []
        folding[name].append(r)

    # merge conflicts
    folded = []
    for name, rs in folding.items():
        folded.append(sum(rs[1:], start=rs[0]))

    # sort, note that python's sort is stable
    folded.sort(key=lambda r: (
            # sort by explicit sort fields
            tuple((Rev
                        if reverse ^ (not k or k in Result._fields)
                        else lambda x: x)(
                    tuple((getattr(r, k_),)
                            if getattr(r, k_) is not None
                            else ()
                        for k_ in ([k] if k else Result._sort)))
                for k, reverse in (sort or [])),
            # sort by result
            r))

    # recurse if we have recursive results
    if hasattr(Result, '_children'):
        folded = [r._replace(**{
                Result._children: fold(
                        Result, getattr(r, Result._children),
                        by=by,
                        # only filter defines at the top level!
                        sort=sort,
                        depth=depth-1)})
                    for r in folded]

    return folded

def hotify(Result, results, *,
        enumerates=None,
        depth=1,
        hot=None,
        **_):
    # note! hotifying risks confusion if you don't enumerate/have a
    # z field, since it will allow folding across recursive boundaries

    # hotify only makes sense for recursive results
    assert hasattr(Result, '_children')

    results_ = []
    for r in results:
        hot_ = []
        def recurse(results_, depth_):
            nonlocal hot_
            if not results_:
                return

            # find the hottest result
            r = min(results_, key=lambda r:
                    tuple((Rev
                                if reverse ^ (not k or k in Result._fields)
                                else lambda x: x)(
                            tuple((getattr(r, k_),)
                                    if getattr(r, k_) is not None
                                    else ()
                                for k_ in ([k] if k else Result._sort)))
                        for k, reverse in it.chain(hot, [(None, False)])))

            hot_.append(r._replace(**(
                    # enumerate?
                    ({e: len(hot_) for e in enumerates}
                            if enumerates is not None
                            else {})
                        | {Result._children: []})))

            # recurse?
            if depth_ > 1:
                recurse(getattr(r, Result._children),
                        depth_-1)

        recurse(getattr(r, Result._children), depth-1)
        results_.append(r._replace(**{Result._children: hot_}))

    return results_

def table(Result, results, diff_results=None, *,
        by=None,
        fields=None,
        sort=None,
        labels=None,
        depth=1,
        hot=None,
        diff=None,
        percent=None,
        all=False,
        compare=None,
        no_header=False,
        small_header=False,
        no_total=False,
        small_table=False,
        summary=False,
        **_):
    import builtins
    all_, all = all, builtins.all

    if by is None:
        by = Result._by
    if fields is None:
        fields = Result._fields
    types = Result._types

    # organize by name
    table = {
            ','.join(str(getattr(r, k)
                        if getattr(r, k) is not None
                        else '')
                    for k in by): r
                for r in results}
    diff_table = {
            ','.join(str(getattr(r, k)
                        if getattr(r, k) is not None
                        else '')
                    for k in by): r
                for r in diff_results or []}

    # lost results? this only happens if we didn't fold by the same
    # by field, which is an error and risks confusing results
    assert len(table) == len(results)
    if diff_results is not None:
        assert len(diff_table) == len(diff_results)

    # find compare entry if there is one
    if compare:
        compare_r = table.get(','.join(str(k) for k in compare))

    # build up our lines
    lines = []

    # header
    if not no_header:
        header = ['%s%s' % (
                    ','.join(labels if labels is not None else by),
                    ' (%d added, %d removed)' % (
                            sum(1 for n in table if n not in diff_table),
                            sum(1 for n in diff_table if n not in table))
                        if diff else '')
                if not small_header and not small_table and not summary
                    else '']
        if not diff:
            for k in fields:
                header.append(k)
        else:
            for k in fields:
                header.append('o'+k)
            for k in fields:
                header.append('n'+k)
            for k in fields:
                header.append('d'+k)
        lines.append(header)

    # delete these to try to catch typos below, we need to rebuild
    # these tables at each recursive layer
    del table
    del diff_table

    # entry helper
    def table_entry(name, r, diff_r=None):
        # prepend name
        entry = [name]

        # normal entry?
        if ((compare is None or r == compare_r)
                and not percent
                and not diff):
            for k in fields:
                entry.append(
                        (getattr(r, k).table(),
                                getattr(getattr(r, k), 'notes', lambda: [])())
                            if getattr(r, k, None) is not None
                            else types[k].none)
        # compare entry?
        elif not percent and not diff:
            for k in fields:
                entry.append(
                        (getattr(r, k).table()
                                if getattr(r, k, None) is not None
                                else types[k].none,
                            (lambda t: ['+∞%'] if t == +mt.inf
                                    else ['-∞%'] if t == -mt.inf
                                    else ['%+.1f%%' % (100*t)])(
                                types[k].ratio(
                                    getattr(r, k, None),
                                    getattr(compare_r, k, None)))))
        # percent entry?
        elif not diff:
            for k in fields:
                entry.append(
                        (getattr(r, k).table()
                                if getattr(r, k, None) is not None
                                else types[k].none,
                            (lambda t: ['+∞%'] if t == +mt.inf
                                    else ['-∞%'] if t == -mt.inf
                                    else ['%+.1f%%' % (100*t)])(
                                types[k].ratio(
                                    getattr(r, k, None),
                                    getattr(diff_r, k, None)))))
        # diff entry?
        else:
            for k in fields:
                entry.append(getattr(diff_r, k).table()
                        if getattr(diff_r, k, None) is not None
                        else types[k].none)
            for k in fields:
                entry.append(getattr(r, k).table()
                        if getattr(r, k, None) is not None
                        else types[k].none)
            for k in fields:
                entry.append(
                        (types[k].diff(
                                getattr(r, k, None),
                                getattr(diff_r, k, None)),
                            (lambda t: ['+∞%'] if t == +mt.inf
                                    else ['-∞%'] if t == -mt.inf
                                    else ['%+.1f%%' % (100*t)] if t
                                    else [])(
                                types[k].ratio(
                                    getattr(r, k, None),
                                    getattr(diff_r, k, None)))))

        # append any notes
        if hasattr(Result, '_notes') and r is not None:
            notes = sorted(getattr(r, Result._notes))
            if isinstance(entry[-1], tuple):
                entry[-1] = (entry[-1][0], entry[-1][1] + notes)
            else:
                entry[-1] = (entry[-1], notes)

        return entry

    # recursive entry helper
    def table_recurse(results_, diff_results_,
            depth_,
            prefixes=('', '', '', '')):
        # build the children table at each layer
        table_ = {
                ','.join(str(getattr(r, k)
                            if getattr(r, k) is not None
                            else '')
                        for k in by): r
                    for r in results_}
        diff_table_ = {
                ','.join(str(getattr(r, k)
                            if getattr(r, k) is not None
                            else '')
                        for k in by): r
                    for r in diff_results_ or []}
        names_ = [n
                for n in table_.keys() | diff_table_.keys()
                if diff_results is None
                    or all_
                    or any(
                        types[k].ratio(
                                getattr(table_.get(n), k, None),
                                getattr(diff_table_.get(n), k, None))
                            for k in fields)]

        # sort again, now with diff info, note that python's sort is stable
        names_.sort(key=lambda n: (
                # sort by explicit sort fields
                tuple((Rev
                            if reverse ^ (not k or k in Result._fields)
                            else lambda x: x)(
                        tuple((getattr(table_[n], k_),)
                                if getattr(table_.get(n), k_, None) is not None
                                else ()
                            for k_ in ([k] if k else Result._sort)))
                    for k, reverse in (sort or [])),
                # sort by ratio if diffing
                Rev(tuple(types[k].ratio(
                            getattr(table_.get(n), k, None),
                            getattr(diff_table_.get(n), k, None))
                        for k in fields))
                    if diff or percent
                    else (),
                # move compare entry to the top, note this can be
                # overridden by explicitly sorting by fields
                (table_.get(n) != compare_r,
                        # sort by ratio if comparing
                        Rev(tuple(
                            types[k].ratio(
                                    getattr(table_.get(n), k, None),
                                    getattr(compare_r, k, None))
                                for k in fields)))
                    if compare
                    else (),
                # sort by result
                (table_[n],) if n in table_ else (),
                # and finally by name (diffs may be missing results)
                n))

        for i, name in enumerate(names_):
            # find comparable results
            r = table_.get(name)
            diff_r = diff_table_.get(name)

            # figure out a good label
            if labels is not None:
                label = ','.join(str(getattr(r, k)
                            if getattr(r, k) is not None
                            else '')
                        for k in labels)
            else:
                label = name

            # build line
            line = table_entry(label, r, diff_r)

            # add prefixes
            line = [x if isinstance(x, tuple) else (x, []) for x in line]
            line[0] = (prefixes[0+(i==len(names_)-1)] + line[0][0], line[0][1])
            lines.append(line)

            # recurse?
            if name in table_ and depth_ > 1:
                table_recurse(
                        getattr(r, Result._children),
                        getattr(diff_r, Result._children, None),
                        depth_-1,
                        (prefixes[2+(i==len(names_)-1)] + "|-> ",
                         prefixes[2+(i==len(names_)-1)] + "'-> ",
                         prefixes[2+(i==len(names_)-1)] + "|   ",
                         prefixes[2+(i==len(names_)-1)] + "    "))

    # build entries
    if not summary:
        table_recurse(results, diff_results, depth)

    # total
    if not no_total and not (small_table and not summary):
        r = next(iter(fold(Result, results, by=[])), None)
        if diff_results is None:
            diff_r = None
        else:
            diff_r = next(iter(fold(Result, diff_results, by=[])), None)
        lines.append(table_entry('TOTAL', r, diff_r))

    # homogenize
    lines = [[x if isinstance(x, tuple) else (x, []) for x in line]
            for line in lines]

    # find the best widths, note that column 0 contains the names and is
    # handled a bit differently
    widths = co.defaultdict(lambda: 7, {0: 7})
    nwidths = co.defaultdict(lambda: 0)
    for line in lines:
        for i, x in enumerate(line):
            widths[i] = max(widths[i], ((len(x[0])+1+4-1)//4)*4-1)
            if i != len(line)-1:
                nwidths[i] = max(nwidths[i], 1+sum(2+len(n) for n in x[1]))

    # print our table
    for line in lines:
        print('%-*s  %s' % (
                widths[0], line[0][0],
                ' '.join('%*s%-*s' % (
                        widths[i], x[0],
                        nwidths[i], ' (%s)' % ', '.join(x[1]) if x[1] else '')
                    for i, x in enumerate(line[1:], 1))))

def read_csv(path, Result, *,
        depth=1,
        **_):
    with openio(path, 'r') as f:
        # csv or json? assume json starts with [
        json = (f.buffer.peek(1)[:1] == b'[')

        # read csv?
        if not json:
            results = []
            reader = csv.DictReader(f, restval='')
            for r in reader:
                if not any(k in r and r[k].strip()
                        for k in Result._fields):
                    continue
                try:
                    # note this allows by/fields to overlap
                    results.append(Result(**(
                            {k: r[k] for k in Result._by
                                    if k in r and r[k].strip()}
                                | {k: r[k] for k in Result._fields
                                    if k in r and r[k].strip()})))
                except TypeError:
                    pass
            return results

        # read json?
        else:
            import json
            def unjsonify(results, depth_):
                results_ = []
                for r in results:
                    if not any(k in r and r[k].strip()
                            for k in Result._fields):
                        continue
                    try:
                        # note this allows by/fields to overlap
                        results_.append(Result(**(
                                {k: r[k] for k in Result._by
                                        if k in r and r[k] is not None}
                                    | {k: r[k] for k in Result._fields
                                        if k in r and r[k] is not None}
                                    | ({Result._children: unjsonify(
                                            r[Result._children],
                                            depth_-1)}
                                        if hasattr(Result, '_children')
                                            and Result._children in r
                                            and r[Result._children] is not None
                                            and depth_ > 1
                                        else {})
                                    | ({Result._notes: set(r[Result._notes])}
                                        if hasattr(Result, '_notes')
                                            and Result._notes in r
                                            and r[Result._notes] is not None
                                        else {}))))
                    except TypeError:
                        pass
                return results_
            return unjsonify(json.load(f), depth)

def write_csv(path, Result, results, *,
        json=False,
        by=None,
        fields=None,
        depth=1,
        **_):
    with openio(path, 'w') as f:
        # write csv?
        if not json:
            writer = csv.DictWriter(f, list(co.OrderedDict.fromkeys(it.chain(
                    by
                        if by is not None
                        else Result._by,
                    fields
                        if fields is not None
                        else Result._fields)).keys()))
            writer.writeheader()
            for r in results:
                # note this allows by/fields to overlap
                writer.writerow(
                        {k: getattr(r, k)
                                for k in (by
                                    if by is not None
                                    else Result._by)
                                if getattr(r, k) is not None}
                            | {k: str(getattr(r, k))
                                for k in (fields
                                    if fields is not None
                                    else Result._fields)
                                if getattr(r, k) is not None})

        # write json?
        else:
            import json
            # the neat thing about json is we can include recursive results
            def jsonify(results, depth_):
                results_ = []
                for r in results:
                    # note this allows by/fields to overlap
                    results_.append(
                            {k: getattr(r, k)
                                    for k in (by
                                        if by is not None
                                        else Result._by)
                                    if getattr(r, k) is not None}
                                | {k: str(getattr(r, k))
                                    for k in (fields
                                        if fields is not None
                                        else Result._fields)
                                    if getattr(r, k) is not None}
                                | ({Result._children: jsonify(
                                        getattr(r, Result._children),
                                        depth_-1)}
                                    if hasattr(Result, '_children')
                                        and getattr(r, Result._children)
                                        and depth_ > 1
                                    else {})
                                | ({Result._notes: list(
                                        getattr(r, Result._notes))}
                                    if hasattr(Result, '_notes')
                                        and getattr(r, Result._notes)
                                    else {}))
                return results_
            json.dump(jsonify(results, depth), f,
                    separators=(',', ':'))


def main(ci_paths,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        depth=None,
        hot=None,
        **args):
    # figure out what fields we're interested in
    labels = None
    if by is None:
        if depth is not None or hot is not None:
            by = ['z', 'function']
            labels = ['function']
        else:
            by = ['function']

    if fields is None:
        fields = ['frame', 'limit']

    # figure out depth
    if depth is None:
        depth = mt.inf if hot else 1
    elif depth == 0:
        depth = mt.inf

    # find sizes
    if not args.get('use', None):
        # not enough info?
        if not ci_paths:
            print("error: no *.ci files?",
                    file=sys.stderr)
            sys.exit(1)

        # collect info
        results = collect_stack(ci_paths,
                depth=depth,
                **args)

    else:
        results = read_csv(args['use'], StackResult,
                depth=depth,
                **args)

    # fold
    results = fold(StackResult, results,
            by=by,
            defines=defines,
            depth=depth)

    # hotify?
    if hot:
        results = hotify(StackResult, results,
                depth=depth,
                hot=hot)

    # write results to CSV/JSON
    if args.get('output'):
        write_csv(args['output'], StackResult, results,
                by=by,
                fields=fields,
                depth=depth,
                **args)
    if args.get('output_json'):
        write_csv(args['output_json'], StackResult, results, json=True,
                by=by,
                fields=fields,
                depth=depth,
                **args)

    # find previous results?
    diff_results = None
    if args.get('diff') or args.get('percent'):
        try:
            diff_results = read_csv(
                    args.get('diff') or args.get('percent'),
                    StackResult,
                    depth=depth,
                    **args)
        except FileNotFoundError:
            diff_results = []

        # fold
        diff_results = fold(StackResult, diff_results,
                by=by,
                defines=defines,
                depth=depth)

    # print table
    if not args.get('quiet'):
        table(StackResult, results, diff_results,
                by=by,
                fields=fields,
                sort=sort,
                labels=labels,
                depth=depth,
                **args)

    # error on recursion
    if args.get('error_on_recursion') and any(
            mt.isinf(float(r.limit)) for r in results):
        sys.exit(2)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Find stack usage at the function level.",
            allow_abbrev=False)
    parser.add_argument(
            'ci_paths',
            nargs='*',
            help="Input *.ci files.")
    parser.add_argument(
            '-v', '--verbose',
            action='store_true',
            help="Output commands that run behind the scenes.")
    parser.add_argument(
            '-q', '--quiet',
            action='store_true',
            help="Don't show anything, useful with -o.")
    parser.add_argument(
            '-o', '--output',
            help="Specify CSV file to store results.")
    parser.add_argument(
            '-O', '--output-json',
            help="Specify JSON file to store results. This may contain "
                "recursive info.")
    parser.add_argument(
            '-u', '--use',
            help="Don't parse anything, use this CSV/JSON file.")
    parser.add_argument(
            '-d', '--diff',
            help="Specify CSV/JSON file to diff against.")
    parser.add_argument(
            '-p', '--percent',
            help="Specify CSV/JSON file to diff against, but only show "
                "percentage change, not a full diff.")
    parser.add_argument(
            '-c', '--compare',
            type=lambda x: tuple(v.strip() for v in x.split(',')),
            help="Compare results to the row matching this by pattern.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Show all, not just the ones that changed.")
    parser.add_argument(
            '-b', '--by',
            action='append',
            choices=StackResult._by,
            help="Group by this field.")
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action='append',
            choices=StackResult._fields,
            help="Show this field.")
    parser.add_argument(
            '-D', '--define',
            dest='defines',
            action='append',
            type=lambda x: (
                lambda k, vs: (
                    k.strip(),
                    {v.strip() for v in vs.split(',')})
                )(*x.split('=', 1)),
            help="Only include results where this field is this value.")
    class AppendSort(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.sort is None:
                namespace.sort = []
            namespace.sort.append((value, option in {'-S', '--reverse-sort'}))
    parser.add_argument(
            '-s', '--sort',
            nargs='?',
            action=AppendSort,
            help="Sort by this field.")
    parser.add_argument(
            '-S', '--reverse-sort',
            nargs='?',
            action=AppendSort,
            help="Sort by this field, but backwards.")
    parser.add_argument(
            '-z', '--depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of function calls to show. 0 shows all calls unless "
                "we find a cycle. Defaults to 0.")
    class AppendHot(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.hot is None:
                namespace.hot = []
            namespace.hot.append((value, option in {'-R', '--reverse-hot'}))
    parser.add_argument(
            '-r', '--hot',
            nargs='?',
            action=AppendHot,
            help="Show only the hot path for each function call. Can "
                "optionally provide fields like sort.")
    parser.add_argument(
            '-R', '--reverse-hot',
            nargs='?',
            action=AppendHot,
            help="Like -r/--hot, but backwards.")
    parser.add_argument(
            '--no-header',
            action='store_true',
            help="Don't show the header.")
    parser.add_argument(
            '--small-header',
            action='store_true',
            help="Don't show by field names.")
    parser.add_argument(
            '--no-total',
            action='store_true',
            help="Don't show the total.")
    parser.add_argument(
            '-Q', '--small-table',
            action='store_true',
            help="Equivalent to --small-header + --no-total.")
    parser.add_argument(
            '-Y', '--summary',
            action='store_true',
            help="Only show the total.")
    parser.add_argument(
            '--everything',
            action='store_true',
            help="Include builtin and libc specific symbols.")
    parser.add_argument(
            '-e', '--error-on-recursion',
            action='store_true',
            help="Error if any functions are recursive.")
    parser.add_argument(
            '--objdump-path',
            type=lambda x: x.split(),
            default=OBJDUMP_PATH,
            help="Path to the objdump executable, may include flags. "
                "Defaults to %r." % OBJDUMP_PATH)
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
