# SysEx リモート制御（双方向）

HSynth は組み込み機器としての利用を前提に、**シンセサイザーの状態すべてを MIDI System Exclusive（SysEx）で取得・変更できます**。
音色パラメーター、パートのミックス、モード、プリセット、サンプル、IR、ソング再生、フレーズレコーダー、ルーパー、エンジン設定、MIDI ラーンまで、画面で操作できることはすべて SysEx からも操作できます。

- **双方向**: 要求（Request）に対して必ず応答（Reply）が返ります。購読（subscribe）すると、本体側の変更（画面操作・MIDI CC など）が通知（Notify）として自動で送られてきます。
- **どのプラットフォームでも同じ**: Android / iPhone / Windows・macOS デスクトップ版が同じプロトコルで応答します（実装はエンジン共通の `engine/src/remote.cpp`）。
- **PC から操作**: `desktop/tools/hsynth_remote.py` で、コマンドラインから取得・変更・保存・監視ができます。

---

## 1. つなぎ方

| 本体 | PC との接続 | 備考 |
|---|---|---|
| Android（Galaxy S23 など） | USB ケーブルで接続し、通知の「USB の設定」で **MIDI** を選ぶ | PC 側には「Android」などの名前の MIDI ポートが現れます |
| Android / iPhone（ケーブルなし） | **Bluetooth MIDI**: 本体をペリフェラルとして公開し、PC から `hsynth_remote.py --ble 名前` | [BLE_MIDI.md](BLE_MIDI.md) |
| iPhone / iPad | **ネットワーク MIDI**（同じ Wi-Fi）。Windows は rtpMIDI、Mac は「Audio MIDI 設定」→ ネットワーク | アプリ起動時にネットワークセッションを自動で有効にします（初回はローカルネットワークの許可を求められます）。USB-MIDI インターフェースや Bluetooth MIDI でも可 |
| Windows デスクトップ版 | 同じ PC で使うなら loopMIDI のポートを 1 つ作る | 同じポートで送受信して構いません（自分の送ったメッセージのこだまは無視します） |
| macOS デスクトップ版 / iOS 内の他アプリ | 仮想ポート **「HSynth」** に直接つなぐ | HSynth が仮想の入力・出力ポートを作ります |

応答は「最後に要求が来た機器」に返します（同じ機器の出力ポートを自動で探して開きます）。設定は不要です。
デスクトップ版は Settings の MIDI 欄に、受けた要求数と応答先ポートが表示されます。

---

## 2. メッセージの形式

```
F0 7D 48 53 <ver> <dev> <type> <seq> <idx_lo> <idx_hi> <total_lo> <total_hi> <JSON の一部…> F7
```

| バイト | 内容 |
|---|---|
| `7D` | メーカー ID（非商用・開発用 ID） |
| `48 53` | "HS"（HSynth） |
| `ver` | プロトコル版 = `01` |
| `dev` | デバイス ID 0〜126。`7F` = 全機器宛て（要求で使う）。応答・通知には本体の ID が入ります（既定 0） |
| `type` | `01` 要求 / `02` 応答 / `03` 通知 |
| `seq` | 要求ごとの番号 0〜127。応答は同じ番号で返ります（通知は本体側の連番） |
| `idx`, `total` | 分割番号と分割数（各 14 ビットを 7 ビット×2、下位が先） |
| 本体 | **JSON テキスト（ASCII）**。日本語などは `\uXXXX` でエスケープします |

- 1 メッセージの JSON は最大 **240 バイト**（メッセージ全体で最大 256 バイト）。長い JSON は分割し、受信側で `idx` 順につなげてから JSON として読みます。
- データバイトはすべて 7 ビット（0x00〜0x7F）です。
- 10 秒以内にそろわなかった分割メッセージは捨てます。
- 他のメーカー ID の SysEx は無視します。

### 例: `{"op":"hello"}` を全機器に送る

```
F0 7D 48 53 01 7F 01 01 00 00 01 00 7B 22 6F 70 22 3A 22 68 65 6C 6C 6F 22 7D F7
```

---

## 3. 要求と応答

要求は `{"op": "<操作名>", ...}` の JSON です。応答には必ず `"op"`（同じ操作名）と `"ok"`（true / false）が入り、失敗時は `"error"` に理由が入ります。

### 共通の約束

