// Prepare (or --check) the recorded Scratch→LLVM example. No compiler is run.
// The normal site build uses the checked-in snapshot and needs neither Python nor xyo.
import { readFile, writeFile, mkdir } from 'node:fs/promises';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createHash } from 'node:crypto';
import { spawnSync } from 'node:child_process';
import assert from 'node:assert/strict';

const root = fileURLToPath(new URL('../../', import.meta.url));
const check = process.argv.includes('--check');
const capture = JSON.parse(await readFile(resolve(root, 'site/src/data/llvm-ir.json'), 'utf8'));
const provenance = JSON.parse(await readFile(resolve(root, 'site/public/ir/micro-loop.provenance.json'), 'utf8'));
assert.deepEqual(capture, provenance, 'IR metadata/provenance differ');
const hash = bytes => createHash('sha256').update(bytes).digest('hex');
const published = resolve(root, 'site/public/examples/micro-loop.sb3');
const project = await readFile(check ? published : resolve(root, capture.project));
assert.equal(hash(project), capture.projectSha256, 'Project differs from the recorded compilation');
const ir = await readFile(resolve(root, 'site/public', capture.sourceFile));
assert.equal(hash(ir), capture.irSha256, 'LLVM IR differs from the recorded snapshot');
const lines = ir.toString('utf8').trimEnd().split('\n');
assert.equal(lines.length, capture.moduleLines);
assert.deepEqual(lines.slice(capture.excerpt.startLine - 1, capture.excerpt.endLine), capture.excerpt.lines, 'Excerpt is not verbatim');

const inspect = spawnSync('python3', ['-c', String.raw`
import sys, json, zipfile, io
p=json.loads(zipfile.ZipFile(io.BytesIO(sys.stdin.buffer.read())).read('project.json'))
found=[]
for t in p['targets']:
 b=t.get('blocks',{})
 def ref(inp):
  assert isinstance(inp[1],str), inp
  return b[inp[1]]
 def num(inp):
  v=inp[1]
  if isinstance(v,str):
   shadow=b[v]
   return float(shadow['fields']['NUM'][0])
  assert isinstance(v,list) and v[0] in [4,5,6,7,8,10], v
  return float(v[1])
 for d in b.values():
  if d['opcode']!='procedures_definition': continue
  proto=ref(d['inputs']['custom_block'])
  m=proto['mutation']
  if m['proccode']!='bench %s': continue
  args=json.loads(m['argumentnames']); ids=json.loads(m['argumentids'])
  assert args==['n'] and len(ids)==1 and str(m['warp']).lower()=='true'
  initial=b[d['next']]
  assert initial['opcode']=='data_setvariableto' and num(initial['inputs']['VALUE'])==0
  variable,variable_id=initial['fields']['VARIABLE']
  assert variable=='i'
  repeat=b[initial['next']]
  assert repeat['opcode']=='control_repeat' and not repeat.get('next')
  arg=ref(repeat['inputs']['TIMES'])
  assert arg['opcode']=='argument_reporter_string_number' and arg['fields']['VALUE'][0]=='n'
  change=ref(repeat['inputs']['SUBSTACK'])
  assert change['opcode']=='data_changevariableby' and change['fields']['VARIABLE']==[variable,variable_id]
  assert num(change['inputs']['VALUE'])==1 and not change.get('next')
  calls=[num(x['inputs'][ids[0]]) for x in b.values() if x['opcode']=='procedures_call' and x['mutation']['proccode']==m['proccode']]
  assert 1000000 in calls and 1000 in calls
  assert any(x['opcode']=='sensing_dayssince2000' for x in b.values())
  found.append(dict(sprite=t['name'],proccode=m['proccode'],argument=args[0],variable=variable,initial=0,delta=1,exampleCount=1000000,warp=True))
assert len(found)==1, found
print(json.dumps(found[0]))
`], { input: project, encoding: 'utf8', maxBuffer: 1024 * 1024 });
if (inspect.status !== 0 || inspect.error) throw new Error(`Source verification failed: ${inspect.error?.message ?? inspect.stderr}`);
const source = JSON.parse(inspect.stdout);
const lineFor = predicate => {
  const found = capture.excerpt.lines.map((text, index) => ({ text, line: capture.excerpt.startLine + index })).filter(x => predicate(x.text));
  assert.equal(found.length, 1, 'Expected exactly one matching IR instruction');
  return found[0];
};
const repeat = lineFor(text => /%rep_i[^ ]* = phi double/.test(text));
const increment = lineFor(text => /fadd double %pvd[^,]*, 1\.000000e\+00/.test(text));
const result = increment.text.match(/^(\s*)(%\d+) =/)[2];
const store = lineFor(text => text.trim() === `store double ${result}, ptr %pv, align 8`);
const example = {
  title: 'Counter loop', sourceFile: 'examples/micro-loop.sb3',
  projectSha256: capture.projectSha256, irSha256: capture.irSha256,
  source,
  steps: [
    { id: 'loop', label: `repeat ${source.argument}`, lines: [repeat.line], explanation: 'The remaining-repeat counter is carried between iterations with phi. Its decrement and exit test are in another basic block.' },
    { id: 'increment', label: `change ${source.variable} by ${source.delta}`, lines: [increment.line, store.line], explanation: 'fadd adds one to the running numeric value of i. store saves it in a compiler temporary; the variable-slot writeback is elsewhere in the module.' },
  ],
};
const output = resolve(root, 'site/src/data/compilation-example.json');
if (check) {
  assert.deepEqual(JSON.parse(await readFile(output, 'utf8')), example, 'Example metadata is stale; run node site/scripts/prepare-compilation-example.mjs');
  console.log('Verified project hash, actual source blocks, IR hash, verbatim excerpt, and instruction mapping.');
} else {
  for (const [file, bytes] of [[output, JSON.stringify(example, null, 2) + '\n'], [published, project]]) {
    await mkdir(dirname(file), { recursive: true });
    let previous;
    try { previous = await readFile(file); } catch (error) { if (error.code !== 'ENOENT') throw error; }
    const next = Buffer.from(bytes);
    if (!previous?.equals(next)) await writeFile(file, next);
  }
  console.log('Prepared verified source project and compilation example metadata.');
}
