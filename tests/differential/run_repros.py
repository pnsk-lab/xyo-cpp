#!/usr/bin/env python3
"""Run the differential repro projects and compare scratch-vm with xyo.

    python3 tests/differential/run_repros.py [--xyo build/xyo] [--json]

For every ``repro/*.sb3``:

  * scratch-vm (vendored, headless) dumps String(value) for every variable;
  * xyo runs twice: default HotSpot mode and ``--jit off`` (interpreter);
  * the ``out`` list and the process exit status are compared.

This script is informational: some cases are expected to differ until the
divergences documented in ``docs/scratch-divergences.md`` are fixed.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
DUMP_RE = re.compile(r"var (.*?)/(.*?)\[(.*?)\](?: list\[(\d+)\](.*)|=(.*))$")


def parse_xyo_dump(text):
    out = {}
    for line in text.split("\n"):
        m = DUMP_RE.match(line)
        if not m:
            continue
        target, name, _vid = m.group(1), m.group(2), m.group(3)
        if m.group(4) is not None:
            n = int(m.group(4))
            rest = m.group(5)
            items = rest[1:].split(" ") if n else []
            out.setdefault(target, {})[name] = items
        else:
            out.setdefault(target, {})[name] = m.group(6)
    return out


def run(cmd, env=None, timeout=120):
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=e)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, b"", b"timeout"


def scratch_out(sb3, steps):
    cmd = ["node", os.path.join(HERE, "scratch_vm_dump.js"), sb3, str(steps)]
    code, stdout, stderr = run(cmd)
    if code != 0:
        return code, None
    data = json.loads(stdout)
    return code, data.get("Stage", {}).get("out")


def xyo_out(xyo, sb3, steps, jit):
    cmd = [xyo, "run", sb3, "--frames", str(steps)]
    if jit:
        cmd += ["--jit", jit]
    code, stdout, stderr = run(cmd, env={"XYO_DUMPVARS": "1"})
    # xyo writes the dump to stdout; tolerate invalid UTF-8 from lone
    # surrogates in unrelated fixtures.
    text = stdout.decode("utf-8", "surrogatepass")
    dump = parse_xyo_dump(text)
    return code, dump.get("Stage", {}).get("out")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--xyo", default=os.path.join(ROOT, "build", "xyo"))
    ap.add_argument("--steps", type=int, default=1000)
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--repro-dir", default=os.path.join(HERE, "repro"))
    args = ap.parse_args()

    files = sorted(f for f in os.listdir(args.repro_dir) if f.endswith(".sb3"))
    rows = []
    for name in files:
        sb3 = os.path.join(args.repro_dir, name)
        scode, sout = scratch_out(sb3, args.steps)
        hcode, hout = xyo_out(args.xyo, sb3, args.steps, None)
        icode, iout = xyo_out(args.xyo, sb3, args.steps, "off")
        rows.append({
            "case": name,
            "scratch": {"exit": scode, "out": sout},
            "hotspot": {"exit": hcode, "out": hout},
            "interp": {"exit": icode, "out": iout},
            "hotspot_differs": hout != sout or hcode != scode,
            "interp_differs": iout != sout or icode != scode,
        })

    if args.json:
        print(json.dumps(rows, ensure_ascii=False, indent=1))
        return 0

    for r in rows:
        print("=" * 72)
        print(r["case"])
        print("  scratch : exit=%-3d out=%s" % (r["scratch"]["exit"],
                                                json.dumps(r["scratch"]["out"],
                                                           ensure_ascii=False)))
        print("  hotspot : exit=%-3d out=%s%s" % (
            r["hotspot"]["exit"],
            json.dumps(r["hotspot"]["out"], ensure_ascii=False),
            "   <-- DIFFERS" if r["hotspot_differs"] else ""))
        print("  interp  : exit=%-3d out=%s%s" % (
            r["interp"]["exit"],
            json.dumps(r["interp"]["out"], ensure_ascii=False),
            "   <-- DIFFERS" if r["interp_differs"] else ""))
    print("=" * 72)
    print("%d cases: %d differ in HotSpot mode, %d differ in interpreter mode"
          % (len(rows), sum(r["hotspot_differs"] for r in rows),
             sum(r["interp_differs"] for r in rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
