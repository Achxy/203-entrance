import assert from 'node:assert/strict';
import http from 'node:http';
import {readFile, mkdir, writeFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import {chromium} from 'playwright';
import '../web/media.js';
import '../web/replay.js';
const M=globalThis.EntranceMedia;
const replayBytes=await readFile('assets/neko/neko.ecl');
const replay=new EntranceReplay.Replay(replayBytes);
await mkdir('.local/browser',{recursive:true});
const server=http.createServer(async(req,res)=>{
  const name={'/':'index.html','/app.js':'app.js','/media.js':'media.js','/display.js':'display.js','/replay.js':'replay.js','/style.css':'style.css'}[req.url];
  if(!name){res.writeHead(404).end();return;}
  res.setHeader('Content-Type',name.endsWith('.js')?'text/javascript':name.endsWith('.css')?'text/css':'text/html');
  res.end(await readFile(`web/${name}`));
});
await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
const origin=`http://127.0.0.1:${server.address().port}`;
const browser=await chromium.launch({headless:true,...(process.env.PLAYWRIGHT_CHROMIUM_PATH?{executablePath:process.env.PLAYWRIGHT_CHROMIUM_PATH}:{})});
const evidence=[];
try {
  for(const width of [1440,390,320]) {
    const context=await browser.newContext({viewport:{width,height:1000},reducedMotion:'reduce'}),page=await context.newPage();
    const errors=[],requests=[];let saved=null,failReply=true,mode='replay',puts=0,csrfChecked=0,replayFrame=3;
    page.on('pageerror',e=>errors.push(e.message));page.on('request',r=>requests.push(r.url()));
    const display=()=>{
      const c=saved?M.decode(saved):null;
      return {ready:true,storage_ready:true,suspended:false,mode,profile:'black',rotation:1,inverted:false,width:160,height:128,
        capacity:12288,size:saved?.length||0,sha256:saved?createHash('sha256').update(saved).digest('hex'):'',title:c?.title||'',
        clip_width:c?.width||0,clip_height:c?.height||0,fps:c?.fps||0,frames:c?.frames.length||0,frame:0,loops:2,error:'',
        replay:{active:mode.startsWith('replay'),bytes:replayBytes.length,frame:replayFrame,frames:replay.frames,
          period_us:replay.period*1000,chapters:1,title:replay.title,width:replay.width,height:replay.height,loops:2,caption:EntranceReplay.messages[0],caption_index:0}};
    };
    await page.route(`${origin}/api/v1/**`,async route=>{
      const req=route.request(),url=new URL(req.url()),path=url.pathname.replace('/api/v1','');let body={ok:true};
      if(path==='/session') body={csrf:'b'.repeat(32),token:'a'.repeat(64)};
      else if(path==='/status') body={device_id:'test-panel',board:'esp32c3-supermini-4m',version:'0.2.0',slot:'ota_0',idf:'v6.1',uptime_ms:90000,
        reset_reason:3,settings_healthy:true,rollback_available:true,free_heap:130000,minimum_heap:80000,largest_block:90000,
        network:{ap_ssid:'203-TEST',station_ip:'offline',online:false},ota:{phase:'idle',busy:false,healthy:true},display:display(),slots:[]};
      else if(path==='/logs')body=[];
      else if(path==='/releases')body={message:'Offline'};
      else if(path==='/display'&&req.method()==='GET')body=display();
      else if(path==='/display'&&req.method()==='PUT'){assert.equal(req.headers()['x-csrf-token'],'b'.repeat(32));csrfChecked++;mode=req.postDataJSON().mode||mode;if(req.postDataJSON().replay_chapter!==undefined)replayFrame=0;}
      else if(path==='/display/replay') {await route.fulfill({status:200,contentType:'application/octet-stream',body:replayBytes});return;}
      else if(path==='/display/clip'&&req.method()==='PUT'){
        assert.equal(req.headers()['x-csrf-token'],'b'.repeat(32));csrfChecked++;
        saved=req.postDataBuffer();M.decode(saved);puts++;
        if(failReply){failReply=false;await route.abort('connectionreset');return;}
      } else if(path==='/display/clip'&&req.method()==='GET'){
        await route.fulfill({status:saved?200:404,contentType:'application/octet-stream',body:saved||Buffer.alloc(0)});return;
      } else if(path==='/display/clip'&&req.method()==='DELETE'){saved=null;}
      await route.fulfill({status:200,contentType:'application/json',body:JSON.stringify(body)});
    });
    await page.goto(origin);await page.locator('#password').fill('correct-password');await page.locator('#login-form button').click();
    await page.locator('#console').waitFor({state:'visible'});
    await page.locator('#display-badge').filter({hasText:'SPI ready'}).waitFor();
    await page.locator('#replay-studio').waitFor({state:'visible'});
    await page.locator('#replay-pause').click();await page.locator('#replay-position').filter({hasText:'Paused'}).waitFor();assert.equal(mode,'replay_pause');
    await page.locator('#replay-play').focus();await page.keyboard.press('Enter');await page.locator('#replay-position').filter({hasText:'Playing'}).waitFor();
    await page.locator('#replay-preview-toggle').click();await page.locator('#replay-work').filter({hasText:'Local preview only'}).waitFor();
    const captionHashes=[];
    for(let sample=0;sample<3;sample++) {
      captionHashes.push(await page.evaluate(()=>{
        const c=document.getElementById('replay-preview'),ctx=c.getContext('2d');
        if(c.width!==160||c.height!==128)throw new Error('Preview is not landscape');
        const divider=ctx.getImageData(0,40,160,1).data;
        for(let x=0;x<160;x++)if(divider[x*4]!==255||divider[x*4+1]!==255||divider[x*4+2]!==0)throw new Error('Yellow divider missing');
        return Array.from(ctx.getImageData(0,0,160,40).data).join(',');
      }));
      await page.waitForTimeout(2100);
    }
    assert.equal(new Set(captionHashes).size,1,'The room number must stay fixed while the cat animates');
    await page.locator('#replay-preview-toggle').click();
    await page.locator('#replay-restart').click();await page.locator('#replay-position').filter({hasText:'0:00 /'}).waitFor();
    await page.locator('#replay-studio').screenshot({path:`.local/browser/replay-${width}.png`});
    await page.locator('#clip-source').selectOption('demo');await page.locator('#clip-prepare').click();
    await page.locator('#clip-work').filter({hasText:'Preview ready'}).waitFor();
    assert.equal(await page.locator('#clip-save').isEnabled(),true);
    await page.locator('#preview-toggle').click();assert.equal(await page.locator('#preview-toggle').getAttribute('aria-pressed'),'true');
    await page.locator('#preview-toggle').click();
    await page.locator('#clip-save').click();await page.locator('#clip-work').filter({hasText:'Saved, verified, and playing'}).waitFor();
    assert.equal(puts,1,'Lost save acknowledgement must not replay the upload');assert.equal(mode,'play');
    await page.locator('[data-display-mode=pause]').click();await page.locator('#display-badge').filter({hasText:'pause'}).waitFor();
    await page.locator('#clip-load').click();await page.locator('#clip-work').filter({hasText:'Saved clip loaded'}).waitFor();
    await page.locator('#clip-source').selectOption('text');await page.locator('#clip-message').fill('ROOM 203\nHELLO ☀');
    await page.locator('#clip-prepare').click();await page.locator('#clip-work').filter({hasText:'Preview ready'}).waitFor();
    assert.equal(await page.locator('#preview-toggle').isDisabled(),true);
    await page.locator('#clip-save').click();await page.locator('#clip-work').filter({hasText:'Saved, verified, and playing'}).waitFor();
    assert.equal(M.decode(saved).frames.length,1);
    for(const theme of ['dark','light']) {
      if(await page.locator('html').getAttribute('data-theme')!==theme)await page.locator('#theme').click();
      assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth<=innerWidth),true,`overflow ${width}`);
      await page.locator('#display').screenshot({path:`.local/browser/display-${width}-${theme}.png`});
    }
    await page.locator('#clip-source').selectOption('demo');await page.locator('#clip-duration').fill('60');await page.locator('#clip-prepare').click();
    await page.locator('#clip-work').filter({hasText:'at most 120 frames'}).waitFor();assert.equal(await page.locator('#clip-save').isDisabled(),true);
    await page.locator('#clip-source').selectOption('file');
    await page.locator('#clip-file').setInputFiles({name:'broken.eclip',mimeType:'application/octet-stream',buffer:Buffer.alloc(100)});
    await page.locator('#clip-prepare').click();await page.locator('#clip-work').filter({hasText:'checksum is invalid'}).waitFor();
    assert.equal(await page.locator('#clip-save').isDisabled(),true);
    if(width===1440) {
      const videoBase64=await page.evaluate(async()=>{
        const c=document.createElement('canvas');c.width=64;c.height=80;const ctx=c.getContext('2d');
        const stream=c.captureStream(6),recorder=new MediaRecorder(stream,{mimeType:'video/webm;codecs=vp8'}),chunks=[];
        const done=new Promise(resolve=>{recorder.onstop=async()=>{const b=new Uint8Array(await new Blob(chunks).arrayBuffer());resolve(btoa(String.fromCharCode(...b)));};});
        recorder.ondataavailable=e=>chunks.push(e.data);recorder.start();
        for(let i=0;i<8;i++){ctx.fillStyle='#101010';ctx.fillRect(0,0,64,80);ctx.fillStyle='#f0b050';ctx.fillRect(i*5,30,12,12);await new Promise(r=>setTimeout(r,170));}
        recorder.stop();stream.getTracks().forEach(t=>t.stop());return done;
      });
      await page.locator('#clip-file').setInputFiles({name:'motion.webm',mimeType:'video/webm',buffer:Buffer.from(videoBase64,'base64')});
      await page.locator('#clip-duration').fill('1');await page.locator('#clip-resolution').selectOption('32');
      await page.locator('#clip-prepare').click();await page.locator('#clip-work').filter({hasText:'Preview ready'}).waitFor({timeout:20000});
      await page.locator('#clip-save').click();await page.locator('#clip-work').filter({hasText:'Saved, verified, and playing'}).waitFor();
      assert.ok(M.decode(saved).frames.length>1,'A real browser-decoded video must produce multiple frames');
      await writeFile('.local/browser/converted-video.eclip',saved);
    }
    await page.locator('#clip-remove').click();await page.locator('#display-current').filter({hasText:'No saved clip'}).waitFor();
    assert.ok(csrfChecked>=4);assert.deepEqual(errors,[]);
    assert.ok(requests.every(u=>u.startsWith(origin)||u.startsWith('blob:')),'external dependency');
    evidence.push({width,overflow:false,pageErrors:errors,localAssets:true,saveReadback:true,lostAcknowledgement:true,framesDecoded:width===1440});
    await context.close();
  }
  await writeFile('.local/browser/display-evidence.json',JSON.stringify({passed:true,layouts:evidence},null,2));
  console.log('Display editor: three layouts, themes, local conversion, video decoding, bounds, lost acknowledgements passed');
} finally {await browser.close();await new Promise(resolve=>server.close(resolve));}
