#!/usr/bin/env python3
"""Differential test of one shared Scratch Call/Return CFG on LLVM and WASM."""
from __future__ import annotations

import argparse
import json
import zipfile
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))

from gen_algo_bench import (
    Gen, S_add, S_call, S_change, S_repeat, S_set, S_if, S_ifelse, gt,
    add, arg, lt, S_until, make_project, num, text, var, write_sb3,
)

BOOLEAN_CASES = {
    "boolean_arg", "boolean_arg_false", "boolean_arg_assignment",
    "nonwarp_loop_bool_arg", "nonwarp_loop_bool_arg_deopt",
}

SUPPORTED = {
    "scalar", "parameter", "two_parameters", "snapshot", "nested_procedure",
    "nonwarp", "nonwarp_parameter", "nonwarp_snapshot", "nonwarp_nested",
    "three_parameters", "many_parameters", "repeated_helper", "repeated_snapshot",
    "conditional", "conditional_false", "nonwarp_conditional",
    "boolean_arg", "boolean_arg_false", "nonwarp_loop", "nonwarp_loop_deopt",
    "nonwarp_loop_arg", "nonwarp_loop_arg_deopt",
    "nonwarp_loop_bool_arg", "nonwarp_loop_bool_arg_deopt",
    "nonwarp_loop_nested_args", "nonwarp_loop_nested_args_deopt",
    "nonwarp_loop_arg_bound", "nonwarp_loop_arg_bound_deopt",
    "nonwarp_loop_branch", "nonwarp_loop_branch_deopt",
    "nonwarp_loop_nested_repeat", "nonwarp_loop_nested_repeat_deopt",
    "nonwarp_loop_distinct_calls", "nonwarp_loop_distinct_calls_deopt",
    "nonwarp_loop_shared_nested", "nonwarp_loop_shared_nested_deopt",
    "nonwarp_nested_shared_call", "nonwarp_nested_shared_call_deopt",
    "nonwarp_nested_shared_call_late_deopt",
    "nonwarp_until_arg", "nonwarp_until_arg_deopt",
    "nonwarp_while_arg", "nonwarp_while_arg_deopt",
    "nonwarp_until_nested", "nonwarp_until_nested_deopt",
    "nonwarp_while_nested", "nonwarp_while_nested_deopt",
}
CASES = (
    "scalar", "parameter", "two_parameters", "snapshot", "nested_procedure",
    "side_effect", "nonwarp", "nested_loop", "three_parameters",
    "non_numeric_arg", "repeated_helper",
    "recursive", "multiple_waits", "type_switch",
    "nonwarp_parameter", "nonwarp_snapshot", "nonwarp_nested",
    "many_parameters", "too_many_parameters", "repeated_snapshot",
    "conditional", "conditional_false", "nonwarp_conditional",
    "boolean_arg", "boolean_arg_false", "boolean_arg_assignment", "nonwarp_loop", "nonwarp_loop_deopt",
    "nonwarp_loop_arg", "nonwarp_loop_arg_deopt",
    "nonwarp_loop_bool_arg", "nonwarp_loop_bool_arg_deopt",
    "nonwarp_loop_nested_args", "nonwarp_loop_nested_args_deopt",
    "nonwarp_loop_arg_bound", "nonwarp_loop_arg_bound_deopt",
    "nonwarp_loop_branch", "nonwarp_loop_branch_deopt",
    "nonwarp_loop_nested_repeat", "nonwarp_loop_nested_repeat_deopt",
    "nonwarp_loop_distinct_calls", "nonwarp_loop_distinct_calls_deopt",
    "nonwarp_loop_shared_nested", "nonwarp_loop_shared_nested_deopt",
    "nonwarp_nested_shared_call", "nonwarp_nested_shared_call_deopt",
    "nonwarp_nested_shared_call_late_deopt",
    "nonwarp_until_arg", "nonwarp_until_arg_deopt",
    "nonwarp_while_arg", "nonwarp_while_arg_deopt",
    "nonwarp_until_nested", "nonwarp_until_nested_deopt",
    "nonwarp_while_nested", "nonwarp_while_nested_deopt",
)


