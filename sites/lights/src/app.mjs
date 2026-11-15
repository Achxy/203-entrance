import {ROOM_DESCRIPTOR} from './config.mjs';
import {GatewayClient} from './gateway.mjs';
import {PreviewClient} from './preview.mjs';
import {extractWiZHomeKey} from './wiz-import.mjs';
import {FEATURES, SCENES, escapeHTML as h, nameValid, utf8Length, indices, maskFor,
  commonCapabilities, mixedValue, restriction, parseRGB, hexRGB, receiptText} from './model.mjs';

const $ = selector => document.querySelector(selector), $$ = selector => [...document.querySelectorAll(selector)];
const query = new URLSearchParams(location.search), preview = query.get('preview') === '1';
const state = {client: null, descriptor: ROOM_DESCRIPTOR, auth: null, selected: [], groups: [], group: 'all',
  mode: 'white', tab: 'lights', busy: false, connecting: false, denied: false, draft: null, draftTimer: null,
  lastWrite: -Infinity, lastCommand: null, generation: 0};
const bulb = '<svg viewBox="0 0 24 30" aria-hidden="true"><path d="M7 21c0-4-5-5-5-11a10 10 0 0 1 20 0c0 6-5 7-5 11H7Z"/><path d="M8 25h8m-6 4h4"/></svg>';
const colors = [['amber', '#ffb45f'], ['coral', '#ff6860'], ['rose', '#ff9bbd'], ['lilac', '#bc9aff'], ['blue', '#70b3ff'], ['mint', '#80e3bd']];
$('#swatches').innerHTML = colors.map(([name, value]) => `<button class="swatch swatch-${name}" data-color="${value}" aria-label="Set ${name}" title="${name}"></button>`).join('');
$('#identity-name').textContent = ROOM_DESCRIPTOR.gateway_id;
let securityModule;
const security = () => securityModule ??= import('./security/gateway-web-session.mjs');
const canOperate = () => !!state.client && !state.client.closed && !state.denied;
const selectedLights = () => state.client?.lights.filter(light => state.selected.includes(light.index)) ?? [];
function notice(message, tone = 'info') {const element = $('#notice'); element.textContent = message; element.dataset.tone = tone; element.hidden = !message;}
function cancelDraft() {clearTimeout(state.draftTimer); state.draft = null;}
function errorMessage(error) {return error instanceof Error ? error.message : 'This action could not finish. Please try again.';}
function cleanSelection() {
  const enabled = state.client?.room?.enabled ?? 0;
  state.selected = state.selected.filter(index => enabled & (1 << index));
  if (state.group === 'all' && state.selected.join() !== indices(enabled).join()) state.group = '';
  state.groups = state.groups.map(group => ({...group, indices: group.indices.filter(index => enabled & (1 << index))})).filter(group => group.indices.length);
}
function selectionTitle() {
  if (state.group === 'all') return 'Room lighting';
  const group = state.groups.find(item => item.id === state.group);
  return group?.name ?? (state.selected.length === 1 ? selectedLights()[0]?.name ?? 'Selected light' : 'Your selection');
}
function render() {
  const client = state.client, connected = !!client, operational = canOperate(), owner = client?.role === 'owner';
  $('#connect-panel').hidden = connected; $('#room-panel').hidden = !connected; $('#logout').hidden = !connected;
  $('#preview-banner').hidden = !preview;
  $('#connect-button').disabled = state.connecting; $('#connect-button').textContent = state.connecting ? 'Verifying room identity…' : 'Connect to the room ↗';
  $$('#connect-form input').forEach(input => {input.disabled = state.connecting;});
  const connection = $('#connection-status');
  const label = preview ? (client?.closed ? 'Preview · offline' : 'Preview') : state.connecting ? 'Connecting…' : client?.closed ? 'Connection lost' : client ? 'Connected securely' : 'Not connected';
  connection.replaceChildren(Object.assign(document.createElement('span'), {className: 'status-dot'}), document.createTextNode(label));
  connection.dataset.state = preview ? 'preview' : operational ? 'connected' : 'offline';
  $$('[data-owner]').forEach(element => {element.hidden = !owner;});
  if (!connected) return;
  cleanSelection();
  $('#room-summary').textContent = `${client.lights.length} ${client.lights.length === 1 ? 'light' : 'lights'} · ${owner ? 'Owner access' : 'Guest access'}${state.auth?.name ? ' · ' + state.auth.name : ''}`;
  $('#reconnect').hidden = !client.closed || preview || state.denied; $('#reconnect').disabled = state.connecting;
  $('#refresh').disabled = state.busy || !operational;
  $('#check-change').disabled = state.busy || !operational;
  $$('.tab-panel').forEach(panel => {panel.hidden = panel.id !== state.tab + '-tab';});
  $$('[data-tab]').forEach(button => {if (button.dataset.tab === state.tab) button.setAttribute('aria-current', 'page'); else button.removeAttribute('aria-current');});
  renderLights(); renderControls();
  if (owner) renderOwner();
}
function renderLights() {
  const client = state.client;
  $('#light-count').textContent = `${client.lights.length} enrolled`;
  $('#groups').innerHTML = `<button data-group="all" aria-pressed="${state.group === 'all'}">All lights</button>` + state.groups.map(group => `<button data-group="${h(group.id)}" aria-pressed="${state.group === group.id}">${h(group.name)}</button>`).join('');
  $('#light-list').innerHTML = client.lights.length ? client.lights.map(light => {
    const report = light.reported, mode = report.scene ? SCENES[report.scene] ?? `Scene ${report.scene}` : report.kelvin ? `${report.kelvin} K` : report.rgb ? 'Color' : 'No color reported';
    const detail = light.pending ? 'A change is pending' : !light.observed_valid ? 'No confirmed state' : `${report.brightness === undefined ? 'Brightness unknown' : report.brightness + '%'} · ${mode}`;
    return `<article class="light-row"><button class="light-pick" data-light="${light.index}" aria-pressed="${state.selected.length === 1 && state.selected[0] === light.index}"><span class="bulb-glyph ${light.observed_valid && report.power ? 'is-on' : ''}">${bulb}</span><span class="light-meta"><strong>${h(light.name)}</strong><span>${h(detail)}</span></span><span class="light-state">${!light.observed_valid ? '—' : report.power === true ? 'On' : report.power === false ? 'Off' : '—'}</span></button><label class="light-check"><input type="checkbox" data-select-light="${light.index}" ${state.selected.includes(light.index) ? 'checked' : ''}><span class="sr-only">Select ${h(light.name)}</span></label></article>`;
  }).join('') : `<p class="empty-state">No lights are enrolled yet.${client.role === 'owner' ? ' Open Room setup to find your first one.' : ' The owner can add lights in Room setup.'}</p>`;
  $('#manage-groups').disabled = !client.lights.length;
}
function setRange(id, outputID, mixed, fallback, suffix = '') {
  const input = $(id), output = $(outputID), value = mixed.kind === 'same' ? mixed.value : fallback;
  if (document.activeElement !== input) input.value = String(value);
  output.textContent = mixed.kind === 'same' ? `${mixed.value}${suffix}` : mixed.kind === 'mixed' ? 'Mixed' : 'Not reported';
  input.setAttribute('aria-valuetext', output.textContent);
}
function renderControls() {
  const lights = selectedLights(), caps = commonCapabilities(lights), blocked = restriction(state.client.room, state.client.role, state.selected);
  const disabled = !canOperate() || !lights.length || !!blocked || state.busy;
  $('#selection-title').textContent = selectionTitle(); $('#selection-count').textContent = `${lights.length} ${lights.length === 1 ? 'LIGHT' : 'LIGHTS'} SELECTED`;
  $('#restriction').textContent = !canOperate() ? 'The connection is paused. Reconnect before changing lights.' : blocked; $('#restriction').hidden = !$('#restriction').textContent;
  const power = mixedValue(lights, 'power');
  $$('[data-patch-power]').forEach(button => {button.disabled = disabled || !(caps.features & FEATURES.power); button.setAttribute('aria-pressed', String(power.kind === 'same' && power.value === (button.dataset.patchPower === 'true')));});
  $('#selection-state').textContent = power.kind === 'mixed' ? 'Mixed power' : power.kind === 'same' ? power.value ? 'On' : 'Off' : 'State not reported';
  $('#brightness-control').hidden = !(caps.features & FEATURES.brightness); $('#brightness').min = String(caps.brightness_min);
  setRange('#brightness', '#brightness-value', mixedValue(lights, 'brightness'), 50, '%');
  const available = {white: !!(caps.features & FEATURES.kelvin), color: !!(caps.features & FEATURES.rgb), scene: !!(caps.features & FEATURES.scene)};
  if (!available[state.mode]) state.mode = Object.keys(available).find(mode => available[mode]) ?? 'white';
  $$('[data-mode]').forEach(button => {button.disabled = !available[button.dataset.mode]; button.setAttribute('aria-pressed', String(button.dataset.mode === state.mode));});
  for (const mode of ['white', 'color', 'scene']) $('#' + mode + '-mode').hidden = mode !== state.mode || !available[mode];
  $('#mode-unavailable').hidden = Object.values(available).some(Boolean);
  $('#temperature').min = String(caps.kelvin_min); $('#temperature').max = String(caps.kelvin_max);
  setRange('#temperature', '#temperature-value', mixedValue(lights, 'kelvin'), Math.max(caps.kelvin_min, Math.min(caps.kelvin_max, 4000)), ' K');
  $$('[data-kelvin]').forEach(button => {button.disabled = disabled || Number(button.dataset.kelvin) < caps.kelvin_min || Number(button.dataset.kelvin) > caps.kelvin_max;});
  const rgb = mixedValue(lights, 'rgb');
  if (rgb.kind === 'same') {$('#color').value = hexRGB(rgb.value); $('#color-value').textContent = hexRGB(rgb.value);} else $('#color-value').textContent = rgb.kind === 'mixed' ? 'Mixed' : 'Choose color';
  const scene = mixedValue(lights, 'scene');
  $('#scene-list').innerHTML = caps.scenes.map(id => `<button data-scene="${id}" aria-pressed="${scene.kind === 'same' && scene.value === id}" ${disabled ? 'disabled' : ''}>${h(SCENES[id] ?? `Scene ${id}`)}</button>`).join('');
  $('#speed-control').hidden = !(caps.features & FEATURES.speed); setRange('#speed', '#speed-value', mixedValue(lights, 'speed'), 100);
  for (const id of ['brightness', 'temperature', 'color', 'speed']) $('#' + id).disabled = disabled;
  $$('[data-color]').forEach(button => {button.disabled = disabled;});
  $('#hold-settings').disabled = disabled;
  $('#observed-label').textContent = preview ? 'Sample state · no real lights' : lights.some(light => !light.observed_valid) ? 'Some states are unconfirmed' : 'Last reported by the lights';
}
function renderOwner() {
  const client = state.client, disabled = !canOperate() || state.busy;
  $('#freeze-state').textContent = client.room.frozen ? 'Our guest controls are paused for some or all lights.' : 'Guests can adjust the room through our app and site.';
  for (const id of ['freeze', 'unfreeze', 'revoke-all', 'refresh-sessions', 'discover', 'check-discovery', 'cancel-discovery']) $('#' + id).disabled = disabled;
  $('#freeze').disabled ||= !client.room.enabled; $('#unfreeze').disabled ||= !client.room.frozen;
  $$('#password-form input, #password-form button, #home-key-form input, #home-key-form button').forEach(element => {element.disabled = disabled;});
  $('#session-list').innerHTML = client.sessions.length ? client.sessions.map(session => `<div class="session-row"><div><strong>${h(session.name || 'Guest')}</strong><p>${session.revoked ? 'Revoked' : session.expired ? 'Expired' : 'Authorized guest'} · Self-reported name</p></div><button class="quiet small" data-revoke="${h(session.grant)}" ${disabled || session.revoked || session.expired ? 'disabled' : ''}>Revoke</button></div>`).join('') : '<p class="muted">No sessions loaded. Refresh to read the current guest list.</p>';
  $('#enrolled-list').innerHTML = client.lights.length ? client.lights.map(light => `<div class="enrolled-row"><div><strong>${h(light.name)}</strong><p>${h(light.capabilities.module || 'Unknown module')} · ${light.legacy_control ? 'Legacy control allowed' : 'Requires signed commands from our gateway'}</p><p class="mono">${h(light.mac)}</p></div><button class="quiet small danger" data-remove="${light.index}" ${disabled ? 'disabled' : ''}>Remove</button></div>`).join('') : '<p class="muted">No enrolled lights.</p>';
  $('#candidate-list').innerHTML = client.candidates.map(candidate => `<section class="settings-card candidate-card"><h3>${h(candidate.module || 'New WiZ candidate')}</h3><p class="mono">${h(candidate.mac)} · ${h(candidate.firmware || 'Firmware unknown')}</p><form data-adopt="${candidate.index}"><label class="field">Name this light<input name="light-name" maxlength="32" required placeholder="e.g. Desk" ${disabled || !candidate.identified ? 'disabled' : ''}></label><button class="primary" type="submit" ${disabled || !candidate.identified || client.discovering ? 'disabled' : ''}>Adopt light</button><details><summary>Legacy compatibility</summary><label class="check-row"><input name="legacy" type="checkbox">Allow legacy unsigned control<span class="field-note">Devices on the room Wi-Fi may bypass guest limits. Leave this off when verified controls are available.</span></label></details></form></section>`).join('');
}
async function perform(work, {message = '', quiet = false} = {}) {
  if (state.busy || !state.client) return;
  const generation = state.generation; state.busy = true; if (!quiet) notice(''); render();
  try {const result = await work(); if (generation === state.generation && message) notice(message); return result;}
  catch (error) {
    if (generation !== state.generation) return;
    if (['revoked', 'unauthorized', 'expired'].includes(error.code)) {state.denied = true; state.client.close(); cancelDraft();}
    if (!quiet || state.client?.closed || state.denied) notice(errorMessage(error), 'error');
  } finally {if (generation === state.generation) {state.busy = false; render(); flushDraft();}}
}
function showReceipt(receipt) {
  const panel = $('#change-status'); panel.hidden = false; panel.dataset.state = receipt.result;
  $('#change-title').textContent = ({preparing: 'Preparing change', queued: 'Change pending', sent: 'Waiting for the lights', verified: 'Settings reported', partial: 'Some lights did not confirm', unknown: 'Outcome unknown', timed_out: 'No confirmation', rejected: 'Change not accepted', superseded: 'Newer settings took precedence'})[receipt.result] ?? 'Change status';
  $('#change-description').textContent = receiptText(receipt, state.client?.lights ?? []);
  $('#check-change').hidden = !state.lastCommand || ['verified', 'rejected', 'superseded'].includes(receipt.result);
}
function queuePatch(patch) {
  if (!canOperate() || !state.selected.length || restriction(state.client.room, state.client.role, state.selected)) return;
  const selected = [...state.selected], previous = state.draft?.selected.join() === selected.join() ? state.draft.patch : {};
  const combined = {...previous, ...patch};
  for (const mode of ['rgb', 'kelvin', 'scene']) if (Object.hasOwn(patch, mode)) for (const other of ['rgb', 'kelvin', 'scene']) if (other !== mode) delete combined[other];
  state.draft = {selected, patch: combined}; flushDraft();
}
function flushDraft() {
  clearTimeout(state.draftTimer);
  if (!state.draft || state.busy || !canOperate() || document.hidden || state.tab !== 'lights') return;
  const delay = Math.max(100, 500 - (performance.now() - state.lastWrite));
  state.draftTimer = setTimeout(async () => {
    const draft = state.draft; state.draft = null;
    if (!draft || state.busy || draft.selected.join() !== state.selected.join() || !canOperate()) return;
    state.lastWrite = performance.now(); showReceipt({result: 'preparing'});
    await perform(async () => {
      const previous = state.client.lastCommand;
      try {return await state.client.change(draft.selected, draft.patch, {hold: state.client.role === 'owner' && $('#hold-settings').checked ? 30000 : 0, onReceipt: receipt => {state.lastCommand = state.client.lastCommand; showReceipt(receipt);}});}
      catch (error) {if (state.client.lastCommand !== previous) {state.lastCommand = state.client.lastCommand; showReceipt({result: error.application ? 'rejected' : 'unknown'});} else $('#change-status').hidden = true; throw error;}
    });
  }, delay);
}
async function confirmAction(title, description, action = 'Continue') {
  const dialog = $('#confirm-dialog'); $('#confirm-title').textContent = title; $('#confirm-description').textContent = description; $('#confirm-action').textContent = action;
  dialog.returnValue = ''; dialog.showModal();
  return new Promise(resolve => {dialog.addEventListener('close', () => resolve(dialog.returnValue === 'confirm'), {once: true});});
}
function logout() {
  state.generation++; state.client?.close(); state.client = null; state.auth = null; state.groups = []; state.selected = []; state.lastCommand = null;
  state.busy = false; state.connecting = false; state.denied = false; state.tab = 'lights'; cancelDraft();
  $$('input[type=password]').forEach(input => {input.value = '';}); $('#guest-name').value = ''; $('#change-status').hidden = true;
  notice(''); $('#connect-feedback').textContent = ''; render(); $('#credential').focus();
}
async function connect(auth, reconnect = false) {
  if (state.connecting) return;
  state.connecting = true; state.denied = false; $('#connect-feedback').textContent = ''; const generation = ++state.generation; render();
  let session;
  try {
    const {GatewayWebSession, randomHex} = await security();
    session = await GatewayWebSession.connect({descriptor: state.descriptor, role: auth.role, credential: auth.credential,
      onState: phase => {
        if (phase === 'closed' && generation === state.generation && state.client?.session === session) {
          state.client.closed = true; cancelDraft(); notice('The secure connection closed. Reconnect to read the current room state.', 'error'); render();
        }
      }});
    if (generation !== state.generation) {session.close(); return;}
    if (auth.role === 'guest') {
      if (!reconnect) auth.token = randomHex(32);
      const response = await session.request({v: 1, op: reconnect ? 'guest.resume' : 'guest.enroll', token: auth.token, ...(!reconnect ? {name: auth.name} : {})});
      if (!response.ok) throw Object.assign(Error(`Guest access was not accepted (${response.code}). No automatic re-enrollment was attempted.`), {code: response.code});
    }
    if (generation !== state.generation) {session.close(); return;}
    state.client?.close(); state.auth = auth; state.client = new GatewayClient(session); state.selected = []; state.group = 'all';
    state.busy = true; render(); await state.client.refresh();
    if (generation === state.generation) {state.selected = indices(state.client.room.enabled); notice(reconnect ? 'Reconnected. Previous changes were not replayed.' : '');}
  } catch (error) {
    session?.close();
    if (generation === state.generation) {
      $('#connect-feedback').textContent = errorMessage(error); notice(errorMessage(error), 'error');
      if (['revoked', 'unauthorized', 'expired'].includes(error.code)) state.denied = true;
      state.client?.close();
    }
  } finally {if (generation === state.generation) {state.connecting = false; state.busy = false; render();}}
}

