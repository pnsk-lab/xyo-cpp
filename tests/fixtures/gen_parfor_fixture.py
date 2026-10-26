#!/usr/bin/env python3
"""par-for（並列 append ループ）の意味論検証フィクスチャを生成する。

parfor_fill.sb3 — warp 手続きの中で

    set i to 0
    repeat N
      change i by 1
      add (sqrt of (i*i + 1)) to L

を実行する。JIT はこのループを「反復ごとに L へ 1 個 append・カウンタは
整数 +1」と認識し、索引書き込みへ privatize して並列実行する。
--jit off（interp）と JIT で L の内容・i の最終値が一致することを
XYO_DUMPVARS で比較する。

  python3 tests/fixtures/gen_parfor_fixture.py tests/fixtures/parfor_fill.sb3 [N]
  ./build/xyo run tests/fixtures/parfor_fill.sb3 --frames 1
  ./build/xyo run tests/fixtures/parfor_fill.sb3 --jit off --frames 1
"""
import json
import sys
import zipfile


def project(n, heavy=False):
    # --- スプライトのブロック（warp 手続き + 旗）---
    blocks = {
        # 旗 → parfor fill N
        "hat": {"opcode": "event_whenflagclicked", "next": "call",
                "parent": None, "inputs": {}, "fields": {},
                "topLevel": True, "shadow": False, "x": 0, "y": 0},
        "call": {"opcode": "procedures_call", "next": None, "parent": "hat",
                 "inputs": {"argN": [1, "argn"]}, "fields": {},
                 "topLevel": False, "shadow": False,
                 "mutation": {"tagName": "mutation", "children": [],
                              "proccode": "fill %s",
                              "argumentids": "[\"argN\"]",
                              "argumentnames": "[\"n\"]",
                              "argumentdefaults": "[\"0\"]",
                              "warp": "true"}},
        "argn": {"opcode": "math_number", "next": None, "parent": "call",
                 "inputs": {}, "fields": {"NUM": [n, None]},
                 "topLevel": False, "shadow": True},

        # 定義 fill %s（warp）
        "def": {"opcode": "procedures_definition", "next": "seti",
                "parent": None, "inputs": {"custom_block": [1, "proto"]},
                "fields": {}, "topLevel": True, "shadow": False,
                "x": 0, "y": 400},
        "proto": {"opcode": "procedures_prototype", "next": None,
                  "parent": "def",
                  "inputs": {"argN": [1, "argn2"]}, "fields": {},
                  "topLevel": False, "shadow": True,
                  "mutation": {"tagName": "mutation", "children": [],
                               "proccode": "fill %s",
                               "argumentids": "[\"argN\"]",
                               "argumentnames": "[\"n\"]",
                               "argumentdefaults": "[\"0\"]",
                               "warp": "true"}},
        "argn2": {"opcode": "argument_reporter_string_number", "next": None,
                  "parent": "proto", "inputs": {},
                  "fields": {"VALUE": ["n", None]},
                  "topLevel": False, "shadow": True},

        "seti": {"opcode": "data_setvariableto", "next": "loop",
                 "parent": "def", "inputs": {"VALUE": [1, [10, "0"]]},
                 "fields": {"VARIABLE": ["i", "vi"]},
                 "topLevel": False, "shadow": False},
        "loop": {"opcode": "control_repeat", "next": None, "parent": "seti",
                 "inputs": {"TIMES": [3, "argr", [6, "0"]],
                            "SUBSTACK": [2, "chg"]},
                 "fields": {}, "topLevel": False, "shadow": False},
        "argr": {"opcode": "argument_reporter_string_number", "next": None,
                 "parent": "loop", "inputs": {},
                 "fields": {"VALUE": ["n", None]},
                 "topLevel": False, "shadow": False},
        # 本体: change i by 1 / add (sqrt of (i*i + 1)) to L
        "chg": {"opcode": "data_changevariableby", "next": "add",
                "parent": "loop", "inputs": {"VALUE": [1, [4, "1"]]},
                "fields": {"VARIABLE": ["i", "vi"]},
                "topLevel": False, "shadow": False},
        "add": {"opcode": "data_addtolist", "next": None, "parent": "chg",
                "inputs": {"ITEM": [3, "sqrt", [10, "0"]]},
                "fields": {"LIST": ["L", "vL"]},
                "topLevel": False, "shadow": False},
        "sqrt": {"opcode": "operator_mathop", "next": None, "parent": "add",
                 "inputs": {"NUM": [3, "sinb" if heavy else "plus", [4, "0"]]},
                 "fields": {"OPERATOR": ["sqrt", None]},
                 "topLevel": False, "shadow": False},
        "sinb": {"opcode": "operator_mathop", "next": None, "parent": "sqrt",
                 "inputs": {"NUM": [3, "plus", [4, "0"]]},
                 "fields": {"OPERATOR": ["sin", None]},
                 "topLevel": False, "shadow": False},
        "plus": {"opcode": "operator_add", "next": None, "parent": "sqrt",
                 "inputs": {"NUM1": [3, "mul", [4, "0"]],
                            "NUM2": [1, [4, "1"]]},
                 "fields": {}, "topLevel": False, "shadow": False},
        "mul": {"opcode": "operator_multiply", "next": None, "parent": "plus",
                "inputs": {"NUM1": [3, "rdi", [4, "0"]],
                           "NUM2": [3, "rdi2", [4, "1"]]},
                "fields": {}, "topLevel": False, "shadow": False},
        "rdi": {"opcode": "data_variable", "next": None, "parent": "mul",
                "inputs": {}, "fields": {"VARIABLE": ["i", "vi"]},
                "topLevel": False, "shadow": False},
        "rdi2": {"opcode": "data_variable", "next": None, "parent": "mul",
                 "inputs": {}, "fields": {"VARIABLE": ["i", "vi"]},
                 "topLevel": False, "shadow": False},
    }

    stage = {
        "isStage": True, "name": "Stage",
        "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
        "comments": {}, "costumes": [], "sounds": [],
        "currentCostume": 0, "volume": 100, "layerOrder": 0,
    }
    sprite = {
        "isStage": False, "name": "S", "layerOrder": 1,
        "variables": {"vi": ["i", 0]},
        "lists": {"vL": ["L", []]},
        "broadcasts": {}, "blocks": blocks, "comments": {},
        "costumes": [], "sounds": [],
        "currentCostume": 0, "volume": 100,
        "visible": True, "x": 0, "y": 0, "size": 100, "direction": 90,
        "draggable": False, "rotationStyle": "all around",
    }
    return {"targets": [stage, sprite], "monitors": [], "extensions": [],
            "meta": {"semver": "3.0.0", "vm": "0", "agent": "t"}}


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures/parfor_fill.sb3"
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 20000
    heavy = len(sys.argv) > 3 and sys.argv[3] == "heavy"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(project(n, heavy)))
    print("wrote", out, "n =", n)


if __name__ == "__main__":
    main()
