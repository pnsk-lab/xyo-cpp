import { chromium } from 'playwright-core';
import { strict as assert } from 'node:assert';
import { zipSync, strToU8 } from 'fflate';

const sb3 = zipSync({ 'project.json': strToU8(JSON.stringify({
  targets: [{ isStage: true, name: 'Stage', variables: { score: ['score', 0] },
    lists: {}, broadcasts: {}, blocks: {
      hat: { opcode: 'event_whenflagclicked', next: 'set', parent: null,
        topLevel: true, x: 0, y: 0, inputs: {}, fields: {} },
      set: { opcode: 'data_setvariableto', next: null, parent: 'hat',
        inputs: { VALUE: [1, [4, '42']] }, fields: { VARIABLE: ['score', 'score'] } },
    },
    costumes: [], sounds: [], currentCostume: 0, volume: 100, layerOrder: 0,
  }],
  monitors: [], extensions: [], meta: { semver: '3.0.0' },
})) });

const server = Bun.spawn([
  'bun', 'node_modules/vite/bin/vite.js', '--host', '127.0.0.1',
  '--port', '53987', '--strictPort',
], { cwd: new URL('..', import.meta.url).pathname, stdout: 'pipe', stderr: 'pipe' });
let browser: Awaited<ReturnType<typeof chromium.launch>> | undefined;

try {
  for (let i = 0; i < 100; i++) {
    try {
      const response = await fetch('http://127.0.0.1:53987/');
      if (response.ok) break;
    } catch { /* wait for listening */ }
    await Bun.sleep(100);
    if (i === 99) throw Error('Vite server never started');
  }
  browser = await chromium.launch({
    executablePath: '/usr/bin/google-chrome-stable', headless: true,
    args: ['--no-sandbox', '--disable-dev-shm-usage'],
  });
  const page = await browser.newPage();
  const errors: string[] = [];
  page.on('pageerror', err => errors.push('pageerror: ' + err.message));
  page.on('console', msg => { if (msg.type() !== 'debug') errors.push('console[' + msg.type() + ']: ' + msg.text()); });
  await page.goto('http://127.0.0.1:53987/');
  await page.locator('#project').setInputFiles({
    name: 'compiled.sb3', mimeType: 'application/zip', buffer: Buffer.from(sb3),
  });
  await page.locator('#run').click();
  await page.waitForFunction(() => {
    const text = document.querySelector('#status')?.textContent ?? '';
    return text.includes('実行しました') || text.includes('実行失敗') || text.includes('エラー') || text.includes('タイムアウト');
  }, undefined, { timeout: 60000 });

  const status = (await page.locator('#status').textContent()) ?? '';
  const text = (await page.locator('#result').textContent()) ?? '';
  console.log('status:', status);
  if (errors.length) console.log('browser diagnostics:', errors.join('\n'));
  assert.match(status, /LLVM生成コードを実行しました（C\+\+ jitEmitScript）/);
  const snapshot = JSON.parse(text);
  assert.equal(snapshot.targets[0].variables.score, '42');
  console.log('PASS: Chrome Worker all-WASM SB3 -> C++ LLVM IR -> WASM llc/lld -> xyo runtime');
} finally {
  await browser?.close();
  server.kill();
  await server.exited;
}
