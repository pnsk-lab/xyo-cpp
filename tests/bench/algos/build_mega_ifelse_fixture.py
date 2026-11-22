#!/usr/bin/env python3
"""Generate 2..8 finite-numeric Scratch if/else repeat benchmark scripts.

Each script:
  when green flag
  repeat R:
    if v_i > 0: change v_i by -1
    else: change v_i by +3
Initial v_i=3, so the state cycles through 3,2,1,0,3.
"""
import argparse
import json
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


def generate(rounds: int, scripts: int, reverse: bool = False,
             no_else: bool = False, two_writes: bool = False) -> dict:
    variables = {}
    blocks = {}
    for i in range(scripts):
        name = f"v{i}"
        variables[name] = [name, 3]
        if two_writes:
            variables[f"w{i}"] = [f"w{i}", 0]
        hat, repeat, branch, cmp, read, yes, no = (
            f"{part}{i}" for part in
            ("hat", "repeat", "branch", "cmp", "read", "yes", "no")
        )
        blocks[hat] = dict(
            opcode="event_whenflagclicked", topLevel=True,
            next=repeat, parent=None, shadow=False,
            inputs={}, fields={}, x=0, y=i * 200)
        blocks[repeat] = dict(
            opcode="control_repeat", next=None, parent=hat,
            inputs={"TIMES": [1, [4, str(rounds)]],
                    "SUBSTACK": [2, branch]}, fields={}, shadow=False)
        branchInputs = {"CONDITION": [2, cmp],
                        "SUBSTACK": [2, yes]}
        if not no_else:
            branchInputs["SUBSTACK2"] = [2, no]
        blocks[branch] = dict(
            opcode="control_if" if no_else else "control_if_else",
            next=None, parent=repeat,
            inputs=branchInputs, fields={}, shadow=False)
        blocks[cmp] = dict(
            # v > 0 and 0 < v are numerically equivalent. The second
            # tests reversed operands in native and helper paths.
            opcode="operator_lt" if reverse else "operator_gt",
            next=None, parent=branch,
            inputs=({"OPERAND1": [1, [4, "0"]],
                     "OPERAND2": [2, read]} if reverse else
                    {"OPERAND1": [2, read],
                     "OPERAND2": [1, [4, "0"]]}),
            fields={}, shadow=False)
        blocks[read] = dict(
            opcode="data_variable", next=None, parent=cmp,
            inputs={}, fields={"VARIABLE": [name, name]}, shadow=False)
        for id, delta in (
            ((yes, -1),) if no_else else ((yes, -1), (no, 3))
        ):
            second_id = f"{id}second" if two_writes else None
            blocks[id] = dict(
                opcode="data_changevariableby", next=second_id,
                parent=branch,
                inputs={"VALUE": [1, [4, str(delta)]]},
                fields={"VARIABLE": [name, name]}, shadow=False)
            if two_writes:
                second_variable = f"w{i}"
                second_delta = 1 if id == yes else 2
                blocks[second_id] = dict(
                    opcode="data_changevariableby", next=None,
                    parent=id, shadow=False,
                    inputs={"VALUE": [1, [4, str(second_delta)]]},
                    fields={"VARIABLE": [second_variable, second_variable]})
    return dict(targets=[dict(
        name="Stage", isStage=True, variables=variables, lists={},
        broadcasts={}, blocks=blocks, costumes=[], sounds=[],
        currentCostume=0, layerOrder=0, volume=100)],
        monitors=[], extensions=[], meta={"semver": "3.0.0"})


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=40000)
    parser.add_argument("--scripts", type=int, default=8)
    parser.add_argument("--reverse", action="store_true",
                        help="use 0 < v instead of v > 0")
    parser.add_argument("--no-else", action="store_true",
                        help="emit control_if with no false branch")
    parser.add_argument("--two-writes", action="store_true",
                        help="add a second ordered numeric write per branch")
    a = parser.parse_args()
    if a.rounds < 0 or not 2 <= a.scripts <= 8:
        parser.error("rounds >= 0, 2 <= scripts <= 8 required")
    a.out.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(a.out, "w", ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(generate(a.rounds, a.scripts, a.reverse,
                                             a.no_else, a.two_writes)))
    expected = (max(0, 3 - a.rounds) if a.no_else else
                [3, 2, 1, 0][a.rounds % 4])
    print(f"expected v0..v{a.scripts-1} = {expected}")
    if a.two_writes:
        true_count = (min(a.rounds, 3) if a.no_else else
                      3 * (a.rounds // 4) + min(3, a.rounds % 4))
        false_count = a.rounds - true_count
        second = true_count + (0 if a.no_else else 2 * false_count)
        print(f"expected w0..w{a.scripts-1} = {second}")
