#!/usr/bin/env python3
"""Exercise the SAME resumable CFG using LLVM, Builtin WASM, and Binaryen."""
import argparse
import json
import math
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[2]
RESULT = re.compile(r"xyo-common-cfg: completed script=\S+ result=([^\s]+)")


def create_fixture(path, count, value, dynamic, absolute=False,
                   rounded=False, condition=None, broadcast=False, loop=None, nan_compare=False):
    def block(op, nxt=None, parent=None, inputs=None, fields=None, hat=False):
        return dict(opcode=op, next=nxt, parent=parent, inputs=inputs or {},
                    fields=fields or {}, topLevel=hat, shadow=False)
    def variable(vid, name):
        return {"VARIABLE": [name, vid]}
    def num(x):
        return [1, [4, str(x)]]
    blocks = {
        "hat": block("event_whenflagclicked", "set-source", hat=True),
        "set-source": block("data_setvariableto", "set-sum", "hat",
                            {"VALUE": num(value)}, variable("vsource", "source")),
        "set-sum": block("data_setvariableto", "repeat", "set-source",
                         {"VALUE": num(0)}, variable("vsum", "sum")),
        "repeat": block("control_repeat", None, "set-sum",
                        {"TIMES": num(count), "SUBSTACK": [2, "inc"]}),
        "inc": block("data_changevariableby", None, "repeat",
                     {"VALUE": [3, "read-source", [10, ""]]}, variable("vsum", "sum")),
        "read-source": block("data_variable", None, "inc",
                             fields=variable("vsource", "source")),
    }
    if rounded:
        blocks["inc"]["inputs"]["VALUE"] = [3, "rounded", [10, ""]]
        blocks["rounded"] = block(
            "operator_round", None, "inc",
            {"NUM": [3, "read-source", [4, "0"]]})
        blocks["read-source"]["parent"] = "rounded"
    if absolute:
        blocks["inc"]["inputs"]["VALUE"] = [3, "absolute", [10, ""]]
        blocks["absolute"] = block(
            "operator_mathop", None, "inc",
            {"NUM": [3, "read-source", [4, "0"]]},
            {"OPERATOR": [absolute if isinstance(absolute, str) else "abs", None]})
        blocks["read-source"]["parent"] = "absolute"
    if condition:
        blocks["repeat"]["inputs"]["SUBSTACK"] = [2, "if"]
        blocks["if"] = block("control_if", None, "repeat",
                             {"CONDITION": [2, "logic"], "SUBSTACK": [2, "inc"]})
        blocks["inc"]["parent"] = "if"
        def comparison(id_, opcode):
            return block(opcode, None, "logic",
                         {"OPERAND1": [3, id_ + "-get", [4, "0"]],
                          "OPERAND2": [1, [4, "0"]]})
        blocks["positive"] = comparison("positive", "operator_gt")
        blocks["positive-get"] = block("data_variable", None, "positive",
                                        fields=variable("vsource", "source"))
        blocks["negative"] = comparison("negative", "operator_lt")
        blocks["negative-get"] = block("data_variable", None, "negative",
                                        fields=variable("vsource", "source"))
        blocks["not-negative"] = block("operator_not", None, "logic",
                                       {"OPERAND": [2, "negative"]})
        if condition == "not":
            blocks["logic"] = block("operator_not", None, "if",
                                    {"OPERAND": [2, "negative"]})
            blocks["negative"]["parent"] = "logic"
        else:
            blocks["logic"] = block("operator_" + condition, None, "if",
                                    {"OPERAND1": [2, "positive"],
                                     "OPERAND2": [2, "not-negative" if
                                         condition == "and" else "negative"]})
            blocks["positive"]["parent"] = "logic"
            blocks["negative"]["parent"] = (
                "not-negative" if condition == "and" else "logic")
            if condition == "and":
                blocks["not-negative"]["parent"] = "logic"
        if condition != "and":
            blocks.pop("not-negative", None)
    variables = {"vsource": ["source", 0], "vsum": ["sum", 0]}
    if dynamic:
        variables["vcount"] = ["count", 0]
        blocks["set-sum"]["next"] = "set-count"
        blocks["set-count"] = block("data_setvariableto", "repeat", "set-sum",
                                    {"VALUE": num(count)}, variable("vcount", "count"))
        blocks["repeat"]["parent"] = "set-count"
        blocks["repeat"]["inputs"]["TIMES"] = [3, "read-count", [4, "1"]]
        blocks["read-count"] = block("data_variable", None, "repeat",
                                     fields=variable("vcount", "count"))
    if loop:
        blocks["repeat"]["opcode"] = "control_" + loop
        blocks["repeat"]["inputs"].pop("TIMES")
        blocks["repeat"]["inputs"]["CONDITION"] = [2, "loop-condition"]
        blocks["loop-condition"] = block(
            "operator_lt", parent="repeat",
            inputs={"OPERAND1": [3, "sum-read", [4, "0"]],
                    "OPERAND2": num(count * float(value))})
        blocks["sum-read"] = block("data_variable", parent="loop-condition",
                                   fields=variable("vsum", "sum"))
        if loop == "repeat_until":
            blocks["loop-condition"]["parent"] = "loop-not"
            blocks["loop-not"] = block(
                "operator_not", parent="repeat",
                inputs={"OPERAND": [2, "loop-condition"]})
            blocks["repeat"]["inputs"]["CONDITION"] = [2, "loop-not"]
    if nan_compare:
        blocks["set-source"]["inputs"]["VALUE"] = [3, "nan", [4, "0"]]
        blocks["nan"] = block(
            "operator_divide", parent="set-source",
            inputs={"NUM1": num(0), "NUM2": num(0)})
        blocks["inc"]["inputs"]["VALUE"] = num(1)
    common = dict(lists={}, broadcasts={}, comments={}, costumes=[], sounds=[],
                  currentCostume=0, volume=100)
    stage = dict(common, isStage=True, name="Stage", variables={}, blocks={},
                 layerOrder=0, tempo=60, videoTransparency=50, videoState="on")
    sprite = dict(common, isStage=False, name="Bench", variables=variables,
                  blocks=blocks, layerOrder=1, visible=False, x=0, y=0,
                  size=100, direction=90, draggable=False, rotationStyle="all around")
    project = {"targets": [stage, sprite], "monitors": [], "extensions": [],
               "meta": {"semver": "3.0.0", "vm": "0.2.0"}}
    if broadcast:
        blocks["hat"]["opcode"] = "event_whenbroadcastreceived"
        blocks["hat"]["fields"] = {"BROADCAST_OPTION": ["go", "go-id"]}
        stage["broadcasts"] = {"go-id": "go"}
        stage["blocks"] = {
            "sender": block("event_whenflagclicked", "send", hat=True),
            "send": block("event_broadcast", parent="sender",
                          inputs={"BROADCAST_INPUT": [1, [11, "go", "go-id"]]}),
        }
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as out:
        out.writestr("project.json", json.dumps(project))


