/* Reproducible sprite-sheet assembly and ECL1 packing; the artwork is generated separately. */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import '../web/media.js';
const root=path.resolve(import.meta.dirname,'..');process.chdir(root);
const W=96,H=87,FPS=10,source='assets/neko/source-atlas.png',out='output/neko';
fs.mkdirSync(out,{recursive:true});
const poses=[];
for(let i=0;i<8;i++){
 const col=i%4,row=Math.floor(i/4),x=Math.round(col*1774/4),y=Math.round(row*887/2),w=Math.round((col+1)*1774/4)-x,h=Math.round((row+1)*887/2)-y;
 const rgba=execFileSync('magick',[source,'-crop',`${w}x${h}+${x}+${y}`,'+repage','-filter','point','-resize','88x88!','-background','#000000','-gravity','center','-extent',`${W}x${H}`,'-alpha','remove','-alpha','on','-depth','8','rgba:-']);
 if(rgba.length!==W*H*4)throw Error('Unexpected source raster size');
 // Near-transparent generation residue must not become faint floating pixels.
 for(let p=0;p<rgba.length;p+=4)if(Math.max(rgba[p],rgba[p+1],rgba[p+2])<40)rgba[p]=rgba[p+1]=rgba[p+2]=0;
 poses.push(new Uint8Array(rgba));
}
// Keep the master body's raster fixed: all generated variation outside the
// moving forearm is excluded, including face/coin/feet sub-pixel drift.
const moving=(x,y)=>x>=64 && x<86 && y>=14 && y<62;
for(let i=1;i<poses.length;i++)for(let y=0;y<H;y++)for(let x=0;x<W;x++)if(!moving(x,y)){
 const at=(y*W+x)*4;poses[i].set(poses[0].subarray(at,at+4),at);
}
const order=[0,0,1,1,2,3,4,5,6,7,7,7,7,7,6,5,4,3,2,1,1,0,0,0];
// A fixed palette keeps the small red collar, pink nose and yellow eyes from
// being averaged into the much larger gold/cream areas by adaptive quantization.
const paletteHex=['000000','ffffff','f1eee8','dcd7d3','bbb5b0','d99c9b','e53935','a52022','f1a138','cd772b','f3da67','d4b546','997c21','7d823d','505b28','f4e61b'];
const palette=paletteHex.map(h=>{const n=parseInt(h,16);return ((n>>19)&31)<<11|((n>>10)&63)<<5|((n>>3)&31);});
const colors=palette.map(EntranceMedia.rgb);
const indexed=poses.map(rgba=>{const frame=new Uint8Array(W*H);for(let p=0;p<frame.length;p++){let best=Infinity;for(let c=0;c<16;c++){let d=0;for(let a=0;a<3;a++)d+=(rgba[p*4+a]-colors[c][a])**2;if(d<best){best=d;frame[p]=c;}}}return frame;});
const result=EntranceMedia.encodeIndexed({width:W,height:H,fps:FPS,frames:order.map(i=>indexed[i]),palette,title:'Neko wave'});
const decoded=EntranceMedia.decode(result.data);
fs.writeFileSync('assets/neko/neko.ecl',result.data);
const rgb=decoded.palette.map(EntranceMedia.rgb);
let changedOutside=0;const changes=[],frameHashes=[];
for(let i=0;i<decoded.frames.length;i++){
 const frame=decoded.frames[i],rgba=new Uint8Array(W*H*4);let changed=0;
 for(let p=0;p<frame.length;p++){
  rgba.set([...rgb[frame[p]],255],p*4);
  if(frame[p]!==decoded.frames[0][p]&&!moving(p%W,Math.floor(p/W)))changedOutside++;
  if(frame[p]!==decoded.frames[(i+decoded.frames.length-1)%decoded.frames.length][p])changed++;
 }
 changes.push(changed);
 const native=Buffer.alloc(frame.length*2);for(let p=0;p<frame.length;p++)native.writeUInt16LE(decoded.palette[frame[p]],p*2);
 frameHashes.push(crypto.createHash("sha256").update(native).digest("hex"));
 execFileSync('magick',['-size',`${W}x${H}`,'-depth','8','rgba:-',`${out}/frame-${String(i).padStart(2,'0')}.png`],{input:rgba});
}
if(changedOutside||changes[0])throw Error('Static body drift or a discontinuous loop');
const report={title:'Neko wave',format:'ECL1',width:W,height:H,fps:FPS,frames:order.length,duration_s:order.length/FPS,poses:8,pose_order:order,size:result.data.length,sha256:crypto.createHash('sha256').update(result.data).digest('hex'),source_sha256:crypto.createHash('sha256').update(fs.readFileSync(source)).digest('hex'),palette_rgb565:decoded.palette,frame_sha256_rgb565le:frameHashes,changed_pixels_by_transition:changes,changed_pixels_outside_paw:changedOutside,loop_seam_changed_pixels:changes[0],moving_region:{x:64,y:14,width:22,height:48},generator:'OpenAI built-in imagegen; user-supplied reference; deterministic alignment and shared-palette packing',visual_review:'All 24 frames / eight unique poses reviewed in a contact sheet; static body, paw attachment, colors, bounds and wrap inspected. Physical panel review remains separate.'};
fs.writeFileSync(`${out}/frame-audit.json`,JSON.stringify(report,null,2)+'\n');
fs.writeFileSync('assets/neko/provenance.json',JSON.stringify(report,null,2)+'\n');
execFileSync('magick',['montage','-font',fs.existsSync('/System/Library/Fonts/Menlo.ttc')?'/System/Library/Fonts/Menlo.ttc':'DejaVu-Sans-Mono',...Array.from({length:24},(_,i)=>`${out}/frame-${String(i).padStart(2,'0')}.png`),'-background','#242424','-filter','point','-geometry','288x261+8+8','-tile','6x4',`${out}/all-frames.png`]);
execFileSync('ffmpeg',['-v','error','-y','-framerate',String(FPS),'-i',`${out}/frame-%02d.png`,'-vf','scale=576:522:flags=neighbor','-c:v','libx264','-preset','slow','-crf','16','-pix_fmt','yuv420p','-movflags','+faststart',`${out}/neko-wave.mp4`]);
execFileSync('magick',['-delay','10',...Array.from({length:24},(_,i)=>`${out}/frame-${String(i).padStart(2,'0')}.png`),'-filter','point','-resize','384x348','-loop','0',`${out}/neko-wave.gif`]);
console.log(JSON.stringify(report,null,2));
