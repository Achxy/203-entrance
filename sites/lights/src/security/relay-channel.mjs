import {base64, fail, fromHex, GatewaySecurityError, unbase64, utf8} from './bytes.mjs';
import {exactKeys, parseStrictJSON} from './strict-json.mjs';

export function validateRelayURL(input) {
  let endpoint;
  try { endpoint = new URL(input); } catch { return fail('invalid_endpoint'); }
  if (endpoint.protocol !== 'wss:' || !endpoint.hostname || endpoint.pathname !== '/v1/lights' ||
      endpoint.username || endpoint.password || endpoint.search || endpoint.hash) fail('invalid_endpoint');
  return endpoint.href;
}

export function decodeOnline(input) {
  const reply = parseStrictJSON(input, {maximumBytes: 2048, flat: true, maximumKeys: 9});
  exactKeys(reply, ['v', 'type', 'online', 'connection', 'boot', 'protocol']);
  if (reply.v !== 1 || reply.type !== 'light_online' || reply.online !== true || reply.protocol !== 1) fail('gateway_offline');
  fromHex(reply.connection, 16); fromHex(reply.boot, 8);
  return Object.freeze({connection: reply.connection, boot: reply.boot});
}
export function decodeReply(input, id, identity, operation) {
  const reply = parseStrictJSON(input, {maximumBytes: 2048, flat: true, maximumKeys: 9});
  exactKeys(reply, ['v', 'type', 'connection', 'id', 'boot', 'status', 'payload'], ['role', 'error']);
  if (reply.v !== 1 || reply.type !== 'light_reply' || reply.connection !== identity.connection ||
      reply.boot !== identity.boot || reply.id !== id || !Number.isInteger(reply.status) ||
      reply.status < 200 || reply.status > 599 ||
      Object.hasOwn(reply, 'role') && (!['owner', 'guest', 'none'].includes(reply.role) || !['session', 'data'].includes(operation))) fail('mismatched_reply');
  if (reply.status !== 200 || Object.hasOwn(reply, 'error')) fail('transport_rejected');
  const bytes = unbase64(reply.payload);
  if (operation === 'close' ? bytes.length !== 0 : bytes.length === 0) fail();
  return bytes;
}

/** Bounded framing only. Relay role hints, public-info keys and online metadata
 * never grant application privileges. No automatic retry/reconnect is offered. */