- **パラメーター名**はプリセットファイルと同じ名前（例 `filter.cutoff`、`a.fm.op1.level`、`fx.slot1.type`）。一覧は `param_info` で取れます（全 1446 個）。
- **値**: 数値はそのままの単位（Hz、秒、0〜1 など）。列挙型は**選択肢の文字列**（例 `"lp24"`）、ON/OFF は `true` / `false`。列挙型は番号でも指定できます。範囲外の値は丸められ、応答に実際の値が返ります。
- **パート**: `"part": 0〜15`。省略するとマルチモードでは選択中のパート、シングルモードでは 0。パートごとの値でないもの（エフェクトラック、マスター音量など）は全体の値です。
- **ファイル**: サンプル・IR はライブラリからの相対パス、MIDI ファイルは MIDI フォルダーからの相対パス。`..` でフォルダーの外には出られません。

### 情報

| op | 要求 | 応答 |
|---|---|---|
| `hello` | — | 機器名、版、デバイス ID、パラメーター数、パート数、モード、使える op の一覧 |
| `param_info` | `offset`, `count`（既定 100）, `prefix`（例 `"filter."`） | `total` と `params[]`: `id, name, group, label, kind(float/int/enum/bool), min, max, def, unit, options, part`（パートごとの値か） |
| `mix_info` | — | パートミックス項目の一覧 |
| `status` | — | モード、選択パート、発音数、CPU 負荷、トランスポート、ソング（再生中・位置・長さ・曲名）、フレーズ、ルーパー、各パートのサンプル名・発音数・プログラム、IR 名 |
| `meters` | — | 16 パートのレベル・発音数・CC7/CC11/パン・ベンド・モジュレーション・鳴っている鍵盤、出力ピーク |

### パラメーター

| op | 要求 | 応答 |
|---|---|---|
| `get` | `part`, `names`（名前の配列、または `"fx."` のような接頭辞の文字列。省略で全部）, `changed`（true: 既定値と違うものだけ） | `values: {名前: 値}` |
| `set` | `part`, `values: {名前: 値}`, `relative`（true: 今の値に足す） | 実際に設定された値。知らない名前は `error` / `errors` に入り、残りは設定されます |

```json
{"op":"set","values":{"filter.cutoff":1200,"filter.type":"lp24","amp.release":0.8}}
{"op":"get","part":3,"names":["filter.cutoff","osc1.wave"]}
```

### 状態まるごと

| op | 要求 | 応答 |
|---|---|---|
| `get_state` | `compact`（既定 true: 既定値と違う値だけ） | `multi`, `selected`, `state`。`state` はシングルなら**プリセット**、マルチなら**マルチ**のファイル形式そのもの（サンプル・IR・ミックス込み） |
| `set_state` | `state`（`get_state` の `state`、またはプリセット / マルチのファイルの中身） | `warnings`（読めなかったサンプルなど） |

`get_state` → ファイルに保存 → `set_state` で、音を丸ごと PC にバックアップ・復元できます。省略された値は既定値として読み込まれます。

| op | 要求 | 応答 |
|---|---|---|
| `setup_get` | `name` | `setup`: セットアップファイル（`hsynth-setup`。状態 + MIDI Learn + 同時発音数などの設定、[CONTROLLERS.md](CONTROLLERS.md)） |
| `setup_set` | `setup`（アプリが書き出したセットアップファイルの中身） | `name`, `multi`, `warnings` |

### モード・パート・ミックス

| op | 要求 | 応答 |
|---|---|---|
| `mode` | `multi`（true/false）, `mpe`（true/false） | 現在のモードと選択パート |
| `select_part` | `part` | 選択パート |
| `mix_get` | `part`（省略で 16 パート全部） | `mix` または `parts[]` |
| `mix_set` | `part`, `values: {volume, pan, mute, channel, key.low, key.high, transpose, fx.send}` | 設定後のミックス |

### プリセット・サンプル・ファイル

| op | 要求 | 応答 |
|---|---|---|
| `presets` | `query`（検索語、`#タグ`）, `category`, `favorites`, `user`, `offset`, `count`（既定 100）, `rescan`, `categories` | `total`, `presets[]: {name, category, file, user, favorite, tags}` |
| `load_preset` | `name` または `file`、`part` | 読み込んだプリセット名、`warnings`。マルチモードでは指定パートに読み込みます |
| `save_preset` | `name`, `category`, `comment`, `tags`, `part` | 保存したファイルのパス（ユーザープリセットフォルダー） |
| `samples` | `part`, `layer`（`"a"`/`"b"`/`"c"`）, `file`（SFZ / WAV / FLAC。`""` で空に） | 読み込んだサンプル名 |
| `impulse` | `target`（`"cab"` / `"reverb"`）, `file`（`""` で外す） | IR 名 |
| `files` | `kind`（`samples` / `impulses` / `midi` / `presets`）, `dir`, `offset`, `count` | `dirs[]`, `files[]`, `total` |

