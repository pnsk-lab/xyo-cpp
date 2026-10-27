#!/usr/bin/env python3
"""Build minimal Scratch projects that expose xyo-cpp divergences.

Every project uses only stock Scratch 3 blocks.  Results are appended to the
stage-global list ``out`` so the same file can be executed by scratch-vm and
by xyo and compared with ``run_repros.py``.

    python3 tests/differential/gen_repros.py [outdir]

The default outdir is ``tests/differential/repro``.  Each generated file is
checked in, so regenerating is only necessary after editing this script.
"""
import hashlib
import json
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

# A 2x2 empty SVG keeps scratch-parser happy (it requires >=1 costume).
SVG = b'<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2"></svg>'
SVG_MD5 = hashlib.md5(SVG).hexdigest()
COSTUME = [{"name": "c1", "assetId": SVG_MD5, "md5ext": SVG_MD5 + ".svg",
            "dataFormat": "svg", "rotationCenterX": 1, "rotationCenterY": 1}]


class Project:
    """Tiny sb3 writer.

    A block id is returned by every constructor.  Values that are used as
    inputs must be wrapped by the caller (``[1, id]`` / ``[3, id, shadow]``);
    a reporter must never also be a member of the ``next`` chain.
    """

    def __init__(self):
        self.blocks = {}
        self._n = 0
        self._prev = None
        self.variables = None
        self.lists = None

    def _new(self, opcode, inputs=None, fields=None, parent=None, top=False,
             shadow=False, x=0, y=0):
        self._n += 1
        bid = "b%d" % self._n
        b = {"opcode": opcode, "next": None, "parent": parent,
             "inputs": inputs or {}, "fields": fields or {},
             "topLevel": top, "shadow": shadow}
        if top:
            b["x"], b["y"] = x, y
        self.blocks[bid] = b
        return bid

    # ---- expression helpers -------------------------------------------
    def var(self, name, vid, parent=None):
        return self._new("data_variable", {}, {"VARIABLE": [name, vid]},
                         parent=parent)

    def num(self, v):
        return [1, [4, str(v)]]

    def txt(self, s):
        return [1, [10, s]]

    def binop(self, opcode, a, b):
        return self._new(opcode, {"NUM1": a, "NUM2": b})

    def mathop(self, op, a):
        return self._new("operator_mathop", {"NUM": a},
                         {"OPERATOR": [op, None]})

    def round(self, a):
        return self._new("operator_round", {"NUM": a})

    def join(self, a, b):
        return self._new("operator_join", {"STRING1": a, "STRING2": b})

    def item(self, list_name, list_id, index):
        return self._new("data_itemoflist", {"INDEX": index},
                         {"LIST": [list_name, list_id]})

    # ---- stack helpers ------------------------------------------------
    def start(self):
        self._prev = self._new("event_whenflagclicked", top=True)
        return self._prev

    def push(self, opcode, inputs=None, fields=None):
        bid = self._new(opcode, inputs or {}, fields or {}, parent=self._prev)
        self.blocks[self._prev]["next"] = bid
        self._prev = bid
        return bid

    def add_out(self, expr_input):
        return self.push("data_addtolist", {"ITEM": expr_input},
                         {"LIST": ["out", "vout"]})

    def set_var(self, name, vid, value_input):
        return self.push("data_setvariableto", {"VALUE": value_input},
                         {"VARIABLE": [name, vid]})

    def repeat_one(self):
        """A one-iteration ``repeat``: its presence makes HotSpot compile the
        script on the first run, which is what exposes JIT-only divergences."""
        rep = self.push("control_repeat", {"TIMES": self.num(1)})
        return rep

    def repeat_body(self, rep, opcode, inputs=None, fields=None):
        body = self._new(opcode, inputs or {}, fields or {}, parent=rep)
        self.blocks[rep]["inputs"]["SUBSTACK"] = [2, body]
        self._prev = rep
        return body

    def build(self, stage_vars=None):
        stage = {"isStage": True, "name": "Stage",
                 "variables": stage_vars or {},
                 "lists": {"vout": ["out", []]}, "broadcasts": {},
                 "blocks": {}, "comments": {}, "costumes": COSTUME,
                 "sounds": [], "currentCostume": 0, "volume": 100,
                 "layerOrder": 0}
        sprite = {"isStage": False, "name": "S", "layerOrder": 1,
                  "variables": self.variables or {}, "lists": self.lists or {},
                  "broadcasts": {}, "blocks": self.blocks, "comments": {},
                  "costumes": COSTUME, "sounds": [], "currentCostume": 0,
                  "volume": 100, "visible": True, "x": 0, "y": 0,
                  "size": 100, "direction": 90, "draggable": False,
                  "rotationStyle": "all around"}
        return {"targets": [stage, sprite], "monitors": [], "extensions": [],
                "meta": {"semver": "3.0.0", "vm": "0.2.0", "agent": "xyo-diff"}}

    def write(self, path):
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("project.json",
                       json.dumps(self.build(), ensure_ascii=False))
            z.writestr(SVG_MD5 + ".svg", SVG)


