// SRP-6a profile adapted from Espressif's pinned provisioning client and
// ESP-IDF server. See NOTICE.md for attribution, formulas, and limitations.
import {concat, cryptoProvider, digest, equal, fail, fromHex, hex, randomBytes, utf8} from './bytes.mjs';

const PRIME_HEX = 'ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e08' +
  '8a67cc74020bbea63b139b22514a08798e3404ddef9519b3cd3a431b' +
  '302b0a6df25f14374fe1356d6d51c245e485b576625e7ec6f44c42e9' +
  'a637ed6b0bff5cb6f406b7edee386bfb5a899fa5ae9f24117c4b1fe6' +
  '49286651ece45b3dc2007cb8a163bf0598da48361c55d39a69163fa8' +
  'fd24cf5f83655d23dca3ad961c62f356208552bb9ed529077096966d' +
  '670c354e4abc9804f1746c08ca18217c32905e462e36ce3be39e772c' +
  '180e86039b2783a2ec07a28fb5c55df06f4c52c9de2bcbf695581718' +
  '3995497cea956ae515d2261898fa051015728e5a8aaac42dad33170d' +
  '04507a33a85521abdf1cba64ecfb850458dbef0a8aea71575d060c7d' +
  'b3970f85a6e1e4c7abf5ae8cdb0933d71e8c94e04a25619dcee3d226' +
  '1ad2ee6bf12ffa06d98a0864d87602733ec86a64521f2b18177b200c' +
  'bbe117577a615d6c770988c0bad946e208e24fa074e5ab3143db5bfc' +
  'e0fd108e4b82d120a93ad2caffffffffffffffff';
const N = BigInt(`0x${PRIME_HEX}`), G = 5n;
const N_BYTES = fromHex(PRIME_HEX, 384);
function numeric(bytes) { return BigInt(`0x${hex(bytes) || '0'}`); }
function minimal(number) {
  let value = number.toString(16); if (value.length % 2) value = `0${value}`;
  return fromHex(value);
}
function pad(bytes) {
  if (bytes.length > 384) fail();
  const result = new Uint8Array(384); result.set(bytes, 384 - bytes.length); return result;
}
const G_PAD = pad(new Uint8Array([5]));

// Native JS BigInt modular arithmetic is variable-time. This is not hardened
// against same-process timing attacks, and WebCrypto currently has no SRP API.
// Yield periodically to preserve UI responsiveness on mobile browsers.
async function power(base, exponent) {
  let result = 1n, steps = 0; base %= N;
  while (exponent > 0n) {
    if (exponent & 1n) result = result * base % N;
    exponent >>= 1n; base = base * base % N;
    if (++steps % 128 === 0) await new Promise(resolve => setTimeout(resolve, 0));
  }
  return result;
}

export function credentialBytes(value) {
  if (typeof value !== 'string') fail('invalid_credentials');
  for (const point of value) {
    const n = point.codePointAt(0); if (n >= 0xd800 && n <= 0xdfff || n === 0) fail('invalid_credentials');
  }
  const bytes = utf8(value);
  if (bytes.length < 1 || bytes.length > 256) fail('invalid_credentials');
  return bytes;
}

export class SRPClient {
  #provider; #a; #username; #password; #public; #proof; #key; #used = false;
  constructor(username, password, provider = globalThis.crypto) {
    this.#provider = cryptoProvider(provider);
    this.#username = utf8(username); this.#password = credentialBytes(password);
    if (this.#username.length < 1 || this.#username.length > 64) { this.close(); fail('invalid_credentials'); }
  }
  async start() {
    if (this.#a !== undefined || this.#used) fail('invalid_state');
    const ephemeral = randomBytes(32, this.#provider);
    this.#a = numeric(ephemeral); ephemeral.fill(0);
    if (!this.#a) fail('random_failed');
    this.#public = pad(minimal(await power(G, this.#a)));
    return this.#public.slice();
  }
  async challenge(salt, BData) {
    if (this.#used || !this.#public || this.#a === undefined || salt.length < 16 || salt.length > 64 ||
        BData.length < 1 || BData.length > 384) fail('invalid_state');
    this.#used = true;
    const H = bytes => digest('SHA-512', bytes, this.#provider);
    try {
      const B = numeric(BData); if (B <= 0n || B >= N) fail('authentication_failed');
      const u = numeric(await H(concat(this.#public, pad(BData)))); if (u === 0n) fail('authentication_failed');
      const k = numeric(await H(concat(N_BYTES, G_PAD)));
      const credentials = concat(this.#username, new Uint8Array([58]), this.#password);
      let inner;
      try { inner = await H(credentials); } finally { credentials.fill(0); }
      const x = numeric(await H(concat(salt, inner))); inner.fill(0);
      const v = await power(G, x);
      const S = await power((B + N - k * v % N) % N, this.#a + u * x);
      if (S === 0n) fail('authentication_failed');
      const K = await H(minimal(S));
      try {
        const keyBytes = K.slice(0, 32);
        try { this.#key = await this.#provider.subtle.importKey('raw', keyBytes, {name: 'AES-GCM'}, false, ['encrypt', 'decrypt']); }
        finally { keyBytes.fill(0); }
        const hN = await H(N_BYTES), hg = await H(G_PAD);
        const xor = hN.map((n, i) => n ^ hg[i]);
        // IDF pads A on the wire, but hashes B exactly as received and S minimally.
        const M = await H(concat(xor, await H(this.#username), salt, this.#public, BData, K));
        this.#proof = await H(concat(this.#public, M, K));
        return M;
      } finally { K.fill(0); }
    } finally { this.#password.fill(0); this.#username.fill(0); this.#a = undefined; }
  }
  verify(proof) {
    if (!this.#proof || !this.#key || proof.length !== 64 || !equal(proof, this.#proof)) {
      this.close(); fail('authentication_failed');
    }
    const key = this.#key; this.close(); return key;
  }
  close() {
    this.#password?.fill(0); this.#username?.fill(0); this.#proof?.fill(0);
    this.#password = new Uint8Array(); this.#username = new Uint8Array();
    this.#a = undefined; this.#key = undefined; this.#proof = undefined; this.#public = undefined; this.#used = true;
  }
}

/** Internal deterministic hook is used only by fixture tests. Production caller
 * never supplies salt; export through deriveGuestVerifier uses fresh CSPRNG. */
export async function deriveVerifier(username, password, salt, provider = globalThis.crypto) {
  provider = cryptoProvider(provider);
  if (!(salt instanceof Uint8Array) || salt.length !== 16 || !['203-guest', 'wifiprov'].includes(username)) fail();
  const secret = credentialBytes(password), identity = utf8(username);
  const credentials = concat(identity, new Uint8Array([58]), secret);
  try {
    const inner = await digest('SHA-512', credentials, provider);
    const x = numeric(await digest('SHA-512', concat(salt, inner), provider)); inner.fill(0);
    return pad(minimal(await power(G, x)));
  } finally { secret.fill(0); credentials.fill(0); }
}
export async function deriveGuestVerifier(password) {
  const salt = randomBytes(16);
  const verifier = await deriveVerifier('203-guest', password, salt);
  return {salt: hex(salt), verifier: hex(verifier)};
}
