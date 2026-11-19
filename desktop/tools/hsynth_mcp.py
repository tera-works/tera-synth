#!/usr/bin/env python3
"""HSynth MCP server: lets an AI agent (Claude Code, Claude Desktop, ...) play and edit HSynth.

It talks to the desktop app on this PC through its local control (127.0.0.1:47800,
desktop/src/local_control.h; Monitor > "Local control (CLI / MCP / AI)") with the same
requests as hsynth_remote.py (docs/SYSEX.md). Standard library only; stdio transport.

Claude Code: the repository's .mcp.json registers it (approve "hsynth" when asked), or
  claude mcp add hsynth -- python desktop/tools/hsynth_mcp.py
Other clients: command "python", args ["<repo>/desktop/tools/hsynth_mcp.py"].
Options: --port 47800, --host 127.0.0.1 (env HSYNTH_PORT / HSYNTH_HOST).
"""
import base64
import json
import os
import struct
import sys
import threading
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hsynth_remote as hr  # noqa: E402

SERVER = {'name': 'hsynth', 'version': '0.8.7'}
_client = None
_lock = threading.Lock()


def _arg(name, default):
    if name in sys.argv:
        i = sys.argv.index(name)
        if i + 1 < len(sys.argv):
            return sys.argv[i + 1]
    return default


HOST = _arg('--host', os.environ.get('HSYNTH_HOST', '127.0.0.1'))
PORT = int(_arg('--port', os.environ.get('HSYNTH_PORT', str(hr.LOCAL_PORT))))


class HSynthError(Exception):
    pass


def request(obj, timeout=30.0):
    """One request to the app; reconnects once if the app was restarted."""
    global _client
    with _lock:
        for attempt in (0, 1):
            try:
                if _client is None:
                    _client = hr.Client(hr.Tcp(f'{HOST}:{PORT}'))
                return _client.request(obj, timeout=timeout)
            except SystemExit as e:  # hsynth_remote reports failures this way
                if _client is not None:
                    try:
                        _client.port.close()
                    except Exception:
                        pass
                _client = None
                if attempt == 1:
                    raise HSynthError(str(e))
            except OSError as e:
                _client = None
                if attempt == 1:
                    raise HSynthError(f'connection lost: {e}')


# ---------------------------------------------------------------- BMP -> PNG (screenshots)

def bmp_to_png(path, scale=1):
    data = open(path, 'rb').read()
    off = struct.unpack_from('<I', data, 10)[0]
    w, h = struct.unpack_from('<ii', data, 18)
    bpp = struct.unpack_from('<H', data, 28)[0]
    if bpp not in (24, 32):
        raise HSynthError(f'unsupported BMP ({bpp} bit)')
    bottom_up = h > 0
    h = abs(h)
    step = bpp // 8
    row = (w * step + 3) & ~3
    sw, sh = w // scale, h // scale
    raw = bytearray()
    for y in range(sh):
        sy = y * scale
        src = off + ((h - 1 - sy) if bottom_up else sy) * row
        raw.append(0)
        line = data[src:src + w * step]
        for x in range(sw):
            i = x * scale * step
            raw += bytes((line[i + 2], line[i + 1], line[i]))

    def chunk(tag, body):
        c = struct.pack('>I', len(body)) + tag + body
        return c + struct.pack('>I', zlib.crc32(tag + body) & 0xFFFFFFFF)

    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', sw, sh, 8, 2, 0, 0, 0)) +
            chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))


# ---------------------------------------------------------------- tools

def ok(r):
    if not r.get('ok', False):
        raise HSynthError(r.get('error', 'failed'))
    return r


def t_status(a):
    return ok(request({'op': 'status'}))


def t_request(a):
    req = a.get('request')
    if isinstance(req, str):
        req = json.loads(req)
    return request(req, timeout=float(a.get('timeout', 60)))


def t_params(a):
    out, off = [], 0
    while True:
        r = ok(request({'op': 'param_info', 'offset': off, 'count': 200, 'prefix': a.get('prefix', '')}))
        out += r['params']
        off += len(r['params'])
        if not r['params'] or off >= r['total']:
            break
    return {'total': len(out), 'params': out}


