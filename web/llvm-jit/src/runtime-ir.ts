import { admitScriptChain } from './admission';
import { decodeSb3 } from './sb3';

type Json = Record<string, unknown>;
const isObj = (v: unknown): v is Json => v !== null && typeof v === 'object' && !Array.isArray(v);
const hex = (s: string): string => [...new TextEncoder().encode(s)].map(b => b.toString(16).padStart(2, '0')).join('');
function numberBits(n: number): bigint {
  const b = new DataView(new ArrayBuffer(8));
  b.setFloat64(0, n, true);
  return b.getBigUint64(0, true);
}
function numericShadow(block: Json): number | undefined {
  if (!isObj(block.inputs)) return;
  const input = block.inputs.VALUE;
  if (!Array.isArray(input) || input[0] !== 1 || !Array.isArray(input[1]) || input[1][0] !== 4) return;
  const raw = input[1][1];
  const n = typeof raw === 'number' ? raw : typeof raw === 'string' && raw.trim() !== '' ? Number(raw) : NaN;
  return Number.isFinite(n) ? n : undefined;
}
export interface RuntimeSlot { kind: number; global: string; keyHex: string }
export interface RuntimeIR {
  ir: string;
  slots: RuntimeSlot[];
  hat: string;
  target: number;
  /** This is a limited compatibility lowering, not xyo's C++ jitEmitScript. */
  source: 'typescript-restricted' | 'cpp-jit-emit';
}

/** Emit PIC LLVM IR that invokes the *real xyo* variable runtime helpers.
 * This is deliberately a small bridge until the wasm32 C++ jitEmitScript
 * compiler is built. Unsupported scripts fail closed.
 */
export function emitRuntimeIR(sb3: Uint8Array): RuntimeIR {
  const project = decodeSb3(sb3);
  if (!Array.isArray(project.targets)) throw Error('missing targets');
  const tops: Array<{ target: number; id: string; block: Json; blocks: Json }> = [];
  project.targets.forEach((t, target) => {
    if (!isObj(t) || !isObj(t.blocks)) return;
    for (const [id, block] of Object.entries(t.blocks)) {
      if (isObj(block) && block.topLevel === true) tops.push({ target, id, block, blocks: t.blocks as Json });
    }
  });
  if (tops.length !== 1) throw Error('requires exactly one top-level script');
  const top = tops[0]!;
  const chain = admitScriptChain(top.blocks as Parameters<typeof admitScriptChain>[0], top.id, top.block);
  if (!chain.ok) throw Error(chain.reason);
  const target = project.targets[top.target];
  if (!isObj(target) || !isObj(target.variables)) throw Error('target variables missing');
  const slots: RuntimeSlot[] = [];
  const names = new Map<string, number>();
  const instructions: string[] = [];
  for (const id of chain.chain.blocks.slice(1)) {
    const block = top.blocks[id];
    if (!isObj(block) || !isObj(block.fields)) throw Error('invalid block');
    const field = block.fields.VARIABLE;
    const variableId = Array.isArray(field) && typeof field[1] === 'string' ? field[1] : null;
    if (!variableId || !Object.hasOwn(target.variables, variableId)) throw Error('variable is not defined');
    const value = numericShadow(block);
    if (value === undefined) throw Error('only inline finite numeric shadows are supported');
    let index = names.get(variableId);
    if (index === undefined) {
      index = slots.length;
      names.set(variableId, index);
      slots.push({ kind: 0, global: 'xyo_aot_slot_' + index, keyHex: hex(variableId) });
    }
    const pointer = '%id' + instructions.length;
    const fn = block.opcode === 'data_setvariableto' ? 'xyo_var_set'
      : block.opcode === 'data_changevariableby' ? 'xyo_var_change' : null;
    if (!fn) throw Error('unsupported opcode');
    instructions.push('  ' + pointer + ' = load ptr, ptr @xyo_aot_slot_' + index + ', align 4');
    instructions.push('  call void @' + fn + '(ptr %ctx, ptr ' + pointer + ', ptr null, i64 ' + numberBits(value).toString() + ')');
  }
  const lines = [
    'target triple = "wasm32-unknown-emscripten"',
    ...slots.map(s => '@' + s.global + ' = global ptr null, align 4'),
    'declare void @xyo_var_set(ptr, ptr, ptr, i64)',
    'declare void @xyo_var_change(ptr, ptr, ptr, i64)',
    'define void @xyo_browser_script(ptr %ctx) {',
    'entry:',
    ...instructions,
    '  ret void',
    '}',
    '',
  ];
  return { ir: lines.join('\n'), slots, hat: top.id, target: top.target, source: 'typescript-restricted' };
}
