import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import {execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
import '../web/media.js';import '../web/replay.js';
const asset=fs.readFileSync('assets/study/study.ecl'),proof=JSON.parse(fs.readFileSync('assets/study/provenance.json'));
const sha=b=>createHash('sha256').update(b).digest('hex');
const movie=new EntranceReplay.Replay(asset,2),palette=EntranceMedia.decodeBuiltin(asset).palette;
assert.equal(sha(asset),proof.sha256);assert.equal(movie.frames,36);
assert.deepEqual(EntranceReplay.studyMessages,['*paws at you* please study for endsems vro','*paws at you* actually vro','*paws at you* さよなら vro']);
for(const [ms,expected] of [[0,0],[1349,0],[1350,1],[2699,1],[2700,2],[4049,2],[4050,0],[86400000,1]])assert.equal(EntranceReplay.captionIndex(ms),expected);
assert.throws(()=>new EntranceReplay.Replay(asset,0));
assert.throws(()=>EntranceMedia.decode(asset),'Custom clips retain their 12 KiB limit');
const tmp=fs.mkdtempSync(path.join(os.tmpdir(),'study-'));
try {
 const exe=path.join(tmp,'raster');
 execFileSync('cc',['-std=c11','-O2','-Wall','-Wextra','-Werror','-DLOOP_FIXTURE_MAIN','-Icomponents/entrance/include','tests/native/loop_harness.c',...['builtin_loop','media','core','captions'].map(s=>`components/entrance/${s}.c`),'-o',exe]);
 const expected=execFileSync(exe,['assets/study/study.ecl','2'],{maxBuffer:16*1024*1024});let at=0;
 const captionHashes=[new Set(),new Set()];
 for(let f=0;f<movie.frames;f++){
  const frame=Buffer.alloc(movie.width*movie.height*2);
  for(let y=0;y<movie.height;y++)for(let x=0;x<movie.width;x++)frame.writeUInt16LE(palette[movie.row(y)[x]],(y*movie.width+x)*2);
  assert.equal(sha(frame),proof.frame_sha256_rgb565le[f]);
  for(let rot=0;rot<2;rot++)for(let cap=0;cap<3;cap++){
   let image;const canvas={classList:{toggle(){}},getContext(){return {createImageData(w,h){return {data:new Uint8ClampedArray(w*h*4)}},putImageData(v){image=v.data}}}};
   movie.paint(canvas,cap,rot);const pixels=Buffer.alloc(160*128*2);
   for(let p=0;p<160*128;p++)pixels.writeUInt16LE((image[p*4]>>3)<<11|(image[p*4+1]>>2)<<5|(image[p*4+2]>>3),p*2);
   assert.deepEqual(pixels,expected.subarray(at,at+pixels.length),`Frame ${f}, rotation ${rot}, caption ${cap}`);at+=pixels.length;
   const w=rot?160:128,header=rot?32:40,textHeight=rot?20:30;
   for(let x=0;x<w;x++)assert.equal(pixels.readUInt16LE((header*w+x)*2),0xffe0);
   const headerColors=new Set();for(let i=0;i<header*w;i++)headerColors.add(pixels.readUInt16LE(i*2));assert.deepEqual([...headerColors].sort((a,b)=>a-b),[0,65535]);
   captionHashes[rot].add(sha(pixels.subarray((header+1)*w*2,(header+1+textHeight)*w*2)));
  }
  await movie.next();
 }
 assert.equal(movie.frame,0);assert.equal(at,expected.length);
 assert.equal(proof.frame_sha256_rgb565le[0],proof.frame_sha256_rgb565le.at(-1));
 assert.equal(proof.changed_pixels_outside_paw,0);assert.deepEqual(captionHashes.map(v=>v.size),[3,3]);
 for(const n of [0,80,asset.length-1])assert.throws(()=>new EntranceReplay.Replay(asset.subarray(0,n),2));
 const corrupt=Uint8Array.from(asset);corrupt[corrupt.length-1]^=1;assert.throws(()=>new EntranceReplay.Replay(corrupt,2));
 console.log('Study paw: all 216 LCD rasters match C/browser; exact captions, Japanese glyphs, 1.35-second boundaries, size separation and seamless wrap pass.');
}finally{fs.rmSync(tmp,{recursive:true,force:true});}
