import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {createHash} from 'node:crypto';
import '../web/media.js';
import './fixtures/legacy-replay.js';

const bytes=await readFile('assets/entrance.rpl');
const evidence=JSON.parse(await readFile('assets/replay-provenance.json','utf8'));
const sha=data=>createHash('sha256').update(data).digest('hex');
assert.equal(sha(bytes),evidence.sha256);
const movie=new EntranceReplay.Replay(bytes),decoded=createHash('sha256');
assert.equal(movie.frames,evidence.compressed_scene_matches);
assert.equal(movie.chapterCount,32);
await movie.seek(0);
for(let frame=0;frame<movie.frames;frame++) {
  assert.equal(movie.frame,frame);
  decoded.update(movie.scene);
  await movie.next();
}
assert.equal(movie.frame,0,'Replay must wrap deterministically');
assert.equal(decoded.digest('hex'),evidence.decoded_scenes_sha256,'All browser scenes must match the shipped C decoder');
for(const sample of evidence.raster_samples) {
  await movie.seek(sample.frame);
  const pixels=Buffer.concat(Array.from({length:240},(_,y)=>Buffer.from(movie.row(y))));
  assert.equal(sha(pixels),sample.sha256,`C/browser raster mismatch at frame ${sample.frame}`);
}
for(const size of [0,4,63,64,8192,bytes.length-1])assert.throws(()=>new EntranceReplay.Replay(bytes.subarray(0,size)));
const bad=Buffer.from(bytes);bad[bad.length-1]^=1;assert.throws(()=>new EntranceReplay.Replay(bad));
await assert.rejects(()=>movie.seek(movie.frames));
console.log(`Replay: ${movie.frames} C/browser scene matches, ${evidence.raster_samples.length} raster matches, seek, loop and rejection checks passed`);
