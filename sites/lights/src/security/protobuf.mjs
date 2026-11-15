import {concat, equal, fail} from './bytes.mjs';

function varint(value) {
  const bytes = [];
  do { let n = value & 127; value >>>= 7; if (value) n |= 128; bytes.push(n); } while (value);
  return Uint8Array.from(bytes);
}
const integer = (field, value) => concat(varint(field * 8), varint(value));
const blob = (field, value) => concat(varint(field * 8 + 2), varint(value.length), value);

function readFields(bytes, schema) {
  if (!(bytes instanceof Uint8Array) || !bytes.length || bytes.length > 1024) fail();
  let at = 0, previous = 0; const out = new Map();
  function number() {
    const start = at; let n = 0, factor = 1;
    for (let count = 0; count < 5 && at < bytes.length; count++) {
      const b = bytes[at++]; n += (b & 127) * factor;
      if (!(b & 128)) {
        if (n > 0xffffffff || !equal(bytes.slice(start, at), varint(n))) fail();
        return n;
      }
      factor *= 128;
    }
    return fail();
  }
  while (at < bytes.length) {
    const tag = number(), field = Math.floor(tag / 8), wire = tag % 8;
    if (field <= previous || !Object.hasOwn(schema, field) || schema[field] !== wire) fail();
    previous = field;
    if (wire === 0) {
      const n = number(); if (!n) fail(); // Proto3 canonical encoding omits zero defaults.
      out.set(field, n);
    } else {
      const length = number();
      if (!length || length > bytes.length - at) fail();
      out.set(field, bytes.slice(at, at + length)); at += length;
    }
  }
  return out;
}

export function command0(username, publicKey) {
  if (username.length < 1 || username.length > 64 || publicKey.length !== 384) fail();
  return blobEnvelope(blob(20, concat(blob(1, username), blob(2, publicKey))));
}
export function command1(proof) {
  if (proof.length !== 64) fail();
  return blobEnvelope(concat(integer(1, 2), blob(22, blob(1, proof))));
}
function blobEnvelope(payload) { return concat(integer(2, 2), blob(12, payload)); }

export function response(bytes, stage) {
  const envelope = readFields(bytes, {2: 0, 12: 2});
  if (envelope.size !== 2 || envelope.get(2) !== 2) fail('unsupported_security');
  const field = stage === 0 ? 21 : stage === 1 ? 23 : fail();
  const payload = readFields(envelope.get(12), {1: 0, [field]: 2});
  if (payload.size !== 2 || payload.get(1) !== (stage === 0 ? 1 : 3)) fail();
  const result = readFields(payload.get(field), {1: 0, 2: 2, 3: 2});
  if (result.has(1)) fail('authentication_failed');
  if (result.size !== 2 || !result.has(2) || !result.has(3)) fail();
  if (stage === 0) {
    const publicKey = result.get(2), salt = result.get(3);
    if (publicKey.length < 1 || publicKey.length > 384 || salt.length < 16 || salt.length > 64) fail();
    return {publicKey, salt};
  }
  const proof = result.get(2), nonce = result.get(3);
  if (proof.length !== 64 || nonce.length !== 12 || !equal(nonce.slice(8), new Uint8Array([0, 0, 0, 1]))) fail();
  return {proof, nonce};
}
