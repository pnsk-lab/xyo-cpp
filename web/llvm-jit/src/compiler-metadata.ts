import type { RuntimeSlot } from './runtime-ir';

export interface BrowserCompilerMetadata {
  version: number;
  dataLayout: string;
  targetIndex: number;
  targetTriple: string;
  abiDescriptor: string;
  slots: RuntimeSlot[];
}
const parseDescriptor = (text: string): Map<string, string> => {
  const parts = text.split(';');
  if (parts.shift() !== 'xyo-browser-ir-abi-v2') throw Error('unsupported compiler ABI version');
  const result = new Map<string, string>();
  for (const part of parts) {
    const eq = part.indexOf('=');
    if (eq < 1) throw Error('malformed compiler ABI descriptor');
    const key = part.slice(0, eq);
    if (result.has(key)) throw Error('duplicate compiler ABI field');
    result.set(key, part.slice(eq + 1));
  }
  return result;
};

/** Ensures C++ code generated for the wasm32 xyo layout is never bound against
 * a native/other-runtime layout. Diagnostic fingerprints are not proof that a
 * side module is secure or that arbitrary code is safe to execute.
 */
export function verifyBrowserCompilerMetadata(
  metadata: BrowserCompilerMetadata,
  hostDescriptor: string,
  expectedTarget: number,
): void {
  if (metadata.version !== 1) throw Error('unsupported compiler metadata schema');
  if (typeof metadata.dataLayout !== 'string' || !metadata.dataLayout.includes('p:32:32')) throw Error('invalid wasm32 data layout');
  if (metadata.targetTriple !== 'wasm32-unknown-emscripten') throw Error('wrong LLVM triple');
  if (metadata.targetIndex !== expectedTarget) throw Error('target index mismatch');
  if (!Array.isArray(metadata.slots) || metadata.slots.length === 0 || metadata.slots.length > 64) throw Error('invalid slot count');
  const host = parseDescriptor(hostDescriptor);
  const compiler = parseDescriptor(metadata.abiDescriptor);
  if (compiler.get('layout') !== metadata.dataLayout || compiler.get('triple') !== metadata.targetTriple) throw Error('compiler layout metadata inconsistent');
  if (host.get('ptr_size') !== '4' || compiler.get('ptr_size') !== '4') throw Error('not wasm32 pointer ABI');
  for (const [key, value] of host) {
    if (compiler.get(key) !== value) throw Error('LLVM runtime ABI mismatch at ' + key);
  }
  const names = new Set<string>();
  for (const slot of metadata.slots) {
    if (!slot || (slot.kind !== 0 && slot.kind !== 3)) throw Error('unsupported AOT slot kind');
    if (!/^xyo_aot_slot_[0-9]+$/.test(slot.global) || names.has(slot.global)) throw Error('invalid AOT slot');
    if (!/^(?:[0-9a-f]{2})+$/.test(slot.keyHex) && slot.keyHex !== '') throw Error('invalid slot key');
    if (slot.kind === 0 && (!slot.keyHex || slot.keyHex.match(/../g)?.includes('00'))) throw Error('invalid variable ID');
    if (slot.kind === 3 && slot.keyHex.length % 4 !== 0) throw Error('invalid UTF-16 key');
    names.add(slot.global);
  }
}
