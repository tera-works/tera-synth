#!/usr/bin/env python3
"""HSynth SysEx remote control from a PC (protocol: docs/SYSEX.md).

Talks to HSynth (Android / iPhone / desktop) over any MIDI connection:
USB MIDI, Bluetooth MIDI, network MIDI (rtpMIDI) or a loopback port such as
loopMIDI. Needs nothing extra on Windows (winmm through ctypes); elsewhere it
uses mido + python-rtmidi (pip install mido python-rtmidi).

Examples:
  python hsynth_remote.py ports
  python hsynth_remote.py -p "loopMIDI Port" hello
  python hsynth_remote.py -p HSynth get filter.cutoff filter.reso
  python hsynth_remote.py -p HSynth set filter.cutoff=1200 filter.type=LP24
  python hsynth_remote.py -p HSynth state-save my_sound.json
  python hsynth_remote.py -p HSynth state-load my_sound.json
  python hsynth_remote.py -p HSynth presets piano
  python hsynth_remote.py -p HSynth load "Grand Piano" --part 2
  python hsynth_remote.py -p HSynth note 60 --vel 0.8 --dur 0.5
  python hsynth_remote.py -p HSynth watch --what params status
  python hsynth_remote.py -p HSynth req '{"op":"phrase","cmd":"record","bars":4}'
  python hsynth_remote.py -p HSynth shell          (type JSON or commands interactively)
  python hsynth_remote.py --sim engine/build-desktop/hsynth_remote_sim.exe hello   (no MIDI needed)
  python hsynth_remote.py ble-scan                     (Bluetooth LE MIDI devices nearby; pip install bleak)
  python hsynth_remote.py --ble Galaxy status         (straight over Bluetooth, no MIDI driver needed)

-p takes a port name (or a part of it); the same name is used for input and
output unless --in / --out are given.
"""
import argparse
import json
import os
import shlex
import subprocess
import sys
import threading
import time
import queue

MANUFACTURER = 0x7D
ID = (0x48, 0x53)
VERSION = 0x01
REQUEST, REPLY, NOTIFY = 1, 2, 3
ALL_DEVICES = 0x7F
CHUNK = 240


# ---------------------------------------------------------------- framing

