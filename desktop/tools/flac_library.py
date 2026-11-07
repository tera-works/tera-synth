"""Converts the sampled library (SFZ instruments with WAV samples) to FLAC, losslessly.

usage: python desktop/tools/flac_library.py [LIBRARY_DIR] [--jobs N] [--keep-wav] [--dry-run]
       (default LIBRARY_DIR: desktop/data/library/user)

For every instrument folder (a folder with .sfz files):
  1. each WAV the SFZ files use is encoded to FLAC with ffmpeg (compression level 8);
  2. the WAV's "smpl" chunk (root key, loop) is embedded in the FLAC as an
     APPLICATION "riff" block, which the engine's FLAC reader understands, so
     the sample behaves exactly like the WAV did;
  3. the FLAC is decoded again and compared with the WAV, sample by sample;
  4. only when every sample of the folder checks out, the SFZ files are
     rewritten to the .flac names and the WAVs are deleted (--keep-wav keeps them).
Folders changed in the last 10 minutes are skipped (the sampler may still be
writing them; it writes the .sfz last). Run it again later for new instruments.
Needs ffmpeg on the PATH. Test material only: never distribute the result.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT = os.path.normpath(os.path.join(HERE, '..', 'data', 'library', 'user'))
QUIET_SECONDS = 600


def wav_chunks(data):
    """(fmt, data bytes, smpl chunk bytes incl. header or None)."""
    if data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise ValueError('not a WAV file')
    i, fmt, pcm, smpl = 12, None, None, None
    while i + 8 <= len(data):
        cid, n = data[i:i + 4], struct.unpack('<I', data[i + 4:i + 8])[0]
        body = data[i + 8:i + 8 + n]
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif cid == b'data':
            pcm = body
        elif cid == b'smpl':
            smpl = data[i:i + 8 + n]
        i += 8 + n + (n & 1)
    if fmt is None or pcm is None:
        raise ValueError('no fmt / data chunk')
    return fmt, pcm, smpl


def add_riff_block(flac, smpl):
    """Inserts an APPLICATION block 'riff' + smpl chunk as the last metadata block."""
    if flac[:4] != b'fLaC':
        raise ValueError('ffmpeg did not write FLAC')
    pos, blocks = 4, []
    while True:
        hdr = flac[pos]
        n = int.from_bytes(flac[pos + 1:pos + 4], 'big')
        blocks.append((hdr & 0x7F, flac[pos + 4:pos + 4 + n]))
        pos += 4 + n
        if hdr & 0x80:
            break
    blocks = [b for b in blocks if not (b[0] == 2 and b[1][:4] == b'riff')]
    blocks.append((2, b'riff' + smpl))
    out = bytearray(b'fLaC')
    for k, (t, body) in enumerate(blocks):
        out.append(t | (0x80 if k == len(blocks) - 1 else 0))
        out += len(body).to_bytes(3, 'big') + body
    return bytes(out + flac[pos:])


def convert(wav):
    """WAV -> FLAC next to it, verified. Returns (flac path, wav bytes, flac bytes)."""
    flac = wav[:-4] + '.flac'
    data = open(wav, 'rb').read()
    (fmt_tag, channels, rate, _, align, bits), pcm, smpl = wav_chunks(data)
    if fmt_tag != 1 or bits not in (16, 24):
        raise ValueError(f'unsupported WAV format {fmt_tag}/{bits} bit')
    # to a file, not a pipe: ffmpeg fills in STREAMINFO (length, MD5) only when it can seek back,
    # and the engine streams a FLAC only when it knows its length
    tmp = flac + '.enc'
    subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', wav, '-map_metadata', '-1', '-c:a', 'flac',
                    '-compression_level', '8', '-f', 'flac', tmp], capture_output=True, check=True)
    with open(tmp, 'rb') as f:
        enc = f.read()
    os.remove(tmp)
    if int.from_bytes(enc[21:26], 'big') & 0xFFFFFFFFF == 0:
        raise ValueError('FLAC without length in STREAMINFO')
    out = add_riff_block(enc, smpl) if smpl else enc
    # lossless check: decode again and compare with the WAV's sample data
    raw = 's16le' if bits == 16 else 's24le'
    dec = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'flac', '-i', '-', '-f', raw, '-'],
                         input=out, capture_output=True, check=True).stdout
    if dec != pcm[:len(pcm) - len(pcm) % align]:
        raise ValueError('FLAC does not decode to the same samples')
    tmp = flac + '.tmp'
    with open(tmp, 'wb') as f:
        f.write(out)
    os.replace(tmp, flac)
    return flac, len(data), len(out)


SAMPLE_RE = re.compile(r'(sample=)(\S+?\.wav)\b', re.IGNORECASE)


def sfz_wavs(sfz):
    """WAV paths an SFZ uses (absolute), with default_path applied."""
    txt = open(sfz, encoding='utf-8', errors='replace').read()
    m = re.search(r'default_path=(\S+)', txt)
    base = m.group(1).replace('\\', '/') if m else ''
    d = os.path.dirname(sfz)
    return txt, [os.path.normpath(os.path.join(d, base + s.replace('\\', '/'))) for _, s in SAMPLE_RE.findall(txt)]


def folder_quiet(d):
    newest = 0
    for root, _, files in os.walk(d):
        for f in files:
            newest = max(newest, os.path.getmtime(os.path.join(root, f)))
    return time.time() - newest > QUIET_SECONDS


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('library', nargs='?', default=DEFAULT)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument('--keep-wav', action='store_true')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args()
    folders = {}
    for root, _, files in os.walk(a.library):
        sfzs = [os.path.join(root, f) for f in files if f.lower().endswith('.sfz')]
        if sfzs:
            folders[root] = sfzs
    total_w = total_f = done = skipped = failed = 0
    t0 = time.time()
    with ThreadPoolExecutor(a.jobs) as pool:
        for n, (d, sfzs) in enumerate(sorted(folders.items()), 1):
            parsed = [(s, *sfz_wavs(s)) for s in sfzs]
            wavs = sorted({w for _, _, ws in parsed for w in ws})
            if not wavs:
                continue
            if not folder_quiet(d):
                print(f'skip (changed in the last {QUIET_SECONDS // 60} min): {d}')
                skipped += 1
                continue
            missing = [w for w in wavs if not os.path.isfile(w)]
            if missing:
                print(f'skip (missing {len(missing)} WAV, e.g. {missing[0]}): {d}')
                skipped += 1
                continue
            if a.dry_run:
                total_w += sum(os.path.getsize(w) for w in wavs)
                done += 1
                continue
            try:
                results = list(pool.map(convert, wavs))
            except Exception as e:  # leave the folder as it was
                print(f'FAILED {d}: {e}')
                for w in wavs:
                    f = w[:-4] + '.flac'
                    if os.path.exists(f):
                        os.remove(f)
                failed += 1
                continue
            for sfz, txt, _ in parsed:
                new = SAMPLE_RE.sub(lambda m: m.group(1) + m.group(2)[:-4] + '.flac', txt)
                tmp = sfz + '.tmp'
                with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
                    f.write(new)
                os.replace(tmp, sfz)
            if not a.keep_wav:
                for w in wavs:
                    os.remove(w)
            wb, fb = sum(r[1] for r in results), sum(r[2] for r in results)
            total_w += wb
            total_f += fb
            done += 1
            print(f'[{n}/{len(folders)}] {os.path.relpath(d, a.library)}: {len(wavs)} samples, '
                  f'{wb / 1e6:.1f} -> {fb / 1e6:.1f} MB ({fb * 100 / wb:.0f} %)', flush=True)
    if a.dry_run:
        print(f'dry run: {done} folders, {total_w / 1e9:.2f} GB of WAV would be converted; {skipped} skipped')
        return
    pct = total_f * 100 / total_w if total_w else 0
    print(f'done: {done} folders, {total_w / 1e9:.2f} GB -> {total_f / 1e9:.2f} GB ({pct:.0f} %), '
          f'{skipped} skipped, {failed} failed, {time.time() - t0:.0f} s')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
