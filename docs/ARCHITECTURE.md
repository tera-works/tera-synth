# HSynth アーキテクチャ解説

## 全体構成

```
Synth/
├─ engine/                 C++17 DSPエンジン（プラットフォーム非依存・CMake単体ビルド可）
│  ├─ include/hsynth/      公開ヘッダ
│  │  ├─ engine.h          Engine: 音声スレッドAPI / スレッド安全な制御API
│  │  ├─ event.h           MIDI非依存の演奏イベント
│  │  ├─ params.h          パラメータID・メタデータ（名前・範囲・enum・グループ）
│  │  ├─ voice.h           ボイス（3レイヤー + フィルター + アンプ + 鍵盤ごとの変調）
│  │  ├─ sample_set.h      PCMサンプル・リージョン（キー/ベロシティ/ループ）
│  │  ├─ midi.h            MIDI 1.0 / MPE / MIDI 2.0 UMP → Event 変換
│  │  ├─ preset.h          JSONプリセット入出力
│  │  ├─ host_utils.h      プリセット適用・サンプル読込・オフラインレンダリング
│  │  ├─ json.h / wav.h    依存ライブラリなしのJSON / WAV入出力
│  │  ├─ dsp/              oscillators, fm, svf, envelope, lfo, effects（マスターリミッター）
│  │  └─ fx/               エフェクトラック: fx_params（レイアウト）, dsp（部品）, convolver（分割畳み込み）,
│  │                       block（共通IF）, rack（スロット・FX変調・ルーパー）, ir_load（ユーザーIR）
│  ├─ src/                 実装
│  ├─ tests/               単体テスト・ゴールデンファイル
│  └─ tools/               hsynth_render（オフラインレンダリング/比較）, hsynth_assets（デモ素材生成）, hsynth_bench
├─ android/                Android Studio プロジェクト（Kotlin UI + Oboe + JNI）
│  └─ app/src/main/
│     ├─ cpp/              audio_engine（Oboe）, jni_bridge（JNI）, CMakeLists（engine を add_subdirectory）
│     ├─ java/com/hsynth/app/  Kotlin UI
│     └─ assets/           デモプリセット・自作サンプル（hsynth_assets で生成）
└─ docs/
```

DSPエンジンはAndroid APIを一切使用しません（`engine/` 以下に `android` / `jni` / `oboe` の参照なし）。
Android固有のコードは `android/app/src/main/cpp/audio_engine.*`（Oboe出力）と `jni_bridge.cpp`、Kotlin UIのみです。
Raspberry Pi / Linux / Windows / macOS では、同じ `engine/` を CMake でビルドし、ALSA / JACK / CoreAudio / WASAPI 等の
オーディオ出力層（`AudioEngine` 相当）だけを書けば動作します。オフラインツールとテストは既にWindows(MinGW)でビルド・実行済みです。

## スレッドモデルとリアルタイム安全性

| スレッド | 役割 | エンジンとの通信 |
|---|---|---|
| オーディオ（Oboeコールバック） | `Engine::process()` のみ | — |
| UI / タッチ | ノート・per-note変調・パラメータ | `sendEvent()` → ロックフリーSPSCキュー（生産者側のみmutex） / `setParam()` → `std::atomic<float>` |
| MIDI（binderスレッド） | MIDI/UMPバイト列 | `MidiTranslator` → `sendEvent()` |
| ローダー（単一Executor） | WAV/SFZ/プリセット読込 | `setSampleSet()` → アトミックポインタ交換 |

オーディオコールバック内では **メモリ確保・ファイルI/O・ブロッキングロックを行いません**。

- パラメータ：UIは `std::atomic<float>` 配列に書き込み、バージョンカウンタを進める。音声スレッドはバージョン変化時のみ
  ローカル配列にコピー。
- サンプルセット：ローダースレッドでWAVを全てデコードしてから `SampleSet` を作成し、`pendingSets_[layer]` に
  アトミックに格納。音声スレッドはブロック先頭で取り出して差し替え、旧セットを使用中のボイスのPCM再生を停止した上で
  旧セットを「廃棄キュー」（SPSC）へ送る。解放は UI スレッドの `collectGarbage()`（250 ms毎）で行う。
- イベントキュー溢れはカウント（`droppedEvents`）され、Monitor画面に表示。
- デノーマル対策：`process()` 冒頭で FTZ/DAZ（ARM64: FPCR.FZ、x86: MXCSR）を有効化。

