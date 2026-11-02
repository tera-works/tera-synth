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
 ├─ Stereo multimode filter (SVF: LP12/LP24/BP/HP/Notch、Ladder、Comb+/-、Formant、drive=非線形)
 ├─ Voice FX（saturate / fold / crush / ring、ボイスごとに独立）
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

## マルチティンバー（16 パート）

エンジンは 16 パート（`kNumParts`）を持ち、MIDI チャンネルごとに別の音色を鳴らせます（GM / マルチチャンネル MIDI）。

```
MIDI ch ──► MidiTranslator（Event::channel を付与）──► Engine
  ch == Mix::Channel のパートへ（複数パート可 = レイヤー） / kOmniChannel は選択パートへ
Part 1..16: パートパラメータ（ボイス・レイヤー・モッドマトリクス）+ サンプルセット 3 個
  └─ 共有ボイスプール（64 + フェード 8）
       └─ パートバス ─ volume² × CC7² ─ pan(+CC10) ─┬─ ×FX send ────► エフェクトラック（共有）─┐
                                                   └─ ×(1-FX send) ─► ドライバス ──────────────┴► Master → Limiter
```

- **パート = 1 つの完全なパッチ**: パラメータ id `[0, P::kPartParams)`（= エフェクトラックより前）のうち
  `master.volume` と `limiter` を除いたもの（`isPartParam(id)`）をパートごとに持ちます。LFO2 もパート単位です。
  エフェクトラック（12 スロット・FX Mod）、ルーパー、マスター音量、リミッター、ユーザー IR は全パート共通（グローバル）。
- **パート 0（UI 表示はパート 1）** は従来の配列（`setParam` / `p_`）そのもので、パート 1〜15 は別の atomic 配列＋
  パートごとのバージョンカウンタを持ちます。音声スレッドはバージョンが変わったパートだけローカル配列にコピーします
  （従来の仕組みをパート数分に拡張。`process()` 内で確保・ロックなし）。
- **サンプルセット / ユーザーウェーブテーブル** はパート × レイヤー（16 × 3）ごとに保留ポインタ → 差し替え → 廃棄キュー
  （容量 128）。差し替え時に PCM を止めるのはそのパートのボイスだけです。
- **ミックス設定**（`Mix::`、パートごと、メタデータは `partMixTable()`）:

  | 名前 | 範囲 | 既定 | 内容 |
  |---|---|---|---|
  | `volume` | 0〜1 | 1 | ゲイン = volume²（マスターと同じカーブ）。MIDI CC7 も二乗で乗算 |
  | `pan` | -1〜1 | 0 | バランス型（中央で左右とも 1.0）。MIDI CC10 はオフセットとして加算 |
  | `mute` | bool | off | ミュート中はノートオンを無視し、出力を 10 ms でフェード |
  | `channel` | 1〜16 | パート番号 | 受信 MIDI チャンネル。同じチャンネルの複数パートはレイヤー |
  | `key.low` / `key.high` | 0〜127 | 0 / 127 | キーレンジ（トランスポーズ前のキーで判定）。スプリット用 |
  | `transpose` | -48〜48 | 0 | 半音 |
  | `fx.send` | 0〜1 | 1 | 1 = 全てラックへ、0 = ラックを通らずドライバスへ（ラック後・マスター前に加算） |

  ゲイン変更は 10 ms の線形ランプ（コントロールブロック単位）でクリックを防ぎます。
- **モード**: `Engine::setMultiTimbral(false)`（既定）は「シングルパート / オムニ」モードで、全イベントがチャンネルに
  関係なくパート 0 に届き、ミックス設定は適用されません。描画経路も従来のコードそのままなので **出力は従来とビット単位で
  一致** します（テスト `multi_single_part_mode_identical` と変更前ビルドとの比較で確認）。マルチモードでパート 0 だけを
  既定ミックスで使った場合もシングルモードとビット一致します。マルチ → シングルに戻すとパート 1〜15 のボイスは即停止。
