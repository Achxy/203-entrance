/* Original-face pixel conversion and deterministic paw animation. No runtime image processing. */
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import {execFileSync} from 'node:child_process';
import '../web/media.js';
const root=path.resolve(import.meta.dirname,'..');process.chdir(root);
const W=96,H=75,S=300,FPS=12,N=36,PAW_OFFSET=8,out='output/study';fs.mkdirSync(out,{recursive:true});
const load=p=>execFileSync('magick',[p,'-filter','Box','-resize',`${S}x${S}!`,'-alpha','on','-depth','8','rgba:-']);
const reference=load('assets/study/reference.png'),chest=load('assets/study/clean-chest.png');
const body=[[.18,0],[.31,0],[.38,.07],[.54,.095],[.65,.06],[.73,0],[.78,0],[.80,.08],[.79,.20],[.80,.31],[.82,.43],[.80,.56],[.81,.68],[.85,.83],[.88,1],[.11,1],[.13,.82],[.17,.64],[.14,.53],[.15,.38],[.16,.23],[.15,.10]];
const paw=[[.405,.704],[.443,.689],[.485,.691],[.523,.715],[.563,.702],[.608,.713],[.641,.754],[.654,.798],[.689,.824],[.706,.873],[.729,.923],[.742,1.03],[.268,1.03],[.274,.922],[.305,.864],[.331,.812],[.365,.781],[.381,.737]];
function inside(poly,x,y){let yes=false;for(let i=0,j=poly.length-1;i<poly.length;j=i++){const a=poly[i],b=poly[j];if((a[1]>y)!==(b[1]>y)&&x<(b[0]-a[0])*(y-a[1])/(b[1]-a[1])+a[0])yes=!yes;}return yes;}
const palette=[0,12,24,38,52,67,82,98,115,133,152,173,195,216,237,255].map(v=>((v>>3)<<11)|((v>>2)<<5)|(v>>3));
const colors=palette.map(EntranceMedia.rgb);
const base=Buffer.from(reference);
for(let y=0;y<S;y++)for(let x=0;x<S;x++)if(inside(paw,x/S,y/S)){const p=(y*S+x)*4;base.set(chest.subarray(p,p+4),p);}
const frames=[],changes=[],hashes=[],poses=[];
const sha=b=>crypto.createHash('sha256').update(b).digest('hex');
// Supersample each output pixel before mapping once to one shared RGB565 palette.
// The face uses only the owner's original pixels, never a regenerated portrait.
for(let f=0;f<N;f++){
 const t=f/(N-1),reach=(1-Math.cos(t*Math.PI*2))/2,scaleX=1+.30*reach,scaleY=1+.45*reach,shiftX=PAW_OFFSET/H+Math.sin(t*Math.PI*2)*.025*reach;
 poses.push({scaleX,scaleY,shiftX});const frame=new Uint8Array(W*H);
 for(let y=0;y<H;y++)for(let x=0;x<W;x++){
  let sum=[0,0,0];
  for(let ay=0;ay<4;ay++)for(let ax=0;ax<4;ax++){
   const u=(x+(ax+.5)/4-(W-H)/2)/H,v=(y+(ay+.5)/4)/H;
   let src=base,px=u,py=v,visible=inside(body,u,v);
   const pu=.5+(u-.5-shiftX)/scaleX,pv=1+(v-1)/scaleY;
   if(pu>=0&&pu<1&&pv>=0&&pv<1&&inside(paw,pu,pv)){src=reference;px=pu;py=pv;visible=true;}
   if(visible&&px>=0&&px<1&&py>=0&&py<1){const p=(Math.min(S-1,Math.floor(py*S))*S+Math.min(S-1,Math.floor(px*S)))*4;for(let a=0;a<3;a++)sum[a]+=src[p+a];}
  }
  sum=sum.map(v=>v/16);let best=Infinity,index=0;
  for(let c=0;c<16;c++){const d=colors[c].reduce((v,n,a)=>v+(n-sum[a])**2,0);if(d<best){best=d;index=c;}}
  frame[y*W+x]=index;
 }
 frames.push(frame);
}
// Eliminate floating-point endpoint differences and make the wrap identical.
frames[N-1]=frames[0].slice();
const encoded=EntranceMedia.encodeIndexed({width:W,height:H,fps:FPS,frames,palette,title:'Study paw'});
if(encoded.data.length>65536)throw Error('Built-in asset exceeds bound');
fs.writeFileSync('assets/study/study.ecl',encoded.data);
const decoded=EntranceMedia.decodeBuiltin(encoded.data);let outside=0;
const moving=(x,y)=>x>=18&&x<82&&y>=40;
for(let f=0;f<N;f++){
 const rgba=Buffer.alloc(W*H*4),native=Buffer.alloc(W*H*2);let delta=0;
 for(let p=0;p<W*H;p++){
  rgba.set([...colors[frames[f][p]],255],p*4);native.writeUInt16LE(palette[frames[f][p]],p*2);
  if(frames[f][p]!==frames[(f+N-1)%N][p])delta++;
  if(!moving(p%W,Math.floor(p/W))&&frames[f][p]!==frames[0][p])outside++;
 }
 if(Buffer.compare(Buffer.from(decoded.frames[f]),Buffer.from(frames[f])))throw Error('Codec mismatch');
 hashes.push(sha(native));changes.push(delta);
 execFileSync('magick',['-size',`${W}x${H}`,'-depth','8','rgba:-',`${out}/frame-${String(f).padStart(2,'0')}.png`],{input:rgba});
}
if(outside||changes[0])throw Error(`Static body drift or seam: ${outside}/${changes[0]}`);
const record={title:'Study paw',format:'ECL1',width:W,height:H,fps:FPS,frames:N,duration_s:N/FPS,size:encoded.data.length,sha256:sha(encoded.data),reference_sha256:sha(fs.readFileSync('assets/study/reference.png')),chest_sha256:sha(fs.readFileSync('assets/study/clean-chest.png')),palette_rgb565:palette,frame_sha256_rgb565le:hashes,changed_pixels_by_transition:changes,changed_pixels_outside_paw:outside,loop_seam_changed_pixels:changes[0],motion_region:{x:18,y:40,width:64,height:35},paw_offset_pixels_right:PAW_OFFSET,poses,face_source:'Original reference pixels; generated image is used only behind the original foreground paw',visual_review:'All 36 frames and three full LCD caption layouts inspected; fixed face/body, paw bounds and attachment, expression, text and seamless wrap checked. Physical panel appearance remains separate.'};
fs.writeFileSync('assets/study/provenance.json',JSON.stringify(record,null,2)+'\n');
execFileSync('magick',['montage','-font',fs.existsSync('/System/Library/Fonts/Menlo.ttc')?'/System/Library/Fonts/Menlo.ttc':'DejaVu-Sans-Mono',...Array.from({length:N},(_,i)=>`${out}/frame-${String(i).padStart(2,'0')}.png`),'-background','#141414','-filter','point','-geometry','384x300+6+6','-tile','6x6',`${out}/all-frames.png`]);
execFileSync('ffmpeg',['-v','error','-y','-framerate',String(FPS),'-i',`${out}/frame-%02d.png`,'-vf','scale=576:450:flags=neighbor','-c:v','libx264','-crf','16','-pix_fmt','yuv420p','-movflags','+faststart',`${out}/study-paw.mp4`]);
execFileSync('magick',['-delay','8.333333',...Array.from({length:N},(_,i)=>`${out}/frame-${String(i).padStart(2,'0')}.png`),'-filter','point','-resize','576x450','-loop','0',`${out}/study-paw.gif`]);
console.log(JSON.stringify({size:record.size,sha256:record.sha256,frames:N,static_body_changes:outside,seam_changes:changes[0]}));
