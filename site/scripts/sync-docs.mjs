import { readdir, readFile, writeFile, mkdir, copyFile, rm } from 'node:fs/promises';
import { extname } from 'node:path';

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
// docs/ is organized into subdirectories (bench/, jit/, render/, ...).
// Walk recursively; basenames are unique across the tree.
async function* walk(dir) {
  for (const entry of await readdir(dir, { withFileTypes: true })) {
    const path = new URL(entry.name, dir);
    if (entry.isDirectory()) yield* walk(new URL(entry.name + '/', dir));
    else yield { name: entry.name, path };
  }
}
const files = [];
for await (const entry of walk(src)) {
  const extension = extname(entry.name);
  if (extension === '.md' || extension === '.png') files.push(entry);
}
for (const { name: file, path } of files) {
  const extension = extname(file);
  if (extension !== '.md' && extension !== '.png') continue;
  const name = file.slice(0, -extension.length);
  if (extension === '.png') {
    await copyFile(path, new URL(file, reference));
    await copyFile(path, new URL(file, research));
    continue;
  }
  const body = fixLinks(await readFile(path, 'utf8'));
  const title = (body.match(/^#\s+(.+)$/m)?.[1] || name).replace(/["']/g, '');
  const header = '---\ntitle: ' + JSON.stringify(title) + '\n---\n\n';
  await writeFile(new URL(file, selected.has(name) ? reference : research), header + body);
}
const running = fixLinks(await readFile(new URL('RUNNING.md', root), 'utf8'));
await writeFile(new URL('running.md', reference), '---\ntitle: プラットフォーム別の実行\n---\n\n' + running);
console.log('Synced', files.filter(x => x.name.endsWith('.md')).length, 'source documents');