- **イベントの振り分け**（マルチモード）: `Event::channel`（0〜15）が一致する全パートへ。`kOmniChannel`（ホスト UI・
  タッチ鍵盤など、`Event` ファクトリの既定値）は `selectPart()` で選んだパートへ。`AllNotesOff/AllSoundOff` を
  オムニで送ると全パート。noteId 付きの NoteOff / per-note イベントはパートに関係なく noteId で探すため、
  発音中にチャンネル設定を変えてもノートが残りません。キー一致（noteId=0）の照合はトランスポーズ前のキーで行います。
- **MIDI**: `MidiTranslator` は全イベントにチャンネルを付けます（UMP グループは無視）。`setMultiTimbral(true)` にすると
  MPE のメンバーチャンネル解釈を無効化し、各チャンネルを独立した GM 的チャンネルとして扱います。MPE はシングルパート
  モードでのみ有効です。プログラムチェンジは従来どおり `EventSink::onProgramChange(channel, program)` でホストへ通知され、
  ホストがそのチャンネルのパートに `applyPartPreset()` します。
- **ボイススチール**: プールは全パート共有、上限は `setMaxVoices()`。スチール対象は
  ① リリース中のボイスを優先、② その中で「このノートオン後に最も多くのボイスを持つパート」（要求したパートは +1 で数える）、
  ③ 同数ならリリース中は最小レベル / 発音中は最古。大量に弾くパートは自分のボイスから奪い、少数ボイスのパートは守られます。
  パートが 1 つなら従来と同じ規則です。
- **エフェクトラックの FX Mod** のグローバルソース（mod wheel、LFO2、チャンネルプレッシャー、expression、breath、ピッチベンド）はパート 1 の値に追従します。
  ラックのノートトリガー（リトリガー等）は FX send > 0 のパートのノートオンで発生します。

### ホスト API（追加分）

| API | 内容 |
|---|---|
| `Engine::setMultiTimbral(bool)` / `multiTimbral()` | モード切替。`MidiTranslator::setMultiTimbral()` も同じ値に設定すること |
| `Engine::selectPart(n)` / `selectedPart()` | オムニイベント（タッチ鍵盤等）の送り先パート。UI の「選択パート」 |
| `setPartParam(n, id, v)` / `getPartParam` / `setPartParams(n, values)` / `getPartParams` | パート N のパラメータ。グローバル id（FX・master・limiter）は `setParam` へ転送 |
| `setGlobalParams(values)` | 非パートパラメータ（ラック・master・limiter）だけを一括設定 |
| `setPartMix(n, Mix::X, v)` / `getPartMix` | ミックス設定 |
| `setPartSampleSet(n, layer, set)` / `partSampleSetName/Path` | パート N のサンプルセット |
| `partActiveVoices(n)` | パートごとの発音数（メーター用） |
| `applyPartPreset(engine, n, preset, root, warn)` / `capturePartPreset` | 通常プリセットをパート N へ（ラック・IR・master は無視） |
| `loadPartSamples` / `loadPartWavetable` | パート N のレイヤーへ WAV/SFZ/ウェーブテーブル |
| `MultiPreset` + `multiToJson / multiFromJson / loadMultiFile / saveMultiFile` | マルチ JSON（`preset.h` 参照） |
| `applyMulti(engine, multi, root, warn)` / `captureMulti` | マルチの読込（マルチモードを ON にする）/ 取得 |

従来 API（`setParam`、`setSampleSet`、`applyPreset`、`capturePreset` …）はすべてパート 0 ＋グローバルを対象にし、
動作は変わりません。

**マルチ JSON**（`"format": "hsynth-multi"`）: `global.params`（非パートパラメータのみ）と `global.impulses`、
16 要素の `parts[]`（各 `mix` と、パートパラメータだけを持つ通常プリセットオブジェクト `preset`）、`selected`（1〜16）。
欠けたパート・キーは既定値、未知のキーは警告です。

