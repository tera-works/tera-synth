# hsynth_sampler — 自動マルチサンプラー（テスト用 PCM 音源の作成）

音源（SoundFont または MIDI で鳴らせるシンセ）を1音ずつ鳴らして録音し、HSynth で使える
マルチサンプル音源（WAV + SFZ + HSynth プリセット）を自動生成するツールです。

> **テスト用途専用**。生成物は `desktop/data/`（`.gitignore` 済み）に出力され、APK の assets には入りません。
> ソースの SoundFont は `testdata/soundfonts/`（`.gitignore` 済み）に置いています。

## 処理の流れ

1. 指定範囲のノート（既定: 3半音おき）× ベロシティレイヤーを順に発音し、ホールド時間 + リリース（無音になるまで）を録音
2. 前後の無音をトリム（-70 dB）、1 ms フェードイン / 20 ms フェードアウト
3. 左右が同一ならモノラル化（容量半減）
4. 持続音はループ点を自動検出:
   - 持続判定（ホールド後半のレベルが前半の 55% 以上）
   - ホールド区間 40–60% と 88% 付近の立ち上がりゼロクロス同士で波形を比較し、最も一致する組を選択
   - 一致が良い（周期的な音: オルガン・フルート・管）→ 40 ms 線形クロスフェード
   - 一致が悪い（アンサンブル・クワイア・パッド）→ 300 ms 等パワークロスフェードで継ぎ目を隠す
   - `loop_mode=loop_sustain`：鍵盤を離すとループを抜けて録音済みのリリースを再生
5. 楽器全体で共通ゲインに正規化（-1 dBFS）→ ベロシティレイヤー間の音量差を保持
6. 16bit WAV（smpl チャンクにループ点・ルートキー）、SFZ（キー範囲は隣接サンプルとの中点で分割、ベロシティも中点分割）、
   HSynth プリセット（Layer A = PCM）を書き出し

## 使い方

```bash
cd desktop
cmake --build build --target hsynth_sampler

# 同梱の GM テストセット（15 音色）を SoundFont からサンプリング
./build/hsynth_sampler.exe sf2 --sf2 ../testdata/soundfonts/MuseScore_General.sf2 --set gm
# 一部だけ
./build/hsynth_sampler.exe sf2 --sf2 ../testdata/soundfonts/MuseScore_General.sf2 --set gm --only grand_piano,strings

# 任意のプリセット（bank:program）
./build/hsynth_sampler.exe sf2 --sf2 FILE.sf2 --program 0:19 --name church_organ --title "Church Organ" \
    --low 36 --high 96 --step 3 --vels 100 --hold 4 --loop on

# 実機シンセ / 任意の MIDI 音源をサンプリング（MIDI 出力 → 録音）
./build/hsynth_sampler.exe list-midi
./build/hsynth_sampler.exe live --midi-out 0 --capture loopback --program 0:0 --name gs_piano --vels 60,110 --hold 4
```

| オプション | 内容 |
|---|---|
| `--out DIR` / `--presets DIR` / `--library DIR` | 出力先（既定: `desktop/data/library/user/gm`、`desktop/data/presets/user`、`desktop/data/library`） |
| `--low --high --step` | サンプリングするノート範囲と間隔 |
| `--vels 40,80,120` | ベロシティレイヤー |
| `--hold` / `--release` | ホールド秒数 / リリース最大秒数 |
| `--loop auto\|on\|off` | ループ検出 |
| `--one-shot` | ドラム等（ノートオフ無視） |
| `--mono` / `--sr 48000` | モノラル化 / サンプルレート |
| `--capture loopback\|input` | live: PC の再生音（WASAPI ループバック）か録音デバイスを録る |
| `--channel 1` | live: MIDI チャンネル（ドラム bank 128 は自動で ch10） |

live モードの注意: ループバックは PC で鳴っている音をすべて録るので、サンプリング中は他の音を止めてください。
録音のレイテンシーは無音トリムで吸収されます。

## GM テストセット（`--set gm`）

