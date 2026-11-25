// Explicit maintenance command, not part of the documentation build:
//   node site/scripts/capture-llvm-ir.mjs
// Requires an existing LLVM-enabled build/xyo and the generated benchmark fixture.
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';

const root = fileURLToPath(new URL('../../', import.meta.url));
const binary = resolve(root, 'build/xyo');
const project = 'tests/bench/algos/out/micro_loop_1000000.sb3';
const hash = data => createHash('sha256').update(data).digest('hex');
const binaryHash = hash(await readFile(binary));
const projectHash = hash(await readFile(resolve(root, project)));
const args = ['run', project, '--jit=eager', '--no-mega', '--frames', '3000'];
// Do not inherit unrelated debug/optimization experiments from the caller's shell.
const env = Object.fromEntries(Object.entries(process.env).filter(([key]) => !key.startsWith('XYO_')));
env.XYO_JIT_DUMP2 = '1';
const run = spawnSync(binary, args, { cwd: root, env, encoding: 'utf8', timeout: 30000, maxBuffer: 8 * 1024 * 1024 });
if (run.error || run.status !== 0) throw new Error(`xyo capture failed: ${run.error?.message ?? run.stderr}`);
if (hash(await readFile(binary)) !== binaryHash || hash(await readFile(resolve(root, project))) !== projectHash) throw new Error('Binary or fixture changed during capture; retry after the build finishes.');
const raw = run.stderr;
if (!raw.startsWith("; ModuleID = 'xyo_script'\n") || !raw.includes('target triple = ') || !raw.includes('define void @xyo_script_')) throw new Error('Expected an LLVM module, not a diagnostic or MLIR dump.');
const allLines = raw.trimEnd().split('\n');
// Extract one complete numeric-loop basic block verbatim, not invented sample IR.
const start = allLines.findIndex(line => /^\.lr\.ph\.split:/.test(line));
if (start < 0) throw new Error('Numeric-loop block changed; review the capture before choosing a new excerpt.');
let end = start + 1;
while (end < allLines.length && allLines[end].trim() !== '') end++;
const lines = allLines.slice(start, end);
if (!lines.some(line => /\bfadd\b/.test(line)) || !lines.some(line => /\bphi\b/.test(line))) throw new Error('The selected block no longer contains the expected numeric-loop IR.');
const commit = spawnSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' });
if (commit.status !== 0) throw new Error('Cannot record capture-time repository revision.');
const data = {
  project, projectName: 'micro_loop_1000000.sb3',
  description: 'Scratch repeat n: change i by 1',
  captureCommand: `XYO_JIT_DUMP2=1 ./build/xyo ${args.join(' ')}`,
  capturedAt: new Date().toISOString(),
  captureRepositoryCommit: commit.stdout.trim(),
  binarySha256: binaryHash, projectSha256: projectHash, irSha256: hash(raw),
  note: 'Captured optimized LLVM IR from the existing build/xyo binary; the capture-time repository commit is not a claim that this binary was built from that exact revision. This is a recorded snapshot, not live browser compilation.',
  target: raw.match(/^target triple = "([^"]+)"/m)?.[1],
  moduleLines: allLines.length, sourceFile: 'ir/micro-loop.ll',
  excerpt: { startLine: start + 1, endLine: end, lines },
};
for (const path of ['site/public/ir/micro-loop.ll', 'site/public/ir/micro-loop.provenance.json', 'site/src/data/llvm-ir.json']) await mkdir(dirname(resolve(root, path)), { recursive: true });
await writeFile(resolve(root, 'site/public/ir/micro-loop.ll'), raw);
await writeFile(resolve(root, 'site/public/ir/micro-loop.provenance.json'), JSON.stringify(data, null, 2) + '\n');
await writeFile(resolve(root, 'site/src/data/llvm-ir.json'), JSON.stringify(data, null, 2) + '\n');
console.log(`Captured ${data.moduleLines} LLVM IR lines; verbatim excerpt ${data.excerpt.startLine}–${data.excerpt.endLine}.`);
console.log(`SHA-256 ${data.irSha256}`);
