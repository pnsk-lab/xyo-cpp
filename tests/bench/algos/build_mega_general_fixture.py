#!/usr/bin/env python3
"""Generate general-coro fixtures (loops, join, broadcast, clone)."""
import argparse, json, sys, zipfile
from pathlib import Path

def blk(opcode, next_id=None, inputs=None, fields=None, top=False, x=0, y=0):
    b = {"opcode": opcode, "next": next_id, "parent": None,
         "inputs": inputs or {}, "fields": fields or {},
         "shadow": False, "topLevel": top}
    if top:
        b["x"], b["y"] = x, y
    return b

def stage(blocks, variables):
    return {"isStage": True, "name": "Stage", "variables": variables,
            "lists": {}, "broadcasts": {}, "blocks": blocks,
            "comments": {}, "currentCostume": 0, "costumes": [],
            "sounds": [], "volume": 100, "layerOrder": 0,
            "tempo": 60, "videoTransparency": 50, "videoState": "on",
            "textToSpeechLanguage": None}

def write(path, targets, assets=None):
    project = {"targets": targets, "monitors": [], "extensions": [],
               "meta": {"semver": "3.0.0", "vm": "0.2.0", "agent": ""}}
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(project))
        for name, data in (assets or {}).items():
            z.writestr(name, data)

def change(var, amount, next_id=None):
    return blk("data_changevariableby", next_id,
               {"VALUE": [1, [4, amount]]},
               {"VARIABLE": [var, var]})

def repeat(count, sub, next_id=None):
    return blk("control_repeat", next_id,
               {"TIMES": [1, [4, count]], "SUBSTACK": [2, sub]})

def hat(next_id):
    return blk("event_whenflagclicked", next_id, top=True)

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument("--out", type=Path, required=True)
out = ap.parse_args().out
out.mkdir(parents=True, exist_ok=True)

# 1. Two writes per iteration (not the narrow single-change proof).
blocks = {
    "hat": hat("rep"),
    "rep": repeat("4", "c1"),
    "c1": change("a", "1", "c2"),
    "c2": change("b", "2"),
}
write(out / "general_loop.sb3",
      [stage(blocks, {"a": ["a", 0], "b": ["b", 0]})])

# 2. String growth across yields (GC stress).
join = blk("operator_join", None,
           {"STRING1": [3, "read", [10, ""]], "STRING2": [1, [10, "x"]]})
blocks = {
    "hat": hat("rep"),
    "rep": repeat("3", "set"),
    "set": blk("data_setvariableto", None,
               {"VALUE": [3, "join", [10, ""]]},
               {"VARIABLE": ["s", "s"]}),
    "join": join,
    "read": blk("data_variable", None, {},
                {"VARIABLE": ["s", "s"]}),
}
write(out / "general_join.sb3",
      [stage(blocks, {"s": ["s", ""]})])