| 名前 | GM | 範囲 | ベロシティ | ループ |
|---|---|---|---|---|
| grand_piano | 0 Acoustic Grand | A0–C8 | 40/80/120 | なし（6 秒） |
| epiano | 4 E.Piano 1 | E1–G7 | 50/110 | なし |
| vibraphone | 11 Vibraphone | F3–F6 | 50/110 | なし |
| organ | 16 Drawbar Organ | C2–C7 | 100 | あり |
| nylon_guitar | 24 Nylon Guitar | E2–E6 | 50/110 | なし |
| finger_bass | 33 Finger Bass | E1–G4 | 50/110 | なし |
| harp | 46 Harp | C1–G7 | 50/110 | なし |
| strings | 48 String Ensemble | C2–C7 | 60/115 | あり |
| choir | 52 Choir Aahs | C3–C6 | 90 | あり |
| trumpet | 56 Trumpet | E3–C6 | 60/115 | あり |
| brass | 61 Brass Section | E2–C6 | 60/115 | あり |
| alto_sax | 65 Alto Sax | C#3–G#5 | 60/115 | あり |
| flute | 73 Flute | C4–C7 | 60/110 | あり |
| warm_pad | 89 Pad 2 (warm) | C2–C7 | 100 | あり |
| gm_drums | Bank 128 Standard Kit | 35–81 各キー | 60/115 | ワンショット |

プリセットは「GM ＜名前＞」として User プリセットに追加されます（Windows 版の Library / プリセット一覧から選択）。

## ソース音源

- `testdata/soundfonts/MuseScore_General.sf2`（約 206 MB、v0.2）— FluidR3 GM を元に S. Christian Collins 氏が改良した GM バンク。
  ライセンスは MIT（`MuseScore_General_License.md`）。取得元: https://ftp.osuosl.org/pub/musescore/soundfont/MuseScore_General/
- 他の SF2（GeneralUser GS、Arachno、SGM など）や、手持ちのハードウェアシンセ（live モード）にも同じ手順で使えます。
  商用音源・ハードウェアの音を録る場合は、それぞれのライセンスに従ってください。
- SoundFont のレンダリングには TinySoundFont（MIT、`third_party/tinysoundfont/tsf.h`）を使用。SF2 のコーラス/リバーブは
  適用されない（ドライ）ため、サンプリングに適しています。SF3（圧縮版）は非対応です。

## Android 実機でのテスト

生成した楽器フォルダー（`desktop/data/library/user/gm/<名前>/`）を端末へコピーし、アプリの Library →「Import SFZ folder…」で取り込みます。
プリセット JSON は Library →「Import…」。サンプルパスはライブラリ相対なので、取り込み後のフォルダー名が同じなら
そのまま読み込めます（異なる場合は PCM ページから SFZ を選び直してください）。

## Roland SonicCell の吸い出し（2026-11-02）

**テスト用途専用・配布禁止**（Roland の音源データ）。出力は `desktop/data/library/user/soniccell/`（`.gitignore` 済み）、
プリセットは「SC …」として User プリセットに入ります。音色リスト（`.ins`）は `testdata/soniccell/`（`.gitignore` 済み）。

### 接続

- **SonicCell 単体（USB）で録れます**。SonicCell は USB オーディオ（24bit、背面スイッチで 44.1/48/96 kHz）を内蔵し、
  音源の出力をそのままデジタルで PC に送ります（To Computer = MIX）。外部オーディオインターフェースは不要で、
  アナログ変換を経ない分 AG03 経由より有利です。MIDI も同じ USB ケーブルで送ります。
- **ドライバー**: Windows 8.1 用（sonic_w81d_v101）は INF が Windows 10 以降を意図的に除外しているため Windows 11 では
  入りません（「互換性のあるドライバーがありません」）。**Vista/7 64bit 用（sonic_v64d_v100）を、デバイスマネージャーの
  「ドライバーの更新 → コンピューターを参照」で `sonic_v64d_v100\Files` を指定**すると入ります（メモリ整合性オンのままで動作）。
  デバイス名: MIDI「Roland SonicCell」、録音「IN (Roland SonicCell)」。
- 実測（96 kHz）: 雑音 -95 dBFS RMS（白色、ハムなし。本体の内部雑音）、22 kHz 以上の成分 -71 dB 以下
  → 音源部は 44.1/48 kHz 相当のため、`--decimate 2` で 48 kHz に落として保存（情報の損失なし、容量半分）。
