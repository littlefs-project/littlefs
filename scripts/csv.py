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

# float fields
class RFloat(co.namedtuple('RFloat', 'x')):
    __slots__ = ()
    def __new__(cls, x=0.0):
        if isinstance(x, RFloat):
            return x
        if isinstance(x, str):
            try:
                x = float(x)
            except ValueError:
                # also accept +-∞ and +-inf
                if re.match('^\s*\+?\s*(?:∞|inf)\s*$', x):
                    x = mt.inf
                elif re.match('^\s*-\s*(?:∞|inf)\s*$', x):
                    x = -mt.inf
                else:
                    raise
        if not isinstance(x, float):
            x = float(x)
        return super().__new__(cls, x)

    def __str__(self):
        if self.x == mt.inf:
            return '∞'
        elif self.x == -mt.inf:
            return '-∞'
        else:
            return '%.1f' % self.x

    def __bool__(self):
        return bool(self.x)

    def __int__(self):
        return int(self.x)

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
            return '%+7.1f' % diff

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
        return self.__class__(self.x / other.x)

    def __mod__(self, other):
        return self.__class__(self.x % other.x)

# fractional fields, a/b
class RFrac(co.namedtuple('RFrac', 'a,b')):
    __slots__ = ()
    def __new__(cls, a=0, b=None):
        if isinstance(a, RFrac) and b is None:
            return a
        if isinstance(a, str) and b is None:
            a, b = a.split('/', 1)
        if b is None:
            b = a
        return super().__new__(cls, RInt(a), RInt(b))

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
        t = self.a.x/self.b.x if self.b.x else 1.0
        return ['∞%' if t == +mt.inf
                else '-∞%' if t == -mt.inf
                else '%.1f%%' % (100*t)]

    def diff(self, other):
        new_a, new_b = self if self else (RInt(0), RInt(0))
        old_a, old_b = other if other else (RInt(0), RInt(0))
        return '%11s' % ('%s/%s' % (
                new_a.diff(old_a).strip(),
                new_b.diff(old_b).strip()))

    def ratio(self, other):
        new_a, new_b = self if self else (RInt(0), RInt(0))
        old_a, old_b = other if other else (RInt(0), RInt(0))
        new = new_a.x/new_b.x if new_b.x else 1.0
        old = old_a.x/old_b.x if old_b.x else 1.0
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
        self_a, self_b = self if self.b.x else (RInt(1), RInt(1))
        other_a, other_b = other if other.b.x else (RInt(1), RInt(1))
        return self_a * other_b == other_a * self_b

    def __ne__(self, other):
        return not self.__eq__(other)

    def __lt__(self, other):
        self_a, self_b = self if self.b.x else (RInt(1), RInt(1))
        other_a, other_b = other if other.b.x else (RInt(1), RInt(1))
        return self_a * other_b < other_a * self_b

    def __gt__(self, other):
        return self.__class__.__lt__(other, self)

    def __le__(self, other):
        return not self.__gt__(other)

    def __ge__(self, other):
        return not self.__lt__(other)


# various fold operations
class RSum:
    def __call__(self, xs):
        return sum(xs[1:], start=xs[0])

class RProd:
    def __call__(self, xs):
        return mt.prod(xs[1:], start=xs[0])

class RMin:
    def __call__(self, xs):
        return min(xs)

class RMax:
    def __call__(self, xs):
        return max(xs)

class RAvg:
    def __call__(self, xs):
        return RFloat(sum(float(x) for x in xs) / len(xs))

class RStddev:
    def __call__(self, xs):
        avg = sum(float(x) for x in xs) / len(xs)
        return RFloat(mt.sqrt(sum((float(x) - avg)**2 for x in xs) / len(xs)))

class RGMean:
    def __call__(self, xs):
        return RFloat(mt.prod(float(x) for x in xs)**(1/len(xs)))

class RGStddev:
    def __call__(self, xs):
        gmean = mt.prod(float(x) for x in xs)**(1/len(xs))
        return RFloat(
                mt.exp(mt.sqrt(
                        sum(mt.log(float(x)/gmean)**2 for x in xs) / len(xs)))
                    if gmean else mt.inf)