export class RelayChannel {
  #url; #WebSocket; #socket; #identity; #nextID = 0; #pending; #closed = false;
  #onClose; #clock; #lastSend = -Infinity; #pacing; #exchanging = false;
  constructor(endpoint, {WebSocket = globalThis.WebSocket, onClose = () => {},
    clock = () => performance.now()} = {}) {
    this.#url = validateRelayURL(endpoint);
    if (typeof WebSocket !== 'function') fail('unsupported_browser');
    this.#WebSocket = WebSocket; this.#onClose = onClose; this.#clock = clock;
  }
  get identity() { return this.#identity; }
  get isOpen() { return !this.#closed && !!this.#identity; }
  async #pause() {
    if (this.#pacing) { this.close('concurrent_request'); fail('concurrent_request'); }
    while (this.isOpen) {
      const now = this.#clock();
      if (now < this.#lastSend) { this.close('clock_rollback'); fail('clock_rollback'); }
      const remaining = Math.ceil(450 - (now - this.#lastSend));
      if (remaining <= 0) return;
      await new Promise((resolve, reject) => {
        const timer = setTimeout(() => { this.#pacing = undefined; resolve(); }, remaining);
        this.#pacing = {timer, reject};
      });
    }
    return fail('transport_closed');
  }
  async waitUntilReady() {
    if (this.#exchanging || this.#pending) { this.close('concurrent_request'); fail('concurrent_request'); }
    await this.#pause();
  }
  #await(deadlineMs, decode) {
    if (this.#pending) { this.close('concurrent_request'); fail('concurrent_request'); }
    const promise = new Promise((resolve, reject) => {
      const deadline = this.#clock() + deadlineMs;
      const timer = setTimeout(() => this.close('transport_timeout'), deadlineMs);
      this.#pending = {resolve, reject, timer, decode, deadline};
    });
    // Constructor/send can throw before the caller reaches its await. Keep the
    // original promise rejectable without creating an unhandled secondary path.
    promise.catch(() => {});
    return promise;
  }
  async open() {
    if (this.#socket || this.#closed) fail('invalid_state');
    const promise = this.#await(10000, decodeOnline);
    try {
      const socket = new this.#WebSocket(this.#url); this.#socket = socket;
      socket.binaryType = 'arraybuffer';
      socket.onmessage = event => {
        const pending = this.#pending;
        if (!pending || this.#clock() >= pending.deadline) { this.close('unexpected_frame'); return; }
        try {
          let input = event.data;
          if (typeof input === 'string') {
            if (input.length > 2048) fail('message_too_large');
            input = utf8(input);
          } else if (input instanceof ArrayBuffer) input = new Uint8Array(input);
          else fail('invalid_frame');
          if (input.length > 2048) fail('message_too_large');
          const result = pending.decode(input);
          this.#pending = undefined; clearTimeout(pending.timer); pending.resolve(result);
        } catch (error) { this.close(error instanceof GatewaySecurityError ? error.code : 'invalid_frame'); }
      };
      socket.onerror = () => this.close('transport_error');
      socket.onclose = () => this.close('transport_closed');
      this.#identity = await promise;
      if (this.#closed) fail('transport_closed');
      return this.#identity;
    } catch (error) { this.close('transport_closed'); throw error; }
  }
  async exchange(operation, payload, roleHint) {
    if (this.#pending || this.#exchanging || this.#pacing) { this.close('concurrent_request'); fail('concurrent_request'); }
    if (!this.isOpen || this.#socket.readyState !== 1) fail('transport_closed');
    if (!['identity', 'session', 'data', 'close'].includes(operation) ||
        roleHint !== undefined && !['owner', 'guest'].includes(roleHint)) fail();
    if (!(payload instanceof Uint8Array) || (operation === 'close' ? payload.length !== 0 : !payload.length)) fail();
    this.#exchanging = true;
    try {
      await this.#pause();
      if (!this.isOpen || this.#socket.readyState !== 1) fail('transport_closed');
      if (this.#nextID === 0xffffffff) fail('counter_exhausted');
      const id = ++this.#nextID;
      const frame = {v: 1, id, op: operation, payload: base64(payload)};
      if (roleHint !== undefined) frame.role_hint = roleHint;
      const wire = JSON.stringify(frame);
      if (utf8(wire).length > 2048 || this.#socket.bufferedAmount > 4096) fail('message_too_large');
      const promise = this.#await(operation === 'data' ? 3000 : 11000,
        input => decodeReply(input, id, this.#identity, operation));
      this.#lastSend = this.#clock();
      this.#socket.send(wire);
      const result = await promise;
      if (operation === 'close') this.close();
      return result;
    } catch (error) { this.close('transport_closed'); throw error; }
    finally { this.#exchanging = false; }
  }
  close(code = 'closed') {
    if (this.#closed) return;
    this.#closed = true; this.#identity = undefined;
    const pending = this.#pending; this.#pending = undefined;
    if (pending) { clearTimeout(pending.timer); pending.reject(new GatewaySecurityError(code)); }
    const pacing = this.#pacing; this.#pacing = undefined;
    if (pacing) { clearTimeout(pacing.timer); pacing.reject(new GatewaySecurityError(code)); }
    const socket = this.#socket; this.#socket = undefined;
    if (socket) {
      socket.onmessage = socket.onerror = socket.onclose = null;
      try { socket.close(1000, 'Lighting session closed'); } catch {}
    }
    try { this.#onClose(code); } catch {}
  }
}
