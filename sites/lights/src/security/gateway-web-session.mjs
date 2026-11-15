import {cryptoProvider, fail, fromHex, GatewaySecurityError, randomHex} from './bytes.mjs';
import {encodeObject, exactKeys, parseStrictJSON} from './strict-json.mjs';
import {validGateway, validRole, validateSPKI} from './identity.mjs';
import {RelayChannel, validateRelayURL} from './relay-channel.mjs';
import {Security2Session} from './security2.mjs';
export {deriveGuestVerifier} from './srp.mjs';
export {randomHex, GatewaySecurityError};

const OPERATIONS = new Set(['guest.enroll', 'guest.resume', 'room', 'light', 'sessions',
  'set', 'command', 'freeze', 'revoke', 'revoke_all', 'rotate_guest', 'discover',
  'candidate', 'discovery_cancel', 'adopt', 'remove', 'home_key']);

/** The caller must obtain this through an owner-verified invitation. A relay
 * /info response, advertisement, or fetched page is not promoted to trust here. */
export function parseTrustedDescriptor(input) {
  const value = typeof input === 'string' || input instanceof Uint8Array ?
    parseStrictJSON(input, {maximumBytes: 2048, flat: true, ascii: true, unescaped: true, maximumKeys: 6}) :
    parseStrictJSON(encodeObject(input, 2048), {maximumBytes: 2048, flat: true, ascii: true, unescaped: true, maximumKeys: 6});
  exactKeys(value, ['v', 'gateway_id', 'public_key_spki', 'relay_url', 'security', 'patch']);
  if (value.v !== 1 || value.security !== 2 || value.patch !== 1) fail('unsupported_security');
  validGateway(value.gateway_id); validateSPKI(value.public_key_spki);
  value.relay_url = validateRelayURL(value.relay_url);
  return Object.freeze(value);
}

const CONSTRUCTOR = Symbol('GatewayWebSession');
export class GatewayWebSession {
  #channel; #security; #descriptor; #busy = false; #closed = false; #onState; #role; #pagehide;
  constructor(token) { if (token !== CONSTRUCTOR) fail('use_connect'); }
  get role() { return this.#closed ? undefined : this.#role; }
  get boot() { return this.#closed ? undefined : this.#channel?.identity?.boot; }
  get isOpen() { return !this.#closed && this.#channel?.isOpen && this.#role !== undefined; }
  #notify(state) { try { this.#onState?.(state); } catch {} }

  static async connect({descriptor, role, credential, onState, signal} = {}) {
    return this.#connect({descriptor, role, credential, onState, signal}, {});
  }
  // Deterministic test transport/provider injection is intentionally isolated
  // from the production connect API. Never feed external input to this hook.
  static async connectForTesting(options, dependencies) { return this.#connect(options, dependencies); }
  static async #connect({descriptor, role, credential, onState, signal}, dependencies) {
    cryptoProvider(dependencies.crypto);
    descriptor = parseTrustedDescriptor(descriptor); validRole(role);
    const instance = new GatewayWebSession(CONSTRUCTOR);
    instance.#descriptor = descriptor; instance.#onState = onState;
    instance.#security = new Security2Session({role, credential, patch: descriptor.patch,
      crypto: dependencies.crypto ?? globalThis.crypto});
    credential = undefined; // Best effort; caller must clear its input field too.
    instance.#channel = new RelayChannel(descriptor.relay_url, {
      ...dependencies, onClose: () => instance.close()
    });
    const abort = () => instance.close();
    signal?.addEventListener('abort', abort, {once: true});
    try {
      if (signal?.aborted) fail('cancelled');
      instance.#notify('connecting');
      const identity = await instance.#channel.open();
      instance.#notify('authenticating');
      const request0 = await instance.#security.startHandshake();
      const response0 = await instance.#channel.exchange('session', request0, role);
      const request1 = await instance.#security.receiveChallenge(response0);
      const response1 = await instance.#channel.exchange('session', request1, role);
      await instance.#security.verifyServer(response1);
      instance.#notify('binding');
      await instance.#channel.waitUntilReady();
      const binding = await instance.#security.makeBinding(descriptor, identity.boot, role);
      await instance.#security.verifyBinding(await instance.#channel.exchange('data', binding, role));
      if (instance.#closed || !instance.#channel.isOpen) fail('transport_closed');
      instance.#role = instance.#security.role;
      instance.#pagehide = () => instance.close();
      globalThis.addEventListener?.('pagehide', instance.#pagehide, {once: true});
      instance.#notify('ready');
      return instance;
    } catch (error) {
      instance.close();
      if (error instanceof GatewaySecurityError) throw error;
      return fail('connection_failed');
    } finally { signal?.removeEventListener('abort', abort); }
  }
  async request(object) {
    if (!this.isOpen || this.#busy) {
      const code = this.#busy ? 'concurrent_request' : 'transport_closed'; this.close(); fail(code);
    }
    this.#busy = true;
    try {
      const bytes = encodeObject(object);
      const request = parseStrictJSON(bytes);
      if (request.v !== 1 || !OPERATIONS.has(request.op)) fail('unsupported_operation');
      // These values survive failures unchanged. The UI must retain the same
      // command ID for status reconciliation, never mint an automatic retry ID.
      if (request.op === 'set') {
        fromHex(request.id, 16); fromHex(request.boot, 8);
        if (request.boot !== this.boot) fail('wrong_boot');
      }
      await this.#channel.waitUntilReady();
      const cipher = await this.#security.sealRequest(bytes);
      bytes.fill(0);
      const plain = await this.#security.openResponse(await this.#channel.exchange('data', cipher, this.#role));
      let reply;
      try { reply = parseStrictJSON(plain); } finally { plain.fill(0); }
      if (reply.v !== 1 || typeof reply.ok !== 'boolean' || typeof reply.code !== 'string' || reply.code.length > 64) fail('invalid_application_reply');
      this.#busy = false;
      return reply; // Authenticated application rejection is returned, not retried.
    } catch (error) {
      this.close();
      if (error instanceof GatewaySecurityError) throw error;
      return fail('request_failed');
    }
  }
  close() {
    if (this.#closed) return;
    this.#closed = true; this.#role = undefined; this.#busy = false;
    this.#security?.close(); this.#channel?.close(); this.#descriptor = undefined;
    if (this.#pagehide) globalThis.removeEventListener?.('pagehide', this.#pagehide);
    this.#pagehide = undefined;
    this.#notify('closed');
  }
}
