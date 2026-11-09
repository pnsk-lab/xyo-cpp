import { unzipSync } from 'fflate';
import workerUrl from './player-worker.ts?worker&url';

type Costume = { md5ext?: string; assetId?: string; dataFormat?: string; rotationCenterX?: number; rotationCenterY?: number; bitmapResolution?: number };
type Metadata = { name: string; isStage: boolean; costumes: Costume[]; variables?: Record<string, [string, unknown]> };
type Sprite = {
  name: string; isStage: boolean; drawableId: number; layerOrder: number;
  x: number; y: number; direction: number; size: number; visible: boolean;
  currentCostume: number; rotationStyle: string; bubbleText: string; variables?: Record<string,string>;
};
type Stroke = { type: 'clear' | 'line' | 'stamp'; rgba?: number[]; size?: number;
  x0?: number; y0?: number; x1?: number; y1?: number; drawableId?: number };
type Snapshot = { targets: Sprite[]; pen: Stroke[]; question?: string | null };
type AudioEvent = { kind: number; id: number; format: string; bytes: Uint8Array;
  volume: number; pitch: number; pan: number };
type Response = { id: number; ok: boolean; type: string; mode?: string; backend?: string;
  diagnostic?: string; error?: string; snapshot?: Snapshot; audio?: AudioEvent[] };

const canvas = document.querySelector<HTMLCanvasElement>('#stage')!;
const file = document.querySelector<HTMLInputElement>('#project')!;
const play = document.querySelector<HTMLButtonElement>('#play')!;
const stop = document.querySelector<HTMLButtonElement>('#stop')!;
const status = document.querySelector<HTMLElement>('#player-status')!;
const frame = document.querySelector<HTMLElement>('#player-frame')!;
const askForm = document.querySelector<HTMLFormElement>('#ask-form')!;
const askQuestion = document.querySelector<HTMLElement>('#ask-question')!;
const askAnswer = document.querySelector<HTMLInputElement>('#ask-answer')!;
const variablePanel = document.querySelector<HTMLElement>('#player-vars')!;
const ctx = canvas.getContext('2d')!;
const penCanvas = document.createElement('canvas');
penCanvas.width = 480; penCanvas.height = 360;
const pen = penCanvas.getContext('2d')!;
let worker = new Worker(workerUrl, { type: 'module' });
let audioContext: AudioContext | undefined;
let audioGeneration = 0;
const audioVoices = new Map<number, AudioBufferSourceNode>();
function stopAudio() {
  ++audioGeneration;
  for (const voice of audioVoices.values()) {
    try { voice.stop(); } catch { /* already stopped */ }
  }
  audioVoices.clear();
}
function acknowledge(id: number) {
  void send({ type: 'ackAudio', soundId: id }).catch(() => {});
}
async function playSound(event: AudioEvent, generation: number) {
  try {
    if (!audioContext || !event.bytes.byteLength) { acknowledge(event.id); return; }
    const bytes = Uint8Array.from(event.bytes);
    const buffer = await audioContext.decodeAudioData(bytes.buffer);
    if (generation !== audioGeneration) { acknowledge(event.id); return; }
    const source = audioContext.createBufferSource();
    source.buffer = buffer;
    source.playbackRate.value = Math.pow(2, Math.max(-360, Math.min(360, event.pitch)) / 120);
    const gain = audioContext.createGain();
    gain.gain.value = Math.max(0, Math.min(100, event.volume)) / 100;
    const panner = audioContext.createStereoPanner();
    panner.pan.value = Math.max(-1, Math.min(1, event.pan / 100));
    source.connect(gain).connect(panner).connect(audioContext.destination);
    source.onended = () => {
      audioVoices.delete(event.id);
      acknowledge(event.id);
    };
    audioVoices.set(event.id, source);
    source.start();
  } catch {
    acknowledge(event.id);
  }
}
function processAudio(events: AudioEvent[] = []) {
  for (const event of events) {
    if (event.kind === 2) stopAudio();
    else if (event.kind === 1) void playSound(event, audioGeneration);
  }
}
let counter = 0;
let running = false;
let pendingStep = false;
let ticks = 0;
let metadata: Metadata[] = [];
const assets = new Map<string, ImageBitmap | HTMLImageElement>();
let imageUrls: string[] = [];
const pending = new Map<number, { resolve: (r: Response) => void; reject: (e: Error) => void; timeout: number }>();
const warn = (message: string) => { status.textContent = message; };

