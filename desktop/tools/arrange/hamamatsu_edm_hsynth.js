// 浜松市歌 EDM の HSynth 版（元: midi_keyboard_kit_synth/tools/arrange/hamamatsu_edm.js、2026-11-10）
// 混声版のメロディ・合唱・和声から、4/4・130 BPM に組み直す。
// 12/8 の 1 拍（付点四分 = 8 分 3 つ）を 4/4 の 1 拍に。8 分の位置 0,1,2 → 拍の 0, 1/2, 3/4（ハネを 16 分のノリに）
// HSynth 版で足したもの:
// - 音色は SonicCell の音をバンクセレクト（CC0 / CC32 + プログラム）で指定（gm_bank.h）
// - サブベース、メタリックなうなりのベース（Drop 2）、ブラスの一撃、Drop 直前の 1 拍の空白
// - 最後のくり返しで半音上へ転調
// - 表情: CC1 の遅れて掛かるビブラート、CC5 ポルタメント、CC94 ディレイ投げ、CC10 の左右（docs/CONTROLLERS.md）
//   node hamamatsu_edm_hsynth.js [out.mid]
const { parse, BANK } = require('./smf_io.js');
const fs = require('fs');
const SRC = 'C:/WorkSpace/MIDI/原本/浜松市歌_混声.mid';
const OUT = process.argv[2] || 'C:/WorkSpace/MIDI/原本/浜松市歌_EDM_HSynth.mid';

const PPQ = 480, BAR = 1920, BPM = 130;
const src = parse(SRC);
// 元の小節
const sts = []; for (const e of src.tracks[0]) if (e.meta === 0x58) sts.push({ t: e.t, len: e.data[0] * src.div * 4 / (1 << e.data[1]) });
const sbars = []; { let t = 0, k = 0; while (t < 130000) { while (k + 1 < sts.length && sts[k + 1].t <= t) k++; sbars.push({ t, len: sts[k].len }); t += sts[k].len; } }
function srcNotes(ti) {
  const on = {}, out = [];
  for (const e of src.tracks[ti]) {
    if (e.st === undefined) continue; const ty = e.st & 0xF0;
    if (ty === 0x90 && e.d[1] > 0) on[e.d[0]] = e;
    else if (ty === 0x80 || ty === 0x90) { const s = on[e.d[0]]; if (s) { out.push({ s: s.t, e: e.t, n: e.d[0], v: s.d[1] }); delete on[e.d[0]]; } }
  }
  return out.sort((a, b) => a.s - b.s);
}
const TR = { S: 11, A: 12, T: 13, B: 14 };
const srcBarOf = t => { let i = 0; while (i + 1 < sbars.length && sbars[i + 1].t <= t) i++; return i; };
// 8 分の位置（0..12）→ 4/4 の tick（小節の頭から）
function mapPos(eighths) {
  const b = Math.floor(eighths / 3 + 1e-9), r = eighths - 3 * b;
  const f = r <= 1 ? r * 0.5 : r <= 2 ? 0.5 + (r - 1) * 0.25 : 0.75 + (r - 2) * 0.25;
  return Math.round((b + f) * PPQ);
}

// ---------------- 書き出す側 ----------------
const tracks = {}; // ch → {name, ev:[]}
function T(ch) { return tracks[ch]; }
function ev(ch, t, pri, bytes) { T(ch).ev.push({ t: Math.max(0, Math.round(t)), pri, bytes }); }
// ノートは集めておき、書き出す前に同じ鍵の重なり（合唱のユニゾン、ロールとスネアなど）を片付ける
function note(ch, t, len, n, v) {
  if (n < 0 || n > 127 || len < 10) return;
  (T(ch).notes ||= []).push({ s: Math.round(t), e: Math.round(t + len), n, v: Math.max(1, Math.min(127, Math.round(v))) });
}
function flushNotes(ch) {
  const byKey = {};
  let list = T(ch).notes || [];
  // Drop の直前 1 拍は、メロディ（リード・合唱）と効果音だけ残して全部止める
  if (GAP_CH.includes(ch)) {
    for (const d of [S.drop1, S.drop2]) {
      list = list.filter(x => !(x.s >= d - PPQ && x.s < d));
      for (const x of list) if (x.s < d - PPQ && x.e > d - PPQ) x.e = d - PPQ - 10;
      list = list.filter(x => x.e - x.s >= 20);
    }
  }
  // 最後のくり返しから半音上へ（ドラムを除く）
  if (ch !== DR) for (const x of list) if (x.s >= KEY_UP) x.n += 1;
  for (const x of list) (byKey[x.n] ||= []).push(x);
  for (const k in byKey) {
    const a = byKey[k].sort((p, q) => p.s - q.s || q.e - p.e);
    const keep = [];
    for (const x of a) {
      const l = keep[keep.length - 1];
      if (l && x.s < l.e) {
        if (x.s - l.s < 30) { l.e = Math.max(l.e, x.e); l.v = Math.max(l.v, x.v); continue; } // 同時 → 1 つに
        l.e = x.s - 10;                                                                    // 途中から → 前を切る
        if (l.e - l.s < 20) keep.pop();
      }
      keep.push(x);
    }
    for (const x of keep) { ev(ch, x.s, 5, [0x90 | ch, x.n, x.v]); ev(ch, x.e, 4, [0x80 | ch, x.n, 0]); }
  }
}
function cc(ch, t, c, v) { ev(ch, t, 2, [0xB0 | ch, c, Math.max(0, Math.min(127, Math.round(v)))]); }
function bend(ch, t, v) { v = Math.max(0, Math.min(16383, Math.round(v + 8192))); ev(ch, t, 3, [0xE0 | ch, v & 127, v >> 7]); }
function ramp(ch, c, t0, t1, v0, v1, step = 60) { for (let t = t0; t <= t1; t += step) cc(ch, t, c, v0 + (v1 - v0) * (t - t0) / Math.max(1, t1 - t0)); }
// snd = [[MSB, LSB], SonicCell の番号 1..128]（バンクセレクト）または GM のプログラム番号 0..127
function setup(ch, name, snd, vol, rev, cho, dly, extra = []) {
  tracks[ch] = { name, ev: [] };
  const nb = Buffer.from(name, 'utf8'); ev(ch, 0, 0, [0xFF, 0x03, nb.length, ...nb]);
  if (Array.isArray(snd)) {
    ev(ch, 0, 1, [0xB0 | ch, 0, snd[0][0]]); ev(ch, 0, 1, [0xB0 | ch, 32, snd[0][1]]);
    ev(ch, 0, 1.5, [0xC0 | ch, snd[1] - 1]);
  } else ev(ch, 0, 1.5, [0xC0 | ch, snd]);
  cc(ch, 0, 7, vol); cc(ch, 0, 11, 127); cc(ch, 0, 91, rev); cc(ch, 0, 93, cho); cc(ch, 0, 94, dly);
  for (const [c, v] of extra) cc(ch, 0, c, v);
}

