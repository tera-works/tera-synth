// 浜松市歌 Rock → HSynth edition: the same arrangement, made heavier for HSynth's song player.
//   node hamamatsu_rock_hsynth.js [in.mid] [out.mid]
// - SonicCell sounds by bank select (CC0 / CC32 + program, as on the SonicCell; see gm_bank.h)
// - a double-tracked rhythm guitar (hard left / right), a hard-rock organ and orchestra hits in the choruses
// - the last chorus goes up a semitone (where the original already speeds up to 126 BPM)
// - expression through HSynth's standard controllers (docs/CONTROLLERS.md): CC1 delayed vibrato on long
//   guitar notes, CC74 darker verses and opening builds, CC94 delay throws, CC10 stereo, harder drums
// Test material: the MIDI files and the SonicCell captures are not distributed (CLAUDE.md).
const { parse, write, bytesOf, BANK } = require('./smf_io.js');

const IN = process.argv[2] || 'C:/WorkSpace/MIDI/原本/浜松市歌_Rock.mid';
const OUT = process.argv[3] || 'C:/WorkSpace/MIDI/原本/浜松市歌_Rock_HSynth.mid';
const src = parse(IN);
const DIV = src.div;

// ---------------- bars (3/8 pickup, 12/8, a 6/8 bar near the end) ----------------
const sigs = [];
for (const e of src.tracks[0]) if (e.meta === 0x58) sigs.push({ t: e.t, len: e.data[0] * DIV * 4 / (1 << e.data[1]) });
const bars = [];
{ let t = 0, k = 0; while (t < 260000) { while (k + 1 < sigs.length && sigs[k + 1].t <= t) k++; bars.push({ t, len: sigs[k].len }); t += sigs[k].len; } }
const BAR = n => bars[n].t;
const barOf = t => { let i = 0; while (i + 1 < bars.length && bars[i + 1].t <= t) i++; return i; };
const EIGHTH = DIV / 2;

// sections (bar numbers of the original; see the bar map in the session notes)
const CHORUS = [[29, 36], [49, 64], [70, 81]];          // full band + strings / choir
const VERSE = [[5, 12], [21, 28], [37, 48]];           // band, guitars a little darker
const BUILD = [36, 48, 56, 68];                         // one bar that opens into the next section
const HITS = [5, 21, 29, 37, 41, 49, 57, 61, 63, 69, 70, 78, 83, 87];
const KEY_UP = BAR(70);                                 // last chorus: +1 semitone to the end
const inRange = (b, rs) => rs.some(([a, z]) => b >= a && b <= z);

// ---------------- channels (0-based) ----------------
const PIANO = 0, LEAD = 1, RHY = 2, BASS = 3, HARM = 4, STR = 5, CLEAN = 6, CHOIR = 7, TIMP = 8, DR = 9;
const RHY2 = 10, HIT = 11, ORGAN = 12;
// [bank, program number as printed on the SonicCell (1..128), pan, name]
const SOUND = {
  [PIANO]: [BANK.A, 1, 58, 'Rich Grand'],
  [LEAD]: [BANK.B, 2, 64, 'SuperDistLd2'],
  [RHY]: [BANK.B, 64, 18, 'SC PowerChd'],
  [BASS]: [BANK.B, 97, 64, 'Low Bass'],            // the loudest heavy bass of the audition (Kickin' Bass was 10 dB down)
  [HARM]: [BANK.B, 58, 84, 'LP Dist'],
  [STR]: [BANK.C, 78, 70, 'Orch & Horns'],
  [CLEAN]: [BANK.B, 54, 44, 'Strat Gtr'],
  [CHOIR]: [BANK.G, 62, 64, 'Angels Choir'],
  [TIMP]: [BANK.CAPITAL, 48, 64, 'Timpani'],
  [DR]: [BANK.RHYTHM, 9, 64, 'Rock Kit 2'],         // louder and roomier than Rock Kit 1
  [RHY2]: [BANK.B, 61, 110, 'SC Loud Gtr'],
  [HIT]: [BANK.C, 90, 64, 'Mix Hit 1'],
  [ORGAN]: [BANK.A, 127, 76, 'HardRockORG1'],
};

// ---------------- read the original into notes + other events per track ----------------
const tracks = src.tracks.map(tr => {
  const notes = [], other = [], on = {};
  let ch = -1;
  for (const e of tr) {
    if (e.st !== undefined) {
      ch = e.st & 15;
      const ty = e.st & 0xF0;
      if (ty === 0x90 && e.d[1] > 0) { (on[e.d[0]] ||= []).push(e); continue; }
      if (ty === 0x80 || ty === 0x90) {
        const s = (on[e.d[0]] || []).shift();
        if (s) notes.push({ s: s.t, e: e.t, n: e.d[0], v: s.d[1], ch: s.st & 15 });
        continue;
      }
      if (ty === 0xC0 || (ty === 0xB0 && (e.d[0] === 0 || e.d[0] === 32 || e.d[0] === 10))) continue;  // replaced below
    }
    const bytes = bytesOf(e);
    if (bytes) other.push({ t: e.t, pri: e.meta !== undefined ? 0 : 3, bytes });
  }
  return { ch, notes, other };
});

