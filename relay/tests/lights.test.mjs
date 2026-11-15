import test from 'node:test';
import assert from 'node:assert/strict';
import {DatabaseSync} from 'node:sqlite';
import {generateKeyPairSync, sign} from 'node:crypto';
import worker, {ArcadeRoom} from '../src/worker.mjs';
import {authenticationMessage} from '../src/protocol.mjs';
import {lightPayload, lightRequest, lightReply, lightForward, LIGHTS_MAX_WIRE} from '../src/lights-protocol.mjs';

const keys = generateKeyPairSync('ec', {namedCurve: 'prime256v1'});
const pem = keys.publicKey.export({type: 'spki', format: 'pem'});
const baseEnv = {DEVICE_ID: 'test-gateway', DEVICE_PUBLIC_KEY: pem,
  LIGHTS_ENABLED: 'true', ALLOWED_ORIGINS: 'https://room.example'};
const boot = 'a'.repeat(16);
class Socket {
  constructor(a) {this.a = a; this.sent = []; this.readyState = 1;}
  serializeAttachment(a) {this.a = structuredClone(a);}
  deserializeAttachment() {return structuredClone(this.a);}
  send(message) {if (this.readyState !== 1) throw Error('closed'); this.sent.push(JSON.parse(message));}
  close(code, reason) {this.readyState = 3; this.reason = reason; this.code = code;}
}
function room(t, overrides = {}) {
  const sockets = [], db = new DatabaseSync(':memory:'); let alarm = null, serial = 1;
  t.after(() => db.close());
  const ctx = {getWebSockets: () => sockets.filter(w => w.readyState === 1),
    acceptWebSocket: ws => sockets.push(ws), storage: {
      sql: {exec: (q, ...args) => {const stmt = db.prepare(q); return stmt.columns().length ? stmt.all(...args) : (stmt.run(...args), []);}},
      transactionSync: fn => fn(), getAlarm: async () => alarm, setAlarm: async value => {alarm = value;}}};
  ctx.storage.transaction = fn => fn(ctx.storage);
  const env = {...baseEnv, ...overrides}, r = new ArcadeRoom(ctx, env);
  const add = kind => {
    const time = Date.now();
    const a = {kind, id: (serial++).toString(16).padStart(32, '0'), peer: 1, created: time, last: time,
      until: time + 1800000, rateAt: time, count: 0, lastId: 0, boot};
    if (kind === 'device') Object.assign(a, {lease: 5000, lightProtocol: 1});
    if (kind === 'light') Object.assign(a, {role: 'none', authUntil: time + 10000, pending: null});
    if (kind === 'phone') Object.assign(a, {pending: [], joined: true, token: 'd'.repeat(32)});
    const ws = new Socket(a); sockets.push(ws); return ws;
  };
  return {r, sockets, env, add, db, getAlarm: () => alarm};
}
function upgrades(t) {
  const NativeResponse = globalThis.Response;
  t.mock.method(globalThis, 'Response', class extends NativeResponse {
    constructor(body, options = {}) {
      super(body, {...options, status: options.status === 101 ? 200 : options.status});
      if (options.status === 101) Object.defineProperty(this, 'status', {value: 101});
      if (options.webSocket) this.webSocket = options.webSocket;
    }
  });
  const old = Object.getOwnPropertyDescriptor(globalThis, 'WebSocketPair');
  globalThis.WebSocketPair = class {constructor() {this[0] = new Socket(); this[1] = new Socket();}};
  t.after(() => old ? Object.defineProperty(globalThis, 'WebSocketPair', old) : delete globalThis.WebSocketPair);
}
const request = (id, op = 'session', extra = {}) => JSON.stringify({v: 1, id, op, payload: op === 'close' ? '' : 'AQID', ...extra});
const reply = (client, id, extra = {}) => JSON.stringify({v: 1, type: 'light_reply', connection: client.a.id, id, boot, status: 200, payload: 'BAUG', ...extra});
const game = id => JSON.stringify({v: 1, type: 'request', id, op: 'status', body: '{}', token: '', lease: 5000});
const pulse = (b, extra = {}) => JSON.stringify({v: 1, type: 'pulse', lease: b.a.lease, ...extra});
const forwarded = b => b.sent.filter(m => m.type === 'light_request');

