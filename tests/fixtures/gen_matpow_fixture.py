#!/usr/bin/env python3
"""matpow（線形漸化式ループの行列累乗）用のベンチ・確認フィクスチャ生成器。

  python3 tests/fixtures/gen_matpow_fixture.py [outdir]

生成物（warp 手続き run %s の中に repeat を置いた最小プロジェクト）:
  matpow_fib.sb3     repeat(n) { t=a+b; a=b; b=t }        （n=60, fib）
  matpow_incr.sb3    repeat(n) { change x by 3 }          （n=2e9, 一定時間）
  matpow_nested.sb3  repeat(n) { repeat(2) { change x by 2 } }
  matpow_mlir.sb3    repeat(n) { change x by 1; change y by 2 }
  matpow_negstep.sb3 repeat(n) { change x by -3 }          （負の係数）
  matpow_dyninner.sb3 repeat(n) { repeat k { change x by 1 } }（内側が動的回数）
  matpow_until.sb3   repeat until i < 1 { change i by -1; change s by i }
  matpow_while.sb3   repeat while i > 0 { change i by -1; change s by 1 }
matpow が効くのは warp 手続き内かつ範囲証明（整数・|値| < 2^52）が立つ場合だけ。
"""
import json
import os
import sys
import zipfile

BASE_BLOCKS = {
    "hat": {"opcode": "event_whenflagclicked", "next": "call", "parent": None,
            "inputs": {}, "fields": {}, "topLevel": True, "shadow": False},
    "call": {"opcode": "procedures_call", "next": None, "parent": "hat",
             "inputs": {"argN": [1, "argn"]}, "fields": {},
             "topLevel": False, "shadow": False,
             "mutation": {"tagName": "mutation", "children": [],
                          "proccode": "run %s", "argumentids": '["argN"]',
                          "argumentnames": '["n"]',
                          "argumentdefaults": '["0"]', "warp": "true"}},
    "argn": {"opcode": "math_number", "next": None, "parent": "call",
             "inputs": {}, "fields": {"NUM": [60, None]},
             "topLevel": False, "shadow": True},
    "def": {"opcode": "procedures_definition", "next": "init0", "parent": None,
            "inputs": {"custom_block": [1, "proto"]}, "fields": {},
            "topLevel": True, "shadow": False},
    "proto": {"opcode": "procedures_prototype", "next": None, "parent": "def",
              "inputs": {"argN": [1, "argn2"]}, "fields": {},
              "topLevel": False, "shadow": True,
              "mutation": {"tagName": "mutation", "children": [],
                           "proccode": "run %s", "argumentids": '["argN"]',
                           "argumentnames": '["n"]',
                           "argumentdefaults": '["0"]', "warp": "true"}},
    "argn2": {"opcode": "argument_reporter_string_number", "next": None,
              "parent": "proto", "inputs": {}, "fields": {"VALUE": ["n", None]},
              "topLevel": False, "shadow": True},
    "loop": {"opcode": "control_repeat", "next": None, "parent": "init0",
             "inputs": {"TIMES": [3, "argr", [6, "0"]],
                        "SUBSTACK": [2, "body0"]},
             "fields": {}, "topLevel": False, "shadow": False},
    "argr": {"opcode": "argument_reporter_string_number", "next": None,
             "parent": "loop", "inputs": {}, "fields": {"VALUE": ["n", None]},
             "topLevel": False, "shadow": False},
}


def setvar(bid, var, vid, nxt, value):
    return {bid: {"opcode": "data_setvariableto", "next": nxt,
                  "parent": "loop", "inputs": {"VALUE": value},
                  "fields": {"VARIABLE": [var, vid]},
                  "topLevel": False, "shadow": False}}


def changevar(bid, var, vid, nxt, value):
    return {bid: {"opcode": "data_changevariableby", "next": nxt,
                  "parent": "loop", "inputs": {"VALUE": value},
                  "fields": {"VARIABLE": [var, vid]},
                  "topLevel": False, "shadow": False}}


def varref(bid, var, vid):
    return {bid: {"opcode": "data_variable", "next": None, "parent": "loop",
                  "inputs": {}, "fields": {"VARIABLE": [var, vid]},
                  "topLevel": False, "shadow": False}}


def add(bid, n1, n2):
    return {bid: {"opcode": "operator_add", "next": None, "parent": "loop",
                  "inputs": {"NUM1": n1, "NUM2": n2}, "fields": {},
                  "topLevel": False, "shadow": False}}


