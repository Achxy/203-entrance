import {cryptoProvider, equal, fail, fromHex, utf8} from './bytes.mjs';
import {exactKeys, parseStrictJSON} from './strict-json.mjs';

const SPKI_PREFIX = fromHex('3059301306072a8648ce3d020106082a8648ce3d03010703420004');
const ORDER = BigInt('0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551');
export function validateSPKI(value) {
  const bytes = fromHex(value, 91);
  if (!equal(bytes.slice(0, SPKI_PREFIX.length), SPKI_PREFIX)) fail('invalid_descriptor');
  return bytes;
}
export function validRole(role) { if (role !== 'owner' && role !== 'guest') fail('invalid_role'); return role; }
export function validGateway(value) { if (typeof value !== 'string' || !/^[a-z0-9-]{1,48}$/.test(value)) fail('invalid_descriptor'); return value; }

export function derToRaw(signature) {
  if (!(signature instanceof Uint8Array) || signature.length < 8 || signature.length > 72 ||
      signature[0] !== 0x30 || signature[1] !== signature.length - 2) fail('invalid_signature');
  let at = 2; const raw = new Uint8Array(64);
  for (let half = 0; half < 2; half++) {
    if (signature[at++] !== 2) fail('invalid_signature');
    const length = signature[at++];
    if (length < 1 || length > 33 || at + length > signature.length || signature[at] & 0x80 ||
        length > 1 && signature[at] === 0 && !(signature[at + 1] & 0x80)) fail('invalid_signature');
    let value = signature.slice(at, at + length); at += length;
    if (value.length === 33) { if (value[0] !== 0) fail('invalid_signature'); value = value.slice(1); }
    let integer = 0n; for (const n of value) integer = integer * 256n + BigInt(n);
    if (integer === 0n || integer >= ORDER) fail('invalid_signature');
    raw.set(value, (half + 1) * 32 - value.length);
  }
  if (at !== signature.length) fail('invalid_signature');
  return raw;
}

export function bindingContext({gateway_id, boot, transcript, challenge, role}) {
  validGateway(gateway_id); fromHex(boot, 8); fromHex(transcript, 32); fromHex(challenge, 32); validRole(role);
  return utf8(`203-lights-channel-v1\n${gateway_id}\n${boot}\n${transcript}\n${challenge}\n${role}\n`);
}

export async function verifyBinding(plaintext, expected, provider = globalThis.crypto) {
  const reply = parseStrictJSON(plaintext, {flat: true, ascii: true, unescaped: true, maximumKeys: 8});
  exactKeys(reply, ['v', 'op', 'gateway_id', 'boot', 'transcript', 'challenge', 'role', 'signature']);
  if (reply.v !== 1 || reply.op !== 'bound' ||
      ['gateway_id', 'boot', 'transcript', 'challenge', 'role'].some(k => reply[k] !== expected[k])) fail('identity_mismatch');
  const signature = derToRaw(fromHex(reply.signature));
  const key = await cryptoProvider(provider).subtle.importKey('spki', validateSPKI(expected.public_key_spki),
    {name: 'ECDSA', namedCurve: 'P-256'}, false, ['verify']);
  if (!await provider.subtle.verify({name: 'ECDSA', hash: 'SHA-256'}, key, signature, bindingContext(reply))) fail('identity_mismatch');
}