def encode(device, typ, seq, text):
    data = text.encode('ascii')
    total = max(1, (len(data) + CHUNK - 1) // CHUNK)
    out = []
    for k in range(total):
        chunk = data[k * CHUNK:(k + 1) * CHUNK]
        out.append(bytes([0xF0, MANUFACTURER, ID[0], ID[1], VERSION, device & 0x7F, typ, seq & 0x7F,
                          k & 0x7F, (k >> 7) & 0x7F, total & 0x7F, (total >> 7) & 0x7F]) + chunk + b'\xF7')
    return out


def parse(msg):
    if len(msg) < 13 or msg[0] != 0xF0 or msg[-1] != 0xF7:
        return None
    if msg[1] != MANUFACTURER or msg[2] != ID[0] or msg[3] != ID[1] or msg[4] != VERSION:
        return None
    return dict(device=msg[5], type=msg[6], seq=msg[7], index=msg[8] | msg[9] << 7,
                total=msg[10] | msg[11] << 7, payload=bytes(msg[12:-1]).decode('ascii', 'replace'))


class Assembler:
    def __init__(self):
        self.partial = {}

    def add(self, f):
        if f['total'] == 1:
            return f['payload']
        key = (f['type'], f['seq'])
        p = self.partial.get(key)
        if p is None or p['total'] != f['total'] or (f['index'] == 0 and p['chunks'][0] is not None):
            p = self.partial[key] = dict(total=f['total'], chunks=[None] * f['total'])
        p['chunks'][f['index']] = f['payload']
        if any(c is None for c in p['chunks']):
            return None
        del self.partial[key]
        return ''.join(p['chunks'])


# ---------------------------------------------------------------- MIDI backends

class WinMM:
    """Windows multimedia MIDI through ctypes (no packages needed)."""

    def __init__(self, in_name, out_name):
        import ctypes
        from ctypes import wintypes
        self.ct = ctypes
        self.mm = ctypes.WinDLL('winmm')
        self.rx = queue.Queue()
        self.cur = bytearray()

        class MIDIHDR(ctypes.Structure):
            _fields_ = [('lpData', ctypes.c_void_p), ('dwBufferLength', wintypes.DWORD),
                        ('dwBytesRecorded', wintypes.DWORD), ('dwUser', ctypes.c_size_t),
                        ('dwFlags', wintypes.DWORD), ('lpNext', ctypes.c_void_p),
                        ('reserved', ctypes.c_size_t), ('dwOffset', wintypes.DWORD),
                        ('dwReserved', ctypes.c_size_t * 8)]
        self.MIDIHDR = MIDIHDR
        self.inputs, self.outputs = WinMM.ports(ctypes)
        i = pick(self.inputs, in_name, 'input')
        o = pick(self.outputs, out_name, 'output')
        self.hin = wintypes.HANDLE()
        self.hout = wintypes.HANDLE()
        proto = ctypes.WINFUNCTYPE(None, wintypes.HANDLE, wintypes.UINT, ctypes.c_size_t, ctypes.c_size_t, ctypes.c_size_t)
        self.cb = proto(self._callback)
        if self.mm.midiInOpen(ctypes.byref(self.hin), i, self.cb, 0, 0x30000) != 0:  # CALLBACK_FUNCTION
            raise SystemExit(f'cannot open MIDI input {self.inputs[i]!r} (in use by another program?)')
        if self.mm.midiOutOpen(ctypes.byref(self.hout), o, 0, 0, 0) != 0:
            raise SystemExit(f'cannot open MIDI output {self.outputs[o]!r}')
        self.bufs, self.hdrs = [], []
        for _ in range(16):
            buf = ctypes.create_string_buffer(1024)
            h = MIDIHDR()
            h.lpData = ctypes.cast(buf, ctypes.c_void_p)
            h.dwBufferLength = 1024
            self.mm.midiInPrepareHeader(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            self.mm.midiInAddBuffer(self.hin, ctypes.byref(h), ctypes.sizeof(h))
            self.bufs.append(buf)
            self.hdrs.append(h)
        self.closing = False
        self.mm.midiInStart(self.hin)
        self.name = f'{self.inputs[i]} / {self.outputs[o]}'

    @staticmethod
    def ports(ctypes=None):
        import ctypes as ct
        from ctypes import wintypes
        mm = ct.WinDLL('winmm')

        class INCAPS(ct.Structure):
            _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD), ('vDriverVersion', wintypes.UINT),
                        ('szPname', wintypes.WCHAR * 32), ('dwSupport', wintypes.DWORD)]

        class OUTCAPS(ct.Structure):
            _fields_ = [('wMid', wintypes.WORD), ('wPid', wintypes.WORD), ('vDriverVersion', wintypes.UINT),
                        ('szPname', wintypes.WCHAR * 32), ('wTechnology', wintypes.WORD), ('wVoices', wintypes.WORD),
                        ('wNotes', wintypes.WORD), ('wChannelMask', wintypes.WORD), ('dwSupport', wintypes.DWORD)]
        ins, outs = [], []
        for i in range(mm.midiInGetNumDevs()):
            c = INCAPS()
            mm.midiInGetDevCapsW(i, ct.byref(c), ct.sizeof(c))
            ins.append(c.szPname)
        for i in range(mm.midiOutGetNumDevs()):
            c = OUTCAPS()
            mm.midiOutGetDevCapsW(i, ct.byref(c), ct.sizeof(c))
            outs.append(c.szPname)
        return ins, outs

    def _callback(self, h, msg, inst, p1, p2):
        if msg != 0x3C4:  # MIM_LONGDATA
            return
        hdr = self.MIDIHDR.from_address(p1)
        data = self.ct.string_at(hdr.lpData, hdr.dwBytesRecorded)
        for c in data:
            if c == 0xF0:
                self.cur = bytearray()
            if c == 0xF0 or self.cur:
                self.cur.append(c)
            if c == 0xF7 and self.cur:
                self.rx.put(bytes(self.cur))
                self.cur = bytearray()
        if not self.closing:
            self.mm.midiInAddBuffer(self.hin, self.ct.byref(hdr), self.ct.sizeof(hdr))

    def send(self, msg):
        ct = self.ct
        buf = ct.create_string_buffer(bytes(msg), len(msg))
        h = self.MIDIHDR()
        h.lpData = ct.cast(buf, ct.c_void_p)
        h.dwBufferLength = len(msg)
        h.dwBytesRecorded = len(msg)
        self.mm.midiOutPrepareHeader(self.hout, ct.byref(h), ct.sizeof(h))
        self.mm.midiOutLongMsg(self.hout, ct.byref(h), ct.sizeof(h))
        for _ in range(2000):
            if h.dwFlags & 1:  # MHDR_DONE
                break
            time.sleep(0.001)
        while self.mm.midiOutUnprepareHeader(self.hout, ct.byref(h), ct.sizeof(h)) == 65:  # STILLPLAYING
            time.sleep(0.001)

    def recv(self, timeout):
        try:
            return self.rx.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.closing = True
        self.mm.midiInStop(self.hin)
        self.mm.midiInReset(self.hin)
        for h in self.hdrs:
            self.mm.midiInUnprepareHeader(self.hin, self.ct.byref(h), self.ct.sizeof(h))
        self.mm.midiInClose(self.hin)
        self.mm.midiOutClose(self.hout)


