# 依存関係とライセンス

| コンポーネント | 用途 | ライセンス | 取得元 |
|---|---|---|---|
| Oboe 1.10.0 | Android 低遅延オーディオ出力（AAudio / OpenSL ES） | Apache License 2.0 | Maven `com.google.oboe:oboe`（prefab） |
| Dear ImGui 1.91.8 | Windows/デスクトップ版 UI（`third_party/imgui-1.91.8`） | MIT | github.com/ocornut/imgui |
| miniaudio 0.11.22 | デスクトップ版オーディオ出力（WASAPI / ALSA / CoreAudio） | Public domain または MIT-0 | github.com/mackron/miniaudio |
| TinySoundFont 0.9 | サンプラー（`hsynth_sampler`）の SF2 レンダリング | MIT | github.com/schellingb/TinySoundFont |
| Kotlin 標準ライブラリ | Android UI | Apache License 2.0 | Android Gradle Plugin 内蔵 Kotlin |
| LLVM libc++ (`libc++_shared.so`) | NDK C++ 標準ライブラリ | Apache 2.0 with LLVM Exception | Android NDK r29 |
| Android SDK / NDK | ビルド環境 | Android SDK License | Google |

DSPエンジン（`engine/`）は外部ライブラリに依存しません。ImGui と miniaudio はデスクトップホスト（`desktop/`）のみが使用します。JSON / WAV / SFZ パーサーも自作です。
AndroidX / Jetpack ライブラリは使用していません（フレームワーク API のみ）。

## デモ音源

`android/app/src/main/assets/library/factory/` のオーディオはすべて `engine/tools/hsynth_assets.cpp` で数式から生成した自作波形です
（加算合成ピアノ風音色、ループパッド、合成ドラム、テスト波形）。第三者の録音・商用音源は含みません。CC0 として扱えます。

## テスト用サンプル（非配布）

`desktop/data/library/user/gm/` の GM 音色は `hsynth_sampler` で MuseScore_General.sf2（MIT）からサンプリングしたテスト用素材です。
リポジトリ・APK には含めません（`.gitignore` 済み）。詳細は [SAMPLER.md](SAMPLER.md)。
