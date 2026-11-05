import { test, expect } from 'bun:test';
import { existsSync } from 'node:fs';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';
import { zipSync, strToU8 } from 'fflate';
import { BrowserIrCompiler } from '../src/browser-ir-compiler';
import { LLVMPipeline, type PipelineToolClient } from '../src/pipeline';
import { runTool, type ToolFactories } from '../src/runtime';
import { UnifiedSB3Executor } from '../src/unified-runtime';
import type { RunRequest, RunResult } from '../src/protocol';

const assets = join(import.meta.dir, '../public/llvm-assets');
const required = ['llc-browser.mjs', 'llc-browser.wasm', 'lld-browser.mjs', 'lld-browser.wasm',
  'xyo_browser_ir.mjs', 'xyo_browser_ir.wasm', 'xyo_browser_unified.mjs', 'xyo_browser_unified.wasm'];
const ready = required.every(name => existsSync(join(assets, name)));

class WasmLLVM implements PipelineToolClient {
  private factories!: ToolFactories;
  async start() {
    const [{ default: llc }, { default: lld }] = await Promise.all([
      import(pathToFileURL(join(assets, 'llc-browser.mjs')).href),
      import(pathToFileURL(join(assets, 'lld-browser.mjs')).href),
    ]);
    this.factories = { llc, 'wasm-ld': lld };
  }
  run(request: RunRequest): Promise<RunResult> {
    return runTool(this.factories, request);
  }
  restart() {}
  stop() {}
}
const input = zipSync({ 'project.json': strToU8(JSON.stringify({
  targets: [{
    isStage: true, name: 'Stage', variables: { score: ['score', 0] }, lists: {}, broadcasts: {},
    blocks: {
      hat: { opcode: 'event_whenflagclicked', next: 'set', parent: null, inputs: {}, fields: {},
        topLevel: true, x: 0, y: 0 },
      set: { opcode: 'data_setvariableto', next: null, parent: 'hat',
        inputs: { VALUE: [1, [4, '42']] }, fields: { VARIABLE: ['score', 'score'] } },
    }, costumes: [], sounds: [], currentCostume: 0, volume: 100, layerOrder: 0,
  }],
  monitors: [], extensions: [], meta: { semver: '3.0.0' },
})) });

test.skipIf(!ready)('all-wasm: real C++ jitEmitScript -> wasm32 llc -> wasm32 LLD -> dlopen xyo runtime', async () => {
  const [{ default: runtimeFactory }, { default: compilerFactory }] = await Promise.all([
    import(pathToFileURL(join(assets, 'xyo_browser_unified.mjs')).href),
    import(pathToFileURL(join(assets, 'xyo_browser_ir.mjs')).href),
  ]);
  const tools = new WasmLLVM();
  await tools.start();
  const [host, cpp] = await Promise.all([runtimeFactory(), compilerFactory()]);
  const pipeline = new LLVMPipeline(tools);
  const runner = new UnifiedSB3Executor(host, pipeline, new BrowserIrCompiler(cpp));
  try {
    await runner.load(input);
    expect(runner.mode).toBe('llvm');
    expect(runner.compilerBackend).toBe('cpp-jit-emit');
    runner.greenFlag();
    runner.step();
    expect((runner.snapshot() as any).targets[0].variables.score).toBe('42');
  } finally {
    runner.dispose();
    pipeline.stop();
  }
});
