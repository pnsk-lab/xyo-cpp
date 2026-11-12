#!/usr/bin/env python3
"""Delayed PLaMo stub for stop/restart/target-stop/stopAll cancellation."""
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import threading
import time


class Handler(BaseHTTPRequestHandler):
    count = 0
    def do_POST(self):
        Handler.count += 1
        length = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(length)
        time.sleep(3.0 if os.environ.get("XYO_PLAMO_STALLED") == "1"
                   else 0.12)
        payload = json.dumps({"choices": [
            {"message": {"content": "outdated translation"}}]}).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def log_message(self, *_):
        return


def main():
    binary = str(Path(sys.argv[1]).resolve())
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    try:
        env = os.environ.copy()
        env.update(XYO_PLAMO_TRANSLATE="1", XYO_PLAMO_PORT=str(server.server_port))
        started = time.monotonic()
        result = subprocess.run([binary], env=env, capture_output=True,
                                text=True, timeout=12)
        elapsed = time.monotonic() - started
        print(result.stdout, end="")
        assert result.returncode == 0, result.stderr
        assert result.stdout.count("PASS ") == 7, result.stdout
        assert Handler.count > 0, "HTTP transport never exercised"
        if os.environ.get("XYO_PLAMO_STALLED") == "1":
            # The server deliberately holds requests for 3 seconds.
            # Cancellation must not defer Runtime shutdown until timeout.
            assert elapsed < 2.5, f"cancel stalled HTTP took {elapsed:.2f}s"
        print(f"PASS delayed localhost cancellation "
              f"(requests={Handler.count}, client={elapsed:.2f}s)")
    finally:
        server.shutdown()
        server.server_close()


if __name__ == "__main__":
    main()
