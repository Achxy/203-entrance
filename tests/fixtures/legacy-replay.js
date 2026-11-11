"use strict";
(() => {
  const MAX = 737280, SCENE = 2432;
  const messages = ["Shenanigans by the 203 dudes", "Achyuth & Jaideep", "Knock the mario theme to unlock the door"];
  const fail = () => { throw new Error("The built-in replay is invalid. Reinstall verified firmware."); };
  class Replay {
    constructor(bytes) {
      this.data = new Uint8Array(bytes); this.view = new DataView(this.data.buffer, this.data.byteOffset, this.data.byteLength);
      const d = this.data, v = this.view, u16 = (a) => v.getUint16(a, true), u32 = (a) => v.getUint32(a, true);
      if (d.length < 64 || d.length > MAX || String.fromCharCode(...d.slice(0, 4)) !== "RPL1" || u32(4) !== d.length || u32(8) !== EntranceMedia.crc32(d.subarray(12)) || u16(12) !== 1 || u16(14) !== SCENE || u16(24) !== 16384 || u16(30)) fail();
      this.frames = u32(16); this.period = u32(20) / 1000; this.blockCount = u16(26); this.chapterCount = u16(28);
      this.index = u32(40); this.chapterOffset = u32(44);
      if (!this.frames || this.frames > 100000 || this.period < 50 || this.period > 1000 || !this.blockCount || this.blockCount > this.frames || !this.chapterCount || this.chapterCount > 32 || u32(32) !== 64 || u32(36) !== 8256 || this.index !== 8384 || this.chapterOffset !== this.index + this.blockCount * 16 || u32(48) !== this.chapterOffset + this.chapterCount * 8 || u32(48) > d.length || d.slice(52, 64).some(Boolean)) fail();
      this.blocks = []; let frame = 0, offset = u32(48);
      for (let i = 0; i < this.blockCount; i++) {
        const a = this.index + i * 16, b = { first: u32(a), offset: u32(a + 4), compressed: u16(a + 8), raw: u16(a + 10), frames: u16(a + 12) };
        if (b.first !== frame || b.offset !== offset || !b.compressed || offset + b.compressed > d.length || b.raw < SCENE || b.raw > 16384 || !b.frames || b.frames > 128 || u16(a + 14)) fail();
        frame += b.frames; offset += b.compressed; this.blocks.push(b);
      }
      if (frame !== this.frames || offset !== d.length) fail();
      this.chapters = [];
      for (let i = 0; i < this.chapterCount; i++) {
        const a = this.chapterOffset + i * 8, frame = u32(a);
        if (frame >= this.frames || (!i && frame) || (i && frame <= this.chapters[i - 1].frame) || d[a + 4] > 7 || d[a + 5] > 3 || d[a + 6] || d[a + 7]) fail();
        this.chapters.push({ frame, world: d[a + 4] + 1, level: d[a + 5] + 1 });
      }
      this.tiles = d.subarray(64, 8256); this.palette = Array.from({ length: 64 }, (_, i) => EntranceMedia.rgb(u16(8256 + i * 2)));
      this.scene = new Uint8Array(SCENE); this.frame = 0;
    }
    validateScene() {
      const s = this.scene, n = s[2404];
      if (!n || n > 8 || s[2340] || s[2405] > 7 || s[2406] > 3 || s.slice(2304, 2336).some((c) => c > 63)) fail();
      for (let i = 0; i < n; i++) { const a = 2340 + i * 8; if (s[a] >= 240 || (i && s[a] <= s[a - 8]) || (s[a + 2] | s[a + 3] << 8) >= 512 || (s[a + 4] | s[a + 5] << 8) >= 240 || s[a + 7] & 224) fail(); }
    }
    async load(block) {
      if (!(block >= 0 && block < this.blocks.length) || typeof DecompressionStream === "undefined") throw new Error("Replay preview needs a browser with DecompressionStream support.");
      const b = this.blocks[block], stream = new Blob([this.data.slice(b.offset, b.offset + b.compressed)]).stream().pipeThrough(new DecompressionStream("deflate"));
      const reader = stream.getReader(), decoded = new Uint8Array(b.raw); let at = 0;
      try { while (true) { const { value, done } = await reader.read(); if (done) break; if (at + value.length > decoded.length) { await reader.cancel(); fail(); } decoded.set(value, at); at += value.length; } } finally { reader.releaseLock(); }
      if (at !== b.raw || (b.frames === 1 && b.raw !== SCENE)) fail();
      this.decoded = decoded; this.cursor = SCENE; this.block = block; this.local = 0; this.frame = b.first; this.scene.set(decoded.subarray(0, SCENE)); this.validateScene();
    }
    step() {
      const d = this.decoded, b = this.blocks[this.block];
      if (this.local + 1 >= b.frames || this.cursor + 2 > d.length) fail();
      const end = this.cursor + 2 + (d[this.cursor] | d[this.cursor + 1] << 8); this.cursor += 2;
      if (end > d.length) fail();
      const variable = () => { if (this.cursor >= end) fail(); const a = d[this.cursor++]; let n = a & 127; if (a & 128) { if (this.cursor >= end || d[this.cursor] > 19) fail(); n |= d[this.cursor++] << 7; } if (n > SCENE) fail(); return n; };
      let pos = 0;
      while (this.cursor < end) { pos += variable(); const n = variable(); if (!n || pos + n > SCENE || this.cursor + n > end) fail(); for (let i = 0; i < n; i++) this.scene[pos++] += d[this.cursor++]; }
      this.local++; this.frame++; this.validateScene();
      if (this.local + 1 === b.frames && this.cursor !== d.length) fail();
    }
    async seek(frame) { if (!Number.isInteger(frame) || frame < 0 || frame >= this.frames) fail(); let block = this.blocks.findLastIndex((b) => b.first <= frame); await this.load(block); while (this.frame < frame) this.step(); }
    async next() {
      if (this.frame + 1 === this.frames) { if (this.cursor !== this.decoded.length) fail(); await this.load(0); }
      else if (this.local + 1 === this.blocks[this.block].frames) { if (this.cursor !== this.decoded.length) fail(); await this.load(this.block + 1); }
      else this.step();
    }
    row(y) {
      const s = this.scene, t = this.tiles, out = new Uint8Array(256), opaque = new Uint8Array(256), occupied = new Uint8Array(256);
      let at = 2340; for (let i = 1; i < s[2404]; i++) if (s[2340 + i * 8] <= y) at = 2340 + i * 8;
      const scroll = s[at + 2] | s[at + 3] << 8, sy = ((s[at + 4] | s[at + 5] << 8) + y - s[at]) % 240, ctrl = s[at + 6], mask = s[at + 7];
      const pixel = (tile, x, y) => (t[tile * 16 + y] >> (7 - x) & 1) | (t[tile * 16 + y + 8] >> (7 - x) & 1) << 1;
      for (let x = 0; x < 256; x++) {
        let color = 0;
        if (mask & 8 && (x >= 8 || mask & 2)) {
          const sx = x + scroll & 511, nt = (sx >> 8) * 1024, tx = (sx & 255) >> 3, ty = sy >> 3, tile = s[nt + ty * 32 + tx] + (ctrl & 16 ? 256 : 0), p = pixel(tile, sx & 7, sy & 7);
          if (p) { const bank = s[nt + 960 + (ty >> 2) * 8 + (tx >> 2)] >> ((ty & 2) * 2 + (tx & 2)) & 3; color = bank * 4 + p; opaque[x] = 1; }
        }
        out[x] = s[2304 + color];
      }
      if (mask & 16) {
        let count = 0; const height = ctrl & 32 ? 16 : 8;
        for (let i = 0; i < 64 && count < 8; i++) {
          const a = 2048 + i * 4, dy = y - s[a] - 1; if (dy < 0 || dy >= height) continue; count++;
          const attr = s[a + 2], row = attr & 128 ? height - 1 - dy : dy; let tile = s[a + 1];
          tile = height === 16 ? (tile & 254) + (tile & 1 ? 256 : 0) + (row >> 3) : tile + (ctrl & 8 ? 256 : 0);
          for (let dx = 0; dx < 8 && s[a + 3] + dx < 256; dx++) { const x = s[a + 3] + dx; if (occupied[x] || x < 8 && !(mask & 4)) continue; const p = pixel(tile, attr & 64 ? 7 - dx : dx, row & 7); if (!p) continue; occupied[x] = 1; if (!(attr & 32) || !opaque[x]) out[x] = s[2320 + (attr & 3) * 4 + p]; }
        }
      }
      if (mask & 1) for (let x = 0; x < 256; x++) out[x] &= 48;
      return out;
    }
    paint(canvas, captionIndex = 0, rotation = 0) {
      const w = rotation & 1 ? 160 : 128, h = rotation & 1 ? 128 : 160, header = h === 160 ? 50 : 40, glyph = 10;
      canvas.width = w; canvas.height = h; canvas.classList.toggle("landscape", Boolean(rotation & 1));
      const ctx = canvas.getContext("2d"), im = ctx.createImageData(w, h); for (let i = 3; i < im.data.length; i += 4) im.data[i] = 255;
      let dw = w, dh = h - header - 1; if (w * 240 > dh * 256) dw = Math.floor(dh * 256 / 240); else dh = Math.floor(w * 240 / 256);
      const left = Math.floor((w - dw) / 2), top = header + 1 + Math.floor((h - header - 1 - dh) / 2), fade = Math.min(8, this.frame, this.frames - 1 - this.frame) / 8;
      for (let y = 0; y < dh; y++) { const row = this.row(Math.floor(y * 240 / dh)); for (let x = 0; x < dw; x++) { const rgb = this.palette[row[Math.floor(x * 256 / dw)]]; im.data.set(rgb.map((v) => Math.floor(v * fade)), ((top + y) * w + left + x) * 4); } }
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
    for (const id of ["replay-play", "replay-pause", "replay-restart", "replay-chapter"]) $(id).disabled = unavailable;
    $("replay-chapter-form").querySelector("button").disabled = unavailable;
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
    text("replay-position", `${r.active ? `${value.mode === "replay_pause" ? "Paused" : value.suspended ? "Paused for OTA" : "Playing"} · World ${r.world}-${r.level}` : "Ready to play"} · ${duration(r.frame * r.period_us / 1000000)} / ${duration(r.frames * r.period_us / 1000000)} · ${r.loops} loops`);
    text("replay-spec", `${r.chapters} levels · ${(1000000 / r.period_us).toFixed(1)} fps · ${bytes(r.bytes)} in signed firmware. Saved custom clips keep their separate 12 KiB allowance. The full game stays visible at its original proportions, below the yellow divider.`);
    const select = $("replay-chapter"); if (select.options.length !== r.chapters) { select.replaceChildren(); for (let i = 0; i < r.chapters; i++) select.add(new Option(`World ${Math.floor(i / 4) + 1}-${i % 4 + 1}`, String(i))); }
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
  $("replay-chapter-form").addEventListener("submit", (event) => { event.preventDefault(); change({ mode: "replay", replay_chapter: Number($("replay-chapter").value) }); });
  function stop() { playing = false; clearTimeout(timer); text("replay-preview-toggle", preview ? "Play local preview" : "Load local preview"); $("replay-preview-toggle").setAttribute("aria-pressed", "false"); }
  async function tick() {
    if (!playing || document.hidden) return stop();
    try { preview.paint($("replay-preview"), (previewCaption + Math.floor((performance.now() - previewEpoch) / 2000)) % 3, state?.rotation || 0); await preview.next(); }
    catch (error) { text("replay-work", error.message); return stop(); }
    timer = setTimeout(tick, preview.period);
  }
  $("replay-preview-toggle").addEventListener("click", async () => {
    if (playing) return stop();
    loading = true; controls();
    try {
      if (!preview) {
        text("replay-work", "Loading the signed firmware's built-in replay for a local preview…");
        const response = await fetch("/api/v1/display/replay", { cache: "no-store", signal: AbortSignal.timeout(60000) });
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
    finally { loading = false; controls(); }
  });
  document.addEventListener("visibilitychange", () => { if (document.hidden) stop(); });
  globalThis.ReplayConsole = { render };
})();
