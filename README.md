# HSynth — モバイル＆デスクトップ向けフラッグシップ・ハイブリッドシンセサイザー

Android スマートフォン（Google Pixel 7 / Tensor G2 等）や Windows / macOS デスクトップを、**超低遅延のスタンドアロン音源モジュール＆ワークステーション**として動かすための C++17 ハイブリッドシンセサイザーです。

DSP エンジン（`engine/`）は OS 非依存の純粋 C++17 で書かれており、オーディオコールバック内でのヒープ確保ゼロ（**229 / 229 C++ テスト全件通過**）を実現しています。

---

## 📘 ドキュメント（PDF）

| ドキュメント | 内容 |
|---|---|
| **[取扱説明書（PDF）](docs/HSynth_取扱説明書.pdf)** | 全 7 画面（STAGE / MULTI / EDIT / LIB / PLAY / SONG / SYS）の操作、Edge TPU AI 2D Morph、4-Stem 分離、全 7 音源フォーマット取り込み、Sound Packs ダウンローダー（BATCH キュー）、専用モジュール／Root セットアップ要約（実機 Pixel 7 スクリーンショット付き） |
| **[性能詳細レポート（PDF）](docs/HSynth_性能詳細レポート.pdf)** | 229 / 229 テスト、実機ベンチマーク（遅延 3.9–4.8 ms・Edge TPU 591 µs・4-Stem 1.19 s・737 mW）、エンジン／FX／NPU アーキテクチャ、フラッグシップ機との機能比較 |

## 主な機能

### 1. マルチエンジン音源 & 45 種エフェクトラック
- **音源レイヤー（最大 3 レイヤー / 16 パートマルチ対応）**:
  - **VA**（PolyBLEP アナログモデリング）、**6-op FM**（DX7 互換アルゴリズム含む）、**PCM マルチサンプル**、**Wavetable**（2.5D リボンビジュアライザー搭載）、**Granular**、**Piano**（共鳴・ハンマー・ダンパーモデリング）、**Physical Modeling**（Karplus-Strong / Bowed / Blown）
- **45 種エフェクトラック（最大 12 スロット自由配列）** + **マスター 5 バンドパラメトリック EQ & コンプレッサー**
- **ライブパフォーマンス**: Stage / Live Sets（64 スロット + シームレス・サウンド・トランジション SST）、Super Knob、16 パートミキサー、アルペジエーター、フレーズシーケンサー、リズムマシン、チューナー、リアルタイムオシロスコープ＆スペアナ、MIDI ラーン、ペダル／フットスイッチ設定

### 2. Tensor G2 Edge TPU / NPU 機能（Android）
- **AI 2D パッチモーフィング（`STAGE > AI 2D`）**: Android NNAPI 経由で Pixel 7 の **`google-edgetpu`（uint8）** を直接駆動し、最大 16 アンカーの 2D 音色モーフィングを **0.59 ms（591 µs）** で実時間推論（NNAPI FP32 / CPU 自動フォールバック付き）。
- **AI 4-Stem 分離＆マイナスワン（`SONG > AUDIO > ✦ ステム分離 (NPU)`）**: ONNX Runtime Mobile + NNAPI（Spleeter 4-Stems FP16 オンデマンド取得）およびオフライン高速スペクトル分離（3 分曲を約 1.2 秒で分離）に対応。`VOCALS / DRUMS / BASS / KEYS` の 4 ステム同期ミキサーとワンタップ `MINUS-ONE / KARAOKE / DRUMS+BASS` を搭載。

