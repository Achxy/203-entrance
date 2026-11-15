import {readdir, readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
import {spawnSync} from 'node:child_process';
const root = fileURLToPath(new URL('..', import.meta.url));
async function check(folder) {
  for (const entry of await readdir(folder, {withFileTypes: true})) {
    const file = path.join(folder, entry.name);
    if (entry.isDirectory()) await check(file);
    else if (entry.name.endsWith('.mjs')) {
      const result = spawnSync(process.execPath, ['--check', file], {stdio: 'inherit'});
      if (result.status !== 0) process.exit(result.status ?? 1);
    }
  }
}
await check(path.join(root, 'src'));
await check(path.join(root, 'worker'));
await check(path.join(root, 'scripts'));
const html = await readFile(path.join(root, 'index.html'), 'utf8');
if (/<script(?![^>]*\bsrc=)[^>]*>/i.test(html) || /\son[a-z]+\s*=/i.test(html) || /\sstyle\s*=/i.test(html)) throw Error('Inline executable content is forbidden.');
console.log('JavaScript syntax and static CSP surface checks passed.');
