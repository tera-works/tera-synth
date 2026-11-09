# HSynth — ハイブリッドシンセサイザー

VA / FM / PCM（SFZ・WAV・FLAC）/ ウェーブテーブル / グラニュラーを 1 つの音色の中で重ねられる、
16 パートのマルチティンバー・シンセサイザーです。外部 MIDI キーボードをつないでスタンドアロン音源として使うことを
目的にしています。DSP エンジンはプラットフォームに依存しない C++17 ライブラリで、同じエンジンを Windows・macOS・
iOS・Android の各アプリが使います。

## 対応プラットフォーム

| プラットフォーム | 状態 | 場所 | 資料 |
|---|---|---|---|
| Windows デスクトップ | 開発・テストの主な環境（Dear ImGui + WASAPI） | `desktop/` | [WINDOWS.md](docs/WINDOWS.md) |
| macOS デスクトップ | CoreAudio / CoreMIDI 版。ビルドは GitHub Actions で確認 | `desktop/` | [APPLE.md](docs/APPLE.md) |
| iOS（iPhone / iPad） | SwiftUI アプリ。AltStore で配布中（0.8.4） | `ios/` | [ALTSTORE.md](docs/ALTSTORE.md) |
| Android（Galaxy S23） | Kotlin + Oboe。最終段階で移植・ビルド | `android/` | [BUILD.md](docs/BUILD.md) |

## 主な機能

- **音源方式**: 1 つの音色に複数のレイヤーを重ねられます。
  - VA（polyBLEP の 2 オシレーター）
  - FM（8 オペレーター、88 アルゴリズム）
  - PCM（SFZ / WAV / FLAC、ストリーミング再生）
  - ウェーブテーブル
  - グラニュラー
- **音作り**
  - フィルター（SVF / ラダーほか）、エンベロープ、LFO
  - モジュレーションマトリクス（16 スロット、ソース 25 / 行き先 55、ノートごと）
  - MPE
- **エフェクト**: 43 ブロック（サブタイプ込みで約 110 種）を 12 スロットに自由な順番で並べるラック。
  ほかにパートごとのインサートと、リバーブ / ディレイのセンドがあります。→ [EFFECTS.md](docs/EFFECTS.md)
- **マルチ**: 16 パートに MIDI チャンネルを割り当てます。MIDI ファイルのプレーヤーもあります。
- **MIDI 入力**
  - USB MIDI と BLE MIDI（iOS は標準の接続画面、Android は内蔵のスキャン）→ [BLE_MIDI.md](docs/BLE_MIDI.md)
  - Apple 版は MIDI 2.0（UMP）の受信も選べます。
  - 機器の抜き差しは自動で検出します。
- **標準コントローラー**: CC1 / 5 / 11 / 71 / 74 / 91 / 94 は、どの音色でも効きます。
  MIDI ラーンでパートごとに別のパラメータへ割り当て直せます。→ [CONTROLLERS.md](docs/CONTROLLERS.md)
- **セットアップファイル**: 1 つの JSON ファイルに、次をまとめて保存・読み込みできます。
  - マルチ（各チャンネルの音色、音量、エフェクト）
  - MIDI ラーンの割り当て
  - 発音数などの設定
- **リモート操作**: PC から SysEx で音色やセットアップを読み書きします（`desktop/tools/hsynth_remote.py`）。
  → [SYSEX.md](docs/SYSEX.md)
- **自動サンプラー**: ハードウェア音源を録音して SFZ + FLAC の音色を作ります（`hsynth_sampler`）。
  → [SAMPLER.md](docs/SAMPLER.md)

実装済み・未実装の一覧は [FEATURES.md](docs/FEATURES.md) にあります。

## リポジトリの構成

| フォルダー | 内容 |
|---|---|
| `engine/` | C++17 の DSP エンジン（外部依存なし、CMake 単体ビルド可）、単体テスト、オフラインツール |
| `desktop/` | Windows / macOS 版アプリと PC 用ツール（`tools/`: リモート操作、サンプラー補助、セットアップ作成） |
| `ios/` | iOS アプリ（XcodeGen の `project.yml`、SwiftUI、Objective-C++ のブリッジ） |
| `android/` | Android アプリ（Kotlin UI、Oboe、JNI） |
| `ios-distribution/` | iOS の AltStore 配布用スクリプト |
| `docs/` | 資料（日本語） |
| `third_party/` | 同梱ライブラリ → [THIRD_PARTY.md](docs/THIRD_PARTY.md) |

設計は [ARCHITECTURE.md](docs/ARCHITECTURE.md)、テスト結果と既知の問題は [TEST_REPORT.md](docs/TEST_REPORT.md) を見てください。

## ビルド

エンジンとテスト（CMake と C++17 コンパイラがあればどの OS でも可）:

```bash
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build -j
./engine/build/hsynth_tests
```

Windows 版（WinLibs MinGW GCC + Ninja）:

```bash
cmake -S desktop -B desktop/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build desktop/build
desktop/build/hsynth_desktop.exe
```

macOS 版と iOS アプリは [APPLE.md](docs/APPLE.md)、Android は [BUILD.md](docs/BUILD.md) を見てください。

## iOS アプリを入れる

AltStore Classic に次の Source を追加すると、HSynth をインストールできます。

```
https://tera-works.github.io/tera-synth-ios/source.json
```

公開リポジトリ `tera-works/tera-synth-ios` でビルドしています。新しい版の出し方は [ALTSTORE.md](docs/ALTSTORE.md) にあります。

## 音源と MIDI ファイルについて

テスト用にサンプリングした音源（GM / SonicCell）と MIDI ファイルは配布しません。
このリポジトリとアプリには含めず、非公開の場所にだけ置いています。
アプリに入っているのは、内蔵プリセットとデモ用サンプルだけです。