### 3. マルチフォーマット対応（全 7 形式＋SFZ/WAV/FLAC）
- **SoundFont**: `.sf2` / `.sf3`（Ogg Vorbis 展開対応）
- **DLS / GigaStudio**: `.dls`（DLS1 / DLS2）/ `.gig`（v2 / v3・`.gxNN` 分割対応）
- **Decent Sampler**: `.dspreset` / `.dslibrary`（zip-slip 防御展開）
- **Yamaha DX7 SysEx**: `.syx`（32 音色 VMEM / 1 音色 VCED ブラウザ＆一括プリセット変換）
- **Wavetable**: `.wt`（Surge）/ `.vitaltable`（Vital）/ `.wav`（Serum `clm ` チャンク対応）
- **SFZ / WAV / FLAC**: マルチサンプル＆ストリーミング再生、ライブサンプリングウィザード、オーディオバッキング（A-B ループ / 独立速度・ピッチ変更）、16 パッドサンプラー

### 4. アプリ内・オープン音源パックダウンローダー（`LIB > ⬇ パック`）
- 商用利用可能なオープンライセンス（MIT / CC BY / CC0）の **12 ライブラリ（15 パック）** をアプリ内から直接取得可能（MuseScore General SF2/SF3、Salamander Grand Piano、Meta Open Sound、Karoryfer Bass/Guitar/Drums、Virtuosity Drums、VSCO 2 CE、VCSL、AKWF-FREE、WaveEdit 等）。
- **選択順一括キューダウンロード（`BATCH` モード）**・ZIP64（4 GB 超）安全展開・SHA-256 検証・ライセンス＆クレジット一覧ビューアを搭載。
- `+ ADD JSON` / `TEMPLATE` 書き出しにより、ユーザー独自の外部 `.json` カタログを自由に追加可能。

### 5. 運用・接続機能
- **Wi-Fi Web エディタ**（LAN 内ブラウザから音色編集）、**一括 ZIP バックアップ＆復元**、USB MIDI / BLE MIDI / MPE / MIDI 2.0 UMP 対応。

---

## ディレクトリ構成

- `engine/` — C++17 DSP エンジン（外部依存ゼロ）・単体＆リアルタイム安全性テスト（229 件）
- `android/` — Android Kotlin アプリ（ゼロ XML Obsidian UI、Oboe、JNI、ONNX Runtime Mobile）
- `android/root-tuning/` — **Root（Magisk）専用音源モジュール化スクリプト群** → [docs/ANDROID_ROOT_SETUP.md](docs/ANDROID_ROOT_SETUP.md)
- `desktop/` — Windows ネイティブ版（Dear ImGui + WASAPI）・自動サンプラー `hsynth_sampler` → [docs/WINDOWS.md](docs/WINDOWS.md)
- `ios/` — iOS 版プロジェクト → [docs/APPLE.md](docs/APPLE.md)
- `docs/` — [取扱説明書 PDF](docs/HSynth_取扱説明書.pdf) / [性能詳細レポート PDF](docs/HSynth_性能詳細レポート.pdf) / [Root セットアップガイド](docs/ANDROID_ROOT_SETUP.md) / [アーキテクチャ](docs/ARCHITECTURE.md) / [ビルド手順](docs/BUILD.md) / [機能一覧](docs/FEATURES.md) / [エフェクト](docs/EFFECTS.md) / [依存関係・ライセンス](docs/THIRD_PARTY.md)

---

## クイックスタート

### 1. C++17 エンジン単体テスト（229 件）
```bash
cd engine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

### 2. Windows デスクトップ版ビルド
```bash
cd desktop
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hsynth_desktop.exe
```

### 3. Android APK ビルド＆インストール
```powershell
cd android
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
$env:ANDROID_HOME = "C:\Android\Sdk"
.\gradlew.bat assembleDebug

# 端末へインストール（USB デバッグ有効）
adb install -r app\build\outputs\apk\debug\app-debug.apk
```

### 4. Android Root（Magisk）専用音源モジュール化（任意）
Pixel 7 等で **1 ms（48f）AAudio exclusive / 3.9–4.8 ms 遅延 / BLE MIDI 7.5 ms / Direct ALSA USB-DAC / 段階省電力** を有効にする手順は **[docs/ANDROID_ROOT_SETUP.md](docs/ANDROID_ROOT_SETUP.md)** を参照してください。