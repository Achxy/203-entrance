import test from 'node:test';
import assert from 'node:assert/strict';
import {readFile, readdir} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {FEATURES, nameValid, decimal, maskFor, indices, readRoom, readLight, readReceipt,
  commonCapabilities, validatePatch, mixedValue, restriction, makeCommand, receiptText, escapeHTML, parseRGB, hexRGB} from '../src/model.mjs';
import {GatewayClient} from '../src/gateway.mjs';
import {PreviewClient} from '../src/preview.mjs';
import {ROOM_DESCRIPTOR} from '../src/config.mjs';
import {extractWiZHomeKey} from '../src/wiz-import.mjs';

const baseRoom = {v: 1, ok: true, code: 'ok', boot: 'a'.repeat(16), now_ms: '9007199254740993',
  policy_epoch: '9007199254740994', guest_epoch: '7', last_sequence: '9007199254740995', enabled: 5, frozen: 0, owner_held: 0, storage_healthy: true};
const color = {features: 63, scenes: [1, 3, 7], brightness_min: 10, kelvin_min: 2200, kelvin_max: 6500};
function light(index = 0, overrides = {}) {return {v: 1, ok: true, code: 'ok', index, name: index ? 'Ceiling' : 'Desk', mac: '000000000001', revision: 3,
  observed_valid: true, observed_ms: '9007199254740993', pending: false, reported: {power: true, brightness: 50, kelvin: 4000}, capabilities: structuredClone(color), ...overrides};}
function harness(options = {}) {
  let clock = 1000, poll = 0; const calls = [], room = structuredClone(baseRoom), lights = [light(0), light(2)];
  const session = {role: options.role ?? 'owner', closed: false, close() {this.closed = true;}, async request(request) {
    calls.push(structuredClone(request)); clock += 10;
    if (options.loss === request.op) throw Error('Transport lost');
    if (options.reject === request.op) return {v: 1, ok: false, code: options.code ?? 'stale_revision'};
    if (request.op === 'room') return structuredClone(room);
    if (request.op === 'light') {
      const current = lights.find(item => item.index === request.index);
      if (options.newRevision) current.revision = 9;
      return structuredClone(current);
    }
    if (request.op === 'set') {session.command = request; room.last_sequence = request.sequence; return {v: 1, ok: true, code: 'ok', id: request.id, result: 'queued', sent: 0, verified: 0, failed: 0};}
    if (request.op === 'command') {
      poll++;
      const verified = options.partial ? 1 : session.command?.targets ?? 5, result = options.pending ? 'sent' : poll === 1 ? 'sent' : options.partial ? 'partial' : 'verified';
      if (result === 'verified' || result === 'partial') for (const current of lights) if (verified & (1 << current.index)) {Object.assign(current.reported, session.command.patch); current.revision++;}
      return {v: 1, ok: true, code: 'ok', id: options.wrongReceipt ? 'c'.repeat(32) : request.id, result, sent: 5, verified: result === 'sent' ? 0 : verified, failed: result === 'partial' ? 4 : 0};
    }
    if (request.op === 'sessions') return {v: 1, ok: true, code: 'ok', present: request.page === 0, grant: '2', name: 'Guest', revoked: false, expired: false};
    if (request.op === 'candidate') return {v: 1, ok: true, code: 'ok', present: request.index === 0, count: 1, discovering: false, mac: '000000000009', identified: true};
    return {v: 1, ok: true, code: 'ok'};
  }};
  const client = new GatewayClient(session, {pace: 450, sleep: async ms => {clock += ms;}, clock: () => clock, id: () => 'b'.repeat(32)});
  return {client, session, calls, room, lights};
}

