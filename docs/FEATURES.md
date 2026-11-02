# 実装済み機能 / 未実装機能

凡例: ✅ 実装・テスト済み　🟡 実装済み（実機未検証 / 部分的）　⬜ 未実装（予定フェーズ）

## Phase 1（今回の対象）

| 項目 | 状態 | 備考 |
|---|---|---|
| Android Studio プロジェクト | ✅ | AGP 9.1 / Gradle 9.3.1 / NDK r29 / CMake 3.31.6、minSdk 28、targetSdk 36 |
| Oboe 低遅延出力 | 🟡 | LowLatency + Exclusive（Sharedへ自動フォールバック）、Float32、48 kHz要求、burst×N / 自動チューナー。Galaxy S23 実機での遅延・XRun は未計測 |
| C++ 音源エンジン（Android非依存） | ✅ | Windows でビルド・テスト済み。Android では arm64-v8a / x86_64 でビルド |
| VA | ✅ | Saw / Square / Pulse(PWM) / Triangle / Sine、2osc、デチューン、ノイズ、polyBLEP/BLAMP |
| FM | ✅ | 4オペレーター、8アルゴリズム、比率・デチューン・レベル・ADSR・ベロシティ、フィードバック |
| WAV 再生 | ✅ | 8/16/24/32bit, float32/64, mono/stereo, smpl ループ/ルートキー、Hermite補間 |
| SFZ 基本マッピング | ✅ | 下表参照。未対応オペコードは警告表示 |
| ADSR | ✅ | アンプ・モッドEG・FMオペレーター毎・SFZリージョン毎 |
| 基本フィルター | ✅ | SVF LP12 / LP24 / BP / HP / Notch、レゾナンス、ドライブ（非線形）、キートラック、EG/ベロシティ量 |
| ディレイ・リバーブ | ✅ | v0.2 でエフェクトラックに拡張（下記 Phase 2、[EFFECTS.md](EFFECTS.md)） |
| タッチ鍵盤 | ✅ | マルチタッチ、位置ベロシティ、グリッサンド/ベンド、指ごとのTimbre・Pressure・Pitch・X、Hold |
| 音色パラメータ編集 | ✅ | メタデータから自動生成（全519パラメータ）。FX はチェーン編集画面 |
| プリセット保存・読込 | ✅ | JSON、デモ23種（うち FX デモ10種）、SAFインポート/エクスポート |
| 発音数・負荷表示 | ✅ | Play 画面上部と Monitor 画面（DSP負荷、コールバック時間、XRun、遅延推定） |
| Pitch Bend / Modulation ストリップ | ✅ | |
| WAV / SFZ インポート（SAF） | ✅ | WAV 複数選択、SFZ フォルダーごと取り込み（相対パス保持） |
| エラー表示 | ✅ | Library 画面のメッセージログ（警告・エラー色分け） |
| USB MIDI 入力 | 🟡 | android.media.midi、ホットプラグ。実機・実MIDI機器では未検証 |
| MIDI 2.0 UMP | 🟡 | 変換器はテスト済み。Android 13+ の UMP デバイスとの実機接続は未検証 |
| デバッグ計測 | ✅ | Monitor 画面、オフライン `hsynth_bench` |

### SFZ 対応範囲

- ヘッダ: `<control> <global> <master> <group> <region>`（継承あり）。`<curve> <effect> <midi>` 等は警告して無視
- プリプロセッサ: `#define $VAR`, `#include "file"`
- オペコード: `sample, lokey, hikey, key, lovel, hivel, lorand, hirand, pitch_keycenter, pitch_keytrack, tune, pitch,
  transpose, volume, pan, loop_mode/loopmode (no_loop, one_shot, loop_continuous, loop_sustain), loop_start/loopstart,
  loop_end/loopend, offset, end, ampeg_attack, ampeg_decay, ampeg_sustain, ampeg_release, amp_veltrack,
  trigger (attack のみ), default_path, note_offset, octave_offset`
