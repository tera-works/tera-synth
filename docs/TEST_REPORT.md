# テスト結果と既知の問題（2026-11-02, v0.4 エンジン）

## v0.4 エンジン（Linux クラウド環境, GCC 13, -O2 Release）

`engine/build/hsynth_tests` — **119 テスト全て合格**（約 45 秒）。追加分: マルチティンバー（`test_multi`）、
ボイス DSP / ボイスモード / モジュレーション / アルペジエーター / チューニング（`test_voice`）、SFZ 拡張（`test_sfz`）、
FLAC（`test_flac`）、音質回帰（`test_quality`）、同梱プリセット全数の読込・発音（`factory_presets_render`）、
コードレビューで見つけた不具合の再現テスト（`review_*`）。

- **既存の音の互換性**: 既存プリセットの出力が `main` のビルドとサンプル単位で一致することを確認（golden シナリオ、
  MPE / S&H / サステイン / スチールを含む MIDI シナリオ、原音程以下の SFZ / PCM シナリオ）。golden ファイルは未変更。
  意図した変更は「PCM を原音程より上で鳴らした音」のみ（折り返し除去、下記）。
- **AddressSanitizer + UndefinedBehaviorSanitizer** で全テストを実行: メモリエラー・未定義動作なし（FLAC デコーダーの
  壊れたデータでの符号付き整数オーバーフローを修正済み）。`fx_block_cpu_cost` だけはサニタイザーの遅さで時間超過（Release では合格）。
- ランダムパラメータの負荷試験: 全パラメータ（20 s）、マルチモードのパートパラメータ・ミックス・モード切替（12 s）、
  モノ / ユニゾン / アルペジエーターのノートスタック（ハングノートなし）。

### 音質の測定（`test_quality.cpp`, 折り返し / 倍音のエネルギー比）

| 項目 | 以前 | 現在 |
|---|---|---|
| PCM（全帯域ノコギリ波サンプル）を +12 半音、10 kHz 以下 | -35 dB | **-90 dB** |
| 同 +24 / +36 半音、10 kHz 以下 | -26 / -22 dB | **-88 dB** |
| ウェーブテーブル明るいフレーム（basic pos 1.0, C5） | -40 dB | **-66 dB** |
| VA ノコギリ波 C7、10 kHz 以下 | -54 dB | -54 dB（polyBLEP、変更なし） |

### 性能（`hsynth_bench 32 2`, Intel Xeon 2.1 GHz の仮想 CPU 1 コア, 48 kHz / 128 フレーム）

クラウド VM のため ±20 % 程度ばらつきます。主な行（ns/ボイス・サンプル）: VA×2 + LP24 32、Ladder + drive 60、
FM 4op 57 / 6op 69、WT ユニゾン 7 266、PCM 原音程以下 39 / ピッチアップ（sinc + ミップ）66、
16 パート × 4 ボイス 32（64 ボイスのシングルパートと同等）、同 + パートインサート 2 個 55。
エンジン初期化（初回 `prepare()`、内蔵ウェーブテーブル生成）は 460 ms → 180 ms に短縮。

### 非公開テスト音源（tera-synth-assets, a7527a7）での確認

`presets/user/` の 23 プリセット（GM 15、SonicCell ピアノ 3、ドラムキット 5）を現在のエンジンで読み込み・発音:

- 全プリセットが警告なしで読み込み、発音、クリップなし。オフラインで 48〜80 倍速（1 音ずつのスケール）
- メロディ楽器では鍵盤の 30〜60 % が録音レートより速く再生（44.1 kHz 素材・数半音おきのサンプル）、最上部ゾーンは
  最大 10.7 倍（Finger Bass）。ピッチアップの帯域制限が効く範囲で、Normal と Low の差は -36 dB（Trumpet）〜 -86 dB
