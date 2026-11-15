import test, {before, after} from 'node:test';
import assert from 'node:assert/strict';
import {mkdtemp, readFile, readdir, rm} from 'node:fs/promises';
import {tmpdir} from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {buildSite} from '../scripts/build.mjs';
import {SECURITY_HEADERS} from '../worker/handler.mjs';

const root = fileURLToPath(new URL('..', import.meta.url));
let temporary, destination, worker, result, moduleSource;
before(async () => {
  temporary = await mkdtemp(path.join(tmpdir(), '203-lights-worker-'));
  destination = path.join(temporary, 'first'); result = await buildSite(destination);
  moduleSource = await readFile(path.join(destination, 'server/index.js'), 'utf8');
  ({default: worker} = await import('data:text/javascript;base64,' + Buffer.from(moduleSource).toString('base64')));
});
after(async () => {if (temporary) await rm(temporary, {recursive: true, force: true});});
const request = (pathname = '/', options = {}) => new Request('https://iot.203.at.achyuth.dev' + pathname, options);
function secure(response) {
  for (const [name, value] of Object.entries(SECURITY_HEADERS)) assert.equal(response.headers.get(name), value, name);
  assert.match(response.headers.get('Content-Security-Policy'), /frame-ancestors 'none'/);
  assert.match(response.headers.get('Content-Security-Policy'), /script-src 'self'/);
  assert.doesNotMatch(response.headers.get('Content-Security-Policy'), /unsafe-inline|unsafe-eval|\*/);
  assert.equal(response.headers.get('X-Frame-Options'), 'DENY');
  assert.equal(response.headers.get('Cache-Control'), 'no-store');
  assert.equal(response.headers.get('Set-Cookie'), null);
}

test('the generated Worker serves the exact original HTML with enforceable response headers', async () => {
  const response = await worker.fetch(request()); secure(response);
  const expected = await readFile(path.join(root, 'index.html'));
  assert.equal(response.status, 200); assert.equal(response.headers.get('Content-Type'), 'text/html; charset=utf-8');
  assert.equal(response.headers.get('Content-Length'), String(expected.length));
  assert.deepEqual(Buffer.from(await response.arrayBuffer()), expected);
});

test('every compiled asset is byte-identical to source with an explicit safe MIME type', async () => {
  for (const pathname of result.files) {
    const response = await worker.fetch(request(pathname)); secure(response);
    assert.equal(response.status, 200, pathname);
    const expected = await readFile(path.join(root, pathname.slice(1)));
    assert.equal(response.headers.get('Content-Length'), String(expected.length), pathname);
    assert.deepEqual(Buffer.from(await response.arrayBuffer()), expected, pathname);
    if (pathname.endsWith('.mjs')) assert.equal(response.headers.get('Content-Type'), 'text/javascript; charset=utf-8');
    else if (pathname.endsWith('.css')) assert.equal(response.headers.get('Content-Type'), 'text/css; charset=utf-8');
    else if (pathname.endsWith('.svg')) assert.equal(response.headers.get('Content-Type'), 'image/svg+xml; charset=utf-8');
  }
});

test('HEAD preserves GET metadata and security policy without returning an asset body', async () => {
  for (const pathname of ['/', '/styles.css', '/src/app.mjs', '/src/security/gateway-web-session.mjs', '/assets/bulb.svg']) {
    const get = await worker.fetch(request(pathname)), head = await worker.fetch(request(pathname, {method: 'HEAD'}));
    secure(head); assert.equal(head.status, 200); assert.equal((await head.arrayBuffer()).byteLength, 0);
    assert.deepEqual(Object.fromEntries(head.headers), Object.fromEntries(get.headers));
  }
});

test('preview and asset query strings preserve the app while never selecting a different asset', async () => {
  const expected = await readFile(path.join(root, 'index.html'), 'utf8');
  for (const query of ['?preview=1', '?preview=1&role=guest&scenario=frozen', '?anything=%3Cscript%3E']) {
    const response = await worker.fetch(request('/' + query)); secure(response);
    assert.equal(response.status, 200); assert.equal(await response.text(), expected);
  }
  const original = await worker.fetch(request('/src/app.mjs'));
  const versioned = await worker.fetch(request('/src/app.mjs?v=123'));
  assert.equal(await versioned.text(), await original.text());
  assert.equal((await worker.fetch(request('/missing?path=/index.html'))).status, 404);
});

