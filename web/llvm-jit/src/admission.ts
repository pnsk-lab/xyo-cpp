/** Green-flag routing decision for the in-browser LLVM backend (test-only).
 *
 * Mirrors the structural admission gates of the metadata compiler
 * (`browser_ir_api.cpp`) over raw project JSON: exactly one top-level script
 * and it must be a single green-flag hat followed by a terminal straight-line
 * chain (up to 8 blocks) of `data_setvariableto`/`data_changevariableby`,
 * each with a numeric-shadow VALUE.
 * The C++ compiler remains the authority and re-validates everything at
 * compile time; this router may only send provably admitted shapes toward
 * LLVM and must send everything else to the built-in VM fallback. Either
 * kind of mirror mistake is fail-closed (a missed optimization, never an
 * unsafe compile), because admission here never executes anything.
 */
export type Admission = { route: 'llvm' } | { route: 'fallback'; reason: string };

type JsonBlock = {
  opcode?: unknown;
  next?: unknown;
  topLevel?: unknown;
  inputs?: unknown;
  fields?: unknown;
  shadow?: unknown;
  mutation?: unknown;
};

function isObject(value: unknown): value is Record<string, unknown> {
  return !!value && typeof value === 'object' && !Array.isArray(value);
}

function isEmptyNext(value: unknown): boolean {
  return value === null || value === undefined || value === '';
}

function hasOwn(value: object, key: string): boolean {
  return Object.hasOwn(value, key);
}

/** Resolve a VALUE input desc the way the SB3 loader does (info flag first
 * element: 1 = same block+shadow, 2 = block without shadow, else block+shadow
 * pair). Returns the child descriptor and whether it lands in shadow position. */
function resolveValueChild(desc: unknown): { desc: unknown; shadowPosition: boolean } | null {
  if (!Array.isArray(desc) || desc.length === 0) return null;
  const info = desc[0];
  if (info === 1) {
    if (desc.length < 2) return null;
    return { desc: desc[1], shadowPosition: true };
  }
  if (info === 2) {
    if (desc.length < 2) return null;
    return { desc: desc[1], shadowPosition: false };
  }
  const block = desc.length > 1 ? desc[1] : undefined;
  const shadow = desc.length > 2 ? desc[2] : undefined;
  const useBlock = typeof block === 'string' ? block !== '' : Array.isArray(block);
  return { desc: useBlock ? block : shadow, shadowPosition: !useBlock };
}

/** A numeric-shadow literal: primitive [4, ...] in shadow position, or an
 * explicit shadow math_number block with a NUM field, terminal and inert. */
function isNumericShadowLiteral(desc: unknown, shadowPosition: boolean, blocks: Record<string, JsonBlock>): boolean {
  if (Array.isArray(desc)) {
    return shadowPosition && desc.length >= 1 && desc[0] === 4;
  }
  if (typeof desc !== 'string' || desc === '') return false;
  const literal = blocks[desc];
  if (!literal || literal.shadow !== true || literal.opcode !== 'math_number') return false;
  if (!isObject(literal.fields) || !hasOwn(literal.fields, 'NUM')) return false;
  const num = (literal.fields as Record<string, unknown>).NUM;
  if (!Array.isArray(num) || num.length === 0) return false;
  if (literal.mutation !== null && literal.mutation !== undefined) return false;
  if (!isEmptyNext(literal.next)) return false;
  if (literal.inputs !== null && literal.inputs !== undefined) {
    if (!isObject(literal.inputs) || Object.keys(literal.inputs).length !== 0) return false;
  }
  return true;
}

/** One admitted green-flag script: hat plus its straight-line body chain. */
export interface AdmittedChain { hat: string; blocks: string[] }
export type ChainVerdict = { ok: true; chain: AdmittedChain } | { ok: false; reason: string };

