#!/usr/bin/env python3
"""Looks/sensing probes: costume + backdrop switching, sensing_current.

    python3 tests/differential/probe/gen_looks.py

These are stack probes (the switch) followed by a value probe (the observable
number/name), so the battery is one long deterministic script.
"""
import json
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import Battery, build_battery, PROBE_DIR  # noqa: E402

N = Battery.nlit
T = Battery.slit

SPRITE_COSTUMES = ["a", "B", "c"]
STAGE_BACKDROPS = ["b1", "b2", "b3"]


def patch_costumes(name, sprite_names, stage_names):
    path = os.path.join(PROBE_DIR, name + ".sb3")
    with zipfile.ZipFile(path) as z:
        proj = json.loads(z.read("project.json"))
        svg = None
        for nm in z.namelist():
            if nm.endswith(".svg"):
                svg = z.read(nm)
    import gen_repros
    md5 = gen_repros.SVG_MD5
    for target, names in ((proj["targets"][0], stage_names),
                          (proj["targets"][1], sprite_names)):
        target["costumes"] = [
            {"name": nm, "assetId": md5, "md5ext": md5 + ".svg",
             "dataFormat": "svg", "rotationCenterX": 1, "rotationCenterY": 1}
            for nm in names]
        target["currentCostume"] = 0
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(proj, ensure_ascii=True))
        z.writestr(md5 + ".svg", svg)


def numin(p, v):
    """A real number input: a reporter block (typeof === 'number')."""
    return [3, p.add(N(0), N(v))[1], [10, ""]]


def costume_battery():
    P = []
    a = P.append
    num = lambda p: p._rep("looks_costumenumbername", {},
                           {"NUMBER_NAME": ["number", None]})
    nm = lambda p: p._rep("looks_costumenumbername", {},
                          {"NUMBER_NAME": ["name", None]})

    for val, label in ((T("B"), "'B'"), (T("b"), "'b'"), (T(" 2 "), "' 2 '"),
                       (T("0"), "'0'"), (T("-1"), "'-1'"), (T("1.5"), "'1.5'"),
                       (T(""), "''"), (T(" "), "' '"), (T("abc"), "'abc'"),
                       (T("2.0"), "'2.0'"), (T("1e400"), "'1e400'"),
                       (T("next costume"), "'next costume'"),
                       (T("previous costume"), "'previous costume'")):
        a(("-- costume %s" % label,
           lambda p, v=val: p.push("looks_switchcostumeto", {"COSTUME": v}),
           "stack"))
        a(("  -> %s" % label, num))
    for v in ("2.5", "0", "-1", "1e400", "0.5", "2"):
        a(("-- costume num %s" % v,
           lambda p, v=v: p.push("looks_switchcostumeto",
                                 {"COSTUME": numin(p, v)}), "stack"))
        a(("  -> num %s" % v, num))
        a(("  -> name num %s" % v, nm))
    a(("-- costume NaN", lambda p: p.push(
        "looks_switchcostumeto",
        {"COSTUME": [3, p.div(N(0), N(0))[1], [10, ""]]}), "stack"))
    a(("  -> after NaN", num))
    a(("  -> name after NaN", nm))
    a(("-- next costume x2", lambda p: (p.push("looks_nextcostume"),
                                       p.push("looks_nextcostume")), "stack"))
    a(("  -> after next x2", num))
    a(("  -> name after next x2", nm))
    a(("-- switch to 3", lambda p: p.push("looks_switchcostumeto",
                                          {"COSTUME": numin(p, "3")}), "stack"))
    a(("  -> after 3", num))

    build_battery("costume", P)
    patch_costumes("costume", SPRITE_COSTUMES, STAGE_BACKDROPS)


def backdrop_battery():
    P = []
    a = P.append
    num = lambda p: p._rep("looks_backdropnumbername", {},
                           {"NUMBER_NAME": ["number", None]})
    nm = lambda p: p._rep("looks_backdropnumbername", {},
                          {"NUMBER_NAME": ["name", None]})

    def sw(val, label):
        a(("-- backdrop %s" % label,
           lambda p: p.push("looks_switchbackdropto", {"BACKDROP": val}),
           "stack"))
        a(("  -> %s" % label, num))
        a(("  -> name %s" % label, nm))

    for val, label in ((T("b2"), "'b2'"), (T("B2"), "'B2'"), (T("next backdrop"), "'next'"),
                       (T("previous backdrop"), "'previous'"),
                       (T(""), "''"), (T("  "), "'  '"), (N("1.5"), "1.5"),
                       (T("abc"), "'abc'")):
        sw(val, label)
    for v in ("3", "0", "1.5"):
        a(("-- backdrop num %s" % v,
           lambda p, v=v: p.push("looks_switchbackdropto",
                                 {"BACKDROP": numin(p, v)}), "stack"))
        a(("  -> num %s" % v, num))
        a(("  -> name num %s" % v, nm))
    a(("-- next backdrop", lambda p: p.push("looks_nextbackdrop"), "stack"))
    a(("  -> after next backdrop", num))
    build_battery("backdrop", P)
    patch_costumes("backdrop", SPRITE_COSTUMES, STAGE_BACKDROPS)


def current_battery():
    P = []
    a = P.append
    for part in ("year", "month", "date", "dayofweek", "hour", "minute",
                 "second"):
        a(("current " + part,
           (lambda part: lambda p: p._rep(
               "sensing_current", {},
               {"CURRENTMENU": [part.upper(), None]}))(part)))
    a(("timer >= 0", lambda p: p.gt(p._rep("sensing_timer"), N(-1))))
    a(("mousedown", lambda p: p._rep("sensing_mousedown")))
    a(("keypressed space",
       lambda p: p._rep("sensing_keypressed",
                        {"KEY_OPTION": [1, p._new(
                            "sensing_keyoptions", {},
                            {"KEY_OPTION": ["space", None]}, shadow=True)]})))
    a(("username", lambda p: p._rep("sensing_username")))
    a(("loudness", lambda p: p._rep("sensing_loudness")))
    a(("mousex", lambda p: p._rep("sensing_mousex")))
    build_battery("current", P)


if __name__ == "__main__":
    costume_battery()
    backdrop_battery()
    current_battery()
    print("wrote looks probes")