# ---------------------------------------------------------------------------
# 1. NaN list index -> out-of-bounds memmove in xyo
def nan_delete():
    p = Project()
    p.start()
    nan = p.binop("operator_divide", p.num(0), p.num(0))   # 0 / 0
    p.push("data_deleteoflist", {"INDEX": [1, nan]}, {"LIST": ["L", "vL"]})
    p.add_out([3, p.item("L", "vL", p.num(1)), [10, "0"]])
    p.lists = {"vL": ["L", ["a", "b", "c"]]}
    p.write(os.path.join(OUT, "nan_index_delete.sb3"))


def nan_insert():
    p = Project()
    p.start()
    nan = p.binop("operator_divide", p.num(0), p.num(0))
    p.push("data_insertatlist", {"INDEX": [1, nan], "ITEM": p.txt("X")},
           {"LIST": ["L", "vL"]})
    p.add_out([3, p.item("L", "vL", p.num(1)), [10, "0"]])
    p.lists = {"vL": ["L", ["a", "b", "c"]]}
    p.write(os.path.join(OUT, "nan_index_insert.sb3"))


# 2. Number -> string: ES uses the shortest round-tripping decimal.
def big_numbers():
    p = Project()
    p.start()
    base = "9007199254740992"                                   # 2^53
    for k in (16, 8, 4):                                        # 2^57, 2^56, 2^55
        e = p.binop("operator_multiply", p.num(base), p.num(k))
        j = p.join(p.txt(""), [1, e])
        p.add_out([3, j, [10, "0"]])
    j = p.join(p.txt(""), [1, p.var("nv", "vnv")])
    p.add_out([3, j, [10, "0"]])
    p.write(os.path.join(OUT, "big_number_string.sb3"))
    # Patch a real JSON number into the stage variable.  A math_number field
    # would keep the literal as a string in Scratch and hide the bug.
    path = os.path.join(OUT, "big_number_string.sb3")
    with zipfile.ZipFile(path) as z:
        proj = json.loads(z.read("project.json"))
    proj["targets"][0]["variables"]["vnv"] = ["nv", 123456789012345678]
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(proj, ensure_ascii=False))
        z.writestr(SVG_MD5 + ".svg", SVG)


# 3. Unicode case folding: only BMP lowercase mappings exist in xyo.
def unicode_casefold():
    p = Project()
    p.start()
    for a, b in [("\U00010400", "\U00010428"),   # Deseret capital/small
                 ("\u00c4", "\u00e4"),           # Ä / ä (BMP control)
                 ("\u212a", "k")]:               # Kelvin sign / k
        eq = p._new("operator_equals",
                    {"OPERAND1": p.txt(a), "OPERAND2": p.txt(b)})
        ifb = p.push("control_if_else", {"CONDITION": [2, eq]})
        t = p._new("data_addtolist", {"ITEM": p.txt("1")},
                   {"LIST": ["out", "vout"]}, parent=ifb)
        f = p._new("data_addtolist", {"ITEM": p.txt("0")},
                   {"LIST": ["out", "vout"]}, parent=ifb)
        p.blocks[ifb]["inputs"]["SUBSTACK"] = [2, t]
        p.blocks[ifb]["inputs"]["SUBSTACK2"] = [2, f]
        p._prev = ifb
    p.write(os.path.join(OUT, "unicode_casefold.sb3"))


