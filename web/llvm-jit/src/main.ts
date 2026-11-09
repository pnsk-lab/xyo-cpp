import { admitSb3 } from './sb3';
import workerUrl from './unified-worker.ts?worker&url';
import './style.css';

const file = document.querySelector<HTMLInputElement>('#project')!;
const button = document.querySelector<HTMLButtonElement>('#run')!;
const status = document.querySelector<HTMLElement>('#status')!;
const result = document.querySelector<HTMLElement>('#result')!;
const ir = document.querySelector<HTMLElement>('#ir')!;
let bytes: Uint8Array | undefined;
let generation = 0;

file.addEventListener('change', async () => {
  const id = ++generation;
  const uploaded = file.files?.[0];
  bytes = undefined;
  button.disabled = true;
  ir.textContent = '';
  result.textContent = '';
  if (!uploaded) { status.textContent = 'SB3ファイルを選択してください。'; return; }
  if (uploaded.size > 16 * 1024 * 1024) { status.textContent = '16MiBを超えるSB3は使用できません。'; return; }
  try {
    const source = new Uint8Array(await uploaded.arrayBuffer());
    if (id !== generation) return;
    // Admission here is only a hint. The Worker always validates again.
    const admitted = admitSb3(source);
    status.textContent = admitted.ok
      ? '統合LLVM候補です。生成コードとインタプリタが同じxyoランタイムを使用します。'
      : 'LLVM非対応: xyoインタプリタで実行します。';
    ir.textContent = admitted.ok ? admitted.compilation.ir : '(インタプリタへのフォールバック)';
    bytes = source;
    button.disabled = false;
  } catch (error) { status.textContent = String(error); }
});

button.addEventListener('click', () => {
  if (!bytes) return;
  const id = ++generation;
  const source = bytes;
  const worker = new Worker(workerUrl, { type: 'module' });
  button.disabled = true;
  status.textContent = 'SB3をコンパイル／実行中…';
  result.textContent = '';
  const timeout = setTimeout(() => {
    worker.terminate();
    if (id !== generation) return;
    status.textContent = '実行タイムアウト。Workerを停止しました。';
    button.disabled = false;
  }, 120_000);
  const finish = () => { clearTimeout(timeout); worker.terminate(); if (id === generation) button.disabled = false; };
  worker.onmessage = (event: MessageEvent<{ id: number; ok: boolean; mode?: string; backend?: string; diagnostic?: string; snapshot?: unknown; error?: string }>) => {
    if (event.data.id !== id) return;
    finish();
    if (!event.data.ok) {
      status.textContent = '実行失敗: ' + (event.data.error ?? 'unknown error');
      return;
    }
    result.textContent = JSON.stringify(event.data.snapshot, null, 2);
    status.textContent = event.data.mode === 'llvm'
      ? 'xyo-cpp統合ランタイム内でLLVM生成コードを実行しました（' + (event.data.backend === 'cpp-jit-emit' ? 'C++ jitEmitScript' : '限定TypeScript生成器') + '）。'
      : 'LLVM非対応のため、xyo-cppインタプリタで実行しました。' + (event.data.diagnostic ? ' 原因: ' + event.data.diagnostic : '');
  };
  worker.onerror = event => {
    finish();
    if (id === generation) status.textContent = 'Workerエラー: ' + event.message;
  };
  worker.postMessage({ id, bytes: source, steps: 20, origin: location.origin });
});

import './player-ui';