# 3. Broadcast receiver running a loop (receiver must use coro).
blocks = {
    "hat": hat("send"),
    "send": blk("event_broadcast", None,
                {"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
    "recv": {"opcode": "event_whenbroadcastreceived", "next": "rep",
             "parent": None, "inputs": {}, "fields": {
                 "BROADCAST_OPTION": ["PING", "ping"]},
             "shadow": False, "topLevel": True, "x": 0, "y": 0},
    "rep": repeat("3", "inc"),
    "inc": change("a", "1"),
}
project_target = stage(blocks, {"a": ["a", 0]})
project_target["broadcasts"] = {"ping": "PING"}
write(out / "general_broadcast.sb3", [project_target])

# 4. Long string growth (GC stress while the coroutine is suspended).
blocks = {
    "hat": hat("rep"),
    "rep": repeat("5000", "set"),
    "set": blk("data_setvariableto", None,
               {"VALUE": [3, "join", [10, ""]]},
               {"VARIABLE": ["s", "s"]}),
    "join": blk("operator_join", None,
                {"STRING1": [3, "read", [10, ""]],
                 "STRING2": [1, [10, "x"]]}),
    "read": blk("data_variable", None, {}, {"VARIABLE": ["s", "s"]}),
}
write(out / "general_join_stress.sb3", [stage(blocks, {"s": ["s", ""]})])


# 5. Clone-start hat running a loop (clone must use the coro path).
sprite_blocks = {
    "hat": hat("clone"),
    "clone": blk("control_create_clone_of", None,
                 {"CLONE_OPTION": [1, "menu"]}),
    "menu": {**blk("control_create_clone_of_menu", None, {},
                   {"CLONE_OPTION": ["_myself_", None]}), "shadow": True},
    "cloneHat": {**blk("control_start_as_clone", "rep"), "topLevel": True,
                 "x": 0, "y": 0},
    "rep": repeat("3", "inc"),
    "inc": change("a", "1"),
}
stage_target = stage({}, {"a": ["a", 0]})
sprite = {
    "isStage": False, "name": "Sprite1", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": sprite_blocks, "comments": {},
    "currentCostume": 0, "costumes": [], "sounds": [], "volume": 100,
    "layerOrder": 1, "visible": True, "x": 0, "y": 0, "size": 100,
    "direction": 90, "draggable": False, "rotationStyle": "all around",
}
write(out / "general_clone.sb3", [stage_target, sprite])


# 6. Two independent sprites with coroutine loops (parallel scheduling).
def sprite(name, var, layer):
    blocks = {
        "hat": hat("rep"),
        "rep": repeat("50", "inc"),
        "inc": change(var, "1"),
    }
    return {
        "isStage": False, "name": name, "variables": {}, "lists": {},
        "broadcasts": {}, "blocks": blocks, "comments": {},
        "currentCostume": 0, "costumes": [], "sounds": [], "volume": 100,
        "layerOrder": layer, "visible": True, "x": 0, "y": 0,
        "size": 100, "direction": 90, "draggable": False,
        "rotationStyle": "all around",
    }

write(out / "general_parallel.sb3", [
    stage({}, {"a": ["a", 0], "b": ["b", 0]}),
    sprite("Left", "a", 1),
    sprite("Right", "b", 2),
])


def procedure_blocks(name, warp):
    """Green flag -> repeat 2 { call custom block; change b by 1 }."""
    mut = json.dumps({
        "tagName": "mutation", "children": [], "proccode": name,
        "argumentids": "[]", "argumentnames": "[]",
        "argumentdefaults": "[]", "warp": "true" if warp else "false",
    })
    return {
        "hat": hat("outer"),
        "outer": repeat("2", "call"),
        "call": {**blk("procedures_call", "incb"), "mutation": json.loads(mut)},
        "incb": change("b", "1"),
        "def": {**blk("procedures_definition", "rep"), "topLevel": True,
                "x": 0, "y": 300,
                "inputs": {"custom_block": [1, "proto"]}},
        "proto": {**blk("procedures_prototype", None, {}), "shadow": True,
                  "mutation": json.loads(mut)},
        "rep": repeat("4", "inc"),
        "inc": change("a", "1", "inc2"),
        "inc2": change("b", "2"),
    }


for warp in (False, True):
    suffix = "warp" if warp else "plain"
    write(out / f"general_procedure_{suffix}.sb3",
          [stage(procedure_blocks("doLoop", warp),
                 {"a": ["a", 0], "b": ["b", 0]})])


# 7. wait inside a loop: an opaque C++ suspension must not lower.
blocks = {
    "hat": hat("rep"),
    "rep": repeat("2", "wait"),
    "wait": blk("control_wait", "inc",
                {"DURATION": [1, [4, "0.01"]]}),
    "inc": change("a", "1"),
}
write(out / "general_wait.sb3", [stage(blocks, {"a": ["a", 0]})])


# 8. wait until: the wait comes from a conditional yield, not a helper.
timer = blk("sensing_timer", None)
cond = blk("operator_gt", None,
           {"OPERAND1": [3, "timer", [10, ""]],
            "OPERAND2": [1, [4, "0.001"]]})
blocks = {
    "hat": hat("rep"),
    "rep": repeat("2", "wu"),
    "wu": blk("control_wait_until", "inc",
              {"CONDITION": [2, "cond"]}),
    "inc": change("a", "1"),
    "cond": cond,
    "timer": timer,
}
write(out / "general_wait_until.sb3", [stage(blocks, {"a": ["a", 0]})])

# 9. wait inside a non-warp procedure (fused into the coroutine frame).
mut = json.dumps({
    "tagName": "mutation", "children": [], "proccode": "waitLoop",
    "argumentids": "[]", "argumentnames": "[]",
    "argumentdefaults": "[]", "warp": "false",
})
blocks = {
    "hat": hat("call"),
    "call": {**blk("procedures_call", None), "mutation": json.loads(mut)},
    "def": {**blk("procedures_definition", "rep"), "topLevel": True,
            "x": 0, "y": 300,
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None, {}), "shadow": True,
              "mutation": json.loads(mut)},
    "rep": repeat("2", "wait"),
    "wait": blk("control_wait", "inc", {"DURATION": [1, [4, "0.001"]]}),
    "inc": change("a", "1"),
}
write(out / "general_procedure_wait.sb3", [stage(blocks, {"a": ["a", 0]})])


# 10. broadcast-and-wait: the sender suspends until the receiver finishes.
blocks = {
    "sendHat": hat("bcast"),
    "bcast": blk("event_broadcastandwait", "tail",
                 {"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
    "tail": change("b", "1"),
    "recvHat": {"opcode": "event_whenbroadcastreceived", "next": "recvWait",
                "parent": None, "inputs": {},
                "fields": {"BROADCAST_OPTION": ["PING", "ping"]},
                "shadow": False, "topLevel": True, "x": 0, "y": 200},
    "recvWait": blk("control_wait", "recvInc",
                    {"DURATION": [1, [4, "0.005"]]}),
    "recvInc": change("a", "1"),
}
project = stage(blocks, {"a": ["a", 0], "b": ["b", 0]})
project["broadcasts"] = {"ping": "PING"}
write(out / "general_bcastwait.sb3", [project])


# 11. broadcast-and-wait with no receivers: must not suspend forever.
blocks = {
    "sendHat": hat("bcast"),
    "bcast": blk("event_broadcastandwait", "tail",
                 {"BROADCAST_INPUT": [1, [11, "NONE", "none"]]}),
    "tail": change("a", "1"),
}
project = stage(blocks, {"a": ["a", 0]})
project["broadcasts"] = {"none": "NONE"}
write(out / "general_bcastwait_none.sb3", [project])

# 12. broadcast-and-wait inside a loop.
blocks = {
    "sendHat": hat("rep"),
    "rep": repeat("3", "bcast"),
    "bcast": blk("event_broadcastandwait", "tail",
                 {"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
    "tail": change("b", "1"),
    "recvHat": {"opcode": "event_whenbroadcastreceived", "next": "recvInc",
                "parent": None, "inputs": {},
                "fields": {"BROADCAST_OPTION": ["PING", "ping"]},
                "shadow": False, "topLevel": True, "x": 0, "y": 200},
    "recvInc": change("a", "1"),
}
project = stage(blocks, {"a": ["a", 0], "b": ["b", 0]})
project["broadcasts"] = {"ping": "PING"}
write(out / "general_bcastwait_loop.sb3", [project])


# 13. wait until a *variable* (normally the event-dependency fast path).
blocks = {
    "sendHat": hat("send"),
    "send": blk("event_broadcast", "wait",
                {"BROADCAST_INPUT": [1, [11, "GO", "go"]]}),
    "wait": blk("control_wait", "setFlag", {"DURATION": [1, [4, "0.01"]]}),
    "setFlag": blk("data_setvariableto", None,
                   {"VALUE": [1, [4, "1"]]}, {"VARIABLE": ["flag", "flag"]}),
    "recvHat": {"opcode": "event_whenbroadcastreceived", "next": "spin",
                "parent": None, "inputs": {},
                "fields": {"BROADCAST_OPTION": ["GO", "go"]},
                "shadow": False, "topLevel": True, "x": 0, "y": 200},
    "spin": blk("control_wait_until", "inc",
                {"CONDITION": [2, "cond"]}),
    "inc": change("a", "1"),
    "cond": blk("operator_gt", None,
                {"OPERAND1": [3, "read", [10, ""]],
                 "OPERAND2": [1, [4, "0"]]}),
    "read": blk("data_variable", None, {}, {"VARIABLE": ["flag", "flag"]}),
}
project = stage(blocks, {"a": ["a", 0], "flag": ["flag", 0]})
project["broadcasts"] = {"go": "GO"}
write(out / "general_wait_until_var.sb3", [project])


# 14. glide: a time-based loop with per-frame position writes.
# Motion runs on a sprite (the Stage has no position).
glide_blocks = {
    "hat": hat("glide"),
    "glide": blk("motion_glidesecstoxy", "set",
                 {"SECS": [1, [4, "0.02"]], "X": [1, [4, "100"]],
                  "Y": [1, [4, "0"]]}),
    "set": blk("data_setvariableto", None,
               {"VALUE": [3, "xpos", [10, ""]]},
               {"VARIABLE": ["a", "a"]}),
    "xpos": blk("motion_xposition", None),
}
glide_sprite = {
    "isStage": False, "name": "Glider", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": glide_blocks, "comments": {},
    "currentCostume": 0, "costumes": [], "sounds": [], "volume": 100,
    "layerOrder": 1, "visible": True, "x": 0, "y": 0, "size": 100,
    "direction": 90, "draggable": False, "rotationStyle": "all around",
}
write(out / "general_glide.sb3",
      [stage({}, {"a": ["a", 0]}), glide_sprite])

# 15. say for secs: bubble + WaitEntry, then continue.
blocks = {
    "hat": hat("say"),
    "say": blk("looks_sayforsecs", "inc",
               {"MESSAGE": [1, [10, "hi"]], "SECS": [1, [4, "0.01"]]}),
    "inc": change("a", "1"),
}
write(out / "general_saysecs.sb3", [stage(blocks, {"a": ["a", 0]})])



# 18. switch backdrop and wait (no matching receiver: completes at once).
bd_menu = {**blk("looks_backdrops", None, {}, {"BACKDROP": ["b1", None]}),
           "shadow": True}
blocks = {
    "hat": hat("wait"),
    "wait": blk("looks_switchbackdroptoandwait", "inc",
                {"BACKDROP": [1, "menu"]}),
    "menu": bd_menu,
    "inc": change("a", "1"),
}
write(out / "general_backdropwait.sb3", [stage(blocks, {"a": ["a", 0]})])



# 19. stop this script inside a loop (top level): every loop yield is
# unreachable, so this exercises the zero-suspend coroutine path.
stop_this = {**blk("control_stop", None, {}, {"STOP_OPTION": ["this script", None]})}
blocks = {
    "hat": hat("rep"),
    "rep": repeat("3", "inc"),
    "inc": change("a", "1", "stop"),
    "stop": stop_this,
}
write(out / "general_stop.sb3", [stage(blocks, {"a": ["a", 0]})])


# 20. stop this script after a yielding loop: the loop yields are real,
# the stop then ends the script immediately.
stop_after = {**blk("control_stop", None, {}, {"STOP_OPTION": ["this script", None]})}
blocks = {
    "hat": hat("rep"),
    "rep": repeat("2", "inc"),
    "inc": change("a", "1"),
    "tail": change("a", "10"),
}
# The stop is emitted after the loop (unreachable tail after it).
blocks["rep"]["next"] = "stop"
blocks["stop"] = stop_after
write(out / "general_stop_after_loop.sb3", [stage(blocks, {"a": ["a", 0]})])


# 21. stop all (no reachable yield: ordinary entry, stopAll retires self).
stop_all = {**blk("control_stop", None, {}, {"STOP_OPTION": ["all", None]})}
blocks = {
    "hat": hat("inc"),
    "inc": change("a", "1"),
    "stop": stop_all,
    "tail": change("a", "10"),
}
blocks["inc"]["next"] = "stop"
blocks["stop"]["next"] = "tail"
write(out / "general_stop_all.sb3", [stage(blocks, {"a": ["a", 0]})])

# 22. stop all after a yielding loop.
blocks = {
    "hat": hat("rep"),
    "rep": repeat("2", "inc"),
    "inc": change("a", "1"),
    "stop": stop_all,
    "tail": change("a", "10"),
}
blocks["rep"]["next"] = "stop"
blocks["stop"]["next"] = "tail"
write(out / "general_stop_all_after_loop.sb3",
      [stage(blocks, {"a": ["a", 0]})])


# 23. stop this script inside a custom block: Scratch unwinds the whole
# script, so the tail after the call must not run.
proc_body = {**blk("control_stop", None, {}, {"STOP_OPTION": ["this script", None]})}
proc_blocks = {
    "def": {"opcode": "procedures_definition", "next": "pstop",
            "parent": None,
            "inputs": {"custom_block": [1, "proto"]},
            "fields": {}, "shadow": False, "topLevel": True, "x": 300, "y": 0},
    "proto": {"opcode": "procedures_prototype", "next": None,
              "parent": "def", "inputs": {},
              "fields": {}, "shadow": True,
              "mutation": {"tagName": "mutation",
                           "children": [], "proccode": "stop in proc",
                           "argumentids": "[]", "argumentnames": "[]",
                           "argumentdefaults": "[]", "warp": "false"}},
    "pstop": proc_body,
}
blocks = {
    "hat": hat("call"),
    "call": blk("procedures_call", "tail", {},
                {"mutation": {"tagName": "mutation", "children": [],
                              "proccode": "stop in proc",
                              "argumentids": "[]", "warp": "false"}}),
    "tail": change("a", "10"),
}
blocks.update(proc_blocks)
write(out / "general_stop_in_proc.sb3",
      [stage(blocks, {"a": ["a", 0]})])


# 24. a warp procedure with a long loop: the procedure must stay tight
# (yield only on the watchdog budget), and the tail after it still runs.
warp_proc = {
    "def": {"opcode": "procedures_definition", "next": "p_rep",
            "parent": None, "inputs": {"custom_block": [1, "proto"]},
            "fields": {}, "shadow": False, "topLevel": True, "x": 300, "y": 0},
    "proto": {"opcode": "procedures_prototype", "next": None,
              "parent": "def", "inputs": {}, "fields": {}, "shadow": True,
              "mutation": {"tagName": "mutation", "children": [],
                           "proccode": "warp loop", "argumentids": "[]",
                           "argumentnames": "[]", "argumentdefaults": "[]",
                           "warp": "true"}},
    "p_rep": repeat("1000", "p_inc"),
    "p_inc": change("a", "1"),
}
blocks = {
    "hat": hat("call"),
    "call": {**blk("procedures_call", "tail", {}),
             "mutation": {"tagName": "mutation", "children": [],
                          "proccode": "warp loop",
                          "argumentids": "[]", "warp": "true"}},
    "tail": change("a", "1"),
}
blocks.update(warp_proc)
write(out / "general_procedure_warp_loop.sb3",
      [stage(blocks, {"a": ["a", 0]})])


# 25. an empty warp repeat: the inline empty-loop scheduler is skipped for
# coroutine candidates and the generic warp-aware loop is used instead.
empty_rep = repeat("5000", "")
empty_rep.pop("next", None)
warp_empty = {
    "def": {"opcode": "procedures_definition", "next": "p_empty",
            "parent": None, "inputs": {"custom_block": [1, "proto"]},
            "fields": {}, "shadow": False, "topLevel": True, "x": 300, "y": 0},
    "proto": {"opcode": "procedures_prototype", "next": None,
              "parent": "def", "inputs": {}, "fields": {}, "shadow": True,
              "mutation": {"tagName": "mutation", "children": [],
                           "proccode": "warp empty", "argumentids": "[]",
                           "argumentnames": "[]", "argumentdefaults": "[]",
                           "warp": "true"}},
    "p_empty": empty_rep,
}
blocks = {
    "hat": hat("call"),
    "call": {**blk("procedures_call", "tail", {}),
             "mutation": {"tagName": "mutation", "children": [],
                          "proccode": "warp empty",
                          "argumentids": "[]", "warp": "true"}},
    "tail": change("a", "1"),
}
blocks.update(warp_empty)
write(out / "general_procedure_warp_empty.sb3",
      [stage(blocks, {"a": ["a", 0]})])


# 26. recursive non-yielding procedure: it cannot be fused into one
# function, but the sync dispatcher may keep it as an out-of-line call.
rec_proc = {
    "def": {"opcode": "procedures_definition", "next": "p_if",
            "parent": None, "inputs": {"custom_block": [1, "proto"]},
            "fields": {}, "shadow": False, "topLevel": True, "x": 300, "y": 0},
    "proto": {"opcode": "procedures_prototype", "next": None,
              "parent": "def", "inputs": {}, "fields": {}, "shadow": True,
              "mutation": {"tagName": "mutation", "children": [],
                           "proccode": "recurse", "argumentids": "[]",
                           "argumentnames": "[]", "argumentdefaults": "[]",
                           "warp": "false"}},
    "p_if": blk("control_if", None,
                {"CONDITION": [2, "p_cond"], "SUBSTACK": [2, "p_body"]}),
    "p_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "p_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "p_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "p_body": change("a", "-1", "p_call"),
    "p_call": {**blk("procedures_call", None),
               "mutation": {"tagName": "mutation", "children": [],
                            "proccode": "recurse",
                            "argumentids": "[]", "warp": "false"}},
}
rec_call = {**blk("procedures_call", "setb"),
            "mutation": {"tagName": "mutation", "children": [],
                         "proccode": "recurse",
                         "argumentids": "[]", "warp": "false"}}
blocks = {
    "hat": hat("seta"),
    "seta": blk("data_setvariableto", "call",
                {"VALUE": [1, [4, "3"]]}, {"VARIABLE": ["a", "a"]}),
    "call": rec_call,
    "setb": blk("data_setvariableto", None,
                {"VALUE": [3, "b_read", [10, ""]]},
                {"VARIABLE": ["b", "b"]}),
    "b_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "hat2": blk("event_whenflagclicked", "incc", top=True, x=0, y=300),
    "incc": change("c", "1"),
}
blocks.update(rec_proc)
write(out / "general_recursive.sb3",
      [stage(blocks, {"a": ["a", 0], "b": ["b", 0], "c": ["c", 0]})])


# 27. sound effect/volume setters are not suspension points; a loop with
# them must still lower to a coroutine.
blocks = {
    "hat": hat("rep"),
    "rep": repeat("3", "inc"),
    "inc": change("a", "1", "vol"),
    "vol": blk("sound_setvolumeto", "eff", {"VOLUME": [1, [4, "50"]]}),
    "eff": blk("sound_seteffectto", "tail",
               {"VALUE": [1, [4, "10"]]}, {"EFFECT": ["PITCH", None]}),
    "tail": change("a", "1"),
}
write(out / "general_sound_effects.sb3", [stage(blocks, {"a": ["a", 0]})])

# 28. delete this clone (clone hat): the loop runs in the clone, then the
# thread ends before the tail.
clone_blocks = {
    "hat": {"opcode": "control_start_as_clone", "next": "rep",
            "parent": None, "inputs": {}, "fields": {}, "shadow": False,
            "topLevel": True, "x": 0, "y": 0},
    "rep": repeat("5", "inc"),
    "inc": change("a", "1"),
    "del": blk("control_delete_this_clone", "tail"),
    "tail": change("a", "10"),
    "flag": hat("create"),
    "create": blk("control_create_clone_of", None, {"CLONE_OPTION": [1, "menu"]}),
    "menu": {**blk("control_create_clone_of_menu", None, {},
                   {"CLONE_OPTION": ["_myself_", None]}), "shadow": True},
}
clone_blocks["rep"]["next"] = "del"
clone_sprite = {
    "isStage": False, "name": "Cloner", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": clone_blocks, "comments": {},
    "currentCostume": 0, "costumes": [], "sounds": [], "volume": 100,
    "layerOrder": 1, "visible": True, "x": 0, "y": 0, "size": 100,
    "direction": 90, "draggable": False, "rotationStyle": "all around",
}
write(out / "general_deleteclone.sb3",
      [stage({}, {"a": ["a", 0]}), clone_sprite])


# 29. stop all inside a custom block: the caller must unwind after the call.
sp = {"tagName": "mutation", "children": [], "proccode": "stop in proc all",
      "argumentids": "[]", "argumentnames": "[]",
      "argumentdefaults": "[]", "warp": "false"}
stop_all = {**blk("control_stop", None, {}, {"STOP_OPTION": ["all", None]})}
blocks = {
    "hat": hat("call"),
    "call": {**blk("procedures_call", "tail"), "mutation": json.loads(json.dumps(sp))},
    "tail": change("a", "10"),
    "def": {**blk("procedures_definition", "rep", top=True, x=300),
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None), "shadow": True,
              "mutation": json.loads(json.dumps(sp))},
    "rep": repeat("2", "inc"),
    "inc": change("a", "1", "stop"),
    "stop": stop_all,
}
write(out / "general_stop_all_in_proc.sb3", [stage(blocks, {"a": ["a", 0]})])

# 30. delete this clone inside a custom block (clone hat).
sp2 = {"tagName": "mutation", "children": [], "proccode": "delete in proc",
       "argumentids": "[]", "argumentnames": "[]",
       "argumentdefaults": "[]", "warp": "false"}
clone_blocks = {
    "hat": {"opcode": "control_start_as_clone", "next": "rep",
            "parent": None, "inputs": {}, "fields": {}, "shadow": False,
            "topLevel": True, "x": 0, "y": 0},
    "rep": repeat("5", "inc"),
    "inc": change("a", "1"),
    "call": {**blk("procedures_call", "tail"),
             "mutation": json.loads(json.dumps(sp2))},
    "tail": change("a", "10"),
    "def": {**blk("procedures_definition", "del", top=True, x=300),
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None), "shadow": True,
              "mutation": json.loads(json.dumps(sp2))},
    "del": blk("control_delete_this_clone", None),
    "flag": hat("create"),
    "create": blk("control_create_clone_of", None, {"CLONE_OPTION": [1, "menu"]}),
    "menu": {**blk("control_create_clone_of_menu", None, {},
                   {"CLONE_OPTION": ["_myself_", None]}), "shadow": True},
}
clone_blocks["rep"]["next"] = "call"
clone_sprite = {
    "isStage": False, "name": "ProcCloner", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": clone_blocks, "comments": {},
    "currentCostume": 0, "costumes": [], "sounds": [], "volume": 100,
    "layerOrder": 1, "visible": True, "x": 0, "y": 0, "size": 100,
    "direction": 90, "draggable": False, "rotationStyle": "all around",
}
write(out / "general_deleteclone_in_proc.sb3",
      [stage({}, {"a": ["a", 0]}), clone_sprite])


# 31. warp procedure + broadcast-and-wait inside its loop.
sp3 = {"tagName": "mutation", "children": [], "proccode": "warp ping",
       "argumentids": "[]", "argumentnames": "[]",
       "argumentdefaults": "[]", "warp": "true"}
blocks = {
    "hat": hat("call"),
    "call": {**blk("procedures_call", "tail"), "mutation": json.loads(json.dumps(sp3))},
    "tail": change("a", "100"),
    "def": {**blk("procedures_definition", "rep", top=True, x=300),
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None), "shadow": True,
              "mutation": json.loads(json.dumps(sp3))},
    "rep": repeat("3", "inc"),
    "inc": change("a", "1", "bw"),
    "bw": blk("event_broadcastandwait", None, {"BROADCAST_INPUT": [1, "bwid"]}),
    "bwid": {"opcode": "event_broadcast_menu", "next": None, "parent": "bw",
             "inputs": {}, "fields": {"BROADCAST_OPTION": ["PING", "ping"]},
             "shadow": True, "topLevel": False},
    "recv": {"opcode": "event_whenbroadcastreceived", "next": "binc",
             "parent": None, "inputs": {},
             "fields": {"BROADCAST_OPTION": ["PING", "ping"]},
             "shadow": False, "topLevel": True, "x": 0, "y": 300},
    "binc": change("b", "1"),
}
proj = stage(blocks, {"a": ["a", 0], "b": ["b", 0]})
proj["broadcasts"] = {"ping": "PING"}
write(out / "general_procedure_warp_bcastwait.sb3", [proj])

# 32. clone stops its own script (clone itself must survive).
clone_blocks = {
    "hat": {"opcode": "control_start_as_clone", "next": "rep",
            "parent": None, "inputs": {}, "fields": {}, "shadow": False,
            "topLevel": True, "x": 0, "y": 0},
    "rep": repeat("5", "inc"),
    "inc": change("a", "1"),
    "stop": blk("control_stop", "tail", {}, {"STOP_OPTION": ["this script", None]}),
    "tail": change("a", "100"),
    "flag": hat("create"),
    "create": blk("control_create_clone_of", "after", {"CLONE_OPTION": [1, "menu"]}),
    "after": change("a", "1"),
    "menu": {**blk("control_create_clone_of_menu", None, {},
                   {"CLONE_OPTION": ["_myself_", None]}), "shadow": True},
}
clone_blocks["rep"]["next"] = "stop"
clone_sprite = {
    "isStage": False, "name": "StopCloner", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": clone_blocks, "comments": {},
    "currentCostume": 0, "costumes": [], "sounds": [], "volume": 100,
    "layerOrder": 1, "visible": True, "x": 0, "y": 0, "size": 100,
    "direction": 90, "draggable": False, "rotationStyle": "all around",
}
write(out / "general_clone_stopscript.sb3",
      [stage({}, {"a": ["a", 0]}), clone_sprite])

# 33. stop all two procedure levels deep.
sp4 = {"tagName": "mutation", "children": [], "proccode": "outer",
       "argumentids": "[]", "argumentnames": "[]",
       "argumentdefaults": "[]", "warp": "false"}
sp5 = {"tagName": "mutation", "children": [], "proccode": "inner",
       "argumentids": "[]", "argumentnames": "[]",
       "argumentdefaults": "[]", "warp": "false"}
blocks = {
    "hat": hat("call"),
    "call": {**blk("procedures_call", "tail"), "mutation": json.loads(json.dumps(sp4))},
    "tail": change("a", "100"),
    "def": {**blk("procedures_definition", "call2", top=True, x=300),
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None), "shadow": True,
              "mutation": json.loads(json.dumps(sp4))},
    "call2": {**blk("procedures_call", "tail2"), "mutation": json.loads(json.dumps(sp5))},
    "tail2": change("a", "10"),
    "def2": {**blk("procedures_definition", "rep2", top=True, x=600),
              "inputs": {"custom_block": [1, "proto2"]}},
    "proto2": {**blk("procedures_prototype", None), "shadow": True,
               "mutation": json.loads(json.dumps(sp5))},
    "rep2": repeat("2", "inc2"),
    "inc2": change("a", "1", "stop2"),
    "stop2": blk("control_stop", None, {}, {"STOP_OPTION": ["all", None]}),
}
write(out / "general_stop_all_nested_proc.sb3", [stage(blocks, {"a": ["a", 0]})])


# 34. recursive WARP procedure with an argument and no loops: frames are
# elided and the recursion never suspends, so the sync dispatcher can call
# the procedure function natively (out-of-line, yield-free).
mut = json.dumps({
    "tagName": "mutation", "children": [], "proccode": "sum %s",
    "argumentids": json.dumps(["argN"]), "argumentnames": json.dumps(["n"]),
    "argumentdefaults": json.dumps(["0"]), "warp": "true",
})
def arg_rep(name="n"):
    return {**blk("argument_reporter_string_number", None, {},
                  {"VALUE": [name, None]}), "shadow": False}
blocks = {
    "hat": hat("seta"),
    "seta": blk("data_setvariableto", "call",
                {"VALUE": [1, [4, "0"]]}, {"VARIABLE": ["a", "a"]}),
    "call": {**blk("procedures_call", "setb"),
             "inputs": {"argN": [1, [4, "5"]]},
             "mutation": json.loads(mut)},
    "setb": blk("data_setvariableto", None,
                {"VALUE": [3, "b_read", [10, ""]]},
                {"VARIABLE": ["b", "b"]}),
    "b_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "def": {**blk("procedures_definition", "p_if", top=True, x=300),
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None, {}, {"argN": [1, "proto_arg"]}),
              "shadow": True, "mutation": json.loads(mut)},
    "proto_arg": {**blk("argument_reporter_string_number", None, {},
                        {"VALUE": ["n", None]}), "shadow": True,
                  "parent": "proto"},
    "p_if": blk("control_if", None,
                {"CONDITION": [2, "p_cond"], "SUBSTACK": [2, "p_add"]}),
    "p_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "p_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "p_read": arg_rep(),
    "p_add": blk("data_changevariableby", "p_call",
                 {"VALUE": [3, "p_read2", [10, ""]]},
                 {"VARIABLE": ["a", "a"]}),
    "p_read2": arg_rep(),
    "p_call": {**blk("procedures_call", None),
               "inputs": {"argN": [3, "p_dec", [10, ""]]},
               "mutation": json.loads(mut)},
    "p_dec": blk("operator_subtract", None,
                 {"NUM1": [3, "p_read3", [10, ""]], "NUM2": [1, [4, "1"]]}),
    "p_read3": arg_rep(),
    "hat2": blk("event_whenflagclicked", "incc", top=True, x=0, y=400),
    "incc": change("c", "1"),
}
write(out / "general_recursive_warp.sb3",
      [stage(blocks, {"a": ["a", 0], "b": ["b", 0], "c": ["c", 0]})])


