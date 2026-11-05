import { describe, expect, test } from 'bun:test';
import { zipSync, strToU8 } from 'fflate';
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { LLVMPipeline, type PipelineToolClient } from '../src/pipeline';
import type { RunRequest, RunResult } from '../src/protocol';
import { admitSb3, decodeSb3, executeSb3 } from '../src/sb3';

function project(blocks: Record<string, unknown>, value: string | number = 7) {
  return {
    targets: [{ isStage: true, name: 'Stage', variables: { var1: ['score', value] }, lists: {}, blocks }],
    monitors: [], extensions: [], meta: { semver: '3.0.0' },
  };
}
const hat = { opcode: 'event_whenflagclicked', topLevel: true, next: 'a', parent: null, inputs: {}, fields: {} };
const block = (opcode: string, next: string | null, n: string) => ({
  opcode, next, parent: 'hat', fields: { VARIABLE: ['score', 'var1'] },
  inputs: { VALUE: [1, [4, n]] },
});
function pack(value: unknown): Uint8Array {
  return zipSync({ 'project.json': strToU8(JSON.stringify(value)) });
}

class NativeLLVM implements PipelineToolClient {
  run(request: RunRequest): Promise<RunResult> {
    const dir = mkdtempSync(join(import.meta.dir, '.xyo-sb3-llvm-'));
    try {
      for (const file of request.inputs) {
        if (file.path.includes('/')) throw Error('unexpected path');
        writeFileSync(join(dir, file.path), file.data);
      }
      const command = request.tool === 'llc' ? 'llc' : 'wasm-ld';
      const proc = Bun.spawnSync([command, ...request.args], { cwd: dir, stdout: 'pipe', stderr: 'pipe' });
      const stdout = proc.stdout.toString(), stderr = proc.stderr.toString();
      if (proc.exitCode !== 0) throw Object.assign(Error(command + ' failed: ' + stderr), { stdout, stderr });
      return Promise.resolve({ files: request.outputs.map(path => ({ path, data: new Uint8Array(readFileSync(join(dir, path))) })), stdout, stderr });
    } finally {
      rmSync(dir, { recursive: true, force: true });
    }
  }
  stop() {}
  restart() {}
}

describe('SB3 to LLVM/Wasm execution', () => {
  test('real llc + wasm-ld compile a real ZIP and execute set/change', async () => {
    const bytes = pack(project({
      hat,
      a: block('data_setvariableto', 'b', '10'),
      b: block('data_changevariableby', 'c', '3'),
      c: block('data_changevariableby', null, '4'),
    }));
    const admitted = admitSb3(bytes);
    expect(admitted.ok).toBe(true);
    if (!admitted.ok) return;
    expect(admitted.compilation.ir).toContain('fadd double');
    const result = await executeSb3(bytes, new LLVMPipeline(new NativeLLVM()));
    expect(result.engine).toBe('llvm-wasm');
    expect(result.variables[0]?.value).toBe(17);
    expect((result.project.targets as any[])[0].variables.var1[1]).toBe(17);
    expect((decodeSb3(bytes).targets as any[])[0].variables.var1[1]).toBe(7);
  });
  test('change uses the existing numeric value', async () => {
    const bytes = pack(project({ hat, a: block('data_changevariableby', null, '6') }, '7'));
    const result = await executeSb3(bytes, new LLVMPipeline(new NativeLLVM()));
    expect(result.variables[0]?.value).toBe(13);
  });
  test('rejects unsupported branches and nonnumeric variables instead of silently executing', () => {
    const unsupported = pack(project({ hat, a: { opcode: 'control_wait', next: null } }));
    expect(admitSb3(unsupported)).toMatchObject({ ok: false });
    const badVar = pack(project({ hat, a: block('data_changevariableby', null, '4') }, 'hello'));
    expect(admitSb3(badVar)).toMatchObject({ ok: false });
    const multi = pack(project({ hat, another: { ...hat, next: 'a' }, a: block('data_setvariableto', null, '4') }));
    expect(admitSb3(multi)).toMatchObject({ ok: false });
    expect(admitSb3(new Uint8Array([1, 2, 3]))).toMatchObject({ ok: false });
  });
});