// 元の小節を並べて置く: list = 元の小節番号の並び。from = 最初の小節の何 8 分目から、to = 最後の小節の何 8 分目まで
function place(trackIdx, list, dst0, opt) {
  const { ch, tr = 0, vel = null, velScale = 1, from = 0, to = 99, legato = false, out = null } = opt;
  const notes = srcNotes(trackIdx);
  let dst = dst0; const placed = [];
  list.forEach((b, i) => {
    const bs = sbars[b].t, be = bs + sbars[b].len, beats = sbars[b].len / 720;
    const nextConsec = i + 1 < list.length && list[i + 1] === b + 1;
    for (const x of notes) {
      if (x.s < bs || x.s >= be) continue;
      const pe = (x.s - bs) / 240;
      if (i === 0 && pe < from - 1e-6) continue;
      if (i === list.length - 1 && pe >= to - 1e-6) continue;
      const s = dst + mapPos(pe);
      let e;
      if (x.e <= be) e = dst + mapPos((x.e - bs) / 240);
      else if (nextConsec) { const nb = sbars[b + 1]; e = dst + beats * PPQ + mapPos(Math.min(nb.len, x.e - nb.t) / 240); }
      else e = dst + beats * PPQ;
      placed.push({ s, e, n: x.n + tr, v: vel ?? x.v * velScale });
    }
    dst += beats * PPQ;
  });
  placed.sort((a, b) => a.s - b.s);
  // 同じ鍵の重なりを切る。legato なら次の音まで伸ばす（少しだけすき間）
  for (let i = 0; i < placed.length; i++) {
    const p = placed[i];
    const nxt = placed.slice(i + 1).find(q => q.s > p.s);
    if (legato && nxt && nxt.s - p.e < 120) p.e = Math.max(p.e, nxt.s - 20);
    const same = placed.slice(i + 1).find(q => q.n === p.n && q.s >= p.s);
    if (same && p.e > same.s - 10) p.e = same.s - 10;
  }
  for (const p of placed) (out || ((s, l, n, v) => note(ch, s, l, n, v)))(p.s, p.e - p.s, p.n, p.v);
  return dst;
}