def warp_proc_def(proccode, body_head, proto_arg=False):
    mut = json.dumps({
        "tagName": "mutation", "children": [], "proccode": proccode,
        "argumentids": json.dumps(["argN"] if proto_arg else []),
        "argumentnames": json.dumps(["n"] if proto_arg else []),
        "argumentdefaults": json.dumps(["0"] if proto_arg else []),
        "warp": "true",
    })
    return mut

# 35. mutual recursion between two yield-free warp procedures.
m_f = warp_proc_def("f %s", None, True)
m_g = warp_proc_def("g %s", None, True)
def pdef(mut, body, proto, x):
    # Each definition needs its own block id; `proto` is already unique.
    return {
        "def_" + proto: {**blk("procedures_definition", body, top=True, x=x),
                         "inputs": {"custom_block": [1, proto]}},
        proto: {**blk("procedures_prototype", None, {}, {"argN": [1, proto + "_a"]}),
                "shadow": True, "mutation": json.loads(mut)},
        proto + "_a": {**blk("argument_reporter_string_number", None, {},
                             {"VALUE": ["n", None]}), "shadow": True,
                       "parent": proto},
    }
blocks = {
    "hat": hat("seta"),
    "seta": blk("data_setvariableto", "callf",
                {"VALUE": [1, [4, "0"]]}, {"VARIABLE": ["a", "a"]}),
    "callf": {**blk("procedures_call", "setb"),
              "inputs": {"argN": [1, [4, "5"]]},
              "mutation": json.loads(m_f)},
    "setb": blk("data_setvariableto", None,
                {"VALUE": [3, "b_read", [10, ""]]},
                {"VARIABLE": ["b", "b"]}),
    "b_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    # f(n): if n > 0 { change a by n; call g(n-1) }
    "f_if": blk("control_if", None,
                {"CONDITION": [2, "f_cond"], "SUBSTACK": [2, "f_add"]}),
    "f_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "f_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "f_read": arg_rep(),
    "f_add": blk("data_changevariableby", "f_call",
                 {"VALUE": [3, "f_read2", [10, ""]]}, {"VARIABLE": ["a", "a"]}),
    "f_read2": arg_rep(),
    "f_call": {**blk("procedures_call", None),
               "inputs": {"argN": [3, "f_dec", [10, ""]]},
               "mutation": json.loads(m_g)},
    "f_dec": blk("operator_subtract", None,
                 {"NUM1": [3, "f_read3", [10, ""]], "NUM2": [1, [4, "1"]]}),
    "f_read3": arg_rep(),
    # g(n): if n > 0 { change a by n; call f(n-1) }
    "g_if": blk("control_if", None,
                {"CONDITION": [2, "g_cond"], "SUBSTACK": [2, "g_add"]}),
    "g_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "g_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "g_read": arg_rep(),
    "g_add": blk("data_changevariableby", "g_call",
                 {"VALUE": [3, "g_read2", [10, ""]]}, {"VARIABLE": ["a", "a"]}),
    "g_read2": arg_rep(),
    "g_call": {**blk("procedures_call", None),
               "inputs": {"argN": [3, "g_dec", [10, ""]]},
               "mutation": json.loads(m_f)},
    "g_dec": blk("operator_subtract", None,
                 {"NUM1": [3, "g_read3", [10, ""]], "NUM2": [1, [4, "1"]]}),
    "g_read3": arg_rep(),
    "hat2": blk("event_whenflagclicked", "incc", top=True, x=0, y=500),
    "incc": change("c", "1"),
}
blocks.update(pdef(m_f, "f_if", "protof", 300))
blocks.update(pdef(m_g, "g_if", "protog", 600))
write(out / "general_recursive_mutual.sb3",
      [stage(blocks, {"a": ["a", 0], "b": ["b", 0], "c": ["c", 0]})])

