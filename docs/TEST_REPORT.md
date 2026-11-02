# テスト結果と既知の問題（2026-11-02, v0.2.0）

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
