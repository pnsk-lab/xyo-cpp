#!/usr/bin/env python3
"""Run probe batteries in scratch-vm and xyo and print element-level diffs.

    python3 tests/differential/probe/run_probes.py [battery ...] [--jit off]

Each battery stores one probe per sprite variable ``pN``; the runner compares
``String(value)`` for every probe and prints the label of each mismatch.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PROBE_DIR = os.path.join(HERE, "probes")
XYO_DUMP = re.compile(r"^var S/(p\d+)\[[^\]]*\]=?(.*)$")


def run(cmd, env=None, timeout=180):
    e = dict(os.environ)
    if env:
        e.update(env)
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=e)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return 124, b"", b"timeout"


def scratch_probes(sb3, steps):
    code, out, err = run(["node", os.path.join(HERE, "..", "scratch_vm_dump.js"),
                          sb3, str(steps)])
    if code != 0:
        return code, {}, err.decode("utf-8", "replace")
    data = json.loads(out)
    return 0, data.get("S", {}), ""


def xyo_probes(xyo, sb3, steps, jit):
    cmd = [xyo, "run", sb3, "--frames", str(steps)]
    if jit:
        cmd += ["--jit", jit]
    code, out, err = run(cmd, env={"XYO_DUMPVARS": "1"})
    vals = {}
    for line in out.decode("utf-8", "surrogatepass").split("\n"):
        m = XYO_DUMP.match(line)
        if m:
            vals[m.group(1)] = m.group(2)
    return code, vals, err.decode("utf-8", "replace")


def show(v):
    return json.dumps(v, ensure_ascii=False)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("batteries", nargs="*")
    ap.add_argument("--xyo", default=os.path.join(ROOT, "build", "xyo"))
    ap.add_argument("--steps", type=int, default=2000)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    names = args.batteries
    if not names:
        names = sorted(f[:-4] for f in os.listdir(PROBE_DIR)
                       if f.endswith(".sb3"))
    total = bad = 0
    for name in names:
        sb3 = os.path.join(PROBE_DIR, name + ".sb3")
        labels = json.load(open(os.path.join(PROBE_DIR, name + ".json")))
        scode, svals, serr = scratch_probes(sb3, args.steps)
        hcode, hvals, herr = xyo_probes(args.xyo, sb3, args.steps, None)
        icode, ivals, ierr = xyo_probes(args.xyo, sb3, args.steps, "off")
        print("=" * 74)
        print("%s  (scratch exit=%d, hotspot exit=%d, interp exit=%d)"
              % (name, scode, hcode, icode))
        if scode != 0:
            print("  scratch failed:", serr.strip()[:400])
        n = 0
        for ent in labels:
            if ent.get("stack"):
                continue
            v = ent["var"]
            s, h, i = svals.get(v), hvals.get(v), ivals.get(v)
            total += 1
            if s == h == i:
                if args.verbose:
                    print("  ok   %-40s %s" % (ent["label"], show(s)))
                continue
            bad += 1
            n += 1
            tag = "BOTH" if (s != h and s != i) else ("HOT " if s != h else "INT ")
            print("  %s %-40s scratch=%s hotspot=%s interp=%s"
                  % (tag, ent["label"], show(s), show(h), show(i)))
        if n == 0:
            print("  %d probes agree" % sum(1 for e in labels
                                            if not e.get("stack")))
    print("=" * 74)
    print("%d probes, %d differ" % (total, bad))
    return 0


if __name__ == "__main__":
    sys.exit(main())
