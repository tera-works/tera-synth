# 引き継ぎ: 曲のバンクセレクトと「浜松市歌」HSynth 版（2026-11-10、Windows のセッションから）

開発を続ける AI 向けの資料です。ユーザーとは日本語で話してください。最初にこの文書と `CLAUDE.md` を読んでください。

## 1. 背景（ユーザーの要望）
- 「浜松市歌」の EDM 版とロック版を、HSynth で鳴らしたときに**もっと重厚に、ハードに、聴いていて高揚する（ドーパミンが出る）**ようにしたい。
- そのために、HSynth の表現力（SonicCell の録音、標準コントローラー）を生かす。
- 使ってよい音: `C:\WorkSpace\HSynth_backup`（= `desktop/data` の SonicCell / GM の録音とプリセット 1208 個）。テスト用の素材なので配布しない。

## 2. やったこと

### 2.1 エンジン: 曲のバンクセレクト（コミット前）
曲（SMF）の CC0 / CC32 → プログラムチェンジで、SonicCell の録音を本体と同じ番号で選べるようにした。
それまでは CC0 を読んでも使っておらず、GM2 Capital（GM の 128 音）しか選べなかった。

| バンク | MSB / LSB | プリセット名 |
|---|---|---|
| GM2 Capital | 121 / 0 | `SC GM2 Capital NNN ...` |
| GM2 Var #k | 121 / k | `SC GM2 Var #k NNN ...` |
| PRESET A〜G | 87 / 64〜70 | `SC PRESET A NNN ...` 〜 `SC PRESET G NNN ...` |
| PRESET Rhythm | 86 / 64 | `SC PRESET Rhythm NNN ...` |
| GM2 Rhythm Sets | 120 / 0 | `SC GM2 Rhythm Sets NNN ...` |

番号は `desktop/data/library/user/soniccell/catalog.csv` の msb / lsb 列と一致している。

しくみ:
- **パッチキー**（`engine/include/hsynth/smf.h`）: バンクなし = プログラム 0..127。バンクあり = `program + 128 * (1 + MSB * 128 + LSB)`。
  - `patchKey()` / `patchProgram()` / `patchMsb()` / `patchLsb()` / `patchHasBank()` で扱う。
  - `Song::initialProgram`、`Song::programChanges[].program`、`Engine::partProgram()` / `songProgram()` はこのキーを持つ。
- **曲の中のイベント**: ProgramChange の `controller` は従来どおりプログラム番号（0..127）、`value` にキーを入れる（float で正確に表せる範囲）。
  - `engine.cpp` の `songPatch()` が読む。ライブの MIDI 入力は従来どおり（value = 0）。
- **`GmBank`**（`gm_bank.h` / `gm_bank.cpp`）:
  - 名前から `Entry::msb` / `lsb` / `bankProgram` を付ける（`soniccellBank()`）。
  - PRESET A〜G と GM2 Var は `bankOnly`（バンクセレクトのときだけ使い、バンクなしの GM 選択には出さない）。
  - `pick(key)`: まず同じバンク・番号の録音を探し、無ければバンクなしの番号として選ぶ（従来の選び方）。
  - ドラムキット（PRESET Rhythm）は従来の GM 選択にも残したまま、バンクでも選べる。
- **表示**: `prepareSong()` のレポートと `SongPart::gmName` は、GM 番号のバンク（なし / 0 / 120 / 121）だけ GM 名を出す。それ以外は `Bank 87/67 #18` と出す。
- **`remote.cpp`** の `program` はバンクを除いた番号を返す。
- **テスト**: `song_bank_select_soniccell_banks`（`engine/tests/test_song.cpp`）を追加。全 197 テスト通過（golden も変化なし）。
- **資料**: `docs/FEATURES.md` の「MIDI ファイル再生」に追記した。

変更ファイル（すべて未コミット）:
- `engine/include/hsynth/smf.h`、`engine/include/hsynth/gm_bank.h`
- `engine/src/smf.cpp`、`engine/src/gm_bank.cpp`、`engine/src/engine.cpp`、`engine/src/remote.cpp`
- `engine/tests/test_song.cpp`
- `docs/FEATURES.md`
- `desktop/tools/arrange/`（新規。下記）

### 2.2 編曲スクリプト（Node、パッケージ不要）
`desktop/tools/arrange/`:

| ファイル | 内容 |
|---|---|
| `smf_io.js` | SMF の読み書き、SonicCell のバンク表（`BANK`） |
| `hamamatsu_edm_hsynth.js` | EDM 版を作る。出力は `C:/WorkSpace/MIDI/原本/浜松市歌_EDM_HSynth.mid` |
| `hamamatsu_rock_hsynth.js` | ロック版を作る。入力は `浜松市歌_Rock.mid`、出力は `浜松市歌_Rock_HSynth.mid` |

- **EDM 版**
  - 元は `C:\WorkSpace\Arduino\midi_keyboard_kit_synth\tools\arrange\hamamatsu_edm.js`（混声版から 4/4 の EDM に組み直すもの）。
  - 130 BPM。音色は Super Saw / Epic Lead / Seq Saw / Nu Epic Pad / Power Synth / Storm Bass / Angels Choir / House Kit / Mix Hit 2。
  - 足したパート: サブベース（Lo Bass、Drop だけ）、うなりのベース（Metalic Bass、Drop 2）、ブラスの一撃（Stacked Brs）。
  - 盛り上げ: Drop 直前の 1 拍はリード・合唱・効果音以外を止める。Drop 2 の 2 回目のくり返しから半音上へ転調。Build と Breakdown は CC11 で抑えてから上げる。
  - 表情: CC1（遅れて掛かるビブラート）、CC5（Drop のポルタメント）、CC94（フレーズ末のディレイ）、CC91（空白の残響）。