# 4. round() negative halves: JS Math.round() vs std::round().
def round_half():
    p = Project()
    p.start()
    for v in ("-0.5", "-1.5", "-2.5", "0.5", "1.5"):
        p.add_out([3, p.round(p.num(v)), [10, "0"]])
    q = p.round(p.num("-0.5"))
    p.add_out([3, p.binop("operator_divide", p.num(1), [1, q]), [10, "0"]])
    p.write(os.path.join(OUT, "round_negative_half.sb3"))


# 5. JIT mod: the inlined ((n % m) + m) % m differs from Scratch's
#    (n % m) + (m if (n % m) / m < 0) for infinite / huge / tiny operands.
def jit_mod():
    p = Project()
    p.start()
    cases = [("1", "Infinity"), ("0", "Infinity"), ("-1", "-Infinity"),
             ("1", "1e21"), ("1e-7", "1"), ("5e-324", "-1")]
    for a, b in cases:
        p.set_var("x", "vx", p.num(a))
        rep = p.repeat_one()
        p.repeat_body(rep, "data_setvariableto",
                      {"VALUE": [1, p.binop("operator_mod",
                                            [1, p.var("x", "vx")], p.num(b))]},
                      {"VARIABLE": ["x", "vx"]})
        p.add_out([3, p.join(p.txt(""), [1, p.var("x", "vx")]), [10, "0"]])
    p.variables = {"vx": ["x", 0]}
    p.write(os.path.join(OUT, "jit_mod_extremes.sb3"))


# 6. JIT sin/cos: x*(pi/180) never overflows, Scratch's (pi*x)/180 does.
def jit_trig():
    p = Project()
    p.start()
    for op in ("sin", "cos"):
        p.set_var("x", "vx", p.num("1e308"))
        rep = p.repeat_one()
        p.repeat_body(rep, "data_setvariableto",
                      {"VALUE": [1, p.mathop(op, [1, p.var("x", "vx")])]},
                      {"VARIABLE": ["x", "vx"]})
        p.add_out([3, p.join(p.txt(op + "="), [1, p.var("x", "vx")]),
                   [10, "0"]])
    p.variables = {"vx": ["x", 0]}
    p.write(os.path.join(OUT, "jit_trig_overflow.sb3"))


# 7. JIT inverse trig: (asin(x) * 180) / pi vs asin(x) * (180 / pi).
def jit_invtrig():
    p = Project()
    p.start()
    for op, v in [("asin", "1e-7"), ("acos", "0.5"), ("atan", "1")]:
        p.set_var("x", "vx", p.num(v))
        rep = p.repeat_one()
        p.repeat_body(rep, "data_setvariableto",
                      {"VALUE": [1, p.mathop(op, [1, p.var("x", "vx")])]},
                      {"VARIABLE": ["x", "vx"]})
        p.add_out([3, p.join(p.txt(op + "(" + v + ")="),
                             [1, p.var("x", "vx")]), [10, "0"]])
    p.variables = {"vx": ["x", 0]}
    p.write(os.path.join(OUT, "jit_invtrig_ulp.sb3"))