/** Per-script gate shared by the whole-project router and the splitter. */
export function admitScriptChain(blocks: Record<string, JsonBlock>, hatId: string, hat: JsonBlock): ChainVerdict {
  const no = (reason: string): ChainVerdict => ({ ok: false, reason });
  if (hat.opcode !== 'event_whenflagclicked') return no('hat is not green flag');
  if (typeof hat.next !== 'string' || hat.next === '') return no('hat has no body');
  // Straight-line chain of set/change blocks, capped like the compiler gate
  // (the cap also bounds cyclic chains).
  const chain: string[] = [hatId];
  let next: unknown = hat.next;
  let count = 0;
  for (;;) {
    if (typeof next !== 'string' || next === '') return no('hat has no body');
    count++;
    if (count > 8) return no('script chain too long');
    const body = blocks[next];
    if (!body) return no('body block missing');
    chain.push(next);
    if (body.opcode !== 'data_setvariableto' && body.opcode !== 'data_changevariableby') {
      return no(`unsupported body opcode ${String(body.opcode)}`);
    }
    if (body.mutation !== null && body.mutation !== undefined) return no('body has mutation');
    // The loader skips non-array and empty input descs when counting; mirror
    // that so non-array extras do not change the verdict.
    if (!isObject(body.inputs)) return no('body needs exactly the VALUE input');
    const counted = Object.keys(body.inputs).filter(k => {
      const v = (body.inputs as Record<string, unknown>)[k];
      return Array.isArray(v) && v.length > 0;
    });
    if (counted.length !== 1 || counted[0] !== 'VALUE') {
      return no('body needs exactly the VALUE input');
    }
    const resolved = resolveValueChild((body.inputs as Record<string, unknown>).VALUE);
    if (!resolved || !isNumericShadowLiteral(resolved.desc, resolved.shadowPosition, blocks)) {
      return no('VALUE must be a numeric shadow');
    }
    if (!isObject(body.fields) || !hasOwn(body.fields, 'VARIABLE')) return no('body has no variable');
    const variable = (body.fields as Record<string, unknown>).VARIABLE;
    if (!Array.isArray(variable) || variable.length < 2) return no('malformed VARIABLE field');
    const [varName, varId] = variable;
    if (typeof varId !== 'string' || varId === '' || varId.includes('\0')) {
      return no('variable id must be non-empty without NUL');
    }
    if (typeof varName === 'string' && varName.includes('\0')) return no('variable name must not contain NUL');
    if (isEmptyNext(body.next)) break;
    next = body.next;
  }
  return { ok: true, chain: { hat: hatId, blocks: chain } };
}

export function admitSb3Project(project: unknown): Admission {
  const fail = (reason: string): Admission => ({ route: 'fallback', reason });
  if (!isObject(project)) return fail('not a project object');
  if (!Array.isArray(project.targets)) return fail('no targets');
  const blocksByTarget: Record<string, JsonBlock>[] = [];
  const tops: Array<{ target: number; id: string; block: JsonBlock }> = [];
  for (let ti = 0; ti < project.targets.length; ti++) {
    const target = project.targets[ti];
    if (!isObject(target)) continue;
    const blocks = (target as { blocks?: unknown }).blocks;
    if (!isObject(blocks)) continue;
    const table: Record<string, JsonBlock> = {};
    for (const [id, b] of Object.entries(blocks)) {
      if (!isObject(b)) continue;
      table[id] = b as JsonBlock;
      if ((b as JsonBlock).topLevel === true) tops.push({ target: ti, id, block: b as JsonBlock });
    }
    blocksByTarget.push(table);
  }
  if (tops.length !== 1) return fail(`expected exactly one script, found ${tops.length}`);
  const top = tops[0];
  if (!top) return fail('expected exactly one script, found none');
  const blocks = blocksByTarget[top.target];
  if (!blocks) return fail('body block missing');
  const verdict = admitScriptChain(blocks, top.id, top.block);
  if (!verdict.ok) return fail(verdict.reason);
  return { route: 'llvm' };
}
