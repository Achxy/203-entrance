export const MAX_REQUEST = 1536, MAX_REPLY = 3072;
export const operations = new Set(['status','knock','join','command','receipt']);
const encoder = new TextEncoder();
// A deliberately flat grammar. JSON.parse alone accepts duplicate keys.
export function flat(value, limit = MAX_REQUEST) {
  if (typeof value !== 'string' || encoder.encode(value).length > limit || /\\u0000/i.test(value)) throw Error('Invalid message');
  let at = 0; const space = () => { while (/\s/.test(value[at] || '\0')) at++; };
  const string = () => {
    const start = at++; let escaped = false;
    while (at < value.length) {
      const c = value[at++];
      if (c === '"' && !escaped) return JSON.parse(value.slice(start, at));
      if (c.charCodeAt(0) < 32) throw Error('Invalid string');
      if (c === '\\' && !escaped) escaped = true; else escaped = false;
    }
    throw Error('Invalid string');
  };
  space(); if (value[at++] !== '{') throw Error('Object required');
  const keys = new Set(); space();
  if (value[at] !== '}') for (;;) {
    space(); if (value[at] !== '"') throw Error('Key required');
    const key = string();
    if (keys.has(key) || keys.size >= 16 || key === '__proto__') throw Error('Ambiguous key');
    keys.add(key); space(); if (value[at++] !== ':') throw Error('Colon required'); space();
    if (value[at] === '"') string();
    else { const m = /^(?:true|false|null|-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?)/.exec(value.slice(at)); if (!m) throw Error('Flat values required'); at += m[0].length; }
    space(); if (value[at] === '}') break;
    if (value[at++] !== ',') throw Error('Comma required');
  }
  at++; space(); if (at !== value.length) throw Error('Trailing content');
  const out = JSON.parse(value);
  if (Object.values(out).some(v => typeof v === 'number' && !Number.isFinite(v))) throw Error('Finite values required');
  return out;
}
export const hex = n => [...crypto.getRandomValues(new Uint8Array(n))].map(v=>v.toString(16).padStart(2,'0')).join('');
export const validHex = (s,n) => typeof s === 'string' && s.length === n && /^[a-f0-9]+$/.test(s);
export const integer = (v,lo,hi) => Number.isSafeInteger(v) && v >= lo && v <= hi;
export function request(text) {
  const m = flat(text);
  const allowed = new Set(['v','type','id','op','token','body','lease']);
  if (Object.keys(m).some(k=>!allowed.has(k)) || Object.keys(m).length!==7) throw Error('Unknown request field');
  if (m.v !== 1 || m.type !== 'request' || !integer(m.id,1,0xffffffff) || !operations.has(m.op) ||
      typeof m.token !== 'string' || (m.token !== '' && !validHex(m.token,32)) ||
      typeof m.body !== 'string' || m.body.length > 1024 || !integer(m.lease,0,Number.MAX_SAFE_INTEGER)) throw Error('Invalid request');
  const body = flat(m.body,1024);
  if (m.op === 'command' && (!integer(body.sequence,0,0xffffffff) || !integer(body.action,0,5) || !integer(body.value,0,127) || !integer(body.axis,-1000,1000))) throw Error('Invalid command');
  if (m.op === 'join' && (!integer(body.code,100000,999999) || typeof body.profile !== 'string' || body.profile.length > 640)) throw Error('Invalid join');
  return m;
}
export function derToRaw(value) {
  if (!validHex(value, value?.length) || value.length < 16 || value.length > 144 || value.length % 2) throw Error('Invalid signature');
  const d = Uint8Array.from(value.match(/../g),v=>parseInt(v,16)), out = new Uint8Array(64);
  let at=2;
  if(d[0]!==48 || d[1]+2!==d.length) throw Error('Invalid signature');
  for(let i=0;i<2;i++) {
    if(d[at++]!==2) throw Error('Invalid signature');
    let n=d[at++];
    if(!n || at+n>d.length || (d[at]&128) || (n>1&&d[at]===0&&!(d[at+1]&128))) throw Error('Invalid signature');
    if(n===33&&d[at]===0){at++;n--;}
    if(n>32||!n) throw Error('Invalid signature');
    out.set(d.slice(at,at+n),i*32+32-n);at+=n;
  }
  if(at!==d.length) throw Error('Invalid signature'); return out;
}
export function authenticationMessage(device,nonce,connection,boot) {
  if(!/^[a-z0-9-]{1,40}$/.test(device)||!validHex(nonce,64)||!validHex(connection,32)||!validHex(boot,16)) throw Error('Invalid challenge');
  return encoder.encode(`203-relay-device-v1\n${device}\n${nonce}\n${connection}\n${boot}`);
}
export function connectionMessage(device,boot,time,nonce) {
  if(!/^[a-z0-9-]{1,40}$/.test(device)||!validHex(boot,16)||!/^\d{10}$/.test(time)||!validHex(nonce,32))throw Error('Invalid connection proof');
  return encoder.encode(`203-relay-connect-v1\n${device}\n${boot}\n${time}\n${nonce}`);
}
export async function verifyDevice(pem,message,signature) {
  const der=Uint8Array.from(atob(pem.replace(/-----[^-]+-----|\s/g,'')),c=>c.charCodeAt(0));
  const key=await crypto.subtle.importKey('spki',der,{name:'ECDSA',namedCurve:'P-256'},false,['verify']);
  return crypto.subtle.verify({name:'ECDSA',hash:'SHA-256'},key,derToRaw(signature),message);
}