class Mido:
    def __init__(self, in_name, out_name):
        import mido
        self.mido = mido
        ins, outs = mido.get_input_names(), mido.get_output_names()
        self.inp = mido.open_input(ins[pick(ins, in_name, 'input')])
        self.out = mido.open_output(outs[pick(outs, out_name, 'output')])
        self.name = f'{self.inp.name} / {self.out.name}'

    @staticmethod
    def ports():
        import mido
        return mido.get_input_names(), mido.get_output_names()

    def send(self, msg):
        self.out.send(self.mido.Message('sysex', data=list(msg[1:-1])))

    def recv(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            m = self.inp.poll()
            if m is not None and m.type == 'sysex':
                return bytes([0xF0] + list(m.data) + [0xF7])
            if m is None:
                time.sleep(0.002)
        return None

    def close(self):
        self.inp.close()
        self.out.close()


class Sim:
    """hsynth_remote_sim: the engine without MIDI (hex lines over a pipe)."""

    def __init__(self, exe, args):
        exe = os.path.abspath(exe)
        self.p = subprocess.Popen([exe] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.rx = queue.Queue()
        threading.Thread(target=self._read, daemon=True).start()
        self.name = os.path.basename(exe)

    def _read(self):
        for line in self.p.stdout:
            line = line.strip()
            if line:
                self.rx.put(bytes(int(x, 16) for x in line.split()))

    def send(self, msg):
        self.p.stdin.write(' '.join(f'{b:02X}' for b in msg) + '\n')
        self.p.stdin.flush()

    def recv(self, timeout):
        try:
            return self.rx.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        self.p.stdin.close()
        self.p.wait(5)


BLE_SERVICE = '03b80e5a-ede8-4b33-a751-6ce34ec4c700'
BLE_CHAR = '7772e5db-3868-4112-a1a9-f2669d106bf3'


def ble_decode(state, pkt, out):
    """BLE MIDI packet -> complete MIDI messages (state: dict kept per connection)."""
    if len(pkt) < 2 or not pkt[0] & 0x80:
        return
    i, n = 1, len(pkt)
    while i < n:
        b = pkt[i]
        if state.get('sx') is not None:
            if b < 0x80:
                state['sx'].append(b)
                i += 1
                continue
            i += 1
            if i >= n:
                break
            c = pkt[i]
            if c == 0xF7:
                out(bytes(state['sx'] + [0xF7]))
                state['sx'] = None
                i += 1
            elif c >= 0xF8:
                out(bytes([c]))
                i += 1
            else:
                state['sx'] = None
                i -= 1
            continue
        if b & 0x80:
            i += 1
            if i >= n:
                break
            c = pkt[i]
            if c & 0x80:
                i += 1
                if c == 0xF0:
                    state['sx'] = [0xF0]
                    continue
                if c >= 0xF8:
                    out(bytes([c]))
                    continue
                status = c
                state['run'] = c if c < 0xF0 else 0
            else:
                status = state.get('run', 0)
        else:
            status = state.get('run', 0)
        if not status:
            i += 1
            continue
        nd = 1 if (status & 0xF0) in (0xC0, 0xD0) or status in (0xF1, 0xF3) else (0 if status >= 0xF4 else 2)
        data = []
        while len(data) < nd and i < n and pkt[i] < 0x80:
            data.append(pkt[i])
            i += 1
        if len(data) == nd:
            out(bytes([status] + data))


def ble_encode(msg, max_packet, ms):
    header, ts = 0x80 | ((ms >> 7) & 0x3F), 0x80 | (ms & 0x7F)
    if msg[0] != 0xF0:
        return [bytes([header, ts]) + bytes(msg)]
    body = msg[:-1] if msg[-1] == 0xF7 else msg
    out, pk = [], [header, ts]
    for b in body:
        if len(pk) >= max_packet:
            out.append(bytes(pk))
            pk = [header]
        pk.append(b)
    if len(pk) + 2 > max_packet:
        out.append(bytes(pk))
        pk = [header]
    out.append(bytes(pk + [ts, 0xF7]))
    return out


class Ble:
    """Bluetooth LE MIDI straight from the PC (pip install bleak): connects to a BLE MIDI
    device by name, e.g. HSynth on Android in peripheral mode or an iPhone advertising."""

    def __init__(self, name):
        try:
            import bleak  # noqa: F401
        except ImportError:
            raise SystemExit('Bluetooth needs bleak:  pip install bleak')
        import asyncio
        self.asyncio = asyncio
        self.rx = queue.Queue()
        self.state = {}
        self.t0 = time.time()
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        self.client, self.name = self._run(self._connect(name), 40)

    def _run(self, coro, timeout=20):
        return self.asyncio.run_coroutine_threadsafe(coro, self.loop).result(timeout)

    @staticmethod
    async def scan_async(seconds=6.0):
        from bleak import BleakScanner
        found = await BleakScanner.discover(timeout=seconds, service_uuids=[BLE_SERVICE], return_adv=True)
        return [(d.name or adv.local_name or d.address, d.address) for d, adv in found.values()]

    @staticmethod
    def scan(seconds=6.0):
        import asyncio
        return asyncio.run(Ble.scan_async(seconds))

    async def _connect(self, want):
        from bleak import BleakClient
        devs = await Ble.scan_async()
        hit = [d for d in devs if want and (want.lower() in d[0].lower() or want.lower() == d[1].lower())]
        if not hit:
            raise SystemExit(f'Bluetooth MIDI device {want!r} not found (found: {", ".join(d[0] for d in devs) or "none"})')
        client = BleakClient(hit[0][1])
        await client.connect()
        await client.start_notify(BLE_CHAR, lambda _c, data: ble_decode(self.state, bytes(data), self.rx.put))
        return client, f'BLE {hit[0][0]}'

    def send(self, msg):
        mtu = getattr(self.client, 'mtu_size', 23) or 23
        ms = int((time.time() - self.t0) * 1000)
        for p in ble_encode(msg, max(20, mtu - 3), ms):
            self._run(self.client.write_gatt_char(BLE_CHAR, p, response=False))

    def recv(self, timeout):
        try:
            return self.rx.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self):
        try:
            self._run(self.client.disconnect(), 10)
        except Exception:
            pass
        self.loop.call_soon_threadsafe(self.loop.stop)


def pick(names, want, what):
    if not names:
        raise SystemExit(f'no MIDI {what} ports')
    if want is None:
        raise SystemExit(f'choose a port with -p (MIDI {what}s: {", ".join(names)})')
    for i, n in enumerate(names):
        if n == want:
            return i
    for i, n in enumerate(names):
        if want.lower() in n.lower():
            return i
    raise SystemExit(f'MIDI {what} {want!r} not found (have: {", ".join(names)})')


def backend_class():
    if sys.platform == 'win32':
        return WinMM
    try:
        import mido  # noqa: F401
        return Mido
    except ImportError:
        raise SystemExit('install mido and python-rtmidi:  pip install mido python-rtmidi')


# ---------------------------------------------------------------- client

class Client:
    def __init__(self, port, device=ALL_DEVICES, verbose=False):
        self.port = port
        self.device = device
        self.verbose = verbose
        self.seq = 0
        self.asm = Assembler()
        self.notes = []  # notifications received while waiting

    def request(self, obj, timeout=10.0):
        self.seq = self.seq % 127 + 1
        text = json.dumps(obj, ensure_ascii=True, separators=(',', ':'))
        for m in encode(self.device, REQUEST, self.seq, text):
            if self.verbose:
                print('>>', m.hex(' ').upper(), file=sys.stderr)
            self.port.send(m)
        end = time.time() + timeout
        while time.time() < end:
            m = self.port.recv(0.05)
            if m is None:
                continue
            if self.verbose:
                print('<<', m.hex(' ').upper(), file=sys.stderr)
            f = parse(m)
            if not f or f['type'] == REQUEST:
                continue
            whole = self.asm.add(f)
            if whole is None:
                continue
            body = json.loads(whole)
            if f['type'] == NOTIFY:
                self.notes.append(body)
                continue
            if f['seq'] == self.seq:
                return body
        raise SystemExit(f'no reply to {obj.get("op")!r} within {timeout:.0f} s (is HSynth listening on this port?)')

    def listen(self, seconds=None):
        end = None if seconds is None else time.time() + seconds
        while end is None or time.time() < end:
            while self.notes:
                yield self.notes.pop(0)
            m = self.port.recv(0.1)
            if m is None:
                continue
            f = parse(m)
            if not f or f['type'] != NOTIFY:
                continue
            whole = self.asm.add(f)
            if whole is not None:
                yield json.loads(whole)


def value(text):
    """'1200' -> 1200, 'true' -> True, 'LP24' -> 'LP24'"""
    t = text.strip()
    if t.lower() in ('true', 'on'):
        return True
    if t.lower() in ('false', 'off'):
        return False
    try:
        return int(t)
    except ValueError:
        pass
    try:
        return float(t)
    except ValueError:
        return t


def show(obj):
    print(json.dumps(obj, ensure_ascii=False, indent=2))


def fail_if_error(r):
    if not r.get('ok', False):
        print('error:', r.get('error'), file=sys.stderr)
        return 1
    return 0


def run_command(c, argv):
    ap = argparse.ArgumentParser(prog='hsynth_remote.py COMMAND', add_help=True)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('hello')
    sub.add_parser('status')
    s = sub.add_parser('params', help='parameter list (all pages)')
    s.add_argument('prefix', nargs='?', default='')
    s = sub.add_parser('get')
    s.add_argument('names', nargs='*', help='names (empty: every parameter); "prefix." for a group')
    s.add_argument('--part', type=int)
    s.add_argument('--changed', action='store_true', help='only values that differ from the default')
    s = sub.add_parser('set')
    s.add_argument('pairs', nargs='+', help='name=value')
    s.add_argument('--part', type=int)
    s.add_argument('--relative', action='store_true')
    s = sub.add_parser('mix')
    s.add_argument('part', type=int, nargs='?')
    s.add_argument('pairs', nargs='*', help='volume=0.8 pan=-0.2 mute=true channel=2 ...')
    s = sub.add_parser('mode')
    s.add_argument('mode', choices=['single', 'multi'])
    s = sub.add_parser('select')
    s.add_argument('part', type=int, help='0..15')
    s = sub.add_parser('state-save')
    s.add_argument('file')
    s.add_argument('--full', action='store_true', help='every parameter (default: only non-default ones)')
    s = sub.add_parser('state-load')
    s.add_argument('file', help='a state saved by state-save, or a preset / multi JSON')
    s = sub.add_parser('presets')
    s.add_argument('query', nargs='?', default='')
    s.add_argument('--category', default='')
    s.add_argument('--rescan', action='store_true')
    s = sub.add_parser('load')
    s.add_argument('name', help='preset name (or --file)')
    s.add_argument('--file', action='store_true', help='NAME is a preset file path on the device')
    s.add_argument('--part', type=int)
    s = sub.add_parser('save')
    s.add_argument('name')
    s.add_argument('--category', default='')
    s.add_argument('--part', type=int)
    s = sub.add_parser('samples')
    s.add_argument('layer', choices=['a', 'b', 'c'])
    s.add_argument('file', help='library-relative SFZ / WAV / FLAC ("" clears the layer)')
    s.add_argument('--part', type=int)
    s = sub.add_parser('files')
    s.add_argument('kind', choices=['samples', 'impulses', 'midi', 'presets'])
    s.add_argument('dir', nargs='?', default='')
    s = sub.add_parser('note')
    s.add_argument('key', type=int)
    s.add_argument('--vel', type=float, default=0.8)
    s.add_argument('--dur', type=float, default=0.5)
    s.add_argument('--channel', type=int, default=0)
    sub.add_parser('panic')
    sub.add_parser('songs')
    s = sub.add_parser('song-load')
    s.add_argument('file')
    s = sub.add_parser('song')
    s.add_argument('action', choices=['play', 'stop', 'rewind', 'seek'])
    s.add_argument('position', type=float, nargs='?', default=0.0)
    s = sub.add_parser('phrase')
    s.add_argument('action', choices=['record', 'play', 'stop', 'clear', 'undo', 'quantize', 'export', 'status'])
    s.add_argument('--bars', type=int)
    s.add_argument('--grid', type=float, default=0.25)
    s.add_argument('--file', default='phrase.mid')
    s = sub.add_parser('looper')
    s.add_argument('action', choices=['tap', 'play', 'stop', 'clear', 'undo', 'status'])
    s = sub.add_parser('settings')
    s.add_argument('pairs', nargs='*', help='max_voices=64 quality=high render_threads=2')
    s = sub.add_parser('meters')
    s = sub.add_parser('learn', help='MIDI learn: get / arm PARAM / cancel / remove PARAM / clear')
    s.add_argument('action', nargs='?', default='get', choices=['get', 'arm', 'cancel', 'remove', 'clear'])
    s.add_argument('param', nargs='?')
    s.add_argument('--part', type=int)
    s = sub.add_parser('watch', help='print change notifications (Ctrl+C to stop)')
    s.add_argument('--what', nargs='+', default=['params'], choices=['params', 'status', 'meters', 'all'])
    s.add_argument('--interval', type=int, default=50, help='ms')
    s = sub.add_parser('req', help='a raw JSON request')
    s.add_argument('json')
    a = ap.parse_args(argv)

    def part(r):
        if getattr(a, 'part', None) is not None:
            r['part'] = a.part
        return r

    if a.cmd in ('hello', 'status', 'panic', 'songs', 'meters'):
        r = c.request({'op': a.cmd})
    elif a.cmd == 'params':
        out, off = [], 0
        while True:
            r = c.request({'op': 'param_info', 'offset': off, 'count': 100, 'prefix': a.prefix})
            out += r['params']
            off += len(r['params'])
            if not r['params'] or off >= r['total']:
                break
        for p in out:
            extra = f' {p["options"]}' if 'options' in p else f' {p["min"]}..{p["max"]} {p.get("unit", "")}'
            print(f'{p["name"]:32} {p["kind"]:5} def {p["def"]}{extra}{"  (part)" if p["part"] else ""}')
        return 0
    elif a.cmd == 'get':
        req = part({'op': 'get', 'changed': a.changed})
        if len(a.names) == 1 and a.names[0].endswith('.'):
            req['names'] = a.names[0]
        elif a.names:
            req['names'] = a.names
        r = c.request(req)
        if r.get('ok'):
            for k, v in r['values'].items():
                print(f'{k} = {v}')
            return 0
    elif a.cmd == 'set':
        vals = {}
        for p in a.pairs:
            k, _, v = p.partition('=')
            vals[k] = value(v)
        r = c.request(part({'op': 'set', 'values': vals, 'relative': a.relative}))
    elif a.cmd == 'mix':
        if a.pairs:
            vals = {k: value(v) for k, _, v in (p.partition('=') for p in a.pairs)}
            r = c.request({'op': 'mix_set', 'part': a.part or 0, 'values': vals})
        else:
            r = c.request({'op': 'mix_get', **({'part': a.part} if a.part is not None else {})})
    elif a.cmd == 'mode':
        r = c.request({'op': 'mode', 'multi': a.mode == 'multi'})
    elif a.cmd == 'select':
        r = c.request({'op': 'select_part', 'part': a.part})
    elif a.cmd == 'state-save':
        r = c.request({'op': 'get_state', 'compact': not a.full}, timeout=30)
        if r.get('ok'):
            with open(a.file, 'w', encoding='utf-8') as f:
                json.dump(r['state'], f, ensure_ascii=False, indent=2)
            print(f'saved {a.file} ({"multi" if r["multi"] else "single"})')
            return 0
    elif a.cmd == 'state-load':
        with open(a.file, encoding='utf-8') as f:
            state = json.load(f)
        if 'state' in state and 'format' not in state:
            state = state['state']
        r = c.request({'op': 'set_state', 'state': state}, timeout=60)
    elif a.cmd == 'presets':
        out, off = [], 0
        while True:
            r = c.request({'op': 'presets', 'query': a.query, 'category': a.category, 'rescan': a.rescan and off == 0,
                           'offset': off, 'count': 200}, timeout=30)
            if not r.get('ok'):
                break
            out += r['presets']
            off += len(r['presets'])
            if not r['presets'] or off >= r['total']:
                break
        for p in out:
            print(f'{p["category"] or "-":16} {p["name"]}')
        return fail_if_error(r)
    elif a.cmd == 'load':
        r = c.request(part({'op': 'load_preset', ('file' if a.file else 'name'): a.name}), timeout=60)
    elif a.cmd == 'save':
        r = c.request(part({'op': 'save_preset', 'name': a.name, 'category': a.category}))
    elif a.cmd == 'samples':
        r = c.request(part({'op': 'samples', 'layer': a.layer, 'file': a.file}), timeout=60)
    elif a.cmd == 'files':
        r = c.request({'op': 'files', 'kind': a.kind, 'dir': a.dir})
    elif a.cmd == 'note':
        base = {'op': 'note', 'key': a.key, 'vel': a.vel}
        if a.channel:
            base['channel'] = a.channel
        c.request({**base, 'on': True})
        time.sleep(a.dur)
        r = c.request({**base, 'on': False})
    elif a.cmd == 'song-load':
        r = c.request({'op': 'song_load', 'file': a.file}, timeout=120)
    elif a.cmd == 'song':
        r = c.request({'op': 'song', 'cmd': a.action, 'position': a.position})
    elif a.cmd == 'phrase':
        req = {'op': 'phrase'}
        if a.action != 'status':
            req['cmd'] = a.action
        if a.bars is not None:
            req['bars'] = a.bars
        if a.action == 'quantize':
            req['grid'] = a.grid
        if a.action == 'export':
            req['file'] = a.file
        r = c.request(req)
    elif a.cmd == 'looper':
        r = c.request({'op': 'looper', **({} if a.action == 'status' else {'cmd': a.action})})
    elif a.cmd == 'settings':
        r = c.request({'op': 'settings', **{k: value(v) for k, _, v in (p.partition('=') for p in a.pairs)}})
    elif a.cmd == 'watch':
        r = c.request({'op': 'subscribe', 'what': a.what, 'interval_ms': a.interval})
        if fail_if_error(r):
            return 1
        print('watching (Ctrl+C to stop)', file=sys.stderr)
        try:
            for n in c.listen():
                print(json.dumps(n, ensure_ascii=False))
                sys.stdout.flush()
        except KeyboardInterrupt:
            pass
        c.request({'op': 'unsubscribe', 'what': 'all'})
        return 0
    elif a.cmd == 'learn':
        req = part({'op': 'learn', 'cmd': a.action})
        if a.param:
            req['param'] = a.param
        r = c.request(req)
    elif a.cmd == 'req':
        r = c.request(json.loads(a.json), timeout=60)
    else:
        ap.error('unknown command')
        return 2
    show(r)
    return fail_if_error(r)


def shell(c):
    print('HSynth remote shell: JSON requests ({"op":"status"}) or commands (get filter.cutoff); "quit" ends.')
    while True:
        try:
            line = input('hsynth> ').strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not line:
            continue
        if line in ('quit', 'exit'):
            return 0
        try:
            if line.startswith('{') or line.startswith('req '):
                show(c.request(json.loads(line[4:] if line.startswith('req ') else line), timeout=60))
            else:
                run_command(c, shlex.split(line))
        except SystemExit as e:
            if e.code not in (0, None):
                print(e)
        except Exception as e:  # keep the shell alive
            print('error:', e)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-p', '--port', help='MIDI port name (or part of it) for input and output')
    ap.add_argument('--in', dest='inp', help='input port (default: --port)')
    ap.add_argument('--out', help='output port (default: --port)')
    ap.add_argument('--device', type=int, default=ALL_DEVICES, help='device id 0..126 (default 127 = all)')
    ap.add_argument('--ble', metavar='NAME', help='connect over Bluetooth LE MIDI to this device (pip install bleak)')
    ap.add_argument('--sim', help='path of hsynth_remote_sim (instead of MIDI)')
    ap.add_argument('--sim-args', default='', help='arguments for the simulator')
    ap.add_argument('-v', '--verbose', action='store_true', help='print the SysEx bytes')
    ap.add_argument('command', nargs=argparse.REMAINDER)
    a = ap.parse_args()
    if not a.command:
        ap.print_help()
        return 2
    if a.command[0] == 'ble-scan':
        for name, addr in Ble.scan():
            print(f'{name}  ({addr})')
        return 0
    if a.command[0] == 'ports':
        ins, outs = backend_class().ports()
        print('inputs: ', *ins, sep='\n  ')
        print('outputs:', *outs, sep='\n  ')
        return 0
    if a.sim:
        port = Sim(a.sim, shlex.split(a.sim_args))
    elif a.ble:
        port = Ble(a.ble)
    else:
        port = backend_class()(a.inp or a.port, a.out or a.port)
    try:
        c = Client(port, a.device, a.verbose)
        if a.command[0] == 'shell':
            return shell(c)
        return run_command(c, a.command)
    finally:
        port.close()


if __name__ == '__main__':
    sys.exit(main())
