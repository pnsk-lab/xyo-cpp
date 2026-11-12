#!/usr/bin/env bun
/**
 * Opt-in, content-addressed xyo AOT cache for repeat launches.
 *
 * Usage:
 *   bun tools/xyo-aot-cache.ts prepare project.sb3 --bin ./build/xyo
 *   bun tools/xyo-aot-cache.ts play project.sb3 --bin ./build/xyo -- --scale 2
 *
 * Preparation is explicit and can take time. A cache miss NEVER compiles
 * during launch, preserving the existing GUI startup path.
 */
import { createHash, randomUUID } from "node:crypto";
import { createReadStream } from "node:fs";
import * as fs from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import { spawn } from "node:child_process";

export type Action = "prepare" | "play" | "run";

export interface Settings {
  action: Action;
  project: string;
  binary: string;
  cacheRoot: string;
  passthrough: string[];
}

export function parseArgs(
  argv: string[],
  env: NodeJS.ProcessEnv = process.env,
): Settings {
  const [action, project, ...tail] = argv;
  if (action !== "prepare" && action !== "play" && action !== "run") {
    throw new Error("Expected prepare, play, or run as first argument");
  }
  if (!project || project.startsWith("-")) {
    throw new Error("Expected project.sb3 after action");
  }

  let binary = "./build/xyo";
  let cacheRoot =
    env.XYO_AOT_CACHE_DIR ??
    path.join(env.XDG_CACHE_HOME ?? path.join(os.homedir(), ".cache"), "xyo-cpp", "aot");
  let passthrough: string[] = [];
  for (let i = 0; i < tail.length; ++i) {
    const argument = tail[i];
    if (argument === "--") {
      passthrough = tail.slice(i + 1);
      break;
    }
    if (argument === "--bin" || argument === "--cache-dir") {
      const value = tail[++i];
      if (!value || value.startsWith("--")) {
        throw new Error("Missing value for " + argument);
      }
      if (argument === "--bin") binary = value;
      else cacheRoot = value;
      continue;
    }
    throw new Error("Unknown option " + argument + "; use -- for xyo arguments");
  }
  if (action === "prepare" && passthrough.length) {
    throw new Error("prepare does not accept xyo playback arguments");
  }
  if (passthrough.includes("--aot")) {
    throw new Error("Do not pass --aot directly; the launcher selects its own cache");
  }
  return {
    action,
    project: path.resolve(project),
    binary: path.resolve(binary),
    cacheRoot: path.resolve(cacheRoot),
    passthrough,
  };
}

async function sha256File(filename: string): Promise<string> {
  const digest = createHash("sha256");
  for await (const chunk of createReadStream(filename)) digest.update(chunk);
  return digest.digest("hex");
}

export interface CachePaths {
  object: string;
  stamp: string;
  binaryHash: string;
  projectHash: string;
}

export async function cachePaths(settings: Settings): Promise<CachePaths> {
  const [binaryHash, projectHash] = await Promise.all([
    sha256File(settings.binary),
    sha256File(settings.project),
  ]);
  const dir = path.join(settings.cacheRoot, binaryHash);
  const object = path.join(dir, projectHash + ".o");
  return { object, stamp: object + ".json", binaryHash, projectHash };
}

interface CacheStamp {
  version: 1;
  binaryHash: string;
  projectHash: string;
  objectHash: string;
}

export async function validObject(paths: CachePaths): Promise<boolean> {
  try {
    const stamp: CacheStamp = JSON.parse(await fs.readFile(paths.stamp, "utf8"));
    if (
      stamp.version !== 1 ||
      stamp.binaryHash !== paths.binaryHash ||
      stamp.projectHash !== paths.projectHash
    ) return false;
    const [objectStat, stampStat] = await Promise.all([
      fs.lstat(paths.object),
      fs.lstat(paths.stamp),
    ]);
    return stampStat.isFile() && objectStat.isFile() && objectStat.size > 0 &&
      (await sha256File(paths.object)) === stamp.objectHash;
  } catch {
    return false;
  }
}