def t_get(a):
    req = {'op': 'get'}
    if a.get('names'):
        req['names'] = a['names']
    elif a.get('prefix'):
        req['names'] = a['prefix'] if a['prefix'].endswith('.') else a['prefix'] + '.'
    if 'part' in a:
        req['part'] = a['part']
    return ok(request(req))


def t_set(a):
    req = {'op': 'set', 'values': a['values'], 'relative': bool(a.get('relative', False))}
    if 'part' in a:
        req['part'] = a['part']
    return ok(request(req))


def t_presets(a):
    out, off = [], 0
    while True:
        r = ok(request({'op': 'presets', 'query': a.get('query', ''), 'category': a.get('category', ''),
                        'offset': off, 'count': 200}, timeout=30))
        out += r['presets']
        off += len(r['presets'])
        if not r['presets'] or off >= r['total'] or off >= int(a.get('limit', 200)):
            break
    return {'total': len(out), 'presets': [{'name': p['name'], 'category': p.get('category', '')} for p in out]}


def t_load(a):
    req = {'op': 'load_preset', 'name': a['name']}
    if 'part' in a:
        req['part'] = a['part']
    return ok(request(req, timeout=60))


def t_play(a):
    """Notes with start times: [{key, vel?, start?, dur?, channel?}] (seconds)."""
    notes = a.get('notes') or []
    events = []
    for n in notes:
        key = int(n['key'])
        vel = float(n.get('vel', 0.8))
        start = float(n.get('start', 0.0))
        dur = float(n.get('dur', 0.5))
        base = {'op': 'note', 'key': key, 'vel': vel}
        if n.get('channel'):
            base['channel'] = int(n['channel'])
        events.append((start, {**base, 'on': True}))
        events.append((start + dur, {**base, 'on': False}))
    events.sort(key=lambda e: (e[0], e[1]['on']))
    t0 = time.time()
    for at, req in events:
        wait = at - (time.time() - t0)
        if wait > 0:
            time.sleep(wait)
        request(req)
    return {'ok': True, 'notes': len(notes), 'seconds': round(time.time() - t0, 2)}


def t_panic(a):
    return ok(request({'op': 'panic'}))


def t_mix(a):
    if a.get('values'):
        return ok(request({'op': 'mix_set', 'part': a.get('part', 0), 'values': a['values']}))
    return ok(request({'op': 'mix_get', **({'part': a['part']} if 'part' in a else {})}))


def t_mode(a):
    return ok(request({'op': 'mode', 'multi': bool(a['multi'])}))


def t_select(a):
    return ok(request({'op': 'select_part', 'part': int(a['part'])}))


def t_player(a):
    req = {'op': 'host.player', 'cmd': a.get('cmd', 'list')}
    if 'index' in a:
        req['index'] = int(a['index'])
    if 'repeat' in a:
        req['repeat'] = a['repeat']
    r = ok(request(req, timeout=120))
    if a.get('cmd') == 'seek' or 'position' in a:
        request({'op': 'song', 'cmd': 'seek', 'position': float(a.get('position', 0))})
    return r


def t_live(a):
    req = {'op': 'host.live', 'cmd': a.get('cmd', 'list')}
    for k in ('index', 'list'):
        if k in a:
            req[k] = a[k]
    return ok(request(req, timeout=60))


def t_tab(a):
    return ok(request({'op': 'host.tab', 'tab': a['tab']}))


def t_log(a):
    return ok(request({'op': 'host.log', 'lines': int(a.get('lines', 40))}))


def t_screenshot(a):
    r = ok(request({'op': 'host.screenshot'}, timeout=10))
    png = bmp_to_png(r['file'], max(1, int(a.get('scale', 2))))
    return {'_image': base64.b64encode(png).decode('ascii'), 'file': r['file']}


def t_setup_save(a):
    r = ok(request({'op': 'setup_get', 'name': a.get('name') or os.path.splitext(os.path.basename(a['file']))[0]}, timeout=60))
    with open(a['file'], 'w', encoding='utf-8') as f:
        json.dump(r['setup'], f, ensure_ascii=False, indent=2)
    return {'ok': True, 'file': os.path.abspath(a['file'])}


