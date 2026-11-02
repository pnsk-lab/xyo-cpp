'use strict';
// Diagnostic only: process-wide profiles include startup, loading, compilation,
// warmup, all three measured rounds, verification and shutdown. Not a benchmark.
// node tests/bench/algos/profile_tw_strings.js [output-directory]
// Existing projects must be scale80 build/string-perf/{join,join_observe,join_observe_last}.sb3.
const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const crypto = require('crypto');
const root = path.resolve(__dirname, '../../..');
const out = path.resolve(process.argv[2] || path.join(root, 'build/string-profile-v8'));
if (fs.existsSync(out)) throw new Error('Choose a new output directory: ' + out);
fs.mkdirSync(out, {recursive: true});
const runnerPath = path.join(out, 'runner.js');
let runner = fs.readFileSync(path.join(__dirname, 'run_tw.js'), 'utf8');
runner = runner.replace("path.resolve(__dirname, '../../../refs/scratch-vm')", `path.resolve(${JSON.stringify(root)}, 'refs/scratch-vm')`);
runner = runner.replace('const VM = require', `const JSGenerator = require(path.join(ROOT, 'src/compiler/jsgen'));
const outDir = process.env.PROFILE_OUT;
fs.mkdirSync(outDir, {recursive:true});
let serial=0;
JSGenerator.testingApparatus={report(gen,source){fs.writeFileSync(path.join(outDir, 'generated_'+(serial++)+'.js'), '// kind='+ (gen.script.procedureCode || 'script')+'\\n'+source+'\\n');}};
console.error(JSON.stringify({node:process.version,v8:process.versions.v8,execPath:process.execPath,argv:process.execArgv}));
const VM = require`);
fs.writeFileSync(runnerPath, runner);
const manifest = {node: process.version, v8: process.versions.v8, execPath: process.execPath,
    scope: 'Whole process; diagnostic only; times in stdout are instrumented and not new benchmark results', commands: []};
function run(name, mode, flags) {
    const dir = path.join(out, name, mode);
    fs.mkdirSync(dir, {recursive:true});
    const project = path.join(root, 'build/string-perf', name + '.sb3');
    const args = [...flags, runnerPath, project, 'compiled'];
    const expected = name === 'join' ? '3200000' : '320000';
    const entry = {name, mode, executable:process.execPath, args, PROFILE_OUT:dir,
        project_sha256: crypto.createHash('sha256').update(fs.readFileSync(project)).digest('hex'),
        expected_check: expected};
    manifest.commands.push(entry);
    const stdout = fs.openSync(path.join(out, name, mode+'.log'), 'w');
    const stderr = fs.openSync(path.join(out, name, mode+'.stderr'), 'w');
    const result = cp.spawnSync(process.execPath, args, {cwd:root, env:{...process.env, PROFILE_OUT:dir}, stdio:['ignore',stdout,stderr]});
    fs.closeSync(stdout); fs.closeSync(stderr);
    if (result.error || result.status !== 0) throw result.error || new Error(`${name}/${mode}: exit ${result.status}`);
    const lines = fs.readFileSync(path.join(out, name, mode+'.log'), 'utf8').split('\n');
    let payload;
    for (const line of lines.reverse()) {
        try { const value = JSON.parse(line); if (Array.isArray(value.times) && value.check !== undefined) { payload = value; break; } } catch (_) {}
    }
    if (!payload || String(payload.check) !== expected || payload.times.length !== 3)
        throw new Error(`${name}/${mode}: missing or incorrect benchmark result`);
    entry.result = payload;
}
for (const name of ['join','join_observe','join_observe_last']) {
    run(name, 'trace', ['--trace-opt','--trace-deopt','--trace-gc']);
    const generated = fs.readdirSync(path.join(out,name,'trace')).filter(f=>f.endsWith('.js')).map(f=>fs.readFileSync(path.join(out,name,'trace',f),'utf8')).join('\n');
    const match = /return function\s+(\w+)\s*\(p0\)/.exec(generated);
    if (!match) throw new Error('Cannot identify actual benchmark function name');
    run(name, 'cpu', ['--cpu-prof', '--cpu-prof-dir='+path.join(out,name,'cpu'), '--cpu-prof-name=process.cpuprofile']);
    run(name, 'opt', ['--print-opt-code','--print-opt-code-filter='+match[1],'--code-comments']);
}
fs.writeFileSync(path.join(out,'manifest.json'), JSON.stringify(manifest,null,2)+'\n');
console.log(out);
console.log('Flatten counts are NOT measured. CPU samples and GC totals cover the entire process.');
