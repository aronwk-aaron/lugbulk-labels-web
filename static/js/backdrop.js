// Port of src/image_backdrop.cpp: sets light parts (trans, white) on a
// light gray rounded tile so they show up on a printed label. The pixel
// math is a pure function over RGB arrays (runs in Node for the parity
// tests); backdropJpeg() is the thin browser glue that decodes and
// re-encodes with createImageBitmap / OffscreenCanvas.

const TILE = [218, 220, 224];
const BG_TOLERANCE = 4;
const HOLE_MIN = 0.02;
const SPECK_MIN = 0.15;
const SCALE = 2;
const TRANS_GAIN = 1.6;
const FAINT_PART_MIN = 150;

const clamp = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
// std::lround, for the non-negative values used here.
const lround = (v) => Math.round(v);

function rankFilter(inp, w, h, dilate, radius) {
  const out = new Uint8Array(inp.length);
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) {
      let m = dilate ? 0 : 255;
      for (let dy = -radius; dy <= radius; ++dy) {
        const row = clamp(y + dy, 0, h - 1) * w;
        for (let dx = -radius; dx <= radius; ++dx) {
          const n = inp[row + clamp(x + dx, 0, w - 1)];
          m = dilate ? Math.max(m, n) : Math.min(m, n);
        }
      }
      out[y * w + x] = m;
    }
  }
  return out;
}

function gaussianBlur(inp, w, h, sigma) {
  const r = Math.max(1, Math.ceil(sigma * 3));
  const k = new Float64Array(2 * r + 1);
  let sum = 0;
  for (let i = -r; i <= r; ++i) sum += k[i + r] = Math.exp(-(i * i) / (2 * sigma * sigma));
  for (let i = 0; i < k.length; ++i) k[i] /= sum;
  const tmp = new Float64Array(inp.length);
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) {
      let acc = 0;
      for (let i = -r; i <= r; ++i) acc += k[i + r] * inp[y * w + clamp(x + i, 0, w - 1)];
      tmp[y * w + x] = acc;
    }
  }
  const out = new Uint8Array(inp.length);
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) {
      let acc = 0;
      for (let i = -r; i <= r; ++i) acc += k[i + r] * tmp[clamp(y + i, 0, h - 1) * w + x];
      out[y * w + x] = clamp(lround(acc), 0, 255);
    }
  }
  return out;
}

function upscale(src, w, h, channels, scale) {
  const W = w * scale;
  const H = h * scale;
  const out = new Uint8Array(W * H * channels);
  for (let y = 0; y < H; ++y) {
    const sy = clamp((y + 0.5) / scale - 0.5, 0, h - 1);
    const y0 = Math.trunc(sy);
    const y1 = Math.min(y0 + 1, h - 1);
    const fy = sy - y0;
    for (let x = 0; x < W; ++x) {
      const sx = clamp((x + 0.5) / scale - 0.5, 0, w - 1);
      const x0 = Math.trunc(sx);
      const x1 = Math.min(x0 + 1, w - 1);
      const fx = sx - x0;
      for (let c = 0; c < channels; ++c) {
        const p = (xx, yy) => src[(yy * w + xx) * channels + c];
        const v = (p(x0, y0) * (1 - fx) + p(x1, y0) * fx) * (1 - fy) +
                  (p(x0, y1) * (1 - fx) + p(x1, y1) * fx) * fy;
        out[(y * W + x) * channels + c] = lround(v);
      }
    }
  }
  return out;
}

// Connected 4-neighbour regions where inRegion(i) is true, in scan order.
function regions(w, h, inRegion) {
  const seen = new Uint8Array(w * h);
  const out = [];
  const queue = new Int32Array(w * h);
  for (let start = 0; start < w * h; ++start) {
    if (seen[start] || !inRegion(start)) continue;
    const pixels = [];
    let head = 0;
    let tail = 0;
    queue[tail++] = start;
    seen[start] = 1;
    while (head < tail) {
      const i = queue[head++];
      pixels.push(i);
      const x = i % w;
      const y = (i - x) / w;
      const next = [[x + 1, y], [x - 1, y], [x, y + 1], [x, y - 1]];
      for (const [nx, ny] of next) {
        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const j = ny * w + nx;
        if (!seen[j] && inRegion(j)) {
          seen[j] = 1;
          queue[tail++] = j;
        }
      }
    }
    out.push(pixels);
  }
  return out;
}

function partMask(px, w, h) {
  const white = (i) => 255 - Math.min(px[i * 3], px[i * 3 + 1], px[i * 3 + 2]) <= BG_TOLERANCE;
  let part = new Uint8Array(w * h).fill(255);
  for (const region of regions(w, h, white)) {
    const border = region.some((i) => {
      const x = i % w;
      const y = (i - x) / w;
      return x === 0 || y === 0 || x === w - 1 || y === h - 1;
    });
    if (border || region.length >= HOLE_MIN * w * h) for (const i of region) part[i] = 0;
  }
  part = rankFilter(rankFilter(part, w, h, false, 1), w, h, true, 1);
  const blobs = regions(w, h, (i) => part[i] !== 0);
  let largest = 0;
  for (const b of blobs) largest = Math.max(largest, b.length);
  for (const b of blobs) if (b.length < SPECK_MIN * largest) for (const i of b) part[i] = 0;
  return rankFilter(rankFilter(part, w, h, true, 3), w, h, false, 3);
}