def t_setup_load(a):
    with open(a['file'], encoding='utf-8') as f:
        setup = json.load(f)
    return ok(request({'op': 'setup_set', 'setup': setup}, timeout=120))


def t_phrase(a):
    req = {'op': 'phrase'}
    if a.get('cmd') and a['cmd'] != 'status':
        req['cmd'] = a['cmd']
    for k in ('bars', 'grid', 'file'):
        if k in a:
            req[k] = a[k]
    return ok(request(req))


def schema(props=None, required=()):
    return {'type': 'object', 'properties': props or {}, 'required': list(required)}


PART = {'type': 'integer', 'description': 'part 0..15 (multi mode; default the edit part)'}

TOOLS = [
    ('hsynth_status', 'Status of HSynth: preset / song, voices, DSP load, mode, MIDI ports.', schema(), t_status),
    ('hsynth_params', 'List parameters (name, kind, range, options). Filter with a name prefix such as '
                      '"filter." or "layer1.fm.". There are about 1400.',
     schema({'prefix': {'type': 'string'}}), t_params),
    ('hsynth_get', 'Read parameter values by name (or every parameter under a prefix such as "amp.").',
     schema({'names': {'type': 'array', 'items': {'type': 'string'}}, 'prefix': {'type': 'string'}, 'part': PART}),
     t_get),
    ('hsynth_set', 'Set parameters: values = {"filter.cutoff": 1200, "filter.type": "LP24"}. Enum values by name; '
                   'relative=true adds to the current values.',
     schema({'values': {'type': 'object'}, 'relative': {'type': 'boolean'}, 'part': PART}, ['values']), t_set),
    ('hsynth_presets', 'Search the preset library (factory + user). query matches the name.',
     schema({'query': {'type': 'string'}, 'category': {'type': 'string'}, 'limit': {'type': 'integer'}}), t_presets),
    ('hsynth_load_preset', 'Load a preset by name (into a part in multi mode).',
     schema({'name': {'type': 'string'}, 'part': PART}, ['name']), t_load),
    ('hsynth_play_notes', 'Play notes and wait until they end. notes = [{"key":60,"vel":0.8,"start":0,"dur":0.5,'
                          '"channel":1}] (key = MIDI note, C4 = 60; times in seconds; channel 1..16 for multi mode).',
     schema({'notes': {'type': 'array', 'items': {'type': 'object'}}}, ['notes']), t_play),
    ('hsynth_panic', 'All notes off.', schema(), t_panic),
    ('hsynth_mix', 'Read (no values) or set the mix of a part: volume, pan, mute, channel, key range, sends.',
     schema({'part': PART, 'values': {'type': 'object'}}), t_mix),
    ('hsynth_mode', 'Single sound (multi=false) or 16-part multi mode (multi=true).',
     schema({'multi': {'type': 'boolean'}}, ['multi']), t_mode),
    ('hsynth_select_part', 'Select the part that parameter edits address (multi mode).',
     schema({'part': {'type': 'integer'}}, ['part']), t_select),
    ('hsynth_player', 'MIDI file player of the app: cmd = list | play (index?) | toggle | pause | stop | next | prev | '
                      'repeat (repeat = off/all/one). position (s) seeks. Replies the songs and the state.',
     schema({'cmd': {'type': 'string'}, 'index': {'type': 'integer'}, 'repeat': {'type': 'string'},
             'position': {'type': 'number'}}), t_player),
    ('hsynth_live', 'Set lists (Live tab): cmd = list | recall (index) | next | prev | add_current; list = set list name.',
     schema({'cmd': {'type': 'string'}, 'index': {'type': 'integer'}, 'list': {'type': 'string'}}), t_live),
    ('hsynth_phrase', 'Phrase recorder: cmd = status | record | play | stop | undo | clear | quantize (grid in beats) | '
                      'export (file); bars = 0 (free) / 1 / 2 / 4 / 8.',
     schema({'cmd': {'type': 'string'}, 'bars': {'type': 'integer'}, 'grid': {'type': 'number'},
             'file': {'type': 'string'}}), t_phrase),
    ('hsynth_tab', 'Show a tab of the window: Play Edit Browser Multi Seq Perform Live Player Sample Library Monitor.',
     schema({'tab': {'type': 'string'}}, ['tab']), t_tab),
    ('hsynth_screenshot', 'A picture of the HSynth window (scale 2 = half size, 1 = full).',
     schema({'scale': {'type': 'integer'}}), t_screenshot),
    ('hsynth_log', 'The last lines of the app\'s message log (loading, warnings, errors).',
     schema({'lines': {'type': 'integer'}}), t_log),
    ('hsynth_setup_save', 'Save the whole setup (parts, effects, MIDI learn, settings) as an hsynth-setup JSON file '
                          'that the Windows, Mac, iPhone and Android apps all read.',
     schema({'file': {'type': 'string'}, 'name': {'type': 'string'}}, ['file']), t_setup_save),
    ('hsynth_setup_load', 'Load an hsynth-setup JSON file (from any of the apps).',
     schema({'file': {'type': 'string'}}, ['file']), t_setup_load),
    ('hsynth_request', 'Any request of the remote protocol (docs/SYSEX.md), e.g. {"op":"looper","cmd":"tap"} or '
                       '{"op":"song_load","file":"x.mid"}. Use when no other tool fits.',
     schema({'request': {'type': 'object'}, 'timeout': {'type': 'number'}}, ['request']), t_request),
]
BY_NAME = {t[0]: t for t in TOOLS}


