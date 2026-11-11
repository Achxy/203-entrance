/* ECL1: bounded 16-color RGB565 clips. Shared by the editor and codec tests. */
"use strict";
(() => {
  const CAPACITY = 12288, HEADER = 80, MAX_PIXELS = 20480, MAX_FRAMES = 120;
  const crc32 = (data) => {
    let crc = 0xffffffff;
    for (const byte of data) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (~crc) >>> 0;
  };
  const rgb565 = (r, g, b) => ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
  const rgb = (value) => [Math.round((value >> 11) * 255 / 31), Math.round(((value >> 5) & 63) * 255 / 63), Math.round((value & 31) * 255 / 31)];
  function bounds(width, height, fps, count) {
    if (![width, height, fps, count].every(Number.isInteger) || width < 1 || height < 1 ||
        width > 160 || height > 160 || width * height > MAX_PIXELS || fps < 1 || fps > 12 ||
        count < 1 || count > MAX_FRAMES || count > fps * 60) throw new Error("Clip dimensions, frame count, or frame rate are outside the supported limits.");
  }
  function paletteFor(frames) {
    const histogram = new Uint32Array(65536);
    for (const frame of frames) for (let p = 0; p < frame.length; p += 4)
      histogram[rgb565(frame[p], frame[p + 1], frame[p + 2])]++;
    let buckets = [[]];
    for (let c = 0; c < histogram.length; c++) if (histogram[c]) buckets[0].push({ rgb: rgb(c), n: histogram[c] });
    const spread = (bucket) => [0, 1, 2].map((axis) => {
      let lo = 255, hi = 0;
      for (const point of bucket) { lo = Math.min(lo, point.rgb[axis]); hi = Math.max(hi, point.rgb[axis]); }
      return hi - lo;
    });
    while (buckets.length < 16) {
      let chosen = -1, score = -1, axis = 0;
      buckets.forEach((bucket, index) => {
        if (bucket.length < 2) return;
        const range = spread(bucket), a = range.indexOf(Math.max(...range));
        const value = range[a] * Math.sqrt(bucket.reduce((sum, p) => sum + p.n, 0));
        if (value > score) { chosen = index; score = value; axis = a; }
      });
      if (chosen < 0) break;
      const bucket = buckets.splice(chosen, 1)[0].sort((a, b) => a.rgb[axis] - b.rgb[axis]);
      const half = bucket.reduce((sum, p) => sum + p.n, 0) / 2;
      let sum = 0, at = 0;
      do { sum += bucket[at++].n; } while (sum < half && at < bucket.length - 1);
      buckets.push(bucket.slice(0, at), bucket.slice(at));
    }
    const palette = buckets.map((bucket) => {
      let count = 0, r = 0, g = 0, b = 0;
      for (const point of bucket) { count += point.n; r += point.rgb[0] * point.n; g += point.rgb[1] * point.n; b += point.rgb[2] * point.n; }
      return rgb565(Math.round(r / count), Math.round(g / count), Math.round(b / count));
    });
    while (palette.length < 16) palette.push(0);
    return palette;
  }
  function encodeIndexed({ width, height, fps, frames, palette, title = "Untitled" }) {
    bounds(width, height, fps, frames.length);
    if (palette.length !== 16 || palette.some((v) => !Number.isInteger(v) || v < 0 || v > 65535)) throw new Error("Invalid palette.");
    const payloads = [];
    let previous = new Uint8Array(width * height);
    for (let index = 0; index < frames.length; index++) {
      const frame = frames[index];
      if (frame.length !== previous.length || frame.some((v) => v > 15)) throw new Error("Invalid indexed frame.");
      const raw = new Uint8Array(1 + Math.ceil(frame.length / 2));
      for (let p = 0; p < frame.length; p++) raw[1 + (p >> 1)] |= frame[p] << (p & 1 ? 0 : 4);
      const delta = [1];
      for (let p = 0; p < frame.length;) {
        let n = 1;
        if (frame[p] === previous[p]) {
          while (n < 128 && p + n < frame.length && frame[p + n] === previous[p + n]) n++;
          delta.push(n - 1);
        } else {
          while (n < 128 && p + n < frame.length && frame[p + n] === frame[p]) n++;
          delta.push(128 | (n - 1), frame[p]);
        }
        p += n;
      }
      payloads.push(index && delta.length < raw.length ? new Uint8Array(delta) : raw);
      previous = frame;
    }
    const size = HEADER + payloads.reduce((sum, frame) => sum + frame.length + 2, 0);
    const data = new Uint8Array(size), view = new DataView(data.buffer);
    data.set([69, 67, 76, 49]); view.setUint32(4, size, true);
    data[12] = width; data[13] = height; data[14] = fps; view.setUint16(16, frames.length, true);
    const label = title.replace(/[^\x20-\x7e]/g, "_").slice(0, 27);
    for (let i = 0; i < label.length; i++) data[20 + i] = label.charCodeAt(i);
    palette.forEach((value, index) => view.setUint16(48 + index * 2, value, true));
    let at = HEADER;
    for (const frame of payloads) { view.setUint16(at, frame.length, true); data.set(frame, at + 2); at += frame.length + 2; }
    view.setUint32(8, crc32(data.subarray(12)), true);
    return { data, width, height, fps, frames, palette, title: label };
  }
  function encode({ width, height, fps, frames, title }) {
    bounds(width, height, fps, frames.length);
    if (frames.some((f) => f.length !== width * height * 4)) throw new Error("Invalid source frame.");
    const palette = paletteFor(frames), colors = palette.map(rgb);
    const cache = new Int16Array(65536); cache.fill(-1);
    const indexed = frames.map((frame) => {
      const result = new Uint8Array(width * height);
      for (let p = 0; p < result.length; p++) {
        const key = rgb565(frame[p * 4], frame[p * 4 + 1], frame[p * 4 + 2]);
        if (cache[key] < 0) {
          const color = rgb(key); let distance = Infinity, chosen = 0;
          colors.forEach((value, i) => {
            const d = value.reduce((sum, channel, axis) => sum + (channel - color[axis]) ** 2, 0);
            if (d < distance) { distance = d; chosen = i; }
          });
          cache[key] = chosen;
        }
        result[p] = cache[key];
      }
      return result;
    });
    return encodeIndexed({ width, height, fps, frames: indexed, palette, title });
  }
  function decode(input) {
    const data = input instanceof Uint8Array ? input : new Uint8Array(input);
    if (data.length < HEADER + 3 || data.length > CAPACITY) throw new Error("Clip must fit the 12 KiB storage budget.");
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    if (String.fromCharCode(...data.slice(0, 4)) !== "ECL1" || view.getUint32(4, true) !== data.length ||
        view.getUint32(8, true) !== crc32(data.subarray(12)) || data[15] || data[18] || data[19] || data[47]) throw new Error("Clip header or checksum is invalid.");
    const width = data[12], height = data[13], fps = data[14], count = view.getUint16(16, true);
    bounds(width, height, fps, count);
    let title = "", ended = false;
    for (const char of data.slice(20, 48)) {
      if (!char) ended = true;
      else if (ended || char < 32 || char > 126) throw new Error("Invalid clip label.");
      else title += String.fromCharCode(char);
    }
    const palette = Array.from({ length: 16 }, (_, i) => view.getUint16(48 + i * 2, true));
    const frames = []; let at = HEADER, previous = new Uint8Array(width * height);
    for (let i = 0; i < count; i++) {
      if (at + 3 > data.length) throw new Error("Truncated clip frame.");
      const length = view.getUint16(at, true); at += 2;
      const end = at + length, mode = data[at++];
      if (length < 1 || end > data.length || (!i && mode !== 0)) throw new Error("Invalid clip frame.");
      const frame = previous.slice();
      if (mode === 0) {
        if (end - at !== Math.ceil(frame.length / 2) || (frame.length & 1 && data[end - 1] & 15)) throw new Error("Invalid raw frame length.");
        for (let p = 0; p < frame.length; p++) frame[p] = (data[at + (p >> 1)] >> (p & 1 ? 0 : 4)) & 15;
        at = end;
      } else if (mode === 1) {
        let p = 0;
        while (at < end) {
          const token = data[at++], n = (token & 127) + 1;
          if (p + n > frame.length) throw new Error("Frame runs exceed the image.");
          if (token & 128) {
            if (at === end || data[at] > 15) throw new Error("Invalid frame color.");
            frame.fill(data[at++], p, p + n);
          }
          p += n;
        }
        if (p !== frame.length) throw new Error("Incomplete frame.");
      } else throw new Error("Unsupported frame encoding.");
      frames.push(frame); previous = frame;
    }
    if (at !== data.length) throw new Error("Unexpected bytes after clip.");
    return { data, width, height, fps, frames, palette, title };
  }
  globalThis.EntranceMedia = { CAPACITY, HEADER, crc32, rgb, encode, encodeIndexed, decode };
})();
