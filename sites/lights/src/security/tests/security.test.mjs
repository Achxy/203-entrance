import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import test from 'node:test';
import {webcrypto} from 'node:crypto';
import {base64, concat, fromHex, hex, uint32, unbase64, utf8} from '../bytes.mjs';
import {parseStrictJSON} from '../strict-json.mjs';
import {derToRaw, verifyBinding} from '../identity.mjs';
import {response} from '../protobuf.mjs';
import {Security2Session} from '../security2.mjs';
import {deriveVerifier} from '../srp.mjs';
import {decodeOnline, decodeReply, RelayChannel, validateRelayURL} from '../relay-channel.mjs';
import {GatewayWebSession, deriveGuestVerifier, parseTrustedDescriptor, randomHex} from '../gateway-web-session.mjs';

const fixtures = JSON.parse(await readFile(new URL('./interop-fixtures.json', import.meta.url)));
assert.equal(fixtures.idf_revision, 'fff9895c82d744c7237be8847347bdd1b07c6643');
const normal = fixtures.vectors[0];
function provider(vector) {
  const pending = [vector.ephemeral, vector.challenge];
  return {subtle: webcrypto.subtle, getRandomValues(target) {
    assert.equal(target.length, 32); assert.ok(pending.length);
    target.set(fromHex(pending.shift(), 32)); return target;
  }};
}
const descriptor = (vector = normal) => ({v: 1, gateway_id: vector.gateway,
  public_key_spki: vector.spki, relay_url: 'wss://example.test/v1/lights', security: 2, patch: 1});
async function handshaken(vector = normal) {
  const session = new Security2Session({role: 'guest', credential: vector.password, crypto: provider(vector)});
  assert.equal(hex(await session.startHandshake()), vector.request0);
  assert.equal(hex(await session.receiveChallenge(fromHex(vector.response0))), vector.request1);
  await session.verifyServer(fromHex(vector.response1));
  assert.equal(session.state, 'binding'); assert.equal(session.transcript, vector.transcript);
  return session;
}
async function bound(vector = normal) {
  const session = await handshaken(vector);
  const request = await session.makeBinding(descriptor(vector), vector.boot, 'guest');
  const key = await webcrypto.subtle.importKey('raw', fromHex(vector.key), 'AES-GCM', false, ['decrypt']);
  const plain = await webcrypto.subtle.decrypt({name: 'AES-GCM', iv: fromHex(vector.nonce), tagLength: 128}, key, request);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(plain)), {v: 1, op: 'bind', challenge: vector.challenge});
  await session.verifyBinding(fromHex(vector.binding_ciphertext));
  assert.equal(session.role, 'guest'); assert.equal(session.state, 'request'); return session;
}

for (const vector of fixtures.vectors) {
  test(`IDF exact SRP, transcript, identity and AES counters: ${vector.name}`, async () => {
    const session = await bound(vector);
    assert.equal(hex(await session.sealRequest(fromHex(vector.plaintext_request))), vector.request_ciphertext);
    assert.equal(hex(await session.openResponse(fromHex(vector.response_ciphertext))), vector.plaintext_response);
    await session.sealRequest(fromHex(vector.plaintext_request));
    assert.equal(hex(await session.openResponse(fromHex(vector.next_response_ciphertext))), vector.plaintext_response);
    session.close(); assert.equal(session.role, undefined);
  });
}

test('native ESP-IDF salt/verifier fixture matches exactly', async () => {
  // Copied public corpus is pinned against ESP-IDF test_srp.c by check_upstream.py
  // in the monorepo. Keeping this fixture local makes the site suite standalone.
  const native = fixtures.verifier_fixture;
  assert.equal(fixtures.native_verifier_matched, true);
  assert.equal(native.username, 'wifiprov'); assert.equal(native.password, 'abcd1234');
  assert.equal(hex(await deriveVerifier(native.username, native.password, fromHex(native.salt), webcrypto)), native.verifier);
  const leading = fixtures.leading_zero_verifier_fixture;
  assert.equal(leading.username, '203-guest');
  const padded = hex(await deriveVerifier(leading.username, leading.password, fromHex(leading.salt), webcrypto));
  assert.equal(padded, leading.verifier); assert.equal(padded.length, 768);
  const inner = hex(new Uint8Array(await webcrypto.subtle.digest('SHA-512', utf8(`${leading.username}:${leading.password}`))));
  assert.equal(inner, leading.inner_digest); assert.equal(inner.length, 128); assert.ok(inner.startsWith('00'));
  const result = await deriveGuestVerifier('fixture-only-guest-password');
  assert.match(result.salt, /^[0-9a-f]{32}$/); assert.match(result.verifier, /^[0-9a-f]{768}$/);
  assert.equal(hex(await deriveVerifier('203-guest', 'fixture-only-guest-password', fromHex(result.salt), webcrypto)), result.verifier);
  const second = await deriveGuestVerifier('fixture-only-guest-password'); assert.notEqual(result.salt, second.salt);
  assert.match(randomHex(16), /^[0-9a-f]{32}$/);
  await assert.rejects(deriveGuestVerifier(''));
});

