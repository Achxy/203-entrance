/* Signed, bounded ECL1 loop preview. Frame colors match the LCD RGB565 output. */
"use strict";
(() => {
  const MAX = 65536;
  const messages = ["203"];
  const studyMessages = ["*paws at you* please study for endsems vro", "*paws at you* actually vro", "*paws at you* さよなら vro"];
  const kana = {"さ":[16,16,126,32,36,2,60,0],"よ":[16,16,112,16,28,50,76,0],"な":[4,47,68,34,57,100,24,0],"ら":[12,16,2,58,70,64,60,0]};
  const captionIndex = elapsedMs => Math.floor(Math.max(0,elapsedMs)/1350)%3;
  function glyph(ch) {
    if(kana[ch])return {rows:kana[ch],left:0,width:8,advance:8};
    if(ch===' ')return {rows:[],left:0,width:0,advance:3};
    const code=ch.codePointAt(0)<128?ch.codePointAt(0):63,rows=font.subarray(code*8,code*8+8);
    let bits=0,left=0,right=7;for(const row of rows)bits|=row;
    while(left<7&&!(bits&1<<left))left++;while(right>left&&!(bits&1<<right))right--;
    return {rows,left,width:right-left+1,advance:right-left+2};
  }
  function studyText(im,w,top,index) {
    let lines=['*paws at you*',''];
    if(index%3===0)lines=w>=160?['*paws at you* please','study for endsems vro']:['*paws at you*','please study for','endsems vro'];
    else lines[1]=index%3===1?'actually vro':'さよなら vro';
    const height=w>=160?20:30;
    top+=Math.floor((height-lines.length*10)/2)+1;
    lines.forEach((text,line)=>{
      const glyphs=Array.from(text,glyph),ink=glyphs.reduce((n,g)=>n+g.advance,0)-1;
      let x=Math.floor((w-ink)/2);
      for(const g of glyphs){for(let y=0;y<8;y++)for(let c=0;c<g.width;c++)if(g.rows[y]&1<<(c+g.left))im.data.set([255,255,255,255],((top+line*10+y)*w+x+c)*4);x+=g.advance;}
    });
  }
  class Replay {
    constructor(bytes, slot = 1) {
      if(![1,2].includes(slot))throw new Error("Unknown animation slot.");
      this.slot=slot;
      const clip=EntranceMedia.decodeBuiltin(bytes);
      this.data=clip.data; this.images=clip.frames; this.frames=clip.frames.length;
      this.width=clip.width; this.height=clip.height; this.period=1000/clip.fps;
      this.palette=clip.palette.map(EntranceMedia.rgb); this.title=clip.title; this.frame=0;
    }
    async seek(frame) {
      if(!Number.isInteger(frame)||frame<0||frame>=this.frames)throw new Error("Animation frame is outside the loop.");
      this.frame=frame;
    }
    async next() { this.frame=(this.frame+1)%this.frames; }
    row(y) {
      if(!Number.isInteger(y)||y<0||y>=this.height)throw new Error("Animation row is outside the frame.");
      return this.images[this.frame].subarray(y*this.width,(y+1)*this.width);
    }
    paint(canvas, captionIndex = 0, rotation = 0) {
      const w = rotation & 1 ? 160 : 128, h = rotation & 1 ? 128 : 160, header = this.slot===1 ? (h===160?50:40) : (h===160?40:32), textHeight=this.slot===1?0:(h===160?30:20), contentTop=header+1+textHeight;
      canvas.width = w; canvas.height = h; canvas.classList.toggle("landscape", Boolean(rotation & 1));
      const ctx = canvas.getContext("2d"), im = ctx.createImageData(w, h); for (let i = 3; i < im.data.length; i += 4) im.data[i] = 255;
      let dw = w, dh = h - contentTop; if (w * this.height > dh * this.width) dw = Math.floor(dh * this.width / this.height); else dh = Math.floor(w * this.height / this.width); dw=Math.max(1,dw); dh=Math.max(1,dh);
      const left = Math.floor((w - dw) / 2), top = contentTop + Math.floor((h - contentTop - dh) / 2);
      for (let y = 0; y < dh; y++) { const row = this.row(Math.floor(y * this.height / dh)); for (let x = 0; x < dw; x++) { const rgb = this.palette[row[Math.floor(x * this.width / dw)]]; im.data.set(rgb, ((top + y) * w + left + x) * 4); } }
      const scale = this.slot===1?4:3, inkWidth = 22 * scale, inkHeight = 7 * scale;
      const textLeft = Math.floor((w - inkWidth) / 2), textTop = Math.floor((header - inkHeight) / 2);
      for (let y = 0; y < inkHeight; y++) for (let x = 0; x < inkWidth; x++) {
        const column = Math.floor(x / scale), ch = messages[0].charCodeAt(Math.floor(column / 8));
        if (font[ch * 8 + Math.floor(y / scale)] & 1 << (column % 8))
          im.data.set([255,255,255,255], ((textTop + y) * w + textLeft + x) * 4);
      }
      for (let x = 0; x < w; x++) im.data.set([255, 255, 0, 255], (header * w + x) * 4);
      if(this.slot===2)studyText(im,w,header+1,captionIndex%3);
      ctx.putImageData(im, 0, 0);
    }
  }
  const font = Uint8Array.from(atob("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGDw8GBgAGAA2NgAAAAAAADY2fzZ/NjYADD4DHjAfDAAAYzMYDGZjABw2HG47M24ABgYDAAAAAAAYDAYGBgwYAAYMGBgYDAYAAGY8/zxmAAAADAw/DAwAAAAAAAAADAwGAAAAPwAAAAAAAAAAAAwMAGAwGAwGAwEAPmNze29nPgAMDgwMDAw/AB4zMBwGMz8AHjMwHDAzHgA4PDYzfzB4AD8DHzAwMx4AHAYDHzMzHgA/MzAYDAwMAB4zMx4zMx4AHjMzPjAYDgAADAwAAAwMAAAMDAAADAwGGAwGAwYMGAAAAD8AAD8AAAYMGDAYDAYAHjMwGAwADAA+Y3t7ewMeAAweMzM/MzMAP2ZmPmZmPwA8ZgMDA2Y8AB82ZmZmNh8Af0YWHhZGfwB/RhYeFgYPADxmAwNzZnwAMzMzPzMzMwAeDAwMDAweAHgwMDAzMx4AZ2Y2HjZmZwAPBgYGRmZ/AGN3f39rY2MAY2dve3NjYwAcNmNjYzYcAD9mZj4GBg8AHjMzMzseOAA/ZmY+NmZnAB4zBw44Mx4APy0MDAwMHgAzMzMzMzM/ADMzMzMzHgwAY2Nja393YwBjYzYcHDZjADMzMx4MDB4Af2MxGExmfwAeBgYGBgYeAAMGDBgwYEAAHhgYGBgYHgAIHDZjAAAAAAAAAAAAAAD/DAwYAAAAAAAAAB4wPjNuAAcGBj5mZjsAAAAeMwMzHgA4MDA+MzNuAAAAHjM/Ax4AHDYGDwYGDwAAAG4zMz4wHwcGNm5mZmcADAAODAwMHgAwADAwMDMzHgcGZjYeNmcADgwMDAwMHgAAADN/f2tjAAAAHzMzMzMAAAAeMzMzHgAAADtmZj4GDwAAbjMzPjB4AAA7bmYGDwAAAD4DHjAfAAgMPgwMLBgAAAAzMzMzbgAAADMzMx4MAAAAY2t/fzYAAABjNhw2YwAAADMzMz4wHwAAPxkMJj8AOAwMBwwMOAAYGBgAGBgYAAcMDDgMDAcAbjsAAAAAAAAAAAAAAAAAAA=="), (c) => c.charCodeAt(0));
  globalThis.EntranceReplay = { Replay, messages, studyMessages, captionIndex, MAX };
})();

