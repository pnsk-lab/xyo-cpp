#!/usr/bin/env python3
"""MLIR フロントエンドの意味論パリティ検査（interpreter vs MLIR JIT）。

同一の sb3 を「インタプリタ」と「MLIR フロントエンド（XYO_MLIR=1）」で
実行し、全変数・リストの最終状態が一致することを確かめる。MLIR 経路が
実際に使われたことは `[mlir] emit` トレースで確認する（ループを含む
ケースのみ。HotSpot では直線スクリプトは JIT されないため）。

    python3 tests/mlir/xcheck.py ./build/xyo [--mode eager|hotspot]
"""
import json, os, subprocess, sys, zipfile, tempfile

args = [a for a in sys.argv[1:] if not a.startswith("--")]
XYO = args[0] if args else "./build/xyo"
for a in sys.argv[1:]:
    if a.startswith("--mode="):
        os.environ["XCHECK_MODE"] = a.split("=", 1)[1]
TMP = os.environ.get("XCHECK_TMP") or tempfile.mkdtemp(prefix="mlirx-")
os.makedirs(TMP, exist_ok=True)


def B(op, next=None, parent=None, inputs=None, fields=None, top=False,
      shadow=False, mutation=None):
    b = {"opcode": op, "next": next, "parent": parent,
         "inputs": inputs or {}, "fields": fields or {},
         "topLevel": top, "shadow": shadow, "x": 0, "y": 0}
    if mutation:
        b["mutation"] = mutation
    return b


def proc_mutation(warp):
    """Scratch の procedures_prototype/call 用 mutation（引数 n を 1 個）。"""
    return {"tagName": "mutation", "children": [], "proccode": "spin %s",
            "argumentids": '["argN"]', "argumentnames": '["n"]',
            "argumentdefaults": '["0"]', "warp": "true" if warp else "false"}


class P:
    def __init__(self):
        self.stage_vars = False
        self.blocks = {}
        self.variables = {"vx": ["x", 0], "vy": ["y", 0], "vs": ["s", ""], "vz": ["z", 0],
                          "ve": ["e", 0]}
        self.lists = {"vL": ["L", []], "vM": ["M", []]}
        self.broadcasts = {}
        self.counter = 0

    def id(self, p="b"):
        self.counter += 1
        return f"{p}{self.counter}"

    def add(self, bid, op, nxt=None, parent=None, inputs=None, fields=None,
            top=False, shadow=False, mutation=None):
        self.blocks[bid] = B(op, nxt, parent, inputs, fields, top, shadow,
                             mutation)
        return bid

    def warp_proc(self, body_first, body_blocks, count, warp=True):
        """warp 手続き spin(n) を定義し、hat から count 回で呼ぶ。"""
        mut = proc_mutation(warp)
        self.add("hat", "event_whenflagclicked", "call", top=True)
        self.add("call", "procedures_call", "after", "hat",
                 {"argN": self.num(count)}, mutation=mut)
        self.listop("after", "data_addtolist", "L", None, "call",
                    {"ITEM": self.var("x")})
        self.add("def", "procedures_definition", body_first, None,
                 {"custom_block": [1, "proto"]}, top=True)
        self.add("proto", "procedures_prototype", None, "def",
                 {"argN": [1, "argsh"]}, shadow=True, mutation=mut)
        self.add("argsh", "argument_reporter_string_number", None, "proto",
                 {}, {"VALUE": ["n", None]}, shadow=True)
        for item in body_blocks:
            if len(item) != 6:
                raise SystemExit(f"bad body tuple ({len(item)}): {item}")
            bid, op, nxt, parent, inputs, fields = item
            self.add(bid, op, nxt, parent, inputs, fields)

    def num(self, v):
        i = self.id("n")
        self.add(i, "math_number", fields={"NUM": [v, None]}, shadow=True)
        return [1, i]

    def text(self, s):
        i = self.id("t")
        self.add(i, "text", fields={"TEXT": [s, None]}, shadow=True)
        return [1, i]

    def var(self, name):
        vid = {"x": "vx", "y": "vy", "s": "vs", "e": "ve", "z": "vz"}[name]
        i = self.id("v")
        self.add(i, "data_variable", fields={"VARIABLE": [name, vid]})
        return [3, i, [12, name, vid]]

    def setvar(self, bid, var, nxt=None, parent=None, value=None):
        vid = {"x": "vx", "y": "vy", "s": "vs", "e": "ve", "z": "vz"}[var]
        return self.add(bid, "data_setvariableto", nxt, parent,
                        {"VALUE": value}, {"VARIABLE": [var, vid]})

    def changevar(self, bid, var, nxt=None, parent=None, value=None):
        vid = {"x": "vx", "y": "vy", "s": "vs", "e": "ve", "z": "vz"}[var]
        return self.add(bid, "data_changevariableby", nxt, parent,
                        {"VALUE": value}, {"VARIABLE": [var, vid]})

    def listop(self, bid, op, lst, nxt=None, parent=None, inputs=None):
        lid = {"L": "vL", "M": "vM"}[lst]
        return self.add(bid, op, nxt, parent, inputs or {}, {"LIST": [lst, lid]})

    def project(self):
        stage = {"isStage": True, "name": "Stage", "variables": {},
                 "lists": {}, "broadcasts": self.broadcasts, "blocks": {},
                 "comments": {},
                 "costumes": [], "sounds": [], "currentCostume": 0,
                 "volume": 100, "layerOrder": 0}
        if self.stage_vars:
            stage["variables"] = self.variables
            stage["lists"] = self.lists
            svars, slists = {}, {}
        else:
            svars, slists = self.variables, self.lists
        sprite = {"isStage": False, "name": "S", "layerOrder": 1,
                  "variables": svars, "lists": slists,
                  "broadcasts": self.broadcasts, "blocks": self.blocks, "comments": {},
                  "costumes": [], "sounds": [], "currentCostume": 0,
                  "volume": 100, "visible": True, "x": 0, "y": 0, "size": 100,
                  "direction": 90, "draggable": False,
                  "rotationStyle": "all around"}
        return {"targets": [stage, sprite], "monitors": [], "extensions": [],
                "meta": {"semver": "3.0.0"}}


def run(p, name, env_extra, timeout=60):
    path = os.path.join(TMP, name + ".sb3")
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("project.json", json.dumps(p.project()))
    env = dict(os.environ)
    env.update({"XYO_DUMPVARS": "1",
                "XYO_JIT_MODE": os.environ.get("XCHECK_MODE", "eager")})
    env.update(env_extra)
    out = subprocess.run([XYO, "run", path], capture_output=True, text=True,
                         timeout=timeout, env=env)
    lines = [l for l in out.stdout.splitlines() if l.startswith("var ")]
    err = "\n".join(l for l in out.stderr.splitlines() if l.startswith("[mlir]"))
    return out.returncode, "\n".join(lines), err


def compare(name, build, require_mlir=True, timeout=60,
              require_fallback=False):
    p = P()
    build(p)
    rc1, a, _ = run(p, name, {"XYO_JIT_MODE": "off"}, timeout)
    rc2, b, trace = run(p, name, {"XYO_MLIR": "1", "XYO_MLIR_TRACE": "1"},
                        timeout)
    ok = rc1 == 0 and rc2 == 0 and a == b and a != ""
    if require_mlir and "emit" not in trace:
        ok = False
    # ガード版プロモーションを使うケースは guard=0（未発火）を拒否する。
    # float 昇格でも emit は出るため、これがないとガードの退行を見逃す。
    if name in GUARD_CASES and " guard=0" in trace:
        ok = False
    if name in INT_CASES and " int=0" in trace:
        ok = False
    if require_fallback and "fallback" not in trace:
        ok = False
    print(("PASS " if ok else "FAIL ") + name + ("" if require_mlir else " (fallback)"))
    if not ok:
        print("  interp:", a[:400], "rc", rc1)
        print("  mlir  :", b[:400], "rc", rc2)
        print("  trace :", trace[:800])
    return ok


# ---------------------------------------------------------------- ケース ----

def case_small(p):
    p.add("hat", "event_whenflagclicked", "a1", top=True)
    p.listop("a1", "data_addtolist", "L", "a2", "hat", {"ITEM": p.num(1)})
    p.listop("a2", "data_addtolist", "L", "a3", "a1", {"ITEM": p.num(2)})
    p.listop("a3", "data_addtolist", "L", None, "a2", {"ITEM": p.num(3)})
    p.setvar("a4", "x", None, "a3", p.num(7))