// ---------------- output tracks ----------------
const out = [];  // [{name, ev}]
const evOf = [];
function newTrack(name) { const ev = []; out.push(ev); const nb = Buffer.from(name, 'utf8'); ev.push({ t: 0, pri: 0, bytes: [0xFF, 0x03, nb.length, ...nb] }); return ev; }
function cc(ev, ch, t, c, v) { ev.push({ t: Math.max(0, Math.round(t)), pri: 2, bytes: [0xB0 | ch, c, Math.max(0, Math.min(127, Math.round(v)))] }); }
function sound(ev, ch) {
  const [[msb, lsb], num, pan] = SOUND[ch];
  ev.push({ t: 0, pri: 1, bytes: [0xB0 | ch, 0, msb] }, { t: 0, pri: 1, bytes: [0xB0 | ch, 32, lsb] }, { t: 0, pri: 1.5, bytes: [0xC0 | ch, num - 1] });
  cc(ev, ch, 1, 10, pan);
}
function noteOut(ev, x) {
  const n = x.ch !== DR && x.s >= KEY_UP ? x.n + 1 : x.n;
  ev.push({ t: x.s, pri: 5, bytes: [0x90 | x.ch, n, Math.max(1, Math.min(127, Math.round(x.v)))] });
  ev.push({ t: Math.max(x.s + 1, x.e), pri: 4, bytes: [0x80 | x.ch, n, 0] });
}
function ramp(ev, ch, c, t0, t1, v0, v1, step = 60) { for (let t = t0; t <= t1; t += step) cc(ev, ch, t, c, v0 + (v1 - v0) * (t - t0) / Math.max(1, t1 - t0)); }

const byCh = {};
tracks.forEach((tr, i) => {
  const ev = i === 0 ? (out.push([]), out[0]) : [];
  if (i > 0) out.push(ev);
  for (const o of tr.other) ev.push(o);
  if (i === 0) {
    // the name of the song
    const nb = Buffer.from('浜松市歌 Rock (HSynth)', 'utf8');
    for (let k = ev.length - 1; k >= 0; k--) if (ev[k].bytes[0] === 0xFF && ev[k].bytes[1] === 0x03) ev.splice(k, 1);
    ev.push({ t: 0, pri: -1, bytes: [0xFF, 0x03, nb.length, ...nb] });
    return;
  }
  if (tr.ch >= 0 && SOUND[tr.ch] && !byCh[tr.ch]) sound(ev, tr.ch);  // one bank / program per channel
  (byCh[tr.ch] ||= []).push(...tr.notes);
  evOf[i] = ev;
});
const trackOfCh = ch => evOf[tracks.findIndex((tr, i) => i > 0 && tr.ch === ch)];

// ---------------- drums: harder in the choruses, accents on the backbeat ----------------
for (const x of byCh[DR]) {
  const b = barOf(x.s);
  const kick = x.n === 35 || x.n === 36, snare = x.n === 38 || x.n === 40;
  if (inRange(b, CHORUS) && (kick || snare)) x.v = Math.min(127, x.v * 1.15 + 6);
  else if (inRange(b, VERSE) && (kick || snare)) x.v = Math.min(127, x.v * 1.06 + 3);
}

// ---------------- rhythm guitar: double-tracked (the copy on the right, a hair late) ----------------
let seed = 7;
const rnd = () => ((seed = (seed * 1103515245 + 12345) & 0x7fffffff) / 0x7fffffff);
const rhy2 = byCh[RHY].map(x => ({ ...x, ch: RHY2, s: x.s + 6 + Math.round(rnd() * 8), e: x.e + 6, v: x.v * (0.9 + rnd() * 0.08) }));

// ---------------- organ: the rhythm guitar's chord held per half bar in the choruses ----------------
const organ = [];
for (const [a, z] of CHORUS) {
  for (let b = a; b <= z; b++) {
    for (let h = 0; h < 2; h++) {
      const t0 = BAR(b) + h * bars[b].len / 2, t1 = t0 + bars[b].len / 2;
      const ns = byCh[RHY].filter(x => x.s < t1 && x.e > t0);
      if (!ns.length) continue;
      const low = Math.min(...ns.map(x => x.n));
      const pcs = [...new Set(ns.map(x => (x.n - low + 120) % 12))].sort((p, q) => p - q);
      let root = low;
      while (root < 48) root += 12;
      while (root > 59) root -= 12;
      const keys = new Set([root, root + 12]);
      for (const pc of pcs) if (pc) keys.add(root + pc), keys.add(root + pc + 12);
      for (const n of [...keys].filter(k => k <= 79).slice(0, 5)) organ.push({ s: t0 + 5, e: t1 - 15, n, v: 84, ch: ORGAN });
    }
  }
}

