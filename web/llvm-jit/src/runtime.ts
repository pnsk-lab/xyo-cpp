import { assertPath, type RunRequest, type RunResult, type Tool, type WorkerRequest, type WorkerResponse } from './protocol';

/** Contract required of each independently instantiated Emscripten tool. */
export interface ToolModule {
  FS: { mkdir(path: string): void; writeFile(path: string, data: Uint8Array): void; readFile(path: string): Uint8Array };
  callMain(args: string[]): number | void;
}
export type ToolFactory = (options: { noInitialRun: true; print(line: string): void; printErr(line: string): void }) => Promise<ToolModule> | ToolModule;
export type ToolFactories = Record<Tool, ToolFactory>;

export class ToolRunError extends Error {
  constructor(message: string, public stdout = '', public stderr = '') { super(message); this.name = 'ToolRunError'; }
}

export async function runTool(factories: ToolFactories, request: RunRequest): Promise<RunResult> {
  const out: string[] = [], err: string[] = [];
  try {
    if (!request || (request.tool !== 'llc' && request.tool !== 'wasm-ld') || !Array.isArray(request.args) || !request.args.every(x => typeof x === 'string') || !Array.isArray(request.inputs) || !Array.isArray(request.outputs)) throw new Error('Invalid tool request');
    const inputPaths = new Set<string>();
    for (const file of request.inputs) {
      assertPath(file.path);
      if (!(file.data instanceof Uint8Array) || inputPaths.has(file.path)) throw new Error(`Invalid or duplicate input: ${file.path}`);
      inputPaths.add(file.path);
    }
    const outputs = new Set<string>();
    for (const path of request.outputs) {
      assertPath(path);
      if (outputs.has(path) || inputPaths.has(path)) throw new Error(`Duplicate or overlapping output: ${path}`);
      outputs.add(path);
    }
    if (request.validateWasm !== undefined && !Array.isArray(request.validateWasm)) throw new Error('Invalid validation list');
    for (const path of request.validateWasm ?? []) if (!outputs.has(path)) throw new Error(`Validation target is not an output: ${path}`);
    const module = await factories[request.tool]({ noInitialRun: true, print: s => out.push(String(s)), printErr: s => err.push(String(s)) });
    const directories = new Set<string>();
    for (const path of [...inputPaths, ...outputs]) {
      const parts = path.split('/'); parts.pop();
      let dir = '';
      for (const part of parts) {
        dir = dir ? `${dir}/${part}` : part;
        if (!directories.has(dir)) { module.FS.mkdir(dir); directories.add(dir); }
      }
    }
    for (const file of request.inputs) module.FS.writeFile(file.path, file.data);
    let status: number | void;
    try { status = module.callMain(request.args); }
    catch (error) {
      // Emscripten may throw an ExitStatus for a normal exit.
      if (typeof error === 'object' && error !== null && 'status' in error && typeof error.status === 'number') status = error.status;
      else throw error;
    }
    if (status !== undefined && status !== 0) throw new Error(`${request.tool} exited with status ${status}`);
    const files = request.outputs.map(path => ({ path, data: new Uint8Array(module.FS.readFile(path)) }));
    for (const path of request.validateWasm ?? []) {
      const bytes = files.find(file => file.path === path)!.data;
      if (!WebAssembly.validate(bytes)) throw new Error(`Invalid WebAssembly output: ${path}`);
    }
    return { files, stdout: out.join('\n'), stderr: err.join('\n') };
  } catch (error) {
    throw new ToolRunError(error instanceof Error ? error.message : String(error), out.join('\n'), err.join('\n'));
  }
}

/** Serializes invocations; a fresh module and virtual FS is used for every request. */
export function createWorkerHandler(factories: ToolFactories, post: (response: WorkerResponse) => void): (message: WorkerRequest) => void {
  let queue = Promise.resolve();
  return message => {
    queue = queue.then(async () => {
      try { post({ id: message.id, ok: true, result: await runTool(factories, message.request) }); }
      catch (error) {
        const e = error instanceof ToolRunError ? error : new ToolRunError(String(error));
        post({ id: message.id, ok: false, error: { message: e.message, stdout: e.stdout, stderr: e.stderr } });
      }
    }).catch(() => { /* Keep the queue usable if the transport throws. */ });
  };
}