## 演奏イベント（MIDI非依存）

`hsynth::Event` は `NoteOn/NoteOff`（noteId付き）、per-note の `NotePitch / NotePressure / NoteTimbre / NoteX / NoteHold`、
チャンネル共通の `PitchBend / ModWheel / ChannelPressure / Sustain / Controller` 等で構成されます。
各ノートは一意の `noteId` を持ち、per-note イベントは noteId（0の場合はキー番号）で対象ボイスを特定します。
これにより **鍵盤ごとに独立した変調** が可能です。

入力層は `EventSink` に `Event` を流し込む「翻訳器」として実装します:

- `MidiTranslator::processMidi1()` — MIDI 1.0 バイト列（ランニングステータス、SysEx読み飛ばし、Poly AT→NotePressure）、
  MPEモード（ch1マスター / ch2-16メンバー：ピッチベンド→NotePitch、CP→NotePressure、CC74→NoteTimbre）
- `MidiTranslator::processUmp()/processUmpBytes()` — MIDI 2.0 UMP（MT2: MIDI1チャンネルボイス、MT4: MIDI2チャンネルボイス
  16bitベロシティ、Per-Note Pitch Bend、Registered/Assignable Per-Note Controller、Per-Note Management）
- タッチ鍵盤（Kotlin `KeyboardView`）はJNI経由で直接 `Event` を生成
- 将来のRP2350ショルダーキーボードの独自USBプロトコルも、同じく `EventSink` を出力する翻訳器を追加するだけで、
  DSPコードの変更は不要です。MIDI 2.0で送る場合は Assignable Per-Note Controller #1=NoteX、#2=NoteHold、#3=NoteTimbre を
  HSynth の規約として予約しています。

## 音源構造

```
Voice (最大64 + フェード用8)
 ├─ Layer A ┐  各レイヤー: Off / VA / FM / PCM / WT（ウェーブテーブル）
 ├─ Layer B ├─ level・pan・coarse/fine・velocity感度、Morph(A⇔B)
 ├─ Layer C ┘
 ├─ Stereo multimode filter (SVF: LP12/LP24/BP/HP/Notch, drive=非線形)
 ├─ Amp ADSR ・ Mod ADSR(Env2) ・ LFO1(ボイス単位、ディレイ付き)
 └─ per-note expression (pitch / pressure / timbre / X / hold)
Effect rack: 12 スロット（31 種、docs/EFFECTS.md）→ Looper → Volume → Peak Limiter
```

**ウェーブテーブル**（`dsp/wavetable.h`）: 1 フレーム 2048 サンプルの単一波形を並べたテーブルを位置（0〜1）で
走査します。各フレームは FFT で倍音数を 1024, 512, …, 1 に制限した 11 段のコピーを持ち、オシレーターは位相増分から
ナイキストを超えない段を選び、1 段鈍いコピーとクロスフェードします（ピッチを動かしても段差・折り返しなし）。
内蔵テーブルは初回の `Engine::prepare()` で生成し全ボイスで共有、ユーザー WAV は `SampleSet::wavetable` として
サンプルセットと同じ仕組み（非音声スレッドで構築 → ポインタ差し替え）で読み込みます。

- **VA**: polyBLEP（Saw/Square/Pulse）、polyBLAMP（Triangle）、Sine。2オシレーター + デチューン + ノイズ + PWM。
- **FM**: 4オペレーター（`P::kFmOps` 定数 + アルゴリズムのデータテーブルにより6/8オペレーターへ拡張可能）。
  8アルゴリズム、オペレーターごとの比率・デチューン・レベル・ADSR・ベロシティ感度、フィードバック。
  32bit整数位相 + 4096点サイン表。オペレーターEGはコントロールレートで補間。
- **PCM**: WAV（8/16/24/32bit整数、32/64bit float、モノ/ステレオ、smplチャンクのループ点・ルートキー）、
  4点Hermite補間、ループ（no_loop / one_shot / loop_continuous / loop_sustain）、キーレンジ/ベロシティレイヤー/ラウンドロビン(lorand/hirand)、
  リージョン単位のADSR（ampeg_*）。SFZ対応範囲は `docs/FEATURES.md` 参照。
- **ハイブリッド**: 3レイヤーを任意の方式で同時発音。`mix.morph` とモジュレーション先 `Morph` / `a.level` 等で、
  演奏中にPCMとFMの比率などを連続的に変化させられます。

## モジュレーションマトリクス