function attach() {
  worker.onmessage = (event: MessageEvent<Response>) => {
    const entry = pending.get(event.data.id);
    if (!entry) return;
    pending.delete(event.data.id);
    clearTimeout(entry.timeout);
    if (event.data.ok) { processAudio(event.data.audio); entry.resolve(event.data); }
    else entry.reject(Error(event.data.error ?? 'runtime error'));
  };
}
attach();
function reset() {
  stopAudio();
  worker.terminate();
  for (const p of pending.values()) { clearTimeout(p.timeout); p.reject(Error('worker restarted')); }
  pending.clear();
  worker = new Worker(workerUrl, { type: 'module' });
  attach();
  running = false;
}
function send(request: Record<string, unknown>, ms = 15000): Promise<Response> {
  return new Promise((resolve, reject) => {
    const id = ++counter;
    const timeout = window.setTimeout(() => {
      reset();
      reject(Error('worker timeout'));
    }, ms);
    pending.set(id, { resolve, reject, timeout });
    worker.postMessage({ ...request, id });
  });
}
function drawSprite(context: CanvasRenderingContext2D, sprite: Sprite) {
  const costume = metadata.find(t => t.name === sprite.name && t.isStage === sprite.isStage)?.costumes[sprite.currentCostume];
  if (!costume || !sprite.visible) return;
  const filename = costume.md5ext ?? (costume.assetId + '.' + costume.dataFormat);
  const bitmap = assets.get(filename);
  if (!bitmap || ![sprite.x, sprite.y, sprite.size, sprite.direction].every(Number.isFinite)) return;
  context.save();
  context.translate(240 + sprite.x, 180 - sprite.y);
  if (sprite.rotationStyle === 'all around' && !sprite.isStage)
    context.rotate((90 - sprite.direction) * Math.PI / 180);
  if (sprite.rotationStyle === 'left-right' && sprite.direction < 0 && !sprite.isStage)
    context.scale(-1, 1);
  const res = costume.bitmapResolution || 1;
  const scale = sprite.size / 100 / res;
  context.scale(scale, scale);
  const w = bitmap.width, h = bitmap.height;
  context.drawImage(bitmap, -(costume.rotationCenterX ?? w / 2), -(costume.rotationCenterY ?? h / 2));
  context.restore();
}
function render(snapshot: Snapshot) {
  const wasHidden = askForm.hidden;
  askForm.hidden = snapshot.question === undefined || snapshot.question === null;
  if (!askForm.hidden) {
    askQuestion.textContent = snapshot.question ?? '';
    if (wasHidden) { askAnswer.value = ''; askAnswer.focus(); }
  }
  for (const event of snapshot.pen ?? []) {
    if (event.type === 'clear') pen.clearRect(0, 0, 480, 360);
    else if (event.type === 'stamp') {
      const sprite = snapshot.targets.find(t => t.drawableId === event.drawableId);
      if (sprite) drawSprite(pen, sprite);
    } else if (event.type === 'line') {
      const rgba = event.rgba ?? [0, 0, 0, 1];
      const xy = [event.x0, event.y0, event.x1, event.y1, event.size];
      if (![...rgba, ...xy].every(x => typeof x === 'number' && Number.isFinite(x))) continue;
      const color = rgba.map(x => Math.max(0, Math.min(1, x)));
      pen.strokeStyle = `rgba(${Math.round(color[0] * 255)},${Math.round(color[1] * 255)},${Math.round(color[2] * 255)},${color[3]})`;
      pen.fillStyle = pen.strokeStyle;
      pen.lineWidth = Math.max(0.1, event.size ?? 1);
      pen.lineCap = 'round';
      pen.beginPath();
      pen.moveTo(240 + event.x0!, 180 - event.y0!);
      pen.lineTo(240 + event.x1!, 180 - event.y1!);
      pen.stroke();
      if (event.x0 === event.x1 && event.y0 === event.y1) {
        pen.beginPath();
        pen.arc(240 + event.x0!, 180 - event.y0!, Math.max(0.05, (event.size ?? 1) / 2), 0, Math.PI * 2);
        pen.fill();
      }
    }
  }
  ctx.fillStyle = '#fff'; ctx.fillRect(0, 0, 480, 360);
  const ordered = [...snapshot.targets].sort((a, b) => a.layerOrder - b.layerOrder || a.drawableId - b.drawableId);
  for (const sprite of ordered) if (sprite.isStage) drawSprite(ctx, sprite);
  ctx.drawImage(penCanvas, 0, 0);
  for (const sprite of ordered) if (!sprite.isStage) drawSprite(ctx, sprite);
  for (const sprite of ordered) {
    if (!sprite.visible || !sprite.bubbleText) continue;
    const text = sprite.bubbleText.slice(0, 90);
    ctx.font = '13px system-ui';
    const width = Math.min(260, ctx.measureText(text).width + 24);
    const x = Math.max(0, Math.min(480 - width, 260 + sprite.x));
    const y = Math.max(22, Math.min(342, 130 - sprite.y));
    ctx.fillStyle = '#fff'; ctx.strokeStyle = '#b6b6b6'; ctx.lineWidth = 1;
    ctx.beginPath(); ctx.roundRect(x, y - 18, width, 27, 10); ctx.fill(); ctx.stroke();
    ctx.fillStyle = '#222'; ctx.fillText(text, x + 10, y, width - 16);
  }
  const variables: string[] = [];
  for (const target of snapshot.targets) {
    if (!target.variables || !target.isStage && !target.name) continue;
    const names = metadata.find(t => t.name === target.name)?.variables ?? {};
    for (const [id, value] of Object.entries(target.variables))
      variables.push(`${target.name || 'Stage'} / ${names[id]?.[0] ?? id}: ${value}`);
  }
  variablePanel.textContent = variables.slice(0, 50).join('  |  ');
  frame.textContent = `フレーム ${ticks} / ターゲット ${ordered.length}`;
}