test('lighting control stays unavailable unless the flag is exactly the string true', async () => {
  let forwarded = 0;
  for (const value of [undefined, false, true, 'false', '1', 'TRUE']) {
    const env = {...baseEnv, LIGHTS_ENABLED: value,
      ROOM: {idFromName: () => 1, get: () => ({fetch: () => {forwarded++; return new Response('ok');}})}};
    assert.equal((await worker.fetch(new Request('https://relay.example/v1/lights'), env)).status, 404);
    assert.equal((await worker.fetch(new Request('https://relay.example/v1/lights', {headers: {Upgrade: 'websocket'}}), env)).status, 404);
  }
  assert.equal(forwarded, 0);
});

test('disabled public readiness returns exact offline descriptor without accessing a Durable Object', async () => {
  for (const value of [undefined, false, true, 'false', '1', 'TRUE']) {
    const env = {...baseEnv, LIGHTS_ENABLED: value, PRIVATE_TEST_VALUE: 'must-not-appear',
      get ROOM() {throw Error('Disabled readiness must not allocate a room');}};
    for (const origin of [undefined, 'https://room.example']) {
      const req = new Request('https://relay.example/v1/lights/info', {headers: origin ? {Origin: origin} : {}});
      const response = await worker.fetch(req, env);
      assert.equal(response.status, 200);
      assert.deepEqual(await response.json(), {protocol: 1, online: false, gateway_id: 'test-gateway', public_key: pem});
      assert.equal(response.headers.get('Cache-Control'), 'no-store');
      assert.equal(response.headers.get('Vary'), 'Origin');
      assert.equal(response.headers.get('Access-Control-Allow-Origin'), origin ?? null);
    }
  }
});

test('disabled readiness still enforces strict supplied Origin and GET without query parameters', async () => {
  const env = {...baseEnv, LIGHTS_ENABLED: 'false', get ROOM() {throw Error('No room expected');}};
  for (const origin of ['', 'null', 'https://evil.example', 'https://room.example.evil', 'https://room.example/']) {
    const response = await worker.fetch(new Request('https://relay.example/v1/lights/info', {headers: {Origin: origin}}), env);
    assert.equal(response.status, 403); assert.equal(response.headers.has('Access-Control-Allow-Origin'), false);
  }
  const headers = {Origin: 'https://room.example'};
  assert.equal((await worker.fetch(new Request('https://relay.example/v1/lights/info?x=1', {headers}), env)).status, 400);
  for (const method of ['POST', 'PUT', 'DELETE', 'OPTIONS', 'HEAD'])
    assert.equal((await worker.fetch(new Request('https://relay.example/v1/lights/info', {headers, method}), env)).status, 400);
  assert.equal((await worker.fetch(new Request('https://relay.example/v1/lights/info/extra', {headers}), env)).status, 404);
});

