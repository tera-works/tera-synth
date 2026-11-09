"""Builds the recommended setup for the sampled SonicCell library: one sound per MIDI
channel (multi-timbral), for a keyboard that sends CC1 / CC5 / CC11 / CC71 / CC74 /
CC91 / CC94. The engine itself writes the file (hsynth_remote_sim over SysEx), so it
is exactly what the apps export.

usage:
  python desktop/tools/make_soniccell_setup.py [--out FILE] [--copy DIR ...]
  default out: desktop/data/setups/SonicCell Keyboard.json
  --copy C:/HSynth-iPhone/setups   also copies it there (iPhone / Android folders)

Needs the SonicCell presets in desktop/data/presets/user (from hsynth_sampler) and
engine/build-desktop/hsynth_remote_sim.exe. The result refers to the test library:
for testing only, never distribute.

The controllers need no assignment: the engine gives every preset (also the sampled
ones without a filter) the standard responses -
  CC1 vibrato, CC5 portamento time, CC11 expression (level), CC71 resonance,
  CC74 brightness (cutoff; 64 = the preset as sampled), CC91 reverb send, CC94 delay send.
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

# channel -> (preset, part volume 0..1, base reverb send, base delay send, what it is for)
CHANNELS = [
    ('SC PRESET A 001 Rich Grand', 0.80, 0.12, 0.00, 'grand piano'),
    ('SC PRESET A 044 Tine EP', 0.80, 0.10, 0.05, 'electric piano (CC1 vibrato, CC94 delay)'),
    ('SC PRESET B 003 FullDraw Org', 0.70, 0.08, 0.00, 'organ'),
    ('SC PRESET C 047 String Ens', 0.75, 0.18, 0.00, 'strings (CC11 swells)'),
    ('SC PRESET D 004 Stereo Brass', 0.75, 0.12, 0.00, 'brass section'),
    ('SC PRESET D 077 Classic Lead', 0.70, 0.08, 0.15, 'synth lead (CC5 portamento, CC74 / CC71 filter)'),
    ('SC PRESET G 005 Silk Pad', 0.70, 0.20, 0.05, 'pad'),
    ('SC PRESET B 075 FingerMaster', 0.85, 0.00, 0.00, 'finger bass'),
    ('SC PRESET B 049 Clean Gtr', 0.75, 0.10, 0.08, 'clean guitar'),
    ('SC PRESET Rhythm 005 StandardKit1', 0.85, 0.06, 0.00, 'drums (GM channel 10)'),
    ('SC PRESET G 059 Choir Aahs 1', 0.70, 0.20, 0.00, 'choir'),
    ('SC PRESET A 113 SC Vibe', 0.75, 0.12, 0.05, 'vibraphone'),
    ('SC PRESET D 036 Tenor Sax', 0.75, 0.10, 0.00, 'tenor sax (CC11 expression)'),
    ('SC PRESET B 101 SH-101 Bs 1', 0.80, 0.00, 0.00, 'synth bass (CC74 / CC71 filter)'),
    ('SC PRESET D 018 Super Saw', 0.70, 0.10, 0.10, 'supersaw (CC74 sweeps)'),
    ('SC PRESET C 104 SC Flute', 0.75, 0.15, 0.00, 'flute'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default=os.path.join(DATA, 'setups', 'SonicCell Keyboard.json'))
    ap.add_argument('--copy', action='append', default=[])
    a = ap.parse_args()
    presets = os.path.join(DATA, 'presets', 'user')
    missing = [n for n, *_ in CHANNELS if not os.path.isfile(os.path.join(presets, n + '.json'))]
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
    for k, (name, vol, rev, dly, _) in enumerate(CHANNELS):
        r = req({'op': 'load_preset', 'name': name, 'part': k}, timeout=120)
        for w in r.get('warnings', []):
            print(f'  ch{k + 1}: {w}')
        req({'op': 'mix_set', 'part': k, 'values': {'channel': k + 1, 'volume': vol}})
        req({'op': 'set', 'part': k, 'values': {'send.a': rev, 'send.b': dly}})
        print(f'ch{k + 1:2d}  {name}')
    req({'op': 'select_part', 'part': 0})
    setup = req({'op': 'setup_get', 'name': 'SonicCell Keyboard'}, timeout=120)['setup']
    setup['comment'] = ('One SonicCell sound per MIDI channel. ' +
                        '; '.join(f'ch{k + 1} {n.replace("SC PRESET ", "")} = {what}'
                                  for k, (n, _, _, _, what) in enumerate(CHANNELS)) +
                        '. Controllers on every channel: CC1 vibrato, CC5 portamento time, CC11 expression, '
                        'CC71 resonance, CC74 brightness, CC91 reverb, CC94 delay.')
    for k, (name, *_) in enumerate(CHANNELS):  # the names the apps show per part
        setup['state']['parts'][k]['preset']['name'] = name
    setup['host'] = {'voices': 128, 'quality': 1}
    setup.pop('midiLearn', None)  # keep the device's own MIDI learn assignments
    text = hr.json.dumps(setup, ensure_ascii=False, indent=2)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
    print('written:', a.out)
    for d in a.copy:
        os.makedirs(d, exist_ok=True)
        shutil.copy(a.out, d)
        print('copied to', d)


if __name__ == '__main__':
    main()