- ミップ（1/2・1/4 レートのコピー）によるメモリ増加は 1 楽器 0.5〜2.9 MB（Grand Piano は全体 210 MB 中 2.5 MB）
- マルチティンバー（ピアノ / ベース / ストリングス / ドラム、ch1/2/3/10）を警告なしで読み込み・演奏
- 隣り合う鍵の音量差 6 dB 超: Grand Piano（キー 56 / 59 / 65 / 80 / 98、最大 12 dB）、SC Piano 1 / 2、Choir、
  Strings、Vibraphone、Warm Pad の一部。**エンジンではなく録音素材の差**（例: Grand Piano v120 の fs5 = -14.2 dB →
  a5 = -26.7 dB、fs5 の v80 と v120 の差 26 dB）。サンプラー側（`hsynth_sampler` / 元の SoundFont）での確認が必要

### 追加の検証（v0.4.1）

- **ThreadSanitizer**: UI スレッド（パート / ミックス / モード編集と読み出し、`captureMulti`）・ローダースレッド
  （パートのサンプル差し替え）・オーディオスレッドの同時実行テスト（`multi_api_concurrent_threads`）と
  `engine_sample_swap_concurrent` でデータ競合なし
- **clang -Wall -Wextra -Wshadow / clang-tidy（bugprone, performance）**: 実害のある指摘なし（変数のシャドーイング 1 件を修正）
- **ホストの互換性**: Android JNI ブリッジとデスクトップ `app.cpp` が現在のエンジン API でコンパイルできることを確認
  （プラットフォームヘッダーはスタブ。Windows / Android のビルド自体は未実施）
- **サンプルセット差し替え時のクリックを除去**: 鳴っているレイヤーの PCM を即停止していたのを 6 ms のフェードに変更
  （古いセットはフェードが終わるまで保持してから破棄）。差し替え時の段差（2 階差分の最大）0.173 → 0.0007
  （`sample_set_swap_fades_without_click`、ASan でも確認）
- **`writeWav` の範囲外読み出しを修正**: 左右のチャンネル長が違うと短い方を範囲外まで読んでいた（ファイルにメモリの
  ゴミが入り、マシンや実行ごとに内容が変わる）。短い方は無音で埋めるように修正（`wav_write_unequal_channel_lengths`）

### リアルタイム性（オーディオスレッドでのメモリ確保）

`realtime_process_never_allocates`: テストバイナリで `operator new` を差し替え、`process()` 実行中の確保を数える
（検出器の自己チェック付き）。マルチ 6 パート、12 スロットのラック（畳み込み・ピッチ・ディレイ等）、Quality High、
PCM（sinc + ミップ、リリーストリガー）/ WT / FM 6op、Ladder / Comb / Formant + ドライブ、Voice FX、ユニゾン、
モノ / レガート / グライド、アルペジエーター、パートインサート、MIDI クロック、演奏中のサンプル差し替え、
モード切替、2048 フレームを含む不規則なブロック長 → **確保 0 回**。

### ファイル読込のファジング（`engine/tools/hsynth_fuzz.cpp`, ASan + UBSan）

変異ファジング（ビット反転・境界値・切り詰め・挿入・連結、テキスト形式は SFZ / JSON のトークン挿入）で、
ユーザーが取り込む全形式を検査:

| 対象 | 入力数 | 結果 |
|---|---|---|
| WAV（16/24/32 bit, float 32/64, extensible, smpl, Serum clm） | 200,000 | **ヒープ範囲外読み出し 1 件を修正** |
| FLAC | 30,000 | 問題なし |
| JSON / プリセット（読込 → 適用 → 発音） / マルチ | 20,000 / 4,000 / 600 | 問題なし |
| SFZ（読込 → 発音 → ノートオフ） | 約 6,000 | **巨大確保によるクラッシュ 1 件を修正** |
| Scala / ウェーブテーブル / IR | 200,000 / 3,000 / 3,000 | 問題なし |

修正した 2 件:
- WAV の fmt チャンクが「ブロック長 < チャンネル数 × サンプルサイズ」を宣言すると、データチャンクの外まで読んでいた
- SFZ の `sample=`（空）やフォルダー名がディレクトリを開き、`readWav` がその見かけのサイズ（2^63）を確保しようとして
  異常終了。RIFF としてあり得ないサイズは拒否し、空の `sample=` は警告して読み飛ばすように修正

ツールとして `hsynth_fuzz` を追加（`hsynth_fuzz <対象> <回数> <作業フォルダー> <シード...>`。サニタイザー付きでビルドして使う）。