test('strict JSON rejects duplicates, escaped aliases, prototypes and numeric ambiguity', () => {
  const good = parseStrictJSON('{"lights":[{"name":"Room 猫","state":true}],"now_ms":"18446744073709551615"}');
  assert.equal(Object.getPrototypeOf(good), null);
  for (const value of ['', '{}{}', '[]', 'null', '{"a":1,"a":2}', '{"a":1,"\\u0061":2}',
    '{"a":{"b":1,"b":2}}', '{"n":1.0}', '{"n":1e0}', '{"n":01}', '{"n":-0}',
    '{"n":9007199254740992}', '{"s":"\\ud800"}', '{"a":NaN}', '{"a":true,}']) assert.throws(() => parseStrictJSON(value));
  const prototype = parseStrictJSON('{"__proto__":{"polluted":true}}');
  assert.equal(Object.getPrototypeOf(prototype), null); assert.equal({}.polluted, undefined);
  assert.throws(() => parseStrictJSON('{"a":[]}', {flat: true}));
  assert.throws(() => parseStrictJSON('{"a":{"b":1}}', {flat: true}));
  assert.throws(() => parseStrictJSON('{"\\u0061":1}', {unescaped: true}));
  assert.throws(() => parseStrictJSON(utf8('x'.repeat(1009))));
  assert.throws(() => parseStrictJSON(new Uint8Array([0xff])));
  assert.throws(() => parseStrictJSON('{"a":' + '['.repeat(9) + '0' + ']'.repeat(9) + '}'));
});

test('canonical bounded protobuf rejects alternative encodings and wrong scheme', () => {
  const packet = fromHex(normal.response0);
  assert.equal(response(packet, 0).salt.length, 18);
  for (const malformed of [concat(packet, new Uint8Array([16, 2])),
    concat(new Uint8Array([0x90, 0]), packet.slice(1)),
    concat(packet, new Uint8Array([0x68, 1])), new Uint8Array(1025)]) assert.throws(() => response(malformed, 0));
  const wrong = packet.slice(); wrong[1] = 1; assert.throws(() => response(wrong, 0));
  for (let length = 0; length < packet.length; length++) assert.throws(() => response(packet.slice(0, length), 0));
  assert.throws(() => response(fromHex(normal.response1), 0));
});

test('invalid SRP proof and zero B close the session with no fallback', async () => {
  let session = new Security2Session({role: 'guest', credential: normal.password, crypto: provider(normal)});
  await session.startHandshake(); await session.receiveChallenge(fromHex(normal.response0));
  const proof = fromHex(normal.response1); proof[15] ^= 1;
  await assert.rejects(session.verifyServer(proof)); assert.equal(session.state, 'closed');
  session = new Security2Session({role: 'guest', credential: normal.password, crypto: provider(normal)});
  await session.startHandshake();
  const bad = fromHex(normal.response0), B = response(bad, 0).publicKey;
  const start = Buffer.from(bad).indexOf(Buffer.from(B)); assert.ok(start > 0); bad.fill(0, start, start + B.length);
  await assert.rejects(session.receiveChallenge(bad)); assert.equal(session.state, 'closed');
  assert.throws(() => new Security2Session({role: 'guest', credential: normal.password, patch: 0}));
});

test('commands are impossible before identity binding; wrong boot/role/key fail', async () => {
  let session = await handshaken();
  await assert.rejects(session.sealRequest(utf8('{"v":1,"op":"room"}'))); assert.equal(session.state, 'closed');
  for (const change of ['boot', 'role', 'key']) {
    session = await handshaken();
    const d = descriptor(); if (change === 'key') d.public_key_spki = d.public_key_spki.slice(0, -2) + '10';
    await session.makeBinding(d, change === 'boot' ? '1123456789abcdef' : normal.boot, change === 'role' ? 'owner' : 'guest');
    await assert.rejects(session.verifyBinding(fromHex(normal.binding_ciphertext))); assert.equal(session.role, undefined);
  }
});