if (typeof document !== "undefined") (() => {
  let state = null, busy = false, loading = false, preview = null, playing = false, timer = null, slotEdited = false, captionEpoch = 0, frameEpoch = 0, frameOrigin = 0;
  const duration = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, "0")}`;
  function controls() {
    const unavailable = !signedIn || busy || loading || !state?.ready;
    for (const id of ["replay-play", "replay-pause", "replay-restart", "animation-apply", "animation-slot"]) $(id).disabled = unavailable;
    $("replay-preview-toggle").disabled = loading || (!preview && unavailable);
    $("replay-play").setAttribute("aria-pressed", String(state?.mode === "replay"));
    $("replay-pause").setAttribute("aria-pressed", String(state?.mode === "replay_pause"));
  }
  function render(value, operationBusy = false) {
    const changed = (state?.animation_slot||1)!==(value?.animation_slot||1);
    if(changed){preview=null;stop();}
    state = value; busy = operationBusy; const r = value?.replay;
    $("replay-studio").hidden = !r;
    if (!r) return;
    if(!slotEdited)$("animation-slot").value=String(value.animation_slot||1);
    text("replay-title",`${value.animation_slot||1} · ${r.title||'Neko wave'}`);
    text("replay-description",value.animation_slot===2?'A reaching paw and study reminders every 1.35 seconds. The white 203 stays fixed above the yellow line.':'Your original Neko wave, with the original white 203, spacing and yellow line.');
    text("replay-caption", r.caption);
    $("replay-progress").max = r.frames; $("replay-progress").value = r.frame;
    text("replay-position", `${r.active ? `${value.mode === "replay_pause" ? "Paused" : value.suspended ? "Paused for OTA" : "Playing"}` : "Ready to play"} · ${duration(r.frame * r.period_us / 1000000)} / ${duration(r.frames * r.period_us / 1000000)} · ${r.loops} loops`);
    text("replay-spec", `${r.frames} frames · ${(r.frames*r.period_us/1000000).toFixed(1)}-second loop · ${(1000000/r.period_us).toFixed(1)} fps · ${bytes(r.bytes)}. The cat plays from signed firmware; custom clips retain their separate storage.`);
    controls();
  }
  async function change(body) {
    if (busy || loading) return;
    loading = true; controls();
    try { await api("/display", { method: "PUT", body }); DisplayConsole.render(await api("/display")); text("replay-work", "Saved on the display. Your selection will survive a restart."); }
    catch (error) { text("replay-work", error.message); }
    finally { loading = false; controls(); }
  }
  $("animation-slot").addEventListener("change",()=>{slotEdited=true;});
  $("animation-apply").addEventListener("click",async()=>{
    const slot=Number($("animation-slot").value);await change({animation_slot:slot,mode:"replay"});slotEdited=false;
    if(state)$("animation-slot").value=String(state.animation_slot||1);
  });
  $("replay-play").addEventListener("click", () => change({ mode: "replay" }));
  $("replay-pause").addEventListener("click", () => change({ mode: "replay_pause" }));
  $("replay-restart").addEventListener("click", () => change({ mode: "replay", replay_chapter: 0 }));
  function stop() { playing = false; clearTimeout(timer); text("replay-preview-toggle", preview ? "Play local preview" : "Load local preview"); $("replay-preview-toggle").setAttribute("aria-pressed", "false"); }
  async function tick() {
    if (!playing || document.hidden) return stop();
    const now = performance.now(), elapsed = now - frameEpoch;
    try {
      await preview.seek((frameOrigin + Math.floor(elapsed / preview.period)) % preview.frames);
      if (!playing) return;
      preview.paint($("replay-preview"), EntranceReplay.captionIndex(now-captionEpoch), state?.rotation || 0);
    }
    catch (error) { text("replay-work", error.message); return stop(); }
    const frameDue = frameEpoch + (Math.floor(elapsed / preview.period) + 1) * preview.period;
    const captionDue = captionEpoch + (Math.floor((now-captionEpoch)/1350) + 1) * 1350;
    timer = setTimeout(tick, Math.max(1, Math.ceil(Math.min(frameDue, captionDue)-performance.now())));
  }
  $("replay-preview-toggle").addEventListener("click", async () => {
    if (playing) return stop();
    loading = true; controls(); let downloadTimer = null;
    try {
      if (!preview) {
        text("replay-work", "Loading the signed firmware's built-in replay for a local preview…");
        const controller = new AbortController();
        downloadTimer = setTimeout(() => controller.abort(), 60000);
        const response = await fetch("/api/v1/display/replay", { cache: "no-store", signal: controller.signal });
        if (!response.ok) throw new Error(response.status === 409 ? "Finish the firmware update before loading the preview." : "Could not load the replay. Sign in and retry.");
        const reader = response.body.getReader(), chunks = []; let size = 0;
        try { while (true) { const { value, done } = await reader.read(); if (done) break; size += value.length; if (size > EntranceReplay.MAX) { await reader.cancel(); throw new Error("Replay exceeds its size limit."); } chunks.push(value); } } finally { reader.releaseLock(); }
        const bytes = new Uint8Array(size); let at = 0; for (const chunk of chunks) { bytes.set(chunk, at); at += chunk.length; }
        const slot=Number(response.headers.get("X-Animation-Slot")||state?.animation_slot||1);
        if(slot!==(state?.animation_slot||1))throw new Error("Animation changed while loading. Try the preview again.");
        preview = new EntranceReplay.Replay(bytes,slot); await preview.seek(Math.min(state?.replay.frame || 0, preview.frames - 1));
      }
      captionEpoch=performance.now()-(state?.replay.caption_index||0)*1350;
      frameEpoch=performance.now(); frameOrigin=preview.frame;
      playing = true; text("replay-preview-toggle", "Pause local preview"); $("replay-preview-toggle").setAttribute("aria-pressed", "true");
      text("replay-work", "Local preview only. The physical device continues on its own timeline."); tick();
    } catch (error) { preview = null; text("replay-work", error.message); stop(); }
    finally { clearTimeout(downloadTimer); loading = false; controls(); }
  });
  document.addEventListener("visibilitychange", () => { if (document.hidden) stop(); });
  globalThis.ReplayConsole = { render };
})();
