#!/usr/bin/env python3
"""Paired startup-to-completion benchmark: one yielding broadcast recipient.

This is a synthetic source-proven control_repeat fixture, not ScratchBench.
The sender loops over broadcast-and-wait, and the sole receiver repeats a
constant numeric update. All Scratch side effects must match exactly.
"""
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

from build_mega_broadcast_fixture import block, project


def fixture(sender_repetitions: int, receiver_repetitions: int,
            no_receiver: bool = False, until: bool = False) -> dict:
    data = project([receiver_repetitions, 0], second_receiver=False)
    stage = data["targets"][0]
    blocks = stage["blocks"]
    for suffix in ("hat1", "send1", "change1"):
        del blocks[suffix]
    del stage["variables"]["v1"]
    del stage["variables"]["rx1"]
    stage["broadcasts"] = {"ping": "PING"}
    if no_receiver:
        for key in ("recv0", "repeat0", "inc0"):
            del blocks[key]
    blocks["hat0"]["next"] = "senderRepeat"
    blocks["senderRepeat"] = block(
        "control_repeat", "change0", parent="hat0", inputs={
            "TIMES": [1, [4, str(sender_repetitions)]],
            "SUBSTACK": [2, "send0"]
        })
    blocks["send0"]["next"] = None
    blocks["send0"]["parent"] = "senderRepeat"
    blocks["change0"]["parent"] = "senderRepeat"
    if until:
        # The receiver writes rx0, not the sender. Reevaluate the condition
        # after each joined broadcast instead of using a fixed loop latch.
        blocks["senderRepeat"]["opcode"] = "control_repeat_until"
        blocks["senderRepeat"]["inputs"] = {
            "CONDITION": [2, "untilCondition"],
            "SUBSTACK": [2, "send0"],
        }
        blocks["untilCondition"] = block(
            "operator_gt", parent="senderRepeat", inputs={
                "OPERAND1": [2, "untilValue"],
                "OPERAND2": [1, [4, str(sender_repetitions * receiver_repetitions - 1)]],
            })
        blocks["untilValue"] = block(
            "data_variable", parent="untilCondition",
            fields={"VARIABLE": ["rx0", "rx0"]})
    return data