test('native origins are optional only on lighting routes and provided origins remain strict', async () => {
  let forwarded = 0;
  const env = {...baseEnv, ROOM: {idFromName: () => 1, get: () => ({fetch: () => {forwarded++; return new Response('ok');}})}};
  for (const path of ['/v1/lights', '/v1/lights/info']) {
    const headers = {Upgrade: 'websocket'};
    assert.equal((await worker.fetch(new Request('https://relay.example' + path, {headers}), env)).status, 200);
    for (const origin of ['null', '', 'https://evil.example', 'https://room.example.evil', 'https://room.example/'])
      assert.equal((await worker.fetch(new Request('https://relay.example' + path, {headers: {...headers, Origin: origin}}), env)).status, 403);
    assert.equal((await worker.fetch(new Request('https://relay.example' + path, {headers: {...headers, Origin: 'https://room.example'}}), env)).status, 200);
    assert.equal((await worker.fetch(new Request('https://relay.example' + path + '?x=1', {headers}), env)).status, 400);
    assert.equal((await worker.fetch(new Request('https://relay.example' + path, {method: 'POST', headers}), env)).status, 400);
  }
  assert.equal((await worker.fetch(new Request('https://relay.example/v1/player', {headers: {Upgrade: 'websocket'}}), env)).status, 403);
  assert.equal((await worker.fetch(new Request('https://relay.example/v1/lights'), env)).status, 426);
  assert.equal(forwarded, 4);
  env.ALLOWED_ORIGINS = ', https://room.example, ';
  assert.equal((await worker.fetch(new Request('https://relay.example/v1/lights/info', {headers: {Origin: ''}}), env)).status, 403);
});

test('public lighting descriptor discloses only public identity and actual lighting availability', async t => {
  const {r, add} = room(t, {PRIVATE_TEST_VALUE: 'must-not-appear'}), b = add('device');
  const req = new Request('https://relay.example/v1/lights/info');
  let response = await r.fetch(req);
  assert.deepEqual(await response.json(), {protocol: 1, online: true, gateway_id: 'test-gateway', public_key: pem});
  assert.equal(response.headers.get('Cache-Control'), 'no-store');
  assert.equal(response.headers.has('Access-Control-Allow-Origin'), false);
  delete b.a.lightProtocol;
  assert.equal((await (await r.fetch(req)).json()).online, false);
  b.a.lightProtocol = 1; b.a.last = Date.now() - 1600;
  assert.equal((await (await r.fetch(req)).json()).online, false);
  response = await r.fetch(new Request(req, {headers: {Origin: 'https://room.example'}}));
  assert.equal(response.headers.get('Access-Control-Allow-Origin'), 'https://room.example');
});

test('disabled durable object readiness cannot imply online or create lighting admission storage', async t => {
  const {r, db, add, sockets} = room(t, {LIGHTS_ENABLED: 'false'}); add('device');
  assert.equal((await r.fetch(new Request('https://relay.example/v1/lights'))).status, 404);
  const response = await r.fetch(new Request('https://relay.example/v1/lights/info'));
  assert.deepEqual(await response.json(), {protocol: 1, online: false, gateway_id: 'test-gateway', public_key: pem});
  assert.equal(sockets.length, 1);
  assert.equal(db.prepare("SELECT name FROM sqlite_master WHERE name='light_limits'").all().length, 0);
  assert.equal(db.prepare('SELECT count(*) n FROM relay_limits').get().n, 0);
});

test('lighting admission requires capability and sends a boot-bound unauthenticated online envelope', async t => {
  upgrades(t);
  const {r, add} = room(t), b = add('device'); delete b.a.lightProtocol;
  assert.equal((await r.lights.connect(1)).status, 503);
  await r.webSocketMessage(b, pulse(b, {light_protocol: 1}));
  assert.equal((await r.lights.connect(1)).status, 101);
  const client = r.lights.clients()[0];
  assert.deepEqual(client.sent[0], {v: 1, type: 'light_online', online: true,
    connection: client.a.id, boot, protocol: 1});
  assert.equal(client.a.role, 'none');
});

test('lighting socket and unauthenticated handshake caps bound concurrent sessions', async t => {
  upgrades(t);
  const {r, add} = room(t); add('device');
  for (let i = 0; i < 2; i++) assert.equal((await r.lights.connect(i)).status, 101);
  assert.equal((await r.lights.connect(3)).status, 429);
  r.lights.clients().forEach(ws => {ws.a.role = 'guest';});
  for (let i = 2; i < 4; i++) assert.equal((await r.lights.connect(i)).status, 101);
  assert.equal((await r.lights.connect(5)).status, 429);
});