// ---------------- orchestra hits on the big downbeats (root, octave, fifth of the bass) ----------------
const hits = [];
for (const b of HITS) {
  const t = BAR(b);
  const bs = byCh[BASS].filter(x => x.s <= t + 30 && x.e > t).sort((p, q) => p.n - q.n)[0] ||
             byCh[BASS].filter(x => x.s >= t).sort((p, q) => p.s - q.s)[0];
  if (!bs) continue;
  let r = bs.n;
  while (r < 48) r += 12;
  for (const n of [r, r + 7, r + 12]) hits.push({ s: t, e: t + 3 * EIGHTH, n, v: b >= 69 ? 120 : 110, ch: HIT });
}

// ---------------- write the notes and the expression ----------------
for (const ch of Object.keys(byCh).map(Number)) {
  const ev = trackOfCh(ch);
  for (const x of byCh[ch]) noteOut(ev, x);
}
const evR2 = newTrack('Rhythm Guitar 2 (double, right)'); sound(evR2, RHY2); for (const x of rhy2) noteOut(evR2, x);
for (const tr of tracks) for (const o of tr.other)  // the double follows the rhythm guitar's swells (CC11)
  if (tr.ch === RHY && o.bytes[0] === (0xB0 | RHY) && o.bytes[1] === 11) evR2.push({ ...o, t: o.t + 6, bytes: [0xB0 | RHY2, 11, o.bytes[2]] });
const evHit = newTrack('Orchestra Hits'); sound(evHit, HIT); for (const x of hits) noteOut(evHit, x);
const evOrg = newTrack('Hard Rock Organ'); sound(evOrg, ORGAN); for (const x of organ) noteOut(evOrg, x);
cc(evOrg, ORGAN, 2, 7, 82); cc(evR2, RHY2, 2, 7, 96); cc(evHit, HIT, 2, 7, 100); cc(evHit, HIT, 2, 91, 70);

// brightness: verses a little darker (CC74 < 64 = HSynth's low-pass on sampled sounds), choruses open,
// and every build bar sweeps open into the next section
const GTR = [[RHY, trackOfCh(RHY)], [RHY2, evR2], [HARM, trackOfCh(HARM)], [CLEAN, trackOfCh(CLEAN)]];
for (const [ch, ev] of GTR) {
  cc(ev, ch, 0, 74, 64);
  for (const [a] of VERSE) cc(ev, ch, BAR(a), 74, 50);
  for (const [a] of CHORUS) cc(ev, ch, BAR(a), 74, 64);
  for (const b of BUILD) ramp(ev, ch, 74, BAR(b), BAR(b + 1) - 30, 34, 64);
  for (const b of BUILD) if (!inRange(b + 1, CHORUS)) cc(ev, ch, BAR(b + 1), 74, 50);
}
// organ swells into each chorus
for (const [a] of CHORUS) ramp(evOrg, ORGAN, 11, BAR(a), BAR(a) + 2 * EIGHTH * 3, 70, 127);

// lead and harmony guitars: delayed vibrato on held notes, delay throws at the ends of phrases
for (const ch of [LEAD, HARM]) {
  const ev = trackOfCh(ch);
  const ns = byCh[ch].slice().sort((p, q) => p.s - q.s);
  cc(ev, ch, 0, 1, 0);
  cc(ev, ch, 0, 94, 0);
  ns.forEach((x, k) => {
    const len = x.e - x.s;
    if (len >= 3 * EIGHTH) {
      const a = x.s + len * 0.3, z = x.s + len * 0.85;
      ramp(ev, ch, 1, a, z, 0, ch === LEAD ? 78 : 60, 40);
      cc(ev, ch, x.e, 1, 0);
    }
    const next = ns[k + 1];
    if (ch === LEAD && len >= 5 * EIGHTH && (!next || next.s - x.e >= 2 * EIGHTH)) {
      cc(ev, ch, x.s + len * 0.5, 94, 70);
      cc(ev, ch, next ? next.s - 5 : x.e + 4 * DIV, 94, 0);
    }
  });
}
// choir and strings: CC11 already shaped in the original; more air in the last chorus
cc(trackOfCh(CHOIR), CHOIR, KEY_UP, 91, 90);
cc(trackOfCh(STR), STR, KEY_UP, 91, 80);

write(OUT, DIV, out);
const count = out.reduce((a, ev) => a + ev.filter(e => (e.bytes[0] & 0xF0) === 0x90).length, 0);
console.log(`→ ${OUT}\n  ${out.length} tracks, ${count} notes (organ ${organ.length}, hits ${hits.length}, double ${rhy2.length}); key up from bar 70 (tick ${KEY_UP})`);
