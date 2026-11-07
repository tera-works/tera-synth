# iOS アプリの AltStore 配布

iOS アプリは AltStore Classic で配布しています。ソース（このリポジトリ）は非公開のままにして、
署名していない IPA と AltStore の Source だけを公開リポジトリ **tera-works/tera-synth-ios** に置きます。

- Source URL: `https://tera-works.github.io/tera-synth-ios/source.json`
- 仕組みは `C:\WorkSpace\iOS` の配布モジュール（`ios-distribution/`、`docs/IOS_DISTRIBUTION.md`）を使っています。
  ただし、モジュールの標準のやり方（同じリポジトリの Releases と Pages で公開）は非公開リポジトリでは使えません。
  そのため、ビルドだけをここの GitHub Actions で行い、公開は PC から `publish_public.py` で行います。

## 新しい版を出す

1. `ios/project.yml` の `CFBundleShortVersionString`（例 `0.3.1`）を上げます。
   `CFBundleVersion` も前回より大きい数にします（版ごとに必ず増やす）。
2. コミットしてプッシュし、同じ版のタグを付けてプッシュします。
   ```bash
   git tag v0.3.1
   git push origin v0.3.1
   ```
3. タグで Actions の「iOS distribution」が動き、署名なしの IPA（成果物 `ios-package`）を作ります（約 10 分）。
4. PC で公開します。IPA を確かめてから公開リポジトリに Release を作り、`source.json` を作り直します。
   ```bash
   python ios-distribution/scripts/publish_public.py v0.3.1 --notes "変更点"
   ```
   `--dry-run` を付けると、確かめるだけで公開しません。
   `--source-only` は Release を作らずに `source.json` だけを作り直します（Pages の復旧用）。

## 決まり

- 公開した Release は置き換えません。間違えたときは、版と `CFBundleVersion` を上げて出し直します。
- 公開リポジトリへのコミットは `tera-works` の noreply アドレスで行います（`publish_public.py` が指定）。
  個人のメールアドレスを公開リポジトリに入れないこと。
- IPA に入るのは内蔵プリセットとデモ用サンプル（`android/app/src/main/assets/presets`・`library/factory`）だけです。
  テスト用の音源（GM / SonicCell）と MIDI ファイルは絶対に入れません。`publish_public.py` も IPA の中を確かめます。
- macOS の実行時間は非公開リポジトリでは 10 倍で数えられるので、「iOS distribution」はタグと手動実行のときだけ動きます。

## iPhone 側

1. PC に AltServer を入れ、iPhone に AltStore Classic を入れます（[公式手順](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows)）。
2. AltStore の Sources に上の Source URL を追加し、HSynth をインストールします。
3. 無料の Apple ID の署名は 7 日で切れるので、期限内に AltStore で Refresh します（新しい版への更新とは別の操作）。
4. 無料の Apple ID では、同時に入れられるアプリは AltStore を含めて 3 つまでです。