### 演奏

| op | 要求 | 備考 |
|---|---|---|
| `note` | `key`, `vel`（0〜1）, `on`（true/false）, `channel`（1〜16、省略で選択パート） | |
| `event` | `type`: `bend`(-1〜1) / `mod` / `pressure` / `sustain` / `cc`（`cc` 番号）/ `program`（`program`）/ `all_notes_off` / `all_sound_off`、`value`, `channel` | |
| `panic` | — | 全チャンネルの音を止める |
| `transport` | `play`（true/false）, `tempo`（BPM） | |

### ソング（MIDI ファイル）・フレーズ・ルーパー

| op | 要求 | 備考 |
|---|---|---|
| `songs` | `dir`, `offset`, `count` | MIDI フォルダーの一覧 |
| `song_load` | `file` | GM 音色を選んで読み込み（本体のプレイヤーと同じ処理）。曲名・長さ・各チャンネルの音色を返す |
| `song` | `cmd`: `play` / `stop` / `rewind` / `seek`（`position` 秒） | |
| `phrase` | `cmd`: `record` / `play` / `stop` / `clear` / `undo` / `quantize`（`grid` 拍、`strength`）/ `export`（`file`, `repeats`。MIDI フォルダーに保存）、`bars` | フレーズレコーダー |
| `looper` | `cmd`: `tap` / `play` / `stop` / `clear` / `undo` | エフェクトラックのルーパー |

### 設定・MIDI ラーン

| op | 要求 | 備考 |
|---|---|---|
| `settings` | `max_voices`, `quality`（`low`/`normal`/`high`）, `render_threads` | 省略で現在値だけ返す |
| `learn` | `cmd`: `get` / `arm`（`param`, `part`）/ `cancel` / `remove`（`param`）/ `clear` / `set`（`mappings`: MIDI マップファイル形式） | 割り当て一覧と待機中のパラメーター |

### 通知の購読

| op | 要求 | 備考 |
|---|---|---|
| `subscribe` | `what`: `["params","status","meters"]` または `"all"`（省略で params）, `interval_ms`（10〜2000、既定 50） | |
| `unsubscribe` | `what` | |

購読中に送られてくる通知（`type = 03`）:

```json
{"event":"changed","part":2,"values":{"filter.cutoff":950},"mix":{"volume":0.8},"samples":{"a":"Piano"}}
{"event":"changed","part":-1,"values":{"master.volume":0.6}}      // パートに属さない値
{"event":"mode","multi":true,"selected":3}
{"event":"status", ...status と同じ内容...}                        // interval_ms×4 ごと
{"event":"meters", ...meters と同じ内容...}
```

- 変更はパラメーター・ミックス・サンプルを `interval_ms` ごとに比べて、変わったものだけを送ります。
- **自分が SysEx で変えた値は通知されません**（こだま防止）。画面・MIDI CC・ソングなど他の経路で変わった値だけが届きます。

### 機種固有の操作

`host.` で始まる op は各アプリが独自に追加できる枠です（現在は未使用）。

---

## 4. PC ツール `hsynth_remote.py`

Windows では追加のインストールなしで動きます（winmm を直接使用）。macOS / Linux では `pip install mido python-rtmidi` が必要です。

```bash
python desktop/tools/hsynth_remote.py ports                                  # MIDI ポート一覧
python desktop/tools/hsynth_remote.py -p "Galaxy" hello                      # ポート名は一部でよい
python desktop/tools/hsynth_remote.py -p "Galaxy" status
python desktop/tools/hsynth_remote.py -p "Galaxy" params filter.             # パラメーター一覧（範囲・選択肢つき）
python desktop/tools/hsynth_remote.py -p "Galaxy" get filter.cutoff filter.type
python desktop/tools/hsynth_remote.py -p "Galaxy" get --changed              # 既定値から変えた値だけ
python desktop/tools/hsynth_remote.py -p "Galaxy" set filter.cutoff=1200 filter.type=lp24
python desktop/tools/hsynth_remote.py -p "Galaxy" mode multi
python desktop/tools/hsynth_remote.py -p "Galaxy" mix 3 volume=0.7 pan=-0.3 channel=4
python desktop/tools/hsynth_remote.py -p "Galaxy" state-save mysound.json    # 状態を丸ごと PC へ
python desktop/tools/hsynth_remote.py -p "Galaxy" state-load mysound.json    # PC から丸ごと戻す
python desktop/tools/hsynth_remote.py -p "Galaxy" setup-save band.json       # セットアップ（状態 + MIDI Learn + 設定）
python desktop/tools/hsynth_remote.py -p "Galaxy" setup-load band.json
python desktop/tools/hsynth_remote.py -p "Galaxy" presets piano
python desktop/tools/hsynth_remote.py -p "Galaxy" load "Grand Piano" --part 2
python desktop/tools/hsynth_remote.py -p "Galaxy" save "My Lead" --category Lead
python desktop/tools/hsynth_remote.py -p "Galaxy" note 60 --vel 0.9 --dur 1
python desktop/tools/hsynth_remote.py -p "Galaxy" songs
python desktop/tools/hsynth_remote.py -p "Galaxy" song-load "billie_jean.mid"
python desktop/tools/hsynth_remote.py -p "Galaxy" song play
python desktop/tools/hsynth_remote.py -p "Galaxy" phrase record --bars 4
python desktop/tools/hsynth_remote.py -p "Galaxy" learn arm filter.cutoff
python desktop/tools/hsynth_remote.py -p "Galaxy" watch --what params status # 本体側の変更をリアルタイム表示
python desktop/tools/hsynth_remote.py -p "Galaxy" req '{"op":"settings","quality":"high"}'
python desktop/tools/hsynth_remote.py -p "Galaxy" shell                      # 対話モード（JSON もコマンドも可）
```