def case_repeat(p):
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "loop", "hat", p.num(0))
    p.add("loop", "control_repeat", "tag", "set",
          {"TIMES": p.num(1000), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "x", None, "loop", p.num(3))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def case_repeat_frac(p):
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "loop", "hat", p.num(0))
    p.add("loop", "control_repeat", "tag", "set",
          {"TIMES": p.num(2.6), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "x", None, "loop", p.num(1))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def case_until(p):
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "loop", "hat", p.num(0))
    p.add("loop", "control_repeat_until", "tag", "set",
          {"CONDITION": [2, "cmp"], "SUBSTACK": [2, "chg"]})
    p.add("cmp", "operator_gt", None, "loop",
          {"OPERAND1": p.var("x"), "OPERAND2": p.num(10)})
    p.changevar("chg", "x", None, "loop", p.num(1))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def case_while(p):
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "loop", "hat", p.num(0))
    p.add("loop", "control_while", "tag", "set",
          {"CONDITION": [2, "cmp"], "SUBSTACK": [2, "chg"]})
    p.add("cmp", "operator_lt", None, "loop",
          {"OPERAND1": p.var("x"), "OPERAND2": p.num(10)})
    p.changevar("chg", "x", None, "loop", p.num(2))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def case_ifelse(p):
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "loop", "hat", p.num(0))
    p.add("loop", "control_repeat", "tag", "set",
          {"TIMES": p.num(20), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "x", "iff", "loop", p.num(1))
    p.add("iff", "control_if_else", None, "chg",
          {"CONDITION": [2, "and1"], "SUBSTACK": [2, "inc"],
           "SUBSTACK2": [2, "dec"]})
    p.add("and1", "operator_and", None, "iff",
          {"OPERAND1": [2, "modc"], "OPERAND2": [2, "ltc"]})
    p.add("modc", "operator_equals", None, "and1",
          {"OPERAND1": [2, "mod"], "OPERAND2": p.num(0)})
    p.add("mod", "operator_mod", None, "modc",
          {"NUM1": p.var("x"), "NUM2": p.num(3)})
    p.add("ltc", "operator_lt", None, "and1",
          {"OPERAND1": p.var("x"), "OPERAND2": p.num(15)})
    p.changevar("inc", "y", None, "iff", p.num(100))
    p.changevar("dec", "y", None, "iff", p.num(-1))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("y")})


def case_nested(p):
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "outer", "hat", p.num(0))
    p.add("outer", "control_repeat", "tag", "set",
          {"TIMES": p.num(10), "SUBSTACK": [2, "inner"]})
    p.add("inner", "control_repeat", None, "outer",
          {"TIMES": p.num(10), "SUBSTACK": [2, "inc"]})
    p.changevar("inc", "x", None, "inner", p.num(1))
    p.listop("tag", "data_addtolist", "L", None, "outer", {"ITEM": p.var("x")})


def case_strings(p):
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "s", "s2", "hat", p.text("ab"))
    p.add("s2", "data_setvariableto", "s3", "s1",
          {"VALUE": [3, "j1", [10, "cd"]]}, {"VARIABLE": ["s", "vs"]})
    p.add("j1", "operator_join", None, "s2",
          {"STRING1": p.var("s"), "STRING2": p.text("cd")})
    p.add("s3", "data_setvariableto", "s4", "s2",
          {"VALUE": [3, "l1", [10, "z"]]}, {"VARIABLE": ["s", "vs"]})
    p.add("l1", "operator_letter_of", None, "s3",
          {"LETTER": p.num(2), "STRING": p.var("s")})
    p.listop("s4", "data_addtolist", "L", "s5", "s3", {"ITEM": p.var("s")})
    p.listop("s5", "data_addtolist", "L", "s6", "s4", {"ITEM": [2, "len"]})
    p.add("len", "operator_length", None, "s5", {"STRING": p.var("s")})
    p.listop("s6", "data_addtolist", "L", None, "s5", {"ITEM": [2, "cnt"]})
    p.add("cnt", "operator_contains", None, "s6",
          {"STRING1": p.var("s"), "STRING2": p.text("z")})


def case_lists(p):
    p.add("hat", "event_whenflagclicked", "a1", top=True)
    p.listop("a1", "data_deletealloflist", "L", "a2", "hat")
    p.listop("a2", "data_addtolist", "L", "a3", "a1", {"ITEM": p.num(10)})
    p.listop("a3", "data_addtolist", "L", "a4", "a2", {"ITEM": p.num(20)})
    p.listop("a4", "data_replaceitemoflist", "L", "a5", "a3",
             {"INDEX": p.num(1), "ITEM": p.num(30)})
    p.listop("a5", "data_insertatlist", "L", "a6", "a4",
             {"INDEX": p.num(1), "ITEM": p.num(40)})
    p.listop("a6", "data_deleteoflist", "L", "a7", "a5", {"INDEX": p.num(2)})
    p.add("a7", "data_setvariableto", "a8", "a6",
          {"VALUE": [3, "it1", [10, "0"]]}, {"VARIABLE": ["x", "vx"]})
    p.add("it1", "data_itemoflist", None, "a7", {"INDEX": p.num(2)},
          {"LIST": ["L", "vL"]})
    p.listop("a8", "data_addtolist", "M", "a9", "a7", {"ITEM": p.num(-7)})
    p.listop("a9", "data_addtolist", "M", "a10", "a8", {"ITEM": [2, "idx"]})
    p.add("idx", "data_itemnumoflist", None, "a9", {"ITEM": p.num(-7)},
          {"LIST": ["M", "vM"]})
    p.listop("a10", "data_addtolist", "M", None, "a9", {"ITEM": [2, "has"]})
    p.add("has", "data_listcontainsitem", None, "a10", {"ITEM": p.num(30)},
          {"LIST": ["L", "vL"]})


def case_math(p):
    p.add("hat", "event_whenflagclicked", "m1", top=True)
    p.add("m1", "data_setvariableto", "m2", "hat",
          {"VALUE": [3, "rnd", [4, "0"]]}, {"VARIABLE": ["x", "vx"]})
    p.add("rnd", "operator_round", None, "m1", {"NUM": [2, "sqrt"]})
    p.add("sqrt", "operator_mathop", None, "rnd", {"NUM": p.num(2)},
          {"OPERATOR": ["sqrt", None]})
    p.listop("m2", "data_addtolist", "L", "m3", "m1", {"ITEM": p.var("x")})
    p.listop("m3", "data_addtolist", "L", "m4", "m2", {"ITEM": [2, "div"]})
    p.add("div", "operator_divide", None, "m3",
          {"NUM1": p.num(1), "NUM2": p.num(0)})
    p.listop("m4", "data_addtolist", "L", None, "m3", {"ITEM": [2, "mod"]})
    p.add("mod", "operator_mod", None, "m4",
          {"NUM1": p.num(-7), "NUM2": p.num(3)})


def case_mod_rounding(p):
    # rem/m < 0 の判定は fdiv を残す: 微小な負の剰余 (-5e-324) を巨大な
    # 正の定数 (1e308) で割ると -0.0 に丸まるため、(rem < 0) への単純化は
    # 誤って除数を足してしまう。正しくは -5e-324 のまま。
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "loop", "hat", p.num(-5e-324))
    p.add("loop", "control_repeat", "tag", "set",
          {"TIMES": p.num(3), "SUBSTACK": [2, "m1"]})
    p.add("m1", "data_setvariableto", None, "loop", {"VALUE": [2, "mod"]},
          {"VARIABLE": ["x", "vx"]})
    p.add("mod", "operator_mod", None, "m1",
          {"NUM1": p.var("x"), "NUM2": p.num(1e308)})
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def _call_warp(p, blocks, count=1):
    """warp 手続き spin(n) の中に blocks を置いて count 回呼ぶ。"""
    norm = []
    for b in blocks:
        b = tuple(b)
        norm.append(b if len(b) == 6 else b + ({},))
    p.warp_proc(norm[0][0], norm, count)


def _addL(bid, nxt, parent, item):
    return (bid, "data_addtolist", nxt, parent, {"ITEM": item},
            {"LIST": ["L", "vL"]})


