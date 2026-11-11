import {DurableObject} from 'cloudflare:workers';
import {flat,request,hex,validHex,integer,authenticationMessage,connectionMessage,verifyDevice,MAX_REPLY} from './protocol.mjs';
const now = () => Date.now();
const send = (ws,m) => { try { ws.send(JSON.stringify(m)); return true; } catch { return false; } };
const close = (ws,reason) => { try { ws.close(1008,reason); } catch {} };
const attachment = ws => ws.deserializeAttachment();
function failure(ws,id,code,message,status=503) {send(ws,{v:1,type:'reply',id,status,body:JSON.stringify({error:code,message})});}
export default {
  async fetch(req,env) {
    const url=new URL(req.url);
    if (url.pathname !== '/v1/device' && url.pathname !== '/v1/player' && url.pathname !== '/v1/status') return new Response('Not found',{status:404});
    const origin=req.headers.get('Origin');
    const allowed=env.ALLOWED_ORIGINS.split(',');
    if(url.pathname!=='/v1/device' && !allowed.includes(origin)) return new Response('Origin denied',{status:403});
    if(url.search || req.method!=='GET') return new Response('Invalid request',{status:400});
    if(url.pathname!=='/v1/status' && req.headers.get('Upgrade')?.toLowerCase()!=='websocket') return new Response('WebSocket required',{status:426});
    if(url.pathname==='/v1/device'){
      // Reject unsolicited device sockets before they consume either pending
      // authentication slot. The fresh server challenge still completes auth.
      try {
        const time=req.headers.get('X-203-Time');
        if(Math.abs(Math.floor(now()/1000)-Number(time))>90)throw Error();
        const message=connectionMessage(env.DEVICE_ID,req.headers.get('X-203-Boot'),time,req.headers.get('X-203-Nonce'));
        if(!await verifyDevice(env.DEVICE_PUBLIC_KEY,message,req.headers.get('X-203-Proof')))throw Error();
      }catch{return new Response('Device proof required',{status:401});}
    }
    const headers=new Headers(req.headers);
    // This value comes from Cloudflare, never from a forwarded client field.
    headers.set('X-Relay-Peer',req.headers.get('CF-Connecting-IP')||'unknown');
    return env.ROOM.get(env.ROOM.idFromName(env.DEVICE_ID)).fetch(new Request(req,{headers}));
  }
};
export class ArcadeRoom extends DurableObject {
  constructor(ctx,env){super(ctx,env);this.ctx=ctx;this.env=env;this.ctx.storage.sql.exec("CREATE TABLE IF NOT EXISTS relay_limits (k TEXT PRIMARY KEY, start INTEGER NOT NULL, n INTEGER NOT NULL)");}
  sockets(){return this.ctx.getWebSockets().filter(ws=>ws.readyState===1);}
  device(){return this.sockets().find(ws=>attachment(ws)?.kind==='device');}
  players(){return this.sockets().filter(ws=>attachment(ws)?.kind==='phone');}
  prune(){
    const t=now();
    for(const ws of this.sockets()){
      const a=attachment(ws);
      if(!a || (a.kind==='pending' && t-a.created>10000) || (a.kind==='phone' && (t>a.until||t-a.last>12000)) || (a.kind==='device' && t-a.last>15000)) close(ws,'Session expired');
    }
  }
  rate(key,limit,ms){
    // Low-frequency admission limits survive hibernation; input limits live in attachments.
    return this.ctx.storage.transactionSync(()=>{
      const t=now(),sql=this.ctx.storage.sql;
      const old=[...sql.exec('SELECT start,n FROM relay_limits WHERE k=?',key)][0];
      if(old&&t-old.start<ms&&old.n>=limit)return false;
      const start=old&&t-old.start<ms?old.start:t, n=start===old?.start?old.n+1:1;
      sql.exec('INSERT INTO relay_limits(k,start,n) VALUES(?,?,?) ON CONFLICT(k) DO UPDATE SET start=excluded.start,n=excluded.n',key,start,n);
      sql.exec("DELETE FROM relay_limits WHERE k IN (SELECT k FROM relay_limits WHERE k LIKE 'connect:%' OR k LIKE 'device:%' OR k LIKE 'join:%' OR k LIKE 'resume:%' ORDER BY start DESC LIMIT -1 OFFSET 128)");
      return true;
    });
  }
  async fetch(req){
    this.prune();const path=new URL(req.url).pathname;
    const device=this.device(), da=device&&attachment(device);
    if(path==='/v1/status') return Response.json({online:!!da&&now()-da.last<10000,protocol:1},{headers:{'Cache-Control':'no-store','Access-Control-Allow-Origin':req.headers.get('Origin'),'Vary':'Origin'}});
    const ip=req.headers.get('X-Relay-Peer');
    const digest=await crypto.subtle.digest('SHA-256',new TextEncoder().encode(ip));
    const peer=new DataView(digest).getUint32(0)||1;
    // The display and phones can share one campus NAT. Phone reconnects must not
    // consume the display's separate recovery budget.
    if(!this.rate((path==='/v1/device'?'device:':'connect:')+peer,12,60000)) return new Response('Please retry later',{status:429});
    if(path==='/v1/device'){
      if(this.sockets().filter(w=>attachment(w)?.kind==='pending').length>=2) return new Response('Authentication busy',{status:429});
    }else{
      if(!da||now()-da.last>10000) return new Response('Display offline',{status:503});
      const phones=this.players();
      if(phones.length>=6 || phones.filter(w=>attachment(w).peer===peer&&!attachment(w).joined).length>=2) return new Response('Controllers busy',{status:429});
    }
    const [client,ws]=Object.values(new WebSocketPair());
    const a={kind:path==='/v1/device'?'pending':'phone',id:hex(16),peer,created:now(),last:now(),until:now()+60000,rateAt:now(),count:0,pending:[],lastId:0,joined:false};
    if(a.kind==='pending')a.nonce=hex(32);
    this.ctx.acceptWebSocket(ws);ws.serializeAttachment(a);
    if(a.kind==='pending')send(ws,{v:1,type:'challenge',nonce:a.nonce,connection:a.id});
    else send(ws,{v:1,type:'online',online:true,boot:da.boot,lease:da.lease||0});
    // An alarm runs only to expire abandoned sockets, never per input.
    if(await this.ctx.storage.getAlarm()===null)await this.ctx.storage.setAlarm(now()+15000);
    return new Response(null,{status:101,webSocket:client});
  }
  async webSocketMessage(ws,text){
    const a=attachment(ws);if(!a)return close(ws,'Unknown connection');
    const t=now();
    if(t-a.rateAt>10000){a.rateAt=t;a.count=0;}
    if(++a.count>(a.kind==='phone'?260:600))return close(ws,'Rate limited');
    a.last=t;ws.serializeAttachment(a);
    try{
      if(a.kind==='pending'){
        const m=flat(text);
        if(m.v!==1||m.type!=='authenticate'||t-a.created>10000||m.device!==this.env.DEVICE_ID||!validHex(m.boot,16))throw Error('Authentication invalid');
        a.kind='verifying';ws.serializeAttachment(a);
        if(!await verifyDevice(this.env.DEVICE_PUBLIC_KEY,authenticationMessage(m.device,a.nonce,a.id,m.boot),m.signature))throw Error('Authentication failed');
        const old=this.device();if(old){const retired=attachment(old);retired.kind='retired';old.serializeAttachment(retired);this.disconnectPlayers();close(old,'Replaced by authenticated display');}
        a.kind='device';a.boot=m.boot;a.lease=0;delete a.nonce;ws.serializeAttachment(a);
        send(ws,{v:1,type:'authenticated'});return;
      }
      if(a.kind==='device')return this.fromDevice(ws,a,text);
      if(a.kind!=='phone')throw Error('Authentication in progress');
      const m=request(text);
      if(m.id<=a.lastId)throw Error('Request replay');
      a.lastId=m.id;
      if(t>a.until)return close(ws,'Turn ended');
      const device=this.device(),d=device&&attachment(device);
      if(!d||t-d.last>1500){failure(ws,m.id,'offline','The display is offline. Please try again shortly.');return;}
      // Two in-flight requests; never queue inputs behind an unresponsive ESP.
      a.pending=a.pending.filter(p=>t-p.at<2000);
      if(m.op==='status'&&!m.token&&d.publicState){
        send(ws,{v:1,type:'reply',id:m.id,status:200,body:d.publicState});ws.serializeAttachment(a);return;
      }
      const aggregate=this.players().reduce((n,w)=>n+attachment(w).pending.filter(p=>t-p.at<2000).length,0);
      const anonymous=this.players().filter(w=>!attachment(w).joined).reduce((n,w)=>n+attachment(w).pending.filter(p=>t-p.at<2000).length,0);
      if(aggregate>=4||(!a.joined&&anonymous>=1)){
        failure(ws,m.id,'busy','The display is busy. Please retry shortly.',429);ws.serializeAttachment(a);return;
      }
      if(!a.joined&&m.op==='status'&&!this.rate('resume:'+a.peer,8,10000)){
        failure(ws,m.id,'rate','Please wait before reconnecting.',429);ws.serializeAttachment(a);return;
      }
      if(a.pending.length>=2){failure(ws,m.id,'busy','The connection is catching up.',429);ws.serializeAttachment(a);return;}
      if(m.op==='command'&&(!m.token||!a.joined||m.token!==a.token)){failure(ws,m.id,'session','Join the display first.',401);return;}
      if(m.op==='join'&&a.joined){failure(ws,m.id,'session','This controller already has a turn.',409);return;}
      if(m.op==='knock'&&!this.rate('knock',4,60000)){failure(ws,m.id,'rate','Please wait for the current invitation.',429);return;}
      if(m.op==='join'&&(!this.rate('join:'+a.peer,5,60000)||!this.rate('join',15,60000))){failure(ws,m.id,'rate','Too many attempts. Please wait a minute.',429);return;}
      if(m.op==='command'&&(!m.lease||m.lease>d.lease||d.lease-m.lease>750)){failure(ws,m.id,'stale','The connection paused. Controls were released.',409);return;}
      a.pending.push({id:m.id,op:m.op,at:t,token:m.token});ws.serializeAttachment(a);
      send(device,{v:1,type:'request',id:m.id,op:m.op,token:m.token,body:m.body,lease:m.lease,peer:a.peer,connection:a.id,boot:d.boot});
    }catch{close(ws,'Invalid protocol message');}
  }
  fromDevice(ws,a,text){
    const m=flat(text,MAX_REPLY);
    if(m.v!==1)throw Error('Version mismatch');
    if(m.type==='pulse'){
      if(!integer(m.lease,0,Number.MAX_SAFE_INTEGER)||m.lease<a.lease)throw Error('Invalid clock');
      a.lease=m.lease;
      if(typeof m.state==='string'&&m.state.length<=1200){
        const view=JSON.parse(m.state);
        if(view.slot!==-1||view.boot!==a.boot||Object.hasOwn(view,'token')||Object.hasOwn(view,'receipt'))throw Error('Invalid public state');
        a.publicState=m.state;
      }
      ws.serializeAttachment(a);
      for(const phone of this.players())send(phone,{v:1,type:'online',online:true,boot:a.boot,lease:a.lease});
      return;
    }
    if(m.type!=='reply'||!validHex(m.connection,32)||!integer(m.id,1,0xffffffff)||!integer(m.status,200,599)||typeof m.body!=='string'||m.body.length>2400)throw Error('Invalid reply');
    const phone=this.players().find(w=>attachment(w).id===m.connection);if(!phone)return;
    const p=attachment(phone),pending=p.pending.find(x=>x.id===m.id);if(!pending)return;
    p.pending=p.pending.filter(x=>x.id!==m.id);
    // Only a reply from the authenticated ESP may turn a lobby socket into a player.
    if(m.status===200){
      const body=JSON.parse(m.body);
      const admitted=pending.op==='join'?body.token:pending.op==='status'?pending.token:'';
      if(validHex(admitted,32)&&integer(body.slot,0,1)){
        // An existing token may resume, but only one socket can use that token.
        for(const other of this.players())if(other!==phone&&attachment(other).token===admitted)close(other,'Controller resumed elsewhere');
        if(!p.joined&&this.players().filter(w=>attachment(w).joined).length>=2){close(phone,'Controllers full');return;}
        p.joined=true;p.token=admitted;p.until=p.created+360000;
      }
      if(pending.op==='status'&&p.joined&&body.slot<0)p.until=now()+5000;
    }
    phone.serializeAttachment(p);send(phone,{v:1,type:'reply',id:m.id,status:m.status,body:m.body});
  }
  disconnectPlayers(){for(const ws of this.players()){send(ws,{v:1,type:'online',online:false});close(ws,'Display disconnected');}}
  webSocketClose(ws){const a=attachment(ws);if(a?.kind==='device')this.disconnectPlayers();try{ws.close(1000,'Closed');}catch{}}
  webSocketError(ws){this.webSocketClose(ws);}
  async alarm(){this.prune();if(this.sockets().length)await this.ctx.storage.setAlarm(now()+15000);}
}
