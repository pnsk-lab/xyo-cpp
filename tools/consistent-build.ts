#!/usr/bin/env bun
/**
 * Build and freeze a coherent xyo native binary in a shared multi-agent tree.
 *
 * CMake tracks dependencies, but concurrent source edits can occur between
 * compiling separate translation units. Such mixed layouts can corrupt the
 * native JIT cache even if linking succeeds. Retry until the complete source
 * tree remained unchanged throughout a successful build and artifact copy.
 *
 * Example:
 * bun tools/consistent-build.ts --build board/.osr-isolation --output board/xyo-verified
 */
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import { copyFile, mkdir, readFile, readdir, stat, writeFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

interface Options {
  buildDir: string;
  output: string;
  artifact: string;
  target: string;
  jobs: number;
  attempts: number;
}

const repository = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

function positiveInteger(value: string, field: string): number {
  const number = Number(value);
  if (!Number.isSafeInteger(number) || number < 1 || number > 64) {
    throw new Error(field + " must be an integer between 1 and 64");
  }
  return number;
}

function parseArguments(args: string[]): Options {
  const values = new Map<string, string>();
  const supported = new Set([
    "--build", "--output", "--artifact", "--target", "--jobs", "--attempts",
  ]);
  for (let i = 0; i < args.length; i += 2) {
    const key = args[i];
    const value = args[i + 1];
    if (!supported.has(key) || !value || value.startsWith("--")) {
      throw new Error("Unknown or incomplete option: " + String(key));
    }
    values.set(key, value);
  }

  const buildDir = path.resolve(repository, values.get("--build") ?? "build");
  const outputName = values.get("--output");
  if (!outputName) throw new Error("--output is required");
  return {
    buildDir,
    output: path.resolve(repository, outputName),
    artifact: path.resolve(buildDir, values.get("--artifact") ?? "xyo"),
    target: values.get("--target") ?? "xyo-cli",
    jobs: positiveInteger(values.get("--jobs") ?? "3", "--jobs"),
    attempts: positiveInteger(values.get("--attempts") ?? "4", "--attempts"),
  };
}

async function collectFiles(location: string, files: string[]): Promise<void> {
  let metadata;
  try {
    metadata = await stat(location);
  } catch (error) {
    if ((error as NodeJS.ErrnoException).code === "ENOENT") return;
    throw error;
  }
  if (metadata.isFile()) {
    files.push(location);
    return;
  }
  if (!metadata.isDirectory()) return;
  const entries = await readdir(location, { withFileTypes: true });
  entries.sort((a, b) => a.name.localeCompare(b.name));
  for (const entry of entries) {
    if (entry.isDirectory() || entry.isFile()) {
      await collectFiles(path.join(location, entry.name), files);
    }
  }
}

async function sourceDigest(): Promise<string> {
  const files: string[] = [];
  // Include C++ sources, shared headers, CMake configuration and bundled
  // compiler inputs. Generated build output and board logs are excluded.
  for (const root of ["src", "cmake", "CMakeLists.txt", "tests/CMakeLists.txt"]) {
    await collectFiles(path.join(repository, root), files);
  }
  files.sort();
  const hash = createHash("sha256");
  for (const file of files) {
    const relativeName = path.relative(repository, file);
    hash.update(relativeName);
    hash.update("\0");
    hash.update(await readFile(file));
    hash.update("\0");
  }
  return hash.digest("hex");
}

async function run(options: Options): Promise<void> {
  await mkdir(path.dirname(options.output), { recursive: true });
  for (let attempt = 1; attempt <= options.attempts; attempt += 1) {
    const before = await sourceDigest();
    const result = spawnSync(
      "cmake",
      ["--build", options.buildDir, "--target", options.target,
       "--parallel", String(options.jobs)],
      { cwd: repository, stdio: "inherit", env: process.env },
    );
    if (result.error) throw result.error;
    if (result.status !== 0) {
      throw new Error("CMake build failed, status=" + String(result.status));
    }

    if (before !== await sourceDigest()) {
      console.warn("Source changed during build; retrying (" + attempt + ")");
      continue;
    }

    await copyFile(options.artifact, options.output);
    const binaryHash = createHash("sha256")
      .update(await readFile(options.output))
      .digest("hex");

    if (before !== await sourceDigest()) {
      console.warn("Source changed while freezing binary; retrying (" + attempt + ")");
      continue;
    }

    const manifest = {
      builtAt: new Date().toISOString(),
      sourceSha256: before,
      binarySha256: binaryHash,
      buildDir: path.relative(repository, options.buildDir),
      target: options.target,
      attempts: attempt,
    };
    await writeFile(options.output + ".manifest.json",
      JSON.stringify(manifest, null, 2) + "\n");
    console.log("Coherent binary: " + options.output);
    console.log("Source digest: " + before);
    console.log("Binary digest: " + binaryHash);
    return;
  }
  throw new Error("Sources kept changing; no build was marked coherent");
}

try {
  await run(parseArguments(process.argv.slice(2)));
} catch (error) {
  console.error(error);
  process.exitCode = 1;
}