# 36. recursion with stop all in the base case (batch must unwind).
m_sa = json.dumps({
    "tagName": "mutation", "children": [], "proccode": "down %s",
    "argumentids": json.dumps(["argN"]), "argumentnames": json.dumps(["n"]),
    "argumentdefaults": json.dumps(["0"]), "warp": "true",
})
blocks = {
    "hat": hat("seta"),
    "seta": blk("data_setvariableto", "call",
                {"VALUE": [1, [4, "0"]]}, {"VARIABLE": ["a", "a"]}),
    "call": {**blk("procedures_call", "tail"),
             "inputs": {"argN": [1, [4, "3"]]},
             "mutation": json.loads(m_sa)},
    "tail": change("a", "100"),
    "def": {**blk("procedures_definition", "p_if", top=True, x=300),
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None, {}, {"argN": [1, "proto_a"]}),
              "shadow": True, "mutation": json.loads(m_sa)},
    "proto_a": {**blk("argument_reporter_string_number", None, {},
                      {"VALUE": ["n", None]}), "shadow": True,
                "parent": "proto"},
    "p_if": blk("control_if_else", None,
                {"CONDITION": [2, "p_cond"], "SUBSTACK": [2, "p_add"],
                 "SUBSTACK2": [2, "p_stop"]}),
    "p_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "p_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "p_read": arg_rep(),
    "p_add": blk("data_changevariableby", "p_call",
                 {"VALUE": [1, [4, "1"]]}, {"VARIABLE": ["a", "a"]}),
    "p_call": {**blk("procedures_call", None),
               "inputs": {"argN": [3, "p_dec", [10, ""]]},
               "mutation": json.loads(m_sa)},
    "p_dec": blk("operator_subtract", None,
                 {"NUM1": [3, "p_read2", [10, ""]], "NUM2": [1, [4, "1"]]}),
    "p_read2": arg_rep(),
    "p_stop": blk("control_stop", None, {}, {"STOP_OPTION": ["all", None]}),
    "hat2": blk("event_whenflagclicked", "incc", top=True, x=0, y=400),
    "incc": change("c", "1"),
}
write(out / "general_recursive_stopall.sb3",
      [stage(blocks, {"a": ["a", 0], "c": ["c", 0]})])

