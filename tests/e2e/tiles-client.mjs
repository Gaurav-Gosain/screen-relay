// Minimal tiles client for tests: connects to /stream?view=1&codec=tiles,
// applies type 8 frames to an RGB buffer, ACKs each one, and can compare the
// buffer with /screenshot.png. Usage:
//   node tests/e2e/tiles-client.mjs http://127.0.0.1:8080 SECONDS [--keyframe-at S] [--ack-delay MS] [--check]
// Prints one JSON line with frames, bytes, latency and the check result.
import zlib from 'node:zlib';

const [base, secondsText, ...rest] = process.argv.slice(2);
const seconds = Number(secondsText || 5);
const check = rest.includes('--check');
const keyAt = rest.includes('--keyframe-at') ? Number(rest[rest.indexOf('--keyframe-at') + 1]) : -1;
// --ack-delay MS holds each ACK back, like a slow link or a slow terminal.
const ackDelay = rest.includes('--ack-delay') ? Number(rest[rest.indexOf('--ack-delay') + 1]) : 0;
const token = process.env.SCREEN_RELAY_TOKEN;

const u16 = (b, o) => (b[o] << 8) | b[o + 1];
const u32 = (b, o) => ((b[o] << 24) >>> 0) + (b[o + 1] << 16) + (b[o + 2] << 8) + b[o + 3];
const u64 = (b, o) => Number((BigInt(u32(b, o)) << 32n) + BigInt(u32(b, o + 4)));
const monoUs = () => Number(process.hrtime.bigint() / 1000n);

function header(type, gen, seq, payload = new Uint8Array(0)) {
  const m = new Uint8Array(20 + payload.length);
  m.set([78, 83, 76, 49, 1, type]);
  const v = new DataView(m.buffer);
  v.setUint32(8, gen); v.setUint32(12, seq); v.setUint32(16, payload.length);
  m.set(payload, 20);
  return m;
}

let rgb = null, W = 0, H = 0;
export function apply(p) {
  if (p[0] !== 1) throw new Error('version');
  const key = p[1] & 1, w = u16(p, 2), h = u16(p, 4), pts = u64(p, 6), count = u16(p, 14), blobLen = u32(p, 16);
  if (key) { W = w; H = h; rgb = new Uint8Array(w * h * 3); }
  else if (w !== W || h !== H) throw new Error('size without keyframe');
  const end = p.length - blobLen;
  const pixels = blobLen ? zlib.zstdDecompressSync(p.subarray(end)) : new Uint8Array(0);
  let q = 20, off = 0, copies = 0;
  const stride = W * 3;
  for (let i = 0; i < count; i++) {
    const kind = p[q++];
    let sx = 0, sy = 0;
    if (kind === 1) { sx = u16(p, q); sy = u16(p, q + 2); q += 4; }
    const x = u16(p, q), y = u16(p, q + 2), rw = u16(p, q + 4), rh = u16(p, q + 6); q += 8;
    if (!rw || !rh || x + rw > W || y + rh > H) throw new Error('bounds');
    if (kind === 1) {
      copies++;
      const order = sy >= y ? [...Array(rh).keys()] : [...Array(rh).keys()].reverse();
      for (const r of order) rgb.copyWithin((y + r) * stride + x * 3, (sy + r) * stride + sx * 3, (sy + r) * stride + (sx + rw) * 3);
    } else if (kind === 2) {
      for (let r = 0; r < rh; r++) { rgb.set(pixels.subarray(off, off + rw * 3), (y + r) * stride + x * 3); off += rw * 3; }
    } else if (kind === 3) {
      const c = [p[q], p[q + 1], p[q + 2]]; q += 3;
      for (let r = 0; r < rh; r++) for (let k = 0; k < rw; k++) rgb.set(c, (y + r) * stride + (x + k) * 3);
    } else throw new Error('op ' + kind);
  }
  if (q !== end || off !== pixels.length) throw new Error('length');
  return { key, pts, copies };
}

