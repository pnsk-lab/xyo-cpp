#!/usr/bin/env python3
"""Fuzz the number -> string conversion (ES shortest round-trip formatting).

Every literal is written with Python's ``repr``, which is the shortest decimal
that round-trips to the same double, so the JS ``Number()`` parser recovers the
identical bit pattern.  The probe value is ``join("", 0 + literal)``.
"""
import json
import math
import os
import random
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import Battery, build_battery, PROBE_DIR  # noqa: E402

N = Battery.nlit
T = Battery.slit


def literals():
    rng = random.Random(424242)
    out = []
    # random finite bit patterns, any exponent
    while len(out) < 260:
        bits = rng.getrandbits(64)
        d = struct.unpack("<d", struct.pack("<Q", bits))[0]
        if math.isinf(d) or math.isnan(d):
            continue
        out.append(repr(d))
    # powers of ten with a random mantissa (exercises the %e thresholds)
    for _ in range(130):
        m = rng.uniform(1.0, 9.999999999999999)
        k = rng.randint(-320, 308)
        try:
            d = float("%.17ge%d" % (m, k))
        except (OverflowError, ValueError):
            continue
        if math.isinf(d) or d == 0.0:
            continue
        out.append(repr(d))
    # boundaries: 1e21, 1e-7, 1e-6, 2^53, subnormals, max double
    boundary = [1e21, 1e20, 1e-7, 1e-6, 2.0**53, 2.0**53 + 2, 2.0**53 - 1,
                5e-324, 1e-323, 2.2250738585072014e-308,
                1.7976931348623157e308, 9007199254740993.0,
                999999999999999999999.0, 1e21 - 65536.0, 1e21 + 65536.0,
                1.0000000000000002e21, 9.999999999999999e20,
                123456789.123456789, 0.1, 0.2, 0.3, 1 / 3, 2 / 3,
                1e100, 1e-100, 1234567890123456789.0]
    for d in boundary:
        out.append(repr(d))
    return out


def main():
    lits = literals()
    P = []
    for i, lit in enumerate(lits):
        P.append(("n%03d %s" % (i, lit[:24]),
                  (lambda v: lambda p: p.join(T(""), p.add(N(0), N(v))))(lit)))
    # integers that only differ in the shortest representation
    for i, v in enumerate(["1e22", "1e23", "1e24", "123456789012345680000",
                           "1.2345678901234568e+20", "-1e21", "-0",
                           "-5e-324", "-1.7976931348623157e308"]):
        P.append(("s%d %s" % (i, v),
                  (lambda v: lambda p: p.join(T(""), p.add(N(0), N(v))))(v)))
    build_battery("numfuzz", P)
    print("wrote %d number probes" % len(P))


if __name__ == "__main__":
    main()
