#!/usr/bin/env python3
"""Paired native-Fiber vs Mega outer repeat + wall-clock until broadcast.

This is a TIME-BOUNDED workload. Its broadcast count legitimately varies
between runs; only deterministic effect/order invariants are asserted.
Raw outputs are retained for diagnosis. Use the score (total receptions /
process elapsed ms) only as an exploratory metric, not a fixed-work speedup.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import statistics
import subprocess
import time
import zipfile


def block(opcode: str, nxt: str | None = None, *,
          inputs: dict | None = None, fields: dict | None = None,
          top_level: bool = False) -> dict:
    return {
        "opcode": opcode,
        "next": nxt,
        "parent": None,
        "topLevel": top_level,
        "inputs": inputs or {},
        "fields": fields or {},
        "shadow": False,
    }


def fixture(outer_count: int, budget_ms: float,
            post_measure: bool = False) -> dict:
    num = lambda v: [1, [4, str(v)]]
    reader = lambda name: block(
        "data_variable", fields={"VARIABLE": [name, name]})
    inc = lambda name, amount=1, nxt=None: block(
        "data_changevariableby", nxt,
        inputs={"VALUE": num(amount)},
        fields={"VARIABLE": [name, name]})
    blocks = {
        "hat": block("event_whenflagclicked", "outer", top_level=True),
        "outer": block("control_repeat", "final",
                       inputs={"TIMES": num(outer_count),
                               "SUBSTACK": [2, "reset"]}),
        "reset": block("data_setvariableto", "t0Set",
                       inputs={"VALUE": num(0)},
                       fields={"VARIABLE": ["hits", "hits"]}),
        "t0Set": block("data_setvariableto", "until",
                       inputs={"VALUE": [2, "t0Clock"]},
                       fields={"VARIABLE": ["b_t0", "b_t0"]}),
        "t0Clock": block("sensing_dayssince2000"),
        "until": block("control_repeat_until", "inner",
                       inputs={"CONDITION": [2, "not"],
                               "SUBSTACK": [2, "send"]}),
        "not": block("operator_not", inputs={"OPERAND": [2, "compare"]}),
        "compare": block(
            "operator_lt",
            inputs={"OPERAND1": [2, "elapsed"],
                    "OPERAND2": [2, "budget"]}),
        "elapsed": block(
            "operator_multiply",
            inputs={"NUM1": [2, "subtract"],
                    "NUM2": num(86400000)}),
        "subtract": block(
            "operator_subtract",
            inputs={"NUM1": [2, "now"],
                    "NUM2": [2, "startRead"]}),
        "now": block("sensing_dayssince2000"),
        "startRead": reader("b_t0"),
        "budget": reader("b_budget"),
        "send": block("event_broadcastandwait",
                      inputs={"BROADCAST_INPUT": [1, [11, "PING", "ping"]]}),
        "inner": inc("sum", 10),
        "final": inc("sum", 100),
        "recvHat": block("event_whenbroadcastreceived", "incHits",
                         top_level=True,
                         fields={"BROADCAST_OPTION": ["PING", "ping"]}),
        "incHits": inc("hits", 1, "incTotal"),
        "incTotal": inc("total"),
    }
    if post_measure:
        blocks["until"]["next"] = "postClock"
        blocks["postClock"] = block(
            "data_setvariableto", "postCount",
            inputs={"VALUE": [2, "finishedAt"]},
            fields={"VARIABLE": ["b_t1", "b_t1"]})
        blocks["finishedAt"] = block("sensing_dayssince2000")
        blocks["postCount"] = block(
            "data_setvariableto", "inner",
            inputs={"VALUE": [2, "hitsRead"]},
            fields={"VARIABLE": ["b_n", "b_n"]})
        blocks["hitsRead"] = reader("hits")
    stage = {
        "isStage": True,
        "name": "Stage",
        "variables": {
            "sum": ["sum", 0], "hits": ["hits", 0],
            "total": ["total", 0], "b_t0": ["b_t0", 0],
            "b_budget": ["b_budget", budget_ms],
            "b_t1": ["b_t1", 0], "b_n": ["b_n", -1],
        },
        "lists": {},
        "broadcasts": {"ping": "PING"},
        "blocks": blocks,
        "costumes": [], "sounds": [],
        "currentCostume": 0, "layerOrder": 0,
    }
    return {"targets": [stage], "monitors": [],
            "extensions": [], "meta": {}}


def graft_real_bench_round(project: dict, original: Path) -> None:
    """Graft original ScratchBench's exact bench_round procedure AST."""
    with zipfile.ZipFile(original) as archive:
        source = json.loads(archive.read("project.json"))
    original_stage = next(t for t in source["targets"] if t.get("isStage"))
    nodes = original_stage["blocks"]
    definition = None
    for key, value in nodes.items():
        if value.get("opcode") != "procedures_definition":
            continue
        ref = value.get("inputs", {}).get("custom_block", [None, None])[1]
        prototype = nodes.get(ref, {})
        if prototype.get("mutation", {}).get("proccode") == "bench_round":
            definition = key
            break
    if definition is None:
        raise ValueError("ScratchBench does not contain bench_round definition")
    calling = next((node for node in nodes.values()
                    if node.get("opcode") == "procedures_call" and
                    node.get("mutation", {}).get("proccode") == "bench_round"),
                   None)
    if calling is None or calling.get("inputs"):
        raise ValueError("Expected argumentless bench_round call")
    imported: dict = {}
    queue = [definition]
    while queue:
        key = queue.pop()
        if key in imported:
            continue
        node = nodes.get(key)
        if node is None:
            raise ValueError(f"Missing ScratchBench block {key}")
        imported[key] = node
        if isinstance(node.get("next"), str):
            queue.append(node["next"])
        for item in node.get("inputs", {}).values():
            if len(item) > 1 and isinstance(item[1], str):
                queue.append(item[1])
    target = project["targets"][0]
    if set(imported) & set(target["blocks"]):
        raise ValueError("ScratchBench block IDs collide with fixture IDs")
    target["blocks"].update(imported)
    target["blocks"]["inner"] = {
        "opcode": "procedures_call", "next": None, "parent": "until",
        "topLevel": False, "inputs": {}, "fields": {},
        "mutation": calling["mutation"],
    }
    for var_id in ("b_ms", "b_round", "b_total", "b_chunks",
                   "b_bestrate"):
        target["variables"][var_id] = [var_id, 0]
    target["variables"]["b_n"] = ["b_n", -1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument("--cpu", type=int, default=16)
    parser.add_argument("--outer-count", type=int, default=5)
    parser.add_argument("--budget-ms", type=float, default=15.0)
    parser.add_argument("--post-measure", action="store_true",
                        help="Add exact clock and counter snapshots after inner until")
    parser.add_argument("--bench-round-from", type=Path,
                        help="Real ScratchBench .sb3 from which to import bench_round AST")
    args = parser.parse_args()
    if args.bench_round_from and not args.post_measure:
        parser.error("--bench-round-from requires --post-measure")
    if args.runs <= 0 or args.outer_count <= 0 or args.budget_ms < 2:
        parser.error("need positive runs/outer-count and budget-ms >= 2")
    os.sched_setaffinity(0, {args.cpu})
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binary = out / "frozen-xyo"
    shutil.copy2(args.binary.resolve(), binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    project = out / "outer-until-timed.sb3"
    generated = fixture(args.outer_count, args.budget_ms,
                        args.post_measure)
    if args.bench_round_from:
        graft_real_bench_round(generated, args.bench_round_from)
    with zipfile.ZipFile(project, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(generated))

    base_env = {
        k: v for k, v in os.environ.items() if not k.startswith("XYO_")
    }
    base_env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_PAR": "0",
        "XYO_YIELD_QMODE": "off",
        "XYO_JIT_MEGA_BATCH_YIELD": "1",
        "XYO_JIT_MEGA_SINGLE_SENDER": "1",
        "XYO_JIT_MEGA_UNTIL_BCAST": "1",
        "XYO_JIT_MEGA_UNTIL_POST_SET": "1" if args.post_measure else "0",
        "XYO_JIT_MEGA_NORMALIZE_YIELD": "1",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_BATCH_TRACE": "1",
    })
    results = []
    for pair in range(1, args.runs + 1):
        for on in ((False, True) if pair % 2 else (True, False)):
            label = "on" if on else "off"
            env = dict(base_env)
            if args.bench_round_from:
                env["XYO_JIT_MEGA_UNTIL_OUTER"] = "1"
                env["XYO_JIT_MEGA_UNTIL_BENCH_ROUND"] = "1" if on else "0"
            else:
                env["XYO_JIT_MEGA_UNTIL_OUTER"] = "1" if on else "0"
            start = time.perf_counter()
            proc = subprocess.run(
                [str(binary), "run", str(project), "--fps", "0"],
                cwd=out, env=env, capture_output=True, text=True,
                timeout=60)
            elapsed = (time.perf_counter() - start) * 1000
            (out / f"{pair:02d}-{label}.out").write_text(proc.stdout)
            (out / f"{pair:02d}-{label}.err").write_text(proc.stderr)
            match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}',
                              proc.stdout, re.S)
            values = json.loads(match.group())["Stage"] if match else {}
            receptions = float(values.get("total", -1))
            expected_sum = (100 if args.bench_round_from else
                            args.outer_count * 10 + 100)
            valid = (proc.returncode == 0 and
                     float(values.get("sum", -1)) == expected_sum and
                     receptions >= args.outer_count and
                     1 <= float(values.get("hits", -1)) <= receptions)
            if args.post_measure:
                valid = (valid and
                         float(values.get("b_n", -1)) ==
                         float(values.get("hits", -2)) and
                         float(values.get("b_t1", -1)) >=
                         float(values.get("b_t0", 0)))
            if args.bench_round_from:
                valid = (valid and
                         float(values.get("b_round", -1)) == args.outer_count and
                         float(values.get("b_chunks", -1)) >= 0 and
                         float(values.get("b_total", -1)) >= 0)
            installed = "[mega-yield]" in proc.stderr
            if not valid or installed != on:
                raise RuntimeError(
                    f"Invalid {pair}/{label}: code={proc.returncode}, "
                    f"vars={values}, installed={installed}, "
                    f"stderr={proc.stderr[-1500:]}")
            score = receptions / elapsed
            print(f"{pair:02d} {label}: elapsed={elapsed:.3f}ms "
                  f"received={receptions:.0f} score={score:.3f}/ms",
                  flush=True)
            results.append({
                "pair": pair, "variant": label,
                "elapsed_ms": elapsed, "receptions": receptions,
                "receptions_per_process_ms": score,
            })
    ratios = []
    for pair in range(1, args.runs + 1):
        off = next(r for r in results if r["pair"] == pair and
                   r["variant"] == "off")
        on = next(r for r in results if r["pair"] == pair and
                  r["variant"] == "on")
        ratios.append(on["receptions_per_process_ms"] /
                      off["receptions_per_process_ms"])
    summary = {
        "off_median_receptions_per_process_ms": statistics.median(
            r["receptions_per_process_ms"] for r in results
            if r["variant"] == "off"),
        "on_median_receptions_per_process_ms": statistics.median(
            r["receptions_per_process_ms"] for r in results
            if r["variant"] == "on"),
        "paired_geomean_score_ratio": math.exp(statistics.mean(
            math.log(x) for x in ratios)),
        "on_wins": sum(ratio > 1 for ratio in ratios),
        "score_disclaimer": "Time-bounded variable-workload exploratory score; "
                            "not a controlled fixed-work throughput speedup.",
    }
    report = {
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "project_sha256": hashlib.sha256(project.read_bytes()).hexdigest(),
        "outer_count": args.outer_count,
        "budget_ms": args.budget_ms,
        "post_measure": args.post_measure,
        "real_bench_round": str(args.bench_round_from) if args.bench_round_from else None,
        "runs": results,
        "summary": summary,
    }
    (out / "results.json").write_text(
        json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
