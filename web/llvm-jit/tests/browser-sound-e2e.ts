import { chromium } from 'playwright-core';
import { strict as assert } from 'node:assert';
import { zipSync, strToU8 } from 'fflate';

function tone(): Uint8Array {
  const sampleRate = 8000, samples = 1600;
  const bytes = new Uint8Array(44 + samples * 2);
  const view = new DataView(bytes.buffer);
  const text = (offset: number, value: string) => bytes.set(new TextEncoder().encode(value), offset);
  text(0, 'RIFF'); view.setUint32(4, bytes.length - 8, true); text(8, 'WAVE');
  text(12, 'fmt '); view.setUint32(16, 16, true); view.setUint16(20, 1, true);
  view.setUint16(22, 1, true); view.setUint32(24, sampleRate, true);
  view.setUint32(28, sampleRate * 2, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true);
  text(36, 'data'); view.setUint32(40, samples * 2, true);
  for (let i = 0; i < samples; i++) {
    const val = Math.round(Math.sin(2 * Math.PI * i * 440 / sampleRate) * 3000);
    view.setInt16(44 + i * 2, val, true);
  }
  return bytes;
}
const sb3 = zipSync({
  'project.json': strToU8(JSON.stringify({
    targets: [{
      isStage: true, name: 'Stage', variables: { done: ['done', 0] },
      lists: {}, broadcasts: {}, blocks: {
        hat: { opcode: 'event_whenflagclicked', topLevel: true, parent: null, next: 'snd', x: 0, y: 0, fields: {}, inputs: {} },
        snd: { opcode: 'sound_playuntildone', topLevel: false, parent: 'hat', next: 'set', fields: {},
          inputs: { SOUND_MENU: [1, 'menu'] } },
        menu: { opcode: 'sound_sounds_menu', topLevel: false, parent: 'snd', next: null, shadow: true,
          inputs: {}, fields: { SOUND_MENU: ['Tone', null] } },
        set: { opcode: 'data_setvariableto', parent: 'snd', next: null,
          inputs: { VALUE: [1, [4, '1']] }, fields: { VARIABLE: ['done', 'done'] } },
      }, costumes: [], sounds: [{
        assetId: 'tone', md5ext: 'tone.wav', dataFormat: 'wav', name: 'Tone',
        rate: 8000, sampleCount: 1600, format: '', 
      }], currentCostume: 0, volume: 100, layerOrder: 0,
    }],
    monitors: [], extensions: ['sound'], meta: { semver: '3.0.0' },
  })),
  'tone.wav': tone(),
});
const app = Bun.spawn(['bun', 'node_modules/vite/bin/vite.js', '--host', '127.0.0.1', '--port', '53989', '--strictPort'], {
  cwd: new URL('..', import.meta.url).pathname, stdout: 'pipe', stderr: 'pipe',
});
let browser: Awaited<ReturnType<typeof chromium.launch>> | undefined;
try {
  for (let i = 0; i < 100; i++) {
    try { if ((await fetch('http://127.0.0.1:53989/')).ok) break; } catch {}
    await Bun.sleep(100);
    if (i === 99) throw Error('Vite did not start');
  }
  browser = await chromium.launch({
    executablePath: '/usr/bin/google-chrome-stable', headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage', '--autoplay-policy=no-user-gesture-required'],
  });
  const page = await browser.newPage();
  const errors: string[] = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto('http://127.0.0.1:53989/');
  await page.locator('#project').setInputFiles({
    name: 'sound-e2e.sb3', mimeType: 'application/zip', buffer: Buffer.from(sb3),
  });
  await page.locator('#play').click();
  await page.waitForFunction(() => (document.querySelector('#player-vars')?.textContent ?? '').includes('done: 1'), {}, { timeout: 60000 });
  assert.equal(errors.length, 0, errors.join('\n'));
  await page.locator('#stop').click();
  await page.waitForFunction(() => (document.querySelector('#player-status')?.textContent ?? '').includes('停止'), {}, { timeout: 15000 });
  console.log('PASS Chrome WebAudio sound_playuntildone -> resolveWait -> next Scratch block');
} finally {
  await browser?.close();
  app.kill(); await app.exited;
}
