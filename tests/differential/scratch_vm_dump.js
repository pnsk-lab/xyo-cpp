// Headless reference runner: load an sb3 in the vendored scratch-vm, run it
// until the scripts stop, and dump every variable/list as String(value).
//
//   node tests/differential/scratch_vm_dump.js <project.sb3> [maxSteps]
//
// String(value) is exactly the representation Scratch uses for join/say/list
// display, so the output is directly comparable with xyo's XYO_DUMPVARS dump.
//
// The fake renderer and the document stub are required because TurboWarp's
// runtime calls renderer/destroyDrawable and reads document.hidden even when
// no window exists.
'use strict';

global.document = {hidden: true};

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '../../refs/scratch-vm');
const VM = require(path.join(ROOT, 'src/index.js'));
const ScratchStorage = require(path.join(ROOT, 'node_modules/scratch-storage'));
const FakeRenderer = require(path.join(ROOT, 'test/fixtures/fake-renderer.js'));

async function main () {
    const file = process.argv[2];
    const maxSteps = parseInt(process.argv[3] || '2000', 10);
    if (!file) {
        console.error('usage: node scratch_vm_dump.js <project.sb3> [maxSteps]');
        process.exit(2);
    }

    const vm = new VM();
    // Official Scratch semantics: run scratch-vm's interpreter, not
    // TurboWarp's optional compiler (they intentionally differ in a few
    // places, e.g. data_listcontents' single-character test).
    vm.setCompilerOptions({enabled: false});
    vm.attachStorage(new ScratchStorage());
    const renderer = new FakeRenderer();
    // RenderedTarget.dispose() calls these when a clone is deleted.
    renderer.destroyDrawable = () => {};
    renderer.destroySkin = () => {};
    // The pen extension needs a skin API; headless runs never rasterise, so
    // recording nothing is enough to keep the project running.
    renderer.createPenSkin = () => 1;
    renderer.penClear = () => {};
    renderer.penPoint = () => {};
    renderer.penLine = () => {};
    renderer.penStamp = () => {};
    renderer.updateDrawableSkinId = () => {};
    renderer.getSkinSize = () => [0, 0];
    vm.attachRenderer(renderer);

    await vm.loadProject(fs.readFileSync(file));
    vm.runtime.greenFlag();

    for (let i = 0; i < maxSteps; i++) {
        vm.runtime._step();
        const active = vm.runtime.threads.filter(t => !t.updateMonitor);
        if (active.length === 0) break;
    }

    const out = {};
    for (const target of vm.runtime.targets) {
        const vars = {};
        for (const id of Object.keys(target.variables)) {
            const v = target.variables[id];
            vars[v.name] = v.type === 'list'
                ? Array.from(v.value).map(x => String(x))
                : String(v.value);
        }
        if (Object.keys(vars).length > 0) out[target.getName()] = vars;
    }
    process.stdout.write(JSON.stringify(out, null, 1) + '\n');
}

main().catch(err => {
    console.error(err);
    process.exit(1);
});