test('lighting admission counters cannot evict or consume arcade and device budgets', t => {
  const {r, db} = room(t);
  for (let i = 0; i < 8; i++) assert.equal(r.lights.rate('peer:connect:1', 8, 60000), true);
  assert.equal(r.lights.rate('peer:connect:1', 8, 60000), false);
  for (let i = 0; i < 12; i++) {
    assert.equal(r.rate('device:1', 12, 60000), true);
    assert.equal(r.rate('connect:1', 12, 60000), true);
  }
  r.lights.rate('connect', 24, 60000);
  for (let i = 0; i < 200; i++) r.lights.rate('peer:auth:' + i, 6, 10000);
  assert.equal(db.prepare("SELECT count(*) n FROM light_limits WHERE k LIKE 'peer:%'").get().n, 128);
  assert.equal(r.rate('device:1', 12, 60000), false);
  assert.equal(db.prepare("SELECT n FROM light_limits WHERE k='connect'").get().n, 1);
  const wake = new ArcadeRoom(r.ctx, baseEnv);
  for (let i = 1; i < 24; i++) assert.equal(wake.lights.rate('connect', 24, 60000), true);
  assert.equal(wake.lights.rate('connect', 24, 60000), false);
});

test('lighting request grammar rejects unknown fields, ambiguous JSON and noncanonical base64', () => {
  for (const value of ['A', 'AA', 'AB==', 'AAB=', 'AA-_', 'AA==\n', 'AA== ', '====', 'éAAA', btoa('x'.repeat(1025))])
    assert.throws(() => lightPayload(value), value);
  assert.equal(lightPayload(btoa('x'.repeat(1024))).length, 1368);
  assert.equal(lightPayload(''), '');
  for (const text of [request(0), request(0x100000000), request(1, 'http'), request(1, 'session', {payload: ''}),
    request(1, 'close', {payload: 'AQID'}), request(1, 'session', {role: 'owner'}),
    request(1, 'session', {role_hint: 'admin'}), request(1, 'session', {url: 'https://example.com'}),
    '{"v":1,"id":1,"id":2,"op":"session","payload":"AQID"}',
    request(1, 'session', {payload: {secret: true}})]) assert.throws(() => lightRequest(text));
  assert.equal(lightRequest(request(1)).role_hint, 'unknown');
  assert.equal(lightRequest(request(1, 'session', {role_hint: 'owner'})).role_hint, 'owner');
});

test('maximum-size records and routing metadata fit the ESP 2048-byte input budget', () => {
  const m = lightRequest(request(0xffffffff, 'data', {payload: btoa('x'.repeat(1024)), role_hint: 'unknown'}));
  const wire = lightForward(m, 'f'.repeat(32), boot, Number.MAX_SAFE_INTEGER, 10000);
  assert.ok(new TextEncoder().encode(JSON.stringify(wire)).length <= LIGHTS_MAX_WIRE);
  assert.equal(wire.lease, Number.MAX_SAFE_INTEGER);
  assert.equal(wire.ttl_ms, 10000);
});

test('binary, oversized and malformed native frames close only the offending lighting client', async t => {
  const {r, add} = room(t), b = add('device'), player = add('phone');
  for (const value of [new TextEncoder().encode(request(1)).buffer, request(1) + ' '.repeat(2048), '{"v":1,}']) {
    const c = add('light');
    await r.webSocketMessage(c, value);
    assert.equal(c.readyState, 3); assert.equal(c.a.pending, null);
  }
  assert.equal(forwarded(b).length, 0); assert.equal(b.readyState, 1); assert.equal(player.readyState, 1);
  assert.equal(b.sent.filter(m => m.type === 'light_disconnect').length, 3);
});

