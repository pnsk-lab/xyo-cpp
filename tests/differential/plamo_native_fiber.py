#!/usr/bin/env python3
"""Opt-in native LLVM Fiber PLaMo reporter continuation test, no model weights."""
import json, os, subprocess, sys, threading, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from translate_plamo_local import create_fixture as basic_fixture
from plamo_jit_procedure_gate import fixture as nested_fixture
from plamo_clone_stress import fixture as clone_fixture
sys.path.insert(0, str(ROOT / "tests/bench/algos"))
from gen_algo_bench import Gen, S_call, S_change, S_repeat, S_set, join, num, op, text, var, make_project, write_sb3


def expression_fixture(path):
    # Keep a runtime-generated (not constant-folded) string alive as an
    # LLVM value across the asynchronous reporter's Fiber suspension.
    gen = Gen(["source", "result", "done"], [])
    gen.script([
        S_set("source", join(text("start"), text("abcdef"))),
        S_set("result", join(
            join(var("source"), text("|")),
            op("translate_getTranslate", WORDS=text("Hello from Scratch"),
               LANGUAGE=text("ja")))),
        S_set("done", num(1)),
    ])
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


def string_churn_fixture(path):
    # Hundreds of distinct Scratch-created strings before a native
    # asynchronous reporter; the large value is used after Fiber resume.
    gen = Gen(["acc", "i", "result", "done"], [])
    gen.script([
        S_set("acc", text("")),
        S_set("i", num(0)),
        S_repeat(num(400), [
            S_change("i", num(1)),
            S_set("acc", join(var("acc"),
                              join(text("|"), var("i")))),
        ]),
        S_set("result", join(
            var("acc"),
            op("translate_getTranslate",
               WORDS=text("Hello from Scratch"),
               LANGUAGE=text("ja")))),
        S_set("done", num(1)),
    ])
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


def mixed_sequential_reporters_fixture(path):
    # A Variable::value string is live at BOTH reporter call sites; the
    # first reporter return is additionally live at the second site.
    # Rewriting the first call must not leave stale LLVM Value* references
    # in the frozen second checkpoint analysis.
    gen = Gen(["prefix", "result", "done"], [])
    gen.script([
        S_set("prefix", join(text("prefix"), text("-old"))),
        S_set("result", join(
            var("prefix"),
            join(op("translate_getTranslate",
                    WORDS=text("Hello from Scratch"), LANGUAGE=text("ja")),
                 op("translate_getTranslate",
                    WORDS=text("nested procedure"), LANGUAGE=text("ja"))))),
        S_set("done", num(1)),
    ], x=0)
    hat = gen.script([], x=500)
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0.02), wait)
    gen.blocks[hat]["next"] = wait
    replace = gen.emit_stmt(S_set("prefix", text("prefix-new")), wait)
    gen.blocks[wait]["next"] = replace
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


def lazy_root_fixture(path):
    # Explicit lazy mode requires >=2 native functions. Keep the async
    # reporter in the entry function with a live boxed prefix value.
    gen = Gen(["prefix", "result", "done", "precheck"], [])
    gen.define("noop", [], [S_set("precheck", num(1))], warp=False)
    gen.script([
        S_call("noop"),
        S_set("prefix", join(text("hello"), text("world"))),
        S_set("result", join(
            var("prefix"),
            op("translate_getTranslate",
               WORDS=text("Hello from Scratch"), LANGUAGE=text("ja")))),
        S_set("done", num(1)),
    ], x=0)
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


def sequential_reporters_fixture(path):
    # The first async reporter's boxed Value remains live while a
    # second reporter suspends the very same native LLVM activation.
    gen = Gen(["result", "done"], [])
    gen.script([
        S_set("result", join(
            op("translate_getTranslate",
               WORDS=text("Hello from Scratch"), LANGUAGE=text("ja")),
            op("translate_getTranslate",
               WORDS=text("nested procedure"), LANGUAGE=text("ja")))),
        S_set("done", num(1)),
    ])
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


