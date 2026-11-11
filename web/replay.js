/* Signed, bounded ECL1 loop preview. Frame colors match the LCD RGB565 output. */
"use strict";
(() => {
  const MAX = EntranceMedia.CAPACITY;
  const messages = ["Shenanigans by the 203 dudes", "Achyuth & Jaideep", "Knock the mario theme to unlock the door"];
  class Replay {
    constructor(bytes) {
      const clip=EntranceMedia.decode(bytes);
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
      const w = rotation & 1 ? 160 : 128, h = rotation & 1 ? 128 : 160, header = h === 160 ? 50 : 40, glyph = 10;
      canvas.width = w; canvas.height = h; canvas.classList.toggle("landscape", Boolean(rotation & 1));
      const ctx = canvas.getContext("2d"), im = ctx.createImageData(w, h); for (let i = 3; i < im.data.length; i += 4) im.data[i] = 255;
      let dw = w, dh = h - header - 1; if (w * this.height > dh * this.width) dw = Math.floor(dh * this.width / this.height); else dh = Math.floor(w * this.height / this.width); dw=Math.max(1,dw); dh=Math.max(1,dh);
      const left = Math.floor((w - dw) / 2), top = header + 1 + Math.floor((h - header - 1 - dh) / 2);
      for (let y = 0; y < dh; y++) { const row = this.row(Math.floor(y * this.height / dh)); for (let x = 0; x < dw; x++) { const rgb = this.palette[row[Math.floor(x * this.width / dw)]]; im.data.set(rgb, ((top + y) * w + left + x) * 4); } }
      const lines = []; let words = messages[captionIndex % 3].split(" "), line = "";
      for (const word of words) { if (line && line.length + word.length + 1 > Math.floor(w / glyph)) { lines.push(line); line = word; } else line += (line ? " " : "") + word; } if (line) lines.push(line);
      const textTop = Math.floor((header - lines.length * glyph) / 2);
      lines.forEach((line, row) => { const x0 = Math.floor((w - line.length * glyph) / 2); for (let i = 0; i < line.length; i++) for (let y = 0; y < glyph; y++) for (let x = 0; x < glyph; x++) if (font[line.charCodeAt(i) * 8 + Math.floor(y * 8 / glyph)] & 1 << Math.floor(x * 8 / glyph)) im.data.set([255,255,255,255], ((textTop + row * glyph + y) * w + x0 + i * glyph + x) * 4); });
      for (let x = 0; x < w; x++) im.data.set([255, 255, 0, 255], (header * w + x) * 4);
      ctx.putImageData(im, 0, 0);
    }
  }
  const font = Uint8Array.from(atob("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAGDw8GBgAGAA2NgAAAAAAADY2fzZ/NjYADD4DHjAfDAAAYzMYDGZjABw2HG47M24ABgYDAAAAAAAYDAYGBgwYAAYMGBgYDAYAAGY8/zxmAAAADAw/DAwAAAAAAAAADAwGAAAAPwAAAAAAAAAAAAwMAGAwGAwGAwEAPmNze29nPgAMDgwMDAw/AB4zMBwGMz8AHjMwHDAzHgA4PDYzfzB4AD8DHzAwMx4AHAYDHzMzHgA/MzAYDAwMAB4zMx4zMx4AHjMzPjAYDgAADAwAAAwMAAAMDAAADAwGGAwGAwYMGAAAAD8AAD8AAAYMGDAYDAYAHjMwGAwADAA+Y3t7ewMeAAweMzM/MzMAP2ZmPmZmPwA8ZgMDA2Y8AB82ZmZmNh8Af0YWHhZGfwB/RhYeFgYPADxmAwNzZnwAMzMzPzMzMwAeDAwMDAweAHgwMDAzMx4AZ2Y2HjZmZwAPBgYGRmZ/AGN3f39rY2MAY2dve3NjYwAcNmNjYzYcAD9mZj4GBg8AHjMzMzseOAA/ZmY+NmZnAB4zBw44Mx4APy0MDAwMHgAzMzMzMzM/ADMzMzMzHgwAY2Nja393YwBjYzYcHDZjADMzMx4MDB4Af2MxGExmfwAeBgYGBgYeAAMGDBgwYEAAHhgYGBgYHgAIHDZjAAAAAAAAAAAAAAD/DAwYAAAAAAAAAB4wPjNuAAcGBj5mZjsAAAAeMwMzHgA4MDA+MzNuAAAAHjM/Ax4AHDYGDwYGDwAAAG4zMz4wHwcGNm5mZmcADAAODAwMHgAwADAwMDMzHgcGZjYeNmcADgwMDAwMHgAAADN/f2tjAAAAHzMzMzMAAAAeMzMzHgAAADtmZj4GDwAAbjMzPjB4AAA7bmYGDwAAAD4DHjAfAAgMPgwMLBgAAAAzMzMzbgAAADMzMx4MAAAAY2t/fzYAAABjNhw2YwAAADMzMz4wHwAAPxkMJj8AOAwMBwwMOAAYGBgAGBgYAAcMDDgMDAcAbjsAAAAAAAAAAAAAAAAAAA=="), (c) => c.charCodeAt(0));
  globalThis.EntranceReplay = { Replay, messages, MAX };
})();

