import { createWorkerHandler, type ToolFactory } from './runtime';
import type { WorkerRequest } from './protocol';

// Construct this module worker with ?llc=<absolute-url>&wasm-ld=<absolute-url>.
// URLs must point to trusted Emscripten ES module factory scripts; their locateFile
// defaults must resolve the corresponding .wasm assets correctly.
const params = new URL(self.location.href).searchParams;
let toolUrls: { llc: string; 'wasm-ld': string } | undefined;
function loadFactory(name: string): ToolFactory {
  return async options => {
    const url = toolUrls?.[name as 'llc' | 'wasm-ld'] ?? params.get(name);
    if (!url) throw new Error(`Missing ${name} factory URL`);
    const imported = await import(/* @vite-ignore */ url);
    if (typeof imported.default !== 'function') throw new Error(`Invalid ${name} factory`);
    return imported.default(options);
  };
}
const handle = createWorkerHandler({ llc: loadFactory('llc'), 'wasm-ld': loadFactory('wasm-ld') }, response => self.postMessage(response));
self.addEventListener('message', (event: MessageEvent<WorkerRequest>) => {
  const candidate = event.data.toolUrls;
  if (candidate) {
    for (const url of [candidate.llc, candidate['wasm-ld']]) {
      if (!url || new URL(url).origin !== self.location.origin) {
        self.postMessage({ id: event.data.id, ok: false, error: { message: 'Untrusted LLVM factory URL', stdout: '', stderr: '' } });
        return;
      }
    }
    toolUrls = candidate;
  }
  handle(event.data);
});
