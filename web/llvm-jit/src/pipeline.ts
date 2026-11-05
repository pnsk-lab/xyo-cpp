import type { RunRequest, RunResult } from './protocol';

export interface PipelineToolClient {
  run(request: RunRequest): Promise<RunResult>;
  stop(): void;
  /** Terminate the current worker and reject its pending operations before replacing it. */
  restart(): void;
}
export interface StageDiagnostics { stdout: string; stderr: string }
export interface PipelineDiagnostics { llc?: StageDiagnostics; linker?: StageDiagnostics }
/** `sideModule` links a shared object with `--allow-undefined` instead of an
 * executable; imports are resolved by whatever loads it. */
export interface CompileOptions { ir: string; exports: string[]; sideModule?: true; exportMemory?: true }
export interface CompileResult { binary: Uint8Array; diagnostics: PipelineDiagnostics }
export class PipelineError extends Error {
  constructor(message: string, public readonly stage: 'llc' | 'linker' | 'validation' | 'cancelled', public readonly diagnostics: PipelineDiagnostics) {
    super(message); this.name = 'PipelineError';
  }
}

/** Latest request wins: a new compilation interrupts an active one by restarting its worker.
 * The client must be exclusive to this pipeline. stop() terminates it; restart it separately to reuse. */
function isDenseArray(value: unknown): value is unknown[] {
  if (!Array.isArray(value)) return false;
  for (let i = 0; i < value.length; i++) if (!Object.hasOwn(value, i)) return false;
  return true;
}

export class LLVMPipeline {
  private generation = 0;
  private active: number | undefined;
  constructor(private readonly client: PipelineToolClient) {}
  stop(): void { this.generation++; this.active = undefined; this.client.stop(); }
  async compile(options: CompileOptions): Promise<CompileResult> {
    const generation = ++this.generation;
    const diagnostics: PipelineDiagnostics = {};
    const interrupt = this.active !== undefined;
    this.active = generation;
    try {
      if (interrupt) {
        try { this.client.restart(); }
        catch (error) { throw new PipelineError(`Worker restart failed: ${String(error)}`, 'llc', diagnostics); }
      }
      return await this.compileActive(options, generation, diagnostics);
    } finally {
      if (this.active === generation) this.active = undefined;
    }
  }
  private async compileActive(options: CompileOptions, generation: number, diagnostics: PipelineDiagnostics): Promise<CompileResult> {
    const checkCurrent = () => {
      if (generation !== this.generation) throw new PipelineError('Compilation superseded or stopped', 'cancelled', diagnostics);
    };
    if (typeof options.ir !== 'string' || !isDenseArray(options.exports) || !options.exports.every(name => typeof name === 'string' && name.length > 0 && !name.startsWith('-') && !name.includes('\0'))) {
      throw new PipelineError('Invalid IR or export names', 'llc', diagnostics);
    }
    const side = options.sideModule === true;
    if (options.exportMemory !== undefined && options.exportMemory !== true) {
      throw new PipelineError('Invalid exportMemory flag', 'validation', diagnostics);
    }
    if (options.sideModule !== undefined && options.sideModule !== true) {
      throw new PipelineError('Invalid side-module flag', 'validation', diagnostics);
    }
    // Snapshot mutable caller input before the first await.
    const ir = options.ir, exports = [...options.exports];
    const run = async (stage: 'llc' | 'linker', request: RunRequest): Promise<RunResult> => {
      try {
        const result = await this.client.run(request);
        checkCurrent();
        diagnostics[stage] = { stdout: result.stdout, stderr: result.stderr };
        return result;
      } catch (error) {
        checkCurrent();
        if (error instanceof PipelineError) throw error;
        const detail = error as { message?: unknown; stdout?: unknown; stderr?: unknown } | null;
        diagnostics[stage] = { stdout: typeof detail?.stdout === 'string' ? detail.stdout : '', stderr: typeof detail?.stderr === 'string' ? detail.stderr : '' };
        throw new PipelineError(typeof detail?.message === 'string' ? detail.message : String(error), stage, diagnostics);
      }
    };
    const object = await run('llc', { tool: 'llc', args: [side ? '-mtriple=wasm32-unknown-emscripten' : '-mtriple=wasm32-unknown-unknown', ...(side ? ['-relocation-model=pic'] : []), '-filetype=obj', 'input.ll', '-o', 'output.o'], inputs: [{ path: 'input.ll', data: new TextEncoder().encode(ir) }], outputs: ['output.o'] });
    const objectBytes = object.files.find(file => file.path === 'output.o')?.data;
    if (!(objectBytes instanceof Uint8Array)) throw new PipelineError('llc did not produce output.o', 'llc', diagnostics);
    checkCurrent();
    const linked = await run('linker', { tool: 'wasm-ld', args: ['-flavor', 'wasm', ...(side ? ['--shared', '--allow-undefined'] : ['--no-entry', '--stack-first', '-z', 'stack-size=65536', '--initial-memory=131072']), ...(options.exportMemory ? ['--export-memory'] : []), ...exports.map(name => `--export=${name}`), 'input.o', '-o', 'output.wasm'], inputs: [{ path: 'input.o', data: objectBytes }], outputs: ['output.wasm'] });
    const linkedBytes = linked.files.find(file => file.path === 'output.wasm')?.data;
    // Copy into a fresh ArrayBuffer-backed view: some DOM lib typings only
    // accept ArrayBuffer-backed BufferSource for validate/compile.
    const binary = linkedBytes instanceof Uint8Array ? new Uint8Array(linkedBytes) : undefined;
    if (!binary || !WebAssembly.validate(binary)) throw new PipelineError('Linker output is not a valid WebAssembly module', 'validation', diagnostics);
    checkCurrent();
    return { binary: binary.slice(), diagnostics };
  }
}
