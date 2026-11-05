import { describe, expect, test } from 'bun:test';
import { LLVMPipeline, PipelineError, type PipelineToolClient } from '../src/pipeline';
import type { RunRequest, RunResult } from '../src/protocol';
import { LLVMWorkerClient, type WorkerLike } from '../src/client';

// Pure fakes: these do not compile IR and do not exercise LLVM or LLD.
const wasm = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
const result = (path: string, data: Uint8Array): RunResult => ({ files: [{ path, data }], stdout: `${path} stdout`, stderr: `${path} stderr` });
class FakeClient implements PipelineToolClient {
  calls: RunRequest[] = [];
  stopped = false;
  restarts = 0;
  restart() { this.restarts++; this.stopped = false; }
  async run(request: RunRequest): Promise<RunResult> {
    this.calls.push(request);
    return request.tool === 'llc' ? result('output.o', new Uint8Array([1, 2])) : result('output.wasm', wasm);
  }
  stop() { this.stopped = true; }
}
const opts = { ir: '; 日本語', exports: ['add', 'main'] };
describe('fake IR-to-module pipeline (not real LLVM)', () => {
  test('transfers UTF-8 and object, supplies exports and preserves both diagnostics', async () => {
    const fake = new FakeClient();
    const output = await new LLVMPipeline(fake).compile(opts);
    expect(output.binary).toEqual(wasm);
    expect(new TextDecoder().decode(fake.calls[0]!.inputs[0]!.data)).toBe(opts.ir);
    expect(fake.calls[0]!.args).toContain('-filetype=obj');
    expect(fake.calls[1]!.inputs[0]!.data).toEqual(new Uint8Array([1, 2]));
    expect(fake.calls[1]!.args.slice(0, 2)).toEqual(['-flavor', 'wasm']);
    expect(fake.calls[1]!.args).toContain('--export=add');
    expect(fake.calls[1]!.args).toContain('--export=main');
    expect(output.diagnostics.llc?.stderr).toBe('output.o stderr');
    expect(output.diagnostics.linker?.stdout).toBe('output.wasm stdout');
  });
  test('standalone browser modules export memory and reserve stack', async () => {
    const fake = new FakeClient();
    await new LLVMPipeline(fake).compile({ ir: '', exports: ['xyo_script'], exportMemory: true });
    expect(fake.calls[1]!.args).toContain('--export-memory');
    expect(fake.calls[1]!.args).toContain('--stack-first');
    expect(fake.calls[1]!.args).toContain('--initial-memory=131072');
  });
  test('side modules use PIC/shared flags', async () => {
    const fake = new FakeClient();
    fake.run = async r => { fake.calls.push(r); return r.tool === 'llc' ? result('output.o', new Uint8Array([1])) : result('output.wasm', wasm); };
    await expect(new LLVMPipeline(fake).compile({ ir: '', exports: [], sideModule: true })).resolves.toHaveProperty('binary');
    expect(fake.calls[0]!.args).toContain('-relocation-model=pic');
    expect(fake.calls[0]!.args).toContain('-mtriple=wasm32-unknown-emscripten');
    expect(fake.calls[1]!.args).toContain('--shared');
    expect(fake.calls[1]!.args).toContain('--allow-undefined');
  });
  test('rejects sparse exports and a non-true side-module flag before invoking tools', async () => {
    const fake = new FakeClient();
    const pipeline = new LLVMPipeline(fake);
    await expect(pipeline.compile({ ir: '', exports: new Array(1) })).rejects.toMatchObject({ stage: 'llc' });
    await expect(pipeline.compile({ ir: '', exports: [], sideModule: false as unknown as true })).rejects.toMatchObject({ stage: 'validation' });
    expect(fake.calls).toHaveLength(0);
  });
  test('refuses bad module and propagates stage diagnostics on failure', async () => {
    const fake = new FakeClient();
    fake.run = async r => r.tool === 'llc' ? result('output.o', new Uint8Array([1])) : result('output.wasm', new Uint8Array([1]));
    try { await new LLVMPipeline(fake).compile(opts); throw Error('expected failure'); }
    catch (e) { expect(e).toBeInstanceOf(PipelineError); expect((e as PipelineError).stage).toBe('validation'); expect((e as PipelineError).diagnostics.linker?.stderr).toBe('output.wasm stderr'); expect('binary' in (e as object)).toBe(false); }
    fake.run = async r => { if (r.tool === 'llc') return result('output.o', new Uint8Array([1])); throw Object.assign(Error('link failed'), { stderr: 'link diagnostic', stdout: 'link output' }); };
    try { await new LLVMPipeline(fake).compile(opts); throw Error('expected failure'); }
    catch (e) { const error = e as PipelineError; expect(error.stage).toBe('linker'); expect(error.diagnostics.llc?.stdout).toBe('output.o stdout'); expect(error.diagnostics.linker?.stderr).toBe('link diagnostic'); expect('binary' in error).toBe(false); }
  });
  test('superseded compilation cannot emit a result or launch its linker', async () => {
    const fake = new FakeClient();
    let resolveFirst!: (value: RunResult) => void;
    let calls = 0;
    fake.run = async r => {
      if (calls++ === 0) return new Promise(resolve => { resolveFirst = resolve; });
      return r.tool === 'llc' ? result('output.o', new Uint8Array([1])) : result('output.wasm', wasm);
    };
    const pipeline = new LLVMPipeline(fake);
    const old = pipeline.compile(opts);
    const current = pipeline.compile(opts);
    expect(fake.restarts).toBe(1);
    await expect(current).resolves.toMatchObject({ binary: wasm });
    resolveFirst(result('output.o', new Uint8Array([1])));
    await expect(old).rejects.toMatchObject({ stage: 'cancelled' });
    expect(calls).toBe(3);
  });
  test('stop rejects an in-flight result even if a client completes after stop', async () => {
    const fake = new FakeClient();
    let resolve!: (value: RunResult) => void;
    fake.run = () => new Promise(r => { resolve = r; });
    const pipeline = new LLVMPipeline(fake);
    const pending = pipeline.compile(opts); pipeline.stop();
    resolve(result('output.o', new Uint8Array([1])));
    await expect(pending).rejects.toMatchObject({ stage: 'cancelled' });
    expect(fake.stopped).toBe(true);
  });
});