test('room clocks and identity sequences preserve integers above JavaScript precision', () => {
  assert.equal(readRoom(baseRoom).now_ms, '9007199254740993');
  const command = makeCommand(baseRoom, [light(0), light(2)], [0, 2], {brightness: 60}, 'b'.repeat(32), 10);
  assert.equal(command.sequence, '9007199254740996'); assert.equal(command.policy_epoch, baseRoom.policy_epoch);
  assert.equal(command.expires_ms, '9007199254742503');
  for (const invalid of [1, '-1', '01', '1e3', '18446744073709551616']) assert.throws(() => decimal(invalid));
});
test('selection is a bounded explicit mask and cannot target nonexistent slots', () => {
  assert.equal(maskFor([0, 2, 7]), 133); assert.deepEqual(indices(133), [0, 2, 7]);
  for (const selected of [[], [0, 0], [8], [-1], [0.5]]) assert.throws(() => maskFor(selected));
  assert.throws(() => makeCommand(baseRoom, [light()], [1], {power: false}, 'b'.repeat(32)));
});
test('group capabilities use intersection, including supported scene and temperature ranges', () => {
  const white = light(2, {capabilities: {features: 11, scenes: [], brightness_min: 20, kelvin_min: 2700, kelvin_max: 5000}});
  const caps = commonCapabilities([light(), white]);
  assert.equal(caps.features, 11); assert.equal(caps.brightness_min, 20); assert.equal(caps.kelvin_min, 2700); assert.equal(caps.kelvin_max, 5000); assert.deepEqual(caps.scenes, []);
  assert.throws(() => validatePatch({rgb: [1, 2, 3]}, [light(), white]));
  assert.throws(() => validatePatch({kelvin: 6500}, [light(), white]));
  assert.deepEqual(validatePatch({brightness: 20}, [light(), white]), {brightness: 20});
});
test('patch grammar refuses brightness zero, unsupported scenes and ambiguous modes', () => {
  for (const patch of [{brightness: 0}, {kelvin: 1200}, {scene: 999}, {rgb: [1, 2, 300]}, {speed: 9}, {power: 1}, {url: 'http://example.com'}, {rgb: [1, 2, 3], kelvin: 4000}, {}]) assert.throws(() => validatePatch(patch, [light()]));
  assert.deepEqual(validatePatch({power: false}, [light()]), {power: false});
});
test('mixed and unavailable observations never become a false common state', () => {
  assert.equal(mixedValue([light(), light(2, {reported: {brightness: 10}})], 'brightness').kind, 'mixed');
  assert.equal(mixedValue([light(), light(2, {observed_valid: false})], 'brightness').kind, 'unknown');
  assert.deepEqual(mixedValue([light(), light(2)], 'brightness'), {kind: 'same', value: 50});
});
test('guest policy applies only to selected targets while owner maintenance applies to everyone', () => {
  assert.match(restriction({...baseRoom, frozen: 1}, 'guest', [0]), /paused/);
  assert.equal(restriction({...baseRoom, frozen: 1}, 'guest', [2]), '');
  assert.equal(restriction({...baseRoom, frozen: 1}, 'owner', [0]), '');
  assert.match(restriction({...baseRoom, owner_held: 4}, 'guest', [2]), /keeping/);
  assert.match(restriction({...baseRoom, storage_healthy: false}, 'owner', [0]), /maintenance/);
});
test('names are UTF-8 bounded and user labels are HTML escaped', () => {
  assert.equal(nameValid('A guest'), true); assert.equal(nameValid('猫'.repeat(10)), true);
  assert.equal(nameValid('猫'.repeat(11)), false); assert.equal(nameValid('  '), false); assert.equal(nameValid('x\n'), false);
  assert.equal(escapeHTML('<img src=x onerror="bad">'), '&lt;img src=x onerror=&quot;bad&quot;&gt;');
});
test('invalid gateway pages and receipts fail closed rather than inventing state', () => {
  assert.throws(() => readRoom({...baseRoom, boot: 'x'})); assert.throws(() => readRoom({...baseRoom, enabled: 256}));
  assert.throws(() => readLight(light(0, {revision: -1}))); assert.throws(() => readLight(light(0, {reported: {rgb: [300, 0, 0]}})));
  assert.throws(() => readReceipt({v: 1, ok: true, id: 'a', result: 'verified', sent: 1, verified: 1, failed: 1}, 'a'));
});
test('new commands are bounded by fresh gateway time and exact observed revisions', () => {
  const command = makeCommand(baseRoom, [light(), light(2, {revision: 8})], [0, 2], {power: false}, 'b'.repeat(32), 999, 30000);
  assert.deepEqual(command.expected, [3, 0, 8, 0, 0, 0, 0, 0]); assert.equal(command.hold_ms, 30000);
  assert.throws(() => makeCommand(baseRoom, [light()], [0], {power: false}, 'b'.repeat(32), 1001));
  assert.throws(() => makeCommand(baseRoom, [light()], [0], {power: false}, 'b'.repeat(32), 0, 60001));
});
test('color input remains bounded and scenes describe reported outcomes honestly', () => {
  assert.deepEqual(parseRGB('#ffb45f'), [255, 180, 95]); assert.equal(hexRGB([255, 180, 95]), '#ffb45f');
  assert.throws(() => parseRGB('red'));
  assert.match(receiptText({result: 'preparing'}), /before sending/);
  assert.match(receiptText({result: 'queued'}), /Waiting/);
  assert.match(receiptText({result: 'partial', verified: 1, failed: 4}, [light(), light(2)]), /Desk updated. Ceiling did not confirm/);
});
test('paged refresh fetches only enabled lights', async () => {
  const {client, calls} = harness(); await client.refresh();
  assert.deepEqual(calls.map(call => [call.op, call.index]), [['room', undefined], ['light', 0], ['light', 2]]);
  assert.equal(client.lights.length, 2);
});
test('writing reads revisions then a final room clock immediately before sending', async () => {
  const {client, calls} = harness({newRevision: true}); await client.change([0, 2], {brightness: 65});
  const setIndex = calls.findIndex(call => call.op === 'set');
  assert.deepEqual(calls.slice(0, setIndex).map(call => call.op), ['room', 'light', 'light', 'room']);
  assert.equal(calls[setIndex].expected[0], 9); assert.equal(calls[setIndex].expected[2], 9);
  assert.equal(calls[setIndex - 1].op, 'room'); assert.equal(calls.filter(call => call.op === 'set').length, 1);
});
test('accepted and sent receipts do not optimistically overwrite observed light state', async () => {
  const {client} = harness(), observed = [];
  await client.change([0], {brightness: 75}, {onReceipt: receipt => {observed.push([receipt.result, client.lights[0].reported.brightness]);}});
  assert.deepEqual(observed.slice(0, 2), [['queued', 50], ['sent', 50]]);
  assert.equal(client.lights[0].reported.brightness, 75);
});
test('partial change retains failed target observations and carries target-specific evidence', async () => {
  const {client} = harness({partial: true}); const receipt = await client.change([0, 2], {brightness: 75});
  assert.equal(receipt.result, 'partial'); assert.equal(receipt.verified, 1); assert.equal(receipt.failed, 4);
  assert.equal(client.lights.find(item => item.index === 0).reported.brightness, 75);
  assert.equal(client.lights.find(item => item.index === 2).reported.brightness, 50);
});
test('stale revisions receive one authenticated rejection and are never automatically replayed', async () => {
  const {client, calls} = harness({reject: 'set'});
  await assert.rejects(client.change([0], {brightness: 80}), error => error.application && error.code === 'stale_revision');
  assert.equal(calls.filter(call => call.op === 'set').length, 1); assert.equal(client.closed, false);
});
test('lost command replies preserve the original ID and stop all subsequent network requests', async () => {
  const {client, session, calls} = harness({loss: 'set'});
  await assert.rejects(client.change([0], {brightness: 80}), /Transport lost/);
  const count = calls.length; assert.equal(client.lastCommand, 'b'.repeat(32)); assert.equal(session.closed, true);
  await assert.rejects(client.refresh(), /closed/); assert.equal(calls.length, count);
});
test('revoked guest grants are never silently enrolled again', async () => {
  const {client, calls} = harness({role: 'guest', reject: 'room', code: 'revoked'});
  await assert.rejects(client.refresh(), error => error.code === 'revoked');
  assert.equal(calls.length, 1); assert.equal(calls.some(call => call.op.startsWith('guest.')), false);
});
test('guest owner actions fail before producing a request', async () => {
  const {client, calls} = harness({role: 'guest'});
  await assert.rejects(client.owner('revoke_all'), error => error.code === 'unauthorized');
  await assert.rejects(client.guestPages(), error => error.code === 'unauthorized'); assert.equal(calls.length, 0);
});
test('freeze and maintenance are rechecked against current server policy before a write', async () => {
  const frozen = harness({role: 'guest'}); frozen.room.frozen = 1;
  await assert.rejects(frozen.client.change([0], {brightness: 80}), error => error.code === 'frozen');
  assert.equal(frozen.calls.some(call => call.op === 'set'), false);
  const broken = harness(); broken.room.storage_healthy = false;
  await assert.rejects(broken.client.change([0], {brightness: 80}), error => error.code === 'storage_maintenance');
});
test('malformed or cross-command receipts close the session', async () => {
  const {client} = harness({wrongReceipt: true}); await assert.rejects(client.change([0], {brightness: 80}), /invalid change receipt/);
  assert.equal(client.closed, true);
});
test('pending receipt polling is finite and does not reissue an intent', async () => {
  const {client, calls} = harness({pending: true}); const receipt = await client.change([0], {brightness: 80});
  assert.equal(receipt.result, 'sent'); assert.equal(calls.filter(call => call.op === 'command').length, 5);
  assert.equal(calls.filter(call => call.op === 'set').length, 1); assert.equal(client.lights[0].reported.brightness, 50);
});
test('owner session pages are bounded and discovery never accepts an arbitrary network address', async () => {
  const {client, calls} = harness(); assert.equal((await client.guestPages()).length, 1);
  assert.equal(calls.filter(call => call.op === 'sessions').length, 16);
  assert.equal((await client.discoverPages()).length, 1);
  assert.equal(calls.some(call => 'url' in call || 'ip' in call || 'port' in call), false);
});
test('only one high-level operation can own the encrypted request pipe', async () => {
  let release; const first = new Promise(resolve => {release = resolve;});
  const session = {role: 'owner', close() {}, request: async () => {await first; return {...baseRoom, enabled: 0};}};
  const client = new GatewayClient(session, {pace: 0, sleep: async () => {}}), refresh = client.refresh();
  await assert.rejects(client.change([0], {power: false}), error => error.code === 'busy'); release(); await refresh;
});
test('logout while a paced request waits suppresses the request itself', async () => {
  let release; let requests = 0; const sleep = () => new Promise(resolve => {release = resolve;});
  const client = new GatewayClient({role: 'guest', close() {}, request() {requests++;}}, {sleep});
  const result = client.refresh(); await Promise.resolve(); client.close(); release();
  await assert.rejects(result, /closed/); assert.equal(requests, 0);
});
test('background polling reads one light per interval rather than repeatedly paging the whole fleet', async () => {
  const {client, calls} = harness(); await client.poll(); await client.poll();
  assert.deepEqual(calls.map(call => [call.op, call.index]), [['room', undefined], ['light', 0], ['room', undefined], ['light', 2]]);
});
test('preview is explicit, network-free and preserves partial/offline/guest behavior', async () => {
  const preview = new PreviewClient('owner', 'partial'), receipt = await preview.change([0, 2], {brightness: 80});
  assert.equal(preview.preview, true); assert.equal(receipt.result, 'partial'); assert.equal(preview.lights[2].reported.brightness, 60);
  await assert.rejects(new PreviewClient('owner', 'offline').refresh(), /offline/);
  await assert.rejects(new PreviewClient('guest').owner('revoke_all'), /Owner access/);
});
test('static app has no third-party scripts, inline executable content or credential persistence', async () => {
  const root = new URL('..', import.meta.url), html = await readFile(new URL('index.html', root), 'utf8');
  assert.doesNotMatch(html, /<script(?![^>]*\bsrc=)|\son\w+=|\sstyle=/i);
  const app = await readFile(new URL('src/app.mjs', root), 'utf8');
  assert.doesNotMatch(app, /localStorage|sessionStorage|indexedDB|document\.cookie|sendBeacon/);
  assert.match(app, /query\.get\('preview'\) === '1'/); assert.match(app, /state\.auth = null/);
  assert.match(html, /Content-Security-Policy/); assert.match(html, /<dialog/);
});
test('WiZ export import reads only one bounded local key and rejects duplicate or multiple-home ambiguity', () => {
  const key = 'a'.repeat(32);
  assert.equal(extractWiZHomeKey(JSON.stringify({home: {udp_signing_key: key, wifi_password: 'ignored'}, url: 'https://never-fetched.example'})), key);
  assert.throws(() => extractWiZHomeKey(`{"udp_signing_key":"${key}","udp_signing_key":"${'b'.repeat(32)}"}`));
  assert.throws(() => extractWiZHomeKey(JSON.stringify({homes: [{udp_signing_key: key}, {udp_signing_key: key}]})));
  assert.throws(() => extractWiZHomeKey(JSON.stringify({udp_signing_key: 'bad'})));
  assert.throws(() => extractWiZHomeKey('https://wiz.example/invitation'));
  assert.throws(() => extractWiZHomeKey(JSON.stringify({udp_signing_key: key, padding: 'x'.repeat(65536)})));
});
test('public pin matches the production relay public identity and contains no private material', async () => {
  const {createPublicKey} = await import('node:crypto');
  const key = createPublicKey({key: Buffer.from(ROOM_DESCRIPTOR.public_key_spki, 'hex'), type: 'spki', format: 'der'});
  assert.equal(key.asymmetricKeyType, 'ec'); assert.equal(key.asymmetricKeyDetails.namedCurve, 'prime256v1');
  // In the monorepo, additionally verify the independently deployed relay pin.
  // A standalone Site source checkout intentionally has no sibling relay.
  const source = await readFile(fileURLToPath(new URL('../../../relay/wrangler.jsonc', import.meta.url)), 'utf8').catch(error => {if (error.code === 'ENOENT') return null; throw error;});
  if (source) {
    const config = JSON.parse(source), hex = createPublicKey(config.vars.DEVICE_PUBLIC_KEY).export({type: 'spki', format: 'der'}).toString('hex');
    assert.equal(ROOM_DESCRIPTOR.public_key_spki, hex); assert.equal(ROOM_DESCRIPTOR.gateway_id, config.vars.DEVICE_ID);
  }
  assert.deepEqual(Object.keys(ROOM_DESCRIPTOR).sort(), ['gateway_id', 'patch', 'public_key_spki', 'relay_url', 'security', 'v']);
});