### 既知の問題・制限（v0.4 で追加分）

- フィルタードライブ / Voice FX saturate は Normal ではオーバーサンプリングなし（高音域・強いドライブで可聴帯域 -24〜-30 dB）。
  Quality High では ADAA で 12〜17 dB 改善（`quality_high_antialiased_drive_and_saturate`）。fold / ring / ladder の tanh は対象外。
  1 次 ADAA は出力が半サンプル遅れるため、saturate を mix < 1 で使うと高域がわずかに下がる（10.5 kHz で約 -1 dB、可聴帯域に
  ノッチはなし）。
- PCM の 4 オクターブを超えるピッチアップは帯域制限が不完全（ミップは 3 段 = 1/8 レートまで。+43 半音で -75 dB）。
- Windows ビルド・試聴・Android 実機は未確認（クラウドでは engine のみ）。

---

# v0.2.0 までの記録

## 0. v0.2.0 エフェクトラック

`engine/build-desktop/hsynth_tests` — **52 テスト全て合格**（エフェクト 23 テストを追加、約1〜2.5分）。
内容・測定値・CPU は [EFFECTS.md](EFFECTS.md) の「検証結果」。主な点:

- 全 31 ブロックを既定値・全 enum 選択肢・最小/最大値・ランダム設定・44.1/96 kHz で処理して NaN / 発散なし
- EQ・コンプ・リミッター・テンポ同期ディレイ・ピッチ/周波数シフト・畳み込み等を数値で検証
- ON/OFF のクリックなし、スピルオーバー、FX Mod、ルーパー、ユーザー IR、旧プリセット変換
- 全 23 プリセットのレンダリングを波形段差で検査
- Windows 版自動テスト PASS（遅延コールバック 0、コールバック負荷ピーク 12〜24 %、スクリーンショット 7 枚）
- APK 0.2.0（debug / release）ビルド成功。**実機・エミュレーターでの動作は未確認**

開発中に見つけて修正した不具合:

1. コーラスのキー・リトリガーで遅延時間が飛びクリック → 遅延線を動かす LFO は頭出ししない（同期時は位相をなめらかに引き込む）
2. マスターリミッターの瞬時アタックで波形に段差 → 0.5 ms のアタック + ソフトクリップ（遅延は増やさない）
3. ディレイ時間のグライドが float の分解能で約 3 サンプル手前で止まる → 最小ステップ付きのグライド
4. 多ボイスコーラスのフィードバックが発散 → ボイス平均をフィードバック
5. 周波数シフターの側帯波が逆、IR 読込で末尾が消える、キャビネットのカットフィルターが最大値でも残る

ゴールデンファイル（`engine_golden_render`）は、リバーブ・ディレイの作り直しとマスターリミッター変更に合わせて再生成
（旧結果との差: 発音中の音量はほぼ同じ（-0.4〜+1.3 dB）、残響部はテストがディレイを直接設定しているため大きめ）。

## 1. DSP エンジン単体テスト（v0.1.0 時点。Windows / MinGW GCC 16.2, -O3）

`engine/build-desktop/hsynth_tests` — **30 テスト全て合格**（約20〜40秒）