async function readProject(upload: File): Promise<Uint8Array> {
  if (upload.size > 16 * 1024 * 1024) throw Error('SB3 must be under 16 MiB');
  const bytes = new Uint8Array(await upload.arrayBuffer());
  let total = 0;
  const zip = unzipSync(bytes, { filter: entry => {
    total += entry.originalSize;
    if (entry.originalSize > 16 * 1024 * 1024 || total > 64 * 1024 * 1024) throw Error('ZIP exceeds size limit');
    return true;
  } });
  if (!zip['project.json']) throw Error('project.json missing');
  const project = JSON.parse(new TextDecoder().decode(zip['project.json'])) as { targets: Metadata[] };
  if (!Array.isArray(project.targets)) throw Error('invalid target list');
  metadata = project.targets;
  for (const entry of assets.values()) if (entry instanceof ImageBitmap) entry.close();
  for (const url of imageUrls) URL.revokeObjectURL(url);
  imageUrls = []; assets.clear();
  await Promise.all(metadata.flatMap(t => (t.costumes ?? []).map(async c => {
    const filename = c.md5ext ?? (c.assetId + '.' + c.dataFormat);
    const data = zip[filename];
    const ext = filename?.split('.').pop()?.toLowerCase();
    if (!data || !['png', 'jpg', 'jpeg', 'svg'].includes(ext ?? '')) return;
    const mime = ext === 'svg' ? 'image/svg+xml' : ext === 'png' ? 'image/png' : 'image/jpeg';
    try {
      const image = await createImageBitmap(new Blob([Uint8Array.from(data)], { type: mime }));
      assets.set(filename, image);
    } catch {
      // SVG is not consistently supported by createImageBitmap in browsers.
      const url = URL.createObjectURL(new Blob([Uint8Array.from(data)], { type: mime }));
      imageUrls.push(url);
      const image = new Image();
      image.src = url;
      await image.decode().catch(() => {});
      if (image.naturalWidth) assets.set(filename, image);
    }
  })));
  pen.clearRect(0, 0, 480, 360);
  ticks = 0;
  return bytes;
}
play.addEventListener('click', async () => {
  if (!file.files?.[0]) { warn('SB3を選択してください'); return; }
  play.disabled = true;
  // Must unlock AudioContext synchronously in response to a user gesture.
  audioContext ??= new AudioContext();
  void audioContext.resume();
  reset();
  warn('xyo-cppのプロジェクトを読み込み中…');
  try {
    const bytes = await readProject(file.files[0]);
    const loaded = await send({ type: 'load', bytes, origin: location.origin }, 120000);
    if (loaded.snapshot) render(loaded.snapshot);
    const started = await send({ type: 'greenFlag' });
    if (started.snapshot) render(started.snapshot);
    running = true;
    stop.disabled = false;
    warn(loaded.mode === 'llvm'
      ? `LLVM JIT: ${loaded.backend}`
      : 'インタプリタで実行中' + (loaded.diagnostic ? `（${loaded.diagnostic}）` : ''));
    canvas.focus();
  } catch (error) { running = false; warn('エラー: ' + String(error)); }
  finally { play.disabled = false; }
});
stop.addEventListener('click', async () => {
  running = false; stop.disabled = true;
  stopAudio();
  try {
    const r = await send({ type: 'stop' });
    if (r.snapshot) render(r.snapshot);
    warn('停止');
  } catch (error) { warn(String(error)); }
});
file.addEventListener('change', () => { running = false; stop.disabled = true; });
window.setInterval(async () => {
  if (!running || pendingStep) return;
  pendingStep = true;
  try {
    const r = await send({ type: 'step' });
    ticks++;
    if (r.snapshot) render(r.snapshot);
  } catch (error) { running = false; warn('実行エラー: ' + String(error)); }
  finally { pendingStep = false; }
}, 1000 / 30);