# 8. JIT negative zero: +0.0 normalises -0 in round and mod.
def jit_negzero():
    p = Project()
    p.start()
    negzero = p.binop("operator_multiply", p.num(0), p.num(-1))   # -0
    # round(-0.4) -> -0 in Scratch
    p.set_var("x", "vx", p.num("-0.4"))
    rep = p.repeat_one()
    p.repeat_body(rep, "data_setvariableto",
                  {"VALUE": [1, p.round([1, p.var("x", "vx")])]},
                  {"VARIABLE": ["x", "vx"]})
    p.add_out([3, p.binop("operator_divide", p.num(1), [1, p.var("x", "vx")]),
               [10, "0"]])
    # round(-0) -> -0
    p.set_var("x", "vx", [1, negzero])
    rep = p.repeat_one()
    p.repeat_body(rep, "data_setvariableto",
                  {"VALUE": [1, p.round([1, p.var("x", "vx")])]},
                  {"VARIABLE": ["x", "vx"]})
    p.add_out([3, p.binop("operator_divide", p.num(1), [1, p.var("x", "vx")]),
               [10, "0"]])
    # mod(-0, 1) -> -0
    p.set_var("x", "vx", [1, p.binop("operator_multiply", p.num(0), p.num(-1))])
    rep = p.repeat_one()
    p.repeat_body(rep, "data_setvariableto",
                  {"VALUE": [1, p.binop("operator_mod",
                                        [1, p.var("x", "vx")], p.num(1))]},
                  {"VARIABLE": ["x", "vx"]})
    p.add_out([3, p.binop("operator_divide", p.num(1), [1, p.var("x", "vx")]),
               [10, "0"]])
    p.variables = {"vx": ["x", 0]}
    p.write(os.path.join(OUT, "jit_negzero.sb3"))


# 9. Costume index rounding: Target::setCostume uses std::round, Scratch uses
#    Math.round.  A .5 index therefore selects a different costume.
def costume_rounding():
    p = Project()
    p.start()
    for v in ("-0.5", "-1.5", "-2.5", "0.5", "1.5", "2.5", "-3.5"):
        n = p._new("operator_add", {"NUM1": p.num(v), "NUM2": p.num(0)})
        p.push("looks_switchcostumeto", {"COSTUME": [3, n, [10, "1"]]})
        num = p._new("looks_costumenumbername", {},
                     {"NUMBER_NAME": ["number", None]})
        name = p._new("looks_costumenumbername", {},
                      {"NUMBER_NAME": ["name", None]})
        jn = p.join(p.txt(v + "|#"), [1, num])
        jm = p.join([1, jn], p.txt("|"))
        jm = p.join([1, jm], [1, name])
        p.add_out([3, jm, [10, "0"]])
    proj = p.build()
    costumes = []
    for nm in ("c1", "c2", "c3"):
        costumes.append({"name": nm, "assetId": SVG_MD5,
                         "md5ext": SVG_MD5 + ".svg", "dataFormat": "svg",
                         "rotationCenterX": 1, "rotationCenterY": 1})
    proj["targets"][1]["costumes"] = costumes
    with zipfile.ZipFile(os.path.join(OUT, "costume_rounding.sb3"), "w",
                         zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(proj, ensure_ascii=False))
        z.writestr(SVG_MD5 + ".svg", SVG)


# 10. libm vs V8: same double, different last digit (informational).
def libm_ulp():
    p = Project()
    p.start()
    for op, v in [("ln", "3"), ("log", "3")]:
        p.add_out([3, p.join(p.txt(op + "="), [1, p.mathop(op, p.num(v))]),
                   [10, "0"]])
    p.write(os.path.join(OUT, "libm_ln_log.sb3"))



# 11. glibc vs V8 fdlibm: asin/acos/atan/exp differ in the last bit.
def mislibm():
    p = Project()
    p.start()
    cases = [
        ("asin", "0.7137281021284307"),
        ("asin", "-0.5797469269752726"),
        ("acos", "-0.5467314076748808"),
        ("acos", "0.4053648469011235"),
        ("atan", "0.04135948024180158"),
        ("atan", "1.2132071766933517"),
        ("e ^", "0.33089555746090094"),
        ("e ^", "-0.8499533886188995"),
        ("e ^", "0.2600296241107938"),
    ]
    for op, v in cases:
        p.set_var("x", "vx", p.num(v))
        rep = p.repeat_one()
        p.repeat_body(rep, "data_setvariableto",
                      {"VALUE": [1, p.mathop(op, [1, p.var("x", "vx")])]},
                      {"VARIABLE": ["x", "vx"]})
        label = "exp" if op == "e ^" else op
        p.add_out([3, p.join(p.txt(label + "="), [1, p.var("x", "vx")]),
                   [10, "0"]])
    p.variables = {"vx": ["x", 0]}
    p.write(os.path.join(OUT, "math_fdlibm.sb3"))


