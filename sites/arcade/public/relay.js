'use strict';
/* The local and Internet controllers use the same interface and game rules. */
if (location.hostname !== '192.168.4.1') {
  window.ArcadeRelay = (() => {
    let socket, opening, url, id = 0, boot = '', lease = 0, lastPulse = 0, retryAt = 0;
    const pending = new Map();
    const fail = message => Object.assign(new Error(message), {code:'connection'});
    function lost() {
      lease=0; lastPulse=0;
      for(const p of pending.values()){clearTimeout(p.timer);p.reject(fail('The display connection paused. Please wait.'));}
      pending.clear();window.dispatchEvent(new Event('arcade-disconnected'));
    }
    async function connect() {
      if(socket?.readyState===WebSocket.OPEN)return;
      if(opening)return opening;
      if(performance.now()<retryAt)throw fail('Reconnecting to the display…');
      opening=(async()=>{
        if(!url){
          const c=new AbortController(),timer=setTimeout(()=>c.abort(),7000);
          try {
            const response=await fetch('/api/config',{cache:'no-store',signal:c.signal});
            const config=await response.json();
            const endpoint=new URL(config.relay_url);
            if(endpoint.protocol!=='wss:'||endpoint.pathname!=='/v1/player'||endpoint.search||endpoint.hash)throw Error();
            url=endpoint.href;
          } catch {throw fail('Internet play is not available yet. Use the display Wi-Fi connection.');}
          finally{clearTimeout(timer);}
        }
        await new Promise((resolve,reject)=>{
          const ws=new WebSocket(url);socket=ws;
          const timer=setTimeout(()=>{ws.close();reject(fail('The display is offline. Try again shortly.'));},7000);
          ws.onopen=()=>{clearTimeout(timer);resolve();};
          ws.onerror=()=>{clearTimeout(timer);reject(fail('The display is offline or busy. Try again shortly.'));};
          ws.onclose=()=>{clearTimeout(timer);if(socket===ws){socket=null;retryAt=performance.now()+1500;lost();}reject(fail('The display connection closed.'));};
          ws.onmessage=event=>{
            if(socket!==ws)return;
            try {
              if(typeof event.data!=='string'||event.data.length>4096)throw Error();
              const m=JSON.parse(event.data);
              if(m.v!==1)throw Error();
              if(m.type==='online'){
                if(!m.online){lost();return;}
                if(boot&&boot!==m.boot){lost();window.dispatchEvent(new Event('arcade-rebooted'));}
                boot=m.boot;lease=m.lease;lastPulse=performance.now();return;
              }
              if(m.type!=='reply')throw Error();
              const p=pending.get(m.id);if(!p)return;
              pending.delete(m.id);clearTimeout(p.timer);
              const result=JSON.parse(m.body);
              if(m.status>=400)p.reject(Object.assign(new Error(result.message||'That action is unavailable.'),{code:result.error,status:m.status}));
              else p.resolve(result);
            }catch{ws.close(1002,'Invalid relay message');}
          };
        });
      })();
      try {await opening;} finally {opening=null;}
    }
    async function request(op,body,token,timeout=2500) {
      await connect();
      if(socket?.readyState!==WebSocket.OPEN)throw fail('Reconnecting to the display…');
      if(pending.size>=2||socket.bufferedAmount>2048)throw fail('The connection is catching up.');
      if(op==='command'&&(!lease||performance.now()-lastPulse>750))throw fail('Waiting for a fresh connection to the display.');
      const requestId=++id, ws=socket;
      return new Promise((resolve,reject)=>{
        const timer=setTimeout(()=>{pending.delete(requestId);reject(fail('The display did not answer in time.'));},timeout);
        pending.set(requestId,{resolve,reject,timer});
        try {ws.send(JSON.stringify({v:1,type:'request',id:requestId,op,token:token||'',body:JSON.stringify(body||{}),lease:lease||0}));}
        catch {pending.delete(requestId);clearTimeout(timer);reject(fail('Connection lost.'));}
      });
    }
    function close(){socket?.close(1000,'Controller hidden');lost();}
    return {request,close};
  })();
}
