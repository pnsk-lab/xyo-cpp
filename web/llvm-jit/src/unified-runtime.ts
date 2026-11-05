import { zipSync, strToU8, unzipSync } from 'fflate';
import { splitProjectForLlvm } from './split';
import { emitRuntimeIR, type RuntimeIR } from './runtime-ir';
import type { BrowserIrCompiler } from './browser-ir-compiler';
import type { LLVMPipeline } from './pipeline';

export interface XyoUnifiedModule {
  HEAPU8: Uint8Array;
  FS: { writeFile(path: string, bytes: Uint8Array): void };
  _malloc(length: number): number;
  _free(pointer: number): void;
  _xyo_browser_create(): number;
  _xyo_browser_destroy(handle: number): number;
  _xyo_browser_load_sb3(handle: number, ptr: number, length: number): number;
  _xyo_browser_green_flag(handle: number): number;
  _xyo_browser_step(handle: number): number;
  _xyo_browser_stop(handle: number): number;
  _xyo_browser_snapshot(handle: number): number;
  _xyo_browser_error(handle: number): number;
  _xyo_browser_abi_descriptor(handle?: number): number;
  _xyo_llvm_prepare(handle: number, hat: number): number;
  _xyo_llvm_expect_slots(handle: number, count: number): number;
  _xyo_llvm_bind_slot(handle: number, address: number, kind: number, key: number, size: number): number;
  _xyo_llvm_is_sealed(handle: number): number;
  _xyo_llvm_dlopen(handle: number, path: number): number;
  _xyo_llvm_dlsym(opened: number, name: number): number;
  _xyo_llvm_invoke(handle: number, entry: number): void;
  _xyo_llvm_release(handle: number): void;
}

/** Runs a narrowly admitted SB3 script in the same xyo::Runtime as the
 * interpreter. Unsupported input is interpreted in full, never dropped.
 * The codegen is a limited TS bridge; the C++ jitEmitScript metadata compiler
 * will replace it when LLVM itself is available as an Emscripten binary.
 */
export class UnifiedSB3Executor {
  readonly handle: number;
  private entry = 0;
  private disposed = false;
  private compiled = false;
  private frontend: RuntimeIR['source'] | undefined;
  private fallbackDiagnostic: string | undefined;
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder();

  constructor(private readonly m: XyoUnifiedModule, private readonly compiler: LLVMPipeline,
              private readonly cppCompiler?: BrowserIrCompiler) {
    this.handle = m._xyo_browser_create();
    if (!this.handle) throw Error('xyo browser create failed');
  }

  get mode(): 'llvm' | 'interpreter' { return this.compiled ? 'llvm' : 'interpreter'; }
  get compilerBackend(): RuntimeIR['source'] | undefined { return this.frontend; }
  get compileDiagnostic(): string | undefined { return this.fallbackDiagnostic; }

  private ensureActive() { if (this.disposed) throw Error('unified executor already disposed'); }
  private withBytes<T>(bytes: Uint8Array, f: (ptr: number) => T): T {
    const ptr = this.m._malloc(Math.max(bytes.length, 1));
    if (!ptr) throw Error('WASM allocation failed');
    try {
      this.m.HEAPU8.set(bytes, ptr);
      return f(ptr);
    } finally { this.m._free(ptr); }
  }
  private withCString<T>(value: string, f: (ptr: number) => T): T {
    return this.withBytes(this.encoder.encode(value + '\0'), f);
  }
  private readCString(ptr: number): string {
    if (!ptr) return '';
    const bytes = this.m.HEAPU8;
    let end = ptr;
    while (end < bytes.length && bytes[end]) end++;
    return this.decoder.decode(bytes.subarray(ptr, end));
  }
  private error(): Error { return Error(this.readCString(this.m._xyo_browser_error(this.handle)) || 'xyo runtime call failed'); }
  private checkAbi(): void {
    const descriptor = this.readCString(this.m._xyo_browser_abi_descriptor());
    if (!descriptor.startsWith('xyo-browser-ir-abi-v2;') || !descriptor.includes(';ptr_size=4;') ||
        !descriptor.includes(';helper_xyo_varslot_init=void(ptr,i64,ptr,ptr,i32);')) {
      throw Error('incompatible current xyo browser runtime ABI');
    }
  }
  private loadBytes(sb3: Uint8Array): void {
    const ok = this.withBytes(sb3, p => this.m._xyo_browser_load_sb3(this.handle, p, sb3.length));
    if (ok !== 1) throw this.error();
  }

