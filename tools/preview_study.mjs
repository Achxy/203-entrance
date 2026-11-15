/* Exact LCD layout preview; shared with the browser and compared with C tests. */
import fs from 'node:fs';
import {execFileSync} from 'node:child_process';
import '../web/media.js';import '../web/replay.js';
const out='output/study';fs.mkdirSync(out,{recursive:true});
const movie=new EntranceReplay.Replay(fs.readFileSync('assets/study/study.ecl'),2);
function paint(caption,rotation=1){let rgba;const canvas={classList:{toggle(){}},getContext(){return {createImageData(w,h){return {data:new Uint8ClampedArray(w*h*4)}},putImageData(v){rgba=v.data}}}};movie.paint(canvas,caption,rotation);return Buffer.from(rgba);}
for(let caption=0;caption<3;caption++){
 await movie.seek(17);execFileSync('magick',['-size','160x128','-depth','8','rgba:-','-filter','point','-resize','800x640',`${out}/lcd-caption-${caption+1}.png`],{input:paint(caption)});
}
// 81 s is the exact joint period of a 3 s paw loop and a 4.05 s caption cycle.
// Twenty preview fps places every 1.35 s caption transition on an exact frame.
const FPS=20,N=81*FPS,raw=Buffer.alloc(N*160*128*4);
for(let f=0;f<N;f++){const ms=f*1000/FPS;await movie.seek(Math.floor(ms/movie.period+1e-7)%movie.frames);raw.set(paint(EntranceReplay.captionIndex(ms)),f*160*128*4);}
execFileSync('ffmpeg',['-v','error','-y','-f','rawvideo','-pixel_format','rgba','-video_size','160x128','-framerate',String(FPS),'-i','pipe:0','-vf','scale=640:512:flags=neighbor','-c:v','libx264','-crf','16','-pix_fmt','yuv420p','-movflags','+faststart',`${out}/study-lcd.mp4`],{input:raw});
execFileSync('ffmpeg',['-v','error','-y','-f','rawvideo','-pixel_format','rgba','-video_size','160x128','-framerate',String(FPS),'-i','pipe:0','-filter_complex','scale=640:512:flags=neighbor,split[a][b];[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=none','-loop','0',`${out}/study-lcd.gif`],{input:raw});
console.log('Exact layout previews and 81-second joint-loop GIF/MP4 written.');
