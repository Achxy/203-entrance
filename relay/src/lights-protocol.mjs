import {flat, integer, validHex} from './protocol.mjs';

export const LIGHTS_MAX_PAYLOAD = 1024;
export const LIGHTS_MAX_WIRE = 2048;
export const LIGHTS_DATA_TTL = 2000;
export const LIGHTS_HANDSHAKE_TTL = 10000;
export const LIGHTS_IDLE_TTL = 60000;
export const LIGHTS_SESSION_TTL = 30 * 60000;
const encoder = new TextEncoder();
const operations = new Set(['identity', 'session', 'data', 'close']);
const hints = new Set(['unknown', 'owner', 'guest']);
const roles = new Set(['owner', 'guest', 'none']);

// Require a unique encoding: whitespace, URL-safe spelling, missing padding,
// non-zero trailing bits and oversized binary records are all rejected.
export function lightPayload(value) {
  if (typeof value !== 'string' || value.length > 4 * Math.ceil(LIGHTS_MAX_PAYLOAD / 3) ||
      !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) throw Error('Invalid lighting payload');
  const decoded = atob(value);
  if (decoded.length > LIGHTS_MAX_PAYLOAD || btoa(decoded) !== value) throw Error('Invalid lighting payload');
  return value;
}

export function lightRequest(text) {
  const m = flat(text, LIGHTS_MAX_WIRE);
  const allowed = new Set(['v', 'id', 'op', 'payload', 'role_hint']);
  if (Object.keys(m).some(k => !allowed.has(k)) || m.v !== 1 ||
      !integer(m.id, 1, 0xffffffff) || !operations.has(m.op)) throw Error('Invalid lighting request');
  lightPayload(m.payload);
  if (Object.hasOwn(m, 'role_hint') && !hints.has(m.role_hint)) throw Error('Invalid role hint');
  if (m.op === 'close' ? m.payload !== '' : m.payload === '') throw Error('Invalid lighting payload');
  return {...m, role_hint: m.role_hint ?? 'unknown'};
}

export function lightReply(text) {
  const m = flat(text, LIGHTS_MAX_WIRE);
  const allowed = new Set(['v', 'type', 'connection', 'id', 'boot', 'status', 'payload', 'role']);
  if (Object.keys(m).some(k => !allowed.has(k)) || m.v !== 1 || m.type !== 'light_reply' ||
      !validHex(m.connection, 32) || !validHex(m.boot, 16) ||
      !integer(m.id, 1, 0xffffffff) || !integer(m.status, 200, 599) ||
      (Object.hasOwn(m, 'role') && !roles.has(m.role))) throw Error('Invalid lighting reply');
  lightPayload(m.payload);
  return m;
}

export function lightForward(m, connection, boot, lease, ttl) {
  const out = {v: 1, type: 'light_request', connection, id: m.id, op: m.op,
    payload: m.payload, role_hint: m.role_hint, boot, lease, ttl_ms: ttl};
  if (encoder.encode(JSON.stringify(out)).length > LIGHTS_MAX_WIRE) throw Error('Lighting frame exceeds device budget');
  return out;
}