function isFaint(px, part) {
  const hist = new Array(256).fill(0);
  let total = 0;
  for (let i = 0; i < part.length; ++i) {
    if (!part[i]) continue;
    hist[Math.trunc((px[i * 3] + px[i * 3 + 1] + px[i * 3 + 2]) / 3)]++;
    total++;
  }
  let seen = 0;
  for (let level = 0; level < 256; ++level) {
    seen += hist[level];
    if (seen >= total * 0.05) return level >= FAINT_PART_MIN;
  }
  return false;
}

function roundedTile(w, h) {
  const t = new Uint8Array(w * h);
  const r = w / 10;
  for (let y = 0; y < h; ++y) {
    for (let x = 0; x < w; ++x) {
      const cx = clamp(x + 0.5, r, w - r);
      const cy = clamp(y + 0.5, r, h - r);
      const d = Math.hypot(x + 0.5 - cx, y + 0.5 - cy);
      // static_cast<uint8_t> of a clamped double truncates.
      t[y * w + x] = Math.trunc(clamp((r - d + 0.5) * 255, 0, 255));
    }
  }
  return t;
}

// RGBA (canvas ImageData) to packed RGB.
export function rgbaToRgb(rgba) {
  const n = rgba.length / 4;
  const out = new Uint8Array(n * 3);
  for (let i = 0; i < n; ++i) {
    out[i * 3] = rgba[i * 4];
    out[i * 3 + 1] = rgba[i * 4 + 1];
    out[i * 3 + 2] = rgba[i * 4 + 2];
  }
  return out;
}

// image_backdrop::backdrop. `rgb` is w*h*3 bytes. Returns {width, height,
// pixels (RGB)} at 2x the size, or null when the photo is fine as it is.
export function backdropPixels(rgb, w, h, trans, lightColor) {
  if (w <= 0 || h <= 0) return null;
  let part = null;
  if (!trans) {
    part = partMask(rgb, w, h);
    if (!lightColor && !isFaint(rgb, part)) return null;
  }
  const W = w * SCALE;
  const H = h * SCALE;
  const photo = upscale(rgb, w, h, 3, SCALE);
  if (trans) {
    // Deepen the faint edge lines a little; the darkest pixel is the part's.
    let darkest = 255;
    for (let i = 0; i < rgb.length; i += 3) {
      darkest = Math.min(darkest, Math.trunc((rgb[i] + rgb[i + 1] + rgb[i + 2]) / 3));
    }
    const gain = Math.min(TRANS_GAIN, 150 / Math.max(1, 255 - darkest));
    for (let i = 0; i < photo.length; ++i) {
      photo[i] = Math.trunc(clamp(255 - (255 - photo[i]) * gain, 0, 255));
    }
  }
  let mask = null;
  if (part) {
    const m = gaussianBlur(upscale(part, w, h, 1, SCALE), W, H, SCALE * 0.8);
    for (let i = 0; i < m.length; ++i) m[i] = m[i] >= 128 ? 255 : 0;
    mask = gaussianBlur(rankFilter(m, W, H, false, 2), W, H, SCALE * 0.5);
  }
  const tile = roundedTile(W, H);
  const out = new Uint8Array(photo.length);
  for (let i = 0; i < tile.length; ++i) {
    const t = tile[i] / 255;
    const m = mask ? (mask[i] / 255) * t : 0;
    for (let c = 0; c < 3; ++c) {
      const p = photo[i * 3 + c];
      const multiplied = (p * TILE[c]) / 255;
      let v = 255 * (1 - t) + multiplied * t; // tile over white
      v = v * (1 - m) + p * m; // white part on top
      out[i * 3 + c] = lround(v);
    }
  }
  return { width: W, height: H, pixels: out };
}

// Browser glue: JPEG bytes in, JPEG bytes out, or null to use the photo as
// it is (also when the browser lacks OffscreenCanvas). Works in a worker.
export async function backdropJpeg(jpegBytes, trans, lightColor) {
  if (typeof createImageBitmap !== 'function' || typeof OffscreenCanvas !== 'function') return null;
  const bitmap = await createImageBitmap(new Blob([jpegBytes], { type: 'image/jpeg' }));
  const w = bitmap.width;
  const h = bitmap.height;
  const canvas = new OffscreenCanvas(w, h);
  const ctx = canvas.getContext('2d');
  ctx.drawImage(bitmap, 0, 0);
  if (bitmap.close) bitmap.close();
  const tiled = backdropPixels(rgbaToRgb(ctx.getImageData(0, 0, w, h).data), w, h, trans, lightColor);
  if (!tiled) return null;
  const out = new OffscreenCanvas(tiled.width, tiled.height);
  const octx = out.getContext('2d');
  const img = octx.createImageData(tiled.width, tiled.height);
  for (let i = 0, n = tiled.width * tiled.height; i < n; ++i) {
    img.data[i * 4] = tiled.pixels[i * 3];
    img.data[i * 4 + 1] = tiled.pixels[i * 3 + 1];
    img.data[i * 4 + 2] = tiled.pixels[i * 3 + 2];
    img.data[i * 4 + 3] = 255;
  }
  octx.putImageData(img, 0, 0);
  const blob = await out.convertToBlob({ type: 'image/jpeg', quality: 0.92 });
  return new Uint8Array(await blob.arrayBuffer());
}