async function exitStatus(child: ReturnType<typeof spawn>): Promise<number> {
  return await new Promise((resolve, reject) => {
    child.once("error", reject);
    child.once("close", (code, signal) => {
      resolve(code ?? (signal ? 128 + (os.constants.signals[signal] ?? 0) : 1));
    });
  });
}

export async function prepare(settings: Settings, paths: CachePaths): Promise<void> {
  if (await validObject(paths)) {
    console.error("[aot-cache] hit: " + paths.object);
    return;
  }
  await fs.mkdir(path.dirname(paths.object), { recursive: true, mode: 0o700 });
  const temporary = paths.object + "." + process.pid + "." + randomUUID() + ".tmp";
  const stampTemp = temporary + ".json";
  try {
    console.error("[aot-cache] compiling once for this binary + project...");
    const child = spawn(
      settings.binary,
      ["aot", settings.project, "--obj", temporary],
      // A cached object may be reused on a different machine sharing the
      // same home directory. Never persist host-specific instruction sets.
      { stdio: "inherit", env: { ...process.env, XYO_AOT_TARGET_CPU: "generic" } },
    );
    if (await exitStatus(child) !== 0) {
      throw new Error("xyo aot compilation failed; cache is unchanged");
    }
    const stat = await fs.stat(temporary);
    if (!stat.isFile() || stat.size === 0) {
      throw new Error("xyo aot did not produce a nonempty object");
    }
    const stamp: CacheStamp = {
      version: 1,
      binaryHash: paths.binaryHash,
      projectHash: paths.projectHash,
      objectHash: await sha256File(temporary),
    };
    // Readers accept only an object that matches a fully published stamp.
    // Partial or interrupted compilations are discarded.
    await fs.writeFile(stampTemp, JSON.stringify(stamp) + "\n", { mode: 0o600 });
    await fs.rename(temporary, paths.object);
    await fs.rename(stampTemp, paths.stamp);
    console.error("[aot-cache] ready: " + paths.object + " (" + stat.size + " bytes)");
  } finally {
    await Promise.allSettled([
      fs.rm(temporary, { force: true }),
      fs.rm(stampTemp, { force: true }),
    ]);
  }
}

/** Reject silent JIT fallback on a cache hit by checking the AOT loader log. */
export async function launch(settings: Settings, paths: CachePaths): Promise<number> {
  const hit = await validObject(paths);
  console.error("[aot-cache] " + (hit ? "hit" : "miss") + ": " + paths.object);
  const args = [
    settings.action,
    settings.project,
    ...settings.passthrough,
    ...(hit ? ["--aot", paths.object] : []),
  ];
  const child = spawn(settings.binary, args, {
    stdin: "inherit",
    stdout: "pipe",
    stderr: "pipe",
  });
  // Explicitly forward stdout: Bun's Node spawn shim may not propagate
  // inherited stdout through a parent process's captured pipe.
  child.stdout?.on("data", (chunk: Buffer) => process.stdout.write(chunk));
  let tail = "";
  let nativeLoaded = false;
  let loadFailed = false;
  const stderr = child.stderr;
  if (!stderr) throw new Error("Cannot capture AOT load result");
  stderr.on("data", (chunk: Buffer) => {
    const part = chunk.toString("utf8");
    process.stderr.write(part);
    tail = (tail + part).slice(-4096);
    if (/aot:\s+[1-9]\d* scripts precompiled\b/.test(tail)) nativeLoaded = true;
    if (/aot:\s+(?:load failed|cannot read)/.test(tail)) {
      loadFailed = true;
      child.kill();
    }
  });
  const result = await exitStatus(child);
  if (hit && (loadFailed || !nativeLoaded)) {
    throw new Error("Cached AOT object did not load; refusing silent JIT fallback");
  }
  return result;
}

export async function main(argv: string[]): Promise<number> {
  const settings = parseArgs(argv);
  const paths = await cachePaths(settings);
  if (settings.action === "prepare") {
    await prepare(settings, paths);
    return 0;
  }
  return launch(settings, paths);
}

if (import.meta.main) {
  main(process.argv.slice(2))
    .then((code) => { process.exitCode = code; })
    .catch((error: unknown) => {
      console.error("[aot-cache] error: " + String(error));
      process.exitCode = 1;
    });
}
