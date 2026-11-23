# 依存関係とライセンス

## アプリ・エンジン依存ライブラリ

| コンポーネント | 用途 | ライセンス | 取得元 |
|---|---|---|---|
| Oboe 1.10.0 | Android 低遅延オーディオ出力（AAudio / OpenSL ES） | Apache License 2.0 | Maven `com.google.oboe:oboe`（prefab） |
| ONNX Runtime Mobile (`onnxruntime-android` 1.20.0) | Android NNAPI / CPU 4-Stem 分離推論 | MIT | Maven `com.microsoft.onnxruntime:onnxruntime-android` |
| Dear ImGui 1.91.8 | Windows/デスクトップ版 UI（`third_party/imgui-1.91.8`） | MIT | github.com/ocornut/imgui |
| miniaudio 0.11.22 | デスクトップ版オーディオ出力（WASAPI / ALSA / CoreAudio） | Public domain または MIT-0 | github.com/mackron/miniaudio |
| TinySoundFont 0.9 | サンプラー（`hsynth_sampler`）の SF2 レンダリング | MIT | github.com/schellingb/TinySoundFont |
| Kotlin 標準ライブラリ | Android UI | Apache License 2.0 | Android Gradle Plugin 内蔵 Kotlin |
| LLVM libc++ (`libc++_shared.so`) | NDK C++ 標準ライブラリ | Apache 2.0 with LLVM Exception | Android NDK |
| Android SDK / NDK | ビルド環境 | Android SDK License | Google |

DSP エンジン（`engine/`）は外部ライブラリに依存しません（JSON / WAV / FLAC / SFZ / SF2 / SF3 / DLS / GIG / Decent Sampler / DX7 SysEx / Surge `.wt` / Vital `.vitaltable` パーサーおよび NNAPI 動的ロードも自作です）。AndroidX / Jetpack ライブラリも使用していません。

## オンデマンド取得モデル・外部音源カタログ（アプリ非同梱）

以下は APK / リポジトリには同梱せず、ユーザー操作時に公式配布先から直接ダウンロードされます。アプリ内の `LICENSES` 画面（`LIB > ⬇ パック > LICENSES` / `SYS > SYSTEM > SOUND LICENSES`）でもクレジットおよびライセンスリンクを確認できます。

### AI モデル（任意ダウンロード）
- **Spleeter 4-Stems (FP16 ONNX)** — Deezer Research（MIT License）。`SONG > AUDIO > ✦ ステム分離 (NPU)` からユーザー操作時のみ取得。

### 組み込みカタログ（`android/app/src/main/assets/catalog/sound_packs.json`）
| 音源パック | 著作者 / 配布元 | ライセンス |
|---|---|---|
| MuseScore General (SF3 Compact / SF2 HQ) | S. Christian Collins / MuseScore | MIT |
| Salamander Grand Piano (Yamaha C5) | Alexander Holm | CC BY 3.0 |
| Meta Open Sound (Upright Piano / Nylon Guitar / Steel Guitar) | Meta Creative Audio | CC0 1.0 |
| Black And Blue Basses / Black And Green Guitars / Big Rusty Drums | Karoryfer Samples | CC0 1.0 |
| Virtuosity Drums | Versilian Studios / sfzinstruments | CC0 1.0 |
| VSCO 2 Community Edition (v1.1.0 SFZ) | Versilian Studios | CC0 1.0 |
| VCSL (Versilian Community Sample Library v1.2.2-RC SFZ) | Versilian Studios Community | CC0 1.0 |
| Mecate (1970s String Machine, Web リンク案内のみ) | Anode Labs | CC BY 4.0 |
| AKWF-FREE (Adventure Kid Waveforms) | Kristoffer Ekstrand | CC0 1.0 |
| WaveEdit CC0 Wavetable Banks | Synthesis Technology / Community | CC0 1.0 |

## デモ音源（リポジトリ同梱）

`android/app/src/main/assets/library/factory/` のオーディオはすべて `engine/tools/hsynth_assets.cpp` で数式から生成した自作波形です（加算合成ピアノ風音色、ループパッド、合成ドラム、テスト波形）。第三者の録音・商用音源は含みません。CC0 として扱えます。

## テスト用サンプル（非配布・`.gitignore` 済み）

`desktop/data/` や `testdata/` 配下の検証用サンプルはリポジトリ・APK には含めません（`.gitignore` 済み）。