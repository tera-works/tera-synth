// Minimal Standard MIDI File reader / writer for the HSynth song arrangements (Node, no packages).
// parse(file) -> {div, tracks: [[{t, meta, data} | {t, sysex} | {t, st, d}]]}
// write(file, div, tracks) with tracks = [[{t, pri, bytes}]] (pri orders events at the same tick)
const fs = require('fs');

function readVlq(b, p) { let v = 0, c; do { c = b[p.i++]; v = (v << 7) | (c & 127); } while (c & 128); return v; }

exports.parse = function (file) {
  const b = fs.readFileSync(file);
  const p = { i: 14 };
  const div = b.readUInt16BE(12), ntr = b.readUInt16BE(10), tracks = [];
  for (let t = 0; t < ntr; t++) {
    if (b.toString('latin1', p.i, p.i + 4) !== 'MTrk') break;
    const len = b.readUInt32BE(p.i + 4);
    p.i += 8;
    const end = p.i + len, ev = [];
    let tick = 0, rs = 0;
    while (p.i < end) {
      tick += readVlq(b, p);
      let s = b[p.i];
      if (s < 0x80) s = rs; else p.i++;
      if (s === 0xFF) { const ty = b[p.i++], l = readVlq(b, p); ev.push({ t: tick, meta: ty, data: b.slice(p.i, p.i + l) }); p.i += l; }
      else if (s === 0xF0 || s === 0xF7) { const l = readVlq(b, p); ev.push({ t: tick, sysex: Buffer.concat([Buffer.from([s]), b.slice(p.i, p.i + l)]) }); p.i += l; }
      else { rs = s; const n = (s & 0xE0) === 0xC0 ? 1 : 2; ev.push({ t: tick, st: s, d: [...b.slice(p.i, p.i + n)] }); p.i += n; }
    }
    p.i = end;
    tracks.push(ev);
  }
  return { div, tracks };
};

function vlq(n) { const a = [n & 127]; while ((n >>= 7)) a.unshift((n & 127) | 128); return a; }
exports.vlq = vlq;

// parsed event -> raw bytes (end-of-track metas are dropped: write() adds its own)
exports.bytesOf = function (e) {
  if (e.meta !== undefined) return e.meta === 0x2F ? null : [0xFF, e.meta, ...vlq(e.data.length), ...e.data];
  if (e.sysex) return [e.sysex[0], ...vlq(e.sysex.length - 1), ...e.sysex.slice(1)];
  return [e.st, ...e.d];
};

exports.write = function (file, div, tracks) {
  let endT = 0;
  for (const tr of tracks) for (const e of tr) endT = Math.max(endT, e.t);
  const chunks = tracks.map(evs => {
    evs.sort((a, b) => a.t - b.t || a.pri - b.pri);
    const out = [];
    let last = 0;
    for (const e of evs) { out.push(...vlq(e.t - last), ...e.bytes); last = e.t; }
    out.push(...vlq(Math.max(0, endT - last)), 0xFF, 0x2F, 0);
    const h = Buffer.alloc(8);
    h.write('MTrk');
    h.writeUInt32BE(out.length, 4);
    return Buffer.concat([h, Buffer.from(out)]);
  });
  const hd = Buffer.alloc(14);
  hd.write('MThd');
  hd.writeUInt32BE(6, 4);
  hd.writeUInt16BE(1, 8);
  hd.writeUInt16BE(chunks.length, 10);
  hd.writeUInt16BE(div, 12);
  fs.writeFileSync(file, Buffer.concat([hd, ...chunks]));
};

// SonicCell banks (MSB, LSB) as HSynth's song player resolves them (engine/include/hsynth/gm_bank.h)
exports.BANK = {
  CAPITAL: [121, 0], VAR1: [121, 1], A: [87, 64], B: [87, 65], C: [87, 66], D: [87, 67], E: [87, 68], F: [87, 69],
  G: [87, 70], RHYTHM: [86, 64], GM2_RHYTHM: [120, 0],
};
