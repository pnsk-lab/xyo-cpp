import { createLLVMWorkerClient } from './client';
import { LLVMPipeline } from './pipeline';
import { loadRuntime, loadCppCompiler } from './emscripten-modules';
import { UnifiedSB3Executor } from './unified-runtime';
import toolWorkerUrl from './worker.ts?worker&url';

type Request =
  | { id: number; type: 'load'; bytes: Uint8Array; origin: string }
  | { id: number; type: 'greenFlag' | 'step' | 'stop' | 'snapshot' }
  | { id: number; type: 'key'; key: string; down: boolean }
  | { id: number; type: 'mouse'; x: number; y: number; down: boolean; button: number; hasDown: boolean }
  | { id: number; type: 'wheel'; dx: number; dy: number }
  | { id: number; type: 'ackAudio'; soundId: number }
  | { id: number; type: 'answer'; text: string };

let runner: UnifiedSB3Executor | undefined;
let compiler: LLVMPipeline | undefined;
let busy = Promise.resolve();

const emit = (id: number, payload: Record<string, unknown>): void => {
  self.postMessage({ id, ...payload });
};

async function dispatch(request: Request): Promise<void> {
  const { id } = request;
  try {
    if (request.type === 'load') {
      if (!(request.bytes instanceof Uint8Array) || request.bytes.length > 16 * 1024 * 1024)
        throw Error('SB3 ZIP exceeds 16MiB limit');
      runner?.dispose();
      compiler?.stop();
      runner = undefined;
      compiler = undefined;
      const client = createLLVMWorkerClient(
        new URL(toolWorkerUrl, request.origin),
        new URL('/llvm-assets/llc-browser.mjs', request.origin),
        new URL('/llvm-assets/lld-browser.mjs', request.origin),
      );
      compiler = new LLVMPipeline(client);
      const [runtime, cppCompiler] = await Promise.all([loadRuntime(), loadCppCompiler()]);
      runner = new UnifiedSB3Executor(runtime, compiler, cppCompiler);
      await runner.load(request.bytes);
      emit(id, { ok: true, type: 'loaded', mode: runner.mode,
        backend: runner.compilerBackend, diagnostic: runner.compileDiagnostic,
        snapshot: runner.snapshot(), audio: runner.audioEvents() });
      return;
    }
    if (!runner) throw Error('Load a project before using the player');
    switch (request.type) {
      case 'greenFlag': runner.greenFlag(); break;
      case 'step': runner.step(); break;
      case 'stop': runner.stop(); break;
      case 'snapshot': break;
      case 'key': runner.key(request.key, request.down); break;
      case 'mouse': runner.mouse(request.x, request.y, request.down, request.button, request.hasDown); break;
      case 'wheel': runner.wheel(request.dx, request.dy); break;
      case 'ackAudio': runner.completeSound(request.soundId); break;
      case 'answer': runner.answer(request.text); break;
    }
    emit(id, { ok: true, type: request.type,
      ...(request.type === 'key' || request.type === 'mouse' || request.type === 'wheel' || request.type === 'ackAudio'
        ? {} : { snapshot: runner.snapshot(), audio: runner.audioEvents() }) });
  } catch (error) {
    emit(id, { ok: false, type: request.type,
      error: error instanceof Error ? error.message : String(error) });
  }
}

self.addEventListener('message', (event: MessageEvent<Request>) => {
  // Process all events in order, including key changes before a step.
  busy = busy.then(() => dispatch(event.data)).catch(error => {
    emit(event.data.id, { ok: false, error: String(error) });
  });
});
