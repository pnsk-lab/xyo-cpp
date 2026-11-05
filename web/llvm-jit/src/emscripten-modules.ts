import { BrowserIrCompiler, type XyoBrowserIrModule } from './browser-ir-compiler';
import type { XyoUnifiedModule } from './unified-runtime';

export async function loadEmscriptenModule<T>(stem: string): Promise<T> {
  if (!/^[a-z][a-z0-9_]*$/.test(stem)) throw Error('invalid Emscripten artifact name');
  const root = '/llvm-assets/';
  const [javascript, binary] = await Promise.all([
    fetch(root + stem + '.mjs'), fetch(root + stem + '.wasm'),
  ]);
  if (!javascript.ok || !binary.ok) throw Error(stem + ' artifacts are not installed');

  // Emscripten resolves the Wasm name relative to import.meta.url even when
  // wasmBinary is supplied. A blob: module URL therefore breaks findWasmBinary.
  // Import trusted same-origin static assets directly instead.
  const moduleUrl = new URL(root + stem + '.mjs', self.location.origin).href;
  const imported = await import(/* @vite-ignore */ moduleUrl) as {
    default: (options: {
      wasmBinary: Uint8Array<ArrayBuffer>;
      locateFile: (path: string) => string;
    }) => Promise<T>
  };
  if (typeof imported.default !== 'function') throw Error('invalid Emscripten factory');
  return imported.default({
    wasmBinary: new Uint8Array(await binary.arrayBuffer()),
    locateFile: (path) => new URL(root + path, self.location.origin).href,
  });
}
export const loadRuntime = () => loadEmscriptenModule<XyoUnifiedModule>('xyo_browser_unified');

export async function loadCppCompiler(): Promise<BrowserIrCompiler | undefined> {
  try {
    return new BrowserIrCompiler(await loadEmscriptenModule<XyoBrowserIrModule>('xyo_browser_ir'));
  } catch (error) {
    console.warn('optional C++ frontend unavailable:', error);
    return undefined;
  }
}