test('identity JSON and DER strictness, including raw vs DER WebCrypto conversion', async () => {
  const expected = {gateway_id: normal.gateway, public_key_spki: normal.spki,
    boot: normal.boot, transcript: normal.transcript, challenge: normal.challenge, role: 'guest'};
  const plain = fromHex(normal.binding_response);
  await verifyBinding(plain, expected, webcrypto);
  const body = new TextDecoder().decode(plain);
  for (const bad of [body.replace('"v":1', '"v":1,"v":1'), body.replace('"v":1', '"\\u0076":1'),
    body.replace('"v":1', '"v":1,"extra":true'), body.replace('"role":"guest"', '"role":"owner"')])
    await assert.rejects(verifyBinding(utf8(bad), expected, webcrypto));
  const signature = fromHex(JSON.parse(body).signature);
  assert.equal(derToRaw(signature).length, 64);
  for (const bad of [signature.slice(1), concat(signature, new Uint8Array([0])),
    new Uint8Array([48, 6, 2, 1, 0, 2, 1, 1]), new Uint8Array([48, 7, 2, 2, 0, 1, 2, 1, 1])]) assert.throws(() => derToRaw(bad));
});

test('ciphertext corruption/replay and concurrent calls permanently close', async () => {
  let session = await bound();
  await session.sealRequest(fromHex(normal.plaintext_request));
  const corrupt = fromHex(normal.response_ciphertext); corrupt[0] ^= 1;
  await assert.rejects(session.openResponse(corrupt)); assert.equal(session.state, 'closed');
  session = await bound();
  await session.sealRequest(fromHex(normal.plaintext_request)); await session.openResponse(fromHex(normal.response_ciphertext));
  await session.sealRequest(fromHex(normal.plaintext_request));
  await assert.rejects(session.openResponse(fromHex(normal.response_ciphertext)));
  session = new Security2Session({role: 'guest', credential: normal.password, crypto: provider(normal)});
  const results = await Promise.allSettled([session.startHandshake(), session.startHandshake()]);
  assert.ok(results.every(r => r.status === 'rejected')); assert.equal(session.state, 'closed');
  session = await bound(); await assert.rejects(session.sealRequest(new Uint8Array(1009))); assert.equal(session.state, 'closed');
});

const identity = {connection: '1234567890abcdef1234567890abcdef', boot: normal.boot};
const online = () => JSON.stringify({v: 1, type: 'light_online', online: true, ...identity, protocol: 1});
const reply = (id, payload, extra = {}) => JSON.stringify({v: 1, type: 'light_reply', ...identity, id, status: 200, payload: base64(payload), ...extra});
test('relay framing is bounded and pins boot, connection, ordered ID, encoding', () => {
  assert.deepEqual(decodeOnline(online()), identity);
  assert.equal(hex(decodeReply(reply(1, fromHex(normal.response0)), 1, identity, 'session')), normal.response0);
  for (const frame of [reply(2, new Uint8Array([1])), reply(1, new Uint8Array([1]), {boot: 'ffffffffffffffff'}),
    reply(1, new Uint8Array([1]), {connection: 'f'.repeat(32)}), reply(1, new Uint8Array([1]), {status: 429}),
    reply(1, new Uint8Array([1]), {payload: 'AQ'}), reply(1, new Uint8Array([1]), {payload: 'AR=='}),
    reply(1, new Uint8Array([1]), {extra: 'bad'})]) assert.throws(() => decodeReply(frame, 1, identity, 'session'));
  for (const url of ['http://example.test/v1/lights', 'ws://example.test/v1/lights',
    'wss://u:p@example.test/v1/lights', 'wss://example.test/v1/lights?q=1', 'wss://example.test/elsewhere']) assert.throws(() => validateRelayURL(url));
  assert.throws(() => unbase64('A'.repeat(1369)));
  assert.throws(() => parseTrustedDescriptor({...descriptor(), role: 'owner'}));
  assert.throws(() => parseTrustedDescriptor({...descriptor(), patch: 0}));
});

