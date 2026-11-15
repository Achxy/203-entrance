import {mkdir, readFile, readdir, rm, writeFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const root = fileURLToPath(new URL('..', import.meta.url));
const types = {'.html': 'text/html; charset=utf-8', '.css': 'text/css; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8', '.svg': 'image/svg+xml; charset=utf-8',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.webp': 'image/webp', '.ico': 'image/x-icon', '.woff2': 'font/woff2'};

export async function buildSite(destination = path.join(root, 'dist')) {
  const manifest = JSON.parse(await readFile(path.join(root, '.openai/hosting.json'), 'utf8'));
  if (typeof manifest.project_id !== 'string' || !manifest.project_id || Object.hasOwn(manifest, 'static'))
    throw Error('Worker output requires the existing Site identity and no static manifest entry.');
  const files = ['index.html', 'styles.css'];
  async function collect(relative, modulesOnly = false) {
    const entries = await readdir(path.join(root, relative), {withFileTypes: true});
    for (const entry of entries.sort((a, b) => a.name.localeCompare(b.name, 'en'))) {
      const next = path.posix.join(relative, entry.name);
      if (entry.isFile() && (modulesOnly ? entry.name.endsWith('.mjs') : Object.hasOwn(types, path.extname(entry.name)))) files.push(next);
      else if (entry.isDirectory() && (!modulesOnly || relative === 'src' && entry.name === 'security')) await collect(next, modulesOnly);
    }
  }
  await collect('assets'); await collect('src', true);
  const assets = Object.create(null);
  for (const file of files.sort()) {
    const bytes = await readFile(path.join(root, file));
    assets['/' + file] = {type: types[path.extname(file)], length: bytes.length, body: bytes.toString('base64')};
  }
  const handler = await readFile(path.join(root, 'worker/handler.mjs'), 'utf8');
  const output = handler + '\nconst bundledAssets = Object.freeze(' + JSON.stringify(assets) + ');\nexport default createWorker(bundledAssets);\n';
  // Prepare the complete artifact before replacing the obsolete static output.
  await rm(destination, {recursive: true, force: true});
  await mkdir(path.join(destination, 'server'), {recursive: true});
  await mkdir(path.join(destination, '.openai'), {recursive: true});
  await writeFile(path.join(destination, 'server/index.js'), output);
  await writeFile(path.join(destination, '.openai/hosting.json'), JSON.stringify(manifest, null, 2) + '\n');
  return {files: Object.keys(assets), bytes: Buffer.byteLength(output), projectID: manifest.project_id};
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  const result = await buildSite();
  console.log(`Built Worker ESM with ${result.files.length} embedded assets (${result.bytes} bytes); excluded fixtures, tests and private metadata from routes.`);
}
