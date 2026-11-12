#!/usr/bin/env python3
"""Native LLVM translation Fiber must retire if another hat stops all."""
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import Gen, S_set, num, op, text, make_project, write_sb3


def fixture(path):
    gen = Gen(["translated", "after", "stopped"], [])
    # First script pauses in a native reporter waiting for HTTP.
    gen.script([
        S_set("translated", op("translate_getTranslate",
                              WORDS=text("never complete"),
                              LANGUAGE=text("ja"))),
        S_set("after", num(1)),
    ], x=0)
    # Second hat requests stop-all while the reporter's Fiber is parked.
    hat = gen.script([], x=550)
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0.04), wait)
    gen.blocks[hat]["next"] = wait
    mark = gen.emit_stmt(S_set("stopped", num(1)), wait)
    gen.blocks[wait]["next"] = mark
    end = gen.new("control_stop", parent=mark,
                  fields={"STOP_OPTION": ["all", None]})
    gen.blocks[mark]["next"] = end
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


class Handler(BaseHTTPRequestHandler):
    requests = 0

    def do_POST(self):
        size = int(self.headers["Content-Length"])
        self.rfile.read(size)
        Handler.requests += 1
        time.sleep(2.0)
        payload = json.dumps({"choices": [{"message": {"content": "late"}}]}).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        pass


def main():
    binary = Path(sys.argv[1]).resolve()
    project = Path(sys.argv[2]).resolve() / "native-stop-all.sb3"
    fixture(project)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for mode in ("off", "eager"):
            Handler.requests = 0
            env = dict(os.environ, XYO_JIT_PLAMO_FIBER="1",
               XYO_RT_REQUIRE_GENERATED_SSA_ROOTS="1",
                       XYO_PLAMO_TRANSLATE="1", XYO_JIT_VERBOSE="1",
                       XYO_PLAMO_PORT=str(server.server_port),
                       XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0")
            started = time.monotonic()
            proc = subprocess.run([str(binary), "run", str(project),
                                   "--jit", mode, "--frames", "100",
                                   "--fps", "60"], cwd=ROOT, env=env,
                                  capture_output=True, text=True, timeout=8)
            elapsed = time.monotonic() - started
            assert proc.returncode == 0, (mode, proc.stderr[-1200:])
            values = json.loads(proc.stdout[proc.stdout.index("{"):
                                                 proc.stdout.rindex("}")+1])
            state = values["Bench"]
            assert state["after"] == "0", (mode, state)
            assert state["translated"] == "0", (mode, state)
            assert state["stopped"] == "1", (mode, state)
            assert elapsed < 1.5, (mode, elapsed)
            if mode == "eager":
                assert "xyo-jit: compiled" in proc.stderr, proc.stderr[-900:]
            print(f"PASS native stop-all {mode}: {elapsed:.3f}s "
                  f"requests={Handler.requests}", flush=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