def case_const_literal_loop(p):
    # 定数ループのコンパイル時評価: 入口定数・純整数本体・定数回数なら
    # MLIR はループを畳んで定数代入にする。端数・0 回・負回数・文字列の
    # 非書き換えまで legacy と一致することを確認する（warp 手続き内）。
    _call_warp(p, [
        ("s1", "data_setvariableto", "r1", "def",
         {"VALUE": p.num(0)}, {"VARIABLE": ["x", "vx"]}),
        ("r1", "control_repeat", "r1t", "s1",
         {"TIMES": p.num(5), "SUBSTACK": [2, "r1b"]}),
        ("r1b", "data_changevariableby", None, "r1",
         {"VALUE": p.num(3)}, {"VARIABLE": ["x", "vx"]}),
        _addL("r1t", "r2a", "r1", p.var("x")),
        ("r2a", "data_setvariableto", "r2", "r1t",
         {"VALUE": p.num(0)}, {"VARIABLE": ["y", "vy"]}),
        ("r2", "control_repeat", "r2t", "r2a",
         {"TIMES": p.num(2.6), "SUBSTACK": [2, "r2b"]}),
        ("r2b", "data_changevariableby", None, "r2",
         {"VALUE": p.num(7)}, {"VARIABLE": ["y", "vy"]}),
        _addL("r2t", "r3a", "r2", p.var("y")),
        ("r3a", "data_setvariableto", "r3", "r2t",
         {"VALUE": p.num(42)}, {"VARIABLE": ["x", "vx"]}),
        ("r3", "control_repeat", "r3t", "r3a",
         {"TIMES": p.num(0), "SUBSTACK": [2, "r3b"]}),
        ("r3b", "data_changevariableby", None, "r3",
         {"VALUE": p.num(1)}, {"VARIABLE": ["x", "vx"]}),
        _addL("r3t", "r4a", "r3", p.var("x")),
        ("r4a", "data_setvariableto", "r4", "r3t",
         {"VALUE": p.text("x9")}, {"VARIABLE": ["s", "vs"]}),
        ("r4", "control_repeat", "r4t", "r4a",
         {"TIMES": p.num(-3), "SUBSTACK": [2, "r4b"]}),
        ("r4b", "data_changevariableby", None, "r4",
         {"VALUE": p.num(1)}, {"VARIABLE": ["s", "vs"]}),
        _addL("r4t", None, "r4", p.var("s")),
    ])


def case_const_literal_while(p):
    # 定数条件の while も畳めること（カウンタで 20 回）。
    _call_warp(p, [
        ("s", "data_setvariableto", "w", "def",
         {"VALUE": p.num(0)}, {"VARIABLE": ["x", "vx"]}),
        ("w", "control_while", "wt", "s",
         {"CONDITION": [2, "cond"], "SUBSTACK": [2, "body"]}),
        ("cond", "operator_lt", None, "w",
         {"OPERAND1": p.var("x"), "OPERAND2": p.num(20)}),
        ("body", "data_changevariableby", None, "w",
         {"VALUE": p.num(1)}, {"VARIABLE": ["x", "vx"]}),
        _addL("wt", None, "w", p.var("x")),
    ])


def case_const_literal_nested(p):
    # 入れ子の定数ループ（内側 4 回 × 外側 3 回）。
    _call_warp(p, [
        ("s", "data_setvariableto", "o", "def",
         {"VALUE": p.num(0)}, {"VARIABLE": ["x", "vx"]}),
        ("o", "control_repeat", "ot", "s",
         {"TIMES": p.num(3), "SUBSTACK": [2, "i1"]}),
        ("i1", "control_repeat", None, "o",
         {"TIMES": p.num(4), "SUBSTACK": [2, "b"]}),
        ("b", "data_changevariableby", None, "i1",
         {"VALUE": p.num(1)}, {"VARIABLE": ["x", "vx"]}),
        _addL("ot", None, "o", p.var("x")),
    ])


def case_broadcastwait(p):
    # broadcast and wait は MLIR が begin → step が 0 になるまで yield の
    # プロトコルで native に lower する（compat 直呼びはハングした）。
    # 受信側の完了待ちと結果一致を検証する。
    p.add("hat", "event_whenflagclicked", "call", top=True)
    p.add("call", "event_broadcastandwait", "tag0", "hat",
          {"BROADCAST_INPUT": [1, "bc"]}, {"BROADCAST_OPTION": ["go", "b1"]})
    p.add("bc", "event_broadcast_menu", None, "call",
          {}, {"BROADCAST_OPTION": ["go", "b1"]}, shadow=True)
    p.listop("tag0", "data_addtolist", "L", None, "call", {"ITEM": p.num(9)})
    p.add("recv", "event_whenbroadcastreceived", "tag", None,
          {}, {"BROADCAST_OPTION": ["go", "b1"]}, top=True)
    p.listop("tag", "data_addtolist", "L", None, "recv", {"ITEM": p.num(2)})
    p.broadcasts = {"b1": "go"}


def case_mod_rounding_bits(p):    # ループ外の bits 域 xyo.mod: 除数は正の定数だが rem/m の fdiv を残す。
    # 微小な負の剰余では rem/m が -0.0 に丸まるため (rem < 0) は誤り。
    p.add("hat", "event_whenflagclicked", "set", top=True)
    p.setvar("set", "x", "m", "hat", p.num(-5e-324))
    p.add("m", "data_setvariableto", "tag", "set", {"VALUE": [2, "mod"]},
          {"VARIABLE": ["y", "vy"]})
    p.add("mod", "operator_mod", None, "m",
          {"NUM1": p.var("x"), "NUM2": p.num(1e308)})
    p.listop("tag", "data_addtolist", "L", None, "m", {"ITEM": p.var("y")})


