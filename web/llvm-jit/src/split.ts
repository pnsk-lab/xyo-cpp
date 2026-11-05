/** Split a general project into LLVM-admitted scripts and the remainder.
 *
 * The metadata compiler only accepts whole single-script projects, so each
 * admitted green-flag hat is extracted together with its body chain into a
 * minimal single-script project that the existing flow can compile as-is.
 * The remainder keeps every other block untouched (including blocks shared
 * with admitted scripts, which are left in place) for the built-in VM.
 * Pure and total: never executes anything, never mutates its input.
 */
import { admitScriptChain } from './admission';

type JsonBlock = {
  opcode?: unknown;
  next?: unknown;
  parent?: unknown;
  topLevel?: unknown;
  inputs?: unknown;
  fields?: unknown;
  shadow?: unknown;
  mutation?: unknown;
};

function isObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

export interface SplitScript {
  target: number;
  hat: string;
  blocks: string[];
  /** Minimal single-script project compiling exactly this script. */
  project: unknown;
}
export interface SplitSkipped { target: number; hat: string; reason: string }
export type SplitResult =
  | { ok: true; admitted: SplitScript[]; skipped: SplitSkipped[]; rest: unknown }
  | { ok: false; error: string };

function blockTable(target: unknown): Record<string, JsonBlock> | null {
  if (!isObject(target)) return null;
  const blocks = (target as { blocks?: unknown }).blocks;
  if (!isObject(blocks)) return null;
  const table: Record<string, JsonBlock> = {};
  for (const [id, b] of Object.entries(blocks)) {
    if (isObject(b)) table[id] = b as JsonBlock;
  }
  return table;
}

/** Collect block ids reachable from string VALUE references (explicit shadows). */
function referencedLiterals(blocks: Record<string, JsonBlock>, ids: string[]): string[] {
  const found: string[] = [];
  for (const id of ids) {
    const b = blocks[id];
    if (!b || !isObject(b.inputs)) continue;
    const value = (b.inputs as Record<string, unknown>).VALUE;
    if (!Array.isArray(value)) continue;
    for (const desc of value.slice(1)) {
      if (typeof desc === 'string' && desc !== '' && blocks[desc] && !ids.includes(desc) && !found.includes(desc)) {
        found.push(desc);
      }
    }
  }
  return found;
}

export function splitProjectForLlvm(project: unknown): SplitResult {
  const fail = (error: string): SplitResult => ({ ok: false, error });
  if (!isObject(project)) return fail('not a project object');
  if (!Array.isArray(project.targets)) return fail('no targets');
  let rest: unknown;
  try {
    rest = JSON.parse(JSON.stringify(project));
  } catch {
    return fail('project is not JSON-serializable');
  }
  const targets = project.targets;
  if (!Array.isArray(targets)) return fail('no targets');
  const restTargets = (rest as { targets: unknown[] }).targets;
  const admitted: SplitScript[] = [];
  const skipped: SplitSkipped[] = [];
  for (let ti = 0; ti < targets.length; ti++) {
    const table = blockTable(targets[ti]);
    if (!table) continue;
    for (const [id, b] of Object.entries(table)) {
      if (b.topLevel !== true) continue;
      const verdict = admitScriptChain(table, id, b);
      if (!verdict.ok) {
        if (b.opcode === 'event_whenflagclicked') skipped.push({ target: ti, hat: id, reason: verdict.reason });
        continue;
      }
      const keep = [...verdict.chain.blocks, ...referencedLiterals(table, verdict.chain.blocks)];
      const subset: Record<string, JsonBlock> = {};
      for (const bid of keep) {
        // Structured clone keeps parent/next links valid inside the subset.
        subset[bid] = JSON.parse(JSON.stringify(table[bid]));
      }
      const target = targets[ti];
      const single = isObject(target) ? { ...JSON.parse(JSON.stringify(target)), blocks: subset } : { blocks: subset };
      admitted.push({ target: ti, hat: id, blocks: verdict.chain.blocks, project: { targets: [single], monitors: [], extensions: [], meta: project.meta !== undefined ? project.meta : { semver: '3.0.0' } } });
      // Silence exactly this script in the remainder while keeping the hat
      // present: the unified runtime binds an LLVM session to the hat id, and
      // the interpreter must still see a script there so it starts nothing.
      // Only the body blocks are removed; shared literals stay untouched.
      const restTarget = restTargets[ti];
      const restBlocks = isObject(restTarget) && isObject((restTarget as { blocks?: unknown }).blocks)
        ? (restTarget as { blocks: Record<string, unknown> }).blocks
        : null;
      if (restBlocks) {
        const hatBlock = restBlocks[verdict.chain.hat];
        if (isObject(hatBlock)) (hatBlock as { next?: unknown }).next = null;
        for (const bid of verdict.chain.blocks.slice(1)) delete restBlocks[bid];
      }
    }
  }
  return { ok: true, admitted, skipped, rest };
}
