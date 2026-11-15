import {concat, cryptoProvider, digest, fail, fromHex, GatewaySecurityError, hex, randomBytes, uint32, utf8} from './bytes.mjs';
import {command0, command1, response} from './protobuf.mjs';
import {SRPClient} from './srp.mjs';
import {validGateway, validRole, validateSPKI, verifyBinding} from './identity.mjs';

/** Internal transport-neutral Security2 patch-1 state machine. No Security0/1
 * fallback and no application encryption until the trusted identity binds. */
export class Security2Session {
  #provider; #srp; #username; #key; #noncePrefix; #counter = 0; #records = [];
  #transcript; #pending; #state = 'ready'; #busy = false; #epoch = 0; #role;
  constructor({role, credential, patch = 1, crypto = globalThis.crypto}) {
    validRole(role); if (patch !== 1) fail('unsupported_security');
    this.#provider = cryptoProvider(crypto); this.#username = utf8(`203-${role}`);
    this.#srp = new SRPClient(`203-${role}`, credential, this.#provider);
  }
  get state() { return this.#state; }
  get role() { return this.#role; }
  get transcript() { if (!this.#transcript) fail('invalid_state'); return this.#transcript; }
  async #run(expected, work) {
    if (this.#busy || this.#state !== expected) { this.close(); fail('invalid_state'); }
    this.#busy = true; const epoch = this.#epoch;
    try {
      const result = await work();
      if (epoch !== this.#epoch) fail('closed');
      this.#busy = false; return result;
    } catch (error) {
      this.close();
      if (error instanceof GatewaySecurityError) throw error;
      return fail('cryptographic_failure');
    }
  }
  #append(bytes) { this.#records.push(uint32(bytes.length), bytes.slice()); }
  startHandshake() {
    return this.#run('ready', async () => {
      const bytes = command0(this.#username, await this.#srp.start());
      this.#append(bytes); this.#state = 'challenge'; return bytes;
    });
  }
  receiveChallenge(bytes) {
    return this.#run('challenge', async () => {
      const {salt, publicKey} = response(bytes, 0);
      const M = await this.#srp.challenge(salt, publicKey);
      this.#append(bytes); const request = command1(M); M.fill(0);
      this.#append(request); this.#state = 'proof'; return request;
    });
  }
  verifyServer(bytes) {
    return this.#run('proof', async () => {
      const {proof, nonce} = response(bytes, 1);
      this.#key = this.#srp.verify(proof); this.#srp = undefined;
      this.#noncePrefix = nonce.slice(0, 8); this.#counter = 1;
      this.#append(bytes);
      this.#transcript = hex(await digest('SHA-256', concat(...this.#records), this.#provider));
      this.#records = []; this.#username.fill(0); this.#state = 'binding';
    });
  }
  #nonce() {
    if (!this.#noncePrefix || this.#counter < 1 || this.#counter >= 0xffffffff) fail('nonce_exhausted');
    return concat(this.#noncePrefix, uint32(this.#counter));
  }
  async #seal(plain) {
    if (!(plain instanceof Uint8Array) || plain.length > 1008) fail('message_too_large');
    const nonce = this.#nonce(); this.#counter++;
    return new Uint8Array(await this.#provider.subtle.encrypt({name: 'AES-GCM', iv: nonce, tagLength: 128}, this.#key, plain));
  }
  async #open(cipher) {
    if (!(cipher instanceof Uint8Array) || cipher.length < 16 || cipher.length > 1024) fail('message_too_large');
    const nonce = this.#nonce(); this.#counter++;
    return new Uint8Array(await this.#provider.subtle.decrypt({name: 'AES-GCM', iv: nonce, tagLength: 128}, this.#key, cipher));
  }
  makeBinding(descriptor, boot, role) {
    return this.#run('binding', async () => {
      validGateway(descriptor.gateway_id); validateSPKI(descriptor.public_key_spki); fromHex(boot, 8); validRole(role);
      const challenge = hex(randomBytes(32, this.#provider));
      this.#pending = {gateway_id: descriptor.gateway_id, public_key_spki: descriptor.public_key_spki,
        boot, role, challenge, transcript: this.#transcript};
      const cipher = await this.#seal(utf8(JSON.stringify({v: 1, op: 'bind', challenge})));
      this.#state = 'binding_reply'; return cipher;
    });
  }
  verifyBinding(cipher) {
    return this.#run('binding_reply', async () => {
      const plain = await this.#open(cipher);
      try { await verifyBinding(plain, this.#pending, this.#provider); } finally { plain.fill(0); }
      this.#role = this.#pending.role; this.#pending = undefined; this.#state = 'request';
    });
  }
  sealRequest(plain) {
    return this.#run('request', async () => { const cipher = await this.#seal(plain); this.#state = 'reply'; return cipher; });
  }
  openResponse(cipher) {
    return this.#run('reply', async () => { const plain = await this.#open(cipher); this.#state = 'request'; return plain; });
  }
  close() {
    this.#epoch++; this.#srp?.close(); this.#srp = undefined; this.#username?.fill(0);
    this.#key = undefined; this.#noncePrefix = undefined; this.#counter = 0;
    this.#records = []; this.#transcript = undefined; this.#pending = undefined;
    this.#role = undefined; this.#state = 'closed'; this.#busy = false;
  }
}