- 古いドライバーは長時間の連続録音で入力が止まることがあるため、各テイクの受信量を経過時間と照合し、
  欠けていれば録音デバイスを開き直してそのノートを録り直します。

### 何をどう録るか

`.ins`（Cakewalk 音色定義）から全 1,193 音色を読み込み、音色ごとに試奏して録り方を決めます（`probeInstrument`）:

- ベロシティ 32 と 127 の音色差（1/3 オクターブ帯域のスペクトル差）→ 1.5 dB 未満なら 1 層、4 dB 未満なら 2 層、それ以上 3 層。
  音量差から SFZ の `amp_veltrack` を設定
- 持続音（ループ作成、ホールド 3 s）か減衰音（-55 dB まで鳴らして止める、低音ほど長く）か、リリースの長さ
- リズムキットは `.ins` に名前のある鍵だけ（合計 2,840 鍵）、ワンショット
- リバーブ / コーラスのセンドは 0（HSynth 側で付ける）、CC7=127。クリップしたら 6 dB 下げて音色ごと録り直し

### 実行・停止・進み具合（`desktop/tools/`）

| スクリプト | 動作 |
|---|---|
| `soniccell_start.bat` | 開始 / 再開。録り終えた音色は飛ばすので、何度実行しても続きから |
| `soniccell_stop.bat` | 停止。今鳴っている 1 音を録り終えたら止まる（数秒）。Ctrl+C やウィンドウを閉じても同じ |
| `soniccell_status.bat` | 優先グループごとの進み具合、実測の 1 音色あたり時間、残り時間の見積もり |

途中で止めた音色は再開時に最初から録り直します（完成した音色だけが SFZ を書き出すため、壊れたデータは残りません）。
記録: `desktop/data/library/user/soniccell/catalog.csv`（1 音色 1 行）と `capture_log.txt`。

### 録る順番（重要な音から）

`soniccell_order.py` が `testdata/soniccell/order.txt` を作ります（取説のパッチ一覧から抜き出したカテゴリー
`categories.tsv` を使用）。各グループの中ではカテゴリーを順番に回すので、どこで止めても全カテゴリーが揃います。

| グループ | 内容 | 数 | 密度 |
|---|---|---|---|
| 1 | 主要キット（GM2 Standard、SonicCellKit、StandardKit 1〜3） | 5 | full |
| 2 | GM2 標準音色 | 128 | full |
| 3 | 中核カテゴリー（アコピ、エレピ、オルガン、ストリングス、ブラス、ベース、ギター、ボイス、パッド、キーボード）の先頭 3 音色 | 36 | full |
| 4 | 残りのリズムキット | 36 | full |
| 5 | 中核カテゴリーの残り | 411 | lite |
| 6 | シンセ / リード / マレット / ベル / 撥弦 / 管 / オーケストラ等 | 369 | lite |
| 7 | FX / ヒット / ビート / 民族楽器等 | 80 | lite |
| 8 | GM2 バリエーション音色 | 128 | lite |

full = 4 半音おき・最大 3 層（実測 約 6 分 / 音色）、lite = 6 半音おき・最大 2 層・持続音ホールド 2 秒（見積もり 約 2.5 分）。
全体の見積もりは約 62 時間（グループ 1〜4 で約 21 時間）。

試験録音（Rich Grand / Phat Strings / GM2 Piano 1 / SonicCellKit）: 1 音色 3.6〜6.5 分、Phat Strings の 66 ループは
すべて継ぎ目のクリックなし（継ぎ目の段差 / 波形内の p99.9 = 最大 0.66）。

```bash
# 個別コマンド
./build/hsynth_sampler.exe ins-list --ins ../testdata/soniccell/SonicCell_Patch.ins --ins ../testdata/soniccell/SonicCell_Rhythm.ins
./build/hsynth_sampler.exe probe --midi-out SonicCell --capture-device SonicCell --program 87:64:0
./build/hsynth_sampler.exe capture --midi-out SonicCell --capture-device SonicCell --seconds 3 --wav noise.wav
```
