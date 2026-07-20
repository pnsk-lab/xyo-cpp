import createXyoModule from './xyo-web.js';

const stage = document.querySelector('#stage');
const context = stage.getContext('2d');
const projectFile = document.querySelector('#project-file');
const startButton = document.querySelector('#start');
const stopButton = document.querySelector('#stop');
const status = document.querySelector('#status');
const askForm = document.querySelector('#ask');
const askLabel = document.querySelector('#ask-label');
const askInput = document.querySelector('#ask-input');

const module = await createXyoModule({
  locateFile: (file) => new URL(file, import.meta.url).href
});

const api = {
  loadProject: module.cwrap('sjit_web_load_project_bytes', 'number', ['number', 'number']),
  lastError: module.cwrap('sjit_web_last_error', 'string', []),
  start: module.cwrap('sjit_web_start', null, []),
  stop: module.cwrap('sjit_web_stop', null, []),
  tick: module.cwrap('sjit_web_tick', 'number', ['number', 'number']),
  setMouse: module.cwrap('sjit_web_set_mouse', null, ['number', 'number', 'number', 'number']),
  setKey: module.cwrap('sjit_web_set_key', null, ['number', 'number']),
  blur: module.cwrap('sjit_web_blur', null, []),
  drawCount: module.cwrap('sjit_web_draw_count', 'number', []),
  drawCommand: module.cwrap('sjit_web_draw_command', 'number', ['number', 'number']),
  penCount: module.cwrap('sjit_web_pen_count', 'number', []),
  penCommand: module.cwrap('sjit_web_pen_command', 'number', ['number', 'number']),
  targetCount: module.cwrap('sjit_web_target_count', 'number', []),
  targetId: module.cwrap('sjit_web_target_id', 'number', ['number']),
  targetIsStage: module.cwrap('sjit_web_target_is_stage', 'number', ['number']),
  targetName: module.cwrap('sjit_web_target_name', 'string', ['number']),
  renderTargetId: module.cwrap('sjit_web_render_target_id', 'number', ['number']),
  currentCostume: module.cwrap('sjit_web_current_costume', 'number', ['number']),
  costumeCount: module.cwrap('sjit_web_costume_count', 'number', ['number']),
  costumeFormat: module.cwrap('sjit_web_costume_format', 'string', ['number', 'number']),
  costumeData: module.cwrap('sjit_web_costume_data', 'number', ['number', 'number']),
  costumeDataSize: module.cwrap('sjit_web_costume_data_size', 'number', ['number', 'number']),
  costumeWidth: module.cwrap('sjit_web_costume_width', 'number', ['number', 'number']),
  costumeHeight: module.cwrap('sjit_web_costume_height', 'number', ['number', 'number']),
  costumeCenterX: module.cwrap('sjit_web_costume_rotation_center_x', 'number', ['number', 'number']),
  costumeCenterY: module.cwrap('sjit_web_costume_rotation_center_y', 'number', ['number', 'number']),
  answerPending: module.cwrap('sjit_web_answer_pending', 'number', []),
  question: module.cwrap('sjit_web_question', 'string', []),
  answer: module.cwrap('sjit_web_answer', null, ['string']),
  bubbleText: module.cwrap('sjit_web_bubble_text', 'string', ['number']),
  bubbleThought: module.cwrap('sjit_web_bubble_thought', 'number', ['number'])
};

const drawPtr = module._malloc(16 * Float64Array.BYTES_PER_ELEMENT);
const penPtr = module._malloc(11 * Float64Array.BYTES_PER_ELEMENT);
const assetsById = new Map();
let assetUrls = [];
let running = false;
let animationFrame = 0;
let startTime = 0;
let lastFrameTime = 0;
let pointerDown = false;

function setStatus(message, isError = false) {
  status.textContent = message;
  status.style.color = isError ? '#fca5a5' : '';
}

function worldToCanvasX(x) { return x + 240; }
function worldToCanvasY(y) { return 180 - y; }

function keyIndex(key) {
  const special = {
    ArrowUp: 128, ArrowDown: 129, ArrowRight: 130, ArrowLeft: 131,
    Enter: 132, Backspace: 133, Delete: 134, Shift: 135,
    CapsLock: 136, ScrollLock: 137, Control: 138, Escape: 139,
    Insert: 140, Home: 141, End: 142, PageUp: 143, PageDown: 144,
    ' ': 32
  };
  if (Object.hasOwn(special, key)) return special[key];
  if (key.length !== 1) return -1;
  const upper = key.toUpperCase();
  const code = upper.codePointAt(0);
  return code < 128 ? code : -1;
}

function canvasPoint(event) {
  const rect = stage.getBoundingClientRect();
  const px = (event.clientX - rect.left) * stage.width / rect.width;
  const py = (event.clientY - rect.top) * stage.height / rect.height;
  return {x: px - 240, y: 180 - py};
}

function imageType(format) {
  const normalized = format.toLowerCase();
  if (normalized === 'svg') return 'image/svg+xml';
  if (normalized === 'jpg' || normalized === 'jpeg') return 'image/jpeg';
  if (normalized === 'webp') return 'image/webp';
  return 'image/png';
}

