#!/usr/bin/env python3
"""Run the multi-target probes (hat order, broadcast, sensing_of)."""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
PROBE_DIR = os.path.join(HERE, "probes")
LIST_RE = re.compile(r"^var Stage/out\[[^\]]*\] list\[(\d+)\](.*)$")


def run(cmd, env=None, timeout=180):
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.run(cmd, capture_output=True, timeout=timeout, env=e)
    return p.returncode, p.stdout, p.stderr


def scratch(sb3, steps=1500):
    code, out, err = run(["node", os.path.join(HERE, "..", "scratch_vm_dump.js"),
                          sb3, str(steps)])
    if code != 0:
        return code, None, err.decode("utf-8", "replace")
    return 0, json.loads(out), ""


def xyo(xyo_bin, sb3, steps, jit):
    cmd = [xyo_bin, "run", sb3, "--frames", str(steps)]
    if jit:
        cmd += ["--jit", jit]
    code, out, err = run(cmd, env={"XYO_DUMPVARS": "1"})
    return code, out.decode("utf-8", "surrogatepass"), err.decode("utf-8", "replace")


def xyo_out_list(text):
    for line in text.split("\n"):
        m = LIST_RE.match(line)
        if m:
            n = int(m.group(1))
            body = m.group(2).strip()
            return body.split(" ") if body else []
    return None


def main():
    xyob = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build/xyo")
    bad = 0
    for name in ("order_flag", "order_broadcast"):
        sb3 = os.path.join(PROBE_DIR, name + ".sb3")
        scode, sdata, serr = scratch(sb3)
        hcode, htext, herr = xyo(xyob, sb3, 1500, None)
        icode, itext, ierr = xyo(xyob, sb3, 1500, "off")
        s = (sdata or {}).get("Stage", {}).get("out")
        h = xyo_out_list(htext)
        i = xyo_out_list(itext)
        print("=" * 74)
        print(name, "scratch=", s)
        print(" " * len(name), "hotspot=", h)
        print(" " * len(name), "interp =", i)
        if not (s == h == i):
            bad += 1
            print("  <-- DIFFERS")

    sb3 = os.path.join(PROBE_DIR, "sensing.sb3")
    labels = json.load(open(os.path.join(PROBE_DIR, "sensing.json")))
    scode, sdata, _ = scratch(sb3)
    hcode, htext, _ = xyo(xyob, sb3, 1500, None)
    icode, itext, _ = xyo(xyob, sb3, 1500, "off")
    svars = (sdata or {}).get("S", {})

    def parse(text):
        vals = {}
        for line in text.split("\n"):
            m = re.match(r"^var S/(p\d+)\[[^\]]*\]=?(.*)$", line)
            if m:
                vals[m.group(1)] = m.group(2)
        return vals
    hv, iv = parse(htext), parse(itext)
    print("=" * 74)
    print("sensing")
    for ent in labels:
        v = ent["var"]
        s, h, i = svars.get(v), hv.get(v), iv.get(v)
        if s == h == i:
            print("  ok   %-28s %s" % (ent["label"], json.dumps(s)))
        else:
            bad += 1
            print("  DIFF %-28s scratch=%s hotspot=%s interp=%s"
                  % (ent["label"], json.dumps(s), json.dumps(h),
                     json.dumps(i)))
    print("=" * 74)
    print("multi: %d differing groups" % bad)
    return 0


if __name__ == "__main__":
    sys.exit(main())
