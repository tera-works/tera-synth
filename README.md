# HSynth — Android 向けハイブリッドシンセサイザー

Galaxy S23（Snapdragon 8 Gen 2）をスタンドアロン音源として使うための、VA / FM / PCM(SFZ) ハイブリッドシンセサイザーです。
DSP エンジンは Android に依存しない C++17 ライブラリで、将来 Raspberry Pi などの Linux SBC へ移植できる構造になっています。

- `engine/` — C++ DSP エンジン（CMake 単体ビルド可、外部依存なし）、単体テスト、オフラインツール
- `desktop/` — **Windows ネイティブ版**（Dear ImGui + WASAPI）。開発・テストはまずこちらで行います → [docs/WINDOWS.md](docs/WINDOWS.md)。テスト用 PCM 音源を作る自動サンプラー `hsynth_sampler` も含みます → [docs/SAMPLER.md](docs/SAMPLER.md)
- `android/` — Android Studio プロジェクト（Kotlin UI、Oboe、JNI）
- `dist/` — ビルド済み APK
- `docs/` — [アーキテクチャ](docs/ARCHITECTURE.md) / [ビルド・インストール手順](docs/BUILD.md) /
  [実装済み・未実装機能](docs/FEATURES.md) / [テスト結果と既知の問題](docs/TEST_REPORT.md) / [依存ライセンス](docs/THIRD_PARTY.md)

## エフェクト

31 種類のエフェクトを 12 スロットに自由な順番で並べるエフェクトラック（ダイナミクス、歪み・アンプ・キャビネット＋
ユーザー IR、EQ、フィルター、モジュレーション、ピッチ、ディレイ、アルゴリズム/IR リバーブ、スライサー、ボコーダー、
ルーパー、FX モジュレーション）。詳細と参照機との比較は [docs/EFFECTS.md](docs/EFFECTS.md)。

## 画面

| タブ | 内容 |
|---|---|
| Play | マルチタッチ鍵盤（指ごとに Timbre / Pressure / Pitch）、Bend / Mod ストリップ、プリセット切替、音量、Hold、発音数・DSP負荷 |
| Edit | Layers / VA / FM / PCM / Filter / Env / LFO / FX / Mod の全パラメータ（エンジンのメタデータから自動生成） |
| Library | プリセット保存・読込・インポート/エクスポート、WAV / SFZ フォルダーの取り込み（SAF）、メッセージログ |
| Monitor | オーディオストリーム情報、コールバック時間、XRun、遅延、ポリフォニー / 品質 / バッファ / 出力デバイス / MPE 設定、MIDI 機器 |

## クイックスタート（Windows で試す）

```bash
cd desktop && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/hsynth_desktop.exe
```

## Android（Galaxy S23）向けビルド

```powershell
# APK ビルド
cd android; $env:JAVA_HOME="C:\Program Files\Android\Android Studio\jbr"; .\gradlew.bat assembleRelease
# 端末へインストール（USB デバッグ有効）
C:\Android\Sdk\platform-tools\adb.exe install -r app\build\outputs\apk\release\app-release.apk
```

エンジン単体のテスト:

```bash
cd engine && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ./build/hsynth_tests
```