function releaseAssets() {
  for (const url of assetUrls) URL.revokeObjectURL(url);
  assetUrls = [];
  assetsById.clear();
}

async function loadImage(targetIndex, costumeIndex, format, dataPtr, dataSize) {
  if (!dataPtr || dataSize <= 0) return null;
  const bytes = module.HEAPU8.slice(dataPtr, dataPtr + dataSize);
  const url = URL.createObjectURL(new Blob([bytes], {type: imageType(format)}));
  assetUrls.push(url);
  const image = new Image();
  image.src = url;
  try {
    await image.decode();
  } catch {
    return null;
  }
  return image;
}

async function collectAssets() {
  releaseAssets();
  const count = api.targetCount();
  const targets = [];
  for (let targetIndex = 0; targetIndex < count; ++targetIndex) {
    const target = {
      index: targetIndex,
      id: api.targetId(targetIndex),
      isStage: api.targetIsStage(targetIndex) !== 0,
      name: api.targetName(targetIndex),
      currentCostume: api.currentCostume(targetIndex),
      costumes: []
    };
    const costumeCount = api.costumeCount(targetIndex);
    for (let costumeIndex = 0; costumeIndex < costumeCount; ++costumeIndex) {
      const format = api.costumeFormat(targetIndex, costumeIndex);
      const costume = {
        width: api.costumeWidth(targetIndex, costumeIndex),
        height: api.costumeHeight(targetIndex, costumeIndex),
        centerX: api.costumeCenterX(targetIndex, costumeIndex),
        centerY: api.costumeCenterY(targetIndex, costumeIndex),
        image: await loadImage(
          targetIndex,
          costumeIndex,
          format,
          api.costumeData(targetIndex, costumeIndex),
          api.costumeDataSize(targetIndex, costumeIndex))
      };
      target.costumes.push(costume);
    }
    assetsById.set(target.id, target);
    targets.push(target);
  }
  return targets;
}

function drawFallbackSprite(command, costume) {
  const scale = Math.max(0, command[8]) / 100;
  const width = Math.max(18, (costume?.width || 36) * scale);
  const height = Math.max(18, (costume?.height || 36) * scale);
  const x = worldToCanvasX(command[3]);
  const y = worldToCanvasY(command[4]);
  context.save();
  context.translate(x, y);
  context.rotate((command[7] - 90) * Math.PI / 180);
  context.fillStyle = '#f79a2b';
  context.strokeStyle = '#5a3720';
  context.lineWidth = 2;
  context.fillRect(-width / 2, -height / 2, width, height);
  context.strokeRect(-width / 2, -height / 2, width, height);
  context.restore();
}

function drawSprite(command, target) {
  const costume = target?.costumes[command[2]];
  if (!target || !costume || command[14] === 0 || command[8] <= 0) return;
  const scale = Math.max(0, command[8]) / 100;
  const x = worldToCanvasX(command[3]);
  const y = worldToCanvasY(command[4]);
  context.save();
  context.translate(x, y);
  context.rotate((command[7] - 90) * Math.PI / 180);
  if (costume.image && costume.width > 0 && costume.height > 0) {
    context.drawImage(
      costume.image,
      -costume.centerX * scale,
      -costume.centerY * scale,
      costume.width * scale,
      costume.height * scale);
  } else {
    context.restore();
    drawFallbackSprite(command, costume);
    return;
  }
  context.restore();
}

function drawPen() {
  const count = api.penCount();
  for (let i = 0; i < count; ++i) {
    if (!api.penCommand(i, penPtr)) continue;
    const command = module.HEAPF64.subarray(penPtr / 8, penPtr / 8 + 11);
    context.beginPath();
    context.moveTo(worldToCanvasX(command[2]), worldToCanvasY(command[3]));
    context.lineTo(worldToCanvasX(command[4]), worldToCanvasY(command[5]));
    context.lineWidth = Math.max(.25, command[6]);
    context.lineCap = 'round';
    context.strokeStyle = `rgba(${command[7]}, ${command[8]}, ${command[9]}, ${command[10] / 255})`;
    context.stroke();
  }
}

function drawBubbles(positions) {
  const count = api.targetCount();
  context.font = '12px system-ui, sans-serif';
  for (let index = 0; index < count; ++index) {
    const text = api.bubbleText(index);
    if (!text) continue;
    const targetId = api.targetId(index);
    const position = positions.get(targetId);
    if (!position) continue;
    const label = text.length > 42 ? `${text.slice(0, 40)}…` : text;
    const width = Math.min(220, Math.max(60, context.measureText(label).width + 20));
    const left = Math.max(4, Math.min(stage.width - width - 4, worldToCanvasX(position.x) - width / 2));
    const top = Math.max(4, worldToCanvasY(position.y + 36) - 28);
    context.fillStyle = '#fff';
    context.strokeStyle = '#263238';
    context.lineWidth = 1;
    context.beginPath();
    context.roundRect(left, top, width, 28, 8);
    context.fill();
    context.stroke();
    context.fillStyle = '#263238';
    context.fillText(label, left + 10, top + 18);
    context.beginPath();
    context.moveTo(worldToCanvasX(position.x) - 6, top + 28);
    context.lineTo(worldToCanvasX(position.x), top + 36);
    context.lineTo(worldToCanvasX(position.x) + 6, top + 28);
    context.fill();
    if (api.bubbleThought(index)) {
      context.beginPath();
      context.arc(worldToCanvasX(position.x) - 12, top + 38, 3, 0, Math.PI * 2);
      context.fill();
    }
  }
}

