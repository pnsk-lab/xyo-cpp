#!/usr/bin/env node
// Generate src/core/case_props.inc from the running Node's Unicode data.
// Used by u16ToLower's Final_Sigma rule (Greek capital sigma at word end).
//
//   node tools/gen_case_props.mjs > src/core/case_props.inc
const reCased = new RegExp('^\\p{Cased}$', 'u');
const reIgnorable = new RegExp('^\\p{Case_Ignorable}$', 'u');

function ranges(re) {
  const out = [];
  let start = -1;
  for (let cp = 0; cp <= 0x10ffff; ++cp) {
    if (cp >= 0xd800 && cp <= 0xdfff) {
      if (start >= 0) { out.push([start, cp - 1]); start = -1; }
      continue;
    }
    const ok = re.test(String.fromCodePoint(cp));
    if (ok && start < 0) start = cp;
    else if (!ok && start >= 0) { out.push([start, cp - 1]); start = -1; }
  }
  if (start >= 0) out.push([start, 0x10ffff]);
  return out;
}

const hex = cp => '0x' + cp.toString(16);
console.log('// 自動生成: Unicode Cased / Case_Ignorable（Node の');
console.log('// \\p{Cased} / \\p{Case_Ignorable} から生成。Final_Sigma 判定用。');
console.log('// 生成: node tools/gen_case_props.mjs > src/core/case_props.inc');
console.log('static const CasePropRange kCasedRanges[] = {');
for (const [l, h] of ranges(reCased)) console.log(`    {${hex(l)}, ${hex(h)}},`);
console.log('};');
console.log('static const CasePropRange kCaseIgnorableRanges[] = {');
for (const [l, h] of ranges(reIgnorable)) console.log(`    {${hex(l)}, ${hex(h)}},`);
console.log('};');
