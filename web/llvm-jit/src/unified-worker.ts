import { createLLVMWorkerClient } from './client';
import { LLVMPipeline } from './pipeline';
import { UnifiedSB3Executor, type XyoUnifiedModule } from './unified-runtime';
import llcWorkerUrl from './worker.ts?worker&url';

interface Run {
  id: number;
  bytes: Uint8Array;
  steps: number;
}
const js = '/llvm-assets/xyo_browser_unified.mjs';
const wasm = '/llvm-assets/xyo_browser_unified.wasm';

async function factory(): Promise<XyoUnifiedModule> {
  const [javascript, binary] = await Promise.all([fetch(js), fetch(wasm)]);
  if (!javascript.ok || !binary.ok) throw Error('xyo unified runtime artifacts are missing (build and copy native host first)');
  const source = await javascript.text();
  const url = URL.createObjectURL(new Blob([source], { type: 'text/javascript' }));
  try {
    const imported = await import(/* @vite-ignore */ url) as {
      default: (options: { wasmBinary: Uint8Array<ArrayBuffer> }) => Promise<XyoUnifiedModule>
    };
    return await imported.default({ wasmBinary: new Uint8Array(await binary.arrayBuffer()) });
  } finally { URL.revokeObjectURL(url); }
}

self.onmessage = async (event: MessageEvent<Run>): Promise<void> => {
  const { id, bytes, steps } = event.data;
  const client = createLLVMWorkerClient(
    llcWorkerUrl,
    new URL('/llvm-assets/llc-browser.mjs', self.location.href),
    new URL('/llvm-assets/lld-browser.mjs', self.location.href),
  );
  const compiler = new LLVMPipeline(client);
  let executor: UnifiedSB3Executor | undefined;
  try {
    if (!(bytes instanceof Uint8Array) || bytes.length > 16 * 1024 * 1024) throw Error('SB3 exceeds limit');
    const runtime = await factory();
    executor = new UnifiedSB3Executor(runtime, compiler);
    await executor.load(bytes);
    const mode = executor.mode;
    executor.greenFlag();
    for (let i = 0; i < Math.max(0, Math.min(steps, 300)); i++) executor.step();
    const snapshot = executor.snapshot();
    self.postMessage({ id, ok: true, mode, snapshot });
  } catch (error) {
    self.postMessage({ id, ok: false, error: error instanceof Error ? error.message : String(error) });
  } finally {
    try { executor?.dispose(); } catch { /* isolate and discard failed module */ }
    compiler.stop();
  }
};
