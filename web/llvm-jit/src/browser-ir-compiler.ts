import type { RuntimeIR } from './runtime-ir';
import { verifyBrowserCompilerMetadata, type BrowserCompilerMetadata } from './compiler-metadata';

export interface XyoBrowserIrModule {
  HEAPU8: Uint8Array;
  _malloc(size: number): number;
  _free(ptr: number): void;
  _xyo_browser_ir_compile(bytes: number, length: number): number;
  _xyo_browser_ir_ok(result: number): number;
  _xyo_browser_ir_text(result: number): number;
  _xyo_browser_ir_metadata(result: number): number;
  _xyo_browser_ir_error(result: number): number;
  _xyo_browser_ir_destroy(result: number): void;
}

function readString(module: XyoBrowserIrModule, ptr: number, limit: number): string {
  if (ptr === 0 || ptr >= module.HEAPU8.length) throw Error('invalid C++ compiler string');
  let end = ptr;
  const max = Math.min(module.HEAPU8.length, ptr + limit);
  while (end < max && module.HEAPU8[end] !== 0) end++;
  if (end === max) throw Error('unterminated or oversized C++ compiler string');
  return new TextDecoder('utf-8', { fatal: true }).decode(module.HEAPU8.subarray(ptr, end));
}

/** Wraps xyo's actual jitEmitScript C++ frontend compiled with Emscripten.
 * The LLVM toolchain object must target wasm32, matching the xyo runtime's ABI.
 * No C++ compiler artifact is bundled; a missing build is not substituted for one.
 */
export class BrowserIrCompiler {
  constructor(private readonly module: XyoBrowserIrModule) {}

  compile(sb3: Uint8Array, hostDescriptor: string, expectedTarget = 0): RuntimeIR {
    if (!sb3.length || sb3.length > 16 * 1024 * 1024) throw Error('SB3 exceeds C++ compiler input limit');
    const m = this.module;
    const buffer = m._malloc(sb3.length);
    if (!buffer) throw Error('C++ compiler allocation failed');
    let result = 0;
    try {
      m.HEAPU8.set(sb3, buffer);
      result = m._xyo_browser_ir_compile(buffer, sb3.length);
      if (!result) throw Error('C++ compiler returned null');
      if (m._xyo_browser_ir_ok(result) !== 1) {
        throw Error(readString(m, m._xyo_browser_ir_error(result), 8192));
      }
      const ir = readString(m, m._xyo_browser_ir_text(result), 8 * 1024 * 1024);
      const raw = readString(m, m._xyo_browser_ir_metadata(result), 1024 * 1024);
      const metadata = JSON.parse(raw) as BrowserCompilerMetadata;
      verifyBrowserCompilerMetadata(metadata, hostDescriptor, expectedTarget);
      return {
        ir,
        slots: metadata.slots,
        hat: '', // resolved against the source project by UnifiedSB3Executor
        target: metadata.targetIndex,
        source: 'cpp-jit-emit',
      };
    } finally {
      if (result) m._xyo_browser_ir_destroy(result);
      m._free(buffer);
    }
  }
}
