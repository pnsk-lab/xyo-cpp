import { createLLVMWorkerClient } from './client';
import { LLVMPipeline } from './pipeline';
import { UnifiedSB3Executor } from './unified-runtime';
import { loadRuntime, loadCppCompiler } from './emscripten-modules';
import llcWorkerUrl from './worker.ts?worker&url';

interface Run {
  id: number;
  bytes: Uint8Array;
  steps: number;
  origin: string;
}
self.onmessage = async (event: MessageEvent<Run>): Promise<void> => {
  const { id, bytes, steps, origin } = event.data;
  const client = createLLVMWorkerClient(
    new URL(llcWorkerUrl, origin),
    new URL('/llvm-assets/llc-browser.mjs', origin),
    new URL('/llvm-assets/lld-browser.mjs', origin),
  );
  const compiler = new LLVMPipeline(client);
  let executor: UnifiedSB3Executor | undefined;
  try {
    if (!(bytes instanceof Uint8Array) || bytes.length > 16 * 1024 * 1024) throw Error('SB3 exceeds limit');
    const runtime = await loadRuntime();
    const cppCompiler = await loadCppCompiler();
    executor = new UnifiedSB3Executor(runtime, compiler, cppCompiler);
    await executor.load(bytes);
    const mode = executor.mode;
    const backend = executor.compilerBackend;
    const diagnostic = executor.compileDiagnostic;
    executor.greenFlag();
    for (let i = 0; i < Math.max(0, Math.min(steps, 300)); i++) executor.step();
    const snapshot = executor.snapshot();
    self.postMessage({ id, ok: true, mode, backend, diagnostic, snapshot });
  } catch (error) {
    self.postMessage({ id, ok: false, error: error instanceof Error ? error.message : String(error) });
  } finally {
    try { executor?.dispose(); } catch { /* isolate and discard failed module */ }
    compiler.stop();
  }
};
