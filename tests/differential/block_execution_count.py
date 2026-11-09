#!/usr/bin/env python3
"""Regression test: exact Scratch primitive execution counts.

Usage: python3 tests/differential/block_execution_count.py [path/to/xyo]
"""
import json
import math
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import zipfile


def block(opcode, parent=None, next_id=None, *, inputs=None,
          fields=None, top=False):
    value = {
        "opcode": opcode, "next": next_id, "parent": parent,
        "inputs": inputs or {}, "fields": fields or {},
        "shadow": False, "topLevel": top,
    }
    if top:
        value["x"] = 0
        value["y"] = 0
    return value


def project():
    # repeat(3) checks its condition 4 times. 3 changes, then
    # one variable reporter + one addition + one set => 10 calls.
    blocks = {
        "hat": block("event_whenflagclicked", next_id="repeat", top=True),
        "repeat": block("control_repeat", "hat", "set",
                        inputs={"TIMES": [1, [4, "3"]],
                                "SUBSTACK": [2, "change"]}),
        "change": block("data_changevariableby", "repeat",
                        inputs={"VALUE": [1, [4, "1"]]},
                        fields={"VARIABLE": ["x", "x"]}),
        "set": block("data_setvariableto", "repeat",
                     inputs={"VALUE": [3, "sum", [4, "0"]]},
                     fields={"VARIABLE": ["y", "y"]}),
        "sum": block("operator_add", "set",
                     inputs={"NUM1": [3, "readx", [4, "0"]],
                             "NUM2": [1, [4, "1"]]}),
        "readx": block("data_variable", "sum",
                       fields={"VARIABLE": ["x", "x"]}),
    }
    stage = {
        "isStage": True, "name": "Stage",
        "variables": {"x": ["x", 0], "y": ["y", 0]},
        "lists": {}, "broadcasts": {}, "blocks": blocks,
        "comments": {}, "costumes": [], "sounds": [],
        "currentCostume": 0, "volume": 100, "layerOrder": 0,
    }
    return {"targets": [stage], "monitors": [], "extensions": [],
            "meta": {"semver": "3.0.0", "vm": "11.1.0", "agent": "xyo block count test"}}