# 37. recursion with stop this script in the base case.
m_ss = json.loads(m_sa)
m_ss["proccode"] = "down ss %s"
blocks = dict(blocks)
blocks = {k: v for k, v in blocks.items()}
blocks["def"]["next"] = "p_if"
blocks["p_stop"] = blk("control_stop", None, {},
                       {"STOP_OPTION": ["this script", None]})
blocks["call"] = {**blocks["call"], "mutation": json.loads(json.dumps(m_ss))}
blocks["p_call"] = {**blocks["p_call"], "mutation": json.loads(json.dumps(m_ss))}
blocks["proto"]["mutation"] = json.loads(json.dumps(m_ss))
write(out / "general_recursive_stopscript.sb3",
      [stage(blocks, {"a": ["a", 0], "c": ["c", 0]})])


# 38. recursive WARP procedure that also contains a loop. The loop yield is
# a cooperative warp-budget check, so the sync batch may run it on a worker.
m_rl = json.dumps({
    "tagName": "mutation", "children": [], "proccode": "sumloop %s",
    "argumentids": json.dumps(["argN"]), "argumentnames": json.dumps(["n"]),
    "argumentdefaults": json.dumps(["0"]), "warp": "true",
})
blocks = {
    "hat": hat("seta"),
    "seta": blk("data_setvariableto", "call",
                {"VALUE": [1, [4, "0"]]}, {"VARIABLE": ["a", "a"]}),
    "call": {**blk("procedures_call", "setb"),
             "inputs": {"argN": [1, [4, "5"]]},
             "mutation": json.loads(m_rl)},
    "setb": blk("data_setvariableto", None,
                {"VALUE": [3, "b_read", [10, ""]]},
                {"VARIABLE": ["b", "b"]}),
    "b_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "def": {**blk("procedures_definition", "p_if", top=True, x=300),
            "inputs": {"custom_block": [1, "proto"]}},
    "proto": {**blk("procedures_prototype", None, {}, {"argN": [1, "proto_a"]}),
              "shadow": True, "mutation": json.loads(m_rl)},
    "proto_a": {**blk("argument_reporter_string_number", None, {},
                      {"VALUE": ["n", None]}), "shadow": True,
                "parent": "proto"},
    "p_if": blk("control_if", None,
                {"CONDITION": [2, "p_cond"], "SUBSTACK": [2, "p_rep"]}),
    "p_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "p_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "p_read": arg_rep(),
    "p_rep": blk("control_repeat", "p_call",
                 {"TIMES": [3, "p_read2", [10, ""]], "SUBSTACK": [2, "p_inc"]}),
    "p_read2": arg_rep(),
    "p_inc": blk("data_changevariableby", None,
                 {"VALUE": [1, [4, "1"]]}, {"VARIABLE": ["a", "a"]}),
    "p_call": {**blk("procedures_call", None),
               "inputs": {"argN": [3, "p_dec", [10, ""]]},
               "mutation": json.loads(m_rl)},
    "p_dec": blk("operator_subtract", None,
                 {"NUM1": [3, "p_read3", [10, ""]], "NUM2": [1, [4, "1"]]}),
    "p_read3": arg_rep(),
    "hat2": blk("event_whenflagclicked", "incc", top=True, x=0, y=500),
    "incc": change("c", "1"),
}
write(out / "general_recursive_warp_loop.sb3",
      [stage(blocks, {"a": ["a", 0], "b": ["b", 0], "c": ["c", 0]})])


