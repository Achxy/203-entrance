"use strict";
(() => {
  const M = globalThis.EntranceMedia;
  let draft = null, state = null, working = false, saving = false, remoteBusy = false;
  let cancelled = false, previewPlaying = false, previewAt = 0, previewTimer = null, panelEdited = false;
  const canvas = $("display-preview"), context = canvas.getContext("2d");
  function controlState() {
    globalThis.ReplayConsole?.render(state, remoteBusy || saving);
    const unavailable = remoteBusy || saving || !signedIn || !state?.ready;
    $("clip-save").disabled = unavailable || working || !draft || draft.data.length > M.CAPACITY;
    $("clip-export").disabled = !draft || working || draft.data.length > M.CAPACITY;
    $("clip-prepare").disabled = working || saving;
    $("preview-toggle").disabled = !draft || draft.frames.length < 2;
    $("clip-load").disabled = unavailable || !state?.size || working;
    $("clip-remove").disabled = unavailable || !(state?.has_stored_clip || state?.size);
    document.querySelectorAll("[data-display-mode]").forEach((button) => {
      button.disabled = unavailable || (["play", "pause"].includes(button.dataset.displayMode) && !state?.size);
      button.setAttribute("aria-pressed", String(state?.mode === button.dataset.displayMode));
    });
    $("panel-form").querySelectorAll("input,select,button").forEach((input) => { input.disabled = unavailable; });
  }
  function render(value) {
    state = value;
    if (!value) { text("display-badge", "Firmware update required"); controlState(); return; }
    text("display-badge", !value.ready ? "Driver unavailable" : value.suspended ? "Paused for OTA" : `${value.mode} · SPI ready`);
    text("display-current", value.size ? `${value.title || "Saved clip"} · ${bytes(value.size)} · ${value.clip_width} × ${value.clip_height} · ${value.frames} frames at ${value.fps} fps · ${value.loops} loops` : "No saved clip. Prepare content above, then save it to the device.");
    text("display-error", value.error || (value.suspended ? "Playback resumes when the firmware operation completes." : "The preview is local; check the physical panel to confirm its appearance."));
    if (!panelEdited) {
      $("panel-profile").value = value.profile; $("panel-rotation").value = value.rotation;
      $("panel-inverted").value = String(value.inverted);
    }
    controlState();
  }
  function paint() {
    if (!draft) return;
    const frame = draft.frames[previewAt % draft.frames.length];
    canvas.width = draft.width; canvas.height = draft.height;
    canvas.classList.toggle("landscape", draft.width > draft.height);
    const image = context.createImageData(draft.width, draft.height), colors = draft.palette.map(M.rgb);
    for (let p = 0; p < frame.length; p++) { image.data.set(colors[frame[p]], p * 4); image.data[p * 4 + 3] = 255; }
    context.putImageData(image, 0, 0);
  }
  function stopPreview() {
    clearInterval(previewTimer); previewTimer = null; previewPlaying = false;
    text("preview-toggle", "Play preview"); $("preview-toggle").setAttribute("aria-pressed", "false");
  }
  function useDraft(value) {
    stopPreview(); draft = value; previewAt = 0; paint();
    text("preview-name", value.title || "Untitled clip");
    text("clip-summary", `${value.width} × ${value.height} · 16 colors · ${value.frames.length === 1 ? "Still image" : `${value.frames.length} frames · ${(value.frames.length / value.fps).toFixed(2)}s loop · ${value.fps} fps`}`);
    $("clip-budget").value = Math.min(value.data.length, M.CAPACITY);
    const over = value.data.length > M.CAPACITY;
    $("clip-budget-label").classList.toggle("over-budget", over);
    text("clip-budget-label", `${bytes(value.data.length)} / 12.0 KiB${over ? " — too large. Shorten the clip or reduce detail / frame rate." : " — fits persistent storage."}`);
    controlState();
  }
  function invalidate() {
    if (working) cancelled = true;
    stopPreview(); draft = null; controlState();
    text("clip-work", "Settings changed. Prepare a new preview before saving.");
  }
  function chooseSource() {
    const source = $("clip-source").value;
    $("clip-file-fields").hidden = source !== "file";
    $("clip-text-fields").hidden = source !== "text";
    $("clip-time-fields").hidden = source === "text";
    invalidate();
  }
  function waitEvent(element, event, action, timeout = 10000) {
    return new Promise((resolve, reject) => {
      const clear = () => { clearTimeout(timer); element.removeEventListener(event, success); element.removeEventListener("error", fail); };
      const success = () => { clear(); resolve(); };
      const fail = () => { clear(); reject(new Error("This browser could not decode the file. Try an MP4, WebM, PNG, JPEG, or WebP it supports.")); };
      const timer = setTimeout(() => { clear(); reject(new Error("Media decoding timed out. Try a shorter or smaller file.")); }, timeout);
      element.addEventListener(event, success, { once: true }); element.addEventListener("error", fail, { once: true });
      try { action(); } catch (error) { clear(); reject(error); }
    });
  }
  function frameCanvas(width, height) {
    const source = document.createElement("canvas"); source.width = width; source.height = height;
    return [source, source.getContext("2d", { willReadFrequently: true })];
  }
  function drawSource(ctx, source, width, height, sw, sh) {
    const scale = $("clip-fit").value === "cover" ? Math.max(width / sw, height / sh) : Math.min(width / sw, height / sh);
    ctx.fillStyle = "#000"; ctx.fillRect(0, 0, width, height);
    ctx.drawImage(source, (width - sw * scale) / 2, (height - sh * scale) / 2, sw * scale, sh * scale);
  }
  function messageFrame(ctx, width, height) {
    ctx.fillStyle = $("clip-background").value; ctx.fillRect(0, 0, width, height);
    ctx.fillStyle = $("clip-foreground").value; ctx.textAlign = "center"; ctx.textBaseline = "middle";
    const input = $("clip-message").value.trim();
    if (!input) throw new Error("Enter a message first.");
    let lines = [], size = Math.max(8, Math.floor(width / 7));
    for (; size >= 4; size--) {
      ctx.font = `600 ${size}px sans-serif`; lines = [];
      for (const paragraph of input.split("\n")) {
        let line = "";
        for (const char of paragraph) {
          if (ctx.measureText(line + char).width > width - 12 && line) { lines.push(line); line = ""; }
          line += char;
        }
        lines.push(line);
      }
      if (lines.length * size * 1.35 <= height - 12) break;
    }
    if (lines.length * size * 1.35 > height - 12) throw new Error("The message is too long at this detail level. Shorten it or choose a larger resolution.");
    lines.forEach((line, i) => ctx.fillText(line, width / 2, height / 2 + (i - (lines.length - 1) / 2) * size * 1.35));
  }
  async function prepare() {
    working = true; cancelled = false; controlState(); $("clip-stop-conversion").hidden = false;
    let objectURL = null, video = null;
    try {
      const source = $("clip-source").value, file = $("clip-file").files[0];
      if (source === "file" && !file) throw new Error("Choose an image or video first.");
      if (source === "file" && file.name.toLowerCase().endsWith(".eclip")) {
        if (file.size > M.CAPACITY) throw new Error("The clip exceeds 12 KiB.");
        useDraft(M.decode(await file.arrayBuffer())); text("clip-work", "Exported clip verified. Ready to save."); return;
      }
      let width = Number($("clip-resolution").value), height = width * 5 / 4;
      if (state?.rotation & 1) [width, height] = [height, width];
      const fps = Number($("clip-fps").value), start = Number($("clip-start").value), duration = Number($("clip-duration").value);
      if (!Number.isFinite(start) || start < 0 || start > 21600 || !Number.isFinite(duration) || duration < 0.1 || duration > 60) throw new Error("Choose a valid start and a length between 0.1 and 60 seconds.");
      const [buffer, ctx] = frameCanvas(width, height), frames = [];
      const capture = () => frames.push(ctx.getImageData(0, 0, width, height).data);
      if (source === "text") { messageFrame(ctx, width, height); capture(); }
      else if (source === "demo") {
        const count = Math.ceil(duration * fps);
        if (count > 120) throw new Error("Use at most 120 frames: shorten the duration or lower the frame rate.");
        for (let i = 0; i < count; i++) {
          ctx.fillStyle = "#17231c"; ctx.fillRect(0, 0, width, height);
          ctx.fillStyle = "#e5ba70"; const size = Math.max(3, Math.floor(width / 5));
          const x = Math.round((width - size - 4) * (0.5 + 0.5 * Math.sin(i * Math.PI * 2 / count)));
          ctx.fillRect(x + 2, Math.floor(height / 2 - size / 2), size, size);
          ctx.fillStyle = "#f5eee0"; ctx.fillRect(2, height - 6, width - 4, 2); capture();
        }
      } else {
        if (file.size > 128 * 1024 * 1024) throw new Error("Choose a source file smaller than 128 MiB.");
        objectURL = URL.createObjectURL(file);
        if (file.type.startsWith("image/")) {
          const image = new Image(); await waitEvent(image, "load", () => { image.src = objectURL; });
          if (image.naturalWidth * image.naturalHeight > 24000000) throw new Error("Resize the source image below 24 megapixels first.");
          drawSource(ctx, image, width, height, image.naturalWidth, image.naturalHeight); capture();
        } else {
          video = document.createElement("video"); video.muted = true; video.playsInline = true; video.preload = "auto";
          await waitEvent(video, "loadeddata", () => { video.src = objectURL; video.load(); });
          if (!Number.isFinite(video.duration) || !video.videoWidth || start >= video.duration) throw new Error("Choose a start time inside a video with a readable duration.");
          const count = Math.max(1, Math.ceil(Math.min(duration, video.duration - start) * fps));
          if (count > 120) throw new Error("Use at most 120 frames: shorten the duration or lower the frame rate.");
          for (let i = 0; i < count; i++) {
            if (cancelled) throw new Error("Conversion cancelled. The device was not changed.");
            const at = Math.min(start + i / fps, video.duration - 0.001);
            if (Math.abs(video.currentTime - at) > 0.0001) await waitEvent(video, "seeked", () => { video.currentTime = at; });
            drawSource(ctx, video, width, height, video.videoWidth, video.videoHeight); capture();
            text("clip-work", `Reading frame ${i + 1} of ${count}…`); await sleep(0);
          }
        }
      }
      text("clip-work", "Building the shared palette and compressing frames…"); await sleep(20);
      if (cancelled) throw new Error("Conversion cancelled. The device was not changed.");
      const value = M.encode({ width, height, fps: frames.length === 1 ? 1 : fps, frames, title: $("clip-title").value });
      useDraft(value); text("clip-work", value.data.length <= M.CAPACITY ? "Preview ready. Saving is a separate action." : "This draft is too large to save. Adjust the settings and prepare it again.");
      buffer.width = 1;
    } catch (error) { text("clip-work", error.message); }
    finally {
      if (video) { video.removeAttribute("src"); video.load(); }
      if (objectURL) URL.revokeObjectURL(objectURL);
      working = false; $("clip-stop-conversion").hidden = true; controlState();
    }
  }
  function download(data, name) {
    const url = URL.createObjectURL(new Blob([data], { type: "application/octet-stream" }));
    const link = document.createElement("a"); link.href = url; link.download = name; link.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }
  async function refresh() { render(await api("/display")); }
  async function change(body) {
    saving = true; controlState();
    try { await api("/display", { method: "PUT", body }); await refresh(); text("clip-work", "Display control saved."); }
    catch (error) { notice(`Display: ${error.message}`, true); }
    finally { saving = false; controlState(); }
  }
  $("clip-source").addEventListener("change", chooseSource);
  $("clip-form").addEventListener("input", invalidate);
  $("clip-file").addEventListener("change", invalidate);
  $("clip-form").addEventListener("submit", (event) => { event.preventDefault(); if (!working) prepare(); });
  $("clip-stop-conversion").addEventListener("click", () => { cancelled = true; });
  $("preview-toggle").addEventListener("click", () => {
    if (previewPlaying) return stopPreview();
    if (!draft) return;
    previewPlaying = true; text("preview-toggle", "Pause preview"); $("preview-toggle").setAttribute("aria-pressed", "true");
    previewTimer = setInterval(() => { previewAt++; paint(); }, 1000 / draft.fps);
  });
  document.addEventListener("visibilitychange", () => { if (document.hidden) stopPreview(); });
  $("clip-export").addEventListener("click", () => { if (draft) download(draft.data, "display.eclip"); });
  $("clip-save").addEventListener("click", async () => {
    if (!draft || saving || draft.data.length > M.CAPACITY) return;
    const candidate = draft; saving = true; controlState(); text("clip-work", "Saving clip to device storage…");
    try {
      const digest = await crypto.subtle.digest("SHA-256", candidate.data);
      const hash = Array.from(new Uint8Array(digest), (v) => v.toString(16).padStart(2, "0")).join("");
      try { await api("/display/clip", { method: "PUT", body: candidate.data, extra: { "Content-Type": "application/octet-stream" } }); }
      catch (error) {
        if (error.status) throw error;
        const saved = await api("/display");
        if (saved.sha256 !== hash) throw new Error("Save was not acknowledged. The previous clip may still be active; reconnect and load the saved clip before retrying.");
      }
      await refresh();
      if (state.sha256 !== hash) throw new Error("Saved content changed before verification. Load the saved clip and retry.");
      await api("/display", { method: "PUT", body: { mode: "play" } }); await refresh();
      text("clip-work", "Saved, verified, and playing. You can close this browser.");
    } catch (error) { text("clip-work", `Display: ${error.message}`); notice(`Display: ${error.message}`, true); }
    finally { saving = false; controlState(); }
  });
  document.querySelectorAll("[data-display-mode]").forEach((button) => button.addEventListener("click", () => change({ mode: button.dataset.displayMode })));
  $("panel-form").addEventListener("input", () => { panelEdited = true; });
  $("panel-form").addEventListener("submit", async (event) => {
    event.preventDefault();
    await change({ profile: $("panel-profile").value, rotation: Number($("panel-rotation").value), inverted: $("panel-inverted").value === "true" });
    panelEdited = false;
  });
  $("clip-load").addEventListener("click", async () => {
    saving = true; controlState();
    try {
      const response = await fetch("/api/v1/display/clip", { cache: "no-store", signal: AbortSignal.timeout(20000) });
      if (!response.ok) throw new Error("Cannot load the saved clip. Sign in and retry.");
      const length = Number(response.headers.get("content-length"));
      if (length > M.CAPACITY) throw new Error("Device returned an oversized clip.");
      useDraft(M.decode(await response.arrayBuffer())); text("clip-work", "Saved clip loaded into the local preview.");
    } catch (error) { notice(error.message, true); }
    finally { saving = false; controlState(); }
  });
  $("clip-remove").addEventListener("click", async () => {
    saving = true; controlState();
    try { await api("/display/clip", { method: "DELETE" }); await refresh(); text("clip-work", "Saved clip removed. Your local draft is still available."); }
    catch (error) { notice(error.message, true); }
    finally { saving = false; controlState(); }
  });
  globalThis.DisplayConsole = { render, controls: (busy) => { remoteBusy = busy; controlState(); } };
  chooseSource();
})();