def run():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--runs", type=int, default=12)
    parser.add_argument("--send-count", type=int, default=10000)
    parser.add_argument("--receive-count", type=int, default=3)
    parser.add_argument("--no-receiver", action="store_true",
                        help="Sender join has zero receivers every iteration")
    parser.add_argument("--until", action="store_true",
                        help="Use receiver-updated dynamic until instead of repeat")
    parser.add_argument("--cpu", type=int, default=16)
    parser.add_argument("--optimize", choices=["receiver", "sender", "latch",
                                                "sender-latch", "normalize",
                                                "sender-latch-normalized",
                                                "until-sender"],
                        default="receiver",
                        help="Toggle one receiving vs one sending Mega PC")
    args = parser.parse_args()
    if args.runs < 1 or args.send_count < 1 or args.receive_count < 1:
        parser.error("all counts must be positive")
    if args.until and args.no_receiver:
        parser.error("--until needs a receiver to make progress")
    if (args.optimize == "until-sender") != args.until:
        parser.error("--until and --optimize until-sender must be used together")
    os.sched_setaffinity(0, {args.cpu})
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    source = args.binary.resolve()
    binary = args.out / "frozen-xyo"
    shutil.copy2(source, binary)
    binary.chmod(binary.stat().st_mode | 0o111)
    project_file = args.out / "single-yielding-receiver.sb3"
    with zipfile.ZipFile(project_file, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("project.json", json.dumps(
            fixture(args.send_count, args.receive_count,
                    args.no_receiver, args.until)))
    baseline = None
    rows = []
    env = {k: v for k, v in os.environ.items() if not k.startswith("XYO_")}
    env.update({
        "XYO_JIT_MODE": "eager",
        "XYO_PAR": "0",
        "XYO_YIELD_QMODE": "off",
        "XYO_DUMPVARS": "json",
        "XYO_JIT_MEGA_BATCH_YIELD": "1",
        "XYO_JIT_MEGA_BCAST_RECEIVERS": "1",
    })
    if args.optimize == "latch":
        # Both variants have the same singleton LLVM sender installed.
        # Vary ONLY the fast host-side broadcast repeat latch.
        env["XYO_JIT_MEGA_SINGLE_SENDER"] = "1"
    if args.optimize in ("sender-latch", "sender-latch-normalized"):
        # Compare complete fast-latch sender path with original Fiber sender.
        env["XYO_JIT_MEGA_FAST_BCAST_LATCH"] = "1"
    if args.optimize == "normalize":
        env["XYO_JIT_MEGA_SINGLE_SENDER"] = "1"
        env["XYO_JIT_MEGA_FAST_BCAST_LATCH"] = "1"
    if args.optimize == "sender-latch-normalized":
        env["XYO_JIT_MEGA_NORMALIZE_YIELD"] = "1"
    if args.optimize == "until-sender":
        env["XYO_JIT_MEGA_UNTIL_BCAST"] = "1"
        env["XYO_JIT_MEGA_NORMALIZE_YIELD"] = "1"
    for pair in range(1, args.runs + 1):
        for on in ((False, True) if pair % 2 else (True, False)):
            label = "on" if on else "off"
            flag = {
                "receiver": "XYO_JIT_MEGA_SINGLE_RECEIVER",
                "sender": "XYO_JIT_MEGA_SINGLE_SENDER",
                "latch": "XYO_JIT_MEGA_FAST_BCAST_LATCH",
                "sender-latch": "XYO_JIT_MEGA_SINGLE_SENDER",
                "normalize": "XYO_JIT_MEGA_NORMALIZE_YIELD",
                "sender-latch-normalized": "XYO_JIT_MEGA_SINGLE_SENDER",
                "until-sender": "XYO_JIT_MEGA_SINGLE_SENDER",
            }[args.optimize]
            trial = dict(env, **{flag: "1" if on else "0"})
            t = time.perf_counter()
            result = subprocess.run(
                [str(binary), "run", str(project_file), "--fps", "0"],
                env=trial, cwd=args.out, capture_output=True, text=True,
                timeout=180)
            elapsed = (time.perf_counter() - t) * 1000
            (args.out / f"{pair:02d}-{label}.out").write_text(result.stdout)
            (args.out / f"{pair:02d}-{label}.err").write_text(result.stderr)
            match = re.search(r'\{\s*"Stage"\s*:\s*\{.*?\}\s*\}',
                              result.stdout, re.S)
            values = json.loads(match.group())["Stage"] if match else None
            expected = (0 if args.no_receiver else
                        args.send_count * args.receive_count)
            if (result.returncode != 0 or values is None or
                float(values.get("rx0", -1)) != expected or
                float(values.get("v0", -1)) != 10):
                raise RuntimeError(
                    f"Incorrect {pair}/{label} rc={result.returncode}, "
                    f"vars={values}, stderr={result.stderr[-1000:]}")
            if baseline is None:
                baseline = values
            elif values != baseline:
                raise RuntimeError(f"ON/OFF Scratch mismatch: {values} != {baseline}")
            print(f"{pair:02d} {label}: {elapsed:.3f} ms", flush=True)
            rows.append({"pair": pair, "variant": label, "ms": elapsed})
    ratios = []
    for pair in range(1, args.runs + 1):
        off = next(row["ms"] for row in rows if row["pair"] == pair
                   and row["variant"] == "off")
        on = next(row["ms"] for row in rows if row["pair"] == pair
                  and row["variant"] == "on")
        ratios.append(off / on)
    summary = {
        "off_median_ms": statistics.median(
            row["ms"] for row in rows if row["variant"] == "off"),
        "on_median_ms": statistics.median(
            row["ms"] for row in rows if row["variant"] == "on"),
        "paired_speed_ratio": math.exp(statistics.mean(
            math.log(ratio) for ratio in ratios)),
        "on_wins": sum(ratio > 1 for ratio in ratios),
    }
    report = {
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "fixture_sha256": hashlib.sha256(project_file.read_bytes()).hexdigest(),
        "fixture": {"send_count": args.send_count,
                    "receive_count": args.receive_count,
                    "no_receiver": args.no_receiver,
                    "dynamic_until": args.until},
        "optimization": args.optimize,
        "expected_stage_variables": baseline,
        "runs": rows,
        "summary": summary,
    }
    (args.out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    run()
