# ビルド・インストール手順

開発・テストは Windows ネイティブ版（`desktop/`）で行い、最後に Android へビルドします。Windows 版の手順は [WINDOWS.md](WINDOWS.md) を参照してください。

## 必要な環境

| ツール | バージョン | 備考 |
|---|---|---|
| Android Studio | 2026.1 以降（同梱 JBR / JDK 21+） | |
| Android SDK Platform | 36 | |
| Android NDK | 29.0.14206865 (r29) | `app/build.gradle.kts` の `ndkVersion` |
| CMake (SDK) | 3.31.6 | `externalNativeBuild.cmake.version` |
| Gradle | 9.3.1（ラッパー同梱） | |
| デスクトップ用 C++ コンパイラ | GCC 9+ / Clang 10+ / MSVC 2019+ | engine 単体のテスト・ツール用 |

このPCでは SDK を `C:\Android\Sdk` に配置済みで、`android/local.properties` もそこを指しています。
別のPCでは Android Studio の SDK Manager で上記 NDK / CMake を入れるか、`local.properties` の `sdk.dir` を修正してください。

> **注意（このPC）**: ユーザー環境変数 `ANDROID_HOME` が存在しない `C:\Users\naoki\AppData\Local\Android\Sdk` を指しており、
> Gradle がそちらを優先して失敗します。コマンドラインでは `$env:ANDROID_HOME="C:\Android\Sdk"` を設定してから実行するか、
> 環境変数自体を `C:\Android\Sdk` に変更してください。Android Studio では File ▸ Project Structure ▸ SDK Location を `C:\Android\Sdk` に設定します。

## APK のビルド

Android Studio: `Synth/android` フォルダーを開き、Build ▸ Build APK(s)。

コマンドライン（Windows）:

```powershell
cd C:\WorkSpace\Project\Synth\android
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
$env:ANDROID_HOME = "C:\Android\Sdk"
.\gradlew.bat assembleRelease
```

出力:

- `android/app/build/outputs/apk/release/app-release.apk`（最適化ビルド、**debug 鍵で署名**、サイドロード用）
- `android/app/build/outputs/apk/debug/app-debug.apk`（`assembleDebug`。DSP は debug でも -O2 でビルド）

配布用には `app/build.gradle.kts` の `signingConfig` を自分のキーストアに差し替えてください。

## Galaxy S23 へのインストール

1. 端末で「開発者向けオプション」→「USB デバッグ」を有効化し、USB 接続して許可する。
2. インストール:

```powershell
C:\Android\Sdk\platform-tools\adb.exe install -r C:\WorkSpace\Project\Synth\android\app\build\outputs\apk\release\app-release.apk
```

（あるいは APK ファイルを端末にコピーしてファイルアプリから開き、「提供元不明のアプリ」を許可してインストール）

3. 起動すると初回にデモ音源・プリセットをアプリ内ストレージへ展開し、最後に使ったプリセット（初回は Init VA）を読み込みます。

ログ確認: `adb logcat -s HSynthAudio`

## DSP エンジン単体（デスクトップ）

```bash
cd Synth/engine
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/hsynth_tests                  # 単体テスト（約30秒）
./build/hsynth_bench 32 3             # 方式別の CPU 負荷
./build/hsynth_assets out             # デモ音源とプリセットを生成（assets の再生成）
./build/hsynth_render --preset ../android/app/src/main/assets/presets/08_hybrid_piano_fm.json \
     --library ../android/app/src/main/assets/library --out piano.wav --pattern chord
```

- `HSYNTH_LONG_TEST_SECONDS=3600 ./build/hsynth_tests long_run` で長時間ソークテスト（既定は120秒相当）。
- `HSYNTH_UPDATE_GOLDEN=1 ./build/hsynth_tests golden` で音質回帰用ゴールデンファイルを更新（意図的に音を変えた時のみ）。
- 最適化前後の比較: 変更前に `hsynth_render ... --out ref.wav` を作り、変更後に
  `hsynth_render ... --compare ref.wav --max-error-db -80` を実行（誤差/信号比が閾値を超えると終了コード 3）。

Raspberry Pi / Linux でも同じ手順で `engine/` をビルドできます（ALSA / JACK 出力層は Phase 4 で追加予定）。

## デモ素材の再生成

```bash
./engine/build/hsynth_assets android/app/src/main/assets
```