test('unsupported methods return 405 and never expose an asset or omit security headers', async () => {
  for (const method of ['POST', 'PUT', 'PATCH', 'DELETE', 'OPTIONS']) {
    const response = await worker.fetch(request('/', {method})); secure(response);
    assert.equal(response.status, 405); assert.equal(response.headers.get('Allow'), 'GET, HEAD');
    assert.equal(response.headers.get('Content-Type'), 'text/plain; charset=utf-8');
    assert.equal(await response.text(), 'Method not allowed\n');
  }
});

test('unknown and private paths return bounded plain-text 404 without SPA fallback', async () => {
  for (const pathname of ['/missing', '/index.html/extra', '/.openai/hosting.json', '/_headers', '/.local/lights/wiz-home.json',
    '/src/security/tests/interop-fixtures.json', '/src/security/tests/browser-check.html', '/tests/app.test.mjs',
    '/scripts/build.mjs', '/worker/handler.mjs', '/package.json', '/README.md', '//src/app.mjs']) {
    const response = await worker.fetch(request(pathname)); secure(response);
    assert.equal(response.status, 404, pathname); assert.equal(await response.text(), 'Not found\n');
  }
  const head = await worker.fetch(request('/missing', {method: 'HEAD'})); secure(head);
  assert.equal(head.status, 404); assert.equal(await head.text(), ''); assert.equal(head.headers.get('Content-Length'), '10');
});

test('malformed escapes, encoded separators and control characters cannot change asset routing', async () => {
  for (const pathname of ['/%', '/%zz', '/%E0%A4%A', '/src%2fapp.mjs', '/src%5capp.mjs', '/index.html%00', '/index.html%0a', '/index.html%7f']) {
    const response = await worker.fetch(request(pathname)); secure(response);
    assert.equal(response.status, 400, pathname); assert.equal(await response.text(), 'Invalid path\n');
  }
  for (const pathname of ['/../.openai/hosting.json', '/%2e%2e/.openai/hosting.json', '/%252e%252e/.openai/hosting.json'])
    assert.equal((await worker.fetch(request(pathname))).status, 404, pathname);
  assert.equal((await worker.fetch(request('/%69ndex.html'))).status, 200);
});

test('conditional and range headers cannot produce cached or partial responses without the security policy', async () => {
  const response = await worker.fetch(request('/src/app.mjs', {headers: {'If-None-Match': '*', 'If-Modified-Since': 'Wed, 01 Jan 2025 00:00:00 GMT', Range: 'bytes=0-10'}}));
  secure(response); assert.equal(response.status, 200);
  assert.equal(await response.text(), await readFile(path.join(root, 'src/app.mjs'), 'utf8'));
});

test('Worker packaging preserves the exact Site identity and excludes static bindings and source fixtures', async () => {
  const source = JSON.parse(await readFile(path.join(root, '.openai/hosting.json'), 'utf8'));
  const packaged = JSON.parse(await readFile(path.join(destination, '.openai/hosting.json'), 'utf8'));
  assert.equal(source.project_id, 'appgprj_6afa17238cd48191b9a4b2e5fa536b8a'); assert.deepEqual(packaged, source);
  assert.equal(Object.hasOwn(packaged, 'static'), false); assert.equal(result.projectID, source.project_id);
  assert.deepEqual((await readdir(destination)).sort(), ['.openai', 'server']);
  assert.deepEqual(await readdir(path.join(destination, 'server')), ['index.js']);
  assert.equal(result.files.some(name => /tests|fixtures|\.openai|README|package|_headers/.test(name)), false);
  assert.doesNotMatch(moduleSource, /from ['"]node:|require\(/);
});

test('identical source produces byte-identical Worker and hosting artifacts', async () => {
  const second = path.join(temporary, 'second'); await buildSite(second);
  assert.equal(await readFile(path.join(second, 'server/index.js'), 'utf8'), moduleSource);
  assert.equal(await readFile(path.join(second, '.openai/hosting.json'), 'utf8'), await readFile(path.join(destination, '.openai/hosting.json'), 'utf8'));
});