### Windows / Android ホストでの対応が必要な項目（未実装）

- マルチモード ON/OFF の UI と、`engine.setMultiTimbral()` + `translator.setMultiTimbral()` の同期。
- パート選択（`selectPart`）と、Edit 画面の対象を選択パートにする（`get/setPartParam`、サンプル読込は `loadPartSamples`）。
  現在の UI は従来 API を使うためパート 1 を編集します。
- パートごとのミキサー画面（`partMixTable()` から自動生成可能）と発音数メーター（`partActiveVoices`）。
- `onProgramChange(channel, program)` → そのチャンネルのパートへ `applyPartPreset`。
- マルチの保存・読込（`saveMultiFile` / `loadMultiFile` → `applyMulti` / `captureMulti`）。
- JNI ブリッジに上記関数を追加（Android は最後の S23 移植時）。

**ボイスフィルター**（`dsp/svf.h`）: SVF 系に加えて
- **Ladder**: 4 段トランジスタラダー LP（TPT / ゼロ遅延帰還、tanh 入力段）。resonance=1 で自己発振、パスバンドの音量低下を一部補正。
- **Comb+ / Comb-**: カットオフ周波数に同調した帰還コム（遅延 = 1 / cutoff、resonance = 帰還量）。+ は倍音列、- は奇数倍音。
  ループ内に穏やかなダンピングと tanh。遅延メモリ（4096 サンプル × 2ch）はボイスごとに `prepare()` で確保。
- **Formant**: a / e / i / o / u のフォルマント（F1〜F3）を 3 本の並列 BPF で再現。`filter.morph`（Vowel, 0〜1）で母音を
  連続変化、カットオフ 1 kHz で原音域・それ以外は比例シフト（0.25〜4 倍）、resonance で Q。

**ボイス FX**（`dsp/voice_fx.h`、`vfx.*`）: フィルターとアンプ EG の間に入るボイス単位のインサート。
`saturate`（tanh）、`fold`（サイン波フォールダー）、`crush`（16→3 bit + 最大 1/24 のサンプルレート低下）、
`ring`（ノート周波数 × `vfx.ratio` のサイン波によるリングモジュレーション、キー追従）。`vfx.amount` はモジュレーション先
（per-note 可）なので、鍵盤ごとの圧力で歪み量を変える、といった演奏ができます。オーバーサンプリングなし（軽量重視。
重い歪みはラックの Drive / Amp を使用）。

## モジュレーションマトリクス

8スロット、各スロット `source × via × amount → destination`。コントロールレート（16サンプル毎）で評価し、
音声パラメータはブロック内で線形補間します。

- ソース（20）: velocity, key, ampenv, env2, lfo1, lfo2, modwheel, pitchbend, chanpressure,
  **notepressure, notetimbre, notex, notepitch**（鍵盤ごと）, random, breath(CC2), expression(CC11),
  **cc1〜cc4**（割り当て可能な MIDI CC。`ctl.cc1..4` で CC 番号を指定、既定 16〜19。ネックセンサーやペダル等を
  CC として送れば任意の行き先へ割り当て可能。CC1 も指定可）
- 行き先（23）: pitch, a/b/c.level, morph, a/b/c.pitch, va.pw, va.mix, va.noise, fm.index, fm.feedback,
  cutoff, resonance, drive, amp, pan, lfo1.rate, wt.pos, wt.warp, filter.morph, vfx.amount

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
  5 msでフェードアウト（予備8ボイスを使用）。マルチモードではボイス数の多いパートから優先的に奪います（上記）。
- マルチティンバーの負荷: `hsynth_bench` の `multi 16 parts x 4 voices` 行（同じ 64 ボイスのシングルパートと比較）。
  パートごとの追加コストは 16 サンプル単位のパートバス合成と LFO2 程度です。
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
