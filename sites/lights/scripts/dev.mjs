import {createServer} from 'node:http';
import {fileURLToPath, pathToFileURL} from 'node:url';
import path from 'node:path';
import {buildSite} from './build.mjs';
await buildSite();
const root = fileURLToPath(new URL('..', import.meta.url));
const {default: worker, SECURITY_HEADERS} = await import(pathToFileURL(path.join(root, 'dist/server/index.js')));
const port = Number(process.env.PORT || 8793);
if (!Number.isSafeInteger(port) || port < 1024 || port > 65535) throw Error('Invalid preview port.');
createServer(async (incoming, outgoing) => {
  try {
    const url = new URL(incoming.url, `http://127.0.0.1:${port}`);
    const response = await worker.fetch({url: url.href, method: incoming.method});
    outgoing.writeHead(response.status, Object.fromEntries(response.headers));
    outgoing.end(Buffer.from(await response.arrayBuffer()));
  } catch {outgoing.writeHead(500, {...SECURITY_HEADERS, 'Content-Type': 'text/plain; charset=utf-8'}); outgoing.end(incoming.method === 'HEAD' ? undefined : 'Preview failed\n');}
}).listen(port, '127.0.0.1', () => console.log(`Worker preview: http://127.0.0.1:${port}`));
