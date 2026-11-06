# Mac への引き継ぎ（2026-11-06、Windows のセッションから）

この文書は、Mac で作業を続ける Claude 向けの引き継ぎです。ユーザーとは日本語で話してください。
最初にこの文書と `CLAUDE.md`、`docs/APPLE.md` を読んでください。

## 最優先の作業: iOS アプリを iPhone に入れる

ユーザーの希望は「とりあえず先に iOS に入れたい」です。次の順で進めてください。

### 1. 準備（Mac）
```bash
xcode-select -p || xcode-select --install     # Xcode 本体は App Store から（未導入なら案内する）
brew --version || echo "Homebrew が必要"        # https://brew.sh
brew install xcodegen cmake
git clone https://github.com/tera-works/tera-synth.git && cd tera-synth   # すでにあれば git pull
```

### 2. Xcode プロジェクトを作る
```bash
cd ios && xcodegen && open HSynth.xcodeproj
```
`HSynth.xcodeproj` は生成物なのでコミットしません（`.gitignore` 済み）。設定を変えるときは `ios/project.yml` を直して `xcodegen` をやり直します。

### 3. 署名（ここはユーザーの操作が必要）
- Xcode の HSynth ターゲット → Signing & Capabilities → Team にユーザーの Apple ID を選んでもらう（無料の Apple ID でも可。その場合アプリは 7 日で期限切れ）。
- バンドル ID `com.hsynth.ios` が使えないと言われたら、`ios/project.yml` の `PRODUCT_BUNDLE_IDENTIFIER` をユーザー固有のもの（例 `com.<名前>.hsynth`）に変えて `xcodegen` し直す。
- チーム ID を `project.yml` の `DEVELOPMENT_TEAM` に入れておくと次回から楽（ユーザーに確認してから）。

### 4. iPhone 側（ユーザーの操作）
- USB で Mac につなぎ、「このコンピュータを信頼」を許可。
- 設定 → プライバシーとセキュリティ → デベロッパモード をオン（再起動あり）。
- 初回起動時に「信頼されていない開発元」と出たら、設定 → 一般 → VPN とデバイス管理 で開発元を信頼。

### 5. ビルドして入れる
- **Release で実行すること。** Debug だとエンジンが最適化されず、音が途切れます（Product → Scheme → Edit Scheme → Run → Build Configuration = Release）。
- Xcode でデバイスに iPhone を選んで Run。コマンドラインなら:
```bash
xcodebuild -project HSynth.xcodeproj -scheme HSynth -configuration Release \
  -destination 'generic/platform=iOS' -allowProvisioningUpdates DEVELOPMENT_TEAM=<チームID> build
xcrun devicectl list devices
xcrun devicectl device install app --device <ID> build/Release-iphoneos/HSynth.app   # パスは実際の出力先に合わせる
```

### 6. 実機で確認すること
- 起動して音が出るか（Play 画面の鍵盤）、プリセット切り替え（◀ ▶、Library）、ノブ、ピッチ / モッドホイール。
- 音切れ・遅延（`HSynthEngine.mm` で 5 ms バッファを要求。足りなければ 10 ms に）。
- 電話着信や他アプリの音で中断したあとに復帰するか（未対応の可能性大: `AVAudioSessionInterruptionNotification` を見て `startAudio` し直す処理を足す）。
- USB / Bluetooth MIDI キーボード（CoreMIDI。今は起動時に全ソースを開くだけで、抜き差しの自動検出はない）。
- CI（GitHub Actions、`.github/workflows/apple.yml`）ではシミュレーター向けビルドは通っているが、**実機でもシミュレーターでも一度も動かしていない**。

## 手持ちの音源と MIDI ファイルを iPhone で使う

アプリの「書類」フォルダ（`Documents`）は、ファイル App と Finder（Mac に iPhone をつなぐ → Finder のサイドバーで iPhone →「ファイル」タブ → HSynth）から見えます。
音源と MIDI ファイルはここへコピーして使います。**アプリ本体（バンドル）には入れません**（配布しないものなので）。

| 置き場所（HSynth の書類の中） | 入れるもの |
|---|---|
| `library/user/` | サンプル。assets リポジトリの `library/user` の中身（`gm/`、`soniccell/`）をそのまま。プリセットが `user/gm/...` のように参照する |
| `presets/user/` | assets リポジトリの `presets/user/*.json`（「GM ...」「SC ...」のプリセット） |
| `midi/` | MIDI ファイル（.mid）。assets リポジトリの `midi/` の中身。Player の「Open Songs」からも取り込める |

```bash
git lfs install
git clone https://github.com/tera-works/tera-synth-assets ../tera-synth-assets   # 非公開。GM 285 MB、SonicCell は数 GB で増え続ける
```
Finder の HSynth のファイル欄に、`../tera-synth-assets/library/user` を `library` の中へ、`presets/user` を `presets` の中へドラッグする。
`library/factory` はアプリ内蔵の音源へのリンクなので、消したり上書きしたりしない（起動のたびに作り直される）。

