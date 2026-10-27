#!/usr/bin/env python3
"""Generate probe batteries: clusters of semantically adjacent expressions.

    python3 tests/differential/probe/gen_probes.py [battery ...]

Each battery is a single .sb3 (+ .json label map) under ``probes/``.  The
probes deliberately sit on casting / rounding / indexing / Unicode boundaries
where the JS reference and a C++ reimplementation are most likely to differ.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import Battery, build_battery  # noqa: E402

N = Battery.nlit
T = Battery.slit


def num(v):
    """A real JS number (the shadow field itself is a string)."""
    return lambda p: p.add(N(0), N(v))


def numv(p, v):
    return p.add(N(0), N(v))


# ---------------------------------------------------------------------------
def battery_num():
    P = []
    a = P.append

    # -- String(number): ES shortest round-trip formatting -------------------
    for v in ("1e21", "1e20", "1e-6", "1e-7", "5e-324", "9007199254740994",
              "123456789012345678901", "1.7976931348623157e308",
              "0.000001234", "1234567890123456789012"):
        a(("str " + v,
           (lambda v: lambda p: p.join(T(""), numv(p, v)))(v)))
    a(("str 0.1+0.2", lambda p: p.join(T(""), p.add(numv(p, "0.1"),
                                                   numv(p, "0.2")))))
    a(("str -0", lambda p: p.join(T(""), p.mul(N(0), N(-1)))))
    a(("str 1/0", lambda p: p.join(T(""), p.div(N(1), N(0)))))
    a(("str -1/0", lambda p: p.join(T(""), p.div(N(-1), N(0)))))
    a(("str 0/0", lambda p: p.join(T(""), p.div(N(0), N(0)))))
    a(("str 1e308*10", lambda p: p.join(T(""), p.mul(numv(p, "1e308"),
                                                      N(10)))))

    # -- String -> number casts ---------------------------------------------
    for s in (" 42 ", "0x10", "0b101", "0o17", "1_000", "\n7\n", "\t8",
              "\u00a09", "Infinity", "-Infinity", "1e309", "+", ".5", "5.",
              "1e", "", "abc", "12px", "1,000", "0x1p3", "1e+2", "0X1F",
              "- 1", "٣"):
        a(("cast %r" % s,
           (lambda s=s: (lambda p: p.add(N(0), T(s))))()))

    # -- Cast.compare: whitespace / NaN / Infinity quirks --------------------
    a(("'' = 0", lambda p: p.eq(T(""), N(0))))
    a(("' ' = 0", lambda p: p.eq(T(" "), N(0))))
    a(("'\\n' = 0", lambda p: p.eq(T("\n"), N(0))))
    a(("'\\t' = 0", lambda p: p.eq(T("\t"), N(0))))
    a(("'\\u000b' = 0", lambda p: p.eq(T("\u000b"), N(0))))
    a(("NBSP = 0", lambda p: p.eq(T("\u00a0"), N(0))))
    a(("'0' = 0", lambda p: p.eq(T("0"), N(0))))
    a(("'00' = 0", lambda p: p.eq(T("00"), N(0))))
    a(("'0.0' = 0", lambda p: p.eq(T("0.0"), N(0))))
    a(("'0x0' = 0", lambda p: p.eq(T("0x0"), N(0))))
    a(("'false' = 0", lambda p: p.eq(T("false"), N(0))))
    a(("'NaN' = 0/0", lambda p: p.eq(T("NaN"), p.div(N(0), N(0)))))
    a(("'Infinity' = 1/0", lambda p: p.eq(T("Infinity"), p.div(N(1), N(0)))))
    a(("'infinity' = 1/0", lambda p: p.eq(T("infinity"), p.div(N(1), N(0)))))
    a(("'1e3' = 1000", lambda p: p.eq(T("1e3"), N(1000))))
    a(("' 1 ' = 1", lambda p: p.eq(T(" 1 "), N(1))))
    a(("'Z' < 'a'", lambda p: p.lt(T("Z"), T("a"))))
    a(("'ABC' < 'abd'", lambda p: p.lt(T("ABC"), T("abd"))))
    a(("'10' < '9'", lambda p: p.lt(T("10"), T("9"))))
    a(("'' < 'a'", lambda p: p.lt(T(""), T("a"))))
    a(("1/0 = 1/0", lambda p: p.eq(p.div(N(1), N(0)), p.div(N(1), N(0)))))
    a(("0/0 = 0/0", lambda p: p.eq(p.div(N(0), N(0)), p.div(N(0), N(0)))))
    a(("'K' = KELVIN", lambda p: p.eq(T("K"), T("\u212a"))))
    a(("DOTTED-I = i", lambda p: p.eq(T("\u0130"), T("i"))))
    a(("SS = \u00df", lambda p: p.eq(T("\u1e9e"), T("\u00df"))))

    # -- mathop ------------------------------------------------------------
    a(("round -0.5", lambda p: p.rnd(N("-0.5"))))
    a(("round 0.5", lambda p: p.rnd(N("0.5"))))
    a(("round -1.5", lambda p: p.rnd(N("-1.5"))))
    a(("round 1e308", lambda p: p.rnd(numv(p, "1e308"))))
    a(("round 1/0", lambda p: p.rnd(p.div(N(1), N(0)))))
    a(("round 0/0", lambda p: p.rnd(p.div(N(0), N(0)))))
    a(("round -0.4", lambda p: p.rnd(N("-0.4"))))
    a(("round -0", lambda p: p.rnd(p.mul(N(0), N(-1)))))
    a(("floor -0.5", lambda p: p.math("floor", N("-0.5"))))
    a(("ceiling -0.5", lambda p: p.math("ceiling", N("-0.5"))))
    a(("abs -0", lambda p: p.math("abs", p.mul(N(0), N(-1)))))
    a(("sqrt -1", lambda p: p.math("sqrt", N(-1))))
    a(("sqrt 1/0", lambda p: p.math("sqrt", p.div(N(1), N(0)))))
    a(("ln 0", lambda p: p.math("ln", N(0))))
    a(("ln -1", lambda p: p.math("ln", N(-1))))
    a(("log 0", lambda p: p.math("log", N(0))))
    a(("tan 90", lambda p: p.math("tan", N(90))))
    a(("tan -90", lambda p: p.math("tan", N(-90))))
    a(("tan 270", lambda p: p.math("tan", N(270))))
    a(("sin 180", lambda p: p.math("sin", N(180))))
    a(("cos 90", lambda p: p.math("cos", N(90))))
    a(("10^400", lambda p: p.math("10 ^", N(400))))
    a(("0^0", lambda p: p.math("10 ^", N(0))))
    a(("0^-1", lambda p: p.math("10 ^", N(-1))))
    a(("(-8)^(1/3)", lambda p: p.math("10 ^", p.div(N(-8), N(3)))))
    a(("e^709", lambda p: p.math("e ^", N(709))))
    a(("e^-1000", lambda p: p.math("e ^", N(-1000))))
    a(("asin 2", lambda p: p.math("asin", N(2))))
    a(("atan 1/0", lambda p: p.math("atan", p.div(N(1), N(0)))))

    # -- mod / div ---------------------------------------------------------
    a(("5 mod 0", lambda p: p.mod(N(5), N(0))))
    a(("-5 mod 3", lambda p: p.mod(N(-5), N(3))))
    a(("5 mod -3", lambda p: p.mod(N(5), N(-3))))
    a(("0.3 mod 0.1", lambda p: p.mod(N("0.3"), N("0.1"))))
    a(("1e308 mod 3", lambda p: p.mod(numv(p, "1e308"), N(3))))
    a(("-0 mod 3", lambda p: p.mod(p.mul(N(0), N(-1)), N(3))))
    a(("0/0 mod 3", lambda p: p.mod(p.div(N(0), N(0)), N(3))))
    a(("1 mod 0/0", lambda p: p.mod(N(1), p.div(N(0), N(0)))))
    a(("2^53 mod 3", lambda p: p.mod(numv(p, "9007199254740992"), N(3))))
    build_battery("num", P)


# ---------------------------------------------------------------------------
def battery_str():
    P = []
    a = P.append
    a(("len emoji", lambda p: p.length(T("\U0001f600"))))
    a(("letter 1 emoji", lambda p: p.letter(N(1), T("\U0001f600"))))
    a(("letter 2 emoji", lambda p: p.letter(N(2), T("\U0001f600"))))
    a(("letter 3 emoji", lambda p: p.letter(N(3), T("\U0001f600"))))
    a(("letter 1.5 abc", lambda p: p.letter(N("1.5"), T("abc"))))
    a(("letter 2.999 abc", lambda p: p.letter(N("2.999"), T("abc"))))
    a(("letter NaN abc", lambda p: p.letter(p.div(N(0), N(0)), T("abc"))))
    a(("letter Inf abc", lambda p: p.letter(p.div(N(1), N(0)), T("abc"))))
    a(("letter -Inf abc", lambda p: p.letter(p.div(N(-1), N(0)), T("abc"))))
    a(("letter 0 abc", lambda p: p.letter(N(0), T("abc"))))
    a(("letter 4 abc", lambda p: p.letter(N(4), T("abc"))))
    a(("letter 2 of 12345", lambda p: p.letter(N(2), numv(p, 12345))))
    a(("letter '2' abc", lambda p: p.letter(T("2"), T("abc"))))
    a(("letter 'a' abc", lambda p: p.letter(T("a"), T("abc"))))
    a(("letter -0", lambda p: p.letter(p.mul(N(0), N(-1)), T("abc"))))
    a(("letter 1 -1e-9", lambda p: p.letter(N("-1e-9"), T("abc"))))
    a(("len ''", lambda p: p.length(T(""))))
    a(("len 1e21", lambda p: p.length(numv(p, "1e21"))))
    a(("len e-acute", lambda p: p.length(T("\u00e9"))))
    a(("len e+combining", lambda p: p.length(T("e\u0301"))))
    a(("len ZWJ family", lambda p: p.length(T("\U0001f468\u200d\U0001f469\u200d"
                                              "\U0001f467"))))
    a(("len lone hi", lambda p: p.length(T("\ud83d"))))
    a(("len -0", lambda p: p.length(p.mul(N(0), N(-1)))))
    a(("len 0.000001", lambda p: p.length(numv(p, "0.000001"))))
    a(("contains emoji", lambda p: p.contains(T("\U0001f600x"),
                                              T("\U0001f600"))))
    a(("contains ''", lambda p: p.contains(T("abc"), T(""))))
    a(("contains 123,2", lambda p: p.contains(numv(p, 123), N(2))))
    a(("contains aBc,bc", lambda p: p.contains(T("aBc"), T("bc"))))
    a(("contains 1e21", lambda p: p.contains(numv(p, "1e21"), T("1e+21"))))
    a(("contains '',''", lambda p: p.contains(T(""), T(""))))
    a(("contains 0.1,'0.1'", lambda p: p.contains(N("0.1"), T("0.1"))))
    a(("join 0.1 0.2", lambda p: p.join(N("0.1"), N("0.2"))))
    a(("join 1e21 ''", lambda p: p.join(numv(p, "1e21"), T(""))))
    a(("join -0 ''", lambda p: p.join(p.mul(N(0), N(-1)), T(""))))
    a(("join emoji", lambda p: p.join(T("\U0001f600"), T("x"))))
    a(("join lone hi", lambda p: p.join(T("\ud83d"), T("x"))))
    a(("join 0/0 ''", lambda p: p.join(p.div(N(0), N(0)), T(""))))
    a(("len join astral", lambda p: p.length(p.join(T("\U0001f600"),
                                                    T("\U0001f600")))))
    build_battery("str", P)


# ---------------------------------------------------------------------------
def battery_list():
    lists = {
        "v1": ["l1", ["a", "b", "c"]],
        "v2": ["l2", ["1", "2"]],
        "v3": ["l3", []],
        "v4": ["l4", [1, 2, 3]],
        "v5": ["l5", ["A", "a"]],
        "v6": ["l6", ["1.0", "2"]],
        "v7": ["l7", ["x y", "z"]],
        "v8": ["l8", [0, 0]],
    }
    P = []
    a = P.append
    # indexing
    a(("item 1 l1", lambda p: p.item("l1", "v1", N(1))))
    a(("item 1.9 l1", lambda p: p.item("l1", "v1", N("1.9"))))
    a(("item 2.999 l1", lambda p: p.item("l1", "v1", N("2.999"))))
    a(("item -0 l1", lambda p: p.item("l1", "v1", p.mul(N(0), N(-1)))))
    a(("item NaN l1", lambda p: p.item("l1", "v1", p.div(N(0), N(0)))))
    a(("item Inf l1", lambda p: p.item("l1", "v1", p.div(N(1), N(0)))))
    a(("item -Inf l1", lambda p: p.item("l1", "v1", p.div(N(-1), N(0)))))
    a(("item 0 l1", lambda p: p.item("l1", "v1", N(0))))
    a(("item '2' l1", lambda p: p.item("l1", "v1", T("2"))))
    a(("item 'last' l1", lambda p: p.item("l1", "v1", T("last"))))
    a(("item 'all' l1", lambda p: p.item("l1", "v1", T("all"))))
    a(("item 1 l3 (empty)", lambda p: p.item("l3", "v3", N(1))))
    a(("item 'last' l3", lambda p: p.item("l3", "v3", T("last"))))
    a(("item 1 l4 (numbers)", lambda p: p.item("l4", "v4", N(1))))
    a(("item 3 l4", lambda p: p.item("l4", "v4", N(3))))
    # item # of / contains
    a(("num# b in l1", lambda p: p.itemnum("l1", "v1", T("b"))))
    a(("num# B in l1", lambda p: p.itemnum("l1", "v1", T("B"))))
    a(("num# 1 in l4", lambda p: p.itemnum("l4", "v4", N(1))))
    a(("num# '1' in l4", lambda p: p.itemnum("l4", "v4", T("1"))))
    a(("num# '1.0' in l4", lambda p: p.itemnum("l4", "v4", T("1.0"))))
    a(("num# '01' in l4", lambda p: p.itemnum("l4", "v4", T("01"))))
    a(("num# 0 in l4", lambda p: p.itemnum("l4", "v4", N(0))))
    a(("num# '' in l1", lambda p: p.itemnum("l1", "v1", T(""))))
    a(("num# 'x y' in l7", lambda p: p.itemnum("l7", "v7", T("x y"))))
    a(("num# 0 in l8", lambda p: p.itemnum("l8", "v8", N(0))))
    a(("has B l1", lambda p: p.listhas("l1", "v1", T("B"))))
    a(("has '' l1", lambda p: p.listhas("l1", "v1", T(""))))
    a(("has 1 l4", lambda p: p.listhas("l4", "v4", N(1))))
    a(("has '1.0' l4", lambda p: p.listhas("l4", "v4", T("1.0"))))
    a(("len l1", lambda p: p.listlen("l1", "v1")))
    a(("len l3", lambda p: p.listlen("l3", "v3")))
    a(("contents l1", lambda p: p.listcontents("l1", "v1")))
    a(("contents l4", lambda p: p.listcontents("l4", "v4")))
    a(("contents l3", lambda p: p.listcontents("l3", "v3")))
    a(("contents l5", lambda p: p.listcontents("l5", "v5")))
    a(("contents l7", lambda p: p.listcontents("l7", "v7")))
    a(("contents l8", lambda p: p.listcontents("l8", "v8")))

    # mutations on a scratch list l9
    lists["v9"] = ["l9", []]
    a(("-- insert at 0", lambda p: p.l_insert("l9", "v9", N(0), T("q")), "stack"))
    a(("after insert 0", lambda p: p.listcontents("l9", "v9")))
    a(("-- insert at 99", lambda p: p.l_insert("l9", "v9", N(99), T("r")), "stack"))
    a(("after insert 99", lambda p: p.listcontents("l9", "v9")))
    a(("-- insert NaN", lambda p: p.l_insert("l9", "v9",
                                             p.div(N(0), N(0)), T("n")), "stack"))
    a(("after insert NaN", lambda p: p.listcontents("l9", "v9")))
    a(("-- insert 'last'", lambda p: p.l_insert("l9", "v9", T("last"),
                                                T("L")), "stack"))
    a(("after insert last", lambda p: p.listcontents("l9", "v9")))
    a(("-- delete Inf", lambda p: p.l_delete("l9", "v9", p.div(N(1), N(0))), "stack"))
    a(("after delete Inf", lambda p: p.listcontents("l9", "v9")))
    a(("-- delete 0", lambda p: p.l_delete("l9", "v9", N(0)), "stack"))
    a(("after delete 0", lambda p: p.listcontents("l9", "v9")))
    a(("-- delete -1", lambda p: p.l_delete("l9", "v9", N(-1)), "stack"))
    a(("after delete -1", lambda p: p.listcontents("l9", "v9")))
    a(("-- replace 0", lambda p: p.l_replace("l9", "v9", N(0), T("z")), "stack"))
    a(("after replace 0", lambda p: p.listcontents("l9", "v9")))
    a(("-- replace NaN", lambda p: p.l_replace("l9", "v9",
                                               p.div(N(0), N(0)), T("z")), "stack"))
    a(("after replace NaN", lambda p: p.listcontents("l9", "v9")))
    a(("-- replace 99", lambda p: p.l_replace("l9", "v9", N(99), T("z")), "stack"))
    a(("after replace 99", lambda p: p.listcontents("l9", "v9")))
    a(("-- delete 'all'", lambda p: p.l_delete("l9", "v9", T("all")), "stack"))
    a(("after delete all", lambda p: p.listcontents("l9", "v9")))
    a(("len after all", lambda p: p.listlen("l9", "v9")))
    a(("-- add ''", lambda p: p.l_add("l9", "v9", T("")), "stack"))
    a(("contents after add ''", lambda p: p.listcontents("l9", "v9")))
    a(("-- delete 'last'", lambda p: p.l_delete("l9", "v9", T("last")), "stack"))
    a(("contents after delete last", lambda p: p.listcontents("l9", "v9")))
    build_battery("list", P, lists=lists)




# ---------------------------------------------------------------------------
# Sign of zero, non-finite mathop operands, boolean coercion.
def battery_math2():
    P = []
    a = P.append
    inv = lambda p, e: p.div(N(1), e)          # reveal the sign of a zero
    a(("1/floor(-0)", lambda p: inv(p, p.math("floor", p.mul(N(0), N(-1))))))
    a(("1/floor(-0.0)", lambda p: inv(p, p.math("floor", N("-0.0")))))
    a(("1/ceil(-0.5)", lambda p: inv(p, p.math("ceiling", N("-0.5")))))
    a(("1/ceil(-0)", lambda p: inv(p, p.math("ceiling", p.mul(N(0), N(-1))))))
    a(("1/round(-0.4)", lambda p: inv(p, p.rnd(N("-0.4")))))
    a(("1/round(-0)", lambda p: inv(p, p.rnd(p.mul(N(0), N(-1))))))
    a(("1/sqrt(-0)", lambda p: inv(p, p.math("sqrt", p.mul(N(0), N(-1))))))
    a(("1/abs(-0)", lambda p: inv(p, p.math("abs", p.mul(N(0), N(-1))))))
    a(("1/sin(-0)", lambda p: inv(p, p.math("sin", p.mul(N(0), N(-1))))))
    a(("1/sin(-180)", lambda p: inv(p, p.math("sin", N(-180)))))
    a(("1/tan(-0)", lambda p: inv(p, p.math("tan", p.mul(N(0), N(-1))))))
    a(("1/asin(-0)", lambda p: inv(p, p.math("asin", p.mul(N(0), N(-1))))))
    a(("1/atan(-0)", lambda p: inv(p, p.math("atan", p.mul(N(0), N(-1))))))
    a(("1/atan(-1/0)", lambda p: inv(p, p.math("atan", p.div(N(-1), N(0))))))
    a(("1/(-4 mod 2)", lambda p: inv(p, p.mod(N(-4), N(2)))))
    a(("1/(0 mod -2)", lambda p: inv(p, p.mod(N(0), N(-2)))))
    a(("1/(-0 mod 2)", lambda p: inv(p, p.mod(p.mul(N(0), N(-1)), N(2)))))
    a(("1/(0*-1 + 0)", lambda p: inv(p, p.add(p.mul(N(0), N(-1)), N(0)))))
    a(("1/(e^-1000)", lambda p: inv(p, p.math("e ^", N(-1000)))))
    a(("1/(10^-1e400)", lambda p: inv(p, p.math("10 ^", N("-1e400")))))
    a(("1/(1/0)", lambda p: inv(p, p.div(N(1), N(0)))))
    a(("1/(0/0)", lambda p: inv(p, p.div(N(0), N(0)))))
    a(("1/(-1/0)", lambda p: inv(p, p.div(N(-1), N(0)))))
    a(("-0 = 0", lambda p: p.eq(p.mul(N(0), N(-1)), N(0))))
    a(("1/(-0) < 0", lambda p: p.lt(inv(p, p.mul(N(0), N(-1))), N(0))))
    a(("round(2^52+0.5)", lambda p: p.rnd(N("4503599627370496.5"))))
    a(("round(2^53+0.5)", lambda p: p.rnd(N("9007199254740993.5"))))
    a(("round(1e21+0.5)", lambda p: p.rnd(p.add(N("1e21"), N("0.5")))))
    a(("floor(1/0)", lambda p: p.math("floor", p.div(N(1), N(0)))))
    a(("ceil(-1/0)", lambda p: p.math("ceiling", p.div(N(-1), N(0)))))
    a(("floor(0/0)", lambda p: p.math("floor", p.div(N(0), N(0)))))
    a(("abs(0/0)", lambda p: p.math("abs", p.div(N(0), N(0)))))
    a(("ln(1/0)", lambda p: p.math("ln", p.div(N(1), N(0)))))
    a(("log(1/0)", lambda p: p.math("log", p.div(N(1), N(0)))))
    a(("10^(1/0)", lambda p: p.math("10 ^", p.div(N(1), N(0)))))
    a(("10^(-1/0)", lambda p: p.math("10 ^", p.div(N(-1), N(0)))))
    a(("10^(0/0)", lambda p: p.math("10 ^", p.div(N(0), N(0)))))
    a(("e^(0/0)", lambda p: p.math("e ^", p.div(N(0), N(0)))))
    a(("e^(1/0)", lambda p: p.math("e ^", p.div(N(1), N(0)))))
    a(("tan(1/0)", lambda p: p.math("tan", p.div(N(1), N(0)))))
    a(("sin(1/0)", lambda p: p.math("sin", p.div(N(1), N(0)))))
    a(("asin(1/0)", lambda p: p.math("asin", p.div(N(1), N(0)))))
    a(("atan(1/0)", lambda p: p.math("atan", p.div(N(1), N(0)))))
    a(("ln(-1/0)", lambda p: p.math("ln", p.div(N(-1), N(0)))))
    a(("sqrt(1/0)", lambda p: p.math("sqrt", p.div(N(1), N(0)))))
    a(("0^0.5", lambda p: p.math("10 ^", N("0.5"))))
    a(("(-1)^0.5", lambda p: p.math("10 ^", N("0.5"))))
    a(("3^0", lambda p: p.math("10 ^", N(0))))

    # -- boolean values in the data model ---------------------------------
    a(("join(true)", lambda p: p.join(T(""), p.lt(N(1), N(2)))))
    a(("true + 1", lambda p: p.add(p.lt(N(1), N(2)), N(1))))
    a(("true = 1", lambda p: p.eq(p.lt(N(1), N(2)), N(1))))
    a(("true = 'true'", lambda p: p.eq(p.lt(N(1), N(2)), T("true"))))
    a(("false = ''", lambda p: p.eq(p.gt(N(1), N(2)), T(""))))
    a(("not true", lambda p: p.not_(p.lt(N(1), N(2)))))
    a(("not 0", lambda p: p.not_(N(0))))
    a(("not ''", lambda p: p.not_(T(""))))
    a(("not 'false'", lambda p: p.not_(T("false"))))
    a(("not 'FALSE'", lambda p: p.not_(T("FALSE"))))
    a(("not '0.0'", lambda p: p.not_(T("0.0"))))
    a(("not (0/0)", lambda p: p.not_(p.div(N(0), N(0)))))
    a(("not 'abc'", lambda p: p.not_(T("abc"))))
    a(("and(true, 5)", lambda p: p.and_(p.lt(N(1), N(2)), N(5))))
    a(("or(false, '')", lambda p: p._rep("operator_or", {
        "OPERAND1": p.gt(N(1), N(2)), "OPERAND2": T("")})))
    a(("join(not '', 'x')", lambda p: p.join(p.not_(T("")), T("x"))))
    build_battery("math2", P)


# ---------------------------------------------------------------------------
# Loop-count semantics, for-each over a mutating list, stop variants.
def battery_control():
    P = []
    a = P.append
    V = {"vn": ["n", 0], "vi": ["i", 0], "vm": ["m", 0]}
    L = {"v1": ["l1", ["a", "b", "c"]], "vc": ["lc", []]}

    def counted(times, tag):
        def mk(p):
            p.set("n", "vn", N(0))
            p.repeat(times, lambda c: c.change("n", "vn", N(1)))
        a(("-- repeat %s" % tag, mk, "stack"))
        a(("repeat %s -> n" % tag, lambda p: p.var("n", "vn")))
    for times, tag in ((N("2.5"), "2.5"), (N("0.5"), "0.5"), (N(0), "0"),
                       (N(-1), "-1"), (N("3.999"), "3.999"),
                       (N("1e-9"), "1e-9"), (N("-1e-9"), "-1e-9"),
                       (T("3"), "'3'"), (T("abc"), "'abc'"),
                       (T("0.5"), "'0.5'")):
        counted(times, tag)
    a(("-- repeat (0/0)", lambda p: (p.set("n", "vn", N(0)),
                                    p.repeat(p.div(N(0), N(0)),
                                             lambda c: c.change("n", "vn",
                                                                N(1)))), "stack"))
    a(("repeat (0/0) -> n", lambda p: p.var("n", "vn")))

    # repeat until
    def until(p):
        p.set("n", "vn", N(0))
        p.repeat(p.gt(p.var("n", "vn"), N(4)),
                 lambda c: c.change("n", "vn", N(1)),
                 opcode="control_repeat_until")
    a(("-- repeat until n>4", until, "stack"))
    a(("until -> n", lambda p: p.var("n", "vn")))

    # while with a false condition
    def whiles(p):
        p.set("n", "vn", N(7))
        p.repeat(p.lt(p.var("n", "vn"), N(3)),
                 lambda c: c.change("n", "vn", N(1)),
                 opcode="control_while")
    a(("-- while n<3 (false)", whiles, "stack"))
    a(("while -> n", lambda p: p.var("n", "vn")))

    # for each over a 3-item list, appending while iterating
    def fe(p):
        p.set("n", "vn", N(0))
        p.for_each("l1", "v1", "i", "vi",
                   lambda c: (c.change("n", "vn", N(1)),
                              c.if_(p.lt(p.var("n", "vn"), N(10)),
                                    lambda d: d.add("l1", "v1", T("x")))))
    a(("-- for each + append", fe, "stack"))
    a(("foreach -> n", lambda p: p.var("n", "vn")))
    a(("foreach -> len", lambda p: p.listlen("l1", "v1")))
    a(("foreach -> i", lambda p: p.var("i", "vi")))

    # stop this script inside a loop
    def stopscript(p):
        p.set("n", "vn", N(0))
        p.set("m", "vm", N(0))
        p.repeat(N(5), lambda c: (c.change("n", "vn", N(1)),
                                  c.if_(p.eq(p.var("n", "vn"), N(2)),
                                        lambda d: d.stop("this script"))))
        p.set("m", "vm", N(99))
    a(("-- stop this script", stopscript, "stack"))
    a(("stop script -> n", lambda p: p.var("n", "vn")))
    a(("stop script -> m", lambda p: p.var("m", "vm")))

    # delete this clone outside a clone: stops the script
    def delclone(p):
        p.set("n", "vn", N(0))
        p.push("control_delete_this_clone")
        p.set("n", "vn", N(99))
    a(("-- delete this clone (no clone)", delclone, "stack"))
    a(("delete clone -> n", lambda p: p.var("n", "vn")))

    # clone creation up to the runtime limit (no deletion)
    def clones(p):
        p.set("n", "vn", N(0))
        p.repeat(N(320), lambda c: c.push(
            "control_create_clone_of",
            {"CLONE_OPTION": [1, c.proj._new(
                "control_create_clone_of_menu", {},
                {"CLONE_OPTION": ["_myself_", None]}, shadow=True)]}))
    a(("-- create 320 clones", clones, "stack"))
    a(("clone -> n", lambda p: p.var("n", "vn")))

    # stop all
    def stopall(p):
        p.set("n", "vn", N(1))
        p.push("control_stop", {}, {"STOP_OPTION": ["all", None]})
        p.set("n", "vn", N(2))
    a(("-- stop all", stopall, "stack"))
    a(("stop all -> n", lambda p: p.var("n", "vn")))
    build_battery("control", P, lists=L, variables=V)


BATTERIES = {"num": battery_num, "str": battery_str, "list": battery_list,
             "math2": battery_math2, "control": battery_control}

if __name__ == "__main__":
    want = sys.argv[1:] or list(BATTERIES)
    for name in want:
        BATTERIES[name]()
        print("wrote", name)
