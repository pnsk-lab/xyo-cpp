import { readdir, readFile, writeFile, mkdir, copyFile, rm } from 'node:fs/promises';
import { join, extname } from 'node:path';

const root = new URL('../../', import.meta.url);
const src = new URL('../../docs/', import.meta.url);
const content = new URL('../src/content/docs/', import.meta.url);
const reference = new URL('../src/content/docs/reference/', import.meta.url);
const research = new URL('../src/content/docs/research/', import.meta.url);

await rm(reference, { recursive: true, force: true });
await rm(research, { recursive: true, force: true });
await mkdir(reference, { recursive: true });
await mkdir(research, { recursive: true });

const selected = new Set(['design', 'wasm', 'scratch-bench', 'scratch-divergences']);
function fixLinks(body) {
  return body.replace(/\]\((?:\.\/)?(?:docs\/)?([\w.-]+)\.md(#[\w.-]+)?\)/g, (_m, name, anchor = '') => {
    const dir = selected.has(name) ? 'reference' : 'research';
    return `](/xyo-cpp/${dir}/${name}/${anchor})`;
  });
}
const files = await readdir(src);
for (const file of files) {
  const extension = extname(file);
  if (extension !== '.md' && extension !== '.png') continue;
  const name = file.slice(0, -extension.length);
  if (extension === '.png') {
    await copyFile(new URL(file, src), new URL(file, reference));
    await copyFile(new URL(file, src), new URL(file, research));
    continue;
  }
  const body = fixLinks(await readFile(new URL(file, src), 'utf8'));
  const title = (body.match(/^#\s+(.+)$/m)?.[1] || name).replace(/["']/g, '');
  const header = '---\ntitle: ' + JSON.stringify(title) + '\n---\n\n';
  await writeFile(new URL(file, selected.has(name) ? reference : research), header + body);
}
const running = fixLinks(await readFile(new URL('RUNNING.md', root), 'utf8'));
await writeFile(new URL('running.md', reference), '---\ntitle: プラットフォーム別の実行\n---\n\n' + running);
console.log('Synced', files.filter(x => x.endsWith('.md')).length, 'source documents');
