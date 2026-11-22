#!/usr/bin/env python3
"""Original ScratchBench bench_end AST regression; Mega must fail closed."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import zipfile

from bench_mega_outer_until import block

def extract(source: Path):
    with zipfile.ZipFile(source) as z:
        original = json.loads(z.read("project.json"))
    stage = next(t for t in original["targets"] if t["isStage"])
    nodes = stage["blocks"]
    defs = {}
    for key, node in nodes.items():
        if node["opcode"] != "procedures_definition":
            continue
        prototype_id = node.get("inputs", {}).get("custom_block", [None, None])[1]
        proto = nodes.get(prototype_id, {})
        proccode = proto.get("mutation", {}).get("proccode")
        if proccode:
            defs[proccode] = key
    target_code = "bench_end %s %s"
    if target_code not in defs or "log text %s" not in defs:
        raise AssertionError("original bench_end / log text definitions missing")
    imported = {}
    queue = [defs[target_code]]
    while queue:
        key = queue.pop()
        if key in imported:
            continue
        node = nodes[key]
        imported[key] = node
        next_key = node.get("next")
        if isinstance(next_key, str):
            queue.append(next_key)
        for inp in node.get("inputs", {}).values():
            if len(inp) >= 2 and isinstance(inp[1], str):
                queue.append(inp[1])
        code = node.get("mutation", {}).get("proccode")
        if code in defs and code != target_code:
            queue.append(defs[code])
    call = next(n for n in nodes.values()
                if n["opcode"] == "procedures_call"
                and n.get("mutation", {}).get("proccode") == target_code
                and n.get("inputs", {}).get("id", [None, [None, None]])[1][1] == "sched_pingpong")
    result = {
        "targets": [{
            "isStage": True, "name": "Stage",
            "variables": {
                key: value for key, value in stage["variables"].items()
                if key in ("b_chk", "b_chunks", "b_total", "b_bestrate")
            },
            "lists": {"output": ["output", []]},
            "broadcasts": {}, "blocks": {
                "hat": block("event_whenflagclicked", "benchCall", top_level=True),
                "benchCall": {**call, "next": None, "parent": "hat"},
                **imported,
            }, "costumes": [], "sounds": [],
            "currentCostume": 0, "layerOrder": 0
        }],
        "monitors": [], "extensions": original.get("extensions", []), "meta": {}
    }
    return result

def run(binary, proj, directory, enabled, marker):
    sb3 = directory / f"{marker}.sb3"
    with zipfile.ZipFile(sb3, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("project.json", json.dumps(proj))
    env = {k:v for k,v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": "eager", "XYO_PAR": "0",
        "XYO_JIT_MEGA_BATCH_YIELD": "1",
        "XYO_JIT_MEGA_SINGLE_SENDER": "1",
        "XYO_JIT_MEGA_UNTIL_BCAST": "1",
        "XYO_JIT_MEGA_UNTIL_OUTER": "1",
        "XYO_JIT_MEGA_UNTIL_POST_SET": "1",
        "XYO_JIT_MEGA_UNTIL_BENCH_ROUND": "1",
        "XYO_JIT_MEGA_UNTIL_OUTER_POST_SET": "1" if enabled else "0",
        "XYO_JIT_MEGA_NORMALIZE_YIELD": "1",
        "XYO_DUMPVARS": "json", "XYO_DUMPLISTS": "json",
        "XYO_JIT_MEGA_BATCH_TRACE": "1",
    })
    r = subprocess.run([str(binary), "run", str(sb3), "--fps", "0"],
                       cwd=directory, env=env, capture_output=True, text=True, timeout=30)
    if r.returncode:
        raise AssertionError((marker, r.returncode, r.stderr[-1600:]))
    (directory / f"{marker}.out").write_text(r.stdout)
    (directory / f"{marker}.err").write_text(r.stderr)
    if "[mega-yield]" in r.stderr:
        raise AssertionError(f"{marker}: unsupported bench_end unexpectedly installed Mega")
    return r.stdout

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    args = parser.parse_args()
    baseline = extract(args.source.resolve())
    with tempfile.TemporaryDirectory(prefix="bench-end-real-") as root:
        directory = Path(root)
        for n in [0, 1, 10, 100]:
            p = json.loads(json.dumps(baseline))
            values = p["targets"][0]["variables"]
            values["b_chk"][1] = n
            values["b_chunks"][1] = 5
            values["b_total"][1] = 10
            values["b_bestrate"][1] = 2
            outputs = [run(args.binary.resolve(), p, directory, on, f"{n}-{on}")
                       for on in [False, True]]
            assert outputs[0] == outputs[1], (n, outputs)
            assert '"Stage"' in outputs[0], outputs[0]
            expected = f"sched_pingpong | 10 | 5 | 0.5 | 2 | {n}"
            assert f'"output": ["{expected}"]' in outputs[0], (
                n, outputs[0][-1000:])
            print(f"PASS original bench_end body, check={n}: stable fallback", flush=True)

if __name__ == "__main__":
    main()