// PNG decoder for 8-bit RGB or RGBA, non-interlaced (what the engine writes).
function decodePng(buf) {
  let o = 8, w = 0, h = 0, ct = 0; const idat = [];
  while (o < buf.length) {
    const len = buf.readUInt32BE(o), type = buf.toString('ascii', o + 4, o + 8), data = buf.subarray(o + 8, o + 8 + len);
    if (type === 'IHDR') { w = data.readUInt32BE(0); h = data.readUInt32BE(4); ct = data[9]; if (data[8] !== 8 || data[12]) throw new Error('png format'); }
    if (type === 'IDAT') idat.push(data);
    o += 12 + len;
  }
  const bpp = ct === 6 ? 4 : 3, raw = zlib.inflateSync(Buffer.concat(idat)), out = new Uint8Array(w * h * 3), line = w * bpp;
  let prev = new Uint8Array(line);
  for (let y = 0; y < h; y++) {
    const f = raw[y * (line + 1)], cur = new Uint8Array(raw.subarray(y * (line + 1) + 1, (y + 1) * (line + 1)));
    for (let i = 0; i < line; i++) {
      const a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      if (f === 1) cur[i] += a; else if (f === 2) cur[i] += b; else if (f === 3) cur[i] += (a + b) >> 1;
      else if (f === 4) { const pp = a + b - c, pa = Math.abs(pp - a), pb = Math.abs(pp - b), pc = Math.abs(pp - c); cur[i] += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }
    }
    for (let x = 0; x < w; x++) out.set(cur.subarray(x * bpp, x * bpp + 3), (y * w + x) * 3);
    prev = cur;
  }
  return { w, h, rgb: out };
}

const stats = { frames: 0, keyframes: 0, bytes: 0, copies: 0, maxBytes: 0, lat: [], errors: [] };
const ws = new WebSocket(base.replace(/^http/, 'ws') + '/stream?view=1&codec=tiles', token ? ['nsl1', 'bearer.' + token] : ['nsl1']);
ws.binaryType = 'arraybuffer';
let gen = 0, keySent = false;
const started = Date.now();
ws.onmessage = (e) => {
  const m = new Uint8Array(e.data);
  const type = m[5]; gen = u32(m, 8);
  const seq = u32(m, 12);
  if (type !== 8) return;
  try {
    const t0 = monoUs();
    const r = apply(m.subarray(20));
    stats.frames++; stats.keyframes += r.key; stats.bytes += m.length; stats.copies += r.copies;
    stats.maxBytes = Math.max(stats.maxBytes, m.length);
    if (!r.key) stats.lat.push(t0 - r.pts);
  } catch (err) { stats.errors.push(String(err)); ws.send(header(9, gen, 0)); }
  const ack = () => ws.send(header(2, gen, seq, new Uint8Array(4)));
  if (ackDelay) setTimeout(ack, ackDelay); else ack();
  if (keyAt >= 0 && !keySent && Date.now() - started > keyAt * 1000) { keySent = true; ws.send(header(9, gen, 0)); }
};
ws.onerror = (e) => { stats.errors.push('socket: ' + (e.message || 'error')); };
await new Promise((resolve) => setTimeout(resolve, seconds * 1000));
let lossless = null;
if (check && rgb) {
  const headers = token ? { Authorization: 'Bearer ' + token } : {};
  const png = Buffer.from(await (await fetch(base + '/screenshot.png', { headers })).arrayBuffer());
  const shot = decodePng(png);
  let diff = 0;
  if (shot.w === W && shot.h === H) { for (let i = 0; i < rgb.length; i++) if (rgb[i] !== shot.rgb[i]) diff++; } else diff = -1;
  lossless = { differing_bytes: diff, width: W, height: H };
}
ws.close();
const lat = stats.lat.sort((a, b) => a - b);
const q = (f) => (lat.length ? Math.round(lat[Math.min(lat.length - 1, Math.floor(f * lat.length))] / 100) / 10 : null);
console.log(JSON.stringify({ seconds, frames: stats.frames, fps: +(stats.frames / seconds).toFixed(1), keyframes: stats.keyframes,
  kb: Math.round(stats.bytes / 1024), avg_bytes: Math.round(stats.bytes / Math.max(1, stats.frames)), max_bytes: stats.maxBytes,
  copies: stats.copies, latency_ms_p50: q(0.5), latency_ms_p99: q(0.99), errors: stats.errors.slice(0, 5), lossless }));
