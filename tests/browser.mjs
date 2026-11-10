import assert from 'node:assert/strict';
import http from 'node:http';
import {readFile, mkdir, writeFile} from 'node:fs/promises';
import {createRequire} from 'node:module';
import {fileURLToPath} from 'node:url';
import path from 'node:path';
const require = createRequire(import.meta.url);
let playwright;
try { playwright = require('playwright'); }
catch { playwright = require('/opt/homebrew/lib/node_modules/playwright'); }
const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const output = path.join(root, '.local/browser');
await mkdir(output, {recursive: true});
const server = http.createServer(async (req, res) => {
  const file = {'/':'index.html', '/app.js':'app.js', '/style.css':'style.css'}[req.url];
  if (!file) { res.writeHead(404).end(); return; }
  res.setHeader('Content-Type', file.endsWith('.js') ? 'text/javascript' : file.endsWith('.css') ? 'text/css' : 'text/html');
  res.end(await readFile(path.join(root, 'web', file)));
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
const origin = `http://127.0.0.1:${server.address().port}`;
const browser = await playwright.chromium.launch({headless: true,
  ...(process.env.PLAYWRIGHT_CHROMIUM_PATH ? {executablePath:process.env.PLAYWRIGHT_CHROMIUM_PATH} : {})});
const evidence = [];
try {
  for (const width of [1440, 390, 320]) {
    const context = await browser.newContext({viewport:{width,height:1000},reducedMotion:'reduce'});
    const page = await context.newPage();
    const errors = [], requests = [];
    page.on('pageerror', error => errors.push(error.message));
    page.on('request', request => requests.push(request.url()));
    let mode = 'idle', checks = 0, confirmed = false, chunkCount = 0, failed = false;
    let networkOnline = false, releaseReady = false, remoteInstalls = 0, expectedVersion = '0.2.0';
    const status = () => ({device_id:'entrance-c5d474', board:'esp32c3-supermini-4m', version:'0.1.0',
      idf:'v6.1', slot:'ota_0', uptime_ms:90000, free_heap:110000, minimum_heap:81000, largest_block:72000,
      reset_reason:1, settings_healthy:true, rollback_available:true,
      slots:[{slot:'ota_0',version:'0.1.0',state:'valid',running:true},{slot:'ota_1',version:'0.0.9',state:'valid',running:false}],
      network:{ap_ssid:'203-C5D474',station_ip:'offline',station_ssid:'',online:networkOnline,maintenance:false},
      ota:{phase:mode,busy:false,trial:false,healthy:true}});
    await page.route(`${origin}/api/v1/**`, async route => {
      const request = route.request(), url = new URL(request.url()), p = url.pathname.replace('/api/v1','');
      let body = {}, code = 200;
      if (p === '/session' && request.method() === 'POST') {
        if (request.postDataJSON().password === 'slow-but-valid-password') await new Promise(resolve=>setTimeout(resolve,25000));
        if (request.postDataJSON().password === 'incorrect-password') {code=401;body={error:'authentication',message:'Administrator password was not accepted.'};}
        else body={token:'a'.repeat(64),csrf:'b'.repeat(32)};
      } else if (p === '/status') body=status();
      else if (p === '/releases') body=releaseReady?{verified:true,newer:true,version:'0.3.0',message:'Verified release'}:{verified:false,message:'Offline: connect an optional Wi-Fi uplink to check GitHub'};
      else if (p === '/releases/install') {remoteInstalls++;checks=0;confirmed=false;mode='trial';expectedVersion='0.3.0';body={ticket:'d'.repeat(64)};}
      else if (p === '/logs') body=Array.from({length:64},(_,i)=>({id:i+1,ms:i*1000,message:i===63?'Literal <script>never execute</script>':'Management service healthy'}));
      else if (p === '/ota/start') {mode='receiving';body={ticket:'c'.repeat(64),chunk_size:8192};}
      else if (p === '/ota/chunk') {chunkCount++; if(failed){code=409;body={error:'signature',message:'Image signature was rejected.'};}}
      else if (p === '/ota/finish') {mode='trial';body={ok:true};}
      else if (p === '/ota/status') {
        checks++;
        if(checks===1) {await route.abort('connectionreset');return;}
        body={phase: confirmed?'confirmed':mode,trial:!confirmed,running_version:expectedVersion,detail:'Trial health check'};
      } else if (p === '/ota/confirm') {confirmed=true;body={ok:true};}
      else if (p === '/diagnostics') body=status();
      else body={ok:true};
      await route.fulfill({status:code,contentType:'application/json',body:JSON.stringify(body)});
    });
    await page.goto(origin);
    await page.keyboard.press('Tab');
    assert.equal(await page.locator(':focus').textContent(), 'Skip to console');
    await page.locator('#password').fill('incorrect-password');
    await page.locator('#login-form button').click();
    await page.locator('#notice').filter({hasText:'not accepted'}).waitFor();
    await page.locator('#password').fill(width===1440?'slow-but-valid-password':'correct-password');
    await page.locator('#login-form button').click();
    if(width===1440) assert.equal(await page.locator('#login-form button').textContent(),'Checking credentials…');
    await page.locator('#console').waitFor({state:'visible'});
    await page.locator('#logs').filter({hasText:'Literal <script>'}).waitFor();
    assert.equal(await page.locator('#logs script').count(),0);
    assert.equal(await page.locator('#check-release').isDisabled(),true);
    assert.equal(await page.evaluate(()=>document.documentElement.scrollWidth <= innerWidth),true,`overflow at ${width}`);
    await page.screenshot({path:path.join(output,`console-${width}-dark.png`),fullPage:true});
    await page.locator('#theme').click();
    assert.equal(await page.locator('html').getAttribute('data-theme'),'light');
    await page.screenshot({path:path.join(output,`console-${width}-light.png`),fullPage:true});
    if(width===1440) {
      const data=Buffer.alloc(8192);data[0]=0xe9;data.write('0.2.0',48);
      await page.locator('#image').setInputFiles({name:'firmware.bin',mimeType:'application/octet-stream',buffer:data});
      await page.locator('#upload').click();
      await page.locator('#notice').filter({hasText:'Installed 0.2.0'}).waitFor({timeout:20000});
      assert.equal(chunkCount,1);assert.equal(confirmed,true);assert.ok(checks>=3);
      await page.locator('#password').fill('correct-password');await page.locator('#login-form button').click();
      await page.locator('#console').waitFor({state:'visible'});
      failed=true;mode='idle';
      await page.locator('#image').setInputFiles({name:'bad.bin',mimeType:'application/octet-stream',buffer:data});
      await page.locator('#upload').click();
      await page.locator('#notice').filter({hasText:'Image signature was rejected.'}).waitFor();
      assert.equal(await page.locator('#progress-stage').textContent(),'Not confirmed');
      await page.reload();assert.equal(await page.locator('html').getAttribute('data-theme'),'light');
      networkOnline=true;releaseReady=true;failed=false;mode='idle';
      await page.locator('#password').fill('correct-password');await page.locator('#login-form button').click();
      await page.locator('#install-release:not([disabled])').waitFor();await page.locator('#install-release').click();
      await page.waitForFunction(()=>JSON.parse(sessionStorage.getItem('203-update')||'{}').expected==='0.3.0');
      await page.reload();
      await page.locator('#notice').filter({hasText:'Installed 0.3.0'}).waitFor({timeout:20000});
      assert.equal(remoteInstalls,1);assert.equal(confirmed,true);
      assert.equal(await page.evaluate(()=>sessionStorage.getItem('203-update')),null);
    }
    assert.deepEqual(errors,[]);
    assert.ok(requests.every(url=>url.startsWith(origin)), 'unexpected external asset dependency');
    evidence.push({width,overflow:false,pageErrors:errors,externalRequests:0,theme:'dark and light',keyboard:'skip link focused'});
    await context.close();
  }
  await writeFile(path.join(output,'results.json'),JSON.stringify({testedAt:new Date().toISOString(),evidence,update:'success with connection loss and reconnect; backend rejection surfaced'},null,2));
  console.log('Browser checks passed at 1440, 390 and 320 px; themes, keyboard, local assets, login errors, reconnect and update failure reporting.');
} finally { await browser.close(); await new Promise(resolve=>server.close(resolve)); }