def main():
    exe = str(Path(sys.argv[1] if len(sys.argv) > 1 else "build/xyo").resolve())
    with tempfile.TemporaryDirectory(prefix="xyo-block-count-") as temp:
        sb3 = Path(temp) / "count.sb3"
        with zipfile.ZipFile(sb3, "w") as out:
            out.writestr("project.json", json.dumps(project()))
        cases = [
            (["--block-count"], {}, 10),
            (["--block-count", "--jit=eager"], {}, 10),
            (["--block-count", "--jit=off"], {}, 10),
            ([], {"XYO_BLOCK_COUNT": "1"}, 10),
            ([], {}, None),
        ]
        for flags, override, expected in cases:
            env = dict(os.environ)
            env.pop("XYO_BLOCK_COUNT", None)
            env.update(override)
            cmd = [exe, "run", str(sb3), "--frames", "20", *flags]
            result = subprocess.run(cmd, env=env, text=True,
                                    capture_output=True, timeout=20)
            assert result.returncode == 0, (cmd, result.stdout, result.stderr)
            assert "done" in result.stdout, (cmd, result.stdout)
            match = re.search(r"\[block-count\] executed=(\d+)", result.stderr)
            actual = int(match.group(1)) if match else None
            assert actual == expected, (cmd, expected, actual, result.stderr)
            timing = re.search(r"elapsed=([0-9.]+)s blocks/s=([0-9.]+)", result.stderr)
            if expected is not None:
                assert timing is not None, (cmd, result.stderr)
                seconds, rate = map(float, timing.groups())
                assert seconds > 0 and math.isfinite(seconds), result.stderr
                assert rate > 0 and math.isfinite(rate), result.stderr
                assert abs(rate * seconds - expected) < 0.05, result.stderr
            else:
                assert timing is None, result.stderr
            print(f"PASS: {flags or override or 'disabled'} => {actual}")

        # A false branch must evaluate its condition, but never its body.
        # 10 baseline calls + read-y, greater-than, if = 13 calls.
        branched = project()
        bs = branched["targets"][0]["blocks"]
        bs["set"]["next"] = "if"
        bs["if"] = block("control_if", "set", inputs={
            "CONDITION": [3, "cmp", [10, ""]],
            "SUBSTACK": [2, "dead"],
        })
        bs["cmp"] = block("operator_gt", "if", inputs={
            "OPERAND1": [3, "ready", [4, "0"]],
            "OPERAND2": [1, [4, "100"]],
        })
        bs["ready"] = block("data_variable", "cmp",
                            fields={"VARIABLE": ["y", "y"]})
        bs["dead"] = block("data_changevariableby", "if",
                           inputs={"VALUE": [1, [4, "1"]]},
                           fields={"VARIABLE": ["x", "x"]})
        sb3 = Path(temp) / "dead-branch.sb3"
        with zipfile.ZipFile(sb3, "w") as out:
            out.writestr("project.json", json.dumps(branched))
        result = subprocess.run(
            [exe, "run", str(sb3), "--frames", "20", "--block-count",
             "--jit=eager"],
            capture_output=True, text=True, timeout=20,
        )
        assert result.returncode == 0, result.stderr
        assert "[block-count] executed=13 " in result.stderr, result.stderr
        print("PASS: false branch condition counted, skipped body excluded => 13")

        def compare_interpreter_jit(name, blocks, *, variables=None,
                                    graph=None):
            if graph is None:
                graph = project()
                stage = graph["targets"][0]
                stage["blocks"] = blocks
                if variables:
                    stage["variables"] = variables
            candidate = Path(temp) / (name + ".sb3")
            with zipfile.ZipFile(candidate, "w") as out:
                out.writestr("project.json", json.dumps(graph))
            results = []
            for mode in ["off", "eager", "hotspot"]:
                env = dict(os.environ)
                env.pop("XYO_BLOCK_COUNT", None)
                env["XYO_JIT_VERBOSE"] = "1"
                args = [
                    exe, "run", str(candidate), "--frames", "40",
                    "--block-count", "--jit=" + mode
                ]
                result = subprocess.run(args, env=env, text=True,
                                        capture_output=True, timeout=20)
                assert result.returncode == 0, (
                    name, mode, result.stdout, result.stderr)
                match = re.search(r"\[block-count\] executed=(\d+)", result.stderr)
                assert match is not None, (name, mode, result.stderr)
                if mode == "eager":
                    assert "xyo-jit: compiled " in result.stderr, (
                        name, "eager failed to use native JIT", result.stderr)
                results.append(int(match.group(1)))
            assert results[0] == results[1] == results[2], (name, results)
            print(f"PASS: {name} Interpreter/Eager/HotSpot = {results[0]}")

        compare_interpreter_jit("dynamic_repeat", {
            "hat": block("event_whenflagclicked", next_id="rep", top=True),
            "rep": block("control_repeat", "hat", "set", inputs={
                "TIMES": [3, "r0", [4, "3"]],
                "SUBSTACK": [2, "change"]}),
            "r0": block("data_variable", "rep",
                         fields={"VARIABLE": ["x", "x"]}),
            "change": block("data_changevariableby", "rep",
                            inputs={"VALUE": [1, [4, "-1"]]},
                            fields={"VARIABLE": ["x", "x"]}),
            "set": block("data_setvariableto", "rep",
                         inputs={"VALUE": [1, [4, "1"]]},
                         fields={"VARIABLE": ["y", "y"]}),
        }, variables={"x": ["x", 3], "y": ["y", 0]})

        compare_interpreter_jit("nested_repeat", {
            "hat": block("event_whenflagclicked", next_id="outer", top=True),
            "outer": block("control_repeat", "hat", inputs={
                "TIMES": [1, [4, "2"]], "SUBSTACK": [2, "inner"]}),
            "inner": block("control_repeat", "outer", inputs={
                "TIMES": [1, [4, "3"]], "SUBSTACK": [2, "change"]}),
            "change": block("data_changevariableby", "inner",
                            inputs={"VALUE": [1, [4, "1"]]},
                            fields={"VARIABLE": ["x", "x"]}),
        })

        compare_interpreter_jit("while_variable", {
            "hat": block("event_whenflagclicked", next_id="while", top=True),
            "while": block("control_while", "hat", inputs={
                "CONDITION": [3, "lt", [10, ""]],
                "SUBSTACK": [2, "change"]}),
            "lt": block("operator_lt", "while", inputs={
                "OPERAND1": [3, "readx", [4, "0"]],
                "OPERAND2": [1, [4, "3"]]}),
            "readx": block("data_variable", "lt",
                           fields={"VARIABLE": ["x", "x"]}),
            "change": block("data_changevariableby", "while",
                            inputs={"VALUE": [1, [4, "1"]]},
                            fields={"VARIABLE": ["x", "x"]}),
        })

        compare_interpreter_jit("for_each", {
            "hat": block("event_whenflagclicked", next_id="for", top=True),
            "for": block("control_for_each", "hat", inputs={
                "VALUE": [1, [4, "3"]],
                "SUBSTACK": [2, "change"]},
                fields={"VARIABLE": ["x", "x"]}),
            "change": block("data_changevariableby", "for",
                            inputs={"VALUE": [1, [4, "1"]]},
                            fields={"VARIABLE": ["y", "y"]}),
        })

        compare_interpreter_jit("wait_until_ready", {
            "hat": block("event_whenflagclicked", next_id="wait", top=True),
            "wait": block("control_wait_until", "hat", "set", inputs={
                "CONDITION": [3, "eq", [10, ""]]}),
            "eq": block("operator_equals", "wait", inputs={
                "OPERAND1": [3, "r0", [4, "0"]],
                "OPERAND2": [1, [4, "0"]]}),
            "r0": block("data_variable", "eq",
                         fields={"VARIABLE": ["x", "x"]}),
            "set": block("data_setvariableto", "wait",
                         inputs={"VALUE": [1, [4, "1"]]},
                         fields={"VARIABLE": ["y", "y"]}),
        })

        # A native control_stop never returns to the generated function:
        # xyo_yield must flush pending blocks before the fiber is discarded.
        compare_interpreter_jit("stop_discards_fiber", {
            "hat": block("event_whenflagclicked", next_id="rep", top=True),
            "rep": block("control_repeat", "hat", "unreached", inputs={
                "TIMES": [1, [4, "100"]],
                "SUBSTACK": [2, "stop"]}),
            "stop": block("control_stop", "rep",
                          fields={"STOP_OPTION": ["this script", None]}),
            "unreached": block("data_changevariableby", "rep",
                               fields={"VARIABLE": ["x", "x"]},
                               inputs={"VALUE": [1, [4, "1"]]}),
        })

        # Consecutive non-warp yields must publish multiple short slices,
        # including the last slice when the native entry returns.
        compare_interpreter_jit("repeated_yields", {
            "hat": block("event_whenflagclicked", next_id="rep", top=True),
            "rep": block("control_repeat", "hat", inputs={
                "TIMES": [1, [4, "64"]], "SUBSTACK": [2, "change"]}),
            "change": block("data_changevariableby", "rep",
                            inputs={"VALUE": [1, [4, "1"]]},
                            fields={"VARIABLE": ["x", "x"]}),
        })

        compare_interpreter_jit("if_else", {
            "hat": block("event_whenflagclicked", next_id="if", top=True),
            "if": block("control_if_else", "hat", inputs={
                "CONDITION": [3, "eq", [10, ""]],
                "SUBSTACK": [2, "true"], "SUBSTACK2": [2, "false"]}),
            "eq": block("operator_equals", "if", inputs={
                "OPERAND1": [3, "readx", [4, "0"]],
                "OPERAND2": [1, [4, "0"]]}),
            "readx": block("data_variable", "eq",
                           fields={"VARIABLE": ["x", "x"]}),
            "true": block("data_changevariableby", "if",
                          fields={"VARIABLE": ["x", "x"]},
                          inputs={"VALUE": [1, [4, "1"]]}),
            "false": block("data_changevariableby", "if",
                           fields={"VARIABLE": ["x", "x"]},
                           inputs={"VALUE": [1, [4, "2"]]}),
        })

        # Regression for compiled native procedures and their argument
        # reporter: one procedures_definition primitive executes per call.
        import importlib.util
        fixture_path = (Path(__file__).resolve().parents[1] /
                        "fixtures" / "gen_parfor_fixture.py")
        spec = importlib.util.spec_from_file_location("parfor_fixture",
                                                       fixture_path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        compare_interpreter_jit("warp_procedure_with_arguments", {},
                                graph=module.project(20))
    return 0


if __name__ == "__main__":
    sys.exit(main())
