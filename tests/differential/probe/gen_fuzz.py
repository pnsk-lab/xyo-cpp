#!/usr/bin/env python3
"""Random nested-expression battery (deterministic seed).

Nested reporters combine casts in ways hand-written probes miss: a list index
that comes out of a `join`, a `mod` whose divisor is `letter of`, a comparison
of a boolean with an empty string, and so on.  Every expression is evaluated
once and stored into its own variable, so a mismatch is pinpointed to one
expression and its exact source text.
"""
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import Battery, build_battery, PROBE_DIR  # noqa: E402

N = Battery.nlit
T = Battery.slit

NUMBERS = ["0", "1", "-1", "2", "0.5", "-0.5", "1.5", "2.5", "-2.5",
           "3.999", "1e21", "1e-7", "1e-6", "5e-324", "9007199254740993",
           "123456789012345678901", "1e308", "3", "10", "0.1", "0.2",
           "1e400", "-1e400"]
STRINGS = ["", " ", "  ", "0", "1", "1.5", "abc", "ABC", "0x10", "1e3",
           "Infinity", "NaN", "true", "false", "\t", "\r", "\u00a0",
           "\u00e9", "e\u0301", "\U0001f600", "\u0130", "\u1e9e",
           "\u00df", "\u212a", "K", "12px", "-", ".", "1_000"]

OPS2 = ["operator_add", "operator_subtract", "operator_multiply",
        "operator_divide", "operator_mod", "operator_join",
        "operator_equals", "operator_lt", "operator_gt", "operator_and",
        "operator_or", "operator_letter_of", "operator_contains"]
OPS1 = ["operator_round", "operator_not", "operator_length"]
MATHS = ["abs", "floor", "ceiling", "sqrt", "sin", "cos", "tan", "asin",
         "acos", "atan", "ln", "log", "e ^", "10 ^"]


def atom(p, rng):
    k = rng.random()
    if k < 0.42:
        return ("num", p.add(N(0), N(rng.choice(NUMBERS))))
    if k < 0.80:
        return ("text", T(rng.choice(STRINGS)))
    if k < 0.92:
        return ("var", p.var(rng.choice(["fv1", "fv2", "fv3"]),
                             {"fv1": "vfv1", "fv2": "vfv2",
                              "fv3": "vfv3"}[rng.choice(["fv1", "fv2", "fv3"])]))
    lid = rng.choice(["fl1", "fl2", "fl3"])
    lname = {"fl1": "fl1", "fl2": "fl2", "fl3": "fl3"}[lid]
    which = rng.random()
    if which < 0.4:
        return ("item", p.item(lname, lid, p.add(N(0), N(rng.choice(NUMBERS)))))
    if which < 0.6:
        return ("listlen", p.listlen(lname, lid))
    if which < 0.8:
        return ("listcontents", p.listcontents(lname, lid))
    return ("itemnum", p.itemnum(lname, lid, p.add(N(0), N(rng.choice(NUMBERS)))))


def expr(p, rng, depth):
    if depth <= 0 or rng.random() < 0.35:
        return atom(p, rng)
    r = rng.random()
    if r < 0.48:
        op = rng.choice(OPS2)
        if op == "operator_letter_of":
            return (op, p.letter(expr(p, rng, depth - 1)[1],
                                 expr(p, rng, depth - 1)[1]))
        a = expr(p, rng, depth - 1)[1]
        b = expr(p, rng, depth - 1)[1]
        if op in ("operator_join", "operator_contains"):
            return (op, p._rep(op, {"STRING1": a, "STRING2": b}))
        if op in ("operator_equals", "operator_lt", "operator_gt"):
            return (op, p._rep(op, {"OPERAND1": a, "OPERAND2": b}))
        if op in ("operator_and", "operator_or"):
            return (op, p._rep(op, {"OPERAND1": a, "OPERAND2": b}))
        return (op, p.b(op, a, b))
    if r < 0.68:
        op = rng.choice(OPS1)
        if op == "operator_length":
            return (op, p.length(expr(p, rng, depth - 1)[1]))
        if op == "operator_round":
            return (op, p.rnd(expr(p, rng, depth - 1)[1]))
        return (op, p.not_(expr(p, rng, depth - 1)[1]))
    op = rng.choice(MATHS)
    return ("math " + op, p.math(op, expr(p, rng, depth - 1)[1]))


def main():
    rng = random.Random(20251027)
    probes = []
    for i in range(400):
        probes.append(("e%03d" % i, lambda p, i=i: _build(p, rng, i)))
    # The label must describe the expression, but labels are written before
    # the expression is built; record them afterwards instead.
    labels = []
    orig = build_battery

    def make(i, p, rng_local):
        return expr(p, rng_local, 3)

    built = []

    def probe_factory(i):
        def fn(p):
            r = random.Random(20251027 + i)
            kind, inp = make(i, p, r)
            return inp
        return fn

    P = [("e%03d" % i, probe_factory(i)) for i in range(400)]
    build_battery("fuzz", P,
                  lists={"fl1": ["fl1", ["a", "1", 2, ""]],
                         "fl2": ["fl2", []],
                         "fl3": ["fl3", ["1e21", "1e-7"]]},
                  variables={"vfv1": ["fv1", 0], "vfv2": ["fv2", "abc"],
                             "vfv3": ["fv3", 1.5]})
    print("wrote fuzz probes")


if __name__ == "__main__":
    main()