function fakeTransport({tamper = false, code = 'ok', clock} = {}) {
  const sent = []; const sockets = []; const sendTimes = [];
  class FakeWebSocket {
    readyState = 1; bufferedAmount = 0;
    constructor() { sockets.push(this); queueMicrotask(() => this.onmessage?.({data: online()})); }
    send(wire) {
      sendTimes.push(performance.now());
      sent.push(wire);
      const frame = JSON.parse(wire), id = frame.id;
      assert.equal(id, sent.length); assert.equal(frame.role_hint, 'guest');
      let task;
      if (id === 1) { assert.equal(hex(unbase64(frame.payload)), normal.request0); task = Promise.resolve(fromHex(normal.response0)); }
      else if (id === 2) { assert.equal(hex(unbase64(frame.payload)), normal.request1); task = Promise.resolve(fromHex(normal.response1)); }
      else if (id === 3) task = Promise.resolve(fromHex(normal.binding_ciphertext));
      else task = (async () => {
        const key = await webcrypto.subtle.importKey('raw', fromHex(normal.key), 'AES-GCM', false, ['encrypt', 'decrypt']);
        const nonce = concat(fromHex(normal.nonce).slice(0, 8), uint32(3));
        const plain = await webcrypto.subtle.decrypt({name: 'AES-GCM', iv: nonce}, key, unbase64(frame.payload));
        assert.deepEqual(JSON.parse(new TextDecoder().decode(plain)), {v: 1, op: 'room'});
        return new Uint8Array(await webcrypto.subtle.encrypt({name: 'AES-GCM', iv: concat(nonce.slice(0, 8), uint32(4))}, key,
          utf8(JSON.stringify({v: 1, ok: code === 'ok', code, boot: normal.boot}))));
      })();
      task.then(payload => this.onmessage?.({data: reply(id, payload, tamper && id === 3 ? {boot: 'f'.repeat(16)} : {})}));
    }
    close() { this.readyState = 3; }
  }
  return {dependencies: {WebSocket: FakeWebSocket, crypto: provider(normal), ...(clock ? {clock} : {})}, sent, sockets, sendTimes};
}

test('complete gateway pipe: proven role, explicit commands, no password in relay, no automatic enrollment', async () => {
  const fake = fakeTransport(); const states = [];
  const session = await GatewayWebSession.connectForTesting({descriptor: descriptor(), role: 'guest', credential: normal.password,
    onState: state => states.push(state)}, fake.dependencies);
  assert.equal(session.role, 'guest'); assert.equal(session.boot, normal.boot); assert.equal(fake.sent.length, 3);
  assert.deepEqual(states, ['connecting', 'authenticating', 'binding', 'ready']);
  const result = await session.request({v: 1, op: 'room'}); assert.equal(result.code, 'ok');
  for (let i = 1; i < fake.sendTimes.length; i++) assert.ok(fake.sendTimes[i] - fake.sendTimes[i - 1] >= 449);
  assert.ok(fake.sent.every(wire => !wire.includes(normal.password) && !wire.includes('guest.enroll')));
  session.close(); assert.equal(session.role, undefined); assert.equal(states.at(-1), 'closed');
});

test('application rejections return authenticated data without a retry or new guest grant', async () => {
  const fake = fakeTransport({code: 'revoked'});
  const session = await GatewayWebSession.connectForTesting({descriptor: descriptor(), role: 'guest', credential: normal.password}, fake.dependencies);
  const result = await session.request({v: 1, op: 'room'});
  assert.equal(result.ok, false); assert.equal(result.code, 'revoked'); assert.equal(fake.sent.length, 4); session.close();
});

test('boot change fails binding, no application command goes out', async () => {
  const fake = fakeTransport({tamper: true});
  await assert.rejects(GatewayWebSession.connectForTesting({descriptor: descriptor(), role: 'guest', credential: normal.password}, fake.dependencies));
  assert.equal(fake.sent.length, 3); assert.equal(fake.sockets[0].readyState, 3);
});

test('WebSocket constructor/send failure and expired replies fail closed', async () => {
  class Throws { constructor() { throw Error('fixture'); } }
  await assert.rejects(new RelayChannel('wss://example.test/v1/lights', {WebSocket: Throws}).open());
  let time = 0, socket;
  class Slow {
    readyState = 1; bufferedAmount = 0;
    constructor() { socket = this; queueMicrotask(() => this.onmessage?.({data: online()})); }
    send() { time = 4000; queueMicrotask(() => this.onmessage?.({data: reply(1, new Uint8Array([1]))})); }
    close() { this.readyState = 3; }
  }
  const channel = new RelayChannel('wss://example.test/v1/lights', {WebSocket: Slow, clock: () => time});
  await channel.open(); await assert.rejects(channel.exchange('data', new Uint8Array([1]), 'guest'));
  assert.equal(socket.readyState, 3); assert.equal(channel.isOpen, false);
  class ThrowSend extends Slow { send() { throw Error('fixture-send'); } }
  const throwing = new RelayChannel('wss://example.test/v1/lights', {WebSocket: ThrowSend});
  await throwing.open(); await assert.rejects(throwing.exchange('data', new Uint8Array([1]), 'guest'));
  assert.equal(throwing.isOpen, false);
});

test('close cancels pacing before any encrypted application record is sent', async () => {
  const fake = fakeTransport();
  const session = await GatewayWebSession.connectForTesting({descriptor: descriptor(), role: 'guest', credential: normal.password}, fake.dependencies);
  const request = session.request({v: 1, op: 'room'});
  queueMicrotask(() => session.close());
  await assert.rejects(request); assert.equal(fake.sent.length, 3); assert.equal(session.role, undefined);
});
