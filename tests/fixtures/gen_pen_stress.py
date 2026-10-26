#!/usr/bin/env python3
"""ペン経路の決定論的ストレス用 sb3 を生成する。

`xyo play` で緑旗 → warp 手続き "bench" が
  rows × cols 回「set x / change x」(+ 色変更) を実行する。
- pen_stress_color.sb3   : 1 ストロークごとに set pen color（色変換のコスト込み）
- pen_stress_nocolor.sb3 : 色は最初に 1 回だけ（ストローク発行のみ）
- pen_stress_off.sb3     : ペンを下げない（set x の VM/レンダラ経路のみ）
比較: off→nocolor がペン発行、nocolor→color が色変換のコスト。
"""
import json, sys, zipfile

ROWS = 2000
COLS = 10000


def blocks_for(mode):
    b = {}

    def add(bid, op, nxt=None, parent=None, inputs=None, fields=None,
            top=False, shadow=False, mutation=None):
        d = {"opcode": op, "next": nxt, "parent": parent,
             "inputs": inputs or {}, "fields": fields or {}}
        if top:
            d["topLevel"] = True
        if shadow:
            d["shadow"] = True
        if mutation:
            d["mutation"] = mutation
        b[bid] = d

    add("hat", "event_whenflagclicked", nxt="call", top=True)
    add("call", "procedures_call", parent="hat",
        mutation={"tagName": "mutation", "children": [], "proccode": "bench",
                  "argumentids": "[]", "argumentnames": "[]",
                  "argumentdefaults": "[]", "warp": "true"})
    add("def", "procedures_definition", nxt="setC", top=True,
        inputs={"custom_block": [1, "proto"]})
    add("proto", "procedures_prototype", parent="def", shadow=True,
        mutation={"tagName": "mutation", "children": [], "proccode": "bench",
                  "argumentids": "[]", "argumentnames": "[]",
                  "argumentdefaults": "[]", "warp": "true"})
    add("setC", "data_setvariableto", nxt="setX", parent="def",
        fields={"VARIABLE": ["c", "c"]}, inputs={"VALUE": [1, [4, "0"]]})
    add("setX", "data_setvariableto", nxt="down", parent="setC",
        fields={"VARIABLE": ["x", "x"]}, inputs={"VALUE": [1, [4, "-240"]]})
    if mode == "off":
        add("down", "pen_penUp", nxt="rows", parent="setX")
    else:
        add("down", "pen_penDown", nxt="rows", parent="setX")
    if mode == "color":
        pass
    else:
        # 色は 1 回だけ設定（pen_stress_nocolor / off）
        b["down"]["next"] = "pcol"
        add("pcol", "pen_setPenColorToColor", nxt="rows", parent="down",
            inputs={"COLOR": [1, [4, "3360030"]]})
    add("rows", "control_repeat", parent=b["down"]["next"] if False else None,
        inputs={"TIMES": [1, [4, str(ROWS)]], "SUBSTACK": [2, "rowHead"]})
    b["rows"]["parent"] = "pcol" if "pcol" in b else "down"
    add("rowHead", "data_setvariableto", nxt="cols", parent="rows",
        fields={"VARIABLE": ["x", "x"]}, inputs={"VALUE": [1, [4, "-240"]]})
    add("cols", "control_repeat", parent="rowHead",
        inputs={"TIMES": [1, [4, str(COLS)]], "SUBSTACK": [2, "colHead"]})
    nxt = "chgX"
    if mode == "colorpal":
        # 24 色パレットを巡回 — 往復キャッシュがヒットする実プロジェクト型
        add("chgC", "data_changevariableby", nxt="pal", parent="cols",
            fields={"VARIABLE": ["c", "c"]},
            inputs={"VALUE": [1, [4, "1"]]})
        add("pal", "data_setvariableto", nxt="setCol", parent="chgC",
            fields={"VARIABLE": ["col", "col"]},
            inputs={"VALUE": [3, "palMul", [4, "0"]]})
        add("palMod", "operator_mod", parent="palMul",
            inputs={"NUM1": [3, "varC", [4, "0"]],
                    "NUM2": [1, [4, "24"]]})
        add("varC0", "data_variable", parent="palMod",
            fields={"VARIABLE": ["c", "c"]})
        add("palMul", "operator_multiply", parent="pal",
            inputs={"NUM1": [3, "palMod", [4, "0"]],
                    "NUM2": [1, [4, "699051"]]})
        add("setCol", "pen_setPenColorToColor", nxt="setx", parent="pal",
            inputs={"COLOR": [3, "varCol", [4, "0"]]})
        add("varCol", "data_variable", parent="setCol",
            fields={"VARIABLE": ["col", "col"]})
        nxt = "setx"
    elif mode == "color":
        add("setCol", "pen_setPenColorToColor", nxt="chgC", parent="cols",
            inputs={"COLOR": [3, "varC", [4, "0"]]})
        add("varC", "data_variable", parent="setCol",
            fields={"VARIABLE": ["c", "c"]})
        add("chgC", "data_changevariableby", nxt="setx", parent="setCol",
            fields={"VARIABLE": ["c", "c"]},
            inputs={"VALUE": [1, [4, "7919"]]})
        nxt = "setx"
    else:
        add("chgC", "data_changevariableby", nxt="setx", parent="cols",
            fields={"VARIABLE": ["c", "c"]},
            inputs={"VALUE": [1, [4, "7919"]]})
        nxt = "chgC"
    add("setx", "motion_setx", nxt="chgX", parent="chgC" if mode == "color" else "chgC",
        inputs={"X": [3, "varX", [4, "0"]]})
    add("varX", "data_variable", parent="setx", fields={"VARIABLE": ["x", "x"]})
    add("chgX", "data_changevariableby", parent="setx",
        fields={"VARIABLE": ["x", "x"]}, inputs={"VALUE": [1, [4, "1"]]})
    b["cols"]["inputs"]["SUBSTACK"] = [2, "colHead"]
    if mode == "colorpal":
        b["colHead"] = b.pop("chgC")
        b["colHead"]["parent"] = "cols"
    elif mode == "color":
        b["colHead"] = b.pop("setCol")  # 最初のブロック id を colHead に
        b["colHead"]["parent"] = "cols"
    else:
        b["colHead"] = b.pop("chgC")
        b["colHead"]["parent"] = "cols"
        b["chgC"] = b.pop("chgX") if False else b.get("chgC")
        # chgC を colHead にしたので、setx -> chgX の chain を維持
        b["chgC"] = b["colHead"]
    return b


def build(mode, out):
    src = "examples/play_demo.sb3"
    with zipfile.ZipFile(src) as z:
        pj = json.loads(z.read("project.json"))
        assets = {n: z.read(n) for n in z.namelist() if n != "project.json"}
    stage, cat = pj["targets"][0], pj["targets"][1]
    cat["blocks"] = blocks_for(mode)
    cat["variables"] = {"c": ["c", 0], "x": ["x", 0], "col": ["col", 0]}
    stage["variables"] = {}
    stage["lists"] = {}
    cat["lists"] = {}
    with zipfile.ZipFile(out, "w", zipfile.ZIP_STORED) as z:
        z.writestr("project.json", json.dumps(pj, separators=(",", ":")))
        for n, d in assets.items():
            z.writestr(n, d)
    print("wrote", out)


if __name__ == "__main__":
    base = sys.argv[1] if len(sys.argv) > 1 else "tests/fixtures"
    for m in ("color", "colorpal", "nocolor", "off"):
        build(m, f"{base}/pen_stress_{m}.sb3")
