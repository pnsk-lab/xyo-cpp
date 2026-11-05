import { createLLVMWorkerClient } from './client';
import { LLVMPipeline } from './pipeline';
import { admitSb3, executeSb3 } from './sb3';
import workerUrl from './worker.ts?worker&url';
import './style.css';

const file = document.querySelector<HTMLInputElement>('#project')!;
const button = document.querySelector<HTMLButtonElement>('#run')!;
const status = document.querySelector<HTMLElement>('#status')!;
const result = document.querySelector<HTMLElement>('#result')!;
const ir = document.querySelector<HTMLElement>('#ir')!;
let bytes: Uint8Array | undefined;
let current = 0;

file.addEventListener('change', async () => {
  const ticket = ++current;
  const uploaded = file.files?.[0];
  bytes = undefined;
  button.disabled = true;
  ir.textContent = '';
  result.textContent = '';
  if (!uploaded) { status.textContent = 'SB3ファイルを選択してください。'; return; }
  if (uploaded.size > 16 * 1024 * 1024) { status.textContent = '16MiBを超えるSB3は使用できません。'; return; }
  try {
    const loaded = new Uint8Array(await uploaded.arrayBuffer());
    if (ticket !== current) return;
    const admitted = admitSb3(loaded);
    if (!admitted.ok) {
      status.textContent = 'LLVM非対応（インタプリタが必要）: ' + admitted.reason;
      return;
    }
    ir.textContent = admitted.compilation.ir;
    bytes = loaded;
    button.disabled = false;
    status.textContent = 'コンパイル準備完了（LLVM対象）';
  } catch (error) { status.textContent = String(error); }
});

button.addEventListener('click', async () => {
  if (!bytes) return;
  const ticket = ++current;
  const source = bytes;
  button.disabled = true;
  status.textContent = 'LLVMコンパイル中…';
  result.textContent = '';
  const client = createLLVMWorkerClient(
    workerUrl,
    new URL('/llvm-assets/llc-browser.mjs', location.href),
    new URL('/llvm-assets/lld-browser.mjs', location.href),
  );
  const compiler = new LLVMPipeline(client);
  try {
    const execution = await executeSb3(source, compiler);
    if (ticket !== current) return;
    result.textContent = JSON.stringify(execution.variables, null, 2);
    status.textContent = 'LLVMで生成したWebAssemblyを実行しました。';
  } catch (error) {
    if (ticket !== current) return;
    status.textContent = '実行失敗: ' + (error instanceof Error ? error.message : String(error));
  } finally {
    compiler.stop();
    if (ticket === current) button.disabled = false;
  }
});
