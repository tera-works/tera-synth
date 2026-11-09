"""Builds the recommended setup for the sampled SonicCell library: one sound per MIDI
channel (multi-timbral), for a keyboard that sends CC1 / CC5 / CC11 / CC71 / CC74 /
CC91 / CC94. Channels 1-10 hold the essentials (the keyboard sends on 1-10 for now),
11-16 more sounds. The engine itself writes the file (hsynth_remote_sim over SysEx),
so it is exactly what the apps export.

usage:
  python desktop/tools/make_soniccell_setup.py [--out FILE] [--copy DIR ...]
  default out: desktop/data/setups/SonicCell Keyboard.json
  --copy C:/HSynth-iPhone/setups   also copies it there (iPhone / Android folders)

Needs the SonicCell presets in desktop/data/presets/user (from hsynth_sampler) and
engine/build-desktop/hsynth_remote_sim.exe. The result refers to the test library:
for testing only, never distribute.

Every channel has the standard responses (docs/CONTROLLERS.md): CC1 vibrato, CC5
portamento time, CC11 expression, CC71 resonance, CC74 brightness, CC91 reverb, CC94
delay. Where another job suits the sound better (an electric piano's wheel = tremolo,
not vibrato), the setup maps that CC to a part effect on that channel only (MIDI learn
with channel + part); a mapped CC replaces the standard response on that part.
"""
import argparse
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hsynth_remote as hr  # noqa: E402

SYNTH = os.path.normpath(os.path.join(HERE, '..', '..'))
DATA = os.path.join(SYNTH, 'desktop', 'data')
SIM = os.path.join(SYNTH, 'engine', 'build-desktop', 'hsynth_remote_sim.exe')

