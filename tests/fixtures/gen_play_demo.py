#!/usr/bin/env python3
"""xyo play 用デモ sb3 を生成する。

- SVG コスチューム付きスプライト（緑旗でペン下げ→正方形を描く + 矢印キーで移動）
- 表示モニタ（score 変数） + リストモニタ
- say バブル + stage 背景 SVG
"""
import json, zipfile, sys

CAT_SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96" viewBox="0 0 96 96">
  <ellipse cx="48" cy="60" rx="34" ry="30" fill="#ff8c1a"/>
  <circle cx="48" cy="34" r="24" fill="#ff8c1a"/>
  <circle cx="40" cy="30" r="4" fill="#fff"/>
  <circle cx="56" cy="30" r="4" fill="#fff"/>
  <circle cx="40" cy="30" r="2" fill="#222"/>
  <circle cx="56" cy="30" r="2" fill="#222"/>
  <polygon points="30,16 40,26 30,30" fill="#ff8c1a"/>
  <polygon points="66,16 56,26 66,30" fill="#ff8c1a"/>
</svg>'''

BG_SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360" viewBox="0 0 480 360">
  <rect width="480" height="360" fill="#cfe8ff"/>
  <rect y="300" width="480" height="60" fill="#9adc8f"/>
  <circle cx="70" cy="60" r="30" fill="#fff59d"/>
</svg>'''

def proj():
    sprite_blocks = {
        "hat": {"opcode": "event_whenflagclicked", "next": "say",
                "fields": {}, "inputs": {}, "topLevel": True},
        "say": {"opcode": "looks_say", "next": "setscore", "parent": "hat",
                "fields": {}, "inputs": {"MESSAGE": [1, [10, "hello xyo"]]}},
        "setscore": {"opcode": "data_setvariableto", "next": "down",
                "parent": "say",
                "fields": {"VARIABLE": ["score", "score"]},
                "inputs": {"VALUE": [1, [4, "0"]]}},
        "down": {"opcode": "pen_penDown", "next": "psize", "parent": "setscore",
                "fields": {}, "inputs": {}},
        "psize": {"opcode": "pen_setPenSizeTo", "next": "pcol", "parent": "down",
                "fields": {}, "inputs": {"SIZE": [1, [4, "5"]]}},
        "pcol": {"opcode": "pen_setPenColorToColor", "next": "rep",
                "parent": "psize",
                "fields": {}, "inputs": {"COLOR": [1, [9, "#cc0033"]]}},
        "rep": {"opcode": "control_repeat", "next": "up", "parent": "pcol",
                "fields": {},
                "inputs": {"TIMES": [1, [4, "4"]], "SUBSTACK": [2, "mv"]}},
        "mv": {"opcode": "motion_movesteps", "next": "turn", "parent": "rep",
                "fields": {}, "inputs": {"STEPS": [1, [4, "100"]]}},
        "turn": {"opcode": "motion_turnright", "next": "inc", "parent": "mv",
                "fields": {}, "inputs": {"DEGREES": [1, [4, "90"]]}},
        "inc": {"opcode": "data_changevariableby", "next": None,
                "parent": "turn",
                "fields": {"VARIABLE": ["score", "score"]},
                "inputs": {"VALUE": [1, [4, "25"]]}},
        "up": {"opcode": "pen_penUp", "next": None, "parent": "rep",
                "fields": {}, "inputs": {}},
        # キー移動スクリプト（インタラクション確認用）
        "keyhat": {"opcode": "event_whenkeypressed", "next": "kmove",
                "fields": {"KEY_OPTION": ["space", None]}, "inputs": {},
                "topLevel": True},
        "kmove": {"opcode": "motion_movesteps", "next": None,
                "parent": "keyhat",
                "fields": {}, "inputs": {"STEPS": [1, [4, "20"]]}},
        "listadd": {"opcode": "data_addtolist", "next": None,
                "parent": "kmove",
                "fields": {"LIST": ["items", "items"]},
                "inputs": {"ITEM": [1, [10, "tick"]]}},
    }
    kmove = sprite_blocks["kmove"]
    kmove["next"] = "listadd"
    return {
        "targets": [
            {"isStage": True, "name": "Stage",
             "variables": {}, "lists": {}, "broadcasts": {}, "blocks": {},
             "costumes": [{"name": "bg", "dataFormat": "svg",
                           "assetId": "bgsvg", "md5ext": "bgsvg.svg",
                           "rotationCenterX": 240, "rotationCenterY": 180}],
             "sounds": [], "currentCostume": 0, "layerOrder": 0,
             "volume": 100},
            {"isStage": False, "name": "Cat",
             "variables": {"score": ["score", 0]},
             "lists": {"items": ["items", ["one", "two"]]},
             "broadcasts": {}, "blocks": sprite_blocks,
             "costumes": [{"name": "cat", "dataFormat": "svg",
                           "assetId": "catsvg", "md5ext": "catsvg.svg",
                           "rotationCenterX": 48, "rotationCenterY": 50}],
             "sounds": [], "currentCostume": 0,
             "x": 0, "y": 50, "size": 100, "direction": 90,
             "rotationStyle": "all around", "visible": True,
             "layerOrder": 1, "volume": 100},
        ],
        "monitors": [
            {"id": "score", "mode": "default", "opcode": "data_variable",
             "params": {"VARIABLE": "score"}, "spriteName": "Cat",
             "targetId": None, "value": 0, "width": 0, "height": 0,
             "x": 8, "y": 8, "visible": True,
             "sliderMin": 0, "sliderMax": 100, "isDiscrete": True},
            {"id": "items", "mode": "list", "opcode": "data_listcontents",
             "params": {"LIST": "items"}, "spriteName": "Cat",
             "targetId": None, "value": "", "width": 0, "height": 0,
             "x": 8, "y": 40, "visible": True,
             "sliderMin": 0, "sliderMax": 100, "isDiscrete": True},
            {"id": "big", "mode": "large", "opcode": "data_variable",
             "params": {"VARIABLE": "score"}, "spriteName": "Cat",
             "targetId": None, "value": 0, "width": 0, "height": 0,
             "x": 350, "y": 8, "visible": True,
             "sliderMin": 0, "sliderMax": 100, "isDiscrete": True},
        ],
        "extensions": ["pen"],
        "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": "t"},
    }

out = sys.argv[1] if len(sys.argv) > 1 else "demo.sb3"
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("project.json", json.dumps(proj()))
    z.writestr("catsvg.svg", CAT_SVG)
    z.writestr("bgsvg.svg", BG_SVG)
print("wrote", out)