def ref(bid):
    return [3, bid, [4, "0"]]


def num(v):
    return [1, [4, str(v)]]


def write_project(path, times, body, variables):
    blocks = dict(BASE_BLOCKS)
    blocks["loop"]["inputs"]["TIMES"] = [3, "argr", [6, "0"]]
    blocks["argn"]["fields"]["NUM"] = [times, None]
    blocks.update(body)
    blocks["init0"] = {"opcode": "data_setvariableto", "next": "loop",
                       "parent": "def", "inputs": {"VALUE": num(0)},
                       "fields": {"VARIABLE": ["x", "vx"]},
                       "topLevel": False, "shadow": False}
    for bid, b in blocks.items():
        # ループの直前は init0（init0 自身は書き換えない — 自己参照になる）
        if bid != "init0" and b.get("next") == "loop":
            b["next"] = "init0"
    project = {"targets": [
        {"isStage": True, "name": "Stage", "variables": {}, "lists": {},
         "broadcasts": {}, "blocks": {}, "comments": {}, "costumes": [],
         "sounds": [], "currentCostume": 0, "volume": 100, "layerOrder": 0},
        {"isStage": False, "name": "S", "layerOrder": 1, "variables": variables,
         "lists": {}, "broadcasts": {}, "blocks": blocks, "comments": {},
         "costumes": [], "sounds": [], "currentCostume": 0, "volume": 100,
         "visible": True, "x": 0, "y": 0, "size": 100, "direction": 90,
         "draggable": False, "rotationStyle": "all around"}],
        "monitors": [], "extensions": [], "meta": {"semver": "3.0.0"}}
    with zipfile.ZipFile(path, "w") as z:
        z.writestr("project.json", json.dumps(project))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)

    fib = {"body0": setvar("body0", "t", "vt", "body1",
                           [3, "sum", [4, "0"]])["body0"],
           "body1": setvar("body1", "a", "va", "body2",
                           ref("b_r"))["body1"],
           "body2": setvar("body2", "b", "vb", None,
                           ref("t_r"))["body2"]}
    fib.update(add("sum", ref("a_r"), ref("b_r")))
    fib.update(varref("a_r", "a", "va"))
    fib.update(varref("b_r", "b", "vb"))
    fib.update(varref("t_r", "t", "vt"))
    write_project(os.path.join(out, "matpow_fib.sb3"), 60, fib,
                  {"va": ["a", 0], "vb": ["b", 1], "vt": ["t", 0],
                   "vx": ["x", 0]})

    write_project(os.path.join(out, "matpow_incr.sb3"), 2000000000,
                  changevar("body0", "x", "vx", None, num(3)),
                  {"vx": ["x", 0]})

    nested = {"body0": {"opcode": "control_repeat", "next": None,
                        "parent": "loop",
                        "inputs": {"TIMES": [1, [4, "2"]],
                                   "SUBSTACK": [2, "inner0"]},
                        "fields": {}, "topLevel": False, "shadow": False}}
    nested.update(changevar("inner0", "x", "vx", None, num(2)))
    write_project(os.path.join(out, "matpow_nested.sb3"), 1000, nested,
                  {"vx": ["x", 0]})

    two = dict(changevar("body0", "x", "vx", "body1", num(1)))
    two.update(changevar("body1", "y", "vy", None, num(2)))
    write_project(os.path.join(out, "matpow_mlir.sb3"), 200000, two,
                  {"vx": ["x", 0], "vy": ["y", 0], "vx2": ["x2", 0]})

    write_project(os.path.join(out, "matpow_negstep.sb3"), 200,
                  changevar("body0", "x", "vx", None, num(-3)),
                  {"vx": ["x", 0]})

    dyn = {"body0": {"opcode": "control_repeat", "next": None,
                     "parent": "loop",
                     "inputs": {"TIMES": [3, "k_r", [4, "0"]],
                                "SUBSTACK": [2, "inner0"]},
                     "fields": {}, "topLevel": False, "shadow": False},
           "k_r": {"opcode": "data_variable", "next": None, "parent": "body0",
                   "inputs": {}, "fields": {"VARIABLE": ["k", "vk"]},
                   "topLevel": False, "shadow": False}}
    dyn.update(changevar("inner0", "x", "vx", None, num(1)))
    write_project(os.path.join(out, "matpow_dyninner.sb3"), 1000, dyn,
                  {"vx": ["x", 0], "vk": ["k", 3]})

    print("wrote matpow fixtures to", out)


if __name__ == "__main__":
    main()
