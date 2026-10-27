#!/usr/bin/env python3
"""Motion probes: position/direction/size extremes and rotation styles."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import Battery, build_battery, PROBE_DIR  # noqa: E402

N = Battery.nlit
T = Battery.slit


def numin(p, v):
    return [3, p.add(N(0), N(v))[1], [10, ""]]


def nanin(p):
    return [3, p.div(N(0), N(0))[1], [10, ""]]


def main():
    P = []
    a = P.append
    x = lambda p: p._rep("motion_xposition")
    y = lambda p: p._rep("motion_yposition")
    d = lambda p: p._rep("motion_direction")
    sz = lambda p: p._rep("looks_size")

    def st(pushfn, label, expr):
        a(("-- " + label, pushfn, "stack"))
        a(("  " + label, expr))

    for v in ("1e300", "-1e300", "1000", "-1000", "240", "-240"):
        st(lambda p, v=v: p.push("motion_setx", {"X": numin(p, v)}),
           "set x " + v, x)
    st(lambda p: p.push("motion_setx", {"X": nanin(p)}), "set x NaN", x)
    st(lambda p: p.push("motion_setx", {"X": N(1)}), "set x 1", x)
    for v in ("1e300", "1000", "-1000"):
        st(lambda p, v=v: p.push("motion_sety", {"Y": numin(p, v)}),
           "set y " + v, y)
    st(lambda p: p.push("motion_sety", {"Y": nanin(p)}), "set y NaN", y)
    st(lambda p: p.push("motion_sety", {"Y": N(2)}), "set y 2", y)

    for v in ("180.5", "181", "359", "-180", "-179.5", "360", "1e300",
              "-1e300", "0.5"):
        st(lambda p, v=v: p.push("motion_pointindirection",
                                 {"DIRECTION": numin(p, v)}),
           "point " + v, d)
    st(lambda p: p.push("motion_pointindirection", {"DIRECTION": nanin(p)}),
       "point NaN", d)
    st(lambda p: p.push("motion_pointindirection",
                        {"DIRECTION": T("abc")}), "point 'abc'", d)
    st(lambda p: p.push("motion_pointindirection",
                        {"DIRECTION": T("")}), "point ''", d)
    st(lambda p: p.push("motion_pointindirection", {"DIRECTION": N(90)}),
       "point 90", d)
    st(lambda p: p.push("motion_turnright", {"DEGREES": numin(p, "90")}),
       "turn right 90", d)
    st(lambda p: p.push("motion_turnleft", {"DEGREES": numin(p, "180")}),
       "turn left 180", d)
    st(lambda p: p.push("motion_turnright", {"DEGREES": N("1e400")}),
       "turn right inf", d)
    st(lambda p: p.push("motion_turnright", {"DEGREES": nanin(p)}),
       "turn right NaN", d)

    for style in ("don't rotate", "left-right", "all around"):
        st(lambda p, s=style: p.push("motion_setrotationstyle", {},
                                     {"STYLE": [s, None]}),
           "style " + style, lambda p: d(p))
        st(lambda p: p.push("motion_pointindirection",
                            {"DIRECTION": numin(p, "45")}),
           "style then point 45", d)

    # size: keep >= 100 so the renderer's fencing clamp (which depends on
    # costume size) does not dominate; probes below 100 are still listed.
    for v in ("150", "1000", "50", "1", "0", "-5", "10000"):
        st(lambda p, v=v: p.push("looks_setsizeto", {"SIZE": numin(p, v)}),
           "size " + v, sz)
    st(lambda p: p.push("looks_setsizeto", {"SIZE": nanin(p)}), "size NaN", sz)
    st(lambda p: p.push("looks_changesizeby", {"CHANGE": numin(p, "10")}),
       "size +10", sz)
    st(lambda p: p.push("looks_setsizeto", {"SIZE": T("150.5")}),
       "size '150.5'", sz)

    build_battery("motion", P)


if __name__ == "__main__":
    main()
    print("wrote motion probes")