- `-v` を付けると送受信した SysEx のバイト列を表示します。
- `--in` / `--out` で入力と出力に別のポートを指定できます。`--device N` で特定の機器だけに送れます。
- MIDI 機器がなくても `--sim engine/build-desktop/hsynth_remote_sim.exe` で、音の出ないエンジン（シミュレーター）を相手に試せます。

---

## 5. 自作の機器・ソフトから使うには

1. JSON を ASCII で作る（非 ASCII 文字は `\uXXXX`）。
2. 240 バイトずつに分け、2 章のヘッダーを付けて `F0 … F7` で送る（`type = 01`、`seq` は要求ごとに変える）。
3. 受信した SysEx のうち `F0 7D 48 53 01` で始まり `type = 02` / `03` のものを、`(type, seq)` ごとに `idx` 順につなげて JSON として読む。
4. 応答は `seq` で要求と対応づける。通知（`03`）はいつでも届く。

参考実装: エンジン側 `engine/include/hsynth/remote.h`（`sysex::encode` / `parseFrame` / `Assembler`）、PC 側 `desktop/tools/hsynth_remote.py`（`encode` / `parse` / `Assembler`）。

---

## 6. 仕組み（開発者向け）

- `RemoteControl`（`engine/src/remote.cpp`）が要求を専用スレッドで処理します。オーディオスレッドは止めません。値の設定はエンジンのスレッドセーフな API を使います。
- 各ホストは SysEx を組み立てて `RemoteControl::onSysEx()` に渡し、出てきた応答を送り返すだけです。
  - Windows: winmm の SysEx バッファー（`MIM_LONGDATA`）と `midiOutLongMsg`（`desktop/src/midi_in_win32.cpp`, `midi_out_win32.cpp`）
  - macOS / iOS: CoreMIDI。MIDI 2.0 プロトコルのポートでは UMP の SysEx7 パケットから組み立て。仮想ポート「HSynth」と、iOS ではネットワークセッションも有効化（`desktop/src/midi_in_coremidi.cpp`, `remote_link.cpp`, `ios/HSynth/Bridge/HSynthEngine.mm`）
  - Android: JNI でバイト列（または UMP）から機器ごとに SysEx を取り出し、応答は Kotlin の送信スレッドが要求元の機器へ送る（UMP 機器には SysEx7 パケット）（`android/.../cpp/jni_bridge.cpp`, `RemoteLink.kt`, `MidiInput.kt`）
- リモートで値が変わると `changeCount()` が増え、各アプリは画面を読み直します。
- テスト: `engine/tests/test_remote.cpp`（分割・ASCII・取得/設定・状態の往復・通知・フォルダー外アクセスの禁止）。

### 制限・今後

- サンプルファイルそのもの（WAV / FLAC）を SysEx で転送する機能はありません。ファイルは USB / ファイル共有でコピーし、`samples` で読み込んでください。
- iOS のセットリスト、プレイヤーのプレイリストなどアプリの画面側だけにある設定は、現在は対象外です（`host.*` で追加予定の枠）。
- DIN MIDI（31.25 kbps）では 256 バイトのメッセージ 1 つに約 80 ms かかります。`get_state` など大きな応答は USB / ネットワーク接続での利用をおすすめします。
- iOS 版と macOS 版の CoreMIDI 部分は、Windows 上ではコンパイル確認ができていません（次の Mac / CI ビルドで確認）。