# a lazily-evaluated field expression
class RExpr:
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
                raise RExpr.Error("mismatched types? %r" % self)
            return t

        def fold(self, types={}):
            return self.a.fold(types)

        def eval(self, fields={}):
            return self.a.eval(fields)

    # expr nodes

    # literal exprs
    class StrLit(Expr):
        def fields(self):
            return set()

        def eval(self, fields={}):
            return self.a

    class IntLit(Expr):
        def fields(self):
            return set()

        def type(self, types={}):
            return RInt

        def fold(self, types={}):
            return RSum, RInt

        def eval(self, fields={}):
            return self.a

    class FloatLit(Expr):
        def fields(self):
            return set()

        def type(self, types={}):
            return RFloat

        def fold(self, types={}):
            return RSum, RFloat

        def eval(self, fields={}):
            return self.a

    # field expr
    class Field(Expr):
        def fields(self):
            return {self.a}

        def type(self, types={}):
            if self.a not in types:
                raise RExpr.Error("untyped field? %s" % self.a)
            return types[self.a]

        def fold(self, types={}):
            if self.a not in types:
                raise RExpr.Error("unfoldable field? %s" % self.a)
            return RSum, types[self.a]

        def eval(self, fields={}):
            if self.a not in fields:
                raise RExpr.Error("unknown field? %s" % self.a)
            return fields[self.a]

    # func expr helper
    def func(name, args="a"):
        def func(f):
            f._func = name
            f._fargs = args
            return f
        return func

    class Funcs:
        @ft.cache
        def __get__(self, _, cls):
            return {x._func: x
                    for x in cls.__dict__.values()
                    if hasattr(x, '_func')}
    funcs = Funcs()

    # type exprs
    @func('int', 'a')
    class Int(Expr):
        """Convert to an integer"""
        def type(self, types={}):
            return RInt

        def eval(self, fields={}):
            return RInt(self.a.eval(fields))

    @func('float', 'a')
    class Float(Expr):
        """Convert to a float"""
        def type(self, types={}):
            return RFloat

        def eval(self, fields={}):
            return RFloat(self.a.eval(fields))

    @func('frac', 'a[, b]')
    class Frac(Expr):
        """Convert to a fraction"""
        def type(self, types={}):
            return RFrac

        def eval(self, fields={}):
            if len(self) == 1:
                return RFrac(self.a.eval(fields))
            else:
                return RFrac(self.a.eval(fields), self.b.eval(fields))

    # fold exprs
    @func('sum', 'a[, ...]')
    class Sum(Expr):
        """Find the sum of this column or fields"""
        def fold(self, types={}):
            if len(self) == 1:
                return RSum, self.a.type(types)
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return RSum()([v.eval(fields) for v in self])

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
                return RMin, self.a.type(types)
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return RMin()([v.eval(fields) for v in self])

    @func('max', 'a[, ...]')
    class Max(Expr):
        """Find the maximum of this column or fields"""
        def fold(self, types={}):
            if len(self) == 1:
                return RMax, self.a.type(types)
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return RMax()([v.eval(fields) for v in self])

    @func('avg', 'a[, ...]')
    class Avg(Expr):
        """Find the average of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return RFloat

        def fold(self, types={}):
            if len(self) == 1:
                return RAvg, RFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return RAvg()([v.eval(fields) for v in self])

    @func('stddev', 'a[, ...]')
    class Stddev(Expr):
        """Find the standard deviation of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return RFloat

        def fold(self, types={}):
            if len(self) == 1:
                return RStddev, RFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return RStddev()([v.eval(fields) for v in self])

    @func('gmean', 'a[, ...]')
    class GMean(Expr):
        """Find the geometric mean of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return RFloat

        def fold(self, types={}):
            if len(self) == 1:
                return RGMean, RFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return RGMean()([v.eval(fields) for v in self])

    @func('gstddev', 'a[, ...]')
    class GStddev(Expr):
        """Find the geometric stddev of this column or fields"""
        def type(self, types={}):
            if len(self) == 1:
                return self.a.type(types)
            else:
                return RFloat

        def fold(self, types={}):
            if len(self) == 1:
                return RGStddev, RFloat
            else:
                return self.a.fold(types)

        def eval(self, fields={}):
            if len(self) == 1:
                return self.a.eval(fields)
            else:
                return RGStddev()([v.eval(fields) for v in self])

    # functions
    @func('ratio', 'a')
    class Ratio(Expr):
        """Ratio of a fraction as a float"""
        def type(self, types={}):
            return RFloat

        def eval(self, fields={}):
            v = RFrac(self.a.eval(fields))
            if not float(v.b):
                return RFloat(1)
            else:
                return RFloat(float(v.a) / float(v.b))

    @func('total', 'a')
    class Total(Expr):
        """Total part of a fraction"""
        def type(self, types={}):
            return RInt

        def eval(self, fields={}):
            return RFrac(self.a.eval(fields)).b

    @func('abs', 'a')
    class Abs(Expr):
        """Absolute value"""
        def eval(self, fields={}):
            return abs(self.a.eval(fields))

    @func('ceil', 'a')
    class Ceil(Expr):
        """Round up to nearest integer"""
        def type(self, types={}):
            return RFloat

        def eval(self, fields={}):
            return RFloat(mt.ceil(float(self.a.eval(fields))))

    @func('floor', 'a')
    class Floor(Expr):
        """Round down to nearest integer"""
        def type(self, types={}):
            return RFloat

        def eval(self, fields={}):
            return RFloat(mt.floor(float(self.a.eval(fields))))

    @func('log', 'a[, b]')
    class Log(Expr):
        """Log of a with base e, or log of a with base b"""
        def type(self, types={}):
            return RFloat

        def eval(self, fields={}):
            if len(self) == 1:
                return RFloat(mt.log(
                        float(self.a.eval(fields))))
            else:
                return RFloat(mt.log(
                        float(self.a.eval(fields)),
                        float(self.b.eval(fields))))

    @func('pow', 'a[, b]')
    class Pow(Expr):
        """e to the power of a, or a to the power of b"""
        def type(self, types={}):
            return RFloat

        def eval(self, fields={}):
            if len(self) == 1:
                return RFloat(mt.exp(
                        float(self.a.eval(fields))))
            else:
                return RFloat(mt.pow(
                        float(self.a.eval(fields)),
                        float(self.b.eval(fields))))

    @func('sqrt', 'a')
    class Sqrt(Expr):
        """Square root"""
        def type(self, types={}):
            return RFloat

        def eval(self, fields={}):
            return RFloat(mt.sqrt(float(self.a.eval(fields))))

    @func('isint', 'a')
    class IsInt(Expr):
        """1 if a is an integer, otherwise 0"""
        def type(self, types={}):
            return RInt

        def eval(self, fields={}):
            if isinstance(self.a.eval(fields), RInt):
                return RInt(1)
            else:
                return RInt(0)

    @func('isfloat', 'a')
    class IsFloat(Expr):
        """1 if a is a float, otherwise 0"""
        def type(self, types={}):
            return RInt

        def eval(self, fields={}):
            if isinstance(self.a.eval(fields), RFloat):
                return RInt(1)
            else:
                return RInt(0)

    @func('isfrac', 'a')
    class IsFrac(Expr):
        """1 if a is a fraction, otherwise 0"""
        def type(self, types={}):
            return RInt

        def eval(self, fields={}):
            if isinstance(self.a.eval(fields), RFrac):
                return RInt(1)
            else:
                return RInt(0)

    @func('isinf', 'a')
    class IsInf(Expr):
        """1 if a is infinite, otherwise 0"""
        def type(self, types={}):
            return RInt

        def eval(self, fields={}):
            if mt.isinf(self.a.eval(fields)):
                return RInt(1)
            else:
                return RInt(0)

    @func('isnan')
    class IsNan(Expr):
        """1 if a is a NAN, otherwise 0"""
        def type(self, types={}):
            return RInt

        def eval(self, fields={}):
            if mt.isnan(self.a.eval(fields)):
                return RInt(1)
            else:
                return RInt(0)

    # unary expr helper
    def uop(op):
        def uop(f):
            f._uop = op
            return f
        return uop

    class UOps:
        @ft.cache
        def __get__(self, _, cls):
            return {x._uop: x
                    for x in cls.__dict__.values()
                    if hasattr(x, '_uop')}
    uops = UOps()

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
            return RInt

        def eval(self, fields={}):
            if self.a.eval(fields):
                return RInt(0)
            else:
                return RInt(1)

    # binary expr help
    def bop(op, prec):
        def bop(f):
            f._bop = op
            f._bprec = prec
            return f
        return bop

    class BOps:
        @ft.cache
        def __get__(self, _, cls):
            return {x._bop: x
                    for x in cls.__dict__.values()
                    if hasattr(x, '_bop')}
    bops = BOps()

    class BPrecs:
        @ft.cache
        def __get__(self, _, cls):
            return {x._bop: x._bprec
                    for x in cls.__dict__.values()
                    if hasattr(x, '_bop')}
    bprecs = BPrecs()

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
            if isinstance(a, str) or isinstance(b, str):
                return str(a) + str(b)
            else:
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
                return RInt(1)
            else:
                return RInt(0)

    @bop('!=', 4)
    class Ne(Expr):
        """1 if a does not equal b, otherwise 0"""
        def eval(self, fields={}):
            if self.a.eval(fields) != self.b.eval(fields):
                return RInt(1)
            else:
                return RInt(0)

    @bop('<', 4)
    class Lt(Expr):
        """1 if a is less than b"""
        def eval(self, fields={}):
            if self.a.eval(fields) < self.b.eval(fields):
                return RInt(1)
            else:
                return RInt(0)

    @bop('<=', 4)
    class Le(Expr):
        """1 if a is less than or equal to b"""
        def eval(self, fields={}):
            if self.a.eval(fields) <= self.b.eval(fields):
                return RInt(1)
            else:
                return RInt(0)

    @bop('>', 4)
    class Gt(Expr):
        """1 if a is greater than b"""
        def eval(self, fields={}):
            if self.a.eval(fields) > self.b.eval(fields):
                return RInt(1)
            else:
                return RInt(0)

    @bop('>=', 4)
    class Ge(Expr):
        """1 if a is greater than or equal to b"""
        def eval(self, fields={}):
            if self.a.eval(fields) >= self.b.eval(fields):
                return RInt(1)
            else:
                return RInt(0)

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
    def top(op_a, op_b, prec):
        def top(f):
            f._top = (op_a, op_b)
            f._tprec = prec
            return f
        return top

    class TOps:
        @ft.cache
        def __get__(self, _, cls):
            return {x._top: x
                    for x in cls.__dict__.values()
                    if hasattr(x, '_top')}
    tops = TOps()

    class TPrecs:
        @ft.cache
        def __get__(self, _, cls):
            return {x._top: x._tprec
                    for x in cls.__dict__.values()
                    if hasattr(x, '_top')}
    tprecs = TPrecs()

    # ternary ops
    @top('?', ':', 1)
    class IfElse(Expr):
        """b if a is non-zero, otherwise c"""
        def type(self, types={}):
            t = self.b.type(types)
            u = self.c.type(types)
            if t != u:
                raise RExpr.Error("mismatched types? %r" % self)
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
            print('  %-21s %s' % ('%sa' % op, RExpr.uops[op].__doc__))
        print('bops:')
        for op in cls.bops.keys():
            print('  %-21s %s' % ('a %s b' % op, RExpr.bops[op].__doc__))
        print('tops:')
        for op in cls.tops.keys():
            print('  %-21s %s' % ('a %s b %s c' % op, RExpr.tops[op].__doc__))
        print('funcs:')
        for func in cls.funcs.keys():
            print('  %-21s %s' % (
                    '%s(%s)' % (func, RExpr.funcs[func]._fargs),
                    RExpr.funcs[func].__doc__))

    # parse an expr
    def __init__(self, expr):
        self.expr = expr.strip()

        # parse the expression into a tree
        def p_expr(expr, prec=0):
            # parens
            if expr.startswith('('):
                a, tail = p_expr(expr[1:].lstrip())
                if not tail.startswith(')'):
                    raise RExpr.Error("mismatched parens? %s" % tail)
                tail = tail[1:].lstrip()

            # strings
            elif re.match('(?:"(?:\\.|[^"])*"|\'(?:\\.|[^\'])\')', expr):
                m = re.match('(?:"(?:\\.|[^"])*"|\'(?:\\.|[^\'])\')', expr)
                a = RExpr.StrLit(m.group()[1:-1])
                tail = expr[len(m.group()):].lstrip()

            # floats
            elif re.match('[+-]?(?:[_0-9]*\.[_0-9eE]|nan)', expr):
                m = re.match('[+-]?(?:[_0-9]*\.[_0-9eE]|nan)', expr)
                a = RExpr.FloatLit(RFloat(m.group()))
                tail = expr[len(m.group()):].lstrip()

            # ints
            elif re.match('[+-]?(?:[0-9][bBoOxX]?[_0-9a-fA-F]*|∞|inf)', expr):
                m = re.match('[+-]?(?:[0-9][bBoOxX]?[_0-9a-fA-F]*|∞|inf)', expr)
                a = RExpr.IntLit(RInt(m.group()))
                tail = expr[len(m.group()):].lstrip()

            # fields/functions
            elif re.match('[_a-zA-Z][_a-zA-Z0-9]*', expr):
                m = re.match('[_a-zA-Z][_a-zA-Z0-9]*', expr)
                tail = expr[len(m.group()):].lstrip()

                if tail.startswith('('):
                    tail = tail[1:].lstrip()
                    if m.group() not in RExpr.funcs:
                        raise RExpr.Error("unknown function? %s" % m.group())
                    args = []
                    while True:
                        a, tail = p_expr(tail)
                        args.append(a)
                        if tail.startswith(','):
                            tail = tail[1:].lstrip()
                            continue
                        else:
                            if not tail.startswith(')'):
                                raise RExpr.Error(
                                        "mismatched parens? %s" % tail)
                            a = RExpr.funcs[m.group()](*args)
                            tail = tail[1:].lstrip()
                            break

                else:
                    a = RExpr.Field(m.group())

            # unary ops
            elif any(expr.startswith(op) for op in RExpr.uops.keys()):
                # sort by len to avoid ambiguities
                for op in sorted(RExpr.uops.keys(), reverse=True):
                    if expr.startswith(op):
                        a, tail = p_expr(expr[len(op):].lstrip(), mt.inf)
                        a = RExpr.uops[op](a)
                        break
                else:
                    assert False

            # unknown expr?
            else:
                raise RExpr.Error("unknown expr? %s" % expr)

            # parse tail
            while True:
                # binary ops
                if any(tail.startswith(op) and prec < RExpr.bprecs[op]
                        for op in RExpr.bops.keys()):
                    # sort by len to avoid ambiguities
                    for op in sorted(RExpr.bops.keys(), reverse=True):
                        if tail.startswith(op) and prec < RExpr.bprecs[op]:
                            b, tail = p_expr(
                                    tail[len(op):].lstrip(),
                                    RExpr.bprecs[op])
                            a = RExpr.bops[op](a, b)
                            break
                    else:
                        assert False

                # ternary ops, these are intentionally right associative
                elif any(tail.startswith(op[0]) and prec <= RExpr.tprecs[op]
                        for op in RExpr.tops.keys()):
                    # sort by len to avoid ambiguities
                    for op in sorted(RExpr.tops.keys(), reverse=True):
                        if tail.startswith(op[0]) and prec <= RExpr.tprecs[op]:
                            b, tail = p_expr(
                                    tail[len(op[0]):].lstrip(),
                                    RExpr.tprecs[op])
                            if not tail.startswith(op[1]):
                                raise RExpr.Error(
                                        'mismatched ternary op? %s %s' % op)
                            c, tail = p_expr(
                                    tail[len(op[1]):].lstrip(),
                                    RExpr.tprecs[op])
                            a = RExpr.tops[op](a, b, c)
                            break
                    else:
                        assert False

                # no tail
                else:
                    return a, tail

        try:
            self.tree, tail = p_expr(self.expr)
            if tail:
                raise RExpr.Error("trailing expr? %s" % tail)

        except (RExpr.Error, ValueError) as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursively find all fields
    def fields(self):
        try:
            return self.tree.fields()
        except RExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursively find the type
    def type(self, types={}):
        try:
            return self.tree.type(types)
        except RExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursively find the fold operation
    def fold(self, types={}):
        try:
            return self.tree.fold(types)
        except RExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)

    # recursive evaluate the expr
    def eval(self, fields={}):
        try:
            return self.tree.eval(fields)
        except RExpr.Error as e:
            print('error: in expr: %s' % self.expr,
                    file=sys.stderr)
            print('error: %s' % e,
                    file=sys.stderr)
            sys.exit(3)


def openio(path, mode='r', buffering=-1):
    # allow '-' for stdin/stdout
    if path == '-':
        if 'r' in mode:
            return os.fdopen(os.dup(sys.stdin.fileno()), mode, buffering)
        else:
            return os.fdopen(os.dup(sys.stdout.fileno()), mode, buffering)
    else:
        return open(path, mode, buffering)

def collect(csv_paths, defines=[]):
    # collect results from CSV files
    fields = []
    results = []
    for path in csv_paths:
        try:
            with openio(path) as f:
                reader = csv.DictReader(f, restval='')
                fields.extend(
                        k for k in reader.fieldnames
                            if k not in fields)
                for r in reader:
                    # filter by matching defines
                    if not all(k in r and r[k] in vs for k, vs in defines):
                        continue

                    results.append(r)
        except FileNotFoundError:
            pass

    return fields, results

def infer(fields_, results,
        by=None,
        fields=None,
        exprs=[],
        defines=[],
        sort=None):
    # we only really care about the last expr for each field
    exprs = {k: expr for k, expr in exprs}

    # find all fields our exprs depend on
    fields__ = set(it.chain.from_iterable(
            expr.fields() for _, expr in exprs.items()))

    # if by not specified, guess it's anything not in fields/exprs/defines
    if by is None:
        by = [k for k in fields_
                if k not in (fields or [])
                    and k not in fields__
                    and not any(k == k_ for k_, _ in defines)]

    # if fields not specified, guess it's anything not in by/exprs/defines
    if fields is None:
        fields = [k for k in fields_
                if k not in (by or [])
                    and k not in fields__
                    and not any(k == k_ for k_, _ in defines)]

    # deduplicate by/fields
    by = list(co.OrderedDict.fromkeys(by).keys())
    fields = list(co.OrderedDict.fromkeys(fields).keys())

    # make sure sort fields are included
    if sort is not None:
        by.extend(k for k, reverse in sort
                if k not in by and k not in fields)

    # find best type for all fields used by field exprs
    fields__ = set(it.chain.from_iterable(
            exprs[k].fields() if k in exprs else {k}
                for k in fields))
    types = {}
    for k in fields__:
        if k not in fields_:
            print("error: no field %r?" % k,
                    file=sys.stderr)
            sys.exit(2)

        for t in [RInt, RFloat, RFrac]:
            for r in results:
                if k in r and r[k].strip():
                    try:
                        t(r[k])
                    except ValueError:
                        break
            else:
                types[k] = t
                break
        else:
            print("error: no type matches field %r?" % k,
                    file=sys.stderr)
            sys.exit(2)

    # typecheck field exprs, note these may reference input fields
    # with the same name
    types__ = types.copy()
    for k, expr in exprs.items():
        if k in fields:
            types__[k] = expr.type(types)

    # foldcheck field exprs
    folds = {k: (RSum, t) for k, v in types.items()}
    for k, expr in exprs.items():
        if k in fields:
            folds[k] = expr.fold(types)
    folds = {k: (f(), t) for k, (f, t) in folds.items()}

    # create result class
    def __new__(cls, **r):
        # evaluate types
        r_ = r.copy()
        for k, t in types.items():
            r_[k] = t(r[k]) if k in r else t()
        # evaluate exprs
        r__ = r_.copy()
        for k, expr in exprs.items():
            r__[k] = expr.eval(r_)

        # return result
        return cls.__mro__[1].__new__(cls,
                **{k: r__.get(k, '') for k in by},
                **{k: ([r__[k]], 1) if k in r__ else ([], 0)
                    for k in fields})

    def __add__(self, other):
        # reuse lists if possible
        def extend(a, b):
            if len(a[0]) == a[1]:
                a[0].extend(b[0][:b[1]])
                return (a[0], a[1] + b[1])
            else:
                return (a[0][:a[1]] + b[0][:b[1]], a[1] + b[1])

        # lazily fold results
        return self.__class__.__mro__[1].__new__(self.__class__,
                **{k: getattr(self, k) for k in by},
                **{k: extend(
                        object.__getattribute__(self, k),
                        object.__getattribute__(other, k))
                    for k in fields})

    def __getattribute__(self, k):
        # lazily fold results on demand, this avoids issues with fold
        # operations that depend on the number of results
        if k in fields:
            v = object.__getattribute__(self, k)
            if v[1]:
                return folds[k][0](v[0][:v[1]])
            else:
                return None
        return object.__getattribute__(self, k)

    return type('Result', (co.namedtuple('Result', by + fields),), {
        '__slots__': (),
        '__new__': __new__,
        '__add__': __add__,
        '__getattribute__': __getattribute__,
        '_by': by,
        '_fields': fields,
        '_sort': fields,
        '_types': {k: t for k, (_, t) in folds.items()},
    })


def fold(Result, results, by=None, defines=[]):
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
            if all(getattr(r, k) in vs for k, vs in defines):
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

    return folded

def table(Result, results, diff_results=None, *,
        by=None,
        fields=None,
        sort=None,
        summary=False,
        all=False,
        percent=False,
        **_):
    all_, all = all, __builtins__.all

    if by is None:
        by = Result._by
    if fields is None:
        fields = Result._fields
    types = Result._types

    # fold again
    results = fold(Result, results, by=by)
    if diff_results is not None:
        diff_results = fold(Result, diff_results, by=by)

    # organize by name
    table = {
            ','.join(str(getattr(r, k) or '') for k in by): r
                for r in results}
    diff_table = {
            ','.join(str(getattr(r, k) or '') for k in by): r
                for r in diff_results or []}
    names = [name
            for name in table.keys() | diff_table.keys()
            if diff_results is None
                or all_
                or any(
                    types[k].ratio(
                            getattr(table.get(name), k, None),
                            getattr(diff_table.get(name), k, None))
                        for k in fields)]

    # sort again, now with diff info, note that python's sort is stable
    names.sort()
    if diff_results is not None:
        names.sort(
                key=lambda n: tuple(
                    types[k].ratio(
                            getattr(table.get(n), k, None),
                            getattr(diff_table.get(n), k, None))
                        for k in fields),
                reverse=True)
    if sort:
        for k, reverse in reversed(sort):
            names.sort(
                    key=lambda n: tuple(
                        (getattr(table[n], k),)
                                if getattr(table.get(n), k, None) is not None
                                else ()
                            for k in (
                                [k] if k else [
                                    k for k in Result._sort
                                        if k in fields])),
                    reverse=reverse ^ (not k or k in Result._fields))


    # build up our lines
    lines = []

    # header
    header = ['%s%s' % (
                ','.join(by),
                ' (%d added, %d removed)' % (
                        sum(1 for n in table if n not in diff_table),
                        sum(1 for n in diff_table if n not in table))
                    if diff_results is not None and not percent else '')
            if not summary else '']
    if diff_results is None:
        for k in fields:
            header.append(k)
    elif percent:
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

    # entry helper
    def table_entry(name, r, diff_r=None):
        entry = [name]
        if diff_results is None:
            for k in fields:
                entry.append(
                        (getattr(r, k).table(),
                                getattr(getattr(r, k), 'notes', lambda: [])())
                            if getattr(r, k, None) is not None
                            else types[k].none)
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
        return entry

    # entries
    if not summary:
        for name in names:
            r = table.get(name)
            if diff_results is None:
                diff_r = None
            else:
                diff_r = diff_table.get(name)
            lines.append(table_entry(name, r, diff_r))

    # total
    r = next(iter(fold(Result, results, by=[])), None)
    if diff_results is None:
        diff_r = None
    else:
        diff_r = next(iter(fold(Result, diff_results, by=[])), None)
    lines.append(table_entry('TOTAL', r, diff_r))

    # homogenize
    lines = [
            [x if isinstance(x, tuple) else (x, []) for x in line]
                for line in lines]

    # find the best widths, note that column 0 contains the names and is
    # handled a bit differently
    widths = co.defaultdict(lambda: 7, {0: 7})
    notes = co.defaultdict(lambda: 0)
    for line in lines:
        for i, x in enumerate(line):
            widths[i] = max(widths[i], ((len(x[0])+1+4-1)//4)*4-1)
            notes[i] = max(notes[i], 1+2*len(x[1])+sum(len(n) for n in x[1]))

    # print our table
    for line in lines:
        print('%-*s  %s' % (
                widths[0], line[0][0],
                ' '.join('%*s%-*s' % (
                        widths[i], x[0],
                        notes[i], ' (%s)' % ', '.join(x[1]) if x[1] else '')
                    for i, x in enumerate(line[1:], 1))))


def main(csv_paths, *,
        by=None,
        fields=None,
        defines=[],
        sort=None,
        **args):
    # show expr help text?
    if args.get('help_exprs'):
        return RExpr.help()

    # separate out exprs
    exprs = [(k, v)
            for k, v in it.chain(
                by or [],
                fields or [],
                ((k, v) for (k, v), reverse in sort or []))
            if v is not None]
    if by is not None:
        by = [k for k, _ in by]
    if fields is not None:
        fields = [k for k, _ in fields]
    if sort is not None:
        sort = [(k, reverse) for (k, v), reverse in sort]

    if by is None and fields is None:
        print("error: needs --by or --fields to figure out fields",
                file=sys.stderr)
        sys.exit(-1)

    # use is just an alias
    if args.get('use'):
        csv_paths = csv_paths + [args['use']]

    # find CSV files
    fields_, results = collect(csv_paths, defines)

    # homogenize
    Result = infer(fields_, results,
            by=by,
            fields=fields,
            exprs=exprs,
            defines=defines,
            sort=sort)
    results_ = []
    for r in results:
        results_.append(Result(**{
                k: v for k, v in r.items() if v.strip()}))
    results = results_

    # fold
    results = fold(Result, results, by=by)

    # sort, note that python's sort is stable
    results.sort()
    if sort:
        for k, reverse in reversed(sort):
            results.sort(
                    key=lambda r: tuple(
                        (getattr(r, k),) if getattr(r, k) is not None else ()
                            for k in ([k] if k else Result._sort)),
                    reverse=reverse ^ (not k or k in Result._fields))

    # write results to CSV
    if args.get('output'):
        with openio(args['output'], 'w') as f:
            writer = csv.DictWriter(f, Result._by + Result._fields)
            writer.writeheader()
            for r in results:
                # note we need to go through getattr to resolve lazy fields
                writer.writerow({
                        k: getattr(r, k)
                            for k in Result._by + Result._fields})

    # find previous results?
    if args.get('diff'):
        _, diff_results = collect([args['diff']], defines)
        diff_results_ = []
        for r in diff_results:
            if not any(k in r and r[k].strip()
                    for k in Result._fields):
                continue
            try:
                diff_results_.append(Result(**{
                        k: r[k] for k in Result._by + Result._fields
                            if k in r and r[k].strip()}))
            except TypeError:
                pass
        diff_results = diff_results_

        # fold
        diff_results = fold(Result, diff_results, by=by)

    # print table
    if not args.get('quiet'):
        table(Result, results,
                diff_results if args.get('diff') else None,
                by=by,
                fields=fields,
                sort=sort,
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
            '--help-exprs',
            action='store_true',
            help="Show what field exprs are available.")
    parser.add_argument(
            '-q', '--quiet',
            action='store_true',
            help="Don't show anything, useful with -o.")
    parser.add_argument(
            '-o', '--output',
            help="Specify CSV file to store results.")
    parser.add_argument(
            '-u', '--use',
            help="Don't parse anything, use this CSV file.")
    parser.add_argument(
            '-d', '--diff',
            help="Specify CSV file to diff against.")
    parser.add_argument(
            '-a', '--all',
            action='store_true',
            help="Show all, not just the ones that changed.")
    parser.add_argument(
            '-p', '--percent',
            action='store_true',
            help="Only show percentage change, not a full diff.")
    parser.add_argument(
            '-b', '--by',
            action='append',
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    RExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            help="Group by this field. Can include an expression of the form "
                "field=expr.")
    parser.add_argument(
            '-f', '--field',
            dest='fields',
            action='append',
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    RExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            help="Show this field. Can include an expression of the form "
                "field=expr.")
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
            namespace.sort.append((value, True if option == '-S' else False))
    parser.add_argument(
            '-s', '--sort',
            nargs='?',
            action=AppendSort,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    RExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            help="Sort by this field. Can include an expression of the form "
                "field=expr.")
    parser.add_argument(
            '-S', '--reverse-sort',
            nargs='?',
            action=AppendSort,
            type=lambda x: (
                lambda k, v=None: (
                    k.strip(),
                    RExpr(v) if v is not None else None)
                )(*x.split('=', 1)),
            help="Sort by this field, but backwards. Can include an expression "
                "of the form field=expr.")
    parser.add_argument(
            '-Y', '--summary',
            action='store_true',
            help="Only show the total.")
    sys.exit(main(**{k: v
            for k, v in vars(parser.parse_intermixed_args()).items()
            if v is not None}))