def multi_value_mutation_fixture(path):
    # Two independently loaded Variable::value words must both survive
    # a native async reporter. A later hat overwrites both mutable
    # slots; the unmodified SSA copies are used after the yield.
    gen = Gen(["left", "right", "result", "done"], [])
    gen.script([
        S_set("left", join(text("LEFT"), text("-original"))),
        S_set("right", join(text("RIGHT"), text("-original"))),
        S_set("result", join(
            var("left"),
            join(var("right"),
                 op("translate_getTranslate",
                    WORDS=text("Hello from Scratch"),
                    LANGUAGE=text("ja"))))),
        S_set("done", num(1)),
    ], x=0)
    hat = gen.script([], x=500)
    delay = gen.new("control_wait", parent=hat)
    gen.blocks[delay]["inputs"]["DURATION"] = gen.emit_value(num(0.02), delay)
    gen.blocks[hat]["next"] = delay
    overwrite1 = gen.emit_stmt(S_set("left", text("LEFT-replaced")), delay)
    gen.blocks[delay]["next"] = overwrite1
    overwrite2 = gen.emit_stmt(S_set("right", text("RIGHT-replaced")),
                              overwrite1)
    gen.blocks[overwrite1]["next"] = overwrite2
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


def variable_mutation_fixture(path):
    # A competing Scratch hat replaces the source variable while the
    # LLVM reporter keeps its OLD value across a native Fiber suspension.
    gen = Gen(["source", "result", "done"], [])
    gen.script([
        S_set("source", join(text("before"), text("waiting"))),
        S_set("result", join(
            var("source"),
            op("translate_getTranslate",
               WORDS=text("Hello from Scratch"), LANGUAGE=text("ja")))),
        S_set("done", num(1)),
    ], x=0)
    hat = gen.script([], x=500)
    wait = gen.new("control_wait", parent=hat)
    gen.blocks[wait]["inputs"]["DURATION"] = gen.emit_value(num(0.02), wait)
    gen.blocks[hat]["next"] = wait
    overwrite = gen.emit_stmt(S_set("source", text("changed")), wait)
    gen.blocks[wait]["next"] = overwrite
    write_sb3(str(path), make_project(gen.vars.keys(), [], gen))


class Handler(BaseHTTPRequestHandler):
    calls = []
    lock = threading.Lock()
    active = 0
    peak = 0
    def do_POST(self):
        size = int(self.headers["Content-Length"])
        data = json.loads(self.rfile.read(size))
        source = data["messages"][1]["content"]
        with self.lock:
            Handler.calls.append(source)
            Handler.active += 1
            Handler.peak = max(Handler.peak, Handler.active)
        time.sleep(0.08)
        translated = {"Hello from Scratch": "Scratchからこんにちは",
                      "nested procedure": "入れ子の手続き",
                      "clone hello": "完了"}.get(source, source)
        payload = json.dumps({"choices": [{"message": {"content": translated}}]}).encode()
        try:
            self.send_response(200)
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with self.lock:
                Handler.active -= 1
    def log_message(self, *_): pass