# 12. JS toLowerCase applies the Unicode Final_Sigma rule; a per-character
# lowercase leaves Greek capital sigma as medial sigma.
def final_sigma():
    p = Project()
    p.start()
    pairs = [("ΟΣ", "ος"), ("ΟΣ", "οσ"), ("ΟΣΑ", "οσα"), ("ΣΣ", "σς"),
             ("ΑΣ", "ας"), ("ΟΣ", "ος")]
    for a, b in pairs:
        eq = p._new("operator_equals", {"OPERAND1": p.txt(a), "OPERAND2": p.txt(b)})
        p.add_out([3, p.join(p.txt(""), [1, eq]), [10, "0"]])
    for hay, needle in [("ΟΣ", "Σ"), ("ΟΣ", "ς"), ("ΟΣΑ", "Σ")]:
        c = p._new("operator_contains", {"STRING1": p.txt(hay),
                                          "STRING2": p.txt(needle)})
        p.add_out([3, p.join(p.txt(""), [1, c]), [10, "0"]])
    p.write(os.path.join(OUT, "final_sigma.sb3"))



# 13. Unicode 17 (Node 26) added case pairs absent from the Unicode 16 table.
def unicode17_casefold():
    p = Project()
    p.start()
    pairs = [("\uA7CE", "\uA7CF"), ("\U00016EA0", "\U00016EBB")]
    for a, b in pairs:
        eq = p._new("operator_equals", {"OPERAND1": p.txt(a), "OPERAND2": p.txt(b)})
        p.add_out([3, p.join(p.txt(""), [1, eq]), [10, "0"]])
    p.write(os.path.join(OUT, "unicode17_casefold.sb3"))



# 13. data_listcontents: scratch-vm joins with spaces unless every item is a
# string of length 1. Numeric single digits are not strings, so [1,2,3]
# reports "1 2 3" (TurboWarp's compiler reports "123").
def list_contents():
    p = Project()
    p.start()
    p.lists = {
        "vl1": ["L1", [1, 2, 3]],
        "vl2": ["L2", ["a", 1]],
        "vl3": ["L3", ["a", "b"]],
        "vl4": ["L4", ["a", "bc"]],
        "vl5": ["L5", []],
    }
    cases = [("L1", "vl1", "1 2 3", "123"),
             ("L2", "vl2", "a 1", "a1"),
             ("L3", "vl3", "ab", "a b"),
             ("L4", "vl4", "a bc", "abc"),
             ("L5", "vl5", "", " ")]
    for name, lid, expected, wrong in cases:
        for want, label in ((expected, "match"), (wrong, "different")):
            lc = p._new("data_listcontents", {}, {"LIST": [name, lid]})
            j = p.join(p.txt(""), [1, lc])
            eq = p._new("operator_equals",
                        {"OPERAND1": [1, j], "OPERAND2": p.txt(want)})
            p.add_out([3, p.join(p.txt(""), [1, eq]), [10, "0"]])
    p.write(os.path.join(OUT, "list_contents.sb3"))



# 14. motion_pointtowards uses atan2; glibc differs from V8's fdlibm atan2
# in the last bit for some ratios.
def point_towards_ulp():
    p = Project()
    p.start()
    p.push("control_repeat", {"TIMES": p.num(1)})  # force HotSpot compile
    targets = [("T0", -45.079255250489865, 145.56588329463045),
               ("T1", -17.98930690750825, 116.55084054025008),
               ("T2", -57.472222890702966, 130.781674148923)]
    for name, x, y in targets:
        menu = p._new("motion_pointtowards_menu", {},
                      {"TOWARDS": [name, None]}, shadow=True)
        b = p.push("motion_pointtowards", {"TOWARDS": [1, menu]})
        p.blocks[menu]["parent"] = b
        p.add_out([3, p.join(p.txt(name + "="),
                             [1, p._new("motion_direction")]), [10, "0"]])
    d = p.build()
    sprite = d["targets"][1]
    for name, x, y in targets:
        t = json.loads(json.dumps(sprite))
        t["name"] = name
        t["x"] = x
        t["y"] = y
        t["blocks"] = {}
        d["targets"].append(t)
    with zipfile.ZipFile(os.path.join(OUT, "point_towards_ulp.sb3"), "w",
                         zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(d, ensure_ascii=False))
        z.writestr(SVG_MD5 + ".svg", SVG)



