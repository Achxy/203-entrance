import test from 'node:test';
import assert from 'node:assert/strict';
import {DatabaseSync} from 'node:sqlite';
import {generateKeyPairSync,sign} from 'node:crypto';
import {flat,request,authenticationMessage,connectionMessage,verifyDevice} from '../src/protocol.mjs';
import worker,{ArcadeRoom} from '../src/worker.mjs';
const keys=generateKeyPairSync('ec',{namedCurve:'prime256v1'});
const pem=keys.publicKey.export({type:'spki',format:'pem'});
const env={DEVICE_ID:'entrance-c7f088',DEVICE_PUBLIC_KEY:pem};
const hex=n=>'a'.repeat(n);
class Socket{
  constructor(a){this.a=a;this.sent=[];this.readyState=1;}
  serializeAttachment(a){this.a=structuredClone(a);}
  deserializeAttachment(){return structuredClone(this.a);}
  send(m){if(this.readyState!==1)throw Error('closed');this.sent.push(JSON.parse(m));}
  close(){this.readyState=3;}
}
function room(){const sockets=[],db=new DatabaseSync(':memory:');const ctx={getWebSockets:()=>sockets.filter(w=>w.readyState===1),storage:{sql:{exec:(q,...args)=>{const stmt=db.prepare(q);return stmt.columns().length?stmt.all(...args):(stmt.run(...args),[]);}},transactionSync:fn=>fn(),getAlarm:async()=>null,setAlarm:async()=>{}}};return {r:new ArcadeRoom(ctx,env),sockets};}
function a(kind,id){return {kind,id,peer:1,created:Date.now(),last:Date.now(),until:Date.now()+60000,rateAt:Date.now(),count:0,lastId:0,pending:[],joined:false};}
function board(sockets){const s=new Socket({...a('device',hex(32)),boot:hex(16),lease:5000});sockets.push(s);return s;}
function phone(sockets){const s=new Socket(a('phone','b'.repeat(32)));sockets.push(s);return s;}
const req=(id,op='status',token='',body={})=>JSON.stringify({v:1,type:'request',id,op,token,body:JSON.stringify(body),lease:5000});
test('flat protocol rejects duplicate keys, nesting, suffixes and size abuse',()=>{
 assert.equal(flat('{"hello":"世界","number":1}').number,1);
 for(const s of ['{"v":1,"v":2}','{"v":{}}','{"v":[]}','{}{}','{"n":1e400}','{"n":"\\u0000"}','{"v":1,}'])assert.throws(()=>flat(s));
 assert.throws(()=>flat(JSON.stringify({s:'x'.repeat(2048)})));
 assert.throws(()=>request(req(1,'reboot')));
 assert.throws(()=>request(req(1,'command','',{sequence:1,action:-1,value:0,axis:0})));
});
test('device authentication is bound to room, nonce, connection and boot',async()=>{
 const input=authenticationMessage(env.DEVICE_ID,hex(64),hex(32),hex(16));
 const sig=sign('sha256',input,keys.privateKey).toString('hex');
 assert.equal(await verifyDevice(pem,input,sig),true);
 assert.equal(await verifyDevice(pem,authenticationMessage(env.DEVICE_ID,'b'.repeat(64),hex(32),hex(16)),sig),false);
 assert.throws(()=>authenticationMessage('evil\nroom',hex(64),hex(32),hex(16)));
});
test('unauthenticated device sockets never consume a durable authentication slot',async()=>{
 let forwarded=0;
 const e={...env,ALLOWED_ORIGINS:'https://203.at.achyuth.dev',ROOM:{idFromName:()=>1,get:()=>({fetch:()=>{forwarded++;return new Response('accepted');}})}};
 assert.equal((await worker.fetch(new Request('https://relay.test/v1/device',{headers:{Upgrade:'websocket'}}),e)).status,401);
 const time=String(Math.floor(Date.now()/1000)),nonce=hex(32),boot=hex(16);
 const signature=sign('sha256',connectionMessage(env.DEVICE_ID,boot,time,nonce),keys.privateKey).toString('hex');
 const h={Upgrade:'websocket','X-203-Time':time,'X-203-Nonce':nonce,'X-203-Boot':boot,'X-203-Proof':signature};
 assert.equal((await worker.fetch(new Request('https://relay.test/v1/device',{headers:h}),e)).status,200);
 h['X-203-Time']=String(Number(time)-1000);
 assert.equal((await worker.fetch(new Request('https://relay.test/v1/device',{headers:h}),e)).status,401);
 assert.equal(forwarded,1);
});
test('forged device cannot replace the authenticated board',async()=>{
 const {r,sockets}=room(),good=board(sockets),bad=new Socket({...a('pending','c'.repeat(32)),nonce:hex(64)});sockets.push(bad);
 await r.webSocketMessage(bad,JSON.stringify({v:1,type:'authenticate',device:env.DEVICE_ID,boot:hex(16),signature:'00'}));
 assert.equal(bad.readyState,3);assert.equal(good.readyState,1);assert.equal(r.device(),good);
});
test('two in-flight requests bound stalled-device pressure and replay closes socket',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets);
 p.a.joined=true;p.a.token='d'.repeat(32);
 await r.webSocketMessage(p,req(1));await r.webSocketMessage(p,req(2));await r.webSocketMessage(p,req(3));
 assert.equal(b.sent.length,2);assert.equal(p.sent.at(-1).status,429);
 await r.webSocketMessage(p,req(3));assert.equal(p.readyState,3);
});
test('unknown envelope fields cannot reach or disconnect the display',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets);
 const m=JSON.parse(req(1));m.extra=1;
 assert.throws(()=>request(JSON.stringify(m)));
 await r.webSocketMessage(p,JSON.stringify(m));
 assert.equal(p.readyState,3);assert.equal(b.sent.length,0);assert.equal(b.readyState,1);
});
test('anonymous polling uses cached public state without board work',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets);
 await r.webSocketMessage(b,JSON.stringify({v:1,type:'pulse',lease:5100,state:JSON.stringify({slot:-1,boot:b.a.boot,stage:'idle'})}));
 for(let id=1;id<=20;id++)await r.webSocketMessage(p,req(id));
 assert.equal(b.sent.length,0);assert.equal(p.a.pending.length,0);
 assert.equal(JSON.parse(p.sent.at(-1).body).slot,-1);
});
test('aggregate credits reserve board capacity against anonymous traffic',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets),q=phone(sockets),active=phone(sockets),second=phone(sockets);
 active.a.joined=second.a.joined=true;
 await r.webSocketMessage(p,req(1));await r.webSocketMessage(q,req(1));
 assert.equal(q.sent.at(-1).status,429);assert.equal(b.sent.length,1);
 await r.webSocketMessage(active,req(1));await r.webSocketMessage(active,req(2));
 await r.webSocketMessage(second,req(1));await r.webSocketMessage(second,req(2));
 assert.equal(b.sent.length,4);assert.equal(second.sent.at(-1).status,429);assert.equal(b.readyState,1);
});
test('stale board pulses stop request forwarding before pending credits expire',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets);b.a.last=Date.now()-1700;
 await r.webSocketMessage(p,req(1));
 assert.equal(p.sent.at(-1).status,503);assert.equal(b.sent.length,0);
});
test('campus NAT phone admission does not consume board reconnect budget',()=>{
 const {r}=room();for(let i=0;i<12;i++)assert.equal(r.rate('connect:123',12,60000),true);
 assert.equal(r.rate('connect:123',12,60000),false);assert.equal(r.rate('device:123',12,60000),true);
});
test('only ESP admission grants a controller, and stale commands do not reach board',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets),token='d'.repeat(32);
 await r.webSocketMessage(p,req(1,'join','',{code:123456,profile:'G1.test'}));
 await r.webSocketMessage(b,JSON.stringify({v:1,type:'reply',connection:p.a.id,id:1,status:200,body:JSON.stringify({token,slot:0})}));
 assert.equal(p.a.joined,true);
 const m=JSON.parse(req(2,'command',token,{sequence:1,action:0,value:1,axis:0}));m.lease=1000;
 await r.webSocketMessage(p,JSON.stringify(m));assert.equal(p.sent.at(-1).status,409);assert.equal(b.sent.length,1);
 await r.webSocketMessage(p,req(3,'command',token,{sequence:2,action:0,value:1,axis:0}));assert.equal(b.sent.length,2);
});
test('board disconnect closes controllers and expires anonymous sockets',()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets);
 r.webSocketClose(b);assert.equal(p.readyState,3);
 const q=phone(sockets);q.a.until=Date.now()-1;r.prune();assert.equal(q.readyState,3);
});
test('connection attachments restore admission and pending replies after hibernation',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets);
 await r.webSocketMessage(p,req(1));
 const wake=new ArcadeRoom(r.ctx,env);
 await wake.webSocketMessage(b,JSON.stringify({v:1,type:'reply',connection:p.a.id,id:1,status:200,body:'{"slot":-1}'}));
 assert.equal(p.sent.at(-1).id,1);assert.equal(p.a.pending.length,0);
});

test('admission rate limits survive object hibernation',()=>{
 const {r}=room();for(let i=0;i<5;i++)assert.equal(r.rate('join:123',5,60000),true);
 const wake=new ArcadeRoom(r.ctx,env);assert.equal(wake.rate('join:123',5,60000),false);
});
test('a live token can resume after transport loss without creating a new turn',async()=>{
 const {r,sockets}=room(),b=board(sockets),p=phone(sockets),token='e'.repeat(32);
 await r.webSocketMessage(p,req(1,'status',token));
 await r.webSocketMessage(b,JSON.stringify({v:1,type:'reply',connection:p.a.id,id:1,status:200,body:'{"slot":0}'}));
 assert.equal(p.a.joined,true);assert.equal(p.a.token,token);
 await r.webSocketMessage(p,req(2,'command',token,{sequence:9,action:0,value:0,axis:0}));
 assert.equal(b.sent.at(-1).op,'command');
});
