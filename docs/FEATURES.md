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
| 音色パラメータ編集 | ✅ | メタデータから自動生成（全パラメータ）。FX はチェーン編集画面 |
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
  loop_end/loopend, loop_crossfade, offset, end, delay, ampeg_delay, ampeg_attack, ampeg_hold, ampeg_decay, ampeg_sustain,
  ampeg_release, amp_veltrack, amp_keytrack, amp_keycenter, trigger (attack / release / release_key), rt_decay,
  group, off_by, off_mode (fast / normal), seq_length, seq_position, sw_lokey, sw_hikey, sw_last, sw_default,
  fil_type (lpf_1p, hpf_1p, lpf_2p, hpf_2p, bpf_2p, brf_2p; *_4p は 2 ポールで代用し警告), cutoff, resonance,
  fil_keytrack, fil_keycenter, fil_veltrack, default_path, note_offset, octave_offset`
- 音名（`c4` = 60, `c#4`, `db4`）、サンプル名の空白、`\` 区切りパス
- 動作の補足:
  - リリーストリガー（`trigger=release`）はノートオフでノートオンのベロシティを使って発音。ボイスのアンプ EG / フィルターを
    通らず、リージョン自身の EG・レイヤーのレベル / パン / チューニングで鳴ります。`rt_decay` は押鍵時間 1 秒あたりの減衰 dB。
  - `group` / `off_by`: 新しいリージョン（グループ G）が、同じパートで鳴っている `off_by=G` のリージョンを止めます
    （`off_mode=fast` は 6 ms フェード、`normal` はリージョンのリリース）。
  - ラウンドロビン（`seq_*`）のカウンターはパート × レイヤーごとにノートオンで 1 進みます（キーごとではない）。
  - キースイッチの状態はパート × レイヤーごと。`sw_default` が無い場合は最小の `sw_last` から開始。サンプルセット差し替えでリセット。
  - リージョンフィルターはノートオン時に固定（キー / ベロシティ追従あり、フィルター EG・LFO は未対応）。ボイスのフィルターとは別に掛かります。
- 未対応（警告）: `fileg_*`、`pitcheg_*`、LFO 系、`trigger=first/legato`、CC 系 `on_locc` / `locc` 等、`ampeg_vel2*`、
  `*` ジェネレーターサンプル、`<curve> <effect> <midi>` ヘッダ、OGG（FLAC は対応済み）

## Phase 2 以降

| 項目 | 状態 | 備考 |
|---|---|---|
| ウェーブテーブル | ✅（v0.3） | レイヤー種別 WT。内蔵 8 テーブル（各 64 フレーム）＋ユーザー WAV（Serum 形式 2048 サンプル × 最大 256 フレーム、AKWF 等の単一波形も可）。FFT による帯域制限ミップマップ 11 段（折り返し -86 dB）、ポジション（Env2・モジュレーション行き先）、ワープ 4 種（sync / bend / mirror / squeeze）、ユニゾン最大 7（デチューン・ステレオ幅） |
| 複数音源のレイヤー | ✅（先行実装） | 3レイヤー + Morph |
| モジュレーションマトリクス | ✅（先行実装） | 8スロット、per-note ソース対応。エフェクトへは FX Mod（6スロット、全246パラメータ） |
| 高度なフィルター（ラダー、コムなど） | ✅ | Ladder（4 段 ZDF、自己発振）、Comb+ / Comb-（キー追従のカットオフ同調）、Formant（a/e/i/o/u モーフ、`filter.morph`） |
| エフェクトラック | ✅（v0.2） | 31ブロック × 12スロット任意順。ダイナミクス5、歪み/アンプ/キャビ/ローファイ6、EQ/フィルター4、モジュレーション7、ピッチ2、ディレイ/リバーブ/IRリバーブ、スライサー/スタッター/ステレオ/ボコーダー、ルーパー。[EFFECTS.md](EFFECTS.md) |
| ユーザー IR（キャビネット / 畳み込みリバーブ） | ✅（v0.2） | WAV 読込・リサンプル、遅延ゼロの分割畳み込み、プリセットに保存 |
| ボコーダー用音声入力 | 🟡 | Windows 版は対応（WASAPI デュプレックス）。Android 版は未対応（S23 移植時に追加） |
| MIDI クロック同期 | ✅ | F8/FA/FB/FC（MIDI 1.0 / UMP）。1 拍分のティック間隔からテンポを測定し、`fx.clocksync` ON でラックとパートインサートのテンポ・拍位置が追従。0.5 s 途切れると `fx.tempo` に戻る |
| パート単位エフェクト | ✅ | パートごとにインサート 3 スロット（12 種の軽量ブロック）。ボイス / パート / マスターの 3 階層（ARCHITECTURE.md） |
| ボイス単位エフェクト | ✅ | フィルタードライブ + Voice FX（saturate / fold / crush / ring、`vfx.amount` は per-note 変調可） |
| オーバーサンプリング | ✅（v0.2） | Drive / Amp の非線形部。DSP品質 Low=1x / Normal=2x / High=4x |
| ベロシティレイヤー・ループ改善 | ✅ | ループクロスフェード、リリーストリガー、ラウンドロビン、キースイッチ、排他グループ、リージョンフィルター |
| FLAC | ✅ | 自前デコーダー（依存なし）。4〜32 bit、固定 / LPC、全ステレオ相関モード。WAV / SFZ の `sample=` / ユーザーウェーブテーブルで使用可。ロード時に全展開（ストリーミングは未対応） |
| ディスクストリーミング | ⬜ | 現在は全サンプルをメモリにプリロード |
| マルチパート / マルチティンバー | 🟡（エンジンのみ） | 16 パート（MIDI チャンネル単位、レイヤー/スプリット可）。パートごとに音色パラメータ・サンプルセット 3 個、ミックス（volume / pan / mute / channel / キーレンジ / transpose / FX send、CC7・CC10）。エフェクトラック・ルーパー・マスターは共通、ボイスプール共有でパート間公平にスチール。マルチ JSON（`hsynth-multi`）。既定はシングルパート（従来と出力一致）。Windows / Android の UI・JNI は未対応（[ARCHITECTURE.md](ARCHITECTURE.md#マルチティンバー16-パート)） |
| RP2350 ショルダーキーボード | ⬜ | `EventSink` 翻訳器を追加する設計（ARCHITECTURE.md） |
| MIDI 2.0 Per-Note 制御 | 🟡 | エンジン側は対応・テスト済み |
| ネックセンサー | 🟡 | 割り当て可能 CC ソース cc1〜cc4（`ctl.cc1..4`）で受信可能。専用 Event はセンサー仕様確定後 |
| NEON SIMD 最適化 | ⬜ | |
| Raspberry Pi / Linux ビルド | 🟡 | engine はポータブル CMake。ALSA/JACK 出力層は未作成、実機ビルド未確認 |
| 低性能SBC向け品質設定 | 🟡 | Quality=Low（リバーブ4ライン）とポリフォニー制限のみ |