// ---------------- 和音 ----------------
const PC = { C: 0, D: 2, E: 4, F: 5, G: 7, A: 9, B: 11 };
const QUAL = { '': [0, 4, 7], m: [0, 3, 7], '7': [0, 4, 7, 10], m7: [0, 3, 7, 10], maj7: [0, 4, 7, 11], sus4: [0, 5, 7], '6': [0, 4, 7, 9], add9: [0, 2, 4, 7] };
function pcOf(s) { return (PC[s[0]] + (s[1] === '#' ? 1 : s[1] === 'b' ? -1 : 0) + 12) % 12; }
function chordOf(name) {
  const [main, sl] = name.split('/');
  const m = main.match(/^([A-G][#b]?)(.*)$/);
  const root = pcOf(m[1]);
  return { name, root, pcs: QUAL[m[2]].map(i => (root + i) % 12), bass: sl ? pcOf(sl) : root };
}
// 元の小節ごとの和音（1 拍ずつ）。メロディと合唱の和声から整理したもの
const CH = {
  5: 'D D D D', 6: 'F#m7 F#m7 F#7 F#7', 7: 'Bm Bm/A G G', 8: 'Asus4 Asus4 A A', 9: 'D/F# G Em Em', 10: 'A7/C# A6 D D',
  11: 'Gmaj7 Gmaj7 Em Em7', 12: 'Asus4 Asus4 A Asus4', 13: 'D D D D', 14: 'F#m7 F#m7 F#7 F#7', 15: 'Bm Bm/A G G', 16: 'Asus4 Asus4 A A6',
  17: 'F#m7 G Em Em', 18: 'A7/C# A6 D D', 19: 'Gmaj7 Gmaj7 G6 A', 20: 'D D D D',
  21: 'Bm Bm F#m F#m', 22: 'G G F#m F#m', 23: 'G6 Em E7 E7', 24: 'Asus4 Asus4 A D', 25: 'Bm Bm F#m F#m', 26: 'G G F#m F#m',
  27: 'G6 Gmaj7 Asus4 A', 28: 'D D D D',
  30: 'D D D D', 31: 'F#m7 F#m7 F#7 F#7', 32: 'Bm Bm/A G G', 33: 'Asus4 Asus4 A A', 34: 'D/F# G Em Em', 35: 'A7/C# A6 D D',
  36: 'Gmaj7 Gmaj7 Em A7', 37: 'Dadd9/F# D/F# F#m7 F#m7', 38: 'B7 B7 Em Em7', 39: 'A7/C# A6 D D', 40: 'Gmaj7 Gmaj7 G6 A',
  41: 'D D', 42: 'G G G G', 43: 'D D G D', 44: 'D D D D',
};
const chart = []; // {t, len, c}
function chords(list, dst0) { // list: 元の小節番号、または '和音 和音 …' の文字列
  let t = dst0;
  for (const b of list) {
    const names = (typeof b === 'string' ? b : CH[b]).split(' ');
    for (const n of names) { chart.push({ t, len: PPQ, c: chordOf(n) }); t += PPQ; }
  }
  return t;
}
function chartIn(t0, t1) { return chart.filter(x => x.t >= t0 && x.t < t1); }
function merged(t0, t1) { // 同じ和音が続くところをまとめる
  const out = [];
  for (const x of chartIn(t0, t1)) { const l = out[out.length - 1]; if (l && l.c.name === x.c.name && l.t + l.len === x.t) l.len += x.len; else out.push({ ...x }); }
  return out;
}
let prevVoicing = [62, 66, 69];
function voicing(c, lo = 54, hi = 72, n = 3) {
  let pcs = c.pcs;
  if (pcs.length > n) pcs = [pcs[0], pcs[1], pcs[3]]; // 7 の和音は 5 度を抜く
  const cands = pcs.map(pc => { const a = []; for (let x = lo; x <= hi; x++) if (x % 12 === pc) a.push(x); return a; });
  let best = null;
  const rec = (i, cur) => {
    if (i === cands.length) {
      const s = [...cur].sort((a, b) => a - b);
      if (s[s.length - 1] - s[0] > 14) return;
      const cost = s.reduce((a, x, k) => a + Math.abs(x - (prevVoicing[k] ?? x)), 0);
      if (!best || cost < best.cost) best = { s, cost };
      return;
    }
    for (const x of cands[i]) if (!cur.includes(x)) rec(i + 1, [...cur, x]);
  };
  rec(0, []);
  prevVoicing = best.s;
  return best.s;
}
const bassNote = pc => 31 + ((pc - 7 + 12) % 12); // G1..F#2

// ---------------- 段の位置 ----------------
const S = {};
S.intro = 0; S.verse = 8 * BAR; S.build = 24 * BAR; S.drop1 = 32 * BAR; S.brk = 48 * BAR; S.drop2 = 56 * BAR;
const D2_LIST = [30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 37, 38, 39, 40];
S.half = S.drop2 + D2_LIST.length * BAR;          // 2/4 の小節（元の 41）
S.coda = S.half + 960;                            // 元の 42〜44
S.last = S.coda + 2 * BAR;                        // 元の 44（最後の和音）
S.end = S.coda + 3 * BAR;                         // 最後の和音を伸ばして消す 1 小節
S.fin = S.end + BAR;
const KEY_UP = S.drop2 + 11 * BAR;                // Drop 2 の 2 回目のくり返し（合唱が重なるところ）から半音上

// ---------------- パート ----------------
const LEAD = 0, LEAD2 = 1, PLUCK = 2, PAD = 4, STAB = 5, BASS = 6, CHOIR = 7, DR = 9, RISER = 10, HIT = 11;
const SUB = 12, GROWL = 13, BRASS = 14;
const GAP_CH = [DR, BASS, SUB, GROWL, PAD, PLUCK, STAB, BRASS];
// 音色（SonicCell）。名前は HSynth のプリセット名「SC PRESET D 018 Super Saw」など
setup(LEAD, 'Lead (Super Saw)', [BANK.D, 18], 104, 45, 20, 30, [[74, 64], [10, 64]]);
setup(LEAD2, 'Lead 2 (Epic Lead, oct)', [BANK.D, 80], 84, 40, 30, 20, [[10, 46]]);
setup(PLUCK, 'Pluck / Arp (Seq Saw)', [BANK.E, 28], 90, 50, 10, 55, [[10, 84]]);
setup(PAD, 'Pad (Nu Epic Pad)', [BANK.G, 46], 74, 80, 40, 0, [[74, 40]]);
setup(STAB, 'Stabs (Power Synth)', [BANK.E, 43], 80, 40, 20, 25, [[10, 40]]);
setup(BASS, 'Bass (Storm Bass)', [BANK.C, 31], 104, 5, 0, 0);
setup(CHOIR, 'Choir (Angels Choir)', [BANK.G, 62], 94, 95, 30, 0);
setup(DR, 'Drums (House Kit)', [BANK.RHYTHM, 19], 114, 20, 0, 0);
setup(RISER, 'Riser', 122, 70, 60, 0, 0);
setup(HIT, 'Impact (Mix Hit 2)', [BANK.C, 84], 96, 70, 0, 0);
setup(SUB, 'Sub Bass (Lo Bass)', [BANK.C, 8], 100, 0, 0, 0);
setup(GROWL, 'Growl Bass (Metalic Bass)', [BANK.F, 38], 70, 10, 0, 10, [[10, 70]]);
setup(BRASS, 'Brass Hits (Stacked Brs)', [BANK.D, 28], 86, 55, 10, 20, [[10, 86]]);
// ライザーはベンド幅 12 半音
cc(RISER, 0, 101, 0); cc(RISER, 0, 100, 0); cc(RISER, 0, 6, 12); cc(RISER, 0, 38, 0); cc(RISER, 0, 101, 127); cc(RISER, 0, 100, 127);

// 和音の進行
chords([21, 22, 23, 24, 25, 26, 27, 'Asus4 Asus4 A Asus4'], S.intro);
chords([5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20], S.verse);
chords([21, 22, 23, 24, 25, 26, 27, 'D D A Asus4'], S.build);
chords([5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20], S.drop1);
chords([21, 22, 23, 24, 25, 26, 27, 'D D A Asus4'], S.brk);
chords(D2_LIST, S.drop2); chords([41], S.half); chords([42, 43, 44, 'D D D D'], S.coda);

// ---- メロディ ----
const A_LIST = [4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20];
// Verse: プラック（1 オクターブ上）。弱起 = 元の 4 小節目の 9 8 分目から
place(TR.S, A_LIST, S.verse - BAR, { ch: PLUCK, tr: 12, from: 9, velScale: 1.25 });
// Build: のこぎり波のリード（こもった音から開く）
place(TR.S, [21, 22, 23, 24, 25, 26, 27, 28], S.build, { ch: LEAD, tr: 12, vel: 100, legato: true });
ramp(LEAD, 74, S.build, S.drop1 - PPQ, 30, 64, 120);
// Drop 1: リード（弱起を Build の最後の拍に。元の 20 小節目の弱起は Breakdown の合唱が歌う）
place(TR.S, A_LIST, S.drop1 - BAR, { ch: LEAD, tr: 12, from: 9, to: 9, vel: 112, legato: true });
// Breakdown: 合唱 4 声（元の 20 小節目の弱起から）
for (const v of ['S', 'A', 'T', 'B']) place(TR[v], [20, 21, 22, 23, 24, 25, 26, 27, 28], S.brk - BAR, { ch: CHOIR, from: 9, velScale: 1.0 });
ramp(LEAD, 74, S.brk, S.brk + 10, 64, 64);
// Drop 2: 2 本のリード（1 オクターブ違い）。弱起 = 元の 29 小節目の 9 8 分目から
const D2_FULL = [29, ...D2_LIST, 41, 42, 43, 44];
place(TR.S, D2_FULL, S.drop2 - BAR, { ch: LEAD, tr: 12, from: 9, vel: 115, legato: true });
place(TR.S, D2_FULL, S.drop2 - BAR, { ch: LEAD2, tr: 0, from: 9, vel: 100, legato: true });
// Drop 2 の後半（2 回目のくり返し）からは合唱も重ねる
for (const v of ['S', 'A', 'T', 'B']) place(TR[v], [37, 38, 39, 40, 41, 42, 43, 44], S.drop2 + 11 * BAR, { ch: CHOIR, velScale: 0.95 });

// 2/4 の小節（元の 41）と最後の部分の長さのつじつま: place は元の小節の長さで進むので、41 は 2 拍 = 960 で合っている

// ---- パッド（和音を伸ばす）----
function pad(t0, t1, vel, opt = {}) {
  for (const x of merged(t0, t1)) {
    const v = voicing(x.c, 54, 72, opt.n || 3);
    for (const n of v) note(PAD, x.t, x.len - 15, n, vel);
  }
}
pad(S.intro, S.verse, 70); pad(S.verse, S.build, 72); pad(S.build, S.drop1, 78);
pad(S.drop1, S.brk, 80); pad(S.brk, S.drop2, 82); pad(S.drop2, S.drop2 + 11 * BAR, 84); // 後半は合唱が和音を持つのでパッドは休む
// 最後の和音（伸ばして消す）
for (const n of voicing(chordOf('D'), 54, 72)) note(PAD, S.end, BAR - 30, n, 90);
note(LEAD, S.end, BAR - 30, 93, 100); note(LEAD2, S.end, BAR - 30, 81, 90);
for (const n of [50, 62, 66, 69]) note(CHOIR, S.end, BAR - 30, n, 95);
// パッドの明るさ: Intro で開く、Build で開ききる、Breakdown は一度こもらせて開く
ramp(PAD, 74, S.intro, S.verse, 30, 60, 120);
ramp(PAD, 74, S.build, S.drop1 - PPQ, 60, 100, 120); cc(PAD, S.drop1, 74, 72);
cc(PAD, S.brk, 74, 40); ramp(PAD, 74, S.brk + 4 * BAR, S.drop2 - PPQ, 40, 100, 120); cc(PAD, S.drop2, 74, 76);

// ---- サイドチェイン風のうねり（キックの拍で CC11 を落として戻す）----
function pump(ch, t0, t1, depth = 40) {
  for (let t = t0; t < t1; t += PPQ) {
    cc(ch, t, 11, depth); cc(ch, t + 60, 11, depth + (127 - depth) * 0.45); cc(ch, t + 120, 11, depth + (127 - depth) * 0.75);
    cc(ch, t + 200, 11, depth + (127 - depth) * 0.92); cc(ch, t + 280, 11, 127);
  }
  cc(ch, t1, 11, 127);
}
pump(PAD, S.verse + 8 * BAR, S.build, 70);
pump(PAD, S.drop1, S.brk, 35); pump(PAD, S.drop2, S.drop2 + 11 * BAR, 35);
pump(CHOIR, S.drop2 + 11 * BAR, S.last, 60);

// ---- アルペジオ（プラック）----
function arp(t0, t1, vel, opt = {}) {
  const pat = [0, 1, 2, 3, 2, 1, 2, 3];
  let k = 0;
  for (const x of chartIn(t0, t1)) {
    const base = voicing({ ...x.c }, 62, 79, 3); prevVoicing = prevVoicing; // 声部の近さを使い回す
    const tones = [...base, base[0] + 12];
    for (let i = 0; i < 4; i++) {
      const t = x.t + i * 120;
      if (opt.skipLastBeat && t >= t1 - PPQ) continue;
      note(PLUCK, t, 100, tones[pat[k++ % pat.length]], vel * (i === 0 ? 1 : i === 2 ? 0.9 : 0.78));
    }
  }
}
const savedV = prevVoicing;
arp(S.intro, S.verse, 88, { skipLastBeat: true });
ramp(PLUCK, 74, S.intro, S.verse - PPQ, 25, 64, 120); cc(PLUCK, S.verse, 74, 64);
arp(S.build, S.drop1, 80, { skipLastBeat: true });
arp(S.drop1, S.brk - PPQ, 72);
arp(S.drop2, S.last, 76);
pump(PLUCK, S.drop1, S.brk, 50); pump(PLUCK, S.drop2, S.last, 50);
prevVoicing = savedV;

// ---- スタブ（裏拍の和音）----
function stabs(t0, t1, vel) {
  let pv = [67, 71, 74];
  for (const x of chartIn(t0, t1)) {
    const keep = prevVoicing; prevVoicing = pv; const v = voicing(x.c, 62, 79, 3); pv = prevVoicing; prevVoicing = keep;
    for (const n of v) note(STAB, x.t + 240, 170, n, vel);
  }
}
stabs(S.drop1, S.brk - PPQ, 92); stabs(S.drop2, S.last, 96);
for (const n of [62, 66, 69, 74]) note(STAB, S.last, 700, n, 115); // 最後の和音の一撃

// ---- ベース ----
function bassSustain(t0, t1, vel) { for (const x of merged(t0, t1)) note(BASS, x.t, x.len - 20, bassNote(x.c.bass), vel); }
function bassOff8(t0, t1, vel) { for (const x of chartIn(t0, t1)) note(BASS, x.t + 240, 200, bassNote(x.c.bass), vel); }
function bassRoll(t0, t1, vel) { // 16 分の 2〜4 つ目（頭はキック）
  for (const x of chartIn(t0, t1)) {
    const n = bassNote(x.c.bass);
    note(BASS, x.t + 120, 100, n, vel * 0.85); note(BASS, x.t + 240, 100, n, vel); note(BASS, x.t + 360, 100, n + (x.t % BAR === 3 * PPQ ? 12 : 0), vel * 0.9);
  }
}
function bass8(t0, t1, v0, v1) { for (let t = t0; t < t1; t += 240) { const x = chart.find(c => c.t <= t && t < c.t + c.len); if (x) note(BASS, t, 200, bassNote(x.c.bass), v0 + (v1 - v0) * (t - t0) / (t1 - t0)); } }
function bass16(t0, t1, v0, v1) { for (let t = t0; t < t1; t += 120) { const x = chart.find(c => c.t <= t && t < c.t + c.len); if (x) note(BASS, t, 100, bassNote(x.c.bass), v0 + (v1 - v0) * (t - t0) / (t1 - t0)); } }
bassSustain(S.intro + 4 * BAR, S.verse, 80);
bassSustain(S.verse, S.verse + 8 * BAR, 90); bassOff8(S.verse + 8 * BAR, S.build, 100);
bass8(S.build, S.build + 6 * BAR, 80, 105); bass16(S.build + 6 * BAR, S.drop1 - PPQ, 95, 115);
bassRoll(S.drop1, S.brk - PPQ, 112);
bassSustain(S.brk, S.brk + 4 * BAR, 80); bass8(S.brk + 4 * BAR, S.brk + 7 * BAR, 85, 105); bass16(S.brk + 7 * BAR, S.drop2 - PPQ, 100, 115);
bassRoll(S.drop2, S.last, 115);
note(BASS, S.last, 2 * BAR - 30, bassNote(2), 118);

// ---- ドラム ----
const KICK = 36, CLAP = 39, SNARE = 38, CHH = 42, OHH = 46, CRASH = 49, RIDE = 51, TOM_H = 50, TOM_M = 47, TOM_L = 43;
const hit = (n, t, v, len = 60) => note(DR, t, len, n, v);
function beats(t0, t1, fn) { for (let t = t0, i = 0; t < t1; t += PPQ, i++) fn(t, i); }
function groove(t0, t1, o = {}) {
  beats(t0, t1, (t, i) => {
    if (o.kick !== false && (!o.halfKick || i % 2 === 0)) hit(KICK, t, o.kickVel || 118);
    if (o.clap !== false && i % 2 === 1) { hit(CLAP, t, o.clapVel || 105); if (o.snare) hit(SNARE, t, 80); }
    if (o.ohh) hit(OHH, t + 240, 80, 120);
    if (o.hats) for (let k = 0; k < 4; k++) if (!(o.ohh && k === 2)) hit(CHH, t + k * 120, [45, 38, 62, 40][k] * (o.hats || 1));
    if (o.ride) hit(RIDE, t, 60);
  });
}
function roll(t0, t1, step, v0, v1, n = SNARE) { for (let t = t0; t < t1; t += step) hit(n, t, v0 + (v1 - v0) * (t - t0) / (t1 - t0), Math.min(60, step - 5)); }
function fill(t) { roll(t, t + PPQ, 120, 80, 115); } // 最後の拍のフィル
// Intro
groove(S.intro + 2 * BAR, S.intro + 4 * BAR, { kick: false, clap: false, hats: 0.8 });
groove(S.intro + 4 * BAR, S.verse - PPQ, { hats: 1, ohh: true, clap: false });
fill(S.verse - PPQ);
// Verse
hit(CRASH, S.verse, 100, 200);
groove(S.verse, S.verse + 8 * BAR, { halfKick: true, hats: 0.9, clapVel: 90 });
groove(S.verse + 8 * BAR, S.build - PPQ, { hats: 1, ohh: true, clapVel: 100 });
fill(S.build - PPQ);
// Build: キックは 6 小節、スネアは 4 分 → 8 分 → 16 分 → 32 分
hit(CRASH, S.build, 105, 200);
groove(S.build, S.build + 6 * BAR, { clap: false, hats: 0.9 });
roll(S.build, S.build + 2 * BAR, PPQ, 60, 75);
roll(S.build + 2 * BAR, S.build + 4 * BAR, 240, 70, 88);
roll(S.build + 4 * BAR, S.build + 6 * BAR, 120, 85, 100);
roll(S.build + 6 * BAR, S.build + 7 * BAR, 120, 100, 110);
roll(S.build + 7 * BAR, S.drop1 - PPQ, 60, 105, 125);
// Drop 1
function drop(t0, t1, fills) {
  hit(CRASH, t0, 120, 300); note(HIT, t0, 400, 40, 120);
  groove(t0, t1, { hats: 1.25, ohh: true, snare: true, kickVel: 127, clapVel: 118 });
  for (const f of fills) fill(f);
}
drop(S.drop1, S.brk - PPQ, [S.drop1 + 8 * BAR - PPQ]);
hit(CRASH, S.drop1 + 8 * BAR, 105, 200);
// Breakdown: 前半は打楽器なし。後半で Build
hit(CRASH, S.brk, 95, 300);
groove(S.brk + 4 * BAR, S.brk + 6 * BAR, { clap: false, hats: 0.7 });
roll(S.brk + 4 * BAR, S.brk + 6 * BAR, 240, 65, 90);
roll(S.brk + 6 * BAR, S.brk + 7 * BAR, 120, 90, 108);
roll(S.brk + 7 * BAR, S.drop2 - PPQ, 60, 105, 125);
// Drop 2（後半のくり返しの前にフィルとクラッシュ）
drop(S.drop2, S.half, [S.drop2 + 7 * BAR - PPQ, S.drop2 + 11 * BAR - PPQ]);
hit(CRASH, S.drop2 + 7 * BAR, 105, 200); hit(CRASH, S.drop2 + 11 * BAR, 120, 300); note(HIT, S.drop2 + 11 * BAR, 400, 40, 110);
groove(S.drop2, S.half, { ride: true, clap: false, kick: false });
// 2/4 の小節: タムで下りる
[TOM_H, TOM_H, TOM_M, TOM_M, TOM_L, TOM_L, SNARE, SNARE].forEach((n, i) => hit(n, S.half + i * 120, 95 + i * 3));
// 最後の部分
hit(CRASH, S.coda, 120, 300); note(HIT, S.coda, 400, 40, 115);
groove(S.coda, S.last - PPQ, { hats: 1, ohh: true, snare: true });
roll(S.last - PPQ, S.last, 60, 95, 125);
hit(KICK, S.last, 127); hit(CRASH, S.last, 127, 400); hit(CLAP, S.last, 110); note(HIT, S.last, 600, 36, 127);
hit(CRASH, S.end, 90, 400);

// ---- サブベース: Drop では和音の根音を伸ばし、キックの拍で CC11 を落とす（うねり）----
function subSustain(t0, t1, vel) { for (const x of merged(t0, t1)) note(SUB, x.t, x.len - 20, bassNote(x.c.bass), vel); }
// （Build / Breakdown には入れない: サブが入る瞬間が Drop）
subSustain(S.drop1, S.brk, 112);
subSustain(S.drop2, S.last, 116);
note(SUB, S.last, 2 * BAR - 30, bassNote(2), 120);
pump(SUB, S.drop1, S.brk, 45); pump(SUB, S.drop2, S.last, 45);

// ---- うなりのベース（Drop 2 と最後）: 1 オクターブ上で 16 分の裏を刻む ----
function growl(t0, t1, vel) {
  for (const x of chartIn(t0, t1)) {
    const n = bassNote(x.c.bass) + 12;
    note(GROWL, x.t + 120, 90, n, vel * 0.8); note(GROWL, x.t + 240, 110, n, vel); note(GROWL, x.t + 360, 90, n + (x.t % (2 * PPQ) ? 7 : 0), vel * 0.85);
  }
}
growl(S.drop2, S.last, 104);
ramp(GROWL, 74, S.drop2, S.drop2 + 4 * BAR, 34, 64, 120);   // 開いていく
cc(GROWL, S.drop2 + 11 * BAR, 74, 64);

// ---- ブラスの一撃: Drop の 2 小節ごと（最後のくり返しは毎小節）に、拍頭と 2 拍目の裏 ----
function brass(t0, t1, every) {
  let pv = [62, 67, 71, 74];
  for (let t = t0; t < t1; t += every * BAR) {
    const x = chart.find(c => c.t <= t && t < c.t + c.len);
    if (!x) continue;
    const keep = prevVoicing; prevVoicing = pv; const v = voicing(x.c, 58, 79, 4); pv = prevVoicing; prevVoicing = keep;
    for (const n of v) { note(BRASS, t, 330, n, 112); note(BRASS, t + 720, 200, n, 104); }
  }
}
brass(S.drop1, S.brk - BAR, 2); brass(S.drop2, S.drop2 + 11 * BAR, 2); brass(S.drop2 + 11 * BAR, S.last, 1);
for (const n of [62, 66, 69, 74, 78]) note(BRASS, S.last, 900, n, 124);
cc(BRASS, S.drop2 + 11 * BAR, 91, 75);

// ---- 表情（HSynth の標準コントローラー）----
// リード: 長い音に遅れて掛かるビブラート（CC1）、フレーズの終わりでディレイを投げる（CC94）
function leadExpression(ch, depth) {
  const ns = (T(ch).notes || []).slice().sort((a, b) => a.s - b.s);
  cc(ch, 0, 1, 0);
  ns.forEach((x, k) => {
    const len = x.e - x.s;
    if (len >= PPQ) { ramp(ch, 1, x.s + len * 0.35, x.s + len * 0.9, 0, depth, 40); cc(ch, x.e, 1, 0); }
    const nx = ns[k + 1];
    if (len >= 1.5 * PPQ && (!nx || nx.s - x.e >= PPQ / 2)) { cc(ch, x.s + len * 0.5, 94, 75); cc(ch, nx ? nx.s - 5 : x.e + BAR, 94, 30); }
  });
}
leadExpression(LEAD, 80); leadExpression(LEAD2, 60);
// Drop ではリードが前の音から少し滑る（CC5 ポルタメント）。Breakdown では戻す
for (const ch of [LEAD, LEAD2]) { cc(ch, S.drop1, 5, 14); cc(ch, S.brk, 5, 0); cc(ch, S.drop2, 5, 14); }
// Drop 直前の 1 拍: 合唱とリードに残響を足して、空白を響かせる
for (const d of [S.drop1, S.drop2]) for (const ch of [LEAD, CHOIR]) { ramp(ch, 91, d - PPQ, d - 30, 50, 115, 60); cc(ch, d, 91, ch === CHOIR ? 95 : 45); }
// 最後のくり返し（転調後）はリードを少し明るく
cc(LEAD, KEY_UP, 74, 72);
// Drop が跳ね上がるように、Build と Breakdown は音量（CC11）を抑えてから上げていく
ramp(LEAD, 11, S.build, S.drop1 - PPQ, 72, 118, 120); cc(LEAD, S.drop1, 11, 127);
ramp(PAD, 11, S.build, S.drop1 - PPQ, 80, 120, 120);
ramp(PLUCK, 11, S.build, S.drop1 - 2 * PPQ, 80, 115, 120);
cc(CHOIR, S.brk - BAR, 11, 84); ramp(CHOIR, 11, S.brk + 4 * BAR, S.drop2 - PPQ, 84, 122, 120); cc(CHOIR, S.drop2, 11, 127);
cc(PAD, S.brk, 11, 82); ramp(PAD, 11, S.brk + 4 * BAR, S.drop2 - PPQ, 82, 120, 120);
ramp(BASS, 11, S.build, S.drop1 - PPQ, 64, 112, 120); cc(BASS, S.drop1, 11, 127);
ramp(BASS, 11, S.brk + 4 * BAR, S.drop2 - PPQ, 64, 112, 120); cc(BASS, S.drop2, 11, 127);

// ---- ライザー（雑音が上がっていく）----
function riser(t0, t1) {
  note(RISER, t0, t1 - t0 - 10, 60, 100);
  for (let t = t0; t <= t1; t += 60) bend(RISER, t, 8191 * ((t - t0) / (t1 - t0)) ** 1.6);
  ramp(RISER, 11, t0, t1, 15, 127, 60); ramp(RISER, 74, t0, t1, 20, 120, 60);
  bend(RISER, t1 + 5, 0); cc(RISER, t1 + 5, 11, 127);
}
riser(S.intro + 6 * BAR, S.verse - 20);
riser(S.build + 4 * BAR, S.drop1 - PPQ);
riser(S.brk + 4 * BAR, S.drop2 - PPQ);

// ---- 最後の部分の音量（伸ばした和音を消していく）----
for (const ch of [PAD, CHOIR, LEAD, LEAD2, BASS, SUB, BRASS]) ramp(ch, 11, S.end, S.fin - 60, 127, 0, 60);

// ---------------- 指揮者トラック ----------------
const cond = [];
const meta = (t, ty, data) => cond.push({ t, pri: 0, bytes: [0xFF, ty, data.length, ...data] });
const tempo = (t, bpm) => { const u = Math.round(60e6 / bpm); meta(t, 0x51, [u >> 16, (u >> 8) & 255, u & 255]); };
cond.push({ t: 0, pri: 0, bytes: [0xFF, 0x03, ...(() => { const b = Buffer.from('浜松市歌 EDM (HSynth)', 'utf8'); return [b.length, ...b]; })()] });
meta(0, 0x58, [4, 2, 24, 8]); tempo(0, BPM);
meta(S.half, 0x58, [2, 2, 24, 8]); meta(S.coda, 0x58, [4, 2, 24, 8]);
// 最後の和音の手前 2 拍と伸ばしにリタルダンド
tempo(S.last - 2 * PPQ, 120); tempo(S.last - PPQ, 110); tempo(S.last, 104); tempo(S.end, 96);
cond.push({ t: 0, pri: -1, bytes: [0xF0, 5, 0x7E, 0x7F, 0x09, 0x01, 0xF7] }); // GM オン

// ---------------- SMF に書く ----------------
function vlq(n) { const a = [n & 127]; while ((n >>= 7)) a.unshift((n & 127) | 128); return a; }
function trackChunk(evs, endT) {
  evs.sort((a, b) => a.t - b.t || a.pri - b.pri);
  const out = []; let last = 0;
  for (const e of evs) { out.push(...vlq(e.t - last), ...e.bytes); last = e.t; }
  out.push(...vlq(Math.max(0, endT - last)), 0xFF, 0x2F, 0);
  const h = Buffer.alloc(8); h.write('MTrk'); h.writeUInt32BE(out.length, 4);
  return Buffer.concat([h, Buffer.from(out)]);
}
for (const ch in tracks) flushNotes(+ch);
const endT = S.fin;
const chunks = [trackChunk(cond, endT)];
for (const ch of Object.keys(tracks).map(Number).sort((a, b) => a - b)) chunks.push(trackChunk(tracks[ch].ev, endT));
const hd = Buffer.alloc(14); hd.write('MThd'); hd.writeUInt32BE(6, 4); hd.writeUInt16BE(1, 8); hd.writeUInt16BE(chunks.length, 10); hd.writeUInt16BE(PPQ, 12);
fs.writeFileSync(OUT, Buffer.concat([hd, ...chunks]));
let total = 0; for (const ch in tracks) total += tracks[ch].ev.filter(e => (e.bytes[0] & 0xF0) === 0x90).length;
const sec = (S.last / PPQ) * 60 / BPM + 2 * BAR / PPQ * 60 / 100;
console.log(`→ ${OUT}\n  ${(S.fin / BAR).toFixed(1)} 小節, 約 ${Math.floor(sec / 60)}:${String(Math.round(sec % 60)).padStart(2, '0')}, ノート ${total}`);
console.log('  段:', Object.entries(S).map(([k, v]) => `${k}=${(v / BAR + 1).toFixed(2)}`).join(' '));