def check(binary, project, port, expected, required_logs,
          guard_vars=False, expected_i64=None, root_live_vars=False):
    env = dict(os.environ, XYO_JIT_PLAMO_FIBER="1",
               XYO_RT_REQUIRE_GENERATED_SSA_ROOTS="1",
               XYO_JIT_PLAMO_SSA_AUDIT="1",
               XYO_JIT_VERBOSE="1", XYO_PLAMO_TRANSLATE="1",
               XYO_PLAMO_PORT=str(port), XYO_DUMPVARS="json",
               XYO_GPU="0", XYO_PAR="0")
    if root_live_vars:
        env["XYO_JIT_PLAMO_ROOT_LIVE_VARS"] = "1"
        env["XYO_RT_TRACE_ASYNC_VAR_ROOTS"] = "1"
        env["XYO_RT_POSTOPT_ASYNC_ROOT_AUDIT"] = "1"
    else:
        env.pop("XYO_JIT_PLAMO_ROOT_LIVE_VARS", None)
        env.pop("XYO_RT_TRACE_ASYNC_VAR_ROOTS", None)
        env.pop("XYO_RT_POSTOPT_ASYNC_ROOT_AUDIT", None)
    if guard_vars:
        env["XYO_RT_GUARD_NATIVE_VAR_SLOTS"] = "1"
    else:
        env.pop("XYO_RT_GUARD_NATIVE_VAR_SLOTS", None)
    proc = subprocess.run([str(binary), "run", str(project),
                           "--jit", "eager", "--frames", "500"],
                          cwd=ROOT, env=env, capture_output=True,
                          text=True, timeout=20)
    assert proc.returncode == 0, proc.stderr[-1600:]
    values = json.loads(proc.stdout[proc.stdout.index("{"):
                                         proc.stdout.rindex("}")+1])
    state = values["Stage"] if "created" in expected else values["Bench"]
    for key, expected_value in expected.items():
        assert state[key] == expected_value, (project.name, key, state)
    for entry in required_logs:
        assert entry in proc.stderr, (project.name, entry, proc.stderr[-1600:])
    if root_live_vars:
        compiler_roots = [line for line in proc.stderr.splitlines()
                          if line.startswith(
                              "[plamo-async-var-root-codegen]")]
        runtime_roots = [line for line in proc.stderr.splitlines()
                         if line.startswith("[native-async-var-root]")]
        assert compiler_roots and runtime_roots, (
            project.name, compiler_roots, runtime_roots, proc.stderr[-2000:])
        expected_counts = (
            (1, 2) if project.name == "native-mixed-reporters.sb3"
            else (2,) if project.name == "native-multi-mutation.sb3"
            else (1,)
        )
        for expected_roots in expected_counts:
            assert any(f"words={expected_roots} gc_safe=0" in line
                       for line in compiler_roots), (
                           project.name, expected_roots, compiler_roots)
            assert any(
                f"count={expected_roots} rooted={expected_roots} "
                "immediate=0 valid=1 gc_safe=0" in line
                for line in runtime_roots), (
                    project.name, expected_roots, runtime_roots)
        waits = [line for line in proc.stderr.splitlines()
                 if line.startswith("[native-async-var-wait]")]
        releases = [line for line in proc.stderr.splitlines()
                    if line.startswith("[native-async-var-release]")]
        for expected_roots in expected_counts:
            assert any(
                f"owners=1 heap_roots={expected_roots} gc_safe=0" in line
                for line in waits), (project.name, expected_roots, waits)
        assert any("owners=0 gc_safe=0" in line
                   for line in releases), (project.name, releases)
        optimized = [
            line for line in proc.stderr.splitlines()
            if line.startswith("[rt001-postopt-async-root] ")
        ]
        assert optimized, (project.name, proc.stderr[-2400:])
        if project.name == "native-lazy-root.sb3":
            assert all("tier=lazy" in line for line in optimized), (
                project.name, optimized)
        postopt_counts = []
        for line in optimized:
            data = dict(part.split("=", 1) for part in line.split()
                        if "=" in part)
            assert (
                data["valid"] == "1" and
                data["signature"] == "1" and
                data["buffer"] == "1" and
                data["stores"] == "1" and
                data["covered"] == "1" and
                data["machine_roots_proven"] == "0" and
                data["gc_safe"] == "0" and
                int(data["words"]) == int(data["filled"]) and
                0 <= int(data["covered_i64"]) <= int(data["live_i64"])
            ), (project.name, line)
            postopt_counts.append(int(data["words"]))
        assert all(n in postopt_counts for n in expected_counts), (
            project.name, expected_counts, postopt_counts)
        if project.name in (
            "native-mixed-reporters.sb3", "native-sequential-reporters.sb3"
        ):
            assert any(
                int(dict(field.split("=", 1) for field in line.split()
                         if "=" in field)["live_i64"]) == 1 and
                int(dict(field.split("=", 1) for field in line.split()
                         if "=" in field)["covered_i64"]) == 1
                for line in optimized
            ), (project.name, optimized)
        print(f"PASS post-O3 root buffer {project.name}: "
              f"sites={len(optimized)} counts={postopt_counts} "
              "machine_roots_proven=0 gc_safe=0", flush=True)
        print(f"PASS pre-wait GC roots {project.name}: "
              f"{runtime_roots[0]}; wait and release verified", flush=True)
    audits = [line for line in proc.stderr.splitlines()
              if line.startswith("[plamo-ssa-audit]")]
    assert audits, (project.name, proc.stderr[-2000:])
    assert all("gc_safe=0" in line and "classification_complete=" in line
               for line in audits), (project.name, audits)
    i64_reports = [line for line in proc.stderr.splitlines()
                   if line.startswith("[plamo-ssa-i64]")]
    assert len(i64_reports) == len(audits), (project.name, i64_reports, audits)
    for line in i64_reports:
        fields = dict(part.split("=", 1) for part in line.split()
                      if "=" in part)
        assert (
            int(fields["total"]) ==
            int(fields["arg_alias"]) +
            int(fields["generated_string"]) +
            int(fields["var_slot"]) +
            int(fields["var_checked"]) +
            int(fields["reporter"]) +
            int(fields["unclassified"]) and
            fields["complete"] == "1" and fields["gc_safe"] == "0"
        ), (project.name, line)
    if expected_i64:
        def matches(line):
            fields = dict(part.split("=", 1) for part in line.split()
                          if "=" in part)
            return all(int(fields[key]) == value
                       for key, value in expected_i64.items())
        assert any(matches(line) for line in i64_reports), (
            project.name, expected_i64, i64_reports)
    print(f"PASS live i64 provenance {project.name}: "
          f"{i64_reports[0]}", flush=True)
    unknown = [line for line in proc.stderr.splitlines()
               if line.startswith("[plamo-ssa-unknown]")]
    assert len(unknown) == len(audits), (project.name, audits, unknown)
    for census, causes in zip(audits, unknown):
        counts = dict(part.split("=", 1) for part in causes.split()[2:]
                      if "=" in part)
        total = sum(int(counts[name]) for name in (
            "load_stack", "load_host", "load_static", "load_unknown",
            "call_return", "addr_cast", "foreign_arg", "phi_select",
            "other"))
        unknown_count = int(census.split("ptr_unknown=")[1].split()[0])
        assert total == unknown_count, (project.name, total, unknown_count)
    chains = [line for line in proc.stderr.splitlines()
              if line.startswith("[plamo-ssa-load-path]")]
    summaries = [line for line in proc.stderr.splitlines()
                 if line.startswith("[plamo-ssa-load-summary]")]
    assert len(summaries) == len(audits), (project.name, summaries, audits)
    total_loads = sum(
        sum(int(part.split("=", 1)[1]) for part in line.split()
            if part.startswith(("load_stack=", "load_host=",
                                "load_static=", "load_unknown=")))
        for line in unknown
    )
    total_chains = sum(int(line.split("total=")[1].split()[0])
                       for line in summaries)
    assert total_loads == total_chains, (project.name, unknown, summaries)
    if guard_vars:
        # Current fixture uses only Variable slot chains; guarded access
        # resolves the owner at the use site instead of retaining the
        # Variable* as live native SSA across the async reporter.
        assert total_chains == 0, (project.name, summaries)
        assert all("ptr_unknown=0" in line for line in audits), (
            project.name, audits)
    assert len(chains) == sum(min(
        int(line.split("total=")[1].split()[0]), 16)
        for line in summaries), (project.name, chains, summaries)
    assert all("derefs=" in line and "terminal=" in line
               and "ctx_field=" in line
               for line in chains), (project.name, chains)
    assert all("gc_safe=0" in line and "max_derefs=" in line
               and "host_offsets_known=" in line for line in summaries)
    for line in summaries:
        fields = dict(part.split("=", 1) for part in line.split()
                      if "=" in part)
        assert sum(int(fields[name]) for name in (
            "terminal_host", "terminal_stack", "terminal_other")) == (
                int(fields["total"])), line
        assert int(fields["host_offsets_known"]) <= int(fields["total"])
        assert int(fields["max_derefs"]) > 0 if int(fields["total"]) else (
            int(fields["max_derefs"]) == 0)
    print(f"PASS SSA load-chain census {project.name}: "
          f"chains={total_chains} {summaries[0]}", flush=True)
    if chains:
        from collections import Counter
        origins = Counter(
            line.split("ctx_field=")[1].split()[0]
            for line in chains)
        print(f"PASS SSA path origins {project.name}: "
              f"{dict(origins)}", flush=True)
        print(f"PASS SSA path sample {project.name}: {chains[0]}",
              flush=True)
    print(f"PASS SSA census {project.name}: {audits[0]} "
          f"{unknown[0]}", flush=True)
    return state