MIDI ファイル（43 曲、4 MB）は非公開の assets リポジトリの `midi/` にあります（2026-11-06 にユーザーの指示で追加）。
`../tera-synth-assets/midi/*.mid` を HSynth の `midi` フォルダへドラッグすれば Player に出ます。
公開リポジトリやアプリ本体には絶対に入れないこと。

Player（iOS）: 曲を選ぶと、そのチャンネルの GM 音色を全プリセット（内蔵 + `presets/user`）から選んで 16 パートに読み込む
（Android と同じ `gm_bank.h`）。SonicCell / GM のプリセットがあればそれが優先される。

## いまの状態（2026-11-06 時点）

| 対象 | 状態 |
|---|---|
| エンジン（`engine/`） | C++17。Windows で 170 テスト合格、macOS の CI でもテスト合格。 |
| Android（`android/`） | Galaxy S23 で動作確認済み。いちばん機能が揃っている（下記）。 |
| Windows デスクトップ（`desktop/`） | ImGui の開発用ホスト。動作確認済み。 |
| macOS デスクトップ | `desktop/` の CMake で HSynth.app が作れる（CI でビルド成功）。**実機未確認**。 |
| iOS（`ios/`） | SwiftUI の初版（Play / Edit / Player / Library）。iPhone は横画面固定（iPad は回転可）。CI でシミュレーター向けビルド成功。**未実行**。 |

Android アプリにあって iOS にまだ無いもの（移植の参考は `android/app/src/main/java/com/hsynth/app/`）:
- 画面: Multi（16 パート・スプリット / レイヤー・シーン）、System（設定・メッセージ）。Player は iOS にも入った（`PlayerScreen.swift`。チャンネル表の音量ドラッグ、パン等のメーターは未対応）。
- エディット: グラフ（`Graphs.kt`: エンベロープ、フィルター特性、LFO、FM アルゴリズム図、ステップシーケンサーのマス目、MSEG、モジュレーションマトリクス表）、FM のオペレーター別ページ、エフェクトチェーンの編集画面。
- MIDI ラーン / MIDI 出力、アンドゥ / リドゥ / コンペア（`History.kt`）、録音（WAV）、オーディオ入力（ボコーダー / ルーパー）、サンプル（WAV / SFZ）の読み込み、プリセットのお気に入り。
- 見た目: 実機シンセ風（LED リング付きノブ、LED + スライドスイッチ、液晶、Rajdhani / Share Tech Mono フォント）。iOS は `Controls.swift` に同じ配色の簡易版がある。

その次の候補: AUv3（GarageBand / AUM で使える Audio Unit 拡張）、macOS 版の実機確認。

## ユーザーとの約束・好み
- 日本語で、結果を先に簡潔に。確認の質問は少なめに、動くものを優先。
- コミットやプッシュは、ユーザーに頼まれたときだけ。コミットメッセージの末尾に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`。
- **テスト用の音源（GM テスト音源、Roland SonicCell の録音）と MIDI ファイルは配布しない。** このリポジトリ・アプリ（APK / iOS バンドル）・公開の場所に入れない。音源は非公開リポジトリ `tera-works/tera-synth-assets` にだけある。MIDI ファイルはユーザーの手元（Windows の `C:\WorkSpace\MIDI\原本`）にだけある。iOS アプリに同梱してよいのは `android/app/src/main/assets/presets` と `library/factory` だけ（今の `project.yml` のとおり）。
- UI の言葉: パネル名・ボタン・パラメーター名など、シンセで普通に英語のものは英語のまま。日本語にするのは説明文・ダイアログ・メッセージだけ（Android の `L10n.kt` の方針）。
- 見た目は「柔らかすぎない、シンセっぽい」もの。ON / OFF はスイッチと LED。
- 音を出すテストは OK（ユーザーが忙しいと言ったときは無音で）。

## Windows の PC にだけあるもの
- SonicCell の自動録音が進行中（約 50 / 1193、`desktop/tools/soniccell_*.bat`）。結果は Windows 側から assets リポジトリへ同期する。Mac からは触らない。
- `desktop/data/`（ユーザーライブラリ）、`C:\WorkSpace\MIDI\原本`（MIDI ファイル）。

## 主なファイル
- エンジン: `engine/include/hsynth/engine.h`（スレッドの約束: `process()` は確保もロックもしない）、`params.h` / `src/params.cpp`（全パラメーターの名前・範囲・グループ。UI はここから自動生成）。
- iOS: `ios/project.yml`、`ios/HSynth/Bridge/HSynthEngine.{h,mm}`（Swift から使うエンジン API）、`ios/HSynth/*.swift`。
- macOS: `desktop/src/main_macos.mm`、`platform_macos.mm`、`midi_in_coremidi.cpp`、`midi_out_coremidi.cpp`、`desktop/CMakeLists.txt`。
- Android のリアルタイム対策（参考）: オーディオスレッドを速いコアに固定、ADPF 目標は周期の半分、24 ボイス未満は並列描画しない（`docs/ARCHITECTURE.md`）。
