"""Mirrors the source the iOS build needs into src/ of the PUBLIC AltStore repository
(tera-works/tera-synth-ios, which also holds source.json and the releases) and tags it,
so the IPA is built there (GitHub Actions minutes are free on public repositories;
on this private repository macOS minutes count 10x).

usage:
  python ios-distribution/scripts/mirror_build.py v0.8.0 [--dry-run]

What is copied: only the files listed in PATHS, taken from the commit the tag points
to in this repository (git archive, so nothing uncommitted and nothing ignored), into
src/ - the engine, the iOS app, the CoreMIDI / audio host files, miniaudio, the factory
presets and factory sounds bundled in the app, distribute.py - plus the workflow
(public_build/ios.yml -> .github/workflows/). Never: sampled libraries, MIDI files,
docs, tools, Android code, history. The AltStore files at the top (source.json,
index.html, README.md) are left alone. Each release is one commit by the noreply identity.

Then: wait for the "iOS distribution" run there and publish with publish_public.py.
The token comes from `git credential fill` and is never printed.
"""
import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE))
import distribute  # noqa: E402

BUILD = 'tera-works/tera-synth-ios'
IDENTITY = ('tera-works', '240604790+tera-works@users.noreply.github.com')
PATHS = [
    'engine/include', 'engine/src',
    'desktop/src/audio_out.h', 'desktop/src/audio_out.cpp',
    'desktop/src/midi_in.h', 'desktop/src/midi_in_coremidi.cpp',
    'desktop/src/midi_out.h', 'desktop/src/midi_out_coremidi.cpp',
    'desktop/src/remote_link.h', 'desktop/src/remote_link.cpp',
    'ios/HSynth', 'ios/project.yml',
    'third_party/miniaudio',
    'android/app/src/main/assets/presets', 'android/app/src/main/assets/library/factory',
    'ios-distribution/scripts/distribute.py', 'distribution.json',
]
README = '''# HSynth iOS app source (build snapshot)

The source of the HSynth iOS app as of the latest release in this repository: each release
tag is built from here into the unsigned IPA that source.json lists. Development happens
elsewhere; this folder receives one snapshot per release.
'''
IGNORE = 'ios/HSynth.xcodeproj/\ndist/\n.ios-build/\n__pycache__/\n'


def git(*args, cwd=ROOT, check=True, **kw):
    return subprocess.run(['git', *args], cwd=cwd, check=check, capture_output=True, text=True, **kw).stdout.strip()


def main():
    p = argparse.ArgumentParser()
    p.add_argument('tag')
    p.add_argument('--dry-run', action='store_true')
    a = p.parse_args()
    distribute.require(bool(re.fullmatch(r'v\d+\.\d+\.\d+', a.tag)), 'give the tag: vMAJOR.MINOR.PATCH')
    sha = git('rev-parse', f'{a.tag}^{{commit}}')
    url = f'https://github.com/{BUILD}.git'
    distribute.require(not git('ls-remote', '--tags', url, a.tag), f'{a.tag} already exists in {BUILD}')
    with tempfile.TemporaryDirectory() as tmp:
        work = Path(tmp) / 'repo'
        git('clone', '-q', '--depth', '1', url, str(work), cwd=tmp)
        src = work / 'src'
        if src.exists():
            shutil.rmtree(src)
        src.mkdir()
        tar = subprocess.run(['git', 'archive', '--format=tar', sha, *PATHS], cwd=ROOT, check=True,
                             capture_output=True).stdout
        with tarfile.open(fileobj=io.BytesIO(tar)) as t:
            names = [m.name for m in t.getmembers() if m.isfile()]
            t.extractall(src, filter='data')
        (src / 'README.md').write_text(README, encoding='utf-8', newline='\n')
        (src / '.gitignore').write_text(IGNORE, encoding='utf-8', newline='\n')
        wf = work / '.github' / 'workflows'
        wf.mkdir(parents=True, exist_ok=True)
        shutil.copy(HERE.parent / 'public_build' / 'ios.yml', wf / 'ios.yml')
        size = sum(f.stat().st_size for f in src.rglob('*') if f.is_file())
        print(f'{len(names)} files from {a.tag} ({sha[:7]}), {size / 1e6:.1f} MB -> {BUILD}/src')
        if a.dry_run:
            for n in sorted({'/'.join(n.split('/')[:3]) for n in names}):
                print('  ', n)
            print('top level kept:', sorted(c.name for c in work.iterdir() if c.name not in ('.git', 'src', '.github')))
            return
        env = dict(os.environ, GIT_AUTHOR_NAME=IDENTITY[0], GIT_AUTHOR_EMAIL=IDENTITY[1],
                   GIT_COMMITTER_NAME=IDENTITY[0], GIT_COMMITTER_EMAIL=IDENTITY[1])
        git('add', '-A', cwd=work)
        git('-c', 'commit.gpgsign=false', 'commit', '-q', '-m', f'HSynth {a.tag} source', cwd=work, env=env)
        git('tag', a.tag, cwd=work, env=env)
        git('push', '-q', 'origin', 'HEAD:main', cwd=work)
        git('push', '-q', 'origin', a.tag, cwd=work)
        print(f'pushed {a.tag}: the build runs at https://github.com/{BUILD}/actions')
        print(f'then: python ios-distribution/scripts/publish_public.py {a.tag}')


if __name__ == '__main__':
    main()
