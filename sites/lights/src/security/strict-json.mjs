import {fail, text, utf8} from './bytes.mjs';

/** Bounded parser: duplicate decoded keys cannot be discarded by JSON.parse.
 * Objects have no prototype. Integers are exact; 64-bit protocol values are
 * decimal strings. This checks syntax, never application authorization. */
export function parseStrictJSON(input, {maximumBytes = 1008, flat = false,
  ascii = false, unescaped = false, maximumKeys = 64} = {}) {
  const bytes = typeof input === 'string' ? utf8(input) : input;
  if (!(bytes instanceof Uint8Array) || bytes.length > maximumBytes) fail('message_too_large');
  if (!bytes.length) fail();
  const source = text(bytes); let at = 0, nodes = 0;
  const whitespace = () => { while (/[\t\n\r ]/.test(source[at] ?? '\0')) at++; };
  const take = c => { whitespace(); if (source[at] !== c) return false; at++; return true; };
  function string() {
    whitespace(); if (source[at] !== '"') fail();
    const start = at++; let escaped = false;
    while (at < source.length) {
      const c = source[at++];
      if (!escaped && c === '"') {
        let value;
        try { value = JSON.parse(source.slice(start, at)); } catch { return fail(); }
        for (const point of value) {
          const n = point.codePointAt(0);
          if (n >= 0xd800 && n <= 0xdfff || ascii && (n < 32 || n > 126)) fail();
        }
        return value;
      }
      if (!escaped && c === '\\') { if (unescaped) fail(); escaped = true; }
      else escaped = false;
    }
    return fail();
  }
  function value(depth) {
    whitespace(); if (++nodes > 256 || depth > 8) fail('message_too_large');
    const c = source[at];
    if (c === '"') return string();
    if (c === '{') {
      if (flat && depth) fail();
      at++; const result = Object.create(null); let count = 0;
      if (take('}')) return result;
      do {
        const key = string();
        if (++count > maximumKeys || Object.hasOwn(result, key) || !take(':')) fail();
        result[key] = value(depth + 1);
        if (take('}')) return result;
      } while (take(','));
      return fail();
    }
    if (c === '[') {
      if (flat) fail();
      at++; const result = [];
      if (take(']')) return result;
      do { result.push(value(depth + 1)); if (take(']')) return result; } while (take(','));
      return fail();
    }
    for (const [literal, result] of [['true', true], ['false', false], ['null', null]]) {
      if (source.startsWith(literal, at)) { if (flat && result === null) fail(); at += literal.length; return result; }
    }
    const found = /^-?(?:0|[1-9][0-9]*)/.exec(source.slice(at));
    if (!found) fail();
    at += found[0].length;
    const result = Number(found[0]);
    if (!Number.isSafeInteger(result) || Object.is(result, -0)) fail();
    return result;
  }
  const result = value(0); whitespace();
  if (at !== source.length || !result || typeof result !== 'object' || Array.isArray(result)) fail();
  return result;
}

export function exactKeys(object, required, optional = []) {
  const keys = Object.keys(object);
  if (required.some(k => !Object.hasOwn(object, k)) || keys.some(k => !required.includes(k) && !optional.includes(k))) fail();
}

export function encodeObject(object, maximumBytes = 1008) {
  if (!object || typeof object !== 'object' || Array.isArray(object) ||
      ![Object.prototype, null].includes(Object.getPrototypeOf(object))) fail();
  let result;
  try { result = utf8(JSON.stringify(object)); } catch { return fail(); }
  parseStrictJSON(result, {maximumBytes});
  return result;
}
