# CLAUDE.md — HSynth（tera-synth）

ハイブリッドシンセサイザー。プラットフォームに依存しない C++17 のエンジン（`engine/`）、デスクトップ版
（`desktop/`: Windows と macOS）、Android アプリ（`android/`、Kotlin + Oboe）、iOS アプリ（`ios/`、SwiftUI）。
資料は `docs/` にあります（日本語）。

> **Mac で作業する場合:** 最初に `docs/HANDOFF_MAC.md` を読んでください（Windows のセッションからの引き継ぎ。
> 最初の作業は iOS アプリをユーザーの iPhone に入れること）。Apple 向けのビルド方法は `docs/APPLE.md`。

## 作業のルール
- ユーザーとは**日本語**で話す。資料も日本語で書く。結果を先に、簡潔に。
- コミットとプッシュは、ユーザーに頼まれたときだけ。コミットメッセージの末尾に
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` を付ける。
- Windows では、機能はデスクトップ版と Galaxy S23 で確かめる（APK のビルドはユーザーに頼まれたとき）。
  Mac では、macOS 版と iOS アプリで確かめる（`docs/HANDOFF_MAC.md`）。
- **サンプリングした音源をこのリポジトリにコミットしない。** `desktop/data/`（SonicCell / GM の録音、ユーザーライブラリ）と
  `testdata/`（SoundFont、Roland の音色定義、マニュアルから作った一覧）は git の対象外のテスト用素材。
  音源は非公開リポジトリ **tera-works/tera-synth-assets**（下記）にだけ置く。このリポジトリ、公開の場所、
  PR の説明、成果物に入れない。
- **MIDI ファイルとテスト用の音源をアプリ（APK / iOS）に同梱しない。** 配布しないものです。
- 実際にビルドして試していないものを「動く」と言わない。確かめていないことはそう伝える。

## ビルドとテスト
エンジン（CMake と C++17 コンパイラがあればどの OS でも可）:
```bash
cmake -S engine -B engine/build -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build -j
./engine/build/hsynth_tests            # 全テスト（約 1 分）。引数で名前を絞り込める
./engine/build/hsynth_bench 32 2       # レイヤー / エフェクト構成ごとの CPU 負荷
```
`engine_golden_render` は `engine/tests/golden/` と比べるテスト。意図した DSP の変更のときだけ作り直し
（`HSYNTH_UPDATE_GOLDEN=1`）、そのことをユーザーに伝える。

Windows 版（Windows のみ。WinLibs MinGW GCC、Ninja、`-static` でリンク）:
```bash
cmake -S desktop -B desktop/build -G Ninja && cmake --build desktop/build
desktop/build/hsynth_desktop.exe --autotest desktop/autotest_out/run   # 音を出し、report.txt とスクリーンショットを書く
```
macOS 版と iOS アプリは `docs/APPLE.md` を参照。
内蔵プリセットとデモ用サンプルは `engine/build/hsynth_assets android/app/src/main/assets` で作る。

## テスト用の音源（非公開の assets リポジトリ）
`github.com/tera-works/tera-synth-assets`（Git LFS）は Windows PC の `desktop/data` の写し:
`library/user/gm/`（GM テスト音源 15 種）、`library/user/soniccell/`（Roland SonicCell の録音。PC で録音が進むにつれて
増える。`catalog.csv` に各音色の説明）、`presets/user/`（「GM ...」「SC ...」のプリセット。サンプルのパスは `library/`
からの相対パス）、`soundfonts/MuseScore_General.sf2`。このリポジトリの隣に置いて使う:
```bash
git lfs install
git clone https://github.com/tera-works/tera-synth-assets ../tera-synth-assets   # 数百 MB、増え続ける
./engine/build/hsynth_render --preset "../tera-synth-assets/presets/user/GM Grand Piano.json"     --library ../tera-synth-assets/library --out /tmp/gm_piano.wav
```
更新するのは Windows PC だけ（`python desktop/tools/sync_assets.py --push`）。ほかの環境は読むだけでプッシュしない。
クローンできない（権限がない）ときは、回避せずにそう伝える。

## コードの地図
- `engine/include/hsynth/` — `engine.h`（スレッドの約束: `process()` はメモリ確保もロックもしない）、`params.h`
  （フラットなパラメーター配列とメタデータ。プリセットはパラメーターを名前で保存する）、`voice.h`、`dsp/`
  （オシレーター、FM、ウェーブテーブル、SVF、エンベロープ）、`fx/`（エフェクトラック、コンボルバー、ルーパー）。
- `engine/src/params.cpp`、`params_fx.inc` — 全パラメーターの名前・範囲・グループ（UI はここから自動生成）。
- `desktop/src/` — ImGui のアプリ、miniaudio による音声（WASAPI / CoreAudio）、MIDI（winmm / CoreMIDI）、
  `sampler/` は自動サンプラー `hsynth_sampler`（Windows のみ）。
- `android/app/src/main/java/com/hsynth/app/` — Android の UI（Kotlin）。iOS へ移植するときの手本。
- `ios/` — iOS アプリ（`project.yml` は XcodeGen の設定、`HSynth/Bridge/` は Swift から使うエンジンの窓口）。
- テキストファイルはエディターのツールか Python で編集する（UTF-8、LF）。Windows の PowerShell 5.1 の
  `Set-Content` は UTF-8 を壊す。
