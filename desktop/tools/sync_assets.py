"""Copies the local test sound library into the private assets repository
(github.com/tera-works/tera-synth-assets, Git LFS) so cloud sessions can use it.

Only finished instruments are copied (a folder counts once its .sfz exists, which
hsynth_sampler writes last), so a capture running at the same time is safe.

usage:  python desktop/tools/sync_assets.py [--assets DIR] [--push]
  default assets checkout: ../tera-synth-assets next to the Synth folder

Layout in the assets repository (mirrors desktop/data, so presets resolve unchanged):
  library/user/gm/...           GM test instruments (sampled from MuseScore General)
  library/user/soniccell/...    Roland SonicCell captures + catalog.csv
  presets/user/GM *.json, SC *.json
  soundfonts/MuseScore_General.sf2 (+ license)
  midi/*.mid                     the user's MIDI files (C:\WorkSpace\MIDI\原本)
Test material only - never distribute (Roland sound data, the user's MIDI files).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SYNTH = os.path.normpath(os.path.join(HERE, '..', '..'))
DATA = os.path.join(SYNTH, 'desktop', 'data')
MIDI = r'C:\WorkSpace\MIDI\原本'


def copy_if_changed(src, dst):
    if os.path.exists(dst):
        a, b = os.stat(src), os.stat(dst)
        if a.st_size == b.st_size and int(a.st_mtime) <= int(b.st_mtime):
            return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return True


def finished_instruments(root):
    """Folders below root that contain an .sfz (finished instruments)."""
    for dirpath, dirnames, filenames in os.walk(root):
        if any(f.lower().endswith('.sfz') for f in filenames):
            yield dirpath
            dirnames[:] = []  # an instrument folder is never nested


def sync(assets):
    copied = 0
    for lib in ('gm', 'soniccell'):
        src_root = os.path.join(DATA, 'library', 'user', lib)
        if not os.path.isdir(src_root):
            continue
        for inst in finished_instruments(src_root):
            rel = os.path.relpath(inst, DATA)
            for dirpath, _, filenames in os.walk(inst):
                for f in filenames:
                    s = os.path.join(dirpath, f)
                    copied += copy_if_changed(s, os.path.join(assets, os.path.relpath(s, DATA)))
        cat = os.path.join(src_root, 'catalog.csv')
        if os.path.exists(cat):
            copied += copy_if_changed(cat, os.path.join(assets, 'library', 'user', lib, 'catalog.csv'))
    # presets of finished instruments only (their sample path must exist in the assets)
    pdir = os.path.join(DATA, 'presets', 'user')
    for f in sorted(os.listdir(pdir)) if os.path.isdir(pdir) else []:
        if not (f.startswith('GM ') or f.startswith('SC ')) or not f.endswith('.json'):
            continue
        text = open(os.path.join(pdir, f), encoding='utf-8').read()
        try:
            a = json.loads(text).get('samples', {}).get('a', '')
        except ValueError:
            continue
        if a and os.path.exists(os.path.join(assets, 'library', a.replace('/', os.sep))):
            copied += copy_if_changed(os.path.join(pdir, f), os.path.join(assets, 'presets', 'user', f))
    if os.path.isdir(MIDI):
        for f in sorted(os.listdir(MIDI)):
            if f.lower().endswith(('.mid', '.midi')):
                copied += copy_if_changed(os.path.join(MIDI, f), os.path.join(assets, 'midi', f))
    sf = os.path.join(SYNTH, 'testdata', 'soundfonts')
    for f in ('MuseScore_General.sf2', 'MuseScore_General_License.md'):
        if os.path.exists(os.path.join(sf, f)):
            copied += copy_if_changed(os.path.join(sf, f), os.path.join(assets, 'soundfonts', f))
    return copied


def git(assets, *args):
    return subprocess.run(['git', '-C', assets] + list(args), check=True, capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--assets', default=os.path.join(os.path.dirname(SYNTH), 'tera-synth-assets'))
    ap.add_argument('--push', action='store_true', help='commit and push the changes')
    args = ap.parse_args()
    if not os.path.isdir(os.path.join(args.assets, '.git')):
        sys.exit('assets checkout not found: ' + args.assets)
    n = sync(args.assets)
    print('%d files copied or updated' % n)
    if args.push:
        git(args.assets, 'add', '-A')
        if git(args.assets, 'status', '--porcelain').strip():
            done = sum(1 for _ in finished_instruments(os.path.join(args.assets, 'library', 'user', 'soniccell')))
            git(args.assets, 'commit', '-q', '-m', 'Sync test sound library (%d SonicCell instruments)' % done)
            subprocess.run(['git', '-C', args.assets, 'push', '-u', 'origin', 'main'], check=True)
        else:
            print('nothing to commit')


if __name__ == '__main__':
    main()