- 音名（`c4` = 60, `c#4`, `db4`）、サンプル名の空白、`\` 区切りパス
- 未対応（警告）: フィルター系 `cutoff/fil_*`、`fileg_*`、`pitcheg_*`、LFO 系、`trigger=release`、`seq_*`、CC 系 `on_locc`、
  `group/off_by`（排他グループ）、`ampeg_hold/delay`、`*` ジェネレーターサンプル、FLAC/OGG

## Phase 2 以降

| 項目 | 状態 | 備考 |
|---|---|---|
| ウェーブテーブル | ✅（v0.3） | レイヤー種別 WT。内蔵 8 テーブル（各 64 フレーム）＋ユーザー WAV（Serum 形式 2048 サンプル × 最大 256 フレーム、AKWF 等の単一波形も可）。FFT による帯域制限ミップマップ 11 段（折り返し -86 dB）、ポジション（Env2・モジュレーション行き先）、ワープ 4 種（sync / bend / mirror / squeeze）、ユニゾン最大 7（デチューン・ステレオ幅） |
| 複数音源のレイヤー | ✅（先行実装） | 3レイヤー + Morph |
| モジュレーションマトリクス | ✅（先行実装） | 8スロット、per-note ソース対応。エフェクトへは FX Mod（6スロット、全246パラメータ） |
| 高度なフィルター（ラダー、コムなど） | ⬜ | |
| エフェクトラック | ✅（v0.2） | 31ブロック × 12スロット任意順。ダイナミクス5、歪み/アンプ/キャビ/ローファイ6、EQ/フィルター4、モジュレーション7、ピッチ2、ディレイ/リバーブ/IRリバーブ、スライサー/スタッター/ステレオ/ボコーダー、ルーパー。[EFFECTS.md](EFFECTS.md) |
| ユーザー IR（キャビネット / 畳み込みリバーブ） | ✅（v0.2） | WAV 読込・リサンプル、遅延ゼロの分割畳み込み、プリセットに保存 |
| ボコーダー用音声入力 | 🟡 | Windows 版は対応（WASAPI デュプレックス）。Android 版は未対応（S23 移植時に追加） |
| MIDI クロック同期 | ⬜ | テンポはパラメータで指定 |
| ボイス単位エフェクト | 🟡 | フィルタードライブのみ。ボイスFXスロットは未実装 |
| オーバーサンプリング | ✅（v0.2） | Drive / Amp の非線形部。DSP品質 Low=1x / Normal=2x / High=4x |
| ベロシティレイヤー・ループ改善 | 🟡 | 基本は実装済み。ループクロスフェード、リリーストリガーは未 |
| FLAC | ⬜ | `loadSampleSet` で明示エラー |
| ディスクストリーミング | ⬜ | 現在は全サンプルをメモリにプリロード |
| マルチパート / マルチティンバー | 🟡（エンジンのみ） | 16 パート（MIDI チャンネル単位、レイヤー/スプリット可）。パートごとに音色パラメータ・サンプルセット 3 個、ミックス（volume / pan / mute / channel / キーレンジ / transpose / FX send、CC7・CC10）。エフェクトラック・ルーパー・マスターは共通、ボイスプール共有でパート間公平にスチール。マルチ JSON（`hsynth-multi`）。既定はシングルパート（従来と出力一致）。Windows / Android の UI・JNI は未対応（[ARCHITECTURE.md](ARCHITECTURE.md#マルチティンバー16-パート)） |
| RP2350 ショルダーキーボード | ⬜ | `EventSink` 翻訳器を追加する設計（ARCHITECTURE.md） |
| MIDI 2.0 Per-Note 制御 | 🟡 | エンジン側は対応・テスト済み |
| ネックセンサー | ⬜ | Event / モジュレーションソース追加で対応予定 |
| NEON SIMD 最適化 | ⬜ | |
| Raspberry Pi / Linux ビルド | 🟡 | engine はポータブル CMake。ALSA/JACK 出力層は未作成、実機ビルド未確認 |
| 低性能SBC向け品質設定 | 🟡 | Quality=Low（リバーブ4ライン）とポリフォニー制限のみ |
