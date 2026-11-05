import { describe, expect, test } from 'bun:test';
import { LLVMWorkerClient, type WorkerLike } from '../src/client';
import { createWorkerHandler, runTool, type ToolFactories, type ToolFactory } from '../src/runtime';
import type { RunRequest, WorkerRequest } from '../src/protocol';

const wasm = new Uint8Array([0,97,115,109,1,0,0,0]);
const bytes = (s: string) => new TextEncoder().encode(s);
function fake(): ToolFactories {
  const factory: ToolFactory = options => {
    const files = new Map<string, Uint8Array>();
    const dirs = new Set<string>();
    return { FS: {
      mkdir(p) { if (dirs.has(p)) throw Error('directory exists'); dirs.add(p); },
      writeFile(p, data) { files.set(p, data.slice()); },
      readFile(p) { const value = files.get(p); if (!value) throw Error(`missing ${p}`); return value; },
    }, callMain(args) {
      options.print('fake stdout'); options.printErr('fake stderr');
      if (args[0] === 'fail') return 2;
      if (args[0] === 'throw') throw Error('fake failure');
      if (args[0] === 'exit') throw { status: 0 };
      if (args[0] === 'copy') { const source = args[1], destination = args[2]; if (!source || !destination) throw Error('missing path'); const data = files.get(source); if (!data) throw Error('missing source'); files.set(destination, data); }
      return 0;
    } };
  };
  return { llc: factory, 'wasm-ld': factory };
}
const request = (data = wasm): RunRequest => ({ tool: 'llc', args: ['copy', 'src/in', 'out/result'], inputs: [{ path: 'src/in', data }], outputs: ['out/result'] });

describe('fake tool runtime (not real LLVM)', () => {
  test('passes files and logs through separate tool invocations', async () => {
    const first = await runTool(fake(), request(bytes('object')));
    expect(first.stdout).toBe('fake stdout'); expect(first.stderr).toBe('fake stderr');
    const second = await runTool(fake(), { tool: 'wasm-ld', args: ['copy', 'obj/a', 'bin/a'], inputs: [{ path: 'obj/a', data: first.files[0]!.data }], outputs: ['bin/a'] });
    expect(second.files[0]!.data).toEqual(bytes('object'));
    await expect(runTool(fake(), { ...request(), inputs: [], args: ['copy', 'src/in', 'out/result'] })).rejects.toThrow('missing source');
  });
  test('rejects unsafe paths, duplicates and absent files', async () => {
    for (const path of ['/x', '../x', 'x/../y', 'x//y', 'x\\y', '.', 'x\0y']) await expect(runTool(fake(), { ...request(), outputs: [path] })).rejects.toThrow('Invalid relative');
    await expect(runTool(fake(), { ...request(), outputs: ['src/in'] })).rejects.toThrow('overlapping');
    await expect(runTool(fake(), { ...request(), args: [] })).rejects.toThrow('missing');
  });
  test('reports tool errors and captures diagnostics', async () => {
    try { await runTool(fake(), { ...request(), args: ['fail'] }); throw Error('expected failure'); }
    catch (e: any) { expect(e.message).toContain('status 2'); expect(e.stderr).toBe('fake stderr'); }
    await expect(runTool(fake(), { ...request(), args: ['throw'] })).rejects.toThrow('fake failure');
    await expect(runTool(fake(), { ...request(), args: ['exit'], outputs: [] })).resolves.toBeDefined();
  });
  test('validates final modules, not arbitrary objects', async () => {
    await expect(runTool(fake(), { ...request(), validateWasm: ['out/result'] })).resolves.toBeDefined();
    await expect(runTool(fake(), { ...request(bytes('not wasm')), validateWasm: ['out/result'] })).rejects.toThrow('Invalid WebAssembly');
    await expect(runTool(fake(), { ...request(), validateWasm: ['other'] })).rejects.toThrow('not an output');
  });
});

class FakeWorker implements WorkerLike {
  listeners: Record<string, ((e: any) => void)[]> = {};
  terminated = false;
  handler = createWorkerHandler(fake(), response => this.emit('message', { data: response }));
  postMessage(message: unknown) { this.handler(message as WorkerRequest); }
  terminate() { this.terminated = true; }
  addEventListener(type: string, fn: (event: any) => void) { (this.listeners[type] ??= []).push(fn); }
  emit(type: string, event: unknown) { for (const fn of this.listeners[type] ?? []) fn(event); }
}
describe('client lifecycle with fake worker', () => {
  test('parallel requests correlate responses and isolate FS', async () => {
    const client = new LLVMWorkerClient(() => new FakeWorker());
    const results = await Promise.all([client.run(request(bytes('one'))), client.run(request(bytes('two')))]);
    expect(results.map(r => r.files[0]!.data)).toEqual([bytes('one'), bytes('two')]); client.terminate();
  });
  test('stop rejects pending, restart ignores old responses and permits new work', async () => {
    const workers: FakeWorker[] = [];
    const client = new LLVMWorkerClient(() => { const w = new FakeWorker(); workers.push(w); return w; });
    const pending = client.run(request()); client.stop();
    await expect(pending).rejects.toThrow('stopped');
    expect(workers[0]!.terminated).toBe(true);
    await expect(client.run(request())).rejects.toThrow('stopped');
    client.restart();
    workers[0]!.emit('message', { data: { id: 2, ok: true, result: 'stale' } });
    expect((await client.run(request())).files[0]!.data).toEqual(wasm);
    const next = client.run(request()); workers[1]!.emit('error', {});
    await expect(next).rejects.toThrow('failed'); client.terminate();
  });
  test('propagates tool diagnostics', async () => {
    const client = new LLVMWorkerClient(() => new FakeWorker());
    try { await client.run({ ...request(), args: ['fail'] }); throw Error('expected'); }
    catch (e: any) { expect(e.message).toContain('status 2'); expect(e.stderr).toBe('fake stderr'); }
    client.stop();
  });
});