test('untrusted role hints never authenticate data or take the owner pending credit', async t => {
  const {r, add} = room(t), b = add('device'), guest = add('light'), guest2 = add('light'), forged = add('light'), owner = add('light');
  guest.a.role = guest2.a.role = 'guest'; owner.a.role = 'owner';
  await r.webSocketMessage(guest, request(1, 'data'));
  await r.webSocketMessage(guest2, request(1, 'data'));
  await r.webSocketMessage(forged, request(1, 'session', {role_hint: 'owner'}));
  assert.equal(forged.sent.at(-1).error, 'busy'); assert.equal(forged.readyState, 3);
  await r.webSocketMessage(owner, request(1, 'data'));
  assert.equal(forwarded(b).length, 3);
  const secondOwner = add('light'); secondOwner.a.role = 'owner';
  await r.webSocketMessage(secondOwner, request(1, 'data'));
  assert.equal(secondOwner.sent.at(-1).error, 'busy'); assert.equal(forwarded(b).length, 3);
  const other = add('light');
  await r.webSocketMessage(other, request(1, 'data', {role_hint: 'owner'}));
  assert.equal(other.sent.at(-1).error, 'authentication_required');
  assert.equal(other.a.role, 'none');
});

test('lighting can never use arcade pending credits and arcade can still use all four credits', async t => {
  const {r, add} = room(t), b = add('device'), owner = add('light'), guest = add('light'), guest2 = add('light'), p = add('phone'), q = add('phone');
  owner.a.role = 'owner'; guest.a.role = guest2.a.role = 'guest';
  await r.webSocketMessage(owner, request(1, 'data'));
  await r.webSocketMessage(guest, request(1, 'data'));
  await r.webSocketMessage(guest2, request(1, 'data'));
  await r.webSocketMessage(p, game(1)); await r.webSocketMessage(p, game(2));
  await r.webSocketMessage(q, game(1)); await r.webSocketMessage(q, game(2));
  assert.equal(b.sent.filter(m => m.type === 'request').length, 4);
  assert.equal(b.sent.filter(m => m.type === 'light_request').length, 3);
  assert.equal(p.a.pending.length + q.a.pending.length, 4);
});

test('exactly one inflight lighting request is allowed and replay always closes the session', async t => {
  const {r, add} = room(t), b = add('device'), c = add('light');
  await r.webSocketMessage(c, request(1));
  await r.webSocketMessage(c, request(2));
  assert.equal(forwarded(b).length, 1); assert.equal(c.sent.at(-1).error, 'in_flight'); assert.equal(c.readyState, 3);
  const d = add('light');
  await r.webSocketMessage(d, request(7)); await r.webSocketMessage(b, reply(d, 7));
  await r.webSocketMessage(d, request(7));
  assert.equal(d.sent.at(-1).error, 'replay'); assert.equal(d.readyState, 3);
});

test('only a matching current authenticated device reply grants the role and no replies cross clients', async t => {
  const {r, add} = room(t), b = add('device'), c = add('light'), other = add('light');
  await r.webSocketMessage(c, request(1));
  await r.webSocketMessage(b, reply(other, 1, {role: 'owner'}));
  await r.webSocketMessage(b, reply(c, 2, {role: 'owner'}));
  await r.webSocketMessage(b, reply(c, 1, {role: 'owner', boot: 'b'.repeat(16)}));
  assert.equal(c.a.role, 'none'); assert.equal(other.a.role, 'none'); assert.equal(c.sent.length, 0);
  await r.webSocketMessage(b, reply(c, 1, {role: 'owner'}));
  assert.equal(c.a.role, 'owner'); assert.equal(c.a.pending, null); assert.equal(c.sent.length, 1);
  await r.webSocketMessage(b, reply(c, 1, {role: 'guest'}));
  assert.equal(c.a.role, 'owner'); assert.equal(c.sent.length, 1);
  await r.webSocketMessage(c, reply(other, 1, {role: 'owner'}));
  assert.equal(c.readyState, 3); assert.equal(other.a.role, 'none');
});

