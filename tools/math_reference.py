#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Maurizio Cammalleri
"""math_reference.py - the reference of the precision of limba/math.h.

Writes tests/math/reference.txt: for each function, arguments chosen as
test_math chooses them, and the two doubles around the exact result,
computed by mpmath with hundreds of bits (thousands for the trigonometric
functions of huge arguments). A line is

    name x [y] nearest other

in hexadecimal bits: nearest is the exact result rounded to nearest even,
other the double on the other side of it (the same when the result is
exact). The library is within 1 ulp when it returns one of the two.

Run it on the machine of development only, with mpmath (Debian:
python3-mpmath); never by the build: the file it writes is in the
repository. Usage: python3 tools/math_reference.py > tests/math/reference.txt
"""

import math
import random
import struct
import sys

import mpmath

COUNT = 1000


def bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def from_bits(u):
    return struct.unpack("<d", struct.pack("<Q", u))[0]


def any_finite(rng):
    while True:
        x = from_bits(rng.getrandbits(64))
        if math.isfinite(x):
            return x


def args(name, rng):
    if name in ("sin", "cos", "tan"):
        k = rng.randrange(4)
        if k == 0:
            return (rng.uniform(-4, 4),)
        if k == 1:
            return (rng.uniform(-1e6, 1e6),)
        if k == 2:
            return (rng.uniform(-1e-8, 1e-8),)
        return (any_finite(rng),)
    if name == "atan":
        return (rng.uniform(-20, 20) if rng.random() < 0.5 else any_finite(rng),)
    if name == "exp":
        return (rng.uniform(-745, 709),)
    if name == "ln":
        x = rng.uniform(0, 4) if rng.random() < 0.5 else abs(any_finite(rng))
        return (x if x > 0 else 1.0,)
    # pow
    if rng.random() < 0.25:
        return (-rng.uniform(0.1, 20), float(rng.randrange(-30, 31)))
    return (rng.uniform(1e-3, 1000), rng.uniform(-60, 60))


def exact(name, a):
    x = mpmath.mpf(a[0])
    if name in ("sin", "cos", "tan"):
        # enough bits for the reduction of a huge argument
        mpmath.mp.prec = 200 + max(0, int(mpmath.log(abs(x) + 1, 2)))
        return {"sin": mpmath.sin, "cos": mpmath.cos, "tan": mpmath.tan}[name](x)
    mpmath.mp.prec = 300
    if name == "atan":
        return mpmath.atan(x)
    if name == "exp":
        return mpmath.exp(x)
    if name == "ln":
        return mpmath.log(x)
    return mpmath.power(x, mpmath.mpf(a[1]))


def main():
    rng = random.Random(20261022)
    out = sys.stdout
    for name in ("sin", "cos", "tan", "atan", "exp", "ln", "pow"):
        done = 0
        while done < COUNT:
            a = args(name, rng)
            v = exact(name, a)
            if not mpmath.isfinite(v) or mpmath.im(v) != 0:
                continue
            v = mpmath.re(v)
            near = float(v)  # to nearest even
            if not math.isfinite(near) or near == 0 or abs(near) < 2.3e-308:
                continue  # overflow, underflow, subnormal: not this check
            if mpmath.mpf(near) == v:
                other = near
            elif mpmath.mpf(near) < v:
                other = math.nextafter(near, math.inf)
            else:
                other = math.nextafter(near, -math.inf)
            fields = [name] + ["%#018x" % bits(x) for x in a]
            fields += ["%#018x" % bits(near), "%#018x" % bits(other)]
            out.write(" ".join(fields) + "\n")
            done += 1


if __name__ == "__main__":
    main()
