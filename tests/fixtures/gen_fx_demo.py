#!/usr/bin/env python3
"""完全対応検証用デモ sb3 を生成する。

- 7 スプライト: 各エフェクト（color/fisheye/whirl/pixelate/mosaic/
  brightness/ghost）を静的に適用
- ask and wait（入力バー表示の確認）
- スライダモニタ + ドラッグ可能スプライト
- WAV サウンド（sine）で sound_play / playuntildone の経路確認
"""
import json, math, struct, sys, zipfile

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

def wav_sine(dur=0.25, freq=880, rate=22050):
    """16bit PCM mono sine WAV"""
    n = int(dur * rate)
    frames = b"".join(
        struct.pack("<h", int(16000 * math.sin(2 * math.pi * freq * i / rate)))
        for i in range(n))
    data = b"data" + struct.pack("<I", len(frames)) + frames
    fmt = struct.pack("<HHIIHH", 1, 1, rate, rate * 2, 2, 16)
    return (b"RIFF" + struct.pack("<I", 4 + (8 + len(fmt)) + len(data)) +
            b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + data)

def flag_chain(*blocks):
    """whenflagclicked + 直列ブロック列を id 辞書にして返す"""
    out = {"h": {"opcode": "event_whenflagclicked", "next": "b0",
                 "fields": {}, "inputs": {}, "topLevel": True}}
    for i, (op, fields, inputs, extra) in enumerate(blocks):
        bid = "b%d" % i
        out[bid] = {"opcode": op, "fields": fields, "inputs": inputs,
                    "next": "b%d" % (i + 1) if i + 1 < len(blocks) else None,
                    "parent": "h" if i == 0 else "b%d" % (i - 1)}
        out[bid].update(extra)
    return out

def eff(effect, val):
    return ("looks_seteffectto", {"EFFECT": [effect, None]},
            {"VALUE": [1, [4, str(val)]]}, {})

def say(msg):
    return ("looks_say", {}, {"MESSAGE": [1, [10, msg]]}, {})

SQ_SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="30" height="30">
  <rect width="30" height="30" fill="#cc2244"/>
