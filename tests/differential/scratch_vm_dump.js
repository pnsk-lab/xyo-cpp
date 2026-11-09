// Headless reference runner: load an sb3 in the vendored scratch-vm, run it
// until the scripts stop, and dump every variable/list as String(value).
//
//   node tests/differential/scratch_vm_dump.js <project.sb3> [maxSteps]
//
// Optional fourth argument 'compiled' enables TurboWarp's compiler.
// The default 'interp' preserves the original reference behavior.
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
    const mode = process.argv[4] || 'interp';
    if (!['interp', 'compiled'].includes(mode)) {
        throw new Error('mode must be interp or compiled');
    }
    if (!file) {
        console.error('usage: node scratch_vm_dump.js <project.sb3> [maxSteps] [interp|compiled]');
        process.exit(2);
    }

    const vm = new VM();
    // Default: scratch-vm reference interpreter. Optional compiled mode:
    // TurboWarp's native JavaScript compiler, with the same project/input.
    vm.setCompilerOptions({enabled: mode === 'compiled'});
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

    // A flag being enabled does not prove compilation succeeded. Verify at
    // least one green-flag script compiled successfully in compiled mode.
    if (mode === 'compiled') {
        let compiledScripts = 0;
        let compileFailures = 0;
        for (const target of vm.runtime.targets) {
            for (const script of target.blocks.getScripts()) {
                const result = target.blocks.getCachedCompileResult(script);
                if (result) {
                    if (result.success) compiledScripts++;
                    else compileFailures++;
                }
            }
        }
        if (compiledScripts < 1 || compileFailures > 0) {
            throw new Error('TurboWarp compiled mode not exercised: ' +
                JSON.stringify({compiledScripts, compileFailures}));
        }
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