test('two authenticated guests leave room for fresh owner authentication', async t => {
  const {r, add} = room(t), b = add('device'), first = add('light'), second = add('light');
  first.a.role = second.a.role = 'guest';
  const third = add('light');
  await r.webSocketMessage(third, request(1));
  await r.webSocketMessage(b, reply(third, 1, {role: 'guest'}));
  assert.equal(third.a.role, 'none'); assert.equal(third.readyState, 3);
  assert.equal(third.sent.at(-1).error, 'busy');
  assert.equal(b.sent.filter(m => m.type === 'light_disconnect' && m.connection === third.a.id).length, 1);
  const owner = add('light');
  await r.webSocketMessage(owner, request(1, 'session', {role_hint: 'owner'}));
  await r.webSocketMessage(b, reply(owner, 1, {role: 'owner'}));
  assert.equal(owner.a.role, 'owner'); assert.equal(owner.readyState, 1);
});

test('identity replies cannot authenticate and gateway role loss closes the encrypted session', async t => {
  const {r, add} = room(t), b = add('device'), c = add('light');
  await r.webSocketMessage(c, request(1, 'identity'));
  await r.webSocketMessage(b, reply(c, 1, {role: 'owner'}));
  assert.equal(c.a.role, 'none'); assert.equal(c.sent.at(-1).error, 'invalid_gateway_role');
  const d = add('light'); d.a.role = 'guest';
  await r.webSocketMessage(d, request(1, 'data'));
  await r.webSocketMessage(b, reply(d, 1, {role: 'none'}));
  assert.equal(d.readyState, 3); assert.equal(d.a.role, 'none');
  const e = add('light');
  await r.webSocketMessage(e, request(1));
  await r.webSocketMessage(b, reply(e, 1, {role: 'owner', payload: ''}));
  assert.equal(e.a.role, 'none'); assert.equal(e.sent.at(-1).error, 'invalid_gateway_payload');
});

test('transport failures never leave a reusable cipher session or pending offline queue', async t => {
  const {r, add} = room(t), b = add('device'), c = add('light'); c.a.role = 'guest';
  b.a.last = Date.now() - 1600;
  await r.webSocketMessage(c, request(1, 'data'));
  assert.equal(forwarded(b).length, 0); assert.equal(c.readyState, 3); assert.equal(c.a.pending, null);
  b.a.last = Date.now(); const d = add('light');
  await r.webSocketMessage(d, request(1));
  await r.webSocketMessage(b, reply(d, 1, {status: 401}));
  assert.equal(d.readyState, 3);
});

test('handshakes have a total ten-second budget and data expires at two seconds including after hibernation', async t => {
  let clock = 1000000; t.mock.method(Date, 'now', () => clock);
  const {r, add, getAlarm} = room(t), b = add('device'), c = add('light');
  clock += 3000; b.a.last = clock;
  await r.webSocketMessage(c, request(1));
  assert.equal(b.sent.at(-1).ttl_ms, 7000); assert.equal(getAlarm(), 1010000);
  await r.webSocketMessage(b, reply(c, 1, {role: 'owner'}));
  await r.webSocketMessage(c, request(2, 'data'));
  assert.equal(b.sent.at(-1).ttl_ms, 2000); assert.equal(b.sent.at(-1).lease, 5000);
  assert.equal(getAlarm(), clock + 2000);
  clock += 2000; b.a.last = clock;
  const wake = new ArcadeRoom(r.ctx, baseEnv);
  await wake.webSocketMessage(b, reply(c, 2));
  assert.equal(c.sent.at(-1).error, 'timeout'); assert.equal(c.readyState, 3);
  const d = add('light'); clock += 10000; b.a.last = clock; wake.prune();
  assert.equal(d.readyState, 3); assert.equal(d.sent.at(-1).type, 'light_offline');
});