def fixture(directory: Path, case: str) -> Path:
    gen = Gen(["x", "y"], ["effects"])
    if case in ("nested_procedure", "nonwarp_nested"):
        gen.define("inner %s", ["v"], [
            S_change("y", arg("v")),
        ], warp=case != "nonwarp_nested")
        names = ["a"]
        body = [
            S_call("inner %s", [add(arg("a"), num(1))]),
            S_change("x", num(1)),
        ]
    elif case in ("nonwarp_loop_nested_args", "nonwarp_loop_nested_args_deopt"):
        gen.define("inner %s", ["v"], [
            S_repeat(num(2), [S_change("x", arg("v"))]),
            S_change("y", arg("v")),
        ], warp=False)
        names = ["a"]
        body = [
            S_call("inner %s", [arg("a")]),
            S_change("y", num(1)),
        ]
    elif case in ("nonwarp_nested_shared_call", "nonwarp_nested_shared_call_deopt",
                 "nonwarp_nested_shared_call_late_deopt"):
        # Same inner Scratch call source ID appears under two distinct
        # inlined outer calls. The SSA argument frames must not alias.
        gen.define("inner %s", ["v"], [
            S_repeat(num(2), [S_change("y", arg("v"))]),
        ], warp=False)
        names = ["a"]
        body = [S_call("inner %s", [arg("a")]),
                S_change("y", num(1))]
    elif case in ("nonwarp_until_nested", "nonwarp_until_nested_deopt",
                 "nonwarp_while_nested", "nonwarp_while_nested_deopt"):
        condition = (lt(var("y"), num(3)) if case.startswith("nonwarp_while")
                     else gt(var("y"), num(2)))
        gen.define("inner %s", ["v"], [
            S_until(condition, [
                S_change("x", arg("v")),
                S_change("y", num(1)),
            ]),
        ], warp=False)
        names = ["a"]
        body = [S_repeat(num(2), [
            S_call("inner %s", [arg("a")]),
        ])]
    elif case in ("nonwarp_until_arg", "nonwarp_until_arg_deopt",
                 "nonwarp_while_arg", "nonwarp_while_arg_deopt"):
        names = ["a"]
        condition = (lt(var("y"), num(3)) if case.startswith("nonwarp_while")
                     else gt(var("y"), num(2)))
        body = [S_until(condition, [
            S_change("x", arg("a")),
            S_change("y", num(1)),
        ]), S_change("x", num(0))]
    elif case in ("nonwarp_loop_arg_bound", "nonwarp_loop_arg_bound_deopt"):
        names = ["a"]
        body = [S_repeat(arg("a"), [S_change("x", num(2))]),
                S_change("y", arg("a"))]
    elif case in ("nonwarp_loop_branch", "nonwarp_loop_branch_deopt"):
        names = ["a"]
        body = [S_if(gt(arg("a"), num(0)),
                     [S_repeat(num(2), [S_change("x", arg("a"))])]),
                S_change("y", arg("a"))]
    elif case in ("nonwarp_loop_nested_repeat", "nonwarp_loop_nested_repeat_deopt"):
        names = ["a"]
        body = [S_repeat(num(2), [
            S_repeat(num(3), [S_change("x", arg("a"))])]),
            S_change("y", arg("a"))]
    elif case in ("nonwarp_loop_distinct_calls", "nonwarp_loop_distinct_calls_deopt"):
        names = ["a"]
        body = [S_repeat(num(2), [S_change("y", arg("a"))])]
    elif case in ("nonwarp_loop_shared_nested", "nonwarp_loop_shared_nested_deopt"):
        names = ["a"]
        body = [S_repeat(num(2),
            [S_repeat(num(2), [S_change("y", arg("a"))])])]
    elif case in ("nonwarp_loop_bool_arg", "nonwarp_loop_bool_arg_deopt"):
        names = ["a"]
        body = [S_repeat(num(2), [
            S_ifelse(arg("a"), [S_change("x", num(2))],
                     [S_change("y", num(2))]),
        ])]
    elif case == "two_parameters":
        names = ["a", "b"]
        body = [
            S_change("x", arg("a")),
            S_change("y", arg("b")),
            S_change("y", arg("a")),
        ]
    elif case == "boolean_arg_assignment":
        names = ["a"]
        body = [S_set("x", arg("a"))]
    elif case in ("boolean_arg", "boolean_arg_false"):
        names = ["a"]
        body = [S_ifelse(arg("a"), [S_change("x", num(1))],
                        [S_change("y", num(1))])]
    elif case in ("conditional", "conditional_false", "nonwarp_conditional"):
        names = ["a"]
        body = [S_ifelse(gt(arg("a"), num(0)),
                        [S_change("x", arg("a"))],
                        [S_change("y", num(1))])]
    elif case in ("many_parameters", "too_many_parameters"):
        names = [f"arg{i}" for i in range(32 if case == "many_parameters" else 33)]
        body = [S_change("x", arg(names[0])),
                S_change("y", arg(names[-1]))]
    elif case == "three_parameters":
        names = ["a", "b", "c"]
        body = [S_change("x", arg("a"))]
    elif case in ("parameter", "snapshot", "non_numeric_arg",
                  "nonwarp_parameter", "nonwarp_snapshot", "repeated_snapshot",
                  "nonwarp_loop_arg", "nonwarp_loop_arg_deopt"):
        names = ["a"]
        body = [
            S_change("x", arg("a")),
            S_change("x", num(1)),
            S_change("y", arg("a")),
        ]
    else:
        names = []
        body = [
            S_change("x", num(2)),
            S_change("y", add(var("x"), num(1))),
        ]

    if case == "recursive":
        body = [
            S_change("x", num(1)),
            S_if(gt(num(3), var("x")), [S_call("update", [])]),
            S_add("effects", var("x")),
        ]
    if case == "type_switch":
        body = [S_set("x", text("hello")), S_add("effects", var("x")),
                S_set("x", num(7)), S_add("effects", var("x"))]
    if case in ("side_effect", "multiple_waits"):
        body.append(S_add("effects", num(1)))
    if case in ("nested_loop", "nonwarp_loop", "nonwarp_loop_deopt"):
        body.append(S_repeat(num(2), [S_change("x", num(3))]))

    if case in ("nonwarp_loop_arg", "nonwarp_loop_arg_deopt"):
        body.append(S_repeat(num(2), [S_change("x", arg("a"))]))
    code = "update" + " %s" * len(names)
    if case in BOOLEAN_CASES:
        code = "update %b"
    gen.define(code, names, body, warp=not case.startswith("nonwarp") and case not in ("recursive", "multiple_waits"))
    values = {
        "two_parameters": [num(1), num(2)],
        "three_parameters": [num(1), num(2), num(3)],
        "snapshot": [var("x")],
        "boolean_arg": [num(0)],
        "boolean_arg_assignment": [num(0)],
        "boolean_arg_false": [num(0)],
        "conditional_false": [num(-1)],
        "nonwarp_snapshot": [var("x")],
        "nonwarp_loop_arg": [var("x")],
        "nonwarp_loop_arg_deopt": [var("x")],
        "nonwarp_loop_nested_args": [var("x")],
        "nonwarp_loop_nested_args_deopt": [var("x")],
        "nonwarp_loop_arg_bound": [num(3)],
        "nonwarp_loop_arg_bound_deopt": [num(3)],
        "nonwarp_loop_branch": [num(2)],
        "nonwarp_loop_branch_deopt": [num(2)],
        "nonwarp_loop_nested_repeat": [num(2)],
        "nonwarp_loop_nested_repeat_deopt": [num(2)],
        "nonwarp_until_arg": [num(2)],
        "nonwarp_until_arg_deopt": [num(2)],
        "nonwarp_while_arg": [num(2)],
        "nonwarp_while_arg_deopt": [num(2)],
        "nonwarp_until_nested": [num(2)],
        "nonwarp_until_nested_deopt": [num(2)],
        "nonwarp_while_nested": [num(2)],
        "nonwarp_while_nested_deopt": [num(2)],
        "repeated_snapshot": [var("x")],
        "non_numeric_arg": [text("5")],
    }.get(case, [num(1)] if names else [])
    if case in ("many_parameters", "too_many_parameters"):
        values = [num(i + 1) for i in range(len(names))]
    calls = ([S_call(code, [num(2)]), S_call(code, [num(5)])]
             if case.startswith("nonwarp_nested_shared_call")
             else [S_call(code, [var("x")]),
              S_call(code, [num(3) if case.startswith("nonwarp_loop_shared_nested")
                           else num(2)])]
             if case.startswith(("nonwarp_loop_distinct_calls",
                                 "nonwarp_loop_shared_nested"))
             else [S_call(code, values)])
    if case in ("repeated_helper", "repeated_snapshot"):
        calls.append(S_call(code, values))
    gen.script([
        S_set("x", num(2 if case.startswith(("nonwarp_loop_arg", "nonwarp_loop_nested_args",
                                            "nonwarp_loop_distinct_calls",
                                            "nonwarp_loop_shared_nested")) else 0)),
        S_set("y", num(0)),
        S_repeat(num(12), calls),
    ])
    if case == "nonwarp_nested_shared_call_late_deopt":
        # Wait across several scheduler checkpoints before forcing deopt;
        # this exercises the SECOND expansion of the inner call source ID.
        gen.script([S_repeat(num(3), [S_change("x", num(0))]),
                    S_set("x", text(""))])
    if case in ("nonwarp_loop_deopt", "nonwarp_loop_arg_deopt",
                "nonwarp_nested_shared_call_deopt",
                "nonwarp_loop_bool_arg_deopt", "nonwarp_loop_nested_args_deopt",
                "nonwarp_until_arg_deopt", "nonwarp_while_arg_deopt",
                "nonwarp_until_nested_deopt", "nonwarp_while_nested_deopt",
                "nonwarp_loop_arg_bound_deopt", "nonwarp_loop_branch_deopt",
                "nonwarp_loop_nested_repeat_deopt",
                "nonwarp_loop_distinct_calls_deopt",
                "nonwarp_loop_shared_nested_deopt"):
        gen.script([S_set("x", text(""))])
    path = directory / f"call-{case}.sb3"
    write_sb3(str(path), make_project(list(gen.vars), list(gen.lists), gen))
    if case.startswith(("nonwarp_while_arg", "nonwarp_while_nested")):
        # The shared benchmark generator has no control_while shorthand.
        # Convert its equivalent until C-block without changing its inputs.
        with zipfile.ZipFile(path) as archive:
            project = json.loads(archive.read("project.json"))
        replacements = 0
        for target in project["targets"]:
            for block in target["blocks"].values():
                if block["opcode"] == "control_repeat_until":
                    block["opcode"] = "control_while"
                    replacements += 1
        assert replacements == 1
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("project.json", json.dumps(project))
    if case in BOOLEAN_CASES:
        with zipfile.ZipFile(path) as archive:
            project = json.loads(archive.read("project.json"))
        for target in project["targets"]:
            for bid, block in list(target["blocks"].items()):
                if block["opcode"] == "argument_reporter_string_number":
                    block["opcode"] = "argument_reporter_boolean"
                if block["opcode"] == "procedures_call":
                    input_id = json.loads(block["mutation"]["argumentids"])[0]
                    boolean_id = "boolean-input-" + bid
                    block["inputs"][input_id] = [2, boolean_id]
                    target["blocks"][boolean_id] = dict(
                        opcode="operator_gt", next=None, parent=bid,
                        inputs={"OPERAND1": [1, [4, "0" if case ==
                            "boolean_arg_false" else "1"]],
                                "OPERAND2": [1, [4, "0"]]},
                        fields={}, shadow=False, topLevel=False)
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("project.json", json.dumps(project))
    if case == "multiple_waits":
        with zipfile.ZipFile(path) as archive:
            project = json.loads(archive.read("project.json"))
        for target in project["targets"]:
            blocks = target["blocks"]
            for index, (bid, block) in enumerate(list(blocks.items())):
                if block["opcode"] != "data_addtolist":
                    continue
                following = block["next"]
                first, second = f"wait-a-{index}", f"wait-b-{index}"
                block["next"] = first
                for wid, parent, nxt in ((first, bid, second),
                                          (second, first, following)):
                    blocks[wid] = dict(opcode="control_wait", next=nxt,
                        parent=parent, inputs={"DURATION": [1, [4, "0"]]},
                        fields={}, shadow=False, topLevel=False)
                if following:
                    blocks[following]["parent"] = second
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
            archive.writestr("project.json", json.dumps(project))
    return path


