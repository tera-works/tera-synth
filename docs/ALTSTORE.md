# iOS アプリの AltStore 配布

iOS アプリは AltStore Classic で配布しています。ソース（このリポジトリ）は非公開のままにして、
署名していない IPA と AltStore の Source だけを公開リポジトリ **tera-works/tera-synth-ios** に置きます。

- Source URL: `https://tera-works.github.io/tera-synth-ios/source.json`
- 仕組みは `C:\WorkSpace\iOS` の配布モジュール（`ios-distribution/`、`docs/IOS_DISTRIBUTION.md`）を使っています。
  ただし、非公開リポジトリでは macOS の実行時間が 10 倍で数えられます。
  そのため、ビルドに必要なソースの写しを公開リポジトリの `src/` に置き、ビルドもそこの GitHub Actions で行います
  （公開リポジトリは無料）。公開は PC から `publish_public.py` で行います。

## 新しい版を出す

1. `ios/project.yml` の `CFBundleShortVersionString`（例 `0.8.5`）を上げます。
   `CFBundleVersion` も前回より大きい数にします（版ごとに必ず増やす）。
2. 非公開リポジトリでコミットし、同じ版のタグを付けてプッシュします。
   ```bash
   git tag v0.8.5
   git push origin main v0.8.5
   ```
3. ソースの写しを公開リポジトリに送ります。`src/` を作り直してコミットし、タグを付けてプッシュします。
   ```bash
   python ios-distribution/scripts/mirror_build.py v0.8.5
   ```
   公開リポジトリの「iOS distribution」が動き、署名なしの IPA（成果物 `ios-package`）を作ります（約 10 分）。
4. PC で公開します。IPA を確かめてから公開リポジトリに Release を作り、`source.json` を作り直します。
   ```bash
   python ios-distribution/scripts/publish_public.py v0.8.5 --notes "変更点"
   ```
   `--dry-run` を付けると、確かめるだけで公開しません。
   `--source-only` は Release を作らずに `source.json` だけを作り直します（Pages の復旧用）。
   GitHub が一時的なエラー（5xx）を返したときは、自動でやり直します。

## 決まり

- 公開した Release は置き換えません。間違えたときは、版と `CFBundleVersion` を上げて出し直します。
- 公開リポジトリへのコミットは `tera-works` の noreply アドレスで行います（`publish_public.py` が指定）。
  個人のメールアドレスを公開リポジトリに入れないこと。
- IPA に入るのは内蔵プリセットとデモ用サンプル（`android/app/src/main/assets/presets`・`library/factory`）だけです。
  テスト用の音源（GM / SonicCell）と MIDI ファイルは絶対に入れません。`publish_public.py` も IPA の中を確かめます。
- 非公開リポジトリの「iOS distribution」は手動実行のときだけ動きます（macOS の実行時間が 10 倍で数えられるため）。
- `mirror_build.py` が公開リポジトリに送るのは、ビルドに必要なソースと内蔵アセットだけです（スクリプトの `PATHS`）。

## iPhone 側

1. PC に AltServer を入れ、iPhone に AltStore Classic を入れます（[公式手順](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows)）。
2. AltStore の Sources に上の Source URL を追加し、HSynth をインストールします。
3. 無料の Apple ID の署名は 7 日で切れるので、期限内に AltStore で Refresh します（新しい版への更新とは別の操作）。
4. 無料の Apple ID では、同時に入れられるアプリは AltStore を含めて 3 つまでです。