# 39. switch backdrop and wait with a REAL receiver: the sender must wait
# for event_whenbackdropswitchesto to finish (coro begin/step join).
SVG = b'<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"></svg>'
bd_menu2 = {**blk("looks_backdrops", None, {}, {"BACKDROP": ["b2", None]}),
            "shadow": True}
blocks = {
    "hat": hat("wait"),
    "wait": blk("looks_switchbackdroptoandwait", "inc",
                {"BACKDROP": [1, "menu2"]}),
    "menu2": bd_menu2,
    "inc": change("a", "1"),
    "recv": {**blk("event_whenbackdropswitchesto", "rinc", top=True),
             "fields": {"BACKDROP": ["b2", None]}},
    "rinc": change("b", "1"),
}
proj = stage(blocks, {"a": ["a", 0], "b": ["b", 0]})
proj["costumes"] = [
    {"name": "b1", "md5ext": "aaa.svg", "assetId": "aaa", "dataFormat": "svg",
     "rotationCenterX": 0, "rotationCenterY": 0},
    {"name": "b2", "md5ext": "bbb.svg", "assetId": "bbb", "dataFormat": "svg",
     "rotationCenterX": 0, "rotationCenterY": 0},
]
write(out / "general_backdrop_receiver.sb3", [proj],
      {"aaa.svg": SVG, "bbb.svg": SVG})


# 40. nested procedures with a wait in the innermost one: the coroutine
# lowering must fuse both bodies and suspend inside the inner frame.
mut_a = json.dumps({
    "tagName": "mutation", "children": [], "proccode": "outer",
    "argumentids": "[]", "argumentnames": "[]",
    "argumentdefaults": "[]", "warp": "false",
})
mut_b = json.dumps({
    "tagName": "mutation", "children": [], "proccode": "inner wait",
    "argumentids": "[]", "argumentnames": "[]",
    "argumentdefaults": "[]", "warp": "false",
})
blocks = {
    "hat": hat("call_a"),
    "call_a": {**blk("procedures_call", "tail"), "mutation": json.loads(mut_a)},
    "tail": change("b", "1"),
    "def_a": {**blk("procedures_definition", "rep_a", top=True, x=300),
              "inputs": {"custom_block": [1, "proto_a"]}},
    "proto_a": {**blk("procedures_prototype", None), "shadow": True,
                "mutation": json.loads(mut_a)},
    "rep_a": repeat("2", "call_b"),
    "call_b": {**blk("procedures_call", None), "mutation": json.loads(mut_b)},
    "def_b": {**blk("procedures_definition", "wait_b", top=True, x=600),
              "inputs": {"custom_block": [1, "proto_b"]}},
    "proto_b": {**blk("procedures_prototype", None), "shadow": True,
                "mutation": json.loads(mut_b)},
    "wait_b": blk("control_wait", "inc_b", {"DURATION": [1, [4, "0.001"]]}),
    "inc_b": change("a", "1"),
}
write(out / "general_procedure_wait_nested.sb3",
      [stage(blocks, {"a": ["a", 0], "b": ["b", 0]})])


