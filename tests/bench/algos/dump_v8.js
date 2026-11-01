// Capture TurboWarp's generated JavaScript for a benchmark project, and (with
// V8 flags) its bytecode / optimized machine code.
//
//   node tests/bench/algos/dump_v8.js <project.sb3> [outdir]
//       -> writes outdir/js_<n>_<name>.js for every compiled script and prints
//          a `# name: funN` index.
//
//   node --print-bytecode --print-bytecode-filter=fun3 \
//        tests/bench/algos/dump_v8.js <project.sb3>   > bytecode.txt
//   node --print-opt-code --print-opt-code-filter=fun3 --code-comments \
//        tests/bench/algos/dump_v8.js <project.sb3>   > optcode.txt
//
// The script executes the project headlessly (like run_tw.js), so the compiler
// actually runs and the hot procedure reaches TurboFan.
'use strict';

global.document = {hidden: true};

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '../../../refs/scratch-vm');
const VM = require(path.join(ROOT, 'src/index.js'));
const ScratchStorage = require(path.join(ROOT, 'node_modules/scratch-storage'));
const FakeRenderer = require(path.join(ROOT, 'test/fixtures/fake-renderer.js'));
const JSGenerator = require(path.join(ROOT, 'src/compiler/jsgen'));

function functionName(source) {
    const m = /function\s+\*\s*(\w+)\s*\(|function\s+(\w+)\s*\(/.exec(source);
    return m ? (m[1] || m[2]) : 'unknown';
}

async function main () {
    const file = process.argv[2];
    const outDir = process.argv[3] || path.join(__dirname, 'v8out');
    if (!file) {
        console.error('usage: node dump_v8.js <project.sb3> [outdir]');
        process.exit(2);
    }
    const sources = [];
    JSGenerator.testingApparatus = {
        report (gen, source) {
            sources.push({
                name: functionName(source),
                kind: gen.script.procedureCode || 'script',
                target: gen.target && gen.target.getName ? gen.target.getName() : '?'
            });
            sources[sources.length - 1].source = source;
        }
    };

    const vm = new VM();
    vm.setCompilerOptions({enabled: true});
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
    vm.runtime.greenFlag();
    for (let i = 0; i < 200000; i++) {
        vm.runtime._step();
        if (vm.runtime.threads.filter(t => !t.updateMonitor).length === 0) break;
    }

    fs.mkdirSync(outDir, {recursive: true});
    let n = 0;
    for (const s of sources) {
        const out = path.join(outDir, `js_${n}_${s.name}.js`);
        fs.writeFileSync(out, `// target=${s.target} kind=${s.kind}\n` + s.source + '\n');
        process.stderr.write(`# ${s.name}  ${s.target}  ${s.kind}  -> ${out}\n`);
        ++n;
    }
}

main().catch(err => {
    console.error(err && err.stack ? err.stack : err);
    process.exit(1);
});