| 区分 | テスト | 内容 |
|---|---|---|
| DSP | adsr_stages | 各ステージ時間・サステイン値・リリース完了 |
| | va_oscillator_frequency_and_dc | 全波形の周波数精度（±1 Hz）・DCオフセット・ピーク |
| | polyblep_reduces_aliasing | 4567 Hz ノコギリ波の折り返し成分: polyBLEP 0.066 / ナイーブ 0.837（約 -22 dB） |
| | svf_lowpass_response | LP24 で 10×カットオフが -54 dB 以下、BP/HP/Notch の特性 |
| | svf_stable_under_modulation | カットオフ・レゾナンス・ドライブを毎ブロック乱数変調しても発散しない |
| | fm_zero_index_is_sine / fm_modulation_adds_sidebands | FM の基本動作 |
| | effects_stable_extreme_settings | （v0.2 で `test_fx.cpp` の全ブロック試験に置き換え） |
| エンジン | engine_note_on_off | 発音・リリース・ボイス解放 |
| | engine_pitch_and_per_note_expression | **noteId 別の per-note ピッチ**（他ノート宛のイベントは無影響、+12 で 880 Hz） |
| | engine_pitch_bend_range | ベンドレンジ 12 で -1 → 220 Hz |
| | engine_polyphony_limit_and_stealing | 40ノート/上限16 でスチール、ピーク出力 ≤ 1.0 |
| | engine_sustain_pedal | サステインペダル |
| | engine_hybrid_layers_and_morph | VA+FM レイヤーと ModWheel→Morph |
| | engine_deterministic_render | 同じ入力 → ビット一致の出力 |
| | engine_random_parameter_stress | 全パラメータを20秒間ランダム変更しながら演奏、NaN なし・ピーク ≤ 1 |
| | engine_sample_swap_concurrent | 別スレッドが 0.2 ms 毎にサンプルセットを差し替え中も演奏（スレッド安全性） |
| | engine_long_run_stability | 120秒相当の連続ランダム演奏（`HSYNTH_LONG_TEST_SECONDS` で延長可）。初期版で600秒も合格 |
| | engine_golden_render | **ゴールデンファイル比較**（RMS包絡、許容 2%）。最適化による音質変化の検出用 |
| I/O | wav_roundtrip | 16bit/float32、ステレオ、smpl ループ、不正ファイル |
| | sfz_parse_and_mapping | グループ継承、#define、default_path、音名、空白入りファイル名、ベロシティレイヤー、未対応オペコード警告、欠落サンプル |
| | pcm_pitch_and_loop_playback | 44.1 kHz サンプルを 48 kHz で1オクターブ上（2000 Hz ±5）、ループ継続 |
| | preset_roundtrip / preset_apply_to_engine | JSON 保存→読込で全パラメータ一致、未知キー警告、範囲外クランプ、サンプル相対パス |
| | json_basics / param_metadata_consistency | |
| MIDI | midi1_translation | ランニングステータス、vel0=NoteOff、SysEx・クロック無視、分割受信 |
| | mpe_translation | メンバーチャンネルの PB/CP/CC74 → per-note |
| | ump_translation | MIDI2 Note On(16bit vel)、Per-Note Pitch Bend、Registered PNC #74、MT2、バイト列（ビッグエンディアン） |

### 性能（`hsynth_bench 32 2`, Intel i5-1135G7 1コア, 48 kHz / 128 フレーム）

| 構成 | 1コア負荷 | ns / ボイス・サンプル |
|---|---|---|
| VA 2osc, フィルター無 | 6.7 % | 44 |
| VA 2osc, LP24 | 8.5 % | 55 |
| FM 4op | 13.9 % | 90 |
| VA + FM + VA（3レイヤー）, LP24 | 25.4 % | 165 |
| 64 ボイス × 3レイヤー + 全エフェクト（最悪ケース） | 45〜57 % | — |

最適化（`fastFloor`、`fastExp2`、整数位相FM、ブロック単位のFM演算、ステレオフィルター係数共有）により、
3レイヤー構成で 287 → 165 ns に短縮。最適化後はゴールデンファイルを再生成しています（FM EG のコントロールレート化による意図的な変化）。

### プリセットのオフラインレンダリング

デモプリセット 13 種すべてを `hsynth_render` でレンダリングし、読込警告なし・クリッピングなし（リミッター動作）を確認。
レンダリング速度は 28〜106 倍速。

## 2. Android（エミュレーター: Android 15 / x86_64, Pixel 7 プロファイル）

| 項目 | 結果 |
|---|---|
| debug / release APK ビルド | ✅（arm64-v8a + x86_64、16 KB ページ整列、apksigner 検証 OK） |
| インストール・起動 | ✅ 初回起動でデモ素材展開→Init VA 読込 |
| Oboe ストリーム | ✅ AAudio 48 kHz float32 stereo で起動（エミュレーターのため Shared / 非LowLatency、バッファ 40 ms） |
| タッチ鍵盤で発音 | ✅ voices 1/32 表示、鍵盤ハイライト |
| 4画面の表示 | ✅ `docs/screenshots/` |
| SFZ 読込（glass_piano → Layer A） | ✅ 76 ms、PCM へ自動切替 |
| プリセット保存→別プリセット→再読込 | ✅ Layer A = PCM / glass_piano が復元 |
| ランダムUI操作（monkey 3000イベント） | ✅ クラッシュ・ANR なし |

