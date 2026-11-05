import { zipSync, strToU8 } from 'fflate';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';

const { default: createRuntime } = await import('./build/xyo_browser_unified.mjs');
const runtime = await createRuntime();
const project = {
  targets: [{
    isStage: true, name: 'Stage', variables: { score: ['score', 0] },
    lists: {}, broadcasts: {}, blocks: {
      flag: { opcode: 'event_whenflagclicked', next: 'set', parent: null, inputs: {}, fields: {}, topLevel: true, x: 0, y: 0 },
      set: { opcode: 'data_setvariableto', next: 'change', parent: 'flag',
        inputs: { VALUE: [1, [4, '40']] }, fields: { VARIABLE: ['score', 'score'] }, topLevel: false },
      change: { opcode: 'data_changevariableby', next: null, parent: 'set',
        inputs: { VALUE: [1, [4, '2']] }, fields: { VARIABLE: ['score', 'score'] }, topLevel: false },
    },
    comments: {}, currentCostume: 0, costumes: [], sounds: [], volume: 100, layerOrder: 0, tempo: 60, videoTransparency: 50,
  }],
  monitors: [], extensions: [], meta: { semver: '3.0.0' },
};
const sb3 = zipSync({ 'project.json': strToU8(JSON.stringify(project)) });
const handle = runtime._xyo_browser_create();
assert.ok(handle > 0, 'create');
const ptr = runtime._malloc(sb3.length);
runtime.HEAPU8.set(sb3, ptr);
assert.equal(runtime._xyo_browser_load_sb3(handle, ptr, sb3.length), 1, 'load');
runtime._free(ptr);
assert.equal(runtime._xyo_browser_green_flag(handle), 1, 'green flag');
for (let i = 0; i < 5; i++) assert.equal(runtime._xyo_browser_step(handle), 1, 'step');
const snapshot = JSON.parse(runtime.UTF8ToString(runtime._xyo_browser_snapshot(handle)));
assert.equal(snapshot.targets[0].variables.score, '42');
// Load a genuine PIC wasm side module through Emscripten dlopen.
const utf8 = new TextEncoder();
const withBytes = (data, action) => {
  const address = runtime._malloc(data.byteLength || 1);
  runtime.HEAPU8.set(data, address);
  try { return action(address); }
  finally { runtime._free(address); }
};
const withText = (value, action) => withBytes(utf8.encode(value + '\0'), action);
assert.equal(withText('flag', p => runtime._xyo_llvm_prepare(handle, p)), 1);
assert.equal(runtime._xyo_llvm_expect_slots(handle, 1), 1);
runtime.FS.writeFile('/side.wasm', readFileSync(new URL('./build/side/side.wasm', import.meta.url)));
const opened = withText('/side.wasm', p => runtime._xyo_llvm_dlopen(handle, p));
assert.ok(opened, 'dlopen side');
const symbol = name => withText(name, p => runtime._xyo_llvm_dlsym(opened, p));
const entry = symbol('xyo_browser_script');
const slot = symbol('xyo_aot_slot_0');
assert.ok(entry > 0 && slot > 0, 'dlsym');
assert.equal(withBytes(utf8.encode('score'), p => runtime._xyo_llvm_bind_slot(handle, slot, 0, p, 5)), 1);
assert.equal(runtime._xyo_llvm_is_sealed(handle), 1);
runtime._xyo_llvm_invoke(handle, entry);
const after = JSON.parse(runtime.UTF8ToString(runtime._xyo_browser_snapshot(handle)));
assert.equal(after.targets[0].variables.score, '99', 'side module uses same xyo::Runtime variables');
assert.equal(runtime._xyo_browser_destroy(handle), 1);
console.log('PASS xyo-cpp unified Wasm: interpreter -> 42, dynamically linked LLVM side -> 99');
