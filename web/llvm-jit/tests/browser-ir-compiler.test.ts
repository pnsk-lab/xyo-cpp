import { describe, expect, test } from 'bun:test';
import { BrowserIrCompiler, type XyoBrowserIrModule } from '../src/browser-ir-compiler';
import { emitRuntimeIR } from '../src/runtime-ir';
import { zipSync, strToU8 } from 'fflate';

const layout = 'e-m:e-p:32:32-i64:64-n32:64-S128';
const host = 'xyo-browser-ir-abi-v2;ptr_size=4;ptr_align=4;ctx_size=80;helper_xyo_varslot_init=void(ptr,i64,ptr,ptr,i32);helper_xyo_to_num=double(i64)';
const source = zipSync({ 'project.json': strToU8(JSON.stringify({
  targets: [{ isStage: true, name: 'Stage', variables: { v: ['v', 0] }, lists: {},
    blocks: { hat: { opcode: 'event_whenflagclicked', next: 'set', parent: null, topLevel: true, inputs: {}, fields: {} },
      set: { opcode: 'data_setvariableto', next: null, parent: 'hat', inputs: { VALUE: [1, [4, '42']] },
        fields: { VARIABLE: ['v', 'v'] } } } }],
  monitors: [], extensions: [], meta: { semver: '3.0.0' },
})) });

function fakeCompiler(descriptor: string, error?: string) {
  const memory = new Uint8Array(new ArrayBuffer(16384));
  const writer = new TextEncoder();
  let offset = 1000, destroyed = 0, freed = 0;
  const str = (s: string): number => {
    const p = offset, bytes = writer.encode(s + '\0');
    memory.set(bytes, p);
    offset += bytes.length;
    return p;
  };
  const fallback = emitRuntimeIR(source);
  const irPtr = str(fallback.ir);
  const metaPtr = str(JSON.stringify({
    version: 1, targetIndex: 0, targetTriple: 'wasm32-unknown-emscripten', dataLayout: layout,
    abiDescriptor: descriptor + ';triple=wasm32-unknown-emscripten;layout=' + layout, slots: fallback.slots,
  }));
  const errorPtr = str(error ?? '');
  const module: XyoBrowserIrModule = {
    HEAPU8: memory,
    _malloc: () => 64,
    _free: () => { freed++; },
    _xyo_browser_ir_compile: () => 512,
    _xyo_browser_ir_ok: () => error ? 0 : 1,
    _xyo_browser_ir_text: () => irPtr,
    _xyo_browser_ir_metadata: () => metaPtr,
    _xyo_browser_ir_error: () => errorPtr,
    _xyo_browser_ir_destroy: () => { destroyed++; },
  };
  return { compiler: new BrowserIrCompiler(module), counts: () => [destroyed, freed] };
}

describe('C++ jitEmitScript Emscripten browser adapter', () => {
  test('reads returned IR and validated native slot metadata', () => {
    const { compiler, counts } = fakeCompiler(host);
    const result = compiler.compile(source, host);
    expect(result.source).toBe('cpp-jit-emit');
    expect(result.slots[0]?.keyHex).toBe('76');
    expect(result.ir).toContain('xyo_var_set');
    expect(counts()).toEqual([1, 1]);
  });
  test('rejects host-ABI mismatch and always frees results', () => {
    const { compiler, counts } = fakeCompiler(host.replace('ctx_size=80', 'ctx_size=144'));
    expect(() => compiler.compile(source, host)).toThrow('LLVM runtime ABI mismatch');
    expect(counts()).toEqual([1, 1]);
  });
  test('reports C++ frontend rejection without leaking its output', () => {
    const { compiler, counts } = fakeCompiler(host, 'C++ gate refused yield');
    expect(() => compiler.compile(source, host)).toThrow('C++ gate refused yield');
    expect(counts()).toEqual([1, 1]);
  });
});
