// Reference runner: execute a benchmark sb3 in the vendored scratch-vm
// (TurboWarp fork) and print the in-project `times`/`check` as JSON.
//
//   node tests/bench/algos/run_tw.js <project.sb3> <compiled|interp>
//
// `compiled` enables TurboWarp's compiler (its default and the mode the
// TurboWarp desktop/web app uses); `interp` is scratch-vm's interpreter.
//
// The fake renderer/document stubs are the same ones
// tests/differential/scratch_vm_dump.js needs: TurboWarp's runtime touches
// renderer and document even when no window exists.
'use strict';

global.document = {hidden: true};

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '../../../refs/scratch-vm');
const VM = require(path.join(ROOT, 'src/index.js'));
const ScratchStorage = require(path.join(ROOT, 'node_modules/scratch-storage'));
const FakeRenderer = require(path.join(ROOT, 'test/fixtures/fake-renderer.js'));

async function run (file, compiled) {
    const vm = new VM();
    vm.setCompilerOptions({enabled: compiled});
    vm.attachStorage(new ScratchStorage());
    const renderer = new FakeRenderer();
    renderer.destroyDrawable = () => {};
    renderer.destroySkin = () => {};
    renderer.createPenSkin = () => 1;
    renderer.penClear = () => {};
    renderer.penPoint = () => {};
    renderer.penLine = () => {};
    renderer.penStamp = () => {};
    renderer.updateDrawableSkinId = () => {};
    renderer.getSkinSize = () => [0, 0];
    vm.attachRenderer(renderer);

    await vm.loadProject(fs.readFileSync(file));
    const t0 = process.hrtime.bigint();
    vm.runtime.greenFlag();
    let steps = 0;
    for (; steps < 5000000; steps++) {
        vm.runtime._step();
        if (vm.runtime.threads.filter(t => !t.updateMonitor).length === 0) break;
    }
    const wallMs = Number(process.hrtime.bigint() - t0) / 1e6;

    const target = vm.runtime.targets.find(t => t.getName() === 'Bench');
    const vars = {};
    if (target) {
        for (const id of Object.keys(target.variables)) {
            const v = target.variables[id];
            vars[v.name] = v.type === 'list'
                ? Array.from(v.value).map(x => String(x))
                : String(v.value);
        }
    }
    return {
        times: (vars.times || []).map(Number),
        check: vars.check,
        wall_ms: wallMs,
        steps
    };
}

async function main () {
    const file = process.argv[2];
    const mode = process.argv[3] || 'compiled';
    if (!file) {
        console.error('usage: node run_tw.js <project.sb3> <compiled|interp>');
        process.exit(2);
    }
    const res = await run(file, mode !== 'interp');
    process.stdout.write(JSON.stringify(res) + '\n');
}

main().catch(err => {
    console.error(err && err.stack ? err.stack : err);
    process.exit(1);
});