function drawFrame() {
  context.fillStyle = '#fff';
  context.fillRect(0, 0, stage.width, stage.height);
  const targets = [...assetsById.values()];
  const stageTarget = targets.find((target) => target.isStage);
  const stageCostume = stageTarget?.costumes[api.currentCostume(stageTarget.index)];
  if (stageCostume?.image && stageCostume.width > 0 && stageCostume.height > 0) {
    context.drawImage(
      stageCostume.image,
      240 - stageCostume.centerX,
      180 - stageCostume.centerY,
      stageCostume.width,
      stageCostume.height);
  }

  drawPen();
  const positions = new Map();
  const count = api.drawCount();
  for (let index = 0; index < count; ++index) {
    if (!api.drawCommand(index, drawPtr)) continue;
    const command = module.HEAPF64.subarray(drawPtr / 8, drawPtr / 8 + 16);
    const kind = command[0];
    if (kind !== 1) continue;
    const targetId = command[1];
    positions.set(targetId, {x: command[3], y: command[4]});
    const renderId = api.renderTargetId(targetId);
    drawSprite(command, assetsById.get(renderId));
  }
  drawBubbles(positions);
}

function updateAskBox() {
  if (!api.answerPending()) {
    askForm.hidden = true;
    return;
  }
  askLabel.textContent = api.question();
  askForm.hidden = false;
  if (document.activeElement !== askInput) askInput.focus();
}

function animate(now) {
  if (!running) return;
  if (!startTime) startTime = now;
  const delta = Math.min(100, Math.max(0, lastFrameTime ? now - lastFrameTime : 0));
  lastFrameTime = now;
  api.tick(now - startTime, delta);
  drawFrame();
  updateAskBox();
  animationFrame = requestAnimationFrame(animate);
}

function startAnimation() {
  cancelAnimationFrame(animationFrame);
  running = true;
  startTime = 0;
  lastFrameTime = 0;
  api.start();
  animationFrame = requestAnimationFrame(animate);
}

function stopAnimation() {
  running = false;
  cancelAnimationFrame(animationFrame);
  api.stop();
  drawFrame();
}

async function loadProject(file) {
  stopAnimation();
  releaseAssets();
  setStatus(`${file.name} を読み込み中…`);
  const bytes = new Uint8Array(await file.arrayBuffer());
  const pointer = module._malloc(bytes.length);
  module.HEAPU8.set(bytes, pointer);
  const loaded = api.loadProject(pointer, bytes.length);
  module._free(pointer);
  if (!loaded) {
    startButton.disabled = true;
    stopButton.disabled = true;
    setStatus(api.lastError() || 'プロジェクトを読み込めませんでした', true);
    return;
  }
  await collectAssets();
  startButton.disabled = false;
  stopButton.disabled = false;
  setStatus(`${file.name} — ${api.targetCount()} targets`);
  startAnimation();
}

projectFile.addEventListener('change', () => {
  const [file] = projectFile.files;
  if (file) loadProject(file).catch((error) => setStatus(error.message, true));
});

startButton.addEventListener('click', startAnimation);
stopButton.addEventListener('click', stopAnimation);

stage.addEventListener('pointerdown', (event) => {
  if (event.button !== 0) return;
  stage.focus();
  pointerDown = true;
  stage.setPointerCapture(event.pointerId);
  const point = canvasPoint(event);
  api.setMouse(point.x, point.y, 1, 1);
});

stage.addEventListener('pointermove', (event) => {
  if (!pointerDown) return;
  const point = canvasPoint(event);
  api.setMouse(point.x, point.y, 1, 0);
});

stage.addEventListener('pointerup', (event) => {
  if (event.button !== 0) return;
  pointerDown = false;
  const point = canvasPoint(event);
  api.setMouse(point.x, point.y, 0, 0);
});

window.addEventListener('keydown', (event) => {
  if (event.target instanceof HTMLInputElement) return;
  const key = keyIndex(event.key);
  if (key < 0) return;
  if (key >= 32 && key <= 126) event.preventDefault();
  api.setKey(key, 1);
});

window.addEventListener('keyup', (event) => {
  const key = keyIndex(event.key);
  if (key >= 0) api.setKey(key, 0);
});

window.addEventListener('blur', () => api.blur());

askForm.addEventListener('submit', (event) => {
  event.preventDefault();
  api.answer(askInput.value);
  askInput.value = '';
});

setStatus('WASM 準備完了。SB3 を選択してください。');
