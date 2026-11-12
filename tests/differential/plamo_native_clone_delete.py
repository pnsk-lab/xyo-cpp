#!/usr/bin/env python3
"""Delete clones while their native LLVM reporter Fibers await localhost HTTP."""
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
from gen_algo_bench import Gen, S_set, S_change, num, op, text, make_project, write_sb3

CLONES = 8

def fixture(path):
    gen = Gen(["created", "deleted", "after", "translated"], [])
    root = gen.script([S_set("created", num(0)), S_set("deleted", num(0)),
                       S_set("after", num(0))])
    tail = root
    while gen.blocks[tail]["next"]:
        tail = gen.blocks[tail]["next"]
    for _ in range(CLONES):
        clone = gen.new("control_create_clone_of", parent=tail)
        menu = gen.new("control_create_clone_of_menu", shadow=True,
                       parent=clone, fields={"CLONE_OPTION": ["_myself_", None]})
        gen.blocks[clone]["inputs"]["CLONE_OPTION"] = [1, menu]
        gen.blocks[tail]["next"] = clone
        tail = clone

    # Reporter path: should be destroyed before it produces a value.
    h1 = gen.new("control_start_as_clone", top=True, x=500)
    reporter = gen.emit_stmt(S_set("translated",
        op("translate_getTranslate", WORDS=text("delete pending"),
           LANGUAGE=text("ja"))), h1)
    gen.blocks[h1]["next"] = reporter
    after = gen.emit_stmt(S_change("after", num(1)), reporter)
    gen.blocks[reporter]["next"] = after

    # Another hat of the SAME clone deletes it while h1 is suspended.
    h2 = gen.new("control_start_as_clone", top=True, x=800)
    created = gen.emit_stmt(S_change("created", num(1)), h2)
    gen.blocks[h2]["next"] = created
    wait = gen.new("control_wait", parent=created)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0.06), wait)
    gen.blocks[created]["next"] = wait
    deleted = gen.emit_stmt(S_change("deleted", num(1)), wait)
    gen.blocks[wait]["next"] = deleted
    drop = gen.new("control_delete_this_clone", parent=deleted)
    gen.blocks[deleted]["next"] = drop

    project = make_project(gen.vars.keys(), [], gen)
    stage, sprite = project["targets"]
    for key in ("created", "deleted", "after"):
        stage["variables"][gen.vars[key]] = [key, 0]
        del sprite["variables"][gen.vars[key]]
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(str(path), project)


class Handler(BaseHTTPRequestHandler):
    requests = 0
    def do_POST(self):
        size = int(self.headers["Content-Length"])
        self.rfile.read(size)
        Handler.requests += 1
        time.sleep(2.0)
        response = json.dumps({"choices": [
            {"message": {"content": "should not apply"}}]}).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(response)))
            self.end_headers()
            self.wfile.write(response)
        except (BrokenPipeError, ConnectionResetError):
            pass
    def log_message(self, *_):
        pass


def main():
    binary = Path(sys.argv[1]).resolve()
    path = Path(sys.argv[2]).resolve() / "native-delete-clone.sb3"
    fixture(path)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for mode in ("off", "eager"):
            Handler.requests = 0
            env = dict(os.environ, XYO_PLAMO_TRANSLATE="1",
                       XYO_JIT_PLAMO_FIBER="1",
               XYO_RT_REQUIRE_GENERATED_SSA_ROOTS="1", XYO_JIT_VERBOSE="1",
                       XYO_PLAMO_PORT=str(server.server_port),
                       XYO_DUMPVARS="json", XYO_GPU="0", XYO_PAR="0")
            start = time.monotonic()
            proc = subprocess.run([str(binary), "run", str(path),
                                   "--jit", mode, "--frames", "200",
                                   "--fps", "60"], cwd=ROOT, env=env,
                                  capture_output=True, text=True, timeout=8)
            elapsed = time.monotonic() - start
            assert proc.returncode == 0, (mode, proc.stderr[-1200:])
            parsed = json.loads(proc.stdout[proc.stdout.index("{"):
                                               proc.stdout.rindex("}")+1])
            state = parsed["Stage"]
            assert state["created"] == str(CLONES), (mode, state)
            assert state["deleted"] == str(CLONES), (mode, state)
            # Four clones acquire in-flight translation slots. Four
            # excess clones receive immediate original-string fallback,
            # which legitimately executes the next block before deletion.
            assert state["after"] == "4", (mode, state)
            assert Handler.requests == 4, (mode, Handler.requests)
            assert elapsed < 1.5, (mode, elapsed)
            if mode == "eager":
                assert "xyo-jit: compiled" in proc.stderr, proc.stderr[-900:]
            print(f"PASS native clone deletion {mode}: "
                  f"created={state['created']} deleted={state['deleted']} "
                  f"after={state['after']} requests={Handler.requests} "
                  f"elapsed={elapsed:.3f}s", flush=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