テスト中に見つけて修正した不具合:

1. **起動時クラッシュ**: Activity 再生成時、破棄済み Activity のコールバックが停止済み Executor を使用 → Executor をプロセス共通化し、破棄後のコールバックを無視。
2. **Play 画面が空白**: タブバーのスペーサー View が全高に広がっていた → 高さ固定。
3. Play 画面上部のボタンが画面外にはみ出す → 横スクロール化。
4. エンジン: 初回ブロックでボイス開始時にパラメータ未設定（null 参照）→ イベント処理前にコンテキスト設定。
5. エンジン: FM の CPU 負荷が過大 → 整数位相・ブロック処理に変更。

## 3. 未検証事項

- **Galaxy S23 実機での動作は未検証です**（このPCに端末が接続されていませんでした）。特に以下は実機で確認が必要です:
  実際の出力遅延、Exclusive / LowLatency の取得可否、XRun、ARM64 での CPU 負荷、タッチ操作感、USB オーディオ機器。
- USB MIDI / MIDI 2.0 UMP デバイスとの実接続（変換ロジックは単体テスト済み）。
- SAF の WAV / SFZ フォルダー取り込み（エミュレーターではファイルピッカー操作を自動化していません。取り込み後の読込処理は同じコードパスでテスト済み）。
- Raspberry Pi / Linux でのビルド。

## 4. 既知の問題・制限

- 音量: 和音で多くのプリセットがリミッターに当たります（-1 dBFS から上はソフトクリップ）。必要に応じ Master Volume を下げてください。
- エフェクトの音質は数値・波形の検査のみで、耳での確認はしていません。Android 版のボコーダーは音声入力が未対応です。
- タッチスクリーンには圧力センサーがないため、Pressure は接触面積から推定、ベロシティは鍵盤上のタッチ位置（下ほど強い）から算出します。
- PCM レイヤーのサンプル差し替え時、そのレイヤーを発音中のボイスは即停止します（クリックが出る可能性）。
- SFZ は Phase 1 のサブセット（`docs/FEATURES.md`）。リリーストリガー、排他グループ、フィルター/LFO 系オペコード、FLAC は未対応。
- 全サンプルをメモリにプリロードするため、大規模ライブラリは RAM に制約されます（ストリーミングは Phase 4）。
- FM の高比率オペレーターはアンチエイリアシングなし。
- MIDI は全チャンネルを1パートで受信（マルチティンバー未対応）。Program Change はライブラリのプリセット一覧の n 番目を選択。
- release APK は debug 鍵で署名（サイドロード用）。
- このPCではユーザー環境変数 `ANDROID_HOME` が古いパスを指しているため、ビルド時に上書きが必要（`docs/BUILD.md`）。
- Windows 上の Gradle ビルドで中間ディレクトリの削除がロックで失敗することがあります（ウイルス対策のスキャン等）。再実行で解消します。

## v0.3 ウェーブテーブル（2026-11-02）

`hsynth_tests` 56 テスト全て合格（追加: `wavetable_builtins_and_pitch`、`wavetable_band_limited`、
`wavetable_wav_import`、`engine_wavetable_layer`）。

- 正弦フレームの周波数 440 Hz ±0.5、フレームのピーク 0.99
- 4567 Hz のノコギリ波フレーム: 折り返し成分 / 倍音 = 0.00004（約 -86 dB）
- WAV 読込: 2048 × 2 フレーム、600 サンプルの単一波形（再サンプリング）
- エンジン: ユニゾン 5 + bend ワープ + モッドホイール→ポジションで有限・ステレオ化
- 負荷（`hsynth_bench 32 2`）: WT 1 ボイス 28 ns / ボイス・サンプル（VA 2 osc 24 ns と同程度）、ユニゾン 7 で 210 ns
- デモプリセット 23〜25（WT Morph Pad / WT Formant Lead / WT Sync Bass）をオフラインレンダリングで確認
