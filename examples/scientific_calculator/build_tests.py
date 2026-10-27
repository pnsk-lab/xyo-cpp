#!/usr/bin/env python3
"""Generate the headless evaluator test fixture (xyo test)."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "tools"))

from model import build_model
from sb3 import Builder, write_sb3, costume
from evaluator import Evaluator

CASES = [
    ("1+2*3", "7", "DEG"),
    ("(1+2)*3", "9", "DEG"),
    ("2^3^2", "512", "DEG"),
    ("-2^2", "-4", "DEG"),
    ("2^-3", "0.125", "DEG"),
    ("sin(30)", "0.5", "DEG"),
    ("cos(60)", "0.5", "DEG"),
    ("tan(45)", "1", "DEG"),
    ("asin(0.5)", "30", "DEG"),
    ("ln(e)", "1", "DEG"),
    ("log(1000)", "3", "DEG"),
    ("sqrt(2)", "1.41421356237", "DEG"),
    ("5!", "120", "DEG"),
    ("0.5!", "0.886226925453", "DEG"),
    ("2pi", "6.28318530718", "DEG"),
    ("2(3+4)", "14", "DEG"),
    ("50%", "0.5", "DEG"),
    ("1e3", "1000", "DEG"),
    ("1/3", "0.333333333333", "DEG"),
    ("0.1+0.2", "0.3", "DEG"),
    ("1/0", "ERR", "DEG"),
    ("sqrt(-1)", "i", "DEG"),
    ("foo(1)", "ERR", "DEG"),
    ("1+", "ERR", "DEG"),
    ("(1+2", "3", "DEG"),
    ("sin(30", "0.5", "DEG"),
    ("min(3,1,2)", "1", "DEG"),
    ("mod(10,3)", "1", "DEG"),
    ("mod(-1,3)", "2", "DEG"),
    ("ncr(5,2)", "10", "DEG"),
    ("gamma(5)", "24", "DEG"),
    ("2^100", "1.26765060023E+30", "DEG"),
    ("170!", "7.25741561531E+306", "DEG"),
    ("171!", "ERR", "DEG"),
    ("1e-7", "1E-7", "DEG"),
    ("999999999999", "999999999999", "DEG"),
    ("10^12", "1E+12", "DEG"),
    ("sin(pi/2)", "1", "RAD"),
    ("asin(1)", "1.57079632679", "RAD"),
    ("atan2(1,1)", "0.785398163397", "RAD"),
    ("gcd(12,18)", "6", "DEG"),
    ("lcm(4,6)", "12", "DEG"),
    ("sum(1,2,3)", "6", "DEG"),
    ("avg(1,2,3)", "2", "DEG"),
    ("hypot(3,4)", "5", "DEG"),
    ("root(27,3)", "3", "DEG"),
    ("npr(5,2)", "20", "DEG"),
    ("log(2,8)", "3", "DEG"),
    ("abs(-5)", "5", "DEG"),
    ("floor(2.9)", "2", "DEG"),
    ("ceil(2.1)", "3", "DEG"),
    ("round(2.5)", "3", "DEG"),
    ("sign(-3)", "-1", "DEG"),
    ("deg(pi)", "180", "DEG"),
    ("rad(180)", "3.14159265359", "DEG"),
    ("phi", "1.61803398875", "DEG"),
    ("tau", "6.28318530718", "DEG"),
    ("x", "0", "DEG"),
    # complex numbers
    ("i", "i", "DEG"),
    ("-i", "-i", "DEG"),
    ("2i", "2i", "DEG"),
    ("i*i", "-1", "DEG"),
    ("i^2", "-1", "DEG"),
    ("i^3", "-i", "DEG"),
    ("i^4", "1", "DEG"),
    ("i*j", "k", "DEG"),
    ("j*i", "-k", "DEG"),
    ("j*k", "i", "DEG"),
    ("k*j", "-i", "DEG"),
    ("i+j", "i+j", "DEG"),
    ("1+i", "1+i", "DEG"),
    ("(1+i)*(1-i)", "2", "DEG"),
    ("(1+2i)+(3-4i)", "4-2i", "DEG"),
    ("(1+2i)/(3+4i)", "0.44+0.08i", "DEG"),
    ("i*j*k", "-1", "DEG"),
    ("j^2", "-1", "DEG"),
    ("k^2", "-1", "DEG"),
    ("1+2i+3j+4k", "1+2i+3j+4k", "DEG"),
    ("(1+i)^2", "2i", "DEG"),
    ("exp(i*pi)", "-1", "DEG"),
    ("exp(i*pi/2)", "i", "DEG"),
    # real factorial / gamma
    ("gamma(0.5)", "1.77245385091", "DEG"),
    ("1.5!", "1.32934038818", "DEG"),
    ("(-0.5)!", "1.77245385091", "DEG"),
    ("(-1)!", "ERR", "DEG"),
    ("i!", "ERR", "DEG"),
    ("sqrt(-4)", "2i", "DEG"),
    ("1/i", "-i", "DEG"),
    ("(-1)^0.5", "i", "DEG"),
    ("i^0.5", "0.7071067812+0.7071067812i", "DEG"),
    ("sqrt(i)", "0.7071067812+0.7071067812i", "DEG"),
    ("ln(-1)", "3.14159265359i", "DEG"),
    ("log(-100)", "2+1.36437635384i", "DEG"),
    ("abs(3+4i)", "5", "DEG"),
    ("conj(3+4i)", "3-4i", "DEG"),
    ("sin(i)", "1.17520119364i", "DEG"),
    ("i^i", "0.207879576351", "DEG"),
    ("asin(2)", "1.57079632679-1.31695789692i", "DEG"),
    ("acos(2)", "1.31695789692i", "DEG"),
    ("pow(-8,1/3)", "1+1.7320508076i", "DEG"),
    ("root(-16,2)", "4i", "DEG"),
    ("cbrt(-27)", "-3", "DEG"),
    ("abs(1+2i+3j+4k)", "5.47722557505", "DEG"),
    ("conj(1+2i+3j+4k)", "1-2i-3j-4k", "DEG"),
    ("exp(j*pi)", "-1", "DEG"),
    ("exp(j*pi/2)", "j", "DEG"),
    ("gamma(2.5)", "1.32934038818", "DEG"),
    ("gamma(-0.5)", "-3.54490770181", "DEG"),
    ("gamma(i)", "ERR", "DEG"),
    ("log(j)", "0.682188176921j", "DEG"),
    ("ln(j)", "1.57079632679j", "DEG"),
    ("sqrt(j)", "0.7071067812+0.7071067812j", "DEG"),
    ("(1+i)*(1+j)", "1+i+j+k", "DEG"),
    ("(1+2j)*(3+4k)", "3+8i+6j+4k", "DEG"),
    ("arg(1+i)", "0.785398163397", "DEG"),
    ("re(3+4i)", "3", "DEG"),
    ("im(3+4i)", "4", "DEG"),
    ("jpart(1+2j)", "2", "DEG"),
    ("kpart(1+3k)", "3", "DEG"),
    ("norm(1+2i+3j+4k)", "5.47722557505", "DEG"),
    ("inv(1+i)", "0.5-0.5i", "DEG"),
    ("cos(i)", "1.54308063482", "DEG"),
    ("tan(i)", "0.761594155956i", "DEG"),
    ("sinh(i)", "0.8414709848i", "DEG"),
    ("cosh(i)", "0.5403023059", "DEG"),
    ("atanh(2)", "0.549306144334+1.57079632679i", "DEG"),
    ("acosh(0.5)", "1.0471975512i", "DEG"),
    ("gamma(2.5)", "1.32934038818", "DEG"),
    ("2.5!", "3.32335097045", "DEG"),
    ("(-1.5)!", "-3.54490770181", "DEG"),
    ("0!", "1", "DEG"),
    ("gamma(0)", "ERR", "DEG"),
    ("gamma(-1)", "ERR", "DEG"),
    ("1e-13", "1E-13", "DEG"),
    ("sum(1+i,2+3i)", "3+4i", "DEG"),
    ("avg(1+i,3+3i)", "2+2i", "DEG"),
    ("sum(1+2j,3+4k)", "4+2j+4k", "DEG"),
    ("avg(1+2j,3+4k)", "2+j+2k", "DEG"),
]

TESTER_SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2">'
              '<rect width="2" height="2" fill="#000" fill-opacity="0"/></svg>')
BG_SVG = ('<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">'
          '<rect width="480" height="360" fill="#000"/></svg>')


def build(path, cases=CASES):
    m = build_model()
    m.list_values["testExpr"] = [c[0] for c in cases]
    m.list_values["testMode"] = [c[2] for c in cases]
    m.list_values["testExpected"] = [c[1] for c in cases]
    b = Builder(m)
    ev = Evaluator(b)
    p = ev.p

    run = b.defproc("runTests", [], True)
    i = b.vvar("testIndex")
    item = lambda name: b.vblock(b.list_item(name, i))

    def msg(expr):
        return b.setv("msg", expr)

    body = [
        b.setv("testIndex", b.vnum(1)),
        b.repeat(b.vblock(b.list_len("testExpr")), [
            b.setv("angleMode", item("testMode")),
            b.setv("expr", item("testExpr")),
            b.call(p["evaluate"], [b.vblock(b.var("expr"))]),
            b.ifelse(b.eq(b.vvar("err"), b.vnum(1)),
                     [
                         b.ifelse(b.eq(item("testExpected"), b.vtext("ERR")),
                                  [msg(b.vtext("pass"))],
                                  [msg(b.vtext("fail"))]),
                     ],
                     [
                         b.call(p["fmtVal"], [b.vblock(b.var("result")),
                                              b.vblock(b.var("resultIm")),
                                              b.vblock(b.var("resultJ")),
                                              b.vblock(b.var("resultK"))]),
                         b.ifelse(b.eq(b.vvar("fmtValOut"), item("testExpected")),
                                  [msg(b.vtext("pass"))],
                                  [msg(b.vtext("fail"))]),
                     ]),
            b.list_add("testLog", b.vvar("msg")),
            b.op("looks_say", {"MESSAGE": b.vblock(b.var("msg"))}),
            b.changev("testIndex", b.vnum(1)),
        ]),
        b.op("looks_say", {"MESSAGE": b.vtext("end")}),
    ]
    run.body(body)
    hat = b._add("event_whenflagclicked", top=True, x=0, y=0)
    first = b.call(run, [])
    b.stack(hat, [first])
    b.blocks[hat]["next"] = first

    sprite = b.sprite_json(
        "Tester",
        [costume("normal", TESTER_SVG, 1, 1)],
        b.blocks, layer=1, visible=False,
    )
    stage = b.stage_json([
        costume("calc", BG_SVG, 240, 180),
        costume("graph", BG_SVG, 240, 180),
    ])
    project = {
        "targets": [stage, sprite],
        "monitors": [],
        "extensions": ["pen"],
        "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": "calculator-tests"},
    }
    write_sb3(path, project, {"tester": TESTER_SVG, "bg": BG_SVG})
    print("wrote", path, "cases:", len(cases))


if __name__ == "__main__":
    build(sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/calculator_tests.sb3")
