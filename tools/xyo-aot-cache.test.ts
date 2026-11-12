import { afterEach, expect, test } from "bun:test";
import * as fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { spawnSync } from "node:child_process";
import {
  cachePaths,
  parseArgs,
  validObject,
} from "./xyo-aot-cache.ts";

const roots: string[] = [];
afterEach(() => {
  for (const root of roots.splice(0)) fs.rmSync(root, { recursive: true, force: true });
});

function fixture() {
  const base = path.join(os.homedir(), ".cache");
  fs.mkdirSync(base, { recursive: true });
  const root = fs.mkdtempSync(path.join(base, "xyo-aot-cache-test-"));
  roots.push(root);
  const binary = path.join(root, "fake-xyo");
  const project = path.join(root, "test.sb3");
  const cacheRoot = path.join(root, "aot-cache");
  const launcher = path.resolve("tools/xyo-aot-cache.ts");
  fs.writeFileSync(project, "project version 1");
  fs.writeFileSync(binary, [
    "#!/bin/sh",
    "if [ \"$1\" = \"aot\" ]; then",
    "  if [ \"$FAKE_AOT_FAIL\" = \"1\" ]; then exit 7; fi",
    "  printf 'valid object data' > \"$4\"",
    "  exit 0",
    "fi",
    "printf 'FAKE_ARGS:%s\\n' \"$*\"",
    "case \"$*\" in",
    "  *'--aot '*)",
    "    if [ \"$FAKE_AOT_BAD_LOAD\" = \"1\" ]; then",
    "      echo 'aot: load failed (broken)' >&2",
    "    else",
    "      echo 'aot: 3 scripts precompiled (4 slots)' >&2",
    "    fi",
    "    ;;",
    "esac",
  ].join("\n") + "\n", { mode: 0o700 });
  const args = (action: "prepare" | "play" | "run") => [
    action, project, "--bin", binary, "--cache-dir", cacheRoot,
  ];
  const run = (args: string[], additionalEnv: Record<string, string> = {}) => {
    // Bun's nested test subprocess cannot always write to inherited pipe
    // descriptors inside the local sandbox. File descriptors are reliable.
    const output = path.join(root, "last.stdout");
    const errors = path.join(root, "last.stderr");
    const out = fs.openSync(output, "w");
    const err = fs.openSync(errors, "w");
    let status: number | null;
    try {
      const result = spawnSync(process.execPath, [launcher, ...args], {
        cwd: path.resolve("."),
        stdio: ["ignore", out, err],
        env: { ...process.env, ...additionalEnv },
      });
      status = result.status;
    } finally {
      fs.closeSync(out);
      fs.closeSync(err);
    }
    return {
      status,
      stdout: fs.readFileSync(output, "utf8"),
      stderr: fs.readFileSync(errors, "utf8"),
    };
  };
  return { binary, project, cacheRoot, args, run };
}

test("cache miss launches GUI without blocking compilation", async () => {
  const f = fixture();
  const r = f.run([...f.args("play"), "--", "--fps", "60"]);
  if (r.status !== 0) throw new Error("miss playback: " + r.stderr + " / " + r.stdout);
  expect(r.stderr).toContain("[aot-cache] miss");
  expect(r.stdout).toContain("FAKE_ARGS:play");
  expect(r.stdout).toContain("--fps 60");
  expect(r.stdout).not.toContain("--aot");
});

test("prepare caches atomically and the next launch actually loads native AOT", async () => {
  const f = fixture();
  const prepared = f.run(f.args("prepare"));
  expect(prepared.status).toBe(0);
  expect(prepared.stderr).toContain("[aot-cache] ready");
  const paths = await cachePaths(parseArgs(f.args("play")));
  expect(await validObject(paths)).toBe(true);
  expect(f.run(f.args("prepare")).stderr).toContain("[aot-cache] hit");
  const played = f.run([...f.args("play"), "--", "--turbo"]);
  if (played.status !== 0) throw new Error("cache-hit playback: " + played.stderr + " / " + played.stdout);
  expect(played.stderr).toContain("[aot-cache] hit");
  expect(played.stderr).toContain("aot: 3 scripts precompiled");
  expect(played.stdout).toContain("--aot");
  expect(played.stdout).toContain("--turbo");
});

test("a changed binary or project cannot reuse the native object", async () => {
  const f = fixture();
  expect(f.run(f.args("prepare")).status).toBe(0);
  fs.appendFileSync(f.project, "\nproject version 2");
  expect(f.run(f.args("play")).stderr).toContain("[aot-cache] miss");
  expect(f.run(f.args("prepare")).status).toBe(0);
  fs.appendFileSync(f.binary, "\n# binary version changed\n");
  expect(f.run(f.args("run")).stderr).toContain("[aot-cache] miss");
});

test("corrupted native object is rejected before it is loaded", async () => {
  const f = fixture();
  expect(f.run(f.args("prepare")).status).toBe(0);
  const paths = await cachePaths(parseArgs(f.args("play")));
  fs.appendFileSync(paths.object, "tampering");
  expect(await validObject(paths)).toBe(false);
  const played = f.run(f.args("play"));
  if (played.status !== 0) throw new Error("corrupt playback: " + played.stderr + " / " + played.stdout);
  expect(played.stderr).toContain("[aot-cache] miss");
  expect(played.stdout).not.toContain("--aot");
});

test("a cache symlink is not trusted", async () => {
  const f = fixture();
  expect(f.run(f.args("prepare")).status).toBe(0);
  const paths = await cachePaths(parseArgs(f.args("play")));
  fs.renameSync(paths.object, paths.object + ".original");
  fs.symlinkSync(paths.object + ".original", paths.object);
  expect(await validObject(paths)).toBe(false);
  const played = f.run(f.args("play"));
  expect(played.status).toBe(0);
  expect(played.stderr).toContain("[aot-cache] miss");
});

test("an AOT loader failure cannot silently be accepted as a cache hit", () => {
  const f = fixture();
  expect(f.run(f.args("prepare")).status).toBe(0);
  const played = f.run(f.args("play"), { FAKE_AOT_BAD_LOAD: "1" });
  expect(played.status).not.toBe(0);
  expect(played.stderr).toContain("refusing silent JIT fallback");
});

test("a failed compilation leaves no valid cache entry", async () => {
  const f = fixture();
  const failed = f.run(f.args("prepare"), { FAKE_AOT_FAIL: "1" });
  expect(failed.status).not.toBe(0);
  const paths = await cachePaths(parseArgs(f.args("play")));
  expect(await validObject(paths)).toBe(false);
  expect(fs.existsSync(path.dirname(paths.object))).toBe(true);
  expect(fs.readdirSync(path.dirname(paths.object))).toEqual([]);
});

test("invalid arguments fail without launching the binary", () => {
  expect(() => parseArgs(["play"])).toThrow();
  expect(() => parseArgs(["play", "test.sb3", "--bin"])).toThrow();
  expect(() => parseArgs(["prepare", "test.sb3", "--", "--fps", "2"])).toThrow();
  expect(() => parseArgs(["play", "test.sb3", "--", "--aot", "bad.o"])).toThrow();
});