  async load(sb3: Uint8Array): Promise<void> {
    this.ensureActive();
    this.m._xyo_llvm_release(this.handle);
    this.entry = 0;
    this.compiled = false;
    this.frontend = undefined;
    this.fallbackDiagnostic = undefined;
    // No filtering of the project unless compilation and linking succeed.
    let ir: RuntimeIR | undefined;
    let side: Uint8Array | undefined;
    try {
      this.checkAbi();
      // The restricted admission check is intentionally the same regardless
      // of frontend. Never run general C++ output without the yield/ABI gate.
      ir = emitRuntimeIR(sb3);
      if (this.cppCompiler) {
        const native = this.cppCompiler.compile(sb3, this.readCString(this.m._xyo_browser_abi_descriptor()), ir.target);
        ir = { ...native, hat: ir.hat };
      }
      const result = await this.compiler.compile({
        ir: ir.ir,
        exports: ['xyo_browser_script', ...ir.slots.map(s => s.global)],
        sideModule: true,
      });
      side = result.binary;
    } catch (error) {
      this.fallbackDiagnostic = error instanceof Error ? error.message : String(error);
      this.loadBytes(sb3);
      return;
    }
    this.ensureActive();
    // The splitter retains the hat but removes its interpreted body; only
    // activate that split if the module and all slots bind successfully.
    let archive: Record<string, Uint8Array>;
    try {
      let total = 0;
      archive = unzipSync(sb3, { filter: (file) => {
        if (file.originalSize > 16 * 1024 * 1024) throw Error('oversized ZIP entry');
        total += file.originalSize;
        if (total > 64 * 1024 * 1024) throw Error('oversized ZIP');
        return true;
      }});
    } catch { this.loadBytes(sb3); return; }
    const json = archive['project.json'];
    if (!json) { this.loadBytes(sb3); return; }
    const project = JSON.parse(new TextDecoder().decode(json)) as unknown;
    const split = splitProjectForLlvm(project);
    if (!split.ok || split.admitted.length !== 1 || split.skipped.length !== 0 || !ir || !side) {
      this.loadBytes(sb3);
      return;
    }
    const remainder = zipSync({ ...archive, 'project.json': strToU8(JSON.stringify(split.rest)) });
    this.loadBytes(remainder);
    let success = false;
    try {
      if (!this.withCString(ir.hat, p => this.m._xyo_llvm_prepare(this.handle, p))) throw this.error();
      if (!this.m._xyo_llvm_expect_slots(this.handle, ir.slots.length)) throw Error('expected slots rejected');
      this.m.FS.writeFile('/xyo-compiled-side.wasm', side);
      const opened = this.withCString('/xyo-compiled-side.wasm', p => this.m._xyo_llvm_dlopen(this.handle, p));
      if (!opened) throw this.error();
      const sym = (name: string): number => this.withCString(name, p => this.m._xyo_llvm_dlsym(opened, p));
      for (const slot of ir.slots) {
        const address = sym(slot.global);
        if (!address) throw Error('missing side module slot ' + slot.global);
        const key = Uint8Array.from(slot.keyHex.match(/../g) ?? [], b => parseInt(b, 16));
        if (this.withBytes(key, p => this.m._xyo_llvm_bind_slot(this.handle, address, slot.kind, p, key.length)) !== 1) {
          throw Error('side module slot binding rejected');
        }
      }
      if (!this.m._xyo_llvm_is_sealed(this.handle)) throw Error('unsealed side module');
      this.entry = sym('xyo_browser_script');
      if (!this.entry) throw Error('missing side module entry');
      this.compiled = true;
      this.frontend = ir.source;
      success = true;
    } finally {
      if (!success) {
        this.entry = 0;
        this.m._xyo_llvm_release(this.handle);
        this.loadBytes(sb3);
      }
    }
  }

  greenFlag(): void {
    this.ensureActive();
    if (this.m._xyo_browser_green_flag(this.handle) !== 1) throw this.error();
    if (this.entry) this.m._xyo_llvm_invoke(this.handle, this.entry);
  }
  step(): void {
    this.ensureActive();
    if (this.m._xyo_browser_step(this.handle) !== 1) throw this.error();
  }
  snapshot(): unknown {
    this.ensureActive();
    const ptr = this.m._xyo_browser_snapshot(this.handle);
    if (!ptr) throw this.error();
    return JSON.parse(this.readCString(ptr));
  }
  stop(): void {
    this.ensureActive();
    if (this.m._xyo_browser_stop(this.handle) !== 1) throw this.error();
  }
  dispose(): void {
    if (this.disposed) return;
    this.disposed = true;
    this.m._xyo_browser_destroy(this.handle);
  }
}
