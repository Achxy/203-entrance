import assert from 'node:assert/strict';
import '../web/media.js';
const M = globalThis.EntranceMedia;
const palette = Array.from({length:16}, (_, i)=>i * 4096);
let seed = 203;
const random = () => {seed = (Math.imul(seed,1664525)+1013904223)>>>0;return seed;};
for (const [width,height] of [[1,1],[16,20],[32,40],[64,80],[128,160],[160,128]]) {
  const frames=[Uint8Array.from({length:width*height},()=>random()&15)];
  if(width<128) for(let i=0;i<4;i++) {
    const frame=frames.at(-1).slice();for(let p=0;p<Math.min(10,frame.length);p++)frame[random()%frame.length]=random()&15;frames.push(frame);
  }
  const clip=M.encodeIndexed({width,height,frames,palette,fps:6,title:'Round trip'});
  const decoded=M.decode(clip.data);
  assert.deepEqual(decoded.frames,frames);assert.deepEqual(decoded.palette,palette);
  for(const size of [0,12,79,80,82,clip.data.length-1])assert.throws(()=>M.decode(clip.data.slice(0,size)));
}
assert.equal(M.crc32(new TextEncoder().encode('123456789')),0xcbf43926);
const rgba=new Uint8Array(16*20*4);
for(let p=0;p<rgba.length;p+=4){rgba[p]=p%256;rgba[p+1]=127;rgba[p+2]=64;rgba[p+3]=255;}
const clip=M.encode({width:16,height:20,fps:1,frames:[rgba],title:'Unicode ☃'});
assert.equal(M.decode(clip.data).title,'Unicode _');assert.equal(clip.palette.length,16);
const hostile=clip.data.slice();hostile[15]=1;new DataView(hostile.buffer).setUint32(8,M.crc32(hostile.subarray(12)),true);
assert.throws(()=>M.decode(hostile));
console.log('ECL1 JavaScript codec: round trips, bounds, quantization, corruption passed');
