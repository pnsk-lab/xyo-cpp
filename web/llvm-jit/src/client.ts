import type { RunRequest, RunResult, WorkerResponse } from './protocol';

export interface WorkerLike {
  postMessage(message: unknown): void;
  terminate(): void;
  addEventListener(type: 'message' | 'error' | 'messageerror', listener: (event: any) => void): void;
}
export class WorkerToolError extends Error {
  constructor(message: string, public stdout = '', public stderr = '') { super(message); this.name = 'WorkerToolError'; }
}
/** stop() aborts all in-flight work by terminating the worker; restart() creates a new one. */
export class LLVMWorkerClient {
  private worker: WorkerLike | undefined;
  private nextId = 1;
  private pending = new Map<number, { resolve: (result: RunResult) => void; reject: (error: Error) => void }>();
  constructor(private readonly createWorker: () => WorkerLike) { this.restart(); }
  run(request: RunRequest): Promise<RunResult> {
    const worker = this.worker;
    if (!worker) return Promise.reject(new WorkerToolError('Worker is stopped'));
    const id = this.nextId++;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      try { worker.postMessage({ id, request }); }
      catch (error) { this.pending.delete(id); reject(error); }
    });
  }
  stop(): void { this.shutdown('Worker stopped'); }
  terminate(): void { this.stop(); }
  restart(): void {
    this.shutdown('Worker restarted');
    const worker = this.createWorker();
    this.worker = worker;
    worker.addEventListener('message', event => {
      if (this.worker !== worker) return;
      const data = event.data as WorkerResponse;
      if (!data || typeof data.id !== 'number') return;
      const pending = this.pending.get(data.id);
      if (!pending) return;
      this.pending.delete(data.id);
      if (data.ok === true) pending.resolve(data.result);
      else if (data.ok === false && data.error) pending.reject(new WorkerToolError(data.error.message, data.error.stdout, data.error.stderr));
      else pending.reject(new WorkerToolError('Malformed worker response'));
    });
    const fail = () => { if (this.worker === worker) this.shutdown('Worker failed'); };
    worker.addEventListener('error', fail);
    worker.addEventListener('messageerror', fail);
  }
  private shutdown(message: string): void {
    const worker = this.worker;
    this.worker = undefined;
    worker?.terminate();
    for (const pending of this.pending.values()) pending.reject(new WorkerToolError(message));
    this.pending.clear();
  }
}

/** Explicit opt-in: constructing this client creates a Worker, not a compiler module. */
export function createLLVMWorkerClient(workerUrl: string | URL, llcUrl: string | URL, lldUrl: string | URL): LLVMWorkerClient {
  const url = new URL(workerUrl, globalThis.location?.href);
  url.searchParams.set('llc', new URL(llcUrl, globalThis.location?.href).href);
  url.searchParams.set('wasm-ld', new URL(lldUrl, globalThis.location?.href).href);
  return new LLVMWorkerClient(() => new Worker(url, { type: 'module' }));
}
