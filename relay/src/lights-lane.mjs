import {hex} from './protocol.mjs';
import {lightRequest, lightReply, lightForward, LIGHTS_DATA_TTL,
  LIGHTS_HANDSHAKE_TTL, LIGHTS_IDLE_TTL, LIGHTS_SESSION_TTL} from './lights-protocol.mjs';

const now = () => Date.now();
const attachment = ws => ws.deserializeAttachment();
const send = (ws, message) => {try {ws.send(JSON.stringify(message)); return true;} catch {return false;}};
const close = (ws, reason) => {try {ws.close(1008, reason);} catch {}};

// Public identity only. The disabled rollout path uses this without creating
// a Durable Object, lighting lane, socket or admission record.
export function lightInfo(req, env, online = false) {
  const headers = {'Cache-Control': 'no-store', 'Vary': 'Origin'};
  if (req.headers.has('Origin')) headers['Access-Control-Allow-Origin'] = req.headers.get('Origin');
  return Response.json({protocol: 1, online: online === true, gateway_id: env.DEVICE_ID,
    public_key: env.DEVICE_PUBLIC_KEY}, {headers});
}

// This lane admits bounded opaque traffic. Only the gateway can authenticate
// Security2 clients or authorize light operations. Its limits never use the
// arcade admission table, controller credits or game lease.
export class LightsLane {
  constructor(room) {
    this.room = room;
    if (this.enabled()) room.ctx.storage.sql.exec('CREATE TABLE IF NOT EXISTS light_limits (k TEXT PRIMARY KEY, start INTEGER NOT NULL, n INTEGER NOT NULL)');
  }
  enabled() {return this.room.env.LIGHTS_ENABLED === 'true';}
  clients() {return this.room.sockets().filter(ws => attachment(ws)?.kind === 'light');}
  gateway() {
    const ws = this.room.device(), a = ws && attachment(ws);
    return a?.lightProtocol === 1 && now() - a.last < 1500 ? {ws, a} : null;
  }
  info(req) {
    return lightInfo(req, this.room.env, this.enabled() && !!this.gateway());
  }
  rate(key, limit, ms) {
    return this.room.ctx.storage.transactionSync(() => {
      const t = now(), sql = this.room.ctx.storage.sql;
      const old = [...sql.exec('SELECT start,n FROM light_limits WHERE k=?', key)][0];
      if (old && t - old.start < ms && old.n >= limit) return false;
      const start = old && t - old.start < ms ? old.start : t, n = start === old?.start ? old.n + 1 : 1;
      sql.exec('INSERT INTO light_limits(k,start,n) VALUES(?,?,?) ON CONFLICT(k) DO UPDATE SET start=excluded.start,n=excluded.n', key, start, n);
      sql.exec("DELETE FROM light_limits WHERE k IN (SELECT k FROM light_limits WHERE k LIKE 'peer:%' ORDER BY start DESC LIMIT -1 OFFSET 128)");
      return true;
    });
  }
  async connect(peer) {
    this.prune();
    const gateway = this.gateway();
    if (!gateway) return new Response('Lighting gateway offline', {status: 503});
    if (!this.rate('peer:connect:' + peer, 8, 60000) || !this.rate('connect', 24, 60000))
      return new Response('Please retry later', {status: 429});
    const clients = this.clients();
    if (clients.length >= 4 || clients.filter(ws => attachment(ws).role === 'none').length >= 2)
      return new Response('Lighting sessions busy', {status: 429});
    const [client, ws] = Object.values(new WebSocketPair());
    const t = now();
    this.room.ctx.acceptWebSocket(ws);
    ws.serializeAttachment({kind: 'light', id: hex(16), peer, created: t, last: t,
      authUntil: t + LIGHTS_HANDSHAKE_TTL, until: t + LIGHTS_SESSION_TTL,
      boot: gateway.a.boot, role: 'none', pending: null, lastId: 0, rateAt: t, count: 0});
    send(ws, {v: 1, type: 'light_online', online: true, connection: attachment(ws).id,
      boot: gateway.a.boot, protocol: 1});
    await this.schedule(t + LIGHTS_HANDSHAKE_TTL);
    return new Response(null, {status: 101, webSocket: client});
  }
  reject(ws, a, id, error, status = 409) {
    if (id > 0) send(ws, {v: 1, type: 'light_reply', connection: a.id, id, boot: a.boot,
      status, payload: '', error});
    else send(ws, {v: 1, type: 'light_offline', online: false, connection: a.id, boot: a.boot, error});
    // Security2 uses one shared request/response nonce. A rejected transport
    // request must restart SRP, never resume with uncertain cipher state.
    this.end(ws, 'Lighting session ended: ' + error);
  }
  cleanup(ws, notify = true) {
    const a = attachment(ws);
    if (a?.kind !== 'light' || a.cleaned) return;
    a.cleaned = true;
    ws.serializeAttachment(a);
    const device = this.room.device(), d = device && attachment(device);
    if (notify && this.enabled() && d?.lightProtocol === 1 && d.boot === a.boot)
      send(device, {v: 1, type: 'light_disconnect', connection: a.id, boot: a.boot});
  }
  end(ws, reason, notify = true) {
    this.cleanup(ws, notify);
    close(ws, reason);
  }
  prune() {
    const t = now(), gateway = this.gateway();
    for (const ws of this.clients()) {
      const a = attachment(ws);
      if (!this.enabled() || !gateway || a.boot !== gateway.a.boot)
        this.reject(ws, a, a.pending?.id ?? 0, 'offline', 503);
      else if (a.pending && t >= a.pending.deadline)
        this.reject(ws, a, a.pending.id, 'timeout', 504);
      else if (t >= a.until || t - a.last >= LIGHTS_IDLE_TTL || (a.role === 'none' && t >= a.authUntil))
        this.reject(ws, a, a.pending?.id ?? 0, 'expired', 401);
    }
  }
  async schedule(deadline) {
    await this.room.ctx.storage.transaction(async txn => {
      const old = await txn.getAlarm();
      if (old === null || old > deadline) await txn.setAlarm(deadline);
    });
  }
  nextDeadline() {
    let deadline = now() + 15000;
    for (const ws of this.clients()) {
      const a = attachment(ws);
      deadline = Math.min(deadline, a.until, a.last + LIGHTS_IDLE_TTL,
        a.role === 'none' ? a.authUntil : Infinity, a.pending?.deadline ?? Infinity);
    }
    return deadline;
  }
  async message(ws, text) {
    this.prune();
    if (ws.readyState !== 1) return;
    const a = attachment(ws), t = now();
    let m;
    try {m = lightRequest(text);} catch {return this.end(ws, 'Invalid lighting message');}
    if (m.id <= a.lastId) return this.reject(ws, a, m.id, 'replay');
    a.lastId = m.id;
    a.last = t;
    ws.serializeAttachment(a);
    if (a.pending) return this.reject(ws, a, m.id, 'in_flight', 429);
    if (m.op === 'data' && a.role === 'none') return this.reject(ws, a, m.id, 'authentication_required', 401);
    if (a.role !== 'none' && (m.op === 'identity' || m.op === 'session'))
      return this.reject(ws, a, m.id, 'new_connection_required');
    const gateway = this.gateway();
    if (!gateway || a.boot !== gateway.a.boot) return this.reject(ws, a, m.id, 'offline', 503);
    const clients = this.clients().map(attachment), pending = clients.filter(p => p.pending);
    if (pending.length >= 3 || (a.role !== 'owner' && pending.filter(p => p.role !== 'owner').length >= 2))
      return this.reject(ws, a, m.id, 'busy', 429);
    if (a.role === 'none' && (!this.rate('peer:auth:' + a.peer, 6, 10000) || !this.rate('auth', 12, 10000)))
      return this.reject(ws, a, m.id, 'rate', 429);
    const rates = (gateway.a.lightRates ?? []).filter(p => t - p.at < 1000);
    if (rates.length >= 8 || (a.role !== 'owner' && rates.filter(p => p.role !== 'owner').length >= 6))
      return this.reject(ws, a, m.id, 'rate', 429);
    rates.push({at: t, role: a.role});
    gateway.a.lightRates = rates;
    gateway.ws.serializeAttachment(gateway.a);
    const ttl = m.op === 'session' || m.op === 'identity' ? Math.min(LIGHTS_HANDSHAKE_TTL, a.authUntil - t) : LIGHTS_DATA_TTL;
    if (ttl <= 0) return this.reject(ws, a, m.id, 'expired', 401);
    a.pending = {id: m.id, op: m.op, deadline: t + ttl, boot: a.boot};
    ws.serializeAttachment(a);
    if (!send(gateway.ws, lightForward(m, a.id, a.boot, gateway.a.lease, ttl))) return this.reject(ws, a, m.id, 'offline', 503);
    await this.schedule(t + ttl);
  }
  reply(device, text) {
    // Malformed lighting replies are isolated from the existing arcade socket.
    // The device identity is authenticated, but its lighting implementation can
    // still fail independently. Close the lighting lane, not the game channel.
    let m;
    try {m = lightReply(text);} catch {this.disconnect('invalid_gateway_reply'); return;}
    if (!this.enabled() || m.boot !== device.boot) return;
    const ws = this.clients().find(w => attachment(w).id === m.connection);
    if (!ws) return;
    const a = attachment(ws), pending = a.pending;
    if (!pending || pending.id !== m.id || pending.boot !== m.boot || a.boot !== m.boot) return;
    if (now() >= pending.deadline) return this.reject(ws, a, m.id, 'timeout', 504);
    if (m.status === 200 && pending.op !== 'close' && m.payload === '')
      return this.reject(ws, a, m.id, 'invalid_gateway_payload', 502);
    if (Object.hasOwn(m, 'role') && m.role !== 'none' &&
        (m.status !== 200 || (pending.op !== 'session' && pending.op !== 'data')))
      return this.reject(ws, a, m.id, 'invalid_gateway_role', 502);
    if (m.role === 'guest' && a.role !== 'guest' && this.clients().filter(w => attachment(w).role === 'guest').length >= 2)
      return this.reject(ws, a, m.id, 'busy', 429);
    a.pending = null;
    if (Object.hasOwn(m, 'role')) a.role = m.role;
    ws.serializeAttachment(a);
    send(ws, m);
    if (pending.op === 'close' || m.status !== 200 || m.role === 'none')
      this.end(ws, 'Lighting session ended', pending.op !== 'close' || m.status !== 200);
  }
  disconnect(error = 'offline') {
    for (const ws of this.clients()) {
      const a = attachment(ws);
      this.reject(ws, a, a.pending?.id ?? 0, error, 503);
    }
  }
}