function input(message: Record<string, unknown>) {
  if (running) void send(message).catch(error => {
    running = false; warn('入力エラー: ' + String(error));
  });
}
function point(event: PointerEvent) {
  const bounds = canvas.getBoundingClientRect();
  return { x: (event.clientX - bounds.left) / bounds.width * 480,
    y: (event.clientY - bounds.top) / bounds.height * 360 };
}
canvas.addEventListener('pointerdown', event => {
  canvas.setPointerCapture(event.pointerId);
  input({ type: 'mouse', ...point(event), down: true, button: event.button, hasDown: true });
});
canvas.addEventListener('pointerup', event => {
  input({ type: 'mouse', ...point(event), down: false, button: event.button, hasDown: true });
});
canvas.addEventListener('pointermove', event => {
  input({ type: 'mouse', ...point(event), down: (event.buttons & 1) !== 0, button: 0, hasDown: false });
});
canvas.addEventListener('wheel', event => {
  event.preventDefault();
  input({ type: 'wheel', dx: event.deltaX, dy: event.deltaY });
}, { passive: false });
canvas.addEventListener('keydown', event => {
  if (event.key === ' ' || event.key.startsWith('Arrow')) event.preventDefault();
  input({ type: 'key', key: event.key, down: true });
});
canvas.addEventListener('keyup', event => input({ type: 'key', key: event.key, down: false }));
canvas.addEventListener('blur', () => {
  for (const key of ['ArrowLeft','ArrowRight','ArrowUp','ArrowDown',' ']) input({ type: 'key', key, down: false });
});

askForm.addEventListener('submit', event => {
  event.preventDefault();
  if (!running) return;
  const text = askAnswer.value;
  void send({ type: 'answer', text }).then(() => {
    askForm.hidden = true;
    canvas.focus();
  }).catch(error => warn('回答送信エラー: ' + String(error)));
});