def run(command: list[str], path: Path, emitter: str, addon: Path | None,
        common: bool) -> tuple[str, str]:
    env = dict(
        os.environ, XYO_JIT_COMMON_CFG="1" if common else "0",
        XYO_JIT_VERBOSE="1", XYO_DUMPVARS="json",
        XYO_HOT_INVOCATIONS="1", XYO_EXIT_ON_DONE="1",
        XYO_PAR="0", XYO_WASM_HOT_EMITTER=emitter,
    )
    if addon:
        env["XYO_BINARYEN_NODE_ADDON"] = str(addon)
    proc = subprocess.run(
        [*command, "run", str(path), "--jit", "hotspot" if common else "off",
         "--fps", "30", "--frames", "150"],
        cwd=ROOT, env=env, text=True, capture_output=True, timeout=120,
    )
    if proc.returncode:
        raise AssertionError(proc.stderr[-1200:])
    match = re.search(r'(?s)(\{\s*"Bench".*\})', proc.stdout)
    if not match:
        raise AssertionError(f"missing Scratch variable JSON: {proc.stdout[-400:]}")
    return match.group(1), proc.stderr


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--wasm", required=True, type=Path)
    parser.add_argument("--addon", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    native, wasm, addon = (p.resolve() for p in
                           (args.native, args.wasm, args.addon))
    args.out.mkdir(parents=True, exist_ok=True)
    configs = {
        "interpreter": ([str(native)], "builtin", None, False),
        "llvm": ([str(native)], "builtin", None, True),
        "builtin": (["node", "web/xyo-node.mjs", str(wasm)],
                    "builtin", None, True),
        "binaryen": (["node", "web/xyo-node.mjs", str(wasm)],
                     "binaryen", addon, True),
    }
    for case in CASES:
        path = fixture(args.out, case)
        results: dict[str, str] = {}
        for backend, (command, mode, addon_, common) in configs.items():
            output, diagnostics = run(command, path, mode, addon_, common)
            results[backend] = output
            if backend != "interpreter":
                compiled = (("xyo-common-cfg: completed" in diagnostics or
                             "xyo-common-cfg: deopt" in diagnostics)
                            if backend == "llvm" else
                            "compiled shared CFG" in diagnostics)
                if case in ("nonwarp_loop_deopt", "nonwarp_loop_arg_deopt",
                            "nonwarp_loop_bool_arg_deopt",
                            "nonwarp_loop_nested_args_deopt", "nonwarp_loop_arg_bound_deopt",
                            "nonwarp_loop_branch_deopt", "nonwarp_loop_nested_repeat_deopt",
                            "nonwarp_loop_distinct_calls_deopt",
                            "nonwarp_loop_shared_nested_deopt",
                            "nonwarp_nested_shared_call_deopt",
                            "nonwarp_nested_shared_call_late_deopt",
                            "nonwarp_until_arg_deopt",
                            "nonwarp_while_arg_deopt",
                            "nonwarp_until_nested_deopt",
                            "nonwarp_while_nested_deopt") and (
                                "xyo-common-cfg: deopt" not in diagnostics or
                                "committed=1" not in diagnostics or
                                "reason=invalid-bindings" not in diagnostics or
                                "rollbacks=1" not in diagnostics):
                    raise AssertionError(
                        f"{backend}: missing post-commit procedure-loop rollback: "
                        f"{diagnostics[-800:]}"
                    )
                if case == "nonwarp_nested_shared_call_late_deopt" and backend != "interpreter":
                    if "slices=4" not in diagnostics:
                        raise AssertionError(
                            f"{backend}: deopt did not reach second inner call instance: "
                            f"{diagnostics[-800:]}"
                        )
                if compiled != (case in SUPPORTED):
                    raise AssertionError(
                        f"{case}/{backend}: compiled={compiled}: "
                        f"{diagnostics[-1000:]}"
                    )
        if len(set(results.values())) != 1:
            raise AssertionError(f"{case}: {results}")
        state = json.loads(results["interpreter"])["Bench"]
        expected_args = {
            "three_parameters": ("12", "0"),
            "conditional": ("12", "0"),
            "nonwarp_loop": ("96", "564"),
            "nonwarp_loop_deopt": ("91", "509"),
            "nonwarp_loop_arg": ("39146837", "13048941"),
            "nonwarp_loop_arg_deopt": ("9786709", "3262234"),
            "nonwarp_loop_bool_arg": ("48", "0"),
            "nonwarp_loop_nested_args": ("1062882", "531452"),
            "nonwarp_loop_arg_bound": ("74", "36"),
            "nonwarp_loop_branch": ("48", "24"),
            "nonwarp_loop_nested_repeat": ("144", "24"),
            "nonwarp_loop_distinct_calls": ("2", "96"),
            "nonwarp_loop_shared_nested": ("2", "240"),
            "nonwarp_nested_shared_call": ("0", "192"),
            "nonwarp_until_arg": ("6", "3"),
            "nonwarp_while_arg": ("6", "3"),
            "nonwarp_until_nested": ("6", "3"),
            "nonwarp_while_nested": ("6", "3"),
            "boolean_arg": ("12", "0"),
            "boolean_arg_false": ("0", "12"),
            "nonwarp_conditional": ("12", "0"),
            "conditional_false": ("0", "12"),
            "repeated_snapshot": ("16777215", "16777191"),
            "many_parameters": ("12", "384"),
            "too_many_parameters": ("12", "396"),
        }.get(case)
        if expected_args and (state["x"], state["y"]) != expected_args:
            raise AssertionError(f"{case}: wrong argument binding: {state}")
        expected_effects = {
            "recursive": ["3"] * 3 + [str(i) for i in range(4, 15)],
            "multiple_waits": ["1"] * 12,
            "type_switch": ["hello", "7"] * 12,
        }.get(case)
        if expected_effects is not None and state["effects"] != expected_effects:
            raise AssertionError(f"{case}: wrong effect order/count: {state}")
        print(
            f"PASS {case}: four-way exact variables, "
            f"compiled={case in SUPPORTED}", flush=True
        )
    print(f"PASS {len(CASES)} shared-IR Call/Return cases", flush=True)


if __name__ == "__main__":
    main()