$('#connect-form').addEventListener('submit', event => {
  event.preventDefault(); const role = $('#connect-form input[name=role]:checked').value, name = $('#guest-name').value.trim(), credential = $('#credential').value;
  if (role === 'guest' && !nameValid(name)) {$('#connect-feedback').textContent = 'Use a name between 1 and 32 UTF-8 bytes.'; return;}
  if (!credential || utf8Length(credential) > 256) {$('#connect-feedback').textContent = 'Enter a valid access credential.'; return;}
  $('#credential').value = ''; connect({role, name: role === 'owner' ? '' : name, credential});
});
$$('#connect-form input[name=role]').forEach(input => input.addEventListener('change', () => {
  const owner = input.value === 'owner' && input.checked; $('#name-field').hidden = owner; $('#guest-name').required = !owner;
  $('#credential-label').textContent = owner ? 'Owner key' : 'Room password'; $('#credential').value = '';
}));
$('#descriptor-file').addEventListener('change', async event => {
  try {
    const file = event.target.files?.[0]; if (!file) return;
    if (file.size > 4096) throw Error('This room file is too large.');
    const {parseTrustedDescriptor} = await security(), descriptor = parseTrustedDescriptor(await file.text());
    if (descriptor.relay_url !== ROOM_DESCRIPTOR.relay_url) throw Error('This app accepts the Room 203 relay only.');
    state.descriptor = descriptor; $('#identity-name').textContent = descriptor.gateway_id; $('#descriptor-feedback').textContent = 'Owner-issued room identity loaded for this tab.';
  } catch (error) {$('#descriptor-feedback').textContent = errorMessage(error);} finally {event.target.value = '';}
});
$('#logout').addEventListener('click', logout);
$('#reconnect').addEventListener('click', () => {if (state.auth) connect(state.auth, true);});
$('#theme-toggle').addEventListener('click', () => {const current = document.documentElement.dataset.theme ?? (matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'); document.documentElement.dataset.theme = current === 'dark' ? 'light' : 'dark';});
$('#refresh').addEventListener('click', () => perform(() => state.client.refresh(), {message: 'The room state was refreshed.'}));
$$('[data-tab]').forEach(button => button.addEventListener('click', () => {if (button.dataset.tab !== 'lights' && state.client?.role !== 'owner') return; state.tab = button.dataset.tab; cancelDraft(); render();}));
$('#groups').addEventListener('click', event => {
  const button = event.target.closest('[data-group]'); if (!button) return; state.group = button.dataset.group;
  state.selected = state.group === 'all' ? indices(state.client.room.enabled) : [...state.groups.find(group => group.id === state.group).indices]; cancelDraft(); render();
});
$('#light-list').addEventListener('click', event => {const button = event.target.closest('[data-light]'); if (!button) return; state.selected = [Number(button.dataset.light)]; state.group = ''; cancelDraft(); render();});
$('#light-list').addEventListener('change', event => {
  const input = event.target.closest('[data-select-light]'); if (!input) return; const index = Number(input.dataset.selectLight);
  state.selected = input.checked ? [...new Set([...state.selected, index])].sort() : state.selected.filter(i => i !== index);
  state.group = ''; cancelDraft(); renderControls(); renderLights();
});
$$('[data-patch-power]').forEach(button => button.addEventListener('click', () => queuePatch({power: button.dataset.patchPower === 'true'})));
for (const [id, key, output, suffix] of [['brightness', 'brightness', 'brightness-value', '%'], ['temperature', 'kelvin', 'temperature-value', ' K'], ['speed', 'speed', 'speed-value', '']]) {
  $('#' + id).addEventListener('input', event => {$('#' + output).textContent = event.target.value + suffix; event.target.setAttribute('aria-valuetext', event.target.value + suffix);});
  $('#' + id).addEventListener('change', event => queuePatch({[key]: Number(event.target.value)}));
}
$$('[data-kelvin]').forEach(button => button.addEventListener('click', () => queuePatch({kelvin: Number(button.dataset.kelvin)})));
$$('[data-mode]').forEach(button => button.addEventListener('click', () => {state.mode = button.dataset.mode; renderControls();}));
$('#color').addEventListener('input', event => {$('#color-value').textContent = event.target.value;});
$('#color').addEventListener('change', event => queuePatch({rgb: parseRGB(event.target.value)}));
$('#swatches').addEventListener('click', event => {const button = event.target.closest('[data-color]'); if (button) queuePatch({rgb: parseRGB(button.dataset.color)});});
$('#scene-list').addEventListener('click', event => {const button = event.target.closest('[data-scene]'); if (button) queuePatch({scene: Number(button.dataset.scene)});});
$('#check-change').addEventListener('click', () => {if (state.lastCommand && canOperate()) perform(async () => {showReceipt(await state.client.reconcile(state.lastCommand));});});
$('#manage-groups').addEventListener('click', () => {$('#group-members').textContent = 'Members: ' + selectedLights().map(light => light.name).join(', '); renderSavedGroups(); $('#group-dialog').showModal();});
function renderSavedGroups() {$('#saved-groups').innerHTML = state.groups.map(group => `<div class="session-row"><span>${h(group.name)} · ${group.indices.length} lights</span><button class="quiet small" data-delete-group="${group.id}">Delete</button></div>`).join('');}
$('#group-form').addEventListener('submit', event => {event.preventDefault(); const name = $('#group-name').value.trim(); if (!nameValid(name) || !state.selected.length) {$('#group-members').textContent = 'Select at least one light and use a name up to 32 UTF-8 bytes.'; return;} if (state.groups.length >= 8) {$('#group-members').textContent = 'This tab already has eight groups. Delete one first.'; return;} state.groups.push({id: crypto.randomUUID(), name, indices: [...state.selected]}); $('#group-name').value = ''; renderSavedGroups(); render();});
$('#saved-groups').addEventListener('click', event => {const button = event.target.closest('[data-delete-group]'); if (!button) return; state.groups = state.groups.filter(group => group.id !== button.dataset.deleteGroup); if (state.group === button.dataset.deleteGroup) state.group = ''; renderSavedGroups(); render();});
$('#freeze').addEventListener('click', () => perform(() => state.client.owner('freeze', {targets: state.client.room.enabled, duration_ms: Number($('#freeze-duration').value)}), {message: 'Guest controls in our app and site are paused.'}));
$('#unfreeze').addEventListener('click', () => perform(() => state.client.owner('freeze', {targets: state.client.room.enabled, duration_ms: 0}), {message: 'Guest controls in our app and site resumed.'}));
$('#refresh-sessions').addEventListener('click', () => perform(() => state.client.guestPages()));
$('#session-list').addEventListener('click', async event => {
  const button = event.target.closest('[data-revoke]'); if (!button) return; const entry = state.client.sessions.find(item => item.grant === button.dataset.revoke);
  if (await confirmAction(`Revoke ${entry?.name || 'this guest'}?`, 'This ends their current grant. Someone who still knows the shared password can explicitly join again.', 'Revoke session')) perform(async () => {await state.client.owner('revoke', {grant: button.dataset.revoke}); await state.client.guestPages();}, {message: 'Guest session revoked.'});
});
$('#revoke-all').addEventListener('click', async () => {if (await confirmAction('Revoke all guests?', 'All guest grants will end. Rotate the password too if it should no longer allow new guests.', 'Revoke all')) perform(async () => {await state.client.owner('revoke_all'); await state.client.guestPages();}, {message: 'All guest sessions were revoked.'});});
$('#password-form').addEventListener('submit', async event => {
  event.preventDefault(); let password = $('#new-password').value; const matches = password === $('#confirm-password').value; $('#new-password').value = ''; $('#confirm-password').value = '';
  if (!matches || utf8Length(password) < 12 || utf8Length(password) > 128) {password = ''; notice('Use matching passwords between 12 and 128 UTF-8 bytes.', 'error'); return;}
  if (!await confirmAction('Change the room password?', 'Every guest session will be revoked. Share the new password only with people you want to invite.', 'Change password')) {password = ''; return;}
  await perform(async () => {const verifier = preview ? {salt: '0'.repeat(32), verifier: '0'.repeat(768)} : await (await security()).deriveGuestVerifier(password); password = ''; await state.client.owner('rotate_guest', verifier); await state.client.guestPages();}, {message: 'Room password changed. Previous guest access ended.'}); password = '';
});
$('#home-key-form').addEventListener('submit', async event => {event.preventDefault(); let key = $('#home-key').value.toLowerCase(); $('#home-key').value = ''; if (!/^[0-9a-f]{32}$/.test(key)) {notice('Enter the WiZ local key as 32 hexadecimal characters.', 'error'); return;} await perform(async () => {await state.client.owner('home_key', {key}); key = '';}, {message: 'WiZ local key imported. Test an enrolled light to verify control.'}); key = '';});
$('#wiz-export-file').addEventListener('change', async event => {
  const generation = state.generation;
  try {
    const file = event.target.files?.[0]; if (!file) return;
    if (file.size > 65536) throw Error('This export is larger than 64 KiB. Paste the intended home’s key instead.');
    const key = extractWiZHomeKey(await file.text());
    if (generation !== state.generation || state.client?.role !== 'owner') return;
    $('#home-key').value = key; $('#wiz-export-feedback').textContent = 'One home key loaded locally. Choose Import securely to send only that key to the gateway.';
  } catch (error) {$('#wiz-export-feedback').textContent = errorMessage(error);} finally {event.target.value = '';}
});
$('#discover').addEventListener('click', () => perform(async () => {await state.client.discoverPages(true); $('#discovery-status').textContent = state.client.discovering ? 'Discovery is still running. Check results in a moment.' : `${state.client.candidates.length} candidate lights found.`;}));
$('#check-discovery').addEventListener('click', () => perform(async () => {await state.client.discoverPages(false); $('#discovery-status').textContent = state.client.discovering ? 'Discovery is still running.' : `${state.client.candidates.length} candidate lights found.`;}));
$('#cancel-discovery').addEventListener('click', () => perform(() => state.client.owner('discovery_cancel'), {message: 'Discovery stopped.'}));
$('#candidate-list').addEventListener('submit', async event => {
  const form = event.target.closest('[data-adopt]'); if (!form) return; event.preventDefault(); const name = form.elements['light-name'].value.trim(), legacy = form.elements.legacy.checked;
  if (!nameValid(name)) {notice('Use a light name up to 32 UTF-8 bytes.', 'error'); return;}
  if (legacy && !await confirmAction('Allow unsigned local control?', 'Other devices on the room Wi-Fi may bypass guest limits for this light. Prefer verified controls when the bulb supports them.', 'Allow legacy control')) return;
  perform(async () => {await state.client.owner('adopt', {candidate: Number(form.dataset.adopt), name, legacy}); await state.client.refresh(); state.client.candidates = [];}, {message: 'Light adopted. Try a setting and check the physical bulb.'});
});
$('#enrolled-list').addEventListener('click', async event => {const button = event.target.closest('[data-remove]'); if (!button) return; const index = Number(button.dataset.remove), light = state.client.lights.find(item => item.index === index); if (await confirmAction(`Remove ${light?.name || 'this light'}?`, 'This removes gateway enrollment. The bulb will not be factory-reset.', 'Remove light')) perform(async () => {await state.client.owner('remove', {index}); await state.client.refresh();}, {message: 'Light removed from the room.'});});
document.addEventListener('visibilitychange', () => {if (document.hidden) cancelDraft();});
window.addEventListener('pagehide', () => {state.generation++; state.client?.close(); state.auth = null; cancelDraft(); $$('input[type=password]').forEach(input => {input.value = '';});});
window.addEventListener('pageshow', event => {if (event.persisted && !state.auth && !preview) logout();});
setInterval(() => {if (!document.hidden && !document.activeElement?.matches('input,select') && canOperate() && !state.busy && !state.draft && !state.connecting && state.tab === 'lights') perform(() => state.client.poll ? state.client.poll() : state.client.refresh(), {quiet: true});}, 7000);

if (preview) {
  state.client = new PreviewClient(query.get('role') === 'guest' ? 'guest' : 'owner', query.get('scenario') ?? 'ready'); state.selected = indices(state.client.room.enabled); render();
  if (state.client.closed) notice('Preview scenario: offline. Settings stay visible, and controls are disabled.');
} else {
  render();
  const statusURL = ROOM_DESCRIPTOR.relay_url.replace(/^wss:/, 'https:') + '/info';
  fetch(statusURL, {cache: 'no-store', credentials: 'omit', redirect: 'error', signal: AbortSignal.timeout(5000)})
    .then(response => {if (!response.ok) throw Error(); return response.json();})
    .then(info => {$('#gateway-status').textContent = info.online ? 'Room gateway available' : 'Room gateway is offline';})
    .catch(() => {$('#gateway-status').textContent = 'Room gateway unavailable';});
}