# One entry per channel:
#   preset, part volume, reverb send, delay send, description,
#   part settings (effects), CC mappings {cc: (parameter, value at 0, value at 127, what it does)}
CHANNELS = [
    # ---- 1-10: the essentials
    ('SC PRESET A 001 Rich Grand', 0.80, 0.12, 0.00, 'grand piano',
     {'ins.slot1': 'chorus', 'ins.chorus.mode': 'dimension', 'ins.chorus.mix': 0.0},
     {1: ('ins.chorus.mix', 0.0, 0.6, 'chorus (wider piano)')}),
    ('SC PRESET A 044 Tine EP', 0.80, 0.10, 0.05, 'electric piano',
     {'ins.slot1': 'tremolo', 'ins.tremolo.mode': 'autopan', 'ins.tremolo.rate': 4.5, 'ins.tremolo.depth': 0.0},
     {1: ('ins.tremolo.depth', 0.0, 0.9, 'stereo tremolo (autopan) depth')}),
    ('SC PRESET B 003 FullDraw Org', 0.70, 0.08, 0.00, 'organ',
     {'ins.slot1': 'drive', 'ins.drive.type': 'tube', 'ins.drive.amount': 0.6, 'ins.drive.mix': 0.0,
      'ins.slot2': 'chorus', 'ins.chorus.mode': 'classic', 'ins.chorus.rate': 0.8, 'ins.chorus.depth': 0.6,
      'ins.chorus.mix': 0.5},
     {1: ('ins.chorus.rate', 0.8, 6.5, 'rotary-like speed: slow -> fast'),
      71: ('ins.drive.mix', 0.0, 0.9, 'overdrive')}),
    ('SC PRESET C 047 String Ens', 0.75, 0.18, 0.00, 'strings', {}, {}),
    ('SC PRESET D 004 Stereo Brass', 0.75, 0.12, 0.00, 'brass section', {}, {}),
    ('SC PRESET G 005 Silk Pad', 0.70, 0.20, 0.05, 'pad',
     {'ins.slot1': 'phaser', 'ins.phaser.rate': 0.25, 'ins.phaser.mix': 0.0},
     {1: ('ins.phaser.mix', 0.0, 0.7, 'slow phaser sweep')}),
    ('SC PRESET D 077 Classic Lead', 0.70, 0.08, 0.15, 'synth lead', {}, {}),
    ('SC PRESET B 075 FingerMaster', 0.85, 0.00, 0.00, 'finger bass',
     {'ins.slot1': 'drive', 'ins.drive.type': 'tube', 'ins.drive.amount': 0.55, 'ins.drive.mix': 0.0},
     {1: ('ins.drive.mix', 0.0, 0.8, 'grit (drive)')}),
    ('SC PRESET D 018 Super Saw', 0.70, 0.10, 0.10, 'supersaw / poly synth', {}, {}),
    ('SC PRESET Rhythm 005 StandardKit1', 0.85, 0.06, 0.00, 'drums (GM channel 10)',
     {'ins.slot1': 'filter', 'ins.filter.type': 'lp24', 'ins.filter.cutoff': 20000, 'ins.filter.reso': 0.0},
     {1: ('ins.filter.cutoff', 20000, 250, 'DJ low-pass filter (up = darker)'),
      5: ('ins.filter.reso', 0.0, 0.8, 'filter resonance (no pitch glide on drums)')}),
    # ---- 11-16: more sounds
    ('SC PRESET B 049 Clean Gtr', 0.75, 0.10, 0.08, 'clean guitar',
     {'ins.slot1': 'wah', 'ins.wah.mode': 'auto', 'ins.wah.mix': 0.0},
     {1: ('ins.wah.mix', 0.0, 1.0, 'auto-wah')}),
    ('SC PRESET G 059 Choir Aahs 1', 0.70, 0.20, 0.00, 'choir', {}, {}),
    ('SC PRESET A 113 SC Vibe', 0.75, 0.12, 0.05, 'vibraphone',
     {'ins.slot1': 'tremolo', 'ins.tremolo.mode': 'tremolo', 'ins.tremolo.rate': 5.0, 'ins.tremolo.depth': 0.0},
     {1: ('ins.tremolo.depth', 0.0, 0.7, 'vibraphone motor (tremolo)')}),
    ('SC PRESET D 036 Tenor Sax', 0.75, 0.10, 0.00, 'tenor sax', {}, {}),
    ('SC PRESET B 101 SH-101 Bs 1', 0.80, 0.00, 0.00, 'synth bass', {}, {}),
    ('SC PRESET A 073 Cell Clav', 0.75, 0.08, 0.00, 'clavinet',
     {'ins.slot1': 'wah', 'ins.wah.mode': 'auto', 'ins.wah.mix': 0.0},
     {1: ('ins.wah.mix', 0.0, 1.0, 'auto-wah (funky clav)')}),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(DATA, 'setups', 'SonicCell Keyboard.json'))
    ap.add_argument('--copy', action='append', default=[])
    a = ap.parse_args()
    presets = os.path.join(DATA, 'presets', 'user')
    missing = [c[0] for c in CHANNELS if not os.path.isfile(os.path.join(presets, c[0] + '.json'))]
    if missing:
        raise SystemExit('missing presets: ' + ', '.join(missing))
    port = hr.Sim(SIM, ['--lib', os.path.join(DATA, 'library'), '--presets', presets])
    c = hr.Client(port)

    def req(obj, timeout=60):
        r = c.request(obj, timeout=timeout)
        if not r or not r.get('ok'):
            raise SystemExit(f'{obj.get("op")}: {r}')
        return r

    req({'op': 'mode', 'multi': True})
    mappings = []
    for k, (name, vol, rev, dly, what, settings, ccs) in enumerate(CHANNELS):
        r = req({'op': 'load_preset', 'name': name, 'part': k}, timeout=120)
        for w in r.get('warnings', []):
            print(f'  ch{k + 1}: {w}')
        req({'op': 'mix_set', 'part': k, 'values': {'channel': k + 1, 'volume': vol}})
        req({'op': 'set', 'part': k, 'values': {'send.a': rev, 'send.b': dly, **settings}})
        for cc, (param, lo, hi, _) in ccs.items():
            mappings.append({'param': param, 'cc': cc, 'channel': k + 1, 'part': k + 1, 'min': lo, 'max': hi})
        print(f'ch{k + 1:2d}  {name}' + ''.join(f'  CC{cc}={p}' for cc, (p, *_) in ccs.items()))
    req({'op': 'select_part', 'part': 0})
    setup = req({'op': 'setup_get', 'name': 'SonicCell Keyboard'}, timeout=120)['setup']
    for k, ch in enumerate(CHANNELS):  # the names the apps show per part
        setup['state']['parts'][k]['preset']['name'] = ch[0]

    def describe(k, ch):
        extra = ', '.join(f'CC{cc} {d}' for cc, (_, _, _, d) in ch[6].items())
        return f'ch{k + 1} {ch[0].replace("SC PRESET ", "")} = {ch[4]}' + (f' ({extra})' if extra else '')

    setup['comment'] = ('One SonicCell sound per MIDI channel (1-10 essentials, 11-16 more). ' +
                        '; '.join(describe(k, ch) for k, ch in enumerate(CHANNELS)) +
                        '. Every other controller has its standard response: CC1 vibrato, CC5 portamento time, '
                        'CC11 expression, CC71 resonance, CC74 brightness, CC91 reverb, CC94 delay.')
    setup['midiLearn'] = {'format': 'hsynth-midimap', 'version': 1, 'mappings': mappings}
    setup['host'] = {'voices': 128, 'quality': 1}
    text = hr.json.dumps(setup, ensure_ascii=False, indent=2)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('written:', a.out)
    # check: the file loads into a fresh engine with the effects and mappings
    port2 = hr.Sim(SIM, ['--lib', os.path.join(DATA, 'library'), '--presets', presets])
    c2 = hr.Client(port2)
    r = c2.request({'op': 'setup_set', 'setup': setup}, timeout=180)
    if not r or not r.get('ok'):
        raise SystemExit(f'the setup does not load: {r}')
    back = c2.request({'op': 'learn', 'cmd': 'get'}, timeout=30) or {}
    maps = back.get('mappings', {})
    n = len(maps.get('mappings', []) if isinstance(maps, dict) else maps) if back.get('ok') else -1
    if n != len(mappings):
        raise SystemExit(f'MIDI learn mappings: {n} loaded, {len(mappings)} written')
    print(f'loads back: ok, {n} MIDI learn mappings' if n >= 0 else 'loads back: ok')
    for d in a.copy:
        os.makedirs(d, exist_ok=True)
        shutil.copy(a.out, d)
        print('copied to', d)


if __name__ == '__main__':
    main()
