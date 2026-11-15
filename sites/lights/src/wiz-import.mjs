import {parseStrictJSON} from './security/strict-json.mjs';

// A bounded local file convenience, not a cloud invitation reader. Export
// nesting is not standardized; require exactly one key and ignore all metadata.
export function extractWiZHomeKey(input) {
  let root;
  try {root = parseStrictJSON(input, {maximumBytes: 65536});}
  catch {throw Error('This export could not be read safely. Paste its 32-character udp_signing_key instead.');}
  const found = [];
  function visit(value) {
    if (!value || typeof value !== 'object') return;
    if (Object.hasOwn(value, 'udp_signing_key')) found.push(value.udp_signing_key);
    for (const item of Object.values(value)) visit(item);
  }
  visit(root);
  if (found.length !== 1 || typeof found[0] !== 'string' || !/^[a-f0-9]{32}$/i.test(found[0]))
    throw Error('The file must contain exactly one 32-character udp_signing_key. For multiple homes, paste the intended home’s key instead.');
  return found[0].toLowerCase();
}
