import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// Execute the real upload helper without DOM startup or network access.
const source = readFileSync(new URL('../web/app.js', import.meta.url), 'utf8');
const helper = source.slice(0, source.indexOf('\nfunction loginView()'));
assert.ok(helper.includes('async function writeChunk('));
let cases = 0;
function fixture(replies, start = 0) {
  let now = start;
  const calls = [], sleeps = [];
  const context = vm.createContext({
    performance: { now: () => now },
    setTimeout: (callback, delay) => { sleeps.push(delay); now += delay; callback(); },
    scriptedAPI: async (path, options) => {
      calls.push({ path, ...options });
      const reply = replies.shift();
      assert.ok(reply, 'No unexpected request after budget or terminal error');
      now += reply.consumeTimeout ? (options.timeout ?? 20000) : (reply.ms || 0);
      if (reply.error) throw reply.error;
      return reply.value || { ok: true };
    },
  });
  vm.runInContext(helper + '\napi = scriptedAPI; globalThis.upload = writeChunk;', context);
  return { calls, sleeps, context, clock: () => now, advance: ms => { now += ms; },
    write: (offset = 0, timing) => context.upload('private-ticket', offset, new Uint8Array(4), timing) };
}
const failure = (ms = 0) => ({ ms, error: new Error('connection lost') });
const state = (received, ms = 0) => ({ ms, value: { phase: 'receiving', received } });
const budget = /recovery time is exhausted/;

{
  const f = fixture([failure(8000), state(8, 3000)]), timing = { started: 0, progress: 0 };
  await f.write(4, timing);
  assert.deepEqual(f.calls.map(c => c.path), ['/ota/chunk', '/ota/status']);
  assert.deepEqual(f.calls.map(c => c.timeout), [8000, 3000]);
  assert.equal(timing.progress, 0, 'A reconciled write is anchored to its send start');
  cases++;
}
{
  const f = fixture([failure(8000), state(4, 2000), { ms: 1000 }]);
  await f.write(4);
  assert.deepEqual(f.calls.map(c => c.path), ['/ota/chunk', '/ota/status', '/ota/chunk']);
  assert.equal(f.calls[0].body, f.calls[2].body);
  assert.deepEqual(f.calls[0].extra, f.calls[2].extra);
  assert.equal(f.calls[0].ticket, f.calls[2].ticket);
  assert.deepEqual(f.sleeps, [500]);
  cases++;
}
{
  const f = fixture([failure(8000), state(0, 3000), failure(8000), state(0, 3000)]);
  await assert.rejects(f.write(), budget);
  assert.equal(f.clock(), 22500);
  assert.deepEqual(f.calls.map(c => c.method || 'GET'), ['PUT', 'GET', 'PUT', 'GET']);
  cases++;
}
{
  const f = fixture(Array.from({ length: 4 }, () => ({ consumeTimeout: true, error: new Error('timed out') })));
  await assert.rejects(f.write(), /Cannot resolve upload acknowledgement/);
  assert.equal(f.clock(), 18000, 'The old 36-second unresolved-ACK chain is eliminated');
  assert.equal(f.calls.filter(c => c.method === 'PUT').length, 1);
  cases++;
}
for (const replies of [[{ ms: 26000 }], [failure(8000), state(4, 18000)]]) {
  const f = fixture(replies);
  await assert.rejects(f.write(), budget);
  assert.equal(f.clock(), 26000);
  cases++;
}
{
  const f = fixture([{ ms: 7000 }]), timing = { started: 0, progress: 0 };
  await f.write(0, timing);
  f.advance(8000);
  await assert.rejects(f.write(4, timing), budget);
  assert.equal(f.calls.length, 1, 'Delayed previous acknowledgement cannot grant a fresh window');
  cases++;
}
{
  const f = fixture([], 285000);
  await assert.rejects(f.write(4, { started: 0, progress: 284000 }), budget);
  assert.equal(f.calls.length, 0, 'Upload-wide deadline also applies after recent progress');
  cases++;
}
for (const code of ['authentication', 'ESP_ERR_TIMEOUT']) {
  const error = Object.assign(new Error('denied'), { status: 401, code });
  const f = fixture([{ error }]);
  await assert.rejects(f.write(), /denied/);
  assert.equal(f.calls.length, 1);
  cases++;
}
for (const received of [2, 12]) {
  const f = fixture([failure(), state(received)]);
  await assert.rejects(f.write(), /Unexpected upload offset/);
  assert.equal(f.calls.length, 2);
  cases++;
}
{
  const f = fixture([failure(), { value: { phase: 'failed', received: 4, detail: 'expired' } }]);
  await assert.rejects(f.write(), /expired/);
  assert.equal(f.calls.length, 2);
  cases++;
}
console.log(`${cases} browser upload recovery scenarios passed without network access.`);
