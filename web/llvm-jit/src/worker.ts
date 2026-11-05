import { createWorkerHandler, type ToolFactory } from './runtime';
import type { WorkerRequest } from './protocol';

// Construct this module worker with ?llc=<absolute-url>&wasm-ld=<absolute-url>.
// URLs must point to trusted Emscripten ES module factory scripts; their locateFile
// defaults must resolve the corresponding .wasm assets correctly.
const params = new URL(self.location.href).searchParams;
function loadFactory(name: string): ToolFactory {
  return async options => {
    const url = params.get(name);
    if (!url) throw new Error(`Missing ${name} factory URL`);
    const imported = await import(/* @vite-ignore */ url);
    if (typeof imported.default !== 'function') throw new Error(`Invalid ${name} factory`);
    return imported.default(options);
  };
}
const handle = createWorkerHandler({ llc: loadFactory('llc'), 'wasm-ld': loadFactory('wasm-ld') }, response => self.postMessage(response));
self.addEventListener('message', (event: MessageEvent<WorkerRequest>) => handle(event.data));
