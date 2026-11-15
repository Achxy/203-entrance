import {readRoom, readLight, readReceipt, indices, maskFor, makeCommand, applicationMessage, restriction} from './model.mjs';
export class ApplicationError extends Error {constructor(code) {super(applicationMessage(code)); this.code = code; this.application = true;}}
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const randomID = () => Array.from(crypto.getRandomValues(new Uint8Array(16)), n => n.toString(16).padStart(2, '0')).join('');
export class GatewayClient {
  constructor(session, {pace = 450, sleep = wait, clock = () => performance.now(), id = randomID} = {}) {
    this.session = session; this.role = session.role; this.pace = pace; this.sleep = sleep; this.clock = clock; this.id = id;
    this.room = null; this.lights = []; this.sessions = []; this.candidates = []; this.lastSent = -Infinity; this.roomAt = 0;
    this.busy = false; this.closed = false; this.preview = false; this.lastCommand = null;
  }
  async request(request) {
    if (this.closed) throw Error('The secure connection is closed. Reconnect to continue.');
    await this.sleep(Math.max(0, this.pace - (this.clock() - this.lastSent)));
    if (this.closed) throw Error('The secure connection is closed.');
    this.lastSent = this.clock();
    let response;
    try {response = await this.session.request(request);} catch (error) {this.close(); throw error;}
    if (this.closed) throw Error('The secure connection closed while this action was pending.');
    if (response?.v !== 1 || typeof response.ok !== 'boolean' || typeof response.code !== 'string') {this.close(); throw Error('Invalid gateway reply. Reconnect to authenticate again.');}
    if (!response.ok) throw new ApplicationError(response.code);
    return response;
  }
  async exclusive(work) {
    if (this.busy) throw new ApplicationError('busy');
    this.busy = true;
    try {return await work();} finally {this.busy = false;}
  }
  async roomPage() {this.room = readRoom(await this.request({v: 1, op: 'room'})); this.roomAt = this.clock(); return this.room;}
  async lightPages(selected) {
    for (const index of selected) {
      const light = readLight(await this.request({v: 1, op: 'light', index}));
      this.lights = [...this.lights.filter(item => item.index !== index), light].sort((a, b) => a.index - b.index);
    }
    this.lights = this.lights.filter(light => this.room.enabled & (1 << light.index));
  }
  async refresh() {return this.exclusive(async () => {await this.roomPage(); await this.lightPages(indices(this.room.enabled)); return this;});}
  async poll() {return this.exclusive(async () => {
    await this.roomPage(); const enabled = indices(this.room.enabled);
    if (enabled.length) {this.pollIndex = ((this.pollIndex ?? -1) + 1) % enabled.length; await this.lightPages([enabled[this.pollIndex]]);}
    return this;
  });}
  async change(selected, patch, {hold = 0, onReceipt = () => {}} = {}) {
    return this.exclusive(async () => {
      await this.roomPage();
      const blocked = restriction(this.room, this.role, selected); if (blocked) throw new ApplicationError(this.room.storage_healthy === false ? 'storage_maintenance' : this.room.frozen & maskFor(selected) ? 'frozen' : 'owner_held');
      await this.lightPages(selected); await this.roomPage();
      const command = makeCommand(this.room, this.lights, selected, patch, this.id(), Math.floor(this.clock() - this.roomAt), hold);
      this.lastCommand = command.id;
      let receipt = this.receipt(await this.request(command), command.id); onReceipt(receipt);
      for (let attempt = 0; ['queued', 'sent'].includes(receipt.result) && attempt < 5; attempt++) {
        await this.sleep(750); receipt = this.receipt(await this.request({v: 1, op: 'command', id: command.id}), command.id); onReceipt(receipt);
      }
      await this.roomPage(); await this.lightPages(selected.filter(index => this.room.enabled & (1 << index)));
      return receipt;
    });
  }
  receipt(value, id) {try {return readReceipt(value, id);} catch (error) {this.close(); throw error;}}
  async reconcile(id) {return this.exclusive(async () => {const receipt = this.receipt(await this.request({v: 1, op: 'command', id}), id); await this.roomPage(); await this.lightPages(indices(this.room.enabled)); return receipt;});}
  async owner(op, fields = {}) {
    if (this.role !== 'owner') throw new ApplicationError('unauthorized');
    return this.exclusive(async () => {const result = await this.request({v: 1, op, ...fields}); await this.roomPage(); return result;});
  }
  async guestPages() {
    if (this.role !== 'owner') throw new ApplicationError('unauthorized');
    return this.exclusive(async () => {
      this.sessions = [];
      for (let page = 0; page < 16; page++) {const entry = await this.request({v: 1, op: 'sessions', page}); if (entry.present) this.sessions.push(entry);}
      return this.sessions;
    });
  }
  async discoverPages(start = true) {
    if (this.role !== 'owner') throw new ApplicationError('unauthorized');
    return this.exclusive(async () => {
      if (start) await this.request({v: 1, op: 'discover'});
      this.candidates = []; this.discovering = true;
      for (let index = 0; index < 8; index++) {
        const entry = await this.request({v: 1, op: 'candidate', index});
        this.discovering = entry.discovering;
        if (entry.present) this.candidates.push({...entry, index});
        if (!entry.discovering && index + 1 >= entry.count) break;
      }
      return this.candidates;
    });
  }
  close() {this.closed = true; this.session?.close(); this.session = null;}
}
