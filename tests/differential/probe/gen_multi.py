#!/usr/bin/env python3
"""Multi-target probe batteries: hat start order, broadcast, sensing_of.

    python3 tests/differential/probe/gen_multi.py

Targets are laid out in the order Stage, S1, S2, S3 (layerOrder 0..3), which is
what scratch-vm iterates when starting hats.  The order in which independent
scripts get to append to the stage list ``out`` is therefore observable.
"""
import json
import os
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from probe_lib import Battery, PROBE_DIR  # noqa: E402
import gen_repros  # noqa: E402

SVG = gen_repros.SVG
SVG_MD5 = gen_repros.SVG_MD5
COSTUME = gen_repros.COSTUME


def _sprite(name, layer, blocks, variables=None, lists=None, x=0, y=0):
    return {"isStage": False, "name": name, "layerOrder": layer,
            "variables": variables or {}, "lists": lists or {},
            "broadcasts": {}, "blocks": blocks, "comments": {},
            "costumes": COSTUME, "sounds": [], "currentCostume": 0,
            "volume": 100, "visible": True, "x": x, "y": y, "size": 100,
            "direction": 90, "draggable": False, "rotationStyle": "all around"}


def _stage(blocks, lists=None, variables=None, broadcasts=None):
    return {"isStage": True, "name": "Stage", "variables": variables or {},
            "lists": lists if lists is not None else {"vout": ["out", []]},
            "broadcasts": broadcasts or {}, "blocks": blocks, "comments": {},
            "costumes": COSTUME, "sounds": [], "currentCostume": 0,
            "volume": 100, "layerOrder": 0}


def write_multi(filename, targets):
    proj = {"targets": targets, "monitors": [], "extensions": [],
            "meta": {"semver": "3.0.0", "vm": "0.2.0", "agent": "xyo-diff"}}
    os.makedirs(PROBE_DIR, exist_ok=True)
    path = os.path.join(PROBE_DIR, filename)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(proj, ensure_ascii=True))
        z.writestr(SVG_MD5 + ".svg", SVG)
    return path


class Spr(Battery):
    """A battery rooted at one target; ``finish()`` returns its block table."""


def sprite(name, layer, body, variables=None, lists=None, x=0, y=0):
    p = Spr()
    body(p)
    return _sprite(name, layer, p.blocks,
                   variables=variables if variables is not None else p.variables,
                   lists=lists if lists is not None else p.lists, x=x, y=y)


# ---------------------------------------------------------------------------
def order_flag():
    """Three sprites with green-flag hats: who appends first?"""
    sprites = []
    for i, name in enumerate(("S1", "S2", "S3")):
        def body(p, name=name):
            p.start()
            p.add_out(p.slit(name))
        sprites.append(sprite(name, i + 1, body))
    write_multi("order_flag.sb3", [_stage({})] + sprites)


def order_broadcast():
    """A broadcast starts three receivers; observe their start order."""
    bcast = {"b1": "go"}
    sprites = []
    for i, name in enumerate(("S1", "S2", "S3")):
        def body(p, name=name):
            hat = p._new("event_whenbroadcastreceived", {},
                         {"BROADCAST_OPTION": ["go", "b1"]}, top=True)
            p._prev = hat
            p.push("data_addtolist", {"ITEM": p.txt(name)},
                   {"LIST": ["out", "vout"]})
        sprites.append(sprite(name, i + 1, body))
    # Stage: broadcast, then broadcast-and-wait a second message, then mark.
    p = Battery()
    p.start()
    p.push("event_broadcast", {"BROADCAST_INPUT": [1, [11, "go", "b1"]]})
    p.push("event_broadcastandwait", {"BROADCAST_INPUT": [1, [11, "go", "b1"]]})
    p.push("data_addtolist", {"ITEM": p.txt("after-wait")},
           {"LIST": ["out", "vout"]})
    stage = _stage(p.blocks, broadcasts=bcast)
    write_multi("order_broadcast.sb3", [stage] + sprites)


def sensing():
    """sensing_of across targets: attributes, variables, missing names."""
    stats = {"vT": ["tv", 42], "vS": ["gv", 99]}
    tlst = {"vL": ["tl", [1, 2]], "vE": ["te", []], "vO": ["t1", ["x"]]}
    tgt = sprite("T", 1, lambda p: p.start(), variables=stats, lists=tlst,
                 x=100, y=50)
    tgt["size"] = 133.5
    tgt["direction"] = -45
    tgt["volume"] = 77
    stage = _stage({}, lists={"vout": ["out", []]},
                   variables={"vg": ["gv", 7]})

    p = Battery()
    p.start()
    V = {"vp0": ["p0", 0]}
    labels = []

    def so(object_name, prop, label):
        menu = p._new("sensing_of_object_menu", {},
                      {"OBJECT": [object_name, None]}, shadow=True)
        b = p._new("sensing_of", {"OBJECT": [1, menu]},
                   {"PROPERTY": [prop, None]})
        p.blocks[menu]["parent"] = b
        vid = "vp%d" % (len(labels) + 1)
        V[vid] = ["p%d" % (len(labels) + 1), 0]
        p.set("p%d" % (len(labels) + 1), vid, [1, b])
        labels.append({"i": len(labels) + 1, "label": label,
                       "var": "p%d" % (len(labels) + 1)})

    for prop in ("x position", "y position", "direction", "costume #",
                 "costume name", "size", "volume", "tv", "tl", "te", "t1",
                 "gv", "TV", "missing", "backdrop #", "backdrop name",
                 "background #", "list contents"):
        so("T", prop, "T." + prop)
    for prop in ("backdrop #", "backdrop name", "volume", "gv", "tv",
                 "x position", "out", "OUT"):
        so("_stage_", prop, "Stage." + prop)
    so("Nope", "x position", "missing target.x")
    so("S", "x position", "self.x")
    p.variables = V
    me = _sprite("S", 2, p.blocks, variables=V)
    write_multi("sensing.sb3", [stage, tgt, me])
    with open(os.path.join(PROBE_DIR, "sensing.json"), "w") as f:
        json.dump(labels, f, ensure_ascii=False, indent=1)


if __name__ == "__main__":
    order_flag()
    order_broadcast()
    sensing()
    print("wrote multi-target probes")
