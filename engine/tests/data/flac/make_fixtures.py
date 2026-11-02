#!/usr/bin/env python3
"""Generates the FLAC decoder test fixtures (synthetic, integer-only signal).

The same signal is regenerated in engine/tests/test_flac.cpp, so the decoder
output can be compared sample by sample (FLAC is lossless). Requires ffmpeg.
usage: python3 make_fixtures.py   (run in this directory)
"""
import struct, subprocess, wave, os

N = 9000  # not a multiple of any block size: exercises the short last frame


def signal(i, ch, bits):
    x = (i * 2654435761 + ch * 40503) & 0xFFFFFFFF
    noise = ((x >> 16) % 201) - 100
    period = 100 if ch == 0 else 73
    ph = i % period
    tri = (ph * 4 * 8000 // period) - 8000 if ph < period // 2 else 24000 - (ph * 4 * 8000 // period)
    sq = 3000 if (i % 500) < 250 else -3000
    v = tri + noise + (sq if ch == 1 else 0)
    if 3000 <= i < 3600:
        v = 0 if ch == 0 else 1234  # constant subframes
    if bits == 24:
        v = v * 256 + (noise & 0xFF)
    return v


def write_wav(path, channels, bits, rate=44100):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        frames = bytearray()
        for i in range(N):
            for c in range(channels):
                v = signal(i, c, bits)
                frames += struct.pack("<h", v) if bits == 16 else struct.pack("<i", v)[:3]
        w.writeframes(bytes(frames))


def ffmpeg(src, dst, *args):
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", src, "-c:a", "flac", *args, dst], check=True)


write_wav("m16.wav", 1, 16)
write_wav("s16.wav", 2, 16)
write_wav("s24.wav", 2, 24)
ffmpeg("m16.wav", "m16_fixed.flac", "-compression_level", "0")
ffmpeg("m16.wav", "m16_lpc.flac", "-compression_level", "12")
ffmpeg("s16.wav", "s16_indep.flac", "-ch_mode", "indep", "-compression_level", "5")
ffmpeg("s16.wav", "s16_left_side.flac", "-ch_mode", "left_side")
ffmpeg("s16.wav", "s16_right_side.flac", "-ch_mode", "right_side")
ffmpeg("s16.wav", "s16_mid_side.flac", "-ch_mode", "mid_side", "-compression_level", "8")
ffmpeg("s24.wav", "s24_lpc.flac", "-sample_fmt", "s32", "-compression_level", "8")
for f in ("m16.wav", "s16.wav", "s24.wav"):
    os.remove(f)
