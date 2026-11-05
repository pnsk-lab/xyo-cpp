import { unzipSync } from 'fflate';
import { admitScriptChain } from './admission';
import type { LLVMPipeline } from './pipeline';

type RecordObject = Record<string, unknown>;
type Variable = { id: string; name: string; value: number };
export interface Sb3Compilation {
  /** LLVM IR emitted for a single admitted green-flag script. */
  ir: string;
  variables: Variable[];
  /** Value of every variable after running this script. */
  readonly targetName: string;
}
export interface Sb3Execution {
  engine: 'llvm-wasm';
  variables: Variable[];
  project: RecordObject;
}
export type AdmissionResult =
  | { ok: true; compilation: Sb3Compilation; project: RecordObject }
  | { ok: false; reason: string };

function object(v: unknown): v is RecordObject {
  return typeof v === 'object' && v !== null && !Array.isArray(v);
}

function toScratchNumber(v: unknown): number | null {
  // This restricted backend handles only finite numbers. Non-numeric,
  // NaN/Infinity and nontrivial string coercions must use the interpreter.
  const n = typeof v === 'number' ? v : typeof v === 'string' && v.trim() !== '' ? Number(v) : NaN;
  return Number.isFinite(n) ? n : null;
}

function llvmDouble(value: number): string {
  if (!Number.isFinite(value)) throw new Error('non-finite LLVM constant');
  const view = new DataView(new ArrayBuffer(8));
  view.setFloat64(0, value, false);
  return '0x' + view.getBigUint64(0, false).toString(16).padStart(16, '0').toUpperCase();
}

/** Reads an SB3 ZIP; refuses oversized payloads before parsing project JSON. */
export function decodeSb3(bytes: Uint8Array): RecordObject {
  if (bytes.byteLength === 0 || bytes.byteLength > 16 * 1024 * 1024) throw new Error('SB3 exceeds input size limit');
  const archive = unzipSync(bytes, { filter: file => file.name === 'project.json' && file.originalSize <= 8 * 1024 * 1024 });
  const data = archive['project.json'];
  if (!data || data.length > 8 * 1024 * 1024) throw new Error('project.json missing or oversized');
  const project: unknown = JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(data));
  if (!object(project)) throw new Error('project.json must be an object');
  return project;
}

function numericLiteral(input: unknown, blocks: RecordObject): number | null {
  if (!Array.isArray(input) || input.length < 2) return null;
  const info = input[0];
  const child: unknown = info === 1 || info === 2 ? input[1] :
    typeof input[1] === 'string' ? input[1] : Array.isArray(input[1]) ? input[1] : input[2];
  if (Array.isArray(child) && child[0] === 4 && (info === 1 || info === 3)) return toScratchNumber(child[1]);
  if (typeof child !== 'string') return null;
  const literal = blocks[child];
  if (!object(literal) || literal.shadow !== true || literal.opcode !== 'math_number') return null;
  if (!object(literal.fields)) return null;
  const num = literal.fields.NUM;
  return Array.isArray(num) ? toScratchNumber(num[0]) : null;
}

