export class GatewaySecurityError extends Error {
  constructor(code) { super(code); this.name = 'GatewaySecurityError'; this.code = code; }
}
export function fail(code = 'invalid_message') { throw new GatewaySecurityError(code); }
export function cryptoProvider(value = globalThis.crypto) {
  if (!value?.subtle || typeof value.getRandomValues !== 'function' || typeof BigInt !== 'function') fail('unsupported_browser');
  return value;
}
export const utf8 = value => new TextEncoder().encode(value);
export function text(bytes) {
  try { return new TextDecoder('utf-8', {fatal: true}).decode(bytes); }
  catch { return fail(); }
}
export function concat(...parts) {
  const size = parts.reduce((n, p) => n + p.length, 0);
  const result = new Uint8Array(size); let at = 0;
  for (const part of parts) { result.set(part, at); at += part.length; }
  return result;
}
export function hex(bytes) { return Array.from(bytes, n => n.toString(16).padStart(2, '0')).join(''); }
export function fromHex(value, length) {
  if (typeof value !== 'string' || value.length % 2 || !/^[0-9a-f]*$/.test(value) ||
      (length !== undefined && value.length !== length * 2)) fail();
  return Uint8Array.from(value.match(/../g) ?? [], n => Number.parseInt(n, 16));
}
export function equal(a, b) {
  if (a.length !== b.length) return false;
  let difference = 0;
  for (let i = 0; i < a.length; i++) difference |= a[i] ^ b[i];
  return difference === 0; // Logical full-length comparison, not a JS timing guarantee.
}
export function uint32(value) {
  if (!Number.isInteger(value) || value < 0 || value > 0xffffffff) fail();
  const result = new Uint8Array(4); new DataView(result.buffer).setUint32(0, value, false); return result;
}
export function randomBytes(size, provider = globalThis.crypto) {
  if (!Number.isInteger(size) || size < 1 || size > 64) fail();
  return cryptoProvider(provider).getRandomValues(new Uint8Array(size));
}
export function randomHex(size = 32) { return hex(randomBytes(size)); }
export async function digest(name, bytes, provider = globalThis.crypto) {
  return new Uint8Array(await cryptoProvider(provider).subtle.digest(name, bytes));
}
export function base64(bytes) {
  if (!(bytes instanceof Uint8Array) || bytes.length > 1024) fail('message_too_large');
  return btoa(String.fromCharCode(...bytes));
}
export function unbase64(value) {
  if (typeof value !== 'string' || value.length > 1368 ||
      !/^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$/.test(value)) fail();
  let result;
  try { result = Uint8Array.from(atob(value), n => n.charCodeAt(0)); } catch { return fail(); }
  if (result.length > 1024 || base64(result) !== value) fail();
  return result;
}