# ---------------------------------------------------------------- JSON-RPC over stdio

def send(msg):
    sys.stdout.write(json.dumps(msg, ensure_ascii=False) + '\n')
    sys.stdout.flush()


def handle(msg):
    method = msg.get('method')
    mid = msg.get('id')
    if mid is None:
        return  # notifications (initialized, cancelled)
    if method == 'initialize':
        want = (msg.get('params') or {}).get('protocolVersion') or '2025-06-18'
        send({'jsonrpc': '2.0', 'id': mid, 'result': {
            'protocolVersion': want,
            'capabilities': {'tools': {}},
            'serverInfo': SERVER,
            'instructions': 'HSynth is a hybrid synthesizer running on this PC. Start the desktop app first '
                            '(desktop/build/hsynth_desktop.exe); it answers within a few seconds. Use '
                            'hsynth_params / hsynth_get before setting values, and hsynth_screenshot to see the UI.'}})
    elif method == 'ping':
        send({'jsonrpc': '2.0', 'id': mid, 'result': {}})
    elif method == 'tools/list':
        send({'jsonrpc': '2.0', 'id': mid, 'result': {'tools': [
            {'name': n, 'description': d, 'inputSchema': s} for n, d, s, _ in TOOLS]}})
    elif method == 'tools/call':
        p = msg.get('params') or {}
        t = BY_NAME.get(p.get('name'))
        if not t:
            send({'jsonrpc': '2.0', 'id': mid, 'error': {'code': -32602, 'message': f'unknown tool {p.get("name")}'}})
            return
        try:
            r = t[3](p.get('arguments') or {})
            content = []
            if isinstance(r, dict) and '_image' in r:
                content.append({'type': 'image', 'data': r.pop('_image'), 'mimeType': 'image/png'})
            content.insert(0, {'type': 'text', 'text': json.dumps(r, ensure_ascii=False, indent=1)})
            send({'jsonrpc': '2.0', 'id': mid, 'result': {'content': content, 'isError': False}})
        except Exception as e:
            send({'jsonrpc': '2.0', 'id': mid, 'result': {
                'content': [{'type': 'text', 'text': f'error: {e}'}], 'isError': True}})
    else:
        send({'jsonrpc': '2.0', 'id': mid, 'error': {'code': -32601, 'message': f'method not found: {method}'}})


def main():
    if hasattr(sys.stdout, 'reconfigure'):
        sys.stdout.reconfigure(encoding='utf-8', newline='\n')
        sys.stdin.reconfigure(encoding='utf-8')
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            send({'jsonrpc': '2.0', 'id': None, 'error': {'code': -32700, 'message': 'parse error'}})
            continue
        try:
            handle(msg)
        except Exception as e:  # never die on one bad request
            if msg.get('id') is not None:
                send({'jsonrpc': '2.0', 'id': msg['id'], 'error': {'code': -32603, 'message': str(e)}})
    return 0


if __name__ == '__main__':
    sys.exit(main())
