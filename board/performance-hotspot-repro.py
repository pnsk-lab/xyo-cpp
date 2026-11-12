#!/usr/bin/env python3
"""Run the exact scratch-bench performance workload until its DONE marker."""
import argparse
import os
from pathlib import Path
import selectors
import subprocess
import sys
import time

p = argparse.ArgumentParser()
p.add_argument("--binary", required=True)
p.add_argument("--project", required=True)
p.add_argument("--lazy", choices=["auto", "on", "off"], default="auto")
p.add_argument("--parfor", choices=["on", "off"], default="on")
p.add_argument("--out", required=True)
p.add_argument("--limit", type=float, default=52.0)
a = p.parse_args()
env = dict(os.environ)
env["XYO_JIT_MODE"] = "hotspot"
env["XYO_JIT_LAZY_MODE"] = a.lazy
env["XYO_PARFOR"] = "0" if a.parfor == "off" else "1"
cmd = [str(Path(a.binary).resolve()), "run",
       str(Path(a.project).resolve()), "--print-list", "output", "--fps", "30"]
out = Path(a.out)
out.parent.mkdir(parents=True, exist_ok=True)
start = time.monotonic()
with out.open("w") as log:
    log.write(f"CMD={cmd} LAZY={a.lazy} PARFOR={a.parfor}\n")
    proc = subprocess.Popen(
        cmd, cwd=str(Path(a.binary).resolve().parent),
        env=env, stdout=subprocess.PIPE, stderr=log,
        bufsize=0, start_new_session=True,
    )
    ok = False
    lines = 0
    pending = b""
    import os as _os
    import signal
    try:
        sel = selectors.DefaultSelector()
        sel.register(proc.stdout, selectors.EVENT_READ)
        while time.monotonic() - start < a.limit:
            ready = sel.select(timeout=0.3)
            for key, _ in ready:
                chunk = _os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    sel.unregister(key.fileobj)
                    continue
                pending += chunk
                while b"\n" in pending:
                    line, pending = pending.split(b"\n", 1)
                    lines += 1
                    if line.strip() == b"DONE":
                        ok = True
                        break
            if ok or not sel.get_map(): break
            if proc.poll() is not None and not ready: break
    finally:
        if proc.poll() is None:
            _os.killpg(proc.pid, signal.SIGTERM)
            try: proc.wait(timeout=3)
            except subprocess.TimeoutExpired: _os.killpg(proc.pid, signal.SIGKILL)
        if proc.stdout: proc.stdout.close()
    result = f"DONE={ok} EXIT={proc.returncode} LINES={lines} SECONDS={time.monotonic()-start:.2f}"
    log.write(result + "\n")
print(result)
sys.exit(0 if ok else 1)
