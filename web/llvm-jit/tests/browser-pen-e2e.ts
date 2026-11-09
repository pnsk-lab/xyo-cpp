import { chromium } from 'playwright-core';
import { strict as assert } from 'node:assert';
import { zipSync, strToU8 } from 'fflate';

const blocks = {
  hat: { opcode: 'event_whenflagclicked', topLevel: true, parent: null, next: 'down', inputs: {}, fields: {}, x: 0, y: 0 },
  down: { opcode: 'pen_penDown', parent: 'hat', next: 'move', inputs: {}, fields: {} },
  move: { opcode: 'motion_movesteps', parent: 'down', next: 'up', inputs: { STEPS: [1, [4, '60']] }, fields: {} },
  up: { opcode: 'pen_penUp', parent: 'move', next: null, inputs: {}, fields: {} },
};
const sb3 = zipSync({ 'project.json': strToU8(JSON.stringify({
  targets: [
    { isStage: true, name: 'Stage', variables: {}, lists: {}, blocks: {}, costumes: [], sounds: [], layerOrder: 0 },
    { isStage: false, name: 'Painter', variables: {}, lists: {}, blocks, costumes: [], sounds: [],
      x: -30, y: 0, direction: 90, size: 100, visible: false, layerOrder: 1, rotationStyle: 'all around' },
  ], monitors: [], extensions: ['pen'], meta: { semver: '3.0.0' },
})) });
const app = Bun.spawn(['bun', 'node_modules/vite/bin/vite.js', '--host', '127.0.0.1', '--port', '53990', '--strictPort'], {
  cwd: new URL('..', import.meta.url).pathname, stdout: 'pipe', stderr: 'pipe',
});
let browser: Awaited<ReturnType<typeof chromium.launch>> | undefined;
try {
  for (let i = 0; i < 100; i++) {
    try { if ((await fetch('http://127.0.0.1:53990/')).ok) break; } catch {}
    await Bun.sleep(100);
    if (i === 99) throw Error('Vite did not start');
  }
  browser = await chromium.launch({
    executablePath: '/usr/bin/google-chrome-stable', headless: true,
    args: ['--no-sandbox','--disable-dev-shm-usage'],
  });
  const page = await browser.newPage();
  const errors: string[] = [];
  page.on('pageerror', e => errors.push(e.message));
  await page.goto('http://127.0.0.1:53990/');
  await page.locator('#project').setInputFiles({
    name: 'pen-e2e.sb3', mimeType: 'application/zip', buffer: Buffer.from(sb3),
  });
  await page.locator('#play').click();
  await page.waitForFunction(() => (document.querySelector('#player-frame')?.textContent ?? '').includes('フレーム 5'), {}, { timeout: 60000 });
  const pixel = await page.locator('#stage').evaluate((canvas: HTMLCanvasElement) =>
    [...canvas.getContext('2d')!.getImageData(240, 180, 1, 1).data]);
  console.log('pen center pixel:', pixel);
  assert.ok(pixel[2]! >= 180 && pixel[0]! < 150, 'pen stroke not rendered');
  assert.deepEqual(errors, []);
  console.log('PASS Chrome Scratch pen extension -> Canvas2D');
} finally {
  await browser?.close();
  app.kill(); await app.exited;
}
