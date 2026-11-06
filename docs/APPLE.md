# macOS / iOS

HSynth は macOS（デスクトップ版。Windows 版と同じ UI）と iOS（SwiftUI のアプリ）でも動きます。
どちらも Android・Windows と同じ C++ のエンジンを使います。

## 構成

| パス | 内容 |
|---|---|
| `engine/` | DSP エンジン（C++17）。macOS でも CMake でビルドとテストができる。 |
| `desktop/src/main_macos.mm` | macOS の起動部分: AppKit のウィンドウ、Metal、Dear ImGui（OSX + Metal バックエンド）。 |
| `desktop/src/platform_macos.mm` | ファイルを開く / 保存するダイアログ（NSOpenPanel / NSSavePanel）、Finder で表示。 |
| `desktop/src/midi_in_coremidi.cpp`、`midi_out_coremidi.cpp` | CoreMIDI の入力 / 出力（iOS でも使う）。 |
| `desktop/src/audio_out.cpp` | miniaudio による音声（macOS は CoreAudio、iOS は AVAudioSession / RemoteIO）。 |
| `ios/project.yml` | iOS アプリの XcodeGen 設定（`.xcodeproj` は生成物なのでコミットしない）。 |
| `ios/HSynth/Bridge/HSynthEngine.{h,mm}` | Objective-C++ の窓口: Swift から使うエンジン・音声・MIDI・プリセット・パラメーター。 |
| `ios/HSynth/*.swift` | SwiftUI のアプリ: Play（プリセット、Sound のノブ、鍵盤、ホイール）、Edit（全パラメーターをページ別に）、Library（検索、カテゴリ、保存）。 |
| `.github/workflows/apple.yml` | macOS のランナーでの CI: エンジンのビルドとテスト、macOS 版、iOS アプリ（シミュレーター向け、署名なし）。 |

## Mac でのビルド

必要なもの: Xcode 15 以降（コマンドラインツール）、CMake 3.18 以降、Homebrew。

```bash
# エンジンとテスト
cmake -S engine -B build/engine -DCMAKE_BUILD_TYPE=Release
cmake --build build/engine -j
./build/engine/hsynth_tests

# macOS 版（build/desktop/HSynth.app ができる）
cmake -S desktop -B build/desktop -DCMAKE_BUILD_TYPE=Release
cmake --build build/desktop -j
open build/desktop/HSynth.app

# iOS アプリ
brew install xcodegen
cd ios && xcodegen && open HSynth.xcodeproj
```

iOS アプリを実機に入れるには、Xcode で一度だけ開発チーム（Signing & Capabilities → Team）を選びます。
バンドル ID は `com.hsynth.ios`。手順の詳細は `docs/HANDOFF_MAC.md`。

macOS 版は、ユーザーデータを `desktop/data`、内蔵コンテンツを `android/app/src/main/assets` から読みます
（`--data` / `--factory` で変更可。`--autotest DIR [--mute]` で Windows 版と同じ自動テスト）。

## 同梱してよいもの

サンプリングしたテスト用音源（GM テスト音源、SonicCell の録音）と MIDI ファイルはテスト専用で、
コミットもアプリへの同梱もしません。iOS アプリに入れるのは `android/app/src/main/assets/presets` と
`library/factory` だけです。

## 状況（2026-11、Windows のセッションから引き継ぎ）

- Mac なしで Windows 上で書いたもの。GitHub Actions の macOS ジョブが最初のコンパイル確認で、3 つとも成功している
  （エンジンのビルドとテスト、macOS 版、iOS のシミュレーター向けビルド）。
- macOS 版: Windows 版と同じ `App` のコードなので、機能は同等のはず。まだ Mac で動かしていない。確認すること:
  音声デバイスの選択、CoreMIDI（起動時と「再スキャン」でポートを開く）、Retina での文字の大きさ、
  WAV / SFZ / プリセットのドラッグ & ドロップ、`--autotest` のスクリーンショット。
- iOS: 初版。Android と比べて足りないもの: Multi 画面、Player（MIDI ファイル）、グラフィックエディター
  （エンベロープ、フィルター、FM アルゴリズム、ステップのマス目、MSEG）、MIDI ラーン / 出力、アンドゥ / コンペア、
  録音、サンプルの読み込み（ファイル App）、全画面の実機シンセ風ノブ。移植の手本は Android の Kotlin の UI
  （`android/app/src/main/java/com/hsynth/app/`）。
- iOS の音声: miniaudio で 5 ms のバッファを要求している。実機で遅延と中断（電話、出力先の切り替え、他アプリ）を確認する。
  次の段階として AUv3（GarageBand / AUM で使える Audio Unit 拡張）が自然。