def main():
    binary = Path(sys.argv[1]).resolve()
    base = Path(sys.argv[2]).resolve()
    base.mkdir(parents=True, exist_ok=True)
    churn = base / "native-churn.sb3"
    mutated = base / "native-mutation.sb3"
    multi_mutated = base / "native-multi-mutation.sb3"
    sequential = base / "native-sequential-reporters.sb3"
    lazy_root = base / "native-lazy-root.sb3"
    mixed_seq = base / "native-mixed-reporters.sb3"
    basic, nested, clones, expression = (
        base / "native-basic.sb3",
        base / "native-nested.sb3",
        base / "native-clones.sb3",
        base / "native-expression.sb3",
    )
    basic_fixture(basic)
    nested_fixture(nested)
    clone_fixture(clones)
    expression_fixture(expression)
    string_churn_fixture(churn)
    variable_mutation_fixture(mutated)
    multi_value_mutation_fixture(multi_mutated)
    sequential_reporters_fixture(sequential)
    lazy_root_fixture(lazy_root)
    mixed_sequential_reporters_fixture(mixed_seq)
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        check(binary, basic, server.server_port,
              {"translation": "Scratchからこんにちは",
               "second": "Scratchからこんにちは", "done": "1"},
              ["xyo-jit: compiled "])
        print("PASS native LLVM reporter direct async", flush=True)
        check(binary, basic, server.server_port,
              {"translation": "Scratchからこんにちは",
               "second": "Scratchからこんにちは", "done": "1"},
              ["xyo-jit: compiled "], guard_vars=True)
        print("PASS native reporter guarded variable slots", flush=True)
        check(binary, nested, server.server_port,
              {"translation": "入れ子の手続き", "done": "1",
               "plain": "1000", "plainDone": "1"},
              ["xyo-jit: frame keep (yield) inner", "xyo-jit: compiled "])
        print("PASS native LLVM reporter nested procedure", flush=True)
        check(binary, expression, server.server_port,
              {"source": "startabcdef",
               "result": "startabcdef|Scratchからこんにちは",
               "done": "1"},
              ["xyo-jit: compiled "],
              expected_i64={"total": 1, "generated_string": 1})
        check(binary, expression, server.server_port,
              {"source": "startabcdef",
               "result": "startabcdef|Scratchからこんにちは",
               "done": "1"},
              ["xyo-jit: compiled "],
              expected_i64={"total": 1, "generated_string": 1},
              root_live_vars=True)
        print("PASS native LLVM string expression survives async yield",
              flush=True)
        expected_string = "".join(f"|{i}" for i in range(1, 401))
        check(binary, churn, server.server_port,
              {"acc": expected_string,
               "i": "400",
               "result": expected_string + "Scratchからこんにちは",
               "done": "1"},
              ["xyo-jit: compiled "],
              expected_i64={"total": 1, "var_slot": 1, "unclassified": 0})
        print("PASS 400 distinct Scratch strings survive native async yield",
              flush=True)
        check(binary, churn, server.server_port,
              {"acc": expected_string,
               "i": "400",
               "result": expected_string + "Scratchからこんにちは",
               "done": "1"},
              ["xyo-jit: compiled "], guard_vars=True,
              expected_i64={"total": 1, "var_checked": 1, "unclassified": 0})
        print("PASS native string churn with guarded variable slots",
              flush=True)
        check(binary, churn, server.server_port,
              {"acc": expected_string,
               "i": "400",
               "result": expected_string + "Scratchからこんにちは",
               "done": "1"},
              ["xyo-jit: compiled "],
              expected_i64={"total": 1, "var_slot": 1, "unclassified": 0},
              root_live_vars=True)
        check(binary, churn, server.server_port,
              {"acc": expected_string,
               "i": "400",
               "result": expected_string + "Scratchからこんにちは",
               "done": "1"},
              ["xyo-jit: compiled "], guard_vars=True,
              expected_i64={"total": 1, "var_checked": 1,
                            "unclassified": 0},
              root_live_vars=True)
        print("PASS native live Variable::value roots across async Fiber "
              "suspension in both variable modes", flush=True)
        for guarded in (False, True):
            check(binary, mutated, server.server_port,
                  {"source": "changed",
                   "result": "beforewaitingScratchからこんにちは",
                   "done": "1"},
                  ["xyo-jit: compiled "], guard_vars=guarded,
                  expected_i64={"total": 1, "unclassified": 0,
                                "var_checked" if guarded else "var_slot": 1},
                  root_live_vars=True)
        print("PASS competing script overwrites Variable::value while "
              "native async reporter keeps its old SSA string", flush=True)
        for guarded in (False, True):
            check(binary, multi_mutated, server.server_port,
                  {"left": "LEFT-replaced",
                   "right": "RIGHT-replaced",
                   "result": "LEFT-originalRIGHT-originalScratchからこんにちは",
                   "done": "1"},
                  ["xyo-jit: compiled "], guard_vars=guarded,
                  root_live_vars=True)
        print("PASS two independent overwritten Variable::value roots "
              "survive native translation suspension", flush=True)
        check(binary, sequential, server.server_port,
              {"result": "Scratchからこんにちは入れ子の手続き",
               "done": "1"}, ["xyo-jit: compiled "])
        check(binary, sequential, server.server_port,
              {"result": "Scratchからこんにちは入れ子の手続き",
               "done": "1"}, ["xyo-jit: compiled "],
              expected_i64={"total": 1, "reporter": 1, "unclassified": 0},
              root_live_vars=True)
        print("PASS two sequential native async reporters; "
              "first result survives second suspension", flush=True)
        for guarded in (False, True):
            check(binary, mixed_seq, server.server_port,
                  {"prefix": "prefix-new",
                   "result": "prefix-oldScratchからこんにちは入れ子の手続き",
                   "done": "1"}, ["xyo-jit: compiled "],
                  guard_vars=guarded, root_live_vars=True,
                  expected_i64={"total": 2, "reporter": 1,
                    "var_checked" if guarded else "var_slot": 1,
                    "unclassified": 0})
        print("PASS frozen SSA references across multiple rewritten "
              "reporter sites and concurrent variable mutation", flush=True)
        # Check the independent ORC lazy-transform O3 path, not just
        # eager JIT with the word "lazy" in its environment.
        previous_lazy = os.environ.get("XYO_JIT_LAZY_MODE")
        try:
            os.environ["XYO_JIT_LAZY_MODE"] = "on"
            check(binary, lazy_root, server.server_port,
                  {"prefix": "helloworld",
                   "result": "helloworldScratchからこんにちは",
                   "precheck": "1", "done": "1"},
                  ["xyo-jit: compiled "],
                  expected_i64={"total": 1, "unclassified": 0},
                  root_live_vars=True)
        finally:
            if previous_lazy is None:
                os.environ.pop("XYO_JIT_LAZY_MODE", None)
            else:
                os.environ["XYO_JIT_LAZY_MODE"] = previous_lazy
        print("PASS ORC lazy O3 native async-root compiler and Fiber",
              flush=True)
        check(binary, clones, server.server_port,
              {"created": "32", "completed": "32",
               "translated": "4", "fallback": "28", "done": "1"},
              ["xyo-jit: compiled "])
        cloneRequests = Handler.calls.count("clone hello")
        assert cloneRequests == 4, (cloneRequests, Handler.calls)
        assert Handler.peak == 4, Handler.peak
        print(f"PASS native LLVM reporter 32 clones; requests={cloneRequests}, "
              f"peak HTTP={Handler.peak}", flush=True)
    finally:
        server.shutdown()
        server.server_close()

if __name__ == "__main__":
    main()
