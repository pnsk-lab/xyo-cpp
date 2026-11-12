#!/usr/bin/env python3
"""No model download: mock a local PLaMo OpenAI server and verify Scratch."""
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import threading
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import Gen, S_set, make_project, num, op, text, write_sb3


def create_fixture(path):
    gen = Gen(["translation", "second", "digits", "viewer", "done"], [])
    gen.script([
        S_set("translation", op("translate_getTranslate",
             WORDS=text("Hello from Scratch"), LANGUAGE=text("Japanese"))),
        S_set("second", op("translate_getTranslate",
             WORDS=text("Hello from Scratch"), LANGUAGE=text("ja"))),
        S_set("digits", op("translate_getTranslate",
             WORDS=text("12345"), LANGUAGE=text("ja"))),
        S_set("viewer", op("translate_getViewerLanguage")),
        S_set("done", num(1)),
    ])
    path.parent.mkdir(parents=True, exist_ok=True)
    write_sb3(path, make_project(gen.vars.keys(), gen.lists.keys(), gen))


class Handler(BaseHTTPRequestHandler):
    calls = []
    malformed = False
    def do_POST(self):
        # Force a real-world async delay. A fast headless runtime must
        # not spend its entire frame budget during network I/O.
        time.sleep(0.08)
        length = int(self.headers["Content-Length"])
        body = json.loads(self.rfile.read(length))
        Handler.calls.append((self.path, body))
        response = (b"{bad json" if Handler.malformed else
                    json.dumps({"choices": [
                        {"message": {"role": "assistant",
                                     "content": "Scratchからこんにちは"}}
                    ]}).encode())
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def log_message(self, *args):
        pass


def execute(binary, project, port, enabled, mode):
    env = dict(os.environ)
    env["XYO_DUMPVARS"] = "json"
    env["XYO_GPU"] = "0"
    env["XYO_PAR"] = "0"
    env["XYO_PLAMO_PORT"] = str(port)
    env["LC_ALL"] = "C"
    if enabled:
        env["XYO_PLAMO_TRANSLATE"] = "1"
    else:
        env.pop("XYO_PLAMO_TRANSLATE", None)
    result = subprocess.run([
        str(binary), "run", str(project), "--jit", mode,
        "--frames", "30"
    ], env=env, cwd=ROOT, capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, (enabled, mode, result.stderr[-1000:])
    return json.loads(result.stdout[result.stdout.index("{"):
                                    result.stdout.rindex("}")+1])["Bench"]


def main():
    binary = Path(sys.argv[1]).resolve()
    output = Path(sys.argv[2]).resolve()
    project = output / "translate-local.sb3"
    create_fixture(project)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        for mode in ("off", "eager", "hotspot"):
            calls_before = len(Handler.calls)
            disabled = execute(binary, project, server.server_port, False, mode)
            assert disabled["translation"] == "Hello from Scratch", disabled
            assert disabled["done"] == "1", disabled
            assert disabled["second"] == "Hello from Scratch", disabled
            assert disabled["digits"] == "12345", disabled
            assert disabled["viewer"] == "English", disabled
            assert len(Handler.calls) == calls_before, "model accessed without opt-in"
            print("PASS disabled", mode, flush=True)
            enabled = execute(binary, project, server.server_port, True, mode)
            assert enabled["translation"] == "Scratchからこんにちは", enabled
            assert enabled["done"] == "1", enabled
            assert enabled["second"] == "Scratchからこんにちは", enabled
            assert enabled["digits"] == "12345", enabled
            assert len(Handler.calls) == calls_before + 1, (
                mode, "cache miss", len(Handler.calls), calls_before)
            assert Handler.calls[-1][0] == "/v1/chat/completions"
            body = Handler.calls[-1][1]
            assert body["model"] == "plamo-3-translate", body
            assert body["messages"][0]["role"] == "system", body
            assert body["messages"][1]["role"] == "user", body
            assert body["messages"][1]["content"] == "Hello from Scratch"
            assert "target_lang=ja" in body["messages"][0]["content"]
            # A malformed endpoint cannot send data outside loopback.
            rejected = execute(binary, project, 0, True, mode)
            assert rejected["translation"] == "Hello from Scratch", rejected
            assert rejected["done"] == "1", rejected
            assert len(Handler.calls) == calls_before + 1
            # A server may return HTTP 200 with invalid JSON. This must
            # not unwind through std::future::get() into the runtime.
            Handler.malformed = True
            try:
                malformed = execute(binary, project, server.server_port,
                                    True, mode)
                assert malformed["translation"] == "Hello from Scratch", (
                    mode, malformed)
                assert malformed["done"] == "1", (mode, malformed)
            finally:
                Handler.malformed = False
            print("PASS local mock, invalid port and malformed JSON",
                  mode, flush=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
