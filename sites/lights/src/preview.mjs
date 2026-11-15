import {validatePatch, maskFor, restriction} from './model.mjs';
const pause = () => new Promise(resolve => setTimeout(resolve, 180));
export class PreviewClient {
  constructor(role = 'owner', scenario = 'ready') {
    this.role = role; this.preview = true; this.busy = false; this.closed = scenario === 'offline'; this.lastCommand = null;
    this.room = {v: 1, ok: true, code: 'ok', boot: 'a'.repeat(16), now_ms: '100000', policy_epoch: '1', guest_epoch: '1', last_sequence: '0', enabled: 7, frozen: scenario === 'frozen' ? 7 : 0, owner_held: 0, storage_healthy: true, driver_busy: false};
    const color = {features: 63, brightness_min: 10, kelvin_min: 2200, kelvin_max: 6500, scenes: [1, 3, 5, 7, 8, 15, 16, 18], module: 'Preview color bulb', firmware: 'Preview'};
    this.lights = ['Desk', 'Bedside', 'Ceiling'].map((name, index) => ({v: 1, ok: true, code: 'ok', index, name, mac: '00000000000' + (index + 1), revision: 1,
      observed_valid: !(scenario === 'partial' && index === 2), observed_ms: '100000', pending: false, last_result: 'verified', legacy_control: false,
      reply_trust: 'unverified_lan', permission_enforcement: 'requires_hardware_validation', capabilities: {...color, ...(index === 2 ? {features: 11, kelvin_min: 2700, scenes: []} : {})},
      reported: index === 0 ? {power: true, brightness: 72, kelvin: 4000} : index === 1 ? {power: true, brightness: 35, rgb: [255, 180, 95]} : {power: false, brightness: 60, kelvin: 4000}}));
    this.sessions = [{present: true, grant: '2', name: 'Study buddy', revoked: false, expired: false}, {present: true, grant: '3', name: 'A guest', revoked: false, expired: false}];
    this.candidates = []; this.discovering = false;
  }
  async refresh() {if (this.closed) throw Error('Preview is offline. Choose a different preview scenario.'); await pause(); return this;}
  async change(selected, patch, {onReceipt = () => {}} = {}) {
    if (this.closed) throw Error('Preview is offline.');
    const blocked = restriction(this.room, this.role, selected); if (blocked) throw Error(blocked);
    validatePatch(patch, selected.map(i => this.lights.find(light => light.index === i)));
    this.busy = true;
    try {
      const receipt = {v: 1, ok: true, code: 'ok', id: 'b'.repeat(32), result: 'queued', verified: 0, failed: 0, sent: maskFor(selected)};
      onReceipt(receipt); await pause();
      for (const index of selected) {
        const light = this.lights.find(item => item.index === index);
        if (!light.observed_valid) {receipt.failed |= 1 << index; continue;}
        for (const mode of ['rgb', 'kelvin', 'scene']) if (Object.hasOwn(patch, mode)) for (const other of ['rgb', 'kelvin', 'scene']) delete light.reported[other];
        Object.assign(light.reported, patch); light.revision++; receipt.verified |= 1 << index;
      }
      receipt.result = receipt.failed ? 'partial' : 'verified'; this.lastCommand = receipt.id; onReceipt(receipt); return receipt;
    } finally {this.busy = false;}
  }
  async owner(op, fields = {}) {
    if (this.role !== 'owner' || this.closed) throw Error('Owner access is required.');
    await pause();
    if (op === 'freeze') this.room.frozen = fields.duration_ms ? fields.targets : 0;
    else if (op === 'revoke') this.sessions = this.sessions.map(entry => entry.grant === fields.grant ? {...entry, revoked: true} : entry);
    else if (op === 'revoke_all' || op === 'rotate_guest') this.sessions = this.sessions.map(entry => ({...entry, revoked: true}));
    else if (op === 'remove') {this.lights = this.lights.filter(light => light.index !== fields.index); this.room.enabled &= ~(1 << fields.index);}
    else if (op === 'adopt') {
      const index = Array.from({length: 8}, (_, i) => i).find(i => !(this.room.enabled & (1 << i)));
      if (index === undefined) throw Error('The gateway’s eight light slots are full.');
      this.lights.push({...structuredClone(this.lights[0]), index, name: fields.name, mac: '000000000009', legacy_control: fields.legacy}); this.room.enabled |= 1 << index;
      this.candidates = [];
    }
    return {v: 1, ok: true, code: 'ok'};
  }
  async guestPages() {await pause(); return this.sessions;}
  async discoverPages() {await pause(); this.candidates = [{index: 0, mac: '000000000009', identified: true, module: 'Preview color bulb', firmware: 'Preview'}]; return this.candidates;}
  async reconcile() {return {result: 'verified', verified: this.room.enabled, failed: 0};}
  close() {this.closed = true;}
}