def run(command, project, mode, addon, frames, interpreted=False, expected_deopt=False):
    env = dict(os.environ, XYO_JIT_COMMON_CFG="1", XYO_JIT_VERBOSE="1",
               XYO_HOT_INVOCATIONS="1", XYO_EXIT_ON_DONE="1",
               XYO_PAR="0", XYO_WASM_HOT_EMITTER=mode, XYO_DUMPVARS="json")
    env.pop("XYO_BINARYEN_JS", None)
    env.pop("XYO_BINARYEN_NODE_ADDON", None)
    if addon:
        env["XYO_BINARYEN_NODE_ADDON"] = str(addon)
        env["XYO_BINARYEN_DIAGNOSTICS"] = "1"
    proc = subprocess.run([*command, "run", str(project), "--jit", "off" if interpreted else "hotspot",
                           "--fps", "30", "--frames", str(frames)],
                          cwd=ROOT, env=env, text=True, capture_output=True,
                          timeout=120)
    matches = RESULT.findall(proc.stderr)
    if proc.returncode or (not interpreted and len(matches) != (0 if expected_deopt else 1)):
        raise AssertionError(f"{mode} CFG failed: {proc.stderr[-1500:]}")
    if len(command) > 1 and ("compiled shared CFG" not in proc.stderr or
                             f"emitter={mode}" not in proc.stderr):
        raise AssertionError(f"{mode} was not compiled: {proc.stderr[-1500:]}")
    if addon and "[binaryen-native] optimized #" not in proc.stderr:
        raise AssertionError(f"Binaryen optimizer did not run: {proc.stderr[-1500:]}")
    if expected_deopt and not interpreted and "xyo-common-cfg: deopt" not in proc.stderr:
        raise AssertionError(f"compiled deopt was not exercised: {proc.stderr[-1500:]}")
    if interpreted and matches:
        raise AssertionError("interpreter unexpectedly ran compiled CFG")
    try:
        state, _ = json.JSONDecoder().raw_decode(
            proc.stdout[proc.stdout.index("{"):])
        value = float(state["Bench"]["sum"])
    except (ValueError, KeyError) as error:
        raise AssertionError(f"{mode}: missing final state: {proc.stdout}") from error
    if not interpreted and not expected_deopt and value != float(matches[0]):
        raise AssertionError(f"{mode}: CFG result differs from committed state")
    return value, state


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--wasm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--node", default="node")
    args = parser.parse_args()
    native, wasm, addon = (p.resolve() for p in
                           (args.native, args.wasm, args.addon))
    for file in (native, wasm, addon):
        if not file.is_file():
            parser.error(f"missing: {file}")
    cases = [(7, "7", False, False), (17, "-3", False, False),
             (23, "1.25", False, False), (31, "0.5", True, False),
             (800, "7", False, False), (0, "9", False, False),
             (-2, "9", True, False), (0.5, "7", False, False),
             (1.5, "-2", True, False), (2.49, "3", True, False),
             (2.5, "3", False, False),
             (4, "-7.5", False, True), (11, "-0.25", True, True),
             (7, "1.5", False, False, True),
             (13, "-1.5", True, False, True),
             (5, "-0.5", False, False, True),
             (9, "-0.5000000000000001", True, False, True),
             (5, "7", False, False, False, "and"),
             (5, "-7", False, False, False, "and"),
             (5, "-7", True, False, False, "or"),
             (5, "0", False, False, False, "not"),
             (5, "-7", False, False, False, "not")]
    cases += [(5, "-1.25", False, "floor"),
              (5, "-1.25", False, "ceiling"),
              (5, "2.25", True, "sqrt"),
              (5, "0", False, "sqrt")]
    cases += [(7, "7", False, False, False, None, True),
              (13, "-2", True, False, False, None, True)]
    cases += [(n, "7", False, False, False, None, False, loop)
              for loop in ("while", "repeat_until") for n in (0, 7)]
    cases += [(5, "0", False, False, False, "or", False, None, True)]
    # Build artifacts are explicitly supplied; temporary SB3 lives alongside
    # them to support sandboxes with a read-only /tmp.
    with tempfile.TemporaryDirectory(dir=wasm.parent) as temp:
        for index, case in enumerate(cases):
            count, value, dynamic, absolute, *extra = case
            rounded = bool(extra and extra[0])
            condition = extra[1] if len(extra) > 1 else None
            broadcast = bool(len(extra) > 2 and extra[2])
            loop = extra[3] if len(extra) > 3 else None
            nan_compare = bool(len(extra) > 4 and extra[4])
            project = Path(temp) / f"cfg-{index}.sb3"
            create_fixture(project, count, value, dynamic,
                           absolute, rounded, condition, broadcast, loop, nan_compare)
            frames = max(0, math.floor(count + 0.5)) + 100
            results = {
                "interpreter": run([str(native)], project, "builtin", None,
                                   frames, interpreted=True, expected_deopt=nan_compare),
                "llvm": run([str(native)], project, "builtin", None, frames, expected_deopt=nan_compare),
                "builtin": run([args.node, str(ROOT / "web/xyo-node.mjs"),
                                str(wasm)], project, "builtin", None, frames, expected_deopt=nan_compare),
                "binaryen": run([args.node, str(ROOT / "web/xyo-node.mjs"),
                                 str(wasm)], project, "binaryen", addon, frames, expected_deopt=nan_compare),
            }
            operand = float(value)
            if rounded:
                operand = math.floor(operand + 0.5)
            elif absolute:
                operand = ({"floor": math.floor, "ceiling": math.ceil,
                            "sqrt": math.sqrt}.get(absolute, abs))(operand)
            if condition:
                positive = float(value) > 0
                negative = float(value) < 0
                passes = (positive and not negative if condition == "and"
                          else positive or negative if condition == "or"
                          else not negative)
                if not passes:
                    operand = 0
            expected = max(0, math.floor(count + 0.5)) * operand
            if nan_compare:
                expected = count
            if any(result[0] != expected for result in results.values()):
                raise AssertionError(f"case {index}: expected {expected}: {results}")
            reference = results["interpreter"][1]
            if any(state != reference for _, state in results.values()):
                raise AssertionError(f"case {index}: final state mismatch: {results}")
            print(f"{index}: {count} * {value}, dynamic={dynamic}, "
                  f"loop={loop or 'repeat'}, broadcast={broadcast}: {results}")
    print(f"PASS: {len(cases)} cases × Interpreter/LLVM/Builtin/Binaryen")


if __name__ == "__main__":
    main()