</svg>'''

def sprite(name, x, y, blocks, **kw):
    costume = kw.get("costume", "cat")
    return {"isStage": False, "name": name,
            "variables": kw.get("variables", {}),
            "lists": {}, "broadcasts": {},
            "blocks": blocks,
            "costumes": [{"name": costume, "dataFormat": "svg",
                          "assetId": costume + "svg",
                          "md5ext": costume + "svg.svg",
                          "rotationCenterX": 48 if costume == "cat" else 15,
                          "rotationCenterY": 50 if costume == "cat" else 15}],
            "sounds": kw.get("sounds", []),
            "currentCostume": 0,
            "x": x, "y": y, "size": kw.get("size", 60), "direction": 90,
            "rotationStyle": "all around", "visible": True,
            "draggable": kw.get("draggable", False),
            "layerOrder": kw.get("layer", 1), "volume": 100}

def proj():
    targets = [{
        "isStage": True, "name": "Stage",
        "variables": {"svar": ["svar", 30]},
        "lists": {}, "broadcasts": {}, "blocks": {},
        "costumes": [{"name": "bg", "dataFormat": "svg", "assetId": "bg",
                      "md5ext": "bg.svg", "rotationCenterX": 240,
                      "rotationCenterY": 180}],
        "sounds": [], "currentCostume": 0, "layerOrder": 0, "volume": 100,
    }]
    # 7 エフェクトスプライト（y=100 の列）
    names = [("color", 80), ("fisheye", 60), ("whirl", 60),
             ("pixelate", 15), ("mosaic", 40), ("brightness", 50),
             ("ghost", 60)]
    for i, (e, v) in enumerate(names):
        targets.append(sprite("fx-%s" % e, -210 + i * 70, 100,
                              flag_chain(eff(e, v)), size=55,
                              layer=i + 1))
    # ask スプライト（質問はバブルで、入力はバー）
    asker = flag_chain(
        say("hi"),
        ("control_wait", {}, {"SECS": [1, [4, "0.2"]]}, {}),
        ("sensing_askandwait", {}, {"QUESTION": [1, [10, "your name?"]]}, {}),
        say("done"),
    )
    targets.append(sprite("asker", -100, -60, asker, layer=9))
    # 音: 0.25s sine WAV → play + playuntildone
    snd = {"name": "beep", "dataFormat": "wav", "assetId": "beepwav",
           "md5ext": "beepwav.wav", "rate": 22050, "sampleCount": 5513,
           "format": ""}
    player = flag_chain(
        ("sound_play", {}, {"SOUND_MENU": [1, "sm"]}, {}),
        ("sound_playuntildone", {}, {"SOUND_MENU": [1, "sm2"]}, {}),
        ("data_setvariableto", {"VARIABLE": ["svar", "svar"]},
         {"VALUE": [1, [4, "77"]]}, {}),
    )
    player["sm"] = {"opcode": "sound_sounds_menu", "parent": "b0",
                    "fields": {"SOUND_MENU": ["beep", None]}, "inputs": {},
                    "shadow": True}
    player["sm2"] = {"opcode": "sound_sounds_menu", "parent": "b1",
                     "fields": {"SOUND_MENU": ["beep", None]}, "inputs": {},
                     "shadow": True}
    # クリックハット検証: クリック（非ドラッグ）で (0,-60) へワープ
    player["ch"] = {"opcode": "event_whenthisspriteclicked", "next": "gt",
                    "fields": {}, "inputs": {}, "topLevel": True}
    player["gt"] = {"opcode": "motion_gotoxy", "parent": "ch",
                    "fields": {},
                    "inputs": {"X": [1, [4, "0"]], "Y": [1, [4, "-60"]]},
                    "next": None}
    targets.append(sprite("player", 120, -60, player, layer=10,
                          sounds=[snd], draggable=True))
    # インジケータ: forever { svar*2-200 を x に設定 }（スライダ検証用）
    ind = {
        "h": {"opcode": "event_whenflagclicked", "next": "f", "fields": {},
              "inputs": {}, "topLevel": True},
        "f": {"opcode": "control_forever", "next": None, "parent": "h",
              "fields": {}, "inputs": {"SUBSTACK": [2, "sx"]}},
        "sx": {"opcode": "motion_setx", "next": None, "parent": "f",
               "fields": {}, "inputs": {"X": [2, "sub"]}},
        "sub": {"opcode": "operator_subtract", "parent": "sx", "fields": {},
                "inputs": {"NUM1": [2, "mul"], "NUM2": [1, [4, "200"]]}},
        "mul": {"opcode": "operator_multiply", "parent": "sub", "fields": {},
                "inputs": {"NUM1": [2, "var"], "NUM2": [1, [4, "2"]]}},
        "var": {"opcode": "data_variable", "parent": "mul",
                "fields": {"VARIABLE": ["svar", "svar"]}, "inputs": {}},
    }
    targets.append(sprite("ind", 0, -150, ind, costume="sq",
                          size=100, layer=11))
    monitors = [{
        "id": "svar", "mode": "slider", "opcode": "data_variable",
        "params": {"VARIABLE": "svar"}, "spriteName": None,
        "targetId": None, "value": 0, "width": 0, "height": 0,
        "x": 8, "y": 8, "visible": True,
        "sliderMin": 0, "sliderMax": 100, "isDiscrete": True,
    }]
    return {"targets": targets, "monitors": monitors,
            "extensions": ["pen"],
            "meta": {"semver": "3.0.0", "vm": "0.0.0", "agent": "t"}}

BG_SVG = '''<svg xmlns="http://www.w3.org/2000/svg" width="480" height="360">
  <rect width="480" height="360" fill="#cfe8ff"/>
</svg>'''

out = sys.argv[1] if len(sys.argv) > 1 else "fx_demo.sb3"
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("project.json", json.dumps(proj()))
    z.writestr("catsvg.svg", CAT_SVG)
    z.writestr("sqsvg.svg", SQ_SVG)
    z.writestr("bg.svg", BG_SVG)
    z.writestr("beepwav.wav", wav_sine())
print("wrote", out)
