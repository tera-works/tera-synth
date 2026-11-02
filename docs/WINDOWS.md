# Windows ネイティブ版（開発・テスト用）

エミュレーターを使わず、同じ C++ エンジンを Windows 上でネイティブに動かして開発・テストするためのホストです。
音作り・DSP・プリセット・SFZ 読込はここで確認し、最後に Android（Galaxy S23）でビルドして実機確認する流れを想定しています。

```
            ┌──────────── engine/（共通 C++17 DSP、プリセット、SFZ、MIDI 変換）────────────┐
 desktop/ ──┤ Windows: Dear ImGui UI + miniaudio(WASAPI) + winmm MIDI                        │
 android/ ──┤ Android: Kotlin UI + Oboe(AAudio) + android.media.midi                         │
            └── 共通のプリセット / サンプルライブラリ（android/app/src/main/assets を両方が使用）──┘
```

## ビルドと起動

```bash
cd desktop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hsynth_desktop.exe
```

- 必要なもの: CMake 3.18+、Ninja、GCC（MinGW-w64, WinLibs 16.2 で確認）。MSVC でもビルドできる構成です（未確認）。
- 実行ファイルは C++ ランタイムを静的リンクしているので、`hsynth_desktop.exe` 単体でコピーして動きます。
- データフォルダー: `desktop/data/`（ライブラリ、ユーザープリセット、`settings.json`）。起動時に
  `android/app/src/main/assets` からファクトリープリセットとデモ音源を同期します。`--data` / `--factory` で変更可能。

## 画面と操作

Android 版と同じ4画面（Play / Edit / Library / Monitor）です。

| 操作 | 内容 |
|---|---|
| PC キーボード | `A W S E D F T G Y H U J K O L P ;` がピアノ配列（A = C4 から）。`Z`/`X` オクターブ、`C`/`V` ベロシティ、`Space` サステイン |
| マウス鍵盤 | クリック位置の高さ = ベロシティ、ドラッグ上下 = 鍵盤ごとの Timbre。「Slide = per-note bend」をオンにすると左右ドラッグで per-note ピッチ |
| Bend / Mod | 左の縦スライダー（Bend は離すと中央に戻る） |
| Edit | 全パラメータ。右クリックで既定値、Ctrl+クリックで数値入力、指数カーブのパラメータは対数スライダー |
| Edit → FX | エフェクトチェーン（12 スロットの選択・▲▼で並べ替え・on/off）、テンポ・マクロ、各ブロックのパネル、Cabinet / IR Reverb の「Load IR (.wav)...」、ルーパー |
| Edit → FX Mod | モッドホイール・エクスプレッション・マクロ等 → 任意の FX パラメータ（6 スロット） |
| Library | プリセット保存/読込/インポート/エクスポート、WAV / SFZ フォルダー取り込み。**ファイルやフォルダーをウィンドウへドラッグ&ドロップ**でも取り込めます |
| Monitor | WASAPI デバイス選択、サンプルレート、バッファ（period）、排他モード、**音声入力（ボコーダー用マイク）**、DSP/コールバック負荷、遅延コールバック数、オシロスコープ、**出力の WAV 録音** |
| MIDI | 接続中の MIDI 入力ポートを全て開きます（winmm, MIDI 1.0 / MPE）。抜き差し後は「Rescan MIDI」 |

## 自動スモークテスト

```bash
./build/hsynth_desktop.exe --autotest C:/WorkSpace/Project/Synth/desktop/autotest_out
```

約16秒で、VA 和音 → Hybrid（PCM+FM、Mod ホイールでモーフ）→ ドラムキット（SFZ）→ Amp Stack Lead（FX 画面）→
Trance Gate（ルーパーで録音・再生）→ Shimmer Pad を実際のオーディオデバイスで鳴らし、
各画面のスクリーンショット（BMP）、出力録音 `autotest_output.wav`、`report.txt`（遅延コールバック数・負荷・ピーク・ログ）を書き出して終了します。
**既定の出力デバイスから音が出ます。**

2026-11-01 の結果（Realtek スピーカー、WASAPI 共有 48 kHz / 10 ms）: PASS、遅延コールバック 0、コールバック最大負荷 18 %。
2026-11-02（v0.2.0、エフェクト込み）: PASS、遅延コールバック 0、コールバック最大負荷 12〜24 %（3 回）。

## Windows 版と Android 版の違い

| | Windows | Android |
|---|---|---|
| 出力 | WASAPI 共有（10 ms 周期）/ 排他モード選択可 | AAudio LowLatency / Exclusive（Oboe） |
| 演奏入力 | マウス（単音）、PC キーボード、MIDI | マルチタッチ（指ごとの表現）、USB MIDI、MIDI 2.0 UMP |
| MIDI 2.0 UMP | 未対応（winmm は MIDI 1.0 のみ。Windows MIDI Services 対応は今後） | Android 13+ |
| ファイル選択 | Win32 ダイアログ、D&D | Storage Access Framework |
| 音声入力（ボコーダー） | 対応（WASAPI デュプレックス） | 未対応 |

DSP・プリセット・SFZ・MIDI 変換は同一コードなので、Windows で作ったプリセットは Android にそのまま持っていけます
（サンプルパスはライブラリ相対。`desktop/data/library/user/` の内容を端末のライブラリへインポートしてください）。

## Linux / Raspberry Pi への展開（予定）

`audio_out`（miniaudio: ALSA/PulseAudio）、`host_library`、`app`（ImGui）はそのまま使えます。
必要なのは GLFW/SDL ベースの `main` と ALSA MIDI 入力（`midi_in_alsa.cpp`）、`platform_linux.cpp` だけです。