if (typeof document !== "undefined") (() => {
  let state = null, busy = false, loading = false, preview = null, playing = false, timer = null, previewEpoch = 0, previewCaption = 0;
  const duration = (seconds) => `${Math.floor(seconds / 60)}:${String(Math.floor(seconds % 60)).padStart(2, "0")}`;
  function controls() {
    const unavailable = !signedIn || busy || loading || !state?.ready;
    for (const id of ["replay-play", "replay-pause", "replay-restart"]) $(id).disabled = unavailable;
    $("replay-preview-toggle").disabled = loading || (!preview && unavailable);
    $("replay-play").setAttribute("aria-pressed", String(state?.mode === "replay"));
    $("replay-pause").setAttribute("aria-pressed", String(state?.mode === "replay_pause"));
  }
  function render(value, operationBusy = false) {
    state = value; busy = operationBusy; const r = value?.replay;
    $("replay-studio").hidden = !r;
    if (!r) return;
    text("replay-caption", r.caption);
    $("replay-progress").max = r.frames; $("replay-progress").value = r.frame;
    text("replay-position", `${r.active ? `${value.mode === "replay_pause" ? "Paused" : value.suspended ? "Paused for OTA" : "Playing"}` : "Ready to play"} · ${duration(r.frame * r.period_us / 1000000)} / ${duration(r.frames * r.period_us / 1000000)} · ${r.loops} loops`);
    text("replay-spec", `${r.frames} frames · ${(r.frames*r.period_us/1000000).toFixed(1)}-second loop · ${(1000000/r.period_us).toFixed(1)} fps · ${bytes(r.bytes)}. The cat plays from signed firmware; custom clips retain their separate storage.`);
    controls();
  }
  async function change(body) {
    if (busy || loading) return;
    loading = true; controls();
    try { await api("/display", { method: "PUT", body }); DisplayConsole.render(await api("/display")); text("replay-work", "Device playback updated. Caption timing continues independently."); }
    catch (error) { text("replay-work", error.message); }
    finally { loading = false; controls(); }
  }
  $("replay-play").addEventListener("click", () => change({ mode: "replay" }));
  $("replay-pause").addEventListener("click", () => change({ mode: "replay_pause" }));
  $("replay-restart").addEventListener("click", () => change({ mode: "replay", replay_chapter: 0 }));
  function stop() { playing = false; clearTimeout(timer); text("replay-preview-toggle", preview ? "Play local preview" : "Load local preview"); $("replay-preview-toggle").setAttribute("aria-pressed", "false"); }
  async function tick() {
    if (!playing || document.hidden) return stop();
    try { preview.paint($("replay-preview"), (previewCaption + Math.floor((performance.now() - previewEpoch) / 2000)) % 3, state?.rotation || 0); await preview.next(); }
    catch (error) { text("replay-work", error.message); return stop(); }
    timer = setTimeout(tick, preview.period);
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
        preview = new EntranceReplay.Replay(bytes); await preview.seek(Math.min(state?.replay.frame || 0, preview.frames - 1));
      }
      previewEpoch = performance.now(); previewCaption = state?.replay.caption_index || 0;
      playing = true; text("replay-preview-toggle", "Pause local preview"); $("replay-preview-toggle").setAttribute("aria-pressed", "true");
      text("replay-work", "Local preview only. The physical device continues on its own timeline."); tick();
    } catch (error) { preview = null; text("replay-work", error.message); stop(); }
    finally { clearTimeout(downloadTimer); loading = false; controls(); }
  });
  document.addEventListener("visibilitychange", () => { if (document.hidden) stop(); });
  globalThis.ReplayConsole = { render };
})();
