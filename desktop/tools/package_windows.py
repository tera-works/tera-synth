#!/usr/bin/env python3
"""Windows packages of HSynth: a portable zip and an installer (Inno Setup).

  python desktop/tools/package_windows.py            (version from ios/project.yml)
  python desktop/tools/package_windows.py --no-build --version 0.8.7

Builds desktop/build/hsynth_desktop.exe (Release, static), then writes into dist/:
  HSynth-<v>-windows/           the portable folder (HSynth.bat starts it; data in .\\data)
  HSynth-<v>-windows.zip
  HSynth-<v>-setup.exe          per-user installer (Start menu, optional desktop icon,
                                data in Documents\\HSynth, uninstall from Windows Settings)
Only the factory content goes in (built-in presets, demo samples, fonts): never the
recorded test sounds (desktop/data) or MIDI files.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
FACTORY = os.path.join(ROOT, 'android', 'app', 'src', 'main', 'assets')
EXE = os.path.join(ROOT, 'desktop', 'build', 'hsynth_desktop.exe')
DIST = os.path.join(ROOT, 'dist')

README = """HSynth {v} (Windows)

起動
  ポータブル版: HSynth.bat をダブルクリック。設定・プリセット・取り込んだ音源は同じフォルダーの data に保存されます。
  インストール版: スタートメニューの「HSynth」。データは ドキュメント\\HSynth に保存されます（アンインストールしても消えません）。

画面
  上の液晶: プリセット名（クリックで検索して選ぶ）。ノブを回している間はその値。
  LED 付きのボタンで画面を切り替えます: Play / Edit / Browser / Multi / Seq / Perform / Live / Player / Sample / Library / Monitor。
  ノブ: 上下にドラッグ（Shift で細かく）、ダブルクリックで初期値、Ctrl+クリックで数値入力、右クリックで MIDI Learn。

演奏
  MIDI キーボードをつなぐと、そのまま弾けます（抜き差ししたら Monitor 画面の「Rescan MIDI」）。
  PC のキーボード: A W S E D F T G Y H U J K O L P ; が鍵盤、Z / X でオクターブ、Space でサステイン。

曲とセットアップ
  MIDI ファイルは Player 画面の「Open Songs...」で追加します（data\\midi にコピーされます）。
  セットアップファイル（パート・エフェクト・MIDI Learn・設定）は Monitor 画面で書き出し・読み込み。iPhone / Android 版と共通です。

AI・コマンドからの操作
  起動中は 127.0.0.1:47800 で操作を受け付けます（Monitor 画面でオン / オフ）。
  python hsynth_remote.py --tcp status、MCP サーバー hsynth_mcp.py（ソースの desktop/tools）。

factory には内蔵プリセット、デモ用サンプル、フォント（Rajdhani / Share Tech Mono、SIL OFL）が入っています。
"""

BAT = ('@echo off\r\n'
       'rem HSynth {v} for Windows. User data is kept in the "data" folder next to this file.\r\n'
       'start "" "%~dp0hsynth_desktop.exe" --data "%~dp0data" --factory "%~dp0factory"\r\n')


def version_from_project():
    text = open(os.path.join(ROOT, 'ios', 'project.yml'), encoding='utf-8').read()
    m = re.search(r'CFBundleShortVersionString:\s*"([^"]+)"', text)
    if not m:
        raise SystemExit('version not found in ios/project.yml')
    return m.group(1)


def find_iscc():
    cands = [shutil.which('iscc'), shutil.which('ISCC'),
             os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Programs', 'Inno Setup 6', 'ISCC.exe'),
             r'C:\Program Files (x86)\Inno Setup 6\ISCC.exe', r'C:\Program Files\Inno Setup 6\ISCC.exe']
    for c in cands:
        if c and os.path.isfile(c):
            return c
    return None


def check_factory(folder):
    """Refuse anything that is not factory content (recorded sounds, MIDI files)."""
    for d, _, files in os.walk(folder):
        for f in files:
            low = f.lower()
            if low.endswith(('.mid', '.midi')) or 'soniccell' in low or low.startswith(('sc ', 'gm ')):
                raise SystemExit(f'refusing to package {os.path.join(d, f)} (test material is never distributed)')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--version')
    ap.add_argument('--no-build', action='store_true', help='package the exe as it is')
    ap.add_argument('--no-installer', action='store_true')
    a = ap.parse_args()
    v = a.version or version_from_project()

    if not a.no_build:
        subprocess.run(['cmake', '--build', os.path.join(ROOT, 'desktop', 'build')], check=True)
    if not os.path.isfile(EXE):
        raise SystemExit(f'{EXE} not found: build the desktop app first')

    # portable folder
    stage = os.path.join(DIST, f'HSynth-{v}-windows')
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    shutil.copy2(EXE, stage)
    shutil.copytree(FACTORY, os.path.join(stage, 'factory'))
    check_factory(os.path.join(stage, 'factory'))
    with open(os.path.join(stage, 'HSynth.bat'), 'w', encoding='ascii', newline='') as f:
        f.write(BAT.format(v=v))
    with open(os.path.join(stage, 'README.txt'), 'w', encoding='utf-8-sig', newline='\r\n') as f:
        f.write(README.format(v=v))

    # zip
    zpath = stage + '.zip'
    if os.path.exists(zpath):
        os.remove(zpath)
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for d, _, files in os.walk(stage):
            for f in files:
                full = os.path.join(d, f)
                z.write(full, os.path.join(os.path.basename(stage), os.path.relpath(full, stage)))
    print(f'zip: {zpath} ({os.path.getsize(zpath) / 1e6:.1f} MB)')

    # installer
    if a.no_installer:
        return 0
    iscc = find_iscc()
    if not iscc:
        raise SystemExit('Inno Setup (ISCC.exe) not found: winget install JRSoftware.InnoSetup')
    iss = os.path.join(ROOT, 'desktop', 'installer', 'hsynth.iss')
    subprocess.run([iscc, '/Qp', f'/DAppVersion={v}', f'/DStage={stage}', f'/O{DIST}', iss], check=True)
    setup = os.path.join(DIST, f'HSynth-{v}-setup.exe')
    print(f'installer: {setup} ({os.path.getsize(setup) / 1e6:.1f} MB)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