test('alarm pruning expires pending ciphertext and cannot postpone an earlier deadline', async t => {
  let clock = 1000000; t.mock.method(Date, 'now', () => clock);
  const {r, add, getAlarm} = room(t), b = add('device'), c = add('light'); c.a.role = 'owner';
  await r.webSocketMessage(c, request(1, 'data'));
  await r.lights.schedule(clock + 10000);
  assert.equal(getAlarm(), clock + 2000);
  clock += 2000; b.a.last = clock;
  await r.alarm();
  assert.equal(c.readyState, 3); assert.equal(c.sent.at(-1).error, 'timeout');
  assert.equal(getAlarm(), clock + 15000);
});

test('rolling aggregate lighting rate reserves two operations each second for proven owners', async t => {
  const {r, add} = room(t), b = add('device'), guest = add('light'), owner = add('light');
  guest.a.role = 'guest'; owner.a.role = 'owner';
  for (let id = 1; id <= 6; id++) {await r.webSocketMessage(guest, request(id, 'data')); await r.webSocketMessage(b, reply(guest, id));}
  await r.webSocketMessage(guest, request(7, 'data'));
  assert.equal(guest.sent.at(-1).error, 'rate');
  for (let id = 1; id <= 2; id++) {await r.webSocketMessage(owner, request(id, 'data')); await r.webSocketMessage(b, reply(owner, id));}
  await r.webSocketMessage(owner, request(3, 'data'));
  assert.equal(owner.sent.at(-1).error, 'rate');
  assert.equal(forwarded(b).length, 8); assert.equal(b.a.count, 0);
});

test('rolling rate history survives hibernation and does not reset at a clock-second boundary', async t => {
  let clock = 1000999; t.mock.method(Date, 'now', () => clock);
  const {r, add} = room(t), b = add('device'), c = add('light'); c.a.role = 'owner';
  for (let id = 1; id <= 8; id++) {await r.webSocketMessage(c, request(id, 'data')); await r.webSocketMessage(b, reply(c, id));}
  clock += 2; b.a.last = clock;
  const wake = new ArcadeRoom(r.ctx, baseEnv);
  await wake.webSocketMessage(c, request(9, 'data'));
  assert.equal(c.sent.at(-1).error, 'rate'); assert.equal(forwarded(b).length, 8);
  clock += 998; b.a.last = clock;
  const d = add('light'); d.a.role = 'owner';
  await wake.webSocketMessage(d, request(1, 'data'));
  assert.equal(forwarded(b).length, 9);
});

test('preauthentication rate budget survives connections and hibernation', async t => {
  const {r, add} = room(t), b = add('device');
  for (let i = 0; i < 6; i++) {
    const c = add('light'); await r.webSocketMessage(c, request(1));
    await r.webSocketMessage(b, reply(c, 1)); c.close();
    b.a.lightRates = [];
  }
  const c = add('light'), wake = new ArcadeRoom(r.ctx, baseEnv);
  await wake.webSocketMessage(c, request(1));
  assert.equal(c.sent.at(-1).error, 'rate'); assert.equal(forwarded(b).length, 6);
});

test('malformed and oversized lighting replies cannot close the arcade device or its controllers', async t => {
  const {r, add} = room(t), b = add('device'), p = add('phone'), c = add('light');
  await r.webSocketMessage(c, request(1));
  await r.webSocketMessage(b, reply(c, 1, {unexpected: true}));
  assert.equal(c.readyState, 3); assert.equal(p.readyState, 1); assert.equal(b.readyState, 1);
  const d = add('light');
  await r.webSocketMessage(d, request(1));
  await r.webSocketMessage(b, reply(d, 1, {payload: btoa('x'.repeat(1025))}));
  assert.equal(d.readyState, 3); assert.equal(b.readyState, 1); assert.equal(b.a.count, 0);
  assert.throws(() => lightReply(reply(d, 1, {role: 'admin'})));
});

