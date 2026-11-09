import { chromium } from 'playwright-core';
import { strict as assert } from 'node:assert';
import { zipSync, strToU8 } from 'fflate';

const svg = '<svg xmlns="http://www.w3.org/2000/svg" width="40" height="40"><rect x="0" y="0" width="40" height="40" fill="#ff0000"/></svg>';
const stage = {
  isStage: true, name: 'Stage', variables: { score: ['score', 0] },
  lists: {}, broadcasts: {}, blocks: {
    hat: { opcode: 'event_whenflagclicked', next: 'set', topLevel: true, parent: null, x: 0, y: 0, inputs: {}, fields: {} },
    set: { opcode: 'data_setvariableto', parent: 'hat', next: null,
      inputs: { VALUE: [1, [4, '7']] }, fields: { VARIABLE: ['score', 'score'] } },
    keyhat: { opcode: 'event_whenkeypressed', topLevel: true, parent: null, next: 'change', x: 20, y: 0,
      inputs: {}, fields: { KEY_OPTION: ['space', null] } },
    change: { opcode: 'data_changevariableby', parent: 'keyhat', next: null,
      inputs: { VALUE: [1, [4, '3']] }, fields: { VARIABLE: ['score', 'score'] } },
  }, costumes: [], sounds: [], currentCostume: 0, volume: 100, layerOrder: 0,
};
const sprite = {
  isStage: false, name: 'Square', variables: {}, lists: {}, broadcasts: {}, blocks: {},
  costumes: [{ name: 'Red', assetId: 'red', md5ext: 'red.svg', dataFormat: 'svg',
    rotationCenterX: 20, rotationCenterY: 20, bitmapResolution: 1 }],
  sounds: [], x: 0, y: 0, direction: 90, size: 100, visible: true,
  currentCostume: 0, rotationStyle: 'all around', layerOrder: 1,
};
const sb3 = zipSync({
  'project.json': strToU8(JSON.stringify({
    targets: [stage, sprite], monitors: [], extensions: [], meta: { semver: '3.0.0' },
  })),
  'red.svg': strToU8(svg),
});

const app = Bun.spawn(['bun', 'node_modules/vite/bin/vite.js', '--host', '127.0.0.1', '--port', '53988', '--strictPort'], {
  cwd: new URL('..', import.meta.url).pathname, stdout: 'pipe', stderr: 'pipe',
});
let browser: Awaited<ReturnType<typeof chromium.launch>> | undefined;
try {
  for (let i = 0; i < 100; ++i) {
    try { if ((await fetch('http://127.0.0.1:53988/')).ok) break; } catch {}
    await Bun.sleep(100);
    if (i === 99) throw Error('Vite did not start');
  }
  browser = await chromium.launch({
    executablePath: '/usr/bin/google-chrome-stable', headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage'],
  });
  const page = await browser.newPage();
  const diagnostics: string[] = [];
  page.on('pageerror', e => diagnostics.push(e.message));
  await page.goto('http://127.0.0.1:53988/');
  await page.locator('#project').setInputFiles({
    name: 'player-e2e.sb3', mimeType: 'application/zip', buffer: Buffer.from(sb3),
  });
  await page.locator('#play').click();
  await page.waitForFunction(() => (document.querySelector('#player-frame')?.textContent ?? '').includes('フレーム 5'), {}, { timeout: 60000 });
  const pixel = await page.locator('#stage').evaluate((element: HTMLCanvasElement) => {
    const data = element.getContext('2d')!.getImageData(240, 180, 1, 1).data;
    return [...data];
  });
  console.log('center pixel:', pixel);
  assert.ok(pixel[0]! > 220 && pixel[1]! < 70 && pixel[2]! < 70, 'red sprite was not drawn');
  const initial = await page.locator('#player-vars').textContent();
  assert.match(initial ?? '', /score: 7/);
  await page.locator('#stage').focus();
  await page.keyboard.down('Space');
  await page.keyboard.up('Space');
  await page.waitForFunction(() => (document.querySelector('#player-vars')?.textContent ?? '').includes('score: 10'), {}, { timeout: 15000 });
  await page.locator('#stop').click();
  await page.waitForFunction(() => (document.querySelector('#player-status')?.textContent ?? '').includes('停止'), {}, { timeout: 15000 });
  assert.deepEqual(diagnostics, []);
  console.log('PASS: Chrome persistent xyo runtime + costume SVG + keyboard events + stop');
} finally {
  await browser?.close();
  app.kill();
  await app.exited;
}
