import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import '../web/media.js';import '../web/replay.js';
const asset=fs.readFileSync('assets/neko/neko.ecl'),proof=JSON.parse(fs.readFileSync('assets/neko/provenance.json'));
const palette=EntranceMedia.decode(asset).palette;
const movie=new EntranceReplay.Replay(asset),sha=b=>createHash('sha256').update(b).digest('hex');
assert.equal(sha(asset),proof.sha256);assert.equal(movie.frames,24);assert.equal(movie.period,100);
const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'neko-'));
try {
 const exe=path.join(tmp,'raster');
 execFileSync('cc',['-std=c11','-O2','-Wall','-Wextra','-Werror','-DLOOP_FIXTURE_MAIN','-Icomponents/entrance/include','tests/native/loop_harness.c',...['builtin_loop','media','core','captions'].map(s=>`components/entrance/${s}.c`),'-o',exe]);
 const expected=execFileSync(exe,['assets/neko/neko.ecl'],{maxBuffer:8*1024*1024});let at=0;
 for(let f=0;f<24;f++){
  const frame=Buffer.alloc(96*87*2);for(let y=0;y<87;y++)for(let x=0;x<96;x++){const c=palette[movie.row(y)[x]];frame.writeUInt16LE(c,(y*96+x)*2);}
  assert.equal(sha(frame),proof.frame_sha256_rgb565le[f]);
  for(let rot=0;rot<2;rot++)for(let cap=0;cap<3;cap++){
   let image;const canvas={classList:{toggle(){}},getContext(){return {createImageData(w,h){return {data:new Uint8ClampedArray(w*h*4)}},putImageData(v){image=v.data}}}};
   movie.paint(canvas,cap,rot);const pixels=Buffer.alloc(160*128*2);
   for(let p=0;p<160*128;p++)pixels.writeUInt16LE((image[p*4]>>3)<<11|(image[p*4+1]>>2)<<5|(image[p*4+2]>>3),p*2);
   assert.deepEqual(pixels,expected.subarray(at,at+pixels.length),`Frame ${f}, rotation ${rot}, caption ${cap}`);at+=pixels.length;
  }
  await movie.next();
 }
 assert.equal(movie.frame,0);assert.equal(at,expected.length);
 for(const n of [0,4,79,80,asset.length-1])assert.throws(()=>new EntranceReplay.Replay(asset.subarray(0,n)));
 await assert.rejects(()=>movie.seek(24));
 console.log('Neko: every frame matches artwork hashes; 144 full LCD rasters match C/browser; loop, seek and rejection checks pass.');
}finally{fs.rmSync(tmp,{recursive:true,force:true});}