test('device disconnect, replacement, capability removal and reboot invalidate lighting sessions', async t => {
  const {r, add, sockets} = room(t), b = add('device'), c = add('light');
  r.webSocketClose(b); assert.equal(c.readyState, 3);
  const fresh = add('device'), d = add('light');
  await r.webSocketMessage(fresh, pulse(fresh, {light_protocol: 0}));
  assert.equal(d.readyState, 3);
  fresh.a.lightProtocol = 1; const e = add('light');
  fresh.a.boot = 'e'.repeat(16); r.prune(); assert.equal(e.readyState, 3);
  const f = add('light'); f.a.boot = fresh.a.boot;
  const pending = new Socket({kind: 'pending', id: 'f'.repeat(32), nonce: 'c'.repeat(64),
    created: Date.now(), last: Date.now(), rateAt: Date.now(), count: 0}); sockets.push(pending);
  const signature = sign('sha256', authenticationMessage(baseEnv.DEVICE_ID, pending.a.nonce, pending.a.id, boot), keys.privateKey).toString('hex');
  await r.webSocketMessage(pending, JSON.stringify({v: 1, type: 'authenticate', device: baseEnv.DEVICE_ID, boot, signature}));
  assert.equal(f.readyState, 3); assert.equal(pending.a.kind, 'device'); assert.equal(fresh.readyState, 3);
});

test('disconnect cleanup is boot-bound, payload-free and at most once per admitted connection', async t => {
  const {r, add} = room(t), b = add('device'), c = add('light'); c.a.role = 'owner';
  await r.webSocketMessage(c, request(1, 'data'));
  r.webSocketClose(c); r.webSocketError(c); r.webSocketClose(c);
  const cleanup = b.sent.filter(m => m.type === 'light_disconnect');
  assert.deepEqual(cleanup, [{v: 1, type: 'light_disconnect', connection: c.a.id, boot}]);
  assert.equal(forwarded(b).length, 1);
  const before = c.sent.length;
  await r.webSocketMessage(b, reply(c, 1));
  assert.equal(c.sent.length, before); assert.equal(b.a.count, 0);
  const stale = add('light'); stale.a.boot = 'f'.repeat(16); r.webSocketClose(stale);
  assert.equal(b.sent.filter(m => m.type === 'light_disconnect').length, 1);
  const offline = add('light'); b.close(); r.webSocketClose(offline);
  const replacement = add('device');
  r.prune(); assert.equal(replacement.sent.length, 0);
});

test('a client cannot select another connection for lifecycle cleanup', async t => {
  const {r, add} = room(t), b = add('device'), victim = add('light'), attacker = add('light');
  await r.webSocketMessage(attacker, request(1, 'close', {connection: victim.a.id}));
  assert.equal(attacker.readyState, 3); assert.equal(victim.readyState, 1);
  assert.deepEqual(b.sent, [{v: 1, type: 'light_disconnect', connection: attacker.a.id, boot}]);
});

test('close acknowledgement ends the session and established sessions cannot renegotiate in place', async t => {
  const {r, add} = room(t), b = add('device'), c = add('light'); c.a.role = 'owner';
  await r.webSocketMessage(c, request(1, 'close'));
  assert.equal(b.sent.at(-1).op, 'close');
  await r.webSocketMessage(b, reply(c, 1, {payload: ''}));
  assert.equal(c.readyState, 3);
  assert.equal(b.sent.filter(m => m.type === 'light_disconnect').length, 0);
  const d = add('light'); d.a.role = 'owner';
  await r.webSocketMessage(d, request(1));
  assert.equal(d.sent.at(-1).error, 'new_connection_required');
});

test('idle and absolute session deadlines cannot be extended indefinitely', t => {
  let clock = 1000000; t.mock.method(Date, 'now', () => clock);
  const {r, add} = room(t), b = add('device'), c = add('light'); c.a.role = 'owner';
  clock += 60000; b.a.last = clock; r.prune(); assert.equal(c.readyState, 3);
  const d = add('light'); d.a.role = 'owner';
  clock = d.a.until; b.a.last = clock; d.a.last = clock; r.prune(); assert.equal(d.readyState, 3);
});