function extractNumericScript(project: RecordObject): AdmissionResult {
  const fail = (reason: string): AdmissionResult => ({ ok: false, reason });
  if (!Array.isArray(project.targets)) return fail('missing targets');
  // Use one script only. This avoids changing Scratch's scheduling/ordering
  // when there are multiple hats, events or concurrently running threads.
  const tops: Array<{ target: RecordObject; blocks: RecordObject; id: string; block: RecordObject }> = [];
  for (const target of project.targets) {
    if (!object(target) || !object(target.blocks)) continue;
    for (const [id, block] of Object.entries(target.blocks)) {
      if (object(block) && block.topLevel === true) tops.push({ target, blocks: target.blocks, id, block });
    }
  }
  if (tops.length !== 1) return fail('requires exactly one top-level script');
  const top = tops[0]!;
  const admitted = admitScriptChain(top.blocks as Parameters<typeof admitScriptChain>[0], top.id, top.block);
  if (!admitted.ok) return fail(admitted.reason);
  if (!object(top.target.variables)) return fail('target has no variables');

  const initial: Variable[] = [];
  for (const [id, entry] of Object.entries(top.target.variables)) {
    if (!Array.isArray(entry) || typeof entry[0] !== 'string') return fail('malformed variable');
    const number = toScratchNumber(entry[1]);
    if (number === null) return fail('non-numeric variable requires interpreter');
    initial.push({ id, name: entry[0], value: number });
  }
  if (initial.length > 1024) return fail('too many variables');
  const indices = new Map(initial.map((v, i) => [v.id, i]));
  const lines: string[] = [
    'target triple = "wasm32-unknown-unknown"',
    'define void @xyo_script(ptr %variables) {',
    'entry:',
  ];
  let tmp = 0;
  for (const id of admitted.chain.blocks.slice(1)) {
    const block = top.blocks[id];
    if (!object(block) || !object(block.fields) || !object(block.inputs)) return fail('invalid block');
    const field = block.fields.VARIABLE;
    const variableId = Array.isArray(field) ? field[1] : undefined;
    if (typeof variableId !== 'string') return fail('missing variable id');
    const index = indices.get(variableId);
    if (index === undefined) return fail('variable not found on target');
    const literal = numericLiteral(block.inputs.VALUE, top.blocks);
    if (literal === null) return fail('invalid or non-finite numeric literal');
    const pointer = '%p' + tmp;
    lines.push('  ' + pointer + ' = getelementptr double, ptr %variables, i32 ' + index);
    if (block.opcode === 'data_setvariableto') {
      lines.push('  store double ' + llvmDouble(literal) + ', ptr ' + pointer + ', align 8');
    } else {
      const prior = '%v' + tmp;
      const result = '%r' + tmp;
      lines.push('  ' + prior + ' = load double, ptr ' + pointer + ', align 8');
      lines.push('  ' + result + ' = fadd double ' + prior + ', ' + llvmDouble(literal));
      lines.push('  store double ' + result + ', ptr ' + pointer + ', align 8');
    }
    tmp++;
  }
  lines.push('  ret void', '}', '');
  return { ok: true, compilation: { ir: lines.join('\n'), variables: initial, targetName: String(top.target.name ?? '') }, project };
}

export function admitSb3(bytes: Uint8Array): AdmissionResult {
  try { return extractNumericScript(decodeSb3(bytes)); }
  catch (e) { return { ok: false, reason: e instanceof Error ? e.message : String(e) }; }
}

/** Compile with real LLVM llc + wasm-ld, run in a separate WebAssembly instance.
 * This is a restricted numeric-only Scratch backend: it does not mutate a live
 * xyo::Runtime and must not be used as a silent fallback for unsupported blocks.
 */
export async function executeSb3(bytes: Uint8Array, pipeline: LLVMPipeline): Promise<Sb3Execution> {
  const admission = admitSb3(bytes);
  if (!admission.ok) throw new Error('Interpreter required: ' + admission.reason);
  const { compilation, project } = admission;
  const { binary } = await pipeline.compile({ ir: compilation.ir, exports: ['xyo_script'], exportMemory: true });
  const module = await WebAssembly.compile(Uint8Array.from(binary));
  const instance = await WebAssembly.instantiate(module);
  const entry = instance.exports.xyo_script;
  const memory = instance.exports.memory;
  if (typeof entry !== 'function' || !(memory instanceof WebAssembly.Memory)) {
    throw new Error('WASM module missing entry or memory');
  }
  // wasm-ld reserves the first 64 KiB for the stack (via --stack-first).
  // Start variable slots at the next page, beyond static data and the stack.
  const offset = 65536;
  const size = compilation.variables.length * 8;
  if (memory.buffer.byteLength < offset + size) throw new Error('WASM memory too small');
  const data = new DataView(memory.buffer);
  for (let i = 0; i < compilation.variables.length; i++) {
    data.setFloat64(offset + i * 8, compilation.variables[i]!.value, true);
  }
  entry(offset);
  const variables = compilation.variables.map((v, i) => ({ ...v, value: data.getFloat64(offset + i * 8, true) }));
  if (!Array.isArray(project.targets)) throw new Error('project mutated');
  const copy = structuredClone(project);
  const target = (copy.targets as RecordObject[]).find(t => t.name === compilation.targetName);
  if (!target || !object(target.variables)) throw new Error('target missing after execution');
  for (const v of variables) {
    const entry = target.variables[v.id];
    if (!Array.isArray(entry)) throw new Error('variable missing after execution');
    entry[1] = v.value;
  }
  return { engine: 'llvm-wasm', variables, project: copy };
}
