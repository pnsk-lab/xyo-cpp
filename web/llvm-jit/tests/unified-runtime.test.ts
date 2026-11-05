import { describe, expect, test } from 'bun:test';
import { existsSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { zipSync, strToU8 } from 'fflate';
import { LLVMPipeline, type PipelineToolClient } from '../src/pipeline';
import { emitRuntimeIR } from '../src/runtime-ir';
import { verifyBrowserCompilerMetadata } from '../src/compiler-metadata';
import { BrowserIrCompiler, type XyoBrowserIrModule } from '../src/browser-ir-compiler';
import { UnifiedSB3Executor } from '../src/unified-runtime';
import type { RunRequest, RunResult } from '../src/protocol';

const host = join(import.meta.dir, '../native/build/xyo_browser_unified.mjs');
const cpp = join(import.meta.dir, '../native/build/xyo_browser_ir.mjs');
const hasCpp = existsSync(cpp) && existsSync(join(import.meta.dir, '../native/build/xyo_browser_ir.wasm'));
const hasHost = existsSync(host) && existsSync(join(import.meta.dir, '../native/build/xyo_browser_unified.wasm'));
class NativeLLVM implements PipelineToolClient {
  async run(request: RunRequest): Promise<RunResult> {
    const dir = mkdtempSync(join(import.meta.dir, '.unified-tool-'));
    try {
      for (const input of request.inputs) writeFileSync(join(dir, input.path), input.data);
      const cmd = request.tool === 'llc' ? 'llc' : 'wasm-ld';
      const result = Bun.spawnSync([cmd, ...request.args], { cwd: dir, stdout: 'pipe', stderr: 'pipe' });
      if (result.exitCode !== 0) throw Error(result.stderr.toString());
      return { stdout: result.stdout.toString(), stderr: result.stderr.toString(),
        files: request.outputs.map(path => ({ path, data: new Uint8Array(readFileSync(join(dir, path))) })) };
    } finally { rmSync(dir, { recursive: true, force: true }); }
  }
  stop() {}
  restart() {}
}
function example(next: string | null = 'change') {
  return {
    targets: [{
      isStage: true, name: 'Stage', variables: { score: ['score', 7] },
      lists: {}, broadcasts: {}, blocks: {
        hat: { opcode: 'event_whenflagclicked', next: 'set', parent: null, topLevel: true, x: 0, y: 0, inputs: {}, fields: {} },
        set: { opcode: 'data_setvariableto', next, parent: 'hat', inputs: { VALUE: [1, [4, '40']] }, fields: { VARIABLE: ['score', 'score'] } },
        change: { opcode: 'data_changevariableby', next: null, parent: 'set', inputs: { VALUE: [1, [4, '2']] }, fields: { VARIABLE: ['score', 'score'] } },
      },
      costumes: [], sounds: [], currentCostume: 0, volume: 100, layerOrder: 0,
    }],
    monitors: [], extensions: [], meta: { semver: '3.0.0' },
  };
}
const pack = (obj: unknown) => zipSync({ 'project.json': strToU8(JSON.stringify(obj)) });

describe('unified xyo Runtime using real PIC LLVM side module', () => {
  test('rejects native 64-bit compiler metadata against wasm32 runtime', () => {
    const layout = 'e-m:e-p:32:32-i64:64-n32:64-S128';
    const host = 'xyo-browser-ir-abi-v2;ptr_size=4;ctx_size=80;tag_base=7ff8000000000000';
    const valid = {
      version: 1,
      targetIndex: 0,
      targetTriple: 'wasm32-unknown-emscripten',
      dataLayout: layout,
      abiDescriptor: host + ';triple=wasm32-unknown-emscripten;layout=' + layout,
      slots: [{ kind: 0, global: 'xyo_aot_slot_0', keyHex: '73636f7265' }],
    };
    expect(() => verifyBrowserCompilerMetadata(valid, host, 0)).not.toThrow();
    expect(() => verifyBrowserCompilerMetadata({ ...valid, abiDescriptor: valid.abiDescriptor.replace('ptr_size=4', 'ptr_size=8') }, host, 0))
      .toThrow('not wasm32 pointer ABI');
    expect(() => verifyBrowserCompilerMetadata({ ...valid, abiDescriptor: valid.abiDescriptor.replace('ctx_size=80', 'ctx_size=144') }, host, 0))
      .toThrow('LLVM runtime ABI mismatch at ctx_size');
  });
  test('SB3 lowering creates runtime helper calls and real slot metadata', () => {
    const result = emitRuntimeIR(pack(example()));
    expect(result.ir).toContain('call void @xyo_var_change');
    expect(result.ir).toContain('call void @xyo_var_set');
    expect(result.slots).toEqual([{ kind: 0, global: 'xyo_aot_slot_0', keyHex: '73636f7265' }]);
  });
  test.skipIf(!hasHost)('real Emscripten host and side module update one xyo Runtime', async () => {
    const { default: createRuntime } = await import(pathToFileURL(host).href);
    const m = await createRuntime();
    const pipeline = new LLVMPipeline(new NativeLLVM());
    const executor = new UnifiedSB3Executor(m, pipeline);
    try {
      await executor.load(pack(example()));
      expect(executor.mode).toBe('llvm');
      executor.greenFlag();
      executor.step();
      const snapshot = executor.snapshot() as any;
      expect(snapshot.targets[0].variables.score).toBe('42');
    } finally { executor.dispose(); pipeline.stop(); }
  });
  test.skipIf(!hasHost || !hasCpp)('real C++ jitEmitScript compiled as wasm32 executes on the current xyo Runtime', async () => {
    const [{ default: createRuntime }, { default: createCompiler }] = await Promise.all([
      import(pathToFileURL(host).href),
      import(pathToFileURL(cpp).href),
    ]);
    const runtime = await createRuntime();
    const compiler = await createCompiler();
    const pipeline = new LLVMPipeline(new NativeLLVM());
    const executor = new UnifiedSB3Executor(runtime, pipeline, new BrowserIrCompiler(compiler));
    try {
      await executor.load(pack(example(null)));
      expect(executor.mode).toBe('llvm');
      expect(executor.compilerBackend).toBe('cpp-jit-emit');
      executor.greenFlag();
      executor.step();
      expect((executor.snapshot() as any).targets[0].variables.score).toBe('40');
    } finally { executor.dispose(); pipeline.stop(); }
  });
  test.skipIf(!hasHost)('optional C++ compiler ABI is checked and its IR executes on the real host', async () => {
    const { default: createRuntime } = await import(pathToFileURL(host).href);
    const runtime = await createRuntime();
    const hostBytes = runtime.HEAPU8 as Uint8Array;
    const hostPtr = runtime._xyo_browser_abi_descriptor();
    let end = hostPtr;
    while (hostBytes[end]) end++;
    const abi = new TextDecoder().decode(hostBytes.subarray(hostPtr, end));
    const input = pack(example());
    const lowered = emitRuntimeIR(input);
    const layout = 'e-m:e-p:32:32-i64:64-n32:64-S128';
    const writer = new TextEncoder();
    const memory = new Uint8Array(new ArrayBuffer(20000));
    let cursor = 1024;
    const put = (value: string) => {
      const pos = cursor, bytes = writer.encode(value + '\0');
      memory.set(bytes, pos);
      cursor += bytes.length;
      return pos;
    };
    const irPtr = put(lowered.ir);
    const metaPtr = put(JSON.stringify({
      version: 1, targetIndex: 0, targetTriple: 'wasm32-unknown-emscripten',
      dataLayout: layout,
      abiDescriptor: abi + ';triple=wasm32-unknown-emscripten;layout=' + layout,
      slots: lowered.slots,
    }));
    const api: XyoBrowserIrModule = {
      HEAPU8: memory,
      _malloc: () => 16,
      _free: () => {},
      _xyo_browser_ir_compile: () => 64,
      _xyo_browser_ir_ok: () => 1,
      _xyo_browser_ir_text: () => irPtr,
      _xyo_browser_ir_metadata: () => metaPtr,
      _xyo_browser_ir_error: () => 0,
      _xyo_browser_ir_destroy: () => {},
    };
    const pipeline = new LLVMPipeline(new NativeLLVM());
    const executor = new UnifiedSB3Executor(runtime, pipeline, new BrowserIrCompiler(api));
    try {
      await executor.load(input);
      expect(executor.mode).toBe('llvm');
      expect(executor.compilerBackend).toBe('cpp-jit-emit');
      executor.greenFlag();
      executor.step();
      expect((executor.snapshot() as any).targets[0].variables.score).toBe('42');
    } finally { executor.dispose(); pipeline.stop(); }
  });
  test.skipIf(!hasHost)('unsupported scripts fall back to xyo interpreter', async () => {
    const { default: createRuntime } = await import(pathToFileURL(host).href);
    const m = await createRuntime();
    const executor = new UnifiedSB3Executor(m, new LLVMPipeline(new NativeLLVM()));
    try {
      const project = example();
      project.targets[0].blocks.set.opcode = 'data_setvariableto';
      project.targets[0].blocks.hat.next = 'set';
      // A second top-level hat makes the LLVM gate refuse the project.
      const blocks = project.targets[0].blocks as Record<string, any>;
      blocks.extra = { opcode: 'event_whenflagclicked', next: null, topLevel: true, parent: null, inputs: {}, fields: {} };
      await executor.load(pack(project));
      expect(executor.mode).toBe('interpreter');
      executor.greenFlag();
      executor.step();
      expect((executor.snapshot() as any).targets[0].variables.score).toBe('42');
    } finally { executor.dispose(); }
  });
});