# 15. motion_move uses Math.cos/Math.sin; glibc differs from V8's fdlibm
# in the last bit for some directions.
def move_steps_ulp():
    p = Project()
    p.start()
    p.push("control_repeat", {"TIMES": p.num(1)})  # force HotSpot compile
    cases = [(42, 10), (677.56447505852, 1e-07), (-79.68115708811217, 10000000.0),
             (230, 1e-07), (219, 1e-07), (-386.03383183152323, 1),
             (-497.17408918189227, -10), (153, 1), (303, 1),
             (-421.5547732743649, 0.1)]
    for d, s in cases:
        p.push("motion_pointindirection", {"DIRECTION": p.num(repr(d))})
        p.push("motion_setx", {"X": p.num(0)})
        p.push("motion_sety", {"Y": p.num(0)})
        p.push("motion_movesteps", {"STEPS": p.num(repr(s))})
        p.add_out([3, p.join(p.txt("x="),
                             [1, p._new("motion_xposition")]), [10, "0"]])
        p.add_out([3, p.join(p.txt("y="),
                             [1, p._new("motion_yposition")]), [10, "0"]])
    p.write(os.path.join(OUT, "move_steps_ulp.sb3"))



# 16. sensing_timer is quantized to whole milliseconds: scratch-vm's
# runtime.currentMSecs is Date.now(), while std::chrono has sub-ms precision.
def timer_granularity():
    p = Project()
    p.start()
    p.push("control_repeat", {"TIMES": p.num(1)})  # force HotSpot compile
    t1 = p._new("sensing_timer")
    ms = p.binop("operator_multiply", [1, t1], p.num(1000))
    r = p._new("operator_round", {"NUM": [1, ms]})
    d = p.binop("operator_divide", [1, r], p.num(1000))
    t2 = p._new("sensing_timer")
    eq = p._new("operator_equals", {"OPERAND1": [1, t2], "OPERAND2": [1, d]})
    p.add_out([3, p.join(p.txt("eq="), [1, eq]), [10, "0"]])
    p.write(os.path.join(OUT, "timer_granularity.sb3"))



# 17. JIT variable cache keyed only by id: a block field whose id and name
# disagree resolves by id -> name in scratch-vm, but the JIT cached the first
# id -> Variable* and used it for a different name.
def var_id_name_mismatch():
    p = Project()
    p.start()
    p.push("control_repeat", {"TIMES": p.num(1)})  # force HotSpot compile
    for name, vid in [("stage", "l"), ("local", "g"),
                      ("stage", "g"), ("local", "l")]:
        p.push("data_setvariableto", {"VALUE": p.num(1)},
               {"VARIABLE": [name, vid]})
        for lab, vname, vlist in [("S", "stage", "vg"), ("L", "local", "vl")]:
            r = p._new("data_variable", {}, {"VARIABLE": [vname, vlist]})
            p.add_out([3, p.join(p.txt(lab + "="), [1, r]), [10, "0"]])
    d = p.build({"vg": ["stage", 10]})
    d["targets"][1]["variables"] = {"vl": ["local", 20]}
    d["targets"][1]["lists"] = {}
    with zipfile.ZipFile(os.path.join(OUT, "var_id_name_mismatch.sb3"), "w",
                         zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(d, ensure_ascii=False))
        z.writestr(SVG_MD5 + ".svg", SVG)


OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "repro")
os.makedirs(OUT, exist_ok=True)
if __name__ == "__main__":
    for fn in [nan_delete, nan_insert, big_numbers, unicode_casefold, round_half,
               costume_rounding, jit_mod, jit_trig, jit_invtrig, jit_negzero,
               libm_ulp, mislibm, final_sigma, unicode17_casefold, list_contents, point_towards_ulp, move_steps_ulp, timer_granularity, var_id_name_mismatch]:
        fn()
    print("wrote repros to", OUT)
