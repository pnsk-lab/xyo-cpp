#!/usr/bin/env python3
"""並列実行の正しさ検証フィクスチャを生成する。

par_order.sb3   — 8 スプライトが共有変数 s に join(s,"i") で追記。
                  全員が var:s で衝突 → 逐次グループ → spawn 順に追記。
                  executableTargets は TW 準拠で逆順走査 → spawn 順 S7→S0、
                  最後に走る S0 が s=="76543210" を検証して say pass/fail/end。
par_indep.sb3   — 8 スプライトがそれぞれ自分の変数 v_i を repeat で +1
                  （スクリプトは安全 → 並列対象）。stage の別スレッドが
                  wait_until(Σ sensing_of(Si,v) == 8*M) で完了を検出して
                  say pass/end。sum が所定値に届く = 全スレッド完了。
"""
import json, zipfile

def zproj(targets):
    return {"targets": targets, "monitors": [], "extensions": [],
            "meta": {"semver": "3.0.0", "vm": "0", "agent": "t"}}

def write_sb3(path, proj):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(proj))
    print("wrote", path)

# ---------- par_order ----------
def order_fixture(path):
    targets = [{
        "isStage": True, "name": "Stage",
        "variables": {"s": ["s", ""]},
        "lists": {}, "broadcasts": {}, "blocks": {},
        "costumes": [], "sounds": [], "currentCostume": 0,
    }]
    for i in range(8):
        last = (i == 0)
        blocks = {
            "hat": {"opcode": "event_whenflagclicked", "next": "app",
                    "fields": {}, "inputs": {}, "topLevel": True},
            "app": {"opcode": "data_setvariableto",
                    "next": "chk" if last else None, "parent": "hat",
                    "fields": {"VARIABLE": ["s", "s"]},
                    "inputs": {"VALUE": [2, "jn"]}},
            "jn": {"opcode": "operator_join", "next": None, "parent": "app",
                   "fields": {},
                   "inputs": {"STRING1": [2, "rd"],
                              "STRING2": [1, [10, str(i)]]}},
            "rd": {"opcode": "data_variable", "next": None, "parent": "jn",
                   "fields": {"VARIABLE": ["s", "s"]}, "inputs": {}},
        }
        if last:
            blocks["chk"] = {
                "opcode": "control_if_else", "next": "end", "parent": "app",
                "fields": {},
                "inputs": {"CONDITION": [2, "eq"],
                           "SUBSTACK": [2, "sayok"],
                           "SUBSTACK2": [2, "sayng"]}}
            blocks["eq"] = {
                "opcode": "operator_equals", "next": None, "parent": "chk",
                "fields": {},
                "inputs": {"OPERAND1": [2, "rd2"],
                           "OPERAND2": [1, [10, "76543210"]]}}
            blocks["rd2"] = {
                "opcode": "data_variable", "next": None, "parent": "eq",
                "fields": {"VARIABLE": ["s", "s"]}, "inputs": {}}
            blocks["sayok"] = {
                "opcode": "looks_say", "next": None, "parent": "chk",
                "fields": {}, "inputs": {"MESSAGE": [1, [10, "pass"]]} }
            blocks["sayng"] = {
                "opcode": "looks_say", "next": None, "parent": "chk",
                "fields": {},
                "inputs": {"MESSAGE": [2, "jfail"]}}
            blocks["jfail"] = {
                "opcode": "operator_join", "next": None, "parent": "sayng",
                "fields": {},
                "inputs": {"STRING1": [1, [10, "fail s="]],
                           "STRING2": [2, "rd3"]}}
            blocks["rd3"] = {
                "opcode": "data_variable", "next": None, "parent": "jfail",
                "fields": {"VARIABLE": ["s", "s"]}, "inputs": {}}
            blocks["end"] = {
                "opcode": "looks_say", "next": None, "parent": "chk",
                "fields": {}, "inputs": {"MESSAGE": [1, [10, "end"]]} }
        targets.append({
            "isStage": False, "name": f"S{i}", "layerOrder": i + 1,
            "variables": {}, "lists": {}, "broadcasts": {},
            "blocks": blocks, "costumes": [], "sounds": [],
            "currentCostume": 0})
    write_sb3(path, zproj(targets))

# ---------- par_indep ----------
def indep_fixture(path, n=8, m=2000):
    # stage: wait_until Σ_i sensing_of(Si,"v") == n*m → say pass; say end
    sb = {
        "hat": {"opcode": "event_whenflagclicked", "next": "wu",
                "fields": {}, "inputs": {}, "topLevel": True},
        "wu": {"opcode": "control_wait_until", "next": "sayok",
               "parent": "hat", "fields": {},
               "inputs": {"CONDITION": [2, "eq"]}},
        "eq": {"opcode": "operator_equals", "next": None, "parent": "wu",
               "fields": {},
               "inputs": {"OPERAND1": [2, "sum0"],
                          "OPERAND2": [1, [4, str(n * m)]]}},
        "sayok": {"opcode": "looks_say", "next": "end", "parent": "wu",
                  "fields": {}, "inputs": {"MESSAGE": [1, [10, "pass"]]}},
        "end": {"opcode": "looks_say", "next": None, "parent": "sayok",
                "fields": {}, "inputs": {"MESSAGE": [1, [10, "end"]]}},
    }
    # sum_i = of(S_i, v) + sum_{i+1} の右再帰チェーン
    for i in range(n):
        cur = f"sum{i}"
        sb[cur] = {
            "opcode": "operator_add", "next": None,
            "parent": "eq" if i == 0 else f"sum{i-1}",
            "fields": {},
            "inputs": {"NUM1": [2, f"of{i}"],
                       "NUM2": ([2, f"sum{i+1}"] if i < n - 1
                                else [1, [4, "0"]])}}
        sb[f"of{i}"] = {
            "opcode": "sensing_of", "next": None, "parent": cur,
            "fields": {"PROPERTY": ["v", None]},
            "inputs": {"OBJECT": [1, [10, f"S{i}"]]}}
    targets = [{
        "isStage": True, "name": "Stage", "variables": {},
        "lists": {}, "broadcasts": {}, "blocks": sb,
        "costumes": [], "sounds": [], "currentCostume": 0,
    }]
    for i in range(n):
        vid = f"v{i}"
        blocks = {
            "hat": {"opcode": "event_whenflagclicked", "next": "rep",
                    "fields": {}, "inputs": {}, "topLevel": True},
            "rep": {"opcode": "control_repeat", "next": None,
                    "parent": "hat", "fields": {},
                    "inputs": {"TIMES": [1, [4, str(m)]],
                               "SUBSTACK": [2, "inc"]}},
            "inc": {"opcode": "data_changevariableby", "next": None,
                    "parent": "rep", "fields": {"VARIABLE": ["v", vid]},
                    "inputs": {"VALUE": [1, [4, "1"]]}},
        }
        targets.append({
            "isStage": False, "name": f"S{i}", "layerOrder": i + 1,
            "variables": {vid: ["v", 0]}, "lists": {}, "broadcasts": {},
            "blocks": blocks, "costumes": [], "sounds": [],
            "currentCostume": 0})
    write_sb3(path, zproj(targets))

order_fixture("/tmp/par_order.sb3")
indep_fixture("/tmp/par_indep.sb3")