8スロット、各スロット `source × via × amount → destination`。コントロールレート（16サンプル毎）で評価し、
音声パラメータはブロック内で線形補間します。

- ソース（16）: velocity, key, ampenv, env2, lfo1, lfo2, modwheel, pitchbend, chanpressure,
  **notepressure, notetimbre, notex, notepitch**（鍵盤ごと）, random, breath(CC2), expression(CC11)
- 行き先（19）: pitch, a/b/c.level, morph, a/b/c.pitch, va.pw, va.mix, va.noise, fm.index, fm.feedback,
  cutoff, resonance, drive, amp, pan, lfo1.rate

全てボイス単位で評価されるため、per-noteソースは鍵盤ごとに独立して作用します。
ソース・行き先は enum に追加するだけで拡張可能です。エフェクトは全ボイス共通のため、別系統の **FX Mod**
（6スロット、グローバルなソース → 任意の FX パラメータ）で変調します（[EFFECTS.md](EFFECTS.md)）。

## エフェクトラック

`Engine::process()` の最後で、全ボイスの合計を `fx::FxRack` に通します: スロット1〜12（31種のブロックから任意の順、
同じブロックは1回だけ）→ ルーパー → マスター音量 → マスターリミッター（0.5 ms アタック + ソフトクリップ、遅延なし）。
ラックは32サンプル単位で処理し、FX Mod・テンポ同期のビート位置もこの単位で更新します。ブロックは
`fx::FxBlock`（`prepare / reset / update / process`）を実装し、メモリは `prepare()` でのみ確保します。
ユーザー IR はサンプルセットと同じ方式（非音声スレッドで構築 → ポインタ差し替え → `collectGarbage()` で解放）です。

## パラメータとプリセット

全パラメータは1つの float 配列（現在 519 個、うちエフェクト 314 個）で、メタデータ（名前、グループ、範囲、カーブ、enumラベル）を
`paramMetadataJson()` で公開します。Android の Edit 画面はこのメタデータから自動生成されるため、
エンジンにパラメータを追加するとUIコードの変更なしで編集可能になります。

プリセットはJSONでパラメータ名をキーとして保存（enum は文字列）。未知のキーは警告、欠落キーは既定値になるため、
エンジンが拡張されても旧プリセットを読み込めます。サンプル参照・ユーザー IR はライブラリルートからの相対パスです。
v0.1 の固定エフェクト（`chorus.* / delay.* / reverb.* / eq.*`）は `applyLegacyParam()` で新しいブロックへ変換します。

## 品質設定・性能

- ポリフォニー 16 / 32 / 64（設定画面）。超過時は「リリース中で最も小さいボイス」→「最も古いボイス」をスチールし、
  5 msでフェードアウト（予備8ボイスを使用）。
- DSP品質: Low / Normal / High。Drive・Amp のオーバーサンプリングが 1x / 2x / 4x に切り替わります。
- 高速化: `fastFloor` / `fastExp2`（多項式）/ 整数位相FM / 係数共有のステレオフィルター。
  NEON明示最適化は未実装（Phase 4）。現在のコードは汎用C++で、コンパイラの自動ベクトル化に任せています。
  `HSYNTH_ENABLE_SIMD` オプションは将来のSIMD実装用で、無効でも同じ結果になるよう汎用パスを維持する方針です。

## Android 層

- `AudioEngine`（Oboe）: LowLatency、Exclusive（失敗時Sharedへ自動フォールバック）、Float32ステレオ、48 kHz要求
  （端末側が異なればOboeがリサンプル）、バッファ = burst × N または LatencyTuner による自動。
  ストリーム切断（USBオーディオ抜き差し等）時は別スレッドで自動再オープン。コールバック処理時間・負荷・XRun数を計測。
- `MidiInput`: android.media.midi。USB/BLE/仮想MIDIのホットプラグ対応。Android 13+ では UMP トランスポートの
  デバイスを UMP として開き、バイト列をそのまま C++ の `MidiTranslator` へ渡します。
- Storage Access Framework: WAV（複数選択）、SFZフォルダー（ツリー選択→相対パスを保持してアプリ内ライブラリへコピー）、
  プリセットのインポート/エクスポート。エンジンはファイルパスのみを扱う（移植性のため）ので、SAF のコンテンツはコピーしてから読み込みます。
- Activity は再生成されることがあるため（ウィンドウ構成変更など）、ローダーExecutorとエンジンはプロセス単位のシングルトンです。