# 41. broadcast-and-wait with THREE receivers: the sender joins all of them
# on every round (multi-thread continuation).
blocks = {
    "hat": hat("send"),
    "send": blk("event_broadcastandwait", "sinc",
                {"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
    "sinc": change("s", "1", "rep"),
    "rep": repeat("4", "bcast"),
    "bcast": blk("event_broadcastandwait", None,
                 {"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
}
for k in range(3):
    blocks[f"r{k}"] = {"opcode": "event_whenbroadcastreceived", "next": f"rr{k}",
                       "parent": None, "inputs": {},
                       "fields": {"BROADCAST_OPTION": ["PING", "ping"]},
                       "shadow": False, "topLevel": True, "x": 0, "y": 100 * (k + 1)}
    blocks[f"rr{k}"] = repeat("3", f"rw{k}")
    blocks[f"rw{k}"] = blk("control_wait", f"ri{k}",
                           {"DURATION": [1, [4, "0.001"]]})
    blocks[f"ri{k}"] = change("a", "1")
proj = stage(blocks, {"a": ["a", 0], "s": ["s", 0]})
proj["broadcasts"] = {"ping": "PING"}
write(out / "general_bcast_storm.sb3", [proj])

# 42. four clones each waiting in a loop: concurrent continuations with
# clones (each clone owns its thread and JitCtx).
clone_blocks = {
    "hat": {"opcode": "control_start_as_clone", "next": "rep",
            "parent": None, "inputs": {}, "fields": {}, "shadow": False,
            "topLevel": True, "x": 0, "y": 0},
    "rep": repeat("5", "wait"),
    "wait": blk("control_wait", "inc", {"DURATION": [1, [4, "0.001"]]}),
    "inc": change("a", "1"),
    "flag": hat("create"),
    "create": blk("control_create_clone_of", "again", {"CLONE_OPTION": [1, "menu"]}),
    "again": blk("control_create_clone_of", "again2", {"CLONE_OPTION": [1, "menu"]}),
    "again2": blk("control_create_clone_of", "binc", {"CLONE_OPTION": [1, "menu"]}),
    "binc": change("b", "1"),
    "menu": {**blk("control_create_clone_of_menu", None, {},
                   {"CLONE_OPTION": ["_myself_", None]}), "shadow": True},
}
clone_sprite = {
    "isStage": False, "name": "WaitCloner", "variables": {}, "lists": {},
    "broadcasts": {}, "blocks": clone_blocks, "comments": {},
    "currentCostume": 0, "costumes": [], "sounds": [], "volume": 100,
    "layerOrder": 1, "visible": True, "x": 0, "y": 0, "size": 100,
    "direction": 90, "draggable": False, "rotationStyle": "all around",
}
write(out / "general_clone_wait.sb3",
      [stage({}, {"a": ["a", 0], "b": ["b", 0]}), clone_sprite])


# 43. play sound until done (no audio device: resolves immediately) in a
# coroutine; the begin helper must still suspend once and continue.
import struct
samples = 800
wav = (b"RIFF" + struct.pack("<I", 36 + samples * 2) + b"WAVEfmt " +
       struct.pack("<IHHIIHH", 16, 1, 1, 22050, 44100, 2, 16) +
       b"data" + struct.pack("<I", samples * 2) + b"\x00\x00" * samples)
blocks = {
    "hat": hat("play"),
    "play": {**blk("sound_playuntildone", "inc", {"SOUND_MENU": [1, "snd"]})},
    "snd": {"opcode": "sounds_menu", "next": None, "parent": "play",
            "inputs": {}, "fields": {"SOUND": ["Blip", None]},
            "shadow": True, "topLevel": False},
    "inc": change("a", "1"),
}
proj = stage(blocks, {"a": ["a", 0]})
proj["sounds"] = [{"name": "Blip", "md5ext": "blip.wav", "assetId": "blip",
                   "dataFormat": "wav", "rate": 22050, "sampleCount": samples}]
write(out / "general_playuntildone.sb3", [proj], {"blip.wav": wav})


# -- non-warp (yielding) recursion on the coroutine path ----------------
def nonwarp_def(proccode):
    return json.dumps({
        "tagName": "mutation", "children": [], "proccode": proccode,
        "argumentids": "[]", "argumentnames": "[]",
        "argumentdefaults": "[]", "warp": "false",
    })

# 44. mutual non-warp recursion: f -> g -> f, every call yields.
m_f = nonwarp_def("f")
m_g = nonwarp_def("g")
blocks = {
    "hat": hat("seta"),
    "seta": blk("data_setvariableto", "callf",
                {"VALUE": [1, [4, "3"]]}, {"VARIABLE": ["a", "a"]}),
    "callf": {**blk("procedures_call", "setb"), "mutation": json.loads(m_f)},
    "setb": blk("data_setvariableto", None,
                {"VALUE": [3, "b_read", [10, ""]]}, {"VARIABLE": ["b", "b"]}),
    "b_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "f_if": blk("control_if", None,
                {"CONDITION": [2, "f_cond"], "SUBSTACK": [2, "f_inc"]}),
    "f_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "f_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "f_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "f_inc": change("a", "-1", "f_call"),
    "f_call": {**blk("procedures_call", None), "mutation": json.loads(m_g)},
    "g_if": blk("control_if", None,
                {"CONDITION": [2, "g_cond"], "SUBSTACK": [2, "g_inc"]}),
    "g_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "g_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "g_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "g_inc": change("a", "-1", "g_call"),
    "g_call": {**blk("procedures_call", None), "mutation": json.loads(m_f)},
    "hat2": blk("event_whenflagclicked", "incc", top=True, x=0, y=300),
    "incc": change("c", "1"),
}
def nw_pdef(mut, body, proto, x):
    return {
        "def_" + proto: {**blk("procedures_definition", body, top=True, x=x),
                         "inputs": {"custom_block": [1, proto]}},
        proto: {**blk("procedures_prototype", None, {}, {}),
                "shadow": True, "mutation": json.loads(mut)},
    }
blocks.update(nw_pdef(m_f, "f_if", "pfn", 300))
blocks.update(nw_pdef(m_g, "g_if", "pgn", 600))
write(out / "general_recursive_mutual_nonwarp.sb3",
      [stage(blocks, {"a": ["a", 0], "b": ["b", 0], "c": ["c", 0]})])

# 45. non-warp recursion whose body also has a yielding loop.
blocks = {
    "hat": hat("seta"),
    "seta": blk("data_setvariableto", "callp",
                {"VALUE": [1, [4, "3"]]}, {"VARIABLE": ["a", "a"]}),
    "callp": {**blk("procedures_call", None), "mutation": json.loads(
        nonwarp_def("p"))},
    "p_if": blk("control_if", None,
                {"CONDITION": [2, "p_cond"], "SUBSTACK": [2, "p_rep"]}),
    "p_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "p_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "p_read": blk("data_variable", None, {}, {"VARIABLE": ["a", "a"]}),
    "p_rep": repeat("2", "p_inc"),
    "p_inc": blk("data_changevariableby", None,
                 {"VALUE": [1, [4, "1"]]}, {"VARIABLE": ["b", "b"]}),
    "p_dec": change("a", "-1", "p_call"),
    "p_call": {**blk("procedures_call", None), "mutation": json.loads(
        nonwarp_def("p"))},
}
blocks["p_rep"]["next"] = "p_dec"
blocks.update(nw_pdef(nonwarp_def("p"), "p_if", "ppn", 300))
proj = stage(blocks, {"a": ["a", 0], "b": ["b", 0]})
write(out / "general_recursive_loop_nonwarp.sb3", [proj])

# 46. deep non-warp recursion building strings (GC roots in nested frames).
blocks = {
    "hat": hat("setn"),
    "setn": blk("data_setvariableto", "callp",
                {"VALUE": [1, [4, "3000"]]}, {"VARIABLE": ["n", "n"]}),
    "callp": {**blk("procedures_call", "setlen"), "mutation": json.loads(
        nonwarp_def("build"))},
    "setlen": blk("data_setvariableto", None,
                  {"VALUE": [3, "len", [10, ""]]}, {"VARIABLE": ["a", "a"]}),
    "len": blk("operator_length", None,
               {"STRING": [3, "s_read", [10, ""]]}),
    "s_read": blk("data_variable", None, {}, {"VARIABLE": ["s", "s"]}),
    "b_if": blk("control_if", None,
                {"CONDITION": [2, "b_cond"], "SUBSTACK": [2, "b_add"]}),
    "b_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "n_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "n_read": blk("data_variable", None, {}, {"VARIABLE": ["n", "n"]}),
    "b_add": blk("data_setvariableto", "b_dec",
                 {"VALUE": [3, "join", [10, ""]]}, {"VARIABLE": ["s", "s"]}),
    "join": blk("operator_join", None,
                {"STRING1": [3, "s_read2", [10, ""]],
                 "STRING2": [1, [10, "x"]]}),
    "s_read2": blk("data_variable", None, {}, {"VARIABLE": ["s", "s"]}),
    "b_dec": change("n", "-1", "b_call"),
    "b_call": {**blk("procedures_call", None), "mutation": json.loads(
        nonwarp_def("build"))},
}
blocks.update(nw_pdef(nonwarp_def("build"), "b_if", "pbn", 300))
write(out / "general_recursive_strings.sb3",
      [stage(blocks, {"n": ["n", 0], "s": ["s", ""], "a": ["a", 0]})])


# 47. stop all while unwinding non-warp recursion (n is decremented before
# the recursive call, so the recursion terminates).
blocks = {
    "hat": hat("setn"),
    "setn": blk("data_setvariableto", "callp",
                {"VALUE": [1, [4, "10"]]}, {"VARIABLE": ["n", "n"]}),
    "callp": {**blk("procedures_call", None), "mutation": json.loads(
        nonwarp_def("stoprec"))},
    "s_if": blk("control_if", None,
                {"CONDITION": [2, "s_cond"], "SUBSTACK": [2, "s_dec"]}),
    "s_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "s_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "s_read": blk("data_variable", None, {}, {"VARIABLE": ["n", "n"]}),
    # Decrement before recursing, then stop all while unwinding.
    "s_dec": change("n", "-1", "s_call"),
    "s_call": {**blk("procedures_call", "s_inc"), "mutation": json.loads(
        nonwarp_def("stoprec"))},
    "s_inc": blk("data_changevariableby", "s_stop",
                 {"VALUE": [1, [4, "1"]]}, {"VARIABLE": ["a", "a"]}),
    "s_stop": {**blk("control_stop", None),
               "fields": {"STOP_OPTION": ["all", None]},
               "mutation": {"tagName": "mutation", "children": [],
                            "hasnext": "false"}},
}
blocks.update(nw_pdef(nonwarp_def("stoprec"), "s_if", "psn", 300))
write(out / "general_recursive_stopall_nonwarp.sb3",
      [stage(blocks, {"n": ["n", 0], "a": ["a", 0]})])


# 48. `stop this script` inside non-warp recursion: it only returns from the
# current procedure invocation, so the callers continue (a=3, b=1).
blocks = {
    "hat": hat("setn"),
    "setn": blk("data_setvariableto", "callp",
                {"VALUE": [1, [4, "3"]]}, {"VARIABLE": ["n", "n"]}),
    "callp": {**blk("procedures_call", "after"),
              "mutation": json.loads(nonwarp_def("stopscript"))},
    "after": blk("data_setvariableto", None,
                 {"VALUE": [1, [4, "1"]]}, {"VARIABLE": ["b", "b"]}),
    "t_if": blk("control_if_else", None,
                {"CONDITION": [2, "t_cond"], "SUBSTACK": [2, "t_dec"],
                 "SUBSTACK2": [2, "t_stop"]}),
    "t_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "t_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "t_read": blk("data_variable", None, {}, {"VARIABLE": ["n", "n"]}),
    "t_dec": change("n", "-1", "t_call"),
    "t_call": {**blk("procedures_call", "t_inc"),
               "mutation": json.loads(nonwarp_def("stopscript"))},
    "t_inc": change("a", "1", None),
    "t_stop": {**blk("control_stop", None),
               "fields": {"STOP_OPTION": ["this script", None]},
               "mutation": {"tagName": "mutation", "children": [],
                            "hasnext": "false"}},
}
blocks.update(nw_pdef(nonwarp_def("stopscript"), "t_if", "ptx", 300))
write(out / "general_recursive_stopscript_nonwarp.sb3",
      [stage(blocks, {"n": ["n", 0], "a": ["a", 0], "b": ["b", 0]})])


# 49. broadcast and wait inside non-warp recursion (nested suspension).
blocks = {
    "hat": hat("setn"),
    "setn": blk("data_setvariableto", "callp",
                {"VALUE": [1, [4, "3"]]}, {"VARIABLE": ["n", "n"]}),
    "callp": {**blk("procedures_call", None),
              "mutation": json.loads(nonwarp_def("bcastrec"))},
    "recv": {**blk("event_whenbroadcastreceived", "recv_inc", top=True),
             "fields": {"BROADCAST_OPTION": ["go", "go"]}},
    "recv_inc": change("a", "1", None),
    "w_if": blk("control_if", None,
                {"CONDITION": [2, "w_cond"], "SUBSTACK": [2, "w_dec"]}),
    "w_cond": blk("operator_gt", None,
                  {"OPERAND1": [3, "w_read", [10, ""]],
                   "OPERAND2": [1, [4, "0"]]}),
    "w_read": blk("data_variable", None, {}, {"VARIABLE": ["n", "n"]}),
    "w_dec": change("n", "-1", "w_bcast"),
    "w_bcast": blk("event_broadcastandwait", "w_call",
                   {"BROADCAST_INPUT": [1, [11, "go", "go"]]}),
    "w_call": {**blk("procedures_call", None),
               "mutation": json.loads(nonwarp_def("bcastrec"))},
}
blocks.update(nw_pdef(nonwarp_def("bcastrec"), "w_if", "pwx", 300))
t = stage(blocks, {"n": ["n", 0], "a": ["a", 0]})
t["broadcasts"] = {"go": "go"}
write(out / "general_recursive_bcastwait_nonwarp.sb3", [t])


def warp_def(proccode):
    return json.dumps({
        "tagName": "mutation", "children": [], "proccode": proccode,
        "argumentids": "[]", "argumentnames": "[]",
        "argumentdefaults": "[]", "warp": "true",
    })

# 50. warp recursion with a real yield (wait): the warp-budget loop yields
# are cooperative and tagged, so the wait drives the coroutine suspension.
blocks = {
    "hat": hat("setn"),
    "setn": blk("data_setvariableto", "callp",
                {"VALUE": [1, [4, "5"]]}, {"VARIABLE": ["n", "n"]}),
    "callp": {**blk("procedures_call", None), "mutation": json.loads(
        warp_def("wp"))},
    "pw_if": blk("control_if", None,
                 {"CONDITION": [2, "pw_cond"], "SUBSTACK": [2, "pw_dec"]}),
    "pw_cond": blk("operator_gt", None,
                   {"OPERAND1": [3, "pw_read", [10, ""]],
                    "OPERAND2": [1, [4, "0"]]}),
    "pw_read": blk("data_variable", None, {}, {"VARIABLE": ["n", "n"]}),
    "pw_dec": change("n", "-1", "pw_wait"),
    "pw_wait": blk("control_wait", "pw_call", {"DURATION": [1, [4, "0.01"]]}),
    "pw_call": {**blk("procedures_call", None), "mutation": json.loads(
        warp_def("wp"))},
}
blocks.update(nw_pdef(warp_def("wp"), "pw_if", "pwp", 300))
write(out / "general_warp_rec_wait.sb3",
      [stage(blocks, {"n": ["n", 0]})])

print("wrote", out)


