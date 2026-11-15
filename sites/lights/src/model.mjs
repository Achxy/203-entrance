export const FEATURES = Object.freeze({power: 1, brightness: 2, rgb: 4, kelvin: 8, scene: 16, speed: 32});
export const SCENES = Object.freeze({1: 'Ocean', 2: 'Romance', 3: 'Sunset', 4: 'Party', 5: 'Fireplace', 6: 'Cozy', 7: 'Forest', 8: 'Pastel colors', 9: 'Wake up', 10: 'Bedtime', 11: 'Warm white', 12: 'Daylight', 13: 'Cool white', 14: 'Night light', 15: 'Focus', 16: 'Relax', 17: 'True colors', 18: 'TV time', 19: 'Plant growth', 20: 'Spring', 21: 'Summer', 22: 'Fall', 23: 'Deep dive', 24: 'Jungle', 25: 'Mojito', 26: 'Club', 27: 'Christmas', 28: 'Halloween', 29: 'Candlelight', 30: 'Golden white', 31: 'Pulse', 32: 'Steampunk', 33: 'Diwali', 34: 'White', 35: 'Alarm', 36: 'Snowy sky', 40: 'Dim-to-warm', 1000: 'Rhythm'});
const encoder = new TextEncoder();
export const utf8Length = value => encoder.encode(value).length;
export const escapeHTML = value => String(value).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
export const integer = (n, min, max) => Number.isSafeInteger(n) && n >= min && n <= max;
export function decimal(value) {
  if (typeof value !== 'string' || !/^(0|[1-9][0-9]{0,19})$/.test(value) || BigInt(value) > 18446744073709551615n) throw Error('Invalid gateway clock or identity.');
  return value;
}
export function nameValid(name) {return typeof name === 'string' && name.trim().length > 0 && utf8Length(name) <= 32 && !/[\u0000-\u001f\u007f]/u.test(name);}
export function indices(mask) {if (!integer(mask, 0, 255)) throw Error('Invalid light selection.'); return Array.from({length: 8}, (_, i) => i).filter(i => mask & (1 << i));}
export function maskFor(selected) {
  if (!selected.length || selected.some(i => !integer(i, 0, 7)) || new Set(selected).size !== selected.length) throw Error('Choose at least one light.');
  return selected.reduce((mask, i) => mask | (1 << i), 0);
}
export function readRoom(value) {
  if (value?.v !== 1 || value.ok !== true || !/^[a-f0-9]{16}$/.test(value.boot) ||
      !integer(value.enabled, 0, 255) || !integer(value.frozen, 0, 255) || !integer(value.owner_held, 0, 255)) throw Error('The gateway returned an invalid room.');
  for (const key of ['now_ms', 'policy_epoch', 'guest_epoch', 'last_sequence']) decimal(value[key]);
  return {...value};
}
export function readLight(value) {
  const c = value?.capabilities;
  if (value?.v !== 1 || value.ok !== true || !integer(value.index, 0, 7) || !nameValid(value.name) ||
      !integer(value.revision, 0, 0xffffffff) || !/^[a-f0-9]{12}$/.test(value.mac) ||
      typeof value.observed_valid !== 'boolean' || typeof value.pending !== 'boolean' ||
      !c || !integer(c.features, 0, 63) || !Array.isArray(c.scenes) || c.scenes.length > 40 ||
      c.scenes.some(n => !integer(n, 0, 65535)) || !integer(c.brightness_min, 0, 100) ||
      !integer(c.kelvin_min, 0, 10000) || !integer(c.kelvin_max, 0, 10000)) throw Error('The gateway returned an invalid light.');
  decimal(value.observed_ms);
  const reported = value.reported;
  if (!reported || typeof reported !== 'object' || Array.isArray(reported)) throw Error('Invalid reported light state.');
  for (const [key, item] of Object.entries(reported)) {
    if (!Object.hasOwn(FEATURES, key)) throw Error('Unsupported reported state.');
    if (key === 'power' ? typeof item !== 'boolean' : key === 'rgb' ? !Array.isArray(item) || item.length !== 3 || item.some(n => !integer(n, 0, 255)) : !integer(item, 0, 65535)) throw Error('Invalid reported light state.');
  }
  return {...value, capabilities: {...c, scenes: [...new Set(c.scenes)]}, reported: {...reported}};
}
export function commonCapabilities(lights) {
  if (!lights.length) return {features: 0, scenes: [], brightness_min: 1, kelvin_min: 0, kelvin_max: 0};
  const first = lights[0].capabilities;
  const caps = {features: lights.reduce((f, light) => f & light.capabilities.features, 63),
    scenes: first.scenes.filter(id => lights.every(light => light.capabilities.scenes.includes(id))),
    brightness_min: Math.max(1, ...lights.map(light => light.capabilities.brightness_min)),
    kelvin_min: Math.max(...lights.map(light => light.capabilities.kelvin_min)),
    kelvin_max: Math.min(...lights.map(light => light.capabilities.kelvin_max))};
  if (caps.kelvin_min > caps.kelvin_max) caps.features &= ~FEATURES.kelvin;
  if (!caps.scenes.length) caps.features &= ~FEATURES.scene;
  return caps;
}
export function validatePatch(patch, lights) {
  const caps = commonCapabilities(lights), keys = Object.keys(patch);
  if (!keys.length || keys.some(key => !Object.hasOwn(FEATURES, key) || !(caps.features & FEATURES[key]))) throw Error('This setting is not supported by every selected light.');
  if (['rgb', 'kelvin', 'scene'].filter(key => Object.hasOwn(patch, key)).length > 1) throw Error('Choose one color mode at a time.');
  for (const [key, value] of Object.entries(patch)) {
    const valid = key === 'power' ? typeof value === 'boolean'
      : key === 'brightness' ? integer(value, caps.brightness_min, 100)
      : key === 'kelvin' ? integer(value, caps.kelvin_min, caps.kelvin_max)
      : key === 'scene' ? caps.scenes.includes(value)
      : key === 'speed' ? integer(value, 10, 200)
      : Array.isArray(value) && value.length === 3 && value.every(n => integer(n, 0, 255));
    if (!valid) throw Error('That value is outside this light’s supported range.');
  }
  return structuredClone(patch);
}
export function mixedValue(lights, key) {
  const values = lights.map(light => light.observed_valid ? light.reported[key] : undefined);
  if (!values.length || values.some(value => value === undefined)) return {kind: 'unknown'};
  return values.every(value => JSON.stringify(value) === JSON.stringify(values[0])) ? {kind: 'same', value: values[0]} : {kind: 'mixed'};
}
export function restriction(room, role, selected) {
  if (!room) return 'Connect to the room to change its lights.';
  if (room.storage_healthy === false) return 'The gateway needs owner maintenance. Changes are paused.';
  const mask = selected.length ? maskFor(selected) : 0;
  if (role === 'guest' && (room.frozen & mask)) return 'The owner has paused guest controls for this selection.';
  if (role === 'guest' && (room.owner_held & mask)) return 'The owner is keeping these settings for a little while.';
  return '';
}
export function hexRGB(value) {return '#' + value.map(n => n.toString(16).padStart(2, '0')).join('');}
export function parseRGB(value) {
  if (!/^#[0-9a-f]{6}$/i.test(value)) throw Error('Choose a valid color.');
  return [1, 3, 5].map(i => parseInt(value.slice(i, i + 2), 16));
}
export function makeCommand(room, lights, selected, patch, id, elapsed = 0, hold = 0) {
  readRoom(room);
  if (!/^[a-f0-9]{32}$/.test(id) || !integer(hold, 0, 60000) || !integer(elapsed, 0, 1000)) throw Error('Refresh the room before sending this change.');
  const targets = maskFor(selected), chosen = selected.map(i => lights.find(light => light.index === i));
  if (targets & ~room.enabled || chosen.some(light => !light)) throw Error('A selected light is no longer enrolled.');
  validatePatch(patch, chosen);
  const expected = Array(8).fill(0); chosen.forEach(light => {expected[light.index] = light.revision;});
  const sequence = BigInt(room.last_sequence) + 1n;
  if (sequence > 18446744073709551615n) throw Error('Open a new session before changing lights.');
  return {v: 1, op: 'set', id, boot: room.boot, policy_epoch: room.policy_epoch, sequence: String(sequence),
    expires_ms: String(BigInt(room.now_ms) + BigInt(elapsed) + 1500n), targets, expected, patch,
    ...(hold ? {hold_ms: hold} : {})};
}
export function receiptText(receipt, lights = []) {
  const names = mask => lights.filter(light => mask & (1 << light.index)).map(light => light.name).join(', ');
  if (receipt.result === 'preparing') return 'Reading current settings before sending this change…';
  if (receipt.result === 'verified') return 'The lights reported the new settings.';
  if (receipt.result === 'partial') return `${names(receipt.verified) || 'Some lights'} updated. ${names(receipt.failed) || 'Other lights'} did not confirm.`;
  if (receipt.result === 'queued' || receipt.result === 'sent') return 'Change sent. Waiting for the lights to report back…';
  if (receipt.result === 'superseded') return 'A newer authorized change took precedence. Refresh to see it.';
  if (receipt.result === 'rejected') return 'The gateway rejected this change. Refresh the room and review access.';
  return 'This change did not confirm. Read the current state before trying again.';
}
export function readReceipt(value, expectedID) {
  if (value?.v !== 1 || value.ok !== true || value.id !== expectedID ||
      !['queued', 'sent', 'verified', 'partial', 'unknown', 'rejected', 'superseded', 'timed_out'].includes(value.result) ||
      !['sent', 'verified', 'failed'].every(key => integer(value[key], 0, 255)) || (value.verified & value.failed)) throw Error('The gateway returned an invalid change receipt.');
  return {...value};
}
export function applicationMessage(code) {
  return ({frozen: 'The owner has paused guest controls.', owner_held: 'The owner’s settings currently take precedence.',
    stale: 'These settings changed elsewhere. The latest state has been refreshed.', stale_revision: 'These settings changed elsewhere. Refresh and try again.',
    revoked: 'This guest session was revoked. It will not rejoin automatically.', unauthorized: 'This access is no longer authorized.',
    expired: 'This session or change expired. Reconnect before trying again.', storage_maintenance: 'The gateway needs owner maintenance before changes can continue.',
    discovery_busy: 'The gateway is already discovering lights. Check the results in a moment.',
    discovery_required: 'Discover lights again before adopting this candidate.', not_found: 'This item is no longer available.',
    busy: 'The room is handling another request. Wait a moment and try again.'})[code] || `The gateway did not accept this action (${String(code).slice(0, 64)}).`;
}