def case_int_replace_var_item(p):
    # 整数昇格ループ内の replace（ITEM が浮動小数・昇格変数）。
    # ITEM の getvar は本体から除去されるため、bits 影へ差し替えないと
    # dangling use になり verifier 落ちでスクリプト全体が fallback する。
    p.add("hat", "event_whenflagclicked", "a1", top=True)
    p.listop("a1", "data_addtolist", "L", "a2", "hat", {"ITEM": p.num(5)})
    p.listop("a2", "data_addtolist", "L", "a3", "a1", {"ITEM": p.num(1)})
    p.listop("a3", "data_addtolist", "L", "s1", "a2", {"ITEM": p.num(4)})
    p.setvar("s1", "x", "s2", "a3", p.num(0))
    p.setvar("s2", "s", "loop", "s1", p.num(0))
    p.add("loop", "control_repeat", "tag", "s2",
          {"TIMES": p.num(2), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "x", "rd", "loop", p.num(1))
    p.add("rd", "data_setvariableto", "rep", "chg", {"VALUE": [2, "it"]},
          {"VARIABLE": ["s", "vs"]})
    p.add("it", "data_itemoflist", None, "rd", {"INDEX": p.var("x")},
          {"LIST": ["L", "vL"]})
    p.add("rep", "data_replaceitemoflist", None, "rd",
          {"INDEX": p.var("x"), "ITEM": p.var("s")}, {"LIST": ["L", "vL"]})
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": p.var("s")})


def case_guarded_entry(p):
    # entry 非定数（変数経由）の整数ループは入口ガード付き複製で昇格する。
    # y = x（x=4 だが定数伝播しない）→ repeat 5 { y += 2 }。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(4))
    p.setvar("s2", "y", "loop", "s1", p.var("x"))
    p.add("loop", "control_repeat", "tag", "s2",
          {"TIMES": p.num(5), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(2))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": p.var("y")})


def case_guarded_nonneg(p):
    # 下側発散で full ボックスが収束しない形（y += y、entry 非定数）。
    # 非負ガード [0, 2^20] で昇格する。y=3 → 6 → 12 → 24 → 48 で脱出。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(3))
    p.setvar("s2", "y", "loop", "s1", p.var("x"))
    p.add("loop", "control_repeat_until", "tag", "s2",
          {"CONDITION": [2, "c"], "SUBSTACK": [2, "chg"]})
    p.add("c", "operator_gt", None, "loop",
          {"OPERAND1": p.var("y"), "OPERAND2": p.num(30)})
    p.changevar("chg", "y", "add", "loop", p.var("y"))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": p.var("y")})


def case_box_repeat(p):
    # 動的 trip の repeat は通常の整数化が失敗する（区間発散＋非定数 entry）。
    # ボックス版（入口＋trip ガード＋inrange＋float 継続）で昇格する。
    # y = x（x=4 だが定数伝播しない）→ repeat s { y += 2 }（s = x+1 = 5）。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(4))
    p.setvar("s2", "s", "s3", "s1", [2, "nplus"])
    p.add("nplus", "operator_add", None, "s2",
          {"NUM1": p.var("x"), "NUM2": p.num(1)})
    p.setvar("s3", "y", "loop", "s2", p.var("x"))
    p.add("loop", "control_repeat", "tag", "s3",
          {"TIMES": p.var("s"), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(2))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": p.var("y")})


def case_box_repeat_const(p):
    # 定数 trip＋定数 entry でも、プロジェクト全体に非整数書き込みが
    # あると通常版は count 不一致で失敗する。ボックス版（relaxClean）で救う。
    # y = 0 → repeat 5 { y += 1 }（=5）。その後に y = "dirty"（数えない書き込み）。
    # ループ実行時は y は整数なので int 版が走り、最終 y は "dirty"。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "y", "loop", "hat", p.num(0))
    p.add("loop", "control_repeat", "tag", "s1",
          {"TIMES": p.num(5), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(1))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", "dirty", "loop", {"ITEM": p.var("y")})
    p.setvar("dirty", "y", None, "tag", p.text("dirty"))


def case_box_repeat_dynpois(p):
    # 動的 trip＋非定数 entry＋count 不一致（sort 内側の形）。
    # y = x → repeat s { y += 2 }（s 動的）に加え、後続 loop2 で y += 10。
    # 全て整数書き込みだが count 不一致で通常版は失敗、box 版が救う。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(4))
    p.setvar("s2", "s", "s3", "s1", [2, "nplus"])
    p.add("nplus", "operator_add", None, "s2",
          {"NUM1": p.var("x"), "NUM2": p.num(1)})
    p.setvar("s3", "y", "loop", "s2", p.var("x"))
    p.add("loop", "control_repeat", "tag", "s3",
          {"TIMES": p.var("s"), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(2))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", "loop2", "loop", {"ITEM": p.var("y")})
    p.add("loop2", "control_repeat", "done", "tag",
          {"TIMES": p.num(3), "SUBSTACK": [2, "chg2"]})
    p.changevar("chg2", "y", "add2", "loop2", p.num(10))
    p.listop("add2", "data_addtolist", "M", None, "chg2", {"ITEM": p.var("y")})
    p.listop("done", "data_addtolist", "M", None, "loop2", {"ITEM": p.var("y")})


def case_box_zero_trip(p):
    # trip=0 でループ本体が実行されない。元の値がそのまま残る。
    # y = x（x=7）→ repeat 0 { y += 1 } → y は 7 のまま。
    # 後書きの非整数で count 不一致→通常版失敗→box 版が救う（guard=1）。
    # 0-trip select で元の値を正しく書き戻すことを検証。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(7))
    p.setvar("s2", "y", "loop", "s1", p.var("x"))
    p.add("loop", "control_repeat", "tag", "s2",
          {"TIMES": p.num(0), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(1))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", "dirty", "loop", {"ITEM": p.var("y")})
    p.setvar("dirty", "y", None, "tag", p.text("dirty"))

def case_box_neg_trip(p):
    # trip=-3 でループ本体が実行されない（0-trip と同様に扱う）。
    # y = x（x=7）→ repeat -3 { y += 1 } → y は 7 のまま。
    # 後書きの非整数で count 不一致→通常版失敗→box 版が救う（guard=1）。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(7))
    p.setvar("s2", "y", "loop", "s1", p.var("x"))
    p.add("loop", "control_repeat", "tag", "s2",
          {"TIMES": p.num(-3), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(1))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", "dirty2", "loop", {"ITEM": p.var("y")})
    p.setvar("dirty2", "y", None, "tag", p.text("dirty"))



def case_box_big_trip(p):
    # trip が box 上限（2^20）を超える。入口 trip ガードで弾かれ、
    # float フォールバックで正しく実行される（guard=1 で box を試みるが
    # ガード失敗で float にフォールバックすることを検証）。
    # y = x（x=7）→ repeat 5000000 { y += 1 } → y は 5000007（実行は重いが
    # 正しさのみ検証、性能は問わない）。
    # 注意: 実際に 500 万回回すと遅いため、テストでは trip=2000000（>1048576）
    # を使い、y の最終値を検証する（y = 7 + 2000000）。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(7))
    p.setvar("s2", "y", "loop", "s1", p.var("x"))
    p.add("loop", "control_repeat", "tag", "s2",
          {"TIMES": p.num(2000000), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(1))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", "dirty", "loop", {"ITEM": p.var("y")})
    p.setvar("dirty", "y", None, "tag", p.text("dirty"))



def case_box_bail(p):
    # 入口は box 内だが本体ですぐ範囲外になる（bail→float 再実行）。
    # y = x（x=1048576=2^20、定数伝播しない）から repeat 5 { y += 1 }。
    # int_exact_ok は通るが inrange で即 bail し、float で正しく
    # 1048581 になる。guard=1（box 版が発火）を確認する。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(1048576))
    p.setvar("s2", "y", "loop", "s1", p.var("x"))
    p.add("loop", "control_repeat", "tag", "s2",
          {"TIMES": p.num(5), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "y", "add", "loop", p.num(1))
    p.listop("add", "data_addtolist", "M", None, "chg", {"ITEM": p.var("y")})
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": p.var("y")})


def case_list_item_int(p):
    # 整数リテラルのみを含むリストの要素読みは整数。acc += L[1]*L[2] が
    # int 昇格する（範囲外読みは ""→0 になるので区間に 0 を含める）。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(0))
    p.setvar("s2", "y", "a1", "s1", p.num(0))
    p.listop("a1", "data_addtolist", "L", "a2", "s2", {"ITEM": p.num(3)})
    p.listop("a2", "data_addtolist", "L", "loop", "a1", {"ITEM": p.num(4)})
    p.add("loop", "control_repeat", "tag", "a2",
          {"TIMES": p.num(5), "SUBSTACK": [2, "chg"]})
    p.add("chg", "data_changevariableby", "inc", "loop",
          {"VALUE": [2, "mul"]}, {"VARIABLE": ["y", "vy"]})
    p.add("mul", "operator_multiply", None, "chg",
          {"NUM1": [2, "i1"], "NUM2": [2, "i2"]})
    p.add("i1", "data_itemoflist", None, "mul", {"INDEX": p.num(1)},
          {"LIST": ["L", "vL"]})
    p.add("i2", "data_itemoflist", None, "mul", {"INDEX": p.num(2)},
          {"LIST": ["L", "vL"]})
    p.changevar("inc", "x", None, "chg", p.num(1))
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": p.var("y")})


def case_negzero_mod(p):
    # -0.0 は入口ガードが拒否する（i32 輸送で +0 になり 1/x の符号が
    # 変わるため）。float 経路の 2 の冪 mod（x - c*trunc(x/c)）も -0 を
    # 保つ。1/(-0) == -Infinity。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "m", "hat", p.num(0))
    p.add("m", "data_setvariableto", "loop", "s1",
          {"VALUE": [2, "mul"]}, {"VARIABLE": ["x", "vx"]})
    p.add("mul", "operator_multiply", None, "m",
          {"NUM1": p.num(0), "NUM2": p.num(-1)})
    p.add("loop", "control_repeat", "tag", "m",
          {"TIMES": p.num(1), "SUBSTACK": [2, "rm"]})
    p.add("rm", "data_setvariableto", None, "loop", {"VALUE": [2, "mod"]},
          {"VARIABLE": ["x", "vx"]})
    p.add("mod", "operator_mod", None, "rm",
          {"NUM1": p.var("x"), "NUM2": p.num(1)})
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": [2, "dv"]})
    p.add("dv", "operator_divide", None, "tag",
          {"NUM1": p.num(1), "NUM2": p.var("x")})


def case_stringvar(p):
    # 文字列 "5" を保持する変数への set があるループは昇格してはいけない。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "loop", "hat", p.text("5"))
    p.add("loop", "control_repeat", "tag", "s1",
          {"TIMES": p.num(3), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "x", None, "loop", p.num(1))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def case_compare_strings(p):
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "s", "c1", "hat", p.text("5abc"))
    p.add("c1", "data_addtolist", "c2", "s1", {"ITEM": [2, "eq"]},
          {"LIST": ["L", "vL"]})
    p.add("eq", "operator_equals", None, "c1",
          {"OPERAND1": p.var("s"), "OPERAND2": p.text("5abc")})
    p.listop("c2", "data_addtolist", "L", None, "c1", {"ITEM": [2, "lt"]})
    p.add("lt", "operator_lt", None, "c2",
          {"OPERAND1": p.text("abc"), "OPERAND2": p.var("s")})


def case_interleave(p):
    # ハット A は毎反復 yield する MLIR ループ、ハット B は wait until（既存 JIT）。
    p.add("hatA", "event_whenflagclicked", "setA", top=True)
    p.setvar("setA", "x", "loopA", "hatA", p.num(0))
    p.add("loopA", "control_repeat", None, "setA",
          {"TIMES": p.num(300), "SUBSTACK": [2, "chgA"]})
    p.changevar("chgA", "x", None, "loopA", p.num(1))
    p.add("hatB", "event_whenflagclicked", "waitB", top=True)
    p.add("waitB", "control_wait_until", "tagB", "hatB",
          {"CONDITION": [2, "cmpB"]})
    p.add("cmpB", "operator_gt", None, "waitB",
          {"OPERAND1": p.var("x"), "OPERAND2": p.num(100)})
    p.listop("tagB", "data_addtolist", "L", None, "waitB", {"ITEM": p.var("x")})


def case_string_grow(p):
    p.add("hat", "event_whenflagclicked", "s0", top=True)
    p.setvar("s0", "s", "loop", "hat", p.text(""))
    p.add("loop", "control_repeat", "tag", "s0",
          {"TIMES": p.num(2000), "SUBSTACK": [2, "cat"]})
    p.add("cat", "data_setvariableto", None, "loop",
          {"VALUE": [3, "join", [10, "0"]]}, {"VARIABLE": ["s", "vs"]})
    p.add("join", "operator_join", None, "cat",
          {"STRING1": p.var("s"), "STRING2": p.text("ab")})
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": [2, "len"]})
    p.add("len", "operator_length", None, "tag", {"STRING": p.var("s")})


def case_two_hats(p):
    p.add("hatA", "event_whenflagclicked", "setA", top=True)
    p.setvar("setA", "x", "loopA", "hatA", p.num(0))
    p.add("loopA", "control_repeat", None, "setA",
          {"TIMES": p.num(500), "SUBSTACK": [2, "chgA"]})
    p.changevar("chgA", "x", None, "loopA", p.num(2))
    p.add("hatB", "event_whenflagclicked", "setB", top=True)
    p.setvar("setB", "y", "loopB", "hatB", p.num(0))
    p.add("loopB", "control_repeat", "tagB", "setB",
          {"TIMES": p.num(500), "SUBSTACK": [2, "chgB"]})
    p.changevar("chgB", "y", None, "loopB", p.num(3))
    p.listop("tagB", "data_addtolist", "L", "tag2", "loopB", {"ITEM": p.var("y")})
    p.listop("tag2", "data_addtolist", "L", None, "tagB", {"ITEM": p.var("x")})


def case_zero_repeat(p):
    # 0 回反復では変数の実体（文字列のまま）を書き換えてはいけない。
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "loop", "hat", p.text("5abc"))
    p.add("loop", "control_repeat", "c1", "s1",
          {"TIMES": p.num(0), "SUBSTACK": [2, "chg"]})
    p.changevar("chg", "x", None, "loop", p.num(1))
    p.add("c1", "data_addtolist", "c2", "loop", {"ITEM": [2, "eq"]},
          {"LIST": ["L", "vL"]})
    p.add("eq", "operator_equals", None, "c1",
          {"OPERAND1": p.var("x"), "OPERAND2": p.text("5abc")})
    p.listop("c2", "data_addtolist", "L", "c3", "c1", {"ITEM": p.var("x")})
    p.add("c3", "data_addtolist", None, "c2", {"ITEM": [2, "cnt"]},
          {"LIST": ["L", "vL"]})
    p.add("cnt", "operator_divide", None, "c3",
          {"NUM1": p.num(0), "NUM2": p.num(0)})


def case_warp_counter(p):
    # warp 手続き内の数値ループ（64 反復まとめて進める chunked 経路）。
    n = 200000
    p.warp_proc(
        "setx",
        [("setx", "data_setvariableto", "loop", "def",
          {"VALUE": p.num(0)}, {"VARIABLE": ["x", "vx"]}),
         ("loop", "control_repeat", None, "setx",
          {"TIMES": [3, "argr", p.num(0)], "SUBSTACK": [2, "chg"]}, {}),
         ("argr", "argument_reporter_string_number", None, "loop", {},
          {"VALUE": ["n", None]}),
         ("chg", "data_changevariableby", "chg2", "loop",
          {"VALUE": p.num(1)}, {"VARIABLE": ["x", "vx"]}),
         ("chg2", "data_changevariableby", None, "chg",
          {"VALUE": p.num(2)}, {"VARIABLE": ["y", "vy"]})],
        n)
    p.listop("after2", "data_addtolist", "L", None, "after",
             {"ITEM": p.var("y")})


def case_warp_zero(p):
    # warp 手続きでも 0 回反復なら文字列を数値に変えない。
    mut = proc_mutation(True)
    p.add("hat", "event_whenflagclicked", "sets", top=True)
    p.setvar("sets", "s", "call", "hat", p.text("abc"))
    p.add("call", "procedures_call", "after", "sets",
          {"argN": p.num(0)}, mutation=mut)
    p.listop("after", "data_addtolist", "L", None, "call", {"ITEM": p.var("s")})
    p.add("def", "procedures_definition", "loop", None,
          {"custom_block": [1, "proto"]}, top=True)
    p.add("proto", "procedures_prototype", None, "def",
          {"argN": [1, "argsh"]}, shadow=True, mutation=mut)
    p.add("argsh", "argument_reporter_string_number", None, "proto", {},
          {"VALUE": ["n", None]}, shadow=True)
    p.add("loop", "control_repeat", "tag", "def",
          {"TIMES": [3, "argr", p.num(0)], "SUBSTACK": [2, "chg"]}, {})
    p.add("argr", "argument_reporter_string_number", None, "loop", {},
          {"VALUE": ["n", None]})
    p.changevar("chg", "s", None, "loop", p.num(1))
    p.listop("tag", "data_addtolist", "M", None, "loop", {"ITEM": p.var("s")})


def case_warp_nested(p):
    # warp 手続き内の入れ子ループ（内側が chunked / 外側が通常）。
    p.warp_proc(
        "setx",
        [("setx", "data_setvariableto", "outer", "def",
          {"VALUE": p.num(0)}, {"VARIABLE": ["x", "vx"]}),
         ("outer", "control_repeat", None, "setx",
          {"TIMES": p.num(10), "SUBSTACK": [2, "inner"]}, {}),
         ("inner", "control_repeat", "chg5", "outer",
          {"TIMES": p.num(3), "SUBSTACK": [2, "chg1"]}, {}),
         ("chg1", "data_changevariableby", None, "inner",
          {"VALUE": p.num(1)}, {"VARIABLE": ["x", "vx"]}),
         ("chg5", "data_changevariableby", None, "outer",
          {"VALUE": p.num(0.5)}, {"VARIABLE": ["x", "vx"]})],
        0)


def case_warp_math_chunk(p):
    # warp ループの 64 反復チャンク展開で、各反復の計算がカウンタに依存
    # する形。to_num の SSA キャッシュをコピー間で共有すると、チャンク内
    # の mathop 入力が先頭反復の値に固定されて合計が変わる。
    # trip は定数にして、境界ガードの if を含まない平坦な整数本体にする
    # （動的 trip だと bail フラグの if が入りチャンク対象外になる）。
    mut = proc_mut("spin", [], [], warp=True)
    p.add("hat", "event_whenflagclicked", "call", top=True)
    p.add("call", "procedures_call", "after", "hat", {}, mutation=mut)
    p.listop("after", "data_addtolist", "L", None, "call",
             {"ITEM": p.var("e")})
    p.add("def", "procedures_definition", "sete", None,
          {"custom_block": [1, "proto"]}, top=True)
    p.add("proto", "procedures_prototype", None, "def", {}, shadow=True,
          mutation=mut)
    p.add("sete", "data_setvariableto", "setx", "def",
          {"VALUE": p.num(0)}, {"VARIABLE": ["e", "ve"]})
    p.add("setx", "data_setvariableto", "loop", "def",
          {"VALUE": p.num(0)}, {"VARIABLE": ["x", "vx"]})
    p.add("loop", "control_repeat", None, "setx",
          {"TIMES": p.num(300), "SUBSTACK": [2, "chg"]}, {})
    p.add("chg", "data_changevariableby", "add", "loop",
          {"VALUE": p.num(1)}, {"VARIABLE": ["x", "vx"]})
    p.add("add", "data_changevariableby", None, "chg",
          {"VALUE": [3, "sqrt", [10, ""]]}, {"VARIABLE": ["e", "ve"]})
    p.add("sqrt", "operator_mathop", None, "add",
          {"NUM": [3, "vxr", [10, ""]]}, {"OPERATOR": ["sqrt", None]})
    p.add("vxr", "data_variable", None, "sqrt", {},
          {"VARIABLE": ["x", "vx"]})


def proc_mut(proccode, argnames, argids, warp=False, ret=False):
    return {"tagName": "mutation", "children": [], "proccode": proccode,
            ("return" if ret else "x-ret"): "true",
            "argumentids": json.dumps(argids),
            "argumentnames": json.dumps(argnames),
            "argumentdefaults": json.dumps(["0"] * len(argnames)),
            "warp": "true" if warp else "false"}


def case_proc_param_loop(p):
    """手続き引数（文字列）をループ条件・letter of で使う。"""
    mut = proc_mut("parse %s", ["src"], ["argSrc"])
    p.add("hat", "event_whenflagclicked", "call", top=True)
    p.add("call", "procedures_call", "tag", "hat",
          {"argSrc": p.text("ab xcd")}, mutation=mut)
    p.listop("tag", "data_addtolist", "L", None, "call", {"ITEM": p.var("x")})
    # 定義: i=1; repeat until (i > length of src) { ch = letter i of src;
    #                                       if ch = " " なら n を +1; i を +1 }
    p.add("def", "procedures_definition", "seti", None,
          {"custom_block": [1, "proto"]}, top=True)
    p.add("proto", "procedures_prototype", None, "def",
          {"argSrc": [1, "argsh"]}, shadow=True, mutation=mut)
    p.add("argsh", "argument_reporter_string_number", None, "proto",
          {}, {"VALUE": ["src", None]}, shadow=True)
    p.setvar("seti", "x", None, "def", p.num(0))
    p.add("seti2", "data_setvariableto", "loop", "seti", {}, {"VARIABLE": ["y", "vy"]})
    p.blocks["seti2"]["inputs"] = {"VALUE": p.num(1)}
    p.add("loop", "control_repeat_until", "done", "seti2",
          {"CONDITION": [2, "cond"], "SUBSTACK": [2, "chset"]})
    p.add("cond", "operator_gt", None, "loop",
          {"OPERAND1": p.var("y"), "OPERAND2": [2, "len"]})
    p.add("len", "operator_length", None, "cond",
          {"STRING": [3, "arg2", [12, "src", None]]})
    p.add("arg2", "argument_reporter_string_number", None, "len",
          {}, {"VALUE": ["src", None]})
    p.add("chset", "data_setvariableto", "isspace", "loop", {},
          {"VARIABLE": ["s", "vs"]})
    p.blocks["chset"]["inputs"] = {"VALUE": [3, "ch", [10, " "]]}
    p.add("ch", "operator_letter_of", None, "chset",
          {"LETTER": [2, "var1"], "STRING": [3, "arg3", [12, "src", None]]})
    p.add("arg3", "argument_reporter_string_number", None, "ch",
          {}, {"VALUE": ["src", None]})
    # y を直接読む別ブロック（var は y/vx の対応なので data_variable を直書き）
    p.add("var1", "data_variable", None, "ch", {}, {"VARIABLE": ["y", "vy"]})
    p.add("isspace", "control_if", None, "chset",
          {"CONDITION": [2, "eqsp"], "SUBSTACK": [2, "incx"]})
    p.add("eqsp", "operator_equals", None, "isspace",
          {"OPERAND1": [3, "chv", [10, " "]], "OPERAND2": p.text(" ")})
    p.add("chv", "data_variable", None, "eqsp", {}, {"VARIABLE": ["s", "vs"]})
    p.changevar("incx", "x", None, "isspace", p.num(1))
    p.changevar("incy", "y", None, "isspace", p.num(1))


def case_proc_recursion(p):
    """再帰手続き（引数の受け渡しと復帰値の保持）。"""
    mut = proc_mut("sumdown %s", ["n"], ["argN"])
    p.add("hat", "event_whenflagclicked", "set0", top=True)
    p.setvar("set0", "x", "call", "hat", p.num(0))
    p.add("call", "procedures_call", "tag", "set0", {"argN": p.num(5)},
          mutation=mut)
    p.listop("tag", "data_addtolist", "L", None, "call", {"ITEM": p.var("x")})
    p.add("def", "procedures_definition", "iff", None,
          {"custom_block": [1, "proto"]}, top=True)
    p.add("proto", "procedures_prototype", None, "def",
          {"argN": [1, "argsh"]}, shadow=True, mutation=mut)
    p.add("argsh", "argument_reporter_string_number", None, "proto",
          {}, {"VALUE": ["n", None]}, shadow=True)
    p.add("iff", "control_if", None, "def",
          {"CONDITION": [2, "cmp"], "SUBSTACK": [2, "add"]})
    p.add("cmp", "operator_gt", None, "iff",
          {"OPERAND1": [3, "argc", [12, "n", None]], "OPERAND2": p.num(0)})
    p.add("argc", "argument_reporter_string_number", None, "cmp",
          {}, {"VALUE": ["n", None]})
    p.add("add", "data_changevariableby", "rec", "iff", {}, {"VARIABLE": ["x", "vx"]})
    p.blocks["add"]["inputs"] = {"VALUE": [3, "argv", [12, "n", None]]}
    p.add("argv", "argument_reporter_string_number", None, "add",
          {}, {"VALUE": ["n", None]})
    p.add("rec", "procedures_call", None, "add", {"argN": [2, "dec"]},
          mutation=mut)
    p.add("dec", "operator_subtract", None, "rec",
          {"NUM1": [3, "argd", [12, "n", None]], "NUM2": p.num(1)})
    p.add("argd", "argument_reporter_string_number", None, "dec",
          {}, {"VALUE": ["n", None]})


def case_promoted_parse(p):
    """repeat until の条件が or(昇格変数比較) で、本体が if の中で
    昇格変数を書く形（calculator のパーサループの縮小版）。"""
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(1))       # i = 1
    p.setvar("s2", "y", "s3", "s1", p.text("ab cd"))
    p.setvar("s3", "s", "loop", "s2", p.num(0))      # err = 0
    p.add("loop", "control_repeat_until", "tag", "s3",
          {"CONDITION": [2, "cond"], "SUBSTACK": [2, "chset"]})
    p.add("cond", "operator_or", None, "loop",
          {"OPERAND1": [2, "gt"], "OPERAND2": [2, "err1"]})
    p.add("gt", "operator_gt", None, "cond",
          {"OPERAND1": p.var("x"), "OPERAND2": p.num(6)})
    p.add("err1", "operator_equals", None, "cond",
          {"OPERAND1": p.var("s"), "OPERAND2": p.num(1)})
    # ch = letter i of "ab cd"
    p.add("chset", "data_setvariableto", "iff", "loop", {}, {"VARIABLE": ["s", "vs"]})
    p.blocks["chset"]["inputs"] = {"VALUE": [3, "ch", [10, ""]]}
    p.add("ch", "operator_letter_of", None, "chset",
          {"LETTER": p.var("x"), "STRING": p.text("abcde")})
    p.add("iff", "control_if_else", None, "chset",
          {"CONDITION": [2, "eqx"], "SUBSTACK": [2, "inc"], "SUBSTACK2": [2, "iff2"]})
    p.add("eqx", "operator_equals", None, "iff",
          {"OPERAND1": [3, "chv", [10, ""]], "OPERAND2": p.text("c")})
    p.add("chv", "data_variable", None, "eqx", {}, {"VARIABLE": ["s", "vs"]})
    p.changevar("inc", "x", None, "iff", p.num(1))
    p.add("iff2", "control_if_else", None, "iff",
          {"CONDITION": [2, "eqy"], "SUBSTACK": [2, "sete"],
           "SUBSTACK2": [2, "inc2"]})
    p.add("eqy", "operator_equals", None, "iff2",
          {"OPERAND1": [3, "chv2", [10, ""]], "OPERAND2": p.text("d")})
    p.add("chv2", "data_variable", None, "eqy", {}, {"VARIABLE": ["s", "vs"]})
    p.setvar("sete", "s", None, "iff2", p.num(1))
    p.changevar("inc2", "x", None, "iff2", p.num(1))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def case_parse_var_bound(p):
    """calculator の字句解析ループの縮小版: x=i, y=n, s=ch, e=err。
    条件は or(gt(i,n), eq(err,1))、本体は if/else if/else で i か err を進める。"""
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(1))            # i = 1
    p.setvar("s2", "y", "s3", "s1", p.num(5))             # n = 5
    p.setvar("s3", "e", "loop", "s2", p.num(0))           # err = 0
    p.add("loop", "control_repeat_until", "tag", "s3",
          {"CONDITION": [2, "cond"], "SUBSTACK": [2, "chset"]})
    p.add("cond", "operator_or", None, "loop",
          {"OPERAND1": [2, "gt"], "OPERAND2": [2, "err1"]})
    p.add("gt", "operator_gt", None, "cond",
          {"OPERAND1": p.var("x"), "OPERAND2": p.var("y")})
    p.add("err1", "operator_equals", None, "cond",
          {"OPERAND1": p.var("e"), "OPERAND2": p.num(1)})
    p.add("chset", "data_setvariableto", "iff", "loop", {}, {"VARIABLE": ["s", "vs"]})
    p.blocks["chset"]["inputs"] = {"VALUE": [3, "ch", [10, ""]]}
    p.add("ch", "operator_letter_of", None, "chset",
          {"LETTER": p.var("x"), "STRING": p.text("abcde")})
    p.add("iff", "control_if_else", None, "chset",
          {"CONDITION": [2, "eqx"], "SUBSTACK": [2, "inc"], "SUBSTACK2": [2, "iff2"]})
    p.add("eqx", "operator_equals", None, "iff",
          {"OPERAND1": [3, "chv", [10, ""]], "OPERAND2": p.text("c")})
    p.add("chv", "data_variable", None, "eqx", {}, {"VARIABLE": ["s", "vs"]})
    p.changevar("inc", "x", None, "iff", p.num(1))
    p.add("iff2", "control_if_else", None, "iff",
          {"CONDITION": [2, "eqy"], "SUBSTACK": [2, "sete"],
           "SUBSTACK2": [2, "inc2"]})
    p.add("eqy", "operator_equals", None, "iff2",
          {"OPERAND1": [3, "chv2", [10, ""]], "OPERAND2": p.text("e")})
    p.add("chv2", "data_variable", None, "eqy", {}, {"VARIABLE": ["s", "vs"]})
    p.setvar("sete", "e", None, "iff2", p.num(1))
    p.changevar("inc2", "x", None, "iff2", p.num(1))
    p.listop("tag", "data_addtolist", "L", None, "loop", {"ITEM": p.var("x")})


def case_proc_param_passthrough(p):
    """手続き引数を別の手続きへそのまま渡す（引数レポータを引数に使う）。"""
    outer = proc_mut("outer %s", ["s1"], ["argS1"])
    inner = proc_mut("inner %s", ["s2"], ["argS2"])
    p.add("hat", "event_whenflagclicked", "call1", top=True)
    p.add("call1", "procedures_call", "tag", "hat",
          {"argS1": p.text("hello")}, mutation=outer)
    p.listop("tag", "data_addtolist", "L", None, "call1", {"ITEM": p.var("x")})
    p.add("defO", "procedures_definition", "call2", None,
          {"custom_block": [1, "protoO"]}, top=True)
    p.add("protoO", "procedures_prototype", None, "defO",
          {"argS1": [1, "argshO"]}, shadow=True, mutation=outer)
    p.add("argshO", "argument_reporter_string_number", None, "protoO",
          {}, {"VALUE": ["s1", None]}, shadow=True)
    p.add("call2", "procedures_call", "addL2", "defO",
          {"argS2": [3, "argpas", [12, "s1", None]]}, mutation=inner)
    p.add("argpas", "argument_reporter_string_number", None, "call2",
          {}, {"VALUE": ["s1", None]})
    p.listop("addL2", "data_addtolist", "L", None, "call2",
             {"ITEM": [2, "letter2"]})
    p.add("letter2", "operator_letter_of", None, "addL2",
          {"LETTER": p.num(2), "STRING": [3, "argO", [12, "s1", None]]})
    p.add("argO", "argument_reporter_string_number", None, "letter2",
          {}, {"VALUE": ["s1", None]})
    p.add("defI", "procedures_definition", "addL3", None,
          {"custom_block": [1, "protoI"]}, top=True)
    p.add("protoI", "procedures_prototype", None, "defI",
          {"argS2": [1, "argshI"]}, shadow=True, mutation=inner)
    p.add("argshI", "argument_reporter_string_number", None, "protoI",
          {}, {"VALUE": ["s2", None]}, shadow=True)
    p.listop("addL3", "data_addtolist", "L", "addL4", "defI",
             {"ITEM": [2, "lenI"]})
    p.add("lenI", "operator_length", None, "addL3",
          {"STRING": [3, "argI", [12, "s2", None]]})
    p.add("argI", "argument_reporter_string_number", None, "lenI",
          {}, {"VALUE": ["s2", None]})
    p.listop("addL4", "data_addtolist", "L", None, "addL3",
             {"ITEM": [2, "letter1"]})
    p.add("letter1", "operator_letter_of", None, "addL4",
          {"LETTER": p.num(1), "STRING": [3, "argI2", [12, "s2", None]]})
    p.add("argI2", "argument_reporter_string_number", None, "letter1",
          {}, {"VALUE": ["s2", None]})


def case_norm_string_build(p):
    """normalize 相当: ループで 1 文字ずつ join して文字列を作り、
    item # of で変換表を引く（calculator の normalize/set n to length）。"""
    mut = proc_mut("norm %s", ["rawIn"], ["argRaw"])
    # L = 変換元, M = 変換先
    p.lists["vL"][1][:] = ["a", "x", "1"]
    p.lists["vM"][1][:] = ["A", "X", "one"]
    p.add("hat", "event_whenflagclicked", "call", top=True)
    p.add("call", "procedures_call", "tag", "hat",
          {"argRaw": p.text("ax1b")}, mutation=mut)
    p.listop("tag", "data_addtolist", "M", None, "call", {"ITEM": p.var("s")})
    p.add("def", "procedures_definition", "clr", None,
          {"custom_block": [1, "proto"]}, top=True)
    p.add("proto", "procedures_prototype", None, "def",
          {"argRaw": [1, "argsh"]}, shadow=True, mutation=mut)
    p.add("argsh", "argument_reporter_string_number", None, "proto",
          {}, {"VALUE": ["rawIn", None]}, shadow=True)
    p.setvar("clr", "s", "seti", "def", p.text(""))
    p.setvar("seti", "x", "loop", "clr", p.num(1))
    p.add("loop", "control_repeat", "done", "seti",
          {"TIMES": [2, "lenb"], "SUBSTACK": [2, "chset"]})
    p.add("lenb", "operator_length", None, "loop",
          {"STRING": [3, "argl", [12, "rawIn", None]]})
    p.add("argl", "argument_reporter_string_number", None, "lenb",
          {}, {"VALUE": ["rawIn", None]})
    p.add("chset", "data_setvariableto", "lookup", "loop", {}, {"VARIABLE": ["e", "ve"]})
    p.blocks["chset"]["inputs"] = {"VALUE": [2, "chb"]}
    p.add("chb", "operator_letter_of", None, "chset",
          {"LETTER": p.var("x"), "STRING": [3, "argc2", [12, "rawIn", None]]})
    p.add("argc2", "argument_reporter_string_number", None, "chb",
          {}, {"VALUE": ["rawIn", None]})
    p.add("lookup", "data_setvariableto", "iff", "chset", {}, {"VARIABLE": ["y", "vy"]})
    p.blocks["lookup"]["inputs"] = {"VALUE": [2, "idx"]}
    p.add("idx", "data_itemnumoflist", None, "lookup",
          {"ITEM": [3, "chv", [12, "e", None]]})
    p.add("chv", "data_variable", None, "idx", {}, {"VARIABLE": ["e", "ve"]})
    p.add("iff", "control_if_else", None, "lookup",
          {"CONDITION": [2, "gt0"], "SUBSTACK": [2, "joinM"],
           "SUBSTACK2": [2, "joinCh"]})
    p.add("gt0", "operator_gt", None, "iff",
          {"OPERAND1": p.var("y"), "OPERAND2": p.num(0)})
    p.add("joinM", "data_setvariableto", None, "iff", {}, {"VARIABLE": ["s", "vs"]})
    p.blocks["joinM"]["inputs"] = {"VALUE": [2, "j1"]}
    p.add("j1", "operator_join", None, "joinM",
          {"STRING1": p.var("s"), "STRING2": [2, "it"]})
    p.add("it", "data_itemoflist", None, "j1",
          {"INDEX": p.var("y")})
    p.blocks["it"]["fields"] = {"LIST": ["M", "vM"]}
    p.add("joinCh", "data_setvariableto", None, "iff", {}, {"VARIABLE": ["s", "vs"]})
    p.blocks["joinCh"]["inputs"] = {"VALUE": [2, "j2"]}
    p.add("j2", "operator_join", None, "joinCh",
          {"STRING1": p.var("s"), "STRING2": [3, "chv2", [12, "e", None]]})
    p.add("chv2", "data_variable", None, "j2", {}, {"VARIABLE": ["e", "ve"]})
    p.add("inc", "data_changevariableby", None, "chset", {}, {"VARIABLE": ["x", "vx"]})
    p.blocks["inc"]["inputs"] = {"VALUE": p.num(1)}
    # inc を loop 本体の最後に繋ぐ
    p.blocks["iff"]["next"] = "inc"
    p.blocks["inc"]["next"] = None


def case_stage_vars(p):
    """ブロックはスプライト、変数/リストはステージ（グローバル）に置く。
    calculator プロジェクトは全変数をステージに持つ。"""
    p.stage_vars = True
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "s2", "hat", p.num(7))
    p.add("s2", "data_setvariableto", "s3", "s1", {}, {"VARIABLE": ["s", "vs"]})
    p.blocks["s2"]["inputs"] = {"VALUE": [2, "j"]}
    p.add("j", "operator_join", None, "s2", {"STRING1": p.text("a"), "STRING2": p.text("b")})
    p.add("s3", "data_changevariableby", "s4", "s2", {}, {"VARIABLE": ["x", "vx"]})
    p.blocks["s3"]["inputs"] = {"VALUE": p.num(3)}
    p.listop("s4", "data_addtolist", "L", "s5", "s3", {"ITEM": p.var("x")})
    p.listop("s5", "data_addtolist", "L", "s6", "s4", {"ITEM": p.var("s")})
    p.add("s6", "data_setvariableto", None, "s5", {}, {"VARIABLE": ["e", "ve"]})
    p.blocks["s6"]["inputs"] = {"VALUE": [2, "len"]}
    p.add("len", "operator_length", None, "s6", {"STRING": [3, "sv", [12, "s", "vs"]]})
    p.add("sv", "data_variable", None, "len", {}, {"VARIABLE": ["s", "vs"]})


def case_proc_reporter(p):
    """reporter 手続き（procedures_return で値を返し、入力として使う）。"""
    mut = proc_mut("inc %s", ["n"], ["argN"], ret=True)
    # hat: x = inc(5); L += x; L += inc(x); L += (inc 0 を文として呼ぶ)
    p.add("hat", "event_whenflagclicked", "s1", top=True)
    p.setvar("s1", "x", "a1", "hat", [2, "callv"])
    p.add("callv", "procedures_call", None, "s1", {"argN": p.num(5)},
          mutation=mut)
    p.listop("a1", "data_addtolist", "L", "a2", "s1", {"ITEM": p.var("x")})
    p.add("a2", "data_setvariableto", "a3", "a1", {}, {"VARIABLE": ["y", "vy"]})
    p.blocks["a2"]["inputs"] = {"VALUE": [2, "callv2"]}
    p.add("callv2", "procedures_call", None, "a2",
          {"argN": [3, "xv", [12, "x", "vx"]]}, mutation=mut)
    p.add("xv", "data_variable", None, "callv2", {}, {"VARIABLE": ["x", "vx"]})
    p.listop("a3", "data_addtolist", "L", "a4", "a2", {"ITEM": p.var("y")})
    p.add("a4", "procedures_call", "a5", "a3", {"argN": p.num(0)},
          mutation=mut)                      # 文としての reporter 呼び出し
    p.listop("a5", "data_addtolist", "L", None, "a4", {"ITEM": p.var("x")})
    # 定義: return (n + 1)
    p.add("def", "procedures_definition", "iff", None,
          {"custom_block": [1, "proto"]}, top=True)
    p.add("proto", "procedures_prototype", None, "def",
          {"argN": [1, "argsh"]}, shadow=True, mutation=mut)
    p.add("argsh", "argument_reporter_string_number", None, "proto",
          {}, {"VALUE": ["n", None]}, shadow=True)
    p.add("iff", "control_if", None, "def",
          {"CONDITION": [2, "cmp"], "SUBSTACK": [2, "ret0"]})
    p.add("cmp", "operator_gt", None, "iff",
          {"OPERAND1": [3, "argc", [12, "n", None]], "OPERAND2": p.num(0)})
    p.add("argc", "argument_reporter_string_number", None, "cmp",
          {}, {"VALUE": ["n", None]})
    p.add("ret0", "procedures_return", "ret1", "iff", {"VALUE": [2, "plus"]})
    p.add("plus", "operator_add", None, "ret0",
          {"NUM1": [3, "argn", [12, "n", None]], "NUM2": p.num(1)})
    p.add("argn", "argument_reporter_string_number", None, "plus",
          {}, {"VALUE": ["n", None]})
    p.add("ret1", "procedures_return", None, "iff", {"VALUE": p.num(-1)})


CASES = [case_zero_repeat, case_small, case_repeat, case_repeat_frac, case_until, case_while,
         case_ifelse, case_nested, case_strings, case_lists, case_math,
         case_stringvar, case_negzero_mod, case_int_replace_var_item, case_guarded_entry, case_guarded_nonneg, case_list_item_int, case_compare_strings, case_interleave,
         case_string_grow, case_two_hats, case_warp_counter, case_warp_zero,
         case_warp_nested, case_warp_math_chunk, case_proc_param_loop, case_proc_recursion, case_promoted_parse, case_parse_var_bound, case_proc_param_passthrough, case_stage_vars, case_proc_reporter, case_mod_rounding, case_mod_rounding_bits, case_broadcastwait,
         case_const_literal_loop, case_const_literal_while, case_const_literal_nested, case_box_repeat, case_box_bail, case_box_repeat_const, case_box_repeat_dynpois, case_box_zero_trip, case_box_neg_trip, case_box_big_trip]
LOOP_CASES = {"case_zero_repeat", "case_repeat", "case_repeat_frac", "case_until", "case_while",
              "case_int_replace_var_item", "case_negzero_mod", "case_guarded_entry", "case_guarded_nonneg", "case_list_item_int",
              "case_const_literal_loop", "case_const_literal_while",
              "case_const_literal_nested", "case_box_repeat", "case_box_bail", "case_box_repeat_const", "case_box_repeat_dynpois", "case_box_zero_trip", "case_box_neg_trip", "case_box_big_trip",
              "case_ifelse", "case_nested", "case_string_grow",
              "case_warp_counter", "case_warp_zero", "case_warp_nested",
              "case_warp_math_chunk",
              "case_proc_param_loop", "case_proc_recursion", "case_promoted_parse", "case_parse_var_bound", "case_proc_param_passthrough", "case_stage_vars", "case_proc_reporter", "case_mod_rounding"}

# 入口ガード付き複製を使うケース。emit 行の guard=N が 0 でないことを要求。
GUARD_CASES = {"case_guarded_entry", "case_guarded_nonneg", "case_box_repeat", "case_box_bail", "case_box_repeat_const", "case_box_repeat_dynpois", "case_box_zero_trip", "case_box_neg_trip", "case_box_big_trip"}

# 整数昇格ループを使うケース。emit 行の int=N が 0 でないことを要求。
INT_CASES = {"case_guarded_entry", "case_guarded_nonneg", "case_list_item_int", "case_box_repeat", "case_box_bail", "case_box_repeat_const", "case_box_repeat_dynpois", "case_box_zero_trip", "case_box_neg_trip", "case_box_big_trip", "case_warp_math_chunk"}

only = os.environ.get("XCHECK_ONLY")
cases = [c for c in CASES if not only or c.__name__ in only.split(",")]
fail = 0
for case in cases:
    try:
        # hotspot は昇格待ちなので、小さいスクリプト（手続き呼び出しだけの
        # ハット等）は MLIR に到達せず emit が出ないことがある。hotspot では
        # 値の一致だけを要求し、emit の要求は eager に限る。
        need_mlir = os.environ.get("XCHECK_MODE", "eager") != "hotspot"
        if case.__name__ == "case_broadcastwait":
            # native lower するので fallback は不要。hotspot では小規模
            # すぎて MLIR コンパイル自体が走らないためパリティのみ見る
            # （ハング回帰の検出は eager が担う）。
            eager = os.environ.get("XCHECK_MODE", "eager") != "hotspot"
            if not compare(case.__name__, case, require_mlir=eager,
                           require_fallback=False):
                fail += 1
            continue
        if not compare(case.__name__, case, require_mlir=need_mlir):
            fail += 1
    except Exception as e:
        print("ERROR", case.__name__, e)
        fail += 1
print(f"{len(cases) - fail}/{len(cases)} passed")
sys.exit(1 if fail else 0)
