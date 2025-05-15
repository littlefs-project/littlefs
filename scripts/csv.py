#!/usr/bin/env python3
#
# Script to manipulate CSV files.
#
# Example:
# ./scripts/csv.py lfs.code.csv lfs.stack.csv \
#         -bfunction -fcode -fstack='max(stack)'
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
import itertools as it
import math as mt
import os
import re
import sys


# various field types

# integer fields
class CsvInt(co.namedtuple('CsvInt', 'a')):
    __slots__ = ()
    def __new__(cls, a=0):
        if isinstance(a, CsvInt):
            return a
        if isinstance(a, str):
            try:
                a = int(a, 0)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', a):
                    a = mt.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', a):
                    a = -mt.inf
                else:
                    raise
        if not (isinstance(a, int) or mt.isinf(a)):
            a = int(a)
        return super().__new__(cls, a)

    def __repr__(self):
        return '%s(%r)' % (self.__class__.__name__, self.a)

    def __str__(self):
        if self.a == mt.inf:
            return '∞'
        elif self.a == -mt.inf:
            return '-∞'
        else:
            return str(self.a)

    def __bool__(self):
        return bool(self.a)

    def __int__(self):
        assert not mt.isinf(self.a)
        return self.a

    def __float__(self):
        return float(self.a)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    def diff(self, other):
        new = self.a if self else 0
        old = other.a if other else 0
        diff = new - old
        if diff == +mt.inf:
            return '%7s' % '+∞'
        elif diff == -mt.inf:
            return '%7s' % '-∞'
        else:
            return '%+7d' % diff

    def ratio(self, other):
        new = self.a if self else 0
        old = other.a if other else 0
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
        return self.__class__(+self.a)

    def __neg__(self):
        return self.__class__(-self.a)

    def __abs__(self):
        return self.__class__(abs(self.a))

    def __add__(self, other):
        return self.__class__(self.a + other.a)

    def __sub__(self, other):
        return self.__class__(self.a - other.a)

    def __mul__(self, other):
        return self.__class__(self.a * other.a)

    def __truediv__(self, other):
        if not other:
            if self >= self.__class__(0):
                return self.__class__(+mt.inf)
            else:
                return self.__class__(-mt.inf)
        return self.__class__(self.a // other.a)

    def __mod__(self, other):
        return self.__class__(self.a % other.a)

# float fields
class CsvFloat(co.namedtuple('CsvFloat', 'a')):
    __slots__ = ()
    def __new__(cls, a=0.0):
        if isinstance(a, CsvFloat):
            return a
        if isinstance(a, str):
            try:
                a = float(a)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', a):
                    a = mt.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', a):
                    a = -mt.inf
                else:
                    raise
        if not isinstance(a, float):
            a = float(a)
        return super().__new__(cls, a)

    def __repr__(self):
        return '%s(%r)' % (self.__class__.__name__, self.a)

    def __str__(self):
        if self.a == mt.inf:
            return '∞'
        elif self.a == -mt.inf:
            return '-∞'
        else:
            return '%.1f' % self.a

    def __bool__(self):
        return bool(self.a)

    def __int__(self):
        return int(self.a)

    def __float__(self):
        return float(self.a)

    none = '%7s' % '-'
    def table(self):
        return '%7s' % (self,)

    def diff(self, other):
        new = self.a if self else 0
        old = other.a if other else 0
        diff = new - old
        if diff == +mt.inf:
            return '%7s' % '+∞'
        elif diff == -mt.inf:
            return '%7s' % '-∞'
        else:
            return '%+7.1f' % diff

    def ratio(self, other):
        new = self.a if self else 0
        old = other.a if other else 0
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
        return self.__class__(+self.a)

    def __neg__(self):
        return self.__class__(-self.a)

    def __abs__(self):
        return self.__class__(abs(self.a))

    def __add__(self, other):
        return self.__class__(self.a + other.a)

    def __sub__(self, other):
        return self.__class__(self.a - other.a)

    def __mul__(self, other):
        return self.__class__(self.a * other.a)

    def __truediv__(self, other):
        if not other:
            if self >= self.__class__(0):
                return self.__class__(+mt.inf)
            else:
                return self.__class__(-mt.inf)
        return self.__class__(self.a / other.a)

    def __mod__(self, other):
        return self.__class__(self.a % other.a)

# fractional fields, a/b
class CsvFrac(co.namedtuple('CsvFrac', 'a,b')):
    __slots__ = ()
    def __new__(cls, a=0, b=None):
        if isinstance(a, CsvFrac) and b is None:
            return a
        if isinstance(a, str) and b is None:
            a, b = a.split('/', 1)
        if b is None:
            b = a
        return super().__new__(cls, CsvInt(a), CsvInt(b))

    def __repr__(self):
        return '%s(%r, %r)' % (self.__class__.__name__, self.a.a, self.b.a)

    def __str__(self):
        return '%s/%s' % (self.a, self.b)

    def __bool__(self):
        return bool(self.a)

    def __int__(self):
        return int(self.a)

    def __float__(self):
        return float(self.a)

    none = '%11s' % '-'
    def table(self):
        return '%11s' % (self,)

    def notes(self):
        if self.b.a == 0 and self.a.a == 0:
            t = 1.0
        elif self.b.a == 0:
            t = mt.copysign(mt.inf, self.a.a)
        else:
            t = self.a.a / self.b.a
        return ['∞%' if t == +mt.inf
                else '-∞%' if t == -mt.inf
                else '%.1f%%' % (100*t)]

    def diff(self, other):
        new_a, new_b = self if self else (CsvInt(0), CsvInt(0))
        old_a, old_b = other if other else (CsvInt(0), CsvInt(0))
        return '%11s' % ('%s/%s' % (
                new_a.diff(old_a).strip(),
                new_b.diff(old_b).strip()))

    def ratio(self, other):
        new_a, new_b = self if self else (CsvInt(0), CsvInt(0))
        old_a, old_b = other if other else (CsvInt(0), CsvInt(0))
        new = new_a.a/new_b.a if new_b.a else 1.0
        old = old_a.a/old_b.a if old_b.a else 1.0
        return new - old

    def __pos__(self):
        return self.__class__(+self.a, +self.b)

    def __neg__(self):
        return self.__class__(-self.a, -self.b)

    def __abs__(self):
        return self.__class__(abs(self.a), abs(self.b))

    def __add__(self, other):
        return self.__class__(self.a + other.a, self.b + other.b)

    def __sub__(self, other):
        return self.__class__(self.a - other.a, self.b - other.b)

    def __mul__(self, other):
        return self.__class__(self.a * other.a, self.b * other.b)

    def __truediv__(self, other):
        return self.__class__(self.a / other.a, self.b / other.b)

    def __mod__(self, other):
        return self.__class__(self.a % other.a, self.b % other.b)

    def __eq__(self, other):
        self_a, self_b = self if self.b.a else (CsvInt(1), CsvInt(1))
        other_a, other_b = other if other.b.a else (CsvInt(1), CsvInt(1))
        return self_a * other_b == other_a * self_b

    def __ne__(self, other):
        return not self.__eq__(other)

    def __lt__(self, other):
        self_a, self_b = self if self.b.a else (CsvInt(1), CsvInt(1))
        other_a, other_b = other if other.b.a else (CsvInt(1), CsvInt(1))
        return self_a * other_b < other_a * self_b

    def __gt__(self, other):
        return self.__class__.__lt__(other, self)

    def __le__(self, other):
        return not self.__gt__(other)

    def __ge__(self, other):
        return not self.__lt__(other)


# various fold operations
class CsvSum:
    def __call__(self, xs):
        return sum(xs[1:], start=xs[0])

class CsvProd:
    def __call__(self, xs):
        return mt.prod(xs[1:], start=xs[0])

class CsvMin:
    def __call__(self, xs):
        return min(xs)

class CsvMax:
    def __call__(self, xs):
        return max(xs)

class CsvAvg:
    def __call__(self, xs):
        return CsvFloat(sum(float(x) for x in xs) / len(xs))

class CsvStddev:
    def __call__(self, xs):
        avg = sum(float(x) for x in xs) / len(xs)
        return CsvFloat(mt.sqrt(
                sum((float(x) - avg)**2 for x in xs) / len(xs)))

class CsvGMean:
    def __call__(self, xs):
        return CsvFloat(mt.prod(float(x) for x in xs)**(1/len(xs)))

class CsvGStddev:
    def __call__(self, xs):
        gmean = mt.prod(float(x) for x in xs)**(1/len(xs))
        return CsvFloat(
                mt.exp(mt.sqrt(
                        sum(mt.log(float(x)/gmean)**2 for x in xs) / len(xs)))
                    if gmean else mt.inf)


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

# a lazily-evaluated field expression
class CsvExpr:
    # expr parsing/typechecking/etc errors
    class Error(Exception):
        pass

    # expr node base class
    class Expr:
        def __init__(self, *args):
            for k, v in zip('abcdefghijklmnopqrstuvwxyz', args):
                setattr(self, k, v)

        def __iter__(self):
            return (getattr(self, k)
                    for k in it.takewhile(
                        lambda k: hasattr(self, k),
                        'abcdefghijklmnopqrstuvwxyz'))

        def __len__(self):
            return sum(1 for _ in self)

        def __repr__(self):
            return '%s(%s)' % (
                    self.__class__.__name__,
                    ','.join(repr(v) for v in self))

        def fields(self):
            return set(it.chain.from_iterable(v.fields() for v in self))

        def type(self, types={}):
            t = self.a.type(types)
            if not all(t == v.type(types) for v in it.islice(self, 1, None)):
                raise CsvExpr.Error("mismatched types? %r" % self)
            return t

        def fold(self, types={}):
            return self.a.fold(types)

        def eval(self, fields={}):
            return self.a.eval(fields)

    # expr nodes

    # literal exprs
    class IntLit(Expr):
        def fields(self):
            return set()

        def type(self, types={}):
            return CsvInt

        def fold(self, types={}):
            return CsvSum, CsvInt

        def eval(self, fields={}):
            return self.a

    class FloatLit(Expr):
        def fields(self):
            return set()

        def type(self, types={}):
            return CsvFloat

        def fold(self, types={}):
            return CsvSum, CsvFloat

        def eval(self, fields={}):
            return self.a

    # field expr
    class Field(Expr):
        def fields(self):
            return {self.a}

        def type(self, types={}):
            if self.a not in types:
                raise CsvExpr.Error("untyped field? %s" % self.a)
            return types[self.a]

        def fold(self, types={}):
            if self.a not in types:
                raise CsvExpr.Error("unfoldable field? %s" % self.a)
            return CsvSum, types[self.a]

        def eval(self, fields={}):
            if self.a not in fields:
                raise CsvExpr.Error("unknown field? %s" % self.a)
            return fields[self.a]

    # func expr helper
    def func(funcs):
        def func(name, args="a"):
            def func(f):
                f._func = name
                f._fargs = args
                funcs[f._func] = f
                return f
            return func
        return func

    funcs = {}
    func = func(funcs)

    # type exprs
    @func('int', 'a')
    class Int(Expr):
        """Convert to an integer"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            return CsvInt(self.a.eval(fields))

    @func('float', 'a')
    class Float(Expr):
        """Convert to a float"""
        def type(self, types={}):
            return CsvFloat

        def eval(self, fields={}):
            return CsvFloat(self.a.eval(fields))

    @func('frac', 'a[, b]')
    class Frac(Expr):
        """Convert to a fraction"""
        def type(self, types={}):
            return CsvFrac

        def eval(self, fields={}):
            if len(self) == 1:
                return CsvFrac(self.a.eval(fields))
            else:
                return CsvFrac(self.a.eval(fields), self.b.eval(fields))

    # fold exprs
    @func('sum', 'a[, ...]')
    class Sum(Expr):
        """Find the sum of this column or fields"""
        def fold(self, types={}):
            if len(self) == 1:
                return CsvSum, self.a.type(types)
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return CsvSum()([v.eval(fields) for v in self])

    @func('prod', 'a[, ...]')
    class Prod(Expr):
        """Find the product of this column or fields"""
        def fold(self, types={}):
            if len(self) == 1:
                return Prod, self.a.type(types)
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return Prod()([v.eval(fields) for v in self])

    @func('min', 'a[, ...]')
    class Min(Expr):
        """Find the minimum of this column or fields"""
        def fold(self, types={}):
            if len(self) == 1:
                return CsvMin, self.a.type(types)
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return CsvMin()([v.eval(fields) for v in self])

    @func('max', 'a[, ...]')
    class Max(Expr):
        """Find the maximum of this column or fields"""
        def fold(self, types={}):
            if len(self) == 1:
                return CsvMax, self.a.type(types)
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return CsvMax()([v.eval(fields) for v in self])

    @func('avg', 'a[, ...]')
    class Avg(Expr):
        """Find the average of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return CsvFloat

        def fold(self, types={}):
            if len(self) == 1:
                return CsvAvg, CsvFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return CsvAvg()([v.eval(fields) for v in self])

    @func('stddev', 'a[, ...]')
    class Stddev(Expr):
        """Find the standard deviation of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return CsvFloat

        def fold(self, types={}):
            if len(self) == 1:
                return CsvStddev, CsvFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return CsvStddev()([v.eval(fields) for v in self])

    @func('gmean', 'a[, ...]')
    class GMean(Expr):
        """Find the geometric mean of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return CsvFloat

        def fold(self, types={}):
            if len(self) == 1:
                return CsvGMean, CsvFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return CsvGMean()([v.eval(fields) for v in self])

    @func('gstddev', 'a[, ...]')
    class GStddev(Expr):
        """Find the geometric stddev of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return CsvFloat

        def fold(self, types={}):
            if len(self) == 1:
                return CsvGStddev, CsvFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return CsvGStddev()([v.eval(fields) for v in self])

    # functions
    @func('ratio', 'a')
    class Ratio(Expr):
        """Ratio of a fraction as a float"""
        def type(self, types={}):
            return CsvFloat

        def eval(self, fields={}):
            v = CsvFrac(self.a.eval(fields))
            if not float(v.b) and not float(v.a):
                return CsvFloat(1)
            elif not float(v.b):
                return CsvFloat(mt.copysign(mt.inf, float(v.a)))
            else:
                return CsvFloat(float(v.a) / float(v.b))

    @func('total', 'a')
    class Total(Expr):
        """Total part of a fraction"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            return CsvFrac(self.a.eval(fields)).b

    @func('abs', 'a')
    class Abs(Expr):
        """Absolute value"""
        def eval(self, fields={}):
            return abs(self.a.eval(fields))

    @func('ceil', 'a')
    class Ceil(Expr):
        """Round up to nearest integer"""
        def type(self, types={}):
            return CsvFloat

        def eval(self, fields={}):
            return CsvFloat(mt.ceil(float(self.a.eval(fields))))

    @func('floor', 'a')
    class Floor(Expr):
        """Round down to nearest integer"""
        def type(self, types={}):
            return CsvFloat

        def eval(self, fields={}):
            return CsvFloat(mt.floor(float(self.a.eval(fields))))

    @func('log', 'a[, b]')
    class Log(Expr):
        """Log of a with base e, or log of a with base b"""
        def type(self, types={}):
            return CsvFloat

        def eval(self, fields={}):
            if len(self) == 1:
                return CsvFloat(mt.log(
                        float(self.a.eval(fields))))
            else:
                return CsvFloat(mt.log(
                        float(self.a.eval(fields)),
                        float(self.b.eval(fields))))

    @func('pow', 'a[, b]')
    class Pow(Expr):
        """e to the power of a, or a to the power of b"""
        def type(self, types={}):
            return CsvFloat

        def eval(self, fields={}):
            if len(self) == 1:
                return CsvFloat(mt.exp(
                        float(self.a.eval(fields))))
            else:
                return CsvFloat(mt.pow(
                        float(self.a.eval(fields)),
                        float(self.b.eval(fields))))

    @func('sqrt', 'a')
    class Sqrt(Expr):
        """Square root"""
        def type(self, types={}):
            return CsvFloat

        def eval(self, fields={}):
            return CsvFloat(mt.sqrt(float(self.a.eval(fields))))

    @func('isint', 'a')
    class IsInt(Expr):
        """1 if a is an integer, otherwise 0"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            if isinstance(self.a.eval(fields), CsvInt):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @func('isfloat', 'a')
    class IsFloat(Expr):
        """1 if a is a float, otherwise 0"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            if isinstance(self.a.eval(fields), CsvFloat):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @func('isfrac', 'a')
    class IsFrac(Expr):
        """1 if a is a fraction, otherwise 0"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            if isinstance(self.a.eval(fields), CsvFrac):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @func('isinf', 'a')
    class IsInf(Expr):
        """1 if a is infinite, otherwise 0"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            if mt.isinf(self.a.eval(fields)):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @func('isnan')
    class IsNan(Expr):
        """1 if a is a NAN, otherwise 0"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            if mt.isnan(self.a.eval(fields)):
                return CsvInt(1)
            else:
                return CsvInt(0)

    # unary expr helper
    def uop(uops):
        def uop(op):
            def uop(f):
                f._uop = op
                uops[f._uop] = f
                return f
            return uop
        return uop

    uops = {}
    uop = uop(uops)

    # unary ops
    @uop('+')
    class Pos(Expr):
        """Non-negation"""
        def eval(self, fields={}):
            return +self.a.eval(fields)

    @uop('-')
    class Neg(Expr):
        """Negation"""
        def eval(self, fields={}):
            return -self.a.eval(fields)

    @uop('!')
    class NotNot(Expr):
        """1 if a is zero, otherwise 0"""
        def type(self, types={}):
            return CsvInt

        def eval(self, fields={}):
            if self.a.eval(fields):
                return CsvInt(0)
            else:
                return CsvInt(1)

    # binary expr help
    def bop(bops, bprecs):
        def bop(op, prec):
            def bop(f):
                f._bop = op
                f._bprec = prec
                bops[f._bop] = f
                bprecs[f._bop] = f._bprec
                return f
            return bop
        return bop

    bops = {}
    bprecs = {}
    bop = bop(bops, bprecs)

    # binary ops
    @bop('*', 10)
    class Mul(Expr):
        """Multiplication"""
        def eval(self, fields={}):
            return self.a.eval(fields) * self.b.eval(fields)

    @bop('/', 10)
    class Div(Expr):
        """Division"""
        def eval(self, fields={}):
            return self.a.eval(fields) / self.b.eval(fields)

    @bop('%', 10)
    class Mod(Expr):
        """Modulo"""
        def eval(self, fields={}):
            return self.a.eval(fields) % self.b.eval(fields)

    @bop('+', 9)
    class Add(Expr):
        """Addition"""
        def eval(self, fields={}):
            a = self.a.eval(fields)
            b = self.b.eval(fields)
            return a + b

    @bop('-', 9)
    class Sub(Expr):
        """Subtraction"""
        def eval(self, fields={}):
            return self.a.eval(fields) - self.b.eval(fields)

    @bop('==', 4)
    class Eq(Expr):
        """1 if a equals b, otherwise 0"""
        def eval(self, fields={}):
            if self.a.eval(fields) == self.b.eval(fields):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @bop('!=', 4)
    class Ne(Expr):
        """1 if a does not equal b, otherwise 0"""
        def eval(self, fields={}):
            if self.a.eval(fields) != self.b.eval(fields):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @bop('<', 4)
    class Lt(Expr):
        """1 if a is less than b"""
        def eval(self, fields={}):
            if self.a.eval(fields) < self.b.eval(fields):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @bop('<=', 4)
    class Le(Expr):
        """1 if a is less than or equal to b"""
        def eval(self, fields={}):
            if self.a.eval(fields) <= self.b.eval(fields):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @bop('>', 4)
    class Gt(Expr):
        """1 if a is greater than b"""
        def eval(self, fields={}):
            if self.a.eval(fields) > self.b.eval(fields):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @bop('>=', 4)
    class Ge(Expr):
        """1 if a is greater than or equal to b"""
        def eval(self, fields={}):
            if self.a.eval(fields) >= self.b.eval(fields):
                return CsvInt(1)
            else:
                return CsvInt(0)

    @bop('&&', 3)
    class AndAnd(Expr):
        """b if a is non-zero, otherwise a"""
        def eval(self, fields={}):
            a = self.a.eval(fields)
            if a:
                return self.b.eval(fields)
            else:
                return a

    @bop('||', 2)
    class OrOr(Expr):
        """a if a is non-zero, otherwise b"""
        def eval(self, fields={}):
            a = self.a.eval(fields)
            if a:
                return a
            else:
                return self.b.eval(fields)

    # ternary expr help
    def top(tops, tprecs):
        def top(op_a, op_b, prec):
            def top(f):
                f._top = (op_a, op_b)
                f._tprec = prec
                tops[f._top] = f
                tprecs[f._top] = f._tprec
                return f
            return top
        return top

    tops = {}
    tprecs = {}
    top = top(tops, tprecs)

    # ternary ops
    @top('?', ':', 1)
    class IfElse(Expr):
        """b if a is non-zero, otherwise c"""
        def type(self, types={}):
            t = self.b.type(types)
            u = self.c.type(types)
            if t != u:
                raise CsvExpr.Error("mismatched types? %r" % self)
            return t

        def fold(self, types={}):
            return self.b.fold(types)

        def eval(self, fields={}):
            a = self.a.eval(fields)
            if a:
                return self.b.eval(fields)
            else:
                return self.c.eval(fields)

    # show expr help text
    @classmethod
    def help(cls):
        print('uops:')
        for op in cls.uops.keys():
            print('  %-21s %s' % ('%sa' % op, CsvExpr.uops[op].__doc__))
        print('bops:')
        for op in cls.bops.keys():
            print('  %-21s %s' % ('a %s b' % op, CsvExpr.bops[op].__doc__))
        print('tops:')
        for op in cls.tops.keys():
            print('  %-21s %s' % ('a %s b %s c' % op, CsvExpr.tops[op].__doc__))
        print('funcs:')
        for func in cls.funcs.keys():
            print('  %-21s %s' % (
                    '%s(%s)' % (func, CsvExpr.funcs[func]._fargs),
                    CsvExpr.funcs[func].__doc__))

    # parse an expr
    def __init__(self, expr):
        self.expr = expr.strip()

        # parse the expression into a tree
        def p_expr(p, prec=0):
            # parens
            if p.match('\('):
                p.chomp()
                a = p_expr(p)
                if not p.match('\)'):
                    raise CsvExpr.Error("mismatched parens? %s" % p)
                p.chomp()

            # floats
            elif p.match('[+-]?(?:[_0-9]*\.[_0-9eE]|nan)'):
                a = CsvExpr.FloatLit(CsvFloat(p.chomp()))

            # ints
            elif p.match('[+-]?(?:[0-9][bBoOxX]?[_0-9a-fA-F]*|∞|inf)'):
                a = CsvExpr.IntLit(CsvInt(p.chomp()))

            # fields/functions
            elif p.match('[_a-zA-Z][_a-zA-Z0-9]*'):
                a = p.chomp()

                if p.match('\('):
                    p.chomp()
                    if a not in CsvExpr.funcs:
                        raise CsvExpr.Error("unknown function? %s" % a)
                    args = []
                    while True:
                        b = p_expr(p)
                        args.append(b)
                        if p.match(','):
                            p.chomp()
                            continue
                        else:
                            if not p.match('\)'):
                                raise CsvExpr.Error("mismatched parens? %s" % p)
                            p.chomp()
                            a = CsvExpr.funcs[a](*args)
                            break
                else:
                    a = CsvExpr.Field(a)

            # unary ops
            elif any(p.match(re.escape(op)) for op in CsvExpr.uops.keys()):
                # sort by len to avoid ambiguities
                for op in sorted(CsvExpr.uops.keys(), reverse=True):
                    if p.match(re.escape(op)):
                        p.chomp()
                        a = p_expr(p, mt.inf)
                        a = CsvExpr.uops[op](a)
                        break
                else:
                    assert False

            # unknown expr?
            else:
                raise CsvExpr.Error("unknown expr? %s" % p)

            # parse tail
            while True:
                # binary ops
                if any(p.match(re.escape(op))
                            and prec < CsvExpr.bprecs[op]
                        for op in CsvExpr.bops.keys()):
                    # sort by len to avoid ambiguities
                    for op in sorted(CsvExpr.bops.keys(), reverse=True):
                        if (p.match(re.escape(op))
                                and prec < CsvExpr.bprecs[op]):
                            p.chomp()
                            b = p_expr(p, CsvExpr.bprecs[op])
                            a = CsvExpr.bops[op](a, b)
                            break
                    else:
                        assert False

                # ternary ops, these are intentionally right associative
                elif any(p.match(re.escape(op[0]))
                            and prec <= CsvExpr.tprecs[op]
                        for op in CsvExpr.tops.keys()):
                    # sort by len to avoid ambiguities
                    for op in sorted(CsvExpr.tops.keys(), reverse=True):
                        if (p.match(re.escape(op[0]))
                                and prec <= CsvExpr.tprecs[op]):
                            p.chomp()
                            b = p_expr(p, CsvExpr.tprecs[op])
                            if not p.match(re.escape(op[1])):
                                raise CsvExpr.Error(
                                        'mismatched ternary op? %s %s' % op)
                            p.chomp()
                            c = p_expr(p, CsvExpr.tprecs[op])
                            a = CsvExpr.tops[op](a, b, c)
                            break
                    else:
                        assert False

                # no tail
                else:
                    return a

        try:
            p = Parser(self.expr)
            self.tree = p_expr(p)
            if p:
                raise CsvExpr.Error("trailing expr? %s" % p)

        except (CsvExpr.Error, ValueError) as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursively find all fields
    def fields(self):
        try:
            return self.tree.fields()
        except CsvExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursively find the type
    def type(self, types={}):
        try:
            return self.tree.type(types)
        except CsvExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursively find the fold operation
    def fold(self, types={}):
        try:
            return self.tree.fold(types)
        except CsvExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursive evaluate the expr
    def eval(self, fields={}):
        try:
            return self.tree.eval(fields)
        except CsvExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)


# parse %-escaped strings
#
# attrs can override __getitem__ for lazy attr generation
def punescape(s, attrs=None):
    pattern = re.compile(
        '%[%n]'
            '|' '%x..'
            '|' '%u....'
            '|' '%U........'
            '|' '%\((?P<field>[^)]*)\)'
                '(?P<format>[+\- #0-9\.]*[sdboxXfFeEgG])')
    def unescape(m):
        if m.group()[1] == '%': return '%'
        elif m.group()[1] == 'n': return '\n'
        elif m.group()[1] == 'x': return chr(int(m.group()[2:], 16))
        elif m.group()[1] == 'u': return chr(int(m.group()[2:], 16))
        elif m.group()[1] == 'U': return chr(int(m.group()[2:], 16))
        elif m.group()[1] == '(':
            if attrs is not None:
                try:
                    v = attrs[m.group('field')]
                except KeyError:
                    return m.group()
            else:
                return m.group()
            f = m.group('format')
            if f[-1] in 'dboxX':
                if isinstance(v, str):
                    v = dat(v, 0)
                v = int(v)
            elif f[-1] in 'fFeEgG':
                if isinstance(v, str):
                    v = dat(v, 0)
                v = float(v)
            else:
                f = ('<' if '-' in f else '>') + f.replace('-', '')
                v = str(v)
            # note we need Python's new format syntax for binary
            return ('{:%s}' % f).format(v)
        else: assert False

    return re.sub(pattern, unescape, s)

def punescape_help():
    print('mods:')
    print('  %-21s %s' % ('%%', 'A literal % character'))
    print('  %-21s %s' % ('%n', 'A newline'))
    print('  %-21s %s' % (
            '%xaa', 'A character with the hex value aa'))
    print('  %-21s %s' % (
            '%uaaaa', 'A character with the hex value aaaa'))
    print('  %-21s %s' % (
            '%Uaaaaaaaa', 'A character with the hex value aaaaaaaa'))
    print('  %-21s %s' % (
            '%(field)s', 'An existing field formatted as a string'))
    print('  %-21s %s' % (
            '%(field)[dboxX]', 'An existing field formatted as an integer'))
    print('  %-21s %s' % (
            '%(field)[fFeEgG]', 'An existing field formatted as a float'))


# open with '-' for stdin/stdout
def openio(path, mode='r', buffering=-1):
    import os
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

def collect_csv(csv_paths, *,
        depth=1,
        children=None,
        notes=None,
        **_):
    # collect both results and fields from CSV files
    fields = co.OrderedDict()
    results = []
    for path in csv_paths:
        try:
            with openio(path) as f:
                # csv or json? assume json starts with [
                is_json = (f.buffer.peek(1)[:1] == b'[')

                # read csv?
                if not is_json:
                    reader = csv.DictReader(f, restval='')
                    # collect fields
                    fields.update((k, True) for k in reader.fieldnames or [])
                    for r in reader:
                        # strip and drop empty fields
                        r_ = {k: v.strip()
                                for k, v in r.items()
                                if k not in {'notes'}
                                    and v.strip()}
                        # special handling for notes field
                        if notes is not None and notes in r:
                            r_[notes] = set(r[notes].split(','))
                        results.append(r_)

                # read json?
                else:
                    import json
                    def unjsonify(results, depth_):
                        results_ = []
                        for r in results:
                            # collect fields
                            fields.update((k, True) for k in r.keys())
                            # convert to strings, we'll reparse these later
                            #
                            # this may seem a bit backwards, but it keeps
                            # the rest of the script simpler if we pretend
                            # everything came from a csv
                            r_ = {k: str(v).strip()
                                    for k, v in r.items()
                                    if k not in {'children', 'notes'}
                                        and str(v).strip()}
                            # special handling for children field
                            if (children is not None
                                    and children in r
                                    and r[children] is not None
                                    and depth_ > 1):
                                r_[children] = unjsonify(
                                        r[children],
                                        depth_-1)
                            # special handling for notes field
                            if (notes is not None
                                    and notes in r
                                    and r[notes] is not None):
                                r_[notes] = set(r[notes])
                            results_.append(r_)
                        return results_
                    results.extend(unjsonify(json.load(f), depth))

        except FileNotFoundError:
            pass

    return list(fields.keys()), results

def compile(fields_, results,
        by=None,
        fields=None,
        mods=[],
        exprs=[],
        sort=None,
        children=None,
        hot=None,
        notes=None,
        prefix=None,
        **_):
    # default to no prefix
    if prefix is None:
        prefix = ''

    by = by.copy()
    fields = fields.copy()

    # make sure sort/hot fields are included
    for k, reverse in it.chain(sort or [], hot or []):
        # this defaults to typechecking sort/hot fields, which is
        # probably safer, if you really want to sort by strings you
        # can use --by + --label to create hidden by fields
        if k and k not in by and k not in fields:
            fields.append(k)
    # make sure all expr targets are in fields so they get typechecked
    # correctly
    for k, _ in exprs:
        if k not in fields:
            fields.append(k)

    # we only really care about the last mod/expr for each field
    mods = {k: mod for k, mod in mods}
    exprs = {k: expr for k, expr in exprs}

    # find best type for all fields used by field exprs
    fields__ = set(it.chain.from_iterable(
            exprs[k].fields() if k in exprs else [k]
                for k in fields))
    types__ = {}
    for k in fields__:
        # check if dependency is in original fields
        #
        # it's tempting to also allow enumerate fields here, but this
        # currently doesn't work when hotifying
        if prefix+k not in fields_:
            print("error: no field %r?" % k,
                    file=sys.stderr)
            sys.exit(2)

        for t in [CsvInt, CsvFloat, CsvFrac]:
            for r in results:
                if prefix+k in r and r[prefix+k].strip():
                    try:
                        t(r[prefix+k])
                    except ValueError:
                        break
            else:
                types__[k] = t
                break
        else:
            print("error: no type matches field %r?" % k,
                    file=sys.stderr)
            sys.exit(2)

    # typecheck exprs, note these may reference input fields with
    # the same name, which is why we only do a single eval pass
    types___ = types__.copy()
    for k, expr in exprs.items():
        types___[k] = expr.type(types__)

    # foldcheck field exprs
    folds___ = {k: (CsvSum, t) for k, v in types__.items()}
    for k, expr in exprs.items():
        folds___[k] = expr.fold(types__)
    folds___ = {k: (f(), t) for k, (f, t) in folds___.items()}

    # create result class
    def __new__(cls, **r):
        r_ = r.copy()
        # evaluate types, strip prefix
        for k, t in types__.items():
            r_[k] = t(r[prefix+k]) if prefix+k in r else t()

        r__ = r_.copy()
        # evaluate exprs
        for k, expr in exprs.items():
            r__[k] = expr.eval(r_)
        # evaluate mods
        for k, m in mods.items():
            r__[k] = punescape(m, r_)

        # return result
        return cls.__mro__[1].__new__(cls, **(
                {k: r__.get(k, '') for k in by}
                    | {k: ([r__[k]], 1) if k in r__ else ([], 0)
                        for k in fields}
                    | ({children: r[children] if children in r else []}
                        if children is not None else {})
                    | ({notes: r[notes] if notes in r else set()}
                        if notes is not None else {})))

    def __add__(self, other):
        # reuse lists if possible
        def extend(a, b):
            if len(a[0]) == a[1]:
                a[0].extend(b[0][:b[1]])
                return (a[0], a[1] + b[1])
            else:
                return (a[0][:a[1]] + b[0][:b[1]], a[1] + b[1])

        # lazily fold results
        return self.__class__.__mro__[1].__new__(self.__class__, **(
                {k: getattr(self, k) for k in by}
                    | {k: extend(
                            object.__getattribute__(self, k),
                            object.__getattribute__(other, k))
                        for k in fields}
                    | ({children: self.children + other.children}
                        if children is not None else {})
                    | ({notes: self.notes | other.notes}
                        if notes is not None else {})))

    def __getattribute__(self, k):
        # lazily fold results on demand, this avoids issues with fold
        # operations that depend on the number of results
        if k in fields:
            v = object.__getattribute__(self, k)
            if v[1]:
                return folds___[k][0](v[0][:v[1]])
            else:
                return None
        return object.__getattribute__(self, k)

    return type(
            'Result',
            (co.namedtuple('Result', list(co.OrderedDict.fromkeys(it.chain(
                by,
                fields,
                [children] if children is not None else [],
                [notes] if notes is not None else [])).keys())),),
            dict(
                __slots__=(),
                __new__=__new__,
                __add__=__add__,
                __getattribute__=__getattribute__,
                _by=by,
                _fields=fields,
                _sort=fields,
                _types={k: t for k, (_, t) in folds___.items()},
                _mods=mods,
                _exprs=exprs,
                **{'_children': children} if children is not None else {},
                **{'_notes': notes} if notes is not None else {}))

def homogenize(Result, results, *,
        enumerates=None,
        defines=[],
        depth=1,
        **_):
    # this just converts all (possibly recursive) results to our
    # result type
    results_ = []
    for r in results:
        # filter by matching defines
        #
        # we do this here instead of in fold to be consistent with
        # evaluation order of exprs/mods/etc, note this isn't really
        # inconsistent with the other scripts, since they don't really
        # evaluate anything
        if not all(k in r and str(r[k]) in vs for k, vs in defines):
            continue

        # append a result
        results_.append(Result(**(
                r
                    # enumerate?
                    | ({e: len(results_) for e in enumerates}
                        if enumerates is not None
                        else {})
                    # recurse?
                    | ({Result._children: homogenize(
                            Result, r[Result._children],
                            # only filter defines at the top level!
                            enumerates=enumerates,
                            depth=depth-1)}
                        if hasattr(Result, '_children')
                            and Result._children in r
                            and r[Result._children] is not None
                            and depth > 1
                        else {}))))
    return results_


# common folding/tabling/read/write code

class Rev(co.namedtuple('Rev', 'a')):
    __slots__ = ()
    # yes we need all of these because we're a namedtuple
    def __lt__(self, other):
        return self.a > other.a
    def __gt__(self, other):
        return self.a < other.a
    def __le__(self, other):
        return self.a >= other.a
    def __ge__(self, other):
        return self.a <= other.a

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
        percent=False,
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
                        if diff_results is not None and not percent else '')
                if not small_header and not small_table and not summary
                    else '']
        if diff_results is None or percent:
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
                and diff_results is None):
            for k in fields:
                entry.append(
                        (getattr(r, k).table(),
                                getattr(getattr(r, k), 'notes', lambda: [])())
                            if getattr(r, k, None) is not None
                            else types[k].none)
        # compare entry?
        elif diff_results is None:
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
        elif percent:
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
                next(
                    tuple((Rev
                                    if reverse ^ (not k or k in Result._fields)
                                    else lambda x: x)(
                                tuple((getattr(r_, k_),)
                                        if getattr(r_, k_) is not None
                                        else ()
                                    for k_ in ([k] if k else Result._sort)))
                            for k, reverse in (sort or []))
                        for r_ in [table_.get(n), diff_table_.get(n)]
                        if r_ is not None),
                # sort by ratio if diffing
                Rev(tuple(types[k].ratio(
                            getattr(table_.get(n), k, None),
                            getattr(diff_table_.get(n), k, None))
                        for k in fields))
                    if diff_results is not None
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
                label = next(
                        ','.join(str(getattr(r_, k)
                                    if getattr(r_, k) is not None
                                    else '')
                                for k in labels)
                            for r_ in [r, diff_r]
                            if r_ is not None)
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
        prefix=None,
        **_):
    # prefix? this only applies to field fields
    if prefix is None:
        if hasattr(Result, '_prefix'):
            prefix = '%s_' % Result._prefix
        else:
            prefix = ''

    by = Result._by
    fields = Result._fields

    with openio(path, 'r') as f:
        # csv or json? assume json starts with [
        json = (f.buffer.peek(1)[:1] == b'[')

        # read csv?
        if not json:
            results = []
            reader = csv.DictReader(f, restval='')
            for r in reader:
                if not any(prefix+k in r and r[prefix+k].strip()
                        for k in fields):
                    continue
                try:
                    # note this allows by/fields to overlap
                    results.append(Result(**(
                            {k: r[k] for k in by
                                    if k in r
                                        and r[k].strip()}
                                | {k: r[prefix+k] for k in fields
                                    if prefix+k in r
                                        and r[prefix+k].strip()})))
                except TypeError:
                    pass
            return results

        # read json?
        else:
            import json
            def unjsonify(results, depth_):
                results_ = []
                for r in results:
                    if not any(prefix+k in r and r[prefix+k].strip()
                            for k in fields):
                        continue
                    try:
                        # note this allows by/fields to overlap
                        results_.append(Result(**(
                                {k: r[k] for k in by
                                        if k in r
                                            and r[k] is not None}
                                    | {k: r[prefix+k] for k in fields
                                        if prefix+k in r
                                            and r[prefix+k] is not None}
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
        prefix=None,
        **_):
    # prefix? this only applies to field fields
    if prefix is None:
        if hasattr(Result, '_prefix'):
            prefix = '%s_' % Result._prefix
        else:
            prefix = ''

    if by is None:
        by = Result._by
    if fields is None:
        fields = Result._fields

    with openio(path, 'w') as f:
        # write csv?
        if not json:
            writer = csv.DictWriter(f, list(
                    co.OrderedDict.fromkeys(it.chain(
                        by,
                        (prefix+k for k in fields))).keys()))
            writer.writeheader()
            for r in results:
                # note this allows by/fields to overlap
                writer.writerow(
                        {k: getattr(r, k)
                                for k in by
                                if getattr(r, k) is not None}
                            | {prefix+k: str(getattr(r, k))
                                for k in fields
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
                                    for k in by
                                    if getattr(r, k) is not None}
                                | {prefix+k: str(getattr(r, k))
                                    for k in fields
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


def main(csv_paths, *,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        depth=None,
        children=None,
        hot=None,
        notes=None,
        **args):
    # show mod help text?
    if args.get('help_mods'):
        return punescape_help()
    # show expr help text?
    if args.get('help_exprs'):
        return CsvExpr.help()

    if by is None and fields is None:
        print("error: needs --by or --fields to figure out fields",
                file=sys.stderr)
        sys.exit(-1)

    if children is not None:
        if len(children) > 1:
            print("error: multiple --children fields currently not supported",
                    file=sys.stderr)
            sys.exit(-1)
        children = children[0]

    if notes is not None:
        if len(notes) > 1:
            print("error: multiple --notes fields currently not supported",
                    file=sys.stderr)
            sys.exit(-1)
        notes = notes[0]

    # recursive results imply --children
    if (depth is not None or hot is not None) and children is None:
        children = 'children'

    # figure out depth
    if depth is None:
        depth = mt.inf if hot else 1
    elif depth == 0:
        depth = mt.inf

    # separate out enumerates/mods/exprs
    #
    # enumerate enumerates: -ia
    # by supports mods: -ba=%(b)s
    # fields/sort/etc supports exprs: -fa=b+c
    #
    enumerates = [k
            for (k, v), hidden in (by or [])
                if v == enumerate]
    mods = [(k, v)
            for k, v in it.chain(
                ((k, v) for (k, v), hidden in (by or [])
                    if v != enumerate))
            if v is not None]
    exprs = [(k, v)
            for k, v in it.chain(
                ((k, v) for (k, v), hidden in (fields or [])),
                ((k, v) for (k, v), reverse in (sort or [])),
                ((k, v) for (k, v), reverse in (hot or [])))
            if v is not None]
    labels = None
    if by is not None:
        labels = [k for (k, v), hidden in by if not hidden]
        by = [k for (k, v), hidden in by]
    if fields is not None:
        fields = [k for (k, v), hidden in fields
                if not hidden
                    or args.get('output')
                    or args.get('output_json')]
    if sort is not None:
        sort = [(k, reverse) for (k, v), reverse in sort]
    if hot is not None:
        hot = [(k, reverse) for (k, v), reverse in hot]

    # find results
    if not args.get('use', None):
        # not enough info?
        if not csv_paths:
            print("error: no *.csv files?",
                    file=sys.stderr)
            sys.exit(1)

        # collect info
        fields_, results = collect_csv(csv_paths,
                depth=depth,
                children=children,
                notes=notes,
                **args)

    else:
        # use is just an alias but takes priority
        fields_, results = collect_csv([args['use']],
                depth=depth,
                children=children,
                notes=notes,
                **args)

    # if by not specified, guess it's anything not in fields/defines/exprs/etc
    if not by:
        by = [k for k in fields_
                if k not in (fields or [])
                    and not any(k == k_ for k_, _ in defines)
                    and not any(k == k_ for k_, _ in (sort or []))
                    and k != children
                    and not any(k == k_ for k_, _ in (hot or []))
                    and k != notes
                    and not any(k == k_
                        for _, expr in exprs
                        for k_ in expr.fields())]

    # if fields not specified, guess it's anything not in by/defines/exprs/etc
    if not fields:
        fields = [k for k in fields_
                if k not in (by or [])
                    and not any(k == k_ for k_, _ in defines)
                    and not any(k == k_ for k_, _ in (sort or []))
                    and k != children
                    and not any(k == k_ for k_, _ in (hot or []))
                    and k != notes
                    and not any(k == k_
                        for _, expr in exprs
                        for k_ in expr.fields())]

    # build result type
    Result = compile(fields_, results,
            by=by,
            fields=fields,
            mods=mods,
            exprs=exprs,
            sort=sort,
            children=children,
            hot=hot,
            notes=notes,
            **args)

    # homogenize
    results = homogenize(Result, results,
            enumerates=enumerates,
            defines=defines,
            depth=depth)

    # fold
    results = fold(Result, results,
            by=by,
            depth=depth)

    # hotify?
    if hot:
        results = hotify(Result, results,
                enumerates=enumerates,
                depth=depth,
                hot=hot)

    # find previous results?
    diff_results = None
    if args.get('diff'):
        # note! don't use read_csv here
        #
        # it's tempting now that we have a Result type, but we want to
        # make sure all the defines/exprs/mods/etc are evaluated in the
        # same order
        try:
            _, diff_results = collect_csv(
                    [args.get('diff')],
                    depth=depth,
                    children=children,
                    notes=notes,
                    **args)
        except FileNotFoundError:
            diff_results = []

        # homogenize
        diff_results = homogenize(Result, diff_results,
                enumerates=enumerates,
                defines=defines,
                depth=depth)

        # fold
        diff_results = fold(Result, diff_results,
                by=by,
                depth=depth)

        # hotify?
        if hot:
            diff_results = hotify(Result, diff_results,
                    enumerates=enumerates,
                    depth=depth,
                    hot=hot)

    # write results to JSON
    if args.get('output_json'):
        write_csv(args['output_json'], Result, results, json=True,
                by=by,
                fields=fields,
                depth=depth,
                **args)
    # write results to CSV
    elif args.get('output'):
        write_csv(args['output'], Result, results,
                by=by,
                fields=fields,
                depth=depth,
                **args)
    # print table
    elif not args.get('quiet'):
        table(Result, results, diff_results,
                by=by,
                fields=fields,
                sort=sort,
                labels=labels,
                depth=depth,
                **args)


if __name__ == "__main__":
    import argparse
    import sys
    parser = argparse.ArgumentParser(
            description="Script to manipulate CSV files.",
            allow_abbrev=False)
    parser.add_argument(
            'csv_paths',
            nargs='*',
            help="Input *.csv files.")
    parser.add_argument(
            '--help-mods',
            action='store_true',
            help="Show what %% modifiers are available.")
    parser.add_argument(
            '--help-exprs',
            action='store_true',
            help="Show what field exprs are available.")
    parser.add_argument(
            '-q', '--quiet',
            action='store_true',
            help="Don't show anything, useful when checking for errors.")
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
            action='store_true',
            help="Only show percentage change, not a full diff.")
    parser.add_argument(
            '-c', '--compare',
            type=lambda x: tuple(v.strip() for v in x.split(',')),
            help="Compare results to the row matching this by pattern.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Show all, not just the ones that changed.")
    class AppendBy(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.by is None:
                namespace.by = []
            namespace.by.append((value, option in {
                    '-B', '--hidden-by',
                    '-I', '--hidden-enumerate'}))
    parser.add_argument(
            '-i', '--enumerate',
            action=AppendBy,
            nargs='?',
            type=lambda x: (x, enumerate),
            const=('i', enumerate),
            help="Enumerate results with this field. This will prevent "
                "result folding.")
    parser.add_argument(
            '-I', '--hidden-enumerate',
            action=AppendBy,
            nargs='?',
            type=lambda x: (x, enumerate),
            const=('i', enumerate),
            help="Like -i/--enumerate, but hidden from the table renderer.")
    parser.add_argument(
            '-b', '--by',
            action=AppendBy,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    v.strip() if v is not None else None)
                )(*x.split('=', 1)),
            help="Group by this field. This does _not_ support expressions, "
                "but can be assigned a string with %% modifiers.")
    parser.add_argument(
            '-B', '--hidden-by',
            action=AppendBy,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    v.strip() if v is not None else None)
                )(*x.split('=', 1)),
            help="Like -b/--by, but hidden from the table renderer.")
    class AppendField(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.fields is None:
                namespace.fields = []
            namespace.fields.append((value, option in {
                    '-F', '--hidden-field'}))
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action=AppendField,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    CsvExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            help="Show this field. Can include an expression of the form "
                "field=expr.")
    parser.add_argument(
            '-F', '--hidden-field',
            dest='fields',
            action=AppendField,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    v.strip() if v is not None else None)
                )(*x.split('=', 1)),
            help="Like -f/--field, but hidden from the table renderer.")
    parser.add_argument(
            '-D', '--define',
            dest='defines',
            action='append',
            type=lambda x: (
                lambda k, vs: (
                    k.strip(),
                    {v.strip() for v in vs.split(',')})
                )(*x.split('=', 1)),
            help="Only include results where this field is this value. May "
                "include comma-separated options.")
    class AppendSort(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.sort is None:
                namespace.sort = []
            namespace.sort.append((value, option in {'-S', '--reverse-sort'}))
    parser.add_argument(
            '-s', '--sort',
            nargs='?',
            action=AppendSort,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    CsvExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            const=(None, None),
            help="Sort by this field. Can include an expression of the form "
                "field=expr.")
    parser.add_argument(
            '-S', '--reverse-sort',
            nargs='?',
            action=AppendSort,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    CsvExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            const=(None, None),
            help="Sort by this field, but backwards. Can include an expression "
                "of the form field=expr.")
    parser.add_argument(
            '-z', '--depth',
            nargs='?',
            type=lambda x: int(x, 0),
            const=0,
            help="Depth of function calls to show. 0 shows all calls unless "
                "we find a cycle. Defaults to 0.")
    parser.add_argument(
            '-Z', '--children',
            nargs='?',
            const='children',
            action='append',
            help="Field to use for recursive results. This expects a list "
                "and really only works with JSON input.")
    class AppendHot(argparse.Action):
        def __call__(self, parser, namespace, value, option):
            if namespace.hot is None:
                namespace.hot = []
            namespace.hot.append((value, option in {'-R', '--reverse-hot'}))
    parser.add_argument(
            '-r', '--hot',
            nargs='?',
            action=AppendHot,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    CsvExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            const=(None, None),
            help="Show only the hot path for each function call. Can "
                "optionally provide fields like sort. Can include an "
                "expression in the form of field=expr.")
    parser.add_argument(
            '-R', '--reverse-hot',
            nargs='?',
            action=AppendHot,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    CsvExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            const=(None, None),
            help="Like -r/--hot, but backwards.")
    parser.add_argument(
            '-N', '--notes',
            nargs='?',
            const='notes',
            action='append',
            help="Field to use for notes.")
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
            '--prefix',
            help="Prefix to use for fields in CSV/JSON output.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
