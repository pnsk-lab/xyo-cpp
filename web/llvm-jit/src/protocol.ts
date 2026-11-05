// The worker loads trusted Emscripten MODULARIZE ES-module factories, not arbitrary user code.
export type Tool = 'llc' | 'wasm-ld';
export interface InputFile { path: string; data: Uint8Array }
export interface RunRequest {
  tool: Tool;
  args: string[];
  inputs: InputFile[];
  outputs: string[];
  /** Validate these requested outputs as complete executable Wasm modules (not .o files). */
  validateWasm?: string[];
}
export interface RunResult { files: InputFile[]; stdout: string; stderr: string }
export interface WorkerRequest {
  id: number;
  request: RunRequest;
  /** Trusted same-origin factory URL configuration for Vite's immutable Worker URL. */
  toolUrls?: { llc: string; 'wasm-ld': string };
}
export type WorkerResponse =
  | { id: number; ok: true; result: RunResult }
  | { id: number; ok: false; error: { message: string; stdout: string; stderr: string } };

/** Strict relative paths inside an initially empty Emscripten working directory. */
export function assertPath(path: string): void {
  if (typeof path !== 'string' || !path || path.startsWith('/') || path.includes('\\') || path.includes('\0') || path.split('/').some(p => !p || p === '.' || p === '..')) {
    throw new Error(`Invalid relative file path: ${String(path)}`);
  }
}