- **ロック版**（元の編曲の音符はそのまま、後から加工）
  - 音色: Rich Grand / SuperDistLd2 / SC PowerChd / Low Bass / LP Dist / Orch & Horns / Strat Gtr / Angels Choir / Timpani / Rock Kit 2。
  - 足したパート: リズムギターの複製を右に（SC Loud Gtr、少し遅らせる、CC11 もなぞる）、サビにハードロックオルガン（リズムギターの和音を半小節ごとに伸ばす）、大事な小節の頭にオーケストラヒット。
  - 盛り上げ: 70 小節目（元からテンポが 126 に上がる所）から最後まで半音上へ。
  - 表情: CC74（Aメロは暗め、サビで開く、Build の小節で開いていく）、ギターの CC1 / CC94、サビのキック・スネアを強く。
  - 段の位置（小節番号）はスクリプト冒頭の `CHORUS` / `VERSE` / `BUILD` / `HITS` / `KEY_UP`。

作り直し:
```bash
cd desktop/tools/arrange
node hamamatsu_edm_hsynth.js
node hamamatsu_rock_hsynth.js
```

### 2.3 確認の方法と結果
レンダリング（オフラインで WAV に書き出す）:
```bash
./engine/build/hsynth_render --midi "C:/WorkSpace/MIDI/原本/浜松市歌_EDM_HSynth.mid" \
  --presets desktop/data/presets/user --library desktop/data/library --out edm.wav
```
レポートの `ch N: Bank 87/67 #18 -> SC PRESET D 018 Super Saw` で、全チャンネルが狙いの音になっていることを確認した。

区間ごとの音量（RMS）と低音の割合を元の版と比べた（ffmpeg で MP3 にしてユーザーに送付済み）:
- **EDM**: Drop の低音のエネルギーが元より約 +5 dB。Drop 直前の空白は −33 dB、Build → Drop 1 で +3.4 dB、Breakdown → Drop 2 で +3.7 dB。ピーク 0.77。
- **ロック**: 元はほぼ全編 −20.6 dB で平坦だった。新しい版は静かな所が下がり、最後のサビは直前の静かな部分より +3.6 dB。低音のエネルギーは元より +1.4 dB、ギターの壁で中域が厚い。ピーク 0.96。
- **音色の選び方**: 同じフレーズを候補ごとに書き出して比べた。最初の Kickin' Bass は元の Picked Bass より 5 dB 小さかったので、Low Bass と Rock Kit 2 に替えた。

**耳では確かめていない**（AI は聴けない）。ユーザーの感想で調整すること。特に次の 3 点。
- 転調のつなぎ（ピボットなしで半音上がる）
- オルガンの和音（パワーコードから作っているので 3 度がないことが多い）
- PRESET の音色どうしの音量バランス

## 3. 残っている作業（ユーザーに確認してから）
1. **コミット**: 上の変更を確認してコミット（ユーザーに頼まれたときだけ。メッセージ末尾に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。
   - この変更は Windows の PC にしかない。Mac で作業する場合は、先に Windows 側でコミット・プッシュしてもらう。
   - 生成した MIDI と書き出した音はコミットしない（`CLAUDE.md`）。
2. **iOS アプリの更新**: iPhone の 0.8.4 はバンクセレクト非対応。新しい MIDI を鳴らすと、別の音（例: Super Saw の番号が GM の Rock Organ）になる。
   - エンジンの変更を入れて新しい版を出す。手順は `docs/ALTSTORE.md`（公開リポジトリ tera-works/tera-synth-ios）。
   - 版を上げたら、iPhone で 2 曲の各チャンネルの音色名（Player の表示）を確認する。
3. **Windows 版・Android 版**: エンジンを入れ直してビルドすれば同じく対応する（Android の APK はユーザーに頼まれたときだけ）。
4. **テスト用音源の同期**: 新しい MIDI は `C:\WorkSpace\MIDI\原本` にある。iPhone に入れるには `python desktop/tools/sync_assets.py --push`（ユーザーに頼まれたときだけ。Windows PC からのみ）。
5. **ユーザーが聴いたあとの調整**:
   - 音色はスクリプトの `SOUND` 表 / `setup()` の行を差し替える。候補は `desktop/data/presets/user` の名前。番号は名前の 3 桁。
   - 音量は CC7（`setup()` の vol）と CC11。
   - 変えたら書き出して区間の音量を比べ、Drop / サビの「跳ね上がり」（+3 dB 前後）が残っているかを見る。

## 4. 守ること
- サンプリングした音源（SonicCell / GM）、ユーザーの MIDI ファイル、それらを書き出した音を、リポジトリ・公開の場所・アプリに入れない。
- バンクなしの MIDI ファイル（既存の 43 曲）の鳴り方を変えない。`song_gm_bank_selection` のテストがその確認。
- `GmBank` の名前の判定（`soniccellBank()`）は `hsynth_sampler` の命名（`SC <バンク一覧名> NNN <音色名>`）に依存している。サンプラーの命名を変えるときは合わせて直す。
